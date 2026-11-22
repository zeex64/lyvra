#include "ui.h"

#include <cfloat>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "app.h"
#include "imgui.h"
#include "imgui_internal.h"

extern const unsigned char font_Inter_latin_400_normal[], font_Inter_latin_600_normal[], font_Inter_latin_700_normal[];
extern const unsigned char font_JetBrainsMono_latin_400_normal[], font_JetBrainsMono_latin_600_normal[];
extern const size_t font_Inter_latin_400_normal_size, font_Inter_latin_600_normal_size, font_Inter_latin_700_normal_size;
extern const size_t font_JetBrainsMono_latin_400_normal_size, font_JetBrainsMono_latin_600_normal_size;

namespace ui {
namespace {

// ---- Palette (matches the mockups) -----------------------------------------
constexpr uint32_t kBg = 0x0B0D10, kVideoBg = 0x07080A, kPanel = 0x14171C, kPanel2 = 0x1B1F26, kBorder = 0x2A2F38;
constexpr uint32_t kText = 0xE8EAED, kMuted = 0xA1A8B3, kDim = 0x7D8591, kIcon = 0xC9CED6;
constexpr uint32_t kAccent = 0x5AD1C8, kAccentText = 0x8FE3DC, kAccentBg = 0x12302D, kAccentBorder = 0x1F5A54, kAccentSel = 0x12201F;
constexpr uint32_t kWarn = 0xF2B248, kWarnText = 0xF7CD7E, kWarnBody = 0xD9C9A6, kWarnBg = 0x221C10, kWarnBorder = 0x5A4720, kWarnBadge = 0x3A2E14;
constexpr uint32_t kBlue = 0x6EA8FE;

constexpr float kTopBarH = 52, kBottomBarH = 34;

ImU32 col(uint32_t hex, float a = 1.0f) {
    return IM_COL32((hex >> 16) & 255, (hex >> 8) & 255, hex & 255, int(a * 255));
}
ImVec4 colv(uint32_t hex, float a = 1.0f) {
    return ImVec4(((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, a);
}

App* g = nullptr;
Fonts& F() { return g->fonts; }

ImVec2 measure(ImFont* f, float size, const char* s) { return f->CalcTextSizeA(size, FLT_MAX, 0, s); }
void text(ImDrawList* dl, ImFont* f, float size, ImVec2 p, uint32_t c, const char* s, float wrap = 0) {
    dl->AddText(f, size, p, col(c), s, nullptr, wrap);
}

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
    char b[256];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}

void textColored(ImFont* f, float size, uint32_t c, const char* s) {
    ImGui::PushFont(f, size);
    ImGui::PushStyleColor(ImGuiCol_Text, colv(c));
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
void textWrapped(ImFont* f, float size, uint32_t c, const char* s) {
    ImGui::PushFont(f, size);
    ImGui::PushStyleColor(ImGuiCol_Text, colv(c));
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(s);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// ---- Icons (vector, so they stay crisp at any DPI) --------------------------
void iconScreen(ImDrawList* dl, ImVec2 c, float s, ImU32 k, float th = 2) {
    dl->AddRect({c.x - s * .45f, c.y - s * .3f}, {c.x + s * .45f, c.y + s * .3f}, k, s * .08f, 0, th);
    dl->AddTriangleFilled({c.x - s * .1f, c.y - s * .14f}, {c.x - s * .1f, c.y + s * .14f}, {c.x + s * .16f, c.y}, k);
}
// The Lyvra mark: screen + play with motion streaks fading in from the left (matches data/icons/lyvra.svg).
void iconLogo(ImDrawList* dl, ImVec2 c, float s, uint32_t hex) {
    ImU32 k = col(hex);
    dl->AddRect({c.x - s * .47f, c.y - s * .31f}, {c.x + s * .47f, c.y + s * .31f}, k, s * .12f, 0, std::max(1.5f, s * .07f));
    float th = std::max(1.2f, s * .05f);
    for (int i = -1; i <= 1; i++) {
        float y = c.y + i * s * .09f, x0 = c.x - (i == 0 ? s * .36f : s * .24f), x1 = c.x - s * .07f;
        dl->AddRectFilledMultiColor({x0, y - th * .5f}, {x1, y + th * .5f}, col(hex, 0), k, k, col(hex, 0));
    }
    dl->AddTriangleFilled({c.x - s * .02f, c.y - s * .14f}, {c.x - s * .02f, c.y + s * .14f}, {c.x + s * .21f, c.y}, k);
}

void iconBars(ImDrawList* dl, ImVec2 c, float s, ImU32 k) {
    float base = c.y + s * .4f;
    dl->AddLine({c.x - s * .45f, base}, {c.x + s * .45f, base}, k, 2);
    float hs[4] = {.35f, .7f, .5f, .25f};
    for (int i = 0; i < 4; i++) {
        float x = c.x - s * .3f + i * s * .2f;
        dl->AddLine({x, base - s * .12f}, {x, base - s * .12f - s * hs[i]}, k, 2);
    }
}
void iconGear(ImDrawList* dl, ImVec2 c, float s, ImU32 k) {
    dl->AddCircle(c, s * .14f, k, 16, 2);
    dl->AddCircle(c, s * .3f, k, 24, 2);
    for (int i = 0; i < 8; i++) {
        float a = i * 3.14159f / 4;
        dl->AddLine({c.x + cosf(a) * s * .3f, c.y + sinf(a) * s * .3f}, {c.x + cosf(a) * s * .46f, c.y + sinf(a) * s * .46f}, k, 2.6f);
    }
}
void iconExpand(ImDrawList* dl, ImVec2 c, float s, ImU32 k, bool inward) {
    float o = s * .42f, l = s * .2f;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) {
            ImVec2 corner = inward ? ImVec2(c.x + sx * (o - l), c.y + sy * (o - l)) : ImVec2(c.x + sx * o, c.y + sy * o);
            float dx = inward ? sx * l : -sx * l, dy = inward ? sy * l : -sy * l;
            dl->AddLine(corner, {corner.x + dx, corner.y}, k, 2);
            dl->AddLine(corner, {corner.x, corner.y + dy}, k, 2);
        }
}
void iconSpeaker(ImDrawList* dl, ImVec2 c, float s, ImU32 k) {
    ImVec2 pts[6] = {{c.x - s * .45f, c.y - s * .15f}, {c.x - s * .2f, c.y - s * .15f}, {c.x + s * .05f, c.y - s * .38f},
                     {c.x + s * .05f, c.y + s * .38f}, {c.x - s * .2f, c.y + s * .15f}, {c.x - s * .45f, c.y + s * .15f}};
    dl->AddPolyline(pts, 6, k, ImDrawFlags_Closed, 1.6f);
    dl->PathArcTo({c.x + s * .05f, c.y}, s * .3f, -0.8f, 0.8f, 8);
    dl->PathStroke(k, 0, 1.6f);
}
void iconBolt(ImDrawList* dl, ImVec2 c, float s, ImU32 k) {
    ImVec2 p[6] = {{c.x + s * .08f, c.y - s * .5f}, {c.x - s * .32f, c.y + s * .08f}, {c.x - s * .02f, c.y + s * .08f},
                   {c.x - s * .1f, c.y + s * .5f}, {c.x + s * .32f, c.y - s * .1f}, {c.x + s * .02f, c.y - s * .1f}};
    dl->AddConcavePolyFilled(p, 6, k);
}
void iconChevron(ImDrawList* dl, ImVec2 c, float s, ImU32 k) {
    dl->AddLine({c.x - s * .3f, c.y - s * .15f}, {c.x, c.y + s * .15f}, k, 1.8f);
    dl->AddLine({c.x, c.y + s * .15f}, {c.x + s * .3f, c.y - s * .15f}, k, 1.8f);
}
void iconClose(ImDrawList* dl, ImVec2 c, float s, ImU32 k) {
    dl->AddLine({c.x - s * .3f, c.y - s * .3f}, {c.x + s * .3f, c.y + s * .3f}, k, 2);
    dl->AddLine({c.x - s * .3f, c.y + s * .3f}, {c.x + s * .3f, c.y - s * .3f}, k, 2);
}
void iconArrow(ImDrawList* dl, ImVec2 c, float s, ImU32 k) {
    dl->AddLine({c.x - s * .4f, c.y}, {c.x + s * .4f, c.y}, k, 1.6f);
    dl->AddLine({c.x + s * .1f, c.y - s * .3f}, {c.x + s * .4f, c.y}, k, 1.6f);
    dl->AddLine({c.x + s * .1f, c.y + s * .3f}, {c.x + s * .4f, c.y}, k, 1.6f);
}
void iconWarn(ImDrawList* dl, ImVec2 c, float s, ImU32 k) {
    dl->AddTriangle({c.x, c.y - s * .45f}, {c.x - s * .5f, c.y + s * .4f}, {c.x + s * .5f, c.y + s * .4f}, k, 1.8f);
    dl->AddLine({c.x, c.y - s * .12f}, {c.x, c.y + s * .12f}, k, 1.8f);
    dl->AddCircleFilled({c.x, c.y + s * .26f}, 1.3f, k);
}

// ---- Widgets ---------------------------------------------------------------
using IconFn = void (*)(ImDrawList*, ImVec2, float, ImU32);

bool iconButton(const char* id, IconFn icon, const char* tooltip, float size = 36) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, {size, size});
    bool hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hov) dl->AddRectFilled(p, {p.x + size, p.y + size}, col(kPanel2), 8);
    icon(dl, {p.x + size / 2, p.y + size / 2}, 18, col(hov ? kText : kIcon));
    if (hov && tooltip) ImGui::SetTooltip("%s", tooltip);
    return clicked;
}

float chipWidth(const char* label, ImFont* f, float extra = 0) { return measure(f, 12, label).x + 20 + extra; }

void chip(const char* label, uint32_t bg, uint32_t border, uint32_t fg, ImFont* f, float h = 28, IconFn icon = nullptr) {
    float w = chipWidth(label, f, icon ? 18 : 0);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy({w, h});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + w, p.y + h}, col(bg), 6);
    dl->AddRect(p, {p.x + w, p.y + h}, col(border), 6);
    float x = p.x + 10;
    if (icon) { icon(dl, {x + 6, p.y + h / 2}, 12, col(fg)); x += 18; }
    text(dl, f, 12, {x, p.y + (h - 12) / 2 - 1}, fg, label);
}

bool primaryButton(const char* label, ImVec2 size = {0, 40}, bool enabled = true) {
    ImGui::PushFont(F().semibold, 13);
    if (size.x == 0) size.x = measure(F().semibold, 13, label).x + 40;
    ImGui::PushStyleColor(ImGuiCol_Button, colv(kAccent, enabled ? 1 : .35f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colv(kAccentText));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colv(kAccent));
    ImGui::PushStyleColor(ImGuiCol_Text, colv(kBg));
    ImGui::BeginDisabled(!enabled);
    bool r = ImGui::Button(label, size);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    ImGui::PopFont();
    return r;
}

