-- Tags: no-s3-storage

SET merge_tree_read_split_ranges_into_intersecting_and_non_intersecting_injection_probability = 0.0;

drop table if exists t;

create table t(a UInt64) engine=MergeTree order by tuple();

system stop merges t;

insert into t select * from numbers_mt(1e3);
insert into t select * from numbers_mt(1e3);
insert into t select * from numbers_mt(1e3);

set allow_asynchronous_read_from_io_pool_for_merge_tree = 1;
set max_streams_for_merge_tree_reading = 64;
set max_block_size = 65409;

-- slightly different transforms will be generated by reading steps if we let settings randomisation to change this setting value --
set read_in_order_two_level_merge_threshold = 1000;

-- for pretty simple queries (no filter, aggregation and so on) with a limit smaller than the `max_block_size` we request reading using only a single stream for better performance --
explain pipeline select * from t limit 100;
