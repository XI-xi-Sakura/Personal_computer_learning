/* Copyright (c) 2020, 2025, Oracle and/or its affiliates.

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
#include <atomic>
#include <functional>
#include <set>
#include <string>

#include <mysql/components/services/mysql_rwlock.h>

#include <unordered_map>
#include "cache_allocator.h"
#include "reference_cache_common.h"

namespace reference_caching {

class channel_imp : public Cache_malloced {
 public:
  static channel_imp *create(service_names_set<> &service_names);
  static bool destroy(channel_imp *channel);
  static bool factory_init();
  static bool factory_deinit();
  static bool ignore_list_add(channel_imp *channel,
                              std::string service_implementation);
  static bool ignore_list_clear(channel_imp *channel);
  static bool ignore_list_remove(channel_imp *channel,
                                 std::string service_implementation);
  static void increment_version(channel_imp *channel);
  static bool service_notification(const char **services, unsigned int count,
                                   bool unload);

  unsigned int version() { return m_version.load(std::memory_order_relaxed); }

  service_names_set<> &get_service_names();

  void ignore_list_copy(
      service_names_set<std::string, std::less<std::string>> &dest_set);

  bool is_alone() { return m_reference_count == 1; }
  channel_imp *ref() {
    m_reference_count.fetch_add(1, std::memory_order_relaxed);
    return this;
  }
  int unref() {
    return m_reference_count.fetch_sub(1, std::memory_order_relaxed);
  }
  ~channel_imp();

 protected:
  channel_imp();
  explicit channel_imp(service_names_set<> &service_names);

 private:
  inline void increment_version_no_lock() {
    m_version.fetch_add(1, std::memory_order_relaxed);
  }
  bool ignore_list_add(std::string &service_implementation);
  bool ignore_list_clear();
  bool ignore_list_remove(std::string &service_implementation);
  // disable copy constructors
  channel_imp(const channel_imp &) = delete;
  channel_imp &operator=(const channel_imp &) = delete;
  void initialize_service_counts();

  service_names_set<> m_service_names;
  service_names_set<std::string, std::less<std::string>> m_ignore_list;
  std::atomic<bool> m_has_ignore_list;
  std::atomic<int> m_reference_count;
  std::atomic<unsigned int> m_version;

  mysql_rwlock_t m_lock;
};

typedef std::unordered_multimap<
    std::string, channel_imp *, std::hash<std::string>,
    std::equal_to<std::string>,
    Component_malloc_allocator<std::pair<const std::string, channel_imp *>>>
    channel_by_name_hash_t;

extern channel_by_name_hash_t *channel_by_name_hash;
extern mysql_rwlock_t LOCK_channels;

}  // namespace reference_caching