bool secondaryButton(const char* label, ImVec2 size = {0, 40}) {
    ImGui::PushFont(F().regular, 13);
    if (size.x == 0) size.x = measure(F().regular, 13, label).x + 32;
    ImGui::PushStyleColor(ImGuiCol_Button, colv(kPanel2, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colv(kPanel2));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, colv(kBorder));
    ImGui::PushStyleColor(ImGuiCol_Text, colv(kIcon));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    ImGui::PopFont();
    return r;
}

// Segmented control; returns true if changed.
bool segmented(const char* id, const char* const* labels, int n, int* value) {
    ImGui::PushID(id);
    bool changed = false;
    for (int i = 0; i < n; i++) {
        if (i) ImGui::SameLine(0, 6);
        bool sel = *value == i;
        ImGui::PushFont(sel ? F().semibold : F().regular, 13);
        ImGui::PushStyleColor(ImGuiCol_Button, colv(sel ? kAccent : kPanel2));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colv(sel ? kAccentText : kBorder));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colv(sel ? kAccent : kBorder));
        ImGui::PushStyleColor(ImGuiCol_Text, colv(sel ? kBg : kIcon));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, sel ? 0.f : 1.f);
        ImGui::PushID(i);
        if (ImGui::Button(labels[i], {0, 32})) { *value = i; changed = true; }
        ImGui::PopID();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        ImGui::PopFont();
    }
    ImGui::PopID();
    return changed;
}

struct CardText { const char* title; const char* badge; const char* desc; const char* footer; };

// Selectable card used for upscaler / present-mode choices.
bool card(const char* id, const CardText& t, float w, float h, bool selected, bool enabled, const char* tooltip = nullptr) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    bool clicked = ImGui::InvisibleButton(id, {w, h});
    ImGui::EndDisabled();
    bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float a = enabled ? 1.f : .55f;
    dl->AddRectFilled(p, {p.x + w, p.y + h}, col(selected ? kAccentSel : kPanel, 1), 10);
    dl->AddRect(p, {p.x + w, p.y + h}, col(selected ? kAccent : (hov && enabled ? 0x3A404A : kBorder)), 10, 0, selected ? 1.5f : 1.f);
    // radio
    ImVec2 rc{p.x + 22, p.y + 24};
    dl->AddCircle(rc, 7, col(selected ? kAccent : kDim, a), 16, 1.5f);
    if (selected) dl->AddCircleFilled(rc, 3.5f, col(kAccent));
    text(dl, F().semibold, 14, {p.x + 36, p.y + 15}, kText, t.title);
    if (t.badge) {
        ImVec2 bs = measure(F().semibold, 10, t.badge);
        ImVec2 bp{p.x + w - bs.x - 26, p.y + 15};
        bool good = !strcmp(t.badge, "RECOMMENDED");
        dl->AddRectFilled(bp, {bp.x + bs.x + 12, bp.y + 18}, col(good ? kAccentBg : kWarnBadge), 4);
        text(dl, F().semibold, 10, {bp.x + 6, bp.y + 3}, good ? kAccentText : kWarn, t.badge);
    }
    ImU32 descCol = col(kMuted, a);
    dl->AddText(F().regular, 12, {p.x + 16, p.y + 44}, descCol, t.desc, nullptr, w - 32);
    if (t.footer) text(dl, F().mono, 11, {p.x + 16, p.y + h - 24}, kDim, t.footer);
    if (hov && tooltip) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(320);
        ImGui::TextUnformatted(tooltip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
    return clicked && enabled;
}

void sectionTitle(const char* title, const char* sub = nullptr) {
    textColored(F().bold, 20, kText, title);
    if (sub) {
        ImGui::Dummy({0, 2});
        textWrapped(F().regular, 13, kMuted, sub);
    }
    ImGui::Dummy({0, 10});
}

void label(const char* s) { textColored(F().semibold, 13, kText, s); }

void note(uint32_t bg, uint32_t border, uint32_t titleCol, uint32_t bodyCol, const char* title, const char* body) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float bodyH = F().regular->CalcTextSizeA(13, FLT_MAX, w - 56, body).y;
    float h = 16 + 18 + 4 + bodyH + 14;
    ImGui::Dummy({w, h});
    dl->AddRectFilled(p, {p.x + w, p.y + h}, col(bg), 10);
    dl->AddRect(p, {p.x + w, p.y + h}, col(border), 10);
    iconWarn(dl, {p.x + 25, p.y + 24}, 16, col(kWarn));
    text(dl, F().semibold, 13, {p.x + 44, p.y + 15}, titleCol, title);
    text(dl, F().regular, 13, {p.x + 44, p.y + 37}, bodyCol, body, w - 56);
}

bool combo(const char* id, const char* preview, float width) {
    ImGui::SetNextItemWidth(width);
    ImGui::PushFont(F().regular, 13);
    bool open = ImGui::BeginCombo(id, preview);
    if (!open) ImGui::PopFont();
    return open;
}
void endCombo() {
    ImGui::EndCombo();
    ImGui::PopFont();
}

const char* upscalerName(Upscaler u) {
    switch (u) {
        case Upscaler::FSR1: return "FSR 1";
        case Upscaler::FSR3: return "FSR 3";
        case Upscaler::FSR4: return "FSR 4";
        case Upscaler::NIS: return "NIS";
        case Upscaler::Anime4K: return "Anime4K";
        default: return "Native";
    }
}

std::string res(uint32_t w, uint32_t h) { return fmt("%u×%u", w, h); }

