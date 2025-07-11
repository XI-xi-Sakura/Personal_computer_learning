/* Copyright (c) 2018, 2025, Oracle and/or its affiliates.

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

#include <algorithm>
#include <memory>

#include "my_sqlcommand.h"
#include "mysql/components/services/log_builtins.h"
#include "mysql/components/services/log_shared.h"
#include "mysqld_error.h"
#include "partial_revokes.h"
#include "sql-common/json_dom.h"
#include "sql/auth/auth_acls.h"
#include "sql/auth/auth_common.h"
#include "sql/auth/auth_internal.h"
#include "sql/auth/sql_auth_cache.h"
#include "sql/mysqld.h"
#include "sql/sql_class.h"
#include "sql/sql_lex.h"

namespace consts {
const std::string Database("Database");
const std::string Privileges("Privileges");
const std::string Restrictions("Restrictions");
}  // namespace consts

/**
  Abstract restriction constructor
*/
Abstract_restrictions::Abstract_restrictions() = default;

/** Abstract restriction destructor */
Abstract_restrictions::~Abstract_restrictions() = default;

/**
  DB Restrictions constructor
*/
DB_restrictions::DB_restrictions() : Abstract_restrictions() {}

/**
  Copy constructor for DB Restrictions

  @param [in] other Source DB restrictions
*/
DB_restrictions::DB_restrictions(const DB_restrictions &other) {
  copy_restrictions(other);
}

/** Destructor */
DB_restrictions::~DB_restrictions() { clear(); }

/**
  Assignment operator

  @param [in] other Source DB restrictions
*/
DB_restrictions &DB_restrictions::operator=(const DB_restrictions &other) {
  if (this != &other) {
    clear();
    copy_restrictions(other);
  }
  return *this;
}

/**
  Assignment operator

  @param [in] restrictions Source DB restrictions
*/
DB_restrictions &DB_restrictions::operator=(DB_restrictions &&restrictions) {
  if (this != &restrictions) {
    clear();
    add(restrictions);
  }
  return *this;
}
/**
  Compare the two restrictions.

  @param [in] restrictions DB_restrictions object to be compared with this

  @retval true    If both DB_restrictions are same
  @retval false   Otherwise

*/
bool DB_restrictions::operator==(const DB_restrictions &restrictions) const {
  if (is_empty() && restrictions.is_empty()) return true;
  if (is_empty() || restrictions.is_empty()) return false;
  return (*m_restrictions == *restrictions.m_restrictions);
}

/**
  Add given privileges as restricted for the database

  @param [in] db_name      Database information
  @param [in] revoke_privs Privileges to be restricted
*/
void DB_restrictions::add(const std::string &db_name,
                          const Access_bitmask revoke_privs) {
  auto restrictions = create_restrictions_if_needed();
  if (restrictions->find(db_name) != restrictions->end()) {
    /* Partial revokes already exists on this DB so update them */
    (*restrictions)[db_name] |= (revoke_privs);
  } else {
    /* Partial revokes do not exist on this DB so add them */
    restrictions->emplace(db_name, revoke_privs);
  }
}

/**
  Add restriction information from source DB_restrictions

  @param [in] restrictions List of <database, privileges>
*/
void DB_restrictions::add(const DB_restrictions &restrictions) {
  if (!restrictions.m_restrictions) {
    return;
  }
  for (auto &itr : *(restrictions.m_restrictions))
    add(itr.first.c_str(), itr.second);
}

/**
  Deserializer. Converts a JSON object to <database, privileges> list.

  @param [in] json_object Restrictions represented by JSON

  @returns Status of transformation.
    @retval true  Error in deserializing the data.
    @retval false All good.
*/
bool DB_restrictions::add(const Json_object &json_object) {
  const Json_dom *restrictions_dom = json_object.get(consts::Restrictions);
  if (restrictions_dom) {
    if (restrictions_dom->json_type() != enum_json_type::J_ARRAY) return true;

    const Json_array *restrictions_array =
        down_cast<const Json_array *>(restrictions_dom);
    for (size_t i = 0; i < restrictions_array->size(); i++) {
      const Json_dom *restriction = (*restrictions_array)[i];
      if (restriction->json_type() != enum_json_type::J_OBJECT) return true;

      const Json_object *restricton_obj =
          down_cast<const Json_object *>(restriction);
      const Json_dom *db_dom = restricton_obj->get(consts::Database);
      if (db_dom && db_dom->json_type() != enum_json_type::J_STRING)
        return true;

      const Json_dom *privs_dom = restricton_obj->get(consts::Privileges);
      if (privs_dom && privs_dom->json_type() != enum_json_type::J_ARRAY)
        return true;
      const Json_string *db_string = nullptr;
      const Json_array *privs_array = nullptr;
      Access_bitmask priv_mask;

      if (db_dom) {
        db_string = down_cast<const Json_string *>(db_dom);
      }
      if (privs_dom) {
        privs_array = down_cast<const Json_array *>(privs_dom);
        priv_mask = 0;
        for (size_t pi = 0; pi < privs_array->size(); pi++) {
          const Json_dom *priv_dom = (*privs_array)[pi];
          if (priv_dom->json_type() != enum_json_type::J_STRING) return true;
          const Json_string *priv = down_cast<const Json_string *>(priv_dom);
          const auto &itr = global_acls_map.find(priv->value());
          if (itr == global_acls_map.end()) return true;
          priv_mask |= static_cast<Access_bitmask>(1UL << itr->second);
        }
        add(db_string->value(), priv_mask);
      }
    }
  }
  return false;
}

/**
  Remove given set of privilegs for a database from restriction list

  @param [in] db_name      Database information
  @param [in] revoke_privs List of privileges to remove
*/
void DB_restrictions::remove(const std::string &db_name,
                             const Access_bitmask revoke_privs) {
  if (m_restrictions != nullptr) {
    auto rest_itr = m_restrictions->find(db_name);
    if (rest_itr != m_restrictions->end()) {
      remove(revoke_privs, rest_itr->second);
    }
  }
}

/**
  Remove given set of privs from restricted list for all databases.

  If it turns out to be no restrictions on a DB after removal of
  the restrictions, then remove the db_name entry from the internal
  container as well.

  @param [in] revoke_privs Privileges to be removed
*/
void DB_restrictions::remove(const Access_bitmask revoke_privs) {
  if (m_restrictions != nullptr) {
    for (auto rest_itr = m_restrictions->begin();
         rest_itr != m_restrictions->end();) {
      remove(revoke_privs, rest_itr->second);
      if (rest_itr->second == 0)
        rest_itr = m_restrictions->erase(rest_itr);
      else
        ++rest_itr;
    }
  }
}

/**
  Private function: Remove given set of privs from restricted list

  @param [in]  remove_restrictions Restriction to be removed
  @param [out] restriction_mask    Resultant value to be returned
*/
void DB_restrictions::remove(const Access_bitmask remove_restrictions,
                             Access_bitmask &restriction_mask) const noexcept {
  const Access_bitmask mask = restriction_mask ^ remove_restrictions;
  restriction_mask = restriction_mask & mask;
}

