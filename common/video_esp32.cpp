// ESP32-P4 video backend for Vanilla Conquer.
//
// Pipeline: game 8bpp 320x200 + 6-bit CnC palette
//   -> Set_DD_Palette() builds palette565[256] (byte-swapped RGB565)
//   -> Video_Render_Frame() expands to 2x scale (640x400) centered in
//      800x480 landscape, then writes with 90 deg CCW rotation into the
//      portrait (480x800) PSRAM framebuffer, then DMA flushes to ST7701S.

#include "video.h"
#include "gbuffer.h"
#include "palette.h"
#include <cstring>
#include <cstdint>
#include <cstdio>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
// Implemented in display_esp32.cpp, compiled as an IDF component.
extern "C" esp_lcd_panel_handle_t esp32_display_init(void);
#endif

// Logical landscape resolution
static constexpr int LCD_W     = 800;
static constexpr int LCD_H     = 480;
// Physical portrait panel (rotated 90 deg CCW in software)
static constexpr int LCD_PHY_W = 480;
static constexpr int LCD_PHY_H = 800;

// Game native resolution and integer scale factor
static constexpr int GAME_W = 320;
static constexpr int GAME_H = 200;
static constexpr int SCALE  = 2;

// Black borders around the 640x400 game area inside the 800x480 viewport
static constexpr int OFF_X = (LCD_W - GAME_W * SCALE) / 2; // 80
static constexpr int OFF_Y = (LCD_H - GAME_H * SCALE) / 2; // 40

// Precomputed byte-swapped RGB565 palette (ST7701S: P4 sends low byte first)
static uint16_t palette565[256];

// Physical framebuffer in PSRAM (portrait layout: LCD_PHY_W * LCD_PHY_H * 2)
static uint16_t* physfb = nullptr;

#ifdef ESP_PLATFORM
static esp_lcd_panel_handle_t panel_handle = nullptr;
#endif

// Map landscape (lx, ly) to index in portrait physical framebuffer.
// 90 deg CCW: portrait_x = ly, portrait_y = (LCD_PHY_H-1) - lx
static inline int phys_idx(int lx, int ly)
{
    return (LCD_PHY_H - 1 - lx) * LCD_PHY_W + ly;
}

// ---------------------------------------------------------------------------
// SurfaceMonitorClass

class SurfaceMonitorClassESP32 : public SurfaceMonitorClass
{
public:
    SurfaceMonitorClassESP32() {}
    virtual void Restore_Surfaces() {}
    virtual void Set_Surface_Focus(bool) {}
    virtual void Release() {}
};

static SurfaceMonitorClassESP32 smc_instance;
SurfaceMonitorClass& AllSurfaces = smc_instance;

SurfaceMonitorClass::SurfaceMonitorClass()
{
    SurfacesRestored = false;
}

// ---------------------------------------------------------------------------
// Free functions

bool Set_Video_Mode(int /*w*/, int /*h*/, int /*bpp*/)
{
    size_t fb_bytes = LCD_PHY_W * LCD_PHY_H * sizeof(uint16_t);

#ifdef ESP_PLATFORM
    physfb = (uint16_t*)heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!physfb) {
        printf("[video] PSRAM alloc failed\n");
        return false;
    }
    memset(physfb, 0, fb_bytes);

    panel_handle = esp32_display_init();
    if (!panel_handle) {
        printf("[video] display init failed\n");
        return false;
    }
#else
    physfb = new uint16_t[LCD_PHY_W * LCD_PHY_H]();
#endif

    printf("[video] ESP32-P4 %dx%d ready (game %dx%d at %dx, offset %d,%d)\n",
           LCD_W, LCD_H, GAME_W, GAME_H, SCALE, OFF_X, OFF_Y);
    return true;
}

bool Is_Video_Fullscreen()
{
    return true;
}

void Reset_Video_Mode()
{
#ifdef ESP_PLATFORM
    // panel is owned by IDF; we just clear our pointer
#else
    delete[] physfb;
#endif
    physfb = nullptr;
}

unsigned Get_Free_Video_Memory()
{
    return 1000000;
}

unsigned Get_Video_Hardware_Capabilities()
{
    return 0;
}

void Wait_Vert_Blank()
{
}

void Wait_Blit()
{
}

void Set_Video_Cursor_Clip(bool)
{
}

void Toggle_Video_Fullscreen()
{
}

void Get_Video_Scale(float& x, float& y)
{
    x = (float)SCALE;
    y = (float)SCALE;
}

// Mouse state in game-coordinate space (0..319, 0..199)
static int mouse_x = GAME_W / 2;
static int mouse_y = GAME_H / 2;

void Move_Video_Mouse(float xrel, float yrel)
{
    mouse_x += (int)(xrel / SCALE);
    mouse_y += (int)(yrel / SCALE);
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_x >= GAME_W) mouse_x = GAME_W - 1;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_y >= GAME_H) mouse_y = GAME_H - 1;
}

