/* Copyright (c) 2007, 2025, Oracle and/or its affiliates.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef ALTER_INDX_IMPL_HPP
#define ALTER_INDX_IMPL_HPP

#include "SignalData.hpp"

#define JAM_FILE_ID 209

struct AlterIndxImplReq {
  enum RequestType {
    AlterIndexOnline = 1,
    AlterIndexOffline = 2,
    AlterIndexAddPartition = 3,
    AlterIndexBuilding = 4
  };

  static constexpr Uint32 SignalLength = 8;

  Uint32 senderRef;
  Uint32 senderData;
  Uint32 requestType;
  Uint32 tableId;
  Uint32 tableVersion;
  Uint32 indexId;
  Uint32 indexVersion;
  Uint32 indexType;
};

struct AlterIndxImplConf {
  static constexpr Uint32 SignalLength = 2;

  Uint32 senderRef;
  Uint32 senderData;
};

struct AlterIndxImplRef {
  enum ErrorCode {
    NoError = 0,
    InvalidIndexVersion = 241,
    Busy = 701,
    BusyWithNR = 711,
    NotMaster = 702,
    IndexNotFound = 4243,
    BadRequestType = 4247,
    InvalidName = 4248,
    NotAnIndex = 4254,
    InconsistentTC = 292
  };

  static constexpr Uint32 SignalLength = 6;

  Uint32 senderRef;
  Uint32 senderData;
  Uint32 errorCode;
  Uint32 errorLine;
  Uint32 errorNodeId;
  Uint32 masterNodeId;
};

#undef JAM_FILE_ID

#endif
