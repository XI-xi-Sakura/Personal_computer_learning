/* Copyright (c) 2019, 2025, Oracle and/or its affiliates.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql/dd/impl/upgrade/server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/types.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>

#include "server.h"
#include "sql/dd/upgrade/server.h"
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <vector>

#include "my_dbug.h"
#include "my_rapidjson_size_t.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/psi/mysql_file.h"
#include "mysql/strings/m_ctype.h"
#include "nulls.h"
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include "scripts/mysql_fix_privilege_tables_sql.h"
#include "scripts/sql_commands_system_tables_data_fix.h"
#include "scripts/sql_firewall_sp_firewall_group_delist.h"
#include "scripts/sql_firewall_sp_firewall_group_enlist.h"
#include "scripts/sql_firewall_sp_reload_firewall_group_rules.h"
#include "scripts/sql_firewall_sp_reload_firewall_rules.h"
#include "scripts/sql_firewall_sp_set_firewall_group_mode.h"
#include "scripts/sql_firewall_sp_set_firewall_group_mode_and_user.h"
#include "scripts/sql_firewall_sp_set_firewall_mode.h"
#include "sql/dd/cache/dictionary_client.h"  // dd::cache::Dictionary_client
#include "sql/dd/dd_schema.h"                // dd::Schema_MDL_locker
#include "sql/dd/dd_table.h"                 // dd::prefix_key_partition_exists
#include "sql/dd/dd_tablespace.h"            // dd::fill_table_and_parts...
#include "sql/dd/dd_trigger.h"               // dd::create_trigger
#include "sql/dd/impl/bootstrap/bootstrap_ctx.h"  // dd::DD_bootstrap_ctx
#include "sql/dd/impl/bootstrap/bootstrapper.h"
#include "sql/dd/impl/tables/dd_properties.h"  // dd::tables::DD_properties
#include "sql/dd/impl/tables/events.h"         // create_key_by_schema_id
#include "sql/dd/impl/tables/routines.h"       // create_key_by_schema_id
#include "sql/dd/impl/tables/tables.h"         // create_key_by_schema_id
#include "sql/dd/impl/utils.h"                 // dd::end_transaction
#include "sql/dd/types/routine.h"              // dd::Table
#include "sql/dd/types/table.h"                // dd::Table
#include "sql/dd/types/tablespace.h"
#include "sql/dd_sp.h"      // prepare_sp_chistics_from_dd_routine
#include "sql/sd_notify.h"  // sysd::notify
#include "sql/sp.h"         // Stored_routine_creation_ctx
#include "sql/sp_head.h"    // sp_head
#include "sql/sql_base.h"
#include "sql/sql_table.h"
#include "sql/statement/ed_connection.h"
#include "sql/strfunc.h"
#include "sql/table_trigger_dispatcher.h"  // Table_trigger_dispatcher
#include "sql/thd_raii.h"
#include "sql/trigger.h"  // Trigger
#include "sql/trigger_def.h"
#include "string_with_len.h"

using sql_mode_t = uint64_t;
extern const char *mysql_sys_schema[];
extern const char *fill_help_tables[];

const char *upgrade_modes[] = {"NONE", "MINIMAL", "AUTO", "FORCE", NullS};
TYPELIB upgrade_mode_typelib = {array_elements(upgrade_modes) - 1, "",
                                upgrade_modes, nullptr};

const char *check_table_fun_modes[] = {"WARN", "ABORT", NullS};
TYPELIB check_table_fun_mode_typelib = {
    array_elements(check_table_fun_modes) - 1, "", check_table_fun_modes,
    nullptr};

namespace dd {
namespace upgrade {

/***************************************************************************
 * Bootstrap_error_handler implementation
 ***************************************************************************/

void Bootstrap_error_handler::my_message_bootstrap(uint error, const char *str,
                                                   myf MyFlags) {
  set_abort_on_error(error);
  my_message_sql(error, str, MyFlags);
  if (should_log_error(error))
    LogEvent()
        .type(LOG_TYPE_ERROR)
        .subsys(LOG_SUBSYSTEM_TAG)
        .prio(ERROR_LEVEL)
        .errcode(ER_ERROR_INFO_FROM_DA)
        .verbatim(str);
}

void Bootstrap_error_handler::set_abort_on_error(uint error) {
  switch (error) {
    case ER_WRONG_COLUMN_NAME: {
      abort_on_error = true;
      m_log_error = true;
      break;
    }
    default:
      break;
  }
}

Bootstrap_error_handler::Bootstrap_error_handler() {
  if (error_handler_hook != my_message_sql) {
    m_old_error_handler_hook = error_handler_hook;
    error_handler_hook = my_message_bootstrap;
  }
}

void Bootstrap_error_handler::set_log_error(bool log_error) {
  m_log_error = log_error;
}

bool Bootstrap_error_handler::should_log_error(uint error) {
  return (m_log_error ||
          (!m_allowlist_errors.empty() &&
           m_allowlist_errors.find(error) != m_allowlist_errors.end()));
}

void Bootstrap_error_handler::set_allowlist_errors(std::set<uint> &errors) {
  assert(m_allowlist_errors.empty());
  m_allowlist_errors = errors;
}

void Bootstrap_error_handler::clear_allowlist_errors() {
  m_allowlist_errors.clear();
}

Bootstrap_error_handler::~Bootstrap_error_handler() {
  // Skip reverting to old error handler in case someone else
  // has updated the hook.
  if (error_handler_hook == my_message_bootstrap)
    error_handler_hook = m_old_error_handler_hook;
}

bool Bootstrap_error_handler::m_log_error = true;
bool Bootstrap_error_handler::abort_on_error = false;
std::set<uint> Bootstrap_error_handler::m_allowlist_errors;

/***************************************************************************
 * Routine_event_context_guard implementation
 ***************************************************************************/

Routine_event_context_guard::Routine_event_context_guard(THD *thd)
    : m_thd(thd) {
  m_thd = thd;
  m_sql_mode = m_thd->variables.sql_mode;
  m_client_cs = m_thd->variables.character_set_client;
  m_connection_cl = m_thd->variables.collation_connection;
  m_saved_time_zone = m_thd->variables.time_zone;
}
Routine_event_context_guard::~Routine_event_context_guard() {
  m_thd->variables.sql_mode = m_sql_mode;
  m_thd->variables.character_set_client = m_client_cs;
  m_thd->variables.collation_connection = m_connection_cl;
  m_thd->variables.time_zone = m_saved_time_zone;
}

/***************************************************************************
 * Syntax_error_handler implementation
 ***************************************************************************/

