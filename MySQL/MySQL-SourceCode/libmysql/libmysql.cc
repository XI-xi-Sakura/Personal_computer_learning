/* Copyright (c) 2000, 2025, Oracle and/or its affiliates.

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

   Without limiting anything contained in the foregoing, this file,
   which is part of C Driver for MySQL (Connector/C), is also subject to the
   Universal FOSS Exception, version 1.0, a copy of which can be found at
   http://oss.oracle.com/licenses/universal-foss-exception.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "my_config.h"

#include <fcntl.h>
#include <sys/types.h>
#include <climits>
#include <cmath>

#include "dig_vec.h"
#include "my_alloc.h"
#include "my_sys.h"
#include "my_time.h"
#include "mysql/strings/m_ctype.h"
#include "mysys_err.h"
#ifndef _WIN32
#include <netdb.h>
#endif
#include <sys/stat.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <algorithm>
#include <vector>

#include "errmsg.h"
#include "m_string.h"
#include "my_byteorder.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_double2ulonglong.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_macros.h"
#include "my_pointer_arithmetic.h"
#include "my_thread_local.h"
#include "mysql.h"
#include "mysql/service_mysql_alloc.h"
#include "mysql/strings/dtoa.h"
#include "mysql/strings/int2str.h"
#include "mysql/strings/my_strtoll10.h"
#include "mysql_com.h"
#include "mysql_version.h"
#include "mysqld_error.h"
#include "nulls.h"
#include "string_with_len.h"
#include "strmake.h"
#include "strxnmov.h"
#include "template_utils.h"
#include "violite.h"

#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#ifdef HAVE_POLL
#include <poll.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#if !defined(_WIN32)
#include "my_thread.h" /* because of signal()	*/
#endif
#ifndef INADDR_NONE
#define INADDR_NONE -1
#endif

#include <memory>
#include <set>

#include "../sql-common/client_extensions_macros.h"
#include "client_settings.h"
#include "mysql_trace.h"
#include "sql_common.h"

static void append_wild(char *to, char *end, const char *wild);

static bool mysql_client_init = false;
static bool org_my_init_done = false;

struct MYSQL_STMT_EXT {
  MEM_ROOT fields_mem_root;
  struct {
    /**
     * Stores the size of array of names for statement bind parameters.
     * The size should match the total sum of the unnamed and named bind params
     * for this statement, with name entries for the unnamed parameters being
     * set to nullptr. This differs from stmt->param_count that only
     * counts unnamed parameters calculated by counting parameter placeholders
     * during the statement prepare.
     **/
    uint n_params;
    char **names;
    MEM_ROOT mem_root; /* for bind params and names only */
#ifndef NDEBUG
    std::set<void *> *allocations;
#endif
  } bind_data;
};

/*
  Initialize the MySQL client library

  SYNOPSIS
    mysql_server_init()

  NOTES
    Should be called before doing any other calls to the MySQL
    client library to initialize thread specific variables etc.
    It's called by mysql_init() to ensure that things will work for
    old not threaded applications that doesn't call mysql_server_init()
    directly.

  RETURN
    0  ok
    1  could not initialize environment (out of memory or thread keys)
*/

int STDCALL mysql_server_init(int argc [[maybe_unused]],
                              char **argv [[maybe_unused]],
                              char **groups [[maybe_unused]]) {
  int result = 0;
  if (!mysql_client_init) {
    mysql_client_init = true;
    org_my_init_done = my_init_done;
    if (my_init()) /* Will init threads */
      return 1;
    init_client_errs();
    if (mysql_client_plugin_init()) return 1;
    ssl_start();

    if (!mysql_port) {
      char *env;
      struct servent *serv_ptr [[maybe_unused]];

      mysql_port = MYSQL_PORT;

      /*
        if builder specifically requested a default port, use that
        (even if it coincides with our factory default).
        only if they didn't do we check /etc/services (and, failing
        on that, fall back to the factory default of 3306).
        either default can be overridden by the environment variable
        MYSQL_TCP_PORT, which in turn can be overridden with command
        line options.
      */

#if MYSQL_PORT_DEFAULT == 0
      if ((serv_ptr = getservbyname("mysql", "tcp")))
        mysql_port = (uint)ntohs((ushort)serv_ptr->s_port);
#endif
      if ((env = getenv("MYSQL_TCP_PORT"))) mysql_port = (uint)atoi(env);
    }

    if (!mysql_unix_port) {
      char *env;
#ifdef _WIN32
      mysql_unix_port = const_cast<char *>(MYSQL_NAMEDPIPE);
#else
      mysql_unix_port = const_cast<char *>(MYSQL_UNIX_ADDR);
#endif
      if ((env = getenv("MYSQL_UNIX_PORT"))) mysql_unix_port = env;
    }
    mysql_debug(NullS);
#if defined(SIGPIPE) && !defined(_WIN32)
    (void)signal(SIGPIPE, SIG_IGN);
#endif
  } else
    result = (int)my_thread_init(); /* Init if new thread */
  return result;
}

/*
  Free all memory and resources used by the client library

  NOTES
    When calling this there should not be any other threads using
    the library.

    To make things simpler when used with windows dll's (which calls this
    function automatically), it's safe to call this function multiple times.
*/

void STDCALL mysql_server_end() {
  if (!mysql_client_init) return;

  mysql_client_plugin_deinit();

  finish_client_errs();
  vio_end();

  /* If library called my_init(), free memory allocated by it */
  if (!org_my_init_done) {
    my_end(0);
  } else {
    mysql_thread_end();
  }

  mysql_client_init = org_my_init_done = false;
}

bool STDCALL mysql_thread_init() { return my_thread_init(); }

void STDCALL mysql_thread_end() { my_thread_end(); }

/*
  Expand wildcard to a sql string
*/

static void append_wild(char *to, char *end, const char *wild) {
  end -= 5; /* Some extra */
  if (wild && wild[0]) {
    to = my_stpcpy(to, " like '");
    while (*wild && to < end) {
      if (*wild == '\\' || *wild == '\'') *to++ = '\\';
      *to++ = *wild++;
    }
    if (*wild)     /* Too small buffer */
      *to++ = '%'; /* Nicer this way */
    to[0] = '\'';
    to[1] = 0;
  }
}

/**************************************************************************
  Init debugging if MYSQL_DEBUG environment variable is found
**************************************************************************/

void STDCALL mysql_debug(const char *debug [[maybe_unused]]) {
#ifndef NDEBUG
  char *env;
  if (debug) {
    DBUG_PUSH(debug);
  } else if ((env = getenv("MYSQL_DEBUG"))) {
    DBUG_PUSH(env);
#if !defined(_WINVER) && !defined(WINVER)
    puts("\n-------------------------------------------------------");
    puts("MYSQL_DEBUG found. libmysql started with the following:");
    puts(env);
    puts("-------------------------------------------------------\n");
#else
    {
      char buff[80];
      buff[sizeof(buff) - 1] = 0;
      strxnmov(buff, sizeof(buff) - 1, "libmysql: ", env, NullS);
      MessageBox(nullptr, "Debugging variable MYSQL_DEBUG used", buff, MB_OK);
    }
#endif
  }
#endif
}

/**************************************************************************
  Change user and database
**************************************************************************/

bool STDCALL mysql_change_user(MYSQL *mysql, const char *user,
                               const char *passwd, const char *db) {
  int rc;
  CHARSET_INFO *saved_cs = mysql->charset;
  char *saved_user = mysql->user;
  char *saved_passwd = mysql->passwd;
  char *saved_db = mysql->db;

  DBUG_TRACE;

  /* Get the connection-default character set. */

  if (mysql_init_character_set(mysql)) {
    mysql->charset = saved_cs;
    return true;
  }

  /*
    Use an empty string instead of NULL.
    Alloc user and password on heap because mysql_reconnect()
    calls mysql_close() on success.
  */
  mysql->user = my_strdup(PSI_NOT_INSTRUMENTED, user ? user : "", MYF(MY_WME));
  mysql->passwd =
      my_strdup(PSI_NOT_INSTRUMENTED, passwd ? passwd : "", MYF(MY_WME));
  mysql->db = nullptr;

  rc = run_plugin_auth(mysql, nullptr, 0, nullptr, db);

  MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);

  /*
    The server will close all statements no matter was the attempt
    to change user successful or not.
  */
  mysql_detach_stmt_list(&mysql->stmts, "mysql_change_user");
  if (rc == 0) {
    /* Free old connect information */
    my_free(saved_user);
    my_free(saved_passwd);
    my_free(saved_db);

    /* alloc new connect information */
    if (!mysql->db)
      mysql->db =
          db ? my_strdup(PSI_NOT_INSTRUMENTED, db, MYF(MY_WME)) : nullptr;
  } else {
    /* Free temporary connect information */
    my_free(mysql->user);
    my_free(mysql->passwd);
    my_free(mysql->db);

    /* Restore saved state */
    mysql->charset = saved_cs;
    mysql->user = saved_user;
    mysql->passwd = saved_passwd;
    mysql->db = saved_db;
  }

  return rc;
}

#if defined(HAVE_GETPWUID) && defined(NO_GETPWUID_DECL)
struct passwd *getpwuid(uid_t);
char *getlogin(void);
#endif

#if !defined(_WIN32)

void read_user_name(char *name) {
  DBUG_TRACE;
  if (geteuid() == 0)
    (void)my_stpcpy(name, "root"); /* allow use of surun */
  else {
#ifdef HAVE_GETPWUID
    struct passwd *skr;
    const char *str;
    if ((str = getlogin()) == nullptr) {
      if ((skr = getpwuid(geteuid())) != nullptr)
        str = skr->pw_name;
      else if (!(str = getenv("USER")) && !(str = getenv("LOGNAME")) &&
               !(str = getenv("LOGIN")))
        str = "UNKNOWN_USER";
    }
    (void)strmake(name, str, USERNAME_LENGTH);
#elif HAVE_CUSERID
    (void)cuserid(name);
#else
    my_stpcpy(name, "UNKNOWN_USER");
#endif
  }
}

#else /* If Windows */

void read_user_name(char *name) {
  char *str = getenv("USER"); /* ODBC will send user variable */
  strmake(name, str ? str : "ODBC", USERNAME_LENGTH);
}

#endif

/**
  Checks if the file name supplied by the server is a valid name.

  Name is valid if it's either equal to or starts with the value stored
  in the mysql options.
  If the value in the options is NULL then no name is valid.

  Note that we rely that the options name, if supplied, is normalized before
  being stored.

  @note Will allocate the extension if not already allocated

  @param options the options to read the load_data_file_from.
  @param net_filename the path to check
  @retval true the name is valid
  @retval false the name is invalid
*/
static bool is_valid_local_infile_name(st_mysql_options *options,
                                       const char *net_filename) {
  char buff1[FN_REFLEN], buff2[FN_REFLEN];

  ENSURE_EXTENSIONS_PRESENT(options);

  // null load_data_dir means no exceptions (compatibility)
  if (options->extension->load_data_dir == nullptr) return false;

  // make fully qualified name
  if (my_realpath(buff1, net_filename, 0)) return false;

  // with uniform directory separators
  convert_dirname(buff2, buff1, NullS);

  /* if the name supplied starts with load_data_dir accept it */
  const int ret = strncmp(options->extension->load_data_dir, buff2,
                          strlen(options->extension->load_data_dir));
  return ret == 0;
}

bool handle_local_infile(MYSQL *mysql, const char *net_filename) {
  bool result = true;
  const uint packet_length = MY_ALIGN(mysql->net.max_packet - 16, IO_SIZE);
  NET *net = &mysql->net;
  int readcount;
  void *li_ptr; /* pass state to local_infile functions */
  char *buf;    /* buffer to be filled by local_infile_read */
  struct st_mysql_options *options = &mysql->options;
  DBUG_TRACE;

  /*
    Throw an error if --local-infile is not specified and the
    file requested is not "safe" (i.e. within the supplied directory
    to MYSQL_OPT_LOAD_DATA_LOCAL_DIR.
    If --local-infile is specified then no need to check the file name.
  */
  if (!(mysql->options.client_flag & CLIENT_LOCAL_FILES) &&
      !is_valid_local_infile_name(&(mysql->options), net_filename)) {
    MYSQL_TRACE(SEND_FILE, mysql, (0, nullptr));
    (void)my_net_write(net, (const uchar *)"", 0); /* Server needs one packet */
    net_flush(net);
    MYSQL_TRACE(PACKET_SENT, mysql, (0));
    set_mysql_error(mysql, CR_LOAD_DATA_LOCAL_INFILE_REJECTED,
                    unknown_sqlstate);
    return true;
  }

  /* check that we've got valid callback functions */
  if (!(options->local_infile_init && options->local_infile_read &&
        options->local_infile_end && options->local_infile_error)) {
    /* if any of the functions is invalid, set the default */
    mysql_set_local_infile_default(mysql);
  }

  /* copy filename into local memory and allocate read buffer */
  if (!(buf = pointer_cast<char *>(
            my_malloc(PSI_NOT_INSTRUMENTED, packet_length, MYF(0))))) {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    return true;
  }

  /* initialize local infile (open file, usually) */
  if ((*options->local_infile_init)(&li_ptr, net_filename,
                                    options->local_infile_userdata)) {
    MYSQL_TRACE(SEND_FILE, mysql, (0, nullptr));
    (void)my_net_write(net, (const uchar *)"", 0); /* Server needs one packet */
    net_flush(net);
    MYSQL_TRACE(PACKET_SENT, mysql, (0));
    my_stpcpy(net->sqlstate, unknown_sqlstate);
    net->last_errno = (*options->local_infile_error)(
        li_ptr, net->last_error, sizeof(net->last_error) - 1);
    MYSQL_TRACE(ERROR, mysql, ());
    goto err;
  }

  /* read blocks of data from local infile callback */
  while ((readcount =
              (*options->local_infile_read)(li_ptr, buf, packet_length)) > 0) {
    MYSQL_TRACE(SEND_FILE, mysql,
                ((size_t)readcount, (const unsigned char *)buf));
    if (my_net_write(net, (uchar *)buf, readcount)) {
      DBUG_PRINT(
          "error",
          ("Lost connection to MySQL server during LOAD DATA of local file"));
      set_mysql_error(mysql, CR_SERVER_LOST, unknown_sqlstate);
      goto err;
    }
    MYSQL_TRACE(PACKET_SENT, mysql, (static_cast<size_t>(readcount)));
  }

  /* Send empty packet to mark end of file */
  MYSQL_TRACE(SEND_FILE, mysql, (0, nullptr));
  if (my_net_write(net, (const uchar *)"", 0) || net_flush(net)) {
    set_mysql_error(mysql, CR_SERVER_LOST, unknown_sqlstate);
    goto err;
  }
  MYSQL_TRACE(PACKET_SENT, mysql, (0));

  if (readcount < 0) {
    net->last_errno = (*options->local_infile_error)(
        li_ptr, net->last_error, sizeof(net->last_error) - 1);
    MYSQL_TRACE(ERROR, mysql, ());
    goto err;
  }

  result = false; /* Ok */

err:
  /* free up memory allocated with _init, usually */
  (*options->local_infile_end)(li_ptr);
  my_free(buf);
  return result;
}

/****************************************************************************
  Default handlers for LOAD LOCAL INFILE
****************************************************************************/

struct default_local_infile_data {
  int fd;
  int error_num;
  const char *filename;
  char error_msg[LOCAL_INFILE_ERROR_LEN];
};

/*
  Open file for LOAD LOCAL INFILE

  SYNOPSIS
    default_local_infile_init()
    ptr			Store pointer to internal data here
    filename		File name to open. This may be in unix format !


  NOTES
    Even if this function returns an error, the load data interface
    guarantees that default_local_infile_end() is called.

  RETURN
    0	ok
    1	error
*/

static int default_local_infile_init(void **ptr, const char *filename,
                                     void *userdata [[maybe_unused]]) {
  default_local_infile_data *data;
  char tmp_name[FN_REFLEN];

  if (!(*ptr = data = ((default_local_infile_data *)my_malloc(
            PSI_NOT_INSTRUMENTED, sizeof(default_local_infile_data), MYF(0)))))
    return 1; /* out of memory */

  data->error_msg[0] = 0;
  data->error_num = 0;
  data->filename = filename;

  fn_format(tmp_name, filename, "", "", MY_UNPACK_FILENAME);
  if ((data->fd = my_open(tmp_name, O_RDONLY, MYF(0))) < 0) {
    char errbuf[MYSYS_STRERROR_SIZE];
    data->error_num = my_errno();
    snprintf(data->error_msg, sizeof(data->error_msg) - 1, EE(EE_FILENOTFOUND),
             tmp_name, data->error_num,
             my_strerror(errbuf, sizeof(errbuf), data->error_num));
    return 1;
  }
  return 0; /* ok */
}

/*
  Read data for LOAD LOCAL INFILE

  SYNOPSIS
    default_local_infile_read()
    ptr			Points to handle allocated by _init
    buf			Read data here
    buf_len		Amount of data to read

  RETURN
    > 0		number of bytes read
    == 0	End of data
    < 0		Error
*/

static int default_local_infile_read(void *ptr, char *buf, uint buf_len) {
  int count;
  auto *data = (default_local_infile_data *)ptr;

  if ((count = (int)my_read(data->fd, (uchar *)buf, buf_len, MYF(0))) < 0) {
    char errbuf[MYSYS_STRERROR_SIZE];
    data->error_num = EE_READ; /* the errmsg for not entire file read */
    snprintf(data->error_msg, sizeof(data->error_msg) - 1, EE(EE_READ),
             data->filename, my_errno(),
             my_strerror(errbuf, sizeof(errbuf), my_errno()));
  }
  return count;
}

/*
  Read data for LOAD LOCAL INFILE

  SYNOPSIS
    default_local_infile_end()
    ptr			Points to handle allocated by _init
                        May be NULL if _init failed!

  RETURN
*/

static void default_local_infile_end(void *ptr) {
  auto *data = (default_local_infile_data *)ptr;
  if (data) /* If not error on open */
  {
    if (data->fd >= 0) my_close(data->fd, MYF(MY_WME));
    my_free(ptr);
  }
}

/*
  Return error from LOAD LOCAL INFILE

  SYNOPSIS
    default_local_infile_end()
    ptr			Points to handle allocated by _init
                        May be NULL if _init failed!
    error_msg		Store error text here
    error_msg_len	Max length of error_msg

  RETURN
    error message number
*/

static int default_local_infile_error(void *ptr, char *error_msg,
                                      uint error_msg_len) {
  auto *data = (default_local_infile_data *)ptr;
  if (data) /* If not error on open */
  {
    strmake(error_msg, data->error_msg, error_msg_len);
    return data->error_num;
  }
  /* This can only happen if we got error on malloc of handle */
  my_stpcpy(error_msg, ER_CLIENT(CR_OUT_OF_MEMORY));
  return CR_OUT_OF_MEMORY;
}

void mysql_set_local_infile_handler(
    MYSQL *mysql, int (*local_infile_init)(void **, const char *, void *),
    int (*local_infile_read)(void *, char *, uint),
    void (*local_infile_end)(void *),
    int (*local_infile_error)(void *, char *, uint), void *userdata) {
  mysql->options.local_infile_init = local_infile_init;
  mysql->options.local_infile_read = local_infile_read;
  mysql->options.local_infile_end = local_infile_end;
  mysql->options.local_infile_error = local_infile_error;
  mysql->options.local_infile_userdata = userdata;
}

