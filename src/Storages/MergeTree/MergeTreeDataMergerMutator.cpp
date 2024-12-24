#include <Storages/MergeTree/Compaction/CompactionStatistics.h>
#include <Storages/MergeTree/MergeTreeDataMergerMutator.h>

#include <base/insertAtEnd.h>

namespace CurrentMetrics
{
    extern const Metric BackgroundMergesAndMutationsPoolTask;
}

namespace ProfileEvents
{
    extern const Event MergerMutatorPrepareRangesForMergeElapsedMicroseconds;
    extern const Event MergerMutatorRangesForMergeCount;
    extern const Event MergerMutatorPartsInRangesForMergeCount;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
    extern const int ABORTED;
}

namespace MergeTreeSetting
{
    extern const MergeTreeSettingsInt64 merge_with_recompression_ttl_timeout;
    extern const MergeTreeSettingsBool min_age_to_force_merge_on_partition_only;
    extern const MergeTreeSettingsUInt64 min_age_to_force_merge_seconds;
    extern const MergeTreeSettingsUInt64 number_of_free_entries_in_pool_to_execute_optimize_entire_partition;
}

namespace
{

size_t calculatePartsCount(const PartsRanges & ranges)
{
    size_t count = 0;

    for (const auto & range : ranges)
        count += range.size();

    return count;
}

PartsRanges splitByMergePredicate(PartsRange && range, const AllowedMergingPredicate & can_merge)
{
    const auto & build_next_range = [&](PartsRange::iterator & current_it)
    {
        PartsRange mergeable_range;

        /// Find beginning of next range. It should be a part that can be merged with itself.
        /// Parts can be merged with themselves for TTL needs for example.
        /// So we have to check if this part is currently being inserted with quorum and so on and so forth.
        /// Obviously we have to check it manually only for the first part
        /// of each range because it will be automatically checked for a pair of parts.
        while (current_it < range.end())
        {
            PartProperties & current_part = *current_it++;

            if (can_merge(nullptr, &current_part))
            {
                mergeable_range.push_back(std::move(current_part));
                break;
            }
        }

        /// All parts can't participate in merges
        if (mergeable_range.empty())
            return mergeable_range;

        while (current_it != range.end())
        {
            PartProperties & prev_part = mergeable_range.back();
            PartProperties & current_part = *current_it++;

            /// If we cannot merge with previous part we need to close this range.
            if (!can_merge(&prev_part, &current_part))
                return mergeable_range;

            /// Check for consistency of data parts. If assertion is failed, it requires immediate investigation.
            if (current_part.part_info.contains(prev_part.part_info))
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Part {} contains previous part {}", current_part.name, prev_part.name);

            if (!current_part.part_info.isDisjoint(prev_part.part_info))
                throw Exception(ErrorCodes::LOGICAL_ERROR, "Part {} intersects previous part {}", current_part.name, prev_part.name);

            mergeable_range.push_back(std::move(current_part));
        }

        return mergeable_range;
    };

    PartsRanges mergeable_ranges;
    for (auto current_it = range.begin(); current_it != range.end();)
        if (auto next_mergeable_range = build_next_range(current_it); !next_mergeable_range.empty())
            mergeable_ranges.push_back(std::move(next_mergeable_range));

    return mergeable_ranges;
}

PartsRanges splitByMergePredicate(PartsRanges && ranges, const AllowedMergingPredicate & can_merge)
{
    Stopwatch ranges_for_merge_timer;

    PartsRanges mergeable_ranges;
    for (auto && range : ranges)
    {
        auto splitted_range_by_predicate = splitByMergePredicate(std::move(range), can_merge);
        insertAtEnd(mergeable_ranges, std::move(splitted_range_by_predicate));
    }

    ProfileEvents::increment(ProfileEvents::MergerMutatorPartsInRangesForMergeCount, calculatePartsCount(mergeable_ranges));
    ProfileEvents::increment(ProfileEvents::MergerMutatorRangesForMergeCount, mergeable_ranges.size());
    ProfileEvents::increment(ProfileEvents::MergerMutatorPrepareRangesForMergeElapsedMicroseconds, ranges_for_merge_timer.elapsedMicroseconds());

    return mergeable_ranges;
}

tl::expected<void, PreformattedMessage> canMergeAllParts(const PartsRange & range, const AllowedMergingPredicate & can_merge)
{
    const PartProperties * prev_part = nullptr;

    for (const auto & part : range)
    {
        if (auto can_merge_result = can_merge(prev_part, &part); !can_merge_result.has_value())
            return tl::make_unexpected(std::move(can_merge_result.error()));

        prev_part = &part;
    }

    return {};
}

std::unordered_map<String, PartsRanges> combineByPartitions(PartsRanges && ranges)
{
    std::unordered_map<String, PartsRanges> ranges_by_partitions;

    for (auto && range : ranges)
    {
        assert(!range.empty());
        ranges_by_partitions[range.front().part_info.partition_id].push_back(std::move(range));
    }

    return ranges_by_partitions;
}

struct PartitionStatistics
{
    time_t min_age = std::numeric_limits<time_t>::max();
    size_t parts_count = 0;
};

std::unordered_map<String, PartitionStatistics> calculateStatisticsForPartitions(const PartsRanges & ranges)
{
    std::unordered_map<String, PartitionStatistics> stats;

    for (const auto & range : ranges)
    {
        assert(!range.empty());
        PartitionStatistics & partition_stats = stats[range.front().part_info.partition_id];

        partition_stats.parts_count += range.size();

        for (const auto & part : range)
            partition_stats.min_age = std::min(partition_stats.min_age, part.age);
    }

    return stats;
}

String getBestPartitionToOptimizeEntire(
    const ContextPtr & context,
    const MergeTreeSettingsPtr & settings,
    const std::unordered_map<String, PartitionStatistics> & stats,
    const LoggerPtr & log)
{
    if (!(*settings)[MergeTreeSetting::min_age_to_force_merge_on_partition_only])
        return {};

    if (!(*settings)[MergeTreeSetting::min_age_to_force_merge_seconds])
        return {};

    size_t occupied = CurrentMetrics::values[CurrentMetrics::BackgroundMergesAndMutationsPoolTask].load(std::memory_order_relaxed);
    size_t max_tasks_count = context->getMergeMutateExecutor()->getMaxTasksCount();
    if (occupied > 1 && max_tasks_count - occupied < (*settings)[MergeTreeSetting::number_of_free_entries_in_pool_to_execute_optimize_entire_partition])
    {
        LOG_INFO(log,
            "Not enough idle threads to execute optimizing entire partition. See settings "
            "'number_of_free_entries_in_pool_to_execute_optimize_entire_partition' and 'background_pool_size'");

        return {};
    }

    auto best_partition_it = std::max_element(
        stats.begin(),
        stats.end(),
        [](const auto & e1, const auto & e2)
        {
            // If one partition has only a single part, always select the other partition.
            if (e1.second.parts_count == 1)
                return true;
            if (e2.second.parts_count == 1)
                return false;

            // If both partitions have more than one part, select the older partition.
            return e1.second.min_age < e2.second.min_age;
        });

    assert(best_partition_it != stats.end());

    const size_t best_partition_min_age = static_cast<size_t>(best_partition_it->second.min_age);
    const size_t best_partition_parts_count = best_partition_it->second.parts_count;
    if (best_partition_min_age < (*settings)[MergeTreeSetting::min_age_to_force_merge_seconds] || best_partition_parts_count == 1)
        return {};

    return best_partition_it->first;
}

}

