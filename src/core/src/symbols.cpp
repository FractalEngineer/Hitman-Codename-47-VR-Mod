#include "h47vr/symbols.hpp"

#include "h47vr/paths.hpp"

#include <cstdint>

namespace h47vr {

HMODULE LoadedModule(const wchar_t* moduleName) {
    return ::GetModuleHandleW(moduleName);
}

void* ResolveSymbol(const wchar_t* moduleName, const char* symbol) {
    HMODULE module = ::GetModuleHandleW(moduleName);
    if (module == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<void*>(::GetProcAddress(module, symbol));
}

std::wstring ModuleFileName(HMODULE module) {
    return FileName(ModulePath(module));
}

std::wstring ModuleContaining(const void* address) {
    if (address == nullptr) {
        return {};
    }
    HMODULE module = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                  GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &module) ||
        module == nullptr) {
        return {};
    }
    return ModuleFileName(module);
}

bool ModuleOffset(const void* address, const wchar_t* moduleName, unsigned long long& offset) {
    HMODULE module = ::GetModuleHandleW(moduleName);
    if (module == nullptr || address == nullptr) {
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    if (target < base) {
        return false;
    }
    offset = static_cast<unsigned long long>(target - base);
    return true;
}

}  // namespace h47vr
