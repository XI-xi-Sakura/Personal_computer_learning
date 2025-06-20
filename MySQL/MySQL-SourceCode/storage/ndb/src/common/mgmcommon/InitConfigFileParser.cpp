/*
   Copyright (c) 2003, 2025, Oracle and/or its affiliates.

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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include <ndb_global.h>
#include <ndb_version.h>
#include "util/require.h"

#include <NdbOut.hpp>
#include <cstdlib>
#include <util/SparseBitmask.hpp>
#include "../common/util/parse_mask.hpp"
#include "Config.hpp"
#include "ConfigInfo.hpp"
#include "EventLogger.hpp"
#include "InitConfigFileParser.hpp"
#include "m_string.h"
#include "my_alloc.h"

const int MAX_LINE_LENGTH = 1024;  // Max length of line of text in config file
static void trim(char *);

//****************************************************************************
//  Ctor / Dtor
//****************************************************************************
InitConfigFileParser::InitConfigFileParser() { m_info = new ConfigInfo(); }

InitConfigFileParser::~InitConfigFileParser() { delete m_info; }

//****************************************************************************
//  Read Config File
//****************************************************************************
InitConfigFileParser::Context::Context()
    : m_info(nullptr), m_userProperties(true), m_configValues() {
  m_config = new Properties(true);
  m_defaults = new Properties(true);
}

InitConfigFileParser::Context::~Context() {
  delete m_config;

  delete m_defaults;
}

Config *InitConfigFileParser::parseConfig(const char *filename) {
  FILE *file = fopen(filename, "r");
  if (file == nullptr) {
    g_eventLogger->error("Error opening '%s', error: %d, %s", filename, errno,
                         strerror(errno));
    return nullptr;
  }

  Config *ret = parseConfig(file);
  fclose(file);
  return ret;
}

Config *InitConfigFileParser::parseConfig(FILE *file) {
  char line[MAX_LINE_LENGTH];

  Context ctx;
  ctx.m_lineno = 0;
  ctx.m_currentSection = nullptr;

  /*************
   * Open file *
   *************/
  if (file == nullptr) {
    return nullptr;
  }

  /***********************
   * While lines to read *
   ***********************/
  while (fgets(line, MAX_LINE_LENGTH, file)) {
    ctx.m_lineno++;

    trim(line);

    if (isEmptyLine(line))  // Skip if line is empty or comment
      continue;

    // End with NULL instead of newline
    if (line[strlen(line) - 1] == '\n') line[strlen(line) - 1] = '\0';

    /********************************
     * 1. Parse new default section *
     ********************************/
    if (char *section = parseDefaultSectionHeader(line)) {
      if (!storeSection(ctx)) {
        free(section);
        ctx.reportError(
            "Could not store previous default section "
            "of configuration file.");
        delete ctx.m_currentSection;
        ctx.m_currentSection = nullptr;
        return nullptr;
      }
      BaseString::snprintf(ctx.fname, sizeof(ctx.fname), "%s", section);
      free(section);
      ctx.type = InitConfigFileParser::DefaultSection;
      ctx.m_sectionLineno = ctx.m_lineno;
      ctx.m_currentSection = new Properties(true);
      ctx.m_userDefaults = nullptr;
      require((ctx.m_currentInfo = m_info->getInfo(ctx.fname)) != nullptr);
      require((ctx.m_systemDefaults = m_info->getDefaults(ctx.fname)) !=
              nullptr);
      continue;
    }

    /************************
     * 2. Parse new section *
     ************************/
    if (char *section = parseSectionHeader(line)) {
      if (!storeSection(ctx)) {
        free(section);
        ctx.reportError(
            "Could not store previous section "
            "of configuration file.");
        delete ctx.m_currentSection;
        ctx.m_currentSection = nullptr;
        return nullptr;
      }
      BaseString::snprintf(ctx.fname, sizeof(ctx.fname), "%s", section);
      free(section);
      ctx.type = InitConfigFileParser::Section;
      ctx.m_sectionLineno = ctx.m_lineno;
      ctx.m_currentSection = new Properties(true);
      ctx.m_userDefaults = getSection(ctx.fname, ctx.m_defaults);
      require((ctx.m_currentInfo = m_info->getInfo(ctx.fname)) != nullptr);
      require((ctx.m_systemDefaults = m_info->getDefaults(ctx.fname)) !=
              nullptr);
      continue;
    }

    /****************************
     * 3. Parse name-value pair *
     ****************************/
    if (!parseNameValuePair(ctx, line)) {
      ctx.reportError("Could not parse name-value pair in config file.");
      delete ctx.m_currentSection;
      ctx.m_currentSection = nullptr;
      return nullptr;
    }
  }

  if (ferror(file)) {
    ctx.reportError("Failure in reading");
    delete ctx.m_currentSection;
    ctx.m_currentSection = nullptr;
    return nullptr;
  }

  if (!storeSection(ctx)) {
    ctx.reportError("Could not store section of configuration file.");
    delete ctx.m_currentSection;
    ctx.m_currentSection = nullptr;
    return nullptr;
  }

  return run_config_rules(ctx);
}

