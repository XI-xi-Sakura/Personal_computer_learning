/*
   Copyright (c) 2005, 2025, Oracle and/or its affiliates.

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

/*
 *  ndbapi_simple.cpp: Using synchronous transactions in NDB API
 *
 *  Correct output from this program is:
 *
 *  ATTR1 ATTR2
 *    0    10
 *    1     1
 *    2    12
 *  Detected that deleted tuple doesn't exist!
 *    4    14
 *    5     5
 *    6    16
 *    7     7
 *    8    18
 *    9     9
 *
 */

#include <mysql.h>
#include <mysqld_error.h>
#include <NdbApi.hpp>
#include <cstdlib>
// Used for cout
#include <cstdio>
#include <iostream>

static void run_application(MYSQL &, Ndb_cluster_connection &);

#define PRINT_ERROR(code, msg)                                   \
  std::cout << "Error in " << __FILE__ << ", line: " << __LINE__ \
            << ", code: " << code << ", msg: " << msg << "." << std::endl
#define MYSQLERROR(mysql)                                  \
  {                                                        \
    PRINT_ERROR(mysql_errno(&mysql), mysql_error(&mysql)); \
    exit(-1);                                              \
  }
#define APIERROR(error)                     \
  {                                         \
    PRINT_ERROR(error.code, error.message); \
    exit(-1);                               \
  }

int main(int argc, char **argv) {
  if (argc != 3) {
    std::cout << "Arguments are <socket mysqld> <connect_string cluster>.\n";
    exit(-1);
  }
  // ndb_init must be called first
  ndb_init();

  // connect to mysql server and cluster and run application
  {
    char *mysqld_sock = argv[1];
    const char *connectstring = argv[2];
    // Object representing the cluster
    Ndb_cluster_connection cluster_connection(connectstring);

    // Connect to cluster management server (ndb_mgmd)
    if (cluster_connection.connect(4 /* retries               */,
                                   5 /* delay between retries */,
                                   1 /* verbose               */)) {
      std::cout << "Cluster management server was not ready within 30 secs.\n";
      exit(-1);
    }

    // Optionally connect and wait for the storage nodes (ndbd's)
    if (cluster_connection.wait_until_ready(30, 0) < 0) {
      std::cout << "Cluster was not ready within 30 secs.\n";
      exit(-1);
    }

    // connect to mysql server
    MYSQL mysql;
    if (!mysql_init(&mysql)) {
      std::cout << "mysql_init failed\n";
      exit(-1);
    }
    if (!mysql_real_connect(&mysql, "localhost", "root", "", "", 0, mysqld_sock,
                            0))
      MYSQLERROR(mysql);

    // run the application code
    run_application(mysql, cluster_connection);
    mysql_close(&mysql);
  }

  ndb_end(0);

  return 0;
}

static void create_table(MYSQL &);
static void do_insert(Ndb &);
static void do_update(Ndb &);
static void do_delete(Ndb &);
static void do_read(Ndb &);

static void run_application(MYSQL &mysql,
                            Ndb_cluster_connection &cluster_connection) {
  /********************************************
   * Connect to database via mysql-c          *ndb_examples
   ********************************************/
  mysql_query(&mysql, "CREATE DATABASE ndb_examples");
  if (mysql_query(&mysql, "USE ndb_examples") != 0) MYSQLERROR(mysql);
  create_table(mysql);

  /********************************************
   * Connect to database via NdbApi           *
   ********************************************/
  // Object representing the database
  Ndb myNdb(&cluster_connection, "ndb_examples");
  if (myNdb.init()) APIERROR(myNdb.getNdbError());

  /*
   * Do different operations on database
   */
  do_insert(myNdb);
  do_update(myNdb);
  do_delete(myNdb);
  do_read(myNdb);
}

/*********************************************************
 * Create a table named api_simple if it does not exist *
 *********************************************************/
static void create_table(MYSQL &mysql) {
  while (mysql_query(&mysql,
                     "CREATE TABLE"
                     "  api_simple"
                     "    (ATTR1 INT UNSIGNED NOT NULL PRIMARY KEY,"
                     "     ATTR2 INT UNSIGNED NOT NULL)"
                     "  ENGINE=NDB")) {
    if (mysql_errno(&mysql) == ER_TABLE_EXISTS_ERROR) {
      std::cout << "MySQL Cluster already has example table: api_simple. "
                << "Dropping it..." << std::endl;
      mysql_query(&mysql, "DROP TABLE api_simple");
    } else
      MYSQLERROR(mysql);
  }
}

/**************************************************************************
 * Using 5 transactions, insert 10 tuples in table: (0,0),(1,1),...,(9,9) *
 **************************************************************************/
