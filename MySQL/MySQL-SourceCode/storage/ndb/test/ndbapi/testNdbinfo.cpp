/*
   Copyright (c) 2009, 2025, Oracle and/or its affiliates.

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

#include <NDBT.hpp>
#include <NDBT_Test.hpp>
#include "../../src/ndbapi/NdbInfo.hpp"
#include "util/require.h"

#include <NdbRestarter.hpp>

int runTestNdbInfo(NDBT_Context *ctx, NDBT_Step *step) {
  NdbInfo ndbinfo(&ctx->m_cluster_connection, "ndbinfo/");
  if (!ndbinfo.init()) {
    g_err << "ndbinfo.init failed" << endl;
    return NDBT_FAILED;
  }

  const NdbInfo::Table *table;
  if (ndbinfo.openTable("ndbinfo/tables", &table) != 0) {
    g_err << "Failed to openTable(tables)" << endl;
    return NDBT_FAILED;
  }

  for (int l = 0; l < ctx->getNumLoops(); l++) {
    NdbInfoScanOperation *scanOp = NULL;
    if (ndbinfo.createScanOperation(table, &scanOp)) {
      g_err << "No NdbInfoScanOperation" << endl;
      return NDBT_FAILED;
    }

    if (scanOp->readTuples() != 0) {
      g_err << "scanOp->readTuples failed" << endl;
      return NDBT_FAILED;
    }

    const NdbInfoRecAttr *tableName = scanOp->getValue("table_name");
    const NdbInfoRecAttr *comment = scanOp->getValue("comment");

    if (scanOp->execute() != 0) {
      g_err << "scanOp->execute failed" << endl;
      return NDBT_FAILED;
    }

    while (scanOp->nextResult() == 1) {
      g_info << "NAME: " << tableName->c_str() << endl;
      g_info << "COMMENT: " << comment->c_str() << endl;
    }
    ndbinfo.releaseScanOperation(scanOp);
  }

  ndbinfo.closeTable(table);
  return NDBT_OK;
}

static bool scan_table(NdbInfo &ndbinfo, const NdbInfo::Table *table,
                       int &rows) {
  NdbInfoScanOperation *scanOp = NULL;
  if (ndbinfo.createScanOperation(table, &scanOp)) {
    g_err << "No NdbInfoScanOperation" << endl;
    return false;
  }

  if (scanOp->readTuples() != 0) {
    g_err << "scanOp->readTuples failed" << endl;
    ndbinfo.releaseScanOperation(scanOp);
    return false;
  }

  int columnId = 0;
  while (scanOp->getValue(columnId)) columnId++;
  // At least one column
  require(columnId >= 1);
  int ret;
  if ((ret = scanOp->execute()) != 0) {
    g_err << "scanOp->execute failed, ret: " << ret << endl;
    ndbinfo.releaseScanOperation(scanOp);
    return false;
  }

  while ((ret = scanOp->nextResult()) == 1) rows++;

  ndbinfo.releaseScanOperation(scanOp);

  if (ret != 0) {
    g_err << "scanOp->nextResult failed, ret: " << ret << endl;
    return false;
  }

  return true;
}

int runScanAll(NDBT_Context *ctx, NDBT_Step *step) {
  NdbInfo ndbinfo(&ctx->m_cluster_connection, "ndbinfo/");
  if (!ndbinfo.init()) {
    g_err << "ndbinfo.init failed" << endl;
    return NDBT_FAILED;
  }

  Uint32 tableId = 0;
  while (true) {
    const NdbInfo::Table *table;

    int err = ndbinfo.openTable(tableId, &table);
    if (err == NdbInfo::ERR_NoSuchTable) {
      // No more tables -> return
      return NDBT_OK;
    } else if (err != 0) {
      // Unexpected return code
      g_err << "Failed to openTable(" << tableId << "), err: " << err << endl;
      return NDBT_FAILED;
    }
    ndbout << "table(" << tableId << "): " << table->getName() << endl;

    int last_rows = 0;
    bool rows_may_increase1 = (strstr(table->getName(), "cpustat_") != nullptr);
    bool rows_may_increase2 = (strstr(table->getName(), "cpudata_") != nullptr);
    bool rows_may_increase3 =
        (strstr(table->getName(), "index_stats") != nullptr);
    bool rows_may_increase =
        (rows_may_increase1 || rows_may_increase2 || rows_may_increase3);
    for (int l = 0; l < ctx->getNumLoops(); l++) {
      if (ctx->isTestStopped()) return NDBT_OK;

      int rows = 0;
      if (!scan_table(ndbinfo, table, rows)) {
        ctx->stopTest();
        return NDBT_FAILED;
      }
      // Check if the number of rows is as expected:
      // Expected scenario 1: Same number of rows as last round (or)
      // Expected scenario 2: Same or increased number of rows for tables which
      //                      might be still getting filled.
      if (l > 0 &&
          (rows != last_rows && !(rows_may_increase && rows > last_rows))) {
        g_err << "Got different number of rows this round, table: "
              << table->getName() << ", expected: "
              << ((rows_may_increase) ? "equal to or more than " : "")
              << last_rows << ", got: " << rows << endl;
        ndbinfo.closeTable(table);
        ctx->stopTest();
        return NDBT_FAILED;
      }
      last_rows = rows;
    }
    ndbinfo.closeTable(table);
    tableId++;
  }

  // Should never come here
  require(false);
  return NDBT_FAILED;
}

int runScanStop(NDBT_Context *ctx, NDBT_Step *step) {
  NdbInfo ndbinfo(&ctx->m_cluster_connection, "ndbinfo/");
  if (!ndbinfo.init()) {
    g_err << "ndbinfo.init failed" << endl;
    return NDBT_FAILED;
  }

  Uint32 tableId = 0;
  while (true) {
    const NdbInfo::Table *table;

    int err = ndbinfo.openTable(tableId, &table);
    if (err == NdbInfo::ERR_NoSuchTable) {
      // No more tables -> return
      return NDBT_OK;
    } else if (err != 0) {
      // Unexpected return code
      g_err << "Failed to openTable(" << tableId << "), err: " << err << endl;
      return NDBT_FAILED;
    }
    ndbout << "table: " << table->getName() << endl;

    for (int l = 0; l < ctx->getNumLoops() * 10; l++) {
      NdbInfoScanOperation *scanOp = NULL;
      if (ndbinfo.createScanOperation(table, &scanOp)) {
        g_err << "No NdbInfoScanOperation" << endl;
        return NDBT_FAILED;
      }

      if (scanOp->readTuples() != 0) {
        g_err << "scanOp->readTuples failed" << endl;
        return NDBT_FAILED;
      }

      int columnId = 0;
      while (scanOp->getValue(columnId)) columnId++;
      // At least one column
      require(columnId >= 1);

      if (scanOp->execute() != 0) {
        g_err << "scanOp->execute failed" << endl;
        return NDBT_FAILED;
      }

      int stopRow = rand() % 100;
      int row = 0;
      while (scanOp->nextResult() == 1) {
        row++;
        if (row == stopRow) {
          ndbout_c("Aborting scan at row %d", stopRow);
          break;
        }
      }
      ndbinfo.releaseScanOperation(scanOp);
    }
    ndbinfo.closeTable(table);
    tableId++;
  }

  // Should never come here
  require(false);
  return NDBT_FAILED;
}

int runRatelimit(NDBT_Context *ctx, NDBT_Step *step) {
  NdbInfo ndbinfo(&ctx->m_cluster_connection, "ndbinfo/");
  if (!ndbinfo.init()) {
    g_err << "ndbinfo.init failed" << endl;
    return NDBT_FAILED;
  }

  Uint32 tableId = 0;
  while (true) {
    const NdbInfo::Table *table;

    int err = ndbinfo.openTable(tableId, &table);
    if (err == NdbInfo::ERR_NoSuchTable) {
      // No more tables -> return
      return NDBT_OK;
    } else if (err != 0) {
      // Unexpected return code
      g_err << "Failed to openTable(" << tableId << "), err: " << err << endl;
      return NDBT_FAILED;
    }
    ndbout << "table: " << table->getName() << endl;

    struct {
      Uint32 rows;
      Uint32 bytes;
    } limits[] = {{0, 0}, {1, 0},   {2, 0},   {10, 0},     {37, 0},   {1000, 0},
                  {0, 1}, {0, 2},   {0, 10},  {0, 37},     {0, 1000}, {1, 1},
                  {2, 2}, {10, 10}, {37, 37}, {1000, 1000}};

    int lastRows = 0;
    bool rows_may_increase1 = (strstr(table->getName(), "cpustat_") != nullptr);
    bool rows_may_increase2 = (strstr(table->getName(), "cpudata_") != nullptr);
    bool rows_may_increase3 =
        (strstr(table->getName(), "index_stats") != nullptr);
    bool rows_may_increase =
        (rows_may_increase1 || rows_may_increase2 || rows_may_increase3);
    for (int l = 0; l < (int)(sizeof(limits) / sizeof(limits[0])); l++) {
      Uint32 maxRows = limits[l].rows;
      Uint32 maxBytes = limits[l].bytes;

      NdbInfoScanOperation *scanOp = NULL;
      if (ndbinfo.createScanOperation(table, &scanOp, maxRows, maxBytes)) {
        g_err << "No NdbInfoScanOperation" << endl;
        return NDBT_FAILED;
      }

      if (scanOp->readTuples() != 0) {
        g_err << "scanOp->readTuples failed" << endl;
        return NDBT_FAILED;
      }

      int columnId = 0;
      while (scanOp->getValue(columnId)) columnId++;
      // At least one column
      require(columnId >= 1);

      if (scanOp->execute() != 0) {
        g_err << "scanOp->execute failed" << endl;
        return NDBT_FAILED;
      }

      int row = 0;
      while (scanOp->nextResult() == 1) row++;
      ndbinfo.releaseScanOperation(scanOp);

      ndbout_c("[%u,%u] rows: %d", maxRows, maxBytes, row);
      // Check if the number of rows is as expected:
      // Expected scenario 1: Same number of rows as last round (or)
      // Expected scenario 2: Same or increased number of rows for tables which
      //                      might be still getting filled.
      if (lastRows != 0 &&
          (row != lastRows && !(rows_may_increase && row > lastRows))) {
        g_err << "Got different number of rows this round, table: "
              << table->getName() << ", expected: "
              << ((rows_may_increase) ? "equal to or more than " : "")
              << lastRows << ", got: " << row << endl;
        ndbinfo.closeTable(table);
        return NDBT_FAILED;
      }
      lastRows = row;
    }
    ndbinfo.closeTable(table);
    tableId++;
  }

  // Should never come here
  require(false);
  return NDBT_FAILED;
}

int runTestTable(NDBT_Context *ctx, NDBT_Step *step) {
  NdbInfo ndbinfo(&ctx->m_cluster_connection, "ndbinfo/");
  if (!ndbinfo.init()) {
    g_err << "ndbinfo.init failed" << endl;
    return NDBT_FAILED;
  }

  const NdbInfo::Table *table;
  if (ndbinfo.openTable("ndbinfo/test", &table) != 0) {
    g_err << "Failed to openTable(test)" << endl;
    return NDBT_FAILED;
  }

  for (int l = 0; l < ctx->getNumLoops(); l++) {
    NdbInfoScanOperation *scanOp = NULL;
    if (ndbinfo.createScanOperation(table, &scanOp)) {
      ndbinfo.closeTable(table);
      g_err << "No NdbInfoScanOperation" << endl;
      return NDBT_FAILED;
    }

    if (scanOp->readTuples() != 0) {
      ndbinfo.releaseScanOperation(scanOp);
      ndbinfo.closeTable(table);
      g_err << "scanOp->readTuples failed" << endl;
      return NDBT_FAILED;
    }

    const NdbInfoRecAttr *nodeId = scanOp->getValue("node_id");
    const NdbInfoRecAttr *blockNumber = scanOp->getValue("block_number");
    const NdbInfoRecAttr *blockInstance = scanOp->getValue("block_instance");
    const NdbInfoRecAttr *counter = scanOp->getValue("counter");
    const NdbInfoRecAttr *counter2 = scanOp->getValue("counter2");

    if (scanOp->execute() != 0) {
      ndbinfo.releaseScanOperation(scanOp);
      ndbinfo.closeTable(table);
      g_err << "scanOp->execute failed" << endl;
      return NDBT_FAILED;
    }

    int ret;
    int rows = 0;
    while ((ret = scanOp->nextResult()) == 1) {
      rows++;
      (void)nodeId->u_32_value();
      (void)blockNumber->u_32_value();
      (void)blockInstance->u_32_value();
      (void)counter->u_32_value();
      (void)counter2->u_64_value();
    }
    ndbinfo.releaseScanOperation(scanOp);
    if (ret != 0) {
      ndbinfo.closeTable(table);
      g_err << "scan failed, ret: " << ret << endl;
      return NDBT_FAILED;
    }
    ndbout << "rows: " << rows << endl;
  }

  ndbinfo.closeTable(table);
  return NDBT_OK;
}

int runTestTableUntilStopped(NDBT_Context *ctx, NDBT_Step *step) {
  int i = 0;
  while (ctx->isTestStopped() == false) {
    g_info << i << ": ";
    (void)runTestTable(ctx, step);
    i++;
  }
  return NDBT_OK;
}

int runRestarter(NDBT_Context *ctx, NDBT_Step *step) {
  int result = NDBT_OK;
  int loops = ctx->getNumLoops();
  int sync_threads = ctx->getProperty("SyncThreads", (unsigned)0);
  int sleep0 = ctx->getProperty("Sleep0", (unsigned)0);
  int sleep1 = ctx->getProperty("Sleep1", (unsigned)0);
  int randnode = ctx->getProperty("RandNode", (unsigned)0);
  NdbRestarter restarter;
  int i = 0;
  int lastId = 0;

  if (restarter.getNumDbNodes() < 2) {
    ctx->stopTest();
    return NDBT_OK;
  }

  if (restarter.waitClusterStarted() != 0) {
    g_err << "Cluster failed to start" << endl;
    return NDBT_FAILED;
  }

  if (loops > restarter.getNumDbNodes()) loops = restarter.getNumDbNodes();

  while (i < loops && result != NDBT_FAILED && !ctx->isTestStopped()) {
    int id = lastId % restarter.getNumDbNodes();
    if (randnode == 1) {
      id = rand() % restarter.getNumDbNodes();
    }
    int nodeId = restarter.getDbNodeId(id);
    ndbout << "Restart node " << nodeId << endl;
    if (restarter.restartOneDbNode(nodeId, false, true, true) != 0) {
      g_err << "Failed to restartNextDbNode" << endl;
      result = NDBT_FAILED;
      break;
    }

    if (restarter.waitNodesNoStart(&nodeId, 1)) {
      g_err << "Failed to waitNodesNoStart" << endl;
      result = NDBT_FAILED;
      break;
    }

    if (sleep1) NdbSleep_MilliSleep(sleep1);

    if (restarter.startNodes(&nodeId, 1)) {
      g_err << "Failed to start node" << endl;
      result = NDBT_FAILED;
      break;
    }

    if (restarter.waitClusterStarted() != 0) {
      g_err << "Cluster failed to start" << endl;
      result = NDBT_FAILED;
      break;
    }

    if (sleep0) NdbSleep_MilliSleep(sleep0);

    ctx->sync_up_and_wait("PauseThreads", sync_threads);

    lastId++;
    i++;
  }

  ctx->stopTest();

  return result;
}

int runCreateDropTableUntilStopped(NDBT_Context *ctx, NDBT_Step *step) {
  Ndb *pNdb = GETNDB(step);
  NdbRestarter res;
  const char *tableName = ctx->getProperty("tableName", (char *)NULL);

  NdbDictionary::Dictionary *pDict = pNdb->getDictionary();
  Uint32 stepNum = step->getStepNo();
  BaseString tabName(tableName);
  tabName.appfmt("_%i", stepNum);

  NdbDictionary::Table tab(tabName.c_str());
  {
    NdbDictionary::Column col("a");
    col.setType(NdbDictionary::Column::Unsigned);
    col.setPrimaryKey(true);
    tab.addColumn(col);
  }
  {
    NdbDictionary::Column col("b");
    col.setType(NdbDictionary::Column::Unsigned);
    col.setNullable(false);
    tab.addColumn(col);
  }

  while (!ctx->isTestStopped()) {
    /*
     * 3006: Error Insert to delay handling of ACCFRAGREQ (and indirectly
     * TUPFRAGREQ).
     * 4039: Error Insert to delay fragment release in TUP.
     */
    int error = rand() % 2 ? 3006 : 4039;
    if (res.insertErrorInAllNodes(error) != 0) {
      g_err << "Failed to insertError " << error << endl;
      return NDBT_FAILED;
    }

    if (pDict->createTable(tab) != 0) {
      NdbError err = pDict->getNdbError();
      g_err << "Failed to create table (" << tabName.c_str() << ") " << err
            << endl;
      /**
       * if error is:
       * 701: System busy with other schema operation, or
       * 721: Schema object with given name already exists
       * test can continue
       */
      if (err.code != 701 && err.code != 721) {
        return NDBT_FAILED;
      }
    }

    /**
     * if error is:
     * 701: System busy with other schema operation, or
     * 723: No such table existed
     * test can continue
     */
    if (pDict->dropTable(tabName.c_str()) != 0) {
      NdbError err = pDict->getNdbError();
      g_err << "Failed to drop table (" << tabName.c_str() << ") " << err
            << endl;
      if (err.code != 701 && err.code != 723) {
        return NDBT_FAILED;
      }
    }
    NdbSleep_MilliSleep(20);
    if (res.insertErrorInAllNodes(0) != 0) {
      g_err << "Failed to clear Error " << error << endl;
      return NDBT_FAILED;
    }
  }
  return NDBT_OK;
}