// ---- Top bar ---------------------------------------------------------------
void topBar(float W, float y) {
    ImGui::SetNextWindowPos({0, y});
    ImGui::SetNextWindowSize({W, kTopBarH});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colv(kPanel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##top", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoFocusOnAppearing);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine({wp.x, wp.y + kTopBarH - 1}, {wp.x + W, wp.y + kTopBarH - 1}, col(kBorder));
    auto centerY = [](float h) { ImGui::SetCursorPosY((kTopBarH - h) / 2); };

    // Logo + name
    centerY(24);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy({24, 24});
    iconLogo(dl, {p.x + 12, p.y + 12}, 24, kAccent);
    ImGui::SameLine(0, 10);
    centerY(18);
    textColored(F().bold, 15, kText, APP_NAME);
    ImGui::SameLine(0, 18);

    // Device button
    const DeviceInfo* dev = g->currentDevice();
    std::string devName = g->capture.isOpen() ? g->capture.name : g->lost ? g->lostName : "No device";
    float bw = measure(F().regular, 13, devName.c_str()).x + 52;
    centerY(32);
    p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##dev", {bw, 32})) g->showPicker = true;
    bool hov = ImGui::IsItemHovered();
    if (hov) ImGui::SetTooltip("Change capture device (D)");
    dl->AddRectFilled(p, {p.x + bw, p.y + 32}, col(hov ? kBorder : kPanel2), 8);
    dl->AddRect(p, {p.x + bw, p.y + 32}, col(kBorder), 8);
    dl->AddCircleFilled({p.x + 16, p.y + 16}, 4, col(g->signalOk() ? kAccent : (dev || g->lost ? kWarn : kDim)));
    text(dl, F().regular, 13, {p.x + 28, p.y + 8}, kText, devName.c_str());
    iconChevron(dl, {p.x + bw - 16, p.y + 16}, 12, col(kMuted));
    ImGui::SameLine(0, 14);

    // Input -> output summary
    if (g->capture.isOpen()) {
        std::string inS = fmt("%s · %.0f Hz · %s", res(g->capture.width, g->capture.height).c_str(), g->capture.fps, fourccName(g->capture.fourcc));
        std::string outS = fmt("%s · %s", res(g->renderer.outW, g->renderer.outH).c_str(), upscalerName(g->renderer.effectiveUpscaler));
        centerY(16);
        p = ImGui::GetCursorScreenPos();
        float w1 = measure(F().mono, 12, inS.c_str()).x, w2 = measure(F().mono, 12, outS.c_str()).x;
        ImGui::Dummy({w1 + 28 + w2, 16});
        text(dl, F().mono, 12, {p.x, p.y + 1}, kMuted, inS.c_str());
        iconArrow(dl, {p.x + w1 + 14, p.y + 8}, 14, col(kDim));
        text(dl, F().mono, 12, {p.x + w1 + 28, p.y + 1}, kText, outS.c_str());
    }

    // Right-aligned group
    std::string up = g->settings.upscaler == Upscaler::Off ? "Upscaling Off" : std::string(upscalerName(g->settings.upscaler));
    if (g->settings.upscaler != Upscaler::Off && g->renderer.effectiveUpscaler == Upscaler::Off) up += " · bypassed";
    else if ((g->settings.upscaler == Upscaler::FSR3 || g->settings.upscaler == Upscaler::FSR4) && g->renderer.effectiveUpscaler == Upscaler::FSR1)
        up = std::string(upscalerName(g->settings.upscaler)) + " · fell back to FSR 1";
    if (g->renderer.showingMotion) up = "Motion view";
    std::string lat = g->stats.count ? fmt("%.1f ms", g->stats.latAvg) : "-- ms";
    bool fgOn = g->settings.frameGen && g->stats.genFps > 1;
    const char* fgLabel = fgOn ? (g->settings.fgPredict ? "Frame Gen 2× · Predict" : "Frame Gen 2×") : g->settings.frameGen ? "Frame Gen · waiting" : "Frame Gen Off";
    float right = chipWidth(up.c_str(), F().semibold) + 8 + chipWidth(fgLabel, F().regular) + 8 +
                  chipWidth(lat.c_str(), F().monoSemibold, 18) + 12 + 36 * 3 + 8;
    float x = W - 16 - right;
    if (x > ImGui::GetCursorPosX() + 12) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(x);
        centerY(28);
        chip(up.c_str(), kPanel2, kBorder, kText, F().semibold);
        ImGui::SameLine(0, 8);
        centerY(28);
        if (fgOn) chip(fgLabel, kAccentBg, kAccentBorder, kAccentText, F().regular);
        else chip(fgLabel, kPanel2, kBorder, kMuted, F().regular);
        ImGui::SameLine(0, 8);
        centerY(28);
        chip(lat.c_str(), kAccentBg, kAccentBorder, kAccentText, F().monoSemibold, 28, iconBolt);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Average time from the capture timestamp to the frame being ready on the GPU");
        ImGui::SameLine(0, 12);
    } else {
        ImGui::SameLine();
        ImGui::SetCursorPosX(W - 16 - (36 * 3 + 8));
    }
    centerY(36);
    if (iconButton("##stats", iconBars, "Performance (P)")) g->showStats = !g->showStats;
    ImGui::SameLine(0, 4);
    centerY(36);
    if (iconButton("##settings", iconGear, "Settings (S)")) g->showSettings = !g->showSettings;
    ImGui::SameLine(0, 4);
    centerY(36);
    if (iconButton("##fs", g->fullscreen ? +[](ImDrawList* d, ImVec2 c, float s, ImU32 k) { iconExpand(d, c, s, k, true); }
                                         : +[](ImDrawList* d, ImVec2 c, float s, ImU32 k) { iconExpand(d, c, s, k, false); },
                   g->fullscreen ? "Exit fullscreen (F11)" : "Fullscreen (F11)"))
        g->toggleFullscreen();

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ---- Bottom bar ------------------------------------------------------------
void kbd(ImDrawList* dl, ImVec2& p, const char* key, const char* what) {
    float kw = measure(F().mono, 11, key).x + 12;
    dl->AddRect({p.x, p.y + 1}, {p.x + kw, p.y + 19}, col(kBorder), 4);
    text(dl, F().mono, 11, {p.x + 6, p.y + 4}, kIcon, key);
    text(dl, F().regular, 12, {p.x + kw + 6, p.y + 3}, kMuted, what);
    p.x += kw + 6 + measure(F().regular, 12, what).x + 18;
}

void bottomBar(float W, float y) {
    ImGui::SetNextWindowPos({0, y});
    ImGui::SetNextWindowSize({W, kBottomBarH});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colv(kPanel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("##bottom", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoInputs);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    dl->AddLine(wp, {wp.x + W, wp.y}, col(kBorder));
    ImVec2 p{wp.x + 16, wp.y + (kBottomBarH - 20) / 2};

    // Signal
    const char* sig;
    uint32_t sc;
    std::string sigS;
    if (g->lost) { sigS = "Disconnected — waiting for " + g->lostName; sig = sigS.c_str(); sc = kWarn; }
    else if (!g->capture.isOpen()) { sig = "No device"; sc = kDim; }
    else if (g->signalOk()) { sig = "Receiving frames"; sc = kAccent; }
    else { sig = "No signal — check the source"; sc = kWarn; }
    dl->AddCircleFilled({p.x + 4, p.y + 10}, 4, col(sc));
    text(dl, F().regular, 12, {p.x + 14, p.y + 3}, kMuted, sig);
    p.x += 14 + measure(F().regular, 12, sig).x + 22;

    // Audio meter
    iconSpeaker(dl, {p.x + 7, p.y + 10}, 14, col(kMuted));
    p.x += 20;
    float pk = std::max(g->audio.peak(0), g->audio.peak(1));
    int lit = int(std::ceil(pk * 8));
    for (int i = 0; i < 8; i++) {
        uint32_t c = i < lit ? (i >= 6 ? kWarn : kAccent) : kBorder;
        dl->AddRectFilled({p.x + i * 6.f, p.y + 5}, {p.x + i * 6.f + 4, p.y + 15}, col(c));
    }
    p.x += 54;
    std::string a = !g->settings.audioEnabled ? "Audio off" : g->audio.active() ? (g->settings.muted ? "Muted" : fmt("Audio %.1f ms", g->audio.latencyMs())) : "No audio input";
    text(dl, F().mono, 12, {p.x, p.y + 3}, kMuted, a.c_str());
    p.x += measure(F().mono, 12, a.c_str()).x + 22;

    std::string r = fmt("Vulkan · %s", g->renderer.presentModeName());
    text(dl, F().mono, 12, {p.x, p.y + 3}, kMuted, r.c_str());
    p.x += measure(F().mono, 12, r.c_str()).x + 22;

    // Shortcut hints, right-aligned if there's room
    float hintsW = 0;
    const char* keys[][2] = {{"F11", "Fullscreen"}, {"Tab", "Overlay"}, {"S", "Settings"}, {"P", "Stats"}};
    for (auto& k : keys) hintsW += measure(F().mono, 11, k[0]).x + 12 + 6 + measure(F().regular, 12, k[1]).x + 18;
    if (W - 16 - hintsW > p.x - wp.x) {
        ImVec2 q{wp.x + W - hintsW, p.y};
        for (auto& k : keys) kbd(dl, q, k[0], k[1]);
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// ---- Stats overlay (HUD) ---------------------------------------------------
void sparkline(ImDrawList* dl, ImVec2 p, ImVec2 sz, const Stats& s, int samples) {
    int n = std::min(s.count, samples);
    if (n < 2) return;
    float avg = s.latAvg, lo = avg * 0.5f, hi = avg * 1.5f + 0.5f;
    for (int i = 0; i < n; i++) hi = std::max(hi, s.at(s.latency, i));
    float yAvg = p.y + sz.y - (avg - lo) / (hi - lo) * sz.y;
    for (float x = p.x; x < p.x + sz.x; x += 6) dl->AddLine({x, yAvg}, {std::min(x + 3, p.x + sz.x), yAvg}, col(kBorder));
    ImVec2 pts[240];
    for (int i = 0; i < n; i++) {
        float v = s.at(s.latency, n - 1 - i);
        pts[i] = {p.x + sz.x * i / (samples - 1), p.y + sz.y - std::clamp((v - lo) / (hi - lo), 0.f, 1.f) * sz.y};
    }
    dl->AddPolyline(pts, n, col(kAccent), 0, 1.5f);
}

void hud(const Rect& v /* points */) {
    if (!g->settings.showHud || !g->capture.isOpen() || g->noSignalSec() >= 1.5f) return;
    const float w = 272, m = 16;
    int c = g->settings.hudCorner;
    ImVec2 pos{c % 2 ? v.x + v.w - w - m : v.x + m, c >= 2 ? v.y + v.h - m : v.y + m};
    ImGui::SetNextWindowPos(pos, 0, {0, c >= 2 ? 1.f : 0.f});
    ImGui::SetNextWindowSize({w, 0});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colv(kBg, 0.86f));
    ImGui::PushStyleColor(ImGuiCol_Border, colv(kBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 14});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    ImGui::Begin("##hud", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const Stats& s = g->stats;
    float iw = w - 32;

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy({iw, 28});
    text(dl, F().semibold, 11, {p.x, p.y + 10}, kMuted, "LATENCY");
    std::string big = s.count ? fmt("%.1f", s.latAvg) : "--";
    float uw = measure(F().mono, 12, " ms").x, bw = measure(F().monoSemibold, 22, big.c_str()).x;
    text(dl, F().monoSemibold, 22, {p.x + iw - uw - bw, p.y}, kAccentText, big.c_str());
    text(dl, F().mono, 12, {p.x + iw - uw, p.y + 9}, kMuted, " ms");

    ImGui::Dummy({0, 2});
    p = ImGui::GetCursorScreenPos();
    ImGui::Dummy({iw, 36});
    sparkline(dl, p, {iw, 36}, s, 120);
    ImGui::Dummy({0, 4});

    auto& r = g->renderer;
    std::string rows[][2] = {
        {"Capture", fmt("%.1f fps", g->capture.measuredFps.load())},
        {"Shown", s.genFps > 1 ? fmt("%.0f fps (+%.0f gen)", s.shownFps, s.genFps) : fmt("%.1f fps", s.shownFps)},
        {"Input", fmt("%s %s", res(g->capture.width, g->capture.height).c_str(), fourccName(g->capture.fourcc))},
        {"Output", res(r.outW, r.outH)},
        {"Upscaler", r.effectiveUpscaler != Upscaler::Off ? fmt("%s · %.2f×", upscalerName(r.effectiveUpscaler), float(r.outW) / std::max(1u, r.inW)) : std::string("Bilinear")},
        {"GPU time", r.gpu.valid ? fmt("%.2f ms", r.gpu.total) : std::string("--")},
        {"Dropped", fmt("%llu", (unsigned long long)(g->capture.framesSkipped + g->capture.framesLostByDriver))},
        {"Audio", g->audio.active() ? fmt("%.1f ms", g->audio.latencyMs()) : std::string("off")},
    };
    for (auto& row : rows) {
        p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({iw, 18});
        text(dl, F().mono, 12, {p.x, p.y + 2}, kMuted, row[0].c_str());
        float vw = measure(F().mono, 12, row[1].c_str()).x;
        text(dl, F().mono, 12, {p.x + iw - vw, p.y + 2}, kText, row[1].c_str());
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// ---- Shared modal-ish window frame ------------------------------------------
bool beginPanel(const char* id, const char* title, bool* open, ImVec2 size) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    size.x = std::min(size.x, vp->WorkSize.x - 32);
    size.y = std::min(size.y, vp->WorkSize.y - 32);
    // Re-centre when the screen size changes (e.g. entering fullscreen) so panels never end up in a corner.
    static ImVec2 lastVp{0, 0};
    bool vpChanged = lastVp.x != vp->WorkSize.x || lastVp.y != vp->WorkSize.y;
    ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x / 2, vp->WorkPos.y + vp->WorkSize.y / 2}, vpChanged ? ImGuiCond_Always : ImGuiCond_Appearing, {0.5f, 0.5f});
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints({480, 360}, {FLT_MAX, FLT_MAX});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colv(kBg));
    ImGui::PushStyleColor(ImGuiCol_Border, colv(kBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12);
    if (vpChanged && ImGui::GetFrameCount() > 1) lastVp = vp->WorkSize;
    bool vis = ImGui::Begin(id, open, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!vis) { ImGui::End(); return false; }
    // Header strip (also the drag handle)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos();
    float W = ImGui::GetWindowWidth();
    text(dl, F().bold, 17, {wp.x + 24, wp.y + 18}, kText, title);
    ImGui::SetCursorPos({W - 52, 10});
    if (iconButton("##close", iconClose, "Close (Esc)")) *open = false;
    dl->AddLine({wp.x, wp.y + 56}, {wp.x + W, wp.y + 56}, col(kBorder));
    ImGui::SetCursorPos({0, 57});
    return true;
}

// ---- Settings --------------------------------------------------------------
void settingsSource() {
    Settings& st = g->settings;
    sectionTitle("Source", "The capture device and video mode. Uncompressed NV12 at the highest frame rate gives the lowest latency.");
    float w = std::min(460.f, ImGui::GetContentRegionAvail().x);

    label("Capture device");
    const DeviceInfo* cur = g->currentDevice();
    if (combo("##device", cur ? cur->name.c_str() : "None", w)) {
        for (auto& d : g->devices) {
            std::string l = d.name + "  (" + d.path + ")";
            if (ImGui::Selectable(l.c_str(), cur && cur->path == d.path)) g->openDevice(d.path, nullptr);
        }
        endCombo();
    }
    ImGui::Dummy({0, 8});
    label("Video mode");
    std::string prev = st.width ? g->activeMode.label() : "Auto — " + g->activeMode.label();
    if (combo("##mode", prev.c_str(), w)) {
        if (ImGui::Selectable("Auto (best available)", st.width == 0) && cur) {
            st.width = st.height = st.fourcc = st.fpsNum = st.fpsDen = 0;
            g->openDevice(cur->path, nullptr);
        }
        if (cur)
            for (auto& m : cur->modes) {
                bool sel = st.width && m.width == g->activeMode.width && m.height == g->activeMode.height &&
                           m.fourcc == g->activeMode.fourcc && m.fpsNum == g->activeMode.fpsNum && m.fpsDen == g->activeMode.fpsDen;
                if (ImGui::Selectable(m.label().c_str(), sel)) g->openDevice(cur->path, &m);
            }
        endCombo();
    }
    ImGui::Dummy({0, 14});
    label("Colour matrix");
    static const char* mats[] = {"Auto", "BT.709", "BT.601"};
    int mi = int(st.matrix);
    if (segmented("matrix", mats, 3, &mi)) st.matrix = ColorMatrix(mi);
    ImGui::Dummy({0, 8});
    label("Range");
    static const char* rngs[] = {"Auto", "Limited (16–235)", "Full (0–255)"};
    int ri = int(st.range);
    if (segmented("range", rngs, 3, &ri)) st.range = ColorRange(ri);
    textWrapped(F().regular, 12, kDim, "Auto uses BT.709 for HD, BT.601 for SD, and limited range. If blacks look grey or crushed, try the other range.");

    if (g->capture.isOpen()) {
        ImGui::Dummy({0, 14});
        double mbps = g->capture.usbMBps();
        std::string usb = usbSpeedFor(g->capture.path);
        std::string info = fmt("Negotiated %s @ %.2f %s · %.0f MB/s%s%s", res(g->capture.width, g->capture.height).c_str(), g->capture.fps,
                               fourccName(g->capture.fourcc), mbps, usb.empty() ? "" : " · USB ", usb.c_str());
        textColored(F().mono, 12, kMuted, info.c_str());
    }
}

void settingsUpscaling() {
    Settings& st = g->settings;
    sectionTitle("Upscaling", "Upscales the capture to fit your window on the GPU. FSR 1, NIS and Anime4K are spatial (this frame only, no ghosting); FSR 3 and 4 are temporal.");
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = avail > 700 ? 3 : 2;
    float cw = (avail - 10 * (cols - 1)) / cols, ch = 128;
    struct Opt { Upscaler u; CardText t; bool enabled; const char* tip; };
    Opt opts[] = {
        {Upscaler::Off, {"Native", nullptr, "No upscaling. The Sharpness slider applies AMD CAS at native resolution (0 = untouched).", "+0.0–0.1 ms"}, true, nullptr},
        {Upscaler::FSR1, {"FSR 1", "RECOMMENDED", "Spatial edge-aware upscale plus sharpening. No ghosting.", "~0.1–0.4 ms GPU"}, true, nullptr},
        {Upscaler::NIS, {"NIS", nullptr, "NVIDIA Image Scaling: 6-tap upscale with adaptive sharpening. Spatial, crisp.", "~0.2–0.5 ms GPU"}, g->renderer.nisAvailable(), nullptr},
        {Upscaler::Anime4K, {"Anime4K", nullptr, "CNN upscaler (2×) made for anime and cel-shaded games. Can over-smooth realistic games.", "~0.3–2 ms GPU"},
         g->renderer.anime4kAvailable(), "Anime4K by bloc97 (MIT). Doubles the capture resolution with a small neural network, then resizes to the window."},
        {Upscaler::FSR3, {"FSR 3", "EXPERIMENTAL", "Temporal FSR 3.1 using optical flow. Can calm shimmer in motion; similar detail to FSR 1.", "~1–2 ms GPU"}, g->renderer.temporalAvailable(),
         g->renderer.temporalAvailable()
             ? "FSR 3.1 normally gets motion vectors and camera jitter from the game. Here motion is estimated from the video and there's no jitter, so expect steadier edges and less shimmer rather than lots of new detail. Fast or transparent motion can ghost."
             : "FidelityFX failed to initialise on this GPU, so FSR 3 is unavailable."},
        {Upscaler::FSR4, {"FSR 4", "EXPERIMENTAL", "Best quality: ML upscaler via fsr4-vk (FP8 on RDNA 4). Clearly cleaner foliage than FSR 1.", "~2–4 ms GPU"},
         g->renderer.fsr4Available(),
         g->renderer.fsr4Available()
             ? "FSR 4.1 through fsr4-vk, an independent Vulkan implementation using AMD's FSR 4 model (GPL-3.0). Like FSR 3 it gets estimated motion and synthetic jitter, so gains over FSR 1 may be modest."
             : nullptr},
    };
    const int nOpts = int(sizeof opts / sizeof opts[0]);
    for (int i = 0; i < nOpts; i++) {
        if (i % cols) ImGui::SameLine(0, 10);
        if (card(fmt("##up%d", i).c_str(), opts[i].t, cw, ch, st.upscaler == opts[i].u, opts[i].enabled, opts[i].tip)) st.upscaler = opts[i].u;
    }
    ImGui::Dummy({0, 14});

    ImGui::BeginDisabled(st.upscaler == Upscaler::Anime4K);
    label(st.upscaler == Upscaler::Off ? "Sharpness (AMD CAS)" : st.upscaler == Upscaler::NIS ? "Sharpness (NIS)" : "Sharpness");
    ImGui::SetNextItemWidth(std::min(360.f, avail));
    ImGui::PushFont(F().mono, 12);
    ImGui::SliderFloat("##sharp", &st.sharpness, 0.0f, 1.0f, "%.2f");
    ImGui::PopFont();
    ImGui::EndDisabled();

    if (st.upscaler == Upscaler::Anime4K) {
        ImGui::Dummy({0, 10});
        label("Anime4K network size");
        static const char* vs[] = {"S (fastest)", "M (balanced)", "L (best)"};
        segmented("a4kv", vs, 3, &st.anime4kVariant);
    }
    if (st.upscaler == Upscaler::NIS || st.upscaler == Upscaler::Anime4K) {
        ImGui::Dummy({0, 6});
        ImGui::PushFont(F().regular, 13);
        ImGui::Checkbox(fmt("Split view: FSR 1 left, %s right (C)", upscalerName(st.upscaler)).c_str(), &g->compareView);
        ImGui::PopFont();
    }
    auto& r = g->renderer;
    bool temporalSel = st.upscaler == Upscaler::FSR3 || st.upscaler == Upscaler::FSR4;
    if (temporalSel) {
        ImGui::Dummy({0, 10});
        label("Quality (FSR output size)");
        static const char* qs[] = {"Auto", "Native AA", "Quality", "Balanced", "Performance", "Ultra Perf."};
        segmented("fsrq", qs, 6, &st.fsrQuality);
        static const char* qhelp[] = {
            "Upscale straight to the window size.",
            "1.0×: FSR as anti-aliasing and clean-up at capture resolution, then resized. Cheapest.",
            "1.5× the capture resolution, then resized to the window.",
            "1.7× the capture resolution, then resized to the window.",
            "2.0× the capture resolution (1080p to 4K), then resized to the window.",
            "3.0× the capture resolution. Most GPU work; only worth it on very large outputs."};
        textWrapped(F().regular, 12, kDim, qhelp[std::clamp(st.fsrQuality, 0, 5)]);
    }
    if (r.inW) {
        std::string s;
        uint32_t dw = uint32_t(r.lastDst.w), dh = uint32_t(r.lastDst.h);
        if (r.effectiveUpscaler == Upscaler::Off && st.upscaler != Upscaler::Off)
            s = fmt("The %s window isn't larger than the %s input, so upscaling is bypassed.", res(dw, dh).c_str(), res(r.inW, r.inH).c_str());
        else if (r.effectiveUpscaler != Upscaler::Off && (r.outW != dw || r.outH != dh))
            s = fmt("%s renders %s to %s (%.2f×), resized to %s on screen", upscalerName(r.effectiveUpscaler), res(r.inW, r.inH).c_str(),
                    res(r.outW, r.outH).c_str(), float(r.outW) / r.inW, res(dw, dh).c_str());
        else
            s = fmt("Scale %s to %s (%.2f×)", res(r.inW, r.inH).c_str(), res(r.outW, r.outH).c_str(), float(r.outW) / std::max(1u, r.inW));
        ImGui::Dummy({0, 4});
        textColored(F().mono, 12, kMuted, s.c_str());
    }
    if (!g->renderer.fsr4Available() && st.upscaler != Upscaler::FSR4) {
        ImGui::Dummy({0, 4});
        textWrapped(F().regular, 12, kDim, ("FSR 4: " + g->renderer.fsr4Status() + ". Build fsr4-vk and put amd_fidelityfx_upscaler_vk.so next to the app or in ~/.local/share/lyvra/.").c_str());
    }
    if (st.upscaler == Upscaler::FSR3 || st.upscaler == Upscaler::FSR4) {
        ImGui::Dummy({0, 6});
        if (!r.temporalStatus.empty())
            textWrapped(F().regular, 12, kWarn, (std::string(upscalerName(st.upscaler)) + " fell back to FSR 1: " + r.temporalStatus).c_str());
        ImGui::PushFont(F().regular, 13);
        ImGui::Checkbox("Show motion vectors (debug)", &g->debugMotion);
        ImGui::Checkbox(st.upscaler == Upscaler::FSR4 ? "Split view: FSR 1 left, FSR 4 right (C)" : "Split view: FSR 1 left, FSR 3 right (C)", &g->compareView);
        ImGui::PopFont();
        textWrapped(F().regular, 12, kDim, "Colour shows direction, saturation shows speed, dark means static. A black and white checker marks vectors that failed the check against the previous frame; FSR uses the current frame there instead of history.");
    }

    ImGui::Dummy({0, 18});
    ImGui::PushStyleColor(ImGuiCol_Separator, colv(kBorder));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Dummy({0, 18});

    sectionTitle("Frame Generation", "Inserts generated frames between captured ones for smoother motion on high-refresh displays.");
    bool fgAvail = g->renderer.frameGenAvailable();
    ImGui::BeginDisabled(!fgAvail);
    ImGui::PushFont(F().semibold, 13);
    ImGui::Checkbox("Enable frame generation (G)", &st.frameGen);
    ImGui::PopFont();
    ImGui::EndDisabled();
    double fps = g->capture.isOpen() ? g->capture.fps : 60.0;
    textColored(F().mono, 12, kMuted, fmt("%s · %.0f fps in, %.0f fps out (2×)", st.fgPredict ? "Motion prediction" : "FidelityFX frame interpolation",
                                          fps, fps * 2).c_str());
    ImGui::Dummy({0, 6});
    ImGui::BeginDisabled(!fgAvail);
    ImGui::PushFont(F().semibold, 13);
    ImGui::Checkbox("Low-latency mode (predict)", &st.fgPredict);
    ImGui::PopFont();
    ImGui::EndDisabled();
    textWrapped(F().regular, 12, kDim, "Shows each real frame the moment it arrives and predicts the next in-between frame from the motion so far, "
                                       "instead of waiting for the next real frame. No added latency, but edges of moving objects can smear "
                                       "and sudden stops or turns can overshoot for one frame.");
    ImGui::Dummy({0, 8});
    textColored(F().semibold, 13, kText, "Flow scale");
    textWrapped(F().regular, 12, kDim, "Motion is tracked on a smaller copy of the frame. Lower is faster; small or thin moving things "
                                       "(crosshairs, text, particles) may wobble more. Generated frames stay full resolution.");
    {
        const char* fs[] = {"Auto", "100%", "75%", "50%", "25%"};
        const int vals[] = {0, 100, 75, 50, 25};
        int fi = 0;
        for (int k = 0; k < 5; k++) if (st.fgFlowScale == vals[k]) fi = k;
        ImGui::BeginDisabled(!fgAvail);
        if (segmented("flowscale", fs, 5, &fi)) st.fgFlowScale = vals[fi];
        ImGui::EndDisabled();
        if (st.frameGen && g->renderer.frameGenFlowW() > 0)
            textColored(F().mono, 12, kMuted, fmt("Motion tracked at %u×%u", g->renderer.frameGenFlowW(), g->renderer.frameGenFlowH()).c_str());
    }
    ImGui::Dummy({0, 8});
    if (!fgAvail) textWrapped(F().regular, 12, kWarn, ("Unavailable: " + g->renderer.frameGenStatus).c_str());
    else if (st.frameGen && !g->renderer.frameGenStatus.empty()) textWrapped(F().regular, 12, kWarn, g->renderer.frameGenStatus.c_str());
    if (g->displayHz > 0 && g->displayHz + 1 < fps * 2)
        textWrapped(F().regular, 12, kWarn, fmt("This display refreshes at %.0f Hz, below %.0f fps, so some generated frames won't be seen. "
                                                "Try a lower capture frame rate or a faster display.", g->displayHz, fps * 2).c_str());
    if (g->compareView || g->debugMotion) textWrapped(F().regular, 12, kDim, "Paused while split view or the motion debug view is on.");
    if (st.fgPredict)
        textWrapped(F().regular, 12, kDim, "Unchanged pixels such as HUD text are kept still so they don't warp.");
    else
        textWrapped(F().regular, 12, kDim, "During very fast motion, where a generated frame would smear, the real frame is shown instead. "
                                           "Unchanged pixels such as HUD text are kept from the real frame so they don't warp.");
    if (st.frameGen && g->renderer.frameGenSkipped() > 0)
        textColored(F().mono, 12, kMuted, fmt("%llu generated frames skipped for fast motion so far", (unsigned long long)g->renderer.frameGenSkipped()).c_str());
    ImGui::Dummy({0, 10});
    if (st.fgPredict) {
        note(kWarnBg, kWarnBorder, kWarnText, kWarnBody, "No added latency",
             "Real frames are shown as soon as they're captured; motion tracking runs after they're on screen. "
             "Predicted frames are guesses, so expect more artifacts than the default mode.");
    } else {
        std::string body = fmt("Each generated frame sits between two real frames, so the real frame is held back by half a frame "
                               "(%.1f ms at %.0f fps) plus the time to generate it (about 2 ms). Turn on low-latency mode, or leave "
                               "frame generation off, for competitive play.",
                               500.0 / std::max(1.0, fps), fps);
        note(kWarnBg, kWarnBorder, kWarnText, kWarnBody, "Adds about half a frame of latency", body.c_str());
    }
}

void settingsDisplay() {
    Settings& st = g->settings;
    sectionTitle("Display", "How frames reach your screen.");
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = avail > 700 ? 3 : 1;
    float cw = cols == 3 ? (avail - 20) / 3 : avail, ch = 112;
    struct Opt { PresentMode m; CardText t; };
    Opt opts[] = {
        {PresentMode::Mailbox, {"Mailbox", nullptr, "Low latency without tearing. Recommended.", "always newest frame"}},
        {PresentMode::Immediate, {"Immediate", nullptr, "Lowest latency. May tear.", "no wait for vblank"}},
        {PresentMode::Fifo, {"VSync", nullptr, "Smoothest pacing. Can add a frame or two.", "waits for vblank"}},
    };
    for (int i = 0; i < 3; i++) {
        if (i && cols == 3) ImGui::SameLine(0, 10);
        if (card(fmt("##pm%d", i).c_str(), opts[i].t, cw, ch, st.present == opts[i].m, true)) st.present = opts[i].m;
    }
    ImGui::Dummy({0, 6});
    textColored(F().mono, 12, kDim, fmt("Active: %s", g->renderer.presentModeName()).c_str());
    ImGui::Dummy({0, 14});
    label("Scaling");
    static const char* sc[] = {"Fit", "Integer", "Stretch"};
    int si = int(st.scale);
    if (segmented("scale", sc, 3, &si)) st.scale = ScaleMode(si);
    ImGui::Dummy({0, 14});
    ImGui::PushFont(F().regular, 13);
    ImGui::Checkbox("Start in fullscreen", &st.startFullscreen);
    ImGui::Checkbox("Keep the top and bottom bars visible in fullscreen", &st.barsInFullscreen);
    ImGui::PopFont();
}

void settingsAudio() {
    Settings& st = g->settings;
    sectionTitle("Audio", "Plays the capture card's audio through your speakers with a small, bounded buffer.");
    float w = std::min(460.f, ImGui::GetContentRegionAvail().x);
    bool restart = false;
    ImGui::PushFont(F().semibold, 13);
    restart |= ImGui::Checkbox("Play capture card audio", &st.audioEnabled);
    ImGui::PopFont();
    ImGui::Dummy({0, 10});
    ImGui::BeginDisabled(!st.audioEnabled);
    label("Input");
    if (combo("##ain", st.audioInput.empty() ? "Auto (match capture device)" : st.audioInput.c_str(), w)) {
        if (ImGui::Selectable("Auto (match capture device)", st.audioInput.empty())) { st.audioInput.clear(); restart = true; }
        for (auto& d : g->audioInputs)
            if (ImGui::Selectable(d.name.c_str(), st.audioInput == d.name)) { st.audioInput = d.name; restart = true; }
        endCombo();
    }
    ImGui::Dummy({0, 8});
    label("Output");
    if (combo("##aout", st.audioOutput.empty() ? "System default" : st.audioOutput.c_str(), w)) {
        if (ImGui::Selectable("System default", st.audioOutput.empty())) { st.audioOutput.clear(); restart = true; }
        for (auto& d : g->audioOutputs)
            if (ImGui::Selectable(d.name.c_str(), st.audioOutput == d.name)) { st.audioOutput = d.name; restart = true; }
        endCombo();
    }
    ImGui::Dummy({0, 8});
    label("Volume");
    ImGui::SetNextItemWidth(w);
    float pct = st.volume * 100;
    ImGui::PushFont(F().mono, 12);
    if (ImGui::SliderFloat("##vol", &pct, 0, 150, "%.0f%%")) st.volume = pct / 100;
    ImGui::PopFont();
    ImGui::PushFont(F().regular, 13);
    ImGui::Checkbox("Mute (M)", &st.muted);
    ImGui::PopFont();
    ImGui::Dummy({0, 8});
    label("Maximum buffer");
    ImGui::SetNextItemWidth(w);
    ImGui::PushFont(F().mono, 12);
    ImGui::SliderInt("##abuf", &st.audioMaxMs, 10, 100, "%d ms");
    ImGui::PopFont();
    textWrapped(F().regular, 12, kDim, "Lower keeps audio tighter with the video. Raise it if you hear crackles.");
    ImGui::EndDisabled();
    ImGui::Dummy({0, 12});
    if (g->audio.active())
        textColored(F().mono, 12, kMuted, fmt("Live · %.1f ms · %s to %s", g->audio.latencyMs(), g->audio.inputName.c_str(), g->audio.outputName.c_str()).c_str());
    else if (st.audioEnabled && !g->audioError.empty())
        textColored(F().regular, 12, kWarn, g->audioError.c_str());
    if (restart) g->restartAudio();
}

void settingsOverlay() {
    Settings& st = g->settings;
    sectionTitle("Stats overlay", "A compact panel over the video with latency, frame rates and resolutions.");
    ImGui::PushFont(F().semibold, 13);
    ImGui::Checkbox("Show overlay (Tab)", &st.showHud);
    ImGui::PopFont();
    ImGui::Dummy({0, 10});
    label("Position");
    static const char* corners[] = {"Top left", "Top right", "Bottom left", "Bottom right"};
    segmented("corner", corners, 4, &st.hudCorner);
}

void settingsShortcuts() {
    sectionTitle("Keyboard shortcuts");
    const char* rows[][2] = {{"F11 / F", "Toggle fullscreen"}, {"Tab", "Show or hide the stats overlay"}, {"S", "Settings"},
                             {"P", "Performance"}, {"D", "Choose capture device"}, {"M", "Mute audio"},
                             {"U", "Cycle upscaler (Native / FSR 1 / NIS / Anime4K / FSR 3 / FSR 4)"}, {"F12", "Save a screenshot to Pictures/Lyvra"}, {"C", "Split view: FSR 1 | FSR 3"}, {"G", "Frame generation on/off"}, {"Esc", "Close panel / leave fullscreen"}, {"Ctrl+Q", "Quit"}};
    if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 140);
        for (auto& r : rows) {
            ImGui::TableNextRow(0, 30);
            ImGui::TableNextColumn();
            textColored(F().monoSemibold, 13, kText, r[0]);
            ImGui::TableNextColumn();
            textColored(F().regular, 13, kMuted, r[1]);
        }
        ImGui::EndTable();
    }
}

void settingsWindow() {
    if (!g->showSettings) return;
    bool open = true;
    if (beginPanel("Settings##win", "Settings", &open, {980, 700})) {
        float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
        const float navW = 220, footH = 64;
        // Left nav
        ImGui::PushStyleColor(ImGuiCol_ChildBg, colv(kPanel));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12, 14});
        ImGui::BeginChild("nav", {navW, H - 57 - footH}, ImGuiChildFlags_AlwaysUseWindowPadding);
        const char* tabs[] = {"Source", "Upscaling & Frame Gen", "Display", "Audio", "Stats overlay", "Shortcuts"};
        for (int i = 0; i < 6; i++) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = navW - 24;
            if (ImGui::InvisibleButton(tabs[i], {w, 40})) g->settingsTab = i;
            bool sel = g->settingsTab == i, hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (sel) dl->AddRectFilled(p, {p.x + w, p.y + 40}, col(0x1F2A2A), 8);
            else if (hov) dl->AddRectFilled(p, {p.x + w, p.y + 40}, col(kPanel2), 8);
            text(dl, sel ? F().semibold : F().regular, 13, {p.x + 14, p.y + 12}, sel ? kAccent : kIcon, tabs[i]);
            ImGui::Dummy({0, 2});
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // Content
        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {36, 28});
        ImGui::BeginChild("content", {W - navW, H - 57 - footH}, ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10, 8});
        switch (g->settingsTab) {
            case 0: settingsSource(); break;
            case 1: settingsUpscaling(); break;
            case 2: settingsDisplay(); break;
            case 3: settingsAudio(); break;
            case 4: settingsOverlay(); break;
            default: settingsShortcuts(); break;
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // Footer
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos();
        dl->AddRectFilled({wp.x, wp.y + H - footH}, {wp.x + W, wp.y + H}, col(kPanel), 12, ImDrawFlags_RoundCornersBottom);
        dl->AddLine({wp.x, wp.y + H - footH}, {wp.x + W, wp.y + H - footH}, col(kBorder));
        ImGui::SetCursorPos({W - 24 - 90 - 10 - 150, H - footH + 12});
        if (secondaryButton("Reset to defaults", {150, 40})) {
            std::string dev = g->settings.devicePath;
            g->settings = Settings{};
            g->settings.devicePath = dev;
            g->restartAudio();
        }
        ImGui::SameLine(0, 10);
        if (primaryButton("Done", {90, 40})) open = false;
        ImGui::End();
    }
    if (!open) { g->showSettings = false; g->settings.save(); }
}

