#pragma once

namespace h47vr {

// Captures RenderOpenGL's native window input object and exposes its mouse
// handlers. This avoids the window-procedure gate that drops our posted menu
// messages when MouseConCam owns the input mode.
bool RendererInputStart();
void RendererInputRearm();
bool RendererInputAvailable();
bool RendererInputMove(int x, int y);
bool RendererInputButton(bool down, int x, int y);
bool RendererInputKey(int vk, bool down);

}  // namespace h47vr
