/*
   Copyright (c) 2003, 2025, Oracle and/or its affiliates.

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

#include <ndb_global.h>
#include <NdbOut.hpp>

#include <NdbSleep.h>
#include <getarg.h>
#include <NDBT.hpp>
#include <NdbApi.hpp>
#include "Bank.hpp"

int main(int argc, const char **argv) {
  ndb_init();
  int _help = 0;
  const char *_database = "BANK";

  struct getargs args[] = {
      {"usage", '?', arg_flag, &_help, "Print help", ""},
      {"database", 'd', arg_string, &_database, "Database name", ""}};
  int num_args = sizeof(args) / sizeof(args[0]);
  int optind = 0;
  char desc[] = "This program will make GL records in the bank\n";

  if (getarg(args, num_args, argc, argv, &optind) || _help) {
    arg_printusage(args, num_args, argv[0], desc);
    return NDBT_ProgramExit(NDBT_WRONGARGS);
  }

  Ndb_cluster_connection con;
  con.configure_tls(opt_tls_search_path, opt_mgm_tls);
  if (con.connect(12, 5, 1) != 0) {
    return NDBT_ProgramExit(NDBT_FAILED);
  }

  Bank bank(con, _database);

  if (bank.performMakeGLs() != 0) return NDBT_ProgramExit(NDBT_FAILED);

  return NDBT_ProgramExit(NDBT_OK);
}
