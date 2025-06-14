/*
  Copyright (c) 2010, 2025, Oracle and/or its affiliates.

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

#include "NdbapiAB.hpp"

#include <cassert>
#include <cstddef>
#include <iostream>
#include <string>

#include "CrundDriver.hpp"
#include "helpers.hpp"
#include "string_helpers.hpp"

#include <NdbError.hpp>

using std::cout;
using std::endl;
using std::flush;
using std::ostringstream;
using std::string;

// ----------------------------------------------------------------------
// Helper Macros & Functions
// ----------------------------------------------------------------------

// Current error handling is crude & simple:
// - all calls' return value is checked for errors
// - all errors are reported and followed by a process exit

#define ABORT_NDB_ERROR(error)                                              \
  do {                                                                      \
    cout << "!!! error in " << __FILE__ << ", line: " << __LINE__           \
         << ", code: " << (int)(error).code << ", msg: " << (error).message \
         << "." << endl;                                                    \
    exit(-1);                                                               \
  } while (0)

#define VERIFY_SCHEMA(cond) \
  if (cond)                 \
    ;                       \
  else                      \
    ABORT_ERROR("!!! schema data verification failed")

/* XXX use toStr() instead of abort() ?
// returns a string representation of an NdbError
string toStr(const NdbError* e) {
    return string("NdbError[") + toString(e->code) + "]: " + e->message;
}
*/

// ----------------------------------------------------------------------
// Crund metadata
// ----------------------------------------------------------------------

// crund's schema information as constants
struct Model {
  // type shortcut
  typedef NdbDictionary D;

  // dictionary objects
  const D::Table *table_A;
  const D::Table *table_B;
  const D::Column *column_A_id;
  const D::Column *column_A_cint;
  const D::Column *column_A_clong;
  const D::Column *column_A_cfloat;
  const D::Column *column_A_cdouble;
  const D::Column *column_B_id;
  const D::Column *column_B_cint;
  const D::Column *column_B_clong;
  const D::Column *column_B_cfloat;
  const D::Column *column_B_cdouble;
  const D::Column *column_B_aid;
  const D::Column *column_B_cvarbinary_def;
  const D::Column *column_B_cvarchar_def;
  const D::Index *idx_B_aid;

  // attribute ids
  int attr_id;
  int attr_cint;
  int attr_clong;
  int attr_cfloat;
  int attr_cdouble;
  int attr_B_aid;
  int attr_B_cvarbinary_def;
  int attr_B_cvarchar_def;
  int attr_idx_B_aid;

  // widths
  int width_id;
  int width_cint;
  int width_clong;
  int width_cfloat;
  int width_cdouble;
  int width_B_aid;
  int width_B_cvarbinary_def;    // width of column including length prefix
  int width_B_cvarchar_def;      // width of column including length prefix
  int width_A_row;               // sum of width of columns in A
  int width_B_row;               // sum of width of columns in B
  int width_AB_row;              // sum of width of common columns in A,B
  int wprefix_B_cvarbinary_def;  // width of length prefix
  int wprefix_B_cvarchar_def;    // width of length prefix

  static const D::Table *getTable(const D::Dictionary *d, const char *t) {
    const D::Table *r = d->getTable(t);
    if (r == nullptr) ABORT_NDB_ERROR(d->getNdbError());
    return r;
  }

  static const D::Column *getColumn(const D::Dictionary *d, const D::Table *t,
                                    const char *c) {
    const D::Column *r = t->getColumn(c);
    if (r == nullptr) ABORT_NDB_ERROR(d->getNdbError());
    return r;
  }

  static const D::Index *getIndex(const D::Dictionary *d, const char *n,
                                  const char *c) {
    const D::Index *r = d->getIndex(n, c);
    if (r == nullptr) ABORT_NDB_ERROR(d->getNdbError());
    return r;
  }

  static int attrId(const D::Column *c) { return c->getColumnNo(); }

  static int getAttrId(const D::Column *c0, const D::Column *c1) {
    int r = attrId(c0);
    VERIFY_SCHEMA(r == attrId(c1));
    return r;
  }

  static int columnWidth(const D::Column *c) {
    int s = c->getSize();        // size of type or of base type
    int al = c->getLength();     // length or max length, 1 for scalars
    int at = c->getArrayType();  // size of length prefix, practically
    return (s * al) + at;
  }

  static int getColumnWidth(const D::Column *c0, const D::Column *c1) {
    int r = columnWidth(c0);
    VERIFY_SCHEMA(r == columnWidth(c1));
    return r;
  }

  // initialize this instance from the dictionary
  void init(Ndb *ndb) {
    assert(ndb);
    const D::Dictionary *const dict = ndb->getDictionary();

    // get dictionary objects for tables A, B
    table_A = getTable(dict, "a");
    table_B = getTable(dict, "b");
    column_A_id = getColumn(dict, table_A, "id");
    column_A_cint = getColumn(dict, table_A, "cint");
    column_A_clong = getColumn(dict, table_A, "clong");
    column_A_cfloat = getColumn(dict, table_A, "cfloat");
    column_A_cdouble = getColumn(dict, table_A, "cdouble");
    column_B_id = getColumn(dict, table_B, "id");
    column_B_cint = getColumn(dict, table_B, "cint");
    column_B_clong = getColumn(dict, table_B, "clong");
    column_B_cfloat = getColumn(dict, table_B, "cfloat");
    column_B_cdouble = getColumn(dict, table_B, "cdouble");
    column_B_aid = getColumn(dict, table_B, "a_id");
    column_B_cvarbinary_def = getColumn(dict, table_B, "cvarbinary_def");
    column_B_cvarchar_def = getColumn(dict, table_B, "cvarchar_def");
    idx_B_aid = getIndex(dict, "I_B_FK", "b");

    // get common attribute ids for tables A, B
    attr_id = getAttrId(column_A_id, column_B_id);
    attr_cint = getAttrId(column_A_cint, column_B_cint);
    attr_clong = getAttrId(column_A_clong, column_B_clong);
    attr_cfloat = getAttrId(column_A_cfloat, column_B_cfloat);
    attr_cdouble = getAttrId(column_A_cdouble, column_B_cdouble);

    // get extra attribute ids for table B
    attr_B_aid = attrId(column_B_aid);
    attr_B_cvarbinary_def = attrId(column_B_cvarbinary_def);
    attr_B_cvarchar_def = attrId(column_B_cvarchar_def);

    // get attribute ids for columns in index B_aid
    attr_idx_B_aid = attrId(idx_B_aid->getColumn(0));

    // get the width of common columns in tables A, B
    width_id = getColumnWidth(column_A_id, column_B_id);
    width_cint = getColumnWidth(column_A_cint, column_B_cint);
    width_clong = getColumnWidth(column_A_clong, column_B_clong);
    width_cfloat = getColumnWidth(column_A_cfloat, column_B_cfloat);
    width_cdouble = getColumnWidth(column_A_cdouble, column_B_cdouble);

    // get the width of extra columns in table B
    width_B_aid = columnWidth(column_B_aid);
    width_B_cvarbinary_def = columnWidth(column_B_cvarbinary_def);
    width_B_cvarchar_def = columnWidth(column_B_cvarchar_def);

    // row width of tables A, B
    width_A_row =
        (+width_id + width_cint + width_clong + width_cfloat + width_cdouble);
    width_B_row =
        (+width_id + width_cint + width_clong + width_cfloat + width_cdouble +
         width_B_aid + width_B_cvarbinary_def + width_B_cvarchar_def);
    width_AB_row =
        (+width_id + width_cint + width_clong + width_cfloat + width_cdouble);

    // get the width of the length prefix of columns in table B
    wprefix_B_cvarbinary_def = column_B_cvarbinary_def->getArrayType();
    wprefix_B_cvarchar_def = column_B_cvarchar_def->getArrayType();
  }
};

