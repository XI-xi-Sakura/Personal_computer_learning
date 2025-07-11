/* Copyright (c) 2003, 2025, Oracle and/or its affiliates.

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

/*
  This file is included by both libmysql.c (the MySQL client C API)
  and the mysqld server to connect to another MYSQL server.

  The differences for the two cases are:

  - Things that only works for the client:
  - Trying to automatically determine user name if not supplied to
    mysql_real_connect()
  - Support for reading local file with LOAD DATA LOCAL
  - SHARED memory handling
  - Prepared statements
  - Things that only works for the server

  In all other cases, code should be identical for the client and
  server.
*/

#include <openssl/opensslv.h>
#include <stdarg.h>
#include <sys/types.h>

#include "m_string.h"
#include "my_config.h"
#include "my_sys.h"
#include "mysql/strings/m_ctype.h"
#include "mysys_err.h"
#ifndef _WIN32
#include <netdb.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#include <stdio.h>

#include <algorithm>
#include <fstream>
#include <ios>
#include <iostream>
#include <string>

#include "base64.h"
#include "client_async_authentication.h"
#include "compression.h"  // validate_compression_attributes
#include "errmsg.h"
#include "lex_string.h"
#include "map_helpers.h"
#include "my_byteorder.h"
#include "my_compiler.h"
#include "my_config.h"
#include "my_dbug.h"
#include "my_default.h"
#include "my_inttypes.h"
#include "my_io.h"
#include "my_macros.h"
#include "my_openssl_fips.h"  // OPENSSL_ERROR_LENGTH, set_fips_mode
#include "my_psi_config.h"
#include "my_shm_defaults.h"
#include "mysql.h"
#include "mysql/client_authentication.h"
#include "mysql/my_loglevel.h"
#include "mysql/plugin_auth_common.h"
#include "mysql/psi/mysql_memory.h"
#include "mysql/service_mysql_alloc.h"
#include "mysql/strings/int2str.h"
#include "mysql_version.h"
#include "mysqld_error.h"
#include "strmake.h"
#include "strxmov.h"
#include "strxnmov.h"
#include "template_utils.h"
#include "typelib.h"
#include "violite.h"

#if !defined(_WIN32)
#include "my_thread.h" /* because of signal()*/
#endif                 /* !defined(_WIN32) */

#include <signal.h>
#include <sys/stat.h>
#include <time.h>

#ifdef HAVE_PWD_H
#include <pwd.h>
#endif

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#ifndef _WIN32
#include <errno.h>

#define INVALID_SOCKET -1
#endif

#include <mysql/client_plugin.h>
#include <openssl/x509v3.h>

#include <new>

#include "../libmysql/init_commands_array.h"
#include "../libmysql/mysql_trace.h" /* MYSQL_TRACE() instrumentation */
#include "sql_common.h"
#ifdef MYSQL_SERVER
#include "mysql_com_server.h"
#include "sql/client_settings.h"
#include "sql/server_component/mysql_command_services_imp.h"
/* mysql_command_service_extn */
#else
#include "libmysql/client_settings.h"
#endif
#include "client_extensions_macros.h"
#include "sql/log_event.h"     /* Log_event_type */
#include "sql/rpl_constants.h" /* mysql_binlog_XXX() */

using mysql::binlog::event::Log_event_type;
using std::string;
using std::swap;

#define STATE_DATA(M) \
  (nullptr != (M) ? &(MYSQL_EXTENSION_PTR(M)->state_change) : nullptr)

#define ADD_INFO(M, element, type)                       \
  {                                                      \
    M = STATE_DATA(mysql);                               \
    M->info_list[type].head_node =                       \
        list_add(M->info_list[type].head_node, element); \
  }

#define caching_sha2_password_plugin_name "caching_sha2_password"
#define MYSQL_NATIVE_PASSWORD_PLUGIN_NAME "mysql_native_password"

PSI_memory_key key_memory_mysql_options;
PSI_memory_key key_memory_MYSQL_DATA;
PSI_memory_key key_memory_MYSQL;
PSI_memory_key key_memory_MYSQL_RES;
PSI_memory_key key_memory_MYSQL_ROW;
PSI_memory_key key_memory_MYSQL_state_change_info;
PSI_memory_key key_memory_MYSQL_HANDSHAKE;
PSI_memory_key key_memory_MYSQL_ssl_session_data;

#if defined(_WIN32)
PSI_memory_key key_memory_create_shared_memory;
#endif /* _WIN32 */

#ifdef HAVE_PSI_INTERFACE
/*
  This code is common to the client and server,
  and also used in the server when server A connects to server B,
  for example with replication.
  Therefore, the code is also instrumented.
*/

static PSI_memory_info all_client_memory[] = {
#if defined(_WIN32)
    {&key_memory_create_shared_memory, "create_shared_memory", 0, 0,
     PSI_DOCUMENT_ME},
#endif /* _WIN32 */

    {&key_memory_mysql_options, "mysql_options", 0, 0, PSI_DOCUMENT_ME},
    {&key_memory_MYSQL_DATA, "MYSQL_DATA", 0, 0, PSI_DOCUMENT_ME},
    {&key_memory_MYSQL, "MYSQL", 0, 0, PSI_DOCUMENT_ME},
    {&key_memory_MYSQL_RES, "MYSQL_RES", 0, 0, PSI_DOCUMENT_ME},
    {&key_memory_MYSQL_ROW, "MYSQL_ROW", 0, 0, PSI_DOCUMENT_ME},
    {&key_memory_MYSQL_state_change_info, "MYSQL_STATE_CHANGE_INFO", 0, 0,
     PSI_DOCUMENT_ME},
    {&key_memory_MYSQL_HANDSHAKE, "MYSQL_HANDSHAKE", 0, 0, PSI_DOCUMENT_ME},
    {&key_memory_MYSQL_ssl_session_data, "MYSQL_SSL_session", 0, 0,
     "Saved SSL sessions"}};

void init_client_psi_keys(void) {
  const char *category = "client";
  int count;

  count = static_cast<int>(array_elements(all_client_memory));
  mysql_memory_register(category, all_client_memory, count);
}

#endif /* HAVE_PSI_INTERFACE */

/* SSL_SESSION_is_resumable is openssl 1.1.1+ */
#if OPENSSL_VERSION_NUMBER < 0x10101000L
#define SSL_SESSION_is_resumable(x) true
#endif

uint mysql_port = 0;
char *mysql_unix_port = nullptr;
const char *unknown_sqlstate = "HY000";
const char *not_error_sqlstate = "00000";
const char *cant_connect_sqlstate = "08001";
#if defined(_WIN32)
const char *def_shared_memory_base_name = default_shared_memory_base_name;
#endif
ulong g_net_buffer_length = 8192;
ulong g_max_allowed_packet = 1024L * 1024L * 1024L;

static void mysql_prune_stmt_list(MYSQL *mysql);
static int read_com_query_metadata(MYSQL *mysql, uchar *pos, ulong field_count);

CHARSET_INFO *default_client_charset_info = &my_charset_latin1;

/* Server error code and message */
unsigned int mysql_server_last_errno;
char mysql_server_last_error[MYSQL_ERRMSG_SIZE];
/* forward declaration */
static int read_one_row(MYSQL *mysql, uint fields, MYSQL_ROW row,
                        ulong *lengths);
static net_async_status read_one_row_nonblocking(MYSQL *mysql, uint fields,
                                                 MYSQL_ROW row, ulong *lengths,
                                                 int *res);
/**
 Free async_qp_data buffer in async_context.

 @param async_context pointer to asynchronous context structure
*/
void inline free_async_qp_data(MYSQL_ASYNC *async_context) {
  if (async_context->async_qp_data) {
    my_free(async_context->async_qp_data);
    async_context->async_qp_data = nullptr;
    async_context->async_qp_data_length = 0;
  }
}

/**
 Set async_context to idle status, free async_qp_data buffer.

 @param async_context pointer to asynchronous context structure
*/
void inline set_query_idle(MYSQL_ASYNC *async_context) {
  async_context->async_op_status = ASYNC_OP_UNSET;
  async_context->async_query_state = QUERY_IDLE;
  async_context->async_query_length = 0;
  DBUG_PRINT("async", ("set state=%d", async_context->async_query_state));
  free_async_qp_data(async_context);
}

/**
  Convert the connect timeout option to a timeout value for VIO
  functions (vio_socket_connect() and vio_io_wait()).

  @param mysql  Connection handle (client side).

  @return The timeout value in milliseconds, or -1 if no timeout.
*/

static int get_vio_connect_timeout(MYSQL *mysql) {
  int timeout_ms;
  uint timeout_sec;

  /*
    A timeout of 0 means no timeout. Also, the connect_timeout
    option value is in seconds, while VIO timeouts are measured
    in milliseconds. Hence, check for a possible overflow. In
    case of overflow, set to no timeout.
  */
  timeout_sec = mysql->options.connect_timeout;

  if (!timeout_sec || (timeout_sec > INT_MAX / 1000))
    timeout_ms = -1;
  else
    timeout_ms = (int)(timeout_sec * 1000);

  return timeout_ms;
}

#ifdef _WIN32

/**
  Convert the connect timeout option to a timeout value for WIN32
  synchronization functions.

  @remark Specific for WIN32 connection methods shared memory and
          named pipe.

  @param mysql  Connection handle (client side).

  @return The timeout value in milliseconds, or INFINITE if no timeout.
*/

static DWORD get_win32_connect_timeout(MYSQL *mysql) {
  DWORD timeout_ms;
  uint timeout_sec;

  /*
    A timeout of 0 means no timeout. Also, the connect_timeout
    option value is in seconds, while WIN32 timeouts are in
    milliseconds. Hence, check for a possible overflow. In case
    of overflow, set to no timeout.
  */
  timeout_sec = mysql->options.connect_timeout;

  if (!timeout_sec || (timeout_sec > INT_MAX / 1000))
    timeout_ms = INFINITE;
  else
    timeout_ms = (DWORD)(timeout_sec * 1000);

  return timeout_ms;
}

#endif

/**
  Set the internal error message to mysql handler

  @param mysql    connection handle (client side)
  @param errcode  CR_ error code, passed to ER macro to get
                  error text
  @param sqlstate SQL standard sqlstate
*/

void set_mysql_error(MYSQL *mysql, int errcode, const char *sqlstate) {
  NET *net;
  DBUG_TRACE;
  DBUG_PRINT("enter", ("error :%d '%s'", errcode, ER_CLIENT(errcode)));
  assert(mysql != nullptr);

  if (mysql) {
    net = &mysql->net;
    net->last_errno = errcode;
    my_stpcpy(net->last_error, ER_CLIENT(errcode));
    my_stpcpy(net->sqlstate, sqlstate);
    MYSQL_TRACE(ERROR, mysql, ());
  } else {
    mysql_server_last_errno = errcode;
    my_stpcpy(mysql_server_last_error, ER_CLIENT(errcode));
  }
}

/**
  Is this NET instance initialized?
  @c my_net_init() and net_end()
 */

static bool my_net_is_inited(NET *net) { return net->buff != nullptr; }

/**
  Clear possible error state of struct NET

  @param net  clear the state of the argument
*/

void net_clear_error(NET *net) {
  net->last_errno = 0;
  net->last_error[0] = '\0';
  my_stpcpy(net->sqlstate, not_error_sqlstate);
}

/**
  Set an error message on the client.

  @param mysql     connection handle
  @param errcode   CR_* errcode, for client errors
  @param sqlstate  SQL standard sql state, unknown_sqlstate for the
                   majority of client errors.
  @param format    error message template, in sprintf format
  @param ...       variable number of arguments
*/

void set_mysql_extended_error(MYSQL *mysql, int errcode, const char *sqlstate,
                              const char *format, ...) {
  NET *net;
  va_list args;
  DBUG_TRACE;
  DBUG_PRINT("enter", ("error :%d '%s'", errcode, format));
  assert(mysql != nullptr);

  net = &mysql->net;
  net->last_errno = errcode;
  va_start(args, format);
  vsnprintf(net->last_error, sizeof(net->last_error) - 1, format, args);
  va_end(args);
  my_stpcpy(net->sqlstate, sqlstate);

  MYSQL_TRACE(ERROR, mysql, ());
}

/*
  Create a named pipe connection
*/

#ifdef _WIN32

static HANDLE create_named_pipe(MYSQL *mysql, DWORD connect_timeout,
                                const char **arg_host,
                                const char **arg_unix_socket) {
  HANDLE hPipe = INVALID_HANDLE_VALUE;
  char pipe_name[1024];
  DWORD dwMode;
  int i;
  const char *host = *arg_host, *unix_socket = *arg_unix_socket;

  if (!unix_socket || (unix_socket)[0] == 0x00) unix_socket = mysql_unix_port;
  if (!host || !strcmp(host, LOCAL_HOST)) host = LOCAL_HOST_NAMEDPIPE;

  pipe_name[sizeof(pipe_name) - 1] = 0; /* Safety if too long string */
  strxnmov(pipe_name, sizeof(pipe_name) - 1, "\\\\.\\pipe\\", unix_socket,
           NullS);

  DBUG_PRINT("info", ("Server name: '%s'.  Named Pipe: %s", host, unix_socket));

  for (i = 0; i < 100; i++) /* Don't retry forever */
  {
    if ((hPipe = CreateFile(pipe_name,
                            FILE_READ_ATTRIBUTES | FILE_READ_DATA |
                                FILE_WRITE_ATTRIBUTES | FILE_WRITE_DATA,
                            0, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                                SECURITY_IDENTIFICATION,
                            nullptr)) != INVALID_HANDLE_VALUE)
      break;
    if (GetLastError() != ERROR_PIPE_BUSY) {
      set_mysql_extended_error(mysql, CR_NAMEDPIPEOPEN_ERROR, unknown_sqlstate,
                               ER_CLIENT(CR_NAMEDPIPEOPEN_ERROR), host,
                               unix_socket, (ulong)GetLastError());
      return INVALID_HANDLE_VALUE;
    }
    /* wait for for an other instance */
    if (!WaitNamedPipe(pipe_name, connect_timeout)) {
      set_mysql_extended_error(mysql, CR_NAMEDPIPEWAIT_ERROR, unknown_sqlstate,
                               ER_CLIENT(CR_NAMEDPIPEWAIT_ERROR), host,
                               unix_socket, (ulong)GetLastError());
      return INVALID_HANDLE_VALUE;
    }
  }
  if (hPipe == INVALID_HANDLE_VALUE) {
    set_mysql_extended_error(mysql, CR_NAMEDPIPEOPEN_ERROR, unknown_sqlstate,
                             ER_CLIENT(CR_NAMEDPIPEOPEN_ERROR), host,
                             unix_socket, (ulong)GetLastError());
    return INVALID_HANDLE_VALUE;
  }
  dwMode = PIPE_READMODE_BYTE | PIPE_WAIT;
  if (!SetNamedPipeHandleState(hPipe, &dwMode, nullptr, nullptr)) {
    CloseHandle(hPipe);
    set_mysql_extended_error(mysql, CR_NAMEDPIPESETSTATE_ERROR,
                             unknown_sqlstate,
                             ER_CLIENT(CR_NAMEDPIPESETSTATE_ERROR), host,
                             unix_socket, (ulong)GetLastError());
    return INVALID_HANDLE_VALUE;
  }
  *arg_host = host;
  *arg_unix_socket = unix_socket; /* connect arg */
  return (hPipe);
}
#endif

/*
  Create new shared memory connection, return handler of connection

  @param mysql  Pointer of mysql structure
  @param net    Pointer of net structure
  @param connect_timeout  Timeout of connection (in milliseconds)

  @return HANDLE to the shared memory area.
*/

#if defined(_WIN32)
static HANDLE create_shared_memory(MYSQL *mysql, NET *net,
                                   DWORD connect_timeout) {
  const ulong smem_buffer_length = shared_memory_buffer_length + 4;
  /*
    event_connect_request is event object for start connection actions
    event_connect_answer is event object for confirm, that server put data
    handle_connect_file_map is file-mapping object, use for create shared
    memory
    handle_connect_map is pointer on shared memory
    handle_map is pointer on shared memory for client
    event_server_wrote,
    event_server_read,
    event_client_wrote,
    event_client_read are events for transfer data between server and client
    handle_file_map is file-mapping object, use for create shared memory
  */
  HANDLE event_connect_request = nullptr;
  HANDLE event_connect_answer = nullptr;
  HANDLE handle_connect_file_map = nullptr;
  char *handle_connect_map = nullptr;

  char *handle_map = nullptr;
  HANDLE event_server_wrote = nullptr;
  HANDLE event_server_read = nullptr;
  HANDLE event_client_wrote = nullptr;
  HANDLE event_client_read = nullptr;
  HANDLE event_conn_closed = nullptr;
  HANDLE handle_file_map = nullptr;
  HANDLE connect_named_mutex = nullptr;
  ulong connect_number;
  char connect_number_char[22];
  char *tmp = nullptr;
  char *suffix_pos = nullptr;
  DWORD error_allow = 0;
  DWORD error_code = 0;
  const DWORD event_access_rights = SYNCHRONIZE | EVENT_MODIFY_STATE;
  char *shared_memory_base_name = mysql->options.shared_memory_base_name;
  static const char *name_prefixes[] = {"", "Global\\"};
  const char *prefix;

  /*
    If this is nullptr, somebody freed the MYSQL* options.  mysql_close()
    is a good candidate.  We don't just silently (re)set it to
    def_shared_memory_base_name as that would create really confusing/buggy
    behavior if the user passed in a different name on the command-line or
    in a my.cnf.
  */
  assert(shared_memory_base_name != nullptr);

  /*
     get enough space base-name + '_' + longest suffix we might ever send
   */
  if (!(tmp = (char *)my_malloc(key_memory_create_shared_memory,
                                strlen(shared_memory_base_name) + 32L,
                                MYF(MY_FAE))))
    goto err;

  /*
    The name of event and file-mapping events create agree next rule:
    shared_memory_base_name+unique_part
    Where:
    shared_memory_base_name is unique value for each server
    unique_part is uniquel value for each object (events and file-mapping)
  */
  for (size_t i = 0; i < array_elements(name_prefixes); i++) {
    prefix = name_prefixes[i];
    suffix_pos = strxmov(tmp, prefix, shared_memory_base_name, "_", NullS);
    my_stpcpy(suffix_pos, "CONNECT_REQUEST");
    event_connect_request = OpenEvent(event_access_rights, false, tmp);
    if (event_connect_request) {
      break;
    }
  }
  if (!event_connect_request) {
    error_allow = CR_SHARED_MEMORY_CONNECT_REQUEST_ERROR;
    goto err;
  }
  assert(suffix_pos != nullptr);
  my_stpcpy(suffix_pos, "CONNECT_ANSWER");
  if (!(event_connect_answer = OpenEvent(event_access_rights, false, tmp))) {
    error_allow = CR_SHARED_MEMORY_CONNECT_ANSWER_ERROR;
    goto err;
  }
  my_stpcpy(suffix_pos, "CONNECT_DATA");
  if (!(handle_connect_file_map =
            OpenFileMapping(FILE_MAP_WRITE, false, tmp))) {
    error_allow = CR_SHARED_MEMORY_CONNECT_FILE_MAP_ERROR;
    goto err;
  }
  if (!(handle_connect_map = static_cast<char *>(MapViewOfFile(
            handle_connect_file_map, FILE_MAP_WRITE, 0, 0, sizeof(DWORD))))) {
    error_allow = CR_SHARED_MEMORY_CONNECT_MAP_ERROR;
    goto err;
  }

  my_stpcpy(suffix_pos, "CONNECT_NAMED_MUTEX");
  connect_named_mutex = OpenMutex(SYNCHRONIZE, false, tmp);
  if (connect_named_mutex == nullptr) {
    error_allow = CR_SHARED_MEMORY_CONNECT_SET_ERROR;
    goto err;
  }

  if (WaitForSingleObject(connect_named_mutex, connect_timeout) !=
      WAIT_OBJECT_0) {
    error_allow = CR_SHARED_MEMORY_CONNECT_ABANDONED_ERROR;
    goto err;
  }

  /* Send to server request of connection */
  if (!SetEvent(event_connect_request)) {
    error_allow = CR_SHARED_MEMORY_CONNECT_SET_ERROR;
    goto err;
  }

  /* Wait of answer from server */
  if (WaitForSingleObject(event_connect_answer, connect_timeout) !=
      WAIT_OBJECT_0) {
    error_allow = CR_SHARED_MEMORY_CONNECT_ABANDONED_ERROR;
    goto err;
  }

  /* Get number of connection */
  connect_number = uint4korr(handle_connect_map); /*WAX2*/

  ReleaseMutex(connect_named_mutex);
  CloseHandle(connect_named_mutex);
  connect_named_mutex = nullptr;

  longlong10_to_str(connect_number, connect_number_char, 10);

  /*
    The name of event and file-mapping events create agree next rule:
    shared_memory_base_name+unique_part+number_of_connection

    Where:
    shared_memory_base_name is uniquel value for each server
    unique_part is uniquel value for each object (events and file-mapping)
    number_of_connection is number of connection between server and client
  */
  suffix_pos = strxmov(tmp, prefix, shared_memory_base_name, "_",
                       connect_number_char, "_", NullS);
  my_stpcpy(suffix_pos, "DATA");
  if ((handle_file_map = OpenFileMapping(FILE_MAP_WRITE, false, tmp)) ==
      nullptr) {
    error_allow = CR_SHARED_MEMORY_FILE_MAP_ERROR;
    goto err2;
  }
  if ((handle_map = static_cast<char *>(MapViewOfFile(
           handle_file_map, FILE_MAP_WRITE, 0, 0, smem_buffer_length))) ==
      nullptr) {
    error_allow = CR_SHARED_MEMORY_MAP_ERROR;
    goto err2;
  }

  my_stpcpy(suffix_pos, "SERVER_WROTE");
  if ((event_server_wrote = OpenEvent(event_access_rights, false, tmp)) ==
      nullptr) {
    error_allow = CR_SHARED_MEMORY_EVENT_ERROR;
    goto err2;
  }

  my_stpcpy(suffix_pos, "SERVER_READ");
  if ((event_server_read = OpenEvent(event_access_rights, false, tmp)) ==
      nullptr) {
    error_allow = CR_SHARED_MEMORY_EVENT_ERROR;
    goto err2;
  }

  my_stpcpy(suffix_pos, "CLIENT_WROTE");
  if ((event_client_wrote = OpenEvent(event_access_rights, false, tmp)) ==
      nullptr) {
    error_allow = CR_SHARED_MEMORY_EVENT_ERROR;
    goto err2;
  }

  my_stpcpy(suffix_pos, "CLIENT_READ");
  if ((event_client_read = OpenEvent(event_access_rights, false, tmp)) ==
      nullptr) {
    error_allow = CR_SHARED_MEMORY_EVENT_ERROR;
    goto err2;
  }

  my_stpcpy(suffix_pos, "CONNECTION_CLOSED");
  if ((event_conn_closed = OpenEvent(event_access_rights, false, tmp)) ==
      nullptr) {
    error_allow = CR_SHARED_MEMORY_EVENT_ERROR;
    goto err2;
  }
  /*
    Set event that server should send data
  */
  SetEvent(event_server_read);

err2:
  if (error_allow == 0) {
    net->vio = vio_new_win32shared_memory(
        handle_file_map, handle_map, event_server_wrote, event_server_read,
        event_client_wrote, event_client_read, event_conn_closed);
  } else {
    error_code = GetLastError();
    if (event_server_read) CloseHandle(event_server_read);
    if (event_server_wrote) CloseHandle(event_server_wrote);
    if (event_client_read) CloseHandle(event_client_read);
    if (event_client_wrote) CloseHandle(event_client_wrote);
    if (event_conn_closed) CloseHandle(event_conn_closed);
    if (handle_map) UnmapViewOfFile(handle_map);
    if (handle_file_map) CloseHandle(handle_file_map);
  }
err:
  my_free(tmp);
  if (error_allow) error_code = GetLastError();
  if (event_connect_request) CloseHandle(event_connect_request);
  if (event_connect_answer) CloseHandle(event_connect_answer);
  if (handle_connect_map) UnmapViewOfFile(handle_connect_map);
  if (handle_connect_file_map) CloseHandle(handle_connect_file_map);
  if (error_allow) {
    if (connect_named_mutex) {
      ReleaseMutex(connect_named_mutex);
      CloseHandle(connect_named_mutex);
    }

    if (error_allow == CR_SHARED_MEMORY_EVENT_ERROR)
      set_mysql_extended_error(mysql, error_allow, unknown_sqlstate,
                               ER_CLIENT(error_allow), suffix_pos, error_code);
    else
      set_mysql_extended_error(mysql, error_allow, unknown_sqlstate,
                               ER_CLIENT(error_allow), error_code);
    return (INVALID_HANDLE_VALUE);
  }
  return (handle_map);
}
#endif

/*
  Free all memory acquired to store state change information.
*/
static void free_state_change_info(MYSQL_EXTENSION *ext) {
  STATE_INFO *info;
  int i;

  if (ext)
    info = &ext->state_change;
  else
    return;

  for (i = SESSION_TRACK_SYSTEM_VARIABLES; i <= SESSION_TRACK_END; i++) {
    if (list_length(info->info_list[i].head_node) != 0) {
      list_free(info->info_list[i].head_node, (uint)0);
    }
  }
  memset(info, 0, sizeof(STATE_INFO));
}

/**
  Helper function to check if the buffer has at least bytes remaining

  If the buffer is too small it raises CR_MALFORMED_PACKET_ERROR.

  @param mysql the handle that has the buffer
  @param packet the current position in the buffer
  @param packet_length the size of the packet
  @param bytes the bytes that we want available
  @retval true the buffer has that many bytes
  @retval false the buffer has less bytes remaining
*/
inline bool buffer_check_remaining(MYSQL *mysql, uchar *packet,
                                   ulong packet_length, size_t bytes) {
  size_t remaining_bytes;
  /* Check to avoid underflow */
  if (packet_length < (ulong)(packet - mysql->net.read_pos)) {
    set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
    return false;
  }
  remaining_bytes = packet_length - (packet - mysql->net.read_pos);
  if (remaining_bytes < bytes) {
    set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
    return false;
  }
  return true;
}

/*
  Helper function to safely read a variable size from a buffer.
  If the buffer is too small, it raises CR_MALFORMED_PACKET_ERROR
  and sets is_error to true.
  Otherwise it sets is_error to false and calls @ref inet_field_length_ll.

  @sa @ref net_field_length_ll

  @param mysql the handle to return an error in
  @param [in,out] packet pointer to the buffer to read the length from
  @param packet_length  remaining bytes in packet
  @param [out] is_error set to true if the buffer contains no room for a
  full length, false otherwise.
  @return the size read.
*/
inline my_ulonglong net_field_length_ll_safe(MYSQL *mysql, uchar **packet,
                                             ulong packet_length,
                                             bool *is_error) {
  const size_t sizeof_len = net_field_length_size(*packet);
  DBUG_EXECUTE_IF("simulate_bad_packet",
                  { *packet = *packet + packet_length + 1000000L; });
  if (!buffer_check_remaining(mysql, *packet, packet_length, sizeof_len)) {
    *is_error = true;
    return 0;
  }

  *is_error = false;
  return net_field_length_ll(packet);
}

/**
 Read Ok packet along with the server state change information.
*/
void read_ok_ex(MYSQL *mysql, ulong length) {
  size_t total_len, len;
  uchar *pos, *saved_pos;
  my_ulonglong affected_rows, insert_id;
  char *db;
  char *data_str;

  CHARSET_INFO *saved_cs;
  bool is_charset;

  STATE_INFO *info = nullptr;
  LIST *element = nullptr;
  LEX_STRING *data = nullptr;
  bool is_error;

  pos = mysql->net.read_pos + 1;

  affected_rows = net_field_length_ll_safe(mysql, &pos, length,
                                           &is_error); /* affected rows */
  if (is_error) return;
  insert_id =
      net_field_length_ll_safe(mysql, &pos, length, &is_error); /* insert id */
  if (is_error) return;

  /*
   The following check ensures that we skip the assignment for the
   above read fields (i.e. affected_rows and insert_id) wherein the
   EOF packets are deprecated and the server sends OK packet instead
   with a packet header of 0xFE (254) to identify it as an EOF packet.
   We ignore this assignment as the valid contents of EOF packet include
   packet marker, server status and warning count only. However, we would
   assign these values to the connection handle if it was an OK packet
   with a packet header of 0x00.
  */

  if (!((mysql->server_capabilities & CLIENT_DEPRECATE_EOF) &&
        mysql->net.read_pos[0] == 254)) {
    mysql->affected_rows = affected_rows;
    mysql->insert_id = insert_id;

    DBUG_PRINT("info", ("affected_rows: %lu  insert_id: %lu",
                        (ulong)mysql->affected_rows, (ulong)mysql->insert_id));
  }

  if (!buffer_check_remaining(mysql, pos, length, 2)) return;
  /* server status */
  mysql->server_status = uint2korr(pos);
  pos += 2;

  if (protocol_41(mysql)) {
    if (!buffer_check_remaining(mysql, pos, length, 2)) return;
    mysql->warning_count = uint2korr(pos);
    pos += 2;
  } else
    mysql->warning_count = 0; /* MySQL 4.0 protocol */

  DBUG_PRINT("info", ("status: %u  warning_count: %u", mysql->server_status,
                      mysql->warning_count));
  if (mysql->server_capabilities & CLIENT_SESSION_TRACK) {
    free_state_change_info(static_cast<MYSQL_EXTENSION *>(mysql->extension));

    if (pos < mysql->net.read_pos + length) {
      /* get the info field */
      const size_t length_msg_member =
          (size_t)net_field_length_ll_safe(mysql, &pos, length, &is_error);
      if (is_error) return;
      if (!buffer_check_remaining(mysql, pos, length, length_msg_member))
        return;
      mysql->info = (length_msg_member ? (char *)pos : nullptr);
      pos += (length_msg_member);

      /* read session state changes info */
      if (mysql->server_status & SERVER_SESSION_STATE_CHANGED) {
        saved_pos = pos;
        total_len =
            (size_t)net_field_length_ll_safe(mysql, &pos, length, &is_error);
        if (is_error) return;
        /* ensure that mysql->info is zero-terminated */
        if (mysql->info) *saved_pos = 0;

        while (total_len > 0) {
          saved_pos = pos;
          const my_ulonglong type =
              net_field_length_ll_safe(mysql, &pos, length, &is_error);
          if (is_error) return;
          switch (type) {
            case SESSION_TRACK_SYSTEM_VARIABLES:
              /* Move past the total length of the changed entity. */
              (void)net_field_length_ll_safe(mysql, &pos, length, &is_error);
              if (is_error) return;

              /* Name of the system variable. */
              len = (size_t)net_field_length_ll_safe(mysql, &pos, length,
                                                     &is_error);
              if (is_error) return;
              if (!buffer_check_remaining(mysql, pos, length, len)) return;

              if (!my_multi_malloc(key_memory_MYSQL_state_change_info, MYF(0),
                                   &element, sizeof(LIST), &data,
                                   sizeof(LEX_STRING), &data_str, len, NullS)) {
                set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
                return;
              }

              data->str = data_str;
              memcpy(data->str, (char *)pos, len);
              data->length = len;
              pos += len;

              element->data = data;
              ADD_INFO(info, element, SESSION_TRACK_SYSTEM_VARIABLES);

              /*
               Check if the changed variable was charset. In that case we need
               to update mysql->charset.
               */
              if (!strncmp(data->str, "character_set_client", data->length))
                is_charset = true;
              else
                is_charset = false;

              /* Value of the system variable. */
              len = (size_t)net_field_length_ll_safe(mysql, &pos, length,
                                                     &is_error);
              if (is_error) return;
              if (!buffer_check_remaining(mysql, pos, length, len)) return;

              if (!my_multi_malloc(key_memory_MYSQL_state_change_info, MYF(0),
                                   &element, sizeof(LIST), &data,
                                   sizeof(LEX_STRING), &data_str, len, NullS)) {
                set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
                return;
              }

              data->str = data_str;
              memcpy(data->str, (char *)pos, len);
              data->length = len;
              pos += len;

              element->data = data;
              ADD_INFO(info, element, SESSION_TRACK_SYSTEM_VARIABLES);

              if (is_charset == 1) {
                char charset_name[MY_CS_NAME_SIZE * 8];  // MY_CS_BUFFER_SIZE
                size_t charset_name_length =
                    std::min(data->length, sizeof(charset_name) - 1);
                saved_cs = mysql->charset;

                memcpy(charset_name, data->str, charset_name_length);
                charset_name[charset_name_length] = 0;

                if (!(mysql->charset = get_charset_by_csname(
                          charset_name, MY_CS_PRIMARY, MYF(MY_WME)))) {
                  DBUG_PRINT(
                      "warning",
                      ("session tracker supplied %s is not a valid charset."
                       " Keeping the old one.",
                       charset_name));
                  mysql->charset = saved_cs;
                }
              }
              break;
            case SESSION_TRACK_TRANSACTION_STATE:
            case SESSION_TRACK_TRANSACTION_CHARACTERISTICS:
            case SESSION_TRACK_SCHEMA:

              /* Move past the total length of the changed entity. */
              (void)net_field_length_ll_safe(mysql, &pos, length, &is_error);
              if (is_error) return;
              len = (size_t)net_field_length_ll_safe(mysql, &pos, length,
                                                     &is_error);
              if (is_error) return;
              if (!buffer_check_remaining(mysql, pos, length, len)) return;

              if (!my_multi_malloc(key_memory_MYSQL_state_change_info, MYF(0),
                                   &element, sizeof(LIST), &data,
                                   sizeof(LEX_STRING), &data_str, len, NullS)) {
                set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
                return;
              }

              data->str = data_str;
              memcpy(data->str, (char *)pos, len);
              data->length = len;
              pos += len;

              element->data = data;
              ADD_INFO(info, element, type);

              if (type == SESSION_TRACK_SCHEMA) {
                if (!(db = (char *)my_malloc(key_memory_MYSQL_state_change_info,
                                             data->length + 1, MYF(MY_WME)))) {
                  set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
                  return;
                }

                if (mysql->db) my_free(mysql->db);

                memcpy(db, data->str, data->length);
                db[data->length] = '\0';
                mysql->db = db;
              }

              break;
            case SESSION_TRACK_GTIDS:
              /* Move past the total length of the changed entity. */
              (void)net_field_length_ll_safe(mysql, &pos, length, &is_error);
              if (is_error) return;

              /* read (and ignore for now) the GTIDS encoding specification code
               */
              (void)net_field_length_ll_safe(mysql, &pos, length, &is_error);
              if (is_error) return;

              /*
                 For now we ignore the encoding specification, since only one
                 is supported. In the future the decoding of what comes next
                 depends on the specification code.
                 */

              /* read the length of the encoded string. */
              len = (size_t)net_field_length_ll_safe(mysql, &pos, length,
                                                     &is_error);
              if (is_error) return;
              if (!buffer_check_remaining(mysql, pos, length, len)) return;

              if (!my_multi_malloc(key_memory_MYSQL_state_change_info, MYF(0),
                                   &element, sizeof(LIST), &data,
                                   sizeof(LEX_STRING), &data_str, len, NullS)) {
                set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
                return;
              }

              data->str = data_str;
              memcpy(data->str, (char *)pos, len);
              data->length = len;
              pos += len;

              element->data = data;
              ADD_INFO(info, element, SESSION_TRACK_GTIDS);
              break;
            case SESSION_TRACK_STATE_CHANGE:
              /* Get the length of the boolean tracker */
              len = (size_t)net_field_length_ll_safe(mysql, &pos, length,
                                                     &is_error);
              if (is_error) return;

              /* length for boolean tracker is always 1 */
              assert(len == 1);
              if (!buffer_check_remaining(mysql, pos, length, len)) return;

              if (!my_multi_malloc(key_memory_MYSQL_state_change_info, MYF(0),
                                   &element, sizeof(LIST), &data,
                                   sizeof(LEX_STRING), &data_str, len, NullS)) {
                set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
                return;
              }

              data->str = data_str;
              memcpy(data->str, (char *)pos, len);
              data->length = len;
              pos += len;

              element->data = data;
              ADD_INFO(info, element, SESSION_TRACK_STATE_CHANGE);

              break;
            default:
              if (type > SESSION_TRACK_END) {
                DBUG_PRINT(
                    "warning",
                    ("invalid/unknown session tracker type received: %llu",
                     (unsigned long long)type));
              }
              /*
               Unknown/unsupported type received, get the total length and
               move past it.
               */

              len = (size_t)net_field_length_ll_safe(mysql, &pos, length,
                                                     &is_error);
              if (is_error) return;
              if (!buffer_check_remaining(mysql, pos, length, len)) return;
              pos += len;
              break;
          }
          total_len -= (pos - saved_pos);
        }
        if (info) {
          int itype;
          for (itype = SESSION_TRACK_BEGIN; itype <= SESSION_TRACK_END;
               itype++) {
            if (info->info_list[itype].head_node) {
              info->info_list[itype].current_node =
                  info->info_list[itype].head_node =
                      list_reverse(info->info_list[itype].head_node);
            }
          }
        }
      }
    }
  } else if (pos < mysql->net.read_pos + length && net_field_length(&pos))
    mysql->info = (char *)pos;
  else
    mysql->info = nullptr;
  return;
}

/* Helper for cli_safe_read and cli_safe_read_nonblocking */
static ulong cli_safe_read_with_ok_complete(MYSQL *mysql, bool parse_ok,
                                            bool *is_data_packet, ulong len);

/**
  Read a packet from server in asynchronous way. This function can return
  without completely reading the packet, in such a case call this function
  again until complete packet is read.

  @param[in]  mysql           connection handle
  @param[in]  parse_ok        if set to true then parse OK packet if it
                              was sent by server
  @param[out] is_data_packet  if set to true then the packet received
                              was a "data packet".
  @param[out] res             The length of the packet that was read or
                              packet_error in case of error.

  @retval     NET_ASYNC_NOT_READY  packet was not completely read
  @retval     NET_ASYNC_COMPLETE   finished reading packet
*/
net_async_status cli_safe_read_with_ok_nonblocking(MYSQL *mysql, bool parse_ok,
                                                   bool *is_data_packet,
                                                   ulong *res) {
  NET *net = &mysql->net;
  ulong len = 0;
  DBUG_TRACE;
  assert(net->vio);

  if (NET_ASYNC_NOT_READY == my_net_read_nonblocking(net, &len)) {
    return NET_ASYNC_NOT_READY;
  }

  DBUG_PRINT("info",
             ("total nb read: %lu,  net->where_b: %lu", len, net->where_b));

  *res = cli_safe_read_with_ok_complete(mysql, parse_ok, is_data_packet, len);

  /*
    In case, packet is too large or connection is lost, net_end() is called to
    free up net->extension. Thus return NET_ASYNC_ERROR.
  */
  if ((*res == packet_error) && (NET_ASYNC_DATA(net) == nullptr)) {
    return NET_ASYNC_ERROR;
  }
  return NET_ASYNC_COMPLETE;
}

/**
  Its a non blocking version of cli_safe_read
*/
net_async_status cli_safe_read_nonblocking(MYSQL *mysql, bool *is_data_packet,
                                           ulong *res) {
  return cli_safe_read_with_ok_nonblocking(mysql, false, is_data_packet, res);
}

/**
  Read a packet from server. Give error message if socket was down
  or packet is an error message

  @param[in]    mysql           connection handle
  @param[in]    parse_ok        if set to true then parse OK packet
                                if it is received
  @param[out]   is_data_packet
                                if set to true then packet received is
                                a "data packet", that is not OK or ERR
                                packet or EOF in case of old servers

  @return  The length of the packet that was read or packet_error in
           case of error. In case of error its description is stored
            in mysql handle.
*/

ulong cli_safe_read_with_ok(MYSQL *mysql, bool parse_ok, bool *is_data_packet) {
  DBUG_TRACE;
  NET *net = &mysql->net;
  ulong len = 0;

  MYSQL_TRACE(READ_PACKET, mysql, ());

  if (is_data_packet) *is_data_packet = false;

  if (net->vio != nullptr) len = my_net_read(net);
  return cli_safe_read_with_ok_complete(mysql, parse_ok, is_data_packet, len);
}

ulong cli_safe_read_with_ok_complete(MYSQL *mysql, bool parse_ok,
                                     bool *is_data_packet, ulong len) {
  NET *net = &mysql->net;
  DBUG_TRACE;

  if (len == packet_error || len == 0) {
#ifndef NDEBUG
    char desc[VIO_DESCRIPTION_SIZE]{"n/a"};
    if (net->vio) vio_description(net->vio, desc);
    DBUG_PRINT("error",
               ("Wrong connection or packet. fd: %s  len: %lu", desc, len));
#endif  // NDEBUG
#ifdef MYSQL_SERVER
    if (net->vio && (net->last_errno == ER_NET_READ_INTERRUPTED))
      return packet_error;
#endif /*MYSQL_SERVER*/
    end_server(mysql);
    set_mysql_error(mysql,
                    net->last_errno == ER_NET_PACKET_TOO_LARGE
                        ? CR_NET_PACKET_TOO_LARGE
                        : CR_SERVER_LOST,
                    unknown_sqlstate);
    return packet_error;
  }

  MYSQL_TRACE(PACKET_RECEIVED, mysql, (len, net->read_pos));

  if (net->read_pos[0] == 255) {
    /*
      After server reprts an error, usually it is ready to accept new commands
      and we set stage to READY_FOR_COMMAND. This can be modified by the caller
      of cli_safe_read().
    */
    MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);

    if (len > 3) {
      uchar *pos = net->read_pos + 1;
      net->last_errno = uint2korr(pos);
      pos += 2;
      len -= 2;
      if (protocol_41(mysql) && pos[0] == '#') {
        strmake(net->sqlstate, (char *)pos + 1, SQLSTATE_LENGTH);
        pos += SQLSTATE_LENGTH + 1;
      } else {
        /*
          The SQL state hasn't been received -- it should be reset to HY000
          (unknown error sql state).
        */

        my_stpcpy(net->sqlstate, unknown_sqlstate);
      }

      (void)strmake(net->last_error, (char *)pos,
                    std::min<ulong>(len, sizeof(net->last_error) - 1));
    } else
      set_mysql_error(mysql, CR_UNKNOWN_ERROR, unknown_sqlstate);
    /*
      Cover a protocol design error: error packet does not
      contain the server status. Therefore, the client has no way
      to find out whether there are more result sets of
      a multiple-result-set statement pending. Luckily, in 5.0 an
      error always aborts execution of a statement, wherever it is
      a multi-statement or a stored procedure, so it should be
      safe to unconditionally turn off the flag here.
    */
    mysql->server_status &= ~SERVER_MORE_RESULTS_EXISTS;

    DBUG_PRINT("error", ("Got error: %d/%s (%s)", net->last_errno,
                         net->sqlstate, net->last_error));
    return packet_error;
  } else {
    /* if it is OK packet irrespective of new/old server */
    if (net->read_pos[0] == 0) {
      if (parse_ok) {
        read_ok_ex(mysql, len);
        return len;
      }
    }
    /*
      Now we have a data packet, unless it is OK packet starting with
      0xFE - we detect that case below.
    */
    if (is_data_packet) *is_data_packet = true;
    /*
       For a packet starting with 0xFE detect if it is OK packet or a
       huge data packet. Note that old servers do not send OK packets
       starting with 0xFE.
    */
    if ((mysql->server_capabilities & CLIENT_DEPRECATE_EOF) &&
        (net->read_pos[0] == 254)) {
      /* detect huge data packet */
      if (len > MAX_PACKET_LENGTH) return len;
      /* otherwise we have OK packet starting with 0xFE */
      if (is_data_packet) *is_data_packet = false;
      /* parse it if requested */
      if (parse_ok) read_ok_ex(mysql, len);
      return len;
    }
    /* for old client detect EOF packet */
    if (!(mysql->server_capabilities & CLIENT_DEPRECATE_EOF) &&
        (net->read_pos[0] == 254) && (len < 8)) {
      if (is_data_packet) *is_data_packet = false;
    }
  }
  return len;
}

/**
  Read a packet from server. Give error message if connection was broken or
  ERR packet was received. Detect if the packet received was an OK, ERR or
  something else (a "data packet").

  @param[in]  mysql           connection handle
  @param[out] is_data_packet
                              if set to true then the packet received
                              was a "data packet".

  @retval The length of the packet that was read or packet_error in case of
          error. In case of error its description is stored in mysql handle.
*/
ulong cli_safe_read(MYSQL *mysql, bool *is_data_packet) {
  return cli_safe_read_with_ok(mysql, false, is_data_packet);
}

void free_rows(MYSQL_DATA *cur) {
  if (cur) {
    cur->alloc->Clear();
    my_free(cur->alloc);
    my_free(cur);
  }
}

bool cli_advanced_command(MYSQL *mysql, enum enum_server_command command,
                          const uchar *header, size_t header_length,
                          const uchar *arg, size_t arg_length, bool skip_check,
                          MYSQL_STMT *stmt) {
  NET *net = &mysql->net;
  bool result = true;
  const bool stmt_skip = stmt ? stmt->state != MYSQL_STMT_INIT_DONE : false;
  DBUG_TRACE;

  if (mysql->net.vio == nullptr || net->error == NET_ERROR_SOCKET_UNUSABLE) {
    /* Do reconnect is possible */
    if (!mysql->reconnect || mysql_reconnect(mysql) || stmt_skip) {
      set_mysql_error(mysql, CR_SERVER_LOST, unknown_sqlstate);
      return true;
    }
    /* reconnect succeeded */
    assert(mysql->net.vio != nullptr);
  }

  /* turn off non blocking operations */
  if (!vio_is_blocking(mysql->net.vio))
    vio_set_blocking_flag(mysql->net.vio, true);

  if (mysql->status != MYSQL_STATUS_READY ||
      mysql->server_status & SERVER_MORE_RESULTS_EXISTS) {
    DBUG_PRINT("error", ("state: %d", mysql->status));
    set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    return true;
  }

  net_clear_error(net);
  mysql->info = nullptr;
  mysql->affected_rows = ~(my_ulonglong)0;
  /*
    Do not check the socket/protocol buffer as the
    result/error/timeout of a previous command might not have been read.
    This can happen if a client sends a query but does not reap the result
    before attempting to close the connection or wait_timeout occurs on
    the server.
  */
  net_clear(&mysql->net, false);

  MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
  MYSQL_TRACE(SEND_COMMAND, mysql,
              (command, header_length, arg_length, header, arg));

  /*
    If auto-reconnect mode is enabled check if connection is still alive before
    sending new command. Otherwise, send() might not notice that connection was
    closed by the server (for example, due to KILL statement), and the fact that
    connection is gone will be noticed only on attempt to read command's result,
    when it is too late to reconnect. Note that such scenario can still occur if
    connection gets killed after this check but before command is sent to
    server. But this should be rare.
  */
  if ((command != COM_QUIT) && mysql->reconnect && !vio_is_connected(net->vio))
    net->error = NET_ERROR_SOCKET_UNUSABLE;

  if (net_write_command(net, (uchar)command, header, header_length, arg,
                        arg_length)) {
    DBUG_PRINT("error",
               ("Can't send command to server. Error: %d", socket_errno));
    if (net->last_errno == ER_NET_PACKET_TOO_LARGE) {
      set_mysql_error(mysql, CR_NET_PACKET_TOO_LARGE, unknown_sqlstate);
      goto end;
    }
    if (net->last_errno == ER_NET_ERROR_ON_WRITE) {
      /*
        Write error, try to read and see if the server gave an error
        before closing the connection. Most likely Unix Domain Socket.
      */
      if (net->vio) {
        my_net_set_read_timeout(net, 1);
        /*
          cli_safe_read will also set error variables in net,
          and we are already in error state.
        */
        if (cli_safe_read(mysql, nullptr) == packet_error) {
          if (!mysql->reconnect) goto end;
        }
        /* Can this happen in any other case than COM_QUIT? */
        if (!mysql->reconnect) assert(command == COM_QUIT);
      }
    }
    end_server(mysql);
    if (mysql_reconnect(mysql) || stmt_skip) goto end;
    MYSQL_TRACE(SEND_COMMAND, mysql,
                (command, header_length, arg_length, header, arg));
    if (net_write_command(net, (uchar)command, header, header_length, arg,
                          arg_length)) {
      set_mysql_error(mysql, CR_SERVER_GONE_ERROR, unknown_sqlstate);
      goto end;
    }
  }

  MYSQL_TRACE(PACKET_SENT, mysql, (header_length + arg_length));

#if defined(CLIENT_PROTOCOL_TRACING)
  switch (command) {
    case COM_STMT_PREPARE:
      MYSQL_TRACE_STAGE(mysql, WAIT_FOR_PS_DESCRIPTION);
      break;

    case COM_STMT_FETCH:
      MYSQL_TRACE_STAGE(mysql, WAIT_FOR_ROW);
      break;

    /*
            No server reply is expected after these commands so we reamin
            ready for the next command.
     */
    case COM_STMT_SEND_LONG_DATA:
    case COM_STMT_CLOSE:
    case COM_REGISTER_SLAVE:
    case COM_QUIT:
      break;

    /*
            These replication commands are not supported and we bail out
            by pretending that connection has been closed.
     */
    case COM_BINLOG_DUMP:
    case COM_BINLOG_DUMP_GTID:
    case COM_TABLE_DUMP:
      MYSQL_TRACE(DISCONNECTED, mysql, ());
      break;

    /*
            After COM_CHANGE_USER a regular authentication exchange
            is performed.
     */
    case COM_CHANGE_USER:
      MYSQL_TRACE_STAGE(mysql, AUTHENTICATE);
      break;

    /*
            Server replies to COM_STATISTICS with a single packet
            containing a string with statistics information.
     */
    case COM_STATISTICS:
      MYSQL_TRACE_STAGE(mysql, WAIT_FOR_PACKET);
      break;

    /*
            For all other commands we expect server to send regular reply
            which is either OK, ERR or a result-set header.
     */
    default:
      MYSQL_TRACE_STAGE(mysql, WAIT_FOR_RESULT);
      break;
  }
#endif

  result = false;
  if (!skip_check) {
    result = ((mysql->packet_length =
                   cli_safe_read_with_ok(mysql, true, nullptr)) == packet_error
                  ? 1
                  : 0);

#if defined(CLIENT_PROTOCOL_TRACING)
    /*
      Return to READY_FOR_COMMAND protocol stage in case server reports error
      or sends OK packet.
    */
    if (result || mysql->net.read_pos[0] == 0x00)
      MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
#endif
  }

end:
  DBUG_PRINT("exit", ("result: %d", result));
  return result;
}

net_async_status cli_advanced_command_nonblocking(
    MYSQL *mysql, enum enum_server_command command, const uchar *header,
    ulong header_length, const uchar *arg, ulong arg_length, bool skip_check,
    MYSQL_STMT *stmt, bool *ret) {
  NET *net = &mysql->net;
  NET_ASYNC *net_async = NET_ASYNC_DATA(net);
  bool result = true;
  *ret = result;
  const bool stmt_skip = stmt ? stmt->state != MYSQL_STMT_INIT_DONE : false;
  DBUG_TRACE;
  DBUG_DUMP("sending", header, header_length);
  if (arg && arg_length) {
    DBUG_DUMP("sending arg", arg, arg_length);
  }

  if (mysql->net.vio == nullptr) {
    set_mysql_error(mysql, CR_SERVER_GONE_ERROR, unknown_sqlstate);
    goto end;
  }
  /**
    When non blocking API execution is pending and did not complete then
    it can result in async context to be null. In such case if user executes
    any other API report command out of sync error.
  */
  if (net_async == nullptr) {
    set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    goto end;
  }
  if (net_async->async_send_command_status == NET_ASYNC_SEND_COMMAND_IDLE) {
    if (vio_is_blocking(mysql->net.vio)) {
      vio_set_blocking_flag(net->vio, false);
    }

    if (mysql->status != MYSQL_STATUS_READY ||
        mysql->server_status & SERVER_MORE_RESULTS_EXISTS) {
      DBUG_PRINT("error", ("state: %d", mysql->status));
      set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
      return NET_ASYNC_COMPLETE;
    }

    net_clear_error(net);
    mysql->info = nullptr;
    mysql->affected_rows = ~(my_ulonglong)0;
    /*
      Do not check the socket/protocol buffer on COM_QUIT as the
      result of a previous command might not have been read. This
      can happen if a client sends a query but does not reap
      the result before attempting to close the connection.
    */
    assert(command <= COM_END);
    net_clear(&mysql->net, false);
    net_async->async_send_command_status = NET_ASYNC_SEND_COMMAND_WRITE_COMMAND;
  }

  MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
  if (net_async->async_send_command_status ==
      NET_ASYNC_SEND_COMMAND_WRITE_COMMAND) {
    bool err;
    MYSQL_TRACE(SEND_COMMAND, mysql,
                (command, header_length, arg_length, header, arg));
    const net_async_status status = net_write_command_nonblocking(
        net, (uchar)command, header, header_length, arg, arg_length, &err);
    if (status == NET_ASYNC_NOT_READY) {
      return NET_ASYNC_NOT_READY;
    }
    if (err) {
      DBUG_PRINT("error",
                 ("Can't send command to server. Error: %d", socket_errno));
      if (net->last_errno == ER_NET_PACKET_TOO_LARGE) {
        set_mysql_error(mysql, CR_NET_PACKET_TOO_LARGE, unknown_sqlstate);
        goto end;
      }
      end_server(mysql);
      /* reset net_async to null as its reference has been freed */
      net_async = nullptr;
      if (stmt_skip) goto end;
      set_mysql_error(mysql, CR_SERVER_GONE_ERROR, unknown_sqlstate);
      goto end;
    }
    MYSQL_TRACE(PACKET_SENT, mysql, (header_length + arg_length));
    if (skip_check) {
      result = false;
      goto end;
    } else {
      net_async->async_send_command_status = NET_ASYNC_SEND_COMMAND_READ_STATUS;
    }
  }

  if (net_async->async_send_command_status ==
      NET_ASYNC_SEND_COMMAND_READ_STATUS) {
    ulong pkt_len;
    const net_async_status status =
        cli_safe_read_with_ok_nonblocking(mysql, true, nullptr, &pkt_len);
    if (status == NET_ASYNC_NOT_READY) {
      return NET_ASYNC_NOT_READY;
    }
    mysql->packet_length = pkt_len;
    result = (pkt_len == packet_error ? 1 : 0);
#if defined(CLIENT_PROTOCOL_TRACING)
    /*
      Return to READY_FOR_COMMAND protocol stage in case server reports
      error or sends OK packet.
    */
    if (!result || mysql->net.read_pos[0] == 0x00)
      MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
#endif
  }
end:
  if (net_async)
    net_async->async_send_command_status = NET_ASYNC_SEND_COMMAND_IDLE;
  DBUG_PRINT("exit", ("result: %d", result));
  *ret = result;
  return NET_ASYNC_COMPLETE;
}

void free_old_query(MYSQL *mysql) {
  DBUG_TRACE;
  if (mysql->field_alloc) {
    mysql->field_alloc->Clear();
  }
  mysql->fields = nullptr;
  mysql->field_count = 0; /* For API */
  mysql->warning_count = 0;
  mysql->info = nullptr;
}

/**
  Finish reading of a partial result set from the server in asynchronous
  way. This function can return without completely flushing the result set,
  in such a case call this function again until result set in flushed.
  Read OK packet in case result set is not a data packet.

  @param[in]  mysql           connection handle
  @param[out] res             true in case of protocol error, false otherwise

  @retval     NET_ASYNC_NOT_READY  result set not flushed yet
  @retval     NET_ASYNC_COMPLETE   finished flushing result set
*/
static net_async_status flush_one_result_nonblocking(MYSQL *mysql, bool *res) {
  DBUG_TRACE;

  *res = false;
  while (true) {
    ulong packet_length;
    bool is_data_packet;
    if (cli_safe_read_nonblocking(mysql, &is_data_packet, &packet_length) ==
        NET_ASYNC_NOT_READY) {
      return NET_ASYNC_NOT_READY;
    }
    mysql->packet_length = packet_length;
    if (packet_length == packet_error) {
      *res = true;
      break;
    }
    if (mysql->net.read_pos[0] != 0 && !is_data_packet) {
      if (protocol_41(mysql)) {
        uchar *pos = mysql->net.read_pos + 1;
        if (mysql->server_capabilities & CLIENT_DEPRECATE_EOF &&
            !is_data_packet) {
          read_ok_ex(mysql, packet_length);
        } else {
          mysql->warning_count = uint2korr(pos);
          pos += 2;
          mysql->server_status = uint2korr(pos);
        }
        pos += 2;
      }
      break;
    }
  }
  return NET_ASYNC_COMPLETE;
}

/**
  Finish reading of a partial result set from the server.
  Get the EOF packet, and update mysql->status
  and mysql->warning_count.

  @return  true if a communication or protocol error, an error
           is set in this case, false otherwise.
*/

static bool flush_one_result(MYSQL *mysql) {
  ulong packet_length;
  bool is_data_packet;

  assert(mysql->status != MYSQL_STATUS_READY);

  do {
    packet_length = cli_safe_read(mysql, &is_data_packet);
    /*
      There is an error reading from the connection,
      or (sic!) there were no error and no
      data in the stream, i.e. no more data from the server.
      Since we know our position in the stream (somewhere in
      the middle of a result set), this latter case is an error too
      -- each result set must end with a EOF packet.
      cli_safe_read() has set an error for us, just return.
    */
    if (packet_length == packet_error) return true;
  } while (mysql->net.read_pos[0] == 0 || is_data_packet);

  /* Analyse final OK packet (EOF packet if it is old client) */

  if (protocol_41(mysql)) {
    uchar *pos = mysql->net.read_pos + 1;
    if (mysql->server_capabilities & CLIENT_DEPRECATE_EOF && !is_data_packet)
      read_ok_ex(mysql, packet_length);
    else {
      mysql->warning_count = uint2korr(pos);
      pos += 2;
      mysql->server_status = uint2korr(pos);
    }
    pos += 2;
  }
#if defined(CLIENT_PROTOCOL_TRACING)
  if (mysql->server_status & SERVER_MORE_RESULTS_EXISTS)
    MYSQL_TRACE_STAGE(mysql, WAIT_FOR_RESULT);
  else
    MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
#endif
  return false;
}

static bool is_OK_packet(MYSQL *mysql, ulong length) {
  return ((mysql->net.read_pos[0] == 0) ||
          ((mysql->server_capabilities & CLIENT_DEPRECATE_EOF) &&
           mysql->net.read_pos[0] == 254 && length < MAX_PACKET_LENGTH));
}

/**
  Helper method to check if received packet is AuthNextFactor packet.

  @param[in]   mysql              connection handle

  @retval      true packet is AuthNextFactor packet
  @retval      false if its not AuthNextFactor packet
*/
static bool is_auth_next_factor_packet(MYSQL *mysql) {
  return ((mysql->server_capabilities & MULTI_FACTOR_AUTHENTICATION) &&
          (mysql->net.read_pos[0] == AUTH_NEXT_FACTOR_PACKETTYPE));
}
/**
  Read a packet from network. If it's an OK packet, flush it.

  @return  true if error, false otherwise. In case of
           success, is_ok_packet is set to true or false,
           based on what we got from network.
*/

static bool opt_flush_ok_packet(MYSQL *mysql, bool *is_ok_packet) {
  bool is_data_packet;
  const ulong packet_length = cli_safe_read(mysql, &is_data_packet);

  if (packet_length == packet_error) return true;

  /* cli_safe_read always reads a non-empty packet. */
  assert(packet_length);

  *is_ok_packet = is_OK_packet(mysql, packet_length);
  if (*is_ok_packet) {
    read_ok_ex(mysql, packet_length);
#if defined(CLIENT_PROTOCOL_TRACING)
    if (mysql->server_status & SERVER_MORE_RESULTS_EXISTS)
      MYSQL_TRACE_STAGE(mysql, WAIT_FOR_RESULT);
    else
      MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
#endif
  }

  return false;
}

static net_async_status cli_flush_use_result_nonblocking(MYSQL *mysql,
                                                         bool flush_all_results
                                                         [[maybe_unused]]) {
  DBUG_TRACE;
  /*
    flush_all_results is only used for mysql_stmt_close, and async is not
    supported for that.
  */
  assert(!flush_all_results);
  bool res;
  return flush_one_result_nonblocking(mysql, &res);
}

/*
  Flush result set sent from server
*/

static void cli_flush_use_result(MYSQL *mysql, bool flush_all_results) {
  /* Clear the current execution status */
  DBUG_TRACE;
  DBUG_PRINT("warning", ("Not all packets read, clearing them"));

  if (flush_one_result(mysql)) return; /* An error occurred */

  if (!flush_all_results) return;

  while (mysql->server_status & SERVER_MORE_RESULTS_EXISTS) {
    bool is_ok_packet;
    if (opt_flush_ok_packet(mysql, &is_ok_packet))
      return; /* An error occurred. */
    if (is_ok_packet) {
      /*
        Indeed what we got from network was an OK packet, and we
        know that OK is the last one in a multi-result-set, so
        just return.
      */
      return;
    }

    /*
      It's a result set, not an OK packet. A result set contains
      of two result set subsequences: field metadata, terminated
      with EOF packet, and result set data, again terminated with
      EOF packet. Read and flush them.
    */
    MYSQL_TRACE_STAGE(mysql, WAIT_FOR_FIELD_DEF);
    if (!(mysql->server_capabilities & CLIENT_DEPRECATE_EOF)) {
      if (flush_one_result(mysql)) return; /* An error occurred. */
    } else {
      uchar *pos = (uchar *)mysql->net.read_pos;
      const ulong field_count = net_field_length(&pos);
      if (read_com_query_metadata(mysql, pos, field_count)) {
        return;
      } else {
        mysql->field_alloc->Clear();
      }
    }
    MYSQL_TRACE_STAGE(mysql, WAIT_FOR_ROW);
    if (flush_one_result(mysql)) return;
  }
}

#ifdef _WIN32
static bool is_NT(void) {
  char *os = getenv("OS");
  return (os && !strcmp(os, "Windows_NT")) ? 1 : 0;
}
#endif

#ifdef CHECK_LICENSE
/**
  Check server side variable 'license'.

  If the variable does not exist or does not contain 'Commercial',
  we're talking to non-commercial server from commercial client.

  @retval  0   success
  @retval  !0  network error or the server is not commercial.
               Error code is saved in mysql->net.last_errno.
*/

static int check_license(MYSQL *mysql) {
  MYSQL_ROW row;
  MYSQL_RES *res;
  NET *net = &mysql->net;
  static const char query[] = "SELECT @@license";
  static const char required_license[] = STRINGIFY_ARG(LICENSE);

  if (mysql_real_query(mysql, query, (ulong)(sizeof(query) - 1))) {
    if (net->last_errno == ER_UNKNOWN_SYSTEM_VARIABLE) {
      set_mysql_extended_error(mysql, CR_WRONG_LICENSE, unknown_sqlstate,
                               ER_CLIENT(CR_WRONG_LICENSE), required_license);
    }
    return 1;
  }
  if (!(res = mysql_use_result(mysql))) return 1;
  row = mysql_fetch_row(res);
  /*
    If no rows in result set, or column value is nullptr (none of these
    two is ever true for server variables now), or column value
    mismatch, set wrong license error.
  */
  if (!net->last_errno &&
      (!row || !row[0] ||
       strncmp(row[0], required_license, sizeof(required_license)))) {
    set_mysql_extended_error(mysql, CR_WRONG_LICENSE, unknown_sqlstate,
                             ER_CLIENT(CR_WRONG_LICENSE), required_license);
  }
  mysql_free_result(res);
  return net->last_errno;
}
#endif /* CHECK_LICENSE */

/**************************************************************************
  Shut down connection
**************************************************************************/

void end_server(MYSQL *mysql) {
  const int save_errno = errno;
  DBUG_TRACE;
  if (mysql->net.vio != nullptr) {
#ifndef NDEBUG
    char desc[VIO_DESCRIPTION_SIZE];
    vio_description(mysql->net.vio, desc);
    DBUG_PRINT("info", ("Net: %s", desc));
#endif  // NDEBUG
#ifdef MYSQL_SERVER
    slave_io_thread_detach_vio();
#endif
    vio_delete(mysql->net.vio);
    mysql->net.vio = nullptr; /* Marker */
    mysql_prune_stmt_list(mysql);
  }
  net_end(&mysql->net);
  //  net_extension_free(&mysql->net);
  free_old_query(mysql);
  errno = save_errno;
  MYSQL_TRACE(DISCONNECTED, mysql, ());
}

/**
  Frees the memory allocated for a result, set by APIs which would have
  returned rows.

  @param[in]   result              buffer which needs to be freed

  @retval      NET_ASYNC_NOT_READY operation not complete, retry again
  @retval      NET_ASYNC_COMPLETE  operation complete
*/
net_async_status STDCALL mysql_free_result_nonblocking(MYSQL_RES *result) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("mysql_res: %p", result));
  if (!result) return NET_ASYNC_COMPLETE;

  MYSQL *mysql = result->handle;
  if (mysql) {
    if (mysql->unbuffered_fetch_owner == &result->unbuffered_fetch_cancelled)
      mysql->unbuffered_fetch_owner = nullptr;
    if (mysql->status == MYSQL_STATUS_USE_RESULT) {
      if (mysql->methods->flush_use_result_nonblocking(mysql, false) ==
          NET_ASYNC_NOT_READY) {
        return NET_ASYNC_NOT_READY;
      }
      mysql->status = MYSQL_STATUS_READY;
      if (mysql->unbuffered_fetch_owner) *mysql->unbuffered_fetch_owner = true;
    }
  }
  free_rows(result->data);
  if (result->field_alloc) {
    result->field_alloc->Clear();
    my_free(result->field_alloc);
  }
  my_free(result->row);
  my_free(result);

  return NET_ASYNC_COMPLETE;
}

void STDCALL mysql_free_result(MYSQL_RES *result) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("mysql_res: %p", result));
  if (result) {
    MYSQL *mysql = result->handle;
    if (mysql) {
      if (mysql->unbuffered_fetch_owner == &result->unbuffered_fetch_cancelled)
        mysql->unbuffered_fetch_owner = nullptr;
      if (mysql->status == MYSQL_STATUS_USE_RESULT) {
        (*mysql->methods->flush_use_result)(mysql, false);
        mysql->status = MYSQL_STATUS_READY;
        if (mysql->unbuffered_fetch_owner)
          *mysql->unbuffered_fetch_owner = true;
      }
    }
    free_rows(result->data);
    if (result->field_alloc) {
      result->field_alloc->Clear();
      my_free(result->field_alloc);
      result->field_alloc = nullptr;  // to avoid double free
    }
    my_free(result->row);
    my_free(result);
  }
}

/****************************************************************************
  Get options from my.cnf
****************************************************************************/

static const char *default_options[] = {"port",
                                        "socket",
                                        "compress",
                                        "password",
                                        "pipe",
                                        "timeout",
                                        "user",
                                        "init-command",
                                        "host",
                                        "database",
                                        "debug",
                                        "return-found-rows",
                                        "ssl-key",
                                        "ssl-cert",
                                        "ssl-ca",
                                        "ssl-capath",
                                        "character-sets-dir",
                                        "default-character-set",
                                        "interactive-timeout",
                                        "connect-timeout",
                                        "local-infile",
                                        "disable-local-infile",
                                        "ssl-cipher",
                                        "max-allowed-packet",
                                        "protocol",
                                        "shared-memory-base-name",
                                        "multi-results",
                                        "multi-statements",
                                        "multi-queries",
                                        "report-data-truncation",
                                        "plugin-dir",
                                        "default-auth",
                                        "bind-address",
                                        "ssl-crl",
                                        "ssl-crlpath",
                                        "enable-cleartext-plugin",
                                        "tls-version",
                                        "ssl_mode",
                                        "optional-resultset-metadata",
                                        "ssl-fips-mode",
                                        "tls-ciphersuites",
                                        NullS};
enum option_id {
  OPT_port = 1,
  OPT_socket,
  OPT_compress,
  OPT_password,
  OPT_pipe,
  OPT_timeout,
  OPT_user,
  OPT_init_command,
  OPT_host,
  OPT_database,
  OPT_debug,
  OPT_return_found_rows,
  OPT_ssl_key,
  OPT_ssl_cert,
  OPT_ssl_ca,
  OPT_ssl_capath,
  OPT_character_sets_dir,
  OPT_default_character_set,
  OPT_interactive_timeout,
  OPT_connect_timeout,
  OPT_local_infile,
  OPT_disable_local_infile,
  OPT_ssl_cipher,
  OPT_max_allowed_packet,
  OPT_protocol,
  OPT_shared_memory_base_name,
  OPT_multi_results,
  OPT_multi_statements,
  OPT_multi_queries,
  OPT_report_data_truncation,
  OPT_plugin_dir,
  OPT_default_auth,
  OPT_bind_address,
  OPT_ssl_crl,
  OPT_ssl_crlpath,
  OPT_enable_cleartext_plugin,
  OPT_tls_version,
  OPT_ssl_mode,
  OPT_optional_resultset_metadata,
  OPT_ssl_fips_mode,
  OPT_tls_ciphersuites,
  OPT_keep_this_one_last
};

static TYPELIB option_types = {array_elements(default_options) - 1, "options",
                               default_options, nullptr};

const char *sql_protocol_names_lib[] = {"TCP", "SOCKET", "PIPE", "MEMORY",
                                        NullS};
TYPELIB sql_protocol_typelib = {array_elements(sql_protocol_names_lib) - 1, "",
                                sql_protocol_names_lib, nullptr};

static int add_init_command(struct st_mysql_options *options, const char *cmd) {
  char *tmp;

  if (!options->init_commands) {
    void *rawmem = my_malloc(key_memory_mysql_options,
                             sizeof(Init_commands_array), MYF(MY_WME));
    if (!rawmem) return 1;
    options->init_commands =
        new (rawmem) Init_commands_array(key_memory_mysql_options);
  }

  if (!(tmp = my_strdup(key_memory_mysql_options, cmd, MYF(MY_WME))) ||
      options->init_commands->push_back(tmp)) {
    my_free(tmp);
    return 1;
  }

  return 0;
}

static char *set_ssl_option_unpack_path(const char *arg) {
  char *opt_var = nullptr;
  if (arg) {
    char *buff =
        (char *)my_malloc(key_memory_mysql_options, FN_REFLEN + 1, MYF(MY_WME));
    unpack_filename(buff, arg);
    opt_var = my_strdup(key_memory_mysql_options, buff, MYF(MY_WME));
    my_free(buff);
  }
  return opt_var;
}

void mysql_read_default_options(struct st_mysql_options *options,
                                const char *filename, const char *group) {
  int argc;
  char *argv_buff[1], **argv;
  const char *groups[3];
  DBUG_TRACE;
  DBUG_PRINT("enter",
             ("file: %s  group: %s", filename, group ? group : "NULL"));

  static_assert(OPT_keep_this_one_last == array_elements(default_options),
                "OPT_keep_this_one_last needs to be the last element.");

  argc = 1;
  argv = argv_buff;
  argv_buff[0] = const_cast<char *>("client");
  groups[0] = "client";
  groups[1] = group;
  groups[2] = nullptr;

  MEM_ROOT alloc{PSI_NOT_INSTRUMENTED, 512};
  my_load_defaults(filename, groups, &argc, &argv, &alloc, nullptr);
  if (argc != 1) /* If some default option */
  {
    char **option = argv;
    while (*++option) {
      if (my_getopt_is_args_separator(option[0])) /* skip arguments separator */
        continue;
      /* DBUG_PRINT("info",("option: %s",option[0])); */
      if (option[0][0] == '-' && option[0][1] == '-') {
        char *end = strchr(*option, '=');
        char *opt_arg = nullptr;
        if (end != nullptr) {
          opt_arg = end + 1;
          *end = 0; /* Remove '=' */
        }
        /* Change all '_' in variable name to '-' */
        for (end = *option; end != nullptr; end = strchr(end, '_')) *end = '-';
        switch (find_type(*option + 2, &option_types, FIND_TYPE_BASIC)) {
          case OPT_port:
            if (opt_arg) options->port = atoi(opt_arg);
            break;
          case OPT_socket:
            if (opt_arg) {
              my_free(options->unix_socket);
              options->unix_socket =
                  my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
            }
            break;
          case OPT_compress:
            options->compress = true;
            options->client_flag |= CLIENT_COMPRESS;
            break;
          case OPT_password:
            if (opt_arg) {
              my_free(options->password);
              options->password =
                  my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
            }
            break;
          case OPT_pipe:
            options->protocol = MYSQL_PROTOCOL_PIPE;
            break;
          case OPT_connect_timeout:
          case OPT_timeout:
            if (opt_arg) options->connect_timeout = atoi(opt_arg);
            break;
          case OPT_user:
            if (opt_arg) {
              my_free(options->user);
              options->user =
                  my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
            }
            break;
          case OPT_init_command:
            add_init_command(options, opt_arg);
            break;
          case OPT_host:
            if (opt_arg) {
              my_free(options->host);
              options->host =
                  my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
            }
            break;
          case OPT_database:
            if (opt_arg) {
              my_free(options->db);
              options->db =
                  my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
            }
            break;
          case OPT_debug:
#ifndef MYSQL_SERVER
            mysql_debug(opt_arg ? opt_arg : "d:t:o,/tmp/client.trace");
            break;
#endif
          case OPT_return_found_rows:
            options->client_flag |= CLIENT_FOUND_ROWS;
            break;
          case OPT_ssl_key:
            my_free(options->ssl_key);
            options->ssl_key =
                my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
            break;
          case OPT_ssl_cert:
            my_free(options->ssl_cert);
            options->ssl_cert =
                my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
            break;
          case OPT_ssl_ca:
            my_free(options->ssl_ca);
            options->ssl_ca =
                my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
            break;
          case OPT_ssl_capath:
            my_free(options->ssl_capath);
            options->ssl_capath =
                my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
            break;
          case OPT_ssl_cipher:
            my_free(options->ssl_cipher);
            options->ssl_cipher =
                my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
            break;
          case OPT_tls_ciphersuites:
            EXTENSION_SET_STRING(options, tls_ciphersuites, opt_arg);
            break;
          case OPT_tls_version:
            EXTENSION_SET_SSL_STRING(options, tls_version, opt_arg,
                                     SSL_MODE_PREFERRED);
            break;
          case OPT_ssl_crl:
            EXTENSION_SET_SSL_STRING(options, ssl_crl, opt_arg,
                                     SSL_MODE_PREFERRED);
            break;
          case OPT_ssl_crlpath:
            EXTENSION_SET_SSL_STRING(options, ssl_crlpath, opt_arg,
                                     SSL_MODE_PREFERRED);
            break;
          case OPT_character_sets_dir:
            my_free(options->charset_dir);
            options->charset_dir =
                my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
            break;
          case OPT_default_character_set:
            my_free(options->charset_name);
            options->charset_name =
                my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
            break;
          case OPT_interactive_timeout:
            options->client_flag |= CLIENT_INTERACTIVE;
            break;
          case OPT_local_infile:
            if (!opt_arg || atoi(opt_arg) != 0)
              options->client_flag |= CLIENT_LOCAL_FILES;
            else
              options->client_flag &= ~CLIENT_LOCAL_FILES;
            break;
          case OPT_disable_local_infile:
            options->client_flag &= ~CLIENT_LOCAL_FILES;
            break;
          case OPT_max_allowed_packet:
            if (opt_arg) options->max_allowed_packet = atoi(opt_arg);
            break;
          case OPT_protocol:
            if ((options->protocol = find_type(opt_arg, &sql_protocol_typelib,
                                               FIND_TYPE_BASIC)) <= 0) {
              my_message_local(ERROR_LEVEL, EE_UNKNOWN_PROTOCOL_OPTION,
                               opt_arg);
              exit(1);
            }
            break;
          case OPT_shared_memory_base_name:
#if defined(_WIN32)
            my_free(options->shared_memory_base_name);

            options->shared_memory_base_name =
                my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
#endif
            break;
          case OPT_multi_results:
            options->client_flag |= CLIENT_MULTI_RESULTS;
            break;
          case OPT_multi_statements:
          case OPT_multi_queries:
            options->client_flag |=
                CLIENT_MULTI_STATEMENTS | CLIENT_MULTI_RESULTS;
            break;
          case OPT_report_data_truncation:
            options->report_data_truncation =
                opt_arg ? (atoi(opt_arg) != 0) : true;
            break;
          case OPT_plugin_dir: {
            char buff[FN_REFLEN], buff2[FN_REFLEN];
            if (strlen(opt_arg) >= FN_REFLEN) opt_arg[FN_REFLEN] = '\0';
            if (my_realpath(buff, opt_arg, 0)) {
              DBUG_PRINT("warning",
                         ("failed to normalize the plugin path: %s", opt_arg));
              break;
            }
            convert_dirname(buff2, buff, nullptr);
            EXTENSION_SET_STRING(options, plugin_dir, buff2);
          } break;
          case OPT_default_auth:
            EXTENSION_SET_STRING(options, default_auth, opt_arg);
            break;
          case OPT_bind_address:
            my_free(options->bind_address);
            options->bind_address =
                my_strdup(key_memory_mysql_options, opt_arg, MYF(MY_WME));
            break;
          case OPT_enable_cleartext_plugin:
            ENSURE_EXTENSIONS_PRESENT(options);
            options->extension->enable_cleartext_plugin =
                (!opt_arg || atoi(opt_arg) != 0) ? true : false;
            break;
          case OPT_optional_resultset_metadata:
            if (!opt_arg || atoi(opt_arg) != 0)
              options->client_flag |= CLIENT_OPTIONAL_RESULTSET_METADATA;
            else
              options->client_flag &= ~CLIENT_OPTIONAL_RESULTSET_METADATA;
            break;

          default:
            DBUG_PRINT("warning", ("unknown option: %s", option[0]));
        }
      }
    }
  }
}

/**************************************************************************
  Get column lengths of the current row
  If one uses mysql_use_result, res->lengths contains the length information,
  else the lengths are calculated from the offset between pointers.
**************************************************************************/

static void cli_fetch_lengths(ulong *to, MYSQL_ROW column,
                              unsigned int field_count) {
  ulong *prev_length;
  char *start = nullptr;
  MYSQL_ROW end;

  prev_length = nullptr; /* Keep gcc happy */
  for (end = column + field_count + 1; column != end; column++, to++) {
    if (!*column) {
      *to = 0; /* Null */
      continue;
    }
    if (start) /* Found end of prev string */
      *prev_length = (ulong)(*column - start - 1);
    start = *column;
    prev_length = to;
  }
}

/**
  Read field metadata from field descriptor and store it in MYSQL_FIELD
  structure. String values in MYSQL_FIELD are allocated in a given allocator
  root.

  @param mysql          connection handle
  @param alloc          memory allocator root
  @param default_value  flag telling if default values should be read from
                        descriptor
  @param server_capabilities  protocol capability flags which determine format
  of the descriptor
  @param row            field descriptor
  @param field          address of MYSQL_FIELD structure to store metadata in.

  @returns 0 on success.
*/

static int unpack_field(MYSQL *mysql, MEM_ROOT *alloc, bool default_value,
                        uint server_capabilities, MYSQL_ROWS *row,
                        MYSQL_FIELD *field) {
  ulong lengths[9]; /* Max length of each field */
  DBUG_TRACE;

  if (!field) {
    set_mysql_error(mysql, CR_UNKNOWN_ERROR, unknown_sqlstate);
    return 1;
  }

  memset(field, 0, sizeof(MYSQL_FIELD));

  if (server_capabilities & CLIENT_PROTOCOL_41) {
    uchar *pos;
    /* fields count may be wrong */
    cli_fetch_lengths(&lengths[0], row->data, default_value ? 8 : 7);
    field->catalog = strmake_root(alloc, (char *)row->data[0], lengths[0]);
    field->db = strmake_root(alloc, (char *)row->data[1], lengths[1]);
    field->table = strmake_root(alloc, (char *)row->data[2], lengths[2]);
    field->org_table = strmake_root(alloc, (char *)row->data[3], lengths[3]);
    field->name = strmake_root(alloc, (char *)row->data[4], lengths[4]);
    field->org_name = strmake_root(alloc, (char *)row->data[5], lengths[5]);

    field->catalog_length = lengths[0];
    field->db_length = lengths[1];
    field->table_length = lengths[2];
    field->org_table_length = lengths[3];
    field->name_length = lengths[4];
    field->org_name_length = lengths[5];

    /* Unpack fixed length parts */
    if (lengths[6] != 12) {
      /* malformed packet. signal an error. */
      set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
      return 1;
    }

    pos = (uchar *)row->data[6];
    field->charsetnr = uint2korr(pos);
    field->length = (uint)uint4korr(pos + 2);
    field->type = (enum enum_field_types)pos[6];
    field->flags = uint2korr(pos + 7);
    field->decimals = (uint)pos[9];

    if (IS_NUM(field->type)) field->flags |= NUM_FLAG;
    if (default_value && row->data[7]) {
      field->def = strmake_root(alloc, (char *)row->data[7], lengths[7]);
      field->def_length = lengths[7];
    } else
      field->def = nullptr;
    field->max_length = 0;
  }
#ifndef DELETE_SUPPORT_OF_4_0_PROTOCOL
  else {
    /*
      If any of the row->data[] below is NULL, it can result in a
      crash. Error out early as it indicates a malformed packet.
      For data[0], data[1] and data[5], strmake_root() will handle
      NULL values.
    */
    if (!row->data[2] || !row->data[3] || !row->data[4]) {
      set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
      return 1;
    }

    cli_fetch_lengths(&lengths[0], row->data, default_value ? 6 : 5);
    field->org_table = field->table =
        strmake_root(alloc, (char *)row->data[0], lengths[0]);
    field->name = strmake_root(alloc, (char *)row->data[1], lengths[1]);
    field->length = (uint)uint3korr((uchar *)row->data[2]);
    field->type = (enum enum_field_types)(uchar)row->data[3][0];

    field->catalog = const_cast<char *>("");
    field->db = const_cast<char *>("");
    field->catalog_length = 0;
    field->db_length = 0;
    field->org_table_length = field->table_length = lengths[0];
    field->name_length = lengths[1];

    if (server_capabilities & CLIENT_LONG_FLAG) {
      if (lengths[4] != 3) {
        /* malformed packet. signal an error. */
        set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
        return 1;
      }
      field->flags = uint2korr((uchar *)row->data[4]);
      field->decimals = (uint)(uchar)row->data[4][2];
    } else {
      if (lengths[4] != 2) {
        /* malformed packet. signal an error. */
        set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
        return 1;
      }
      field->flags = (uint)(uchar)row->data[4][0];
      field->decimals = (uint)(uchar)row->data[4][1];
    }
    if (IS_NUM(field->type)) field->flags |= NUM_FLAG;
    if (default_value && row->data[5]) {
      field->def = strmake_root(alloc, (char *)row->data[5], lengths[5]);
      field->def_length = lengths[5];
    } else
      field->def = nullptr;
    field->max_length = 0;
  }
#endif /* DELETE_SUPPORT_OF_4_0_PROTOCOL */
  return 0;
}

/***************************************************************************
  Change field rows to field structs
***************************************************************************/
MYSQL_FIELD *unpack_fields(MYSQL *mysql, MYSQL_ROWS *data, MEM_ROOT *alloc,
                           uint fields, bool default_value,
                           uint server_capabilities) {
  MYSQL_ROWS *row;
  MYSQL_FIELD *field, *result;
  DBUG_TRACE;
  field = result = (MYSQL_FIELD *)alloc->Alloc((uint)sizeof(*field) * fields);
  if (!result) {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    return nullptr;
  }
  memset(field, 0, sizeof(MYSQL_FIELD) * fields);
  for (row = data; row; row = row->next, field++) {
    /* fields count may be wrong */
    if (field < result || static_cast<uint>(field - result) >= fields) {
      return nullptr;
    }
    if (unpack_field(mysql, alloc, default_value, server_capabilities, row,
                     field)) {
      return nullptr;
    }
  }
  return result;
}

/**
  Read metadata resultset from server in asynchronous way.

  @param[in]    mysql           connection handle
  @param[in]    alloc           memory allocator root
  @param[in]    field_count     total number of fields
  @param[in]    field           number of columns in single field descriptor
  @param[out]   ret             an array of field rows

  @retval     NET_ASYNC_NOT_READY  metadata resultset not read completely
  @retval     NET_ASYNC_COMPLETE   finished reading metadata resultset
*/
net_async_status cli_read_metadata_ex_nonblocking(MYSQL *mysql, MEM_ROOT *alloc,
                                                  ulong field_count,
                                                  unsigned int field,
                                                  MYSQL_FIELD **ret) {
  DBUG_TRACE;
  uchar *pos;
  ulong pkt_len;
  NET *net = &mysql->net;
  MYSQL_ASYNC *async_data = ASYNC_DATA(mysql);
  *ret = nullptr;

  if (!async_data->async_read_metadata_field_len) {
    async_data->async_read_metadata_field_len =
        (ulong *)alloc->Alloc(sizeof(ulong) * field);
  }
  if (!async_data->async_read_metadata_fields) {
    async_data->async_read_metadata_fields =
        (MYSQL_FIELD *)alloc->Alloc((uint)sizeof(MYSQL_FIELD) * field_count);
    if (async_data->async_read_metadata_fields)
      memset(async_data->async_read_metadata_fields, 0,
             sizeof(MYSQL_FIELD) * field_count);
  }

  if (!async_data->async_read_metadata_fields) {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    goto end;
  }

  if (!async_data->async_read_metadata_data.data) {
    async_data->async_read_metadata_data.data =
        (MYSQL_ROW)alloc->Alloc(sizeof(char *) * (field + 1));
    memset(async_data->async_read_metadata_data.data, 0,
           sizeof(char *) * (field + 1));
  }

  /*
    In this below loop we read each column info as 1 single row
    and save it in mysql->fields array
  */
  while (async_data->async_read_metadata_cur_field < field_count) {
    int res;
    if (read_one_row_nonblocking(mysql, field,
                                 async_data->async_read_metadata_data.data,
                                 async_data->async_read_metadata_field_len,
                                 &res) == NET_ASYNC_NOT_READY) {
      return NET_ASYNC_NOT_READY;
    }

    if (res == -1) {
      goto end;
    }

    if (unpack_field(mysql, alloc, false, mysql->server_capabilities,
                     &async_data->async_read_metadata_data,
                     async_data->async_read_metadata_fields +
                         async_data->async_read_metadata_cur_field)) {
      goto end;
    }
    async_data->async_read_metadata_cur_field++;
  }

  /* Read EOF packet in case of old client */
  if (!(mysql->server_capabilities & CLIENT_DEPRECATE_EOF)) {
    if (cli_safe_read_nonblocking(mysql, nullptr, &pkt_len) ==
        NET_ASYNC_NOT_READY) {
      return NET_ASYNC_NOT_READY;
    }

    if (pkt_len == packet_error) {
      goto end;
    }

    pos = net->read_pos;
    if (*pos == 254) {
      mysql->warning_count = uint2korr(pos + 1);
      mysql->server_status = uint2korr(pos + 3);
    }
  }
  *ret = async_data->async_read_metadata_fields;

end:
  async_data->async_read_metadata_field_len = nullptr;
  async_data->async_read_metadata_fields = nullptr;
  memset(&async_data->async_read_metadata_data, 0,
         sizeof(async_data->async_read_metadata_data));
  async_data->async_read_metadata_cur_field = 0;
  return NET_ASYNC_COMPLETE;
}

/**
  Read metadata resultset from server
  Memory allocated in a given allocator root.

  @param[in]    mysql           connection handle
  @param[in]    alloc           memory allocator root
  @param[in]    field_count     total number of fields
  @param[in]    field           number of columns in single field descriptor

  @retval an array of field rows

*/
MYSQL_FIELD *cli_read_metadata_ex(MYSQL *mysql, MEM_ROOT *alloc,
                                  ulong field_count, unsigned int field) {
  ulong *len;
  uint f;
  uchar *pos;
  MYSQL_FIELD *fields, *result;
  MYSQL_ROWS data;
  NET *net = &mysql->net;
  size_t size;

  DBUG_TRACE;

  len = (ulong *)alloc->Alloc(sizeof(ulong) * field);
  if (!len) {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    end_server(mysql);
    return nullptr;
  }
  size = sizeof(MYSQL_FIELD) * field_count;

  if (field_count != (size / sizeof(MYSQL_FIELD))) {
    set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
    end_server(mysql);
    return nullptr;
  }

  fields = result = (MYSQL_FIELD *)alloc->Alloc(size);
  if (!result) {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    end_server(mysql);
    return nullptr;
  }
  memset(fields, 0, sizeof(MYSQL_FIELD) * field_count);

  data.data = (MYSQL_ROW)alloc->Alloc(sizeof(char *) * (field + 1));
  memset(data.data, 0, sizeof(char *) * (field + 1));

  /*
    In this below loop we read each column info as 1 single row
    and save it in mysql->fields array
  */
  for (f = 0; f < field_count; ++f) {
    if (read_one_row(mysql, field, data.data, len) == -1) return nullptr;
    if (unpack_field(mysql, alloc, false, mysql->server_capabilities, &data,
                     fields++))
      return nullptr;
  }
  /* Read EOF packet in case of old client */
  if (!(mysql->server_capabilities & CLIENT_DEPRECATE_EOF)) {
    if (packet_error == cli_safe_read(mysql, nullptr)) return nullptr;
    pos = net->read_pos;
    if (*pos == 254) {
      mysql->warning_count = uint2korr(pos + 1);
      mysql->server_status = uint2korr(pos + 3);
    }
  }
  return result;
}

static int alloc_field_alloc(MYSQL *mysql) {
  if (mysql->field_alloc == nullptr) {
    mysql->field_alloc =
        new (my_malloc(key_memory_MYSQL, sizeof(MEM_ROOT), MYF(MY_WME)))
            MEM_ROOT(PSI_NOT_INSTRUMENTED, 8192); /* Assume rowlength < 8192 */
    if (mysql->field_alloc == nullptr) {
      set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
      return 1;
    }
  }
  /* At this point the NET is receiving a resultset. max packet should be set */
  assert(mysql->net.max_packet_size != 0);
  /* Limit the size of the columns buffer to MAX packet size or 1M */
  mysql->field_alloc->set_max_capacity(
      std::max(1024UL * 1024UL, mysql->net.max_packet_size));
  return 0;
}

/**
  Read metadata resultset from server

  @param[in]    mysql           connection handle
  @param[in]    field_count     total number of fields
  @param[in]    field           number of columns in single field descriptor

  @retval an array of field rows

*/
MYSQL_FIELD *cli_read_metadata(MYSQL *mysql, ulong field_count,
                               unsigned int field) {
  alloc_field_alloc(mysql);
  return cli_read_metadata_ex(mysql, mysql->field_alloc, field_count, field);
}

/**
  Helper method to read metadata in asynchronous way.
*/
static net_async_status cli_read_metadata_nonblocking(MYSQL *mysql,
                                                      ulong field_count,
                                                      unsigned int field,
                                                      MYSQL_FIELD **ret) {
  alloc_field_alloc(mysql);
  if (cli_read_metadata_ex_nonblocking(mysql, mysql->field_alloc, field_count,
                                       field, ret) == NET_ASYNC_NOT_READY) {
    return NET_ASYNC_NOT_READY;
  }
  return NET_ASYNC_COMPLETE;
}

/**
  Read resultset metadata returned by COM_QUERY command.

  @param[in]    mysql           Client connection handle.
  @param[in]    pos             Position in the packet where the metadata
  starts.
  @param[in]    field_count     Number of columns in the field descriptor.

  @retval       0               Success.
  @retval       1               Error.
*/
static int read_com_query_metadata(MYSQL *mysql, uchar *pos,
                                   ulong field_count) {
  /* Store resultset metadata flag. */
  if (mysql->client_flag & CLIENT_OPTIONAL_RESULTSET_METADATA) {
    mysql->resultset_metadata = static_cast<enum enum_resultset_metadata>(*pos);
  } else {
    mysql->resultset_metadata = RESULTSET_METADATA_FULL;
  }

  switch (mysql->resultset_metadata) {
    case RESULTSET_METADATA_FULL:
      /* Read metadata. */
      MYSQL_TRACE_STAGE(mysql, WAIT_FOR_FIELD_DEF);

      if (!(mysql->fields = cli_read_metadata(mysql, field_count,
                                              protocol_41(mysql) ? 7 : 5))) {
        mysql->field_alloc->Clear();
        return 1;
      }
      break;

    case RESULTSET_METADATA_NONE:
      /* Skip metadata. */
      mysql->fields = nullptr;
      break;

    default:
      /* Unknown metadata flag. */
      mysql->fields = nullptr;
      return 1;
  }

  return 0;
}

/**
  Read resultset metadata returned by COM_QUERY command in asynchronous way.

  @param[in]    mysql           Client connection handle.
  @param[in]    pos             Position in the packet where the metadata
                                starts.
  @param[in]    field_count     Number of columns in the field descriptor.
  @param[out]   res             set to false in case of success and true for
  error.

  @retval     NET_ASYNC_NOT_READY  metadata resultset not read completely
  @retval     NET_ASYNC_COMPLETE   finished reading metadata resultset
*/
static net_async_status read_com_query_metadata_nonblocking(MYSQL *mysql,
                                                            uchar *pos,
                                                            ulong field_count,
                                                            int *res) {
  DBUG_TRACE;
  /* pos is only set on the first reentrant call. */
  if (pos) {
    /* Store resultset metadata flag. */
    if (mysql->client_flag & CLIENT_OPTIONAL_RESULTSET_METADATA) {
      mysql->resultset_metadata =
          static_cast<enum enum_resultset_metadata>(*pos);
    } else {
      mysql->resultset_metadata = RESULTSET_METADATA_FULL;
    }
  }

  switch (mysql->resultset_metadata) {
    case RESULTSET_METADATA_FULL:
      /* Read metadata. */
      MYSQL_TRACE_STAGE(mysql, WAIT_FOR_FIELD_DEF);

      if (cli_read_metadata_nonblocking(
              mysql, field_count, protocol_41(mysql) ? 7 : 5, &mysql->fields) ==
          NET_ASYNC_NOT_READY) {
        return NET_ASYNC_NOT_READY;
      }

      if (!mysql->fields) {
        mysql->field_alloc->Clear();
        *res = 1;
        return NET_ASYNC_COMPLETE;
      }
      break;

    case RESULTSET_METADATA_NONE:
      /* Skip metadata. */
      mysql->fields = nullptr;
      break;

    default:
      /* Unknown metadata flag. */
      mysql->fields = nullptr;
      *res = 1;
      return NET_ASYNC_COMPLETE;
  }

  *res = 0;
  return NET_ASYNC_COMPLETE;
}

net_async_status cli_read_rows_nonblocking(MYSQL *mysql,
                                           MYSQL_FIELD *mysql_fields,
                                           unsigned int fields,
                                           MYSQL_DATA **result_out) {
  uint field;
  ulong pkt_len;
  ulong len;
  uchar *cp;
  char *to, *end_to;
  MYSQL_ROWS *cur;
  NET *net = &mysql->net;
  bool is_data_packet;
  DBUG_TRACE;
  MYSQL_ASYNC *async_context = ASYNC_DATA(mysql);
  NET_ASYNC *net_async = NET_ASYNC_DATA(net);
  *result_out = nullptr;

  if (cli_safe_read_nonblocking(mysql, &is_data_packet, &pkt_len) ==
      NET_ASYNC_NOT_READY) {
    return NET_ASYNC_NOT_READY;
  }

  mysql->packet_length = pkt_len;
  if (pkt_len == packet_error) {
    if (net_async->read_rows_is_first_read) {
      free_rows(async_context->rows_result_buffer);
      async_context->rows_result_buffer = nullptr;
    }
    net_async->read_rows_is_first_read = true;
    return NET_ASYNC_COMPLETE;
  }

  if (net_async->read_rows_is_first_read) {
    MYSQL_DATA *result;
    if (!(result =
              (MYSQL_DATA *)my_malloc(key_memory_MYSQL_DATA, sizeof(MYSQL_DATA),
                                      MYF(MY_WME | MY_ZEROFILL))) ||
        !(result->alloc =
              (MEM_ROOT *)my_malloc(key_memory_MYSQL_DATA, sizeof(MEM_ROOT),
                                    MYF(MY_WME | MY_ZEROFILL)))) {
      set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
      net_async->read_rows_is_first_read = true;
      free_rows(result);
      return NET_ASYNC_COMPLETE;
    }
    async_context->rows_result_buffer = result;
    ::new ((void *)result->alloc)
        MEM_ROOT(PSI_NOT_INSTRUMENTED, 8192); /* Assume rowlength < 8192 */
    async_context->prev_row_ptr = &result->data;
    result->rows = 0;
    result->fields = fields;

    net_async->read_rows_is_first_read = false;
  }

  /*
    The last EOF packet is either a single 254 character or (in MySQL 4.1)
    254 followed by 1-7 status bytes or an OK packet starting with 0xFE
  */
  while (*(cp = net->read_pos) == 0 || is_data_packet) {
    MYSQL_DATA *result = async_context->rows_result_buffer;
    result->rows++;
    if (!(cur = (MYSQL_ROWS *)result->alloc->Alloc(sizeof(MYSQL_ROWS))) ||
        !(cur->data = ((MYSQL_ROW)result->alloc->Alloc(
              (fields + 1) * sizeof(char *) + pkt_len)))) {
      free_rows(result);
      async_context->rows_result_buffer = nullptr;
      set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
      net_async->read_rows_is_first_read = true;
      return NET_ASYNC_COMPLETE;
    }
    *async_context->prev_row_ptr = cur;
    async_context->prev_row_ptr = &cur->next;
    to = (char *)(cur->data + fields + 1);
    end_to = to + pkt_len - 1;
    for (field = 0; field < fields; field++) {
      if ((len = (ulong)net_field_length(&cp)) ==
          NULL_LENGTH) { /* null field */
        cur->data[field] = nullptr;
      } else {
        cur->data[field] = to;
        if (to > end_to || len > (ulong)(end_to - to)) {
          free_rows(result);
          async_context->rows_result_buffer = nullptr;
          set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
          net_async->read_rows_is_first_read = true;
          return NET_ASYNC_COMPLETE;
        }
        memcpy(to, (char *)cp, len);
        to[len] = 0;
        to += len + 1;
        cp += len;
        if (mysql_fields) {
          if (mysql_fields[field].max_length < len)
            mysql_fields[field].max_length = len;
        }
      }
    }
    cur->data[field] = to; /* End of last field */
    if (cli_safe_read_nonblocking(mysql, &is_data_packet, &pkt_len) ==
        NET_ASYNC_NOT_READY) {
      return NET_ASYNC_NOT_READY;
    }
    mysql->packet_length = pkt_len;
    if (pkt_len == packet_error) {
      free_rows(async_context->rows_result_buffer);
      async_context->rows_result_buffer = nullptr;
      net_async->read_rows_is_first_read = true;
      return NET_ASYNC_COMPLETE;
    }
  }

  *async_context->prev_row_ptr = nullptr; /* last pointer is null */
  /* read EOF packet or OK packet if it is new client */
  if (pkt_len > 1) {
    if (mysql->server_capabilities & CLIENT_DEPRECATE_EOF && !is_data_packet)
      read_ok_ex(mysql, pkt_len);
    else {
      mysql->warning_count = uint2korr(cp + 1);
      mysql->server_status = uint2korr(cp + 3);
    }

    DBUG_PRINT("info", ("status: %u  warning_count:  %u", mysql->server_status,
                        mysql->warning_count));
  }

#if defined(CLIENT_PROTOCOL_TRACING)
  if (mysql->server_status & SERVER_MORE_RESULTS_EXISTS)
    MYSQL_TRACE_STAGE(mysql, WAIT_FOR_RESULT);
  else
    MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
#endif
  DBUG_PRINT("exit",
             ("Got %lu rows", (ulong)async_context->rows_result_buffer->rows));
  *result_out = async_context->rows_result_buffer;
  async_context->rows_result_buffer = nullptr;
  net_async->read_rows_is_first_read = true;
  return NET_ASYNC_COMPLETE;
}

/* Read all rows (data) from server */

MYSQL_DATA *cli_read_rows(MYSQL *mysql, MYSQL_FIELD *mysql_fields,
                          unsigned int fields) {
  uint field;
  ulong pkt_len;
  ulong len;
  uchar *cp;
  char *to, *end_to;
  MYSQL_DATA *result;
  MYSQL_ROWS **prev_ptr, *cur;
  NET *net = &mysql->net;
  bool is_data_packet;
  DBUG_TRACE;

  if ((pkt_len = cli_safe_read(mysql, &is_data_packet)) == packet_error)
    return nullptr;

  if (pkt_len == 0) return nullptr;
  if (!(result =
            (MYSQL_DATA *)my_malloc(key_memory_MYSQL_DATA, sizeof(MYSQL_DATA),
                                    MYF(MY_WME | MY_ZEROFILL))) ||
      !(result->alloc =
            (MEM_ROOT *)my_malloc(key_memory_MYSQL_DATA, sizeof(MEM_ROOT),
                                  MYF(MY_WME | MY_ZEROFILL)))) {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    free_rows(result);
    return nullptr;
  }
  ::new ((void *)result->alloc)
      MEM_ROOT(PSI_NOT_INSTRUMENTED, 8192); /* Assume rowlength < 8192 */
  prev_ptr = &result->data;
  result->rows = 0;
  result->fields = fields;

  /*
    The last EOF packet is either a single 254 character or (in MySQL 4.1)
    254 followed by 1-7 status bytes or an OK packet starting with 0xFE
  */

  while (*(cp = net->read_pos) == 0 || is_data_packet) {
    ulong packet_left = pkt_len;
    result->rows++;
    if (!(cur = (MYSQL_ROWS *)result->alloc->Alloc(sizeof(MYSQL_ROWS))) ||
        !(cur->data = ((MYSQL_ROW)result->alloc->Alloc(
              (fields + 1) * sizeof(char *) + pkt_len)))) {
      free_rows(result);
      set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
      return nullptr;
    }
    *prev_ptr = cur;
    prev_ptr = &cur->next;
    to = (char *)(cur->data + fields + 1);
    end_to = to + pkt_len - 1;
    for (field = 0; field < fields; field++) {
      uint length_len = 0;
      if (packet_left < 1 ||
          packet_left < (length_len = net_field_length_size(cp))) {
        free_rows(result);
        set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
        return nullptr;
      }
      if ((len = (ulong)net_field_length(&cp)) ==
          NULL_LENGTH) { /* null field */
        cur->data[field] = nullptr;
        packet_left -= length_len;
      } else {
        cur->data[field] = to;
        DBUG_EXECUTE_IF("simulate_invalid_packet_data", {
          to = end_to + 1;
          len = ULONG_MAX - 1;
        });
        if (to > end_to || len > (ulong)(end_to - to)) {
          free_rows(result);
          set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
          return nullptr;
        }
        memcpy(to, (char *)cp, len);
        to[len] = 0;
        to += len + 1;
        cp += len;
        packet_left -= len + length_len;
        if (mysql_fields) {
          if (mysql_fields[field].max_length < len)
            mysql_fields[field].max_length = len;
        }
      }
    }
    cur->data[field] = to; /* End of last field */
    if ((pkt_len = cli_safe_read(mysql, &is_data_packet)) == packet_error) {
      free_rows(result);
      return nullptr;
    }
  }
  *prev_ptr = nullptr; /* last pointer is null */
  /* read EOF packet or OK packet if it is new client */
  if (pkt_len > 1) {
    if (mysql->server_capabilities & CLIENT_DEPRECATE_EOF && !is_data_packet)
      read_ok_ex(mysql, pkt_len);
    else {
      mysql->warning_count = uint2korr(cp + 1);
      mysql->server_status = uint2korr(cp + 3);
    }

    DBUG_PRINT("info", ("status: %u  warning_count:  %u", mysql->server_status,
                        mysql->warning_count));
  }

#if defined(CLIENT_PROTOCOL_TRACING)
  if (mysql->server_status & SERVER_MORE_RESULTS_EXISTS)
    MYSQL_TRACE_STAGE(mysql, WAIT_FOR_RESULT);
  else
    MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
#endif
  DBUG_PRINT("exit", ("Got %lu rows", (ulong)result->rows));
  return result;
}

static int read_one_row_complete(MYSQL *mysql, ulong pkt_len,
                                 bool is_data_packet, uint fields,
                                 MYSQL_ROW row, ulong *lengths) {
  DBUG_TRACE;
  uint field;
  ulong len;
  uchar *pos, *prev_pos, *end_pos;
  NET *net = &mysql->net;

  if (net->read_pos[0] != 0x00 && !is_data_packet) {
    if (pkt_len > 1) /* MySQL 4.1 protocol */
    {
      if (mysql->server_capabilities & CLIENT_DEPRECATE_EOF)
        read_ok_ex(mysql, pkt_len);
      else {
        mysql->warning_count = uint2korr(net->read_pos + 1);
        mysql->server_status = uint2korr(net->read_pos + 3);
      }
    }
#if defined(CLIENT_PROTOCOL_TRACING)
    if (mysql->server_status & SERVER_MORE_RESULTS_EXISTS)
      MYSQL_TRACE_STAGE(mysql, WAIT_FOR_RESULT);
    else
      MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
#endif
    return 1; /* End of data */
  }
  prev_pos = nullptr; /* allowed to write at packet[-1] */
  pos = net->read_pos;
  end_pos = pos + pkt_len;
  for (field = 0; field < fields; field++) {
    if (pos >= end_pos) {
      set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
      return -1;
    }
    len = (ulong)net_field_length_checked(&pos, (ulong)(end_pos - pos));

    DBUG_EXECUTE_IF("simulate_bad_field_length_1", { len = 1000000L; });
    DBUG_EXECUTE_IF("simulate_bad_field_length_2", { len = pkt_len - 1; });
    if (pos > end_pos) {
      set_mysql_error(mysql, CR_UNKNOWN_ERROR, unknown_sqlstate);
      return -1;
    }

    if (len == NULL_LENGTH) {
      row[field] = nullptr;
      *lengths++ = 0;
    } else {
      row[field] = (char *)pos;
      pos += len;
      *lengths++ = len;
    }
    /*
     It's safe to write to prev_pos here because we already check
     for a valid pos in the beginning of this loop.
    */
    if (prev_pos) *prev_pos = 0; /* Terminate prev field */
    prev_pos = pos;
  }
  row[field] = (char *)prev_pos + 1;     /* End of last field */
  if (prev_pos < end_pos) *prev_pos = 0; /* Terminate last field */
  return 0;
}

/*
  Read one row. Uses packet buffer as storage for fields.
  When next packet is read, the previous field values are destroyed
*/

static int read_one_row(MYSQL *mysql, uint fields, MYSQL_ROW row,
                        ulong *lengths) {
  ulong pkt_len;
  bool is_data_packet;

  if ((pkt_len = cli_safe_read(mysql, &is_data_packet)) == packet_error)
    return -1;

  return read_one_row_complete(mysql, pkt_len, is_data_packet, fields, row,
                               lengths);
}

static net_async_status read_one_row_nonblocking(MYSQL *mysql, uint fields,
                                                 MYSQL_ROW row, ulong *lengths,
                                                 int *res) {
  DBUG_TRACE;
  ulong pkt_len;
  bool is_data_packet;
  net_async_status status;

  status = cli_safe_read_nonblocking(mysql, &is_data_packet, &pkt_len);
  if (status == NET_ASYNC_NOT_READY) {
    return status;
  }

  mysql->packet_length = pkt_len;
  if (pkt_len == packet_error) {
    *res = -1;
    return NET_ASYNC_COMPLETE;
  }

  *res = read_one_row_complete(mysql, pkt_len, is_data_packet, fields, row,
                               lengths);
  return NET_ASYNC_COMPLETE;
}

static bool cli_read_query_result(MYSQL *mysql);
static net_async_status cli_read_query_result_nonblocking(MYSQL *mysql);
static MYSQL_RES *cli_use_result(MYSQL *mysql);
static MYSQL_ROW cli_fetch_row(MYSQL_RES *mysql_res);
static MYSQL *cli_connect(mysql_async_connect *ctx);

int cli_read_change_user_result(MYSQL *mysql) {
  return cli_safe_read(mysql, nullptr);
}

net_async_status cli_read_change_user_result_nonblocking(MYSQL *mysql,
                                                         ulong *ret) {
  return cli_safe_read_nonblocking(mysql, nullptr, ret);
}

static MYSQL_METHODS client_methods = {
    cli_connect,
    cli_read_query_result,      /* read_query_result */
    cli_advanced_command,       /* advanced_command */
    cli_read_rows,              /* read_rows */
    cli_use_result,             /* use_result */
    cli_fetch_row,              /* fetch_row */
    cli_fetch_lengths,          /* fetch_lengths */
    cli_flush_use_result,       /* flush_use_result */
    cli_read_change_user_result /* read_change_user_result */
#ifndef MYSQL_SERVER
    ,
    cli_list_fields,         /* list_fields */
    cli_read_prepare_result, /* read_prepare_result */
    cli_stmt_execute,        /* stmt_execute */
    cli_read_binary_rows,    /* read_binary_rows */
    cli_unbuffered_fetch,    /* unbuffered_fetch */
    cli_read_statistics,     /* read_statistics */
    cli_read_query_result,   /* next_result */
    cli_read_binary_rows,    /* read_rows_from_cursor */
    free_rows
#endif
    ,
    cli_read_query_result_nonblocking,      /* read_query_result_nonblocking */
    cli_advanced_command_nonblocking,       /* advanced_command_nonblocking */
    cli_read_rows_nonblocking,              /* read_rows_nonblocking */
    cli_flush_use_result_nonblocking,       /* flush_use_result_nonblocking */
    cli_read_query_result_nonblocking,      /* next_result_nonblocking */
    cli_read_change_user_result_nonblocking /* read_change_user_result_nonblocking
                                             */
};
/****************************************************************************
  Init MySQL structure or allocate one
****************************************************************************/

MYSQL *STDCALL mysql_init(MYSQL *mysql) {
  if (mysql_server_init(0, nullptr, nullptr)) return nullptr;
  if (!mysql) {
    if (!(mysql = (MYSQL *)my_malloc(key_memory_MYSQL, sizeof(*mysql),
                                     MYF(MY_WME | MY_ZEROFILL)))) {
      set_mysql_error(nullptr, CR_OUT_OF_MEMORY, unknown_sqlstate);
      return nullptr;
    }
    mysql->free_me = true;
  } else
    memset(mysql, 0, sizeof(*(mysql)));
  mysql->charset = default_client_charset_info;
  mysql->field_alloc = (MEM_ROOT *)my_malloc(
      key_memory_MYSQL, sizeof(*mysql->field_alloc), MYF(MY_WME | MY_ZEROFILL));
  if (!mysql->field_alloc) {
    set_mysql_error(nullptr, CR_OUT_OF_MEMORY, unknown_sqlstate);
    if (mysql->free_me) my_free(mysql);
    return nullptr;
  }
  my_stpcpy(mysql->net.sqlstate, not_error_sqlstate);

  /*
    Only enable LOAD DATA INFILE by default if configured with option
    ENABLED_LOCAL_INFILE
  */

#if defined(ENABLED_LOCAL_INFILE) && !defined(MYSQL_SERVER)
  mysql->options.client_flag |= CLIENT_LOCAL_FILES;
#endif

#if defined(_WIN32)
  mysql->options.shared_memory_base_name = my_strdup(
      key_memory_mysql_options, def_shared_memory_base_name, MYF(MY_WME));
#endif

  mysql->options.report_data_truncation = true; /* default */

  /* Initialize extensions. */
  if (!(mysql->extension = mysql_extension_init(mysql))) {
    set_mysql_error(nullptr, CR_OUT_OF_MEMORY, unknown_sqlstate);
    return nullptr;
  }
  /*
    By default we don't reconnect because it could silently corrupt data (after
    reconnection you potentially lose table locks, user variables, session
    variables (transactions but they are specifically dealt with in
    mysql_reconnect()).
    This is a change: < 5.0.3 mysql->reconnect was set to 1 by default.
    How this change impacts existing apps:
    - existing apps which relied on the default will see a behaviour change;
    they will have to set reconnect=1 after mysql_real_connect().
    - existing apps which explicitly asked for reconnection (the only way they
    could do it was by setting mysql.reconnect to 1 after mysql_real_connect())
    will not see a behaviour change.
    - existing apps which explicitly asked for no reconnection
    (mysql.reconnect=0) will not see a behaviour change.
  */
  mysql->reconnect = false;
#if !defined(MYSQL_SERVER)
  ENSURE_EXTENSIONS_PRESENT(&mysql->options);
  mysql->options.extension->ssl_mode = SSL_MODE_PREFERRED;
#endif
  /* by default connection_compressed should be OFF */
  ENSURE_EXTENSIONS_PRESENT(&mysql->options);
  mysql->options.extension->connection_compressed = false;
  /*
    Set the client methods to use. It can be overridden just before
    calling the mysql_real_connect(). May be the ideal way is to pass through
    the argument but that will break the ABI.
  */
  mysql->methods = &client_methods;
  mysql->resultset_metadata = RESULTSET_METADATA_FULL;
  ASYNC_DATA(mysql)->async_op_status = ASYNC_OP_UNSET;
  return mysql;
}

/*
  MYSQL::extension handling (see sql_common.h for declaration
  of MYSQL_EXTENSION structure).
*/

MYSQL_EXTENSION *mysql_extension_init(MYSQL *mysql [[maybe_unused]]) {
  MYSQL_EXTENSION *ext;
  DBUG_TRACE;

  ext = static_cast<MYSQL_EXTENSION *>(my_malloc(
      key_memory_MYSQL, sizeof(MYSQL_EXTENSION), MYF(MY_WME | MY_ZEROFILL)));
  ext->mysql_async_context = static_cast<MYSQL_ASYNC *>(my_malloc(
      key_memory_MYSQL, sizeof(struct MYSQL_ASYNC), MYF(MY_WME | MY_ZEROFILL)));
  /* set default value */
  ext->mysql_async_context->async_op_status = ASYNC_OP_UNSET;
#ifdef MYSQL_SERVER
  ext->server_extn = nullptr;
  ext->mcs_extn = static_cast<struct mysql_command_service_extn *>(
      my_malloc(key_memory_MYSQL, sizeof(struct mysql_command_service_extn),
                MYF(MY_WME | MY_ZEROFILL)));
#endif
  DBUG_PRINT("async",
             ("set state=%d", ext->mysql_async_context->async_query_state));
  return ext;
}

void mysql_extension_bind_free(MYSQL_EXTENSION *ext) {
  DBUG_TRACE;
  if (ext->bind_info.n_params) {
    my_free(ext->bind_info.bind);
    for (uint idx = 0; idx < ext->bind_info.n_params; idx++)
      my_free(ext->bind_info.names[idx]);
    my_free(ext->bind_info.names);
  }
  memset(&ext->bind_info, 0, sizeof(ext->bind_info));
}

void mysql_extension_free(MYSQL_EXTENSION *ext) {
  if (!ext) return;
  if (ext->trace_data) my_free(ext->trace_data);
  if (ext->mysql_async_context) {
    if (ext->mysql_async_context->connect_context) {
      if (ext->mysql_async_context->connect_context
              ->scramble_buffer_allocated) {
        my_free(ext->mysql_async_context->connect_context->scramble_buffer);
        ext->mysql_async_context->connect_context->scramble_buffer = nullptr;
      }
      if (ext->mysql_async_context->connect_context->ssl) {
        SSL_free(ext->mysql_async_context->connect_context->ssl);
        ext->mysql_async_context->connect_context->ssl = nullptr;
      }
      my_free(ext->mysql_async_context->connect_context);
      ext->mysql_async_context->connect_context = nullptr;
    }
    if (ext->mysql_async_context->async_qp_data) {
      my_free(ext->mysql_async_context->async_qp_data);
      ext->mysql_async_context->async_qp_data = nullptr;
      ext->mysql_async_context->async_qp_data_length = 0;
    }
    my_free(ext->mysql_async_context);
    ext->mysql_async_context = nullptr;
  }
#ifdef MYSQL_SERVER
  if (ext->mcs_extn) my_free(ext->mcs_extn);
#endif
  // free state change related resources.
  free_state_change_info(ext);
  mysql_extension_bind_free(ext);
  my_free(ext);
}

/*
  Fill in SSL part of MYSQL structure and set 'use_ssl' flag.
  NB! Errors are not reported until you do mysql_real_connect.
*/
bool STDCALL mysql_ssl_set(MYSQL *mysql [[maybe_unused]],
                           const char *key [[maybe_unused]],
                           const char *cert [[maybe_unused]],
                           const char *ca [[maybe_unused]],
                           const char *capath [[maybe_unused]],
                           const char *cipher [[maybe_unused]]) {
  bool result = false;
  DBUG_TRACE;
  result = mysql_options(mysql, MYSQL_OPT_SSL_KEY, key) +
                   mysql_options(mysql, MYSQL_OPT_SSL_CERT, cert) +
                   mysql_options(mysql, MYSQL_OPT_SSL_CA, ca) +
                   mysql_options(mysql, MYSQL_OPT_SSL_CAPATH, capath) +
                   mysql_options(mysql, MYSQL_OPT_SSL_CIPHER, cipher)
               ? true
               : false;
  return result;
}

/*
  Free strings in the SSL structure and clear 'use_ssl' flag.
  NB! Errors are not reported until you do mysql_real_connect.
*/

static void mysql_ssl_free(MYSQL *mysql) {
  DBUG_TRACE;

  my_free(mysql->options.ssl_key);
  my_free(mysql->options.ssl_cert);
  my_free(mysql->options.ssl_ca);
  my_free(mysql->options.ssl_capath);
  my_free(mysql->options.ssl_cipher);
  if (mysql->options.extension) {
    my_free(mysql->options.extension->tls_version);
    my_free(mysql->options.extension->ssl_crl);
    my_free(mysql->options.extension->ssl_crlpath);
    my_free(mysql->options.extension->tls_ciphersuites);
    my_free(mysql->options.extension->load_data_dir);
    my_free(mysql->options.extension->tls_sni_servername);
    for (unsigned int idx = 0; idx < MAX_AUTHENTICATION_FACTOR; idx++) {
      if (mysql->options.extension->client_auth_info[idx].plugin_name) {
        my_free(mysql->options.extension->client_auth_info[idx].plugin_name);
        mysql->options.extension->client_auth_info[idx].plugin_name = nullptr;
      }
      if (mysql->options.extension->client_auth_info[idx].password) {
        my_free(mysql->options.extension->client_auth_info[idx].password);
        mysql->options.extension->client_auth_info[idx].password = nullptr;
      }
    }
  }
  mysql->options.ssl_key = nullptr;
  mysql->options.ssl_cert = nullptr;
  mysql->options.ssl_ca = nullptr;
  mysql->options.ssl_capath = nullptr;
  mysql->options.ssl_cipher = nullptr;
  if (mysql->options.extension) {
    mysql->options.extension->ssl_crl = nullptr;
    mysql->options.extension->ssl_crlpath = nullptr;
    mysql->options.extension->ssl_ctx_flags = 0;
    mysql->options.extension->tls_version = nullptr;
    mysql->options.extension->ssl_mode = SSL_MODE_DISABLED;
    mysql->options.extension->ssl_fips_mode = SSL_FIPS_MODE_OFF;
    mysql->options.extension->tls_ciphersuites = nullptr;
    mysql->options.extension->load_data_dir = nullptr;
    mysql->options.extension->tls_sni_servername = nullptr;
  }
  mysql->connector_fd = nullptr;
}

/*
  Return the SSL cipher (if any) used for current
  connection to the server.

  SYNOPSIS
    mysql_get_ssl_cipher()
      mysql pointer to the mysql connection

*/

const char *STDCALL mysql_get_ssl_cipher(MYSQL *mysql [[maybe_unused]]) {
  DBUG_TRACE;
  if (mysql->net.vio && mysql->net.vio->ssl_arg)
    return SSL_get_cipher_name((SSL *)mysql->net.vio->ssl_arg);
  return nullptr;
}

/**
  Get the current SSL session serialization

  Return the SSL session serialized (if any) used for current
  connection to the server.
  The caller needs to free the return value by calling @ref
  mysql_free_ssl_session_data()

  @param mysql pointer to the mysql connection
  @param n_ticket the ticket to return. Currently on 0 is supported.
  @param[out] out_len if a non-null is supplied, stores the length of data
  returned.
  @retval null-terminated string of the session serialization
  @retval null pointer if not an SSL connection or error
*/

void *STDCALL mysql_get_ssl_session_data(MYSQL *mysql, unsigned int n_ticket,
                                         unsigned int *out_len) {
  DBUG_TRACE;

  /* multiple TLS ticket not implemented yet */
  if (n_ticket != 0) return nullptr;

  using raii_bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
  using raii_sess = std::unique_ptr<SSL_SESSION, decltype(&SSL_SESSION_free)>;

  if (!mysql->net.vio || !mysql->net.vio->ssl_arg) {
    set_mysql_extended_error(
        mysql, CR_CANT_GET_SESSION_DATA, unknown_sqlstate,
        ER_CLIENT(CR_CANT_GET_SESSION_DATA),
        !mysql->net.vio ? "Not connected" : "Not a TLS connection");
    return nullptr;
  }
  raii_sess sess(
      SSL_get1_session(reinterpret_cast<SSL *>(mysql->net.vio->ssl_arg)),
      &SSL_SESSION_free);
  if (!sess || !SSL_SESSION_is_resumable(sess.get())) {
    set_mysql_extended_error(
        mysql, CR_CANT_GET_SESSION_DATA, unknown_sqlstate,
        ER_CLIENT(CR_CANT_GET_SESSION_DATA),
        !sess ? "no session returned" : "session returned not resumable");
    return nullptr;
  }

  raii_bio bio(BIO_new(BIO_s_mem()), &BIO_free);
  if (!bio) {
    set_mysql_extended_error(mysql, CR_CANT_GET_SESSION_DATA, unknown_sqlstate,
                             ER_CLIENT(CR_CANT_GET_SESSION_DATA),
                             "Can't create the session data encoding object");
    return nullptr;
  }
  if (0 == PEM_write_bio_SSL_SESSION(bio.get(), sess.get())) {
    set_mysql_extended_error(mysql, CR_CANT_GET_SESSION_DATA, unknown_sqlstate,
                             ER_CLIENT(CR_CANT_GET_SESSION_DATA),
                             "Can't encode the session data");
    return nullptr;
  }

  BUF_MEM *mem = nullptr;
  BIO_get_mem_ptr(bio.get(), &mem);
  if (!mem || mem->length == 0) {
    set_mysql_extended_error(mysql, CR_CANT_GET_SESSION_DATA, unknown_sqlstate,
                             ER_CLIENT(CR_CANT_GET_SESSION_DATA),
                             "Can't get a pointer to the session data");
    return nullptr;
  }
  char *ret = reinterpret_cast<char *>(
      my_malloc(key_memory_MYSQL_ssl_session_data, mem->length + 1, MYF(0)));
  memcpy(ret, mem->data, mem->length);
  ret[mem->length] = 0;
  if (out_len) *out_len = static_cast<unsigned int>(mem->length);
  return ret;
}

static SSL_SESSION *ssl_session_deserialize_from_data_ptr(MYSQL *, char *data) {
  using raii_bio = std::unique_ptr<BIO, decltype(&BIO_free)>;
  if (data != nullptr) {
    SSL_SESSION *ret = nullptr;
    raii_bio bio(BIO_new_mem_buf(data, strlen(data)), &BIO_free);
    if (!bio) return ret;
    ret = PEM_read_bio_SSL_SESSION(bio.get(), &ret, nullptr, nullptr);
    if (ret && !SSL_SESSION_is_resumable(ret)) {
      if (ret) SSL_SESSION_free(ret);
      ret = nullptr;
    }
    return ret;
  }
  return nullptr;
}

/**
  Free a saved SSL session serialization

  Frees the session serialization as returned by @ref
  mysql_get_ssl_session_data()

  @param mysql pointer to the mysql connection
  @param data the session data to dispose of
  @retval true failure
  @retval false success
*/

bool STDCALL mysql_free_ssl_session_data(MYSQL *mysql, void *data) {
  SSL_SESSION *ses = ssl_session_deserialize_from_data_ptr(
      mysql, reinterpret_cast<char *>(data));
  my_free(data);
  if (ses) {
    SSL_SESSION_free(ses);
    return false;
  } else
    return true;
}

/**
  Check if the current ssl session is reused

  @param mysql pointer to the mysql connection
  @retval false not SSL or session not reused
  @retval true session reused
*/

bool STDCALL mysql_get_ssl_session_reused(MYSQL *mysql) {
  DBUG_TRACE;
  if (mysql->net.vio && mysql->net.vio->ssl_arg) {
    return SSL_session_reused(
               reinterpret_cast<SSL *>(mysql->net.vio->ssl_arg)) != 0;
  }
  return false;
}

/*
  Check the server's (subject) Common Name against the
  hostname we connected to

  SYNOPSIS
  ssl_verify_server_cert()
    vio              pointer to a SSL connected vio
    server_hostname  name of the server that we connected to
    errptr           if we fail, we'll return (a pointer to a string
                     describing) the reason here

  RETURN VALUES
   0 Success
   1 Failed to validate server

 */

static int ssl_verify_server_cert(Vio *vio, const char *server_hostname,
                                  const char **errptr) {
  SSL *ssl;
  X509 *server_cert = nullptr;
  int ret_validation = 1;

#if !(OPENSSL_VERSION_NUMBER >= 0x10002000L)
  int cn_loc = -1;
  char *cn = nullptr;
  ASN1_STRING *cn_asn1 = nullptr;
  X509_NAME_ENTRY *cn_entry = nullptr;
  X509_NAME *subject = nullptr;
#endif

  DBUG_TRACE;
  DBUG_PRINT("enter", ("server_hostname: %s", server_hostname));

  if (!(ssl = (SSL *)vio->ssl_arg)) {
    *errptr = "No SSL pointer found";
    goto error;
  }

  if (!server_hostname) {
    *errptr = "No server hostname supplied";
    goto error;
  }

  if (!(server_cert = SSL_get_peer_certificate(ssl))) {
    *errptr = "Could not get server certificate";
    goto error;
  }

  if (X509_V_OK != SSL_get_verify_result(ssl)) {
    *errptr = "Failed to verify the server certificate";
    goto error;
  }
  /*
    We already know that the certificate exchanged was valid; the SSL library
    handled that. Now we need to verify that the contents of the certificate
    are what we expect.
  */

  /* Use OpenSSL certificate matching functions instead of our own if we
     have OpenSSL. The X509_check_* functions return 1 on success.
  */
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
  /*
    For OpenSSL 1.0.2 and up we already set certificate verification
    parameters in the new_VioSSLFd() to perform automatic checks.
  */
  ret_validation = 0;
#else  /* OPENSSL_VERSION_NUMBER < 0x10002000L */
  /*
     OpenSSL prior to 1.0.2 do not support X509_check_host() function.
     Use deprecated X509_get_subject_name() instead.
  */
  subject = X509_get_subject_name((X509 *)server_cert);
  // Find the CN location in the subject
  cn_loc = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
  if (cn_loc < 0) {
    *errptr = "Failed to get CN location in the certificate subject";
    goto error;
  }

  // Get the CN entry for given location
  cn_entry = X509_NAME_get_entry(subject, cn_loc);
  if (cn_entry == nullptr) {
    *errptr = "Failed to get CN entry using CN location";
    goto error;
  }

  // Get CN from common name entry
  cn_asn1 = X509_NAME_ENTRY_get_data(cn_entry);
  if (cn_asn1 == nullptr) {
    *errptr = "Failed to get CN from CN entry";
    goto error;
  }

  cn = (char *)ASN1_STRING_data(cn_asn1);

  // There should not be any NULL embedded in the CN
  if ((size_t)ASN1_STRING_length(cn_asn1) != strlen(cn)) {
    *errptr = "NULL embedded in the certificate CN";
    goto error;
  }

  DBUG_PRINT("info", ("Server hostname in cert: %s", cn));
  if (!strcmp(cn, server_hostname)) {
    /* Success */
    ret_validation = 0;
  }
#endif /* OPENSSL_VERSION_NUMBER >= 0x10002000L */

  *errptr = "SSL certificate validation success";

error:
  if (server_cert != nullptr) X509_free(server_cert);
  return ret_validation;
}

/*
  Note that the mysql argument must be initialized with mysql_init()
  before calling mysql_real_connect !
*/

typedef enum my_cs_match_type_enum {
  /* MySQL and OS charsets are fully compatible */
  my_cs_exact,
  /* MySQL charset is very close to OS charset  */
  my_cs_approx,
  /*
    MySQL knows this charset, but it is not supported as client character set.
  */
  my_cs_unsupp
} my_cs_match_type;

struct MY_CSET_OS_NAME {
  const char *os_name;
  const char *my_name;
  my_cs_match_type param;
};

const MY_CSET_OS_NAME charsets[] = {
#ifdef _WIN32
    {"cp437", "cp850", my_cs_approx},     {"cp850", "cp850", my_cs_exact},
    {"cp852", "cp852", my_cs_exact},      {"cp858", "cp850", my_cs_approx},
    {"cp866", "cp866", my_cs_exact},      {"cp874", "tis620", my_cs_approx},
    {"cp932", "cp932", my_cs_exact},      {"cp936", "gbk", my_cs_approx},
    {"cp949", "euckr", my_cs_approx},     {"cp950", "big5", my_cs_exact},
    {"cp1200", "utf16le", my_cs_unsupp},  {"cp1201", "utf16", my_cs_unsupp},
    {"cp1250", "cp1250", my_cs_exact},    {"cp1251", "cp1251", my_cs_exact},
    {"cp1252", "latin1", my_cs_exact},    {"cp1253", "greek", my_cs_exact},
    {"cp1254", "latin5", my_cs_exact},    {"cp1255", "hebrew", my_cs_approx},
    {"cp1256", "cp1256", my_cs_exact},    {"cp1257", "cp1257", my_cs_exact},
    {"cp10000", "macroman", my_cs_exact}, {"cp10001", "sjis", my_cs_approx},
    {"cp10002", "big5", my_cs_approx},    {"cp10008", "gb2312", my_cs_approx},
    {"cp10021", "tis620", my_cs_approx},  {"cp10029", "macce", my_cs_exact},
    {"cp12001", "utf32", my_cs_unsupp},   {"cp20107", "swe7", my_cs_exact},
    {"cp20127", "latin1", my_cs_approx},  {"cp20866", "koi8r", my_cs_exact},
    {"cp20932", "ujis", my_cs_exact},     {"cp20936", "gb2312", my_cs_approx},
    {"cp20949", "euckr", my_cs_approx},   {"cp21866", "koi8u", my_cs_exact},
    {"cp28591", "latin1", my_cs_approx},  {"cp28592", "latin2", my_cs_exact},
    {"cp28597", "greek", my_cs_exact},    {"cp28598", "hebrew", my_cs_exact},
    {"cp28599", "latin5", my_cs_exact},   {"cp28603", "latin7", my_cs_exact},
    {"cp38598", "hebrew", my_cs_exact},   {"cp51932", "ujis", my_cs_exact},
    {"cp51936", "gb2312", my_cs_exact},   {"cp51949", "euckr", my_cs_exact},
    {"cp51950", "big5", my_cs_exact},     {"cp54936", "gb18030", my_cs_exact},
    {"cp65001", "utf8mb4", my_cs_exact},

#else /* not Windows */

    {"646", "latin1", my_cs_approx}, /* Default on Solaris */
    {"ANSI_X3.4-1968", "latin1", my_cs_approx},
    {"ansi1251", "cp1251", my_cs_exact},
    {"armscii8", "armscii8", my_cs_exact},
    {"armscii-8", "armscii8", my_cs_exact},
    {"ASCII", "latin1", my_cs_approx},
    {"Big5", "big5", my_cs_exact},
    {"cp1251", "cp1251", my_cs_exact},
    {"cp1255", "hebrew", my_cs_approx},
    {"CP866", "cp866", my_cs_exact},
    {"eucCN", "gb2312", my_cs_exact},
    {"euc-CN", "gb2312", my_cs_exact},
    {"eucJP", "ujis", my_cs_exact},
    {"euc-JP", "ujis", my_cs_exact},
    {"eucKR", "euckr", my_cs_exact},
    {"euc-KR", "euckr", my_cs_exact},
    {"gb18030", "gb18030", my_cs_exact},
    {"gb2312", "gb2312", my_cs_exact},
    {"gbk", "gbk", my_cs_exact},
    {"georgianps", "geostd8", my_cs_exact},
    {"georgian-ps", "geostd8", my_cs_exact},
    {"IBM-1252", "cp1252", my_cs_exact},

    {"iso88591", "latin1", my_cs_approx},
    {"ISO_8859-1", "latin1", my_cs_approx},
    {"ISO8859-1", "latin1", my_cs_approx},
    {"ISO-8859-1", "latin1", my_cs_approx},

    {"iso885913", "latin7", my_cs_exact},
    {"ISO_8859-13", "latin7", my_cs_exact},
    {"ISO8859-13", "latin7", my_cs_exact},
    {"ISO-8859-13", "latin7", my_cs_exact},

    {"iso88592", "latin2", my_cs_exact},
    {"ISO_8859-2", "latin2", my_cs_exact},
    {"ISO8859-2", "latin2", my_cs_exact},
    {"ISO-8859-2", "latin2", my_cs_exact},

    {"iso88597", "greek", my_cs_exact},
    {"ISO_8859-7", "greek", my_cs_exact},
    {"ISO8859-7", "greek", my_cs_exact},
    {"ISO-8859-7", "greek", my_cs_exact},

    {"iso88598", "hebrew", my_cs_exact},
    {"ISO_8859-8", "hebrew", my_cs_exact},
    {"ISO8859-8", "hebrew", my_cs_exact},
    {"ISO-8859-8", "hebrew", my_cs_exact},

    {"iso88599", "latin5", my_cs_exact},
    {"ISO_8859-9", "latin5", my_cs_exact},
    {"ISO8859-9", "latin5", my_cs_exact},
    {"ISO-8859-9", "latin5", my_cs_exact},

    {"koi8r", "koi8r", my_cs_exact},
    {"KOI8-R", "koi8r", my_cs_exact},
    {"koi8u", "koi8u", my_cs_exact},
    {"KOI8-U", "koi8u", my_cs_exact},

    {"roman8", "hp8", my_cs_exact}, /* Default on HP UX */

    {"Shift_JIS", "sjis", my_cs_exact},
    {"SJIS", "sjis", my_cs_exact},
    {"shiftjisx0213", "sjis", my_cs_exact},

    {"tis620", "tis620", my_cs_exact},
    {"tis-620", "tis620", my_cs_exact},

    {"ujis", "ujis", my_cs_exact},

    {"US-ASCII", "latin1", my_cs_approx},

    {"utf8", "utf8mb4", my_cs_exact},
    {"utf-8", "utf8mb4", my_cs_exact},
#endif
    {nullptr, nullptr, my_cs_exact}};

const char *my_os_charset_to_mysql_charset(const char *csname) {
  const MY_CSET_OS_NAME *csp;
  for (csp = charsets; csp->os_name; csp++) {
    if (!my_strcasecmp(&my_charset_latin1, csp->os_name, csname)) {
      switch (csp->param) {
        case my_cs_exact:
          return csp->my_name;

        case my_cs_approx:
          /*
            Maybe we should print a warning eventually:
            character set correspondence is not exact.
          */
          return csp->my_name;

        default:
          my_printf_error(ER_UNKNOWN_ERROR,
                          "OS character set '%s'"
                          " is not supported by MySQL client",
                          MYF(0), csp->my_name);
          goto def;
      }
    }
  }

  my_printf_error(ER_UNKNOWN_ERROR, "Unknown OS character set '%s'.", MYF(0),
                  csname);

def:
  csname = MYSQL_DEFAULT_CHARSET_NAME;
  my_printf_error(ER_UNKNOWN_ERROR,
                  "Switching to the default character set '%s'.", MYF(0),
                  csname);
  return csname;
}

#ifndef _WIN32
#include <stdlib.h> /* for getenv() */
#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif
#include <locale.h>
#endif /* _WIN32 */

static int mysql_autodetect_character_set(MYSQL *mysql) {
  const char *csname = MYSQL_DEFAULT_CHARSET_NAME;

#ifdef _WIN32
  char cpbuf[64];
  {
    snprintf(cpbuf, sizeof(cpbuf), "cp%d", (int)GetConsoleCP());
    csname = my_os_charset_to_mysql_charset(cpbuf);
  }
#elif defined(HAVE_NL_LANGINFO)
  {
    if (setlocale(LC_CTYPE, "") && (csname = nl_langinfo(CODESET)))
      csname = my_os_charset_to_mysql_charset(csname);
  }
#endif

  if (mysql->options.charset_name) my_free(mysql->options.charset_name);
  if (!(mysql->options.charset_name =
            my_strdup(key_memory_mysql_options, csname, MYF(MY_WME))))
    return 1;
  return 0;
}

static void mysql_set_character_set_with_default_collation(MYSQL *mysql) {
  const char *save = charsets_dir;
  if (mysql->options.charset_dir) {
#ifdef MYSQL_SERVER
    // Do not change charsets_dir, it is not thread safe.
    assert(false);
#else
    charsets_dir = mysql->options.charset_dir;
#endif
  }
  if ((mysql->charset = get_charset_by_csname(mysql->options.charset_name,
                                              MY_CS_PRIMARY, MYF(MY_WME)))) {
    /* Try to set compiled default collation when it's possible. */
    CHARSET_INFO *collation;
    if ((collation =
             get_charset_by_name(MYSQL_DEFAULT_COLLATION_NAME, MYF(MY_WME))) &&
        my_charset_same(mysql->charset, collation)) {
      mysql->charset = collation;
    } else {
      /*
        Default compiled collation not found, or is not applicable
        to the requested character set.
        Continue with the default collation of the character set.
      */
    }
  }
  charsets_dir = save;
}

int mysql_init_character_set(MYSQL *mysql) {
  /* Set character set */
  if (!mysql->options.charset_name) {
    if (!(mysql->options.charset_name =
              my_strdup(key_memory_mysql_options, MYSQL_DEFAULT_CHARSET_NAME,
                        MYF(MY_WME))))
      return 1;
  } else if (!strcmp(mysql->options.charset_name,
                     MYSQL_AUTODETECT_CHARSET_NAME) &&
             mysql_autodetect_character_set(mysql))
    return 1;

  mysql_set_character_set_with_default_collation(mysql);

  if (!mysql->charset) {
    if (mysql->options.charset_dir)
      set_mysql_extended_error(mysql, CR_CANT_READ_CHARSET, unknown_sqlstate,
                               ER_CLIENT(CR_CANT_READ_CHARSET),
                               mysql->options.charset_name,
                               mysql->options.charset_dir);
    else {
      char cs_dir_name[FN_REFLEN];
      get_charsets_dir(cs_dir_name);
      set_mysql_extended_error(mysql, CR_CANT_READ_CHARSET, unknown_sqlstate,
                               ER_CLIENT(CR_CANT_READ_CHARSET),
                               mysql->options.charset_name, cs_dir_name);
    }
    return 1;
  }
  return 0;
}

/*********** client side authentication support **************************/

static int client_mpvio_write_packet(MYSQL_PLUGIN_VIO *, const uchar *, int);
static net_async_status client_mpvio_write_packet_nonblocking(
    struct MYSQL_PLUGIN_VIO *, const uchar *, int, int *);
static int clear_password_auth_client(MYSQL_PLUGIN_VIO *vio, MYSQL *mysql);

static auth_plugin_t clear_password_client_plugin = {
    MYSQL_CLIENT_AUTHENTICATION_PLUGIN,
    MYSQL_CLIENT_AUTHENTICATION_PLUGIN_INTERFACE_VERSION,
    "mysql_clear_password",
    MYSQL_CLIENT_PLUGIN_AUTHOR_ORACLE,
    "Clear password authentication plugin",
    {0, 1, 0},
    "GPL",
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    clear_password_auth_client,
    nullptr};

static auth_plugin_t sha256_password_client_plugin = {
    MYSQL_CLIENT_AUTHENTICATION_PLUGIN,
    MYSQL_CLIENT_AUTHENTICATION_PLUGIN_INTERFACE_VERSION,
    "sha256_password",
    MYSQL_CLIENT_PLUGIN_AUTHOR_ORACLE,
    "SHA256 based authentication with salt",
    {1, 0, 0},
    "GPL",
    nullptr,
    sha256_password_init,
    sha256_password_deinit,
    nullptr,
    nullptr,
    sha256_password_auth_client,
    sha256_password_auth_client_nonblocking};

static auth_plugin_t caching_sha2_password_client_plugin = {
    MYSQL_CLIENT_AUTHENTICATION_PLUGIN,
    MYSQL_CLIENT_AUTHENTICATION_PLUGIN_INTERFACE_VERSION,
    caching_sha2_password_plugin_name,
    MYSQL_CLIENT_PLUGIN_AUTHOR_ORACLE,
    "SHA2 based authentication with salt",
    {1, 0, 0},
    "GPL",
    nullptr,
    caching_sha2_password_init,
    caching_sha2_password_deinit,
    nullptr,
    nullptr,
    caching_sha2_password_auth_client,
    caching_sha2_password_auth_client_nonblocking};
#ifdef AUTHENTICATION_WIN
extern "C" auth_plugin_t win_auth_client_plugin;
#endif

/*
  Test trace plugin can be used only in debug builds. In non-debug ones
  it is ignored, even if it was enabled by build options (TEST_TRACE_PLUGIN
  macro).
*/

#if defined(CLIENT_PROTOCOL_TRACING) && defined(TEST_TRACE_PLUGIN) && \
    !defined(NDEBUG)
extern auth_plugin_t test_trace_plugin;
#endif

struct st_mysql_client_plugin *mysql_client_builtins[] = {
    (struct st_mysql_client_plugin *)&clear_password_client_plugin,
    (struct st_mysql_client_plugin *)&sha256_password_client_plugin,
    (struct st_mysql_client_plugin *)&caching_sha2_password_client_plugin,
#ifdef AUTHENTICATION_WIN
    (struct st_mysql_client_plugin *)&win_auth_client_plugin,
#endif
#if defined(CLIENT_PROTOCOL_TRACING) && defined(TEST_TRACE_PLUGIN) && \
    !defined(NDEBUG)
    (struct st_mysql_client_plugin *)&test_trace_plugin,
#endif
    nullptr};

static uchar *write_length_encoded_string3(uchar *buf, const char *string,
                                           size_t length) {
  buf = net_store_length(buf, length);
  memcpy(buf, string, length);
  buf += length;
  return buf;
}

/*
  The main purpose of this is to hide C++ from st_mysql_options_extention.
 */
struct My_hash {
  malloc_unordered_map<string, string> hash{key_memory_mysql_options};
};

uchar *send_client_connect_attrs(MYSQL *mysql, uchar *buf) {
  /* check if the server supports connection attributes */
  if (mysql->server_capabilities & CLIENT_CONNECT_ATTRS) {
    /* Always store the length if the client supports it */
    buf = net_store_length(
        buf, mysql->options.extension
                 ? mysql->options.extension->connection_attributes_length
                 : 0);

    /* check if we have connection attributes */
    if (mysql->options.extension &&
        mysql->options.extension->connection_attributes) {
      /* loop over and dump the connection attributes */
      for (const auto &key_and_value :
           mysql->options.extension->connection_attributes->hash) {
        const string &key = key_and_value.first;
        const string &value = key_and_value.second;

        /* we can't have zero length keys */
        assert(!key.empty());

        buf = write_length_encoded_string3(buf, key.data(), key.size());
        buf = write_length_encoded_string3(buf, value.data(), value.size());
      }
    }
  }
  return buf;
}

static size_t get_length_store_length(size_t length) {
/* as defined in net_store_length */
#define MAX_VARIABLE_STRING_LENGTH 9
  uchar length_buffer[MAX_VARIABLE_STRING_LENGTH], *ptr;

  ptr = net_store_length(length_buffer, length);

  return ptr - &length_buffer[0];
}

/*
  Write 1-8 bytes of string length header information to dest depending on
  value of src_len, then copy src_len bytes from src to dest.

 @param dest Destination buffer of size src_len+8
 @param dest_end One byte past the end of the dest buffer
 @param src Source buff of size src_len
 @param src_end One byte past the end of the src buffer

 @return pointer dest+src_len+header size or nullptr if
*/

static char *write_length_encoded_string4(char *dest, char *dest_end,
                                          const uchar *src,
                                          const uchar *src_end) {
  const size_t src_len = (size_t)(src_end - src);
  uchar *to = net_store_length((uchar *)dest, src_len);
  if ((char *)(to + src_len) >= dest_end) return nullptr;
  memcpy(to, src, src_len);
  return (char *)(to + src_len);
}

/*
  Write 1 byte of string length header information to dest and
  copy src_len bytes from src to dest.
*/
static char *write_string(char *dest, char *dest_end, const uchar *src,
                          const uchar *src_end) {
  const size_t src_len = (size_t)(src_end - src);
  uchar *to = nullptr;
  if (src_len >= 251) return nullptr;
  *dest = (uchar)src_len;
  to = (uchar *)dest + 1;
  if ((char *)(to + src_len) >= dest_end) return nullptr;
  memcpy(to, src, src_len);
  return (char *)(to + src_len);
}
/**
  Sends a @ref page_protocol_com_change_user
  with a caller provided payload

  @retval 0 ok
  @retval 1 error
*/
static int send_change_user_packet(MCPVIO_EXT *mpvio, const uchar *data,
                                   int data_len) {
  MYSQL *mysql = mpvio->mysql;
  char *buff, *end;
  int res = 1;
  const size_t connect_attrs_len =
      (mysql->server_capabilities & CLIENT_CONNECT_ATTRS &&
       mysql->options.extension)
          ? mysql->options.extension->connection_attributes_length
          : 0;

  buff = static_cast<char *>(
      my_alloca(USERNAME_LENGTH + data_len + 1 + NAME_LEN + 2 + NAME_LEN +
                connect_attrs_len + 9 /* for the length of the attrs */));

  end = strmake(buff, mysql->user, USERNAME_LENGTH) + 1;

  if (!data_len)
    *end++ = 0;
  else {
    assert(data_len <= 255);
    if (data_len > 255) {
      set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
      goto error;
    }
    *end++ = data_len;
    memcpy(end, data, data_len);
    end += data_len;
  }
  end = strmake(end, mpvio->db ? mpvio->db : "", NAME_LEN) + 1;

  if (mysql->server_capabilities & CLIENT_PROTOCOL_41) {
    int2store((uchar *)end, (ushort)mysql->charset->number);
    end += 2;
  }

  if (mysql->server_capabilities & CLIENT_PLUGIN_AUTH)
    end = strmake(end, mpvio->plugin->name, NAME_LEN) + 1;

  end = (char *)send_client_connect_attrs(mysql, (uchar *)end);

  res = simple_command(mysql, COM_CHANGE_USER, (uchar *)buff,
                       (ulong)(end - buff), 1);

error:
  return res;
}

/* clang-format off */
/**
  @page page_protocol_connection_phase_packets_protocol_ssl_request Protocol::SSLRequest:

  SSL Connection Request Packet. It is like
  @ref page_protocol_connection_phase_packets_protocol_handshake_response but is
  truncated right before username field. If server supports ::CLIENT_SSL
  capability, client can send this packet to request a secure SSL connection.
  The ::CLIENT_SSL capability flag must be set inside the SSL Connection Request Packet.

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td colspan="3">if capabilities @& ::CLIENT_PROTOCOL_41 {</td></tr>
  <tr><td>@ref a_protocol_type_int4 "int&lt;4&gt;"</td>
    <td>client_flag</td>
    <td>\ref group_cs_capabilities_flags</td></tr>
  <tr><td>@ref a_protocol_type_int4 "int&lt;4&gt;"</td>
    <td>max_packet_size</td>
    <td>maximum packet size</td></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>character_set</td>
    <td>client charset \ref a_protocol_character_set, only the lower 8-bits</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_fix "string[23]"</td>
    <td>filler</td>
    <td>filler to the size of the handhshake response packet. All 0s.</td></tr>
  <tr><td colspan="3">} else {</td></tr>
  <tr><td>@ref a_protocol_type_int2 "int&lt;2&gt;"</td>
    <td>client_flag</td>
    <td>\ref group_cs_capabilities_flags, only the lower 16 bits</td></tr>
  <tr><td>@ref a_protocol_type_int3 "int&lt;3&gt;"</td>
    <td>max_packet_size</td>
    <td>maximum packet size, 0xFFFFFF max</td></tr>
  <tr><td colspan="3">}</td></tr>
  </table>

  @sa int2store(), int3store(), int4store(), mysql_fill_packet_header()
*/
/* clang-format on */
/**
  Fill in the beginning of the client reply packet.

  Used to fill in the beginning of the client reply packet
  or the ssl request packet.

  @param       mysql     The mysql handler to operate
  @param[out]  buff      The buffer to receive the packet
  @param       buff_size The max size of the buffer. Used in debug only.
  @return                one past to where the buffer is filled

  @sa page_protocol_conn_packets_protocol_ssl_request
  send_client_reply_packet()
*/
static char *mysql_fill_packet_header(MYSQL *mysql, char *buff,
                                      size_t buff_size [[maybe_unused]]) {
  NET *net = &mysql->net;
  char *end;
  uchar *buff_p = (uchar *)buff;
  /*
    Always send CLIENT_LOCAL_FILES to the server.
    This needs to be done since the client can always decide to support
    local files even if this option is disabled by enabling the directory.
    But we can't turn it on in the client flag since it's used throughout the
    code base if the option is enabled or not.
  */
  const unsigned long client_flag = mysql->client_flag | CLIENT_LOCAL_FILES;

  if (client_flag & CLIENT_PROTOCOL_41) {
    /* 4.1 server and 4.1 client has a 32 byte option flag */
    assert(buff_size >= 32);

    int4store(buff_p, client_flag);
    int4store(buff_p + 4, net->max_packet_size);
    buff[8] = (char)mysql->charset->number;
    memset(buff + 9, 0, 32 - 9);
    end = buff + 32;
  } else {
    assert(buff_size >= 5);
    assert(client_flag <= UINT_MAX16);

    int2store(buff_p, (uint16)client_flag);
    int3store(buff_p + 2, net->max_packet_size);
    end = buff + 5;
  }
  return end;
}

/**
  Calculates client capabilities in effect (mysql->client_flag)

  Needs to be called immediately after receiving the server handshake packet.

  @param  mysql   the connection context
  @param  db      The database specified by the client app
  @param  client_flag The client flag as specified by the client app
  */

static void cli_calculate_client_flag(MYSQL *mysql, const char *db,
                                      ulong client_flag) {
  mysql->client_flag = client_flag;
  mysql->client_flag |= mysql->options.client_flag;
  mysql->client_flag |= CLIENT_CAPABILITIES;

  if (mysql->client_flag & CLIENT_MULTI_STATEMENTS)
    mysql->client_flag |= CLIENT_MULTI_RESULTS;

  if (mysql->options.extension &&
      mysql->options.extension->ssl_mode != SSL_MODE_DISABLED)
    mysql->client_flag |= CLIENT_SSL;

  if (db)
    mysql->client_flag |= CLIENT_CONNECT_WITH_DB;
  else
    mysql->client_flag &= ~CLIENT_CONNECT_WITH_DB;

  /* Remove options that server doesn't support */
  mysql->client_flag = mysql->client_flag &
                       (~(CLIENT_COMPRESS | CLIENT_SSL | CLIENT_PROTOCOL_41 |
                          CLIENT_OPTIONAL_RESULTSET_METADATA) |
                        mysql->server_capabilities);

  if (mysql->options.protocol == MYSQL_PROTOCOL_SOCKET &&
      mysql->options.extension &&
      mysql->options.extension->ssl_mode <= SSL_MODE_PREFERRED) {
    mysql->client_flag &= ~CLIENT_SSL;
    mysql->options.extension->ssl_mode = SSL_MODE_DISABLED;
  }
}

static SSL_SESSION *ssl_session_deserialize_from_data(MYSQL *mysql) {
  return ssl_session_deserialize_from_data_ptr(
      mysql,
      reinterpret_cast<char *>(mysql->options.extension->ssl_session_data));
}

/**
  Establishes SSL if requested and supported.

  @param  mysql   the connection handle
  @retval 0       success
  @retval 1       failure
*/
static int cli_establish_ssl(MYSQL *mysql) {
  NET *net = &mysql->net;

  /* Don't fallback on unencrypted connection if SSL required. */
  if (mysql->options.extension &&
      mysql->options.extension->ssl_mode >= SSL_MODE_REQUIRED &&
      !(mysql->server_capabilities & CLIENT_SSL)) {
    set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                             ER_CLIENT(CR_SSL_CONNECTION_ERROR),
                             "SSL is required but the server doesn't "
                             "support it");
    goto error;
  }

  /*
    If the ssl_mode is VERIFY_CA or VERIFY_IDENTITY, make sure that the
    connection doesn't succeed without providing the CA certificate.
  */
  if (mysql->options.extension &&
      mysql->options.extension->ssl_mode > SSL_MODE_REQUIRED &&
      !(mysql->options.ssl_ca || mysql->options.ssl_capath)) {
    set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                             ER_CLIENT(CR_SSL_CONNECTION_ERROR),
                             "CA certificate is required if ssl-mode "
                             "is VERIFY_CA or VERIFY_IDENTITY");
    goto error;
  }

  /*
    Attempt SSL connection if ssl_mode != SSL_MODE_DISABLED and the
    server supports SSL. Fallback on unencrypted connection otherwise.
  */
  if (mysql->options.extension &&
      mysql->options.extension->ssl_mode != SSL_MODE_DISABLED &&
      (mysql->server_capabilities & CLIENT_SSL)) {
    /* Do the SSL layering. */
    struct st_mysql_options *options = &mysql->options;
    struct st_VioSSLFd *ssl_fd;
    enum enum_ssl_init_error ssl_init_error = SSL_INITERR_NOERROR;
    const char *cert_error;
    unsigned long ssl_error;
    char buff[33], *end;
    const bool verify_identity =
        mysql->client_flag & CLIENT_SSL_VERIFY_SERVER_CERT;

    /* check if server supports compression else turn off client capability */
    if (!(mysql->server_capabilities & CLIENT_ZSTD_COMPRESSION_ALGORITHM))
      mysql->client_flag &= ~CLIENT_ZSTD_COMPRESSION_ALGORITHM;
    if (!(mysql->server_capabilities & CLIENT_COMPRESS))
      mysql->client_flag &= ~CLIENT_COMPRESS;

    end = mysql_fill_packet_header(mysql, buff, sizeof(buff));

    /*
    Send mysql->client_flag, max_packet_size - unencrypted otherwise
    the server does not know we want to do SSL
    */
    MYSQL_TRACE(SEND_SSL_REQUEST, mysql,
                ((size_t)(end - buff), (const unsigned char *)buff));
    if (my_net_write(net, (uchar *)buff, (size_t)(end - buff)) ||
        net_flush(net)) {
      set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                               ER_CLIENT(CR_SERVER_LOST_EXTENDED),
                               "sending connection information to server",
                               errno);
      goto error;
    }

    MYSQL_TRACE_STAGE(mysql, SSL_NEGOTIATION);

    /* Create the VioSSLConnectorFd - init SSL and load certs */
    if (!(ssl_fd = new_VioSSLConnectorFd(
              options->ssl_key, options->ssl_cert, options->ssl_ca,
              options->ssl_capath, options->ssl_cipher,
              options->extension ? options->extension->tls_ciphersuites
                                 : nullptr,
              &ssl_init_error,
              options->extension ? options->extension->ssl_crl : nullptr,
              options->extension ? options->extension->ssl_crlpath : nullptr,
              options->extension ? options->extension->ssl_ctx_flags : 0,
              verify_identity ? mysql->host : nullptr))) {
      set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                               ER_CLIENT(CR_SSL_CONNECTION_ERROR),
                               sslGetErrString(ssl_init_error));
      goto error;
    }
    mysql->connector_fd = (unsigned char *)ssl_fd;
    SSL_SESSION *ssl_session = ssl_session_deserialize_from_data(mysql);

    /* Connect to the server */
    DBUG_PRINT("info", ("IO layer change in progress..."));
    MYSQL_TRACE(SSL_CONNECT, mysql, ());
    if (sslconnect(ssl_fd, net->vio, (long)(mysql->options.connect_timeout),
                   ssl_session, &ssl_error, nullptr,
                   MYSQL_OPTIONS_EXTENSION_PTR(mysql, tls_sni_servername))) {
      char buf[512];
      ERR_error_string_n(ssl_error, buf, 512);
      buf[511] = 0;
      if (ERR_GET_REASON(ssl_error) == SSL_R_TLSV1_UNRECOGNIZED_NAME) {
        set_mysql_extended_error(mysql, CR_TLS_SERVER_NOT_FOUND,
                                 unknown_sqlstate,
                                 ER_CLIENT(CR_TLS_SERVER_NOT_FOUND), buf);
      } else {
        set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR,
                                 unknown_sqlstate,
                                 ER_CLIENT(CR_SSL_CONNECTION_ERROR), buf);
      }
      if (ssl_session != nullptr) SSL_SESSION_free(ssl_session);
      goto error;
    }
    if (ssl_session != nullptr) SSL_SESSION_free(ssl_session);
    DBUG_PRINT("info", ("IO layer change done!"));

    /* Verify server cert */
    if (verify_identity &&
        ssl_verify_server_cert(net->vio, mysql->host, &cert_error)) {
      set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                               ER_CLIENT(CR_SSL_CONNECTION_ERROR), cert_error);
      goto error;
    }

    MYSQL_TRACE(SSL_CONNECTED, mysql, ());
    MYSQL_TRACE_STAGE(mysql, AUTHENTICATE);
  }

  return 0;

error:
  return 1;
}

/**
  This function will establish asynchronous ssl connection by completing 4
  different ssl connection states. Initial state is set to SSL_NONE during
  which this functions does priliminary checks like if server supports ssl
  or not, if CA certificate is required etc. Once preliminary checks are
  done state is changed to SSL_REQUEST. In this state ssl request packet
  is sent by client. If this network IO is complete, state is changed to
  SSL_CONNECT. During SSL_CONNECT sslconnect() is called which can return
  immediately or complete SSL handshake. If it returns immediately client
  will save all SSL context in struct mysql_async_auth, so that next call
  to this function will ensure that SSL_new() is not called twice. Once
  ssl connection is established state is changed to SSL_COMPLETE.

  @param[in]    mysql           Client connection handle.
  @param[out]   res             set to false in case of success and true for
  error.

  @retval     NET_ASYNC_NOT_READY  ssl connection not yet established
  @retval     NET_ASYNC_COMPLETE   ssl connection established
*/
static net_async_status cli_establish_ssl_nonblocking(MYSQL *mysql, int *res) {
  DBUG_TRACE;
  NET *net = &mysql->net;
  NET_ASYNC *net_async = NET_ASYNC_DATA(net);
  mysql_async_connect *ctx = ASYNC_DATA(mysql)->connect_context;

  if (ctx->ssl_state == SSL_NONE) {
    /* Don't fallback on unencrypted connection if SSL required. */
    if (mysql->options.extension &&
        mysql->options.extension->ssl_mode >= SSL_MODE_REQUIRED &&
        !(mysql->server_capabilities & CLIENT_SSL)) {
      set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                               ER_CLIENT(CR_SSL_CONNECTION_ERROR),
                               "SSL is required but the server doesn't "
                               "support it");
      goto error;
    }

    /*
      If the ssl_mode is VERIFY_CA or VERIFY_IDENTITY, make sure
      that the connection doesn't succeed without providing the
      CA certificate.
    */
    if (mysql->options.extension &&
        mysql->options.extension->ssl_mode > SSL_MODE_REQUIRED &&
        !(mysql->options.ssl_ca || mysql->options.ssl_capath)) {
      set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                               ER_CLIENT(CR_SSL_CONNECTION_ERROR),
                               "CA certificate is required if ssl-mode "
                               "is VERIFY_CA or VERIFY_IDENTITY");
      goto error;
    }

    /*
      Attempt SSL connection if ssl_mode != SSL_MODE_DISABLED and
      the server supports SSL. Fallback on unencrypted
      connection otherwise.
    */
    if (!mysql->options.extension ||
        mysql->options.extension->ssl_mode == SSL_MODE_DISABLED ||
        !(mysql->server_capabilities & CLIENT_SSL)) {
      goto done;
    }
    ctx->ssl_state = SSL_REQUEST;
  }

  if (ctx->ssl_state == SSL_REQUEST) {
    char buff[33], *end;

    end = mysql_fill_packet_header(mysql, buff, sizeof(buff));

    /*
      Send mysql->client_flag, max_packet_size - unencrypted
      otherwise the server does not know we want to do SSL
    */
    MYSQL_TRACE(SEND_SSL_REQUEST, mysql,
                ((size_t)(end - buff), (const unsigned char *)buff));
    bool ret;
    if (my_net_write_nonblocking(net, (uchar *)buff, (size_t)(end - buff),
                                 &ret) == NET_ASYNC_NOT_READY) {
      return NET_ASYNC_NOT_READY;
    }

    if (ret) {
      set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                               ER_CLIENT(CR_SERVER_LOST_EXTENDED),
                               "sending connection information to server",
                               errno);
      goto error;
    }

    ctx->ssl_state = SSL_CONNECT;
  }

  if (ctx->ssl_state == SSL_CONNECT) {
    /* Do the SSL layering. */
    struct st_mysql_options *options = &mysql->options;
    struct st_VioSSLFd *ssl_fd;
    enum enum_ssl_init_error ssl_init_error;
    const char *cert_error;
    unsigned long ssl_error;
    size_t ret;
    const bool verify_identity =
        mysql->client_flag & CLIENT_SSL_VERIFY_SERVER_CERT;

    MYSQL_TRACE_STAGE(mysql, SSL_NEGOTIATION);

    if (!mysql->connector_fd) {
      const long flags =
          options->extension ? options->extension->ssl_ctx_flags : 0;

      /* Create the VioSSLConnectorFd - init SSL and load certs */
      if (!(ssl_fd = new_VioSSLConnectorFd(
                options->ssl_key, options->ssl_cert, options->ssl_ca,
                options->ssl_capath, options->ssl_cipher,
                options->extension ? options->extension->tls_ciphersuites
                                   : nullptr,
                &ssl_init_error,
                options->extension ? options->extension->ssl_crl : nullptr,
                options->extension ? options->extension->ssl_crlpath : nullptr,
                flags, verify_identity ? mysql->host : nullptr))) {
        set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR,
                                 unknown_sqlstate,
                                 ER_CLIENT(CR_SSL_CONNECTION_ERROR),
                                 sslGetErrString(ssl_init_error));
        goto error;
      }
      mysql->connector_fd = (unsigned char *)ssl_fd;
    } else {
      ssl_fd = (struct st_VioSSLFd *)mysql->connector_fd;
    }
    SSL_SESSION *ssl_session = ssl_session_deserialize_from_data(mysql);

    /* Connect to the server */
    DBUG_PRINT("info", ("IO layer change in progress..."));
    MYSQL_TRACE(SSL_CONNECT, mysql, ());
    if ((ret = sslconnect(
             ssl_fd, net->vio, (long)(mysql->options.connect_timeout),
             ssl_session, &ssl_error, &ctx->ssl,
             MYSQL_OPTIONS_EXTENSION_PTR(mysql, tls_sni_servername)))) {
      if (ssl_session != nullptr) SSL_SESSION_free(ssl_session);
      switch (ret) {
        case VIO_SOCKET_WANT_READ:
          net_async->async_blocking_state = NET_NONBLOCKING_READ;
          return NET_ASYNC_NOT_READY;
        case VIO_SOCKET_WANT_WRITE:
          net_async->async_blocking_state = NET_NONBLOCKING_WRITE;
          return NET_ASYNC_NOT_READY;
        default:
          break;
          /* continue for error handling */
      }

      char buf[512];
      ERR_error_string_n(ssl_error, buf, 512);
      buf[511] = 0;
      if (ERR_GET_REASON(ssl_error) == SSL_R_TLSV1_UNRECOGNIZED_NAME) {
        set_mysql_extended_error(mysql, CR_TLS_SERVER_NOT_FOUND,
                                 unknown_sqlstate,
                                 ER_CLIENT(CR_TLS_SERVER_NOT_FOUND), buf);

      } else {
        set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR,
                                 unknown_sqlstate,
                                 ER_CLIENT(CR_SSL_CONNECTION_ERROR), buf);
      }
      goto error;
    }
    if (ssl_session != nullptr) SSL_SESSION_free(ssl_session);
    DBUG_PRINT("info", ("IO layer change done!"));

    /* sslconnect creates a new vio, so update it. */
    vio_set_blocking_flag(net->vio, !ctx->non_blocking);

    /* Verify server cert */
    if (verify_identity &&
        ssl_verify_server_cert(net->vio, mysql->host, &cert_error)) {
      set_mysql_extended_error(mysql, CR_SSL_CONNECTION_ERROR, unknown_sqlstate,
                               ER_CLIENT(CR_SSL_CONNECTION_ERROR), cert_error);
      goto error;
    }

    MYSQL_TRACE(SSL_CONNECTED, mysql, ());
    MYSQL_TRACE_STAGE(mysql, AUTHENTICATE);
  }

done:
  *res = 0;
  ctx->ssl_state = SSL_COMPLETE;
  return NET_ASYNC_COMPLETE;

error:
  *res = 1;
  ctx->ssl_state = SSL_COMPLETE;
  return NET_ASYNC_COMPLETE;
}

/**
  Asynchronous authentication phase is divided into several smaller chunks
  of subtasks like:
  1. Determine the default/initial plugin to use
  2. Call authentication plugin API
  3. Handle response from authentication plugin API
  4. Check if server asked to use a different authentication plugin
  5. In case server asked to use a different authentication plugin
     use that plugin to start the authentication process again.
  6. Complete authentication.

  All above tasks are implemented in below authsm_* functions where
  authsm stads for authentication state machine.
*/
static mysql_state_machine_status authsm_begin_plugin_auth(
    mysql_async_auth *ctx);
static mysql_state_machine_status authsm_run_first_authenticate_user(
    mysql_async_auth *ctx);
static mysql_state_machine_status authsm_handle_first_authenticate_user(
    mysql_async_auth *ctx);
static mysql_state_machine_status authsm_read_change_user_result(
    mysql_async_auth *ctx);
static mysql_state_machine_status authsm_handle_change_user_result(
    mysql_async_auth *ctx);
static mysql_state_machine_status authsm_run_second_authenticate_user(
    mysql_async_auth *ctx);
static mysql_state_machine_status authsm_handle_second_authenticate_user(
    mysql_async_auth *ctx);
static mysql_state_machine_status authsm_finish_auth(mysql_async_auth *ctx);
static mysql_state_machine_status authsm_init_multi_auth(mysql_async_auth *ctx);
static mysql_state_machine_status authsm_do_multi_plugin_auth(
    mysql_async_auth *ctx);
static mysql_state_machine_status authsm_handle_multi_auth_response(
    mysql_async_auth *ctx);

/**
  Asynchronous connection phase is divided into several smaller modules
  where wach module does following:
  1. Begin the connection to the server, including any DNS resolution
     necessary, socket configuration, etc
  2. Complete the connection itself
  3. Connection established, read the first packet
  4. Parse the handshake from the server
  5. Establish SSL connection if needed
  6. Invoke the plugin to send the authentication data to the server
  7. Authenticated, set initial database if specified
  8. Send COM_INIT_DB.
  9. Prepare to send a sequence of init commands.
  10.Send an init command.

  Below are the modules which does all above tasks.
*/
static mysql_state_machine_status csm_begin_connect(mysql_async_connect *ctx);
static mysql_state_machine_status csm_complete_connect(
    mysql_async_connect *ctx);
static mysql_state_machine_status csm_wait_connect(mysql_async_connect *ctx);
static mysql_state_machine_status csm_read_greeting(mysql_async_connect *ctx);
static mysql_state_machine_status csm_parse_handshake(mysql_async_connect *ctx);
static mysql_state_machine_status csm_establish_ssl(mysql_async_connect *ctx);
static mysql_state_machine_status csm_authenticate(mysql_async_connect *ctx);
static mysql_state_machine_status csm_prep_select_database(
    mysql_async_connect *ctx);
#ifndef MYSQL_SERVER
static mysql_state_machine_status csm_prep_init_commands(
    mysql_async_connect *ctx);
static mysql_state_machine_status csm_send_one_init_command(
    mysql_async_connect *ctx);
#endif

#define MAX_CONNECTION_ATTR_STORAGE_LENGTH 65536

int mysql_get_socket_descriptor(MYSQL *mysql) {
  if (mysql && mysql->net.vio) {
    return vio_fd(mysql->net.vio);
  }
  return -1;
}

connect_stage STDCALL mysql_get_connect_nonblocking_stage(MYSQL *mysql) {
  static const std::map<csm_function, connect_stage> stages = {
    {csm_begin_connect, CONNECT_STAGE_NET_BEGIN_CONNECT},
    {csm_complete_connect, CONNECT_STAGE_NET_COMPLETE_CONNECT},
    {csm_wait_connect, CONNECT_STAGE_NET_WAIT_CONNECT},
    {csm_read_greeting, CONNECT_STAGE_READ_GREETING},
    {csm_parse_handshake, CONNECT_STAGE_PARSE_HANDSHAKE},
    {csm_establish_ssl, CONNECT_STAGE_ESTABLISH_SSL},
    {csm_authenticate, CONNECT_STAGE_AUTHENTICATE},
    {csm_prep_select_database, CONNECT_STAGE_PREP_SELECT_DATABASE},
#if !defined(MYSQL_SERVER)
    {csm_prep_init_commands, CONNECT_STAGE_PREP_INIT_COMMANDS},
    {csm_send_one_init_command, CONNECT_STAGE_SEND_ONE_INIT_COMMAND},
#endif
  };

  static const std::map<authsm_function, connect_stage> auth_stages = {
      {authsm_begin_plugin_auth, CONNECT_STAGE_AUTH_BEGIN},
      {authsm_run_first_authenticate_user,
       CONNECT_STAGE_AUTH_RUN_FIRST_AUTHENTICATE_USER},
      {authsm_handle_first_authenticate_user,
       CONNECT_STAGE_AUTH_HANDLE_FIRST_AUTHENTICATE_USER},
      {authsm_read_change_user_result,
       CONNECT_STAGE_AUTH_READ_CHANGE_USER_RESULT},
      {authsm_handle_change_user_result,
       CONNECT_STAGE_AUTH_HANDLE_CHANGE_USER_REQUEST},
      {authsm_run_second_authenticate_user,
       CONNECT_STAGE_AUTH_RUN_SECOND_AUTHENTICATE_USER},
      {authsm_init_multi_auth, CONNECT_STAGE_AUTH_INIT_MULTI_AUTH},
      {authsm_finish_auth, CONNECT_STAGE_AUTH_FINISH_AUTH},
      {authsm_handle_second_authenticate_user,
       CONNECT_STAGE_AUTH_HANDLE_SECOND_AUTHENTICATE_USER},
      {authsm_do_multi_plugin_auth, CONNECT_STAGE_AUTH_DO_MULTI_PLUGIN_AUTH},
      {authsm_handle_multi_auth_response,
       CONNECT_STAGE_AUTH_HANDLE_MULTI_AUTH_RESPONSE}};

  if (mysql) {
    NET *net = &mysql->net;
    if (!net->vio) {
      /* Seems the first stage hasn't started yet or it was unsuccessful, and
        needs to be restarted so return the 1st stage */
      return CONNECT_STAGE_NOT_STARTED;
    }

    mysql_async_connect *ctx = ASYNC_DATA(mysql)->connect_context;
    if (!ctx) {
      // If context is null and vio->net is set, means connection is complete
      return CONNECT_STAGE_COMPLETE;
    }

    // Do the more detailed authentication stage
    if (ctx->state_function == csm_authenticate &&
        ctx->auth_context != nullptr &&
        ctx->auth_context->state_function != nullptr) {
      auto search = auth_stages.find(ctx->auth_context->state_function);
      assert(search != auth_stages.end());
      if (search != auth_stages.end()) return search->second;
    }

    auto search = stages.find(ctx->state_function);
    assert(search != stages.end());
    if (search != stages.end()) {
      return search->second;
    }
  }
  return CONNECT_STAGE_INVALID;
}
/* clang-format off */
/**
  @page page_protocol_connection_phase_packets_protocol_handshake_response Protocol::HandshakeResponse:

  Depending on the servers support for the ::CLIENT_PROTOCOL_41 capability and
  the clients understanding of that flag the client has to send either
  a @ref sect_protocol_connection_phase_packets_protocol_handshake_response320 or
  @ref sect_protocol_connection_phase_packets_protocol_handshake_response41.

  @sa send_client_reply_packet

  @section sect_protocol_connection_phase_packets_protocol_handshake_response320 Protocol::HandshakeResponse320

  Old Handshake Response Packet used by old clients or if the server doesn't
  support ::CLIENT_PROTOCOL_41 @ref group_cs_capabilities_flags flag.

  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int2 "int&lt;2&gt;"</td>
    <td>client_flag</td>
    <td>\ref group_cs_capabilities_flags, only the lower 16 bits. ::CLIENT_PROTOCOL_41 should never be set</td></tr>
  <tr><td>@ref a_protocol_type_int3 "int&lt;3&gt;"</td>
    <td>max_packet_size</td>
    <td>maximum packet size, 0xFFFFFF max</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string&lt;NUL&gt;"</td>
      <td>username</td>
      <td>login user name</td></tr>
  <tr><td colspan="3">if capabilities @& ::CLIENT_CONNECT_WITH_DB {</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string&lt;NUL&gt;"</td>
      <td>auth-response</td>
      <td>Opaque authentication response data generated by
          Authentication Method indicated by the plugin name field.</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string&lt;NUL&gt;"</td>
      <td>database</td>
      <td>initial database for the connection.
      This string should be interpreted using the character set indicated by
      character set field.</td></tr>
  <tr><td colspan="3">} else {</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_eof "string&lt;EOF&gt;"</td>
      <td>auth-response</td>
      <td>Opaque authentication response data generated by
          Authentication Method indicated by the plugin name field.</td></tr>
  <tr><td colspan="3">}</td></tr>
  </table>

  Example
  ========

  ~~~~~~~~~~~~~~~~~~~~~
  11 00 00 01 85 24 00 00    00 6f 6c 64 00 47 44 53    .....$...old.GDS
  43 51 59 52 5f                                        CQYR_
  ~~~~~~~~~~~~~~~~~~~~~

  @note If auth-response is followed by a database field it must be
  NULL terminated.

  @section sect_protocol_connection_phase_packets_protocol_handshake_response41 Protocol::HandshakeResponse41

  Handshake Response Packet sent by 4.1+ clients supporting
  ::CLIENT_PROTOCOL_41 @ref group_cs_capabilities_flags flag,
  if the server announced it in its
  @ref page_protocol_connection_phase_packets_protocol_handshake.
  Otherwise (talking to an old server) the
  @ref sect_protocol_connection_phase_packets_protocol_handshake_response320
  packet must be used.


  <table>
  <caption>Payload</caption>
  <tr><th>Type</th><th>Name</th><th>Description</th></tr>
  <tr><td>@ref a_protocol_type_int4 "int&lt;4&gt;"</td>
    <td>client_flag</td>
    <td>\ref group_cs_capabilities_flags, ::CLIENT_PROTOCOL_41 always set.</td></tr>
  <tr><td>@ref a_protocol_type_int4 "int&lt;4&gt;"</td>
    <td>max_packet_size</td>
    <td>maximum packet size</td></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>character_set</td>
    <td>client charset \ref a_protocol_character_set, only the lower 8-bits</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_fix "string[23]"</td>
    <td>filler</td>
    <td>filler to the size of the handhshake response packet. All 0s.</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string&lt;NUL&gt;"</td>
      <td>username</td>
      <td>login user name</td></tr>
  <tr><td colspan="3">if capabilities @& ::CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA {</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_le "string&lt;length&gt;"</td>
      <td>auth_response</td>
      <td>opaque authentication response data generated by
      Authentication Method indicated by the plugin name field. </td></tr>
  <tr><td colspan="3">} else {</td></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>auth_response_length</td>
    <td>length of auth_response</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_le "string&lt;length&gt;"</td>
      <td>auth_response</td>
      <td>opaque authentication response data generated by
      Authentication Method indicated by the plugin name field. </td></tr>
  <tr><td colspan="3">}</td></tr>
  <tr><td colspan="3">if capabilities @& ::CLIENT_CONNECT_WITH_DB {</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string&lt;NUL&gt;"</td>
      <td>database</td>
      <td>initial database for the connection.
      This string should be interpreted using the character set indicated by
      character set field.</td></tr>
  <tr><td colspan="3">}</td></tr>
  <tr><td colspan="3">if capabilities @& ::CLIENT_PLUGIN_AUTH {</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_null "string&lt;NUL&gt;"</td>
      <td>client_plugin_name</td>
      <td>the Authentication Method used by the client to generate
      auth-response value in this packet. This is an UTF-8 string. </td></tr>
  <tr><td colspan="3">}</td></tr>
  <tr><td colspan="3">if capabilities @& ::CLIENT_CONNECT_ATTRS {</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_int_le "int&lt;lenenc&gt;"</td>
      <td>length of all key-values</td>
      <td>affected rows</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_le "string&lt;lenenc&gt;"</td>
      <td>key1</td>
      <td>Name of the 1st client attribute</td></tr>
  <tr><td>@ref sect_protocol_basic_dt_string_le "string&lt;lenenc&gt;"</td>
      <td>value1</td>
      <td>Value of the 1st client attribute</td></tr>
  <tr><td colspan="3">.. (if more data in length of all key-values, more keys and values parts)</td></tr>
  <tr><td colspan="3">}</td></tr>
  <tr><td colspan="3">if capabilities @& ::CLIENT_ZSTD_COMPRESSION_ALGORITHM {</td></tr>
  <tr><td>@ref a_protocol_type_int1 "int&lt;1&gt;"</td>
    <td>zstd_compression_level</td>
    <td>compression level for zstd compression algorithm</td></tr>
  <tr><td colspan="3">}</td></tr>
  </table>

  Example
  ========

  On MySQL 5.5.8 with ::CLIENT_PROTOCOL_41 ::CLIENT_PLUGIN_AUTH, CLIENT_SECURE_CONNECTION (removed in 8.0),
  and ::CLIENT_CONNECT_WITH_DB set, it may look like:

  ~~~~~~~~~~~~~~~~~~~~~
  54 00 00 01 8d a6 0f 00    00 00 00 01 08 00 00 00    T...............
  00 00 00 00 00 00 00 00    00 00 00 00 00 00 00 00    ................
  00 00 00 00 70 61 6d 00    14 ab 09 ee f6 bc b1 32    ....pam........2
  3e 61 14 38 65 c0 99 1d    95 7d 75 d4 47 74 65 73    >a.8e....}u.Gtes
  74 00 6d 79 73 71 6c 5f    6e 61 74 69 76 65 5f 70    t.mysql_native_p
  61 73 73 77 6f 72 64 00                               assword.
  ~~~~~~~~~~~~~~~~~~~~~

  Starting with MySQL 5.6.6 the client may send attributes
  if ::CLIENT_CONNECT_ATTRS is set:

  ~~~~~~~~~~~~~~~~~~~~~
  b2 00 00 01 85 a2 1e 00    00 00 00 40 08 00 00 00    ...........@....
  00 00 00 00 00 00 00 00    00 00 00 00 00 00 00 00    ................
  00 00 00 00 72 6f 6f 74    00 14 22 50 79 a2 12 d4    ....root.."Py...
  e8 82 e5 b3 f4 1a 97 75    6b c8 be db 9f 80 6d 79    .......uk.....my
  73 71 6c 5f 6e 61 74 69    76 65 5f 70 61 73 73 77    sql_native_passw
  6f 72 64 00 61 03 5f 6f    73 09 64 65 62 69 61 6e    ord.a._os.debian
  36 2e 30 0c 5f 63 6c 69    65 6e 74 5f 6e 61 6d 65    6.0._client_name
  08 6c 69 62 6d 79 73 71    6c 04 5f 70 69 64 05 32    .libmysql._pid.2
  32 33 34 34 0f 5f 63 6c    69 65 6e 74 5f 76 65 72    2344._client_ver
  73 69 6f 6e 08 35 2e 36    2e 36 2d 6d 39 09 5f 70    sion.5.6.6-m9._p
  6c 61 74 66 6f 72 6d 06    78 38 36 5f 36 34 03 66    latform.x86_64.f
  6f 6f 03 62 61 72                                     oo.bar
  ~~~~~~~~~~~~~~~~~~~~~

  @warning Currently, multibyte character sets such as UCS2, UTF16 and
  UTF32 are not supported.

  @note If client wants to have a secure SSL connection and sets
  CLIENT_SSL flag it should first send the
  @ref page_protocol_connection_phase_packets_protocol_ssl_request packet
  and only then, after establishing the secure connection, it should send
  the @ref page_protocol_connection_phase_packets_protocol_handshake_response
  packet.
*/
/* clang-format on */

/**
  sends a client authentication packet (second packet in the 3-way handshake)

  @param mpvio      The connection to use
  @param data       The scramble to send
  @param data_len   Length of data
  @param buff_out   Buffer holding client handshake packet
  @param buff_len   Length of buffer holding client handshake packet
  @retval 0 ok
  @retval 1 error

  @sa mysql_fill_packet_header()
  page_protocol_conn_packets_protocol_handshake_response
*/
static bool prep_client_reply_packet(MCPVIO_EXT *mpvio, const uchar *data,
                                     int data_len, char **buff_out,
                                     int *buff_len) {
  DBUG_TRACE;
  MYSQL *mysql = mpvio->mysql;
  // Simulate mismatching ssl_charset_code number in handshake response
  // In order to have different, but valid number using  my_charset_* objects
  DBUG_EXECUTE_IF("simulate_bad_ssl_charset_code", {
    if (mysql->charset->number != my_charset_bin.number)
      mysql->charset->number = my_charset_bin.number;
    else
      mysql->charset->number = my_charset_latin1.number;
  });
  char *buff, *end;
  size_t buff_size;
  const size_t connect_attrs_len =
      (mysql->server_capabilities & CLIENT_CONNECT_ATTRS &&
       mysql->options.extension)
          ? mysql->options.extension->connection_attributes_length
          : 0;
  unsigned int compress_level = 0;
  const bool server_zstd =
      (mysql->server_capabilities & CLIENT_ZSTD_COMPRESSION_ALGORITHM);
  const bool client_zstd =
      (mysql->options.client_flag & CLIENT_ZSTD_COMPRESSION_ALGORITHM);

  /* validate compression configuration */
  ENSURE_EXTENSIONS_PRESENT(&mysql->options);
  if (mysql->options.extension->compression_algorithm) {
    const std::string algorithm =
        mysql->options.extension->compression_algorithm;
    if (!algorithm.empty() &&
        validate_compression_attributes(algorithm, std::string(), true)) {
      set_mysql_error(mysql, CR_COMPRESSION_WRONGLY_CONFIGURED,
                      unknown_sqlstate);
      return true;
    }
  }

  /**
    If server/client is configured to use zstd compression then set compression
    level if specified, else set level to a default value.
  */
  if (server_zstd && client_zstd) {
    if (mysql->options.extension &&
        mysql->options.extension->zstd_compression_level)
      compress_level = mysql->options.extension->zstd_compression_level;
    else
      compress_level = default_zstd_compression_level;
  }
  /* Remove those compression capabilities that server does not support. */
  if (!(mysql->server_capabilities & CLIENT_COMPRESS))
    mysql->client_flag &= ~CLIENT_COMPRESS;
  if (!(mysql->server_capabilities & CLIENT_ZSTD_COMPRESSION_ALGORITHM))
    mysql->client_flag &= ~CLIENT_ZSTD_COMPRESSION_ALGORITHM;
  /*
   If server and client have no compression algorithms in common, we must
   fall back to uncompressed. In that case, check that uncompressed is
   allowed by client.
  */
  if (!(mysql->client_flag & CLIENT_COMPRESS) &&
      !(mysql->client_flag & CLIENT_ZSTD_COMPRESSION_ALGORITHM) &&
      mysql->options.extension->connection_compressed) {
    set_mysql_error(mysql, CR_COMPRESSION_WRONGLY_CONFIGURED, unknown_sqlstate);
    return true;
  }
  assert(connect_attrs_len < MAX_CONNECTION_ATTR_STORAGE_LENGTH);

  *buff_out = nullptr;
  *buff_len = 0;

  /*
    Fixed size of the packet is 32 bytes. See mysql_fill_packet_header.
    +9 because data is a length encoded binary where meta data size is max 9.
  */
  buff_size = 33 + USERNAME_LENGTH + data_len + 9 + NAME_LEN + NAME_LEN +
              connect_attrs_len + 9 + ((server_zstd && client_zstd) ? 1 : 0);

  buff = static_cast<char *>(
      my_malloc(PSI_NOT_INSTRUMENTED, buff_size, MYF(MY_WME | MY_ZEROFILL)));

  /* The client_flags is already calculated. Just fill in the packet header */
  end = mysql_fill_packet_header(mysql, buff, buff_size);

  DBUG_PRINT(
      "info",
      ("Server version = '%s'  capabilites: %lu  status: %u  client_flag: %lu",
       mysql->server_version, mysql->server_capabilities, mysql->server_status,
       mysql->client_flag));

  static_assert(MYSQL_USERNAME_LENGTH == USERNAME_LENGTH, "");

  /* This needs to be changed as it's not useful with big packets */
  if (mysql->user[0])
    strmake(end, mysql->user, USERNAME_LENGTH);
  else {
#if defined(KERBEROS_LIB_CONFIGURED)
    /*
      Kerberos user name should have already read inside LDAP SASL client
      plugin. If it is still empty we should return error.
    */
    if (strcmp(mpvio->plugin->name, "authentication_ldap_sasl_client") == 0) {
      if (!mysql->user[0]) {
        set_mysql_error(mysql, CR_KERBEROS_USER_NOT_FOUND, unknown_sqlstate);
        goto error;
      }
    } else
#endif
      read_user_name(end);
  }

  /* We have to handle different version of handshake here */
  DBUG_PRINT("info", ("user: %s", end));
  end = strend(end) + 1;
  if (data_len) {
    /*
      Since the older versions of server do not have
      CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA capability,
      a check is performed on this before sending auth data.
      If lenenc support is not available, the data is sent
      in the format of first byte representing the length of
      the string followed by the actual string.
      */
    if (mysql->server_capabilities & CLIENT_PLUGIN_AUTH_LENENC_CLIENT_DATA)
      end = write_length_encoded_string4(end, (char *)(buff + buff_size), data,
                                         data + data_len);
    else
      end =
          write_string(end, (char *)(buff + buff_size), data, data + data_len);
    if (end == nullptr) goto error;
  } else
    *end++ = 0;

  /* Add database if needed */
  if (mpvio->db && (mysql->server_capabilities & CLIENT_CONNECT_WITH_DB)) {
    end = strmake(end, mpvio->db, NAME_LEN) + 1;
    mysql->db = my_strdup(key_memory_MYSQL, mpvio->db, MYF(MY_WME));
  }

  if (mysql->server_capabilities & CLIENT_PLUGIN_AUTH)
    end = strmake(end, mpvio->plugin->name, NAME_LEN) + 1;

  end = (char *)send_client_connect_attrs(mysql, (uchar *)end);

  if (server_zstd && client_zstd) {
    /* send compression level if both client and server support it */
    *end = static_cast<unsigned char>(compress_level);
    end++;
  }

  *buff_out = buff;
  *buff_len = end - buff;

  return false;

error:
  my_free(buff);
  return true;
}

static int send_client_reply_packet(MCPVIO_EXT *mpvio, const uchar *data,
                                    int data_len) {
  DBUG_TRACE;
  MYSQL *mysql = mpvio->mysql;
  NET *net = &mysql->net;
  char *buff = nullptr, *end = nullptr;
  int buff_len;
  int ret = 0;
  bool prep_err;

  prep_err = prep_client_reply_packet(mpvio, data, data_len, &buff, &buff_len);
  if (prep_err) {
    return 1;
  }

  end = buff + buff_len;
  /* Write authentication package */
  MYSQL_TRACE(SEND_AUTH_RESPONSE, mysql,
              ((size_t)(end - buff), (const unsigned char *)buff));
  if (my_net_write(net, (uchar *)buff, (size_t)(end - buff)) ||
      net_flush(net)) {
    set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                             ER_CLIENT(CR_SERVER_LOST_EXTENDED),
                             "sending authentication information", errno);
    ret = 1;
  }
  MYSQL_TRACE(PACKET_SENT, mysql, ((size_t)(end - buff)));
  my_free(buff);
  return ret;
}

static net_async_status send_client_reply_packet_nonblocking(MCPVIO_EXT *mpvio,
                                                             const uchar *pkt,
                                                             int pkt_len,
                                                             bool *result) {
  DBUG_TRACE;
  MYSQL *mysql = mpvio->mysql;
  mysql_async_auth *ctx = ASYNC_DATA(mysql)->connect_context->auth_context;
  net_async_status status;

  bool error = false;
  if (!ctx->change_user_buff) {
    error =
        prep_client_reply_packet(mpvio, pkt, pkt_len, &ctx->change_user_buff,
                                 &ctx->change_user_buff_len);
    if (error) {
      goto end;
    }
  }

  status = my_net_write_nonblocking(&mysql->net, (uchar *)ctx->change_user_buff,
                                    ctx->change_user_buff_len, &error);

  if (status == NET_ASYNC_NOT_READY) {
    return NET_ASYNC_NOT_READY;
  }

end:
  *result = error;
  my_free(ctx->change_user_buff);
  ctx->change_user_buff = nullptr;

  return NET_ASYNC_COMPLETE;
}

#ifdef __clang__
// Clang UBSAN false positive?
// Call to function through pointer to incorrect function type
static int client_mpvio_read_packet(MYSQL_PLUGIN_VIO *mpv,
                                    uchar **buf) SUPPRESS_UBSAN;
#endif  // __clang__

/**
  vio->read_packet() callback method for client authentication plugins

  This function is called by a client authentication plugin, when it wants
  to read data from the server.
*/
static int client_mpvio_read_packet(MYSQL_PLUGIN_VIO *mpv, uchar **buf) {
  MCPVIO_EXT *mpvio = (MCPVIO_EXT *)mpv;
  MYSQL *mysql = mpvio->mysql;
  ulong pkt_len;

  /* there are cached data left, feed it to a plugin */
  if (mpvio->cached_server_reply.pkt_received) {
    *buf = mpvio->cached_server_reply.pkt;
    mpvio->cached_server_reply.pkt = nullptr;
    mpvio->packets_read++;
    pkt_len = mpvio->cached_server_reply.pkt_len;
    mpvio->cached_server_reply.pkt_len = 0;
    mpvio->cached_server_reply.pkt_received = false;
    return pkt_len;
  }

  if (mpvio->packets_read == 0) {
    /*
      the server handshake packet came from the wrong plugin,
      or it's mysql_change_user(). Either way, there is no data
      for a plugin to read. send a dummy packet to the server
      to initiate a dialog.
    */
    if (client_mpvio_write_packet(mpv, nullptr, 0)) return (int)packet_error;
  }

  /* otherwise read the data */
  pkt_len = (*mysql->methods->read_change_user_result)(mysql);

  /* error while reading the change user request */
  if (pkt_len == packet_error) return (int)packet_error;

  mpvio->last_read_packet_len = pkt_len;
  *buf = mysql->net.read_pos;

  /* was it a request to change plugins ? */
  if (**buf == 254)
    return (int)packet_error; /* if yes, this plugin shan't continue */

  /*
    the server sends \1\255 or \1\254 instead of just \255 or \254 -
    for us to not confuse it with an error or "change plugin" packets.
    We remove this escaping \1 here.

    See also server_mpvio_write_packet() where the escaping is done.
  */
  if (pkt_len && **buf == 1) {
    (*buf)++;
    pkt_len--;
  }
  mpvio->packets_read++;
  return pkt_len;
}

/**
  vio->read_packet() nonblocking callback method for client authentication
  plugins
*/
static net_async_status client_mpvio_read_packet_nonblocking(
    struct MYSQL_PLUGIN_VIO *mpv, uchar **buf, int *result) {
  DBUG_TRACE;
  MCPVIO_EXT *mpvio = (MCPVIO_EXT *)mpv;
  MYSQL *mysql = mpvio->mysql;
  ulong pkt_len;
  int error;

  /* there are cached data left, feed it to a plugin */
  if (mpvio->cached_server_reply.pkt_received) {
    *buf = mpvio->cached_server_reply.pkt;
    mpvio->cached_server_reply.pkt = nullptr;
    mpvio->packets_read++;
    *result = mpvio->cached_server_reply.pkt_len;
    mpvio->cached_server_reply.pkt_len = 0;
    mpvio->cached_server_reply.pkt_received = false;
    return NET_ASYNC_COMPLETE;
  }

  if (mpvio->packets_read == 0) {
    /*
      the server handshake packet came from the wrong plugin,
      or it's mysql_change_user(). Either way, there is no data
      for a plugin to read. send a dummy packet to the server
      to initiate a dialog.
    */
    const net_async_status status =
        client_mpvio_write_packet_nonblocking(mpv, nullptr, 0, &error);
    if (status == NET_ASYNC_NOT_READY) {
      return NET_ASYNC_NOT_READY;
    }
    if (error) {
      *result = (int)packet_error;
      return NET_ASYNC_COMPLETE;
    }
  }

  /*
    packets_read needs to be set here to avoid entering above condition
    again.
  */
  mpvio->packets_read++;
  /* otherwise read the data */
  const net_async_status status =
      mysql->methods->read_change_user_result_nonblocking(mysql, &pkt_len);
  if (status == NET_ASYNC_NOT_READY) {
    return NET_ASYNC_NOT_READY;
  }

  mpvio->last_read_packet_len = pkt_len;
  *buf = mysql->net.read_pos;

  /* was it a request to change plugins ? */
  if (**buf == 254) {
    *result = (int)packet_error;
    return NET_ASYNC_COMPLETE;
  }

  /*
    the server sends \1\255 or \1\254 instead of just \255 or \254 -
    for us to not confuse it with an error or "change plugin" packets.
    We remove this escaping \1 here.
    See also server_mpvio_write_packet() where the escaping is
    done.
  */
  if (pkt_len && **buf == 1) {
    (*buf)++;
    pkt_len--;
  }
  *result = pkt_len;
  return NET_ASYNC_COMPLETE;
}

#ifdef __clang__
// Clang UBSAN false positive?
// Call to function through pointer to incorrect function type
static int client_mpvio_write_packet(MYSQL_PLUGIN_VIO *mpv, const uchar *pkt,
                                     int pkt_len) SUPPRESS_UBSAN;
#endif  // __clang__

/**
  vio->write_packet() callback method for client authentication plugins

  This function is called by a client authentication plugin, when it wants
  to send data to the server.

  It transparently wraps the data into a change user or authentication
  handshake packet, if necessary.
*/
static int client_mpvio_write_packet(MYSQL_PLUGIN_VIO *mpv, const uchar *pkt,
                                     int pkt_len) {
  int res;
  MCPVIO_EXT *mpvio = (MCPVIO_EXT *)mpv;

  if (mpvio->packets_written == 0) {
    if (mpvio->mysql_change_user)
      res = send_change_user_packet(mpvio, pkt, pkt_len);
    else
      res = send_client_reply_packet(mpvio, pkt, pkt_len);
  } else {
    NET *net = &mpvio->mysql->net;

    MYSQL_TRACE(SEND_AUTH_DATA, mpvio->mysql, ((size_t)pkt_len, pkt));

    if (mpvio->mysql->thd)
      res = 1; /* no chit-chat in embedded */
    else
      res = my_net_write(net, pkt, pkt_len) || net_flush(net);

    if (!res) {
      MYSQL_TRACE(PACKET_SENT, mpvio->mysql, ((size_t)pkt_len));
    } else
      set_mysql_extended_error(mpvio->mysql, CR_SERVER_LOST, unknown_sqlstate,
                               ER_CLIENT(CR_SERVER_LOST_EXTENDED),
                               "sending authentication information", errno);
  }
  mpvio->packets_written++;
  return res;
}

/**
  vio->write_packet() nonblocking callback method for client authentication
  plugins
*/
static net_async_status client_mpvio_write_packet_nonblocking(
    struct MYSQL_PLUGIN_VIO *mpv, const uchar *pkt, int pkt_len, int *result) {
  DBUG_TRACE;
  MCPVIO_EXT *mpvio = (MCPVIO_EXT *)mpv;
  bool error = false;

  if (mpvio->packets_written == 0) {
    /* mysql_change_user_nonblocking not implemented yet. */
    assert(!mpvio->mysql_change_user);
    const net_async_status status =
        send_client_reply_packet_nonblocking(mpvio, pkt, pkt_len, &error);
    if (status == NET_ASYNC_NOT_READY) {
      return NET_ASYNC_NOT_READY;
    }
  } else {
    NET *net = &mpvio->mysql->net;

    MYSQL_TRACE(SEND_AUTH_DATA, mpvio->mysql, ((size_t)pkt_len, pkt));

    if (mpvio->mysql->thd)
      error = true; /* no chit-chat in embedded */
    else {
      const net_async_status status =
          my_net_write_nonblocking(net, pkt, pkt_len, &error);
      if (status == NET_ASYNC_NOT_READY) {
        return NET_ASYNC_NOT_READY;
      }
      *result = error;

      if (error) {
        set_mysql_extended_error(mpvio->mysql, CR_SERVER_LOST, unknown_sqlstate,
                                 ER_CLIENT(CR_SERVER_LOST_EXTENDED),
                                 "sending authentication information", errno);
      } else {
        MYSQL_TRACE(PACKET_SENT, mpvio->mysql, ((size_t)pkt_len));
      }
    }
  }
  mpvio->packets_written++;
  *result = error ? -1 : 0;
  return NET_ASYNC_COMPLETE;
}

/**
  fills MYSQL_PLUGIN_VIO_INFO structure with the information about the
  connection
*/
void mpvio_info(Vio *vio, MYSQL_PLUGIN_VIO_INFO *info) {
  memset(info, 0, sizeof(*info));
  switch (vio->type) {
    case VIO_TYPE_TCPIP:
      info->protocol = MYSQL_PLUGIN_VIO_INFO::MYSQL_VIO_TCP;
      info->socket = (int)vio_fd(vio);
      return;
    case VIO_TYPE_SOCKET:
      info->protocol = MYSQL_PLUGIN_VIO_INFO::MYSQL_VIO_SOCKET;
      info->socket = (int)vio_fd(vio);
      return;
    case VIO_TYPE_SSL: {
      info->is_tls_established = true;
      struct sockaddr addr;
      socklen_t addrlen = sizeof(addr);
      if (getsockname(vio_fd(vio), &addr, &addrlen)) return;
      info->protocol = addr.sa_family == AF_UNIX
                           ? MYSQL_PLUGIN_VIO_INFO::MYSQL_VIO_SOCKET
                           : MYSQL_PLUGIN_VIO_INFO::MYSQL_VIO_TCP;
      info->socket = (int)vio_fd(vio);
      return;
    }
#ifdef _WIN32
    case VIO_TYPE_NAMEDPIPE:
      info->protocol = MYSQL_PLUGIN_VIO_INFO::MYSQL_VIO_PIPE;
      info->handle = vio->hPipe;
      return;
#if defined(_WIN32)
    case VIO_TYPE_SHARED_MEMORY:
      info->protocol = MYSQL_PLUGIN_VIO_INFO::MYSQL_VIO_MEMORY;
      info->handle = vio->handle_file_map; /* or what ? */
      return;
#endif
#endif
    default:
      assert(0);
  }
}

static void client_mpvio_info(MYSQL_PLUGIN_VIO *vio,
                              MYSQL_PLUGIN_VIO_INFO *info) {
  MCPVIO_EXT *mpvio = (MCPVIO_EXT *)vio;
  mpvio_info(mpvio->mysql->net.vio, info);
}

bool libmysql_cleartext_plugin_enabled = false;

static bool check_plugin_enabled(MYSQL *mysql, mysql_async_auth *ctx) {
  if (ctx->auth_plugin == &clear_password_client_plugin &&
      (!libmysql_cleartext_plugin_enabled &&
       (!mysql->options.extension ||
        !mysql->options.extension->enable_cleartext_plugin))) {
    set_mysql_extended_error(
        mysql, CR_AUTH_PLUGIN_CANNOT_LOAD, unknown_sqlstate,
        ER_CLIENT(CR_AUTH_PLUGIN_CANNOT_LOAD),
        clear_password_client_plugin.name, "plugin not enabled");
    return true;
  }
  if (ctx->non_blocking && !ctx->auth_plugin->authenticate_user_nonblocking) {
    set_mysql_extended_error(
        mysql, CR_AUTH_PLUGIN_CANNOT_LOAD, unknown_sqlstate,
        ER_CLIENT(CR_AUTH_PLUGIN_CANNOT_LOAD), ctx->auth_plugin->name,
        "plugin does not support nonblocking connect");
    /*
      We don't return true here because not all authentication plugins support
      non-blocking APIs.
      In case plugin does not have non-blocking API, we fallback to blocking
      API calls to further proceed with the authentication.
    */
  }
  return false;
}

static void reset_async_auth_data(mysql_async_auth &ctx) {
  ctx.mysql = nullptr;
  ctx.non_blocking = false;
  ctx.data = nullptr;
  ctx.data_len = 0;
  ctx.data_plugin = nullptr;
  ctx.db = nullptr;
  ctx.auth_plugin_name = nullptr;
  ctx.auth_plugin = nullptr;
  memset(&ctx.mpvio, 0, sizeof(MCPVIO_EXT));
  ctx.pkt_length = 0;
  ctx.res = 0;
  ctx.change_user_buff = nullptr;
  ctx.change_user_buff_len = 0;
  ctx.client_auth_plugin_state = 0;
  ctx.state_function = nullptr;
  ctx.current_factor_index = 0;
  memset(ctx.sha2_auth.encrypted_password, 0,
         sizeof(ctx.sha2_auth.encrypted_password));
  ctx.sha2_auth.public_key = nullptr;
  memset(ctx.sha2_auth.scramble_pkt, 0, sizeof(ctx.sha2_auth.scramble_pkt));
  ctx.sha2_auth.cipher_length = 0;
}

/**
  Client side of the plugin driver authentication.

  @note this is used by both the mysql_real_connect and mysql_change_user

  @param mysql       mysql
  @param data        pointer to the plugin auth data (scramble) in the
                     handshake packet
  @param data_len    the length of the data
  @param data_plugin a plugin that data were prepared for
                     or 0 if it's mysql_change_user()
  @param db          initial db to use, can be 0

  @retval 0 ok
  @retval 1 error
*/
int run_plugin_auth(MYSQL *mysql, char *data, uint data_len,
                    const char *data_plugin, const char *db) {
  DBUG_TRACE;
  mysql_state_machine_status status;
  mysql_async_auth ctx;
  reset_async_auth_data(ctx);

  ctx.mysql = mysql;
  ctx.data = data;
  ctx.data_len = data_len;
  ctx.data_plugin = data_plugin;
  ctx.db = db;
  ctx.non_blocking = false;
  /* set initial auth factor to be first factor */
  ctx.current_factor_index = 0;
  ctx.state_function = authsm_begin_plugin_auth;

  do {
    status = ctx.state_function(&ctx);
    DBUG_PRINT("info", ("status %d", (int)status));
  } while (status != STATE_MACHINE_FAILED && status != STATE_MACHINE_DONE);

  return status == STATE_MACHINE_FAILED;
}

/**
  This functions drives the authentication on client side in a nonblocking
  way. This function will call different modules in a sequence where each
  module is responsible to achieve a particular part in entire authentication
  phase.

  @note this is used by both the mysql_real_connect_nonblocking

  @param mysql       mysql
  @param data        pointer to the plugin auth data (scramble) in the
                     handshake packet
  @param data_len    the length of the data
  @param data_plugin a plugin that data were prepared for
                     or 0 if it's mysql_change_user()
  @param db          initial db to use, can be 0

  @retval     NET_ASYNC_NOT_READY  authentication not yet complete
  @retval     NET_ASYNC_COMPLETE   authentication done
*/
mysql_state_machine_status run_plugin_auth_nonblocking(MYSQL *mysql, char *data,
                                                       uint data_len,
                                                       const char *data_plugin,
                                                       const char *db) {
  DBUG_TRACE;
  mysql_async_auth *ctx = ASYNC_DATA(mysql)->connect_context->auth_context;
  if (!ctx) {
    ctx = static_cast<mysql_async_auth *>(
        my_malloc(key_memory_MYSQL, sizeof(*ctx), MYF(MY_WME | MY_ZEROFILL)));

    ctx->mysql = mysql;
    ctx->data = data;
    ctx->data_len = data_len;
    ctx->data_plugin = data_plugin;
    ctx->db = db;
    ctx->non_blocking = true;
    ctx->current_factor_index = 0;
    ctx->state_function = authsm_begin_plugin_auth;
    ASYNC_DATA(mysql)->connect_context->auth_context = ctx;
  }

  const mysql_state_machine_status ret = ctx->state_function(ctx);
  if (ret == STATE_MACHINE_FAILED || ret == STATE_MACHINE_DONE) {
    my_free(ctx);
    ASYNC_DATA(mysql)->connect_context->auth_context = nullptr;
  }

  return ret;
}

/**
  Determine the default/initial plugin to use
*/
static mysql_state_machine_status authsm_begin_plugin_auth(
    mysql_async_auth *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  ctx->auth_plugin_name = nullptr;
  if (mysql->server_capabilities & CLIENT_PLUGIN_AUTH &&
      ctx->data_plugin != nullptr) {
    /*
      LDAP SASL Kerberos and Native Karberos plug-in depends on client side set
      default plug-in to obtained user name from credential cache. If we don't
      use client side authentication plug-in, user and password less
      functionality will not work. And this is very important feature for
      Kerberos.

      We are not overriding client side default authentication plug-in if
      it is configured in the client side. Once server supports configuration of
      all the authentication plug-ins as default, Below code shall be removed.
      Checks:
      1. Default authentication plug-in is configured
      2. Default authentication plug-in is valid.
    */
    auth_plugin_t *client_plugin{nullptr};
    if (mysql->options.extension && mysql->options.extension->default_auth &&
        (client_plugin = (auth_plugin_t *)mysql_client_find_plugin(
             mysql, mysql->options.extension->default_auth,
             MYSQL_CLIENT_AUTHENTICATION_PLUGIN))) {
      ctx->auth_plugin_name = mysql->options.extension->default_auth;
    } else {
      ctx->auth_plugin_name = ctx->data_plugin;
    }
    if (!(ctx->auth_plugin = (auth_plugin_t *)mysql_client_find_plugin(
              mysql, ctx->auth_plugin_name,
              MYSQL_CLIENT_AUTHENTICATION_PLUGIN))) {
      /*
        Client didn't recognize the server default plugin.
        Fallback on any client plugin set as the default.
      */
      if (mysql->options.extension && mysql->options.extension->default_auth) {
        ctx->auth_plugin_name = mysql->options.extension->default_auth;
        if (!(ctx->auth_plugin = (auth_plugin_t *)mysql_client_find_plugin(
                  mysql, ctx->auth_plugin_name,
                  MYSQL_CLIENT_AUTHENTICATION_PLUGIN)))
          return STATE_MACHINE_FAILED; /* oops, not found */
      }
    }
  }

  if (ctx->auth_plugin_name == nullptr || ctx->auth_plugin == nullptr) {
    auth_plugin_t *client_plugin{nullptr};
    if (mysql->options.extension && mysql->options.extension->default_auth &&
        (client_plugin = (auth_plugin_t *)mysql_client_find_plugin(
             mysql, mysql->options.extension->default_auth,
             MYSQL_CLIENT_AUTHENTICATION_PLUGIN))) {
      // try default_auth again in case CLIENT_PLUGIN_AUTH wasn't on.
      ctx->auth_plugin_name = mysql->options.extension->default_auth;
      ctx->auth_plugin = client_plugin;
    } else {
      /*
       * before we go here, the following happens:
       * a. in csm_parse_handshake(),
       *    if server has CLIENT_PLUGIN_AUTH,
       *    ctx->scramble_plugin is taken from Server Greetings
       *    otherwise it is set to "mysql_native_password"
       * b. ctx->scramble_plugin is passed as data_plugin arg
       *    into run_plugin_auth()
       * c. run_plugin_auth() assigns data_plugin arg to ctx->data_plugin
       *
       * and we have to find/load the plugin indicated
       */
      if (mysql->server_capabilities & CLIENT_PLUGIN_AUTH) {
        // what is a scenario we could get here ?
        // maybe new plugin (not existing now) which is a default
        // on server and it is unknown to client
        ctx->auth_plugin = &caching_sha2_password_client_plugin;
        ctx->auth_plugin_name = ctx->auth_plugin->name;
      } else if (ctx->data_plugin &&
                 (client_plugin = (auth_plugin_t *)mysql_client_find_plugin(
                      mysql, ctx->data_plugin,
                      MYSQL_CLIENT_AUTHENTICATION_PLUGIN))) {
        ctx->auth_plugin_name = ctx->data_plugin;
        ctx->auth_plugin = client_plugin;
      } else {
        // some abnormal case, we should not get here
        assert(ctx->auth_plugin_name);
        assert(ctx->auth_plugin);
      }
    }
  }

  if (check_plugin_enabled(mysql, ctx)) return STATE_MACHINE_FAILED;

  DBUG_PRINT("info", ("using plugin %s", ctx->auth_plugin_name));

  mysql->net.last_errno = 0; /* just in case */

  /*
    data_plugin has the plugin the server wants us to use and
    auth_plugin_name has the plugin we think we will need.
    Server doesn't know what user the client will send yet.
    so client knows more than the server - hence we should use
    auth_plugin_name if it is different from ctx->data_plugin
  */
  if (ctx->data_plugin && strcmp(ctx->data_plugin, ctx->auth_plugin_name)) {
    ctx->data = nullptr;
    ctx->data_len = 0;
  }

  ctx->mpvio.mysql_change_user = ctx->data_plugin == nullptr;
  ctx->mpvio.cached_server_reply.pkt = (uchar *)ctx->data;
  ctx->mpvio.cached_server_reply.pkt_len = ctx->data_len;
  /*
    Sometimes plugin provided data (like scramble) can be optional, in such a
    case we set pkt_received flag to false based on data length. This flag is
    later checked in client_mpvio_read_packet() to decide on to read from
    network or return the cached data.
  */
  if (ctx->data_len == 0)
    ctx->mpvio.cached_server_reply.pkt_received = false;
  else
    ctx->mpvio.cached_server_reply.pkt_received = true;
  ctx->mpvio.read_packet = client_mpvio_read_packet;
  ctx->mpvio.write_packet = client_mpvio_write_packet;
  ctx->mpvio.read_packet_nonblocking = client_mpvio_read_packet_nonblocking;
  ctx->mpvio.write_packet_nonblocking = client_mpvio_write_packet_nonblocking;
  ctx->mpvio.info = client_mpvio_info;
  ctx->mpvio.mysql = mysql;
  ctx->mpvio.packets_read = ctx->mpvio.packets_written = 0;
  ctx->mpvio.db = ctx->db;
  ctx->mpvio.plugin = ctx->auth_plugin;
  ctx->client_auth_plugin_state =
      (int)(client_auth_caching_sha2_password_plugin_status::
                CACHING_SHA2_READING_PASSWORD);

  ctx->state_function = authsm_run_first_authenticate_user;
  return STATE_MACHINE_CONTINUE;
}

/**
  Authentication can have two authenticate_user calls, depending on
  what the server responds with; this handles the first.
*/
static mysql_state_machine_status authsm_run_first_authenticate_user(
    mysql_async_auth *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  MYSQL_TRACE(AUTH_PLUGIN, mysql, (ctx->auth_plugin->name));

  if (ctx->non_blocking && ctx->auth_plugin->authenticate_user_nonblocking) {
    const net_async_status status =
        ctx->auth_plugin->authenticate_user_nonblocking(
            (struct MYSQL_PLUGIN_VIO *)&ctx->mpvio, mysql, &ctx->res);
    if (status == NET_ASYNC_NOT_READY) {
      return STATE_MACHINE_WOULD_BLOCK;
    }
  } else {
    ctx->res = ctx->auth_plugin->authenticate_user(
        (struct MYSQL_PLUGIN_VIO *)&ctx->mpvio, mysql);
  }

  ctx->state_function = authsm_handle_first_authenticate_user;
  return STATE_MACHINE_CONTINUE;
}

/**
  Handle the result of the first authenticate_user.
*/
static mysql_state_machine_status authsm_handle_first_authenticate_user(
    mysql_async_auth *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  DBUG_PRINT("info", ("authenticate_user returned %s",
                      ctx->res == CR_OK      ? "CR_OK"
                      : ctx->res == CR_ERROR ? "CR_ERROR"
                      : ctx->res == CR_OK_HANDSHAKE_COMPLETE
                          ? "CR_OK_HANDSHAKE_COMPLETE"
                          : "error"));

  static_assert(CR_OK == -1, "");
  static_assert(CR_ERROR == 0, "");

  /*
    The connection may be closed. If so: do not try to read from the buffer.
    If server sends OK packet without sending auth-switch first, client side
    auth plugin may not be able to process it correctly.
    However, if server sends OK, it means server side authentication plugin
    already performed required checks. Further, server side plugin did not
    really care about plugin used by client in this case.
  */
  if (ctx->res > CR_OK &&
      (!my_net_is_inited(&mysql->net) ||
       (mysql->net.read_pos[0] != 0 && mysql->net.read_pos[0] != 254))) {
    /*
      the plugin returned an error. write it down in mysql,
      unless the error code is CR_ERROR and mysql->net.last_errno
      is already set (the plugin has done it)
    */
    DBUG_PRINT("info", ("res=%d", ctx->res));
    if (ctx->res > CR_ERROR)
      set_mysql_error(mysql, ctx->res, unknown_sqlstate);
    else if (!mysql->net.last_errno)
      set_mysql_error(mysql, CR_UNKNOWN_ERROR, unknown_sqlstate);
    return STATE_MACHINE_FAILED;
  }
  ctx->state_function = authsm_read_change_user_result;
  return STATE_MACHINE_CONTINUE;
}

/**
  After the first authenticate_user comes a call to read the result of the
  implied change_user.
*/
static mysql_state_machine_status authsm_read_change_user_result(
    mysql_async_auth *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  /* read the OK packet (or use the cached value in mysql->net.read_pos */
  if (ctx->res == CR_OK) {
    if (ctx->non_blocking) {
      const net_async_status status =
          (*mysql->methods->read_change_user_result_nonblocking)(
              mysql, &ctx->pkt_length);
      if (status == NET_ASYNC_NOT_READY) {
        return STATE_MACHINE_WOULD_BLOCK;
      }
    } else {
      ctx->pkt_length = (*mysql->methods->read_change_user_result)(mysql);
    }
  } else /* res == CR_OK_HANDSHAKE_COMPLETE */
    ctx->pkt_length = ctx->mpvio.last_read_packet_len;

  ctx->state_function = authsm_handle_change_user_result;
  return STATE_MACHINE_CONTINUE;
}

/**
  Check if server asked to use a different authentication plugin
*/
static mysql_state_machine_status authsm_handle_change_user_result(
    mysql_async_auth *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  DBUG_PRINT("info", ("OK packet length=%lu", ctx->pkt_length));
  if (ctx->pkt_length == packet_error) {
    if (mysql->net.last_errno == CR_SERVER_LOST)
      set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                               ER_CLIENT(CR_SERVER_LOST_EXTENDED),
                               "reading authorization packet", errno);
    return STATE_MACHINE_FAILED;
  }

  if (mysql->net.read_pos[0] == 254) {
    ctx->state_function = authsm_run_second_authenticate_user;
  } else if (is_auth_next_factor_packet(mysql)) {
    ctx->state_function = authsm_init_multi_auth;
  } else if (is_OK_packet(mysql, ctx->pkt_length)) {
    read_ok_ex(mysql, ctx->pkt_length);
    ctx->state_function = authsm_finish_auth;
  } else {
    set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
    return STATE_MACHINE_FAILED;
  }

  return STATE_MACHINE_CONTINUE;
}

/**
  Start the authentication process again with the plugin which
  server asked for.
*/
static mysql_state_machine_status authsm_run_second_authenticate_user(
    mysql_async_auth *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  /* The server asked to use a different authentication plugin */
  if (ctx->pkt_length < 2) {
    set_mysql_error(mysql, CR_MALFORMED_PACKET,
                    unknown_sqlstate); /* purecov: inspected */
    return STATE_MACHINE_FAILED;
  } else {
    /* "use different plugin" packet */
    uint len;
    ctx->auth_plugin_name = (char *)mysql->net.read_pos + 1;
    len = (uint)strlen(
        ctx->auth_plugin_name); /* safe as my_net_read always appends \0 */
    ctx->mpvio.cached_server_reply.pkt_len = ctx->pkt_length - len - 2;
    ctx->mpvio.cached_server_reply.pkt = mysql->net.read_pos + len + 2;
    ctx->mpvio.cached_server_reply.pkt_received = true;
    DBUG_PRINT("info", ("change plugin packet from server for plugin %s",
                        ctx->auth_plugin_name));
  }
  if (!(ctx->auth_plugin = (auth_plugin_t *)mysql_client_find_plugin(
            mysql, ctx->auth_plugin_name, MYSQL_CLIENT_AUTHENTICATION_PLUGIN)))
    return STATE_MACHINE_FAILED;

  if (check_plugin_enabled(mysql, ctx)) return STATE_MACHINE_FAILED;

  MYSQL_TRACE(AUTH_PLUGIN, mysql, (ctx->auth_plugin->name));

  DBUG_EXECUTE_IF("simulate_fido_testing", {
    bool is_fido_testing = true;
    mysql_plugin_options((struct st_mysql_client_plugin *)ctx->auth_plugin,
                         "is_fido_testing", &is_fido_testing);
  });
  ctx->mpvio.plugin = ctx->auth_plugin;
  ctx->res = ctx->auth_plugin->authenticate_user(
      (struct MYSQL_PLUGIN_VIO *)&ctx->mpvio, mysql);

  ctx->state_function = authsm_handle_second_authenticate_user;
  return STATE_MACHINE_CONTINUE;
}

/* Now read the results. */
static mysql_state_machine_status authsm_handle_second_authenticate_user(
    mysql_async_auth *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  DBUG_PRINT("info", ("second authenticate_user returned %s",
                      ctx->res == CR_OK      ? "CR_OK"
                      : ctx->res == CR_ERROR ? "CR_ERROR"
                      : ctx->res == CR_OK_HANDSHAKE_COMPLETE
                          ? "CR_OK_HANDSHAKE_COMPLETE"
                          : "error"));
  if (ctx->res > CR_OK) {
    if (ctx->res > CR_ERROR)
      set_mysql_error(mysql, ctx->res, unknown_sqlstate);
    else if (!mysql->net.last_errno)
      set_mysql_error(mysql, CR_UNKNOWN_ERROR, unknown_sqlstate);
    return STATE_MACHINE_FAILED;
  }

  if (ctx->res != CR_OK_HANDSHAKE_COMPLETE) {
    /* Read what server thinks about out new auth message report */
    if ((ctx->pkt_length = cli_safe_read(mysql, nullptr)) == packet_error) {
      if (mysql->net.last_errno == CR_SERVER_LOST)
        set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                                 ER_CLIENT(CR_SERVER_LOST_EXTENDED),
                                 "reading final connect information", errno);
      return STATE_MACHINE_FAILED;
    }
    if (is_auth_next_factor_packet(mysql)) {
      ctx->state_function = authsm_init_multi_auth;
      return STATE_MACHINE_CONTINUE;
    } else if (is_OK_packet(mysql, ctx->pkt_length)) {
      read_ok_ex(mysql, ctx->pkt_length);
    } else {
      set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
      return STATE_MACHINE_FAILED;
    }
  }
  ctx->state_function = authsm_finish_auth;
  return STATE_MACHINE_CONTINUE;
}

/* Final cleanup */
static mysql_state_machine_status authsm_finish_auth(mysql_async_auth *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  /*
    net->read_pos[0] should always be 0 here if the server implements
    the protocol correctly
  */
  ctx->res = (mysql->net.read_pos[0] != 0);

  MYSQL_TRACE(AUTHENTICATED, mysql, ());
  return ctx->res ? STATE_MACHINE_FAILED : STATE_MACHINE_DONE;
}

/** Start multi factor authentication */
static mysql_state_machine_status authsm_init_multi_auth(
    mysql_async_auth *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  /*
    If previous factor authentication is a success, read AuthNextFactor packet,
    extract client plugin name and plugin specific data and initiate next factor
    authentication.
  */
  size_t len;
  ctx->auth_plugin_name = (char *)mysql->net.read_pos + 1;
  len = strlen(ctx->auth_plugin_name);
  /* adjust cached plugin data packet */
  ctx->mpvio.cached_server_reply.pkt_len = ctx->pkt_length - len - 2;
  ctx->mpvio.cached_server_reply.pkt = mysql->net.read_pos + len + 2;
  ctx->mpvio.cached_server_reply.pkt_received = true;
  DBUG_PRINT("info", ("AuthNextFactor plugin packet from server %s",
                      ctx->auth_plugin_name));

  /* update current factor index to point to nth factor */
  ctx->current_factor_index++;
  if (!(ctx->auth_plugin = (auth_plugin_t *)mysql_client_find_plugin(
            mysql, ctx->auth_plugin_name,
            MYSQL_CLIENT_AUTHENTICATION_PLUGIN))) {
    set_mysql_extended_error(mysql, CR_AUTH_PLUGIN_CANNOT_LOAD,
                             unknown_sqlstate,
                             ER_CLIENT(CR_AUTH_PLUGIN_CANNOT_LOAD),
                             ctx->auth_plugin_name, "plugin not available");
    return STATE_MACHINE_FAILED;
  }
  if (mysql->options.extension) {
    char **plugin_name =
        &mysql->options.extension->client_auth_info[ctx->current_factor_index]
             .plugin_name;
    *plugin_name = static_cast<char *>(
        my_malloc(PSI_NOT_INSTRUMENTED, len + 1, MYF(MY_WME | MY_ZEROFILL)));
    if (!*plugin_name) {
      set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
      return STATE_MACHINE_FAILED;
    }
    memcpy(*plugin_name, ctx->auth_plugin_name, len);
  }
  DBUG_EXECUTE_IF("simulate_fido_testing", {
    bool is_fido_testing = true;
    mysql_plugin_options((struct st_mysql_client_plugin *)ctx->auth_plugin,
                         "is_fido_testing", &is_fido_testing);
  });
  /* check if plugin is enabled */
  if (check_plugin_enabled(mysql, ctx)) return STATE_MACHINE_FAILED;

  /* reset password for next client auth plugin */
  if (mysql->passwd) mysql->passwd[0] = 0;
  /* get password */
  if (mysql->options.extension &&
      mysql->options.extension->client_auth_info[ctx->current_factor_index]
          .password) {
    my_free(mysql->passwd);
    mysql->passwd = my_strdup(
        key_memory_MYSQL,
        mysql->options.extension->client_auth_info[ctx->current_factor_index]
            .password,
        MYF(0));
  }
  ctx->state_function = authsm_do_multi_plugin_auth;
  return STATE_MACHINE_CONTINUE;
}

/** Invoke client plugins authentication method */
static mysql_state_machine_status authsm_do_multi_plugin_auth(
    mysql_async_auth *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  MYSQL_TRACE(AUTH_PLUGIN, mysql, (ctx->auth_plugin->name));

  ctx->mpvio.plugin = ctx->auth_plugin;
  ctx->res = ctx->auth_plugin->authenticate_user(
      (struct MYSQL_PLUGIN_VIO *)&ctx->mpvio, mysql);

  ctx->state_function = authsm_handle_multi_auth_response;
  return STATE_MACHINE_CONTINUE;
}

/** Handle response from client plugins authentication method */
static mysql_state_machine_status authsm_handle_multi_auth_response(
    mysql_async_auth *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;

  if (ctx->res > CR_OK) {
    if (ctx->res > CR_ERROR)
      set_mysql_error(mysql, ctx->res, unknown_sqlstate);
    else if (!mysql->net.last_errno)
      set_mysql_error(mysql, CR_UNKNOWN_ERROR, unknown_sqlstate);
    return STATE_MACHINE_FAILED;
  }

  if (ctx->res != CR_OK_HANDSHAKE_COMPLETE) {
    /* Read what server thinks about new auth message report */
    if ((ctx->pkt_length = cli_safe_read(mysql, nullptr)) == packet_error) {
      if (mysql->net.last_errno == CR_SERVER_LOST)
        set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                                 ER_CLIENT(CR_SERVER_LOST_EXTENDED),
                                 "reading final connect information", errno);
      return STATE_MACHINE_FAILED;
    }
    if (is_auth_next_factor_packet(mysql)) {
      ctx->state_function = authsm_init_multi_auth;
      return STATE_MACHINE_CONTINUE;
    } else if (is_OK_packet(mysql, ctx->pkt_length)) {
      read_ok_ex(mysql, ctx->pkt_length);
    } else {
      set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
      return STATE_MACHINE_FAILED;
    }
  }
  ctx->state_function = authsm_finish_auth;
  assert(ctx->current_factor_index < 3);
  return STATE_MACHINE_CONTINUE;
}

/** set some default attributes */
static int set_connect_attributes(MYSQL *mysql, char *buff, size_t buf_len) {
  DBUG_TRACE;
  int rc = 0;

  /*
    Clean up any values set by the client code. We want these options as
    consistent as possible
  */
  rc += mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_DELETE, "_client_name");
  rc += mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_DELETE, "_os");
  rc += mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_DELETE, "_platform");
  rc += mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_DELETE, "_pid");
  rc += mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_DELETE, "_thread");
  rc += mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_DELETE, "_client_version");

  /*
   Now let's set up some values
  */
  rc += mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD, "_client_name",
                       "libmysql");
  rc += mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD, "_client_version",
                       PACKAGE_VERSION);
  rc += mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD, "_os", SYSTEM_TYPE);
  rc += mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD, "_platform",
                       MACHINE_TYPE);
#ifdef _WIN32
  snprintf(buff, buf_len, "%lu", (ulong)GetCurrentProcessId());
#else
  snprintf(buff, buf_len, "%lu", (ulong)getpid());
#endif
  rc += mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD, "_pid", buff);

#ifdef _WIN32
  snprintf(buff, buf_len, "%lu", (ulong)GetCurrentThreadId());
  rc += mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD, "_thread", buff);
#endif

  return rc > 0 ? 1 : 0;
}

MYSQL *connect_helper(mysql_async_connect *ctx) {
  mysql_state_machine_status status;
  auto mysql = ctx->mysql;
  mysql->options.client_flag |= ctx->client_flag;
  do {
    status = ctx->state_function(ctx);
  } while (status != STATE_MACHINE_FAILED && status != STATE_MACHINE_DONE);

  if (status == STATE_MACHINE_DONE) {
    DBUG_PRINT("exit", ("Mysql handler: %p", mysql));
    return ctx->mysql;
  }

  DBUG_PRINT("error", ("message: %u/%s (%s)", mysql->net.last_errno,
                       mysql->net.sqlstate, mysql->net.last_error));
  {
    /* Free alloced memory */
    end_server(mysql);
    mysql_close_free(mysql);
    if (!(ctx->client_flag & CLIENT_REMEMBER_OPTIONS))
      mysql_close_free_options(mysql);
    if (ctx->scramble_buffer_allocated) my_free(ctx->scramble_buffer);
  }
  return nullptr;
}

MYSQL *cli_connect(mysql_async_connect *ctx) {
  DBUG_TRACE;
  assert(ctx);
  ctx->state_function = csm_begin_connect;
  return connect_helper(ctx);
}

MYSQL *STDCALL mysql_real_connect(MYSQL *mysql, const char *host,
                                  const char *user, const char *passwd,
                                  const char *db, uint port,
                                  const char *unix_socket, ulong client_flag) {
  DBUG_TRACE;
  mysql_async_connect ctx;
  memset(&ctx, 0, sizeof(ctx));

  // Reset multipacket processing state
  NET_ASYNC *net_async = NET_ASYNC_DATA(&mysql->net);
  if (net_async) net_async->mp_state.reset();

  ctx.mysql = mysql;
  ctx.host = host;
  ctx.port = port;
  ctx.db = db;
  ctx.user = user;
  ENSURE_EXTENSIONS_PRESENT(&mysql->options);
  /* password will be extracted from mysql options */
  if (mysql->options.extension->client_auth_info[0].password)
    ctx.passwd = mysql->options.extension->client_auth_info[0].password;
  else
    ctx.passwd = passwd;
  ctx.unix_socket = unix_socket;
  if (0 != (client_flag & CLIENT_NO_SCHEMA)) {
    fprintf(stderr,
            "WARNING: CLIENT_NO_SCHEMA is deprecated and will be removed in a "
            "future version.\n");
  }
  mysql->options.client_flag |= client_flag;
  ctx.client_flag = mysql->options.client_flag;
  ctx.ssl_state = SSL_NONE;
  return (*mysql->methods->connect_method)(&ctx);
}

/**
  This API attempts to initialize all the context needed to make an asynchronous
  connection followed by establishing a connection to MySQL database. If this
  API returns NET_ASYNC_COMPLETE then connection is established else call this
  API from the client application until the status returned is
  NET_ASYNC_COMPLETE.

  @param[in]      mysql           connection handle
  @param[in]      host            host name or IP address
  @param[in]      user            login ID used to connect to host
  @param[in]      passwd          password for this login ID
  @param[in]      db              default database to be set after connection
  @param[in]      port            port number to use for connection
  @param[in]      unix_socket     socket file to use for connection
  @param[in]      client_flag     flag to indidcate what client can handle

  @retval       NET_ASYNC_COMPLETE               Success.
  @retval       NET_ASYNC_ERROR                  Error.
*/
net_async_status STDCALL mysql_real_connect_nonblocking(
    MYSQL *mysql, const char *host, const char *user, const char *passwd,
    const char *db, uint port, const char *unix_socket, ulong client_flag) {
  DBUG_TRACE;

  mysql_state_machine_status status;
  mysql_async_connect *ctx = ASYNC_DATA(mysql)->connect_context;

  if (!ctx) {
    ctx = static_cast<mysql_async_connect *>(
        my_malloc(key_memory_MYSQL, sizeof(*ctx), MYF(MY_WME | MY_ZEROFILL)));
    if (!ctx) return NET_ASYNC_ERROR;

    ctx->mysql = mysql;
    ctx->host = host;
    ctx->port = port;
    ctx->db = db;
    ctx->user = user;
    ENSURE_EXTENSIONS_PRESENT(&mysql->options);
    /* password will be extracted from mysql options */
    if (mysql->options.extension->client_auth_info[0].password)
      ctx->passwd = mysql->options.extension->client_auth_info[0].password;
    else
      ctx->passwd = passwd;
    ctx->unix_socket = unix_socket;
    mysql->options.client_flag |= client_flag;
    ctx->client_flag = mysql->options.client_flag;
    ctx->non_blocking = true;
    ctx->state_function = csm_begin_connect;
    ctx->ssl_state = SSL_NONE;
    ASYNC_DATA(mysql)->connect_context = ctx;
    ASYNC_DATA(mysql)->async_op_status = ASYNC_OP_CONNECT;
  }

  /*
    Continue to loop When different state returns STATE_MACHINE_CONTINUE, which
    means more work has to be done immediately and should not return to the
    caller.
  */
  do {
    status = ctx->state_function(ctx);
  } while (status == STATE_MACHINE_CONTINUE);

  if (status == STATE_MACHINE_DONE) {
    my_free(ASYNC_DATA(mysql)->connect_context);
    ASYNC_DATA(mysql)->connect_context = nullptr;
    ASYNC_DATA(mysql)->async_op_status = ASYNC_OP_UNSET;
    return NET_ASYNC_COMPLETE;
  }
  if (status == STATE_MACHINE_FAILED) {
    DBUG_PRINT("error", ("message: %u/%s (%s)", mysql->net.last_errno,
                         mysql->net.sqlstate, mysql->net.last_error));
    /* Free allocated memory */
    end_server(mysql);
    mysql_close_free(mysql);
    if (!(mysql->options.client_flag & CLIENT_REMEMBER_OPTIONS))
      mysql_close_free_options(mysql);
    return NET_ASYNC_ERROR;
  }
  /*
    State machine returned STATE_MACHINE_WOULD_BLOCK, thus expecting caller to
    call this function again to continue with state machine once pending IO is
    completed.
  */
  return NET_ASYNC_NOT_READY;
}
/**
  Begin the connection to the server, including any DNS resolution
  necessary, socket configuration, etc.
*/
static mysql_state_machine_status csm_begin_connect(mysql_async_connect *ctx) {
  MYSQL *mysql = ctx->mysql;
  const char *host = ctx->host;
  const char *user = ctx->user;
  const char *passwd = ctx->passwd;
  const char *db = ctx->db;
  uint port = ctx->port;
  const char *unix_socket = ctx->unix_socket;
  const ulong client_flag = ctx->client_flag;
  bool connect_done =
      true;  // this is true for most of the connect methods except sockets

  DBUG_TRACE;

  DBUG_PRINT("enter",
             ("host: %s  db: %s  user: %s (client)", host ? host : "(Null)",
              db ? db : "(Null)", user ? user : "(Null)"));

  NET *net = &mysql->net;
#ifdef _WIN32
  HANDLE hPipe = INVALID_HANDLE_VALUE;
#endif
#ifdef HAVE_SYS_UN_H
  struct sockaddr_un UNIXaddr;
#endif
  /* Test whether we're already connected */
  if (net->vio) {
    set_mysql_error(mysql, CR_ALREADY_CONNECTED, unknown_sqlstate);
    return STATE_MACHINE_FAILED;
  }

  if (set_connect_attributes(mysql, ctx->buff, sizeof(ctx->buff)))
    return STATE_MACHINE_FAILED;

  net->vio = nullptr;     /* If something goes wrong */
  mysql->client_flag = 0; /* For handshake */

  /* use default options */
  if (mysql->options.my_cnf_file || mysql->options.my_cnf_group) {
    mysql_read_default_options(
        &mysql->options,
        (mysql->options.my_cnf_file ? mysql->options.my_cnf_file : "my"),
        mysql->options.my_cnf_group);
    my_free(mysql->options.my_cnf_file);
    my_free(mysql->options.my_cnf_group);
    mysql->options.my_cnf_file = mysql->options.my_cnf_group = nullptr;
  }

  /* Some empty-string-tests are done because of ODBC */
  if (!host || !host[0]) host = mysql->options.host;
  if (!user || !user[0]) {
    user = mysql->options.user;
    if (!user) user = "";
  }
  if (!passwd) {
    passwd = mysql->options.password;
#if !defined(MYSQL_SERVER)
    if (!passwd) passwd = getenv("MYSQL_PWD"); /* get it from environment */
#endif
    if (!passwd) passwd = "";
  }
  if (!db || !db[0]) db = mysql->options.db;
  if (!port) port = mysql->options.port;
  if (!unix_socket) unix_socket = mysql->options.unix_socket;

  mysql->server_status = SERVER_STATUS_AUTOCOMMIT;
  DBUG_PRINT("info", ("Connecting"));

  MYSQL_TRACE_STAGE(mysql, CONNECTING);
  MYSQL_TRACE(CONNECTING, mysql, ());

#if defined(_WIN32)
  if ((!mysql->options.protocol ||
       mysql->options.protocol == MYSQL_PROTOCOL_MEMORY) &&
      (!host || !strcmp(host, LOCAL_HOST))) {
    HANDLE handle_map;
    DBUG_PRINT("info", ("Using shared memory"));

    handle_map =
        create_shared_memory(mysql, net, get_win32_connect_timeout(mysql));

    if (handle_map == INVALID_HANDLE_VALUE) {
      DBUG_PRINT("error",
                 ("host: '%s'  socket: '%s'  shared memory: %s  have_tcpip: %d",
                  host ? host : "<null>", unix_socket ? unix_socket : "<null>",
                  mysql->options.shared_memory_base_name, (int)have_tcpip));
      if (mysql->options.protocol == MYSQL_PROTOCOL_MEMORY)
        return STATE_MACHINE_FAILED;
      /*
        Try also with PIPE or TCP/IP. Clear the error from
        create_shared_memory().
      */

      net_clear_error(net);
    } else {
      mysql->options.protocol = MYSQL_PROTOCOL_MEMORY;
      unix_socket = nullptr;
      host = mysql->options.shared_memory_base_name;
      snprintf(ctx->host_info = ctx->buff, sizeof(ctx->buff) - 1,
               ER_CLIENT(CR_SHARED_MEMORY_CONNECTION), host);
    }
  }
#endif /* _WIN32 */
#if defined(HAVE_SYS_UN_H)
  if (!net->vio &&
      (!mysql->options.protocol ||
       mysql->options.protocol == MYSQL_PROTOCOL_SOCKET) &&
      (unix_socket || mysql_unix_port) &&
      (!host || !strcmp(host, LOCAL_HOST))) {
    my_socket sock = socket(AF_UNIX, SOCK_STREAM, 0);
    DBUG_PRINT("info", ("Using socket"));
    if (sock == INVALID_SOCKET) {
      set_mysql_extended_error(mysql, CR_SOCKET_CREATE_ERROR, unknown_sqlstate,
                               ER_CLIENT(CR_SOCKET_CREATE_ERROR), socket_errno);
      return STATE_MACHINE_FAILED;
    }

    net->vio =
        vio_new(sock, VIO_TYPE_SOCKET, VIO_LOCALHOST | VIO_BUFFERED_READ);
    if (!net->vio) {
      DBUG_PRINT("error", ("Unknow protocol %d ", mysql->options.protocol));
      set_mysql_error(mysql, CR_CONN_UNKNOW_PROTOCOL, unknown_sqlstate);
      closesocket(sock);
      return STATE_MACHINE_FAILED;
    }

    if (ctx->non_blocking) vio_set_blocking_flag(net->vio, !ctx->non_blocking);

    host = LOCAL_HOST;
    if (!unix_socket) unix_socket = mysql_unix_port;
    ctx->host_info = const_cast<char *>(ER_CLIENT(CR_LOCALHOST_CONNECTION));
    DBUG_PRINT("info", ("Using UNIX sock '%s'", unix_socket));

    memset(&UNIXaddr, 0, sizeof(UNIXaddr));
    UNIXaddr.sun_family = AF_UNIX;
    strmake(UNIXaddr.sun_path, unix_socket, sizeof(UNIXaddr.sun_path) - 1);

    if (mysql->options.extension && mysql->options.extension->retry_count)
      my_net_set_retry_count(net, mysql->options.extension->retry_count);

    if (vio_socket_connect(net->vio, (struct sockaddr *)&UNIXaddr,
                           sizeof(UNIXaddr), ctx->non_blocking,
                           get_vio_connect_timeout(mysql), &connect_done)) {
      DBUG_PRINT("error",
                 ("Got error %d on connect to local server", socket_errno));
      set_mysql_extended_error(mysql, CR_CONNECTION_ERROR, unknown_sqlstate,
                               ER_CLIENT(CR_CONNECTION_ERROR), unix_socket,
                               socket_errno);
      vio_delete(net->vio);
      net->vio = nullptr;
      return STATE_MACHINE_FAILED;
    }
    mysql->options.protocol = MYSQL_PROTOCOL_SOCKET;
  }
#elif defined(_WIN32)
  if (!net->vio && (mysql->options.protocol == MYSQL_PROTOCOL_PIPE ||
                    (host && !strcmp(host, LOCAL_HOST_NAMEDPIPE)) ||
                    (!have_tcpip && (unix_socket || (!host && is_NT()))))) {
    hPipe = create_named_pipe(mysql, get_win32_connect_timeout(mysql), &host,
                              &unix_socket);

    if (hPipe == INVALID_HANDLE_VALUE) {
      DBUG_PRINT(
          "error",
          ("host: '%s'  socket: '%s'  have_tcpip: %d", host ? host : "<null>",
           unix_socket ? unix_socket : "<null>", (int)have_tcpip));
      if (mysql->options.protocol == MYSQL_PROTOCOL_PIPE ||
          (host && !strcmp(host, LOCAL_HOST_NAMEDPIPE)) ||
          (unix_socket && !strcmp(unix_socket, MYSQL_NAMEDPIPE)))
        return STATE_MACHINE_FAILED;
      /* Try also with TCP/IP */
    } else {
      net->vio = vio_new_win32pipe(hPipe);
      snprintf(ctx->host_info = ctx->buff, sizeof(ctx->buff) - 1,
               ER_CLIENT(CR_NAMEDPIPE_CONNECTION), unix_socket);
    }
  }
#endif
  DBUG_PRINT("info",
             ("net->vio: %p  protocol: %d", net->vio, mysql->options.protocol));
  if (!net->vio && (!mysql->options.protocol ||
                    mysql->options.protocol == MYSQL_PROTOCOL_TCP)) {
    struct addrinfo *res_lst, *client_bind_ai_lst = nullptr, hints, *t_res;
    char port_buf[NI_MAXSERV];
    my_socket sock = INVALID_SOCKET;
    int gai_errno, saved_error = 0, status = -1, bind_result = 0;
    const uint flags = VIO_BUFFERED_READ;

    unix_socket = nullptr; /* This is not used */

    if (!port) port = mysql_port;

    if (!host) host = LOCAL_HOST;

    snprintf(ctx->host_info = ctx->buff, sizeof(ctx->buff) - 1,
             ER_CLIENT(CR_TCP_CONNECTION), host);
    DBUG_PRINT("info", ("Server name: '%s'.  TCP sock: %d", host, port));

    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_family = AF_UNSPEC;

    DBUG_PRINT("info", ("IPV6 getaddrinfo %s", host));
    snprintf(port_buf, NI_MAXSERV, "%d", port);
    gai_errno = getaddrinfo(host, port_buf, &hints, &res_lst);

    DBUG_EXECUTE_IF("vio_client_use_localhost", {
      assert(strlen(host) == 255);
      gai_errno = getaddrinfo(LOCAL_HOST, port_buf, &hints, &res_lst);
    });

    if (gai_errno != 0) {
      DBUG_PRINT("info", ("IPV6 getaddrinfo error %d", gai_errno));
      set_mysql_extended_error(mysql, CR_UNKNOWN_HOST, unknown_sqlstate,
                               ER_CLIENT(CR_UNKNOWN_HOST), host, gai_errno);
      return STATE_MACHINE_FAILED;
    }

    /* Get address info for client bind name if it is provided */
    if (mysql->options.bind_address) {
      int bind_gai_errno = 0;

      DBUG_PRINT("info", ("Resolving addresses for client bind: '%s'",
                          mysql->options.bind_address));
      /* Lookup address info for name */
      bind_gai_errno = getaddrinfo(mysql->options.bind_address, nullptr, &hints,
                                   &client_bind_ai_lst);
      if (bind_gai_errno) {
        DBUG_PRINT("info",
                   ("client bind getaddrinfo error %d", bind_gai_errno));
        set_mysql_extended_error(mysql, CR_UNKNOWN_HOST, unknown_sqlstate,
                                 ER_CLIENT(CR_UNKNOWN_HOST),
                                 mysql->options.bind_address, bind_gai_errno);

        freeaddrinfo(res_lst);
        return STATE_MACHINE_FAILED;
      }
      DBUG_PRINT("info", ("  got address info for client bind name"));
    }

    /*
      A hostname might map to multiple IP addresses (IPv4/IPv6). Go over the
      list of IP addresses until a successful connection can be established.
      For each IP address, attempt to bind the socket to each client address
      for the client-side bind hostname until the bind is successful.
    */
    DBUG_PRINT("info", ("Try connect on all addresses for host."));
    for (t_res = res_lst; t_res; t_res = t_res->ai_next) {
      DBUG_PRINT("info",
                 ("Create socket, family: %d  type: %d  proto: %d",
                  t_res->ai_family, t_res->ai_socktype, t_res->ai_protocol));

      sock = socket(t_res->ai_family, t_res->ai_socktype, t_res->ai_protocol);
      if (sock == INVALID_SOCKET) {
        DBUG_PRINT("info", ("Socket created was invalid"));
        /* Try next address if there is one */
        saved_error = socket_errno;
        continue;
      }

      if (client_bind_ai_lst) {
        struct addrinfo *curr_bind_ai = nullptr;
        DBUG_PRINT("info", ("Attempting to bind socket to bind address(es)"));

        /*
           We'll attempt to bind to each of the addresses returned, until
           we find one that works.
           If none works, we'll try the next destination host address
           (if any)
        */
        curr_bind_ai = client_bind_ai_lst;

        while (curr_bind_ai != nullptr) {
          /* Attempt to bind the socket to the given address */
          bind_result = bind(sock, curr_bind_ai->ai_addr,
                             static_cast<int>(curr_bind_ai->ai_addrlen));
          if (!bind_result) break; /* Success */

          DBUG_PRINT("info", ("bind failed, attempting another bind address"));
          /* Problem with the bind, move to next address if present */
          curr_bind_ai = curr_bind_ai->ai_next;
        }

        if (bind_result) {
          /*
            Could not bind to any client-side address with this destination
             Try the next destination address (if any)
          */
          DBUG_PRINT("info", ("All bind attempts with this address failed"));
          saved_error = socket_errno;
          closesocket(sock);
          continue;
        }
        DBUG_PRINT("info", ("Successfully bound client side of socket"));
      }

      /* Create a new Vio object to abstract the socket. */
      if (!net->vio) {
        if (!(net->vio = vio_new(sock, VIO_TYPE_TCPIP, flags))) {
          set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
          closesocket(sock);
          freeaddrinfo(res_lst);
          if (client_bind_ai_lst) freeaddrinfo(client_bind_ai_lst);
          return STATE_MACHINE_FAILED;
        }
      }
      /* Just reinitialize if one is already allocated. */
      else if (vio_reset(net->vio, VIO_TYPE_TCPIP, sock, nullptr, flags)) {
        set_mysql_error(mysql, CR_UNKNOWN_ERROR, unknown_sqlstate);
        closesocket(sock);
        freeaddrinfo(res_lst);
        if (client_bind_ai_lst) freeaddrinfo(client_bind_ai_lst);
        return STATE_MACHINE_FAILED;
      }

      if (ctx->non_blocking)
        vio_set_blocking_flag(net->vio, !ctx->non_blocking);

      DBUG_PRINT("info", ("Connect socket"));

      if (mysql->options.extension && mysql->options.extension->retry_count)
        my_net_set_retry_count(net, mysql->options.extension->retry_count);

      status = vio_socket_connect(
          net->vio, t_res->ai_addr, (socklen_t)t_res->ai_addrlen,
          ctx->non_blocking, get_vio_connect_timeout(mysql), &connect_done);
      /*
        Here we rely on vio_socket_connect() to return success only if
        the connect attempt was really successful. Otherwise we would
        stop trying another address, believing we were successful.
      */
      if (!status) break;

      /*
        Save either the socket error status or the error code of
        the failed vio_connection operation. It is necessary to
        avoid having it overwritten by later operations.
      */
      saved_error = socket_errno;

      DBUG_PRINT("info", ("No success, try next address."));
    }
    DBUG_PRINT("info", ("End of connect attempts, sock: " MY_SOCKET_FMT
                        "  status: %d  error: %d",
                        sock, status, saved_error));

    freeaddrinfo(res_lst);
    if (client_bind_ai_lst) freeaddrinfo(client_bind_ai_lst);

    if (sock == INVALID_SOCKET) {
      set_mysql_extended_error(mysql, CR_IPSOCK_ERROR, unknown_sqlstate,
                               ER_CLIENT(CR_IPSOCK_ERROR), saved_error);
      return STATE_MACHINE_FAILED;
    }

    if (status) {
      DBUG_PRINT("error",
                 ("Got error %d on connect to '%s'", saved_error, host));
      set_mysql_extended_error(mysql, CR_CONN_HOST_ERROR, unknown_sqlstate,
                               ER_CLIENT(CR_CONN_HOST_ERROR), host, port,
                               saved_error);
      return STATE_MACHINE_FAILED;
    }
  }

  ctx->state_function = connect_done ? csm_complete_connect : csm_wait_connect;
  ctx->host = host;
  ctx->user = user;
  ctx->passwd = passwd;
  ctx->db = db;
  ctx->port = port;
  ctx->unix_socket = unix_socket;
  ctx->client_flag = client_flag;
  return STATE_MACHINE_CONTINUE;
}

/**
  Wait for async connect attempt to complete.
*/
static mysql_state_machine_status csm_wait_connect(mysql_async_connect *ctx) {
  NET *net = &(ctx->mysql->net);
  MYSQL_VIO vio = net->vio;
  int timeout_ms = 1;  // this is 1ms: the smallest non-zero timeout we can use
  int ret;

  DBUG_TRACE;

  DBUG_PRINT(
      "enter",
      ("host: %s  db: %s  user: %s (client)", ctx->host ? ctx->host : "(Null)",
       ctx->db ? ctx->db : "(Null)", ctx->user ? ctx->user : "(Null)"));

  if (!net->vio) {
    DBUG_PRINT("error", ("Unknown protocol %d", ctx->mysql->options.protocol));
    set_mysql_error(ctx->mysql, CR_CONN_UNKNOW_PROTOCOL, unknown_sqlstate);
    return STATE_MACHINE_FAILED;
  }

  /*
    The connect() is in progress. The vio_io_wait() with the smallest non-zero
    timeout possible can be used to peek if connect() completed.

    If vio_io_wait() returns 0,
    the socket never became writable or there is timeout and we'll return
    to caller.
    Otherwise, if vio_io_wait() returns 1, then one of two conditions
    exist:

    1. An error occurred. Use getsockopt() to check for this.
    2. The connection was set up successfully: getsockopt() will
       return 0 as an error.
  */

  const int io_wait_ret = vio_io_wait(vio, VIO_IO_EVENT_CONNECT, timeout_ms);
  if (io_wait_ret == 0) return STATE_MACHINE_WOULD_BLOCK;
  if (io_wait_ret == -1) return STATE_MACHINE_FAILED;

  int error;
  IF_WIN(int, socklen_t) optlen = sizeof(error);
  IF_WIN(char, void) *optval = (IF_WIN(char, void) *)&error;

  /*
    At this point, we know that something happened on the socket.
    But this does not means that everything is alright. The connect
    might have failed. We need to retrieve the error code from the
    socket layer. We must return success only if we are sure that
    it was really a success. Otherwise we might prevent the caller
    from trying another address to connect to.
  */
  DBUG_PRINT("info", ("Connect to '%s' completed", ctx->host));
  ctx->state_function = csm_complete_connect;
  if (!(ret = mysql_socket_getsockopt(vio->mysql_socket, SOL_SOCKET, SO_ERROR,
                                      optval, &optlen))) {
#ifdef _WIN32
    WSASetLastError(error);
#else
    errno = error;
#endif
    if (error != 0) {
      DBUG_PRINT("error",
                 ("Got error %d on connect to '%s'", error, ctx->host));
      set_mysql_extended_error(ctx->mysql, CR_CONN_HOST_ERROR, unknown_sqlstate,
                               ER_CLIENT(CR_CONN_HOST_ERROR), ctx->host,
                               ctx->port, error);
      return STATE_MACHINE_FAILED;
    }
  }
  return STATE_MACHINE_CONTINUE;
}

/**
  Complete the connection itself, setting options on the now-connected socket.
*/
static mysql_state_machine_status csm_complete_connect(
    mysql_async_connect *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  NET *net = &mysql->net;
  DBUG_PRINT("info", ("net->vio: %p", net->vio));
  if (!net->vio) {
    DBUG_PRINT("error", ("Unknow protocol %d ", mysql->options.protocol));
    set_mysql_error(mysql, CR_CONN_UNKNOW_PROTOCOL, unknown_sqlstate);
    return STATE_MACHINE_FAILED;
  }

  if (my_net_init(net, net->vio)) {
    vio_delete(net->vio);
    net->vio = nullptr;
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    return STATE_MACHINE_FAILED;
  }
  vio_keepalive(net->vio, true);

  /* If user set read_timeout, let it override the default */
  if (mysql->options.read_timeout)
    my_net_set_read_timeout(net, mysql->options.read_timeout);

  /* If user set write_timeout, let it override the default */
  if (mysql->options.write_timeout)
    my_net_set_write_timeout(net, mysql->options.write_timeout);

  /* If user set retry_count, let it override the default */
  if (mysql->options.extension && mysql->options.extension->retry_count)
    my_net_set_retry_count(net, mysql->options.extension->retry_count);

  if (mysql->options.max_allowed_packet)
    net->max_packet_size = mysql->options.max_allowed_packet;

  MYSQL_TRACE(CONNECTED, mysql, ());
  MYSQL_TRACE_STAGE(mysql, WAIT_FOR_INIT_PACKET);

  /* Get version info */
  mysql->protocol_version = PROTOCOL_VERSION; /* Assume this */
  if (mysql->options.connect_timeout && !ctx->non_blocking &&
      (vio_io_wait(net->vio, VIO_IO_EVENT_READ,
                   get_vio_connect_timeout(mysql)) < 1)) {
    set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                             ER_CLIENT(CR_SERVER_LOST_EXTENDED),
                             "waiting for initial communication packet",
                             socket_errno);
    return STATE_MACHINE_FAILED;
  }
  ctx->state_function = csm_read_greeting;
  return STATE_MACHINE_CONTINUE;
}

/**
  Read the greeting from the server that is read the first packet
*/
static mysql_state_machine_status csm_read_greeting(mysql_async_connect *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  DBUG_PRINT("info", ("Read first packet."));

  if (!ctx->non_blocking)
    ctx->pkt_length = cli_safe_read(mysql, nullptr);
  else {
    if (cli_safe_read_nonblocking(mysql, nullptr, &ctx->pkt_length) ==
        NET_ASYNC_NOT_READY) {
      return STATE_MACHINE_WOULD_BLOCK;
    }
  }
  if (ctx->pkt_length == packet_error) {
    if (mysql->net.last_errno == CR_SERVER_LOST)
      set_mysql_extended_error(mysql, CR_SERVER_LOST, unknown_sqlstate,
                               ER_CLIENT(CR_SERVER_LOST_EXTENDED),
                               "reading initial communication packet",
                               socket_errno);
    return STATE_MACHINE_FAILED;
  }
  ctx->state_function = csm_parse_handshake;
  return STATE_MACHINE_CONTINUE;
}

/**
  Parse the handshake from the server.
*/
static mysql_state_machine_status csm_parse_handshake(
    mysql_async_connect *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  NET *net = &mysql->net;
  const int pkt_length = ctx->pkt_length;
  int pkt_scramble_len = 0;
  char *end, *server_version_end, *pkt_end;
  pkt_end = (char *)net->read_pos + pkt_length;
  /* Check if version of protocol matches current one */
  mysql->protocol_version = net->read_pos[0];
  DBUG_DUMP("packet", (uchar *)net->read_pos, 10);
  DBUG_PRINT("info", ("mysql protocol version %d, server=%d", PROTOCOL_VERSION,
                      mysql->protocol_version));
  if (mysql->protocol_version != PROTOCOL_VERSION) {
    set_mysql_extended_error(mysql, CR_VERSION_ERROR, unknown_sqlstate,
                             ER_CLIENT(CR_VERSION_ERROR),
                             mysql->protocol_version, PROTOCOL_VERSION);
    return STATE_MACHINE_FAILED;
  }
  server_version_end = end = strend((char *)net->read_pos + 1);
  mysql->thread_id = uint4korr((uchar *)end + 1);
  end += 5;
  /*
    Scramble is split into two parts because old clients do not understand
    long scrambles; here goes the first part.
  */
  ctx->scramble_data = end;
  ctx->scramble_data_len = AUTH_PLUGIN_DATA_PART_1_LENGTH + 1;
  ctx->scramble_plugin = nullptr;
  end += ctx->scramble_data_len;

  if (pkt_end >= end + 1) mysql->server_capabilities = uint2korr((uchar *)end);
  if (pkt_end >= end + 18) {
    /* New protocol with 16 bytes to describe server characteristics */
    mysql->server_language = end[2];
    mysql->server_status = uint2korr((uchar *)end + 3);
    mysql->server_capabilities |= uint2korr((uchar *)end + 5) << 16;
    pkt_scramble_len = end[7];
    if (pkt_scramble_len < 0) {
      set_mysql_error(mysql, CR_MALFORMED_PACKET,
                      unknown_sqlstate); /* purecov: inspected */
      return STATE_MACHINE_FAILED;
    }
  }
  end += 18;

  if (mysql_init_character_set(mysql)) return STATE_MACHINE_FAILED;

  /* Save connection information */
  if (!my_multi_malloc(
          key_memory_MYSQL, MYF(0), &mysql->host_info,
          (uint)strlen(ctx->host_info) + 1, &mysql->host,
          (uint)strlen(ctx->host) + 1, &mysql->unix_socket,
          ctx->unix_socket ? (uint)strlen(ctx->unix_socket) + 1 : (uint)1,
          &mysql->server_version,
          (uint)(server_version_end - (char *)net->read_pos + 1), NullS) ||
      !(mysql->user = my_strdup(key_memory_MYSQL, ctx->user, MYF(0))) ||
      !(mysql->passwd = my_strdup(key_memory_MYSQL, ctx->passwd, MYF(0)))) {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    return STATE_MACHINE_FAILED;
  }
  my_stpcpy(mysql->host_info, ctx->host_info);
  my_stpcpy(mysql->host, ctx->host);
  if (ctx->unix_socket)
    my_stpcpy(mysql->unix_socket, ctx->unix_socket);
  else
    mysql->unix_socket = nullptr;
  my_stpcpy(mysql->server_version, (char *)net->read_pos + 1);
  mysql->port = ctx->port;

  if (pkt_end >= end + SCRAMBLE_LENGTH - AUTH_PLUGIN_DATA_PART_1_LENGTH + 1)

  {
    /*
     move the first scramble part - directly in the NET buffer -
     to get a full continuous scramble. We've read all the header,
     and can overwrite it now.
    */
    memmove(end - AUTH_PLUGIN_DATA_PART_1_LENGTH, ctx->scramble_data,
            AUTH_PLUGIN_DATA_PART_1_LENGTH);
    ctx->scramble_data = end - AUTH_PLUGIN_DATA_PART_1_LENGTH;
    if (mysql->server_capabilities & CLIENT_PLUGIN_AUTH) {
      ctx->scramble_data_len = pkt_scramble_len;
      ctx->scramble_plugin = ctx->scramble_data + ctx->scramble_data_len;
      /*
       There is a possibility that we did not get a correct plugin name
       for some reason. For example, the packet was malformed and some
       of the fields had incorrect values. In such cases, we keep the
       plugin name empty so that the default authentication plugin
       gets used later on. Since we don't really know the plugin for which
       the scramble_data was prepared, we can discard it and set it's length
       to 0.
      */
      if (ctx->scramble_data + ctx->scramble_data_len > pkt_end) {
        ctx->scramble_data = nullptr;
        ctx->scramble_data_len = 0;
        ctx->scramble_plugin = const_cast<char *>("");
      }
    } else {
      /**
        old server with no CLIENT_PLUGIN_AUTH support, so assume native auth
      */
      ctx->scramble_data_len = (int)(pkt_end - ctx->scramble_data);
      ctx->scramble_plugin = MYSQL_NATIVE_PASSWORD_PLUGIN_NAME;
    }
  } else {
    set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
    return STATE_MACHINE_FAILED;
  }
  ctx->state_function = csm_establish_ssl;
  return STATE_MACHINE_CONTINUE;
}

/**
  Establish SSL if needed.
*/
static mysql_state_machine_status csm_establish_ssl(mysql_async_connect *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  /* This check happens to work for both sync and async. */
  if (ctx->ssl_state == SSL_NONE) {
    MYSQL_TRACE(INIT_PACKET_RECEIVED, mysql,
                (ctx->pkt_length, mysql->net.read_pos));
    MYSQL_TRACE_STAGE(mysql, AUTHENTICATE);

#if defined(_WIN32)
    if ((mysql->options.extension &&
         mysql->options.extension->ssl_mode <= SSL_MODE_PREFERRED) &&
        (mysql->options.protocol == MYSQL_PROTOCOL_MEMORY ||
         mysql->options.protocol == MYSQL_PROTOCOL_PIPE)) {
      mysql->options.extension->ssl_mode = SSL_MODE_DISABLED;
    }
#endif
    /* try and bring up SSL if possible */
    cli_calculate_client_flag(mysql, ctx->db, ctx->client_flag);

    /*
      Allocate separate buffer for scramble data if we are going
      to attempt TLS connection. This would prevent a possible
      overwrite through my_net_write.
    */
    if (ctx->scramble_data_len && mysql->options.extension &&
        mysql->options.extension->ssl_mode != SSL_MODE_DISABLED) {
      if (!(ctx->scramble_buffer =
                (char *)my_malloc(key_memory_MYSQL_HANDSHAKE,
                                  ctx->scramble_data_len, MYF(MY_WME)))) {
        set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
        return STATE_MACHINE_FAILED;
      }
      ctx->scramble_buffer_allocated = true;
      memcpy(ctx->scramble_buffer, ctx->scramble_data, ctx->scramble_data_len);
    } else {
      ctx->scramble_buffer = ctx->scramble_data;
    }
  }
  if (ctx->non_blocking) {
    int ret;
    if (cli_establish_ssl_nonblocking(mysql, &ret) == NET_ASYNC_NOT_READY) {
      return STATE_MACHINE_WOULD_BLOCK;
    }
    if (ret) {
      return STATE_MACHINE_FAILED;
    }
  } else {
    if (cli_establish_ssl(mysql)) {
      return STATE_MACHINE_FAILED;
    }
  }

  ctx->state_function = csm_authenticate;
  return STATE_MACHINE_CONTINUE;
}

/**
  Invoke the authentication client plugin API to send the authentication
  data to the server
*/
static mysql_state_machine_status csm_authenticate(mysql_async_connect *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  if (ctx->non_blocking) {
    const mysql_state_machine_status status = run_plugin_auth_nonblocking(
        ctx->mysql, ctx->scramble_data, ctx->scramble_data_len,
        ctx->scramble_plugin, ctx->db);
    if (status != STATE_MACHINE_DONE) {
      return status;
    }
  } else {
    if (run_plugin_auth(mysql, ctx->scramble_buffer, ctx->scramble_data_len,
                        ctx->scramble_plugin, ctx->db)) {
      return STATE_MACHINE_FAILED;
    }
  }

  if (ctx->scramble_buffer_allocated) {
    ctx->scramble_buffer_allocated = false;
    my_free(ctx->scramble_buffer);
    ctx->scramble_buffer = nullptr;
  }

  ctx->state_function = csm_prep_select_database;
  return STATE_MACHINE_CONTINUE;
}

/**
  Authenticated, set initial database if specified
*/
static mysql_state_machine_status csm_prep_select_database(
    mysql_async_connect *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  NET *net = &mysql->net;

  MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);

  /* We will use compression */
  if ((mysql->client_flag & CLIENT_COMPRESS) ||
      (mysql->client_flag & CLIENT_ZSTD_COMPRESSION_ALGORITHM)) {
    net->compress = true;
    uint compress_level;
    const enum enum_compression_algorithm algorithm =
        mysql->client_flag & CLIENT_COMPRESS ? MYSQL_ZLIB : MYSQL_ZSTD;
    if (mysql->options.extension &&
        mysql->options.extension->zstd_compression_level)
      compress_level = mysql->options.extension->zstd_compression_level;
    else
      compress_level = mysql_default_compression_level(algorithm);
#ifndef MYSQL_SERVER
    NET_EXTENSION *ext = NET_EXTENSION_PTR(net);
    assert(ext != nullptr);
    mysql_compress_context_init(&ext->compress_ctx, algorithm, compress_level);
#else
    NET_SERVER *server_ext = static_cast<NET_SERVER *>(net->extension);
    if (server_ext == nullptr) {
      server_ext =
          static_cast<NET_SERVER *>(MYSQL_EXTENSION_PTR(mysql)->server_extn);
      net->extension = server_ext;
    }
    assert(server_ext != nullptr);
    mysql_compress_context_init(&server_ext->compress_ctx, algorithm,
                                compress_level);
#endif
  }
#ifdef CHECK_LICENSE
  if (check_license(mysql)) return STATE_MACHINE_FAILED;
#endif

#ifdef MYSQL_SERVER
  return STATE_MACHINE_DONE;
#else
  ctx->state_function = csm_prep_init_commands;
#endif

  return STATE_MACHINE_CONTINUE;
}

#ifndef MYSQL_SERVER
/**
  Prepare to send a sequence of init commands.
*/
static mysql_state_machine_status csm_prep_init_commands(
    mysql_async_connect *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;
  if (!mysql->options.init_commands) {
    return STATE_MACHINE_DONE;
  }

  ctx->saved_reconnect = mysql->reconnect;
  mysql->reconnect = false;
  ctx->current_init_command = mysql->options.init_commands->begin();

  ctx->state_function = csm_send_one_init_command;
  return STATE_MACHINE_CONTINUE;
}

/**
  Send an init command.  This is called once per init command until
  they've all been run (or a failure occurs).
*/
static mysql_state_machine_status csm_send_one_init_command(
    mysql_async_connect *ctx) {
  DBUG_TRACE;
  MYSQL *mysql = ctx->mysql;

  if (mysql_real_query(mysql, *ctx->current_init_command,
                       (ulong)strlen(*ctx->current_init_command)))
    return STATE_MACHINE_FAILED;
  int status;
  do {
    if (mysql->fields) {
      MYSQL_RES *res;
      if (!(res = cli_use_result(mysql))) return STATE_MACHINE_FAILED;
      mysql_free_result(res);
    }
    if ((status = mysql_next_result(mysql)) > 0) return STATE_MACHINE_FAILED;
  } while (status == 0);

  ++ctx->current_init_command;
  if (ctx->current_init_command < mysql->options.init_commands->end()) {
    return STATE_MACHINE_CONTINUE;
  }
  mysql->reconnect = ctx->saved_reconnect;
  DBUG_PRINT("exit", ("Mysql handler: %p", mysql));
  return STATE_MACHINE_DONE;
}
#endif

bool mysql_reconnect(MYSQL *mysql) {
  MYSQL tmp_mysql;
  DBUG_TRACE;
  assert(mysql);
  DBUG_PRINT("enter", ("mysql->reconnect: %d", mysql->reconnect));

  if ((mysql->server_status & SERVER_STATUS_IN_TRANS) || !mysql->host_info) {
    /* Allow reconnect next time */
    mysql->server_status &= ~SERVER_STATUS_IN_TRANS;
    if (mysql->net.last_errno == 0)
      set_mysql_error(mysql, CR_SERVER_LOST, unknown_sqlstate);
    return true;
  }
  mysql_init(&tmp_mysql);
  mysql_close_free_options(&tmp_mysql);
  tmp_mysql.options = mysql->options;
  tmp_mysql.options.my_cnf_file = tmp_mysql.options.my_cnf_group = nullptr;
#ifdef MYSQL_SERVER
  NET_SERVER *server_extn = MYSQL_EXTENSION_PTR(&tmp_mysql)->server_extn =
      MYSQL_EXTENSION_PTR(mysql)->server_extn;
  MYSQL_EXTENSION_PTR(mysql)->server_extn = nullptr;
#endif
  if (!mysql_real_connect(&tmp_mysql, mysql->host, mysql->user, mysql->passwd,
                          mysql->db, mysql->port, mysql->unix_socket,
                          mysql->client_flag | CLIENT_REMEMBER_OPTIONS)) {
#ifdef MYSQL_SERVER
    MYSQL_EXTENSION_PTR(mysql)->server_extn = server_extn;
#endif
    memset(&tmp_mysql.options, 0, sizeof(tmp_mysql.options));
    mysql->net.last_errno = tmp_mysql.net.last_errno;
    my_stpcpy(mysql->net.last_error, tmp_mysql.net.last_error);
    my_stpcpy(mysql->net.sqlstate, tmp_mysql.net.sqlstate);
    return true;
  }
  if (mysql_set_character_set(&tmp_mysql, mysql->charset->csname)) {
    DBUG_PRINT("error", ("mysql_set_character_set() failed"));
#ifdef MYSQL_SERVER
    MYSQL_EXTENSION_PTR(mysql)->server_extn = server_extn;
#endif
    memset(&tmp_mysql.options, 0, sizeof(tmp_mysql.options));
    mysql_close(&tmp_mysql);
    mysql->net.last_errno = tmp_mysql.net.last_errno;
    my_stpcpy(mysql->net.last_error, tmp_mysql.net.last_error);
    my_stpcpy(mysql->net.sqlstate, tmp_mysql.net.sqlstate);
    return true;
  }

  DBUG_PRINT("info", ("reconnect succeded"));
  tmp_mysql.reconnect = true;
  tmp_mysql.free_me = mysql->free_me;

  /* Move prepared statements (if any) over to the new mysql object */
  tmp_mysql.stmts = mysql->stmts;
  mysql->stmts = nullptr;

  /* Don't free options as these are now used in tmp_mysql */
  memset(&mysql->options, 0, sizeof(mysql->options));
  mysql->free_me = false;
  mysql_close(mysql);
  *mysql = std::move(tmp_mysql);
  net_clear(&mysql->net, true);
  mysql->affected_rows = ~(my_ulonglong)0;
  return false;
}

/**
  Open a new replication stream.

  Compose and send COM_BINLOG_DUMP[_GTID] command
  using information in the MYSQL_RPL structure.

  Caller must set the following MYSQL_RPL's slots:
  file_name_length, file_name, start_positions, server_id, flags
  and in case of MYSQL_RPL_GTID: gtid_set_size, gtid_set
  or fix_gtid_set/fix_gtid_set_arg which is used to compose command packet.

  Note: we treat NULL rpl->file_name as an empty string.
  If rpl->file_name_length is 0, strlen(rpl->file_name)
  will be called to set it.
  If rpl->fix_gtid_set is not NULL it will be called to fill
  packet gtid set data (rpl->gtid_set is ignored).

  @param  mysql  Connection handle.
  @param  rpl    Replication stream information.

  @retval  -1  Error.
  @retval  0   Success.
*/
int STDCALL mysql_binlog_open(MYSQL *mysql, MYSQL_RPL *rpl) {
  DBUG_TRACE;
  assert(mysql);
  assert(rpl);

  enum enum_server_command command;
  uchar *command_buffer = nullptr;
  size_t command_size = 0;

  /*
    No need to check mysql->net.vio here as
    it'll be checked in the simple_command().
  */

  if (!rpl->file_name) {
    rpl->file_name = const_cast<char *>("");
    rpl->file_name_length = 0;
  } else if (rpl->file_name_length == 0)
    rpl->file_name_length = strlen(rpl->file_name);

  if (rpl->file_name_length > UINT_MAX) {
    set_mysql_error(mysql, CR_FILE_NAME_TOO_LONG, unknown_sqlstate);
    return -1;
  }

  if (rpl->flags & MYSQL_RPL_GTID) {
    command = COM_BINLOG_DUMP_GTID;

#define GTID_ENCODED_DATA_SIZE 8

    const size_t alloc_size =
        rpl->file_name_length + ::BINLOG_FLAGS_INFO_SIZE +
        ::BINLOG_SERVER_ID_INFO_SIZE + ::BINLOG_NAME_SIZE_INFO_SIZE +
        ::BINLOG_POS_INFO_SIZE + ::BINLOG_DATA_SIZE_INFO_SIZE +
        (rpl->gtid_set_encoded_size ? rpl->gtid_set_encoded_size
                                    : GTID_ENCODED_DATA_SIZE) +
        1;

    if (!(command_buffer = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, alloc_size,
                                              MYF(MY_WME)))) {
      set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
      return -1;
    }

    uchar *ptr = command_buffer;

    int2store(ptr, rpl->flags);  // Note: we use low 16 bits
    ptr += ::BINLOG_FLAGS_INFO_SIZE;
    int4store(ptr, rpl->server_id);
    ptr += ::BINLOG_SERVER_ID_INFO_SIZE;
    int4store(ptr, static_cast<uint32>(rpl->file_name_length));
    ptr += ::BINLOG_NAME_SIZE_INFO_SIZE;
    memcpy(ptr, rpl->file_name, rpl->file_name_length);
    ptr += rpl->file_name_length;
    int8store(ptr, rpl->start_position);
    ptr += ::BINLOG_POS_INFO_SIZE;
    if (rpl->gtid_set_encoded_size) {
      int4store(ptr, static_cast<uint32>(rpl->gtid_set_encoded_size));
      ptr += ::BINLOG_DATA_SIZE_INFO_SIZE;
      if (rpl->fix_gtid_set)
        rpl->fix_gtid_set(rpl, ptr);
      else
        memcpy(ptr, rpl->gtid_set_arg, rpl->gtid_set_encoded_size);
      ptr += rpl->gtid_set_encoded_size;
    } else {
      /* No GTID set data, store 0 as its length. */
      int4store(ptr, static_cast<uint32>(GTID_ENCODED_DATA_SIZE));
      ptr += ::BINLOG_DATA_SIZE_INFO_SIZE;
      int8store(ptr, static_cast<uint64>(0));
      ptr += GTID_ENCODED_DATA_SIZE;
    }

    command_size = ptr - command_buffer;
    assert(command_size == (alloc_size - 1));
  } else {
    command = COM_BINLOG_DUMP;
    const size_t alloc_size =
        rpl->file_name_length + ::BINLOG_POS_OLD_INFO_SIZE +
        ::BINLOG_FLAGS_INFO_SIZE + ::BINLOG_SERVER_ID_INFO_SIZE + 1;

    if (!(command_buffer = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, alloc_size,
                                              MYF(MY_WME)))) {
      set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
      return -1;
    }

    uchar *ptr = command_buffer;

    /*
      COM_BINLOG_DUMP accepts only 4 bytes for the position, so
      we are forced to cast to uint32.
    */
    int4store(ptr, (uint32)rpl->start_position);
    ptr += ::BINLOG_POS_OLD_INFO_SIZE;
    int2store(ptr, rpl->flags);  // note: we use low 16 bits
    ptr += ::BINLOG_FLAGS_INFO_SIZE;
    int4store(ptr, rpl->server_id);
    ptr += ::BINLOG_SERVER_ID_INFO_SIZE;
    memcpy(ptr, rpl->file_name, rpl->file_name_length);
    ptr += rpl->file_name_length;

    command_size = ptr - command_buffer;
    assert(command_size == (alloc_size - 1));
  }

  if (simple_command(mysql, command, command_buffer, command_size, 1)) {
    my_free(command_buffer);
    return -1;
  }

  my_free(command_buffer);

  return 0;
}

/**
  Fetch one event from the server.

  Read one packet and check its validity,
  set rpl->buffer and rpl->size accordingly.

  @param  mysql  Connection handle.
  @param  rpl    Replication stream information.

  @retval  -1    Got error packet.
  @retval  0     Success.
*/
int STDCALL mysql_binlog_fetch(MYSQL *mysql, MYSQL_RPL *rpl) {
  DBUG_TRACE;
  assert(mysql);
  assert(rpl);

  for (;;) {
    /* Read a packet from the server. */
    const ulong packet_len = cli_safe_read(mysql, nullptr);

    NET *net = &mysql->net;

    /* Check if error packet. */
    if (packet_len == packet_error || packet_len == 0) {
      return -1;
    }
    /* Check if EOF packet. */
    else if (packet_len < 8 && net->read_pos[0] == 254) {
      rpl->size = 0;
      return 0;
    }

    /* Normal packet. */
    if (rpl->flags & MYSQL_RPL_SKIP_HEARTBEAT) {
      const Log_event_type event_type =
          (Log_event_type)net->read_pos[1 + EVENT_TYPE_OFFSET];
      if ((event_type == mysql::binlog::event::HEARTBEAT_LOG_EVENT) ||
          (event_type == mysql::binlog::event::HEARTBEAT_LOG_EVENT_V2))
        continue;
    }

    rpl->buffer = net->read_pos;
    rpl->size = packet_len;
    return 0;
  }
}

/**
  Close replication stream.

  @param  mysql  Connection handle.
  @param  rpl    Replication stream information.
*/
void STDCALL mysql_binlog_close(MYSQL *mysql, MYSQL_RPL *rpl) {
  DBUG_TRACE;
  assert(mysql);
  assert(rpl);

  end_server(mysql);

  rpl->buffer = nullptr;
  rpl->size = 0;
}

/**************************************************************************
  Set current database
**************************************************************************/

int STDCALL mysql_select_db(MYSQL *mysql, const char *db) {
  int error;
  DBUG_TRACE;
  DBUG_PRINT("enter", ("db: '%s'", db));

  if ((error = simple_command(mysql, COM_INIT_DB, (const uchar *)db,
                              (ulong)strlen(db), 0)))
    return error;
  my_free(mysql->db);
  mysql->db = my_strdup(key_memory_MYSQL, db, MYF(MY_WME));
  return 0;
}

/*************************************************************************
  Send a QUIT to the server and close the connection
  If handle is allocated by mysql connect free it.
*************************************************************************/

void mysql_close_free_options(MYSQL *mysql) {
  DBUG_TRACE;

  my_free(mysql->options.user);
  my_free(mysql->options.host);
  my_free(mysql->options.password);
  my_free(mysql->options.unix_socket);
  my_free(mysql->options.db);
  my_free(mysql->options.my_cnf_file);
  my_free(mysql->options.my_cnf_group);
  my_free(mysql->options.charset_dir);
  my_free(mysql->options.charset_name);
  my_free(mysql->options.bind_address);
  if (mysql->options.init_commands) {
    char **ptr = mysql->options.init_commands->begin();
    char **end = mysql->options.init_commands->end();
    for (; ptr < end; ptr++) my_free(*ptr);
    mysql->options.init_commands->~Init_commands_array();
    my_free(mysql->options.init_commands);
  }
  mysql_ssl_free(mysql);
#if defined(_WIN32)
  my_free(mysql->options.shared_memory_base_name);
#endif /* _WIN32 */
  if (mysql->options.extension) {
    my_free(mysql->options.extension->plugin_dir);
    my_free(mysql->options.extension->default_auth);
    my_free(mysql->options.extension->server_public_key_path);
    delete mysql->options.extension->connection_attributes;
    my_free(mysql->options.extension->compression_algorithm);
    my_free(mysql->options.extension->ssl_session_data);
    my_free(mysql->options.extension);
  }
  memset(&mysql->options, 0, sizeof(mysql->options));
}

/*
  Free all memory allocated in a MYSQL handle but preserve
  current options if any.
*/

void mysql_close_free(MYSQL *mysql) {
  my_free(mysql->host_info);
  my_free(mysql->user);
  my_free(mysql->passwd);
  my_free(mysql->db);

  /* Free extension if any */
  if (mysql->extension)
    mysql_extension_free(static_cast<MYSQL_EXTENSION *>(mysql->extension));

  my_free(mysql->field_alloc);

  if (mysql->connector_fd)
    free_vio_ssl_acceptor_fd(
        reinterpret_cast<st_VioSSLFd *>(mysql->connector_fd));
  mysql->connector_fd = nullptr;

  mysql->field_alloc = nullptr;

  /* Clear pointers for better safety */
  mysql->host_info = nullptr;
  mysql->host = nullptr;
  mysql->unix_socket = nullptr;
  mysql->server_version = nullptr;
  mysql->user = nullptr;
  mysql->passwd = nullptr;
  mysql->db = nullptr;
  mysql->extension = nullptr;
  mysql->thd = nullptr;
  mysql->charset = nullptr;
}

/**
  For use when the connection to the server has been lost (in which case
  the server has discarded all information about prepared statements
  associated with the connection).

  Mark all statements in mysql->stmts by setting stmt->mysql= 0 if the
  statement has transitioned beyond the MYSQL_STMT_INIT_DONE state, and
  unlink the statement from the mysql->stmts list.

  The remaining pruned list of statements (if any) is kept in mysql->stmts.

  @param mysql       pointer to the MYSQL object
*/
static void mysql_prune_stmt_list(MYSQL *mysql) {
  LIST *pruned_list = nullptr;

  while (mysql->stmts) {
    LIST *element = mysql->stmts;
    MYSQL_STMT *stmt;

    mysql->stmts = list_delete(element, element);
    stmt = (MYSQL_STMT *)element->data;
    if (stmt->state != MYSQL_STMT_INIT_DONE) {
      stmt->mysql = nullptr;
      stmt->last_errno = CR_SERVER_LOST;
      my_stpcpy(stmt->last_error, ER_CLIENT(CR_SERVER_LOST));
      my_stpcpy(stmt->sqlstate, unknown_sqlstate);
    } else {
      pruned_list = list_add(pruned_list, element);
    }
  }

  mysql->stmts = pruned_list;
}

/*
  Clear connection pointer of every statement: this is necessary
  to give error on attempt to use a prepared statement of closed
  connection.

  SYNOPSIS
    mysql_detach_stmt_list()
      stmt_list  pointer to mysql->stmts
      func_name  name of calling function

  NOTE
    There is similar code in mysql_reconnect(), so changes here
    should also be reflected there.
*/

void mysql_detach_stmt_list(LIST **stmt_list [[maybe_unused]],
                            const char *func_name [[maybe_unused]]) {
#ifndef MYSQL_SERVER
  /* Reset connection handle in all prepared statements. */
  LIST *element = *stmt_list;
  DBUG_TRACE;

  for (; element; element = element->next) {
    MYSQL_STMT *stmt = (MYSQL_STMT *)element->data;
    set_stmt_extended_error(stmt, CR_STMT_CLOSED, unknown_sqlstate,
                            ER_CLIENT(CR_STMT_CLOSED), func_name);
    stmt->mysql = nullptr;
    /* No need to call list_delete for statement here */
  }
  *stmt_list = nullptr;
  return;
#endif /* !MYSQL_SERVER */
}

void STDCALL mysql_close(MYSQL *mysql) {
  DBUG_TRACE;
  if (mysql) /* Some simple safety */
  {
    /* If connection is still up, send a QUIT message */
    if (mysql->net.vio != nullptr &&
        mysql->net.last_errno != NET_ERROR_SOCKET_UNUSABLE &&
        mysql->net.last_errno != NET_ERROR_SOCKET_NOT_WRITABLE) {
      free_old_query(mysql);
      mysql->status = MYSQL_STATUS_READY; /* Force command */
      const bool old_reconnect = mysql->reconnect;
      mysql->reconnect = false;  // avoid recursion
      if (vio_is_blocking(mysql->net.vio)) {
        simple_command(mysql, COM_QUIT, (uchar *)nullptr, 0, 1);
      } else {
        /*
          Best effort; try to toss a command on the wire, but we can't wait
          to hear back.
        */
        bool err; /* unused */
        simple_command_nonblocking(mysql, COM_QUIT, (uchar *)nullptr, 0, 1,
                                   &err);
      }
      mysql->reconnect = old_reconnect;
      end_server(mysql); /* Sets mysql->net.vio= 0 */
    }
    mysql_close_free(mysql);
    mysql_close_free_options(mysql);
    mysql_detach_stmt_list(&mysql->stmts, "mysql_close");
    if (mysql->free_me) {
      my_free(mysql);
    }
  }
}

static bool cli_read_query_result(MYSQL *mysql) {
  uchar *pos;
  ulong field_count;
  ulong length;
  DBUG_TRACE;

  if ((length = cli_safe_read(mysql, nullptr)) == packet_error) return true;
  free_old_query(mysql); /* Free old result */
#ifndef MYSQL_SERVER     /* Avoid warn of unused labels*/
get_info:
#endif
  pos = (uchar *)mysql->net.read_pos;
  if ((field_count = net_field_length(&pos)) == 0) {
    read_ok_ex(mysql, length);
#if defined(CLIENT_PROTOCOL_TRACING)
    if (mysql->server_status & SERVER_MORE_RESULTS_EXISTS)
      MYSQL_TRACE_STAGE(mysql, WAIT_FOR_RESULT);
    else
      MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
#endif
    return false;
  }
#ifndef MYSQL_SERVER
  if (field_count == NULL_LENGTH) /* LOAD DATA LOCAL INFILE */
  {
    int error;

    MYSQL_TRACE_STAGE(mysql, FILE_REQUEST);

    error = handle_local_infile(mysql, (char *)pos);

    MYSQL_TRACE_STAGE(mysql, WAIT_FOR_RESULT);

    if ((length = cli_safe_read(mysql, nullptr)) == packet_error || error)
      return true;
    goto get_info; /* Get info packet */
  }
#endif
  if (!(mysql->server_status & SERVER_STATUS_AUTOCOMMIT))
    mysql->server_status |= SERVER_STATUS_IN_TRANS;

  if (read_com_query_metadata(mysql, pos, field_count)) return true;

  mysql->status = MYSQL_STATUS_GET_RESULT;
  mysql->field_count = (uint)field_count;

  MYSQL_TRACE_STAGE(mysql, WAIT_FOR_ROW);

  DBUG_PRINT("exit", ("ok"));
  return false;
}
static net_async_status cli_read_query_result_nonblocking(MYSQL *mysql) {
  DBUG_TRACE;
  NET *net = &mysql->net;
  NET_ASYNC *net_async = NET_ASYNC_DATA(net);
  MYSQL_ASYNC *async_context = ASYNC_DATA(mysql);
  uchar *pos = nullptr;
  ulong field_count;
  ulong length;

  if (net_async->async_read_query_result_status ==
      NET_ASYNC_READ_QUERY_RESULT_IDLE) {
    net_async->async_read_query_result_status =
        NET_ASYNC_READ_QUERY_RESULT_FIELD_COUNT;
  }

  if (net_async->async_read_query_result_status ==
      NET_ASYNC_READ_QUERY_RESULT_FIELD_COUNT) {
    const net_async_status status =
        cli_safe_read_nonblocking(mysql, nullptr, &length);
    if (status == NET_ASYNC_NOT_READY) {
      return NET_ASYNC_NOT_READY;
    }
    if (length == packet_error) {
      if (NET_ASYNC_DATA(net) != nullptr)
        net_async->async_read_query_result_status =
            NET_ASYNC_READ_QUERY_RESULT_IDLE;
      async_context->async_op_status = ASYNC_OP_UNSET;
      async_context->async_query_state = QUERY_IDLE;
      async_context->async_query_length = 0;
      return NET_ASYNC_ERROR;
    }
    mysql->packet_length = length;

    free_old_query(mysql); /* Free old result */
#ifndef MYSQL_SERVER       /* Avoid warn of unused labels*/
  get_info:
#endif
    pos = (uchar *)mysql->net.read_pos;
    if ((field_count = net_field_length(&pos)) == 0) {
      read_ok_ex(mysql, length);
#if defined(CLIENT_PROTOCOL_TRACING)
      if (mysql->server_status & SERVER_MORE_RESULTS_EXISTS)
        MYSQL_TRACE_STAGE(mysql, WAIT_FOR_RESULT);
      else
        MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
#endif
      net_async->async_read_query_result_status =
          NET_ASYNC_READ_QUERY_RESULT_IDLE;
      async_context->async_op_status = ASYNC_OP_UNSET;
      async_context->async_query_state = QUERY_IDLE;
      async_context->async_query_length = 0;
      return NET_ASYNC_COMPLETE;
    }
#ifndef MYSQL_SERVER
    if (field_count == NULL_LENGTH) /* LOAD DATA LOCAL INFILE */
    {
      int error;

      MYSQL_TRACE_STAGE(mysql, FILE_REQUEST);

      if (!(mysql->options.client_flag & CLIENT_LOCAL_FILES)) {
        set_mysql_error(mysql, CR_MALFORMED_PACKET, unknown_sqlstate);
        net_async->async_read_query_result_status =
            NET_ASYNC_READ_QUERY_RESULT_IDLE;
        async_context->async_op_status = ASYNC_OP_UNSET;
        async_context->async_query_state = QUERY_IDLE;
        async_context->async_query_length = 0;
        return NET_ASYNC_ERROR;
      }

      error = handle_local_infile(mysql, (char *)pos);

      MYSQL_TRACE_STAGE(mysql, WAIT_FOR_RESULT);

      /* TODO: Make LOAD DATA LOCAL INFILE asynchronous. */
      if ((length = cli_safe_read(mysql, nullptr)) == packet_error || error) {
        /*
          When processing of LOAD DATA fails, server sends an error packet,
          which is handled here, for async connections.
        */
        if (NET_ASYNC_DATA(net) != nullptr)
          net_async->async_read_query_result_status =
              NET_ASYNC_READ_QUERY_RESULT_IDLE;
        async_context->async_op_status = ASYNC_OP_UNSET;
        async_context->async_query_state = QUERY_IDLE;
        async_context->async_query_length = 0;
        return NET_ASYNC_ERROR;
      }
      goto get_info; /* Get info packet */
    }
#endif
    if (!(mysql->server_status & SERVER_STATUS_AUTOCOMMIT))
      mysql->server_status |= SERVER_STATUS_IN_TRANS;

    mysql->field_count = (uint)field_count;
    net_async->async_read_query_result_status =
        NET_ASYNC_READ_QUERY_RESULT_FIELD_INFO;
  }

  if (net_async->async_read_query_result_status ==
      NET_ASYNC_READ_QUERY_RESULT_FIELD_INFO) {
    int res;
    const net_async_status status = read_com_query_metadata_nonblocking(
        mysql, pos, mysql->field_count, &res);
    if (status == NET_ASYNC_NOT_READY) {
      return NET_ASYNC_NOT_READY;
    }

    if (res) {
      net_async->async_read_query_result_status =
          NET_ASYNC_READ_QUERY_RESULT_IDLE;
      async_context->async_op_status = ASYNC_OP_UNSET;
      async_context->async_query_state = QUERY_IDLE;
      async_context->async_query_length = 0;
      return NET_ASYNC_ERROR;
    }
  }

  mysql->status = MYSQL_STATUS_GET_RESULT;
  DBUG_PRINT("exit", ("ok, %u", mysql->field_count));
  net_async->async_read_query_result_status = NET_ASYNC_READ_QUERY_RESULT_IDLE;
  async_context->async_op_status = ASYNC_OP_UNSET;
  async_context->async_query_state = QUERY_IDLE;
  async_context->async_query_length = 0;
  return NET_ASYNC_COMPLETE;
}

/**
  Helper function to serialize the parameters data.

  @param mysql the mysql handle
  @param[out] pret_data out pointer to the data. set to nullptr if there's no
  data
  @param[out] pret_data_length length of the data in pred_data. 0 if there's no
  data
  @return operation status
  @retval non-zero failed
  @retval zero success. Check pret_*
*/
static int mysql_prepare_com_query_parameters(MYSQL *mysql,
                                              unsigned char **pret_data,
                                              unsigned long *pret_data_length) {
  DBUG_TRACE;
  MYSQL_EXTENSION *ext = MYSQL_EXTENSION_PTR(mysql);
  assert(ext);
  const bool send_named_params =
      (mysql->server_capabilities & CLIENT_QUERY_ATTRIBUTES) != 0;
  *pret_data = nullptr;
  *pret_data_length = 0;
  if (send_named_params) {
    /*
      The state is checked later in cli_advanced_command too, but it's
      already too late since the below will reset the NET buffers.
      So we need to check before doing the below too.
    */
    if (mysql->status != MYSQL_STATUS_READY ||
        mysql->server_status & SERVER_MORE_RESULTS_EXISTS) {
      DBUG_PRINT("error", ("state: %d", mysql->status));
      set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
      return 1;
    }

    if (mysql->net.vio == nullptr) { /* Do reconnect if possible */
      if (!mysql->reconnect) {
        set_mysql_error(mysql, CR_SERVER_LOST, unknown_sqlstate);
        return 1;
      }
      if (mysql_reconnect(mysql)) return 1;
      /* mysql has a new ext at this point, take it again */
      ext = MYSQL_EXTENSION_PTR(mysql);
      assert(ext);
    }

    if (mysql_int_serialize_param_data(
            &mysql->net, ext->bind_info.n_params, ext->bind_info.bind,
            const_cast<const char **>(ext->bind_info.names), 1, pret_data,
            pret_data_length, 1, true, true, true)) {
      set_mysql_error(mysql, mysql->net.last_errno, mysql->net.sqlstate);
      return 1;
    }
    assert(ext == MYSQL_EXTENSION_PTR(mysql));
    assert(ext);
    mysql_extension_bind_free(ext);
  }
  return 0;
}
/*
Send the query and return so we can do something else.
Needs to be followed by mysql_read_query_result() when we want to
finish processing it.
*/

int STDCALL mysql_send_query(MYSQL *mysql, const char *query, ulong length) {
  DBUG_TRACE;

  const bool extension_was_present = (mysql->extension != nullptr);

  STATE_INFO *info;
  assert(mysql);

  MYSQL_EXTENSION *ext = MYSQL_EXTENSION_PTR(mysql);
  assert(ext);

  if ((info = STATE_DATA(mysql))) free_state_change_info(ext);
  uchar *ret_data;
  unsigned long ret_data_length;
  const int retval =
      mysql_prepare_com_query_parameters(mysql, &ret_data, &ret_data_length);
  /*
    mysql->extension is allocated memory inside mysql_init() before
    establishing a client-server connection. When MYSQL_EXTENSION_PTR() is
    called here in mysql_send_query() or in
    mysql_prepare_com_query_parameters() it allocates memory to
    mysql->extension if it is null (disconnected client). This memory is not
    freed later. When another query is executed mysql->extension is reallocated
    memory and this will cause a memory leak. To avoid this, mysql->extension
    is freed here if it was null when mysql_send_query was called and client is
    disconnected.
  */
  if (!extension_was_present && !mysql->net.vio) {
    mysql_extension_free(static_cast<MYSQL_EXTENSION *>(mysql->extension));
    mysql->extension = nullptr;
  }
  if (retval) return 1;
  int ret = (*mysql->methods->advanced_command)(
      mysql, COM_QUERY, ret_data, ret_data_length,
      pointer_cast<const uchar *>(query), length, true, nullptr);
  if (ret_data) my_free(ret_data);
  return ret;
}

/**
    Executes the SQL statement pointed by query. This API is called by
    mysql_real_query_nonblocking to send query to server in asynchronous way.

    @param[in]   mysql               connection handle
    @param[in]   query               query string to be executed
    @param[in]   length              length of query

    @retval      NET_ASYNC_ERROR     query execution failed
    @retval      NET_ASYNC_NOT_READY query not yet completed, call this API
    again
    @retval      NET_ASYNC_COMPLETE  query execution finished
  */
static net_async_status mysql_send_query_nonblocking_inner(MYSQL *mysql,
                                                           const char *query,
                                                           ulong length) {
  DBUG_TRACE;
  STATE_INFO *info;

  if ((info = STATE_DATA(mysql)))
    free_state_change_info(static_cast<MYSQL_EXTENSION *>(mysql->extension));

  bool ret;
  MYSQL_ASYNC *async_context = ASYNC_DATA(mysql);

  if ((*mysql->methods->advanced_command_nonblocking)(
          mysql, COM_QUERY, async_context->async_qp_data,
          async_context->async_qp_data_length,
          pointer_cast<const uchar *>(query), length, true, nullptr,
          &ret) == NET_ASYNC_NOT_READY) {
    return NET_ASYNC_NOT_READY;
  }
  if (ret)
    return NET_ASYNC_ERROR;
  else
    return NET_ASYNC_COMPLETE;
}

/**
  Wrapper around mysql_send_query_nonblocking_inner to be called externally

  @param[in]   mysql               connection handle
  @param[in]   query               query string to be executed
  @param[in]   length              length of query

  @retval      NET_ASYNC_ERROR     query execution failed
  @retval      NET_ASYNC_NOT_READY query not yet completed, call this API again
  @retval      NET_ASYNC_COMPLETE  query execution finished
*/
net_async_status STDCALL mysql_send_query_nonblocking(MYSQL *mysql,
                                                      const char *query,
                                                      ulong length) {
  DBUG_TRACE;
  MYSQL_ASYNC *async_context = ASYNC_DATA(mysql);
  net_async_status ret = NET_ASYNC_NOT_READY;
  DBUG_PRINT("async", ("enter mysql_send_query_nonblocking state=%d",
                       async_context->async_query_state));
  if (async_context->async_query_state == QUERY_IDLE) {
    assert(async_context->async_qp_data == nullptr);
    assert(async_context->async_qp_data_length == 0);

    async_context->async_query_length = length;
    async_context->async_query_state = QUERY_SENDING;
    async_context->async_op_status = ASYNC_OP_QUERY;
    DBUG_PRINT("async", ("set state=%d", async_context->async_query_state));

    if (mysql_prepare_com_query_parameters(
            mysql, &async_context->async_qp_data,
            &async_context->async_qp_data_length)) {
      set_query_idle(async_context);
      return NET_ASYNC_ERROR;
    }
  }

  ret = mysql_send_query_nonblocking_inner(mysql, query, length);

  if (ret == NET_ASYNC_NOT_READY)
    return ret;
  else if (ret == NET_ASYNC_ERROR) {
    set_query_idle(async_context);
    return ret;
  }

  async_context->async_query_state = QUERY_READING_RESULT;
  DBUG_PRINT("async", ("set state=%d", async_context->async_query_state));
  /*
    Technically we don't need to keep the query attributes data until the
    state change as they're stored into the NET at the first call to
    advanced_command. But since advanced_command() requires these we
    keep them as they are.
  */
  free_async_qp_data(async_context);
  return ret;
}

int STDCALL mysql_real_query(MYSQL *mysql, const char *query, ulong length) {
  int retval;
  DBUG_TRACE;
  DBUG_PRINT("enter", ("handle: %p", mysql));
  DBUG_PRINT("query", ("Query = '%-.*s'", (int)length, query));
  DBUG_EXECUTE_IF("inject_ER_NET_READ_INTERRUPTED", {
    mysql->net.last_errno = ER_NET_READ_INTERRUPTED;
    DBUG_SET("-d,inject_ER_NET_READ_INTERRUPTED");
    return 1;
  });

  if (mysql_send_query(mysql, query, length)) return 1;
  retval = (int)(*mysql->methods->read_query_result)(mysql);
  mysql_extension_bind_free(MYSQL_EXTENSION_PTR(mysql));
  return retval;
}

/**
  Executes the SQL statement pointed by query. This sql statement length is set
  in length parameter. query string can contain multiple sql statements
  separated by semicolons. This function can return immediately with status set
  to NET_ASYNC_NOT_READY, in this case client application is expected to call
  this API until it returns NET_ASYNC_COMPLETE.

  @param[in]   mysql               connection handle
  @param[in]   query               query string to be executed
  @param[in]   length              length of query

  @retval      NET_ASYNC_ERROR     query execution failed
  @retval      NET_ASYNC_NOT_READY query not yet completed, call this API again
  @retval      NET_ASYNC_COMPLETE  query execution finished
*/
net_async_status STDCALL mysql_real_query_nonblocking(MYSQL *mysql,
                                                      const char *query,
                                                      ulong length) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("handle: %p", mysql));
  DBUG_PRINT("query", ("Query = '%-.*s'", (int)length, query));
  DBUG_EXECUTE_IF("inject_ER_NET_READ_INTERRUPTED", {
    mysql->net.last_errno = ER_NET_READ_INTERRUPTED;
    DBUG_SET("-d,inject_ER_NET_READ_INTERRUPTED");
    return NET_ASYNC_ERROR;
  });
  MYSQL_ASYNC *async_context = ASYNC_DATA(mysql);
  assert(async_context->async_op_status == ASYNC_OP_UNSET ||
         async_context->async_op_status == ASYNC_OP_QUERY);

  net_async_status status = NET_ASYNC_NOT_READY;
  DBUG_PRINT("async", ("mysql_real_query_nonblocking start state=%d",
                       async_context->async_query_state));
  /* 1st phase: send query. */
  if (async_context->async_query_state == QUERY_IDLE) {
    assert(async_context->async_qp_data == nullptr);
    assert(async_context->async_qp_data_length == 0);
    if (mysql_prepare_com_query_parameters(
            mysql, &async_context->async_qp_data,
            &async_context->async_qp_data_length)) {
      status = NET_ASYNC_ERROR;
      goto end;
    }
    async_context->async_query_length = length;
    async_context->async_op_status = ASYNC_OP_QUERY;
    async_context->async_query_state = QUERY_SENDING;
    DBUG_PRINT("async", ("set state=%d", async_context->async_query_state));
  }

  if (async_context->async_query_state == QUERY_SENDING) {
    status = mysql_send_query_nonblocking_inner(mysql, query, length);
    if (status == NET_ASYNC_NOT_READY)
      return NET_ASYNC_NOT_READY;
    else if (status == NET_ASYNC_ERROR)
      goto end;
    async_context->async_query_state = QUERY_READING_RESULT;
    DBUG_PRINT("async", ("set state=%d", async_context->async_query_state));
    /*
      Technically we don't need to keep the query attributes data until the
      state change as they're stored into the NET at the first call to
      advanced_command. But since advanced_command() requires these we
      keep them as they are.
    */
    if (async_context->async_qp_data) {
      my_free(async_context->async_qp_data);
      async_context->async_qp_data = nullptr;
      async_context->async_qp_data_length = 0;
    }
  }

  /* 2nd phase: read query result (field count, field info) */
  if (async_context->async_query_state == QUERY_READING_RESULT) {
    status = (*mysql->methods->read_query_result_nonblocking)(mysql);
    if (status == NET_ASYNC_NOT_READY)
      return NET_ASYNC_NOT_READY;
    else if (status == NET_ASYNC_ERROR)
      goto end;
  }

end:
  async_context->async_op_status = ASYNC_OP_UNSET;
  async_context->async_query_state = QUERY_IDLE;
  DBUG_PRINT("async", ("set state=%d", async_context->async_query_state));
  async_context->async_query_length = 0;
  if (status == NET_ASYNC_ERROR)
    return NET_ASYNC_ERROR;
  else
    return NET_ASYNC_COMPLETE;
}

/**************************************************************************
  Alloc result struct for buffered results. All rows are read to buffer.
  mysql_data_seek may be used.
**************************************************************************/

MYSQL_RES *STDCALL mysql_store_result(MYSQL *mysql) {
  MYSQL_RES *result;
  DBUG_TRACE;

  /*
    Some queries (e.g. "CALL") may return an empty resultset.
    mysql->field_count is 0 in such cases.
  */
  if (!mysql->field_count) return nullptr;
  if (mysql->status != MYSQL_STATUS_GET_RESULT) {
    set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    return nullptr;
  }
  mysql->status = MYSQL_STATUS_READY; /* server is ready */
  if (!(result = (MYSQL_RES *)my_malloc(
            key_memory_MYSQL_RES,
            (uint)(sizeof(MYSQL_RES) + sizeof(ulong) * mysql->field_count),
            MYF(MY_WME | MY_ZEROFILL)))) {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    return nullptr;
  }
  if (!(result->field_alloc = (MEM_ROOT *)my_malloc(
            key_memory_MYSQL, sizeof(MEM_ROOT), MYF(MY_WME | MY_ZEROFILL)))) {
    set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
    my_free(result);
    return nullptr;
  }
  result->methods = mysql->methods;
  result->eof = true; /* Marker for buffered */
  result->lengths = (ulong *)(result + 1);
  if (!(result->data = (*mysql->methods->read_rows)(mysql, mysql->fields,
                                                    mysql->field_count))) {
    my_free(result->field_alloc);
    my_free(result);
    return nullptr;
  }
  mysql->affected_rows = result->row_count = result->data->rows;
  result->data_cursor = result->data->data;
  result->fields = mysql->fields;
  *result->field_alloc = std::move(*mysql->field_alloc);
  result->field_count = mysql->field_count;
  result->metadata = mysql->resultset_metadata;
  /* The rest of result members is zerofilled in my_malloc */
  mysql->fields = nullptr; /* fields is now in result */
  /* just in case this was mistakenly called after mysql_stmt_execute() */
  mysql->unbuffered_fetch_owner = nullptr;
  return result; /* Data fetched */
}

/**
  This API reads all result set sent by server in an asynchronous way

  @param[in]   mysql    connection handle
  @param[in]   result   buffer which holds all result sets.

  @retval      NET_ASYNC_NOT_READY    reading of result sets not complete
  @retval      NET_ASYNC_COMPLETE     completed this asynchronous operation
*/
enum net_async_status STDCALL
mysql_store_result_nonblocking(MYSQL *mysql, MYSQL_RES **result) {
  DBUG_TRACE;
  MYSQL_ASYNC *async_context = ASYNC_DATA(mysql);
  *result = nullptr;

  /*
    Some queries (e.g. "CALL") may return an empty resultset.
    mysql->field_count is 0 in such cases.
  */
  if (!mysql->field_count) {
    goto end;
  }
  if (!async_context->async_store_result_result) {
    if (mysql->status != MYSQL_STATUS_GET_RESULT) {
      set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
      goto end;
    }
    mysql->status = MYSQL_STATUS_READY; /* server is ready */

    if (!(async_context->async_store_result_result = (MYSQL_RES *)my_malloc(
              key_memory_MYSQL_RES,
              (uint)(sizeof(MYSQL_RES) + sizeof(ulong) * mysql->field_count),
              MYF(MY_WME | MY_ZEROFILL)))) {
      set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
      goto end;
    }
    if (!(async_context->async_store_result_result->field_alloc =
              (MEM_ROOT *)my_malloc(key_memory_MYSQL, sizeof(MEM_ROOT),
                                    MYF(MY_WME | MY_ZEROFILL)))) {
      set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
      my_free(async_context->async_store_result_result);
      goto end;
    }
    async_context->async_store_result_result->methods = mysql->methods;
    async_context->async_store_result_result->eof =
        true; /* Marker for buffered */
    async_context->async_store_result_result->lengths =
        (ulong *)(async_context->async_store_result_result + 1);
  }

  if ((*mysql->methods->read_rows_nonblocking)(
          mysql, mysql->fields, mysql->field_count,
          &async_context->async_store_result_result->data) ==
      NET_ASYNC_NOT_READY) {
    return NET_ASYNC_NOT_READY;
  }

  if (!async_context->async_store_result_result->data) {
    my_free(async_context->async_store_result_result->field_alloc);
    my_free(async_context->async_store_result_result);
    goto end;
  }
  mysql->affected_rows = async_context->async_store_result_result->row_count =
      async_context->async_store_result_result->data->rows;
  async_context->async_store_result_result->data_cursor =
      async_context->async_store_result_result->data->data;
  async_context->async_store_result_result->fields = mysql->fields;
  *async_context->async_store_result_result->field_alloc =
      std::move(*mysql->field_alloc);
  async_context->async_store_result_result->field_count = mysql->field_count;
  async_context->async_store_result_result->metadata =
      mysql->resultset_metadata;
  /* The rest of result members is zerofilled in my_malloc */
  mysql->fields = nullptr; /* fields is now in result */
  /* just in case this was mistakenly called after mysql_stmt_execute() */
  mysql->unbuffered_fetch_owner = nullptr;
  *result = async_context->async_store_result_result;
end:
  async_context->async_store_result_result = nullptr;
  return NET_ASYNC_COMPLETE;
}

/**************************************************************************
  Alloc struct for use with unbuffered reads. Data is fetched by domand
  when calling to mysql_fetch_row.
  mysql_data_seek is a noop.

  No other queries may be specified with the same MYSQL handle.
  There shouldn't be much processing per row because mysql server shouldn't
  have to wait for the client (and will not wait more than 30 sec/packet).
**************************************************************************/
MYSQL_RES *use_result(MYSQL *mysql) {
  MYSQL_RES *result;
  DBUG_TRACE;

  /*
    Some queries (e.g. "CALL") may return an empty resultset.
    mysql->field_count is 0 in such cases.
  */
  if (!mysql->field_count) return nullptr;
  if (mysql->status != MYSQL_STATUS_GET_RESULT) {
    set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    return nullptr;
  }
  if (!(result = (MYSQL_RES *)my_malloc(
            key_memory_MYSQL_RES,
            sizeof(*result) + sizeof(ulong) * mysql->field_count,
            MYF(MY_WME | MY_ZEROFILL))))
    return nullptr;
  result->lengths = (ulong *)(result + 1);
  result->methods = mysql->methods;
  if (!(result->row = (MYSQL_ROW)my_malloc(
            key_memory_MYSQL_ROW,
            sizeof(result->row[0]) * (mysql->field_count + 1),
            MYF(MY_WME)))) { /* Ptrs: to one row */
    my_free(result);
    return nullptr;
  }
  if (!(result->field_alloc = (MEM_ROOT *)my_malloc(
            key_memory_MYSQL, sizeof(MEM_ROOT), MYF(MY_WME | MY_ZEROFILL)))) {
    my_free(result->row);
    my_free(result);
    return nullptr;
  }
  result->fields = mysql->fields;
  *result->field_alloc = std::move(*mysql->field_alloc);
  result->field_count = mysql->field_count;
  result->metadata = mysql->resultset_metadata;
  result->current_field = 0;
  result->handle = mysql;
  result->current_row = nullptr;
  mysql->fields = nullptr; /* fields is now in result */
  mysql->status = MYSQL_STATUS_USE_RESULT;
  mysql->unbuffered_fetch_owner = &result->unbuffered_fetch_cancelled;
  return result; /* Data is read to be fetched */
}
static MYSQL_RES *cli_use_result(MYSQL *mysql) { return use_result(mysql); }

/**************************************************************************
  Return next row of the query results
**************************************************************************/

MYSQL_ROW cli_fetch_row(MYSQL_RES *res) {
  DBUG_TRACE;
  if (!res->data) { /* Unbufferred fetch */
    if (!res->eof) {
      MYSQL *mysql = res->handle;
      if (mysql->status != MYSQL_STATUS_USE_RESULT) {
        set_mysql_error(mysql,
                        res->unbuffered_fetch_cancelled
                            ? CR_FETCH_CANCELED
                            : CR_COMMANDS_OUT_OF_SYNC,
                        unknown_sqlstate);
      } else if (!(read_one_row(mysql, res->field_count, res->row,
                                res->lengths))) {
        res->row_count++;
        return res->current_row = res->row;
      }
      DBUG_PRINT("info", ("end of data"));
      res->eof = true;
      mysql->status = MYSQL_STATUS_READY;
      /*
        Reset only if owner points to us: there is a chance that somebody
        started new query after mysql_stmt_close():
      */
      if (mysql->unbuffered_fetch_owner == &res->unbuffered_fetch_cancelled)
        mysql->unbuffered_fetch_owner = nullptr;
      /* Don't clear handle in mysql_free_result */
      res->handle = nullptr;
    }
    return (MYSQL_ROW) nullptr;
  }
  {
    MYSQL_ROW tmp;
    if (!res->data_cursor) {
      DBUG_PRINT("info", ("end of data"));
      return res->current_row = (MYSQL_ROW) nullptr;
    }
    tmp = res->data_cursor->data;
    res->data_cursor = res->data_cursor->next;
    return res->current_row = tmp;
  }
}

MYSQL_ROW STDCALL mysql_fetch_row(MYSQL_RES *res) {
  if (res->methods) {
    return res->methods->fetch_row(res);
  } else {
    set_mysql_error(res->handle, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    return (MYSQL_ROW) nullptr;
  }
}

/**
  Reads next row of a result set in an asynchronous way.

  @param[in]    res     buffer in which all rows are stored
  @param[out]   row     return pointer to one row from result set

  @retval      NET_ASYNC_NOT_READY fetch operation not complete, retry again
  @retval      NET_ASYNC_COMPLETE  fetch operation complete
*/
net_async_status STDCALL mysql_fetch_row_nonblocking(MYSQL_RES *res,
                                                     MYSQL_ROW *row) {
  DBUG_TRACE;
  MYSQL *mysql = res->handle;
  *row = nullptr;

  if (!res->data) { /* Unbufferred fetch */
    if (!res->eof) {
      /*
        Can be -1 (error), 0 (success) and 1 (eof).
        Init to -1 so if state is not MYSQL_STATUS_USE_RESULT we get
        out of sync error.
      */
      int read_row_result = -1;
      if (mysql->status == MYSQL_STATUS_USE_RESULT) {
        if (read_one_row_nonblocking(mysql, res->field_count, res->row,
                                     res->lengths,
                                     &read_row_result) == NET_ASYNC_NOT_READY) {
          return NET_ASYNC_NOT_READY;
        }

        // we arrive here on NET_ASYNC_ERROR or NET_ASYNC_COMPLETE

        if (read_row_result == 0) {  // we've got a row: process it
          res->row_count++;
          *row = res->current_row = res->row;
          goto end;
        }
      }

      if (read_row_result == -1)  // on row reading error
        set_mysql_error(mysql,
                        res->unbuffered_fetch_cancelled
                            ? CR_FETCH_CANCELED
                            : CR_COMMANDS_OUT_OF_SYNC,
                        unknown_sqlstate);
      DBUG_PRINT("info", ("end of data"));
      res->eof = true;
      mysql->status = MYSQL_STATUS_READY;
      /*
        Reset only if owner points to us: there is a chance that
        somebody started new query after mysql_stmt_close():
      */
      if (mysql->unbuffered_fetch_owner == &res->unbuffered_fetch_cancelled)
        mysql->unbuffered_fetch_owner = nullptr;
      /* Don't clear handle in mysql_free_result */
      res->handle = nullptr;
    }

    *row = nullptr;
    goto end;
  }
  {
    MYSQL_ROW tmp;
    if (!res->data_cursor) {
      DBUG_PRINT("info", ("end of data"));
      *row = nullptr;
      goto end;
    }
    tmp = res->data_cursor->data;
    res->data_cursor = res->data_cursor->next;
    *row = res->current_row = tmp;
    goto end;
  }

end:
  return NET_ASYNC_COMPLETE;
}

/**************************************************************************
  Get column lengths of the current row
  If one uses mysql_use_result, res->lengths contains the length information,
  else the lengths are calculated from the offset between pointers.
**************************************************************************/

ulong *STDCALL mysql_fetch_lengths(MYSQL_RES *res) {
  MYSQL_ROW column;

  if (!(column = res->current_row)) return nullptr; /* Something is wrong */
  if (res->data)
    (*res->methods->fetch_lengths)(res->lengths, column, res->field_count);
  return res->lengths;
}

/**
  Validates, makes into an absolute path and sets @ref
  MYSQL_OPT_LOAD_DATA_LOCAL_DIR value

  @param mysql connection handle
  @param arg the value to set. Can be null

  @retval true failed
  @retval false success
*/
static bool set_load_data_local_infile_option(MYSQL *mysql, const char *arg) {
  char buff1[FN_REFLEN], buff2[FN_REFLEN];

  ENSURE_EXTENSIONS_PRESENT(&mysql->options);

  // NULL is a valid argument
  if (arg == nullptr || !arg[0]) {
    EXTENSION_SET_STRING(&mysql->options, load_data_dir, nullptr);
    return false;
  }

  // make fully qualified name
  if (my_realpath(buff1, arg, 0)) {
    char errbuf[MYSYS_STRERROR_SIZE];
    set_mysql_extended_error(
        mysql, CR_LOAD_DATA_LOCAL_INFILE_REALPATH_FAIL, unknown_sqlstate,
        ER_CLIENT(CR_LOAD_DATA_LOCAL_INFILE_REALPATH_FAIL), arg, my_errno(),
        my_strerror(errbuf, sizeof(errbuf), my_errno()));
    return true;
  }

  // with uniform directory separators
  convert_dirname(buff2, buff1, NullS);
  EXTENSION_SET_STRING(&mysql->options, load_data_dir, buff2);
  return false;
}

int STDCALL mysql_options(MYSQL *mysql, enum mysql_option option,
                          const void *arg) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("option: %d", (int)option));
  switch (option) {
    case MYSQL_OPT_CONNECT_TIMEOUT:
      mysql->options.connect_timeout = *static_cast<const uint *>(arg);
      break;
    case MYSQL_OPT_READ_TIMEOUT:
      mysql->options.read_timeout = *static_cast<const uint *>(arg);
      break;
    case MYSQL_OPT_WRITE_TIMEOUT:
      mysql->options.write_timeout = *static_cast<const uint *>(arg);
      break;
    case MYSQL_OPT_COMPRESS:
      mysql->options.compress = true; /* Remember for connect */
      mysql->options.client_flag |= CLIENT_COMPRESS;
      break;
    case MYSQL_OPT_NAMED_PIPE: /* This option is deprecated */
      mysql->options.protocol = MYSQL_PROTOCOL_PIPE; /* Force named pipe */
      break;
    case MYSQL_OPT_LOCAL_INFILE: /* Allow LOAD DATA LOCAL ?*/
      if (!arg || (*static_cast<const uint *>(arg) != 0))
        mysql->options.client_flag |= CLIENT_LOCAL_FILES;
      else
        mysql->options.client_flag &= ~CLIENT_LOCAL_FILES;
      break;
    case MYSQL_INIT_COMMAND:
      add_init_command(&mysql->options, static_cast<const char *>(arg));
      break;
    case MYSQL_READ_DEFAULT_FILE:
      my_free(mysql->options.my_cnf_file);
      mysql->options.my_cnf_file =
          my_strdup(key_memory_mysql_options, static_cast<const char *>(arg),
                    MYF(MY_WME));
      break;
    case MYSQL_READ_DEFAULT_GROUP:
      my_free(mysql->options.my_cnf_group);
      mysql->options.my_cnf_group =
          my_strdup(key_memory_mysql_options, static_cast<const char *>(arg),
                    MYF(MY_WME));
      break;
    case MYSQL_SET_CHARSET_DIR:
      my_free(mysql->options.charset_dir);
      mysql->options.charset_dir =
          my_strdup(key_memory_mysql_options, static_cast<const char *>(arg),
                    MYF(MY_WME));
      break;
    case MYSQL_SET_CHARSET_NAME:
      my_free(mysql->options.charset_name);
      mysql->options.charset_name =
          my_strdup(key_memory_mysql_options, static_cast<const char *>(arg),
                    MYF(MY_WME));
      break;
    case MYSQL_OPT_PROTOCOL:
      mysql->options.protocol = *static_cast<const uint *>(arg);
      break;
    case MYSQL_SHARED_MEMORY_BASE_NAME:
#if defined(_WIN32)
      my_free(mysql->options.shared_memory_base_name);

      mysql->options.shared_memory_base_name =
          my_strdup(key_memory_mysql_options, static_cast<const char *>(arg),
                    MYF(MY_WME));
#endif
      break;
    case MYSQL_REPORT_DATA_TRUNCATION:
      mysql->options.report_data_truncation = *static_cast<const bool *>(arg);
      break;
    case MYSQL_OPT_RECONNECT:
      fprintf(stderr,
              "WARNING: MYSQL_OPT_RECONNECT is deprecated and will be "
              "removed in a future version.\n");
      mysql->reconnect = *static_cast<const bool *>(arg);
      break;
    case MYSQL_OPT_BIND:
      my_free(mysql->options.bind_address);
      mysql->options.bind_address =
          my_strdup(key_memory_mysql_options, static_cast<const char *>(arg),
                    MYF(MY_WME));
      break;
    case MYSQL_PLUGIN_DIR:
      EXTENSION_SET_STRING(&mysql->options, plugin_dir,
                           static_cast<const char *>(arg));
      break;
    case MYSQL_DEFAULT_AUTH:
      EXTENSION_SET_STRING(&mysql->options, default_auth,
                           static_cast<const char *>(arg));
      break;
    case MYSQL_OPT_SSL_KEY:
      if (mysql->options.ssl_key) my_free(mysql->options.ssl_key);
      mysql->options.ssl_key =
          set_ssl_option_unpack_path(static_cast<const char *>(arg));
      break;
    case MYSQL_OPT_SSL_CERT:
      if (mysql->options.ssl_cert) my_free(mysql->options.ssl_cert);
      mysql->options.ssl_cert =
          set_ssl_option_unpack_path(static_cast<const char *>(arg));
      break;
    case MYSQL_OPT_SSL_CA:
      if (mysql->options.ssl_ca) my_free(mysql->options.ssl_ca);
      mysql->options.ssl_ca =
          set_ssl_option_unpack_path(static_cast<const char *>(arg));
      break;
    case MYSQL_OPT_SSL_CAPATH:
      if (mysql->options.ssl_capath) my_free(mysql->options.ssl_capath);
      mysql->options.ssl_capath =
          set_ssl_option_unpack_path(static_cast<const char *>(arg));
      break;
    case MYSQL_OPT_SSL_CIPHER:
      SET_OPTION(ssl_cipher, static_cast<const char *>(arg));
      break;
    case MYSQL_OPT_TLS_CIPHERSUITES:
      EXTENSION_SET_STRING(&mysql->options, tls_ciphersuites,
                           static_cast<const char *>(arg));
      break;
    case MYSQL_OPT_SSL_CRL:
      if (mysql->options.extension)
        my_free(mysql->options.extension->ssl_crl);
      else
        ALLOCATE_EXTENSIONS(&mysql->options);
      mysql->options.extension->ssl_crl =
          set_ssl_option_unpack_path(static_cast<const char *>(arg));
      break;
    case MYSQL_OPT_SSL_CRLPATH:
      if (mysql->options.extension)
        my_free(mysql->options.extension->ssl_crlpath);
      else
        ALLOCATE_EXTENSIONS(&mysql->options);
      mysql->options.extension->ssl_crlpath =
          set_ssl_option_unpack_path(static_cast<const char *>(arg));
      break;
    case MYSQL_OPT_TLS_VERSION:
      EXTENSION_SET_STRING(&mysql->options, tls_version,
                           static_cast<const char *>(arg));
      if ((mysql->options.extension->ssl_ctx_flags = process_tls_version(
               mysql->options.extension->tls_version)) == -1)
        return 1;
      break;
    case MYSQL_OPT_SSL_FIPS_MODE: { /* This option is deprecated */
      fprintf(stderr,
              "WARNING: MYSQL_OPT_SSL_FIPS_MODE is deprecated and will be "
              "removed in a future version.\n");
      char ssl_err_string[OPENSSL_ERROR_LENGTH] = {'\0'};
      ENSURE_EXTENSIONS_PRESENT(&mysql->options);
      mysql->options.extension->ssl_fips_mode =
          *static_cast<const ulong *>(arg);
      if (set_fips_mode(mysql->options.extension->ssl_fips_mode,
                        ssl_err_string)) {
        DBUG_PRINT("error", ("fips mode set error %s:", ssl_err_string));
        set_mysql_extended_error(
            mysql, CR_SSL_FIPS_MODE_ERR, unknown_sqlstate,
            "Set Fips mode ON/STRICT failed, detail: '%s'.", ssl_err_string);
        return 1;
      }
    } break;
    case MYSQL_OPT_SSL_MODE:
      ENSURE_EXTENSIONS_PRESENT(&mysql->options);
      mysql->options.extension->ssl_mode = *static_cast<const uint *>(arg);
      if (mysql->options.extension->ssl_mode == SSL_MODE_VERIFY_IDENTITY)
        mysql->options.client_flag |= CLIENT_SSL_VERIFY_SERVER_CERT;
      else
        mysql->options.client_flag &= ~CLIENT_SSL_VERIFY_SERVER_CERT;
      break;
    case MYSQL_SERVER_PUBLIC_KEY:
      EXTENSION_SET_STRING(&mysql->options, server_public_key_path,
                           static_cast<const char *>(arg));
      break;

    case MYSQL_OPT_GET_SERVER_PUBLIC_KEY:
      ENSURE_EXTENSIONS_PRESENT(&mysql->options);
      mysql->options.extension->get_server_public_key =
          *static_cast<const bool *>(arg);
      break;

    case MYSQL_OPT_CONNECT_ATTR_RESET:
      ENSURE_EXTENSIONS_PRESENT(&mysql->options);
      if (mysql->options.extension->connection_attributes) {
        delete mysql->options.extension->connection_attributes;
        mysql->options.extension->connection_attributes = nullptr;
        mysql->options.extension->connection_attributes_length = 0;
      }
      break;
    case MYSQL_OPT_CONNECT_ATTR_DELETE:
      ENSURE_EXTENSIONS_PRESENT(&mysql->options);
      if (mysql->options.extension->connection_attributes) {
        string key = arg ? pointer_cast<const char *>(arg) : "";

        if (!key.empty()) {
          auto it =
              mysql->options.extension->connection_attributes->hash.find(key);
          if (it !=
              mysql->options.extension->connection_attributes->hash.end()) {
            const string &attr_key = it->first;
            const string &attr_value = it->second;
            mysql->options.extension->connection_attributes_length -=
                get_length_store_length(attr_key.size()) + attr_key.size() +
                get_length_store_length(attr_value.size()) + attr_value.size();

            mysql->options.extension->connection_attributes->hash.erase(it);
          }
        }
      }
      break;
    case MYSQL_ENABLE_CLEARTEXT_PLUGIN:
      ENSURE_EXTENSIONS_PRESENT(&mysql->options);
      mysql->options.extension->enable_cleartext_plugin =
          *static_cast<const bool *>(arg);
      break;
    case MYSQL_OPT_RETRY_COUNT:
      ENSURE_EXTENSIONS_PRESENT(&mysql->options);
      mysql->options.extension->retry_count = *static_cast<const uint *>(arg);
      break;
    case MYSQL_OPT_CAN_HANDLE_EXPIRED_PASSWORDS:
      if (*static_cast<const bool *>(arg))
        mysql->options.client_flag |= CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS;
      else
        mysql->options.client_flag &= ~CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS;
      break;

    case MYSQL_OPT_MAX_ALLOWED_PACKET:
      if (mysql)
        mysql->options.max_allowed_packet = *static_cast<const ulong *>(arg);
      else
        g_max_allowed_packet = *static_cast<const ulong *>(arg);
      break;

    case MYSQL_OPT_NET_BUFFER_LENGTH:
      g_net_buffer_length = *static_cast<const ulong *>(arg);
      break;

    case MYSQL_OPT_OPTIONAL_RESULTSET_METADATA:
      if (*static_cast<const bool *>(arg))
        mysql->options.client_flag |= CLIENT_OPTIONAL_RESULTSET_METADATA;
      else
        mysql->options.client_flag &= ~CLIENT_OPTIONAL_RESULTSET_METADATA;
      break;

    case MYSQL_OPT_COMPRESSION_ALGORITHMS: {
      std::string compress_option(static_cast<const char *>(arg));
      std::vector<std::string> list;
      parse_compression_algorithms_list(compress_option, list);
      ENSURE_EXTENSIONS_PRESENT(&mysql->options);
      mysql->options.extension->connection_compressed = true;
      mysql->options.client_flag &=
          ~(CLIENT_COMPRESS | CLIENT_ZSTD_COMPRESSION_ALGORITHM);
      mysql->options.compress = false;
      auto it = list.begin();
      unsigned int cnt = 0;
      while (it != list.end() && cnt < COMPRESSION_ALGORITHM_COUNT_MAX) {
        std::string value = *it;
        switch (get_compression_algorithm(value)) {
          case enum_compression_algorithm::MYSQL_ZLIB:
            mysql->options.client_flag |= CLIENT_COMPRESS;
            mysql->options.compress = true;
            break;
          case enum_compression_algorithm::MYSQL_ZSTD:
            mysql->options.client_flag |= CLIENT_ZSTD_COMPRESSION_ALGORITHM;
            mysql->options.compress = true;
            break;
          case enum_compression_algorithm::MYSQL_UNCOMPRESSED:
            mysql->options.extension->connection_compressed = false;
            break;
          case enum_compression_algorithm::MYSQL_INVALID:
            break;  // report error
        }
        it++;
        cnt++;
      }
      if (cnt)
        EXTENSION_SET_STRING(&mysql->options, compression_algorithm,
                             static_cast<const char *>(arg));
      mysql->options.extension->total_configured_compression_algorithms = cnt;
    } break;
    case MYSQL_OPT_ZSTD_COMPRESSION_LEVEL:
      ENSURE_EXTENSIONS_PRESENT(&mysql->options);
      mysql->options.extension->zstd_compression_level =
          *static_cast<const unsigned int *>(arg);
      break;

    case MYSQL_OPT_LOAD_DATA_LOCAL_DIR:
      if (set_load_data_local_infile_option(mysql,
                                            static_cast<const char *>(arg)))
        return 1;
      break;
    case MYSQL_OPT_SSL_SESSION_DATA:
      EXTENSION_SET_STRING(&mysql->options, ssl_session_data,
                           static_cast<const char *>(arg));
      break;
    case MYSQL_OPT_TLS_SNI_SERVERNAME:
      EXTENSION_SET_STRING(&mysql->options, tls_sni_servername,
                           static_cast<const char *>(arg));
      break;
    default:
      return 1;
  }
  return 0;
}

/**
  Return the current values for the options settable through mysql_options()

  Returns the current values for all of the connection options.
  Callers should not manipulate the returned data !
  Data are valid at the time of returning them until the next C API CALL
  arg should always be a pointer to a variable of the appropriate type.
  type of variable, based on the parameter:

  uint
    MYSQL_OPT_CONNECT_TIMEOUT, MYSQL_OPT_READ_TIMEOUT, MYSQL_OPT_WRITE_TIMEOUT,
    MYSQL_OPT_PROTOCOL, MYSQL_OPT_SSL_MODE, MYSQL_OPT_RETRY_COUNT

  bool
    MYSQL_OPT_COMPRESS, MYSQL_OPT_LOCAL_INFILE,
    MYSQL_REPORT_DATA_TRUNCATION, MYSQL_OPT_RECONNECT,
    MYSQL_ENABLE_CLEARTEXT_PLUGIN, MYSQL_OPT_CAN_HANDLE_EXPIRED_PASSWORDS,
    MYSQL_OPT_OPTIONAL_RESULTSET_METADATA

  const char *
    MYSQL_READ_DEFAULT_FILE, MYSQL_READ_DEFAULT_GROUP,
    MYSQL_SET_CHARSET_DIR, MYSQL_SET_CHARSET_NAME,
    MYSQL_SHARED_MEMORY_BASE_NAME, MYSQL_SET_CLIENT_IP, MYSQL_OPT_BIND,
    MYSQL_PLUGIN_DIR, MYSQL_DEFAULT_AUTH, MYSQL_OPT_SSL_KEY, MYSQL_OPT_SSL_CERT,
    MYSQL_OPT_SSL_CA, MYSQL_OPT_SSL_CAPATH, MYSQL_OPT_SSL_CIPHER,
    MYSQL_OPT_TLS_CIPHERSUITES, MYSQL_OPT_SSL_CRL, MYSQL_OPT_SSL_CRLPATH,
    MYSQL_OPT_TLS_VERSION, MYSQL_SERVER_PUBLIC_KEY, MYSQL_OPT_SSL_FIPS_MODE,
    MYSQL_OPT_TLS_SNI_SERVERNAME

  <none, error returned>
    MYSQL_OPT_NAMED_PIPE, MYSQL_OPT_CONNECT_ATTR_RESET,
    MYSQL_OPT_CONNECT_ATTR_DELETE, MYSQL_INIT_COMMAND

  @param      mysql       The MYSQL connection to operate on
  @param      option      The option to return the value for
  @param  [out] arg         Must be non-null. Receives the current value.
  @return status
  @retval 0 SUCCESS
*/

int STDCALL mysql_get_option(MYSQL *mysql, enum mysql_option option,
                             const void *arg) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("option: %d", (int)option));

  if (!arg) return 1;

  switch (option) {
    case MYSQL_OPT_CONNECT_TIMEOUT:
      *(const_cast<uint *>(static_cast<const uint *>(arg))) =
          mysql->options.connect_timeout;
      break;
    case MYSQL_OPT_READ_TIMEOUT:
      *(const_cast<uint *>(static_cast<const uint *>(arg))) =
          mysql->options.read_timeout;
      break;
    case MYSQL_OPT_WRITE_TIMEOUT:
      *(const_cast<uint *>(static_cast<const uint *>(arg))) =
          mysql->options.write_timeout;
      break;
    case MYSQL_OPT_COMPRESS:
      *(const_cast<bool *>(static_cast<const bool *>(arg))) =
          mysql->options.compress;
      break;
    case MYSQL_OPT_LOCAL_INFILE: /* Allow LOAD DATA LOCAL ?*/
      *(const_cast<uint *>(static_cast<const uint *>(arg))) =
          (mysql->options.client_flag & CLIENT_LOCAL_FILES) != 0;
      break;
    case MYSQL_READ_DEFAULT_FILE:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.my_cnf_file;
      break;
    case MYSQL_READ_DEFAULT_GROUP:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.my_cnf_group;
      break;
    case MYSQL_SET_CHARSET_DIR:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.charset_dir;
      break;
    case MYSQL_SET_CHARSET_NAME:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.charset_name;
      break;
    case MYSQL_OPT_PROTOCOL:
      *(const_cast<uint *>(static_cast<const uint *>(arg))) =
          mysql->options.protocol;
      break;
    case MYSQL_SHARED_MEMORY_BASE_NAME:
#if defined(_WIN32)
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.shared_memory_base_name;
#else
      *(static_cast<char **>(const_cast<void *>(arg))) = const_cast<char *>("");
#endif
      break;
    case MYSQL_REPORT_DATA_TRUNCATION:
      *(const_cast<bool *>(static_cast<const bool *>(arg))) =
          mysql->options.report_data_truncation;
      break;
    case MYSQL_OPT_RECONNECT:
      fprintf(stderr,
              "WARNING: MYSQL_OPT_RECONNECT is deprecated and will be "
              "removed in a future version.\n");
      *(const_cast<bool *>(static_cast<const bool *>(arg))) = mysql->reconnect;
      break;
    case MYSQL_OPT_BIND:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.bind_address;
      break;
    case MYSQL_OPT_SSL_MODE:
      *(const_cast<uint *>(static_cast<const uint *>(arg))) =
          mysql->options.extension ? mysql->options.extension->ssl_mode : 0;
      break;
    case MYSQL_OPT_SSL_FIPS_MODE: /* This option is deprecated */
      *(const_cast<uint *>(static_cast<const uint *>(arg))) =
          mysql->options.extension ? mysql->options.extension->ssl_fips_mode
                                   : 0;
      break;
    case MYSQL_PLUGIN_DIR:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.extension ? mysql->options.extension->plugin_dir
                                   : nullptr;
      break;
    case MYSQL_DEFAULT_AUTH:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.extension ? mysql->options.extension->default_auth
                                   : nullptr;
      break;
    case MYSQL_OPT_SSL_KEY:
      *(static_cast<char **>(const_cast<void *>(arg))) = mysql->options.ssl_key;
      break;
    case MYSQL_OPT_SSL_CERT:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.ssl_cert;
      break;
    case MYSQL_OPT_SSL_CA:
      *(static_cast<char **>(const_cast<void *>(arg))) = mysql->options.ssl_ca;
      break;
    case MYSQL_OPT_SSL_CAPATH:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.ssl_capath;
      break;
    case MYSQL_OPT_SSL_CIPHER:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.ssl_cipher;
      break;
    case MYSQL_OPT_TLS_CIPHERSUITES:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.extension ? mysql->options.extension->tls_ciphersuites
                                   : nullptr;
      break;
    case MYSQL_OPT_TLS_SNI_SERVERNAME:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          MYSQL_OPTIONS_EXTENSION_PTR(mysql, tls_sni_servername);
      break;
    case MYSQL_OPT_RETRY_COUNT:
      *(const_cast<uint *>(static_cast<const uint *>(arg))) =
          mysql->options.extension ? mysql->options.extension->retry_count : 1;
      break;
    case MYSQL_OPT_TLS_VERSION:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.extension ? mysql->options.extension->tls_version
                                   : nullptr;
      break;
    case MYSQL_OPT_SSL_CRL:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.extension ? mysql->options.extension->ssl_crl
                                   : nullptr;
      break;
    case MYSQL_OPT_SSL_CRLPATH:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.extension ? mysql->options.extension->ssl_crlpath
                                   : nullptr;
      break;
    case MYSQL_SERVER_PUBLIC_KEY:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.extension
              ? mysql->options.extension->server_public_key_path
              : nullptr;
      break;
    case MYSQL_OPT_GET_SERVER_PUBLIC_KEY:
      *(const_cast<bool *>(static_cast<const bool *>(arg))) =
          mysql->options.extension &&
          mysql->options.extension->get_server_public_key;
      break;
    case MYSQL_ENABLE_CLEARTEXT_PLUGIN:
      *(const_cast<bool *>(static_cast<const bool *>(arg))) =
          mysql->options.extension &&
          mysql->options.extension->enable_cleartext_plugin;
      break;
    case MYSQL_OPT_CAN_HANDLE_EXPIRED_PASSWORDS:
      *(const_cast<bool *>(static_cast<const bool *>(arg))) =
          (mysql->options.client_flag & CLIENT_CAN_HANDLE_EXPIRED_PASSWORDS) !=
          0;
      break;

    case MYSQL_OPT_MAX_ALLOWED_PACKET:
      if (mysql)
        *(const_cast<ulong *>(static_cast<const ulong *>(arg))) =
            mysql->options.max_allowed_packet;
      else
        *(const_cast<ulong *>(static_cast<const ulong *>(arg))) =
            g_max_allowed_packet;
      break;

    case MYSQL_OPT_NET_BUFFER_LENGTH:
      *(const_cast<ulong *>(static_cast<const ulong *>(arg))) =
          g_net_buffer_length;
      break;

    case MYSQL_OPT_OPTIONAL_RESULTSET_METADATA:
      *(const_cast<bool *>(static_cast<const bool *>(arg))) =
          (mysql->options.client_flag & CLIENT_OPTIONAL_RESULTSET_METADATA) !=
          0;
      break;
    case MYSQL_OPT_LOAD_DATA_LOCAL_DIR:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.extension ? mysql->options.extension->load_data_dir
                                   : nullptr;
      break;
    case MYSQL_OPT_SSL_SESSION_DATA:
      *(static_cast<char **>(const_cast<void *>(arg))) =
          mysql->options.extension
              ? static_cast<char *>(mysql->options.extension->ssl_session_data)
              : nullptr;
      break;

    case MYSQL_OPT_NAMED_PIPE:          /* This option is deprecated */
    case MYSQL_INIT_COMMAND:            /* Cumulative */
    case MYSQL_OPT_CONNECT_ATTR_RESET:  /* Cumulative */
    case MYSQL_OPT_CONNECT_ATTR_DELETE: /* Cumulative */
    default:
      return 1;
  }
  return 0;
}

int STDCALL mysql_options4(MYSQL *mysql, enum mysql_option option,
                           const void *arg1, const void *arg2) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("option: %d", (int)option));

  switch (option) {
    case MYSQL_OPT_CONNECT_ATTR_ADD: {
      const char *key = static_cast<const char *>(arg1);
      const char *value = static_cast<const char *>(arg2);
      const size_t key_len = arg1 ? strlen(key) : 0;
      const size_t value_len = arg2 ? strlen(value) : 0;
      size_t attr_storage_length = key_len + value_len;

      /* we can't have a zero length key */
      if (!key_len) {
        set_mysql_error(mysql, CR_INVALID_PARAMETER_NO, unknown_sqlstate);
        return 1;
      }

      /* calculate the total storage length of the attribute */
      attr_storage_length += get_length_store_length(key_len);
      attr_storage_length += get_length_store_length(value_len);

      ENSURE_EXTENSIONS_PRESENT(&mysql->options);

      /*
        Throw and error if the maximum combined length of the attribute value
        will be greater than the maximum that we can safely transmit.
      */
      if (attr_storage_length +
              mysql->options.extension->connection_attributes_length >
          MAX_CONNECTION_ATTR_STORAGE_LENGTH) {
        set_mysql_error(mysql, CR_INVALID_PARAMETER_NO, unknown_sqlstate);
        return 1;
      }

      if (!mysql->options.extension->connection_attributes) {
        mysql->options.extension->connection_attributes =
            new (std::nothrow) My_hash();
        if (!mysql->options.extension->connection_attributes) {
          set_mysql_error(mysql, CR_OUT_OF_MEMORY, unknown_sqlstate);
          return 1;
        }
      }
      if (!mysql->options.extension->connection_attributes->hash
               .emplace(key, value)
               .second) {
        /* can't insert the value */
        set_mysql_error(mysql, CR_DUPLICATE_CONNECTION_ATTR, unknown_sqlstate);
        return 1;
      }

      mysql->options.extension->connection_attributes_length +=
          attr_storage_length;

      break;
    }
    case MYSQL_OPT_USER_PASSWORD: {
      const unsigned int factor = *static_cast<const uint *>(arg1) - 1;
      ENSURE_EXTENSIONS_PRESENT(&mysql->options);
      switch (factor) {
        case 0:
          my_free(mysql->options.extension->client_auth_info[0].password);
          mysql->options.extension->client_auth_info[0].password =
              my_strdup(key_memory_mysql_options,
                        static_cast<const char *>(arg2), MYF(MY_FAE));
          break;
        case 1:
          my_free(mysql->options.extension->client_auth_info[1].password);
          mysql->options.extension->client_auth_info[1].password =
              my_strdup(key_memory_mysql_options,
                        static_cast<const char *>(arg2), MYF(MY_FAE));
          break;
        case 2:
          my_free(mysql->options.extension->client_auth_info[2].password);
          mysql->options.extension->client_auth_info[2].password =
              my_strdup(key_memory_mysql_options,
                        static_cast<const char *>(arg2), MYF(MY_FAE));
          break;
        default:
          set_mysql_error(mysql, CR_INVALID_FACTOR_NO, unknown_sqlstate);
          return 1;
      }
      break;
    }
    default:
      return 1;
  }
  return 0;
}

/****************************************************************************
  Functions to get information from the MySQL structure
  These are functions to make shared libraries more usable.
****************************************************************************/

/* MYSQL_RES */
my_ulonglong STDCALL mysql_num_rows(MYSQL_RES *res) { return res->row_count; }

unsigned int STDCALL mysql_num_fields(MYSQL_RES *res) {
  return res->field_count;
}

uint STDCALL mysql_errno(MYSQL *mysql) {
  return mysql ? mysql->net.last_errno : mysql_server_last_errno;
}

const char *STDCALL mysql_error(MYSQL *mysql) {
  return mysql ? mysql->net.last_error : mysql_server_last_error;
}

/**
  Read data and its length from a LIST node.

  Assumes LIST which stores data blobs in LEX_STRING structures,
  where LEX_STRING::str is pointer to the data and LEX_STRING::length
  is the length of this data.

  If node is NULL then data and length are set to NULL and 0, respectively,
  and function returns 0, otherwise, if data has been read from the node,
  function returns 1.
*/

static int get_data_and_length(LIST *node, const char **data, size_t *length) {
  assert(!node || node->data);
  if (data) *data = node ? ((LEX_STRING *)(node->data))->str : nullptr;
  if (length) *length = node ? ((LEX_STRING *)(node->data))->length : 0;

  return node ? 0 : 1;
}

/**
  Get the first state change information received from the server.

  @param [in] mysql          mysql handle
  @param [in] type           state change type
  @param [out] data          buffer to store the data
  @param [out] length        length of the data

  @return
    0 - Valid data stored
    1 - No data
*/

int STDCALL mysql_session_track_get_first(MYSQL *mysql,
                                          enum enum_session_state_type type,
                                          const char **data, size_t *length) {
  STATE_INFO *info = STATE_DATA(mysql);

  if (!info || !IS_SESSION_STATE_TYPE(type) ||
      !(info->info_list[type].head_node))
    return get_data_and_length(nullptr, data, length);

  info->info_list[type].current_node = info->info_list[type].head_node;

  return mysql_session_track_get_next(mysql, type, data, length);
}

/**
  Get the subsequent state change information received from the server.

  @param [in] mysql          mysql handle
  @param [in] type           state change type
  @param [out] data          buffer to store the data
  @param [out] length        length of the data

  @return
    0 - Valid data stored
    1 - No data
*/

int STDCALL mysql_session_track_get_next(MYSQL *mysql,
                                         enum enum_session_state_type type,
                                         const char **data, size_t *length) {
  STATE_INFO *info = STATE_DATA(mysql);
  int ret;

  if (!info || !IS_SESSION_STATE_TYPE(type) ||
      !(info->info_list[type].current_node))
    return get_data_and_length(nullptr, data, length);

  ret = get_data_and_length(info->info_list[type].current_node, data, length);

  info->info_list[type].current_node =
      list_rest(info->info_list[type].current_node);

  return ret;
}

/*
  Get version number for server in a form easy to test on

  SYNOPSIS
    mysql_get_server_version()
    mysql   Connection

  EXAMPLE
    4.1.0-alfa ->  40100

  NOTES
    We will ensure that a newer server always has a bigger number.

  RETURN
   Signed number > 323000
   Zero if there is no connection
*/

ulong STDCALL mysql_get_server_version(MYSQL *mysql) {
  ulong major = 0, minor = 0, version = 0;

  if (mysql->server_version) {
    char *pos = mysql->server_version, *end_pos;
    major = strtoul(pos, &end_pos, 10);
    pos = end_pos + 1;
    minor = strtoul(pos, &end_pos, 10);
    pos = end_pos + 1;
    version = strtoul(pos, &end_pos, 10);
  } else {
    set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
  }

  return major * 10000 + minor * 100 + version;
}

/*
   mysql_set_character_set function sends SET NAMES cs_name to
   the server (which changes character_set_client, character_set_result
   and character_set_connection) and updates mysql->charset so other
   functions like mysql_real_escape will work correctly.
*/
int STDCALL mysql_set_character_set(MYSQL *mysql, const char *cs_name) {
  CHARSET_INFO *cs;
  const char *save_csdir = charsets_dir;

  if (mysql->options.charset_dir) {
#ifdef MYSQL_SERVER
    // Do not change charsets_dir, it is not thread safe.
    assert(false);
#else
    charsets_dir = mysql->options.charset_dir;
#endif
  }
  if (!mysql->net.vio) {
    /* Initialize with automatic OS character set detection. */
    mysql_options(mysql, MYSQL_SET_CHARSET_NAME, cs_name);
    mysql_init_character_set(mysql);
    /*
      In case of automatic OS character set detection
      mysql_init_character_set changes mysql->options.charset_name
      from "auto" to the real character set name.
      Reset cs_name to the detected character set name, accordingly.
    */
    cs_name = mysql->options.charset_name;
  }

#ifndef MYSQL_SERVER
  if (mysql->charset != nullptr) {
    if (!is_supported_parser_charset(mysql->charset)) {
      set_mysql_extended_error(mysql, CR_INVALID_CLIENT_CHARSET,
                               unknown_sqlstate,
                               ER_CLIENT(CR_INVALID_CLIENT_CHARSET), cs_name);
      return 1;
    }
  }
#endif

  if (strlen(cs_name) < MY_CS_NAME_SIZE &&
      (cs = get_charset_by_csname(cs_name, MY_CS_PRIMARY, MYF(0)))) {
    char buff[MY_CS_NAME_SIZE + 10];
    charsets_dir = save_csdir;
    if (!mysql->net.vio) {
      /* If there is no connection yet we don't send "SET NAMES" query */
      mysql->charset = cs;
      return 0;
    }
    /* Skip execution of "SET NAMES" for pre-4.1 servers */
    if (mysql_get_server_version(mysql) < 40100) return 0;
    sprintf(buff, "SET NAMES %s", cs_name);
    if (!mysql_real_query(mysql, buff, (ulong)strlen(buff))) {
      mysql->charset = cs;
    }
  } else {
    char cs_dir_name[FN_REFLEN];
    get_charsets_dir(cs_dir_name);
    set_mysql_extended_error(mysql, CR_CANT_READ_CHARSET, unknown_sqlstate,
                             ER_CLIENT(CR_CANT_READ_CHARSET), cs_name,
                             cs_dir_name);
  }
  charsets_dir = save_csdir;
  return mysql->net.last_errno;
}

const char *STDCALL mysql_info(MYSQL *mysql) {
  if (!mysql) {
#if defined(CLIENT_PROTOCOL_TRACING)
    return "protocol tracing enabled";
#else
    return nullptr;
#endif
  }
  return mysql->info;
}

static void reset_connection(MYSQL *mysql) {
  mysql_detach_stmt_list(&mysql->stmts, "mysql_reset_connection");
  /* reset some of the members in mysql */
  mysql->insert_id = 0;
  mysql->affected_rows = ~(uint64_t)0;
  free_old_query(mysql);
  mysql->status = MYSQL_STATUS_READY;
  mysql_extension_bind_free(MYSQL_EXTENSION_PTR(mysql));
}

int STDCALL mysql_reset_connection(MYSQL *mysql) {
  DBUG_TRACE;
  if (simple_command(mysql, COM_RESET_CONNECTION, nullptr, 0, 0))
    return 1;
  else {
    reset_connection(mysql);
    return 0;
  }
}

net_async_status STDCALL mysql_reset_connection_nonblocking(MYSQL *mysql) {
  DBUG_TRACE;
  bool error;
  net_async_status status = simple_command_nonblocking(
      mysql, COM_RESET_CONNECTION, nullptr, 0, 0, &error);
  if (status == NET_ASYNC_COMPLETE) {
    if (error) {
      return NET_ASYNC_ERROR;
    } else {
      reset_connection(mysql);
    }
  }
  return status;
}

/********************************************************************
 Transactional APIs
*********************************************************************/

/*
  Commit the current transaction
*/

bool STDCALL mysql_commit(MYSQL *mysql) {
  DBUG_TRACE;
  return (bool)mysql_real_query(mysql, "commit", 6);
}

/*
  Rollback the current transaction
*/

bool STDCALL mysql_rollback(MYSQL *mysql) {
  DBUG_TRACE;
  return (bool)mysql_real_query(mysql, "rollback", 8);
}

/*
  Set autocommit to either true or false
*/

bool STDCALL mysql_autocommit(MYSQL *mysql, bool auto_mode) {
  DBUG_TRACE;
  DBUG_PRINT("enter", ("mode : %d", auto_mode));

  return (bool)mysql_real_query(
      mysql, auto_mode ? "set autocommit=1" : "set autocommit=0", 16);
}

/********************************************************************
 Multi query execution + SPs APIs
*********************************************************************/

/*
  Returns true/false to indicate whether any more query results exist
  to be read using mysql_next_result()
*/

bool STDCALL mysql_more_results(MYSQL *mysql) {
  bool res;
  DBUG_TRACE;

  res = ((mysql->server_status & SERVER_MORE_RESULTS_EXISTS) ? 1 : 0);
  DBUG_PRINT("exit", ("More results exists ? %d", res));
  return res;
}

/*
  Reads and returns the next query results
*/
int STDCALL mysql_next_result(MYSQL *mysql) {
  DBUG_TRACE;

  MYSQL_TRACE_STAGE(mysql, WAIT_FOR_RESULT);

  if (mysql->status != MYSQL_STATUS_READY) {
    set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    return 1;
  }

  net_clear_error(&mysql->net);
  mysql->affected_rows = ~(uint64_t)0;

  if (mysql->server_status & SERVER_MORE_RESULTS_EXISTS)
    return (*mysql->methods->read_query_result)(mysql);
  else {
    MYSQL_TRACE_STAGE(mysql, READY_FOR_COMMAND);
  }

  return -1; /* No more results */
}

MYSQL_RES *STDCALL mysql_use_result(MYSQL *mysql) {
  return (*mysql->methods->use_result)(mysql);
}

uint64_t STDCALL mysql_affected_rows(MYSQL *mysql) {
  return mysql->affected_rows;
}

enum_resultset_metadata STDCALL mysql_result_metadata(MYSQL_RES *result) {
  return result->metadata;
}

/**************************************************************************
  Return next field of the query results
**************************************************************************/

MYSQL_FIELD *STDCALL mysql_fetch_field(MYSQL_RES *result) {
  if (result->current_field >= result->field_count || !result->fields)
    return (nullptr);
  return &result->fields[result->current_field++];
}

MYSQL_FIELD *STDCALL mysql_fetch_fields(MYSQL_RES *res) {
  return (res)->fields;
}

unsigned int STDCALL mysql_field_count(MYSQL *mysql) {
  return mysql->field_count;
}

const char *STDCALL mysql_sqlstate(MYSQL *mysql) {
  return mysql ? mysql->net.sqlstate : cant_connect_sqlstate;
}

/* clang-format off */
/**
  @page page_protocol_connection_phase_authentication_methods_clear_text_password Clear text client plugin

  <ul>
  <li>
  This client side plugin is used by a number of server plugins:
  LDAP (*authentication_ldap_simple*) and PAM (*authentication_pam*) to name a few.
  </li>
  <li>
  The client name is *mysql_clear_password*
  </li>
  <li>
  Client side requires nothing from the server. But the server generates
  and sends a 20-byte mysql_native_password compatible scramble.
  </li>
  <li>
  Client side sends the password in clear text to the server
  </li>
  </ul>

  @startuml
  Server->Client: 20 bytes of scramble to be ignored
  Client->Server: The clear text password. null terminated.
  @enduml

  @note
  Sending the scramble is not necessary for the clear text
  method, but, since the server always initiates the exchange by
  sending @ref page_protocol_connection_phase_packets_protocol_handshake
  and that one has a placeholder for authentication plugin dependent data the
  server does fill that space with a scramble should it come to pass that
  it will back down to mysql_native_password.
  This is also why it's OK no to specifically read this in
  @ref clear_password_auth_client since it's already read as a part of
  the initial exchange.


  @sa ::clear_password_auth_client, ::server_mpvio_write_packet,
    ::send_server_handshake_packet
*/
/* clang-format on */

/**
  The main function of the mysql_clear_password authentication plugin.
*/

static int clear_password_auth_client(MYSQL_PLUGIN_VIO *vio, MYSQL *mysql) {
  int res;

  /* send password in clear text */
  res = vio->write_packet(vio, (const unsigned char *)mysql->passwd,
                          (int)strlen(mysql->passwd) + 1);

  return res ? CR_ERROR : CR_OK;
}

const char *fieldtype2str(enum enum_field_types type) {
  switch (type) {
    case MYSQL_TYPE_BIT:
      return "BIT";
    case MYSQL_TYPE_BLOB:
      return "BLOB";
    case MYSQL_TYPE_VECTOR:
      return "VECTOR";
    case MYSQL_TYPE_BOOL:
      return "BOOLEAN";
    case MYSQL_TYPE_DATE:
      return "DATE";
    case MYSQL_TYPE_DATETIME:
      return "DATETIME";
    case MYSQL_TYPE_NEWDECIMAL:
      return "NEWDECIMAL";
    case MYSQL_TYPE_DECIMAL:
      return "DECIMAL";
    case MYSQL_TYPE_DOUBLE:
      return "DOUBLE";
    case MYSQL_TYPE_ENUM:
      return "ENUM";
    case MYSQL_TYPE_FLOAT:
      return "FLOAT";
    case MYSQL_TYPE_GEOMETRY:
      return "GEOMETRY";
    case MYSQL_TYPE_INT24:
      return "INT24";
    case MYSQL_TYPE_JSON:
      return "JSON";
    case MYSQL_TYPE_LONG:
      return "LONG";
    case MYSQL_TYPE_LONGLONG:
      return "LONGLONG";
    case MYSQL_TYPE_LONG_BLOB:
      return "LONG_BLOB";
    case MYSQL_TYPE_MEDIUM_BLOB:
      return "MEDIUM_BLOB";
    case MYSQL_TYPE_NEWDATE:
      return "NEWDATE";
    case MYSQL_TYPE_NULL:
      return "NULL";
    case MYSQL_TYPE_SET:
      return "SET";
    case MYSQL_TYPE_SHORT:
      return "SHORT";
    case MYSQL_TYPE_STRING:
      return "STRING";
    case MYSQL_TYPE_TIME:
      return "TIME";
    case MYSQL_TYPE_TIMESTAMP:
      return "TIMESTAMP";
    case MYSQL_TYPE_TINY:
      return "TINY";
    case MYSQL_TYPE_TINY_BLOB:
      return "TINY_BLOB";
    case MYSQL_TYPE_VARCHAR:
      return "VARCHAR";
    case MYSQL_TYPE_VAR_STRING:
      return "VAR_STRING";
    case MYSQL_TYPE_YEAR:
      return "YEAR";
    case MYSQL_TYPE_INVALID:
      return "?-invalid-?";
    default:
      return "?-unknown-?";
  }
}