Config *InitConfigFileParser::run_config_rules(Context &ctx) {
  for (size_t i = 0; ConfigInfo::m_ConfigRules[i].m_configRule != nullptr;
       i++) {
    ctx.type = InitConfigFileParser::Undefined;
    ctx.m_info = m_info;
    ctx.m_currentSection = nullptr;
    ctx.m_userDefaults = nullptr;
    ctx.m_currentInfo = nullptr;
    ctx.m_systemDefaults = nullptr;

    Vector<ConfigInfo::ConfigRuleSection> tmp;
    if (!(*ConfigInfo::m_ConfigRules[i].m_configRule)(
            tmp, ctx, ConfigInfo::m_ConfigRules[i].m_ruleData))
      return nullptr;

    for (unsigned j = 0; j < tmp.size(); j++) {
      BaseString::snprintf(ctx.fname, sizeof(ctx.fname), "%s",
                           tmp[j].m_sectionType.c_str());
      ctx.type = InitConfigFileParser::Section;
      // Memory that belongs to m_sectionData is transferred to
      // ctx.m_currentSection and will be released by ctx.
      ctx.m_currentSection = tmp[j].m_sectionData;
      tmp[j].m_sectionData = nullptr;
      ctx.m_userDefaults = getSection(ctx.fname, ctx.m_defaults);
      require((ctx.m_currentInfo = m_info->getInfo(ctx.fname)) != nullptr);
      require((ctx.m_systemDefaults = m_info->getDefaults(ctx.fname)) !=
              nullptr);
      if (!storeSection(ctx)) {
        // Memory at ctx.m_currentSection will be released by storeSection() in
        // Success case. So, releasing memory on failure.
        delete ctx.m_currentSection;
        ctx.m_currentSection = nullptr;
        // Releasing memory referenced by tmp[].m_sectionData
        for (unsigned itr = j + 1; itr < tmp.size(); itr++) {
          delete tmp[itr].m_sectionData;
        }
        return nullptr;
      }
    }
  }

  Uint32 nConnections = 0;
  Uint32 nComputers = 0;
  Uint32 nNodes = 0;
  Uint32 nExtConnections = 0;
  const char *system = "?";
  ctx.m_userProperties.get("NoOfConnections", &nConnections);
  ctx.m_userProperties.get("NoOfComputers", &nComputers);
  ctx.m_userProperties.get("NoOfNodes", &nNodes);
  ctx.m_userProperties.get("ExtNoOfConnections", &nExtConnections);
  ctx.m_userProperties.get("ExtSystem", &system);
  ctx.m_config->put("NoOfConnections", nConnections);
  ctx.m_config->put("NoOfComputers", nComputers);
  ctx.m_config->put("NoOfNodes", nNodes);

  char tmpLine[MAX_LINE_LENGTH];
  BaseString::snprintf(tmpLine, MAX_LINE_LENGTH,
                       "EXTERNAL SYSTEM_%s:NoOfConnections", system);
  ctx.m_config->put(tmpLine, nExtConnections);

  return new Config(ctx.m_configValues.getConfigValues());
}

//****************************************************************************
//  Parse Name-Value Pair
//****************************************************************************

bool InitConfigFileParser::parseNameValuePair(Context &ctx, const char *line) {
  if (ctx.m_currentSection == nullptr) {
    ctx.reportError("Value specified outside section");
    return false;
  }

  // *************************************
  //  Split string at first occurrence of
  //  '=' or ':'
  // *************************************

  Vector<BaseString> tmp_string_split;
  if (BaseString(line).split(tmp_string_split, "=:", 2) != 2) {
    ctx.reportError("Parse error");
    return false;
  }

  // *************************************
  //  Remove all after #
  // *************************************

  Vector<BaseString> tmp_string_split2;
  tmp_string_split[1].split(tmp_string_split2, "#", 2);
  tmp_string_split[1] = tmp_string_split2[0];

  // *************************************
  // Remove leading and trailing chars
  // *************************************
  {
    for (int i = 0; i < 2; i++) tmp_string_split[i].trim("\r\n \t");
  }

  return storeNameValuePair(ctx,
                            tmp_string_split[0].c_str(),   // fname
                            tmp_string_split[1].c_str());  // value
}

