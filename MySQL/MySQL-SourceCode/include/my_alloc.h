/* Copyright (c) 2000, 2025, Oracle and/or its affiliates.

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
 * @file include/my_alloc.h
 *
 * This file follows Google coding style, except for the name MEM_ROOT (which is
 * kept for historical reasons).
 */

#ifndef INCLUDE_MY_ALLOC_H_
#define INCLUDE_MY_ALLOC_H_

#include <string.h>

#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "memory_debugging.h"
#include "my_compiler.h"
#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_pointer_arithmetic.h"
#include "mysql/psi/psi_memory.h"

#if defined(MYSQL_SERVER)
extern "C" void sql_alloc_error_handler();
#endif

/**
 * The MEM_ROOT is a simple arena, where allocations are carved out of
 * larger blocks. Using an arena over plain malloc gives you two main
 * advantages:
 *
 *   * Allocation is very cheap (only a few CPU cycles on the fast path).
 *   * You do not need to keep track of which memory you have allocated,
 *     as it will all be freed when the arena is destroyed.
 *
 * Thus, if you need to do many small allocations that all are to have
 * roughly the same lifetime, the MEM_ROOT is probably a good choice.
 * The flip side is that _no_ memory is freed until the arena is destroyed,
 * and no destructors are run (although you can run them manually yourself).
 *
 *
 * This specific implementation works by allocating exponentially larger blocks
 * each time it needs more memory (generally increasing them by 50%), which
 * guarantees O(1) total calls to malloc and free. Only one free block is
 * ever used; as soon as there's an allocation that comes in that doesn't fit,
 * that block is stored away and never allocated from again. (There's an
 * exception for allocations larger than the block size; see #AllocSlow
 * for details.)
 *
 * The MEM_ROOT is thread-compatible but not thread-safe. This means you cannot
 * use the same instance from multiple threads at the same time without external
 * synchronization, but you can use different MEM_ROOTs concurrently in
 * different threads.
 *
 * For C compatibility reasons, MEM_ROOT is a struct, even though it is
 * logically a class and follows the style guide for classes.
 */
struct MEM_ROOT {
 private:
  struct Block {
    Block *prev{nullptr}; /** Previous block; used for freeing. */
    char *end{nullptr};   /** One byte past the end; used for Contains(). */
  };

 public:
  MEM_ROOT() : MEM_ROOT(0, 512) {}  // 0 = PSI_NOT_INSTRUMENTED.

  MEM_ROOT(PSI_memory_key key, size_t block_size)
      : m_block_size(block_size),
        m_orig_block_size(block_size),
        m_psi_key(key) {
#if defined(MYSQL_SERVER)
    m_error_handler = sql_alloc_error_handler;
#endif
  }

  // MEM_ROOT is movable but not copyable.
  MEM_ROOT(const MEM_ROOT &) = delete;
  MEM_ROOT(MEM_ROOT &&other)
  noexcept
      : m_current_block(other.m_current_block),
        m_current_free_start(other.m_current_free_start),
        m_current_free_end(other.m_current_free_end),
        m_block_size(other.m_block_size),
        m_orig_block_size(other.m_orig_block_size),
        m_max_capacity(other.m_max_capacity),
        m_allocated_size(other.m_allocated_size),
        m_error_for_capacity_exceeded(other.m_error_for_capacity_exceeded),
        m_error_handler(other.m_error_handler),
        m_psi_key(other.m_psi_key) {
    other.m_current_block = nullptr;
    other.m_allocated_size = 0;
    other.m_block_size = m_orig_block_size;
    other.m_current_free_start = &s_dummy_target;
    other.m_current_free_end = &s_dummy_target;
  }

  MEM_ROOT &operator=(const MEM_ROOT &) = delete;
  MEM_ROOT &operator=(MEM_ROOT &&other) noexcept {
    Clear();
    ::new (this) MEM_ROOT(std::move(other));
    return *this;
  }

  ~MEM_ROOT() { Clear(); }

