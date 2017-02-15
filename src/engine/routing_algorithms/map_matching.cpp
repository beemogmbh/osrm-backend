#include "engine/routing_algorithms/map_matching.hpp"

namespace osrm
{
namespace engine
{
namespace routing_algorithms
{

unsigned MapMatching::GetMedianSampleTime(const std::vector<unsigned> &timestamps) const
{
    BOOST_ASSERT(timestamps.size() > 1);

    std::vector<unsigned> sample_times(timestamps.size());

    std::adjacent_difference(timestamps.begin(), timestamps.end(), sample_times.begin());

    // don't use first element of sample_times -> will not be a difference.
    auto first_elem = std::next(sample_times.begin());
    auto median = first_elem + std::distance(first_elem, sample_times.end()) / 2;
    std::nth_element(first_elem, median, sample_times.end());
    return *median;
}

SubMatchingList MapMatching::
operator()(const std::shared_ptr<const datafacade::BaseDataFacade> facade,
           const CandidateLists &candidates_list,
           const std::vector<util::Coordinate> &trace_coordinates,
           const std::vector<unsigned> &trace_timestamps,
           const std::vector<boost::optional<double>> &trace_gps_precision) const
{
    SubMatchingList sub_matchings;

    BOOST_ASSERT(candidates_list.size() == trace_coordinates.size());
    BOOST_ASSERT(candidates_list.size() > 1);

    const bool use_timestamps = trace_timestamps.size() > 1;

    const auto median_sample_time = [&] {
        if (use_timestamps)
        {
            return std::max(1u, GetMedianSampleTime(trace_timestamps));
        }
        else
        {
            return 1u;
        }
    }();
    const auto max_broken_time = median_sample_time * MAX_BROKEN_STATES;
    const auto max_distance_delta = [&] {
        if (use_timestamps)
        {
            return median_sample_time * facade->GetMapMatchingMaxSpeed();
        }
        else
        {
            return MAX_DISTANCE_DELTA;
        }
    }();

    std::vector<std::vector<double>> emission_log_probabilities(trace_coordinates.size());
    if (trace_gps_precision.empty())
    {
        for (auto t = 0UL; t < candidates_list.size(); ++t)
        {
            emission_log_probabilities[t].resize(candidates_list[t].size());
            std::transform(candidates_list[t].begin(),
                           candidates_list[t].end(),
                           emission_log_probabilities[t].begin(),
                           [this](const PhantomNodeWithDistance &candidate) {
                               return default_emission_log_probability(candidate.distance);
                           });
        }
    }
    else
    {
        for (auto t = 0UL; t < candidates_list.size(); ++t)
        {
            emission_log_probabilities[t].resize(candidates_list[t].size());
            if (trace_gps_precision[t])
            {
                map_matching::EmissionLogProbability emission_log_probability(
                    *trace_gps_precision[t]);
                std::transform(
                    candidates_list[t].begin(),
                    candidates_list[t].end(),
                    emission_log_probabilities[t].begin(),
                    [&emission_log_probability](const PhantomNodeWithDistance &candidate) {
                        return emission_log_probability(candidate.distance);
                    });
            }
            else
            {
                std::transform(candidates_list[t].begin(),
                               candidates_list[t].end(),
                               emission_log_probabilities[t].begin(),
                               [this](const PhantomNodeWithDistance &candidate) {
                                   return default_emission_log_probability(candidate.distance);
                               });
            }
        }
    }

    HMM model(candidates_list, emission_log_probabilities);

    std::size_t initial_timestamp = model.initialize(0);
    if (initial_timestamp == map_matching::INVALID_STATE)
    {
        return sub_matchings;
    }

    engine_working_data.InitializeOrClearFirstThreadLocalStorage(facade->GetNumberOfNodes());
    engine_working_data.InitializeOrClearSecondThreadLocalStorage(facade->GetNumberOfNodes());

    QueryHeap &forward_heap = *(engine_working_data.forward_heap_1);
    QueryHeap &reverse_heap = *(engine_working_data.reverse_heap_1);
    QueryHeap &forward_core_heap = *(engine_working_data.forward_heap_2);
    QueryHeap &reverse_core_heap = *(engine_working_data.reverse_heap_2);

    std::size_t breakage_begin = map_matching::INVALID_STATE;
    std::vector<std::size_t> split_points;
    std::vector<std::size_t> prev_unbroken_timestamps;
    prev_unbroken_timestamps.reserve(candidates_list.size());
    prev_unbroken_timestamps.push_back(initial_timestamp);
    for (auto t = initial_timestamp + 1; t < candidates_list.size(); ++t)
    {

        const bool gap_in_trace = [&, use_timestamps]() {
            // use temporal information if available to determine a split
            if (use_timestamps)
            {
                return trace_timestamps[t] - trace_timestamps[prev_unbroken_timestamps.back()] >
                       max_broken_time;
            }
            else
            {
                return t - prev_unbroken_timestamps.back() > MAX_BROKEN_STATES;
            }
        }();

        if (!gap_in_trace)
        {
            BOOST_ASSERT(!prev_unbroken_timestamps.empty());
            const std::size_t prev_unbroken_timestamp = prev_unbroken_timestamps.back();

            const auto &prev_viterbi = model.viterbi[prev_unbroken_timestamp];
            const auto &prev_pruned = model.pruned[prev_unbroken_timestamp];
            const auto &prev_unbroken_timestamps_list = candidates_list[prev_unbroken_timestamp];
            const auto &prev_coordinate = trace_coordinates[prev_unbroken_timestamp];

            auto &current_viterbi = model.viterbi[t];
            auto &current_pruned = model.pruned[t];
            auto &current_parents = model.parents[t];
            auto &current_lengths = model.path_distances[t];
            const auto &current_timestamps_list = candidates_list[t];
            const auto &current_coordinate = trace_coordinates[t];

            const auto haversine_distance = util::coordinate_calculation::haversineDistance(
                prev_coordinate, current_coordinate);
            // assumes minumum of 0.1 m/s
            const int duration_upper_bound =
                ((haversine_distance + max_distance_delta) * 0.25) * 10;

            // compute d_t for this timestamp and the next one
            for (const auto s : util::irange<std::size_t>(0UL, prev_viterbi.size()))
            {
                if (prev_pruned[s])
                {
                    continue;
                }

                for (const auto s_prime : util::irange<std::size_t>(0UL, current_viterbi.size()))
                {
                    const double emission_pr = emission_log_probabilities[t][s_prime];
                    double new_value = prev_viterbi[s] + emission_pr;
                    if (current_viterbi[s_prime] > new_value)
                    {
                        continue;
                    }

                    forward_heap.Clear();
                    reverse_heap.Clear();

                    double network_distance;
                    if (facade->GetCoreSize() > 0)
                    {
                        forward_core_heap.Clear();
                        reverse_core_heap.Clear();
                        network_distance = super::GetNetworkDistanceWithCore(
                            facade,
                            forward_heap,
                            reverse_heap,
                            forward_core_heap,
                            reverse_core_heap,
                            prev_unbroken_timestamps_list[s].phantom_node,
                            current_timestamps_list[s_prime].phantom_node,
                            duration_upper_bound);
                    }
                    else
                    {
                        network_distance = super::GetNetworkDistance(
                            facade,
                            forward_heap,
                            reverse_heap,
                            prev_unbroken_timestamps_list[s].phantom_node,
                            current_timestamps_list[s_prime].phantom_node);
                    }

                    // get distance diff between loc1/2 and locs/s_prime
                    const auto d_t = std::abs(network_distance - haversine_distance);

                    // very low probability transition -> prune
                    if (d_t >= max_distance_delta)
                    {
                        continue;
                    }

                    const double transition_pr = transition_log_probability(d_t);
                    new_value += transition_pr;

                    if (new_value > current_viterbi[s_prime])
                    {
                        current_viterbi[s_prime] = new_value;
                        current_parents[s_prime] = std::make_pair(prev_unbroken_timestamp, s);
                        current_lengths[s_prime] = network_distance;
                        current_pruned[s_prime] = false;
                        model.breakage[t] = false;
                    }
                }
            }

            if (model.breakage[t])
            {
                // save start of breakage -> we need this as split point
                if (t < breakage_begin)
                {
                    breakage_begin = t;
                }

                BOOST_ASSERT(prev_unbroken_timestamps.size() > 0);
                // remove both ends of the breakage
                prev_unbroken_timestamps.pop_back();
            }
            else
            {
                prev_unbroken_timestamps.push_back(t);
            }
        }

        // breakage recover has removed all previous good points
        const bool trace_split = prev_unbroken_timestamps.empty();

        if (trace_split || gap_in_trace)
        {
            std::size_t split_index = t;
            if (breakage_begin != map_matching::INVALID_STATE)
            {
                split_index = breakage_begin;
                breakage_begin = map_matching::INVALID_STATE;
            }
            split_points.push_back(split_index);

            // note: this preserves everything before split_index
            model.Clear(split_index);
            std::size_t new_start = model.initialize(split_index);
            // no new start was found -> stop viterbi calculation
            if (new_start == map_matching::INVALID_STATE)
            {
                break;
            }

            prev_unbroken_timestamps.clear();
            prev_unbroken_timestamps.push_back(new_start);
            // Important: We potentially go back here!
            // However since t > new_start >= breakge_begin
            // we can only reset trace_coordindates.size() times.
            t = new_start;
            // note: the head of the loop will call ++t, hence the next
            // iteration will actually be on new_start+1
        }
    }

    if (!prev_unbroken_timestamps.empty())
    {
        split_points.push_back(prev_unbroken_timestamps.back() + 1);
    }

    std::size_t sub_matching_begin = initial_timestamp;
    for (const auto sub_matching_end : split_points)
    {
        map_matching::SubMatching matching;

        std::size_t parent_timestamp_index = sub_matching_end - 1;
        while (parent_timestamp_index >= sub_matching_begin &&
               model.breakage[parent_timestamp_index])
        {
            --parent_timestamp_index;
        }
        while (sub_matching_begin < sub_matching_end && model.breakage[sub_matching_begin])
        {
            ++sub_matching_begin;
        }
        const auto sub_matching_last_timestamp = parent_timestamp_index;

        // matchings that only consist of one candidate are invalid
        if (parent_timestamp_index - sub_matching_begin + 1 < 2)
        {
            sub_matching_begin = sub_matching_end;
            continue;
        }

        // loop through the columns, and only compare the last entry
        const auto max_element_iter =
            std::max_element(model.viterbi[parent_timestamp_index].begin(),
                             model.viterbi[parent_timestamp_index].end());

        std::size_t parent_candidate_index =
            std::distance(model.viterbi[parent_timestamp_index].begin(), max_element_iter);

        std::deque<std::pair<std::size_t, std::size_t>> reconstructed_indices;
        while (parent_timestamp_index > sub_matching_begin)
        {
            reconstructed_indices.emplace_front(parent_timestamp_index, parent_candidate_index);
            model.viterbi_reachable[parent_timestamp_index][parent_candidate_index] = true;
            const auto &next = model.parents[parent_timestamp_index][parent_candidate_index];
            // make sure we can never get stuck in this loop
            if (parent_timestamp_index == next.first)
            {
                break;
            }
            parent_timestamp_index = next.first;
            parent_candidate_index = next.second;
        }
        reconstructed_indices.emplace_front(parent_timestamp_index, parent_candidate_index);
        model.viterbi_reachable[parent_timestamp_index][parent_candidate_index] = true;
        if (reconstructed_indices.size() < 2)
        {
            sub_matching_begin = sub_matching_end;
            continue;
        }

        // fill viterbi reachability matrix
        for (const auto s_last :
             util::irange<std::size_t>(0UL, model.viterbi[sub_matching_last_timestamp].size()))
        {
            parent_timestamp_index = sub_matching_last_timestamp;
            parent_candidate_index = s_last;
            while (parent_timestamp_index > sub_matching_begin)
            {
                if (model.viterbi_reachable[parent_timestamp_index][parent_candidate_index] ||
                    model.pruned[parent_timestamp_index][parent_candidate_index])
                {
                    break;
                }
                model.viterbi_reachable[parent_timestamp_index][parent_candidate_index] = true;
                const auto &next = model.parents[parent_timestamp_index][parent_candidate_index];
                parent_timestamp_index = next.first;
                parent_candidate_index = next.second;
            }
            model.viterbi_reachable[parent_timestamp_index][parent_candidate_index] = true;
        }

        auto matching_distance = 0.0;
        auto trace_distance = 0.0;
        matching.nodes.reserve(reconstructed_indices.size());
        matching.indices.reserve(reconstructed_indices.size());
        for (const auto &idx : reconstructed_indices)
        {
            const auto timestamp_index = idx.first;
            const auto location_index = idx.second;

            matching.indices.push_back(timestamp_index);
            matching.nodes.push_back(candidates_list[timestamp_index][location_index].phantom_node);
            auto const routes_count =
                std::accumulate(model.viterbi_reachable[timestamp_index].begin(),
                                model.viterbi_reachable[timestamp_index].end(),
                                0);
            BOOST_ASSERT(routes_count > 0);
            // we don't count the current route in the "alternatives_count" parameter
            matching.alternatives_count.push_back(routes_count - 1);
            matching_distance += model.path_distances[timestamp_index][location_index];
        }
        util::for_each_pair(
            reconstructed_indices,
            [&trace_distance, &trace_coordinates](const std::pair<std::size_t, std::size_t> &prev,
                                                  const std::pair<std::size_t, std::size_t> &curr) {
                trace_distance += util::coordinate_calculation::haversineDistance(
                    trace_coordinates[prev.first], trace_coordinates[curr.first]);
            });

        matching.confidence = confidence(trace_distance, matching_distance);

        sub_matchings.push_back(matching);
        sub_matching_begin = sub_matching_end;
    }

    return sub_matchings;
}

} // namespace routing_algorithms
} // namespace engine
} // namespace osrm

//[1] "Hidden Markov Map Matching Through Noise and Sparseness"; P. Newson and J. Krumm; 2009; ACM
// GIS