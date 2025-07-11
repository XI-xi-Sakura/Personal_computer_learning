/* Copyright (c) 2013, 2025, Oracle and/or its affiliates.

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

#ifndef MYSQLD_THD_MANAGER_INCLUDED
#define MYSQLD_THD_MANAGER_INCLUDED

#include <assert.h>
#include <stddef.h>
#include <sys/types.h>
#include <atomic>

#include "my_inttypes.h"
#include "my_thread_local.h"  // my_thread_id
#include "mysql/components/services/bits/mysql_cond_bits.h"
#include "mysql/components/services/bits/mysql_mutex_bits.h"
#include "prealloced_array.h"

class THD;

void thd_lock_thread_count();
void thd_unlock_thread_count();

/**
  Base class to perform actions on all thds in the thd list.
  Users of do_for_all_thd() need to subclass this and override operator().

  @note THD can be in the disposal state. Accessing resources freed in disposal
        state is not safe. Before accessing any such resources in operator()
        please check THD state.
*/

class Do_THD_Impl {
 public:
  virtual ~Do_THD_Impl() = default;
  virtual void operator()(THD *) = 0;
};

/**
  Base class to find specific thd from the thd list.
  Users of find_thd() need to subclass this and override operator()
  to provide implementation to find thd from thd list.
*/

class Find_THD_Impl {
 public:
  virtual ~Find_THD_Impl() = default;
  /**
    Override this operator to provide implementation to find specific thd.

    @param thd THD of one element in global thread list

    @returns bool
      @retval true  for matching thd
              false otherwise
  */
  virtual bool operator()(THD *thd) = 0;
};

/**
  Callback function used by kill_one_thread and timer_notify functions
  to find "thd" based on the thread id.
*/
class Find_thd_with_id : public Find_THD_Impl {
 public:
  Find_thd_with_id(my_thread_id value, bool daemon_allowed = false)
      : m_thread_id(value), m_daemon_allowed(daemon_allowed) {}
  bool operator()(THD *thd) override;

  const my_thread_id m_thread_id;
  const bool m_daemon_allowed;
};

/**
  This class encapsulates THD instance, controls access to the actual THD.
  It also ensures that THD::LOCK_thd_data mutex is acquired at instantiation and
  released at destruction.
*/
class THD_ptr {
 public:
  /**
    Default class constructor.
  */
  THD_ptr() = default;
  /**
    Constructor assigns THD instance to manage and acquires THD::LOCK_thd_data
    mutex.
    @param   thd   THD instance.
  */
  explicit THD_ptr(THD *thd);
  /**
    Delete copy constructor, THD_ptr copy is not allowed.
  */
  THD_ptr(THD_ptr const &) = delete;
  /**
    Move constructor.
    @param  thd_ptr    THD_ptr instance to collect underlying THD instance.
  */
  THD_ptr(THD_ptr &&thd_ptr);

  /**
    Destructor to release underlying THD instance's control and release mutex
    THD::LOCK_thd_data.
  */
  ~THD_ptr() { release(); }
  /**
    Release underlying THD instance's control and release THD::LOCK_thd_data.
    @returns  underlying THD instance.
  */
  THD *release();

  /**
    Delete copy operator, THD_ptr copy is not allowed.
  */
  THD_ptr &operator=(THD_ptr const &) = delete;
  /**
    Move semantics assignment operator.
    @param  thd_ptr    THD_ptr instance to collect underlying THD instance.
  */
  THD_ptr &operator=(THD_ptr &&thd_ptr);

  /**
    Access underlying THD instance.
    returns pointer to underlying THD instance.
  */
  THD *get() { return m_underlying; }
  /**
    Access underlying THD instance.
    returns pointer to underlying THD instance.
  */
  THD *operator->() { return m_underlying; }
  /**
    Access underlying THD instance.
    returns reference to underlying THD instance.
  */
  THD &operator*() { return *m_underlying; }

  /**
    Check if there is an underlying THD instance.
  */
  operator bool() const { return m_underlying != nullptr; }

  /**
    Compare underlying THD pointer value with the "nullptr".
    @returns true if underlying THD pointer value equals "nullptr".
  */
  bool operator==(std::nullptr_t) const { return m_underlying == nullptr; }
  /**
    Compare this instance with other THD_ptr instance.
    @param  thd_ptr    Other THD_ptr instance for comparison.
    @returns true if this instance equals other THD_ptr instance.
  */
  bool operator==(THD const *thd_ptr) const { return m_underlying == thd_ptr; }
  /**
    Compare underlying THD pointer value with the "nullptr".
    @returns true if underlying THD pointer value *not* equals nullptr.
  */
  bool operator!=(std::nullptr_t) const { return m_underlying != nullptr; }
  /**
    Compare this instance with other THD_ptr instance.
    @param  thd_ptr    Other THD_ptr instance for comparison.
    @returns true if this instance differs from other THD_ptr instance.
  */
  bool operator!=(THD const *thd_ptr) const { return m_underlying != thd_ptr; }

 private:
  // Underlying THD instance to manage.
  THD *m_underlying{nullptr};
};

/**
  This class maintains THD object of all registered threads.
  It provides interface to perform functions such as find, count,
  perform some action for each THD object in the list.

  It also provide mutators for inserting, and removing an element:
  add_thd() inserts a THD into the set, and increments the counter.
  remove_thd() removes a THD from the set, and decrements the counter.
  Method remove_thd() also broadcasts COND_thd_list.
*/

