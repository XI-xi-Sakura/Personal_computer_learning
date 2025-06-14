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

#define DBTUP_C
#define DBTUP_COMMIT_CPP
#include <ndb_limits.h>
#include <EventLogger.hpp>
#include <RefConvert.hpp>
#include <pc.hpp>
#include <signaldata/TupCommit.hpp>
#include "../backup/Backup.hpp"
#include "../dblqh/Dblqh.hpp"
#include "Dbtup.hpp"

#define JAM_FILE_ID 416

#if (defined(VM_TRACE) || defined(ERROR_INSERT))
// #define DEBUG_DISK 1
// #define DEBUG_LCP 1
// #define DEBUG_LCP_SKIP_DELETE_EXTRA 1
// #define DEBUG_INSERT_EXTRA 1
// #define DEBUG_LCP_SCANNED_BIT 1
// #define DEBUG_PGMAN 1
// #define DEBUG_ROW_COUNT_DEL 1
// #define DEBUG_ROW_COUNT_INS 1
// #define DEBUG_DELETE 1
// #define DEBUG_DELETE_EXTRA 1
// #define DEBUG_LCP_SKIP_DELETE2 1
// #define DEBUG_LCP_DEL 1
// #define DEBUG_LCP_SKIP 1
// #define DEBUG_LCP_SKIP_DELETE 1
#endif

#ifdef DEBUG_DISK
#define DEB_DISK(arglist)        \
  do {                           \
    g_eventLogger->info arglist; \
  } while (0)
#else
#define DEB_DISK(arglist) \
  do {                    \
  } while (0)
#endif

#ifdef DEBUG_LCP
#define DEB_LCP(arglist)         \
  do {                           \
    g_eventLogger->info arglist; \
  } while (0)
#else
#define DEB_LCP(arglist) \
  do {                   \
  } while (0)
#endif

#ifdef DEBUG_DELETE_EXTRA
#define DEB_DELETE_EXTRA(arglist) \
  do {                            \
    g_eventLogger->info arglist;  \
  } while (0)
#else
#define DEB_DELETE_EXTRA(arglist) \
  do {                            \
  } while (0)
#endif

#ifdef DEBUG_INSERT_EXTRA
#define DEB_INSERT_EXTRA(arglist) \
  do {                            \
    g_eventLogger->info arglist;  \
  } while (0)
#else
#define DEB_INSERT_EXTRA(arglist) \
  do {                            \
  } while (0)
#endif

#ifdef DEBUG_LCP_DEL
#define DEB_LCP_DEL(arglist)     \
  do {                           \
    g_eventLogger->info arglist; \
  } while (0)
#else
#define DEB_LCP_DEL(arglist) \
  do {                       \
  } while (0)
#endif

#ifdef DEBUG_LCP_SKIP
#define DEB_LCP_SKIP(arglist)    \
  do {                           \
    g_eventLogger->info arglist; \
  } while (0)
#else
#define DEB_LCP_SKIP(arglist) \
  do {                        \
  } while (0)
#endif

#ifdef DEBUG_LCP_SKIP_DELETE
#define DEB_LCP_SKIP_DELETE(arglist) \
  do {                               \
    g_eventLogger->info arglist;     \
  } while (0)
#else
#define DEB_LCP_SKIP_DELETE(arglist) \
  do {                               \
  } while (0)
#endif

#ifdef DEBUG_LCP_SKIP_DELETE2
#define DEB_LCP_SKIP_DELETE2(arglist) \
  do {                                \
    g_eventLogger->info arglist;      \
  } while (0)
#else
#define DEB_LCP_SKIP_DELETE2(arglist) \
  do {                                \
  } while (0)
#endif

#ifdef DEBUG_LCP_SCANNED_BIT
#define DEB_LCP_SCANNED_BIT(arglist) \
  do {                               \
    g_eventLogger->info arglist;     \
  } while (0)
#else
#define DEB_LCP_SCANNED_BIT(arglist) \
  do {                               \
  } while (0)
#endif

#ifdef DEBUG_PGMAN
#define DEB_PGMAN(arglist)       \
  do {                           \
    g_eventLogger->info arglist; \
  } while (0)
#else
#define DEB_PGMAN(arglist) \
  do {                     \
  } while (0)
#endif

#ifdef DEBUG_DELETE
#define DEB_DELETE(arglist)      \
  do {                           \
    g_eventLogger->info arglist; \
  } while (0)
#else
#define DEB_DELETE(arglist) \
  do {                      \
  } while (0)
#endif

void Dbtup::execTUP_DEALLOCREQ(Signal *signal) {
  TablerecPtr regTabPtr;
  FragrecordPtr regFragPtr;
  Uint32 frag_page_id, frag_id;

  jamEntry();

  frag_id = signal->theData[0];
  regTabPtr.i = signal->theData[1];
  frag_page_id = signal->theData[2];
  Uint32 page_index = signal->theData[3];

  ptrCheckGuard(regTabPtr, cnoOfTablerec, tablerec);

  getFragmentrec(regFragPtr, frag_id, regTabPtr.p);
  ndbassert(regFragPtr.p != NULL);

  if (!Local_key::isInvalid(frag_page_id, page_index)) {
    /**
     * When we arrive here we should always be using the primary table
     * fragment.
     */
    Dblqh::Fragrecord *fragPtrP =
        c_lqh->get_fragptr(regFragPtr.p->fragTableId, regFragPtr.p->fragmentId);
    c_lqh->upgrade_to_exclusive_frag_access(fragPtrP);
    Local_key tmp;
    tmp.m_page_no = getRealpid(regFragPtr.p, frag_page_id);
    tmp.m_page_idx = page_index;
    PagePtr pagePtr;
    Tuple_header *ptr = (Tuple_header *)get_ptr(&pagePtr, &tmp, regTabPtr.p);

    DEB_DELETE(("(%u)dealloc tab(%u,%u), row(%u,%u), header: %x", instance(),
                regTabPtr.i, frag_id, frag_page_id, page_index,
                ptr->m_header_bits));

    ndbrequire(ptr->m_header_bits & Tuple_header::FREE);

    if (regTabPtr.p->m_attributes[MM].m_no_of_varsize +
        regTabPtr.p->m_attributes[MM].m_no_of_dynamic) {
      jam();
      free_var_rec(regFragPtr.p, regTabPtr.p, &tmp, pagePtr);
    } else {
      free_fix_rec(regFragPtr.p, regTabPtr.p, &tmp, (Fix_page *)pagePtr.p);
    }
    c_lqh->downgrade_from_exclusive_frag_access(fragPtrP);
  } else {
    jam();
  }
}

void Dbtup::execTUP_WRITELOG_REQ(Signal *signal) {
  jamEntryDebug();
  OperationrecPtr loopOpPtr;
  loopOpPtr.i = signal->theData[0];
  Uint32 gci_hi = signal->theData[1];
  Uint32 gci_lo = signal->theData[2];
  ndbrequire(c_operation_pool.getValidPtr(loopOpPtr));
  ndbassert(!m_is_query_block);
  while (loopOpPtr.p->prevActiveOp != RNIL) {
    jamDebug();
    loopOpPtr.i = loopOpPtr.p->prevActiveOp;
    ndbrequire(c_operation_pool.getValidPtr(loopOpPtr));
  }
  do {
    ndbrequire(get_trans_state(loopOpPtr.p) == TRANS_STARTED);
    signal->theData[0] = loopOpPtr.p->userpointer;
    signal->theData[1] = gci_hi;
    signal->theData[2] = gci_lo;
    if (loopOpPtr.p->nextActiveOp == RNIL) {
      jamDebug();
      c_lqh->execLQH_WRITELOG_REQ(signal);
      return;
    }
    jamDebug();
    c_lqh->execLQH_WRITELOG_REQ(signal);
    jamEntryDebug();
    loopOpPtr.i = loopOpPtr.p->nextActiveOp;
    ndbrequire(c_operation_pool.getValidPtr(loopOpPtr));
  } while (true);
}

/* ---------------------------------------------------------------- */
/* INITIALIZATION OF ONE CONNECTION RECORD TO PREPARE FOR NEXT OP.  */
/* ---------------------------------------------------------------- */
void Dbtup::initOpConnection(Operationrec *regOperPtr) {
  set_tuple_state(regOperPtr, TUPLE_ALREADY_ABORTED);
  set_trans_state(regOperPtr, TRANS_IDLE);
  regOperPtr->m_commit_state = Operationrec::CommitNotStarted;
  regOperPtr->op_type = ZREAD;
  regOperPtr->op_struct.bit_field.m_disk_preallocated = 0;
  regOperPtr->op_struct.bit_field.m_load_diskpage_on_commit = 0;
  regOperPtr->op_struct.bit_field.m_wait_log_buffer = 0;
  regOperPtr->op_struct.bit_field.in_active_list = false;
  regOperPtr->m_undo_buffer_space = 0;
}

