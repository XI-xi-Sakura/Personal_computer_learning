/*
  Copyright (c) 2017, 2025, Oracle and/or its affiliates.

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

#ifndef MYSQL_HARNESS_LOGGER_LOGGER_INCLUDED
#define MYSQL_HARNESS_LOGGER_LOGGER_INCLUDED

#include "harness_export.h"
#include "mysql/harness/logging/logging.h"

#include <set>
#include <string>

namespace mysql_harness {

namespace logging {

class Registry;

/**
 * Logger class.
 *
 * The logger class handles the logging for one or more logging
 * handlers. Each logger class instance keeps state for logging for one
 * module or subsystem. You can add handlers to a logger which will
 * then be used for all logging to that subsystem.
 */
class HARNESS_EXPORT Logger {
 public:
  explicit Logger(Registry &registry, LogLevel level = kDefaultLogLevel);

  // such null object is useless, however we need the ability to create an
  // object and populate it later inside of guarded scope (e.g: std::lock_guard)
  Logger() : level_(LogLevel::kNotSet) {}

  void attach_handler(std::string name);
  void detach_handler(std::string name, bool handler_must_exist = true);
  void handle(const Record &record);

  void lazy_handle(LogLevel record_level,
                   std::function<Record()> record_creator) const;

  /**
   * check if the log-level will be handled.
   *
   * log-messages may be filtered on global and on handler level.
   *
   * in case it is not handled, there is no need call the prepare data for
   * the log-function.
   *
   * @returns if log-level will be handled or not
   * @retval true log-level (quite likely) will be handled
   * @retval false log-level will be ignored
   */
  bool is_handled(LogLevel level) const;
  const std::set<std::string> &get_handler_names() const { return handlers_; }

  void set_level(LogLevel level) { level_ = level; }
  LogLevel get_level() const { return level_; }
  void set_timestamp_precision(LogTimestampPrecision precision) {
    precision_ = precision;
  }
  LogTimestampPrecision get_timestamp_precision() const { return precision_; }

 private:
  LogLevel level_;
  LogTimestampPrecision precision_;
  std::set<std::string> handlers_;
  const Registry *registry_;  // owner backreference (we don't own Registry,
                              // Registry owns us)
};

class HARNESS_EXPORT DomainLogger {
 public:
  DomainLogger() = default;

  DomainLogger(std::string domain) : domain_(std::move(domain)) {}

  void debug(std::invocable auto producer) const {
    log(LogLevel::kDebug, std::move(producer));
  }
  void debug(const std::string &msg) const { log(LogLevel::kDebug, msg); }

  void info(std::invocable auto producer) const {
    log(LogLevel::kInfo, std::move(producer));
  }
  void info(const std::string &msg) const { log(LogLevel::kInfo, msg); }

  void warning(std::invocable auto producer) const {
    log(LogLevel::kWarning, std::move(producer));
  }
  void warning(const std::string &msg) const { log(LogLevel::kWarning, msg); }

  void system(std::invocable auto producer) const {
    log(LogLevel::kSystem, std::move(producer));
  }
  void system(const std::string &msg) const { log(LogLevel::kSystem, msg); }

  void note(std::invocable auto producer) const {
    log(LogLevel::kNote, std::move(producer));
  }
  void note(const std::string &msg) const { log(LogLevel::kNote, msg); }

  void error(std::invocable auto producer) const {
    log(LogLevel::kError, std::move(producer));
  }
  void error(const std::string &msg) const { log(LogLevel::kError, msg); }

  void log(LogLevel log_level, std::function<std::string()> producer) const;

  void log(LogLevel log_level, std::string msg) const;

 private:
  bool init_logger() const;

  mutable std::mutex logger_mtx_;
  mutable std::optional<mysql_harness::logging::Logger> logger_;

  std::string domain_{MYSQL_ROUTER_LOG_DOMAIN};
};

}  // namespace logging

}  // namespace mysql_harness

#endif /* MYSQL_HARNESS_LOGGER_LOGGER_INCLUDED */
