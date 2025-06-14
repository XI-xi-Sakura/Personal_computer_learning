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

#ifndef ConfigInfo_H
#define ConfigInfo_H

#include <kernel_types.h>
#include <ndb_limits.h>
#include <NdbOut.hpp>
#include <Properties.hpp>
#include <cassert>
#include <optional>
#include "InitConfigFileParser.hpp"

// Parameter must be specified in config file
#define MANDATORY ((char *)~(UintPtr)0)

/**
 * @class  ConfigInfo
 * @brief  Metainformation about ALL cluster configuration parameters
 *
 * Use the getters to find out metainformation about parameters.
 *
 * Note that the static data member m_ParamInfo is dynamically initialized in
 * debug build due to the asserts.
 *
 * This prohibits the use of static ConfigInfo objects since the initialization
 * order is not determined and the constructor of ConfigInfo accesses
 * m_ParamInfo.
 */
class ConfigInfo {
 public:
  enum Type {
    CI_BOOL,
    CI_INT,
    CI_INT64,
    CI_STRING,
    CI_ENUM,     // String externally, int internally
    CI_BITMASK,  // String both externally and internally
    CI_SECTION
  };
  enum Status {
    CI_USED,            ///< Active
    CI_EXPERIMENTAL,    ///< Active but experimental
    CI_DEPRECATED,      ///< Can be used, but shouldn't
    CI_NOTIMPLEMENTED,  ///< Is ignored.
    CI_INTERNAL         ///< Not configurable by the user
  };

  enum Flags {
    CI_ONLINE_UPDATEABLE = 1,  // Parameter can be updated online
    CI_CHECK_WRITABLE = 2,     // Path given by parameter should be writable

    /*
      Flags  telling how the system must be restarted for a changed
      parameter to take effect

      Default is none of these flags set, which means node restart
      of one node at a time for the setting to take effect

      CS_RESTART_INITIAL
      Each data node need to be restarted one at a time with --initial

      CS_RESTART_SYSTEM
      The whole system need to be stopped and then started up again

      CS_RESTART_SYSTEM + CS_RESTART_INITIAL
      The whole system need to be stopped and then restarted with --initial
      thus destroying any data in the cluster

      These flags can not be combined with CI_ONLINE_UPDATABLE flag which
      indicates that the parameter can be changed online without
      restarting anything
    */
    CI_RESTART_SYSTEM = 4,  // System restart is necessary to apply setting
    CI_RESTART_INITIAL = 8  // Initial restart is necessary to apply setting
  };

  struct Typelib {
    const char *name;
    Uint32 value;
  };

  /**
   *   Entry for one configuration parameter
   */
  struct ParamInfo {
    /**
     * Internal id used to identify configuration parameter when accessing
     * config.
     */
    Uint32 _paramId;
    /* External name, as given in text in config file. */
    const char *_fname;
    /**
     * Name (as it appears in config file text) of section that this entry
     * belongs to.
     *
     * Each section alsa has one entry with the section name stored in both
     * _fname and _section.
     */
    const char *_section;
    /* Short textual description/documentation for entry. */
    const char *_description;
    Status _status;
    Uint32 _flags;
    Type _type;
    /**
     * Default value, minimum value (if any), and maximum value (if any).
     *
     * Stored as pointers to char * representation of default (eg "10k").
     *
     * For section entries, instead the _default member gives the internal id
     * of that kind of section (CONNECTION_TYPE_TCP, NODE_TYPE_MGM, etc.)
     */
    const char *_default;
    const char *_min;
    const char *_max;

   public:
    ParamInfo(Uint32 paramId, const char *fname, const char *section,
              const char *description, Status status, Uint32 flags, Type type,
              const char *default_, const char *min, const char *max)
        : _paramId(paramId),
          _fname(fname),
          _section(section),
          _description(description),
          _status(status),
          _flags(flags),
          _type(type),
          _default(default_),
          _min(min),
          _max(max) {
      // If assert trigger, see special constructor for CI_SECTION below
      assert(type != CI_SECTION);
      // If assert trigger, see special constructor for CI_ENUM below
      assert(type != CI_ENUM);
    }
    /*
     * Constructor for CI_ENUM: _min must be a Typelib pointer and _max must
     * not be provided (it is not used).
     */
    ParamInfo(Uint32 paramId, const char *fname, const char *section,
              const char *description, Status status, Uint32 flags, Type type,
              const char *default_, const Typelib *typelibptr)
        : _paramId(paramId),
          _fname(fname),
          _section(section),
          _description(description),
          _status(status),
          _flags(flags),
          _type(type),
          _default(default_),
          _min(reinterpret_cast<const char *>(typelibptr)),
          _max(nullptr) {
      assert(type == CI_ENUM);
    }
    /*
     * Constructor for CI_SECTION: _default must be a section type and _min
     * and _max must not be provided (they are not used).
     */
    ParamInfo(Uint32 paramId, const char *fname, const char *section,
              const char *description, Status status, Uint32 flags, Type type,
              Uint32 section_type)
        : _paramId(paramId),
          _fname(fname),
          _section(section),
          _description(description),
          _status(status),
          _flags(flags),
          _type(type),
          _default(reinterpret_cast<const char *>(UintPtr{section_type})),
          _min(nullptr),
          _max(nullptr) {
      assert(type == CI_SECTION);
    }
  };

