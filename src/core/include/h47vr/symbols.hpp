#pragma once

#include <windows.h>

#include <string>

namespace h47vr {

// Handle for a module that is already loaded into this process, or null.
HMODULE LoadedModule(const wchar_t* moduleName);

// Address of an exported symbol in an already-loaded module, or null when the
// module is not loaded or the symbol does not exist.
//
// The gameplay module exports its entry points by name, so most hooks can be
// resolved this way instead of hard-coding an address. The build fingerprint
// still guarantees the export table has not changed.
void* ResolveSymbol(const wchar_t* moduleName, const char* symbol);

// Base file name of a loaded module, e.g. "HitmanDlc.dlc".
std::wstring ModuleFileName(HMODULE module);

// Base file name of the loaded module that contains address, or an empty
// string when the address is not inside any loaded module. Useful for logging
// a runtime pointer (a vtable slot, a callback) in terms a Ghidra session can
// look up: module plus RVA.
std::wstring ModuleContaining(const void* address);

// Offset of address from the base of the module that contains it. Returns
// false when the address is not inside that module.
bool ModuleOffset(const void* address, const wchar_t* moduleName, unsigned long long& offset);

}  // namespace h47vr
