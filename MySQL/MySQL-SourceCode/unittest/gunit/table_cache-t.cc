/* Copyright (c) 2012, 2025, Oracle and/or its affiliates.

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

#include <gtest/gtest.h>
#include <sys/types.h>

#include "lex_string.h"
#include "my_inttypes.h"
#include "sql/mysqld_thd_manager.h"
#include "sql/table_cache.h"
#include "storage/example/ha_example.h"
#include "unittest/gunit/test_utils.h"

/*
  We need example_hton to be able short-cut creation of example
  handler instances for mock TABLE objects.
*/
extern handlerton *example_hton;

namespace table_cache_unittest {

using my_testing::Server_initializer;

#ifdef SAFE_MUTEX
static const char *assert_string = ".*Assertion.*count > 0.*my_thread_equal.*";
#endif

/**
  Test fixture for basic tests involving Table_cache
  and Table_cache_manager classes.

  Unlike more advanced fixture it doesn't initialize
  table cache manager, but only prepares THD objects
  necessary for testing.
*/

class TableCacheBasicTest : public ::testing::Test {
 protected:
  static const uint MAX_THREADS = 3;

  void SetUp() override {
    Global_THD_manager *thd_manager = Global_THD_manager::get_instance();
    thd_manager->set_unit_test();
    // Reset thread ID counter for each test.
    thd_manager->set_thread_id_counter(1);
    for (uint i = 0; i < MAX_THREADS; ++i) {
      initializer[i].SetUp();
    }

    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
  }
  void TearDown() override {
    for (uint i = 0; i < MAX_THREADS; ++i) initializer[i].TearDown();
  }

  THD *get_thd(uint index) { return initializer[index].thd(); }

  Server_initializer initializer[MAX_THREADS];
};

/**
  A more advanced fixture that also initializes table_cache_manager
  with one Table_cache instance and TDC.
*/

class TableCacheSingleCacheTest : public TableCacheBasicTest {
 protected:
  virtual uint CachesNumber() { return 1; }
  void SetUp() override {
    TableCacheBasicTest::SetUp();

    /*
      In addition to table_cache_manager we want to have initialized
      TDC so we can use its HASH object for calculating hash values
      and be able to free TABLE objects correctly (we need LOCK_open
      initialized for this).
    */
    table_cache_instances = CachesNumber();
    table_cache_size_per_instance = 100;
    table_cache_triggers_per_instance = 100;
    ASSERT_FALSE(table_def_init());
  }
  void TearDown() override {
    table_def_free();
    TableCacheBasicTest::TearDown();
  }
};

/**
  Another advanced fixture that also initializes table_cache_manager
  with two Table_cache instances and TDC.
*/

class TableCacheDoubleCacheTest : public TableCacheSingleCacheTest {
 protected:
  uint CachesNumber() override { return 2; }
};

/**
  Class for mock TABLE_SHARE object which also allows to create
  associated TABLE objects which are usable with Table_cache.
*/

class Mock_share : public TABLE_SHARE {
  MEM_ROOT m_mem_root;
  Table_cache_element *cache_element_arr[Table_cache_manager::MAX_TABLE_CACHES];

 public:
  Mock_share(const char *key)
      :  // Assertion in some of Table_cache methods check that the
         // version of the share is up-to-date, so make sure it's set.
        TABLE_SHARE(refresh_version, false),
        // MEM_ROOT is used for constructing ha_example() instances.
        m_mem_root(PSI_NOT_INSTRUMENTED, 1024) {
    /*
      Both table_cache_key and cache_element array are used by
      Table_cache code.
    */
    table_cache_key.str = key;
    table_cache_key.length = strlen(key);
    memset(cache_element_arr, 0, sizeof(cache_element_arr));
    cache_element = cache_element_arr;
    // Ensure that share is never destroyed.
    increment_ref_count();
  }

  ~Mock_share() { m_mem_root.Clear(); }

  TABLE *create_table(THD *thd) {
    TABLE *result =
        (TABLE *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(TABLE), MYF(0));
    new (result) TABLE;

    result->s = this;
    // We create TABLE which is already marked as used
    result->in_use = thd;
    /*
      Assertions in some of Table_cache methods need non-NULL
      TABLE::file and TABLE::db_stat. Code that frees unused
      TABLE objects needs proper "handler" instance.
    */
    result->file = new (&m_mem_root) ha_example(example_hton, this);
    result->db_stat = HA_READ_ONLY;

    return result;
  }

  TABLE *create_table_with_triggers(THD *thd) {
    // Make calling Table_trigger_dispatcher::finalize_load() safe.
    if (triggers == nullptr) triggers = new (&m_mem_root) List<Trigger>;

    TABLE *result = create_table(thd);

    result->triggers = new (&m_mem_root) Table_trigger_dispatcher(result);

    return result;
  }

