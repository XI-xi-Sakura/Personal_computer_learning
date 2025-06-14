/* Copyright (c) 2008, 2025, Oracle and/or its affiliates.

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

/**
  @file storage/perfschema/pfs_instr_class.cc
  Performance schema instruments metadata (implementation).
*/

#include "storage/perfschema/pfs_instr_class.h"

#include <assert.h>
#include <string.h>
#include <algorithm>
#include <atomic>

#include "lex_string.h"
#include "lf.h"

#include "my_dbug.h"
#include "my_macros.h"
#include "my_sys.h"
#include "my_systime.h"
#include "mysql/psi/mysql_thread.h"
#include "mysql/strings/m_ctype.h"
#include "sql/mysqld.h"  // lower_case_table_names
#include "sql/table.h"
#include "storage/perfschema/pfs_buffer_container.h"
#include "storage/perfschema/pfs_builtin_memory.h"
#include "storage/perfschema/pfs_column_values.h"
#include "storage/perfschema/pfs_events_waits.h"
#include "storage/perfschema/pfs_global.h"
#include "storage/perfschema/pfs_instr.h"
#include "storage/perfschema/pfs_program.h"
#include "storage/perfschema/pfs_setup_object.h"
#include "storage/perfschema/pfs_timer.h"
#include "storage/perfschema/terminology_use_previous.h"

#include <mysql/components/services/mysql_server_telemetry_logs_service.h>
extern std::atomic<log_delivery_callback_t> g_telemetry_log;

/**
  @defgroup performance_schema_buffers Performance Schema Buffers
  @ingroup performance_schema_implementation
  @{
*/

/**
  Global performance schema flag.
  Indicate if the performance schema is enabled.
  This flag is set at startup, and never changes.
*/
bool pfs_enabled = true;

/**
  Global performance schema reference count for plugin and component events.
  Incremented when a shared library is being unloaded, decremented when
  the performance schema is finished processing the event.
*/
std::atomic<uint32> pfs_unload_plugin_ref_count(0);

/**
  PFS_INSTRUMENT option settings array
 */
Pfs_instr_config_array *pfs_instr_config_array = nullptr;

static void configure_instr_class(PFS_instr_class *entry);

static void init_instr_class(PFS_instr_class *klass, const char *name,
                             uint name_length, int flags, int volatility,
                             const char *documentation,
                             PFS_class_type class_type);

/**
  PFS_METER option settings array
 */
Pfs_meter_config_array *pfs_meter_config_array = nullptr;

static void configure_meter_class(PFS_meter_class *entry);

/**
  PFS_LOGGER option settings array
 */
Pfs_logger_config_array *pfs_logger_config_array = nullptr;

static void configure_logger_class(PFS_logger_class *entry);

/**
  Current number of elements in mutex_class_array.
  This global variable is written to during:
  - the performance schema initialization
  - a plugin initialization
*/
static std::atomic<uint32> mutex_class_dirty_count{0};
static std::atomic<uint32> mutex_class_allocated_count{0};
static std::atomic<uint32> rwlock_class_dirty_count{0};
static std::atomic<uint32> rwlock_class_allocated_count{0};
static std::atomic<uint32> cond_class_dirty_count{0};
static std::atomic<uint32> cond_class_allocated_count{0};
static std::atomic<uint32> meter_class_dirty_count{0};
static std::atomic<uint32> meter_class_allocated_count{0};
static std::atomic<uint32> metric_class_dirty_count{0};
static std::atomic<uint32> metric_class_allocated_count{0};
static std::atomic<uint32> logger_class_dirty_count{0};
static std::atomic<uint32> logger_class_allocated_count{0};

/** Size of the mutex class array. @sa mutex_class_array */
ulong mutex_class_max = 0;
/** Number of mutex class lost. @sa mutex_class_array */
ulong mutex_class_lost = 0;
/** Size of the rwlock class array. @sa rwlock_class_array */
ulong rwlock_class_max = 0;
/** Number of rwlock class lost. @sa rwlock_class_array */
ulong rwlock_class_lost = 0;
/** Size of the condition class array. @sa cond_class_array */
ulong cond_class_max = 0;
/** Number of condition class lost. @sa cond_class_array */
ulong cond_class_lost = 0;
/** Size of the thread class array. @sa thread_class_array */
ulong thread_class_max = 0;
/** Number of thread class lost. @sa thread_class_array */
ulong thread_class_lost = 0;
/** Size of the file class array. @sa file_class_array */
ulong file_class_max = 0;
/** Number of file class lost. @sa file_class_array */
ulong file_class_lost = 0;
/** Size of the stage class array. @sa stage_class_array */
ulong stage_class_max = 0;
/** Number of stage class lost. @sa stage_class_array */
ulong stage_class_lost = 0;
/** Size of the statement class array. @sa statement_class_array */
ulong statement_class_max = 0;
/** Number of statement class lost. @sa statement_class_array */
ulong statement_class_lost = 0;
/** Size of the socket class array. @sa socket_class_array */
ulong socket_class_max = 0;
/** Number of socket class lost. @sa socket_class_array */
ulong socket_class_lost = 0;
/** Size of the memory class array. @sa memory_class_array */
ulong memory_class_max = 0;
/** Number of memory class lost. @sa memory_class_array */
ulong memory_class_lost = 0;
/** Size of the meter class array. @sa meter_class_array */
ulong meter_class_max = 0;
/** Number of meter class lost. @sa meter_class_array */
ulong meter_class_lost = 0;
/** Size of the metric class array. @sa metric_class_array */
ulong metric_class_max = 0;
/** Number of metric class lost. @sa metric_class_array */
ulong metric_class_lost = 0;
/** Size of the logger class array. @sa logger_class_array */
ulong logger_class_max = 0;
/** Number of logger class lost. @sa logger_class_array */
ulong logger_class_lost = 0;

/**
  Number of transaction classes. Although there is only one transaction class,
  this is used for sizing by other event classes.
  @sa global_transaction_class
*/
ulong transaction_class_max = 0;

/**
  Number of error classes. Although there is only one error class,
  this is kept for future use if there is more error classification required.
  @sa global_error_class
*/
ulong error_class_max = 0;

PFS_mutex_class *mutex_class_array = nullptr;
PFS_rwlock_class *rwlock_class_array = nullptr;
PFS_cond_class *cond_class_array = nullptr;
PFS_meter_class *meter_class_array = nullptr;
PFS_metric_class *metric_class_array = nullptr;
PFS_logger_class *logger_class_array = nullptr;

/**
  Current number or elements in thread_class_array.
  This global variable is written to during:
  - the performance schema initialization
  - a plugin initialization
*/
static std::atomic<uint32> thread_class_dirty_count{0};
static std::atomic<uint32> thread_class_allocated_count{0};

static PFS_thread_class *thread_class_array = nullptr;

PFS_ALIGNED PFS_single_stat global_idle_stat;
PFS_ALIGNED PFS_table_io_stat global_table_io_stat;
PFS_ALIGNED PFS_table_lock_stat global_table_lock_stat;
PFS_ALIGNED PFS_single_stat global_metadata_stat;
PFS_ALIGNED PFS_transaction_stat global_transaction_stat;
PFS_ALIGNED PFS_error_stat global_error_stat;
PFS_ALIGNED PFS_instr_class global_table_io_class;
PFS_ALIGNED PFS_instr_class global_table_lock_class;
PFS_ALIGNED PFS_instr_class global_idle_class;
PFS_ALIGNED PFS_instr_class global_metadata_class;
PFS_ALIGNED PFS_error_class global_error_class;
PFS_ALIGNED PFS_transaction_class global_transaction_class;
PFS_ALIGNED PFS_meter_class global_meter_class;
PFS_ALIGNED PFS_metric_class global_metric_class;
PFS_ALIGNED PFS_logger_class global_logger_class;

/**
  Hash index for instrumented table shares.
  This index is searched by table fully qualified name (@c PFS_table_share_key),
  and points to instrumented table shares (@c PFS_table_share).
  @sa PFS_table_share_key
  @sa PFS_table_share
  @sa table_share_hash_get_key
  @sa get_table_share_hash_pins
*/
LF_HASH table_share_hash;
/** True if table_share_hash is initialized. */
static bool table_share_hash_inited = false;

static std::atomic<uint32> file_class_dirty_count{0};
static std::atomic<uint32> file_class_allocated_count{0};

PFS_file_class *file_class_array = nullptr;

static std::atomic<uint32> stage_class_dirty_count{0};
static std::atomic<uint32> stage_class_allocated_count{0};

static PFS_stage_class *stage_class_array = nullptr;

static std::atomic<uint32> statement_class_dirty_count{0};
static std::atomic<uint32> statement_class_allocated_count{0};

static PFS_statement_class *statement_class_array = nullptr;

static std::atomic<uint32> socket_class_dirty_count{0};
static std::atomic<uint32> socket_class_allocated_count{0};

static PFS_socket_class *socket_class_array = nullptr;

static std::atomic<uint32> memory_class_dirty_count{0};
static std::atomic<uint32> memory_class_allocated_count{0};

static std::atomic<PFS_memory_class *> memory_class_array{nullptr};

uint mutex_class_start = 0;
uint rwlock_class_start = 0;
uint cond_class_start = 0;
uint file_class_start = 0;
uint wait_class_max = 0;
uint socket_class_start = 0;

const char *PFS_instr_name::str() const {
  DBUG_TRACE;
  if (m_private_old_name != nullptr &&
      terminology_use_previous::is_older_required(m_private_version))
    return m_private_old_name;
  return m_private_name;
}

uint PFS_instr_name::length() const {
  if (m_private_old_name != nullptr &&
      terminology_use_previous::is_older_required(m_private_version))
    return m_private_old_name_length;
  return m_private_name_length;
}

/**
  Like strlen (or POSIX strnlen), but don't read past the max_len'th
  character.

  This is useful when the string may be terminated without '\0' at the
  end of the buffer.

  @param s The string
  @param max_len The maxmium length
  @return The length of the string, or max_len if the string is longer
  than that.
*/
static uint safe_strlen(const char *s, uint max_len) {
  const char *end =
      static_cast<const char *>(memchr(s, '\0', static_cast<size_t>(max_len)));
  return end == nullptr ? max_len : static_cast<uint>(end - s);
}

void PFS_instr_name::set(PFS_class_type class_type, const char *name,
                         uint max_length_arg) {
  // Copy the given name to the member.
  const uint length = safe_strlen(name, std::min(max_length, max_length_arg));
  memcpy(m_private_name, name, length);
  m_private_name[length] = '\0';
  m_private_name_length = length;

  // Check if there is an alternative name to use when
  // @@terminology_use_previous is enabled.
  const auto compatible_name =
      terminology_use_previous::lookup(class_type, std::string{name, length});
  m_private_old_name = compatible_name.old_name;
  m_private_old_name_length =
      compatible_name.old_name ? strlen(compatible_name.old_name) : 0;
  m_private_version = compatible_name.version;
}

void init_event_name_sizing(const PFS_global_param *param) {
  /* global table I/O, table lock, idle, metadata */
  mutex_class_start = COUNT_GLOBAL_EVENT_INDEX;
  rwlock_class_start = mutex_class_start + param->m_mutex_class_sizing;
  cond_class_start = rwlock_class_start + param->m_rwlock_class_sizing;
  file_class_start = cond_class_start + param->m_cond_class_sizing;
  socket_class_start = file_class_start + param->m_file_class_sizing;
  wait_class_max = socket_class_start + param->m_socket_class_sizing;
}

