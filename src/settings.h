// Blockbench-style Settings dialog + the persisted settings behind it.
// Values live in <exe>/settings.json; the dialog is a centred modal with a
// left category rail and label/description/control rows (css/dialogs.css
// .settings_list). Call settings::load() once at startup and settings::draw()
// once per frame inside the ImGui frame.
#pragma once

#include <string>

namespace settings {

struct Settings {
    std::string theme;             // theme name; empty = leave theme.txt in charge
    bool        autoplay_on_open = false;
    bool        loop_at_end = false;
    float       default_speed = 1.0f;   // 0.5 / 1 / 2 / 4
    int         default_rotation = 0;   // 0 / 90 / 180 / 270 degrees CW
    int         default_fit = 0;        // 0 = contain (letterbox), 1 = cover (crop)
    int         layout = 0;             // 0 = grid, 1 = spotlight
};

Settings& get();

// Read <exe>/settings.json (missing / bad file -> defaults). Call at startup.
void load();
// Write the current settings back to <exe>/settings.json.
void save();

// Request the dialog to open on the next draw().
void open();
bool is_open();

// Render the modal if open. Call once per frame, inside the ImGui frame,
// after the rest of the UI so it stacks on top.
void draw();

} // namespace settings