bool Dbtup::is_rowid_in_remaining_lcp_set(
    const Page *page, Fragrecord *regFragPtr, const Local_key &key1,
    const Dbtup::ScanOp &op, Uint32 check_lcp_scanned_state_reversed) {
  if (page->is_page_to_skip_lcp() ||
      (check_lcp_scanned_state_reversed == 0 &&
       get_lcp_scanned_bit(regFragPtr, key1.m_page_no))) {
    /**
     * We have to check whether the page have already been scanned by
     * the LCP. We have two different flags for this. The first one
     * is checked by is_page_to_skip_lcp(). This is set when a page
     * is allocated during an LCP scan and not previously released
     * in the same LCP scan.
     *
     * If a page is released during the LCP scan we set the lcp
     * scanned bit in the page map. We need to check both those to
     * see if the page have been LCP scanned.
     *
     * When check_lcp_scanned_state_reversed is != 0 we are not interested
     * in the lcp scanned state and will ignore checking this. We can
     * call it with check_lcp_scanned_state_reversed set to 0 even if we
     * know that the lcp scanned bit isn't set. The reason is that the
     * check_lcp_state_reversed is also used for debug printouts as well.
     */
    jam();
    return false; /* Page already scanned for skipped pages */
  }
  bool dummy;
  int ret_val = c_backup->is_page_lcp_scanned(key1.m_page_no, dummy);
  if (ret_val == +1) {
    jam();
    return false;
  } else if (ret_val == -1) {
    jam();
    if (check_lcp_scanned_state_reversed != 0) {
      DEB_LCP_SCANNED_BIT(("(%u)Line: %u, page: %u, debug_val: %u", instance(),
                           __LINE__, key1.m_page_no,
                           check_lcp_scanned_state_reversed));
    }
    return true;
  }
  /* We are scanning the given page */
  Local_key key2 = op.m_scanPos.m_key;
  switch (op.m_state) {
    case Dbtup::ScanOp::First: {
      jam();
      ndbrequire(key2.isNull());
      if (check_lcp_scanned_state_reversed != 0) {
        DEB_LCP_SCANNED_BIT(("(%u)Line: %u, page: %u, debug_val: %u",
                             instance(), __LINE__, key1.m_page_no,
                             check_lcp_scanned_state_reversed));
      }
      return true; /* Already checked page id above, so will scan the page */
    }
    case Dbtup::ScanOp::Current: {
      /* Impossible state for LCP scans */
      ndbabort();
    }
    case Dbtup::ScanOp::Next: {
      ndbrequire(key1.m_page_no == key2.m_page_no);
      ndbrequire(!key2.isNull());
      if (op.m_scanPos.m_get == ScanPos::Get_next_page_mm) {
        jam();
        /**
         * We got a real-time break while switching to a new page.
         * In this case we can skip the page since it is already
         * LCP:ed.
         */
        return false;
      }
      if (key1.m_page_idx < key2.m_page_idx) {
        jam();
        /* Ignore rows already LCP:ed */
        return false;
      }
      if (key1.m_page_idx > key2.m_page_idx) {
        jam();
        /* Include rows not LCP:ed yet */
        if (check_lcp_scanned_state_reversed != 0) {
          DEB_LCP_SCANNED_BIT(("(%u)Line: %u, page: %u, debug_val: %u",
                               instance(), __LINE__, key1.m_page_no,
                               check_lcp_scanned_state_reversed));
        }
        return true;
      }
      ndbrequire(key1.m_page_idx == key2.m_page_idx);
      /* keys are equal */
      jam();
      /* Ignore current row that already have been LCP:ed. */
      return false;
    }
    case Dbtup::ScanOp::Last:
    case Dbtup::ScanOp::Aborting: {
      jam();
      return false; /* Everything scanned already */
    }
    default:
      break;
  }
  /* Will never arrive here */
  jamLine(Uint16(op.m_state));
  ndbabort();
  return true;
}

void Dbtup::dealloc_tuple(Signal *signal, Uint32 gci_hi, Uint32 gci_lo,
                          Page *page, Tuple_header *ptr,
                          KeyReqStruct *req_struct, Operationrec *regOperPtr,
                          Fragrecord *regFragPtr, Tablerec *regTabPtr,
                          Ptr<GlobalPage> pagePtr) {
  Uint32 lcpScan_ptr_i = regFragPtr->m_lcp_scan_op;
  Uint32 average_row_size = regFragPtr->m_average_row_size;

  Uint32 bits = ptr->m_header_bits;
  Uint32 extra_bits = Tuple_header::FREE;
  c_lqh->add_delete_size(average_row_size);
  if (bits & Tuple_header::DISK_PART) {
    if (likely(pagePtr.i != RNIL)) {
      jam();
      ndbrequire(c_lqh->is_restore_phase_done());
      Local_key disk;
      memcpy(&disk, ptr->get_disk_ref_ptr(regTabPtr), sizeof(disk));
      PagePtr tmpptr;
      Local_key rowid = regOperPtr->m_tuple_location;
      rowid.m_page_no = page->frag_page_id;
      tmpptr.i = pagePtr.i;
      tmpptr.p = reinterpret_cast<Page *>(pagePtr.p);
      disk_page_free(signal, regTabPtr, regFragPtr, &disk, tmpptr, gci_hi,
                     &rowid, regOperPtr->m_undo_buffer_space);
    } else {
      ndbrequire(!c_lqh->is_restore_phase_done());
    }
  }

  if (!(bits & (Tuple_header::LCP_SKIP | Tuple_header::ALLOC |
                Tuple_header::LCP_DELETE)) &&
      lcpScan_ptr_i != RNIL) {
    jam();
    ScanOpPtr scanOp;
    scanOp.i = lcpScan_ptr_i;
    ndbrequire(c_scanOpPool.getValidPtr(scanOp));
    Local_key rowid = regOperPtr->m_tuple_location;
    rowid.m_page_no = page->frag_page_id;
    if (is_rowid_in_remaining_lcp_set(page, regFragPtr, rowid, *scanOp.p, 0)) {
      jam();

      /**
       * We're committing a delete, on a row that should
       *   be part of LCP. Copy original row into copy-tuple
       *   and add this copy-tuple to lcp-keep-list
       *
       * We also need to set the LCP_SKIP bit in the tuple header to avoid
       * that the LCP scan finds this row and records it as a deleted
       * rowid before the LCP scan start. This can happen on CHANGED ROW
       * pages only.
       *
       */
      /* Coverage tested */
      extra_bits |= Tuple_header::LCP_SKIP;
      DEB_LCP_SKIP_DELETE(
          ("(%u)tab(%u,%u), row(%u,%u),"
           " handle_lcp_keep_commit"
           ", set LCP_SKIP, bits: %x",
           instance(), regFragPtr->fragTableId, regFragPtr->fragmentId,
           rowid.m_page_no, rowid.m_page_idx, bits | extra_bits));
      handle_lcp_keep_commit(&rowid, req_struct, regOperPtr, regFragPtr,
                             regTabPtr);
    } else {
      /* Coverage tested */
      DEB_LCP_SKIP_DELETE2(
          ("(%u)tab(%u,%u), row(%u,%u) DELETE"
           " already LCP:ed",
           instance(), regFragPtr->fragTableId, regFragPtr->fragmentId,
           rowid.m_page_no, rowid.m_page_idx));
    }
  } else {
#ifdef DEBUG_LCP_SKIP_DELETE_EXTRA
    Local_key rowid = regOperPtr->m_tuple_location;
    rowid.m_page_no = page->frag_page_id;
    g_eventLogger->info(
        "(%u)tab(%u,%u)row(%u,%u),"
        ", skip LCP, bits: %x"
        ", lcpScan_ptr: %u",
        instance(), regFragPtr->fragTableId, regFragPtr->fragmentId,
        rowid.m_page_no, rowid.m_page_idx, bits, lcpScan_ptr_i);
#endif
  }

#ifdef DEBUG_DELETE_EXTRA
  if (c_started) {
    Local_key rowid = regOperPtr->m_tuple_location;
    rowid.m_page_no = page->frag_page_id;
    DEB_DELETE_EXTRA(("(%u)tab(%u,%u),DELETE row(%u,%u)", instance(),
                      regFragPtr->fragTableId, regFragPtr->fragmentId,
                      rowid.m_page_no, rowid.m_page_idx));
  }
#endif
  ptr->m_header_bits = bits | extra_bits;

  if (regTabPtr->m_bits & Tablerec::TR_RowGCI) {
    jam();
    update_gci(regFragPtr, regTabPtr, ptr, gci_hi);
    if (regTabPtr->m_bits & Tablerec::TR_ExtraRowGCIBits) {
      Uint32 attrId = regTabPtr->getExtraAttrId<Tablerec::TR_ExtraRowGCIBits>();
      store_extra_row_bits(attrId, regTabPtr, ptr, gci_lo, /* truncate */ true);
    }
  } else {
    /**
     * This should be dead code, but we ensure that we don't miss those
     * updates even for those tables.
     */
    jam();
    regFragPtr->m_lcp_changed_rows++;
  }
  Tup_fixsize_page *fix_page = (Tup_fixsize_page *)page;
  fix_page->set_change_maps(regOperPtr->m_tuple_location.m_page_idx);
  ndbassert(fix_page->verify_change_maps(jamBuffer()));
  fix_page->set_max_gci(gci_hi);
  setInvalidChecksum(ptr, regTabPtr);
  if (regOperPtr->op_struct.bit_field.m_tuple_existed_at_start) {
    ndbrequire(regFragPtr->m_row_count > 0);
    regFragPtr->m_row_count--;
#ifdef DEBUG_ROW_COUNT_DEL
    Local_key rowid = regOperPtr->m_tuple_location;
    rowid.m_page_no = page->frag_page_id;
    g_eventLogger->info(
        "(%u) tab(%u,%u) Deleted row(%u,%u)"
        ", bits: %x, row_count = %llu"
        ", tuple_header_ptr: %p, gci: %u",
        instance(), regFragPtr->fragTableId, regFragPtr->fragmentId,
        rowid.m_page_no, rowid.m_page_idx, ptr->m_header_bits,
        regFragPtr->m_row_count, ptr, gci_hi);
#endif
  }
}

void Dbtup::update_gci(Fragrecord *regFragPtr, Tablerec *regTabPtr,
                       Tuple_header *ptr, Uint32 new_gci) {
  /**
   * Update GCI on the row, also update statistics used by LCP.
   */
  Uint32 *gci_ptr = ptr->get_mm_gci(regTabPtr);
  Uint32 old_gci = *gci_ptr;
  *gci_ptr = new_gci;
  if (old_gci <= regFragPtr->m_lcp_start_gci) {
    jam();
    regFragPtr->m_lcp_changed_rows++;
  }
}

