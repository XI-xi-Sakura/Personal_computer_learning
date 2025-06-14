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
#ifndef PLUGIN_PROTOCOL_INCLUDED
#define PLUGIN_PROTOCOL_INCLUDED

#ifndef MYSQL_ABI_CHECK
#include "field_types.h" /* enum_field_types */
#include "mysql_com.h"   /* mysql_enum_shutdown_level */
#endif

/**
  @file include/mysql/com_data.h
  Definition of COM_DATA to be used with the Command service as data input
  structure.
*/

struct COM_INIT_DB_DATA {
  const char *db_name;
  unsigned long length;
};

struct COM_SET_OPTION_DATA {
  unsigned int opt_command;
};

struct PS_PARAM {
  unsigned char null_bit;
  enum enum_field_types type;
  unsigned char unsigned_type;
  const unsigned char *value;
  unsigned long length;
  const unsigned char *name;
  unsigned long name_length;
};

struct COM_STMT_EXECUTE_DATA {
  unsigned long stmt_id;
  /** This holds the flags, as defined in @ref enum_cursor_type */
  unsigned long open_cursor;
  PS_PARAM *parameters;
  unsigned long parameter_count;
  unsigned char has_new_types;
};

struct COM_STMT_FETCH_DATA {
  unsigned long stmt_id;
  unsigned long num_rows;
};

struct COM_STMT_SEND_LONG_DATA_DATA {
  unsigned long stmt_id;
  unsigned int param_number;
  unsigned char *longdata;
  unsigned long length;
};

struct COM_STMT_PREPARE_DATA {
  const char *query;
  unsigned int length;
};

struct COM_STMT_CLOSE_DATA {
  unsigned int stmt_id;
};

struct COM_STMT_RESET_DATA {
  unsigned int stmt_id;
};

struct COM_QUERY_DATA {
  const char *query;
  unsigned int length;
  PS_PARAM *parameters;
  unsigned long parameter_count;
};

struct COM_FIELD_LIST_DATA {
  unsigned char *table_name;
  unsigned int table_name_length;
  const unsigned char *query;
  unsigned int query_length;
};

union COM_DATA {
  COM_INIT_DB_DATA com_init_db;
  COM_SET_OPTION_DATA com_set_option;
  COM_STMT_EXECUTE_DATA com_stmt_execute;
  COM_STMT_FETCH_DATA com_stmt_fetch;
  COM_STMT_SEND_LONG_DATA_DATA com_stmt_send_long_data;
  COM_STMT_PREPARE_DATA com_stmt_prepare;
  COM_STMT_CLOSE_DATA com_stmt_close;
  COM_STMT_RESET_DATA com_stmt_reset;
  COM_QUERY_DATA com_query;
  COM_FIELD_LIST_DATA com_field_list;
};

#endif /* PLUGIN_PROTOCOL_INCLUDED */