bool InitConfigFileParser::storeNameValuePair(Context &ctx, const char *fname,
                                              const char *value) {
  /**
   * The parameters MaxNoOfConcurrentIndexOperations, MaxNoOfLocalOperations
   * MaxNoOfFiredTriggers and MaxNoOfLocalScans cannot be used concurrently with
   * TransactionMemory.
   * Explicitly setting any of these parameters when TransactionMemory has also
   * been set keeps the management node from starting.
   *
   * The parameter MaxNoOfConcurrentOperations can be used concurrently with
   * TransactionMemory since it also sets the size of Dbtc record pools for
   * takeover and puts an upper limit on MaxDMLOperationsPerTransaction.
   *
   * The parameter MaxNoOfConcurrentScans can be used concurrently with
   * TransactionMemory  it is also determines max size of pool
   * Dbtc::scanRecordPool and will limit the number of concurrent scans in one
   * Dbtc instance.
   */
  if (native_strcasecmp(fname, "MaxNoOfConcurrentIndexOperations") == 0 ||
      native_strcasecmp(fname, "MaxNoOfFiredTriggers") == 0 ||
      native_strcasecmp(fname, "MaxNoOfLocalScans") == 0 ||
      native_strcasecmp(fname, "MaxNoOfLocalOperations") == 0) {
    if (ctx.m_currentSection->contains("TransactionMemory")) {
      ctx.reportError(
          "[%s] Parameter %s can not be set along with TransactionMemory",
          ctx.fname, fname);
      return false;
    }
  }

  if (native_strcasecmp(fname, "TransactionMemory") == 0) {
    if (ctx.m_currentSection->contains("MaxNoOfConcurrentIndexOperations") ||
        ctx.m_currentSection->contains("MaxNoOfFiredTriggers") ||
        ctx.m_currentSection->contains("MaxNoOfLocalScans") ||
        ctx.m_currentSection->contains("MaxNoOfLocalOperations")) {
      ctx.reportError(
          "[%s] Parameter %s can not be set along with any of the below "
          "deprecated parameter(s) MaxNoOfConcurrentIndexOperations, "
          "MaxNoOfFiredTriggers, MaxNoOfLocalScans "
          "and MaxNoOfLocalOperations.",
          ctx.fname, fname);
      return false;
    }
  }

  if (ctx.m_currentSection->contains(fname)) {
    ctx.reportError("[%s] Parameter %s specified twice", ctx.fname, fname);
    return false;
  }

  if (!ctx.m_currentInfo->contains(fname)) {
    ctx.reportError("[%s] Unknown parameter: %s", ctx.fname, fname);
    return false;
  }

  ConfigInfo::Status status = m_info->getStatus(ctx.m_currentInfo, fname);
  if (status == ConfigInfo::CI_NOTIMPLEMENTED) {
    ctx.reportWarning("[%s] %s not yet implemented", ctx.fname, fname);
  }
  if (status == ConfigInfo::CI_DEPRECATED) {
    const char *desc = m_info->getDescription(ctx.m_currentInfo, fname);
    if (desc && desc[0]) {
      ctx.reportWarning("[%s] %s is deprecated, will use %s instead", ctx.fname,
                        fname, desc);
    } else {
      ctx.reportWarning("[%s] %s is deprecated", ctx.fname, fname);
    }
  }
  if (status == ConfigInfo::CI_INTERNAL) {
    ctx.reportError("[%s] Parameter %s not configurable by user", ctx.fname,
                    fname);
    return false;
  }

  const ConfigInfo::Type type = m_info->getType(ctx.m_currentInfo, fname);
  switch (type) {
    case ConfigInfo::CI_BOOL: {
      bool value_bool;
      if (!convertStringToBool(value, value_bool)) {
        ctx.reportError("Illegal boolean value for parameter %s", fname);
        return false;
      }
      require(ctx.m_currentSection->put(fname, value_bool));
      break;
    }
    case ConfigInfo::CI_INT:
    case ConfigInfo::CI_INT64: {
      Uint64 value_int;
      if (!convertStringToUint64(value, value_int)) {
        ctx.reportError("Illegal integer value for parameter %s", fname);
        return false;
      }
      if (!m_info->verify(ctx.m_currentInfo, fname, value_int)) {
        ctx.reportError(
            "Illegal value %s for parameter %s.\n"
            "Legal values are between %llu and %llu",
            value, fname, m_info->getMin(ctx.m_currentInfo, fname),
            m_info->getMax(ctx.m_currentInfo, fname));
        return false;
      }
      if (type == ConfigInfo::CI_INT) {
        require(ctx.m_currentSection->put(fname, (Uint32)value_int));
      } else {
        require(ctx.m_currentSection->put64(fname, value_int));
      }
      break;
    }
    case ConfigInfo::CI_STRING:
      require(ctx.m_currentSection->put(fname, value));
      break;

    case ConfigInfo::CI_ENUM: {
      Uint32 value_int;
      if (!m_info->verify_enum(ctx.m_currentInfo, fname, value, value_int)) {
        BaseString values;
        m_info->get_enum_values(ctx.m_currentInfo, fname, values);
        ctx.reportError(
            "Illegal value '%s' for parameter %s. "
            "Legal values are: '%s'",
            value, fname, values.c_str());
        return false;
      }
      require(ctx.m_currentSection->put(fname, value_int));
      break;
    }

    case ConfigInfo::CI_BITMASK: {
      if (strlen(value) <= 0) {
        ctx.reportError(
            "Illegal value '%s' for parameter %s. "
            "Error: Zero length string",
            value, fname);
        return false;
      }
      Uint64 max = m_info->getMax(ctx.m_currentInfo, fname);
      SparseBitmask mask((unsigned)max);
      int res = parse_mask(value, mask);
      if (res < 0) {
        BaseString desc("Unknown error.");
        switch (res) {
          case -1:
            desc.assign("Invalid syntax for bitmask");
            break;
          case -2:
            desc.assfmt("Too large id used in bitmask, max is %llu", max);
            break;
          case -3:
            desc.assfmt("Empty bitmask not allowed");
            break;
          default:
            break;
        }

        ctx.reportError("Illegal value '%s' for parameter %s. Error: %s", value,
                        fname, desc.c_str());
        return false;
      }
      require(ctx.m_currentSection->put(fname, value));
      break;
    }

    case ConfigInfo::CI_SECTION:
      abort();
  }
  return true;
}

