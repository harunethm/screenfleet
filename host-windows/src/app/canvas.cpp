#include "canvas.h"

#include "../../res/resource.h"

namespace sst::app {

namespace {

struct FontSpec {
    float size;
    float lineHeight;
    DWRITE_FONT_WEIGHT weight;
};

constexpr FontSpec kSpecs[size_t(Font::Count)] = {
    {14, 20, DWRITE_FONT_WEIGHT_NORMAL}, {14, 20, DWRITE_FONT_WEIGHT_MEDIUM}, {12, 16, DWRITE_FONT_WEIGHT_NORMAL},
    {12, 16, DWRITE_FONT_WEIGHT_MEDIUM}, {20, 28, DWRITE_FONT_WEIGHT_MEDIUM}, {16, 24, DWRITE_FONT_WEIGHT_MEDIUM},
    {24, 32, DWRITE_FONT_WEIGHT_NORMAL}, {30, 36, DWRITE_FONT_WEIGHT_NORMAL}, {11, 14, DWRITE_FONT_WEIGHT_NORMAL},
};

bool AddResourceFont(IDWriteFactory5* factory, IDWriteInMemoryFontFileLoader* loader, IDWriteFontSetBuilder1* builder,
                     int id) {
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    HGLOBAL data = LoadResource(module, res);
    DWORD size = SizeofResource(module, res);
    if (!data || !size) return false;
    // The resource stays mapped for the life of the process, so no copy is needed.
    ComPtr<IDWriteFontFile> file;
    if (FAILED(loader->CreateInMemoryFontFileReference(factory, LockResource(data), size, nullptr, &file)))
        return false;
    return SUCCEEDED(builder->AddFontFile(file.Get()));
}

}  // namespace

bool Fonts::Init(IDWriteFactory5* factory) {
    bool geist = false;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (SUCCEEDED(factory->CreateInMemoryFontFileLoader(&loader_)) &&
        SUCCEEDED(factory->RegisterFontFileLoader(loader_.Get())) &&
        SUCCEEDED(factory->CreateFontSetBuilder(&builder)) &&
        AddResourceFont(factory, loader_.Get(), builder.Get(), IDR_FONT_REGULAR) &&
        AddResourceFont(factory, loader_.Get(), builder.Get(), IDR_FONT_MEDIUM)) {
        ComPtr<IDWriteFontSet> set;
        geist = SUCCEEDED(builder->CreateFontSet(&set)) &&
                SUCCEEDED(factory->CreateFontCollectionFromFontSet(set.Get(), &collection_));
    }
    const wchar_t* family = geist ? L"Geist" : L"Segoe UI Variable Text";
    for (size_t i = 0; i < size_t(Font::Count); ++i) {
        const FontSpec& s = kSpecs[i];
        if (FAILED(factory->CreateTextFormat(family, geist ? collection_.Get() : nullptr, s.weight,
                                             DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, s.size, L"en-us",
                                             &formats_[i]))) {
            return false;
        }
        formats_[i]->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, s.lineHeight, s.lineHeight * 0.76f);
    }
    return true;
}

TextBox Canvas::Measure(const std::wstring& text, Font font, float maxWidth, bool wrap, bool tabular,
                        DWRITE_TEXT_ALIGNMENT align) const {
    TextBox box;
    if (FAILED(dwrite_->CreateTextLayout(text.c_str(), UINT32(text.size()), fonts_->Get(font), maxWidth, 10000,
                                         &box.layout))) {
        return box;
    }
    box.layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    box.layout->SetTextAlignment(align);
    if (!wrap) {
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(fonts_->Get(font), &ellipsis))) {
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            box.layout->SetTrimming(&trimming, ellipsis.Get());
        }
    }
    if (tabular) {
        ComPtr<IDWriteTypography> typography;
        if (SUCCEEDED(dwrite_->CreateTypography(&typography))) {
            typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
            box.layout->SetTypography(typography.Get(), {0, UINT32(text.size())});
        }
    }
    DWRITE_TEXT_METRICS m{};
    box.layout->GetMetrics(&m);
    box.width = m.widthIncludingTrailingWhitespace;
    box.height = m.height;
    DWRITE_LINE_METRICS line{};
    UINT32 count = 0;
    box.layout->GetLineMetrics(&line, 1, &count);
    box.baseline = line.baseline;
    return box;
}

