#include "el.h"

#include "fonts.h"
#include "icons.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace el {
namespace {

ImU32 u32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
ImVec4 rgb(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}
ImVec4 mix(ImVec4 a, ImVec4 b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

struct TypeColors {
    ImVec4 base, hover, active, plain_bg, plain_text;
};

const TypeColors& colors_for(ButtonType t) {
    static const TypeColors table[] = {
        /*Default*/ {rgb(0xff, 0xff, 0xff), rgb(0xec, 0xf5, 0xff), rgb(0xd9, 0xec, 0xff),
                    rgb(0xff, 0xff, 0xff), rgb(0x40, 0x9e, 0xff)},
        /*Primary*/ {rgb(0x40, 0x9e, 0xff), rgb(0x66, 0xb1, 0xff), rgb(0x3a, 0x8e, 0xe6),
                    rgb(0xec, 0xf5, 0xff), rgb(0x40, 0x9e, 0xff)},
        /*Success*/ {rgb(0x67, 0xc2, 0x3a), rgb(0x85, 0xce, 0x61), rgb(0x5d, 0xaf, 0x34),
                    rgb(0xf0, 0xf9, 0xeb), rgb(0x67, 0xc2, 0x3a)},
        /*Warning*/ {rgb(0xe6, 0xa2, 0x3c), rgb(0xeb, 0xb5, 0x63), rgb(0xcf, 0x92, 0x36),
                    rgb(0xfd, 0xf6, 0xec), rgb(0xe6, 0xa2, 0x3c)},
        /*Danger */ {rgb(0xf5, 0x6c, 0x6c), rgb(0xf7, 0x89, 0x89), rgb(0xdd, 0x61, 0x61),
                    rgb(0xfe, 0xf0, 0xf0), rgb(0xf5, 0x6c, 0x6c)},
        /*Info   */ {rgb(0x90, 0x93, 0x99), rgb(0xa6, 0xa9, 0xad), rgb(0x82, 0x84, 0x8a),
                    rgb(0xf4, 0xf4, 0xf5), rgb(0x90, 0x93, 0x99)},
        /*Text   */ {ImVec4(0, 0, 0, 0), rgb(0xec, 0xf5, 0xff), rgb(0xd9, 0xec, 0xff),
                    ImVec4(0, 0, 0, 0), rgb(0x40, 0x9e, 0xff)},
    };
    return table[(int)t];
}

} // namespace

bool button(const char* label, ButtonType type, const ButtonOpts& o) {
    const TypeColors& c = colors_for(type);
    ImGui::PushID(label);

    ImGui::PushFont(nullptr, theme::size::SMALL);
    ImVec2 ts = ImGui::CalcTextSize(o.icon ? "" : label);
    if (o.icon) {
        ImGui::PushFont(fonts::body(), 15.0f);
        ImVec2 its = ImGui::CalcTextSize(o.icon);
        ImGui::PopFont();
        ts.x += its.x + (label[0] ? 6.0f : 0.0f);
        ts.y = std::max(ts.y, its.y);
    }
    ImVec2 lts = ImGui::CalcTextSize(label);
    float text_w = o.icon ? ts.x : lts.x;
    ImGui::PopFont();

    float h = o.height;
    float w = o.circle ? h : text_w + 24.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 size(w, h);
    ImGui::InvisibleButton("##btn", size);
    bool hovered = !o.disabled && !o.loading && ImGui::IsItemHovered();
    bool active = !o.disabled && !o.loading && ImGui::IsItemActive();
    bool clicked = !o.disabled && !o.loading && ImGui::IsItemClicked();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(pos.x + w, pos.y + h);
    float rounding = o.circle ? h * 0.5f : (o.round ? h * 0.5f : theme::RADIUS);

    ImVec4 fill = o.plain ? c.plain_bg : c.base;
    ImVec4 border = o.plain ? c.base : c.base;
    ImVec4 text_col = o.plain ? c.plain_text
                     : (type == ButtonType::Default ? rgb(0x60, 0x62, 0x66) : rgb(0xff, 0xff, 0xff));
    if (type == ButtonType::Text) {
        text_col = rgb(0x60, 0x62, 0x66);
        border = ImVec4(0, 0, 0, 0);
    }
    if (active) {
        fill = o.plain ? mix(c.plain_bg, c.base, 0.3f) : c.active;
        border = c.active;
        if (type == ButtonType::Default) text_col = c.active;
        if (type == ButtonType::Text) text_col = c.active;
    } else if (hovered) {
        fill = o.plain ? c.plain_bg : c.hover;
        border = c.hover;
        if (type == ButtonType::Default) text_col = c.hover;
        if (type == ButtonType::Text) text_col = c.hover;
    }
    if (o.disabled) {
        fill = mix(fill, rgb(255, 255, 255), 0.5f);
        text_col = ImVec4(text_col.x, text_col.y, text_col.z, 0.5f);
        border = ImVec4(border.x, border.y, border.z, 0.5f);
    }

    if (fill.w > 0.001f) dl->AddRectFilled(pos, br, u32(fill), rounding);
    if (border.w > 0.001f && type != ButtonType::Text)
        dl->AddRect(pos, br, u32(border), rounding, 0, 1.0f);

    float cx = pos.x + w * 0.5f, cy = pos.y + h * 0.5f;
    if (o.loading) {
        ImDrawList* fdl = ImGui::GetWindowDrawList();
        float t = (float)ImGui::GetTime() * 8.0f;
        for (int i = 0; i < 8; i++) {
            float a0 = t + (float)i * (6.2831853f / 8.0f);
            ImVec2 p1(cx + std::cos(a0) * 6.0f, cy + std::sin(a0) * 6.0f);
            fdl->AddCircleFilled(p1, 1.4f, u32(ImVec4(text_col.x, text_col.y, text_col.z,
                                                       0.2f + 0.8f * (float)i / 8.0f)));
        }
    } else {
        ImGui::PushFont(nullptr, theme::size::SMALL);
        float x = cx - text_w * 0.5f;
        if (o.icon) {
            ImGui::PushFont(fonts::body(), 15.0f);
            ImVec2 its = ImGui::CalcTextSize(o.icon);
            dl->AddText(ImVec2(x, cy - its.y * 0.5f), u32(text_col), o.icon);
            ImGui::PopFont();
            x += its.x + (label[0] ? 6.0f : 0.0f);
        }
        if (label[0])
            dl->AddText(ImVec2(x, cy - lts.y * 0.5f), u32(text_col), label);
        ImGui::PopFont();
    }

    ImGui::PopID();
    return clicked;
}

bool input_number(const char* id, double* v, double step, double min, double max,
                  int decimals) {
    const theme::Palette& p = theme::palette();
    ImGui::PushID(id);
    const float h = 32.0f, bw = 32.0f;
    bool changed = false;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float total_w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    auto step_btn = [&](ImVec2 bpos, const char* glyph, double delta) {
        ImGui::SetCursorScreenPos(bpos);
        ImGui::PushID(glyph);
        ImGui::InvisibleButton("b", ImVec2(bw, h));
        bool hov = ImGui::IsItemHovered();
        if (hov) dl->AddRectFilled(bpos, ImVec2(bpos.x + bw, bpos.y + h), u32(p.selected));
        if (ImGui::IsItemClicked()) {
            *v += delta;
            if (min != max) *v = std::clamp(*v, min, max);
            changed = true;
        }
        ImGui::PushFont(fonts::body(), 15.0f);
        ImVec2 ts = ImGui::CalcTextSize(glyph);
        dl->AddText(ImVec2(bpos.x + (bw - ts.x) * 0.5f, bpos.y + (h - ts.y) * 0.5f),
                    u32(rgb(0x60, 0x62, 0x66)), glyph);
        ImGui::PopFont();
        ImGui::PopID();
    };

    dl->AddRectFilled(pos, ImVec2(pos.x + total_w, pos.y + h), u32(p.deep), theme::RADIUS);
    dl->AddRect(pos, ImVec2(pos.x + total_w, pos.y + h), u32(p.border), theme::RADIUS);
    dl->AddLine(ImVec2(pos.x + bw, pos.y), ImVec2(pos.x + bw, pos.y + h), u32(p.border));
    dl->AddLine(ImVec2(pos.x + total_w - bw, pos.y), ImVec2(pos.x + total_w - bw, pos.y + h),
                u32(p.border));

    step_btn(pos, ICON_REMOVE, -step);
    step_btn(ImVec2(pos.x + total_w - bw, pos.y), ICON_ADD, step);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, *v);
    ImGui::PushFont(nullptr, theme::size::SMALL);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(pos.x + (total_w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f),
                u32(p.text), buf);
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(pos);
    ImGui::InvisibleButton("##mid", ImVec2(total_w - 2 * bw < 0 ? 0 : total_w - 2 * bw, h));
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && ImGui::IsItemHovered()) {
        // (No inline text edit in this pass — steppers cover the common case.)
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    ImGui::Dummy(ImVec2(total_w, 0));
    ImGui::PopID();
    return changed;
}

