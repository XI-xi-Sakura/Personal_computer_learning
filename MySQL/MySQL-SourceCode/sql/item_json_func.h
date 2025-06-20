#ifndef ITEM_JSON_FUNC_INCLUDED
#define ITEM_JSON_FUNC_INCLUDED

/* Copyright (c) 2015, 2025, Oracle and/or its affiliates.

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

#include <assert.h>
#include <sys/types.h>

#include <cstdint>
#include <memory>
#include <utility>  // std::forward

#include "field_types.h"
#include "my_alloc.h"
#include "my_inttypes.h"
#include "my_table_map.h"
#include "my_time.h"
#include "mysql/strings/m_ctype.h"
#include "mysql/udf_registration_types.h"
#include "mysql_com.h"
#include "mysql_time.h"
#include "prealloced_array.h"  // Prealloced_array
#include "sql-common/json_error_handler.h"
#include "sql-common/json_path.h"    // Json_path
#include "sql-common/json_schema.h"  //Json_schema_validator_holder
#include "sql/enum_query_type.h"
#include "sql/field.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_strfunc.h"    // Item_str_func
#include "sql/mem_root_array.h"  // Mem_root_array
#include "sql/parse_location.h"  // POS
#include "sql/psi_memory_key.h"  // key_memory_JSON
#include "sql_string.h"

class Json_array;
class Json_diff_vector;
class Json_dom;
class Json_object;
class Json_scalar_holder;
class Json_schema_validator;
class Json_wrapper;
class PT_item_list;
class THD;
class my_decimal;
enum Cast_target : unsigned char;
enum class Json_on_response_type : uint16;
enum class enum_json_diff_status;

struct Cast_type;
struct TABLE;

/** For use by JSON_CONTAINS_PATH() and JSON_SEARCH() */
enum enum_one_or_all_type {
  ooa_one,
  ooa_all,
  ooa_null,
  ooa_error,
  ooa_uninitialized
};

/**
  Path cache for JSON functions. Caches parsed path
  objects for arguments which are string literals.
  Maintains a vector of path objects and an array of
  ints which map path argument numbers to slots in
  the array.
*/
class Json_path_cache {
 private:
  /// Holder for path strings.
  String m_path_value;

  /// List of paths.
  Prealloced_array<Json_path, 8> m_paths;

  /// Enum that tells the status of a cell in m_paths.
  enum class enum_path_status : uint8 {
    UNINITIALIZED,
    OK_NOT_NULL,
    OK_NULL,
  };

  /// Struct that points to a cell in m_paths and tells its status.
  struct Path_cell {
    enum_path_status m_status = enum_path_status::UNINITIALIZED;
    size_t m_index = 0;
  };

  /// Map argument indexes to indexes into m_paths.
  Mem_root_array<Path_cell> m_arg_idx_to_vector_idx;

 public:
  Json_path_cache(THD *thd, uint size);
  ~Json_path_cache();

  /**
    Parse a path expression if necessary. Does nothing if the path
    expression is constant and it has already been parsed. Assumes that
    we've already verified that the path expression is not null. Raises an
    error if the path expression is syntactically incorrect. Raises an
    error if the path expression contains wildcard tokens but is not
    supposed to. Otherwise puts the parsed path onto the
    path vector.

    @param[in]  thd              THD handle
    @param[in]  args             Array of args to a JSON function
    @param[in]  arg_idx          Index of the path_expression in args
    @param[in]  forbid_wildcards True if the path shouldn't contain * or **

    @returns false on success (valid path or NULL), true on error
  */
  bool parse_and_cache_path(const THD *thd, Item **args, uint arg_idx,
                            bool forbid_wildcards);

  /**
    Return an already parsed path expression.

    @param[in]  arg_idx   Index of the path_expression in the JSON function args

    @returns the already parsed path, possibly NULL
  */
  const Json_path *get_path(uint arg_idx) const;

  /**
    Reset the cache for re-use when a statement is re-executed.
  */
  void reset_cache();
};

/* JSON function support  */

/**
  Base class for all item functions that a return JSON value
*/
class Item_json_func : public Item_func {
  /// Can this function type be used in partial update?
  virtual bool can_use_in_partial_update() const { return false; }

 protected:
  /// String used when reading JSON binary values or JSON text values.
  String m_value;
  /// String used for converting JSON text values to utf8mb4 charset.
  String m_conversion_buffer;
  /// String used for converting a JSON value to text in val_str().
  String m_string_buffer;

  // Cache for constant path expressions
  Json_path_cache m_path_cache;

  /**
    Target column for partial update, if this function is used in an
    update statement and partial update can be used.
  */
  const Field_json *m_partial_update_column = nullptr;

 public:
  /**
    Construct an Item_json_func instance.
    @param thd   THD handle
    @param parent_args  arguments to forward to Item_func's constructor
  */
  template <typename... Args>
  Item_json_func(THD *thd, Args &&...parent_args)
      : Item_func(std::forward<Args>(parent_args)...),
        m_path_cache(thd, arg_count) {
    set_data_type_json();
  }