uint Syntax_error_handler::parse_error_count = 0;
bool Syntax_error_handler::is_parse_error = false;
dd::String_type Syntax_error_handler::reason = "";
const uint Syntax_error_handler::MAX_SERVER_CHECK_FAILS = 50;

bool Syntax_error_handler::handle_condition(
    THD *, uint sql_errno, const char *,
    Sql_condition::enum_severity_level *level, const char *msg) {
  if (sql_errno == ER_PARSE_ERROR && *level == Sql_condition::SL_ERROR) {
    parse_error_count++;
    if (m_global_counter) (*m_global_counter)++;
    is_parse_error = true;
    reason = msg;
  } else {
    is_parse_error = false;
    reason = "";
  }
  return false;
}

bool Syntax_error_handler::has_too_many_errors() {
  return parse_error_count > MAX_SERVER_CHECK_FAILS;
}

bool Syntax_error_handler::has_errors() { return parse_error_count > 0; }

const char *Syntax_error_handler::error_message() { return reason.c_str(); }

/***************************************************************************
 * Upgrade_error_handler implementation
 ***************************************************************************/

bool Upgrade_error_counter::has_errors() { return (m_error_count > 0); }
bool Upgrade_error_counter::has_too_many_errors() {
  return (m_error_count > ERROR_LIMIT);
}
Upgrade_error_counter Upgrade_error_counter::operator++(int) {
  m_error_count++;
  return *this;
}

namespace {

static std::vector<uint> ignored_errors{
    ER_DUP_FIELDNAME, ER_DUP_KEYNAME, ER_BAD_FIELD_ERROR,
    ER_COL_COUNT_DOESNT_MATCH_PLEASE_UPDATE_V2, ER_DUP_ENTRY};

template <typename T>
class Server_option_guard {
  T *server_opt;
  T old_value;

 public:
  Server_option_guard(T *opt, T new_value) : server_opt(opt), old_value(*opt) {
    *server_opt = new_value;
  }

  ~Server_option_guard() { *server_opt = old_value; }
};

class MySQL_check {
 private:
  std::vector<dd::String_type> alter_cmds, repairs;
  bool needs_repair;

  static dd::String_type escape_str(const dd::String_type &src) {
    dd::String_type res = "`";
    for (size_t i = 0; i < src.size(); i++) {
      if (src[i] == '`') res += '`';
      res += src[i];
    }
    res += "`";
    return res;
  }

  void comma_separated_join(std::vector<dd::String_type> &list,
                            dd::String_type &dest) {
    dest = list[0];
    for (auto it = list.begin() + 1; it != list.end(); it++) dest += "," + *it;
  }

  bool get_schema_tables(THD *thd, const char *schema,
                         dd::String_type &tables_list) {
    Schema_MDL_locker mdl_handler(thd);
    dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
    const dd::Schema *sch = nullptr;
    std::vector<String_type> tables;
    dd::Stringstream_type t_list;

    if (mdl_handler.ensure_locked(schema) ||
        thd->dd_client()->acquire(schema, &sch) ||
        thd->dd_client()->fetch_schema_component_names<Abstract_table>(
            sch, &tables)) {
      LogErr(ERROR_LEVEL, ER_DD_UPGRADE_FAILED_TO_FETCH_TABLES);
      return (true);
    }

    char schema_name_buf[NAME_LEN + 1];
    const char *converted_schema_name = sch->name().c_str();
    if (lower_case_table_names == 2) {
      my_stpcpy(schema_name_buf, converted_schema_name);
      my_casedn_str(system_charset_info, schema_name_buf);
      converted_schema_name = schema_name_buf;
    }

    bool first = true;
    for (const dd::String_type &table : tables) {
      char table_name_buf[NAME_LEN + 1];
      const char *converted_table_name = table.c_str();
      if (lower_case_table_names == 2) {
        my_stpcpy(table_name_buf, converted_table_name);
        my_casedn_str(system_charset_info, table_name_buf);
        converted_table_name = table_name_buf;
      }

      MDL_request table_request;
      MDL_REQUEST_INIT(&table_request, MDL_key::TABLE, converted_schema_name,
                       converted_table_name, MDL_SHARED, MDL_EXPLICIT);

      if (thd->mdl_context.acquire_lock(&table_request,
                                        thd->variables.lock_wait_timeout)) {
        return true;
      }
      dd::cache::Dictionary_client::Auto_releaser table_releaser(
          thd->dd_client());
      const dd::Abstract_table *table_obj = nullptr;
      if (thd->dd_client()->acquire(converted_schema_name, converted_table_name,
                                    &table_obj))
        return true;

      if (table_obj->type() != dd::enum_table_type::BASE_TABLE ||
          table_obj->hidden() != dd::Abstract_table::HT_VISIBLE) {
        thd->mdl_context.release_lock(table_request.ticket);
        continue;
      }
      if (!first)
        t_list << ", ";
      else
        first = false;
      t_list << escape_str(sch->name()) << "." << escape_str(table_obj->name());
      thd->mdl_context.release_lock(table_request.ticket);
    }

    tables_list = t_list.str();
    return false;
  }

  bool check_table(List<Ed_row>::iterator &it,
                   const List<Ed_row>::iterator &end, bool repair) {
    Ed_row &row = *it;
    const char *table = row[0].str, *alter_txt = nullptr;
    bool found_error = false;
    it++;

    while (strcmp(row[2].str, "status")) {
      if (strcmp(row[2].str, "note")) {
        found_error = true;
        alter_txt = strstr(row[3].str, "ALTER TABLE");
      }
      if (it == end || strcmp((*it)[0].str, table)) break;
      row = *it;
      ++it;
    }

    if (found_error && strcmp(row[3].str, "OK")) {
      if (repair) {
        LogErr(ERROR_LEVEL, ER_SERVER_UPGRADE_REPAIR_STATUS, table, "failed");
        return true;
      } else
        LogErr(WARNING_LEVEL, ER_SERVER_UPGRADE_REPAIR_REQUIRED, table);
      if (alter_txt)
        alter_cmds.push_back(dd::String_type(alter_txt));
      else
        repairs.push_back(dd::String_type(table));
    } else if (repair) {
      LogErr(INFORMATION_LEVEL, ER_SERVER_UPGRADE_REPAIR_STATUS, table,
             "successful");
    } else
      found_error = false;

    return found_error;
  }

  bool verify_response(List<Ed_row> &rset, bool repair) {
    auto it = rset.begin();
    bool error = false;
    while (it != rset.end()) error |= check_table(it, rset.end(), repair);
    return error;
  }