  void destroy_table(TABLE *table) { my_free(table); }
};

// Google Test recommends DeathTest suffix for classes used in death tests.
typedef TableCacheBasicTest TableCacheBasicDeathTest;
typedef TableCacheDoubleCacheTest TableCacheDoubleCacheDeathTest;

/*
  Test initilization/destruction of Table_cache.
*/

TEST_F(TableCacheBasicDeathTest, CacheCreateAndDestroy) {
  Table_cache table_cache;

  ASSERT_FALSE(table_cache.init());

  // Cache should be empty after creation
  EXPECT_EQ(0U, table_cache.cached_tables());

  // Cache should be not locked after creation
#ifdef SAFE_MUTEX
  EXPECT_DEATH_IF_SUPPORTED(table_cache.assert_owner(), assert_string);
#endif
  table_cache.destroy();
}

/*
  Test locking for Table_cache object.
*/

TEST_F(TableCacheBasicDeathTest, CacheLockAndUnlock) {
  Table_cache table_cache;

  ASSERT_FALSE(table_cache.init());

#ifdef SAFE_MUTEX
  // Cache should not be locked after creation
  EXPECT_DEATH_IF_SUPPORTED(table_cache.assert_owner(), assert_string);
#endif

  // And get locked after we call its lock() method
  table_cache.lock();
  table_cache.assert_owner();

  // And get unlocked after we call its unlock() method
  table_cache.unlock();
#ifdef SAFE_MUTEX
  EXPECT_DEATH_IF_SUPPORTED(table_cache.assert_owner(), assert_string);
#endif

  table_cache.destroy();
}

/*
  Tests for the rest of methods of Table_cache need to use an
  object controlled by the global instance of Table_cache_manager.
  Let us start testing of Table_cache_manager with test for
  its initialization/destruction. This test also covers well
  Table_cache_manager::get_cache() method.
*/

TEST_F(TableCacheBasicDeathTest, ManagerCreateAndDestroy) {
  // Request two instances of Table_cache
  table_cache_instances = 2;

  ASSERT_FALSE(table_cache_manager.init());

  // All caches are empty after creation
  EXPECT_EQ(0U, table_cache_manager.cached_tables());

  // There should be two different caches in the manager
  Table_cache *cache_1, *cache_2, *cache_3;
  cache_1 = table_cache_manager.get_cache(get_thd(0));
  cache_2 = table_cache_manager.get_cache(get_thd(1));
  cache_3 = table_cache_manager.get_cache(get_thd(2));
  EXPECT_TRUE(cache_1 != cache_2);
  // And not three !
  EXPECT_TRUE(cache_3 == cache_1);

  // Both caches should be empty
  EXPECT_EQ(0U, cache_1->cached_tables());
  EXPECT_EQ(0U, cache_2->cached_tables());

  // And not locked
#ifdef SAFE_MUTEX
  EXPECT_DEATH_IF_SUPPORTED(cache_1->assert_owner(), assert_string);
  EXPECT_DEATH_IF_SUPPORTED(cache_2->assert_owner(), assert_string);
#endif

  table_cache_manager.destroy();
}

/**
  Add a TABLE to a table cache and increment the reference count of
  its TABLE_SHARE.
*/
void add_used_table(Table_cache *table_cache, THD *thd, TABLE *table) {
  table->s->increment_ref_count();
  EXPECT_FALSE(table_cache->add_used_table(thd, table));
}

/*
  Test addition and removal of TABLE objects to/from the table cache.
*/

TEST_F(TableCacheSingleCacheTest, CacheAddAndRemove) {
  THD *thd = get_thd(0);

  Mock_share share_1("share_1");
  TABLE *table_1 = share_1.create_table(thd);

  Table_cache *table_cache = table_cache_manager.get_cache(thd);
  table_cache->lock();
  add_used_table(table_cache, thd, table_1);

  // There should be one TABLE in the cache after we have added table_1.
  EXPECT_EQ(1U, table_cache->cached_tables());

  // There should be no unused TABLE objects for the same table in the
  // cache. OTOH it should contain info about table share of table_1.
  TABLE *table_2;
  TABLE_SHARE *share_2;
  table_2 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_2 == nullptr);
  EXPECT_TRUE(share_2 == &share_1);

  // Table_cache_iterator should be able to find only one TABLE instance
  // in all caches. And this instance should be table_1.
  Table_cache_iterator it(&share_1);
  EXPECT_TRUE(it++ == table_1);
  EXPECT_TRUE(it++ == nullptr);

  // We must be able to release TABLE into table cache and reuse it after
  // this.
  table_cache->release_table(thd, table_1);
  table_2 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_2 == table_1);
  EXPECT_TRUE(share_2 == &share_1);

  table_cache->remove_table(table_1);

  // Once TABLE is removed from the cache the latter should become empty.
  EXPECT_EQ(0U, table_cache->cached_tables());

  table_2 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_2 == nullptr);
  EXPECT_TRUE(share_2 == nullptr);

  it.rewind();
  EXPECT_TRUE(it++ == nullptr);

  // Also it should be possible to remove unused TABLE from the cache
  // Add TABLE instance and mark it as unused
  add_used_table(table_cache, thd, table_1);
  table_cache->release_table(thd, table_1);

  table_cache->remove_table(table_1);

  // Once TABLE is removed from cache the latter should become empty.
  EXPECT_EQ(0U, table_cache->cached_tables());

  table_2 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_2 == nullptr);
  EXPECT_TRUE(share_2 == nullptr);

  table_cache->unlock();

  share_1.destroy_table(table_1);
}

/*
  Now let us test how Table_cache handles overflows.
*/

TEST_F(TableCacheSingleCacheTest, CacheOverflow) {
  THD *thd = get_thd(0);

  // Set cache size low so it will overflow quickly.
  table_cache_size_per_instance = 2;

  Mock_share share_1("share_1");
  Mock_share share_2("share_2");
  TABLE *table_1 = share_1.create_table(thd);
  TABLE *table_2 = share_1.create_table(thd);
  TABLE *table_3 = share_2.create_table(thd);

  Table_cache *table_cache = table_cache_manager.get_cache(thd);

  table_cache->lock();
  add_used_table(table_cache, thd, table_1);
  add_used_table(table_cache, thd, table_2);

  // There should be two TABLE instances in the cache.
  EXPECT_EQ(2U, table_cache->cached_tables());

  table_cache->release_table(thd, table_1);
  table_cache->release_table(thd, table_2);

  // Still there should be two TABLE instances in the cache.
  EXPECT_EQ(2U, table_cache->cached_tables());

  add_used_table(table_cache, thd, table_3);

  // One TABLE was added and one expelled (table_1), so still two TABLE objects.
  EXPECT_EQ(2U, table_cache->cached_tables());

  // Old value of table_1 points to garbage thanks to expelling
  table_1 = share_1.create_table(thd);
  add_used_table(table_cache, thd, table_1);

  // Still two TABLE instances (table_2 was expelled).
  EXPECT_EQ(2U, table_cache->cached_tables());

  // Old value of table_2 points to garbage thanks to expelling
  table_2 = share_1.create_table(thd);
  add_used_table(table_cache, thd, table_2);

  /*
    Now we should have three TABLE instances in cache since all
    of them are used.
  */
  EXPECT_EQ(3U, table_cache->cached_tables());

  table_cache->release_table(thd, table_2);

  // The first table that gets released is expelled.
  EXPECT_EQ(2U, table_cache->cached_tables());

  table_cache->remove_table(table_1);
  table_cache->remove_table(table_3);

  // Cache should be empty after that
  EXPECT_EQ(0U, table_cache->cached_tables());

  table_cache->unlock();

  share_1.destroy_table(table_1);
  share_1.destroy_table(table_3);
}