  bool resolve_type(THD *) override {
    if (reject_vector_args()) return true;
    set_nullable(true);
    return false;
  }
  enum Item_result result_type() const override { return STRING_RESULT; }
  String *val_str(String *arg) override;
  bool get_date(MYSQL_TIME *ltime, my_time_flags_t fuzzydate) override;
  bool get_time(MYSQL_TIME *ltime) override;
  longlong val_int() override;
  double val_real() override;
  my_decimal *val_decimal(my_decimal *decimal_value) override;

  void cleanup() override;

  Item_result cast_to_int_type() const override { return INT_RESULT; }

  /**
    Does this function call support partial update of the given JSON column?

    JSON_SET, JSON_REPLACE and JSON_REMOVE support partial update of a JSON
    column if the JSON column is the first argument of the function call, or if
    the first argument is a sequence of nested JSON_SET, JSON_REPLACE and
    JSON_REMOVE calls in which the JSON column is the first argument of the
    inner function call.

    For example, this expression can be used to partially update column
    `json_col`:

        JSON_SET(JSON_REPLACE(json_col, path1, val1), path2, val2)
  */
  bool supports_partial_update(const Field_json *field) const override;

  /**
    Mark this expression as used in partial update. Should only be
    called if #supports_partial_update returns true.
  */
  void mark_for_partial_update(const Field_json *field);
};

bool sql_scalar_to_json(Item *arg, const char *calling_function, String *value,
                        String *tmp, Json_wrapper *wr,
                        Json_scalar_holder *scalar, bool scalar_string);

/**
  Return the JSON value of the argument in a wrapper.

  Handles arguments with type JSON, including array objects (which do
  not report type JSON but rather the type of individual elements).

  Does not handle literals.
  See also get_json_wrapper.

  @param[in]     arg      the argument
  @param[in,out] result   the JSON value wrapper
  @param[out]    has_value true if argument was handled, false otherwise
                           undefined when error
*/
bool json_value(Item *arg, Json_wrapper *result, bool *has_value);

/**
  Return the JSON value of the argument in a wrapper. Abstracts whether
  the value comes from a field or a function or a valid JSON text.

  @param[in]  args          the arguments
  @param[in]  arg_idx       the argument index
  @param[out] str           the string buffer
  @param[in]  func_name     the name of the function we are executing
  @param[out] wrapper       the JSON value wrapper
  @returns false if we found a value or NULL, true if not.
*/
bool get_json_wrapper(Item **args, uint arg_idx, String *str,
                      const char *func_name, Json_wrapper *wrapper);

/**
  Convert Json values or MySQL values to JSON.

  @param[in]     args       arguments to function
  @param[in]     arg_idx    the index of the argument to process
  @param[in]     calling_function    name of the calling function
  @param[in,out] value      working area (if the returned Json_wrapper points
                            to a binary value rather than a DOM, this string
                            will end up holding the binary representation, and
                            it must stay alive until the wrapper is destroyed
                            or converted from binary to DOM)
  @param[in,out] tmp        temporary scratch space for converting strings to
                            the correct charset; only used if accept_string is
                            true and conversion is needed
  @param[in,out] wr         the result wrapper
  @param[in,out] scalar     pointer to pre-allocated memory that can be
                            borrowed by the result wrapper if the result is a
                            scalar. If the pointer is NULL, memory for a
                            scalar result will be allocated on the heap.
  @param[in]                accept_string
                            if true, accept MySQL strings as JSON strings
                            by converting them to UTF8, else emit an error
  @returns false if we found a value or NULL, true otherwise
*/
bool get_json_atom_wrapper(Item **args, uint arg_idx,
                           const char *calling_function, String *value,
                           String *tmp, Json_wrapper *wr,
                           Json_scalar_holder *scalar, bool accept_string);

/**
  Check a non-empty val for character set. If it has character set
  my_charset_binary, signal error and return false. Else, try to convert to
  my_charset_utf8mb4_bin. If this fails, signal error and return true, else
  return false.

  @param[in]     val       the string to be checked
  @param[in,out] buf       buffer to hold the converted string
  @param[out]    resptr    the resulting, possibly converted string,
                           only set if no error
  @param[out]    reslength the length of resptr
  @param[in]     require_string
                           If true, give error messages if binary string. If we
                           see a conversion error (space), we give error
                           notwithstanding this parameter value

  @returns True if the string could not be converted. False on success.
*/
bool ensure_utf8mb4(const String &val, String *buf, const char **resptr,
                    size_t *reslength, bool require_string);

/**
  Represents the JSON function JSON_VALID( <value> )
*/
class Item_func_json_valid final : public Item_int_func {
  String m_value;

 public:
  Item_func_json_valid(const POS &pos, Item *a) : Item_int_func(pos, a) {}

  const char *func_name() const override { return "json_valid"; }
  enum Functype functype() const override { return JSON_VALID_FUNC; }

  bool is_bool_func() const override { return true; }

  longlong val_int() override;

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_JSON)) return true;
    set_nullable(true);
    return false;
  }
};

/**
  Represents the JSON function JSON_SCHEMA_VALID( <json schema>, <json doc> )
*/
class Item_func_json_schema_valid final : public Item_bool_func {
 public:
  Item_func_json_schema_valid(const POS &pos, Item *a, Item *b);
  ~Item_func_json_schema_valid() override;

