/* Copyright (c) 2017, 2025, Oracle and/or its affiliates.

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

#define LOG_COMPONENT_TAG "pfs_example_plugin_employee"

#include <mysql/plugin.h>
#include <mysql_version.h>

#include <mysql/components/my_service.h>
#include <mysql/components/services/log_builtins.h>
#include <mysqld_error.h>

#include "mysql/psi/mysql_mutex.h"

#include "plugin/pfs_table_plugin/pfs_example_employee_name.h"
#include "plugin/pfs_table_plugin/pfs_example_employee_salary.h"
#include "plugin/pfs_table_plugin/pfs_example_machine.h"
#include "plugin/pfs_table_plugin/pfs_example_machines_by_emp_by_mtype.h"

static SERVICE_TYPE(registry) *reg_srv = nullptr;
SERVICE_TYPE(log_builtins) *log_bi = nullptr;
SERVICE_TYPE(log_builtins_string) *log_bs = nullptr;

/**
  @page EXAMPLE_PLUGIN An example plugin

  Plugin Name     : pfs_example_plugin_employee \n
  Source location : plugin/pfs_table_plugin

  This file contains a definition of the pfs_example_plugin_employee.
*/

/* clang-format off */
/* Records to be inserted into pfs_example_employee_name table from plugin code */
Ename_Record ename_array[] =
{
  {{1, false}, "foo1", 4, "bar1", 4, true},
  {{2, false}, "foo2", 4, "bar2", 4, true},
  {{3, false}, "foo3", 4, "bar3", 4, true}
};

/* Records to be inserted into pfs_example_employee_salary table from plugin code */
Esalary_Record esalary_array[] =
{
  {{1, false}, {1000, false}, "2013-11-12", 10, "12:02:34", 8, true},
  {{2, false}, {2000, false}, "2016-02-29", 10, "12:12:30", 8, true},
  {{3, false}, {3000, false}, "2017-03-24", 10, "11:12:50", 8, true}
};

/* Records to be inserted into pfs_example_machine table from plugin code */
Machine_Record machine_array[] =
{
  {{1, false}, {DESKTOP, false}, "Lenovo", 6, {1, false}, true},
  {{2, false}, {LAPTOP, false}, "Dell", 4, {2, false}, true},
  {{3, false}, {MOBILE, false}, "Apple", 5, {1, false}, true},
  {{4, false}, {MOBILE, false}, "Samsung", 7, {1, false}, true},
  {{5, false}, {LAPTOP, false}, "Lenovo", 6, {2, false}, true},
  {{6, false}, {MOBILE, false}, "Nokia", 5, {2, false}, true},
  {{7, false}, {LAPTOP, false}, "Apple", 5, {1, false}, true},
  {{8, false}, {LAPTOP, false}, "HP", 2, {3, false}, true},
  {{9, false}, {DESKTOP, false}, "Apple", 5, {3, false}, true},
};
/* clang-format off */

/* Global handles */
SERVICE_TYPE(registry) *r = nullptr;
my_h_service h_ret_table_svc = nullptr;
SERVICE_TYPE(pfs_plugin_table_v1) *table_svc = nullptr;
my_h_service h_ret_col_int_svc = nullptr;
SERVICE_TYPE(pfs_plugin_column_integer_v1) * col_int_svc = nullptr;
my_h_service h_ret_col_string_svc = nullptr;
SERVICE_TYPE(pfs_plugin_column_string_v2) * col_string_svc = nullptr;
my_h_service h_ret_col_bigint_svc = nullptr;
SERVICE_TYPE(pfs_plugin_column_bigint_v1) * col_bigint_svc = nullptr;
my_h_service h_ret_col_date_svc = nullptr;
SERVICE_TYPE(pfs_plugin_column_date_v1) * col_date_svc = nullptr;
my_h_service h_ret_col_time_svc = nullptr;
SERVICE_TYPE(pfs_plugin_column_time_v1) * col_time_svc = nullptr;
my_h_service h_ret_col_enum_svc = nullptr;
SERVICE_TYPE(pfs_plugin_column_enum_v1) * col_enum_svc = nullptr;

/* Collection of table shares to be added to performance schema */
PFS_engine_table_share_proxy* share_list[4]= {nullptr, nullptr, nullptr, nullptr};
unsigned int share_list_count= 4;

/* Mutex info definitions for the table mutexes */
static PSI_mutex_key key_mutex_name;
static PSI_mutex_key key_mutex_salary;
static PSI_mutex_key key_mutex_machine;

