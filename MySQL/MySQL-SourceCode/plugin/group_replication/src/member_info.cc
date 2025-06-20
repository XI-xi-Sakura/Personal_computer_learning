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

#include "plugin/group_replication/include/member_info.h"

#include <cstddef>

#include "mutex_lock.h"
#include "my_byteorder.h"
#include "my_dbug.h"
#include "my_systime.h"

#include "plugin/group_replication/include/plugin_constants.h"
#include "plugin/group_replication/include/plugin_handlers/metrics_handler.h"

using std::map;
using std::string;
using std::vector;

Group_member_info::Group_member_info(PSI_mutex_key psi_mutex_key_arg)
    : Plugin_gcs_message(CT_MEMBER_INFO_MESSAGE),
      port(0),
      status(MEMBER_OFFLINE),
      write_set_extraction_algorithm(0),
      gtid_assignment_block_size(0),
      unreachable(false),
      role(MEMBER_ROLE_SECONDARY),
      configuration_flags(0),
      conflict_detection_enable(true),
      member_weight(0),
      lower_case_table_names(false),
      default_table_encryption(false),
      group_action_running(false),
      primary_election_running(false),
      recovery_endpoints("DEFAULT"),
      m_view_change_uuid("AUTOMATIC"),
      m_allow_single_leader(false),
      m_preemptive_garbage_collection(PREEMPTIVE_GARBAGE_COLLECTION_DEFAULT),
      m_component_primary_election_enabled(false),
#ifndef NDEBUG
      skip_encode_default_table_encryption(false),
      m_skip_encode_view_change_uuid(false),
#endif
      psi_mutex_key(psi_mutex_key_arg) {
  mysql_mutex_init(psi_mutex_key, &update_lock, MY_MUTEX_INIT_FAST);
}

Group_member_info::Group_member_info(
    const char *hostname_arg, uint port_arg, const char *uuid_arg,
    int write_set_extraction_algorithm_arg,
    const std::string &gcs_member_id_arg,
    Group_member_info::Group_member_status status_arg,
    Member_version &member_version_arg,
    ulonglong gtid_assignment_block_size_arg,
    Group_member_info::Group_member_role role_arg, bool in_single_primary_mode,
    bool has_enforces_update_everywhere_checks, uint member_weight_arg,
    uint lower_case_table_names_arg, bool default_table_encryption_arg,
    const char *recovery_endpoints_arg, const char *view_change_uuid_arg,
    bool allow_single_leader, bool preemptive_garbage_collection,
    bool component_primary_election_enabled, PSI_mutex_key psi_mutex_key_arg)
    : Plugin_gcs_message(CT_MEMBER_INFO_MESSAGE),
      hostname(hostname_arg),
      port(port_arg),
      uuid(uuid_arg),
      status(status_arg),
      write_set_extraction_algorithm(write_set_extraction_algorithm_arg),
      gtid_assignment_block_size(gtid_assignment_block_size_arg),
      unreachable(false),
      role(role_arg),
      configuration_flags(0),
      conflict_detection_enable(!in_single_primary_mode),
      member_weight(member_weight_arg),
      lower_case_table_names(lower_case_table_names_arg),
      default_table_encryption(default_table_encryption_arg),
      group_action_running(false),
      primary_election_running(false),
      recovery_endpoints(recovery_endpoints_arg ? recovery_endpoints_arg
                                                : "DEFAULT"),
      m_view_change_uuid(view_change_uuid_arg ? view_change_uuid_arg
                                              : "AUTOMATIC"),
      m_allow_single_leader(allow_single_leader),
      m_preemptive_garbage_collection(preemptive_garbage_collection),
      m_component_primary_election_enabled(component_primary_election_enabled),
#ifndef NDEBUG
      skip_encode_default_table_encryption(false),
      m_skip_encode_view_change_uuid(false),
#endif
      psi_mutex_key(psi_mutex_key_arg) {
  mysql_mutex_init(psi_mutex_key, &update_lock, MY_MUTEX_INIT_FAST);
  gcs_member_id = new Gcs_member_identifier(gcs_member_id_arg);
  member_version = new Member_version(member_version_arg.get_version());

  /* Handle single_primary_mode */
  if (in_single_primary_mode) configuration_flags |= CNF_SINGLE_PRIMARY_MODE_F;

  /* Handle enforce_update_everywhere_checks */
  if (has_enforces_update_everywhere_checks)
    configuration_flags |= CNF_ENFORCE_UPDATE_EVERYWHERE_CHECKS_F;
}

Group_member_info::Group_member_info(Group_member_info &other)
    : Plugin_gcs_message(CT_MEMBER_INFO_MESSAGE),
      hostname(other.get_hostname()),
      port(other.get_port()),
      uuid(other.get_uuid()),
      status(other.get_recovery_status()),
      executed_gtid_set(other.get_gtid_executed()),
      purged_gtid_set(other.get_gtid_purged()),
      retrieved_gtid_set(other.get_gtid_retrieved()),
      write_set_extraction_algorithm(
          other.get_write_set_extraction_algorithm()),
      gtid_assignment_block_size(other.get_gtid_assignment_block_size()),
      unreachable(other.is_unreachable()),
      role(other.get_role()),
      configuration_flags(other.get_configuration_flags()),
      conflict_detection_enable(other.is_conflict_detection_enabled()),
      member_weight(other.get_member_weight()),
      lower_case_table_names(other.get_lower_case_table_names()),
      default_table_encryption(other.get_default_table_encryption()),
      group_action_running(other.is_group_action_running()),
      primary_election_running(other.is_primary_election_running()),
      recovery_endpoints(other.get_recovery_endpoints()),
      m_view_change_uuid(other.get_view_change_uuid()),
      m_allow_single_leader(other.get_allow_single_leader()),
      m_group_action_running_name(other.get_group_action_running_name()),
      m_group_action_running_description(
          other.get_group_action_running_description()),
      m_preemptive_garbage_collection(
          other.get_preemptive_garbage_collection()),
      m_component_primary_election_enabled(
          other.get_component_primary_election_enabled()),
#ifndef NDEBUG
      skip_encode_default_table_encryption(false),
      m_skip_encode_view_change_uuid(false),
#endif
      psi_mutex_key(other.psi_mutex_key) {
  mysql_mutex_init(psi_mutex_key, &update_lock, MY_MUTEX_INIT_FAST);
  gcs_member_id =
      new Gcs_member_identifier(other.get_gcs_member_id().get_member_id());
  member_version = new Member_version(other.get_member_version().get_version());
}

Group_member_info::Group_member_info(const uchar *data, size_t len,
                                     PSI_mutex_key psi_mutex_key_arg)
    : Plugin_gcs_message(CT_MEMBER_INFO_MESSAGE),
      gcs_member_id(nullptr),
      member_version(nullptr),
      unreachable(false),
      lower_case_table_names(DEFAULT_NOT_RECEIVED_LOWER_CASE_TABLE_NAMES),
      default_table_encryption(false),
      group_action_running(false),
      primary_election_running(false),
      recovery_endpoints("DEFAULT"),
      m_view_change_uuid("AUTOMATIC"),
      m_allow_single_leader(false),
      m_preemptive_garbage_collection(PREEMPTIVE_GARBAGE_COLLECTION_DEFAULT),
      m_component_primary_election_enabled(false),
