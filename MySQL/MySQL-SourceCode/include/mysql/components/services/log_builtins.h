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

/**
  This defines built-in functions for use by logging services.
  These helpers are organized into a number of APIs grouping
  related functionality.

  For documentation of the individual functions, see log_builtins.cc
*/

#ifndef LOG_BUILTINS_H
#define LOG_BUILTINS_H

#include <mysql/components/component_implementation.h>
#include <mysql/components/my_service.h>
#include <mysql/components/service_implementation.h>
#include <mysql/components/services/log_service.h>
#include <mysql/components/services/log_shared.h>
#if defined(MYSQL_DYNAMIC_PLUGIN)
#include <mysql/service_plugin_registry.h>
#endif
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <my_compiler.h>
#if defined(MYSQL_SERVER) && !defined(MYSQL_DYNAMIC_PLUGIN)
#include "sql/log.h"
#endif

/**
  typedef for log-processing functions ("buffer this event",
  "process this event", etc.)
*/
typedef bool (*log_line_processor)(log_line *ll);

/**
  Set the log-event processor.

  When a log-event is submitted, a function is applied to that event.
  That function usually either buffers the event for later processing,
  or filters and logs the event.

  That function can be set here.

  @param llp   A log-processor
*/
extern void log_line_process_hook_set(log_line_processor llp);

/**
  Get current log-event processor.

  When a log-event is submitted, a function is applied to that event.
  That function usually either buffers the event for later processing,
  or filters and logs the event.
  log_line_process_hook_get() returns a pointer to that function.

  @retval      a pointer to a log-event processing function
*/
log_line_processor log_line_process_hook_get(void);

/**
  Primitives for services to interact with the structured logger:
  functions pertaining to log_line and log_item data
*/
BEGIN_SERVICE_DEFINITION(log_builtins)
/**
  See whether a type is wellknown.

  @param t       log item type to examine

  @retval        LOG_ITEM_TYPE_NOT_FOUND: key not found
  @retval        >0:                      index in array of wellknowns
*/
DECLARE_METHOD(int, wellknown_by_type, (log_item_type t));

/**
  See whether a string is a wellknown field name.

  @param key     potential key starts here
  @param length  length of the string to examine

  @retval        LOG_ITEM_TYPE_RESERVED:  reserved, but not "wellknown" key
  @retval        LOG_ITEM_TYPE_NOT_FOUND: key not found
  @retval        >0:                      index in array of wellknowns
*/
DECLARE_METHOD(int, wellknown_by_name, (const char *key, size_t length));

/**
  Accessor: from a record describing a wellknown key, get its type

  @param idx     index in array of wellknowns, see log_item_wellknown_by_...()

  @retval        the log item type for the wellknown key
*/
DECLARE_METHOD(log_item_type, wellknown_get_type, (uint idx));

/**
  Accessor: from a record describing a wellknown key, get its name

  @param   idx  index in array of wellknowns, see log_item_wellknown_by_...()

  @retval       name (NTBS)
*/
DECLARE_METHOD(const char *, wellknown_get_name, (uint idx));

/**
  Sanity check an item.
  Certain log sinks have very low requirements with regard to the data
  they receive; they write keys as strings, and then data according to
  the item's class (string, integer, or float), formatted to the sink's
  standards (e.g. JSON, XML, ...).
  Code that has higher requirements can use this check to see whether
  the given item is of a known type (whether generic or wellknown),
  whether the given type and class agree, and whether in case of a
  well-known type, the given key is correct for that type.
  If your code generates items that don't pass this check, you should
  probably go meditate on it.

  @param  li  the log_item to check

  @retval LOG_ITEM_OK              no problems
  @retval LOG_ITEM_TYPE_NOT_FOUND  unknown item type
  @retval LOG_ITEM_CLASS_MISMATCH  item_class derived from type isn't
                                   what's set on the item
  @retval LOG_ITEM_KEY_MISMATCH    class not generic, so key should
                                   match wellknown
  @retval LOG_ITEM_STRING_NULL     class is string, pointer is nullptr
  @retval LOG_ITEM_KEY_NULL        no key set (this is legal e.g. on aux
                                   items of filter rules, but should not
                                   occur in a log_line, i.e., log_sinks are
                                   within their rights to discard such items)
*/
DECLARE_METHOD(int, item_inconsistent, (log_item * li));

// helpers: predicates to find out about types and classes

/**
  Predicate used to determine whether a type is generic
  (generic string, generic float, generic integer) rather
  than a well-known type.

  @param t          log item type to examine

  @retval  true     if generic type
  @retval  false    if wellknown type
*/
DECLARE_METHOD(bool, item_generic_type, (log_item_type t));

/**
  Predicate used to determine whether a class is a string
  class (C-string or Lex-string).

  @param c          log item class to examine

  @retval   true    if of a string class
  @retval   false   if not of a string class
*/
DECLARE_METHOD(bool, item_string_class, (log_item_class c));

/**
  Predicate used to determine whether a class is a numeric
  class (integer or float).

  @param c         log item class to examine

  @retval   true   if of a numeric class
  @retval   false  if not of a numeric class
*/
DECLARE_METHOD(bool, item_numeric_class, (log_item_class c));

/**
  Set an integer value on a log_item.
  Fails gracefully if no log_item_data is supplied, so it can safely
  wrap log_line_item_set[_with_key]().

  @param  lid    log_item_data struct to set the value on
  @param  i      integer to set

  @retval true   lid was nullptr (possibly: OOM, could not set up log_item)
  @retval false  all's well
*/
DECLARE_METHOD(bool, item_set_int, (log_item_data * lid, longlong i));
/**
  Set a floating point value on a log_item.
  Fails gracefully if no log_item_data is supplied, so it can safely
  wrap log_line_item_set[_with_key]().

  @param  lid    log_item_data struct to set the value on
  @param  f      float to set

  @retval true   lid was nullptr (possibly: OOM, could not set up log_item)
  @retval false  all's well
*/
DECLARE_METHOD(bool, item_set_float, (log_item_data * lid, double f));

/**
  Set a string value on a log_item.
  Fails gracefully if no log_item_data is supplied, so it can safely
  wrap log_line_item_set[_with_key]().

  @param  lid    log_item_data struct to set the value on
  @param  s      pointer to string
  @param  s_len  length of string

  @retval true   lid was nullptr (possibly: OOM, could not set up log_item)
  @retval false  all's well
*/
DECLARE_METHOD(bool, item_set_lexstring,
               (log_item_data * lid, const char *s, size_t s_len));

