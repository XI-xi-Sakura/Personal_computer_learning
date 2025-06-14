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

#include "plugin/group_replication/include/plugin_messages/group_service_message.h"
#include <cstring>
#include "my_dbug.h"
#include "plugin/group_replication/include/plugin_handlers/metrics_handler.h"

Group_service_message::Group_service_message()
    : Plugin_gcs_message(CT_MESSAGE_SERVICE_MESSAGE),
      m_data(Malloc_allocator<uchar>(key_message_service_received_message)),
      m_data_pointer(nullptr),
      m_data_pointer_length(0) {}

Group_service_message::Group_service_message(const uchar *buf, size_t len)
    : Plugin_gcs_message(CT_MESSAGE_SERVICE_MESSAGE),
      m_data(Malloc_allocator<uchar>(key_message_service_received_message)),
      m_data_pointer(nullptr),
      m_data_pointer_length(0) {
  decode(buf, len);
}

Group_service_message::~Group_service_message() = default;

bool Group_service_message::set_data(const uchar *data,
                                     const size_t data_length) {
  DBUG_TRACE;

  m_data.clear();
  m_data_pointer = data;
  m_data_pointer_length = data_length;
  return false;
}

const uchar *Group_service_message::get_data() {
  DBUG_TRACE;

  if (nullptr != m_data_pointer) {
    return m_data_pointer; /* purecov: inspected */
  }
  return m_data.data();
}

size_t Group_service_message::get_data_length() {
  DBUG_TRACE;

  if (nullptr != m_data_pointer) {
    return m_data_pointer_length; /* purecov: inspected */
  }
  return m_data.size();
}

void Group_service_message::encode_payload(std::vector<uchar> *buffer) const {
  DBUG_TRACE;

  encode_payload_item_string(buffer, PIT_TAG, m_tag.c_str(), m_tag.length());

  if (nullptr != m_data_pointer) {
    encode_payload_item_type_and_length(buffer, PIT_DATA,
                                        m_data_pointer_length);
    buffer->insert(buffer->end(), m_data_pointer,
                   m_data_pointer + m_data_pointer_length);
  } else {
    /* purecov: begin inspected */
    encode_payload_item_type_and_length(buffer, PIT_DATA, m_data.size());
    buffer->insert(buffer->end(), m_data.begin(), m_data.end());
    /* purecov: end */
  }

  encode_payload_item_int8(buffer, PIT_SENT_TIMESTAMP,
                           Metrics_handler::get_current_time());
}

void Group_service_message::decode_payload(const uchar *buffer, const uchar *) {
  DBUG_TRACE;
  const unsigned char *slider = buffer;
  uint16 payload_item_type = 0;
  unsigned long long payload_item_length = 0;

  decode_payload_item_string(&slider, &payload_item_type, &m_tag,
                             &payload_item_length);

  decode_payload_item_type_and_length(&slider, &payload_item_type,
                                      &payload_item_length);
  m_data.clear();
  m_data.insert(m_data.end(), slider, slider + payload_item_length);
  m_data_pointer = nullptr;
  m_data_pointer_length = 0;
}

bool Group_service_message::set_tag(const char *tag) {
  DBUG_TRACE;

  if (tag != nullptr && strlen(tag) != 0) {
    m_tag.assign(tag);
    return false;
  }
  return true;
}

uint64_t Group_service_message::get_sent_timestamp(const unsigned char *buffer,
                                                   size_t length) {
  DBUG_TRACE;
  return Plugin_gcs_message::get_sent_timestamp(buffer, length,
                                                PIT_SENT_TIMESTAMP);
}
