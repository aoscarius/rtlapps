#pragma once
// CrtDisplay: renders a grayscale composite-video framebuffer into an
// SDL2 window with an "old TV" treatment -- barrel distortion, per-row
// scanline darkening, a vignette, snow/noise while unsynced, and a
// retro channel/status on-screen display in the corners.
//
// Inspired by the CRT-emulation feature described in GOROman's
// famicom-rf-hackrf-decoder (barrel distortion + scanlines + vignette,
// toggled at runtime, with a retro channel-number + sync-status OSD and
// "snow" while unlocked) -- reimplemented from scratch here for an
// RTL-SDR / SDL2 pipeline rather than ported from their source, since
// their source wasn't fetchable for direct reuse. Same visual idea,
// independent code.
//
// The whole effect is software: a distortion lookup table is built once
// per canvas size, and each frame just walks that LUT to resample the
// source picture into an RGBA buffer that gets uploaded as one texture.

#include <cstdint>
#include <functional>
#include <random>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

class CrtDisplay {
public:
    // `channels` is 1 for grayscale (mono decoder) or 3 for interleaved
    // RGB (color decoder, --c64) -- must match whatever FrameBuffer/
    // frames given to updateFrame() use.
    CrtDisplay(int windowW, int windowH, int sourceW, int sourceH, int channels = 1);
    ~CrtDisplay();

    CrtDisplay(const CrtDisplay&) = delete;
    CrtDisplay& operator=(const CrtDisplay&) = delete;

    // Upload a new source frame: sourceW*sourceH bytes for channels==1
    // (grayscale), or sourceW*sourceH*3 interleaved RGB bytes for
    // channels==3.
    void updateFrame(const std::vector<uint8_t>& pixels);

    // OSD / status inputs, set as often as you like -- cheap.
    void setChannelLabel(const std::string& text) { channelLabel_ = text; }
    void setStatusLine(const std::string& text)   { statusLine_ = text; }
    void setFreqLine(const std::string& text)     { freqLine_ = text; }
    void setLocked(bool locked)                   { locked_ = locked; }

    void toggleCrtEffect() { crtEffectOn_ = !crtEffectOn_; }
    bool crtEffectOn() const { return crtEffectOn_; }

    void toggleFullscreen();

    void handleResize(int newW, int newH);

    // Called with the RGBA canvas buffer + its size right before it's
    // uploaded/presented, after the picture and status OSD are drawn but
    // before the frame goes to screen -- lets OsdMenu (or anything else)
    // draw on top without CrtDisplay needing to know it exists.
    using OverlayFn = std::function<void(std::vector<uint32_t>&, int, int)>;

    // Render one frame to the window.
    void render(const OverlayFn& overlay = nullptr);

    SDL_Window* window() const { return window_; }

private:
    void rebuildDistortionLUT();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* canvasTex_ = nullptr;

    int winW_, winH_;
    int srcW_, srcH_;
    int channels_;

    // Distortion LUT: for every canvas pixel, either the source pixel
    // index to sample (>=0) or -1 if it falls outside the tube (black).
    // Paired with a precomputed brightness multiplier (vignette +
    // per-row scanline factor baked in as a byte 0..255). srcIndex is a
    // *pixel* index (multiply by channels_ to get the byte offset).
    struct LutEntry { int32_t srcIndex; uint8_t shade; };
    std::vector<LutEntry> lut_;

    std::vector<uint8_t> srcPixels_;    // last uploaded source frame (1 or 3 bytes/pixel)
    std::vector<uint32_t> canvasPixels_; // scratch RGBA canvas, rebuilt each render

    std::string channelLabel_ = "CH3";
    std::string statusLine_   = "H-SYNC:NO";
    std::string freqLine_     = "0.00MHZ";
    bool locked_ = false;
    bool crtEffectOn_ = true;
    bool fullscreen_ = false;

    std::mt19937 rng_{std::random_device{}()};
};