class Global_THD_manager {
 public:
  /**
    Value for thread_id reserved for THDs which does not have an
    assigned value yet. get_new_thread_id() will never return this
    value.
  */
  static const my_thread_id reserved_thread_id;

  /**
    Retrieves singleton instance
  */
  static Global_THD_manager *get_instance() {
    assert(thd_manager != nullptr);
    return thd_manager;
  }

  /**
    Checks if the singleton is not already deinitialized
  */
  static bool is_initialized() { return thd_manager != nullptr; }

  /**
    Initializes the thd manager.
    Must be called before get_instance() can be used.

    @return true if initialization failed, false otherwise.
  */
  static bool create_instance();

  /**
    Destroys the singleton instance.
  */
  static void destroy_instance();

  /**
    Internally used to bypass code.
    It enables unit test scripts to create dummy THD object for testing.
  */
  void set_unit_test() { unit_test = true; }

  /**
    Adds THD to global THD list.

    @param thd THD object
  */
  void add_thd(THD *thd);

  /**
    Removes THD from global THD list.

    @param thd THD object
  */
  void remove_thd(THD *thd);

  /**
    Retrieves thread running statistic variable.
    @return int Returns the total number of threads currently running
  */
  int get_num_thread_running() const { return atomic_num_thread_running; }

  /**
    Increments thread running statistic variable.
  */
  void inc_thread_running() { atomic_num_thread_running++; }

  /**
    Decrements thread running statistic variable.
  */
  void dec_thread_running() { atomic_num_thread_running--; }

  /**
    Retrieves thread created statistic variable.
    @return ulonglong Returns the total number of threads created
                      after server start
  */
  ulonglong get_num_thread_created() const { return atomic_thread_created; }

  /**
    Increments thread created statistic variable.
  */
  void inc_thread_created() { atomic_thread_created++; }

  /**
    Returns an unused thread id.
  */
  my_thread_id get_new_thread_id();

  /**
    Releases a thread id so that it can be reused.
    Note that this is done automatically by remove_thd().
  */
  void release_thread_id(my_thread_id thread_id);

  /**
    Retrieves thread id counter value.
    @return my_thread_id Returns the thread id counter value
    @note                This is a dirty read.
  */
  my_thread_id get_thread_id() const { return thread_id_counter; }

  /**
    Sets thread id counter value. Only used in testing for now.
    @param new_id  The next ID to hand out (if it's unused).
  */
  void set_thread_id_counter(my_thread_id new_id);

  /**
    Retrieves total number of items in global THD lists (all partitions).
    @return uint Returns the count of items in global THD lists.
  */
  static uint get_thd_count() { return atomic_global_thd_count; }

  /**
    Waits until all THDs are removed from global THD lists (all partitions).
    In other words, get_thd_count() to become zero.
  */
  void wait_till_no_thd();

  /**
    This function calls func() for all THDs in every thd list partition
    after taking local copy of the THD list partition. It acquires
    LOCK_thd_remove to prevent removal of the THD.
    @param func Object of class which overrides operator()
  */
  void do_for_all_thd_copy(Do_THD_Impl *func);

  /**
    This function calls func() for all THDs in all THD list partitions.
    @param func Object of class which overrides operator()
    @note One list partition is unlocked before the next partition is locked.
  */
  void do_for_all_thd(Do_THD_Impl *func);

  /**
   * This function calls func() for all first "n" THDs across all THD list
   * partitions.
   * @param func Object of class which overrides operator()
   * @param n number of elements we want to call func for
   */
  void do_for_first_n_thd(Do_THD_Impl *func, uint n);

  /**
    Returns a THD_ptr containing first THD for which operator() returns true.

    @param func Object of class which overrides operator()
    @return THD_ptr
      @retval THD_ptr{THD*}    When matching THD is found.
      @retval THD_ptr{nullptr} When THD is *not* found.
  */
  THD_ptr find_thd(Find_THD_Impl *func);

  THD_ptr find_thd(Find_thd_with_id *func);

  // Declared static as it is referenced in handle_fatal_signal()
  static std::atomic<uint> atomic_global_thd_count;

  // Number of THD list partitions.
  static const int NUM_PARTITIONS = 8;

 private:
  Global_THD_manager();
  ~Global_THD_manager();

  // Singleton instance.
  static Global_THD_manager *thd_manager;

  // Array of current THDs. Protected by LOCK_thd_list.
  typedef Prealloced_array<THD *, 60> THD_array;
  THD_array thd_list[NUM_PARTITIONS];

  // Array of thread ID in current use. Protected by LOCK_thread_ids.
  typedef Prealloced_array<my_thread_id, 1000> Thread_id_array;
  Thread_id_array thread_ids;

  mysql_cond_t COND_thd_list[NUM_PARTITIONS];

  // Mutexes that guard thd_list partitions
  mysql_mutex_t LOCK_thd_list[NUM_PARTITIONS];
  // Mutexes used to guard removal of elements from thd_list partitions.
  mysql_mutex_t LOCK_thd_remove[NUM_PARTITIONS];
  // Mutex protecting thread_ids
  mysql_mutex_t LOCK_thread_ids;

  // Count of active threads which are running queries in the system.
  std::atomic<int> atomic_num_thread_running;

  // Cumulative number of threads created by mysqld daemon.
  std::atomic<ulonglong> atomic_thread_created;

  // Counter to assign thread id.
  my_thread_id thread_id_counter;

  // Used during unit test to bypass creating real THD object.
  bool unit_test;

  friend void thd_lock_thread_count();
  friend void thd_unlock_thread_count();
};

#endif /* MYSQLD_INCLUDED */
