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

#include "Suma.hpp"
#include "util/require.h"

#include <ndb_version.h>

#include <NdbTCP.h>
#include <Bitmask.hpp>
#include <SimpleProperties.hpp>

#include <signaldata/NodeFailRep.hpp>
#include <signaldata/ReadNodesConf.hpp>

#include <signaldata/AlterTab.hpp>
#include <signaldata/AlterTable.hpp>
#include <signaldata/CheckNodeGroups.hpp>
#include <signaldata/CreateTab.hpp>
#include <signaldata/CreateTrigImpl.hpp>
#include <signaldata/DiGetNodes.hpp>
#include <signaldata/DictTabInfo.hpp>
#include <signaldata/DihScanTab.hpp>
#include <signaldata/DropTab.hpp>
#include <signaldata/DropTrigImpl.hpp>
#include <signaldata/FireTrigOrd.hpp>
#include <signaldata/GCP.hpp>
#include <signaldata/GetTabInfo.hpp>
#include <signaldata/ListTables.hpp>
#include <signaldata/ScanFrag.hpp>
#include <signaldata/StopMe.hpp>
#include <signaldata/SumaImpl.hpp>
#include <signaldata/SystemError.hpp>
#include <signaldata/TransIdAI.hpp>
#include <signaldata/TrigAttrInfo.hpp>

#include <ndbapi/NdbDictionary.hpp>
#include <signaldata/DictLock.hpp>

#include <DebuggerNames.hpp>
#include "../dbdih/Dbdih.hpp"
#include "../dbtup/Dbtup.hpp"

#include <signaldata/CreateNodegroup.hpp>
#include <signaldata/CreateNodegroupImpl.hpp>

#include <signaldata/DropNodegroup.hpp>
#include <signaldata/DropNodegroupImpl.hpp>

#include <signaldata/DbinfoScan.hpp>
#include <signaldata/DumpStateOrd.hpp>
#include <signaldata/TransIdAI.hpp>

#include <ndb_version.h>
#include <EventLogger.hpp>

#define JAM_FILE_ID 467

// #define HANDOVER_DEBUG
// #define NODEFAIL_DEBUG
// #define NODEFAIL_DEBUG2
// #define DEBUG_SUMA_SEQUENCE
// #define EVENT_DEBUG
// #define EVENT_PH3_DEBUG
// #define EVENT_DEBUG2
#if 1
#undef DBUG_ENTER
#undef DBUG_PRINT
#undef DBUG_RETURN
#undef DBUG_VOID_RETURN

#if 0
#define DBUG_ENTER(a) \
  { g_eventLogger->info("%s:%d >%s", __FILE__, __LINE__, a); }
#define DBUG_PRINT(a, b)                                      \
  {                                                           \
    g_eventLogger->info("%s:%d %s: ", __FILE__, __LINE__, a); \
    g_eventLogger->info b;                                    \
  }
#define DBUG_RETURN(a)                                  \
  {                                                     \
    g_eventLogger->info("%s:%d <", __FILE__, __LINE__); \
    return (a);                                         \
  }
#define DBUG_VOID_RETURN                                \
  {                                                     \
    g_eventLogger->info("%s:%d <", __FILE__, __LINE__); \
    return;                                             \
  }
#else
#define DBUG_ENTER(a)
#define DBUG_PRINT(a, b)
#define DBUG_RETURN(a) return a
#define DBUG_VOID_RETURN return
#endif

#endif

#define DBG_3R 0

/**
 * @todo:
 * SUMA crashes if an index is created at the same time as
 * global replication. Very easy to reproduce using testIndex.
 * Note: This only happens occasionally, but is quite easy to reprod.
 */

Uint32 g_subPtrI = RNIL;
static const Uint32 SUMA_SEQUENCE = 0xBABEBABE;

static const Uint32 MAX_CONCURRENT_GCP = 2;

static constexpr Uint32 REPORT_SUBSCRIPTION_INTERVAL = 16;

static bool g_reporting_in_progress = false;
/**************************************************************
 *
 * Start of suma
 *
 */

#define PRINT_ONLY 0

void Suma::execREAD_CONFIG_REQ(Signal *signal) {
  jamEntry();

  const ReadConfigReq *req = (ReadConfigReq *)signal->getDataPtr();

  Uint32 ref = req->senderRef;
  Uint32 senderData = req->senderData;

  const ndb_mgm_configuration_iterator *p =
      m_ctx.m_config.getOwnConfigIterator();
  ndbrequire(p != 0);

  // SumaParticipant
  Uint32 noTables, noAttrs, maxBufferedEpochs;
  ndb_mgm_get_int_parameter(p, CFG_DICT_TABLE, &noTables);
  ndb_mgm_get_int_parameter(p, CFG_DICT_ATTRIBUTE, &noAttrs);
  ndb_mgm_get_int_parameter(p, CFG_DB_MAX_BUFFERED_EPOCHS, &maxBufferedEpochs);

  c_tablePool.setSize(noTables);
  c_tables.setSize(noTables);

  c_subscriptions.setSize(noTables);

  Uint32 cnt = 0;
  ndb_mgm_get_int_parameter(p, CFG_DB_SUBSCRIPTIONS, &cnt);
  if (cnt == 0) {
    jam();
    cnt = noTables;
  }
  c_subscriptionPool.setSize(cnt);

  cnt *= 2;
  {
    Uint32 val = 0;
    ndb_mgm_get_int_parameter(p, CFG_DB_SUBSCRIBERS, &val);
    if (val) {
      jam();
      cnt = val;
    } else {
      // Autosize: Add two subscribers for each API node (since each
      // MySQL Server uses two for detecting schema changes)
      Uint32 num_api_nodes = 0;
      ndb_mgm_configuration_iterator *iter =
          m_ctx.m_config.getClusterConfigIterator();
      for (ndb_mgm_first(iter); ndb_mgm_valid(iter); ndb_mgm_next(iter)) {
        Uint32 node_type;
        ndbrequire(
            !ndb_mgm_get_int_parameter(iter, CFG_TYPE_OF_SECTION, &node_type));
        if (node_type == NODE_TYPE_API) {
          num_api_nodes++;
        }
      }
      cnt += 2 * num_api_nodes;
    }
  }
  c_subscriberPool.setSize(cnt);

  cnt = 0;
  ndb_mgm_get_int_parameter(p, CFG_DB_SUB_OPERATIONS, &cnt);
  if (cnt)
    c_subOpPool.setSize(cnt);
  else
    c_subOpPool.setSize(256);

  c_syncPool.setSize(2);

  // Trix: max 5 concurrent index stats ops with max 9 words bounds
  Uint32 noOfBoundWords = 5 * 9;

  // XXX multiplies number of words by 15 ???
  c_dataBufferPool.setSize(noAttrs + noOfBoundWords);

  c_maxBufferedEpochs = maxBufferedEpochs;
  infoEvent("Buffering maximum epochs %u", c_maxBufferedEpochs);

  // Need GCP records for every non-fully-acknowledged
  // GCP from the API nodes
  // GCPs may be un-acked due to API node failure, or
  // overload
  // Users specify a maxBufferedEpochs value which we will use
  // as a disconnect threshold
  // Disconnect takes non-zero time, so we should allow
  // some extra records to absorb this time
  // If epoch buffering is a tighter bound than API heartbeats
  // then API heartbeat failure may be handled and logged as
  // a problem with max buffered epochs.
  // That should be ok.
  //
  Uint32 gcpInterval, microGcpInterval = 0;
  ndb_mgm_get_int_parameter(p, CFG_DB_GCP_INTERVAL, &gcpInterval);
  ndb_mgm_get_int_parameter(p, CFG_DB_MICRO_GCP_INTERVAL, &microGcpInterval);

  if (microGcpInterval) {
    gcpInterval = microGcpInterval;
  }

  const Uint32 disconnectBufferSeconds = 5;
  const Uint32 disconnectBufferEpochs =
      ((disconnectBufferSeconds * 1000) + gcpInterval - 1) / gcpInterval;

  const Uint32 poolSize = disconnectBufferEpochs + c_maxBufferedEpochs;

  c_gcp_pool.setSize(poolSize);

  Uint32 maxBufferedEpochBytes, numPages, numPageChunks;
  ndb_mgm_get_int_parameter(p, CFG_DB_MAX_BUFFERED_EPOCH_BYTES,
                            &maxBufferedEpochBytes);
  numPages = (maxBufferedEpochBytes + Page_chunk::CHUNK_PAGE_SIZE - 1) /
             Page_chunk::CHUNK_PAGE_SIZE;
  numPageChunks = (numPages + Page_chunk::PAGES_PER_CHUNK - 1) /
                  Page_chunk::PAGES_PER_CHUNK;
  c_page_chunk_pool.setSize(numPageChunks);

  {
    SyncRecord_sllist tmp(c_syncPool);
    Ptr<SyncRecord> ptr;
    while (tmp.seizeFirst(ptr)) new (ptr.p) SyncRecord(*this, c_dataBufferPool);
    while (tmp.releaseFirst())
      ;
  }

  // Suma
  c_masterNodeId = getOwnNodeId();

  c_nodeGroup = c_noNodesInGroup = 0;
  for (Uint32 i = 0; i < MAX_REPLICAS; i++) {
    c_nodesInGroup[i] = 0;
  }

  m_first_free_page = RNIL;

  c_no_of_buckets = 0;
  memset(c_buckets, 0, sizeof(c_buckets));
  for (Uint32 i = 0; i < NO_OF_BUCKETS; i++) {
    Bucket *bucket = c_buckets + i;
    bucket->m_buffer_tail = RNIL;
    bucket->m_max_acked_gci = 0;
    bucket->m_buffer_head.m_page_id = RNIL;
    bucket->m_buffer_head.m_page_pos = Buffer_page::DATA_WORDS;
  }

  m_max_seen_gci = 0;       // FIRE_TRIG_ORD
  m_max_sent_gci = 0;       // FIRE_TRIG_ORD -> send
  m_last_complete_gci = 0;  // SUB_GCP_COMPLETE_REP
  m_gcp_complete_rep_count = 0;
  m_fully_buffering_after_gci = 0;
  m_max_fully_acked_gci = UINT64_MAX;
  m_missing_data = false;

  c_startup.m_wait_handover = false;
  c_startup.m_forced_disconnect_attempted = false;
  c_failedApiNodes.clear();
  c_startup.m_wait_handover_timeout_ms = 120000; /* Default for old MGMD */
  ndb_mgm_get_int_parameter(p, CFG_DB_AT_RESTART_SUBSCRIBER_CONNECT_TIMEOUT,
                            &c_startup.m_wait_handover_timeout_ms);

  ReadConfigConf *conf = (ReadConfigConf *)signal->getDataPtrSend();
  conf->senderRef = reference();
  conf->senderData = senderData;
  sendSignal(ref, GSN_READ_CONFIG_CONF, signal, ReadConfigConf::SignalLength,
             JBB);
}

void Suma::execSTTOR(Signal *signal) {
  jamEntry();

  DBUG_ENTER("Suma::execSTTOR");
  m_startphase = signal->theData[1];
  m_typeOfStart = signal->theData[7];

  DBUG_PRINT("info", ("startphase = %u, typeOfStart = %u", m_startphase,
                      m_typeOfStart));

  if (m_startphase == 3) {
    jam();
    void *ptr = m_ctx.m_mm.get_memroot();
    c_page_pool.set((Buffer_page *)ptr, (Uint32)~0);
  }

  if (m_startphase == 5) {
    jam();

    if (ERROR_INSERTED(13029)) /* Hold startphase 5 */
    {
      sendSignalWithDelay(SUMA_REF, GSN_STTOR, signal, 30, signal->getLength());
      DBUG_VOID_RETURN;
    }

    signal->theData[0] = reference();
    sendSignal(NDBCNTR_REF, GSN_READ_NODESREQ, signal, 1, JBB);
    DBUG_VOID_RETURN;
  }

  if (m_startphase == 7) {
    if (m_typeOfStart != NodeState::ST_NODE_RESTART &&
        m_typeOfStart != NodeState::ST_INITIAL_NODE_RESTART) {
      for (Uint32 i = 0; i < c_no_of_buckets; i++) {
        if (get_responsible_node(i) == getOwnNodeId()) {
          // I'm running this bucket
          DBUG_PRINT("info", ("bucket %u set to true", i));
          m_active_buckets.set(i);
          g_eventLogger->info("Activating bucket %u in SUMA", i);
        }
      }
    }

    if (!m_active_buckets.isclear()) {
      NdbNodeBitmask tmp;
      Uint32 bucket = 0;
      while ((bucket = m_active_buckets.find(bucket)) !=
             Bucket_mask::NotFound) {
        tmp.set(get_responsible_node(bucket, c_nodes_in_nodegroup_mask));
        bucket++;
      }

      ndbassert(tmp.get(getOwnNodeId()));
      m_gcp_complete_rep_count = m_active_buckets.count();
    } else
      m_gcp_complete_rep_count = 0;  // I contribute 1 gcp complete rep

    if (m_typeOfStart == NodeState::ST_INITIAL_START &&
        c_masterNodeId == getOwnNodeId()) {
      jam();
      createSequence(signal);
      DBUG_VOID_RETURN;
    }  // if

    if (ERROR_INSERTED(13030)) {
      g_eventLogger->info("Dont start handover");
      DBUG_VOID_RETURN;
    }
  }  // if

  if (m_startphase == 101) {
    if (ERROR_INSERTED(13053)) {
      jam();
      g_eventLogger->info("SUMA : ERROR 13053 : Stalling phase 101");
      sendSignalWithDelay(SUMA_REF, GSN_STTOR, signal, 1000,
                          signal->getLength());
      return;
    }

    if (m_typeOfStart == NodeState::ST_NODE_RESTART ||
        m_typeOfStart == NodeState::ST_INITIAL_NODE_RESTART) {
      jam();
      /**
       * Handover code here
       */
      c_startup.m_wait_handover = true;
      check_start_handover(signal);
      if (c_startup.m_wait_handover) {
        jam();
        /**
         * Handover is waiting for some Api connections,
         * We don't want to wait indefinitely
         */
        NdbTick_Invalidate(&c_startup.m_wait_handover_message_expire);
        if (c_startup.m_wait_handover_timeout_ms == 0) {
          jam();
          /* Unlimited wait */
          g_eventLogger->info(
              "Suma: handover waiting until all subscribers connected");
          NdbTick_Invalidate(&c_startup.m_wait_handover_expire);
        } else {
          jam();
          /* Bounded wait */
          NDB_TICKS now = NdbTick_getCurrentTicks();
          g_eventLogger->info(
              "Suma: handover waiting up to %ums for all subscribers to "
              "connect",
              c_startup.m_wait_handover_timeout_ms);
          c_startup.m_wait_handover_expire = NdbTick_AddMilliseconds(
              now, c_startup.m_wait_handover_timeout_ms);
        }
        check_wait_handover_timeout(signal);
      }
      DBUG_VOID_RETURN;
    }
  }
  sendSTTORRY(signal);
  DBUG_VOID_RETURN;
}

void Suma::send_dict_lock_req(Signal *signal, Uint32 state) {
  jam();
  DictLockReq *req = (DictLockReq *)signal->getDataPtrSend();
  req->lockType = state;
  req->userPtr = state;
  req->userRef = reference();
  sendSignal(calcDictBlockRef(c_masterNodeId), GSN_DICT_LOCK_REQ, signal,
             DictLockReq::SignalLength, JBB);
}

void Suma::execDICT_LOCK_CONF(Signal *signal) {
  jamEntry();

  DictLockConf *conf = (DictLockConf *)signal->getDataPtr();
  Uint32 state = conf->userPtr;
  SubscriptionPtr subPtr;

  switch (state) {
    case DictLockReq::SumaStartMe:
      jam();
      ndbrequire(c_startup.m_restart_server_node_id == RNIL);
      c_startup.m_restart_server_node_id = 0;
      CRASH_INSERTION(13039);
      send_start_me_req(signal);
      return;
    case DictLockReq::SumaHandOver:
      jam();
      if ((c_no_of_buckets == 0) && (!c_subscriptions.first(subPtr))) {
        /**
         * no subscriptions to report
         * Continue restart
         */
        jam();
        send_dict_unlock_ord(signal, DictLockReq::SumaHandOver);
        sendSTTORRY(signal);
        return;
      }

      /**
       * All subscribers are now connected.
       * Report subscriptions details to all the subscribers.
       * The DictLockReq::SumaHandOver lock also makes sure that no
       * changes (add/drop subscriber/subscription) are made to the
       * subscriptions when the reports are being sent out across various
       * CONTINUEB signals. The only exception is API node failure that can
       * happen at any time. To prevent API failure handling to be performed in
       * parallel with the reporting it is delayed while reporting is in
       * progress.
       */
      g_reporting_in_progress = true;
      report_subscription_set(signal, RNIL, RNIL, RNIL,
                              REPORT_SUBSCRIPTION_INTERVAL);

      return;
    default:
      jam();
      jamLine(state);
      ndbabort();
  }
}

void Suma::execDICT_LOCK_REF(Signal *signal) {
  jamEntry();

  DictLockRef *ref = (DictLockRef *)signal->getDataPtr();
  Uint32 state = ref->userPtr;

  ndbrequire(ref->errorCode == DictLockRef::TooManyRequests);
  signal->theData[0] = SumaContinueB::RETRY_DICT_LOCK;
  signal->theData[1] = state;
  sendSignalWithDelay(reference(), GSN_CONTINUEB, signal, 300, 2);
}

void Suma::send_dict_unlock_ord(Signal *signal, Uint32 state) {
  jam();
  DictUnlockOrd *ord = (DictUnlockOrd *)signal->getDataPtrSend();
  ord->lockPtr = 0;
  ord->lockType = state;
  ord->senderData = state;
  ord->senderRef = reference();
  sendSignal(calcDictBlockRef(c_masterNodeId), GSN_DICT_UNLOCK_ORD, signal,
             DictUnlockOrd::SignalLength, JBB);
}

void Suma::send_start_me_req(Signal *signal) {
  Uint32 nodeId = c_startup.m_restart_server_node_id;
  do {
    nodeId = c_alive_nodes.find(nodeId + 1);

    if (nodeId == getOwnNodeId()) continue;
    if (nodeId == NdbNodeBitmask::NotFound) {
      nodeId = 0;
      continue;
    }
    break;
  } while (true);

  infoEvent("Suma: asking node %d to recreate subscriptions on me", nodeId);
  c_startup.m_restart_server_node_id = nodeId;
  sendSignal(calcSumaBlockRef(nodeId), GSN_SUMA_START_ME_REQ, signal, 1, JBB);
}

void Suma::execSUMA_START_ME_REF(Signal *signal) {
  const SumaStartMeRef *ref = (SumaStartMeRef *)signal->getDataPtr();

  Uint32 error = ref->errorCode;
  if (error != SumaStartMeRef::Busy && error != SumaStartMeRef::NotStarted) {
    jam();
    // for some reason we did not manage to create a subscription
    // on the starting node
    SystemError *const sysErr = (SystemError *)&signal->theData[0];
    sysErr->errorCode = SystemError::CopySubscriptionRef;
    sysErr->errorRef = reference();
    sysErr->data[0] = error;
    sysErr->data[1] = 0;
    sendSignal(NDBCNTR_REF, GSN_SYSTEM_ERROR, signal, SystemError::SignalLength,
               JBB);
    return;
  }

  infoEvent("Suma: node %d refused %d", c_startup.m_restart_server_node_id,
            ref->errorCode);

  send_start_me_req(signal);
}

void Suma::execSUMA_START_ME_CONF(Signal *signal) {
  infoEvent("Suma: node %d has completed restoring me",
            c_startup.m_restart_server_node_id);
  sendSTTORRY(signal);
  send_dict_unlock_ord(signal, DictLockReq::SumaStartMe);
  c_startup.m_restart_server_node_id = 0;
}

void Suma::createSequence(Signal *signal) {
  jam();
  DBUG_ENTER("Suma::createSequence");

  UtilSequenceReq *req = (UtilSequenceReq *)signal->getDataPtrSend();

  req->senderData = RNIL;
  req->sequenceId = SUMA_SEQUENCE;
  req->requestType = UtilSequenceReq::Create;
  sendSignal(DBUTIL_REF, GSN_UTIL_SEQUENCE_REQ, signal,
             UtilSequenceReq::SignalLength, JBB);
  // execUTIL_SEQUENCE_CONF will call createSequenceReply()
  DBUG_VOID_RETURN;
}

void Suma::createSequenceReply(Signal *signal, UtilSequenceConf *conf,
                               UtilSequenceRef *ref) {
  jam();

  if (ref != NULL) {
    switch ((UtilSequenceRef::ErrorCode)ref->errorCode) {
      case UtilSequenceRef::NoSuchSequence:
        ndbabort();
      case UtilSequenceRef::TCError: {
        char buf[128];
        BaseString::snprintf(
            buf, sizeof(buf),
            "Startup failed during sequence creation. TC error %d",
            ref->TCErrorCode);
        progError(__LINE__, NDBD_EXIT_RESOURCE_ALLOC_ERROR, buf);
      }
    }
    ndbabort();
  }

  sendSTTORRY(signal);
}

void Suma::execREAD_NODESCONF(Signal *signal) {
  jamEntry();
  ReadNodesConf *const conf = (ReadNodesConf *)signal->getDataPtr();

  {
    ndbrequire(signal->getNoOfSections() == 1);
    SegmentedSectionPtr ptr;
    SectionHandle handle(this, signal);
    ndbrequire(handle.getSection(ptr, 0));
    ndbrequire(ptr.sz == 5 * NdbNodeBitmask::Size);
    copy((Uint32 *)&conf->definedNodes.rep.data, ptr);
    releaseSections(handle);
  }

  if (getNodeState().getNodeRestartInProgress()) {
    NdbNodeBitmask started_nodes;
    started_nodes.assign(conf->startedNodes);
    c_alive_nodes.bitOR(started_nodes);
    c_alive_nodes.set(getOwnNodeId());
  } else {
    c_alive_nodes = conf->startingNodes;
    NdbNodeBitmask tmp = conf->startedNodes;
    ndbrequire(tmp.isclear());  // No nodes can be started during SR
  }

  if (DBG_3R) {
    for (Uint32 i = 0; i < MAX_NDB_NODES; i++) {
      if (c_alive_nodes.get(i))
        g_eventLogger->info("%u c_alive_nodes.set(%u)", __LINE__, i);
    }
  }

  c_masterNodeId = conf->masterNodeId;

  getNodeGroupMembers(signal);
}

void Suma::getNodeGroupMembers(Signal *signal) {
  jam();
  DBUG_ENTER("Suma::getNodeGroupMembers");
  /**
   * Ask DIH for nodeGroupMembers
   */
  CheckNodeGroups *sd = (CheckNodeGroups *)signal->getDataPtrSend();
  sd->blockRef = reference();
  sd->requestType = CheckNodeGroups::GetNodeGroupMembers;
  sd->nodeId = getOwnNodeId();
  sd->senderData = RNIL;
  {
    /* Prepare a node bitmask sent in section, ignored in this case. */
    sd->mask.clear();
    LinearSectionPtr lsptr[3];
    lsptr[0].p = sd->mask.rep.data;
    lsptr[0].sz = sd->mask.getPackedLengthInWords();
    sendSignal(DBDIH_REF, GSN_CHECKNODEGROUPSREQ, signal,
               CheckNodeGroups::SignalLengthNoBitmask, JBB, lsptr, 1);
  }
  DBUG_VOID_RETURN;
}

static bool valid_seq(Uint32 n, Uint32 r, Uint16 dst[]) {
  Uint16 tmp[MAX_REPLICAS];
  /**
   * Set to 0 the unused entries in m_nodes
   */
  for (Uint32 i = r; i < MAX_REPLICAS; i++) {
    dst[i] = 0;
  }

  for (Uint32 i = 0; i < r; i++) {
    tmp[i] = n % r;
    for (Uint32 j = 0; j < i; j++)
      if (tmp[j] == tmp[i]) return false;
    n /= r;
  }

  /**
   * reverse order for backward compatibility (with 2 replica)
   */
  for (Uint32 i = 0; i < r; i++) dst[i] = tmp[r - i - 1];

  return true;
}

void Suma::calculate_sub_data_stream(Uint16 bucket, Uint16 buckets,
                                     Uint16 replicas) {
  ndbassert(bucket < NO_OF_BUCKETS);
  Bucket *ptr = c_buckets + bucket;

  // First responsible node, irrespective of it is up or not
  const Uint16 node = ptr->m_nodes[0];
  ndbassert(node >= 1);
  ndbassert(node <= MAX_SUB_DATA_STREAM_GROUPS);
  const Uint16 buckets_per_node = buckets / replicas;
  ndbassert(buckets_per_node <= MAX_SUB_DATA_STREAMS_PER_GROUP);
  const Uint16 sub_data_stream = (node << 8) | (bucket % buckets_per_node);

#ifdef VM_TRACE
  // Verify that this blocks sub data stream identifiers are unique.
  for (Uint32 i = 0; i < bucket; i++) {
    ndbassert(c_buckets[i].m_sub_data_stream != sub_data_stream);
  }
#endif

  ptr->m_sub_data_stream = sub_data_stream;
}

void Suma::fix_nodegroup() {
  Uint32 i, pos = 0;

  for (i = 0; i < MAX_NDB_NODES; i++) {
    if (c_nodes_in_nodegroup_mask.get(i)) {
      c_nodesInGroup[pos++] = i;
    }
  }

  const Uint32 replicas = c_noNodesInGroup = pos;

  if (replicas) {
    Uint32 buckets = 1;
    for (i = 1; i <= replicas; i++) buckets *= i;

    Uint32 tot = 0;
    switch (replicas) {
      case 1:
        tot = 1;
        break;
      case 2:
        tot = 4;  // 2^2
        break;
      case 3:
        tot = 27;  // 3^3
        break;
      case 4:
        tot = 256;  // 4^4
        break;
    }
    Uint32 cnt = 0;
    for (i = 0; i < tot; i++) {
      Bucket *ptr = c_buckets + cnt;
      if (valid_seq(i, replicas, ptr->m_nodes)) {
        jam();
        if (DBG_3R) printf("bucket %u : ", cnt);
        for (Uint32 j = 0; j < replicas; j++) {
          ptr->m_nodes[j] = c_nodesInGroup[ptr->m_nodes[j]];
          if (DBG_3R) printf("%u ", ptr->m_nodes[j]);
        }
        if (DBG_3R) printf("\n");
        calculate_sub_data_stream(cnt, buckets, replicas);
        cnt++;
      }
    }
    ndbrequire(cnt == buckets);
    c_no_of_buckets = buckets;
  } else {
    jam();
    c_no_of_buckets = 0;
  }
}

void Suma::execCHECKNODEGROUPSCONF(Signal *signal) {
  DBUG_ENTER("Suma::execCHECKNODEGROUPSCONF");
  jamEntry();

  {
    CheckNodeGroups *sd = (CheckNodeGroups *)signal->getDataPtrSend();
    /**
     * Handle NDB node bitmask now arriving in section to handle
     * very many data nodes.
     */
    Uint32 *node_bitmask =
        (Uint32 *)&signal->theData[CheckNodeGroups::SignalLength];
    ndbrequire(signal->getNoOfSections() == 1);
    SegmentedSectionPtr ptr;
    SectionHandle handle(this, signal);
    ndbrequire(handle.getSection(ptr, 0));
    ndbrequire(ptr.sz <= NdbNodeBitmask::Size);
    memset(node_bitmask, 0, NdbNodeBitmask::Size * sizeof(Uint32));
    copy(node_bitmask, ptr);
    sd->mask.assign(NdbNodeBitmask::Size, node_bitmask);
    releaseSections(handle);
  }
  const CheckNodeGroups *sd = (const CheckNodeGroups *)signal->getDataPtrSend();

  c_nodeGroup = sd->output;
  c_nodes_in_nodegroup_mask.assign(sd->mask);
  c_noNodesInGroup = c_nodes_in_nodegroup_mask.count();

  fix_nodegroup();

#ifndef NDEBUG
  for (Uint32 i = 0; i < c_noNodesInGroup; i++) {
    DBUG_PRINT("exit", ("Suma: NodeGroup %u, me %u, "
                        "member[%u] %u",
                        c_nodeGroup, getOwnNodeId(), i, c_nodesInGroup[i]));
  }
#endif

  if (m_typeOfStart == NodeState::ST_NODE_RESTART ||
      m_typeOfStart == NodeState::ST_INITIAL_NODE_RESTART) {
    jam();

    send_dict_lock_req(signal, DictLockReq::SumaStartMe);

    return;
  }

  c_startup.m_restart_server_node_id = 0;
  sendSTTORRY(signal);

  DBUG_VOID_RETURN;
}

void Suma::execAPI_START_REP(Signal *signal) {
  Uint32 nodeId = signal->theData[0];
  c_connected_nodes.set(nodeId);

  check_start_handover(signal);
}

void Suma::check_start_handover(Signal *signal) {
  if (c_startup.m_wait_handover) {
    jam();
    NodeBitmask tmp;
    tmp.assign(c_connected_nodes);
    tmp.bitAND(c_subscriber_nodes);
    if (!c_subscriber_nodes.equal(tmp)) {
      return;
    }
    c_startup.m_wait_handover = false;
    SubscriptionPtr subPtr;

    /** Lock the dict.
     * Lock is needed because, at least, one of the three following conditions
     * is always met:
     * 1. There are any buckets to handover
     * 2. There are subscriptions whose reports need to be sent out
     * 3. No buckets to handover nor subscriptions reports to send out (e.g
     * start node with no nodegroup assigned and no subscribers connected), but
     * lock is needed to force dict to trigger DIH to update the ndbinfo
     * restart_state to RESTART COMPLETED
     */
    send_dict_lock_req(signal, DictLockReq::SumaHandOver);
  }
}

void Suma::check_wait_handover_timeout(Signal *signal) {
  jam();
  if (c_startup.m_wait_handover) {
    jam();
    /* Still waiting */

    /* Send CONTINUEB for next check... */
    signal->theData[0] = SumaContinueB::HANDOVER_WAIT_TIMEOUT;
    sendSignalWithDelay(reference(), GSN_CONTINUEB, signal, 1000, 1);

    /* Now check whether we should do something more */
    NDB_TICKS now = NdbTick_getCurrentTicks();
    if (NdbTick_IsValid(c_startup.m_wait_handover_expire)) {
      jam();

      /* Wait is bounded... has it expired? */
      if (NdbTick_Compare(c_startup.m_wait_handover_expire, now) >= 0) {
        jam();

        /* Not expired, consider a log message, then wait some more */
        check_wait_handover_message(now);
        return;
      }

      /* Wait time has expired */
      NdbTick_Invalidate(&c_startup.m_wait_handover_expire);

      NodeBitmask subscribers_not_connected;
      subscribers_not_connected.assign(c_subscriber_nodes);
      subscribers_not_connected.bitANDC(c_connected_nodes);

      if (!subscribers_not_connected.isclear()) {
        jam();
        if (!c_startup.m_forced_disconnect_attempted) {
          // Disconnect API nodes subscribing but not connected
          jam();
          Uint32 nodeId = 0;
          while ((nodeId = subscribers_not_connected.find_next(nodeId + 1)) <
                 MAX_NODES) {
            jam();
            // Disconnecting node
            signal->theData[0] = NDB_LE_SubscriptionStatus;
            signal->theData[1] = 3;  // NOTCONNECTED;
            signal->theData[2] = nodeId;
            signal->theData[3] =
                (c_startup.m_wait_handover_timeout_ms + 999) / 1000;
            sendSignal(CMVMI_REF, GSN_EVENT_REP, signal, 4, JBB);

            // Same message to data node log file
            LogLevel ll;
            ll.setLogLevel(LogLevel::llError, 15);
            g_eventLogger->log(NDB_LE_SubscriptionStatus, signal->theData,
                               signal->getLength(), getOwnNodeId(), &ll);

            /**
             * Force API_FAILREQ
             */
            if (ERROR_INSERTED(13048)) {
              g_eventLogger->info("Skipping forced disconnect of %u", nodeId);
            } else {
              signal->theData[0] = nodeId;
              sendSignal(QMGR_REF, GSN_API_FAILREQ, signal, 1, JBB);
            }
          }

          /* Restart timing checks, but if we expire again
           * then we will shut down
           */
          c_startup.m_forced_disconnect_attempted = true;

          NDB_TICKS now = NdbTick_getCurrentTicks();
          c_startup.m_wait_handover_expire = NdbTick_AddMilliseconds(
              now, c_startup.m_wait_handover_timeout_ms);
        } else {
          jam();
          /* We already tried forcing a disconnect, and it failed
           * to get all subscribers connected.  Shutdown
           */
          g_eventLogger->critical(
              "Failed to establish direct connection to all subscribers, "
              "shutting down.  (%s)",
              BaseString::getPrettyTextShort(subscribers_not_connected)
                  .c_str());
          CRASH_INSERTION(13048);
          progError(__LINE__, NDBD_EXIT_GENERIC,
                    "Failed to establish direct connection to all subscribers");
        }
      } else {
        /* Why are we waiting if there are no disconnected subscribers? */
        g_eventLogger->critical(
            "Subscriber nodes : %s",
            BaseString::getPrettyTextShort(c_subscriber_nodes).c_str());
        g_eventLogger->critical(
            "Connected nodes  : %s",
            BaseString::getPrettyTextShort(c_connected_nodes).c_str());
        ndbabort();
      }
    } else {
      /* Unbounded wait, display message */
      check_wait_handover_message(now);
    }
  }
}

