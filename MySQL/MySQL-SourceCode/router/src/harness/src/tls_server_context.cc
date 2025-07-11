/*
  Copyright (c) 2018, 2025, Oracle and/or its affiliates.

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
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "mysql/harness/tls_server_context.h"

#include <memory>
#include <string>
#include <vector>

#include <openssl/dh.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/safestack.h>
#include <openssl/ssl.h>

#include "mysql/harness/stdx/expected.h"
#include "mysql/harness/tls_error.h"
#include "mysql/harness/utility/string.h"

#include "openssl_version.h"

#if OPENSSL_VERSION_NUMBER < ROUTER_OPENSSL_VERSION(1, 1, 0)
#define DH_bits(dh) BN_num_bits(dh->p)
#endif

#if OPENSSL_VERSION_NUMBER >= ROUTER_OPENSSL_VERSION(3, 0, 0)
#include <openssl/core_names.h>  // OSSL_PKEY_...
#include <openssl/decoder.h>     // OSSL_DECODER...
#endif

#include <dh_ecdh_config.h>

constexpr int kMinDhKeySize{1024};
#if OPENSSL_VERSION_NUMBER >= ROUTER_OPENSSL_VERSION(1, 1, 0)
constexpr int kMaxSecurityLevel{5};
#endif

namespace {
const SSL_METHOD *server_method =
#if OPENSSL_VERSION_NUMBER >= ROUTER_OPENSSL_VERSION(1, 1, 0)
    TLS_server_method()
#else
    SSLv23_server_method()
#endif
    ;

template <class T>
struct OsslDeleter;

template <class T>
using OsslUniquePtr = std::unique_ptr<T, OsslDeleter<T>>;

#if OPENSSL_VERSION_NUMBER >= ROUTER_OPENSSL_VERSION(3, 0, 0)
template <>
struct OsslDeleter<OSSL_DECODER_CTX> {
  void operator()(OSSL_DECODER_CTX *ctx) { OSSL_DECODER_CTX_free(ctx); }
};
#endif

template <>
struct OsslDeleter<EVP_PKEY_CTX> {
  void operator()(EVP_PKEY_CTX *ctx) { EVP_PKEY_CTX_free(ctx); }
};

template <>
struct OsslDeleter<EVP_PKEY> {
  void operator()(EVP_PKEY *pkey) { EVP_PKEY_free(pkey); }
};

template <>
struct OsslDeleter<BIO> {
  void operator()(BIO *bio) { BIO_free(bio); }
};

// DH_free is deprecated in 3.0.0 and later.
#if OPENSSL_VERSION_NUMBER < ROUTER_OPENSSL_VERSION(3, 0, 0)
template <>
struct OsslDeleter<EC_KEY> {
  void operator()(EC_KEY *key) { EC_KEY_free(key); }
};

template <>
struct OsslDeleter<DH> {
  void operator()(DH *dh) { DH_free(dh); }
};

template <>
struct OsslDeleter<RSA> {
  void operator()(RSA *rsa) { RSA_free(rsa); }
};
#endif

/**
 * set DH params from filename to a SSL_CTX.
 *
 * ensures that the DH param has at least kMinDhKeySize bits.
 *
 * @returns nothing on success, std::error_code on error.
 */
