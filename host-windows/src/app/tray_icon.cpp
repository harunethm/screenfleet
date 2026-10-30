#include "tray_icon.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace sst::app {

namespace {

// The mark in its 22 x 16 design grid (see the mockup's #mark symbol).
struct Box {
    float x0, y0, x1, y1;
    bool Contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

bool InMark(float x, float y) {
    const float t = 1.6f;
    const Box outer{1 - t / 2, 1.5f - t / 2, 14 + t / 2, 11.5f + t / 2};
    const Box inner{1 + t / 2, 1.5f + t / 2, 14 - t / 2, 11.5f - t / 2};
    const Box stand{5.5f - t / 2, 14.5f - t / 2, 9.5f + t / 2, 14.5f + t / 2};
    const Box tablet{16, 5.5f, 21, 12.5f};
    return (outer.Contains(x, y) && !inner.Contains(x, y)) || stand.Contains(x, y) || tablet.Contains(x, y);
}

}  // namespace

HICON CreateMarkIcon(int size, bool white) {
    // Fit the 22-unit-wide mark into the square with a small margin.
    const float scale = size * 0.94f / 22.0f;
    const float offX = (size - 22 * scale) / 2;
    const float offY = (size - 16 * scale) / 2;
    const int ss = 4;  // 4x4 supersampling for smooth edges

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;

    const uint32_t rgb = white ? 0xFFFFFF : 0x141414;
    auto* px = static_cast<uint32_t*>(bits);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            int hits = 0;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    float ux = (x + (sx + 0.5f) / ss - offX) / scale;
                    float uy = (y + (sy + 0.5f) / ss - offY) / scale;
                    hits += InMark(ux, uy);
                }
            uint32_t a = uint32_t(std::lround(255.0 * hits / (ss * ss)));
            // premultiplied BGRA
            uint32_t r = ((rgb >> 16) & 0xff) * a / 255, g = ((rgb >> 8) & 0xff) * a / 255, b = (rgb & 0xff) * a / 255;
            px[y * size + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    std::vector<BYTE> maskBits(size_t((size + 15) / 16 * 2) * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());
    ICONINFO info{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

}  // namespace sst::app