void Suma::check_wait_handover_message(NDB_TICKS now) {
  jam();

  NodeBitmask subscribers_not_connected;
  subscribers_not_connected.assign(c_subscriber_nodes);
  subscribers_not_connected.bitANDC(c_connected_nodes);

  if (!NdbTick_IsValid(
          c_startup.m_wait_handover_message_expire) ||  // First time
      NdbTick_Compare(c_startup.m_wait_handover_message_expire, now) <
          0)  // Time is up
  {
    jam();
    if (NdbTick_IsValid(c_startup.m_wait_handover_expire)) {
      /* Bounded wait */
      ndbassert(NdbTick_Compare(c_startup.m_wait_handover_expire, now) >= 0);
      NdbDuration time_left =
          NdbTick_Elapsed(now, c_startup.m_wait_handover_expire);
      Uint64 milliseconds_left = time_left.milliSec();
      g_eventLogger->info(
          "Start phase 101 waiting %us for absent subscribers to connect : %s",
          (unsigned)((milliseconds_left + 999) / 1000),
          BaseString::getPrettyTextShort(subscribers_not_connected).c_str());
      if (milliseconds_left > 0) {  // Plan next message on next even 10s
                                    // multiple before wait handover expire
        c_startup.m_wait_handover_message_expire =
            NdbTick_AddMilliseconds(now, (milliseconds_left - 1) % 10000 + 1);
      } else {
        c_startup.m_wait_handover_message_expire = now;
      }
    } else {
      /* Unbounded wait, show progress */
      g_eventLogger->info(
          "Start phase 101 waiting for absent subscribers to connect : %s",
          BaseString::getPrettyTextShort(subscribers_not_connected).c_str());
      c_startup.m_wait_handover_message_expire =
          NdbTick_AddMilliseconds(now, 10000);
    }
  }
}

void Suma::send_handover_req(Signal *signal, Uint32 type) {
  jam();
  c_startup.m_handover_nodes.assign(c_alive_nodes);
  c_startup.m_handover_nodes.bitAND(c_nodes_in_nodegroup_mask);
  c_startup.m_handover_nodes.clear(getOwnNodeId());
  Uint32 gci = Uint32(m_last_complete_gci >> 32) + 3;

  SumaHandoverReq *req = (SumaHandoverReq *)signal->getDataPtrSend();
  char buf[NdbNodeBitmask::TextLength + 1];
  c_startup.m_handover_nodes.getText(buf);
  infoEvent("Suma: initiate handover for %s with nodes %s GCI: %u",
            (type == SumaHandoverReq::RT_START_NODE ? "startup" : "shutdown"),
            buf, gci);

  req->gci = gci;
  req->nodeId = getOwnNodeId();
  req->requestType = type;

  NodeReceiverGroup rg(SUMA, c_startup.m_handover_nodes);
  sendSignal(rg, GSN_SUMA_HANDOVER_REQ, signal, SumaHandoverReq::SignalLength,
             JBB);
}

void Suma::sendSTTORRY(Signal *signal) {
  signal->theData[0] = 0;
  signal->theData[3] = 1;
  signal->theData[4] = 3;
  signal->theData[5] = 5;
  signal->theData[6] = 7;
  signal->theData[7] = 101;
  signal->theData[8] = 255;  // No more start phases from missra
  sendSignal(NDBCNTR_REF, GSN_STTORRY, signal, 9, JBB);
}

void Suma::execNDB_STTOR(Signal *signal) { jamEntry(); }

void Suma::execCONTINUEB(Signal *signal) {
  jamEntry();
  Uint32 type = signal->theData[0];
  switch (type) {
    case SumaContinueB::RELEASE_GCI: {
      Uint32 gci_hi = signal->theData[2];
      Uint32 gci_lo = signal->theData[3];
      Uint64 gci = gci_lo | (Uint64(gci_hi) << 32);
      release_gci(signal, signal->theData[1], gci);
      return;
    }
    case SumaContinueB::RESEND_BUCKET: {
      Uint32 min_gci_hi = signal->theData[2];
      Uint32 min_gci_lo = signal->theData[5];
      Uint32 last_gci_hi = signal->theData[4];
      Uint32 last_gci_lo = signal->theData[6];
      Uint64 min_gci = min_gci_lo | (Uint64(min_gci_hi) << 32);
      Uint64 last_gci = last_gci_lo | (Uint64(last_gci_hi) << 32);
      resend_bucket(signal, signal->theData[1], min_gci, signal->theData[3],
                    last_gci);
      return;
    }
    case SumaContinueB::OUT_OF_BUFFER_RELEASE:
      out_of_buffer_release(signal, signal->theData[1]);
      return;
    case SumaContinueB::API_FAIL_GCI_LIST:
      api_fail_gci_list(signal, signal->theData[1]);
      return;
    case SumaContinueB::API_FAIL_SUBSCRIBER_LIST:
      api_fail_subscriber_list(signal, signal->theData[1]);
      return;
    case SumaContinueB::API_FAIL_SUBSCRIPTION:
      api_fail_subscription(signal);
      return;
    case SumaContinueB::SUB_STOP_REQ:
      sub_stop_req(signal);
      return;
    case SumaContinueB::RETRY_DICT_LOCK:
      jam();
      send_dict_lock_req(signal, signal->theData[1]);
      return;
    case SumaContinueB::HANDOVER_WAIT_TIMEOUT:
      jam();
      check_wait_handover_timeout(signal);
      return;
    case SumaContinueB::WAIT_SCAN_TAB_REQ:
      jam();
      sendDIH_SCAN_TAB_REQ(signal, signal->theData[1], signal->theData[2],
                           signal->theData[3]);
      return;
    case SumaContinueB::WAIT_GET_FRAGMENT: {
      sendDIGETNODESREQ(signal, signal->theData[1], signal->theData[2],
                        signal->theData[3]);
      return;
    }
    case SumaContinueB::SEND_SUB_GCP_COMPLETE_REP: {
      jam();
      sendSUB_GCP_COMPLETE_REP(signal);
      return;
    }
    case SumaContinueB::REPORT_SUBSCRIPTION_SET: {
      jam();
      report_subscription_set(signal, signal->theData[1], signal->theData[2],
                              signal->theData[3], signal->theData[4]);
      return;
    }
    default: {
      ndbabort();
    }
  }
}

/*****************************************************************************
 *
 * Node state handling
 *
 *****************************************************************************/

void Suma::execAPI_FAILREQ(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execAPI_FAILREQ");
  Uint32 failedApiNode = signal->theData[0];
  ndbrequire(failedApiNode < MAX_NODES);
  ndbrequire(signal->theData[1] == QMGR_REF);  // As callback hard-codes QMGR

  c_connected_nodes.clear(failedApiNode);

  if (c_failedApiNodes.get(failedApiNode)) {
    jam();
    /* Being handled already, just conf */
    goto CONF;
  }

  if (!c_subscriber_nodes.get(failedApiNode)) {
    jam();
    /* No Subscribers on that node, no SUMA
     * specific work to do
     */
    goto BLOCK_CLEANUP;
  }

  c_failedApiNodes.set(failedApiNode);
  c_subscriber_nodes.clear(failedApiNode);
  c_subscriber_per_node[failedApiNode] = 0;
  c_failedApiNodesState[failedApiNode] = __LINE__;

  check_start_handover(signal);

  signal->theData[0] = SumaContinueB::API_FAIL_GCI_LIST;
  signal->theData[1] = failedApiNode;
  sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 2, JBB);
  return;

BLOCK_CLEANUP:
  jam();
  api_fail_block_cleanup(signal, failedApiNode);
  DBUG_VOID_RETURN;

CONF:
  jam();
  signal->theData[0] = failedApiNode;
  signal->theData[1] = reference();
  sendSignal(QMGR_REF, GSN_API_FAILCONF, signal, 2, JBB);

  c_failedApiNodesState[failedApiNode] = 0;

  DBUG_VOID_RETURN;
}  // execAPI_FAILREQ()

void Suma::api_fail_block_cleanup_callback(Signal *signal, Uint32 failedNodeId,
                                           Uint32 elementsCleaned) {
  jamEntry();

  /* Suma should not have any block level elements
   * to be cleaned (Fragmented send/receive structures etc.)
   * As it only uses Fragmented send/receive locally
   */
  ndbassert(elementsCleaned == 0);

  /* Node failure handling is complete */
  signal->theData[0] = failedNodeId;
  signal->theData[1] = reference();
  sendSignal(QMGR_REF, GSN_API_FAILCONF, signal, 2, JBB);
  c_failedApiNodes.clear(failedNodeId);
  c_failedApiNodesState[failedNodeId] = 0;
}

void Suma::api_fail_block_cleanup(Signal *signal, Uint32 failedNode) {
  jam();

  c_failedApiNodesState[failedNode] = __LINE__;

  Callback cb = {safe_cast(&Suma::api_fail_block_cleanup_callback), failedNode};

  simBlockNodeFailure(signal, failedNode, cb);
}

void Suma::api_fail_gci_list(Signal *signal, Uint32 nodeId) {
  jam();

  ndbrequire(nodeId < MAX_NODES);

  Ptr<Gcp_record> gcp;
  if (c_gcp_list.first(gcp)) {
    jam();
    gcp.p->m_subscribers.bitAND(c_subscriber_nodes);

    if (gcp.p->m_subscribers.isclear()) {
      jam();

      SubGcpCompleteAck *ack = (SubGcpCompleteAck *)signal->getDataPtrSend();
      ack->rep.gci_hi = Uint32(gcp.p->m_gci >> 32);
      ack->rep.gci_lo = Uint32(gcp.p->m_gci);
      ack->rep.senderRef = reference();
      NodeReceiverGroup rg(SUMA, c_nodes_in_nodegroup_mask);
      sendSignal(rg, GSN_SUB_GCP_COMPLETE_ACK, signal,
                 SubGcpCompleteAck::SignalLength, JBB);

      c_gcp_list.release(gcp);

      c_failedApiNodesState[nodeId] = __LINE__;
      signal->theData[0] = SumaContinueB::API_FAIL_GCI_LIST;
      signal->theData[1] = nodeId;
      sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 2, JBB);
      return;
    }
  }

  if (ERROR_INSERTED(13023)) {
    CLEAR_ERROR_INSERT_VALUE;
  }

  signal->theData[0] = SumaContinueB::API_FAIL_SUBSCRIBER_LIST;
  signal->theData[1] = nodeId;
  signal->theData[2] = RNIL;  // SubOpPtr
  signal->theData[3] = RNIL;  // c_subscribers bucket
  signal->theData[4] = RNIL;  // subscriptionId
  signal->theData[5] = RNIL;  // SubscriptionKey

  Ptr<SubOpRecord> subOpPtr;
  if (c_subOpPool.seize(subOpPtr)) {
    c_failedApiNodesState[nodeId] = __LINE__;
    signal->theData[2] = subOpPtr.i;
    sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 6, JBB);
  } else {
    c_failedApiNodesState[nodeId] = __LINE__;
    sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 3, JBB);
  }

  return;
}

void Suma::api_fail_subscriber_list(Signal *signal, Uint32 nodeId) {
  jam();
  Ptr<SubOpRecord> subOpPtr;

  ndbrequire(nodeId < MAX_NODES);

  if (c_outstanding_drop_trig_req > NDB_MAX_SUMA_DROP_TRIG_REQ_APIFAIL) {
    /* Avoid overflowing DbtupProxy with too many GSN_DROP_TRIG_IMPL_REQs */
    jam();
    sendSignalWithDelay(reference(), GSN_CONTINUEB, signal, 100,
                        signal->getLength());
    return;
  }

  subOpPtr.i = signal->theData[2];
  if (subOpPtr.i == RNIL) {
    if (c_subOpPool.seize(subOpPtr)) {
      signal->theData[3] = RNIL;
    } else {
      jam();
      sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 3, JBB);
      c_failedApiNodesState[nodeId] = __LINE__;
      return;
    }
  } else {
    jam();
    c_subOpPool.getPtr(subOpPtr);
  }

  Uint32 bucket = signal->theData[3];
  Uint32 subscriptionId = signal->theData[4];
  Uint32 subscriptionKey = signal->theData[5];

  Subscription_hash::Iterator iter;
  if (bucket == RNIL) {
    jam();
    c_subscriptions.first(iter);
    c_failedApiNodesState[nodeId] = __LINE__;
  } else {
    jam();

    Subscription key;
    key.m_subscriptionId = subscriptionId;
    key.m_subscriptionKey = subscriptionKey;
    if (c_subscriptions.find(iter.curr, key) == false) {
      jam();
      /**
       * We restart from this bucket :-(
       */
      c_subscriptions.next(bucket, iter);
      c_failedApiNodesState[nodeId] = __LINE__;
    } else {
      iter.bucket = bucket;
    }
  }

  if (iter.curr.isNull()) {
    jam();
    api_fail_block_cleanup(signal, nodeId);
    c_failedApiNodesState[nodeId] = __LINE__;
    return;
  }

  subOpPtr.p->m_opType = SubOpRecord::R_API_FAIL_REQ;
  subOpPtr.p->m_subPtrI = iter.curr.i;
  subOpPtr.p->m_senderRef = nodeId;
  subOpPtr.p->m_senderData = iter.bucket;

  Local_SubOpRecord_fifo list(c_subOpPool, iter.curr.p->m_stop_req);
  bool empty = list.isEmpty();
  list.addLast(subOpPtr);

  if (empty) {
    jam();
    c_failedApiNodesState[nodeId] = __LINE__;
    signal->theData[0] = SumaContinueB::API_FAIL_SUBSCRIPTION;
    signal->theData[1] = subOpPtr.i;
    signal->theData[2] = RNIL;
    sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 3, JBB);
  } else {
    jam();
    c_failedApiNodesState[nodeId] = __LINE__;
  }
}

void Suma::api_fail_subscription(Signal *signal) {
  if (g_reporting_in_progress) {
    jam();
    signal->theData[0] = SumaContinueB::API_FAIL_SUBSCRIPTION;
    sendSignalWithDelay(reference(), GSN_CONTINUEB, signal, 20,
                        signal->getLength());
    return;
  }
  jam();
  Ptr<SubOpRecord> subOpPtr;
  ndbrequire(c_subOpPool.getPtr(subOpPtr, signal->theData[1]));

  Uint32 nodeId = subOpPtr.p->m_senderRef;

  Ptr<Subscription> subPtr;
  ndbrequire(c_subscriptionPool.getPtr(subPtr, subOpPtr.p->m_subPtrI));

  Ptr<Subscriber> ptr;
  {
    Local_Subscriber_list list(c_subscriberPool, subPtr.p->m_subscribers);
    if (signal->theData[2] == RNIL) {
      jam();
      list.first(ptr);
    } else {
      jam();
      list.getPtr(ptr, signal->theData[2]);
    }

    for (Uint32 i = 0; i < 32 && !ptr.isNull(); i++) {
      jam();
      if (refToNode(ptr.p->m_senderRef) == nodeId) {
        jam();

        Ptr<Subscriber> tmp = ptr;
        list.next(ptr);
        list.remove(tmp);

        /**
         * NOTE: remove before...so we done send UNSUBSCRIBE to self (yuck)
         */
        bool report = subPtr.p->m_options & Subscription::REPORT_SUBSCRIBE;

        send_sub_start_stop_event(signal, tmp, NdbDictionary::Event::_TE_STOP,
                                  report, list);

        c_subscriberPool.release(tmp);
      } else {
        jam();
        list.next(ptr);
      }
    }
  }

  if (!ptr.isNull()) {
    jam();
    c_failedApiNodesState[nodeId] = __LINE__;
    signal->theData[0] = SumaContinueB::API_FAIL_SUBSCRIPTION;
    signal->theData[1] = subOpPtr.i;
    signal->theData[2] = ptr.i;
    sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 3, JBB);
    return;
  }

  // Start potential waiter(s)
  check_remove_queue(signal, subPtr, subOpPtr, true, false);
  check_release_subscription(signal, subPtr);

  // Continue iterating through subscriptions
  Subscription_hash::Iterator iter;
  iter.bucket = subOpPtr.p->m_senderData;
  iter.curr = subPtr;

  if (c_subscriptions.next(iter)) {
    jam();
    c_failedApiNodesState[nodeId] = __LINE__;
    signal->theData[0] = SumaContinueB::API_FAIL_SUBSCRIBER_LIST;
    signal->theData[1] = nodeId;
    signal->theData[2] = subOpPtr.i;
    signal->theData[3] = iter.bucket;
    signal->theData[4] = iter.curr.p->m_subscriptionId;   // subscriptionId
    signal->theData[5] = iter.curr.p->m_subscriptionKey;  // SubscriptionKey
    sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 6, JBB);
    return;
  }

  c_subOpPool.release(subOpPtr);

  /* Now do block level cleanup */
  api_fail_block_cleanup(signal, nodeId);
}

void Suma::execNODE_FAILREP(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execNODE_FAILREP");
  NodeFailRep *rep = (NodeFailRep *)signal->getDataPtr();

  if (signal->getNoOfSections() >= 1) {
    ndbrequire(ndbd_send_node_bitmask_in_section(
        getNodeInfo(refToNode(signal->getSendersBlockRef())).m_version));
    SegmentedSectionPtr ptr;
    SectionHandle handle(this, signal);
    ndbrequire(handle.getSection(ptr, 0));
    memset(rep->theNodes, 0, sizeof(rep->theNodes));
    copy(rep->theNodes, ptr);
    releaseSections(handle);
  } else {
    memset(rep->theNodes + NdbNodeBitmask48::Size, 0, _NDB_NBM_DIFF_BYTES);
  }
  NdbNodeBitmask failed;
  failed.assign(NdbNodeBitmask::Size, rep->theNodes);

  if (c_restart.m_ref && failed.get(refToNode(c_restart.m_ref))) {
    jam();

    if (c_restart.m_waiting_on_self) {
      jam();
      c_restart.m_abort = 1;
    } else {
      jam();
      Ptr<Subscription> subPtr;
      ndbrequire(c_subscriptionPool.getPtr(subPtr, c_restart.m_subPtrI));
      abort_start_me(signal, subPtr, false);
    }
  }

  if (ERROR_INSERTED(13032)) {
    Uint32 node = c_subscriber_nodes.find(0);
    if (node != NodeBitmask::NotFound) {
      g_eventLogger->info("Inserting API_FAILREQ node: %u", node);
      signal->theData[0] = node;
      sendSignal(QMGR_REF, GSN_API_FAILREQ, signal, 1, JBA);
    }
  }

  NdbNodeBitmask tmp;
  tmp.assign(c_alive_nodes);
  tmp.bitANDC(failed);

  NdbNodeBitmask takeover_nodes;

  if (c_nodes_in_nodegroup_mask.overlaps(failed)) {
    Bucket_mask resend_buckets;
    for (Uint32 i = 0; i < c_no_of_buckets; i++) {
      if (m_active_buckets.get(i))
        continue;
      else if (m_switchover_buckets.get(i)) {
        Uint32 state = c_buckets[i].m_state;
        if ((state & Bucket::BUCKET_HANDOVER) &&
            failed.get(get_responsible_node(i))) {
          m_active_buckets.set(i);
          m_switchover_buckets.clear(i);
          g_eventLogger->info("aborting handover");
        } else if (state & Bucket::BUCKET_STARTING) {
          progError(__LINE__, NDBD_EXIT_SYSTEM_ERROR,
                    "Nodefailure during SUMA takeover");
        } else if (state & Bucket::BUCKET_SHUTDOWN_TO) {
          jam();
          // I am taking over from a shutdown node, but another node from
          // the same nodegroup failed before takeover could complete
          c_buckets[i].m_state &= ~Uint32(Bucket::BUCKET_SHUTDOWN_TO);
          m_switchover_buckets.clear(i);
          int rsp_node = get_responsible_node(i, tmp);
          ndbrequire(rsp_node == getOwnNodeId() ||
                     rsp_node == c_buckets[i].m_switchover_node);
          // Calculate m_max_fully_acked_gci for start_resend
          resend_buckets.set(i);
          if (c_buckets[i].m_max_acked_gci < m_max_fully_acked_gci) {
            m_max_fully_acked_gci = c_buckets[i].m_max_acked_gci;
          }
        }
      } else if (get_responsible_node(i, tmp) == getOwnNodeId()) {
        // Calculate m_max_fully_acked_gci for start_resend
        resend_buckets.set(i);
        if (c_buckets[i].m_max_acked_gci < m_max_fully_acked_gci) {
          m_max_fully_acked_gci = c_buckets[i].m_max_acked_gci;
        }
      }
    }
    // Start resend
    for (Uint32 i = 0; i < c_no_of_buckets; i++) {
      if (resend_buckets.get(i)) {
        if (ERROR_INSERTED(13052)) {
          jam();
          g_eventLogger->info(
              "Suma::execNODE_FAILREP cleared error "
              "insertion 13052 simulating OOB");
          CLEAR_ERROR_INSERT_VALUE;
          out_of_buffer(signal);
        }

        start_resend(signal, i);
      }
    }
  }

  /* Block level cleanup */
  for (unsigned i = 1; i < MAX_NDB_NODES; i++) {
    jam();
    if (failed.get(i)) {
      jam();
      Uint32 elementsCleaned = simBlockNodeFailure(signal, i);  // No callback
      ndbassert(elementsCleaned ==
                0);           // As Suma has no remote fragmented signals
      (void)elementsCleaned;  // Avoid compiler error
    }                         // if
  }                           // for

  c_alive_nodes.assign(tmp);

  DBUG_VOID_RETURN;
}

void Suma::execINCL_NODEREQ(Signal *signal) {
  jamEntry();

  const Uint32 senderRef = signal->theData[0];
  const Uint32 nodeId = signal->theData[1];

  ndbrequire(!c_alive_nodes.get(nodeId));
  if (c_nodes_in_nodegroup_mask.get(nodeId)) {
    /**
     *
     * XXX TODO: This should be removed
     *           But, other nodes are (incorrectly) reported as started
     *                even if they're not "started", but only INCL_NODEREQ'ed
     */
    c_alive_nodes.set(nodeId);

    /**
     *
     * Nodes in nodegroup will be "alive" when
     *   sending SUMA_HANDOVER_REQ
     */
  } else {
    jam();
    c_alive_nodes.set(nodeId);
  }

  signal->theData[0] = nodeId;
  signal->theData[1] = reference();
  sendSignal(senderRef, GSN_INCL_NODECONF, signal, 2, JBB);
}

void Suma::execSIGNAL_DROPPED_REP(Signal *signal) {
  jamEntry();
  ndbabort();
}

/********************************************************************
 *
 * Dump state
 *
 */
static const char *cstr(Suma::Subscription::State s) {
  switch (s) {
    case Suma::Subscription::UNDEFINED:
      return "undefined";
    case Suma::Subscription::DEFINED:
      return "defined";
    case Suma::Subscription::DEFINING:
      return "defining";
  }
  return "<unknown>";
}

static const char *cstr(Suma::Subscription::TriggerState s) {
  switch (s) {
    case Suma::Subscription::T_UNDEFINED:
      return "undefined";
    case Suma::Subscription::T_CREATING:
      return "creating";
    case Suma::Subscription::T_DEFINED:
      return "defined";
    case Suma::Subscription::T_DROPPING:
      return "dropping";
    case Suma::Subscription::T_ERROR:
      return "error";
  }
  return "<uknown>";
}

static const char *cstr(Suma::Subscription::Options s) {
  static char buf[256];
  buf[0] = 0;
  strcat(buf, "[");
  if (s & Suma::Subscription::REPORT_ALL) strcat(buf, " reportall");
  if (s & Suma::Subscription::REPORT_SUBSCRIBE) strcat(buf, " reportsubscribe");
  if (s & Suma::Subscription::MARKED_DROPPED) strcat(buf, " dropped");
  if (s & Suma::Subscription::NO_REPORT_DDL) strcat(buf, " noreportddl");
  strcat(buf, " ]");
  return buf;
}

static const char *cstr(Suma::Table::State s) {
  switch (s) {
    case Suma::Table::UNDEFINED:
      return "undefined";
    case Suma::Table::DEFINING:
      return "defining";
    case Suma::Table::DEFINED:
      return "defined";
    case Suma::Table::DROPPED:
      return "dropped";
  }
  return "<unknown>";
}

void Suma::execDUMP_STATE_ORD(Signal *signal) {
  jamEntry();

  Uint32 tCase = signal->theData[0];
#if 0
  if(tCase >= 8000 && tCase <= 8003){
    SubscriptionPtr subPtr;
    c_subscriptions.getPtr(subPtr, g_subPtrI);
    
    Ptr<SyncRecord> syncPtr;
    ndbrequire(c_syncPool.getPtr(syncPtr, subPtr.p->m_syncPtrI));

    if(tCase == 8000){
      syncPtr.p->startMeta(signal);
    }
    
    if(tCase == 8001){
      syncPtr.p->startScan(signal);
    }

    if(tCase == 8002){
      syncPtr.p->startTrigger(signal);
    }
    
    if(tCase == 8003){
      subPtr.p->m_subscriptionType = SubCreateReq::SingleTableScan;
      LocalSyncRecordBuffer attrs(c_dataBufferPool, syncPtr.p->m_attributeList);
      Uint32 tab = 0;
      Uint32 att[] = { 0, 1, 1 };
      syncPtr.p->m_tableList.append(&tab, 1);
      attrs.append(att, 3);
    }
  }
#endif
  if (tCase == 8004) {
    infoEvent("Suma: c_subscriberPool  size: %d free: %d",
              c_subscriberPool.getSize(), c_subscriberPool.getNoOfFree());

    infoEvent("Suma: c_tablePool  size: %d free: %d", c_tablePool.getSize(),
              c_tablePool.getNoOfFree());

    infoEvent("Suma: c_subscriptionPool  size: %d free: %d",
              c_subscriptionPool.getSize(), c_subscriptionPool.getNoOfFree());

    infoEvent("Suma: c_syncPool  size: %d free: %d", c_syncPool.getSize(),
              c_syncPool.getNoOfFree());

    infoEvent("Suma: c_dataBufferPool  size: %d free: %d",
              c_dataBufferPool.getSize(), c_dataBufferPool.getNoOfFree());

    infoEvent("Suma: c_subOpPool  size: %d free: %d", c_subOpPool.getSize(),
              c_subOpPool.getNoOfFree());

#if 0
    infoEvent("Suma: c_dataSubscribers count: %d",
	      count_subscribers(c_dataSubscribers));
    infoEvent("Suma: c_prepDataSubscribers count: %d",
	      count_subscribers(c_prepDataSubscribers));
#endif
  }

  if (tCase == 8005) {
    for (Uint32 i = 0; i < c_no_of_buckets; i++) {
      Bucket *ptr = c_buckets + i;
      infoEvent(
          "Bucket %d %d%d-%x switch gci: %llu max_acked_gci: %llu max_gci: "
          "%llu tail: %d head: %d",
          i, m_active_buckets.get(i), m_switchover_buckets.get(i), ptr->m_state,
          ptr->m_switchover_gci, ptr->m_max_acked_gci,
          ptr->m_buffer_head.m_max_gci, ptr->m_buffer_tail,
          ptr->m_buffer_head.m_page_id);
    }
  }

  if (tCase == 8006) {
    SET_ERROR_INSERT_VALUE(13029);
  }

  if (tCase == 8007) {
    c_startup.m_restart_server_node_id = MAX_NDB_NODES + 1;
    SET_ERROR_INSERT_VALUE(13029);
  }

  if (tCase == 8008) {
    CLEAR_ERROR_INSERT_VALUE;
  }

  if (tCase == 8010) {
    char buf1[NodeBitmask::TextLength + 1];
    char buf2[NodeBitmask::TextLength + 1];
    c_subscriber_nodes.getText(buf1);
    c_connected_nodes.getText(buf2);
    infoEvent("c_subscriber_nodes: %s", buf1);
    infoEvent("c_connected_nodes: %s", buf2);
  }

  if (tCase == 8009) {
    if (ERROR_INSERTED(13030)) {
      CLEAR_ERROR_INSERT_VALUE;
      sendSTTORRY(signal);
    } else {
      SET_ERROR_INSERT_VALUE(13030);
    }
    return;
  }

  if (tCase == 8011) {
    jam();
    Uint32 bucket = signal->theData[1];
    Table_keyhash::Iterator it;
    if (signal->getLength() == 1) {
      jam();
      bucket = 0;
      infoEvent("-- Starting dump of subscribers --");
    }

    c_tables.next(bucket, it);
    const Uint32 RT_BREAK = 16;
    for (Uint32 i = 0; i < RT_BREAK || it.bucket == bucket; i++) {
      jam();
      if (it.curr.i == RNIL) {
        jam();
        infoEvent("-- Ending dump of subscribers --");
        return;
      }

      infoEvent("Table %u ver %u", it.curr.p->m_tableId,
                it.curr.p->m_schemaVersion);

      Uint32 cnt = 0;
      Ptr<Subscription> subPtr;
      Local_Subscription_list subList(c_subscriptionPool,
                                      it.curr.p->m_subscriptions);
      for (subList.first(subPtr); !subPtr.isNull(); subList.next(subPtr)) {
        infoEvent(" Subcription %u", subPtr.i);
        {
          Ptr<Subscriber> ptr;
          Local_Subscriber_list list(c_subscriberPool, subPtr.p->m_subscribers);
          for (list.first(ptr); !ptr.isNull(); list.next(ptr), i++) {
            jam();
            cnt++;
            infoEvent("  Subscriber [ %x %u %x %u ]", ptr.p->m_senderRef,
                      ptr.p->m_senderData, ptr.p->m_requestInfo, subPtr.i);
          }
        }

        {
          Ptr<SubOpRecord> ptr;
          Local_SubOpRecord_fifo list(c_subOpPool, subPtr.p->m_create_req);

          for (list.first(ptr); !ptr.isNull(); list.next(ptr), i++) {
            jam();
            infoEvent("  create [ %x %u ]", ptr.p->m_senderRef,
                      ptr.p->m_senderData);
          }
        }

        {
          Ptr<SubOpRecord> ptr;
          Local_SubOpRecord_fifo list(c_subOpPool, subPtr.p->m_start_req);

          for (list.first(ptr); !ptr.isNull(); list.next(ptr), i++) {
            jam();
            infoEvent("  start [ %x %u ]", ptr.p->m_senderRef,
                      ptr.p->m_senderData);
          }
        }

        {
          Ptr<SubOpRecord> ptr;
          Local_SubOpRecord_fifo list(c_subOpPool, subPtr.p->m_stop_req);

          for (list.first(ptr); !ptr.isNull(); list.next(ptr), i++) {
            jam();
            infoEvent("  stop [ %u %x %u ]", ptr.p->m_opType,
                      ptr.p->m_senderRef, ptr.p->m_senderData);
          }
        }
      }
      infoEvent("Table %u #subscribers %u", it.curr.p->m_tableId, cnt);
      c_tables.next(it);
    }

    signal->theData[0] = tCase;
    signal->theData[1] = it.bucket;
    sendSignalWithDelay(reference(), GSN_DUMP_STATE_ORD, signal, 100, 2);
    return;
  }

  if (tCase == 8012) {
    jam();
    Uint32 bucket = signal->theData[1];
    Subscription_keyhash::Iterator it;
    if (signal->getLength() == 1) {
      jam();
      bucket = 0;
      infoEvent("-- Starting dump of subscribers --");
    }

    c_subscriptions.next(bucket, it);
    const Uint32 RT_BREAK = 16;
    for (Uint32 i = 0; i < RT_BREAK || it.bucket == bucket; i++) {
      jam();
      if (it.curr.i == RNIL) {
        jam();
        infoEvent("-- Ending dump of subscribers --");
        return;
      }

      Ptr<Subscription> subPtr = it.curr;
      Ptr<Table> tabPtr;
      ndbrequire(c_tablePool.getPtr(tabPtr, subPtr.p->m_table_ptrI));
      infoEvent("Subcription %u id: 0x%.8x key: 0x%.8x state: %s", subPtr.i,
                subPtr.p->m_subscriptionId, subPtr.p->m_subscriptionKey,
                cstr(subPtr.p->m_state));
      infoEvent("  trigger state: %s options: %s",
                cstr(subPtr.p->m_trigger_state),
                cstr((Suma::Subscription::Options)subPtr.p->m_options));
      infoEvent("  tablePtr: %u tableId: %u schemaVersion: 0x%.8x state: %s",
                tabPtr.i, subPtr.p->m_tableId, tabPtr.p->m_schemaVersion,
                cstr(tabPtr.p->m_state));
      {
        Ptr<Subscriber> ptr;
        Local_Subscriber_list list(c_subscriberPool, subPtr.p->m_subscribers);
        for (list.first(ptr); !ptr.isNull(); list.next(ptr), i++) {
          jam();
          infoEvent("  Subscriber [ %x %u %x %u ]", ptr.p->m_senderRef,
                    ptr.p->m_senderData, ptr.p->m_requestInfo, subPtr.i);
        }
      }

      {
        Ptr<SubOpRecord> ptr;
        Local_SubOpRecord_fifo list(c_subOpPool, subPtr.p->m_create_req);

        for (list.first(ptr); !ptr.isNull(); list.next(ptr), i++) {
          jam();
          infoEvent("  create [ %x %u ]", ptr.p->m_senderRef,
                    ptr.p->m_senderData);
        }
      }

      {
        Ptr<SubOpRecord> ptr;
        Local_SubOpRecord_fifo list(c_subOpPool, subPtr.p->m_start_req);

        for (list.first(ptr); !ptr.isNull(); list.next(ptr), i++) {
          jam();
          infoEvent("  start [ %x %u ]", ptr.p->m_senderRef,
                    ptr.p->m_senderData);
        }
      }

      {
        Ptr<SubOpRecord> ptr;
        Local_SubOpRecord_fifo list(c_subOpPool, subPtr.p->m_stop_req);

        for (list.first(ptr); !ptr.isNull(); list.next(ptr), i++) {
          jam();
          infoEvent("  stop [ %u %x %u ]", ptr.p->m_opType, ptr.p->m_senderRef,
                    ptr.p->m_senderData);
        }
      }
      c_subscriptions.next(it);
    }

    signal->theData[0] = tCase;
    signal->theData[1] = it.bucket;
    sendSignalWithDelay(reference(), GSN_DUMP_STATE_ORD, signal, 100, 2);
    return;
  }

  if (tCase == 8013) {
    jam();
    Ptr<Gcp_record> gcp;
    infoEvent("-- Starting dump of pending subscribers --");
    infoEvent("Highest epoch %llu, oldest epoch %llu", m_max_seen_gci,
              m_last_complete_gci);
    if (!c_gcp_list.isEmpty()) {
      jam();
      c_gcp_list.first(gcp);
      infoEvent("Waiting for acknowledge of epoch %llu, buffering %u epochs",
                gcp.p->m_gci, c_gcp_list.getCount());
      NodeBitmask subs = gcp.p->m_subscribers;
      for (Uint32 nodeId = 0; nodeId < MAX_NODES; nodeId++) {
        if (subs.get(nodeId)) {
          jam();
          infoEvent("Waiting for subscribing node %u", nodeId);
        }
      }
    }
    infoEvent("-- End dump of pending subscribers --");
  }

  if (tCase == DumpStateOrd::DihTcSumaNodeFailCompleted &&
      signal->getLength() == 2) {
    jam();
    Uint32 nodeId = signal->theData[1];
    if (nodeId < MAX_NODES) {
      warningEvent(" Suma %u %u line: %u", tCase, nodeId,
                   c_failedApiNodesState[nodeId]);
      warningEvent("   c_connected_nodes.get(): %u",
                   c_connected_nodes.get(nodeId));
      warningEvent("   c_failedApiNodes.get(): %u",
                   c_failedApiNodes.get(nodeId));
      warningEvent("   c_subscriber_nodes.get(): %u",
                   c_subscriber_nodes.get(nodeId));
      warningEvent(" c_subscriber_per_node[%u]: %u", nodeId,
                   c_subscriber_per_node[nodeId]);
    } else {
      warningEvent(" SUMA: dump-%u to unknown node: %u", tCase, nodeId);
    }
  }
}