// ----------------------------------------------------------------------
// initializers/finalizers
// ----------------------------------------------------------------------

void NdbapiAB::init() {
  CrundLoad::init();
  assert(mgmd == NULL);

  // ndb_init must be called first
  cout << endl << "initializing library ..." << flush;
  int stat = ndb_init();
  if (stat != 0) ABORT_ERROR("ndb_init() returned: " << stat);
  cout << "        [ok: ndbclient]" << endl;

  // instantiate NDB cluster singleton
  cout << "creating cluster connection ..." << flush;
  assert(!mgmdConnect.empty());
  mgmd = new Ndb_cluster_connection(mgmdConnect.c_str());
  //  mgmd->configure_tls(opt_tls_search_path, opt_mgm_tls);
  cout << " [ok: mgmd@" << mgmdConnect << "]" << endl;
}

void NdbapiAB::close() {
  assert(mgmd != NULL);

  // release NDB cluster singleton
  cout << endl << "closing cluster connection ..." << flush;
  delete mgmd;
  mgmd = nullptr;
  cout << "  [ok]" << endl;

  // ndb_close must be called last
  cout << "closing library ..." << flush;
  ndb_end(0);
  cout << "             [ok: ndbclient]" << endl;

  CrundLoad::close();
}

void NdbapiAB::initProperties() {
  CrundLoad::initProperties();

  cout << endl << "reading NDB properties ..." << flush;
  ostringstream msg;
  Properties &props = driver.getProperties();

  mgmdConnect = toS(props[L"ndb.mgmdConnect"], L"localhost");
  catalog = toS(props[L"ndb.catalog"], L"crunddb");
  schema = toS(props[L"ndb.schema"], L"def");

  nMaxConcTx = toI(props[L"ndb.nMaxConcTx"], 1024, 0);
  if (nMaxConcTx < 1) {
    msg << "[IGNORED] nMaxConcTx:           '" << toS(props[L"nMaxConcTx"])
        << "'" << endl;
    nMaxConcTx = 1024;
  }

  nConcScans = toI(props[L"ndb.nConcScans"], 255, 0);
  if (nConcScans < 1) {
    msg << "[IGNORED] nConcScans:           '" << toS(props[L"nConcScans"])
        << "'" << endl;
    nConcScans = 255;
  }

  if (!msg.tellp()) {  // or msg.str().empty() if ambiguous
    cout << "      [ok]" << endl;
  } else {
    driver.setIgnoredSettings();
    cout << endl << msg.str() << flush;
  }
}

void NdbapiAB::printProperties() {
  CrundLoad::printProperties();

  cout << endl
       << "NDB settings ..." << endl
       << "ndb.mgmdConnect:                \"" << mgmdConnect << "\"" << endl
       << "ndb.catalog:                    \"" << catalog << "\"" << endl
       << "ndb.schema:                     \"" << schema << "\"" << endl
       << "ndb.nMaxConcTx:                 " << nMaxConcTx << endl
       << "ndb.nConcScans:                 " << nConcScans << endl;
}

// ----------------------------------------------------------------------
// datastore operations
// ----------------------------------------------------------------------

void NdbapiAB::initConnection() {
  assert(mgmd != NULL);
  assert(ndb == NULL);
  assert(model == NULL);
  cout << endl << "initializing NDB resources ..." << endl;

  // connect to cluster management node (ndb_mgmd)
  cout << "connecting to cluster ..." << flush;
  const int retries = 0;  // number of retries (< 0 = indefinitely)
  const int delay = 0;    // seconds to wait after retry
  const int verbose = 1;  // print report of progress
  // returns: 0 = success, 1 = recoverable error, -1 = non-recoverable error
  if (mgmd->connect(retries, delay, verbose) != 0)
    ABORT_ERROR("mgmd@" << mgmdConnect << " was not ready within "
                        << (retries * delay) << "s.");
  cout << "       [ok: " << mgmdConnect << "]" << endl;

  // connect to data nodes (ndbds)
  cout << "waiting for data nodes ..." << flush;
  const int initial_wait = 10;  // seconds to wait until first node detected
  const int final_wait = 0;     // seconds to wait after first node detected
  // returns: 0 all nodes live, > 0 at least one node live, < 0 error
  if (mgmd->wait_until_ready(initial_wait, final_wait) < 0)
    ABORT_ERROR("data nodes were not ready within "
                << (initial_wait + final_wait) << "s.");
  cout << "      [ok]" << endl;

  // connect to database
  cout << "connecting to database ..." << flush;
  ndb = new Ndb(mgmd, catalog.c_str(), schema.c_str());
  // note each scan or index scan operation uses one extra transaction
  if (ndb->init(nMaxConcTx) != 0) ABORT_NDB_ERROR(ndb->getNdbError());
  cout << "      [ok: " << catalog << "." << schema << "]" << endl;

  cout << "caching metadata ..." << flush;
  model = new Model();
  model->init(ndb);
  cout << "            [ok]" << endl;

  // type shortcuts
  typedef CrundDriver::LockMode LockMode;
  cout << "using lock mode for reads ..." << flush;
  string lm;
  switch (driver.lockMode) {
    case LockMode::none:
      ndbOpLockMode = NdbOperation::LM_CommittedRead;
      lm = "LM_CommittedRead";
      break;
    case LockMode::shared:
      ndbOpLockMode = NdbOperation::LM_Read;
      lm = "LM_Read";
      break;
    case LockMode::exclusive:
      ndbOpLockMode = NdbOperation::LM_Exclusive;
      lm = "LM_Exclusive";
      break;
    default:
      ndbOpLockMode = NdbOperation::LM_CommittedRead;
      lm = "LM_CommittedRead";
      assert(false);
  }
  cout << "   [ok: " + lm + "]" << endl;

  initOperations();
}

void NdbapiAB::closeConnection() {
  assert(mgmd != NULL);
  assert(ndb != NULL);
  assert(model != NULL);
  cout << endl << "releasing NDB resources ..." << endl;

  closeOperations();

  cout << "clearing metadata cache ..." << flush;
  delete model;
  model = nullptr;
  cout << "     [ok]" << endl;

  cout << "closing database connection ..." << flush;
  // no ndb->close();
  delete ndb;
  ndb = nullptr;
  cout << " [ok]" << endl;
}

// ----------------------------------------------------------------------
// benchmark operations
// ----------------------------------------------------------------------

void NdbapiAB::initOperations() {
  cout << "initializing operations ..." << flush;
  buildOperations();
  cout << "     [Op: " << operations.size() << "]" << endl;
}

void NdbapiAB::closeOperations() {
  cout << "closing operations ..." << flush;
  for (Operations::iterator i = operations.begin(); i != operations.end(); ++i)
    delete *i;
  operations.clear();
  cout << "          [ok]" << endl;
}

// ----------------------------------------------------------------------
// general benchmark operation types
// ----------------------------------------------------------------------

// type shortcuts
typedef NdbDictionary D;
typedef CrundDriver::XMode XMode;

// current model assumption: relationships only 1:1 identity
// (target id of a navigation operation is verified against source id)
template <XMode::E xMode>
struct NdbapiAB::NdbapiOp : Op {
  NdbapiAB &load;
  NdbTransaction *&tx;  // load's tx pointer

