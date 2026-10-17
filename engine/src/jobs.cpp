// Cairn Engine - cooperative job system.
#include "cairn/jobs.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace Cairn {
namespace Jobs {
namespace {

// A `ParallelFor` body must not dispatch again: task_ points at the caller's
// frame, so a nested dispatch would clobber it. This thread-local flag turns a
// nested call into a serial loop instead. It is set while this thread is inside
// a body (the caller participates in Work() too).
thread_local bool t_inDispatch = false;

class Pool {
public:
    void Start(int n) {
        std::lock_guard<std::mutex> lock(m_);
        for (int i = 0; i < std::max(1, n); ++i)
            threads_.emplace_back([this] { WorkerLoop(); });
    }

    // Runs f(0..count-1) across the pool and blocks until done. The calling
    // thread participates in the work.
    void ParallelFor(int count, const std::function<void(int)>& f) {
        if (count <= 0) return;
        if (threads_.empty() || t_inDispatch) {
            // No pool, or a nested dispatch: run on this thread only.
            const bool outer = t_inDispatch;
            t_inDispatch = true;
            for (int i = 0; i < count; ++i) f(i);
            t_inDispatch = outer;
            return;
        }
        // task_ points at the caller's stack frame, so no worker may still be
        // inside Work() when this returns: the next dispatch would overwrite
        // it, and a straggler still looping would call into a destroyed
        // std::function. Waiting for pending_ == 0 is not enough -- a worker
        // that has already claimed its last index decrements pending_ and is
        // still one fetch_add away from leaving the loop. So the wait also
        // requires every worker to have left the dispatch loop (retired_).
        const int workers = (int)threads_.size();
        task_.store(&f, std::memory_order_relaxed);
        jobCount_.store(count, std::memory_order_relaxed);
        next_.store(0, std::memory_order_relaxed);
        pending_.store(count, std::memory_order_relaxed);
        retired_.store(0, std::memory_order_relaxed);
        workerTotal_ = workers;
        {
            std::lock_guard<std::mutex> lock(m_);
            epoch_++;
        }
        wake_.notify_all();
        Work(false);   // the caller participates but is not one of the workers
        std::unique_lock<std::mutex> lock(m_);
        done_.wait(lock, [this] {
            return pending_.load(std::memory_order_acquire) == 0 &&
                   retired_.load(std::memory_order_acquire) == workerTotal_;
        });
    }

    int workerCount() const { return (int)threads_.size() + 1; }

private:
    // `countsRetirement` is false for the calling thread, which is not one of
    // the `workerTotal_` workers ParallelFor waits on.
    void Work(bool countsRetirement) {
        const auto* task = task_.load(std::memory_order_acquire);
        const int count = jobCount_.load(std::memory_order_acquire);
        const bool outer = t_inDispatch;
        t_inDispatch = true;
        while (true) {
            int i = next_.fetch_add(1, std::memory_order_relaxed);
            if (i >= count) break;
            (*task)(i);
            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> lock(m_);
                done_.notify_all();
            }
        }
        t_inDispatch = outer;
        if (countsRetirement && retired_.fetch_add(1, std::memory_order_acq_rel) + 1 == workerTotal_) {
            std::lock_guard<std::mutex> lock(m_);
            done_.notify_all();
        }
    }
    void WorkerLoop() {
        uint64_t seen = 0;
        while (true) {
            {
                std::unique_lock<std::mutex> lock(m_);
                wake_.wait(lock, [this, &seen] { return stop_ || epoch_ != seen; });
                if (stop_) return;
                seen = epoch_;
            }
            Work(true);
        }
    }

    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable wake_, done_;
    std::atomic<const std::function<void(int)>*> task_{nullptr};
    std::atomic<int> jobCount_{0};
    std::atomic<int> next_{0};
    std::atomic<int> pending_{0};
    // Workers that have left the dispatch loop for the current epoch.
    std::atomic<int> retired_{0};
    // Size of the pool for the in-flight dispatch; read by workers only after
    // they observe the epoch bump, and never changes mid-epoch.
    int workerTotal_ = 0;
    uint64_t epoch_ = 0;
    bool stop_ = false;
};

// One shared pool for the whole process, created on first use and never torn
// down (worker threads are cheap to keep; the engine outlives any scene).
// CAIRN_JOBS_WORKERS caps the pool size; 1 (or 0) runs everything on the
// calling thread, which is how the benchmark harness measures the speedup.
Pool& Shared() {
    static Pool* pool = [] {
        Pool* p = new Pool();
        int hw = (int)std::thread::hardware_concurrency();
        int n = std::clamp(hw <= 0 ? 4 : hw, 1, 16);
        if (const char* env = std::getenv("CAIRN_JOBS_WORKERS")) {
            int want = std::atoi(env);
            if (want > 0) n = std::clamp(want, 1, 64);
        }
        if (n <= 1) return p;   // serial: no worker threads, ParallelFor inlines
        p->Start(n - 1);        // Start() spawns workers; the caller is the +1
        return p;
    }();
    return *pool;
}

std::atomic<uint64_t> g_dispatches{0};
std::atomic<uint64_t> g_indices{0};

}  // namespace

int WorkerCount() { return Shared().workerCount(); }

void ParallelFor(int count, const std::function<void(int)>& body) {
    if (count <= 0) return;
    g_dispatches.fetch_add(1, std::memory_order_relaxed);
    g_indices.fetch_add((uint64_t)count, std::memory_order_relaxed);
    Shared().ParallelFor(count, body);
}

Stats Snapshot() {
    Stats s;
    s.workers = WorkerCount();
    s.dispatches = g_dispatches.load(std::memory_order_relaxed);
    s.indices = g_indices.load(std::memory_order_relaxed);
    return s;
}

void ResetStats() {
    g_dispatches.store(0, std::memory_order_relaxed);
    g_indices.store(0, std::memory_order_relaxed);
}

}  // namespace Jobs
}  // namespace Cairn
