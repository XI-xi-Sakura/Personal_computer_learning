/* Copyright (c) 2016, 2025, Oracle and/or its affiliates.

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

#include <example_services.h>
#include <mysql/components/component_implementation.h>
#include <mysql/components/service_implementation.h>
#include <cstddef>

#include "mysql/components/services/registry.h"

REQUIRES_SERVICE_PLACEHOLDER(greetings_localization);

BEGIN_COMPONENT_REQUIRES(cyclic_dependency_test_component_2)
REQUIRES_SERVICE(greetings_localization), END_COMPONENT_REQUIRES();

class greetings_imp {
 public:
  static DEFINE_BOOL_METHOD(say_hello, (const char **)) { return true; }
};

BEGIN_SERVICE_IMPLEMENTATION(cyclic_dependency_test_component_2, greetings)
greetings_imp::say_hello, END_SERVICE_IMPLEMENTATION();

BEGIN_COMPONENT_PROVIDES(cyclic_dependency_test_component_2)
PROVIDES_SERVICE(cyclic_dependency_test_component_2, greetings),
    END_COMPONENT_PROVIDES();

BEGIN_COMPONENT_METADATA(cyclic_dependency_test_component_2)
END_COMPONENT_METADATA();

DECLARE_COMPONENT(cyclic_dependency_test_component_2,
                  "mysql:cyclic_dependency_test_component_2")
nullptr, nullptr END_DECLARE_COMPONENT();

DECLARE_LIBRARY_COMPONENTS &COMPONENT_REF(cyclic_dependency_test_component_2)
    END_DECLARE_LIBRARY_COMPONENTS