TEST_F(TableCacheSingleCacheTest, CacheGetAndRelease) {
  THD *thd = get_thd(0);

  Table_cache *table_cache = table_cache_manager.get_cache(thd);

  table_cache->lock();

  TABLE *table_1, *table_2, *table_3;
  Mock_share share_1("share_1"), share_0("share_0");
  TABLE_SHARE *share_2;

  // There should be no TABLE in cache, nor information about share.
  table_1 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_1 == nullptr);
  EXPECT_TRUE(share_2 == nullptr);

  table_1 = share_1.create_table(thd);
  add_used_table(table_cache, thd, table_1);

  // There should be no unused TABLE in cache, but there should be
  // information about the share.
  table_2 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_2 == nullptr);
  EXPECT_TRUE(share_2 == &share_1);

  // There should be even no information about the share for which
  // TABLE was not added to cache.
  table_2 =
      table_cache->get_table(thd, share_0.table_cache_key.str,
                             share_0.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_2 == nullptr);
  EXPECT_TRUE(share_2 == nullptr);

  table_2 = share_1.create_table(thd);
  add_used_table(table_cache, thd, table_2);

  // Still there should be no unused TABLE in cache, but there should
  // be information about the share.
  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_3 == nullptr);
  EXPECT_TRUE(share_2 == &share_1);

  table_cache->release_table(thd, table_1);

  // After releasing one of TABLE objects it should be possible to get
  // unused TABLE from cache.
  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_3 == table_1);
  EXPECT_TRUE(share_2 == &share_1);

  // But only once!
  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_3 == nullptr);
  EXPECT_TRUE(share_2 == &share_1);

  // After releasing of both TABLE objects it should be possible to
  // get two unused TABLE objects from cache (for 'share_1').
  // There should be nothing for 'share_0'.
  table_cache->release_table(thd, table_1);
  table_cache->release_table(thd, table_2);

  table_3 =
      table_cache->get_table(thd, share_0.table_cache_key.str,
                             share_0.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_3 == nullptr);
  EXPECT_TRUE(share_2 == nullptr);

  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_3 != nullptr);
  EXPECT_TRUE(share_2 == &share_1);
  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_3 != nullptr);
  EXPECT_TRUE(share_2 == &share_1);
  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_3 == nullptr);
  EXPECT_TRUE(share_2 == &share_1);

  // Clean-up
  table_cache->remove_table(table_1);
  table_cache->remove_table(table_2);

  share_1.destroy_table(table_1);
  share_1.destroy_table(table_2);

  table_cache->unlock();
}