int runScanNdbInfoTable(NDBT_Context *ctx, NDBT_Step *step) {
  int result = NDBT_OK;
  int loops = ctx->getNumLoops();
  const char *tableName = ctx->getProperty("infoTableName", (char *)NULL);

  NdbInfo ndbinfo(&ctx->m_cluster_connection, "ndbinfo/");
  if (!ndbinfo.init()) {
    g_err << "ndbinfo.init failed" << endl;
    ctx->stopTest();
    return NDBT_FAILED;
  }

  const NdbInfo::Table *table;
  if (ndbinfo.openTable(tableName, &table) != 0) {
    g_err << "Failed to openTable " << tableName << endl;
    ctx->stopTest();
    return NDBT_FAILED;
  }

  while (loops-- && !ctx->isTestStopped()) {
    NdbInfoScanOperation *scanOp = nullptr;
    if (ndbinfo.createScanOperation(table, &scanOp)) {
      g_err << "createScanOperation failed" << endl;
      ndbinfo.releaseScanOperation(scanOp);
      result = NDBT_FAILED;
      break;
    }

    if (scanOp->readTuples() != 0) {
      g_err << "scanOp->readTuples failed" << endl;
      ndbinfo.releaseScanOperation(scanOp);
      result = NDBT_FAILED;
      break;
    }

    if (scanOp->execute() != 0) {
      g_err << "scanOp->execute failed" << endl;
      ndbinfo.releaseScanOperation(scanOp);
      result = NDBT_FAILED;
      break;
    }
    while (scanOp->nextResult() == 1) {
    }

    ndbinfo.releaseScanOperation(scanOp);
  }

  ndbinfo.closeTable(table);
  ctx->stopTest();
  return result;
}