bool rate(const char* id, float* value, int max_stars, bool allow_half) {
    (void)allow_half;
    const ImVec4 filled = rgb(0xf7, 0xba, 0x2a);
    const ImVec4 empty = rgb(0xc0, 0xc4, 0xcc);
    ImGui::PushID(id);
    bool changed = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(fonts::body(), 20.0f);
    float star_w = ImGui::CalcTextSize(ICON_STAR).x + 4.0f;
    for (int i = 0; i < max_stars; i++) {
        ImGui::PushID(i);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("s", ImVec2(star_w, 24.0f));
        bool hovered = ImGui::IsItemHovered();
        if (hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            *value = (float)(i + 1);
            changed = true;
        } else if (ImGui::IsItemClicked()) {
            *value = (float)(i + 1);
            changed = true;
        }
        bool on = (i + 1) <= (int)std::lround(*value);
        dl->AddText(pos, u32(on ? filled : empty), on ? ICON_STAR : ICON_STAR_OUTLINE);
        ImGui::PopID();
        if (i + 1 < max_stars) ImGui::SameLine(0, 2);
    }
    ImGui::PopFont();
    ImGui::PopID();
    return changed;
}

bool tag(const char* text, TagType type, bool plain, bool closable) {
    struct C { ImVec4 bg, border, fg; };
    static const C table[] = {
        {rgb(0xf4, 0xf4, 0xf5), rgb(0xe9, 0xe9, 0xeb), rgb(0x90, 0x93, 0x99)},
        {rgb(0xf0, 0xf9, 0xeb), rgb(0xe1, 0xf3, 0xd8), rgb(0x67, 0xc2, 0x3a)},
        {rgb(0xfd, 0xf6, 0xec), rgb(0xfa, 0xec, 0xd8), rgb(0xe6, 0xa2, 0x3c)},
        {rgb(0xfe, 0xf0, 0xf0), rgb(0xfd, 0xe2, 0xe2), rgb(0xf5, 0x6c, 0x6c)},
        {rgb(0xf4, 0xf4, 0xf5), rgb(0xe9, 0xe9, 0xeb), rgb(0x90, 0x93, 0x99)},
    };
    const C& c = table[(int)type];
    (void)plain;
    ImGui::PushID(text);
    ImGui::PushFont(nullptr, theme::size::SMALL);
    ImVec2 ts = ImGui::CalcTextSize(text);
    float h = 22.0f, pad = 9.0f;
    float w = ts.x + pad * 2 + (closable ? 16.0f : 0.0f);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), u32(c.bg), 3.0f);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + h), u32(c.border), 3.0f);
    dl->AddText(ImVec2(pos.x + pad, pos.y + (h - ts.y) * 0.5f), u32(c.fg), text);
    bool clicked_close = false;
    if (closable) {
        ImVec2 xpos(pos.x + w - 14.0f, pos.y + h * 0.5f - 6.0f);
        dl->AddLine(ImVec2(xpos.x, xpos.y), ImVec2(xpos.x + 8, xpos.y + 8), u32(c.fg), 1.0f);
        dl->AddLine(ImVec2(xpos.x, xpos.y + 8), ImVec2(xpos.x + 8, xpos.y), u32(c.fg), 1.0f);
        ImGui::SetCursorScreenPos(ImVec2(xpos.x - 2, xpos.y - 2));
        ImGui::InvisibleButton("x", ImVec2(12, 12));
        clicked_close = ImGui::IsItemClicked();
    }
    ImGui::PopFont();
    ImGui::PopID();
    return clicked_close;
}