void Canvas::Text(const TextBox& box, float x, float y, D2D1_COLOR_F color) {
    if (!box.layout) return;
    Op op{Kind::Text};
    op.r = {x, y, x, y};
    op.c = color;
    op.layout = box.layout;
    ops_.push_back(std::move(op));
}

float Canvas::Text(const std::wstring& text, Font font, float x, float y, float maxWidth, D2D1_COLOR_F color,
                   bool wrap, bool tabular) {
    TextBox box = Measure(text, font, maxWidth, wrap, tabular);
    Text(box, x, y, color);
    return box.height;
}

void Canvas::FillRect(D2D1_RECT_F r, D2D1_COLOR_F c) { ops_.push_back({Kind::Rect, r, 0, 1, false, c}); }

void Canvas::FillRound(D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
    ops_.push_back({Kind::FillRound, r, radius, 1, false, c});
}

void Canvas::StrokeRound(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width, bool dashed) {
    ops_.push_back({Kind::StrokeRound, r, radius, width, dashed, c});
}

void Canvas::Line(float x1, float y1, float x2, float y2, D2D1_COLOR_F c, float width) {
    ops_.push_back({Kind::Line, {x1, y1, x2, y2}, 0, width, false, c});
}

void Canvas::Poly(std::vector<D2D1_POINT_2F> points, D2D1_COLOR_F c, float width) {
    Op op{Kind::Poly};
    op.c = c;
    op.width = width;
    op.points = std::move(points);
    ops_.push_back(std::move(op));
}

void Canvas::FillEllipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F c) {
    ops_.push_back({Kind::FillEllipse, {cx - rx, cy - ry, cx + rx, cy + ry}, 0, 1, false, c});
}

void Canvas::StrokeEllipse(float cx, float cy, float rx, float ry, D2D1_COLOR_F c, float width) {
    ops_.push_back({Kind::StrokeEllipse, {cx - rx, cy - ry, cx + rx, cy + ry}, 0, width, false, c});
}

void Canvas::InsertFillRound(size_t mark, D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
    ops_.insert(ops_.begin() + ptrdiff_t(mark), Op{Kind::FillRound, r, radius, 1, false, c});
}

void Canvas::InsertStrokeRound(size_t mark, D2D1_RECT_F r, float radius, D2D1_COLOR_F c) {
    ops_.insert(ops_.begin() + ptrdiff_t(mark), Op{Kind::StrokeRound, r, radius, 1, false, c});
}

void Canvas::Render(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* dashed,
                    float offsetY) const {
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -offsetY));
    ComPtr<ID2D1Factory> factory;
    rt->GetFactory(&factory);
    for (const Op& op : ops_) {
        brush->SetColor(op.c);
        switch (op.kind) {
        case Kind::Rect:
            rt->FillRectangle(op.r, brush);
            break;
        case Kind::FillRound:
            rt->FillRoundedRectangle({op.r, op.radius, op.radius}, brush);
            break;
        case Kind::StrokeRound: {
            // Strokes are centred on the edge; inset by half a pixel so 1px lines stay crisp.
            float h = op.width / 2;
            D2D1_RECT_F r{op.r.left + h, op.r.top + h, op.r.right - h, op.r.bottom - h};
            rt->DrawRoundedRectangle({r, op.radius, op.radius}, brush, op.width, op.dashed ? dashed : nullptr);
            break;
        }
        case Kind::Line:
            rt->DrawLine({op.r.left, op.r.top}, {op.r.right, op.r.bottom}, brush, op.width);
            break;
        case Kind::Poly: {
            if (op.points.size() < 2) break;
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) break;
            sink->BeginFigure(op.points[0], D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLines(op.points.data() + 1, UINT32(op.points.size() - 1));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            rt->DrawGeometry(path.Get(), brush, op.width);
            break;
        }
        case Kind::FillEllipse:
            rt->FillEllipse({{(op.r.left + op.r.right) / 2, (op.r.top + op.r.bottom) / 2},
                             (op.r.right - op.r.left) / 2, (op.r.bottom - op.r.top) / 2},
                            brush);
            break;
        case Kind::StrokeEllipse:
            rt->DrawEllipse({{(op.r.left + op.r.right) / 2, (op.r.top + op.r.bottom) / 2},
                             (op.r.right - op.r.left) / 2, (op.r.bottom - op.r.top) / 2},
                            brush, op.width);
            break;
        case Kind::Text:
            rt->DrawTextLayout({op.r.left, op.r.top}, op.layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
            break;
        }
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
}

}  // namespace sst::app
