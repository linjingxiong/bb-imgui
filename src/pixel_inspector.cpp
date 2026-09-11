#include "pixel_inspector.h"

#include <algorithm>
#include <cstdio>

namespace px {
namespace {

// Card anchor, remembered from the press so it doesn't jitter while dragging.
// Only one InvisibleButton can be active at a time, so a single static is fine.
ImVec2 g_anchor;

int rot_norm(int rot) { return ((rot % 360) + 360) % 360; }

ImU32 style_col(ImGuiCol c, float a = 1.0f) {
    ImVec4 v = ImGui::GetStyleColorVec4(c);
    v.w *= a;
    return ImGui::ColorConvertFloat4ToU32(v);
}

void draw_loupe(ImDrawList* dl, ImVec2 hot) {
    const ImVec2 c(hot.x + 7.0f, hot.y + 7.0f); // lens sits down-right of the hotspot
    const float r = 6.5f;
    dl->AddCircleFilled(c, r, IM_COL32(255, 255, 255, 26), 24);
    dl->AddCircle(c, r + 0.5f, IM_COL32(0, 0, 0, 170), 24, 3.0f);
    dl->AddCircle(c, r, IM_COL32(255, 255, 255, 235), 24, 1.6f);
    const ImVec2 h0(c.x + r * 0.72f, c.y + r * 0.72f), h1(h0.x + 5.0f, h0.y + 5.0f);
    dl->AddLine(h0, h1, IM_COL32(0, 0, 0, 170), 3.4f);
    dl->AddLine(h0, h1, IM_COL32(255, 255, 255, 235), 1.7f);
}

} // namespace

void PixelInspector(const char* str_id, ImVec2 img_min, ImVec2 img_max, int src_w, int src_h,
                    int rot, const char* label,
                    const std::function<bool(int, int, unsigned char*)>& sample) {
    if (src_w <= 0 || src_h <= 0 || !sample) return;
    const ImVec2 sz(img_max.x - img_min.x, img_max.y - img_min.y);
    if (sz.x < 8.0f || sz.y < 8.0f) return;
    const int rn = rot_norm(rot);

    const ImVec2 save_cursor = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(img_min);
    ImGui::InvisibleButton(str_id, sz, ImGuiButtonFlags_MouseButtonLeft);

    // Hold-to-inspect: active from press to release, and only one at a time
    // (ImGui allows a single active item).
    const bool active = ImGui::IsItemActive();
    if (ImGui::IsItemActivated()) g_anchor = ImGui::GetIO().MousePos;
    if (!active) {
        ImGui::SetCursorScreenPos(save_cursor);
        return;
    }

    const ImVec2 mp = ImGui::GetIO().MousePos;

    // pointer -> source pixel, clamped to the image and inverting the CW
    // rotation (so a drag past the edge still tracks the border pixels).
    int px = 0, py = 0;
    {
        float u = std::clamp((mp.x - img_min.x) / sz.x, 0.0f, 0.999999f);
        float v = std::clamp((mp.y - img_min.y) / sz.y, 0.0f, 0.999999f);
        float su, sv;
        switch (rn) {
            case 90:  su = v;         sv = 1.0f - u;  break;
            case 180: su = 1.0f - u;  sv = 1.0f - v;  break;
            case 270: su = 1.0f - v;  sv = u;         break;
            default:  su = u;         sv = v;         break;
        }
        px = std::clamp((int)(su * src_w), 0, src_w - 1);
        py = std::clamp((int)(sv * src_h), 0, src_h - 1);
    }
    // source pixel -> its screen rect (forward rotation; pixels stay axis-aligned).
    auto src_rect = [&](int sx, int sy, ImVec2& lo, ImVec2& hi) {
        auto fwd = [&](float su, float sv) -> ImVec2 {
            float u, v;
            switch (rn) {
                case 90:  u = 1.0f - sv; v = su;        break;
                case 180: u = 1.0f - su; v = 1.0f - sv; break;
                case 270: u = sv;        v = 1.0f - su; break;
                default:  u = su;        v = sv;        break;
            }
            return ImVec2(img_min.x + u * sz.x, img_min.y + v * sz.y);
        };
        ImVec2 a = fwd((float)sx / src_w, (float)sy / src_h);
        ImVec2 b = fwd((float)(sx + 1) / src_w, (float)(sy + 1) / src_h);
        lo = ImVec2(std::min(a.x, b.x), std::min(a.y, b.y));
        hi = ImVec2(std::max(a.x, b.x), std::max(a.y, b.y));
    };

    ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    ImDrawList* fg = ImGui::GetForegroundDrawList();

    // Marker over the pixel under the pointer + the loupe glyph.
    ImVec2 lo, hi;
    src_rect(px, py, lo, hi);
    fg->AddRect(ImVec2(lo.x - 1, lo.y - 1), ImVec2(hi.x + 1, hi.y + 1), IM_COL32(0, 0, 0, 150), 0.0f,
                0, 1.0f);
    fg->AddRect(lo, hi, IM_COL32(255, 255, 255, 235), 0.0f, 0, 1.0f);
    draw_loupe(fg, mp);

    // ── The card ─────────────────────────────────────────────────────────
    const int N = 7;                      // grid radius -> 15x15
    const float CELL = 9.0f;
    const float GRID = (2 * N + 1) * CELL;
    const float PAD = 12.0f, GAP = 14.0f, HEAD = 26.0f, TXT_W = 168.0f;
    const ImVec2 csz(PAD + GRID + GAP + TXT_W + PAD, HEAD + PAD + GRID + PAD);

    ImVec2 cpos(g_anchor.x + 18.0f, g_anchor.y + 14.0f);
    const ImVec2 vp_min = ImGui::GetMainViewport()->WorkPos;
    const ImVec2 vp_sz = ImGui::GetMainViewport()->WorkSize;
    cpos.x = std::clamp(cpos.x, vp_min.x + 4.0f, vp_min.x + vp_sz.x - csz.x - 4.0f);
    cpos.y = std::clamp(cpos.y, vp_min.y + 4.0f, vp_min.y + vp_sz.y - csz.y - 4.0f);

    const ImU32 bg = style_col(ImGuiCol_PopupBg);
    const ImU32 bord = style_col(ImGuiCol_Border, 0.9f);
    const ImU32 txt = style_col(ImGuiCol_Text);
    const ImU32 dim = style_col(ImGuiCol_TextDisabled);

    fg->AddRectFilled(cpos, ImVec2(cpos.x + csz.x, cpos.y + csz.y), bg, 6.0f);
    fg->AddRect(cpos, ImVec2(cpos.x + csz.x, cpos.y + csz.y), bord, 6.0f, 0, 1.0f);

    if (label && label[0])
        fg->AddText(ImVec2(cpos.x + PAD, cpos.y + (HEAD - ImGui::GetFontSize()) * 0.5f), txt, label);
    fg->AddLine(ImVec2(cpos.x, cpos.y + HEAD), ImVec2(cpos.x + csz.x, cpos.y + HEAD), bord, 1.0f);

    // Zoom grid
    const ImVec2 g0(cpos.x + PAD, cpos.y + HEAD + PAD);
    unsigned char centre[3] = {0, 0, 0};
    const bool centre_ok = sample(px, py, centre);
    for (int dy = -N; dy <= N; ++dy)
        for (int dx = -N; dx <= N; ++dx) {
            unsigned char c[3];
            ImVec2 a(g0.x + (dx + N) * CELL, g0.y + (dy + N) * CELL);
            ImVec2 b(a.x + CELL, a.y + CELL);
            if (sample(px + dx, py + dy, c))
                fg->AddRectFilled(a, b, IM_COL32(c[0], c[1], c[2], 255));
            else
                fg->AddRectFilled(a, b, IM_COL32(40, 40, 40, 255));
        }
    fg->AddRect(g0, ImVec2(g0.x + GRID, g0.y + GRID), bord, 0.0f, 0, 1.0f);
    ImVec2 ca(g0.x + N * CELL, g0.y + N * CELL);
    fg->AddRect(ImVec2(ca.x - 1, ca.y - 1), ImVec2(ca.x + CELL + 1, ca.y + CELL + 1),
                IM_COL32(0, 0, 0, 180), 0.0f, 0, 1.0f);
    fg->AddRect(ca, ImVec2(ca.x + CELL, ca.y + CELL), IM_COL32(255, 255, 255, 240), 0.0f, 0, 1.0f);

    // Text column
    const float tx = g0.x + GRID + GAP;
    float ty = g0.y + 2.0f;
    const float lh = ImGui::GetTextLineHeightWithSpacing();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Position:  %d, %d", px, py);
    fg->AddText(ImVec2(tx, ty), txt, buf);
    ty += lh;
    if (centre_ok) {
        std::snprintf(buf, sizeof(buf), "RGB:  %d, %d, %d", centre[0], centre[1], centre[2]);
        fg->AddText(ImVec2(tx, ty), txt, buf);
        ty += lh;
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", centre[0], centre[1], centre[2]);
        fg->AddText(ImVec2(tx, ty), dim, buf);
        ty += lh + 6.0f;
        ImVec2 sw0(tx, ty), sw1(tx + 56.0f, ty + 56.0f);
        fg->AddRectFilled(sw0, sw1, IM_COL32(centre[0], centre[1], centre[2], 255), 3.0f);
        fg->AddRect(sw0, sw1, bord, 3.0f, 0, 1.0f);
    } else {
        fg->AddText(ImVec2(tx, ty), dim, "RGB:  --");
    }

    ImGui::SetCursorScreenPos(save_cursor);
}

} // namespace px