/**
  Get restricted access information for given database.

  @param [in]  db_name Database information
  @param [out] access  Restricted access

  @returns Status of search
    @retval true  Entry found.
    @retval false Entry not found. Do not rely on access.
*/
bool DB_restrictions::find(const std::string &db_name,
                           Access_bitmask &access) const {
  if (!is_empty()) {
    const auto &itr = m_restrictions->find(db_name);
    if (itr != m_restrictions->end()) {
      access = itr->second;
      return true;
    }
  }
  return false;
}

/**
  Status function to check if restriction list is empty.
  m_restrictions may not be initialized yet, that also indicates that the
  restrictions are empty.
*/
bool DB_restrictions::is_empty() const {
  return (m_restrictions != nullptr ? m_restrictions->empty() : true);
}

/** Status function to get number of entries in restriction list */
size_t DB_restrictions::size() const {
  if (is_empty()) return 0;
  return m_restrictions->size();
}

/** Clear restriction list */
void DB_restrictions::clear() {
  delete m_restrictions;
  m_restrictions = nullptr;
}

/**
  Serializer. Converts restriction list to JSON format.

  This is used while storing restriction list in ACL table.
*/
void DB_restrictions::get_as_json(Json_array &restrictions_array) const {
  assert(m_restrictions != nullptr);
  for (auto &revocations_itr : *m_restrictions) {
    Json_array privileges;
    Json_object revocations_obj;
    Json_string db_name(revocations_itr.first.c_str());
    revocations_obj.add_clone(consts::Database, &db_name);
    Access_bitmask revokes_mask = revocations_itr.second;
    while (revokes_mask != 0) {
      Json_string priv_str(get_one_priv(revokes_mask));
      privileges.append_clone(&priv_str);
    }
    revocations_obj.add_clone(consts::Privileges, &privileges);
    restrictions_array.append_clone(&revocations_obj);
  }
}

/**
  Compare is two restriction list for given privileges

  @param [in] other DB_restrictions to compare against
  @param [in] access Privilege filter

  @returns Comparison result of two restriction lists
    @retval false Other restriction list has less or equal restrictions
    @retval true  Otherwise
*/
bool DB_restrictions::has_more_restrictions(const DB_restrictions &other,
                                            Access_bitmask access) const {
  if (other.size() == 0) return false;
  access = access & DB_ACLS;
  db_revocations other_revocations = other.get();

  for (const auto &entry : other_revocations) {
    /* We are only interested in privileges represented by access */
    Access_bitmask other_mask = entry.second & access;
    if (other_mask) {
      Access_bitmask self_restrictions;
      /*
        If there exists a restriction that other list has but *this does not,
        it means, other list has at least one more restriction than *this.
        Stop here and return true.
      */
      if (!find(entry.first, self_restrictions)) return true;
      /*
        If both lists have restrictions but if *this does not have restriction
        for privilege filter while other list does, it means, other list has
        more restrictions than *this. Stop here and return true.
      */
      if ((other_mask & self_restrictions) != other_mask) return true;
    }
  }
  /*
    If we are here it means:
    1. We looked at all restrictions from other restriction list
    2. For each entry, *this has restrictions that is either equal
       or greater than other.
    3. Extra restrictions in *this do not matter because those are
       not present in other list to begin with.
  */
  return false;
}

/**
  A factory method that creates objects from Restrictions_aggregator
  hierarchy.

  Creates an object if --partial_revokes system variable is ON.
  It also records the CURRENT_USER in the binlog so that partial_revokes can
  be executed on slave with context of current user

  @param [in,out] thd       Thread handle
  @param [in]     acl_user  Grantee's info from ACL Cache
  @param [in]     db        Database name if it is DB level operation
  @param [in]     rights    access specified in the SQL statement
  @param [in]     is_grant_revoke_all_on_db  flag that indicates if the
                            REVOKE/GRANT ALL was executed on a DB
  @returns A restriction aggregator object
  @retval  nullptr if partial_revokes system variable is OFF
*/
std::unique_ptr<Restrictions_aggregator>
Restrictions_aggregator_factory::create(THD *thd, const ACL_USER *acl_user,
                                        const char *db,
                                        const Access_bitmask rights,
                                        bool is_grant_revoke_all_on_db) {
  std::unique_ptr<Restrictions_aggregator> aggregator = nullptr;
  /*
    Create aggregator only if partial_revokes system variable is ON, and
    user exists as user would have been granted global privileges already
  */
  if (mysqld_partial_revokes() == false || acl_user == nullptr)
    return aggregator;
  const enum_sql_command command = thd->lex->sql_command;
  const Security_context *security_context = thd->security_context();
  /* Fetch grantor Auth_id */
  const Auth_id grantor = fetch_grantor(security_context);
  /* Fetch grantee Auth_id */
  const Auth_id grantee = fetch_grantee(acl_user);
  /* Fetch access information of grantor */
  Access_bitmask grantor_global_access;
  Restrictions grantor_restrictions;
  fetch_grantor_access(security_context, db, grantor_global_access,
                       grantor_restrictions);
  /* Fetch access information of grantee */
  Access_bitmask grantee_global_access;
  Restrictions grantee_restrictions;
  fetch_grantee_access(acl_user, grantee_global_access, grantee_restrictions);
  if (db) {
    /* Fetch DB privileges of grantor */
    Access_bitmask grantor_db_access = fetch_grantor_db_access(thd, db);
    /* Fetch DB privileges of grantee */
    Access_bitmask grantee_db_access =
        fetch_grantee_db_access(thd, acl_user, db);
    switch (command) {
      case SQLCOM_GRANT:
        aggregator.reset(new DB_restrictions_aggregator_db_grant(
            grantor, grantee, grantor_global_access, grantee_global_access,
            grantor_db_access, grantee_db_access, grantor_restrictions.db(),
            grantee_restrictions.db(), rights, is_grant_revoke_all_on_db, db,
            std::move(security_context)));
        break;
      case SQLCOM_REVOKE:
        aggregator.reset(new DB_restrictions_aggregator_db_revoke(
            grantor, grantee, grantor_global_access, grantee_global_access,
            grantor_db_access, grantee_db_access, grantor_restrictions.db(),
            grantee_restrictions.db(), rights, is_grant_revoke_all_on_db, db,
            std::move(security_context)));
        break;
      default:
        assert(false);
        break;
    }
  } else {
    switch (command) {
      case SQLCOM_GRANT:
        aggregator.reset(new DB_restrictions_aggregator_global_grant(
            grantor, grantee, grantor_global_access, grantee_global_access,
            grantor_restrictions.db(), grantee_restrictions.db(), rights,
            std::move(security_context)));
        break;
      case SQLCOM_REVOKE:
        aggregator.reset(new DB_restrictions_aggregator_global_revoke(
            grantor, grantee, grantor_global_access, grantee_global_access,
            grantor_restrictions.db(), grantee_restrictions.db(), rights,
            std::move(security_context)));
        break;
      case SQLCOM_REVOKE_ALL:
        aggregator.reset(new DB_restrictions_aggregator_global_revoke_all(
            grantor, grantee, grantor_global_access, grantee_global_access,
            grantor_restrictions.db(), grantee_restrictions.db(), rights,
            std::move(security_context)));
        break;
      default:
        assert(false);
        break;
    }
  }
  assert(aggregator);
  return aggregator;
}