  const char *func_name() const override { return "json_schema_valid"; }
  enum Functype functype() const override { return JSON_SCHEMA_VALID_FUNC; }

  bool val_bool() override;

  longlong val_int() override { return val_bool() ? 1 : 0; }

  bool fix_fields(THD *, Item **) override;

  void cleanup() override;

 private:
  Json_schema_validator m_cached_schema_validator;
};

/**
  Represents the JSON function
  JSON_SCHEMA_VALIDATION_REPORT( <json schema>, <json doc> )
*/
class Item_func_json_schema_validation_report final : public Item_json_func {
 public:
  Item_func_json_schema_validation_report(THD *thd, const POS &pos,
                                          PT_item_list *a);
  ~Item_func_json_schema_validation_report() override;

  const char *func_name() const override {
    return "json_schema_validation_report";
  }

  enum Functype functype() const override {
    return JSON_SCHEMA_VALIDATION_REPORT_FUNC;
  }

  bool val_json(Json_wrapper *wr) override;

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_JSON)) return true;
    set_nullable(true);
    return false;
  }

  bool fix_fields(THD *, Item **) override;

  void cleanup() override;

 private:
  Json_schema_validator m_cached_schema_validator;
};

/**
  Represents the JSON function JSON_CONTAINS()
*/
class Item_func_json_contains final : public Item_int_func {
  String m_doc_value;
  Json_path_cache m_path_cache;

 public:
  Item_func_json_contains(THD *thd, const POS &pos, PT_item_list *a)
      : Item_int_func(pos, a), m_path_cache(thd, arg_count) {}

  const char *func_name() const override { return "json_contains"; }
  enum Functype functype() const override { return JSON_CONTAINS; }
  optimize_type select_optimize(const THD *) override { return OPTIMIZE_KEY; }
  bool gc_subst_analyzer(uchar **) override { return true; }

  bool is_bool_func() const override { return true; }

  longlong val_int() override;

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_JSON)) return true;
    if (param_type_is_default(thd, 1, 3)) return true;
    set_nullable(true);
    return false;
  }

  /** Cleanup between executions of the statement */
  void cleanup() override;

  enum_const_item_cache can_cache_json_arg(Item *arg) override {
    return (arg == args[0] || arg == args[1]) ? CACHE_JSON_VALUE : CACHE_NONE;
  }
};

/**
  Represents the JSON function JSON_CONTAINS_PATH()
*/
class Item_func_json_contains_path final : public Item_int_func {
  String m_doc_value;
  enum_one_or_all_type m_cached_ooa;

  // Cache for constant path expressions
  Json_path_cache m_path_cache;

 public:
  Item_func_json_contains_path(THD *thd, const POS &pos, PT_item_list *a)
      : Item_int_func(pos, a),
        m_cached_ooa(ooa_uninitialized),
        m_path_cache(thd, arg_count) {}

  const char *func_name() const override { return "json_contains_path"; }
  enum Functype functype() const override { return JSON_CONTAINS_PATH_FUNC; }

  bool is_bool_func() const override { return true; }

  longlong val_int() override;

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_JSON)) return true;
    if (param_type_is_default(thd, 1, -1)) return true;
    set_nullable(true);
    return false;
  }

  /** Cleanup between executions of the statement */
  void cleanup() override;

  enum_const_item_cache can_cache_json_arg(Item *arg) override {
    return (arg == args[0]) ? CACHE_JSON_VALUE : CACHE_NONE;
  }
};

/**
  Represents the JSON function JSON_TYPE
*/
class Item_func_json_type : public Item_str_func {
  String m_value;

 public:
  Item_func_json_type(const POS &pos, Item *a) : Item_str_func(pos, a) {}

  const char *func_name() const override { return "json_type"; }
  enum Functype functype() const override { return JSON_TYPE_FUNC; }

  bool resolve_type(THD *) override;

  String *val_str(String *) override;
};

/**
  Represents a "CAST( <value> AS JSON )" coercion.
*/
class Item_typecast_json final : public Item_json_func {
  typedef Item_json_func super;

 public:
  Item_typecast_json(THD *thd, const POS &pos, Item *a)
      : Item_json_func(thd, pos, a) {}

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (Item_json_func::resolve_type(thd)) return true;
    return args[0]->propagate_type(thd, MYSQL_TYPE_JSON, false, true);
  }

  void print(const THD *thd, String *str,
             enum_query_type query_type) const override;
  const char *func_name() const override { return "cast_as_json"; }
  const char *cast_type() const { return "json"; }
  bool val_json(Json_wrapper *wr) override;
};

/**
  Represents the JSON function JSON_LENGTH()
*/
class Item_func_json_length final : public Item_int_func {
  String m_doc_value;

 public:
  Item_func_json_length(const POS &pos, Item *doc) : Item_int_func(pos, doc) {}

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_JSON)) return true;
    if (param_type_is_default(thd, 1, 2)) return true;
    set_nullable(true);
    return false;
  }

  const char *func_name() const override { return "json_length"; }
  enum Functype functype() const override { return JSON_LENGTH_FUNC; }

  longlong val_int() override;
};

/**
  Represents the JSON function JSON_DEPTH()
*/
class Item_func_json_depth final : public Item_int_func {
  String m_doc_value;