//****************************************************************************
//  Is Empty Line
//****************************************************************************

bool InitConfigFileParser::isEmptyLine(const char *line) const {
  int i;

  // Check if it is a comment line
  if (line[0] == '#') return true;

  // Check if it is a line with only spaces
  for (i = 0; i < MAX_LINE_LENGTH && line[i] != '\n' && line[i] != '\0'; i++) {
    if (line[i] != ' ' && line[i] != '\t') return false;
  }
  return true;
}

//****************************************************************************
//  Convert String to Int
//****************************************************************************
bool InitConfigFileParser::convertStringToUint64(const char *s, Uint64 &val) {
  if (s == nullptr) return false;

  while (*s != '\0' && isspace(*s)) s++;
  const bool negative = (*s == '-');

  union {
    signed long long vs;
    unsigned long long vu;
  };
  char *p;
  constexpr int log10base = 0;
  errno = 0;
  if (negative) {
    vs = std::strtoll(s, &p, log10base);
    if ((vs == LLONG_MIN || vs == LLONG_MAX) && errno == ERANGE) {
      return false;
    }
  } else {
    vu = std::strtoull(s, &p, log10base);
    if (vu == ULLONG_MAX && errno == ERANGE) {
      return false;
    }
  }
  if (p == s) return false;

  int mul = 0;

  switch (*p) {
    case '\0':
      break;
    case 'k':
    case 'K':
      mul = 10;
      p++;
      break;
    case 'M':
      mul = 20;
      p++;
      break;
    case 'G':
      mul = 30;
      p++;
      break;
    default:
      return false;
  }
  if (*p != '\0') return false;
  if (negative) {
    Int64 v = (vs << mul);
    if ((v >> mul) != vs) return false;
    val = v;
  } else {
    Uint64 v = (vu << mul);
    if ((v >> mul) != vu) return false;
    val = v;
  }
  return true;
}

bool InitConfigFileParser::convertStringToBool(const char *s, bool &val) {
  if (s == nullptr) return false;
  if (strlen(s) == 0) return false;

  if (!strcmp(s, "Y") || !strcmp(s, "y") || !strcmp(s, "Yes") ||
      !strcmp(s, "YES") || !strcmp(s, "yes") || !strcmp(s, "True") ||
      !strcmp(s, "TRUE") || !strcmp(s, "true") || !strcmp(s, "1")) {
    val = true;
    return true;
  }

  if (!strcmp(s, "N") || !strcmp(s, "n") || !strcmp(s, "No") ||
      !strcmp(s, "NO") || !strcmp(s, "no") || !strcmp(s, "False") ||
      !strcmp(s, "FALSE") || !strcmp(s, "false") || !strcmp(s, "0")) {
    val = false;
    return true;
  }

  return false;  // Failure to convert
}

//****************************************************************************
//  Parse Section Header
//****************************************************************************
static void trim(char *str) {
  int len = (int)strlen(str);
  for (len--; (str[len] == '\r' || str[len] == '\n' || str[len] == ' ' ||
               str[len] == '\t') &&
              len > 0;
       len--)
    str[len] = 0;

  int pos = 0;
  while (str[pos] == ' ' || str[pos] == '\t') pos++;

  if (str[pos] == '\"' && str[len] == '\"') {
    pos++;
    str[len] = 0;
    len--;
  }

  memmove(str, &str[pos], len - pos + 2);
}