void register_global_classes() {
  /* Table I/O class */
  init_instr_class(&global_table_io_class, table_io_class_name.str,
                   (uint)table_io_class_name.length, 0, 0, PSI_DOCUMENT_ME,
                   PFS_CLASS_TABLE_IO);
  global_table_io_class.m_event_name_index = GLOBAL_TABLE_IO_EVENT_INDEX;
  configure_instr_class(&global_table_io_class);

  /* Table lock class */
  init_instr_class(&global_table_lock_class, table_lock_class_name.str,
                   (uint)table_lock_class_name.length, 0, 0, PSI_DOCUMENT_ME,
                   PFS_CLASS_TABLE_LOCK);
  global_table_lock_class.m_event_name_index = GLOBAL_TABLE_LOCK_EVENT_INDEX;
  configure_instr_class(&global_table_lock_class);

  /* Idle class */
  init_instr_class(&global_idle_class, idle_class_name.str,
                   (uint)idle_class_name.length, PSI_FLAG_USER,
                   0, /* no volatility */
                   PSI_DOCUMENT_ME, PFS_CLASS_IDLE);
  global_idle_class.m_event_name_index = GLOBAL_IDLE_EVENT_INDEX;
  configure_instr_class(&global_idle_class);

  /* Metadata class */
  init_instr_class(&global_metadata_class, metadata_lock_class_name.str,
                   (uint)metadata_lock_class_name.length, 0, 0, PSI_DOCUMENT_ME,
                   PFS_CLASS_METADATA);
  global_metadata_class.m_event_name_index = GLOBAL_METADATA_EVENT_INDEX;
  configure_instr_class(&global_metadata_class);

  /* Error class */
  init_instr_class(&global_error_class, error_class_name.str,
                   (uint)error_class_name.length, 0, 0, PSI_DOCUMENT_ME,
                   PFS_CLASS_ERROR);
  global_error_class.m_event_name_index = GLOBAL_ERROR_INDEX;
  global_error_class.m_enabled = true; /* Enabled by default */
  configure_instr_class(&global_error_class);
  global_error_class.m_timed = false; /* Not applicable */
  error_class_max = 1;                /* only one error class as of now. */

  /* Transaction class */
  init_instr_class(&global_transaction_class, transaction_instrument_prefix.str,
                   (uint)transaction_instrument_prefix.length, 0, 0,
                   PSI_DOCUMENT_ME, PFS_CLASS_TRANSACTION);
  global_transaction_class.m_event_name_index = GLOBAL_TRANSACTION_INDEX;
  configure_instr_class(&global_transaction_class);
  transaction_class_max = 1; /* used for sizing by other event classes */
}

/**
  Initialize the instrument synch class buffers.
  @param mutex_class_sizing           max number of mutex class
  @param rwlock_class_sizing          max number of rwlock class
  @param cond_class_sizing            max number of condition class
  @return 0 on success
*/
int init_sync_class(uint mutex_class_sizing, uint rwlock_class_sizing,
                    uint cond_class_sizing) {
  mutex_class_dirty_count = mutex_class_allocated_count = 0;
  rwlock_class_dirty_count = rwlock_class_allocated_count = 0;
  cond_class_dirty_count = cond_class_allocated_count = 0;
  mutex_class_max = mutex_class_sizing;
  rwlock_class_max = rwlock_class_sizing;
  cond_class_max = cond_class_sizing;
  mutex_class_lost = rwlock_class_lost = cond_class_lost = 0;

  mutex_class_array = nullptr;
  rwlock_class_array = nullptr;
  cond_class_array = nullptr;

  if (mutex_class_max > 0) {
    mutex_class_array = PFS_MALLOC_ARRAY(
        &builtin_memory_mutex_class, mutex_class_max, sizeof(PFS_mutex_class),
        PFS_mutex_class, MYF(MY_ZEROFILL));
    if (unlikely(mutex_class_array == nullptr)) {
      mutex_class_max = 0;
      return 1;
    }
  }

  if (rwlock_class_max > 0) {
    rwlock_class_array = PFS_MALLOC_ARRAY(
        &builtin_memory_rwlock_class, rwlock_class_max,
        sizeof(PFS_rwlock_class), PFS_rwlock_class, MYF(MY_ZEROFILL));
    if (unlikely(rwlock_class_array == nullptr)) {
      rwlock_class_max = 0;
      return 1;
    }
  }

  if (cond_class_max > 0) {
    cond_class_array = PFS_MALLOC_ARRAY(&builtin_memory_cond_class,
                                        cond_class_max, sizeof(PFS_cond_class),
                                        PFS_cond_class, MYF(MY_ZEROFILL));
    if (unlikely(cond_class_array == nullptr)) {
      cond_class_max = 0;
      return 1;
    }
  }

  return 0;
}

/** Cleanup the instrument synch class buffers. */
void cleanup_sync_class() {
  unsigned int i;

  if (mutex_class_array != nullptr) {
    for (i = 0; i < mutex_class_max; i++) {
      my_free(mutex_class_array[i].m_documentation);
    }
  }

  PFS_FREE_ARRAY(&builtin_memory_mutex_class, mutex_class_max,
                 sizeof(PFS_mutex_class), mutex_class_array);
  mutex_class_array = nullptr;
  mutex_class_dirty_count = mutex_class_allocated_count = mutex_class_max = 0;

  if (rwlock_class_array != nullptr) {
    for (i = 0; i < rwlock_class_max; i++) {
      my_free(rwlock_class_array[i].m_documentation);
    }
  }

  PFS_FREE_ARRAY(&builtin_memory_rwlock_class, rwlock_class_max,
                 sizeof(PFS_rwlock_class), rwlock_class_array);
  rwlock_class_array = nullptr;
  rwlock_class_dirty_count = rwlock_class_allocated_count = rwlock_class_max =
      0;

  if (cond_class_array != nullptr) {
    for (i = 0; i < cond_class_max; i++) {
      my_free(cond_class_array[i].m_documentation);
    }
  }

  PFS_FREE_ARRAY(&builtin_memory_cond_class, cond_class_max,
                 sizeof(PFS_cond_class), cond_class_array);
  cond_class_array = nullptr;
  cond_class_dirty_count = cond_class_allocated_count = cond_class_max = 0;
}

/**
  Initialize the thread class buffer.
  @param thread_class_sizing          max number of thread class
  @return 0 on success
*/
int init_thread_class(uint thread_class_sizing) {
  int result = 0;
  thread_class_dirty_count = thread_class_allocated_count = 0;
  thread_class_max = thread_class_sizing;
  thread_class_lost = 0;

  if (thread_class_max > 0) {
    thread_class_array = PFS_MALLOC_ARRAY(
        &builtin_memory_thread_class, thread_class_max,
        sizeof(PFS_thread_class), PFS_thread_class, MYF(MY_ZEROFILL));
    if (unlikely(thread_class_array == nullptr)) {
      thread_class_max = 0;
      result = 1;
    }
  } else {
    thread_class_array = nullptr;
  }

  return result;
}

/** Cleanup the thread class buffers. */
void cleanup_thread_class() {
  if (thread_class_array != nullptr) {
    for (unsigned int i = 0; i < thread_class_max; i++) {
      my_free(thread_class_array[i].m_documentation);
    }
  }

  PFS_FREE_ARRAY(&builtin_memory_thread_class, thread_class_max,
                 sizeof(PFS_thread_class), thread_class_array);
  thread_class_array = nullptr;
  thread_class_dirty_count = thread_class_allocated_count = 0;
  thread_class_max = 0;
}

/**
  Initialize the table share buffer.
  @param table_share_sizing           max number of table share
  @return 0 on success
*/
int init_table_share(uint table_share_sizing) {
  if (global_table_share_container.init(table_share_sizing)) {
    return 1;
  }

  return 0;
}

/**
  Initialize the meter class buffer.
  @param meter_class_sizing          max number of meter class
  @return 0 on success
*/
int init_meter_class(uint meter_class_sizing) {
  int result = 0;
  meter_class_dirty_count = meter_class_allocated_count = 0;
  meter_class_max = meter_class_sizing;
  meter_class_lost = 0;

  if (meter_class_max > 0) {
    meter_class_array = PFS_MALLOC_ARRAY(
        &builtin_memory_meter_class, meter_class_max, sizeof(PFS_meter_class),
        PFS_meter_class, MYF(MY_ZEROFILL));
    if (unlikely(meter_class_array == nullptr)) {
      meter_class_max = 0;
      result = 1;
    }
  } else {
    meter_class_array = nullptr;
  }

  return result;
}

/** Cleanup the meter class buffers. */
void cleanup_meter_class() {
  if (meter_class_array != nullptr) {
    for (unsigned int i = 0; i < meter_class_max; i++) {
      my_free(meter_class_array[i].m_documentation);
      PFS_FREE_ARRAY(&builtin_memory_meter_class, metric_class_max,
                     sizeof(PFS_metric_key), meter_class_array[i].m_metrics);
      meter_class_array[i].m_metrics = nullptr;
      meter_class_array[i].m_metrics_size = 0;
    }
  }

  PFS_FREE_ARRAY(&builtin_memory_meter_class, meter_class_max,
                 sizeof(PFS_meter_class), meter_class_array);
  meter_class_array = nullptr;
  meter_class_dirty_count = meter_class_allocated_count = 0;
  meter_class_max = 0;
}

/**
  Initialize the metric class buffer.
  @param metric_class_sizing          max number of metric class
  @return 0 on success
*/
int init_metric_class(uint metric_class_sizing) {
  int result = 0;
  metric_class_dirty_count = metric_class_allocated_count = 0;
  metric_class_max = metric_class_sizing;
  metric_class_lost = 0;

  if (metric_class_max > 0) {
    metric_class_array = PFS_MALLOC_ARRAY(
        &builtin_memory_metric_class, metric_class_max,
        sizeof(PFS_metric_class), PFS_metric_class, MYF(MY_ZEROFILL));
    if (unlikely(metric_class_array == nullptr)) {
      metric_class_max = 0;
      result = 1;
    }
  } else {
    metric_class_array = nullptr;
  }

  return result;
}

/** Cleanup the metric class buffers. */
void cleanup_metric_class() {
  if (metric_class_array != nullptr) {
    for (unsigned int i = 0; i < metric_class_max; i++) {
      my_free(metric_class_array[i].m_documentation);
    }
  }

  PFS_FREE_ARRAY(&builtin_memory_metric_class, metric_class_max,
                 sizeof(PFS_metric_class), metric_class_array);
  metric_class_array = nullptr;
  metric_class_dirty_count = metric_class_allocated_count = 0;
  metric_class_max = 0;
}

/**
  Initialize the logger class buffer.
  @param logger_class_sizing          max number of logger class
  @return 0 on success
*/
int init_logger_class(uint logger_class_sizing) {
  int result = 0;
  logger_class_dirty_count = logger_class_allocated_count = 0;
  logger_class_max = logger_class_sizing;
  logger_class_lost = 0;

  if (logger_class_max > 0) {
    logger_class_array = PFS_MALLOC_ARRAY(
        &builtin_memory_logger_class, logger_class_max,
        sizeof(PFS_logger_class), PFS_logger_class, MYF(MY_ZEROFILL));
    if (unlikely(logger_class_array == nullptr)) {
      logger_class_max = 0;
      result = 1;
    }
  } else {
    logger_class_array = nullptr;
  }

  return result;
}

