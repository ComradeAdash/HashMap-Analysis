#include "hashbench/dataset.hpp"
#include "hashbench/library_maps.hpp"
#include "hashbench/maps.hpp"
#include "hashbench/profile.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

namespace hb = hashbench;

struct Options {
    std::string map = "linear";
    std::string dataset = "u64";
    std::string distribution = "random";
    std::string operation = "insert";
    std::string mode = "reserved";
    std::string hash = "mixed";
    std::string profile = "none";
    std::string perf_events = "cycles,instructions,branches,branch-misses,cache-references,cache-misses";
    std::size_t count = 200000;
    std::uint64_t seed = 42;
    double max_load = 0.8;
    bool diagnostics = false;
};

void help() {
    std::cout << R"(Hashmap insertion and lookup benchmark (one case per process)
  --map linear|chain|std|sparse
  --dataset u64|string-person
  --distribution sequential|shuffled|random   (all keys are unique)
  --operation insert|find-hit|find-miss
  --mode reserved|growing
  --hash mixed|weak          weak = integer identity / string byte sum
  --count N                 positive number of entries and operations
  --seed N                  uint64 seed, default 42
  --max-load F              strictly between 0 and 1, default 0.8
  --diagnostics             calculate custom-table statistics outside timing
  --profile none|perf|cachegrind
  --perf-events LIST        comma-separated hardware events
  --help
Writes one JSON object to stdout. Errors go to stderr with nonzero exit.
Use scripts/run_benchmarks.py for repetitions, CSV summaries, and metadata.
)";
}

std::uint64_t unsigned_number(const std::string& value) {
    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("expected an unsigned integer: " + value);
    }
    std::size_t consumed;
    const auto result = std::stoull(value, &consumed);
    if (consumed != value.size()) throw std::invalid_argument("invalid integer");
    return result;
}

void choice(const std::string& name, const std::string& value,
            std::initializer_list<const char*> valid) {
    for (const auto* item : valid) if (value == item) return;
    throw std::invalid_argument("invalid " + name + ": " + value);
}

Options parse(int argc, char** argv) {
    Options opt;
    std::set<std::string> seen;
    for (int i = 1; i < argc; ++i) {
        const std::string name = argv[i];
        if (name == "--help") { help(); std::exit(0); }
        if (!seen.insert(name).second) throw std::invalid_argument("repeated option: " + name);
        if (name == "--diagnostics") { opt.diagnostics = true; continue; }
        if (i + 1 == argc) throw std::invalid_argument("missing value for " + name);
        const std::string value = argv[++i];
        if (name == "--map") opt.map = value;
        else if (name == "--dataset") opt.dataset = value;
        else if (name == "--distribution") opt.distribution = value;
        else if (name == "--operation") opt.operation = value;
        else if (name == "--mode") opt.mode = value;
        else if (name == "--hash") opt.hash = value;
        else if (name == "--profile") opt.profile = value;
        else if (name == "--perf-events") opt.perf_events = value;
        else if (name == "--count") {
            const auto n = unsigned_number(value);
            if (!n || n > std::numeric_limits<std::size_t>::max() / 2) {
                throw std::invalid_argument("count must be positive and fit in half of size_t");
            }
            opt.count = static_cast<std::size_t>(n);
        } else if (name == "--seed") opt.seed = unsigned_number(value);
        else if (name == "--max-load") {
            std::size_t consumed;
            opt.max_load = std::stod(value, &consumed);
            if (consumed != value.size()) throw std::invalid_argument("invalid max load");
        } else throw std::invalid_argument("unknown option: " + name);
    }
    choice("map", opt.map, {"linear", "chain", "std", "sparse"});
    choice("dataset", opt.dataset, {"u64", "string-person"});
    choice("distribution", opt.distribution, {"sequential", "shuffled", "random"});
    choice("operation", opt.operation, {"insert", "find-hit", "find-miss"});
    choice("mode", opt.mode, {"reserved", "growing"});
    choice("hash", opt.hash, {"mixed", "weak"});
    choice("profile", opt.profile, {"none", "perf", "cachegrind"});
    hb::validate_load(opt.max_load);
    if (static_cast<float>(opt.max_load) <= 0 || static_cast<float>(opt.max_load) >= 1) {
        throw std::invalid_argument("max load must also remain strictly between 0 and 1 in float precision (library policy)");
    }
    return opt;
}

template<class T>
void optional_json(const std::optional<T>& value) {
    if (value) std::cout << *value;
    else std::cout << "null";
}

