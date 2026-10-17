// Cairn Engine - frame profiler.
//
// Named samples (BeginSample/EndSample, or the CAIRN_PROFILE RAII helper) are
// aggregated per name into a rolling window. The engine commits a frame at the
// end of Engine::Update; the CLI reads Snapshot(). Samples may be taken from
// any thread.
#pragma once

#include "json.h"

namespace Cairn {

namespace Profiler {

// Opens/closes a named sample. Samples nest like a call tree; EndSample closes
// the most recently opened one. Cheap enough to leave in shipping builds.
void BeginSample(const char* name);
void EndSample();

// Scoped RAII sample: `Profiler::ScopedSample s("physics");`
struct ScopedSample {
    explicit ScopedSample(const char* name) { BeginSample(name); }
    ~ScopedSample() { EndSample(); }
};

// Commits one frame: rolls every scope's accumulated time into its window and
// advances the frame counter. Frame time is measured from the previous Frame()
// call.
void Frame();

// Drops every accumulated scope and window (used between tests).
void Reset();

// JSON: {frame, fps, frameMs, lastFrameMs, scopes:[{name, ms, avgMs, maxMs,
// calls}], threads}. `ms` is the scope's total for the last committed frame.
Json Snapshot();

}  // namespace Profiler

}  // namespace Cairn

#define CAIRN_PROFILE_CONCAT_(a, b) a##b
#define CAIRN_PROFILE_CONCAT(a, b) CAIRN_PROFILE_CONCAT_(a, b)
#define CAIRN_PROFILE(name) \
    ::Cairn::Profiler::ScopedSample CAIRN_PROFILE_CONCAT(_cairn_profile_, __LINE__)(name)
