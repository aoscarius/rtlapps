#include "display/osd_menu.hpp"
#include "display/font5x7.hpp"

#include <SDL.h> // only for SDLK_*/mouse-related constants used below
#include <algorithm>

namespace {
constexpr int kScale = 2;
constexpr int kGlyphAdvance = (5 + 1) * kScale;
constexpr int kRowH = 7 * kScale + 8;      // glyph height + row padding
constexpr int kPad = 10;
constexpr int kBtnAreaW = kGlyphAdvance * 5; // room for "-" + gap + "+"

constexpr uint32_t kBorderColor = 0xFF33FF33u;
constexpr uint32_t kTitleColor  = 0xFF33FF33u;
constexpr uint32_t kLabelColor  = 0xFFDDDDDDu;
constexpr uint32_t kValueColor  = 0xFFFFDD33u;
constexpr uint32_t kSelBg       = 0xFF224422u;
constexpr uint32_t kHoverBg     = 0xFF1A2A1Au;
}

bool OsdMenu::handleKey(int key, bool shift) {
    if (!visible_) return false;
    if (items_.empty()) return true;

    switch (key) {
        case SDLK_UP:
            selected_ = (selected_ - 1 + (int)items_.size()) % (int)items_.size();
            return true;
        case SDLK_DOWN:
            selected_ = (selected_ + 1) % (int)items_.size();
            return true;
        case SDLK_LEFT:
            if (items_[selected_].adjust) items_[selected_].adjust(-1, shift);
            return true;
        case SDLK_RIGHT:
            if (items_[selected_].adjust) items_[selected_].adjust(+1, shift);
            return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            if (items_[selected_].activate) items_[selected_].activate();
            else if (items_[selected_].adjust) items_[selected_].adjust(+1, shift);
            return true;
        case SDLK_ESCAPE:
            visible_ = false;
            return true;
        default:
            // Swallow everything else while the menu is open so global
            // shortcuts (gain/freq keys, quit, etc.) don't also fire.
            return true;
    }
}

bool OsdMenu::handleMouseMotion(int /*x*/, int y) {
    if (!visible_) return false;
    hovered_ = -1;
    for (size_t i = 0; i < layout_.size(); ++i) {
        if (y >= layout_[i].top && y < layout_[i].bottom) { hovered_ = (int)i; break; }
    }
    return true;
}

bool OsdMenu::handleMouseButtonDown(int x, int y, bool shift) {
    if (!visible_) return false;
    for (size_t i = 0; i < layout_.size(); ++i) {
        const RowLayout& row = layout_[i];
        if (y < row.top || y >= row.bottom) continue;

        selected_ = (int)i;
        MenuItem& item = items_[i];
        if (row.hasButtons && x >= row.minusX0 && x < row.minusX1) {
            if (item.adjust) item.adjust(-1, shift);
        } else if (row.hasButtons && x >= row.plusX0 && x < row.plusX1) {
            if (item.adjust) item.adjust(+1, shift);
        } else if (item.activate) {
            item.activate();
        } else if (item.adjust) {
            item.adjust(+1, shift); // bare toggle row: click flips it
        }
        break;
    }
    return true; // menu is modal while open: consume the click either way
}

bool OsdMenu::handleMouseWheel(int amount, bool shift) {
    if (!visible_ || amount == 0) return false;
    int idx = hovered_ >= 0 ? hovered_ : selected_;
    if (idx < 0 || idx >= (int)items_.size()) return true;
    if (items_[idx].adjust) items_[idx].adjust(amount > 0 ? +1 : -1, shift);
    return true;
}