  /**
   * section type is stored in _default
   */
  static Uint32 getSectionType(const ParamInfo &p) {
    assert(p._type == CI_SECTION);
    const auto v = reinterpret_cast<UintPtr>(p._default);
    const auto t = static_cast<Uint32>(v);
    assert(v == t);
    return t;
  }

  /**
   * typelib ptr is stored in _min
   */
  static const Typelib *getTypelibPtr(const ParamInfo &p) {
    assert(p._type == CI_ENUM);
    return reinterpret_cast<const Typelib *>(p._min);
  }

  class ParamInfoIter {
    const ConfigInfo &m_info;
    const char *m_section_name;
    int m_curr_param;

   public:
    ParamInfoIter(const ConfigInfo &info, Uint32 section,
                  Uint32 section_type = ~0);

    const ParamInfo *next();
  };

  struct AliasPair {
    const char *name;
    const char *alias;
  };

  /**
   * Entry for one section rule
   */
  struct SectionRule {
    const char *m_section;
    bool (*m_sectionRule)(struct InitConfigFileParser::Context &,
                          const char *m_ruleData);
    const char *m_ruleData;
  };

  /**
   * Entry for config rule
   */
  struct ConfigRuleSection {
    BaseString m_sectionType;
    Properties *m_sectionData;
  };

  struct ConfigRule {
    bool (*m_configRule)(Vector<ConfigRuleSection> &,
                         struct InitConfigFileParser::Context &,
                         const char *m_ruleData);
    const char *m_ruleData;
  };

  ConfigInfo();
  /*
   * Get reference to a static default instance of ConfigInfo.
   * The instance is an static object within function and will
   * therefore delay initialization until first use.
   */
  static const ConfigInfo &default_instance();

  /**
   *   Checks if the suggested value is valid for the suggested parameter
   *   (i.e. if it is >= than min and <= than max).
   *
   *   @param  section  Init Config file section name
   *   @param  fname    Name of parameter
   *   @param  value    Value to check
   *   @return true if parameter value is valid.
   *
   *   @note Result is not defined if section/name are wrong!
   */
  bool verify(const Properties *section, const char *fname, Uint64 value) const;
  bool verify_enum(const Properties *section, const char *fname,
                   const char *value, Uint32 &value_int) const;
  void get_enum_values(const Properties *section, const char *fname,
                       BaseString &err) const;
  static const char *nameToAlias(const char *);
  static const char *getAlias(const char *);
  bool isSection(const char *) const;

  const char *getDescription(const Properties *sec, const char *fname) const;
  Type getType(const Properties *section, const char *fname) const;
  Status getStatus(const Properties *section, const char *fname) const;
  Uint64 getMin(const Properties *section, const char *fname) const;
  Uint64 getMax(const Properties *section, const char *fname) const;
  Uint64 getDefault(const Properties *section, const char *fname) const;
  Uint32 getFlags(const Properties *section, const char *fname) const;
  const char *getDefaultString(const Properties *section,
                               const char *fname) const;
  bool getMandatory(const Properties *section, const char *fname) const;
  bool hasDefault(const Properties *section, const char *fname) const;

  const Properties *getInfo(const char *section) const;
  const Properties *getDefaults(const char *section) const;

  const char *sectionName(Uint32 section_type, Uint32 type) const;

  void print(const char *section = nullptr) const;
  void print_xml(const char *section = nullptr) const;

  static std::optional<BaseString> normalizeParamValue(
      const ConfigInfo::ParamInfo &pinfo, const char *str);

 private:
  bool is_internal_section(const Properties *sec) const;
  void print_impl(const char *section, class ConfigPrinter &printer) const;

 private:
  Properties m_info;
  Properties m_systemDefaults;

  static const AliasPair m_sectionNameAliases[];
  static const char *m_sectionNames[];
  static const int m_noOfSectionNames;

 public:
  static const ParamInfo m_ParamInfo[];
  static const int m_NoOfParams;

  static const SectionRule m_SectionRules[];
  static const ConfigRule m_ConfigRules[];
  static const int m_NoOfRules;
};

inline const ConfigInfo &ConfigInfo::default_instance() {
  static const ConfigInfo default_instance;
  return default_instance;
}

#endif  // ConfigInfo_H