static PSI_mutex_info mutex_info[] = {
  {&key_mutex_name, "LOCK_ename_records_array",
   PSI_FLAG_SINGLETON, PSI_VOLATILITY_PERMANENT,
   "Mutex for the pfs_example_employee_name table."},

  {&key_mutex_salary, "LOCK_esalary_records_array",
   0, PSI_VOLATILITY_PERMANENT,
   "Mutex for the pfs_example_employee_salary table."},

  {&key_mutex_machine, "LOCK_machine_records_array",
   0, PSI_VOLATILITY_PERMANENT,
   "Mutex for the pfs_example_machine table."}
};

/**
* acquire_service_handles does following:
*   - Acquire the registry service for mysql_server.
*   - Acquire pfs_plugin_table_v1 service implementation.
*/
bool
acquire_service_handles(MYSQL_PLUGIN p [[maybe_unused]])
{
  bool result = false;

  /* Acquire mysql_server's registry service */
  r = mysql_plugin_registry_acquire();
  if (!r)
  {
    LogPluginErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                 "mysql_plugin_registry_acquire() returns empty");
    result = true;
    goto error;
  }

  /* Acquire pfs_plugin_table_v1 service */
  if (r->acquire("pfs_plugin_table_v1", &h_ret_table_svc))
  {
    LogPluginErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                 "can't find pfs_plugin_table_v1 service");
    result = true;
    goto error;
  }

  /* Type cast this handler to proper service handle */
  table_svc =
    reinterpret_cast<SERVICE_TYPE(pfs_plugin_table_v1) *>(h_ret_table_svc);

  /* Acquire pfs_plugin_column_integer_v1 service */
  if (r->acquire("pfs_plugin_column_integer_v1", &h_ret_col_int_svc))
  {
    LogPluginErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                 "can't find pfs_plugin_column_integer_v1 service");
    result = true;
    goto error;
  }

  /* Type cast this handler to proper service handle */
  col_int_svc =
    reinterpret_cast<SERVICE_TYPE(pfs_plugin_column_integer_v1) *>(h_ret_col_int_svc);

  /* Acquire pfs_plugin_column_string_v2 service */
  if (r->acquire("pfs_plugin_column_string_v2", &h_ret_col_string_svc))
  {
    LogPluginErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                 "can't find pfs_plugin_column_string_v2 service");
    result = true;
    goto error;
  }

  /* Type cast this handler to proper service handle */
  col_string_svc =
    reinterpret_cast<SERVICE_TYPE(pfs_plugin_column_string_v2) *>(h_ret_col_string_svc);

  /* Acquire pfs_plugin_column_bigint_v1 service */
  if (r->acquire("pfs_plugin_column_bigint_v1", &h_ret_col_bigint_svc))
  {
    LogPluginErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                 "can't find pfs_plugin_column_bigint_v1 service");
    result = true;
    goto error;
  }

  /* Type cast this handler to proper service handle */
  col_bigint_svc =
    reinterpret_cast<SERVICE_TYPE(pfs_plugin_column_bigint_v1) *>(h_ret_col_bigint_svc);

  /* Acquire pfs_plugin_column_date_v1 service */
  if (r->acquire("pfs_plugin_column_date_v1", &h_ret_col_date_svc))
  {
    LogPluginErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                 "can't find pfs_plugin_column_date_v1 service");
    result = true;
    goto error;
  }

  /* Type cast this handler to proper service handle */
  col_date_svc =
    reinterpret_cast<SERVICE_TYPE(pfs_plugin_column_date_v1) *>(h_ret_col_date_svc);

  /* Acquire pfs_plugin_column_time_v1 service */
  if (r->acquire("pfs_plugin_column_time_v1", &h_ret_col_time_svc))
  {
    LogPluginErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                 "can't find pfs_plugin_column_time_v1 service");
    result = true;
    goto error;
  }

  /* Type cast this handler to proper service handle */
  col_time_svc =
    reinterpret_cast<SERVICE_TYPE(pfs_plugin_column_time_v1) *>(h_ret_col_time_svc);

  /* Acquire pfs_plugin_column_enum_v1 service */
  if (r->acquire("pfs_plugin_column_enum_v1", &h_ret_col_enum_svc))
  {
    LogPluginErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                 "can't find pfs_plugin_column_enum_v1 service");
    result = true;
    goto error;
  }

  /* Type cast this handler to proper service handle */
  col_enum_svc =
    reinterpret_cast<SERVICE_TYPE(pfs_plugin_column_enum_v1) *>(h_ret_col_enum_svc);

error:
  return result;
}