void Dbtup::handle_lcp_keep_commit(const Local_key *rowid,
                                   KeyReqStruct *req_struct,
                                   Operationrec *opPtrP, Fragrecord *regFragPtr,
                                   Tablerec *regTabPtr) {
  bool disk = false;
  /* Coverage tested */
  Uint32 sizes[4];
  Uint32 *copytuple = get_copy_tuple_raw(&opPtrP->m_copy_tuple_location);
  Tuple_header *dst = get_copy_tuple(copytuple);
  Tuple_header *org = req_struct->m_tuple_ptr;
  if (regTabPtr->need_expand(disk)) {
    jam();
    req_struct->fragPtrP = regFragPtr;
    req_struct->m_row_id = opPtrP->m_tuple_location;
    req_struct->operPtrP = opPtrP;
    setup_fixed_tuple_ref(req_struct, opPtrP, regTabPtr);
    setup_fixed_part(req_struct, opPtrP, regTabPtr);
    req_struct->m_tuple_ptr = dst;
    expand_tuple(req_struct, sizes, org, regTabPtr, disk, true);
    shrink_tuple(req_struct, sizes + 2, regTabPtr, disk);
  } else {
    jam();
    memcpy(dst, org, 4 * regTabPtr->m_offsets[MM].m_fix_header_size);
  }
  dst->m_header_bits |= Tuple_header::COPY_TUPLE;

  setChecksum(dst, regTabPtr);
  /**
   * Link it to list
   */
  insert_lcp_keep_list(regFragPtr, opPtrP->m_copy_tuple_location, copytuple,
                       rowid);
  /**
   * And finally clear m_copy_tuple_location so that it won't be freed
   */
  opPtrP->m_copy_tuple_location.setNull();
}

#if 0
static void dump_buf_hex(unsigned char *p, Uint32 bytes)
{
  char buf[3001];
  char *q= buf;
  buf[0]= '\0';

  for(Uint32 i=0; i<bytes; i++)
  {
    if(i==((sizeof(buf)/3)-1))
    {
      sprintf(q, "...");
      break;
    }
    sprintf(q+3*i, " %02X", p[i]);
  }
  g_eventLogger->info("%8p: %s", p, buf);
}
#endif

/**
 * Handling COMMIT
 * ---------------
 * The most complex part of our operations on a tuple is when we have
 * multiple row operations on the same tuple within the same operation.
 * There might even be an insert followed by a delete followed by a
 * new insert followed by an update! The only operation that isn't
 * allowed is a DELETE followed by a DELETE and an INSERT followed by
 * an INSERT and a DELETE followed by an UPDATE.
 *
 * Each operation carries with it a copy row. This makes it easy to
 * commit and abort multi-operations on one tuple within one
 * transaction.
 *
 * At the time of the commit we can have multiple operations in a list
 * linked from the row. The "surviving" operation is the one which is
 * last in the list. This is the only operation that will be truly
 * committed. All other copy rows simply represent intermediate states
 * in getting to the committed state. The transaction itself can have
 * seen these uncommitted intermediate states, but no other transaction
 * have the ability to see those intermediate row states.
 *
 * The last operation in the list is the operation linked from the
 * tuple header. The "last" operation in the list was also the last
 * operation prepared.
 *
 * The last operation in the list will be committed for "real". This means
 * that the copy row for the last operation will be copied to the rowid of
 * the row. However the TUX commit triggers are fired on the first operation
 * in the operation list.
 *
 * COMMIT handling of shrinking varpart's
 * --------------------------------------
 * The varpart entry header contains the actual length of the varpart
 * allocated from the page. This size might be equal or bigger than
 * the size of the varpart to be committed. We will always at COMMIT time
 * ensure that we shrink it to the minimum size. It might even be
 * shrunk to 0 in which case we free the varpart entirely.
 *
 * Handling ABORT
 * --------------
 * Given that we have a copy tuple for each row it means that it is very
 * easy to abort operations without aborting the entire transaction. Abort
 * can happen at any time before the commit has started and abort can
 * happen either on the entire transaction or on a subset of the transaction.
 *
 * One example when we can abort a subset of the transaction is when we get
 * an LQHKEYREF returned from the backup replica. In this case we did a
 * successful operation at the primary replica, but at the backup replica
 * we failed for some reason. There might actually even be multiple operations
 * outstanding at the same time since we allow for multiple operations within
 * the same batch to execute in parallel. It is not defined what the end
 * result will be if such a batch have multiple updates on the same row, but
 * we still have to ensure that we can handle those cases in a secure manner.
 *
 * This also means that the code is prepared to allow for aborting to a
 * savepoint. However the functionality that handles this will be in DBTC and
 * is independent of the code here in DBTUP.
 *
 * When aborting an operation we simply drop it from the list of operations
 * on the row and if it is the last then we also restore the header.
 * This means that an abort operation for a row with multiple changes to it
 * is really easy, it needs only to drop the operation and drop the copy
 * row attached to it.
 *
 * If we increase the size of the varpart for a row we need to extend the
 * size. This means that the header of the varpart will contain the new
 * length. So in order to restore we need to store the original varpart
 * length somewhere.
 *
 * The MM_GROWN bit and its meaning
 * --------------------------------
 * During an operation that increases the size of the varpart we might actually
 * change the location of the committed varpart of the row. To ensure that any
 * readers of the row that does a COMMITTED READ can still see the original
 * row size we store this at the last word of the new varpart. We also set the
 * MM_GROWN bit in the tuple header to indicate this.
 *
 * The consequence of this is that an aborted transaction can not have changed
 * the row content, but it can have changed the place the row is stored. The
 * actual row content is however only changed when we commit the transaction,
 * until then the new data is always stored in the copy rows.
 *
 * When aborting we need to care about MM_GROWN since then we have to restore
 * the varpart size by shrinking it. If MM_GROWN is set we might have attempted
 * to shrink the tuple, but this information is only represented by a smaller
 * size of the copy row and thus when the copy row is free'd we have done
 * everything needed to abort this operation.
 *
 * Acceptable order of ABORT and COMMIT and WRITE operations
 * ---------------------------------------------------------
 * So acceptable order of COMMIT's is that once a COMMIT has arrived on a row
 * then no ABORT is allowed AND no new WRITE operation on the row in the same
 * transaction is allowed. When the commit is complete then the row is
 * unlocked and ready for a new transaction again. COMMIT operations can
 * arrive in any order.
 *
 * Before any operation on the row have received COMMIT we can receive ABORT
 * operations in any order. TUP have no ability to verify that the upper level
 * ABORT operations are executed correctly. However since ABORTs can happen in
 * any order it is only vital that the correct operations are ABORTed, it
 * doesn't matter in which order they are ABORTed.
 *
 * The upper level (mainly TC and LQH) will maintain the correctness when it
 * comes to transaction concepts.
 */
