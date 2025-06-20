/* Copyright (c) 2006, 2025, Oracle and/or its affiliates.

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

#include <fcntl.h>
#include <mysql/plugin.h>
#include <mysql_version.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "m_string.h"  // strlen
#include "my_dbug.h"
#include "my_dir.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_psi_config.h"
#include "my_sys.h"  // my_write, my_malloc
#include "my_thread.h"
#include "mysql/psi/mysql_memory.h"
#include "sql/sql_plugin.h"  // st_plugin_int
#include "template_utils.h"

PSI_memory_key key_memory_mysql_heartbeat_context;

#ifdef HAVE_PSI_INTERFACE

static PSI_memory_info all_deamon_example_memory[] = {
    {&key_memory_mysql_heartbeat_context, "mysql_heartbeat_context", 0, 0,
     PSI_DOCUMENT_ME}};

static void init_deamon_example_psi_keys() {
  const char *category = "deamon_example";
  int count;

  count = static_cast<int>(array_elements(all_deamon_example_memory));
  mysql_memory_register(category, all_deamon_example_memory, count);
}
#endif /* HAVE_PSI_INTERFACE */

#define HEART_STRING_BUFFER 100

struct mysql_heartbeat_context {
  my_thread_handle heartbeat_thread;
  File heartbeat_file;
};

static void *mysql_heartbeat(void *p) {
  DBUG_TRACE;
  auto *con = (struct mysql_heartbeat_context *)p;
  char buffer[HEART_STRING_BUFFER];
  time_t result;
  struct tm tm_tmp;

  while (true) {
    sleep(5);

    result = time(nullptr);
    localtime_r(&result, &tm_tmp);
    snprintf(buffer, sizeof(buffer),
             "Heartbeat at %02d%02d%02d %2d:%02d:%02d\n", tm_tmp.tm_year % 100,
             tm_tmp.tm_mon + 1, tm_tmp.tm_mday, tm_tmp.tm_hour, tm_tmp.tm_min,
             tm_tmp.tm_sec);
    my_write(con->heartbeat_file, (uchar *)buffer, strlen(buffer), MYF(0));
  }

  return nullptr;
}

/*
  Initialize the daemon example at server start or plugin installation.

  SYNOPSIS
    daemon_example_plugin_init()

  DESCRIPTION
    Starts up heartbeatbeat thread

  RETURN VALUE
    0                    success
    1                    failure (cannot happen)
*/

static int daemon_example_plugin_init(void *p) {
  DBUG_TRACE;

#ifdef HAVE_PSI_INTERFACE
  init_deamon_example_psi_keys();
#endif

  struct mysql_heartbeat_context *con;
  my_thread_attr_t attr; /* Thread attributes */
  char heartbeat_filename[FN_REFLEN];
  char buffer[HEART_STRING_BUFFER];
  const time_t result = time(nullptr);
  struct tm tm_tmp;

  auto *plugin = (struct st_plugin_int *)p;

  con = (struct mysql_heartbeat_context *)my_malloc(
      key_memory_mysql_heartbeat_context,
      sizeof(struct mysql_heartbeat_context), MYF(0));

  fn_format(heartbeat_filename, "mysql-heartbeat", "", ".log",
            MY_REPLACE_EXT | MY_UNPACK_FILENAME);
  unlink(heartbeat_filename);
  con->heartbeat_file = my_open(heartbeat_filename, O_CREAT | O_RDWR, MYF(0));

  /*
    No threads exist at this point in time, so this is thread safe.
  */
  localtime_r(&result, &tm_tmp);
  snprintf(buffer, sizeof(buffer),
           "Starting up at %02d%02d%02d %2d:%02d:%02d\n", tm_tmp.tm_year % 100,
           tm_tmp.tm_mon + 1, tm_tmp.tm_mday, tm_tmp.tm_hour, tm_tmp.tm_min,
           tm_tmp.tm_sec);
  my_write(con->heartbeat_file, (uchar *)buffer, strlen(buffer), MYF(0));

  my_thread_attr_init(&attr);
  my_thread_attr_setdetachstate(&attr, MY_THREAD_CREATE_JOINABLE);

  /* now create the thread */
  if (my_thread_create(&con->heartbeat_thread, &attr, mysql_heartbeat,
                       (void *)con) != 0) {
    fprintf(stderr, "Could not create heartbeat thread!\n");
    exit(0);
  }
  plugin->data = (void *)con;

  return 0;
}

/*
  Terminate the daemon example at server shutdown or plugin deinstallation.

  SYNOPSIS
    daemon_example_plugin_deinit()
    Does nothing.

  RETURN VALUE
    0                    success
    1                    failure (cannot happen)

*/

static int daemon_example_plugin_deinit(void *p) {
  DBUG_TRACE;
  char buffer[HEART_STRING_BUFFER];
  auto *plugin = (struct st_plugin_int *)p;
  auto *con = (struct mysql_heartbeat_context *)plugin->data;
  const time_t result = time(nullptr);
  struct tm tm_tmp;
  void *dummy_retval;

  my_thread_cancel(&con->heartbeat_thread);

  localtime_r(&result, &tm_tmp);
  snprintf(buffer, sizeof(buffer),
           "Shutting down at %02d%02d%02d %2d:%02d:%02d\n",
           tm_tmp.tm_year % 100, tm_tmp.tm_mon + 1, tm_tmp.tm_mday,
           tm_tmp.tm_hour, tm_tmp.tm_min, tm_tmp.tm_sec);
  my_write(con->heartbeat_file, (uchar *)buffer, strlen(buffer), MYF(0));

  /*
    Need to wait for the hearbeat thread to terminate before closing
    the file it writes to and freeing the memory it uses
  */
  my_thread_join(&con->heartbeat_thread, &dummy_retval);

  my_close(con->heartbeat_file, MYF(0));

  my_free(con);

  return 0;
}

struct st_mysql_daemon daemon_example_plugin = {MYSQL_DAEMON_INTERFACE_VERSION};

/*
  Plugin library descriptor
*/

mysql_declare_plugin(daemon_example){
    MYSQL_DAEMON_PLUGIN,
    &daemon_example_plugin,
    "daemon_example",
    PLUGIN_AUTHOR_ORACLE,
    "Daemon example, creates a heartbeat beat file in mysql-heartbeat.log",
    PLUGIN_LICENSE_GPL,
    daemon_example_plugin_init,   /* Plugin Init */
    nullptr,                      /* Plugin Check uninstall */
    daemon_example_plugin_deinit, /* Plugin Deinit */
    0x0100 /* 1.0 */,
    nullptr, /* status variables                */
    nullptr, /* system variables                */
    nullptr, /* config options                  */
    0,       /* flags                           */
} mysql_declare_plugin_end;
