// Rasterises the Blockbench SVG wordmark to a WebGPU texture for the title bar.
#pragma once

#include "imgui.h"

typedef struct WGPUDeviceImpl* WGPUDevice;
typedef struct WGPUQueueImpl* WGPUQueue;

namespace logo {

// Parse + rasterise <exe dir>/assets/blockbench-logo.svg at `target_h` px tall
// (rendered at 2x internally for crispness). Call once after WebGPU init.
bool load(WGPUDevice device, WGPUQueue queue, float target_h);

// ImTextureID for ImGui::Image(), or 0 if load() failed.
ImTextureID texture();

// Rasterised pixel size (already includes the 2x factor).
ImVec2 pixel_size();

} // namespace logo