NDBT_TESTSUITE(testNdbinfo);
TESTCASE("NodeRestart", "Scan NdbInfo tables while restarting nodes") {
  STEP(runRestarter);
  STEPS(runTestTableUntilStopped, 1);
}
TESTCASE("Ndbinfo", "Test ndbapi interface to NDB$INFO") {
  INITIALIZER(runTestNdbInfo);
}
TESTCASE("Ndbinfo10", "Test ndbapi interface to NDB$INFO") {
  STEPS(runTestNdbInfo, 10);
}
TESTCASE("ScanAll",
         "Scan all colums of all table known to NdbInfo"
         "check that number of rows returned are as expected."
         "Either they should be same across multiple iterations or"
         "should increasing if in case the table is still getting filled.") {
  STEPS(runScanAll, 1);
}
TESTCASE("ScanAll10",
         "Scan all columns of all table known to NdbInfo from "
         "10 parallel threads, check that number of rows returned are as"
         "expected. Either they should be same across multiple iterations or"
         "should increasing if in case the table is still getting filled.") {
  STEPS(runScanAll, 10);
}
TESTCASE("ScanStop", "Randomly stop the scan") { STEPS(runScanStop, 1); }
TESTCASE("Ratelimit", "Scan wit different combinations of ratelimit") {
  STEPS(runRatelimit, 1);
}
TESTCASE("TestTable",
         "Scan the test table and make sure it returns correct number "
         "of rows which will depend on how many TUP blocks are configured") {
  STEP(runTestTable);
}
TESTCASE("ScanFragOperationsDuringCreateDropTable",
         "Check that scanning of ndbinfo/frag_operations table is robust"
         "to CREATE/DROP table operation running in parallel with the scan") {
  TC_PROPERTY("tableName", "tmp_table");
  TC_PROPERTY("infoTableName", "ndbinfo/frag_operations");
  STEPS(runCreateDropTableUntilStopped, 1);
  STEPS(runScanNdbInfoTable, 16);
}
TESTCASE("ScanFragMemUseDuringCreateDropTable",
         "Check that scanning of ndbinfo/frag_mem_use table is robust"
         "to CREATE/DROP table operation running in parallel with the scan") {
  TC_PROPERTY("tableName", "tmp_table");
  TC_PROPERTY("infoTableName", "ndbinfo/frag_mem_use");
  STEPS(runCreateDropTableUntilStopped, 1);
  STEPS(runScanNdbInfoTable, 16);
}

NDBT_TESTSUITE_END(testNdbinfo)

int main(int argc, const char **argv) {
  ndb_init();
  NDBT_TESTSUITE_INSTANCE(testNdbinfo);
  testNdbinfo.setCreateTable(false);
  testNdbinfo.setRunAllTables(true);
  return testNdbinfo.execute(argc, argv);
}
