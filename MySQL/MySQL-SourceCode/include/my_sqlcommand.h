/* Copyright (c) 2015, 2025, Oracle and/or its affiliates.

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

#ifndef _mysql_sql_command_h
#define _mysql_sql_command_h

/**
  @file include/my_sqlcommand.h
*/

/*
  @enum  enum_sql_command
  @brief SQL Commands

         SQL Command is resolved during SQL parsing and assigned to the Lex
         object, accessible from the THD.

         When a command is added here, be sure it's also added in mysqld.cc
         in "struct show_var_st status_vars[]= {" ...

         If the command returns a result set or is not allowed in stored
         functions or triggers, please also make sure that
         sp_get_flags_for_command (sp_head.cc) returns proper flags for the
         added SQLCOM_.
*/
enum enum_sql_command {
  SQLCOM_SELECT,
  SQLCOM_CREATE_TABLE,
  SQLCOM_CREATE_INDEX,
  SQLCOM_ALTER_TABLE,
  SQLCOM_UPDATE,
  SQLCOM_INSERT,
  SQLCOM_INSERT_SELECT,
  SQLCOM_DELETE,
  SQLCOM_TRUNCATE,
  SQLCOM_DROP_TABLE,
  SQLCOM_DROP_INDEX,
  SQLCOM_SHOW_DATABASES,
  SQLCOM_SHOW_TABLES,
  SQLCOM_SHOW_FIELDS,
  SQLCOM_SHOW_KEYS,
  SQLCOM_SHOW_VARIABLES,
  SQLCOM_SHOW_STATUS,
  SQLCOM_SHOW_ENGINE_LOGS,
  SQLCOM_SHOW_ENGINE_STATUS,
  SQLCOM_SHOW_ENGINE_MUTEX,
  SQLCOM_SHOW_PROCESSLIST,
  SQLCOM_SHOW_BINLOG_STATUS,
  SQLCOM_SHOW_REPLICA_STATUS,
  SQLCOM_SHOW_GRANTS,
  SQLCOM_SHOW_CREATE,
  SQLCOM_SHOW_CHARSETS,
  SQLCOM_SHOW_COLLATIONS,
  SQLCOM_SHOW_CREATE_DB,
  SQLCOM_SHOW_TABLE_STATUS,
  SQLCOM_SHOW_TRIGGERS,
  SQLCOM_LOAD,
  SQLCOM_SET_OPTION,
  SQLCOM_LOCK_TABLES,
  SQLCOM_UNLOCK_TABLES,
  SQLCOM_GRANT,
  SQLCOM_CHANGE_DB,
  SQLCOM_CREATE_DB,
  SQLCOM_DROP_DB,
  SQLCOM_ALTER_DB,
  SQLCOM_REPAIR,
  SQLCOM_REPLACE,
  SQLCOM_REPLACE_SELECT,
  SQLCOM_CREATE_FUNCTION,
  SQLCOM_DROP_FUNCTION,
  SQLCOM_REVOKE,
  SQLCOM_OPTIMIZE,
  SQLCOM_CHECK,
  SQLCOM_ASSIGN_TO_KEYCACHE,
  SQLCOM_PRELOAD_KEYS,
  SQLCOM_FLUSH,
  SQLCOM_KILL,
  SQLCOM_ANALYZE,
  SQLCOM_ROLLBACK,
  SQLCOM_ROLLBACK_TO_SAVEPOINT,
  SQLCOM_COMMIT,
  SQLCOM_SAVEPOINT,
  SQLCOM_RELEASE_SAVEPOINT,
  SQLCOM_REPLICA_START,
  SQLCOM_REPLICA_STOP,
  SQLCOM_START_GROUP_REPLICATION,
  SQLCOM_STOP_GROUP_REPLICATION,
  SQLCOM_BEGIN,
  SQLCOM_CHANGE_REPLICATION_SOURCE,
  SQLCOM_CHANGE_REPLICATION_FILTER,
  SQLCOM_RENAME_TABLE,
  SQLCOM_RESET,
  SQLCOM_PURGE,
  SQLCOM_PURGE_BEFORE,
  SQLCOM_SHOW_BINLOGS,
  SQLCOM_SHOW_OPEN_TABLES,
  SQLCOM_HA_OPEN,
  SQLCOM_HA_CLOSE,
  SQLCOM_HA_READ,
  SQLCOM_SHOW_REPLICAS,
  SQLCOM_DELETE_MULTI,
  SQLCOM_UPDATE_MULTI,
  SQLCOM_SHOW_BINLOG_EVENTS,
  SQLCOM_DO,
  SQLCOM_SHOW_WARNS,
  SQLCOM_EMPTY_QUERY,
  SQLCOM_SHOW_ERRORS,
  SQLCOM_SHOW_STORAGE_ENGINES,
  SQLCOM_SHOW_PRIVILEGES,
  SQLCOM_HELP,
  SQLCOM_CREATE_USER,
  SQLCOM_DROP_USER,
  SQLCOM_RENAME_USER,
  SQLCOM_REVOKE_ALL,
  SQLCOM_CHECKSUM,
  SQLCOM_CREATE_PROCEDURE,
  SQLCOM_CREATE_SPFUNCTION,
  SQLCOM_CALL,
  SQLCOM_DROP_PROCEDURE,
  SQLCOM_ALTER_PROCEDURE,
  SQLCOM_ALTER_FUNCTION,
  SQLCOM_SHOW_CREATE_PROC,
  SQLCOM_SHOW_CREATE_FUNC,
  SQLCOM_SHOW_STATUS_PROC,
  SQLCOM_SHOW_STATUS_FUNC,
  SQLCOM_PREPARE,
  SQLCOM_EXECUTE,
  SQLCOM_DEALLOCATE_PREPARE,
  SQLCOM_CREATE_VIEW,
  SQLCOM_DROP_VIEW,
  SQLCOM_CREATE_TRIGGER,
  SQLCOM_DROP_TRIGGER,
  SQLCOM_XA_START,
  SQLCOM_XA_END,
  SQLCOM_XA_PREPARE,
  SQLCOM_XA_COMMIT,
  SQLCOM_XA_ROLLBACK,
  SQLCOM_XA_RECOVER,
  SQLCOM_SHOW_PROC_CODE,
  SQLCOM_SHOW_FUNC_CODE,
  SQLCOM_ALTER_TABLESPACE,
  SQLCOM_INSTALL_PLUGIN,
  SQLCOM_UNINSTALL_PLUGIN,
  SQLCOM_BINLOG_BASE64_EVENT,
  SQLCOM_SHOW_PLUGINS,
  SQLCOM_CREATE_SERVER,
  SQLCOM_DROP_SERVER,
  SQLCOM_ALTER_SERVER,
  SQLCOM_CREATE_EVENT,
  SQLCOM_ALTER_EVENT,
  SQLCOM_DROP_EVENT,
  SQLCOM_SHOW_CREATE_EVENT,
  SQLCOM_SHOW_EVENTS,
  SQLCOM_SHOW_CREATE_TRIGGER,
  SQLCOM_SHOW_PROFILE,
  SQLCOM_SHOW_PROFILES,
  SQLCOM_SIGNAL,
  SQLCOM_RESIGNAL,
  SQLCOM_SHOW_RELAYLOG_EVENTS,
  SQLCOM_GET_DIAGNOSTICS,
  SQLCOM_ALTER_USER,
  SQLCOM_EXPLAIN_OTHER,
  SQLCOM_SHOW_CREATE_USER,
  SQLCOM_SHUTDOWN,
  SQLCOM_SET_PASSWORD,
  SQLCOM_ALTER_INSTANCE,
  SQLCOM_INSTALL_COMPONENT,
  SQLCOM_UNINSTALL_COMPONENT,
  SQLCOM_CREATE_ROLE,
  SQLCOM_DROP_ROLE,
  SQLCOM_SET_ROLE,
  SQLCOM_GRANT_ROLE,
  SQLCOM_REVOKE_ROLE,
  SQLCOM_ALTER_USER_DEFAULT_ROLE,
  SQLCOM_IMPORT,
  SQLCOM_CREATE_RESOURCE_GROUP,
  SQLCOM_ALTER_RESOURCE_GROUP,
  SQLCOM_DROP_RESOURCE_GROUP,
  SQLCOM_SET_RESOURCE_GROUP,
  SQLCOM_CLONE,
  SQLCOM_LOCK_INSTANCE,
  SQLCOM_UNLOCK_INSTANCE,
  SQLCOM_RESTART_SERVER,
  SQLCOM_CREATE_SRS,
  SQLCOM_DROP_SRS,
  SQLCOM_SHOW_PARSE_TREE,
  SQLCOM_CREATE_LIBRARY,
  SQLCOM_DROP_LIBRARY,
  SQLCOM_SHOW_CREATE_LIBRARY,
  SQLCOM_ALTER_LIBRARY,
  SQLCOM_SHOW_STATUS_LIBRARY,
  /* This should be the last !!! */
  SQLCOM_END
};

#endif /* _mysql_sql_command_h */