std::unique_ptr<Restrictions_aggregator>
Restrictions_aggregator_factory::create(
    const Auth_id &grantor, const Auth_id &grantee,
    const Access_bitmask grantor_access, const Access_bitmask grantee_access,
    const DB_restrictions &grantor_db_restrictions,
    const DB_restrictions &grantee_db_restrictions,
    const Access_bitmask required_access, Db_access_map *db_map) {
  std::unique_ptr<Restrictions_aggregator> aggregator = nullptr;
  /* Create aggregator only if partial_revokes system variable is ON */
  if (mysqld_partial_revokes() == false) return aggregator;
  /* As of now, only caters global grants */
  aggregator.reset(new DB_restrictions_aggregator_set_role(
      grantor, grantee, grantor_access, grantee_access, grantor_db_restrictions,
      grantee_db_restrictions, required_access, db_map));
  assert(aggregator);
  return aggregator;
}

/**
  Returns the grantor user name and host id.
  @param  [in]  sctx Security context

  @returns Grantor's user name and host info
*/
Auth_id Restrictions_aggregator_factory::fetch_grantor(
    const Security_context *sctx) {
  LEX_CSTRING grantor_user, grantor_host;
  // Fetch the grantor auth_id from security context on master
  grantor_user = sctx->priv_user();
  grantor_host = sctx->priv_host();
  const Auth_id grantor(grantor_user, grantor_host);
  return grantor;
}

/**
  Returns the grantee's user name and host info.

  @param [in]  acl_user user handle from ACL_Cache

  @returns Grantee's user name and host info
*/
Auth_id Restrictions_aggregator_factory::fetch_grantee(
    const ACL_USER *acl_user) {
  // Fetch the grantee auth_id
  const std::string grantee_user(acl_user->user != nullptr ? acl_user->user
                                                           : "");
  const std::string grantee_host(
      acl_user->host.get_host() != nullptr ? acl_user->host.get_host() : "");
  const Auth_id grantee(grantee_user, grantee_host);
  return grantee;
}

/**
  Returns the privileges granted on the DB to the grantor

  @param  [in]  thd Thread handle
  @param  [in]  db  Database name for which privileges to be fetched.

  @returns privilege access to the grantor on the specified database
*/
Access_bitmask Restrictions_aggregator_factory::fetch_grantor_db_access(
    THD *thd, const char *db) {
  LEX_CSTRING db_str;
  db_str.str = db;
  db_str.length = sizeof(db);
  return thd->security_context()->db_acl(db_str, false);
}

/**
  Returns the privileges granted on the DB to the grantee

  @param  [in, out] thd   THD handle
  @param  [in]  acl_user  user handle from ACL_Cache
  @param  [in]  db        Database name for which privileges to be fetched.

  @returns privilege access to the grantee on the specified database
*/
Access_bitmask Restrictions_aggregator_factory::fetch_grantee_db_access(
    THD *thd, const ACL_USER *acl_user, const char *db) {
  return acl_get(thd, acl_user->host.get_host(), acl_user->host.get_host(),
                 (acl_user->user ? acl_user->user : ""), db, false);
}

/**
  Returns the privileges and restrictions:

  @param  [in]  sctx          security context of current user
  @param  [in]  db            Database name for which privileges to be fetched.
  @param  [out] global_access fetch grantor's global access
  @param  [out] restrictions  fetch grantor's restrictions
*/
void Restrictions_aggregator_factory::fetch_grantor_access(
    const Security_context *sctx, const char *db, Access_bitmask &global_access,
    Restrictions &restrictions) {
  /* Fetch global privileges of current user */
  global_access = sctx->master_access(db ? db : "");
  /* Fetch restrictions of current user */
  restrictions = sctx->restrictions();
}

void Restrictions_aggregator_factory::fetch_grantee_access(
    const ACL_USER *grantee, Access_bitmask &global_access,
    Restrictions &restrictions) {
  assert(assert_acl_cache_read_lock(current_thd));
  global_access = grantee->access;
  restrictions = acl_restrictions->find_restrictions(grantee);
}

/**
  Constructor.

  @param [in] grantor               Grantor account information
  @param [in] grantee               Grantee account information
  @param [in] grantor_global_access Global static privileges of grantor
  @param [in] grantee_global_access Global static privileges of grantee
  @param [in] requested_access      Privileges being granted/revoked through
                                    current statement
*/
Restrictions_aggregator::Restrictions_aggregator(
    const Auth_id &grantor, const Auth_id grantee,
    const Access_bitmask grantor_global_access,
    const Access_bitmask grantee_global_access,
    const Access_bitmask requested_access)
    : m_grantor(grantor),
      m_grantee(grantee),
      m_grantor_global_access(grantor_global_access),
      m_grantee_global_access(grantee_global_access),
      m_requested_access(requested_access),
      m_status(Status::No_op) {
  // partial_revokes system variable is ON
  assert(mysqld_partial_revokes());
}

/** Destructor */
Restrictions_aggregator::~Restrictions_aggregator() = default;

/**
  Constructor for database level restrictions aggregator

  Database restrictions will be fetched from global cache.
  Assumption: ACL cache is locked - at least in shared mode.

  @param [in] grantor               Grantor information
  @param [in] grantee               Grantee information
  @param [in] grantor_global_access Static global privileges of grantor
  @param [in] grantee_global_access Static global privileges of grantee
  @param [in] grantor_db_restrictions DB_restrictions of grantor
  @param [in] grantee_db_restrictions DB_restrictions of grantee
  @param [in] requested_access      Privileges being granted/revoked through
                                    current statement
  @param [in] sctx                  Security_context of current user.
                                    Default value is nullptr
*/
DB_restrictions_aggregator::DB_restrictions_aggregator(
    const Auth_id &grantor, const Auth_id grantee,
    const Access_bitmask grantor_global_access,
    const Access_bitmask grantee_global_access,
    const DB_restrictions &grantor_db_restrictions,
    const DB_restrictions &grantee_db_restrictions,
    const Access_bitmask requested_access, const Security_context *sctx)
    : Restrictions_aggregator(grantor, grantee, grantor_global_access,
                              grantee_global_access, requested_access),
      m_grantor_rl(grantor_db_restrictions),
      m_grantee_rl(grantee_db_restrictions),
      m_sctx(sctx) {}

