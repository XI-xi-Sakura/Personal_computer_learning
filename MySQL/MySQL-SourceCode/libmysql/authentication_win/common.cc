/* Copyright (c) 2011, 2025, Oracle and/or its affiliates.

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

#include "common.h"

#include <sddl.h>    // for ConvertSidToStringSid()
#include <secext.h>  // for GetUserNameEx()

#include "my_dbug.h"

template <>
void error_log_print<error_log_level::INFO>(const char *fmt, ...);
template <>
void error_log_print<error_log_level::WARNING>(const char *fmt, ...);
template <>
void error_log_print<error_log_level::ERROR>(const char *fmt, ...);

/**
  Option indicating desired level of logging. Values:

  0 - no logging
  1 - log only error messages
  2 - additionally log warnings
  3 - additionally log info notes
  4 - also log debug messages

  Value of this option should be taken into account in the
  implementation of  error_log_vprint() function (see
  log_client.cc).

  Note: No error or debug messages are logged in production code
  (see logging macros in common.h).
*/
int opt_auth_win_log_level = 2;

/** Connection class **************************************************/

/**
  Create connection out of an active MYSQL_PLUGIN_VIO object.

  @param[in] vio  pointer to a @c MYSQL_PLUGIN_VIO object used for
                  connection - it can not be nullptr
*/

Connection::Connection(MYSQL_PLUGIN_VIO *vio) : m_vio(vio), m_error(0) {
  assert(vio);
}

/**
  Write data to the connection.

  @param[in]  blob  data to be written

  @return 0 on success, VIO error code on failure.

  @note In case of error, VIO error code is stored in the connection object
  and can be obtained with @c error() method.
*/

int Connection::write(const Blob &blob) {
  m_error =
      m_vio->write_packet(m_vio, blob.ptr(), static_cast<int>(blob.len()));

#ifndef NDEBUG
  if (m_error) DBUG_PRINT("error", ("vio write error %d", m_error));
#endif

  return m_error;
}

/**
  Read data from connection.

  @return A Blob containing read packet or null Blob in case of error.

  @note In case of error, VIO error code is stored in the connection object
  and can be obtained with @c error() method.
*/

Blob Connection::read() {
  unsigned char *ptr;
  const int len = m_vio->read_packet(m_vio, &ptr);

  if (len < 0) {
    m_error = true;
    return Blob();
  }

  return Blob(ptr, len);
}

/** Sid class *****************************************************/

/**
  Create Sid object corresponding to a given account name.

  @param[in]  account_name  name of a Windows account

  The account name can be in any form accepted by @c LookupAccountName()
  function.

  @note In case of errors created object is invalid and its @c is_valid()
  method returns @c false.
*/

Sid::Sid(const wchar_t *account_name)
    : m_data(nullptr)
#ifndef NDEBUG
      ,
      m_as_string(nullptr)
#endif
{
  DWORD sid_size = 0, domain_size = 0;
  bool success;

  // Determine required buffer sizes

  success = LookupAccountNameW(nullptr, account_name, nullptr, &sid_size,
                               nullptr, &domain_size, &m_type);

  if (!success && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
#ifndef NDEBUG
    Error_message_buf error_buf;
    DBUG_PRINT("error", ("Could not determine SID buffer size, "
                         "LookupAccountName() failed with error %lX (%s)",
                         GetLastError(), get_last_error_message(error_buf)));
#endif
    return;
  }

  // Query for SID (domain is ignored)

  wchar_t *domain = new wchar_t[domain_size];
  m_data = (TOKEN_USER *)new BYTE[sid_size + sizeof(TOKEN_USER)];
  m_data->User.Sid = (BYTE *)m_data + sizeof(TOKEN_USER);

  success = LookupAccountNameW(nullptr, account_name, m_data->User.Sid,
                               &sid_size, domain, &domain_size, &m_type);

  if (!success || !is_valid()) {
#ifndef NDEBUG
    Error_message_buf error_buf;
    DBUG_PRINT("error", ("Could not determine SID of '%S', "
                         "LookupAccountName() failed with error %lX (%s)",
                         account_name, GetLastError(),
                         get_last_error_message(error_buf)));
#endif
    goto fail;
  }

  goto end;

fail:
  if (m_data) delete[] m_data;
  m_data = nullptr;

end:
  if (domain) delete[] domain;
}