#ifndef NDEBUG
      skip_encode_default_table_encryption(false),
      m_skip_encode_view_change_uuid(false),
#endif
      psi_mutex_key(psi_mutex_key_arg) {
  mysql_mutex_init(psi_mutex_key, &update_lock, MY_MUTEX_INIT_FAST);
  decode(data, len);
}

Group_member_info::~Group_member_info() {
  mysql_mutex_destroy(&update_lock);
  delete gcs_member_id;
  delete member_version;
}

void Group_member_info::update(
    const char *hostname_arg, uint port_arg, const char *uuid_arg,
    int write_set_extraction_algorithm_arg,
    const std::string &gcs_member_id_arg,
    Group_member_info::Group_member_status status_arg,
    Member_version &member_version_arg,
    ulonglong gtid_assignment_block_size_arg,
    Group_member_info::Group_member_role role_arg, bool in_single_primary_mode,
    bool has_enforces_update_everywhere_checks, uint member_weight_arg,
    uint lower_case_table_names_arg, bool default_table_encryption_arg,
    const char *recovery_endpoints_arg, const char *view_change_uuid_arg,
    bool allow_single_leader, bool preemptive_garbage_collection,
    bool component_primary_election_enabled) {
  MUTEX_LOCK(lock, &update_lock);

  hostname.assign(hostname_arg);
  port = port_arg;
  uuid.assign(uuid_arg);
  status = status_arg;
  write_set_extraction_algorithm = write_set_extraction_algorithm_arg;
  gtid_assignment_block_size = gtid_assignment_block_size_arg;
  unreachable = false;
  role = role_arg;
  conflict_detection_enable = !in_single_primary_mode;
  member_weight = member_weight_arg;
  lower_case_table_names = lower_case_table_names_arg;
  default_table_encryption = default_table_encryption_arg;
  group_action_running = false;
  primary_election_running = false;

  executed_gtid_set.clear();
  purged_gtid_set.clear();
  retrieved_gtid_set.clear();

  delete gcs_member_id;
  gcs_member_id = new Gcs_member_identifier(gcs_member_id_arg);
  delete member_version;
  member_version = new Member_version(member_version_arg.get_version());

  configuration_flags = 0;
  /* Handle single_primary_mode */
  if (in_single_primary_mode) configuration_flags |= CNF_SINGLE_PRIMARY_MODE_F;
  /* Handle enforce_update_everywhere_checks */
  if (has_enforces_update_everywhere_checks)
    configuration_flags |= CNF_ENFORCE_UPDATE_EVERYWHERE_CHECKS_F;

  recovery_endpoints.assign(recovery_endpoints_arg);

  m_view_change_uuid.assign(view_change_uuid_arg);
  m_allow_single_leader = allow_single_leader;
  m_preemptive_garbage_collection = preemptive_garbage_collection;
  m_component_primary_election_enabled = component_primary_election_enabled;
}

void Group_member_info::update(Group_member_info &other) {
  MUTEX_LOCK(lock, &update_lock);
  hostname.assign(other.get_hostname());
  port = other.get_port();
  uuid.assign(other.get_uuid());
  status = other.get_recovery_status();
  delete gcs_member_id;
  gcs_member_id =
      new Gcs_member_identifier(other.get_gcs_member_id().get_member_id());
  delete member_version;
  member_version = new Member_version(other.get_member_version().get_version());
  executed_gtid_set.assign(other.get_gtid_executed());
  purged_gtid_set.assign(other.get_gtid_purged());
  retrieved_gtid_set.assign(other.get_gtid_retrieved());
  write_set_extraction_algorithm = other.get_write_set_extraction_algorithm();
  gtid_assignment_block_size = other.get_gtid_assignment_block_size();
  unreachable = other.is_unreachable();
  role = other.get_role();
  configuration_flags = other.get_configuration_flags();
  conflict_detection_enable = other.is_conflict_detection_enabled();
  member_weight = other.get_member_weight();
  lower_case_table_names = other.get_lower_case_table_names();
  default_table_encryption = other.get_default_table_encryption();
  group_action_running = other.is_group_action_running();
  primary_election_running = other.is_primary_election_running();
  recovery_endpoints.assign(other.get_recovery_endpoints());
  m_view_change_uuid.assign(other.get_view_change_uuid());
  m_allow_single_leader = other.get_allow_single_leader();
  m_group_action_running_name.assign(other.get_group_action_running_name());
  m_group_action_running_description.assign(
      other.get_group_action_running_description());
  m_preemptive_garbage_collection = other.get_preemptive_garbage_collection();
  m_component_primary_election_enabled =
      other.get_component_primary_election_enabled();
#ifndef NDEBUG
  skip_encode_default_table_encryption =
      other.skip_encode_default_table_encryption;
  m_skip_encode_view_change_uuid = other.m_skip_encode_view_change_uuid;
#endif
}