void Get_Video_Mouse(int& x, int& y)
{
    // Return scaled+offset screen coordinates so the cursor draws correctly
    x = OFF_X + mouse_x * SCALE;
    y = OFF_Y + mouse_y * SCALE;
}

void Set_Video_Cursor(void*, int, int, int, int)
{
}

void Set_DD_Palette(void* rpalette)
{
    const unsigned char* rgb6 = (const unsigned char*)rpalette;
    for (int i = 0; i < 256; i++) {
        // CnC palettes are 6-bit per channel (0-63); shift left 2 to get 8-bit
        uint8_t r = (uint8_t)(rgb6[i * 3 + 0] << 2);
        uint8_t g = (uint8_t)(rgb6[i * 3 + 1] << 2);
        uint8_t b = (uint8_t)(rgb6[i * 3 + 2] << 2);
        uint16_t r5 = (r >> 3) & 0x1F;
        uint16_t g6 = (g >> 2) & 0x3F;
        uint16_t b5 = (b >> 3) & 0x1F;
        uint16_t px = (r5 << 11) | (g6 << 5) | b5;
        // Byte-swap: ESP32-P4 MIPI-DSI sends low byte first, ST7701 expects MSB first
        palette565[i] = (uint16_t)((px >> 8) | (px << 8));
    }
}

// ---------------------------------------------------------------------------
// VideoSurface implementation

class VideoSurfaceESP32 : public VideoSurface
{
public:
    VideoSurfaceESP32(int w, int h, GBC_Enum flags) : w(w), h(h)
    {
        data = new uint8_t[w * h]();
        is_screen = (flags & GBC_VISIBLE) != 0;
    }

    virtual ~VideoSurfaceESP32()
    {
        delete[] data;
    }

    virtual void* GetData() const { return data; }
    virtual int GetPitch() const { return w; }
    virtual bool IsAllocated() const { return true; }
    virtual void AddAttachedSurface(VideoSurface*) {}
    virtual bool IsReadyToBlit() { return true; }
    virtual bool LockWait() { return true; }
    virtual bool Unlock() { return true; }

    virtual void Blt(const Rect& destRect, VideoSurface* src, const Rect& srcRect, bool /*mask*/)
    {
        const VideoSurfaceESP32* s = static_cast<const VideoSurfaceESP32*>(src);
        int rows = srcRect.Height < destRect.Height ? srcRect.Height : destRect.Height;
        int cols = srcRect.Width  < destRect.Width  ? srcRect.Width  : destRect.Width;
        for (int row = 0; row < rows; row++) {
            const uint8_t* sp = s->data + (srcRect.Y + row) * s->w + srcRect.X;
            uint8_t*       dp =   data  + (destRect.Y + row) * w  + destRect.X;
            memcpy(dp, sp, cols);
        }
    }

    virtual void FillRect(const Rect& rect, unsigned char color)
    {
        int y_end = rect.Y + rect.Height;
        if (y_end > h) y_end = h;
        int x_end = rect.X + rect.Width;
        if (x_end > w) x_end = w;
        int fill_w = x_end - rect.X;
        if (fill_w <= 0) return;
        for (int y = rect.Y; y < y_end; y++) {
            memset(data + y * w + rect.X, color, fill_w);
        }
    }

    void RenderSurface()
    {
        if (!physfb) return;

        // Expand 8bpp game buffer -> RGB565 physical framebuffer.
        // Black borders are already zero (cleared on alloc or left from last frame).
        for (int gy = 0; gy < GAME_H; gy++) {
            const uint8_t* row = data + gy * w;
            int ly0 = OFF_Y + gy * SCALE;
            for (int gx = 0; gx < GAME_W; gx++) {
                uint16_t px = palette565[row[gx]];
                int lx0 = OFF_X + gx * SCALE;
                // Write 2x2 block with 90 deg CCW rotation
                physfb[phys_idx(lx0,     ly0    )] = px;
                physfb[phys_idx(lx0 + 1, ly0    )] = px;
                physfb[phys_idx(lx0,     ly0 + 1)] = px;
                physfb[phys_idx(lx0 + 1, ly0 + 1)] = px;
            }
        }

#ifdef ESP_PLATFORM
        esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, LCD_PHY_W, LCD_PHY_H, physfb);
#endif
    }

    bool is_screen;

private:
    uint8_t* data;
    int w, h;
};

static VideoSurfaceESP32* frontSurface = nullptr;

void Video_Render_Frame()
{
    if (frontSurface) {
        frontSurface->RenderSurface();
    }
}

// ---------------------------------------------------------------------------
// Video singleton

Video::Video() {}
Video::~Video() {}

Video& Video::Shared()
{
    static Video video;
    return video;
}

VideoSurface* Video::CreateSurface(int w, int h, GBC_Enum flags)
{
    auto* surf = new VideoSurfaceESP32(w, h, flags);
    if (surf->is_screen) {
        frontSurface = surf;
    }
    return surf;
}