stdx::expected<void, std::error_code> set_dh_params_from_filename(
    SSL_CTX *ssl_ctx, const std::string &dh_params) {
  OsslUniquePtr<BIO> pem_bio_storage(BIO_new_file(dh_params.c_str(), "rb"));
  if (!pem_bio_storage) return stdx::unexpected(make_tls_error());

  auto pem_bio = pem_bio_storage.get();

#if OPENSSL_VERSION_NUMBER >= ROUTER_OPENSSL_VERSION(3, 0, 0)
  EVP_PKEY *dhpkey{};  // gets set when OSSL_DECODER_from_bio() succeeds.
  OsslUniquePtr<OSSL_DECODER_CTX> decoder_ctx_storage(
      OSSL_DECODER_CTX_new_for_pkey(
          &dhpkey, "PEM", nullptr, "DH", OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS,
          nullptr /* libctx */, nullptr /* propquery */));
  if (!decoder_ctx_storage) return stdx::unexpected(make_tls_error());

  auto *decoder_ctx = decoder_ctx_storage.get();

  if (1 != OSSL_DECODER_from_bio(decoder_ctx, pem_bio)) {
    if (0 == ERR_peek_last_error()) {
      // make sure there is at least one error on the stack.
      //
      // OSSL_DECODER_from_bio() should set ERR after it failed ... but it
      // doesn't always does that, like when the PEM fail contains only an SSL
      // cert.
      //
      // It should report something like
      //
      // DECODER::unsupported: No supported data to decode. Input type: PEM
      ERR_raise(ERR_LIB_OSSL_DECODER, ERR_R_UNSUPPORTED);
    }
    return stdx::unexpected(make_tls_error());
  }

  OsslUniquePtr<EVP_PKEY> dhpkey_storage(
      dhpkey);  // take ownership for a while.

  OsslUniquePtr<EVP_PKEY_CTX> evp_ctx_storage(
      EVP_PKEY_CTX_new(dhpkey, nullptr));

  if (1 != EVP_PKEY_param_check(evp_ctx_storage.get())) {
    return stdx::unexpected(make_tls_error());
  }

  int dh_bits;
  if (!EVP_PKEY_get_int_param(dhpkey, OSSL_PKEY_PARAM_BITS, &dh_bits)) {
    // ^^ doesn't set an error in the openssl error-queue.
    //
    // on the other side it should never fail as the "bits" should be always
    // present.
    return stdx::unexpected(make_error_code(std::errc::invalid_argument));
  }
#else
  OsslUniquePtr<DH> dh_storage(
      PEM_read_bio_DHparams(pem_bio, nullptr, nullptr, nullptr));
  if (!dh_storage) return stdx::unexpected(make_tls_error());

  DH *dh = dh_storage.get();

  int codes = 0;
  if (1 != DH_check(dh, &codes)) return stdx::unexpected(make_tls_error());

  if (codes != 0) {
    throw std::runtime_error("check of DH params failed: ");
  }

  auto dh_bits = DH_bits(dh);
#endif

  if (dh_bits < kMinDhKeySize) {
    throw std::runtime_error(
        "key size of DH param " + dh_params + " too small. Expected " +
        std::to_string(kMinDhKeySize) + ", got " + std::to_string(dh_bits));
  }

#if OPENSSL_VERSION_NUMBER >= ROUTER_OPENSSL_VERSION(3, 0, 0)
  // on success, ownership if pkey is moved to the ssl-ctx
  if (1 != SSL_CTX_set0_tmp_dh_pkey(ssl_ctx, dhpkey)) {
    return stdx::unexpected(make_tls_error());
  }
  (void)dhpkey_storage.release();
#else
  if (1 != SSL_CTX_set_tmp_dh(ssl_ctx, dh)) {
    return stdx::unexpected(make_tls_error());
  }
#endif

  return {};
}

/**
 * set auto DH params at SSL_CTX.
 */
stdx::expected<void, std::error_code> set_auto_dh_params(SSL_CTX *ssl_ctx) {
  if (false != set_dh(ssl_ctx)) {
    return stdx::unexpected(make_tls_error());
  }

  return {};
}
}  // namespace

TlsServerContext::TlsServerContext(TlsVersion min_ver, TlsVersion max_ver,
                                   bool session_cache_mode,
                                   size_t session_cache_size,
                                   unsigned int session_cache_timeout)
    : TlsContext(server_method) {
  version_range(min_ver, max_ver);
  (void)set_ecdh(ssl_ctx_.get());
  SSL_CTX_set_options(ssl_ctx_.get(), SSL_OP_NO_COMPRESSION);
  cipher_list("ALL");  // ALL - unacceptable ciphers

  const auto cache_mode =
      session_cache_mode ? SSL_SESS_CACHE_SERVER : SSL_SESS_CACHE_OFF;
  SSL_CTX_set_session_cache_mode(ssl_ctx_.get(), cache_mode);
  if (cache_mode == SSL_SESS_CACHE_OFF) {
    SSL_CTX_set_options(ssl_ctx_.get(), SSL_OP_NO_TICKET);
  } else {
    SSL_CTX_sess_set_cache_size(ssl_ctx_.get(), session_cache_size);
    SSL_CTX_set_timeout(ssl_ctx_.get(), session_cache_timeout);
  }
}

