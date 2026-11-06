#include "ui/canvas.h"
#include "ui/resources.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace xww::ui {

TypeSpec Spec(Type type) {
    constexpr auto Regular = DWRITE_FONT_WEIGHT_NORMAL, Medium = DWRITE_FONT_WEIGHT_MEDIUM;
    constexpr float geist = -0.025f;
    switch (type) {
    case Type::Caption: return { 12, 18, Medium, geist, false };
    case Type::BodySm: return { 14, 20, Medium, geist, false };
    case Type::Body: return { 16, 24, Regular, geist, false };
    case Type::Subheading: return { 18, 28, Regular, geist, false };
    case Type::Heading: return { 20, 28, Medium, geist, false };
    case Type::HeadingLg: return { 48, 56, Medium, geist, false };
    case Type::Code: return { 13, 20, Regular, 0, true };
    case Type::Display: return { 60, 60, Medium, -0.08f, true };
    case Type::DisplayXl: return { 72, 72, Medium, -0.08f, true };
    case Type::Mark: return { 12, 18, Regular, 0, false };
    }
    return { 16, 24, Regular, geist, false };
}

// ------------------------------------------------------------------ setup

bool Canvas::Init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(write_.GetAddressOf())))) return false;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.GetAddressOf()));
    D2D1_STROKE_STYLE_PROPERTIES round{ D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND };
    factory_->CreateStrokeStyle(round, nullptr, 0, round_.GetAddressOf());
    LoadFonts();
    return true;
}

// Geist ships inside the exe; Windows 10 1703 and later load fonts from memory.
void Canvas::LoadFonts() {
    textFamily_ = L"Segoe UI Variable Text";
    monoFamily_ = L"Cascadia Mono";
    ComPtr<IDWriteFactory5> factory;
    ComPtr<IDWriteInMemoryFontFileLoader> loader;
    ComPtr<IDWriteFontSetBuilder1> builder;
    if (FAILED(write_.As(&factory)) || FAILED(factory->CreateInMemoryFontFileLoader(loader.GetAddressOf())) ||
        FAILED(factory->RegisterFontFileLoader(loader.Get())) || FAILED(factory->CreateFontSetBuilder(builder.GetAddressOf())))
        return;
    for (int id : { kFontGeistRegular, kFontGeistMedium, kFontMonoRegular, kFontMonoMedium }) {
        HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
        HGLOBAL data = info ? LoadResource(nullptr, info) : nullptr;
        ComPtr<IDWriteFontFile> file;
        if (!data || FAILED(loader->CreateInMemoryFontFileReference(factory.Get(), LockResource(data), SizeofResource(nullptr, info), nullptr, file.GetAddressOf()))) continue;
        builder->AddFontFile(file.Get());
    }
    ComPtr<IDWriteFontSet> set;
    ComPtr<IDWriteFontCollection1> collection;
    if (FAILED(builder->CreateFontSet(set.GetAddressOf())) || FAILED(factory->CreateFontCollectionFromFontSet(set.Get(), collection.GetAddressOf()))) return;
    UINT32 index = 0;
    BOOL text = FALSE, mono = FALSE;
    collection->FindFamilyName(L"Geist", &index, &text);
    collection->FindFamilyName(L"Geist Mono", &index, &mono);
    if (!text || !mono) return;
    fonts_ = collection;
    textFamily_ = L"Geist";
    monoFamily_ = L"Geist Mono";
}

void Canvas::Begin(ID2D1RenderTarget* target, float scale) {
    if (target_ != target) brush_.Reset();
    target_ = target;
    scale_ = scale;
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
}

void Canvas::ReleaseDeviceResources() {
    brush_.Reset();
    logos_.clear();
}

ID2D1SolidColorBrush* Canvas::Brush(D2D1_COLOR_F color) {
    if (!brush_) target_->CreateSolidColorBrush(color, brush_.GetAddressOf());
    else brush_->SetColor(color);
    return brush_.Get();
}

// ------------------------------------------------------------------ shapes

void Canvas::Fill(const D2D1_RECT_F& r, D2D1_COLOR_F color, float radius) {
    if (color.a <= 0) return;
    if (radius > 0) target_->FillRoundedRectangle({ r, radius, radius }, Brush(color));
    else target_->FillRectangle(r, Brush(color));
}

// a 1 px line inside the edge, on the pixel grid
void Canvas::Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F color, float radius, float width) {
    D2D1_RECT_F inner{ r.left + width / 2, r.top + width / 2, r.right - width / 2, r.bottom - width / 2 };
    if (radius > 0) target_->DrawRoundedRectangle({ inner, std::max(0.f, radius - width / 2), std::max(0.f, radius - width / 2) }, Brush(color), width);
    else target_->DrawRectangle(inner, Brush(color), width);
}