  /**
    Returns true if something went wrong while retrieving the table list or
    executing CHECK TABLE statements.
  */
  bool check_tables(THD *thd, const char *schema) {
    Ed_connection con(thd);
    dd::String_type tables;
    LEX_STRING str;

    LogErr(INFORMATION_LEVEL, ER_SERVER_UPGRADE_CHECKING_DB, schema);
    if (get_schema_tables(thd, schema, tables)) return true;
    if (tables.size() == 0) return false;

    dd::String_type query = "CHECK TABLE " + tables + " FOR UPGRADE";
    lex_string_strmake(thd->mem_root, &str, query.c_str(), query.size());
    if (con.execute_direct(str)) return true;

    needs_repair |= verify_response(*con.get_result_sets(), false);
    return false;
  }

 public:
  MySQL_check() : needs_repair(false) {}

  bool check_all_schemas(THD *thd) {
    std::vector<dd::String_type> schemas;
    if (thd->dd_client()->fetch_global_component_names<dd::Schema>(&schemas))
      return true;
    for (dd::String_type &schema : schemas) {
      if (schema.compare("information_schema") == 0 ||
          schema.compare("performance_schema") == 0)
        continue;
      if (check_tables(thd, schema.c_str())) return true;
    }
    return false;
  }

  bool check_system_schemas(THD *thd) {
    return check_tables(thd, "mysql") || check_tables(thd, "sys");
  }