void Suma::execDBINFO_SCANREQ(Signal *signal) {
  DbinfoScanReq req = *(DbinfoScanReq *)signal->theData;
  const Ndbinfo::ScanCursor *cursor =
      CAST_CONSTPTR(Ndbinfo::ScanCursor, DbinfoScan::getCursorPtr(&req));
  Ndbinfo::Ratelimit rl;

  jamEntry();

  switch (req.tableId) {
    case Ndbinfo::POOLS_TABLEID: {
      Ndbinfo::pool_entry pools[] = {
          {"Subscriber",
           c_subscriberPool.getUsed(),
           c_subscriberPool.getSize(),
           c_subscriberPool.getEntrySize(),
           c_subscriberPool.getUsedHi(),
           {CFG_DB_SUBSCRIBERS, CFG_DB_SUBSCRIPTIONS, CFG_DB_NO_TABLES, 0},
           0},
          {"Table",
           c_tablePool.getUsed(),
           c_tablePool.getSize(),
           c_tablePool.getEntrySize(),
           c_tablePool.getUsedHi(),
           {CFG_DB_NO_TABLES, 0, 0, 0},
           0},
          {"Subscription",
           c_subscriptionPool.getUsed(),
           c_subscriptionPool.getSize(),
           c_subscriptionPool.getEntrySize(),
           c_subscriptionPool.getUsedHi(),
           {CFG_DB_SUBSCRIPTIONS, CFG_DB_NO_TABLES, 0, 0},
           0},
          {"Sync",
           c_syncPool.getUsed(),
           c_syncPool.getSize(),
           c_syncPool.getEntrySize(),
           c_syncPool.getUsedHi(),
           {0, 0, 0, 0},
           0},
          {"Data Buffer",
           c_dataBufferPool.getUsed(),
           c_dataBufferPool.getSize(),
           c_dataBufferPool.getEntrySize(),
           c_dataBufferPool.getUsedHi(),
           {CFG_DB_NO_ATTRIBUTES, 0, 0, 0},
           0},
          {"SubOp",
           c_subOpPool.getUsed(),
           c_subOpPool.getSize(),
           c_subOpPool.getEntrySize(),
           c_subOpPool.getUsedHi(),
           {CFG_DB_SUB_OPERATIONS, 0, 0, 0},
           0},
          {"Page Chunk",
           c_page_chunk_pool.getUsed(),
           c_page_chunk_pool.getSize(),
           c_page_chunk_pool.getEntrySize(),
           c_page_chunk_pool.getUsedHi(),
           {0, 0, 0, 0},
           0},
          {"GCP",
           c_gcp_pool.getUsed(),
           c_gcp_pool.getSize(),
           c_gcp_pool.getEntrySize(),
           c_gcp_pool.getUsedHi(),
           {CFG_DB_API_HEARTBEAT_INTERVAL, CFG_DB_GCP_INTERVAL, 0, 0},
           0},
          {NULL, 0, 0, 0, 0, {0, 0, 0, 0}, 0}};

      const size_t num_config_params =
          sizeof(pools[0].config_params) / sizeof(pools[0].config_params[0]);
      const Uint32 numPools = NDB_ARRAY_SIZE(pools);
      Uint32 pool = cursor->data[0];
      ndbrequire(pool < numPools);
      BlockNumber bn = blockToMain(number());
      while (pools[pool].poolname) {
        jam();
        Ndbinfo::Row row(signal, req);
        row.write_uint32(getOwnNodeId());
        row.write_uint32(bn);          // block number
        row.write_uint32(instance());  // block instance
        row.write_string(pools[pool].poolname);
        row.write_uint64(pools[pool].used);
        row.write_uint64(pools[pool].total);
        row.write_uint64(pools[pool].used_hi);
        row.write_uint64(pools[pool].entry_size);
        for (size_t i = 0; i < num_config_params; i++)
          row.write_uint32(pools[pool].config_params[i]);
        row.write_uint32(GET_RG(pools[pool].record_type));
        row.write_uint32(GET_TID(pools[pool].record_type));
        ndbinfo_send_row(signal, req, row, rl);
        pool++;
        if (rl.need_break(req)) {
          jam();
          ndbinfo_send_scan_break(signal, req, rl, pool);
          return;
        }
      }
      break;
    }
    default:
      break;
  }

  ndbinfo_send_scan_conf(signal, req, rl);
}

/*************************************************************
 *
 * Creation of subscription id's
 *
 ************************************************************/

void Suma::execCREATE_SUBID_REQ(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execCREATE_SUBID_REQ");
  ndbassert(signal->getNoOfSections() == 0);
  CRASH_INSERTION(13001);

  CreateSubscriptionIdReq const *req =
      (CreateSubscriptionIdReq *)signal->getDataPtr();
  SubscriberPtr subbPtr;
  if (!c_subscriberPool.seize(subbPtr)) {
    jam();
    sendSubIdRef(signal, req->senderRef, req->senderData, 1412);
    DBUG_VOID_RETURN;
  }
  DBUG_PRINT("info",
             ("c_subscriberPool  size: %d free: %d", c_subscriberPool.getSize(),
              c_subscriberPool.getNoOfFree()));

  subbPtr.p->m_senderRef = req->senderRef;
  subbPtr.p->m_senderData = req->senderData;

  UtilSequenceReq *utilReq = (UtilSequenceReq *)signal->getDataPtrSend();
  utilReq->senderData = subbPtr.i;
  utilReq->sequenceId = SUMA_SEQUENCE;
  utilReq->requestType = UtilSequenceReq::NextVal;
  sendSignal(DBUTIL_REF, GSN_UTIL_SEQUENCE_REQ, signal,
             UtilSequenceReq::SignalLength, JBB);

  DBUG_VOID_RETURN;
}

void Suma::execUTIL_SEQUENCE_CONF(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execUTIL_SEQUENCE_CONF");
  ndbassert(signal->getNoOfSections() == 0);
  CRASH_INSERTION(13002);

  UtilSequenceConf *conf = (UtilSequenceConf *)signal->getDataPtr();
  if (conf->requestType == UtilSequenceReq::Create) {
    jam();
    createSequenceReply(signal, conf, NULL);
    DBUG_VOID_RETURN;
  }

  Uint64 subId;
  memcpy(&subId, conf->sequenceValue, 8);
  SubscriberPtr subbPtr;
  ndbrequire(c_subscriberPool.getPtr(subbPtr, conf->senderData));

  CreateSubscriptionIdConf *subconf = (CreateSubscriptionIdConf *)conf;
  subconf->senderRef = reference();
  subconf->senderData = subbPtr.p->m_senderData;
  subconf->subscriptionId = (Uint32)subId;
  subconf->subscriptionKey = (getOwnNodeId() << 16) | (Uint32)(subId & 0xFFFF);

  sendSignal(subbPtr.p->m_senderRef, GSN_CREATE_SUBID_CONF, signal,
             CreateSubscriptionIdConf::SignalLength, JBB);

  c_subscriberPool.release(subbPtr);
  DBUG_PRINT("info",
             ("c_subscriberPool  size: %d free: %d", c_subscriberPool.getSize(),
              c_subscriberPool.getNoOfFree()));
  DBUG_VOID_RETURN;
}

void Suma::execUTIL_SEQUENCE_REF(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execUTIL_SEQUENCE_REF");
  ndbassert(signal->getNoOfSections() == 0);
  UtilSequenceRef *ref = (UtilSequenceRef *)signal->getDataPtr();
  Uint32 err = ref->errorCode;

  if (ref->requestType == UtilSequenceReq::Create) {
    jam();
    createSequenceReply(signal, NULL, ref);
    DBUG_VOID_RETURN;
  }

  Uint32 subData = ref->senderData;

  SubscriberPtr subbPtr;
  ndbrequire(c_subscriberPool.getPtr(subbPtr, subData));
  if (err == UtilSequenceRef::TCError) {
    jam();
    err = ref->TCErrorCode;
  }
  sendSubIdRef(signal, subbPtr.p->m_senderRef, subbPtr.p->m_senderData, err);
  c_subscriberPool.release(subbPtr);
  DBUG_PRINT("info",
             ("c_subscriberPool  size: %d free: %d", c_subscriberPool.getSize(),
              c_subscriberPool.getNoOfFree()));
  DBUG_VOID_RETURN;
}  // execUTIL_SEQUENCE_REF()

void Suma::sendSubIdRef(Signal *signal, Uint32 senderRef, Uint32 senderData,
                        Uint32 errCode) {
  jam();
  DBUG_ENTER("Suma::sendSubIdRef");
  CreateSubscriptionIdRef *ref =
      (CreateSubscriptionIdRef *)signal->getDataPtrSend();

  ref->senderRef = reference();
  ref->senderData = senderData;
  ref->errorCode = errCode;
  sendSignal(senderRef, GSN_CREATE_SUBID_REF, signal,
             CreateSubscriptionIdRef::SignalLength, JBB);

  DBUG_VOID_RETURN;
}

/**********************************************************
 * Suma participant interface
 *
 * Creation of subscriptions
 */
void Suma::execSUB_CREATE_REQ(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execSUB_CREATE_REQ");
  ndbassert(signal->getNoOfSections() == 0);
  CRASH_INSERTION(13003);

  const SubCreateReq req = *(SubCreateReq *)signal->getDataPtr();

  const Uint32 senderRef = req.senderRef;
  const Uint32 senderData = req.senderData;
  const Uint32 subId = req.subscriptionId;
  const Uint32 subKey = req.subscriptionKey;
  const Uint32 type = req.subscriptionType & SubCreateReq::RemoveFlags;
  const Uint32 flags = req.subscriptionType & SubCreateReq::GetFlags;
  const Uint32 reportAll =
      (flags & SubCreateReq::ReportAll) ? Subscription::REPORT_ALL : 0;
  const Uint32 reportSubscribe = (flags & SubCreateReq::ReportSubscribe)
                                     ? Subscription::REPORT_SUBSCRIBE
                                     : 0;
  const Uint32 noReportDDL =
      (flags & SubCreateReq::NoReportDDL) ? Subscription::NO_REPORT_DDL : 0;
  const Uint32 tableId = req.tableId;
  const Uint32 schemaTransId = req.schemaTransId;

  bool subDropped = req.subscriptionType & SubCreateReq::NR_Sub_Dropped;

  /**
   * This 2 options are only allowed during NR
   */
  if (subDropped) {
    ndbrequire(refToNode(senderRef) == c_startup.m_restart_server_node_id);
  }

  Subscription key;
  key.m_subscriptionId = subId;
  key.m_subscriptionKey = subKey;

  DBUG_PRINT("enter", ("key.m_subscriptionId: %u, key.m_subscriptionKey: %u",
                       key.m_subscriptionId, key.m_subscriptionKey));

  SubscriptionPtr subPtr;

  bool found = c_subscriptions.find(subPtr, key);

  if (c_startup.m_restart_server_node_id == RNIL) {
    jam();

    /**
     * We haven't started syncing yet
     */
    sendSubCreateRef(signal, senderRef, senderData, SubCreateRef::NotStarted);
    return;
  }

  CRASH_INSERTION2(13040, c_startup.m_restart_server_node_id != RNIL);
  CRASH_INSERTION(13041);

  bool allowDup = true;  // c_startup.m_restart_server_node_id;

  if (found && !allowDup) {
    jam();
    sendSubCreateRef(signal, senderRef, senderData,
                     SubCreateRef::SubscriptionAlreadyExist);
    return;
  }

  if (found == false) {
    jam();
    if (!c_subscriptions.seize(subPtr)) {
      jam();
      sendSubCreateRef(signal, senderRef, senderData,
                       SubCreateRef::OutOfSubscriptionRecords);
      return;
    }

    new (subPtr.p) Subscription();
    subPtr.p->m_seq_no = c_current_seq;
    subPtr.p->m_subscriptionId = subId;
    subPtr.p->m_subscriptionKey = subKey;
    subPtr.p->m_subscriptionType = type;
    subPtr.p->m_tableId = tableId;
    subPtr.p->m_table_ptrI = RNIL;
    subPtr.p->m_state = Subscription::UNDEFINED;
    subPtr.p->m_trigger_state = Subscription::T_UNDEFINED;
    subPtr.p->m_triggers[0] = ILLEGAL_TRIGGER_ID;
    subPtr.p->m_triggers[1] = ILLEGAL_TRIGGER_ID;
    subPtr.p->m_triggers[2] = ILLEGAL_TRIGGER_ID;
    subPtr.p->m_errorCode = 0;
    subPtr.p->m_options = reportSubscribe | reportAll | noReportDDL;
    subPtr.p->m_schemaTransId = schemaTransId;
  }

  Ptr<SubOpRecord> subOpPtr;
  Local_SubOpRecord_fifo subOpList(c_subOpPool, subPtr.p->m_create_req);
  if ((ERROR_INSERTED(13044) && found == false) ||
      subOpList.seizeLast(subOpPtr) == false) {
    jam();
    if (found == false) {
      jam();
      if (ERROR_INSERTED(13044)) {
        CLEAR_ERROR_INSERT_VALUE;
      }
      c_subscriptionPool.release(subPtr);  // not yet in hash
    }
    sendSubCreateRef(signal, senderRef, senderData,
                     SubCreateRef::OutOfTableRecords);
    return;
  }

  subOpPtr.p->m_senderRef = senderRef;
  subOpPtr.p->m_senderData = senderData;

  if (subDropped) {
    jam();
    subPtr.p->m_options |= Subscription::MARKED_DROPPED;
  }

  TablePtr tabPtr;
  if (found) {
    jam();
    ndbrequire(c_tablePool.getPtr(tabPtr, subPtr.p->m_table_ptrI));
  } else if (c_tables.find(tabPtr, tableId)) {
    jam();
  } else {
    jam();
    if (ERROR_INSERTED(13045) || c_tablePool.seize(tabPtr) == false) {
      jam();
      if (ERROR_INSERTED(13045)) {
        CLEAR_ERROR_INSERT_VALUE;
      }

      subOpList.release(subOpPtr);
      c_subscriptionPool.release(subPtr);  // not yet in hash
      sendSubCreateRef(signal, senderRef, senderData,
                       SubCreateRef::OutOfTableRecords);
      return;
    }

    new (tabPtr.p) Table;
    tabPtr.p->m_tableId = tableId;
    tabPtr.p->m_ptrI = tabPtr.i;
    tabPtr.p->m_error = 0;
    tabPtr.p->m_schemaVersion = RNIL;
    tabPtr.p->m_state = Table::UNDEFINED;
    tabPtr.p->m_schemaTransId = schemaTransId;
    c_tables.add(tabPtr);
  }

  if (found == false) {
    jam();
    c_subscriptions.add(subPtr);
    Local_Subscription_list list(c_subscriptionPool, tabPtr.p->m_subscriptions);
    list.addFirst(subPtr);
    subPtr.p->m_table_ptrI = tabPtr.i;
  }

  switch (tabPtr.p->m_state) {
    case Table::DEFINED: {
      jam();
      // Send conf
      subOpList.release(subOpPtr);
      subPtr.p->m_state = Subscription::DEFINED;
      SubCreateConf *const conf = (SubCreateConf *)signal->getDataPtrSend();
      conf->senderRef = reference();
      conf->senderData = senderData;
      sendSignal(senderRef, GSN_SUB_CREATE_CONF, signal,
                 SubCreateConf::SignalLength, JBB);
      return;
    }
    case Table::UNDEFINED: {
      jam();
      tabPtr.p->m_state = Table::DEFINING;
      subPtr.p->m_state = Subscription::DEFINING;

      if (ERROR_INSERTED(13031)) {
        jam();
        CLEAR_ERROR_INSERT_VALUE;
        GetTabInfoRef *ref = (GetTabInfoRef *)signal->getDataPtrSend();
        ref->tableId = tableId;
        ref->senderData = tabPtr.i;
        ref->errorCode = GetTabInfoRef::TableNotDefined;
        sendSignal(reference(), GSN_GET_TABINFOREF, signal,
                   GetTabInfoRef::SignalLength, JBB);
        return;
      }

      GetTabInfoReq *req = (GetTabInfoReq *)signal->getDataPtrSend();
      req->senderRef = reference();
      req->senderData = tabPtr.i;
      req->requestType =
          GetTabInfoReq::RequestById | GetTabInfoReq::LongSignalConf;
      req->tableId = tableId;
      req->schemaTransId = schemaTransId;

      sendSignal(DBDICT_REF, GSN_GET_TABINFOREQ, signal,
                 GetTabInfoReq::SignalLength, JBB);
      return;
    }
    case Table::DEFINING: {
      jam();
      /**
       * just wait for completion
       */
      subPtr.p->m_state = Subscription::DEFINING;
      return;
    }
    case Table::DROPPED: {
      subOpList.release(subOpPtr);

      {
        Local_Subscription_list list(c_subscriptionPool,
                                     tabPtr.p->m_subscriptions);
        list.remove(subPtr);
      }
      c_subscriptions.release(subPtr);

      sendSubCreateRef(signal, senderRef, senderData,
                       SubCreateRef::TableDropped);
      return;
    }
  }

  ndbabort();
}

void Suma::sendSubCreateRef(Signal *signal, Uint32 retRef, Uint32 data,
                            Uint32 errCode) {
  jam();
  SubCreateRef *ref = (SubCreateRef *)signal->getDataPtrSend();
  ref->errorCode = errCode;
  ref->senderData = data;
  sendSignal(retRef, GSN_SUB_CREATE_REF, signal, SubCreateRef::SignalLength,
             JBB);
  return;
}

/**********************************************************
 *
 * Setting upp trigger for subscription
 *
 */

void Suma::execSUB_SYNC_REQ(Signal *signal) {
  jamEntry();

  CRASH_INSERTION(13004);

  SubSyncReq *const req = (SubSyncReq *)signal->getDataPtr();

  SubscriptionPtr subPtr;
  Subscription key;
  key.m_subscriptionId = req->subscriptionId;
  key.m_subscriptionKey = req->subscriptionKey;

  SectionHandle handle(this, signal);
  if (!c_subscriptions.find(subPtr, key)) {
    jam();
    releaseSections(handle);
    sendSubSyncRef(signal, 1407);
    return;
  }

  Ptr<SyncRecord> syncPtr;
  bool seize_ret;
  {
    Local_SyncRecord_dllist list(c_syncPool, subPtr.p->m_syncRecords);
    seize_ret = list.seizeFirst(syncPtr);
  }
  if (!seize_ret) {
    jam();
    releaseSections(handle);
    sendSubSyncRef(signal, 1416);
    return;
  }

  new (syncPtr.p) Ptr<SyncRecord>;
  syncPtr.p->m_senderRef = req->senderRef;
  syncPtr.p->m_senderData = req->senderData;
  syncPtr.p->m_subscriptionPtrI = subPtr.i;
  syncPtr.p->ptrI = syncPtr.i;
  syncPtr.p->m_error = 0;
  syncPtr.p->m_requestInfo = req->requestInfo;
  syncPtr.p->m_frag_cnt = req->fragCount;
  syncPtr.p->m_frag_id = req->fragId;
  syncPtr.p->m_scan_batchsize = req->batchSize;
  syncPtr.p->m_tableId = subPtr.p->m_tableId;
  syncPtr.p->m_sourceInstance = RNIL;
  syncPtr.p->m_headersSection = RNIL;
  syncPtr.p->m_dataSection = RNIL;

  D("SUB_SYNC_REQ: tableId: " << syncPtr.p->m_tableId
                              << " fragId = " << req->fragId
                              << " fragCount = " << req->fragCount
                              << " reqinfo: " << hex << req->requestInfo);

  {
    jam();
    if (handle.m_cnt > 0) {
      SegmentedSectionPtr ptr;
      ndbrequire(handle.getSection(ptr, SubSyncReq::ATTRIBUTE_LIST));
      LocalSyncRecordBuffer attrBuf(c_dataBufferPool,
                                    syncPtr.p->m_attributeList);
      append(attrBuf, ptr, getSectionSegmentPool());
    }
    if (req->requestInfo & SubSyncReq::RangeScan) {
      jam();
      if (handle.m_cnt > 1) {
        SegmentedSectionPtr ptr;
        ndbrequire(handle.getSection(ptr, SubSyncReq::TUX_BOUND_INFO));
        LocalSyncRecordBuffer boundBuf(c_dataBufferPool,
                                       syncPtr.p->m_boundInfo);
        append(boundBuf, ptr, getSectionSegmentPool());
      }
    }
    releaseSections(handle);
  }

  /**
   * We need to gather fragment info
   */
  {
    jam();
    sendDIH_SCAN_TAB_REQ(signal, syncPtr.i, subPtr.p->m_tableId,
                         subPtr.p->m_schemaTransId);
    return;
  }
}

void Suma::sendDIH_SCAN_TAB_REQ(Signal *signal, Uint32 synPtrI, Uint32 tableId,
                                Uint32 schemaTransId) {
  DihScanTabReq *req = (DihScanTabReq *)signal->getDataPtrSend();
  req->senderRef = reference();
  req->senderData = synPtrI;
  req->tableId = tableId;
  req->schemaTransId = schemaTransId;
  req->jamBufferPtr = jamBuffer();
  EXECUTE_DIRECT_MT(DBDIH, GSN_DIH_SCAN_TAB_REQ, signal,
                    DihScanTabReq::SignalLength, 0);
  DihScanTabConf *conf = (DihScanTabConf *)signal->getDataPtr();
  Uint32 retCode = conf->senderData;
  conf->senderData = synPtrI;
  if (retCode == 0) {
    sendSignal(reference(), GSN_DIH_SCAN_TAB_CONF, signal,
               DihScanTabConf::SignalLength, JBB);
  } else {
    sendSignal(reference(), GSN_DIH_SCAN_TAB_REF, signal,
               DihScanTabRef::SignalLength, JBB);
  }
  return;
}

void Suma::sendSubSyncRef(Signal *signal, Uint32 errCode) {
  jam();
  SubSyncRef *ref = (SubSyncRef *)signal->getDataPtrSend();
  ref->errorCode = errCode;
  sendSignal(signal->getSendersBlockRef(), GSN_SUB_SYNC_REF, signal,
             SubSyncRef::SignalLength, JBB);
  return;
}

void Suma::execDIH_SCAN_TAB_REF(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execDIH_SCAN_TAB_REF");
  DihScanTabRef *ref = (DihScanTabRef *)signal->getDataPtr();
  switch ((DihScanTabRef::ErrorCode)ref->error) {
    case DihScanTabRef::ErroneousTableState:
      jam();
      if (ref->tableStatus == Dbdih::TabRecord::TS_CREATING) {
        const Uint32 tableId = ref->tableId;
        const Uint32 synPtrI = ref->senderData;
        const Uint32 schemaTransId = ref->schemaTransId;
        DihScanTabReq *req = (DihScanTabReq *)signal->getDataPtrSend();

        req->senderData = synPtrI;
        req->senderRef = reference();
        req->tableId = tableId;
        req->schemaTransId = schemaTransId;
        signal->theData[0] = SumaContinueB::WAIT_SCAN_TAB_REQ;
        signal->theData[1] = synPtrI;
        signal->theData[2] = tableId;
        signal->theData[3] = schemaTransId;
        sendSignalWithDelay(reference(), GSN_CONTINUEB, signal,
                            DihScanTabReq::RetryInterval, 4);
        DBUG_VOID_RETURN;
      }
      ndbabort();
    default:
      ndbabort();
  }

  DBUG_VOID_RETURN;
}

void Suma::execDIH_SCAN_TAB_CONF(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execDIH_SCAN_TAB_CONF");
  DihScanTabConf *conf = (DihScanTabConf *)signal->getDataPtr();
  const Uint32 tableId = conf->tableId;
  const Uint32 fragCount = conf->fragmentCount;
  const Uint32 scanCookie = conf->scanCookie;

  Ptr<SyncRecord> ptr;
  ndbrequire(c_syncPool.getPtr(ptr, conf->senderData));

  {
    LocalSyncRecordBuffer fragBuf(c_dataBufferPool, ptr.p->m_fragments);
    ndbrequire(fragBuf.getSize() == 0);
  }
  D("fragCount = " << fragCount << " m_frag_cnt = " << ptr.p->m_frag_cnt);
  ndbassert(fragCount >= ptr.p->m_frag_cnt);
  if (ptr.p->m_frag_cnt == 0) {
    jam();
    ptr.p->m_frag_cnt = fragCount;
  }
  ptr.p->m_scan_cookie = scanCookie;
  sendDIGETNODESREQ(signal, ptr.i, tableId, 0);
  return;
}

void Suma::sendDIGETNODESREQ(Signal *signal, Uint32 synPtrI, Uint32 tableId,
                             Uint32 fragNo) {
  Ptr<SyncRecord> ptr;
  ndbrequire(c_syncPool.getPtr(ptr, synPtrI));

  Uint32 loopCount = 0;
  for (; fragNo < ptr.p->m_frag_cnt; fragNo++) {
    loopCount++;
    DiGetNodesReq *const req = (DiGetNodesReq *)signal->getDataPtrSend();
    req->tableId = tableId;
    req->hashValue = fragNo;
    req->distr_key_indicator = ZTRUE;
    req->anyNode = 0;
    req->scan_indicator = ZTRUE;
    req->jamBufferPtr = jamBuffer();
    req->get_next_fragid_indicator = 0;
    EXECUTE_DIRECT_MT(DBDIH, GSN_DIGETNODESREQ, signal,
                      DiGetNodesReq::SignalLength, 0);

    jamEntry();
    DiGetNodesConf *conf = (DiGetNodesConf *)&signal->theData[0];
    Uint32 errCode = conf->zero;
    Uint32 instanceKey = conf->instanceKey;
    ndbrequire(instanceKey > 0);
    Uint32 nodeId = conf->nodes[0];
    Uint32 nodeCount = (conf->reqinfo & 0xFF) + 1;
    ndbrequire(errCode == 0);

    {
      LocalSyncRecordBuffer fragBuf(c_dataBufferPool, ptr.p->m_fragments);

      /**
       * Add primary node for fragment to list
       */
      FragmentDescriptor fd;
      fd.m_fragDesc.m_nodeId = nodeId;
      fd.m_fragDesc.m_fragmentNo = fragNo;
      fd.m_fragDesc.m_lqhInstanceKey = instanceKey;
      if (ptr.p->m_frag_id == ZNIL) {
        signal->theData[2] = fd.m_dummy[0];
        signal->theData[3] = fd.m_dummy[1];
        fragBuf.append(&signal->theData[2], 2);
      } else if (ptr.p->m_frag_id == fragNo) {
        /*
         * Given fragment must have a replica on this node.
         */
        const Uint32 ownNodeId = getOwnNodeId();
        Uint32 i = 0;
        for (i = 0; i < nodeCount; i++)
          if (conf->nodes[i] == ownNodeId) break;
        if (i == nodeCount) {
          sendSubSyncRef(signal, 1428);
          return;
        }
        fd.m_fragDesc.m_nodeId = ownNodeId;
        signal->theData[2] = fd.m_dummy[0];
        signal->theData[3] = fd.m_dummy[1];
        fragBuf.append(&signal->theData[2], 2);
      }
    }
    if (loopCount >= DiGetNodesReq::MAX_DIGETNODESREQS ||
        ERROR_INSERTED(13050)) {
      jam();
      if (ERROR_INSERTED(13050)) {
        CLEAR_ERROR_INSERT_VALUE;
      }
      signal->theData[0] = SumaContinueB::WAIT_GET_FRAGMENT;
      signal->theData[1] = ptr.i;
      signal->theData[2] = tableId;
      signal->theData[3] = fragNo + 1;
      sendSignal(reference(), GSN_CONTINUEB, signal, 4, JBB);
      return;
    }
    if (ERROR_INSERTED(13049) && ((fragNo + 1) == ptr.p->m_frag_cnt)) {
      jam();
      CLEAR_ERROR_INSERT_VALUE;
      signal->theData[0] = SumaContinueB::WAIT_GET_FRAGMENT;
      signal->theData[1] = ptr.i;
      signal->theData[2] = tableId;
      signal->theData[3] = fragNo + 1;
      sendSignal(reference(), GSN_CONTINUEB, signal, 4, JBB);
      return;
    }
  }
  jam();
  ptr.p->startScan(signal);
  return;
}

/**********************************************************
 * Dict interface
 */

/*************************************************************************
 *
 *
 */
void Suma::execGET_TABINFOREF(Signal *signal) {
  jamEntry();
  GetTabInfoRef *ref = (GetTabInfoRef *)signal->getDataPtr();
  Uint32 tableId = ref->tableId;
  Uint32 senderData = ref->senderData;
  Uint32 schemaTransId = ref->schemaTransId;
  GetTabInfoRef::ErrorCode errorCode = (GetTabInfoRef::ErrorCode)ref->errorCode;
  int do_resend_request = 0;
  TablePtr tabPtr;
  ndbrequire(c_tablePool.getPtr(tabPtr, senderData));
  switch (errorCode) {
    case GetTabInfoRef::TableNotDefined:
      // wrong state
      break;
    case GetTabInfoRef::InvalidTableId:
      // no such table
      break;
    case GetTabInfoRef::Busy:
      do_resend_request = 1;
      break;
    case GetTabInfoRef::NoFetchByName:
      jam();
      ndbabort();
    case GetTabInfoRef::TableNameTooLong:
      jam();
      ndbabort();
  }
  if (tabPtr.p->m_state == Table::DROPPED) {
    jam();
    do_resend_request = 0;
  }

  if (do_resend_request) {
    GetTabInfoReq *req = (GetTabInfoReq *)signal->getDataPtrSend();
    req->senderRef = reference();
    req->senderData = senderData;
    req->requestType =
        GetTabInfoReq::RequestById | GetTabInfoReq::LongSignalConf;
    req->tableId = tableId;
    req->schemaTransId = schemaTransId;
    sendSignalWithDelay(DBDICT_REF, GSN_GET_TABINFOREQ, signal, 30,
                        GetTabInfoReq::SignalLength);
    return;
  }
  get_tabinfo_ref_release(signal, tabPtr);
}

void Suma::get_tabinfo_ref_release(Signal *signal, Ptr<Table> tabPtr) {
  Local_Subscription_list subList(c_subscriptionPool,
                                  tabPtr.p->m_subscriptions);
  Ptr<Subscription> subPtr;
  ndbassert(!subList.isEmpty());
  for (subList.first(subPtr); !subPtr.isNull();) {
    jam();
    Ptr<SubOpRecord> ptr;
    ndbassert(subPtr.p->m_start_req.isEmpty());
    ndbassert(subPtr.p->m_stop_req.isEmpty());
    Local_SubOpRecord_fifo list(c_subOpPool, subPtr.p->m_create_req);
    for (list.first(ptr); !ptr.isNull();) {
      jam();
      sendSubCreateRef(signal, ptr.p->m_senderRef, ptr.p->m_senderData,
                       SubCreateRef::TableDropped);

      Ptr<SubOpRecord> tmp0 = ptr;
      list.next(ptr);
      list.release(tmp0);
    }
    Ptr<Subscription> tmp1 = subPtr;
    subList.next(subPtr);
    c_subscriptions.remove(tmp1);
    subList.release(tmp1);
  }

  c_tables.release(tabPtr);
}

void Suma::execGET_TABINFO_CONF(Signal *signal) {
  jamEntry();

  CRASH_INSERTION(13006);

  if (!assembleFragments(signal)) {
    return;
  }

  SectionHandle handle(this, signal);
  GetTabInfoConf *conf = (GetTabInfoConf *)signal->getDataPtr();
  TablePtr tabPtr;
  ndbrequire(c_tablePool.getPtr(tabPtr, conf->senderData));
  SegmentedSectionPtr ptr;
  ndbrequire(handle.getSection(ptr, GetTabInfoConf::DICT_TAB_INFO));
  ndbrequire(tabPtr.p->parseTable(ptr, *this));
  releaseSections(handle);

  if (tabPtr.p->m_state == Table::DROPPED) {
    jam();
    get_tabinfo_ref_release(signal, tabPtr);
    return;
  }

  tabPtr.p->m_state = Table::DEFINED;

  Local_Subscription_list subList(c_subscriptionPool,
                                  tabPtr.p->m_subscriptions);
  Ptr<Subscription> subPtr;
  ndbassert(!subList.isEmpty());
  for (subList.first(subPtr); !subPtr.isNull(); subList.next(subPtr)) {
    jam();
    subPtr.p->m_state = Subscription::DEFINED;

    Ptr<SubOpRecord> ptr;
    Local_SubOpRecord_fifo list(c_subOpPool, subPtr.p->m_create_req);
    for (list.first(ptr); !ptr.isNull();) {
      jam();
      SubCreateConf *const conf = (SubCreateConf *)signal->getDataPtrSend();
      conf->senderRef = reference();
      conf->senderData = ptr.p->m_senderData;
      sendSignal(ptr.p->m_senderRef, GSN_SUB_CREATE_CONF, signal,
                 SubCreateConf::SignalLength, JBB);

      Ptr<SubOpRecord> tmp = ptr;
      list.next(ptr);
      list.release(tmp);
    }
  }
}