/*
  This method does not need to acquire update_lock because it is
  always invoked on a copy of a Group_member_info, that is, it is
  never invoked on local_member_info variable.
*/
void Group_member_info::encode_payload(
    std::vector<unsigned char> *buffer) const {
  DBUG_TRACE;

  encode_payload_item_string(buffer, PIT_HOSTNAME, hostname.c_str(),
                             hostname.length());

  auto port_aux = (uint16)port;
  encode_payload_item_int2(buffer, PIT_PORT, port_aux);

  encode_payload_item_string(buffer, PIT_UUID, uuid.c_str(), uuid.length());

  encode_payload_item_string(buffer, PIT_GCS_ID,
                             gcs_member_id->get_member_id().c_str(),
                             gcs_member_id->get_member_id().length());

  char const status_aux = (uchar)status;
  encode_payload_item_char(buffer, PIT_STATUS, status_aux);

  auto version_aux = (uint32)member_version->get_version();
  encode_payload_item_int4(buffer, PIT_VERSION, version_aux);

  auto write_set_extraction_algorithm_aux =
      (uint16)write_set_extraction_algorithm;
  encode_payload_item_int2(buffer, PIT_WRITE_SET_EXTRACTION_ALGORITHM,
                           write_set_extraction_algorithm_aux);

  encode_payload_item_string(buffer, PIT_EXECUTED_GTID,
                             executed_gtid_set.c_str(),
                             executed_gtid_set.length());

  encode_payload_item_string(buffer, PIT_RETRIEVED_GTID,
                             retrieved_gtid_set.c_str(),
                             retrieved_gtid_set.length());

  encode_payload_item_int8(buffer, PIT_GTID_ASSIGNMENT_BLOCK_SIZE,
                           gtid_assignment_block_size);

  char const role_aux = (uchar)role;
  encode_payload_item_char(buffer, PIT_MEMBER_ROLE, role_aux);

  auto configuration_flags_aux = (uint32)configuration_flags;
  encode_payload_item_int4(buffer, PIT_CONFIGURATION_FLAGS,
                           configuration_flags_aux);

  /*
    MySQL 5.7.18+ payloads
  */
  char const conflict_detection_enable_aux =
      conflict_detection_enable ? '1' : '0';
  encode_payload_item_char(buffer, PIT_CONFLICT_DETECTION_ENABLE,
                           conflict_detection_enable_aux);

  auto member_weight_aux = (uint16)member_weight;
  encode_payload_item_int2(buffer, PIT_MEMBER_WEIGHT, member_weight_aux);

  auto lower_case_table_names_aux = static_cast<uint16>(lower_case_table_names);
#ifndef NDEBUG
  if (lower_case_table_names != SKIP_ENCODING_LOWER_CASE_TABLE_NAMES)
#endif
    encode_payload_item_int2(buffer, PIT_LOWER_CASE_TABLE_NAME,
                             lower_case_table_names_aux);

  /*
    MySQL 8.0+ payloads
  */

  char const is_action_running_aux = group_action_running ? '1' : '0';
  encode_payload_item_char(buffer, PIT_GROUP_ACTION_RUNNING,
                           is_action_running_aux);

  char const is_election_running_aux = primary_election_running ? '1' : '0';
  encode_payload_item_char(buffer, PIT_PRIMARY_ELECTION_RUNNING,
                           is_election_running_aux);

  char const default_table_encryption_aux =
      default_table_encryption ? '1' : '0';
#ifndef NDEBUG
  if (!skip_encode_default_table_encryption)
#endif
    encode_payload_item_char(buffer, PIT_DEFAULT_TABLE_ENCRYPTION,
                             default_table_encryption_aux);

  encode_payload_item_string(buffer, PIT_PURGED_GTID, purged_gtid_set.c_str(),
                             purged_gtid_set.length());

  encode_payload_item_string(buffer, PIT_RECOVERY_ENDPOINTS,
                             recovery_endpoints.c_str(),
                             recovery_endpoints.length());

#ifndef NDEBUG
  if (!m_skip_encode_view_change_uuid)
#endif
    encode_payload_item_string(buffer, PIT_VIEW_CHANGE_UUID,
                               m_view_change_uuid.c_str(),
                               m_view_change_uuid.length());

  char const allow_single_leader_aux = m_allow_single_leader ? '1' : '0';
  encode_payload_item_char(buffer, PIT_ALLOW_SINGLE_LEADER,
                           allow_single_leader_aux);

  if (group_action_running) {
    encode_payload_item_string(buffer, PIT_GROUP_ACTION_RUNNING_NAME,
                               m_group_action_running_name.c_str(),
                               m_group_action_running_name.length());
    encode_payload_item_string(buffer, PIT_GROUP_ACTION_RUNNING_DESCRIPTION,
                               m_group_action_running_description.c_str(),
                               m_group_action_running_description.length());
  }

  char const preemptive_garbage_collection_aux =
      m_preemptive_garbage_collection ? '1' : '0';
  encode_payload_item_char(buffer, PIT_PREEMPTIVE_GARBAGE_COLLECTION,
                           preemptive_garbage_collection_aux);

  char component_primary_election_enabled_aux =
      m_component_primary_election_enabled ? '1' : '0';
  encode_payload_item_char(buffer, PIT_COMPONENT_PRIMARY_ELECTION_ENABLED,
                           component_primary_election_enabled_aux);
}

void Group_member_info::decode_payload(const unsigned char *buffer,
                                       const unsigned char *end) {
  DBUG_TRACE;
  const unsigned char *slider = buffer;
  uint16 payload_item_type = 0;
  unsigned long long payload_item_length = 0;
  MUTEX_LOCK(lock, &update_lock);

  decode_payload_item_string(&slider, &payload_item_type, &hostname,
                             &payload_item_length);

  uint16 port_aux = 0;
  decode_payload_item_int2(&slider, &payload_item_type, &port_aux);
  port = (uint)port_aux;

  decode_payload_item_string(&slider, &payload_item_type, &uuid,
                             &payload_item_length);

  std::string gcs_member_id_aux;
  decode_payload_item_string(&slider, &payload_item_type, &gcs_member_id_aux,
                             &payload_item_length);
  delete gcs_member_id;
  gcs_member_id = new Gcs_member_identifier(gcs_member_id_aux);

  unsigned char status_aux = 0;
  decode_payload_item_char(&slider, &payload_item_type, &status_aux);
  status = (Group_member_status)status_aux;

  uint32 member_version_aux = 0;
  decode_payload_item_int4(&slider, &payload_item_type, &member_version_aux);
  delete member_version;
  member_version = new Member_version(member_version_aux);

  uint16 write_set_extraction_algorithm_aux = 0;
  decode_payload_item_int2(&slider, &payload_item_type,
                           &write_set_extraction_algorithm_aux);
  write_set_extraction_algorithm = (uint)write_set_extraction_algorithm_aux;

  decode_payload_item_string(&slider, &payload_item_type, &executed_gtid_set,
                             &payload_item_length);

  decode_payload_item_string(&slider, &payload_item_type, &retrieved_gtid_set,
                             &payload_item_length);

  decode_payload_item_int8(&slider, &payload_item_type,
                           &gtid_assignment_block_size);

  unsigned char role_aux = 0;
  decode_payload_item_char(&slider, &payload_item_type, &role_aux);
  role = (Group_member_role)role_aux;

  uint32 configuration_flags_aux = 0;
  decode_payload_item_int4(&slider, &payload_item_type,
                           &configuration_flags_aux);
  configuration_flags = configuration_flags_aux;

  /*
    MySQL 5.7.18+ payloads
    We need to check if there are more payload items to read, if the member
    info message was send by a lower version member, there will not.
  */
  while (slider + Plugin_gcs_message::WIRE_PAYLOAD_ITEM_HEADER_SIZE <= end) {
    // Read payload item header to find payload item length.
    decode_payload_item_type_and_length(&slider, &payload_item_type,
                                        &payload_item_length);

    switch (payload_item_type) {
      case PIT_CONFLICT_DETECTION_ENABLE:
        if (slider + payload_item_length <= end) {
          unsigned char const conflict_detection_enable_aux = *slider;
          conflict_detection_enable = conflict_detection_enable_aux == '1';
        }
        break;

      case PIT_MEMBER_WEIGHT:
        if (slider + payload_item_length <= end) {
          uint16 const member_weight_aux = uint2korr(slider);
          member_weight = (uint)member_weight_aux;
        }
        break;

      case PIT_LOWER_CASE_TABLE_NAME:
        if (slider + payload_item_length <= end) {
          uint16 const lower_case_table_names_aux = uint2korr(slider);
          lower_case_table_names =
              static_cast<uint>(lower_case_table_names_aux);
        }
        break;

      case PIT_GROUP_ACTION_RUNNING:
        if (slider + payload_item_length <= end) {
          unsigned char const is_action_running_aux = *slider;
          group_action_running = is_action_running_aux == '1';
        }
        break;

      case PIT_PRIMARY_ELECTION_RUNNING:
        if (slider + payload_item_length <= end) {
          unsigned char const is_election_running_aux = *slider;
          primary_election_running = is_election_running_aux == '1';
        }
        break;

      /*
        If PIT_DEFAULT_TABLE_ENCRYPTION is not included, which will
        happen if this member is from a lower version, the default
        'false' will be used, since those versions did not encrypt
        tables by default.
      */
      case PIT_DEFAULT_TABLE_ENCRYPTION:
        if (slider + payload_item_length <= end) {
          unsigned char const default_table_encryption_aux = *slider;
          default_table_encryption = default_table_encryption_aux == '1';
        }
        break;
      case PIT_PURGED_GTID:
        if (slider + payload_item_length <= end) {
          purged_gtid_set.assign(reinterpret_cast<const char *>(slider),
                                 static_cast<size_t>(payload_item_length));
        }
        break;
      case PIT_RECOVERY_ENDPOINTS:
        if (slider + payload_item_length <= end) {
          recovery_endpoints.assign(reinterpret_cast<const char *>(slider),
                                    static_cast<size_t>(payload_item_length));
        }
        break;
      case PIT_VIEW_CHANGE_UUID:
        if (slider + payload_item_length <= end) {
          m_view_change_uuid.assign(reinterpret_cast<const char *>(slider),
                                    static_cast<size_t>(payload_item_length));
        }
        break;

      case PIT_ALLOW_SINGLE_LEADER:
        if (slider + payload_item_length <= end) {
          unsigned char const allow_single_leader_aux = *slider;
          m_allow_single_leader = allow_single_leader_aux == '1';
        }
        break;
      case PIT_GROUP_ACTION_RUNNING_NAME:
        if (slider + payload_item_length <= end) {
          m_group_action_running_name.assign(
              reinterpret_cast<const char *>(slider),
              static_cast<size_t>(payload_item_length));
        }
        break;
      case PIT_GROUP_ACTION_RUNNING_DESCRIPTION:
        if (slider + payload_item_length <= end) {
          m_group_action_running_description.assign(
              reinterpret_cast<const char *>(slider),
              static_cast<size_t>(payload_item_length));
        }
        break;
      case PIT_PREEMPTIVE_GARBAGE_COLLECTION:
        if (slider + payload_item_length <= end) {
          unsigned char const preemptive_garbage_collection_aux = *slider;
          m_preemptive_garbage_collection =
              preemptive_garbage_collection_aux == '1';
        }
        break;
      case PIT_COMPONENT_PRIMARY_ELECTION_ENABLED:
        if (slider + payload_item_length <= end) {
          unsigned char component_primary_election_enabled_aux = *slider;
          m_component_primary_election_enabled =
              (component_primary_election_enabled_aux == '1') ? true : false;
        }
        break;
    }

    // Seek to next payload item.
    slider += payload_item_length;
  }
}

