#pragma once

#include <cstdint>

namespace taskforge {

// Explicit user-space admission budgets.  They are intentionally independent
// of host sampling and of cgroup v2 kernel enforcement limits.
struct ResourceCapacity {
    std::uint64_t cpu_millis;
    std::uint64_t memory_bytes;
};

struct ResourceRequest {
    std::uint64_t cpu_millis;
    std::uint64_t memory_bytes;
};

struct ResourceSnapshot {
    ResourceCapacity capacity;
    ResourceCapacity reserved;
    ResourceCapacity available;
};

}  // namespace taskforge