void Dbtup::commit_operation(Signal *signal, Uint32 gci_hi, Uint32 gci_lo,
                             Tuple_header *tuple_ptr, PagePtr pagePtr,
                             Operationrec *regOperPtr, Fragrecord *regFragPtr,
                             Tablerec *regTabPtr,
                             Ptr<GlobalPage> globDiskPagePtr) {
  ndbassert(regOperPtr->op_type != ZDELETE);

  Uint32 lcpScan_ptr_i = regFragPtr->m_lcp_scan_op;
  Uint32 save = tuple_ptr->m_operation_ptr_i;
  Uint32 bits = tuple_ptr->m_header_bits;

  Tuple_header *disk_ptr = 0;
  Tuple_header *copy = get_copy_tuple(&regOperPtr->m_copy_tuple_location);

  Uint32 copy_bits = copy->m_header_bits;

  Uint32 fixsize = regTabPtr->m_offsets[MM].m_fix_header_size;
  Uint32 mm_vars = regTabPtr->m_attributes[MM].m_no_of_varsize;
  Uint32 mm_dyns = regTabPtr->m_attributes[MM].m_no_of_dynamic;
  bool update_gci_at_commit = !regOperPtr->op_struct.bit_field.m_gci_written;
  if ((mm_vars + mm_dyns) == 0) {
    jam();
    memcpy(tuple_ptr, copy, 4 * fixsize);
    disk_ptr = (Tuple_header *)(((Uint32 *)copy) + fixsize);
  } else {
    jam();
    /**
     * Var_part_ref is only stored in *allocated* tuple
     * so memcpy from copy, will over write it...
     * hence subtle copyout/assign...
     */
    Local_key tmp;
    Var_part_ref *ref = tuple_ptr->get_var_part_ref_ptr(regTabPtr);
    ref->copyout(&tmp);

    memcpy(tuple_ptr, copy, 4 * fixsize);
    ref->assign(&tmp);

    PagePtr vpagePtr;
    if (copy_bits & Tuple_header::VAR_PART) {
      jam();
      ndbassert(bits & Tuple_header::VAR_PART);
      ndbassert(tmp.m_page_no != RNIL);
      ndbassert(copy_bits & Tuple_header::COPY_TUPLE);

      Uint32 *dst = get_ptr(&vpagePtr, *ref);
      Var_page *vpagePtrP = (Var_page *)vpagePtr.p;
      Varpart_copy *vp =
          (Varpart_copy *)copy->get_end_of_fix_part_ptr(regTabPtr);
      /* The first word of shrunken tuple holds the length in words. */
      Uint32 len = vp->m_len;
      memcpy(dst, vp->m_data, 4 * len);

      /**
       * When we come here we will commit a varpart with length specified in
       * the copy tuple.
       *
       * The length in the page entry specifies the length we have allocated.
       * This means that the page entry length either specifies the original
       * length or the length that we allocated when growing the varsize part
       * of the tuple.
       *
       * The following cases exists:
       * 1) MM_GROWN not set
       *    Since MM_GROWN is never set then we have never extended the length
       *    of the varpart. We might however have executed one operation that
       *    shrunk the varpart size followed by an operation that grew the
       *    varpart again. It can however not have grown to be bigger than the
       *    original size since then MM_GROWN would be set.
       *
       *    The new varpart length might thus in this case be smaller than the
       *    page entry length.
       *
       * 2) MM_GROWN set
       *    In this case we have extended the varpart size in some operation.
       *
       *    If no more operation was performed after that then the page entry
       *    length and the committed varpart length will be equal. However if
       *    more operations are executed after this operation then they might
       *    decrease the varpart length without updating the page entry length.
       *    So also in this case we might actually have a smaller committed
       *    varpart length compared to the current page entry length.
       *
       * So the conclusion is that when we arrive here we can always have a
       * smaller committed varpart length compared to the page entry length.
       * So we always need to check whether we should shrink the varpart
       * entry to the committed length. The new committed length might even
       * be zero in which case we should release the varpart entirely.
       *
       * We need to check this independent of if MM_GROWN is set or not as
       * there might be multiple row operations both increasing and
       * shrinking the tuple.
       */
      ndbassert(vpagePtrP->get_entry_len(tmp.m_page_idx) >= len);
      if (vpagePtrP->get_entry_len(tmp.m_page_idx) > len) {
        /**
         * Page entry is now bigger than it needs to be, we are committing
         * and can thus shrink the entry to its correct size now.
         */
        jam();
        if (len) {
          jam();
          ndbassert(regFragPtr->m_varWordsFree >= vpagePtrP->free_space);
          regFragPtr->m_varWordsFree -= vpagePtrP->free_space;
          vpagePtrP->shrink_entry(tmp.m_page_idx, len);
          // Adds the new free space value for the page to the fragment total.
          update_free_page_list(regFragPtr, vpagePtr);
        } else {
          jam();
          /**
           * We have shrunk the varsize part down to zero, so in this case
           * we don't shrink it, in this case we simply free it.
           */
          free_var_part(regFragPtr, vpagePtr, tmp.m_page_idx);
          tmp.m_page_no = RNIL;
          ref->assign(&tmp);
          copy_bits &= ~(Uint32)Tuple_header::VAR_PART;
        }
      }
      /**
       * Find disk part after
       * header + fixed MM part + length word + varsize part.
       */
      disk_ptr = (Tuple_header *)(vp->m_data + len);
    } else {
      jam();
      ndbassert(tmp.m_page_no == RNIL);
      disk_ptr = (Tuple_header *)copy->get_end_of_fix_part_ptr(regTabPtr);
    }
  }

  if (regTabPtr->m_no_of_disk_attributes &&
      (copy_bits & Tuple_header::DISK_INLINE)) {
    jam();
    Local_key key;
    memcpy(&key, copy->get_disk_ref_ptr(regTabPtr), sizeof(Local_key));
    Uint32 logfile_group_id = regFragPtr->m_logfile_group_id;

    PagePtr diskPagePtr((Tup_page *)globDiskPagePtr.p, globDiskPagePtr.i);
    ndbassert(diskPagePtr.p->m_page_no == key.m_page_no);
    ndbassert(diskPagePtr.p->m_file_no == key.m_file_no);
    Uint32 sz, *dst;
    if (copy_bits & Tuple_header::DISK_ALLOC) {
      jam();
      Local_key rowid = regOperPtr->m_tuple_location;
      rowid.m_page_no = pagePtr.p->frag_page_id;
      disk_page_alloc(signal, regTabPtr, regFragPtr, &key, diskPagePtr, gci_hi,
                      &rowid, regOperPtr->m_undo_buffer_space);
      DEB_DISK(("(%u) Commit disk insert for row(%u,%u) disk_row(%u,%u).%u",
                instance(), rowid.m_page_no, rowid.m_page_idx, key.m_file_no,
                key.m_page_no, key.m_page_idx));
    }

    if (regTabPtr->m_attributes[DD].m_no_of_varsize == 0) {
      jam();
      sz = regTabPtr->m_offsets[DD].m_fix_header_size;
      dst = ((Fix_page *)diskPagePtr.p)->get_ptr(key.m_page_idx, sz);
    } else {
      jam();
      dst = ((Var_page *)diskPagePtr.p)->get_ptr(key.m_page_idx);
      sz = ((Var_page *)diskPagePtr.p)->get_entry_len(key.m_page_idx);
    }

    if (!(copy_bits & Tuple_header::DISK_ALLOC)) {
      jam();
#ifdef DEBUG_PGMAN
      Uint64 lsn =
#endif
          disk_page_undo_update(signal, diskPagePtr.p, &key, dst, sz, gci_hi,
                                logfile_group_id,
                                regOperPtr->m_undo_buffer_space);
      DEB_PGMAN((
          "disk_page_undo_update: page(%u,%u,%u).%u, LSN(%u,%u), gci: %u",
          instance(), key.m_file_no, key.m_page_no, key.m_page_idx,
          Uint32(Uint64(lsn >> 32)), Uint32(Uint64(lsn & 0xFFFFFFFF)), gci_hi));
    }

    memcpy(dst, disk_ptr, 4 * sz);
    memcpy(tuple_ptr->get_disk_ref_ptr(regTabPtr), &key, sizeof(Local_key));
    ndbrequire(disk_ptr->m_base_record_page_idx < Tup_page::DATA_WORDS);
    copy_bits |= Tuple_header::DISK_PART;
  }

#ifdef DEBUG_INSERT_EXTRA
  if (c_started) {
    Local_key rowid = regOperPtr->m_tuple_location;
    rowid.m_page_no = pagePtr.p->frag_page_id;
    g_eventLogger->info("(%u)tab(%u,%u) commit row(%u,%u)", instance(),
                        regFragPtr->fragTableId, regFragPtr->fragmentId,
                        rowid.m_page_no, rowid.m_page_idx);
  }
#endif
  Uint32 lcp_bits = 0;
  if (lcpScan_ptr_i != RNIL && (bits & Tuple_header::ALLOC) &&
      !(bits & (Tuple_header::LCP_SKIP | Tuple_header::LCP_DELETE))) {
    jam();
    ScanOpPtr scanOp;
    scanOp.i = lcpScan_ptr_i;
    ndbrequire(c_scanOpPool.getValidPtr(scanOp));
    Local_key rowid = regOperPtr->m_tuple_location;
    rowid.m_page_no = pagePtr.p->frag_page_id;
    if (is_rowid_in_remaining_lcp_set(pagePtr.p, regFragPtr, rowid, *scanOp.p,
                                      0)) {
      bool all_part;
      ndbrequire(c_backup->is_page_lcp_scanned(rowid.m_page_no, all_part) !=
                 +1);
      if (all_part) {
        /**
         * Rows that are inserted during LCPs are never required to be
         * recorded as part of the LCP, this can be avoided in multiple ways,
         * in this case we avoid it by setting bit on Tuple header.
         */
        jam();
        /* Coverage tested */
        lcp_bits |= Tuple_header::LCP_SKIP;
        DEB_LCP_SKIP(("(%u)Set LCP_SKIP on tab(%u,%u), row(%u,%u)", instance(),
                      regFragPtr->fragTableId, regFragPtr->fragmentId,
                      rowid.m_page_no, rowid.m_page_idx));
      } else {
        jam();
        /**
         * The row state at start of LCP was deleted, so we need to record
         * this to ensure that it doesn't disappear with a later insert
         * operation.
         */
        /* Coverage tested */
        DEB_LCP_DEL(("(%u)Set LCP_DELETE on tab(%u,%u), row(%u,%u)", instance(),
                     regFragPtr->fragTableId, regFragPtr->fragmentId,
                     rowid.m_page_no, rowid.m_page_idx));
        ndbrequire(c_backup->is_partial_lcp_enabled());
        lcp_bits |= Tuple_header::LCP_DELETE;
      }
    }
  }

  /**
   * Here we are copying header bits from the copy row to the main row.
   * We need to ensure that a few bits are retained from the main row
   * that are not necessarily set in the copy row.
   *
   * For example a row could have its LCP_SKIP set when it is updated
   * or deleted before the LCP reaches it. After deleting it is important
   * not to clear these when starting a new insert on the same row id.
   * This is handled in DbtupExecQuery.cpp. Here we can be committing the
   * same insert, so again it is important to not lose the LCP bits
   * on the main row. The LCP bits are never needed on the copy row since
   * the LCP only cares about the main rows. The LCP can even change
   * the LCP bits between prepare and commit of a row change. Thus it is
   * important to not lose the LCP_SKIP bit here.
   *
   * Similarly for LCP_DELETE we might lose the state after coming here
   * again before the LCP have had time to come and reset the bits.
   *
   * Similarly it is very important to not transport those bits from the
   * copy row back to the main row. These bits should only be used in the
   * main row and we should never take those bits from the copy row back
   * to the main row.
   */

  Uint32 clear = Tuple_header::ALLOC | Tuple_header::FREE |
                 Tuple_header::COPY_TUPLE | Tuple_header::DISK_ALLOC |
                 Tuple_header::DISK_INLINE | Tuple_header::MM_GROWN |
                 Tuple_header::LCP_SKIP | Tuple_header::LCP_DELETE;
  copy_bits &= ~(Uint32)clear;
  lcp_bits |= (bits & (Tuple_header::LCP_SKIP | Tuple_header::LCP_DELETE));

  tuple_ptr->m_header_bits = copy_bits | lcp_bits;
  tuple_ptr->m_operation_ptr_i = save;
  ndbrequire(!m_is_in_query_thread);

  Tup_fixsize_page *fix_page = (Tup_fixsize_page *)pagePtr.p;
  fix_page->set_change_maps(regOperPtr->m_tuple_location.m_page_idx);
  fix_page->set_max_gci(gci_hi);
  ndbassert(fix_page->verify_change_maps(jamBuffer()));

  if (regTabPtr->m_bits & Tablerec::TR_RowGCI && update_gci_at_commit) {
    jam();
    update_gci(regFragPtr, regTabPtr, tuple_ptr, gci_hi);
    if (regTabPtr->m_bits & Tablerec::TR_ExtraRowGCIBits) {
      jam();
      Uint32 attrId = regTabPtr->getExtraAttrId<Tablerec::TR_ExtraRowGCIBits>();
      store_extra_row_bits(attrId, regTabPtr, tuple_ptr, gci_lo,
                           /* truncate */ true);
    }
  } else {
    /**
     * This should be dead code, but we ensure that we don't miss those
     * updates even for those tables.
     *
     * In case of an explicit GCI update we always increment number of
     * changed rows to ensure we don't miss any updates.
     */
    jam();
    regFragPtr->m_lcp_changed_rows++;
  }
  setChecksum(tuple_ptr, regTabPtr);
  Uint32 average_row_size = regFragPtr->m_average_row_size;
  if (!regOperPtr->op_struct.bit_field.m_tuple_existed_at_start) {
    regFragPtr->m_row_count++;
    c_lqh->add_insert_size(average_row_size);
#ifdef DEBUG_ROW_COUNT_INS
    Local_key rowid = regOperPtr->m_tuple_location;
    rowid.m_page_no = pagePtr.p->frag_page_id;
    g_eventLogger->info(
        "(%u) tab(%u,%u) Inserted row(%u,%u)"
        ", bits: %x, row_count = %llu, tuple_ptr: %p, gci: %u",
        instance(), regFragPtr->fragTableId, regFragPtr->fragmentId,
        rowid.m_page_no, rowid.m_page_idx, tuple_ptr->m_header_bits,
        regFragPtr->m_row_count, tuple_ptr, gci_hi);
#endif
  } else {
    c_lqh->add_update_size(average_row_size);
  }
}