/** Cleanup the logger class buffers. */
void cleanup_logger_class() {
  PFS_FREE_ARRAY(&builtin_memory_logger_class, logger_class_max,
                 sizeof(PFS_logger_class), logger_class_array);
  logger_class_array = nullptr;
  logger_class_dirty_count = logger_class_allocated_count = 0;
  logger_class_max = 0;
}

/** Cleanup the table share buffers. */
void cleanup_table_share() { global_table_share_container.cleanup(); }

/** get_key function for @c table_share_hash. */
static const uchar *table_share_hash_get_key(const uchar *entry,
                                             size_t *length) {
  const PFS_table_share *const *typed_entry;
  const PFS_table_share *share;
  const void *result;
  typed_entry = reinterpret_cast<const PFS_table_share *const *>(entry);
  assert(typed_entry != nullptr);
  share = *typed_entry;
  assert(share != nullptr);
  *length = sizeof(share->m_key);
  result = &share->m_key;
  return reinterpret_cast<const uchar *>(result);
}

static uint table_share_hash_func(const LF_HASH *, const uchar *key,
                                  size_t key_len [[maybe_unused]]) {
  const PFS_table_share_key *share_key;
  uint64 nr1;
  uint64 nr2;

  assert(key_len == sizeof(PFS_table_share_key));
  share_key = reinterpret_cast<const PFS_table_share_key *>(key);
  assert(share_key != nullptr);

  nr1 = share_key->m_type;
  nr2 = 0;

  share_key->m_schema_name.hash(&nr1, &nr2);

  share_key->m_table_name.hash(&nr1, &nr2);

  return nr1;
}

static int table_share_hash_cmp_func(const uchar *key1,
                                     size_t key_len1 [[maybe_unused]],
                                     const uchar *key2,
                                     size_t key_len2 [[maybe_unused]]) {
  const PFS_table_share_key *share_key1;
  const PFS_table_share_key *share_key2;
  int cmp;

  assert(key_len1 == sizeof(PFS_table_share_key));
  assert(key_len2 == sizeof(PFS_table_share_key));
  share_key1 = reinterpret_cast<const PFS_table_share_key *>(key1);
  share_key2 = reinterpret_cast<const PFS_table_share_key *>(key2);
  assert(share_key1 != nullptr);
  assert(share_key2 != nullptr);

  if (share_key1->m_type > share_key2->m_type) {
    return +1;
  }

  if (share_key1->m_type < share_key2->m_type) {
    return -1;
  }

  cmp = share_key1->m_schema_name.sort(&share_key2->m_schema_name);
  if (cmp != 0) {
    return cmp;
  }

  cmp = share_key1->m_table_name.sort(&share_key2->m_table_name);
  return cmp;
}

/** Initialize the table share hash table. */
int init_table_share_hash(const PFS_global_param *param) {
  if ((!table_share_hash_inited) && (param->m_table_share_sizing != 0)) {
    lf_hash_init3(&table_share_hash, sizeof(PFS_table_share *), LF_HASH_UNIQUE,
                  table_share_hash_get_key, table_share_hash_func,
                  table_share_hash_cmp_func, nullptr, nullptr, nullptr);
    table_share_hash_inited = true;
  }
  return 0;
}

/** Cleanup the table share hash table. */
void cleanup_table_share_hash() {
  if (table_share_hash_inited) {
    lf_hash_destroy(&table_share_hash);
    table_share_hash_inited = false;
  }
}

/**
  Get the hash pins for @sa table_share_hash.
  @param thread The running thread.
  @returns The LF_HASH pins for the thread.
*/
static LF_PINS *get_table_share_hash_pins(PFS_thread *thread) {
  if (unlikely(thread->m_table_share_hash_pins == nullptr)) {
    if (!table_share_hash_inited) {
      return nullptr;
    }
    thread->m_table_share_hash_pins = lf_hash_get_pins(&table_share_hash);
  }
  return thread->m_table_share_hash_pins;
}

/**
  Set a table share hash key.
  @param [out] key The key to populate.
  @param temporary True for TEMPORARY TABLE.
  @param schema_name The table schema name.
  @param schema_name_length The table schema name length.
  @param table_name The table name.
  @param table_name_length The table name length.
*/
static void set_table_share_key(PFS_table_share_key *key, bool temporary,
                                const char *schema_name,
                                size_t schema_name_length,
                                const char *table_name,
                                size_t table_name_length) {
  assert(schema_name_length <= NAME_LEN);
  assert(table_name_length <= NAME_LEN);

  key->m_type = (temporary ? OBJECT_TYPE_TEMPORARY_TABLE : OBJECT_TYPE_TABLE);

  key->m_schema_name.set(schema_name, schema_name_length);

  key->m_table_name.set(table_name, table_name_length);
}

/**
  Find an existing table share lock instrumentation.
  @return a table share lock.
*/
PFS_table_share_lock *PFS_table_share::find_lock_stat() const {
  const auto *that = const_cast<PFS_table_share *>(this);
  return that->m_race_lock_stat.load();
}

/**
  Find or create a table share lock instrumentation.
  @return a table share lock, or NULL.
*/
PFS_table_share_lock *PFS_table_share::find_or_create_lock_stat() {
  PFS_table_share_lock *pfs = this->m_race_lock_stat.load();
  if (pfs != nullptr) {
    return pfs;
  }

  /* (2) Create a lock stat */
  PFS_table_share_lock *new_pfs = create_table_share_lock_stat();
  if (new_pfs == nullptr) {
    return nullptr;
  }
  new_pfs->m_owner = this;

  /* (3) Atomic CAS */
  if (atomic_compare_exchange_strong(&this->m_race_lock_stat, &pfs, new_pfs)) {
    /* Ok. */
    return new_pfs;
  }

  /* Collision with another thread that also executed (2) and (3). */
  release_table_share_lock_stat(new_pfs);

  return pfs;
}

/** Destroy a table share lock instrumentation. */
void PFS_table_share::destroy_lock_stat() {
  PFS_table_share_lock *new_ptr = nullptr;
  PFS_table_share_lock *old_ptr = this->m_race_lock_stat.exchange(new_ptr);
  if (old_ptr != nullptr) {
    release_table_share_lock_stat(old_ptr);
  }
}

/**
  Find an existing table share index instrumentation.
  @return a table share index
*/
PFS_table_share_index *PFS_table_share::find_index_stat(uint index) const {
  assert(index <= MAX_INDEXES);

  return this->m_race_index_stat[index].load();
}

/**
  Find or create a table share index instrumentation.
  @param server_share the server TABLE_SHARE structure
  @param index the index
  @return a table share index, or NULL
*/
PFS_table_share_index *PFS_table_share::find_or_create_index_stat(
    const TABLE_SHARE *server_share, uint index) {
  assert(index <= MAX_INDEXES);

  /* (1) Atomic Load */
  PFS_table_share_index *pfs = this->m_race_index_stat[index].load();
  if (pfs != nullptr) {
    return pfs;
  }

  /* (2) Create an index stat */
  PFS_table_share_index *new_pfs =
      create_table_share_index_stat(server_share, index);
  if (new_pfs == nullptr) {
    return nullptr;
  }
  new_pfs->m_owner = this;

  /* (3) Atomic CAS */
  if (atomic_compare_exchange_strong(&this->m_race_index_stat[index], &pfs,
                                     new_pfs)) {
    /* Ok. */
    return new_pfs;
  }

  /* Collision with another thread that also executed (2) and (3). */
  release_table_share_index_stat(new_pfs);

  return pfs;
}

/** Destroy table share index instrumentation. */
void PFS_table_share::destroy_index_stats() {
  for (uint index = 0; index <= MAX_INDEXES; index++) {
    PFS_table_share_index *new_ptr = nullptr;
    PFS_table_share_index *old_ptr =
        this->m_race_index_stat[index].exchange(new_ptr);
    if (old_ptr != nullptr) {
      release_table_share_index_stat(old_ptr);
    }
  }
}

void PFS_table_share::refresh_setup_object_flags(PFS_thread *thread) {
  lookup_setup_object_table(thread, OBJECT_TYPE_TABLE, &m_key.m_schema_name,
                            &m_key.m_table_name, &m_enabled, &m_timed);
}

/**
  Initialize the table lock stat buffer.
  @param table_stat_sizing           max number of table lock statistics
  @return 0 on success
*/
int init_table_share_lock_stat(uint table_stat_sizing) {
  if (global_table_share_lock_container.init(table_stat_sizing)) {
    return 1;
  }

  return 0;
}

/**
  Create a table share lock instrumentation.
  @return table share lock instrumentation, or NULL
*/
PFS_table_share_lock *create_table_share_lock_stat() {
  pfs_dirty_state dirty_state;

  /* Create a new record in table stat array. */
  PFS_table_share_lock *pfs =
      global_table_share_lock_container.allocate(&dirty_state);
  if (pfs != nullptr) {
    /* Reset the stats. */
    pfs->m_stat.reset();

    /* Use this stat buffer. */
    pfs->m_lock.dirty_to_allocated(&dirty_state);
  }

  return pfs;
}

/** Release a table share lock instrumentation. */
void release_table_share_lock_stat(PFS_table_share_lock *pfs) {
  pfs->m_owner = nullptr;
  global_table_share_lock_container.deallocate(pfs);
}

/** Cleanup the table stat buffers. */
void cleanup_table_share_lock_stat() {
  global_table_share_lock_container.cleanup();
}

/**
  Initialize table index stat buffer.
  @param index_stat_sizing           max number of index statistics
  @return 0 on success
*/
int init_table_share_index_stat(uint index_stat_sizing) {
  if (global_table_share_index_container.init(index_stat_sizing)) {
    return 1;
  }

  return 0;
}

/**
  Create a table share index instrumentation.
  @return table share index instrumentation, or NULL
*/
PFS_table_share_index *create_table_share_index_stat(
    const TABLE_SHARE *server_share, uint server_index) {
  assert((server_share != nullptr) || (server_index == MAX_INDEXES));

  pfs_dirty_state dirty_state;

  /* Create a new record in index stat array. */
  PFS_table_share_index *pfs =
      global_table_share_index_container.allocate(&dirty_state);
  if (pfs != nullptr) {
    if (server_index == MAX_INDEXES) {
      pfs->m_key.m_name_length = 0;
    } else {
      const KEY *key_info = server_share->key_info + server_index;
      const size_t len = strlen(key_info->name);

      memcpy(pfs->m_key.m_name, key_info->name, len);
      pfs->m_key.m_name_length = len;
    }

    /* Reset the stats. */
    pfs->m_stat.reset();

    /* Use this stat buffer. */
    pfs->m_lock.dirty_to_allocated(&dirty_state);
  }

  return pfs;
}

/** Release a table share index instrumentation. */
void release_table_share_index_stat(PFS_table_share_index *pfs) {
  pfs->m_owner = nullptr;
  global_table_share_index_container.deallocate(pfs);
}

/** Cleanup the table stat buffers. */
void cleanup_table_share_index_stat() {
  global_table_share_index_container.cleanup();
}