  /**
   * Allocate memory. Will return nullptr if there's not enough memory,
   * or if the maximum capacity is reached.
   *
   * Note that a zero-length allocation can return _any_ pointer, including
   * nullptr or a pointer that has been given out before. The current
   * implementation takes some pains to make sure we never return nullptr
   * (although it might return a bogus pointer), since there is code that
   * assumes nullptr always means 'out of memory', but you should not rely on
   * it, as it may change in the future.
   *
   * The returned pointer will always be 8-aligned.
   */
  void *Alloc(size_t length) MY_ATTRIBUTE((malloc)) {
    length = ALIGN_SIZE(length);

    // Skip the straight path if simulating OOM; it should always fail.
    DBUG_EXECUTE_IF("simulate_out_of_memory", return AllocSlow(length););

    // Fast path, used in the majority of cases. It would be faster here
    // (saving one register due to CSE) to instead test
    //
    //   m_current_free_start + length <= m_current_free_end
    //
    // but it would invoke undefined behavior, and in particular be prone
    // to wraparound on 32-bit platforms.
    if (static_cast<size_t>(m_current_free_end - m_current_free_start) >=
        length) {
      void *ret = m_current_free_start;
      m_current_free_start += length;
      return ret;
    }

    return AllocSlow(length);
  }

  /**
    Allocate 'num' objects of type T, and initialize them to a default value
    that is created by passing the supplied args to T's constructor. If args
    is empty, value-initialization is used. For primitive types, like int and
    pointers, this means the elements will be set to the equivalent of 0
    (or false or nullptr).

    If the constructor throws an exception, behavior is undefined.

    We don't use new[], as it can put extra data in front of the array.
   */
  template <class T, class... Args>
  T *ArrayAlloc(size_t num, Args... args) {
    static_assert(alignof(T) <= 8, "MEM_ROOT only returns 8-aligned memory.");
    if (num * sizeof(T) < num) {
      // Overflow.
      return nullptr;
    }
    T *ret = static_cast<T *>(Alloc(num * sizeof(T)));
    if (ret == nullptr) {
      // Out of memory.
      return nullptr;
    }

    // Initialize all elements.
    for (size_t i = 0; i < num; ++i) {
      ::new (&ret[i]) T(args...);
    }

    return ret;
  }

  /**
   * Claim all the allocated memory for the current thread in the performance
   * schema. Use when transferring responsibility for a MEM_ROOT from one thread
   * to another.
   */
  void Claim(bool claim);

  /**
   * Deallocate all the RAM used. The MEM_ROOT itself continues to be valid,
   * so you can make new calls to Alloc() afterwards.

   * @note
   *   One can call this function either with a MEM_ROOT initialized with the
   *   constructor, or with one that's memset() to all zeros.
   *   It's also safe to call this multiple times with the same mem_root.
   */
  void Clear();

  /**
   * Similar to Clear(), but anticipates that the block will be reused for
   * further allocations. This means that even though all the data is gone,
   * one memory block (typically the largest allocated) will be kept and
   * made immediately available for calls to Alloc() without having to go to the
   * OS for new memory. This can yield performance gains if you use the same
   * MEM_ROOT many times. Also, the block size is not reset.
   */
  void ClearForReuse();

  /**
    Whether the constructor has run or not.

    This exists solely to support legacy code that memset()s the MEM_ROOT to
    all zeros, which wants to distinguish between that state and a properly
    initialized MEM_ROOT. If you do not run the constructor _nor_ do memset(),
    you are invoking undefined behavior.
  */
  bool inited() const { return m_block_size != 0; }

  /**
   * Set maximum capacity for this MEM_ROOT. Whenever the MEM_ROOT has
   * allocated more than this (not including overhead), and the free block
   * is empty, future allocations will fail.
   *
   * @param max_capacity        Maximum capacity this mem_root can hold
   */
  void set_max_capacity(size_t max_capacity) { m_max_capacity = max_capacity; }