void Dbtup::disk_page_commit_callback(Signal *signal, Uint32 opPtrI,
                                      Uint32 page_id) {
  Uint32 hash_value;
  Uint32 gci_hi, gci_lo;
  Uint32 transId1, transId2;
  OperationrecPtr regOperPtr;
  Ptr<GlobalPage> diskPagePtr;

  jamEntry();

  regOperPtr.i = opPtrI;
  ndbrequire(c_operation_pool.getValidPtr(regOperPtr));
  c_lqh->get_op_info(regOperPtr.p->userpointer, &hash_value, &gci_hi, &gci_lo,
                     &transId1, &transId2);

  TupCommitReq *const tupCommitReq = (TupCommitReq *)signal->getDataPtr();

  tupCommitReq->opPtr = opPtrI;
  tupCommitReq->hashValue = hash_value;
  tupCommitReq->gci_hi = gci_hi;
  tupCommitReq->gci_lo = gci_lo;
  tupCommitReq->diskpage = page_id;
  tupCommitReq->transId1 = transId1;
  tupCommitReq->transId2 = transId2;

  regOperPtr.p->op_struct.bit_field.m_load_diskpage_on_commit = 0;
  regOperPtr.p->m_commit_disk_callback_page = page_id;
  ndbrequire(m_global_page_pool.getPtr(diskPagePtr, page_id));
  prepare_oper_ptr = regOperPtr;

  {
    PagePtr tmp;
    tmp.i = diskPagePtr.i;
    tmp.p = reinterpret_cast<Page *>(diskPagePtr.p);
    disk_page_set_dirty(tmp);
  }
  exec_tup_commit(signal);
}

void Dbtup::disk_page_log_buffer_callback(Signal *signal, Uint32 opPtrI,
                                          Uint32 unused) {
  Uint32 hash_value;
  Uint32 gci_hi, gci_lo;
  Uint32 transId1, transId2;
  OperationrecPtr regOperPtr;

  jamEntry();

  regOperPtr.i = opPtrI;
  ndbrequire(c_operation_pool.getValidPtr(regOperPtr));
  c_lqh->get_op_info(regOperPtr.p->userpointer, &hash_value, &gci_hi, &gci_lo,
                     &transId1, &transId2);
  Uint32 page = regOperPtr.p->m_commit_disk_callback_page;
  prepare_oper_ptr = regOperPtr;

  TupCommitReq *const tupCommitReq = (TupCommitReq *)signal->getDataPtr();

  tupCommitReq->opPtr = opPtrI;
  tupCommitReq->hashValue = hash_value;
  tupCommitReq->gci_hi = gci_hi;
  tupCommitReq->gci_lo = gci_lo;
  tupCommitReq->diskpage = page;
  tupCommitReq->transId1 = transId1;
  tupCommitReq->transId2 = transId2;

  ndbassert(regOperPtr.p->op_struct.bit_field.m_load_diskpage_on_commit == 0);
  regOperPtr.p->op_struct.bit_field.m_wait_log_buffer = 0;

  exec_tup_commit(signal);
}

int Dbtup::retrieve_data_page(Signal *signal, Page_cache_client::Request req,
                              OperationrecPtr regOperPtr,
                              Ptr<GlobalPage> &diskPagePtr,
                              Fragrecord *fragPtrP) {
  req.m_callback.m_callbackData = regOperPtr.i;
  req.m_table_id = fragPtrP->fragTableId;
  req.m_fragment_id = fragPtrP->fragmentId;
  req.m_callback.m_callbackFunction =
      safe_cast(&Dbtup::disk_page_commit_callback);

  /*
   * Consider commit to be correlated.  Otherwise pk op + commit makes
   * the page hot.   XXX move to TUP which knows better.
   */
  int flags = regOperPtr.p->op_type | Page_cache_client::COMMIT_REQ |
              Page_cache_client::CORR_REQ;
  Page_cache_client pgman(this, c_pgman);
  int res = pgman.get_page(signal, req, flags);
  diskPagePtr = pgman.m_ptr;

  switch (res) {
    case 0:
      /**
       * Timeslice
       */
      jam();
      signal->theData[0] = 1;
      return res;
    case -1:
      ndbrequire("NOT YET IMPLEMENTED" == 0);
      break;
    default:
      ndbrequire(res > 0);
      jam();
  }
  {
    PagePtr tmpptr;
    tmpptr.i = diskPagePtr.i;
    tmpptr.p = reinterpret_cast<Page *>(diskPagePtr.p);

    disk_page_set_dirty(tmpptr);
  }
  regOperPtr.p->m_commit_disk_callback_page = res;
  regOperPtr.p->op_struct.bit_field.m_load_diskpage_on_commit = 0;

  return res;
}

int Dbtup::retrieve_log_page(Signal *signal, FragrecordPtr regFragPtr,
                             OperationrecPtr regOperPtr) {
  jam();
  /**
   * Only last op on tuple needs "real" commit,
   *   hence only this one should have m_wait_log_buffer
   */

  CallbackPtr cb;
  cb.m_callbackData = regOperPtr.i;
  cb.m_callbackIndex = DISK_PAGE_LOG_BUFFER_CALLBACK;
  Uint32 sz = regOperPtr.p->m_undo_buffer_space;

  int res;
  {
    D("Logfile_client - execTUP_COMMITREQ");
    Logfile_client lgman(this, c_lgman, regFragPtr.p->m_logfile_group_id);
    res = lgman.get_log_buffer(signal, sz, &cb);
  }
  jamEntry();
  switch (res) {
    case 0:
      jam();
      signal->theData[0] = 1;
      return res;
    case -1:
      g_eventLogger->warning(
          "Out of space in RG_TRANSACTION_MEMORY resource,"
          " increase config parameter GlobalSharedMemory");
      ndbrequire("NOT YET IMPLEMENTED" == 0);
      break;
    default:
      jam();
  }
  regOperPtr.p->op_struct.bit_field.m_wait_log_buffer = 0;

  return res;
}

/**
 * Move to the first operation performed on this tuple
 */
void Dbtup::findFirstOp(OperationrecPtr &firstPtr) {
  jam();
  ndbassert(!firstPtr.p->is_first_operation());
  while (firstPtr.p->prevActiveOp != RNIL) {
    firstPtr.i = firstPtr.p->prevActiveOp;
    ndbrequire(c_operation_pool.getValidPtr(firstPtr));
  }
}

Uint32 Dbtup::prepare_disk_page_for_commit(Signal *signal,
                                           OperationrecPtr leaderOperPtr,
                                           Tuple_header *tuple_ptr,
                                           Ptr<GlobalPage> &diskPagePtr) {
  bool initial_delete = false;
  Tablerec *regTabPtrP = prepare_tabptr.p;
  FragrecordPtr regFragPtr = prepare_fragptr;
  if (leaderOperPtr.p->op_struct.bit_field.m_load_diskpage_on_commit) {
    jam();
    Page_cache_client::Request req;

    /**
     * Only last op on tuple needs "real" commit,
     *   hence only this one should have m_load_diskpage_on_commit
     */
    ndbassert(tuple_ptr->m_operation_ptr_i == leaderOperPtr.i);

    /**
     * Check for page
     */
    if (!leaderOperPtr.p->m_copy_tuple_location.isNull()) {
      jam();
      Tuple_header *tmp =
          get_copy_tuple(&leaderOperPtr.p->m_copy_tuple_location);

      memcpy(&req.m_page, tmp->get_disk_ref_ptr(regTabPtrP), sizeof(Local_key));

      if (unlikely(leaderOperPtr.p->op_type == ZDELETE &&
                   tmp->m_header_bits & Tuple_header::DISK_ALLOC)) {
        jam();
        /**
         * Insert+Delete
         * In this case we want to release the Copy page tuple that was
         * allocated for the insert operation since the commit of the
         * delete operation here makes it unnecessary to save the
         * new record.
         */
        leaderOperPtr.p->op_struct.bit_field.m_load_diskpage_on_commit = 0;
        leaderOperPtr.p->op_struct.bit_field.m_wait_log_buffer = 0;
        disk_page_abort_prealloc(signal, regFragPtr.p, &req.m_page,
                                 req.m_page.m_page_idx);

        {
          D("Logfile_client - execTUP_COMMITREQ");
          Logfile_client lgman(this, c_lgman, regFragPtr.p->m_logfile_group_id);
          lgman.free_log_space(leaderOperPtr.p->m_undo_buffer_space,
                               jamBuffer());
        }
        return ZDISK_PAGE_READY_FOR_COMMIT;
      }
    } else {
      jam();
      // initial delete
      initial_delete = true;
      ndbassert(leaderOperPtr.p->op_type == ZDELETE);
      memcpy(&req.m_page, tuple_ptr->get_disk_ref_ptr(regTabPtrP),
             sizeof(Local_key));
      ndbassert(tuple_ptr->m_header_bits & Tuple_header::DISK_PART);
    }

    if (retrieve_data_page(signal, req, leaderOperPtr, diskPagePtr,
                           regFragPtr.p) == 0) {
      if (!initial_delete) {
        jam();
      } else {
        jam();
        /* Set bit to indicate the tuple is already deleted */
        acquire_frag_mutex(regFragPtr.p, leaderOperPtr.p->fragPageId);
        Uint32 old_header = tuple_ptr->m_header_bits;
        Uint32 new_header = tuple_ptr->m_header_bits =
            old_header | Tuple_header::DELETE_WAIT;
        updateChecksum(tuple_ptr, regTabPtrP, old_header, new_header);
        release_frag_mutex(regFragPtr.p, leaderOperPtr.p->fragPageId);
      }
      return ZDISK_PAGE_NOT_READY_FOR_COMMIT;
    }
  }

  if (leaderOperPtr.p->op_struct.bit_field.m_wait_log_buffer) {
    jam();
    /**
     * Only last op on tuple needs "real" commit,
     *   hence only this one should have m_wait_log_buffer
     */
    ndbassert(tuple_ptr->m_operation_ptr_i == leaderOperPtr.i);

    if (retrieve_log_page(signal, regFragPtr, leaderOperPtr) == 0) {
      if (!initial_delete) {
        jam();
      } else {
        jam();
        /* Set bit to indicate the tuple is already deleted */
        acquire_frag_mutex(regFragPtr.p, leaderOperPtr.p->fragPageId);
        Uint32 old_header = tuple_ptr->m_header_bits;
        Uint32 new_header = tuple_ptr->m_header_bits =
            old_header | Tuple_header::DELETE_WAIT;
        updateChecksum(tuple_ptr, regTabPtrP, old_header, new_header);
        release_frag_mutex(regFragPtr.p, leaderOperPtr.p->fragPageId);
      }
      return ZDISK_PAGE_NOT_READY_FOR_COMMIT;
    }
  }
  return ZDISK_PAGE_READY_FOR_COMMIT;
}

