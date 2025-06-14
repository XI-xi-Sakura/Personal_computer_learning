/*
  Copyright (c) 2015, 2025, Oracle and/or its affiliates.

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

#include "mysqlrouter/routing_plugin_export.h"

#include <mutex>
#include <stdexcept>
#include <vector>

#include "hostname_validator.h"
#include "mysql/harness/config_parser.h"
#include "mysql/harness/filesystem.h"
#include "mysql/harness/loader_config.h"
#include "mysql/harness/logging/logging.h"
#include "mysql/harness/net_ts/io_context.h"
#include "mysql/harness/tls_server_context.h"
#include "mysql/harness/utility/string.h"  // join, string_format
#include "mysql_routing.h"
#include "mysqlrouter/io_component.h"
#include "mysqlrouter/metadata_cache.h"
#include "mysqlrouter/routing_component.h"
#include "mysqlrouter/ssl_mode.h"
#include "mysqlrouter/supported_routing_options.h"
#include "plugin_config.h"
#include "routing_guidelines_adapter.h"
#include "scope_guard.h"
#include "sql_lexer.h"  // init_library()

using mysqlrouter::URI;
using mysqlrouter::URIError;
IMPORT_LOG_FUNCTIONS()

const mysql_harness::AppInfo *g_app_info;
static const std::string kSectionName = "routing";
std::mutex g_dest_tls_contexts_mtx;
std::vector<std::unique_ptr<DestinationTlsContext>> g_dest_tls_contexts;
std::mutex routing_guidelines_create_mtx;

static void validate_socket_info(const std::string &err_prefix,
                                 const mysql_harness::ConfigSection *section,
                                 const RoutingPluginConfig &config) {
  auto is_valid_port = [](int port) -> bool {
    return 0 < port && port < 65536;
  };

  bool have_named_sock = section->has("socket");
  bool have_bind_port = section->has("bind_port");
  bool have_bind_addr = section->has("bind_address");
  bool have_bind_addr_port = have_bind_addr && config.bind_address.port() != 0;

  // NOTE: Several test scenarios below are also covered by
  // RoutingPluginConfig() constructor's simple check.
  //       However, RoutingPluginConfig() doesn't check everything, and
  //       sometimes produces misleading error messages (when bind_address has
  //       no port number, and bind_port is not provided, is one such example;
  //       passing empty socket is another). Below tests should probably replace
  //       the simple check in that constructor, and get called from both
  //       places.

  // validate bind_port
  if (have_bind_port && !is_valid_port(config.bind_port)) {
    throw std::invalid_argument(err_prefix + "invalid bind_port '" +
                                std::to_string(config.bind_port) + "'");
  }

  // validate bind_address : IP
  if (have_bind_addr &&
      !mysql_harness::is_valid_domainname(config.bind_address.hostname())) {
    throw std::invalid_argument(err_prefix +
                                "invalid IP or name in bind_address '" +
                                config.bind_address.str() + "'");
  }

  // validate bind_address : TCP port
  if (have_bind_addr_port && !is_valid_port(config.bind_address.port())) {
    throw std::invalid_argument(err_prefix + "invalid bind_address '" +
                                config.bind_address.str() + "'");
  }

  // validate bind_address / bind_port ambiguity
  if (have_bind_addr_port && have_bind_port &&
      (config.bind_address.port() != config.bind_port)) {
    throw std::invalid_argument(
        err_prefix + "port in bind_address and bind_port are ambiguous '" +
        std::to_string(config.bind_address.port()) + "','" +
        std::to_string(config.bind_port) + "'");
  }

  // validate socket
  if (have_named_sock && !config.named_socket.is_set()) {
    throw std::invalid_argument(err_prefix + "invalid socket '" +
                                config.named_socket.str() + "'");
  }

  // check if we have enough information to open some listening socket (a Unix
  // socket/Windows named pipe or a TCP socket)
  if (!(have_named_sock || have_bind_port || have_bind_addr_port)) {
    if (have_bind_addr) {
      throw std::invalid_argument(err_prefix +
                                  "no socket, no bind_port, and TCP port in "
                                  "bind_address is not provided");
    } else {
      throw std::invalid_argument(
          err_prefix + "one of bind_port, bind_address, or socket is required");
    }
  }
}

// a non-static gateway function, meant to be called only by unit tests (to
// allow calling validate_socket_info(), which is static)
void validate_socket_info_test_proxy(
    const std::string &err_prefix, const mysql_harness::ConfigSection *section,
    const RoutingPluginConfig &config) {
  validate_socket_info(err_prefix, section, config);
}

// work-guards to keep the io-context alive
//
// - one per routing instance
// - it MUST be taken before the io-context 'starts'
// - it MUST be released after routing is finished using it (before routing
// 'start' ends)
std::mutex io_context_work_guard_mtx;
std::list<IoComponent::Workguard> io_context_work_guards;

static void init(mysql_harness::PluginFuncEnv *env) {
  const mysql_harness::AppInfo *info = get_app_info(env);

  SqlLexer::init_library();

  try {
    if (info->config != nullptr) {
      MySQLRoutingComponent::get_instance().init(*info->config);
      bool have_metadata_cache = false;
      bool need_metadata_cache = false;
      std::vector<mysql_harness::TcpDestination> bind_addresses;
      for (const mysql_harness::ConfigSection *section :
           info->config->sections()) {
        if (section->name == kSectionName) {
          io_context_work_guards.emplace_back(IoComponent::get_instance());

          const auto err_prefix = mysql_harness::utility::string_format(
              "in [%s%s%s]: ", section->name.c_str(),
              section->key.empty() ? "" : ":", section->key.c_str());
          // Check the configuration
          RoutingPluginConfig config(section);  // throws std::invalid_argument
          if (config.accept_connections) {
            validate_socket_info(err_prefix, section,
                                 config);  // throws std::invalid_argument

            // ensure that TCP port is unique
            if (config.bind_address.port() != 0) {
              const auto &config_addr = config.bind_address;

              // Check uniqueness of bind_address and port, using IP address
              auto found_addr =
                  std::find(bind_addresses.begin(), bind_addresses.end(),
                            config.bind_address);
              if (found_addr != bind_addresses.end()) {
                throw std::invalid_argument(
                    err_prefix +
                    "duplicate IP or name found in bind_address '" +
                    config.bind_address.str() + "'");
              }
              // Check ADDR_ANY binding on same port
              else if (config_addr.hostname() == "0.0.0.0" ||
                       config_addr.hostname() == "::") {
                found_addr = std::find_if(
                    bind_addresses.begin(), bind_addresses.end(),
                    [&config](const mysql_harness::TcpDestination &dest) {
                      return config.bind_address.port() == dest.port();
                    });
                if (found_addr != bind_addresses.end()) {
                  throw std::invalid_argument(
                      err_prefix +
                      "duplicate IP or name found in bind_address '" +
                      config.bind_address.str() + "'");
                }
              }
              bind_addresses.push_back(config.bind_address);
            }

            // We check if we need special plugins based on URI
            try {
              auto uri = URI(config.destinations, false);
              if (uri.scheme == "metadata-cache") {
                need_metadata_cache = true;
              }
            } catch (URIError &) {
              // No URI, no extra plugin needed
            }
          }
        } else if (section->name == "metadata_cache") {
          have_metadata_cache = true;
        }

        if (need_metadata_cache && !have_metadata_cache) {
          throw std::invalid_argument(
              "Routing needs Metadata Cache, but none "
              "was found in configuration.");
        }
      }
    }
    g_app_info = info;
  } catch (const std::invalid_argument &exc) {
    set_error(env, mysql_harness::kConfigInvalidArgument, "%s", exc.what());

    io_context_work_guards.clear();
  } catch (const std::exception &exc) {
    set_error(env, mysql_harness::kRuntimeError, "%s", exc.what());

    io_context_work_guards.clear();
  } catch (...) {
    set_error(env, mysql_harness::kUndefinedError, "Unexpected exception");

    io_context_work_guards.clear();
  }
}

static void ensure_readable_directory(const std::string &opt_name,
                                      const std::string &opt_value) {
  const auto p = mysql_harness::Path(opt_value);

  // if it is set, check it exists.
  if (!p.exists()) {
    throw std::runtime_error(opt_name + "=" + opt_value + " does not exist");
  }
  if (!p.is_directory()) {
    throw std::runtime_error(opt_name + "=" + opt_value +
                             " is not a directory");
  }
  if (!p.is_readable()) {
    throw std::runtime_error(opt_name + "=" + opt_value + " is not readable");
  }
}

static std::string get_default_ciphers() {
  return mysql_harness::join(TlsServerContext::default_ciphers(), ":");
}

static bool has_metadata_cache() {
  for (const mysql_harness::ConfigSection *section :
       g_app_info->config->sections()) {
    if (section->name == "metadata_cache") return true;
  }
  return false;
}

static void start(mysql_harness::PluginFuncEnv *env) {
  const mysql_harness::ConfigSection *section = get_config_section(env);

  std::string name;
  if (!section->key.empty()) {
    name = section->name + ":" + section->key;
  } else {
    name = section->name;
  }

  try {
    const RoutingPluginConfig config(section);

    if (config.router_require_enforce != 0) {
      if (config.source_ssl_ca_file.empty() &&
          config.source_ssl_ca_dir.empty()) {
        log_info(
            "[%s] %s", name.c_str(),
            "'router_require_enforce=1', but neither 'client_ssl_ca' nor "
            "'client_ssl_cadir' are set. MySQL account with ATTRIBUTE "
            "'{ \"router_require\": { \"x509\": true } }' will fail to auth.");
      }
    }

    // client side TlsContext.
    TlsServerContext source_tls_ctx{TlsVersion::TLS_1_2, TlsVersion::AUTO,
                                    config.client_ssl_session_cache_mode,
                                    config.client_ssl_session_cache_size,
                                    config.client_ssl_session_cache_timeout};

    if (config.source_ssl_mode != SslMode::kDisabled &&
        config.source_ssl_mode != SslMode::kPassthrough) {
      if (config.source_ssl_cert.empty()) {
        throw std::invalid_argument(
            "client_ssl_cert must be set, if client_ssl_mode is enabled.");
      }
      if (config.source_ssl_key.empty()) {
        throw std::invalid_argument(
            "client_ssl_key must be set, if client_ssl_mode is enabled.");
      }

      const auto res = source_tls_ctx.load_key_and_cert(config.source_ssl_key,
                                                        config.source_ssl_cert);
      if (!res) {
        throw std::system_error(
            res.error(), "loading client_ssl_cert '" + config.source_ssl_cert +
                             "' and client_ssl_key '" + config.source_ssl_key +
                             "' failed");
      }

      if (!config.source_ssl_curves.empty()) {
        if (source_tls_ctx.has_set_curves_list()) {
          const auto res = source_tls_ctx.curves_list(config.source_ssl_curves);
          if (!res) {
            throw std::system_error(res.error(), "setting client_ssl_curves=" +
                                                     config.source_ssl_curves +
                                                     " failed");
          }

        } else {
          throw std::invalid_argument(
              "setting client_ssl_curves is not supported by the ssl library, "
              "it should stay unset");
        }
      }

      {
        const auto res =
            source_tls_ctx.init_tmp_dh(config.source_ssl_dh_params);
        if (!res) {
          throw std::system_error(res.error(),
                                  "setting client_ssl_dh_params failed");
        }
      }

      {
        const std::string ssl_cipher = config.source_ssl_cipher.empty()
                                           ? get_default_ciphers()
                                           : config.source_ssl_cipher;

        const auto res = source_tls_ctx.cipher_list(ssl_cipher);
        if (!res) {
          throw std::system_error(res.error(), "setting client_ssl_cipher=" +
                                                   ssl_cipher + " failed");
        }
      }

      if (!config.source_ssl_ca_file.empty() ||
          !config.source_ssl_ca_dir.empty()) {
        if (!config.source_ssl_ca_dir.empty()) {
          // throws on error
          ensure_readable_directory("client_ssl_capath",
                                    config.source_ssl_ca_dir);
        }
        {
          const auto res = source_tls_ctx.ssl_ca(config.source_ssl_ca_file,
                                                 config.source_ssl_ca_dir);
          if (!res) {
            throw std::system_error(
                res.error(),
                "setting client_ssl_ca=" + config.source_ssl_ca_file +
                    " and client_ssl_capath=" + config.source_ssl_ca_dir +
                    " failed");
          }
        }

        if (!config.source_ssl_crl_file.empty() ||
            !config.source_ssl_crl_dir.empty()) {
          const auto res = source_tls_ctx.crl(config.source_ssl_crl_file,
                                              config.source_ssl_crl_dir);
          if (!res) {
            throw std::system_error(
                res.error(),
                "setting client_ssl_crl=" + config.source_ssl_crl_file +
                    " and client_ssl_crlpath=" + config.source_ssl_crl_dir +
                    " failed");
          }
        }

        source_tls_ctx.verify(TlsVerify::PEER, TlsVerifyOpts::kClientOnce);
      }

      if (!config.source_ssl_crl_file.empty() ||
          !config.source_ssl_crl_dir.empty()) {
        if (!config.source_ssl_crl_dir.empty()) {
          // throws on error
          ensure_readable_directory("client_ssl_crlpath",
                                    config.source_ssl_crl_dir);
        }
        const auto res = source_tls_ctx.crl(config.source_ssl_crl_file,
                                            config.source_ssl_crl_dir);
        if (!res) {
          throw std::system_error(
              res.error(),
              "setting client_ssl_crl=" + config.source_ssl_crl_file +
                  " and client_ssl_crlpath=" + config.source_ssl_crl_dir +
                  " failed");
        }
      }
    }

    auto dest_tls_context = std::make_unique<DestinationTlsContext>(
        config.server_ssl_session_cache_mode,
        config.server_ssl_session_cache_size,
        config.server_ssl_session_cache_timeout);
    if (config.dest_ssl_mode != SslMode::kDisabled) {
      // validate the config-values one time
      TlsClientContext precheck_dest_tls_ctx;

      {
        const std::string dest_ssl_cipher = config.dest_ssl_cipher.empty()
                                                ? get_default_ciphers()
                                                : config.dest_ssl_cipher;
        const auto res = precheck_dest_tls_ctx.cipher_list(dest_ssl_cipher);
        if (!res) {
          throw std::system_error(res.error(), "setting server_ssl_cipher=" +
                                                   dest_ssl_cipher + " failed");
        }
        dest_tls_context->ciphers(dest_ssl_cipher);
      }
      if (!config.dest_ssl_curves.empty()) {
        const auto res =
            precheck_dest_tls_ctx.curves_list(config.dest_ssl_curves);
        if (!res) {
          if (res.error() == std::errc::function_not_supported) {
            throw std::runtime_error(
                "setting server_ssl_curves=" + config.dest_ssl_curves +
                " is not supported by the ssl library, it should stay unset");
          } else {
            throw std::system_error(res.error(), "setting server_ssl_curves=" +
                                                     config.dest_ssl_curves +
                                                     " failed");
          }
        }
        dest_tls_context->curves(config.dest_ssl_curves);
      }

      if (!config.dest_ssl_ca_file.empty() || !config.dest_ssl_ca_dir.empty()) {
        if (!config.dest_ssl_ca_dir.empty()) {
          // throws on error
          ensure_readable_directory("server_ssl_capath",
                                    config.dest_ssl_ca_dir);
        }
        const auto res = precheck_dest_tls_ctx.ssl_ca(config.dest_ssl_ca_file,
                                                      config.dest_ssl_ca_dir);
        if (!res) {
          throw std::system_error(
              res.error(), "setting server_ssl_ca=" + config.dest_ssl_ca_file +
                               " and server_ssl_capath=" +
                               config.dest_ssl_ca_dir + " failed");
        }
        dest_tls_context->ca_file(config.dest_ssl_ca_file);
        dest_tls_context->ca_path(config.dest_ssl_ca_dir);
      }

      if (!config.dest_ssl_crl_file.empty() ||
          !config.dest_ssl_crl_dir.empty()) {
        if (!config.dest_ssl_crl_dir.empty()) {
          // throws on error
          ensure_readable_directory("server_ssl_crlpath",
                                    config.dest_ssl_crl_dir);
        }
        const auto res = precheck_dest_tls_ctx.crl(config.dest_ssl_crl_file,
                                                   config.dest_ssl_crl_dir);
        if (!res) {
          throw std::system_error(
              res.error(),
              "setting server_ssl_crl=" + config.dest_ssl_crl_file +
                  " and server_ssl_crlpath=" + config.dest_ssl_crl_dir +
                  " failed");
        }

        dest_tls_context->crl_file(config.dest_ssl_crl_file);
        dest_tls_context->crl_path(config.dest_ssl_crl_dir);
      }

      dest_tls_context->verify(config.dest_ssl_verify);

      if (!config.dest_ssl_key.empty() && !config.dest_ssl_cert.empty()) {
        dest_tls_context->client_key_and_cert_file(config.dest_ssl_key,
                                                   config.dest_ssl_cert);
      } else if (config.dest_ssl_cert.empty() && config.dest_ssl_key.empty()) {
        // ok
      } else {
        throw std::system_error(
            make_error_code(std::errc::invalid_argument),
            "setting server_ssl_key=" + config.dest_ssl_key +
                " and server_ssl_cert=" + config.dest_ssl_cert + " failed");
      }
    }

    if (config.connection_sharing == 1) {
      if (config.source_ssl_mode == SslMode::kPassthrough) {
        log_warning(
            "[%s].connection_sharing=1 has been ignored, as "
            "client_ssl_mode=PASSTHROUGH.",
            name.c_str());
      } else if (config.source_ssl_mode == SslMode::kPreferred &&
                 config.dest_ssl_mode == SslMode::kAsClient) {
        log_warning(
            "[%s].connection_sharing=1 has been ignored, as "
            "client_ssl_mode=PREFERRED and server_ssl_mode=AS_CLIENT.",
            name.c_str());
      }

      if (config.protocol == Protocol::Type::kXProtocol) {
        log_warning("[%s].connection_sharing=1 has been ignored, as protocol=x",
                    name.c_str());
      }
    }

    net::io_context &io_ctx = IoComponent::get_instance().io_context();
    {
      auto &routing_component = MySQLRoutingComponent::get_instance();
      std::lock_guard lock(routing_guidelines_create_mtx);
      if (!routing_component.routing_guidelines_initialized()) {
        const auto &routing_guidelines_document =
            create_routing_guidelines_document(g_app_info->config->sections(),
                                               io_ctx);
        if (routing_guidelines_document) {
          log_debug("Initial routing guidelines: \n%s",
                    routing_guidelines_document.value().c_str());
          routing_component.set_routing_guidelines(
              routing_guidelines_document.value());
        } else if (routing_guidelines_document.error() ==
                   std::errc::not_supported) {
          log_debug(
              "Skip generating initial routing guidelines, only static routes "
              "are configured");
        } else {
          throw std::runtime_error(
              "Unable to create routing guidelines from configuration file: " +
              routing_guidelines_document.error().message());
        }
      }
    }

    auto routing_guidelines =
        MySQLRoutingComponent::get_instance().get_routing_guidelines();
    auto r = std::make_shared<MySQLRouting>(
        config, io_ctx, std::move(routing_guidelines), name,
        config.source_ssl_mode != SslMode::kDisabled ? &source_tls_ctx
                                                     : nullptr,
        config.dest_ssl_mode != SslMode::kDisabled ? dest_tls_context.get()
                                                   : nullptr);

    r->set_destinations(config.destinations);

    MySQLRoutingComponent::get_instance().register_route(section->key, r);

    auto *mdc = metadata_cache::MetadataCacheAPI::instance();
    using namespace std::chrono_literals;
    const bool has_md = has_metadata_cache();
    while (has_md && !mdc->is_initialized() && (!env || is_running(env))) {
      std::this_thread::sleep_for(1ms);
    }

    if (mdc->is_initialized() && !r->is_standalone()) {
      mdc->add_routing_guidelines_update_callbacks(
          [r](const std::string &routing_guidelines_document) {
            return r->update_routing_guidelines(routing_guidelines_document);
          },
          [&r](const auto &affected_routes) {
            r->on_routing_guidelines_update(affected_routes);
          });
      mdc->add_router_info_update_callback([&r](const auto &router_info) {
        r->on_router_info_update(router_info);
      });
    }

    Scope_guard guard{[r, mdc, section_key = section->key]() {
      MySQLRoutingComponent::get_instance().erase(section_key);
      if (mdc->is_initialized() && !r->is_standalone()) {
        mdc->clear_routing_guidelines_update_callbacks();
        mdc->clear_router_info_update_callback();
      }
    }};

    {
      std::lock_guard<std::mutex> lock{g_dest_tls_contexts_mtx};
      g_dest_tls_contexts.emplace_back(std::move(dest_tls_context));
    }

    r->run(env);
  } catch (const std::invalid_argument &exc) {
    set_error(env, mysql_harness::kConfigInvalidArgument, "%s", exc.what());
  } catch (const std::runtime_error &exc) {
    set_error(env, mysql_harness::kRuntimeError, "%s: %s", name.c_str(),
              exc.what());
  } catch (...) {
    set_error(env, mysql_harness::kUndefinedError, "Unexpected exception");
  }

  {
    // as the r->run() shuts down all in parallel, synchronize the access to
    std::lock_guard<std::mutex> lk(io_context_work_guard_mtx);

    io_context_work_guards.erase(io_context_work_guards.begin());
  }
}

static void deinit(mysql_harness::PluginFuncEnv * /* env */) {
  MySQLRoutingComponent::get_instance().deinit();
  // release all that may still be taken
  io_context_work_guards.clear();

  std::lock_guard<std::mutex> lock{g_dest_tls_contexts_mtx};
  g_dest_tls_contexts.clear();
}