void badge(int count, bool is_dot, int max) {
    ImVec2 anchor_min = ImGui::GetItemRectMin();
    ImVec2 anchor_max = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec4 red = rgb(0xf5, 0x6c, 0x6c);
    ImVec2 c(anchor_max.x, anchor_min.y);
    if (is_dot) {
        dl->AddCircleFilled(c, 4.0f, u32(red));
        return;
    }
    char buf[8];
    if (count > max) std::snprintf(buf, sizeof(buf), "%d+", max);
    else std::snprintf(buf, sizeof(buf), "%d", count);
    ImGui::PushFont(nullptr, theme::size::SMALL * 0.8f);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    float r = std::max(8.0f, ts.y * 0.5f + 2.0f);
    dl->AddCircleFilled(c, r, u32(red));
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), u32(rgb(255, 255, 255)), buf);
    ImGui::PopFont();
}

void avatar(const char* initials, float size) {
    static const ImVec4 palette[] = {
        rgb(0x40, 0x9e, 0xff), rgb(0x67, 0xc2, 0x3a), rgb(0xe6, 0xa2, 0x3c),
        rgb(0xf5, 0x6c, 0x6c), rgb(0x90, 0x93, 0x99),
    };
    unsigned h = 0;
    for (const char* p = initials; *p; p++) h = h * 31 + (unsigned char)*p;
    ImVec4 bg = palette[h % 5];

    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(pos.x + size * 0.5f, pos.y + size * 0.5f);
    dl->AddCircleFilled(c, size * 0.5f, u32(bg));
    ImGui::PushFont(fonts::medium(), theme::size::SMALL);
    ImVec2 ts = ImGui::CalcTextSize(initials);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), u32(rgb(255, 255, 255)),
                initials);
    ImGui::PopFont();
}