  NdbapiOp(const string &_name, NdbapiAB &_load)
      : Op(_name + "," + XMode::toString(xMode)), load(_load), tx(_load.tx) {}
  ~NdbapiOp() override { assert(tx == NULL); }

  virtual void init() {}
  virtual void close() {}

  void beginTransaction() {
    assert(tx == NULL);
    if ((tx = load.ndb->startTransaction()) == nullptr)
      ABORT_NDB_ERROR(load.ndb->getNdbError());
  }
  void executeOperations() {
    assert(tx != NULL);
    if (tx->execute(NdbTransaction::NoCommit) != 0 ||
        tx->getNdbError().status != NdbError::Success)
      ABORT_NDB_ERROR(tx->getNdbError());
  }
  void commitTransaction() {
    assert(tx != NULL);
    if (tx->execute(NdbTransaction::Commit) != 0 ||
        tx->getNdbError().status != NdbError::Success)
      ABORT_NDB_ERROR(tx->getNdbError());
  }
  void rollbackTransaction() {
    assert(tx != NULL);
    if (tx->execute(NdbTransaction::Rollback) != 0 ||
        tx->getNdbError().status != NdbError::Success)
      ABORT_NDB_ERROR(tx->getNdbError());
  }
  void closeTransaction() {
    assert(tx != NULL);
    load.ndb->closeTransaction(tx);
    tx = nullptr;
  }
};

template <XMode::E xMode>
struct NdbapiAB::WriteOp : NdbapiOp<xMode> {
  typedef NdbapiOp<xMode> super;
  const D::Table *const table;
  NdbOperation *op;

  WriteOp(string _name, NdbapiAB &ab, const D::Table *tab)
      : super(_name, ab), table(tab), op(nullptr) {}

  void run(const Ids &id) override {
    switch (xMode) {
      case XMode::indy:
        for (Ids::const_iterator i = id.begin(); i != id.end(); ++i) {
          super::beginTransaction();
          alloc(1);
          rewind();  // not needed
          write(*i);
          super::commitTransaction();
          free();
          super::closeTransaction();
        }
        break;
      case XMode::each:
        super::beginTransaction();
        alloc(1);
        for (Ids::const_iterator i = id.begin(); i != id.end(); ++i) {
          rewind();
          write(*i);
          super::executeOperations();
        }
        super::commitTransaction();
        free();
        super::closeTransaction();
        break;
      case XMode::bulk:
        super::beginTransaction();
        const int n = id.size();
        alloc(n);
        rewind();
        write(id);
        super::commitTransaction();
        free();
        super::closeTransaction();
        break;
    }
  }

  virtual void write(const Ids &id) {
    for (Ids::const_iterator i = id.begin(); i != id.end(); ++i) write(*i);
  }

  virtual void write(int id) {
    setOp();
    setValues(id);
  }

  virtual void alloc(int n) {}
  virtual void rewind() {}
  virtual void free() {}

  virtual void setOp() = 0;
  virtual void setValues(int id) = 0;
};

template <XMode::E xMode>
struct NdbapiAB::UpdateOp : WriteOp<xMode> {
  typedef WriteOp<xMode> super;

  UpdateOp(string _name, NdbapiAB &ab, const D::Table *tab)
      : super(_name, ab, tab) {}

  void setOp() final {
    super::op = super::tx->getNdbOperation(super::table);
    if (super::op == nullptr) ABORT_NDB_ERROR(super::tx->getNdbError());
    if (super::op->updateTuple() != 0)
      ABORT_NDB_ERROR(super::tx->getNdbError());
  }
};

template <XMode::E xMode>
struct NdbapiAB::InsertOp : WriteOp<xMode> {
  typedef WriteOp<xMode> super;

  InsertOp(string _name, NdbapiAB &ab, const D::Table *tab)
      : super(_name, ab, tab) {}

  void setOp() final {
    super::op = super::tx->getNdbOperation(super::table);
    if (super::op == nullptr) ABORT_NDB_ERROR(super::tx->getNdbError());
    if (super::op->insertTuple() != 0)
      ABORT_NDB_ERROR(super::tx->getNdbError());
  }
};

template <XMode::E xMode>
struct NdbapiAB::DeleteOp : WriteOp<xMode> {
  typedef WriteOp<xMode> super;

  DeleteOp(string nm, NdbapiAB &ab, const D::Table *tab) : super(nm, ab, tab) {}

  void setOp() final {
    super::op = super::tx->getNdbOperation(super::table);
    if (super::op == nullptr) ABORT_NDB_ERROR(super::tx->getNdbError());
    if (super::op->deleteTuple() != 0)
      ABORT_NDB_ERROR(super::tx->getNdbError());
  }
};

template <XMode::E xMode>
struct NdbapiAB::ReadOp : NdbapiOp<xMode> {
  typedef NdbapiOp<xMode> super;
  const D::Table *const table;
  NdbOperation *op;

  ReadOp(string _name, NdbapiAB &ab, const D::Table *tab)
      : super(_name, ab), table(tab), op(nullptr) {}

  void run(const Ids &id) override {
    switch (xMode) {
      case XMode::indy:
        for (Ids::const_iterator i = id.begin(); i != id.end(); ++i) {
          super::beginTransaction();
          alloc(1);
          rewind();  // not needed
          read(*i);
          super::commitTransaction();
          rewind();
          check(*i);
          free();
          super::closeTransaction();
        }
        break;
      case XMode::each:
        super::beginTransaction();
        alloc(1);
        for (Ids::const_iterator i = id.begin(); i != id.end(); ++i) {
          rewind();
          read(*i);
          super::executeOperations();
          rewind();
          check(*i);
        }
        super::commitTransaction();
        free();  // ok outside loop, no cloned RecAttrs
        super::closeTransaction();
        break;
      case XMode::bulk:
        super::beginTransaction();
        const int n = id.size();
        alloc(n);
        rewind();
        read(id);
        super::executeOperations();
        rewind();
        check(id);
        super::commitTransaction();
        free();
        super::closeTransaction();
        break;
    }
  }

  virtual void read(const Ids &id) {
    for (Ids::const_iterator i = id.begin(); i != id.end(); ++i) read(*i);
  }

  virtual void read(int id) {
    setOp();
    getValues(id);
  }

  void setOp() {  // final
    op = super::tx->getNdbOperation(table);
    if (op == nullptr) ABORT_NDB_ERROR(super::tx->getNdbError());
    if (op->readTuple(super::load.ndbOpLockMode) != 0)
      ABORT_NDB_ERROR(super::tx->getNdbError());
  }

  virtual void alloc(int n) = 0;
  virtual void rewind() = 0;
  virtual void free() = 0;

  virtual void getValues(int id) = 0;

  void check(const Ids &id) {  // final
    for (Ids::const_iterator i = id.begin(); i != id.end(); ++i) check(*i);
  }
  virtual void check(int id) = 0;
};

template <XMode::E xMode>
struct NdbapiAB::IndexScanOp : NdbapiOp<xMode> {
  typedef NdbapiOp<xMode> super;
  const D::Index *const index;
  const bool forceSend;  // no send delay for 1-thread app
  vector<NdbIndexScanOperation *> op;

  IndexScanOp(string _name, NdbapiAB &abLoad, const D::Index *idx)
      : super(_name, abLoad), index(idx), forceSend(true), op(0) {}

