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

#ifndef PREP_FAILREQREF_HPP
#define PREP_FAILREQREF_HPP

#include <NodeBitmask.hpp>
#include "SignalData.hpp"

#define JAM_FILE_ID 160

/**
 * The Req signal is sent by Qmgr to Qmgr
 * and the Ref signal might be sent back
 *
 * NOTE that the signals are identical
 */
class PrepFailReqRef {
  /**
   * Sender(s) / Reciver(s)
   */
  friend class Qmgr;

  friend bool printPREPFAILREQREF(FILE *output, const Uint32 *theData,
                                  Uint32 len, Uint16 receiverBlockNo);

 public:
  static constexpr Uint32 SignalLength = 3;
  static constexpr Uint32 SignalLength_v1 = 3 + NdbNodeBitmask48::Size;

 private:
  Uint32 xxxBlockRef;
  Uint32 failNo;

  Uint32 noOfNodes;
  Uint32 theNodes[NdbNodeBitmask48::Size];
};

#undef JAM_FILE_ID

#endif
