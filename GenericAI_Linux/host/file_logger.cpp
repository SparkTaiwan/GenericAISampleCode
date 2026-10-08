#include "file_logger.h"
#include "console_log.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <cerrno>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace gai_host {

std::atomic<bool> FileLogger::Enabled{false};

namespace {

constexpr long long kMaxFileBytes = 5LL * 1024 * 1024;
constexpr int kMaxBackupFiles = 3;
constexpr std::size_t kMaxQueuedLines = 10000;
// Also the writer's idle wake-up; bounds how long a line can sit unflushed
// (ERROR flushes immediately).
constexpr auto kFlushInterval = std::chrono::milliseconds(250);

struct Entry {
    std::string line;
    bool is_error = false;
};

std::mutex g_mtx;
std::condition_variable g_cv;
std::deque<Entry> g_queue;
std::thread g_writer;
bool g_started = false;
bool g_shutting_down = false;
std::atomic<long long> g_dropped{0};

std::string g_dir_override;
std::string g_main_name = "GenericAI.log";
std::string g_error_name = "error.log";

bool MakeDirs(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    std::size_t pos = 0;
    while (pos != std::string::npos) {
        pos = path.find('/', pos + 1);
        cur = path.substr(0, pos);
        if (cur.empty()) continue;
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return access(path.c_str(), W_OK) == 0;
}

// Picks the log directory on first use (writer thread only).
std::string ResolveLogDir() {
    std::vector<std::string> candidates;
    if (!g_dir_override.empty()) candidates.push_back(g_dir_override);
    if (const char* env = std::getenv("GENERICAI_LOG_DIR")) {
        if (*env) candidates.push_back(env);
    }
    candidates.push_back("/var/log/spark/GenericAI");
    std::string exe = ExeDir();
    if (!exe.empty()) candidates.push_back(exe + "/Logs");
    for (const auto& c : candidates) {
        if (MakeDirs(c)) return c;
    }
    return ".";
}

std::string Now() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    struct tm tmv;
    localtime_r(&tv.tv_sec, &tmv);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                  static_cast<int>(tv.tv_usec / 1000));
    return buf;
}

std::string FormatLine(const char* level, const std::string& message) {
    std::string s;
    s.reserve(message.size() + 40);
    s += Now();
    s += " [";
    s += level;
    s += "] ";
    s += message;
    s += '\n';
    return s;
}

// One log target, used from the writer thread only (no locking). Opened
// lazily on first write; rotation is judged from a running byte count.
class LogFile {
public:
    explicit LogFile(std::string path) : path_(std::move(path)) {}
    ~LogFile() { Close(); }

    void Write(const std::string& line) {
        if (!fp_ && !Open()) return;
        if (bytes_ >= kMaxFileBytes) {
            Rotate();
            if (!fp_ && !Open()) return;
        }
        if (std::fwrite(line.data(), 1, line.size(), fp_) != line.size()) {
            Close();  // next line retries from Open
            return;
        }
        bytes_ += static_cast<long long>(line.size());
    }

    void Flush() {
        if (fp_ && std::fflush(fp_) != 0) Close();
    }

    void Close() {
        if (fp_) std::fclose(fp_);
        fp_ = nullptr;
    }

private:
    bool Open() {
        fp_ = std::fopen(path_.c_str(), "a");
        if (!fp_) return false;
        struct stat st;
        bytes_ = (stat(path_.c_str(), &st) == 0) ? static_cast<long long>(st.st_size) : 0;
        return true;
    }

    void Rotate() {
        Close();
        std::remove((path_ + "." + std::to_string(kMaxBackupFiles)).c_str());
        for (int i = kMaxBackupFiles - 1; i >= 1; --i) {
            std::rename((path_ + "." + std::to_string(i)).c_str(),
                        (path_ + "." + std::to_string(i + 1)).c_str());
        }
        // If this fails we keep appending to the oversized file; the next
        // threshold crossing retries.
        std::rename(path_.c_str(), (path_ + ".1").c_str());
    }

    std::string path_;
    std::FILE* fp_ = nullptr;
    long long bytes_ = 0;
};

void WriterLoop() {
    const std::string dir = ResolveLogDir();
    LogFile main_log(dir + "/" + g_main_name);
    LogFile error_log(dir + "/" + g_error_name);

    bool dirty = false;
    auto last_flush = std::chrono::steady_clock::now();
    while (true) {
        Entry e;
        bool got = false;
        bool done = false;
        {
            std::unique_lock<std::mutex> lk(g_mtx);
            g_cv.wait_for(lk, kFlushInterval,
                          [] { return !g_queue.empty() || g_shutting_down; });
            if (!g_queue.empty()) {
                e = std::move(g_queue.front());
                g_queue.pop_front();
                got = true;
            } else if (g_shutting_down) {
                done = true;
            }
        }
        if (done) break;

        bool force_flush = false;
        if (got) {
            long long drops = g_dropped.exchange(0);
            if (drops > 0) {
                main_log.Write(FormatLine("WARN", "FileLogger queue full: dropped " +
                                                      std::to_string(drops) + " line(s)"));
            }
            main_log.Write(e.line);
            if (e.is_error) {
                error_log.Write(e.line);
                error_log.Flush();
                force_flush = true;
            }
            dirty = true;
        }

        auto now = std::chrono::steady_clock::now();
        if (dirty && (force_flush || now - last_flush >= kFlushInterval)) {
            main_log.Flush();
            dirty = false;
            last_flush = now;
        }
    }
    main_log.Flush();
}

void Enqueue(Entry&& e) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_shutting_down) return;
    if (!g_started) {
        g_started = true;
        g_writer = std::thread(WriterLoop);
    }
    if (g_queue.size() >= kMaxQueuedLines) {
        g_dropped.fetch_add(1);
        return;
    }
    g_queue.push_back(std::move(e));
    g_cv.notify_one();
}

void Write(const char* level, const std::string& message) {
    if (!FileLogger::Enabled.load(std::memory_order_relaxed)) return;
    try {
        Entry e;
        e.line = FormatLine(level, message);
        e.is_error = (level[0] == 'E');
        Enqueue(std::move(e));
    } catch (...) {
        // Logging must never propagate.
    }
}

}  // namespace

void FileLogger::SetDirectoryOverride(const std::string& dir) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_dir_override = dir;
    while (!g_dir_override.empty() && g_dir_override.back() == '/' && g_dir_override.size() > 1)
        g_dir_override.pop_back();
}

void FileLogger::Init(int basePort) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_main_name = "GenericAI-" + std::to_string(basePort) + ".log";
    g_error_name = "error-" + std::to_string(basePort) + ".log";
}

void FileLogger::Info(const std::string& message)  { Write("INFO", message); }
void FileLogger::Warn(const std::string& message)  { Write("WARN", message); }
void FileLogger::Error(const std::string& message) { Write("ERROR", message); }

void FileLogger::Shutdown() {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        if (g_shutting_down) return;
        g_shutting_down = true;
    }
    g_cv.notify_all();
    if (g_writer.joinable()) g_writer.join();
}

}  // namespace gai_host