  bool repair_tables(THD *thd) {
    if (!needs_repair) return false;

    for (auto &alter : alter_cmds)
      if (dd::execute_query(thd, alter)) return true;
    alter_cmds.clear();

    if (repairs.size() == 0) return false;
    dd::String_type tables;
    comma_separated_join(repairs, tables);

    Ed_connection con(thd);
    LEX_STRING str;
    dd::String_type query = "REPAIR TABLE " + tables;
    lex_string_strmake(thd->mem_root, &str, query.c_str(), query.size());
    if (con.execute_direct(str)) return true;
    repairs.clear();
    needs_repair = false;
    (void)verify_response(*con.get_result_sets(), true);
    return false;
  }
};

bool ignore_error_and_execute(THD *thd, const char *query_ptr,
                              bool print_err = true) {
  Ed_connection con(thd);
  LEX_STRING str;
  lex_string_strmake(thd->mem_root, &str, query_ptr, strlen(query_ptr));

  // These are the same errors ignored in the mysql_upgrade client
  if (con.execute_direct(str) &&
      std::find(ignored_errors.begin(), ignored_errors.end(),
                con.get_last_errno()) == ignored_errors.end()) {
    if (print_err)
      LogErr(ERROR_LEVEL, ER_DD_INITIALIZE_SQL_ERROR, query_ptr,
             con.get_last_errno(), con.get_last_error());
    return true;
  }
  return false;
}

/**
 * This function will create the firewall's stored procedures.
 *
 * @param[in]        thd                thread context
 * @param[in]        drop_query         DROP statement to drop procedure
 * @param[in]        fw_proc            stored procedure's SQL definition
 *
 * @retval           false              execution of query successful
 * @retval           true               execution of query failed
 */
static bool reinstall_firewall_procedures(THD *thd, const char *drop_query,
                                          const char *fw_proc[]) {
  if (!ignore_error_and_execute(thd, drop_query, false))
    for (auto query = fw_proc; *query != nullptr; query++)
      if (ignore_error_and_execute(thd, *query)) return true;

  return false;
}

/**
 * This function will check and create the firewall's stored procedures.
 *
 * @param[in]        thd                thread context
 *
 * @retval           false              execution of query successful
 * @retval           true               execution of query failed
 */
static bool upgrade_firewall_procedures(THD *thd) {
  struct firewall_installer {
    const char *drop_query;
    const char **fwproc;
  };

  static firewall_installer fw_commands[] = {
      {"DROP PROCEDURE sp_set_firewall_mode", firewall_sp_set_firewall_mode},
      {"DROP PROCEDURE sp_reload_firewall_rules",
       firewall_sp_reload_firewall_rules},
      {"DROP PROCEDURE sp_set_firewall_group_mode",
       firewall_sp_set_firewall_group_mode},
      {"DROP PROCEDURE sp_set_firewall_group_mode_and_user",
       firewall_sp_set_firewall_group_mode_and_user},
      {"DROP PROCEDURE sp_reload_firewall_group_rules",
       firewall_sp_reload_firewall_group_rules},
      {"DROP PROCEDURE sp_firewall_group_enlist",
       firewall_sp_firewall_group_enlist},
      {"DROP PROCEDURE sp_firewall_group_delist",
       firewall_sp_firewall_group_delist},
  };

  for (auto &fw : fw_commands)
    if (reinstall_firewall_procedures(thd, fw.drop_query, fw.fwproc))
      return true;

  return false;
}

/**
 * This function will switch to the schema which is pointed by the
 * mysql-firewall-database variable by executing USE.
 *
 * @param[in]        thd                thread context
 * @param[out]       fw_schema          value of mysql-firewall-database
 *
 * @retval           false              execution of USE successful
 * @retval           true               execution of USE failed
 */
static bool switch_to_firewall_schema(THD *thd, std::string &fw_schema) {
  LEX_STRING firewall_schema_name = {nullptr, 0};
  Ed_connection conn(thd);

  lex_string_strmake(thd->mem_root, &firewall_schema_name,
                     STRING_WITH_LEN("SELECT @@mysql_firewall_database"));

  if (conn.execute_direct(firewall_schema_name)) return true;

  const List<Ed_row> rows = *(conn.get_result_sets());
  const MYSQL_LEX_STRING *result = rows[0]->get_column(0);
  fw_schema = result->str;

  // if firewall is installed in a schema other than mysql
  // then switch to that schema

  std::string command = "USE ";
  command.append(fw_schema);  // forms USE <schema>

  if (strcmp("mysql", fw_schema.c_str()))
    if (ignore_error_and_execute(thd, command.c_str())) return true;

  return false;
}

/** upgrades Firewall stored procedures */
static bool upgrade_firewall(THD *thd) {
  bool has_old_firewall_tables{false};
  bool has_new_firewall_tables{false};
  bool error{false};
  std::string fw_schema("mysql");

  {
    // lock required tables before checking their existence
    MDL_request request1, request2;
    MDL_REQUEST_INIT(&request1, MDL_key::TABLE, INFORMATION_SCHEMA_NAME.str,
                     "MYSQL_FIREWALL_USERS", MDL_SHARED, MDL_TRANSACTION);
    MDL_REQUEST_INIT(&request2, MDL_key::TABLE, PERFORMANCE_SCHEMA_DB_NAME.str,
                     "firewall_groups", MDL_SHARED, MDL_TRANSACTION);

    // check whether firewall tables exist
    error =
        (thd->mdl_context.acquire_lock(&request1,
                                       thd->variables.lock_wait_timeout) ||
         thd->mdl_context.acquire_lock(&request2,
                                       thd->variables.lock_wait_timeout) ||
         dd::table_exists(thd->dd_client(), INFORMATION_SCHEMA_NAME.str,
                          "MYSQL_FIREWALL_USERS", &has_old_firewall_tables) ||
         dd::table_exists(thd->dd_client(), PERFORMANCE_SCHEMA_DB_NAME.str,
                          "firewall_groups", &has_new_firewall_tables));

    // release locks, leave on error
    thd->mdl_context.release_transactional_locks();
    if (error) return true;
  }

  // upgrade the procedures
  if (has_old_firewall_tables || has_new_firewall_tables) {
    error = switch_to_firewall_schema(thd, fw_schema) ||
            upgrade_firewall_procedures(thd);

    // we might have switched to another schema during fw upgrade
    // go back to mysql
    if (fw_schema != "mysql")
      error |= ignore_error_and_execute(thd, "USE mysql");
  }
  return error;
}

bool fix_sys_schema(THD *thd) {
  /*
    Re-create SYS schema if:

    - There is a server upgrade going on.
    - Or the SYS schema does not exist.

    With the SYS schema versioning removed, we make sure there is indeed
    a server upgrade going on before we re-create the SYS schema. This has
    the consequence that upgrade=FORCE will not re-create the SYS schema,
    unless it does not exist. This is in line with the old behavior of the
    SYS schema versioning and upgrade.
  */
  Schema_MDL_locker mdl_handler(thd);
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  const dd::Schema *sch = nullptr;
  if (mdl_handler.ensure_locked("sys") ||
      thd->dd_client()->acquire("sys", &sch))
    return true;

  if (sch != nullptr &&
      !dd::bootstrap::DD_bootstrap_ctx::instance().is_server_upgrade() &&
      !bootstrap::DD_bootstrap_ctx::instance().is_server_patch_downgrade() &&
      (opt_upgrade_mode != UPGRADE_FORCE))
    return false;

  const char **query_ptr;
  LogErr(INFORMATION_LEVEL,
         dd::bootstrap::DD_bootstrap_ctx::instance().is_server_patch_downgrade()
             ? ER_SERVER_DOWNGRADE_SYS_SCHEMA
             : ER_SERVER_UPGRADE_SYS_SCHEMA);
  for (query_ptr = &mysql_sys_schema[0]; *query_ptr != nullptr; query_ptr++)
    if (ignore_error_and_execute(thd, *query_ptr)) return true;
  DBUG_EXECUTE_IF(
      "try_event_in_fix_sys_schema",
      ignore_error_and_execute(
          thd,
          "CREATE DEFINER = 'mysql.sys'@'localhost' EVENT sys_test_event ON "
          "SCHEDULE EVERY 1 MINUTE ENABLE DO SELECT 1");
      ignore_error_and_execute(thd, "DROP EVENT sys_test_event"););
  thd->mem_root->Clear();
  return false;
}

bool fix_mysql_tables(THD *thd) {
  /* Keep system tables as is for LTS downgrade. */
  if (bootstrap::DD_bootstrap_ctx::instance().is_server_patch_downgrade())
    return false;

  DBUG_EXECUTE_IF(
      "schema_read_only",
      if (dd::execute_query(thd, "CREATE SCHEMA schema_read_only") ||
          dd::execute_query(thd, "ALTER SCHEMA schema_read_only READ ONLY=1") ||
          dd::execute_query(thd, "CREATE TABLE schema_read_only.t(i INT)") ||
          dd::execute_query(thd, "DROP SCHEMA schema_read_only") ||
          dd::execute_query(thd, "CREATE TABLE IF NOT EXISTS S.upgrade(i INT)"))
          assert(false););

  if (ignore_error_and_execute(thd, "USE mysql")) {
    LogErr(ERROR_LEVEL, ER_DD_UPGRADE_FAILED_FIND_VALID_DATA_DIR);
    return true;
  }

  if (upgrade_firewall(thd)) return true;

  LogErr(INFORMATION_LEVEL, ER_SERVER_UPGRADE_MYSQL_TABLES);
  const char **query_ptr;
  for (query_ptr = &mysql_fix_privilege_tables[0]; *query_ptr != nullptr;
       query_ptr++)
    if (ignore_error_and_execute(thd, *query_ptr)) return true;

  LogErr(INFORMATION_LEVEL, ER_SERVER_UPGRADE_SYSTEM_TABLES);
  for (query_ptr = &mysql_system_tables_data_fix[0]; *query_ptr != nullptr;
       query_ptr++)
    if (ignore_error_and_execute(thd, *query_ptr)) return true;

  return false;
}

bool upgrade_help_tables(THD *thd) {
  // know if it's upgrade or downgrade
  bool is_downgrade =
      dd::bootstrap::DD_bootstrap_ctx::instance().is_server_patch_downgrade();

  if (dd::execute_query(thd, "USE mysql")) {
    LogErr(ERROR_LEVEL, ER_DD_UPGRADE_FAILED_FIND_VALID_DATA_DIR);
    return true;
  }

  LogErr(INFORMATION_LEVEL,
         is_downgrade ? ER_SERVER_DOWNGRADE_HELP_TABLE_STATUS
                      : ER_SERVER_UPGRADE_HELP_TABLE_STATUS,
         "started");

  for (const char **query_ptr = &fill_help_tables[0]; *query_ptr != nullptr;
       query_ptr++)
    if (dd::execute_query(thd, *query_ptr)) {
      LogErr(ERROR_LEVEL,
             is_downgrade ? ER_SERVER_DOWNGRADE_HELP_TABLE_STATUS
                          : ER_SERVER_UPGRADE_HELP_TABLE_STATUS,
             "failed");
      return true;
    }

  LogErr(INFORMATION_LEVEL,
         is_downgrade ? ER_SERVER_DOWNGRADE_HELP_TABLE_STATUS
                      : ER_SERVER_UPGRADE_HELP_TABLE_STATUS,
         "completed");

  return false;
}

static bool get_shared_tablespace_names(
    THD *thd, std::set<dd::String_type> *shared_spaces) {
  assert(innodb_hton != nullptr && innodb_hton->get_tablespace_type);
  auto process_spaces = [&](std::unique_ptr<dd::Tablespace> &space) {
    if (my_strcasecmp(system_charset_info, space->engine().c_str(), "InnoDB"))
      return false;
    Tablespace_type space_type;
    if (innodb_hton->get_tablespace_type(*space, &space_type)) {
      LogErr(ERROR_LEVEL, ER_UNKNOWN_TABLESPACE_TYPE, space->name().c_str());
      return true;
    }
    if (space_type != Tablespace_type::SPACE_TYPE_IMPLICIT)
      shared_spaces->insert(space->name());
    return false;
  };

  return thd->dd_client()->foreach<dd::Tablespace>(nullptr, process_spaces);
}

/*
  SQL error handler to use during check_table_funs, i.e.
  while we check table definitions for newly-broken SQL functions.
  It downgrades errors to warnings, and increases a counter each
  time each time it does so.
*/
class Sql_fun_error_handler : public Internal_error_handler {
  uint *m_error_count;  ///< Count of downgraded errors.

