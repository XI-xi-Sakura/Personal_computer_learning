/*
   Copyright (c) 2014, 2025, Oracle and/or its affiliates.

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

#ifndef INCLUDE_NODE_HB_PROTOCOL_HPP
#define INCLUDE_NODE_HB_PROTOCOL_HPP

#include "SignalData.hpp"

/**
 * Request to allocate node id
 */
class InclNodeHBProtocolRep {
 public:
  static constexpr Uint32 SignalLength = 1;

  Uint32 nodeId;
};

class NdbcntrStartWaitRep {
 public:
  static constexpr Uint32 SignalLength = 1;

  Uint32 nodeId;
};

class NdbcntrStartedRep {
 public:
  static constexpr Uint32 SignalLength = 1;

  Uint32 nodeId;
};

class SumaHandoverCompleteRep {
 public:
  static constexpr Uint32 SignalLength = 1;

  Uint32 nodeId;
};

class LocalRecoveryCompleteRep {
 public:
  static constexpr Uint32 SignalLengthLocal = 4;
  static constexpr Uint32 SignalLengthMaster = 2;

  enum PhaseIds {
    RESTORE_FRAG_COMPLETED = 0,
    UNDO_DD_COMPLETED = 1,
    EXECUTE_REDO_LOG_COMPLETED = 2,
    LOCAL_RECOVERY_COMPLETED = 3
  };

  Uint32 nodeId;
  Uint32 phaseId;
  Uint32 senderData;
  Uint32 instanceId;
};

#undef JAM_FILE_ID
#endif