void alert(const char* title, AlertType type, const char* description, bool* open) {
    if (open && !*open) return;
    struct C { ImVec4 bg, fg; const char* icon; };
    static const C table[] = {
        {rgb(0xf0, 0xf9, 0xeb), rgb(0x67, 0xc2, 0x3a), ICON_SUCCESS},
        {rgb(0xfd, 0xf6, 0xec), rgb(0xe6, 0xa2, 0x3c), ICON_WARNING},
        {rgb(0xfe, 0xf0, 0xf0), rgb(0xf5, 0x6c, 0x6c), ICON_ERROR},
        {rgb(0xf4, 0xf4, 0xf5), rgb(0x90, 0x93, 0x99), ICON_INFO},
    };
    const C& c = table[(int)type];
    ImGui::PushID(title);

    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushFont(nullptr, theme::size::SMALL);
    float title_h = ImGui::CalcTextSize(title).y;
    float desc_h = 0.0f;
    if (description) {
        ImGui::PushFont(nullptr, theme::size::SMALL * 0.9f);
        desc_h = ImGui::CalcTextSize(description, nullptr, false, w - 60.0f).y + 4.0f;
        ImGui::PopFont();
    }
    float h = 12.0f * 2 + title_h + desc_h;
    ImGui::PopFont();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), u32(c.bg), theme::RADIUS);

    ImGui::PushFont(fonts::body(), 17.0f);
    dl->AddText(ImVec2(pos.x + 12, pos.y + 12), u32(c.fg), c.icon);
    ImGui::PopFont();

    ImGui::PushFont(nullptr, theme::size::SMALL);
    dl->AddText(ImVec2(pos.x + 34, pos.y + 12), u32(c.fg), title);
    ImGui::PopFont();
    if (description) {
        ImGui::PushFont(nullptr, theme::size::SMALL * 0.9f);
        ImVec2 tp(pos.x + 34, pos.y + 12 + title_h + 4);
        ImGui::SetCursorScreenPos(tp);
        ImGui::PushTextWrapPos(pos.x + w - 30.0f);
        ImGui::TextColored(c.fg, "%s", description);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
    }

    if (open) {
        ImGui::SetCursorScreenPos(ImVec2(pos.x + w - 26, pos.y + 10));
        ImGui::InvisibleButton("close", ImVec2(16, 16));
        if (ImGui::IsItemClicked()) *open = false;
        ImVec2 xp(pos.x + w - 22, pos.y + 14);
        dl->AddLine(xp, ImVec2(xp.x + 8, xp.y + 8), u32(c.fg), 1.2f);
        dl->AddLine(ImVec2(xp.x, xp.y + 8), ImVec2(xp.x + 8, xp.y), u32(c.fg), 1.2f);
    }

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    ImGui::Dummy(ImVec2(w, 0));
    ImGui::PopID();
}

void divider(const char* text) {
    const theme::Palette& p = theme::palette();
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float cy = pos.y + 10.0f;
    if (!text || !text[0]) {
        dl->AddLine(ImVec2(pos.x, cy), ImVec2(pos.x + w, cy), u32(p.border), 1.0f);
    } else {
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImVec2 ts = ImGui::CalcTextSize(text);
        ImGui::PopFont();
        float gap = 16.0f;
        float tx = pos.x + (w - ts.x) * 0.5f;
        dl->AddLine(ImVec2(pos.x, cy), ImVec2(tx - gap, cy), u32(p.border), 1.0f);
        dl->AddLine(ImVec2(tx + ts.x + gap, cy), ImVec2(pos.x + w, cy), u32(p.border), 1.0f);
        ImGui::PushFont(nullptr, theme::size::SMALL);
        dl->AddText(ImVec2(tx, cy - ts.y * 0.5f), u32(p.subtle_text), text);
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(w, 20.0f));
}

void card_begin(const char* header) {
    const theme::Palette& p = theme::palette();
    ImGui::PushID(header ? header : "card");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.ui);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::RADIUS);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14));
    ImGui::BeginChild("##card", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    if (header) {
        ImGui::PushFont(fonts::medium(), theme::size::SMALL);
        ImGui::TextUnformatted(header);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 4));
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p0.x - 18, p0.y), ImVec2(p0.x + ImGui::GetContentRegionAvail().x + 18, p0.y), u32(p.border));
        ImGui::Dummy(ImVec2(0, 10));
    }
}

void card_end() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