  /**
   * Return maximum capacity for this MEM_ROOT.
   */
  size_t get_max_capacity() const { return m_max_capacity; }

  /**
   * Enable/disable error reporting for exceeding the maximum capacity.
   * If error reporting is enabled, an error is flagged to indicate that the
   * capacity is exceeded. However, allocation will still happen for the
   * requested memory.
   *
   * @param report_error    whether the error should be reported
   */
  void set_error_for_capacity_exceeded(bool report_error) {
    m_error_for_capacity_exceeded = report_error;
  }

  /**
   * Return whether error is to be reported when
   * maximum capacity exceeds for MEM_ROOT.
   */
  bool get_error_for_capacity_exceeded() const {
    return m_error_for_capacity_exceeded;
  }

  /**
   * Set the error handler on memory allocation failure (or nullptr for none).
   * The error handler is called called whenever my_malloc() failed to allocate
   * more memory from the OS (which causes my_alloc() to return nullptr).
   */
  void set_error_handler(void (*error_handler)(void)) {
    m_error_handler = error_handler;
  }

  /**
   * Amount of memory we have allocated from the operating system, not including
   * overhead.
   */
  size_t allocated_size() const { return m_allocated_size; }

  /**
   * Set the desired size of the next block to be allocated. Note that future
   * allocations
   * will grow in size over this, although a Clear() will reset the size again.
   */
  void set_block_size(size_t block_size) {
    m_block_size = m_orig_block_size = block_size;
  }

  /**
   * @name Raw interface
   * Peek(), ForceNewBlock() and RawCommit() together define an
   * alternative interface to MEM_ROOT, for special uses. The raw interface
   * gives direct access to the underlying blocks, allowing a user to bypass the
   * normal alignment requirements and to write data directly into them without
   * knowing beforehand exactly how long said data is going to be, while still
   * retaining the convenience of block management and automatic freeing. It
   * generally cannot be combined with calling Alloc() as normal; see RawCommit.
   *
   * The raw interface, unlike Alloc(), is not affected by running under
   * ASan or Valgrind.
   *
   * @{
   */

  /**
   * Get the bounds of the currently allocated memory block. Assuming no other
   * MEM_ROOT calls are made in the meantime, you can start writing into this
   * block and then call RawCommit() once you know how many bytes you actually
   * needed. (This is useful when e.g. packing rows.)
   */
  std::pair<char *, char *> Peek() const {
    return {m_current_free_start, m_current_free_end};
  }

  /**
   * Allocate a new block of at least 'minimum_length' bytes; usually more.
   * This holds no matter how many bytes are free in the current block.
   * The new block will always become the current block, ie., the next call
   * to Peek() will return the newly allocated block. (This is different
   * from Alloc(), where it is possible to allocate a new block that is
   * not made into the current block.)
   *
   * @return true Allocation failed (possibly due to size restrictions).
   */
  bool ForceNewBlock(size_t minimum_length);

  /**
   * Mark the first N bytes as the current block as used.
   *
   * WARNING: If you use RawCommit() with a length that is not a multiple of 8,
   * you cannot use Alloc() afterwards! The exception is that if EnsureSpace()
   * has just returned, you've got a new block, and can use Alloc() again.
   */
  void RawCommit(size_t length) {
    assert(static_cast<size_t>(m_current_free_end - m_current_free_start) >=
           length);
    m_current_free_start += length;
  }

  /**
   * Returns whether this MEM_ROOT contains the given pointer,
   * ie., whether it was given back from Alloc(n) (given n >= 1)
   * at some point. This means it will be legally accessible until
   * the next Clear() or ClearForReuse() call.
   */
  bool Contains(void *ptr) const {
    for (Block *block = m_current_block; block != nullptr;
         block = block->prev) {
      if (ptr >= block && ptr < block->end) {
        return true;
      }
    }
    return false;
  }

  bool IsSingleBlock() const {
    return m_current_block != nullptr && m_current_block->prev == nullptr;
  }
  /// @}