void mysql_set_local_infile_default(MYSQL *mysql) {
  mysql->options.local_infile_init = default_local_infile_init;
  mysql->options.local_infile_read = default_local_infile_read;
  mysql->options.local_infile_end = default_local_infile_end;
  mysql->options.local_infile_error = default_local_infile_error;
}

/**************************************************************************
  Do a query. If query returned rows, free old rows.
  Read data by mysql_store_result or by repeat call of mysql_fetch_row
**************************************************************************/

int STDCALL mysql_query(MYSQL *mysql, const char *query) {
  return mysql_real_query(mysql, query, (ulong)strlen(query));
}

/**************************************************************************
  Move to a specific row and column
**************************************************************************/

void STDCALL mysql_data_seek(MYSQL_RES *result, uint64_t row) {
  MYSQL_ROWS *tmp = nullptr;
  DBUG_PRINT("info", ("mysql_data_seek(%ld)", (long)row));
  if (result->data)
    for (tmp = result->data->data; row-- && tmp; tmp = tmp->next)
      ;
  result->current_row = nullptr;
  result->data_cursor = tmp;
}

/*************************************************************************
  put the row or field cursor one a position one got from mysql_row_tell()
  This doesn't restore any data. The next mysql_fetch_row or
  mysql_fetch_field will return the next row or field after the last used
*************************************************************************/

MYSQL_ROW_OFFSET STDCALL mysql_row_seek(MYSQL_RES *result,
                                        MYSQL_ROW_OFFSET row) {
  MYSQL_ROW_OFFSET return_value = result->data_cursor;
  result->current_row = nullptr;
  result->data_cursor = row;
  return return_value;
}

MYSQL_FIELD_OFFSET STDCALL mysql_field_seek(MYSQL_RES *result,
                                            MYSQL_FIELD_OFFSET field_offset) {
  const MYSQL_FIELD_OFFSET return_value = result->current_field;
  result->current_field = field_offset;
  return return_value;
}

/*****************************************************************************
  List all databases
*****************************************************************************/

MYSQL_RES *STDCALL mysql_list_dbs(MYSQL *mysql, const char *wild) {
  char buff[255];
  DBUG_TRACE;

  append_wild(my_stpcpy(buff, "show databases"), buff + sizeof(buff), wild);
  if (mysql_query(mysql, buff)) return nullptr;
  return mysql_store_result(mysql);
}

/*****************************************************************************
  List all tables in a database
  If wild is given then only the tables matching wild is returned
*****************************************************************************/

MYSQL_RES *STDCALL mysql_list_tables(MYSQL *mysql, const char *wild) {
  char buff[255];
  DBUG_TRACE;

  append_wild(my_stpcpy(buff, "show tables"), buff + sizeof(buff), wild);
  if (mysql_query(mysql, buff)) return nullptr;
  return mysql_store_result(mysql);
}

MYSQL_FIELD *cli_list_fields(MYSQL *mysql) {
  MYSQL_DATA *query;
  MYSQL_FIELD *result;
  MYSQL_TRACE_STAGE(mysql, WAIT_FOR_FIELD_DEF);
  query =
      cli_read_rows(mysql, (MYSQL_FIELD *)nullptr, protocol_41(mysql) ? 8 : 6);
  MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
  if (!query) return nullptr;
  mysql->field_count = (uint)query->rows;
  result = unpack_fields(mysql, query->data, mysql->field_alloc,
                         mysql->field_count, true, mysql->server_capabilities);
  free_rows(query);
  return result;
}

int STDCALL mysql_shutdown(MYSQL *mysql,
                           enum mysql_enum_shutdown_level shutdown_level
                           [[maybe_unused]]) {
  return mysql_real_query(mysql, STRING_WITH_LEN("shutdown"));
}

int STDCALL mysql_kill(MYSQL *mysql, ulong pid) {
  uchar buff[4];
  DBUG_TRACE;
  /*
    Sanity check: if ulong is 64-bits, user can submit a PID here that
    overflows our 32-bit parameter to the somewhat obsolete COM_PROCESS_KILL.
    If this is the case, we'll flag an error here.
    The SQL statement KILL CONNECTION is the safer option here.
    There is an analog of this failsafe in the server as we might see old
    libmysql connection to a new server as well as the other way around.
  */
  if (pid & (~0xffffffffUL)) return CR_INVALID_CONN_HANDLE;
  int4store(buff, pid);
  std::string const kill_stmt = "KILL " + std::to_string(pid);
  return mysql_real_query(mysql, kill_stmt.c_str(), kill_stmt.length());
}

MYSQL_RES *STDCALL mysql_list_processes(MYSQL *mysql) {
  if (mysql_real_query(mysql, STRING_WITH_LEN("SHOW PROCESSLIST")))
    return nullptr;
  return mysql_store_result(mysql);
}

MYSQL_RES *STDCALL mysql_list_fields(MYSQL *mysql, const char *table,
                                     const char *wild) {
  MYSQL_RES *result;
  MYSQL_FIELD *fields;
  MEM_ROOT *new_root;
  char buff[258], *end;
  DBUG_TRACE;
  DBUG_PRINT("enter", ("table: '%s'  wild: '%s'", table, wild ? wild : ""));
  end = strmake(strmake(buff, table, 128) + 1, wild ? wild : "", 128);
  free_old_query(mysql);
  if (simple_command(mysql, COM_FIELD_LIST, (uchar *)buff, (ulong)(end - buff),
                     1) ||
      !(fields = (*mysql->methods->list_fields)(mysql)))
    return nullptr;
  if (!(new_root = (MEM_ROOT *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(MEM_ROOT),
                                         MYF(MY_WME | MY_ZEROFILL))))
    return nullptr;
  if (!(result = (MYSQL_RES *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(MYSQL_RES),
                                        MYF(MY_WME | MY_ZEROFILL)))) {
    my_free(new_root);
    return nullptr;
  }
  result->methods = mysql->methods;
  result->field_alloc = mysql->field_alloc;
  mysql->fields = nullptr;
  mysql->field_alloc = new_root;
  result->field_count = mysql->field_count;
  result->fields = fields;
  result->eof = true;
  return result;
}

int STDCALL mysql_refresh(MYSQL *mysql, uint options) {
  int error = 0;
  std::vector<std::string> commands;

  if (options & REFRESH_GRANT) commands.emplace_back("PRIVILEGES");
  if (options & REFRESH_LOG) commands.emplace_back("LOGS");
  if (options & REFRESH_STATUS) commands.emplace_back("STATUS");

  if (!commands.empty()) {
    std::string flush_command = "FLUSH ";
    for (int i = 0; i < (int)commands.size(); i++) {
      if (i == 0)
        flush_command += commands[i];
      else
        flush_command += "," + commands[i];
    }

    error |=
        mysql_real_query(mysql, flush_command.c_str(), flush_command.length());
    commands.clear();
    flush_command.clear();
  }

  if (options & REFRESH_SOURCE)
    error |=
        mysql_real_query(mysql, STRING_WITH_LEN("RESET BINARY LOGS AND GTIDS"));

  if (options & REFRESH_REPLICA)
    error |= mysql_real_query(mysql, STRING_WITH_LEN("RESET REPLICA"));

  if (options & REFRESH_TABLES)
    error |= mysql_real_query(mysql, STRING_WITH_LEN("FLUSH TABLES"));

  return error;
}

int STDCALL mysql_set_server_option(MYSQL *mysql,
                                    enum enum_mysql_set_option option) {
  uchar buff[2];
  DBUG_TRACE;
  int2store(buff, (uint)option);
  return simple_command(mysql, COM_SET_OPTION, buff, sizeof(buff), 0);
}

int STDCALL mysql_dump_debug_info(MYSQL *mysql) {
  DBUG_TRACE;
  return simple_command(mysql, COM_DEBUG, nullptr, 0, 0);
}

const char *cli_read_statistics(MYSQL *mysql) {
  mysql->net.read_pos[mysql->packet_length] = 0; /* End of stat string */
  if (!mysql->net.read_pos[0]) {
    set_mysql_error(mysql, CR_WRONG_HOST_INFO, unknown_sqlstate);
    return mysql->net.last_error;
  }
  /*
    After reading the single packet with reply to COM_STATISTICS
    we are ready for new commands.
  */
  MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
  return (char *)mysql->net.read_pos;
}

const char *STDCALL mysql_stat(MYSQL *mysql) {
  DBUG_TRACE;
  if (simple_command(mysql, COM_STATISTICS, nullptr, 0, 0))
    return mysql->net.last_error;
  return (*mysql->methods->read_statistics)(mysql);
}

int STDCALL mysql_ping(MYSQL *mysql) {
  int res;
  DBUG_TRACE;
  res = simple_command(mysql, COM_PING, nullptr, 0, 0);
  if (res == CR_SERVER_LOST && mysql->reconnect)
    res = simple_command(mysql, COM_PING, nullptr, 0, 0);
  return res;
}

const char *STDCALL mysql_get_server_info(MYSQL *mysql) {
  return ((char *)mysql->server_version);
}

const char *STDCALL mysql_get_host_info(MYSQL *mysql) {
  return (mysql->host_info);
}

uint STDCALL mysql_get_proto_info(MYSQL *mysql) {
  return (mysql->protocol_version);
}

const char *STDCALL mysql_get_client_info(void) { return MYSQL_SERVER_VERSION; }

ulong STDCALL mysql_get_client_version(void) { return MYSQL_VERSION_ID; }

bool STDCALL mysql_eof(MYSQL_RES *res) { return res->eof; }

MYSQL_FIELD *STDCALL mysql_fetch_field_direct(MYSQL_RES *res, uint fieldnr) {
  if (fieldnr >= res->field_count || !res->fields) return (nullptr);
  return &(res)->fields[fieldnr];
}

MYSQL_ROW_OFFSET STDCALL mysql_row_tell(MYSQL_RES *res) {
  return res->data_cursor;
}

MYSQL_FIELD_OFFSET STDCALL mysql_field_tell(MYSQL_RES *res) {
  return (res)->current_field;
}

/* MYSQL */

uint64_t STDCALL mysql_insert_id(MYSQL *mysql) { return mysql->insert_id; }

uint STDCALL mysql_warning_count(MYSQL *mysql) { return mysql->warning_count; }

ulong STDCALL mysql_thread_id(MYSQL *mysql) {
  /*
    ulong may be 64-bit, but we currently only transmit 32-bit.
    SELECTION CONNECTION_ID() / KILL CONNECTION avoid this issue.
  */
  return (mysql)->thread_id;
}

const char *STDCALL mysql_character_set_name(MYSQL *mysql) {
  return mysql->charset->csname;
}

void STDCALL mysql_get_character_set_info(MYSQL *mysql,
                                          MY_CHARSET_INFO *csinfo) {
  csinfo->number = mysql->charset->number;
  csinfo->state = mysql->charset->state;
  csinfo->csname = mysql->charset->csname;
  csinfo->name = mysql->charset->m_coll_name;
  csinfo->comment = mysql->charset->comment;
  csinfo->mbminlen = mysql->charset->mbminlen;
  csinfo->mbmaxlen = mysql->charset->mbmaxlen;

  if (mysql->options.charset_dir)
    csinfo->dir = mysql->options.charset_dir;
  else
    csinfo->dir = charsets_dir;
}

uint STDCALL mysql_thread_safe(void) { return 1; }

/****************************************************************************
  Some support functions
****************************************************************************/

/*
  Functions called my my_net_init() to set some application specific variables
*/

void my_net_local_init(NET *net) {
  ulong local_net_buffer_length = 0;
  ulong local_max_allowed_packet = 0;

  (void)mysql_get_option(nullptr, MYSQL_OPT_MAX_ALLOWED_PACKET,
                         &local_max_allowed_packet);
  (void)mysql_get_option(nullptr, MYSQL_OPT_NET_BUFFER_LENGTH,
                         &local_net_buffer_length);

  net->max_packet = (uint)local_net_buffer_length;
  my_net_set_read_timeout(net, CLIENT_NET_READ_TIMEOUT);
  my_net_set_write_timeout(net, CLIENT_NET_WRITE_TIMEOUT);
  my_net_set_retry_count(net, CLIENT_NET_RETRY_COUNT);
  net->max_packet_size =
      std::max(local_net_buffer_length, local_max_allowed_packet);
}

/*
  This function is used to create HEX string that you
  can use in a SQL statement in of the either ways:
    INSERT INTO blob_column VALUES (0xAABBCC);  (any MySQL version)
    INSERT INTO blob_column VALUES (X'AABBCC'); (4.1 and higher)

  The string in "from" is encoded to a HEX string.
  The result is placed in "to" and a terminating null byte is appended.

  The string pointed to by "from" must be "length" bytes long.
  You must allocate the "to" buffer to be at least length*2+1 bytes long.
  Each character needs two bytes, and you need room for the terminating
  null byte. When mysql_hex_string() returns, the contents of "to" will
  be a null-terminated string. The return value is the length of the
  encoded string, not including the terminating null character.

  The return value does not contain any leading 0x or a leading X' and
  trailing '. The caller must supply whichever of those is desired.
*/

ulong STDCALL mysql_hex_string(char *to, const char *from, ulong length) {
  char *to0 = to;
  const char *end;

  for (end = from + length; from < end; from++) {
    *to++ = dig_vec_upper[((unsigned char)*from) >> 4];
    *to++ = dig_vec_upper[((unsigned char)*from) & 0x0F];
  }
  *to = '\0';
  return (ulong)(to - to0);
}

/*
  Add escape characters to a string (blob?) to make it suitable for a insert
  to should at least have place for length*2+1 chars
  Returns the length of the to string
*/

ulong STDCALL mysql_escape_string(char *to, const char *from, ulong length) {
  return (uint)escape_string_for_mysql(default_charset_info, to, 0, from,
                                       length);
}

/**
  Escapes special characters in a string for use in an SQL statement.

  Escapes special characters in the unescaped string, taking into account
  the current character set and sql mode of the connection so that is safe
  to place the string in a mysql_query(). This function must be used for
  binary data.

  This function does not work correctly when NO_BACKSLASH_ESCAPES sql mode
  is used and string quote in the SQL statement is different than '\''.

  @deprecated This function should not be used.
              Use mysql_real_escape_string_quote instead.

  @see mysql_real_escape_string_quote

  @param [in] mysql   MySQL connection structure.
  @param [out] to     Escaped string output buffer.
  @param [in] from    String to escape.
  @param [in] length  String to escape length.

  @return Result value.
    @retval != (ulong)-1 Succeeded. Number of bytes written to the output
                         buffer without the '\0' character.
    @retval (ulong)-1    Failed. Use mysql_error() to get error message.
*/

ulong STDCALL mysql_real_escape_string(MYSQL *mysql, char *to, const char *from,
                                       ulong length) {
  if (mysql->server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES) {
    DBUG_PRINT("error", ("NO_BACKSLASH_ESCAPES sql mode requires usage of the "
                         "mysql_real_escape_string_quote function"));
    set_mysql_extended_error(mysql, CR_INSECURE_API_ERR, unknown_sqlstate,
                             ER_CLIENT(CR_INSECURE_API_ERR),
                             "mysql_real_escape_string",
                             "mysql_real_escape_string_quote");
    return (ulong)-1;
  }

  return (uint)mysql_real_escape_string_quote(mysql, to, from, length, '\'');
}

namespace {
/*
  Escape apostrophes by doubling them up

  SYNOPSIS
    escape_quotes_for_mysql()
    charset_info        Charset of the strings
    to                  Buffer for escaped string
    to_length           Length of destination buffer, or 0
    from                The string to escape
    length              The length of the string to escape
    quote               The quote the buffer will be escaped against

  DESCRIPTION
    This escapes the contents of a string by doubling up any character
    specified by the quote parameter. This is used when the
    NO_BACKSLASH_ESCAPES SQL_MODE is in effect on the server.

  NOTE
    To be consistent with escape_string_for_mysql(), to_length may be 0 to
    mean "big enough"

  RETURN VALUES
    ~0          The escaped string did not fit in the to buffer
    >=0         The length of the escaped string
*/

size_t escape_quotes_for_mysql(CHARSET_INFO *charset_info, char *to,
                               size_t to_length, const char *from,
                               size_t length, char quote) {
  const char *to_start = to;
  const char *end = nullptr;
  const char *to_end = to_start + (to_length ? to_length - 1 : 2 * length);
  bool overflow = false;
  const bool use_mb_flag = use_mb(charset_info);
  for (end = from + length; from < end; from++) {
    int tmp_length = 0;
    if (use_mb_flag && (tmp_length = my_ismbchar(charset_info, from, end))) {
      if (to + tmp_length > to_end) {
        overflow = true;
        break;
      }
      while (tmp_length--) *to++ = *from++;
      from--;
      continue;
    }
    /*
      We don't have the same issue here with a non-multi-byte character being
      turned into a multi-byte character by the addition of an escaping
      character, because we are only escaping the ' character with itself.
     */
    if (*from == quote) {
      if (to + 2 > to_end) {
        overflow = true;
        break;
      }
      *to++ = quote;
      *to++ = quote;
    } else {
      if (to + 1 > to_end) {
        overflow = true;
        break;
      }
      *to++ = *from;
    }
  }
  *to = 0;
  return overflow ? (ulong)~0 : (ulong)(to - to_start);
}
}  // namespace

/**
  Escapes special characters in a string for use in an SQL statement.

  Escapes special characters in the unescaped string, taking into account
  the current character set and sql mode of the connection so that is safe
  to place the string in a mysql_query(). This function must be used for
  binary data.

  This function should be used for escaping identifiers and string parameters.

  @param [in] mysql   MySQL connection structure.
  @param [out] to     Escaped string output buffer.
  @param [in] from    String to escape.
  @param [in] length  String to escape length.
  @param [in] quote   String quoting character used in an SQL statement. This
                      should be one of '\'', '"' or '`' depending on the
                      parameter quoting applied in the SQL statement.

  @return Result value.
    @retval != (ulong)-1 Succeeded. Number of bytes written to the output
                         buffer without the '\0' character.
    @retval (ulong)-1    Failed.
*/

ulong STDCALL mysql_real_escape_string_quote(MYSQL *mysql, char *to,
                                             const char *from, ulong length,
                                             char quote) {
  if (quote == '`' || mysql->server_status & SERVER_STATUS_NO_BACKSLASH_ESCAPES)
    return (uint)escape_quotes_for_mysql(mysql->charset, to, 0, from, length,
                                         quote);
  return (uint)escape_string_for_mysql(mysql->charset, to, 0, from, length);
}

void STDCALL myodbc_remove_escape(MYSQL *mysql, char *name) {
  char *to;
  const bool use_mb_flag = use_mb(mysql->charset);
  char *end = nullptr;
  if (use_mb_flag)
    for (end = name; *end; end++)
      ;

  for (to = name; *name; name++) {
    int l;
    if (use_mb_flag && (l = my_ismbchar(mysql->charset, name, end))) {
      while (l--) *to++ = *name++;
      name--;
      continue;
    }
    if (*name == '\\' && name[1]) name++;
    *to++ = *name;
  }
  *to = 0;
}

/********************************************************************
 Implementation of new client API for 4.1 version.

 mysql_stmt_* are real prototypes used by applications.

 All functions performing
 real I/O are prefixed with 'cli_' (abbreviated from 'Call Level
 Interface'). This functions are invoked via pointers set in
 MYSQL::methods structure.
*********************************************************************/

/******************* Declarations ***********************************/

/* Default number of rows fetched per one COM_STMT_FETCH command. */

#define DEFAULT_PREFETCH_ROWS (ulong)1

