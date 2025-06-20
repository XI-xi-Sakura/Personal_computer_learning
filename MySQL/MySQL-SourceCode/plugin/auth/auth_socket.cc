/*  Copyright (c) 2010, 2025, Oracle and/or its affiliates.

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

/**
  @file

  auth_socket authentication plugin.

  Authentication is successful if the connection is done via a unix socket and
  the owner of the client process matches the user name that was used when
  connecting to mysqld.
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* for struct ucred */
#endif

#include <mysql/plugin_auth.h>
#include <pwd.h>
#include <sys/socket.h>
#include <cstring>

#include "my_compiler.h"

static int socket_auth(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info) {
  unsigned char *pkt;
  MYSQL_PLUGIN_VIO_INFO vio_info;
  struct ucred cred;
  socklen_t cred_len = sizeof(cred);
  struct passwd pwd_buf, *pwd;
  char buf[1024];

  /* no user name yet ? read the client handshake packet with the user name */
  if (info->user_name == nullptr) {
    if (vio->read_packet(vio, &pkt) < 0) return CR_ERROR;
  }

  info->password_used = PASSWORD_USED_NO_MENTION;

  vio->info(vio, &vio_info);
  if (vio_info.protocol != MYSQL_PLUGIN_VIO_INFO::MYSQL_VIO_SOCKET)
    return CR_ERROR;

  /* get the UID of the client process */
  if (getsockopt(vio_info.socket, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len))
    return CR_ERROR;

  if (cred_len != sizeof(cred)) return CR_ERROR;

  /* and find the socket user name for this uid */
  getpwuid_r(cred.uid, &pwd_buf, buf, sizeof(buf), &pwd);
  if (pwd == nullptr) return CR_ERROR;

  /* fill in the external user name used */
  strncpy(info->external_user, pwd->pw_name, sizeof(info->external_user) - 1);
  info->external_user[sizeof(info->external_user) - 1] = 0;

  if (!strcmp(pwd->pw_name, info->user_name) ||
      !strcmp(pwd->pw_name, info->auth_string))
    return CR_OK;
  return CR_ERROR;
}

static int generate_auth_string_hash(char *outbuf [[maybe_unused]],
                                     unsigned int *buflen,
                                     const char *inbuf [[maybe_unused]],
                                     unsigned int inbuflen [[maybe_unused]]) {
  *buflen = 0;
  return 0;
}

static int validate_auth_string_hash(char *const inbuf [[maybe_unused]],
                                     unsigned int buflen [[maybe_unused]]) {
  return 0;
}

static int set_salt(const char *password [[maybe_unused]],
                    unsigned int password_len [[maybe_unused]],
                    unsigned char *salt [[maybe_unused]],
                    unsigned char *salt_len) {
  *salt_len = 0;
  return 0;
}

static struct st_mysql_auth socket_auth_handler = {
    MYSQL_AUTHENTICATION_INTERFACE_VERSION,
    nullptr,
    socket_auth,
    generate_auth_string_hash,
    validate_auth_string_hash,
    set_salt,
    AUTH_FLAG_PRIVILEGED_USER_FOR_PASSWORD_CHANGE,
    nullptr};

mysql_declare_plugin(socket_auth){
    MYSQL_AUTHENTICATION_PLUGIN,
    &socket_auth_handler,
    "auth_socket",
    PLUGIN_AUTHOR_ORACLE,
    "Unix Socket based authentication",
    PLUGIN_LICENSE_GPL,
    nullptr, /* Init */
    nullptr, /* Check uninstall */
    nullptr, /* Deinit */
    0x0101,
    nullptr,
    nullptr,
    nullptr,
    0,
} mysql_declare_plugin_end;