 private:
  /**
   * Something to point on that exists solely to never return nullptr
   * from Alloc(0).
   */
  static char s_dummy_target;

  /**
    Allocate a new block of the given length (plus overhead for the block
    header). If the MEM_ROOT is near capacity, it may allocate less memory
    than wanted_length, but if it cannot allocate at least minimum_length,
    will return nullptr.
  */
  Block *AllocBlock(size_t wanted_length, size_t minimum_length);

  /** Allocate memory that doesn't fit into the current free block. */
  void *AllocSlow(size_t length);

  /** Free all blocks in a linked list, starting at the given block. */
  static void FreeBlocks(Block *start);

  /** The current block we are giving out memory from. nullptr if none. */
  Block *m_current_block = nullptr;

  /** Start (inclusive) of the current free block. */
  char *m_current_free_start = &s_dummy_target;

  /** End (exclusive) of the current free block. */
  char *m_current_free_end = &s_dummy_target;

  /** Size of the _next_ block we intend to allocate. */
  size_t m_block_size;

  /** The original block size the user asked for on construction. */
  size_t m_orig_block_size;

  /**
    Maximum amount of memory this MEM_ROOT can hold. A value of 0
    implies there is no limit.
  */
  size_t m_max_capacity = 0;

  /**
   * Total allocated size for this MEM_ROOT. Does not include overhead
   * for block headers or malloc overhead, since especially the latter
   * is impossible to quantify portably.
   */
  size_t m_allocated_size = 0;

  /** If enabled, exceeding the capacity will lead to a my_error() call. */
  bool m_error_for_capacity_exceeded = false;

  void (*m_error_handler)(void) = nullptr;

  PSI_memory_key m_psi_key = 0;
};

/**
 * Allocate an object of the given type. Use like this:
 *
 *   Foo *foo = new (mem_root) Foo();
 *
 * Note that unlike regular operator new, this will not throw exceptions.
 * However, it can return nullptr if the capacity of the MEM_ROOT has been
 * reached. This is allowed since it is not a replacement for global operator
 * new, and thus isn't used automatically by e.g. standard library containers.
 *
 * TODO: This syntax is confusing in that it could look like allocating
 * a MEM_ROOT using regular placement new. We should make a less ambiguous
 * syntax, e.g. new (On(mem_root)) Foo().
 */
inline void *operator new(size_t size, MEM_ROOT *mem_root,
                          const std::nothrow_t &arg
                          [[maybe_unused]] = std::nothrow) noexcept {
  return mem_root->Alloc(size);
}

inline void *operator new[](size_t size, MEM_ROOT *mem_root,
                            const std::nothrow_t &arg
                            [[maybe_unused]] = std::nothrow) noexcept {
  return mem_root->Alloc(size);
}

inline void operator delete(void *, MEM_ROOT *,
                            const std::nothrow_t &) noexcept {
  /* never called */
}

inline void operator delete[](void *, MEM_ROOT *,
                              const std::nothrow_t &) noexcept {
  /* never called */
}

template <class T>
inline void destroy_at(T *ptr) {
  assert(ptr != nullptr);
  std::destroy_at(ptr);
  TRASH(const_cast<std::remove_const_t<T> *>(ptr), sizeof(T));
}

/*
 * For std::unique_ptr with objects allocated on a MEM_ROOT, you shouldn't use
 * Default_deleter; use this deleter instead.
 */
template <class T>
class Destroy_only {
 public:
  void operator()(T *ptr) const { ::destroy_at(ptr); }
};

/** std::unique_ptr, but only destroying. */
template <class T>
using unique_ptr_destroy_only = std::unique_ptr<T, Destroy_only<T>>;

template <typename T, typename... Args>
unique_ptr_destroy_only<T> make_unique_destroy_only(MEM_ROOT *mem_root,
                                                    Args &&...args) {
  return unique_ptr_destroy_only<T>(new (mem_root)
                                        T(std::forward<Args>(args)...));
}

#endif  // INCLUDE_MY_ALLOC_H_
