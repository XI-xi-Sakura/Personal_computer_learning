# This file is used by rpl.rpl_perfschema_applier_status_by_worker_gtid_skipped_transaction
# and rpl.rpl_perfschema_applier_status_by_worker_gtid_skipped_transaction_mts
#
# Run some transactions on master and apply them on slave. Collect the last
# applied GTID.
# Then run a transaction on master and a transaction on slave, both with the
# same GTID, so that the master's transaction is skipped on slave.
# Check that the master's GTID does not show in LAST_APPLIED_TRANSACTION.
# Also, check that *ing_transaction is cleared in the two (mts, three) tables.
#

# replicate some transactions to slave
CREATE TABLE t (a int);
DROP TABLE t;
--source include/rpl/sync_to_replica.inc

--let $slave_param= Replica_SQL_Running_State
--let $slave_param_value= Replica has read all relay log; waiting for more updates
--source include/rpl/wait_for_replica_status.inc

--let $last_processed_gtid_before= query_get_value(select LAST_PROCESSED_TRANSACTION from performance_schema.replication_applier_status_by_coordinator, LAST_PROCESSED_TRANSACTION, 1)
--let $last_applied_gtid_before= query_get_value(select LAST_APPLIED_TRANSACTION from performance_schema.replication_applier_status_by_worker, LAST_APPLIED_TRANSACTION, 1)

--source include/rpl/stop_applier.inc

--source include/rpl/connection_source.inc
--let $current_gtid= aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa:20
--disable_query_log
eval SET SESSION GTID_NEXT= '$current_gtid';
--enable_query_log

CREATE TABLE t1 (a int);

--source include/rpl/connection_replica.inc
--disable_query_log
eval SET @@SESSION.GTID_NEXT= '$current_gtid';
--enable_query_log
CREATE TABLE t2 (a int);

SET @@SESSION.GTID_NEXT= 'AUTOMATIC';
--source include/rpl/start_applier.inc

--source include/rpl/connection_source.inc
--source include/rpl/sync_to_replica.inc

--let $queueing_gtid_= query_get_value(SELECT QUEUEING_TRANSACTION FROM performance_schema.replication_connection_status, QUEUEING_TRANSACTION, 1)
--let $assert_text= queueing_gtid was cleared after the gtid skipped transactions
--let $assert_cond= "$queueing_gtid" = ""
--source include/assert.inc

--let $processing_gtid_= query_get_value(SELECT PROCESSING_TRANSACTION FROM performance_schema.replication_applier_status_by_coordinator, PROCESSING_TRANSACTION, 1)
--let $assert_text= processing_gtid was cleared after the gtid skipped transactions
--let $assert_cond= "$processing_gtid" = ""
--source include/assert.inc

--let $applying_gtid= query_get_value(SELECT APPLYING_TRANSACTION FROM performance_schema.replication_applier_status_by_worker, APPLYING_TRANSACTION, 1)
--let $assert_text= applying_gtid was cleared after the gtid skipped transactions
--let $assert_cond= "$applying_gtid" = ""
--source include/assert.inc

--let $last_queued_gtid= query_get_value(SELECT LAST_QUEUED_TRANSACTION FROM performance_schema.replication_connection_status, LAST_QUEUED_TRANSACTION, 1)
--let $assert_text= Assert that the last_queued_gtid is the same as the skipped transaction
--let $assert_cond= "$last_queued_gtid" = "$current_gtid"
--source include/assert.inc

--let $last_processed_gtid_= query_get_value(SELECT LAST_PROCESSED_TRANSACTION FROM performance_schema.replication_applier_status_by_coordinator, LAST_PROCESSED_TRANSACTION, 1)
--let $assert_text= Assert the the last_processed_gtid is clear after the slave skipped the transaction
--let $assert_cond= "$last_processed_gtid" = ""
--source include/assert.inc

--let $last_applied_gtid= query_get_value(SELECT LAST_APPLIED_TRANSACTION FROM performance_schema.replication_applier_status_by_worker, LAST_APPLIED_TRANSACTION, 1)
--let $assert_text= Assert that the last_applied_gtid is clear after the slave skipped the transaction
--let $assert_cond= "$last_applied_gtid" = ""
--source include/assert.inc


# cleanup
DROP TABLE t2;
--source include/rpl/connection_source.inc
SET SQL_LOG_BIN= 0;
SET @@SESSION.GTID_NEXT= 'AUTOMATIC';
DROP TABLE t1;
SET SQL_LOG_BIN= 1;
