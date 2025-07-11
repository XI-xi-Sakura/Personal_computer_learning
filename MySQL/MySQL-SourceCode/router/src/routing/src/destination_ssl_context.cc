/*
  Copyright (c) 2020, 2025, Oracle and/or its affiliates.

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

#include "destination_ssl_context.h"

#include <memory>  // make_unique

void DestinationTlsContext::verify(SslVerify ssl_verify) {
  std::lock_guard<std::mutex> lk(mtx_);

  ssl_verify_ = ssl_verify;
}

void DestinationTlsContext::ca_file(const std::string &file) {
  std::lock_guard<std::mutex> lk(mtx_);

  ca_file_ = file;
}

void DestinationTlsContext::ca_path(const std::string &path) {
  std::lock_guard<std::mutex> lk(mtx_);

  ca_path_ = path;
}

void DestinationTlsContext::crl_file(const std::string &file) {
  std::lock_guard<std::mutex> lk(mtx_);

  crl_file_ = file;
}

void DestinationTlsContext::crl_path(const std::string &path) {
  std::lock_guard<std::mutex> lk(mtx_);

  crl_path_ = path;
}

void DestinationTlsContext::curves(const std::string &curves) {
  std::lock_guard<std::mutex> lk(mtx_);

  curves_ = curves;
}

void DestinationTlsContext::ciphers(const std::string &ciphers) {
  std::lock_guard<std::mutex> lk(mtx_);

  ciphers_ = ciphers;
}

void DestinationTlsContext::client_key_and_cert_file(std::string key,
                                                     std::string cert) {
  std::lock_guard<std::mutex> lk(mtx_);

  key_file_ = std::move(key);
  cert_file_ = std::move(cert);
}

SslVerify DestinationTlsContext::get_verify() { return ssl_verify_; }
const std::string &DestinationTlsContext::get_ca_file() { return ca_file_; }

const std::string &DestinationTlsContext::get_ca_path() { return ca_path_; }

const std::string &DestinationTlsContext::get_crl_file() { return crl_file_; }

const std::string &DestinationTlsContext::get_crl_path() { return crl_path_; }

const std::string &DestinationTlsContext::get_curves() { return curves_; }

const std::string &DestinationTlsContext::get_ciphers() { return ciphers_; }

TlsClientContext *DestinationTlsContext::get(const std::string &dest_id,
                                             const std::string &hostname) {
  std::lock_guard<std::mutex> lk(mtx_);

  const auto it = tls_contexts_.find(dest_id);
  if (it == tls_contexts_.end()) {
    // not found
    auto res = tls_contexts_.emplace(
        dest_id, std::make_unique<TlsClientContext>(
                     TlsVerify::PEER, session_cache_mode_,
                     ssl_session_cache_size_, ssl_session_cache_timeout_));
    auto *tls_ctx = res.first->second.get();

    if (!ciphers_.empty()) tls_ctx->cipher_list(ciphers_);
    if (!curves_.empty()) tls_ctx->curves_list(curves_);

    if (!key_file_.empty() && !cert_file_.empty()) {
      tls_ctx->load_key_and_cert(key_file_, cert_file_);
    }

    switch (ssl_verify_) {
      case SslVerify::kDisabled:
        tls_ctx->verify(TlsVerify::NONE);
        break;
      case SslVerify::kVerifyIdentity:
        tls_ctx->verify_hostname(hostname);
        [[fallthrough]];
      case SslVerify::kVerifyCa:
        tls_ctx->ssl_ca(ca_file_, ca_path_);
        tls_ctx->crl(crl_file_, crl_path_);
        tls_ctx->verify(TlsVerify::PEER);
        break;
    }

    return tls_ctx;
  } else {
    return it->second.get();
  }
}