/**
  Initialize the file class buffer.
  @param file_class_sizing            max number of file class
  @return 0 on success
*/
int init_file_class(uint file_class_sizing) {
  file_class_dirty_count = file_class_allocated_count = 0;
  file_class_max = file_class_sizing;
  file_class_lost = 0;

  if (file_class_max > 0) {
    file_class_array = PFS_MALLOC_ARRAY(&builtin_memory_file_class,
                                        file_class_max, sizeof(PFS_file_class),
                                        PFS_file_class, MYF(MY_ZEROFILL));
    if (unlikely(file_class_array == nullptr)) {
      file_class_max = 0;
      return 1;
    }
  } else {
    file_class_array = nullptr;
  }

  return 0;
}

/** Cleanup the file class buffers. */
void cleanup_file_class() {
  if (file_class_array != nullptr) {
    for (unsigned int i = 0; i < file_class_max; i++) {
      my_free(file_class_array[i].m_documentation);
    }
  }

  PFS_FREE_ARRAY(&builtin_memory_file_class, file_class_max,
                 sizeof(PFS_file_class), file_class_array);
  file_class_array = nullptr;
  file_class_dirty_count = file_class_allocated_count = 0;
  file_class_max = 0;
}

/**
  Initialize the stage class buffer.
  @param stage_class_sizing            max number of stage class
  @return 0 on success
*/
int init_stage_class(uint stage_class_sizing) {
  stage_class_dirty_count = stage_class_allocated_count = 0;
  stage_class_max = stage_class_sizing;
  stage_class_lost = 0;

  if (stage_class_max > 0) {
    stage_class_array = PFS_MALLOC_ARRAY(
        &builtin_memory_stage_class, stage_class_max, sizeof(PFS_stage_class),
        PFS_stage_class, MYF(MY_ZEROFILL));
    if (unlikely(stage_class_array == nullptr)) {
      stage_class_max = 0;
      return 1;
    }
  } else {
    stage_class_array = nullptr;
  }

  return 0;
}

/** Cleanup the stage class buffers. */
void cleanup_stage_class() {
  if (stage_class_array != nullptr) {
    for (unsigned int i = 0; i < stage_class_max; i++) {
      my_free(stage_class_array[i].m_documentation);
    }
  }

  PFS_FREE_ARRAY(&builtin_memory_stage_class, stage_class_max,
                 sizeof(PFS_stage_class), stage_class_array);
  stage_class_array = nullptr;
  stage_class_dirty_count = stage_class_allocated_count = 0;
  stage_class_max = 0;
}

/**
  Initialize the statement class buffer.
  @param statement_class_sizing            max number of statement class
  @return 0 on success
*/
int init_statement_class(uint statement_class_sizing) {
  statement_class_dirty_count = statement_class_allocated_count = 0;
  statement_class_max = statement_class_sizing;
  statement_class_lost = 0;

  if (statement_class_max > 0) {
    statement_class_array = PFS_MALLOC_ARRAY(
        &builtin_memory_statement_class, statement_class_max,
        sizeof(PFS_statement_class), PFS_statement_class, MYF(MY_ZEROFILL));
    if (unlikely(statement_class_array == nullptr)) {
      statement_class_max = 0;
      return 1;
    }
  } else {
    statement_class_array = nullptr;
  }

  return 0;
}

/** Cleanup the statement class buffers. */
void cleanup_statement_class() {
  if (statement_class_array != nullptr) {
    for (unsigned int i = 0; i < statement_class_max; i++) {
      my_free(statement_class_array[i].m_documentation);
    }
  }

  PFS_FREE_ARRAY(&builtin_memory_statement_class, statement_class_max,
                 sizeof(PFS_statement_class), statement_class_array);
  statement_class_array = nullptr;
  statement_class_dirty_count = statement_class_allocated_count = 0;
  statement_class_max = 0;
}

/**
  Initialize the socket class buffer.
  @param socket_class_sizing            max number of socket class
  @return 0 on success
*/
int init_socket_class(uint socket_class_sizing) {
  socket_class_dirty_count = socket_class_allocated_count = 0;
  socket_class_max = socket_class_sizing;
  socket_class_lost = 0;

  if (socket_class_max > 0) {
    socket_class_array = PFS_MALLOC_ARRAY(
        &builtin_memory_socket_class, socket_class_max,
        sizeof(PFS_socket_class), PFS_socket_class, MYF(MY_ZEROFILL));
    if (unlikely(socket_class_array == nullptr)) {
      socket_class_max = 0;
      return 1;
    }
  } else {
    socket_class_array = nullptr;
  }

  return 0;
}

/** Cleanup the socket class buffers. */
void cleanup_socket_class() {
  if (socket_class_array != nullptr) {
    for (unsigned int i = 0; i < socket_class_max; i++) {
      my_free(socket_class_array[i].m_documentation);
    }
  }

  PFS_FREE_ARRAY(&builtin_memory_socket_class, socket_class_max,
                 sizeof(PFS_socket_class), socket_class_array);
  socket_class_array = nullptr;
  socket_class_dirty_count = socket_class_allocated_count = 0;
  socket_class_max = 0;
}

/**
  Initialize the memory class buffer.
  @param memory_class_sizing            max number of memory class
  @return 0 on success
*/
int init_memory_class(uint memory_class_sizing) {
  memory_class_dirty_count = memory_class_allocated_count = 0;
  memory_class_max = memory_class_sizing;
  memory_class_lost = 0;

  if (memory_class_max > 0) {
    memory_class_array = PFS_MALLOC_ARRAY(
        &builtin_memory_memory_class, memory_class_max,
        sizeof(PFS_memory_class), PFS_memory_class, MYF(MY_ZEROFILL));
    if (unlikely(memory_class_array.load() == nullptr)) {
      memory_class_max = 0;
      return 1;
    }
  } else {
    memory_class_array = nullptr;
  }

  return 0;
}

/** Cleanup the memory class buffers. */
void cleanup_memory_class() {
  if (memory_class_array.load() != nullptr) {
    for (unsigned int i = 0; i < memory_class_max; i++) {
      my_free(memory_class_array[i].m_documentation);
    }
  }

  PFS_FREE_ARRAY(&builtin_memory_memory_class, memory_class_max,
                 sizeof(PFS_memory_class), memory_class_array);
  memory_class_array = nullptr;
  memory_class_dirty_count = memory_class_allocated_count = 0;
  memory_class_max = 0;
}

static void init_instr_class(PFS_instr_class *klass, const char *name,
                             uint name_length, int flags, int volatility,
                             const char *documentation,
                             PFS_class_type class_type) {
  assert(name_length <= PFS_MAX_INFO_NAME_LENGTH);
  memset(klass, 0, sizeof(PFS_instr_class));
  klass->m_name.set(class_type, name, name_length);
  klass->m_flags = flags;
  klass->m_enforced_flags = 0;

  /*
    For memory instruments,
    if memory quotas are suggested by default
    by the instrumentation (in the code),
    then enforce memory quotas (in runtime)
  */
  if (klass->m_flags & PSI_FLAG_MEM_COLLECT) {
    klass->m_enforced_flags |= PSI_FLAG_MEM_COLLECT;
  }

  klass->m_volatility = volatility;
  klass->m_enabled = true;
  klass->m_timed = true;
  klass->m_type = class_type;

  klass->m_documentation = nullptr;
  if (documentation != nullptr) {
    /* PSI_DOCUMENT_ME is an empty string. */
    if (documentation[0] != '\0') {
      klass->m_documentation =
          my_strdup(PSI_NOT_INSTRUMENTED, documentation, 0);
    }
  }
}

/**
  Set user-defined configuration values for an instrument.
*/
static void configure_instr_class(PFS_instr_class *entry) {
  uint match_length = 0; /* length of matching pattern */

  // May be NULL in unit tests
  if (pfs_instr_config_array == nullptr) {
    return;
  }
  Pfs_instr_config_array::iterator it = pfs_instr_config_array->begin();
  for (; it != pfs_instr_config_array->end(); ++it) {
    const PFS_instr_config *e = *it;

    /**
      Compare class name to all configuration entries. In case of multiple
      matches, the longer specification wins. For example, the pattern
      'ABC/DEF/GHI=ON' has precedence over 'ABC/DEF/%=OFF' regardless of
      position within the configuration file or command line.

      Consecutive wildcards affect the count.
    */
    if (!my_wildcmp(&my_charset_latin1, entry->m_name.str(),
                    entry->m_name.str() + entry->m_name.length(), e->m_name,
                    e->m_name + e->m_name_length, '\\', '?', '%')) {
      if (e->m_name_length >= match_length) {
        entry->m_enabled = e->m_enabled;
        entry->m_timed = e->m_timed;
        match_length = std::max(e->m_name_length, match_length);
      }
    }
  }
}

/**
  Set user-defined configuration values for a meter instrument.
*/
static void configure_meter_class(PFS_meter_class *entry) {
  /* separate length of matching pattern for each property */
  uint match_length_enabled = 0;
  uint match_length_frequency = 0;

  // May be NULL in unit tests
  if (pfs_meter_config_array == nullptr) {
    return;
  }
  Pfs_meter_config_array::iterator it = pfs_meter_config_array->begin();
  for (; it != pfs_meter_config_array->end(); ++it) {
    const PFS_meter_config *e = *it;

    /**
      Compare class name to all configuration entries. In case of multiple
      matches, the longer specification wins. For example, the pattern
      'ABC/DEF/GHI=ON' has precedence over 'ABC/DEF/%=OFF' regardless of
      position within the configuration file or command line.

      Consecutive wildcards affect the count.
    */

    // input pattern and P_S.setup_meter 'name' field do not contain instrument
    // prefix
    const size_t pfx_len = strlen("meter/");
    const char *entry_str = entry->m_name.str() + pfx_len;
    const size_t entry_len = entry->m_name.length() - pfx_len;

    if (!my_wildcmp(&my_charset_latin1, entry_str, entry_str + entry_len,
                    e->m_name, e->m_name + e->m_name_length, '\\', '?', '%')) {
      if (e->m_enabled_set) {
        if (e->m_name_length >= match_length_enabled) {
          entry->m_enabled = e->m_enabled;
          match_length_enabled =
              std::max(e->m_name_length, match_length_enabled);
        }
      }

      if (e->m_frequency_set) {
        if (e->m_name_length >= match_length_frequency) {
          entry->m_frequency = e->m_frequency;
          match_length_frequency =
              std::max(e->m_name_length, match_length_frequency);
        }
      }
    }
  }
}

/**
  Set user-defined configuration values for a logger instrument.
*/
static void configure_logger_class(PFS_logger_class *entry) {
  uint match_length = 0; /* length of matching pattern */

  // May be NULL in unit tests
  if (pfs_logger_config_array == nullptr) {
    return;
  }
  Pfs_logger_config_array::iterator it = pfs_logger_config_array->begin();
  for (; it != pfs_logger_config_array->end(); ++it) {
    const PFS_logger_config *e = *it;

    /**
      Compare class name to all configuration entries. In case of multiple
      matches, the longer specification wins. For example, the pattern
      'ABC/DEF/GHI=level:INFO' has precedence over 'ABC/DEF/%=level:INFO'
      regardless of position within the configuration file or command line.

      Consecutive wildcards affect the count.
    */
    if (!my_wildcmp(&my_charset_latin1, entry->m_name.str(),
                    entry->m_name.str() + entry->m_name.length(), e->m_name,
                    e->m_name + e->m_name_length, '\\', '?', '%')) {
      if (e->m_name_length >= match_length) {
        entry->m_level = e->m_level;
        if (g_telemetry_log == nullptr)
          entry->m_effective_level = TLOG_NONE;
        else
          entry->m_effective_level = e->m_level;
        match_length = std::max(e->m_name_length, match_length);
      }
    }
  }
}

