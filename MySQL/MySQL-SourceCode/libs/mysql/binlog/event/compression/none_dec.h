/* Copyright (c) 2019, 2025, Oracle and/or its affiliates.

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

#ifndef MYSQL_BINLOG_EVENT_COMPRESSION_NONE_DEC_H
#define MYSQL_BINLOG_EVENT_COMPRESSION_NONE_DEC_H

#include "mysql/binlog/event/compression/decompressor.h"
#include "mysql/utils/nodiscard.h"

namespace mysql::binlog::event::compression {

/// Decompressor subclass that only copies input to output without
/// decompressing it.
class None_dec : public Decompressor {
 public:
  using typename Decompressor::Char_t;
  using typename Decompressor::Size_t;
  static constexpr type type_code = NONE;

  None_dec() = default;
  ~None_dec() override = default;

  None_dec(const None_dec &) = delete;
  None_dec(const None_dec &&) = delete;
  None_dec &operator=(const None_dec &) = delete;
  None_dec &operator=(const None_dec &&) = delete;

 private:
  /// @return NONE
  type do_get_type_code() const override;

  /// @copydoc Decompressor::do_reset
  void do_reset() override;

  /// @copydoc Decompressor::do_feed
  void do_feed(const Char_t *input_data, Size_t input_size) override;

  /// @copydoc Decompressor::do_decompress
  [[NODISCARD]] std::pair<Decompress_status, Size_t> do_decompress(
      Char_t *out, Size_t output_size) override;

  /// Input data
  const Char_t *m_input_data;
  Size_t m_input_size;
  Size_t m_input_position;
};

}  // namespace mysql::binlog::event::compression

#endif  // MYSQL_BINLOG_EVENT_COMPRESSION_NONE_DEC_H
