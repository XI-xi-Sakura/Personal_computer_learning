/*
   Copyright (c) 2010, 2025, Oracle and/or its affiliates.

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

#ifndef LOOPBACK_TRANSPORTER_HPP
#define LOOPBACK_TRANSPORTER_HPP

#include "TCP_Transporter.hpp"

/**
 * This implements a connection to self,
 *   by using a socketpair...
 * where theSocket is the receive part, and m_send_socket is the write part
 */
class Loopback_Transporter : public TCP_Transporter {
  friend class TransporterRegistry;

 private:
  // Initialize member variables
  Loopback_Transporter(TransporterRegistry &,
                       const TransporterConfiguration *conf);

  // Disconnect, delete send buffers and receive buffer
  ~Loopback_Transporter() override;

  /**
   * overloads TCP_Transporter::doSend
   */
  bool doSend(bool need_wakeup = true) override;

  /**
   * setup socket pair
   * @overload Transporter::connect_client()
   */
  bool connect_client() override;

  /**
   * @overload TCP_Transporter::disconnectImpl
   */
  void disconnectImpl() override;

  void releaseAfterDisconnect() override;

 protected:
 private:
  /**
   * m_send_socket is used to send
   * theSocket (in base class) is used for receive
   */
  ndb_socket_t m_send_socket;

  /**
   * overloads TCP_Transporter::send_is_possible
   */
  bool send_is_possible(int timeout_millisec) const override;
};

#endif  // Define of TCP_Transporter_H
