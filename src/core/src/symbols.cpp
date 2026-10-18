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
    // The base alone is not enough: an address above the base can still belong
    // to a module mapped later. Check the image size too.
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
        reinterpret_cast<const char*>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    const auto size = static_cast<std::uintptr_t>(nt->OptionalHeader.SizeOfImage);
    const auto target = reinterpret_cast<std::uintptr_t>(address);
    if (target < base || target >= base + size) {
        return false;
    }
    offset = static_cast<unsigned long long>(target - base);
    return true;
}

}  // namespace h47vr