TEST_F(TableCacheSingleCacheTest, CacheGetAndReleaseWithTriggers) {
  THD *thd = get_thd(0);

  Table_cache *table_cache = table_cache_manager.get_cache(thd);

  table_cache->lock();

  TABLE *table_1, *table_2, *table_3, *table_4;
  Mock_share share_1("share_1"), share_0("share_0");
  TABLE_SHARE *share_2;

  // There should be no TABLE in cache, nor information about share.
  table_1 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_1 == nullptr);
  EXPECT_TRUE(share_2 == nullptr);

  // There should be no TABLE in cache for update either.
  table_1 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_1 == nullptr);
  EXPECT_TRUE(share_2 == nullptr);

  // Counter of TABLE objects with fully-loaded triggers should be zero.
  EXPECT_EQ(0U, table_cache->loaded_triggers_tables());

  table_1 = share_1.create_table_with_triggers(thd);
  add_used_table(table_cache, thd, table_1);

  // Counter of TABLE objects with fully-loaded triggers should still be zero.
  EXPECT_EQ(0U, table_cache->loaded_triggers_tables());

  // There should be no unused TABLE in cache, but there should be
  // information about the share.
  table_2 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_2 == nullptr);
  EXPECT_TRUE(share_2 == &share_1);

  // There should be no unused TABLE for update either (but again we
  // should be able to get information about the share).
  table_2 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_2 == nullptr);
  EXPECT_TRUE(share_2 == &share_1);

  table_cache->release_table(thd, table_1);

  // Release doesn't increase counter of TABLE objects with fully-loaded
  // triggers.
  EXPECT_EQ(0U, table_cache->loaded_triggers_tables());

  // After releasing the TABLE object it should be possible to get
  // unused TABLE from cache for update, even though table triggers
  // are not fully loaded.
  table_2 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_2 == table_1);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_FALSE(table_1->triggers->has_load_been_finalized());

  // Pretend that we have fully loaded triggers for this object.

  // Number of TABLE objects with fully-loaded triggers is 0 before the load.
  EXPECT_EQ(0U, table_cache->loaded_triggers_tables());

  table_cache->unlock();
  table_1->triggers->finalize_load(thd);
  EXPECT_TRUE(table_1->triggers->has_load_been_finalized());
  table_cache->lock();

  // Number of TABLE objects with fully-loaded triggers is 1 after the load.
  EXPECT_EQ(1U, table_cache->loaded_triggers_tables());

  table_cache->release_table(thd, table_1);

  // After the TABLE object with fully loaded triggers is released
  // to the cache it should be possible to get unused TABLE from
  // cache ready for update.
  table_2 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_2 == table_1);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_TRUE(table_1->triggers->has_load_been_finalized());

  table_cache->release_table(thd, table_1);

  // After releasing the TABLE object again, it should be possible to get
  // the same unused TABLE from cache even for read-only load.
  table_2 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_2 == table_1);
  EXPECT_TRUE(share_2 == &share_1);
  // Even though we have requested TABLE for read-only operation it
  // still has its triggers fully loaded.
  EXPECT_TRUE(table_1->triggers->has_load_been_finalized());

  // Get another TABLE object for the share, but do not finalize loading
  // of its triggers yet.
  table_2 = share_1.create_table_with_triggers(thd);
  add_used_table(table_cache, thd, table_2);

  // Getting fresh TABLE doesn't increase counter of TABLEs with fully-loaded
  // triggers.
  EXPECT_EQ(1U, table_cache->loaded_triggers_tables());

  // There should be no unused TABLE for either read-only or update.
  // But we should be able to get information about the share.
  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_3 == nullptr);
  EXPECT_TRUE(share_2 == &share_1);

  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_3 == nullptr);
  EXPECT_TRUE(share_2 == &share_1);

  // Once this TABLE object without fully loaded triggers is released
  // it becomes usable for both read-only and update operations.
  table_cache->release_table(thd, table_2);

  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_3 == table_2);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_FALSE(table_3->triggers->has_load_been_finalized());

  table_cache->release_table(thd, table_2);
  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_3 == table_2);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_FALSE(table_3->triggers->has_load_been_finalized());

  // Pretend that we have aborted update early and didn't load triggers.
  table_cache->release_table(thd, table_2);

  // Release TABLE object with fully loaded triggers as well.
  table_cache->release_table(thd, table_1);

  // At this point we have two unused TABLE objects one with fully-loaded
  // triggers and one without.

  // This is confirmed by value of counter of TABLEs with fully-loaded triggers.
  EXPECT_EQ(1U, table_cache->loaded_triggers_tables());

  // First, let us check that requests for TABLEs for read-only statements
  // prefer objects without fully-loaded triggers.
  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_3 == table_2);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_FALSE(table_3->triggers->has_load_been_finalized());

  // However, if such object not available TABLE with fully-loaded triggers
  // will do as well.
  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_3 == table_1);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_TRUE(table_3->triggers->has_load_been_finalized());

  // Rinse and repeat to demonstrate that this behavior is not dependent
  // on the order in which TABLE objects returned to cache.
  table_cache->release_table(thd, table_1);
  table_cache->release_table(thd, table_2);

  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_3 == table_2);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_FALSE(table_3->triggers->has_load_been_finalized());

  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, false, &share_2);
  EXPECT_TRUE(table_3 == table_1);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_TRUE(table_3->triggers->has_load_been_finalized());

  // Requests for TABLE objects for updating statements should prefer
  // TABLE objects with fully-loaded triggers.
  table_cache->release_table(thd, table_1);
  table_cache->release_table(thd, table_2);

  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_3 == table_1);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_TRUE(table_3->triggers->has_load_been_finalized());

  // However, if there are no such unused TABLE objects, TABLE without
  // fully-loaded triggers will do.
  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_3 == table_2);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_FALSE(table_3->triggers->has_load_been_finalized());

  // Repeat requests to show that this behavior doesn't depend on
  // the order in which tables were released.
  table_cache->release_table(thd, table_2);
  table_cache->release_table(thd, table_1);

  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_3 == table_1);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_TRUE(table_3->triggers->has_load_been_finalized());

  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_3 == table_2);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_FALSE(table_3->triggers->has_load_been_finalized());

  // Simulate trigger loading for the second TABLE object.
  EXPECT_EQ(1U, table_cache->loaded_triggers_tables());
  table_cache->unlock();
  table_2->triggers->finalize_load(thd);
  EXPECT_TRUE(table_2->triggers->has_load_been_finalized());
  table_cache->lock();
  // Counter of TABLEs with fully-loaded triggers should have been
  // increased.
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  table_cache->release_table(thd, table_1);
  table_cache->release_table(thd, table_2);

  // Releasing objects to the cache doesn't change counter of TABLEs
  // with fully-loaded triggers.
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  // Both unused TABLEs can be used for updating statements.
  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_3 == table_1 || table_3 == table_2);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_TRUE(table_3->triggers->has_load_been_finalized());

  table_4 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_4 == table_1 || table_4 == table_2);
  EXPECT_TRUE(table_4 != table_3);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_TRUE(table_4->triggers->has_load_been_finalized());

  // Number of TABLE objects with fully loaded triggers stays the same.
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  table_cache->release_table(thd, table_1);
  table_cache->release_table(thd, table_2);

  // Also both unused TABLEs can be used for read-only statements.
  table_3 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_3 == table_1 || table_3 == table_2);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_TRUE(table_3->triggers->has_load_been_finalized());

  table_4 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_2);
  EXPECT_TRUE(table_4 == table_1 || table_4 == table_2);
  EXPECT_TRUE(table_4 != table_3);
  EXPECT_TRUE(share_2 == &share_1);
  EXPECT_TRUE(table_4->triggers->has_load_been_finalized());

  // Clean-up
  table_cache->remove_table(table_1);
  table_cache->remove_table(table_2);

  share_1.destroy_table(table_1);
  share_1.destroy_table(table_2);

  table_cache->unlock();
}

