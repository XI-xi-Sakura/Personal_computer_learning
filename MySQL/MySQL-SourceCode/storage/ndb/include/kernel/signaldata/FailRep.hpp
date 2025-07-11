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

#ifndef FAIL_REP_HPP
#define FAIL_REP_HPP

#include <NodeBitmask.hpp>
#include "SignalData.hpp"

#define JAM_FILE_ID 24

/**
 *
 */
class FailRep {
  /**
   * Sender(s) & Reciver(s)
   */
  friend class Qmgr;
  friend class Ndbcntr;

  /**
   * For printing
   */
  friend bool printFAIL_REP(FILE *, const Uint32 *, Uint32, Uint16);

 public:
  static constexpr Uint32 OrigSignalLength = 2;
  /**
   * PartitionedExtraLength_v1 can be reduced to 1 by removing
   * the partition_v1 array in later versions when 7.6.9 and 8.0.15 are
   * the lowest supported versions for upgrade compatibility.
   * The two words are used for sending node bitmask which is sent in
   * a signal section for later versions.
   */
  static constexpr Uint32 PartitionedExtraLength_v1 = 1 + 2;
  static constexpr Uint32 SourceExtraLength = 1;
  static constexpr Uint32 SignalLength = OrigSignalLength + SourceExtraLength;

  enum FailCause {
    ZOWN_FAILURE = 0,
    ZOTHER_NODE_WHEN_WE_START = 1,
    ZIN_PREP_FAIL_REQ = 2,
    ZSTART_IN_REGREQ = 3,
    ZHEARTBEAT_FAILURE = 4,
    ZLINK_FAILURE = 5,
    ZOTHERNODE_FAILED_DURING_START = 6,
    ZMULTI_NODE_SHUTDOWN = 7,
    ZPARTITIONED_CLUSTER = 8,
    ZCONNECT_CHECK_FAILURE = 9,
    ZFORCED_ISOLATION = 10
  };

  Uint32 getFailSourceNodeId(Uint32 sigLen) const {
    /* Get failSourceNodeId from signal given length
     * 2 cases of 2 existing cases :
     *   1) Old node, no source id
     *   2) New node, source id
     *   a) ZPARTITIONED_CLUSTER, extra info
     *   b) Other error, no extra info
     */
    if (failCause == ZPARTITIONED_CLUSTER) {
      return (sigLen == (SignalLength + PartitionedExtraLength_v1))
                 ? partitioned.partitionFailSourceNodeId
                 : 0;
    }

    return (sigLen == SignalLength) ? failSourceNodeId : 0;
  }

 private:
  Uint32 failNodeId;
  Uint32 failCause;
  /**
   * Used when failCause == ZPARTITIONED_CLUSTER
   */
  union {
    struct {
      Uint32 president;
      Uint32 partition_v1[2];
      Uint32 partitionFailSourceNodeId;
    } partitioned;
    Uint32 failSourceNodeId;
  };
};

#undef JAM_FILE_ID

#endif