char *InitConfigFileParser::parseSectionHeader(const char *line) const {
  char *tmp = strdup(line);

  if (tmp[0] != '[') {
    free(tmp);
    return nullptr;
  }

  if (tmp[strlen(tmp) - 1] != ']') {
    free(tmp);
    return nullptr;
  }
  tmp[strlen(tmp) - 1] = 0;

  tmp[0] = ' ';
  trim(tmp);

  // Get the correct header name if an alias
  {
    const char *tmp_alias = m_info->getAlias(tmp);
    if (tmp_alias) {
      free(tmp);
      tmp = strdup(tmp_alias);
    }
  }

  // Lookup token among sections
  if (!m_info->isSection(tmp)) {
    free(tmp);
    return nullptr;
  }
  if (m_info->getInfo(tmp)) return tmp;

  free(tmp);
  return nullptr;
}

//****************************************************************************
//  Parse Default Section Header
//****************************************************************************

char *InitConfigFileParser::parseDefaultSectionHeader(const char *line) const {
  static char token1[MAX_LINE_LENGTH], token2[MAX_LINE_LENGTH];

  int no = sscanf(line, "[%120[A-Z_a-z] %120[A-Z_a-z]]", token1, token2);

  // Not correct no of tokens
  if (no != 2) return nullptr;

  // Not correct keyword at end
  if (native_strcasecmp(token2, "DEFAULT") != 0) return nullptr;

  const char *token1_alias = m_info->getAlias(token1);
  if (token1_alias == nullptr) token1_alias = token1;

  if (m_info->getInfo(token1_alias)) {
    return strdup(token1_alias);
  }

  // Did not find section
  return nullptr;
}

const Properties *InitConfigFileParser::getSection(const char *name,
                                                   const Properties *src) {
  const Properties *p;
  if (src && src->get(name, &p)) return p;

  return nullptr;
}

//****************************************************************************
//  STORE section
//****************************************************************************
bool InitConfigFileParser::storeSection(Context &ctx) {
  if (ctx.m_currentSection == nullptr) return true;
  for (int i = (int)strlen(ctx.fname) - 1; i >= 0; i--) {
    ctx.fname[i] = toupper(ctx.fname[i]);
  }
  BaseString::snprintf(ctx.pname, sizeof(ctx.pname), "%s", ctx.fname);

  char buf[255];
  if (ctx.type == InitConfigFileParser::Section)
    BaseString::snprintf(buf, sizeof(buf), "%s", ctx.fname);
  if (ctx.type == InitConfigFileParser::DefaultSection)
    BaseString::snprintf(buf, sizeof(buf), "%s DEFAULT", ctx.fname);
  BaseString::snprintf(ctx.fname, sizeof(ctx.fname), "%s", buf);

  if (ctx.type == InitConfigFileParser::Section) {
    for (int i = 0; i < m_info->m_NoOfRules; i++) {
      const ConfigInfo::SectionRule &rule = m_info->m_SectionRules[i];
      if (!strcmp(rule.m_section, "*") || !strcmp(rule.m_section, ctx.fname)) {
        if (!(*rule.m_sectionRule)(ctx, rule.m_ruleData)) {
          return false;
        }
      }
    }
  }
  if (ctx.type == InitConfigFileParser::DefaultSection &&
      !ctx.m_defaults->put(ctx.pname, ctx.m_currentSection)) {
    ctx.reportError("Duplicate default section not allowed");
    return false;
  }
  if (ctx.type == InitConfigFileParser::Section)
    require(ctx.m_config->put(ctx.pname, ctx.m_currentSection));
  delete ctx.m_currentSection;
  ctx.m_currentSection = nullptr;
  return true;
}

void InitConfigFileParser::Context::reportError(const char *fmt, ...) {
  va_list ap;
  char buf[1000];

  va_start(ap, fmt);
  if (fmt != nullptr) BaseString::vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  g_eventLogger->error("at line %d: %s", m_lineno, buf);

  // m_currentSection->print();
}

void InitConfigFileParser::Context::reportWarning(const char *fmt, ...) {
  va_list ap;
  char buf[1000];

  va_start(ap, fmt);
  if (fmt != nullptr) BaseString::vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
  va_end(ap);
  g_eventLogger->warning("at line %d: %s", m_lineno, buf);
}

#include "my_default.h"
#include "my_getopt.h"
#include "my_sys.h"

static int order = 1;
static bool parse_mycnf_opt(int, const struct my_option *opt, char *) {
  /*
   * Ok to cast away const since parse_mycnf_opt is always called with a non
   * const my_option object.
   * See load_defaults(Vector<struct my_option>& options, const char* groups[])
   */
  auto *mutable_opt = const_cast<my_option *>(opt);
  long *app_type = (long *)&mutable_opt->app_type;
  if (opt->comment)
    (*app_type)++;
  else
    *app_type = order++;
  return false;
}