TEST_F(TableCacheSingleCacheTest, CacheOverflowWithTriggers) {
  THD *thd = get_thd(0);

  // Set cache size low so it will overflow quickly.
  table_cache_size_per_instance = 4;
  table_cache_triggers_per_instance = 2;

  Mock_share share_1("share_1");
  Mock_share share_2("share_2");
  TABLE *table_1 = share_1.create_table_with_triggers(thd);
  TABLE *table_2 = share_1.create_table_with_triggers(thd);
  TABLE *table_3 = share_1.create_table_with_triggers(thd);
  TABLE *table_4 = share_2.create_table_with_triggers(thd);
  TABLE *table_5 = share_2.create_table_with_triggers(thd);
  TABLE *table_6, *table_7;
  TABLE_SHARE *share_3;

  Table_cache *table_cache = table_cache_manager.get_cache(thd);

  table_cache->lock();
  add_used_table(table_cache, thd, table_1);
  add_used_table(table_cache, thd, table_2);
  add_used_table(table_cache, thd, table_3);

  // There should be two TABLE instances in the cache.
  EXPECT_EQ(3U, table_cache->cached_tables());
  // But no tables with fully-loaded triggers.
  EXPECT_EQ(0U, table_cache->loaded_triggers_tables());

  // Simulate loading of triggers for 2 instances.
  table_cache->unlock();
  table_1->triggers->finalize_load(thd);
  table_2->triggers->finalize_load(thd);
  EXPECT_TRUE(table_1->triggers->has_load_been_finalized());
  EXPECT_TRUE(table_2->triggers->has_load_been_finalized());
  table_cache->lock();

  // There should be two TABLE instances with loaded triggers now.
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  table_cache->release_table(thd, table_1);
  table_cache->release_table(thd, table_2);
  table_cache->release_table(thd, table_3);

  // Still there should be 3 TABLE instances in the cache
  // and 2 TABLE instances with triggers loaded.
  EXPECT_EQ(3U, table_cache->cached_tables());
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  // Add one more TABLE to the cache (sans triggers).
  add_used_table(table_cache, thd, table_4);

  // Number of TABLE instances increases, number of TABLE with
  // triggers stays the same.
  EXPECT_EQ(4U, table_cache->cached_tables());
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  // Simulate loading triggers for table for share_2.
  table_cache->unlock();
  table_4->triggers->finalize_load(thd);
  EXPECT_TRUE(table_4->triggers->has_load_been_finalized());
  table_cache->lock();

  // Number of TABLE instances stays the same.
  EXPECT_EQ(4U, table_cache->cached_tables());
  // Number of TABLE instances with triggers increases and crosses
  // threshold, since loading of triggers doesn't cause freeing
  // TABLE objects per se.
  EXPECT_EQ(3U, table_cache->loaded_triggers_tables());

  // Release TABLE to the cache, this causes LRU TABLE with
  // triggers (table_1) expelled.
  table_cache->release_table(thd, table_4);

  // Total number of TABLE instances decreases.
  EXPECT_EQ(3U, table_cache->cached_tables());
  // Number of TABLE instances with fully-loaded triggers goes below
  // threshold.
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  // Let us check that it is exactly LRU TABLE with triggers (table_1) that
  // got expelled. Acquire two TABLE objects with triggers which should have
  // remained.
  table_6 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_3);
  EXPECT_TRUE(table_6 == table_2);
  EXPECT_TRUE(share_3 == &share_1);
  EXPECT_TRUE(table_6->triggers->has_load_been_finalized());

  table_6 =
      table_cache->get_table(thd, share_2.table_cache_key.str,
                             share_2.table_cache_key.length, true, &share_3);
  EXPECT_TRUE(table_6 == table_4);
  EXPECT_TRUE(share_3 == &share_2);
  EXPECT_TRUE(table_6->triggers->has_load_been_finalized());

  // Total number of TABLE objects and objects with triggers should stay
  // the same.
  EXPECT_EQ(3U, table_cache->cached_tables());
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  // Acquire remaining TABLE object for share_1 and load triggers for it.
  table_6 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_3);
  EXPECT_TRUE(table_6 == table_3);
  EXPECT_TRUE(share_3 == &share_1);
  EXPECT_FALSE(table_6->triggers->has_load_been_finalized());

  table_cache->unlock();
  table_3->triggers->finalize_load(thd);
  EXPECT_TRUE(table_3->triggers->has_load_been_finalized());
  table_cache->lock();

  // Total number of TABLE objects stays the same.
  EXPECT_EQ(3U, table_cache->cached_tables());
  // Number of TABLE objects with fully loaded triggers exceeds threshold,
  // since all 3 of them are used.
  EXPECT_EQ(3U, table_cache->loaded_triggers_tables());

  // Release all three TABLEs with triggers.
  table_cache->release_table(thd, table_4);
  table_cache->release_table(thd, table_2);
  table_cache->release_table(thd, table_3);

  // The first table that gets released is expelled.
  EXPECT_EQ(2U, table_cache->cached_tables());
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  // Two remaining TABLE objects (table_2 and table_3) should be still
  // reachable.
  table_6 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_3);
  EXPECT_TRUE(table_6 == table_2 || table_6 == table_3);
  EXPECT_TRUE(share_3 == &share_1);
  EXPECT_TRUE(table_6->triggers->has_load_been_finalized());

  table_7 =
      table_cache->get_table(thd, share_1.table_cache_key.str,
                             share_1.table_cache_key.length, true, &share_3);
  EXPECT_TRUE(table_7 == table_2 || table_7 == table_3);
  EXPECT_TRUE(table_7 != table_6);
  EXPECT_TRUE(share_3 == &share_1);
  EXPECT_TRUE(table_7->triggers->has_load_been_finalized());

  // Total and number of TABLEs with triggers stay the same.
  EXPECT_EQ(2U, table_cache->cached_tables());
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  // Add one more TABLE object to cache.
  add_used_table(table_cache, thd, table_5);

  // Total number of TABLEs gets increase.
  EXPECT_EQ(3U, table_cache->cached_tables());
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  // Simulate loading of triggers for this last table.
  table_cache->unlock();
  table_5->triggers->finalize_load(thd);
  EXPECT_TRUE(table_5->triggers->has_load_been_finalized());
  table_cache->lock();

  // Number of TABLEs with triggers increases and exceeds threshold,
  // but it is OK they are all used.
  EXPECT_EQ(3U, table_cache->cached_tables());
  EXPECT_EQ(3U, table_cache->loaded_triggers_tables());

  // Now let us add two more used TABLE instances. Note that table_1
  // and table_4 point to trash at this point.
  table_1 = share_1.create_table_with_triggers(thd);
  table_4 = share_2.create_table_with_triggers(thd);
  add_used_table(table_cache, thd, table_1);
  add_used_table(table_cache, thd, table_4);

  // Total number of TABLEs and TABLEs with triggers exceed threshold now.
  // This is OK since they are all used.
  EXPECT_EQ(5U, table_cache->cached_tables());
  EXPECT_EQ(3U, table_cache->loaded_triggers_tables());

  // Release TABLE with triggers, it will be expelled immediately (there
  // should not be double free).
  table_cache->release_table(thd, table_2);

  // Both total and with triggers counters are decremented.
  EXPECT_EQ(4U, table_cache->cached_tables());
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  // Now create and add the TABLE as used without triggers back.
  table_2 = share_1.create_table_with_triggers(thd);
  add_used_table(table_cache, thd, table_2);

  // Total number of TABLEs is incremented.
  EXPECT_EQ(5U, table_cache->cached_tables());
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  // Release TABLE sans triggers now.
  table_cache->release_table(thd, table_1);

  // Only total number of TABLEs gets decremented.
  EXPECT_EQ(4U, table_cache->cached_tables());
  EXPECT_EQ(2U, table_cache->loaded_triggers_tables());

  // Clean-up.
  table_cache->remove_table(table_2);
  table_cache->remove_table(table_3);
  table_cache->remove_table(table_4);
  table_cache->remove_table(table_5);

  // Cache should be empty after that
  EXPECT_EQ(0U, table_cache->cached_tables());
  EXPECT_EQ(0U, table_cache->loaded_triggers_tables());

  table_cache->unlock();

  share_1.destroy_table(table_2);
  share_1.destroy_table(table_3);
  share_2.destroy_table(table_4);
  share_2.destroy_table(table_5);
}

/*
  Test for Table_cache_manager/Table_cache::free_all_unused_tables().
*/

