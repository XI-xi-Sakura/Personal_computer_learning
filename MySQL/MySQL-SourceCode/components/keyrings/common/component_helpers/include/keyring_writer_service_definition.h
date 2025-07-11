/* Copyright (c) 2021, 2025, Oracle and/or its affiliates.

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

#ifndef KEYRING_WRITER_SERVICE_IMPL_INCLUDED
#define KEYRING_WRITER_SERVICE_IMPL_INCLUDED

#include <mysql/components/component_implementation.h>
#include <mysql/components/service_implementation.h>

#include <mysql/components/services/keyring_writer.h>

namespace keyring_common::service_definition {

class Keyring_writer_service_impl {
 public:
  /**
    Store data in keyring

    @param [in]  data_id        Data Identifier
    @param [in]  auth_id        Authorization ID
    @param [in]  data           Data to be stored
    @param [in]  data_size      Size of data to be stored
    @param [in]  data_type      Type of data

    @returns status of the operation
      @retval false Success
      @retval true  Failure
  */

  static DEFINE_BOOL_METHOD(store, (const char *data_id, const char *auth_id,
                                    const unsigned char *data, size_t data_size,
                                    const char *data_type));

  /**
    Remove data from keyring

    @param [in] data_id Data Identifier
    @param [in] auth_id Authorization ID

    @returns status of the operation
      @retval false Success - Key removed successfully or key not present.
      @retval true  Failure
  */

  static DEFINE_BOOL_METHOD(remove, (const char *data_id, const char *auth_id));
};

}  // namespace keyring_common::service_definition

#define KEYRING_WRITER_IMPLEMENTOR(component_name)                            \
  BEGIN_SERVICE_IMPLEMENTATION(component_name, keyring_writer)                \
  keyring_common::service_definition::Keyring_writer_service_impl::store,     \
      keyring_common::service_definition::Keyring_writer_service_impl::remove \
      END_SERVICE_IMPLEMENTATION()

#endif  // KEYRING_WRITER_SERVICE_IMPL_INCLUDED