string Group_member_info::get_hostname() {
  MUTEX_LOCK(lock, &update_lock);
  assert(!hostname.empty());
  return hostname;
}

uint Group_member_info::get_port() {
  MUTEX_LOCK(lock, &update_lock);
  assert(port > 0);
  return port;
}

string Group_member_info::get_uuid() {
  MUTEX_LOCK(lock, &update_lock);
  assert(!uuid.empty());
  return uuid;
}

Group_member_info::Group_member_status
Group_member_info::get_recovery_status() {
  MUTEX_LOCK(lock, &update_lock);
  return status;
}

Group_member_info::Group_member_role Group_member_info::get_role() {
  MUTEX_LOCK(lock, &update_lock);
  return role;
}

const char *Group_member_info::get_member_role_string() {
  MUTEX_LOCK(lock, &update_lock);
  /*
   Member role is only displayed when the member belongs to the group
   and it is reachable.
  */
  if (status != MEMBER_ONLINE && status != MEMBER_IN_RECOVERY) return "";

  if (!in_primary_mode_internal() ||
      role == Group_member_info::MEMBER_ROLE_PRIMARY)
    return "PRIMARY";
  if (role == Group_member_info::MEMBER_ROLE_SECONDARY) return "SECONDARY";
  return "";
}

Gcs_member_identifier Group_member_info::get_gcs_member_id() {
  MUTEX_LOCK(lock, &update_lock);
  assert(!gcs_member_id->get_member_id().empty());
  return *gcs_member_id;
}

void Group_member_info::update_recovery_status(Group_member_status new_status) {
  MUTEX_LOCK(lock, &update_lock);
  status = new_status;
}

void Group_member_info::update_gtid_sets(std::string &executed_gtids,
                                         std::string &purged_gtids,
                                         std::string &retrieved_gtids) {
  MUTEX_LOCK(lock, &update_lock);
  executed_gtid_set.assign(executed_gtids);
  purged_gtid_set.assign(purged_gtids);
  retrieved_gtid_set.assign(retrieved_gtids);
}

void Group_member_info::set_role(Group_member_role new_role) {
  MUTEX_LOCK(lock, &update_lock);
  role = new_role;
}

Member_version Group_member_info::get_member_version() {
  MUTEX_LOCK(lock, &update_lock);
  return *member_version;
}

std::string Group_member_info::get_gtid_executed() {
  MUTEX_LOCK(lock, &update_lock);
  return executed_gtid_set;
}

std::string Group_member_info::get_gtid_purged() {
  MUTEX_LOCK(lock, &update_lock);
  return purged_gtid_set;
}

std::string Group_member_info::get_gtid_retrieved() {
  MUTEX_LOCK(lock, &update_lock);
  return retrieved_gtid_set;
}

uint Group_member_info::get_write_set_extraction_algorithm() {
  MUTEX_LOCK(lock, &update_lock);
  return write_set_extraction_algorithm;
}

const char *Group_member_info::get_write_set_extraction_algorithm_name() {
  switch (get_write_set_extraction_algorithm()) {
    case HASH_ALGORITHM_OFF:
      return "OFF";
    case HASH_ALGORITHM_MURMUR32:
      return "MURMUR32";
    case HASH_ALGORITHM_XXHASH64:
      return "XXHASH64";
    default:
      return "UNKNOWN ALGORITHM";
  }
}

ulonglong Group_member_info::get_gtid_assignment_block_size() {
  MUTEX_LOCK(lock, &update_lock);
  return gtid_assignment_block_size;
}

uint32 Group_member_info::get_configuration_flags() {
  MUTEX_LOCK(lock, &update_lock);
  return configuration_flags;
}

void Group_member_info::set_primary_mode_flag(bool set_primary_mode) {
  MUTEX_LOCK(lock, &update_lock);
  if (set_primary_mode && !(configuration_flags & CNF_SINGLE_PRIMARY_MODE_F)) {
    configuration_flags |= CNF_SINGLE_PRIMARY_MODE_F;
  } else if (!set_primary_mode &&
             configuration_flags & CNF_SINGLE_PRIMARY_MODE_F) {
    configuration_flags ^= CNF_SINGLE_PRIMARY_MODE_F;
  }
}

void Group_member_info::set_enforces_update_everywhere_checks_flag(
    bool enforce_everywhere_checks) {
  MUTEX_LOCK(lock, &update_lock);
  if (enforce_everywhere_checks &&
      !(configuration_flags & CNF_ENFORCE_UPDATE_EVERYWHERE_CHECKS_F)) {
    configuration_flags |= CNF_ENFORCE_UPDATE_EVERYWHERE_CHECKS_F;
  } else if (!enforce_everywhere_checks &&
             configuration_flags & CNF_ENFORCE_UPDATE_EVERYWHERE_CHECKS_F) {
    configuration_flags ^= CNF_ENFORCE_UPDATE_EVERYWHERE_CHECKS_F;
  }
}

bool Group_member_info::in_primary_mode_internal() {
  return configuration_flags & CNF_SINGLE_PRIMARY_MODE_F;
}

bool Group_member_info::in_primary_mode() {
  MUTEX_LOCK(lock, &update_lock);
  return in_primary_mode_internal();
}

bool Group_member_info::has_enforces_update_everywhere_checks() {
  MUTEX_LOCK(lock, &update_lock);
  return configuration_flags & CNF_ENFORCE_UPDATE_EVERYWHERE_CHECKS_F;
}

