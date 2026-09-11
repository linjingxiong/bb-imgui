// A reusable magnifier / colour-picker overlay for an image drawn into the
// current ImGui window. Call it immediately after submitting the image.
//
// Hold-to-inspect: press and hold the left button over the image to hide the
// OS cursor, show a loupe, mark the pixel under the pointer, and open a card
// (anchored at the press) with a zoomed pixel grid, the source coordinate and
// the RGB / hex value. Drag to move the sample point; release to dismiss. One
// instance at a time (ImGui allows a single active item).
//
// The widget is decoupled from any image type: it asks for pixels through a
// `sample` callback and takes its colours from the current ImGui style.
#pragma once

#include "imgui.h"

#include <functional>

namespace px {

// str_id     unique id for this inspector instance
// img_min    image rect top-left, in screen space
// img_max    image rect bottom-right, in screen space
// src_w/h    source image pixel dimensions
// rot        degrees clockwise the image was drawn at (0 / 90 / 180 / 270)
// label      shown in the card header
// sample     fills rgb[3] for source pixel (x, y); returns false if out of range
void PixelInspector(const char* str_id, ImVec2 img_min, ImVec2 img_max, int src_w, int src_h,
                    int rot, const char* label,
                    const std::function<bool(int, int, unsigned char*)>& sample);

} // namespace px
