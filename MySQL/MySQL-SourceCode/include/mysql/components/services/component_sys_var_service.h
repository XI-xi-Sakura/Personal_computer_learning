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

#ifndef COMPONENT_SYS_VAR_SERVICE_H
#define COMPONENT_SYS_VAR_SERVICE_H

#include <stddef.h>

#include <mysql/components/service.h>
#include <nulls.h>

#include <mysql/components/services/bits/system_variables_bits.h>

/**
  A utility class for the ENUM variables
*/

struct TYPE_LIB {
  size_t count{0};           /* How many types */
  const char *name{nullptr}; /* Name of typelib */
  const char **type_names{nullptr};
  unsigned int *type_lengths{nullptr};
};

class THD;
struct SYS_VAR;
#define MYSQL_THD THD *

/**
  Signature for the check function

  This is called to check if the value supplied is valid and can be set
  as a new variable value at that time.
  It needs to extract the value supplied from the value API pointer.
  Note that extracting this value can be expensive (e.g. a scalar subquery)
  hence it should be done only once. This is why the result of this should
  be stored into the save output parameter so that it can be further reused by
  update() etc.
  For a multi-variable SET statement the server will first call all of the
  check functions and only if they all return success it will start calling the
  update functions.
  So the idea is that the update function should succeed no matter what.
  And all the necessary checks should be done in the check function.
  If you do not supply a check or update function the server will use the basic
  ones to check e.g. min and max values for the integer types etc.

  @param thd The thread handle to the current thread
  @param var handle to the system variable definition
  @param[out] save placeholder for the value. This should be cast according to
  the type
  @param value Interface to extract the actual parameter value.
  @return status
  @retval 0 success
  @retval 1 failure

  @sa mysql_sys_var_update_func, mysql_service_component_sys_variable_register_t
*/
typedef int (*mysql_sys_var_check_func)(MYSQL_THD thd, SYS_VAR *var, void *save,
                                        struct st_mysql_value *value);

/**
  Signature for the update function

  This is called to set the updated value into the var_ptr placeholder and
  invoke any side effects that may stem from setting this system variable.

  It receives the pre-calculated value (usually from
  @ref mysql_sys_var_check_func) in the save pointer.
  It needs to set it into the var_ptr pointer and invoke any side effects.

  For a multi-variable SET statement the server will first call all of the
  check functions and only if they all return success it will start calling the
  update functions.
  So the idea is that the update function should succeed no matter what.
  And all the necessary checks should be done in the check function.
  If you do not supply an update function the server will use the basic
  one to set the value according to the variable's type.

  @param thd The thread handle to the current thread
  @param var handle to the system variable definition
  @param[out] val_ptr placeholder for the value. Store save in here.
  @param save The pre-calculated value from check.

  @sa mysql_sys_var_check_func, mysql_service_component_sys_variable_register_t
*/
typedef void (*mysql_sys_var_update_func)(MYSQL_THD thd, SYS_VAR *var,
                                          void *val_ptr, const void *save);

/**
  @addtogroup group_components_services_sys_var_service_args Variable
  definitions

  You need to fill one of these in an pass it to the registration function.
  The values are copied so this doesn't have to survive once the registration
  call is done.
  So usually it's done as an automatic variable in the stack.

  Make sure to use the right one for your variable type.
  See @ref group_components_services_sys_var_service_types for what to use
  for the individual types.

  @sa mysql_service_component_sys_variable_register service.

  @{
*/

/**
 Defines an integral placeholder to fill in with values and
 pass to the registration function.

 Make sure to fill in def_val, min_val, max_val and blk_sz.
*/
#define INTEGRAL_CHECK_ARG(type) \
  struct type##_check_arg_s {    \
    type def_val;                \
    type min_val;                \
    type max_val;                \
    type blk_sz;                 \
  }

/**
 Defines an @ref TYPE_LIB placeholder to fill in with values and
 pass to the registration function.

 Make sure to fill in def_val and typelib.
*/
#define ENUM_CHECK_ARG(type)  \
  struct type##_check_arg_s { \
    unsigned long def_val;    \
    TYPE_LIB *typelib;        \
  }

/**
 Defines a bool placeholder to fill in with values and
 pass to the registration function.

 Make sure to fill in def_val.
*/
#define BOOL_CHECK_ARG(type)  \
  struct type##_check_arg_s { \
    bool def_val;             \
  }

/**
 Defines a char * placeholder to fill in with values and
 pass to the registration function.

 Make sure to fill in def_val.
*/
#define STR_CHECK_ARG(type)   \
  struct type##_check_arg_s { \
    char *def_val;            \
  }
/** @} */

/**
  @ingroup group_components_services_inventory

  Service to register variable and get variable value

  @sa mysql_component_sys_variable_imp
*/
BEGIN_SERVICE_DEFINITION(component_sys_variable_register)