TEST_F(TableCacheDoubleCacheTest, ManagerFreeAllUnused) {
  THD *thd_1 = get_thd(0);
  THD *thd_2 = get_thd(1);

  Table_cache *table_cache_1 = table_cache_manager.get_cache(thd_1);
  Table_cache *table_cache_2 = table_cache_manager.get_cache(thd_2);

  // There should be no TABLE instances in all cachea.
  EXPECT_EQ(0U, table_cache_manager.cached_tables());

  Mock_share share_1("share_1");
  Mock_share share_2("share_2");
  Mock_share share_3("share_2");
  TABLE *table_1 = share_1.create_table(thd_1);
  TABLE *table_2 = share_1.create_table(thd_1);
  TABLE *table_3 = share_2.create_table(thd_1);
  TABLE *table_4 = share_2.create_table(thd_1);
  TABLE *table_5 = share_1.create_table(thd_2);
  TABLE *table_6 = share_3.create_table(thd_2);

  table_cache_manager.lock_all_and_tdc();

  add_used_table(table_cache_1, thd_1, table_1);
  add_used_table(table_cache_1, thd_1, table_2);
  add_used_table(table_cache_1, thd_1, table_3);
  add_used_table(table_cache_1, thd_1, table_4);
  add_used_table(table_cache_2, thd_2, table_5);
  add_used_table(table_cache_2, thd_2, table_6);

  EXPECT_EQ(4U, table_cache_1->cached_tables());
  EXPECT_EQ(2U, table_cache_2->cached_tables());
  EXPECT_EQ(6U, table_cache_manager.cached_tables());

  table_cache_manager.free_all_unused_tables();

  // All TABLE instances should stay around in caches as
  // all of them are used.
  EXPECT_EQ(4U, table_cache_1->cached_tables());
  EXPECT_EQ(2U, table_cache_2->cached_tables());
  EXPECT_EQ(6U, table_cache_manager.cached_tables());

  table_cache_1->release_table(thd_1, table_1);

  table_cache_manager.free_all_unused_tables();

  // One table should be freed. So there should be 3 + 2 TABLE instances.
  EXPECT_EQ(3U, table_cache_1->cached_tables());
  EXPECT_EQ(2U, table_cache_2->cached_tables());
  EXPECT_EQ(5U, table_cache_manager.cached_tables());

  table_cache_1->release_table(thd_1, table_2);
  table_cache_1->release_table(thd_1, table_3);
  table_cache_2->release_table(thd_2, table_5);

  table_cache_manager.free_all_unused_tables();

  // Now there should be 1 + 1 used TABLE instances left.
  EXPECT_EQ(1U, table_cache_1->cached_tables());
  EXPECT_EQ(1U, table_cache_2->cached_tables());
  EXPECT_EQ(2U, table_cache_manager.cached_tables());

  table_cache_1->release_table(thd_1, table_4);

  table_cache_manager.free_all_unused_tables();

  // There should be 0 + 1 TABLE instances around.
  EXPECT_EQ(0U, table_cache_1->cached_tables());
  EXPECT_EQ(1U, table_cache_2->cached_tables());
  EXPECT_EQ(1U, table_cache_manager.cached_tables());

  table_cache_2->release_table(thd_2, table_6);

  table_cache_manager.free_all_unused_tables();

  // All caches should become empty.
  EXPECT_EQ(0U, table_cache_1->cached_tables());
  EXPECT_EQ(0U, table_cache_2->cached_tables());
  EXPECT_EQ(0U, table_cache_manager.cached_tables());

  table_cache_manager.unlock_all_and_tdc();
}

/*
  Test for Table_cache_manager/Table_cache::cached_tables().
*/

TEST_F(TableCacheDoubleCacheTest, ManagerCachedTables) {
  THD *thd_1 = get_thd(0);
  THD *thd_2 = get_thd(1);

  Table_cache *table_cache_1 = table_cache_manager.get_cache(thd_1);
  Table_cache *table_cache_2 = table_cache_manager.get_cache(thd_2);

  // There should be no TABLE instances in all cachea.
  EXPECT_EQ(0U, table_cache_1->cached_tables());
  EXPECT_EQ(0U, table_cache_2->cached_tables());
  EXPECT_EQ(0U, table_cache_manager.cached_tables());

  Mock_share share_1("share_1");
  Mock_share share_2("share_2");
  TABLE *table_1 = share_1.create_table(thd_1);
  TABLE *table_2 = share_1.create_table(thd_1);
  TABLE *table_3 = share_2.create_table(thd_1);
  TABLE *table_4 = share_1.create_table(thd_2);
  TABLE *table_5 = share_2.create_table(thd_2);

  table_cache_manager.lock_all_and_tdc();

  add_used_table(table_cache_1, thd_1, table_1);
  add_used_table(table_cache_1, thd_1, table_2);
  add_used_table(table_cache_1, thd_1, table_3);

  // There should be 3 + 0 TABLE objects in cache
  EXPECT_EQ(3U, table_cache_1->cached_tables());
  EXPECT_EQ(0U, table_cache_2->cached_tables());
  EXPECT_EQ(3U, table_cache_manager.cached_tables());

  add_used_table(table_cache_2, thd_2, table_4);
  add_used_table(table_cache_2, thd_2, table_5);

  // There should be 3 + 2 TABLE objects in cache
  EXPECT_EQ(3U, table_cache_1->cached_tables());
  EXPECT_EQ(2U, table_cache_2->cached_tables());
  EXPECT_EQ(5U, table_cache_manager.cached_tables());

  table_cache_1->release_table(thd_1, table_1);
  table_cache_2->release_table(thd_2, table_4);

  // There should be the same number of TABLE objects - 3 + 2
  EXPECT_EQ(3U, table_cache_1->cached_tables());
  EXPECT_EQ(2U, table_cache_2->cached_tables());
  EXPECT_EQ(5U, table_cache_manager.cached_tables());

  table_cache_2->remove_table(table_5);

  // There should be 3 + 1 TABLE objects in cache
  EXPECT_EQ(3U, table_cache_1->cached_tables());
  EXPECT_EQ(1U, table_cache_2->cached_tables());
  EXPECT_EQ(4U, table_cache_manager.cached_tables());

  table_cache_1->remove_table(table_1);
  table_cache_2->remove_table(table_4);

  // There should be 2 + 0 TABLE objects in cache
  EXPECT_EQ(2U, table_cache_1->cached_tables());
  EXPECT_EQ(0U, table_cache_2->cached_tables());
  EXPECT_EQ(2U, table_cache_manager.cached_tables());

  table_cache_1->remove_table(table_2);
  table_cache_1->remove_table(table_3);

  // Caches should be empty
  EXPECT_EQ(0U, table_cache_1->cached_tables());
  EXPECT_EQ(0U, table_cache_2->cached_tables());
  EXPECT_EQ(0U, table_cache_manager.cached_tables());

  table_cache_manager.unlock_all_and_tdc();

  share_1.destroy_table(table_1);
  share_1.destroy_table(table_2);
  share_2.destroy_table(table_3);
  share_1.destroy_table(table_4);
  share_2.destroy_table(table_5);
}

