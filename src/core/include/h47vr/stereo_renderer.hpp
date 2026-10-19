#pragma once

namespace h47vr {

// RenderOpenGL's draw-list consumer is __thiscall(void* self, int* list).
// The trampoline is invoked through a matching fastcall signature: ECX carries
// self, EDX is ignored, and the list pointer is the only stack argument.
using StereoDrawFn = void(__fastcall*)(void* self, void* ignored, int* list);

bool StereoRendererStart();
void StereoRendererFrame(unsigned long frame);

// Called by the renderer hook. In stereo mode a perspective draw is replayed
// once per eye; orthographic UI and menu draws pass through unchanged.
void StereoRendererDrawWorld(void* self, int* list, StereoDrawFn draw);

}  // namespace h47vr
