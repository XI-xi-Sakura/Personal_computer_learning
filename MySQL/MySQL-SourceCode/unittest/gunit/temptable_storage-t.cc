/* Copyright (c) 2016, 2025, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <gtest/gtest.h>
#include <thread>

#include "storage/temptable/include/temptable/allocator.h"
#include "storage/temptable/include/temptable/block.h"
#include "storage/temptable/include/temptable/storage.h"

namespace temptable_storage_unittest {

TEST(StorageTest, Iterate) {
  /* Set appropriate temptable_max_mmap */
  temptable_use_mmap = 1;
  temptable_max_mmap = 1073741824;

  std::thread t([]() {
    temptable::TableResourceMonitor table_resource_monitor(16 * 1024 * 1024);
    temptable::Block shared_block;
    temptable::Allocator<uint8_t> allocator(&shared_block,
                                            table_resource_monitor);
    {
      temptable::Storage storage(&allocator);

      storage.element_size(sizeof(uint64_t));

      for (uint64_t i = 0; i < 10000; ++i) {
        *static_cast<uint64_t *>(storage.allocate_back()) = i;
      }

      uint64_t i = 0;
      for (auto it = storage.begin(); it != storage.end(); ++it, ++i) {
        EXPECT_EQ(i, *static_cast<uint64_t *>(*it));
      }

      i = storage.size();
      auto it = storage.end();
      for (; it != storage.begin();) {
        --it;
        --i;
        EXPECT_EQ(i, *static_cast<uint64_t *>(*it));
      }
      EXPECT_EQ(0u, i);
    }
    shared_block.destroy();
  });
  t.join();
}

TEST(StorageTest, AllocatorRebind) {
  // Bug in VS2019 error C3409 if we do the same as above.
  // Turns out it is the rebind which confuses the compiler.

  /* Set appropriate temptable_max_mmap */
  temptable_use_mmap = 1;
  temptable_max_mmap = 1073741824;

  auto thread_function = []() {
    temptable::TableResourceMonitor table_resource_monitor(16 * 1024 * 1024);
    temptable::Block shared_block;
    temptable::Allocator<uint8_t> alloc(&shared_block, table_resource_monitor);
    uint8_t *shared_eater = alloc.allocate(
        1048576);  // Make sure to consume the initial shared block.
    uint8_t *ptr = alloc.allocate(100);

    decltype(alloc)::rebind<uint32_t>::other rebound_alloc(alloc);

    alloc.deallocate(ptr, 100);

    uint32_t *ptr2 = rebound_alloc.allocate(50);
    rebound_alloc.deallocate(ptr2, 50);

    alloc.deallocate(shared_eater, 1048576);

    shared_block.destroy();
  };
  std::thread t(thread_function);
  t.join();
}

} /* namespace temptable_storage_unittest */