/**
  Driver function to aggregate restriction lists

  Validate first and then aggregate the restrictionss from combinations of
  grantor & grantee's restrictions, global access and grantee access.

  We also perform dynamic cast here once and call method of respective derived
  classes. This way, derived classes do not have to override aggregate and
  perform similar dynamic casting before proceeding.

  @param [out] restrictions Aggreatated restrictions for grantee

  @returns status of aggregation process
    @retval false Success
    @retval true  Failure. Error would have been raised.
*/
bool DB_restrictions_aggregator::generate(Abstract_restrictions &restrictions) {
  DB_restrictions *db_restrictions =
      dynamic_cast<DB_restrictions *>(&restrictions);
  assert(db_restrictions);
  m_status = validate();
  if (m_status == Status::Validated) {
    aggregate(*db_restrictions);
    return false;
  } else if (m_status == Status::No_op)
    return false;
  return true;
}

/**
  Get list of privileges that are not restricted through restriction list

  @param [out] rights Bitmask of privileges to be processed further

  @returns Any more privilegs remaining?
    @retval false No privileges to be processed further
    @retval true  Either restricted privileges were removed or
                  nothing needs to be filtered
*/
bool DB_restrictions_aggregator::find_if_require_next_level_operation(
    Access_bitmask &rights) const {
  if (m_privs_not_processed) {
    rights = m_privs_not_processed;
    return true;
  } else if (m_status == Status::No_op) {
    rights = m_requested_access;
    return true;
  } else
    return false;
}

/**
  Check possible descrepancy between DB access being granted and existing
  restrictions.

  For a given user account, if a privilege is present in:
  1. Restriction list
  2. List of DB privileges
  AND if server is running with --partial_revokes option, any further attempt
  to modify user's grant should be prevented. Such a case can occur in one of
  the following cases:
  A. DB Restriction is created, --partial_revokes is set to OFF, DB grant is
     created, --partial_revokes is set to ON again.
  B. DB Restriction is created, mysql.db table is modified with INSERT query.

  @param [in] grantee_db_access    Database access to be granted
  @param [in] grantee_restrictions Existing restriction
  @param [in] db_name              Database information

  @returns Collision exists or not
    @retval false No collision detected
    @retval true  Collision detected. Error raised.
*/
bool DB_restrictions_aggregator::check_db_access_and_restrictions_collision(
    const Access_bitmask grantee_db_access,
    const Access_bitmask grantee_restrictions,
    const std::string &db_name) noexcept {
  if (grantee_db_access & grantee_restrictions) {
    // find out the least significant bit(lsb)
    const Access_bitmask lsb = grantee_db_access & ~(grantee_db_access - 1);
    // find out the position of the lsb
    size_t index = static_cast<size_t>(std::log2(lsb));
    my_error(ER_PARTIAL_REVOKE_AND_DB_GRANT_BOTH_EXISTS, MYF(0),
             global_acls_vector[index].c_str(), db_name.c_str());
    m_status = Status::Error;
    return true;
  }
  return false;
}

/**
  Set privileges that needs to be processed further.

  These privileges are not restricted through revocations. So caller
  can safely proceed with further operations

  @param [in] requested_access  Privilege bitmask to be checked
  @param [in] restrictions_mask Confirmed restrictions
*/
void DB_restrictions_aggregator::set_if_db_level_operation(
    const Access_bitmask requested_access,
    const Access_bitmask restrictions_mask) noexcept {
  /*
    DB level privilegss are filtered as following -
    1. If restriction_mask is 0 then there is nothing to filter from the
       requested_access.
    2. If restriction_mask has some access restrictions then remove only
       those access in the requested_access.
    For instance :
    m_privs_not_processed(1100) =
      requested_access(1100) - restrictions_mask(0000)
    m_privs_not_processed(0100) =
      requested_access(1100) - restrictions_mask(1011)
  */
  m_privs_not_processed = (~restrictions_mask & requested_access);
}

/**
  A helper method that aggregates the restrictions for global_grant and set_role
  operations since both are similar in nature.
  Generates DB_restrictions based on the requested access,
  grantor and grantee's DB_restrictions in the ACL cache.
  - If grantor has restrictions
    - Retain Grantee's restrictions if it has more privileges than grantor
    - If grantee has restrictions than aggregate them
    - If grantee does not have restrictions keep only those restrictions
      of grantor for which there is no DB level access to grantee.
  - else if grantee has restrictions
    - Remove the restrictions on which global grant is requested.

  @param  [in]    sql_op        SQL statement type for which aggregation
                                is to be done.
  @param  [in]    db_map        DB_access_map used to fetch grantee's db access
                                for SET ROLE
  @param  [out]   restrictions  Fills the parameter with the generated
                                DB_restrictions.

*/
void DB_restrictions_aggregator::aggregate_restrictions(
    SQL_OP sql_op, const Db_access_map *db_map, DB_restrictions &restrictions) {
  if (!m_grantor_rl.is_empty()) {
    if (test_all_bits(m_grantee_global_access, m_requested_access) &&
        m_grantee_rl.is_empty()) {
      /*
        If grantee does not have any restrictions but grantor has, that
        implies grantee already has more privileges, therefore don't do
        any further processing. Keep the empty restrictions list.
      */
      restrictions = m_grantee_rl;
    } else {
      Access_bitmask restrictions_mask;
      DB_restrictions grantee_rl = m_grantee_rl;
      for (const auto &grantor_rl_itr : m_grantor_rl()) {
        Access_bitmask grantee_revokes;
        if (grantee_rl.find(grantor_rl_itr.first, grantee_revokes)) {
          /*
            If grantor and grantee both have restriction list then aggregration
            of them will be OR operation of the following :
             - Grantee's global privileges negates grantor's restrictions
               (consider relevant restrictions in context of privileges
                specified in statement i.e. requested_access)
             - Grantor and Grantee both might have same restrictions as well
               as global privileges. Remaining restrictions after negation
               by Grantor and Grantee's global privileges.
          */
          Access_bitmask rm1 = ((grantor_rl_itr.second & m_requested_access) &
                                ~m_grantee_global_access);
          Access_bitmask rm2 =
              (grantor_rl_itr.second & m_grantee_global_access &
               grantee_revokes & m_grantor_global_access);
          restrictions_mask = rm1 | rm2;

          /*
            Retain restrictions that are not applicable w.r.t.
            requested operation in the restrictions list.
            For instance :
            Grantee may have restrictions SELECT, INSERT on db,
            the GRANT statement has only SELECT on db, then
            retain INSERT on db.
          */
          Access_bitmask remove_mask = grantee_revokes & m_requested_access;
          grantee_rl.remove(grantor_rl_itr.first, remove_mask);
          /* Remove the schemas without any restrictions remaining */
          grantee_rl.remove(0);
        } else {
          /*
            Filter restrictions relevant to access from the grantor's
            restriction list
          */
          restrictions_mask = grantor_rl_itr.second & m_requested_access;

          Access_bitmask grantee_db_access = 0;
          if (sql_op == SQL_OP::GLOBAL_GRANT) {
            grantee_db_access = get_grantee_db_access(grantor_rl_itr.first);
          } else if (sql_op == SQL_OP::SET_ROLE && db_map != nullptr) {
            const std::string key(grantor_rl_itr.first.c_str(),
                                  grantor_rl_itr.first.length());
            auto it = db_map->find(key);
            grantee_db_access = (it != db_map->end()) ? it->second : 0L;
          }

          /*
            Remove the restrictions from grantor's restrictions for which
            grantee already has DB level privileges.
          */
          restrictions_mask &= ~grantee_db_access;

          /* Remove any global privilege that grantee already has */
          restrictions_mask &= ~m_grantee_global_access;
        }
        if (restrictions_mask)
          restrictions.add(grantor_rl_itr.first, restrictions_mask);
      }
      /*
       We are left with two types of grantee's restrictions to process.
       (1) Grantee's schemas that did not match grantor's restrictions but
           restrictions on these schemas could be negated due to grantor's
           global privileges.
       (2) Restrictions in grantee's schemas which are not relevant with
           the requested access. Grantee must retain these restrictions.
      */
      if (!grantee_rl.is_empty()) {
        for (auto &grantee_rl_itr : grantee_rl()) {
          grantee_rl_itr.second &=
              ~(m_grantor_global_access & m_requested_access);
        }
        /* Remove the schemas without any restrictions remaining */
        grantee_rl.remove(0);
        restrictions.add(grantee_rl);
      }
    }
  } else if (!m_grantee_rl.is_empty()) {
    restrictions = m_grantee_rl;
    for (auto &grantee_rl_itr : restrictions()) {
      restrictions.remove(grantee_rl_itr.first.c_str(), m_requested_access);
      set_if_db_level_operation(m_requested_access, grantee_rl_itr.second);
    }

    /*
      Now, remove the databases with no restrictions without invalidating
      the internal container of DB_restrictions
    */
    restrictions.remove(0);
  }
}

