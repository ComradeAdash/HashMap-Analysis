#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if __has_include(<valgrind/cachegrind.h>)
#include <valgrind/cachegrind.h>
#if defined(CACHEGRIND_START_INSTRUMENTATION) && defined(CACHEGRIND_STOP_INSTRUMENTATION)
#define HASHBENCH_CACHEGRIND 1
#endif
#endif

namespace hashbench {

struct CounterResult {
    std::uint64_t time_enabled_ns = 0;
    std::uint64_t time_running_ns = 0;
    std::vector<std::pair<std::string, std::uint64_t>> raw;
};

class ProfileRegion {
    std::string mode_;
    std::vector<int> fds_;
    std::vector<std::string> names_;

    void close_all() noexcept {
#if defined(__linux__)
        for (auto fd : fds_) ::close(fd);
#endif
        fds_.clear();
    }

public:
    ProfileRegion(const std::string& mode, const std::string& events) : mode_(mode) {
        if (mode == "cachegrind") {
#if defined(HASHBENCH_CACHEGRIND)
            if (!RUNNING_ON_VALGRIND) {
                throw std::runtime_error("cachegrind mode requires valgrind --tool=cachegrind --instr-at-start=no");
            }
#else
            throw std::runtime_error("Cachegrind region support requires Valgrind 3.22+ headers at build time; install headers and rebuild");
#endif
        }
        if (mode != "perf") return;
#if defined(__linux__)
        try {
            std::size_t begin = 0;
            while (begin < events.size()) {
                const auto end = events.find(',', begin);
                const auto name = events.substr(begin, end == std::string::npos ? end : end - begin);
                std::uint64_t config;
                if (name == "cycles") config = PERF_COUNT_HW_CPU_CYCLES;
                else if (name == "instructions") config = PERF_COUNT_HW_INSTRUCTIONS;
                else if (name == "branches") config = PERF_COUNT_HW_BRANCH_INSTRUCTIONS;
                else if (name == "branch-misses") config = PERF_COUNT_HW_BRANCH_MISSES;
                else if (name == "cache-references") config = PERF_COUNT_HW_CACHE_REFERENCES;
                else if (name == "cache-misses") config = PERF_COUNT_HW_CACHE_MISSES;
                else throw std::invalid_argument("unsupported perf event: " + name);
                perf_event_attr attr{};
                attr.type = PERF_TYPE_HARDWARE;
                attr.size = sizeof(attr);
                attr.config = config;
                attr.disabled = 1;
                // Count this thread's user code and leave the kernel out
                attr.exclude_kernel = 1;
                attr.exclude_hv = 1;
                attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                                   PERF_FORMAT_TOTAL_TIME_RUNNING;
                const auto fd = static_cast<int>(::syscall(SYS_perf_event_open, &attr, 0, -1,
                    fds_.empty() ? -1 : fds_.front(), PERF_FLAG_FD_CLOEXEC));
                if (fd < 0) {
                    throw std::runtime_error("perf_event_open(" + name + "): " + std::strerror(errno) +
                        "; hardware counters may be restricted by this machine or container");
                }
                fds_.push_back(fd);
                names_.push_back(name);
                if (end == std::string::npos) break;
                begin = end + 1;
            }
            if (fds_.empty()) throw std::invalid_argument("at least one perf event is required");
        } catch (...) {
            close_all();
            throw;
        }
#else
        (void)events;
        throw std::runtime_error("hardware perf counters require Linux");
#endif
    }

    ProfileRegion(const ProfileRegion&) = delete;
    ProfileRegion& operator=(const ProfileRegion&) = delete;
    ~ProfileRegion() { close_all(); }

    void start() {
        // Counters stay off during input generation, map setup, and validation
#if defined(__linux__)
        if (mode_ == "perf") {
            if (::ioctl(fds_.front(), PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) < 0 ||
                ::ioctl(fds_.front(), PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) < 0) {
                throw std::runtime_error("could not enable perf counter group");
            }
        }
#endif
#if defined(HASHBENCH_CACHEGRIND)
        if (mode_ == "cachegrind") { CACHEGRIND_START_INSTRUMENTATION; }
#endif
    }

    CounterResult stop() {
        CounterResult result;
#if defined(HASHBENCH_CACHEGRIND)
        if (mode_ == "cachegrind") { CACHEGRIND_STOP_INSTRUMENTATION; }
#endif
#if defined(__linux__)
        if (mode_ == "perf") {
            if (::ioctl(fds_.front(), PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) < 0) {
                throw std::runtime_error("could not disable perf counter group");
            }
            std::vector<std::uint64_t> buffer(3 + fds_.size());
            const auto expected = buffer.size() * sizeof(std::uint64_t);
            const auto bytes = ::read(fds_.front(), buffer.data(), expected);
            if (bytes != static_cast<ssize_t>(expected) || buffer[0] != fds_.size()) {
                throw std::runtime_error("could not read complete perf counter group");
            }
            result.time_enabled_ns = buffer[1];
            result.time_running_ns = buffer[2];
            // A busy machine may only schedule the counters for part of the region
            if (!result.time_running_ns) {
                throw std::runtime_error("perf events did not run; try fewer --perf-events (e.g. branches,branch-misses)");
            }
            for (std::size_t i = 0; i < names_.size(); ++i) {
                result.raw.emplace_back(names_[i], buffer[3 + i]);
            }
        }
#endif
        return result;
    }
};

} // namespace hashbench