static void do_insert(Ndb &myNdb) {
  const NdbDictionary::Dictionary *myDict = myNdb.getDictionary();
  const NdbDictionary::Table *myTable = myDict->getTable("api_simple");

  if (myTable == nullptr) APIERROR(myDict->getNdbError());

  for (int i = 0; i < 5; i++) {
    NdbTransaction *myTransaction = myNdb.startTransaction();
    if (myTransaction == nullptr) APIERROR(myNdb.getNdbError());

    NdbOperation *myOperation = myTransaction->getNdbOperation(myTable);
    if (myOperation == nullptr) APIERROR(myTransaction->getNdbError());

    myOperation->insertTuple();
    myOperation->equal("ATTR1", i);
    myOperation->setValue("ATTR2", i);

    myOperation = myTransaction->getNdbOperation(myTable);
    if (myOperation == nullptr) APIERROR(myTransaction->getNdbError());

    myOperation->insertTuple();
    myOperation->equal("ATTR1", i + 5);
    myOperation->setValue("ATTR2", i + 5);

    if (myTransaction->execute(NdbTransaction::Commit) == -1)
      APIERROR(myTransaction->getNdbError());

    myNdb.closeTransaction(myTransaction);
  }
}

/*****************************************************************
 * Update the second attribute in half of the tuples (adding 10) *
 *****************************************************************/
static void do_update(Ndb &myNdb) {
  const NdbDictionary::Dictionary *myDict = myNdb.getDictionary();
  const NdbDictionary::Table *myTable = myDict->getTable("api_simple");

  if (myTable == nullptr) APIERROR(myDict->getNdbError());

  for (int i = 0; i < 10; i += 2) {
    NdbTransaction *myTransaction = myNdb.startTransaction();
    if (myTransaction == nullptr) APIERROR(myNdb.getNdbError());

    NdbOperation *myOperation = myTransaction->getNdbOperation(myTable);
    if (myOperation == nullptr) APIERROR(myTransaction->getNdbError());

    myOperation->updateTuple();
    myOperation->equal("ATTR1", i);
    myOperation->setValue("ATTR2", i + 10);

    if (myTransaction->execute(NdbTransaction::Commit) == -1)
      APIERROR(myTransaction->getNdbError());

    myNdb.closeTransaction(myTransaction);
  }
}

/*************************************************
 * Delete one tuple (the one with primary key 3) *
 *************************************************/
static void do_delete(Ndb &myNdb) {
  const NdbDictionary::Dictionary *myDict = myNdb.getDictionary();
  const NdbDictionary::Table *myTable = myDict->getTable("api_simple");

  if (myTable == nullptr) APIERROR(myDict->getNdbError());

  NdbTransaction *myTransaction = myNdb.startTransaction();
  if (myTransaction == nullptr) APIERROR(myNdb.getNdbError());

  NdbOperation *myOperation = myTransaction->getNdbOperation(myTable);
  if (myOperation == nullptr) APIERROR(myTransaction->getNdbError());

  myOperation->deleteTuple();
  myOperation->equal("ATTR1", 3);

  if (myTransaction->execute(NdbTransaction::Commit) == -1)
    APIERROR(myTransaction->getNdbError());

  myNdb.closeTransaction(myTransaction);
}

/*****************************
 * Read and print all tuples *
 *****************************/
static void do_read(Ndb &myNdb) {
  const NdbDictionary::Dictionary *myDict = myNdb.getDictionary();
  const NdbDictionary::Table *myTable = myDict->getTable("api_simple");

  if (myTable == nullptr) APIERROR(myDict->getNdbError());

  std::cout << "ATTR1 ATTR2" << std::endl;

  for (int i = 0; i < 10; i++) {
    NdbTransaction *myTransaction = myNdb.startTransaction();
    if (myTransaction == nullptr) APIERROR(myNdb.getNdbError());

    NdbOperation *myOperation = myTransaction->getNdbOperation(myTable);
    if (myOperation == nullptr) APIERROR(myTransaction->getNdbError());

    myOperation->readTuple(NdbOperation::LM_Read);
    myOperation->equal("ATTR1", i);

    NdbRecAttr *myRecAttr = myOperation->getValue("ATTR2", nullptr);
    if (myRecAttr == nullptr) APIERROR(myTransaction->getNdbError());

    if (myTransaction->execute(NdbTransaction::Commit) == -1)
      APIERROR(myTransaction->getNdbError());

    if (myTransaction->getNdbError().classification == NdbError::NoDataFound) {
      if (i == 3)
        std::cout << "Detected that deleted tuple doesn't exist!" << std::endl;
      else
        APIERROR(myTransaction->getNdbError());
    }

    if (i != 3) {
      printf(" %2d    %2d\n", i, myRecAttr->u_32_value());
    }
    myNdb.closeTransaction(myTransaction);
  }
}