uint Group_member_info::get_lower_case_table_names() {
  MUTEX_LOCK(lock, &update_lock);
  return lower_case_table_names;
}

bool Group_member_info::is_unreachable() {
  MUTEX_LOCK(lock, &update_lock);
  return unreachable;
}

bool Group_member_info::get_default_table_encryption() {
  MUTEX_LOCK(lock, &update_lock);
  return default_table_encryption;
}

void Group_member_info::set_unreachable() {
  MUTEX_LOCK(lock, &update_lock);
  unreachable = true;
}

void Group_member_info::set_reachable() {
  MUTEX_LOCK(lock, &update_lock);
  unreachable = false;
}

void Group_member_info::enable_conflict_detection() {
  MUTEX_LOCK(lock, &update_lock);
  conflict_detection_enable = true;
}

void Group_member_info::disable_conflict_detection() {
  MUTEX_LOCK(lock, &update_lock);
  conflict_detection_enable = false;
}

bool Group_member_info::is_conflict_detection_enabled() {
  MUTEX_LOCK(lock, &update_lock);
  return conflict_detection_enable;
}

void Group_member_info::set_member_weight(uint new_member_weight) {
  MUTEX_LOCK(lock, &update_lock);
  member_weight = new_member_weight;
}

uint Group_member_info::get_member_weight() {
  MUTEX_LOCK(lock, &update_lock);
  return member_weight;
}

bool Group_member_info::is_group_action_running() {
  MUTEX_LOCK(lock, &update_lock);
  return group_action_running;
}

void Group_member_info::set_is_group_action_running(bool is_running) {
  MUTEX_LOCK(lock, &update_lock);
  group_action_running = is_running;
}

const std::string &Group_member_info::get_group_action_running_name() {
  return m_group_action_running_name;
}

void Group_member_info::set_group_action_running_name(
    const std::string &group_action_running_name) {
  m_group_action_running_name = group_action_running_name;
}

const std::string &Group_member_info::get_group_action_running_description() {
  return m_group_action_running_description;
}

void Group_member_info::set_group_action_running_description(
    const std::string &group_action_running_description) {
  m_group_action_running_description = group_action_running_description;
}

bool Group_member_info::is_primary_election_running() {
  MUTEX_LOCK(lock, &update_lock);
  return primary_election_running;
}

void Group_member_info::set_is_primary_election_running(bool is_running) {
  MUTEX_LOCK(lock, &update_lock);
  primary_election_running = is_running;
}

bool Group_member_info::operator==(Group_member_info &other) {
  MUTEX_LOCK(lock, &update_lock);
  return uuid == other.get_uuid();
}

const char *Group_member_info::get_member_status_string(
    Group_member_status status) {
  switch (status) {
    case MEMBER_ONLINE:
      return "ONLINE";
    case MEMBER_OFFLINE:
      return "OFFLINE";
    case MEMBER_IN_RECOVERY:
      return "RECOVERING";
    case MEMBER_ERROR:
      return "ERROR";
    case MEMBER_UNREACHABLE:
      return "UNREACHABLE";
    default:
      return "OFFLINE"; /* purecov: inspected */
  }
}

const char *Group_member_info::get_configuration_flag_string(
    const uint32 configuation_flag) {
  switch (configuation_flag) {
    case 0:
      return "";
    case CNF_ENFORCE_UPDATE_EVERYWHERE_CHECKS_F:
      return "group_replication_enforce_update_everywhere_checks";
    case CNF_SINGLE_PRIMARY_MODE_F:
      return "group_replication_single_primary_mode";
    default:
      return "UNKNOWN"; /* purecov: inspected */
  }
}

std::string Group_member_info::get_configuration_flags_string(
    const uint32 configuation_flags) {
  std::string result;
  uint32 configuration_flags_mask = 1;

  while (configuration_flags_mask > 0) {
    const uint32 current_flag = configuration_flags_mask & configuation_flags;
    const char *current_flag_name = get_configuration_flag_string(current_flag);

    if (current_flag) {
      if (!result.empty()) result += ","; /* purecov: inspected */

      result += current_flag_name;
    }

    configuration_flags_mask = configuration_flags_mask << 1;
  }

  return result;
}

bool Group_member_info::comparator_group_member_version(Group_member_info *m1,
                                                        Group_member_info *m2) {
  return m2->has_greater_version(m1);
}

string Group_member_info::get_recovery_endpoints() {
  MUTEX_LOCK(lock, &update_lock);
  return recovery_endpoints;
}

void Group_member_info::set_recovery_endpoints(const char *endpoints) {
  MUTEX_LOCK(lock, &update_lock);
  recovery_endpoints.assign(endpoints);
}

string Group_member_info::get_view_change_uuid() {
  MUTEX_LOCK(lock, &update_lock);
  return m_view_change_uuid;
}

bool Group_member_info::get_allow_single_leader() {
  MUTEX_LOCK(lock, &update_lock);
  return m_allow_single_leader;
}

void Group_member_info::set_view_change_uuid(const char *view_change_cnf) {
  MUTEX_LOCK(lock, &update_lock);
  m_view_change_uuid.assign(view_change_cnf);
}

bool Group_member_info::get_preemptive_garbage_collection() {
  MUTEX_LOCK(lock, &update_lock);
  return m_preemptive_garbage_collection;
}

bool Group_member_info::get_component_primary_election_enabled() {
  MUTEX_LOCK(lock, &update_lock);
  return m_component_primary_election_enabled;
}

void Group_member_info::set_component_primary_election_enabled(bool enabled) {
  MUTEX_LOCK(lock, &update_lock);
  m_component_primary_election_enabled = enabled;
}

bool Group_member_info::comparator_group_member_uuid(Group_member_info *m1,
                                                     Group_member_info *m2) {
  return m1->has_lower_uuid(m2);
}

bool Group_member_info::comparator_group_member_weight(Group_member_info *m1,
                                                       Group_member_info *m2) {
  return m1->has_greater_weight(m2);
}

bool Group_member_info::has_greater_version(Group_member_info *other) {
  MUTEX_LOCK(lock, &update_lock);
  return *member_version > *(other->member_version);
}

bool Group_member_info::has_lower_uuid_internal(Group_member_info *other) {
  return uuid.compare(other->get_uuid()) < 0;
}

bool Group_member_info::has_lower_uuid(Group_member_info *other) {
  MUTEX_LOCK(lock, &update_lock);
  return has_lower_uuid_internal(other);
}

bool Group_member_info::has_greater_weight(Group_member_info *other) {
  MUTEX_LOCK(lock, &update_lock);
  if (member_weight > other->get_member_weight()) return true;

  if (member_weight == other->get_member_weight())
    return has_lower_uuid_internal(other);

  return false;
}

Group_member_info_manager::Group_member_info_manager(
    Group_member_info *local_member_info, PSI_mutex_key psi_mutex_key) {
  members = new std::map<
      std::string, Group_member_info *, std::less<std::string>,
      Malloc_allocator<std::pair<const std::string, Group_member_info *>>>(
      Malloc_allocator<std::pair<const std::string, Group_member_info *>>(
          key_group_member_info));
  this->local_member_info = local_member_info;

  mysql_mutex_init(psi_mutex_key, &update_lock, MY_MUTEX_INIT_FAST);

  add(local_member_info);
}