bool Suma::Table::parseTable(SegmentedSectionPtr ptr, Suma &suma) {
  DBUG_ENTER("Suma::Table::parseTable");

  SimplePropertiesSectionReader it(ptr, suma.getSectionSegmentPool());

  SimpleProperties::UnpackStatus s;
  DictTabInfo::Table tableDesc;
  tableDesc.init();
  s = SimpleProperties::unpack(it, &tableDesc, DictTabInfo::TableMapping,
                               DictTabInfo::TableMappingSize);

  jamBlock(&suma);
  suma.suma_ndbrequire(s == SimpleProperties::Break);

  /**
   * Initialize table object
   */
  m_noOfAttributes = tableDesc.NoOfAttributes;
  m_schemaVersion = tableDesc.TableVersion;

  DBUG_RETURN(true);
}

/**********************************************************
 *
 * Scan interface
 *
 */

void Suma::SyncRecord::startScan(Signal *signal) {
  jam();
  DBUG_ENTER("Suma::SyncRecord::startScan");

  /**
   * Get fraginfo
   */
  m_currentFragment = 0;
  nextScan(signal);
  DBUG_VOID_RETURN;
}

bool Suma::SyncRecord::getNextFragment(TablePtr *tab, FragmentDescriptor *fd) {
  jam();
  SubscriptionPtr subPtr;
  suma.c_subscriptions.getPtr(subPtr, m_subscriptionPtrI);
  SyncRecordBuffer::DataBufferIterator fragIt;

  TablePtr tabPtr;
  ndbrequire(suma.c_tablePool.getPtr(tabPtr, subPtr.p->m_table_ptrI));
  LocalSyncRecordBuffer fragBuf(suma.c_dataBufferPool, m_fragments);

  fragBuf.position(fragIt, m_currentFragment);
  for (; !fragIt.curr.isNull(); fragBuf.next(fragIt), m_currentFragment += 2) {
    FragmentDescriptor tmp;
    tmp.m_dummy[0] = *fragIt.data;
    fragBuf.next(fragIt);
    tmp.m_dummy[1] = *fragIt.data;
    if (tmp.m_fragDesc.m_nodeId == suma.getOwnNodeId()) {
      *fd = tmp;
      *tab = tabPtr;
      return true;
    }
  }
  m_currentFragment = 0;
  return false;
}

void Suma::SyncRecord::nextScan(Signal *signal) {
  jam();
  DBUG_ENTER("Suma::SyncRecord::nextScan");
  TablePtr tabPtr;
  FragmentDescriptor fd;
  SubscriptionPtr subPtr;
  if (!getNextFragment(&tabPtr, &fd)) {
    jam();
    completeScan(signal);
    DBUG_VOID_RETURN;
  }

  suma.c_subscriptions.getPtr(subPtr, m_subscriptionPtrI);

  SyncRecordBuffer::Head head = m_attributeList;
  LocalSyncRecordBuffer attrBuf(suma.c_dataBufferPool, head);

  Uint32 instanceKey = fd.m_fragDesc.m_lqhInstanceKey;
  Uint32 instanceNo = suma.getInstanceNo(suma.getOwnNodeId(), instanceKey);
  BlockReference lqhRef = numberToRef(DBLQH, instanceNo, suma.getOwnNodeId());

  ScanFragReq *req = (ScanFragReq *)signal->getDataPtrSend();
  // const Uint32 attrLen = 5 + attrBuf.getSize();

  req->senderData = ptrI;
  req->resultRef = suma.reference();
  req->tableId = tabPtr.p->m_tableId;
  req->requestInfo = 0;
  req->savePointId = 0;
  ScanFragReq::setLockMode(req->requestInfo, 0);
  ScanFragReq::setHoldLockFlag(req->requestInfo, 1);
  ScanFragReq::setKeyinfoFlag(req->requestInfo, 0);
  if (m_requestInfo & SubSyncReq::NoDisk) {
    ScanFragReq::setNoDiskFlag(req->requestInfo, 1);
  }

  if (m_requestInfo & SubSyncReq::LM_Exclusive) {
    ScanFragReq::setLockMode(req->requestInfo, 1);
    ScanFragReq::setHoldLockFlag(req->requestInfo, 1);
    ScanFragReq::setKeyinfoFlag(req->requestInfo, 1);
  }

  if (m_requestInfo & SubSyncReq::ReorgDelete) {
    /* Ignore rows not moved in the reorg delete phase. */
    ScanFragReq::setReorgFlag(req->requestInfo, ScanFragReq::REORG_MOVED);
  }
  /**
   * We read all rows in the copy phase since there is no safe way of
   * ensuring that user transactions actually complete the transfer, they
   * can be aborted at any time. By copying all rows we have control over
   * the aborts and can retry until we succeed or we know to fail the
   * ALTER TABLE reorg.
   */

  if (m_requestInfo & SubSyncReq::TupOrder) {
    ScanFragReq::setTupScanFlag(req->requestInfo, 1);
  }

  if (m_requestInfo & SubSyncReq::LM_CommittedRead) {
    ScanFragReq::setReadCommittedFlag(req->requestInfo, 1);
  }

  if (m_requestInfo & SubSyncReq::RangeScan) {
    ScanFragReq::setRangeScanFlag(req->requestInfo, 1);
  }

  if (m_requestInfo & SubSyncReq::StatScan) {
    ScanFragReq::setStatScanFlag(req->requestInfo, 1);
  }

  req->fragmentNoKeyLen = fd.m_fragDesc.m_fragmentNo;
  req->schemaVersion = tabPtr.p->m_schemaVersion;
  req->transId1 = 0;
  req->transId2 = (SUMA << 20) + (suma.getOwnNodeId() << 8);
  req->clientOpPtr = (ptrI << 16);
  req->batch_size_rows = m_scan_batchsize;
  req->batch_size_bytes = m_scan_batchsize * MAX_NORMAL_ROW_SIZE;

  Uint32 *attrInfo = signal->theData + 25;
  attrInfo[0] = attrBuf.getSize();
  attrInfo[1] = 0;
  attrInfo[2] = 0;
  attrInfo[3] = 0;
  attrInfo[4] = 0;

  Uint32 pos = 5;
  SyncRecordBuffer::DataBufferIterator it;
  for (attrBuf.first(it); !it.curr.isNull(); attrBuf.next(it)) {
    AttributeHeader::init(&attrInfo[pos++], *it.data, 0);
  }
  LinearSectionPtr ptr[3];
  Uint32 noOfSections;
  ptr[0].p = attrInfo;
  ptr[0].sz = pos;
  noOfSections = 1;
  if (m_requestInfo & SubSyncReq::RangeScan) {
    jam();
    Uint32 oldpos = pos;  // after attrInfo
    LocalSyncRecordBuffer boundBuf(suma.c_dataBufferPool, m_boundInfo);
    for (boundBuf.first(it); !it.curr.isNull(); boundBuf.next(it)) {
      attrInfo[pos++] = *it.data;
    }
    if (pos > oldpos) {
      ptr[1].p = &attrInfo[oldpos];
      ptr[1].sz = pos - oldpos;
      noOfSections = 2;
    }
  }
  suma.sendSignal(lqhRef, GSN_SCAN_FRAGREQ, signal, ScanFragReq::SignalLength,
                  JBB, ptr, noOfSections);

  m_currentNoOfAttributes = attrBuf.getSize();

  DBUG_VOID_RETURN;
}

void Suma::execSCAN_FRAGREF(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execSCAN_FRAGREF");
  ScanFragRef *const ref = (ScanFragRef *)signal->getDataPtr();
  Ptr<SyncRecord> syncPtr;
  ndbrequire(c_syncPool.getPtr(syncPtr, ref->senderData));
  syncPtr.p->completeScan(signal, ref->errorCode);
  DBUG_VOID_RETURN;
}

void Suma::execSCAN_FRAGCONF(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execSCAN_FRAGCONF");
  ndbassert(signal->getNoOfSections() == 0);
  CRASH_INSERTION(13011);

  ScanFragConf *const conf = (ScanFragConf *)signal->getDataPtr();

  const Uint32 completed = conf->fragmentCompleted;
  const Uint32 senderData = conf->senderData;
  const Uint32 completedOps = conf->completedOps;

  Ptr<SyncRecord> syncPtr;
  ndbrequire(c_syncPool.getPtr(syncPtr, senderData));

  if (completed != 2) {  // 2==ZSCAN_FRAG_CLOSED
    jam();

#if PRINT_ONLY
    SubSyncContinueConf *const conf =
        (SubSyncContinueConf *)signal->getDataPtrSend();
    conf->subscriptionId = subPtr.p->m_subscriptionId;
    conf->subscriptionKey = subPtr.p->m_subscriptionKey;
    execSUB_SYNC_CONTINUE_CONF(signal);
#else
    SubSyncContinueReq *const req =
        (SubSyncContinueReq *)signal->getDataPtrSend();
    req->subscriberData = syncPtr.p->m_senderData;
    req->noOfRowsSent = completedOps;
    req->senderData = senderData;
    sendSignal(syncPtr.p->m_senderRef, GSN_SUB_SYNC_CONTINUE_REQ, signal,
               SubSyncContinueReq::SignalLength, JBB);
#endif
    DBUG_VOID_RETURN;
  }

  ndbrequire(completedOps == 0);

  syncPtr.p->m_currentFragment += 2;
  syncPtr.p->nextScan(signal);
  DBUG_VOID_RETURN;
}

void Suma::execSUB_SYNC_CONTINUE_CONF(Signal *signal) {
  jamEntry();
  ndbassert(signal->getNoOfSections() == 0);

  CRASH_INSERTION(13012);

  SubSyncContinueConf *const conf = (SubSyncContinueConf *)signal->getDataPtr();

  SubscriptionPtr subPtr;
  Subscription key;
  key.m_subscriptionId = conf->subscriptionId;
  key.m_subscriptionKey = conf->subscriptionKey;
  Uint32 syncPtrI = conf->senderData;

  ndbrequire(c_subscriptions.find(subPtr, key));

  Uint32 batchSize;
  Uint32 instanceKey;
  Uint32 instanceNo;
  {
    Ptr<SyncRecord> syncPtr;
    ndbrequire(c_syncPool.getPtr(syncPtr, syncPtrI));
    batchSize = syncPtr.p->m_scan_batchsize;
    LocalSyncRecordBuffer fragBuf(c_dataBufferPool, syncPtr.p->m_fragments);
    SyncRecordBuffer::DataBufferIterator fragIt;
    bool ok = fragBuf.position(fragIt, syncPtr.p->m_currentFragment);
    ndbrequire(ok);
    FragmentDescriptor tmp;
    tmp.m_dummy[0] = *fragIt.data;
    fragBuf.next(fragIt);
    tmp.m_dummy[1] = *fragIt.data;
    instanceKey = tmp.m_fragDesc.m_lqhInstanceKey;
    instanceNo = getInstanceNo(getOwnNodeId(), instanceKey);
  }
  BlockReference lqhRef = numberToRef(DBLQH, instanceNo, getOwnNodeId());

  ScanFragNextReq *req = (ScanFragNextReq *)signal->getDataPtrSend();
  req->senderData = syncPtrI;
  req->requestInfo = 0;
  req->transId1 = 0;
  req->transId2 = (SUMA << 20) + (getOwnNodeId() << 8);
  req->batch_size_rows = batchSize;
  req->batch_size_bytes = batchSize * MAX_NORMAL_ROW_SIZE;
  sendSignal(lqhRef, GSN_SCAN_NEXTREQ, signal, ScanFragNextReq::SignalLength,
             JBB);
}

void Suma::SyncRecord::completeScan(Signal *signal, int error) {
  jam();
  DBUG_ENTER("Suma::SyncRecord::completeScan");

  SubscriptionPtr subPtr;
  ndbrequire(suma.c_subscriptionPool.getPtr(subPtr, m_subscriptionPtrI));

  DihScanTabCompleteRep *rep = (DihScanTabCompleteRep *)signal->getDataPtr();
  rep->tableId = subPtr.p->m_tableId;
  rep->scanCookie = m_scan_cookie;
  rep->jamBufferPtr = jamBuffer();
  suma.EXECUTE_DIRECT_MT(DBDIH, GSN_DIH_SCAN_TAB_COMPLETE_REP, signal,
                         DihScanTabCompleteRep::SignalLength, 0);

#if PRINT_ONLY
  g_eventLogger->info("GSN_SUB_SYNC_CONF (data)");
#else
  if (error == 0) {
    SubSyncConf *const conf = (SubSyncConf *)signal->getDataPtrSend();
    conf->senderRef = suma.reference();
    conf->senderData = m_senderData;
    suma.sendSignal(m_senderRef, GSN_SUB_SYNC_CONF, signal,
                    SubSyncConf::SignalLength, JBB);
  } else {
    SubSyncRef *const ref = (SubSyncRef *)signal->getDataPtrSend();
    ref->senderRef = suma.reference();
    ref->senderData = m_senderData;
    ref->errorCode = error;
    suma.sendSignal(m_senderRef, GSN_SUB_SYNC_REF, signal,
                    SubSyncRef::SignalLength, JBB);
  }
#endif

  release();
  {
    Local_SyncRecord_dllist list(suma.c_syncPool, subPtr.p->m_syncRecords);
    Ptr<SyncRecord> tmp;
    tmp.i = ptrI;
    tmp.p = this;
    list.release(tmp);
  }

  DBUG_VOID_RETURN;
}

void Suma::execSCAN_HBREP(Signal *signal) {
  jamEntry();
#if 0
  ndbout << "execSCAN_HBREP" << endl << hex;
  for(int i = 0; i<signal->length(); i++){
    ndbout << signal->theData[i] << " ";
    if(((i + 1) % 8) == 0)
      ndbout << endl << hex;
  }
  ndbout << endl;
#endif
}

/**********************************************************
 *
 * Suma participant interface
 *
 * Creation of subscriber
 *
 */

void Suma::execSUB_START_REQ(Signal *signal) {
  jamEntry();
  ndbassert(signal->getNoOfSections() == 0);
  DBUG_ENTER("Suma::execSUB_START_REQ");
  SubStartReq *const req = (SubStartReq *)signal->getDataPtr();

  CRASH_INSERTION(13013);
  Uint32 senderRef = req->senderRef;
  Uint32 senderData = req->senderData;
  Uint32 subscriberData = req->subscriberData;
  Uint32 subscriberRef = req->subscriberRef;
  Uint32 requestInfo = 0;
  if (signal->getLength() > SubStartReq::SignalLengthWithoutRequestInfo) {
    requestInfo = req->requestInfo;
  }
  SubscriptionData::Part part = (SubscriptionData::Part)req->part;
  (void)part;  // TODO validate part

  Subscription key;
  key.m_subscriptionId = req->subscriptionId;
  key.m_subscriptionKey = req->subscriptionKey;

  SubscriptionPtr subPtr;

  CRASH_INSERTION2(13042, getNodeState().startLevel == NodeState::SL_STARTING);

  if (c_startup.m_restart_server_node_id == RNIL) {
    jam();

    /**
     * We haven't started syncing yet
     */
    sendSubStartRef(signal, senderRef, senderData, SubStartRef::NotStarted);
    return;
  }

  bool found = c_subscriptions.find(subPtr, key);
  if (!found) {
    jam();
    sendSubStartRef(signal, senderRef, senderData,
                    SubStartRef::NoSuchSubscription);
    return;
  }

  if (ERROR_INSERTED(13046)) {
    jam();
    CLEAR_ERROR_INSERT_VALUE;
    sendSubStartRef(signal, senderRef, senderData,
                    SubStartRef::NoSuchSubscription);
    return;
  }

  switch (subPtr.p->m_state) {
    case Subscription::UNDEFINED:
      jam();
      ndbabort();
    case Subscription::DEFINING:
      jam();
      sendSubStartRef(signal, senderRef, senderData, SubStartRef::Defining);
      return;
    case Subscription::DEFINED:
      break;
  }

  if (subPtr.p->m_options & Subscription::MARKED_DROPPED) {
    jam();
    if (c_startup.m_restart_server_node_id == 0) {
      sendSubStartRef(signal, senderRef, senderData, SubStartRef::Dropped);
      return;
    } else {
      /**
       * Allow SUB_START_REQ from peer node
       */
    }
  }

  if (subPtr.p->m_trigger_state == Subscription::T_ERROR) {
    jam();
    sendSubStartRef(signal, senderRef, senderData, subPtr.p->m_errorCode);
    return;
  }

  switch (getNodeInfo(refToNode(subscriberRef)).m_type) {
    case NodeInfo::DB:
    case NodeInfo::API:
    case NodeInfo::MGM:
      if (!ERROR_INSERTED_CLEAR(13047)) break;
      // Fall through - if error inserted
      [[fallthrough]];
    default:
      /**
       * This can happen if we start...with a new config
       *   that has dropped a node...that has a subscription active
       *   (or maybe internal error ??)
       *
       * If this is a node-restart, it means that we will refuse to start
       * If not, this mean that substart will simply fail...
       */
      jam();
      sendSubStartRef(signal, senderRef, senderData,
                      SubStartRef::SubscriberNodeIdUndefined);
      return;
  }

  SubscriberPtr subbPtr;
  if (!c_subscriberPool.seize(subbPtr)) {
    jam();
    sendSubStartRef(signal, senderRef, senderData,
                    SubStartRef::OutOfSubscriberRecords);
    return;
  }

  Ptr<SubOpRecord> subOpPtr;
  if (!c_subOpPool.seize(subOpPtr)) {
    jam();
    c_subscriberPool.release(subbPtr);
    sendSubStartRef(signal, senderRef, senderData,
                    SubStartRef::OutOfSubOpRecords);
    return;
  }

  if (!check_sub_start(subscriberRef)) {
    jam();
    c_subscriberPool.release(subbPtr);
    c_subOpPool.release(subOpPtr);
    sendSubStartRef(signal, senderRef, senderData, SubStartRef::NodeDied);
    return;
  }

  // setup subscriber record
  subbPtr.p->m_senderRef = subscriberRef;
  subbPtr.p->m_senderData = subscriberData;
  subbPtr.p->m_requestInfo = requestInfo;

  subOpPtr.p->m_opType = SubOpRecord::R_SUB_START_REQ;
  subOpPtr.p->m_subPtrI = subPtr.i;
  subOpPtr.p->m_senderRef = senderRef;
  subOpPtr.p->m_senderData = senderData;
  subOpPtr.p->m_subscriberRef = subbPtr.i;

  {
    Local_SubOpRecord_fifo subOpList(c_subOpPool, subPtr.p->m_start_req);
    subOpList.addLast(subOpPtr);
  }

  /**
   * Check triggers
   */
  switch (subPtr.p->m_trigger_state) {
    case Subscription::T_UNDEFINED:
      jam();
      /**
       * create triggers
       */
      create_triggers(signal, subPtr);
      break;
    case Subscription::T_CREATING:
      jam();
      /**
       * Triggers are already being created...wait for completion
       */
      return;
    case Subscription::T_DROPPING:
      jam();
      /**
       * Trigger(s) are being dropped...wait for completion
       *   (and recreate them when done)
       */
      break;
    case Subscription::T_DEFINED: {
      jam();
      report_sub_start_conf(signal, subPtr);
      return;
    }
    case Subscription::T_ERROR:
      jam();
      ndbabort();  // Checked above
  }
}

void Suma::sendSubStartRef(Signal *signal, Uint32 dstref, Uint32 data,
                           Uint32 err) {
  jam();
  SubStartRef *ref = (SubStartRef *)signal->getDataPtrSend();
  ref->senderRef = reference();
  ref->senderData = data;
  ref->errorCode = err;
  sendSignal(dstref, GSN_SUB_START_REF, signal, SubStartRef::SignalLength, JBB);
}

void Suma::create_triggers(Signal *signal, SubscriptionPtr subPtr) {
  jam();

  ndbrequire(subPtr.p->m_trigger_state == Subscription::T_UNDEFINED);
  subPtr.p->m_trigger_state = Subscription::T_CREATING;

  TablePtr tabPtr;
  ndbrequire(c_tablePool.getPtr(tabPtr, subPtr.p->m_table_ptrI));

  AttributeMask attrMask;
  tabPtr.p->createAttributeMask(attrMask, *this);

  subPtr.p->m_outstanding_trigger = 3;
  for (Uint32 j = 0; j < 3; j++) {
    Uint32 triggerId = (tabPtr.p->m_schemaVersion << 18) | (j << 16) | subPtr.i;
    ndbrequire(subPtr.p->m_triggers[j] == ILLEGAL_TRIGGER_ID);

    CreateTrigImplReq *const req =
        (CreateTrigImplReq *)signal->getDataPtrSend();
    req->senderRef = SUMA_REF;
    req->senderData = subPtr.i;
    req->requestType = 0;

    Uint32 ti = 0;
    TriggerInfo::setTriggerType(ti, TriggerType::SUBSCRIPTION_BEFORE);
    TriggerInfo::setTriggerActionTime(ti, TriggerActionTime::TA_DETACHED);
    TriggerInfo::setTriggerEvent(ti, (TriggerEvent::Value)j);
    TriggerInfo::setMonitorReplicas(ti, true);
    // TriggerInfo::setMonitorAllAttributes(ti, j == TriggerEvent::TE_DELETE);
    TriggerInfo::setMonitorAllAttributes(ti, true);
    TriggerInfo::setReportAllMonitoredAttributes(
        ti, subPtr.p->m_options & Subscription::REPORT_ALL);
    req->triggerInfo = ti;

    req->receiverRef = SUMA_REF;
    req->triggerId = triggerId;
    req->tableId = subPtr.p->m_tableId;
    req->tableVersion = 0;  // not used
    req->indexId = ~(Uint32)0;
    req->indexVersion = 0;

    LinearSectionPtr ptr[3];
    ptr[0].p = attrMask.rep.data;
    ptr[0].sz = attrMask.getSizeInWords();
    sendSignal(DBTUP_REF, GSN_CREATE_TRIG_IMPL_REQ, signal,
               CreateTrigImplReq::SignalLength, JBB, ptr, 1);
  }
}

void Suma::execCREATE_TRIG_IMPL_CONF(Signal *signal) {
  jamEntry();

  CreateTrigImplConf *conf = (CreateTrigImplConf *)signal->getDataPtr();
  const Uint32 triggerId = conf->triggerId;
  Uint32 type = (triggerId >> 16) & 0x3;
  Uint32 tableId = conf->tableId;

  TablePtr tabPtr;
  SubscriptionPtr subPtr;
  c_subscriptions.getPtr(subPtr, conf->senderData);
  c_tables.getPtr(tabPtr, subPtr.p->m_table_ptrI);

  ndbrequire(tabPtr.p->m_tableId == tableId);
  ndbrequire(subPtr.p->m_trigger_state == Subscription::T_CREATING);

  ndbrequire(type < 3);
  ndbrequire(subPtr.p->m_triggers[type] == ILLEGAL_TRIGGER_ID);
  subPtr.p->m_triggers[type] = triggerId;

  ndbrequire(subPtr.p->m_outstanding_trigger);
  subPtr.p->m_outstanding_trigger--;

  if (subPtr.p->m_outstanding_trigger) {
    jam();
    /**
     * Wait for more
     */
    return;
  }

  if (subPtr.p->m_errorCode == 0) {
    jam();
    subPtr.p->m_trigger_state = Subscription::T_DEFINED;
    report_sub_start_conf(signal, subPtr);
  } else {
    jam();
    subPtr.p->m_trigger_state = Subscription::T_ERROR;
    drop_triggers(signal, subPtr);
  }
}

void Suma::execCREATE_TRIG_IMPL_REF(Signal *signal) {
  jamEntry();

  CreateTrigImplRef *const ref = (CreateTrigImplRef *)signal->getDataPtr();
  const Uint32 triggerId = ref->triggerId;
  Uint32 type = (triggerId >> 16) & 0x3;
  Uint32 tableId = ref->tableId;

  TablePtr tabPtr;
  SubscriptionPtr subPtr;
  c_subscriptions.getPtr(subPtr, ref->senderData);
  c_tables.getPtr(tabPtr, subPtr.p->m_table_ptrI);

  ndbrequire(tabPtr.p->m_tableId == tableId);
  ndbrequire(subPtr.p->m_trigger_state == Subscription::T_CREATING);

  ndbrequire(type < 3);
  ndbrequire(subPtr.p->m_triggers[type] == ILLEGAL_TRIGGER_ID);

  subPtr.p->m_errorCode = ref->errorCode;

  ndbrequire(subPtr.p->m_outstanding_trigger);
  subPtr.p->m_outstanding_trigger--;

  if (subPtr.p->m_outstanding_trigger) {
    jam();
    /**
     * Wait for more
     */
    return;
  }

  subPtr.p->m_trigger_state = Subscription::T_ERROR;
  drop_triggers(signal, subPtr);
}

bool Suma::check_sub_start(Uint32 subscriberRef) {
  Uint32 nodeId = refToNode(subscriberRef);
  bool startme = c_startup.m_restart_server_node_id;
  bool handover = c_startup.m_wait_handover;
  bool connected =
      c_failedApiNodes.get(nodeId) == false && c_connected_nodes.get(nodeId);

  return (startme || handover || connected);
}

void Suma::report_sub_start_conf(Signal *signal, Ptr<Subscription> subPtr) {
  const Uint64 gci = get_current_gci(signal);
  {
    Local_Subscriber_list list(c_subscriberPool, subPtr.p->m_subscribers);
    Local_SubOpRecord_fifo subOpList(c_subOpPool, subPtr.p->m_start_req);

    Ptr<Subscriber> ptr;
    Ptr<SubOpRecord> subOpPtr;
    for (subOpList.first(subOpPtr); !subOpPtr.isNull();) {
      jam();

      Uint32 senderRef = subOpPtr.p->m_senderRef;
      Uint32 senderData = subOpPtr.p->m_senderData;
      ndbrequire(c_subscriberPool.getPtr(ptr, subOpPtr.p->m_subscriberRef));

      if (check_sub_start(ptr.p->m_senderRef)) {
        SubStartConf *conf = (SubStartConf *)signal->getDataPtrSend();
        conf->senderRef = reference();
        conf->senderData = senderData;
        conf->subscriptionId = subPtr.p->m_subscriptionId;
        conf->subscriptionKey = subPtr.p->m_subscriptionKey;
        conf->firstGCI = Uint32(gci >> 32);
        conf->part = SubscriptionData::TableData;
        conf->bucketCount = c_no_of_buckets;
        conf->nodegroup = c_nodeGroup;
        sendSignal(senderRef, GSN_SUB_START_CONF, signal,
                   SubStartConf::SignalLength, JBB);

        /*
         * Report to the new subscriber that its subscription is now active.
         * Also, if REPORT_SUBSCRIBE option is enabled, report to the new
         * subscriber about all other subscribers in this subscription and
         * report to all other subscribers about this new subscriber.
         * Sending all these reports is skipped if this is a restart as that
         * implies that the subscriptions are just being copied from another
         * data node and the subscribers are not actually connected yet.
         */
        if (c_startup.m_restart_server_node_id == 0) {
          bool report = subPtr.p->m_options & Subscription::REPORT_SUBSCRIBE;
          send_sub_start_stop_event(
              signal, ptr, NdbDictionary::Event::_TE_ACTIVE, report, list);
        }

        // Add the new subscriber to the subscription's list.
        list.addFirst(ptr);
        c_subscriber_nodes.set(refToNode(ptr.p->m_senderRef));
        c_subscriber_per_node[refToNode(ptr.p->m_senderRef)]++;
      } else {
        jam();

        sendSubStartRef(signal, senderRef, senderData, SubStartRef::NodeDied);

        c_subscriberPool.release(ptr);
      }

      Ptr<SubOpRecord> tmp = subOpPtr;
      subOpList.next(subOpPtr);
      subOpList.release(tmp);
    }
  }

  check_release_subscription(signal, subPtr);
}

void Suma::report_sub_start_ref(Signal *signal, Ptr<Subscription> subPtr,
                                Uint32 errCode) {
  Local_Subscriber_list list(c_subscriberPool, subPtr.p->m_subscribers);
  Local_SubOpRecord_fifo subOpList(c_subOpPool, subPtr.p->m_start_req);

  Ptr<Subscriber> ptr;
  Ptr<SubOpRecord> subOpPtr;
  for (subOpList.first(subOpPtr); !subOpPtr.isNull();) {
    jam();

    Uint32 senderRef = subOpPtr.p->m_senderRef;
    Uint32 senderData = subOpPtr.p->m_senderData;
    ndbrequire(c_subscriberPool.getPtr(ptr, subOpPtr.p->m_subscriberRef));

    SubStartRef *ref = (SubStartRef *)signal->getDataPtrSend();
    ref->senderRef = reference();
    ref->senderData = senderData;
    ref->errorCode = errCode;

    sendSignal(senderRef, GSN_SUB_START_REF, signal, SubStartConf::SignalLength,
               JBB);

    Ptr<SubOpRecord> tmp = subOpPtr;
    subOpList.next(subOpPtr);
    subOpList.release(tmp);
    c_subscriberPool.release(ptr);
  }
}

void Suma::report_subscription_set(Signal *signal, Uint32 subscriptionIdx,
                                   Uint32 subscriberIdx,
                                   Uint32 otherSubscriberIdx,
                                   const Uint32 batchSize) {
  jam();
  SubscriptionPtr subPtr;
  SubscriberPtr subscriberPtr, otherSubscriberPtr;
  Uint32 totalSignalsSent = 0;
  const Uint64 gci = get_current_gci(signal);

  subPtr.i = subscriptionIdx;
  subscriberPtr.i = subscriberIdx;
  otherSubscriberPtr.i = otherSubscriberIdx;

  if (subPtr.i == RNIL) {
    jam();
    c_subscriptions.first(subPtr);
  } else {
    jam();
    c_subscriptions.getPtr(subPtr);
  }

  while (subPtr.i != RNIL) {
    jam();
    const bool report = subPtr.p->m_options & Subscription::REPORT_SUBSCRIBE;

    // Retrieve list of all subscribers for this subscription
    ConstLocal_Subscriber_list subList(c_subscriberPool,
                                       subPtr.p->m_subscribers);
    if (subscriberPtr.i == RNIL) {
      jam();
      subList.first(subscriberPtr);
    } else {
      jam();
      subList.getPtr(subscriberPtr);
    }

    while (subscriberPtr.i != RNIL) {
      jam();

      const Uint32 subscriberNodeId = refToNode(subscriberPtr.p->m_senderRef);
      bool send_te_active = false;

      ConstLocal_Subscriber_list otherSubList(c_subscriberPool,
                                              subPtr.p->m_subscribers);

      if (otherSubscriberPtr.i == RNIL) {
        jam();
        otherSubList.first(otherSubscriberPtr);
        send_te_active = true;
      } else {
        jam();
        otherSubList.getPtr(otherSubscriberPtr);
      }

      jam();
      while (otherSubscriberPtr.i != RNIL) {
        jam();

        if (send_te_active) {
          jam();
          send_te_active = false;

          /* Send TE_ACTIVE to subscriber */
          SubTableData *data = (SubTableData *)signal->getDataPtrSend();

          // default SubTableData member values for the events
          data->gci_hi = Uint32(gci >> 32);
          data->gci_lo = Uint32(gci);
          data->tableId = 0;
          data->changeMask = 0;
          data->totalLen = 0;
          data->requestInfo = 0;
          SubTableData::setOperation(data->requestInfo,
                                     NdbDictionary::Event::_TE_ACTIVE);
          SubTableData::setNdbdNodeId(data->requestInfo, getOwnNodeId());
          SubTableData::setReqNodeId(data->requestInfo, subscriberNodeId);
          data->senderData = subscriberPtr.p->m_senderData;
          sendSignal(subscriberPtr.p->m_senderRef, GSN_SUB_TABLE_DATA, signal,
                     SubTableData::SignalLength, JBB);
          totalSignalsSent++;
        }

        if ((totalSignalsSent >= batchSize) ||
            ((ERROR_INSERTED(13058) || ERROR_INSERTED(13059)) &&
             totalSignalsSent >= 2)) {
          jam();
          /* Take a break */

          if (ERROR_INSERTED_CLEAR(13058)) {
            jam();

            /* Kill a randomish subscriber */
            signal->theData[0] = 900;
            signal->theData[1] = c_subscriber_nodes.find_last();
            sendSignal(CMVMI_REF, GSN_DUMP_STATE_ORD, signal, 2, JBB);
            SET_ERROR_INSERT_VALUE(13059);
          }

          signal->theData[0] = SumaContinueB::REPORT_SUBSCRIPTION_SET;
          signal->theData[1] = subPtr.i;
          signal->theData[2] = subscriberPtr.i;
          signal->theData[3] = otherSubscriberPtr.i;
          signal->theData[4] = batchSize;

          if (ERROR_INSERTED(13058) || ERROR_INSERTED(13059)) {
            jam();
            sendSignalWithDelay(reference(), GSN_CONTINUEB, signal, 100, 5);
          } else {
            sendSignal(reference(), GSN_CONTINUEB, signal, 5, JBB);
          }

          return;
        }

        if (!report) {
          jam();
          otherSubscriberPtr.i = RNIL;
          break;
        }

        /* Inform this subscriber about all the other subscribers */
        if (subscriberPtr.i != otherSubscriberPtr.i) {
          jam();
          /* Send TE_SUBSCRIBE(other subscriber) to subscriber */
          SubTableData *data = (SubTableData *)signal->getDataPtrSend();

          // default SubTableData member values for the events
          data->gci_hi = Uint32(gci >> 32);
          data->gci_lo = Uint32(gci);
          data->tableId = 0;
          data->changeMask = 0;
          data->totalLen = 0;
          data->requestInfo = 0;
          SubTableData::setOperation(data->requestInfo,
                                     NdbDictionary::Event::_TE_SUBSCRIBE);
          SubTableData::setNdbdNodeId(data->requestInfo, getOwnNodeId());
          SubTableData::setReqNodeId(
              data->requestInfo, refToNode(otherSubscriberPtr.p->m_senderRef));
          data->senderData = subscriberPtr.p->m_senderData;
          sendSignal(subscriberPtr.p->m_senderRef, GSN_SUB_TABLE_DATA, signal,
                     SubTableData::SignalLength, JBB);
          totalSignalsSent++;
        }

        otherSubList.next(otherSubscriberPtr); /* Next other subscriber */
      }

      subList.next(subscriberPtr); /* Next subscriber */
    }
    c_subscriptions.next(subPtr); /* Next subscription */
  }

  /* Reporting done */
  jam();
  g_reporting_in_progress = false;

  if (c_no_of_buckets != 0) {
    jam();
    /* Now trigger bucket handover */
    send_handover_req(signal, SumaHandoverReq::RT_START_NODE);
  } else {
    /**
     * No-one to handover with - no nodegroup assigned
     * Continue restart
     */
    jam();
    send_dict_unlock_ord(signal, DictLockReq::SumaHandOver);
    sendSTTORRY(signal);
  }
}