  void run(const Ids &id) override {
    switch (xMode) {
      case XMode::indy: {
        for (Ids::const_iterator i = id.begin(); i != id.end(); ++i) {
          super::beginTransaction();
          op.assign(1, nullptr);  // space for 1 scan op
          const int o = 0;        // scan op index
          alloc(1);
          rewind();  // not needed
          read(o, *i);
          super::executeOperations();
          rewind();
          fetch(o, *i);
          super::commitTransaction();
          rewind();
          check(*i);
          free();
          super::closeTransaction();
        }
        break;
      }
      case XMode::each: {
        super::beginTransaction();
        op.assign(1, nullptr);  // space for 1 scan op
        const int o = 0;        // scan op index
        for (Ids::const_iterator i = id.begin(); i != id.end(); ++i) {
          alloc(1);
          rewind();
          read(o, *i);
          super::executeOperations();
          rewind();
          fetch(o, *i);
          rewind();
          check(*i);
          free();  // delete cloned RecAttrs within loop
        }
        super::commitTransaction();
        super::closeTransaction();
        break;
      }
      case XMode::bulk: {
        super::beginTransaction();
        const unsigned int bs = super::load.nConcScans;  // batch size
        op.assign(bs, nullptr);                          // scan ops
        const Ids::const_iterator ee = id.end();
        for (Ids::const_iterator b = id.begin(); b < ee; b += bs) {
          const Ids::const_iterator e = (b + bs < ee ? b + bs : ee);
          Ids idb(b, e);
          assert(idb.size() <= bs);
          alloc(bs);
          read(idb);
          super::executeOperations();
          rewind();
          fetch(idb);
          rewind();
          check(idb);
          free();
        }
        super::commitTransaction();
        super::closeTransaction();
        break;
      }
    }
    op.clear();
  }

  void setOp(int o) {  // final
    NdbIndexScanOperation *const iso =
        super::tx->getNdbIndexScanOperation(index);
    if (iso == nullptr) ABORT_NDB_ERROR(super::tx->getNdbError());
    op[o] = iso;

    // define a read scan
    const NdbOperation::LockMode lockMode =
        super::load.ndbOpLockMode;  // NdbOperation::LM_CommittedRead;
    const Uint32 scanFlags = 0 | NdbScanOperation::SF_OrderBy;  // sort on index
    const Uint32 parallel = 0;  // #fragments to scan in parallel (0=max)
    const Uint32 batch = 0;     // #rows to fetch in each batch
    if (iso->readTuples(lockMode, scanFlags, parallel, batch) != 0)
      ABORT_NDB_ERROR(super::tx->getNdbError());
  }

  virtual void read(const Ids &id) {
    const int n = id.size();
    assert(n <= super::load.nConcScans);
    for (int i = 0; i < n; i++) {
      rewind();
      const int o = i;  // scan op index
      read(o, id[i]);
    }
  }
  virtual void read(int o, int id) {
    setOp(o);
    getValues(o, id);
  }

  virtual void fetch(const Ids &id) {
    const int n = id.size();
    assert(n <= super::load.nConcScans);
    for (int i = 0; i < n; i++) {
      const int o = i;  // scan op index
      fetch(o, id[i]);
    }
  }
  virtual void fetch(int o, int id) {
    // read the result set executing the defined read operations
    NdbIndexScanOperation *const iso = op[o];
    int stat;
    const bool allowFetch = true;  // request batches when needed
    while ((stat = iso->nextResult(allowFetch, forceSend)) == 0) copy(o);
    if (stat != 1) ABORT_NDB_ERROR(super::tx->getNdbError());

    // close the scan, no harm in delaying/accumulating close()
    const bool releaseOp = true;
    iso->close(!forceSend, !releaseOp);
  }

  virtual void alloc(int n) = 0;
  virtual void rewind() = 0;
  virtual void copy(int o) = 0;
  virtual void free() = 0;

  virtual void getValues(int o, int id) = 0;  // sets bounds, filter etc

  void check(const Ids &id) {  // final
    // ok to traverse scan results in order and not sort/group them
    // (scan ops are pulled in order, pattern assumes 1:1 relationship)
    for (Ids::const_iterator i = id.begin(); i != id.end(); ++i) check(*i);
  }
  virtual void check(int id) = 0;
};

// a base type for buffer elements if used in index scan operations
struct NdbapiAB::Holder {
  // uniform lifecycle methods for Value- and RecAttr-Holders
  // (ok to be non-virtual since all using code parametrized over type)

  void assignClone(const Holder &that) {  // requires deleteClone()
    assert(false);
  }
  void deleteClone() {  // requires assignClone()
    assert(false);
  }
};

// XXX could refactorize Buffer types...
template <XMode::E xMode, typename ElementT>
struct NdbapiAB::BufUpdateOp : UpdateOp<xMode> {
  typedef UpdateOp<xMode> super;
  const int width;
  ElementT *buf;
  ElementT *pos;

  BufUpdateOp(string _name, NdbapiAB &ab, const D::Table *tab, int w)
      : UpdateOp<xMode>(_name, ab, tab), width(w), buf(nullptr), pos(nullptr) {}

  void alloc(int n) override { pos = buf = new ElementT[n * width]; }

  void rewind() override {
    pos = buf;  // prepare buffer for writing
  }

  void free() override {
    delete[] buf;
    pos = buf = nullptr;
  }
};

// XXX could refactorize Buffer types...
template <XMode::E xMode, typename ElementT>
struct NdbapiAB::BufReadOp : ReadOp<xMode> {
  typedef ReadOp<xMode> super;
  const int width;
  ElementT *buf;
  ElementT *pos;

  BufReadOp(string _name, NdbapiAB &ab, const D::Table *tab, int w)
      : ReadOp<xMode>(_name, ab, tab), width(w), buf(nullptr), pos(nullptr) {}

  void alloc(int n) override { pos = buf = new ElementT[n * width]; }

  void rewind() override {
    pos = buf;  // prepare buffer for reading
  }

  void free() override {
    delete[] buf;
    pos = buf = nullptr;
  }
};

// XXX could refactorize Buffer types...
template <XMode::E xMode, typename ElementT>
struct NdbapiAB::BufIndexScanOp : IndexScanOp<xMode> {
  typedef IndexScanOp<xMode> super;
  typedef typename vector<ElementT>::iterator ElementIterator;

  const int width;
  vector<ElementT> obuf;  // result buffer per scan op, managed by NDBAPI
  vector<ElementT> buf;   // cloned scan result row, content managed by app
  typename vector<ElementT>::iterator pos;  // consumed elements in buf

  BufIndexScanOp(string nm, NdbapiAB &abLoad, const D::Index *idx, int w)
      : IndexScanOp<xMode>(nm, abLoad, idx), width(w) {}

  void alloc(int n) override {
    assert(0 <= n && n <= super::load.nConcScans);
    obuf.assign(n * width, ElementT());
    pos = buf.begin();
  }

  void copy(int o) override {
    // store copy of obuf[o] as overwritten by next scan result
    buf.push_back(ElementT());
    buf.back().assignClone(obuf[o]);
  }

  void rewind() override {
    pos = buf.begin();  // prepare buffer for reading
  }

  void free() override {
    for (ElementIterator i = buf.begin(); i != buf.end(); ++i)
      i->deleteClone();  // clone()'d NdbRecAttrs managed by app
    buf.clear();
    obuf.clear();  // getValue()-returned NdbRecAttrs managed by NDBAPI
  }
};

// ----------------------------------------------------------------------