/**
  Fetches the grantee's DB access on the specified DB
  If security context of current user exists and has some active roles then
  probe the security context since current user must be grantee.
  Otherwise, probe the usual ACL Cache.

  @param [in] db_name   Database name for which we need to fetch the DB level
                        access.
  @returns DB level access.
*/
Access_bitmask DB_restrictions_aggregator::get_grantee_db_access(
    const std::string &db_name) const {
  return Security_context::check_db_level_access(
      current_thd, m_sctx, m_grantee.host().c_str(),
      m_sctx ? m_sctx->ip().str : m_grantee.host().c_str(),
      m_grantee.user().c_str(), db_name.c_str(), db_name.length());
}

/**
  Fetches the grantee's DB access on the specified DB
  If security context of current user exists and has some active roles then
  probe the security context since current user must be grantee.
  Otherwise, do not modify the access argument.

  @param [in]  db_name   Database name for which we need to fetch the DB level
                         access.
  @param [out] access    Access on the specified DB.
*/
void DB_restrictions_aggregator::get_grantee_db_access(
    const std::string &db_name, Access_bitmask &access) const {
  if (m_sctx && m_sctx->get_num_active_roles() > 0) {
    const LEX_CSTRING db = {db_name.c_str(), db_name.length()};
    access = m_sctx->db_acl(db, false);
  }
}

/**
  DB_restrictions_aggregator_set_role constructor

  @param  [in]  grantor Grantor's user name and host info
  @param  [in]  grantee Grantee's user name and host info
  @param  [in]  grantor_global_access Current privileges mask of grantor
  @param  [in]  grantee_global_access Current privileges mask of grantee
  @param  [in]  grantor_db_restrictions Restrictions on the grantor
  @param  [in]  grantee_db_restrictions Restrictions on the grantee
  @param  [in]  requested_access  Requested privileges mask in the SQL statement
  @param  [in,out]  db_map  Grantee's db_accees_map to fetch/update db access
*/
DB_restrictions_aggregator_set_role::DB_restrictions_aggregator_set_role(
    const Auth_id &grantor, const Auth_id grantee,
    const Access_bitmask grantor_global_access,
    const Access_bitmask grantee_global_access,
    const DB_restrictions &grantor_db_restrictions,
    const DB_restrictions &grantee_db_restrictions,
    const Access_bitmask requested_access, Db_access_map *db_map)
    : DB_restrictions_aggregator(grantor, grantee, grantor_global_access,
                                 grantee_global_access, grantor_db_restrictions,
                                 grantee_db_restrictions, requested_access,
                                 nullptr),
      m_db_map(db_map) {}
/**
  Evaluates the restrictions list of grantor and grantee, as well as requested
  privilege.
  -  Checks possible descrepancy between DB access being granted and
     existing restrictions.

  @returns Status   Moves the object in the appropriate status.
                    For instance :
                    - Validated, if validation was performed successfully
                    - NO_op, if there is no aggregation of privileges required.
                    - Error, if an error encountered
*/
Restrictions_aggregator::Status
DB_restrictions_aggregator_set_role::validate() {
  /*
    It must be only Global Grant, if grantor and grantee both do not have
    any restrictions attached.
  */
  if (m_grantor_rl.is_empty() && m_grantee_rl.is_empty())
    return (m_status = Status::No_op);

  return (m_status = Status::Validated);
}

/**
  Generates DB_restrictions based on the requested access, grantor and
  grantee's DB_restrictions in the ACL cache.
  - If grantor has restrictions
    - Retain Grantee's restrictions if it has more privileges than grantor
    - If grantee has restrictions than aggregate them
    - If grantee does not have restrictions keep only those restrictions
      of grantor for which there is no DB level access to grantee.
  - else if grantee has restrictions
    - Remove the restrictions on which global grant is requested.

  @param  [out]  db_restrictions  Fills the parameter with the generated
                                  DB_restrictions
*/
void DB_restrictions_aggregator_set_role::aggregate(
    DB_restrictions &db_restrictions) {
  assert(m_status == Status::Validated);

  if (!m_grantee_rl.is_empty()) {
    /*
      At this point, we already have aggregated DB privileges and grantee's
      restrictions. Therefore, negate the restrictions with the DB privileges.
      In other words remove grantee's restrictions and corresponding grantor's
      privilege.
    */
    Access_bitmask restrictions, privileges;
    for (auto it = m_db_map->begin(); it != m_db_map->end();) {
      privileges = it->second;
      if (m_grantee_rl.find(it->first, restrictions)) {
        m_grantee_rl.remove(it->first, privileges);
        privileges = (restrictions ^ privileges) & privileges;
        if (privileges == 0)
          // If DB does not have any privilege then remove it from db_map
          it = m_db_map->erase(it);
        else {
          // Keep the remaining privileges intact in the db_map
          it->second = privileges;
          ++it;
        }
      } else {
        ++it;
      }
    }
  }
  aggregate_restrictions(SQL_OP::SET_ROLE, m_db_map, db_restrictions);
  m_status = Status::Aggregated;
}