/**
  Set a string value on a log_item.
  Fails gracefully if no log_item_data is supplied, so it can safely
  wrap log_line_item_set[_with_key]().

  @param  lid    log_item_data struct to set the value on
  @param  s      pointer to NTBS

  @retval true   lid was nullptr (possibly: OOM, could not set up log_item)
  @retval false  all's well
*/
DECLARE_METHOD(bool, item_set_cstring, (log_item_data * lid, const char *s));

/**
  Set/reset one or more log line flags.

  Example to set the flag:
    log_line_set_flag(ll, LOG_LINE_EMIT_TELEMETRY, LOG_LINE_EMIT_TELEMETRY);
  to reset the flag:
    log_line_set_flag(ll, LOG_LINE_EMIT_TELEMETRY, 0);
*/
DECLARE_METHOD(void, line_set_flag,
               (log_line * ll, log_line_flags_mask mask,
                log_line_flags_mask value));

/**
  Create new log item with key name "key", and allocation flags of
  "alloc" (see enum_log_item_free).
  Will return a pointer to the item's log_item_data struct for
  convenience.
  This is mostly interesting for filters and other services that create
  items that are not part of a log_line; sources etc. that intend to
  create an item for a log_line (the more common case) should usually
  use the below line_item_set_with_key() which creates an item (like
  this function does), but also correctly inserts it into a log_line.

  @param  li     the log_item to work on
  @param  t      the item-type
  @param  key    the key to set on the item.
                 ignored for non-generic types (may pass nullptr for those)
                 see alloc
  @param  alloc  LOG_ITEM_FREE_KEY  if key was allocated by caller
                 LOG_ITEM_FREE_NONE if key was not allocated
                 Allocated keys will automatically free()d when the
                 log_item is.
                 The log_item's alloc flags will be set to the
                 submitted value; specifically, any pre-existing
                 value will be clobbered.  It is therefore WRONG
                 a) to use this on a log_item that already has a key;
                    it should only be used on freshly init'd log_items;
                 b) to use this on a log_item that already has a
                    value (specifically, an allocated one); the correct
                    order is to init a log_item, then set up type and
                    key, and finally to set the value. If said value is
                    an allocated string, the log_item's alloc should be
                    bitwise or'd with LOG_ITEM_FREE_VALUE.

  @retval        a pointer to the log_item's log_data, for easy chaining:
                 log_item_set_with_key(...)->data_integer= 1;
*/
DECLARE_METHOD(log_item_data *, item_set_with_key,
               (log_item * li, log_item_type t, const char *key, uint32 alloc));

/**
  As log_item_set_with_key(), except that the key is automatically
  derived from the wellknown log_item_type t.

  Create new log item with type "t".
  Will return a pointer to the item's log_item_data struct for
  convenience.
  This is mostly interesting for filters and other services that create
  items that are not part of a log_line; sources etc. that intend to
  create an item for a log_line (the more common case) should usually
  use the below line_item_set_with_key() which creates an item (like
  this function does), but also correctly inserts it into a log_line.

  The allocation of this item will be LOG_ITEM_FREE_NONE;
  specifically, any pre-existing value will be clobbered.
  It is therefore WRONG
  a) to use this on a log_item that already has a key;
     it should only be used on freshly init'd log_items;
  b) to use this on a log_item that already has a
     value (specifically, an allocated one); the correct
     order is to init a log_item, then set up type and
     key, and finally to set the value. If said value is
     an allocated string, the log_item's alloc should be
     bitwise or'd with LOG_ITEM_FREE_VALUE.

  @param  li     the log_item to work on
  @param  t      the item-type

  @retval        a pointer to the log_item's log_data, for easy chaining:
                 log_item_set_with_key(...)->data_integer= 1;
*/
DECLARE_METHOD(log_item_data *, item_set, (log_item * li, log_item_type t));

/**
  Create new log item in log line "ll", with key name "key", and
  allocation flags of "alloc" (see enum_log_item_free).
  On success, the number of registered items on the log line is increased,
  the item's type is added to the log_line's "seen" property,
  and a pointer to the item's log_item_data struct is returned for
  convenience.

  @param  ll     the log_line to work on
  @param  t      the item-type
  @param  key    the key to set on the item.
                 ignored for non-generic types (may pass nullptr for those)
                 see alloc
  @param  alloc  LOG_ITEM_FREE_KEY  if key was allocated by caller
                 LOG_ITEM_FREE_NONE if key was not allocated
                 Allocated keys will automatically free()d when the
                 log_item is.
                 The log_item's alloc flags will be set to the
                 submitted value; specifically, any pre-existing
                 value will be clobbered.  It is therefore WRONG
                 a) to use this on a log_item that already has a key;
                    it should only be used on freshly init'd log_items;
                 b) to use this on a log_item that already has a
                    value (specifically, an allocated one); the correct
                    order is to init a log_item, then set up type and
                    key, and finally to set the value. If said value is
                    an allocated string, the log_item's alloc should be
                    bitwise or'd with LOG_ITEM_FREE_VALUE.

  @retval        a pointer to the log_item's log_data, for easy chaining:
                 log_line_item_set_with_key(...)->data_integer= 1;
*/
DECLARE_METHOD(log_item_data *, line_item_set_with_key,
               (log_line * ll, log_item_type t, const char *key, uint32 alloc));

/**
  Create a new log item of well-known type "t" in log line "ll".
  On success, the number of registered items on the log line is increased,
  the item's type is added to the log_line's "seen" property,
  and a pointer to the item's log_item_data struct is returned for
  convenience.

  The allocation of this item will be LOG_ITEM_FREE_NONE;
  specifically, any pre-existing value will be clobbered.
  It is therefore WRONG
  a) to use this on a log_item that already has a key;
     it should only be used on freshly init'd log_items;
  b) to use this on a log_item that already has a
     value (specifically, an allocated one); the correct
     order is to init a log_item, then set up type and
     key, and finally to set the value. If said value is
     an allocated string, the log_item's alloc should be
     bitwise or'd with LOG_ITEM_FREE_VALUE.

  @param  ll     the log_line to work on
  @param  t      the item-type

  @retval        a pointer to the log_item's log_data, for easy chaining:
                 log_line_item_set_with_key(...)->data_integer= 1;
*/
DECLARE_METHOD(log_item_data *, line_item_set,
               (log_line * ll, log_item_type t));

/**
  Dynamically allocate and initialize a log_line.

  @retval nullptr  could not set up buffer (too small?)
  @retval other    address of the newly initialized log_line
*/
DECLARE_METHOD(log_line *, line_init, ());

/**
  Release a log_line allocated with line_init()

  @param  ll       a log_line previously allocated with line_init()
*/
DECLARE_METHOD(void, line_exit, (log_line * ll));