void Suma::drop_triggers(Signal *signal, SubscriptionPtr subPtr) {
  jam();

  subPtr.p->m_outstanding_trigger = 0;

  Ptr<Table> tabPtr;
  ndbrequire(c_tablePool.getPtr(tabPtr, subPtr.p->m_table_ptrI));
  if (tabPtr.p->m_state == Table::DROPPED) {
    jam();
    subPtr.p->m_triggers[0] = ILLEGAL_TRIGGER_ID;
    subPtr.p->m_triggers[1] = ILLEGAL_TRIGGER_ID;
    subPtr.p->m_triggers[2] = ILLEGAL_TRIGGER_ID;
  } else {
    for (Uint32 j = 0; j < 3; j++) {
      jam();
      Uint32 triggerId = subPtr.p->m_triggers[j];
      if (triggerId != ILLEGAL_TRIGGER_ID) {
        subPtr.p->m_outstanding_trigger++;

        DropTrigImplReq *const req =
            (DropTrigImplReq *)signal->getDataPtrSend();
        req->senderRef = SUMA_REF;  // Sending to myself
        req->senderData = subPtr.i;
        req->requestType = 0;

        // TUP needs some triggerInfo to find right list
        Uint32 ti = 0;
        TriggerInfo::setTriggerType(ti, TriggerType::SUBSCRIPTION_BEFORE);
        TriggerInfo::setTriggerActionTime(ti, TriggerActionTime::TA_DETACHED);
        TriggerInfo::setTriggerEvent(ti, (TriggerEvent::Value)j);
        TriggerInfo::setMonitorReplicas(ti, true);
        // TriggerInfo::setMonitorAllAttributes(ti, j
        // ==TriggerEvent::TE_DELETE);
        TriggerInfo::setMonitorAllAttributes(ti, true);
        TriggerInfo::setReportAllMonitoredAttributes(
            ti, subPtr.p->m_options & Subscription::REPORT_ALL);
        req->triggerInfo = ti;

        req->tableId = subPtr.p->m_tableId;
        req->tableVersion = 0;  // not used
        req->indexId = RNIL;
        req->indexVersion = 0;
        req->triggerId = triggerId;
        req->receiverRef = SUMA_REF;

        c_outstanding_drop_trig_req++;
        if (ERROR_INSERTED(13051)) {
          /* Delay the DROP_TRIG_IMPL_REQ */
          sendSignalWithDelay(DBTUP_REF, GSN_DROP_TRIG_IMPL_REQ, signal, 99,
                              DropTrigImplReq::SignalLength);
        } else {
          sendSignal(DBTUP_REF, GSN_DROP_TRIG_IMPL_REQ, signal,
                     DropTrigImplReq::SignalLength, JBB);
        }
      }
    }
  }

  if (subPtr.p->m_outstanding_trigger == 0) {
    jam();
    drop_triggers_complete(signal, subPtr);
  }
}

void Suma::execDROP_TRIG_IMPL_REF(Signal *signal) {
  jamEntry();
  if (g_reporting_in_progress) {
    jam();
    /* Stall processing until reporting done */
    sendSignalWithDelay(reference(), GSN_DROP_TRIG_IMPL_REF, signal, 20,
                        signal->getLength());
    return;
  }
  DropTrigImplRef *const ref = (DropTrigImplRef *)signal->getDataPtr();
  Ptr<Table> tabPtr;
  Ptr<Subscription> subPtr;
  const Uint32 triggerId = ref->triggerId;
  const Uint32 type = (triggerId >> 16) & 0x3;

  ndbrequire(c_subscriptionPool.getPtr(subPtr, ref->senderData));
  c_tables.getPtr(tabPtr, subPtr.p->m_table_ptrI);
  ndbrequire(tabPtr.p->m_tableId == ref->tableId);

  ndbrequire(type < 3);
  ndbrequire(subPtr.p->m_triggers[type] != ILLEGAL_TRIGGER_ID);
  subPtr.p->m_triggers[type] = ILLEGAL_TRIGGER_ID;

  ndbrequire(subPtr.p->m_outstanding_trigger);
  subPtr.p->m_outstanding_trigger--;

  ndbrequire(c_outstanding_drop_trig_req);
  c_outstanding_drop_trig_req--;

  if (subPtr.p->m_outstanding_trigger) {
    jam();
    /**
     * Wait for more
     */
    return;
  }

  drop_triggers_complete(signal, subPtr);
}

void Suma::execDROP_TRIG_IMPL_CONF(Signal *signal) {
  jamEntry();
  if (g_reporting_in_progress) {
    jam();
    /* Stall processing until reporting done */
    sendSignalWithDelay(reference(), GSN_DROP_TRIG_IMPL_CONF, signal, 20,
                        signal->getLength());
    return;
  }
  DropTrigImplConf *const conf = (DropTrigImplConf *)signal->getDataPtr();

  Ptr<Table> tabPtr;
  Ptr<Subscription> subPtr;
  const Uint32 triggerId = conf->triggerId;
  const Uint32 type = (triggerId >> 16) & 0x3;

  ndbrequire(c_subscriptionPool.getPtr(subPtr, conf->senderData));
  c_tables.getPtr(tabPtr, subPtr.p->m_table_ptrI);
  ndbrequire(tabPtr.p->m_tableId == conf->tableId);

  ndbrequire(type < 3);
  ndbrequire(subPtr.p->m_triggers[type] != ILLEGAL_TRIGGER_ID);
  subPtr.p->m_triggers[type] = ILLEGAL_TRIGGER_ID;

  ndbrequire(subPtr.p->m_outstanding_trigger);
  subPtr.p->m_outstanding_trigger--;

  ndbrequire(c_outstanding_drop_trig_req);
  c_outstanding_drop_trig_req--;

  if (subPtr.p->m_outstanding_trigger) {
    jam();
    /**
     * Wait for more
     */
    return;
  }

  drop_triggers_complete(signal, subPtr);
}

void Suma::drop_triggers_complete(Signal *signal, Ptr<Subscription> subPtr) {
  switch (subPtr.p->m_trigger_state) {
    case Subscription::T_UNDEFINED:
    case Subscription::T_CREATING:
    case Subscription::T_DEFINED:
      jam();
      ndbabort();
    case Subscription::T_DROPPING:
      jam();
      /**
       */
      subPtr.p->m_trigger_state = Subscription::T_UNDEFINED;
      if (!subPtr.p->m_start_req.isEmpty()) {
        jam();
        create_triggers(signal, subPtr);
        return;
      }
      break;
    case Subscription::T_ERROR:
      jam();
      Uint32 err = subPtr.p->m_errorCode;
      subPtr.p->m_trigger_state = Subscription::T_UNDEFINED;
      subPtr.p->m_errorCode = 0;
      report_sub_start_ref(signal, subPtr, err);
      break;
  }

  check_release_subscription(signal, subPtr);
}

/**********************************************************
 * Suma participant interface
 *
 * Stopping and removing of subscriber
 *
 */

void Suma::execSUB_STOP_REQ(Signal *signal) {
  jamEntry();
  ndbassert(signal->getNoOfSections() == 0);
  DBUG_ENTER("Suma::execSUB_STOP_REQ");

  CRASH_INSERTION(13019);

  SubStopReq *const req = (SubStopReq *)signal->getDataPtr();
  Uint32 senderRef = req->senderRef;
  Uint32 senderData = req->senderData;
  Uint32 subscriberRef = req->subscriberRef;
  Uint32 subscriberData = req->subscriberData;
  SubscriptionPtr subPtr;
  Subscription key;
  key.m_subscriptionId = req->subscriptionId;
  key.m_subscriptionKey = req->subscriptionKey;
  bool abortStart = (req->requestInfo & SubStopReq::RI_ABORT_START);

  if (c_startup.m_restart_server_node_id == RNIL) {
    jam();

    /**
     * We haven't started syncing yet
     */
    sendSubStopRef(signal, senderRef, senderData, SubStopRef::NotStarted);
    return;
  }

  bool found = c_subscriptions.find(subPtr, key);
  if (!found) {
    jam();
    sendSubStopRef(signal, senderRef, senderData,
                   SubStopRef::NoSuchSubscription);
    return;
  }

  switch (subPtr.p->m_state) {
    case Subscription::UNDEFINED:
      jam();
      ndbabort();
    case Subscription::DEFINING:
      jam();
      sendSubStopRef(signal, senderRef, senderData, SubStopRef::Defining);
      return;
    case Subscription::DEFINED:
      jam();
      break;
  }

  Ptr<SubOpRecord> subOpPtr;
  Local_SubOpRecord_fifo list(c_subOpPool, subPtr.p->m_stop_req);
  bool empty = list.isEmpty();
  if (list.seizeLast(subOpPtr) == false) {
    jam();
    sendSubStopRef(signal, senderRef, senderData,
                   SubStopRef::OutOfSubOpRecords);
    return;
  }

  if (abortStart) {
    jam();
    subOpPtr.p->m_opType = SubOpRecord::R_SUB_ABORT_START_REQ;
  } else {
    jam();
    subOpPtr.p->m_opType = SubOpRecord::R_SUB_STOP_REQ;
  }
  subOpPtr.p->m_subPtrI = subPtr.i;
  subOpPtr.p->m_senderRef = senderRef;
  subOpPtr.p->m_senderData = senderData;
  subOpPtr.p->m_subscriberRef = subscriberRef;
  subOpPtr.p->m_subscriberData = subscriberData;

  if (empty) {
    jam();
    signal->theData[0] = SumaContinueB::SUB_STOP_REQ;
    signal->theData[1] = subOpPtr.i;
    signal->theData[2] = RNIL;
    sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 3, JBB);
  }
}

void Suma::sub_stop_req(Signal *signal) {
  jam();
  if (c_outstanding_drop_trig_req >= NDB_MAX_SUMA_DROP_TRIG_REQ_SUBSTOP) {
    jam();
    /* Further sub stop requests execution might flood the Short time queue by
     * sending too many drop trigger requests. So they are delayed until the
     * previous requests are processed. */
    sendSignalWithDelay(SUMA_REF, GSN_CONTINUEB, signal, 10, 3);
    return;
  }

  Ptr<SubOpRecord> subOpPtr;
  ndbrequire(c_subOpPool.getPtr(subOpPtr, signal->theData[1]));

  Ptr<Subscription> subPtr;
  ndbrequire(c_subscriptionPool.getPtr(subPtr, subOpPtr.p->m_subPtrI));

  Ptr<Subscriber> ptr;
  {
    Local_Subscriber_list list(c_subscriberPool, subPtr.p->m_subscribers);
    if (signal->theData[2] == RNIL) {
      jam();
      list.first(ptr);
    } else {
      jam();
      list.getPtr(ptr, signal->theData[2]);
    }

    for (Uint32 i = 0; i < 32 && !ptr.isNull(); i++, list.next(ptr)) {
      if (ptr.p->m_senderRef == subOpPtr.p->m_subscriberRef &&
          ptr.p->m_senderData == subOpPtr.p->m_subscriberData) {
        jam();
        goto found;
      }
    }
  }

  if (ptr.isNull()) {
    jam();
    sendSubStopRef(signal, subOpPtr.p->m_senderRef, subOpPtr.p->m_senderData,
                   SubStopRef::NoSuchSubscriber);
    check_remove_queue(signal, subPtr, subOpPtr, true, true);
    return;
  }

  signal->theData[0] = SumaContinueB::SUB_STOP_REQ;
  signal->theData[1] = subOpPtr.i;
  signal->theData[2] = ptr.i;
  sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 3, JBB);
  return;

found : {
  Local_Subscriber_list list(c_subscriberPool, subPtr.p->m_subscribers);
  list.remove(ptr);
  /**
   * NOTE: remove before...so we done send UNSUBSCRIBE to self (yuck)
   */
  bool report = subPtr.p->m_options & Subscription::REPORT_SUBSCRIBE;
  report_sub_stop_conf(signal, subOpPtr, ptr, report, list);
  c_subscriberPool.release(ptr);
}
  check_remove_queue(signal, subPtr, subOpPtr, true, true);
  check_release_subscription(signal, subPtr);
}

void Suma::check_remove_queue(Signal *signal, Ptr<Subscription> subPtr,
                              Ptr<SubOpRecord> subOpPtr, bool ishead,
                              bool dorelease) {
  Local_SubOpRecord_fifo list(c_subOpPool, subPtr.p->m_stop_req);

  {
    Ptr<SubOpRecord> tmp;
    list.first(tmp);
    if (ishead) {
      jam();
      ndbrequire(tmp.i == subOpPtr.i);
    } else {
      jam();
      ishead = (tmp.i == subOpPtr.i);
    }
  }

  if (dorelease) {
    jam();
    list.release(subOpPtr);
  } else {
    jam();
    list.remove(subOpPtr);
  }

  if (ishead) {
    jam();
    if (list.first(subOpPtr) == false) {
      jam();
      c_restart.m_waiting_on_self = 1;
      return;
    }
    // Fall through
  } else {
    jam();
    return;
  }

  switch (subOpPtr.p->m_opType) {
    case SubOpRecord::R_SUB_ABORT_START_REQ:
    case SubOpRecord::R_SUB_STOP_REQ:
      jam();
      signal->theData[0] = SumaContinueB::SUB_STOP_REQ;
      signal->theData[1] = subOpPtr.i;
      signal->theData[2] = RNIL;
      sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 3, JBB);
      return;
    case SubOpRecord::R_API_FAIL_REQ:
      jam();
      signal->theData[0] = SumaContinueB::API_FAIL_SUBSCRIPTION;
      signal->theData[1] = subOpPtr.i;
      signal->theData[2] = RNIL;
      sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 3, JBB);
      return;
    case SubOpRecord::R_START_ME_REQ:
      jam();
      sendSubCreateReq(signal, subPtr);
      return;
  }
}

void Suma::report_sub_stop_conf(Signal *signal, Ptr<SubOpRecord> subOpPtr,
                                Ptr<Subscriber> ptr, bool report,
                                Local_Subscriber_list &list) {
  jam();
  CRASH_INSERTION(13020);

  Uint32 senderRef = subOpPtr.p->m_senderRef;
  Uint32 senderData = subOpPtr.p->m_senderData;
  bool abortStart = subOpPtr.p->m_opType == SubOpRecord::R_SUB_ABORT_START_REQ;

  // let subscriber know that subscriber is stopped
  if (!abortStart) {
    jam();
    send_sub_start_stop_event(signal, ptr, NdbDictionary::Event::_TE_STOP,
                              report, list);
  }

  SubStopConf *const conf = (SubStopConf *)signal->getDataPtrSend();
  const Uint64 gci = m_max_seen_gci;
  conf->senderRef = reference();
  conf->senderData = senderData;
  conf->gci_hi = Uint32(gci >> 32);
  conf->gci_lo = Uint32(gci);
  sendSignal(senderRef, GSN_SUB_STOP_CONF, signal, SubStopConf::SignalLength,
             JBB);

  Uint32 nodeId = refToNode(ptr.p->m_senderRef);
  if (c_subscriber_per_node[nodeId]) {
    c_subscriber_per_node[nodeId]--;
    if (c_subscriber_per_node[nodeId] == 0) {
      jam();
      c_subscriber_nodes.clear(nodeId);
    }
  }
}

void Suma::sendSubStopRef(Signal *signal, Uint32 retref, Uint32 data,
                          Uint32 errCode) {
  jam();
  SubStopRef *ref = (SubStopRef *)signal->getDataPtrSend();
  ref->senderRef = reference();
  ref->errorCode = errCode;
  ref->senderData = data;
  sendSignal(retref, GSN_SUB_STOP_REF, signal, SubStopRef::SignalLength, JBB);
}

// report new started subscriber to all other subscribers
void Suma::send_sub_start_stop_event(Signal *signal, Ptr<Subscriber> ptr,
                                     NdbDictionary::Event::_TableEvent event,
                                     bool report, Local_Subscriber_list &list) {
  const Uint64 gci = get_current_gci(signal);
  SubTableData *data = (SubTableData *)signal->getDataPtrSend();
  Uint32 nodeId = refToNode(ptr.p->m_senderRef);

  NdbDictionary::Event::_TableEvent other;
  if (event == NdbDictionary::Event::_TE_STOP) {
    other = NdbDictionary::Event::_TE_UNSUBSCRIBE;
  } else if (event == NdbDictionary::Event::_TE_ACTIVE) {
    other = NdbDictionary::Event::_TE_SUBSCRIBE;
  } else {
    jamLine(event);
    ndbabort();
  }

  data->gci_hi = Uint32(gci >> 32);
  data->gci_lo = Uint32(gci);
  data->tableId = 0;
  data->requestInfo = 0;
  SubTableData::setOperation(data->requestInfo, event);
  SubTableData::setNdbdNodeId(data->requestInfo, getOwnNodeId());
  SubTableData::setReqNodeId(data->requestInfo, nodeId);
  data->changeMask = 0;
  data->totalLen = 0;
  data->senderData = ptr.p->m_senderData;
  sendSignal(ptr.p->m_senderRef, GSN_SUB_TABLE_DATA, signal,
             SubTableData::SignalLength, JBB);

  if (report == false) {
    return;
  }

  data->requestInfo = 0;
  SubTableData::setOperation(data->requestInfo, other);
  SubTableData::setNdbdNodeId(data->requestInfo, getOwnNodeId());

  Ptr<Subscriber> tmp;
  for (list.first(tmp); !tmp.isNull(); list.next(tmp)) {
    jam();
    SubTableData::setReqNodeId(data->requestInfo, nodeId);
    data->senderData = tmp.p->m_senderData;
    sendSignal(tmp.p->m_senderRef, GSN_SUB_TABLE_DATA, signal,
               SubTableData::SignalLength, JBB);

    ndbassert(tmp.i != ptr.i);  // ptr should *NOT* be in list now
    if (other != NdbDictionary::Event::_TE_UNSUBSCRIBE) {
      jam();
      SubTableData::setReqNodeId(data->requestInfo,
                                 refToNode(tmp.p->m_senderRef));

      data->senderData = ptr.p->m_senderData;
      sendSignal(ptr.p->m_senderRef, GSN_SUB_TABLE_DATA, signal,
                 SubTableData::SignalLength, JBB);
    }
  }
}

void Suma::Table::createAttributeMask(AttributeMask &mask, Suma &suma) {
  mask.clear();
  for (Uint32 i = 0; i < m_noOfAttributes; i++) mask.set(i);
}

void Suma::suma_ndbrequire(bool v) { ndbrequire(v); }

/**********************************************************
 * Scan data interface
 *
 * Assumption: one execTRANSID_AI contains all attr info
 *
 */

#define SUMA_BUF_SZ (MAX_ATTRIBUTES_IN_TABLE + MAX_TUPLE_SIZE_IN_WORDS)

#define NO_LOCK_VAL 0xffffffff
#define TRIGGER_LOCK_BASE 0x00000000

static Uint32 bufferLock = NO_LOCK_VAL;
static Uint32 f_buffer[SUMA_BUF_SZ];
static Uint32 f_trigBufferSize = 0;
static Uint32 b_buffer[SUMA_BUF_SZ];
static Uint32 b_trigBufferSize = 0;

static bool clearBufferLock() {
  if (bufferLock == NO_LOCK_VAL) return false;

  bufferLock = NO_LOCK_VAL;

  return true;
}

static bool setBufferLock(Uint32 lockVal) {
  if (bufferLock != NO_LOCK_VAL) return false;

  bufferLock = lockVal;
  return true;
}

static bool setTriggerBufferLock(Uint32 triggerId) {
  return setBufferLock(triggerId | TRIGGER_LOCK_BASE);
}

static bool checkTriggerBufferLock(Uint32 triggerId) {
  return (bufferLock == (TRIGGER_LOCK_BASE | triggerId));
}

void Suma::execTRANSID_AI(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execTRANSID_AI");

  CRASH_INSERTION(13015);
  TransIdAI *const data = (TransIdAI *)signal->getDataPtr();
  const Uint32 opPtrI = data->connectPtr;
  Uint32 length = signal->length() - 3;

  if (signal->getNoOfSections()) {
    /* Copy long data into linear signal buffer */
    SectionHandle handle(this, signal);
    SegmentedSectionPtr dataPtr;
    ndbrequire(handle.getSection(dataPtr, 0));
    length = dataPtr.sz;
    ndbrequire(length <=
               (NDB_ARRAY_SIZE(signal->theData) - TransIdAI::HeaderLength));
    copy(data->attrData, dataPtr);
    releaseSections(handle);
  }

  Ptr<SyncRecord> syncPtr;
  ndbrequire(c_syncPool.getPtr(syncPtr, (opPtrI >> 16)));

  Uint32 headersSection = RNIL;
  Uint32 dataSection = RNIL;
  const Uint32 *src = &data->attrData[0];
  const Uint32 *const end = &src[length];

  const Uint32 attribs = syncPtr.p->m_currentNoOfAttributes;
  for (Uint32 i = 0; i < attribs; i++) {
    ndbrequire(src < end);
    Uint32 tmp = *src++;
    Uint32 len = AttributeHeader::getDataSize(tmp);
    ndbrequire((src + len) <= end);

    /**
     * Separate AttributeHeaders and data in separate
     * sections
     *
     * Note that len == 0 is legitimate, and can result in
     * dataSection == RNIL
     */
    if (!(appendToSection(headersSection, &tmp, 1) &&
          appendToSection(dataSection, src, len))) {
      ErrorReporter::handleError(NDBD_EXIT_OUT_OF_LONG_SIGNAL_MEMORY,
                                 "Out of LongMessageBuffer in SUMA scan", "");
    }
    src += len;
  }

  ndbrequire(src == end);
  ndbrequire(syncPtr.p->m_sourceInstance == RNIL);
  ndbrequire(syncPtr.p->m_headersSection == RNIL);
  ndbrequire(syncPtr.p->m_dataSection == RNIL);
  syncPtr.p->m_sourceInstance = refToInstance(signal->getSendersBlockRef());
  syncPtr.p->m_headersSection = headersSection;
  syncPtr.p->m_dataSection = dataSection;

  if ((syncPtr.p->m_requestInfo & SubSyncReq::LM_Exclusive) == 0) {
    /* Send it now */
    sendScanSubTableData(signal, syncPtr, 0);
  }

  /* Wait for KEYINFO20 */
  DBUG_VOID_RETURN;
}

void Suma::execKEYINFO20(Signal *signal) {
  jamEntry();
  KeyInfo20 *data = (KeyInfo20 *)signal->getDataPtr();

  const Uint32 opPtrI = data->clientOpPtr;
  const Uint32 takeOver = data->scanInfo_Node;

  Ptr<SyncRecord> syncPtr;
  ndbrequire(c_syncPool.getPtr(syncPtr, (opPtrI >> 16)));

  ndbrequire(syncPtr.p->m_sourceInstance ==
             refToInstance(signal->getSendersBlockRef()));
  ndbrequire(syncPtr.p->m_headersSection != RNIL);
  ndbrequire(syncPtr.p->m_dataSection != RNIL);

  /* SUMA requests a special 'scanInfo only' KeyInfo */
  ndbassert(data->keyLen == 0);
  ndbrequire(signal->getNoOfSections() == 0);

  sendScanSubTableData(signal, syncPtr, takeOver);
}

void Suma::sendScanSubTableData(Signal *signal, Ptr<SyncRecord> syncPtr,
                                Uint32 takeOver) {
  if (unlikely(syncPtr.p->m_dataSection == RNIL)) {
    jam();

    /* Zero length data section, but receivers expect
     * to get something :(
     * import() currently supports empty sections
     */
    Ptr<SectionSegment> emptySection;
    Uint32 junk = 0;
    if (!import(emptySection, &junk, 0)) {
      ErrorReporter::handleError(NDBD_EXIT_OUT_OF_LONG_SIGNAL_MEMORY,
                                 "Out of LongMessageBuffer in SUMA scan", "");
    }
    syncPtr.p->m_dataSection = emptySection.i;
  }

  ndbassert(syncPtr.p->m_headersSection != RNIL);
  ndbassert(syncPtr.p->m_dataSection != RNIL);

  /**
   * Send data to subscriber
   */
  SectionHandle sh(this);
  sh.m_ptr[0].i = syncPtr.p->m_headersSection;
  sh.m_ptr[1].i = syncPtr.p->m_dataSection;
  getSections(2, sh.m_ptr);
  sh.m_cnt = 2;

  /**
   * Initialize signal
   */
  SubTableData *sdata = (SubTableData *)signal->getDataPtrSend();
  Uint32 ref = syncPtr.p->m_senderRef;
  sdata->tableId = syncPtr.p->m_tableId;
  sdata->senderData = syncPtr.p->m_senderData;
  sdata->requestInfo = 0;
  SubTableData::setOperation(sdata->requestInfo,
                             NdbDictionary::Event::_TE_SCAN);  // Scan
  sdata->gci_hi = 0;                                           // Undefined
  sdata->gci_lo = 0;
  sdata->takeOver = takeOver;
#if PRINT_ONLY
  g_eventLogger->info("GSN_SUB_TABLE_DATA (scan) #attr: %d len: %d",
                      getSectionSz(syncPtr.p->m_headersSection),
                      getSectionSz(syncPtr.p->m_dataSection));
#else
  sendSignal(ref, GSN_SUB_TABLE_DATA, signal, SubTableData::SignalLength, JBB,
             &sh);
#endif

  /* Clear section references */
  syncPtr.p->m_sourceInstance = RNIL;
  syncPtr.p->m_headersSection = RNIL;
  syncPtr.p->m_dataSection = RNIL;
}

/**********************************************************
 *
 * Trigger data interface
 *
 */

void Suma::execTRIG_ATTRINFO(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execTRIG_ATTRINFO");

  CRASH_INSERTION(13016);
  TrigAttrInfo *const trg = (TrigAttrInfo *)signal->getDataPtr();
  const Uint32 trigId = trg->getTriggerId();

  const Uint32 dataLen = signal->length() - TrigAttrInfo::StaticLength;

  if (trg->getAttrInfoType() == TrigAttrInfo::BEFORE_VALUES) {
    jam();

    ndbrequire(checkTriggerBufferLock(trigId));

    ndbrequire(b_trigBufferSize + dataLen <= SUMA_BUF_SZ);
    memcpy(b_buffer + b_trigBufferSize, trg->getData(), 4 * dataLen);
    b_trigBufferSize += dataLen;

    // printf("before values %u %u %u\n",trigId, dataLen,  b_trigBufferSize);
  } else {
    jam();

    if (setTriggerBufferLock(trigId)) {
      /* Lock was not taken, we have it now */
      f_trigBufferSize = 0;
      b_trigBufferSize = 0;
    } else {
      /* Lock was taken, must be by us */
      ndbrequire(checkTriggerBufferLock(trigId));
    }

    ndbrequire(f_trigBufferSize + dataLen <= SUMA_BUF_SZ);
    memcpy(f_buffer + f_trigBufferSize, trg->getData(), 4 * dataLen);
    f_trigBufferSize += dataLen;
  }

  DBUG_VOID_RETURN;
}

#ifdef NODEFAIL_DEBUG2
static int theCounts[64] = {0};
#endif

Uint32 Suma::get_responsible_node(Uint32 bucket) const {
  // id will contain id to responsible suma or
  // RNIL if we don't have nodegroup info yet

  jam();
  Uint32 node;
  ndbrequire(bucket < NO_OF_BUCKETS);
  const Bucket *ptr = c_buckets + bucket;
  for (Uint32 i = 0; i < c_noNodesInGroup; i++) {
    node = ptr->m_nodes[i];
    if (c_alive_nodes.get(node)) {
#ifdef NODEFAIL_DEBUG2
      theCounts[node]++;
      g_eventLogger->info("Suma:responsible n=%u, D=%u, id = %u, count=%u", n,
                          D, id, theCounts[node]);
#endif
      return node;
    }
  }

  return 0;
}

Uint32 Suma::get_responsible_node(Uint32 bucket,
                                  const NdbNodeBitmask &mask) const {
  jam();
  Uint32 node;
  ndbrequire(bucket < NO_OF_BUCKETS);
  const Bucket *ptr = c_buckets + bucket;
  for (Uint32 i = 0; i < c_noNodesInGroup; i++) {
    node = ptr->m_nodes[i];
    if (mask.get(node)) {
      return node;
    }
  }

  return 0;
}

bool Suma::check_switchover(Uint32 bucket, Uint64 gci) {
  const Uint32 send_mask = Bucket::BUCKET_STARTING | Bucket::BUCKET_TAKEOVER |
                           Bucket::BUCKET_SHUTDOWN_TO;

  bool send = c_buckets[bucket].m_state & send_mask;
  ndbassert(m_switchover_buckets.get(bucket));
  if (unlikely(gci > c_buckets[bucket].m_switchover_gci)) {
    return send;
  }
  return !send;
}

Uint32 Suma::reformat(Signal *signal, LinearSectionPtr ptr[3],
                      const LinearSectionPtr lsptr[3]) {
  jam();
  Uint32 noOfAttrs = 0, dataLen = 0;
  Uint32 *headers = signal->theData + 25;
  Uint32 *dst = signal->theData + 25 + MAX_ATTRIBUTES_IN_TABLE;

  require(lsptr[0].sz + lsptr[2].sz <= SUMA_BUF_SZ);

  ptr[0].p = headers;
  ptr[1].p = dst;

  for (Uint32 i = 0; i < 2; i++) {
    jam();
    const Uint32 secnum = (i == 0 ? 0 : 2);
    const Uint32 *p = lsptr[secnum].p;
    Uint32 sz = lsptr[secnum].sz;
    while (sz > 0) {
      jamDebug();
      Uint32 tmp = *p++;
      *headers++ = tmp;
      Uint32 len = AttributeHeader::getDataSize(tmp);
      memcpy(dst, p, 4 * len);
      dst += len;
      p += len;

      noOfAttrs++;
      dataLen += len;
      require(sz >= (1 + len));
      sz -= (1 + len);
    }
  }

  ptr[0].sz = noOfAttrs;
  ptr[1].sz = dataLen;

  ptr[2] = lsptr[1];

  return ptr[2].sz > 0 ? 3 : 2;
}

/**
 * Pass entire pages with SUMA-trigger-data from
 *   TUP to SUMA to avoid extensive LongSignalMessage buffer contention
 */
void Suma::execFIRE_TRIG_ORD_L(Signal *signal) {
  jamEntry();

  ndbassert(signal->getNoOfSections() == 0);
  Uint32 pageId = signal->theData[0];
  Uint32 len = signal->theData[1];
  const Uint32 page_count =
      (len + GLOBAL_PAGE_SIZE_WORDS - 1) / GLOBAL_PAGE_SIZE_WORDS;

  if (pageId == RNIL && len == 0) {
    jam();
    /**
     * Out of memory
     */
    out_of_buffer(signal);
    return;
  }

  Uint32 *ptr = reinterpret_cast<Uint32 *>(c_page_pool.getPtr(pageId));
  while (len) {
    Uint32 *save = ptr;
    Uint32 msglen = *ptr++;
    Uint32 siglen = *ptr++;
    Uint32 sec0len = *ptr++;
    Uint32 sec1len = *ptr++;
    Uint32 sec2len = *ptr++;

    /**
     * Copy value directly into local buffers
     */
    Uint32 trigId = ((FireTrigOrd *)ptr)->getTriggerId();
    ndbrequire(setTriggerBufferLock(trigId));

    LinearSectionPtr lsptr[3];
    memcpy(signal->theData, ptr, 4 * siglen);  // signal
    ptr += siglen;

    memcpy(f_buffer, ptr, 4 * sec0len);
    lsptr[0].sz = sec0len;
    lsptr[0].p = f_buffer;
    ptr += sec0len;

    memcpy(b_buffer, ptr, 4 * sec1len);
    lsptr[1].sz = sec1len;
    lsptr[1].p = b_buffer;
    ptr += sec1len;

    memcpy(f_buffer + sec0len, ptr, 4 * sec2len);
    lsptr[2].sz = sec2len;
    lsptr[2].p = f_buffer + sec0len;
    ptr += sec2len;

    f_trigBufferSize = sec0len + sec2len;
    b_trigBufferSize = sec1len;

    doFIRE_TRIG_ORD(signal, lsptr);

    /* Reset bufferlock
     * We will use the buffers until the end of
     * signal processing, but not after
     */
    ndbrequire(clearBufferLock());

    ndbrequire(ptr == save + msglen);
    ndbrequire(len >= msglen);
    len -= msglen;
  }

  // Pages allocated in Dbtup::ndbmtd_buffer_suma_trigger().
  m_ctx.m_mm.release_pages(RT_SUMA_TRIGGER_BUFFER, pageId, page_count);
}

/**
 * Apply the subscriber filters using the anyValue.
 * Return true if deemed to discard, false to keep.
 */
bool Suma::applyAnyValueFilters(Uint32 requestInfo, Uint32 anyValue) const {
  constexpr Uint32 ANYVALUE_RESERVED_BIT = 0x80000000;
  constexpr Uint32 ANYVALUE_NOLOGGING_VALUE = 0x8000007f;

  // No-logging filter
  const bool no_logging =
      requestInfo & SubStartReq::FILTER_ANYVALUE_MYSQL_NO_LOGGING;
  if (no_logging && (anyValue == ANYVALUE_NOLOGGING_VALUE)) return true;

  // No-replica-updates filter
  const bool no_replica_updates =
      requestInfo & SubStartReq::FILTER_ANYVALUE_MYSQL_NO_REPLICA_UPDATES;
  if (no_replica_updates && ((anyValue & ANYVALUE_RESERVED_BIT) == 0) &&
      ((anyValue & ~ANYVALUE_RESERVED_BIT) != 0))
    return true;

  return false;
}

