#pragma once

namespace h47vr {

// Drives the game's console/config command dispatcher directly, instead of
// synthesising keystrokes for the in-game console.
//
// System.dll's dispatcher (RVA 0xE590) is a __thiscall that takes the command
// name and its argument as two stack parameters and cleans them itself, so the
// mod can call it in-process with the interface singleton from Globals.dll
// (RVA 0x3010). That makes "get into a level" a code path the mod owns, with a
// return value and a log line, rather than a race against the intro videos.
//
// The scene to load comes from <game>\h47vr-scene.txt (first non-empty line) or
// the H47VR_SCENE environment variable. With neither set the driver stays idle
// and the game behaves like an unmodified install.
bool ConsoleDriverInstall();

// Called from the presentation hook, on the engine's own thread, once per
// presented frame. Issues the pending scene load and reports what happened.
void ConsoleDriverPump(unsigned long frame);

}  // namespace h47vr