bool InitConfigFileParser::store_in_properties(
    Vector<struct my_option> &options, InitConfigFileParser::Context &ctx,
    const char *name) {
  for (unsigned i = 0; i < options.size(); i++) {
    if (options[i].app_type == nullptr) {
      // Option not found in in my.cnf
      continue;
    }

    const char *section = options[i].comment;
    if (!section) {
      // Option which is not to be saved, like "ndbd", "ndbapi", "mysqld" etc.
      continue;
    }

    if (strcmp(section, name) == 0) {
      const char *value = nullptr;
      char buf[32];
      switch (options[i].var_type) {
        case GET_BOOL:
          BaseString::snprintf(buf, sizeof(buf), "%s",
                               *(bool *)options[i].value ? "true" : "false");
          value = buf;
          break;
        case GET_INT:
        case GET_UINT:
          BaseString::snprintf(buf, sizeof(buf), "%u",
                               *(Uint32 *)options[i].value);
          value = buf;
          break;
        case GET_ULL:
          BaseString::snprintf(buf, sizeof(buf), "%llu",
                               *(Uint64 *)options[i].value);
          value = buf;
          break;
        case GET_STR_ALLOC:
          value = *(char **)options[i].value;
          break;
        default:
          abort();
      }

      const char *fname = options[i].name;
      if (!storeNameValuePair(ctx, fname, value)) return false;
    }
  }
  return true;
}

bool InitConfigFileParser::handle_mycnf_defaults(
    Vector<struct my_option> &options, InitConfigFileParser::Context &ctx,
    const char *name) {
  strcpy(ctx.fname, name);
  ctx.type = InitConfigFileParser::DefaultSection;
  ctx.m_currentSection = new Properties(true);
  ctx.m_userDefaults = nullptr;
  require((ctx.m_currentInfo = m_info->getInfo(ctx.fname)) != nullptr);
  require((ctx.m_systemDefaults = m_info->getDefaults(ctx.fname)) != nullptr);
  if (store_in_properties(options, ctx, name)) {
    if (storeSection(ctx)) {
      return true;
    }
  }
  delete ctx.m_currentSection;
  ctx.m_currentSection = nullptr;
  return false;
}

static int load_defaults(Vector<struct my_option> &options,
                         const char *groups[]) {
  int argc = 1;
  const char *argv[] = {"ndb_mgmd", nullptr, nullptr, nullptr, nullptr};
  BaseString file;
  BaseString extra_file;
  BaseString group_suffix;

  const char *save_file = my_defaults_file;
  const char *save_extra_file = my_defaults_extra_file;
  const char *save_group_suffix = my_defaults_group_suffix;

  if (my_defaults_file) {
    file.assfmt("--defaults-file=%s", my_defaults_file);
    argv[argc++] = file.c_str();
  }

  if (my_defaults_extra_file) {
    extra_file.assfmt("--defaults-extra-file=%s", my_defaults_extra_file);
    argv[argc++] = extra_file.c_str();
  }

  if (my_defaults_group_suffix) {
    group_suffix.assfmt("--defaults-group-suffix=%s", my_defaults_group_suffix);
    argv[argc++] = group_suffix.c_str();
  }

  /*
   * Cast away const from argv is most probably ok.
   * The load_defaults function can modify an argv option, but should do it
   * only for unknown options.  And the options added to argv above should
   * always be valid.
   */
  char **tmp = const_cast<char **>(argv);
  MEM_ROOT alloc;
  int ret = load_defaults("my", groups, &argc, &tmp, &alloc);

  my_defaults_file = save_file;
  my_defaults_extra_file = save_extra_file;
  my_defaults_group_suffix = save_group_suffix;

  if (ret != 0) {
    return ret;
  }
  ret = handle_options(&argc, &tmp, options.getBase(), parse_mycnf_opt);

  return ret;
}

bool InitConfigFileParser::load_mycnf_groups(Vector<struct my_option> &options,
                                             InitConfigFileParser::Context &ctx,
                                             const char *name,
                                             const char *groups[]) {
  unsigned i;
  Vector<struct my_option> copy;
  for (i = 0; i < options.size(); i++) {
    if (options[i].comment && strcmp(options[i].comment, name) == 0) {
      options[i].app_type = nullptr;
      copy.push_back(options[i]);
    }
  }

  struct my_option end;
  memset(&end, 0, sizeof(end));
  copy.push_back(end);

  if (load_defaults(copy, groups)) return false;

  return store_in_properties(copy, ctx, name);
}

