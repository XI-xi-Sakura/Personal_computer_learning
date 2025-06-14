/* Copyright (c) 2023, 2025, Oracle and/or its affiliates.

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

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include "mysql/components/component_implementation.h"
#include "mysql/components/service_implementation.h"
#include "mysql/components/services/psi_memory_service.h"

namespace psi_memory_v2_empty {

static void register_memory(const char *, PSI_memory_info *, int) {}

static PSI_memory_key memory_alloc(PSI_memory_key, size_t,
                                   struct PSI_thread **owner) {
  *owner = nullptr;
  return PSI_NOT_INSTRUMENTED;
}

static PSI_memory_key memory_realloc(PSI_memory_key, size_t, size_t,
                                     struct PSI_thread **owner) {
  *owner = nullptr;
  return PSI_NOT_INSTRUMENTED;
}

static PSI_memory_key memory_claim(PSI_memory_key, size_t,
                                   struct PSI_thread **owner, bool) {
  *owner = nullptr;
  return PSI_NOT_INSTRUMENTED;
}

static void memory_free(PSI_memory_key, size_t, struct PSI_thread *) {}

}  // namespace psi_memory_v2_empty

BEGIN_SERVICE_IMPLEMENTATION(HARNESS_COMPONENT_NAME, psi_memory_v2)
psi_memory_v2_empty::register_memory, psi_memory_v2_empty::memory_alloc,
    psi_memory_v2_empty::memory_realloc, psi_memory_v2_empty::memory_claim,
    psi_memory_v2_empty::memory_free, END_SERVICE_IMPLEMENTATION();