Group_member_info_manager::~Group_member_info_manager() {
  mysql_mutex_destroy(&update_lock);
  clear_members();
  delete members;
}

size_t Group_member_info_manager::get_number_of_members() {
  return members->size();
}

size_t Group_member_info_manager::get_number_of_members_online() {
  size_t number = 0;
  mysql_mutex_lock(&update_lock);

  for (auto &member : *members) {
    if (member.second->get_recovery_status() ==
        Group_member_info::MEMBER_ONLINE) {
      number++;
    }
  }

  mysql_mutex_unlock(&update_lock);
  return number;
}

bool Group_member_info_manager::is_member_info_present(
    const std::string &uuid) {
  bool found = false;
  mysql_mutex_lock(&update_lock);
  map<string, Group_member_info *>::iterator it;
  it = members->find(uuid);
  if (it != members->end()) {
    found = true;
  }
  mysql_mutex_unlock(&update_lock);

  return found;
}

bool Group_member_info_manager::get_group_member_info(
    const string &uuid, Group_member_info &member_info_arg) {
  MUTEX_LOCK(lock, &update_lock);

  auto it = members->find(uuid);
  if (it != members->end()) {
    member_info_arg.update(*it->second);
    return false;
  }

  return true;
}

bool Group_member_info_manager::get_group_member_info_by_index(
    int idx, Group_member_info &member_info_arg) {
  Group_member_info *member = nullptr;
  MUTEX_LOCK(lock, &update_lock);

  map<string, Group_member_info *>::iterator it;
  if (idx < (int)members->size()) {
    int i = 0;
    for (it = members->begin(); i <= idx; i++, it++) {
      member = (*it).second;
    }
  }

  if (member != nullptr) {
    member_info_arg.update(*member);
    return false;
  }

  return true;
}

Member_version Group_member_info_manager::get_group_lowest_online_version() {
  Member_version lowest_version(0xFFFFFF);

  mysql_mutex_lock(&update_lock);

  for (auto &member : *members) {
    if (member.second->get_member_version() < lowest_version &&
        member.second->get_recovery_status() != /* Not part of group */
            Group_member_info::MEMBER_OFFLINE &&
        member.second->get_recovery_status() !=
            Group_member_info::MEMBER_ERROR) {
      lowest_version = member.second->get_member_version();
    }
  }

  mysql_mutex_unlock(&update_lock);

  return lowest_version;
}

Group_member_info *
Group_member_info_manager::get_group_member_info_by_member_id_internal(
    const Gcs_member_identifier &id) {
  mysql_mutex_assert_owner(&update_lock);
  Group_member_info *member = nullptr;

  map<string, Group_member_info *>::iterator it;
  for (it = members->begin(); it != members->end(); it++) {
    if ((*it).second->get_gcs_member_id() == id) {
      member = (*it).second;
      break;
    }
  }

  return member;
}

bool Group_member_info_manager::get_group_member_info_by_member_id(
    const Gcs_member_identifier &id, Group_member_info &member_info_arg) {
  MUTEX_LOCK(lock, &update_lock);

  Group_member_info *member = get_group_member_info_by_member_id_internal(id);
  if (member != nullptr) {
    member_info_arg.update(*member);
    return false;
  }

  return true;
}

std::pair<bool, std::string>
Group_member_info_manager::get_group_member_uuid_from_member_id(
    const Gcs_member_identifier &id) {
  std::pair<bool, std::string> result{true, ""};
  mysql_mutex_lock(&update_lock);

  Group_member_info *member = get_group_member_info_by_member_id_internal(id);
  if (member != nullptr) {
    result.first = false;
    result.second = member->get_uuid();
  }

  mysql_mutex_unlock(&update_lock);
  return result;
}

Group_member_info::Group_member_status
Group_member_info_manager::get_group_member_status_by_member_id(
    const Gcs_member_identifier &id) {
  Group_member_info::Group_member_status status = Group_member_info::MEMBER_END;
  Group_member_info *member = nullptr;
  mysql_mutex_lock(&update_lock);

  member = get_group_member_info_by_member_id_internal(id);
  if (nullptr != member) {
    status = member->get_recovery_status();
  }

  mysql_mutex_unlock(&update_lock);
  return status;
}

Group_member_info_list *Group_member_info_manager::get_all_members() {
  mysql_mutex_lock(&update_lock);

  auto *all_members = new std::vector<Group_member_info *,
                                      Malloc_allocator<Group_member_info *>>(
      Malloc_allocator<Group_member_info *>(key_group_member_info));
  map<string, Group_member_info *>::iterator it;
  for (it = members->begin(); it != members->end(); it++) {
    auto *member_copy = new Group_member_info(*(*it).second);
    all_members->push_back(member_copy);
  }

  mysql_mutex_unlock(&update_lock);
  return all_members;
}

std::list<Gcs_member_identifier>
    *Group_member_info_manager::get_online_members_with_guarantees(
        const Gcs_member_identifier &exclude_member) {
  std::list<Gcs_member_identifier> *online_members = nullptr;
  mysql_mutex_lock(&update_lock);

  for (auto &member : *members) {
    if (member.second->get_member_version().get_version() <
        TRANSACTION_WITH_GUARANTEES_VERSION) {
      goto end; /* purecov: inspected */
    }
  }

  online_members = new std::list<Gcs_member_identifier>();
  for (auto &member : *members) {
    if (member.second->get_recovery_status() ==
            Group_member_info::MEMBER_ONLINE &&
        !(member.second->get_gcs_member_id() == exclude_member)) {
      online_members->push_back(member.second->get_gcs_member_id());
    }
  }

end:
  mysql_mutex_unlock(&update_lock);
  return online_members;
}

void Group_member_info_manager::add(Group_member_info *new_member) {
  mysql_mutex_lock(&update_lock);

  (*members)[new_member->get_uuid()] = new_member;

  mysql_mutex_unlock(&update_lock);
}

void Group_member_info_manager::update(Group_member_info *update_local_member) {
  mysql_mutex_lock(&update_lock);

  this->clear_members();
  members->clear();
  this->local_member_info = update_local_member;
  (*members)[update_local_member->get_uuid()] = update_local_member;

  mysql_mutex_unlock(&update_lock);
}

void Group_member_info_manager::update(Group_member_info_list *new_members) {
  mysql_mutex_lock(&update_lock);

  this->clear_members();

  Group_member_info_list_iterator new_members_it;
  for (new_members_it = new_members->begin();
       new_members_it != new_members->end(); new_members_it++) {
    // If this bears the local member to be updated
    // It will add the current reference and update its status
    if (*(*new_members_it) == *local_member_info) {
      local_member_info->update_recovery_status(
          (*new_members_it)->get_recovery_status());

      delete (*new_members_it);

      continue;
    }

    (*members)[(*new_members_it)->get_uuid()] = (*new_members_it);
  }

  mysql_mutex_unlock(&update_lock);
}

void Group_member_info_manager::update_member_status(
    const string &uuid, Group_member_info::Group_member_status new_status,
    Notification_context &ctx) {
  mysql_mutex_lock(&update_lock);

  map<string, Group_member_info *>::iterator it;

  it = members->find(uuid);

  if (it != members->end()) {
    Group_member_info::Group_member_status const old_status =
        (*it).second->get_recovery_status();
    if (old_status != new_status) {
      (*it).second->update_recovery_status(new_status);
      ctx.set_member_state_changed();
    }
  }

  mysql_mutex_unlock(&update_lock);
}

