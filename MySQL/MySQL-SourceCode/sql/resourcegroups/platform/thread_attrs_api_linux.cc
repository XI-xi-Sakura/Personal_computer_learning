/* Copyright (c) 2017, 2025, Oracle and/or its affiliates.

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

#include <sched.h>
#include <sys/resource.h>
#include <syscall.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>

#include <linux/capability.h>
#include <vector>
#include "my_sys.h"
#include "my_thread_local.h"
#include "my_thread_os_id.h"
#include "mysql_com.h"
#include "thread_attrs_api.h"

#include "my_dbug.h"  // DBUG_*
#include "mysql/components/library_mysys/my_system.h"
#include "mysql/components/services/log_builtins.h"
#include "mysqld_error.h"

namespace resourcegroups::platform {

bool is_platform_supported() { return true; }

bool bind_to_cpu(cpu_id_t cpu_id) {
  return bind_to_cpu(cpu_id, ::syscall(__NR_gettid));
}

bool bind_to_cpu(cpu_id_t cpu_id, my_thread_os_id_t thread_id) {
  DBUG_TRACE;

  cpu_set_t cpu_set;

  CPU_ZERO(&cpu_set);
  CPU_SET(cpu_id, &cpu_set);
  int const rc = ::sched_setaffinity(thread_id, sizeof(cpu_set), &cpu_set);
  if (rc != 0) {
    char errbuf[MYSQL_ERRMSG_SIZE];
    LogErr(ERROR_LEVEL, ER_RES_GRP_SET_THR_AFFINITY_FAILED, thread_id, cpu_id,
           my_errno(), my_strerror(errbuf, MYSQL_ERRMSG_SIZE, my_errno()));
    return true;
  }
  return false;
}

bool bind_to_cpus(const std::vector<cpu_id_t> &cpu_ids) {
  return bind_to_cpus(cpu_ids, ::syscall(__NR_gettid));
}

bool bind_to_cpus(const std::vector<cpu_id_t> &cpu_ids,
                  my_thread_os_id_t thread_id) {
  DBUG_TRACE;

  if (cpu_ids.empty()) return false;

  cpu_set_t cpu_set;

  CPU_ZERO(&cpu_set);
  for (const auto &cpu_id : cpu_ids) CPU_SET(cpu_id, &cpu_set);
  int const rc = ::sched_setaffinity(thread_id, sizeof(cpu_set), &cpu_set);
  if (rc != 0) {
    char errbuf[MYSQL_ERRMSG_SIZE];
    LogErr(ERROR_LEVEL, ER_RES_GRP_SET_THR_AFFINITY_FAILED, thread_id,
           my_errno(), my_strerror(errbuf, MYSQL_ERRMSG_SIZE, my_errno()));
    return true;
  }
  return false;
}

bool unbind_thread() { return unbind_thread(::syscall(__NR_gettid)); }

bool unbind_thread(my_thread_os_id_t thread_id) {
  DBUG_TRACE;

  cpu_set_t cpu_set;

  CPU_ZERO(&cpu_set);
  uint32_t num_cpus = my_num_vcpus();
  if (num_cpus == 0) {
    char errbuf[MYSQL_ERRMSG_SIZE];
    LogErr(ERROR_LEVEL, ER_RES_GRP_THD_UNBIND_FROM_CPU_FAILED, thread_id,
           my_errno(), my_strerror(errbuf, MYSQL_ERRMSG_SIZE, my_errno()));
    return true;
  }
  DBUG_EXECUTE_IF("make_sure_cpu_affinity_is_dropped",
                  /*
                    Test is run on a machine with min 4 CPUs by a thread bound
                    to only one CPU. So CPU count must be greater than 1 here
                    (Should be equal to number of CPUs on machine).
                  */
                  assert(num_cpus > 1););
  for (cpu_id_t cpu_id = 0; cpu_id < num_cpus; ++cpu_id)
    CPU_SET(cpu_id, &cpu_set);
  int const rc = sched_setaffinity(thread_id, sizeof(cpu_set), &cpu_set);
  if (rc != 0) {
    char errbuf[MYSQL_ERRMSG_SIZE];
    LogErr(ERROR_LEVEL, ER_RES_GRP_THD_UNBIND_FROM_CPU_FAILED, thread_id,
           my_errno(), my_strerror(errbuf, MYSQL_ERRMSG_SIZE, my_errno()));
    return true;
  }
  return false;
}

int thread_priority() {
  return getpriority(PRIO_PROCESS, ::syscall(__NR_gettid));
}

int thread_priority(my_thread_os_id_t thread_id) {
  DBUG_TRACE;
  return getpriority(PRIO_PROCESS, thread_id);
}

bool set_thread_priority(int priority) {
  return set_thread_priority(priority, my_thread_os_id());
}

bool set_thread_priority(int priority, my_thread_os_id_t thread_id) {
  DBUG_TRACE;
  if (!can_thread_priority_be_set()) return false;

  if (setpriority(PRIO_PROCESS, thread_id, priority) < 0) {
    char errbuf[MYSQL_ERRMSG_SIZE];
    LogErr(ERROR_LEVEL, ER_RES_GRP_SET_THREAD_PRIORITY_FAILED, priority,
           thread_id, my_errno(),
           my_strerror(errbuf, MYSQL_ERRMSG_SIZE, my_errno()));
    return true;
  }
  return false;
}

bool can_thread_priority_be_set() {
  if (geteuid() == 0) return true;

  // Check if mysqld has CAP_SYS_NICE capability set.
  __user_cap_data_struct cap_bits[_LINUX_CAPABILITY_U32S_3];
  __user_cap_header_struct cap_header = {_LINUX_CAPABILITY_VERSION_3, 0};
  memset(cap_bits, 0, sizeof(cap_bits));
  if (syscall(__NR_capget, &cap_header, &cap_bits) == 0 &&
      cap_header.version == _LINUX_CAPABILITY_VERSION_3) {
    auto index = CAP_TO_INDEX(CAP_SYS_NICE);
    auto mask = CAP_TO_MASK(CAP_SYS_NICE);
    return cap_bits[index].effective & mask;
  }

  LogErr(ERROR_LEVEL, ER_RES_GRP_FAILED_TO_DETERMINE_NICE_CAPABILITY);
  return false;
}
}  // namespace resourcegroups::platform
