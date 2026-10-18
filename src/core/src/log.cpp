#include "h47vr/log.hpp"

#include "h47vr/paths.hpp"

#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace h47vr {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
std::wstring g_path;
std::string g_tag = "h47vr";
thread_local bool g_in_logger = false;

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "trace";
        case LogLevel::Debug: return "debug";
        case LogLevel::Info:  return "info";
        case LogLevel::Warn:  return "warn";
        case LogLevel::Error: return "error";
    }
    return "?";
}

}  // namespace

bool LogStart(const wchar_t* logDirectory, const char* tag) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        return true;
    }
    if (logDirectory == nullptr || logDirectory[0] == L'\0') {
        return false;
    }
    if (tag != nullptr && tag[0] != '\0') {
        g_tag = tag;
    }

    std::wstring directory(logDirectory);
    if (!EnsureDirectory(directory)) {
        return false;
    }

    SYSTEMTIME now{};
    ::GetLocalTime(&now);

    wchar_t name[128]{};
    ::swprintf_s(name, L"h47vr-%04u%02u%02u-%02u%02u%02u.log",
                 now.wYear, now.wMonth, now.wDay,
                 now.wHour, now.wMinute, now.wSecond);

    g_path = directory + L"\\" + name;

    // Opened with _SH_DENYNO rather than _wfopen_s so external tooling can tail
    // the log while the game is running. _wfopen_s opens with sharing that
    // blocks concurrent readers.
    int descriptor = -1;
    if (::_wsopen_s(&descriptor, g_path.c_str(),
                    _O_CREAT | _O_WRONLY | _O_BINARY | _O_TRUNC,
                    _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0) {
        g_path.clear();
        return false;
    }
    FILE* file = ::_wfdopen(descriptor, L"wb");
    if (file == nullptr) {
        ::_close(descriptor);
        g_path.clear();
        return false;
    }
    g_file = file;
    return true;
}

void LogStop() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != nullptr) {
        std::fclose(g_file);
        g_file = nullptr;
    }
}

void LogWriteV(LogLevel level, const char* format, std::va_list args) {
    char message[2048]{};
    ::_vsnprintf_s(message, sizeof(message), _TRUNCATE, format, args);

    SYSTEMTIME now{};
    ::GetLocalTime(&now);

    char line[2304]{};
    ::_snprintf_s(line, sizeof(line), _TRUNCATE,
                  "[%02u:%02u:%02u.%03u] [%-5s] [%s] %s\r\n",
                  now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                  LevelName(level), g_tag.c_str(), message);

    std::lock_guard<std::mutex> lock(g_mutex);
    g_in_logger = true;
    if (g_file != nullptr) {
        std::fputs(line, g_file);
        std::fflush(g_file);  // flush per line: the game may be terminated hard
    }
    ::OutputDebugStringA(line);
    g_in_logger = false;
}

bool LoggerActive() {
    return g_in_logger;
}

void LogWrite(LogLevel level, const char* format, ...) {
    std::va_list args;
    va_start(args, format);
    LogWriteV(level, format, args);
    va_end(args);
}

const wchar_t* LogFilePath() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_path.empty() ? nullptr : g_path.c_str();
}

}  // namespace h47vr
