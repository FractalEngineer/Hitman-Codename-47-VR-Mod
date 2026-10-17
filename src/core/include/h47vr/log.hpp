#pragma once

#include <cstdarg>

namespace h47vr {

enum class LogLevel { Trace, Debug, Info, Warn, Error };

// Opens (creating if needed) a timestamped log file in logDirectory. The tag
// identifies the loading path, for example "opengl32-proxy".
bool LogStart(const wchar_t* logDirectory, const char* tag);
void LogStop();

void LogWrite(LogLevel level, const char* format, ...);
void LogWriteV(LogLevel level, const char* format, std::va_list args);

// Null until LogStart succeeds.
const wchar_t* LogFilePath();

}  // namespace h47vr

#define H47VR_LOG_TRACE(...) ::h47vr::LogWrite(::h47vr::LogLevel::Trace, __VA_ARGS__)
#define H47VR_LOG_DEBUG(...) ::h47vr::LogWrite(::h47vr::LogLevel::Debug, __VA_ARGS__)
#define H47VR_LOG_INFO(...)  ::h47vr::LogWrite(::h47vr::LogLevel::Info, __VA_ARGS__)
#define H47VR_LOG_WARN(...)  ::h47vr::LogWrite(::h47vr::LogLevel::Warn, __VA_ARGS__)
#define H47VR_LOG_ERROR(...) ::h47vr::LogWrite(::h47vr::LogLevel::Error, __VA_ARGS__)