void Suma::sendBatchedSUB_TABLE_DATA(Signal *signal,
                                     const Subscriber_list::Head subscribers,
                                     LinearSectionPtr lsptr[], Uint32 nptr) {
  jam();
  SubTableData *data = (SubTableData *)signal->getDataPtrSend();
  ConstLocal_Subscriber_list list(c_subscriberPool, subscribers);
  SubscriberPtr subbPtr;

  for (list.first(subbPtr); !subbPtr.isNull(); list.next(subbPtr)) {
    jam();
    data->senderData = subbPtr.p->m_senderData;

    // filter anyValue through subscriber options
    if (applyAnyValueFilters(subbPtr.p->m_requestInfo, data->anyValue))
      continue;

    const Uint32 version =
        getNodeInfo(refToNode(subbPtr.p->m_senderRef)).m_version;
    if (ndbd_frag_sub_table_data(version)) {
      jam();
      sendBatchedFragmentedSignal(subbPtr.p->m_senderRef, GSN_SUB_TABLE_DATA,
                                  signal, SubTableData::SignalLengthWithTransId,
                                  JBB, lsptr, nptr);
    } else {
      jam();
      sendSignal(subbPtr.p->m_senderRef, GSN_SUB_TABLE_DATA, signal,
                 SubTableData::SignalLengthWithTransId, JBB, lsptr, nptr);
    }
  }
}

void Suma::execFIRE_TRIG_ORD(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execFIRE_TRIG_ORD");

  CRASH_INSERTION(13016);

  if (!assembleFragments(signal)) {
    jam();
    return;
  }

  FireTrigOrd *const trg = (FireTrigOrd *)signal->getDataPtr();
  const Uint32 trigId = trg->getTriggerId();

  LinearSectionPtr lsptr[3];
  if (signal->getNoOfSections() > 0) {
    jam();
    ndbassert(isNdbMtLqh());
    SectionHandle handle(this, signal);

    ndbrequire(setTriggerBufferLock(trigId));

    SegmentedSectionPtr ptr;
    ndbrequire(handle.getSection(ptr, 0));  // Keys
    const Uint32 sz = ptr.sz;
    ndbrequire(sz <= SUMA_BUF_SZ);
    copy(f_buffer, ptr);
    lsptr[0].sz = ptr.sz;
    lsptr[0].p = f_buffer;

    ndbrequire(handle.getSection(ptr, 2));  // After values
    ndbrequire(ptr.sz <= (SUMA_BUF_SZ - sz));
    copy(f_buffer + sz, ptr);
    f_trigBufferSize = sz + ptr.sz;
    lsptr[2].sz = ptr.sz;
    lsptr[2].p = f_buffer + sz;

    ndbrequire(handle.getSection(ptr, 1));  // Before values
    ndbrequire(ptr.sz <= SUMA_BUF_SZ);
    copy(b_buffer, ptr);
    b_trigBufferSize = ptr.sz;
    lsptr[1].sz = ptr.sz;
    lsptr[1].p = b_buffer;

    releaseSections(handle);
  } else {
    jam();
    /*
     * If no sections, assume f_buffer and b_buffer already have been filled
     * and trigger buffer is locked by preeding sequence of TRIG_ATTRINFO.
     */
    ndbrequire(f_trigBufferSize ==
               trg->getNoOfPrimaryKeyWords() + trg->getNoOfAfterValueWords());
    ndbrequire(b_trigBufferSize == trg->getNoOfBeforeValueWords());
    lsptr[0].p = f_buffer;
    lsptr[0].sz = trg->getNoOfPrimaryKeyWords();
    lsptr[1].p = b_buffer;
    lsptr[1].sz = trg->getNoOfBeforeValueWords();
    lsptr[2].p = f_buffer + trg->getNoOfPrimaryKeyWords();
    lsptr[2].sz = trg->getNoOfAfterValueWords();
  }

  ndbrequire(checkTriggerBufferLock(trigId));

  doFIRE_TRIG_ORD(signal, lsptr);

  /* Reset bufferlock
   * We will use the buffers until the end of
   * signal processing, but not after
   */
  ndbrequire(clearBufferLock());
}

void Suma::doFIRE_TRIG_ORD(Signal *signal, LinearSectionPtr lsptr[3]) {
  jamEntry();
  DBUG_ENTER("Suma::doFIRE_TRIG_ORD");

  FireTrigOrd *const trg = (FireTrigOrd *)signal->getDataPtr();
  const Uint32 trigId = trg->getTriggerId();
  const Uint32 hashValue = trg->getHashValue();
  const Uint32 gci_hi = trg->getGCI();
  const Uint32 gci_lo = trg->m_gci_lo;
  const Uint64 gci = gci_lo | (Uint64(gci_hi) << 32);
  const Uint32 event = trg->getTriggerEvent();
  const Uint32 any_value = trg->getAnyValue();
  const Uint32 transId1 = trg->m_transId1;
  const Uint32 transId2 = trg->m_transId2;

  Ptr<Subscription> subPtr;
  ndbrequire(c_subscriptionPool.getPtr(subPtr, trigId & 0xFFFF));

  ndbrequire(gci > m_last_complete_gci);

  Uint32 tableId = subPtr.p->m_tableId;
  Uint32 schemaVersion =
      c_tablePool.getPtr(subPtr.p->m_table_ptrI)->m_schemaVersion;

  Uint32 bucket = hashValue % c_no_of_buckets;
  m_max_seen_gci = (gci > m_max_seen_gci ? gci : m_max_seen_gci);
  /**
   * Normally a bucket is either in the active or switchover set, or neither.
   * Exception is during add/drop NG when an active bucket may be in the
   * switchover set for notifying subscribers of change.
   */
  if (m_active_buckets.get(bucket) ||
      (m_switchover_buckets.get(bucket) && (check_switchover(bucket, gci)))) {
    jam();
    m_max_sent_gci = (gci > m_max_sent_gci ? gci : m_max_sent_gci);
    ndbrequire(lsptr[0].sz == trg->getNoOfPrimaryKeyWords());
    ndbrequire(lsptr[1].sz == trg->getNoOfBeforeValueWords());
    ndbrequire(lsptr[2].sz == trg->getNoOfAfterValueWords());

    LinearSectionPtr ptr[3];
    const Uint32 nptr = reformat(signal, ptr, lsptr);
    Uint32 ptrLen = 0;
    for (Uint32 i = 0; i < nptr; i++) ptrLen += ptr[i].sz;
    /**
     * Signal to subscriber(s)
     */
    SubTableData *data = (SubTableData *)signal->getDataPtrSend();  // trg;
    data->gci_hi = gci_hi;
    data->gci_lo = gci_lo;
    data->tableId = tableId;
    data->requestInfo = 0;
    SubTableData::setOperation(data->requestInfo, event);
    data->flags = 0;
    data->anyValue = any_value;
    data->totalLen = ptrLen;
    data->transId1 = transId1;
    data->transId2 = transId2;

    sendBatchedSUB_TABLE_DATA(signal, subPtr.p->m_subscribers, ptr, nptr);
  } else {
    jam();
    constexpr uint buffer_header_sz = 6;
    Uint32 *dst1 = nullptr;
    Uint32 *dst2 = nullptr;
    Uint32 sz1 = f_trigBufferSize + buffer_header_sz;
    Uint32 sz2 = b_trigBufferSize;

    static_assert(1 + Buffer_page::GCI_SZ32 + buffer_header_sz + SUMA_BUF_SZ <=
                  Buffer_page::DATA_WORDS);

    if (unlikely(buffering_disabled())) {
      jam();
      DBUG_VOID_RETURN;
    }

    dst1 = get_buffer_ptr(signal, bucket, gci, sz1, 1);
    if (unlikely(buffering_disabled())) {
      jam();
      DBUG_VOID_RETURN;
    }

    dst2 = get_buffer_ptr(signal, bucket, gci, sz2, 2);
    if (unlikely(buffering_disabled())) {
      jam();
      DBUG_VOID_RETURN;
    }

    ndbrequire(dst1 != nullptr && dst2 != nullptr);

    jam();
    dst1[0] = subPtr.i;
    dst1[1] = schemaVersion;
    dst1[2] = (event << 16) | lsptr[0].sz;
    dst1[3] = any_value;
    dst1[4] = transId1;
    dst1[5] = transId2;
    dst1 += buffer_header_sz;
    memcpy(dst1, lsptr[0].p, lsptr[0].sz << 2);
    dst1 += lsptr[0].sz;
    memcpy(dst1, lsptr[2].p, lsptr[2].sz << 2);
    ndbrequire(f_trigBufferSize == lsptr[0].sz + lsptr[2].sz);
    memcpy(dst2, lsptr[1].p, lsptr[1].sz << 2);
    ndbrequire(b_trigBufferSize == lsptr[1].sz);
  }

  DBUG_VOID_RETURN;
}

/**
  The SUB_GCP_COMPLETE_REP signal is used to signal to receiver that an epoch
  is completed and that receiver have received all data for the epoch.

  SUMA get SUB_GCP_COMPLETE_REP from DBDIH via DBLQH so that
  SUB_GCP_COMPLETE_REP for an certain epoch will arrive after any data for that
  epoch that each DBLQH instance have sent to SUMA (via FIRE_TRIG_ORD or
  FIRE_TRIG_ORD_L).

  If one have more than one DBLQH they can be at different epochs so SUMA need
  to count in SUB_GCP_COMPLETE_REP from all DBLQH to know when SUMA have got
  all the data from all DBLQH.

  When SUMA have got SUB_GCP_COMPLETE_REP from all DBLQH it should in turn send
  out SUB_GCP_COMPLETE_REP to all subscribers.  But only after it have sent all
  data to the subscribers.

  Typically SUMA relays the data it got from DBLQH and DBDICT immediately when
  it arrives to SUMA.  But in some cases fragmented signals may be used to send
  data to subscribers.  These fragmented signals should not be considered sent
  until the last fragment of signal is sent.
*/

void Suma::checkMaxBufferedEpochs(Signal *signal) {
  /*
   * Check if any subscribers are exceeding the MaxBufferedEpochs
   */
  Ptr<Gcp_record> gcp;
  jamEntry();
  if (c_gcp_list.isEmpty()) {
    jam();
    return;
  }
  c_gcp_list.first(gcp);
  if (ERROR_INSERTED(13037)) {
    jam();
    CLEAR_ERROR_INSERT_VALUE;
    g_eventLogger->info(
        "Simulating exceeding the MaxBufferedEpochs %u(%llu,%llu,%llu)",
        c_maxBufferedEpochs, m_max_seen_gci, m_last_complete_gci, gcp.p->m_gci);
  } else if (c_gcp_list.getCount() < c_maxBufferedEpochs) {
    return;
  }
  NodeBitmask subs = gcp.p->m_subscribers;
  jam();
  if (!subs.isclear()) {
    char buf[NodeBitmask::TextLength + 1];
    subs.getText(buf);
    infoEvent("Disconnecting lagging nodes '%s', epoch %llu", buf,
              gcp.p->m_gci);
    g_eventLogger->info("SUMA Disconnecting lagging nodes '%s', epoch %llu",
                        buf, gcp.p->m_gci);
  }
  // Disconnect lagging subscribers waiting for oldest epoch
  for (Uint32 nodeId = 0; nodeId < MAX_NODES; nodeId++) {
    if (subs.get(nodeId)) {
      jam();
      subs.clear(nodeId);
      // Disconnecting node
      signal->theData[0] = NDB_LE_SubscriptionStatus;
      signal->theData[1] = 1;  // DISCONNECTED;
      signal->theData[2] = nodeId;
      signal->theData[3] = (Uint32)gcp.p->m_gci;
      signal->theData[4] = (Uint32)(gcp.p->m_gci >> 32);
      signal->theData[5] = (Uint32)c_gcp_list.getCount();
      signal->theData[6] = c_maxBufferedEpochs;
      sendSignal(CMVMI_REF, GSN_EVENT_REP, signal, 8, JBB);

      /**
       * Force API_FAILREQ
       */
      signal->theData[0] = nodeId;
      sendSignal(QMGR_REF, GSN_API_FAILREQ, signal, 1, JBA);
    }
  }
}

void Suma::execSUB_GCP_COMPLETE_REP(Signal *signal) {
  jamEntry();
  ndbassert(signal->getNoOfSections() == 0);

  SubGcpCompleteRep *rep = (SubGcpCompleteRep *)signal->getDataPtrSend();
  Uint32 gci_hi = rep->gci_hi;
  Uint32 gci_lo = rep->gci_lo;
  Uint64 gci = gci_lo | (Uint64(gci_hi) << 32);

#define SSPP 0

  if (SSPP) printf("execSUB_GCP_COMPLETE_REP(%u/%u)", gci_hi, gci_lo);
  jam();

  const Uint32 sz = NDB_ARRAY_SIZE(m_gcp_rep_counter);
  bool found = false;

  ndbrequire(m_snd_gcp_rep_counter_index < sz);
  ndbrequire(m_min_gcp_rep_counter_index < sz);
  ndbrequire(m_max_gcp_rep_counter_index < sz);

  Uint32 i = m_min_gcp_rep_counter_index;
  while (i != m_max_gcp_rep_counter_index) {
    if (gci < m_gcp_rep_counter[i].m_gci) {
      break;
    } else if (gci == m_gcp_rep_counter[i].m_gci) {
      found = true;
      break;
    }
    i = (i + 1) % sz;
  }

  /**
    If ndbrequire fails, epoch already completed.  This should not be possible.
    Each LDM sends message exactly once per epoch.  And an epoch is not
    complete until message from all LDM have arrived.
  */
  ndbrequire(found || i != m_min_gcp_rep_counter_index ||
             i == m_max_gcp_rep_counter_index);

  if (!found) {
    ndbrequire(i == m_max_gcp_rep_counter_index);
    m_gcp_rep_counter[i].m_gci = gci;
    m_gcp_rep_counter[i].m_cnt = 0;
    m_gcp_rep_counter[i].m_flags = rep->flags;
    m_max_gcp_rep_counter_index = (m_max_gcp_rep_counter_index + 1) % sz;

    // Verify epoch buffer not full, else panic!
    ndbrequire(m_snd_gcp_rep_counter_index != m_max_gcp_rep_counter_index);

    if (gci > m_max_seen_gci) {
      m_max_seen_gci = gci;
    }
  }

  if (likely(m_gcp_rep_cnt > 0)) {
    ndbrequire(m_gcp_rep_counter[i].m_cnt < m_gcp_rep_cnt);
    m_gcp_rep_counter[i].m_cnt++;
  } else {
    ndbrequire(m_gcp_rep_counter[i].m_cnt == 0);
  }
  ndbrequire(m_gcp_rep_counter[i].m_flags == rep->flags);

  if (m_gcp_rep_counter[i].m_cnt < m_gcp_rep_cnt) {
    return;
  }

  // Epoch is completed.
  ndbrequire(gci > m_last_complete_gci);
  m_last_complete_gci = gci;
  ndbrequire(m_gcp_rep_counter[i].m_cnt == m_gcp_rep_cnt);
  ndbrequire(i == m_min_gcp_rep_counter_index);
  m_min_gcp_rep_counter_index = (m_min_gcp_rep_counter_index + 1) % sz;
  if (i != m_snd_gcp_rep_counter_index) {
    return;
  }
  checkMaxBufferedEpochs(signal);
  sendSUB_GCP_COMPLETE_REP(signal);
}

void Suma::sendSUB_GCP_COMPLETE_REP(Signal *signal) {
  if (m_snd_gcp_rep_counter_index == m_min_gcp_rep_counter_index) {
    // No complete epoch yet.
    return;
  }

  const Uint64 gci = m_gcp_rep_counter[m_snd_gcp_rep_counter_index].m_gci;
  const Uint32 gci_hi = gci >> 32;
  const Uint32 gci_lo = Uint32(gci);
  ndbrequire(((Uint64(gci_hi) << 32) | gci_lo) == gci);

  const bool no_inflight_gci =
      (m_oldest_gcp_inflight_index == m_newest_gcp_inflight_index);

  if (!no_inflight_gci) {
    if (gci == m_gcp_inflight[m_oldest_gcp_inflight_index].m_gci) {
      // Do not send yet, epoch have undelivered data.
      return;
    }
    ndbrequire(gci < m_gcp_inflight[m_oldest_gcp_inflight_index].m_gci);
  }

  // Send!
  ndbassert(m_gcp_rep_counter[m_snd_gcp_rep_counter_index].m_cnt ==
            m_gcp_rep_cnt);

  Uint32 flags = m_gcp_rep_counter[m_snd_gcp_rep_counter_index].m_flags |
                 (m_missing_data ? SubGcpCompleteRep::MISSING_DATA : 0);
  if (ERROR_INSERTED(13036)) {
    jam();
    CLEAR_ERROR_INSERT_VALUE;
    g_eventLogger->info("Simulating out of event buffer at node failure");
    flags |= SubGcpCompleteRep::MISSING_DATA;
  }

#ifdef VM_TRACE
  if (m_gcp_monitor == 0) {
  } else if (gci_hi == Uint32(m_gcp_monitor >> 32)) {
    ndbrequire(gci_lo == Uint32(m_gcp_monitor) + 1);
  } else {
    if (ERROR_INSERTED(13057)) {
      jam();
      ndbrequire(gci_hi > Uint32(m_gcp_monitor >> 32));
    } else {
      ndbrequire(gci_hi == Uint32(m_gcp_monitor >> 32) + 1);
    }
    ndbrequire(gci_lo == 0);
  }
  m_gcp_monitor = gci;
#endif

  /**
   *
   */
  bool drop = false;
  Bucket_mask dropped_buckets;
  if (!m_switchover_buckets.isclear()) {
    bool starting_unlock = false;
    Uint32 i = m_switchover_buckets.find(0);
    for (; i != Bucket_mask::NotFound; i = m_switchover_buckets.find(i + 1)) {
      if (gci > c_buckets[i].m_switchover_gci) {
        Uint32 state = c_buckets[i].m_state;
        m_switchover_buckets.clear(i);
        g_eventLogger->info(
            "%u/%u (%u/%u) switchover complete bucket %d state: %x",
            Uint32(gci >> 32), Uint32(gci),
            Uint32(c_buckets[i].m_switchover_gci >> 32),
            Uint32(c_buckets[i].m_switchover_gci), i, state);

        if (state & Bucket::BUCKET_STARTING) {
          /**
           * NR case
           */
          jam();
          m_active_buckets.set(i);
          c_buckets[i].m_state &= ~(Uint32)Bucket::BUCKET_STARTING;
          g_eventLogger->info("starting");
          m_gcp_complete_rep_count++;
          starting_unlock = true;
        } else if (state & Bucket::BUCKET_TAKEOVER) {
          /**
           * NF case
           */
          jam();
          Bucket *bucket = c_buckets + i;
          Page_pos pos = bucket->m_buffer_head;

          /**
           * Initialize the bucket to signal resend that no more
           * buffering will take place. Buckets are also initialized
           * when out-of-buffer release completes. Initialized state
           * prevails until buffering is resumed.
           */
          if (!buffering_disabled() && pos.m_page_id != RNIL) {
            ndbrequire(pos.m_max_gci < gci);
            Buffer_page *page = c_page_pool.getPtr(pos.m_page_id);
            page->m_max_gci_hi = (Uint32)(pos.m_max_gci >> 32);
            page->m_max_gci_lo = (Uint32)(pos.m_max_gci & 0xFFFFFFFF);
            ndbassert(pos.m_max_gci != 0);
            page->m_words_used = pos.m_page_pos;
            page->m_next_page = RNIL;
            memset(&bucket->m_buffer_head, 0, sizeof(bucket->m_buffer_head));
            bucket->m_buffer_head.m_page_id = RNIL;
            bucket->m_buffer_head.m_page_pos = Buffer_page::DATA_WORDS + 1;
          }

          m_active_buckets.set(i);
          m_gcp_complete_rep_count++;
          g_eventLogger->info("Takeover for bucket %u is completed", i);
          c_buckets[i].m_state &= ~(Uint32)Bucket::BUCKET_TAKEOVER;
        } else if (state & Bucket::BUCKET_HANDOVER) {
          /**
           * NR, living node
           */
          jam();
          c_buckets[i].m_state &= ~(Uint32)Bucket::BUCKET_HANDOVER;
          m_gcp_complete_rep_count--;
          g_eventLogger->info("handover");
        } else if (state & Bucket::BUCKET_CREATED_MASK) {
          jam();
          Uint32 cnt = state >> 8;
          Uint32 mask = Uint32(Bucket::BUCKET_CREATED_MASK) | (cnt << 8);
          c_buckets[i].m_state &= ~mask;
          flags |= SubGcpCompleteRep::ADD_CNT;
          flags |= (cnt << 16);
          g_eventLogger->info(
              "add %u %s", cnt,
              state & Bucket::BUCKET_CREATED_SELF ? "self" : "other");
          if (state & Bucket::BUCKET_CREATED_SELF &&
              get_responsible_node(i) == getOwnNodeId()) {
            jam();
            m_active_buckets.set(i);
            m_gcp_complete_rep_count++;
          }
        } else if (state & Bucket::BUCKET_DROPPED_MASK) {
          jam();
          Uint32 cnt = state >> 8;
          Uint32 mask = Uint32(Bucket::BUCKET_DROPPED_MASK) | (cnt << 8);
          c_buckets[i].m_state &= ~mask;
          flags |= SubGcpCompleteRep::SUB_CNT;
          flags |= (cnt << 16);
          g_eventLogger->info(
              "sub %u %s", cnt,
              state & Bucket::BUCKET_DROPPED_SELF ? "self" : "other");
          if (state & Bucket::BUCKET_DROPPED_SELF) {
            if (m_active_buckets.get(i)) {
              m_active_buckets.clear(i);
              // Remember this bucket, it should be listed
              // in SUB_GCP_COMPLETE_REP signal
              dropped_buckets.set(i);
            }
            drop = true;
          }
        } else if (state & Bucket::BUCKET_SHUTDOWN) {
          jam();
          Uint32 nodeId = c_buckets[i].m_switchover_node;
          ndbrequire(nodeId == getOwnNodeId());
          m_active_buckets.clear(i);
          m_gcp_complete_rep_count--;
          g_eventLogger->info("shutdown handover");
          c_buckets[i].m_state &= ~(Uint32)Bucket::BUCKET_SHUTDOWN;
        } else if (state & Bucket::BUCKET_SHUTDOWN_TO) {
          jam();
          Uint32 nodeId = c_buckets[i].m_switchover_node;
          NdbNodeBitmask nodegroup = c_nodes_in_nodegroup_mask;
          nodegroup.clear(nodeId);
          ndbrequire(get_responsible_node(i) == nodeId &&
                     get_responsible_node(i, nodegroup) == getOwnNodeId());
          m_active_buckets.set(i);
          m_gcp_complete_rep_count++;
          c_buckets[i].m_state &= ~(Uint32)Bucket::BUCKET_SHUTDOWN_TO;
          g_eventLogger->info("shutdown handover takeover");
        }
      }
    }  // for (m_switchover_buckets...)

    if (m_switchover_buckets.isclear()) {
      jam();
      if (getNodeState().startLevel == NodeState::SL_STARTING &&
          c_startup.m_handover_nodes.isclear()) {
        jam();
        sendSTTORRY(signal);
      } else if (getNodeState().startLevel >= NodeState::SL_STOPPING_1) {
        jam();
        if (c_shutdown.m_wait_handover) {
          jam();
          ndbassert(getNodeState().startLevel == NodeState::SL_STOPPING_3);
          StopMeConf *conf = CAST_PTR(StopMeConf, signal->getDataPtrSend());
          conf->senderData = c_shutdown.m_senderData;
          conf->senderRef = reference();
          sendSignal(c_shutdown.m_senderRef, GSN_STOP_ME_CONF, signal,
                     StopMeConf::SignalLength, JBB);
          c_shutdown.m_wait_handover = false;
          infoEvent("Suma: handover complete");
        }
        /* else
         * either we have not received GSN_STOP_ME_REQ yet
         * or we have already sent STOP_ME_CONF, but we have completed
         * the takeover of the peer node's buckets just now.
         */
      } else if (ERROR_INSERTED(13061)) {
        jam();
        g_eventLogger->info(
            "Takeover provokes OOB and cleared "
            "error insertion 13061");
        CLEAR_ERROR_INSERT_VALUE;
        out_of_buffer(signal);
      }
    }

    /**
     * Check whether we have now completed handover of all starting
     * buckets, and should send a DICT_UNLOCK_ORD
     */
    if (starting_unlock) {
      jam();
      /* All pending SUMA_HANDOVER_CONF received ?*/
      if (c_startup.m_handover_nodes.isclear()) {
        jam();
        /* All bucket handovers completed? */
        Uint32 i = m_switchover_buckets.find(0);
        for (; i != Bucket_mask::NotFound;
             i = m_switchover_buckets.find(i + 1)) {
          if (c_buckets[i].m_state & Bucket::BUCKET_STARTING) {
            jam();
            /* Some other bucket still to handover, don't unlock yet */
            starting_unlock = false;
            break;
          }
        }

        if (starting_unlock) {
          jam();
          send_dict_unlock_ord(signal, DictLockReq::SumaHandOver);
        }
      }
    }
  }

  if (ERROR_INSERTED(13010)) {
    CLEAR_ERROR_INSERT_VALUE;
    g_eventLogger->info("Don't send GCP_COMPLETE_REP(%llu)", gci);
    return;
  }

  /**
   * Signal to subscribers
   */
  SubGcpCompleteRep *rep = (SubGcpCompleteRep *)signal->theData;
  rep->gci_hi = gci_hi;
  rep->gci_lo = gci_lo;
  rep->flags = flags;
  rep->senderRef = reference();
  rep->gcp_complete_rep_count = m_gcp_complete_rep_count;

  /**
   * Append the identifiers of the data streams that this Suma has
   * completed for the gcp.
   * The subscribers can use that to identify duplicates or lack
   * of reception.
   */
  Uint32 siglen = SubGcpCompleteRep::SignalLength;

  Uint32 stream_count = 0;
  for (Uint32 bucket = 0; bucket < NO_OF_BUCKETS; bucket++) {
    if (m_active_buckets.get(bucket) || dropped_buckets.get(bucket) ||
        (m_switchover_buckets.get(bucket) && (check_switchover(bucket, gci)))) {
      Uint32 sub_data_stream = get_sub_data_stream(bucket);
      if ((stream_count & 1) == 0) {
        rep->sub_data_streams[stream_count / 2] = sub_data_stream;
      } else {
        rep->sub_data_streams[stream_count / 2] |= sub_data_stream << 16;
      }
      stream_count++;
    }
  }

  /**
   * If count match the number of buckets that should be reported
   * complete, send subscription data streams identifiers.
   * If this is not the case fallback on old signal without
   * the streams identifiers, but that should not happen!
   */
  if (stream_count == m_gcp_complete_rep_count) {
    rep->flags |= SubGcpCompleteRep::SUB_DATA_STREAMS_IN_SIGNAL;
    siglen += (stream_count + 1) / 2;
  } else {
    g_eventLogger->error(
        "Suma gcp complete rep count (%u) does "
        "not match number of buckets that should "
        "be reported complete (%u).",
        m_gcp_complete_rep_count, stream_count);
    ndbassert(false);
  }

  if (m_gcp_complete_rep_count && !c_subscriber_nodes.isclear()) {
    CRASH_INSERTION(13033);

    if (ERROR_INSERTED(13062)) {
      jam();
      g_eventLogger->info(
          "sendSUB_GCP_COMPLETE_REP: error insertion 13062 "
          "not sending GCP_COMPLETE to SUBSCRIBERS, "
          "but crashing node at %u/%u",
          gci_hi, gci_lo);
      CRASH_INSERTION(13062);
    }
    NodeReceiverGroup rg(API_CLUSTERMGR, c_subscriber_nodes);
    sendSignal(rg, GSN_SUB_GCP_COMPLETE_REP, signal, siglen, JBB);

    Ptr<Gcp_record> gcp;
    if (c_gcp_list.seizeLast(gcp)) {
      gcp.p->m_gci = gci;
      gcp.p->m_subscribers = c_subscriber_nodes;
    } else {
      char buf[NodeBitmask::TextLength + 1];
      c_subscriber_nodes.getText(buf);
      g_eventLogger->error("c_gcp_list.seize() failed: gci: %llu nodes: %s",
                           gci, buf);
    }
  }

  /**
   * Add GCP COMPLETE REP to buffer
   */
  const bool subscribers = !c_subscriber_nodes.isclear();
  for (Uint32 i = 0; i < c_no_of_buckets; i++) {
    if (m_active_buckets.get(i)) continue;

    if (subscribers || (c_buckets[i].m_state & Bucket::BUCKET_RESEND)) {
      if (!buffering_disabled()) {
        get_buffer_ptr(signal, i, gci, 0, 0);
      } else {
        jam();
        break;
      }
    }
  }

  if ((m_fully_buffering_after_gci > 0 &&
       m_fully_buffering_after_gci < UINT64_MAX) &&
      gci > m_fully_buffering_after_gci) {
    jam();
    infoEvent(
        "Event buffering resumed. Epochs > %u/%u will be buffered completely.",
        (Uint32)(gci >> 32), (Uint32)gci);

    // 'gci' will be the lowest resendable epoch after resuming buffering
    g_eventLogger->info(
        "SUMA The first fully buffered epoch (%u/%u), "
        "also flagged as MISSING_DATA, after resuming "
        "buffering is GCP-completed. "
        "m_fully_buffering_after %u/%u",
        (Uint32)(gci >> 32), (Uint32)gci,
        (Uint32)(m_fully_buffering_after_gci >> 32),
        (Uint32)m_fully_buffering_after_gci);

    m_missing_data = false;
    enable_normal_resending();
  }

  if (unlikely(drop)) {
    jam();
    m_gcp_complete_rep_count = 0;
    c_nodeGroup = RNIL;
    c_nodes_in_nodegroup_mask.clear();
    fix_nodegroup();
  }

  m_gcp_rep_counter[m_snd_gcp_rep_counter_index].m_gci = 0;
  m_gcp_rep_counter[m_snd_gcp_rep_counter_index].m_cnt = 0;
  m_gcp_rep_counter[m_snd_gcp_rep_counter_index].m_flags = 0;
  m_snd_gcp_rep_counter_index =
      (m_snd_gcp_rep_counter_index + 1) % NDB_ARRAY_SIZE(m_gcp_rep_counter);
  if (m_snd_gcp_rep_counter_index != m_min_gcp_rep_counter_index) {
    signal->theData[0] = SumaContinueB::SEND_SUB_GCP_COMPLETE_REP;
    sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 1, JBB);
  }
}

Uint32 Suma::mark_epoch_inflight(Uint64 gci) {
  const Uint32 sz = NDB_ARRAY_SIZE(m_gcp_inflight);
  bool found = false;
  Uint32 i = m_oldest_gcp_inflight_index;
  while (i != m_newest_gcp_inflight_index) {
    if (m_gcp_inflight[i].m_gci == gci) {
      found = true;
      break;
    }
    ndbrequire(gci > m_gcp_inflight[i].m_gci);
    i = (i + 1) % sz;
  }
  if (!found) {
    m_gcp_inflight[i].m_gci = gci;
    m_gcp_inflight[i].m_cnt = 0;
    m_newest_gcp_inflight_index = (m_newest_gcp_inflight_index + 1) % sz;
    ndbrequire(m_newest_gcp_inflight_index != m_oldest_gcp_inflight_index);
  }
  m_gcp_inflight[i].m_cnt++;
  return i;
}

void Suma::unmark_epoch_inflight(Signal *signal, Uint32 inflight_index) {
  ndbrequire(m_gcp_inflight[inflight_index].m_cnt > 0);
  m_gcp_inflight[inflight_index].m_cnt--;

  if (m_gcp_inflight[inflight_index].m_cnt > 0) {
    return;
  }

  if (inflight_index != m_oldest_gcp_inflight_index) {
    return;
  }

  const Uint32 sz = NDB_ARRAY_SIZE(m_gcp_inflight);
  while (m_oldest_gcp_inflight_index != m_newest_gcp_inflight_index &&
         m_gcp_inflight[m_oldest_gcp_inflight_index].m_cnt == 0) {
    m_gcp_inflight[m_oldest_gcp_inflight_index].m_gci = 0;
    m_oldest_gcp_inflight_index = (m_oldest_gcp_inflight_index + 1) % sz;
  }
  sendSUB_GCP_COMPLETE_REP(signal);
}

void Suma::execCREATE_TAB_CONF(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execCREATE_TAB_CONF");

  DBUG_VOID_RETURN;
}

void Suma::execDROP_TAB_CONF(Signal *signal) {
  jamEntry();
  ndbassert(signal->getNoOfSections() == 0);

  DropTabConf *const conf = (DropTabConf *)signal->getDataPtr();
  Uint32 senderRef = conf->senderRef;
  Uint32 tableId = conf->tableId;

  TablePtr tabPtr;
  if (!c_tables.find(tabPtr, tableId)) {
    jam();
    return;
  }

  DBUG_PRINT("info", ("drop table id: %d[i=%u]", tableId, tabPtr.i));
  const Table::State old_state = tabPtr.p->m_state;
  tabPtr.p->m_state = Table::DROPPED;
  c_tables.remove(tabPtr);

  if (senderRef != 0) {
    jam();

    // dict coordinator sends info to API

    const Uint64 gci = get_current_gci(signal);
    SubTableData *data = (SubTableData *)signal->getDataPtrSend();
    data->gci_hi = Uint32(gci >> 32);
    data->gci_lo = Uint32(gci);
    data->tableId = tableId;
    data->requestInfo = 0;
    SubTableData::setOperation(data->requestInfo,
                               NdbDictionary::Event::_TE_DROP);
    SubTableData::setReqNodeId(data->requestInfo, refToNode(senderRef));

    Ptr<Subscription> subPtr;
    Local_Subscription_list subList(c_subscriptionPool,
                                    tabPtr.p->m_subscriptions);

    for (subList.first(subPtr); !subPtr.isNull(); subList.next(subPtr)) {
      jam();
      if (subPtr.p->m_subscriptionType != SubCreateReq::TableEvent) {
        jam();
        continue;
        // continue in for-loop if the table is not part of
        // the subscription. Otherwise, send data to subscriber.
      }

      if (subPtr.p->m_options & Subscription::NO_REPORT_DDL) {
        jam();
        continue;
      }

      Ptr<Subscriber> ptr;
      Local_Subscriber_list list(c_subscriberPool, subPtr.p->m_subscribers);
      for (list.first(ptr); !ptr.isNull(); list.next(ptr)) {
        jam();
        data->senderData = ptr.p->m_senderData;
        sendSignal(ptr.p->m_senderRef, GSN_SUB_TABLE_DATA, signal,
                   SubTableData::SignalLength, JBB);
      }
    }
  }

  if (old_state == Table::DEFINING) {
    jam();
    return;
  }

  if (tabPtr.p->m_subscriptions.isEmpty()) {
    jam();
    tabPtr.p->release(*this);
    c_tablePool.release(tabPtr);
    return;
  } else {
    /**
     * check_release_subscription create a subList...
     *   weirdness below is to make sure that it's not created twice
     */
    Ptr<Subscription> subPtr;
    {
      Local_Subscription_list subList(c_subscriptionPool,
                                      tabPtr.p->m_subscriptions);
      subList.first(subPtr);
    }
    while (!subPtr.isNull()) {
      Ptr<Subscription> tmp = subPtr;
      {
        Local_Subscription_list subList(c_subscriptionPool,
                                        tabPtr.p->m_subscriptions);
        subList.next(subPtr);
      }
      check_release_subscription(signal, tmp);
    }
  }
}