 public:
  /**
     Creates a new Sql_fun_error_handler.

     @param error_count  Address of the error count
  */
  explicit Sql_fun_error_handler(uint *error_count)
      : m_error_count(error_count) {}

 public:
  bool handle_condition(THD *, uint, const char *,
                        Sql_condition::enum_severity_level *sl,
                        const char *) override {
    if (*sl == Sql_condition::SL_ERROR) {
      (*m_error_count)++;
      return true;
    }
    return false;
  }
};

/**
  Check table definitions for SQL functions.

  Sometimes, improving a SQL function's behaviour or diagnostics
  may result in an error being thrown in situations where this
  wasn't the case. If that call is part of a table's definition,
  the error will be thrown during the opening of the table,
  which will then fail.

  This is something the user will generally want to know before
  putting an upgraded database into production. Therefore, we
  inspect all user tables that (potentially) have SQL functions
  in DEFAULT clauses, PARTITIONing, virtual columns, or indexes.
  Any such table we try to open. This may take a while, but it
  is preferable to not being aware of breakage.

  @param  thd          The THD to use.
  @param  schema       The schema whose tables to examine.
  @param  error_count  Count of errors (total for all check functions).
  @return false        True if too many errors were detected, false otherwise.
*/
static bool check_table_funs(THD *thd, std::unique_ptr<Schema> &schema,
                             Upgrade_error_counter *error_count) {
  uint sql_fun_errors = 0;

  // Function called on each table to validate it.
  auto process_table = [&](std::unique_ptr<dd::Table> &table) {
    // Skip non-InnoDB tables as their search engine may not be available yet.
    if (my_strcasecmp(system_charset_info, table->engine().c_str(), "InnoDB"))
      return false;

    // Are SQL functions used in table def (defaults, virtual columns, etc.)?
    if (dd::uses_functions(table.get(), schema->name().c_str())) {
      Open_table_context ot_ctx(
          thd, MYSQL_OPEN_GET_NEW_TABLE | MYSQL_OPEN_NO_NEW_TABLE_IN_SE);
      Table_ref tr(schema->name().c_str(), table->name().c_str(), TL_READ);

      // Did trying to open this table throw any new errors?
      uint old_errors = sql_fun_errors;
      open_table(thd, &tr, &ot_ctx);

      // Did we catch any errors that would have prevented open_table()?
      if (sql_fun_errors > old_errors) {
        // Log that the table has problems.
        LogErr(WARNING_LEVEL, ER_CHECK_TABLE_FUNCTIONS, schema->name().c_str(),
               table->name().c_str());

        // On higher log levels, create a detailed description of the table.
        dd::String_type debug_info;
        dd::uses_functions(table.get(), schema->name().c_str(), &debug_info);
        LogErr(INFORMATION_LEVEL, ER_CHECK_TABLE_FUNCTIONS_DETAIL,
               debug_info.c_str());

        // increase global error count
        if (opt_check_table_funs == CHECK_TABLE_FUN_ABORT) (*error_count)++;
      }
    }
    return error_count->has_too_many_errors();
  };

  // Skip pfs.
  if (0 == schema->name().compare("performance_schema")) return false;

  // Our error handler counts errors and downgrades them to warnings.
  Sql_fun_error_handler error_handler(&sql_fun_errors);
  thd->push_internal_handler(&error_handler);

  std::unique_ptr<dd::Object_key> table_key(
      dd::Table::DD_table::create_key_by_schema_id(schema->id()));

  // Iterate over tables in this schema.
  bool res =
      thd->dd_client()->foreach<dd::Table>(table_key.get(), process_table);

  // Clean up.
  thd->pop_internal_handler();

  return res;
}

static bool check_tables(THD *thd, std::unique_ptr<Schema> &schema,
                         const std::set<dd::String_type> *shared_spaces,
                         Upgrade_error_counter *error_count) {
  std::unique_ptr<Object_key> table_key(
      dd::Table::DD_table::create_key_by_schema_id(schema->id()));

  auto process_table = [&](std::unique_ptr<dd::Table> &table) {
    invalid_triggers(thd, schema->name().c_str(), *table);

    // Check for usage of prefix key index in PARTITION BY KEY() function.
    if (dd::prefix_key_partition_exists(
            schema->name().c_str(), table->name().c_str(), table.get(), true))
      return true;

    dd::check_non_standard_key_exists_in_fk(thd, table.get());

    // Check for partitioned innodb tables using shared spaces.
    if (!shared_spaces->empty() &&
        table->partition_type() != dd::Table::PT_NONE &&
        my_strcasecmp(system_charset_info, table->engine().c_str(), "InnoDB") ==
            0) {
      Tablespace_hash_set space_names(PSI_INSTRUMENT_ME);
      if (fill_table_and_parts_tablespace_names(
              thd, schema->name().c_str(), table->name().c_str(), &space_names))
        return true;

      for (const std::string &name : space_names) {
        if (shared_spaces->find(String_type(name.c_str())) !=
            shared_spaces->end()) {
          (*error_count)++;
          LogErr(ERROR_LEVEL, ER_SHARED_TABLESPACE_USED_BY_PARTITIONED_TABLE,
                 table->name().c_str(), name.c_str());
        }
      }
    }

    // Check if AUTO_INCREMENT is used with DOUBLE/FLOAT
    for (const auto &col : *table->columns()) {
      if (col->is_auto_increment() &&
          (col->type() == enum_column_types::DOUBLE ||
           col->type() == enum_column_types::FLOAT)) {
        (*error_count)++;
        LogErr(ERROR_LEVEL, ER_AUTO_INCREMENT_NOT_SUPPORTED_FOR_FLOAT_DOUBLE,
               schema->name().c_str(), table->name().c_str(),
               col->name().c_str());
      }
    }

    return error_count->has_too_many_errors();
  };

  return thd->dd_client()->foreach<dd::Table>(table_key.get(), process_table);
}

static bool check_events(THD *thd, std::unique_ptr<Schema> &schema,
                         Upgrade_error_counter *error_count) {
  std::unique_ptr<Object_key> event_key(
      dd::Event::DD_table::create_key_by_schema_id(schema->id()));

  auto process_event = [&](std::unique_ptr<dd::Event> &event) {
    dd::String_type sql;
    if (build_event_sp(thd, event->name().c_str(), event->name().size(),
                       event->definition().c_str(), event->definition().size(),
                       &sql) ||
        invalid_sql(thd, schema->name().c_str(), sql))
      LogErr(ERROR_LEVEL, ER_UPGRADE_PARSE_ERROR, "Event",
             schema->name().c_str(), event->name().c_str(),
             Syntax_error_handler::error_message());
    return error_count->has_too_many_errors();
  };

  return thd->dd_client()->foreach<dd::Event>(event_key.get(), process_event);
}

static bool check_routines(THD *thd, std::unique_ptr<Schema> &schema,
                           Upgrade_error_counter *error_count) {
  std::unique_ptr<Object_key> routine_key(
      dd::Routine::DD_table::create_key_by_schema_id(schema->id()));

  auto process_routine = [&](std::unique_ptr<dd::Routine> &routine) {
    if (invalid_routine(thd, *schema, *routine))
      LogErr(ERROR_LEVEL, ER_UPGRADE_PARSE_ERROR, "Routine",
             schema->name().c_str(), routine->name().c_str(),
             Syntax_error_handler::error_message());
    return error_count->has_too_many_errors();
  };

  return thd->dd_client()->foreach<dd::Routine>(routine_key.get(),
                                                process_routine);
}

static bool check_views(THD *thd, std::unique_ptr<Schema> &schema,
                        Upgrade_error_counter *error_count) {
  std::unique_ptr<Object_key> view_key(
      dd::View::DD_table::create_key_by_schema_id(schema->id()));

  auto process_view = [&](std::unique_ptr<dd::View> &view) {
    if (invalid_sql(thd, schema->name().c_str(), view->definition()))
      LogErr(ERROR_LEVEL, ER_UPGRADE_PARSE_ERROR, "View",
             schema->name().c_str(), view->name().c_str(),
             Syntax_error_handler::error_message());
    return error_count->has_too_many_errors();
  };

  return thd->dd_client()->foreach<dd::View>(view_key.get(), process_view);
}

/* Make sure the old unsupported "mysql_upgrade_info" file is removed. */
static void remove_legacy_upgrade_info_file() {
  char upgrade_file[FN_REFLEN] = {0};
  fn_format(upgrade_file, "mysql_upgrade_info", mysql_real_data_home_ptr, "",
            MYF(0));
  if (!my_access(upgrade_file, F_OK))
    std::ignore = mysql_file_delete(key_file_misc, upgrade_file, MYF(0));
}
}  // namespace