/**
  How many items are currently set on the given log_line?

  @param   ll     the log-line to examine

  @retval         the number of items set
*/
DECLARE_METHOD(int, line_item_count, (log_line * ll));

/**
  Test whether a given type is presumed present on the log line.

  @param  ll  the log_line to examine
  @param  m   the log_type to test for

  @retval  0  not present
  @retval !=0 present
*/
DECLARE_METHOD(log_item_type_mask, line_item_types_seen,
               (log_line * ll, log_item_type_mask m));

/**
  Get log-line's output buffer.
  If the logger core provides this buffer, the log-service may use it
  to assemble its output therein and implicitly return it to the core.
  Participation is required for services that support populating
  performance_schema.error_log, and optional for all others.

  @param  ll  the log_line to examine

  @retval  nullptr    success, an output buffer is available
  @retval  otherwise  failure, no output buffer is available
*/
DECLARE_METHOD(log_item *, line_get_output_buffer, (log_line * ll));

/**
  Get an iterator for the items in a log_line.
  For now, only one iterator may exist per log_line.

  @param  ll  the log_line to examine

  @retval     a log_iter_iter, or nullptr on failure
*/
DECLARE_METHOD(log_item_iter *, line_item_iter_acquire, (log_line * ll));

/**
  Release an iterator for the items in a log_line.

  @param  it  the iterator to release
*/
DECLARE_METHOD(void, line_item_iter_release, (log_item_iter * it));
/**
  Use the log_line iterator to get the first item from the set.

  @param  it  the iterator to use

  @retval  pointer to the first log_item in the collection, or nullptr
*/
DECLARE_METHOD(log_item *, line_item_iter_first, (log_item_iter * it));

/**
  Use the log_line iterator to get the next item from the set.

  @param  it  the iterator to use

  @retval  pointer to the next log_item in the collection, or nullptr
*/
DECLARE_METHOD(log_item *, line_item_iter_next, (log_item_iter * it));

/**
  Use the log_line iterator to get the current item from the set.

  @param  it  the iterator to use

  @retval  pointer to the current log_item in the collection, or nullptr
*/
DECLARE_METHOD(log_item *, line_item_iter_current, (log_item_iter * it));

/**
  Complete, filter, and write submitted log items.

  This expects a log_line collection of log-related key/value pairs,
  e.g. from log_message().

  Where missing, timestamp, priority, thread-ID (if any) and so forth
  are added.

  Log item source services, log item filters, and log item sinks are
  then called; then all applicable resources are freed.

  This interface is intended to facilitate the building of submission
  interfaces other than the variadic message() one below.  See the
  example fluent C++ LogEvent() wrapper for an example of how to leverage
  it.

  @param   ll                    key/value pairs describing info to log

  @retval  int                   number of fields in created log line
*/
DECLARE_METHOD(int, line_submit, (log_line * ll));

/**
  Submit a log-message for log "log_type".
  Variadic convenience function for logging.

  This fills in the array that is used by the filter and log-writer
  services. Where missing, timestamp, priority, and thread-ID (if any)
  are added. Log item source services, log item filters, and log item
  writers are called.


  The variadic list accepts a list of "assignments" of the form
  - log_item_type, value,         for well-known types, and
  - log_item_type, key, value,    for ad-hoc types (LOG_ITEM_GEN_*)

  As its last item, the list should have
  - an element of type LOG_ITEM_LOG_MESSAGE, containing a printf-style
    format string, followed by all variables necessary to satisfy the
    substitutions in that string

    OR

  - an element of type LOG_ITEM_LOG_LOOKUP, containing a MySQL error code,
    which will be looked up in the list or regular error messages, followed
    by all variables necessary to satisfy the substitutions in that string

    OR

  - an element of type LOG_ITEM_LOG_VERBATIM, containing a string that will
    be used directly, with no % substitutions

  see log_vmessage() for more information.
*/
DECLARE_METHOD(int, message, (int log_type, ...));

/**
  Escape \0 bytes, add \0 terminator. For log-writers and other sinks
  that terminate in an API using C-strings.


  @param  li  list_item to process

  @retval  -1 out of memory
  @retval  0  success
*/
DECLARE_METHOD(int, sanitize, (log_item * li));

/**
  Return MySQL error message for a given error code.

  @param  mysql_errcode  the error code the message for which to look up

  @retval                the message (a printf-style format string)
*/
DECLARE_METHOD(const char *, errmsg_by_errcode, (int mysql_errcode));

/**
  Return MySQL error code for a given error symbol.

  @param  sym  the symbol to look up

  @retval  -1  failure
  @retval >=0  the MySQL error code
*/
DECLARE_METHOD(longlong, errcode_by_errsymbol, (const char *sym));

/**
  Convenience function: Derive a log label ("error", "warning",
  "information") from a severity.

  @param   prio       the severity/prio in question

  @return             a label corresponding to that priority.
  @retval  "System"   for prio of SYSTEM_LEVEL
  @retval  "Error"    for prio of ERROR_LEVEL
  @retval  "Warning"  for prio of WARNING_LEVEL
  @retval  "Note"     for prio of INFORMATION_LEVEL
*/
DECLARE_METHOD(const char *, label_from_prio, (int prio));

/**
  Parse a ISO8601 timestamp and return the number of microseconds
  since the epoch. Heeds +/- timezone info if present.

  @see make_iso8601_timestamp()

  @param timestamp  an ASCII string containing an ISO8601 timestamp
  @param len        Length in bytes of the aforementioned string

  @return microseconds since the epoch
*/
DECLARE_METHOD(ulonglong, parse_iso8601_timestamp,
               (const char *timestamp, size_t len));

/**
  open an error log file

  @param       name_or_ext   if beginning with '.':
                               @@global.log_error, except with this extension
                             otherwise:
                               use this as file name in the same location as
                               @@global.log_error

                             Value not contain folder separators!

  @param[out]  my_errstream  an error log handle, or nullptr on failure

  @returns LOG_SERVICE_SUCCESS                  success
  @returns LOG_SERVICE_INVALID_ARGUMENT         no my_errstream, or bad log name
  @returns LOG_SERVICE_OUT_OF_MEMORY            could not allocate file handle
  @returns LOG_SERVICE_LOCK_ERROR               couldn't lock lock
  @returns LOG_SERVICE_UNABLE_TO_WRITE          couldn't write to given location
  @returns LOG_SERVICE_COULD_NOT_MAKE_LOG_NAME  could not make log name
*/
DECLARE_METHOD(log_service_error, open_errstream,
               (const char *name_or_ext, void **my_errstream));