 public:
  Item_func_json_depth(const POS &pos, Item *a) : Item_int_func(pos, a) {}

  const char *func_name() const override { return "json_depth"; }
  enum Functype functype() const override { return JSON_DEPTH_FUNC; }

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_JSON)) return true;
    set_nullable(true);
    return false;
  }

  longlong val_int() override;
};

/**
  Represents the JSON function JSON_KEYS()
*/
class Item_func_json_keys : public Item_json_func {
  String m_doc_value;

 public:
  Item_func_json_keys(THD *thd, const POS &pos, Item *a)
      : Item_json_func(thd, pos, a) {}

  Item_func_json_keys(THD *thd, const POS &pos, Item *a, Item *b)
      : Item_json_func(thd, pos, a, b) {}

  const char *func_name() const override { return "json_keys"; }

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (Item_json_func::resolve_type(thd)) return true;
    if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_JSON)) return true;
    if (param_type_is_default(thd, 1, 2)) return true;
    return false;
  }

  bool val_json(Json_wrapper *wr) override;
};

/**
  Represents the JSON function JSON_EXTRACT()
*/
class Item_func_json_extract final : public Item_json_func {
  String m_doc_value;

 public:
  Item_func_json_extract(THD *thd, const POS &pos, PT_item_list *a)
      : Item_json_func(thd, pos, a) {}

  Item_func_json_extract(THD *thd, const POS &pos, Item *a, Item *b)
      : Item_json_func(thd, pos, a, b) {}

  const char *func_name() const override { return "json_extract"; }
  enum Functype functype() const override { return JSON_EXTRACT_FUNC; }

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (Item_json_func::resolve_type(thd)) return true;
    if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_JSON)) return true;
    if (param_type_is_default(thd, 1, -1)) return true;
    return false;
  }

  bool val_json(Json_wrapper *wr) override;

  bool eq(const Item *item) const override;
};

/// Base class for all the functions that take a JSON document as the first
/// argument and one of more pairs of a JSON path and a value to insert into the
/// JSON document, and returns the modified JSON document.
class Item_func_modify_json_in_path : public Item_json_func {
 protected:
  template <typename... Args>
  explicit Item_func_modify_json_in_path(Args &&...parent_args)
      : Item_json_func(std::forward<Args>(parent_args)...) {
    // The function does not necessarily return NULL when an argument is NULL.
    // It returns NULL only if the first argument is NULL, or if one of the JSON
    // path arguments is null. The set of tables for which the function is
    // null-rejecting, is calculated in resolve_type() and possibly updated in
    // update_used_tables().
    null_on_null = false;
  }
  String m_doc_value;

 public:
  bool resolve_type(THD *thd) final;
  void update_used_tables() final;

 private:
  /// Calculates the set of tables to return from not_used_tables(). The
  /// returned value is cached by resolve_type() and update_used_tables().
  table_map calculate_not_null_tables() const;
};

/**
  Represents the JSON function JSON_ARRAY_APPEND()
*/
class Item_func_json_array_append final : public Item_func_modify_json_in_path {
 public:
  Item_func_json_array_append(THD *thd, const POS &pos, PT_item_list *a)
      : Item_func_modify_json_in_path(thd, pos, a) {}

  const char *func_name() const override { return "json_array_append"; }

  bool val_json(Json_wrapper *wr) override;
};

/**
  Represents the JSON function JSON_INSERT()
*/
class Item_func_json_insert : public Item_func_modify_json_in_path {
 public:
  Item_func_json_insert(THD *thd, const POS &pos, PT_item_list *a)
      : Item_func_modify_json_in_path(thd, pos, a) {}

  const char *func_name() const override { return "json_insert"; }

  bool val_json(Json_wrapper *wr) override;
};

/**
  Represents the JSON function JSON_ARRAY_INSERT()
*/
class Item_func_json_array_insert final : public Item_func_modify_json_in_path {
 public:
  Item_func_json_array_insert(THD *thd, const POS &pos, PT_item_list *a)
      : Item_func_modify_json_in_path(thd, pos, a) {}

  const char *func_name() const override { return "json_array_insert"; }

  bool val_json(Json_wrapper *wr) override;
};

/**
  Common base class for JSON_SET() and JSON_REPLACE().
*/
class Item_func_json_set_replace : public Item_func_modify_json_in_path {
  /// True if this is JSON_SET, false if it is JSON_REPLACE.
  const bool m_json_set;
  Json_path_clone m_path;
  bool can_use_in_partial_update() const override { return true; }

 protected:
  template <typename... Args>
  explicit Item_func_json_set_replace(bool json_set, Args &&...parent_args)
      : Item_func_modify_json_in_path(std::forward<Args>(parent_args)...),
        m_json_set(json_set),
        m_path(key_memory_JSON) {}

 public:
  bool val_json(Json_wrapper *wr) override;
};

/**
  Represents the JSON function JSON_SET()
*/
class Item_func_json_set final : public Item_func_json_set_replace {
 public:
  template <typename... Args>
  explicit Item_func_json_set(Args &&...parent_args)
      : Item_func_json_set_replace(true, std::forward<Args>(parent_args)...) {}

  const char *func_name() const override { return "json_set"; }
};

