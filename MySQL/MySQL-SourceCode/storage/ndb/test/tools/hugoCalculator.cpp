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

#include <NdbSleep.h>
#include <getarg.h>
#include <HugoCalculator.hpp>
#include <NDBT.hpp>
#include <NDBT_Tables.hpp>
#include <NdbOut.hpp>

// extern NdbOut g_info;

int main(int argc, const char **argv) {
  ndb_init();
  int _row = 0;
  int _column = 0;
  int _updates = 0;
  const char *_tableName = NULL;

  struct getargs args[] = {
      {"row", 'r', arg_integer, &_row, "The row number", "row"},
      {"column", 'c', arg_integer, &_column, "The column id", "column"},
      {"updates", 'u', arg_integer, &_updates, "# of updates", "updates"}};

  int num_args = sizeof(args) / sizeof(args[0]);
  int optind = 0;

  if (getarg(args, num_args, argc, argv, &optind) || argv[optind] == NULL) {
    arg_printusage(args, num_args, argv[0], "table name\n");
    return NDBT_WRONGARGS;
  }
  // Check if table name is supplied
  if (argv[optind] != NULL) _tableName = argv[optind];

  const NdbDictionary::Table *table = NDBT_Tables::getTable(_tableName);
  const NdbDictionary::Column *attribute = table->getColumn(_column);

  g_info << "Table " << _tableName << endl
         << "Row: " << _row << ", "
         << "Column(" << attribute->getName() << ")"
         << "[" << attribute->getType() << "]"
         << ", Updates: " << _updates << endl;

  HugoCalculator calc(*table);
  char buf[8000];
  g_info << "Value: " << calc.calcValue(_row, _column, _updates, buf) << endl;

  return 0;
}