/**
  Register a new system variable.

  The variable registered is then accessible in SQL as "SELECT
  component_name.name" The variable registration needs a global of the relevant
  type that stores the value.

  To define a new variable you need to:
  1. Decide on a variable type among one of the
   @ref group_components_services_sys_var_service_types
  2. Decide on attributes of the type among one of the
   @ref group_components_services_sys_var_service_flags
  3. Declare a local variable placeholder matching the type using one of the
   @ref group_components_services_sys_var_service_args macros.
  4. Fill in the placeholder values (min, max, default etc) as appropriate for
   the selected type.
  5. Provide storage for the variable value: usually a global variable of the
   relevant type.
  6. Call the function with all of the above to register the system variable.

  @warning: Make sure to unregister the system variable when you no longer
  intend to have it or when your component deinitializes. Failure to do so will
  lead to resource leaks and crashes.

  Typical use
  @code
  static int int_variable_value;
  static char *str_variable_value;
  static ulong enum_variable_value;

  static const char *enums[] = {"LOW", "MEDIUM", "STRONG", NullS};

  static TYPE_LIB enums_typelib_t = {array_elements(enums) - 1,
    "enums_typelib_t",
    enums, NULL};

  ...

  INTEGRAL_CHECK_ARG(int) int_arg;
  int_arg.def_val = 8;
  int_arg.min_val = 0;
  int_arg.max_val = 1024;
  int_arg.blk_sz = 0;
  if (mysql_service_component_sys_variable_register->register_variable(
    "foo", "int_sys_var", PLUGIN_VAR_INT,
    "Registering int sys_variable", NULL, NULL, (void *)&int_arg,
    (void *)&int_variable_value)) {
    deal_with_error();
  }

  STR_CHECK_ARG(str) str_arg;
  str_arg.def_val = NULL;
  if (mysql_service_component_sys_variable_register->register_variable(
    "foo", "str_sys_var", PLUGIN_VAR_STR | PLUGIN_VAR_MEMALLOC,
    "Registering string sys_variable", NULL, NULL, (void *)&str_arg,
  (void *)&str_variable_value)) {
    deal_with_error();
  }

  ENUM_CHECK_ARG(enum) enum_arg;
  enum_arg.def_val = 1; //  medium
  enum_arg.typelib = &enum_typelib_t;
  if (mysql_service_component_sys_variable_register->register_variable(
  "foo", "enum_sys_var", PLUGIN_VAR_ENUM,
  "Registering enum sys_variable", NULL, NULL, (void *)&enum_arg,
  (void *)&enum_variable_value)) {
    deal_with_error();
  }
  @endcode


  @sa mysql_service_component_sys_variable_unregister_t

  @param component_name The name of the component the system variable belongs
  to.
  @param name The name of the variable.
  @param flags one of @ref group_components_services_sys_var_service_types plus
   zero or more of group_components_services_sys_var_service_flags
  @param comment explanation of what the variable is to be shown in metadata
  tables
  @param check A function to be called to check for validity of the new value.
  @param update A function to be called when the variable value is updated
  @param check_arg The variable declared through one of
     @ref group_components_services_sys_var_service_args
  @param variable_value A pointer to a storage location of the relevant type.
  @retval true operation failed
  @retval false operation succeeded
*/
DECLARE_BOOL_METHOD(register_variable,
                    (const char *component_name, const char *name, int flags,
                     const char *comment, mysql_sys_var_check_func check,
                     mysql_sys_var_update_func update, void *check_arg,
                     void *variable_value));
/**
  Fetches the global value of a system variable

  Call this to get the global value of a variable. You can access both the
  component variables (SELECT @@global.component_name.variable_name) and the
  "legacy" system variables (SELECT @@global.variable_name) that are registered
  by the server component.
  To access the latter you need to pass "mysql_server" (lowercase) as a
  component name.

  A pointer to the value is returned into the val input/output argument. And the
  length of the value (as applicable) is returned into the out_length_of_val
  argument.

  In case when the user buffer was too small to copy the value, the call fails
  and needed buffer size is returned by 'out_length_of_val'.

  Typical use (char * variable):
  @code

  char *value, buffer_for_value[160];
  size_t value_length;

  value= &buffer_for_value[0];
  value_length= sizeof(buffer_for_value) - 1;
  get_variable("foo", "bar", &value, &value_length);

  printf("%.*s", (int) value_length, value);
  @endcode

  @param component_name Name of the component or "mysql_server" for the legacy
  ones.
  @param name name of the variable
  @param[in,out] val On input: a buffer to hold the value. On output a pointer
  to the value.
  @param[in,out] out_length_of_val On input: the buffer size. On output the
  length of the data copied.

  @retval true    failure
  @retval false   success
*/
#if defined(__cplusplus) && (__cplusplus >= 201402L)
[[deprecated("Use mysql_service_mysql_system_variable_reader->get() instead.")]]
#endif
DECLARE_BOOL_METHOD(get_variable, (const char *component_name, const char *name,
                                   void **val, size_t *out_length_of_val));

END_SERVICE_DEFINITION(component_sys_variable_register)

/**
  @ingroup group_components_services_inventory

  Service to unregister variable

  Make sure to call this for each variable registered.

  @sa mysql_service_component_sys_variable_unregister_t
*/
BEGIN_SERVICE_DEFINITION(component_sys_variable_unregister)

DECLARE_BOOL_METHOD(unregister_variable,
                    (const char *component_name, const char *name));

END_SERVICE_DEFINITION(component_sys_variable_unregister)

#endif /* COMPONENT_SYS_VAR_SERVICE_H */
