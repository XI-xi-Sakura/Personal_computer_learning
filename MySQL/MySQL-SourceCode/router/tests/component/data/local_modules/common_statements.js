var defaults = {
  version_comment: "community",
  account_user: "root",
  metadata_schema_version: [2, 3, 0],
  exec_time: 0.0,
  // array-of-array
  // - server-uuid
  // - hostname
  // - port
  // - state
  // - xport (if available and needed)
  group_replication_members: [],
  port: mysqld.session.port,
  cluster_id: "cluster-specific-id",
  innodb_cluster_name: "test",
  innodb_cluster_replicaset_name: "default",
  use_bootstrap_big_data: false,
  replication_group_members: [],
  innodb_cluster_instances: [
    ["uuid-1", "localhost", "5500"],
    ["uuid-2", "localhost", "5510"],
    ["uuid-3", "localhost", "5520"],
  ],
  innodb_cluster_hosts: [],
  innodb_cluster_user_hosts: [],
  bootstrap_report_host_pattern: ".*",
  account_host_pattern: ".*",
  account_user_pattern: "mysql_router1_[0-9a-z]{7}",
  account_pass_pattern: ".*",
  create_user_warning_count: 0,
  create_user_show_warnings_results: [],

  bootstrap_target_type: "cluster",
  clusterset_present: 0,
  clusterset_target_cluster_id: 0,
  // JSON object describing the clusterset
  // this default is needed for tests that do not use ClusterSets because this
  // data is used in "stmt"
  clusterset_data: {
    "clusterset_id": "clusterset-uuid",
    "clusterset_name": "clusterset-name",
    "this_cluster_id": 0,
    "target_cluster_id": 0,
    "primary_cluster_id": 0,
    "clusters": [
      {
        "primary_node_id": 0,
        "uuid": "cluster-id-1",
        "name": "cluster-name-1",
        "role": "PRIMARY",
        "gr_uuid": "gr-id-1",
        "nodes": [
          {
            "host": "127.0.0.1",
            "classic_port": 11010,
            "http_port": 11011,
            "attributes": "{}",
            "version": "9.0.0"
          },
          {
            "host": "127.0.0.1",
            "classic_port": 11012,
            "http_port": 11013,
            "attributes": "{}",
            "version": "9.0.0"
          }
        ],
        "primary_node_id": 0
      },
      {
        "primary_node_id": 0,
        "uuid": "cluster-id-2",
        "name": "cluster-name-2",
        "role": "SECONDARY",
        "gr_uuid": "gr-id-2",
        "nodes": [
          {
            "host": "127.0.0.1",
            "classic_port": 11014,
            "http_port": 11015,
            "attributes": "{}",
            "version": "9.0.0"
          },
          {
            "host": "127.0.0.1",
            "classic_port": 11016,
            "http_port": 11017,
            "attributes": "{}",
            "version": "9.0.0"
          }
        ],
        "primary_node_id": 0
      }
    ]
  },
  clusterset_simulate_cluster_not_found: 0,

  user_host_pattern: ".*",
  cluster_type: "gr",
  view_id: 1,
  router_id: 1,
  // let the test that uses it set it explicitly, going with some default would
  // mean failures each time the version is bumped up (which we don't even
  // control)
  router_version: "",
  router_rw_classic_port: "",
  router_ro_classic_port: "",
  router_rw_split_classic_port: "",
  router_rw_x_port: "",
  router_ro_x_port: "",
  router_metadata_user: "",
  rest_user_credentials: [],
  version: "8.3.0",  // SELECT @@version;
  router_expected_target_cluster: ".*",
  router_expected_local_cluster: ".*",
  router_options: "",
  client_ssl_mode: "PREFERRED",
  server_ssl_mode: "AS_CLIENT",

  gr_member_state: "ONLINE",
  gr_members_all: 3,
  gr_members_recovering: 0,
  gr_members_online: 3,
  current_instance_attributes: null,
  config_defaults_stored_is_null: 0,
  last_insert_id: 1,

  routing_guidelines: "",  // Use default routing configuration

  router_hostname: "router-host",
  router_options: "",
  router_name: "test_router",
  mrs_router_id: 1,
  mrs_basedir: "",
};

function ensure_type(options, field, expected_type) {
  var current_type = typeof (options[field]);

  var tested_type = (expected_type === "array") ? "object" : expected_type;

  if (current_type !== tested_type)
    throw "expected " + field + " to be a " + expected_type + ", got " +
        current_type;
  if (expected_type === "array") {
    if (!Array.isArray(options[field]))
      throw "expected " + field + " to be a " + expected_type + ", got " +
          current_type;
  }
}

/**
 * get options.
 *
 * - check options are of the type right.
 * - merges default values.
 */
function get_options(options) {
  // let 'options' overwrite the 'defaults'
  options = Object.assign({}, defaults, options);

  ensure_type(options, "version_comment", "string");
  ensure_type(options, "account_user", "string");
  ensure_type(options, "metadata_schema_version", "array");
  ensure_type(options, "exec_time", "number");
  ensure_type(options, "port", "number");

  return options;
}

/**
 * create response for commonly used statements
 *
 * @param {string} stmt_key statement key
 * @param {object} [options] options replacement values for statement text
 *     and response
 * @returns On success, response object, otherwise 'undefined'
 */
exports.get = function(stmt_key, opts) {
  return get_response(stmt_key, get_options(opts));
};

/**
 * get response for a statement_key.
 */
