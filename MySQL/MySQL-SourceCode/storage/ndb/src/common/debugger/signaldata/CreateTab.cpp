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

#include <SignalLoggerManager.hpp>
#include <signaldata/CreateTab.hpp>

bool printCREATE_TAB_REQ(FILE *output, const Uint32 *theData, Uint32 len,
                         Uint16) {
  if (len < CreateTabReq::SignalLength) {
    assert(false);
    return false;
  }

  const auto *sig = (const CreateTabReq *)theData;
  fprintf(output, " senderRef: 0x%x", sig->senderRef);
  fprintf(output, " senderData: %u", sig->senderData);
  fprintf(output, " requestType: %u", sig->requestType);
  fprintf(output, "\n");
  fprintf(output, " tableId: %u", sig->tableId);
  fprintf(output, " tableVersion: 0x%x", sig->tableVersion);
  fprintf(output, " gci: %u", sig->gci);
  fprintf(output, "\n");
  return true;
}

bool printCREATE_TAB_CONF(FILE *output, const Uint32 *theData, Uint32 len,
                          Uint16) {
  if (len < CreateTabConf::SignalLength) {
    assert(false);
    return false;
  }

  const auto *sig = (const CreateTabConf *)theData;
  fprintf(output, " senderRef: 0x%x", sig->senderRef);
  fprintf(output, " senderData: %u", sig->senderData);
  fprintf(output, "\n");
  return true;
}

bool printCREATE_TAB_REF(FILE *output, const Uint32 *theData, Uint32 len,
                         Uint16) {
  if (len < CreateTabRef::SignalLength) {
    assert(false);
    return false;
  }

  const auto *sig = (const CreateTabRef *)theData;
  fprintf(output, " senderRef: 0x%x", sig->senderRef);
  fprintf(output, " senderData: %u", sig->senderData);
  fprintf(output, "\n");
  fprintf(output, " errorCode: %u", sig->errorCode);
  fprintf(output, " errorLine: %u", sig->errorLine);
  fprintf(output, " errorKey: %u", sig->errorKey);
  fprintf(output, " errorStatus: %u", sig->errorStatus);
  fprintf(output, "\n");
  return true;
}