/**
  Represents the JSON function JSON_REPLACE()
*/
class Item_func_json_replace final : public Item_func_json_set_replace {
 public:
  template <typename... Args>
  explicit Item_func_json_replace(Args &&...parent_args)
      : Item_func_json_set_replace(false, std::forward<Args>(parent_args)...) {}

  const char *func_name() const override { return "json_replace"; }
};

/**
  Represents the JSON function JSON_ARRAY()
*/
class Item_func_json_array final : public Item_json_func {
 public:
  template <typename... Args>
  explicit Item_func_json_array(Args &&...parent_args)
      : Item_json_func(std::forward<Args>(parent_args)...) {
    // Does not return NULL on NULL input. A NULL argument is interpreted as the
    // JSON null literal.
    null_on_null = false;
  }

  const char *func_name() const override { return "json_array"; }
  enum Functype functype() const override { return JSON_ARRAY_FUNC; }

  bool resolve_type(THD *thd) override {
    if (Item_json_func::resolve_type(thd)) return true;
    if (param_type_is_default(thd, 0, -1)) return true;
    return false;
  }

  bool val_json(Json_wrapper *wr) override;
};

/**
  Represents the JSON function JSON_OBJECT()
*/
class Item_func_json_row_object final : public Item_json_func {
  String tmp_key_value;

 public:
  Item_func_json_row_object(THD *thd, const POS &pos, PT_item_list *a)
      : Item_json_func(thd, pos, a) {
    // Does not return NULL on NULL input. If a key argument is NULL, an error
    // is raised. If a value argument is NULL, it is interpreted as the JSON
    // null literal.
    null_on_null = false;
  }

  const char *func_name() const override { return "json_object"; }
  enum Functype functype() const override { return JSON_OBJECT_FUNC; }

  bool resolve_type(THD *thd) override {
    if (Item_json_func::resolve_type(thd)) return true;
    if (param_type_is_default(thd, 0, -1)) return true;
    return false;
  }

  bool val_json(Json_wrapper *wr) override;
};

/**
  Represents the JSON function JSON_SEARCH()
*/
class Item_func_json_search : public Item_json_func {
  String m_doc_value;
  enum_one_or_all_type m_cached_ooa;

  // LIKE machinery
  Item_string *m_source_string_item;
  Item_func_like *m_like_node;

 public:
  /**
    Construct a JSON_SEARCH() node.

    @param parent_args arguments to pass to Item_json_func's constructor
  */
  template <typename... Args>
  Item_func_json_search(Args &&...parent_args)
      : Item_json_func(std::forward<Args>(parent_args)...),
        m_cached_ooa(ooa_uninitialized) {}

  const char *func_name() const override { return "json_search"; }

  enum Functype functype() const override { return JSON_SEARCH_FUNC; }

  bool val_json(Json_wrapper *wr) override;

  /**
    Bind logic for the JSON_SEARCH() node.
  */
  bool fix_fields(THD *, Item **) override;

  bool resolve_type(THD *thd) override {
    if (Item_json_func::resolve_type(thd)) return true;
    if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_JSON)) return true;
    if (param_type_is_default(thd, 1, -1)) return true;
    return false;
  }

  void cleanup() override;
};

/**
  Represents the JSON function JSON_REMOVE()
*/
class Item_func_json_remove : public Item_json_func {
  String m_doc_value;
  bool can_use_in_partial_update() const override { return true; }

 public:
  template <typename... Args>
  Item_func_json_remove(Args &&...parent_args)
      : Item_json_func(std::forward<Args>(parent_args)...) {}

  const char *func_name() const override { return "json_remove"; }

  bool resolve_type(THD *thd) override {
    if (Item_json_func::resolve_type(thd)) return true;
    if (param_type_is_default(thd, 0, 1, MYSQL_TYPE_JSON)) return true;
    if (param_type_is_default(thd, 1, -1)) return true;
    return false;
  }

  bool val_json(Json_wrapper *wr) override;
};

/**
  Represents the JSON function JSON_MERGE_PRESERVE.
*/
class Item_func_json_merge_preserve : public Item_json_func {
 public:
  Item_func_json_merge_preserve(THD *thd, const POS &pos, PT_item_list *a)
      : Item_json_func(thd, pos, a) {}

  const char *func_name() const override { return "json_merge_preserve"; }

  bool resolve_type(THD *thd) override {
    if (Item_json_func::resolve_type(thd)) return true;
    if (param_type_is_default(thd, 0, -1, MYSQL_TYPE_JSON)) return true;
    return false;
  }

  bool val_json(Json_wrapper *wr) override;
};

/**
  Represents the JSON function JSON_MERGE. It is a deprecated alias
  for JSON_MERGE_PRESERVE.
*/
class Item_func_json_merge : public Item_func_json_merge_preserve {
 public:
  Item_func_json_merge(THD *thd, const POS &pos, PT_item_list *a);

  bool is_deprecated() const override { return true; }
};

/**
  Represents the JSON function JSON_MERGE_PATCH.
*/
class Item_func_json_merge_patch : public Item_json_func {
 public:
  Item_func_json_merge_patch(THD *thd, const POS &pos, PT_item_list *a)
      : Item_json_func(thd, pos, a) {}

  const char *func_name() const override { return "json_merge_patch"; }

