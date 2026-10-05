#include "cairn/log.h"

#include <cstdarg>
#include <cstdio>

namespace Cairn {

namespace {
std::mutex g_mutex;
std::vector<LogEntry> g_buffer;
uint64_t g_nextId = 1;
bool g_echoStdout = true;
}  // namespace

void Log::SetEchoStdout(bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_echoStdout = on;
}

bool Log::EchoStdout() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_echoStdout;
}

void Log::Push(LogLevel level, const std::string& text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_buffer.push_back({g_nextId++, level, text});
    if (g_buffer.size() > 1024) g_buffer.erase(g_buffer.begin(), g_buffer.begin() + 256);
    if (g_echoStdout) {
        static const char* kTags[] = {"[info]", "[warn]", "[error]"};
        std::fprintf(stdout, "%s %s\n", kTags[(int)level], text.c_str());
        std::fflush(stdout);
    }
}

void Log::Write(LogLevel level, const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n <= 0) return;
    Push(level, std::string(buf, (size_t)std::min(n, (int)sizeof(buf) - 1)));
}

void Log::Info(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n <= 0) return;
    Push(LogLevel::Info, std::string(buf, (size_t)std::min(n, (int)sizeof(buf) - 1)));
}

void Log::Warning(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n <= 0) return;
    Push(LogLevel::Warning, std::string(buf, (size_t)std::min(n, (int)sizeof(buf) - 1)));
}

void Log::Error(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n <= 0) return;
    Push(LogLevel::Error, std::string(buf, (size_t)std::min(n, (int)sizeof(buf) - 1)));
}

std::vector<LogEntry> Log::Poll(uint64_t afterId, uint32_t maxCount) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<LogEntry> out;
    for (const auto& e : g_buffer) {
        if (e.id <= afterId) continue;
        out.push_back(e);
        if (out.size() >= maxCount) break;
    }
    return out;
}

}  // namespace Cairn