MergeTreeDataMergerMutator::MergeTreeDataMergerMutator(MergeTreeData & data_)
    : data(data_), log(getLogger(data.getLogName() + " (MergerMutator)"))
{
}

void MergeTreeDataMergerMutator::updateTTLMergeTimes(const MergeSelectorChoice & merge_choice, time_t next_due_time)
{
    assert(!merge_choice.range.empty());
    const String & partition_id = merge_choice.range.front().part_info.partition_id;

    switch (merge_choice.merge_type)
    {
        case MergeType::Regular:
            /// Do not update anything with regular merge.
            return;
        case MergeType::TTLDelete:
            next_delete_ttl_merge_times_by_partition[partition_id] = next_due_time;
            return;
        case MergeType::TTLRecompress:
            next_recompress_ttl_merge_times_by_partition[partition_id] = next_due_time;
            return;
    }
}

PartitionIdsHint MergeTreeDataMergerMutator::getPartitionsThatMayBeMerged(
    const PartsCollectorPtr & parts_collector,
    const AllowedMergingPredicate & can_merge,
    const MergeSelectorApplier & selector) const
{
    const auto context = data.getContext();
    const auto settings = data.getSettings();
    const auto metadata_snapshot = data.getInMemoryMetadataPtr();
    const auto storage_policy = data.getStoragePolicy();
    const time_t current_time = std::time(nullptr);
    const bool can_use_ttl_merges = !ttl_merges_blocker.isCancelled();

    auto ranges = parts_collector->collectPartsToUse(metadata_snapshot, storage_policy, current_time, /*partitions_hint=*/std::nullopt);
    if (ranges.empty())
        return {};

    ranges = splitByMergePredicate(std::move(ranges), can_merge);
    if (ranges.empty())
        return {};

    const auto partitions_stats = calculateStatisticsForPartitions(ranges);
    const auto ranges_by_partitions = combineByPartitions(std::move(ranges));

    PartitionIdsHint partitions_hint;
    for (const auto & [partition, ranges_in_partition] : ranges_by_partitions)
    {
        assert(!ranges_in_partition.empty());
        assert(!ranges_in_partition.front().empty());

        auto merge_choice = selector.chooseMergeFrom(
            ranges_in_partition,
            metadata_snapshot,
            settings,
            next_delete_ttl_merge_times_by_partition,
            next_recompress_ttl_merge_times_by_partition,
            can_use_ttl_merges,
            current_time,
            log);

        const String & partition_id = ranges_in_partition.front().front().part_info.partition_id;

        if (merge_choice.has_value())
            partitions_hint.insert(partition_id);
        else
            LOG_TRACE(log, "Nothing to merge in partition {} with max_total_size_to_merge = {} (looked up {} ranges)",
                partition_id, ReadableSize(selector.max_total_size_to_merge), ranges_in_partition.size());
    }

    if (auto best = getBestPartitionToOptimizeEntire(context, settings, partitions_stats, log); !best.empty())
        partitions_hint.insert(std::move(best));

    LOG_TRACE(log,
            "Checked {} partitions, found {} partitions with parts that may be merged: [{}] "
            "(max_total_size_to_merge={}, merge_with_ttl_allowed={}, can_use_ttl_merges={})",
            ranges_by_partitions.size(), partitions_hint.size(), fmt::join(partitions_hint, ", "),
            selector.max_total_size_to_merge, selector.merge_with_ttl_allowed, can_use_ttl_merges);

    return partitions_hint;
}