/**
  Create Sid object corresponding to a given security token.

  @param[in]  token   security token of a Windows account

  @note In case of errors created object is invalid and its @c is_valid()
  method returns @c false.
*/

Sid::Sid(HANDLE token)
    : m_data(nullptr)
#ifndef NDEBUG
      ,
      m_as_string(nullptr)
#endif
{
  DWORD req_size = 0;
  bool success;

  // Determine required buffer size

  success = GetTokenInformation(token, TokenUser, nullptr, 0, &req_size);
  if (!success && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
#ifndef NDEBUG
    Error_message_buf error_buf;
    DBUG_PRINT("error", ("Could not determine SID buffer size, "
                         "GetTokenInformation() failed with error %lX (%s)",
                         GetLastError(), get_last_error_message(error_buf)));
#endif
    return;
  }

  m_data = (TOKEN_USER *)new BYTE[req_size];
  success = GetTokenInformation(token, TokenUser, m_data, req_size, &req_size);

  if (!success || !is_valid()) {
    delete[] m_data;
    m_data = nullptr;
#ifndef NDEBUG
    if (!success) {
      Error_message_buf error_buf;
      DBUG_PRINT("error", ("Could not read SID from security token, "
                           "GetTokenInformation() failed with error %lX (%s)",
                           GetLastError(), get_last_error_message(error_buf)));
    }
#endif
  }
}

Sid::~Sid() {
  if (m_data) delete[] m_data;
#ifndef NDEBUG
  if (m_as_string) LocalFree(m_as_string);
#endif
}

/// Check if Sid object is valid.
bool Sid::is_valid(void) const {
  return m_data && m_data->User.Sid && IsValidSid(m_data->User.Sid);
}

#ifndef NDEBUG

/**
  Produces string representation of the SID.

  @return String representation of the SID or nullptr in case of errors.

  @note Memory allocated for the string is automatically freed in Sid's
  destructor.
*/

const char *Sid::as_string() {
  if (!m_data) return nullptr;

  if (!m_as_string) {
    const bool success = ConvertSidToStringSid(m_data->User.Sid, &m_as_string);

    if (!success) {
#ifndef NDEBUG
      Error_message_buf error_buf;
      DBUG_PRINT("error", ("Could not get textual representation of a SID, "
                           "ConvertSidToStringSid() failed with error %lX (%s)",
                           GetLastError(), get_last_error_message(error_buf)));
#endif
      m_as_string = nullptr;
      return nullptr;
    }
  }

  return m_as_string;
}

#endif

bool Sid::operator==(const Sid &other) const {
  if (!is_valid() || !other.is_valid()) return false;

  return EqualSid(m_data->User.Sid, other.m_data->User.Sid);
}

/** Generating User Principal Name *************************/

/**
  Call Windows API functions to get UPN of the current user and store it
  in internal buffer.
*/

UPN::UPN() : m_buf(nullptr) {
  wchar_t buf1[MAX_SERVICE_NAME_LENGTH];

  // First we try to use GetUserNameEx.

  m_len = sizeof(buf1) / sizeof(wchar_t);

  if (!GetUserNameExW(NameUserPrincipal, buf1, (PULONG)&m_len)) {
    if (GetLastError()) {
#ifndef NDEBUG
      Error_message_buf error_buf;
      DBUG_PRINT("note", ("When determining UPN"
                          ", GetUserNameEx() failed with error %lX (%s)",
                          GetLastError(), get_last_error_message(error_buf)));
#endif
      if (ERROR_MORE_DATA == GetLastError())
        ERROR_LOG(INFO, ("Buffer overrun when determining UPN:"
                         " need %ul characters but have %ul",
                         m_len, sizeof(buf1) / sizeof(WCHAR)));
    }

    m_len = 0;  // m_len == 0 indicates invalid UPN
    return;
  }

  /*
    UPN is stored in buf1 in wide-char format - convert it to utf8
    for sending over network.
  */

  m_buf = wchar_to_utf8(buf1, &m_len);

  if (!m_buf) ERROR_LOG(ERROR, ("Failed to convert UPN to utf8"));

  // Note: possible error would be indicated by the fact that m_buf is nullptr.
  return;
}