/**
 * This receives DICT_TAB_INFO in long signal section 1, and releases the data
 * after use.
 */
void Suma::send_fragmented_SUB_TABLE_DATA_callback(Signal *signal,
                                                   Uint32 inflight_index,
                                                   Uint32 returnCode) {
  ndbrequire(returnCode == 0);
  ndbrequire(b_dti_buf_ref_count > 0);
  b_dti_buf_ref_count--;
  ndbrequire(inflight_index < NDB_ARRAY_SIZE(m_gcp_inflight));
  unmark_epoch_inflight(signal, inflight_index);
}

void Suma::execALTER_TAB_REQ(Signal *signal) {
  jamEntry();

  AlterTabReq *const req = (AlterTabReq *)signal->getDataPtr();
  Uint32 senderRef = req->senderRef;
  Uint32 tableId = req->tableId;
  Uint32 changeMask = req->changeMask;
  TablePtr tabPtr;

  // Copy DICT_TAB_INFO to local linear buffer
  SectionHandle handle(this, signal);
  SegmentedSectionPtr tabInfoPtr;
  ndbrequire(handle.getSection(tabInfoPtr, 0));

  if (!c_tables.find(tabPtr, tableId)) {
    jam();
    releaseSections(handle);
    return;
  }

  if (senderRef == 0) {
    jam();
    releaseSections(handle);
    return;
  }
  // dict coordinator sends info to API

#ifndef NDEBUG
  g_eventLogger->info("DICT_TAB_INFO in SUMA,  tabInfoPtr.sz = %d",
                      tabInfoPtr.sz);
  SimplePropertiesSectionReader reader(handle.m_ptr[0],
                                       getSectionSegmentPool());
  reader.printAll(g_eventLogger);
#endif
  ndbrequire(b_dti_buf_ref_count == 0);
  copy(b_dti_buf, tabInfoPtr);
  releaseSections(handle);

  LinearSectionPtr lptr[3];
  lptr[0].p = b_dti_buf;
  lptr[0].sz = tabInfoPtr.sz;

  const Uint64 gci = get_current_gci(signal);
  SubTableData *data = (SubTableData *)signal->getDataPtrSend();
  data->gci_hi = Uint32(gci >> 32);
  data->gci_lo = Uint32(gci);
  data->tableId = tableId;
  data->requestInfo = 0;
  SubTableData::setOperation(data->requestInfo,
                             NdbDictionary::Event::_TE_ALTER);
  SubTableData::setReqNodeId(data->requestInfo, refToNode(senderRef));
  data->flags = 0;
  data->changeMask = changeMask;
  data->totalLen = tabInfoPtr.sz;
  Ptr<Subscription> subPtr;
  Local_Subscription_list subList(c_subscriptionPool,
                                  tabPtr.p->m_subscriptions);

  for (subList.first(subPtr); !subPtr.isNull(); subList.next(subPtr)) {
    if (subPtr.p->m_subscriptionType != SubCreateReq::TableEvent) {
      jam();
      continue;
      // continue in for-loop if the table is not part of
      // the subscription. Otherwise, send data to subscriber.
    }

    if (subPtr.p->m_options & Subscription::NO_REPORT_DDL) {
      jam();
      continue;
    }

    Ptr<Subscriber> ptr;
    Local_Subscriber_list list(c_subscriberPool, subPtr.p->m_subscribers);
    for (list.first(ptr); !ptr.isNull(); list.next(ptr)) {
      jam();
      data->senderData = ptr.p->m_senderData;
      Uint32 inflight_index = mark_epoch_inflight(gci);
      Callback c = {safe_cast(&Suma::send_fragmented_SUB_TABLE_DATA_callback),
                    inflight_index};
      b_dti_buf_ref_count++;
      sendFragmentedSignal(ptr.p->m_senderRef, GSN_SUB_TABLE_DATA, signal,
                           SubTableData::SignalLength, JBB, lptr, 1, c);
    }
  }
}

void Suma::execSUB_GCP_COMPLETE_ACK(Signal *signal) {
  jamEntry();
  ndbassert(signal->getNoOfSections() == 0);

  SubGcpCompleteAck *const ack = (SubGcpCompleteAck *)signal->getDataPtr();
  Uint32 gci_hi = ack->rep.gci_hi;
  Uint32 gci_lo = ack->rep.gci_lo;
  Uint32 senderRef = ack->rep.senderRef;
  ndbrequire(signal->getLength() >= SubGcpCompleteAck::SignalLength);

  Uint64 gci = gci_lo | (Uint64(gci_hi) << 32);
  m_max_seen_gci = (gci > m_max_seen_gci ? gci : m_max_seen_gci);

  if (ERROR_INSERTED(13037)) {
    jam();
    g_eventLogger->info(
        "Simulating exceeding the MaxBufferedEpochs, ignoring ack");
    return;
  }
  if (ERROR_INSERTED(13052) || ERROR_INSERTED(13060) || ERROR_INSERTED(13061)) {
    jam();
    /**
     * g_eventLogger->info("Simulating many unacked epochs by ignoring ack "
     *                 "%u/%u. Not acked epochs %u", gci_hi, gci_lo,
     *                   c_gcp_list.getCount());
     *
     * sendSignalWithDelay(reference(), GSN_SUB_GCP_COMPLETE_ACK, signal, 10,
     *                   signal->getLength());
     */
    return;
  }

  if (refToBlock(senderRef) == SUMA) {
    jam();

    // Ack from other SUMA
    Uint32 nodeId = refToNode(senderRef);
    for (Uint32 i = 0; i < c_no_of_buckets; i++) {
      if (m_active_buckets.get(i) ||
          (m_switchover_buckets.get(i) && (check_switchover(i, gci))) ||
          (!m_switchover_buckets.get(i) && get_responsible_node(i) == nodeId)) {
        release_gci(signal, i, gci);
      }
    }
    return;
  }

  // Ack from User and not an ack from other SUMA, redistribute in nodegroup

  Uint32 nodeId = refToNode(senderRef);
  if (ERROR_INSERTED(13023)) {
    g_eventLogger->info("Throwing SUB_GCP_COMPLETE_ACK gci: %u/%u from %u",
                        Uint32(gci >> 32), Uint32(gci), nodeId);
    return;
  }

  jam();
  Ptr<Gcp_record> gcp;
  for (c_gcp_list.first(gcp); !gcp.isNull(); c_gcp_list.next(gcp)) {
    if (gcp.p->m_gci == gci) {
      gcp.p->m_subscribers.clear(nodeId);
      gcp.p->m_subscribers.bitAND(c_subscriber_nodes);
      if (!gcp.p->m_subscribers.isclear()) {
        jam();
        return;
      }
      break;
    }
  }

  if (gcp.isNull()) {
    g_eventLogger->warning(
        "ACK wo/ gcp record (gci: %u/%u) ref: %.8x from: %.8x",
        Uint32(gci >> 32), Uint32(gci), senderRef,
        signal->getSendersBlockRef());
  } else {
    c_gcp_list.release(gcp);
  }

  CRASH_INSERTION(13011);
  if (ERROR_INSERTED(13012)) {
    CLEAR_ERROR_INSERT_VALUE;
    g_eventLogger->info("Don't redistribute SUB_GCP_COMPLETE_ACK");
    return;
  }

  ack->rep.senderRef = reference();
  NodeReceiverGroup rg(SUMA, c_nodes_in_nodegroup_mask);
  sendSignal(rg, GSN_SUB_GCP_COMPLETE_ACK, signal,
             SubGcpCompleteAck::SignalLength, JBB);
}

/**************************************************************
 *
 * Removing subscription
 *
 */

void Suma::execSUB_REMOVE_REQ(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execSUB_REMOVE_REQ");

  CRASH_INSERTION(13021);

  const SubRemoveReq req = *(SubRemoveReq *)signal->getDataPtr();
  SubscriptionPtr subPtr;
  Subscription key;
  key.m_subscriptionId = req.subscriptionId;
  key.m_subscriptionKey = req.subscriptionKey;

  if (c_startup.m_restart_server_node_id == RNIL) {
    jam();

    /**
     * We haven't started syncing yet
     */
    sendSubRemoveRef(signal, req, SubRemoveRef::NotStarted);
    return;
  }

  bool found = c_subscriptions.find(subPtr, key);

  if (!found) {
    jam();
    sendSubRemoveRef(signal, req, SubRemoveRef::NoSuchSubscription);
    return;
  }

  switch (subPtr.p->m_state) {
    case Subscription::UNDEFINED:
      jam();
      ndbabort();
    case Subscription::DEFINING:
      jam();
      sendSubRemoveRef(signal, req, SubRemoveRef::Defining);
      return;
    case Subscription::DEFINED:
      if (subPtr.p->m_options & Subscription::MARKED_DROPPED) {
        /**
         * already dropped
         */
        jam();
        sendSubRemoveRef(signal, req, SubRemoveRef::AlreadyDropped);
        return;
      }
      break;
  }

  subPtr.p->m_options |= Subscription::MARKED_DROPPED;
  check_release_subscription(signal, subPtr);

  SubRemoveConf *const conf = (SubRemoveConf *)signal->getDataPtrSend();
  conf->senderRef = reference();
  conf->senderData = req.senderData;
  conf->subscriptionId = req.subscriptionId;
  conf->subscriptionKey = req.subscriptionKey;

  sendSignal(req.senderRef, GSN_SUB_REMOVE_CONF, signal,
             SubRemoveConf::SignalLength, JBB);
  return;
}

void Suma::check_release_subscription(Signal *signal,
                                      Ptr<Subscription> subPtr) {
  if (!subPtr.p->m_subscribers.isEmpty()) {
    jam();
    return;
  }

  if (!subPtr.p->m_start_req.isEmpty()) {
    jam();
    return;
  }

  if (!subPtr.p->m_stop_req.isEmpty()) {
    jam();
    return;
  }

  switch (subPtr.p->m_trigger_state) {
    case Subscription::T_UNDEFINED:
      jam();
      goto do_release;
    case Subscription::T_CREATING:
      jam();
      /**
       * Wait for completion
       */
      return;
    case Subscription::T_DEFINED:
      jam();
      subPtr.p->m_trigger_state = Subscription::T_DROPPING;
      drop_triggers(signal, subPtr);
      return;
    case Subscription::T_DROPPING:
      jam();
      /**
       * Wait for completion
       */
      return;
    case Subscription::T_ERROR:
      jam();
      /**
       * Wait for completion
       */
      return;
  }
  ndbabort();

do_release:
  TablePtr tabPtr;
  c_tables.getPtr(tabPtr, subPtr.p->m_table_ptrI);

  if (tabPtr.p->m_state == Table::DROPPED) {
    jam();
    subPtr.p->m_options |= Subscription::MARKED_DROPPED;
  }

  if ((subPtr.p->m_options & Subscription::MARKED_DROPPED) == 0) {
    jam();
    return;
  }

  {
    Local_Subscription_list list(c_subscriptionPool, tabPtr.p->m_subscriptions);
    list.remove(subPtr);
  }

  if (tabPtr.p->m_subscriptions.isEmpty()) {
    jam();
    switch (tabPtr.p->m_state) {
      case Table::UNDEFINED:
        ndbabort();
      case Table::DEFINING:
        break;
      case Table::DEFINED:
        jam();
        c_tables.remove(tabPtr);
        [[fallthrough]];
      case Table::DROPPED:
        jam();
        tabPtr.p->release(*this);
        c_tablePool.release(tabPtr);
    };
  }

  c_subscriptions.release(subPtr);
}

void Suma::sendSubRemoveRef(Signal *signal, const SubRemoveReq &req,
                            Uint32 errCode) {
  jam();
  DBUG_ENTER("Suma::sendSubRemoveRef");
  SubRemoveRef *ref = (SubRemoveRef *)signal->getDataPtrSend();
  ref->senderRef = reference();
  ref->senderData = req.senderData;
  ref->subscriptionId = req.subscriptionId;
  ref->subscriptionKey = req.subscriptionKey;
  ref->errorCode = errCode;
  sendSignal(signal->getSendersBlockRef(), GSN_SUB_REMOVE_REF, signal,
             SubRemoveRef::SignalLength, JBB);
  DBUG_VOID_RETURN;
}

void Suma::Table::release(Suma &suma) {
  jamBlock(&suma);

  m_state = UNDEFINED;
}

void Suma::SyncRecord::release() {
  jam();

  LocalSyncRecordBuffer fragBuf(suma.c_dataBufferPool, m_fragments);
  fragBuf.release();

  LocalSyncRecordBuffer attrBuf(suma.c_dataBufferPool, m_attributeList);
  attrBuf.release();

  LocalSyncRecordBuffer boundBuf(suma.c_dataBufferPool, m_boundInfo);
  boundBuf.release();

  ndbassert(m_sourceInstance == RNIL);
  ndbassert(m_headersSection == RNIL);
  ndbassert(m_dataSection == RNIL);
}

/**************************************************************
 *
 * Restarting remote node functions, master functionality
 * (slave does nothing special)
 * - triggered on INCL_NODEREQ calling startNode
 * - included node will issue START_ME when it's ready to start
 * the subscribers
 *
 */

void Suma::execSUMA_START_ME_REQ(Signal *signal) {
  jamEntry();

  Uint32 retref = signal->getSendersBlockRef();
  if (c_restart.m_ref) {
    jam();
    SumaStartMeRef *ref = (SumaStartMeRef *)signal->getDataPtrSend();
    ref->errorCode = SumaStartMeRef::Busy;
    sendSignal(retref, GSN_SUMA_START_ME_REF, signal,
               SumaStartMeRef::SignalLength, JBB);
    return;
  }

  if (getNodeState().getStarted() == false) {
    jam();
    SumaStartMeRef *ref = (SumaStartMeRef *)signal->getDataPtrSend();
    ref->errorCode = SumaStartMeRef::NotStarted;
    sendSignal(retref, GSN_SUMA_START_ME_REF, signal,
               SumaStartMeRef::SignalLength, JBB);
    return;
  }

  Ptr<SubOpRecord> subOpPtr;
  if (c_subOpPool.seize(subOpPtr) == false) {
    jam();
    SumaStartMeRef *ref = (SumaStartMeRef *)signal->getDataPtrSend();
    ref->errorCode = SumaStartMeRef::Busy;
    sendSignal(retref, GSN_SUMA_START_ME_REF, signal,
               SumaStartMeRef::SignalLength, JBB);
    return;
  }

  subOpPtr.p->m_opType = SubOpRecord::R_START_ME_REQ;

  c_restart.m_abort = 0;
  c_restart.m_waiting_on_self = 0;
  c_restart.m_ref = retref;
  c_restart.m_max_seq = c_current_seq;
  c_restart.m_subOpPtrI = subOpPtr.i;

  Subscription_hash::Iterator it;
  if (c_subscriptions.first(it)) {
    jam();

    /**
     * We only need to handle subscriptions with seq <= c_current_seq
     *   all subscriptions(s) created after this, will be handled by
     *   starting suma directly
     */
    c_current_seq++;
  }

  copySubscription(signal, it);
}

void Suma::copySubscription(Signal *signal, Subscription_hash::Iterator it) {
  jam();

  Ptr<SubOpRecord> subOpPtr;
  ndbrequire(c_subOpPool.getPtr(subOpPtr, c_restart.m_subOpPtrI));

  Ptr<Subscription> subPtr = it.curr;
  if (!subPtr.isNull()) {
    jam();
    c_restart.m_subPtrI = subPtr.i;
    c_restart.m_bucket = it.bucket;

    Local_SubOpRecord_fifo list(c_subOpPool, subPtr.p->m_stop_req);
    bool empty = list.isEmpty();
    list.addLast(subOpPtr);

    if (!empty) {
      /**
       * Wait for lock
       */
      jam();
      c_restart.m_waiting_on_self = 1;
      return;
    }

    sendSubCreateReq(signal, subPtr);
  } else {
    jam();
    SumaStartMeConf *conf = (SumaStartMeConf *)signal->getDataPtrSend();
    conf->unused = 0;
    sendSignal(c_restart.m_ref, GSN_SUMA_START_ME_CONF, signal,
               SumaStartMeConf::SignalLength, JBB);

    c_subOpPool.release(subOpPtr);
    c_restart.m_ref = 0;
    return;
  }
}

void Suma::sendSubCreateReq(Signal *signal, Ptr<Subscription> subPtr) {
  jam();

  if (c_restart.m_abort) {
    jam();
    abort_start_me(signal, subPtr, true);
    return;
  }

  c_restart.m_waiting_on_self = 0;
  SubCreateReq *req = (SubCreateReq *)signal->getDataPtrSend();
  req->senderRef = reference();
  req->senderData = subPtr.i;
  req->subscriptionId = subPtr.p->m_subscriptionId;
  req->subscriptionKey = subPtr.p->m_subscriptionKey;
  req->subscriptionType = subPtr.p->m_subscriptionType;
  req->tableId = subPtr.p->m_tableId;
  req->schemaTransId = 0;

  if (subPtr.p->m_options & Subscription::REPORT_ALL) {
    req->subscriptionType |= SubCreateReq::ReportAll;
  }

  if (subPtr.p->m_options & Subscription::REPORT_SUBSCRIBE) {
    req->subscriptionType |= SubCreateReq::ReportSubscribe;
  }

  if (subPtr.p->m_options & Subscription::NO_REPORT_DDL) {
    req->subscriptionType |= SubCreateReq::NoReportDDL;
  }

  if (subPtr.p->m_options & Subscription::MARKED_DROPPED) {
    req->subscriptionType |= SubCreateReq::NR_Sub_Dropped;
    g_eventLogger->info("copying dropped sub: %u", subPtr.i);
  }

  Ptr<Table> tabPtr;
  ndbrequire(c_tablePool.getPtr(tabPtr, subPtr.p->m_table_ptrI));
  if (tabPtr.p->m_state != Table::DROPPED) {
    jam();
    c_restart.m_waiting_on_self = 0;

    sendSignal(c_restart.m_ref, GSN_SUB_CREATE_REQ, signal,
               SubCreateReq::SignalLength, JBB);
  } else {
    jam();
    g_eventLogger->info("not copying sub %u with dropped table: %u/%u",
                        subPtr.i, tabPtr.p->m_tableId, tabPtr.i);

    c_restart.m_waiting_on_self = 1;
    SubCreateConf *conf = (SubCreateConf *)signal->getDataPtrSend();
    conf->senderRef = reference();
    conf->senderData = subPtr.i;
    sendSignal(reference(), GSN_SUB_CREATE_CONF, signal,
               SubCreateConf::SignalLength, JBB);
  }
}

void Suma::execSUB_CREATE_REF(Signal *signal) {
  jamEntry();

  SubCreateRef *const ref = (SubCreateRef *)signal->getDataPtr();
  Uint32 error = ref->errorCode;

  {
    SumaStartMeRef *ref = (SumaStartMeRef *)signal->getDataPtrSend();
    ref->errorCode = error;
    sendSignal(c_restart.m_ref, GSN_SUMA_START_ME_REF, signal,
               SumaStartMeRef::SignalLength, JBB);
  }

  Ptr<Subscription> subPtr;
  ndbrequire(c_subscriptionPool.getPtr(subPtr, c_restart.m_subPtrI));
  abort_start_me(signal, subPtr, true);
}

void Suma::execSUB_CREATE_CONF(Signal *signal) {
  jamEntry();

  /**
   * We have lock...start all subscriber(s)
   */
  Ptr<Subscription> subPtr;
  ndbrequire(c_subscriptionPool.getPtr(subPtr, c_restart.m_subPtrI));

  c_restart.m_waiting_on_self = 0;

  /**
   * Check if we were aborted...
   *  this signal is sent to self in case of DROPPED subscription...
   */
  if (c_restart.m_abort) {
    jam();
    abort_start_me(signal, subPtr, true);
    return;
  }

  Ptr<Table> tabPtr;
  ndbrequire(c_tablePool.getPtr(tabPtr, subPtr.p->m_table_ptrI));

  Ptr<Subscriber> ptr;
  if (tabPtr.p->m_state != Table::DROPPED) {
    jam();
    Local_Subscriber_list list(c_subscriberPool, subPtr.p->m_subscribers);
    list.first(ptr);
  } else {
    jam();
    ptr.setNull();
    g_eventLogger->info(
        "not copying subscribers on sub: %u with dropped table %u/%u", subPtr.i,
        tabPtr.p->m_tableId, tabPtr.i);
  }

  copySubscriber(signal, subPtr, ptr);
}

void Suma::copySubscriber(Signal *signal, Ptr<Subscription> subPtr,
                          Ptr<Subscriber> ptr) {
  if (!ptr.isNull()) {
    jam();

    SubStartReq *req = (SubStartReq *)signal->getDataPtrSend();
    req->senderRef = reference();
    req->senderData = ptr.i;
    req->subscriptionId = subPtr.p->m_subscriptionId;
    req->subscriptionKey = subPtr.p->m_subscriptionKey;
    req->part = SubscriptionData::TableData;
    req->subscriberData = ptr.p->m_senderData;
    req->subscriberRef = ptr.p->m_senderRef;
    req->requestInfo = ptr.p->m_requestInfo;

    sendSignal(c_restart.m_ref, GSN_SUB_START_REQ, signal,
               SubStartReq::SignalLength, JBB);
    return;
  } else {
    // remove lock from this subscription
    Ptr<SubOpRecord> subOpPtr;
    ndbrequire(c_subOpPool.getPtr(subOpPtr, c_restart.m_subOpPtrI));
    check_remove_queue(signal, subPtr, subOpPtr, true, false);
    check_release_subscription(signal, subPtr);

    Subscription_hash::Iterator it;
    it.curr = subPtr;
    it.bucket = c_restart.m_bucket;
    c_subscriptions.next(it);
    copySubscription(signal, it);
  }
}

void Suma::execSUB_START_CONF(Signal *signal) {
  jamEntry();

  SubStartConf *const conf = (SubStartConf *)signal->getDataPtr();

  Ptr<Subscription> subPtr;
  ndbrequire(c_subscriptionPool.getPtr(subPtr, c_restart.m_subPtrI));

  Ptr<Subscriber> ptr;
  ndbrequire(c_subscriberPool.getPtr(ptr, conf->senderData));

  Local_Subscriber_list list(c_subscriberPool, subPtr.p->m_subscribers);
  list.next(ptr);
  copySubscriber(signal, subPtr, ptr);
}

void Suma::execSUB_START_REF(Signal *signal) {
  jamEntry();

  SubStartRef *sig = (SubStartRef *)signal->getDataPtr();
  Uint32 errorCode = sig->errorCode;

  {
    SumaStartMeRef *ref = (SumaStartMeRef *)signal->getDataPtrSend();
    ref->errorCode = errorCode;
    sendSignal(c_restart.m_ref, GSN_SUMA_START_ME_REF, signal,
               SumaStartMeRef::SignalLength, JBB);
  }

  Ptr<Subscription> subPtr;
  ndbrequire(c_subscriptionPool.getPtr(subPtr, c_restart.m_subPtrI));

  abort_start_me(signal, subPtr, true);
}

void Suma::abort_start_me(Signal *signal, Ptr<Subscription> subPtr,
                          bool lockowner) {
  Ptr<SubOpRecord> subOpPtr;
  ndbrequire(c_subOpPool.getPtr(subOpPtr, c_restart.m_subOpPtrI));
  check_remove_queue(signal, subPtr, subOpPtr, lockowner, true);
  check_release_subscription(signal, subPtr);

  c_restart.m_ref = 0;
}

void Suma::execSUMA_HANDOVER_REQ(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execSUMA_HANDOVER_REQ");
  //  Uint32 sumaRef = signal->getSendersBlockRef();
  const SumaHandoverReq *req =
      CAST_CONSTPTR(SumaHandoverReq, signal->getDataPtr());

  if (ERROR_INSERTED(13054)) {
    SET_ERROR_INSERT_VALUE(13055);
    sendSignalWithDelay(reference(), GSN_SUMA_HANDOVER_REQ, signal, 5000,
                        signal->length());
    return;
  }
  if (ERROR_INSERTED(13055)) {
    SET_ERROR_INSERT_VALUE(13054);
  }
  Uint32 gci = req->gci;
  Uint32 nodeId = req->nodeId;
  Uint32 new_gci = Uint32(m_last_complete_gci >> 32) + MAX_CONCURRENT_GCP + 1;
  Uint32 requestType = req->requestType;

  Uint32 start_gci = (gci > new_gci ? gci : new_gci);
  // mark all active buckets really belonging to restarting SUMA

  Bucket_mask tmp;
  if (requestType == SumaHandoverReq::RT_START_NODE) {
    jam();
    c_alive_nodes.set(nodeId);
    if (DBG_3R)
      g_eventLogger->info("%u c_alive_nodes.set(%u)", __LINE__, nodeId);

    for (Uint32 i = 0; i < c_no_of_buckets; i++) {
      if (get_responsible_node(i) == nodeId) {
        if (m_active_buckets.get(i)) {
          // I'm running this bucket but it should really be the restarted node
          tmp.set(i);
          m_active_buckets.clear(i);
          m_switchover_buckets.set(i);
          c_buckets[i].m_switchover_gci = (Uint64(start_gci) << 32) - 1;
          c_buckets[i].m_state |= Bucket::BUCKET_HANDOVER;
          c_buckets[i].m_switchover_node = nodeId;
          g_eventLogger->info("prepare to handover bucket: %d", i);
        } else if (m_switchover_buckets.get(i)) {
          g_eventLogger->info("dont handover bucket: %d %d", i, nodeId);
        }
      }
    }
  } else if (requestType == SumaHandoverReq::RT_STOP_NODE) {
    jam();

    for (Uint32 i = 0; i < c_no_of_buckets; i++) {
      NdbNodeBitmask nodegroup = c_nodes_in_nodegroup_mask;
      nodegroup.clear(nodeId);
      if (get_responsible_node(i) == nodeId &&
          get_responsible_node(i, nodegroup) == getOwnNodeId()) {
        // I will be running this bucket when nodeId shutdown
        jam();
        tmp.set(i);
        ndbassert(!m_active_buckets.get(i));
        m_switchover_buckets.set(i);
        c_buckets[i].m_switchover_gci = (Uint64(start_gci) << 32) - 1;
        c_buckets[i].m_state |= Bucket::BUCKET_SHUTDOWN_TO;
        c_buckets[i].m_switchover_node = nodeId;
        g_eventLogger->info("prepare to takeover bucket: %d", i);
        if (ERROR_INSERTED(13056)) {
          CLEAR_ERROR_INSERT_VALUE;
          signal->theData[0] = 9999;
          NdbNodeBitmask resp_nodes_minus_self;
          resp_nodes_minus_self.assign(nodegroup);
          resp_nodes_minus_self.clear(getOwnNodeId());
          resp_nodes_minus_self.clear(nodeId);
          int nf_node = get_responsible_node(i, resp_nodes_minus_self);
          sendSignal(numberToRef(CMVMI, refToNode(nf_node)), GSN_NDB_TAMPER,
                     signal, 1, JBA);
        }
      }
    }
  } else {
    jam();
    goto ref;
  }

  {
    SumaHandoverConf *conf =
        CAST_PTR(SumaHandoverConf, signal->getDataPtrSend());
    tmp.copyto(BUCKET_MASK_SIZE, conf->theBucketMask);
    conf->gci = start_gci;
    conf->nodeId = getOwnNodeId();
    conf->requestType = requestType;
    sendSignal(calcSumaBlockRef(nodeId), GSN_SUMA_HANDOVER_CONF, signal,
               SumaHandoverConf::SignalLength, JBB);
  }

  DBUG_VOID_RETURN;

ref:
  signal->theData[0] = 111;
  signal->theData[1] = getOwnNodeId();
  signal->theData[2] = nodeId;
  sendSignal(calcSumaBlockRef(nodeId), GSN_SUMA_HANDOVER_REF, signal, 3, JBB);
  DBUG_VOID_RETURN;
}

// only run on all but restarting suma
void Suma::execSUMA_HANDOVER_REF(Signal *signal) { ndbabort(); }

void Suma::execSUMA_HANDOVER_CONF(Signal *signal) {
  jamEntry();
  DBUG_ENTER("Suma::execSUMA_HANDOVER_CONF");

  const SumaHandoverConf *conf =
      CAST_CONSTPTR(SumaHandoverConf, signal->getDataPtr());

  CRASH_INSERTION(13043);

  Uint32 gci = conf->gci;
  Uint32 nodeId = conf->nodeId;
  Uint32 requestType = conf->requestType;
  Bucket_mask tmp;
  tmp.assign(BUCKET_MASK_SIZE, conf->theBucketMask);
#ifdef HANDOVER_DEBUG
  g_eventLogger->info("Suma::execSUMA_HANDOVER_CONF, gci = %u", gci);
#endif

  if (requestType == SumaHandoverReq::RT_START_NODE) {
    jam();
    for (Uint32 i = 0; i < c_no_of_buckets; i++) {
      if (tmp.get(i)) {
        if (DBG_3R)
          g_eventLogger->info("%u : %u %u", i, get_responsible_node(i),
                              getOwnNodeId());
        ndbrequire(get_responsible_node(i) == getOwnNodeId());
        // We should run this bucket, but _nodeId_ is
        c_buckets[i].m_switchover_gci = (Uint64(gci) << 32) - 1;
        c_buckets[i].m_state |= Bucket::BUCKET_STARTING;
      }
    }

    char buf[255];
    tmp.getText(buf);
    infoEvent("Suma: handover from node %u gci: %u buckets: %s (%u)", nodeId,
              gci, buf, c_no_of_buckets);
    g_eventLogger->info("Suma: handover from node %u gci: %u buckets: %s (%u)",
                        nodeId, gci, buf, c_no_of_buckets);
    ndbassert(!m_active_buckets.overlaps(tmp));
    m_switchover_buckets.bitOR(tmp);
    ndbrequire(c_startup.m_handover_nodes.get(nodeId));
    c_startup.m_handover_nodes.clear(nodeId);
    DBUG_VOID_RETURN;
  } else if (requestType == SumaHandoverReq::RT_STOP_NODE) {
    jam();
    for (Uint32 i = 0; i < c_no_of_buckets; i++) {
      if (tmp.get(i)) {
        ndbrequire(get_responsible_node(i) == getOwnNodeId());
        // We should run this bucket, but _nodeId_ is
        c_buckets[i].m_switchover_node = getOwnNodeId();
        c_buckets[i].m_switchover_gci = (Uint64(gci) << 32) - 1;
        c_buckets[i].m_state |= Bucket::BUCKET_SHUTDOWN;
      }
    }

    char buf[255];
    tmp.getText(buf);
    infoEvent("Suma: handover to node %u gci: %u buckets: %s (%u)", nodeId, gci,
              buf, c_no_of_buckets);
    g_eventLogger->info("Suma: handover to node %u gci: %u buckets: %s (%u)",
                        nodeId, gci, buf, c_no_of_buckets);
    m_active_buckets.bitANDC(tmp);
    m_switchover_buckets.bitOR(tmp);
    c_startup.m_handover_nodes.clear(nodeId);
    DBUG_VOID_RETURN;
  }
}

void Suma::execSTOP_ME_REQ(Signal *signal) {
  jam();
  StopMeReq req = *CAST_CONSTPTR(StopMeReq, signal->getDataPtr());

  ndbrequire(refToNode(req.senderRef) == getOwnNodeId());
  ndbrequire(c_shutdown.m_wait_handover == false);
  c_shutdown.m_wait_handover = true;
  NdbTick_Invalidate(&c_startup.m_wait_handover_expire);
  c_shutdown.m_senderRef = req.senderRef;
  c_shutdown.m_senderData = req.senderData;
  send_handover_req(signal, SumaHandoverReq::RT_STOP_NODE);
}

#ifdef NOT_USED
static NdbOut &operator<<(NdbOut &out, const Suma::Page_pos &pos) {
  out << "[ Page_pos:"
      << " m_page_id: " << pos.m_page_id << " m_page_pos: " << pos.m_page_pos
      << " m_max_gci: " << pos.m_max_gci << " ]";
  return out;
}
#endif