static const std::array required{
    "logger",           //
    "router_protobuf",  //
    "router_openssl",   //
    "io",
    "connection_pool",
    "destination_status",
};

static void expose_configuration(mysql_harness::PluginFuncEnv *env,
                                 const char *key, bool initial) {
  const mysql_harness::AppInfo *info = get_app_info(env);

  if (!info->config) return;

  for (const mysql_harness::ConfigSection *section : info->config->sections()) {
    if (section->name != kSectionName || section->key != key) {
      continue;
    }

    RoutingPluginConfig config(section);
    config.expose_configuration(key, info->config->get_default_section(),
                                initial);
  }
}

mysql_harness::Plugin ROUTING_PLUGIN_EXPORT harness_plugin_routing = {
    mysql_harness::PLUGIN_ABI_VERSION,       // abi-version
    mysql_harness::ARCHITECTURE_DESCRIPTOR,  // arch
    "Routing MySQL connections between MySQL clients/connectors and "
    "servers",  // name
    VERSION_NUMBER(0, 0, 1),
    // requires
    required.size(),
    required.data(),
    // conflicts
    0,
    nullptr,
    init,     // init
    deinit,   // deinit
    start,    // start
    nullptr,  // stop
    true,     // declares_readiness
    routing_supported_options.size(),
    routing_supported_options.data(),
    expose_configuration,
};