/**
  write to an error log file previously opened with open_errstream()

  @param       my_errstream  a handle describing the log file
  @param       buffer        pointer to the string to write
  @param       length        length of the string to write

  @returns  LOG_SERVICE_SUCCESS                 success
  @returns  otherwise                           failure
*/
DECLARE_METHOD(log_service_error, write_errstream,
               (void *my_errstream, const char *buffer, size_t length));

/**
  are we writing to a dedicated errstream, or are we sharing it?

  @param       my_errstream  a handle describing the log file

  @retval  0                 not dedicated (multiplexed, stderr, ...)
  @retval  1                 dedicated
*/
DECLARE_METHOD(int, dedicated_errstream, (void *my_errstream));

/**
  close an error log file previously opened with open_errstream()

  @param       my_stream  a handle describing the log file

  @returns    LOG_SERVICE_SUCCESS on success
*/
DECLARE_METHOD(log_service_error, close_errstream, (void **my_errstream));

/**
  re-open an error log file
  (primarily to facilitate flush/log-rotation)

  If the new file can be opened, update the my_errstream descriptor to
  use it and close the old file. Otherwise, keep using the old file.

  @param       name_or_ext   if beginning with '.':
                               @@global.log_error, except with this extension
                             otherwise:
                               use this as file name in the same location as
                               @@global.log_error

                             Value may not contain folder separators!

                             In the general case, the caller will be a
                             log-writer, the log-writer will just pass
                             its preferred file extension, and the resulting
                             file name and path will therefore be the same
                             as for the original log file.

  @param[in,out]  my_errstream  an error log handle

  @returns LOG_SERVICE_INVALID_ARGUMENT, or the result of open_errstream()
*/
DECLARE_METHOD(log_service_error, reopen_errstream,
               (const char *file, void **my_errstream));

END_SERVICE_DEFINITION(log_builtins)

/**
  String primitives for logging services.
*/
BEGIN_SERVICE_DEFINITION(log_builtins_string)
// alloc (len+1) bytes
DECLARE_METHOD(void *, malloc, (size_t len));
// alloc (len+1) bytes, then copy len bytes from fm, and \0 terminate
// like my_strndup(), and unlike strndup(), \0 in input won't end copying
DECLARE_METHOD(char *, strndup, (const char *fm, size_t len));
// free allocated memory
DECLARE_METHOD(void, free, (void *ptr));

// length of nul terminated byte string
DECLARE_METHOD(size_t, length, (const char *s));
// find char in string, from the left
DECLARE_METHOD(char *, find_first, (const char *s, int c));
// find char in string, from the right
DECLARE_METHOD(char *, find_last, (const char *s, int c));

// compare two NUL-terminated byte-strings
DECLARE_METHOD(int, compare,
               (const char *a, const char *b, size_t len,
                bool case_insensitive));

/**
  Wrapper for std::snprintf()
  Replace all % in format string with variables from list.
  Do not use in new code; use std::snprintf() instead.

  @param  to    buffer to write the result to
  @param  n     size of that buffer
  @param  fmt   format string
  @param  ap    va_list with valuables for all substitutions in format string

  @retval       return value of snprintf
*/
DECLARE_METHOD(size_t, substitutev,
               (char *to, size_t n, const char *fmt, va_list ap))
MY_ATTRIBUTE((format(printf, 3, 0)));

// replace all % in format string with variables from list (std::snprintf())
DECLARE_METHOD(size_t, substitute, (char *to, size_t n, const char *fmt, ...))
MY_ATTRIBUTE((format(printf, 3, 4)));

END_SERVICE_DEFINITION(log_builtins_string)

/**
  Temporary primitives for logging services.
*/
BEGIN_SERVICE_DEFINITION(log_builtins_tmp)
// Are we shutting down yet?  Windows EventLog needs to know.
DECLARE_METHOD(size_t, notify_client,
               (void *thd, uint severity, uint code, char *to, size_t n,
                const char *format, ...))
MY_ATTRIBUTE((format(printf, 6, 7)));
END_SERVICE_DEFINITION(log_builtins_tmp)

/**
  Syslog/Eventlog functions for logging services.
*/
BEGIN_SERVICE_DEFINITION(log_builtins_syseventlog)
DECLARE_METHOD(log_service_error, open,
               (const char *name, int option, int facility));
DECLARE_METHOD(log_service_error, write,
               (enum loglevel level, const char *msg));
DECLARE_METHOD(log_service_error, close, (void));
END_SERVICE_DEFINITION(log_builtins_syseventlog)

#ifdef __cplusplus

#if !defined(LOG_H)

extern SERVICE_TYPE(log_builtins) * log_bi;
extern SERVICE_TYPE(log_builtins_string) * log_bs;

#define log_line_init log_bi->line_init
#define log_line_exit log_bi->line_exit
#define log_line_item_set_with_key log_bi->line_item_set_with_key
#define log_line_item_set log_bi->line_item_set
#define log_line_item_types_seen log_bi->line_item_types_seen
#define log_line_submit log_bi->line_submit
#define log_set_int log_bi->item_set_int
#define log_set_float log_bi->item_set_float
#define log_set_lexstring log_bi->item_set_lexstring
#define log_set_cstring log_bi->item_set_cstring
#define log_line_set_flag log_bi->line_set_flag
#define log_malloc log_bs->malloc
#define log_free log_bs->free
#define log_msg log_bs->substitutev
#define error_msg_by_errcode log_bi->errmsg_by_errcode
#define error_code_by_errsymbol log_bi->errcode_by_errsymbol
#else

#include "sql/derror.h"

#define log_malloc(s) my_malloc(0, (s), MYF(0))
#define log_free my_free
#define log_msg vsnprintf
#define error_msg_by_errcode error_message_for_error_log
#define error_code_by_errsymbol mysql_symbol_to_errno
#define log_set_int log_item_set_int
#define log_set_float log_item_set_float
#define log_set_lexstring log_item_set_lexstring
#define log_set_cstring log_item_set_cstring

#endif  // LOG_H

#ifndef DISABLE_ERROR_LOGGING

#if defined(LOG_COMPONENT_TAG)