/*
  These functions are called by function pointer MYSQL_STMT::read_row_func.
  Each function corresponds to one of the read methods:
  - mysql_stmt_fetch without prior mysql_stmt_store_result,
  - mysql_stmt_fetch when result is stored,
  - mysql_stmt_fetch when there are no rows (always returns MYSQL_NO_DATA)
*/

static int stmt_read_row_unbuffered(MYSQL_STMT *stmt, unsigned char **row);
static int stmt_read_row_buffered(MYSQL_STMT *stmt, unsigned char **row);
static int stmt_read_row_from_cursor(MYSQL_STMT *stmt, unsigned char **row);
static int stmt_read_row_no_data(MYSQL_STMT *stmt, unsigned char **row);
static int stmt_read_row_no_result_set(MYSQL_STMT *stmt, unsigned char **row);

/*
  This function is used in mysql_stmt_store_result if
  STMT_ATTR_UPDATE_MAX_LENGTH attribute is set.
*/
static void stmt_update_metadata(MYSQL_STMT *stmt, MYSQL_ROWS *data);
static bool setup_one_fetch_function(MYSQL_BIND *, MYSQL_FIELD *field);

/* Auxiliary function used to reset statement handle. */

#define RESET_SERVER_SIDE 1
#define RESET_LONG_DATA 2
#define RESET_STORE_RESULT 4
#define RESET_CLEAR_ERROR 8

static bool reset_stmt_handle(MYSQL_STMT *stmt, uint flags);

#define MAX_DOUBLE_STRING_REP_LENGTH 331

/* A macro to check truncation errors */

#define IS_TRUNCATED(value, is_unsigned, min, max, umax)       \
  ((is_unsigned) ? (((value) > (umax) || (value) < 0) ? 1 : 0) \
                 : (((value) > (max) || (value) < (min)) ? 1 : 0))

#define BIND_RESULT_DONE 1
/*
  We report truncations only if at least one of MYSQL_BIND::error
  pointers is set. In this case stmt->bind_result_done |-ed with
  this flag.
*/
#define REPORT_DATA_TRUNCATION 2

/**************** Misc utility functions ****************************/

static void stmt_clear_error(MYSQL_STMT *stmt) {
  if (stmt->last_errno) {
    stmt->last_errno = 0;
    stmt->last_error[0] = '\0';
    my_stpcpy(stmt->sqlstate, not_error_sqlstate);
  }
}

/**
  Set statement error code, sqlstate, and error message
  from given errcode and sqlstate.
*/

void set_stmt_error(MYSQL_STMT *stmt, int errcode, const char *sqlstate) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("error: %d '%s'", errcode, ER_CLIENT(errcode)));
  assert(stmt != nullptr);

  stmt->last_errno = errcode;
  my_stpcpy(stmt->last_error, ER_CLIENT(errcode));
  my_stpcpy(stmt->sqlstate, sqlstate);
}

/**
  Set statement error code, sqlstate, and error message
  from given errcode and sqlstate.
*/

void set_stmt_extended_error(MYSQL_STMT *stmt, int errcode,
                             const char *sqlstate, const char *format, ...) {
  DBUG_TRACE;
  va_list args;
  DBUG_PRINT("enter", ("error: %d '%s'", errcode, format));
  assert(stmt != nullptr);

  stmt->last_errno = errcode;
  va_start(args, format);
  vsnprintf(stmt->last_error, sizeof(stmt->last_error) - 1, format, args);
  va_end(args);
  my_stpcpy(stmt->sqlstate, sqlstate);
}

/**
  Set statement error code, sqlstate, and error message from NET.

  @param stmt  a statement handle. Copy the error here.
  @param net   mysql->net. Source of the error.
*/

void set_stmt_errmsg(MYSQL_STMT *stmt, NET *net) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("error: %d/%s '%s'", net->last_errno, net->sqlstate,
                       net->last_error));
  assert(stmt != nullptr);

  stmt->last_errno = net->last_errno;
  if (net->last_error[0] != '\0') my_stpcpy(stmt->last_error, net->last_error);
  my_stpcpy(stmt->sqlstate, net->sqlstate);
}

/*
  Read and unpack server reply to COM_STMT_PREPARE command (sent from
  mysql_stmt_prepare).

  SYNOPSIS
    cli_read_prepare_result()
    mysql   connection handle
    stmt    statement handle

  RETURN VALUES
    0	ok
    1	error
*/

bool cli_read_prepare_result(MYSQL *mysql, MYSQL_STMT *stmt) {
  uchar *pos;
  uint field_count, param_count;
  ulong packet_length;
  DBUG_TRACE;

  /* free old result and initialize mysql->field_alloc */
  free_old_query(mysql);

  if ((packet_length = cli_safe_read(mysql, nullptr)) == packet_error)
    return true;
  mysql->warning_count = 0;

  pos = (uchar *)mysql->net.read_pos;
  stmt->stmt_id = uint4korr(pos + 1);
  pos += 5;
  /* Number of columns in result set */
  field_count = uint2korr(pos);
  pos += 2;
  /* Number of placeholders in the statement */
  param_count = uint2korr(pos);
  pos += 2;

  mysql->resultset_metadata = RESULTSET_METADATA_FULL;
  if (packet_length >= 12) {
    mysql->warning_count = uint2korr(pos + 1);
    if (mysql->client_flag & CLIENT_OPTIONAL_RESULTSET_METADATA) {
      mysql->resultset_metadata =
          static_cast<enum enum_resultset_metadata>(*(pos + 3));
    }
  }

  if (param_count != 0 &&
      mysql->resultset_metadata == RESULTSET_METADATA_FULL) {
    MYSQL_TRACE_STAGE(mysql, WAIT_FOR_PARAM_DEF);
    /* skip parameters data: we don't support it yet */
    if (!(cli_read_metadata(mysql, param_count, 7))) return true;
    /* free memory allocated by cli_read_metadata() for parameters data */
    mysql->field_alloc->Clear();
  }

  if (field_count != 0) {
    if (!(mysql->server_status & SERVER_STATUS_AUTOCOMMIT))
      mysql->server_status |= SERVER_STATUS_IN_TRANS;

    if (mysql->resultset_metadata == RESULTSET_METADATA_FULL) {
      MYSQL_TRACE_STAGE(mysql, WAIT_FOR_FIELD_DEF);
      if (!(stmt->fields =
                cli_read_metadata_ex(mysql, stmt->mem_root, field_count, 7)))
        return true;
    }
  }

  MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);

  stmt->field_count = field_count;
  stmt->param_count = (ulong)param_count;
  DBUG_PRINT("exit", ("field_count: %u  param_count: %u  warning_count: %u",
                      field_count, param_count, (uint)mysql->warning_count));

  return false;
}

void mysql_stmt_extension_bind_free(MYSQL_STMT_EXT *ext) {
  DBUG_TRACE;
  ext->bind_data.n_params = 0;
  ext->bind_data.names = nullptr;
  ext->bind_data.mem_root.Clear();

  DBUG_EXECUTE_IF("test_stmt_ext_allocations", {
    for (auto alloc : *(ext->bind_data.allocations)) {
      my_free(alloc);
    }
    ext->bind_data.allocations->clear();
  });
}

static void *stmt_ext_allocate(MYSQL_STMT_EXT *ext, size_t bytes) {
  void *result = nullptr;
  DBUG_EXECUTE_IF("test_stmt_ext_allocations", {
    result = my_malloc(key_memory_MYSQL, bytes, MYF(0));
    ext->bind_data.allocations->insert(result);
  });
  if (result == nullptr) {
    result = ext->bind_data.mem_root.Alloc(bytes);
  }
  return result;
}

/*
  Allocate memory and init prepared statement structure.

  SYNOPSIS
    mysql_stmt_init()
    mysql   connection handle

  DESCRIPTION
    This is an entry point of the new API. Returned handle stands for
    a server-side prepared statement. Memory for this structure (~700
    bytes) is allocated using 'malloc'. Once created, the handle can be
    reused many times. Created statement handle is bound to connection
    handle provided to this call: its lifetime is limited by lifetime
    of connection.
    'mysql_stmt_init()' is a pure local call, server side structure is
    created only in mysql_stmt_prepare.
    Next steps you may want to make:
    - set a statement attribute (mysql_stmt_attr_set()),
    - prepare statement handle with a query (mysql_stmt_prepare()),
    - close statement handle and free its memory (mysql_stmt_close()),
    - reset statement with mysql_stmt_reset() (a no-op which will
      just return).
    Behaviour of the rest of API calls on this statement is not defined yet
    (though we're working on making each wrong call sequence return
    error).

  RETURN VALUE
    statement structure upon success and NULL if out of
    memory
*/

MYSQL_STMT *STDCALL mysql_stmt_init(MYSQL *mysql) {
  MYSQL_STMT *stmt;
  DBUG_TRACE;

  if (!(stmt = (MYSQL_STMT *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(MYSQL_STMT),
                                       MYF(MY_WME | MY_ZEROFILL))) ||
      !(stmt->extension = (MYSQL_STMT_EXT *)my_malloc(
            PSI_NOT_INSTRUMENTED, sizeof(MYSQL_STMT_EXT),
            MYF(MY_WME | MY_ZEROFILL))) ||
      !(stmt->mem_root =
            (MEM_ROOT *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(MEM_ROOT),
                                  MYF(MY_WME | MY_ZEROFILL))) ||
      !(stmt->result.alloc =
            (MEM_ROOT *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(MEM_ROOT),
                                  MYF(MY_WME | MY_ZEROFILL)))) {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    my_free(stmt);
    return nullptr;
  }

  ::new ((void *)stmt->mem_root) MEM_ROOT(PSI_NOT_INSTRUMENTED, 2048);
  ::new ((void *)stmt->result.alloc) MEM_ROOT(PSI_NOT_INSTRUMENTED, 4096);
  mysql->stmts = list_add(mysql->stmts, &stmt->list);
  stmt->list.data = stmt;
  stmt->state = MYSQL_STMT_INIT_DONE;
  stmt->mysql = mysql;
  stmt->read_row_func = stmt_read_row_no_result_set;
  stmt->prefetch_rows = DEFAULT_PREFETCH_ROWS;
  my_stpcpy(stmt->sqlstate, not_error_sqlstate);
  /* The rest of statement members was zeroed inside malloc */

  ::new ((void *)&stmt->extension->fields_mem_root)
      MEM_ROOT(PSI_NOT_INSTRUMENTED, 2048);
  ::new ((void *)&stmt->extension->bind_data.mem_root)
      MEM_ROOT(PSI_NOT_INSTRUMENTED, 2048);
#ifndef NDEBUG
  stmt->extension->bind_data.allocations = new std::set<void *>();
#endif

  return stmt;
}

/*
  Prepare server side statement with query.

  SYNOPSIS
    mysql_stmt_prepare()
    stmt    statement handle
    query   statement to prepare
    length  statement length

  DESCRIPTION
    Associate statement with statement handle. This is done both on
    client and server sides. At this point the server parses given query
    and creates an internal structure to represent it.
    Next steps you may want to make:
    - find out if this statement returns a result set by
      calling mysql_stmt_field_count(), and get result set metadata
      with mysql_stmt_result_metadata(),
    - if query contains placeholders, bind input parameters to placeholders
      using mysql_stmt_bind_named_param(),
    - otherwise proceed directly to mysql_stmt_execute().

  IMPLEMENTATION NOTES
  - if this is a re-prepare of the statement, first close previous data
    structure on the server and free old statement data
  - then send the query to server and get back number of placeholders,
    number of columns in result set (if any), and result set metadata.
    At the same time allocate memory for input and output parameters
    to have less checks in mysql_stmt_bind_{param, result}.

  RETURN VALUES
    0  success
   !0  error
*/

int STDCALL mysql_stmt_prepare(MYSQL_STMT *stmt, const char *query,
                               ulong length) {
  MYSQL *mysql = stmt->mysql;
  DBUG_TRACE;

  if (!mysql) {
    /* mysql can be reset in mysql_close called from mysql_reconnect */
    set_stmt_error(stmt, CR_SERVER_LOST, unknown_sqlstate);
    return 1;
  }

  /*
    Reset the last error in any case: that would clear the statement
    if the previous prepare failed.
  */
  stmt->last_errno = 0;
  stmt->last_error[0] = '\0';

  if ((int)stmt->state > (int)MYSQL_STMT_INIT_DONE) {
    /* This is second prepare with another statement */
    uchar buff[MYSQL_STMT_HEADER]; /* 4 bytes - stmt id */

    if (reset_stmt_handle(stmt, RESET_LONG_DATA | RESET_STORE_RESULT)) return 1;
    /*
      These members must be reset for API to
      function in case of error or misuse.
    */
    stmt->bind_param_done = false;
    stmt->bind_result_done = false;
    stmt->param_count = stmt->field_count = 0;
    mysql_stmt_extension_bind_free(stmt->extension);
    stmt->mem_root->ClearForReuse();
    stmt->extension->fields_mem_root.Clear();

    int4store(buff, stmt->stmt_id);

    /*
      Close statement in server

      If there was a 'use' result from another statement, or from
      mysql_use_result it won't be freed in mysql_stmt_free_result and
      we should get 'Commands out of sync' here.
    */
    stmt->state = MYSQL_STMT_INIT_DONE;
    if (stmt_command(mysql, COM_STMT_CLOSE, buff, 4, stmt)) {
      set_stmt_errmsg(stmt, &mysql->net);
      return 1;
    }
  }

  if (stmt_command(mysql, COM_STMT_PREPARE, (const uchar *)query, length,
                   stmt)) {
    set_stmt_errmsg(stmt, &mysql->net);
    return 1;
  }

  if ((*mysql->methods->read_prepare_result)(mysql, stmt)) {
    set_stmt_errmsg(stmt, &mysql->net);
    return 1;
  }

  stmt->params = nullptr;
  stmt->bind = nullptr;
  stmt->state = MYSQL_STMT_PREPARE_DONE;
  DBUG_PRINT("info", ("Parameter count: %u", stmt->param_count));
  return 0;
}

/*
  Get result set metadata from reply to mysql_stmt_execute.
  This is used mainly for SHOW commands, as metadata for these
  commands is sent only with result set.
  To be removed when all commands will fully support prepared mode.
*/

static void alloc_stmt_fields(MYSQL_STMT *stmt) {
  MYSQL_FIELD *fields, *field, *end;
  MEM_ROOT *fields_mem_root = &stmt->extension->fields_mem_root;
  MYSQL *mysql = stmt->mysql;

  assert(stmt->field_count);

  fields_mem_root->Clear();

  /*
    mysql->fields is NULL when the client set CLIENT_OPTIONAL_RESULTSET_METADATA
    flag and server @@session.resultset_metadata is "NONE". That means that the
    client received a resultset without metadata.
  */
  if (!mysql->fields) return;

  /*
    Get the field information for non-select statements
    like SHOW and DESCRIBE commands
  */
  if (!(stmt->fields = (MYSQL_FIELD *)fields_mem_root->Alloc(
            sizeof(MYSQL_FIELD) * stmt->field_count)) ||
      !(stmt->bind = (MYSQL_BIND *)fields_mem_root->Alloc(sizeof(MYSQL_BIND) *
                                                          stmt->field_count))) {
    set_stmt_error(stmt, CR_OUT_OF_MEMORY, unknown_sqlstate);
    return;
  }

  for (fields = mysql->fields, end = fields + stmt->field_count,
      field = stmt->fields;
       field && fields < end; fields++, field++) {
    *field = *fields; /* To copy all numeric parts. */
    field->catalog =
        strmake_root(fields_mem_root, fields->catalog, fields->catalog_length);
    field->db = strmake_root(fields_mem_root, fields->db, fields->db_length);
    field->table =
        strmake_root(fields_mem_root, fields->table, fields->table_length);
    field->org_table = strmake_root(fields_mem_root, fields->org_table,
                                    fields->org_table_length);
    field->name =
        strmake_root(fields_mem_root, fields->name, fields->name_length);
    field->org_name = strmake_root(fields_mem_root, fields->org_name,
                                   fields->org_name_length);
    if (fields->def) {
      field->def =
          strmake_root(fields_mem_root, fields->def, fields->def_length);
      field->def_length = fields->def_length;
    } else {
      field->def = nullptr;
      field->def_length = 0;
    }
    field->extension = nullptr; /* Avoid dangling links. */
    field->max_length = 0; /* max_length is set in mysql_stmt_store_result() */
  }
}

/**
  Update result set columns metadata if it was sent again in
  reply to COM_STMT_EXECUTE.

  @note If the new field count is different from the original one,
        an error is set and no update is performed.
*/

static void update_stmt_fields(MYSQL_STMT *stmt) {
  MYSQL_FIELD *field = stmt->mysql->fields;
  MYSQL_FIELD *field_end =
      field != nullptr ? field + stmt->field_count : nullptr;
  MYSQL_FIELD *stmt_field = stmt->fields;
  MYSQL_BIND *my_bind = stmt->bind_result_done ? stmt->bind : nullptr;

  if (stmt->field_count != stmt->mysql->field_count) {
    /*
      The tables used in the statement were altered,
      and the query now returns a different number of columns.
      There is no way to continue without reallocating the bind
      array:
      - if the number of columns increased, mysql_stmt_fetch()
      will write beyond allocated memory
      - if the number of columns decreased, some user-bound
      buffers will be left unassigned without user knowing
      that.
    */
    set_stmt_error(stmt, CR_NEW_STMT_METADATA, unknown_sqlstate);
    return;
  }

  /*
    mysql->fields is NULL when the client set CLIENT_OPTIONAL_RESULTSET_METADATA
    flag and server @@session.resultset_metadata is "NONE". That means that the
    client received a resultset without metadata.
  */
  if (!field) return;

  for (; field < field_end; ++field, ++stmt_field) {
    stmt_field->charsetnr = field->charsetnr;
    stmt_field->length = field->length;
    stmt_field->type = field->type;
    stmt_field->flags = field->flags;
    stmt_field->decimals = field->decimals;
    if (my_bind) {
      /* Ignore return value: it should be 0 if bind_result succeeded. */
      (void)setup_one_fetch_function(my_bind++, stmt_field);
    }
  }
}

/*
  Returns prepared statement metadata in the form of a result set.

  SYNOPSIS
    mysql_stmt_result_metadata()
    stmt  statement handle

  DESCRIPTION
    This function should be used after mysql_stmt_execute().
    You can safely check that prepared statement has a result set by calling
    mysql_stmt_field_count(): if number of fields is not zero, you can call
    this function to get fields metadata.
    Next steps you may want to make:
    - find out number of columns in result set by calling
      mysql_num_fields(res) (the same value is returned by
      mysql_stmt_field_count())
    - fetch metadata for any column with mysql_fetch_field,
      mysql_fetch_field_direct, mysql_fetch_fields, mysql_field_seek.
    - free returned MYSQL_RES structure with mysql_free_result.
    - proceed to binding of output parameters.

  RETURN
    NULL  statement contains no result set or out of memory.
          In the latter case you can retrieve error message
          with mysql_stmt_error.
    MYSQL_RES  a result set with no rows
*/

MYSQL_RES *STDCALL mysql_stmt_result_metadata(MYSQL_STMT *stmt) {
  MYSQL_RES *result;
  DBUG_TRACE;

  /*
    stmt->fields is only defined if stmt->field_count is not null;
    stmt->field_count is initialized in prepare.
  */
  if (!stmt->field_count) return nullptr;

  if (!(result = (MYSQL_RES *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(*result),
                                        MYF(MY_WME | MY_ZEROFILL)))) {
    set_stmt_error(stmt, CR_OUT_OF_MEMORY, unknown_sqlstate);
    return nullptr;
  }

  result->methods = stmt->mysql->methods;
  result->eof = true; /* Marker for buffered */
  result->fields = stmt->fields;
  result->field_count = stmt->field_count;
  /* The rest of members of 'result' was zeroed inside malloc */
  return result;
}