/*
  Maintain a file named "mysql_upgrade_history" in the data directory.

  The file will contain one entry for each upgrade. The format is structured
  text on JSON format.

  Errors will be written as warnings to the error log; if we e.g. fail to
  open the upgrade history file, we will not abort the server since this file
  is not considered a critical feature of the server.

  @param initialize   If this is the initialization of the data directory.
*/
void update_upgrade_history_file(bool initialize) {
  /* Name of the "mysql_upgrade_history" file. */
  char upgrade_file[FN_REFLEN] = {0};
  fn_format(upgrade_file, "mysql_upgrade_history", mysql_real_data_home_ptr, "",
            MYF(0));

  /* JSON keys. */
  constexpr char k_file_format[] = "file_format";
  constexpr char k_upgrade_history[] = "upgrade_history";
  constexpr char k_date[] = "date";
  constexpr char k_version[] = "version";
  constexpr char k_maturity[] = "maturity";
  constexpr char k_initialize[] = "initialize";
  constexpr char v_file_format[] = "1";

  /* If > max entries, we keep the first and the (max - 1)  last ones. */
  constexpr int MAX_HISTORY_SIZE = 1000;
  static_assert(MAX_HISTORY_SIZE >= 2,
                "The upgrade history should contain at least the first "
                "and last entry.");
  using namespace rapidjson;
  Document doc;

  /* Open file if it exists, auto close on return. */
  auto deleter = [&](FILE *ptr) {
    if (ptr != nullptr) my_fclose(ptr, MYF(0));
  };
  std::unique_ptr<FILE, decltype(deleter)> fp(nullptr, deleter);

  MY_STAT sa;
  char errbuf[MYSYS_STRERROR_SIZE];
  bool file_exists = (my_stat(upgrade_file, &sa, MYF(0)) != nullptr);
  bool append = file_exists;  // Append to current doc if possible.

  /* If the file exists, read the doc and see if it is valid. */
  if (file_exists) {
    fp.reset(my_fopen(upgrade_file, O_RDONLY, MYF(0)));
    if (fp == nullptr) {
      LogErr(WARNING_LEVEL, ER_SERVER_CANT_OPEN_FILE, upgrade_file, my_errno(),
             my_strerror(errbuf, sizeof(errbuf), my_errno()));
      return;
    }

    /* Read contents into buffer. */
    char buff[512] = {0};
    std::string parsed_value;
    do {
      parsed_value.append(buff);
      buff[0] = '\0';
    } while (fgets(buff, sizeof(buff) - 1, fp.get()));

    /* Parse JSON, check expected format. */
    ParseResult ok = doc.Parse(parsed_value.c_str());
    if (!(ok && doc.IsObject() && doc[k_file_format].IsString() &&
          doc[k_upgrade_history].IsArray())) {
      LogErr(WARNING_LEVEL, ER_INVALID_FILE_FORMAT, upgrade_file);
      append = false;  // Cannot append, must overwrite with an empty doc.
    }
  }

  /* If the file existed with valid contents, append, otherwise, overwrite. */
  if (append) {
    /* If current version is same as last entry, return. */
    Value &hist = doc[k_upgrade_history].GetArray();
    int count = hist.Size();
    if (count > 0 &&
        !strcmp(hist[count - 1][k_version].GetString(), server_version))
      return;

    /* If the doc contains too many entries, remove from the second and on. */
    int remove_count = (count - MAX_HISTORY_SIZE) + 1;
    if (remove_count > 0) {
      hist.Erase(hist.Begin() + 1, hist.Begin() + remove_count + 1);
    }
  } else {
    /* Otherwise, if no file existed, initialize an empty JSON document. */
    doc.SetObject();
    doc.AddMember(k_file_format, v_file_format, doc.GetAllocator());
    Value history(kArrayType);
    doc.AddMember(k_upgrade_history, history, doc.GetAllocator());
  }

  /* Append timestamp, MYSQL_SERVER_VERSION and LTS info to version array. */
  std::stringstream str;
  std::time_t sec = my_micro_time() / 1000000;
  str << std::put_time(std::gmtime(&sec), "%F %T");
  Value date(str.str().c_str(), str.str().size(), doc.GetAllocator());
  Value version(server_version, strlen(server_version), doc.GetAllocator());
  Value maturity(MYSQL_VERSION_MATURITY);

  Value new_version;
  new_version.SetObject();
  new_version.AddMember(k_date, date, doc.GetAllocator());
  new_version.AddMember(k_version, version, doc.GetAllocator());
  new_version.AddMember(k_maturity, maturity, doc.GetAllocator());
  if (initialize) {
    Value init(true);
    new_version.AddMember(k_initialize, init, doc.GetAllocator());
  }
  doc[k_upgrade_history].GetArray().PushBack(new_version, doc.GetAllocator());

  /* Reopen the file, which is auto closed on function return. */
  fp.reset(my_fopen(upgrade_file, O_CREAT | O_TRUNC | O_WRONLY, MYF(0)));
  if (fp == nullptr) {
    LogErr(WARNING_LEVEL, ER_SERVER_CANT_OPEN_FILE, upgrade_file, my_errno(),
           my_strerror(errbuf, sizeof(errbuf), my_errno()));
    return;
  }

  /* Write JSON document to a buffer and further to file with a newline. */
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  doc.Accept(writer);
  fputs(buffer.GetString(), fp.get());
  fputs("\n", fp.get());
}