#define LogErr(severity, ecode, ...) \
  LogEvent()                         \
      .prio(severity)                \
      .errcode(ecode)                \
      .subsys(LOG_SUBSYSTEM_TAG)     \
      .component(LOG_COMPONENT_TAG)  \
      .source_line(__LINE__)         \
      .source_file(MY_BASENAME)      \
      .function(__FUNCTION__)        \
      .lookup(ecode, ##__VA_ARGS__)

#ifdef HAVE_LOG_DIAGNOSTIC
#define LogDiag(severity, ecode, ...) \
  LogEvent()                          \
      .prio(severity)                 \
      .errcode(ecode)                 \
      .subsys(LOG_SUBSYSTEM_TAG)      \
      .component(LOG_COMPONENT_TAG)   \
      .source_line(__LINE__)          \
      .source_file(MY_BASENAME)       \
      .function(__FUNCTION__)         \
      .type(LOG_TYPE_DIAG)            \
      .lookup(ecode, ##__VA_ARGS__)
#else
#define LogDiag(...)
#endif /* HAVE_LOG_DIAGNOSTIC */

#define LogComponentErr(severity, ecode, ...)                           \
  LogEvent()                                                            \
      .prio(severity)                                                   \
      .errcode(ecode)                                                   \
      .subsys(LOG_SUBSYSTEM_TAG)                                        \
      .component("component:" LOG_COMPONENT_TAG)                        \
      .source_line(__LINE__)                                            \
      .source_file(MY_BASENAME)                                         \
      .function(__FUNCTION__)                                           \
      .lookup_quoted(ecode, "Component " LOG_COMPONENT_TAG " reported", \
                     ##__VA_ARGS__)

#define LogPluginErr(severity, ecode, ...)                           \
  LogEvent()                                                         \
      .prio(severity)                                                \
      .errcode(ecode)                                                \
      .subsys(LOG_SUBSYSTEM_TAG)                                     \
      .component("plugin:" LOG_COMPONENT_TAG)                        \
      .source_line(__LINE__)                                         \
      .source_file(MY_BASENAME)                                      \
      .function(__FUNCTION__)                                        \
      .lookup_quoted(ecode, "Plugin " LOG_COMPONENT_TAG " reported", \
                     ##__VA_ARGS__)

#define LogPluginErrV(severity, ecode, vl)    \
  LogEvent()                                  \
      .prio(severity)                         \
      .errcode(ecode)                         \
      .subsys(LOG_SUBSYSTEM_TAG)              \
      .component("plugin:" LOG_COMPONENT_TAG) \
      .source_line(__LINE__)                  \
      .source_file(MY_BASENAME)               \
      .function(__FUNCTION__)                 \
      .lookup_quotedv(ecode, "Plugin " LOG_COMPONENT_TAG " reported", vl)

#define LogPluginErrMsg(severity, ecode, ...) \
  LogEvent()                                  \
      .prio(severity)                         \
      .errcode(ecode)                         \
      .subsys(LOG_SUBSYSTEM_TAG)              \
      .component("plugin:" LOG_COMPONENT_TAG) \
      .source_line(__LINE__)                  \
      .source_file(MY_BASENAME)               \
      .function(__FUNCTION__)                 \
      .message_quoted("Plugin " LOG_COMPONENT_TAG " reported", ##__VA_ARGS__)

#else

#define LogErr(severity, ecode, ...) \
  LogEvent()                         \
      .prio(severity)                \
      .errcode(ecode)                \
      .subsys(LOG_SUBSYSTEM_TAG)     \
      .source_line(__LINE__)         \
      .source_file(MY_BASENAME)      \
      .function(__FUNCTION__)        \
      .lookup(ecode, ##__VA_ARGS__)

#ifdef HAVE_LOG_DIAGNOSTIC
#define LogDiag(severity, ecode, ...) \
  LogEvent()                          \
      .prio(severity)                 \
      .errcode(ecode)                 \
      .subsys(LOG_SUBSYSTEM_TAG)      \
      .source_line(__LINE__)          \
      .source_file(MY_BASENAME)       \
      .function(__FUNCTION__)         \
      .type(LOG_TYPE_DIAG)            \
      .lookup(ecode, ##__VA_ARGS__)
#else
#define LogDiag(...)
#endif /* HAVE_LOG_DIAGNOSTIC */
#endif

#else

inline void dummy_log_message(longlong severity [[maybe_unused]],
                              longlong ecode [[maybe_unused]], ...) {
  return;
}

#define LogErr(severity, ecode, ...) \
  dummy_log_message(severity, ecode, ##__VA_ARGS__)

#define LogPluginErr(severity, ecode, ...) \
  dummy_log_message(severity, ecode, ##__VA_ARGS__)
#define LogPluginErrV(severity, ecode, ...) \
  dummy_log_message(severity, ecode, ##__VA_ARGS__)
#define LogPluginErrMsg(severity, ecode, ...) \
  dummy_log_message(severity, ecode, ##__VA_ARGS__)

#endif  // DISABLE_ERROR_LOGGING

/**
  Modular logger: fluid API. Server-internal. Lets you use safe and
  expressive syntax, like so:

  LogEvent(LOG_TYPE_ERROR).prio(INFORMATION_LEVEL).message("Meow! %d", 4711);
*/

class LogEvent {
 private:
  log_line *ll;  // Temporary allocation to hold a log-event.
  char *msg;     // Temporary allocation to hold a message.
  const char *msg_tag;
  bool have_msg;  // Was a message set in `msg` using set_message()?

  /**
    Set MySQL error-code if none has been set yet.

    @param  errcode  the error code (not operating system errno!)

    @retval true     an error occurred, value not set (OOM?)
    @retval false    value was set without incident, or did not need to be set
  */
  bool set_errcode(longlong errcode) {
    if (ll == nullptr) return true;

    if (!log_line_item_types_seen(ll, LOG_ITEM_SQL_ERRCODE) &&
        !log_line_item_types_seen(ll, LOG_ITEM_SQL_ERRSYMBOL)) {
      return log_set_int(log_line_item_set(ll, LOG_ITEM_SQL_ERRCODE), errcode);
    }
    return false;  // already set, that's OK then
  }

  /**
    Set the error message.

    @param  fmt  format string. % substitution will be performed.
    @param  ap   va_list of the arguments for % substitution.
  */
  void set_message(const char *fmt, va_list ap)
      MY_ATTRIBUTE((format(printf, 2, 0)));

  /**
    Set the error message (by MySQL error code).
    The actual message will be looked up using this errcode.
    As the message is a printf-style format string, % substitution
    will be performed.

    @param  errcode  MySQL error code to fetch the message string for
    @param  ap       va_list of the arguments for % substitution.
  */
  void set_message_by_errcode(longlong errcode, va_list ap);

 public:
  /**
    Destructor automatically sends the event on.
    It is auto-free()d after processing.
  */
  ~LogEvent() {
    if (ll != nullptr) {
      log_line_submit(this->ll);
      log_line_exit(ll);
      /*
        If a message was set on the LogEvent using set_message,
        the message became part of a log_item on the log_line.
        have_msg is true. The log_line's log_items were released
        in log_line_submit(). Null `msg` here to prevent double-free.

        On the other hand, if set_message() was not used, the
        convenience buffer was never associated with the log_line,
        and therefore wasn't freed when the log_line was submitted.
        In that case, we'll leave the pointer intact for clean-up
        further down.
      */
      if (have_msg) msg = nullptr;
    }

    /*
      If set_message() attached the buffer `msg` to the log_line,
      the allocation has either been freed in log_line_submit()
      above, or it is now owned by someone who stole it using steal().
      In either case, msg==nullptr and we do nothing here.
    */
    if (msg != nullptr) log_free(msg);
  }

  /**
    "Full customization" constructor.  Use one of the LogErr() macro
    where possible; it's there to help you remember the minimum set
    of particles and their data-types.  Be prepared for stern looks
    from your reviewers if you use this constructor except for external
    (loadable) services that have no error messages registered with the
    server, and therefore need to submit them free-form.
  */
  LogEvent() {
    have_msg = false;
    if ((ll = log_line_init()) != nullptr) {
      if ((msg = (char *)log_malloc(LOG_BUFF_MAX)) == nullptr) {
        log_line_exit(ll);
        ll = nullptr;
      }
    } else
      msg = nullptr;
    msg_tag = nullptr;
  }

  /**
    Save the log-event allocated by LogEvent().

    LogEvent() internally represents the log-event in a log_line
    structure it allocates. steal() saves this pointer to the
    method's argument. The pointer in the LogEvent() is then
    cleared to prevent the destructor from freeing the log_line.
    Freeing allocated memory is now the caller's responsibility:

      log_line *ll;
      LogEvent().prio(SYSTEM_LEVEL).message("Hi %s!", user).steal(&ll);

      // Do some things with the event ...

      log_line_item_free_all(ll);
      log_line_exit(ll);

    If a message was set, the message buffer pointed to by `msg`
    becomes part of the log_line and is released on
    log_line_item_free_all() (or equivalent).

    If no message was set, the message buffer is released when
    the LogEvent's destructor is called.

    Note that verbatim() does NOT copy its argument to the
    LogEvent's internal allocation `msg`.
    Hence the life-cycle management of verbatim()'s argument
    would be up to the developer. When using steal(), using it
    with one of the set_message() wrappers is generally preferable:

      LogEvent().message("%s", my_verbatim_string).steal(&ll);

    @param  save_to  where to save the pointer to the log-event
  */
  void steal(log_line **save_to) {
    *save_to = ll;
    ll = nullptr;
    /*
      If the message was set, the allocation is now part of a log_item
      on the log_line. Thus, it is now owned by the called and will
      be released when they release the log_items on the log_line.
      In that case, we null our pointer to it so we won't double-free
      the allocation.

      Conversely, if the buffer exists, but hasn't become part of
      the log_line through use of a set_message() wrapper, we'll hold
      on to the pointer so the empty buffer is released when we dest
      the LogEvent().
    */
    if (have_msg) msg = nullptr;
  }

  /**
    Set log type.

    @param  val  the log type (LOG_TYPE_ERROR)

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &type(enum_log_type val) {
    log_set_int(log_line_item_set(this->ll, LOG_ITEM_LOG_TYPE), val);
    return *this;
  }

  /**
    Append a numeric error code

    @param  val  the MySQL error code (not operating system errno!).

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &errcode(longlong val) {
    log_set_int(log_line_item_set(this->ll, LOG_ITEM_SQL_ERRCODE), val);
    return *this;
  }

  /**
    Append a (string) error symbol

    @param  val  error symbol. NTBS.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &errsymbol(const char *val) {
    log_set_cstring(log_line_item_set(this->ll, LOG_ITEM_SQL_ERRSYMBOL), val);
    return *this;
  }

  /**
    Append a (string) SQL state

    @param  val  the SQLstate. NTBS.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &sqlstate(const char *val) {
    log_set_cstring(log_line_item_set(this->ll, LOG_ITEM_SQL_STATE), val);
    return *this;
  }

  /**
    Append a numeric (operating system, as opposed to MySQL) error number.

    @param  val  the operating system errno.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &os_errno(longlong val) {
    log_set_int(log_line_item_set(this->ll, LOG_ITEM_SYS_ERRNO), val);
    return *this;
  }

  /**
    Append a textual (operating system, as opposed to MySQL) error message,
    vulgo, strerror()

    @param  val  the error message returned by the operating system. NTBS.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &os_errmsg(const char *val) {
    log_set_cstring(log_line_item_set(this->ll, LOG_ITEM_SYS_STRERROR), val);
    return *this;
  }

  /**
    Which source file was the problem detected in?

    @param  val  the source file's name. NTBS.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &source_file(const char *val) {
    log_set_cstring(log_line_item_set(this->ll, LOG_ITEM_SRC_FILE), val);
    return *this;
  }

  /**
    Which line in the source file was the problem detected on?

    @param  val  the line number.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &source_line(longlong val) {
    log_set_int(log_line_item_set(this->ll, LOG_ITEM_SRC_LINE), val);
    return *this;
  }

  /**
    Which function in the source was the problem detected in?

    @param  val  the function's name. NTBS.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &function(const char *val) {
    log_set_cstring(log_line_item_set(this->ll, LOG_ITEM_SRC_FUNC), val);
    return *this;
  }

  /**
    Which subsystem in the source was the problem detected in?
    ("Repl"/"InnoDB"/"Server")

    @param  val  the subsystem. NTBS.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &subsys(const char *val) {
    if (val != nullptr)
      log_set_cstring(log_line_item_set(this->ll, LOG_ITEM_SRV_SUBSYS), val);
    return *this;
  }

  /**
    Which component in the source was the problem detected in?
    This should be the same string that is given to the
    component/service framework.

    @param  val  the component. NTBS.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &component(const char *val) {
    if (val != nullptr)
      log_set_cstring(log_line_item_set(this->ll, LOG_ITEM_SRV_COMPONENT), val);
    return *this;
  }

  /**
    What user were we working for at the time of the issue?

    @param  val   the user part (of "user@host").  LEX_CSTRING.

    @retval       the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &user(LEX_CSTRING val) {
    log_set_lexstring(log_line_item_set(this->ll, LOG_ITEM_MSC_USER), val.str,
                      val.length);
    return *this;
  }

  /**
    What user were we working for at the time of the issue?

    @param  val  the user part (of "user@host").  NTBS.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &user(const char *val) {
    log_set_cstring(log_line_item_set(this->ll, LOG_ITEM_MSC_USER), val);
    return *this;
  }

  /**
    Whose session did the issue appear in?

    @param  val  the host part (of "user@host").  LEX_CSTRING.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &host(LEX_CSTRING val) {
    log_set_lexstring(log_line_item_set(this->ll, LOG_ITEM_MSC_HOST), val.str,
                      val.length);
    return *this;
  }

  /**
    Whose session did the issue appear in?

    @param  val  the host part (of "user@host").  NTBS.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &host(const char *val) {
    log_set_cstring(log_line_item_set(this->ll, LOG_ITEM_MSC_HOST), val);
    return *this;
  }

  /**
    What thread / "connection ID" was the issue detected in?

    @param  val  the thread_ID of the session the issue appeared in

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &thread_id(longlong val) {
    log_set_int(log_line_item_set(this->ll, LOG_ITEM_SRV_THREAD), val);
    return *this;
  }

  /**
    What query apparently caused the issue?

    @param  val  the query_ID of the offending query

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &query_id(longlong val) {
    log_set_int(log_line_item_set(this->ll, LOG_ITEM_SQL_QUERY_ID), val);
    return *this;
  }

  /**
    What table were we working on?

    @param  val  the table's name/alias. NTBS.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &table_name(const char *val) {
    log_set_cstring(log_line_item_set(this->ll, LOG_ITEM_SQL_TABLE_NAME), val);
    return *this;
  }

  /**
    Set error message priority.
    Assign one of ERROR_LEVEL, WARNING_LEVEL, INFORMATION_LEVEL.
    log-writers and other sinks should use this value (rather
    than that of LOG_ITEM_LOG_EPRIO):

    - file writers should use the value to determine
      what label to write (perhaps by submitting it to label_from_prio())

    - sinks that submit the event data to a sub-system outside of
      the MySQL server (such as syslog, EventLog, systemd journal, etc.)
      should translate this value into a priority/log level understood
      by that target subsystem.

    @param  val   The priority for this LogEvent.

    @retval       the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &prio(longlong val) {
    log_set_int(log_line_item_set(this->ll, LOG_ITEM_LOG_PRIO), val);
    return *this;
  }

  /**
    Set a label (usually "warning"/"error"/"information").
    Will be derived from prio if not set explicitly.
    Some log services may ignore custom labels.

    @param  val  the (custom) label to set

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &label(const char *val) {
    log_set_cstring(log_line_item_set(this->ll, LOG_ITEM_LOG_LABEL), val);
    return *this;
  }

  /**
    Add a message to the event, verbatim (i.e. with no % substitutions).
    This is an analog of message("%s", message); it can be used when
    message may contain user input or a message from another subsystem
    that could contain % that must not be interpreted as an invitation
    to do % substitutions.

    If you use this in a context other than an external service that
    has no messages registered with the server, your reviewers will
    say unkind things about you.  Use registered messages and their
    error codes wherever possible!

    Combining verbatim() with steal() is discouraged as it burdens
    the developer with the life-cycle management of verbatim's
    argument. This is a result of verbatim() using its argument
    verbatim, rather than setting it up in the LogEvent's internal
    allocation `msg` using set_message(). Hence, the LogEvent has
    no copy of the message, which is an optimization for the
    common, non-steal() case.

    @param  msg_arg the message. % substitution will not happen.

    @retval         the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &verbatim(const char *msg_arg) {
    log_set_cstring(log_line_item_set(this->ll, LOG_ITEM_LOG_MESSAGE), msg_arg);
    return *this;
  }

  /**
    Fill in a format string by substituting the % with the given
    arguments, then add the result as the event's message.
    This should be used very sparingly; use registered messages
    and their error codes wherever possible!

    @param  fmt  message (treated as a printf-style format-string,
                 so % substitution will happen)
    @param  ap   valist to satisfy any % in the message

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &messagev(const char *fmt, va_list ap)
      MY_ATTRIBUTE((format(printf, 2, 0))) {
    set_message(fmt, ap);
    return *this;
  }

  /**
    Fill in a format string by substituting the % with the given
    arguments, then add the result as the event's message.

    If you use this in a context other than an external service that
    has no messages registered with the server, your reviewers will
    say unkind things about you.  Use registered messages and their
    error codes wherever possible!

    @param  fmt  message (treated as a printf-style format-string,
                 so % substitution will happen)
    @param  ...  varargs to satisfy any % in the message

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &message(const char *fmt, ...) MY_ATTRIBUTE((format(printf, 2, 3)));

  /**
    Fill in a format string by substituting the % with the given
    arguments and tag, then add the result as the event's message.

    @param  tag  Tag to prefix to message.
    @param  fmt  message (treated as a printf-style format-string,
                 so % substitution will happen)
    @param  ...  varargs to satisfy any % in the message

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &message_quoted(const char *tag, const char *fmt, ...)
      MY_ATTRIBUTE((format(printf, 3, 4))) {
    msg_tag = tag;

    va_list args;
    va_start(args, fmt);
    set_message(fmt, args);
    va_end(args);

    return *this;
  }

  /**
    Find an error message by its MySQL error code.
    Substitute the % in that message with the given
    arguments, then add the result as the event's message.

    @param  errcode  MySQL error code for the message in question,
                     e.g. ER_STARTUP
    @param  ...      varargs to satisfy any % in the message

    @retval          the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &lookup(longlong errcode, ...) {
    va_list args;
    va_start(args, errcode);
    set_message_by_errcode(errcode, args);
    va_end(args);

    return *this;
  }

  /**
    Find an error message by its MySQL error code. Substitute the % in that
    message with the given arguments list, then add the result as the event's
    message.

    @param  errcode  MySQL error code for the message in question,
                     e.g. ER_STARTUP
    @param  args     varargs to satisfy any % in the message

    @retval          the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &lookupv(longlong errcode, va_list args) {
    set_message_by_errcode(errcode, args);

    return *this;
  }

  LogEvent &lookup_quoted(longlong errcode, const char *tag, ...) {
    msg_tag = tag;

    va_list args;
    va_start(args, tag);
    set_message_by_errcode(errcode, args);
    va_end(args);

    return *this;
  }

  LogEvent &lookup_quotedv(longlong errcode, const char *tag, va_list vl) {
    msg_tag = tag;
    set_message_by_errcode(errcode, vl);

    return *this;
  }

  /**
    Add a ad hoc integer value with the given key.

    @param  key  user-defined key (i.e. not wellknown). NTBS.
    @param  val  value.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &int_value(const char *key, longlong val) {
    log_set_int(log_line_item_set_with_key(this->ll, LOG_ITEM_GEN_INTEGER, key,
                                           LOG_ITEM_FREE_NONE),
                val);
    return *this;
  }

  /**
    Add a ad hoc (not "well-known") float value with the given key.

    @param  key  user-defined key (i.e. not wellknown). NTBS.
    @param  val  value.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &float_value(const char *key, double val) {
    log_set_float(log_line_item_set_with_key(this->ll, LOG_ITEM_GEN_FLOAT, key,
                                             LOG_ITEM_FREE_NONE),
                  val);
    return *this;
  }

  /**
    Add a ad hoc string value with the given key.

    @param  key  user-defined key (i.e. not wellknown). NTBS.
    @param  val  value.
    @param  len  length in bytes of the value.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &string_value(const char *key, const char *val, size_t len) {
    log_set_lexstring(
        log_line_item_set_with_key(this->ll, LOG_ITEM_GEN_LEX_STRING, key,
                                   LOG_ITEM_FREE_NONE),
        val, len);
    return *this;
  }

  /**
    Add a ad hoc string value with the given key.

    @param  key  user-defined key (i.e. not wellknown). NTBS.
    @param  val  value. NTBS.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &string_value(const char *key, const char *val) {
    log_set_cstring(
        log_line_item_set_with_key(this->ll, LOG_ITEM_GEN_LEX_STRING, key,
                                   LOG_ITEM_FREE_NONE),
        val);
    return *this;
  }

  /**
    Mark log line to skip being additionally emitted as a telemetry log record.

    @retval      the LogEvent, for easy fluent-style chaining.
  */
  LogEvent &no_telemetry() {
    log_line_set_flag(this->ll, LOG_LINE_EMIT_TELEMETRY, 0);
    return *this;
  }
};

inline void LogEvent::set_message_by_errcode(longlong errcode, va_list ap) {
  const char *fmt = error_msg_by_errcode((int)errcode);

  if ((fmt == nullptr) || (*fmt == '\0')) fmt = "invalid error code";

  set_errcode(errcode);
  set_message(fmt, ap);
}

inline void LogEvent::set_message(const char *fmt, va_list ap) {
  if ((ll != nullptr) && (msg != nullptr)) {
    char buf[LOG_BUFF_MAX];
    if (msg_tag != nullptr) {
      snprintf(buf, LOG_BUFF_MAX, "%s: \'%s\'", msg_tag, fmt);
      fmt = buf;
    }
    size_t len = log_msg(msg, LOG_BUFF_MAX, fmt, ap);
    if (len >= LOG_BUFF_MAX) {
      const char ellipsis[] = " <...>";
      len = LOG_BUFF_MAX - 1;
      strcpy(&msg[LOG_BUFF_MAX - sizeof(ellipsis)], ellipsis);
    }
    log_item_data *lid;
    lid = log_line_item_set_with_key(this->ll, LOG_ITEM_LOG_MESSAGE, nullptr,
                                     LOG_ITEM_FREE_VALUE);
    log_set_lexstring(lid, msg, len);
    have_msg = true;
  }
}

inline LogEvent &LogEvent::message(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  set_message(fmt, args);
  va_end(args);

  return *this;
}

// Methods initialize and de-initialize logging service for plugins.
#if defined(MYSQL_DYNAMIC_PLUGIN)

/**
  Method to de-initialize logging service in plugin.

  param[in,out]  reg_srv    Plugin registry service.
  param[in,out]  log_bi     Error logging service.
  param[in,out]  log_bs     String service for error logging.
*/
inline void deinit_logging_service_for_plugin(
    SERVICE_TYPE(registry) * *reg_srv, SERVICE_TYPE(log_builtins) * *log_bi,
    SERVICE_TYPE(log_builtins_string) * *log_bs) {
  using log_builtins_t = SERVICE_TYPE_NO_CONST(log_builtins);
  using log_builtins_string_t = SERVICE_TYPE_NO_CONST(log_builtins_string);
  if (*log_bi)
    (*reg_srv)->release(
        reinterpret_cast<my_h_service>(const_cast<log_builtins_t *>(*log_bi)));
  if (*log_bs)
    (*reg_srv)->release(reinterpret_cast<my_h_service>(
        const_cast<log_builtins_string_t *>(*log_bs)));
  mysql_plugin_registry_release(*reg_srv);
  *log_bi = nullptr;
  *log_bs = nullptr;
  *reg_srv = nullptr;
}

/**
  Method to de-initialize logging service in plugin.

  param[in,out]  reg_srv    Plugin registry service.
  param[in,out]  log_bi     Error logging service.
  param[in,out]  log_bs     String service for error logging.

  @retval     false  Success.
  @retval     true   Failed.
*/
inline bool init_logging_service_for_plugin(
    SERVICE_TYPE(registry) * *reg_srv, SERVICE_TYPE(log_builtins) * *log_bi,
    SERVICE_TYPE(log_builtins_string) * *log_bs) {
  my_h_service log_srv = nullptr;
  my_h_service log_str_srv = nullptr;
  *reg_srv = mysql_plugin_registry_acquire();
  if (!(*reg_srv)->acquire("log_builtins.mysql_server", &log_srv) &&
      !(*reg_srv)->acquire("log_builtins_string.mysql_server", &log_str_srv)) {
    (*log_bi) = reinterpret_cast<SERVICE_TYPE(log_builtins) *>(log_srv);
    (*log_bs) =
        reinterpret_cast<SERVICE_TYPE(log_builtins_string) *>(log_str_srv);
  } else {
    deinit_logging_service_for_plugin(reg_srv, log_bi, log_bs);
    return true;
  }
  return false;
}

#elif defined(EXTRA_CODE_FOR_UNIT_TESTING)

/**
  Method is used by unit tests.

  param[in,out]  reg_srv    Plugin registry service.
  param[in,out]  log_bi     Error logging service.
  param[in,out]  log_bs     String service for error logging.

  @retval     false  Success.
  @retval     true   Failed.
*/
inline bool init_logging_service_for_plugin(
    SERVICE_TYPE(registry) * *reg_srv [[maybe_unused]],
    SERVICE_TYPE(log_builtins) * *log_bi [[maybe_unused]],
    SERVICE_TYPE(log_builtins_string) * *log_bs [[maybe_unused]])

{
  return false;
}

/**
  Method is used by unit tests.

  param[in,out]  reg_srv    Plugin registry service.
  param[in,out]  log_bi     Error logging service.
  param[in,out]  log_bs     String service for error logging.
*/
inline void deinit_logging_service_for_plugin(
    SERVICE_TYPE(registry) * *reg_srv [[maybe_unused]],
    SERVICE_TYPE(log_builtins) * *log_bi [[maybe_unused]],
    SERVICE_TYPE(log_builtins_string) * *log_bs [[maybe_unused]]) {}

#endif  // MYSQL_DYNAMIC_PLUGIN

#endif  // __cplusplus

#endif
