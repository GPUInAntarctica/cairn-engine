// Cairn Engine - frame profiler implementation.
#include "cairn/profiler.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Cairn {
namespace Profiler {
namespace {

// Rolling window depth per scope (and for frame times): ~1s at 60fps.
constexpr size_t kWindow = 60;

struct ScopeState {
    double pendingMs = 0.0;  // accumulated since the last Frame()
    double lastMs = 0.0;     // total for the last committed frame
    double maxMs = 0.0;      // largest committed frame total seen
    uint64_t calls = 0;      // total samples taken
    std::vector<double> window;
    size_t head = 0;
};

struct OpenSample {
    std::string name;
    std::chrono::steady_clock::time_point t0;
};

struct State {
    std::mutex mutex;
    std::unordered_map<std::string, ScopeState> scopes;
    std::vector<std::string> order;  // insertion order keeps output stable
    std::vector<OpenSample> stack;
    std::vector<double> frameWindow;
    size_t frameHead = 0;
    uint64_t frame = 0;
    double lastFrameMs = 0.0;
    bool havePrev = false;
    std::chrono::steady_clock::time_point prev;
};

// Leaked on purpose: samples can outlive static destructors (e.g. a worker
// thread still finishing a frame while the library unloads).
State& S() {
    static State* state = new State();
    return *state;
}

double Mean(const std::vector<double>& v) {
    if (v.empty()) return 0.0;
    double sum = 0.0;
    for (double x : v) sum += x;
    return sum / (double)v.size();
}

void PushWindow(std::vector<double>& v, size_t& head, double value) {
    if (v.size() < kWindow) {
        v.push_back(value);
    } else {
        v[head] = value;
        head = (head + 1) % kWindow;
    }
}

}  // namespace

void BeginSample(const char* name) {
    State& s = S();
    OpenSample sample;
    sample.name = name ? name : "";
    sample.t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.stack.push_back(std::move(sample));
}

void EndSample() {
    auto now = std::chrono::steady_clock::now();
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.stack.empty()) return;
    OpenSample top = std::move(s.stack.back());
    s.stack.pop_back();
    const double ms = std::chrono::duration<double, std::milli>(now - top.t0).count();
    auto it = s.scopes.find(top.name);
    if (it == s.scopes.end()) {
        s.order.push_back(top.name);
        it = s.scopes.emplace(top.name, ScopeState{}).first;
    }
    it->second.pendingMs += ms;
    it->second.calls++;
}

void Frame() {
    auto now = std::chrono::steady_clock::now();
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.havePrev) {
        const double ms = std::chrono::duration<double, std::milli>(now - s.prev).count();
        s.lastFrameMs = ms;
        PushWindow(s.frameWindow, s.frameHead, ms);
    }
    s.prev = now;
    s.havePrev = true;
    for (auto& entry : s.scopes) {
        ScopeState& sc = entry.second;
        sc.lastMs = sc.pendingMs;
        sc.maxMs = std::max(sc.maxMs, sc.pendingMs);
        PushWindow(sc.window, sc.head, sc.pendingMs);
        sc.pendingMs = 0.0;
    }
    s.frame++;
}

void Reset() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.scopes.clear();
    s.order.clear();
    s.stack.clear();
    s.frameWindow.clear();
    s.frameHead = 0;
    s.frame = 0;
    s.lastFrameMs = 0.0;
    s.havePrev = false;
}

Json Snapshot() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    const double avgFrame = Mean(s.frameWindow);
    Json out = Json::MakeObject();
    out.set("frame", (double)s.frame);
    out.set("fps", avgFrame > 0.0 ? 1000.0 / avgFrame : 0.0);
    out.set("frameMs", avgFrame);
    out.set("lastFrameMs", s.lastFrameMs);

    // Report scopes heaviest-first so the top of the list is the bottleneck.
    std::vector<const std::pair<const std::string, ScopeState>*> list;
    list.reserve(s.order.size());
    for (const std::string& name : s.order) {
        auto it = s.scopes.find(name);
        if (it != s.scopes.end()) list.push_back(&*it);
    }
    std::sort(list.begin(), list.end(), [](const auto* a, const auto* b) {
        return Mean(a->second.window) > Mean(b->second.window);
    });
    Json scopes = Json::MakeArray();
    for (const auto* entry : list) {
        const ScopeState& sc = entry->second;
        Json o = Json::MakeObject();
        o.set("name", entry->first);
        o.set("ms", sc.lastMs);
        o.set("avgMs", Mean(sc.window));
        o.set("maxMs", sc.maxMs);
        o.set("calls", (double)sc.calls);
        scopes.push(o);
    }
    out.set("scopes", scopes);
    return out;
}

}  // namespace Profiler
}  // namespace Cairn