Uint32 Dbtup::exec_prepare_tup_commit(Uint32 regOperPtrI) {
  OperationrecPtr regOperPtr;
  regOperPtr.i = regOperPtrI;
  ndbrequire(c_operation_pool.getUncheckedPtrRW(regOperPtr));
  Operationrec::CommitState commit_state = regOperPtr.p->m_commit_state;
  prepare_oper_ptr = regOperPtr;
  ndbrequire(Magic::check_ptr(regOperPtr.p));
  switch (commit_state) {
    case Operationrec::Operationrec::CommitNotStarted: {
      jam();
      /**
       * We are the first operation to arrive, we need to start the
       * commit procedure.
       */
      regOperPtr.p->m_commit_state = Operationrec::CommitStartedReceived;
      return ZTUP_NOT_COMMITTED;
    }
    case Operationrec::CommitStartedNotReceived: {
      jam();
      /**
       * We are not the first operation to arrive, the commit is still
       * ongoing, the TUP part of our commit will be handled for us
       * by setting the state to CommitStartedReceived.
       */
      regOperPtr.p->m_commit_state = Operationrec::CommitStartedReceived;
      return ZTUP_WAIT_COMMIT;
    }
    case Operationrec::CommitPerformedNotReceived: {
      jam();
      /**
       * TUP has completed the commit, but it needs the list to stay
       * until it is done with the reporting of the committed operations.
       * Thus we only change the state here and let TUP report back at an
       * appropriate time when it is done.
       */
      regOperPtr.p->m_commit_state = Operationrec::CommitPerformedReceived;
      return ZTUP_WAIT_COMMIT;
    }
    case Operationrec::CommitDoneNotReceived: {
      jam();
      /**
       * The commit has been started and also completed, we simply
       * finalize the commit and be done with it. No need to worry
       * about the linked list, it has already been processed and
       * thus the operations are no longer linked to each other.
       */
      FragrecordPtr fragPtr;
      fragPtr.i = regOperPtr.p->fragmentPtr;
      ptrCheckGuard(fragPtr, cnoOfFragrec, fragrecord);
      finalize_commit(regOperPtr.p, fragPtr.p);
      return ZTUP_COMMITTED;
    }
    default: {
      ndbabort();
    }
  }
  return 0;
}

void Dbtup::set_commit_started(Uint32 leaderOperPtrI) {
  OperationrecPtr loopOperPtr;
  loopOperPtr.i = leaderOperPtrI;
  do {
    ndbrequire(c_operation_pool.getValidPtr(loopOperPtr));
    if (loopOperPtr.p->m_commit_state == Operationrec::CommitNotStarted) {
      loopOperPtr.p->m_commit_state = Operationrec::CommitStartedNotReceived;
    }
    loopOperPtr.i = loopOperPtr.p->prevActiveOp;
  } while (loopOperPtr.i != RNIL);
}

void Dbtup::set_commit_performed(OperationrecPtr firstOperPtr,
                                 Fragrecord *fragPtrP) {
  OperationrecPtr loopOperPtr = firstOperPtr;
  goto first;
  do {
    ndbrequire(c_operation_pool.getValidPtr(loopOperPtr));
  first:
    switch (loopOperPtr.p->m_commit_state) {
      case Operationrec::CommitNotStarted:
      case Operationrec::CommitStartedNotReceived: {
        jamDebug();
        loopOperPtr.p->m_commit_state =
            Operationrec::CommitPerformedNotReceived;
        break;
      }
      case Operationrec::CommitStartedReceived: {
        jamDebug();
        loopOperPtr.p->m_commit_state = Operationrec::CommitPerformedReceived;
        break;
      }
      case Operationrec::CommitDoneReceived: {
        jamDebug();
        /* This operation was the one used to commit DBACC */
        break;
      }
      default: {
        ndbabort();
      }
    }
    loopOperPtr.i = loopOperPtr.p->nextActiveOp;
  } while (loopOperPtr.i != RNIL);
}

#ifdef ERROR_INSERT
#define MAX_COMMITS 0xFFFFFFFF
// #define MAX_COMMITS 1
#else
#define MAX_COMMITS 0xFFFFFFFF
// #define MAX_COMMITS 4
#endif
void Dbtup::continue_report_commit_performed(Signal *signal,
                                             Uint32 firstOperPtrI) {
  FragrecordPtr regFragPtr;
  OperationrecPtr firstOperPtr;
  firstOperPtr.i = firstOperPtrI;
  ndbrequire(c_operation_pool.getValidPtr(firstOperPtr));
  c_lqh->setup_key_pointers(firstOperPtr.p->userpointer, false);
  regFragPtr.i = firstOperPtr.p->fragmentPtr;
  ptrCheckGuard(regFragPtr, cnoOfFragrec, fragrecord);
  report_commit_performed(signal, firstOperPtr, MAX_COMMITS, regFragPtr.p);
}

void Dbtup::send_continue_report_commit_performed(Signal *signal,
                                                  Uint32 nextOp) {
  signal->theData[0] = ZTUP_REPORT_COMMIT_PERFORMED;
  signal->theData[1] = nextOp;
#ifdef ERROR_INSERT
  m_continue_report_commit_counter++;
  if ((m_continue_report_commit_counter % 10) == 0) {
    sendSignal(reference(), GSN_CONTINUEB, signal, 2, JBB);
  } else {
    sendSignal(reference(), GSN_CONTINUEB, signal, 2, JBB);
  }
#else
  sendSignal(reference(), GSN_CONTINUEB, signal, 2, JBB);
#endif
}

void Dbtup::report_commit_performed(Signal *signal,
                                    OperationrecPtr &firstOperPtr,
                                    Uint32 max_commits,
                                    Fragrecord *regFragPtrP) {
  Uint32 nextOp = RNIL;
  Uint32 num_commits = 0;
  OperationrecPtr loopOperPtr = firstOperPtr;
  goto first;
  do {
    loopOperPtr.i = nextOp;
    ndbrequire(c_operation_pool.getValidPtr(loopOperPtr));
  first:
    nextOp = loopOperPtr.p->nextActiveOp;
    loopOperPtr.p->nextActiveOp = RNIL;
    loopOperPtr.p->prevActiveOp = RNIL;
    switch (loopOperPtr.p->m_commit_state) {
      case Operationrec::CommitPerformedNotReceived: {
        jam();
        /**
         * Remove it from list, nothing more needs to be done, the
         * operation will be cleaned up when commit message arrives
         * for this operation.
         * Indicate in commit state that everything is done and it
         * is ok to simply release operation and move on.
         */
        loopOperPtr.p->m_commit_state = Operationrec::CommitDoneNotReceived;
        break;
      }
      case Operationrec::CommitPerformedReceived: {
        /**
         * It is time to perform the actual commit operation in
         * DBACC as well, we perform this by calling
         * tupcommit_conf_callback in DBLQH.
         */
        jam();
        num_commits++;
        finalize_commit(loopOperPtr.p, regFragPtrP);
        c_lqh->tupcommit_conf_callback(signal, loopOperPtr.p->userpointer);
        if (num_commits >= max_commits) {
          jam();
          if (nextOp == RNIL) {
            jam();
            return;
          }
          send_continue_report_commit_performed(signal, nextOp);
          return;
        }
        break;
      }
      default: {
        ndbabort();
      }
    }
  } while (nextOp != RNIL);
}

#ifdef ERROR_INSERT
bool Dbtup::check_delayed_commit(Signal *signal, TupCommitReq *tupCommitReq,
                                 Uint32 leaderOperPtrI) {
  TupCommitReq *sendTupCommitReq = (TupCommitReq *)signal->getDataPtrSend();
  *sendTupCommitReq = *tupCommitReq;
  m_delayed_commit++;
  if (false && m_delayed_commit >= 100) {
    jam();
    m_delayed_commit = 0;
    sendTupCommitReq->opPtr = leaderOperPtrI;
    sendSignalWithDelay(reference(), GSN_TUP_COMMITREQ, signal, 10,
                        TupCommitReq::SignalLength);
    return true;
  } else if (false && ((m_delayed_commit % 4) == 0)) {
    jam();
    sendTupCommitReq->opPtr = leaderOperPtrI;
    sendSignal(reference(), GSN_TUP_COMMITREQ, signal,
               TupCommitReq::SignalLength, JBB);
    return true;
  } else {
    jam();
    return false;
  }
}
#endif