/*
  Returns parameter columns meta information in the form of
  result set.

  SYNOPSIS
    mysql_stmt_param_metadata()
    stmt    statement handle

  DESCRIPTION
    This function can be called after you prepared the statement handle
    with mysql_stmt_prepare().
    XXX: not implemented yet.

  RETURN
    MYSQL_RES on success, 0 if there is no metadata.
    Currently this function always returns 0.
*/

MYSQL_RES *STDCALL mysql_stmt_param_metadata(MYSQL_STMT *stmt) {
  DBUG_TRACE;

  if (!stmt->param_count) return nullptr;

  /*
    TODO: Fix this when server sends the information.
    Till then keep a dummy prototype.
  */
  return nullptr;
}

static inline int add_binary_row(NET *net, MYSQL_STMT *stmt, ulong pkt_len,
                                 MYSQL_ROWS ***prev_ptr) {
  MYSQL_ROWS *row;
  uchar *cp = net->read_pos;
  MYSQL_DATA *result = &stmt->result;
  if (!(row = (MYSQL_ROWS *)result->alloc->Alloc(sizeof(MYSQL_ROWS) + pkt_len -
                                                 1))) {
    set_stmt_error(stmt, CR_OUT_OF_MEMORY, unknown_sqlstate);
    return 1;
  }
  row->data = (MYSQL_ROW)(row + 1);
  **prev_ptr = row;
  *prev_ptr = &row->next;
  memcpy((char *)row->data, (char *)cp + 1, pkt_len - 1);
  row->length = pkt_len; /* To allow us to do sanity checks */
  result->rows++;
  return 0;
}

/**
  Auxiliary function to send COM_STMT_EXECUTE packet to server and read reply.

  Used from @ref cli_stmt_execute, which is in turn used by
  @ref mysql_stmt_execute.

  @param stmt the stmt to execute
  @param packet the data for the parameters
  @param length number of bytes in the buffer "data"
  @param send_param_count ON if the server properly processes the
     PARAMETER_COUNT_AVAILABLE flag, so we can send it.
  @retval false success
  @retval true failure. error set
*/
static bool execute(MYSQL_STMT *stmt, char *packet, ulong length,
                    bool send_param_count) {
  MYSQL *mysql = stmt->mysql;
  NET *net = &mysql->net;
  uchar buff[4 /* size of stmt id */ + 5 /* execution flags */];
  bool res;
  bool is_data_packet = false;
  ulong pkt_len;
  MYSQL_ROWS **prev_ptr = nullptr;
  DBUG_TRACE;
  DBUG_DUMP("packet", (uchar *)packet, length);

  int4store(buff, stmt->stmt_id); /* Send stmt id to server */
  auto flags = (uchar)stmt->flags;

  /*
    If the server supports query attributes raise the flag that we
    are going to be sending the parameter block.
    Unfortunately there's a bug in processing the flags in servers
    earlier than 8.0.26 that conflates all the flags into a single
    boolean. Thus we need to cut off sending PARAMETER_COUNT_AVAILABLE
    for these
  */
  if ((mysql->server_capabilities & CLIENT_QUERY_ATTRIBUTES) != 0 &&
      send_param_count) {
    DBUG_PRINT("prep_stmt_exec", ("Setting PARAMETER_COUNT_AVAILABLE"));
    flags |= PARAMETER_COUNT_AVAILABLE;
  }
  buff[4] = (char)(flags);
  int4store(buff + 5, 1); /* iteration count */

  res = (cli_advanced_command(mysql, COM_STMT_EXECUTE, buff, sizeof(buff),
                              (uchar *)packet, length, true, stmt) ||
         (*mysql->methods->read_query_result)(mysql));

  if ((mysql->server_capabilities & CLIENT_DEPRECATE_EOF)) {
    if (mysql->server_status & SERVER_STATUS_CURSOR_EXISTS)
      mysql->server_status &= ~SERVER_STATUS_CURSOR_EXISTS;

    /*
      After having read the query result, we need to make sure that the client
      does not end up into a hang waiting for the server to send a packet.
      If the CURSOR_TYPE_READ_ONLY flag is set, we would want to perform the
      additional packet read mainly for prepared statements involving SELECT
      queries. For SELECT queries, the result format would either be
      <Metadata><OK> or <Metadata><rows><OK>. We would have read the metadata
      by now and have the field_count populated. The check for field_count will
      help determine if we can expect an additional packet from the server.
    */

    if (!res && (stmt->flags & CURSOR_TYPE_READ_ONLY) &&
        mysql->field_count != 0) {
      /*
        server can now respond with a cursor - then the respond will be
        <Metadata><OK> or with binary rows result set <Metadata><row(s)><OK>.
        The former can be the case when the prepared statement is a procedure
        invocation, ie. call(). There also other cases. When server responds
        with <OK> (cursor) packet we read it and get the server status. In case
        it responds with binary row we add it to the binary rows result set
        (the reset of the result set will be read in prepare_to_fetch_result).
      */

      if ((pkt_len = cli_safe_read(mysql, &is_data_packet)) == packet_error) {
        set_stmt_errmsg(stmt, net);
        mysql->status = MYSQL_STATUS_READY;
        stmt->read_row_func = stmt_read_row_no_data;
        return true;
      }

      if (is_data_packet) {
        assert(stmt->result.rows == 0);
        prev_ptr = &stmt->result.data;
        if (add_binary_row(net, stmt, pkt_len, &prev_ptr)) return true;
      } else {
        read_ok_ex(mysql, pkt_len);
        /*
          If the result set was empty and the server did not open a cursor,
          then the response from the server would have been <metadata><OK>.
          This means the OK packet read above was the last OK packet of the
          sequence. Hence, we set the status to indicate that the client is
          now ready for next command. The stmt->read_row_func is set so as
          to ensure that the next call to C API mysql_stmt_fetch() will not
          read on the network. Instead, it will return NO_MORE_DATA.
        */
        if (!(mysql->server_status & SERVER_STATUS_CURSOR_EXISTS)) {
          mysql->status = MYSQL_STATUS_READY;
          stmt->read_row_func = stmt_read_row_no_data;
        }
      }
    }
  }

  stmt->affected_rows = mysql->affected_rows;
  stmt->server_status = mysql->server_status;
  stmt->insert_id = mysql->insert_id;
  if (res) {
    /*
      Don't set stmt error if stmt->mysql is NULL, as the error in this case
      has already been set by mysql_prune_stmt_list().
    */
    if (stmt->mysql) set_stmt_errmsg(stmt, net);
    return true;
  }
  if (mysql->status == MYSQL_STATUS_GET_RESULT)
    stmt->mysql->status = MYSQL_STATUS_STATEMENT_GET_RESULT;
  return false;
}