tl::expected<MergeSelectorChoice, SelectMergeFailure> MergeTreeDataMergerMutator::selectPartsToMerge(
    const PartsCollectorPtr & parts_collector,
    const AllowedMergingPredicate & can_merge,
    const MergeSelectorApplier & selector,
    const std::optional<PartitionIdsHint> & partitions_hint)
{
    const auto context = data.getContext();
    const auto settings = data.getSettings();
    const auto metadata_snapshot = data.getInMemoryMetadataPtr();
    const auto storage_policy = data.getStoragePolicy();
    const time_t current_time = std::time(nullptr);
    const bool can_use_ttl_merges = !ttl_merges_blocker.isCancelled();

    auto ranges = parts_collector->collectPartsToUse(metadata_snapshot, storage_policy, current_time, partitions_hint);
    if (ranges.empty())
    {
        return tl::make_unexpected(SelectMergeFailure{
            .reason = SelectMergeFailure::Reason::CANNOT_SELECT,
            .explanation = PreformattedMessage::create("There are no parts that can be merged. (Collector returned empty ranges set)"),
        });
    }

    ranges = splitByMergePredicate(std::move(ranges), can_merge);
    if (ranges.empty())
    {
        return tl::make_unexpected(SelectMergeFailure{
            .reason = SelectMergeFailure::Reason::CANNOT_SELECT,
            .explanation = PreformattedMessage::create("No parts satisfy preconditions for merge"),
        });
    }

    auto merge_choice = selector.chooseMergeFrom(
        ranges,
        metadata_snapshot,
        settings,
        next_delete_ttl_merge_times_by_partition,
        next_recompress_ttl_merge_times_by_partition,
        can_use_ttl_merges,
        current_time,
        log);

    if (merge_choice.has_value())
    {
        updateTTLMergeTimes(merge_choice.value(), current_time + (*settings)[MergeTreeSetting::merge_with_recompression_ttl_timeout]);
        return std::move(merge_choice.value());
    }

    const auto partitions_stats = calculateStatisticsForPartitions(ranges);

    if (auto best = getBestPartitionToOptimizeEntire(context, settings, partitions_stats, log); !best.empty())
    {
        return selectAllPartsToMergeWithinPartition(
            metadata_snapshot,
            parts_collector,
            can_merge,
            /*partition_id=*/best,
            /*final=*/true,
            /*optimize_skip_merged_partitions=*/true);
    }

    return tl::make_unexpected(SelectMergeFailure{
        .reason = SelectMergeFailure::Reason::CANNOT_SELECT,
        .explanation = PreformattedMessage::create("There is no need to merge parts according to merge selector algorithm"),
    });
}

