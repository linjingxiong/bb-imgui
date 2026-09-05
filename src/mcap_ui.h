// The MCAP-player UI: a Foxglove-style topic tree + video grid + timeline
// scrubber, drawn in Blockbench's visual style. Wires mp::Playback (the
// ported EgoViewer engine) to ImGui panels. All rendering is immediate —
// call these from the frame loop.
#pragma once

#include <webgpu/webgpu.h>

namespace mp {
class Playback;
}

namespace mcap_ui {

// Create the Playback engine + video-texture pool. Call once after wgpu init.
void init(WGPUDevice device, WGPUQueue queue);
void shutdown();

// Windows "open file" dialog -> Playback::open(). No-op elsewhere for now.
void open_dialog();
// Open a file by path directly (CLI arg / drag-drop / tests).
void open_path(const char* utf8_path);

// Panel bodies — call inside the corresponding bb::begin_panel/end_panel.
void topic_tree();   // Left panel
void inspector();    // Right panel — selected topic's latest message
void video_grid();   // Workspace panel
void imu_plots();    // Workspace panel — x/y/z history for each /imu/* topic
void timeline();     // a slim bar (call above the dockspace or in Workspace)

bool has_file();
mp::Playback& playback();

} // namespace mcap_ui
