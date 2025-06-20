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

#include "univ.i"

#include <atomic>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "buf0flu.h"
#include "clone0api.h" /* clone_init(), clone_free() */
#include "fil0fil.h"
#include "log0buf.h"
#include "log0chkp.h"
#include "log0recv.h"
#include "log0test.h"
#include "log0write.h"
#include "srv0srv.h"
#include "srv0start.h" /* srv_is_being_started */
#include "ut0byte.h"
#include "ut0new.h"
#include "ut0rnd.h"

#include <mysql/components/minimal_chassis.h> /* minimal_chassis_init() */
#include <mysql/components/service.h>         /* SERVICE_TYPE_NO_CONST */

static std::map<std::string, std::vector<std::string>> log_sync_points = {
    {"log_buffer_exclussive_access",
     {"log_buffer_x_lock_enter_before_lock",
      "log_buffer_x_lock_enter_after_lock",
      "log_buffer_x_lock_exit_before_unlock",
      "log_buffer_x_lock_exit_after_unlock"}},

    {"log_wait_for_space",
     {"log_wfs_after_reserving_before_buf_size_1",
      "log_wfs_after_reserving_before_buf_size_2",
      "log_wait_for_space_in_buf_middle"}},

    {"log_buffer_reserve", {"log_buffer_reserve_before_buf_limit_sn"}},

    {"log_buffer_write",
     {"log_buffer_write_before_memcpy",
      "log_buffer_write_completed_before_store",
      "log_buffer_write_completed_dpa_before_store",
      "log_buffer_set_first_record_group_before_update"}},

    {"log_writer",
     {"log_advance_ready_for_write_before_reclaim",
      "log_advance_ready_for_write_before_update", "log_writer_write_begin",
      "log_writer_before_write_from_log_buffer",
      "log_writer_before_copy_to_write_ahead_buffer",
      "log_writer_before_write_new_incomplete_block",
      "log_writer_before_write_ahead", "log_writer_after_checkpoint_check",
      "log_writer_after_archiver_check", "log_writer_before_lsn_update",
      "log_writer_before_buf_limit_update", "log_writer_write_end"}},

    {"log_checkpointer",
     {"log_get_available_for_chkp_lsn_before_buf_pool",
      "log_get_available_for_chkp_lsn_before_dpa",
      "log_checkpointer_before_consider_checkpoint",
      "log_before_checkpoint_write", "log_before_checkpoint_flush",
      "log_before_checkpoint_lsn_update", "log_before_checkpoint_limits_update",
      "log_before_checkpoint_data_flush"}},

    {"log_users",
     {"log_wait_for_flush_before_flushed_to_disk_lsn",
      "log_wait_for_flush_before_wait"}},

    {"log_flusher",
     {"log_flusher_before_should_flush", "log_flush_before_fsync",
      "log_flush_before_flushed_to_disk_lsn", "log_flush_before_users_notify",
      "log_flush_before_notifier_notify"}},

    {"log_write_notifier",
     {"log_write_notifier_before_check", "log_write_notifier_after_event_reset",
      "log_write_notifier_before_wait", "log_write_notifier_before_write_lsn",
      "log_write_notifier_before_notify"}},

    {"log_flush_notifier",
     {"log_flush_notifier_before_check", "log_flush_notifier_after_event_reset",
      "log_flush_notifier_before_wait",
      "log_flush_notifier_before_flushed_to_disk_lsn",
      "log_flush_notifier_before_notify"}}};

constexpr size_t LOG_TEST_N_THREADS = 8;

constexpr int LOG_TEST_N_STEPS = 20;

fil_space_t *log_space;

extern SERVICE_TYPE_NO_CONST(registry) * srv_registry;

extern ulong srv_log_checkpoint_every;
extern ulong srv_log_wait_for_flush_timeout;

char log_group_home_dir[1] = {'\0'};