#define REGISTER_CLASS_BODY_PART(INDEX, ARRAY, MAX, NAME, NAME_LENGTH) \
  for (INDEX = 0; INDEX < MAX; ++INDEX) {                              \
    entry = &ARRAY[INDEX];                                             \
    if ((entry->m_name.length() == NAME_LENGTH) &&                     \
        (strncmp(entry->m_name.str(), NAME, NAME_LENGTH) == 0)) {      \
      assert(entry->m_flags == info->m_flags);                         \
      return (INDEX + 1);                                              \
    }                                                                  \
  }

// Changes compared to above version:
// - additionally checks for valid key
// - return UINT_MAX key on error
#define REGISTER_CLASS_BODY_PART_V2(INDEX, ARRAY, MAX, NAME, NAME_LENGTH) \
  for (INDEX = 0; INDEX < MAX; ++INDEX) {                                 \
    entry = &ARRAY[INDEX];                                                \
    if ((entry->m_key > 0) && (entry->m_name.length() == NAME_LENGTH) &&  \
        (strncmp(entry->m_name.str(), NAME, NAME_LENGTH) == 0)) {         \
      assert(entry->m_flags == info->m_flags);                            \
      return UINT_MAX;                                                    \
    }                                                                     \
  }

/**
  Register a mutex instrumentation metadata.
  @param name                         the instrumented name
  @param name_length                  length in bytes of name
  @param info                         the instrumentation properties
  @return a mutex instrumentation key
*/
PFS_sync_key register_mutex_class(const char *name, uint name_length,
                                  PSI_mutex_info *info) {
  uint32 index;
  PFS_mutex_class *entry;

  /*
    This is a full array scan, which is not optimal.
    This is acceptable since this code is only used at startup,
    or when a plugin is loaded.
  */
  REGISTER_CLASS_BODY_PART(index, mutex_class_array, mutex_class_max, name,
                           name_length)
  /*
    Note that:
    mutex_class_dirty_count is incremented *before* an entry is added
    mutex_class_allocated_count is incremented *after* an entry is added
  */
  index = mutex_class_dirty_count++;

  if (index < mutex_class_max) {
    /*
      The instrument was not found (from a possible previous
      load / unload of a plugin), allocate it.
      This code is safe when 2 threads execute in parallel
      for different mutex classes:
      - thread 1 registering class A
      - thread 2 registering class B
      will not collide in the same mutex_class_array[index] entry.
      This code does not protect against 2 threads registering
      in parallel the same class:
      - thread 1 registering class A
      - thread 2 registering class A
      could lead to a duplicate class A entry.
      This is ok, since this case can not happen in the caller:
      - classes names are derived from a plugin name
        ('wait/synch/mutex/<plugin>/xxx')
      - 2 threads can not register concurrently the same plugin
        in INSTALL PLUGIN.
    */
    entry = &mutex_class_array[index];
    init_instr_class(entry, name, name_length, info->m_flags,
                     info->m_volatility, info->m_documentation,
                     PFS_CLASS_MUTEX);
    entry->m_mutex_stat.reset();
    entry->m_event_name_index = mutex_class_start + index;
    entry->m_singleton = nullptr;
    entry->m_enabled = false; /* disabled by default */
    entry->m_timed = false;

    entry->enforce_valid_flags(PSI_FLAG_SINGLETON);

    /* Set user-defined configuration options for this instrument */
    configure_instr_class(entry);

    /*
      Now that this entry is populated, advertise it

      Technically, there is a small race condition here:
      T0:
      mutex_class_dirty_count= 10
      mutex_class_allocated_count= 10
      T1: Thread A increment mutex_class_dirty_count to 11
      T2: Thread B increment mutex_class_dirty_count to 12
      T3: Thread A populate entry 11
      T4: Thread B populate entry 12
      T5: Thread B increment mutex_class_allocated_count to 11,
          advertise thread A incomplete record 11,
          but does not advertise thread B complete record 12
      T6: Thread A increment mutex_class_allocated_count to 12
      This has no impact, and is acceptable.
      A reader will not see record 12 for a short time.
      A reader will see an incomplete record 11 for a short time,
      which is ok: the mutex name / statistics will be temporarily
      empty/NULL/zero, but this won't cause a crash
      (mutex_class_array is initialized with MY_ZEROFILL).
    */
    ++mutex_class_allocated_count;
    return (index + 1);
  }

  /*
    Out of space, report to SHOW STATUS that
    the allocated memory was too small.
  */
  if (pfs_enabled) {
    mutex_class_lost++;
  }
  return 0;
}

/**
  Register a rwlock instrumentation metadata.
  @param name                         the instrumented name
  @param name_length                  length in bytes of name
  @param info                         the instrumentation properties
  @return a rwlock instrumentation key
*/
PFS_sync_key register_rwlock_class(const char *name, uint name_length,
                                   PSI_rwlock_info *info) {
  /* See comments in register_mutex_class */
  uint32 index;
  PFS_rwlock_class *entry;

  REGISTER_CLASS_BODY_PART(index, rwlock_class_array, rwlock_class_max, name,
                           name_length)

  index = rwlock_class_dirty_count++;

  if (index < rwlock_class_max) {
    entry = &rwlock_class_array[index];
    init_instr_class(entry, name, name_length, info->m_flags,
                     info->m_volatility, info->m_documentation,
                     PFS_CLASS_RWLOCK);
    entry->m_rwlock_stat.reset();
    entry->m_event_name_index = rwlock_class_start + index;
    entry->m_singleton = nullptr;
    entry->m_enabled = false; /* disabled by default */
    entry->m_timed = false;

    entry->enforce_valid_flags(PSI_FLAG_SINGLETON | PSI_FLAG_RWLOCK_SX |
                               PSI_FLAG_RWLOCK_PR);

    /* One of rwlock, prlock, sxlock */
    assert(((info->m_flags & PSI_FLAG_RWLOCK_SX) == 0) ||
           ((info->m_flags & PSI_FLAG_RWLOCK_PR) == 0));

    /* Set user-defined configuration options for this instrument */
    configure_instr_class(entry);
    ++rwlock_class_allocated_count;
    return (index + 1);
  }

  if (pfs_enabled) {
    rwlock_class_lost++;
  }
  return 0;
}

/**
  Register a condition instrumentation metadata.
  @param name                         the instrumented name
  @param name_length                  length in bytes of name
  @param info                         the instrumentation properties
  @return a condition instrumentation key
*/
PFS_sync_key register_cond_class(const char *name, uint name_length,
                                 PSI_cond_info *info) {
  /* See comments in register_mutex_class */
  uint32 index;
  PFS_cond_class *entry;

  REGISTER_CLASS_BODY_PART(index, cond_class_array, cond_class_max, name,
                           name_length)

  index = cond_class_dirty_count++;

  if (index < cond_class_max) {
    entry = &cond_class_array[index];
    init_instr_class(entry, name, name_length, info->m_flags,
                     info->m_volatility, info->m_documentation, PFS_CLASS_COND);
    entry->m_event_name_index = cond_class_start + index;
    entry->m_singleton = nullptr;
    entry->m_enabled = false; /* disabled by default */
    entry->m_timed = false;

    entry->enforce_valid_flags(PSI_FLAG_SINGLETON);

    /* Set user-defined configuration options for this instrument */
    configure_instr_class(entry);
    ++cond_class_allocated_count;
    return (index + 1);
  }

  if (pfs_enabled) {
    cond_class_lost++;
  }
  return 0;
}

#define FIND_CLASS_BODY(KEY, COUNT, ARRAY) \
  if ((KEY == 0) || (KEY > COUNT)) {       \
    return NULL;                           \
  }                                        \
  return &ARRAY[KEY - 1]

// Additionally check if this slot has valid key
#define FIND_CLASS_BODY_V2(KEY, COUNT, ARRAY)                       \
  if ((KEY == 0) || (KEY > COUNT) || (ARRAY[KEY - 1].m_key == 0)) { \
    return NULL;                                                    \
  }                                                                 \
  return &ARRAY[KEY - 1]

/**
  Find a mutex instrumentation class by key.
  @param key                          the instrument key
  @return the instrument class, or NULL
*/
PFS_mutex_class *find_mutex_class(PFS_sync_key key) {
  FIND_CLASS_BODY(key, mutex_class_allocated_count, mutex_class_array);
}

PFS_mutex_class *sanitize_mutex_class(PFS_mutex_class *unsafe) {
  SANITIZE_ARRAY_BODY(PFS_mutex_class, mutex_class_array, mutex_class_max,
                      unsafe);
}

/**
  Find a rwlock instrumentation class by key.
  @param key                          the instrument key
  @return the instrument class, or NULL
*/
PFS_rwlock_class *find_rwlock_class(PFS_sync_key key) {
  FIND_CLASS_BODY(key, rwlock_class_allocated_count, rwlock_class_array);
}

PFS_rwlock_class *sanitize_rwlock_class(PFS_rwlock_class *unsafe) {
  SANITIZE_ARRAY_BODY(PFS_rwlock_class, rwlock_class_array, rwlock_class_max,
                      unsafe);
}

/**
  Find a condition instrumentation class by key.
  @param key                          the instrument key
  @return the instrument class, or NULL
*/
PFS_cond_class *find_cond_class(PFS_sync_key key) {
  FIND_CLASS_BODY(key, cond_class_allocated_count, cond_class_array);
}

PFS_cond_class *sanitize_cond_class(PFS_cond_class *unsafe) {
  SANITIZE_ARRAY_BODY(PFS_cond_class, cond_class_array, cond_class_max, unsafe);
}

/**
  Find a meter instrumentation class by key.
  @param key                          the instrument key
  @return the instrument class, or NULL
*/
PFS_meter_class *find_meter_class(PSI_meter_key key) {
  FIND_CLASS_BODY_V2(key, meter_class_max, meter_class_array);
}

PFS_meter_class *sanitize_meter_class(PFS_meter_class *unsafe) {
  SANITIZE_ARRAY_BODY(PFS_meter_class, meter_class_array, meter_class_max,
                      unsafe);
}

/**
  Find a metric instrumentation class by key.
  @param key                          the instrument key
  @return the instrument class, or NULL
*/
PFS_metric_class *find_metric_class(PSI_metric_key key) {
  FIND_CLASS_BODY_V2(key, metric_class_max, metric_class_array);
}

PFS_metric_class *sanitize_metric_class(PFS_metric_class *unsafe) {
  SANITIZE_ARRAY_BODY(PFS_metric_class, metric_class_array, metric_class_max,
                      unsafe);
}

/**
  Find a logger instrumentation class by key.
  @param key                          the instrument key
  @return the instrument class, or NULL
*/
PFS_logger_class *find_logger_class(PSI_logger_key key) {
  FIND_CLASS_BODY_V2(key, logger_class_max, logger_class_array);
}

