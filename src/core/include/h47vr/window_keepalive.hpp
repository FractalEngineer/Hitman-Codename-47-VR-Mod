#pragma once

namespace h47vr {

// Keeps the engine rendering while its window is not the active one.
//
// The engine stops presenting frames the moment it loses activation - audio
// keeps playing, the thread count stays put, and the CPU goes idle. That is the
// "stall" this project chased for a long time: every time the tooling shell took
// focus, the game quietly stopped drawing, and it came back the moment anything
// activated it again.
//
// A worker thread finds the game's own window and subclasses it, swallowing the
// deactivation messages so the engine never learns it lost focus. Everything
// else is passed straight through.
//
// This exists so the game can be left running while the desktop is used for
// something else - and so an automated run is not stopped dead by the shell
// taking focus.
bool WindowKeepaliveStart();

// Number of deactivation messages swallowed so far, for the log.
unsigned long WindowKeepaliveSwallowed();

}  // namespace h47vr
