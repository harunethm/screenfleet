#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace sst::app {

using Microsoft::WRL::ComPtr;

// Type roles from the mockup (size / line height / weight).
enum class Font {
    Body,         // 14 / 20 / 400
    BodyMedium,   // 14 / 20 / 500
    Small,        // 12 / 16 / 400
    SmallMedium,  // 12 / 16 / 500
    Title,        // 20 / 28 / 500
    Section,      // 16 / 24 / 500
    Kpi,          // 24 / 32 / 400
    KpiLead,      // 30 / 36 / 400
    Tiny,         // 11 / 14 / 400
    Count
};

// Geist, loaded from the exe's resources (falls back to Segoe UI Variable).
class Fonts {
public:
    bool Init(IDWriteFactory5* factory);
    IDWriteTextFormat* Get(Font f) const { return formats_[size_t(f)].Get(); }

private:
    ComPtr<IDWriteFontCollection1> collection_;
    ComPtr<IDWriteTextFormat> formats_[size_t(Font::Count)];
    ComPtr<IDWriteInMemoryFontFileLoader> loader_;
};

struct TextBox {
    ComPtr<IDWriteTextLayout> layout;
    float width = 0;
    float height = 0;
    float baseline = 0;  // of the first line, from the top
};

// A list of drawing operations in content coordinates (DIPs). Built during
// layout, then rendered with a scroll offset. Shapes can be inserted at an
// earlier mark, so a card's background is drawn under contents whose height
// was only known after laying them out.
class Canvas {
public:
    Canvas(IDWriteFactory5* dwrite, const Fonts* fonts) : dwrite_(dwrite), fonts_(fonts) {}

    void Reset() { ops_.clear(); }

    TextBox Measure(const std::wstring& text, Font font, float maxWidth, bool wrap = true, bool tabular = false,
                    DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) const;
    void Text(const TextBox& box, float x, float y, D2D1_COLOR_F color);
    // Measures and draws; returns the height.
    float Text(const std::wstring& text, Font font, float x, float y, float maxWidth, D2D1_COLOR_F color,
               bool wrap = true, bool tabular = false);

    void FillRect(D2D1_RECT_F r, D2D1_COLOR_F c);
    void FillRound(D2D1_RECT_F r, float radius, D2D1_COLOR_F c);
    void StrokeRound(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width = 1, bool dashed = false);
    void Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float width = 1);
    void Poly(std::vector<D2D1_POINT_2F> points, D2D1_COLOR_F c, float width);
    void FillEllipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F c);
    void StrokeEllipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F c, float width);

    size_t Mark() const { return ops_.size(); }
    void InsertFillRound(size_t mark, D2D1_RECT_F r, float radius, D2D1_COLOR_F c);
    void InsertStrokeRound(size_t mark, D2D1_RECT_F r, float radius, D2D1_COLOR_F c);

    void Render(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* dashed, float offsetY) const;

private:
    enum class Kind { Rect, FillRound, StrokeRound, Line, Poly, FillEllipse, StrokeEllipse, Text };
    struct Op {
        Kind kind;
        D2D1_RECT_F r{};
        float radius = 0;
        float width = 1;
        bool dashed = false;
        D2D1_COLOR_F c{};
        ComPtr<IDWriteTextLayout> layout;
        std::vector<D2D1_POINT_2F> points;
    };

    IDWriteFactory5* dwrite_;
    const Fonts* fonts_;
    std::vector<Op> ops_;
};

}  // namespace sst::app
