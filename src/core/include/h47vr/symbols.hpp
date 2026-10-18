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

}  // namespace h47vr