Uint32 *Suma::get_buffer_ptr(Signal *signal, Uint32 buck, Uint64 gci, Uint32 sz,
                             Uint32 part) {
  jam();
  sz += 1;  // len
  ndbrequire(buck < NO_OF_BUCKETS);
  Bucket *bucket = c_buckets + buck;
  Page_pos pos = bucket->m_buffer_head;

  Buffer_page *page = 0;
  Uint32 *ptr = 0;

  if (likely(pos.m_page_id != RNIL)) {
    jam();
    page = c_page_pool.getPtr(pos.m_page_id);
    ptr = page->m_data + pos.m_page_pos;
  }

  const bool same_gci = (gci == pos.m_last_gci) && (!ERROR_INSERTED(13022));

  pos.m_page_pos += sz;
  pos.m_last_gci = gci;
  Uint64 max = pos.m_max_gci > gci ? pos.m_max_gci : gci;

  if (likely(same_gci && pos.m_page_pos <= Buffer_page::DATA_WORDS)) {
    jam();
    pos.m_max_gci = max;
    bucket->m_buffer_head = pos;
    *ptr =
        Buffer_page::SAME_GCI_FLAG | (part << Buffer_page::PART_NUM_SHIFT) | sz;
    ptr++;
    return ptr;
  } else if (pos.m_page_pos + Buffer_page::GCI_SZ32 <=
             Buffer_page::DATA_WORDS) {
  loop:
    jam();
    pos.m_max_gci = max;
    pos.m_page_pos += Buffer_page::GCI_SZ32;
    bucket->m_buffer_head = pos;
    *ptr++ =
        (part << Buffer_page::PART_NUM_SHIFT) | (sz + Buffer_page::GCI_SZ32);
    *ptr++ = (Uint32)(gci >> 32);
    *ptr++ = (Uint32)(gci & 0xFFFFFFFF);
    return ptr;
  } else {
    jam();
    /**
     * new page
     * 1) save header on last page
     * 2) seize new page
     */
    static_assert(1 + 6 + SUMA_BUF_SZ + Buffer_page::GCI_SZ32 <=
                  Buffer_page::DATA_WORDS);
    Uint32 next;
    if (unlikely((next = seize_page()) == RNIL)) {
      jam();
      /**
       * Out of buffer
       */
      out_of_buffer(signal);
      return 0;
    }

    if (likely(pos.m_page_id != RNIL)) {
      jam();
      page->m_max_gci_hi = (Uint32)(pos.m_max_gci >> 32);
      page->m_max_gci_lo = (Uint32)(pos.m_max_gci & 0xFFFFFFFF);
      page->m_words_used = pos.m_page_pos - sz;
      page->m_next_page = next;
      ndbassert(pos.m_max_gci != 0);
    } else {
      jam();
      bucket->m_buffer_tail = next;
    }

    memset(&pos, 0, sizeof(pos));
    pos.m_page_id = next;
    pos.m_page_pos = sz;
    pos.m_last_gci = gci;

    page = c_page_pool.getPtr(pos.m_page_id);
    page->m_next_page = RNIL;
    ptr = page->m_data;
    goto loop;  //
  }
}

void Suma::disable_buffering() {
  m_fully_buffering_after_gci = UINT_MAX64;

  infoEvent(
      "Out of event buffer. Node failure will cause event failures. "
      "Consider increasing MaxBufferedEpochBytes.");

  g_eventLogger->info(
      "SUMA Out of event buffer: Disabling buffering. "
      "m_max_seen_gci %u/%u m_last_complete_gci %u/%u",
      (Uint32)(m_max_seen_gci >> 32), (Uint32)m_max_seen_gci,
      Uint32(m_last_complete_gci >> 32), (Uint32)m_last_complete_gci);
}

bool Suma::buffering_disabled() const {
  return (m_fully_buffering_after_gci == UINT64_MAX);
}

void Suma::out_of_buffer(Signal *signal) {
  if (unlikely(buffering_disabled())) {
    // Out of buffer handling is already going on
    jam();
    return;
  }
  disable_buffering();

  if (!c_gcp_list.isEmpty()) {
    jam();
    Ptr<Gcp_record> gcp;
    c_gcp_list.first(gcp);
    infoEvent("Highest epoch %llu, oldest epoch %llu", m_max_seen_gci,
              m_last_complete_gci);
    NodeBitmask subs = gcp.p->m_subscribers;
    if (!subs.isclear()) {
      char buf[NodeBitmask::TextLength + 1];
      subs.getText(buf);
      infoEvent("Pending nodes '%s', epoch %llu", buf, gcp.p->m_gci);
    }
  }

  out_of_buffer_release(signal, 0);
}

void Suma::out_of_buffer_release(Signal *signal, Uint32 buck) {
  ndbrequire(buck < NO_OF_BUCKETS);
  Bucket *bucket = c_buckets + buck;
  Uint32 tail = bucket->m_buffer_tail;

  if (tail != RNIL) {
    Buffer_page *page = c_page_pool.getPtr(tail);
    bucket->m_buffer_tail = page->m_next_page;
    free_page(tail, page);
  }

  // If the page freed above is the last page, update the head
  // and continue releasing the next bucket.
  if (tail == RNIL) {
    bucket->m_buffer_head.m_page_id = RNIL;
    bucket->m_buffer_head.m_page_pos = Buffer_page::DATA_WORDS + 1;

    buck++;
  }

  if (buck != c_no_of_buckets) {
    signal->theData[0] = SumaContinueB::OUT_OF_BUFFER_RELEASE;
    signal->theData[1] = buck;
    sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 2, JBB);
    return;
  }

  /**
   * Out-of-buffer handling is completed. All buffers are
   * released. Resume buffering and record the
   * max known epoch = max(m_max_seen_gci, m_last_complete_gci).
   *
   * Max known epoch may be
   * a) partially buffered if m_max_seen_gci > m_last_complete_gci,
   * b) fully buffered otherwise
   * both of which we have not yet seen at this point of resuming
   * buffering here.
   *
   * The first epoch, say X where X > max known epoch, that we complete
   * is the first epoch that has been fully buffered.
   * To ensure that event-API gets info on gap, X will
   * be the last epoch tagged with MISSING_DATA, in
   * order to cover case b). Therefore it becomes the lowest resendable epoch.
   */
  m_fully_buffering_after_gci = m_max_seen_gci > m_last_complete_gci
                                    ? m_max_seen_gci
                                    : m_last_complete_gci;

  for (Uint32 i = 0; i < c_no_of_buckets; i++) {
    bucket = c_buckets + i;
    // Check if all the buckets are cleared and nothing is buffered.
    ndbrequire(tail == RNIL && bucket->m_buffer_head.m_page_id == RNIL);

    /**
     * After the out of buffer is handled, we should ensure that NO
     * future resend will find a resend start point that could give an
     * event stream with partially buffered epoch. Therefore, we move
     * the buckets' m_max_acked_gci to m_fully_buffering_after_gci
     * s.t., X, mentioned above, becomes the lowest resendable epoch.
     */
    bucket->m_max_acked_gci = m_fully_buffering_after_gci;
  }

  g_eventLogger->info(
      "SUMA Out of buffer release completed for all buckets. "
      "Max seen epoch %u/%u last complete epoch %u/%u",
      (Uint32)(m_max_seen_gci >> 32), (Uint32)m_max_seen_gci,
      (Uint32)(m_last_complete_gci >> 32), (Uint32)m_last_complete_gci);
}

Uint32 Suma::seize_page() {
  if (ERROR_INSERTED(13038)) {
    jam();
    CLEAR_ERROR_INSERT_VALUE;
    g_eventLogger->info("Simulating out of event buffer");
    disable_buffering();
  }
  if (unlikely(buffering_disabled())) {
    jam();
    return RNIL;
  }
loop:
  Ptr<Page_chunk> ptr;
  Uint32 ref = m_first_free_page;
  if (likely(ref != RNIL)) {
    m_first_free_page = (c_page_pool.getPtr(ref))->m_next_page;
    Uint32 chunk = (c_page_pool.getPtr(ref))->m_page_chunk_ptr_i;
    ndbrequire(c_page_chunk_pool.getPtr(ptr, chunk));
    ndbassert(ptr.p->m_free);
    ptr.p->m_free--;
    return ref;
  }

  if (!c_page_chunk_pool.seize(ptr)) return RNIL;

  Uint32 count = Page_chunk::PAGES_PER_CHUNK;
  m_ctx.m_mm.alloc_pages(RT_SUMA_EVENT_BUFFER, &ref, &count, 1);
  if (count == 0) return RNIL;

  g_eventLogger->info(
      "Allocate event buffering page chunk in SUMA, %u pages,"
      " first page ref = %u",
      count, ref);

  m_first_free_page = ptr.p->m_page_id = ref;
  ptr.p->m_size = count;
  ptr.p->m_free = count;

  Buffer_page *page = nullptr;
  for (Uint32 i = 0; i < count; i++) {
    page = c_page_pool.getPtr(ref);
    page->m_page_state = SUMA_SEQUENCE;
    page->m_page_chunk_ptr_i = ptr.i;
    page->m_next_page = ++ref;
  }
  page->m_next_page = RNIL;

  goto loop;
}

void Suma::free_page(Uint32 page_id, Buffer_page *page) {
  Ptr<Page_chunk> ptr;
  ndbrequire(page->m_page_state == SUMA_SEQUENCE);

  Uint32 chunk = page->m_page_chunk_ptr_i;

  ndbrequire(c_page_chunk_pool.getPtr(ptr, chunk));

  ptr.p->m_free++;
  page->m_next_page = m_first_free_page;
  ndbrequire(ptr.p->m_free <= ptr.p->m_size);

  m_first_free_page = page_id;
}

void Suma::release_gci(Signal *signal, Uint32 buck, Uint64 gci) {
  if (unlikely(buffering_disabled())) {
    jam();
    // Out of buffer release has already started, so no need to do more
    return;
  }

  ndbrequire(buck < NO_OF_BUCKETS);
  Bucket *bucket = c_buckets + buck;
  Uint32 tail = bucket->m_buffer_tail;
  Page_pos head = bucket->m_buffer_head;
  Uint64 max_acked = bucket->m_max_acked_gci;

  const Uint32 mask = Bucket::BUCKET_TAKEOVER | Bucket::BUCKET_RESEND;
  if (unlikely(bucket->m_state & mask)) {
    jam();
    g_eventLogger->info("release_gci(%d, %u/%u) 0x%x-> node failure -> abort",
                        buck, Uint32(gci >> 32), Uint32(gci), bucket->m_state);
    return;
  }

  bucket->m_max_acked_gci = (max_acked > gci ? max_acked : gci);
  if (unlikely(tail == RNIL)) {
    return;
  }

  if (tail == head.m_page_id) {
    if (gci >= head.m_max_gci) {
      jam();
      if (ERROR_INSERTED(13034)) {
        jam();
        SET_ERROR_INSERT_VALUE(13035);
        return;
      }
      if (ERROR_INSERTED(13035)) {
        CLEAR_ERROR_INSERT_VALUE;
        NodeReceiverGroup rg(CMVMI, c_nodes_in_nodegroup_mask);
        rg.m_nodes.clear(getOwnNodeId());
        signal->theData[0] = 9999;
        sendSignal(rg, GSN_NDB_TAMPER, signal, 1, JBA);
        return;
      }
      head.m_page_pos = 0;
      head.m_max_gci = gci;
      head.m_last_gci = 0;
      bucket->m_buffer_head = head;
    }
    return;
  } else {
    jam();
    Buffer_page *page = c_page_pool.getPtr(tail);
    Uint64 max_gci = page->m_max_gci_lo | (Uint64(page->m_max_gci_hi) << 32);
    Uint32 next_page = page->m_next_page;

    ndbassert(max_gci != 0);

    if (gci >= max_gci) {
      jam();
      free_page(tail, page);

      bucket->m_buffer_tail = next_page;
      signal->theData[0] = SumaContinueB::RELEASE_GCI;
      signal->theData[1] = buck;
      signal->theData[2] = (Uint32)(gci >> 32);
      signal->theData[3] = (Uint32)(gci & 0xFFFFFFFF);
      sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 4, JBB);
      return;
    } else {
      // g_eventLogger->info("do nothing...");
    }
  }
}
void Suma::enable_normal_resending() {
  g_eventLogger->info("Suma: Enabling normal resending");
  m_max_fully_acked_gci = UINT64_MAX;
  m_fully_buffering_after_gci = 0;
}

bool Suma::normal_resendable() const {
  return (m_fully_buffering_after_gci <= m_max_fully_acked_gci);
}

static Uint32 g_cnt = 0;

void Suma::start_resend(Signal *signal, Uint32 buck) {
  jam();
  ndbrequire(buck < NO_OF_BUCKETS);
  Bucket *bucket = c_buckets + buck;
  Page_pos pos = bucket->m_buffer_head;

  if (normal_resendable() && pos.m_page_id == RNIL) {
    jam();
    g_eventLogger->info(
        "SUMA bucket %u resend complete as the bucket is empty and will "
        "become active. max_acked: %u/%u  max_gci at bucket header : %u/%u",
        buck, Uint32(bucket->m_max_acked_gci >> 32),
        Uint32(bucket->m_max_acked_gci), Uint32(pos.m_max_gci >> 32),
        Uint32(pos.m_max_gci));
    m_active_buckets.set(buck);
    m_gcp_complete_rep_count++;
    return;
  }

  // Start resending from the epoch that is not yet ack'd
  const Uint64 resend_start_gci = bucket->m_max_acked_gci + 1;
  if (normal_resendable() && resend_start_gci > m_max_seen_gci) {
    jam();
    // Everything seen has been sent + acked by subscribers
    ndbrequire(pos.m_page_id == bucket->m_buffer_tail);
    g_eventLogger->info(
        "SUMA bucket %u resend complete as nothing unacknowledged "
        "and will become active. "
        "resend_start_gci %u/%u resend_end_gci %u/%u",
        buck, Uint32(resend_start_gci >> 32), Uint32(resend_start_gci),
        Uint32(m_max_seen_gci >> 32), Uint32(m_max_seen_gci));
    m_active_buckets.set(buck);
    m_gcp_complete_rep_count++;
    return;
  }

  g_cnt = 0;

  /**
   * Mark bucket for takeover on the next epoch boundary.  Don't
   * takeover now as we have data to re-send and we need data in the
   * current epoch to be delivered in-order, therefore we must continue to
   * buffer it so that it is re-sent in order.
   */
  bucket->m_state |= (Bucket::BUCKET_TAKEOVER);
  bucket->m_switchover_node = get_responsible_node(buck);
  bucket->m_switchover_gci = m_max_seen_gci;

  ndbassert(!m_active_buckets.get(buck));
  m_switchover_buckets.set(buck);

  // If the lowest resendable epoch is not yet established due to
  // an out of buffer release handling, do not start resending.
  if (unlikely(!normal_resendable())) {
    jam();
    // Gap in the event stream
    m_missing_data = true;

    // Inform the incident to the users
    signal->theData[0] = NDB_LE_SubscriptionStatus;
    signal->theData[1] = 2;  // INCONSISTENT;
    signal->theData[2] = 0;  // Not used
    signal->theData[3] = (Uint32)resend_start_gci;
    signal->theData[4] = (Uint32)(resend_start_gci >> 32);
    sendSignal(CMVMI_REF, GSN_EVENT_REP, signal, 5, JBB);

    g_eventLogger->info(
        "SUMA Gap in the event stream. No need to start "
        "resending. Bucket %u will become active. "
        "Bucket's max_acked_gci %u/%u max_seen_gci %u/%u",
        buck, Uint32(bucket->m_max_acked_gci >> 32),
        Uint32(bucket->m_max_acked_gci), Uint32(m_max_seen_gci >> 32),
        Uint32(m_max_seen_gci));
    return;
  }

  // Start resending from resend_start_gci to m_max_seen_gci, which is
  // the switchover_gci where resend ends and forwarding starts.
  bucket->m_state |= Bucket::BUCKET_RESEND;

  g_eventLogger->info("SUMA Start resending bucket %u from %u/%u to %u/%u ",
                      buck, Uint32(resend_start_gci >> 32),
                      Uint32(resend_start_gci), Uint32(m_max_seen_gci >> 32),
                      Uint32(m_max_seen_gci));

  signal->theData[0] = SumaContinueB::RESEND_BUCKET;
  signal->theData[1] = buck;
  signal->theData[2] = (Uint32)(resend_start_gci >> 32);
  signal->theData[3] = 0;
  signal->theData[4] = 0;
  signal->theData[5] = (Uint32)(resend_start_gci & 0xFFFFFFFF);
  signal->theData[6] = 0;
  sendSignal(reference(), GSN_CONTINUEB, signal, 7, JBB);

  ndbrequire(m_max_seen_gci >= resend_start_gci);
}

#ifdef ERROR_INSERT
static Uint32 resend_bucket_round = 0;
#endif
void Suma::resend_bucket(Signal *signal, Uint32 buck, Uint64 min_gci,
                         Uint32 pos, Uint64 last_gci) {
  ndbrequire(buck < NO_OF_BUCKETS);
  Bucket *bucket = c_buckets + buck;
  g_eventLogger->info("resend_bucket");

#ifdef ERROR_INSERT
  if (unlikely(ERROR_INSERTED(13060))) {
    jam();
    /*
    // resend_bucket_round accumulates only from the first bucket fiber
    for (Uint32 i = 0; i < m_switchover_buckets.count(); ++i) {
      if (m_switchover_buckets.get(buck)) {
       resend_bucket_round++;
       break;
      }
    }
    */
    // Any bucket fiber can add, and thus meeting the condition faster
    resend_bucket_round++;
    if (resend_bucket_round == ERROR_INSERT_EXTRA) {
      jam();
      g_eventLogger->info(
          "resend_bucket: cleared error insertion 13060 and "
          "simulating OOB at round %u",
          resend_bucket_round);
      resend_bucket_round = 0;
      CLEAR_ERROR_INSERT_VALUE;
      out_of_buffer(signal);
    }
  }
#endif

  /**
   * Check if out_of_buffer handling began and releasing buffers while
   * resending took a break (CONTINUEB).
   */
  if (unlikely(!normal_resendable())) {
    jam();
    // Gap in the event stream
    m_missing_data = true;

    // Stop resending
    ndbassert(bucket->m_state & Bucket::BUCKET_RESEND);
    bucket->m_state &= ~(Uint32)Bucket::BUCKET_RESEND;

    // Ensure bucket take-over :
    // - either it is waiting to be taken over
    // - or it has already been taken over.
    ndbrequire(bucket->m_state & Bucket::BUCKET_TAKEOVER ||
               m_active_buckets.get(buck));

    g_eventLogger->info(
        "SUMA Stops resending due to out-of-buffer "
        "handling while resending. "
        "Bucket %u will become active. "
        "Bucket's max_acked_gci %u/%u "
        "resend-start gci %u/%u ",
        buck, Uint32(bucket->m_max_acked_gci >> 32),
        Uint32(bucket->m_max_acked_gci), (Uint32)(min_gci >> 32),
        (Uint32)(min_gci));

    // Inform the incident
    signal->theData[0] = NDB_LE_SubscriptionStatus;
    signal->theData[1] = 2;  // INCONSISTENT;
    signal->theData[2] = 0;  // Not used
    signal->theData[3] = (Uint32)(min_gci);
    signal->theData[4] = (Uint32)(min_gci >> 32);
    sendSignal(CMVMI_REF, GSN_EVENT_REP, signal, 5, JBB);

    return;
  }

  Uint32 tail = bucket->m_buffer_tail;
  Buffer_page *page = c_page_pool.getPtr(tail);
  Uint64 max_gci = page->m_max_gci_lo | (Uint64(page->m_max_gci_hi) << 32);
  Uint32 next_page = page->m_next_page;
  Uint32 *ptr = page->m_data + pos;
  Uint32 *end = page->m_data + page->m_words_used;
  bool delay = false;
  Uint32 next_pos = 0;  // Part read of next page

  ndbrequire(tail != RNIL);

  if (tail == bucket->m_buffer_head.m_page_id) {
    max_gci = bucket->m_buffer_head.m_max_gci;
    end = page->m_data + bucket->m_buffer_head.m_page_pos;
    next_page = RNIL;

    if (ptr == end) {
      delay = true;
      goto next;
    }
  } else if (pos == 0 && min_gci > max_gci) {
    free_page(tail, page);
    tail = bucket->m_buffer_tail = next_page;
    goto next;
  }

#if 0
  for(Uint32 i = 0; i<page->m_words_used; i++)
  {
    printf("%.8x ", page->m_data[i]);
    if(((i + 1) % 8) == 0)
      printf("\n");
  }
  printf("\n");
#endif

  /* Due to use of goto an extra scope is needed to keep declaration of
   * variables near their use.
   */
  {
    Uint32 *src = nullptr;
    Uint32 sz = 0;
    Uint32 part = 0;
    while (ptr < end) {
      jam();
      src = ptr;
      Uint32 tmp = *src;
      src++;

      sz = tmp & Buffer_page::SIZE_MASK;
      ptr += sz;

      ndbrequire(sz > 0);
      sz--;  // remove *len* part of sz

      part = (tmp >> Buffer_page::PART_NUM_SHIFT) & Buffer_page::PART_NUM_MASK;
      if ((tmp & Buffer_page::SAME_GCI_FLAG) == 0) {
        jam();
        ndbrequire(sz >= Buffer_page::GCI_SZ32);
        sz -= Buffer_page::GCI_SZ32;
        Uint32 last_gci_hi = *src;
        src++;
        Uint32 last_gci_lo = *src;
        src++;
        last_gci = last_gci_lo | (Uint64(last_gci_hi) << 32);
      } else {
        jam();
        ndbrequire(ptr - sz > page->m_data);
      }

      if (last_gci >= min_gci) {
        jam();
        // Found data with good gci
        break;
      }
      // Ignore data with old gci
      src = nullptr;
    }

    if (unlikely(src == nullptr)) {
      jam();
      // no valid data found on tail page
      ndbrequire(ptr == end);
    } else if (sz == 0) {
      jam();
      ndbrequire(part == 0);

      SubGcpCompleteRep *rep = (SubGcpCompleteRep *)signal->getDataPtrSend();
      Uint32 siglen = SubGcpCompleteRep::SignalLength;

      rep->gci_hi = (Uint32)(last_gci >> 32);
      rep->gci_lo = (Uint32)(last_gci & 0xFFFFFFFF);
      rep->flags = (m_missing_data) ? SubGcpCompleteRep::MISSING_DATA : 0;
      rep->senderRef = reference();
      rep->gcp_complete_rep_count = 1;

      // Append the sub data stream id for the bucket
      rep->sub_data_streams[0] = get_sub_data_stream(buck);
      rep->flags |= SubGcpCompleteRep::SUB_DATA_STREAMS_IN_SIGNAL;
      siglen++;

      if (ERROR_INSERTED(13036)) {
        jam();
        CLEAR_ERROR_INSERT_VALUE;
        g_eventLogger->info("Simulating out of event buffer at node failure");
        rep->flags |= SubGcpCompleteRep::MISSING_DATA;
      }

      char buf[NodeBitmask::TextLength + 1];
      c_subscriber_nodes.getText(buf);
      if (g_cnt) {
        jam();
        g_eventLogger->info("resending GCI: %u/%u rows: %d -> %s",
                            Uint32(last_gci >> 32), Uint32(last_gci), g_cnt,
                            buf);
      }
      g_cnt = 0;

      NodeReceiverGroup rg(API_CLUSTERMGR, c_subscriber_nodes);
      sendSignal(rg, GSN_SUB_GCP_COMPLETE_REP, signal, siglen, JBB);
    } else {
      jam();
      ndbrequire(part == 1);

      const uint buffer_header_sz = 6;
      g_cnt++;
      Uint32 subPtrI = src[0];
      Uint32 schemaVersion = src[1];
      Uint32 event = src[2] >> 16;
      Uint32 sz_1 = (src[2]) & 0xFFFF;
      Uint32 any_value = src[3];
      Uint32 transId1 = src[4];
      Uint32 transId2 = src[5];
      src += buffer_header_sz;

      ndbassert(sz - buffer_header_sz >= sz_1);
      Uint32 *src2;
      Uint32 sz2;
      {
        Uint32 *ptr2;
        if (ptr != end) {
          jam();
          ptr2 = ptr;
        } else {
          jam();
          // Second half of data on next page.
          Buffer_page *page = c_page_pool.getPtr(next_page);
          ndbrequire(page->m_words_used > 0);
          ptr2 = page->m_data;
        }
        src2 = ptr2;
        Uint32 tmp2 = *src2;
        src2++;
        sz2 = tmp2 & Buffer_page::SIZE_MASK;
        ndbrequire(sz2 > 0);
        sz2--;
        if (ptr2 != ptr) {
          jam();
          // First block on a page always must have gci.
          ndbrequire((tmp2 & Buffer_page::SAME_GCI_FLAG) == 0);
          next_pos = sz2 + 1;
        } else {
          jam();
          ptr = src2 + sz2;
        }

        part =
            (tmp2 >> Buffer_page::PART_NUM_SHIFT) & Buffer_page::PART_NUM_MASK;
        ndbrequire(part == 2);

        if ((tmp2 & Buffer_page::SAME_GCI_FLAG) == 0) {
          jam();
          ndbrequire(sz2 >= Buffer_page::GCI_SZ32);
          sz2 -= Buffer_page::GCI_SZ32;
          Uint32 last_gci_hi = *src2;
          src2++;
          Uint32 last_gci_lo = *src2;
          src2++;
          // Second block must have same gci as previous.
          ndbrequire(last_gci == (last_gci_lo | (Uint64(last_gci_hi) << 32)));
        }
      }

      LinearSectionPtr ptr[3];
      LinearSectionPtr lsptr[3];
      lsptr[0].p = src;
      lsptr[0].sz = sz_1;
      lsptr[1].p = src2;
      lsptr[1].sz = sz2;
      lsptr[2].p = src + sz_1;
      lsptr[2].sz = sz - buffer_header_sz - sz_1;
      const Uint32 nptr = reformat(signal, ptr, lsptr);

      Uint32 ptrLen = 0;
      for (Uint32 i = 0; i < nptr; i++) {
        jam();
        ptrLen += ptr[i].sz;
      }

      /**
       * Signal to subscriber(s)
       */
      Ptr<Subscription> subPtr;
      ndbrequire(c_subscriptionPool.getPtr(subPtr, subPtrI));
      Ptr<Table> tabPtr;
      ndbrequire(c_tablePool.getPtr(tabPtr, subPtr.p->m_table_ptrI));
      Uint32 table = subPtr.p->m_tableId;
      if (table_version_major(tabPtr.p->m_schemaVersion) ==
          table_version_major(schemaVersion)) {
        jam();
        SubTableData *data = (SubTableData *)signal->getDataPtrSend();  // trg;
        data->gci_hi = (Uint32)(last_gci >> 32);
        data->gci_lo = (Uint32)(last_gci & 0xFFFFFFFF);
        data->tableId = table;
        data->requestInfo = 0;
        SubTableData::setOperation(data->requestInfo, event);
        data->flags = 0;
        data->anyValue = any_value;
        data->totalLen = ptrLen;
        data->transId1 = transId1;
        data->transId2 = transId2;

        sendBatchedSUB_TABLE_DATA(signal, subPtr.p->m_subscribers, ptr, nptr);
      }
    }
  }

  if (next_pos > 0 || (ptr == end && tail != bucket->m_buffer_head.m_page_id)) {
    jam();
    /**
     * release...
     */
    ndbassert(tail != bucket->m_buffer_head.m_page_id);
    free_page(tail, page);
    tail = bucket->m_buffer_tail = next_page;
    pos = next_pos;
    if (pos == 0) {
      jam();
      last_gci = 0;
    }
  } else {
    jam();
    pos = Uint32(ptr - page->m_data);
  }

next:
  if (tail == RNIL) {
    jam();
    bucket->m_state &= ~(Uint32)Bucket::BUCKET_RESEND;

    /**
     * Bucket takeover in sendSUB_GCP_COMPLETE_REP() sets bucket head's
     * m_page_id and m_next_page to RNIL to inform the resend fiber to
     * indicate that the swtich_over gci is completed and no more data
     * will arrive. Resend fiber uses this info to terminate resending.
     */
    ndbassert(!(bucket->m_state & Bucket::BUCKET_TAKEOVER));

    g_eventLogger->info("SUMA Resend done for bucket %u", buck);
    return;
  }

  signal->theData[0] = SumaContinueB::RESEND_BUCKET;
  signal->theData[1] = buck;
  signal->theData[2] = (Uint32)(min_gci >> 32);
  signal->theData[3] = pos;
  signal->theData[4] = (Uint32)(last_gci >> 32);
  signal->theData[5] = (Uint32)(min_gci & 0xFFFFFFFF);
  signal->theData[6] = (Uint32)(last_gci & 0xFFFFFFFF);

#ifdef ERROR_INSERT
  if (ERROR_INSERTED(13061)) {
    jam();
    g_eventLogger->info(
        "resend_bucket: Error insertion 13061 delaying contB"
        " round %u",
        resend_bucket_round);
    delay = true;
  }
#endif

  if (!delay) {
    jam();
    sendSignal(SUMA_REF, GSN_CONTINUEB, signal, 7, JBB);
  } else {
    jam();
    sendSignalWithDelay(SUMA_REF, GSN_CONTINUEB, signal, 10, 7);
  }
}

void Suma::execGCP_PREPARE(Signal *signal) {
  jamEntry();
  const GCPPrepare *prep = (const GCPPrepare *)signal->getDataPtr();
  m_current_gci = prep->gci_lo | (Uint64(prep->gci_hi) << 32);
}

Uint64 Suma::get_current_gci(Signal *) { return m_current_gci; }

void Suma::execCREATE_NODEGROUP_IMPL_REQ(Signal *signal) {
  CreateNodegroupImplReq reqCopy =
      *(CreateNodegroupImplReq *)signal->getDataPtr();
  CreateNodegroupImplReq *req = &reqCopy;

  Uint32 err = 0;
  Uint32 rt = req->requestType;

  NdbNodeBitmask tmp;
  for (Uint32 i = 0; i < NDB_ARRAY_SIZE(req->nodes) && req->nodes[i]; i++) {
    ndbrequire(req->nodes[i] <= MAX_NDB_NODES);
    tmp.set(req->nodes[i]);
  }
  Uint32 cnt = tmp.count();
  Uint32 group = req->nodegroupId;

  switch (rt) {
    case CreateNodegroupImplReq::RT_ABORT:
      jam();
      break;
    case CreateNodegroupImplReq::RT_PARSE:
      jam();
      break;
    case CreateNodegroupImplReq::RT_PREPARE:
      jam();
      break;
    case CreateNodegroupImplReq::RT_COMMIT:
      jam();
      break;
    case CreateNodegroupImplReq::RT_COMPLETE:
      jam();
      CRASH_INSERTION(13043);

      Uint64 gci = (Uint64(req->gci_hi) << 32) | req->gci_lo;
      ndbrequire(gci > m_last_complete_gci);

      Uint32 state = 0;
      if (c_nodeGroup != RNIL) {
        jam();
        NdbNodeBitmask check = tmp;
        check.bitAND(c_nodes_in_nodegroup_mask);
        ndbrequire(check.isclear());
        ndbrequire(c_nodeGroup != group);
        ndbrequire(cnt == c_nodes_in_nodegroup_mask.count());
        state = Bucket::BUCKET_CREATED_OTHER;
      } else if (tmp.get(getOwnNodeId())) {
        jam();
        c_nodeGroup = group;
        c_nodes_in_nodegroup_mask.assign(tmp);
        fix_nodegroup();
        state = Bucket::BUCKET_CREATED_SELF;
      }
      if (state != 0) {
        for (Uint32 i = 0; i < c_no_of_buckets; i++) {
          jam();
          m_switchover_buckets.set(i);
          c_buckets[i].m_switchover_gci = gci - 1;  // start from gci
          c_buckets[i].m_state = state | (c_no_of_buckets << 8);
        }
      }
  }

  {
    CreateNodegroupImplConf *conf =
        (CreateNodegroupImplConf *)signal->getDataPtrSend();
    conf->senderRef = reference();
    conf->senderData = req->senderData;
    sendSignal(req->senderRef, GSN_CREATE_NODEGROUP_IMPL_CONF, signal,
               CreateNodegroupImplConf::SignalLength, JBB);
  }
  return;

  // error:
  CreateNodegroupImplRef *ref =
      (CreateNodegroupImplRef *)signal->getDataPtrSend();
  ref->senderRef = reference();
  ref->senderData = req->senderData;
  ref->errorCode = err;
  sendSignal(req->senderRef, GSN_CREATE_NODEGROUP_IMPL_REF, signal,
             CreateNodegroupImplRef::SignalLength, JBB);
  return;
}

void Suma::execDROP_NODEGROUP_IMPL_REQ(Signal *signal) {
  DropNodegroupImplReq reqCopy = *(DropNodegroupImplReq *)signal->getDataPtr();
  DropNodegroupImplReq *req = &reqCopy;

  Uint32 err = 0;
  Uint32 rt = req->requestType;
  Uint32 group = req->nodegroupId;

  switch (rt) {
    case DropNodegroupImplReq::RT_ABORT:
      jam();
      break;
    case DropNodegroupImplReq::RT_PARSE:
      jam();
      break;
    case DropNodegroupImplReq::RT_PREPARE:
      jam();
      break;
    case DropNodegroupImplReq::RT_COMMIT:
      jam();
      break;
    case DropNodegroupImplReq::RT_COMPLETE:
      jam();
      CRASH_INSERTION(13043);

      Uint64 gci = (Uint64(req->gci_hi) << 32) | req->gci_lo;
      ndbrequire(gci > m_last_complete_gci);

      Uint32 state;
      if (c_nodeGroup != group) {
        jam();
        state = Bucket::BUCKET_DROPPED_OTHER;
        break;
      } else {
        jam();
        state = Bucket::BUCKET_DROPPED_SELF;
      }

      for (Uint32 i = 0; i < c_no_of_buckets; i++) {
        jam();
        m_switchover_buckets.set(i);
        if (c_buckets[i].m_state != 0) {
          jamLine(c_buckets[i].m_state);
          g_eventLogger->info("c_buckets[%u].m_state: %u", i,
                              c_buckets[i].m_state);
        }
        ndbrequire(c_buckets[i].m_state == 0);    // XXX todo
        c_buckets[i].m_switchover_gci = gci - 1;  // start from gci
        c_buckets[i].m_state = state | (c_no_of_buckets << 8);
      }
      break;
  }

  {
    DropNodegroupImplConf *conf =
        (DropNodegroupImplConf *)signal->getDataPtrSend();
    conf->senderRef = reference();
    conf->senderData = req->senderData;
    sendSignal(req->senderRef, GSN_DROP_NODEGROUP_IMPL_CONF, signal,
               DropNodegroupImplConf::SignalLength, JBB);
  }
  return;

  // error:
  DropNodegroupImplRef *ref = (DropNodegroupImplRef *)signal->getDataPtrSend();
  ref->senderRef = reference();
  ref->senderData = req->senderData;
  ref->errorCode = err;
  sendSignal(req->senderRef, GSN_DROP_NODEGROUP_IMPL_REF, signal,
             DropNodegroupImplRef::SignalLength, JBB);
  return;
}

// template void
// append(DataBuffer<11>&,SegmentedSectionPtr,SectionSegmentPool&);