  bool resolve_type(THD *thd) override {
    if (Item_json_func::resolve_type(thd)) return true;
    if (param_type_is_default(thd, 0, -1, MYSQL_TYPE_JSON)) return true;
    return false;
  }

  bool val_json(Json_wrapper *wr) override;
};

/**
  Represents the JSON function JSON_QUOTE()
*/
class Item_func_json_quote : public Item_str_func {
  String m_value;

 public:
  Item_func_json_quote(const POS &pos, PT_item_list *a)
      : Item_str_func(pos, a) {}

  const char *func_name() const override { return "json_quote"; }

  enum Functype functype() const override { return JSON_QUOTE_FUNC; }

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (param_type_is_default(thd, 0, -1)) return true;
    set_nullable(true);

    /*
     Any interior character could be replaced by a 6 character
     escape sequence. Plus we will add 2 framing quote characters.
    */
    const uint32 max_char_length = (6 * args[0]->max_char_length()) + 2;
    set_data_type_string(max_char_length, &my_charset_utf8mb4_bin);
    return false;
  }

  String *val_str(String *tmpspace) override;
};

/**
  Represents the JSON function JSON_UNQUOTE()
*/
class Item_func_json_unquote : public Item_str_func {
  String m_value;
  String m_conversion_buffer;

 public:
  Item_func_json_unquote(const POS &pos, PT_item_list *a)
      : Item_str_func(pos, a) {}

  Item_func_json_unquote(const POS &pos, Item *a) : Item_str_func(pos, a) {}

  const char *func_name() const override { return "json_unquote"; }

  enum Functype functype() const override { return JSON_UNQUOTE_FUNC; }

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (param_type_is_default(thd, 0, -1)) return true;
    set_nullable(true);
    set_data_type_string(args[0]->max_char_length(), &my_charset_utf8mb4_bin);
    return false;
  }

  String *val_str(String *str) override;
};

/**
  Represents the JSON_PRETTY function.
*/
class Item_func_json_pretty final : public Item_str_func {
 public:
  Item_func_json_pretty(const POS &pos, Item *a) : Item_str_func(pos, a) {}

  const char *func_name() const override { return "json_pretty"; }

  enum Functype functype() const override { return JSON_PRETTY_FUNC; }

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (param_type_is_default(thd, 0, -1, MYSQL_TYPE_JSON)) return true;
    set_data_type_string(MAX_BLOB_WIDTH, &my_charset_utf8mb4_bin);
    return false;
  }

  String *val_str(String *str) override;
};

/**
  Class that represents the function JSON_STORAGE_SIZE.
*/
class Item_func_json_storage_size final : public Item_int_func {
 public:
  Item_func_json_storage_size(const POS &pos, Item *a)
      : Item_int_func(pos, a) {}
  const char *func_name() const override { return "json_storage_size"; }
  enum Functype functype() const override { return JSON_STORAGE_SIZE_FUNC; }

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (param_type_is_default(thd, 0, -1, MYSQL_TYPE_JSON)) return true;
    if (Item_int_func::resolve_type(thd)) return true;
    set_nullable(true);
    return false;
  }

  longlong val_int() override;
};

/**
  Class that represents the function JSON_STORAGE_FREE.
*/
class Item_func_json_storage_free final : public Item_int_func {
 public:
  Item_func_json_storage_free(const POS &pos, Item *a)
      : Item_int_func(pos, a) {}
  const char *func_name() const override { return "json_storage_free"; }
  enum Functype functype() const override { return JSON_STORAGE_FREE_FUNC; }

  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (param_type_is_default(thd, 0, -1, MYSQL_TYPE_JSON)) return true;
    return false;
  }

  longlong val_int() override;
};

/**
  Class that represents CAST(<expr> AS <type> ARRAY)
*/

class Item_func_array_cast final : public Item_func {
  /// Type to cast to
  Cast_target cast_type;
  /**
    Whether use of CAST(.. AS .. ARRAY) is allowed

    Currently use of CAST(.. AS .. ARRAY) is limited only to CREATE
    TABLE/INDEX. In all other cases an error is thrown. This flag is set to
    true only for allowed cases to ensure allowed function usage.
  */
  bool m_is_allowed{false};

  /**
    An array used by #save_in_field_inner() to store the result of an array cast
    operation. It is cached in the Item in order to avoid the need for
    reallocation on each row.
  */
  unique_ptr_destroy_only<Json_array> m_result_array;

 protected:
  void add_json_info(Json_object *obj) override;