function get_response(stmt_key, options) {
  switch (stmt_key) {
    case "mysql_client_select_version_comment":
      return {
        stmt: "select @@version_comment limit 1",
        exec_time: options["exec_time"],
        result: {
          columns: [{name: "@@version_comment", type: "STRING"}],
          rows: [[options["version_comment"]]]
        }
      };
    case "mysql_client_select_user":
      return {
        stmt: "select USER()",
        result: {
          columns: [{name: "USER()", type: "STRING"}],
          rows: [[options["account_user"]]]
        }
      };
    case "select_port":
      return {
        stmt: "select @@port",
        result: {
          columns: [{name: "@@port", type: "LONG"}],
          rows: [[mysqld.session.port]]
        }
      };
    case "select_repeat_4097":
      return {
        stmt: "select repeat('a', 4097) as a",
        result:
            {columns: [{name: "a", type: "STRING"}], rows: [["a".repeat(4097)]]}
      };
    case "select_repeat_15M":
      return {
        stmt: "select repeat('a', 15 * 1024 * 1024) as a",
        result: {
          columns: [{name: "a", type: "STRING"}],
          rows: [["a".repeat(15 * 1024 * 1024)]]
        }
      };
    case "select_length_4097":
      return {
        stmt: "select length(" +
            'a'.repeat(4097) + ") as length",
        result: {columns: [{name: "length", type: "LONG"}], rows: [[4097]]}
      };
    case "router_select_schema_version":
      return {
        stmt: "SELECT * FROM mysql_innodb_cluster_metadata.schema_version",
        exec_time: options["exec_time"],
        result: {
          columns: [
            {type: "LONGLONG", name: "major"},
            {type: "LONGLONG", name: "minor"}, {type: "LONGLONG", name: "patch"}
          ],
          rows: [options["metadata_schema_version"]]
        }
      };
    case "router_select_group_membership":
      return {
        stmt:
            "SELECT member_id, member_host, member_port, member_state, member_role, member_version, @@group_replication_single_primary_mode FROM performance_schema.replication_group_members WHERE channel_name = 'group_replication_applier'",
        exec_time: options["exec_time"],
        result: {
          columns: [
            {"name": "member_id", "type": "STRING"},
            {"name": "member_host", "type": "STRING"},
            {"name": "member_port", "type": "LONG"},
            {"name": "member_state", "type": "STRING"},
            {"name": "member_role", "type": "STRING"},
            {"name": "member_version", "type": "STRING"},
            {
              "name": "@@group_replication_single_primary_mode",
              "type": "STRING"
            },
          ],
          rows:
              options["group_replication_members"].map(function(currentValue) {
                return [
                  currentValue[0],
                  currentValue[1],
                  currentValue[2],
                  currentValue[3],
                  currentValue[4],
                  options.version,
                  "ON",
                ];
              }),
        }
      };
    case "router_select_group_membership_pre_8_0_2":
      return {
        stmt:
            "SELECT member_id, member_host, member_port, member_state, IF(g.primary_uuid = '' OR member_id = g.primary_uuid, 'PRIMARY', 'SECONDARY') as member_role, '', @@group_replication_single_primary_mode FROM (SELECT IFNULL(variable_value, '') AS primary_uuid FROM performance_schema.global_status WHERE variable_name = 'group_replication_primary_member') g, performance_schema.replication_group_members WHERE channel_name = 'group_replication_applier'",
        result: {
          columns: [
            {"name": "member_id", "type": "STRING"},
            {"name": "member_host", "type": "STRING"},
            {"name": "member_port", "type": "LONG"},
            {"name": "member_state", "type": "STRING"},
            {"name": "member_role", "type": "STRING"},
            {"": "", "type": "STRING"},
            {
              "name": "@@group_replication_single_primary_mode",
              "type": "STRING"
            },
          ],
          rows:
              options["group_replication_members"].map(function(currentValue) {
                return [
                  currentValue[0],
                  currentValue[1],
                  currentValue[2],
                  currentValue[3],
                  currentValue[4],
                  "",
                  "ON",
                ];
              }),
        }
      };
    case "router_select_metadata_v2_gr":
      return {
        stmt:
            "select C.cluster_id, C.cluster_name, I.mysql_server_uuid, I.endpoint, I.xendpoint, I.attributes, I.label " +
            "from mysql_innodb_cluster_metadata.v2_instances I join mysql_innodb_cluster_metadata.v2_gr_clusters C " +
            "on I.cluster_id = C.cluster_id" +
            (options.gr_id === undefined || options.gr_id === "" ?
                 " where C.cluster_name = '" + options.innodb_cluster_name +
                     "'" :
                 " where C.group_name = '" + options.gr_id + "'"),
        result: {
          columns: [
            {"name": "C.cluster_id", "type": "VAR_STRING"},
            {"name": "C.cluster_name", "type": "VAR_STRING"},
            {"name": "I.mysql_server_uuid", "type": "VAR_STRING"},
            {"name": "I.addresses->>'$.mysqlClassic'", "type": "LONGBLOB"},
            {"name": "I.addresses->>'$.mysqlX'", "type": "LONGBLOB"},
            {"name": "I.attributes", "type": "VAR_STRING"},
            {"name": "I.label", "type": "VAR_STRING"}
          ],
          rows: options["innodb_cluster_instances"].map(function(currentValue) {
            var xport = currentValue[3] === undefined ? 0 : currentValue[3];
            var attributes =
                currentValue[4] === undefined ? "" : currentValue[4];
            var label = currentValue[1] + ":" + currentValue[2];
            return [
              options.cluster_id, options.innodb_cluster_name, currentValue[0],
              currentValue[1] + ":" + currentValue[2],
              currentValue[1] + ":" + xport, attributes, label
            ]
          }),
        }
      };
    case "router_select_metadata_v2_gr_account_verification":
      return {
        stmt:
            "select C.cluster_id, C.cluster_name, I.mysql_server_uuid, I.endpoint, I.xendpoint, I.attributes, I.label " +
            "from mysql_innodb_cluster_metadata.v2_instances I join mysql_innodb_cluster_metadata.v2_gr_clusters C " +
            "on I.cluster_id = C.cluster_id where C.cluster_name = 'some_cluster_name'",
        // The Router should ignore this result, it only checks if the user has
        // rights to do it
        result: {columns: [{"name": "1", "type": "LONG"}], rows: []}
      };
    case "router_select_metadata_v2_ar":
      return {
        stmt:
            "select C.cluster_id, C.cluster_name, M.member_id, I.endpoint, I.xendpoint, M.member_role, I.attributes, I.label " +
            "from mysql_innodb_cluster_metadata.v2_ar_members M join mysql_innodb_cluster_metadata.v2_instances I " +
            "on I.instance_id = M.instance_id join mysql_innodb_cluster_metadata.v2_ar_clusters C on I.cluster_id = C.cluster_id" +
            (options.cluster_id === undefined || options.cluster_id === "" ?
                 "" :
                 (" where C.cluster_id = '" + options.cluster_id + "'")),
        result: {
          columns: [
            {"name": "C.cluster_id", "type": "VAR_STRING"},
            {"name": "C.cluster_name", "type": "VAR_STRING"},
            {"name": "M.member_id", "type": "VAR_STRING"},
            {"name": "I.endpoint", "type": "LONGBLOB"},
            {"name": "I.xendpoint", "type": "LONGBLOB"},
            {"name": "I.member_role", "type": "VAR_STRING"},
            {"name": "I.attributes", "type": "VAR_STRING"},
            {"name": "I.label", "type": "VAR_STRING"}
          ],
          rows: options["innodb_cluster_instances"].map(function(
              currentValue, index) {
            var xport = currentValue[3] === undefined ? 0 : currentValue[3];
            var attributes =
                currentValue[4] === undefined ? "" : currentValue[4];
            var default_role = index == 0 ? "PRIMARY" : "SECONDARY";
            var role =
                currentValue[5] === undefined ? default_role : currentValue[5];
            var label = currentValue[1] + ":" + currentValue[2];
            return [
              options.cluster_id, options.innodb_cluster_name, currentValue[0],
              currentValue[1] + ":" + currentValue[2],
              currentValue[1] + ":" + xport, role, attributes, label
            ]
          }),
        }
      };
    case "router_select_metadata_v2_ar_account_verification":
      return {
        stmt:
            "select C.cluster_id, C.cluster_name, M.member_id, I.endpoint, I.xendpoint, M.member_role, I.attributes, I.label " +
            "from mysql_innodb_cluster_metadata.v2_ar_members M join mysql_innodb_cluster_metadata.v2_instances I " +
            "on I.instance_id = M.instance_id join mysql_innodb_cluster_metadata.v2_ar_clusters C on I.cluster_id = C.cluster_id " +
            "where C.cluster_name ='some_cluster_name';",
        // The Router should ignore this result, it only checks if the user has
        // rights to do it
        result: {columns: [{"name": "1", "type": "LONG"}], rows: []}
      };
    case "router_select_view_id_v2_ar":
      return {
        stmt:
            "select view_id from mysql_innodb_cluster_metadata.v2_ar_members where CAST(member_id AS char ascii) = CAST(@@server_uuid AS char ascii)" +
            (options.cluster_id === undefined || options.cluster_id === "" ?
                 "" :
                 (" and cluster_id = '" + options.cluster_id + "'")),
        result: {
          columns: [
            ,
            {"name": "view_id", "type": "LONG"},
          ],
          rows: [[options.view_id]]
        }
      };
    case "router_select_view_id_bootstrap_ar":
      return {
        stmt:
            "select view_id from mysql_innodb_cluster_metadata.v2_ar_members where CAST(member_id AS char ascii) = CAST(@@server_uuid AS char ascii)",
        result: {
          columns: [
            ,
            {"name": "view_id", "type": "LONG"},
          ],
          rows: [[options.view_id]]
        }
      };
    case "router_check_member_state":
      return {
        stmt:
            "SELECT member_state FROM performance_schema.replication_group_members WHERE CAST(member_id AS char ascii) = CAST(@@server_uuid AS char ascii)",
        result: {
          columns: [{"type": "STRING", "name": "member_state"}],
          rows: [[options.gr_member_state]]
        }
      };
    case "router_select_members_count":
      return {
        "stmt":
            "SELECT SUM(IF(member_state = 'ONLINE', 1, 0)) as num_onlines, SUM(IF(member_state = 'RECOVERING', 1, 0)) as num_recovering, COUNT(*) as num_total FROM performance_schema.replication_group_members",
        "result": {
          "columns": [
            {"type": "LONGLONG", "name": "num_onlines"},
            {"type": "LONGLONG", "name": "num_recovering"},
            {"type": "LONGLONG", "name": "num_total"}
          ],
          "rows": [[
            options.gr_members_online, , options.gr_members_recovering,
            options.gr_members_all
          ]]
        }
      };
    case "router_select_replication_group_name":
      return {
        "stmt": "select @@group_replication_group_name",
        "result": {
          "columns":
              [{"type": "STRING", "name": "@@group_replication_group_name"}],
          "rows": [[options.gr_id]]
        },
      };
    case "router_start_transaction":
      return {
        "stmt": "START TRANSACTION",
        "ok": {
          session_trackers: [
            {
              type: "trx_characteristics",
              trx_stmt: "START TRANSACTION;",
            },
            {
              type: "trx_state",
              state: "________",
            },
          ]
        }
      };
    case "router_select_router_address":
      return {
        stmt_regex:
            "SELECT address FROM mysql_innodb_cluster_metadata.v2_routers WHERE router_id = .*",
        result: {
          columns: [{"type": "STRING", "name": "address"}],
          rows: options["innodb_cluster_hosts"].map(function(currentValue) {
            return [currentValue[1]]
          })
        }
      };
    case "router_select_router_id":
      return {
        stmt_regex:
            "SELECT router_id FROM mysql_innodb_cluster_metadata.v2_routers WHERE router_name = .* and address = .*",
        result: {
          columns: [{"type": "LONG", "name": "router_id"}],
          rows: [[options.router_id]]
        }
      };
    case "router_insert_into_routers":
      return {
        "stmt_regex": "^INSERT INTO mysql_innodb_cluster_metadata.v2_routers.*",
        "ok": {"last_insert_id": options.last_insert_id}
      };
    case "router_delete_old_accounts":
      return {
        // delete all old accounts if necessarry
        // (ConfigGenerator::delete_account_for_all_hosts())
        "stmt_regex": "^SELECT host FROM mysql.user WHERE user = '.*'",
        "result": {
          "columns": [{"type": "LONGLONG", "name": "COUNT..."}],
          "rows": options["innodb_cluster_user_hosts"],
        }
      };
    case "router_create_user":
      return {
        "stmt_regex": "^CREATE USER 'mysql_router1_[0-9a-z]{12}'@" +
            options.user_host_pattern +
            " IDENTIFIED WITH `caching_sha2_password` BY '.*'",
        "ok": {}
      };
    case "router_check_auth_plugin":
      return {
        "stmt_regex": "^select host, plugin from mysql.user where user = .*",
        "result": {
          "columns": [
            {"type": "STRING", "name": "host"},
            {"type": "STRING", "name": "plugin"}
          ],
          "rows": [["localhost", "caching_sha2_password"]],
        }
      };
    case "router_grant_on_metadata_db":
      return {
        "stmt_regex":
            "^GRANT SELECT, EXECUTE ON mysql_innodb_cluster_metadata.*" +
            options.user_host_pattern,
        "ok": {}
      };
    case "router_grant_on_pfs_db":
      return {
        "stmt_regex":
            "^GRANT SELECT ON performance_schema.*" + options.user_host_pattern,
        "ok": {}
      };
    case "router_grant_on_routers":
      return {
        "stmt_regex":
            "^GRANT INSERT, UPDATE, DELETE ON mysql_innodb_cluster_metadata\\.routers.*" +
            options.user_host_pattern,
        "ok": {}
      };
    case "router_grant_on_v2_routers":
      return {
        "stmt_regex":
            "^GRANT INSERT, UPDATE, DELETE ON mysql_innodb_cluster_metadata\\.v2_routers.*" +
            options.user_host_pattern,
        "ok": {}
      };
    case "router_update_routers_in_metadata":
      return {
        "stmt_regex":
            "^UPDATE mysql_innodb_cluster_metadata\\.v2_routers SET attributes = " +
            "JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(IF\\(attributes IS NULL, '\\{\\}', attributes\\),    " +
            "'\\$\\.RWEndpoint', '.*'\\),    '\\$\\.ROEndpoint', '.*'\\),    '\\$\\.RWSplitEndpoint', '.*'\\),    '\\$\\.RWXEndpoint', '.*'\\),    " +
            "'\\$\\.ROXEndpoint', '.*'\\),    '\\$\\.MetadataUser', '.*'\\),    '\\$\\.SupportedRoutingGuidelinesVersion', '.*'\\),    '\\$\\.bootstrapTargetType', '.*'\\),    '\\$\\.Configuration', CAST\\('(.*)' as JSON\\)\\), " +
            "version = '.*', cluster_id = '.*' " +
            "WHERE router_id = .*",
        "ok": {}
      };
    case "router_clusterset_update_routers_in_metadata":
      return {
        "stmt_regex":
            "^UPDATE mysql_innodb_cluster_metadata\\.v2_routers SET attributes = " +
            "JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(IF\\(attributes IS NULL, '\\{\\}', attributes\\),    " +
            "'\\$\\.RWEndpoint', '.*'\\),    '\\$\\.ROEndpoint', '.*'\\),    '\\$\\.RWSplitEndpoint', '.*'\\),    '\\$\\.RWXEndpoint', '.*'\\),    " +
            "'\\$\\.ROXEndpoint', '.*'\\),    '\\$\\.MetadataUser', '.*'\\),    '\\$\\.SupportedRoutingGuidelinesVersion', '.*'\\),    '\\$\\.bootstrapTargetType', '.*'\\),    '\\$\\.Configuration', CAST\\('(.*)' as JSON\\)\\), " +
            "version = '.*', clusterset_id = '.*' " +
            "WHERE router_id = .*",
        "ok": {}
      };
    case "router_update_router_options_in_metadata":
      return {
        "stmt_regex":
            "^UPDATE mysql_innodb_cluster_metadata\\.v2_routers SET " +
            "options = JSON_SET\\(IF\\(options IS NULL, '\\{\\}', options\\), '\\$\\.target_cluster', '" +
            options.router_expected_target_cluster + "'\\) " +
            "WHERE router_id = .*",
        "ok": {}
      };
    case "router_commit":
      return {
        stmt: "COMMIT",
        ok: {
          session_trackers: [
            {
              type: "trx_characteristics",
              value: "",
            },
            {
              type: "trx_state",
              state: "________",
            },
            {
              type: "gtid",
              gtid: "3E11FA47-71CA-11E1-9E33-C80AA9429562:23",
            }
          ]
        }
      };
    case "router_rollback":
      return {
        stmt: "ROLLBACK",
        ok: {
          session_trackers: [
            {
              type: "trx_characteristics",
              value: "",
            },
            {
              type: "trx_state",
              state: "________",
            },
          ]
        }
      };
    case "router_replication_group_members":
      return {
        stmt:
            "SELECT member_host, member_port   FROM performance_schema.replication_group_members  /*!80002 ORDER BY member_role */",
        result: {
          columns: [
            {"type": "STRING", "name": "member_host"},
            {"type": "LONG", "name": "member_port"}
          ],
          rows:
              options["replication_group_members"].map(function(currentValue) {
                return [
                  currentValue[1],
                  currentValue[2],
                ]
              }),
        }
      };
    case "router_drop_users":
      return {stmt_regex: "^DROP USER IF EXISTS 'mysql_router.*", ok: {}};
    case "router_count_clusters_v2":
      return {
        stmt: "select count(*) from " +
            "mysql_innodb_cluster_metadata.v2_gr_clusters",
        result:
            {columns: [{"type": "LONGLONG", "name": "count(*)"}], rows: [[1]]}
      };
    case "router_count_clusters_v2_ar":
      return {
        stmt: "select count(*) from " +
            "mysql_innodb_cluster_metadata.v2_ar_clusters",
        result:
            {columns: [{"type": "LONGLONG", "name": "count(*)"}], rows: [[1]]}
      };
    case "router_show_cipher_status":
      return {
        "stmt": "show status like 'ssl_cipher'",
        "result": {
          "columns": [
            {"type": "STRING", "name": "Variable_name"},
            {"type": "STRING", "name": "Value"}
          ],
          "rows": [["Ssl_cipher", mysqld.session.ssl_cipher]]
        }
      };
    case "router_show_mysqlx_cipher_status":
      return {
        "stmt": "show status like 'mysqlx_ssl_cipher'",
        "result": {
          "columns": [
            {"type": "STRING", "name": "Variable_name"},
            {"type": "STRING", "name": "Value"}
          ],
          "rows": [["Mysqlx_ssl_cipher", mysqld.session.mysqlx_ssl_cipher]]
        }
      };
    case "router_select_cluster_instances_v2_ar":
      return {
        "stmt":
            "select c.cluster_id, c.cluster_id as uuid, c.cluster_name, i.address, i.attributes from " +
            "mysql_innodb_cluster_metadata.v2_instances i join " +
            "mysql_innodb_cluster_metadata.v2_clusters c on c.cluster_id = " +
            "i.cluster_id",
        result: {
          columns: [
            {"type": "STRING", "name": "cluster_id"},
            {"type": "STRING", "name": "uuid"},
            {"type": "STRING", "name": "cluster_name"},
            {"type": "STRING", "name": "i.address"},
            {"type": "STRING", "name": "i.attributes"}
          ],
          rows: options["innodb_cluster_instances"].map(function(currentValue) {
            var attributes =
                currentValue[4] === undefined ? "" : currentValue[4];
            return [
              options.cluster_id,
              options.cluster_id,
              options.innodb_cluster_name,
              currentValue[1] + ":" + currentValue[2],
              attributes,
            ]
          })
        }
      };
    case "router_select_cluster_instances_v2_gr":
      return {
        "stmt":
            "select c.cluster_id, c.group_name as uuid, c.cluster_name, i.address, i.attributes from " +
            "mysql_innodb_cluster_metadata.v2_instances i join " +
            "mysql_innodb_cluster_metadata.v2_gr_clusters c on c.cluster_id = " +
            "i.cluster_id",
        result: {
          columns: [
            {"type": "STRING", "name": "cluster_id"},
            {"type": "STRING", "name": "uuid"},
            {"type": "STRING", "name": "cluster_name"},
            {"type": "STRING", "name": "i.address"},
            {"type": "STRING", "name": "i.attributes"},
          ],
          rows: options["innodb_cluster_instances"].map(function(currentValue) {
            var attributes =
                currentValue[4] === undefined ? "" : currentValue[4];
            return [
              options.cluster_id,
              options.gr_id,
              options.innodb_cluster_name,
              currentValue[1] + ":" + currentValue[2],
              attributes,
            ]
          })
        }
      };
    case "router_select_cluster_instance_addresses_v2":
      return {
        "stmt":
            "select i.address from mysql_innodb_cluster_metadata.v2_instances i join mysql_innodb_cluster_metadata.v2_clusters c on c.cluster_id = i.cluster_id",
        result: {
          columns: [{
            "type": "STRING",
            "name": "JSON_UNQUOTE(JSON_EXTRACT(I.addresses, '$.mysqlClassic'))"
          }],
          rows: options["innodb_cluster_instances"].map(function(currentValue) {
            return [
              currentValue[1] + ":" + currentValue[2],
            ]
          })
        }
      };
    case "router_select_cluster_type_v2":
      return {
        "stmt":
            "select cluster_type from mysql_innodb_cluster_metadata.v2_this_instance",
        "result": {
          "columns": [{"type": "STRING", "name": "cluster_type"}],
          "rows": options.cluster_type === "" ? [] : [[options.cluster_type]]
        }
      };
    case "router_create_user_if_not_exists":
      // CREATE USER IF NOT EXISTS is the default way of creating users
      return {
        "stmt_regex": "^CREATE USER IF NOT EXISTS '" +
            options.account_user_pattern + "'@'" +
            options.account_host_pattern +
            "' IDENTIFIED WITH `caching_sha2_password` BY '" +
            options.account_pass_pattern + "'",
        "ok": {warning_count: options.create_user_warning_count}
      };
    case "router_create_user":
      return {
        // CREATE USER (without IF NOT EXISTS) is triggered by
        // --account-create always
        "stmt_regex": "^CREATE USER '" + options.account_user_pattern + "'@'" +
            options.account_host_pattern +
            "' IDENTIFIED WITH `caching_sha2_password` BY '" +
            options.account_pass_pattern + "'",
        "ok": {}
      };
    case "router_select_cluster_id_v2_ar":
      return {
        "stmt":
            "select cluster_id from mysql_innodb_cluster_metadata.v2_ar_clusters",
        "result": {
          "columns": [{"type": "STRING", "name": "cluster_id"}],
          "rows": [[options.cluster_id]]
        }
      };
    case "router_create_user_show_warnings":
      // this query will only be issued if CREATE USER [IF NOT EXISTS]
      // returned warning_count > 0
      return {
        stmt: "SHOW WARNINGS",
        // SHOW WARNINGS example output
        // +-------+------+---------------------------------------------+
        // | Level | Code | Message                                     |
        // +-------+------+---------------------------------------------+
        // | Note  | 3163 | Authorization ID 'bla'@'h1' already exists. |
        // | Note  | 3163 | Authorization ID 'bla'@'h3' already exists. |
        // +-------+------+---------------------------------------------+
        result: {
          columns: [
            {type: "STRING", name: "Level"}, {type: "LONG", name: "Code"},
            {type: "STRING", name: "Message"}
          ],
          rows: options.create_user_show_warnings_results
        }
      };
    case "router_update_attributes_v2":
      return {
        "stmt_regex": "UPDATE mysql_innodb_cluster_metadata\\.v2_routers" +
            " SET version = '(.*)', last_check_in = NOW\\(\\), attributes = JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(JSON_SET\\(" +
            " IF\\(attributes IS NULL, '\\{\\}', attributes\\)," +
            " '\\$\\.RWEndpoint', '(.*)'\\), '\\$\\.ROEndpoint', '(.*)'\\), '\\$\\.RWSplitEndpoint', '(.*)'\\), '\\$\\.RWXEndpoint', '(.*)'\\)," +
            " '\\$\\.ROXEndpoint', '(.*)'\\), '\\$\\.MetadataUser', '(.*)'\\), '\\$.Configuration', CAST\\('(.*)' as JSON\\)\\) WHERE router_id = (.*)",
        "ok": {}
      };
    case "router_update_last_check_in_v2":
      return {
        "stmt":
            "UPDATE mysql_innodb_cluster_metadata.v2_routers set last_check_in = " +
            "NOW() where router_id = " + options.router_id,
        "ok": {}
      };
    case "router_set_session_options":
      return {
        "stmt":
            "SET @@SESSION.autocommit=1, @@SESSION.character_set_client=utf8, " +
            "@@SESSION.character_set_results=utf8, @@SESSION.character_set_connection=utf8, " +
            "@@SESSION.sql_mode='ONLY_FULL_GROUP_BY,STRICT_TRANS_TABLES," +
            "NO_ZERO_IN_DATE,NO_ZERO_DATE,ERROR_FOR_DIVISION_BY_ZERO,NO_ENGINE_SUBSTITUTION', " +
            "@@SESSION.optimizer_switch='derived_merge=on'",
        "ok": {}
      };
    case "router_set_gr_consistency_level":
      return {
        "stmt": "SET @@SESSION.group_replication_consistency='EVENTUAL'",
        "ok": {}
      };
    case "router_select_rest_accounts_credentials_gr_by_uuid":
      return {
        "stmt": "SELECT user, authentication_string, privileges, " +
            "authentication_method FROM " +
            "mysql_innodb_cluster_metadata.v2_router_rest_accounts WHERE " +
            "cluster_id=(SELECT cluster_id FROM " +
            "mysql_innodb_cluster_metadata.v2_gr_clusters C WHERE C.group_name = '" +
            options.gr_id + "')",
        "result": {
          "columns": [
            {"type": "STRING", "name": "user"},
            {"type": "STRING", "name": "authentication_string"},
            {"type": "STRING", "name": "privileges"},
            {"type": "STRING", "name": "authentication_method"}
          ],
          "rows": options["rest_user_credentials"].map(function(currentValue) {
            return [
              currentValue[0],
              currentValue[1],
              currentValue[2] === "" ? null : currentValue[2],
              currentValue[3],
            ]
          })
        }
      };
    case "mysqlsh_select_connection_id":
      // needed by mysqlsh to start.
      return {
        "stmt":
            "select @@lower_case_table_names, @@version, connection_id(), variable_value " +
            "from performance_schema.session_status " +
            "where variable_name = 'mysqlx_ssl_cipher'",
        "result": {
          "columns": [
            {"type": "LONG", "name": "@@lower_case_table_names"},
            {"type": "VAR_STRING", "name": "@@version"},
            {"type": "LONG", "name": "connection_id()"},
            {"type": "VAR_STRING", "name": "variable_value"}
          ],
          "rows": [[0, options["version"], 1, mysqld.session.mysqlx_ssl_cipher]]
        }
      };
    case "mysqlsh_select_version_comment":
      // needed by mysqlsh to start.
      return {
        "stmt": "select concat(@@version, ' ', @@version_comment)",
        "result": {
          "columns": [
            {
              "type": "VAR_STRING",
              "name": "concat(@@version, ' ', @@version_comment)"
            },
          ],
          "rows": [[options["version"] + " " + options["version_comment"]]]
        }
      };
    case "router_clusterset_cluster_info_by_name":
      return {
        stmt:
            "select C.cluster_id, C.group_name, CS.domain_name, CSM.member_role from mysql_innodb_cluster_metadata.v2_gr_clusters C join mysql_innodb_cluster_metadata.v2_cs_members CSM on CSM.cluster_id = C.cluster_id join mysql_innodb_cluster_metadata.v2_cs_clustersets CS on CS.clusterset_id = CSM.clusterset_id where C.cluster_name = '" +
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .name +
            "'",
        result: {
          columns: [
            {"type": "STRING", "name": "C.cluster_id"},
            {"type": "STRING", "name": "C.group_name"},
            {"type": "STRING", "name": "CS.domain_name"},
            {"type": "STRING", "name": "CSM.member_role"},
          ],
          rows: [[
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .uuid,
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .gr_uuid,
            options.clusterset_data.clusterset_name,
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .role
          ]]
        }
      };
    case "router_clusterset_cluster_info_by_name_unknown":
      return {
        stmt_regex:
            "select C.cluster_id, C.group_name, CS.domain_name, CSM.member_role from mysql_innodb_cluster_metadata.v2_gr_clusters C join mysql_innodb_cluster_metadata.v2_cs_members CSM on CSM.cluster_id = C.cluster_id join mysql_innodb_cluster_metadata.v2_cs_clustersets CS on CS.clusterset_id = CSM.clusterset_id where C.cluster_name = .*",
        result: {
          columns: [
            {"type": "STRING", "name": "C.cluster_id"},
            {"type": "STRING", "name": "C.group_name"},
            {"type": "STRING", "name": "CS.domain_name"},
            {"type": "STRING", "name": "CSM.member_role"},
          ],
          rows: []
        }
      };
    case "router_clusterset_cluster_info_current_cluster":
      return {
        stmt:
            "select C.cluster_id, C.group_name, CS.domain_name, CSM.member_role from mysql_innodb_cluster_metadata.v2_gr_clusters C join mysql_innodb_cluster_metadata.v2_cs_members CSM on CSM.cluster_id = C.cluster_id join mysql_innodb_cluster_metadata.v2_cs_clustersets CS on CS.clusterset_id = CSM.clusterset_id where C.cluster_id = (select cluster_id from mysql_innodb_cluster_metadata.v2_this_instance)",
        result: {
          columns: [
            {"type": "STRING", "name": "C.cluster_id"},
            {"type": "STRING", "name": "C.group_name"},
            {"type": "STRING", "name": "CS.domain_name"},
            {"type": "STRING", "name": "CSM.member_role"},
          ],
          rows: options.clusterset_simulate_cluster_not_found ? [] : [[
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .uuid,
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .gr_uuid,
            options.clusterset_data.clusterset_name,
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .role
          ]]
        }
      };
    case "router_clusterset_cluster_info_primary":
      return {
        stmt:
            "select C.cluster_id, C.group_name, CS.domain_name, CSM.member_role from mysql_innodb_cluster_metadata.v2_gr_clusters C join mysql_innodb_cluster_metadata.v2_cs_members CSM on CSM.cluster_id = C.cluster_id join mysql_innodb_cluster_metadata.v2_cs_clustersets CS on CS.clusterset_id = CSM.clusterset_id where CSM.member_role = 'PRIMARY'",
        result: {
          columns: [
            {"type": "STRING", "name": "C.cluster_id"},
            {"type": "STRING", "name": "C.group_name"},
            {"type": "STRING", "name": "CS.domain_name"},
            {"type": "STRING", "name": "CSM.member_role"},
          ],
          rows: options.clusterset_simulate_cluster_not_found ||
                  (options.clusterset_data.primary_cluster_id >=
                   options.clusterset_data.clusters.length) ?
              [] :
              [[
                options.clusterset_data
                    .clusters[options.clusterset_data.primary_cluster_id]
                    .uuid,
                options.clusterset_data
                    .clusters[options.clusterset_data.primary_cluster_id]
                    .gr_uuid,
                options.clusterset_data.clusterset_name, "PRIMARY"
              ]]
        }
      };
    case "router_clusterset_all_nodes":
      return {
        stmt: "SELECT i.address, i.attributes, csm.member_role " +
            "FROM mysql_innodb_cluster_metadata.v2_instances i " +
            "LEFT JOIN mysql_innodb_cluster_metadata.v2_cs_members csm " +
            "ON i.cluster_id = csm.cluster_id " +
            "WHERE i.cluster_id IN ( " +
            "   SELECT cluster_id " +
            "   FROM mysql_innodb_cluster_metadata.v2_cs_members " +
            "   WHERE clusterset_id = " +
            "      (SELECT clusterset_id " +
            "       FROM mysql_innodb_cluster_metadata.v2_cs_members " +
            "       WHERE cluster_id = '" +
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .uuid +
            "') )",
        result: {
          columns: [
            {"type": "STRING", "name": "i.address"},
            {"type": "STRING", "name": "i.attributes"},
            {"type": "STRING", "name": "csm.member_role"},
          ],

          rows: options.clusterset_data.clusters
                    .reduce(
                        function(nodes, cluster) {
                          var cluster_nodes = cluster.nodes;
                          for (var i = 0; i < cluster_nodes.length; i++) {
                            cluster_nodes[i].cluster_role = cluster.role;
                          }
                          return nodes.concat(cluster.nodes);
                        },
                        [])
                    .map(function(node) {
                      return [
                        node.host + ":" + node.classic_port, node.attributes,
                        node.cluster_role
                      ]
                    })
        }
      };
    case "router_clusterset_all_nodes_by_clusterset_id":
      return {
        stmt:
            "select I.mysql_server_uuid, I.endpoint, I.xendpoint, I.attributes, I.label, " +
            "C.cluster_id, C.cluster_name, CSM.member_role, CSM.invalidated, CS.domain_name " +
            "from mysql_innodb_cluster_metadata.v2_instances I " +
            "join mysql_innodb_cluster_metadata.v2_gr_clusters C on I.cluster_id = C.cluster_id join mysql_innodb_cluster_metadata.v2_cs_members CSM " +
            "on CSM.cluster_id = C.cluster_id left join mysql_innodb_cluster_metadata.v2_cs_clustersets CS on CSM.clusterset_id = CS.clusterset_id " +
            "where CS.clusterset_id = '" +
            options.clusterset_data.clusterset_id + "' " +
            "order by C.cluster_id",
        result: {
          columns: [
            {"type": "STRING", "name": "I.mysql_server_uuid"},
            {"type": "STRING", "name": "I.endpoint"},
            {"type": "STRING", "name": "I.xendpoint"},
            {"type": "STRING", "name": "I.attributes"},
            {"type": "STRING", "name": "I.label"},
            {"type": "STRING", "name": "C.cluster_id"},
            {"type": "STRING", "name": "C.cluster_name"},
            {"type": "STRING", "name": "CSM.member_role"},
            {"type": "LONGLONG", "name": "CSM.invalidated"},
            {"type": "STRING", "name": "CS.domain_name"},
          ],

          rows: options.clusterset_data.clusters
                    .reduce(
                        function(nodes, cluster) {
                          var cluster_nodes = cluster.nodes;
                          for (var i = 0; i < cluster_nodes.length; i++) {
                            cluster_nodes[i].cluster_uuid = cluster.uuid;
                            cluster_nodes[i].cluster_name = cluster.name;
                            cluster_nodes[i].cluster_role = cluster.role;
                            cluster_nodes[i].cluster_invalid = cluster.invalid;
                          }

                          return nodes.concat(cluster_nodes);
                        },
                        [])
                    .map(function(node) {
                      return [
                        node.uuid, node.host + ":" + node.classic_port,
                        node.host + ":" +
                            (node.x_port === undefined ? 0 : node.x_port),
                        node.attributes, node.host + ":" + node.classic_port,
                        node.cluster_uuid, node.cluster_name, node.cluster_role,
                        node.cluster_invalid,
                        options.clusterset_data.clusterset_name
                      ];
                    })
        }
      };
    case "router_clusterset_id":
      return {
        stmt_regex:
            "select CSM.clusterset_id from mysql_innodb_cluster_metadata.v2_cs_members CSM join mysql_innodb_cluster_metadata.v2_gr_clusters C on CSM.cluster_id = C.cluster_id where C.group_name = .*",
        result: {
          columns: [
            {"type": "STRING", "name": "CSM.clusterset_id"},
          ],
          rows: [[options.clusterset_data.clusterset_id]]
        }
      };
    case "router_clusterset_id_current":
      return {
        stmt:
            "select CSM.clusterset_id from mysql_innodb_cluster_metadata.v2_cs_members CSM join mysql_innodb_cluster_metadata.v2_gr_clusters C on CSM.cluster_id = C.cluster_id where C.cluster_id = (select cluster_id from mysql_innodb_cluster_metadata.v2_this_instance)",
        result: {
          columns: [
            {"type": "STRING", "name": "CSM.clusterset_id"},
          ],
          rows: [[options.clusterset_data.clusterset_id]]
        }
      };
    case "router_clusterset_view_id":
      return {
        stmt:
            "select view_id from mysql_innodb_cluster_metadata.v2_cs_clustersets where clusterset_id = '" +
            options.clusterset_data.clusterset_id + "'",
        result: {
          columns: [
            {"name": "view_id", "type": "LONG"},
          ],
          rows: [[options.view_id]]
        }
      };
    case "router_unknown_clusterset_view_id":
      return {
        stmt_regex:
            "select view_id from mysql_innodb_cluster_metadata.v2_cs_clustersets where clusterset_id = .*",
        result: {
          columns: [
            {"name": "view_id", "type": "LONG"},
          ],
          rows: []
        }
      };
    case "router_clusterset_present":
      return {
        stmt:
            "select count(clusterset_id) from mysql_innodb_cluster_metadata.v2_this_instance i join mysql_innodb_cluster_metadata.v2_cs_members " +
            "csm on i.cluster_id = csm.cluster_id where clusterset_id is not null",
        result: {
          columns: [
            {"type": "LONGLONG", "name": "count(clusterset_id)"},
          ],
          rows: [[options.clusterset_present]]
        }
      };
    case "router_bootstrap_target_type":
      return {
        stmt:
            "SELECT JSON_UNQUOTE(JSON_EXTRACT(r.attributes, '$.bootstrapTargetType')) " +
            "FROM mysql_innodb_cluster_metadata.v2_routers r where r.router_id = " +
            options.router_id,
        result: {
          columns: [
            {"type": "VAR_STRING", "name": "bootstrapTargetType"},
          ],
          rows: [[options.bootstrap_target_type]]
        }
      };
    case "router_router_select_cs_options":
      return {
        stmt:
            "SELECT router_options FROM mysql_innodb_cluster_metadata.v2_cs_router_options WHERE router_id = " +
            options.router_id,
        result: {
          columns: [{"name": "router_options", "type": "VAR_STRING"}],
          rows: [[options.router_options]]
        }
      };
    case "router_clusterset_select_cluster_info_by_primary_role":
      return {
        stmt:
            "select C.cluster_id, C.cluster_name, C.group_name from mysql_innodb_cluster_metadata.v2_gr_clusters C join " +
            "mysql_innodb_cluster_metadata.v2_cs_members CSM on CSM.cluster_id = C.cluster_id left join " +
            "mysql_innodb_cluster_metadata.v2_cs_clustersets CS on CSM.clusterset_id = CS.clusterset_id where " +
            "CSM.member_role = 'PRIMARY' and CS.clusterset_id = '" +
            options.clusterset_data.clusterset_id + "'",
        result: {
          columns: [
            {"name": "C.cluster_id", "type": "VAR_STRING"},
            {"name": "C.cluster_name", "type": "VAR_STRING"},
            {"name": "C.group_name", "type": "VAR_STRING"},
          ],
          rows: options.clusterset_simulate_cluster_not_found ? [] : [[
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .uuid,
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .name,
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .gr_uuid
          ]],
        }
      };
    case "router_clusterset_select_cluster_info_by_gr_uuid":
      return {
        stmt:
            "select C.cluster_id, C.cluster_name, C.group_name from mysql_innodb_cluster_metadata.v2_gr_clusters C join " +
            "mysql_innodb_cluster_metadata.v2_cs_members CSM on CSM.cluster_id = C.cluster_id left join " +
            "mysql_innodb_cluster_metadata.v2_cs_clustersets CS on CSM.clusterset_id = CS.clusterset_id where " +
            "C.group_name = '" +
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .gr_uuid +
            "' and CS.clusterset_id = '" +
            options.clusterset_data.clusterset_id + "'",
        result: {
          columns: [
            {"name": "C.cluster_id", "type": "VAR_STRING"},
            {"name": "C.cluster_name", "type": "VAR_STRING"},
            {"name": "C.group_name", "type": "VAR_STRING"},
          ],
          rows: options.clusterset_simulate_cluster_not_found ? [] : [[
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .uuid,
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .name,
            options.clusterset_data
                .clusters[options.clusterset_target_cluster_id]
                .gr_uuid
          ]]
        }
      };
    case "router_clusterset_select_cluster_info_by_gr_uuid_unknown":
      return {
        stmt_regex:
            "select C.cluster_id, C.cluster_name, C.group_name from mysql_innodb_cluster_metadata.v2_gr_clusters C join mysql_innodb_cluster_metadata.v2_cs_members CSM on CSM.cluster_id = C.cluster_id left join mysql_innodb_cluster_metadata.v2_cs_clustersets CS on CSM.clusterset_id = CS.clusterset_id where C.group_name = .*" +
            " and CS.clusterset_id = '" +
            options.clusterset_data.clusterset_id + "'",
        result: {
          columns: [
            {"name": "C.cluster_id", "type": "VAR_STRING"},
            {"name": "C.cluster_name", "type": "VAR_STRING"},
            {"name": "C.group_name", "type": "VAR_STRING"},
          ],
          rows: []
        }
      };
    case "router_clusterset_select_gr_members_status":
      return {
        stmt:
            "SELECT member_id, member_host, member_port, member_state, member_role, member_version, @@group_replication_single_primary_mode FROM performance_schema.replication_group_members WHERE channel_name = 'group_replication_applier'",
        result: {
          columns: [
            {"name": "member_id", "type": "STRING"},
            {"name": "member_host", "type": "STRING"},
            {"name": "member_port", "type": "LONG"},
            {"name": "member_state", "type": "STRING"},
            {"name": "member_role", "type": "STRING"},
            {"name": "member_version", "type": "STRING"},
            {
              "name": "@@group_replication_single_primary_mode",
              "type": "STRING"
            },
          ],
          rows: options.clusterset_data
                    .clusters[options.clusterset_data.this_cluster_id]
                    .gr_nodes.map(function(node) {
                      return [
                        node.uuid, "127.0.0.1", node.classic_port, node.status,
                        node.role, node.version, "ON"
                      ];
                    }),
        }
      };
    case "router_select_router_options_view":
      return {
        stmt:
            "SELECT router_options FROM mysql_innodb_cluster_metadata.v2_router_options WHERE router_id = " +
            options.router_id,
        result: {
          columns: [{"type": "STRING", "name": "router_options"}],
          rows: [[options.router_options]]
        }
      };
    case "router_select_current_instance_attributes":
      return {
        stmt:
            "select i.attributes from mysql_innodb_cluster_metadata.v2_this_instance " +
            "ti left join mysql_innodb_cluster_metadata.v2_instances i " +
            "on ti.instance_id = i.instance_id",
        result: {
          columns: [{"type": "STRING", "name": "attributes"}],
          rows: [[options.current_instance_attributes]]
        }
      };
    case "router_select_config_defaults_stored_gr_cluster":
      return {
        stmt_regex:
            "select JSON_EXTRACT\\(router_options, '\\$\\.Configuration.\"" +
            options["router_version"] +
            "\"'\\) IS NULL from mysql_innodb_cluster_metadata.v2_gr_clusters where cluster_id = \".*\"",
        result: {
          columns: [{
            "type": "LONGLONG",
            "name": "JSON_EXTRACT(router_options, '$.Configuration\"') IS NULL"
          }],
          rows: [[options.config_defaults_stored_is_null]]
        }
      };
    case "router_select_config_defaults_stored_ar_cluster":
      return {
        stmt_regex:
            "select JSON_EXTRACT\\(router_options, '\\$\\.Configuration.\"" +
            options["router_version"] +
            "\"'\\) IS NULL from mysql_innodb_cluster_metadata.v2_ar_clusters where cluster_id = \".*\"",
        result: {
          columns: [{
            "type": "LONGLONG",
            "name": "JSON_EXTRACT(router_options, '$.Configuration') IS NULL"
          }],
          rows: [[options.config_defaults_stored_is_null]]
        }
      };
    case "router_select_config_defaults_stored_clusterset":
      return {
        stmt_regex:
            "select JSON_EXTRACT\\(router_options, '\\$\\.Configuration.\"" +
            options["router_version"] +
            "\"'\\) IS NULL from mysql_innodb_cluster_metadata.v2_cs_clustersets where clusterset_id = \".*\"",
        result: {
          columns: [{
            "type": "LONGLONG",
            "name": "JSON_EXTRACT(router_options, '$.Configuration') IS NULL"
          }],
          rows: [[options.config_defaults_stored_is_null]]
        }
      };
    case "router_store_config_defaults_gr_cluster":
      return {
        stmt_regex:
            "UPDATE mysql_innodb_cluster_metadata\\.v2_gr_clusters SET router_options = " +
            "JSON_MERGE_PATCH\\(IF\\(router_options IS NULL, '\\{\\}', router_options\\), " +
            "CAST\\('(.*)' as JSON\\)\\) WHERE cluster_id = '.*'",
        ok: {}
      };
    case "router_store_config_defaults_ar_cluster":
      return {
        stmt_regex:
            "UPDATE mysql_innodb_cluster_metadata\\.v2_ar_clusters SET router_options = " +
            "JSON_MERGE_PATCH\\(IF\\(router_options IS NULL, '\\{\\}', router_options\\), " +
            "CAST\\('(.*)' as JSON\\)\\) WHERE cluster_id = '.*'",
        ok: {}
      };
    case "router_store_config_defaults_clusterset":
      return {
        stmt_regex:
            "UPDATE mysql_innodb_cluster_metadata\\.v2_cs_clustersets SET router_options = " +
            "JSON_MERGE_PATCH\\(IF\\(router_options IS NULL, '\\{\\}', router_options\\), " +
            "CAST\\('(.*)' as JSON\\)\\) WHERE clusterset_id = '.*'",
        ok: {}
      };
    case "router_update_local_cluster_in_metadata":
      return {
        "stmt_regex":
            "^UPDATE mysql_innodb_cluster_metadata\\.v2_routers SET " +
            "attributes = JSON_SET\\(IF\\(attributes IS NULL, '\\{\\}', attributes\\), '\\$\\.LocalCluster', '" +
            options.router_expected_local_cluster + "'\\) " +
            "WHERE router_id = .*",
        ok: {}
      };
    case "get_local_cluster_name":
      return {
        stmt:
            "select cluster_name from mysql_innodb_cluster_metadata.v2_this_instance",
        result: {
          columns: [{"name": "cluster_name", "type": "VAR_STRING"}],
          rows:
              [[(options.bootstrap_target_type == "cluster") ?
                    options.innodb_cluster_name :
                    options.clusterset_data
                        .clusters[options.clusterset_data.this_cluster_id]
                        .name]]
        }
      };
    case "get_routing_guidelines_version":
      return {
        stmt: "SELECT guideline->>'$.version' FROM\n" +
            "mysql_innodb_cluster_metadata.routing_guidelines WHERE guideline_id = (\n" +
            "  SELECT COALESCE(RO.router_options->>'$.guideline',\n" +
            "                  CS.router_options->>'$.guideline',\n" +
            "                  CL.router_options->>'$.guideline')\n" +
            "  FROM\n" +
            "    mysql_innodb_cluster_metadata.v2_router_options AS RO\n" +
            "  LEFT JOIN\n" +
            "    mysql_innodb_cluster_metadata.clustersets AS CS ON RO.clusterset_id = CS.clusterset_id\n" +
            "  LEFT JOIN\n" +
            "    mysql_innodb_cluster_metadata.clusters AS CL ON RO.cluster_id = CL.cluster_id\n" +
            "  WHERE RO.router_id = " + options.router_id + ")",
        result: {
          columns: [{"name": "version", "type": "VAR_STRING"}],
          rows: [["1.0"]]
        }
      };
    case "get_guidelines_router_info":
      return {
        stmt:
            "SELECT address, attributes->>'$.ROEndpoint', attributes->>'$.RWEndpoint', attributes->>'$.RWSplitEndpoint', attributes->>'$.ROXEndpoint', attributes->>'$.RWXEndpoint', attributes->>'$.LocalCluster', options, router_name FROM mysql_innodb_cluster_metadata.v2_routers WHERE router_id=" +
            options.router_id,
        result: {
          columns: [
            {"name": "address", "type": "STRING"},
            {"name": "ro_endpoint", "type": "STRING"},
            {"name": "rw_endpoint", "type": "STRING"},
            {"name": "rw_split_endpoint", "type": "STRING"},
            {"name": "ro_x_endpoint", "type": "STRING"},
            {"name": "rw_x_endpoint", "type": "STRING"},
            {"name": "target_cluster", "type": "STRING"},
            {"name": "options", "type": "STRING"},
            {"name": "router_name", "type": "STRING"}
          ],
          rows: [[
            options.router_hostname, options.router_ro_classic_port,
            options.router_rw_classic_port,
            options.router_rw_split_classic_port, options.router_ro_x_port,
            options.router_rw_x_port, options.innodb_cluster_name,
            options.router_options, options.router_name
          ]]
        }
      };
    case "get_routing_guidelines":
      return {
        stmt: "SELECT guideline FROM\n" +
            "mysql_innodb_cluster_metadata.routing_guidelines WHERE guideline_id = (\n" +
            "  SELECT COALESCE(RO.router_options->>'$.guideline',\n" +
            "                  CS.router_options->>'$.guideline',\n" +
            "                  CL.router_options->>'$.guideline')\n" +
            "  FROM\n" +
            "    mysql_innodb_cluster_metadata.v2_router_options AS RO\n" +
            "  LEFT JOIN\n" +
            "    mysql_innodb_cluster_metadata.clustersets AS CS ON RO.clusterset_id = CS.clusterset_id\n" +
            "  LEFT JOIN\n" +
            "    mysql_innodb_cluster_metadata.clusters AS CL ON RO.cluster_id = CL.cluster_id\n" +
            "  WHERE RO.router_id = " + options.router_id + ")",
        result: {
          columns: [{"name": "guidelines", "type": "VAR_STRING"}],
          rows: [[options.routing_guidelines]]
        }
      };
    case "mrs_set_sql_mode":
      return {"stmt": "SET @@SESSION.sql_mode=DEFAULT;", "ok": {}};
    case "mrs_set_meta_provider_role":
      return {"stmt": "SET ROLE mysql_rest_service_meta_provider", "ok": {}};
    case "mrs_select_version":
      return {
        stmt:
            "SELECT substring_index(@@version, '.', 1), concat(@@version_comment, @@version)",
        result: {
          columns: [
            {
              "name": "substring_index(@@version, '.', 1)",
              "type": "VAR_STRING"
            },
            {
              "name": "concat(@@version_comment, @@version)",
              "type": "VAR_STRING"
            }
          ],
          rows: [["9", "Source distribution mrs"]],
        }
      };
    case "mrs_select_basedir":
      return {
        stmt: "SELECT @@basedir",
        result: {
          columns: [{"name": "@@basedir", "type": "VAR_STRING"}],
          rows: [[options.mrs_basedir]],
        }
      };
    case "mrs_set_data_provider_role":
      return {"stmt": "SET ROLE mysql_rest_service_data_provider", "ok": {}};
    case "mrs_select_router_id":
      return {
        stmt_regex:
            "SELECT `id` FROM mysql_rest_service_metadata.router WHERE router_name = '.*' AND address = '.*'",
        result: {
          columns: [{"name": "id", "type": "LONG"}],
          rows: [[options.mrs_router_id]],
        }
      };
  };
};

