/*
   Copyright (c) 2000, 2025, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is designed to work with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have either included with
   the program or referenced in the documentation.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

// Dump a table's contents and format to an ASCII file.

#define DUMP_VERSION "10.13"

#include "my_config.h"

#include <fcntl.h>
#include <sys/types.h>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <forward_list>
#include <string>
#include <unordered_map>

#include "client/include/client_priv.h"
#include "client/multi_option.h"
#include "compression.h"
#include "m_string.h"
#include "map_helpers.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_default.h"
#include "my_hostname.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_macros.h"
#include "my_sys.h"
#include "my_systime.h"  // GETDATE_DATE_TIME
#include "my_user.h"
#include "mysql.h"
#include "mysql/service_mysql_alloc.h"
#include "mysql/strings/m_ctype.h"
#include "mysql_version.h"
#include "mysqld_error.h"
#include "nulls.h"
#include "prealloced_array.h"
#include "print_version.h"
#include "scope_guard.h"
#include "string_with_len.h"
#include "strmake.h"
#include "strxmov.h"
#include "template_utils.h"
#include "typelib.h"
#include "welcome_copyright_notice.h" /* ORACLE_WELCOME_COPYRIGHT_NOTICE */

/* Exit codes */

#define EX_USAGE 1
#define EX_MYSQLERR 2
#define EX_CONSCHECK 3
#define EX_EOM 4
#define EX_EOF 5 /* ferror for output file was got */
#define EX_ILLEGAL_TABLE 6

/* index into 'show fields from table' */

#define SHOW_FIELDNAME 0
#define SHOW_TYPE 1
#define SHOW_NULL 2
#define SHOW_DEFAULT 4
#define SHOW_EXTRA 5

/* Size of buffer for dump's select query */
#define QUERY_LENGTH 1536

/* Size of comment buffer. */
#define COMMENT_LENGTH 2048

/* ignore table flags */
#define IGNORE_NONE 0x00 /* no ignore */
#define IGNORE_DATA 0x01 /* don't dump data for this table */

#define MYSQL_UNIVERSAL_CLIENT_CHARSET "utf8mb4"

/* Maximum number of fields per table */
#define MAX_FIELDS 4000

/* One year in seconds */
#define LONG_TIMEOUT (3600UL * 24UL * 365UL)

/*  First mysql version supporting column statistics. */
#define FIRST_COLUMN_STATISTICS_VERSION 80002

/*  First mysql version supporting replica statements. */
#define FIRST_REPLICA_COMMAND_VERSION 80023
/*  First mysql version supporting source statements. */
#define FIRST_SOURCE_COMMAND_VERSION 80200

using std::string;

static void add_load_option(DYNAMIC_STRING *str, const char *option,
                            const char *option_value);
static char *alloc_query_str(size_t size);

static void field_escape(DYNAMIC_STRING *in, const char *from);
static bool verbose = false, opt_no_create_info = false, opt_no_data = false,
            quick = true, extended_insert = true, lock_tables = true,
            opt_force = false, flush_logs = false, flush_privileges = false,
            opt_drop = true, opt_keywords = false, opt_lock = true,
            opt_compress = false, create_options = true, opt_quoted = false,
            opt_databases = false, opt_alldbs = false, opt_create_db = false,
            opt_lock_all_tables = false, opt_set_charset = false,
            opt_dump_date = true, opt_autocommit = false,
            opt_disable_keys = true, opt_xml = false,
            opt_delete_source_logs = false, opt_single_transaction = false,
            opt_comments = false, opt_compact = false, opt_hex_blob = false,
            opt_order_by_primary = false, opt_ignore = false,
            opt_complete_insert = false, opt_drop_database = false,
            opt_replace_into = false, opt_dump_triggers = false,
            opt_routines = false, opt_tz_utc = true, opt_replica_apply = false,
            opt_include_source_host_port = false, opt_events = false,
            opt_comments_used = false, opt_alltspcs = false,
            opt_notspcs = false, opt_drop_trigger = false,
            opt_network_timeout = false, stats_tables_included = false,
            column_statistics = false,
            opt_show_create_table_skip_secondary_engine = false,
            opt_ignore_views = false;
static bool insert_pat_inited = false, debug_info_flag = false,
            debug_check_flag = false;
static ulong opt_max_allowed_packet, opt_net_buffer_length;
static MYSQL mysql_connection, *mysql = nullptr;
static DYNAMIC_STRING insert_pat;
static char *current_user = nullptr, *current_host = nullptr, *path = nullptr,
            *fields_terminated = nullptr, *lines_terminated = nullptr,
            *enclosed = nullptr, *opt_enclosed = nullptr, *escaped = nullptr,
            *where = nullptr, *opt_compatible_mode_str = nullptr,
            *opt_ignore_error = nullptr, *log_error_file = nullptr;
static Multi_option opt_init_commands;
static MEM_ROOT argv_alloc{PSI_NOT_INSTRUMENTED, 512};
static bool ansi_mode = false;  ///< Force the "ANSI" SQL_MODE.
/* Server supports character_set_results session variable? */
static bool server_supports_switching_charsets = true;
/**
  Use double quotes ("") like in the standard  to quote identifiers if true,
  otherwise backticks (``, non-standard MySQL feature).
*/
static bool ansi_quotes_mode = false;

static uint opt_zstd_compress_level = default_zstd_compression_level;
static char *opt_compress_algorithm = nullptr;

#define MYSQL_OPT_SOURCE_DATA_EFFECTIVE_SQL 1
#define MYSQL_OPT_SOURCE_DATA_COMMENTED_SQL 2
#define MYSQL_OPT_REPLICA_DATA_EFFECTIVE_SQL 1
#define MYSQL_OPT_REPLICA_DATA_COMMENTED_SQL 2
static uint opt_enable_cleartext_plugin = 0;
static bool using_opt_enable_cleartext_plugin = false;
static uint opt_mysql_port = 0, opt_source_data;
static uint opt_replica_data;
static ulong opt_long_query_time = 0;
static bool long_query_time_opt_provided = false;
static uint my_end_arg;
static char *opt_mysql_unix_port = nullptr;
static char *opt_bind_addr = nullptr;
static int first_error = 0;
static bool opt_dump_users = false;
static bool opt_add_drop_user = false;
#include "client/include/authentication_kerberos_clientopt-vars.h"
#include "client/include/caching_sha2_passwordopt-vars.h"
#include "client/include/multi_factor_passwordopt-vars.h"
#include "client/include/sslopt-vars.h"

FILE *md_result_file = nullptr;
FILE *stderror_file = nullptr;

const char *set_gtid_purged_mode_names[] = {"OFF", "AUTO", "ON", "COMMENTED",
                                            NullS};
static TYPELIB set_gtid_purged_mode_typelib = {
    array_elements(set_gtid_purged_mode_names) - 1, "",
    set_gtid_purged_mode_names, nullptr};
enum enum_set_gtid_purged_mode {
  SET_GTID_PURGED_OFF = 0,
  SET_GTID_PURGED_AUTO = 1,
  SET_GTID_PURGED_ON = 2,
  SET_GTID_PURGED_COMMENTED = 3
};
static enum enum_set_gtid_purged_mode opt_set_gtid_purged_mode =
    SET_GTID_PURGED_AUTO;

#if defined(_WIN32)
static char *shared_memory_base_name = nullptr;
#endif
static uint opt_protocol = 0;
static char *opt_plugin_dir = nullptr, *opt_default_auth = nullptr;
static bool opt_skip_gipk = false;

const char *set_output_as_version_mode[] = {"SERVER", "BEFORE_8_0_23",
                                            "BEFORE_8_2_0", NullS};
static TYPELIB set_output_as_version_mode_typelib = {
    array_elements(set_output_as_version_mode) - 1, "",
    set_output_as_version_mode, nullptr};

static ulong opt_server_version{0};

enum class Output_as_version_mode {
  SERVER = 0,         /// Output command terminology matching the dumped server
  BEFORE_8_0_23 = 1,  /// Output command terminology for servers below 8.0.23
  BEFORE_8_2_0 = 2    /// Output command terminology for servers below 8.2.0
};

enum Output_as_version_mode opt_output_as_version_mode =
    Output_as_version_mode::SERVER;

Prealloced_array<uint, 12> ignore_error(PSI_NOT_INSTRUMENTED);
static int parse_ignore_error();

/*
Dynamic_string wrapper functions. In this file use these
wrappers, they will terminate the process if there is
an allocation failure.
*/
static void init_dynamic_string_checked(DYNAMIC_STRING *str,
                                        const char *init_str,
                                        size_t init_alloc);
static void dynstr_append_checked(DYNAMIC_STRING *dest, const char *src);
static void dynstr_set_checked(DYNAMIC_STRING *str, const char *init_str);
static void dynstr_append_mem_checked(DYNAMIC_STRING *str, const char *append,
                                      size_t length);
static void dynstr_realloc_checked(DYNAMIC_STRING *str, size_t additional_size);
/*
  Constant for detection of default value of default_charset.
  If default_charset is equal to mysql_universal_client_charset, then
  it is the default value which assigned at the very beginning of main().
*/
static const char *mysql_universal_client_charset =
    MYSQL_UNIVERSAL_CLIENT_CHARSET;
static const char *default_charset;
static CHARSET_INFO *charset_info = &my_charset_latin1;
const char *default_dbug_option = "d:t:o,/tmp/mysqldump.trace";
/* have we seen any VIEWs during table scanning? */
bool seen_views = false;

collation_unordered_set<string> *ignore_table, *include_user;
std::forward_list<string> *exclude_user;