struct NdbapiAB::TableScanDeleteOp : NdbapiOp<XMode::bulk> {
  typedef NdbapiOp<XMode::bulk> super;
  const D::Table *const table;
  NdbScanOperation *op;
  int nDeleted;

  TableScanDeleteOp(const string &_name, NdbapiAB &ab, const D::Table *tab)
      : super(_name, ab), table(tab), op(nullptr), nDeleted(0) {}

  void run(const Ids &id) override {
    (void)id;  // ignored
    super::beginTransaction();
    setOp();
    super::executeOperations();
    deleteScanned();
    closeOp();
    super::commitTransaction();
    super::closeTransaction();
  }

  void setOp() {  // final
    // get a full table scan operation (no scan filter defined)
    op = tx->getNdbScanOperation(table);
    if (op == nullptr) ABORT_NDB_ERROR(tx->getNdbError());

    // define a read scan with exclusive locks
    const NdbOperation::LockMode lock_mode = NdbOperation::LM_Exclusive;
    const int scan_flags = 0;
    const int parallel = 0;
    const int bulk_ = 0;
    if (op->readTuples(lock_mode, scan_flags, parallel, bulk_) != 0)
      ABORT_NDB_ERROR(tx->getNdbError());
  }

  virtual void deleteScanned() {
    // delete all rows in a given scan
    nDeleted = 0;
    int stat;
    const bool allowFetch = true;  // request new batches when exhausted
    const bool forceSend = false;  // send may be delayed
    while ((stat = op->nextResult(allowFetch, forceSend)) == 0) {
      // delete all tuples within a batch
      do {
        if (op->deleteCurrentTuple() != 0) ABORT_NDB_ERROR(tx->getNdbError());
        nDeleted++;
      } while ((stat = op->nextResult(!allowFetch, forceSend)) == 0);

      switch (stat) {
        case 1:  // no more batches
          return;
        case 2:  // end of current batch, fetch next
          if (tx->execute(NdbTransaction::NoCommit) != 0 ||
              tx->getNdbError().status != NdbError::Success)
            ABORT_NDB_ERROR(tx->getNdbError());
          continue;
        default:
          ABORT_ERROR("unknown nextResult() status = " + toString(stat));
      }
    }
  }

  void closeOp() {  // final
    // close the scan
    const bool forceSend_ = false;
    const bool releaseOp = false;
    op->close(forceSend_, releaseOp);
    op = nullptr;
  }
};

// ----------------------------------------------------------------------
// crund benchmark operation types
// ----------------------------------------------------------------------

struct NdbapiAB::ValIdHolder : Holder {
  Int32 id;

  Int32 getId() { return id; }  // uniform Id access
  void assignClone(const ValIdHolder &that) { *this = that; }
  void deleteClone() {}  // nothing cloned, just copied
};

struct NdbapiAB::ValAttrHolder : ValIdHolder {
  Int32 cint;
  Int64 clong;
  float cfloat;
  double cdouble;

  void assignClone(const ValAttrHolder &that) { *this = that; }
};

struct NdbapiAB::RecIdHolder : Holder {
  NdbRecAttr *id;

  RecIdHolder() : id(nullptr) {}
  Int32 getId() {  // uniform Id access
    return id->int32_value();
  }
  void assignClone(const RecIdHolder &that) {  // requires deleteClone()
    assert(id == NULL);
    id = that.id->clone();
  }
  void deleteClone() {  // requires assignClone()
    delete id;
    id = nullptr;
  }
};

struct NdbapiAB::RecAttrHolder : RecIdHolder {
  NdbRecAttr *cint;
  NdbRecAttr *clong;
  NdbRecAttr *cfloat;
  NdbRecAttr *cdouble;

  RecAttrHolder()
      : cint(nullptr), clong(nullptr), cfloat(nullptr), cdouble(nullptr) {}
  void assignClone(const RecAttrHolder &that) {  // requires deleteClone()
    RecIdHolder::assignClone(that);
    assert(cint == NULL);
    assert(clong == NULL);
    assert(cfloat == NULL);
    assert(cdouble == NULL);
    cint = that.cint->clone();
    clong = that.clong->clone();
    cfloat = that.cfloat->clone();
    cdouble = that.cdouble->clone();
  }
  void deleteClone() {  // requires assignClone()
    delete cint;
    delete clong;
    delete cfloat;
    delete cdouble;
    cint = nullptr;
    clong = nullptr;
    cfloat = nullptr;
    cdouble = nullptr;
    RecIdHolder::deleteClone();
  }
};

template <XMode::E xMode, bool setAttr>
struct NdbapiAB::AB_insAttr : InsertOp<xMode> {
  typedef InsertOp<xMode> super;
  AB_insAttr(string nm, NdbapiAB &ab, const D::Table *tab)
      : super(nm + (setAttr ? "Attr" : ""), ab, tab) {}
  void setValues(int id) override {
    super::load.setKeyAB(super::op, id);  // set first
    if (setAttr) super::load.setAttrAB(super::op, -id);
  }
};

template <XMode::E xMode, bool setAttr>
struct NdbapiAB::A_insAttr : AB_insAttr<xMode, setAttr> {
  typedef AB_insAttr<xMode, setAttr> super;
  A_insAttr(NdbapiAB &ab) : super(string("A_ins"), ab, ab.model->table_A) {}
};

template <XMode::E xMode, bool setAttr>
struct NdbapiAB::B_insAttr : AB_insAttr<xMode, setAttr> {
  typedef AB_insAttr<xMode, setAttr> super;
  B_insAttr(NdbapiAB &ab) : super(string("B_ins"), ab, ab.model->table_B) {}
};

template <XMode::E xMode>
struct NdbapiAB::AB_setAttr : UpdateOp<xMode> {
  typedef UpdateOp<xMode> super;
  AB_setAttr(string _name, NdbapiAB &ab, const D::Table *tab)
      : super(_name, ab, tab) {}
  void setValues(int id) override {
    super::load.setKeyAB(super::op, id);  // set first
    super::load.setAttrAB(super::op, id);
  }
};

template <XMode::E xMode>
struct NdbapiAB::A_setAttr : AB_setAttr<xMode> {
  typedef AB_setAttr<xMode> super;
  A_setAttr(NdbapiAB &ab) : super(string("A_setAttr"), ab, ab.model->table_A) {}
};

template <XMode::E xMode>
struct NdbapiAB::B_setAttr : AB_setAttr<xMode> {
  typedef AB_setAttr<xMode> super;
  B_setAttr(NdbapiAB &ab) : super(string("B_setAttr"), ab, ab.model->table_B) {}
};

template <XMode::E xMode>
struct NdbapiAB::AB_del : DeleteOp<xMode> {
  typedef DeleteOp<xMode> super;
  AB_del(string _name, NdbapiAB &ab, const D::Table *_table)
      : super(_name, ab, _table) {}
  void setValues(int id) override { super::load.setKeyAB(super::op, id); }
};

template <XMode::E xMode>
struct NdbapiAB::A_del : AB_del<xMode> {
  typedef AB_del<xMode> super;
  A_del(NdbapiAB &ab) : super("A_del", ab, ab.model->table_A) {}
};

template <XMode::E xMode>
struct NdbapiAB::B_del : AB_del<xMode> {
  typedef AB_del<xMode> super;
  B_del(NdbapiAB &ab) : super("B_del", ab, ab.model->table_B) {}
};

