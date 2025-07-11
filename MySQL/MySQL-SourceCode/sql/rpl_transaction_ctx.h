/* Copyright (c) 2014, 2025, Oracle and/or its affiliates.

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

#ifndef RPL_TRANSACTION_CTX_H
#define RPL_TRANSACTION_CTX_H

#include "mysql/service_rpl_transaction_ctx.h"  // Transaction_termination_ctx
#include "sql/rpl_gtid.h"                       // rpl_gno

/**
  Server side support to provide a service to plugins to report if
  a given transaction should continue or be aborted.
  Its value is reset on Transaction_ctx::cleanup().
  Its value is set through service service_rpl_transaction_ctx.
*/
class Rpl_transaction_ctx {
 public:
  Rpl_transaction_ctx();
  virtual ~Rpl_transaction_ctx() = default;

  /**
    Set transaction context, that is, notify the server that for
    external reasons the ongoing transaction should continue or be
    aborted.
    See @file include/mysql/service_rpl_transaction_ctx.h

    @param transaction_termination_ctx  Transaction termination context
    @retval 0      success
    @retval !=0    error
  */
  int set_rpl_transaction_ctx(
      Transaction_termination_ctx transaction_termination_ctx);

  /**
    Get transaction outcome decision.
    When both sidno and gno are equal or greater than zero,
    transaction should continue.
    By default sidno and gno are 0, transaction will continue.

    @retval true      Transaction should abort
    @retval false     Transaction should continue
  */
  bool is_transaction_rollback();

  /**
    Get transaction sidno.

    @return sidno sidno value.
  */
  int get_sidno() const;

  /**
    Get transaction gno.

    @return gno   gno value.
  */
  rpl_gno get_gno() const;

  /// @brief Sets transaction sidno.
  /// @param sidno sidno value
  void set_sidno(rpl_sidno sidno);

  /// @brief Transaction GTID components accessor
  /// @returns SIDNO, GTID GNO
  std::pair<rpl_sidno, rpl_gno> get_gtid_components() const;

  /**
   Reset transaction context to default values.
  */
  void cleanup();

 private:
  Transaction_termination_ctx m_transaction_ctx;
};

#endif /* RPL_TRANSACTION_CTX_H */
