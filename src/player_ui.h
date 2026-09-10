// The MCAP-player UI. An Ohwow-style fixed layout: a left icon rail (back
// button at top), a centre video stage, a bottom transport bar, and a
// right properties panel with collapsible sections. Wires mp::Playback
// (the ported EgoViewer engine) to ImGui. All rendering is immediate —
// call player_ui::layout() once per frame from the shell's Minimal chrome.
#pragma once

#include "imgui.h"

#include <webgpu/webgpu.h>

namespace mp {
class Playback;
}

namespace player_ui {

// Create the Playback engine + video-texture pool. Call once after wgpu init.
void init(WGPUDevice device, WGPUQueue queue);
void shutdown();

// Windows "open file" dialog (*.mcap) -> Playback::open(). No-op elsewhere.
void open_dialog();
// Windows "pick folder" dialog (LeRobot dataset dir) -> Playback::open().
void open_folder_dialog();
// Open a file by path directly (CLI arg / drag-drop / tests).
void open_path(const char* utf8_path);

// Draw the whole player into the screen-space rect (origin, size) — the
// area shell::content_rect() hands back under the title bar.
void layout(ImVec2 origin, ImVec2 size);

bool has_file();
mp::Playback& playback();

} // namespace player_ui
