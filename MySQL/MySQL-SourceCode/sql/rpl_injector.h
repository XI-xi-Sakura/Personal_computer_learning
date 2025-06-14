/* Copyright (c) 2006, 2025, Oracle and/or its affiliates.

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

#ifndef INJECTOR_H
#define INJECTOR_H

#include <stddef.h>
#include <string_view>

#include "my_dbug.h"
#include "my_inttypes.h"
#include "my_io.h"      // FN_REFLEN
#include "sql/table.h"  // TABLE

class THD;
struct MY_BITMAP;

/*
  Injector to inject rows into the MySQL server.

  The injector class is used to notify the MySQL server of new rows that have
  appeared outside of MySQL control.

  The original purpose of this is to allow clusters---which handle replication
  inside the cluster through other means---to insert new rows into binary log.
  Note, however, that the injector should be used whenever rows are altered in
  any manner that is outside of MySQL server visibility and which therefore
  are not seen by the MySQL server.
 */
class injector {
 public:
  /*
    Get an instance of the injector.

    DESCRIPTION
      The injector is a Singleton, so this static function return the
      available instance of the injector.

    RETURN VALUE
      A pointer to the available injector object.
  */
  static injector *instance();

  /*
    Delete the singleton instance (if allocated). Used during server shutdown.
  */
  static void free_instance();

  /*
    A transaction where rows can be added.

    DESCRIPTION
      The transaction class satisfy the **CopyConstructible** and
      **Assignable** requirements.  Note that the transaction is *not*
      default constructible.
   */
  class transaction {
   public:
    /* Convenience definitions */
    typedef uchar *record_type;
    typedef uint32 server_id_type;

    /*
      Table reference.

      RESPONSIBILITY

        The class contains constructors to handle several forms of
        references to tables.  The constructors can implicitly be used to
        construct references from, e.g., strings containing table names.

      EXAMPLE

        The class is intended to be used *by value*.  Please, do not try to
        construct objects of this type using 'new'; instead construct an
        object, possibly a temporary object.  For example:

          injector::transaction::table tbl(share->table, true);
          MY_BITMAP cols;
          bitmap_init(&cols, NULL, (i + 7) / 8, false);
          inj->write_row(::server_id, tbl, &cols, row_data);

        or

          MY_BITMAP cols;
          bitmap_init(&cols, NULL, (i + 7) / 8, false);
          inj->write_row(::server_id,
                         injector::transaction::table(share->table, true),
                         &cols, row_data);

        This will work, be more efficient, and have greater chance of
        inlining, not run the risk of losing pointers.

      COLLABORATION

        injector::transaction
          Provide a flexible interface to the representation of tables.

    */
    class table {
     public:
      class save_sets {
       public:
        save_sets(table const &tbl, MY_BITMAP const *new_rs,
                  MY_BITMAP const *new_ws)
            : m_table(tbl.get_table()),
              save_read_set(m_table->read_set),
              save_write_set(m_table->write_set) {
          m_table->column_bitmaps_set_no_signal(
              const_cast<MY_BITMAP *>(new_rs), const_cast<MY_BITMAP *>(new_ws));
        }

        ~save_sets() {
          m_table->column_bitmaps_set_no_signal(save_read_set, save_write_set);
        }

       private:
        TABLE *const m_table;
        MY_BITMAP *save_read_set;
        MY_BITMAP *save_write_set;
      };

      table(TABLE *table, bool is_transactional, bool skip_hash = false)
          : m_table(table),
            m_is_transactional(is_transactional),
            m_skip_hash(skip_hash) {}

      TABLE *get_table() const { return m_table; }
      bool is_transactional() const { return m_is_transactional; }
      bool skip_hash() const { return m_skip_hash; }

     private:
      TABLE *const m_table;
      const bool m_is_transactional;
      const bool m_skip_hash;
    };

    /*
      Binlog position as a structure.
    */
    class binlog_pos {
      friend class transaction;

     public:
      char const *file_name() const { return m_file_name; }
      my_off_t file_pos() const { return m_file_pos; }

     private:
      char const *m_file_name;
      my_off_t m_file_pos;
    };

    /*
      Create a new transaction.

      The parameter "calc_writeset_hash" controls whether writeset hashes for
      transaction dependencies will be calculated for rows added to the
      transaction, without this the list of hashes will be empty and thus no
      dependencies are detected. The ability to control this is important where
      MTA is not used, calculating writeset hashes is wasted work, and in NDB
      all binlog content is processed by a single thread and CPU consumption
      might be a bottleneck.
    */
    transaction(THD *thd, bool calc_writeset_hash);
    transaction(const transaction &) = delete;
    transaction(const transaction &&) = delete;
    transaction &operator=(const transaction &) = delete;
    transaction &operator=(const transaction &&) = delete;