// ===========================================================================
// Data (Batch 2)
// ===========================================================================
void table(const char* id, const char* const* headers, int col_count,
          const char* const* rows, int row_count) {
    const theme::Palette& p = theme::palette();
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_TableBorderLight, u32(p.border));
    ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, u32(p.border));
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, u32(rgb(0xfa, 0xfa, 0xfa)));
    ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, u32(rgb(0xfa, 0xfa, 0xfa)));
    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("t", col_count, flags)) {
        for (int c = 0; c < col_count; c++) ImGui::TableSetupColumn(headers[c]);
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImGui::TableHeadersRow();
        for (int r = 0; r < row_count; r++) {
            ImGui::TableNextRow();
            for (int c = 0; c < col_count; c++) {
                ImGui::TableSetColumnIndex(c);
                ImGui::TextColored(p.text, "%s", rows[r * col_count + c]);
            }
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
    ImGui::PopStyleColor(4);
    ImGui::PopID();
}

void progress_circle(float frac, float radius, const char* text) {
    frac = std::clamp(frac, 0.0f, 1.0f);
    const theme::Palette& p = theme::palette();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(pos.x + radius, pos.y + radius);
    float thick = radius * 0.14f;
    dl->PathArcTo(c, radius - thick, -1.5707963f, 6.2831853f - 1.5707963f, 48);
    dl->PathStroke(u32(p.deep), 0, thick);
    if (frac > 0.0f) {
        float a1 = -1.5707963f + frac * 6.2831853f;
        dl->PathArcTo(c, radius - thick, -1.5707963f, a1, std::max(1, (int)(48 * frac)));
        dl->PathStroke(u32(rgb(0x67, 0xc2, 0x3a)), 0, thick);
    }
    char buf[16];
    if (!text) { std::snprintf(buf, sizeof(buf), "%d%%", (int)(frac * 100)); text = buf; }
    ImGui::PushFont(nullptr, theme::size::SMALL);
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), u32(p.text), text);
    ImGui::PopFont();
}

bool tree_node(const char* label, bool leaf, bool selected) {
    const theme::Palette& p = theme::palette();
    ImGui::PushID(label);
    float h = 26.0f;
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGuiStorage* st = ImGui::GetStateStorage();
    ImGuiID open_id = ImGui::GetID("open");
    bool open = leaf || st->GetBool(open_id, false);

    ImGui::InvisibleButton("row", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked() && !leaf) { open = !open; st->SetBool(open_id, open); }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected)
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), u32(p.selected));
    else if (hovered)
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), u32(mix(p.ui, p.accent, 0.04f)));

    float x = pos.x + 4.0f;
    if (!leaf) {
        ImGui::PushFont(fonts::body(), 13.0f);
        ImVec2 its = ImGui::CalcTextSize(ICON_CHEVRON_RIGHT);
        ImVec2 ip(x, pos.y + (h - its.y) * 0.5f);
        if (open) {
            // Rotate 90deg visually by drawing a simple down-caret via text
            // baseline trick isn't available for a PUA glyph, so just nudge
            // colour/weight to show state instead of a true rotation.
            dl->AddText(ip, u32(p.accent), ICON_CHEVRON_RIGHT);
        } else {
            dl->AddText(ip, u32(p.subtle_text), ICON_CHEVRON_RIGHT);
        }
        ImGui::PopFont();
    }
    x += 18.0f;
    ImGui::PushFont(nullptr, theme::size::SMALL);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(x, pos.y + (h - ts.y) * 0.5f), u32(selected ? p.accent : p.text), label);
    ImGui::PopFont();

    ImGui::PopID();
    return !leaf && open;
}

void tree_pop() {}

bool pagination(const char* id, int* current, int total_pages) {
    const theme::Palette& p = theme::palette();
    ImGui::PushID(id);
    bool changed = false;
    float h = 28.0f, bw = 28.0f;

    auto arrow_btn = [&](const char* glyph, int target, bool enabled) {
        ImGui::PushID(glyph);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("a", ImVec2(bw, h));
        bool hov = enabled && ImGui::IsItemHovered();
        if (enabled && ImGui::IsItemClicked()) { *current = target; changed = true; }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hov) dl->AddRectFilled(pos, ImVec2(pos.x + bw, pos.y + h), u32(p.selected), theme::RADIUS);
        ImGui::PushFont(fonts::body(), 14.0f);
        ImVec2 ts = ImGui::CalcTextSize(glyph);
        ImVec4 col = enabled ? (hov ? p.accent : p.text) : p.subtle_text;
        dl->AddText(ImVec2(pos.x + (bw - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), u32(col), glyph);
        ImGui::PopFont();
        ImGui::PopID();
    };

    arrow_btn(ICON_ARROW_BACK, *current - 1, *current > 1);
    ImGui::SameLine(0, 4);

    int lo = std::max(1, *current - 2), hi = std::min(total_pages, *current + 2);
    for (int i = lo; i <= hi; i++) {
        ImGui::PushID(i);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("p", ImVec2(bw, h));
        bool hov = ImGui::IsItemHovered();
        bool on = (i == *current);
        if (ImGui::IsItemClicked() && !on) { *current = i; changed = true; }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (on) dl->AddRectFilled(pos, ImVec2(pos.x + bw, pos.y + h), u32(p.accent), theme::RADIUS);
        else if (hov) dl->AddRectFilled(pos, ImVec2(pos.x + bw, pos.y + h), u32(p.selected), theme::RADIUS);
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%d", i);
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImVec2 ts = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(pos.x + (bw - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f),
                    u32(on ? rgb(255, 255, 255) : (hov ? p.accent : p.text)), buf);
        ImGui::PopFont();
        ImGui::PopID();
        ImGui::SameLine(0, 4);
    }

    arrow_btn(ICON_ARROW_FORWARD, *current + 1, *current < total_pages);
    ImGui::NewLine();
    ImGui::PopID();
    return changed;
}