template <XMode::E xMode, typename HolderT>
struct NdbapiAB::AB_getAttr : BufReadOp<xMode, HolderT> {
  typedef BufReadOp<xMode, HolderT> super;
  AB_getAttr(string _name, NdbapiAB &ab, const D::Table *tab)
      : super(_name, ab, tab, 1) {}
  void getValues(int id) override {
    super::load.setKeyAB(super::op, id);  // set first
    super::load.getKeyAB(super::op, super::pos);
    super::load.getAttrAB(super::op, super::pos);
    super::pos++;  // consumed
  }
  void check(int id) override {
    super::load.checkKeyAB(id, super::pos);
    super::load.checkAttrAB(id, super::pos);
    super::pos++;  // consumed
  }
};

template <XMode::E xMode>
struct NdbapiAB::A_getAttr_bb : AB_getAttr<xMode, ValAttrHolder> {
  typedef AB_getAttr<xMode, ValAttrHolder> super;
  A_getAttr_bb(NdbapiAB &ab) : super("A_getAttr_bb", ab, ab.model->table_A) {}
};

template <XMode::E xMode>
struct NdbapiAB::A_getAttr_ra : AB_getAttr<xMode, RecAttrHolder> {
  typedef AB_getAttr<xMode, RecAttrHolder> super;
  A_getAttr_ra(NdbapiAB &ab) : super("A_getAttr_ra", ab, ab.model->table_A) {}
};

template <XMode::E xMode>
struct NdbapiAB::B_getAttr_bb : AB_getAttr<xMode, ValAttrHolder> {
  typedef AB_getAttr<xMode, ValAttrHolder> super;
  B_getAttr_bb(NdbapiAB &ab) : super("B_getAttr_bb", ab, ab.model->table_B) {}
};

template <XMode::E xMode>
struct NdbapiAB::B_getAttr_ra : AB_getAttr<xMode, RecAttrHolder> {
  typedef AB_getAttr<xMode, RecAttrHolder> super;
  B_getAttr_ra(NdbapiAB &ab) : super("B_getAttr_ra", ab, ab.model->table_B) {}
};

template <XMode::E xMode>
struct NdbapiAB::B_setVarbinary : BufUpdateOp<xMode, char> {
  typedef BufUpdateOp<xMode, char> super;
  const bytes buf;  // for robustness use a copy of bytes at construction
  B_setVarbinary(NdbapiAB &ab, const bytes &_buf)
      : super(string("B_setVarbin_") + toString(_buf.size()), ab,
              ab.model->table_B, ab.model->width_B_cvarbinary_def),
        buf(_buf) {}
  void setValues(int id) override {
    super::load.setKeyAB(super::op, id);  // set first
    super::load.setVarbinaryB(super::op, super::pos, &buf);
  }
};

template <XMode::E xMode>
struct NdbapiAB::B_clearVarbinary : BufUpdateOp<xMode, char> {
  typedef BufUpdateOp<xMode, char> super;
  B_clearVarbinary(NdbapiAB &ab, const bytes &str)
      : super(string("B_clearVarbin_") + toString(str.size()), ab,
              ab.model->table_B, ab.model->width_B_cvarbinary_def) {}
  void setValues(int id) override {
    super::load.setKeyAB(super::op, id);  // set first
    super::load.setVarbinaryB(super::op, super::pos, nullptr);
  }
};

template <XMode::E xMode>
struct NdbapiAB::B_getVarbinary : BufReadOp<xMode, char> {
  typedef BufReadOp<xMode, char> super;
  const bytes buf;  // for robustness use a copy of bytes at construction
  B_getVarbinary(NdbapiAB &ab, const bytes &_buf)
      : super(string("B_getVarbin_") + toString(_buf.size()), ab,
              ab.model->table_B, ab.model->width_B_cvarbinary_def),
        buf(_buf) {}
  void getValues(int id) override {
    super::load.setKeyAB(super::op, id);  // set first
    super::load.getVarbinaryB(super::op, super::pos);
    super::pos += super::width;  // consumed
  }
  void check(int id) override {
    super::load.checkVarbinaryB(&buf, super::pos);
    super::pos += super::width;  // consumed
  }
};

template <XMode::E xMode>
struct NdbapiAB::B_setVarchar : BufUpdateOp<xMode, char> {
  typedef BufUpdateOp<xMode, char> super;
  const string str;  // for robustness use a copy of string at construction
  B_setVarchar(NdbapiAB &ab, const string &str)
      : super(string("B_setVarchar_") + toString(str.size()), ab,
              ab.model->table_B, ab.model->width_B_cvarchar_def),
        str(str) {}
  void setValues(int id) override {
    super::load.setKeyAB(super::op, id);  // set first
    super::load.setVarcharB(super::op, super::pos, &str);
  }
};

template <XMode::E xMode>
struct NdbapiAB::B_clearVarchar : BufUpdateOp<xMode, char> {
  typedef BufUpdateOp<xMode, char> super;
  B_clearVarchar(NdbapiAB &ab, const string &str)
      : super(string("B_clearVarchar_") + toString(str.size()), ab,
              ab.model->table_B, ab.model->width_B_cvarchar_def) {}
  void setValues(int id) override {
    super::load.setKeyAB(super::op, id);  // set first
    super::load.setVarcharB(super::op, super::pos, nullptr);
  }
};

template <XMode::E xMode>
struct NdbapiAB::B_getVarchar : BufReadOp<xMode, char> {
  typedef BufReadOp<xMode, char> super;
  const string str;  // for robustness use a copy of string at construction
  B_getVarchar(NdbapiAB &ab, const string &str)
      : super(string("B_getVarchar_") + toString(str.size()), ab,
              ab.model->table_B, ab.model->width_B_cvarchar_def),
        str(str) {}
  void getValues(int id) override {
    super::load.setKeyAB(super::op, id);  // set first
    super::load.getVarcharB(super::op, super::pos);
    super::pos += super::width;  // consumed
  }
  void check(int id) override {
    super::load.checkVarcharB(&str, super::pos);
    super::pos += super::width;  // consumed
  }
};

template <XMode::E xMode>
struct NdbapiAB::B_setA : UpdateOp<xMode> {
  typedef UpdateOp<xMode> super;
  B_setA(NdbapiAB &ab) : super(string("B_setA"), ab, ab.model->table_B) {}
  void setValues(int id) override {
    super::load.setKeyAB(super::op, id);  // set first
    const int aid = id;
    super::load.setAIdB(super::op, aid);
  }
};

template <XMode::E xMode, typename HolderT>
struct NdbapiAB::B_getA : BufReadOp<xMode, HolderT> {
  typedef BufReadOp<xMode, HolderT> super;

  // sub-query fetching B.a_id
  struct B_getAId : BufReadOp<xMode, HolderT> {
    typedef BufReadOp<xMode, HolderT> super;
    B_getAId(NdbapiAB &ab)
        : super(string("B_getAId"), ab, ab.model->table_B, 1) {}
    void getValues(int id) override {
      super::load.setKeyAB(super::op, id);  // set first
      super::load.getAIdB(super::op, super::pos);
      super::pos++;  // consumed
    }
    void check(int id) override {}  // results traversed within outer query
  };
  B_getAId getAId;

  B_getA(string _name, NdbapiAB &ab, const D::Table *tab)
      : super(_name, ab, tab, 1), getAId(ab) {}

  void alloc(int n) override {
    super::alloc(n);
    getAId.alloc(n);
  }
  void free() override {
    getAId.free();
    super::free();
  }