// ---- Performance window -------------------------------------------------------
void tile(const char* title, const std::string& value, const char* unit, const std::string& sub, uint32_t valueCol, float w) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = 104;
    ImGui::Dummy({w, h});
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + w, p.y + h}, col(kPanel), 12);
    dl->AddRect(p, {p.x + w, p.y + h}, col(kBorder), 12);
    text(dl, F().semibold, 11, {p.x + 16, p.y + 16}, kMuted, title);
    text(dl, F().monoSemibold, 28, {p.x + 16, p.y + 34}, valueCol, value.c_str());
    if (unit) text(dl, F().mono, 14, {p.x + 18 + measure(F().monoSemibold, 28, value.c_str()).x, p.y + 47}, kMuted, unit);
    text(dl, F().regular, 12, {p.x + 16, p.y + 76}, kMuted, sub.c_str());
}

void statsWindow() {
    if (!g->showStats) return;
    bool open = true;
    if (beginPanel("Performance##win", "Performance", &open, {1080, 720})) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {28, 22});
        ImGui::BeginChild("perf", {0, 0}, ImGuiChildFlags_AlwaysUseWindowPadding);
        const Stats& s = g->stats;
        auto& r = g->renderer;
        float avail = ImGui::GetContentRegionAvail().x;

        if (secondaryButton("Reset counters", {140, 34})) {
            g->stats.reset();
            g->capture.framesSkipped = 0;
            g->capture.framesLostByDriver = 0;
        }
        ImGui::Dummy({0, 10});

        // KPI tiles
        int cols = avail > 900 ? 5 : avail > 560 ? 3 : 2;
        float tw = (avail - 12 * (cols - 1)) / cols;
        int k = 0;
        auto next = [&]() { if (k++ % cols) ImGui::SameLine(0, 12); else if (k > 1) ImGui::Dummy({0, 4}); };
        next();
        tile("LATENCY", s.count ? fmt("%.1f", s.latAvg) : "--", " ms", s.count ? fmt("p99 %.1f ms · min %.1f ms", s.latP99, s.latMin) : "waiting for frames", kAccentText, tw);
        next();
        tile("CAPTURE FPS", fmt("%.1f", g->capture.measuredFps.load()), nullptr, fmt("target %.0f · jitter ±%.2f ms", g->capture.fps, g->capture.intervalJitterMs.load()), kText, tw);
        next();
        tile("SHOWN FPS", fmt("%.1f", s.shownFps + s.genFps), nullptr,
             s.genFps > 1 ? fmt("%.0f real + %.0f generated · %llu skipped", s.shownFps, s.genFps, (unsigned long long)r.frameGenSkipped())
                          : std::string("0 generated (frame gen off)"), kText, tw);
        next();
        tile("GPU TIME / FRAME", r.gpu.valid ? fmt("%.2f", r.gpu.total) : "--", " ms",
             g->capture.fps > 0 && r.gpu.valid ? fmt("%s · %.0f%% of frame budget", r.effectiveUpscaler != Upscaler::Off ? upscalerName(r.effectiveUpscaler) : "Bilinear", r.gpu.total / (1000.0 / g->capture.fps) * 100) : "", kText, tw);
        next();
        tile("DROPPED FRAMES", fmt("%llu", (unsigned long long)(g->capture.framesSkipped + g->capture.framesLostByDriver)), nullptr,
             fmt("%llu replaced by newer · %llu lost in driver", (unsigned long long)g->capture.framesSkipped.load(), (unsigned long long)g->capture.framesLostByDriver.load()), kText, tw);
        ImGui::Dummy({0, 16});

        // Latency breakdown
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float bh = 150;
        ImGui::Dummy({avail, bh});
        dl->AddRectFilled(p, {p.x + avail, p.y + bh}, col(kPanel), 12);
        dl->AddRect(p, {p.x + avail, p.y + bh}, col(kBorder), 12);
        text(dl, F().semibold, 15, {p.x + 20, p.y + 18}, kText, "Latency breakdown");
        text(dl, F().regular, 12, {p.x + 20 + measure(F().semibold, 15, "Latency breakdown").x + 10, p.y + 21}, kMuted,
             "From the capture timestamp to the frame being ready on the GPU. Compositor and display scanout come on top.");
        struct Seg { const char* name; float ms; uint32_t c; };
        std::vector<Seg> segs = {{"USB transfer", s.transfer, 0x2E7D76}, {"Queue / FG hold", s.wait, 0x4A515C}, {"CPU + acquire", s.cpu, kAccent},
                                 {"GPU convert", s.gpuConvert, 0x3E9BD6}, {"GPU upscale + FG", s.gpuUpscale, 0xA8EDE6}, {"GPU composite", s.gpuComposite, kBlue}};
        if (g->capture.compressed()) segs.insert(segs.begin() + 1, {"MJPEG decode", s.decode, 0x8A6FD1});
        float total = 0;
        for (auto& sg : segs) total += std::max(0.f, sg.ms);
        float bx = p.x + 20, bw = avail - 40, by = p.y + 52;
        if (total > 0) {
            float x = bx;
            for (auto& sg : segs) {
                float w = std::max(0.f, sg.ms) / total * bw;
                if (w >= 1) dl->AddRectFilled({x, by}, {x + w - 2, by + 28}, col(sg.c), 4);
                x += w;
            }
        } else {
            dl->AddRectFilled({bx, by}, {bx + bw, by + 28}, col(kPanel2), 4);
        }
        int lcols = (int(segs.size()) + 1) / 2;
        float lw = bw / lcols;
        for (int i = 0; i < int(segs.size()); i++) {
            float lx = bx + (i % lcols) * lw, ly = by + 44 + (i / lcols) * 24;
            dl->AddRectFilled({lx, ly + 3}, {lx + 10, ly + 13}, col(segs[i].c), 2);
            text(dl, F().regular, 12, {lx + 18, ly}, kMuted, segs[i].name);
            std::string v = fmt("%.2f ms", segs[i].ms);
            text(dl, F().mono, 12, {lx + lw - 24 - measure(F().mono, 12, v.c_str()).x, ly}, kText, v.c_str());
        }
        ImGui::Dummy({0, 16});

        // Frame time graph + signal path
        float gw = avail > 900 ? avail * 0.6f - 8 : avail, gh = 250;
        p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({gw, gh});
        dl->AddRectFilled(p, {p.x + gw, p.y + gh}, col(kPanel), 12);
        dl->AddRect(p, {p.x + gw, p.y + gh}, col(kBorder), 12);
        text(dl, F().semibold, 15, {p.x + 20, p.y + 18}, kText, "Frame time");
        text(dl, F().regular, 12, {p.x + 20 + measure(F().semibold, 15, "Frame time").x + 10, p.y + 21}, kMuted, "interval between frames shown");
        {
            float target = g->capture.fps > 0 ? float(1000.0 / g->capture.fps) : 16.67f;
            float lo = 0, hi = target * 2;
            ImVec2 a{p.x + 48, p.y + 56}, b{p.x + gw - 20, p.y + gh - 24};
            for (int i = 0; i <= 2; i++) {
                float v = hi - i * (hi - lo) / 2, y = a.y + (b.y - a.y) * i / 2;
                dl->AddLine({a.x, y}, {b.x, y}, col(0x20242B));
                std::string l = fmt("%.0f", v);
                text(dl, F().mono, 11, {a.x - 10 - measure(F().mono, 11, l.c_str()).x, y - 7}, kDim, l.c_str());
            }
            float ty = b.y - (target - lo) / (hi - lo) * (b.y - a.y);
            for (float x = a.x; x < b.x; x += 9) dl->AddLine({x, ty}, {std::min(x + 5, b.x), ty}, col(kDim));
            int n = std::min(s.count, Stats::N);
            if (n > 1) {
                ImVec2 pts[Stats::N];
                for (int i = 0; i < n; i++) {
                    float v = std::clamp(s.at(s.frameMs, n - 1 - i), lo, hi);
                    pts[i] = {a.x + (b.x - a.x) * (Stats::N - n + i) / (Stats::N - 1), b.y - (v - lo) / (hi - lo) * (b.y - a.y)};
                }
                dl->AddPolyline(pts, n, col(kAccent), 0, 1.8f);
            }
        }
        if (avail > 900) ImGui::SameLine(0, 16);
        else ImGui::Dummy({0, 12});
        float iw = avail > 900 ? avail - gw - 16 : avail;
        p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({iw, gh});
        dl->AddRectFilled(p, {p.x + iw, p.y + gh}, col(kPanel), 12);
        dl->AddRect(p, {p.x + iw, p.y + gh}, col(kBorder), 12);
        text(dl, F().semibold, 15, {p.x + 20, p.y + 18}, kText, "Signal path");
        auto& c = g->capture;
        std::string usb = usbSpeedFor(c.path);
        double mbs = c.usbMBps();
        std::string rows[][2] = {
            {"Device", c.isOpen() ? c.name + " · " + c.path : g->lost ? g->lostName + " · disconnected" : "none"},
            {"Signal", !c.isOpen() ? std::string("--") : g->signalOk() ? fmt("receiving%s", g->streamRestarts ? fmt(" · %llu stream restart%s", (unsigned long long)g->streamRestarts, g->streamRestarts == 1 ? "" : "s").c_str() : "")
                                                    : fmt("none for %.0f s · %llu stream restart%s", g->noSignalSec(), (unsigned long long)g->streamRestarts, g->streamRestarts == 1 ? "" : "s")},
            {"Input", c.isOpen() ? fmt("%s @ %.0f · %s", res(c.width, c.height).c_str(), c.fps, fourccName(c.fourcc)) : "--"},
            {"Colour", fmt("%s %s", (g->settings.matrix == ColorMatrix::BT601 || (g->settings.matrix == ColorMatrix::Auto && c.height < 720)) ? "BT.601" : "BT.709",
                           g->settings.range == ColorRange::Full ? "full" : "limited")},
            {"USB link", fmt("%s%s%.0f MB/s", usb.c_str(), usb.empty() ? "" : " · ", mbs)},
            {"Output", fmt("%s · window %s", res(r.outW, r.outH).c_str(), res(r.swapW, r.swapH).c_str())},
            {"Upscaler", r.effectiveUpscaler != Upscaler::Off ? fmt("%s · %.2f× · sharpness %.2f", upscalerName(r.effectiveUpscaler), float(r.outW) / std::max(1u, r.inW), g->settings.sharpness) : std::string("Bilinear")},
            {"Renderer", fmt("Vulkan · %s", r.presentModeName())},
            {"GPU", r.gpuName},
            {"Audio", g->audio.active() ? fmt("48 kHz S16 · %.1f ms", g->audio.latencyMs()) : std::string("off")},
        };
        float ry = p.y + 54;
        for (auto& row : rows) {
            text(dl, F().regular, 12, {p.x + 20, ry}, kMuted, row[0].c_str());
            float vw = measure(F().mono, 12, row[1].c_str()).x;
            text(dl, F().mono, 12, {std::max(p.x + 100, p.x + iw - 20 - vw), ry}, kText, row[1].c_str());
            ry += 20;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::End();
    }
    if (!open) g->showStats = false;
}

