# bb-imgui

A desktop UI shell built with **Dear ImGui**, styled after
[Blockbench](https://www.blockbench.net/)'s "Default (Dark)" theme.

Not affiliated with Blockbench. The theme colours, font (Assistant), Material
Symbols icons and the SVG wordmark are taken from Blockbench (GPL-3.0) purely to
reproduce its look.

## What's here

- Borderless window with a hand-drawn title bar (drag to move, double-click to
  maximise, min/max/close), edge resize handles
- Blockbench menu bar — `File / Edit / …` with styled dropdowns (icon column,
  shortcuts, separators, checkmarks, submenu chevrons)
- 48px left icon navigation rail + status bar
- Docking layout (ImGui docking branch) with Blockbench-style panel headers
- `bb::` widget library on top of ImGui: buttons, `num_slider` (Blockbench's
  drag-to-scrub number field), `vec3`, inputs, combo, segmented, checkbox,
  radio, colour, toggle, feedback widgets, collapsing sections
- A component browser (left list → live example + usage in the workspace)
- `.bbtheme` (JSON) theme loading; runtime SVG rasterisation of the wordmark

## Build

Everything except a C++17 compiler is fetched by CMake (Dear ImGui *docking*,
GLFW, nlohmann/json, and a prebuilt **wgpu-native** — the renderer is WebGPU).

```sh
cmake -S . -B build            # Windows: -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
./build/bin/Release/bb_imgui   # or build/bin/bb_imgui on single-config generators
```

`assets/` (fonts, `blockbench-dark.bbtheme`, `blockbench-logo.svg`) is copied
next to the binary at build time.

## Layout

| file | |
|---|---|
| `src/theme.*` | `.bbtheme` → `ImGuiStyle` + a `Palette`; font-size constants |
| `src/fonts.*` / `src/icons.h` | Assistant + merged Material Symbols; icon codepoints |
| `src/shell.*` | title bar, icon rail, status bar, docking host |
| `src/menu.*` | menu-bar dropdowns |
| `src/bb.*` | the Blockbench-styled widget library |
| `src/gallery.*` | the component browser |
| `src/logo.*` | SVG wordmark → WebGPU texture (nanosvg) |

## Notes

- ImGui `size_pixels` maps the font's ascent-to-descent height to N px, while CSS
  `font-size` maps the em square. For Assistant the factor is ~1.308, so every
  Blockbench CSS px is multiplied by that in `theme::size`.
- A borderless window that is fully occluded is throttled to ~10 fps by Windows
  DWM; that is not the app being slow (per-frame CPU cost is ~1 ms).