/*
  Coverage for lock and unlock methods of Table_cache_manager class.
*/

TEST_F(TableCacheDoubleCacheDeathTest, ManagerLockAndUnlock) {
// Nor caches nor LOCK_open should not be locked after initialization
#ifdef SAFE_MUTEX
  EXPECT_DEATH_IF_SUPPORTED(table_cache_manager.assert_owner_all(),
                            assert_string);
  EXPECT_DEATH_IF_SUPPORTED(table_cache_manager.assert_owner_all_and_tdc(),
                            assert_string);
#endif

  // And get locked after we call its lock_all_and_tdc() method.
  table_cache_manager.lock_all_and_tdc();
  table_cache_manager.assert_owner_all();
  table_cache_manager.assert_owner_all_and_tdc();

  // In addition to Table_cache_manager method we check this by
  // calling Table_cache methods and asserting state of LOCK_open.
  Table_cache *cache_1 = table_cache_manager.get_cache(get_thd(0));
  Table_cache *cache_2 = table_cache_manager.get_cache(get_thd(1));

  cache_1->assert_owner();
  cache_2->assert_owner();
  mysql_mutex_assert_owner(&LOCK_open);

  // Locks should be unlocked after we call unlock method
  table_cache_manager.unlock_all_and_tdc();

#ifdef SAFE_MUTEX
  EXPECT_DEATH_IF_SUPPORTED(table_cache_manager.assert_owner_all(),
                            assert_string);
  EXPECT_DEATH_IF_SUPPORTED(table_cache_manager.assert_owner_all_and_tdc(),
                            assert_string);
#endif
}

/*
  Coverage for Table_cache_manager::free_table();
*/

TEST_F(TableCacheDoubleCacheDeathTest, ManagerFreeTable) {
  THD *thd_1 = get_thd(0);
  THD *thd_2 = get_thd(1);

  Table_cache *table_cache_1 = table_cache_manager.get_cache(thd_1);
  Table_cache *table_cache_2 = table_cache_manager.get_cache(thd_2);

  Mock_share share_1("share_1");
  Mock_share share_2("share_2");
  TABLE *table_1 = share_1.create_table(thd_1);
  TABLE *table_2 = share_1.create_table(thd_1);
  TABLE *table_3 = share_2.create_table(thd_1);
  TABLE *table_4 = share_1.create_table(thd_2);
  TABLE *table_5 = share_2.create_table(thd_2);

  table_cache_manager.lock_all_and_tdc();

  /*
    Coverage for TDC_RT_REMOVE_ALL case.
  */
  add_used_table(table_cache_1, thd_1, table_1);
  add_used_table(table_cache_1, thd_1, table_2);
  table_cache_1->release_table(thd_1, table_2);
  add_used_table(table_cache_1, thd_1, table_3);
  add_used_table(table_cache_2, thd_2, table_4);
  add_used_table(table_cache_2, thd_2, table_5);

  EXPECT_EQ(5U, table_cache_manager.cached_tables());

  // Added three tables for share_1 and two tables for share_2. The
  // reference count should be one higher due to Mock_share's
  // constructor setting it to 1.
  EXPECT_EQ(4U, share_1.ref_count());
  EXPECT_EQ(3U, share_2.ref_count());

  // There should be assert failure since we are trying
  // to free all tables for share_1, while some tables
  // are in use.
#ifndef NDEBUG
  EXPECT_DEATH_IF_SUPPORTED(
      table_cache_manager.free_table(thd_1, TDC_RT_REMOVE_ALL, &share_1),
      ".*Assertion.*is_empty.*");
#endif

  table_cache_1->release_table(thd_1, table_1);
  table_cache_2->release_table(thd_2, table_4);

  // After all tables for share_1 marked as unused freeing
  // all tables should succeed.
  table_cache_manager.free_table(thd_1, TDC_RT_REMOVE_ALL, &share_1);

  // After all the tables for share_1 are freed, the reference count
  // should go down to 1. Not to 0, since Mock_share sets it to 1 in
  // its constructor.
  EXPECT_EQ(1U, share_1.ref_count());

  // We still should have 2 TABLE objects for share_2.
  EXPECT_EQ(2U, table_cache_manager.cached_tables());

  /*
    Coverage for TDC_RT_REMOVE_NOT_OWN case.
  */
  table_1 = share_1.create_table(thd_1);
  table_2 = share_1.create_table(thd_1);
  table_4 = share_1.create_table(thd_2);

  add_used_table(table_cache_1, thd_1, table_1);
  add_used_table(table_cache_1, thd_1, table_2);
  table_cache_1->release_table(thd_1, table_2);
  add_used_table(table_cache_2, thd_2, table_4);

  EXPECT_EQ(5U, table_cache_manager.cached_tables());

  // There should be assert failure since we are trying
  // to free all not own TABLEs for share_1, while thd_2
  // has a TABLE object for it in used
#ifndef NDEBUG
  EXPECT_DEATH_IF_SUPPORTED(
      table_cache_manager.free_table(thd_1, TDC_RT_REMOVE_NOT_OWN, &share_1),
      ".*Assertion.*0.*");
#endif

  table_cache_2->release_table(thd_2, table_4);

  // After TABLE owned by thd_2 is marked as unused, the below
  // call should succeed.
  table_cache_manager.free_table(thd_1, TDC_RT_REMOVE_NOT_OWN, &share_1);

  // We still have 1 TABLE object for share_1 in thd_1 and
  // 2 TABLE objects for share_2.
  EXPECT_EQ(3U, table_cache_manager.cached_tables());

  /*
    Coverage for TDC_RT_REMOVE_UNUSED case.
  */
  table_2 = share_1.create_table(thd_1);
  table_4 = share_1.create_table(thd_2);

  add_used_table(table_cache_1, thd_1, table_2);
  table_cache_1->release_table(thd_1, table_2);
  add_used_table(table_cache_2, thd_2, table_4);

  EXPECT_EQ(5U, table_cache_manager.cached_tables());

  table_cache_manager.free_table(thd_1, TDC_RT_REMOVE_UNUSED, &share_1);

  // The above call should have been freed only 1 table.
  EXPECT_EQ(4U, table_cache_manager.cached_tables());

  // Mark all remaining TABLE objects for share_1 as unused
  table_cache_1->release_table(thd_1, table_1);
  table_cache_2->release_table(thd_2, table_4);

  table_cache_manager.free_table(thd_1, TDC_RT_REMOVE_UNUSED, &share_1);

  // The above call should free all unused TABLE objects for share_1.
  // Therefore only 2 objects for share_2 should be remaining
  EXPECT_EQ(2U, table_cache_manager.cached_tables());

  // Clean-up.
  table_cache_1->remove_table(table_3);
  table_cache_2->remove_table(table_5);

  share_2.destroy_table(table_3);
  share_2.destroy_table(table_5);

  table_cache_manager.unlock_all_and_tdc();
}

