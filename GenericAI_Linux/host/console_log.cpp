#include "console_log.h"
#include "file_logger.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <fstream>
#include <iostream>
#include <mutex>

#include <unistd.h>

namespace gai_host {

std::atomic<bool> ConsoleLog::Enabled{false};
std::atomic<bool> ConsoleLog::NativeDebug{false};

namespace {

// One lock for stdout and stderr so lines from worker threads don't interleave.
std::mutex g_console_mtx;

std::string Trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ParseBool(const std::string& val) {
    const std::string v = Lower(val);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

}  // namespace

std::string ExeDir() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    std::string path(buf, static_cast<std::size_t>(n));
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    return path.substr(0, slash);
}

void ConsoleLog::LoadFromConfig() {
    try {
        std::string dir = ExeDir();
        std::ifstream in((dir.empty() ? std::string(".") : dir) + "/" + kConfigFileName);
        if (!in) return;

        std::string raw;
        while (std::getline(in, raw)) {
            // key = value, with '#' or '//' comments and blank lines ignored.
            std::string line = Trim(raw);
            if (line.empty() || line[0] == '#' || line.rfind("//", 0) == 0) continue;
            std::size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            std::string key = Lower(Trim(line.substr(0, eq)));
            std::string val = Trim(line.substr(eq + 1));

            if (key == "show_debug")
                Enabled = ParseBool(val);
            else if (key == "show_native_debug")
                NativeDebug = ParseBool(val);
            else if (key == "log_to_file")
                FileLogger::Enabled = ParseBool(val);
            else if (key == "log_dir")
                FileLogger::SetDirectoryOverride(val);
        }
    } catch (...) {
        // A missing/broken config must never break startup: leave flags as-is.
    }
}

void ConsoleLog::WriteLine(const std::string& line) {
    if (!Enabled.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(g_console_mtx);
    std::cout << line << std::endl;
}

void ConsoleLog::ErrorLine(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_console_mtx);
    std::cerr << line << std::endl;
}

}  // namespace gai_host