UPN::~UPN() {
  if (m_buf) free(m_buf);
}

/**
  Convert a wide-char string to utf8 representation.

  @param[in]     string   null-terminated wide-char string to be converted
  @param[in,out] len      length of the string to be converted or 0; on
                          return length (in bytes, excluding terminating
                          null character) of the converted string

  If len is 0 then the length of the string will be computed by this function.

  @return Pointer to a buffer containing utf8 representation or nullptr in
          case of error.

  @note The returned buffer must be freed with @c free() call.
*/

char *wchar_to_utf8(const wchar_t *string, size_t *len) {
  char *buf = nullptr;
  const size_t str_len = len && *len ? *len : wcslen(string);

  /*
    A conversion from utf8 to wchar_t will never take more than 3 bytes per
    character, so a buffer of length 3 * str_len should be sufficient.
    We check that assumption with an assertion later.
  */

  const size_t buf_len = 3 * str_len;

  buf = (char *)malloc(buf_len + 1);
  if (!buf) {
    DBUG_PRINT("error",
               ("Out of memory when converting string '%S' to utf8", string));
    return nullptr;
  }

  const int res =
      WideCharToMultiByte(CP_UTF8,            // convert to UTF-8
                          0,                  // conversion flags
                          string,             // input buffer
                          str_len,            // its length
                          buf, buf_len,       // output buffer and its size
                          nullptr, nullptr);  // default character (not used)

  if (res) {
    buf[res] = '\0';
    if (len) *len = res;
    return buf;
  }

  // res is 0 which indicates error

#ifndef NDEBUG
  Error_message_buf error_buf;
  DBUG_PRINT("error",
             ("Could not convert string '%S' to utf8"
              ", WideCharToMultiByte() failed with error %lX (%s)",
              string, GetLastError(), get_last_error_message(error_buf)));
#endif

  // Let's check our assumption about sufficient buffer size
  assert(ERROR_INSUFFICIENT_BUFFER != GetLastError());

  return nullptr;
}

/**
  Convert an utf8 string to a wide-char string.

  @param[in]     string   null-terminated utf8 string to be converted
  @param[in,out] len      length of the string to be converted or 0; on
                          return length (in chars) of the converted string

  If len is 0 then the length of the string will be computed by this function.

  @return Pointer to a buffer containing wide-char representation or nullptr in
          case of error.

  @note The returned buffer must be freed with @c free() call.
*/

wchar_t *utf8_to_wchar(const char *string, size_t *len) {
  size_t buf_len;

  /*
    Note: length (in bytes) of an utf8 string is always bigger than the
    number of characters in this string. Hence a buffer of size len will
    be sufficient. We add 1 for the terminating null character.
  */

  buf_len = len && *len ? *len : strlen(string);
  wchar_t *buf = (wchar_t *)malloc((buf_len + 1) * sizeof(wchar_t));

  if (!buf) {
    DBUG_PRINT("error", ("Out of memory when converting utf8 string '%s'"
                         " to wide-char representation",
                         string));
    return nullptr;
  }

  size_t res;
  res = MultiByteToWideChar(CP_UTF8,        // convert from UTF-8
                            0,              // conversion flags
                            string,         // input buffer
                            buf_len,        // its size
                            buf, buf_len);  // output buffer and its size
  if (res) {
    buf[res] = '\0';
    if (len) *len = res;
    return buf;
  }

  // error in MultiByteToWideChar()

#ifndef NDEBUG
  Error_message_buf error_buf;
  DBUG_PRINT("error", ("Could not convert UPN from UTF-8"
                       ", MultiByteToWideChar() failed with error %lX (%s)",
                       GetLastError(), get_last_error_message(error_buf)));
#endif

  // Let's check our assumption about sufficient buffer size
  assert(ERROR_INSUFFICIENT_BUFFER != GetLastError());

  return nullptr;
}

/** Error handling ****************************************************/

/**
  Returns error message corresponding to the last Windows error given
  by GetLastError().

  @note Error message is overwritten by next call to
  @c get_last_error_message().
*/

const char *get_last_error_message(Error_message_buf buf) {
  const int error = GetLastError();

  buf[0] = '\0';
  FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, error,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR)buf,
                sizeof(Error_message_buf), nullptr);

  return buf;
}