/**
  Register a thread instrumentation metadata.
  @param name                         the instrumented name
  @param name_length                  length in bytes of name
  @param info                         the instrumentation properties
  @return a thread instrumentation key
*/
PFS_thread_key register_thread_class(const char *name, uint name_length,
                                     PSI_thread_info *info) {
  assert(info != nullptr);
  assert(info->m_os_name != nullptr);

  /* See comments in register_mutex_class */
  uint32 index;
  PFS_thread_class *entry;

  REGISTER_CLASS_BODY_PART(index, thread_class_array, thread_class_max, name,
                           name_length);

  index = thread_class_dirty_count++;

  if (index < thread_class_max) {
    entry = &thread_class_array[index];

    init_instr_class(entry, name, name_length, info->m_flags,
                     info->m_volatility, info->m_documentation,
                     PFS_CLASS_THREAD);
    entry->m_singleton = nullptr;
    entry->m_history = true;

    entry->enforce_valid_flags(PSI_FLAG_SINGLETON | PSI_FLAG_USER |
                               PSI_FLAG_THREAD_SYSTEM | PSI_FLAG_AUTO_SEQNUM |
                               PSI_FLAG_NO_SEQNUM);

    configure_instr_class(entry);
    ++thread_class_allocated_count;

    entry->m_seqnum.store(1);

    if (entry->has_seqnum()) {
      /*
        Ensure room for "-%d" suffix with 2 digits minimum,
        so that:
        - the "-%d" format fits into the class
        - the "-NN" suffix fits into the instance
      */
      assert(strlen(info->m_os_name) < PFS_MAX_OS_NAME_LENGTH - 3);

      (void)snprintf(entry->m_os_name, PFS_MAX_OS_NAME_LENGTH, "%s-%%d",
                     info->m_os_name);
    } else {
      assert(strlen(info->m_os_name) < PFS_MAX_OS_NAME_LENGTH);
      strncpy(entry->m_os_name, info->m_os_name, PFS_MAX_OS_NAME_LENGTH);
    }
    entry->m_os_name[PFS_MAX_OS_NAME_LENGTH - 1] = '\0';

    return (index + 1);
  }

  if (pfs_enabled) {
    thread_class_lost++;
  }
  return 0;
}

/**
  Find a thread instrumentation class by key.
  @param key                          the instrument key
  @return the instrument class, or NULL
*/
PFS_thread_class *find_thread_class(PFS_sync_key key) {
  FIND_CLASS_BODY(key, thread_class_allocated_count, thread_class_array);
}

PFS_thread_class *sanitize_thread_class(PFS_thread_class *unsafe) {
  SANITIZE_ARRAY_BODY(PFS_thread_class, thread_class_array, thread_class_max,
                      unsafe);
}

/**
  Register a file instrumentation metadata.
  @param name                         the instrumented name
  @param name_length                  length in bytes of name
  @param info                         the instrumentation properties
  @return a file instrumentation key
*/
PFS_file_key register_file_class(const char *name, uint name_length,
                                 PSI_file_info *info) {
  /* See comments in register_mutex_class */
  uint32 index;
  PFS_file_class *entry;

  REGISTER_CLASS_BODY_PART(index, file_class_array, file_class_max, name,
                           name_length)

  index = file_class_dirty_count++;

  if (index < file_class_max) {
    entry = &file_class_array[index];
    init_instr_class(entry, name, name_length, info->m_flags,
                     info->m_volatility, info->m_documentation, PFS_CLASS_FILE);
    entry->m_event_name_index = file_class_start + index;
    entry->m_singleton = nullptr;
    entry->m_enabled = true; /* enabled by default */
    entry->m_timed = true;

    entry->enforce_valid_flags(PSI_FLAG_SINGLETON);

    /* Set user-defined configuration options for this instrument */
    configure_instr_class(entry);
    ++file_class_allocated_count;

    return (index + 1);
  }

  if (pfs_enabled) {
    file_class_lost++;
  }
  return 0;
}

/**
  Register a stage instrumentation metadata.
  @param name                         the instrumented name
  @param prefix_length                length in bytes of the name prefix
  @param name_length                  length in bytes of name
  @param info                         the instrumentation properties
  @return a stage instrumentation key
*/
PFS_stage_key register_stage_class(const char *name, uint prefix_length,
                                   uint name_length, PSI_stage_info *info) {
  /* See comments in register_mutex_class */
  uint32 index;
  PFS_stage_class *entry;

  REGISTER_CLASS_BODY_PART(index, stage_class_array, stage_class_max, name,
                           name_length)

  index = stage_class_dirty_count++;

  if (index < stage_class_max) {
    entry = &stage_class_array[index];
    init_instr_class(entry, name, name_length, info->m_flags,
                     0, /* stages have no volatility */
                     info->m_documentation, PFS_CLASS_STAGE);
    entry->m_prefix_length = prefix_length;
    entry->m_event_name_index = index;

    entry->enforce_valid_flags(PSI_FLAG_STAGE_PROGRESS);

    if (entry->is_progress()) {
      /* Stages with progress information are enabled and timed by default */
      entry->m_enabled = true;
      entry->m_timed = true;
    } else {
      /* Stages without progress information are disabled by default */
      entry->m_enabled = false;
      entry->m_timed = false;
    }

    /* Set user-defined configuration options for this instrument */
    configure_instr_class(entry);
    ++stage_class_allocated_count;

    return (index + 1);
  }

  if (pfs_enabled) {
    stage_class_lost++;
  }
  return 0;
}

/**
  Register a statement instrumentation metadata.
  @param name                         the instrumented name
  @param name_length                  length in bytes of name
  @param info                         the instrumentation properties
  @return a statement instrumentation key
*/
PFS_statement_key register_statement_class(const char *name, uint name_length,
                                           PSI_statement_info *info) {
  /* See comments in register_mutex_class */
  uint32 index;
  PFS_statement_class *entry;

  REGISTER_CLASS_BODY_PART(index, statement_class_array, statement_class_max,
                           name, name_length)

  index = statement_class_dirty_count++;

  if (index < statement_class_max) {
    entry = &statement_class_array[index];
    init_instr_class(entry, name, name_length, info->m_flags,
                     0, /* statements have no volatility */
                     info->m_documentation, PFS_CLASS_STATEMENT);
    entry->m_event_name_index = index;

    entry->enforce_valid_flags(PSI_FLAG_MUTABLE | PSI_FLAG_DISABLED |
                               PSI_FLAG_UNTIMED);

    if (entry->is_disabled()) {
      /*
        The instrumentation is PSI_FLAG_DISABLED.
      */
      entry->m_enabled = false;
    } else {
      entry->m_enabled = true;
    }

    if (entry->is_untimed()) {
      /*
        The instrumentation is PSI_FLAG_UNTIMED.
      */
      entry->m_timed = false;
    } else {
      entry->m_timed = true;
    }

    /* Set user-defined configuration options for this instrument */
    configure_instr_class(entry);
    ++statement_class_allocated_count;

    return (index + 1);
  }

  if (pfs_enabled) {
    statement_class_lost++;
  }
  return 0;
}

/**
  Find a file instrumentation class by key.
  @param key                          the instrument key
  @return the instrument class, or NULL
*/
PFS_file_class *find_file_class(PFS_file_key key) {
  FIND_CLASS_BODY(key, file_class_allocated_count, file_class_array);
}

PFS_file_class *sanitize_file_class(PFS_file_class *unsafe) {
  SANITIZE_ARRAY_BODY(PFS_file_class, file_class_array, file_class_max, unsafe);
}

/**
  Find a stage instrumentation class by key.
  @param key                          the instrument key
  @return the instrument class, or NULL
*/
PFS_stage_class *find_stage_class(PFS_stage_key key) {
  FIND_CLASS_BODY(key, stage_class_allocated_count, stage_class_array);
}

PFS_stage_class *sanitize_stage_class(PFS_stage_class *unsafe) {
  SANITIZE_ARRAY_BODY(PFS_stage_class, stage_class_array, stage_class_max,
                      unsafe);
}

/**
  Find a statement instrumentation class by key.
  @param key                          the instrument key
  @return the instrument class, or NULL
*/
PFS_statement_class *find_statement_class(PFS_stage_key key) {
  FIND_CLASS_BODY(key, statement_class_allocated_count, statement_class_array);
}

PFS_statement_class *sanitize_statement_class(PFS_statement_class *unsafe) {
  SANITIZE_ARRAY_BODY(PFS_statement_class, statement_class_array,
                      statement_class_max, unsafe);
}

/**
  Register a socket instrumentation metadata.
  @param name                         the instrumented name
  @param name_length                  length in bytes of name
  @param info                         the instrumentation properties
  @return a socket instrumentation key
*/
PFS_socket_key register_socket_class(const char *name, uint name_length,
                                     PSI_socket_info *info) {
  /* See comments in register_mutex_class */
  uint32 index;
  PFS_socket_class *entry;

  REGISTER_CLASS_BODY_PART(index, socket_class_array, socket_class_max, name,
                           name_length)

  index = socket_class_dirty_count++;

  if (index < socket_class_max) {
    entry = &socket_class_array[index];
    init_instr_class(entry, name, name_length, info->m_flags,
                     info->m_volatility, info->m_documentation,
                     PFS_CLASS_SOCKET);
    entry->m_event_name_index = socket_class_start + index;
    entry->m_singleton = nullptr;
    entry->m_enabled = false; /* disabled by default */
    entry->m_timed = false;

    entry->enforce_valid_flags(PSI_FLAG_SINGLETON | PSI_FLAG_USER);

    /* Set user-defined configuration options for this instrument */
    configure_instr_class(entry);
    ++socket_class_allocated_count;
    return (index + 1);
  }

  if (pfs_enabled) {
    socket_class_lost++;
  }
  return 0;
}

/**
  Find a socket instrumentation class by key.
  @param key                          the instrument key
  @return the instrument class, or NULL
*/
PFS_socket_class *find_socket_class(PFS_socket_key key) {
  FIND_CLASS_BODY(key, socket_class_allocated_count, socket_class_array);
}

PFS_socket_class *sanitize_socket_class(PFS_socket_class *unsafe) {
  SANITIZE_ARRAY_BODY(PFS_socket_class, socket_class_array, socket_class_max,
                      unsafe);
}

/**
  Register a memory instrumentation metadata.
  @param name                         the instrumented name
  @param name_length                  length in bytes of name
  @param info                         the instrumentation properties
  @return a memory instrumentation key
*/
PFS_memory_key register_memory_class(const char *name, uint name_length,
                                     PSI_memory_info *info) {
  /* See comments in register_mutex_class */
  uint32 index;
  PFS_memory_class *entry;

  REGISTER_CLASS_BODY_PART(index, memory_class_array, memory_class_max, name,
                           name_length)

  index = memory_class_dirty_count++;

  if (index < memory_class_max) {
    entry = &memory_class_array[index];
    init_instr_class(entry, name, name_length, info->m_flags,
                     info->m_volatility, info->m_documentation,
                     PFS_CLASS_MEMORY);
    entry->m_event_name_index = index;

    entry->enforce_valid_flags(
        (PSI_FLAG_ONLY_GLOBAL_STAT | PSI_FLAG_MEM_COLLECT));

    /* Set user-defined configuration options for this instrument */
    configure_instr_class(entry);
    entry->m_timed = false; /* Immutable */
    ++memory_class_allocated_count;
    return (index + 1);
  }

  if (pfs_enabled) {
    memory_class_lost++;
  }
  return 0;
}