/**
  DB_restrictions_aggregator_global_grant constructor

  @param  [in]  grantor Grantor's user name and host info
  @param  [in]  grantee Grantee's user name and host info
  @param  [in]  grantor_global_access Current privileges mask of grantor
  @param  [in]  grantee_global_access Current privileges mask of grantee
  @param  [in]  grantor_db_restrictions DB_restrictions of grantor
  @param  [in]  grantee_db_restrictions DB_restrictions of grantee
  @param  [in]  requested_access  Requested privileges mask in the SQL statement
  @param  [in]  sctx Security_context of current user. Default value is nullptr
*/
DB_restrictions_aggregator_global_grant::
    DB_restrictions_aggregator_global_grant(
        const Auth_id &grantor, const Auth_id grantee,
        const Access_bitmask grantor_global_access,
        const Access_bitmask grantee_global_access,
        const DB_restrictions &grantor_db_restrictions,
        const DB_restrictions &grantee_db_restrictions,
        const Access_bitmask requested_access, const Security_context *sctx)
    : DB_restrictions_aggregator(grantor, grantee, grantor_global_access,
                                 grantee_global_access, grantor_db_restrictions,
                                 grantee_db_restrictions, requested_access,
                                 sctx) {}
/**
  Evaluates the restrictions list of grantor and grantee, as well as requested
  privilege.
  -  Checks possible descrepancy between DB access being granted and
     existing restrictions.

  @returns Status   Moves the object in the appropriate status.
                    For instance :
                    - Validated, if validation was performed successfully
                    - NO_op, if there is no aggregation of privileges required.
                    - Error, if an error encountered
*/
Restrictions_aggregator::Status
DB_restrictions_aggregator_global_grant::validate() {
  if (!m_grantee_rl.is_empty()) {
    for (auto &grantee_rl_itr : m_grantee_rl()) {
      Access_bitmask grantee_db_access =
          get_grantee_db_access(grantee_rl_itr.first.c_str());
      /*
        Remove the restrictions from grantor's restrictions for which
        grantee already has DB level privileges.
      */
      if (check_db_access_and_restrictions_collision(
              grantee_db_access, grantee_rl_itr.second,
              grantee_rl_itr.first.c_str())) {
        return (m_status = Status::Error);
        break;
      }
    }
  }
  /*
    It must be only Global Grant, if grantor and grantee both does not have any
    restrictions attached.
  */
  if (m_grantor_rl.is_empty() && m_grantee_rl.is_empty())
    return (m_status = Status::No_op);

  return (m_status = Status::Validated);
}

/**
  Generates DB_restrictions based on the requested access, grantor and
  grantee's DB_restrictions in the ACL cache.
  - If grantor has restrictions
    - Retain Grantee's restrictions if it has more privileges than grantor
    - If grantee has restrictions than aggregate them
    - If grantee does not have restrictions keep only those restrictions
      of grantor for which there is no DB level access to grantee.
  - else if grantee has restrictions
    - Remove the restrictions on which global grant is requested.

  @param  [out]  restrictions  Fills the parameter with the generated
                               DB_restrictions
*/
void DB_restrictions_aggregator_global_grant::aggregate(
    DB_restrictions &restrictions) {
  assert(m_status == Status::Validated);
  aggregate_restrictions(SQL_OP::GLOBAL_GRANT, nullptr, restrictions);
  m_status = Status::Aggregated;
}

/**
  DB_restrictions_aggregator_global_revoke constructor

  @param  [in]  grantor Grantor's user name and host info
  @param  [in]  grantee Grantee's user name and host info
  @param  [in]  grantor_global_access Current privileges mask of grantor
  @param  [in]  grantee_global_access Current privileges mask of grantee
  @param  [in]  grantor_db_restrictions DB_restrictions of grantor
  @param  [in]  grantee_db_restrictions DB_restrictions of grantee
  @param  [in]  requested_access  Requested privileges mask in the SQL statement
  @param  [in]  sctx  Security_context of current user. Default value is nullptr
*/
DB_restrictions_aggregator_global_revoke::
    DB_restrictions_aggregator_global_revoke(
        const Auth_id &grantor, const Auth_id grantee,
        const Access_bitmask grantor_global_access,
        const Access_bitmask grantee_global_access,
        const DB_restrictions &grantor_db_restrictions,
        const DB_restrictions &grantee_db_restrictions,
        const Access_bitmask requested_access, const Security_context *sctx)
    : DB_restrictions_aggregator(grantor, grantee, grantor_global_access,
                                 grantee_global_access, grantor_db_restrictions,
                                 grantee_db_restrictions, requested_access,
                                 sctx) {}

/**
  Evaluates the restrictions list of grantor and grantee, as well as requested
  privilege.

  @returns Status   Moves the object in the appropriate status.
                    For instance :
                    - Validated, if validation was performed successfully
                    - NO_op, if there is no aggregation of privileges required.
                    - Error, if an error encountered
*/
Restrictions_aggregator::Status
DB_restrictions_aggregator_global_revoke::validate() {
  if (m_requested_access == m_grantee_global_access &&
      m_grantor_rl == m_grantee_rl) {
    /*
      If requested access is same as grantee's global access and grantor as
      well as grantee both have same restrictions then it must be pure global
      revoke from grantee.
    */
    return (m_status = Status::No_op);
  } else if (!m_grantee_rl.is_empty()) {
    return validate_if_grantee_rl_not_empty();
  } else if (!m_grantor_rl.is_empty()) {
    for (auto &itr : m_grantor_rl.get()) {
      /*
        Grantor cannot revoke the privileges from grantee which are in
        former's restriction list.
      */
      if (test_all_bits(itr.second, m_requested_access)) {
        my_error(ER_DB_ACCESS_DENIED, MYF(0), m_grantor.user().c_str(),
                 m_grantor.host().c_str(),
                 m_grantor_rl.get().begin()->first.c_str());
        return (m_status = Status::Error);
      }
    }
  }
  return (m_status = Status::Validated);
}

/**
  - If grantee has the restriction list
    - Remove only requested restrictions from restriction_list

  @param  [out]  restrictions  Fills the parater with the aggregated
                               DB_restrictions
*/
void DB_restrictions_aggregator_global_revoke::aggregate(
    DB_restrictions &restrictions) {
  assert(m_status == Status::Validated);
  restrictions = m_grantee_rl;
  if (test_all_bits(m_grantee_global_access, m_requested_access)) {
    restrictions.remove(m_requested_access);
  }
  m_status = Status::Aggregated;
}