static bool log_test_general_init() {
  ut_new_boot_safe();

  /* It would be better to ask for default values of all sys vars.
  However, I did not know how to achieve that. */

  srv_log_write_events = 64;
  srv_log_flush_events = 64;
  srv_log_recent_written_size = 4 * 4096;
  srv_buf_flush_list_added_size = 4 * 4096;
  srv_log_writer_threads = true;
  srv_log_wait_for_write_spin_delay = 0;
  srv_log_wait_for_flush_timeout = 100000;
  srv_log_write_max_size = 4096;
  srv_log_writer_spin_delay = 25000;
  srv_log_checkpoint_every = INNODB_LOG_CHECKPOINT_EVERY_DEFAULT;
  srv_log_flusher_spin_delay = 25000;
  srv_log_write_notifier_spin_delay = 0;
  srv_log_flush_notifier_spin_delay = 0;
  srv_log_spin_cpu_abs_lwm = 0;
  srv_log_spin_cpu_pct_hwm = 100;
  srv_log_wait_for_flush_spin_hwm = 1000;
  srv_log_group_home_dir = log_group_home_dir;
  srv_max_n_threads = 1000;
  srv_redo_log_capacity = srv_redo_log_capacity_used = 8 * 1024 * 1024;
  srv_log_buffer_size = 256 * 1024;
  srv_log_write_ahead_size = 4096;
  srv_buf_pool_instances = 0;
  log_checksum_algorithm_ptr.store(log_block_calc_checksum_none);
  srv_n_read_io_threads = 1;
  srv_n_write_io_threads = 1;

  os_event_global_init();
  sync_check_init(srv_max_n_threads);
  recv_sys_var_init();
  os_thread_open();
  ut_crc32_init();
  os_create_block_cache();
  clone_init();

  if (!os_aio_init(srv_n_read_io_threads, srv_n_write_io_threads)) {
    std::cerr << "Cannot initialize aio system" << std::endl;
    return false;
  }

  const size_t max_n_open_files = 1000;

  /* Below function will initialize the srv_registry variable which is
  required for the mysql_plugin_registry_acquire() */
  minimal_chassis_init(&srv_registry, nullptr);
  fil_init(max_n_open_files);

  return true;
}

static bool log_test_init() {
  if (!log_test_general_init()) {
    return false;
  }

  Log_files_context log_files_ctx{srv_log_group_home_dir,
                                  Log_files_ruleset::CURRENT};

  const auto remove_ret = log_remove_files(log_files_ctx);

  const auto remove_unused_ret = log_remove_unused_files(log_files_ctx);

  srv_is_being_started = true;

  lsn_t flushed_lsn = LOG_START_LSN + LOG_BLOCK_HDR_SIZE;

  dberr_t err = log_sys_init(true, flushed_lsn, flushed_lsn);
  ut_a(err == DB_SUCCESS);

  ut_a(log_sys != nullptr);
  log_t &log = *log_sys;

  fil_open_system_tablespace_files();

  buf_flush_list_added = Buf_flush_list_added_lsns::create();
  err = log_start(log, flushed_lsn, flushed_lsn);
  ut_a(err == DB_SUCCESS);

  log_start_background_threads(log);

  srv_is_being_started = false;
  return true;
}

static bool log_test_recovery() {
  srv_is_being_started = true;
  recv_sys_create();

  /** DBLWR directory is the current directory. */
  recv_sys_init();

  lsn_t flushed_lsn = LOG_START_LSN + LOG_BLOCK_HDR_SIZE;
  lsn_t new_files_lsn;

  dberr_t err = log_sys_init(false, flushed_lsn, new_files_lsn);
  ut_a(err == DB_SUCCESS);

  EXPECT_EQ(0, new_files_lsn);

  ut_a(log_sys != nullptr);
  log_t &log = *log_sys;

  std::atomic<bool> stop_flushing = false;

  std::thread flush_thread([&stop_flushing] {
    while (!stop_flushing) {
      os_event_wait(recv_sys->flush_start);
      os_event_reset(recv_sys->flush_start);
      os_event_set(recv_sys->flush_end);
    }
  });

  err = recv_recovery_from_checkpoint_start(log, LOG_START_LSN);

  srv_is_being_started = false;

  if (err == DB_SUCCESS) {
    auto *ret = recv_recovery_from_checkpoint_finish(true);
    EXPECT_EQ(nullptr, ret);

  } else {
    srv_shutdown_state = SRV_SHUTDOWN_FLUSH_PHASE;

    /* XXX: Shouldn't this be guaranteed within log0recv.cc ? */
    while (srv_thread_is_active(srv_threads.m_recv_writer)) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  }

  stop_flushing = true;
  os_event_set(recv_sys->flush_start);
  flush_thread.join();

  recv_sys_close();

  fil_flush_file_spaces();

  return err == DB_SUCCESS;
}

template <typename TFunctor>
static void run_threads(TFunctor f, size_t n_threads) {
  std::atomic_bool started{false};
  std::atomic_size_t ready{0};
  std::vector<std::thread> threads;

  for (size_t i = 0; i < n_threads; ++i) {
    threads.emplace_back(
        [&f, &ready, &started](size_t thread_no) {
          ++ready;
          while (!started) {
            std::this_thread::yield();
          }
          f(thread_no);
        },
        i);
  }

  while (ready < n_threads) {
    std::this_thread::yield();
  }
  started = true;

  for (size_t i = 0; i < n_threads; ++i) {
    threads[i].join();
  }
}

