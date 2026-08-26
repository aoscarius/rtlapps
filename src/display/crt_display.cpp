#include "display/crt_display.hpp"
#include "display/font5x7.hpp"

#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

CrtDisplay::CrtDisplay(int windowW, int windowH, int sourceW, int sourceH)
    : winW_(windowW), winH_(windowH), srcW_(sourceW), srcH_(sourceH) {
    window_ = SDL_CreateWindow("Pong-on-Chip RF Decoder - Old TV",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                winW_, winH_, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    renderer_ = SDL_CreateRenderer(window_, -1,
                                    SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    canvasTex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
                                    SDL_TEXTUREACCESS_STREAMING, winW_, winH_);
    srcGray_.assign((size_t)srcW_ * srcH_, 0);
    rebuildDistortionLUT();
}

CrtDisplay::~CrtDisplay() {
    if (canvasTex_) SDL_DestroyTexture(canvasTex_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
}

void CrtDisplay::handleResize(int newW, int newH) {
    if (newW == winW_ && newH == winH_) return;
    winW_ = newW; winH_ = newH;
    if (canvasTex_) SDL_DestroyTexture(canvasTex_);
    canvasTex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGBA8888,
                                    SDL_TEXTUREACCESS_STREAMING, winW_, winH_);
    rebuildDistortionLUT();
}

// Builds, once per canvas size, a per-pixel mapping from output (canvas)
// coordinates back into the source picture, applying a barrel-style
// distortion so the image bulges like a real CRT tube and the corners
// go black outside the "glass". Also bakes in a per-pixel shade byte
// combining scanline darkening (every other output row) and a radial
// vignette, so the hot per-frame loop is just a table lookup + multiply.
void CrtDisplay::rebuildDistortionLUT() {
    lut_.assign((size_t)winW_ * winH_, LutEntry{-1, 255});

    // Active picture area: a 4:3 region centered in the window with a
    // dark bezel margin, matching an old TV's screen-within-a-cabinet.
    int margin = std::max(20, winW_ / 40);
    int areaW = winW_ - 2 * margin;
    int areaH = winH_ - 2 * margin;
    if (areaW * 3 > areaH * 4) areaW = areaH * 4 / 3; else areaH = areaW * 3 / 4;
    int areaX = (winW_ - areaW) / 2;
    int areaY = (winH_ - areaH) / 2;

    const float k = 0.18f;           // barrel distortion strength
    const float vignetteStrength = 0.55f;

    for (int y = 0; y < winH_; ++y) {
        for (int x = 0; x < winW_; ++x) {
            LutEntry& e = lut_[(size_t)y * winW_ + x];

            // Normalized device coords within the picture area, -1..1.
            float nx = ((x - areaX) / (float)areaW) * 2.0f - 1.0f;
            float ny = ((y - areaY) / (float)areaH) * 2.0f - 1.0f;

            float r2 = nx * nx + ny * ny;
            float factor = 1.0f + k * r2;
            float sx = nx * factor;
            float sy = ny * factor;

            uint8_t scanShade = (y % 2 == 0) ? 255 : 200; // dim odd rows
            float dist = std::sqrt(nx * nx + ny * ny) / 1.4142f; // 0 center .. 1 corner
            float vig = 1.0f - vignetteStrength * dist * dist;
            uint8_t shade = (uint8_t)std::clamp(scanShade * vig, 0.0f, 255.0f);

            if (sx < -1.0f || sx > 1.0f || sy < -1.0f || sy > 1.0f) {
                e = LutEntry{-1, shade};
                continue;
            }

            int srcX = (int)std::clamp((sx * 0.5f + 0.5f) * srcW_, 0.0f, (float)(srcW_ - 1));
            int srcY = (int)std::clamp((sy * 0.5f + 0.5f) * srcH_, 0.0f, (float)(srcH_ - 1));
            e = LutEntry{srcY * srcW_ + srcX, shade};
        }
    }
    canvasPixels_.assign((size_t)winW_ * winH_, 0xFF000000u); // opaque black
}

void CrtDisplay::updateFrame(const std::vector<uint8_t>& gray) {
    if (gray.size() == srcGray_.size()) {
        srcGray_ = gray;
    } else if (!gray.empty()) {
        // Defensive: source size mismatch, just take what fits.
        std::copy_n(gray.begin(), std::min(gray.size(), srcGray_.size()), srcGray_.begin());
    }
}

void CrtDisplay::render(const OverlayFn& overlay) {
    std::uniform_int_distribution<int> noiseDist(0, 255);

    const size_t n = canvasPixels_.size();
    if (crtEffectOn_) {
        for (size_t i = 0; i < n; ++i) {
            const LutEntry& e = lut_[i];
            uint8_t r, g, b;
            if (e.srcIndex < 0) {
                r = g = b = 0;
            } else if (!locked_) {
                // Unsynced: snow. Occasional lucky pixel still shows signal
                // to sell the "fighting through interference" look.
                uint8_t noise = (uint8_t)noiseDist(rng_);
                r = g = b = noise;
            } else {
                uint8_t v = srcGray_[(size_t)e.srcIndex];
                float shaded = v * (e.shade / 255.0f);
                r = g = b = (uint8_t)std::clamp(shaded, 0.0f, 255.0f);
                // Faint green-white phosphor tint, typical of cheap sets.
                g = (uint8_t)std::min(255, g + 6);
            }
            canvasPixels_[i] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    } else {
        // Effect off: plain nearest-neighbor scale, no distortion/scanlines.
        for (int y = 0; y < winH_; ++y) {
            for (int x = 0; x < winW_; ++x) {
                int sx = x * srcW_ / winW_;
                int sy = y * srcH_ / winH_;
                uint8_t v = srcGray_[(size_t)sy * srcW_ + sx];
                canvasPixels_[(size_t)y * winW_ + x] = 0xFF000000u | (v << 16) | (v << 8) | v;
            }
        }
    }

    // Retro status OSD, drawn straight into the canvas buffer.
    font5x7::draw_text(canvasPixels_, winW_, winH_, 24, 18, channelLabel_, 4, 0xFF33FF33u); // big green
    font5x7::draw_text(canvasPixels_, winW_, winH_, winW_ - 260, 18, statusLine_, 2,
                        locked_ ? 0xFFFFDD33u : 0xFFFF5533u);
    font5x7::draw_text(canvasPixels_, winW_, winH_, winW_ - 260, 42, freqLine_, 2, 0xFFFFDD33u);

    // Anything drawn on top of the picture but not part of CrtDisplay
    // itself (currently: the tuning menu) hooks in here.
    if (overlay) overlay(canvasPixels_, winW_, winH_);

    SDL_UpdateTexture(canvasTex_, nullptr, canvasPixels_.data(), winW_ * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, canvasTex_, nullptr, nullptr);
    SDL_RenderPresent(renderer_);
}