  void read(const Ids &id) override {
    // run sub-query
    getAId.rewind();
    getAId.read(id);
    super::executeOperations();

    // run this query
    // cannot call into super.read(int[]) -> this.read(int)
    getAId.rewind();
    const int n = id.size();
    for (int i = 0; i < n; i++) {
      const int aid = getAId.pos->getId();
      getAId.pos++;  // consumed
      super::read(aid);
    }
  }
  void read(int id) override {
    // run sub-query
    getAId.rewind();
    getAId.read(id);
    super::executeOperations();

    // run this query
    getAId.rewind();
    const int aid = getAId.pos->getId();
    getAId.pos++;  // consumed
    super::read(aid);
  }

  // same as in AB_getAttr...
  void getValues(int id) override {
    super::load.setKeyAB(super::op, id);  // set first
    super::load.getKeyAB(super::op, super::pos);
    super::load.getAttrAB(super::op, super::pos);
    super::pos++;  // consumed
  }
  void check(int id) override {
    super::load.checkKeyAB(id, super::pos);
    super::load.checkAttrAB(id, super::pos);
    super::pos++;  // consumed
  }
};

template <XMode::E xMode>
struct NdbapiAB::B_getA_bb : B_getA<xMode, ValAttrHolder> {
  typedef B_getA<xMode, ValAttrHolder> super;
  B_getA_bb(NdbapiAB &ab) : super("B_getA_bb", ab, ab.model->table_A) {}
};

template <XMode::E xMode>
struct NdbapiAB::B_getA_ra : B_getA<xMode, RecAttrHolder> {
  typedef B_getA<xMode, RecAttrHolder> super;
  B_getA_ra(NdbapiAB &ab) : super("B_getA_ra", ab, ab.model->table_A) {}
};

template <XMode::E xMode, typename HolderT>
struct NdbapiAB::A_getBs : BufIndexScanOp<xMode, HolderT> {
  typedef BufIndexScanOp<xMode, HolderT> super;
  A_getBs(string _name, NdbapiAB &ab, const D::Index *_index)
      : super(_name, ab, _index, 1) {}
  void getValues(int o, int id) override {
    super::load.setBoundEqAIdB(super::op[o], id);
    super::load.getKeyAB(super::op[o], &super::obuf[o]);
    super::load.getAttrAB(super::op[o], &super::obuf[o]);
  }
  void check(int id) override {
    super::load.checkKeyAB(id, &(*super::pos));   // deref needed for type
    super::load.checkAttrAB(id, &(*super::pos));  // deref needed for type
    super::pos++;                                 // consumed
  }
};

template <XMode::E xMode>
struct NdbapiAB::A_getBs_bb : A_getBs<xMode, ValAttrHolder> {
  typedef A_getBs<xMode, ValAttrHolder> super;
  A_getBs_bb(NdbapiAB &ab) : super("A_getBs_bb", ab, ab.model->idx_B_aid) {}
};

template <XMode::E xMode>
struct NdbapiAB::A_getBs_ra : A_getBs<xMode, RecAttrHolder> {
  typedef A_getBs<xMode, RecAttrHolder> super;
  A_getBs_ra(NdbapiAB &ab) : super("A_getBs_ra", ab, ab.model->idx_B_aid) {}
};

struct NdbapiAB::A_delAll : TableScanDeleteOp {
  A_delAll(NdbapiAB &ab)
      : TableScanDeleteOp("A_delAll", ab, ab.model->table_A) {}
};

struct NdbapiAB::B_delAll : TableScanDeleteOp {
  B_delAll(NdbapiAB &ab)
      : TableScanDeleteOp("B_delAll", ab, ab.model->table_B) {}
};

template <XMode::E xMode>
void NdbapiAB::addOperations() {
  const bool setAttr = true;
  operations.push_back(new A_insAttr<xMode, setAttr>(*this));
  operations.push_back(new B_insAttr<xMode, setAttr>(*this));
  operations.push_back(new A_setAttr<xMode>(*this));
  operations.push_back(new B_setAttr<xMode>(*this));
  operations.push_back(new A_getAttr_bb<xMode>(*this));
  operations.push_back(new A_getAttr_ra<xMode>(*this));
  operations.push_back(new B_getAttr_bb<xMode>(*this));
  operations.push_back(new B_getAttr_ra<xMode>(*this));

  for (vector<bytes>::const_iterator i = bdata->begin(); i != bdata->end();
       ++i) {
    const bytes &data = *i;
    const int l = data.size();
    if (l > driver.maxVarbinaryBytes) break;

    const int w =
        model->width_B_cvarbinary_def - model->wprefix_B_cvarbinary_def;
    if (l > w) {
      ostringstream msg;
      msg << "skipping varbinary operations of length: " << l
          << ", property maxVarbinaryBinarys > |B.cvarbinary| = " << w;
      driver.logWarning(name, msg.str());
      break;
    }

    operations.push_back(new B_setVarbinary<xMode>(*this, data));
    operations.push_back(new B_getVarbinary<xMode>(*this, data));
    operations.push_back(new B_clearVarbinary<xMode>(*this, data));
  }

  for (vector<string>::const_iterator i = sdata->begin(); i != sdata->end();
       ++i) {
    const string &data = *i;
    const int l = data.size();
    if (l > driver.maxVarcharChars) break;

    const int w = model->width_B_cvarchar_def - model->wprefix_B_cvarchar_def;
    if (l > w) {
      ostringstream msg;
      msg << "skipping varchar operations of length: " << l
          << ", property maxVarcharChars > |B.cvarchar| = " << w;
      driver.logWarning(name, msg.str());
      break;
    }

    operations.push_back(new B_setVarchar<xMode>(*this, data));
    operations.push_back(new B_getVarchar<xMode>(*this, data));
    operations.push_back(new B_clearVarchar<xMode>(*this, data));
  }

  operations.push_back(new B_setA<xMode>(*this));
  operations.push_back(new B_getA_bb<xMode>(*this));
  operations.push_back(new B_getA_ra<xMode>(*this));
  operations.push_back(new A_getBs_bb<xMode>(*this));
  operations.push_back(new A_getBs_ra<xMode>(*this));

  operations.push_back(new B_del<xMode>(*this));
  operations.push_back(new A_del<xMode>(*this));
  operations.push_back(new A_insAttr<xMode, !setAttr>(*this));
  operations.push_back(new B_insAttr<xMode, !setAttr>(*this));
  operations.push_back(new B_delAll(*this));
  operations.push_back(new A_delAll(*this));
}

void NdbapiAB::buildOperations() {
  for (vector<XMode::E>::const_iterator i = driver.xModes.begin();
       i != driver.xModes.end(); ++i) {
    switch (*i) {
      case XMode::indy:
        addOperations<XMode::indy>();
        break;
      case XMode::each:
        addOperations<XMode::each>();
        break;
      case XMode::bulk:
        addOperations<XMode::bulk>();
        break;
      default:
        assert(false);
    }
  }
}

// ----------------------------------------------------------------------

void NdbapiAB::setKeyAB(NdbOperation *op, int id) {
  if (op->equal(model->attr_id, (Int32)id) != 0)
    ABORT_NDB_ERROR(tx->getNdbError());
}