 public:
  Item_func_array_cast(const POS &pos, Item *a, Cast_target type, uint len_arg,
                       uint dec_arg, const CHARSET_INFO *cs_arg);
  ~Item_func_array_cast() override;
  const char *func_name() const override { return "cast_as_array"; }
  enum Functype functype() const override { return TYPECAST_FUNC; }
  bool returns_array() const override { return true; }
  bool val_json(Json_wrapper *wr) override;
  void print(const THD *thd, String *str,
             enum_query_type query_type) const override;
  enum Item_result result_type() const override;
  bool resolve_type(THD *) override;
  Field *tmp_table_field(TABLE *table) override;
  bool fix_fields(THD *thd, Item **ref) override;
  void cleanup() override;
  void allow_array_cast() override { m_is_allowed = true; }
  type_conversion_status save_in_field_inner(Field *field,
                                             bool no_conversions) override;
  // Regular val_x() funcs shouldn't be called
  /* purecov: begin inspected */
  longlong val_int() override {
    assert(false);
    return 0;
  }
  String *val_str(String *) override {
    assert(false);
    return nullptr;
  }
  my_decimal *val_decimal(my_decimal *) override {
    assert(false);
    return nullptr;
  }
  double val_real() override {
    assert(false);
    return 0;
  }
  bool get_date(MYSQL_TIME *, my_time_flags_t) override {
    assert(false);
    return true;
  }
  bool get_time(MYSQL_TIME *) override {
    assert(false);
    return true;
  }
  /* purecov: end */
};

class Item_func_json_overlaps : public Item_bool_func {
 public:
  Item_func_json_overlaps(const POS &pos, Item *a, Item *b)
      : Item_bool_func(pos, a, b) {}
  const char *func_name() const override { return "json_overlaps"; }
  enum Functype functype() const override { return JSON_OVERLAPS; }
  bool gc_subst_analyzer(uchar **) override { return true; }
  optimize_type select_optimize(const THD *) override { return OPTIMIZE_KEY; }
  longlong val_int() override;
  Item *key_item() const override;
  enum_const_item_cache can_cache_json_arg(Item *arg) override {
    return (arg == args[0] || arg == args[1]) ? CACHE_JSON_VALUE : CACHE_NONE;
  }
};

class Item_func_member_of : public Item_bool_func {
 public:
  Item_func_member_of(const POS &pos, Item *a, Item *b)
      : Item_bool_func(pos, a, b) {}
  const char *func_name() const override { return "member of"; }
  enum Functype functype() const override { return MEMBER_OF_FUNC; }
  bool resolve_type(THD *thd) override {
    if (reject_vector_args()) return true;
    if (param_type_is_default(thd, 0, 2, MYSQL_TYPE_JSON)) return true;
    args[0]->mark_json_as_scalar();
    return false;
  }
  bool gc_subst_analyzer(uchar **) override { return true; }
  optimize_type select_optimize(const THD *) override { return OPTIMIZE_KEY; }
  longlong val_int() override;
  void print(const THD *thd, String *str,
             enum_query_type query_type) const override;
  Item *key_item() const override { return args[1]; }
  enum_const_item_cache can_cache_json_arg(Item *arg) override {
    return (arg == args[1]) ? CACHE_JSON_VALUE
                            : ((arg == args[0]) ? CACHE_JSON_ATOM : CACHE_NONE);
  }
};

/**
  Class implementing the JSON_VALUE function.

  Functionality-wise it's a combination of CAST, JSON_UNQUOTE and JSON_EXTRACT,
  but with additional functionality for flexible handling of empty values and
  conversion errors.
*/
class Item_func_json_value final : public Item_func {
 public:
  Item_func_json_value(const POS &pos, Item *arg, Item *path,
                       const Cast_type &cast_type, unsigned length,
                       unsigned precision, Json_on_response_type on_empty_type,
                       Item *on_empty_default,
                       Json_on_response_type on_error_type,
                       Item *on_error_default);
  ~Item_func_json_value() override;
  const char *func_name() const override { return "json_value"; }
  enum Functype functype() const override { return JSON_VALUE_FUNC; }
  enum Item_result result_type() const override;
  bool resolve_type(THD *) override;
  bool fix_fields(THD *thd, Item **ref) override;
  void print(const THD *thd, String *str,
             enum_query_type query_type) const override;
  bool eq_specific(const Item *item) const override;
  bool val_json(Json_wrapper *wr) override;
  String *val_str(String *buffer) override;
  double val_real() override;
  longlong val_int() override;
  my_decimal *val_decimal(my_decimal *value) override;
  bool get_date(MYSQL_TIME *ltime, my_time_flags_t flags) override;
  bool get_time(MYSQL_TIME *ltime) override;
  Json_on_response_type on_empty_response_type() const;
  Json_on_response_type on_error_response_type() const;

 private:
  /// Represents a default value given in JSON_VALUE's DEFAULT xxx ON EMPTY or
  /// DEFAULT xxx ON ERROR clause.
  struct Default_value;

  /// Parsed path.
  Json_path m_path_json;
  /// Type of the ON EMPTY clause.
  Json_on_response_type m_on_empty;
  /// Type of the ON ERROR clause.
  Json_on_response_type m_on_error;
  /// The default value for ON EMPTY (if not ERROR or NULL ON EMPTY).
  unique_ptr_destroy_only<Default_value> m_default_empty;
  /// The default value for ON EMPTY (if not ERROR or NULL ON EMPTY).
  unique_ptr_destroy_only<Default_value> m_default_error;
  /// The target data type.
  Cast_target m_cast_target;

  /**
    Creates a Json_value_default object representing the default value given in
    a DEFAULT xxx ON EMPTY clause or a DEFAULT xxx ON ERROR clause.

    @param thd       the current session
    @param item      the Item that represents the default value expression
    @return a pointer to the created object on success, nullptr on error
  */
  unique_ptr_destroy_only<Default_value> create_json_value_default(THD *thd,
                                                                   Item *item);