tl::expected<MergeSelectorChoice, SelectMergeFailure> MergeTreeDataMergerMutator::selectAllPartsToMergeWithinPartition(
    const StorageMetadataPtr & metadata_snapshot,
    const PartsCollectorPtr & parts_collector,
    const AllowedMergingPredicate & can_merge,
    const String & partition_id,
    bool final,
    bool optimize_skip_merged_partitions)
{
    /// time is not important in this context, since the parts will not be passed through the merge selector.
    const time_t current_time = std::time(nullptr);
    const auto storage_policy = data.getStoragePolicy();

    PartsRanges ranges = parts_collector->collectPartsToUse(metadata_snapshot, storage_policy, current_time, PartitionIdsHint{partition_id});
    if (ranges.empty())
    {
        return tl::make_unexpected(SelectMergeFailure{
            .reason = SelectMergeFailure::Reason::CANNOT_SELECT,
            .explanation = PreformattedMessage::create("There are no parts inside partition"),
        });
    }

    if (ranges.size() > 1)
    {
        return tl::make_unexpected(SelectMergeFailure{
            .reason = SelectMergeFailure::Reason::CANNOT_SELECT,
            .explanation = PreformattedMessage::create("Already produced: {} mergeable ranges, but only one is required.", ranges.size()),
        });
    }

    if (!final && ranges.front().size() == 1)
    {
        return tl::make_unexpected(SelectMergeFailure{
            .reason = SelectMergeFailure::Reason::CANNOT_SELECT,
            .explanation = PreformattedMessage::create("There is only one part inside partition."),
        });
    }

    /// If final, optimize_skip_merged_partitions is true and we have only one part in partition with level > 0
    /// than we don't select it to merge. But if there are some expired TTL then merge is needed
    if (final && optimize_skip_merged_partitions && ranges.front().size() == 1)
    {
        const PartProperties & part = ranges.front().front();

        /// FIXME? Probably we should check expired ttls here, not only calculated.
        if (part.part_info.level > 0 && (!metadata_snapshot->hasAnyTTL() || part.all_ttl_calculated_if_any))
        {
            return tl::make_unexpected(SelectMergeFailure{
                .reason = SelectMergeFailure::Reason::NOTHING_TO_MERGE,
                .explanation = PreformattedMessage::create("Partition skipped due to optimize_skip_merged_partitions."),
            });
        }
    }

    if (auto result = canMergeAllParts(ranges.front(), can_merge); !result.has_value())
    {
        return tl::make_unexpected(SelectMergeFailure{
            .reason = SelectMergeFailure::Reason::CANNOT_SELECT,
            .explanation = std::move(result.error()),
        });
    }

    const auto & parts = ranges.front();

    /// Enough disk space to cover the new merge with a margin.
    const auto required_disk_space = CompactionStatistics::estimateAtLeastAvailableSpace(parts);
    const auto available_disk_space = data.getStoragePolicy()->getMaxUnreservedFreeSpace();
    if (available_disk_space <= required_disk_space)
    {
        if (time_t now = time(nullptr); now - disk_space_warning_time > 3600)
        {
            disk_space_warning_time = now;
            LOG_WARNING(log,
                "Won't merge parts from {} to {} because not enough free space: "
                "{} free and unreserved, {} required now; suppressing similar warnings for the next hour",
                parts.front().name, parts.back().name, ReadableSize(available_disk_space), ReadableSize(required_disk_space));
        }

        return tl::make_unexpected(SelectMergeFailure{
            .reason = SelectMergeFailure::Reason::CANNOT_SELECT,
            .explanation = PreformattedMessage::create("Insufficient available disk space, required {}", ReadableSize(required_disk_space)),
        });
    }

    LOG_DEBUG(log, "Selected {} parts from {} to {}", parts.size(), parts.front().name, parts.back().name);

    return MergeSelectorChoice{std::move(ranges.front()), MergeType::Regular};
}

