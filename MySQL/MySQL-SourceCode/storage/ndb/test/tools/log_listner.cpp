/*
   Copyright (c) 2007, 2025, Oracle and/or its affiliates.

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

#include <mgmapi.h>
#include <ndb_global.h>
#include <ndb_opts.h>
#include <NDBT.hpp>
#include "util/TlsKeyManager.hpp"

static struct my_option my_long_options[] = {
    NDB_STD_OPTS("eventlog"),
    NdbStdOpt::tls_search_path,
    NdbStdOpt::mgm_tls,
    {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}};

int filter[] = {15, NDB_MGM_EVENT_CATEGORY_BACKUP,
                15, NDB_MGM_EVENT_CATEGORY_CONNECTION,
                15, NDB_MGM_EVENT_CATEGORY_NODE_RESTART,
                15, NDB_MGM_EVENT_CATEGORY_STARTUP,
                15, NDB_MGM_EVENT_CATEGORY_SHUTDOWN,
                15, NDB_MGM_EVENT_CATEGORY_STATISTIC,
                15, NDB_MGM_EVENT_CATEGORY_ERROR,
                15, NDB_MGM_EVENT_CATEGORY_CHECKPOINT,
                15, NDB_MGM_EVENT_CATEGORY_CONGESTION,
                0};

extern "C" void catch_signal(int signum) {}

int main(int argc, char **argv) {
  NDB_INIT(argv[0]);
  Ndb_opts opts(argc, argv, my_long_options);

  int ho_error;
#ifndef NDEBUG
  opt_debug = "d:t:O,/tmp/eventlog.trace";
#endif

#ifndef _WIN32
  // Catching signal to allow testing of EINTR safeness
  // with "while killall -USR1 eventlog; do true; done"
  signal(SIGUSR1, catch_signal);
#endif

  if ((ho_error = opts.handle_options()))
    return NDBT_ProgramExit(NDBT_WRONGARGS);

  TlsKeyManager tlsKeyManager;
  tlsKeyManager.init_mgm_client(opt_tls_search_path);

  NdbMgmHandle handle = ndb_mgm_create_handle();
  ndb_mgm_set_ssl_ctx(handle, tlsKeyManager.ctx());
  ndb_mgm_set_connectstring(handle, opt_ndb_connectstring);

  while (true) {
    if (ndb_mgm_connect_tls(handle, 0, 0, 0, opt_mgm_tls) == -1) {
      ndbout_c("Failed to connect");
      exit(0);
    }

    NdbLogEventHandle le = ndb_mgm_create_logevent_handle(handle, filter);
    if (le == 0) {
      ndbout_c("Failed to create logevent handle");
      exit(0);
    }

    struct ndb_logevent event;
    while (true) {
      int r = ndb_logevent_get_next(le, &event, 5000);
      if (r < 0) {
        ndbout_c("Error while getting next event");
        break;
      }
      if (r == 0) {
        continue;
      }
      ndbout_c("Got event: %d", event.type);
    }

    ndb_mgm_destroy_logevent_handle(&le);
    ndb_mgm_disconnect(handle);
  }

  return 0;
}