void Dbtup::execTUP_COMMITREQ(Signal *signal) {
  jamEntry();
  OperationrecPtr leaderOperPtr;
  TupCommitReq *const tupCommitReq = (TupCommitReq *)signal->getDataPtr();
  leaderOperPtr.i = tupCommitReq->opPtr;
  ndbrequire(c_operation_pool.getValidPtr(leaderOperPtr));
  prepare_oper_ptr = leaderOperPtr;

  Uint32 hash_value;
  Uint32 gci_hi, gci_lo;
  Uint32 transId1, transId2;
  c_lqh->get_op_info(leaderOperPtr.p->userpointer, &hash_value, &gci_hi,
                     &gci_lo, &transId1, &transId2);
  exec_tup_commit(signal);
}

Uint32 Dbtup::exec_tup_commit(Signal *signal) {
  /**
   * This code is only executed by the first operation to arrive with a
   * commit for this row. The first operation will switch to using the
   * leader operation record (the last record to be prepared with a write
   * operation. This record is found on the row.
   *
   * It is possible to arrive here as well up to two more times for each
   * commit, one after retrieving the disk data page and one after
   * retrieving a log buffer page for disk data. These calls will both
   * be for the leader operation record.
   *
   * After fetching the disk page and the log buffer page we are ready to
   * commit. The commit happens all in the same real-time break. In this
   * real-time break we will remove entries from the TUX index no longer
   * valid, we will update the row with the new information, and finally
   * we will flag and possibly also remove the deleted row.
   *
   * The real commit is the commit point for this operation except in one
   * case. This is when a DELETE happens on a page that requires a fetch
   * from the disk. In this case the commit point happens in the very first
   * operation that arrives and in that real-time break.
   *
   * The only action required to commit the delete is by setting the
   * DELETE_WAIT flag in the tuple header. The TUX index entries are still
   * there as well as the ACC hash index entries and the operation list
   * on the row is still there. But they will all see the DELETE_WAIT flag
   * and determine that the row is deleted.
   */
  FragrecordPtr regFragPtr;
  OperationrecPtr regOperPtr = prepare_oper_ptr;
  OperationrecPtr leaderOperPtr;
  TablerecPtr regTabPtr;
  KeyReqStruct req_struct(this, KRS_COMMIT);
  TransState trans_state;
  Ptr<GlobalPage> diskPagePtr;
  Uint32 no_of_fragrec, no_of_tablerec;

  TupCommitReq tupCommitReq = *(TupCommitReq *)signal->getDataPtr();

  Uint32 hash_value = tupCommitReq.hashValue;
  Uint32 gci_hi = tupCommitReq.gci_hi;
  Uint32 gci_lo = tupCommitReq.gci_lo;
  Uint32 transId1 = tupCommitReq.transId1;
  Uint32 transId2 = tupCommitReq.transId2;

  jamEntry();

  diskPagePtr.i = tupCommitReq.diskpage;
  regFragPtr.i = regOperPtr.p->fragmentPtr;
  no_of_fragrec = cnoOfFragrec;
  no_of_tablerec = cnoOfTablerec;

  req_struct.signal = signal;
  req_struct.hash_value = hash_value;
  req_struct.gci_hi = gci_hi;
  req_struct.gci_lo = gci_lo;

  trans_state = get_trans_state(regOperPtr.p);

  ndbrequire(trans_state == TRANS_STARTED);
  ptrCheckGuard(regFragPtr, no_of_fragrec, fragrecord);

  regTabPtr.i = regFragPtr.p->fragTableId;

  /* Put transid in req_struct, so detached triggers can access it */
  req_struct.trans_id1 = transId1;
  req_struct.trans_id2 = transId2;
  req_struct.m_reorg = regOperPtr.p->op_struct.bit_field.m_reorg;
  regOperPtr.p->m_commit_disk_callback_page = tupCommitReq.diskpage;

  ptrCheckGuard(regTabPtr, no_of_tablerec, tablerec);
  PagePtr tupPagePtr;
  Tuple_header *tuple_ptr = (Tuple_header *)get_ptr(
      &tupPagePtr, &regOperPtr.p->m_tuple_location, regTabPtr.p);

  req_struct.m_tuple_ptr = tuple_ptr;
  Tup_fixsize_page *fix_page = (Tup_fixsize_page *)tupPagePtr.p;
  fix_page->prefetch_change_map();
  NDB_PREFETCH_WRITE(tuple_ptr);

  if (diskPagePtr.i == RNIL) {
    jam();
    diskPagePtr.p = 0;
    req_struct.m_disk_page_ptr.i = RNIL;
    req_struct.m_disk_page_ptr.p = 0;
  } else {
    jamDebug();
    ndbrequire(m_global_page_pool.getPtr(diskPagePtr, diskPagePtr.i));
  }

  ptrCheckGuard(regTabPtr, no_of_tablerec, tablerec);

  prepare_fragptr = regFragPtr;
  prepare_tabptr = regTabPtr;

  leaderOperPtr.i = tuple_ptr->m_operation_ptr_i;
  ndbrequire(c_operation_pool.getValidPtr(leaderOperPtr));
  if (leaderOperPtr.p->op_struct.bit_field.m_load_diskpage_on_commit ||
      leaderOperPtr.p->op_struct.bit_field.m_wait_log_buffer) {
    /**
     * We need to fetch disk pages before we are allowed to commit this
     * operation.
     */
    jam();
    if (prepare_disk_page_for_commit(signal, leaderOperPtr, tuple_ptr,
                                     diskPagePtr) !=
        ZDISK_PAGE_READY_FOR_COMMIT) {
      jam();
      /**
       * We need to flag all operation records not yet started that
       * the commit is ongoing to ensure that no other operation arrives
       * and also starts the commit.
       */
      set_commit_started(leaderOperPtr.i);
      return ZTUP_WAIT_COMMIT;
    }
  }
#ifdef ERROR_INSERT
  /**
   * In case we perform a real-time break here we need to ensure that
   * the diskpage we retrieved is properly setup for signal sending.
   */
  if (c_started) {
    jam();
    tupCommitReq.diskpage = diskPagePtr.i;
    if (check_delayed_commit(signal, &tupCommitReq, leaderOperPtr.i)) {
      jam();
      set_commit_started(leaderOperPtr.i);
      return ZTUP_WAIT_COMMIT;
    }
  }
#endif
  prepare_oper_ptr = leaderOperPtr;
  execute_real_commit(signal, req_struct, tupPagePtr, diskPagePtr);
  return ZTUP_WAIT_COMMIT;
}

void Dbtup::get_execute_commit_operation(OperationrecPtr &executeOperPtr) {
  while ((executeOperPtr.p->m_commit_state == Operationrec::CommitNotStarted) ||
         (executeOperPtr.p->m_commit_state ==
          Operationrec::CommitStartedNotReceived)) {
    executeOperPtr.i = executeOperPtr.p->nextActiveOp;
    /**
     * At least one operation should have the state CommitStartedReceived
     * and this would normally be the operation that started the commit.
     * But there could be more than one as well.
     */
    ndbrequire(c_operation_pool.getValidPtr(executeOperPtr));
  }
  ndbrequire(executeOperPtr.p->m_commit_state ==
             Operationrec::CommitStartedReceived);
}