template<class Map, class Key, class Value>
int execute(const Options& opt, Map& map, const hb::Dataset<Key, Value>& data) {
    const auto initial_capacity = map.capacity();
    auto insert_all = [&] {
        for (const auto& entry : data.entries) map.insert_or_assign(entry.first, entry.second);
    };
    // Fill the map before timing lookups so setup doesn't enter the measurements
    if (opt.operation != "insert") insert_all();
    const auto before_capacity = map.capacity();
    hb::ProfileRegion region(opt.profile, opt.perf_events);
    std::uint64_t checksum = 0;
    std::size_t hits = 0;
    // Call the timer once before profiling to get its first-call setup out of the way
    (void)std::chrono::steady_clock::now();
    region.start();
    const auto start = std::chrono::steady_clock::now();
    if (opt.operation == "insert") {
        insert_all();
    } else {
        const auto& queries = opt.operation == "find-hit" ? data.hit_queries : data.miss_queries;
        for (const auto& key : queries) {
            if (const auto* value = map.find(key)) {
                ++hits;
                checksum += hb::value_checksum(*value);
            }
        }
    }
    const auto finish = std::chrono::steady_clock::now();
    const auto counters = region.stop();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();

    // Check every value after timing so incorrect results don't look like a speedup
    // Reading the final map also keeps the compiler from discarding the insertions
    if (map.size() != data.entries.size()) throw std::runtime_error("incorrect final map size");
    std::uint64_t expected_checksum = 0;
    for (const auto& entry : data.entries) {
        const auto* value = map.find(entry.first);
        if (!value || *value != entry.second) throw std::runtime_error("incorrect mapped value");
        expected_checksum += hb::value_checksum(entry.second);
    }
    if (opt.operation == "find-hit" && (hits != opt.count || checksum != expected_checksum)) {
        throw std::runtime_error("incorrect successful lookup result");
    }
    if (opt.operation == "find-miss" && hits != 0) throw std::runtime_error("incorrect unsuccessful lookup result");
    if (opt.operation == "insert") checksum = expected_checksum;
    if (opt.mode == "reserved" && map.capacity() != initial_capacity) {
        throw std::runtime_error("reserved map unexpectedly resized; no result emitted");
    }
    const auto diagnostic = opt.diagnostics ? map.diagnostics() : hb::Diagnostics{};
    std::cout << std::setprecision(17)
        << "{\"schema_version\":1,\"map\":" << std::quoted(opt.map)
        << ",\"dataset\":" << std::quoted(opt.dataset)
        << ",\"distribution\":" << std::quoted(opt.distribution)
        << ",\"operation\":" << std::quoted(opt.operation)
        << ",\"mode\":" << std::quoted(opt.mode)
        << ",\"hash\":" << std::quoted(opt.hash)
        << ",\"profile\":" << std::quoted(opt.profile)
        << ",\"count\":" << opt.count << ",\"seed\":" << opt.seed
        << ",\"max_load\":" << opt.max_load
        << ",\"fingerprint\":" << std::quoted(std::to_string(data.fingerprint))
        << ",\"checksum\":" << std::quoted(std::to_string(checksum))
        << ",\"size\":" << map.size()
        << ",\"initial_capacity\":" << initial_capacity
        << ",\"capacity_before_operation\":" << before_capacity
        << ",\"final_capacity\":" << map.capacity()
        << ",\"actual_load\":" << map.load_factor()
        << ",\"elapsed_ns\":" << elapsed_ns
        << ",\"ns_per_op\":" << static_cast<double>(elapsed_ns) / opt.count
        << ",\"max_chain_length\":";
    optional_json(diagnostic.max_chain_length);
    std::cout << ",\"max_successful_probes\":";
    optional_json(diagnostic.max_successful_probes);
    std::cout << ",\"mean_successful_probes\":";
    optional_json(diagnostic.mean_successful_probes);
    std::cout << ",\"compiler\":" << std::quoted(__VERSION__)
        << ",\"pointer_bits\":" << sizeof(void*) * 8
        << ",\"perf_time_enabled_ns\":" << counters.time_enabled_ns
        << ",\"perf_time_running_ns\":" << counters.time_running_ns
        << ",\"counters_raw\":{";
    bool first = true;
    for (const auto& item : counters.raw) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << std::quoted(item.first) << ':' << item.second;
    }
    std::cout << "}}\n";
    return 0;
}

template<class Key, class Value>
int run(const Options& opt) {
    auto data = hb::make_dataset<Key, Value>(opt.count, opt.distribution, opt.seed);
    const hb::SharedHash<Key> hash{opt.hash == "weak"};
    const bool reserved = opt.mode == "reserved";
    const auto capacity = reserved ? hb::capacity_for(opt.count, opt.max_load) : 8;
    if (opt.map == "linear") {
        hb::LinearMap<Key, Value, hb::SharedHash<Key>> map(capacity, opt.max_load, !reserved, hash);
        return execute(opt, map, data);
    }
    if (opt.map == "chain") {
        hb::ChainedMap<Key, Value, hb::SharedHash<Key>> map(capacity, opt.max_load, !reserved, hash);
        return execute(opt, map, data);
    }
    if (opt.map == "std") {
        hb::LibraryMap<Key, Value, hb::SharedHash<Key>, false> map(opt.count, opt.max_load, reserved, hash);
        return execute(opt, map, data);
    }
    hb::LibraryMap<Key, Value, hb::SharedHash<Key>, true> map(opt.count, opt.max_load, reserved, hash);
    return execute(opt, map, data);
}

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        if (options.dataset == "u64") return run<std::uint64_t, std::uint64_t>(options);
        return run<std::string, hb::Person*>(options);
    } catch (const std::exception& error) {
        std::cerr << "hashmap_bench: " << error.what() << '\n';
        return 1;
    }
}