    ~transaction() {}

    /*

      DESCRIPTION

        Register table for use within the transaction.  All tables
        that are going to be used need to be registered before being
        used below.  The member function will fail with an error if
        use_table() is called after any *_row() function has been
        called for the transaction.

      RETURN VALUE

        0         All OK
        >0        Failure

     */
    int use_table(server_id_type sid, table tbl);

    /*
      Add a 'write row' entry to the transaction.
    */
    int write_row(server_id_type sid, table tbl, MY_BITMAP const *cols,
                  record_type record, const unsigned char *extra_row_info);

    /*
      Add a 'delete row' entry to the transaction.
    */
    int delete_row(server_id_type sid, table tbl, MY_BITMAP const *cols,
                   record_type record, const unsigned char *extra_row_info);
    /*
      Add an 'update row' entry to the transaction.
    */
    int update_row(server_id_type sid, table tbl, MY_BITMAP const *before_cols,
                   MY_BITMAP const *after_cols, record_type before,
                   record_type after, const unsigned char *extra_row_info);

    /*
      Commit a transaction.

      This member function will clean up after a sequence of *_row calls by,
      for example, releasing resource and unlocking files.
    */
    int commit();

    /*
      Rollback a transaction.

      This member function will clean up after a sequence of *_row calls by,
      for example, releasing resource and unlocking files.
    */
    int rollback();

    /*
      Get the position for the start of the transaction.

      This is the current 'tail of Binlog' at the time the transaction
      was started.  The first event recorded by the transaction may
      be at this, or some subsequent position.  The first event recorded
      by the transaction will not be before this position.
    */
    binlog_pos start_pos() const;

    /*
      Get the next position after the end of the transaction

      This call is only valid after a transaction has been committed.
      It returns the next Binlog position after the committed transaction.
      It is guaranteed that no other events will be recorded between the
      COMMIT event of the Binlog transaction, and this position.
      Note that this position may be in a different log file to the COMMIT
      event.

      If the commit had an error, or the transaction was empty and nothing
      was binlogged then the next_pos will have a NULL file_name(), and
      0 file_pos().

    */
    binlog_pos next_pos() const;

   private:
    enum enum_state {
      START_STATE, /* Start state */
      TABLE_STATE, /* At least one table has been registered */
      ROW_STATE,   /* At least one row has been registered */
      STATE_COUNT  /* State count and sink state */
    } m_state;

    /*
      Check and update the state.

      PARAMETER(S)

        target_state
            The state we are moving to: TABLE_STATE if we are
            writing a table and ROW_STATE if we are writing a row.

      DESCRIPTION

        The internal state will be updated to the target state if
        and only if it is a legal move.  The only legal moves are:

            START_STATE -> START_STATE
            START_STATE -> TABLE_STATE
            TABLE_STATE -> TABLE_STATE
            TABLE_STATE -> ROW_STATE

        That is:
        - It is not possible to write any row before having written at
          least one table
        - It is not possible to write a table after at least one row
          has been written

      RETURN VALUE

         0    All OK
        -1    Incorrect call sequence
     */
    int check_state(enum_state const target_state) {
#ifndef NDEBUG
      static char const *state_name[] = {"START_STATE", "TABLE_STATE",
                                         "ROW_STATE", "STATE_COUNT"};

      assert(0 <= target_state && target_state <= STATE_COUNT);
      DBUG_PRINT("info", ("In state %s", state_name[m_state]));
#endif

      if (m_state <= target_state && target_state <= m_state + 1 &&
          m_state < STATE_COUNT)
        m_state = target_state;
      else
        m_state = STATE_COUNT;
      return m_state == STATE_COUNT ? 1 : 0;
    }

    char m_start_name_buf[FN_REFLEN];
    char m_end_name_buf[FN_REFLEN];
    binlog_pos m_start_pos;
    binlog_pos m_next_pos;
    THD *const m_thd;
    const bool m_calc_writeset_hash{false};
  };

  int record_incident(THD *, std::string_view message);

 private:
  explicit injector();
  ~injector() = default;      /* Nothing needs to be done */
  injector(injector const &); /* You're not allowed to copy injector
                                 instances.
                              */
};

#endif /* INJECTOR_H */