static lsn_t write_single_mlog_test(Log_test::Key key) {
  byte record[MLOG_TEST_MAX_REC_LEN];
  byte *record_end;
  log_t &log = *log_sys;

  record_end = log_test->create_mlog_rec(record, key, MLOG_TEST_VALUE);

  record[0] |= MLOG_SINGLE_REC_FLAG;

  const size_t rec_len = record_end - record;

  auto handle = log_buffer_reserve(log, rec_len);

  mach_write_to_8(record_end - 16, handle.start_lsn);

  mach_write_to_8(record_end - 8, handle.end_lsn);

  const lsn_t end_lsn =
      log_buffer_write(log, record, rec_len, handle.start_lsn);

  ut_a(end_lsn == handle.end_lsn);

  if (handle.start_lsn / OS_FILE_LOG_BLOCK_SIZE !=
      end_lsn / OS_FILE_LOG_BLOCK_SIZE) {
    log_buffer_set_first_record_group(log, end_lsn);
  }

  log_buffer_write_completed(log, handle.start_lsn, end_lsn, true);

  buf_flush_list_added->wait_to_add(handle.start_lsn);

  Log_test::Page page;
  page.key = key;
  page.value = MLOG_TEST_VALUE;
  page.oldest_modification = handle.start_lsn;
  page.newest_modification = handle.end_lsn;

  log_test->add_dirty_page(page);
  buf_flush_list_added->report_added(page.oldest_modification,
                                     page.newest_modification);

  return end_lsn;
}

static lsn_t write_multi_mlog_tests(Log_test::Key key, size_t n) {
  constexpr size_t MLOG_TEST_MAX_GROUP_LEN =
      MLOG_TEST_MAX_REC_LEN * MLOG_TEST_GROUP_MAX_REC_N + 1;

  byte buf[MLOG_TEST_MAX_GROUP_LEN];
  byte *ptr = buf;
  std::queue<uchar *> record_end;
  Log_test::Value value_left = 0;
  log_t &log = *log_sys;
  bool is_last_block{false};

  /* We need to prepare all log records first, because we need to know
  how much space we have to reserve in redo log. */
  for (size_t i = n; i > 0; --i) {
    Log_test::Value value;
    if (i == n) {
      value = 0;
      value_left = MLOG_TEST_VALUE;
    } else if (i > 1) {
      value = ut::random_from_interval(1, 100);
      ut_a(value < value_left);
      value_left -= value;
    } else {
      value = value_left;
      value_left = 0;
    }

    ptr = log_test->create_mlog_rec(ptr, key, value);
    record_end.push(ptr);
    ut_a(ptr < buf + MLOG_TEST_MAX_GROUP_LEN);
  }

  mach_write_to_1(ptr, MLOG_MULTI_REC_END);
  ptr++;

  const size_t group_len = ptr - buf;

  size_t left_to_write = group_len;

  /* Reserve space in redo log. */
  const auto handle = log_buffer_reserve(log, group_len);

  const lsn_t group_start_lsn = handle.start_lsn;

  const sn_t group_start_sn = log_translate_lsn_to_sn(group_start_lsn);

  const lsn_t group_end_lsn =
      log_translate_sn_to_lsn(group_start_sn + group_len);

  lsn_t start_lsn = group_start_lsn;

  lsn_t end_lsn = group_start_lsn;

  ptr = buf;

  /* Copy log records to the reserved space in log buffer. */
  while (!record_end.empty()) {
    byte *end = record_end.front();

    record_end.pop();

    mach_write_to_8(end - 16, group_start_lsn);
    mach_write_to_8(end - 8, group_end_lsn);

    size_t rec_len = end - ptr;

    if (left_to_write == rec_len + 1) {
      /* Last record has MLOG_MULTI_REC_END attached. */
      ++rec_len;
      ++end;
    }

    end_lsn = log_buffer_write(log, ptr, rec_len, start_lsn);

    left_to_write -= rec_len;
    ptr = end;
    ut_a(ptr <= buf + MLOG_TEST_MAX_GROUP_LEN);

    if (left_to_write == 0) {
      is_last_block = true;
      if (group_start_lsn / OS_FILE_LOG_BLOCK_SIZE !=
          end_lsn / OS_FILE_LOG_BLOCK_SIZE) {
        log_buffer_set_first_record_group(log, end_lsn);
      }
    }

    log_buffer_write_completed(log, start_lsn, end_lsn, is_last_block);

    start_lsn = end_lsn;
  }

  ut_a(group_end_lsn == end_lsn);
  ut_a(left_to_write == 0);

  buf_flush_list_added->wait_to_add(handle.start_lsn);

  Log_test::Page page;
  page.key = key;
  page.value = MLOG_TEST_VALUE;
  page.oldest_modification = group_start_lsn;
  page.newest_modification = group_end_lsn;

  log_test->add_dirty_page(page);

  buf_flush_list_added->report_added(page.oldest_modification,
                                     page.newest_modification);
  return group_end_lsn;
}