// ---- Device picker -------------------------------------------------------------
void deviceCard(int i, const DeviceInfo& d, float w) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    std::string audioMatch;
    std::string token = d.name.substr(0, d.name.find(' '));
    for (auto& a : g->audioInputs) {
        std::string an = a.name, tk = token;
        for (auto& ch : an) ch = char(tolower(ch));
        for (auto& ch : tk) ch = char(tolower(ch));
        if (!tk.empty() && an.find(tk) != std::string::npos) { audioMatch = a.name; break; }
    }
    float h = audioMatch.empty() ? 76 : 98;
    if (ImGui::InvisibleButton(fmt("##devcard%d", i).c_str(), {w, h})) {
        if (g->pickerSelection != i) g->pickerMode = 0;
        g->pickerSelection = i;
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) g->pickerSelection = i;
    bool sel = g->pickerSelection == i, hov = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, {p.x + w, p.y + h}, col(sel ? kAccentSel : kPanel), 12);
    dl->AddRect(p, {p.x + w, p.y + h}, col(sel ? kAccent : hov ? 0x3A404A : kBorder), 12, 0, sel ? 1.5f : 1);
    ImVec2 rc{p.x + 26, p.y + 26};
    dl->AddCircle(rc, 8, col(sel ? kAccent : kDim), 16, 1.5f);
    if (sel) dl->AddCircleFilled(rc, 4, col(kAccent));
    float x = p.x + 46;
    text(dl, F().semibold, 15, {x, p.y + 16}, kText, d.name.c_str());
    float nx = x + measure(F().semibold, 15, d.name.c_str()).x + 10;
    const char* badge = d.isCaptureCard ? "Capture card" : "Camera";
    float bw = measure(F().semibold, 11, badge).x + 16;
    dl->AddRectFilled({nx, p.y + 16}, {nx + bw, p.y + 36}, col(d.isCaptureCard ? kAccentBg : kPanel2), 4);
    text(dl, F().semibold, 11, {nx + 8, p.y + 19}, d.isCaptureCard ? kAccentText : kMuted, badge);
    float pw = measure(F().mono, 12, d.path.c_str()).x;
    text(dl, F().mono, 12, {p.x + w - 18 - pw, p.y + 19}, kDim, d.path.c_str());
    text(dl, F().mono, 12, {x, p.y + 46}, kMuted, d.summary().c_str());
    if (!audioMatch.empty()) {
        iconSpeaker(dl, {x + 6, p.y + 77}, 12, col(kMuted));
        text(dl, F().regular, 12, {x + 18, p.y + 70}, kMuted, ("Paired audio: " + audioMatch).c_str());
    }
}

