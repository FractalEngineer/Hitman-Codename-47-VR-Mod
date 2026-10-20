#pragma once

namespace h47vr {

// Turns the engine's own camera from inside the process: no focus, no synthetic
// input, no cursor stealing.
//
// The engine reads the mouse only through window messages. There is no
// `GetCursorPos` and no DirectInput import anywhere in the game; RenderOpenGL's
// window procedure sends `WM_MOUSEMOVE` (and `WM_NCMOUSEMOVE`) to its own
// handler at RVA `0x29AC0`, which subtracts the client-area centre from the
// cursor position, warps the cursor back to that centre with `SetCursorPos`,
// accumulates the delta into the input interface (`g_pSysInterface` `+0xa95`
// and `+0xa99`), and publishes a normalised crosshair position in the renderer
// object at `+0x3e`/`+0x42`, clamped to +/-0.5.
//
// So posting `WM_MOUSEMOVE` to the game's own window exercises exactly the path
// a real mouse move takes - and unlike `SendInput` it does not need the window
// to be in the foreground, because the message goes straight into the engine's
// own queue.
//
// A request is a single file, `<game>\h47vr-look.txt`, consumed on sight:
//
//     dx=<pixels> dy=<pixels> [steps=<n>] [rate=<frames>]
//
// The turn is spread over `steps` frames (default 15) because the engine
// discards a single large jump.
bool LookDriverStart();

// Called once per presented frame, on the engine's render thread.
void LookDriverFrame(unsigned long frame);

// Supplies the latest headset orientation in OpenXR's convention: yaw positive
// to the right and pitch positive up. The first valid sample after tracking
// starts becomes the recenter reference. The driver converts the target into
// the engine's own mouse deltas, so this remains subject to the same gameplay
// readback gate as an absolute look request.
void LookDriverSetHeadPose(bool valid, double yawDegrees, double pitchDegrees);
double LookDriverScenePitchDegrees();

// Called immediately before the engine's MouseConCam update. It adds OpenXR
// head yaw and controller turn to the native look accumulator consumed by
// MouseConCam, avoiding WM_MOUSEMOVE and cursor-space assumptions.
void LookDriverApplyNativeYaw();

// Marks the synchronous, mod-generated mouse dispatch whose engine cursor warp
// must be suppressed. Real hardware mouse messages are left untouched.
void LookDriverBeginSyntheticMouse();
void LookDriverEndSyntheticMouse();

// Installs the cursor guard if the renderer is loaded. Safe to call repeatedly,
// and needs calling again after a module reload drops the patch.
void LookDriverRearm();

// Periodic report for the log heartbeat.
void LookDriverReport(unsigned long frame);

}  // namespace h47vr