static void log_test_run() {
  const lsn_t max_dirty_page_age = 10 * 1024;

  run_threads(
      [max_dirty_page_age](size_t thread_no) {
        static_cast<void>(max_dirty_page_age);  // clang -Wunused-lambda-capture
        log_t &log = *log_sys;

        for (int j = 0; j < LOG_TEST_N_STEPS; ++j) {
          int64_t value;
          lsn_t end_lsn;

          value = int64_t{j} * LOG_TEST_N_THREADS + thread_no;

          const auto ppb = ut::random_from_interval(0, 100);

          if (ppb <= 50) {
            /* Single MLOG_TEST */
            end_lsn = write_single_mlog_test(value);

          } else if (ppb <= 90) {
            /* Two MLOG_TEST (group of 2 records). */
            end_lsn = write_multi_mlog_tests(value, 2);

          } else {
            /* Random number of more than 2 MLOG_TEST */
            size_t n;

            n = ut::random_from_interval(3, MLOG_TEST_GROUP_MAX_REC_N);

            end_lsn = write_multi_mlog_tests(value, n);
          }

          if (j % log_test->flush_every() == 0) {
            log_test->purge(max_dirty_page_age);

            log_write_up_to(log, end_lsn, true);
          }
        }
      },
      LOG_TEST_N_THREADS);
}

static void log_test_general_close() {
  clone_free();

  undo_spaces_deinit();

  os_aio_free();

  fil_close_all_files();

  fil_close();

  minimal_chassis_deinit(srv_registry, nullptr);

  os_thread_close();

  sync_check_close();

  os_event_global_destroy();

  srv_shutdown_state = SRV_SHUTDOWN_NONE;
}

static void log_test_close() {
  log_t &log = *log_sys;

  srv_shutdown_state = SRV_SHUTDOWN_FLUSH_PHASE;

  log_stop_background_threads(log);

  if (log_test->verbosity() > 0) {
    std::cout << "Finally: lsn = " << log_get_lsn(log)
              << " checkpoint = " << log.last_checkpoint_lsn
              << " oldest_modification_approx = "
              << log_test->oldest_modification_approx() << std::endl;
  }

  log_sys_close();

  fil_close_all_files();
  fil_close();
  const size_t max_n_open_files = 1000;
  fil_init(max_n_open_files);

  log_test_recovery();

  if (log_test->verbosity() > 0) {
    std::cout << "Flushed " << log_test->flushed().size() << " pages."
              << std::endl;

    std::cout << "Found " << log_test->recovered().size()
              << " MLOG_TEST log records." << std::endl;
  }

  Log_test::Pages pages;

  for (auto flushed : log_test->flushed()) {
    pages[flushed.first] = flushed.second;
  }

  for (auto recovered : log_test->recovered()) {
    auto &p = pages[recovered.first];

    if (recovered.second.newest_modification > p.newest_modification) {
      p = recovered.second;
    }
  }

  const size_t n_pages = LOG_TEST_N_THREADS * LOG_TEST_N_STEPS;

  EXPECT_EQ(n_pages, pages.size());

  bool found_missing = false;
  for (size_t i = 0; i < n_pages; ++i) {
    if (pages.find(i) == pages.end()) {
      std::cerr << "Missing page for key: " << i
                << " (thread: " << i % LOG_TEST_N_THREADS
                << ", step: " << i / LOG_TEST_N_THREADS << ")" << std::endl;

      found_missing = true;
    }
  }

  log_sys_close();

  log_test_general_close();

  buf_flush_list_added.reset(nullptr);

  ASSERT_FALSE(found_missing);
}

static void execute_test() {
  ASSERT_TRUE(log_test_init());
  log_test_run();
  log_test_close();
}