/// parts should be sorted.
MergeTaskPtr MergeTreeDataMergerMutator::mergePartsToTemporaryPart(
    FutureMergedMutatedPartPtr future_part,
    const StorageMetadataPtr & metadata_snapshot,
    MergeList::Entry * merge_entry,
    std::unique_ptr<MergeListElement> projection_merge_list_element,
    TableLockHolder & holder,
    time_t time_of_merge,
    ContextPtr context,
    ReservationSharedPtr space_reservation,
    bool deduplicate,
    const Names & deduplicate_by_columns,
    bool cleanup,
    const MergeTreeData::MergingParams & merging_params,
    const MergeTreeTransactionPtr & txn,
    bool need_prefix,
    IMergeTreeDataPart * parent_part,
    const String & suffix)
{
    return std::make_shared<MergeTask>(
        future_part,
        const_cast<StorageMetadataPtr &>(metadata_snapshot),
        merge_entry,
        std::move(projection_merge_list_element),
        time_of_merge,
        context,
        holder,
        space_reservation,
        deduplicate,
        deduplicate_by_columns,
        cleanup,
        merging_params,
        need_prefix,
        parent_part,
        suffix,
        txn,
        &data,
        this,
        &merges_blocker,
        &ttl_merges_blocker);
}

MutateTaskPtr MergeTreeDataMergerMutator::mutatePartToTemporaryPart(
    FutureMergedMutatedPartPtr future_part,
    StorageMetadataPtr metadata_snapshot,
    MutationCommandsConstPtr commands,
    MergeListEntry * merge_entry,
    time_t time_of_mutation,
    ContextPtr context,
    const MergeTreeTransactionPtr & txn,
    ReservationSharedPtr space_reservation,
    TableLockHolder & holder,
    bool need_prefix)
{
    return std::make_shared<MutateTask>(
        future_part,
        metadata_snapshot,
        commands,
        merge_entry,
        time_of_mutation,
        context,
        space_reservation,
        holder,
        txn,
        data,
        *this,
        merges_blocker,
        need_prefix);
}

