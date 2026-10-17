#pragma once

#include <string>

namespace h47vr {

// Full path of a loaded module, or of the main executable when module is null.
std::wstring ModulePath(void* module = nullptr);

// Directory containing that module, without a trailing separator.
std::wstring ModuleDirectory(void* module = nullptr);

// Directory of the process image. For the proxy this is the game directory.
std::wstring ExecutableDirectory();

std::wstring JoinPath(const std::wstring& base, const std::wstring& leaf);

bool FileExists(const std::wstring& path);

// Creates every missing component of a directory path.
bool EnsureDirectory(const std::wstring& path);

std::string Narrow(const std::wstring& text);
std::wstring Widen(const std::string& text);

}  // namespace h47vr
