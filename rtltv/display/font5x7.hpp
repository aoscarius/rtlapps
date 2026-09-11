#pragma once
// Tiny 5x7 dot-matrix font, defined as ASCII art (row-major, '#'=lit).
// Only the glyphs the on-screen displays (status OSD + tuning menu)
// actually use are included -- this is decorative retro-TV chrome, not
// a general text renderer.
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace font5x7 {

struct Glyph { std::array<std::string_view, 7> rows; };

inline const Glyph& get(char c) {
    static const Glyph SPACE{{"     ","     ","     ","     ","     ","     ","     "}};
    static const Glyph D0{{" ### ","#   #","#  ##","# # #","##  #","#   #"," ### "}};
    static const Glyph D1{{"  #  "," ##  ","  #  ","  #  ","  #  ","  #  "," ### "}};
    static const Glyph D2{{" ### ","#   #","    #","   # ","  #  "," #   ","#####"}};
    static const Glyph D3{{" ### ","#   #","    #","  ## ","    #","#   #"," ### "}};
    static const Glyph D4{{"   # ","  ## "," # # ","#  # ","#####","   # ","   # "}};
    static const Glyph D5{{"#####","#    ","#### ","    #","    #","#   #"," ### "}};
    static const Glyph D6{{" ### ","#    ","#### ","#   #","#   #","#   #"," ### "}};
    static const Glyph D7{{"#####","    #","   # ","  #  "," #   "," #   "," #   "}};
    static const Glyph D8{{" ### ","#   #","#   #"," ### ","#   #","#   #"," ### "}};
    static const Glyph D9{{" ### ","#   #","#   #"," ####","    #","#   #"," ### "}};
    static const Glyph C{{" ####","#    ","#    ","#    ","#    ","#    "," ####"}};
    static const Glyph H{{"#   #","#   #","#   #","#####","#   #","#   #","#   #"}};
    static const Glyph S{{" ####","#    ","#    "," ### ","    #","    #","#### "}};
    static const Glyph Y{{"#   #","#   #"," # # ","  #  ","  #  ","  #  ","  #  "}};
    static const Glyph N{{"#   #","##  #","# # #","# # #","#  ##","#   #","#   #"}};
    static const Glyph O{{" ### ","#   #","#   #","#   #","#   #","#   #"," ### "}};
    static const Glyph K{{"#   #","#  # ","# #  ","##   ","# #  ","#  # ","#   #"}};
    static const Glyph M{{"#   #","## ##","# # #","# # #","#   #","#   #","#   #"}};
    static const Glyph Z{{"#####","    #","   # ","  #  "," #   ","#    ","#####"}};
    static const Glyph COLON{{"     ","  #  ","  #  ","     ","  #  ","  #  ","     "}};
    static const Glyph DASH{{"     ","     ","     ","#####","     ","     ","     "}};
    static const Glyph DOT{{"     ","     ","     ","     ","     ","  ## ","  ## "}};
    static const Glyph G{{" ### ","#    ","#    ","#  ##","#   #","#   #"," ### "}};
    static const Glyph A{{" ### ","#   #","#   #","#####","#   #","#   #","#   #"}};
    static const Glyph I{{"#####","  #  ","  #  ","  #  ","  #  ","  #  ","#####"}};
    static const Glyph B{{"#### ","#   #","#   #","#### ","#   #","#   #","#### "}};
    static const Glyph V{{"#   #","#   #","#   #","#   #","#   #"," # # ","  #  "}};
    static const Glyph L{{"#    ","#    ","#    ","#    ","#    ","#    ","#####"}};
    static const Glyph U{{"#   #","#   #","#   #","#   #","#   #","#   #"," ### "}};
    static const Glyph P{{"#### ","#   #","#   #","#### ","#    ","#    ","#    "}};
    static const Glyph F{{"#####","#    ","#    ","#### ","#    ","#    ","#    "}};
    static const Glyph R{{"#### ","#   #","#   #","#### ","# #  ","#  # ","#   #"}};
    static const Glyph Q{{" ### ","#   #","#   #","#   #","# # #","#  # "," ## #"}};
    static const Glyph E{{"#####","#    ","#    ","#### ","#    ","#    ","#####"}};
    static const Glyph T{{"#####","  #  ","  #  ","  #  ","  #  ","  #  ","  #  "}};
    static const Glyph W{{"#   #","#   #","#   #","# # #","# # #","## ##","#   #"}};
    static const Glyph X{{"#   #","#   #"," # # ","  #  "," # # ","#   #","#   #"}};
    static const Glyph PLUS{{"     ","  #  ","  #  ","#####","  #  ","  #  ","     "}};
    static const Glyph SLASH{{"    #","   # ","  #  ","  #  "," #   ","#    ","     "}};

    switch (c) {
        case '0': return D0; case '1': return D1; case '2': return D2;
        case '3': return D3; case '4': return D4; case '5': return D5;
        case '6': return D6; case '7': return D7; case '8': return D8;
        case '9': return D9;
        case 'C': return C; case 'H': return H; case 'S': return S;
        case 'Y': return Y; case 'N': return N; case 'O': return O;
        case 'K': return K; case 'M': return M; case 'Z': return Z;
        case 'G': return G; case 'A': return A; case 'I': return I;
        case 'B': return B; case 'V': return V; case 'L': return L;
        case 'U': return U; case 'P': return P; case 'F': return F;
        case 'R': return R; case 'Q': return Q; case 'E': return E;
        case 'T': return T; case 'W': return W; case 'X': return X;
        case ':': return COLON; case '-': return DASH; case '.': return DOT;
        case '+': return PLUS; case '/': return SLASH;
        default:  return SPACE;
    }
}

// Draws one glyph directly into an RGBA8888 canvas buffer (top-left
// origin at x,y), clipping against canvasW/canvasH. Shared by CrtDisplay
// (status OSD) and OsdMenu (tuning menu) so both use the exact same
// rendering path.
inline void draw_glyph(std::vector<uint32_t>& px, int canvasW, int canvasH,
                        int x, int y, char c, int scale, uint32_t color) {
    const auto& glyph = get((c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c);
    for (int row = 0; row < 7; ++row) {
        for (int col = 0; col < 5; ++col) {
            if (glyph.rows[row][col] != '#') continue;
            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    int px_x = x + col * scale + sx;
                    int px_y = y + row * scale + sy;
                    if (px_x < 0 || px_x >= canvasW || px_y < 0 || px_y >= canvasH) continue;
                    px[(size_t)px_y * canvasW + px_x] = color;
                }
            }
        }
    }
}

inline void draw_text(std::vector<uint32_t>& px, int canvasW, int canvasH,
                       int x, int y, const std::string& text, int scale, uint32_t color) {
    int cursorX = x;
    for (char c : text) {
        draw_glyph(px, canvasW, canvasH, cursorX, y, c, scale, color);
        cursorX += (5 + 1) * scale;
    }
}

// Pixel width of `text` at the given scale -- lets callers right-align
// labels/values without hardcoding column positions.
inline int text_width(const std::string& text, int scale) {
    return (int)text.size() * (5 + 1) * scale;
}

// Horizontal pixel advance from one glyph's origin to the next, at the
// given scale. Useful for computing click-hit rectangles around
// individually-drawn glyphs (e.g. the -/+ buttons in the tuning menu).
inline int glyph_advance(int scale) { return (5 + 1) * scale; }

} // namespace font5x7