void picker() {
    if (!g->showPicker) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colv(kVideoBg, g->capture.isOpen() ? 0.92f : 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 32});
    ImGui::Begin("##picker", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    float w = std::min(660.f, vp->Size.x - 32);
    float estH = 260 + g->devices.size() * 110 + 120;
    ImGui::SetCursorPos({(vp->Size.x - w) / 2, std::max(32.f, (vp->Size.y - estH) / 2)});
    ImGui::BeginGroup();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy({w, 60});
    ImVec2 ic{p.x + w / 2, p.y + 28};
    dl->AddRectFilled({ic.x - 28, ic.y - 28}, {ic.x + 28, ic.y + 28}, col(kPanel), 14);
    dl->AddRect({ic.x - 28, ic.y - 28}, {ic.x + 28, ic.y + 28}, col(kBorder), 14);
    iconLogo(dl, ic, 34, kAccent);
    ImGui::Dummy({0, 8});
    auto centered = [&](ImFont* f, float size, uint32_t c, const char* s) {
        float tw = std::min(w, measure(f, size, s).x);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - tw) / 2);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + tw);
        textColored(f, size, c, s);
        ImGui::PopTextWrapPos();
    };
    centered(F().bold, 24, kText, "Choose a capture device");
    ImGui::Dummy({0, 4});
    centered(F().regular, 13, kMuted, "Capture cards are listed first. Your choice is remembered next time.");
    ImGui::Dummy({0, 20});

    if (g->devices.empty()) {
        p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({w, 90});
        dl->AddRectFilled(p, {p.x + w, p.y + 90}, col(kPanel), 12);
        dl->AddRect(p, {p.x + w, p.y + 90}, col(kBorder), 12);
        text(dl, F().semibold, 15, {p.x + 24, p.y + 24}, kText, "No video capture devices found");
        text(dl, F().regular, 13, {p.x + 24, p.y + 50}, kMuted, "Plug in your capture card: it appears here by itself. You need to be in the \"video\" group.");
    } else {
        g->pickerSelection = std::clamp(g->pickerSelection, 0, int(g->devices.size()) - 1);
        for (int i = 0; i < int(g->devices.size()); i++) {
            deviceCard(i, g->devices[i], w);
            ImGui::Dummy({0, 4});
        }
        ImGui::Dummy({0, 10});
        const DeviceInfo& d = g->devices[g->pickerSelection];
        float half = (w - 16) / 2;
        ImGui::BeginGroup();
        label("Video mode");
        std::string autoLabel = "Auto — " + pickBestMode(d.modes).label();
        g->pickerMode = std::clamp(g->pickerMode, 0, int(d.modes.size()));
        if (combo("##pmode", g->pickerMode == 0 ? autoLabel.c_str() : d.modes[g->pickerMode - 1].label().c_str(), half)) {
            if (ImGui::Selectable(autoLabel.c_str(), g->pickerMode == 0)) g->pickerMode = 0;
            for (int i = 0; i < int(d.modes.size()); i++)
                if (ImGui::Selectable(d.modes[i].label().c_str(), g->pickerMode == i + 1)) g->pickerMode = i + 1;
            endCombo();
        }
        ImGui::EndGroup();
        ImGui::SameLine(0, 16);
        ImGui::BeginGroup();
        label("Start with");
        static const char* presets[] = {"Keep current settings", "Lowest latency (no upscaling, Immediate)", "Balanced (FSR 1, Mailbox)",
                                        "Smooth (FSR 1, VSync)", "Best quality (FSR 4, Mailbox)"};
        int nPresets = g->renderer.fsr4Available() ? 5 : 4;
        if (g->pickerPreset >= nPresets) g->pickerPreset = 0;
        if (combo("##preset", presets[g->pickerPreset], half)) {
            for (int i = 0; i < nPresets; i++)
                if (ImGui::Selectable(presets[i], g->pickerPreset == i)) g->pickerPreset = i;
            endCombo();
        }
        ImGui::EndGroup();
    }
    ImGui::Dummy({0, 20});

    float bx = ImGui::GetCursorPosX();
    std::string err = g->capture.failed ? g->capture.failReason : "";
    if (!err.empty()) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12);
        textColored(F().regular, 12, kWarn, err.c_str());
        ImGui::SameLine();
    }
    bool canCancel = g->capture.isOpen() || g->lost;
    float bwTotal = 110 + 10 + (canCancel ? 100 + 10 : 0) + 150;
    ImGui::SetCursorPosX(bx + w - bwTotal);
    if (secondaryButton("Refresh", {110, 44})) g->refreshDevices();
    if (canCancel) {
        ImGui::SameLine(0, 10);
        if (secondaryButton("Cancel", {100, 44})) g->showPicker = false;
    }
    ImGui::SameLine(0, 10);
    if (primaryButton("Start viewing", {150, 44}, !g->devices.empty())) {
        const DeviceInfo& d = g->devices[g->pickerSelection];
        Settings& st = g->settings;
        switch (g->pickerPreset) {
            case 1: st.upscaler = Upscaler::Off; st.present = PresentMode::Immediate; break;
            case 2: st.upscaler = Upscaler::FSR1; st.present = PresentMode::Mailbox; break;
            case 3: st.upscaler = Upscaler::FSR1; st.present = PresentMode::Fifo; break;
            case 4: st.upscaler = Upscaler::FSR4; st.present = PresentMode::Mailbox; break;
            default: break;
        }
        if (g->pickerMode == 0) st.width = st.height = st.fourcc = st.fpsNum = st.fpsDen = 0;
        if (g->openDevice(d.path, g->pickerMode ? &d.modes[g->pickerMode - 1] : nullptr)) g->showPicker = false;
    }
    ImGui::EndGroup();
    ImGui::End();
}