/*
  Use "GET_STR_ALLOC" instead of "GET_STR" and during exit, call my_free() to
  release the memory allocated by my_strdup() from handle_options().
*/
Config *InitConfigFileParser::parse_mycnf(const char *cluster_config_suffix) {
  Config *res = nullptr;
  bool release_current_section = true;
  Vector<struct my_option> options;
  for (int i = 0; i < ConfigInfo::m_NoOfParams; ++i) {
    {
      struct my_option opt;
      memset(&opt, 0, sizeof(opt));
      const ConfigInfo::ParamInfo &param = ConfigInfo::m_ParamInfo[i];
      switch (param._type) {
        case ConfigInfo::CI_BOOL:
          opt.value = malloc(sizeof(bool));
          opt.var_type = GET_BOOL;
          break;
        case ConfigInfo::CI_INT:
          opt.value = malloc(sizeof(uint));
          opt.var_type = GET_UINT;
          break;
        case ConfigInfo::CI_INT64:
          opt.value = malloc(sizeof(Uint64));
          opt.var_type = GET_ULL;
          break;
        case ConfigInfo::CI_ENUM:
        case ConfigInfo::CI_STRING:
        case ConfigInfo::CI_BITMASK:
          opt.value = malloc(sizeof(char *));
          *((char **)opt.value) = nullptr;
          opt.var_type = GET_STR_ALLOC;
          break;
        default:
          continue;
      }
      opt.name = param._fname;
      opt.id = 256;
      opt.app_type = nullptr;
      opt.arg_type = REQUIRED_ARG;
      opt.comment = param._section;
      options.push_back(opt);
    }
  }

  struct my_option *ndbd, *ndb_mgmd, *mysqld, *api;

  /**
   * Add ndbd, ndb_mgmd, api/mysqld
   */
  Uint32 idx = options.size();
  {
    struct my_option opt;
    memset(&opt, 0, sizeof(opt));
    opt.name = "ndbd";
    opt.id = 256;
    opt.value = malloc(sizeof(char *));
    *((char **)opt.value) = nullptr;
    opt.var_type = GET_STR_ALLOC;
    opt.arg_type = REQUIRED_ARG;
    options.push_back(opt);

    opt.name = "ndb_mgmd";
    opt.id = 256;
    opt.value = malloc(sizeof(char *));
    *((char **)opt.value) = nullptr;
    opt.var_type = GET_STR_ALLOC;
    opt.arg_type = REQUIRED_ARG;
    options.push_back(opt);

    opt.name = "mysqld";
    opt.id = 256;
    opt.value = malloc(sizeof(char *));
    *((char **)opt.value) = nullptr;
    opt.var_type = GET_STR_ALLOC;
    opt.arg_type = REQUIRED_ARG;
    options.push_back(opt);

    opt.name = "ndbapi";
    opt.id = 256;
    opt.value = malloc(sizeof(char *));
    *((char **)opt.value) = nullptr;
    opt.var_type = GET_STR_ALLOC;
    opt.arg_type = REQUIRED_ARG;
    options.push_back(opt);

    memset(&opt, 0, sizeof(opt));
    options.push_back(opt);

    ndbd = &options[idx];
    ndb_mgmd = &options[idx + 1];
    mysqld = &options[idx + 2];
    api = &options[idx + 3];
  }

  Context ctx;
  const char *groups[] = {"cluster_config", nullptr};
  const char *save_group_suffix = my_defaults_group_suffix;
  if (cluster_config_suffix != nullptr) {
    my_defaults_group_suffix = cluster_config_suffix;
  }
  if (load_defaults(options, groups)) goto end;

  ctx.m_lineno = 0;
  if (!handle_mycnf_defaults(options, ctx, "DB")) goto end;
  if (!handle_mycnf_defaults(options, ctx, "API")) goto end;
  if (!handle_mycnf_defaults(options, ctx, "MGM")) goto end;
  if (!handle_mycnf_defaults(options, ctx, "TCP")) goto end;
  if (!handle_mycnf_defaults(options, ctx, "SHM")) goto end;

  {
    struct sect {
      struct my_option *src;
      const char *name;
    } sections[] = {
        {ndb_mgmd, "MGM"}, {ndbd, "DB"}, {mysqld, "API"}, {api, "API"}};

    for (unsigned i = 0; i + 1 < NDB_ARRAY_SIZE(sections); i++) {
      for (unsigned j = i + 1; j < NDB_ARRAY_SIZE(sections); j++) {
        if (sections[j].src->app_type < sections[i].src->app_type) {
          sect swap = sections[i];
          sections[i] = sections[j];
          sections[j] = swap;
        }
      }
    }

    ctx.type = InitConfigFileParser::Section;
    ctx.m_sectionLineno = ctx.m_lineno;
    for (unsigned i = 0; i < NDB_ARRAY_SIZE(sections); i++) {
      if (sections[i].src->app_type) {
        strcpy(ctx.fname, sections[i].name);
        BaseString str(*(char **)sections[i].src->value);
        Vector<BaseString> list;
        str.split(list, ",");

        const char *defaults_groups[] = {nullptr, nullptr, nullptr};
        for (unsigned j = 0; j < list.size(); j++) {
          // Remove leading and trailing spaces from hostname
          list[j].trim();

          BaseString group_idx;
          BaseString group_host;
          group_idx.assfmt("%s.%s.%d", groups[0], sections[i].src->name, j + 1);
          group_host.assfmt("%s.%s.%s", groups[0], sections[i].src->name,
                            list[j].c_str());
          defaults_groups[0] = group_idx.c_str();
          if (list[j].length())
            defaults_groups[1] = group_host.c_str();
          else
            defaults_groups[1] = nullptr;

          ctx.m_currentSection = new Properties(true);
          ctx.m_userDefaults = getSection(ctx.fname, ctx.m_defaults);
          require((ctx.m_currentInfo = m_info->getInfo(ctx.fname)) != nullptr);
          require((ctx.m_systemDefaults = m_info->getDefaults(ctx.fname)) !=
                  nullptr);
          if (!load_mycnf_groups(options, ctx, sections[i].name,
                                 defaults_groups))
            goto end;

          // The [cluster_config] section in my.cnf specifies the hostname,
          // but it can also be specified a second time in the nodes section
          // make sure they match if specified in both places, else
          // save the value from cluster_config section
          //
          // Example:
          // [cluster_config]
          // ndbd=hostname1
          //      ^^^^^^^^^
          // [cluster_config.ndbd.1]
          // HostName=hostname1
          //          ^^^^^^^^^
          //
          if (ctx.m_currentSection->contains("HostName")) {
            // HostName specified a second time, check that it matches
            const char *host_name;
            require(ctx.m_currentSection->get("HostName", &host_name));
            if (strcmp(host_name, list[j].c_str()) != 0) {
              ctx.reportError(
                  "Illegal value 'HostName=%s' specified for "
                  "%s, previously set to '%s'",
                  host_name, group_idx.c_str(), list[j].c_str());
              goto end;
            }
          } else {
            require(ctx.m_currentSection->put("HostName", list[j].c_str()));
          }

          if (!storeSection(ctx)) goto end;
        }
      }
    }
  }

  res = run_config_rules(ctx);
  release_current_section = false;

end:
  my_defaults_group_suffix = save_group_suffix;

  for (int i = 0; options[i].name; i++) {
    if (options[i].var_type == GET_STR_ALLOC)
      my_free(*((char **)options[i].value));
    free(options[i].value);
  }

  /**
   * ctx.m_currentSection is released only in case of failure
   * in success case the memory will be released in storeSection().
   */
  if (release_current_section) {
    delete ctx.m_currentSection;
    ctx.m_currentSection = nullptr;
  }
  return res;
}