void NdbapiAB::getKeyAB(NdbOperation *op, ValIdHolder *vh) {
  if (op->getValue(model->attr_id, (char *)&vh->id) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
}

void NdbapiAB::getKeyAB(NdbOperation *op, RecIdHolder *rh) {
  if ((rh->id = op->getValue(model->attr_id, nullptr)) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
}

void NdbapiAB::checkKeyAB(int i, ValIdHolder *vh) { verify(i, vh->id); }

void NdbapiAB::checkKeyAB(int i, RecIdHolder *rh) {
  verify(i, rh->id->int32_value());
}

// ----------------------------------------------------------------------

void NdbapiAB::setAttrAB(NdbOperation *op, int i) {
  if (op->setValue(model->attr_cint, (Int32)i) != 0)
    ABORT_NDB_ERROR(tx->getNdbError());
  if (op->setValue(model->attr_clong, (Int64)i) != 0)
    ABORT_NDB_ERROR(tx->getNdbError());
  if (op->setValue(model->attr_cfloat, (float)i) != 0)
    ABORT_NDB_ERROR(tx->getNdbError());
  if (op->setValue(model->attr_cdouble, (double)i) != 0)
    ABORT_NDB_ERROR(tx->getNdbError());
}

void NdbapiAB::getAttrAB(NdbOperation *op, ValAttrHolder *vh) {
  if (op->getValue(model->attr_cint, (char *)&vh->cint) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
  if (op->getValue(model->attr_clong, (char *)&vh->clong) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
  if (op->getValue(model->attr_cfloat, (char *)&vh->cfloat) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
  if (op->getValue(model->attr_cdouble, (char *)&vh->cdouble) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
}

void NdbapiAB::getAttrAB(NdbOperation *op, RecAttrHolder *rh) {
  if ((rh->cint = op->getValue(model->attr_cint, nullptr)) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
  if ((rh->clong = op->getValue(model->attr_clong, nullptr)) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
  if ((rh->cfloat = op->getValue(model->attr_cfloat, nullptr)) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
  if ((rh->cdouble = op->getValue(model->attr_cdouble, nullptr)) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
}

void NdbapiAB::checkAttrAB(int i, ValAttrHolder *vh) {
  verify(i, vh->cint);
  verify(i, vh->clong);
  verify(i, vh->cfloat);
  verify(i, vh->cdouble);
}

void NdbapiAB::checkAttrAB(int i, RecAttrHolder *rh) {
  verify(i, rh->cint->int32_value());
  verify(i, rh->clong->int64_value());
  verify(i, rh->cfloat->float_value());
  verify(i, rh->cdouble->double_value());
}

// ----------------------------------------------------------------------

void NdbapiAB::setVarbinaryB(NdbOperation *op, char *&pos, const bytes *data) {
  char *to = nullptr;
  if (data != nullptr) {
    const int lpw = model->wprefix_B_cvarbinary_def;
    to = writeBytes(pos, *data, lpw);
  }
  if (op->setValue(model->attr_B_cvarbinary_def, to) != 0)
    ABORT_NDB_ERROR(tx->getNdbError());
}

void NdbapiAB::getVarbinaryB(NdbOperation *op, char *pos) {
  if (op->getValue(model->attr_B_cvarbinary_def, pos) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
}

void NdbapiAB::checkVarbinaryB(const bytes *data, char *pos) {
  if (data != nullptr) {
    const int w = model->width_B_cvarbinary_def;
    const int lpw = model->wprefix_B_cvarbinary_def;
    bytes to;
    to.reserve(w);
    readBytes(to, pos, lpw);
    verify(*data, to);
  }
}

void NdbapiAB::setVarcharB(NdbOperation *op, char *&pos, const string *data) {
  char *to = nullptr;
  if (data != nullptr) {
    const int lpw = model->wprefix_B_cvarchar_def;
    to = writeString(pos, *data, lpw);
  }
  if (op->setValue(model->attr_B_cvarchar_def, to) != 0)
    ABORT_NDB_ERROR(tx->getNdbError());
}

void NdbapiAB::getVarcharB(NdbOperation *op, char *pos) {
  if (op->getValue(model->attr_B_cvarchar_def, pos) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
}

void NdbapiAB::checkVarcharB(const string *data, char *pos) {
  if (data != nullptr) {
    const int w = model->width_B_cvarchar_def;
    const int lpw = model->wprefix_B_cvarchar_def;
    string to;
    to.reserve(w);
    readString(to, pos, lpw);
    verify(*data, to);
  }
}

// ----------------------------------------------------------------------

void NdbapiAB::setAIdB(NdbOperation *op, int aid) {
  if (op->setValue(model->attr_B_aid, aid) != 0)
    ABORT_NDB_ERROR(tx->getNdbError());
}

void NdbapiAB::getAIdB(NdbOperation *op, ValIdHolder *vh) {
  if (op->getValue(model->attr_B_aid, (char *)&vh->id) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
}

void NdbapiAB::getAIdB(NdbOperation *op, RecIdHolder *rh) {
  if ((rh->id = op->getValue(model->attr_B_aid, nullptr)) == nullptr)
    ABORT_NDB_ERROR(tx->getNdbError());
}

void NdbapiAB::setBoundEqAIdB(NdbIndexScanOperation *op, int id) {
  // define the scan's bounds (more efficient than using a scan filter)
  // the argument to setBound() is not the column's attribute id
  //    if (op->setBound(model->attr_B_a_id, ...
  // or column name
  //    if (op->setBound("a_id", ...
  // but the attribute id of the column in the index:
  //    if (op->setBound(idx_B_a_id->getColumn(0)->getAttrId()...
  if (op->setBound(model->attr_idx_B_aid, NdbIndexScanOperation::BoundEQ,
                   &id) != 0)
    ABORT_NDB_ERROR(tx->getNdbError());
}

// ----------------------------------------------------------------------

void NdbapiAB::writeLengthPrefix(char *&to, int length, int width) {
  assert(1 <= width && width <= 2);
  const unsigned int p = width;
  assert(0 <= length);
  const unsigned int l = length;
  assert(l < 1u << (p * 8));
  for (unsigned int i = 0; i < p; i++) *to++ = (char)(l >> (i * 8) & 0xff);
}

int NdbapiAB::readLengthPrefix(const char *&from, int width) {
  assert(1 <= width && width <= 2);
  const unsigned int p = width;
  unsigned int l = 0;
  for (unsigned int i = 0; i < p; i++)
    l |= (unsigned int)((*from++) << (i * 8));
  assert(l < 1u << (width * 8));
  return l;
}

char *NdbapiAB::writeBytes(char *&to, const bytes &from, int width) {
  char *const dst = to;

  const int length = from.size();
  writeLengthPrefix(to, length, width);

  // copy data
  for (bytes::const_iterator i = from.begin(); i != from.end(); ++i) *to++ = *i;
  return dst;
}

char *NdbapiAB::writeString(char *&to, const string &from, int width) {
  char *const dst = to;

  const int length = from.size();
  writeLengthPrefix(to, length, width);

  // copy data, no decoding
  memcpy(to, from.c_str(), length);
  to += length;
  return dst;
}

void NdbapiAB::readBytes(bytes &to, const char *from, int width) {
  const int length = readLengthPrefix(from, width);

  // copy data
  to.reserve(to.size() + length);
  for (int i = 0; i < length; ++i) to.push_back(*from++);
}

void NdbapiAB::readString(string &to, const char *from, int width) {
  const int length = readLengthPrefix(from, width);

  // copy data, no encoding
  to.append(from, length);
}

// ----------------------------------------------------------------------

void NdbapiAB::clearData() {
  cout << "deleting all rows ..." << flush;
  B_delAll b(*this);
  b.run(Ids());
  int delB = b.nDeleted;
  cout << "           [B: " << delB << flush;
  A_delAll a(*this);
  a.run(Ids());
  int delA = a.nDeleted;
  cout << ", A: " << delA << "]" << endl;
}

// ----------------------------------------------------------------------
