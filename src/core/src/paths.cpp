#include "h47vr/paths.hpp"

#include <windows.h>

#include <vector>

namespace h47vr {
namespace {

std::wstring ModuleFileName(HMODULE module) {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written = ::GetModuleFileNameW(module, buffer.data(),
                                                   static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size() - 1) {
            return std::wstring(buffer.data(), written);
        }
        // Buffer was too small; the API truncates and returns the full size.
        buffer.resize(buffer.size() * 2);
    }
}

}  // namespace

std::wstring ModulePath(void* module) {
    return ModuleFileName(static_cast<HMODULE>(module));
}

std::wstring ModuleDirectory(void* module) {
    std::wstring path = ModuleFileName(static_cast<HMODULE>(module));
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return path.substr(0, slash);
}

std::wstring ExecutableDirectory() {
    return ModuleDirectory(nullptr);
}

std::wstring JoinPath(const std::wstring& base, const std::wstring& leaf) {
    if (base.empty()) {
        return leaf;
    }
    if (base.back() == L'\\' || base.back() == L'/') {
        return base + leaf;
    }
    return base + L"\\" + leaf;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    std::wstring current;
    current.reserve(path.size());
    for (std::size_t i = 0; i < path.size(); ++i) {
        current.push_back(path[i]);
        const bool last = (i + 1 == path.size());
        const bool separator = (path[i] == L'\\' || path[i] == L'/');
        if (!separator && !last) {
            continue;
        }

        // Strip trailing separators: CreateDirectoryW rejects "C:\" and
        // "dir\" even when the directory already exists.
        std::wstring component = current;
        while (!component.empty() &&
               (component.back() == L'\\' || component.back() == L'/')) {
            component.pop_back();
        }
        if (component.empty()) {
            continue;
        }

        if (!::CreateDirectoryW(component.c_str(), nullptr)) {
            // An existing path reports ALREADY_EXISTS, but a drive root such as
            // "C:" or "C:\" reports ACCESS_DENIED instead. Treat any failure as
            // acceptable as long as the path really is a directory.
            const DWORD attributes = ::GetFileAttributesW(component.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                return false;
            }
        }
    }
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                          out.data(), size);
    return out;
}

}  // namespace h47vr