void Dbtup::execute_real_commit(Signal *signal, KeyReqStruct &req_struct,
                                PagePtr tupPagePtr,
                                Ptr<GlobalPage> diskPagePtr) {
  OperationrecPtr leaderOperPtr = prepare_oper_ptr;
  OperationrecPtr firstOperPtr;
  OperationrecPtr executeOperPtr;
  Tablerec *regTabPtrP = prepare_tabptr.p;
  Fragrecord *regFragPtrP = prepare_fragptr.p;
  firstOperPtr = leaderOperPtr;
  if (unlikely(!leaderOperPtr.p->is_first_operation())) {
    findFirstOp(firstOperPtr);
  }
  executeOperPtr = firstOperPtr;
  get_execute_commit_operation(executeOperPtr);
  Tuple_header *tuple_ptr = req_struct.m_tuple_ptr;
  /**
   * We have now prepared everything for the real commit. For the real
   * commit we need to acquire the proper fragment access. The real
   * commit will first remove any no longer used entries from the TUX
   * index. After that it will commit the record in DBTUP which means
   * that the row is updated with its new value and the linked list of
   * operations is removed from the row. Finally it will call
   * tup_commit_conf_callback in DBACC that will ensure that also the
   * index in DBACC is updated and that the lock information is removed
   * for the operation.
   *
   * The first operation that touches DBACC must be called with protection
   * since this sets the flags indicating that a row is deleted. However
   * any further calls are simple cleanups not visible to any other thread.
   *
   * This is a very central point of the transaction handling.
   * An operation on a row is committed at this very point in time. This
   * means that queries that use READ COMMITTED will see the old value
   * before the call to acquire commit access to the fragment and will see
   * the new value after release of the fragment access.
   *
   * READ COMMITTED operation can thus see different views for different
   * records, but each record is a committed operation and is always the
   * last committed version.
   *
   * To achieve serialisable model it is necessary to use locks on rows
   * read and retain those locks until commit time. To achieve a serialisable
   * model for range scans isn't possible since we cannot lock ranges. However
   * apart from that the range scans can use locking as well to get all rows
   * within a range, so the only problem is that new rows can appear in this
   * range from other transactions.
   *
   * Solving the range lock issue is a very hard problem in a distributed
   * system and is thus best left for the application to solve. Many databases
   * employ table locks as a method to achieve range locks and this can be
   * achieved from the application level to achieve full serialisability of
   * the operations in NDB.
   *
   * During execution of executeTuxCommitTriggers we need to ensure that
   * range scans are not running concurrently. It is ok for key reads to
   * execute concurrrently with executeTuxCommitTriggers since it only
   * affects the range scans.
   *
   * We implement this by starting to acquire write key access to the
   * fragment, this stops range scans from being able to access the
   * fragment. As part of this we will stop new read keys from starting,
   * but will allow ongoing read keys to continue concurrently with
   * executeTuxCommitTriggers, this should give a good chance that we
   * are already in exclusive mode when we completed the execution of
   * executeTuxCommitTriggers.
   *
   * Thus before the real commit where the new changed row gets visible to
   * other transactions we ensure that we execute in exclusive mode on
   * the fragment. This is also good for the future changes that we are
   * completely aware of the concurrency limitations we have.
   *
   * To avoid concurrent change of TUX indexes and scans of the TUX index
   * is that this would require lock/unlock of a mutex for each row. Given
   * that scanning a row is done in about 250 ns, this means that the overhead
   * of lock/unlock is likely on the order of 30-40%. This is quite costly,
   * so therefore by locking exclusively and wait for range scans to complete,
   * we ensure that range scans knows that no one is updating the range index
   * while scanning it.
   *
   * The exclusive lock approach instead limits the amount of scanning that
   * can happen when there is a huge amount of updates. This can be solved
   * by splitting the table into more fragments. Splitting a table into
   * smaller fragments also splits the locks used to protect the fragments.
   */
  if (!regTabPtrP->tuxCustomTriggers.isEmpty()) {
    c_lqh->acquire_frag_commit_access_write_key();
    if (get_tuple_state(leaderOperPtr.p) == TUPLE_PREPARED) {
      OperationrecPtr loopPtr = firstOperPtr;
      /**
       * Execute all tux triggers at first commit
       *   since previous tuple is otherwise removed...
       */
      jam();
      goto first;
      while (loopPtr.i != RNIL) {
        ndbrequire(c_operation_pool.getValidPtr(loopPtr));
      first:
        executeTuxCommitTriggers(signal, loopPtr.p, regFragPtrP, regTabPtrP);
        set_tuple_state(loopPtr.p, TUPLE_TO_BE_COMMITTED);
        loopPtr.i = loopPtr.p->nextActiveOp;
      }
    }
  }
  req_struct.m_tuple_ptr = tuple_ptr;

  Uint32 nextOp = leaderOperPtr.p->nextActiveOp;
  Uint32 prevOp = leaderOperPtr.p->prevActiveOp;
  /**
   * The trigger code (which is shared between detached/immediate)
   *   check op-list to check were to read before values from
   *   detached triggers should always read from original tuple value
   *   from before transaction start, not from any intermediate update
   *
   * Setting the op-list has this effect
   */
  c_lqh->acquire_frag_commit_access_exclusive();
  leaderOperPtr.p->nextActiveOp = RNIL;
  leaderOperPtr.p->prevActiveOp = RNIL;
  ndbassert(tuple_ptr->m_operation_ptr_i == leaderOperPtr.i);
  {
    jam();
    /**
     * Perform "real" commit
     */
    Uint32 disk = leaderOperPtr.p->m_commit_disk_callback_page;
    set_commit_change_mask_info(regTabPtrP, &req_struct, leaderOperPtr.p);
    checkDetachedTriggers(&req_struct, leaderOperPtr.p, regTabPtrP,
                          disk != RNIL, diskPagePtr.i);

    tuple_ptr->m_operation_ptr_i = RNIL;

    if (leaderOperPtr.p->op_type == ZDELETE) {
      jam();
      dealloc_tuple(signal, req_struct.gci_hi, req_struct.gci_lo, tupPagePtr.p,
                    tuple_ptr, &req_struct, leaderOperPtr.p, regFragPtrP,
                    regTabPtrP, diskPagePtr);
    } else if (leaderOperPtr.p->op_type != ZREFRESH) {
      jam();
      commit_operation(signal, req_struct.gci_hi, req_struct.gci_lo, tuple_ptr,
                       tupPagePtr, leaderOperPtr.p, regFragPtrP, regTabPtrP,
                       diskPagePtr);
    } else {
      jam();
      commit_refresh(signal, req_struct.gci_hi, req_struct.gci_lo, tuple_ptr,
                     tupPagePtr, &req_struct, leaderOperPtr.p, regFragPtrP,
                     regTabPtrP, diskPagePtr);
    }
  }
  /* Restore list before removing execute operation record from it */
  leaderOperPtr.p->nextActiveOp = nextOp;
  leaderOperPtr.p->prevActiveOp = prevOp;

  if (likely(firstOperPtr.i == leaderOperPtr.i)) {
    /* Leader is also first, only one operation, nothing more to do */
    firstOperPtr.i = RNIL;
  } else if (likely(firstOperPtr.i == executeOperPtr.i)) {
    /**
     * Executed record was first record, this case is easy, simply remove
     * the first record and move first to be the second operation record.
     */
    firstOperPtr.i = executeOperPtr.p->nextActiveOp;
    ndbrequire(c_operation_pool.getValidPtr(firstOperPtr));
  } else {
    jam();
    /**
     * Need to link out the operation record from the list before
     * calling tupcommit_conf_callback since this will release the
     * operation record.
     *
     * We only need to update the next list, we will release them from
     * first to last during the commit processing and will not release
     * any operations before this commit processing is done.
     */
    OperationrecPtr prevOperPtr;
    prevOperPtr.i = executeOperPtr.p->prevActiveOp;
    ndbrequire(c_operation_pool.getValidPtr(prevOperPtr));
    prevOperPtr.p->nextActiveOp = executeOperPtr.p->nextActiveOp;
  }
  finalize_commit(executeOperPtr.p, regFragPtrP);
  c_lqh->tupcommit_conf_callback(signal, executeOperPtr.p->userpointer);
  /**
   * We avoid holding the fragment access lock during sendSignal by releasing
   * the lock inside of tupcommit_conf_callback, so no release required here.
   * --------------------------------------------------------
   * The real commit is now completed and we have released the exclusive lock
   * on the fragment. There is still some cleanup work to be performed.
   *
   * At first we need to set the state to CommitPerformedReceived or
   * CommitPerformedNotReceived on all operations. Next we need to start
   * calling tupcommit_conf_callback on all operations that have the state
   * CommitPerformedReceived.
   *
   * At this point we still have the linked list intact. We need to ensure
   * that the list is intact until we have walked through all operations
   * and finalized their commits. This might require a CONTINUEB signal,
   * so this requires some careful processing.
   */
  if (firstOperPtr.i != RNIL) {
    jam();
    set_commit_performed(firstOperPtr, regFragPtrP);
    report_commit_performed(signal, firstOperPtr, MAX_COMMITS, regFragPtrP);
  }
}

void Dbtup::finalize_commit(Operationrec *regOperPtrP, Fragrecord *fragPtrP) {
  if (!regOperPtrP->m_copy_tuple_location.isNull()) {
    jam();
    c_undo_buffer.free_copy_tuple(&regOperPtrP->m_copy_tuple_location);
  }
  fragPtrP->m_committed_changes++;
  initOpConnection(regOperPtrP);
}

void Dbtup::set_commit_change_mask_info(const Tablerec *regTabPtr,
                                        KeyReqStruct *req_struct,
                                        const Operationrec *regOperPtr) {
  Uint32 masklen = (regTabPtr->m_no_of_attributes + 31) >> 5;
  if (regOperPtr->m_copy_tuple_location.isNull()) {
    ndbassert(regOperPtr->op_type == ZDELETE);
    req_struct->changeMask.set();
  } else {
    Uint32 *dst = req_struct->changeMask.rep.data;
    Uint32 *rawptr = get_copy_tuple_raw(&regOperPtr->m_copy_tuple_location);
    ChangeMask *maskptr = get_change_mask_ptr(rawptr);
    Uint32 cols = maskptr->m_cols;
    if (cols == regTabPtr->m_no_of_attributes) {
      memcpy(dst, maskptr->m_mask, 4 * masklen);
    } else {
      ndbassert(regTabPtr->m_no_of_attributes > cols);  // no drop column
      memcpy(dst, maskptr->m_mask, 4 * ((cols + 31) >> 5));
      req_struct->changeMask.setRange(cols,
                                      regTabPtr->m_no_of_attributes - cols);
    }
  }
}

void Dbtup::commit_refresh(Signal *signal, Uint32 gci_hi, Uint32 gci_lo,
                           Tuple_header *tuple_ptr, PagePtr pagePtr,
                           KeyReqStruct *req_struct, Operationrec *regOperPtr,
                           Fragrecord *regFragPtr, Tablerec *regTabPtr,
                           Ptr<GlobalPage> diskPagePtr) {
  /* Committing a refresh operation.
   * Refresh of an existing row looks like an update
   * and can commit normally.
   * Refresh of a non-existing row looks like an Insert which
   * is 'undone' at commit time.
   * This is achieved by making special calls to ACC to get
   * it to forget, before deallocating the tuple locally.
   */
  switch (regOperPtr->m_copy_tuple_location.m_file_no) {
    case Operationrec::RF_SINGLE_NOT_EXIST:
    case Operationrec::RF_MULTI_NOT_EXIST:
      break;
    case Operationrec::RF_SINGLE_EXIST:
    case Operationrec::RF_MULTI_EXIST:
      // "Normal" update
      commit_operation(signal, gci_hi, gci_lo, tuple_ptr, pagePtr, regOperPtr,
                       regFragPtr, regTabPtr, diskPagePtr);
      return;

    default:
      ndbabort();
  }

  Local_key key = regOperPtr->m_tuple_location;
  key.m_page_no = pagePtr.p->frag_page_id;

  /**
   * Tell ACC to delete
   */
  c_lqh->accremoverow(signal, regOperPtr->userpointer, &key);
  dealloc_tuple(signal, gci_hi, gci_lo, pagePtr.p, tuple_ptr, req_struct,
                regOperPtr, regFragPtr, regTabPtr, diskPagePtr);
}