/**
  If grantor's restrictions_list is not empty, check that grantee has matching
  restrictions

  If grantee restrictions_list is not empty then check if grantee has same
  DB level privilege as well as restriction list on the database -
  essentially nullfying the effect of the partial revoke

  @returns  Status  Moves the object in the appropriate status.
                    For instance :
                    - Validated, if validation was performed successfully
                    - NO_op, if there is no aggregation of privileges required.
                    - Error, if an error encountered
*/
Restrictions_aggregator::Status
DB_restrictions_aggregator_global_revoke::validate_if_grantee_rl_not_empty() {
  /*
     Grantor cannot revoke the requested privileges from grantee which are
     in grantor restriction list but not grantees restriction list.
  */
  if (!m_grantor_rl.is_empty()) {
    for (auto &grantor_rl_itr : m_grantor_rl.get()) {
      if (0 != (grantor_rl_itr.second & m_requested_access)) {
        Access_bitmask grantee_revokes;
        if (!m_grantee_rl.find(grantor_rl_itr.first.c_str(), grantee_revokes)) {
          /*
            Case 1: Grantee does not have restrictions on a schema where grantor
            has some restrictions
          */
          my_error(ER_DB_ACCESS_DENIED, MYF(0), m_grantor.user().c_str(),
                   m_grantor.host().c_str(), grantor_rl_itr.first.c_str());
          return (m_status = Status::Error);
        } else {
          /*
            Case 2: Both, grantor and grantee have some restrictions on a
            schema. However, grantor has some more restrictions than grantee.
          */
          if (0 !=
              (grantor_rl_itr.second & ~grantee_revokes & m_requested_access)) {
            my_error(ER_DB_ACCESS_DENIED, MYF(0), m_grantor.user().c_str(),
                     m_grantor.host().c_str(), grantor_rl_itr.first.c_str());
            return (m_status = Status::Error);
          }
        }
      }
    }
  }

  /*
    Verify DB level privilege consistency

    Enforce that grantee does not have both, restrictions and DB level
    grants involving same set of privileges for any given database.
  */
  for (auto &grantee_rl_itr : m_grantee_rl.get()) {
    Access_bitmask grantee_db_access =
        get_grantee_db_access(grantee_rl_itr.first.c_str());
    if (check_db_access_and_restrictions_collision(
            grantee_db_access, grantee_rl_itr.second,
            grantee_rl_itr.first.c_str())) {
      return (m_status = Status::Error);
    }
  }
  return (m_status = Status::Validated);
}

/**
  DB_restrictions_aggregator_global_revoke_all constructor

  @param  [in]  grantor Grantor's user name and host info
  @param  [in]  grantee Grantee's user name and host info
  @param  [in]  grantor_global_access Current privileges mask of grantor
  @param  [in]  grantee_global_access Current privileges mask of grantee
  @param  [in]  grantor_db_restrictions DB_restrictions of grantor
  @param  [in]  grantee_db_restrictions DB_restrictions of grantee
  @param  [in]  requested_access  Requested privileges mask in the SQL statement
  @param  [in]  sctx  Security_context of current user. Default value is nullptr
*/
DB_restrictions_aggregator_global_revoke_all::
    DB_restrictions_aggregator_global_revoke_all(
        const Auth_id &grantor, const Auth_id grantee,
        const Access_bitmask grantor_global_access,
        const Access_bitmask grantee_global_access,
        const DB_restrictions &grantor_db_restrictions,
        const DB_restrictions &grantee_db_restrictions,
        const Access_bitmask requested_access, const Security_context *sctx)
    : DB_restrictions_aggregator_global_revoke(
          grantor, grantee, grantor_global_access, grantee_global_access,
          grantor_db_restrictions, grantee_db_restrictions, requested_access,
          sctx) {}

/** Validate restriction list for REVOKE ALL */
Restrictions_aggregator::Status
DB_restrictions_aggregator_global_revoke_all::validate() {
  if (m_grantor_rl == m_grantee_rl) {
    return (m_status = Status::No_op);
  } else if (!m_grantee_rl.is_empty()) {
    return validate_if_grantee_rl_not_empty();
  } else if (!m_grantor_rl.is_empty()) {
    for (auto &itr : m_grantor_rl.get()) {
      /*
        Grantor cannot revoke the privileges from grantee which are in
        former's restriction list.
      */
      if (test_all_bits(m_requested_access, itr.second)) {
        my_error(ER_DB_ACCESS_DENIED, MYF(0), m_grantor.user().c_str(),
                 m_grantor.host().c_str(),
                 m_grantor_rl.get().begin()->first.c_str());
        return (m_status = Status::Error);
      }
    }
  }
  return (m_status = Status::Validated);
}

/**
  Clear all the restrictions and changes the status of object to aggregated

  @param  [in,out]  restrictions  Clears the DB_restrictions
*/
void DB_restrictions_aggregator_global_revoke_all::aggregate(
    DB_restrictions &restrictions) {
  assert(m_status == Status::Validated);
  restrictions.clear();
  m_status = Status::Aggregated;
}

/**
  Constructor.

  @param [in] grantor                 Grantor information
  @param [in] grantee                 Grantee information
  @param [in] grantor_global_access   Global static access of grantor
  @param [in] grantee_global_access   Global static access of grantee
  @param [in] grantor_db_access       Database access of grantor
  @param [in] grantee_db_access       Database access of grantee
  @param [in] grantor_db_restrictions DB_restrictions of grantor
  @param [in] grantee_db_restrictions DB_restrictions of grantee
  @param [in] requested_access        Requested privileges to be granted
  @param [in] is_grant_all            Flag for GRANT ALL
  @param [in] db_name                 Database information
  @param [in]  sctx  Security_context of current user. Default value is nullptr
*/
DB_restrictions_aggregator_db_grant::DB_restrictions_aggregator_db_grant(
    const Auth_id &grantor, const Auth_id grantee,
    const Access_bitmask grantor_global_access,
    const Access_bitmask grantee_global_access,
    const Access_bitmask grantor_db_access,
    const Access_bitmask grantee_db_access,
    const DB_restrictions &grantor_db_restrictions,
    const DB_restrictions &grantee_db_restrictions,
    const Access_bitmask requested_access, bool is_grant_all,
    const std::string &db_name, const Security_context *sctx)
    : DB_restrictions_aggregator(grantor, grantee, grantor_global_access,
                                 grantee_global_access, grantor_db_restrictions,
                                 grantee_db_restrictions, requested_access,
                                 sctx),
      m_grantor_db_access(grantor_db_access),
      m_grantee_db_access(grantee_db_access),
      m_is_grant_all(is_grant_all),
      m_db_name(db_name) {}

/** Validation function for database level grant statement. */
Restrictions_aggregator::Status
DB_restrictions_aggregator_db_grant::validate() {
  Access_bitmask grantee_part_revokes = 0;
  if (m_grantee_rl.find(m_db_name.c_str(), grantee_part_revokes)) {
    Access_bitmask grantee_db_access = m_grantee_db_access;
    get_grantee_db_access(m_db_name, grantee_db_access);
    /*
      Same DB level privilege and partial revoke cannot co-exist.
      If both are found that means user modified the ACL tables directly.
      Hence, throw error.
    */
    if (check_db_access_and_restrictions_collision(
            grantee_db_access, grantee_part_revokes, m_db_name))
      return (m_status = Status::Error);
  }
  /*
    Now, if grantee does not have any restriction list then it
    must be usual grant on a database.
  */
  if (m_grantee_rl.is_empty()) {
    return (m_status = Status::No_op);
  }

  return (m_status = Status::Validated);
}

