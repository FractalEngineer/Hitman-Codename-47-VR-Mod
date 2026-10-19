#pragma once

namespace h47vr {

enum class StereoMode { Off, Theater, Stereo };

// RenderOpenGL's draw-list consumer is __thiscall(void* self, int* list).
// The trampoline is invoked through a matching fastcall signature: ECX carries
// self, EDX is ignored, and the list pointer is the only stack argument.
using StereoDrawFn = void(__fastcall*)(void* self, void* ignored, int* list);

bool StereoRendererStart();
void StereoRendererFrame(unsigned long frame);
StereoMode StereoRendererMode();

// Called by the renderer hook. In stereo mode a perspective draw is replayed
// once per eye while MouseConCam is actively updating. Menus, cutscenes and
// other non-gameplay cameras pass through once as the theater-mode image.
void StereoRendererDrawWorld(void* self, int* list, StereoDrawFn draw);

}  // namespace h47vr