MergeTreeData::DataPartPtr MergeTreeDataMergerMutator::renameMergedTemporaryPart(
    MergeTreeData::MutableDataPartPtr & new_data_part,
    const MergeTreeData::DataPartsVector & parts,
    const MergeTreeTransactionPtr & txn,
    MergeTreeData::Transaction & out_transaction)
{
    /// Some of source parts was possibly created in transaction, so non-transactional merge may break isolation.
    if (data.transactions_enabled.load(std::memory_order_relaxed) && !txn)
        throw Exception(ErrorCodes::ABORTED,
            "Cancelling merge, because it was done without starting transaction,"
            "but transactions were enabled for this table");

    /// Rename new part, add to the set and remove original parts.
    auto replaced_parts = data.renameTempPartAndReplace(new_data_part, out_transaction, /*rename_in_transaction=*/true);

    /// Explicitly rename part while still holding the lock for tmp folder to avoid cleanup
    out_transaction.renameParts();

    /// Let's check that all original parts have been deleted and only them.
    if (replaced_parts.size() != parts.size())
    {
        /** This is normal, although this happens rarely.
         *
         * The situation - was replaced 0 parts instead of N can be, for example, in the following case
         * - we had A part, but there was no B and C parts;
         * - A, B -> AB was in the queue, but it has not been done, because there is no B part;
         * - AB, C -> ABC was in the queue, but it has not been done, because there are no AB and C parts;
         * - we have completed the task of downloading a B part;
         * - we started to make A, B -> AB merge, since all parts appeared;
         * - we decided to download ABC part from another replica, since it was impossible to make merge AB, C -> ABC;
         * - ABC part appeared. When it was added, old A, B, C parts were deleted;
         * - AB merge finished. AB part was added. But this is an obsolete part. The log will contain the message `Obsolete part added`,
         *   then we get here.
         *
         * When M > N parts could be replaced?
         * - new block was added in ReplicatedMergeTreeSink;
         * - it was added to working dataset in memory and renamed on filesystem;
         * - but ZooKeeper transaction that adds it to reference dataset in ZK failed;
         * - and it is failed due to connection loss, so we don't rollback working dataset in memory,
         *   because we don't know if the part was added to ZK or not
         *   (see ReplicatedMergeTreeSink)
         * - then method selectPartsToMerge selects a range and sees, that EphemeralLock for the block in this part is unlocked,
         *   and so it is possible to merge a range skipping this part.
         *   (NOTE: Merging with part that is not in ZK is not possible, see checks in 'createLogEntryToMergeParts'.)
         * - and after merge, this part will be removed in addition to parts that was merged.
         */
        LOG_WARNING(log,
            "Unexpected number of parts removed when adding {}: {} instead of {}\n"
            "Replaced parts:\n{}\n"
            "Parts:\n{}\n",
            new_data_part->name,
            replaced_parts.size(),
            parts.size(),
            fmt::join(getPartsNames(replaced_parts), "\n"),
            fmt::join(getPartsNames(parts), "\n"));
    }
    else
    {
        for (size_t i = 0; i < parts.size(); ++i)
            if (parts[i]->name != replaced_parts[i]->name)
                throw Exception(ErrorCodes::LOGICAL_ERROR,
                    "Unexpected part removed when adding {}: {} instead of {}",
                    new_data_part->name, replaced_parts[i]->name, parts[i]->name);
    }

    LOG_TRACE(log, "Merged {} parts: [{}, {}] -> {}", parts.size(), parts.front()->name, parts.back()->name, new_data_part->name);
    return new_data_part;
}

}