int cli_stmt_execute(MYSQL_STMT *stmt) {
  DBUG_TRACE;
  MYSQL *mysql = stmt->mysql;
  const bool send_named_params =
      (mysql->server_capabilities & CLIENT_QUERY_ATTRIBUTES) != 0;
  bool const can_deal_with_flags =
      mysql->server_version && mysql_get_server_version(mysql) >= 80026;
  /*
    When the server can deal with flags properly we should send the 0 param
    count even when there's no parameters when the server supports named
    parameters to signify there's no query attributes either. We are setting the
    PARAMETER_COUNT_AVAILABLE later on the same condition in execute()
  */
  if (stmt->param_count || send_named_params) {
    uchar *param_data = nullptr;
    bool result;
    unsigned long param_length = 0;

    if (!stmt->bind_param_done &&
        (!send_named_params || stmt->param_count != 0)) {
      set_stmt_error(stmt, CR_PARAMS_NOT_BOUND, unknown_sqlstate);
      return 1;
    }
    if (mysql->status != MYSQL_STATUS_READY ||
        mysql->server_status & SERVER_MORE_RESULTS_EXISTS) {
      set_stmt_error(stmt, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
      return 1;
    }

    if (mysql->net.vio)
      net_clear(&mysql->net, true); /* Sets net->write_pos */
    else {
      set_stmt_errmsg(stmt, &mysql->net);
      return 1;
    }

    MYSQL_STMT_EXT *ext = stmt->extension;

    result = mysql_int_serialize_param_data(
        &mysql->net, ext->bind_data.n_params, stmt->params,
        const_cast<const char **>(ext->bind_data.names), 1, &param_data,
        &param_length, 1, send_named_params, false, can_deal_with_flags);
    if (result != 0) {
      set_stmt_errmsg(stmt, &mysql->net);
      return 1;
    }

    result = execute(stmt, pointer_cast<char *>(param_data), param_length,
                     can_deal_with_flags);
    stmt->send_types_to_server = false;
    my_free(param_data);
    return result;
  }
  return (int)execute(stmt, nullptr, 0, can_deal_with_flags);
}

/*
  Read one row from buffered result set.  Result set is created by prior
  call to mysql_stmt_store_result().
  SYNOPSIS
    stmt_read_row_buffered()

  RETURN VALUE
    0             - success; *row is set to valid row pointer (row data
                    is stored in result set buffer)
    MYSQL_NO_DATA - end of result set. *row is set to NULL
*/

static int stmt_read_row_buffered(MYSQL_STMT *stmt, unsigned char **row) {
  if (stmt->data_cursor) {
    *row = (uchar *)stmt->data_cursor->data;
    stmt->data_cursor = stmt->data_cursor->next;
    return 0;
  }
  *row = nullptr;
  return MYSQL_NO_DATA;
}

/*
  Read one row from network: unbuffered non-cursor fetch.
  If last row was read, or error occurred, erase this statement
  from record pointing to object unbuffered fetch is performed from.

  SYNOPSIS
    stmt_read_row_unbuffered()
    stmt  statement handle
    row   pointer to write pointer to row data;

  RETURN VALUE
    0           - success; *row contains valid address of a row;
                  row data is stored in network buffer
    1           - error; error code is written to
                  stmt->last_{errno,error}; *row is not changed
  MYSQL_NO_DATA - end of file was read from network;
                  *row is set to NULL
*/

static int stmt_read_row_unbuffered(MYSQL_STMT *stmt, unsigned char **row) {
  int rc = 1;
  MYSQL *mysql = stmt->mysql;
  /*
    This function won't be called if stmt->field_count is zero
    or execution wasn't done: this is ensured by mysql_stmt_execute.
  */
  if (!mysql) {
    set_stmt_error(stmt, CR_SERVER_LOST, unknown_sqlstate);
    return 1;
  }
  if (mysql->status != MYSQL_STATUS_STATEMENT_GET_RESULT) {
    set_stmt_error(stmt,
                   stmt->unbuffered_fetch_cancelled ? CR_FETCH_CANCELED
                                                    : CR_COMMANDS_OUT_OF_SYNC,
                   unknown_sqlstate);
    goto error;
  }
  if ((*mysql->methods->unbuffered_fetch)(mysql, (char **)row)) {
    set_stmt_errmsg(stmt, &mysql->net);
    /*
      If there was an error, there are no more pending rows:
      reset statement status to not hang up in following
      mysql_stmt_close (it will try to flush result set before
      closing the statement).
    */
    mysql->status = MYSQL_STATUS_READY;
    goto error;
  }
  if (!*row) {
    mysql->status = MYSQL_STATUS_READY;
    rc = MYSQL_NO_DATA;
    goto error;
  }
  return 0;
error:
  if (mysql->unbuffered_fetch_owner == &stmt->unbuffered_fetch_cancelled)
    mysql->unbuffered_fetch_owner = nullptr;
  return rc;
}

/*
  Fetch statement row using server side cursor.

  SYNOPSIS
    stmt_read_row_from_cursor()

  RETURN VALUE
    0            success
    1            error
  MYSQL_NO_DATA  end of data
*/

static int stmt_read_row_from_cursor(MYSQL_STMT *stmt, unsigned char **row) {
  if (stmt->data_cursor) return stmt_read_row_buffered(stmt, row);
  if (stmt->server_status & SERVER_STATUS_LAST_ROW_SENT)
    stmt->server_status &= ~SERVER_STATUS_LAST_ROW_SENT;
  else {
    MYSQL *mysql = stmt->mysql;
    NET *net = &mysql->net;
    MYSQL_DATA *result = &stmt->result;
    uchar buff[4 /* statement id */ + 4 /* number of rows to fetch */];

    result->alloc->ClearForReuse();
    result->data = nullptr;
    result->rows = 0;
    /* Send row request to the server */
    int4store(buff, stmt->stmt_id);
    int4store(buff + 4, stmt->prefetch_rows); /* number of rows to fetch */
    if ((*mysql->methods->advanced_command)(mysql, COM_STMT_FETCH, buff,
                                            sizeof(buff), (uchar *)nullptr, 0,
                                            true, stmt)) {
      /*
        Don't set stmt error if stmt->mysql is NULL, as the error in this case
        has already been set by mysql_prune_stmt_list().
      */
      if (stmt->mysql) set_stmt_errmsg(stmt, net);
      return 1;
    }
    if ((*mysql->methods->read_rows_from_cursor)(stmt)) return 1;
    stmt->server_status = mysql->server_status;

    stmt->data_cursor = result->data;
    return stmt_read_row_buffered(stmt, row);
  }
  *row = nullptr;
  return MYSQL_NO_DATA;
}

/*
  Default read row function to not SIGSEGV in client in
  case of wrong sequence of API calls.
*/

static int stmt_read_row_no_data(MYSQL_STMT *stmt [[maybe_unused]],
                                 unsigned char **row [[maybe_unused]]) {
  return MYSQL_NO_DATA;
}

static int stmt_read_row_no_result_set(MYSQL_STMT *stmt [[maybe_unused]],
                                       unsigned char **row [[maybe_unused]]) {
  set_stmt_error(stmt, CR_NO_RESULT_SET, unknown_sqlstate);
  return 1;
}

/*
  Get/set statement attributes

  SYNOPSIS
    mysql_stmt_attr_get()
    mysql_stmt_attr_set()

    attr_type  statement attribute
    value      casted to const void * pointer to value.

  RETURN VALUE
    0 success
   !0 wrong attribute type
*/

bool STDCALL mysql_stmt_attr_set(MYSQL_STMT *stmt,
                                 enum enum_stmt_attr_type attr_type,
                                 const void *value) {
  switch (attr_type) {
    case STMT_ATTR_UPDATE_MAX_LENGTH:
      stmt->update_max_length = value ? *(const bool *)value : false;
      break;
    case STMT_ATTR_CURSOR_TYPE: {
      ulong cursor_type;
      cursor_type = value ? *static_cast<const ulong *>(value) : 0UL;
      if (cursor_type > (ulong)CURSOR_TYPE_READ_ONLY) goto err_not_implemented;
      stmt->flags = cursor_type;
      break;
    }
    case STMT_ATTR_PREFETCH_ROWS: {
      const ulong prefetch_rows =
          value ? *static_cast<const ulong *>(value) : DEFAULT_PREFETCH_ROWS;
      if (value == nullptr) return true;
      stmt->prefetch_rows = prefetch_rows;
      break;
    }
    default:
      goto err_not_implemented;
  }
  return false;
err_not_implemented:
  set_stmt_error(stmt, CR_NOT_IMPLEMENTED, unknown_sqlstate);
  return true;
}

bool STDCALL mysql_stmt_attr_get(MYSQL_STMT *stmt,
                                 enum enum_stmt_attr_type attr_type,
                                 void *value) {
  switch (attr_type) {
    case STMT_ATTR_UPDATE_MAX_LENGTH:
      *(bool *)value = stmt->update_max_length;
      break;
    case STMT_ATTR_CURSOR_TYPE:
      *(ulong *)value = stmt->flags;
      break;
    case STMT_ATTR_PREFETCH_ROWS:
      *(ulong *)value = stmt->prefetch_rows;
      break;
    default:
      return true;
  }
  return false;
}

/**
  Update statement result set metadata from with the new field
  information sent during statement execute.

  @pre mysql->field_count is not zero
*/

static void reinit_result_set_metadata(MYSQL_STMT *stmt) {
  /* Server has sent result set metadata */
  if (stmt->field_count == 0) {
    /*
      This is 'SHOW'/'EXPLAIN'-like query. Current implementation of
      prepared statements can't send result set metadata for these queries
      on prepare stage. Read it now.
    */

    stmt->field_count = stmt->mysql->field_count;

    alloc_stmt_fields(stmt);
  } else {
    /*
      Update result set metadata if it for some reason changed between
      prepare and execute, i.e.:
      - in case of 'SELECT ?' we don't know column type unless data was
      supplied to mysql_stmt_execute, so updated column type is sent
      now.
      - if data dictionary changed between prepare and execute, for
      example a table used in the query was altered.
      Note, that now (4.1.3) we always send metadata in reply to
      COM_STMT_EXECUTE (even if it is not necessary), so either this or
      previous branch always works.
      TODO: send metadata only when it's really necessary and add a warning
      'Metadata changed' when it's sent twice.
    */
    update_stmt_fields(stmt);
  }
}

static void prepare_to_fetch_result(MYSQL_STMT *stmt) {
  if (stmt->server_status & SERVER_STATUS_CURSOR_EXISTS) {
    stmt->mysql->status = MYSQL_STATUS_READY;
    stmt->read_row_func = stmt_read_row_from_cursor;
  } else if (stmt->flags & CURSOR_TYPE_READ_ONLY) {
    /*
      This is a single-row result set, a result set with no rows, EXPLAIN,
      SHOW VARIABLES, or some other command which either a) bypasses the
      cursors framework in the server and writes rows directly to the
      network or b) is more efficient if all (few) result set rows are
      precached on client and server's resources are freed.
      The below check for mysql->status is required because we could
      have already read the last packet sent by the server in execute()
      and set the status to MYSQL_STATUS_READY. In such cases, we need
      not call mysql_stmt_store_result().
    */
    if (stmt->mysql->status != MYSQL_STATUS_READY)
      mysql_stmt_store_result(stmt);
  } else {
    stmt->mysql->unbuffered_fetch_owner = &stmt->unbuffered_fetch_cancelled;
    stmt->unbuffered_fetch_cancelled = false;
    stmt->read_row_func = stmt_read_row_unbuffered;
  }
}

/*
  Send placeholders data to server (if there are placeholders)
  and execute prepared statement.

  SYNOPSIS
    mysql_stmt_execute()
    stmt  statement handle. The handle must be created
          with mysql_stmt_init() and prepared with
          mysql_stmt_prepare(). If there are placeholders
          in the statement they must be bound to local
          variables with mysql_stmt_bind_named_param().

  DESCRIPTION
    This function will automatically flush pending result
    set (if there is one), send parameters data to the server
    and read result of statement execution.
    If previous result set was cached with mysql_stmt_store_result()
    it will also be freed in the beginning of this call.
    The server can return 3 types of responses to this command:
    - error, can be retrieved with mysql_stmt_error()
    - ok, no result set pending. In this case we just update
      stmt->insert_id and stmt->affected_rows.
    - the query returns a result set: there could be 0 .. N
    rows in it. In this case the server can also send updated
    result set metadata.

    Next steps you may want to make:
    - find out if there is result set with mysql_stmt_field_count().
    If there is one:
    - optionally, cache entire result set on client to unblock
    connection with mysql_stmt_store_result()
    - bind client variables to result set columns and start read rows
    with mysql_stmt_fetch().
    - reset statement with mysql_stmt_reset() or close it with
    mysql_stmt_close()
    Otherwise:
    - find out last insert id and number of affected rows with
    mysql_stmt_insert_id(), mysql_stmt_affected_rows()

  RETURN
    0   success
    1   error, message can be retrieved with mysql_stmt_error().
*/

int STDCALL mysql_stmt_execute(MYSQL_STMT *stmt) {
  MYSQL *mysql = stmt->mysql;
  DBUG_TRACE;

  if (!mysql) {
    /* Error is already set in mysql_detatch_stmt_list */
    return 1;
  }

  if (reset_stmt_handle(stmt, RESET_STORE_RESULT | RESET_CLEAR_ERROR)) return 1;
  /*
    No need to check for stmt->state: if the statement wasn't
    prepared we'll get 'unknown statement handler' error from server.
  */
  if (mysql->methods->stmt_execute(stmt)) return 1;
  stmt->state = MYSQL_STMT_EXECUTE_DONE;
  if (mysql->field_count) {
    reinit_result_set_metadata(stmt);
    prepare_to_fetch_result(stmt);
  }
  return stmt->last_errno != 0;
}

/*
  Return total parameters count in the statement
*/

ulong STDCALL mysql_stmt_param_count(MYSQL_STMT *stmt) {
  DBUG_TRACE;
  return stmt->param_count;
}

/*
  Return total affected rows from the last statement
*/

uint64_t STDCALL mysql_stmt_affected_rows(MYSQL_STMT *stmt) {
  return stmt->affected_rows;
}

/*
  Returns the number of result columns for the most recent query
  run on this statement.
*/

unsigned int STDCALL mysql_stmt_field_count(MYSQL_STMT *stmt) {
  return stmt->field_count;
}

/*
  Return last inserted id for auto_increment columns.

  SYNOPSIS
    mysql_stmt_insert_id()
    stmt    statement handle

  DESCRIPTION
    Current implementation of this call has a caveat: stmt->insert_id is
    unconditionally updated from mysql->insert_id in the end of each
    mysql_stmt_execute(). This works OK if mysql->insert_id contains new
    value (sent in reply to mysql_stmt_execute()), otherwise stmt->insert_id
    value gets undefined, as it's updated from some arbitrary value saved in
    connection structure during some other call.
*/

uint64_t STDCALL mysql_stmt_insert_id(MYSQL_STMT *stmt) {
  return stmt->insert_id;
}
/*
  Set up input data buffers for a statement.
  SYNOPSIS
    mysql_stmt_bind_param()
    stmt    statement handle
            The statement must be prepared with mysql_stmt_prepare().
    my_bind Array of mysql_stmt_param_count() bind parameters.
            This function doesn't check that size of this argument
            is >= mysql_stmt_field_count(): it's user's responsibility.
  DESCRIPTION
    @deprecated This function is deprecated and will be removed in a future
                version. Use mysq_stmt_bind_named_param() instead.

    Use this call after mysql_stmt_prepare() to bind user variables to
    placeholders.

    Each element of bind array stands for a placeholder. Placeholders
    are counted from 0.  For example statement
    'INSERT INTO t (a, b) VALUES (?, ?)'
    contains two placeholders, and for such statement you should supply
    bind array of two elements (MYSQL_BIND bind[2]).

    By properly initializing bind array you can bind virtually any
    C language type to statement's placeholders:
    First, it's strongly recommended to always zero-initialize entire
    bind structure before setting its members. This will both shorten
    your application code and make it robust to future extensions of
    MYSQL_BIND structure.
    Then you need to assign typecode of your application buffer to
    MYSQL_BIND::buffer_type. The following typecodes with their
    correspondence to C language types are supported:
    MYSQL_TYPE_TINY       for 8-bit integer variables. Normally it's
                          'signed char' and 'unsigned char';
    MYSQL_TYPE_SHORT      for 16-bit signed and unsigned variables. This
                          is usually 'short' and 'unsigned short';
    MYSQL_TYPE_LONG       for 32-bit signed and unsigned variables. It
                          corresponds to 'int' and 'unsigned int' on
                          vast majority of platforms. On IA-32 and some
                          other 32-bit systems you can also use 'long'
                          here;
    MYSQL_TYPE_LONGLONG   64-bit signed or unsigned integer.  Stands for
                          '[unsigned] long long' on most platforms;
    MYSQL_TYPE_FLOAT      32-bit floating point type, 'float' on most
                          systems;
    MYSQL_TYPE_DOUBLE     64-bit floating point type, 'double' on most
                          systems;
    MYSQL_TYPE_TIME       broken-down time stored in MYSQL_TIME
                          structure
    MYSQL_TYPE_DATE       date stored in MYSQL_TIME structure
    MYSQL_TYPE_DATETIME   datetime stored in MYSQL_TIME structure See
                          more on how to use these types for sending
                          dates and times below;
    MYSQL_TYPE_STRING     character string, assumed to be in
                          character-set-client. If character set of
                          client is not equal to character set of
                          column, value for this placeholder will be
                          converted to destination character set before
                          insert.
    MYSQL_TYPE_BLOB       sequence of bytes. This sequence is assumed to
                          be in binary character set (which is the same
                          as no particular character set), and is never
                          converted to any other character set. See also
                          notes about supplying string/blob length
                          below.
    MYSQL_TYPE_NULL       special typecode for binding nulls.
    These C/C++ types are not supported yet by the API: long double,
    bool.

    As you can see from the list above, it's responsibility of
    application programmer to ensure that chosen typecode properly
    corresponds to host language type. For example on all platforms
    where we build MySQL packages (as of MySQL 4.1.4) int is a 32-bit
    type. So for int you can always assume that proper typecode is
    MYSQL_TYPE_LONG (however queer it sounds, the name is legacy of the
    old MySQL API). In contrary sizeof(long) can be 4 or 8 8-bit bytes,
    depending on platform.

    TODO: provide client typedefs for each integer and floating point
    typecode, i. e. int8, uint8, float32, etc.

    Once typecode was set, it's necessary to assign MYSQL_BIND::buffer
    to point to the buffer of given type. Finally, additional actions
    may be taken for some types or use cases:

    Binding integer types.
      For integer types you might also need to set MYSQL_BIND::is_unsigned
      member. Set it to true when binding unsigned char, unsigned short,
      unsigned int, unsigned long, unsigned long long.

    Binding floating point types.
      For floating point types you just need to set
      MYSQL_BIND::buffer_type and MYSQL_BIND::buffer. The rest of the
      members should be zero-initialized.

    Binding NULLs.
      You might have a column always NULL, never NULL, or sometimes
      NULL.  For an always NULL column set MYSQL_BIND::buffer_type to
      MYSQL_TYPE_NULL.  The rest of the members just need to be
      zero-initialized.  For never NULL columns set
      MYSQL_BIND::is_null to 0, or this has already been done if you
      zero-initialized the entire structure.  If you set
      MYSQL_TYPE::is_null to point to an application buffer of type
      'bool', then this buffer will be checked on each execution:
      this way you can set the buffer to true, or any non-0 value for
      NULLs, and to false or 0 for not NULL data.

    Binding text strings and sequences of bytes.
      For strings, in addition to MYSQL_BIND::buffer_type and
      MYSQL_BIND::buffer you need to set MYSQL_BIND::length or
      MYSQL_BIND::buffer_length.  If 'length' is set, 'buffer_length'
      is ignored. 'buffer_length' member should be used when size of
      string doesn't change between executions. If you want to vary
      buffer length for each value, set 'length' to point to an
      application buffer of type 'unsigned long' and set this long to
      length of the string before each mysql_stmt_execute().

    Binding dates and times.
      For binding dates and times prepared statements API provides
      clients with MYSQL_TIME structure. A pointer to instance of this
      structure should be assigned to MYSQL_BIND::buffer whenever
      MYSQL_TYPE_TIME, MYSQL_TYPE_DATE, MYSQL_TYPE_DATETIME typecodes
      are used.  When typecode is MYSQL_TYPE_TIME, only members
      'hour', 'minute', 'second' and 'neg' (is time offset negative)
      are used. These members only will be sent to the server.
      MYSQL_TYPE_DATE implies use of 'year', 'month', 'day', 'neg'.
      MYSQL_TYPE_DATETIME utilizes both parts of MYSQL_TIME structure.
      You don't have to set MYSQL_TIME::time_type member: it's not
      used when sending data to the server, typecode information is
      enough.  'second_part' member can hold microsecond precision of
      time value, but now it's only supported on protocol level: you
      can't store microsecond in a column, or use in temporal
      calculations. However, if you send a time value with microsecond
      part for 'SELECT ?', statement, you'll get it back unchanged
      from the server.

    Data conversion.
      If conversion from host language type to data representation,
      corresponding to SQL type, is required it's done on the server.
      Data truncation is possible when conversion is lossy. For
      example, if you supply MYSQL_TYPE_DATETIME value out of valid
      SQL type TIMESTAMP range, the same conversion will be applied as
      if this value would have been sent as string in the old
      protocol.  TODO: document how the server will behave in case of
      truncation/data loss.

    After variables were bound, you can repeatedly set/change their
    values and mysql_stmt_execute() the statement.

    See also: mysql_stmt_send_long_data() for sending long text/blob
    data in pieces, examples in tests/mysql_client_test.c.
    Next steps you might want to make:
    - execute statement with mysql_stmt_execute(),
    - reset statement using mysql_stmt_reset() or reprepare it with
      another query using mysql_stmt_prepare()
    - close statement with mysql_stmt_close().

  IMPLEMENTATION
    The function copies given bind array to internal storage of the
    statement, and sets up typecode-specific handlers to perform
    serialization of bound data. This means that although you don't need
    to call this routine after each assignment to bind buffers, you
    need to call it each time you change parameter typecodes, or other
    members of MYSQL_BIND array.
    This is a pure local call. Data types of client buffers are sent
    along with buffers' data at first execution of the statement.
  RETURN
    0  success
    1  error, can be retrieved with mysql_stmt_error.
*/

bool STDCALL mysql_stmt_bind_param(MYSQL_STMT *stmt, MYSQL_BIND *my_bind) {
  return mysql_stmt_bind_named_param(stmt, my_bind, stmt->param_count, nullptr);
}
bool STDCALL mysql_bind_param(MYSQL *mysql, unsigned n_params,
                              MYSQL_BIND *binds, const char **names) {
  MYSQL_EXTENSION *ext = MYSQL_EXTENSION_PTR(mysql);
  mysql_extension_bind_free(ext);
  /* if any of the above is empty our work here is done */
  if (!n_params || !binds || !names) return false;
  ext->bind_info.n_params = n_params;
  ext->bind_info.bind = (MYSQL_BIND *)my_malloc(
      PSI_NOT_INSTRUMENTED, sizeof(MYSQL_BIND) * n_params, MYF(0));
  ext->bind_info.names = (char **)my_malloc(PSI_NOT_INSTRUMENTED,
                                            sizeof(char *) * n_params, MYF(0));
  memcpy(ext->bind_info.bind, binds, sizeof(MYSQL_BIND) * n_params);
  MYSQL_BIND *param = ext->bind_info.bind;
  for (uint idx = 0; idx < n_params; idx++, param++) {
    ext->bind_info.names[idx] =
        names[idx] ? my_strdup(PSI_NOT_INSTRUMENTED, names[idx], MYF(0))
                   : nullptr;
    if (fix_param_bind(param, idx)) {
      my_stpcpy(mysql->net.sqlstate, unknown_sqlstate);
      sprintf(mysql->net.last_error,
              ER_CLIENT(mysql->net.last_errno = CR_UNSUPPORTED_PARAM_TYPE),
              param->buffer_type, idx);
      for (uint idx2 = 0; idx2 <= idx; idx2++)
        my_free(ext->bind_info.names[idx2]);
      my_free(ext->bind_info.names);
      my_free(ext->bind_info.bind);
      memset(&ext->bind_info, 0, sizeof(ext->bind_info));
      return true;
    }
  }
  return false;
}

/*
  Set up unnamed and named (query attributes) bind parameters for a statement.

  SYNOPSIS
    mysql_stmt_bind_named_param()
    stmt    statement handle
            The statement must be prepared with mysql_stmt_prepare().
    binds   Array of named bind parameters.
    n_params Number of items within arrays.
    names   Array of bind parameter names.

  DESCRIPTION
    Use this call after mysql_stmt_prepare() to store both named and
    unnamed bind user variables (query attributes).

    After variables were bound, you can repeatedly set/change their
    values and mysql_stmt_execute() the statement.

    Note that calling both mysql_stmt_bind_param() and
  mysql_stmt_bind_named_param() is not additive, each call overwrites the
  settings set by the previous call.

    See also: mysql_stmt_send_long_data() for sending long text/blob
    data in pieces, examples in tests/mysql_client_test.c.
    Next steps you might want to make:
    - execute statement with mysql_stmt_execute(),
    - reset statement using mysql_stmt_reset() or reprepare it with
      another query using mysql_stmt_prepare()
    - close statement with mysql_stmt_close().

    The following example demonstrates binding one unnamed and one
    named parameter, notice the unnamed parameter having NULL in a matching
    names array slot:

      MYSQL_STMT *stmt;
      stmt = mysql_simple_prepare(mysql, "SELECT POW(?,2) AS square");

      int int_data = 4;  // unnamed input parameter
      int int_parentid = 1329494394;  // named input parameter ("traceparent")
      MYSQL_BIND all_params[2];
      memset(all_params, 0, sizeof(all_params));

      all_params[0].buffer_type = MYSQL_TYPE_LONG;
      all_params[0].buffer = (char *)&int_data;
      all_params[0].length = nullptr;
      all_params[0].is_null = nullptr;

      all_params[1].buffer_type = MYSQL_TYPE_LONG;
      all_params[1].buffer = (char *)&int_parentid;
      all_params[1].length = nullptr;
      all_params[1].is_null = nullptr;

      const char *names[2] = {nullptr, "traceparent"};
      int rc = mysql_stmt_bind_named_param(stmt, all_params,
  std::size(all_params), names);
      // after executing, binding result and fetching data, result should be 16,
  i.e. 4 squared

    In case you don't have any named parameters, you can pass NULL for the names
    array parameter.

    As a corner case, when passing NULL for binds array or 0 for its size, the
  API will return success (nothing to be bound).

  IMPLEMENTATION
    The function copies given bind array to internal storage of the
    statement, and sets up typecode-specific handlers to perform
    serialization of bound data. This means that although you don't need
    to call this routine after each assignment to bind buffers, you
    need to call it each time you change parameter typecodes, or other
    members of MYSQL_BIND array.
    This is a pure local call. Data types of client buffers are sent
    along with buffers' data at first execution of the statement.

  RETURN
    0  success
    1  error, can be retrieved with mysql_stmt_error.
*/

bool STDCALL mysql_stmt_bind_named_param(MYSQL_STMT *stmt, MYSQL_BIND *binds,
                                         unsigned n_params,
                                         const char **names) {
  DBUG_TRACE;

  MYSQL_STMT_EXT *ext = stmt->extension;

  mysql_stmt_extension_bind_free(ext);
  stmt->params = nullptr;

  if (!stmt->param_count) {
    if ((int)stmt->state < (int)MYSQL_STMT_PREPARE_DONE) {
      set_stmt_error(stmt, CR_NO_PREPARE_STMT, unknown_sqlstate);
      return true;
    }
  }

  /* if any of the below is empty our work here is done */
  if (!n_params || !binds) return false;

  if (!(stmt->params = (MYSQL_BIND *)stmt_ext_allocate(
            ext, sizeof(MYSQL_BIND) * n_params))) {
    set_stmt_error(stmt, CR_OUT_OF_MEMORY, unknown_sqlstate);
    return true;
  }

  // bind result parameters may have already been allocated separately
  if (stmt->bind == nullptr) {
    if (!(stmt->bind = (MYSQL_BIND *)ext->fields_mem_root.Alloc(
              sizeof(MYSQL_BIND) * stmt->field_count))) {
      set_stmt_error(stmt, CR_OUT_OF_MEMORY, unknown_sqlstate);
      return true;
    }
  }

  // copy binds array
  memcpy((char *)stmt->params, (char *)binds, sizeof(MYSQL_BIND) * n_params);

  // copy names array
  ext->bind_data.n_params = n_params;
  ext->bind_data.names =
      (char **)stmt_ext_allocate(ext, sizeof(char *) * n_params);

  MYSQL_BIND *param = stmt->params;
  for (uint idx = 0; idx < n_params; idx++, param++) {
    if (names && names[idx]) {
      const size_t len = strlen(names[idx]) + 1;
      ext->bind_data.names[idx] = (char *)stmt_ext_allocate(ext, len);
      memcpy(ext->bind_data.names[idx], names[idx], len);
    } else {
      ext->bind_data.names[idx] = nullptr;
    }
    if (fix_param_bind(param, idx)) {
      set_stmt_error(stmt, CR_UNSUPPORTED_PARAM_TYPE, unknown_sqlstate);
      mysql_stmt_extension_bind_free(ext);
      return true;
    }
  }

  /* We have to send/resend type information to MySQL */
  stmt->send_types_to_server = true;
  stmt->bind_param_done = true;
  return false;
}

/********************************************************************
 Long data implementation
*********************************************************************/

/*
  Send long data in pieces to the server

  SYNOPSIS
    mysql_stmt_send_long_data()
    stmt			Statement handler
    param_number		Parameter number (0 - N-1)
    data			Data to send to server
    length			Length of data to send (may be 0)

  DESCRIPTION
    This call can be used repeatedly to send long data in pieces
    for any string/binary placeholder. Data supplied for
    a placeholder is saved at server side till execute, and then
    used instead of value from MYSQL_BIND object. More precisely,
    if long data for a parameter was supplied, MYSQL_BIND object
    corresponding to this parameter is not sent to server. In the
    end of execution long data states of placeholders are reset,
    so next time values of such placeholders will be taken again
    from MYSQL_BIND array.
    The server does not reply to this call: if there was an error
    in data handling (which now only can happen if server run out
    of memory) it would be returned in reply to
    mysql_stmt_execute().
    You should choose type of long data carefully if you care
    about character set conversions performed by server when the
    statement is executed.  No conversion is performed at all for
    MYSQL_TYPE_BLOB and other binary typecodes. For
    MYSQL_TYPE_STRING and the rest of text placeholders data is
    converted from client character set to character set of
    connection. If these character sets are different, this
    conversion may require additional memory at server, equal to
    total size of supplied pieces.

  RETURN VALUES
    0	ok
    1	error
*/

bool STDCALL mysql_stmt_send_long_data(MYSQL_STMT *stmt, uint param_number,
                                       const char *data, ulong length) {
  MYSQL_BIND *param;
  DBUG_TRACE;
  assert(stmt != nullptr);
  DBUG_PRINT("enter", ("param no: %d  data: %p, length : %ld", param_number,
                       data, length));

  /*
    We only need to check for stmt->param_count, if it's not null
    prepare was done.
  */
  if (param_number >= stmt->param_count) {
    set_stmt_error(stmt, CR_INVALID_PARAMETER_NO, unknown_sqlstate);
    return true;
  }

  param = stmt->params + param_number;
  if (!IS_LONGDATA(param->buffer_type)) {
    /* Long data handling should be used only for string/binary types */
    my_stpcpy(stmt->sqlstate, unknown_sqlstate);
    sprintf(stmt->last_error,
            ER_CLIENT(stmt->last_errno = CR_INVALID_BUFFER_USE),
            param->param_number);
    return true;
  }

  /*
    Send long data packet if there is data or we're sending long data
    for the first time.
  */
  if (length || param->long_data_used == 0) {
    MYSQL *mysql = stmt->mysql;
    /* Packet header: stmt id (4 bytes), param no (2 bytes) */
    uchar buff[MYSQL_LONG_DATA_HEADER];

    int4store(buff, stmt->stmt_id);
    int2store(buff + 4, param_number);
    param->long_data_used = true;

    /*
      Note that we don't get any ok packet from the server in this case
      This is intentional to save bandwidth.
    */
    if ((*mysql->methods->advanced_command)(
            mysql, COM_STMT_SEND_LONG_DATA, buff, sizeof(buff),
            pointer_cast<const uchar *>(data), length, true, stmt)) {
      /*
        Don't set stmt error if stmt->mysql is NULL, as the error in this case
        has already been set by mysql_prune_stmt_list().
      */
      if (stmt->mysql) set_stmt_errmsg(stmt, &mysql->net);
      return true;
    }
  }
  return false;
}

/********************************************************************
 Fetch and conversion of result set rows (binary protocol).
*********************************************************************/

/*
  Read date, (time, datetime) value from network buffer and store it
  in MYSQL_TIME structure.

  SYNOPSIS
    read_binary_{date,time,datetime}()
    tm    MYSQL_TIME structure to fill
    pos   pointer to current position in network buffer.
          These functions increase pos to point to the beginning of the
          next column.

  Auxiliary functions to read time (date, datetime) values from network
  buffer and store in MYSQL_TIME structure. Jointly used by conversion
  and no-conversion fetching.
*/

static void read_binary_time(MYSQL_TIME *tm, uchar **pos) {
  /* net_field_length will set pos to the first byte of data */
  const uint length = net_field_length(pos);

  if (length) {
    uchar *to = *pos;
    tm->neg = to[0];

    tm->day = (ulong)sint4korr(to + 1);
    tm->hour = (uint)to[5];
    tm->minute = (uint)to[6];
    tm->second = (uint)to[7];
    tm->second_part = (length > 8) ? (ulong)sint4korr(to + 8) : 0;
    tm->year = tm->month = 0;
    if (tm->day) {
      /* Convert days to hours at once */
      tm->hour += tm->day * 24;
      tm->day = 0;
    }
    tm->time_type = MYSQL_TIMESTAMP_TIME;

    *pos += length;
  } else
    set_zero_time(tm, MYSQL_TIMESTAMP_TIME);
}

static void read_binary_datetime(MYSQL_TIME *tm, uchar **pos) {
  const uint length = net_field_length(pos);

  if (length) {
    uchar *to = *pos;

    tm->neg = false;
    tm->year = (uint)sint2korr(to);
    tm->month = (uint)to[2];
    tm->day = (uint)to[3];

    if (length > 4) {
      tm->hour = (uint)to[4];
      tm->minute = (uint)to[5];
      tm->second = (uint)to[6];
    } else
      tm->hour = tm->minute = tm->second = 0;
    tm->second_part = (length > 7) ? (ulong)sint4korr(to + 7) : 0;
    tm->time_type = MYSQL_TIMESTAMP_DATETIME;

    *pos += length;
  } else
    set_zero_time(tm, MYSQL_TIMESTAMP_DATETIME);
}

static void read_binary_date(MYSQL_TIME *tm, uchar **pos) {
  const uint length = net_field_length(pos);

  if (length) {
    uchar *to = *pos;
    tm->year = (uint)sint2korr(to);
    tm->month = (uint)to[2];
    tm->day = (uint)to[3];

    tm->hour = tm->minute = tm->second = 0;
    tm->second_part = 0;
    tm->neg = false;
    tm->time_type = MYSQL_TIMESTAMP_DATE;

    *pos += length;
  } else
    set_zero_time(tm, MYSQL_TIMESTAMP_DATE);
}

/*
  Convert string to supplied buffer of any type.

  SYNOPSIS
    fetch_string_with_conversion()
    param   output buffer descriptor
    value   column data
    length  data length
*/

static void fetch_string_with_conversion(MYSQL_BIND *param, char *value,
                                         size_t length) {
  auto *buffer = pointer_cast<uchar *>(param->buffer);
  const char *endptr = value + length;

  /*
    This function should support all target buffer types: the rest
    of conversion functions can delegate conversion to it.
  */
  switch (param->buffer_type) {
    case MYSQL_TYPE_NULL: /* do nothing */
      break;
    case MYSQL_TYPE_TINY: {
      int err;
      const longlong data = my_strtoll10(value, &endptr, &err);
      *param->error = (IS_TRUNCATED(data, param->is_unsigned, INT_MIN8,
                                    INT_MAX8, UINT_MAX8) ||
                       err > 0);
      *buffer = (uchar)data;
      break;
    }
    case MYSQL_TYPE_SHORT: {
      int err;
      const longlong data = my_strtoll10(value, &endptr, &err);
      *param->error = (IS_TRUNCATED(data, param->is_unsigned, INT_MIN16,
                                    INT_MAX16, UINT_MAX16) ||
                       err > 0);
      shortstore(buffer, (short)data);
      break;
    }
    case MYSQL_TYPE_LONG: {
      int err;
      const longlong data = my_strtoll10(value, &endptr, &err);
      *param->error = (IS_TRUNCATED(data, param->is_unsigned, INT_MIN32,
                                    INT_MAX32, UINT_MAX32) ||
                       err > 0);
      longstore(buffer, (int32)data);
      break;
    }
    case MYSQL_TYPE_LONGLONG: {
      int err;
      const longlong data = my_strtoll10(value, &endptr, &err);
      *param->error =
          param->is_unsigned ? err != 0 : (err > 0 || (err == 0 && data < 0));
      longlongstore(buffer, data);
      break;
    }
    case MYSQL_TYPE_FLOAT: {
      int err;
      const double data =
          my_strntod(&my_charset_latin1, value, length, &endptr, &err);
      const auto fdata = (float)data;
      *param->error = (fdata != data) | (err != 0);
      floatstore(buffer, fdata);
      break;
    }
    case MYSQL_TYPE_DOUBLE: {
      int err;
      const double data =
          my_strntod(&my_charset_latin1, value, length, &endptr, &err);
      *param->error = (err != 0);
      doublestore(buffer, data);
      break;
    }
    case MYSQL_TYPE_TIME: {
      MYSQL_TIME_STATUS status;
      auto *tm = (MYSQL_TIME *)buffer;
      str_to_time(value, length, tm, &status);
      *param->error = (status.warnings != 0);
      break;
    }
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP: {
      MYSQL_TIME_STATUS status;
      auto *tm = (MYSQL_TIME *)buffer;
      (void)str_to_datetime(value, length, tm, TIME_FUZZY_DATE, &status);
      *param->error =
          (status.warnings != 0) && (param->buffer_type == MYSQL_TYPE_DATE &&
                                     tm->time_type != MYSQL_TIMESTAMP_DATE);
      break;
    }
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VECTOR:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    default: {
      /*
        Copy column data to the buffer taking into account offset,
        data length and buffer length.
      */
      char *start = value + param->offset;
      char *end = value + length;
      size_t copy_length;
      if (start < end) {
        copy_length = end - start;
        /* We've got some data beyond offset: copy up to buffer_length bytes */
        if (param->buffer_length)
          memcpy(buffer, start,
                 std::min<size_t>(copy_length, param->buffer_length));
      } else
        copy_length = 0;
      if (copy_length < param->buffer_length) buffer[copy_length] = '\0';
      *param->error = copy_length > param->buffer_length;
      /*
        param->length will always contain length of entire column;
        number of copied bytes may be way different:
      */
      *param->length = (unsigned long)length;
      break;
    }
  }
}

// Convert an integer (signed or unsigned) to float/double, with checking
// for loss of precision in the conversion. (double can represent all integers
// up to 2^53 exactly, but only certain integers above this limit. For instance,
// 2^53 + 1 is rounded off, while 2^53 + 2 is exact.)
template <class Int, class Float>
static inline Float convert_with_inexact_check(Int i, bool *is_inexact) {
  /*
    We need to mark the local variable volatile to
    workaround Intel FPU executive precision feature.
    (See http://gcc.gnu.org/bugzilla/show_bug.cgi?id=323 for details)
   */
  volatile auto f = static_cast<Float>(i);

  // If i is positive, it is possible for it to have been rounded outside
  // Int's range. If so, converting back to check is undefined behavior,
  // and UBSan will complain. Thus, we need to check before we convert.
  // Rounding of 2^64 - 1 (and similarly for int64_t) to float/double is
  // platform-dependent, so we need a bit of trickery to get the right
  // value in a safe manner.
  //
  // For both int64_t and uint64_t, the minimum possible value is a
  // (negative) power of two, which is exact, so the test is applicable
  // for max() only.
  constexpr Float out_of_range =
      static_cast<Float>(1ULL << (std::numeric_limits<Int>::digits - 1)) *
      (std::numeric_limits<Int>::is_signed ? 1.0 : 2.0);
  if (f >= out_of_range) {
    // Obviously inexact.
    *is_inexact = true;
  } else {
    *is_inexact = static_cast<Int>(f) != i;
  }
  return f;
}

/*
  Convert integer value to client buffer of any type.

  SYNOPSIS
    fetch_long_with_conversion()
    param   output buffer descriptor
    field   column metadata
    value   column data
*/

static void fetch_long_with_conversion(MYSQL_BIND *param, MYSQL_FIELD *field,
                                       longlong value, bool is_unsigned) {
  auto *buffer = pointer_cast<uchar *>(param->buffer);

  switch (param->buffer_type) {
    case MYSQL_TYPE_NULL: /* do nothing */
      break;
    case MYSQL_TYPE_TINY:
      *param->error = IS_TRUNCATED(value, param->is_unsigned, INT_MIN8,
                                   INT_MAX8, UINT_MAX8);
      *(uchar *)param->buffer = (uchar)value;
      break;
    case MYSQL_TYPE_SHORT:
      *param->error = IS_TRUNCATED(value, param->is_unsigned, INT_MIN16,
                                   INT_MAX16, UINT_MAX16);
      shortstore(buffer, (short)value);
      break;
    case MYSQL_TYPE_LONG:
      *param->error = IS_TRUNCATED(value, param->is_unsigned, INT_MIN32,
                                   INT_MAX32, UINT_MAX32);
      longstore(buffer, (int32)value);
      break;
    case MYSQL_TYPE_LONGLONG:
      longlongstore(buffer, value);
      *param->error = param->is_unsigned != is_unsigned && value < 0;
      break;
    case MYSQL_TYPE_FLOAT: {
      float data;
      if (is_unsigned) {
        data =
            convert_with_inexact_check<ulonglong, float>(value, param->error);
      } else {
        data = convert_with_inexact_check<longlong, float>(value, param->error);
      }
      floatstore(buffer, data);
      break;
    }
    case MYSQL_TYPE_DOUBLE: {
      double data;
      if (is_unsigned) {
        data =
            convert_with_inexact_check<ulonglong, double>(value, param->error);
      } else {
        data =
            convert_with_inexact_check<longlong, double>(value, param->error);
      }
      doublestore(buffer, data);
      break;
    }
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_DATETIME: {
      int error;
      value = number_to_datetime(value, (MYSQL_TIME *)buffer, TIME_FUZZY_DATE,
                                 &error);
      *param->error = (error != 0);
      break;
    }
    default: {
      uchar buff[22]; /* Enough for longlong */
      auto *end = (uchar *)longlong10_to_str(value, (char *)buff,
                                             is_unsigned ? 10 : -10);
      /* Resort to string conversion which supports all typecodes */
      uint length = (uint)(end - buff);

      if (field->flags & ZEROFILL_FLAG && length < field->length &&
          field->length < 21) {
        memmove(buff + field->length - length, buff, length);
        memset(buff, '0', field->length - length);
        length = field->length;
      }
      fetch_string_with_conversion(param, (char *)buff, length);
      break;
    }
  }
}

/*
  Convert double/float column to supplied buffer of any type.

  SYNOPSIS
    fetch_float_with_conversion()
    param   output buffer descriptor
    field   column metadata
    value   column data
    type    either MY_GCVT_ARG_FLOAT or MY_GCVT_ARG_DOUBLE.
            Affects the maximum number of significant digits
            returned by my_gcvt().
*/

static void fetch_float_with_conversion(MYSQL_BIND *param, MYSQL_FIELD *field,
                                        double value, my_gcvt_arg_type type) {
  auto *buffer = pointer_cast<uchar *>(param->buffer);
  const double val64 = (value < 0 ? -floor(-value) : floor(value));

  switch (param->buffer_type) {
    case MYSQL_TYPE_NULL: /* do nothing */
      break;
    case MYSQL_TYPE_TINY:
      /*
        We need to _store_ data in the buffer before the truncation check to
        workaround Intel FPU executive precision feature.
        (See http://gcc.gnu.org/bugzilla/show_bug.cgi?id=323 for details)
        Sic: AFAIU it does not guarantee to work.
      */
      if (param->is_unsigned) {
        if (value < 0.0) {
          *param->error = true;
          break;
        }
        *buffer = (uint8)value;
      } else {
        *buffer = (int8)value;
      }
      *param->error = val64 != (param->is_unsigned ? (double)((uint8)*buffer)
                                                   : (double)((int8)*buffer));
      break;
    case MYSQL_TYPE_SHORT:
      if (param->is_unsigned) {
        if (value < 0.0) {
          *param->error = true;
          break;
        }
        const auto data = (ushort)value;
        shortstore(buffer, data);
      } else {
        const auto data = (short)value;
        shortstore(buffer, data);
      }
      *param->error =
          val64 != (param->is_unsigned ? (double)(*(ushort *)buffer)
                                       : (double)(*(short *)buffer));
      break;
    case MYSQL_TYPE_LONG:
      if (param->is_unsigned) {
        if (value < 0.0) {
          *param->error = true;
          break;
        }
        const auto data = (uint32)value;
        longstore(buffer, data);
      } else {
        const auto data = (int32)value;
        longstore(buffer, data);
      }
      *param->error =
          val64 != (param->is_unsigned ? (double)(*(uint32 *)buffer)
                                       : (double)(*(int32 *)buffer));
      break;
    case MYSQL_TYPE_LONGLONG:
      if (param->is_unsigned) {
        if (value < 0.0) {
          *param->error = true;
          break;
        }
        const auto data = (ulonglong)value;
        longlongstore(buffer, data);
      } else {
        const auto data = (longlong)value;
        longlongstore(buffer, data);
      }
      *param->error =
          val64 != (param->is_unsigned ? ulonglong2double(*(ulonglong *)buffer)
                                       : (double)(*(longlong *)buffer));
      break;
    case MYSQL_TYPE_FLOAT: {
      const auto data = (float)value;
      floatstore(buffer, data);
      *param->error = (*(float *)buffer) != value;
      break;
    }
    case MYSQL_TYPE_DOUBLE: {
      doublestore(buffer, value);
      break;
    }
    default: {
      /*
        Resort to fetch_string_with_conversion: this should handle
        floating point -> string conversion nicely, honor all typecodes
        and param->offset possibly set in mysql_stmt_fetch_column
      */
      char buff[FLOATING_POINT_BUFFER];
      size_t len;
      if (field->decimals >= DECIMAL_NOT_SPECIFIED)
        len = my_gcvt(
            value, type,
            std::min<unsigned long>(sizeof(buff) - 1, param->buffer_length),
            buff, nullptr);
      else
        len = my_fcvt(value, (int)field->decimals, buff, nullptr);

      if (field->flags & ZEROFILL_FLAG && len < field->length &&
          field->length < MAX_DOUBLE_STRING_REP_LENGTH - 1) {
        memmove(buff + field->length - len, buff, len);
        memset(buff, '0', field->length - len);
        len = field->length;
      }
      fetch_string_with_conversion(param, buff, len);

      break;
    }
  }
}

/*
  Fetch time/date/datetime to supplied buffer of any type

  SYNOPSIS
    param   output buffer descriptor
    time    column data
*/

static void fetch_datetime_with_conversion(MYSQL_BIND *param,
                                           MYSQL_FIELD *field,
                                           MYSQL_TIME *my_time) {
  switch (param->buffer_type) {
    case MYSQL_TYPE_NULL: /* do nothing */
      break;
    case MYSQL_TYPE_DATE:
      *(MYSQL_TIME *)(param->buffer) = *my_time;
      *param->error = my_time->time_type != MYSQL_TIMESTAMP_DATE;
      break;
    case MYSQL_TYPE_TIME:
      *(MYSQL_TIME *)(param->buffer) = *my_time;
      *param->error = my_time->time_type != MYSQL_TIMESTAMP_TIME;
      break;
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      *(MYSQL_TIME *)(param->buffer) = *my_time;
      /* No error: time and date are compatible with datetime */
      break;
    case MYSQL_TYPE_YEAR:
      shortstore(pointer_cast<uchar *>(param->buffer), my_time->year);
      *param->error = true;
      break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE: {
      const ulonglong value = TIME_to_ulonglong(*my_time);
      fetch_float_with_conversion(param, field, ulonglong2double(value),
                                  MY_GCVT_ARG_DOUBLE);
      break;
    }
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      const auto value = (longlong)TIME_to_ulonglong(*my_time);
      fetch_long_with_conversion(param, field, value, true);
      break;
    }
    default: {
      /*
        Convert time value  to string and delegate the rest to
        fetch_string_with_conversion:
      */
      char buff[MAX_DATE_STRING_REP_LENGTH];
      const uint length = my_TIME_to_str(*my_time, buff, field->decimals);
      /* Resort to string conversion */
      fetch_string_with_conversion(param, (char *)buff, length);
      break;
    }
  }
}