void Canvas::Circle(D2D1_POINT_2F c, float radius, D2D1_COLOR_F color) { target_->FillEllipse({ c, radius, radius }, Brush(color)); }

// shadow-cta: 0 1px 3px 0 black 10%, 0 1px 2px -1px black 10%; each blur drawn as a few fading steps
void Canvas::CtaShadow(const D2D1_RECT_F& r) {
    float radius = Height(r) / 2;
    for (int i = 1; i <= 3; ++i) {
        float spread = i * 0.5f;
        Fill({ r.left - spread, r.top + 1 - spread, r.right + spread, r.bottom + 1 + spread }, D2D1_COLOR_F{ 0, 0, 0, 0.06f }, radius + spread);
    }
}

// ------------------------------------------------------------------ text

IDWriteTextFormat* Canvas::Format(const TextStyle& s) {
    auto key = std::make_tuple(static_cast<int>(s.type), s.wrap ? 1 : 0, static_cast<int>(s.align), s.centerVertically ? 1 : 0);
    auto& slot = formats_[key];
    if (slot) return slot.Get();
    auto spec = Spec(s.type);
    const wchar_t* family = s.type == Type::Mark ? L"Yu Gothic UI" : spec.mono ? monoFamily_.c_str() : textFamily_.c_str();
    auto collection = s.type == Type::Mark ? nullptr : fonts_.Get();
    if (FAILED(write_->CreateTextFormat(family, collection, spec.weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, spec.size, L"en-us", slot.GetAddressOf()))) return nullptr;
    slot->SetWordWrapping(s.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    slot->SetTextAlignment(s.align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : s.align == Align::Trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
    slot->SetParagraphAlignment(s.centerVertically ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    slot->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, spec.line, spec.line * 0.78f);
    if (!s.wrap) {
        DWRITE_TRIMMING trimming{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        ComPtr<IDWriteInlineObject> ellipsis;
        write_->CreateEllipsisTrimmingSign(slot.Get(), ellipsis.GetAddressOf());
        slot->SetTrimming(&trimming, ellipsis.Get());
    }
    return slot.Get();
}

ComPtr<IDWriteTextLayout> Canvas::Layout(const std::wstring& text, const TextStyle& style, float width, float height) {
    ComPtr<IDWriteTextLayout> layout;
    auto format = Format(style);
    if (!format) return layout;
    auto spec = Spec(style.type);
    if (FAILED(write_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, std::max(width, 1.f), std::max(height, 1.f), layout.GetAddressOf()))) return layout;
    ComPtr<IDWriteTextLayout1> spaced;
    if (spec.tracking != 0 && SUCCEEDED(layout.As(&spaced))) spaced->SetCharacterSpacing(0, spec.tracking * spec.size, 0, { 0, static_cast<UINT32>(text.size()) });
    return layout;
}

void Canvas::Text(const std::wstring& text, const D2D1_RECT_F& r, D2D1_COLOR_F color, const TextStyle& style) {
    if (text.empty() || Width(r) <= 0) return;
    if (auto layout = Layout(text, style, Width(r), Height(r)))
        target_->DrawTextLayout({ r.left, r.top }, layout.Get(), Brush(color), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

D2D1_SIZE_F Canvas::Measure(const std::wstring& text, float maxWidth, const TextStyle& style) {
    if (text.empty()) return { 0, 0 };
    auto s = style;
    s.centerVertically = false;
    auto layout = Layout(text, s, maxWidth, 10000.f);
    if (!layout) return { 0, 0 };
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return { std::ceil(m.widthIncludingTrailingWhitespace), std::ceil(m.height) };
}

// ------------------------------------------------------------------ icons

// Path data as written by tools\Update-Icons.ps1 (SVG grammar, absolute and relative commands); figures stay open.
ID2D1PathGeometry* Canvas::Geometry(const char* path) {
    auto& slot = geometries_[path];
    if (slot) return slot.Get();
    ComPtr<ID2D1GeometrySink> sink;
    factory_->CreatePathGeometry(slot.GetAddressOf());
    slot->Open(sink.GetAddressOf());

    const char* p = path;
    auto skip = [&] { while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p; };
    auto isNumber = [&] { skip(); return *p == '-' || *p == '+' || *p == '.' || std::isdigit(static_cast<unsigned char>(*p)); };
    auto number = [&] { skip(); char* end = nullptr; float v = std::strtof(p, &end); p = end; return v; };
    auto flag = [&] { skip(); float v = *p == '1' ? 1.f : 0.f; ++p; return v; };   // arc flags may be glued to the next number

    D2D1_POINT_2F cur{}, start{}, ctrl{};
    bool open = false;
    char cmd = 0, last = 0;
    auto begin = [&](D2D1_POINT_2F at) { if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN); sink->BeginFigure(at, D2D1_FIGURE_BEGIN_HOLLOW); open = true; start = at; };
    while (true) {
        skip();
        if (!*p) break;
        if (std::isalpha(static_cast<unsigned char>(*p))) cmd = *p++;
        else if (!isNumber()) { ++p; continue; }
        bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
        auto pt = [&](float x, float y) { return rel ? D2D1_POINT_2F{ cur.x + x, cur.y + y } : D2D1_POINT_2F{ x, y }; };
        switch (std::toupper(static_cast<unsigned char>(cmd))) {
        case 'M': { float x = number(), y = number(); cur = pt(x, y); begin(cur); cmd = rel ? 'l' : 'L'; break; }
        case 'L': { float x = number(), y = number(); cur = pt(x, y); sink->AddLine(cur); break; }
        case 'H': { float x = number(); cur.x = rel ? cur.x + x : x; sink->AddLine(cur); break; }
        case 'V': { float y = number(); cur.y = rel ? cur.y + y : y; sink->AddLine(cur); break; }
        case 'C': {
            float a = number(), b = number(), c = number(), d = number(), e = number(), f = number();
            auto p1 = pt(a, b), p2 = pt(c, d), p3 = pt(e, f);
            sink->AddBezier({ p1, p2, p3 }); ctrl = p2; cur = p3; break;
        }
        case 'S': {
            float c = number(), d = number(), e = number(), f = number();
            bool smooth = std::toupper(static_cast<unsigned char>(last)) == 'C' || std::toupper(static_cast<unsigned char>(last)) == 'S';
            D2D1_POINT_2F p1 = smooth ? D2D1_POINT_2F{ 2 * cur.x - ctrl.x, 2 * cur.y - ctrl.y } : cur;
            auto p2 = pt(c, d), p3 = pt(e, f);
            sink->AddBezier({ p1, p2, p3 }); ctrl = p2; cur = p3; break;
        }
        case 'Q': {
            float a = number(), b = number(), e = number(), f = number();
            auto p1 = pt(a, b), p2 = pt(e, f);
            sink->AddQuadraticBezier({ p1, p2 }); ctrl = p1; cur = p2; break;
        }
        case 'A': {
            float rx = number(), ry = number(), rotation = number(), large = flag(), sweep = flag(), x = number(), y = number();
            cur = pt(x, y);
            sink->AddArc({ cur, { rx, ry }, rotation, sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL });
            break;
        }
        case 'Z': if (open) { sink->EndFigure(D2D1_FIGURE_END_CLOSED); open = false; } cur = start; break;
        default: ++p; break;
        }
        last = cmd;
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    return slot.Get();
}

void Canvas::Icon(const char* path, D2D1_POINT_2F at, float size, D2D1_COLOR_F color) {
    D2D1_MATRIX_3X2_F old;
    target_->GetTransform(&old);
    float scale = size / 24.f;
    target_->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation(at.x, at.y) * old);
    target_->DrawGeometry(Geometry(path), Brush(color), metric::IconStroke, round_.Get());
    target_->SetTransform(old);
}

// ------------------------------------------------------------------ logos

ID2D1Bitmap* Canvas::Logo(int resource, int width, int height) {
    auto& slot = logos_[{ resource, width }];
    if (slot || !wic_) return slot.Get();
    HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(resource), RT_RCDATA);
    HGLOBAL data = info ? LoadResource(nullptr, info) : nullptr;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (!data || FAILED(wic_->CreateStream(stream.GetAddressOf())) ||
        FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(LockResource(data)), SizeofResource(nullptr, info))) ||
        FAILED(wic_->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf())) ||
        FAILED(decoder->GetFrame(0, frame.GetAddressOf())) || FAILED(wic_->CreateBitmapScaler(scaler.GetAddressOf())) ||
        FAILED(scaler->Initialize(frame.Get(), width, height, WICBitmapInterpolationModeHighQualityCubic)) ||
        FAILED(wic_->CreateFormatConverter(converter.GetAddressOf())) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        return nullptr;
    target_->CreateBitmapFromWicBitmap(converter.Get(), nullptr, slot.GetAddressOf());
    return slot.Get();
}

void Canvas::Wordmark(const D2D1_RECT_F& r) {
    if (auto bitmap = Logo(kImageWordmark, static_cast<int>(std::lround(Width(r) * scale_)), static_cast<int>(std::lround(Height(r) * scale_))))
        target_->DrawBitmap(bitmap, r, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

// The app icon: the logo on a black tile with a hairline edge, as on the exe and in the tray.
void Canvas::AppIcon(const D2D1_RECT_F& r) {
    Fill(r, color::Canvas, metric::RadiusMd);
    Stroke(r, color::Hairline, metric::RadiusMd);
    int pixels = static_cast<int>(std::lround(Width(r) * scale_));
    if (auto bitmap = Logo(kImageLogo, pixels, pixels)) target_->DrawBitmap(bitmap, r, 1.f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

}  // namespace xww::ui
