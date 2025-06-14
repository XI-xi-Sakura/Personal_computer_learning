/* Copyright (c) 2021, 2025, Oracle and/or its affiliates.

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

#include "libchangestreams/src/lib/mysql/cs/codec/pb/reader_state_codec_pb.h"
#include "libchangestreams/include/mysql/cs/reader/state.h"
#include "reader_state.pb.h"

namespace cs::reader::codec::pb::example {

void read_from_stream(std::istream &stream, cs::reader::State &out) {
  mysql::gtid::Gtid_set gtid_set;
  cs::reader::codec::pb::example::State state_codec;
  std::string sibuf;

  while (stream.good()) {
    char ibuf[1024];
    stream.read(ibuf, 1024);
    sibuf.append(ibuf, stream.gcount());
  }

  // real failure
  if (stream.fail() && !stream.eof()) return;

  if (!state_codec.ParseFromString(sibuf)) {
    /* purecov: begin inspected */
    stream.setstate(std::ios_base::failbit);
    return;
    /* purecov: end */
  }

  // NOTE: does not build with LITE_RUNTIME in some platforms
  // if (!state_codec.ParseFromIstream(&stream)) {
  //   stream.setstate(std::ios_base::failbit);
  //   return;
  // }

  for (const auto &gtids : state_codec.gtids()) {
    std::string suuid = gtids.uuid();
    mysql::gtid::Tag tag(gtids.tag());
    for (const auto &range : gtids.range()) {
      mysql::gtid::Gno_interval interval{range.start(), range.end()};
      mysql::gtid::Uuid uuid;
      if (uuid.parse(suuid.c_str(), suuid.length())) {
        /* purecov: begin inspected */
        stream.setstate(std::ios_base::failbit);
        return;
        /* purecov: end */
      }
      if (gtid_set.add(mysql::gtid::Tsid(uuid, tag), interval)) {
        /* purecov: begin inspected */
        stream.setstate(std::ios_base::failbit);
        return;
        /* purecov: end */
      }
    }
  }
  out.set_gtids(gtid_set);
}

void write_to_stream(std::ostream &stream, cs::reader::State &in) {
  auto &gtid_set = in.get_gtids();
  const auto &contents = gtid_set.get_gtid_set();
  cs::reader::codec::pb::example::State state_codec;

  for (auto const &[uuid, tag_map] : contents) {
    for (auto const &[tag, intervals] : tag_map) {
      auto *ranges = state_codec.add_gtids();
      ranges->set_uuid(uuid.to_string());
      if (tag.is_defined()) {
        ranges->set_tag(tag.to_string());
      }
      for (auto &interval : intervals) {
        auto range = ranges->add_range();
        range->set_end(interval.get_end());
        range->set_start(interval.get_start());
      }
    }
  }

  std::string obuffer;
  if (!state_codec.SerializeToString(&obuffer)) {
    /* purecov: begin inspected */
    stream.setstate(std::ios_base::failbit);
    return;
    /* purecov: end */
  }
  stream << obuffer;

  // NOTE: does not build with LITE_RUNTIME in some platforms
  // if (!state_codec.SerializeToOstream(&stream)) {
  //   stream.setstate(std::ios_base::failbit);
  // }
}

stringstream &stringstream::operator>>(cs::reader::State &to_decode_into) {
  cs::reader::codec::pb::example::read_from_stream(*this, to_decode_into);
  return (*this);
}

stringstream &stringstream::operator<<(cs::reader::State &to_encode_from) {
  cs::reader::codec::pb::example::write_to_stream(*this, to_encode_from);
  return (*this);
}

}  // namespace cs::reader::codec::pb::example