/*
  Fetch and convert result set column to output buffer.

  SYNOPSIS
    fetch_result_with_conversion()
    param   output buffer descriptor
    field   column metadata
    row     points to a column of result set tuple in binary format

  DESCRIPTION
    This is a fallback implementation of column fetch used
    if column and output buffer types do not match.
    Increases tuple pointer to point at the next column within the
    tuple.
*/

static void fetch_result_with_conversion(MYSQL_BIND *param, MYSQL_FIELD *field,
                                         uchar **row) {
  const enum enum_field_types field_type = field->type;
  const uint field_is_unsigned = field->flags & UNSIGNED_FLAG;

  switch (field_type) {
    case MYSQL_TYPE_BOOL:
    case MYSQL_TYPE_TINY: {
      const uchar value = **row;
      /* sic: we need to cast to 'signed char' as 'char' may be unsigned */
      const longlong data =
          field_is_unsigned ? (longlong)value : (longlong)(signed char)value;
      fetch_long_with_conversion(param, field, data, false);
      *row += 1;
      break;
    }
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_YEAR: {
      const short value = sint2korr(*row);
      const longlong data =
          field_is_unsigned ? (longlong)(unsigned short)value : (longlong)value;
      fetch_long_with_conversion(param, field, data, false);
      *row += 2;
      break;
    }
    case MYSQL_TYPE_INT24: /* mediumint is sent as 4 bytes int */
    case MYSQL_TYPE_LONG: {
      const int32 value = sint4korr(*row);
      const longlong data =
          field_is_unsigned ? (longlong)(uint32)value : (longlong)value;
      fetch_long_with_conversion(param, field, data, false);
      *row += 4;
      break;
    }
    case MYSQL_TYPE_LONGLONG: {
      const auto value = (longlong)sint8korr(*row);
      fetch_long_with_conversion(param, field, value,
                                 field->flags & UNSIGNED_FLAG);
      *row += 8;
      break;
    }
    case MYSQL_TYPE_FLOAT: {
      const float value = float4get(*row);
      fetch_float_with_conversion(param, field, value, MY_GCVT_ARG_FLOAT);
      *row += 4;
      break;
    }
    case MYSQL_TYPE_DOUBLE: {
      const double value = float8get(*row);
      fetch_float_with_conversion(param, field, value, MY_GCVT_ARG_DOUBLE);
      *row += 8;
      break;
    }
    case MYSQL_TYPE_DATE: {
      MYSQL_TIME tm;

      read_binary_date(&tm, row);
      fetch_datetime_with_conversion(param, field, &tm);
      break;
    }
    case MYSQL_TYPE_TIME: {
      MYSQL_TIME tm;

      read_binary_time(&tm, row);
      fetch_datetime_with_conversion(param, field, &tm);
      break;
    }
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP: {
      MYSQL_TIME tm;

      read_binary_datetime(&tm, row);
      fetch_datetime_with_conversion(param, field, &tm);
      break;
    }
    default: {
      const ulong length = net_field_length(row);
      fetch_string_with_conversion(param, (char *)*row, length);
      *row += length;
      break;
    }
  }
}