static struct my_option my_long_options[] = {
    {"all-databases", 'A',
     "Dump all the databases. This will be same as --databases with all "
     "databases selected.",
     &opt_alldbs, &opt_alldbs, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"all-tablespaces", 'Y', "Dump all the tablespaces.", &opt_alltspcs,
     &opt_alltspcs, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"no-tablespaces", 'y', "Do not dump any tablespace information.",
     &opt_notspcs, &opt_notspcs, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"add-drop-database", OPT_DROP_DATABASE,
     "Add a DROP DATABASE before each create.", &opt_drop_database,
     &opt_drop_database, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"add-drop-table", OPT_DROP, "Add a DROP TABLE before each create.",
     &opt_drop, &opt_drop, nullptr, GET_BOOL, NO_ARG, 1, 0, 0, nullptr, 0,
     nullptr},
    {"add-drop-trigger", 0, "Add a DROP TRIGGER before each create.",
     &opt_drop_trigger, &opt_drop_trigger, nullptr, GET_BOOL, NO_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
    {"add-drop-user", 0, "Add DROP USER when dumping the user definitions",
     &opt_add_drop_user, &opt_add_drop_user, nullptr, GET_BOOL, OPT_ARG, 0, 0,
     0, nullptr, 0, nullptr},
    {"add-locks", OPT_LOCKS, "Add locks around INSERT statements.", &opt_lock,
     &opt_lock, nullptr, GET_BOOL, NO_ARG, 1, 0, 0, nullptr, 0, nullptr},
    {"allow-keywords", OPT_KEYWORDS,
     "Allow creation of column names that are keywords.", &opt_keywords,
     &opt_keywords, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"apply-replica-statements", OPT_MYSQLDUMP_REPLICA_APPLY,
     "Adds 'STOP REPLICA' prior to 'CHANGE REPLICATION SOURCE' and 'START "
     "REPLICA' to bottom "
     "of dump. Use --output-as-version to use the old terminology.",
     &opt_replica_apply, &opt_replica_apply, nullptr, GET_BOOL, NO_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
    {"apply-slave-statements", OPT_MYSQLDUMP_SLAVE_APPLY_DEPRECATED,
     "This option is deprecated and will be removed in a future version. "
     "Use apply-replica-statements instead.",
     &opt_replica_apply, &opt_replica_apply, nullptr, GET_BOOL, NO_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
    {"bind-address", 0, "IP address to bind to.", (uchar **)&opt_bind_addr,
     (uchar **)&opt_bind_addr, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr,
     0, nullptr},
    {"character-sets-dir", OPT_CHARSETS_DIR,
     "Directory for character set files.", &charsets_dir, &charsets_dir,
     nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"column-statistics", 0,
     "Add an ANALYZE TABLE statement to regenerate any existing column "
     "statistics.",
     &column_statistics, &column_statistics, nullptr, GET_BOOL, NO_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
    {"comments", 'i', "Write additional information.", &opt_comments,
     &opt_comments, nullptr, GET_BOOL, NO_ARG, 1, 0, 0, nullptr, 0, nullptr},
    {"compatible", OPT_COMPATIBLE,
     "Change the dump to be compatible with a given mode. By default tables "
     "are dumped in a format optimized for MySQL. The only legal mode is ANSI."
     "Note: Requires MySQL server version 4.1.0 or higher. "
     "This option is ignored with earlier server versions.",
     &opt_compatible_mode_str, &opt_compatible_mode_str, nullptr, GET_STR,
     REQUIRED_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"compact", OPT_COMPACT,
     "Give less verbose output (useful for debugging). Disables structure "
     "comments and header/footer constructs.  Enables options --skip-add-"
     "drop-table --skip-add-locks --skip-comments --skip-disable-keys "
     "--skip-set-charset.",
     &opt_compact, &opt_compact, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"complete-insert", 'c', "Use complete insert statements.",
     &opt_complete_insert, &opt_complete_insert, nullptr, GET_BOOL, NO_ARG, 0,
     0, 0, nullptr, 0, nullptr},
    {"compress", 'C', "Use compression in server/client protocol.",
     &opt_compress, &opt_compress, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr,
     0, nullptr},
    {"create-options", 'a', "Include all MySQL specific create options.",
     &create_options, &create_options, nullptr, GET_BOOL, NO_ARG, 1, 0, 0,
     nullptr, 0, nullptr},
    {"databases", 'B',
     "Dump several databases. Note the difference in usage; in this case no "
     "tables are given. All name arguments are regarded as database names. "
     "'USE db_name;' will be included in the output.",
     &opt_databases, &opt_databases, nullptr, GET_BOOL, NO_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
#ifdef NDEBUG
    {"debug", '#', "This is a non-debug version. Catch this and exit.", nullptr,
     nullptr, nullptr, GET_DISABLED, OPT_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"debug-check", OPT_DEBUG_CHECK,
     "This is a non-debug version. Catch this and exit.", nullptr, nullptr,
     nullptr, GET_DISABLED, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"debug-info", OPT_DEBUG_INFO,
     "This is a non-debug version. Catch this and exit.", nullptr, nullptr,
     nullptr, GET_DISABLED, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
#else
    {"debug", '#', "Output debug log.", &default_dbug_option,
     &default_dbug_option, nullptr, GET_STR, OPT_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"debug-check", OPT_DEBUG_CHECK,
     "Check memory and open file usage at exit.", &debug_check_flag,
     &debug_check_flag, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"debug-info", OPT_DEBUG_INFO, "Print some debug info at exit.",
     &debug_info_flag, &debug_info_flag, nullptr, GET_BOOL, NO_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
#endif
    {"default-character-set", OPT_DEFAULT_CHARSET,
     "Set the default character set.", &default_charset, &default_charset,
     nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"delete-source-logs", OPT_DELETE_SOURCE_LOGS,
     "Rotate logs before the backup, equivalent to FLUSH LOGS, and purge "
     "all old binary logs after the backup, equivalent to PURGE LOGS. This "
     "automatically enables --source-data.",
     &opt_delete_source_logs, &opt_delete_source_logs, nullptr, GET_BOOL,
     NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"delete-master-logs", OPT_DELETE_MASTER_LOGS_DEPRECATED,
     "This option is deprecated and will be removed in a future version. "
     "Use delete-source-logs instead.",
     &opt_delete_source_logs, &opt_delete_source_logs, nullptr, GET_BOOL,
     NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"disable-keys", 'K',
     "'/*!40000 ALTER TABLE tb_name DISABLE KEYS */; and '/*!40000 ALTER "
     "TABLE tb_name ENABLE KEYS */; will be put in the output.",
     &opt_disable_keys, &opt_disable_keys, nullptr, GET_BOOL, NO_ARG, 1, 0, 0,
     nullptr, 0, nullptr},
    {"dump-replica", OPT_MYSQLDUMP_REPLICA_DATA,
     "This causes the binary log position and filename of the source to be "
     "appended to the dumped data output. Setting the value to 1, will print"
     "it as a CHANGE REPLICATION SOURCE command in the dumped data output; if "
     "equal to 2, that command will be prefixed with a comment symbol. "
     "This option will turn --lock-all-tables on, unless "
     "--single-transaction is specified too (in which case a "
     "global read lock is only taken a short time at the beginning of the dump "
     "- don't forget to read about --single-transaction below). In all cases "
     "any action on logs will happen at the exact moment of the dump."
     "Option automatically turns --lock-tables off.",
     &opt_replica_data, &opt_replica_data, nullptr, GET_UINT, OPT_ARG, 0, 0,
     MYSQL_OPT_REPLICA_DATA_COMMENTED_SQL, nullptr, 0, nullptr},
    {"dump-slave", OPT_MYSQLDUMP_SLAVE_DATA_DEPRECATED,
     "This option is deprecated and will be removed in a future version. "
     "Use dump-replica instead.",
     &opt_replica_data, &opt_replica_data, nullptr, GET_UINT, OPT_ARG, 0, 0,
     MYSQL_OPT_REPLICA_DATA_COMMENTED_SQL, nullptr, 0, nullptr},
    {"events", 'E', "Dump events.", &opt_events, &opt_events, nullptr, GET_BOOL,
     NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"extended-insert", 'e',
     "Use multiple-row INSERT syntax that include several VALUES lists.",
     &extended_insert, &extended_insert, nullptr, GET_BOOL, NO_ARG, 1, 0, 0,
     nullptr, 0, nullptr},
    {"fields-terminated-by", OPT_FTB,
     "Fields in the output file are terminated by the given string.",
     &fields_terminated, &fields_terminated, nullptr, GET_STR, REQUIRED_ARG, 0,
     0, 0, nullptr, 0, nullptr},
    {"fields-enclosed-by", OPT_ENC,
     "Fields in the output file are enclosed by the given character.",
     &enclosed, &enclosed, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"fields-optionally-enclosed-by", OPT_O_ENC,
     "Fields in the output file are optionally enclosed by the given "
     "character.",
     &opt_enclosed, &opt_enclosed, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
    {"fields-escaped-by", OPT_ESC,
     "Fields in the output file are escaped by the given character.", &escaped,
     &escaped, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"flush-logs", 'F',
     "Flush logs file in server before starting dump. "
     "Note that if you dump many databases at once (using the option "
     "--databases= or --all-databases), the logs will be flushed for "
     "each database dumped. The exception is when using --lock-all-tables "
     "or --source-data: "
     "in this case the logs will be flushed only once, corresponding "
     "to the moment all tables are locked. So if you want your dump and "
     "the log flush to happen at the same exact moment you should use "
     "--lock-all-tables or --source-data with --flush-logs.",
     &flush_logs, &flush_logs, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"flush-privileges", OPT_ESC,
     "Emit a FLUSH PRIVILEGES statement "
     "after dumping the mysql database.  This option should be used any "
     "time the dump contains the mysql database and any other database "
     "that depends on the data in the mysql database for proper restore. ",
     &flush_privileges, &flush_privileges, nullptr, GET_BOOL, NO_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
    {"force", 'f', "Continue even if we get an SQL error.", &opt_force,
     &opt_force, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"help", '?', "Display this help message and exit.", nullptr, nullptr,
     nullptr, GET_NO_ARG, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"hex-blob", OPT_HEXBLOB,
     "Dump binary strings (BINARY, "
     "VARBINARY, BLOB) in hexadecimal format.",
     &opt_hex_blob, &opt_hex_blob, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr,
     0, nullptr},
    {"host", 'h', "Connect to host.", &current_host, &current_host, nullptr,
     GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"ignore-error", OPT_MYSQLDUMP_IGNORE_ERROR,
     "A comma-separated list of "
     "error numbers to be ignored if encountered during dump.",
     &opt_ignore_error, &opt_ignore_error, nullptr, GET_STR_ALLOC, REQUIRED_ARG,
     0, 0, 0, nullptr, 0, nullptr},
    {"ignore-table", OPT_IGNORE_TABLE,
     "Do not dump the specified table. To specify more than one table to "
     "ignore, "
     "use the directive multiple times, once for each table.  Each table must "
     "be specified with both database and table names, e.g., "
     "--ignore-table=database.table.",
     nullptr, nullptr, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"exclude-user", OPT_MYSQLDUMP_EXCLUDE_USER,
     "Do not dump the specified user account. To specify more than one user "
     "account to exclude, use the directive multiple times, once for each user "
     "account.  Each user account must be specified with both user and host "
     "part, e.g., --exclude-user=foo@localhost.",
     nullptr, nullptr, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"include-user", OPT_MYSQLDUMP_INCLUDE_USER,
     "Dump the specified user account. If no --include-user is specified, dump "
     "all user accounts by default. To specify more than one user account to "
     "dump, use the directive multiple times, once for each user account.  "
     "Each user account must be specified with both user and host part, e.g., "
     "--exclude-users=foo@localhost.",
     nullptr, nullptr, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"include-source-host-port", OPT_MYSQLDUMP_INCLUDE_SOURCE_HOST_PORT,
     "Adds 'SOURCE_HOST=<host>, SOURCE_PORT=<port>' to 'CHANGE REPLICATION "
     "SOURCE TO..' "
     "in dump produced with --dump-replica.",
     &opt_include_source_host_port, &opt_include_source_host_port, nullptr,
     GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"include-master-host-port",
     OPT_MYSQLDUMP_INCLUDE_MASTER_HOST_PORT_DEPRECATED,
     "This option is deprecated and will be removed in a future version. "
     "Use include-source-host-port instead.",
     &opt_include_source_host_port, &opt_include_source_host_port, nullptr,
     GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"insert-ignore", OPT_INSERT_IGNORE, "Insert rows with INSERT IGNORE.",
     &opt_ignore, &opt_ignore, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"lines-terminated-by", OPT_LTB,
     "Lines in the output file are terminated by the given string.",
     &lines_terminated, &lines_terminated, nullptr, GET_STR, REQUIRED_ARG, 0, 0,
     0, nullptr, 0, nullptr},
    {"lock-all-tables", 'x',
     "Locks all tables across all databases. This "
     "is achieved by taking a global read lock for the duration of the whole "
     "dump. Automatically turns --single-transaction and --lock-tables off.",
     &opt_lock_all_tables, &opt_lock_all_tables, nullptr, GET_BOOL, NO_ARG, 0,
     0, 0, nullptr, 0, nullptr},
    {"lock-tables", 'l', "Lock all tables for read.", &lock_tables,
     &lock_tables, nullptr, GET_BOOL, NO_ARG, 1, 0, 0, nullptr, 0, nullptr},
    {"log-error", OPT_ERROR_LOG_FILE,
     "Append warnings and errors to given file.", &log_error_file,
     &log_error_file, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"mysqld-long-query-time", OPT_LONG_QUERY_TIME,
     "Set long_query_time for the session of this dump. Ommitting flag means "
     "using the server value.",
     &opt_long_query_time, &opt_long_query_time, nullptr, GET_ULONG,
     REQUIRED_ARG, 0, 0, LONG_TIMEOUT, nullptr, 0, nullptr},
    {"source-data", OPT_SOURCE_DATA,
     "This causes the binary log position and filename to be appended to the "
     "output. If equal to 1, will print it as a CHANGE REPLICATION SOURCE "
     "command; "
     "if equal to 2, that command will be prefixed with a comment symbol. "
     "This option will turn --lock-all-tables on, unless "
     "--single-transaction is specified too (in which case a "
     "global read lock is only taken a short time at the beginning of the "
     "dump; "
     "don't forget to read about --single-transaction below). In all cases, "
     "any action on logs will happen at the exact moment of the dump. "
     "Option automatically turns --lock-tables off.",
     &opt_source_data, &opt_source_data, nullptr, GET_UINT, OPT_ARG, 0, 0,
     MYSQL_OPT_SOURCE_DATA_COMMENTED_SQL, nullptr, 0, nullptr},
    {"master-data", OPT_MASTER_DATA_DEPRECATED,
     "This option is deprecated and will be removed in a future version. "
     "Use source-data instead.",
     &opt_source_data, &opt_source_data, nullptr, GET_UINT, OPT_ARG, 0, 0,
     MYSQL_OPT_SOURCE_DATA_COMMENTED_SQL, nullptr, 0, nullptr},
    {"max_allowed_packet", OPT_MAX_ALLOWED_PACKET,
     "The maximum packet length to send to or receive from server.",
     &opt_max_allowed_packet, &opt_max_allowed_packet, nullptr, GET_ULONG,
     REQUIRED_ARG, 24 * 1024 * 1024, 4096, (longlong)2L * 1024L * 1024L * 1024L,
     nullptr, 1024, nullptr},
    {"net_buffer_length", OPT_NET_BUFFER_LENGTH,
     "The buffer size for TCP/IP and socket communication.",
     &opt_net_buffer_length, &opt_net_buffer_length, nullptr, GET_ULONG,
     REQUIRED_ARG, 1024 * 1024L - 1025, 4096, 16 * 1024L * 1024L, nullptr, 1024,
     nullptr},
    {"no-autocommit", OPT_AUTOCOMMIT,
     "Wrap tables with autocommit/commit statements.", &opt_autocommit,
     &opt_autocommit, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"no-create-db", 'n',
     "Suppress the CREATE DATABASE ... IF EXISTS statement that normally is "
     "output for each dumped database if --all-databases or --databases is "
     "given.",
     &opt_create_db, &opt_create_db, nullptr, GET_BOOL, NO_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
    {"no-create-info", 't', "Don't write table creation info.",
     &opt_no_create_info, &opt_no_create_info, nullptr, GET_BOOL, NO_ARG, 0, 0,
     0, nullptr, 0, nullptr},
    {"no-data", 'd', "No row information.", &opt_no_data, &opt_no_data, nullptr,
     GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"no-set-names", 'N', "Same as --skip-set-charset.", nullptr, nullptr,
     nullptr, GET_NO_ARG, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"opt", OPT_OPTIMIZE,
     "Same as --add-drop-table, --add-locks, --create-options, --quick, "
     "--extended-insert, --lock-tables, --set-charset, and --disable-keys. "
     "Enabled by default, disable with --skip-opt.",
     nullptr, nullptr, nullptr, GET_NO_ARG, NO_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"order-by-primary", OPT_ORDER_BY_PRIMARY,
     "Sorts each table's rows by primary key, or first unique key, if such a "
     "key exists.  Useful when dumping a MyISAM table to be loaded into an "
     "InnoDB table, but will make the dump itself take considerably longer.",
     &opt_order_by_primary, &opt_order_by_primary, nullptr, GET_BOOL, NO_ARG, 0,
     0, 0, nullptr, 0, nullptr},
#include "client/include/multi_factor_passwordopt-longopts.h"
#ifdef _WIN32
    {"pipe", 'W', "Use named pipes to connect to server.", nullptr, nullptr,
     nullptr, GET_NO_ARG, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
#endif
    {"port", 'P', "Port number to use for connection.", &opt_mysql_port,
     &opt_mysql_port, nullptr, GET_UINT, REQUIRED_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"protocol", OPT_MYSQL_PROTOCOL,
     "The protocol to use for connection (tcp, socket, pipe, memory).", nullptr,
     nullptr, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"quick", 'q', "Don't buffer query, dump directly to stdout.", &quick,
     &quick, nullptr, GET_BOOL, NO_ARG, 1, 0, 0, nullptr, 0, nullptr},
    {"quote-names", 'Q', "Quote table and column names with backticks (`).",
     &opt_quoted, &opt_quoted, nullptr, GET_BOOL, NO_ARG, 1, 0, 0, nullptr, 0,
     nullptr},
    {"replace", OPT_MYSQL_REPLACE_INTO,
     "Use REPLACE INTO instead of INSERT INTO.", &opt_replace_into,
     &opt_replace_into, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"result-file", 'r',
     "Direct output to a given file. This option should be used in systems "
     "(e.g., DOS, Windows) that use carriage-return linefeed pairs (\\r\\n) "
     "to separate text lines. This option ensures that only a single newline "
     "is used.",
     nullptr, nullptr, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"routines", 'R', "Dump stored routines (functions and procedures).",
     &opt_routines, &opt_routines, nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr,
     0, nullptr},
    {"set-charset", OPT_SET_CHARSET,
     "Add 'SET NAMES default_character_set' to the output.", &opt_set_charset,
     &opt_set_charset, nullptr, GET_BOOL, NO_ARG, 1, 0, 0, nullptr, 0, nullptr},
    {"set-gtid-purged", OPT_SET_GTID_PURGED,
     "Add 'SET @@GLOBAL.GTID_PURGED' to the output. Possible values for "
     "this option are ON, COMMENTED, OFF and AUTO. If ON is used and GTIDs "
     "are not enabled on the server, an error is generated. If COMMENTED is "
     "used, 'SET @@GLOBAL.GTID_PURGED' is added as a comment. If OFF is "
     "used, this option does nothing. If AUTO is used and GTIDs are enabled "
     "on the server, 'SET @@GLOBAL.GTID_PURGED' is added to the output. "
     "If GTIDs are disabled, AUTO does nothing. If no value is supplied "
     "then the default (AUTO) value will be considered.",
     nullptr, nullptr, nullptr, GET_STR, OPT_ARG, 0, 0, 0, nullptr, 0, nullptr},
#if defined(_WIN32)
    {"shared-memory-base-name", OPT_SHARED_MEMORY_BASE_NAME,
     "Base name of shared memory.", &shared_memory_base_name,
     &shared_memory_base_name, nullptr, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
#endif
    /*
      Note that the combination --single-transaction --source-data
      will give bullet-proof binlog position only if server >=4.1.3. That's the
      old "FLUSH TABLES WITH READ LOCK does not block commit" fixed bug.
    */
    {"single-transaction", OPT_TRANSACTION,
     "Creates a consistent snapshot by dumping all tables in a single "
     "transaction. Works ONLY for tables stored in storage engines which "
     "support multiversioning (currently only InnoDB does); the dump is NOT "
     "guaranteed to be consistent for other storage engines. "
     "While a --single-transaction dump is in process, to ensure a valid "
     "dump file (correct table contents and binary log position), no other "
     "connection should use the following statements: ALTER TABLE, DROP "
     "TABLE, RENAME TABLE, TRUNCATE TABLE, as consistent snapshot is not "
     "isolated from them. Option automatically turns off --lock-tables.",
     &opt_single_transaction, &opt_single_transaction, nullptr, GET_BOOL,
     NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"dump-date", OPT_DUMP_DATE, "Put a dump date to the end of the output.",
     &opt_dump_date, &opt_dump_date, nullptr, GET_BOOL, NO_ARG, 1, 0, 0,
     nullptr, 0, nullptr},
    {"skip-opt", OPT_SKIP_OPTIMIZATION,
     "Disable --opt. Disables --add-drop-table, --add-locks, --create-options, "
     "--quick, --extended-insert, --lock-tables, --set-charset, and "
     "--disable-keys.",
     nullptr, nullptr, nullptr, GET_NO_ARG, NO_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"socket", 'S', "The socket file to use for connection.",
     &opt_mysql_unix_port, &opt_mysql_unix_port, nullptr, GET_STR, REQUIRED_ARG,
     0, 0, 0, nullptr, 0, nullptr},
#include "client/include/caching_sha2_passwordopt-longopts.h"
#include "client/include/sslopt-longopts.h"

    {"tab", 'T',
     "Create tab-separated textfile for each table to given path. (Create .sql "
     "and .txt files.) NOTE: This only works if mysqldump is run on the same "
     "machine as the mysqld server.",
     &path, &path, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"tables", OPT_TABLES, "Overrides option --databases (-B).", nullptr,
     nullptr, nullptr, GET_NO_ARG, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"output-as-version", OPT_OUTPUT_AS_VERSION,
     "Defines what is the terminology used in the dump for replica and event "
     "commands, allowing to create dumps that are compatible with lower "
     "versions that only accept deprecated commands. "
     "Possible values for this option are SERVER, BEFORE_8_0_23 and "
     "BEFORE_8_2_0. The default is SERVER, and if set, it reads the server "
     "version and outputs commands that are compatible for that version, "
     "meaning that if the server is below 8.2.0 it will output the deprecated "
     "DISABLE ON SLAVE terminology for events and if lower than 8.0.23 it "
     "will also use the deprecated SLAVE/CHANGE MASTER terminology for "
     "replica commands. "
     "If set to BEFORE_8_2_0 the command SHOW "
     "CREATE EVENT will always show how the event would have been created "
     "in a server of a version lower than 8.2.0 "
     "If set to BEFORE_8_0_23 the dump will also contain deprecated replica "
     "commands like START SLAVE or CHANGE MASTER TO. "
     "This affects the output of --events, --dump-replica, --source-data, "
     "--apply-replica-statements and --include-source-host-port",
     nullptr, nullptr, nullptr, GET_STR, OPT_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"triggers", OPT_TRIGGERS, "Dump triggers for each dumped table.",
     &opt_dump_triggers, &opt_dump_triggers, nullptr, GET_BOOL, NO_ARG, 1, 0, 0,
     nullptr, 0, nullptr},
    {"tz-utc", OPT_TZ_UTC,
     "SET TIME_ZONE='+00:00' at top of dump to allow dumping of TIMESTAMP data "
     "when a server has data in different time zones or data is being moved "
     "between servers with different time zones.",
     &opt_tz_utc, &opt_tz_utc, nullptr, GET_BOOL, NO_ARG, 1, 0, 0, nullptr, 0,
     nullptr},
    {"user", 'u', "User for login if not current user.", &current_user,
     &current_user, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"users", 0,
     "Dump user accounts as logical definitions in the form of CREATE USER and "
     "GRANT statements. Not compatible with --flush-privileges!",
     &opt_dump_users, &opt_dump_users, nullptr, GET_BOOL, OPT_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
    {"verbose", 'v', "Print info about the various stages.", &verbose, &verbose,
     nullptr, GET_BOOL, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"version", 'V', "Output version information and exit.", nullptr, nullptr,
     nullptr, GET_NO_ARG, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"where", 'w', "Dump only selected records. Quotes are mandatory.", &where,
     &where, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"xml", 'X', "Dump a database as well formed XML.", nullptr, nullptr,
     nullptr, GET_NO_ARG, NO_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"plugin_dir", OPT_PLUGIN_DIR, "Directory for client-side plugins.",
     &opt_plugin_dir, &opt_plugin_dir, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
    {"default_auth", OPT_DEFAULT_AUTH,
     "Default authentication client-side plugin to use.", &opt_default_auth,
     &opt_default_auth, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"enable_cleartext_plugin", OPT_ENABLE_CLEARTEXT_PLUGIN,
     "Enable/disable the clear text authentication plugin.",
     &opt_enable_cleartext_plugin, &opt_enable_cleartext_plugin, nullptr,
     GET_BOOL, OPT_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"network_timeout", 'M',
     "Allows huge tables to be dumped by setting max_allowed_packet to maximum "
     "value and net_read_timeout/net_write_timeout to large value.",
     &opt_network_timeout, &opt_network_timeout, nullptr, GET_BOOL, NO_ARG, 1,
     0, 0, nullptr, 0, nullptr},
    {"show_create_table_skip_secondary_engine", 0,
     "Controls whether SECONDARY_ENGINE CREATE TABLE clause should be dumped "
     "or not. No effect on older servers that do not support the server side "
     "option.",
     &opt_show_create_table_skip_secondary_engine,
     &opt_show_create_table_skip_secondary_engine, nullptr, GET_BOOL, NO_ARG, 0,
     0, 0, nullptr, 0, nullptr},
    {"compression-algorithms", 0,
     "Use compression algorithm in server/client protocol. Valid values "
     "are any combination of 'zstd','zlib','uncompressed'.",
     &opt_compress_algorithm, &opt_compress_algorithm, nullptr, GET_STR,
     REQUIRED_ARG, 0, 0, 0, nullptr, 0, nullptr},
    {"zstd-compression-level", 0,
     "Use this compression level in the client/server protocol, in case "
     "--compression-algorithms=zstd. Valid range is between 1 and 22, "
     "inclusive. Default is 3.",
     &opt_zstd_compress_level, &opt_zstd_compress_level, nullptr, GET_UINT,
     REQUIRED_ARG, 3, 1, 22, nullptr, 0, nullptr},
    {"skip-generated-invisible-primary-key", 0,
     "Controls whether generated invisible primary key and key column should "
     "be dumped or not.",
     &opt_skip_gipk, &opt_skip_gipk, nullptr, GET_BOOL, NO_ARG, 0, 0, 0,
     nullptr, 0, nullptr},
    {"init-command", OPT_INIT_COMMAND,
     "Single SQL Command to execute when connecting to MySQL server. Will "
     "automatically be re-executed when reconnecting.",
     nullptr, nullptr, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"init-command-add", OPT_INIT_COMMAND_ADD,
     "Add SQL command to the list to execute when connecting to MySQL server. "
     "Will automatically be re-executed when reconnecting.",
     nullptr, nullptr, nullptr, GET_STR, REQUIRED_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
    {"ignore-views", 0, "Skip dumping table views.", &opt_ignore_views,
     &opt_ignore_views, nullptr, GET_BOOL, OPT_ARG, 0, 0, 0, nullptr, 0,
     nullptr},
#include "client/include/authentication_kerberos_clientopt-longopts.h"
    {nullptr, 0, nullptr, nullptr, nullptr, nullptr, GET_NO_ARG, NO_ARG, 0, 0,
     0, nullptr, 0, nullptr}};

static const char *load_default_groups[] = {"mysqldump", "client", nullptr};

static void maybe_exit(int error);
static void die(int error, const char *reason, ...);
static void maybe_die(int error, const char *reason, ...);
static void write_header(FILE *sql_file, char *db_name);
static void print_value(FILE *file, MYSQL_RES *result, MYSQL_ROW row,
                        const char *prefix, const char *name, int string_value);
static int dump_selected_tables(char *db, char **table_names, int tables);
static int dump_all_tables_in_db(char *db);
static int init_dumping_views(char *);
static int init_dumping_tables(char *);
static int init_dumping(char *, int init_func(char *));
static int dump_databases(char **);
static int dump_all_databases();
static char *quote_name(char *name, char *buff, bool force);
static const char *quote_name(const char *name, char *buff, bool force);
char check_if_ignore_table(const char *table_name, char *table_type);
bool is_infoschema_db(const char *db);
static char *primary_key_fields(const char *table_name);
static bool get_view_structure(char *table, char *db);
static bool dump_all_views_in_db(char *database);
static bool get_gtid_mode(MYSQL *mysql_con);
static Output_as_version_mode get_output_as_version_mode();
static int set_terminology_use_previous_session_value(
    MYSQL *mysql_con, Output_as_version_mode mode_to_set);
static int dump_all_tablespaces();
static int dump_tablespaces_for_tables(char *db, char **table_names,
                                       int tables);
static int dump_tablespaces_for_databases(char **databases);
static int dump_tablespaces(char *ts_where);
static void print_comment(FILE *sql_file, bool is_error, const char *format,
                          ...);
static void verbose_msg(const char *fmt, ...)
    MY_ATTRIBUTE((format(printf, 1, 2)));
static char const *fix_identifier_with_newline(char const *object_name,
                                               bool *freemem);
static bool dump_users(FILE *sql_file);
static bool dump_grants(FILE *sql_file);
static bool fetch_users_list_if_include_is_empty();

static std::unordered_map<string, string> compatibility_rpl_replica_commands = {
    {"SHOW REPLICA STATUS", "SHOW SLAVE STATUS"},
    {"STOP REPLICA", "STOP SLAVE"},
    {"START REPLICA", "START SLAVE"},
    {"STOP REPLICA SQL_THREAD", "STOP SLAVE SQL_THREAD"},
    {"CHANGE REPLICATION SOURCE TO", "CHANGE MASTER TO"},
    {"SOURCE_HOST", "MASTER_HOST"},
    {"SOURCE_PORT", "MASTER_PORT"},
    {"SOURCE_LOG_FILE", "MASTER_LOG_FILE"},
    {"SOURCE_LOG_POS", "MASTER_LOG_POS"}};

static std::unordered_map<string, string> compatibility_rpl_source_commands = {
    {"SHOW BINARY LOG STATUS", "SHOW MASTER STATUS"}};

static string get_compatible_rpl_source_query(const string &command) {
  return ((opt_server_version < FIRST_REPLICA_COMMAND_VERSION)
              ? compatibility_rpl_source_commands.at(command)
              : command);
}

static string get_compatible_rpl_replica_query(const string &command) {
  return ((opt_server_version < FIRST_REPLICA_COMMAND_VERSION)
              ? compatibility_rpl_replica_commands.at(command)
              : command);
}

static string get_compatible_rpl_replica_command(const string &command) {
  return ((opt_output_as_version_mode == Output_as_version_mode::BEFORE_8_0_23)
              ? compatibility_rpl_replica_commands.at(command)
              : command);
}

/*
  Print the supplied message if in verbose mode

  SYNOPSIS
    verbose_msg()
    fmt   format specifier
    ...   variable number of parameters
*/

static void verbose_msg(const char *fmt, ...) {
  va_list args;
  DBUG_TRACE;

  if (!verbose) return;

  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);

  fflush(stderr);
}

/*
  exit with message if ferror(file)

  SYNOPSIS
    check_io()
    file        - checked file
*/

static void check_io(FILE *file) {
  if (ferror(file) || errno == 5) die(EX_EOF, "Got errno %d on write", errno);
}

static void short_usage_sub() {
  printf("Usage: %s [OPTIONS] database [tables]\n", my_progname);
  printf("OR     %s [OPTIONS] --databases [OPTIONS] DB1 [DB2 DB3...]\n",
         my_progname);
  printf("OR     %s [OPTIONS] --all-databases [OPTIONS]\n", my_progname);
}

static void usage() {
  print_version();
  puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2000"));
  puts("Dumping structure and contents of MySQL databases and tables.");
  short_usage_sub();
  print_defaults("my", load_default_groups);
  my_print_help(my_long_options);
  my_print_variables(my_long_options);
} /* usage */

static void short_usage() {
  short_usage_sub();
  printf("For more options, use %s --help\n", my_progname);
}

static void get_safe_server_info(char *safe_server_info,
                                 size_t safe_server_info_len) {
  const char *server_info = mysql_get_server_info(&mysql_connection);
  if (server_info == nullptr) {
    safe_server_info[0] = 0;
    return;
  }
  DBUG_EXECUTE_IF("server_version_injection_test", {
    const char *payload = "8.0.0-injection_test\n\\! touch /tmp/xxx";
    server_info = payload;
  });
  for (size_t i = 0; i < safe_server_info_len; ++i) {
    // End of string.
    if (server_info[i] == 0) {
      safe_server_info[i] = 0;
      return;
    }
    // Version may include only alphanumeric and punctuation characters.
    // Cut off the rest of the string if incorrect character found.
    if (!(isalnum(server_info[i]) || ispunct(server_info[i]))) {
      safe_server_info[i] = 0;
      fprintf(stderr,
              "-- Warning: version string returned by server is incorrect.\n");
      return;
    }
    safe_server_info[i] = server_info[i];
  }
  safe_server_info[safe_server_info_len - 1] = 0;
}

static void write_header(FILE *sql_file, char *db_name) {
  if (opt_xml) {
    fputs("<?xml version=\"1.0\"?>\n", sql_file);
    /*
      Schema reference.  Allows use of xsi:nil for NULL values and
      xsi:type to define an element's data type.
    */
    fputs("<mysqldump ", sql_file);
    fputs("xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"", sql_file);
    fputs(">\n", sql_file);
    check_io(sql_file);
  } else if (!opt_compact) {
    print_comment(
        sql_file, false, "-- MySQL dump %s  Distrib %s, for %s (%s)\n--\n",
        DUMP_VERSION, MYSQL_SERVER_VERSION, SYSTEM_TYPE, MACHINE_TYPE);

    bool freemem = false;
    char const *text = fix_identifier_with_newline(db_name, &freemem);
    char safe_server_info[SERVER_VERSION_LENGTH];
    get_safe_server_info(safe_server_info, SERVER_VERSION_LENGTH);
    print_comment(sql_file, false, "-- Host: %s    Database: %s\n",
                  current_host ? current_host : "localhost", text);
    if (freemem) my_free(const_cast<char *>(text));

    print_comment(
        sql_file, false,
        "-- ------------------------------------------------------\n");
    print_comment(sql_file, false, "-- Server version\t%s\n", safe_server_info);

    if (opt_set_charset)
      fprintf(
          sql_file,
          "\n/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;"
          "\n/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS "
          "*/;"
          "\n/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;"
          "\n/*!50503 SET NAMES %s */;\n",
          default_charset);

    if (opt_tz_utc) {
      fprintf(sql_file, "/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;\n");
      fprintf(sql_file, "/*!40103 SET TIME_ZONE='+00:00' */;\n");
    }
    if (stats_tables_included) {
      fprintf(sql_file,
              "/*!50606 SET "
              "@OLD_INNODB_STATS_AUTO_RECALC=@@INNODB_STATS_AUTO_RECALC */;\n");
      fprintf(sql_file,
              "/*!50606 SET GLOBAL INNODB_STATS_AUTO_RECALC=OFF */;\n");
    }
    if (!path) {
      fprintf(md_result_file,
              "\
/*!40014 SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0 */;\n\
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;\n\
");
    }
    const char *mode1 = path ? "" : "NO_AUTO_VALUE_ON_ZERO";
    const char *mode2 = ansi_mode ? "ANSI" : "";
    const char *comma = *mode1 && *mode2 ? "," : "";
    fprintf(sql_file,
            "/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='%s%s%s' */;\n"
            "/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;\n",
            mode1, comma, mode2);
    check_io(sql_file);
  }
} /* write_header */

static void write_footer(FILE *sql_file) {
  if (opt_xml) {
    fputs("</mysqldump>\n", sql_file);
    check_io(sql_file);
  } else if (!opt_compact) {
    if (opt_tz_utc)
      fprintf(sql_file, "/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;\n");
    if (stats_tables_included)
      fprintf(sql_file,
              "/*!50606 SET GLOBAL "
              "INNODB_STATS_AUTO_RECALC=@OLD_INNODB_STATS_AUTO_RECALC */;\n");

    fprintf(sql_file, "\n/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;\n");
    if (!path) {
      fprintf(md_result_file,
              "\
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;\n\
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;\n");
    }
    if (opt_set_charset)
      fprintf(
          sql_file,
          "/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;\n"
          "/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;\n"
          "/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;\n");
    fprintf(sql_file, "/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;\n");
    fputs("\n", sql_file);

    if (opt_dump_date) {
      char time_str[20];
      get_date(time_str, GETDATE_DATE_TIME, 0);
      print_comment(sql_file, false, "-- Dump completed on %s\n", time_str);
    } else
      print_comment(sql_file, false, "-- Dump completed\n");

    check_io(sql_file);
  }
} /* write_footer */

static bool get_one_option(int optid, const struct my_option *opt,
                           char *argument) {
  switch (optid) {
    PARSE_COMMAND_LINE_PASSWORD_OPTION;
    case 'r':
      if (!(md_result_file =
                my_fopen(argument, O_WRONLY | MY_FOPEN_BINARY, MYF(MY_WME))))
        exit(1);
      break;
    case 'W':
#ifdef _WIN32
      opt_protocol = MYSQL_PROTOCOL_PIPE;
#endif
      break;
    case 'N':
      opt_set_charset = false;
      break;
    case 'T':
      opt_disable_keys = false;

      if (strlen(argument) >= FN_REFLEN) {
        /*
          This check is made because the some the file functions below
          have FN_REFLEN sized stack allocated buffers and will cause
          a crash even if the input destination buffer is large enough
          to hold the output.
        */
        die(EX_USAGE, "Input filename too long: %s", argument);
      }

      break;
    case '#':
      DBUG_PUSH(argument ? argument : default_dbug_option);
      debug_check_flag = true;
      break;
#include "client/include/sslopt-case.h"

#include "client/include/authentication_kerberos_clientopt-case.h"

    case 'V':
      print_version();
      exit(0);
    case 'X':
      opt_xml = true;
      extended_insert = opt_drop = opt_lock = opt_disable_keys =
          opt_autocommit = opt_create_db = false;
      break;
    case 'i':
      opt_comments_used = true;
      break;
    case 'I':
    case '?':
      usage();
      exit(0);
    case (int)OPT_MASTER_DATA_DEPRECATED:
      CLIENT_WARN_DEPRECATED("--master-data", "--source-data");
      [[fallthrough]];
    case (int)OPT_SOURCE_DATA:
      if (!argument) /* work like in old versions */
        opt_source_data = MYSQL_OPT_SOURCE_DATA_EFFECTIVE_SQL;
      break;
    case (int)OPT_MYSQLDUMP_SLAVE_APPLY_DEPRECATED:
      CLIENT_WARN_DEPRECATED("--apply-slave-statements",
                             "--apply-replica-statements");
      break;
    case (int)OPT_DELETE_MASTER_LOGS_DEPRECATED:
      CLIENT_WARN_DEPRECATED("--delete-master-logs", "--delete-source-logs");
      break;
    case (int)OPT_MYSQLDUMP_SLAVE_DATA_DEPRECATED:
      CLIENT_WARN_DEPRECATED("--dump-slave", "--dump-replica");
      [[fallthrough]];
    case (int)OPT_MYSQLDUMP_REPLICA_DATA:
      if (!argument) /* work like in old versions */
        opt_replica_data = MYSQL_OPT_REPLICA_DATA_EFFECTIVE_SQL;
      break;
    case (int)OPT_MYSQLDUMP_INCLUDE_MASTER_HOST_PORT_DEPRECATED:
      CLIENT_WARN_DEPRECATED("--include-master-host-port",
                             "--include-source-host-port");
      break;
    case (int)OPT_OPTIMIZE:
      extended_insert = opt_drop = opt_lock = quick = create_options =
          opt_disable_keys = lock_tables = opt_set_charset = true;
      break;
    case (int)OPT_SKIP_OPTIMIZATION:
      extended_insert = opt_drop = opt_lock = quick = create_options =
          opt_disable_keys = lock_tables = opt_set_charset = false;
      break;
    case (int)OPT_COMPACT:
      if (opt_compact) {
        opt_comments = opt_drop = opt_disable_keys = opt_lock = opt_tz_utc =
            opt_set_charset = false;
      }
      break;
    case (int)OPT_TABLES:
      opt_databases = false;
      break;
    case (int)OPT_IGNORE_TABLE: {
      if (!strchr(argument, '.')) {
        fprintf(stderr,
                "Illegal use of option --ignore-table=<database>.<table>\n");
        exit(1);
      }
      ignore_table->emplace(argument);
      break;
    }
    case (int)OPT_MYSQLDUMP_EXCLUDE_USER: {
      if (!strchr(argument, '@')) {
        fprintf(stderr, "Illegal use of option --exclude-user=<user>@<host>\n");
        exit(1);
      }
      exclude_user->emplace_front(argument);
      break;
    }
    case (int)OPT_MYSQLDUMP_INCLUDE_USER: {
      if (!strchr(argument, '@')) {
        fprintf(stderr, "Illegal use of option --include-user=<user>@<host>\n");
        exit(1);
      }
      include_user->emplace(argument);
      break;
    }
    case (int)OPT_COMPATIBLE: {
      if (native_strcasecmp("ANSI", argument) != 0) {
        fprintf(stderr, "Invalid mode to --compatible: %s\n", argument);
        exit(1);
      }

      opt_quoted = true;
      opt_set_charset = false;
      ansi_quotes_mode = true;
      ansi_mode = true;
      /*
        Set charset to the default compiled value if it hasn't
        been reset yet by --default-character-set=xxx.
      */
      if (default_charset == mysql_universal_client_charset)
        default_charset = MYSQL_DEFAULT_CHARSET_NAME;
      break;
    }
    case (int)OPT_ENABLE_CLEARTEXT_PLUGIN:
      using_opt_enable_cleartext_plugin = true;
      break;
    case (int)OPT_MYSQL_PROTOCOL:
      opt_protocol =
          find_type_or_exit(argument, &sql_protocol_typelib, opt->name);
      break;
    case (int)OPT_SET_GTID_PURGED: {
      if (argument)
        opt_set_gtid_purged_mode = static_cast<enum_set_gtid_purged_mode>(
            find_type_or_exit(argument, &set_gtid_purged_mode_typelib,
                              opt->name) -
            1);
      break;
    }
    case (int)OPT_MYSQLDUMP_IGNORE_ERROR:
      /* Store the supplied list of errors into an array. */
      if (parse_ignore_error()) exit(EX_EOM);
      break;
    case (int)OPT_LONG_QUERY_TIME:
      long_query_time_opt_provided = true;
      break;
    case 'C':
      CLIENT_WARN_DEPRECATED("--compress", "--compression-algorithms");
      break;
    case OPT_INIT_COMMAND:
      opt_init_commands.add_value(argument, true);
      break;
    case OPT_INIT_COMMAND_ADD:
      opt_init_commands.add_value(argument, false);
      break;
    case (int)OPT_OUTPUT_AS_VERSION: {
      if (argument)
        opt_output_as_version_mode = static_cast<Output_as_version_mode>(
            find_type_or_exit(argument, &set_output_as_version_mode_typelib,
                              opt->name) -
            1);
      break;
    }
  }
  return false;
}

static int get_options(int *argc, char ***argv) {
  int ho_error;

  if (mysql_get_option(nullptr, MYSQL_OPT_MAX_ALLOWED_PACKET,
                       &opt_max_allowed_packet) ||
      mysql_get_option(nullptr, MYSQL_OPT_NET_BUFFER_LENGTH,
                       &opt_max_allowed_packet)) {
    exit(1);
  }

  md_result_file = stdout;
  my_getopt_use_args_separator = true;
  if (load_defaults("my", load_default_groups, argc, argv, &argv_alloc))
    return 1;
  my_getopt_use_args_separator = false;

  ignore_table =
      new collation_unordered_set<string>(charset_info, PSI_NOT_INSTRUMENTED);
  exclude_user = new std::forward_list<string>();
  include_user =
      new collation_unordered_set<string>(charset_info, PSI_NOT_INSTRUMENTED);
  /* Don't copy internal log tables */
  ignore_table->insert("mysql.apply_status");
  ignore_table->insert("mysql.schema");
  ignore_table->insert("mysql.general_log");
  ignore_table->insert("mysql.slow_log");

  if ((ho_error = handle_options(argc, argv, my_long_options, get_one_option)))
    return (ho_error);

  if (mysql_options(nullptr, MYSQL_OPT_MAX_ALLOWED_PACKET,
                    &opt_max_allowed_packet) ||
      mysql_options(nullptr, MYSQL_OPT_NET_BUFFER_LENGTH,
                    &opt_net_buffer_length)) {
    exit(1);
  }
  if (opt_dump_users) {
    if (flush_privileges) {
      fprintf(
          stderr,
          "%s: The --users option is incompatible with --flush-privileges\n",
          my_progname);
      return (EX_USAGE);
    }
    if (opt_xml) {
      fprintf(stderr, "%s: The --users option is incompatible with --xml\n",
              my_progname);
      return (EX_USAGE);
    }
    /* ignore the ACL tables if we are dumping logical ACL */
    ignore_table->insert("mysql.user");
    ignore_table->insert("mysql.global_grants");
    ignore_table->insert("mysql.db");
    ignore_table->insert("mysql.tables_priv");
    ignore_table->insert("mysql.columns_priv");
    ignore_table->insert("mysql.procs_priv");
    ignore_table->insert("mysql.proxies_priv");
    ignore_table->insert("mysql.default_roles");
    ignore_table->insert("mysql.role_edges");
    ignore_table->insert("mysql.password_history");
  } else {
    if (include_user->size() > 0) {
      fprintf(stderr,
              "%s: The --include-user option is a no-op without --users\n",
              my_progname);
    }
    if (!exclude_user->empty()) {
      fprintf(stderr,
              "%s: The --exclude-user option is a no-op without --users\n",
              my_progname);
    }
    if (opt_add_drop_user) {
      fprintf(stderr,
              "%s: The --add-drop-user option is a no-op without --users\n",
              my_progname);
    }
  }

  if (debug_info_flag) my_end_arg = MY_CHECK_ERROR | MY_GIVE_INFO;
  if (debug_check_flag) my_end_arg = MY_CHECK_ERROR;

  if (!path && (enclosed || opt_enclosed || escaped || lines_terminated ||
                fields_terminated)) {
    fprintf(stderr, "%s: You must use option --tab with --fields-...\n",
            my_progname);
    return (EX_USAGE);
  }

  /* We don't delete source logs if replica data option */
  if (opt_replica_data) {
    opt_lock_all_tables = !opt_single_transaction;
    opt_source_data = 0;
    opt_delete_source_logs = false;
  }

  /* Ensure consistency of the set of binlog & locking options */
  if (opt_delete_source_logs && !opt_source_data)
    opt_source_data = MYSQL_OPT_SOURCE_DATA_COMMENTED_SQL;
  if (opt_single_transaction && opt_lock_all_tables) {
    fprintf(stderr,
            "%s: You can't use --single-transaction and "
            "--lock-all-tables at the same time.\n",
            my_progname);
    return (EX_USAGE);
  }
  if (opt_source_data) {
    opt_lock_all_tables = !opt_single_transaction;
    opt_replica_data = 0;
  }
  if (opt_single_transaction || opt_lock_all_tables) lock_tables = false;
  if (enclosed && opt_enclosed) {
    fprintf(stderr,
            "%s: You can't use ..enclosed.. and ..optionally-enclosed.. at the "
            "same time.\n",
            my_progname);
    return (EX_USAGE);
  }
  if ((opt_databases || opt_alldbs) && path) {
    fprintf(stderr,
            "%s: --databases or --all-databases can't be used with --tab.\n",
            my_progname);
    return (EX_USAGE);
  }
  if (0 != strcmp(default_charset, charset_info->csname) &&
      !(charset_info =
            get_charset_by_csname(default_charset, MY_CS_PRIMARY, MYF(MY_WME))))
    exit(1);
  if ((*argc < 1 && !opt_alldbs && !opt_dump_users) ||
      (*argc > 0 && opt_alldbs)) {
    short_usage();
    return EX_USAGE;
  }
  return (0);
} /* get_options */

/*
** DB_error -- prints mysql error message and exits the program.
*/
static void DB_error(MYSQL *mysql_arg, const char *when) {
  DBUG_TRACE;
  maybe_die(EX_MYSQLERR, "Got error: %d: %s %s", mysql_errno(mysql_arg),
            mysql_error(mysql_arg), when);
}

/*
  Prints out an error message and kills the process.

  SYNOPSIS
    die()
    error_num   - process return value
    fmt_reason  - a format string for use by my_vsnprintf.
    ...         - variable arguments for above fmt_reason string

  DESCRIPTION
    This call prints out the formatted error message to stderr and then
    terminates the process.
*/
static void die(int error_num, const char *fmt_reason, ...)
    MY_ATTRIBUTE((format(printf, 2, 3)));

static void die(int error_num, const char *fmt_reason, ...) {
  char buffer[1000];
  va_list args;
  va_start(args, fmt_reason);
  vsnprintf(buffer, sizeof(buffer), fmt_reason, args);
  va_end(args);

  fprintf(stderr, "%s: %s\n", my_progname, buffer);
  fflush(stderr);

  /* force the exit */
  opt_force = false;
  if (opt_ignore_error) my_free(opt_ignore_error);
  opt_ignore_error = nullptr;

  maybe_exit(error_num);
}

/*
  Prints out an error message and maybe kills the process.

  SYNOPSIS
    maybe_die()
    error_num   - process return value
    fmt_reason  - a format string for use by my_vsnprintf.
    ...         - variable arguments for above fmt_reason string

  DESCRIPTION
    This call prints out the formatted error message to stderr and then
    terminates the process, unless the --force command line option is used.

    This call should be used for non-fatal errors (such as database
    errors) that the code may still be able to continue to the next unit
    of work.

*/
static void maybe_die(int error_num, const char *fmt_reason, ...)
    MY_ATTRIBUTE((format(printf, 2, 3)));

static void maybe_die(int error_num, const char *fmt_reason, ...) {
  char buffer[1000];
  va_list args;
  va_start(args, fmt_reason);
  vsnprintf(buffer, sizeof(buffer), fmt_reason, args);
  va_end(args);

  fprintf(stderr, "%s: %s\n", my_progname, buffer);
  fflush(stderr);

  maybe_exit(error_num);
}

/*
  Sends a query to server, optionally reads result, prints error message if
  some.

  SYNOPSIS
    mysql_query_with_error_report()
    mysql_con       connection to use
    res             if non zero, result will be put there with
                    mysql_store_result()
    query           query to send to server

  RETURN VALUES
    0               query sending and (if res!=0) result reading went ok
    1               error
*/

static int mysql_query_with_error_report(MYSQL *mysql_con, MYSQL_RES **res,
                                         const char *query) {
  if (mysql_query(mysql_con, query) ||
      (res && !((*res) = mysql_store_result(mysql_con)))) {
    maybe_die(EX_MYSQLERR, "Couldn't execute '%s': %s (%d)", query,
              mysql_error(mysql_con), mysql_errno(mysql_con));
    return 1;
  }
  return 0;
}

static int fetch_db_collation(const char *db_name, char *db_cl_name,
                              int db_cl_size) {
  bool err_status = false;
  char query[QUERY_LENGTH];
  MYSQL_RES *db_cl_res;
  MYSQL_ROW db_cl_row;
  char quoted_database_buf[NAME_LEN * 2 + 3];
  const char *qdatabase = quote_name(db_name, quoted_database_buf, true);

  snprintf(query, sizeof(query), "use %s", qdatabase);

  if (mysql_query_with_error_report(mysql, nullptr, query)) return 1;

  if (mysql_query_with_error_report(mysql, &db_cl_res,
                                    "select @@collation_database"))
    return 1;

  do {
    if (mysql_num_rows(db_cl_res) != 1) {
      err_status = true;
      break;
    }

    if (!(db_cl_row = mysql_fetch_row(db_cl_res))) {
      err_status = true;
      break;
    }

    strncpy(db_cl_name, db_cl_row[0], db_cl_size - 1);
    db_cl_name[db_cl_size - 1] = 0;

  } while (false);

  mysql_free_result(db_cl_res);

  return err_status ? 1 : 0;
}

static char *my_case_str(char *str, size_t str_len, const char *token,
                         size_t token_len) {
  my_match_t match;

  const bool status = my_charset_latin1.coll->strstr(
      &my_charset_latin1, str, str_len, token, token_len, &match);

  return status ? str + match.end : nullptr;
}

static int switch_db_collation(FILE *sql_file, const char *db_name,
                               const char *delimiter,
                               const char *current_db_cl_name,
                               const char *required_db_cl_name,
                               int *db_cl_altered) {
  if (strcmp(current_db_cl_name, required_db_cl_name) != 0) {
    char quoted_db_buf[NAME_LEN * 2 + 3];
    const char *quoted_db_name = quote_name(db_name, quoted_db_buf, false);

    CHARSET_INFO *db_cl = get_charset_by_name(required_db_cl_name, MYF(0));

    if (!db_cl) return 1;

    fprintf(sql_file, "ALTER DATABASE %s CHARACTER SET %s COLLATE %s %s\n",
            quoted_db_name, db_cl->csname, db_cl->m_coll_name, delimiter);

    *db_cl_altered = 1;

    return 0;
  }

  *db_cl_altered = 0;

  return 0;
}

static int restore_db_collation(FILE *sql_file, const char *db_name,
                                const char *delimiter, const char *db_cl_name) {
  char quoted_db_buf[NAME_LEN * 2 + 3];
  const char *quoted_db_name = quote_name(db_name, quoted_db_buf, false);

  CHARSET_INFO *db_cl = get_charset_by_name(db_cl_name, MYF(0));

  if (!db_cl) return 1;

  fprintf(sql_file, "ALTER DATABASE %s CHARACTER SET %s COLLATE %s %s\n",
          quoted_db_name, db_cl->csname, db_cl->m_coll_name, delimiter);

  return 0;
}

static void switch_cs_variables(FILE *sql_file, const char *delimiter,
                                const char *character_set_client,
                                const char *character_set_results,
                                const char *collation_connection) {
  fprintf(sql_file,
          "/*!50003 SET @saved_cs_client      = @@character_set_client */ %s\n"
          "/*!50003 SET @saved_cs_results     = @@character_set_results */ %s\n"
          "/*!50003 SET @saved_col_connection = @@collation_connection */ %s\n"
          "/*!50003 SET character_set_client  = %s */ %s\n"
          "/*!50003 SET character_set_results = %s */ %s\n"
          "/*!50003 SET collation_connection  = %s */ %s\n",
          (const char *)delimiter, (const char *)delimiter,
          (const char *)delimiter,

          (const char *)character_set_client, (const char *)delimiter,

          (const char *)character_set_results, (const char *)delimiter,

          (const char *)collation_connection, (const char *)delimiter);
}

static void restore_cs_variables(FILE *sql_file, const char *delimiter) {
  fprintf(sql_file,
          "/*!50003 SET character_set_client  = @saved_cs_client */ %s\n"
          "/*!50003 SET character_set_results = @saved_cs_results */ %s\n"
          "/*!50003 SET collation_connection  = @saved_col_connection */ %s\n",
          (const char *)delimiter, (const char *)delimiter,
          (const char *)delimiter);
}

static void switch_sql_mode(FILE *sql_file, const char *delimiter,
                            const char *sql_mode) {
  fprintf(sql_file,
          "/*!50003 SET @saved_sql_mode       = @@sql_mode */ %s\n"
          "/*!50003 SET sql_mode              = '%s' */ %s\n",
          (const char *)delimiter,

          (const char *)sql_mode, (const char *)delimiter);
}

static void restore_sql_mode(FILE *sql_file, const char *delimiter) {
  fprintf(sql_file,
          "/*!50003 SET sql_mode              = @saved_sql_mode */ %s\n",
          (const char *)delimiter);
}

static void switch_time_zone(FILE *sql_file, const char *delimiter,
                             const char *time_zone) {
  fprintf(sql_file,
          "/*!50003 SET @saved_time_zone      = @@time_zone */ %s\n"
          "/*!50003 SET time_zone             = '%s' */ %s\n",
          (const char *)delimiter,

          (const char *)time_zone, (const char *)delimiter);
}

static void restore_time_zone(FILE *sql_file, const char *delimiter) {
  fprintf(sql_file,
          "/*!50003 SET time_zone             = @saved_time_zone */ %s\n",
          (const char *)delimiter);
}

/**
  Switch charset for results to some specified charset.  If the server does not
  support character_set_results variable, nothing can be done here.  As for
  whether something should be done here, future new callers of this function
  should be aware that the server lacking the facility of switching charsets is
  treated as success.

  @note  If the server lacks support, then nothing is changed and no error
         condition is returned.

  @returns  whether there was an error or not
*/
static int switch_character_set_results(MYSQL *mysql, const char *cs_name) {
  char query_buffer[QUERY_LENGTH];
  size_t query_length;

  /* Server lacks facility.  This is not an error, by arbitrary decision . */
  if (!server_supports_switching_charsets) return false;

  query_length =
      std::min<int>(sizeof(query_buffer) - 1,
                    snprintf(query_buffer, sizeof(query_buffer),
                             "SET SESSION character_set_results = '%s'",
                             (const char *)cs_name));

  return mysql_real_query(mysql, query_buffer, (ulong)query_length);
}

/**
  Rewrite statement, enclosing DEFINER clause in version-specific comment.

  This function parses any CREATE statement and encloses DEFINER-clause in
  version-specific comment:
    input query:     CREATE DEFINER=a@b FUNCTION ...
    rewritten query: CREATE * / / *!50020 DEFINER=a@b * / / *!50003 FUNCTION ...

  @note This function will go away when WL#3995 is implemented.

  @param[in] stmt_str                 CREATE statement string.
  @param[in] stmt_length              Length of the stmt_str.
  @param[in] definer_version_str      Minimal MySQL version number when
                                      DEFINER clause is supported in the
                                      given statement.
  @param[in] definer_version_length   Length of definer_version_str.
  @param[in] stmt_version_str         Minimal MySQL version number when the
                                      given statement is supported.
  @param[in] stmt_version_length      Length of stmt_version_str.
  @param[in] keyword_str              Keyword to look for after CREATE.
  @param[in] keyword_length           Length of keyword_str.

  @return pointer to the new allocated query string.
*/

static char *cover_definer_clause(char *stmt_str, size_t stmt_length,
                                  const char *definer_version_str,
                                  size_t definer_version_length,
                                  const char *stmt_version_str,
                                  size_t stmt_version_length,
                                  const char *keyword_str,
                                  size_t keyword_length) {
  char *definer_begin =
      my_case_str(stmt_str, stmt_length, STRING_WITH_LEN(" DEFINER"));
  char *definer_end = nullptr;

  char *query_str = nullptr;
  char *query_ptr;

  if (!definer_begin) return nullptr;

  definer_end = my_case_str(definer_begin, strlen(definer_begin), keyword_str,
                            keyword_length);

  if (!definer_end) return nullptr;

  /*
    Allocate memory for new query string: original string
    from SHOW statement and version-specific comments.
  */
  query_str = alloc_query_str(stmt_length + 23);

  constexpr const char comment_str[] = "*/ /*!";

  query_ptr = my_stpncpy(query_str, stmt_str, definer_begin - stmt_str);
  query_ptr = my_stpncpy(query_ptr, comment_str, sizeof(comment_str));
  query_ptr =
      my_stpncpy(query_ptr, definer_version_str, definer_version_length);
  query_ptr = my_stpncpy(query_ptr, definer_begin, definer_end - definer_begin);
  query_ptr = my_stpncpy(query_ptr, comment_str, sizeof(comment_str));
  query_ptr = my_stpncpy(query_ptr, stmt_version_str, stmt_version_length);
  query_ptr = strxmov(query_ptr, definer_end, NullS);
  assert(query_ptr <= query_str + stmt_length + 23);

  return query_str;
}

/*
  Open a new .sql file to dump the table or view into

  SYNOPSIS
    open_sql_file_for_table
    name      name of the table or view
    flags     flags (as per "man 2 open")

  RETURN VALUES
    0        Failed to open file
    > 0      Handle of the open file
*/
static FILE *open_sql_file_for_table(const char *table, int flags) {
  FILE *res;
  char filename[FN_REFLEN], tmp_path[FN_REFLEN];
  convert_dirname(tmp_path, path, NullS);
  res = my_fopen(fn_format(filename, table, tmp_path, ".sql",
                           MYF(MY_UNPACK_FILENAME | MY_APPEND_EXT)),
                 flags, MYF(MY_WME));
  return res;
}

static void free_resources() {
  if (md_result_file && md_result_file != stdout)
    my_fclose(md_result_file, MYF(0));
  free_passwords();
  delete ignore_table;
  ignore_table = nullptr;
  delete include_user;
  include_user = nullptr;
  delete exclude_user;
  exclude_user = nullptr;
  if (insert_pat_inited) dynstr_free(&insert_pat);
  if (opt_ignore_error) my_free(opt_ignore_error);
  opt_init_commands.free();
  my_end(my_end_arg);
}

/**
  Parse the list of error numbers to be ignored and store into a dynamic
  array.

  @return Operation status
      @retval 0    Success
      @retval >0   Failure
*/
static int parse_ignore_error() {
  const char *search = ",";
  char *token;
  uint my_err;

  DBUG_TRACE;

  token = strtok(opt_ignore_error, search);

  while (token != nullptr) {
    my_err = atoi(token);
    // filter out 0s, if any
    if (my_err != 0) {
      if (ignore_error.push_back(my_err)) goto error;
    }
    token = strtok(nullptr, search);
  }
  return 0;

error:
  return EX_EOM;
}

/**
  Check if the last error should be ignored.
      @retval 1     yes
              0     no
*/
static bool do_ignore_error() {
  uint last_errno;
  bool found = false;

  DBUG_TRACE;

  last_errno = mysql_errno(mysql);

  if (last_errno == 0) goto done;

  for (unsigned int &it : ignore_error) {
    if (last_errno == it) {
      found = true;
      break;
    }
  }
done:
  return found;
}

static void maybe_exit(int error) {
  if (!first_error) first_error = error;

  /*
    Return if --force is used; else return only if the
    last error number is in the list of error numbers
    specified using --ignore-error option.
  */
  if (opt_force || (opt_ignore_error && do_ignore_error())) return;
  if (mysql) mysql_close(mysql);
  free_resources();
  exit(error);
}

/*
  db_connect -- connects to the host and selects DB.
*/

static int connect_to_db(char *host, char *user) {
  char buff[20 + FN_REFLEN];
  DBUG_TRACE;

  verbose_msg("-- Connecting to %s...\n", host ? host : "localhost");
  mysql_init(&mysql_connection);

  opt_init_commands.set_mysql_options(&mysql_connection, MYSQL_INIT_COMMAND);

  if (opt_compress) mysql_options(&mysql_connection, MYSQL_OPT_COMPRESS, NullS);
  if (SSL_SET_OPTIONS(&mysql_connection)) {
    fprintf(stderr, "%s", SSL_SET_OPTIONS_ERROR);
    return 1;
  }
  if (opt_protocol)
    mysql_options(&mysql_connection, MYSQL_OPT_PROTOCOL, (char *)&opt_protocol);
  if (opt_bind_addr)
    mysql_options(&mysql_connection, MYSQL_OPT_BIND, opt_bind_addr);
#if defined(_WIN32)
  if (shared_memory_base_name)
    mysql_options(&mysql_connection, MYSQL_SHARED_MEMORY_BASE_NAME,
                  shared_memory_base_name);
#endif
  mysql_options(&mysql_connection, MYSQL_SET_CHARSET_NAME, default_charset);

  if (opt_plugin_dir && *opt_plugin_dir)
    mysql_options(&mysql_connection, MYSQL_PLUGIN_DIR, opt_plugin_dir);

  if (opt_default_auth && *opt_default_auth)
    mysql_options(&mysql_connection, MYSQL_DEFAULT_AUTH, opt_default_auth);

  if (using_opt_enable_cleartext_plugin)
    mysql_options(&mysql_connection, MYSQL_ENABLE_CLEARTEXT_PLUGIN,
                  (char *)&opt_enable_cleartext_plugin);

  mysql_options(&mysql_connection, MYSQL_OPT_CONNECT_ATTR_RESET, nullptr);
  mysql_options4(&mysql_connection, MYSQL_OPT_CONNECT_ATTR_ADD, "program_name",
                 "mysqldump");
  set_server_public_key(&mysql_connection);
  set_get_server_public_key_option(&mysql_connection);
  set_password_options(&mysql_connection);

#if defined(_WIN32)
  char error[256]{0};
  if (set_authentication_kerberos_client_mode(&mysql_connection, error, 255)) {
    fprintf(stderr, "%s", error);
    return 1;
  }
#endif /* _WIN32 */

  if (opt_compress_algorithm)
    mysql_options(&mysql_connection, MYSQL_OPT_COMPRESSION_ALGORITHMS,
                  opt_compress_algorithm);

  mysql_options(&mysql_connection, MYSQL_OPT_ZSTD_COMPRESSION_LEVEL,
                &opt_zstd_compress_level);

  if (opt_network_timeout) {
    uint timeout = 86400;  // 1 day in seconds
    ulong max_packet_allowed = 1024L * 1024L * 1024L;

    mysql_options(&mysql_connection, MYSQL_OPT_READ_TIMEOUT, (char *)&timeout);
    mysql_options(&mysql_connection, MYSQL_OPT_WRITE_TIMEOUT, (char *)&timeout);
    /* set to maximum value which is 1GB */
    mysql_options(&mysql_connection, MYSQL_OPT_MAX_ALLOWED_PACKET,
                  (char *)&max_packet_allowed);
  }

  if (!(mysql =
            mysql_real_connect(&mysql_connection, host, user, nullptr, nullptr,
                               opt_mysql_port, opt_mysql_unix_port, 0))) {
    DB_error(&mysql_connection, "when trying to connect");
    return 1;
  }

  if (ssl_client_check_post_connect_ssl_setup(
          mysql, [](const char *err) { fprintf(stderr, "%s\n", err); }))
    return 1;
  if (mysql_get_server_version(&mysql_connection) < 40100) {
    /* Don't dump SET NAMES with a pre-4.1 server (bug#7997).  */
    opt_set_charset = false;

    /* Don't switch charsets for 4.1 and earlier.  (bug#34192). */
    server_supports_switching_charsets = false;
  }
  snprintf(buff, sizeof(buff), "/*!40100 SET @@SQL_MODE='%s' */",
           ansi_mode ? "ANSI" : "");
  if (mysql_query_with_error_report(mysql, nullptr, buff)) return 1;
  /*
    set time_zone to UTC to allow dumping date types between servers with
    different time zone settings
  */
  if (opt_tz_utc) {
    snprintf(buff, sizeof(buff), "/*!40103 SET TIME_ZONE='+00:00' */");
    if (mysql_query_with_error_report(mysql, nullptr, buff)) return 1;
  }

  /*
    With the introduction of new information schema views on top
    of new data dictionary, the way the SHOW command works is
    changed. We now have two ways of SHOW command picking table
    statistics.

    One is to read it from DD table mysql.table_stats and
    mysql.index_stats. For this to happen, we need to execute
    ANALYZE TABLE prior to execution of mysqldump tool.  As the
    tool can run on whole database, we would end-up running
    ANALYZE TABLE for all tables in the database irrespective of
    whether statistics are already present in the statistics
    tables. This could be a time consuming additional step to
    carry.

    Second option is to read statistics from SE itself. This
    options looks safe and execution of mysqldump tool need not
    care if ANALYZE TABLE command was run on every table. We
    always get the statistics, which match the behavior without
    data dictionary.

    The first option would be faster as we do not opening the
    underlying tables during execution of SHOW command. However
    the first option might read old statistics, so we feel second
    option is preferred here to get statistics dynamically from
    SE by setting information_schema_stats_expiry=0.
  */
  snprintf(buff, sizeof(buff),
           "/*!80000 SET SESSION information_schema_stats_expiry=0 */");
  if (mysql_query_with_error_report(mysql, nullptr, buff)) return 1;

  /*
    set network read/write timeout value to a larger value to allow tables with
    large data to be sent on network without causing connection lost error due
    to timeout.
    Additionally set long_query_time value for mysqldump session in the same
    query to possibly reduce one RTT.
  */
  if (opt_network_timeout || long_query_time_opt_provided) {
    size_t len = snprintf(buff, sizeof(buff), "SET ");
    if (opt_network_timeout) {
      len += snprintf(buff + len, sizeof(buff) - len,
                      "SESSION NET_READ_TIMEOUT= 86400, "
                      "SESSION NET_WRITE_TIMEOUT= 86400");  // 1 day in seconds
      if (long_query_time_opt_provided) {
        // delimiter needed for appending next variable
        len += snprintf(buff + len, sizeof(buff) - len, ", ");
      }
    }
    if (long_query_time_opt_provided) {
      // add snprintf result to len if new option gets added in the same request
      snprintf(buff + len, sizeof(buff) - len, "SESSION long_query_time=%lu",
               opt_long_query_time);
    }
    if (mysql_query_with_error_report(mysql, nullptr, buff)) return 1;
  }

  if (opt_show_create_table_skip_secondary_engine &&
      mysql_query_with_error_report(
          mysql, nullptr,
          "/*!80018 SET SESSION show_create_table_skip_secondary_engine=1 */"))
    return 1;

  if (opt_skip_gipk &&
      mysql_query_with_error_report(
          mysql, nullptr,
          "/*!80030 SET SESSION "
          "show_gipk_in_create_table_and_information_schema = OFF */"))
    return 1;

  return 0;
} /* connect_to_db */

/*
** dbDisconnect -- disconnects from the host.
*/
static void dbDisconnect(char *host) {
  verbose_msg("-- Disconnecting from %s...\n", host ? host : "localhost");
  mysql_close(mysql);
} /* dbDisconnect */

static void unescape(FILE *file, char *pos, size_t length) {
  char *tmp;
  DBUG_TRACE;
  if (!(tmp = (char *)my_malloc(PSI_NOT_INSTRUMENTED, length * 2 + 1,
                                MYF(MY_WME))))
    die(EX_MYSQLERR, "Couldn't allocate memory");

  mysql_real_escape_string_quote(&mysql_connection, tmp, pos, (ulong)length,
                                 '\'');
  fputc('\'', file);
  fputs(tmp, file);
  fputc('\'', file);
  check_io(file);
  my_free(tmp);
} /* unescape */

static bool test_if_special_chars(const char *str) {
  for (; *str; str++)
    if (!my_isvar(charset_info, *str) && *str != '$') return true;
  return false;
} /* test_if_special_chars */

/*
  quote_name(name, buff, force)

  Quotes char string, taking into account compatible mode

  Args

  name                 Unquoted string containing that which will be quoted
  buff                 The buffer that contains the quoted value, also returned
  force                Flag to make it ignore 'test_if_special_chars'

  Returns

  buff                 quoted string

*/
static char *quote_name(char *name, char *buff, bool force) {
  char *to = buff;
  const char qtype = ansi_quotes_mode ? '"' : '`';

  if (!force && !opt_quoted && !test_if_special_chars(name)) return name;
  *to++ = qtype;
  while (*name) {
    if (*name == qtype) *to++ = qtype;
    *to++ = *name++;
  }
  to[0] = qtype;
  to[1] = 0;
  return buff;
} /* quote_name */

static const char *quote_name(const char *name, char *buff, bool force) {
  return quote_name(const_cast<char *>(name), buff, force);
}

/*
  Quote a table name so it can be used in "SHOW TABLES LIKE <tabname>"

  SYNOPSIS
    quote_for_like()
    name     name of the table
    buff     quoted name of the table

  DESCRIPTION
    Quote \, _, ' and % characters

    Note: Because MySQL uses the C escape syntax in strings
    (for example, '\n' to represent newline), you must double
    any '\' that you use in your LIKE  strings. For example, to
    search for '\n', specify it as '\\n'. To search for '\', specify
    it as '\\\\' (the backslashes are stripped once by the parser
    and another time when the pattern match is done, leaving a
    single backslash to be matched).

    Example: "t\1" => "t\\\\1"

*/
static char *quote_for_like(const char *name, char *buff) {
  char *to = buff;
  *to++ = '\'';
  while (*name) {
    if (*name == '\\') {
      *to++ = '\\';
      *to++ = '\\';
      *to++ = '\\';
    } else if (*name == '\'' || *name == '_' || *name == '%')
      *to++ = '\\';
    *to++ = *name++;
  }
  to[0] = '\'';
  to[1] = 0;
  return buff;
}

/**
  Quote and print a string.

    Quote '<' '>' '&' '\"' chars and print a string to the xml_file.

  @param xml_file          Output file.
  @param str               String to print.
  @param len               Its length.
  @param is_attribute_name A check for attribute name or value.
*/

static void print_quoted_xml(FILE *xml_file, const char *str, size_t len,
                             bool is_attribute_name) {
  const char *end;

  for (end = str + len; str != end; str++) {
    switch (*str) {
      case '<':
        fputs("&lt;", xml_file);
        break;
      case '>':
        fputs("&gt;", xml_file);
        break;
      case '&':
        fputs("&amp;", xml_file);
        break;
      case '\"':
        fputs("&quot;", xml_file);
        break;
      case ' ':
        /* Attribute names cannot contain spaces. */
        if (is_attribute_name) {
          fputs("_", xml_file);
          break;
        }
        [[fallthrough]];
      default:
        fputc(*str, xml_file);
        break;
    }
  }
  check_io(xml_file);
}

/*
  Print xml tag. Optionally add attribute(s).

  SYNOPSIS
    print_xml_tag(xml_file, sbeg, send, tag_name, first_attribute_name,
                    ..., attribute_name_n, attribute_value_n, NullS)
    xml_file              - output file
    sbeg                  - line beginning
    line_end              - line ending
    tag_name              - XML tag name.
    first_attribute_name  - tag and first attribute
    first_attribute_value - (Implied) value of first attribute
    attribute_name_n      - attribute n
    attribute_value_n     - value of attribute n

  DESCRIPTION
    Print XML tag with any number of attribute="value" pairs to the xml_file.

    Format is:
      sbeg<tag_name first_attribute_name="first_attribute_value" ...
      attribute_name_n="attribute_value_n">send
  NOTE
    Additional arguments must be present in attribute/value pairs.
    The last argument should be the null character pointer.
    All attribute_value arguments MUST be NULL terminated strings.
    All attribute_value arguments will be quoted before output.
*/

static void print_xml_tag(FILE *xml_file, const char *sbeg,
                          const char *line_end, const char *tag_name,
                          const char *first_attribute_name, ...) {
  va_list arg_list;
  const char *attribute_name, *attribute_value;

  fputs(sbeg, xml_file);
  fputc('<', xml_file);
  fputs(tag_name, xml_file);

  va_start(arg_list, first_attribute_name);
  attribute_name = first_attribute_name;
  while (attribute_name != NullS) {
    attribute_value = va_arg(arg_list, char *);
    assert(attribute_value != NullS);

    fputc(' ', xml_file);
    fputs(attribute_name, xml_file);
    fputc('\"', xml_file);

    print_quoted_xml(xml_file, attribute_value, strlen(attribute_value), false);
    fputc('\"', xml_file);

    attribute_name = va_arg(arg_list, char *);
  }
  va_end(arg_list);

  fputc('>', xml_file);
  fputs(line_end, xml_file);
  check_io(xml_file);
}

/*
  Print xml tag with for a field that is null

  SYNOPSIS
    print_xml_null_tag()
    xml_file    - output file
    sbeg        - line beginning
    stag_atr    - tag and attribute
    sval        - value of attribute
    line_end        - line ending

  DESCRIPTION
    Print tag with one attribute to the xml_file. Format is:
      <stag_atr="sval" xsi:nil="true"/>
  NOTE
    sval MUST be a NULL terminated string.
    sval string will be quoted before output.
*/

static void print_xml_null_tag(FILE *xml_file, const char *sbeg,
                               const char *stag_atr, const char *sval,
                               const char *line_end) {
  fputs(sbeg, xml_file);
  fputs("<", xml_file);
  fputs(stag_atr, xml_file);
  fputs("\"", xml_file);
  print_quoted_xml(xml_file, sval, strlen(sval), false);
  fputs("\" xsi:nil=\"true\" />", xml_file);
  fputs(line_end, xml_file);
  check_io(xml_file);
}

/**
  Print xml CDATA section.

  @param xml_file    - output file
  @param str         - string to print
  @param len         - length of the string

  @note
    This function also takes care of the presence of '[[>'
    string in the str. If found, the CDATA section is broken
    into two CDATA sections, <![CDATA[]]]]> and <![CDATA[>]].
*/

static void print_xml_cdata(FILE *xml_file, const char *str, ulong len) {
  const char *end;

  fputs("<![CDATA[\n", xml_file);
  for (end = str + len; str != end; str++) {
    switch (*str) {
      case ']':
        if ((*(str + 1) == ']') && (*(str + 2) == '>')) {
          fputs("]]]]><![CDATA[>", xml_file);
          str += 2;
          continue;
        }
        [[fallthrough]];
      default:
        fputc(*str, xml_file);
        break;
    }
  }
  fputs("\n]]>\n", xml_file);
  check_io(xml_file);
}

/*
  Print xml tag with many attributes.

  SYNOPSIS
    print_xml_row()
    xml_file    - output file
    row_name    - xml tag name
    tableRes    - query result
    row         - result row
    str_create  - create statement header string

  DESCRIPTION
    Print tag with many attribute to the xml_file. Format is:
      \t\t<row_name Atr1="Val1" Atr2="Val2"... />
  NOTE
    All attributes and values will be quoted before output.
*/

static void print_xml_row(FILE *xml_file, const char *row_name,
                          MYSQL_RES *tableRes, MYSQL_ROW *row,
                          const char *str_create) {
  uint i;
  char *create_stmt_ptr = nullptr;
  ulong create_stmt_len = 0;
  MYSQL_FIELD *field;
  ulong *lengths = mysql_fetch_lengths(tableRes);

  fprintf(xml_file, "\t\t<%s", row_name);
  check_io(xml_file);
  mysql_field_seek(tableRes, 0);
  for (i = 0; (field = mysql_fetch_field(tableRes)); i++) {
    if ((*row)[i]) {
      /* For 'create' statements, dump using CDATA. */
      if ((str_create) && (strcmp(str_create, field->name) == 0)) {
        create_stmt_ptr = (*row)[i];
        create_stmt_len = lengths[i];
      } else {
        fputc(' ', xml_file);
        print_quoted_xml(xml_file, field->name, field->name_length, true);
        fputs("=\"", xml_file);
        print_quoted_xml(xml_file, (*row)[i], lengths[i], false);
        fputc('"', xml_file);
        check_io(xml_file);
      }
    }
  }

  if (create_stmt_len) {
    fputs(">\n", xml_file);
    print_xml_cdata(xml_file, create_stmt_ptr, create_stmt_len);
    fprintf(xml_file, "\t\t</%s>\n", row_name);
  } else
    fputs(" />\n", xml_file);

  check_io(xml_file);
}

/**
  Print xml comments.

    Print the comment message in the format:
      "<!-- \n comment string  \n -->\n"

  @param xml_file       output file
  @param len            length of comment message
  @param comment_string comment message

  @note
    Any occurrence of continuous hyphens will be
    squeezed to a single hyphen.
*/

static void print_xml_comment(FILE *xml_file, size_t len,
                              const char *comment_string) {
  const char *end;

  fputs("<!-- ", xml_file);

  for (end = comment_string + len; comment_string != end; comment_string++) {
    /*
      The string "--" (double-hyphen) MUST NOT occur within xml comments.
    */
    switch (*comment_string) {
      case '-':
        if (*(comment_string + 1) == '-') /* Only one hyphen allowed. */
          break;
        [[fallthrough]];
      default:
        fputc(*comment_string, xml_file);
        break;
    }
  }
  fputs(" -->\n", xml_file);
  check_io(xml_file);
}

/* A common printing function for xml and non-xml modes. */

static void print_comment(FILE *sql_file, bool is_error, const char *format,
                          ...) MY_ATTRIBUTE((format(printf, 3, 4)));

static void print_comment(FILE *sql_file, bool is_error, const char *format,
                          ...) {
  static char comment_buff[COMMENT_LENGTH];
  va_list args;

  /* If its an error message, print it ignoring opt_comments. */
  if (!is_error && !opt_comments) return;

  va_start(args, format);
  vsnprintf(comment_buff, COMMENT_LENGTH, format, args);
  va_end(args);

  if (!opt_xml) {
    fputs(comment_buff, sql_file);
    check_io(sql_file);
    return;
  }

  print_xml_comment(sql_file, strlen(comment_buff), comment_buff);
}

/**
  @brief Accepts object names and prefixes them with "-- " wherever
         end-of-line character ('\n') is found.

  @param[in]  object_name   object name list (concatenated string)
  @param[out] freemem       should buffer be released after usage

  @returns                  pointer to a string with prefixed objects
*/
static char const *fix_identifier_with_newline(char const *object_name,
                                               bool *freemem) {
  const size_t PREFIX_LENGTH = 3;  // strlen ("-- ")

  // static buffer for replacement procedure
  static char *buffer;
  static size_t buffer_size;
  static char storage[NAME_LEN + 1];

  // we presume memory allocation won't be needed
  *freemem = false;
  buffer = storage;
  buffer_size = sizeof(storage) - 1;

  // traverse and reformat objects
  size_t index = 0;
  size_t required_size = 0;
  while (object_name && *object_name) {
    ++required_size;
    if (*object_name == '\n') required_size += PREFIX_LENGTH;

    // do we need dynamic (re)allocation
    if (required_size > buffer_size) {
      // new alloc size increased in COMMENT_LENGTH multiple
      buffer_size = COMMENT_LENGTH * (1 + required_size / COMMENT_LENGTH);

      // is our buffer already dynamically allocated
      if (*freemem) {
        // just realloc
        buffer = (char *)my_realloc(PSI_NOT_INSTRUMENTED, buffer,
                                    buffer_size + 1, MYF(MY_WME));
        if (!buffer) exit(1);
      } else {
        // dynamic allocation + copy from static buffer
        buffer = (char *)my_malloc(PSI_NOT_INSTRUMENTED, buffer_size + 1,
                                   MYF(MY_WME));
        if (!buffer) exit(1);

        strncpy(buffer, storage, index);
        *freemem = true;
      }
    }

    // copy a character
    buffer[index] = *object_name;
    ++index;

    // prefix new lines with double dash
    if (*object_name == '\n') {
      strcpy(buffer + index, "-- ");
      index += PREFIX_LENGTH;
    }

    ++object_name;
  }

  // don't forget null termination
  buffer[index] = '\0';
  return buffer;
}

/*
 create_delimiter
 Generate a new (null-terminated) string that does not exist in  query
 and is therefore suitable for use as a query delimiter.  Store this
 delimiter in  delimiter_buff .

 This is quite simple in that it doesn't even try to parse statements as an
 interpreter would.  It merely returns a string that is not in the query, which
 is much more than adequate for constructing a delimiter.

 RETURN
   ptr to the delimiter  on Success
   NULL                  on Failure
*/
static char *create_delimiter(char *query, char *delimiter_buff,
                              int delimiter_max_size) {
  int proposed_length;
  char *presence;

  delimiter_buff[0] = ';'; /* start with one semicolon, and */

  for (proposed_length = 2; proposed_length < delimiter_max_size;
       delimiter_max_size++) {
    delimiter_buff[proposed_length - 1] = ';'; /* add semicolons, until */
    delimiter_buff[proposed_length] = '\0';

    presence = strstr(query, delimiter_buff);
    if (presence == nullptr) { /* the proposed delimiter is not in the query. */
      return delimiter_buff;
    }
  }
  return nullptr; /* but if we run out of space, return nothing at all. */
}

/*
 * fprintf_string:
 * -- Print the escaped version of the given char* row into the md_result_file.
 *
 * @param[in] row                 the row to be printed
 * @param[in] row_len             length of the row
 * @param[in] quote               quote character, like ' or ` etc.
 * @param[in] needs_newline       whether to print newline after the row
 *
 * @retval void
 *
 */
static void fprintf_string(char *row, ulong row_len, char quote,
                           bool needs_newline) {
  // Create the buffer where we'll have sanitized row.
  char buffer[2048];
  char *pbuffer;
  pbuffer = &buffer[0];

  uint64_t const curr_row_size = (static_cast<uint64_t>(row_len) * 2) + 1;

  // We'll allocate dynamic memory only for huge rows
  if (curr_row_size > sizeof(buffer))
    pbuffer = (char *)my_malloc(PSI_NOT_INSTRUMENTED, curr_row_size, MYF(0));

  // Put the sanitized row in the buffer.
  mysql_real_escape_string_quote(mysql, pbuffer, row, row_len, quote);

  // Opening quote
  fputc(quote, md_result_file);

  // Print the row to the file.
  fputs(pbuffer, md_result_file);

  // Closing quote
  fputc(quote, md_result_file);

  // Add the new line
  if (needs_newline) fputc('\n', md_result_file);

  // Free the buffer if we have to.
  if (pbuffer != &buffer[0]) my_free(pbuffer);
}

/*
 * is_string_integer:
 * Check if the given string is a valid integer or not.
 *
 * @param[in]  str       number to be checked
 * @param[in]  str_len   length of the string
 *
 * @retval     true      if the string represents an integer
 * @retval     false     if the string has non-digit characters
 */
static bool is_string_integer(const char *str, ulong str_len) {
  // Empty strings are invalid numbers
  if (str_len == 0) return false;

  ulong start_index = 0;

  // For negative integers, start the index with 1
  if (str[0] == '-') {
    if (str_len == 1) return false;
    start_index = 1;
  }

  for (ulong i = start_index; i < str_len; i++)
    if (!std::isdigit(str[i])) return false;

  return true;
}

/*
  dump_events_for_db
  -- retrieves list of events for a given db, and prints out
  the CREATE EVENT statement into the output (the dump).

  RETURN
    0  Success
    1  Error
*/
static uint dump_events_for_db(char *db) {
  char query_buff[QUERY_LENGTH];
  char db_name_buff[NAME_LEN * 2 + 3], name_buff[NAME_LEN * 2 + 3];
  char *event_name;
  char delimiter[QUERY_LENGTH];
  FILE *sql_file = md_result_file;
  MYSQL_RES *event_res, *event_list_res;
  MYSQL_ROW row, event_list_row;

  char db_cl_name[MY_CS_NAME_SIZE];
  int db_cl_altered = false;

  DBUG_TRACE;
  DBUG_PRINT("enter", ("db: '%s'", db));

  mysql_real_escape_string_quote(mysql, db_name_buff, db, (ulong)strlen(db),
                                 '\'');

  /* nice comments */
  bool freemem = false;
  char const *text = fix_identifier_with_newline(db, &freemem);
  print_comment(sql_file, false,
                "\n--\n-- Dumping events for database '%s'\n--\n", text);
  if (freemem) my_free(const_cast<char *>(text));

  if (mysql_query_with_error_report(mysql, &event_list_res, "show events"))
    return 0;

  strcpy(delimiter, ";");
  if (mysql_num_rows(event_list_res) > 0) {
    if (opt_xml)
      fputs("\t<events>\n", sql_file);
    else {
      fprintf(sql_file, "/*!50106 SET @save_time_zone= @@TIME_ZONE */ ;\n");

      /* Get database collation. */

      if (fetch_db_collation(db_name_buff, db_cl_name, sizeof(db_cl_name)))
        return 1;
    }

    if (switch_character_set_results(mysql, "binary")) return 1;

    while ((event_list_row = mysql_fetch_row(event_list_res)) != nullptr) {
      event_name = quote_name(event_list_row[1], name_buff, false);
      DBUG_PRINT("info", ("retrieving CREATE EVENT for %s", name_buff));
      snprintf(query_buff, sizeof(query_buff), "SHOW CREATE EVENT %s",
               event_name);

      if (mysql_query_with_error_report(mysql, &event_res, query_buff))
        return 1;

      while ((row = mysql_fetch_row(event_res)) != nullptr) {
        if (opt_xml) {
          print_xml_row(sql_file, "event", event_res, &row, "Create Event");
          continue;
        }

        /*
          if the user has EXECUTE privilege he can see event names, but not the
          event body!
        */
        if (strlen(row[3]) != 0) {
          char *query_str;

          if (opt_drop)
            fprintf(sql_file, "/*!50106 DROP EVENT IF EXISTS %s */%s\n",
                    event_name, delimiter);

          if (create_delimiter(row[3], delimiter, sizeof(delimiter)) ==
              nullptr) {
            fprintf(stderr,
                    "%s: Warning: Can't create delimiter for event '%s'\n",
                    my_progname, event_name);
            return 1;
          }

          fprintf(sql_file, "DELIMITER %s\n", delimiter);

          if (mysql_num_fields(event_res) >= 7) {
            if (switch_db_collation(sql_file, db_name_buff, delimiter,
                                    db_cl_name, row[6], &db_cl_altered)) {
              return 1;
            }

            switch_cs_variables(sql_file, delimiter,
                                row[4],  /* character_set_client */
                                row[4],  /* character_set_results */
                                row[5]); /* collation_connection */
          } else {
            /*
              mysqldump is being run against the server, that does not
              provide character set information in SHOW CREATE
              statements.

              NOTE: the dump may be incorrect, since character set
              information is required in order to restore event properly.
            */

            fprintf(sql_file,
                    "--\n"
                    "-- WARNING: old server version. "
                    "The following dump may be incomplete.\n"
                    "--\n");
          }

          switch_sql_mode(sql_file, delimiter, row[1]);

          switch_time_zone(sql_file, delimiter, row[2]);

          query_str = cover_definer_clause(
              row[3], strlen(row[3]), STRING_WITH_LEN("50117"),
              STRING_WITH_LEN("50106"), STRING_WITH_LEN(" EVENT"));

          fprintf(sql_file, "/*!50106 %s */ %s\n",
                  (const char *)(query_str != nullptr ? query_str : row[3]),
                  (const char *)delimiter);

          my_free(query_str);
          restore_time_zone(sql_file, delimiter);
          restore_sql_mode(sql_file, delimiter);

          if (mysql_num_fields(event_res) >= 7) {
            restore_cs_variables(sql_file, delimiter);

            if (db_cl_altered) {
              if (restore_db_collation(sql_file, db_name_buff, delimiter,
                                       db_cl_name))
                return 1;
            }
          }
        }
      } /* end of event printing */
      mysql_free_result(event_res);

    } /* end of list of events */
    if (opt_xml) {
      fputs("\t</events>\n", sql_file);
      check_io(sql_file);
    } else {
      fprintf(sql_file, "DELIMITER ;\n");
      fprintf(sql_file, "/*!50106 SET TIME_ZONE= @save_time_zone */ ;\n");
    }

    if (switch_character_set_results(mysql, default_charset)) return 1;
  }
  mysql_free_result(event_list_res);

  return 0;
}

/*
  Print hex value for blob data.

  SYNOPSIS
    print_blob_as_hex()
    output_file         - output file
    str                 - string to print
    len                 - its length

  DESCRIPTION
    Print hex value for blob data.
*/

static void print_blob_as_hex(FILE *output_file, const char *str, ulong len) {
  /* sakaik got the idea to to provide blob's in hex notation. */
  const char *ptr = str, *end = ptr + len;
  for (; ptr < end; ptr++)
    fprintf(output_file, "%02X", static_cast<uchar>(*ptr));
  check_io(output_file);
}

/*
  has_missing_import
  -- Checks if all the libraries imported by a routine are present.

  A function/procedure that has one or more of the libraries it imports,
  deleted, it cannot be re-created.
  Having such routines in the dump makes it invalid.

  RETURN
    false All the libraries imported by the routine exists.
    true  The routine has missing imports.
*/
static bool has_missing_import(const char *schema, const char *name) {
  MYSQL_RES *routine_list_res;
  std::string query{
      std::string{
          "SELECT rl.LIBRARY_SCHEMA, rl.LIBRARY_NAME, rl.ROUTINE_SCHEMA, "
          "rl.ROUTINE_NAME, rl.ROUTINE_TYPE "
          "FROM INFORMATION_SCHEMA.ROUTINE_LIBRARIES rl "
          "LEFT JOIN INFORMATION_SCHEMA.LIBRARIES lib ON "
          "rl.LIBRARY_CATALOG  = lib.LIBRARY_CATALOG AND "
          "rl.LIBRARY_SCHEMA = lib.LIBRARY_SCHEMA AND "
          "rl.LIBRARY_NAME = lib.LIBRARY_NAME "
          "WHERE lib.LIBRARY_NAME IS NULL AND rl.ROUTINE_SCHEMA = '"} +
      schema + "' AND rl.ROUTINE_NAME = '" + name + '\''};
  if (mysql_query_with_error_report(mysql, &routine_list_res, query.c_str()))
    return true;
  if (mysql_num_rows(routine_list_res))
    return true;  // There are imported libraries that do NOT exist.
  mysql_free_result(routine_list_res);
  return false;  // All the libraries that this routine imports, are present.
}

/*
  dump_routines_for_db
  -- retrieves list of routines for a given db, and prints out
  the CREATE PROCEDURE definition into the output (the dump).

  This function has logic to print the appropriate syntax depending on whether
  this is a procedure or functions

  RETURN
    0  Success
    1  Error
*/

static uint dump_routines_for_db(char *db) {
  char query_buff[QUERY_LENGTH];
  const char *routine_type[] = {"FUNCTION", "PROCEDURE"};
  char db_name_buff[NAME_LEN * 2 + 3], name_buff[NAME_LEN * 2 + 3];
  char *routine_name;
  int i;
  FILE *sql_file = md_result_file;
  MYSQL_RES *routine_res, *routine_list_res;
  MYSQL_ROW row, routine_list_row;

  char db_cl_name[MY_CS_NAME_SIZE];
  int db_cl_altered = false;

  DBUG_TRACE;
  DBUG_PRINT("enter", ("db: '%s'", db));

  mysql_real_escape_string_quote(mysql, db_name_buff, db, (ulong)strlen(db),
                                 '\'');

  /* nice comments */
  bool routines_freemem = false;
  char const *routines_text =
      fix_identifier_with_newline(db, &routines_freemem);
  print_comment(sql_file, false,
                "\n--\n-- Dumping routines for database '%s'\n--\n",
                routines_text);
  if (routines_freemem) my_free(const_cast<char *>(routines_text));

  /*
    not using "mysql_query_with_error_report" because we may have not
    enough privileges to lock mysql.proc.
  */
  if (lock_tables) mysql_query(mysql, "LOCK TABLES mysql.proc READ");

  /* Get database collation. */

  if (fetch_db_collation(db_name_buff, db_cl_name, sizeof(db_cl_name)))
    return 1;

  if (switch_character_set_results(mysql, "binary")) return 1;

  if (opt_xml) fputs("\t<routines>\n", sql_file);

  {
    /* First dump the libraries. They may be used by the routines. */
    std::string const query{
        std::string{
            "SELECT LIBRARY_NAME FROM INFORMATION_SCHEMA.LIBRARIES WHERE "
            "LIBRARY_SCHEMA = '"} +
        db_name_buff + "' ORDER BY LIBRARY_NAME"};

    if (mysql_query_with_error_report(mysql, &routine_list_res, query.c_str()))
      return 1;

    if (mysql_num_rows(routine_list_res)) {
      while ((routine_list_row = mysql_fetch_row(routine_list_res))) {
        routine_name = quote_name(routine_list_row[0], name_buff, false);
        DBUG_PRINT("info", ("retrieving CREATE LIBRARY for %s", name_buff));
        std::string const show_create_query{
            std::string{"SHOW CREATE LIBRARY "} + routine_name};
        if (mysql_query_with_error_report(mysql, &routine_res,
                                          show_create_query.c_str()))
          return 1;

        while ((row = mysql_fetch_row(routine_res))) {
          /*
            if the user has EXECUTE privilege he see library names, but NOT the
            library body of other routines that are not the creator of!
          */
          DBUG_PRINT("info",
                     ("length of body for %s row[2] '%s' is %zu", routine_name,
                      row[2] ? row[2] : "(null)", row[2] ? strlen(row[2]) : 0));
          if (row[2] == nullptr) {
            print_comment(sql_file, true,
                          "\n-- insufficient privileges to %s\n", query_buff);

            bool freemem = false;
            char const *text =
                fix_identifier_with_newline(current_user, &freemem);
            print_comment(sql_file, true,
                          "-- does %s have permissions on "
                          "INFORMATION_SCHEMA.LIBRARIES?\n\n",
                          text);
            if (freemem) my_free(const_cast<char *>(text));

            maybe_die(EX_MYSQLERR, "%s has insufficient privileges to %s!",
                      current_user, query_buff);
          } else if (strlen(row[2])) {
            if (opt_xml) {
              print_xml_row(sql_file, "library", routine_res, &row,
                            "Create Library");
              continue;
            }
            if (opt_drop)
              fprintf(sql_file, "DROP LIBRARY IF EXISTS %s;\n", routine_name);

            switch_sql_mode(sql_file, ";", row[1]);

            fprintf(sql_file,
                    "DELIMITER ;;\n"
                    "%s ;;\n"
                    "DELIMITER ;\n",
                    (const char *)row[2]);

            restore_sql_mode(sql_file, ";");
          }
        } /* end of library printing */
        mysql_free_result(routine_res);

      } /* end of list of libraries */
    }
    mysql_free_result(routine_list_res);
  }

  /* 0, retrieve and dump functions, 1, procedures */
  for (i = 0; i <= 1; i++) {
    snprintf(query_buff, sizeof(query_buff), "SHOW %s STATUS WHERE Db = '%s'",
             routine_type[i], db_name_buff);

    if (mysql_query_with_error_report(mysql, &routine_list_res, query_buff))
      return 1;

    if (mysql_num_rows(routine_list_res)) {
      while ((routine_list_row = mysql_fetch_row(routine_list_res))) {
        mysql_real_escape_string_quote(
            mysql, name_buff, routine_list_row[1],
            static_cast<ulong>(strlen(routine_list_row[1])), '\'');

        if (has_missing_import(db_name_buff, name_buff)) {
          // Any of the imported libraries does NOT exist.
          print_comment(
              sql_file, true,
              "\n-- One or more of the libraries used by %s.%s routine, do "
              "not exist. \n",
              db_name_buff, name_buff);
          maybe_die(
              EX_MYSQLERR,
              "Routine %s.%s is missing one or more of its imported libraries.",
              db_name_buff, name_buff);
          return 1;
        }

        routine_name = quote_name(routine_list_row[1], name_buff, false);
        DBUG_PRINT("info",
                   ("retrieving CREATE %s for %s", routine_type[i], name_buff));
        snprintf(query_buff, sizeof(query_buff), "SHOW CREATE %s %s",
                 routine_type[i], routine_name);

        if (mysql_query_with_error_report(mysql, &routine_res, query_buff))
          return 1;

        while ((row = mysql_fetch_row(routine_res))) {
          /*
            if the user has EXECUTE privilege he see routine names, but NOT the
            routine body of other routines that are not the creator of!
          */
          DBUG_PRINT("info",
                     ("length of body for %s row[2] '%s' is %zu", routine_name,
                      row[2] ? row[2] : "(null)", row[2] ? strlen(row[2]) : 0));
          if (row[2] == nullptr) {
            print_comment(sql_file, true,
                          "\n-- insufficient privileges to %s\n", query_buff);

            bool freemem = false;
            char const *text =
                fix_identifier_with_newline(current_user, &freemem);
            print_comment(sql_file, true,
                          "-- does %s have permissions on mysql.proc?\n\n",
                          text);
            if (freemem) my_free(const_cast<char *>(text));

            maybe_die(EX_MYSQLERR, "%s has insufficient privileges to %s!",
                      current_user, query_buff);
          } else if (strlen(row[2])) {
            if (opt_xml) {
              if (i)  // Procedures.
                print_xml_row(sql_file, "routine", routine_res, &row,
                              "Create Procedure");
              else  // Functions.
                print_xml_row(sql_file, "routine", routine_res, &row,
                              "Create Function");
              continue;
            }
            if (opt_drop)
              fprintf(sql_file, "/*!50003 DROP %s IF EXISTS %s */;\n",
                      routine_type[i], routine_name);

            if (mysql_num_fields(routine_res) >= 6) {
              if (switch_db_collation(sql_file, db_name_buff, ";", db_cl_name,
                                      row[5], &db_cl_altered)) {
                return 1;
              }

              switch_cs_variables(sql_file, ";",
                                  row[3],  /* character_set_client */
                                  row[3],  /* character_set_results */
                                  row[4]); /* collation_connection */
            } else {
              /*
                mysqldump is being run against the server, that does not
                provide character set information in SHOW CREATE
                statements.

                NOTE: the dump may be incorrect, since character set
                information is required in order to restore stored
                procedure/function properly.
              */

              fprintf(sql_file,
                      "--\n"
                      "-- WARNING: old server version. "
                      "The following dump may be incomplete.\n"
                      "--\n");
            }

            switch_sql_mode(sql_file, ";", row[1]);

            fprintf(sql_file,
                    "DELIMITER ;;\n"
                    "%s ;;\n"
                    "DELIMITER ;\n",
                    (const char *)row[2]);

            restore_sql_mode(sql_file, ";");

            if (mysql_num_fields(routine_res) >= 6) {
              restore_cs_variables(sql_file, ";");

              if (db_cl_altered) {
                if (restore_db_collation(sql_file, db_name_buff, ";",
                                         db_cl_name))
                  return 1;
              }
            }
          }
        } /* end of routine printing */
        mysql_free_result(routine_res);

      } /* end of list of routines */
    }
    mysql_free_result(routine_list_res);
  } /* end of for i (0 .. 1)  */

  if (opt_xml) {
    fputs("\t</routines>\n", sql_file);
    check_io(sql_file);
  }

  if (switch_character_set_results(mysql, default_charset)) return 1;

  if (lock_tables)
    (void)mysql_query_with_error_report(mysql, nullptr, "UNLOCK TABLES");
  return 0;
}

/* general_log or slow_log tables under mysql database */
static inline bool general_log_or_slow_log_tables(const char *db,
                                                  const char *table) {
  return (!my_strcasecmp(charset_info, db, "mysql")) &&
         (!my_strcasecmp(charset_info, table, "general_log") ||
          !my_strcasecmp(charset_info, table, "slow_log"));
}

/*
 slave_master_info,slave_relay_log_info and gtid_executed tables under
 mysql database
*/
static inline bool replication_metadata_tables(const char *db,
                                               const char *table) {
  return (!my_strcasecmp(charset_info, db, "mysql")) &&
         (!my_strcasecmp(charset_info, table, "slave_master_info") ||
          !my_strcasecmp(charset_info, table, "slave_relay_log_info") ||
          !my_strcasecmp(charset_info, table, "gtid_executed"));
}

/**
  Check if the table is innodb stats table in mysql database.

  @param [in] db           Database name
  @param [in] table        Table name

  @retval true if it is innodb stats table else false
*/
static inline bool innodb_stats_tables(const char *db, const char *table) {
  return (!my_strcasecmp(charset_info, db, "mysql")) &&
         (!my_strcasecmp(charset_info, table, "innodb_table_stats") ||
          !my_strcasecmp(charset_info, table, "innodb_index_stats") ||
          !my_strcasecmp(charset_info, table, "innodb_dynamic_metadata") ||
          !my_strcasecmp(charset_info, table, "innodb_ddl_log"));
}

/**
  Check if the command line option includes innodb stats table
  or in any way mysql database.

  @param [in] argc         Total count of positional arguments
  @param [in] argv         Pointer to positional arguments

  @retval true if dump contains innodb stats table or else false
*/
static inline bool is_innodb_stats_tables_included(int argc, char **argv) {
  if (opt_alldbs) return true;
  if (argc > 0) {
    char **names = argv;
    if (opt_databases) {
      for (char **obj = names; *obj; obj++)
        if (!my_strcasecmp(charset_info, *obj, "mysql")) return true;
    } else {
      char **obj = names;
      if (!my_strcasecmp(charset_info, *obj, "mysql")) {
        for (obj++; *obj; obj++)
          if (!my_strcasecmp(charset_info, *obj, "innodb_table_stats") ||
              !my_strcasecmp(charset_info, *obj, "innodb_index_stats"))
            return true;
      }
    }
  }
  return false;
}

/*
  get_table_structure -- retrieves database structure, prints out corresponding
  CREATE statement and fills out insert_pat if the table is the type we will
  be dumping.

  ARGS
    table        - table name
    db           - db name
    table_type   - table type, e.g. "MyISAM" or "InnoDB", but also "VIEW"
    ignore_flag  - what we must particularly ignore - see IGNORE_ defines above
    real_columns - Contains one byte per column, 0 means unused, 1 is used
                   Generated columns are marked as unused
    column_list  - Contains column list when table has invisible columns.

  RETURN
    number of fields in table, 0 if error
*/

static uint get_table_structure(const char *table, char *db, char *table_type,
                                char *ignore_flag, bool real_columns[],
                                std::string *column_list) {
  bool init = false, write_data, complete_insert, skip_ddl;
  uint64_t num_fields;
  const char *result_table, *opt_quoted_table;
  const char *insert_option;
  char name_buff[NAME_LEN + 3], table_buff[NAME_LEN * 2 + 3];
  char table_buff2[NAME_LEN * 2 + 3], query_buff[QUERY_LENGTH];
  const char *show_fields_stmt =
      "SELECT `COLUMN_NAME` AS `Field`, "
      "`COLUMN_TYPE` AS `Type`, "
      "`IS_NULLABLE` AS `Null`, "
      "`COLUMN_KEY` AS `Key`, "
      "`COLUMN_DEFAULT` AS `Default`, "
      "`EXTRA` AS `Extra`, "
      "`COLUMN_COMMENT` AS `Comment` "
      "FROM `INFORMATION_SCHEMA`.`COLUMNS` WHERE "
      "TABLE_SCHEMA = '%s' AND TABLE_NAME = '%s' "
      "ORDER BY ORDINAL_POSITION";
  FILE *sql_file = md_result_file;
  bool is_log_table;
  bool is_replication_metadata_table;
  bool is_view(false);
  unsigned int colno;
  MYSQL_RES *result;
  MYSQL_ROW row;
  DBUG_TRACE;
  DBUG_PRINT("enter", ("db: %s  table: %s", db, table));

  *ignore_flag = check_if_ignore_table(table, table_type);

  is_view = strcmp(table_type, "VIEW") == 0;

  if (opt_ignore_views && is_view) {
    DBUG_PRINT("exit", ("Dumping view ignored."));
    return 0;
  }

  /*
    for mysql.innodb_table_stats, mysql.innodb_index_stats tables we
    dont dump DDL
  */
  skip_ddl = innodb_stats_tables(db, table);

  complete_insert = false;
  if ((write_data = !(*ignore_flag & IGNORE_DATA))) {
    complete_insert = opt_complete_insert;
    if (!insert_pat_inited) {
      insert_pat_inited = true;
      init_dynamic_string_checked(&insert_pat, "", 1024);
    } else
      dynstr_set_checked(&insert_pat, "");
  }

  insert_option = ((opt_ignore || skip_ddl) ? "IGNORE " : "");

  verbose_msg("-- Retrieving table structure for table %s...\n", table);

  snprintf(query_buff, sizeof(query_buff), "SET SQL_QUOTE_SHOW_CREATE=%d",
           (opt_quoted || opt_keywords));

  result_table = quote_name(table, table_buff, true);
  opt_quoted_table = quote_name(table, table_buff2, false);

  if (!opt_xml && !mysql_query_with_error_report(mysql, nullptr, query_buff)) {
    /* using SHOW CREATE statement */
    if (!opt_no_create_info && !skip_ddl) {
      /* Make an sql-file, if path was given iow. option -T was given */
      char buff[20 + FN_REFLEN];
      MYSQL_FIELD *field;

      snprintf(buff, sizeof(buff), "show create table %s", result_table);

      if (switch_character_set_results(mysql, "binary") ||
          mysql_query_with_error_report(mysql, &result, buff) ||
          switch_character_set_results(mysql, default_charset))
        return 0;

      if (path) {
        if (!(sql_file = open_sql_file_for_table(table, O_WRONLY))) return 0;

        write_header(sql_file, db);
      }

      bool freemem = false;
      char const *text = fix_identifier_with_newline(result_table, &freemem);
      if (is_view) /* view */
        print_comment(sql_file, false,
                      "\n--\n-- Temporary view structure for view %s\n--\n\n",
                      text);
      else
        print_comment(sql_file, false,
                      "\n--\n-- Table structure for table %s\n--\n\n", text);
      if (freemem) my_free(const_cast<char *>(text));

      if (opt_drop) {
        /*
          Even if the "table" is a view, we do a DROP TABLE here.  The
          view-specific code below fills in the DROP VIEW.
          We will skip the DROP TABLE for general_log and slow_log, since
          those stmts will fail, in case we apply dump by enabling logging.
          We will skip this for replication metadata tables as well.
         */
        if (!(general_log_or_slow_log_tables(db, table) ||
              replication_metadata_tables(db, table)))
          fprintf(sql_file, "DROP TABLE IF EXISTS %s;\n", opt_quoted_table);
        check_io(sql_file);
      }

      field = mysql_fetch_field_direct(result, 0);
      if (strcmp(field->name, "View") == 0) {
        char *scv_buff = nullptr;
        uint64_t n_cols;

        verbose_msg("-- It's a view, create dummy view\n");

        /* save "show create" statement for later */
        if ((row = mysql_fetch_row(result)) && (scv_buff = row[1]))
          scv_buff = my_strdup(PSI_NOT_INSTRUMENTED, scv_buff, MYF(0));

        mysql_free_result(result);

        /*
          Create a table with the same name as the view and with columns of
          the same name in order to satisfy views that depend on this view.
          The table will be removed when the actual view is created.

          The properties of each column, are not preserved in this temporary
          table, because they are not necessary.

          This will not be necessary once we can determine dependencies
          between views and can simply dump them in the appropriate order.
        */
        snprintf(query_buff, sizeof(query_buff), "SHOW FIELDS FROM %s",
                 result_table);
        if (switch_character_set_results(mysql, "binary") ||
            mysql_query_with_error_report(mysql, &result, query_buff) ||
            switch_character_set_results(mysql, default_charset)) {
          /*
            View references invalid or privileged table/col/fun (err 1356),
            so we cannot create a stand-in table.  Be defensive and dump
            a comment with the view's 'show create' statement. (Bug #17371)
          */

          if (mysql_errno(mysql) == ER_VIEW_INVALID)
            fprintf(sql_file, "\n-- failed on view %s: %s\n\n", result_table,
                    scv_buff ? scv_buff : "");

          my_free(scv_buff);

          return 0;
        }
        my_free(scv_buff);

        n_cols = mysql_num_rows(result);
        if (0 != n_cols) {
          /*
            The actual formula is based on the column names and how the .FRM
            files are stored and is too volatile to be repeated here.
            Thus we simply warn the user if the columns exceed a limit we
            know works most of the time.
          */
          if (n_cols >= 1000)
            fprintf(stderr,
                    "-- Warning: Creating a stand-in table for view %s may"
                    " fail when replaying the dump file produced because "
                    "of the number of columns exceeding 1000. Exercise "
                    "caution when replaying the produced dump file.\n",
                    table);
          if (opt_drop) {
            /*
              We have already dropped any table of the same name above, so
              here we just drop the view.
            */

            fprintf(sql_file, "/*!50001 DROP VIEW IF EXISTS %s*/;\n",
                    opt_quoted_table);
            check_io(sql_file);
          }

          fprintf(sql_file,
                  "SET @saved_cs_client     = @@character_set_client;\n"
                  "/*!50503 SET character_set_client = utf8mb4 */;\n"
                  "/*!50001 CREATE VIEW %s AS SELECT \n",
                  result_table);

          /*
            Get first row, following loop will prepend comma - keeps from
            having to know if the row being printed is last to determine if
            there should be a _trailing_ comma.
          */

          row = mysql_fetch_row(result);

          /*
            A temporary view is created to resolve the view interdependencies.
            This temporary view is dropped when the actual view is created.
          */

          fprintf(sql_file, " 1 AS %s", quote_name(row[0], name_buff, false));

          while ((row = mysql_fetch_row(result))) {
            fprintf(sql_file, ",\n 1 AS %s",
                    quote_name(row[0], name_buff, false));
          }

          fprintf(sql_file,
                  "*/;\n"
                  "SET character_set_client = @saved_cs_client;\n");

          check_io(sql_file);
        }

        mysql_free_result(result);

        if (path) my_fclose(sql_file, MYF(MY_WME));

        seen_views = true;
        return 0;
      }

      row = mysql_fetch_row(result);

      is_log_table = general_log_or_slow_log_tables(db, table);
      is_replication_metadata_table = replication_metadata_tables(db, table);
      if (is_log_table || is_replication_metadata_table)
        row[1] += 13; /* strlen("CREATE TABLE ")= 13 */

      fprintf(sql_file,
              "/*!40101 SET @saved_cs_client     = @@character_set_client */;\n"
              "/*!50503 SET character_set_client = utf8mb4 */;\n"
              "%s%s;\n"
              "/*!40101 SET character_set_client = @saved_cs_client */;\n",
              (is_log_table || is_replication_metadata_table)
                  ? "CREATE TABLE IF NOT EXISTS "
                  : "",
              row[1]);

      check_io(sql_file);
      mysql_free_result(result);
    }
    snprintf(query_buff, sizeof(query_buff), "show fields from %s",
             result_table);
    if (mysql_query_with_error_report(mysql, &result, query_buff)) {
      if (path) my_fclose(sql_file, MYF(MY_WME));
      return 0;
    }

    bool has_invisible_columns = false;
    if (write_data) {
      while ((row = mysql_fetch_row(result))) {
        if (row[SHOW_EXTRA]) {
          /*
            If data contents of table are to be written and option to prepare
            INSERT statement with complete column list is not set then scan the
            column list for generated columns and invisible columns. Presence
            of any generated column or invisible column will require that an
            explicit list of columns is printed for INSERT statements.
          */
          bool is_generated_column = false;
          if (strcmp(row[SHOW_EXTRA], "STORED GENERATED") == 0) {
            is_generated_column = true;
          } else if (strcmp(row[SHOW_EXTRA], "STORED GENERATED INVISIBLE") ==
                     0) {
            is_generated_column = true;
            has_invisible_columns |= true;
          } else if (strcmp(row[SHOW_EXTRA], "VIRTUAL GENERATED") == 0) {
            is_generated_column = true;
          } else if (strcmp(row[SHOW_EXTRA], "VIRTUAL GENERATED INVISIBLE") ==
                     0) {
            is_generated_column = true;
            has_invisible_columns |= true;
          } else if (!has_invisible_columns &&
                     (strstr(row[SHOW_EXTRA], "INVISIBLE") != nullptr)) {
            /*
              For timestamp and datetime type columns, EXTRA column might
              contain DEFAULT_GENERATED and 'on update CURRENT TIMESTAMP'.
              INVISIBLE keyword is appended at the end if column is invisible.
              So finding INVISIBLE keyword in EXTRA column to check column is
              invisible.
            */
            has_invisible_columns = true;
          }

          complete_insert |= (has_invisible_columns || is_generated_column);
        }
      }
      mysql_free_result(result);

      if (mysql_query_with_error_report(mysql, &result, query_buff)) {
        if (path) my_fclose(sql_file, MYF(MY_WME));
        return 0;
      }
    }
    /*
      If write_data is true, then we build up insert statements for
      the table's data. Note: in subsequent lines of code, this test
      will have to be performed each time we are appending to
      insert_pat.
    */
    if (write_data) {
      if (opt_replace_into)
        dynstr_append_checked(&insert_pat, "REPLACE ");
      else
        dynstr_append_checked(&insert_pat, "INSERT ");
      dynstr_append_checked(&insert_pat, insert_option);
      dynstr_append_checked(&insert_pat, "INTO ");
      dynstr_append_checked(&insert_pat, opt_quoted_table);
      if (complete_insert) {
        dynstr_append_checked(&insert_pat, " (");
      } else {
        dynstr_append_checked(&insert_pat, " VALUES ");
        if (!extended_insert) dynstr_append_checked(&insert_pat, "(");
      }
    }

    colno = 0;
    while ((row = mysql_fetch_row(result))) {
      if (row[SHOW_EXTRA]) {
        real_columns[colno] =
            (strcmp(row[SHOW_EXTRA], "STORED GENERATED") != 0 &&
             strcmp(row[SHOW_EXTRA], "STORED GENERATED INVISIBLE") != 0 &&
             strcmp(row[SHOW_EXTRA], "VIRTUAL GENERATED") != 0 &&
             strcmp(row[SHOW_EXTRA], "VIRTUAL GENERATED INVISIBLE") != 0);
      } else
        real_columns[colno] = true;

      if (has_invisible_columns && column_list != nullptr) {
        if (!column_list->empty()) column_list->append(", ");
        column_list->append(quote_name(row[SHOW_FIELDNAME], name_buff, false));
      }

      if (real_columns[colno++] && complete_insert) {
        if (init) dynstr_append_checked(&insert_pat, ", ");
        init = true;
        dynstr_append_checked(
            &insert_pat, quote_name(row[SHOW_FIELDNAME], name_buff, false));
      }
    }
    num_fields = mysql_num_rows(result);
    mysql_free_result(result);
  } else {
    verbose_msg("%s: Warning: Can't set SQL_QUOTE_SHOW_CREATE option (%s)\n",
                my_progname, mysql_error(mysql));

    snprintf(query_buff, sizeof(query_buff), show_fields_stmt, db, table);

    if (mysql_query_with_error_report(mysql, &result, query_buff)) return 0;

    bool has_invisible_columns = false;
    if (write_data) {
      while ((row = mysql_fetch_row(result))) {
        if (row[SHOW_EXTRA]) {
          /*
            If data contents of table are to be written and option to prepare
            INSERT statement with complete column list is not set then scan the
            column list for generated columns and invisible columns. Presence
            of any generated column or invisible column will require that an
            explicit list of columns is printed for INSERT statements.
          */
          bool is_generated_column = false;
          if (strcmp(row[SHOW_EXTRA], "STORED GENERATED") == 0) {
            is_generated_column = true;
          } else if (strcmp(row[SHOW_EXTRA], "STORED GENERATED INVISIBLE") ==
                     0) {
            is_generated_column = true;
            has_invisible_columns |= true;
          } else if (strcmp(row[SHOW_EXTRA], "VIRTUAL GENERATED") == 0) {
            is_generated_column = true;
          } else if (strcmp(row[SHOW_EXTRA], "VIRTUAL GENERATED INVISIBLE") ==
                     0) {
            is_generated_column = true;
            has_invisible_columns |= true;
          } else if (!has_invisible_columns &&
                     (strstr(row[SHOW_EXTRA], "INVISIBLE") != nullptr)) {
            /*
              For timestamp and datetime type columns, EXTRA column might
              contain DEFAULT_GENERATED and 'on update CURRENT TIMESTAMP'.
              INVISIBLE keyword is appended at the end if column is invisible.
              So finding INVISIBLE keyword in EXTRA column to check column is
              invisible.
            */
            has_invisible_columns = true;
          }

          complete_insert |= (has_invisible_columns || is_generated_column);
        }
      }
      mysql_free_result(result);

      if (mysql_query_with_error_report(mysql, &result, query_buff)) {
        if (path) my_fclose(sql_file, MYF(MY_WME));
        return 0;
      }
    }
    /* Make an sql-file, if path was given iow. option -T was given */
    if (!opt_no_create_info) {
      if (path) {
        if (!(sql_file = open_sql_file_for_table(table, O_WRONLY))) return 0;
        write_header(sql_file, db);
      }

      bool freemem = false;
      char const *text = fix_identifier_with_newline(result_table, &freemem);
      print_comment(sql_file, false,
                    "\n--\n-- Table structure for table %s\n--\n\n", text);
      if (freemem) my_free(const_cast<char *>(text));

      if (opt_drop)
        fprintf(sql_file, "DROP TABLE IF EXISTS %s;\n", result_table);
      if (!opt_xml)
        fprintf(sql_file, "CREATE TABLE %s (\n", result_table);
      else
        print_xml_tag(sql_file, "\t", "\n", "table_structure", "name=", table,
                      NullS);
      check_io(sql_file);
    }

    if (write_data) {
      if (opt_replace_into)
        dynstr_append_checked(&insert_pat, "REPLACE ");
      else
        dynstr_append_checked(&insert_pat, "INSERT ");
      dynstr_append_checked(&insert_pat, insert_option);
      dynstr_append_checked(&insert_pat, "INTO ");
      dynstr_append_checked(&insert_pat, result_table);
      if (complete_insert)
        dynstr_append_checked(&insert_pat, " (");
      else {
        dynstr_append_checked(&insert_pat, " VALUES ");
        if (!extended_insert) dynstr_append_checked(&insert_pat, "(");
      }
    }

    colno = 0;
    while ((row = mysql_fetch_row(result))) {
      ulong *lengths = mysql_fetch_lengths(result);

      if (row[SHOW_EXTRA]) {
        real_columns[colno] =
            (strcmp(row[SHOW_EXTRA], "STORED GENERATED") != 0 &&
             strcmp(row[SHOW_EXTRA], "STORED GENERATED INVISIBLE") != 0 &&
             strcmp(row[SHOW_EXTRA], "VIRTUAL GENERATED") != 0 &&
             strcmp(row[SHOW_EXTRA], "VIRTUAL GENERATED INVISIBLE") != 0);
      } else
        real_columns[colno] = true;

      if (has_invisible_columns && column_list != nullptr) {
        if (!column_list->empty()) column_list->append(", ");
        column_list->append(quote_name(row[SHOW_FIELDNAME], name_buff, false));
      }

      if (!real_columns[colno++]) continue;

      if (init) {
        if (!opt_xml && !opt_no_create_info) {
          fputs(",\n", sql_file);
          check_io(sql_file);
        }
        if (complete_insert) dynstr_append_checked(&insert_pat, ", ");
      }
      init = true;
      if (complete_insert)
        dynstr_append_checked(
            &insert_pat, quote_name(row[SHOW_FIELDNAME], name_buff, false));
      if (!opt_no_create_info) {
        if (opt_xml) {
          print_xml_row(sql_file, "field", result, &row, NullS);
          continue;
        }

        if (opt_keywords)
          fprintf(sql_file, "  %s.%s %s", result_table,
                  quote_name(row[SHOW_FIELDNAME], name_buff, false),
                  row[SHOW_TYPE]);
        else
          fprintf(sql_file, "  %s %s",
                  quote_name(row[SHOW_FIELDNAME], name_buff, false),
                  row[SHOW_TYPE]);
        if (row[SHOW_DEFAULT]) {
          fputs(" DEFAULT ", sql_file);
          unescape(sql_file, row[SHOW_DEFAULT], lengths[SHOW_DEFAULT]);
        }
        if (!row[SHOW_NULL][0]) fputs(" NOT NULL", sql_file);
        if (row[SHOW_EXTRA] && row[SHOW_EXTRA][0])
          fprintf(sql_file, " %s", row[SHOW_EXTRA]);
        check_io(sql_file);
      }
    }
    num_fields = mysql_num_rows(result);
    mysql_free_result(result);
    if (!opt_no_create_info) {
      /* Make an sql-file, if path was given iow. option -T was given */
      char buff[20 + FN_REFLEN];
      uint keynr, primary_key;
      snprintf(buff, sizeof(buff), "show keys from %s", result_table);
      if (mysql_query_with_error_report(mysql, &result, buff)) {
        if (mysql_errno(mysql) == ER_WRONG_OBJECT) {
          /* it is VIEW */
          fputs("\t\t<options Comment=\"view\" />\n", sql_file);
          goto continue_xml;
        }
        fprintf(stderr, "%s: Can't get keys for table %s (%s)\n", my_progname,
                result_table, mysql_error(mysql));
        if (path) my_fclose(sql_file, MYF(MY_WME));
        return 0;
      }

      /* Find first which key is primary key */
      keynr = 0;
      primary_key = INT_MAX;
      while ((row = mysql_fetch_row(result))) {
        if (atoi(row[3]) == 1) {
          keynr++;
          if (!strcmp(row[2], "PRIMARY")) {
            primary_key = keynr;
            break;
          }
        }
      }
      mysql_data_seek(result, 0);
      keynr = 0;
      while ((row = mysql_fetch_row(result))) {
        if (opt_xml) {
          print_xml_row(sql_file, "key", result, &row, NullS);
          continue;
        }

        if (atoi(row[3]) == 1) {
          if (keynr++) putc(')', sql_file);
          if (atoi(row[1])) /* Test if duplicate key */
            /* Duplicate allowed */
            fprintf(sql_file, ",\n  KEY %s (",
                    quote_name(row[2], name_buff, false));
          else if (keynr == primary_key)
            fputs(",\n  PRIMARY KEY (", sql_file); /* First UNIQUE is primary */
          else
            fprintf(sql_file, ",\n  UNIQUE %s (",
                    quote_name(row[2], name_buff, false));
        } else
          putc(',', sql_file);
        fputs(quote_name(row[4], name_buff, false), sql_file);
        if (row[7]) fprintf(sql_file, " (%s)", row[7]); /* Sub key */
        check_io(sql_file);
      }
      mysql_free_result(result);
      if (!opt_xml) {
        if (keynr) putc(')', sql_file);
        fputs("\n)", sql_file);
        check_io(sql_file);
      }

      /* Get MySQL specific create options */
      if (create_options) {
        char show_name_buff[NAME_LEN * 2 + 2 + 24];

        /* Check memory for quote_for_like() */
        snprintf(buff, sizeof(buff), "show table status like %s",
                 quote_for_like(table, show_name_buff));

        if (mysql_query_with_error_report(mysql, &result, buff)) {
          if (mysql_errno(mysql) != ER_PARSE_ERROR) { /* If old MySQL version */
            verbose_msg(
                "-- Warning: Couldn't get status information for "
                "table %s (%s)\n",
                result_table, mysql_error(mysql));
          }
        } else if (!(row = mysql_fetch_row(result))) {
          fprintf(stderr,
                  "Error: Couldn't read status information for table %s (%s)\n",
                  result_table, mysql_error(mysql));
        } else {
          if (opt_xml)
            print_xml_row(sql_file, "options", result, &row, NullS);
          else {
            fputs("/*!", sql_file);
            print_value(sql_file, result, row, "engine=", "Engine", 0);
            print_value(sql_file, result, row, "", "Create_options", 0);
            print_value(sql_file, result, row, "comment=", "Comment", 1);
            fputs(" */", sql_file);
            check_io(sql_file);
          }
        }
        mysql_free_result(result); /* Is always safe to free */
      }
    continue_xml:
      if (!opt_xml)
        fputs(";\n", sql_file);
      else
        fputs("\t</table_structure>\n", sql_file);
      check_io(sql_file);
    }
  }
  if (complete_insert) {
    dynstr_append_checked(&insert_pat, ") VALUES ");
    if (!extended_insert) dynstr_append_checked(&insert_pat, "(");
  }
  if (sql_file != md_result_file) {
    fputs("\n", sql_file);
    write_footer(sql_file);
    my_fclose(sql_file, MYF(MY_WME));
  }
  return (uint)num_fields;
} /* get_table_structure */

static void dump_trigger_old(FILE *sql_file, MYSQL_RES *show_triggers_rs,
                             MYSQL_ROW *show_trigger_row,
                             const char *table_name) {
  char quoted_table_name_buf[NAME_LEN * 2 + 3];
  const char *quoted_table_name =
      quote_name(table_name, quoted_table_name_buf, true);

  char name_buff[NAME_LEN * 4 + 3];
  const char *xml_msg =
      "\nWarning! mysqldump being run against old server "
      "that does not\nsupport 'SHOW CREATE TRIGGERS' "
      "statement. Skipping..\n";

  DBUG_TRACE;

  if (opt_xml) {
    print_xml_comment(sql_file, strlen(xml_msg), xml_msg);
    check_io(sql_file);
    return;
  }

  fprintf(sql_file,
          "--\n"
          "-- WARNING: old server version. "
          "The following dump may be incomplete.\n"
          "--\n");

  if (opt_compact)
    fprintf(sql_file, "/*!50003 SET @OLD_SQL_MODE=@@SQL_MODE*/;\n");

  if (opt_drop_trigger)
    fprintf(sql_file, "/*!50032 DROP TRIGGER IF EXISTS %s */;\n",
            (*show_trigger_row)[0]);

  fprintf(sql_file,
          "DELIMITER ;;\n"
          "/*!50003 SET SESSION SQL_MODE=\"%s\" */;;\n"
          "/*!50003 CREATE */ ",
          (*show_trigger_row)[6]);

  if (mysql_num_fields(show_triggers_rs) > 7) {
    /*
      mysqldump can be run against the server, that does not support
      definer in triggers (there is no DEFINER column in SHOW TRIGGERS
      output). So, we should check if we have this column before
      accessing it.
    */

    size_t user_name_len;
    char user_name_str[USERNAME_LENGTH + 1];
    char quoted_user_name_str[USERNAME_LENGTH * 2 + 3];
    size_t host_name_len;
    char host_name_str[HOSTNAME_LENGTH + 1];
    char quoted_host_name_str[HOSTNAME_LENGTH * 2 + 3];

    parse_user((*show_trigger_row)[7], strlen((*show_trigger_row)[7]),
               user_name_str, &user_name_len, host_name_str, &host_name_len);

    fprintf(sql_file, "/*!50017 DEFINER=%s@%s */ ",
            quote_name(user_name_str, quoted_user_name_str, false),
            quote_name(host_name_str, quoted_host_name_str, false));
  }

  fprintf(sql_file,
          "/*!50003 TRIGGER %s %s %s ON %s FOR EACH ROW%s%s */;;\n"
          "DELIMITER ;\n",
          quote_name((*show_trigger_row)[0], name_buff, false), /* Trigger */
          (*show_trigger_row)[4],                               /* Timing */
          (*show_trigger_row)[1],                               /* Event */
          quoted_table_name,
          (strchr(" \t\n\r", *((*show_trigger_row)[3]))) ? "" : " ",
          (*show_trigger_row)[3] /* Statement */);

  if (opt_compact)
    fprintf(sql_file, "/*!50003 SET SESSION SQL_MODE=@OLD_SQL_MODE */;\n");
}

static int dump_trigger(FILE *sql_file, MYSQL_RES *show_create_trigger_rs,
                        const char *db_name, const char *db_cl_name) {
  MYSQL_ROW row;
  char *query_str;
  int db_cl_altered = false;

  DBUG_TRACE;

  while ((row = mysql_fetch_row(show_create_trigger_rs))) {
    if (opt_xml) {
      print_xml_row(sql_file, "trigger", show_create_trigger_rs, &row,
                    "SQL Original Statement");
      check_io(sql_file);
      continue;
    }

    query_str = cover_definer_clause(
        row[2], strlen(row[2]), STRING_WITH_LEN("50017"),
        STRING_WITH_LEN("50003"), STRING_WITH_LEN(" TRIGGER"));
    if (switch_db_collation(sql_file, db_name, ";", db_cl_name, row[5],
                            &db_cl_altered))
      return true;

    switch_cs_variables(sql_file, ";", row[3], /* character_set_client */
                        row[3],                /* character_set_results */
                        row[4]);               /* collation_connection */

    switch_sql_mode(sql_file, ";", row[1]);

    if (opt_drop_trigger)
      fprintf(sql_file, "/*!50032 DROP TRIGGER IF EXISTS %s */;\n", row[0]);

    fprintf(sql_file,
            "DELIMITER ;;\n"
            "/*!50003 %s */;;\n"
            "DELIMITER ;\n",
            (const char *)(query_str != nullptr ? query_str : row[2]));

    restore_sql_mode(sql_file, ";");
    restore_cs_variables(sql_file, ";");

    if (db_cl_altered) {
      if (restore_db_collation(sql_file, db_name, ";", db_cl_name)) return true;
    }

    my_free(query_str);
  }

  return false;
}

/**
  Dump the triggers for a given table.

  This should be called after the tables have been dumped in case a trigger
  depends on the existence of a table.

  @param[in] table_name table name
  @param[in] db_name db name

  @return Error status.
    @retval true error has occurred.
    @retval false operation succeed.
*/

static int dump_triggers_for_table(char *table_name, char *db_name) {
  char name_buff[NAME_LEN * 4 + 3];
  char query_buff[QUERY_LENGTH];
  const bool old_ansi_quotes_mode = ansi_quotes_mode;
  MYSQL_RES *show_triggers_rs;
  MYSQL_ROW row;
  FILE *sql_file = md_result_file;

  char db_cl_name[MY_CS_NAME_SIZE];
  int ret = true;

  DBUG_TRACE;
  DBUG_PRINT("enter", ("db: %s, table_name: %s", db_name, table_name));

  if (path &&
      !(sql_file = open_sql_file_for_table(table_name, O_WRONLY | O_APPEND)))
    return 1;

  /* Do not use ANSI_QUOTES on triggers in dump */
  ansi_quotes_mode = false;

  /* Get database collation. */

  if (switch_character_set_results(mysql, "binary")) goto done;

  if (fetch_db_collation(db_name, db_cl_name, sizeof(db_cl_name))) goto done;

  /* Get list of triggers. */

  snprintf(query_buff, sizeof(query_buff), "SHOW TRIGGERS LIKE %s",
           quote_for_like(table_name, name_buff));

  if (mysql_query_with_error_report(mysql, &show_triggers_rs, query_buff))
    goto done;

  /* Dump triggers. */

  if (!mysql_num_rows(show_triggers_rs)) goto skip;

  if (opt_xml)
    print_xml_tag(sql_file, "\t", "\n", "triggers", "name=", table_name, NullS);

  while ((row = mysql_fetch_row(show_triggers_rs))) {
    snprintf(query_buff, sizeof(query_buff), "SHOW CREATE TRIGGER %s",
             quote_name(row[0], name_buff, true));

    if (mysql_query(mysql, query_buff)) {
      /*
        mysqldump is being run against old server, that does not support
        SHOW CREATE TRIGGER statement. We should use SHOW TRIGGERS output.

        NOTE: the dump may be incorrect, as old SHOW TRIGGERS does not
        provide all the necessary information to restore trigger properly.
      */

      dump_trigger_old(sql_file, show_triggers_rs, &row, table_name);
    } else {
      MYSQL_RES *show_create_trigger_rs = mysql_store_result(mysql);

      if (!show_create_trigger_rs ||
          dump_trigger(sql_file, show_create_trigger_rs, db_name, db_cl_name))
        goto done;

      mysql_free_result(show_create_trigger_rs);
    }
  }

  if (opt_xml) {
    fputs("\t</triggers>\n", sql_file);
    check_io(sql_file);
  }

skip:
  mysql_free_result(show_triggers_rs);

  if (switch_character_set_results(mysql, default_charset)) goto done;

  /*
    make sure to set back ansi_quotes_mode mode to
    original value
  */
  ansi_quotes_mode = old_ansi_quotes_mode;

  ret = false;

done:
  if (path) my_fclose(sql_file, MYF(0));

  return ret;
}

static bool dump_column_statistics_for_table(char *table_name, char *db_name) {
  char name_buff[NAME_LEN * 4 + 3];
  char column_buffer[NAME_LEN * 4 + 3];
  char query_buff[QUERY_LENGTH * 3 / 2];
  const bool old_ansi_quotes_mode = ansi_quotes_mode;
  char *quoted_table;
  MYSQL_RES *column_statistics_rs;
  MYSQL_ROW row;
  FILE *sql_file = md_result_file;

  bool ret = true;

  DBUG_TRACE;
  DBUG_PRINT("enter", ("db: %s, table_name: %s", db_name, table_name));

  if (path &&
      !(sql_file = open_sql_file_for_table(table_name, O_WRONLY | O_APPEND)))
    return true; /* purecov: deadcode */

  if (switch_character_set_results(mysql, "binary"))
    goto done; /* purecov: deadcode */

  char escaped_db[NAME_LEN * 4 + 3];
  char escaped_table[NAME_LEN * 4 + 3];
  mysql_real_escape_string_quote(mysql, escaped_table, table_name,
                                 static_cast<ulong>(strlen(table_name)), '\'');
  mysql_real_escape_string_quote(mysql, escaped_db, db_name,
                                 static_cast<ulong>(strlen(db_name)), '\'');

  /* Get list of columns with statistics. */
  snprintf(query_buff, sizeof(query_buff),
           "SELECT COLUMN_NAME, \
                      JSON_EXTRACT(HISTOGRAM, '$.\"number-of-buckets-specified\"') \
               FROM information_schema.COLUMN_STATISTICS \
               WHERE SCHEMA_NAME = '%s' AND TABLE_NAME = '%s';",
           escaped_db, escaped_table);

  if (mysql_query_with_error_report(mysql, &column_statistics_rs, query_buff))
    goto done; /* purecov: deadcode */

  /* Dump column statistics. */
  if (!mysql_num_rows(column_statistics_rs)) goto skip;

  if (opt_xml)
    print_xml_tag(sql_file, "\t", "\n", "column_statistics",
                  "table_name=", table_name, nullptr);

  quoted_table = quote_name(table_name, name_buff, false);
  while ((row = mysql_fetch_row(column_statistics_rs))) {
    char *quoted_column = quote_name(row[0], column_buffer, false);
    if (opt_xml) {
      print_xml_tag(sql_file, "\t\t", "", "field", "name=", row[0],
                    "num_buckets=", row[1], nullptr);
      fputs("</field>\n", sql_file);
    } else {
      fprintf(sql_file,
              "/*!80002 ANALYZE TABLE %s UPDATE HISTOGRAM ON %s "
              "WITH %s BUCKETS */;\n",
              quoted_table, quoted_column, row[1]);
    }
  }

  if (opt_xml) {
    fputs("\t</column_statistics>\n", sql_file);
    check_io(sql_file);
  }

skip:
  mysql_free_result(column_statistics_rs);

  if (switch_character_set_results(mysql, default_charset))
    goto done; /* purecov: deadcode */

  /*
    make sure to set back ansi_quotes_mode mode to
    original value
  */
  ansi_quotes_mode = old_ansi_quotes_mode;

  ret = false;

done:
  if (path) my_fclose(sql_file, MYF(0));

  return ret;
}

static void add_load_option(DYNAMIC_STRING *str, const char *option,
                            const char *option_value) {
  if (!option_value) {
    /* Null value means we don't add this option. */
    return;
  }

  dynstr_append_checked(str, option);

  if (strncmp(option_value, "0x", sizeof("0x") - 1) == 0) {
    /* It's a hex constant, don't escape */
    dynstr_append_checked(str, option_value);
  } else {
    /* char constant; escape */
    field_escape(str, option_value);
  }
}

/*
  Allow the user to specify field terminator strings like:
  "'", "\", "\\" (escaped backslash), "\t" (tab), "\n" (newline)
  This is done by doubling ' and add a end -\ if needed to avoid
  syntax errors from the SQL parser.
*/

static void field_escape(DYNAMIC_STRING *in, const char *from) {
  uint end_backslashes = 0;

  dynstr_append_checked(in, "'");

  while (*from) {
    dynstr_append_mem_checked(in, from, 1);

    if (*from == '\\')
      end_backslashes ^= 1; /* find odd number of backslashes */
    else {
      if (*from == '\'' && !end_backslashes) {
        /* We want a duplicate of "'" for MySQL */
        dynstr_append_checked(in, "\'");
      }
      end_backslashes = 0;
    }
    from++;
  }
  /* Add missing backslashes if user has specified odd number of backs.*/
  if (end_backslashes) dynstr_append_checked(in, "\\");

  dynstr_append_checked(in, "'");
}

static char *alloc_query_str(size_t size) {
  char *query;

  if (!(query = (char *)my_malloc(PSI_NOT_INSTRUMENTED, size, MYF(MY_WME))))
    die(EX_MYSQLERR, "Couldn't allocate a query string.");

  return query;
}

/*

 SYNOPSIS
  dump_table()

  dump_table saves database contents as a series of INSERT statements.

  ARGS
   table - table name
   db    - db name

   RETURNS
    void
*/

static void dump_table(char *table, char *db) {
  char ignore_flag;
  char buf[240], table_buff[NAME_LEN + 3];
  DYNAMIC_STRING query_string;
  DYNAMIC_STRING extended_row;
  char table_type[NAME_LEN];
  char *result_table, table_buff2[NAME_LEN * 2 + 3], *opt_quoted_table;
  int error = 0;
  ulong rownr, row_break;
  size_t total_length, init_length;
  uint num_fields;
  MYSQL_RES *res;
  MYSQL_FIELD *field;
  MYSQL_ROW row;
  bool real_columns[MAX_FIELDS];
  DBUG_TRACE;
  char *order_by = nullptr;

  /*
    Make sure you get the create table info before the following check for
    --no-data flag below. Otherwise, the create table info won't be printed.
  */
  std::string column_list;
  num_fields = get_table_structure(table, db, table_type, &ignore_flag,
                                   real_columns, &column_list);

  /*
    The "table" could be a view.  If so, we don't do anything here.
  */
  if (strcmp(table_type, "VIEW") == 0) return;

  /*
    We don't dump data for replication metadata tables.
  */
  if (replication_metadata_tables(db, table)) return;

  /* Check --no-data flag */
  if (opt_no_data) {
    verbose_msg("-- Skipping dump data for table '%s', --no-data was used\n",
                table);
    return;
  }

  DBUG_PRINT("info",
             ("ignore_flag: %x  num_fields: %d", (int)ignore_flag, num_fields));
  /*
    If the table type is a merge table or any type that has to be
     _completely_ ignored and no data dumped
  */
  if (ignore_flag & IGNORE_DATA) {
    verbose_msg(
        "-- Warning: Skipping data for table '%s' because "
        "it's of type %s\n",
        table, table_type);
    return;
  }
  /* Check that there are any fields in the table */
  if (num_fields == 0) {
    verbose_msg("-- Skipping dump data for table '%s', it has no fields\n",
                table);
    return;
  }

  result_table = quote_name(table, table_buff, true);
  opt_quoted_table = quote_name(table, table_buff2, false);

  verbose_msg("-- Sending SELECT query...\n");

  init_dynamic_string_checked(&query_string, "", 1024);
  if (extended_insert) init_dynamic_string_checked(&extended_row, "", 1024);

  if (opt_order_by_primary) order_by = primary_key_fields(result_table);
  if (path) {
    char filename[FN_REFLEN], tmp_path[FN_REFLEN];

    /*
      Convert the path to native os format
      and resolve to the full filepath.
    */
    convert_dirname(tmp_path, path, NullS);
    my_load_path(tmp_path, tmp_path, nullptr);
    fn_format(filename, table, tmp_path, ".txt",
              MYF(MY_UNPACK_FILENAME | MY_APPEND_EXT));

    /* Must delete the file that 'INTO OUTFILE' will write to */
    my_delete(filename, MYF(0));

    /* convert to a unix path name to stick into the query */
    to_unix_path(filename);

    /* now build the query string */

    dynstr_append_checked(&query_string, "SELECT /*!40001 SQL_NO_CACHE */ ");
    if (column_list.empty())
      dynstr_append_checked(&query_string, "*");
    else
      dynstr_append_checked(&query_string, column_list.c_str());
    dynstr_append_checked(&query_string, " INTO OUTFILE '");
    dynstr_append_checked(&query_string, filename);
    dynstr_append_checked(&query_string, "'");

    dynstr_append_checked(&query_string, " /*!50138 CHARACTER SET ");
    dynstr_append_checked(&query_string,
                          default_charset == mysql_universal_client_charset
                              ? my_charset_bin.m_coll_name
                              : /* backward compatibility */
                              default_charset);
    dynstr_append_checked(&query_string, " */");

    if (fields_terminated || enclosed || opt_enclosed || escaped)
      dynstr_append_checked(&query_string, " FIELDS");

    add_load_option(&query_string, " TERMINATED BY ", fields_terminated);
    add_load_option(&query_string, " ENCLOSED BY ", enclosed);
    add_load_option(&query_string, " OPTIONALLY ENCLOSED BY ", opt_enclosed);
    add_load_option(&query_string, " ESCAPED BY ", escaped);
    add_load_option(&query_string, " LINES TERMINATED BY ", lines_terminated);

    dynstr_append_checked(&query_string, " FROM ");
    dynstr_append_checked(&query_string, result_table);

    if (where) {
      dynstr_append_checked(&query_string, " WHERE ");
      dynstr_append_checked(&query_string, where);
    }

    if (order_by) {
      dynstr_append_checked(&query_string, " ORDER BY ");
      dynstr_append_checked(&query_string, order_by);
      my_free(order_by);
      order_by = nullptr;
    }

    if (mysql_real_query(mysql, query_string.str, (ulong)query_string.length)) {
      DB_error(mysql, "when executing 'SELECT INTO OUTFILE'");
      dynstr_free(&query_string);
      return;
    }
  } else {
    bool data_freemem = false;
    char const *data_text =
        fix_identifier_with_newline(result_table, &data_freemem);
    print_comment(md_result_file, false,
                  "\n--\n-- Dumping data for table %s\n--\n", data_text);
    if (data_freemem) my_free(const_cast<char *>(data_text));

    dynstr_append_checked(&query_string, "SELECT /*!40001 SQL_NO_CACHE */ ");
    if (column_list.empty())
      dynstr_append_checked(&query_string, "*");
    else
      dynstr_append_checked(&query_string, column_list.c_str());
    dynstr_append_checked(&query_string, " FROM ");
    dynstr_append_checked(&query_string, result_table);

    if (where) {
      bool where_freemem = false;
      char const *where_text =
          fix_identifier_with_newline(where, &where_freemem);
      print_comment(md_result_file, false, "-- WHERE:  %s\n", where_text);
      if (where_freemem) my_free(const_cast<char *>(where_text));

      dynstr_append_checked(&query_string, " WHERE ");
      dynstr_append_checked(&query_string, where);
    }
    if (order_by) {
      bool order_by_freemem = false;
      char const *order_by_text =
          fix_identifier_with_newline(order_by, &order_by_freemem);
      print_comment(md_result_file, false, "-- ORDER BY:  %s\n", order_by_text);
      if (order_by_freemem) my_free(const_cast<char *>(order_by_text));

      dynstr_append_checked(&query_string, " ORDER BY ");
      dynstr_append_checked(&query_string, order_by_text);
      my_free(order_by);
      order_by = nullptr;
    }

    if (!opt_xml && !opt_compact) {
      fputs("\n", md_result_file);
      check_io(md_result_file);
      fflush(md_result_file);
    }
    if (mysql_query_with_error_report(mysql, nullptr, query_string.str)) {
      DB_error(mysql, "when retrieving data from server");
      goto err;
    }
    if (quick)
      res = mysql_use_result(mysql);
    else
      res = mysql_store_result(mysql);
    if (!res) {
      DB_error(mysql, "when retrieving data from server");
      goto err;
    }
    auto res_guard = create_scope_guard([&] { mysql_free_result(res); });

    verbose_msg("-- Retrieving rows...\n");
    if (mysql_num_fields(res) != num_fields) {
      fprintf(stderr, "%s: Error in field count for table: %s !  Aborting.\n",
              my_progname, result_table);
      error = EX_CONSCHECK;
      goto err;
    }

    if (opt_lock && !(innodb_stats_tables(db, table))) {
      fprintf(md_result_file, "LOCK TABLES %s WRITE;\n", opt_quoted_table);
      check_io(md_result_file);
    }
    /* Moved disable keys to after lock per bug 15977 */
    if (opt_disable_keys) {
      fprintf(md_result_file, "/*!40000 ALTER TABLE %s DISABLE KEYS */;\n",
              opt_quoted_table);
      check_io(md_result_file);
    }

    total_length = opt_net_buffer_length; /* Force row break */
    row_break = 0;
    rownr = 0;
    init_length = (uint)insert_pat.length + 4;
    if (opt_xml)
      print_xml_tag(md_result_file, "\t", "\n", "table_data", "name=", table,
                    NullS);
    if (opt_autocommit) {
      fprintf(md_result_file, "set autocommit=0;\n");
      check_io(md_result_file);
    }

    while ((row = mysql_fetch_row(res))) {
      uint i;
      ulong *lengths = mysql_fetch_lengths(res);
      bool first_column = true;
      rownr++;
      if (!extended_insert && !opt_xml) {
        fputs(insert_pat.str, md_result_file);
        check_io(md_result_file);
      }
      mysql_field_seek(res, 0);

      if (opt_xml) {
        fputs("\t<row>\n", md_result_file);
        check_io(md_result_file);
      }

      for (i = 0; i < mysql_num_fields(res); i++) {
        int is_blob;
        const ulong length = lengths[i];

        if (!(field = mysql_fetch_field(res)))
          die(EX_CONSCHECK, "Not enough fields from table %s! Aborting.\n",
              result_table);

        if (!real_columns[i]) continue;
        /*
           63 is my_charset_bin. If charsetnr is not 63,
           we have not a BLOB but a TEXT column.
        */
        is_blob =
            (field->charsetnr == 63 && (field->type == MYSQL_TYPE_BIT ||
                                        field->type == MYSQL_TYPE_STRING ||
                                        field->type == MYSQL_TYPE_VAR_STRING ||
                                        field->type == MYSQL_TYPE_VARCHAR ||
                                        field->type == MYSQL_TYPE_BLOB ||
                                        field->type == MYSQL_TYPE_VECTOR ||
                                        field->type == MYSQL_TYPE_LONG_BLOB ||
                                        field->type == MYSQL_TYPE_MEDIUM_BLOB ||
                                        field->type == MYSQL_TYPE_TINY_BLOB ||
                                        field->type == MYSQL_TYPE_GEOMETRY))
                ? 1
                : 0;
        if (extended_insert && !opt_xml) {
          if (first_column) {
            dynstr_set_checked(&extended_row, "(");
            first_column = false;
          } else
            dynstr_append_checked(&extended_row, ",");

          if (row[i]) {
            if (length) {
              if (!(field->flags & NUM_FLAG)) {
                /*
                  "length * 2 + 2" is OK for HEX mode:
                  - In HEX mode we need exactly 2 bytes per character
                  plus 2 bytes for '0x' prefix.
                  - In non-HEX mode we need up to 2 bytes per character,
                  plus 2 bytes for leading and trailing '\'' characters
                  and reserve 1 byte for terminating '\0'.
                  In addition to this, for the blob type, we need to
                  reserve for the "_binary " string that gets added in
                  front of the string in the dump.
                */
                if (opt_hex_blob && is_blob) {
                  dynstr_realloc_checked(&extended_row, length * 2 + 2 + 1);
                  dynstr_append_checked(&extended_row, "0x");
                  extended_row.length += mysql_hex_string(
                      extended_row.str + extended_row.length, row[i], length);
                  assert(extended_row.length + 1 <= extended_row.max_length);
                  /* mysql_hex_string() already terminated string by '\0' */
                  assert(extended_row.str[extended_row.length] == '\0');
                } else {
                  dynstr_realloc_checked(
                      &extended_row,
                      length * 2 + 2 + 1 + (is_blob ? strlen("_binary ") : 0));
                  if (is_blob) {
                    /*
                      inform SQL parser that this string isn't in
                      character_set_connection, so it doesn't emit a warning.
                    */
                    dynstr_append_checked(&extended_row, "_binary ");
                  }
                  dynstr_append_checked(&extended_row, "'");
                  extended_row.length += mysql_real_escape_string_quote(
                      &mysql_connection, &extended_row.str[extended_row.length],
                      row[i], length, '\'');
                  extended_row.str[extended_row.length] = '\0';
                  dynstr_append_checked(&extended_row, "'");
                }
              } else {
                /* change any strings ("inf", "-inf", "nan") into NULL */
                char *ptr = row[i];
                if (my_isalpha(charset_info, *ptr) ||
                    (*ptr == '-' && my_isalpha(charset_info, ptr[1])))
                  dynstr_append_checked(&extended_row, "NULL");
                else {
                  if (field->type == MYSQL_TYPE_DECIMAL) {
                    /* add " signs around */
                    dynstr_append_checked(&extended_row, "'");
                    dynstr_append_checked(&extended_row, ptr);
                    dynstr_append_checked(&extended_row, "'");
                  } else
                    dynstr_append_checked(&extended_row, ptr);
                }
              }
            } else
              dynstr_append_checked(&extended_row, "''");
          } else
            dynstr_append_checked(&extended_row, "NULL");
        } else {
          if (i && !opt_xml) {
            fputc(',', md_result_file);
            check_io(md_result_file);
          }
          if (row[i]) {
            if (!(field->flags & NUM_FLAG)) {
              if (opt_xml) {
                if (opt_hex_blob && is_blob && length) {
                  /* Define xsi:type="xs:hexBinary" for hex encoded data */
                  print_xml_tag(md_result_file, "\t\t", "", "field",
                                "name=", field->name,
                                "xsi:type=", "xs:hexBinary", NullS);
                  print_blob_as_hex(md_result_file, row[i], length);
                } else {
                  print_xml_tag(md_result_file, "\t\t", "", "field",
                                "name=", field->name, NullS);
                  print_quoted_xml(md_result_file, row[i], length, false);
                }
                fputs("</field>\n", md_result_file);
              } else if (opt_hex_blob && is_blob && length) {
                fputs("0x", md_result_file);
                print_blob_as_hex(md_result_file, row[i], length);
              } else {
                if (is_blob) {
                  fputs("_binary ", md_result_file);
                  check_io(md_result_file);
                }
                unescape(md_result_file, row[i], length);
              }
            } else {
              /* change any strings ("inf", "-inf", "nan") into NULL */
              char *ptr = row[i];
              if (opt_xml) {
                print_xml_tag(md_result_file, "\t\t", "", "field",
                              "name=", field->name, NullS);
                fputs(!my_isalpha(charset_info, *ptr) ? ptr : "NULL",
                      md_result_file);
                fputs("</field>\n", md_result_file);
              } else if (my_isalpha(charset_info, *ptr) ||
                         (*ptr == '-' && my_isalpha(charset_info, ptr[1])))
                fputs("NULL", md_result_file);
              else if (field->type == MYSQL_TYPE_DECIMAL) {
                /* add " signs around */
                fputc('\'', md_result_file);
                fputs(ptr, md_result_file);
                fputc('\'', md_result_file);
              } else
                fputs(ptr, md_result_file);
            }
          } else {
            /* The field value is NULL */
            if (!opt_xml)
              fputs("NULL", md_result_file);
            else
              print_xml_null_tag(md_result_file, "\t\t",
                                 "field name=", field->name, "\n");
          }
          check_io(md_result_file);
        }
      }

      if (opt_xml) {
        fputs("\t</row>\n", md_result_file);
        check_io(md_result_file);
      }

      if (extended_insert) {
        size_t row_length;
        dynstr_append_checked(&extended_row, ")");
        row_length = 2 + extended_row.length;
        if (total_length + row_length < opt_net_buffer_length) {
          total_length += row_length;
          fputc(',', md_result_file); /* Always row break */
          fputs(extended_row.str, md_result_file);
        } else {
          if (row_break) fputs(";\n", md_result_file);
          row_break = 1; /* This is first row */

          fputs(insert_pat.str, md_result_file);
          fputs(extended_row.str, md_result_file);
          total_length = row_length + init_length;
        }
        check_io(md_result_file);
      } else if (!opt_xml) {
        fputs(");\n", md_result_file);
        check_io(md_result_file);
      }
    }

    /* XML - close table tag and suppress regular output */
    if (opt_xml)
      fputs("\t</table_data>\n", md_result_file);
    else if (extended_insert && row_break)
      fputs(";\n", md_result_file); /* If not empty table */
    fflush(md_result_file);
    check_io(md_result_file);
    if (mysql_errno(mysql)) {
      snprintf(buf, sizeof(buf),
               "%s: Error %d: %s when dumping table %s at row: %ld\n",
               my_progname, mysql_errno(mysql), mysql_error(mysql),
               result_table, rownr);
      fputs(buf, stderr);
      error = EX_CONSCHECK;
      goto err;
    }

    /* Moved enable keys to before unlock per bug 15977 */
    if (opt_disable_keys) {
      fprintf(md_result_file, "/*!40000 ALTER TABLE %s ENABLE KEYS */;\n",
              opt_quoted_table);
      check_io(md_result_file);
    }
    if (opt_lock && !(innodb_stats_tables(db, table))) {
      fputs("UNLOCK TABLES;\n", md_result_file);
      check_io(md_result_file);
    }
    if (opt_autocommit) {
      fprintf(md_result_file, "commit;\n");
      check_io(md_result_file);
    }
    res_guard.reset();
  }
  dynstr_free(&query_string);
  if (extended_insert) dynstr_free(&extended_row);
  return;

err:
  dynstr_free(&query_string);
  if (extended_insert) dynstr_free(&extended_row);
  if (order_by) {
    my_free(order_by);
    order_by = nullptr;
  }
  maybe_exit(error);
} /* dump_table */

static char *getTableName(int reset) {
  static MYSQL_RES *res = nullptr;
  MYSQL_ROW row;

  if (!res) {
    if (!(res = mysql_list_tables(mysql, NullS))) return (nullptr);
  }
  if ((row = mysql_fetch_row(res))) return ((char *)row[0]);

  if (reset)
    mysql_data_seek(res, 0); /* We want to read again */
  else {
    mysql_free_result(res);
    res = nullptr;
  }
  return (nullptr);
} /* getTableName */

/*
  dump all logfile groups and tablespaces
*/

static int dump_all_tablespaces() { return dump_tablespaces(nullptr); }

static int dump_tablespaces_for_tables(char *db, char **table_names,
                                       int tables) {
  DYNAMIC_STRING where;
  int r;
  int i;
  char name_buff[NAME_LEN * 2 + 3];

  mysql_real_escape_string_quote(mysql, name_buff, db, (ulong)strlen(db), '\'');

  init_dynamic_string_checked(&where,
                              " AND TABLESPACE_NAME IN ("
                              "SELECT DISTINCT TABLESPACE_NAME FROM"
                              " INFORMATION_SCHEMA.PARTITIONS"
                              " WHERE"
                              " TABLE_SCHEMA='",
                              256);
  dynstr_append_checked(&where, name_buff);
  dynstr_append_checked(&where, "' AND TABLE_NAME IN (");

  for (i = 0; i < tables; i++) {
    mysql_real_escape_string_quote(mysql, name_buff, table_names[i],
                                   (ulong)strlen(table_names[i]), '\'');

    dynstr_append_checked(&where, "'");
    dynstr_append_checked(&where, name_buff);
    dynstr_append_checked(&where, "',");
  }
  dynstr_trunc(&where, 1);
  dynstr_append_checked(&where, "))");

  DBUG_PRINT("info", ("Dump TS for Tables where: %s", where.str));
  r = dump_tablespaces(where.str);
  dynstr_free(&where);
  return r;
}

static int dump_tablespaces_for_databases(char **databases) {
  DYNAMIC_STRING where;
  int r;
  int i;

  if (databases[0] == nullptr) return 0;

  init_dynamic_string_checked(&where,
                              " AND TABLESPACE_NAME IN ("
                              "SELECT DISTINCT TABLESPACE_NAME FROM"
                              " INFORMATION_SCHEMA.PARTITIONS"
                              " WHERE"
                              " TABLE_SCHEMA IN (",
                              256);

  for (i = 0; databases[i] != nullptr; i++) {
    char db_name_buff[NAME_LEN * 2 + 3];
    mysql_real_escape_string_quote(mysql, db_name_buff, databases[i],
                                   (ulong)strlen(databases[i]), '\'');
    dynstr_append_checked(&where, "'");
    dynstr_append_checked(&where, db_name_buff);
    dynstr_append_checked(&where, "',");
  }
  dynstr_trunc(&where, 1);
  dynstr_append_checked(&where, "))");

  DBUG_PRINT("info", ("Dump TS for DBs where: %s", where.str));
  r = dump_tablespaces(where.str);
  dynstr_free(&where);
  return r;
}

static int dump_tablespaces(char *ts_where) {
  MYSQL_ROW row;
  MYSQL_RES *tableres;
  char buf[FN_REFLEN];
  DYNAMIC_STRING sqlbuf;
  ulong *lengths;
  int first = 0;
  /*
    The following are used for parsing the EXTRA field
  */
  char extra_format[] = "UNDO_BUFFER_SIZE=";
  char *ubs;
  char *endsemi;
  DBUG_TRACE;

  init_dynamic_string_checked(&sqlbuf,
                              "SELECT LOGFILE_GROUP_NAME,"
                              " FILE_NAME,"
                              " TOTAL_EXTENTS,"
                              " INITIAL_SIZE,"
                              " ENGINE,"
                              " EXTRA"
                              " FROM INFORMATION_SCHEMA.FILES"
                              " WHERE ENGINE = 'ndbcluster'"
                              " AND FILE_TYPE = 'UNDO LOG'"
                              " AND FILE_NAME IS NOT NULL"
                              " AND LOGFILE_GROUP_NAME IS NOT NULL",
                              256);
  if (ts_where) {
    dynstr_append_checked(&sqlbuf,
                          " AND LOGFILE_GROUP_NAME IN ("
                          "SELECT DISTINCT LOGFILE_GROUP_NAME"
                          " FROM INFORMATION_SCHEMA.FILES"
                          " WHERE ENGINE = 'ndbcluster'"
                          " AND FILE_TYPE = 'DATAFILE'");
    dynstr_append_checked(&sqlbuf, ts_where);
    dynstr_append_checked(&sqlbuf, ")");
  }
  dynstr_append_checked(&sqlbuf,
                        " GROUP BY LOGFILE_GROUP_NAME, FILE_NAME"
                        ", ENGINE, TOTAL_EXTENTS, INITIAL_SIZE"
                        " ORDER BY LOGFILE_GROUP_NAME");

  if (mysql_query(mysql, sqlbuf.str) ||
      !(tableres = mysql_store_result(mysql))) {
    dynstr_free(&sqlbuf);
    if (mysql_errno(mysql) == ER_BAD_TABLE_ERROR ||
        mysql_errno(mysql) == ER_BAD_DB_ERROR ||
        mysql_errno(mysql) == ER_UNKNOWN_TABLE) {
      fprintf(md_result_file,
              "\n--\n-- Not dumping tablespaces as no INFORMATION_SCHEMA.FILES"
              " table on this server\n--\n");
      check_io(md_result_file);
      return 0;
    }

    my_printf_error(0, "Error: '%s' when trying to dump tablespaces", MYF(0),
                    mysql_error(mysql));
    return 1;
  }

  buf[0] = 0;
  while ((row = mysql_fetch_row(tableres))) {
    lengths = mysql_fetch_lengths(tableres);
    if (strcmp(buf, row[0]) != 0) first = 1;
    if (first) {
      /*
       * The print_comment below prints single line comments in the
       * md_result_file (--). The single line comment is terminated by a new
       * line, however because of the usage of mysql_real_escape_string_quote,
       * the new line character will get escaped too in the string, hence
       * another new line characters are being used at the end of the string
       * to terminate the single line comment.
       */
      mysql_real_escape_string_quote(mysql, buf, row[0], lengths[0], '\'');
      print_comment(md_result_file, false, "\n--\n-- Logfile group: %s\n--\n",
                    buf);
      buf[0] = 0;
      fprintf(md_result_file, "\nCREATE");
    } else {
      fprintf(md_result_file, "\nALTER");
    }
    fprintf(md_result_file, " LOGFILE GROUP ");
    fprintf_string(row[0], lengths[0], '`', true);
    fprintf(md_result_file, "  ADD UNDOFILE ");
    fprintf_string(row[1], lengths[1], '\'', true);
    if (first) {
      ubs = strstr(row[5], extra_format);
      if (!ubs) break;
      ubs += strlen(extra_format);
      endsemi = strstr(ubs, ";");
      if (endsemi) endsemi[0] = '\0';
      if (!is_string_integer(ubs, (ulong)strlen(ubs))) return 1;
      fprintf(md_result_file, "  UNDO_BUFFER_SIZE %s\n", ubs);
    }
    if (!is_string_integer(row[3], lengths[3])) return 1;
    fprintf(md_result_file, "  INITIAL_SIZE %s\n  ENGINE=", row[3]);
    fprintf_string(row[4], lengths[4], '`', false);
    fprintf(md_result_file, ";\n");
    check_io(md_result_file);
    if (first) {
      first = 0;
      strxmov(buf, row[0], NullS);
    }
  }
  dynstr_free(&sqlbuf);
  mysql_free_result(tableres);
  init_dynamic_string_checked(&sqlbuf,
                              "SELECT DISTINCT TABLESPACE_NAME,"
                              " FILE_NAME,"
                              " LOGFILE_GROUP_NAME,"
                              " EXTENT_SIZE,"
                              " INITIAL_SIZE,"
                              " ENGINE"
                              " FROM INFORMATION_SCHEMA.FILES"
                              " WHERE FILE_TYPE = 'DATAFILE'",
                              256);

  if (ts_where) dynstr_append_checked(&sqlbuf, ts_where);

  dynstr_append_checked(&sqlbuf,
                        " ORDER BY TABLESPACE_NAME, LOGFILE_GROUP_NAME");

  if (mysql_query_with_error_report(mysql, &tableres, sqlbuf.str)) {
    dynstr_free(&sqlbuf);
    return 1;
  }

  DBUG_EXECUTE_IF("tablespace_injection_test", {
    mysql_free_result(tableres);
    mysql_query_with_error_report(
        mysql, &tableres,
        "SELECT 'T`N; /*' AS TABLESPACE_NAME, 'FN' AS FILE_NAME, 'LGN' AS "
        "LOGFILE_GROUP_NAME, 77 AS EXTENT_SIZE, 88 AS INITIAL_SIZE, "
        "'*/\nsystem touch foo;\n' AS ENGINE");
  });

  buf[0] = 0;
  while ((row = mysql_fetch_row(tableres))) {
    lengths = mysql_fetch_lengths(tableres);
    if (strcmp(buf, row[0]) != 0) first = 1;
    if (first) {
      /*
       * The print_comment below prints single line comments in the
       * md_result_file (--). The single line comment is terminated by a new
       * line, however because of the usage of mysql_real_escape_string_quote,
       * the new line character will get escaped too in the string, hence
       * another new line characters are being used at the end of the string
       * to terminate the single line comment.
       */
      mysql_real_escape_string_quote(mysql, buf, row[0], lengths[0], '\'');
      print_comment(md_result_file, false, "\n--\n-- Tablespace: %s\n--\n",
                    buf);
      buf[0] = 0;
      fprintf(md_result_file, "\nCREATE");
    } else {
      fprintf(md_result_file, "\nALTER");
    }
    fprintf(md_result_file, " TABLESPACE ");
    fprintf_string(row[0], lengths[0], '`', true);
    fprintf(md_result_file, "  ADD DATAFILE ");
    fprintf_string(row[1], lengths[1], '\'', true);
    if (first) {
      fprintf(md_result_file, "  USE LOGFILE GROUP ");
      fprintf_string(row[2], lengths[2], '`', true);
      if (!is_string_integer(row[3], lengths[3])) return 1;
      fprintf(md_result_file, "  EXTENT_SIZE %s\n", row[3]);
    }
    if (!is_string_integer(row[4], lengths[4])) return 1;
    fprintf(md_result_file, "  INITIAL_SIZE %s\n  ENGINE=", row[4]);
    fprintf_string(row[5], lengths[5], '`', false);
    fprintf(md_result_file, ";\n");
    check_io(md_result_file);
    if (first) {
      first = 0;
      strxmov(buf, row[0], NullS);
    }
  }

  mysql_free_result(tableres);
  dynstr_free(&sqlbuf);
  return 0;
}

static int is_ndbinfo(MYSQL *mysql, const char *dbname) {
  static int checked_ndbinfo = 0;
  static int have_ndbinfo = 0;

  if (!checked_ndbinfo) {
    MYSQL_RES *res;
    MYSQL_ROW row;
    char buf[32], query[64];

    snprintf(query, sizeof(query), "SHOW VARIABLES LIKE %s",
             quote_for_like("ndbinfo_version", buf));

    checked_ndbinfo = 1;

    if (mysql_query_with_error_report(mysql, &res, query)) return 0;

    if (!(row = mysql_fetch_row(res))) {
      mysql_free_result(res);
      return 0;
    }

    have_ndbinfo = 1;
    mysql_free_result(res);
  }

  if (!have_ndbinfo) return 0;

  if (my_strcasecmp(&my_charset_latin1, dbname, "ndbinfo") == 0) return 1;

  return 0;
}

static int dump_all_databases() {
  MYSQL_ROW row;
  MYSQL_RES *tableres;
  int result = 0;

  my_ulonglong total_databases = 0;
  char **database_list;
  uint db_cnt = 0, cnt = 0;
  uint mysql_db_found = 0;

  if (mysql_query_with_error_report(mysql, &tableres, "SHOW DATABASES"))
    return 1;

  total_databases = mysql_num_rows(tableres);
  database_list = (char **)my_malloc(
      PSI_NOT_INSTRUMENTED, (sizeof(char *) * total_databases), MYF(MY_WME));

  while ((row = mysql_fetch_row(tableres))) {
    if (mysql_get_server_version(mysql) >= FIRST_INFORMATION_SCHEMA_VERSION &&
        !my_strcasecmp(&my_charset_latin1, row[0], INFORMATION_SCHEMA_DB_NAME))
      continue;

    if (mysql_get_server_version(mysql) >= FIRST_PERFORMANCE_SCHEMA_VERSION &&
        !my_strcasecmp(&my_charset_latin1, row[0], PERFORMANCE_SCHEMA_DB_NAME))
      continue;

    if (mysql_get_server_version(mysql) >= FIRST_SYS_SCHEMA_VERSION &&
        !my_strcasecmp(&my_charset_latin1, row[0], SYS_SCHEMA_DB_NAME))
      continue;

    if (is_ndbinfo(mysql, row[0])) continue;
    if (mysql_db_found || (!my_strcasecmp(charset_info, row[0], "mysql"))) {
      if (dump_all_tables_in_db(row[0])) result = 1;
      mysql_db_found = 1;
      /*
        once mysql database is found dump all dbs saved as part
        of database_list
      */
      for (; cnt < db_cnt; cnt++) {
        if (dump_all_tables_in_db(database_list[cnt])) result = 1;
        my_free(database_list[cnt]);
      }
    } else {
      /*
        till mysql database is not found save database names to
        database_list
      */
      database_list[db_cnt] =
          my_strdup(PSI_NOT_INSTRUMENTED, row[0], MYF(MY_WME | MY_ZEROFILL));
      db_cnt++;
    }
  }
  assert(mysql_db_found);
  memset(database_list, 0, sizeof(*database_list));
  my_free(database_list);
  mysql_free_result(tableres);
  if (seen_views) {
    if (mysql_query(mysql, "SHOW DATABASES") ||
        !(tableres = mysql_store_result(mysql))) {
      my_printf_error(0, "Error: Couldn't execute 'SHOW DATABASES': %s", MYF(0),
                      mysql_error(mysql));
      return 1;
    }
    while ((row = mysql_fetch_row(tableres))) {
      if (mysql_get_server_version(mysql) >= FIRST_INFORMATION_SCHEMA_VERSION &&
          !my_strcasecmp(&my_charset_latin1, row[0],
                         INFORMATION_SCHEMA_DB_NAME))
        continue;

      if (mysql_get_server_version(mysql) >= FIRST_PERFORMANCE_SCHEMA_VERSION &&
          !my_strcasecmp(&my_charset_latin1, row[0],
                         PERFORMANCE_SCHEMA_DB_NAME))
        continue;

      if (mysql_get_server_version(mysql) >= FIRST_SYS_SCHEMA_VERSION &&
          !my_strcasecmp(&my_charset_latin1, row[0], SYS_SCHEMA_DB_NAME))
        continue;

      if (is_ndbinfo(mysql, row[0])) continue;

      if (dump_all_views_in_db(row[0])) result = 1;
    }
    mysql_free_result(tableres);
  }
  return result;
}
/* dump_all_databases */

static int dump_databases(char **db_names) {
  int result = 0;
  char **db;
  DBUG_TRACE;

  for (db = db_names; *db; db++) {
    if (is_infoschema_db(*db))
      die(EX_USAGE, "Dumping \'%s\' DB content is not supported", *db);

    if (dump_all_tables_in_db(*db)) result = 1;
  }
  if (!result && seen_views) {
    for (db = db_names; *db; db++) {
      if (dump_all_views_in_db(*db)) result = 1;
    }
  }
  return result;
} /* dump_databases */

/*
View Specific database initialization.

SYNOPSIS
  init_dumping_views
  qdatabase      quoted name of the database

RETURN VALUES
  0        Success.
  1        Failure.
*/
int init_dumping_views(char *qdatabase [[maybe_unused]]) {
  return 0;
} /* init_dumping_views */

/*
Table Specific database initialization.

SYNOPSIS
  init_dumping_tables
  qdatabase      quoted name of the database

RETURN VALUES
  0        Success.
  1        Failure.
*/

int init_dumping_tables(char *qdatabase) {
  DBUG_TRACE;

  if (!opt_create_db) {
    char qbuf[256];
    MYSQL_ROW row;
    MYSQL_RES *dbinfo;

    snprintf(qbuf, sizeof(qbuf), "SHOW CREATE DATABASE IF NOT EXISTS %s",
             qdatabase);

    if (mysql_query(mysql, qbuf) || !(dbinfo = mysql_store_result(mysql))) {
      /* Old server version, dump generic CREATE DATABASE */
      if (opt_drop_database)
        fprintf(md_result_file, "\n/*!40000 DROP DATABASE IF EXISTS %s*/;\n",
                qdatabase);
      fprintf(md_result_file,
              "\nCREATE DATABASE /*!32312 IF NOT EXISTS*/ %s;\n", qdatabase);
    } else {
      if (opt_drop_database)
        fprintf(md_result_file, "\n/*!40000 DROP DATABASE IF EXISTS %s*/;\n",
                qdatabase);
      row = mysql_fetch_row(dbinfo);
      if (row[1]) {
        fprintf(md_result_file, "\n%s;\n", row[1]);
      }
      mysql_free_result(dbinfo);
    }
  }
  return 0;
} /* init_dumping_tables */

static int init_dumping(char *database, int init_func(char *)) {
  if (is_ndbinfo(mysql, database)) {
    verbose_msg("-- Skipping dump of ndbinfo database\n");
    return 0;
  }

  if (mysql_select_db(mysql, database)) {
    DB_error(mysql, "when selecting the database");
    return 1; /* If --force */
  }
  if (!path && !opt_xml) {
    if (opt_databases || opt_alldbs) {
      /*
        length of table name * 2 (if name contains quotes), 2 quotes and 0
      */
      char quoted_database_buf[NAME_LEN * 2 + 3];
      char *qdatabase = quote_name(database, quoted_database_buf, opt_quoted);

      bool freemem = false;
      char const *text = fix_identifier_with_newline(qdatabase, &freemem);
      print_comment(md_result_file, false,
                    "\n--\n-- Current Database: %s\n--\n", text);
      if (freemem) my_free(const_cast<char *>(text));

      /* Call the view or table specific function */
      init_func(qdatabase);

      fprintf(md_result_file, "\nUSE %s;\n", qdatabase);
      check_io(md_result_file);
    }
  }
  return 0;
} /* init_dumping */

/* Return 1 if we should copy the table */

static bool include_table(const char *hash_key, size_t len) {
  return ignore_table->count(string(hash_key, len)) == 0;
}

static int dump_all_tables_in_db(char *database) {
  char *table;
  uint numrows;
  char table_buff[NAME_LEN * 2 + 3];
  char hash_key[2 * NAME_LEN + 2]; /* "db.tablename" */
  char *afterdot;
  bool general_log_table_exists = false, slow_log_table_exists = false;
  const int using_mysql_db = !my_strcasecmp(charset_info, database, "mysql");
  bool real_columns[MAX_FIELDS];

  DBUG_TRACE;

  afterdot = my_stpcpy(hash_key, database);
  *afterdot++ = '.';

  if (init_dumping(database, init_dumping_tables)) return 1;
  if (opt_xml)
    print_xml_tag(md_result_file, "", "\n", "database", "name=", database,
                  NullS);

  if (lock_tables) {
    DYNAMIC_STRING query;
    init_dynamic_string_checked(&query, "LOCK TABLES ", 256);
    for (numrows = 0; (table = getTableName(1));) {
      char *end = my_stpcpy(afterdot, table);
      if (include_table(hash_key, end - hash_key)) {
        numrows++;
        dynstr_append_checked(&query, quote_name(table, table_buff, true));
        dynstr_append_checked(&query, " READ /*!32311 LOCAL */,");
      }
    }
    if (numrows &&
        mysql_real_query(mysql, query.str, (ulong)(query.length - 1)))
      DB_error(mysql, "when using LOCK TABLES");
    /* We shall continue here, if --force was given */
    dynstr_free(&query);
  }
  if (flush_logs) {
    if (mysql_query(mysql, "FLUSH /*!40101 LOCAL */ LOGS"))
      DB_error(mysql, "when doing refresh");
    /* We shall continue here, if --force was given */
    else
      verbose_msg("-- dump_all_tables_in_db : logs flushed successfully!\n");
  }
  if (opt_single_transaction && mysql_get_server_version(mysql) >= 50500) {
    verbose_msg("-- Setting savepoint...\n");
    if (mysql_query_with_error_report(mysql, nullptr, "SAVEPOINT sp")) return 1;
  }
  while ((table = getTableName(0))) {
    char *end = my_stpcpy(afterdot, table);
    if (include_table(hash_key, end - hash_key)) {
      dump_table(table, database);
      if (opt_dump_triggers && mysql_get_server_version(mysql) >= 50009) {
        if (dump_triggers_for_table(table, database)) {
          if (path) my_fclose(md_result_file, MYF(MY_WME));
          maybe_exit(EX_MYSQLERR);
        }
      }

      if (column_statistics &&
          dump_column_statistics_for_table(table, database)) {
        /* purecov: begin inspected */
        if (path) my_fclose(md_result_file, MYF(MY_WME));
        maybe_exit(EX_MYSQLERR);
        /* purecov: end */
      }

      /**
        ROLLBACK TO SAVEPOINT in --single-transaction mode to release metadata
        lock on table which was already dumped. This allows to avoid blocking
        concurrent DDL on this table without sacrificing correctness, as we
        won't access table second time and dumps created by --single-transaction
        mode have validity point at the start of transaction anyway.
        Note that this doesn't make --single-transaction mode with concurrent
        DDL safe in general case. It just improves situation for people for whom
        it might be working.
      */
      if (opt_single_transaction && mysql_get_server_version(mysql) >= 50500) {
        verbose_msg("-- Rolling back to savepoint sp...\n");
        if (mysql_query_with_error_report(mysql, nullptr,
                                          "ROLLBACK TO SAVEPOINT sp"))
          maybe_exit(EX_MYSQLERR);
      }
    } else {
      /*
        If general_log and slow_log exists in the 'mysql' database,
         we should dump the table structure. But we cannot
         call get_table_structure() here as 'LOCK TABLES' query got executed
         above on the session and that 'LOCK TABLES' query does not contain
         'general_log' and 'slow_log' tables. (you cannot acquire lock
         on log tables). Hence mark the existence of these log tables here and
         after 'UNLOCK TABLES' query is executed on the session, get the table
         structure from server and dump it in the file.
      */
      if (using_mysql_db) {
        if (!my_strcasecmp(charset_info, table, "general_log"))
          general_log_table_exists = true;
        else if (!my_strcasecmp(charset_info, table, "slow_log"))
          slow_log_table_exists = true;
      }
    }
  }

  if (opt_single_transaction && mysql_get_server_version(mysql) >= 50500) {
    verbose_msg("-- Releasing savepoint...\n");
    if (mysql_query_with_error_report(mysql, nullptr, "RELEASE SAVEPOINT sp"))
      return 1;
  }

  if (opt_events && mysql_get_server_version(mysql) >= 50106) {
    DBUG_PRINT("info", ("Dumping events for database %s", database));
    dump_events_for_db(database);
  }
  if (opt_routines && mysql_get_server_version(mysql) >= 50009) {
    DBUG_PRINT("info", ("Dumping routines for database %s", database));
    dump_routines_for_db(database);
  }
  if (opt_xml) {
    fputs("</database>\n", md_result_file);
    check_io(md_result_file);
  }
  if (lock_tables)
    (void)mysql_query_with_error_report(mysql, nullptr, "UNLOCK TABLES");
  if (using_mysql_db) {
    char table_type[NAME_LEN];
    char ignore_flag;
    if (general_log_table_exists) {
      if (!get_table_structure("general_log", database, table_type,
                               &ignore_flag, real_columns, nullptr))
        verbose_msg(
            "-- Warning: get_table_structure() failed with some internal "
            "error for 'general_log' table\n");
    }
    if (slow_log_table_exists) {
      if (!get_table_structure("slow_log", database, table_type, &ignore_flag,
                               real_columns, nullptr))
        verbose_msg(
            "-- Warning: get_table_structure() failed with some internal "
            "error for 'slow_log' table\n");
    }
    if (flush_privileges) {
      fprintf(md_result_file, "\n--\n-- Flush Grant Tables \n--\n");
      fprintf(md_result_file, "\n/*! FLUSH PRIVILEGES */;\n");
    }
  }
  return 0;
} /* dump_all_tables_in_db */

/*
   dump structure of views of database

   SYNOPSIS
     dump_all_views_in_db()
     database  database name

  RETURN
    0 OK
    1 ERROR
*/

static bool dump_all_views_in_db(char *database) {
  char *table;
  uint numrows;
  char table_buff[NAME_LEN * 2 + 3];
  char hash_key[2 * NAME_LEN + 2]; /* "db.tablename" */
  char *afterdot;

  if (opt_ignore_views) return false;

  afterdot = my_stpcpy(hash_key, database);
  *afterdot++ = '.';

  if (init_dumping(database, init_dumping_views)) return true;
  if (opt_xml)
    print_xml_tag(md_result_file, "", "\n", "database", "name=", database,
                  NullS);
  if (lock_tables) {
    DYNAMIC_STRING query;
    init_dynamic_string_checked(&query, "LOCK TABLES ", 256);
    for (numrows = 0; (table = getTableName(1));) {
      char *end = my_stpcpy(afterdot, table);
      if (include_table(hash_key, end - hash_key)) {
        numrows++;
        dynstr_append_checked(&query, quote_name(table, table_buff, true));
        dynstr_append_checked(&query, " READ /*!32311 LOCAL */,");
      }
    }
    if (numrows &&
        mysql_real_query(mysql, query.str, (ulong)(query.length - 1)))
      DB_error(mysql, "when using LOCK TABLES");
    /* We shall continue here, if --force was given */
    dynstr_free(&query);
  }
  if (flush_logs) {
    if (mysql_query(mysql, "FLUSH /*!40101 LOCAL */ LOGS"))
      DB_error(mysql, "when doing refresh");
    /* We shall continue here, if --force was given */
    else
      verbose_msg("-- dump_all_views_in_db : logs flushed successfully!\n");
  }
  while ((table = getTableName(0))) {
    char *end = my_stpcpy(afterdot, table);
    if (include_table(hash_key, end - hash_key))
      get_view_structure(table, database);
  }
  if (opt_xml) {
    fputs("</database>\n", md_result_file);
    check_io(md_result_file);
  }
  if (lock_tables)
    (void)mysql_query_with_error_report(mysql, nullptr, "UNLOCK TABLES");
  return false;
} /* dump_all_tables_in_db */

/*
  get_actual_table_name -- executes a SHOW TABLES LIKE '%s' to get the actual
  table name from the server for the table name given on the command line.
  we do this because the table name given on the command line may be a
  different case (e.g.  T1 vs t1)

  RETURN
    pointer to the table name
    0 if error
*/

static char *get_actual_table_name(const char *old_table_name, MEM_ROOT *root) {
  char *name = nullptr;
  MYSQL_RES *table_res;
  MYSQL_ROW row;
  char query[4 * NAME_LEN];
  char show_name_buff[FN_REFLEN];
  DBUG_TRACE;

  /* Check memory for quote_for_like() */
  assert(2 * sizeof(old_table_name) < sizeof(show_name_buff));
  snprintf(query, sizeof(query), "SHOW TABLES LIKE %s",
           quote_for_like(old_table_name, show_name_buff));

  if (mysql_query_with_error_report(mysql, nullptr, query)) return NullS;

  if ((table_res = mysql_store_result(mysql))) {
    const uint64_t num_rows = mysql_num_rows(table_res);
    if (num_rows > 0) {
      ulong *lengths;
      /*
        Return first row
        TODO: Return all matching rows
      */
      row = mysql_fetch_row(table_res);
      lengths = mysql_fetch_lengths(table_res);
      name = strmake_root(root, row[0], lengths[0]);
    }
    mysql_free_result(table_res);
  }
  DBUG_PRINT("exit", ("new_table_name: %s", name));
  return name;
}

static int dump_selected_tables(char *db, char **table_names, int tables) {
  char table_buff[NAME_LEN * 2 + 3];
  DYNAMIC_STRING lock_tables_query;
  MEM_ROOT root(PSI_NOT_INSTRUMENTED, 8192);
  char **dump_tables, **pos, **end;
  DBUG_TRACE;

  if (is_infoschema_db(db))
    die(EX_USAGE, "Dumping \'%s\' DB content is not supported", db);

  if (init_dumping(db, init_dumping_tables)) return 1;

  if (!(dump_tables = pos = (char **)root.Alloc(tables * sizeof(char *))))
    die(EX_EOM, "alloc_root failure.");

  init_dynamic_string_checked(&lock_tables_query, "LOCK TABLES ", 256);
  for (; tables > 0; tables--, table_names++) {
    /* the table name passed on commandline may be wrong case */
    if ((*pos = get_actual_table_name(*table_names, &root))) {
      /* Add found table name to lock_tables_query */
      if (lock_tables) {
        dynstr_append_checked(&lock_tables_query,
                              quote_name(*pos, table_buff, true));
        dynstr_append_checked(&lock_tables_query, " READ /*!32311 LOCAL */,");
      }
      pos++;
    } else {
      if (!opt_force) {
        dynstr_free(&lock_tables_query);
        root.Clear();
      }
      maybe_die(EX_ILLEGAL_TABLE, "Couldn't find table: \"%s\"", *table_names);
      /* We shall continue here, if --force was given */
    }
  }
  end = pos;

  /* Can't LOCK TABLES in I_S / P_S, so don't try. */
  if (lock_tables &&
      (mysql_get_server_version(mysql) < FIRST_INFORMATION_SCHEMA_VERSION ||
       my_strcasecmp(&my_charset_latin1, db, INFORMATION_SCHEMA_DB_NAME)) &&
      (mysql_get_server_version(mysql) < FIRST_PERFORMANCE_SCHEMA_VERSION ||
       my_strcasecmp(&my_charset_latin1, db, PERFORMANCE_SCHEMA_DB_NAME))) {
    if (mysql_real_query(mysql, lock_tables_query.str,
                         (ulong)(lock_tables_query.length - 1))) {
      if (!opt_force) {
        dynstr_free(&lock_tables_query);
        root.Clear();
      }
      DB_error(mysql, "when doing LOCK TABLES");
      /* We shall continue here, if --force was given */
    }
  }
  dynstr_free(&lock_tables_query);
  if (flush_logs) {
    if (mysql_query(mysql, "FLUSH /*!40101 LOCAL */ LOGS")) {
      if (!opt_force) root.Clear();
      DB_error(mysql, "when doing refresh");
    }
    /* We shall continue here, if --force was given */
    else
      verbose_msg("-- dump_selected_tables : logs flushed successfully!\n");
  }
  if (opt_xml)
    print_xml_tag(md_result_file, "", "\n", "database", "name=", db, NullS);

  if (opt_single_transaction && mysql_get_server_version(mysql) >= 50500) {
    verbose_msg("-- Setting savepoint...\n");
    if (mysql_query_with_error_report(mysql, nullptr, "SAVEPOINT sp")) return 1;
  }

  /* Dump each selected table */
  for (pos = dump_tables; pos < end; pos++) {
    DBUG_PRINT("info", ("Dumping table %s", *pos));
    dump_table(*pos, db);
    if (opt_dump_triggers && mysql_get_server_version(mysql) >= 50009) {
      if (dump_triggers_for_table(*pos, db)) {
        if (path) my_fclose(md_result_file, MYF(MY_WME));
        maybe_exit(EX_MYSQLERR);
      }
    }

    if (column_statistics && dump_column_statistics_for_table(*pos, db)) {
      /* purecov: begin inspected */
      if (path) my_fclose(md_result_file, MYF(MY_WME));
      maybe_exit(EX_MYSQLERR);
      /* purecov: end */
    }

    /**
      ROLLBACK TO SAVEPOINT in --single-transaction mode to release metadata
      lock on table which was already dumped. This allows to avoid blocking
      concurrent DDL on this table without sacrificing correctness, as we
      won't access table second time and dumps created by --single-transaction
      mode have validity point at the start of transaction anyway.
      Note that this doesn't make --single-transaction mode with concurrent
      DDL safe in general case. It just improves situation for people for whom
      it might be working.
    */
    if (opt_single_transaction && mysql_get_server_version(mysql) >= 50500) {
      verbose_msg("-- Rolling back to savepoint sp...\n");
      if (mysql_query_with_error_report(mysql, nullptr,
                                        "ROLLBACK TO SAVEPOINT sp"))
        maybe_exit(EX_MYSQLERR);
    }
  }

  if (opt_single_transaction && mysql_get_server_version(mysql) >= 50500) {
    verbose_msg("-- Releasing savepoint...\n");
    if (mysql_query_with_error_report(mysql, nullptr, "RELEASE SAVEPOINT sp"))
      return 1;
  }

  /* Dump each selected view */
  if (seen_views) {
    for (pos = dump_tables; pos < end; pos++) get_view_structure(*pos, db);
  }
  if (opt_events && mysql_get_server_version(mysql) >= 50106) {
    DBUG_PRINT("info", ("Dumping events for database %s", db));
    dump_events_for_db(db);
  }
  /* obtain dump of routines (procs/functions) */
  if (opt_routines && mysql_get_server_version(mysql) >= 50009) {
    DBUG_PRINT("info", ("Dumping routines for database %s", db));
    dump_routines_for_db(db);
  }
  root.Clear();
  if (opt_xml) {
    fputs("</database>\n", md_result_file);
    check_io(md_result_file);
  }
  if (lock_tables)
    (void)mysql_query_with_error_report(mysql, nullptr, "UNLOCK TABLES");
  return 0;
} /* dump_selected_tables */

static int do_show_binary_log_status(MYSQL *mysql_con) {
  MYSQL_ROW row;
  MYSQL_RES *source;
  const char *comment_prefix =
      (opt_source_data == MYSQL_OPT_SOURCE_DATA_COMMENTED_SQL) ? "-- " : "";
  if (mysql_query_with_error_report(
          mysql_con, &source,
          get_compatible_rpl_source_query("SHOW BINARY LOG STATUS").c_str())) {
    return 1;
  }
  row = mysql_fetch_row(source);
  if (row && row[0] && row[1]) {
    /* SHOW BINARY LOG STATUS reports file and position */
    print_comment(md_result_file, false,
                  "\n--\n-- Position to start replication or point-in-time "
                  "recovery from\n--\n\n");
    fprintf(
        md_result_file, "%s%s %s='%s', %s=%s;\n", comment_prefix,
        get_compatible_rpl_replica_command("CHANGE REPLICATION SOURCE TO")
            .c_str(),
        get_compatible_rpl_replica_command("SOURCE_LOG_FILE").c_str(), row[0],
        get_compatible_rpl_replica_command("SOURCE_LOG_POS").c_str(), row[1]);
    check_io(md_result_file);
  } else if (!opt_force) {
    /* SHOW BINARY LOG STATUS reports nothing and --force is not enabled */
    my_printf_error(0, "Error: Binlogging on server not active", MYF(0));
    mysql_free_result(source);
    maybe_exit(EX_MYSQLERR);
    return 1;
  }
  mysql_free_result(source);

  return 0;
}

static int do_stop_replica_sql(MYSQL *mysql_con) {
  MYSQL_RES *replica;
  /* We need to check if the replica sql thread is running first */
  if (mysql_query_with_error_report(
          mysql_con, &replica,
          get_compatible_rpl_replica_query("SHOW REPLICA STATUS").c_str()))
    return (1);
  else {
    MYSQL_ROW row = mysql_fetch_row(replica);
    if (row && row[11]) {
      /* if REPLICA SQL is not running, we don't stop it */
      if (!strcmp(row[11], "No")) {
        mysql_free_result(replica);
        /* Silently assume that they don't have the replica running */
        return (0);
      }
    }
  }
  mysql_free_result(replica);

  /* now, stop the replica if running */
  if (mysql_query_with_error_report(
          mysql_con, nullptr,
          get_compatible_rpl_replica_query("STOP REPLICA SQL_THREAD").c_str()))
    return (1);

  return (0);
}

static int add_stop_replica() {
  if (opt_comments)
    fprintf(md_result_file,
            "\n--\n-- stop replica statement to make a recovery dump\n--\n\n");
  fprintf(md_result_file, "%s;\n",
          get_compatible_rpl_replica_command("STOP REPLICA").c_str());
  return (0);
}

static int add_replica_statements() {
  if (opt_comments)
    fprintf(md_result_file,
            "\n--\n-- start replica statement to make a recovery dump\n--\n\n");
  fprintf(md_result_file, "%s;\n",
          get_compatible_rpl_replica_command("START REPLICA").c_str());
  return (0);
}

static int do_show_replica_status(MYSQL *mysql_con) {
  MYSQL_RES *replica = nullptr;
  const char *comment_prefix =
      (opt_replica_data == MYSQL_OPT_REPLICA_DATA_COMMENTED_SQL) ? "-- " : "";
  if (mysql_query_with_error_report(
          mysql_con, &replica,
          get_compatible_rpl_replica_query("SHOW REPLICA STATUS").c_str())) {
    if (!opt_force) {
      /* SHOW REPLICA STATUS reports nothing and --force is not enabled */
      my_printf_error(0, "Error: Replication not configured", MYF(0));
    }
    mysql_free_result(replica);
    return 1;
  } else {
    const int n_source_host = 1;
    const int n_source_port = 3;
    const int n_source_log_file = 9;
    const int n_source_log_pos = 21;
    const int n_channel_name = 55;
    MYSQL_ROW row = mysql_fetch_row(replica);
    /* Since 5.7 is is possible that SSS returns multiple channels */
    while (row) {
      if (row[n_source_log_file] && row[n_source_log_pos]) {
        /* SHOW BINARY LOG STATUS reports file and position */
        if (opt_comments)
          fprintf(md_result_file,
                  "\n--\n-- Position to start replication or point-in-time "
                  "recovery from (the source for this replica)\n--\n\n");

        fprintf(
            md_result_file, "%s%s ", comment_prefix,
            get_compatible_rpl_replica_command("CHANGE REPLICATION SOURCE TO")
                .c_str());

        if (opt_include_source_host_port) {
          if (row[n_source_host])
            fprintf(md_result_file, "%s='%s', ",
                    get_compatible_rpl_replica_command("SOURCE_HOST").c_str(),
                    row[n_source_host]);
          if (row[n_source_port])
            fprintf(md_result_file, "%s=%s, ",
                    get_compatible_rpl_replica_command("SOURCE_PORT").c_str(),
                    row[n_source_port]);
        }
        fprintf(md_result_file, "%s='%s', %s=%s",
                get_compatible_rpl_replica_command("SOURCE_LOG_FILE").c_str(),
                row[n_source_log_file],
                get_compatible_rpl_replica_command("SOURCE_LOG_POS").c_str(),
                row[n_source_log_pos]);

        /* Only print the FOR CHANNEL if there is more than one channel */
        if (replica->row_count > 1)
          fprintf(md_result_file, " FOR CHANNEL '%s'", row[n_channel_name]);

        fprintf(md_result_file, ";\n");
      }
      row = mysql_fetch_row(replica);
    }
    check_io(md_result_file);
    mysql_free_result(replica);
  }
  return 0;
}

static int do_start_replica_sql(MYSQL *mysql_con) {
  MYSQL_RES *replica;
  /* We need to check if the replica sql is stopped in the first place */
  if (mysql_query_with_error_report(
          mysql_con, &replica,
          get_compatible_rpl_replica_query("SHOW REPLICA STATUS").c_str()))
    return (1);
  else {
    MYSQL_ROW row = mysql_fetch_row(replica);
    if (row && row[11]) {
      /* if REPLICA SQL is not running, we don't start it */
      if (!strcmp(row[11], "Yes")) {
        mysql_free_result(replica);
        /* Silently assume that they don't have the replica running */
        return (0);
      }
    }
  }
  mysql_free_result(replica);

  /* now, start the replica if stopped */
  if (mysql_query_with_error_report(
          mysql_con, nullptr,
          get_compatible_rpl_replica_query("START REPLICA").c_str())) {
    my_printf_error(0, "Error: Unable to start replication", MYF(0));
    return 1;
  }
  return (0);
}

static int do_flush_tables_read_lock(MYSQL *mysql_con) {
  /*
    We do first a FLUSH TABLES. If a long update is running, the FLUSH TABLES
    will wait but will not stall the whole mysqld, and when the long update is
    done the FLUSH TABLES WITH READ LOCK will start and succeed quickly. So,
    FLUSH TABLES is to lower the probability of a stage where both mysqldump
    and most client connections are stalled. Of course, if a second long
    update starts between the two FLUSHes, we have that bad stall.
  */
  return (
      mysql_query_with_error_report(mysql_con, nullptr,
                                    "FLUSH /*!40101 LOCAL */ TABLES") ||
      mysql_query_with_error_report(
          mysql_con, nullptr, "FLUSH /*!40101 LOCAL */ TABLES WITH READ LOCK"));
}

static int do_unlock_tables(MYSQL *mysql_con) {
  return mysql_query_with_error_report(mysql_con, nullptr, "UNLOCK TABLES");
}

static int get_bin_log_name(MYSQL *mysql_con, char *buff_log_name,
                            uint buff_len) {
  MYSQL_RES *res = nullptr;
  MYSQL_ROW row;

  if (mysql_query(
          mysql_con,
          get_compatible_rpl_source_query("SHOW BINARY LOG STATUS").c_str()) ||
      !(res = mysql_store_result(mysql)))
    return 1;

  if (!(row = mysql_fetch_row(res))) {
    mysql_free_result(res);
    return 1;
  }
  /*
    Only one row is returned, and the first column is the name of the
    active log.
  */
  strmake(buff_log_name, row[0], buff_len - 1);

  mysql_free_result(res);
  return 0;
}

static int purge_bin_logs_to(MYSQL *mysql_con, char *log_name) {
  DYNAMIC_STRING str;
  int err;
  init_dynamic_string_checked(&str, "PURGE BINARY LOGS TO '", 1024);
  dynstr_append_checked(&str, log_name);
  dynstr_append_checked(&str, "'");
  err = mysql_query_with_error_report(mysql_con, nullptr, str.str);
  dynstr_free(&str);
  return err;
}

static int start_transaction(MYSQL *mysql_con) {
  verbose_msg("-- Starting transaction...\n");
  /*
    We use BEGIN for old servers. --single-transaction --source-data will fail
    on old servers, but that's ok as it was already silently broken (it didn't
    do a consistent read, so better tell people frankly, with the error).

    We want the first consistent read to be used for all tables to dump so we
    need the REPEATABLE READ level (not anything lower, for example READ
    COMMITTED would give one new consistent read per dumped table).
  */
  if ((mysql_get_server_version(mysql_con) < 40100) && opt_source_data) {
    fprintf(stderr,
            "-- %s: the combination of --single-transaction and "
            "--source-data requires a MySQL server version of at least 4.1 "
            "(current server's version is %s). %s\n",
            opt_force ? "Warning" : "Error",
            mysql_con->server_version ? mysql_con->server_version : "unknown",
            opt_force ? "Continuing due to --force, backup may not be "
                        "consistent across all tables!"
                      : "Aborting.");
    if (!opt_force) exit(EX_MYSQLERR);
  }

  return (
      mysql_query_with_error_report(mysql_con, nullptr,
                                    "SET SESSION TRANSACTION ISOLATION "
                                    "LEVEL REPEATABLE READ") ||
      mysql_query_with_error_report(mysql_con, nullptr,
                                    "START TRANSACTION "
                                    "/*!40100 WITH CONSISTENT SNAPSHOT */"));
}

/* Print a value with a prefix on file */
static void print_value(FILE *file, MYSQL_RES *result, MYSQL_ROW row,
                        const char *prefix, const char *name,
                        int string_value) {
  MYSQL_FIELD *field;
  mysql_field_seek(result, 0);

  for (; (field = mysql_fetch_field(result)); row++) {
    if (!strcmp(field->name, name)) {
      if (row[0] && row[0][0] && strcmp(row[0], "0") != 0) /* Skip default */
      {
        fputc(' ', file);
        fputs(prefix, file);
        if (string_value)
          unescape(file, row[0], strlen(row[0]));
        else
          fputs(row[0], file);
        check_io(file);
        return;
      }
    }
  }
  /* This shouldn't happen */
} /* print_value */

/*
  SYNOPSIS

  Check if the table is one of the table types that should be ignored:
  MRG_ISAM, MRG_MYISAM.

  If the table should be altogether ignored, it returns a true, false if it
  should not be ignored.

  ARGS

    check_if_ignore_table()
    table_name                  Table name to check
    table_type                  Type of table

  GLOBAL VARIABLES
    mysql                       MySQL connection
    verbose                     Write warning messages

  RETURN
    char (bit value)            See IGNORE_ values at top
*/

char check_if_ignore_table(const char *table_name, char *table_type) {
  char result = IGNORE_NONE;
  char buff[FN_REFLEN + 80], show_name_buff[FN_REFLEN];
  MYSQL_RES *res = nullptr;
  MYSQL_ROW row;
  DBUG_TRACE;

  /* Check memory for quote_for_like() */
  assert(2 * sizeof(table_name) < sizeof(show_name_buff));
  snprintf(buff, sizeof(buff), "show table status like %s",
           quote_for_like(table_name, show_name_buff));
  if (mysql_query_with_error_report(mysql, &res, buff)) {
    if (mysql_errno(mysql) != ER_PARSE_ERROR) { /* If old MySQL version */
      verbose_msg(
          "-- Warning: Couldn't get status information for "
          "table %s (%s)\n",
          table_name, mysql_error(mysql));
      return result; /* assume table is ok */
    }
  }
  if (!(row = mysql_fetch_row(res))) {
    fprintf(stderr,
            "Error: Couldn't read status information for table %s (%s)\n",
            table_name, mysql_error(mysql));
    mysql_free_result(res);
    return result; /* assume table is ok */
  }
  if (!(row[1]))
    strmake(table_type, "VIEW", NAME_LEN - 1);
  else {
    strmake(table_type, row[1], NAME_LEN - 1);

    /*  If these two types, we want to skip dumping the table. */
    if (!opt_no_data &&
        (!my_strcasecmp(&my_charset_latin1, table_type, "MRG_MyISAM") ||
         !strcmp(table_type, "MRG_ISAM") || !strcmp(table_type, "FEDERATED")))
      result = IGNORE_DATA;
  }
  mysql_free_result(res);
  return result;
}

/**
  Check if the database is 'information_schema' and write a verbose message
  stating that dumping the database is not supported.

  @param  db        Database Name.

  @retval true      If database should be ignored.
  @retval false     If database shouldn't be ignored.
*/

bool is_infoschema_db(const char *db) {
  DBUG_TRACE;

  /*
    INFORMATION_SCHEMA DB content dump is only used to reload the data into
    another tables for analysis purpose. This feature is not the core
    responsibility of mysqldump tool. INFORMATION_SCHEMA DB content can even
    be dumped using other methods like SELECT INTO OUTFILE... for such
    purpose. Hence ignoring INFORMATION_SCHEMA DB here.
  */
  if (mysql_get_server_version(mysql) >= FIRST_INFORMATION_SCHEMA_VERSION &&
      !my_strcasecmp(&my_charset_latin1, db, INFORMATION_SCHEMA_DB_NAME)) {
    verbose_msg("Dumping \'%s\' DB content is not supported", db);
    return true;
  }

  return false;
}

/*
  Get string of comma-separated primary key field names

  SYNOPSIS
    char *primary_key_fields(const char *table_name)
    RETURNS     pointer to allocated buffer (must be freed by caller)
    table_name  quoted table name

  DESCRIPTION
    Use SHOW KEYS FROM table_name, allocate a buffer to hold the
    field names, and then build that string and return the pointer
    to that buffer.

    Returns NULL if there is no PRIMARY or UNIQUE key on the table,
    or if there is some failure.  It is better to continue to dump
    the table unsorted, rather than exit without dumping the data.
*/

static char *primary_key_fields(const char *table_name) {
  MYSQL_RES *res = nullptr;
  MYSQL_ROW row;
  /* SHOW KEYS FROM + table name * 2 (escaped) + 2 quotes + \0 */
  char show_keys_buff[15 + NAME_LEN * 2 + 3];
  size_t result_length = 0;
  char *result = nullptr;
  char buff[NAME_LEN * 2 + 3];
  char *order_by_part;

  snprintf(show_keys_buff, sizeof(show_keys_buff), "SHOW KEYS FROM %s",
           table_name);
  if (mysql_query(mysql, show_keys_buff) ||
      !(res = mysql_store_result(mysql))) {
    fprintf(stderr,
            "Warning: Couldn't read keys from table %s;"
            " records are NOT sorted (%s)\n",
            table_name, mysql_error(mysql));
    /* Don't exit, because it's better to print out unsorted records */
    goto cleanup;
  }

  /*
   * Figure out the length of the ORDER BY clause result.
   * Note that SHOW KEYS is ordered:  a PRIMARY key is always the first
   * row, and UNIQUE keys come before others.  So we only need to check
   * the first key, not all keys.
   */
  while (nullptr != (row = mysql_fetch_row(res))) {
    unsigned const braces_length = 0;
    if (!row[3] || !*row[3]) {
      fprintf(stderr,
              "Warning: Couldn't read key column index from table %s. "
              "Inspect the output from 'SHOW KEYS FROM %s. "
              "Records are probably not fully sorted.'\n",
              table_name, table_name);
      continue;
    }
    if (atoi(row[3]) < 1) break;
    if (row[4] && *row[4]) order_by_part = quote_name(row[4], buff, false);
#ifdef USABLE_EXPR_IN_SHOW_INDEX_BUG35273994
    else if (mysql_num_fields(res) > 14 && row[14] &&
             *row[14]) /* there's an Expression column */
    {
      order_by_part = row[14];
      braces_length = 2;
    }
#endif
    else {
      fprintf(
          stderr,
          "Warning: Couldn't read key column name or expression from table %s;"
          " position %d. Inspect the output from 'SHOW KEYS FROM %s."
          " Records are probably not fully sorted.\n",
          table_name, atoi(row[3]), table_name);
      continue;
    }
    result_length +=
        strlen(order_by_part) + braces_length + 1; /* + 1 for ',' or \0 */
  }

  /* Build the ORDER BY clause result */
  if (result_length) {
    /* result (terminating \0 is already in result_length) */
    char *end = result = static_cast<char *>(
        my_malloc(PSI_NOT_INSTRUMENTED, result_length + 10, MYF(MY_WME)));
    if (!result) {
      fprintf(stderr, "Error: Not enough memory to store ORDER BY clause\n");
      goto cleanup;
    }
    mysql_data_seek(res, 0);
    while (nullptr != (row = mysql_fetch_row(res))) {
      unsigned const braces_length = 0;
      if (!row[3] || !*row[3]) continue;
      if (atoi(row[3]) < 1) break;
      if (row[4] && *row[4]) order_by_part = quote_name(row[4], buff, false);
#ifdef USABLE_EXPR_IN_SHOW_INDEX_BUG35273994
      else if (mysql_num_fields(res) > 14 && row[14] && *row[14]) {
        order_by_part = row[14];
        braces_length = 2;
      }
#endif
      else
        continue;
      if (end != result) end = my_stpcpy(end, ",");
      if (braces_length)
        end = strxmov(end, "(", order_by_part, ")", NullS);
      else
        end = my_stpcpy(end, order_by_part);
    }
  }

cleanup:
  if (res) mysql_free_result(res);

  return result;
}

/*
  Replace a substring

  SYNOPSIS
    replace
    ds_str      The string to search and perform the replace in
    search_str  The string to search for
    search_len  Length of the string to search for
    replace_str The string to replace with
    replace_len Length of the string to replace with

  RETURN
    0 String replaced
    1 Could not find search_str in str
*/

static int replace(DYNAMIC_STRING *ds_str, const char *search_str,
                   size_t search_len, const char *replace_str,
                   size_t replace_len) {
  DYNAMIC_STRING ds_tmp;
  const char *start = strstr(ds_str->str, search_str);
  if (!start) return 1;
  init_dynamic_string_checked(&ds_tmp, "", ds_str->length + replace_len);
  dynstr_append_mem_checked(&ds_tmp, ds_str->str, start - ds_str->str);
  dynstr_append_mem_checked(&ds_tmp, replace_str, replace_len);
  dynstr_append_checked(&ds_tmp, start + search_len);
  dynstr_set_checked(ds_str, ds_tmp.str);
  dynstr_free(&ds_tmp);
  return 0;
}

/**
  This function checks if GTIDs are enabled on the server.

  @param[in]          mysql_con         the connection to the server

  @retval             true              if GTIDs are enabled on the server

  @retval             false             if GTIDs are disabled on the server
*/
static bool get_gtid_mode(MYSQL *mysql_con) {
  MYSQL_RES *gtid_mode_res;
  MYSQL_ROW gtid_mode_row;
  bool gtid_mode = false;
  char *gtid_mode_val = nullptr;

  if (mysql_query_with_error_report(mysql_con, &gtid_mode_res,
                                    "SHOW VARIABLES LIKE 'gtid_mode'"))
    return false;

  gtid_mode_row = mysql_fetch_row(gtid_mode_res);

  /*
     gtid_mode_row is NULL for pre 5.6 versions. For versions >= 5.6,
     get the gtid_mode value from the second column.
  */
  gtid_mode_val = gtid_mode_row ? (char *)gtid_mode_row[1] : nullptr;
  gtid_mode = gtid_mode_val && strcmp(gtid_mode_val, "OFF") != 0;
  mysql_free_result(gtid_mode_res);

  return gtid_mode;
}

/**
  This function checks what should be the outputted terminology according to
  the server version

  @retval  SERVER   if no compatibility mode is needed

  @retval  BEFORE_8_0_23  if the server version is lower
  than 8.0.23

  @retval  BEFORE_8_2_0  if the server version is lower
  than 8.2.0
*/
static Output_as_version_mode get_output_as_version_mode() {
  Output_as_version_mode terminology_mode = Output_as_version_mode::SERVER;

  if (opt_server_version < FIRST_REPLICA_COMMAND_VERSION) {
    terminology_mode = Output_as_version_mode::BEFORE_8_0_23;
  } else if (opt_server_version < FIRST_SOURCE_COMMAND_VERSION) {
    terminology_mode = Output_as_version_mode::BEFORE_8_2_0;
  }

  return terminology_mode;
}

/**
  This method will set depending on the arguments given the server value for
  terminology_use_previous

  @param[in]  mysql_con   the connection to the server
  @param[in]  mode_to_set what mode to set

  @retval 1 if an query error ocurred, 0 otherwise.
*/
static int set_terminology_use_previous_session_value(
    MYSQL *mysql_con, Output_as_version_mode mode_to_set) {
  // If the server doesn't support previous terminology for events, do nothing
  if (opt_server_version < FIRST_SOURCE_COMMAND_VERSION) {
    return 0;
  }

  switch (mode_to_set) {
    case Output_as_version_mode::BEFORE_8_0_23:
    case Output_as_version_mode::BEFORE_8_2_0:
      /*
        For both before 8.0.23 and before 8.2.0 we only care about the
        the event output of SHOW CREATE EVENT so always setting BEFORE_8_2_0
        is ok
      */
      if (mysql_query(mysql_con,
                      "SET @@SESSION.terminology_use_previous = BEFORE_8_2_0"))
        return 1;
      break;
    case Output_as_version_mode::SERVER:
      // This means no mode is needed, so go to default
      if (mysql_query(mysql_con,
                      "SET @@SESSION.terminology_use_previous = NONE"))
        return 1;
      break;
    default:
      assert(0);
      break;
  }
  return 0;
}

/**
  This function sets the session binlog in the dump file.
  When --set-gtid-purged is used, this function is called to
  disable the session binlog and at the end of the dump, to restore
  the session binlog.

  @note: md_result_file should have been opened, before
         this function is called.

  @param[in]      flag          If false, disable binlog.
                                If true and binlog disabled previously,
                                restore the session binlog.
*/

static void set_session_binlog(bool flag) {
  static bool is_binlog_disabled = false;

  if (!flag && !is_binlog_disabled) {
    fprintf(md_result_file,
            "SET @MYSQLDUMP_TEMP_LOG_BIN = @@SESSION.SQL_LOG_BIN;\n");
    fprintf(md_result_file, "SET @@SESSION.SQL_LOG_BIN= 0;\n");
    is_binlog_disabled = true;
  } else if (flag && is_binlog_disabled) {
    fprintf(md_result_file,
            "SET @@SESSION.SQL_LOG_BIN = @MYSQLDUMP_TEMP_LOG_BIN;\n");
    is_binlog_disabled = false;
  }
}

/**
  This function gets the GTID_EXECUTED sets from the
  server and assigns those sets to GTID_PURGED in the
  dump file.

  @param[in]  mysql_con     connection to the server

  @retval     false         successfully printed GTID_PURGED sets
                             in the dump file.
  @retval     true          failed.

*/

static bool add_set_gtid_purged(MYSQL *mysql_con) {
  MYSQL_RES *gtid_purged_res;
  MYSQL_ROW gtid_set;
  ulonglong num_sets, idx;

  /* query to get the GTID_EXECUTED */
  if (mysql_query_with_error_report(mysql_con, &gtid_purged_res,
                                    "SELECT @@GLOBAL.GTID_EXECUTED"))
    return true;

  /* Proceed only if gtid_purged_res is non empty */
  if ((num_sets = mysql_num_rows(gtid_purged_res)) > 0) {
    if (opt_comments)
      fprintf(md_result_file,
              "\n--\n-- GTID state at the beginning of the backup \n--\n\n");

    const char *comment_suffix = "";
    if (opt_set_gtid_purged_mode == SET_GTID_PURGED_COMMENTED) {
      comment_suffix = "*/";
      fprintf(md_result_file, "/* SET @@GLOBAL.GTID_PURGED='+");
    } else {
      fprintf(md_result_file, "SET @@GLOBAL.GTID_PURGED=/*!80000 '+'*/ '");
    }

    /* formatting is not required, even for multiple gtid sets */
    for (idx = 0; idx < num_sets - 1; idx++) {
      gtid_set = mysql_fetch_row(gtid_purged_res);
      fprintf(md_result_file, "%s,", (char *)gtid_set[0]);
    }
    /* for the last set */
    gtid_set = mysql_fetch_row(gtid_purged_res);
    /* close the SET expression */
    fprintf(md_result_file, "%s';%s\n", (char *)gtid_set[0], comment_suffix);
  }
  mysql_free_result(gtid_purged_res);

  return false; /*success */
}

/**
  This function processes the opt_set_gtid_purged option.
  This function also calls set_session_binlog() function before
  setting the SET @@GLOBAL.GTID_PURGED in the output.

  @param[in]          mysql_con     the connection to the server

  @param[in]          is_gtid_enabled  true if server has gtid_mode on

  @retval             false         successful according to the value
                                    of opt_set_gtid_purged.
  @retval             true          fail.
*/

static bool process_set_gtid_purged(MYSQL *mysql_con, bool is_gtid_enabled) {
  if (opt_set_gtid_purged_mode == SET_GTID_PURGED_OFF)
    return false; /* nothing to be done */

  if (is_gtid_enabled) {
    /*
       For any gtid_mode !=OFF and irrespective of --set-gtid-purged
       being AUTO or ON,  add GTID_PURGED in the output.
    */
    if (opt_databases || !opt_alldbs || !opt_dump_triggers || !opt_routines ||
        !opt_events) {
      fprintf(stderr,
              "Warning: A partial dump from a server that has GTIDs will "
              "by default include the GTIDs of all transactions, even "
              "those that changed suppressed parts of the database. If "
              "you don't want to restore GTIDs, pass "
              "--set-gtid-purged=OFF. To make a complete dump, pass "
              "--all-databases --triggers --routines --events. \n");
    }

    if (!opt_single_transaction && !opt_lock_all_tables && !opt_source_data) {
      fprintf(stderr,
              "Warning: A dump from a server that has GTIDs "
              "enabled will by default include the GTIDs "
              "of all transactions, even those that were "
              "executed during its extraction and might "
              "not be represented in the dumped data. "
              "This might result in an inconsistent data dump. \n"
              "In order to ensure a consistent backup of the "
              "database, pass --single-transaction or "
              "--lock-all-tables or --source-data. \n");
    }

    set_session_binlog(false);
    if (add_set_gtid_purged(mysql_con)) {
      return true;
    }
  } else /* gtid_mode is off */
  {
    if (opt_set_gtid_purged_mode == SET_GTID_PURGED_ON ||
        opt_set_gtid_purged_mode == SET_GTID_PURGED_COMMENTED) {
      fprintf(stderr, "Error: Server has GTIDs disabled.\n");
      return true;
    }
  }

  return false;
}

/*
  Getting VIEW structure

  SYNOPSIS
    get_view_structure()
    table   view name
    db      db name

  RETURN
    0 OK
    1 ERROR
*/

static bool get_view_structure(char *table, char *db) {
  MYSQL_RES *table_res;
  MYSQL_ROW row;
  MYSQL_FIELD *field;
  char *result_table, *opt_quoted_table;
  char table_buff[NAME_LEN * 2 + 3];
  char table_buff2[NAME_LEN * 2 + 3];
  char table_string_buff[NAME_LEN * 2 + 3];
  char db_string_buff[NAME_LEN * 2 + 3];
  char query[QUERY_LENGTH];
  FILE *sql_file = md_result_file;
  DBUG_TRACE;

  if (opt_no_create_info) /* Don't write table creation info */
    return false;

  verbose_msg("-- Retrieving view structure for table %s...\n", table);

  result_table = quote_name(table, table_buff, true);
  opt_quoted_table = quote_name(table, table_buff2, false);
  if (((ulong)-1 == mysql_real_escape_string_quote(mysql, table_string_buff,
                                                   table, strlen(table),
                                                   '\'')) ||
      ((ulong)-1 == mysql_real_escape_string_quote(mysql, db_string_buff, db,
                                                   strlen(db), '\''))) {
    DB_error(mysql,
             "when trying to quote table and db names when dumping views.");
    return true;
  }

  if (switch_character_set_results(mysql, "binary")) return true;

  snprintf(query, sizeof(query), "SHOW CREATE TABLE %s", result_table);

  if (mysql_query_with_error_report(mysql, &table_res, query)) {
    switch_character_set_results(mysql, default_charset);
    return false;
  }

  /* Check if this is a view */
  field = mysql_fetch_field_direct(table_res, 0);
  if (strcmp(field->name, "View") != 0) {
    switch_character_set_results(mysql, default_charset);
    verbose_msg("-- It's base table, skipped\n");
    mysql_free_result(table_res);
    return false;
  }

  /* If requested, open separate .sql file for this view */
  if (path) {
    if (!(sql_file = open_sql_file_for_table(table, O_WRONLY))) {
      mysql_free_result(table_res);
      return true;
    }

    write_header(sql_file, db);
  }

  bool freemem = false;
  char const *text = fix_identifier_with_newline(result_table, &freemem);
  print_comment(sql_file, false,
                "\n--\n-- Final view structure for view %s\n--\n\n", text);
  if (freemem) my_free(const_cast<char *>(text));

  verbose_msg("-- Dropping the temporary view structure created\n");
  fprintf(sql_file, "/*!50001 DROP VIEW IF EXISTS %s*/;\n", opt_quoted_table);

  snprintf(query, sizeof(query),
           "SELECT CHECK_OPTION, DEFINER, SECURITY_TYPE, "
           "       CHARACTER_SET_CLIENT, COLLATION_CONNECTION "
           "FROM information_schema.views "
           "WHERE table_name='%s' AND table_schema='%s'",
           table_string_buff, db_string_buff);

  if (mysql_query(mysql, query)) {
    /*
      Use the raw output from SHOW CREATE TABLE if
       information_schema query fails.
     */
    row = mysql_fetch_row(table_res);
    fprintf(sql_file, "/*!50001 %s */;\n", row[1]);
    check_io(sql_file);
    mysql_free_result(table_res);
  } else {
    char *ptr;
    ulong *lengths;
    char search_buf[256], replace_buf[256];
    ulong search_len, replace_len;
    DYNAMIC_STRING ds_view;

    /* Save the result of SHOW CREATE TABLE in ds_view */
    row = mysql_fetch_row(table_res);
    lengths = mysql_fetch_lengths(table_res);
    init_dynamic_string_checked(&ds_view, row[1], lengths[1] + 1);
    mysql_free_result(table_res);

    /* Get the result from "select ... information_schema" */
    if (!(table_res = mysql_store_result(mysql)) ||
        !(row = mysql_fetch_row(table_res))) {
      if (table_res) mysql_free_result(table_res);
      dynstr_free(&ds_view);
      DB_error(
          mysql,
          "when trying to save the result of SHOW CREATE TABLE in ds_view.");
      return true;
    }

    lengths = mysql_fetch_lengths(table_res);

    /*
      "WITH %s CHECK OPTION" is available from 5.0.2
      Surround it with !50002 comments
    */
    if (strcmp(row[0], "NONE") != 0) {
      ptr = search_buf;
      search_len =
          (ulong)(strxmov(ptr, "WITH ", row[0], " CHECK OPTION", NullS) - ptr);
      ptr = replace_buf;
      replace_len = (ulong)(strxmov(ptr, "*/\n/*!50002 WITH ", row[0],
                                    " CHECK OPTION", NullS) -
                            ptr);
      replace(&ds_view, search_buf, search_len, replace_buf, replace_len);
    }

    /*
      "DEFINER=%s SQL SECURITY %s" is available from 5.0.13
      Surround it with !50013 comments
    */
    {
      size_t user_name_len;
      char user_name_str[USERNAME_LENGTH + 1];
      char quoted_user_name_str[USERNAME_LENGTH * 2 + 3];
      size_t host_name_len;
      char host_name_str[HOSTNAME_LENGTH + 1];
      char quoted_host_name_str[HOSTNAME_LENGTH * 2 + 3];

      parse_user(row[1], lengths[1], user_name_str, &user_name_len,
                 host_name_str, &host_name_len);

      ptr = search_buf;
      search_len =
          (ulong)(strxmov(
                      ptr, "DEFINER=",
                      quote_name(user_name_str, quoted_user_name_str, false),
                      "@",
                      quote_name(host_name_str, quoted_host_name_str, false),
                      " SQL SECURITY ", row[2], NullS) -
                  ptr);
      ptr = replace_buf;
      replace_len =
          (ulong)(strxmov(
                      ptr, "*/\n/*!50013 DEFINER=",
                      quote_name(user_name_str, quoted_user_name_str, false),
                      "@",
                      quote_name(host_name_str, quoted_host_name_str, false),
                      " SQL SECURITY ", row[2], " */\n/*!50001", NullS) -
                  ptr);
      replace(&ds_view, search_buf, search_len, replace_buf, replace_len);
    }

    /* Dump view structure to file */

    fprintf(
        sql_file,
        "/*!50001 SET @saved_cs_client          = @@character_set_client */;\n"
        "/*!50001 SET @saved_cs_results         = @@character_set_results */;\n"
        "/*!50001 SET @saved_col_connection     = @@collation_connection */;\n"
        "/*!50001 SET character_set_client      = %s */;\n"
        "/*!50001 SET character_set_results     = %s */;\n"
        "/*!50001 SET collation_connection      = %s */;\n"
        "/*!50001 %s */;\n"
        "/*!50001 SET character_set_client      = @saved_cs_client */;\n"
        "/*!50001 SET character_set_results     = @saved_cs_results */;\n"
        "/*!50001 SET collation_connection      = @saved_col_connection */;\n",
        (const char *)row[3], (const char *)row[3], (const char *)row[4],
        (const char *)ds_view.str);

    check_io(sql_file);
    mysql_free_result(table_res);
    dynstr_free(&ds_view);
  }

  if (switch_character_set_results(mysql, default_charset)) return true;

  /* If a separate .sql file was opened, close it now */
  if (sql_file != md_result_file) {
    fputs("\n", sql_file);
    write_footer(sql_file);
    my_fclose(sql_file, MYF(MY_WME));
  }
  return false;
}

static bool fetch_users_list_if_include_is_empty() {
  if (include_user->size() != 0) return false;

  const char *enum_users_query =
      "SELECT CONCAT('\\'',user,'\\'@\\'',host,'\\''),"
      " CONCAT(QUOTE(user),'@',QUOTE(host)), CONCAT(user,'@',host)"
      "FROM mysql.user";
  MYSQL_RES *enum_users_res;
  MYSQL_ROW enum_users_row;
  if (mysql_query_with_error_report(mysql, &enum_users_res, enum_users_query))
    return true;
  if (!enum_users_res) return true;
  auto enum_users_res_guard =
      create_scope_guard([&] { mysql_free_result(enum_users_res); });

  while ((enum_users_row = mysql_fetch_row(enum_users_res)) != nullptr) {
    const char *user_name;
    if (strcmp(enum_users_row[0], enum_users_row[1]) ||
        strpbrk(enum_users_row[0], ".%- "))
      user_name = enum_users_row[1];
    else
      user_name = enum_users_row[2];
    include_user->emplace(user_name);
  }
  return false;
}

static void retract_excluded_users() {
  for (; !exclude_user->empty(); exclude_user->pop_front()) {
    if (0 == include_user->erase(exclude_user->front())) {
      fprintf(stderr,
              "Warning: --exclude-user=%s didn't match any included account\n",
              exclude_user->front().c_str());
    }
  }
}

/**
  @brief Do a logical dump of all ACL data: users, roles, grants

  @param sql_file The output SQL file to write to
  @retval false success
  @retval true failure
*/
static bool dump_users(FILE *sql_file) {
  for (auto user : *include_user) {
    if (opt_add_drop_user) fprintf(sql_file, "DROP USER %s;\n", user.c_str());

    /* execute and dump SHOW CREATE USER */
    MYSQL_RES *res;
    MYSQL_ROW row;
    std::string query = "SHOW CREATE USER " + user;
    if (mysql_query_with_error_report(mysql, &res, query.c_str())) return true;
    if (!res) return true;
    auto res_guard = create_scope_guard([&] { mysql_free_result(res); });
    if (nullptr == (row = mysql_fetch_row(res))) {
      DB_error(mysql, "retrieving SHOW CREATE USER result");
      return true;
    }
    fprintf(sql_file, "%s;\n", row[0]);
  }
  return false;
}

static bool dump_grants(FILE *sql_file) {
  for (auto user : *include_user) {
    /* execute and dump SHOW GRANTS */
    MYSQL_RES *res;
    MYSQL_ROW row;
    std::string query = "SHOW GRANTS FOR " + user;
    if (mysql_query_with_error_report(mysql, &res, query.c_str())) return true;
    if (!res) return true;
    auto res_guard = create_scope_guard([&] { mysql_free_result(res); });
    while (nullptr != (row = mysql_fetch_row(res))) {
      fprintf(sql_file, "%s;\n", row[0]);
    }
    if (mysql_errno(mysql) != 0) {
      DB_error(mysql, "retrieving SHOW CREATE USER result");
      return true;
    }
  }
  return false;
}

/*
  The following functions are wrappers for the dynamic string functions
  and if they fail, the wrappers will terminate the current process.
*/

#define DYNAMIC_STR_ERROR_MSG "Couldn't perform DYNAMIC_STRING operation"

static void init_dynamic_string_checked(DYNAMIC_STRING *str,
                                        const char *init_str,
                                        size_t init_alloc) {
  if (init_dynamic_string(str, init_str, init_alloc))
    die(EX_MYSQLERR, DYNAMIC_STR_ERROR_MSG);
}

static void dynstr_append_checked(DYNAMIC_STRING *dest, const char *src) {
  if (dynstr_append(dest, src)) die(EX_MYSQLERR, DYNAMIC_STR_ERROR_MSG);
}

static void dynstr_set_checked(DYNAMIC_STRING *str, const char *init_str) {
  if (dynstr_set(str, init_str)) die(EX_MYSQLERR, DYNAMIC_STR_ERROR_MSG);
}

static void dynstr_append_mem_checked(DYNAMIC_STRING *str, const char *append,
                                      size_t length) {
  if (dynstr_append_mem(str, append, length))
    die(EX_MYSQLERR, DYNAMIC_STR_ERROR_MSG);
}

static void dynstr_realloc_checked(DYNAMIC_STRING *str,
                                   size_t additional_size) {
  if (dynstr_realloc(str, additional_size))
    die(EX_MYSQLERR, DYNAMIC_STR_ERROR_MSG);
}

int main(int argc, char **argv) {
  bool server_with_gtids_and_opt_purge_not_off = false;
  bool server_has_gtid_enabled = false;
  char bin_log_name[FN_REFLEN];
  int exit_code, md_result_fd = 0;
  MY_INIT("mysqldump");

  default_charset = mysql_universal_client_charset;

  exit_code = get_options(&argc, &argv);
  if (exit_code) {
    free_resources();
    exit(exit_code);
  }

  /*
    Disable comments in xml mode if 'comments' option is not explicitly used.
  */
  if (opt_xml && !opt_comments_used) opt_comments = false;

  if (log_error_file) {
    if (!(stderror_file = freopen(log_error_file, "a+", stderr))) {
      free_resources();
      exit(EX_MYSQLERR);
    }
  }

  if (connect_to_db(current_host, current_user)) {
    free_resources();
    exit(EX_MYSQLERR);
  }

  stats_tables_included = is_innodb_stats_tables_included(argc, argv);

  if (!path) write_header(md_result_file, *argv);

  opt_server_version = mysql_get_server_version(mysql);

  if (opt_output_as_version_mode == Output_as_version_mode::SERVER) {
    opt_output_as_version_mode = get_output_as_version_mode();
  }
  // Set the terminology mode in the server if we are outputting events
  if (opt_events && set_terminology_use_previous_session_value(
                        mysql, opt_output_as_version_mode)) {
    fprintf(
        stderr,
        " Warning: Could not set terminology mode for outputting events. \n");
  }

  if (opt_replica_data && do_stop_replica_sql(mysql)) goto err;

  server_has_gtid_enabled = get_gtid_mode(mysql);

  server_with_gtids_and_opt_purge_not_off =
      (server_has_gtid_enabled &&
       (opt_set_gtid_purged_mode != SET_GTID_PURGED_OFF));

  if ((opt_lock_all_tables || opt_source_data ||
       (opt_single_transaction &&
        (flush_logs || server_with_gtids_and_opt_purge_not_off))) &&
      do_flush_tables_read_lock(mysql))
    goto err;

  /*
    Flush logs before starting transaction since
    this causes implicit commit starting mysql-5.5.
  */
  if (opt_lock_all_tables || opt_source_data ||
      (opt_single_transaction &&
       (flush_logs || server_with_gtids_and_opt_purge_not_off)) ||
      opt_delete_source_logs) {
    if (flush_logs || opt_delete_source_logs) {
      if (mysql_query(mysql, "FLUSH /*!40101 LOCAL */ LOGS")) {
        DB_error(mysql, "when doing refresh");
        goto err;
      }
      verbose_msg("-- main : logs flushed successfully!\n");
    }

    /* Not anymore! That would not be sensible. */
    flush_logs = false;
  }

  if (opt_delete_source_logs) {
    if (get_bin_log_name(mysql, bin_log_name, sizeof(bin_log_name))) goto err;
  }

  /* Start the transaction */
  if (opt_single_transaction && start_transaction(mysql)) goto err;

  /* Add STOP REPLICA to beginning of dump */
  if (opt_replica_apply && add_stop_replica()) goto err;

  /* Process opt_set_gtid_purged and add SET @@GLOBAL.GTID_PURGED if required.
   */
  if (process_set_gtid_purged(mysql, server_has_gtid_enabled)) goto err;

  if (opt_dump_users) {
    fetch_users_list_if_include_is_empty();
    retract_excluded_users();
    dump_users(md_result_file);
  }

  if (opt_source_data && do_show_binary_log_status(mysql)) goto err;
  if (opt_replica_data && do_show_replica_status(mysql)) goto err;
  if (opt_single_transaction &&
      do_unlock_tables(mysql)) /* unlock but no commit! */
    goto err;

  if (column_statistics &&
      mysql_get_server_version(mysql) < FIRST_COLUMN_STATISTICS_VERSION) {
    column_statistics = false;
    fprintf(stderr,
            "-- Warning: column statistics not supported by the server.\n");
  }

  if (opt_alltspcs) dump_all_tablespaces();

  if (opt_alldbs) {
    if (!opt_alltspcs && !opt_notspcs) dump_all_tablespaces();
    dump_all_databases();
  } else {
    // Check all arguments meet length condition. Currently database and table
    // names are limited to NAME_LEN bytes and stack-based buffers assumes
    // that escaped name will be not longer than NAME_LEN*2 + 2 bytes long.
    int argument;
    for (argument = 0; argument < argc; argument++) {
      const size_t argument_length = strlen(argv[argument]);
      if (argument_length > NAME_LEN) {
        die(EX_CONSCHECK,
            "[ERROR] Argument '%s' is too long, it cannot be "
            "name for any table or database.\n",
            argv[argument]);
      }
    }

    if (argc > 1 && !opt_databases) {
      /* Only one database and selected table(s) */
      if (!opt_alltspcs && !opt_notspcs)
        dump_tablespaces_for_tables(*argv, (argv + 1), (argc - 1));
      dump_selected_tables(*argv, (argv + 1), (argc - 1));
    } else {
      /* One or more databases, all tables */
      if (!opt_alltspcs && !opt_notspcs) dump_tablespaces_for_databases(argv);
      dump_databases(argv);
    }
  }

  if (opt_dump_users) dump_grants(md_result_file);

  /* if --dump-replica , start the replica sql thread */
  if (opt_replica_data && do_start_replica_sql(mysql)) goto err;

  /*
    if --set-gtid-purged, restore binlog at the end of the session
    if required.
  */
  set_session_binlog(true);

  /* add 'START REPLICA' to end of dump */
  if (opt_replica_apply && add_replica_statements()) goto err;

  if (md_result_file) md_result_fd = my_fileno(md_result_file);

  /*
     Ensure dumped data flushed.
     First we will flush the file stream data to kernel buffers with fflush().
     Second we will flush the kernel buffers data to physical disk file with
     my_sync(), this will make sure the data successfully dumped to disk file.
     fsync() fails with EINVAL if stdout is not redirected to any file, hence
     MY_IGNORE_BADFD is passed to ignore that error.
  */
  if (md_result_file &&
      (fflush(md_result_file) || my_sync(md_result_fd, MYF(MY_IGNORE_BADFD)))) {
    if (!first_error) first_error = EX_MYSQLERR;
    goto err;
  }
  /* everything successful, purge the old logs files */
  if (opt_delete_source_logs && purge_bin_logs_to(mysql, bin_log_name))
    goto err;

#if defined(_WIN32)
  my_free(shared_memory_base_name);
#endif
  /*
    No reason to explicitly COMMIT the transaction, neither to explicitly
    UNLOCK TABLES: these will be automatically be done by the server when we
    disconnect now. Saves some code here, some network trips, adds nothing to
    server.
  */
err:
  dbDisconnect(current_host);
  if (!path) write_footer(md_result_file);
  free_resources();

  if (stderror_file) fclose(stderror_file);

  return (first_error);
} /* main */