/**
  Find a memory instrumentation class by key.
  @param key                          the instrument key
  @return the instrument class, or NULL
*/
PFS_memory_class *find_memory_class(PFS_memory_key key) {
  FIND_CLASS_BODY(key, memory_class_allocated_count, memory_class_array);
}

PFS_memory_class *sanitize_memory_class(PFS_memory_class *unsafe) {
  SANITIZE_ARRAY_BODY(PFS_memory_class, memory_class_array.load(),
                      memory_class_max, unsafe);
}

/**
  Register a meter instrumentation metadata.
  @param name                         the instrumented name
  @param name_length                  length in bytes of name
  @param info                         the instrumentation properties
  @return a meter instrumentation key, 0 on error, UINT_MAX on duplicate
*/
PFS_meter_key register_meter_class(const char *name, uint name_length,
                                   PSI_meter_info_v1 *info) {
  /* See comments in register_mutex_class */
  uint32 index;
  PFS_meter_class *entry;

  REGISTER_CLASS_BODY_PART_V2(index, meter_class_array, meter_class_max, name,
                              name_length)

  // search free slot
  if (meter_class_dirty_count < meter_class_max &&
      meter_class_array[meter_class_dirty_count].m_key == 0) {
    // fast path
    index = meter_class_dirty_count;
  } else {
    index = meter_class_max;
    for (uint32 i = 0; i < meter_class_max; i++) {
      if (meter_class_array[i].m_key == 0) {
        index = i;
        break;
      }
    }
  }

  if (index < meter_class_max) {
    const PFS_meter_key key = index + 1;

    entry = &meter_class_array[index];

    // init entry lock
    pfs_dirty_state dirty_state;
    if (!entry->m_lock.free_to_dirty(&dirty_state)) {
      if (pfs_enabled) {
        meter_class_lost++;
      }
      return 0;
    }
    entry->m_lock.dirty_to_allocated(&dirty_state);

    init_instr_class(entry, name, name_length, 0,
                     0, /* statements have no volatility */
                     PSI_DOCUMENT_ME, PFS_CLASS_METRIC);
    entry->m_event_name_index = index;
    entry->m_enabled = true; /* enabled by default */
    entry->m_timed = true;

    // storage for metric keys allocated once per slot
    // on a when-needed bases (instead of doing it in init_meter_class)
    if (metric_class_max > 0 && entry->m_metrics == nullptr) {
      entry->m_metrics = PFS_MALLOC_ARRAY(
          &builtin_memory_meter_class, metric_class_max, sizeof(PFS_metric_key),
          PFS_metric_key, MYF(MY_ZEROFILL));
    }
    entry->m_metrics_size = 0;

    // copy metric source info
    entry->m_meter = info->m_meter;
    entry->m_meter_length =
        (info->m_meter == nullptr) ? 0 : strlen(info->m_meter);
    entry->m_description = info->m_description;
    entry->m_description_length =
        (info->m_description == nullptr) ? 0 : strlen(info->m_description);
    entry->m_frequency = info->m_frequency;
    entry->m_key = key;

    // update input entry (same structure used on unregister)
    info->m_key = key;

    entry->enforce_valid_flags(PSI_FLAG_MUTABLE);

    /* Set user-defined configuration options for this instrument */
    configure_meter_class(entry);
    ++meter_class_allocated_count;

    if (index == meter_class_dirty_count) ++meter_class_dirty_count;

    return key;
  }

  if (pfs_enabled) {
    meter_class_lost++;
  }
  return 0;
}

void unregister_meter_class(PSI_meter_info_v1 *info) {
  assert(info != nullptr);

  if (info->m_key == 0) return;

  const uint32 index = info->m_key - 1;
  PFS_meter_class *entry = &meter_class_array[index];

  // unregister
  entry->m_key = 0;
  info->m_key = 0;

  entry->m_lock.allocated_to_free();

  --meter_class_allocated_count;
  meter_class_dirty_count = 0;
}

uint32 meter_class_count() { return meter_class_allocated_count; }

/**
  Register a metric instrumentation metadata.
  @param name                         the instrumented name
  @param name_length                  length in bytes of name
  @param info                         the instrumentation properties
  @param meter                        group name for this metric
  @return a metric instrumentation key, 0 on error, UINT_MAX on duplicate
*/
PFS_metric_key register_metric_class(const char *name, uint name_length,
                                     PSI_metric_info_v1 *info,
                                     const char *meter) {
  /* See comments in register_mutex_class */
  uint32 index;
  PFS_metric_class *entry;

  REGISTER_CLASS_BODY_PART_V2(index, metric_class_array, metric_class_max, name,
                              name_length)

  // search free slot
  if (metric_class_dirty_count < metric_class_max &&
      metric_class_array[metric_class_dirty_count].m_key == 0) {
    // fast path
    index = metric_class_dirty_count;
  } else {
    index = metric_class_max;
    for (uint32 i = 0; i < metric_class_max; i++) {
      if (metric_class_array[i].m_key == 0) {
        index = i;
        break;
      }
    }
  }

  if (index < metric_class_max) {
    const PFS_metric_key key = index + 1;

    entry = &metric_class_array[index];

    // init entry lock
    pfs_dirty_state dirty_state;
    if (!entry->m_lock.free_to_dirty(&dirty_state)) {
      if (pfs_enabled) {
        meter_class_lost++;
      }
      return 0;
    }
    entry->m_lock.dirty_to_allocated(&dirty_state);

    init_instr_class(entry, name, name_length, 0,
                     0, /* statements have no volatility */
                     PSI_DOCUMENT_ME, PFS_CLASS_METRIC);
    entry->m_event_name_index = index;
    entry->m_enabled = true; /* enabled by default */
    entry->m_timed = true;

    // copy metric source info
    entry->m_metric = info->m_metric;
    entry->m_metric_length =
        (info->m_metric == nullptr) ? 0 : strlen(info->m_metric);
    entry->m_group = meter;
    entry->m_group_length = (meter == nullptr) ? 0 : strlen(meter);
    entry->m_unit = info->m_unit;
    entry->m_unit_length = (info->m_unit == nullptr) ? 0 : strlen(info->m_unit);
    entry->m_description = info->m_description;
    entry->m_description_length =
        (info->m_description == nullptr) ? 0 : strlen(info->m_description);
    entry->m_num_type = info->m_num_type;
    entry->m_metric_type = info->m_metric_type;
    entry->m_key = key;
    entry->m_measurement_callback = info->m_measurement_callback;
    entry->m_measurement_context = info->m_measurement_context;

    // update input entry (same structure used on unregister)
    info->m_key = key;

    entry->enforce_valid_flags(PSI_FLAG_MUTABLE);

    /* Set user-defined configuration options for this instrument */
    configure_instr_class(entry);
    ++metric_class_allocated_count;

    if (index == metric_class_dirty_count) ++metric_class_dirty_count;

    return key;
  }

  if (pfs_enabled) {
    metric_class_lost++;
  }
  return 0;
}

void unregister_metric_class(PSI_metric_info_v1 *info) {
  assert(info != nullptr);

  if (info->m_key == 0) return;

  const uint32 index = info->m_key - 1;
  PFS_metric_class *entry = &metric_class_array[index];

  // unregister
  entry->m_key = 0;
  info->m_key = 0;

  entry->m_lock.allocated_to_free();

  --metric_class_allocated_count;
  metric_class_dirty_count = 0;
}

uint32 metric_class_count() { return metric_class_allocated_count; }

/**
  Register a logger instrumentation metadata.
  @param name                   the instrumented name
  @param name_length            length in bytes of name
  @param info                   the instrumentation properties
  @return a logger instrumentation key, 0 on error, UINT_MAX on duplicate
*/
PFS_logger_key register_logger_class(const char *name, uint name_length,
                                     PSI_logger_info_v1 *info) {
  /* See comments in register_mutex_class */
  uint32 index;
  PFS_logger_class *entry;

  REGISTER_CLASS_BODY_PART_V2(index, logger_class_array, logger_class_max, name,
                              name_length)

  // search free slot
  if (logger_class_dirty_count < logger_class_max &&
      logger_class_array[logger_class_dirty_count].m_key == 0) {
    // fast path
    index = logger_class_dirty_count;
  } else {
    index = logger_class_max;
    for (uint32 i = 0; i < logger_class_max; i++) {
      if (logger_class_array[i].m_key == 0) {
        index = i;
        break;
      }
    }
  }

  if (index < logger_class_max) {
    const PFS_logger_key key = index + 1;

    entry = &logger_class_array[index];

    // init entry lock
    pfs_dirty_state dirty_state;
    if (!entry->m_lock.free_to_dirty(&dirty_state)) {
      if (pfs_enabled) {
        logger_class_lost++;
      }
      return 0;
    }
    entry->m_lock.dirty_to_allocated(&dirty_state);

    init_instr_class(entry, name, name_length, 0,
                     0, /* statements have no volatility */
                     PSI_DOCUMENT_ME, PFS_CLASS_LOGGER);
    entry->m_event_name_index = index;
    entry->m_enabled = true; /* enabled by default */
    entry->m_timed = true;

    // copy logger source info
    entry->m_logger_name_length =
        (info->m_logger_name == nullptr) ? 0 : strlen(info->m_logger_name);
    if (entry->m_logger_name_length > 0)
      memcpy(entry->m_logger_name, info->m_logger_name,
             entry->m_logger_name_length);
    entry->m_description_length =
        (info->m_description == nullptr) ? 0 : strlen(info->m_description);
    if (entry->m_description_length > 0)
      memcpy(entry->m_description, info->m_description,
             entry->m_description_length);
    entry->m_key = key;
    entry->m_level =
        OTELLogLevel::TLOG_INFO;  // default level (be conservative)
    if (g_telemetry_log == nullptr)
      entry->m_effective_level = TLOG_NONE;
    else
      entry->m_effective_level = entry->m_level;

    entry->enforce_valid_flags(0);

    /* Set user-defined configuration options for this instrument */
    configure_logger_class(entry);
    ++logger_class_allocated_count;

    if (index == logger_class_dirty_count) ++logger_class_dirty_count;

    return key;
  }

  if (pfs_enabled) {
    logger_class_lost++;
  }
  return 0;
}

void unregister_logger_class(PSI_logger_info_v1 *info) {
  assert(info != nullptr);

  if (info->m_key == nullptr || *(info->m_key) == 0) return;

  const uint32 index = *(info->m_key) - 1;
  PFS_logger_class *entry = &logger_class_array[index];

  // unregister
  entry->m_key = 0;
  *(info->m_key) = 0;

  entry->m_lock.allocated_to_free();

  --logger_class_allocated_count;
  logger_class_dirty_count = 0;
}

uint32 logger_class_count() { return logger_class_allocated_count; }

PFS_instr_class *find_table_class(uint index) {
  if (index == 1) {
    return &global_table_io_class;
  }
  if (index == 2) {
    return &global_table_lock_class;
  }
  return nullptr;
}

PFS_instr_class *sanitize_table_class(PFS_instr_class *unsafe) {
  if (likely((&global_table_io_class == unsafe) ||
             (&global_table_lock_class == unsafe))) {
    return unsafe;
  }
  return nullptr;
}

PFS_instr_class *find_idle_class(uint index) {
  if (index == 1) {
    return &global_idle_class;
  }
  return nullptr;
}