// ===========================================================================
// Notice (Batch 3)
// ===========================================================================
namespace {

struct NoticeColors { ImVec4 bg, fg, icon_bg; const char* icon; };
const NoticeColors& notice_colors(NoticeType t) {
    static const NoticeColors table[] = {
        {rgb(0xf0, 0xf9, 0xeb), rgb(0x67, 0xc2, 0x3a), rgb(0x67, 0xc2, 0x3a), ICON_SUCCESS},
        {rgb(0xfd, 0xf6, 0xec), rgb(0xe6, 0xa2, 0x3c), rgb(0xe6, 0xa2, 0x3c), ICON_WARNING},
        {rgb(0xfe, 0xf0, 0xf0), rgb(0xf5, 0x6c, 0x6c), rgb(0xf5, 0x6c, 0x6c), ICON_ERROR},
        {rgb(0xf4, 0xf4, 0xf5), rgb(0x90, 0x93, 0x99), rgb(0x90, 0x93, 0x99), ICON_INFO},
    };
    return table[(int)t];
}

struct Toast {
    std::string title; // message: the text; notify: the bold title
    std::string body;  // notify only; empty for message
    NoticeType type;
    float created;
    float duration;
    bool is_notify;
};
std::vector<Toast> g_toasts;

} // namespace

void message(const char* text, NoticeType type, float duration) {
    g_toasts.push_back({text, "", type, (float)ImGui::GetTime(), duration, false});
}

void notify(const char* title, const char* description, NoticeType type, float duration) {
    g_toasts.push_back({title, description ? description : "", type, (float)ImGui::GetTime(),
                        duration, true});
}

void render_notices() {
    float now = (float)ImGui::GetTime();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImGuiViewport* vp = ImGui::GetMainViewport();

    float msg_y = vp->Pos.y + 24.0f;
    float notif_y = vp->Pos.y + 24.0f;
    const float fade = 0.3f;

    for (size_t i = 0; i < g_toasts.size();) {
        Toast& t = g_toasts[i];
        float age = now - t.created;
        if (age >= t.duration) { g_toasts.erase(g_toasts.begin() + i); continue; }
        float alpha = age < fade ? age / fade
                     : (age > t.duration - fade ? (t.duration - age) / fade : 1.0f);
        alpha = std::clamp(alpha, 0.0f, 1.0f);
        const NoticeColors& c = notice_colors(t.type);

        if (!t.is_notify) {
            ImGui::PushFont(nullptr, theme::size::SMALL);
            ImVec2 ts = ImGui::CalcTextSize(t.title.c_str());
            ImGui::PopFont();
            float w = ts.x + 44.0f, h = 40.0f;
            float x = vp->Pos.x + (vp->Size.x - w) * 0.5f;
            ImVec2 p0(x, msg_y), p1(x + w, msg_y + h);
            dl->AddRectFilled(p0, p1, u32(ImVec4(c.bg.x, c.bg.y, c.bg.z, c.bg.w * alpha)), 4.0f);
            dl->AddRect(p0, p1, u32(ImVec4(c.fg.x, c.fg.y, c.fg.z, 0.3f * alpha)), 4.0f);
            ImGui::PushFont(fonts::body(), 15.0f);
            ImVec2 its = ImGui::CalcTextSize(c.icon);
            dl->AddText(ImVec2(p0.x + 14, p0.y + (h - its.y) * 0.5f),
                        u32(ImVec4(c.fg.x, c.fg.y, c.fg.z, alpha)), c.icon);
            ImGui::PopFont();
            ImGui::PushFont(nullptr, theme::size::SMALL);
            dl->AddText(ImVec2(p0.x + 14 + its.x + 8, p0.y + (h - ts.y) * 0.5f),
                        u32(ImVec4(c.fg.x, c.fg.y, c.fg.z, alpha)), t.title.c_str());
            ImGui::PopFont();
            msg_y += h + 10.0f;
        } else {
            float w = 300.0f;
            ImGui::PushFont(fonts::medium(), theme::size::SMALL);
            ImVec2 tts = ImGui::CalcTextSize(t.title.c_str());
            ImGui::PopFont();
            float body_h = 0.0f;
            if (!t.body.empty()) {
                ImGui::PushFont(nullptr, theme::size::SMALL * 0.9f);
                body_h = ImGui::CalcTextSize(t.body.c_str(), nullptr, false, w - 60.0f).y + 6.0f;
                ImGui::PopFont();
            }
            float h = 20.0f + tts.y + body_h;
            float x = vp->Pos.x + vp->Size.x - w - 20.0f;
            ImVec2 p0(x, notif_y), p1(x + w, notif_y + h);
            ImVec4 bg = rgb(0xff, 0xff, 0xff);
            dl->AddRectFilled(p0, p1, u32(ImVec4(bg.x, bg.y, bg.z, alpha)), theme::RADIUS);
            dl->AddRect(p0, p1, u32(ImVec4(0.82f, 0.84f, 0.9f, 0.6f * alpha)), theme::RADIUS);
            ImGui::PushFont(fonts::body(), 18.0f);
            ImVec2 its = ImGui::CalcTextSize(c.icon);
            dl->AddText(ImVec2(p0.x + 16, p0.y + 14),
                        u32(ImVec4(c.fg.x, c.fg.y, c.fg.z, alpha)), c.icon);
            ImGui::PopFont();
            float tx = p0.x + 16 + its.x + 10;
            ImGui::PushFont(fonts::medium(), theme::size::SMALL);
            dl->AddText(ImVec2(tx, p0.y + 14), u32(ImVec4(0.19f, 0.19f, 0.2f, alpha)),
                        t.title.c_str());
            ImGui::PopFont();
            if (!t.body.empty()) {
                ImGui::PushFont(nullptr, theme::size::SMALL * 0.9f);
                dl->AddText(ImVec2(tx, p0.y + 14 + tts.y + 6),
                            u32(ImVec4(0.38f, 0.39f, 0.4f, alpha)), t.body.c_str());
                ImGui::PopFont();
            }
            notif_y += h + 12.0f;
        }
        i++;
    }
}