/**
* release_service_handles does following:
*   - Release the handle to the pfs_plugin_table_v1 service.
*   - Release the handle to registry service.
*/
void
release_service_handles()
{
  if (r != nullptr)
  {
    if (h_ret_table_svc != nullptr)
    {
      /* Release pfs_plugin_table_v1 services */
      r->release(h_ret_table_svc);
      h_ret_table_svc = nullptr;
      table_svc = nullptr;
    }

    if (h_ret_col_int_svc != nullptr)
    {
      /* Release pfs_plugin_column_integer_v1 services */
      r->release(h_ret_col_int_svc);
      h_ret_col_int_svc = nullptr;
      col_int_svc = nullptr;
    }

    if (h_ret_col_string_svc != nullptr)
    {
      /* Release pfs_plugin_column_string_v2 services */
      r->release(h_ret_col_string_svc);
      h_ret_col_string_svc = nullptr;
      col_string_svc = nullptr;
    }

    if (h_ret_col_bigint_svc != nullptr)
    {
      /* Release pfs_plugin_column_bigint_v1 services */
      r->release(h_ret_col_bigint_svc);
      h_ret_col_bigint_svc = nullptr;
      col_bigint_svc = nullptr;
    }

    if (h_ret_col_date_svc != nullptr)
    {
      /* Release pfs_plugin_column_date_v1 services */
      r->release(h_ret_col_date_svc);
      h_ret_col_date_svc = nullptr;
      col_date_svc = nullptr;
    }

    if (h_ret_col_time_svc != nullptr)
    {
      /* Release pfs_plugin_column_time_v1 services */
      r->release(h_ret_col_time_svc);
      h_ret_col_time_svc = nullptr;
      col_time_svc = nullptr;
    }

    if (h_ret_col_enum_svc != nullptr)
    {
      /* Release pfs_plugin_column_enum_v1 services */
      r->release(h_ret_col_enum_svc);
      h_ret_col_enum_svc = nullptr;
      col_enum_svc = nullptr;
    }

    /* Release registry service */
    mysql_plugin_registry_release(r);
    r = nullptr;
  }
}

/* Prepare and insert rows in pfs_example_employee_name table */
int
ename_prepare_insert_row()
{
  int result = 0;
  Ename_Table_Handle handle;

  for (auto & i : ename_array)
  {
    strncpy(handle.current_row.f_name, i.f_name,
        i.f_name_length);
    handle.current_row.f_name_length = i.f_name_length;
    strncpy(handle.current_row.l_name, i.l_name,
        i.l_name_length);
    handle.current_row.l_name_length = i.l_name_length;
    handle.current_row.e_number = i.e_number;
    handle.current_row.m_exist = i.m_exist;

    /* Insert a row in the table to be added */
    result = ename_write_row_values((PSI_table_handle *)&handle);

    if (result)
      break;
  }

  return result;
}

/* Prepare and insert rows in pfs_example_employee_salary table */
int
esalary_prepare_insert_row()
{
  int result = 0;
  Esalary_Table_Handle handle;

  for (auto & i : esalary_array)
  {
    strncpy(handle.current_row.e_dob, i.e_dob,
        i.e_dob_length);
    handle.current_row.e_dob_length= i.e_dob_length;

    strncpy(handle.current_row.e_tob, i.e_tob,
        i.e_tob_length);
    handle.current_row.e_tob_length= i.e_tob_length;

    handle.current_row.e_number = i.e_number;
    handle.current_row.e_salary = i.e_salary;

    handle.current_row.m_exist = i.m_exist;

    /* Insert a row in the table to be added */
    result = esalary_write_row_values((PSI_table_handle *)&handle);
    if (result)
      break;
  }

  return result;
}

/* Prepare and insert rows in pfs_example_machine table */
int
machine_prepare_insert_row()
{
  int result = 0;
  Machine_Table_Handle handle;

  for (auto & i : machine_array)
  {
    handle.current_row.machine_number = i.machine_number;
    strncpy(handle.current_row.machine_made, i.machine_made,
      i.machine_made_length);
    handle.current_row.machine_made_length = i.machine_made_length;
    handle.current_row.machine_type = i.machine_type;
    handle.current_row.employee_number = i.employee_number;

    handle.current_row.m_exist = i.m_exist;

    /* Insert a row in the table to be added */
    result = machine_write_row_values((PSI_table_handle *)&handle);
    if (result)
      break;
  }

  return result;
}