/**
 * create the response for commonly used statements
 *
 * @param {array} stmt_keys - statement keys
 * @param {array} options   - parameters
 * @returns {object} object of 'statement text': response object
 */
exports.prepare_statement_responses = function(stmt_keys, options) {
  // process options once
  var opts_with_defaults = get_options(options);
  var getter = get_response;

  return stmt_keys.reduce(function(acc, stmt_key) {
    var res = getter(stmt_key, opts_with_defaults);
    acc[res.stmt] = res;

    return acc;
  }, {});
};

/**
 * create the callable responses for commonly used statements.
 *
 * @param {array} stmt_keys - statement keys
 * @param {array} options   - parameters
 * @returns {object} object of 'statement text': callable which returns
 *     a response object
 */
exports.prepare_callable_statement_responses = function(stmt_keys, options) {
  return stmt_keys.reduce(function(acc, stmt_key) {
    // lookup the results by stmt_key
    var res = exports.get(stmt_key, options);
    acc[res.stmt] = function() {
      return exports.get(stmt_key, options);
    };

    return acc;
  }, {});
};


/**
 * create the response for commonly used regex statements
 *
 * @param {array} stmt_keys - statement keys
 * @param {array} options   - parameters
 * @returns {object} object of 'statement text': response object
 */
exports.prepare_statement_responses_regex = function(stmt_keys, options) {
  return stmt_keys.reduce(function(acc, stmt_key) {
    // lookup the results by stmt_key
    var res = exports.get(stmt_key, options);
    acc[res.stmt_regex] = res;

    return acc;
  }, {});
};

/**
 * create error-response for unknown statements
 *
 * @param {string} stmt statement text
 * @returns error response
 */
exports.unknown_statement_response = function(stmt) {
  return {
    error: {code: 1273, sql_state: "HY001", message: "Syntax Error at: " + stmt}
  }
};

/**
 * checks if a given statment matches any stmt_regex in
 * in the responses object
 *
 * @param {string} regex_stmt regex statement text
 * @param {object} common_responses_regex object containing
 *     common responses
 * @returns  response if any matches the statement or undefined
 */
exports.handle_regex_stmt = function(regex_stmt, common_responses_regex) {
  for (var stmt in common_responses_regex) {
    if (regex_stmt.match(stmt)) return common_responses_regex[stmt];
  }

  return undefined;
};

exports.empty_if_undef = function(variable) {
  if (variable === undefined) {
    variable = "";
  }
};
