# -----------------------------------------------------
# Basic setup of Inndobcluster with AR replication
# and single node
# -----------------------------------------------------
--disable_query_log
--disable_result_log
--let $port=`SELECT @@GLOBAL.port`

INSERT INTO mysql_innodb_cluster_metadata.clusters(cluster_id, cluster_name,
        description, options, attributes, cluster_type, primary_mode,
        router_options, clusterset_id)
    VALUES(1, "my_cluster", null, null, null, "ar","pm","{}", null);

eval INSERT INTO mysql_innodb_cluster_metadata.instances(instance_id,
        cluster_id, address, mysql_server_uuid, instance_name, addresses,
        attributes, description)
      VALUES(1, 1, "127.0.0.1:$port", @@GLOBAL.server_uuid, "local_i1",
         '{"mysqlClassic": "127.0.0.1:$port"}', null, null);

INSERT INTO mysql_innodb_cluster_metadata.async_cluster_views(cluster_id,
        view_id, topology_type, view_change_reason, view_change_time,
        view_change_info, attributes)
    VALUES(1, 1, 'SINGLE-PRIMARY-TREE', "", NOW(), "{}", "{}");

INSERT INTO mysql_innodb_cluster_metadata.async_cluster_members(cluster_id,
        view_id, instance_id, master_instance_id, primary_master, attributes)
    VALUES(1, 1, 1, 1, true, "{}");

--enable_query_log
--enable_result_log

--echo # InnDBCluster `AR Single node setup` - created