// load DH params
stdx::expected<void, std::error_code> TlsServerContext::init_tmp_dh(
    const std::string &dh_params) {
  if (!dh_params.empty()) {
    auto set_res = set_dh_params_from_filename(ssl_ctx_.get(), dh_params);
    if (!set_res) return stdx::unexpected(set_res.error());
  } else {
    auto set_res = set_auto_dh_params(ssl_ctx_.get());
    if (!set_res) return stdx::unexpected(set_res.error());
  }

  // ensure DH keys are only used once
  SSL_CTX_set_options(ssl_ctx_.get(),
                      SSL_OP_SINGLE_DH_USE | SSL_OP_SINGLE_ECDH_USE);

  return {};
}

stdx::expected<void, std::error_code> TlsServerContext::verify(
    TlsVerify verify, stdx::flags<TlsVerifyOpts> tls_opts) {
  int mode = 0;
  switch (verify) {
    case TlsVerify::NONE:
      mode = SSL_VERIFY_NONE;

      if (tls_opts) {
        // tls_opts MUST be zero if verify is NONE
        return stdx::unexpected(make_error_code(std::errc::invalid_argument));
      }
      break;
    case TlsVerify::PEER:
      mode = SSL_VERIFY_PEER;
      break;
  }
  if (tls_opts & TlsVerifyOpts::kClientOnce) {
    mode |= SSL_VERIFY_CLIENT_ONCE;
  }
  if (tls_opts & TlsVerifyOpts::kFailIfNoPeerCert) {
    mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
  }
  SSL_CTX_set_verify(ssl_ctx_.get(), mode, nullptr);

  return {};
}

stdx::expected<void, std::error_code> TlsServerContext::cipher_list(
    const std::string &ciphers) {
  // append the "unacceptable_cipher_spec" to ensure to NEVER allow weak ciphers

  std::string ci(ciphers);
  if (!ci.empty()) ci += ":";

  ci += mysql_harness::join(unacceptable_cipher_spec, ":");

  // load the cipher-list
  if (1 != SSL_CTX_set_cipher_list(ssl_ctx_.get(), ci.c_str())) {
    return stdx::unexpected(make_tls_error());
  }

  return {};
}

std::vector<std::string> TlsServerContext::default_ciphers() {
  // as TLSv1.2 is the minimum version, only TLSv1.2+ ciphers are set by
  // default

  return {
      // Mandatory Ciphers (P1)
      //
      // TLSv1.2 with PFS, SHA2, AES with GCM
      "ECDHE-ECDSA-AES128-GCM-SHA256",  //
      "ECDHE-ECDSA-AES256-GCM-SHA384",  //
      "ECDHE-RSA-AES128-GCM-SHA256",    //

      // Approved Ciphers (A1)
      //
      // TLSv1.2+ with PFS, SHA2, AES with GCM or other AEAD algo's.

      // TLSv1.3
      "TLS_AES_128_GCM_SHA256",
      "TLS_AES_256_GCM_SHA384",
      "TLS_CHACHA20_POLY1305_SHA256",
      "TLS_AES_128_CCM_SHA256",

      // TLSv1.2
      "ECDHE-RSA-AES256-GCM-SHA384",
      "ECDHE-ECDSA-CHACHA20-POLY1305",
      "ECDHE-RSA-CHACHA20-POLY1305",
      "ECDHE-ECDSA-AES256-CCM",
      "ECDHE-ECDSA-AES128-CCM",
      "DHE-RSA-AES128-GCM-SHA256",
      "DHE-RSA-AES256-GCM-SHA384",
      "DHE-RSA-AES128-CCM",
      "DHE-RSA-AES256-CCM",
      "DHE-RSA-CHACHA20-POLY1305",
  };
}

int TlsServerContext::security_level() const {
#if OPENSSL_VERSION_NUMBER >= ROUTER_OPENSSL_VERSION(1, 1, 0)
  int sec_level = SSL_CTX_get_security_level(ssl_ctx_.get());

  assert(sec_level <= kMaxSecurityLevel);

  /* current range for security level is [1,5] */
  if (sec_level > kMaxSecurityLevel)
    sec_level = kMaxSecurityLevel;
  else if (sec_level <= 1)
    sec_level = 2;

  return sec_level;
#else
  return 2;
#endif
}

stdx::expected<void, std::error_code> TlsServerContext::session_id_context(
    const unsigned char *sid_ctx, unsigned int sid_ctx_len) {
  if (0 ==
      SSL_CTX_set_session_id_context(ssl_ctx_.get(), sid_ctx, sid_ctx_len)) {
    return stdx::unexpected(make_tls_error());
  }

  return {};
}
