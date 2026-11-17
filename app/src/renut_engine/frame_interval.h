// fnxf_xiii (2026-10-25): the frame interval (boot menu / fnxf_xiii_frame_interval): the game's own 30 fps lock removed and
// the frame rate set by the display interval instead, except while a prerendered movie plays (then the game's lock at
// 30 fps on a display interval of 1). renut_engine/frame_interval.cpp.
#pragma once

#include <cstdint>

namespace fnxf_xiii::frame_interval {

// The presentation interval the game's frame limiter works with, in D3DRS_PRESENTINTERVAL's form (device +0x35EC:
// 1, 2, ... vblanks, 0x80000000 immediate), given the game's own 'raw' value. fnxf_xiii_frame_interval 0: 'raw'.
uint32_t GameInterval(uint32_t raw);

// fnxf_xiii (2026-10-25): the game's own lock is off now (fnxf_xiii_frame_interval 1-4 and no movie open).
bool GameLockOff();

// fnxf_xiii (2026-10-27): the vblanks (of the game's 60 Hz pump) a frame the game's limiter waits with the lock off:
// fnxf_xiii_frame_interval (1 = 60 fps, 2 = 30, 3 = 20, 4 = 15).
uint32_t GameVblanks();

// The swapchain's interval (refreshes an image is shown for; 0 = no wait), given renut_vsync.
uint32_t SwapInterval(bool vsync);

// A prerendered movie (.wmp) is open (the SDK counts its file objects).
bool MoviePlaying();

// fnxf_xiii (2026-11-05, seen17): late frames start at once (device.cpp StartLateFrameNow).
// The vblank pump: the game's vblank handler is about to run (steady_clock, now).
void NoteGameVblank();
// StartLateFrameNow: the next frame starts now instead of at the next vblank. Until that frame's step is taken
// (TakeLateStart), the game's resource manager may go on loading until the vblank the frame would have waited for
// (LateLoadDeadlineUs), as it did inside that wait.
void NoteLateStart();
// The resource manager's limit for the current late-started frame (steady_clock us), 0 = none.
uint64_t LateLoadDeadlineUs();
// The frame step hook (after the game's limiter): whether this frame was started early (cleared; the deadline too).
bool TakeLateStart();

}  // namespace fnxf_xiii::frame_interval
