//  Thin Direct2D / DirectWrite layer: rounded rectangles and hairlines, the
//  shadow of the primary button, text in the bundled Geist families with the
//  type scale's tracking, Lucide icons stroked from path data, and the logos.
//  Works on any ID2D1RenderTarget, so the window and --render (WIC bitmap)
//  share every drawing call. All units are DIPs.
#pragma once

#include "ui/theme.h"

#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <map>
#include <string>
#include <unordered_map>

namespace xww::ui {

using Microsoft::WRL::ComPtr;

enum class Align { Leading, Center, Trailing };

struct TextStyle {
    Type type = Type::Body;
    bool wrap = false;
    Align align = Align::Leading;
    bool centerVertically = true;
};

struct TypeSpec { float size, line; DWRITE_FONT_WEIGHT weight; float tracking; bool mono; };
TypeSpec Spec(Type type);

inline TextStyle Style(Type type, Align align = Align::Leading) { TextStyle s; s.type = type; s.align = align; return s; }
inline TextStyle Wrapped(Type type) { TextStyle s; s.type = type; s.wrap = true; s.centerVertically = false; return s; }

inline D2D1_RECT_F Rect(float x, float y, float w, float h) { return { x, y, x + w, y + h }; }
inline float Width(const D2D1_RECT_F& r) { return r.right - r.left; }
inline float Height(const D2D1_RECT_F& r) { return r.bottom - r.top; }
inline bool Inside(const D2D1_RECT_F& r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
inline D2D1_RECT_F Inflate(const D2D1_RECT_F& r, float d) { return { r.left - d, r.top - d, r.right + d, r.bottom + d }; }

class Canvas {
public:
    bool Init();                                            // factories and fonts; once per process
    void Begin(ID2D1RenderTarget* target, float scale);     // the target is owned by the caller; scale = DPI / 96
    void End() { target_ = nullptr; }
    void ReleaseDeviceResources();                          // the target was recreated
    ID2D1RenderTarget* Target() const { return target_; }

    void Clear(D2D1_COLOR_F color) { target_->Clear(color); }
    void Fill(const D2D1_RECT_F& r, D2D1_COLOR_F color, float radius = 0);
    void Stroke(const D2D1_RECT_F& r, D2D1_COLOR_F color, float radius = 0, float width = 1);
    void Circle(D2D1_POINT_2F center, float radius, D2D1_COLOR_F color);
    void CtaShadow(const D2D1_RECT_F& r);                   // shadow-cta, the one shadow in the system

    void Text(const std::wstring& text, const D2D1_RECT_F& r, D2D1_COLOR_F color, const TextStyle& style);
    D2D1_SIZE_F Measure(const std::wstring& text, float maxWidth, const TextStyle& style);

    void Icon(const char* path, D2D1_POINT_2F topLeft, float size, D2D1_COLOR_F color);
    void AppIcon(const D2D1_RECT_F& r);                     // branding/logo.png on a canvas tile
    void Wordmark(const D2D1_RECT_F& r);                    // branding/logotext.png

    void PushClip(const D2D1_RECT_F& r) { target_->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED); }
    void PopClip() { target_->PopAxisAlignedClip(); }
    void Translate(float dx, float dy) { target_->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy)); }
    void ResetTransform() { target_->SetTransform(D2D1::Matrix3x2F::Identity()); }

    ID2D1Factory* Factory() const { return factory_.Get(); }
    IWICImagingFactory* Wic() const { return wic_.Get(); }

private:
    IDWriteTextFormat* Format(const TextStyle& style);
    ComPtr<IDWriteTextLayout> Layout(const std::wstring& text, const TextStyle& style, float width, float height);
    ID2D1PathGeometry* Geometry(const char* path);
    ID2D1SolidColorBrush* Brush(D2D1_COLOR_F color);
    ID2D1Bitmap* Logo(int resource, int width, int height);
    void LoadFonts();

    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> write_;
    ComPtr<IDWriteFontCollection> fonts_;                   // Geist and Geist Mono from the resources, or null
    ComPtr<IWICImagingFactory> wic_;
    ComPtr<ID2D1StrokeStyle> round_;
    ID2D1RenderTarget* target_ = nullptr;
    float scale_ = 1;
    ComPtr<ID2D1SolidColorBrush> brush_;
    std::wstring textFamily_, monoFamily_;
    std::map<std::tuple<int, int, int, int>, ComPtr<IDWriteTextFormat>> formats_;
    std::unordered_map<const char*, ComPtr<ID2D1PathGeometry>> geometries_;
    std::map<std::pair<int, int>, ComPtr<ID2D1Bitmap>> logos_;   // resource, pixel width
};

}  // namespace xww::ui