/*
  Functions to fetch data to application buffers without conversion.

  All functions have the following characteristics:

  SYNOPSIS
    fetch_result_xxx()
    param   MySQL bind param
    pos     Row value

  DESCRIPTION
    These are no-conversion functions, used in binary protocol to store
    rows in application buffers. A function used only if type of binary data
    is compatible with type of application buffer.

  RETURN
    none
*/

static void fetch_result_tinyint(MYSQL_BIND *param, MYSQL_FIELD *field,
                                 uchar **row) {
  const bool field_is_unsigned = (field->flags & UNSIGNED_FLAG);
  const uchar data = **row;
  *(uchar *)param->buffer = data;
  *param->error = param->is_unsigned != field_is_unsigned && data > INT_MAX8;
  (*row)++;
}

static void fetch_result_short(MYSQL_BIND *param, MYSQL_FIELD *field,
                               uchar **row) {
  const bool field_is_unsigned = (field->flags & UNSIGNED_FLAG);
  auto data = (ushort)sint2korr(*row);
  shortstore(pointer_cast<uchar *>(param->buffer), data);
  *param->error = param->is_unsigned != field_is_unsigned && data > INT_MAX16;
  *row += 2;
}

static void fetch_result_int32(MYSQL_BIND *param,
                               MYSQL_FIELD *field [[maybe_unused]],
                               uchar **row) {
  const bool field_is_unsigned = (field->flags & UNSIGNED_FLAG);
  auto data = (uint32)sint4korr(*row);
  longstore(pointer_cast<uchar *>(param->buffer), data);
  *param->error = param->is_unsigned != field_is_unsigned && data > INT_MAX32;
  *row += 4;
}

static void fetch_result_int64(MYSQL_BIND *param,
                               MYSQL_FIELD *field [[maybe_unused]],
                               uchar **row) {
  const bool field_is_unsigned = (field->flags & UNSIGNED_FLAG);
  auto data = (ulonglong)sint8korr(*row);
  *param->error = param->is_unsigned != field_is_unsigned && data > LLONG_MAX;
  longlongstore(pointer_cast<uchar *>(param->buffer), data);
  *row += 8;
}

static void fetch_result_float(MYSQL_BIND *param,
                               MYSQL_FIELD *field [[maybe_unused]],
                               uchar **row) {
  float const value = float4get(*row);
  floatstore(pointer_cast<uchar *>(param->buffer), value);
  *row += 4;
}

static void fetch_result_double(MYSQL_BIND *param,
                                MYSQL_FIELD *field [[maybe_unused]],
                                uchar **row) {
  double const value = float8get(*row);
  doublestore(pointer_cast<uchar *>(param->buffer), value);
  *row += 8;
}

static void fetch_result_time(MYSQL_BIND *param,
                              MYSQL_FIELD *field [[maybe_unused]],
                              uchar **row) {
  auto *tm = (MYSQL_TIME *)param->buffer;
  read_binary_time(tm, row);
}

static void fetch_result_date(MYSQL_BIND *param,
                              MYSQL_FIELD *field [[maybe_unused]],
                              uchar **row) {
  auto *tm = (MYSQL_TIME *)param->buffer;
  read_binary_date(tm, row);
}

static void fetch_result_datetime(MYSQL_BIND *param,
                                  MYSQL_FIELD *field [[maybe_unused]],
                                  uchar **row) {
  auto *tm = (MYSQL_TIME *)param->buffer;
  read_binary_datetime(tm, row);
}

static void fetch_result_bin(MYSQL_BIND *param,
                             MYSQL_FIELD *field [[maybe_unused]], uchar **row) {
  const ulong length = net_field_length(row);
  ulong const copy_length = std::min(length, param->buffer_length);
  memcpy(param->buffer, (char *)*row, copy_length);
  *param->length = length;
  *param->error = copy_length < length;
  *row += length;
}

static void fetch_result_str(MYSQL_BIND *param,
                             MYSQL_FIELD *field [[maybe_unused]], uchar **row) {
  const ulong length = net_field_length(row);
  ulong const copy_length = std::min(length, param->buffer_length);
  memcpy(param->buffer, (char *)*row, copy_length);
  /* Add an end null if there is room in the buffer */
  if (copy_length != param->buffer_length)
    ((uchar *)param->buffer)[copy_length] = '\0';
  *param->length = length; /* return total length */
  *param->error = copy_length < length;
  *row += length;
}

/*
  functions to calculate max lengths for strings during
  mysql_stmt_store_result()
*/

static void skip_result_fixed(MYSQL_BIND *param,
                              MYSQL_FIELD *field [[maybe_unused]], uchar **row)

{
  (*row) += param->pack_length;
}

static void skip_result_with_length(MYSQL_BIND *param [[maybe_unused]],
                                    MYSQL_FIELD *field [[maybe_unused]],
                                    uchar **row)

{
  const ulong length = net_field_length(row);
  (*row) += length;
}

static void skip_result_string(MYSQL_BIND *param [[maybe_unused]],
                               MYSQL_FIELD *field, uchar **row)

{
  const ulong length = net_field_length(row);
  (*row) += length;
  if (field->max_length < length) field->max_length = length;
}

/*
  Check that two field types are binary compatible i. e.
  have equal representation in the binary protocol and
  require client-side buffers of the same type.

  SYNOPSIS
    is_binary_compatible()
    type1   parameter type supplied by user
    type2   field type, obtained from result set metadata

  RETURN
    true or false
*/

static bool is_binary_compatible(enum enum_field_types type1,
                                 enum enum_field_types type2) {
  static const enum enum_field_types
      range1[] = {MYSQL_TYPE_SHORT, MYSQL_TYPE_YEAR, MYSQL_TYPE_NULL},
      range2[] = {MYSQL_TYPE_INT24, MYSQL_TYPE_LONG, MYSQL_TYPE_NULL},
      range3[] = {MYSQL_TYPE_DATETIME, MYSQL_TYPE_TIMESTAMP, MYSQL_TYPE_NULL},
      range4[] = {
          MYSQL_TYPE_ENUM,        MYSQL_TYPE_SET,        MYSQL_TYPE_TINY_BLOB,
          MYSQL_TYPE_MEDIUM_BLOB, MYSQL_TYPE_LONG_BLOB,  MYSQL_TYPE_BLOB,
          MYSQL_TYPE_VECTOR,      MYSQL_TYPE_VAR_STRING, MYSQL_TYPE_STRING,
          MYSQL_TYPE_GEOMETRY,    MYSQL_TYPE_DECIMAL,    MYSQL_TYPE_NULL};
  static const enum enum_field_types *range_list[] = {range1, range2, range3,
                                                      range4},
                                     **range_list_end =
                                         range_list + sizeof(range_list) /
                                                          sizeof(*range_list);
  const enum enum_field_types **range, *type;

  if (type1 == type2) return true;
  for (range = range_list; range != range_list_end; ++range) {
    /* check that both type1 and type2 are in the same range */
    bool type1_found = false, type2_found = false;
    for (type = *range; *type != MYSQL_TYPE_NULL; type++) {
      type1_found |= type1 == *type;
      type2_found |= type2 == *type;
    }
    if (type1_found || type2_found) return type1_found && type2_found;
  }
  return false;
}

/*
  Setup a fetch function for one column of a result set.

  SYNOPSIS
    setup_one_fetch_function()
    param    output buffer descriptor
    field    column descriptor

  DESCRIPTION
    When user binds result set buffers or when result set
    metadata is changed, we need to setup fetch (and possibly
    conversion) functions for all columns of the result set.
    In addition to that here we set up skip_result function, used
    to update result set metadata in case when
    STMT_ATTR_UPDATE_MAX_LENGTH attribute is set.
    Notice that while fetch_result is chosen depending on both
    field->type and param->type, skip_result depends on field->type
    only.

  RETURN
    true   fetch function for this typecode was not found (typecode
          is not supported by the client library)
    false  success
*/

static bool setup_one_fetch_function(MYSQL_BIND *param, MYSQL_FIELD *field) {
  DBUG_TRACE;

  /* Setup data copy functions for the different supported types */
  switch (param->buffer_type) {
    case MYSQL_TYPE_NULL: /* for dummy binds */
      /*
        It's not binary compatible with anything the server can return:
        no need to setup fetch_result, as it'll be reset anyway
      */
      *param->length = 0;
      break;
    case MYSQL_TYPE_TINY:
      param->fetch_result = fetch_result_tinyint;
      *param->length = 1;
      break;
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_YEAR:
      param->fetch_result = fetch_result_short;
      *param->length = 2;
      break;
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      param->fetch_result = fetch_result_int32;
      *param->length = 4;
      break;
    case MYSQL_TYPE_LONGLONG:
      param->fetch_result = fetch_result_int64;
      *param->length = 8;
      break;
    case MYSQL_TYPE_FLOAT:
      param->fetch_result = fetch_result_float;
      *param->length = 4;
      break;
    case MYSQL_TYPE_DOUBLE:
      param->fetch_result = fetch_result_double;
      *param->length = 8;
      break;
    case MYSQL_TYPE_TIME:
      param->fetch_result = fetch_result_time;
      *param->length = sizeof(MYSQL_TIME);
      break;
    case MYSQL_TYPE_DATE:
      param->fetch_result = fetch_result_date;
      *param->length = sizeof(MYSQL_TIME);
      break;
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      param->fetch_result = fetch_result_datetime;
      *param->length = sizeof(MYSQL_TIME);
      break;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VECTOR:
    case MYSQL_TYPE_BIT:
      assert(param->buffer_length != 0);
      param->fetch_result = fetch_result_bin;
      break;
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_JSON:
      assert(param->buffer_length != 0);
      param->fetch_result = fetch_result_str;
      break;
    default:
      DBUG_PRINT("error",
                 ("Unknown param->buffer_type: %u", (uint)param->buffer_type));
      return true;
  }
  if (!is_binary_compatible(param->buffer_type, field->type))
    param->fetch_result = fetch_result_with_conversion;

  /* Setup skip_result functions (to calculate max_length) */
  param->skip_result = skip_result_fixed;
  switch (field->type) {
    case MYSQL_TYPE_NULL: /* for dummy binds */
      param->pack_length = 0;
      field->max_length = 0;
      break;
    case MYSQL_TYPE_TINY:
      param->pack_length = 1;
      field->max_length = 4; /* as in '-127' */
      break;
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_SHORT:
      param->pack_length = 2;
      field->max_length = 6; /* as in '-32767' */
      break;
    case MYSQL_TYPE_INT24:
      field->max_length = 9; /* as in '16777216' or in '-8388607' */
      param->pack_length = 4;
      break;
    case MYSQL_TYPE_LONG:
      field->max_length = 11; /* '-2147483647' */
      param->pack_length = 4;
      break;
    case MYSQL_TYPE_LONGLONG:
      field->max_length = 21; /* '18446744073709551616' */
      param->pack_length = 8;
      break;
    case MYSQL_TYPE_FLOAT:
      param->pack_length = 4;
      field->max_length = MAX_DOUBLE_STRING_REP_LENGTH;
      break;
    case MYSQL_TYPE_DOUBLE:
      param->pack_length = 8;
      field->max_length = MAX_DOUBLE_STRING_REP_LENGTH;
      break;
    case MYSQL_TYPE_TIME:
      field->max_length = 17; /* -819:23:48.123456 */
      param->skip_result = skip_result_with_length;
      break;
    case MYSQL_TYPE_DATE:
      field->max_length = 10; /* 2003-11-11 */
      param->skip_result = skip_result_with_length;
      break;
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
      param->skip_result = skip_result_with_length;
      field->max_length = MAX_DATE_STRING_REP_LENGTH;
      break;
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VECTOR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_JSON:
      param->skip_result = skip_result_string;
      break;
    default:
      DBUG_PRINT("error", ("Unknown field->type: %u", (uint)field->type));
      return true;
  }
  return false;
}

/*
  Setup the bind buffers for resultset processing
*/

bool STDCALL mysql_stmt_bind_result(MYSQL_STMT *stmt, MYSQL_BIND *my_bind) {
  MYSQL_BIND *param, *end;
  MYSQL_FIELD *field;
  const ulong bind_count = stmt->field_count;
  uint param_count = 0;
  DBUG_TRACE;
  DBUG_PRINT("enter", ("field_count: %lu", bind_count));

  if (!bind_count) {
    const int errorcode = (int)stmt->state < (int)MYSQL_STMT_PREPARE_DONE
                              ? CR_NO_PREPARE_STMT
                              : CR_NO_STMT_METADATA;
    set_stmt_error(stmt, errorcode, unknown_sqlstate);
    return true;
  }

  /*
    We only need to check that stmt->field_count - if it is not null
    stmt->bind was initialized in mysql_stmt_bind_named_param()
    stmt->bind overlaps with bind if mysql_stmt_bind_named_param
    is called from mysql_stmt_store_result.
    BEWARE of buffer overwrite ...
  */

  if (stmt->bind == nullptr) {
    /*
      alloc_root will return valid address even in case when param_count
      and field_count are zero. Thus we should never rely on stmt->bind
      or stmt->params when checking for existence of placeholders or
      result set.
     */
    MEM_ROOT *fields_mem_root = &stmt->extension->fields_mem_root;
    if (!(stmt->bind = (MYSQL_BIND *)fields_mem_root->Alloc(
              sizeof(MYSQL_BIND) * stmt->field_count))) {
      set_stmt_error(stmt, CR_OUT_OF_MEMORY, unknown_sqlstate);
      return true;
    }
  }

  if (stmt->bind != my_bind)
    memcpy((char *)stmt->bind, (char *)my_bind,
           sizeof(MYSQL_BIND) * bind_count);

  for (param = stmt->bind, end = param + bind_count, field = stmt->fields;
       param < end; param++, field++) {
    DBUG_PRINT("info", ("buffer_type: %u  field_type: %u",
                        (uint)param->buffer_type, (uint)field->type));
    /*
      Set param->is_null to point to a dummy variable if it's not set.
      This is to make the execute code easier
    */
    if (!param->is_null) param->is_null = &param->is_null_value;

    if (!param->length) param->length = &param->length_value;

    if (!param->error) param->error = &param->error_value;

    param->param_number = param_count++;
    param->offset = 0;

    if (setup_one_fetch_function(param, field)) {
      my_stpcpy(stmt->sqlstate, unknown_sqlstate);
      sprintf(stmt->last_error,
              ER_CLIENT(stmt->last_errno = CR_UNSUPPORTED_PARAM_TYPE),
              field->type, param_count);
      return true;
    }
  }
  stmt->bind_result_done = BIND_RESULT_DONE;
  if (stmt->mysql->options.report_data_truncation)
    stmt->bind_result_done |= REPORT_DATA_TRUNCATION;

  return false;
}

/*
  Fetch row data to bind buffers
*/

static int stmt_fetch_row(MYSQL_STMT *stmt, uchar *row) {
  MYSQL_BIND *my_bind, *end;
  MYSQL_FIELD *field;
  uchar *null_ptr, bit;
  int truncation_count = 0;
  /*
    Precondition: if stmt->field_count is zero or row is NULL, read_row_*
    function must return no data.
  */
  assert(stmt->field_count);
  assert(row);

  if (!stmt->bind_result_done) {
    /* If output parameters were not bound we should just return success */
    return 0;
  }
  assert(stmt->bind != nullptr);

  null_ptr = row;
  row += (stmt->field_count + 9) / 8; /* skip null bits */
  bit = 4;                            /* first 2 bits are reserved */

  /* Copy complete row to application buffers */
  for (my_bind = stmt->bind, end = my_bind + stmt->field_count,
      field = stmt->fields;
       my_bind < end; my_bind++, field++) {
    *my_bind->error = false;
    if (*null_ptr & bit) {
      /*
        We should set both row_ptr and is_null to be able to see
        nulls in mysql_stmt_fetch_column. This is because is_null may point
        to user data which can be overwritten between mysql_stmt_fetch and
        mysql_stmt_fetch_column, and in this case nullness of column will be
        lost. See mysql_stmt_fetch_column for details.
      */
      my_bind->row_ptr = nullptr;
      *my_bind->is_null = true;
    } else {
      *my_bind->is_null = false;
      my_bind->row_ptr = row;
      (*my_bind->fetch_result)(my_bind, field, &row);
      truncation_count += *my_bind->error;
    }
    if (!((bit <<= 1) & 255)) {
      bit = 1; /* To next uchar */
      null_ptr++;
    }
  }
  if (truncation_count && (stmt->bind_result_done & REPORT_DATA_TRUNCATION))
    return MYSQL_DATA_TRUNCATED;
  return 0;
}

int cli_unbuffered_fetch(MYSQL *mysql, char **row) {
  ulong len = 0;
  bool is_data_packet;
  if (packet_error == (len = cli_safe_read(mysql, &is_data_packet))) {
    MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
    return 1;
  }

  if (mysql->net.read_pos[0] != 0 && !is_data_packet) {
    /* in case of new client read the OK packet */
    if (mysql->server_capabilities & CLIENT_DEPRECATE_EOF)
      read_ok_ex(mysql, len);
    *row = nullptr;
    MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
  } else {
    *row = (char *)(mysql->net.read_pos + 1);
  }

  return 0;
}

/*
  Fetch and return row data to bound buffers, if any
*/

int STDCALL mysql_stmt_fetch(MYSQL_STMT *stmt) {
  int rc;
  uchar *row;
  DBUG_TRACE;

  if ((rc = (*stmt->read_row_func)(stmt, &row)) ||
      ((rc = stmt_fetch_row(stmt, row)) && rc != MYSQL_DATA_TRUNCATED)) {
    stmt->state = MYSQL_STMT_PREPARE_DONE; /* XXX: this is buggy */
    stmt->read_row_func = (rc == MYSQL_NO_DATA) ? stmt_read_row_no_data
                                                : stmt_read_row_no_result_set;
  } else {
    /* This is to know in mysql_stmt_fetch_column that data was fetched */
    stmt->state = MYSQL_STMT_FETCH_DONE;
  }
  return rc;
}

/*
  Fetch data for one specified column data

  SYNOPSIS
    mysql_stmt_fetch_column()
    stmt		Prepared statement handler
    my_bind		Where data should be placed. Should be filled in as
                        when calling mysql_stmt_bind_result()
    column		Column to fetch (first column is 0)
    ulong offset	Offset in result data (to fetch blob in pieces)
                        This is normally 0
  RETURN
    0	ok
    1	error
*/

