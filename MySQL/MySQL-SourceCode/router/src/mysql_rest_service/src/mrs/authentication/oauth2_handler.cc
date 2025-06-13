/*
  Copyright (c) 2022, 2025, Oracle and/or its affiliates.

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

#include "mrs/authentication/oauth2_handler.h"

#include <memory>
#include <stdexcept>
#include <utility>

#include <my_rapidjson_size_t.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/reader.h>

#include "helper/container/map.h"
#include "helper/http/url.h"
#include "helper/json/rapid_json_to_map.h"
#include "helper/json/text_to.h"
#include "helper/json/to_string.h"
#include "helper/string/hex.h"
#include "mrs/authentication/www_authentication_handler.h"
#include "mrs/http/error.h"
#include "mrs/http/utilities.h"
#include "mrs/rest/request_context.h"

#include "mysql/harness/logging/logging.h"
#include "mysql/harness/net_ts/io_context.h"
#include "mysql/harness/string_utils.h"
#include "mysqlrouter/http_client.h"

IMPORT_LOG_FUNCTIONS()

namespace mrs {
namespace authentication {

using GenericSessionData = Oauth2Handler::GenericSessionData;
using AuthApp = mrs::database::entry::AuthApp;

bool Oauth2Handler::redirects(RequestContext &) const {
  log_debug("Oauth2Handler::redirects");
  return true;
}

void Oauth2Handler::RequestHandlerJsonSimpleObject::before_send(Request *) {}

bool Oauth2Handler::RequestHandlerJsonSimpleObject::response(
    const std::vector<uint8_t> &value) {
  using HandlerMapOfSimpleValues =
      helper::json::RapidReaderHandlerToMapOfSimpleValues;

  auto result = helper::json::text_to_handler<HandlerMapOfSimpleValues>(value);

  for (auto &e : output_) {
    auto &key = e.first;
    auto &out_value = e.second;
    if (!helper::container::get_value_other(result, key, out_value)) {
      log_debug("Getting key:'%s' from container failed.", key);
      return false;
    }
  }

  return true;
}

std::set<UniversalId> Oauth2Handler::get_service_ids() const {
  return entry_.service_ids;
}

const AuthApp &Oauth2Handler::get_entry() const { return entry_; }

UniversalId Oauth2Handler::get_id() const { return entry_.id; }

bool Oauth2Handler::send_http_request(HttpMethodType method,
                                      const std::string &url,
                                      const std::string &body,
                                      RequestHandler *request_handler) {
  net::io_context io_ctx;
  TlsClientContext tls_ctx{TlsVerify::NONE};
  std::unique_ptr<::http::client::Client> http_client;
  HttpUri u{url};

  if (u.get_port() == 65535u) {
    if (u.get_scheme() == "http") {
      u.set_port(80);
    } else if (u.get_scheme() == "https") {
      u.set_port(443);
    }
  }

  http_client =
      std::make_unique<::http::client::Client>(io_ctx, std::move(tls_ctx));

  log_debug("Oauth2Handler::send_http_request url:%s", url.c_str());
  log_debug("Oauth2Handler::send_http_request body:%s", body.c_str());

  ::http::client::Request req{u, method};
  auto &output_headers = req.get_output_headers();
  auto &output_buffer = req.get_output_buffer();

  output_headers.add("Connection", "close");

  if (method == HttpMethod::Post && !body.empty()) {
    output_headers.add("Content-Type", "application/x-www-form-urlencoded");
  }

  if (!body.empty()) output_buffer.add(body.c_str(), body.length());
  if (request_handler) request_handler->before_send(&req);

  http_client->send_request(&req);

  if (0 != http_client->error_code()) {
    log_debug("Oauth2Handler::send_http_request http_client->error_code():%i",
              http_client->error_code());
    return false;
  }

  if (req.get_response_code() != HttpStatusCode::Ok) {
    log_debug("Oauth2Handler::send_http_request req->get_response_code():%i",
              req.get_response_code());
    return false;
  }

  auto &buffer = req.get_input_buffer();
  auto response_data = buffer.pop_front(buffer.length());

  if (request_handler) {
    if (!request_handler->response(response_data)) {
      std::string v{response_data.begin(), response_data.end()};
      log_debug(
          "Oauth2Handler::send_http_request request_handler returned error for "
          "payload:%s",
          v.c_str());
      return false;
    }
  }

  return true;
}

bool Oauth2Handler::http_acquire_access_token(GenericSessionData *data) {
  log_debug("oauth2: redirection=%s", data->redirection.c_str());
  if (!send_http_request(HttpMethod::Post, get_url_direct_auth(),
                         get_body_access_token_request(data),
                         get_request_handler_access_token(data).get())) {
    return false;
  }

  data->acquired_at = steady_clock::now();
  log_debug("acquired_access_token = %s", data->access_token.c_str());

  return true;
}

static std::string escape(const std::string &in) {
  std::string translate{" []{}\"&"};
  std::string path;
  for (auto c : in) {
    if (translate.find(c) != std::string::npos) {
      path += "%" + helper::string::hex(std::string{c});
    } else {
      path.append(1, c);
    }
  }

  return path;
}
void Oauth2Handler::new_session_start_login(RequestContext &ctxt,
                                            Session *session) {
  auto url = ctxt.get_http_url();

  log_debug("session.proto = %s", session->proto.c_str());
  log_debug("session.host = %s", session->host.c_str());

  std::string uri;
  if (!session->host.empty()) {
    uri = session->proto + "://" + session->host + url.get_path();
  } else {
    auto host_cstr = ctxt.get_in_headers().find_cstr(":authority");
    uri = session->proto + "://" + (host_cstr ? host_cstr : "localhost") +
          url.get_path();
  }
  auto data = new GenericSessionData();
  session->set_data(data);
  data->redirection_host = uri;

  std::string params;
  if (url.get_query().length()) {
    url.remove_query_parameter("onCompletionRedirect");
    params = "?" + url.get_query();
  }

  data->redirection = uri + escape(params);
  log_debug("Oauth2Handler new SessionData: redirection=%s",
            data->redirection.c_str());

  // TODO(lkotula): escape get_id(), cookies doesn't support all characters.
  // For now its ok because ID is a number. (Shouldn't be in review)

  // Redirect the web-browser to `get_url_location` URL.
  throw http::ErrorRedirect(get_url_location(data, &url));
}

bool Oauth2Handler::authorize(RequestContext &ctxt, const SessionPtr &session,
                              AuthUser *out_user) {
  const static std::string kCode{"code"};
  const static std::string kState{"state"};
  const static std::string kError{"error"};
  const static std::string kToken{"token"};
  auto session_data = session->get_data<GenericSessionData>();

  const auto &query_parameters = ctxt.get_http_url().get_query_elements();
  const bool token_in_parameters = 0 != query_parameters.count(kToken);
  const bool code_in_parameters = 0 != query_parameters.count(kCode);

  log_debug(
      "Oauth2Handler::authorize(id=%s, session_id=%s) "
      "=> "
      "%s",
      entry_.id.to_string().c_str(),
      session_data ? session_data->internal_session->get_session_id().c_str()
                   : "null",
      helper::json::to_string(query_parameters).c_str());

  if (nullptr == session_data) {
    if (!token_in_parameters && !code_in_parameters) {
      log_debug("SessionData doesn't exist in new-session");
      new_session_start_login(ctxt, session.get());
      return false;
    }

    session_data = new GenericSessionData();
    session->set_data(session_data);
    if (token_in_parameters)
      session_data->access_token = query_parameters.at(kToken);
    session->state = token_in_parameters ? Session::kTokenVerified
                                         : Session::kWaitingForCode;
  }

  if (session->state == Session::kWaitingForCode && token_in_parameters) {
    session->state = Session::kTokenVerified;
  }
  switch (session->state) {
    case Session::kUninitialized:
    case Session::kWaitingForCode: {
      if (query_parameters.count(kError)) {
        log_debug("Remote side returned and error.");
        // TODO(lkotula): forward the error ? (Shouldn't be in review)
        return false;
      }

      if (!query_parameters.count(kCode) || !query_parameters.count(kState)) {
        log_debug(
            "Remote side didn't return the code and state. Creating new "
            "session, and redirecting.");
        new_session_start_login(ctxt, session.get());
        return false;
      }

      session_data->auth_code = query_parameters.at(kCode);
      if (!http_acquire_access_token(session_data)) return false;

      session->state = Session::kTokenVerified;
    }
      [[fallthrough]];
    case Session::kTokenVerified:
      if (!http_verify_account(session.get(), session_data,
                               &ctxt.sql_session_cache))
        return false;
      session->state = Session::kUserVerified;

      return true;

    case Session::kGettingTokken:
      break;

    case Session::kUserVerified:
      *out_user = session->user;
      return true;
  }

  return false;
}

std::optional<std::string> Oauth2Handler::get_session_id_from_request_data(
    RequestContext &) {
  return {};
}

bool Oauth2Handler::http_verify_account(Session *session,
                                        GenericSessionData *data,
                                        SqlSessionCached *sql_session) {
  std::string url = get_url_validation(data);

  log_debug("verify_user: %s", url.c_str());
  log_debug("oauth2: redirection=%s", data->redirection.c_str());
  if (!send_http_request(
          HttpMethod::Get, url, {},
          get_request_handler_verify_account(session, data).get())) {
    return false;
  }

  log_debug("user_id: %s", session->user.vendor_user_id.c_str());
  session->user.app_id = entry_.id;

  return um_.user_get(&session->user, sql_session);
}

}  // namespace authentication
}  // namespace mrs
