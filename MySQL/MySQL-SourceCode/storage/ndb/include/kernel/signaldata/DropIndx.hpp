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

#ifndef DROP_INDX_HPP
#define DROP_INDX_HPP

#include <NodeBitmask.hpp>
#include "SignalData.hpp"

#define JAM_FILE_ID 147

struct DropIndxReq {
  static constexpr Uint32 SignalLength = 7;

  Uint32 clientRef;
  Uint32 clientData;
  Uint32 transId;
  Uint32 transKey;
  Uint32 requestInfo;
  Uint32 indexId;
  Uint32 indexVersion;
};

struct DropIndxConf {
  static constexpr Uint32 SignalLength = 5;

  Uint32 senderRef;
  Uint32 clientData;
  Uint32 transId;
  Uint32 indexId;
  Uint32 indexVersion;
};

struct DropIndxRef {
  static constexpr Uint32 SignalLength = 9;

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
    SingleUser = 299
  };

  Uint32 senderRef;
  Uint32 clientData;
  Uint32 transId;
  Uint32 indexId;
  Uint32 indexVersion;
  Uint32 errorCode;
  Uint32 errorLine;
  Uint32 errorNodeId;
  Uint32 masterNodeId;
};

#undef JAM_FILE_ID

#endif
