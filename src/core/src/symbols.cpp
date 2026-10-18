#include "h47vr/symbols.hpp"

#include "h47vr/paths.hpp"

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

}  // namespace h47vr
