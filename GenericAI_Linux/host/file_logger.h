#pragma once

#include <atomic>
#include <string>

namespace gai_host {

// Asynchronous file logger. Producers only format the line and enqueue it
// (bounded queue, never blocks, no file I/O on the caller's thread); a single
// background thread drains the queue into long-lived streams.
//
// Log directory (first one that can be created and written wins):
//   1. log_dir in GenericAI.Config, else the GENERICAI_LOG_DIR environment variable
//   2. /var/log/spark/GenericAI
//   3. <exe dir>/Logs
// Files:
//   GenericAI-<basePort>.log   INFO / WARN / ERROR
//   error-<basePort>.log       ERROR duplicated for quick triage
// File names carry the base port so concurrent instances never share a file.
// The directory is created on first actual write, so a disabled logger leaves
// no trace on disk. Logging failures are swallowed.
class FileLogger {
public:
    // log_to_file in GenericAI.Config. Off by default.
    static std::atomic<bool> Enabled;

    static void SetDirectoryOverride(const std::string& dir);

    // Call once at startup before the first log line. Only selects names.
    static void Init(int basePort);

    static void Info(const std::string& message);
    static void Warn(const std::string& message);
    static void Error(const std::string& message);

    // Stops accepting lines, drains what is queued, flushes and closes.
    static void Shutdown();
};

}  // namespace gai_host