void loading_overlay(ImVec2 region_min, ImVec2 region_max, bool active) {
    if (!active) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(region_min, region_max, u32(rgb(255, 255, 255, 0.85f)));
    float radius = 14.0f;
    ImVec2 c((region_min.x + region_max.x) * 0.5f, (region_min.y + region_max.y) * 0.5f);
    float t = (float)ImGui::GetTime() * 6.0f;
    int seg = 20;
    for (int i = 0; i < seg; i++) {
        float a0 = t + (float)i / seg * 6.2831853f;
        float a1 = t + (float)(i + 1) / seg * 6.2831853f;
        float alpha = (float)i / seg;
        dl->PathArcTo(c, radius, a0, a1, 3);
        dl->PathStroke(u32(ImVec4(0.25f, 0.62f, 1.0f, alpha)), 0, 2.5f);
    }
}

MessageBoxResult message_box(const char* id, const char* title, const char* text,
                             bool* open, bool show_cancel) {
    const theme::Palette& p = theme::palette();
    MessageBoxResult result = MessageBoxResult::None;
    if (*open && !ImGui::IsPopupOpen(id)) ImGui::OpenPopup(id);

    ImGui::SetNextWindowSize(ImVec2(320, 0));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme::RADIUS);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 16));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, p.ui);
    if (ImGui::BeginPopupModal(id, nullptr, flags)) {
        ImGui::PushFont(fonts::medium(), theme::size::SMALL);
        ImGui::TextUnformatted(title);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 10));
        ImGui::PushFont(nullptr, theme::size::SMALL * 0.95f);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 280.0f);
        ImGui::TextColored(p.text, "%s", text);
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 16));

        float bw = show_cancel ? 76.0f : 88.0f;
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() - (show_cancel ? bw * 2 + 28 : bw + 20));
        if (show_cancel) {
            if (button("Cancel", ButtonType::Default, {})) {
                result = MessageBoxResult::Cancel;
                *open = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine(0, 8);
        }
        if (button("OK", ButtonType::Primary, {})) {
            result = MessageBoxResult::Confirm;
            *open = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    return result;
}

// ===========================================================================
// Navigation (Batch 4)
// ===========================================================================
bool tabs(const char* id, int* current, const char* const* labels, int count) {
    const theme::Palette& p = theme::palette();
    ImGui::PushID(id);
    bool changed = false;
    float h = 36.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddLine(ImVec2(pos.x, pos.y + h), ImVec2(pos.x + w, pos.y + h), u32(p.border));

    float x = pos.x;
    for (int i = 0; i < count; i++) {
        ImGui::PushID(i);
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        ImGui::PopFont();
        float tw = ts.x + 32.0f;
        ImGui::SetCursorScreenPos(ImVec2(x, pos.y));
        ImGui::InvisibleButton("tab", ImVec2(tw, h));
        bool hovered = ImGui::IsItemHovered();
        bool on = (*current == i);
        if (ImGui::IsItemClicked() && !on) { *current = i; changed = true; }

        ImVec4 col = on ? p.accent : (hovered ? p.light : p.text);
        dl->AddText(ImVec2(x + (tw - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), u32(col), labels[i]);
        if (on)
            dl->AddRectFilled(ImVec2(x + 8, pos.y + h - 2), ImVec2(x + tw - 8, pos.y + h),
                              u32(p.accent));
        x += tw;
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    ImGui::Dummy(ImVec2(w, 0));
    ImGui::PopID();
    return changed;
}

int breadcrumb(const char* const* labels, int count) {
    const theme::Palette& p = theme::palette();
    int clicked = -1;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushFont(nullptr, theme::size::SMALL);
    for (int i = 0; i < count; i++) {
        bool last = (i == count - 1);
        ImGui::PushID(i);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        if (last) {
            dl->AddText(pos, u32(p.text), labels[i]);
            ImGui::Dummy(ts);
        } else {
            ImGui::InvisibleButton("crumb", ts);
            bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) clicked = i;
            dl->AddText(pos, u32(hovered ? p.accent : p.subtle_text), labels[i]);
        }
        ImGui::PopID();
        if (!last) {
            ImGui::SameLine(0, 6);
            ImGui::PushFont(fonts::body(), 13.0f);
            ImGui::TextColored(p.subtle_text, "%s", ICON_CHEVRON_RIGHT);
            ImGui::PopFont();
            ImGui::SameLine(0, 6);
        }
    }
    ImGui::PopFont();
    return clicked;
}

void steps(const char* const* labels, int count, int current) {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x;
    float seg = w / (float)count;
    float r = 12.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float cy = pos.y + r;

    for (int i = 0; i < count; i++) {
        float cx = pos.x + seg * i + r + (i == 0 ? 0.0f : 0.0f);
        ImVec2 c(cx, cy);
        bool done = i < current, active = i == current;

        if (i > 0) {
            ImVec2 prev(pos.x + seg * (i - 1) + r, cy);
            dl->AddLine(ImVec2(prev.x + r + 4, cy), ImVec2(c.x - r - 4, cy),
                        u32(i <= current ? p.accent : p.border), 1.5f);
        }

        if (done) {
            dl->AddCircleFilled(c, r, u32(p.accent));
            ImGui::PushFont(fonts::body(), 13.0f);
            ImVec2 its = ImGui::CalcTextSize(ICON_CHECK);
            dl->AddText(ImVec2(c.x - its.x * 0.5f, c.y - its.y * 0.5f), u32(rgb(255, 255, 255)),
                        ICON_CHECK);
            ImGui::PopFont();
        } else {
            dl->AddCircleFilled(c, r, u32(active ? rgb(255, 255, 255) : p.deep));
            dl->AddCircle(c, r, u32(active ? p.accent : p.border), 0, 1.5f);
            char buf[8];
            std::snprintf(buf, sizeof(buf), "%d", i + 1);
            ImGui::PushFont(nullptr, theme::size::SMALL * 0.85f);
            ImVec2 nts = ImGui::CalcTextSize(buf);
            dl->AddText(ImVec2(c.x - nts.x * 0.5f, c.y - nts.y * 0.5f),
                        u32(active ? p.accent : p.subtle_text), buf);
            ImGui::PopFont();
        }

        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImVec2 lts = ImGui::CalcTextSize(labels[i]);
        dl->AddText(ImVec2(c.x - lts.x * 0.5f, cy + r + 8),
                    u32(active || done ? p.text : p.subtle_text), labels[i]);
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(w, r * 2 + 28));
}

int dropdown(const char* id, const char* label, const char* const* items, int count) {
    const theme::Palette& p = theme::palette();
    ImGui::PushID(id);
    int clicked = -1;

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s  %s", label, ICON_EXPAND_MORE);
    if (button(buf)) ImGui::OpenPopup("menu");

    ImGui::PushStyleColor(ImGuiCol_PopupBg, p.ui);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, mix(p.ui, p.accent, 0.08f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, mix(p.ui, p.accent, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme::RADIUS);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
    if (ImGui::BeginPopup("menu")) {
        ImGui::PushFont(nullptr, theme::size::SMALL);
        for (int i = 0; i < count; i++) {
            if (ImGui::Selectable(items[i])) clicked = i;
        }
        ImGui::PopFont();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(5);
    ImGui::PopID();
    return clicked;
}

} // namespace el
