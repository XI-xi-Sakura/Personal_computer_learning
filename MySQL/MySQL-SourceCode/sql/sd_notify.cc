/* Copyright (c) 2018, 2025, Oracle and/or its affiliates.

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

#include "sql/sd_notify.h"
#include "my_config.h"

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>  // connect
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>  // socket
#endif

#ifdef HAVE_SYS_UN_H
#include <sys/un.h>  // AF_UNIX
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>  // write
#endif

#include <cerrno>   // errno
#include <cstring>  // strcpy

#include <mysqld_error.h>                            // error logging
#include "my_sys.h"                                  // my_strerror
#include "mysql/components/services/log_builtins.h"  // error logging
#include "scope_guard.h"                             // scope_guard
#include "sql/log.h"                                 // error logging

namespace sysd {
/** File descriptor for the systemd notification socket file. */
int NotifyGlobals::socket = -1;

/** Stringstream for formatting notification messages. */
std::stringstream NotifyGlobals::fmt;

/**
  Looks for the name of the socket file in the environment variable
  NOTIFY_SOCKET. Connects NotifyGlobals::socket to it if present.
*/
void notify_connect() {
#ifndef _WIN32
  const char *sockstr = getenv("NOTIFY_SOCKET");
  if (sockstr == nullptr) {
#ifdef WITH_SYSTEMD_DEBUG
    LogErr(WARNING_LEVEL, ER_SYSTEMD_NOTIFY_DEBUG,
           "NOTIFY_SOCKET not set in environment. sd_notify messages "
           "will not be sent!",
           "");
#endif /* WITH_SYSTEMD_DEBUG */
    return;
  }
  size_t const sockstrlen = strlen(sockstr);
  size_t const sunpathlen = sizeof(sockaddr_un::sun_path) - 1;
  if (sockstrlen > sunpathlen) {
    LogErr(SYSTEM_LEVEL, ER_SYSTEMD_NOTIFY_PATH_TOO_LONG, sockstr, sockstrlen,
           sunpathlen);
    return;
  }
  NotifyGlobals::socket = socket(AF_UNIX, SOCK_DGRAM, 0);

  sockaddr_un addr;
  socklen_t addrlen;
  memset(&addr, 0, sizeof(sockaddr_un));
  addr.sun_family = AF_UNIX;
  if (sockstr[0] != '@') {
    strcpy(addr.sun_path, sockstr);
    addrlen = offsetof(struct sockaddr_un, sun_path) + sockstrlen + 1;
  } else {  // Abstract namespace socket
    addr.sun_path[0] = '\0';
    strncpy(&addr.sun_path[1], sockstr + 1, strlen(sockstr) - 1);
    addrlen = offsetof(struct sockaddr_un, sun_path) + sockstrlen;
  }
  int ret = -1;
  do {
    ret = connect(NotifyGlobals::socket,
                  reinterpret_cast<const sockaddr *>(&addr), addrlen);
  } while (ret == -1 && errno == EINTR);
  if (ret == -1) {
    char errbuf[512];
    LogErr(WARNING_LEVEL, ER_SYSTEMD_NOTIFY_CONNECT_FAILED, sockstr,
           my_strerror(errbuf, sizeof(errbuf) - 1, errno));
    NotifyGlobals::socket = -1;
  }
#endif /* not defined _WIN32 */
}

#ifndef _WIN32
/**
  Recursion terminator overload for varargs template function. Creates
  a string from the current content of NotifyGlobals::fmt and sends
  string to notification socket.
 */
void notify() {
  std::string const note = NotifyGlobals::fmt.str();
  const char *src = note.c_str();
  const char *end = src + note.size();
  ssize_t status = -1;

  auto sg = create_scope_guard([&]() {
    NotifyGlobals::fmt.str("");  // clear the fmt buffer for new notification
  });

#ifdef WITH_SYSTEMD_DEBUG
  if (NotifyGlobals::socket == -1) {
    LogErr(WARNING_LEVEL, ER_SYSTEMD_NOTIFY_DEBUG,
           "Invalid systemd notify socket, cannot send: ", note.c_str());
    return;
  }
#endif /* WITH_SYSTEMD_DEBUG */

  // when systemd is enabled, irrespective of whether it's compiled with or
  // without WITH_SYSTEMD_DEBUG, we want to log every systemd message to
  // error-logs.
  LogErr(INFORMATION_LEVEL, ER_SYSTEMD_NOTIFY_DEBUG,
         "systemd notify: ", note.c_str());

  while (true) {
    size_t const remaining = end - src;
    status = write(NotifyGlobals::socket, src, remaining);
    if (status == -1) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    size_t const written = status;
    if (written == remaining) {
      break;
    }
    src += written;
  }
  if (status == -1) {
    char errbuf[512];
    LogErr(WARNING_LEVEL, ER_SYSTEMD_NOTIFY_WRITE_FAILED, note.c_str(),
           my_strerror(errbuf, sizeof(errbuf) - 1, errno));
  }
}
#endif /* not defined _WIN32 */
}  // namespace sysd
