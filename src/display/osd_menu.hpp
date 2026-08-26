#pragma once
// OsdMenu: an in-window tuning menu for the decoder, controllable by
// keyboard (Tab to open/close, arrows to navigate/adjust, Shift for
// fine steps) or mouse (click -/+ buttons, scroll wheel over a row,
// click a row to select or to trigger a toggle/action).
//
// Deliberately decoupled from SDL_Event: main.cpp translates SDL events
// into the plain int/bool calls below, and this class draws into the
// same RGBA canvas buffer CrtDisplay already builds each frame (see
// CrtDisplay::render's OverlayFn hook), reusing the font5x7 free
// functions so the menu's glyphs are pixel-identical to the status OSD.

#include <functional>
#include <string>
#include <vector>
#include <cstdint>

struct MenuItem {
    std::string label;

    // Current value as displayed text, e.g. "61.2500MHZ" or "ON".
    std::function<std::string()> valueText;

    // Adjust by one step in `dir` (+1 or -1). `fine` is true when Shift
    // was held (smaller step, for precise alignment) and false for the
    // coarse/default step. Leave null for activate-only items (e.g. a
    // "RESET ALL" action) -- such items draw no -/+ buttons.
    std::function<void(int dir, bool fine)> adjust;

    // Triggered by Enter/Space, or by clicking the row outside the -/+
    // buttons. Typical uses: toggles (INVERT, CRT FX) and one-shot
    // actions (RESET ALL). May be null if `adjust` alone is enough
    // (adjust(+1,...) is used as a fallback toggle when activate is null).
    std::function<void()> activate;
};

class OsdMenu {
public:
    void addItem(MenuItem item) { items_.push_back(std::move(item)); }

    bool visible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }
    void toggle() { visible_ = !visible_; }

    // Keyboard navigation. `sdlKeycode` is an SDL SDLK_* value, passed
    // as plain int so this header doesn't need <SDL.h>. Returns true if
    // the menu consumed the key (callers should skip their own
    // shortcut handling for that key in that case).
    bool handleKey(int sdlKeycode, bool shiftHeld);

    // Mouse, in window pixel coordinates. Each returns true if the menu
    // is open (and therefore claimed the input) regardless of whether
    // it hit a specific control -- while the menu is up, clicks are not
    // meant to fall through to the picture underneath.
    bool handleMouseMotion(int x, int y);
    bool handleMouseButtonDown(int x, int y, bool shiftHeld);
    bool handleMouseWheel(int amount, bool shiftHeld);

    // Draws into the given RGBA canvas (the same buffer CrtDisplay
    // uploads) and refreshes the hit-test layout the mouse handlers use.
    // No-op (and clears the layout) when the menu isn't visible.
    void render(std::vector<uint32_t>& px, int canvasW, int canvasH);

private:
    struct RowLayout {
        int top = 0, bottom = 0;
        int minusX0 = 0, minusX1 = 0, plusX0 = 0, plusX1 = 0;
        bool hasButtons = false;
    };

    std::vector<MenuItem> items_;
    std::vector<RowLayout> layout_;
    int selected_ = 0;
    int hovered_ = -1;
    bool visible_ = false;
};