/*
  Coverage for Table_cache_iterator
*/

TEST_F(TableCacheDoubleCacheTest, Iterator) {
  THD *thd_1 = get_thd(0);
  THD *thd_2 = get_thd(1);

  table_cache_manager.lock_all_and_tdc();

  Mock_share share_1("share_1");
  Mock_share share_2("share_2");

  // There is no TABLE objects for share_1 so the below iterator
  // should not find anything.
  Table_cache_iterator it(&share_1);
  EXPECT_TRUE(it++ == nullptr);
  // Attempt to iterate behind the end should not give anything.
  EXPECT_TRUE(it++ == nullptr);

  Table_cache *table_cache_1 = table_cache_manager.get_cache(thd_1);
  Table_cache *table_cache_2 = table_cache_manager.get_cache(thd_2);
  TABLE *table_1 = share_1.create_table(thd_1);
  TABLE *table_2 = share_1.create_table(thd_1);
  TABLE *table_3 = share_2.create_table(thd_1);
  TABLE *table_4 = share_1.create_table(thd_2);
  TABLE *table_5 = share_2.create_table(thd_2);

  add_used_table(table_cache_2, thd_2, table_4);

  // Now the iterato should see table_4.
  it.rewind();
  TABLE *table_r1 = it++;
  EXPECT_TRUE(table_r1 == table_4);
  // But only it.
  EXPECT_TRUE(it++ == nullptr);
  EXPECT_TRUE(it++ == nullptr);

  add_used_table(table_cache_1, thd_1, table_1);

  // Now we should see two tables:
  it.rewind();
  table_r1 = it++;
  EXPECT_TRUE(table_r1 != nullptr);
  TABLE *table_r2 = it++;
  EXPECT_TRUE(table_r2 != nullptr);
  EXPECT_TRUE(table_r1 != table_r2);
  EXPECT_TRUE(it++ == nullptr);
  EXPECT_TRUE(it++ == nullptr);

  add_used_table(table_cache_1, thd_1, table_2);

  // And now three !
  it.rewind();
  table_r1 = it++;
  EXPECT_TRUE(table_r1 != nullptr);
  table_r2 = it++;
  EXPECT_TRUE(table_r2 != nullptr);
  TABLE *table_r3 = it++;
  EXPECT_TRUE(table_r3 != nullptr);
  EXPECT_TRUE(table_r1 != table_r2 && table_r1 != table_r3 &&
              table_r2 != table_r3);
  EXPECT_TRUE(it++ == nullptr);
  EXPECT_TRUE(it++ == nullptr);

  table_cache_1->release_table(thd_1, table_1);

  // We should be seeing only used TABLE objects, so two tables now
  it.rewind();
  table_r1 = it++;
  EXPECT_TRUE(table_r1 != nullptr);
  table_r2 = it++;
  EXPECT_TRUE(table_r2 != nullptr);
  EXPECT_TRUE(table_r1 != table_r2);
  EXPECT_TRUE(it++ == nullptr);
  EXPECT_TRUE(it++ == nullptr);

  add_used_table(table_cache_1, thd_1, table_3);
  add_used_table(table_cache_2, thd_2, table_5);

  // We also should not be seeing TABLE objects for share_2
  it.rewind();
  table_r1 = it++;
  EXPECT_TRUE(table_r1 != nullptr);
  table_r2 = it++;
  EXPECT_TRUE(table_r2 != nullptr);
  EXPECT_TRUE(table_r1 != table_r2);
  EXPECT_TRUE(it++ == nullptr);
  EXPECT_TRUE(it++ == nullptr);

  table_cache_1->remove_table(table_2);

  // Now we should se only one used TABLE
  it.rewind();
  table_r1 = it++;
  EXPECT_TRUE(table_r1 == table_4);
  EXPECT_TRUE(it++ == nullptr);
  EXPECT_TRUE(it++ == nullptr);

  table_cache_1->remove_table(table_4);

  // And now no used TABLE objects for share_1 at all
  it.rewind();
  EXPECT_TRUE(it++ == nullptr);
  EXPECT_TRUE(it++ == nullptr);

  table_cache_1->remove_table(table_1);

  // Still the same
  it.rewind();
  EXPECT_TRUE(it++ == nullptr);
  EXPECT_TRUE(it++ == nullptr);

  table_cache_1->remove_table(table_3);
  table_cache_2->remove_table(table_5);

  // Cache is empty so iterator should not show any TABLE objects.
  it.rewind();
  EXPECT_TRUE(it++ == nullptr);
  EXPECT_TRUE(it++ == nullptr);

  table_cache_manager.unlock_all_and_tdc();

  share_1.destroy_table(table_1);
  share_1.destroy_table(table_2);
  share_2.destroy_table(table_3);
  share_1.destroy_table(table_4);
  share_2.destroy_table(table_5);
}

}  // namespace table_cache_unittest