/*
  This function runs checks on the database before running the upgrade to make
  sure that the database is ready to be upgraded to a newer version. New checks
  can be added as required. Returns false if the database can be upgraded.
*/
bool do_server_upgrade_checks(THD *thd) {
  if (!dd::bootstrap::DD_bootstrap_ctx::instance().is_server_upgrade_from_after(
          bootstrap::SERVER_VERSION_50700))
    return false;

  /*
    If upgrade is crossing 8.0.13, we need to look out for partitioned tables
    having partitions in shared tablespaces, and err out if this is found. We
    first collect the shared tablespace names into a set, then this set is
    checked when analyzing tables below.
  */
  dd::cache::Dictionary_client::Auto_releaser releaser(thd->dd_client());
  std::set<dd::String_type> shared_spaces;
  if (dd::bootstrap::DD_bootstrap_ctx::instance().is_server_upgrade_from_before(
          bootstrap::SERVER_VERSION_80013) &&
      get_shared_tablespace_names(thd, &shared_spaces))
    return dd::end_transaction(thd, true);

  /*
    For any server upgrade, we will analyze events, routines, views and
    triggers and reject upgrade if we find invalid syntax or other issues
    that would not have been accepted in a CREATE statement.

    We iterate over the schemas and analyze all entities in each of them.
    For each step, if there is an error that we can not ignore, or if the
    number of errors exceeds a limit, we break out of the analysis and end
    the upgrade.

    For errors that can be ignored (e.g. invalid syntax), we keep on analyzing
    to identify as many errors as possible in one go.
  */
  Upgrade_error_counter error_count;
  Syntax_error_handler error_handler(&error_count);
  thd->push_internal_handler(&error_handler);

  auto process_schema = [&](std::unique_ptr<Schema> &schema) {
    return check_tables(thd, schema, &shared_spaces, &error_count) ||
           check_events(thd, schema, &error_count) ||
           check_routines(thd, schema, &error_count) ||
           check_views(thd, schema, &error_count) ||
           check_table_funs(thd, schema, &error_count);
  };

  if (thd->dd_client()->foreach<dd::Schema>(nullptr, process_schema) ||
      error_count.has_errors()) {
    thd->pop_internal_handler();
    return dd::end_transaction(thd, true);
  }
  ulong non_std_key_cnt = deprecated_use_fk_on_non_standard_key_count;
  if (non_std_key_cnt != 0) {
    LogErr(WARNING_LEVEL, ER_USAGE_DEPRECATION_COUNTER,
           "foreign key referring to a non-unique or partial key",
           std::to_string(non_std_key_cnt).c_str(), "during upgrade");
  }
  thd->pop_internal_handler();
  return false;
}

/**
  Validate a dd::Routine object.
*/
bool invalid_routine(THD *thd, const dd::Schema &schema,
                     const dd::Routine &routine) {
  Routine_event_context_guard guard(thd);
  sp_head *sp = nullptr;
  st_sp_chistics chistics;
  prepare_sp_chistics_from_dd_routine(thd, &routine, &chistics);

  dd::String_type return_type_str;
  prepare_return_type_string_from_dd_routine(thd, &routine, &return_type_str);

  // Create SP creation context to be used in db_load_routine()
  Stored_program_creation_ctx *creation_ctx =
      Stored_routine_creation_ctx::create_routine_creation_ctx(&routine);

  thd->variables.character_set_client = creation_ctx->get_client_cs();
  thd->variables.collation_connection = creation_ctx->get_connection_cl();
  thd->update_charset();

  enum_sp_return_code error = db_load_routine(
      thd,
      routine.type() == dd::Routine::RT_FUNCTION ? enum_sp_type::FUNCTION
                                                 : enum_sp_type::PROCEDURE,
      schema.name().c_str(), schema.name().size(), routine.name().c_str(),
      routine.name().size(), &sp, routine.sql_mode(),
      routine.parameter_str().c_str(), return_type_str.c_str(),
      routine.definition().c_str(), &chistics, routine.definer_user().c_str(),
      routine.definer_host().c_str(), routine.created(true),
      routine.last_altered(true), creation_ctx);

  if (sp != nullptr)  // To be safe
    sp_head::destroy(sp);

  if (error) return (thd->get_stmt_da()->mysql_errno() == ER_PARSE_ERROR);
  thd->clear_error();
  return false;
}