void Group_member_info_manager::set_member_unreachable(
    const std::string &uuid) {
  MUTEX_LOCK(lock, &update_lock);

  auto it = members->find(uuid);
  if (it != members->end()) {
    (*it).second->set_unreachable();
  }
}

void Group_member_info_manager::set_member_reachable(const std::string &uuid) {
  MUTEX_LOCK(lock, &update_lock);

  auto it = members->find(uuid);
  if (it != members->end()) {
    (*it).second->set_reachable();
  }
}

void Group_member_info_manager::update_gtid_sets(const string &uuid,
                                                 string &gtid_executed,
                                                 std::string &purged_gtids,
                                                 string &gtid_retrieved) {
  mysql_mutex_lock(&update_lock);

  map<string, Group_member_info *>::iterator it;

  it = members->find(uuid);

  if (it != members->end()) {
    (*it).second->update_gtid_sets(gtid_executed, purged_gtids, gtid_retrieved);
  }

  mysql_mutex_unlock(&update_lock);

  view_change_timestamp = Metrics_handler::get_current_time();
}

void Group_member_info_manager::update_member_role(
    const string &uuid, Group_member_info::Group_member_role new_role,
    Notification_context &ctx) {
  mysql_mutex_lock(&update_lock);

  map<string, Group_member_info *>::iterator it;

  it = members->find(uuid);

  if (it != members->end()) {
    Group_member_info::Group_member_role const old_role =
        (*it).second->get_role();
    if (old_role != new_role) {
      (*it).second->set_role(new_role);
      ctx.set_member_role_changed();
    }
  }

  mysql_mutex_unlock(&update_lock);
}

void Group_member_info_manager::update_group_primary_roles(
    const std::string &uuid, Notification_context &ctx) {
  mysql_mutex_lock(&update_lock);

  for (std::pair<const string, Group_member_info *> &member_info : *members) {
    Group_member_info::Group_member_role const new_role =
        (member_info.second->get_uuid() == uuid)
            ? Group_member_info::MEMBER_ROLE_PRIMARY
            : Group_member_info::MEMBER_ROLE_SECONDARY;
    Group_member_info::Group_member_role const old_role =
        member_info.second->get_role();

    if (old_role != new_role) {
      member_info.second->set_role(new_role);
      ctx.set_member_role_changed();
    }
  }

  mysql_mutex_unlock(&update_lock);
}

void Group_member_info_manager::update_member_weight(const std::string &uuid,
                                                     uint member_weight) {
  mysql_mutex_lock(&update_lock);

  map<string, Group_member_info *>::iterator it;

  it = members->find(uuid);

  if (it != members->end()) {
    (*it).second->set_member_weight(member_weight);
  }

  mysql_mutex_unlock(&update_lock);
}

void Group_member_info_manager::update_primary_member_flag(
    bool in_primary_mode) {
  mysql_mutex_lock(&update_lock);
  for (std::pair<const string, Group_member_info *> &member_info : *members) {
    member_info.second->set_primary_mode_flag(in_primary_mode);
  }
  mysql_mutex_unlock(&update_lock);
}

void Group_member_info_manager::update_enforce_everywhere_checks_flag(
    bool enforce_everywhere) {
  mysql_mutex_lock(&update_lock);
  for (std::pair<const string, Group_member_info *> &member_info : *members) {
    member_info.second->set_enforces_update_everywhere_checks_flag(
        enforce_everywhere);
  }
  mysql_mutex_unlock(&update_lock);
}

void Group_member_info_manager::clear_members() {
  auto it = members->begin();
  while (it != members->end()) {
    if ((*it).second == local_member_info) {
      ++it;
      continue;
    }

    delete (*it).second;
    members->erase(it++);
  }
}

bool Group_member_info_manager::is_conflict_detection_enabled() {
  bool conflict_detection = false;

  mysql_mutex_lock(&update_lock);
  auto it = members->begin();
  while (it != members->end()) {
    if ((*it).second != local_member_info) {
      conflict_detection |= (*it).second->is_conflict_detection_enabled();
    }
    ++it;
  }
  mysql_mutex_unlock(&update_lock);

  return conflict_detection;
}

void Group_member_info_manager::encode(vector<uchar> *to_encode) {
  auto *group_info_message = new Group_member_info_manager_message(*this);
  group_info_message->encode(to_encode);
  delete group_info_message;
}

Group_member_info_list *Group_member_info_manager::decode(
    const uchar *to_decode, size_t length) {
  Group_member_info_list *decoded_members = nullptr;

  auto *group_info_message = new Group_member_info_manager_message();
  group_info_message->decode(to_decode, length);
  decoded_members = group_info_message->get_all_members();
  delete group_info_message;

  return decoded_members;
}

bool Group_member_info_manager::get_primary_member_uuid(
    std::string &primary_member_uuid) {
  mysql_mutex_lock(&update_lock);

  if (!local_member_info->in_primary_mode()) {
    mysql_mutex_unlock(&update_lock); /* purecov: inspected */
    primary_member_uuid.assign("");   /* purecov: inspected */
    return false;                     /* purecov: inspected */
  }

  map<string, Group_member_info *>::iterator it;
  for (it = members->begin(); it != members->end(); it++) {
    Group_member_info *info = (*it).second;
    if (info->get_role() == Group_member_info::MEMBER_ROLE_PRIMARY) {
      assert(primary_member_uuid.empty());
      primary_member_uuid = info->get_uuid();
    }
  }

  if (primary_member_uuid.empty() ||
      Group_member_info::MEMBER_ERROR ==
          local_member_info->get_recovery_status())
    primary_member_uuid = "UNDEFINED";
  mysql_mutex_unlock(&update_lock);

  return true;
}

bool Group_member_info_manager::get_primary_member_info(
    Group_member_info &member_info_arg) {
  MUTEX_LOCK(lock, &update_lock);

  map<string, Group_member_info *>::iterator it;
  for (it = members->begin(); it != members->end(); it++) {
    Group_member_info *info = (*it).second;
    if (info->get_role() == Group_member_info::MEMBER_ROLE_PRIMARY) {
      assert(info->in_primary_mode());
      member_info_arg.update(*info);
      return false;
    }
  }

  return true;
}

bool Group_member_info_manager::is_majority_unreachable() {
  bool ret = false;
  int unreachables = 0;

  mysql_mutex_lock(&update_lock);
  auto it = members->begin();

  for (it = members->begin(); it != members->end(); it++) {
    Group_member_info *info = (*it).second;
    if (info->is_unreachable()) unreachables++;
  }
  ret = (members->size() - unreachables) <= (members->size() / 2);
  mysql_mutex_unlock(&update_lock);

  return ret;
}

bool Group_member_info_manager::is_unreachable_member_present() {
  bool ret = false;

  mysql_mutex_lock(&update_lock);
  auto it = members->begin();

  for (it = members->begin(); it != members->end() && !ret; it++) {
    Group_member_info *info = (*it).second;
    if (info->is_unreachable()) {
      ret = true;
    }
  }
  mysql_mutex_unlock(&update_lock);

  return ret;
}

