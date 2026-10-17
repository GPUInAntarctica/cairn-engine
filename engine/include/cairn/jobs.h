// Cairn Engine - cooperative job system.
//
// One process-wide worker pool backs Cairn::Jobs::ParallelFor. Indices are
// handed out dynamically, so an uneven body still balances across the pool, and
// the calling thread participates (a ParallelFor of one long index still runs).
//
// Work must be *index-independent*: `body(i)` must not depend on what any other
// index does, and must not touch the same memory as another index. Callers that
// need a deterministic result write into a per-index slot and merge in index
// order afterwards. A body must not call ParallelFor again — a nested dispatch
// would clobber the in-flight task; nested calls therefore run serially.
#pragma once

#include <cstdint>
#include <functional>

namespace Cairn {
namespace Jobs {

// Workers in the shared pool, including the calling thread (>= 1).
int WorkerCount();

// Runs body(0..count-1) across the pool and blocks until every index is done.
// count <= 0 is a no-op.
void ParallelFor(int count, const std::function<void(int)>& body);

// Number of ParallelFor dispatches / indices executed since the pool started.
// Read by the profiler and `engine.capabilities`.
struct Stats {
    int workers = 1;
    uint64_t dispatches = 0;
    uint64_t indices = 0;
};
Stats Snapshot();

// Test/telemetry hook: resets the dispatch counters (not the workers).
void ResetStats();

}  // namespace Jobs
}  // namespace Cairn