void toast() {
    if (g->toast.empty() || nowNs() > g->toastUntilNs) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({vp->Pos.x + vp->Size.x / 2, vp->Pos.y + vp->Size.y - 56}, 0, {0.5f, 1});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, colv(kPanel2));
    ImGui::PushStyleColor(ImGuiCol_Border, colv(kBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {16, 12});
    ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize |
                                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
    textColored(F().regular, 13, kText, g->toast.c_str());
    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void emptyState(const Rect& v) {
    if (g->showPicker) return;
    bool open = g->capture.isOpen();
    // Longer than signalOk()'s 0.5 s so a hiccup doesn't flash it; a freshly opened card gets 4 s for its first frame.
    bool noSignal = open && g->noSignalSec() >= (g->capture.lastFrameNs.load() ? 1.5f : 4.0f);
    if (open && g->renderer.hasInput() && !noSignal) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImVec2 c{v.x + v.w / 2, v.y + v.h / 2};
    if (noSignal && g->renderer.hasInput()) dl->AddRectFilled({v.x, v.y}, {v.x + v.w, v.y + v.h}, col(kVideoBg, 0.8f));  // dim the stale frame

    std::string title, sub;
    bool warn = false;
    if (g->lost) {
        title = g->lostName + " disconnected";
        sub = "It reconnects automatically when it's plugged back in.";
        warn = true;
    } else if (!open) {
        title = "No capture device open";
    } else if (noSignal) {
        title = "No signal";
        sub = g->capture.name + " is connected but isn't sending frames. Check that the source is on and the cable is seated.";
        if (g->streamRestarts) sub += fmt(" The stream was restarted %llu time%s.", (unsigned long long)g->streamRestarts, g->streamRestarts == 1 ? "" : "s");
        warn = true;
    } else {
        title = "Waiting for the first frame…";
    }
    if (warn) iconWarn(dl, {c.x, c.y - 24}, 40, col(kWarn, 0.85f));
    else iconScreen(dl, {c.x, c.y - 24}, 44, col(0x4A515C), 1.5f);
    ImVec2 ts = measure(F().semibold, 15, title.c_str());
    text(dl, F().semibold, 15, {c.x - ts.x / 2, c.y + 8}, warn ? kText : kDim, title.c_str());
    if (!sub.empty()) {
        float wrap = std::min(520.f, v.w - 64);
        ImFont* f = F().regular;
        ImVec2 ss = f->CalcTextSizeA(13, FLT_MAX, wrap, sub.c_str());
        text(dl, f, 13, {c.x - std::min(ss.x, wrap) / 2, c.y + 34}, kMuted, sub.c_str(), wrap);
    }
    if (g->lost) {
        ImGui::SetNextWindowPos({c.x, c.y + 78}, 0, {0.5f, 0});
        ImGui::Begin("##lost", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoFocusOnAppearing);
        if (secondaryButton("Choose another device", {210, 40})) g->showPicker = true;
        ImGui::End();
    }
}

void shortcuts() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false) || ImGui::IsKeyPressed(ImGuiKey_F, false)) g->toggleFullscreen();
    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) g->settings.showHud = !g->settings.showHud;
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) g->showSettings = !g->showSettings;
    if (ImGui::IsKeyPressed(ImGuiKey_P, false)) g->showStats = !g->showStats;
    if (ImGui::IsKeyPressed(ImGuiKey_D, false)) g->showPicker = true;
    if (ImGui::IsKeyPressed(ImGuiKey_M, false)) {
        g->settings.muted = !g->settings.muted;
        g->showToast(g->settings.muted ? "Audio muted" : "Audio unmuted");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_U, false)) {
        Upscaler u = g->settings.upscaler;
        // Native -> FSR 1 -> NIS -> Anime4K -> FSR 3 -> FSR 4 -> Native, skipping unavailable ones
        const Upscaler order[] = {Upscaler::Off, Upscaler::FSR1, Upscaler::NIS, Upscaler::Anime4K, Upscaler::FSR3, Upscaler::FSR4};
        auto avail = [&](Upscaler x) {
            return x == Upscaler::NIS ? g->renderer.nisAvailable() : x == Upscaler::Anime4K ? g->renderer.anime4kAvailable()
                 : x == Upscaler::FSR3 ? g->renderer.temporalAvailable() : x == Upscaler::FSR4 ? g->renderer.fsr4Available() : true;
        };
        int idx = 0;
        for (int k = 0; k < 6; k++) if (order[k] == u) idx = k;
        for (int k = 1; k <= 6; k++) if (avail(order[(idx + k) % 6])) { u = order[(idx + k) % 6]; break; }
        g->settings.upscaler = u;
        g->showToast(std::string("Upscaling: ") + (u == Upscaler::Off ? "off" : upscalerName(u)));
    }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Q, false)) g->quit = true;
    if (ImGui::IsKeyPressed(ImGuiKey_F12, false)) g->screenshotRequested = true;
    if (ImGui::IsKeyPressed(ImGuiKey_G, false) && g->renderer.frameGenAvailable()) {
        g->settings.frameGen = !g->settings.frameGen;
        g->showToast(g->settings.frameGen ? "Frame generation on (2×)" : "Frame generation off");
    }
    if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        g->compareView = !g->compareView;
        Upscaler cu = g->settings.upscaler;
        if (g->compareView && (cu == Upscaler::Off || cu == Upscaler::FSR1)) g->settings.upscaler = g->renderer.temporalAvailable() ? Upscaler::FSR3 : Upscaler::NIS;
        g->showToast(g->compareView ? std::string("Split view: FSR 1 | ") + upscalerName(g->settings.upscaler) : std::string("Split view off"));
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (g->showPicker && (g->capture.isOpen() || g->lost)) g->showPicker = false;
        else if (g->showSettings) { g->showSettings = false; g->settings.save(); }
        else if (g->showStats) g->showStats = false;
        else if (g->fullscreen) g->toggleFullscreen();
    }
}

}  // namespace

