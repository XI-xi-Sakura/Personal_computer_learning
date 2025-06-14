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

#ifndef CREATE_INDX_IMPL_HPP
#define CREATE_INDX_IMPL_HPP

#include "SignalData.hpp"

#define JAM_FILE_ID 33

struct CreateIndxImplReq {
  static constexpr Uint32 SignalLength = 8;
  SECTION(ATTRIBUTE_LIST_SECTION = 0);
  SECTION(INDEX_NAME_SECTION = 1);

  Uint32 senderRef;
  Uint32 senderData;
  Uint32 requestType;
  Uint32 tableId;
  Uint32 tableVersion;
  Uint32 indexType;
  Uint32 indexId;
  Uint32 indexVersion;
};

struct CreateIndxImplConf {
  static constexpr Uint32 SignalLength = 2;

  Uint32 senderRef;
  Uint32 senderData;
};

struct CreateIndxImplRef {
  static constexpr Uint32 SignalLength = 6;

  enum ErrorCode {
    NoError = 0,
    Busy = 701,
    BusyWithNR = 711,
    NotMaster = 702,
    IndexOnDiskAttributeError = 756,
    TriggerNotFound = 4238,
    TriggerExists = 4239,
    IndexNameTooLong = 4241,
    TooManyIndexes = 4242,
    IndexExists = 4244,
    AttributeNullable = 4246,
    BadRequestType = 4247,
    InvalidName = 4248,
    InvalidPrimaryTable = 4249,
    InvalidIndexType = 4250,
    NotUnique = 4251,
    AllocationError = 4252,
    CreateIndexTableFailed = 4253,
    DuplicateAttributes = 4258,
    TableIsTemporary = 776,
    TableIsNotTemporary = 777,
    NoLoggingTemporaryIndex = 778,
    InconsistentTC = 292
  };

  Uint32 senderRef;
  Uint32 senderData;
  Uint32 errorCode;
  Uint32 errorLine;
  Uint32 errorNodeId;
  Uint32 masterNodeId;
};

#undef JAM_FILE_ID

#endif
