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

#include <RefConvert.hpp>
#include <signaldata/StartFragReq.hpp>
#include <signaldata/StartRec.hpp>

bool printSTART_REC_REQ(FILE *output, const Uint32 *theData, Uint32 len,
                        Uint16 /*recBlockNo*/) {
  const auto *sig = (const StartRecReq *)theData;

  if (len != StartRecReq::SignalLength) return false;

  fprintf(output, " receivingNodeId: %d senderRef: (%d, %d)\n",
          sig->receivingNodeId, refToNode(sig->senderRef),
          refToBlock(sig->senderRef));

  fprintf(output,
          " keepGci: %d lastCompletedGci: %d newestGci: %d senderData: %x\n",
          sig->keepGci, sig->lastCompletedGci, sig->newestGci, sig->senderData);

  if (len == sig->SignalLength_v1) {
    NdbNodeBitmask48 mask;
    mask.assign(NdbNodeBitmask48::Size, sig->sr_nodes);

    char buf[NdbNodeBitmask48::TextLength + 1];
    fprintf(output, " sr_nodes: %s\n", mask.getText(buf));
  } else {
    fprintf(output, "sr_nodes in signal section\n");
  }
  return true;
}

bool printSTART_REC_CONF(FILE *output, const Uint32 *theData, Uint32 len,
                         Uint16 /*recBlockNo*/) {
  const auto *sig = (const StartRecConf *)theData;

  if (len != StartRecConf::SignalLength) return false;

  fprintf(output, " startingNodeId: %d senderData: %u\n", sig->startingNodeId,
          sig->senderData);

  return true;
}

bool printSTART_FRAG_REQ(FILE *output, const Uint32 *theData, Uint32 len,
                         Uint16 /*recBlockNo*/) {
  const auto *sig = (const StartFragReq *)theData;

  fprintf(output,
          " table: %d frag: %d lcpId: %d lcpNo: %d #nodes: %d"
          ", reqinfo: %x \n",
          sig->tableId, sig->fragId, sig->lcpId, sig->lcpNo, sig->noOfLogNodes,
          sig->requestInfo);

  for (Uint32 i = 0; i < sig->noOfLogNodes; i++) {
    fprintf(output, " (node: %d startGci: %d lastGci: %d)", sig->lqhLogNode[i],
            sig->startGci[i], sig->lastGci[i]);
  }
  if (len == StartFragReq::SignalLength) {
    fprintf(output, "\nnodeRestorableGci: %u", sig->nodeRestorableGci);
  } else {
    fprintf(output, "\nnodeRestorableGci: 0 (from older version)");
  }
  fprintf(output, "\n");
  return true;
}