/**
  Aggregate restriction lists

  @param [out] restrictions Database restrictions
*/
void DB_restrictions_aggregator_db_grant::aggregate(
    DB_restrictions &restrictions) {
  /*
    1. We are granting database privilege. This implies grantor has ability
       to grant them (i.e. they are not in grantor's database restrictions)
    2. Grantee may or may not have database restrictions
    3. Privileges being granted may be present in grantee's database
       restrictions
    4. If last point is correct, those privileges must be removed. There is
       no need to record corresponding grants because grantee would already
       have it covered through global privileges.
    5. For privileges that are not in grantee's database restrictions, they
       should be recorded in mysql.db table. Hence, we call
       set_if_db_level_operation().
  */
  assert(m_status == Status::Validated);
  restrictions = m_grantee_rl;
  Access_bitmask grantee_db_revokes = 0;

  for (auto &entry : restrictions()) {
    if (m_db_name.compare(entry.first) == 0) {
      grantee_db_revokes = entry.second;
      restrictions.remove(entry.first.c_str(), m_requested_access);
      // Remove the databases with no restrictions
      restrictions.remove(0);
      break;
    }
  }
  set_if_db_level_operation(m_requested_access, grantee_db_revokes);
  m_status = Status::Aggregated;
}

/**
  Constructor.

  @param [in] grantor               Grantor information
  @param [in] grantee               Grantee information
  @param [in] grantor_global_access Global static access of grantor
  @param [in] grantee_global_access Global static access of grantee
  @param [in] grantor_db_access     Database access of grantor
  @param [in] grantee_db_access     Database access of grantee
  @param [in] grantor_db_restrictions DB_restrictions of grantor
  @param [in] grantee_db_restrictions DB_restrictions of grantee
  @param [in] requested_access      Requested privileges to be granted
  @param [in] is_revoke_all         Flag for REVOKE ALL
  @param [in] db_name               Database information
  @param [in] sctx  Security_context of current user. Default value is nullptr
*/
DB_restrictions_aggregator_db_revoke::DB_restrictions_aggregator_db_revoke(
    const Auth_id &grantor, const Auth_id grantee,
    const Access_bitmask grantor_global_access,
    const Access_bitmask grantee_global_access,
    const Access_bitmask grantor_db_access,
    const Access_bitmask grantee_db_access,
    const DB_restrictions &grantor_db_restrictions,
    const DB_restrictions &grantee_db_restrictions,
    const Access_bitmask requested_access, bool is_revoke_all,
    const std::string &db_name, const Security_context *sctx)
    : DB_restrictions_aggregator(grantor, grantee, grantor_global_access,
                                 grantee_global_access, grantor_db_restrictions,
                                 grantee_db_restrictions, requested_access,
                                 sctx),
      m_grantor_db_access(grantor_db_access),
      m_grantee_db_access(grantee_db_access),
      m_is_revoke_all(is_revoke_all),
      m_db_name(db_name) {}

/**  Validation function for database level revoke statement. */
Restrictions_aggregator::Status
DB_restrictions_aggregator_db_revoke::validate() {
  Access_bitmask grantee_part_revokes = 0;
  if (m_grantee_rl.find(m_db_name.c_str(), grantee_part_revokes)) {
    Access_bitmask grantee_db_access = m_grantee_db_access;
    get_grantee_db_access(m_db_name, grantee_db_access);

    /*
      Same DB level privilege and partial revoke cannot co-exist.
      If both are found that means user modified the ACL tables directly.
      Hence, throw error.
    */
    if (check_db_access_and_restrictions_collision(
            grantee_db_access, grantee_part_revokes, m_db_name)) {
      return (m_status = Status::Error);
    }
  }
  /*
    There is nothing for partial revokes to do :
     - Grantee does not have either of the global or DB privilege.
    Therefore, skip aggregating the restrictions list and let next
    level handlers take care of revoke statement.
  */
  if (m_grantee_global_access == 0 && m_grantee_db_access == 0) {
    return (m_status = Status::No_op);
  }
  return (m_status = Status::Validated);
}

/**
  Aggregate restriction lists

  @param [out] restrictions Database restrictions
*/
void DB_restrictions_aggregator_db_revoke::aggregate(
    DB_restrictions &restrictions) {
  assert(m_status == Status::Validated);
  restrictions = m_grantee_rl;
  if ((test_all_bits(m_grantee_db_access, m_requested_access))) {
    /*
      If same db privilege(s) exist at DB level, then there are no new
      restrictions to add. Instead, let the db level grant handle it.
    */
    m_privs_not_processed = m_requested_access;
  } else {
    /*
      Filter out the access for which grantee does not have DB level access but
      the Global level access.
    */
    const Access_bitmask revoke_mask =
        (m_grantee_global_access &
         (m_requested_access & (m_requested_access ^ m_grantee_db_access)));
    if (revoke_mask)  // Create restrictions only if there is a restriction mask
      restrictions.add(m_db_name, revoke_mask);
    /*
      Filter out the access other than restrictions from the request_acess
      to let next level handlers take care of those access.
    */
    m_privs_not_processed = m_requested_access ^ revoke_mask;
  }
  m_status = Status::Aggregated;
}

/**
  Constructor for Restrictions
*/
Restrictions::Restrictions() : m_db_restrictions() {}

/** Destructor */
Restrictions ::~Restrictions() { m_db_restrictions.clear(); }

/**
  Move constructor for Restrictions
*/
Restrictions::Restrictions(Restrictions &&restrictions)
    : m_db_restrictions(restrictions.m_db_restrictions) {}

/** Assignment operator for Restrictions */
Restrictions &Restrictions::operator=(const Restrictions &restrictions) {
  if (this != &restrictions) {
    m_db_restrictions = restrictions.m_db_restrictions;
  }
  return *this;
}

/** Assignment operator for Restrictions */
Restrictions &Restrictions::operator=(Restrictions &&restrictions) {
  if (this != &restrictions) {
    m_db_restrictions = restrictions.m_db_restrictions;
  }
  return *this;
}

/* DB restrictions comparator */
bool Restrictions::has_more_db_restrictions(const Restrictions &other,
                                            Access_bitmask access) {
  return m_db_restrictions.has_more_restrictions(other.db(), access);
}

/** Get database restrictions */
const DB_restrictions &Restrictions::db() const { return m_db_restrictions; }

/** Set given database restrictions */
void Restrictions::set_db(const DB_restrictions &db_restrictions) {
  m_db_restrictions = db_restrictions;
}

/** Clear database restrictions */
void Restrictions::clear_db() { m_db_restrictions.clear(); }

/** Return if restrictions are empty or not */
bool Restrictions::is_empty() const { return m_db_restrictions.is_empty(); }