/**
*  pfs_example_func does following :
*    - Instantiate PFS_engine_table_share_proxy(s).
*    - Prepare and insert rows in tables from here.
*    - Acquire pfs_plugin_table_v1 service handle.
*    - Call add_table method of pfs_plugin_table_v1 service.

*  Error messages are written to the server's error log.
*  In case of success writes a single information message to the server's log.

*  @retval false  success
*  @retval true   failure
*/
static bool
pfs_example_func(MYSQL_PLUGIN p)
{
  bool result = false;

  /* Instantiate and initialize PFS_engine_table_share_proxy */
  init_ename_share(&ename_st_share);
  init_esalary_share(&esalary_st_share);
  init_machine_share(&machine_st_share);
  init_m_by_emp_by_mtype_share(&m_by_emp_by_mtype_st_share);

  /* From here, prepare rows for tables and insert */
  if (ename_prepare_insert_row() ||
      esalary_prepare_insert_row() ||
      machine_prepare_insert_row())
  {
    LogPluginErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                 "Error returned during prepare and insert row.");
    result = true;
    goto error;
  }

  /* Get pfs_plugin_table_v1 service handle. */
  result = acquire_service_handles(p);
  if (result)
    goto error;

  /* Prepare the shares list to be passed to the service call */
  share_list[0]= &ename_st_share;
  share_list[1]= &esalary_st_share;
  share_list[2]= &machine_st_share;
  share_list[3]= &m_by_emp_by_mtype_st_share;

  /**
   * Call add_tables function of pfs_plugin_table_v1 service to
   * add plugin tables in performance schema.
   */
  if (table_svc->add_tables(&share_list[0], share_list_count))
  {
    LogPluginErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                 "Error returned from add_tables()");
    result = true;
    goto error;
  }

  return result;

error:
  /* Release service handles. */
  release_service_handles();
  return result;
}

/**
*  Initialize the pfs_example_plugin_employee at server start or plugin
*  installation.
*
*   - Call pfs_example_func.
*/

static int
pfs_example_plugin_employee_init(void *p)
{
  DBUG_TRACE;
  int result = 0;

  if (init_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs))
    return 1;

  /* Register the mutex classes */
  mysql_mutex_register("pfs_example2", mutex_info, 3);

  /* Initialize mutexes to be used for table records */
  mysql_mutex_init(key_mutex_name, &LOCK_ename_records_array,
                   MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_mutex_salary, &LOCK_esalary_records_array,
                   MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_mutex_machine, &LOCK_machine_records_array,
                   MY_MUTEX_INIT_FAST);

  /* In case the plugin has been unloaded, and reloaded */
  ename_delete_all_rows();
  esalary_delete_all_rows();
  machine_delete_all_rows();

  result = pfs_example_func(reinterpret_cast<MYSQL_PLUGIN>(p)) ? 1 : 0;

  if (result)
  {
    /* Destroy mutexes for table records */
    mysql_mutex_destroy(&LOCK_ename_records_array);
    mysql_mutex_destroy(&LOCK_esalary_records_array);
    mysql_mutex_destroy(&LOCK_machine_records_array);
  }

  return result;
}

static int
pfs_example_plugin_employee_check(void *)
{
  DBUG_TRACE;

  if (table_svc != nullptr)
  {
    if (table_svc->delete_tables(&share_list[0], share_list_count))
    {
      /* Block execution of UNINSTALL PLUGIN. */
      return 1;
    }
  }

  return 0;
}

/**
*  Terminate the pfs_example_plugin_employee at server shutdown or plugin
*  deinstallation.
*
*   - Delete/Drop plugin tables from Performance Schema.
*   - Release pfs_plugin_table_v1 service handle.
*/
static int
pfs_example_plugin_employee_deinit(void *p  [[maybe_unused]])
{
  DBUG_TRACE;

  /**
   * Call delete_tables function of pfs_plugin_table_v1 service to
   * delete plugin tables from performance schema
   */
  if (table_svc != nullptr)
  {
    if (table_svc->delete_tables(&share_list[0], share_list_count))
    {
      LogPluginErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                   "Error returned from delete_tables()");
      deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
      return 1;
    }
  }
  else /* Service not found or released */
  {
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
    return 1;
  }

  /* Destroy mutexes for table records */
  mysql_mutex_destroy(&LOCK_ename_records_array);
  mysql_mutex_destroy(&LOCK_esalary_records_array);
  mysql_mutex_destroy(&LOCK_machine_records_array);

  deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);

  /* Release service handles. */
  release_service_handles();

  return 0;
}

static struct st_mysql_daemon pfs_example_plugin_employee = {
  MYSQL_DAEMON_INTERFACE_VERSION};

/**
  pfs_example_plugin_employee plugin descriptor
*/

/* clang-format off */
mysql_declare_plugin(pfs_example_plugin_employee)
{
  MYSQL_DAEMON_PLUGIN,
  &pfs_example_plugin_employee,
  "pfs_example_plugin_employee",
  PLUGIN_AUTHOR_ORACLE,
  "pfs_example_plugin_employee",
  PLUGIN_LICENSE_GPL,
  pfs_example_plugin_employee_init,   /* Plugin Init      */
  pfs_example_plugin_employee_check,  /* Plugin Check uninstall */
  pfs_example_plugin_employee_deinit, /* Plugin Deinit    */
  0x0100 /* 1.0 */, nullptr,             /* status variables */
  nullptr,                               /* system variables */
  nullptr,                               /* config options   */
  PLUGIN_OPT_ALLOW_EARLY,             /* flags            */
}
mysql_declare_plugin_end;
/* clang-format on */
