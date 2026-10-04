// Cairn Engine - log sink. Engine and gameplay code write here; the editor
// console polls the ring buffer over the native addon boundary.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace Cairn {

enum class LogLevel : int { Info = 0, Warning = 1, Error = 2 };

struct LogEntry {
    uint64_t id = 0;
    LogLevel level = LogLevel::Info;
    std::string text;
};

class Log {
public:
    // Thread-safe printf-style logging.
    static void Write(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    static void Info(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    static void Warning(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
    static void Error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

    // Returns entries with id greater than `afterId`, oldest first. The buffer
    // holds the most recent 1024 entries regardless of polling.
    static std::vector<LogEntry> Poll(uint64_t afterId, uint32_t maxCount = 512);
    static void SetEchoStdout(bool on);

private:
    static void Push(LogLevel level, const std::string& text);
};

}  // namespace Cairn