void OsdMenu::render(std::vector<uint32_t>& px, int canvasW, int canvasH) {
    layout_.clear();
    if (!visible_ || items_.empty()) return;

    int labelColW = 0, valueColW = 0;
    for (const auto& it : items_) {
        labelColW = std::max(labelColW, font5x7::text_width(it.label, kScale));
        std::string v = it.valueText ? it.valueText() : "";
        valueColW = std::max(valueColW, font5x7::text_width(v, kScale));
    }

    int panelW = kPad * 2 + labelColW + kPad + valueColW + kPad + kBtnAreaW;
    int panelH = kPad * 2 + kRowH + (int)items_.size() * kRowH; // +1 row for the title
    panelW = std::min(panelW, canvasW - 20);
    panelH = std::min(panelH, canvasH - 20);
    int panelX = std::max(10, (canvasW - panelW) / 2);
    int panelY = std::max(10, (canvasH - panelH) / 2);

    // Darken the picture behind the panel so text stays legible over
    // whatever's currently on screen (including CRT snow).
    for (int y = panelY; y < panelY + panelH && y < canvasH; ++y) {
        for (int x = panelX; x < panelX + panelW && x < canvasW; ++x) {
            uint32_t c = px[(size_t)y * canvasW + x];
            uint8_t r = (uint8_t)(((c >> 16) & 0xFF) * 0.15f);
            uint8_t g = (uint8_t)(((c >> 8) & 0xFF) * 0.15f);
            uint8_t b = (uint8_t)((c & 0xFF) * 0.15f);
            px[(size_t)y * canvasW + x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    // Simple 1px border outline.
    for (int x = panelX; x < panelX + panelW && x < canvasW; ++x) {
        if (panelY < canvasH) px[(size_t)panelY * canvasW + x] = kBorderColor;
        int by = panelY + panelH - 1;
        if (by < canvasH) px[(size_t)by * canvasW + x] = kBorderColor;
    }
    for (int y = panelY; y < panelY + panelH && y < canvasH; ++y) {
        if (panelX < canvasW) px[(size_t)y * canvasW + panelX] = kBorderColor;
        int bx = panelX + panelW - 1;
        if (bx < canvasW) px[(size_t)y * canvasW + bx] = kBorderColor;
    }

    font5x7::draw_text(px, canvasW, canvasH, panelX + kPad, panelY + kPad,
                        "TUNING MENU", kScale, kTitleColor);

    int rowY = panelY + kPad + kRowH;
    for (size_t i = 0; i < items_.size(); ++i) {
        const MenuItem& item = items_[i];
        RowLayout row;
        row.top = rowY - 3;
        row.bottom = rowY + kRowH - 3;

        bool isSel = (int)i == selected_;
        bool isHover = (int)i == hovered_;
        if (isSel || isHover) {
            uint32_t bg = isSel ? kSelBg : kHoverBg;
            for (int y = std::max(row.top, 0); y < row.bottom && y < canvasH; ++y)
                for (int x = panelX + 2; x < panelX + panelW - 2 && x < canvasW; ++x)
                    px[(size_t)y * canvasW + x] = bg;
        }

        font5x7::draw_text(px, canvasW, canvasH, panelX + kPad, rowY, item.label, kScale, kLabelColor);

        std::string v = item.valueText ? item.valueText() : "";
        int valueX = panelX + kPad + labelColW + kPad;
        font5x7::draw_text(px, canvasW, canvasH, valueX, rowY, v, kScale, kValueColor);

        row.hasButtons = (bool)item.adjust;
        if (row.hasButtons) {
            int minusX = panelX + panelW - kPad - kBtnAreaW;
            int plusX = minusX + kGlyphAdvance * 3;
            font5x7::draw_glyph(px, canvasW, canvasH, minusX, rowY, '-', kScale, kBorderColor);
            font5x7::draw_glyph(px, canvasW, canvasH, plusX, rowY, '+', kScale, kBorderColor);
            // Generous click targets: the full row height, a bit wider
            // than the glyph itself, easier to hit than the bare pixels.
            row.minusX0 = minusX - kGlyphAdvance / 2;
            row.minusX1 = minusX + kGlyphAdvance;
            row.plusX0 = plusX - kGlyphAdvance / 2;
            row.plusX1 = plusX + kGlyphAdvance;
        }

        layout_.push_back(row);
        rowY += kRowH;
    }
}