bool Group_member_info_manager::is_recovering_member_present() {
  bool ret = false;

  mysql_mutex_lock(&update_lock);
  auto it = members->begin();

  for (it = members->begin(); it != members->end() && !ret; it++) {
    Group_member_info *info = (*it).second;
    if (info->get_recovery_status() == Group_member_info::MEMBER_IN_RECOVERY) {
      ret = true;
    }
  }
  mysql_mutex_unlock(&update_lock);

  return ret;
}

std::string Group_member_info_manager::get_string_current_view_active_hosts()
    const {
  std::stringstream hosts_string;
  auto all_members_it = members->begin();
  bool first_entry = true;

  while (all_members_it != members->end()) {
    Group_member_info *member_info = (*all_members_it).second;
    if (!first_entry)
      hosts_string << ", ";
    else
      first_entry = false;
    hosts_string << member_info->get_hostname() << ":"
                 << member_info->get_port();
    all_members_it++;
  }

  return hosts_string.str();
}

void Group_member_info_manager::update_component_primary_election_enabled(
    const std::string &uuid, bool enabled) {
  MUTEX_LOCK(lock, &update_lock);

  map<string, Group_member_info *>::iterator it;

  it = members->find(uuid);

  if (it != members->end()) {
    (*it).second->set_component_primary_election_enabled(enabled);
  }
}

bool Group_member_info_manager::
    is_group_replication_elect_prefers_most_updated_enabled() {
  MUTEX_LOCK(lock, &update_lock);
  bool enabled = true;

  for (auto it = members->begin(); it != members->end() && enabled; it++) {
    enabled &= (*it).second->get_component_primary_election_enabled();
  }

  return enabled;
}

uint64_t Group_member_info_manager::get_timestamp_last_view_change() {
  return view_change_timestamp;
}

Group_member_info_manager_message::Group_member_info_manager_message()
    : Plugin_gcs_message(CT_MEMBER_INFO_MANAGER_MESSAGE) {
  DBUG_TRACE;
  members = new std::vector<Group_member_info *,
                            Malloc_allocator<Group_member_info *>>(
      Malloc_allocator<Group_member_info *>(key_group_member_info));
}

Group_member_info_manager_message::Group_member_info_manager_message(
    Group_member_info_manager &group_info)
    : Plugin_gcs_message(CT_MEMBER_INFO_MANAGER_MESSAGE),
      members(group_info.get_all_members()) {
  DBUG_TRACE;
}

Group_member_info_manager_message::Group_member_info_manager_message(
    Group_member_info *member_info)
    : Plugin_gcs_message(CT_MEMBER_INFO_MANAGER_MESSAGE), members(nullptr) {
  DBUG_TRACE;
  members = new std::vector<Group_member_info *,
                            Malloc_allocator<Group_member_info *>>(
      Malloc_allocator<Group_member_info *>(key_group_member_info));
  members->push_back(member_info);
}

Group_member_info_manager_message::~Group_member_info_manager_message() {
  DBUG_TRACE;
  clear_members();
  delete members;
}

void Group_member_info_manager_message::clear_members() {
  DBUG_TRACE;
  Group_member_info_list_iterator it;
  for (it = members->begin(); it != members->end(); it++) {
    delete (*it);
  }
  members->clear();
}

Group_member_info_list *Group_member_info_manager_message::get_all_members() {
  DBUG_TRACE;
  auto *all_members = new std::vector<Group_member_info *,
                                      Malloc_allocator<Group_member_info *>>(
      Malloc_allocator<Group_member_info *>(key_group_member_info));

  Group_member_info_list_iterator it;
  for (it = members->begin(); it != members->end(); it++) {
    auto *member_copy = new Group_member_info(*(*it));
    all_members->push_back(member_copy);
  }

  return all_members;
}

void Group_member_info_manager_message::encode_payload(
    std::vector<unsigned char> *buffer) const {
  DBUG_TRACE;

  auto number_of_members = (uint16)members->size();
  encode_payload_item_int2(buffer, PIT_MEMBERS_NUMBER, number_of_members);

  Group_member_info_list_iterator it;
  for (it = members->begin(); it != members->end(); it++) {
    std::vector<uchar> encoded_member;
    (*it)->encode(&encoded_member);

    encode_payload_item_type_and_length(buffer, PIT_MEMBER_DATA,
                                        encoded_member.size());
    buffer->insert(buffer->end(), encoded_member.begin(), encoded_member.end());
  }
}

void Group_member_info_manager_message::decode_payload(
    const unsigned char *buffer, const unsigned char *) {
  DBUG_TRACE;
  const unsigned char *slider = buffer;
  uint16 payload_item_type = 0;
  unsigned long long payload_item_length = 0;

  uint16 number_of_members = 0;
  decode_payload_item_int2(&slider, &payload_item_type, &number_of_members);

  clear_members();
  for (uint16 i = 0; i < number_of_members; i++) {
    decode_payload_item_type_and_length(&slider, &payload_item_type,
                                        &payload_item_length);
    auto *member = new Group_member_info(slider,
                                         payload_item_length
#ifdef DISABLE_PSI_MUTEX
                                         // Allow use this method on unit tests.
                                         ,
                                         PSI_NOT_INSTRUMENTED
#endif
    );
    members->push_back(member);
    slider += payload_item_length;
  }
}

void Group_member_info_manager_message::
    add_member_actions_serialized_configuration(
        std::vector<unsigned char> *buffer,
        const std::string &member_actions_serialized_configuration) const {
  DBUG_TRACE;

  encode_payload_item_type_and_length(
      buffer, PIT_MEMBER_ACTIONS,
      member_actions_serialized_configuration.size());
  buffer->insert(buffer->end(), member_actions_serialized_configuration.begin(),
                 member_actions_serialized_configuration.end());
}

bool Group_member_info_manager_message::get_pit_data(
    const enum_payload_item_type pit, const unsigned char *buffer,
    size_t length, const unsigned char **pit_data, size_t *pit_length) {
  DBUG_TRACE;
  const unsigned char *slider = buffer;
  const unsigned char *end = buffer + length;
  uint16 payload_item_type = 0;
  unsigned long long payload_item_length = 0;

  decode_header(&slider);

  uint16 number_of_members = 0;
  decode_payload_item_int2(&slider, &payload_item_type, &number_of_members);

  for (uint16 i = 0; i < number_of_members; i++) {
    decode_payload_item_type_and_length(&slider, &payload_item_type,
                                        &payload_item_length);
    slider += payload_item_length;
  }

  while (slider + Plugin_gcs_message::WIRE_PAYLOAD_ITEM_HEADER_SIZE <= end) {
    // Read payload item header to find payload item length.
    decode_payload_item_type_and_length(&slider, &payload_item_type,
                                        &payload_item_length);

    if (pit == payload_item_type) {
      if (slider + payload_item_length <= end) {
        *pit_data = slider;
        *pit_length = payload_item_length;
        return false;
      }
    }

    // Seek to next payload item.
    slider += payload_item_length;
  }

  return true;
}

void Group_member_info_manager_message::
    add_replication_failover_channels_serialized_configuration(
        std::vector<unsigned char> *buffer,
        const std::string
            &replication_failover_channels_serialized_configuration) const {
  DBUG_TRACE;

  encode_payload_item_type_and_length(
      buffer, PIT_RPL_FAILOVER_CONFIGURATION,
      replication_failover_channels_serialized_configuration.size());
  buffer->insert(buffer->end(),
                 replication_failover_channels_serialized_configuration.begin(),
                 replication_failover_channels_serialized_configuration.end());
}