template class Vector<struct my_option>;

/*
  See include/my_getopt.h for the declaration of struct my_option
*/

#ifdef TEST_INITCONFIGFILEPARSER

#include "unittest/mytap/tap.h"

int main() {
  plan(29);

  // Check the string to int parsing function convertStringToUint64()
  Uint64 value;
  ok1(InitConfigFileParser::convertStringToUint64("37", value));
  ok1(value == 37);
  ok1(InitConfigFileParser::convertStringToUint64("0", value));
  ok1(value == 0);
  ok1(InitConfigFileParser::convertStringToUint64("-0", value));  // ??
  ok1(static_cast<Int64>(value) == -0);
  ok1(InitConfigFileParser::convertStringToUint64("-1", value));
  ok1(static_cast<Int64>(value) == -1);
  ok1(InitConfigFileParser::convertStringToUint64("-37", value));
  ok1(static_cast<Int64>(value) == -37);

  // Valid multipliers
  ok1(InitConfigFileParser::convertStringToUint64("37k", value));
  ok1(value == 37ULL * 1024);
  ok1(InitConfigFileParser::convertStringToUint64("37K", value));
  ok1(value == 37ULL * 1024);
  ok1(InitConfigFileParser::convertStringToUint64("37M", value));
  ok1(value == 37ULL * 1024 * 1024);
  ok1(InitConfigFileParser::convertStringToUint64("37G", value));
  ok1(value == 37ULL * 1024 * 1024 * 1024);
#ifdef NOT_YET
  ok1(InitConfigFileParser::convertStringToUint64("37T", value));
  ok1(value == 37ULL * 1024 * 1024 * 1024 * 1024);
#endif

  // Invalid multipliers
  ok1(!InitConfigFileParser::convertStringToUint64("10kB", value));
  ok1(!InitConfigFileParser::convertStringToUint64("10 M", value));
  ok1(!InitConfigFileParser::convertStringToUint64("10 \"M12L\"", value));
  ok1(!InitConfigFileParser::convertStringToUint64("10 'M12L'", value));
  ok1(!InitConfigFileParser::convertStringToUint64("10M12L", value));
  ok1(!InitConfigFileParser::convertStringToUint64("10T'", value));

  ok1(!InitConfigFileParser::convertStringToUint64("M", value));
  ok1(!InitConfigFileParser::convertStringToUint64("MAX_UINT", value));
  ok1(!InitConfigFileParser::convertStringToUint64("Magnus'", value));
  ok1(!InitConfigFileParser::convertStringToUint64("", value));
  ok1(!InitConfigFileParser::convertStringToUint64("trettisju", value));

  return exit_status();
}

#endif