/**
  Validate all the triggers of the given table.
*/
bool invalid_triggers(THD *thd, const char *schema_name,
                      const dd::Table &table) {
  if (!table.has_trigger()) return false;
  List<::Trigger> triggers;
  if (dd::load_triggers(thd, thd->mem_root, schema_name, table.name().c_str(),
                        table, &triggers))
    return true;
  for (::Trigger &t : triggers) {
    if (t.parse(thd, false) || t.has_parse_error()) {
      LogEvent()
          .type(LOG_TYPE_ERROR)
          .subsys(LOG_SUBSYSTEM_TAG)
          .prio(ERROR_LEVEL)
          .errcode(ER_UPGRADE_PARSE_ERROR)
          .verbatim(t.get_parse_error_message());
      thd->clear_error();
    }
    sp_head::destroy(t.get_sp());
    if (Syntax_error_handler::has_too_many_errors()) return true;
  }
  return Syntax_error_handler::has_errors();
}

bool invalid_sql(THD *thd, const char *dbname, const dd::String_type &sql) {
  bool error = false;
  Parser_state *old = thd->m_parser_state;
  Parser_state parser_state;

  if (parser_state.init(thd, sql.c_str(), sql.size())) return true;

  LEX_CSTRING old_db = thd->db();
  LEX lex, *lex_saved = thd->lex;

  thd->reset_db(to_lex_cstring(dbname));
  thd->lex = &lex;
  lex_start(thd);

  thd->m_parser_state = &parser_state;
  parser_state.m_lip.m_digest = nullptr;

  if (thd->sql_parser())
    error = (thd->get_stmt_da()->mysql_errno() == ER_PARSE_ERROR);

  lex_end(thd->lex);
  thd->lex = lex_saved;
  thd->reset_db(old_db);
  thd->m_parser_state = old;
  thd->clear_error();

  return error;
}

/**
  Helper function to create a stored procedure from an event body.
*/
bool build_event_sp(const THD *thd, const char *name, size_t name_len,
                    const char *body, size_t body_len,
                    dd::String_type *sp_sql) {
  const uint STATIC_SQL_LENGTH = 44;
  String temp(STATIC_SQL_LENGTH + name_len + body_len);

  temp.append(STRING_WITH_LEN("CREATE "));
  temp.append(STRING_WITH_LEN("PROCEDURE "));

  append_identifier(thd, &temp, name, name_len);

  temp.append(STRING_WITH_LEN("() SQL SECURITY INVOKER "));
  temp.append(body, body_len);

  *sp_sql = temp.ptr();
  return false;
}

bool upgrade_system_schemas(THD *thd) {
  Disable_autocommit_guard autocommit_guard(thd);
  Bootstrap_error_handler bootstrap_error_handler;

  Server_option_guard<bool> acl_guard(&opt_noacl, true);
  Server_option_guard<bool> general_log_guard(&opt_general_log, false);
  Server_option_guard<bool> slow_log_guard(&opt_slow_log, false);
  Disable_binlog_guard disable_binlog(thd);
  Disable_sql_log_bin_guard disable_sql_log_bin(thd);

  uint server_version = MYSQL_VERSION_ID;
  bool exists_version = false;

  if (dd::tables::DD_properties::instance().get(
          thd, "MYSQLD_VERSION_UPGRADED", &server_version, &exists_version) ||
      !exists_version)
    if (dd::tables::DD_properties::instance().get(
            thd, "MYSQLD_VERSION", &server_version, &exists_version) ||
        !exists_version)
      return true;

  MySQL_check check;

  if (dd::bootstrap::DD_bootstrap_ctx::instance().is_server_patch_downgrade()) {
    /* purecov: begin inspected */
    LogErr(SYSTEM_LEVEL, ER_SERVER_DOWNGRADE_STATUS, server_version,
           MYSQL_VERSION_ID, "started");
    sysd::notify("STATUS=Server downgrade in progress\n");
    /* purecov: end */
  } else {
    LogErr(SYSTEM_LEVEL, ER_SERVER_UPGRADE_STATUS, server_version,
           MYSQL_VERSION_ID, "started");
    sysd::notify("STATUS=Server upgrade in progress\n");
  }

  bootstrap_error_handler.set_log_error(false);
  bool err =
      fix_mysql_tables(thd) || fix_sys_schema(thd) || upgrade_help_tables(thd);
  if (!err) {
    /*
      Initialize structures necessary for federated server from mysql.servers
      table.
    */
    servers_init(thd);
    err = (DBUG_EVALUATE_IF("force_fix_user_schemas", true,
                            dd::bootstrap::DD_bootstrap_ctx::instance()
                                .is_server_upgrade_from_before(
                                    bootstrap::SERVER_VERSION_80011))
               ? check.check_all_schemas(thd)
               : check.check_system_schemas(thd)) ||
          check.repair_tables(thd) ||
          dd::tables::DD_properties::instance().set(
              thd, "MYSQLD_VERSION_UPGRADED", MYSQL_VERSION_ID);
  }

  remove_legacy_upgrade_info_file();
  bootstrap_error_handler.set_log_error(true);

  if (dd::bootstrap::DD_bootstrap_ctx::instance().is_server_patch_downgrade()) {
    /* purecov: begin inspected */
    if (!err)
      LogErr(SYSTEM_LEVEL, ER_SERVER_DOWNGRADE_STATUS, server_version,
             MYSQL_VERSION_ID, "completed");
    sysd::notify("STATUS=Server downgrade complete\n");
    /* purecov: end */
  } else {
    if (!err)
      LogErr(SYSTEM_LEVEL, ER_SERVER_UPGRADE_STATUS, server_version,
             MYSQL_VERSION_ID, "completed");
    sysd::notify("STATUS=Server upgrade complete\n");
  }

  /*
   * During server startup, dd::reset_tables_and_tablespaces is called, which
   * calls innobase_dict_cache_reset_tables_and_tablespaces. This tries to clear
   * the open tables cache. But not able to, which causes an assert. So we force
   * close everything.
   */
  close_thread_tables(thd);
  close_cached_tables(nullptr, nullptr, false, LONG_TIMEOUT);

  return dd::end_transaction(thd, err);
}

bool no_server_upgrade_required() {
  return !(
      dd::bootstrap::DD_bootstrap_ctx::instance().is_server_upgrade() ||
      bootstrap::DD_bootstrap_ctx::instance().is_server_patch_downgrade() ||
      opt_upgrade_mode == UPGRADE_FORCE);
}

bool I_S_upgrade_required() {
  return dd::bootstrap::DD_bootstrap_ctx::instance().is_server_upgrade() ||
         bootstrap::DD_bootstrap_ctx::instance().is_server_patch_downgrade() ||
         dd::bootstrap::DD_bootstrap_ctx::instance().I_S_upgrade_done() ||
         opt_upgrade_mode == UPGRADE_FORCE;
}

}  // namespace upgrade
}  // namespace dd