int STDCALL mysql_stmt_fetch_column(MYSQL_STMT *stmt, MYSQL_BIND *my_bind,
                                    uint column, ulong offset) {
  MYSQL_BIND *param = stmt->bind + column;
  DBUG_TRACE;

  if ((int)stmt->state < (int)MYSQL_STMT_FETCH_DONE) {
    set_stmt_error(stmt, CR_NO_DATA, unknown_sqlstate);
    return 1;
  }
  if (column >= stmt->field_count) {
    set_stmt_error(stmt, CR_INVALID_PARAMETER_NO, unknown_sqlstate);
    return 1;
  }

  if (!my_bind->error) my_bind->error = &my_bind->error_value;
  *my_bind->error = false;
  if (param->row_ptr) {
    MYSQL_FIELD *field = stmt->fields + column;
    uchar *row = param->row_ptr;
    my_bind->offset = offset;
    if (my_bind->is_null) *my_bind->is_null = false;
    if (my_bind->length) /* Set the length if non char/binary types */
      *my_bind->length = *param->length;
    else
      my_bind->length = &param->length_value; /* Needed for fetch_result() */
    fetch_result_with_conversion(my_bind, field, &row);
  } else {
    if (my_bind->is_null) *my_bind->is_null = true;
  }
  return 0;
}

/*
  Read all rows of data from server  (binary format)
*/

int cli_read_binary_rows(MYSQL_STMT *stmt) {
  ulong pkt_len;
  uchar *cp;
  MYSQL *mysql = stmt->mysql;
  MYSQL_DATA *result = &stmt->result;
  MYSQL_ROWS **prev_ptr = &result->data;
  NET *net;
  bool is_data_packet;

  DBUG_TRACE;

  if (!mysql) {
    set_stmt_error(stmt, CR_SERVER_LOST, unknown_sqlstate);
    return 1;
  }

  net = &mysql->net;
  /*
   We could have read one row in execute() due to the lack of a cursor,
   but one at most.
  */
  assert(result->rows <= 1);
  if (result->rows == 1) prev_ptr = &result->data->next;

  while ((pkt_len = cli_safe_read(mysql, &is_data_packet)) != packet_error) {
    cp = net->read_pos;
    if (pkt_len < 1) {
      set_stmt_error(stmt, CR_MALFORMED_PACKET, unknown_sqlstate);
      return 1;
    }
    if (*cp == 0 || is_data_packet) {
      if (add_binary_row(net, stmt, pkt_len, &prev_ptr)) goto err;
    } else {
      /* end of data */
      *prev_ptr = nullptr;
      /* read warning count from OK packet or EOF packet if it is old client */
      if (mysql->server_capabilities & CLIENT_DEPRECATE_EOF && !is_data_packet)
        read_ok_ex(mysql, pkt_len);
      else {
        if (pkt_len < 3) {
          set_stmt_error(stmt, CR_MALFORMED_PACKET, unknown_sqlstate);
          return 1;
        }
        mysql->warning_count = uint2korr(cp + 1);
      }
      /*
        OUT parameters result sets has SERVER_PS_OUT_PARAMS and
        SERVER_MORE_RESULTS_EXISTS flags in first EOF_Packet only.
        Last EOF_Packet of OUT parameters result sets have no
        SERVER_MORE_RESULTS_EXISTS flag as described here:
        http://dev.mysql.com/doc/internals/en/stored-procedures.html#out-parameter-set
        Following code reads last EOF_Packet of result set and can clear
        those flags in server_status if we don't preserve them.
        Without SERVER_MORE_RESULTS_EXISTS flag mysql_stmt_next_result fails
        to read OK_Packet after OUT parameters result set.
        So we need to preserve SERVER_MORE_RESULTS_EXISTS flag for OUT
        parameters result set.
      */
      if (pkt_len < 5) {
        set_stmt_error(stmt, CR_MALFORMED_PACKET, unknown_sqlstate);
        return 1;
      }
      if (mysql->server_status & SERVER_PS_OUT_PARAMS) {
        mysql->server_status =
            uint2korr(cp + 3) | SERVER_PS_OUT_PARAMS |
            (mysql->server_status & SERVER_MORE_RESULTS_EXISTS);
      } else
        mysql->server_status = uint2korr(cp + 3);
      DBUG_PRINT("info", ("status: %u  warning_count: %u", mysql->server_status,
                          mysql->warning_count));
#if defined(CLIENT_PROTOCOL_TRACING)
      if (mysql->server_status & SERVER_MORE_RESULTS_EXISTS)
        MYSQL_TRACE_STAGE(mysql, WAIT_FOR_RESULT);
      else
        MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
#endif
      return 0;
    }
  }
  set_stmt_errmsg(stmt, net);

err:
  return 1;
}

/*
  Update meta data for statement

  SYNOPSIS
    stmt_update_metadata()
    stmt			Statement handler
    row				Binary data

  NOTES
    Only updates MYSQL_FIELD->max_length for strings
*/

static void stmt_update_metadata(MYSQL_STMT *stmt, MYSQL_ROWS *data) {
  MYSQL_BIND *my_bind, *end;
  MYSQL_FIELD *field;
  uchar *null_ptr, bit;
  auto *row = (uchar *)data->data;
#ifndef NDEBUG
  uchar *row_end = row + data->length;
#endif

  null_ptr = row;
  row += (stmt->field_count + 9) / 8; /* skip null bits */
  bit = 4;                            /* first 2 bits are reserved */

  /* Go through all fields and calculate metadata */
  for (my_bind = stmt->bind, end = my_bind + stmt->field_count,
      field = stmt->fields;
       my_bind < end; my_bind++, field++) {
    if (!(*null_ptr & bit)) (*my_bind->skip_result)(my_bind, field, &row);
    assert(row <= row_end);
    if (!((bit <<= 1) & 255)) {
      bit = 1; /* To next uchar */
      null_ptr++;
    }
  }
}

/*
  Store or buffer the binary results to stmt
*/

int STDCALL mysql_stmt_store_result(MYSQL_STMT *stmt) {
  MYSQL *mysql = stmt->mysql;
  MYSQL_DATA *result = &stmt->result;
  DBUG_TRACE;

  if (!mysql) {
    /* mysql can be reset in mysql_close called from mysql_reconnect */
    set_stmt_error(stmt, CR_SERVER_LOST, unknown_sqlstate);
    return 1;
  }

  if (!stmt->field_count) return 0;

  if ((int)stmt->state < (int)MYSQL_STMT_EXECUTE_DONE) {
    set_stmt_error(stmt, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    return 1;
  }

  if (stmt->last_errno) {
    /* An attempt to use an invalid statement handle. */
    return 1;
  }

  if (mysql->status == MYSQL_STATUS_READY &&
      stmt->server_status & SERVER_STATUS_CURSOR_EXISTS) {
    /*
      Server side cursor exist, tell server to start sending the rows
    */
    NET *net = &mysql->net;
    uchar buff[4 /* statement id */ + 4 /* number of rows to fetch */];

    /* Send row request to the server */
    int4store(buff, stmt->stmt_id);
    int4store(buff + 4, (int)~0); /* number of rows to fetch */
    if (cli_advanced_command(mysql, COM_STMT_FETCH, buff, sizeof(buff),
                             (uchar *)nullptr, 0, true, stmt)) {
      /*
        Don't set stmt error if stmt->mysql is NULL, as the error in this case
        has already been set by mysql_prune_stmt_list().
      */
      if (stmt->mysql) set_stmt_errmsg(stmt, net);
      return 1;
    }
  } else if (mysql->status != MYSQL_STATUS_STATEMENT_GET_RESULT) {
    set_stmt_error(stmt, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    return 1;
  }

  if (stmt->update_max_length && !stmt->bind_result_done) {
    /*
      We must initialize the bind structure to be able to calculate
      max_length
    */
    MYSQL_BIND *my_bind, *end;
    if (stmt->bind == nullptr) {
      /*
        alloc_root will return valid address even in case when param_count
        and field_count are zero. Thus we should never rely on stmt->bind
        or stmt->params when checking for existence of placeholders or
        result set.
       */
      MEM_ROOT *fields_mem_root = &stmt->extension->fields_mem_root;
      if (!(stmt->bind = (MYSQL_BIND *)fields_mem_root->Alloc(
                sizeof(MYSQL_BIND) * stmt->field_count))) {
        set_stmt_error(stmt, CR_OUT_OF_MEMORY, unknown_sqlstate);
        return 1;
      }
    }
    memset(stmt->bind, 0, sizeof(*stmt->bind) * stmt->field_count);

    for (my_bind = stmt->bind, end = my_bind + stmt->field_count; my_bind < end;
         my_bind++) {
      my_bind->buffer_type = MYSQL_TYPE_NULL;
      my_bind->buffer_length = 1;
    }

    if (mysql_stmt_bind_result(stmt, stmt->bind)) return 1;
    stmt->bind_result_done = 0; /* No normal bind done */
  }

  if ((*mysql->methods->read_binary_rows)(stmt)) {
    result->alloc->ClearForReuse();
    result->data = nullptr;
    result->rows = 0;
    mysql->status = MYSQL_STATUS_READY;
    return 1;
  }

  /* Assert that if there was a cursor, all rows have been fetched */
  assert(mysql->status != MYSQL_STATUS_READY ||
         (mysql->server_status & SERVER_STATUS_LAST_ROW_SENT));

  if (stmt->update_max_length) {
    MYSQL_ROWS *cur = result->data;
    for (; cur; cur = cur->next) stmt_update_metadata(stmt, cur);
  }

  stmt->data_cursor = result->data;
  mysql->affected_rows = stmt->affected_rows = result->rows;
  stmt->read_row_func = stmt_read_row_buffered;
  mysql->unbuffered_fetch_owner = nullptr; /* set in stmt_execute */
  mysql->status = MYSQL_STATUS_READY;      /* server is ready */
  return 0; /* Data buffered, must be fetched with mysql_stmt_fetch() */
}

/*
  Seek to desired row in the statement result set
*/

MYSQL_ROW_OFFSET STDCALL mysql_stmt_row_seek(MYSQL_STMT *stmt,
                                             MYSQL_ROW_OFFSET row) {
  MYSQL_ROW_OFFSET offset = stmt->data_cursor;
  DBUG_TRACE;

  stmt->data_cursor = row;
  return offset;
}

/*
  Return the current statement row cursor position
*/

MYSQL_ROW_OFFSET STDCALL mysql_stmt_row_tell(MYSQL_STMT *stmt) {
  DBUG_TRACE;

  return stmt->data_cursor;
}

/*
  Move the stmt result set data cursor to specified row
*/

void STDCALL mysql_stmt_data_seek(MYSQL_STMT *stmt, uint64_t row) {
  MYSQL_ROWS *tmp = stmt->result.data;
  DBUG_TRACE;
  DBUG_PRINT("enter", ("row id to seek: %ld", (long)row));

  for (; tmp && row; --row, tmp = tmp->next)
    ;
  stmt->data_cursor = tmp;
  if (!row && tmp) {
    /*  Rewind the counter */
    stmt->read_row_func = stmt_read_row_buffered;
    stmt->state = MYSQL_STMT_EXECUTE_DONE;
  }
}

/*
  Return total rows the current statement result set
*/

uint64_t STDCALL mysql_stmt_num_rows(MYSQL_STMT *stmt) {
  DBUG_TRACE;

  return stmt->result.rows;
}

/*
  Free the client side memory buffers, reset long data state
  on client if necessary, and reset the server side statement if
  this has been requested.
*/

static bool reset_stmt_handle(MYSQL_STMT *stmt, uint flags) {
  /* If statement hasn't been prepared there is nothing to reset */
  if ((int)stmt->state > (int)MYSQL_STMT_INIT_DONE) {
    MYSQL *mysql = stmt->mysql;
    MYSQL_DATA *result = &stmt->result;

    /*
      Reset stored result set if so was requested or it's a part
      of cursor fetch.
    */
    if (flags & RESET_STORE_RESULT) {
      /* Result buffered */
      result->alloc->ClearForReuse();
      result->data = nullptr;
      result->rows = 0;
      stmt->data_cursor = nullptr;
    }
    if (flags & RESET_LONG_DATA) {
      if (stmt->params) {
        MYSQL_BIND *param = stmt->params,
                   *param_end = param + stmt->param_count;
        /* Clear long_data_used flags */
        for (; param < param_end; param++) param->long_data_used = false;
      }
    }
    stmt->read_row_func = stmt_read_row_no_result_set;
    if (mysql) {
      if ((int)stmt->state > (int)MYSQL_STMT_PREPARE_DONE) {
        if (mysql->unbuffered_fetch_owner == &stmt->unbuffered_fetch_cancelled)
          mysql->unbuffered_fetch_owner = nullptr;
        if (stmt->field_count && mysql->status != MYSQL_STATUS_READY) {
          /* There is a result set and it belongs to this statement */
          (*mysql->methods->flush_use_result)(mysql, false);
          if (mysql->unbuffered_fetch_owner)
            *mysql->unbuffered_fetch_owner = true;
          mysql->status = MYSQL_STATUS_READY;
        }
      }
      if (flags & RESET_SERVER_SIDE) {
        /*
          Reset the server side statement and close the server side
          cursor if it exists.
        */
        uchar buff[MYSQL_STMT_HEADER]; /* packet header: 4 bytes for stmt id */
        int4store(buff, stmt->stmt_id);
        if ((*mysql->methods->advanced_command)(mysql, COM_STMT_RESET, buff,
                                                sizeof(buff), nullptr, 0, false,
                                                stmt)) {
          set_stmt_errmsg(stmt, &mysql->net);
          stmt->state = MYSQL_STMT_INIT_DONE;
          return true;
        }
      }
    }
    if (flags & RESET_CLEAR_ERROR) stmt_clear_error(stmt);
    stmt->state = MYSQL_STMT_PREPARE_DONE;
  }
  return false;
}

bool STDCALL mysql_stmt_free_result(MYSQL_STMT *stmt) {
  DBUG_TRACE;

  /* Free the client side and close the server side cursor if there is one */
  return reset_stmt_handle(
      stmt, RESET_LONG_DATA | RESET_STORE_RESULT | RESET_CLEAR_ERROR);
}

/********************************************************************
 statement error handling and close
*********************************************************************/

/*
  Close the statement handle by freeing all allocated resources

  SYNOPSIS
    mysql_stmt_close()
    stmt	       Statement handle

  RETURN VALUES
    0	ok
    1	error
*/

bool STDCALL mysql_stmt_close(MYSQL_STMT *stmt) {
  MYSQL *mysql = stmt->mysql;
  int rc = 0;
  DBUG_TRACE;

  mysql_stmt_extension_bind_free(stmt->extension);
  stmt->result.alloc->Clear();
  stmt->mem_root->Clear();
  stmt->extension->fields_mem_root.Clear();

  if (mysql) {
    mysql->stmts = list_delete(mysql->stmts, &stmt->list);
    /*
      Clear NET error state: if the following commands come through
      successfully, connection will still be usable for other commands.
    */
    net_clear_error(&mysql->net);
    if ((int)stmt->state > (int)MYSQL_STMT_INIT_DONE) {
      uchar buff[MYSQL_STMT_HEADER]; /* 4 bytes - stmt id */

      if (mysql->unbuffered_fetch_owner == &stmt->unbuffered_fetch_cancelled)
        mysql->unbuffered_fetch_owner = nullptr;
      if (mysql->status != MYSQL_STATUS_READY) {
        /*
          Flush result set of the connection. If it does not belong
          to this statement, set a warning.
        */
        (*mysql->methods->flush_use_result)(mysql, true);
        if (mysql->unbuffered_fetch_owner)
          *mysql->unbuffered_fetch_owner = true;
        mysql->status = MYSQL_STATUS_READY;
      }
      int4store(buff, stmt->stmt_id);
      /*
        If stmt_command failed, it would have already raised
        error using set_mysql_error. Caller should use
        mysql_error() or mysql_errno() to find out details.
        Memory allocated for stmt will be released regardless
        of the error.
      */
      rc = stmt_command(mysql, COM_STMT_CLOSE, buff, 4, stmt);
    }
  }

#ifndef NDEBUG
  delete stmt->extension->bind_data.allocations;
#endif

  my_free(stmt->result.alloc);
  my_free(stmt->mem_root);
  my_free(stmt->extension);
  my_free(stmt);

  return rc != 0;
}

/*
  Reset the statement buffers in server
*/

bool STDCALL mysql_stmt_reset(MYSQL_STMT *stmt) {
  DBUG_TRACE;
  assert(stmt != nullptr);
  if (!stmt->mysql) {
    /* mysql can be reset in mysql_close called from mysql_reconnect */
    set_stmt_error(stmt, CR_SERVER_LOST, unknown_sqlstate);
    return true;
  }
  /* Reset the client and server sides of the prepared statement */
  return reset_stmt_handle(
      stmt, RESET_SERVER_SIDE | RESET_LONG_DATA | RESET_CLEAR_ERROR);
}

/*
  Return statement error code
*/

uint STDCALL mysql_stmt_errno(MYSQL_STMT *stmt) {
  DBUG_TRACE;
  return stmt->last_errno;
}

const char *STDCALL mysql_stmt_sqlstate(MYSQL_STMT *stmt) {
  DBUG_TRACE;
  return stmt->sqlstate;
}

/*
  Return statement error message
*/

const char *STDCALL mysql_stmt_error(MYSQL_STMT *stmt) {
  DBUG_TRACE;
  return stmt->last_error;
}

/********************************************************************
 Multi query execution + SPs APIs
*********************************************************************/

/*
  This API reads the next statement result and returns a status to indicate
  whether more results exist

  @param[in]    mysql                                    connection handle

  @retval       NET_ASYNC_ERROR                          Error
  @retval       NET_ASYNC_NOT_READY                      reading next result not
                                                         yet completed, call
                                                         this API again
  @retval       NET_ASYNC_COMPLETE                       finished reading result
  @retval       NET_ASYNC_COMPLETE_NO_MORE_RESULTS       status to indicate if
                                                         more results exist
*/
net_async_status STDCALL mysql_next_result_nonblocking(MYSQL *mysql) {
  DBUG_TRACE;
  net_async_status status;
  if (mysql->status != MYSQL_STATUS_READY) {
    set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    return NET_ASYNC_ERROR;
  }
  net_clear_error(&mysql->net);
  mysql->affected_rows = ~(uint64_t)0;

  if (mysql->server_status & SERVER_MORE_RESULTS_EXISTS) {
    status = (*mysql->methods->next_result_nonblocking)(mysql);
    return status;
  }
  MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);

  return NET_ASYNC_COMPLETE_NO_MORE_RESULTS; /* No more results */
}

int STDCALL mysql_stmt_next_result(MYSQL_STMT *stmt) {
  MYSQL *mysql = stmt->mysql;
  int rc;
  DBUG_TRACE;

  if (!mysql) return 1;

  if (stmt->last_errno) return stmt->last_errno;

  if (mysql->server_status & SERVER_MORE_RESULTS_EXISTS) {
    if (reset_stmt_handle(stmt, RESET_STORE_RESULT)) return 1;
  }

  rc = mysql_next_result(mysql);

  if (rc) {
    set_stmt_errmsg(stmt, &mysql->net);
    return rc;
  }

  if (mysql->status == MYSQL_STATUS_GET_RESULT)
    mysql->status = MYSQL_STATUS_STATEMENT_GET_RESULT;

  stmt->state = MYSQL_STMT_EXECUTE_DONE;
  stmt->bind_result_done = false;
  stmt->field_count = mysql->field_count;

  if (mysql->field_count) {
    alloc_stmt_fields(stmt);
    prepare_to_fetch_result(stmt);
  }

  return 0;
}

bool STDCALL mysql_read_query_result(MYSQL *mysql) {
  return (*mysql->methods->read_query_result)(mysql);
}