  /**
    Extracts the JSON value at the given path.

    @param[out] json the extracted JSON value, if the path matched exactly
      one value; empty otherwise
    @param[out] return_default the default value to return if a
      DEFAULT ... ON EMPTY or DEFAULT ... ON ERROR clause was invoked,
      or nullptr if no DEFAULT clause was invoked
    @return true if an error was raised, false otherwise
  */
  bool extract_json_value(Json_wrapper *json,
                          const Default_value **return_default);

  /// Implements val_int() for RETURNING SIGNED and RETURNING UNSIGNED.
  int64_t extract_integer_value();
  /// Implements val_int() for RETURNING YEAR
  int64_t extract_year_value();
  /// Implements get_date() for RETURNING DATE.
  bool extract_date_value(MYSQL_TIME *ltime);
  /// Implements get_time() for RETURNING TIME.
  bool extract_time_value(MYSQL_TIME *ltime);
  /// Implements get_date() for RETURNING DATETIME.
  bool extract_datetime_value(MYSQL_TIME *ltime);
  /// Implements val_decimal() for RETURNING DECIMAL.
  my_decimal *extract_decimal_value(my_decimal *value);
  /// Implements val_str() for RETURNING CHAR and RETURNING BINARY.
  String *extract_string_value(String *buffer);
  /// Implements val_real() for RETURNING FLOAT/REAL/DOUBLE.
  double extract_real_value();
};

/**
  Turn a GEOMETRY value into a JSON value per the GeoJSON specification
  revision 1.0. This method is implemented in item_geofunc.cc.

  @param[in,out] wr The wrapper to be stuffed with the JSON value.
  @param[in]     swkb The source GEOMETRY value.
  @param[in]     calling_function Name of user-invoked function (for errors)
  @param[in]     max_decimal_digits See the user documentation for ST_AsGeoJSON.
  @param[in]     add_bounding_box See the user documentation for ST_AsGeoJSON.
  @param[in]     add_short_crs_urn See the user documentation for ST_AsGeoJSON.
  @param[in]     add_long_crs_urn See the user documentation for ST_AsGeoJSON.
  @param[in,out] geometry_srid Spatial Reference System Identifier to be filled
  in.

  @return false if the conversion succeeds, true otherwise
*/
bool geometry_to_json(Json_wrapper *wr, String *swkb,
                      const char *calling_function, int max_decimal_digits,
                      bool add_bounding_box, bool add_short_crs_urn,
                      bool add_long_crs_urn, uint32 *geometry_srid);

/**
  Convert a value represented with an Item to a JSON value

  @param[in]     item      the input value, may be any data type
  @param[in]     func_name for error reporting
  @param[in,out] wr        the result wrapper for the JSON value

  @return false if success, true if error
*/
bool convert_value_to_json(Item *item, const char *func_name, Json_wrapper *wr);
/**
  Convert JSON values or MySQL values to JSON. Converts SQL NULL
  to the JSON null literal.

  @param[in]     args       arguments to function
  @param[in]     arg_idx    the index of the argument to process
  @param[in]     calling_function    name of the calling function
  @param[in,out] value      working area (if the returned Json_wrapper points
                            to a binary value rather than a DOM, this string
                            will end up holding the binary representation, and
                            it must stay alive until the wrapper is destroyed
                            or converted from binary to DOM)
  @param[in,out] tmp        temporary scratch space for converting strings to
                            the correct charset; only used if accept_string is
                            true and conversion is needed
  @param[in,out] wr         the result wrapper
  @returns false if we found a value or NULL, true otherwise
*/
bool get_atom_null_as_null(Item **args, uint arg_idx,
                           const char *calling_function, String *value,
                           String *tmp, Json_wrapper *wr);

/**
  Gets a JSON object member name from an Item. An error is raised if
  the Item evaluates to NULL, or if it cannot be converted to a
  utf8mb4 string.

  @param[in]  thd         THD handle
  @param[in]  arg_item    An argument Item
  @param[out] value       Where to materialize the arg_item's string value
  @param[out] utf8_res    Buffer for use by ensure_utf8mb4.
  @param[out] safep       String pointer after any relevant conversion
  @param[out] safe_length Corresponding string length

  @returns true if the Item is not a utf8mb4 string
*/
bool get_json_object_member_name(const THD *thd, Item *arg_item, String *value,
                                 String *utf8_res, const char **safep,
                                 size_t *safe_length);
using Json_dom_ptr = std::unique_ptr<Json_dom>;

bool parse_json(const String &res, Json_dom_ptr *dom, bool require_str_or_json,
                const JsonParseErrorHandler &error_handler,
                const JsonErrorHandler &depth_handler);

/**
  Apply a sequence of JSON diffs to the value stored in a JSON column.

    @param field  the column to update
    @param diffs  the diffs to apply
    @return an enum_json_diff_status value that tells if the diffs were
    applied successfully
 */
enum_json_diff_status apply_json_diffs(Field_json *field,
                                       const Json_diff_vector *diffs);

bool save_json_to_field(THD *thd, Field *field, const Json_wrapper *w,
                        bool no_error);
#endif /* ITEM_JSON_FUNC_INCLUDED */