// ---------------------------------------------------------------------------
void loadFonts(App& app) {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    auto add = [&](const unsigned char* data, size_t size) {
        return io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(data), int(size), 14.0f, &cfg);
    };
    app.fonts.regular = add(font_Inter_latin_400_normal, font_Inter_latin_400_normal_size);
    app.fonts.semibold = add(font_Inter_latin_600_normal, font_Inter_latin_600_normal_size);
    app.fonts.bold = add(font_Inter_latin_700_normal, font_Inter_latin_700_normal_size);
    app.fonts.mono = add(font_JetBrainsMono_latin_400_normal, font_JetBrainsMono_latin_400_normal_size);
    app.fonts.monoSemibold = add(font_JetBrainsMono_latin_600_normal, font_JetBrainsMono_latin_600_normal_size);
    io.FontDefault = app.fonts.regular;
}

void applyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.FontSizeBase = 14.0f;
    s.WindowRounding = 12;
    s.ChildRounding = 0;
    s.FrameRounding = 8;
    s.PopupRounding = 8;
    s.GrabRounding = 6;
    s.ScrollbarRounding = 8;
    s.TabRounding = 6;
    s.WindowBorderSize = 1;
    s.FrameBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FramePadding = {10, 7};
    s.ItemSpacing = {8, 8};
    s.ItemInnerSpacing = {8, 6};
    s.ScrollbarSize = 12;
    s.GrabMinSize = 14;
    s.WindowPadding = {16, 16};
    s.SelectableTextAlign = {0, 0.5f};

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = colv(kText);
    c[ImGuiCol_TextDisabled] = colv(kDim);
    c[ImGuiCol_WindowBg] = colv(kBg);
    c[ImGuiCol_ChildBg] = colv(kBg, 0);
    c[ImGuiCol_PopupBg] = colv(kPanel2);
    c[ImGuiCol_Border] = colv(kBorder);
    c[ImGuiCol_BorderShadow] = colv(0, 0);
    c[ImGuiCol_FrameBg] = colv(kPanel2);
    c[ImGuiCol_FrameBgHovered] = colv(0x232831);
    c[ImGuiCol_FrameBgActive] = colv(0x2A303A);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = c[ImGuiCol_TitleBgCollapsed] = colv(kPanel);
    c[ImGuiCol_ScrollbarBg] = colv(0, 0);
    c[ImGuiCol_ScrollbarGrab] = colv(kBorder);
    c[ImGuiCol_ScrollbarGrabHovered] = colv(0x3A404A);
    c[ImGuiCol_ScrollbarGrabActive] = colv(kDim);
    c[ImGuiCol_CheckMark] = colv(kAccent);
    c[ImGuiCol_SliderGrab] = colv(kAccent);
    c[ImGuiCol_SliderGrabActive] = colv(kAccentText);
    c[ImGuiCol_Button] = colv(kPanel2);
    c[ImGuiCol_ButtonHovered] = colv(kBorder);
    c[ImGuiCol_ButtonActive] = colv(0x3A404A);
    c[ImGuiCol_Header] = colv(0x1F2A2A);
    c[ImGuiCol_HeaderHovered] = colv(kBorder);
    c[ImGuiCol_HeaderActive] = colv(0x1F2A2A);
    c[ImGuiCol_Separator] = colv(kBorder);
    c[ImGuiCol_TableRowBg] = colv(0, 0);
    c[ImGuiCol_TableRowBgAlt] = colv(kPanel, 0.6f);
    c[ImGuiCol_TableBorderLight] = colv(kBorder);
    c[ImGuiCol_NavCursor] = colv(kAccent);
    c[ImGuiCol_ModalWindowDimBg] = colv(0, 0.6f);
}

void draw(App& app) {
    g = &app;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    float W = vp->Size.x, H = vp->Size.y;

    shortcuts();

    // Bars: always in a window; in fullscreen only while the mouse moves (or if pinned).
    bool recentInput = nowNs() - app.lastInputNs < 2'500'000'000ull;
    bool bars = !app.fullscreen || app.settings.barsInFullscreen || recentInput || app.showSettings || app.showStats;
    bool reserve = !app.fullscreen || app.settings.barsInFullscreen;
    Rect v{0, 0, W, H};
    if (reserve) v = {0, kTopBarH, W, H - kTopBarH - kBottomBarH};
    if (bars) {
        topBar(W, 0);
        bottomBar(W, H - kBottomBarH);
    }
    if (app.fullscreen && !recentInput && !app.showSettings && !app.showStats && !app.showPicker)
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);

    float s = app.pxPerPoint;
    app.videoAreaPx = {v.x * s, v.y * s, v.w * s, v.h * s};
    Rect vid = Renderer::fitRect(v, app.capture.width, app.capture.height, app.settings.scale);
    emptyState(v);
    if (app.renderer.comparing) {
        const Rect& d = app.renderer.lastDst;
        float mx = (d.x + d.w / 2) / s, top = d.y / s, bot = (d.y + d.h) / s;
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        dl->AddLine({mx, top}, {mx, bot}, col(kAccent, 0.9f), 2.0f);
        auto tag = [&](const char* t, float x) {
            ImVec2 ts = measure(F().semibold, 12, t);
            float y = top + (bars && reserve ? 12 : kTopBarH + 12);
            dl->AddRectFilled({x, y}, {x + ts.x + 16, y + 24}, col(kBg, 0.85f), 6);
            text(dl, F().semibold, 12, {x + 8, y + 5}, kText, t);
        };
        tag("FSR 1", mx - measure(F().semibold, 12, "FSR 1").x - 28);
        tag(upscalerName(app.renderer.effectiveUpscaler), mx + 12);
    }
    if (app.renderer.hasInput()) hud(vid);

    settingsWindow();
    statsWindow();
    picker();
    toast();
}

}  // namespace ui
