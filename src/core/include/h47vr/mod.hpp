#pragma once

#include <string>

namespace h47vr::mod {

// Entry point shared by every loading path (opengl32 proxy, injected DLL,
// diagnostic launcher). Resolves the game directory, opens the log, verifies
// the build pins, and only then applies hooks. Returns false - without
// patching anything - when the install is not a supported build.
//
// Safe to call more than once; only the first call does work.
bool Initialize(const char* trigger);

void Shutdown();

bool IsRunning();

// True only when the build pins matched and hooks are allowed to be applied.
bool IsCompatible();

const std::wstring& GameDirectory();

}  // namespace h47vr::mod