PFS_instr_class *sanitize_idle_class(PFS_instr_class *unsafe) {
  if (likely(&global_idle_class == unsafe)) {
    return unsafe;
  }
  return nullptr;
}

PFS_instr_class *find_metadata_class(uint index) {
  if (index == 1) {
    return &global_metadata_class;
  }
  return nullptr;
}

PFS_instr_class *sanitize_metadata_class(PFS_instr_class *unsafe) {
  if (likely(&global_metadata_class == unsafe)) {
    return unsafe;
  }
  return nullptr;
}

PFS_error_class *find_error_class(uint index) {
  if (index == 1) {
    return &global_error_class;
  }
  return nullptr;
}

PFS_error_class *sanitize_error_class(PFS_error_class *unsafe) {
  if (likely(&global_error_class == unsafe)) {
    return unsafe;
  }
  return nullptr;
}

PFS_transaction_class *find_transaction_class(uint index) {
  if (index == 1) {
    return &global_transaction_class;
  }
  return nullptr;
}

PFS_transaction_class *sanitize_transaction_class(
    PFS_transaction_class *unsafe) {
  if (likely(&global_transaction_class == unsafe)) {
    return unsafe;
  }
  return nullptr;
}

static int compare_keys(PFS_table_share *pfs, const TABLE_SHARE *share) {
  if (pfs->m_key_count != share->keys) {
    return 1;
  }

  uint index = 0;
  const uint key_count = share->keys;
  KEY *key_info = share->key_info;

  for (; index < key_count; key_info++, index++) {
    const PFS_table_share_index *index_stat = pfs->find_index_stat(index);
    if (index_stat != nullptr) {
      const size_t len = strlen(key_info->name);

      if (len != index_stat->m_key.m_name_length) {
        return 1;
      }

      if (memcmp(index_stat->m_key.m_name, key_info->name, len) != 0) {
        return 1;
      }
    }
  }

  return 0;
}

/**
  Find or create a table share instrumentation.
  @param thread                       the executing instrumented thread
  @param temporary                    true for TEMPORARY TABLE
  @param share                        table share
  @return a table share, or NULL
*/
PFS_table_share *find_or_create_table_share(PFS_thread *thread, bool temporary,
                                            const TABLE_SHARE *share) {
  /* See comments in register_mutex_class */
  PFS_table_share_key key;

  LF_PINS *pins = get_table_share_hash_pins(thread);
  if (unlikely(pins == nullptr)) {
    global_table_share_container.m_lost++;
    return nullptr;
  }

  const char *schema_name = share->db.str;
  const size_t schema_name_length = share->db.length;
  const char *table_name = share->table_name.str;
  const size_t table_name_length = share->table_name.length;

  set_table_share_key(&key, temporary, schema_name, schema_name_length,
                      table_name, table_name_length);

  PFS_table_share **entry;
  uint retry_count = 0;
  constexpr uint retry_max = 3;
  bool enabled = true;
  bool timed = true;
  PFS_table_share *pfs;
  pfs_dirty_state dirty_state;

search:
  entry = reinterpret_cast<PFS_table_share **>(
      lf_hash_search(&table_share_hash, pins, &key, sizeof(key)));
  if (entry && (entry != MY_LF_ERRPTR)) {
    pfs = *entry;
    pfs->inc_refcount();
    if (compare_keys(pfs, share) != 0) {
      /*
        Some DDL was detected.
        - keep the lock stats, they are unaffected
        - destroy the index stats, indexes changed.
        - adjust the expected key count
        - recreate index stats
      */
      pfs->destroy_index_stats();
      pfs->m_key_count = share->keys;
      for (uint index = 0; index < pfs->m_key_count; index++) {
        (void)pfs->find_or_create_index_stat(share, index);
      }
    }
    lf_hash_search_unpin(pins);
    return pfs;
  }

  lf_hash_search_unpin(pins);

  if (retry_count == 0) {
    lookup_setup_object_table(thread, key.m_type, &key.m_schema_name,
                              &key.m_table_name, &enabled, &timed);
    /*
      Even when enabled is false, a record is added in the dictionary:
      - It makes enabling a table already in the table cache possible,
      - It improves performances for the next time a TABLE_SHARE is reloaded
        in the table cache.
    */
  }

  pfs = global_table_share_container.allocate(&dirty_state);
  if (pfs != nullptr) {
    pfs->m_key = key;
    pfs->m_enabled = enabled;
    pfs->m_timed = timed;
    pfs->init_refcount();
    pfs->destroy_lock_stat();
    pfs->destroy_index_stats();
    pfs->m_key_count = share->keys;

    int res;
    res = lf_hash_insert(&table_share_hash, pins, &pfs);

    if (likely(res == 0)) {
      /* Create table share index stats. */
      for (uint index = 0; index < pfs->m_key_count; index++) {
        (void)pfs->find_or_create_index_stat(share, index);
      }
      pfs->m_lock.dirty_to_allocated(&dirty_state);
      return pfs;
    }

    global_table_share_container.dirty_to_free(&dirty_state, pfs);

    if (res > 0) {
      /* Duplicate insert by another thread */
      if (++retry_count > retry_max) {
        /* Avoid infinite loops */
        global_table_share_container.m_lost++;
        return nullptr;
      }
      goto search;
    }

    /* OOM in lf_hash_insert */
    global_table_share_container.m_lost++;
    return nullptr;
  }

  return nullptr;
}

void PFS_table_share::aggregate_io() {
  uint index;
  const uint safe_key_count = sanitize_index_count(m_key_count);
  PFS_table_share_index *from_stat;
  PFS_table_io_stat sum_io;

  /* Aggregate stats for each index, if any */
  for (index = 0; index < safe_key_count; index++) {
    from_stat = find_index_stat(index);
    if (from_stat != nullptr) {
      sum_io.aggregate(&from_stat->m_stat);
      from_stat->m_stat.reset();
    }
  }

  /* Aggregate stats for the table */
  from_stat = find_index_stat(MAX_INDEXES);
  if (from_stat != nullptr) {
    sum_io.aggregate(&from_stat->m_stat);
    from_stat->m_stat.reset();
  }

  /* Add this table stats to the global sink. */
  global_table_io_stat.aggregate(&sum_io);
}

void PFS_table_share::sum_io(PFS_single_stat *result, uint key_count) {
  uint index;
  PFS_table_share_index *stat;

  assert(key_count <= MAX_INDEXES);

  /* Sum stats for each index, if any */
  for (index = 0; index < key_count; index++) {
    stat = find_index_stat(index);
    if (stat != nullptr) {
      stat->m_stat.sum(result);
    }
  }

  /* Sum stats for the table */
  stat = find_index_stat(MAX_INDEXES);
  if (stat != nullptr) {
    stat->m_stat.sum(result);
  }
}

void PFS_table_share::sum_lock(PFS_single_stat *result) {
  PFS_table_share_lock *lock_stat;
  lock_stat = find_lock_stat();
  if (lock_stat != nullptr) {
    lock_stat->m_stat.sum(result);
  }
}

void PFS_table_share::sum(PFS_single_stat *result, uint key_count) {
  sum_io(result, key_count);
  sum_lock(result);
}

void PFS_table_share::aggregate_lock() {
  PFS_table_share_lock *lock_stat;
  lock_stat = find_lock_stat();
  if (lock_stat != nullptr) {
    global_table_lock_stat.aggregate(&lock_stat->m_stat);
    /* Reset lock stat. */
    lock_stat->m_stat.reset();
  }
}

void release_table_share(PFS_table_share *pfs) {
  assert(pfs->get_refcount() > 0);
  pfs->dec_refcount();
}

/**
  Drop the instrumented table share associated with a table.
  @param thread The running thread
  @param temporary True for TEMPORARY TABLE
  @param schema_name The table schema name
  @param schema_name_length The table schema name length
  @param table_name The table name
  @param table_name_length The table name length
*/
void drop_table_share(PFS_thread *thread, bool temporary,
                      const char *schema_name, uint schema_name_length,
                      const char *table_name, uint table_name_length) {
  PFS_table_share_key key;
  LF_PINS *pins = get_table_share_hash_pins(thread);
  if (unlikely(pins == nullptr)) {
    return;
  }
  set_table_share_key(&key, temporary, schema_name, schema_name_length,
                      table_name, table_name_length);
  PFS_table_share **entry;
  entry = reinterpret_cast<PFS_table_share **>(
      lf_hash_search(&table_share_hash, pins, &key, sizeof(key)));
  if (entry && (entry != MY_LF_ERRPTR)) {
    PFS_table_share *pfs = *entry;
    lf_hash_delete(&table_share_hash, pins, &pfs->m_key, sizeof(pfs->m_key));
    pfs->destroy_lock_stat();
    pfs->destroy_index_stats();

    global_table_share_container.deallocate(pfs);
  }

  lf_hash_search_unpin(pins);
}

/**
  Sanitize an unsafe table_share pointer.
  @param unsafe The possibly corrupt pointer.
  @return A valid table_safe_pointer, or NULL.
*/
PFS_table_share *sanitize_table_share(PFS_table_share *unsafe) {
  return global_table_share_container.sanitize(unsafe);
}

/** Reset the wait statistics per instrument class. */
void reset_events_waits_by_class() {
  reset_file_class_io();
  reset_socket_class_io();
  global_idle_stat.reset();
  global_table_io_stat.reset();
  global_table_lock_stat.reset();
  global_metadata_stat.reset();
}

/** Reset the I/O statistics per file class. */
void reset_file_class_io() {
  PFS_file_class *pfs = file_class_array;
  const PFS_file_class *pfs_last = file_class_array + file_class_max;

  for (; pfs < pfs_last; pfs++) {
    pfs->m_file_stat.m_io_stat.reset();
  }
}

/** Reset the I/O statistics per socket class. */
void reset_socket_class_io() {
  PFS_socket_class *pfs = socket_class_array;
  const PFS_socket_class *pfs_last = socket_class_array + socket_class_max;

  for (; pfs < pfs_last; pfs++) {
    pfs->m_socket_stat.m_io_stat.reset();
  }
}

class Proc_table_share_derived_flags
    : public PFS_buffer_processor<PFS_table_share> {
 public:
  explicit Proc_table_share_derived_flags(PFS_thread *thread)
      : m_thread(thread) {}

  void operator()(PFS_table_share *pfs) override {
    pfs->refresh_setup_object_flags(m_thread);
  }

 private:
  PFS_thread *m_thread;
};

void update_table_share_derived_flags(PFS_thread *thread) {
  Proc_table_share_derived_flags proc(thread);
  global_table_share_container.apply(proc);
}

class Proc_program_share_derived_flags
    : public PFS_buffer_processor<PFS_program> {
 public:
  explicit Proc_program_share_derived_flags(PFS_thread *thread)
      : m_thread(thread) {}

  void operator()(PFS_program *pfs) override {
    pfs->refresh_setup_object_flags(m_thread);
  }

 private:
  PFS_thread *m_thread;
};

void update_program_share_derived_flags(PFS_thread *thread) {
  Proc_program_share_derived_flags proc(thread);
  global_program_container.apply(proc);
}

ulonglong gtid_monitoring_getsystime() {
  if (pfs_enabled) {
    // Divide by 10 to get the time in microseconds
    return my_getsystime() / 10;
  }
  return 0;
}

/** @} (end of group performance_schema_buffers) */