static void configure_delay_for_sync_point(const std::string &sync_point_name,
                                           uint64_t min_us, uint64_t max_us) {
/* NOTE: On Windows sleeps smaller than minimum resolution of
timer, would anyway be equivalent to sleep(0) which only asks
for context switch. However that's good enough for now. */
#ifdef WIN32
  min_us = max_us = 0;
#endif

  using Sync_point = Log_test::Sync_point;

  class Random_delay : public Sync_point {
   public:
    Random_delay(uint64_t min_us, uint64_t max_us)
        : m_min_us(min_us), m_max_us(max_us) {}

    void sync() override {
      if (m_max_us == 0) {
        std::this_thread::yield();
      } else {
        std::this_thread::sleep_for(std::chrono::microseconds(
            ut::random_from_interval(m_min_us, m_max_us)));
      }
    }

   private:
    uint64_t m_min_us, m_max_us;
  };

  std::unique_ptr<Sync_point> sync_point_handler;

  sync_point_handler.reset(new Random_delay{min_us, max_us});

  log_test->register_sync_point_handler(sync_point_name,
                                        std::move(sync_point_handler));
}

static void test_single(const std::string &group) {
  const uint64_t max_delay =
      std::max(uint64_t{10}, uint64_t{500 / log_sync_points[group].size()});

  for (auto pt : log_sync_points[group]) {
    log_test.reset(new Log_test);

    configure_delay_for_sync_point(pt, 10, max_delay);

    execute_test();
  }

  log_test.reset(new Log_test);

  for (auto pt : log_sync_points[group]) {
    configure_delay_for_sync_point(pt, 0, 500);
  }

  execute_test();
}

class Log_test_disturber {
 public:
  virtual ~Log_test_disturber() = default;

  virtual void disturb() = 0;
};

class Log_background_disturber {
 public:
  explicit Log_background_disturber(
      std::unique_ptr<Log_test_disturber> disturber)
      : m_disturber(std::move(disturber)), m_is_active(false) {}

  void start();
  void stop();

 private:
  void run();

  std::unique_ptr<Log_test_disturber> m_disturber;
  std::thread m_thread;
  std::atomic_bool m_is_active;
};

void Log_background_disturber::start() {
  ut_a(!m_is_active);

  m_is_active = true;

  m_thread = std::thread{std::bind(&Log_background_disturber::run, this)};
}

void Log_background_disturber::stop() {
  ut_a(m_is_active);

  m_is_active = false;

  m_thread.join();
}

void Log_background_disturber::run() {
  while (m_is_active) {
    m_disturber->disturb();

    const auto sleep_time = ut::random_from_interval(0, 300 * 1000);

    std::this_thread::sleep_for(std::chrono::microseconds(sleep_time));
  }
}

class Log_buf_resizer : public Log_test_disturber {
 public:
  Log_buf_resizer(size_t min_size, size_t max_size)
      : m_min_size(min_size), m_max_size(max_size) {}

  void disturb() override {
    size_t new_size = ut::random_from_interval(m_min_size, m_max_size);

    new_size = ut_uint64_align_down(new_size, OS_FILE_LOG_BLOCK_SIZE);

    log_buffer_resize(*log_sys, new_size);
  }

 private:
  size_t m_min_size, m_max_size;
};

class Log_write_ahead_resizer : public Log_test_disturber {
 public:
  Log_write_ahead_resizer(size_t min_k, size_t max_k)
      : m_min_k(min_k), m_max_k(max_k) {}

  void disturb() override {
    const auto new_size = 1 << ut::random_from_interval(m_min_k, m_max_k);

    log_write_ahead_resize(*log_sys, new_size);
  }

 private:
  size_t m_min_k, m_max_k;
};

static void execute_disturbed_test(
    std::unique_ptr<Log_test_disturber> &&disturber) {
  log_test.reset(new Log_test);
  ASSERT_TRUE(log_test_init());
  Log_background_disturber bg{std::move(disturber)};
  bg.start();
  log_test_run();
  bg.stop();
  log_test_close();
}

TEST(log0log, log_random_disturb) {
  std::unique_ptr<Log_test_disturber> disturber;

  disturber.reset(new Log_buf_resizer{256 * 1024, 1024 * 1024});
  execute_disturbed_test(std::move(disturber));

  disturber.reset(new Log_write_ahead_resizer{
      9, ut_2_log(INNODB_LOG_WRITE_AHEAD_SIZE_MAX)});
  execute_disturbed_test(std::move(disturber));
}

TEST(log0log, log_buffer_reserve) { test_single("log_buffer_reserve"); }

TEST(log0log, log_buffer_write) { test_single("log_buffer_write"); }

TEST(log0log, log_writer) { test_single("log_writer"); }

TEST(log0log, log_flusher) { test_single("log_flusher"); }

TEST(log0log, log_write_notifier) { test_single("log_write_notifier"); }

TEST(log0log, log_flush_notifier) { test_single("log_flush_notifier"); }

TEST(log0log, log_users) { test_single("log_users"); }

TEST(log0log, log_checkpointer) { test_single("log_checkpointer"); }
