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

#ifndef DROP_TAB_HPP
#define DROP_TAB_HPP

#include "SignalData.hpp"

#define JAM_FILE_ID 138

struct DropTabReq {
  static constexpr Uint32 SignalLength = 5;

  enum RequestType { OnlineDropTab = 0, CreateTabDrop = 1, RestartDropTab = 2 };

  Uint32 senderRef;
  Uint32 senderData;
  Uint32 requestType;
  Uint32 tableId;
  Uint32 tableVersion;
};

struct DropTabConf {
  static constexpr Uint32 SignalLength = 3;

  Uint32 senderRef;
  Uint32 senderData;
  Uint32 tableId;
};

struct DropTabRef {
  static constexpr Uint32 SignalLength = 4;

  enum ErrorCode {
    NoSuchTable = 1,
    DropWoPrep = 2,  // Calling Drop with first calling PrepDrop
    PrepDropInProgress = 3,
    DropInProgress = 4,
    NF_FakeErrorREF = 5,
    InvalidTableState = 6
  };

  Uint32 senderRef;
  Uint32 senderData;
  Uint32 tableId;
  Uint32 errorCode;
};

#undef JAM_FILE_ID

#endif
