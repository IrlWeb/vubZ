#pragma once

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

#include <Dependencies/imgui/addons/imgui_notify.h>
#include <Ui/Overlay/ToastFonts.hpp>
#include <Ui/Overlay/ToastLogo.hpp>

namespace ToastStyle
{

    constexpr float kCardWidth   = 312.0f;
    constexpr float kRadius      = 8.0f;
    constexpr float kPad         = 12.0f;
    constexpr float kBadge       = 30.0f;
    constexpr float kBadgeRadius = 7.0f;
    constexpr float kGap         = 8.0f;
    constexpr float kMinHeight   = kBadge + kPad * 2.0f;
    constexpr float kProgress    = 2.0f;

    constexpr float kTextX       = kPad + kBadge + 11.0f;
    constexpr float kTextWidth   = kCardWidth - kTextX - 14.0f;
    constexpr float kTextTop     = 9.0f;
    constexpr float kTitleGap    = -2.0f;
    constexpr float kTextBottom  = 12.0f;
    constexpr int   kBodyLines   = 2;

    constexpr float kTitleSize   = 13.0f;
    constexpr float kBodySize    = 11.5f;

    constexpr int   kShadowPad    = 18;
    constexpr int   kShadowLayers = 10;
    constexpr float kShadowDepth  = 4.0f;
    constexpr int   kScreenMargin = 16;

    constexpr int   kWindowWidth  = static_cast<int>(kCardWidth) + kShadowPad * 2;
    constexpr int   kWindowHeight = 560;

    constexpr int   kEnterMs = 280;
    constexpr int   kExitMs  = 200;
    constexpr float kSlide   = 28.0f;

    inline float EaseOutQuart(float t)
    {
        t = (std::clamp)(t, 0.0f, 1.0f);
        const float inv = 1.0f - t;
        return 1.0f - inv * inv * inv * inv;
    }

    inline float EaseInCubic(float t)
    {
        t = (std::clamp)(t, 0.0f, 1.0f);
        return t * t * t;
    }

    struct Rgb { BYTE r, g, b; };

    constexpr Rgb kRail        { 0x0A, 0x07, 0x0D };
    constexpr Rgb kStroke      { 0x27, 0x1D, 0x30 };
    constexpr Rgb kStrokeSoft  { 0x16, 0x10, 0x1B };
    constexpr Rgb kTextBright  { 0xF3, 0xE9, 0xF7 };
    constexpr Rgb kTextMuted   { 0xA9, 0x9B, 0xB1 };

    struct Tone { Rgb fill, line, text; };

    inline Tone ToneFor(ImGuiToastType type)
    {
        switch (type)
        {
        case ImGuiToastType::Success:
            return { { 0x1A, 0x0D, 0x24 }, { 0x3A, 0x1C, 0x4C }, { 0xA8, 0x55, 0xF7 } };
        case ImGuiToastType::Error:
            return { { 0x2E, 0x10, 0x20 }, { 0x6B, 0x2E, 0x3A }, { 0xF8, 0x71, 0x71 } };
        case ImGuiToastType::Info:
            return { { 0x1A, 0x0D, 0x24 }, { 0x3A, 0x1C, 0x4C }, { 0xA8, 0x55, 0xF7 } };
        case ImGuiToastType::Warning:
            return { { 0x2A, 0x1F, 0x0A }, { 0x6B, 0x52, 0x20 }, { 0xFB, 0xBF, 0x24 } };
        default:
            return { { 0x18, 0x12, 0x1E }, { 0x27, 0x1D, 0x30 }, { 0xA9, 0x9B, 0xB1 } };
        }
    }

    inline BYTE Alpha(float opacity)
    {
        return static_cast<BYTE>((std::clamp)(opacity, 0.0f, 1.0f) * 255.0f + 0.5f);
    }

    inline Gdiplus::Color Argb(float opacity, Rgb c)
    {
        return Gdiplus::Color(Alpha(opacity), c.r, c.g, c.b);
    }

    class Fonts
    {
    public:
        bool Load()
        {
            m_collection = std::make_unique<Gdiplus::PrivateFontCollection>();
            m_collection->AddMemoryFont(ToastFonts::kPoppinsRegular,
                static_cast<INT>(ToastFonts::kPoppinsRegularSize));
            m_collection->AddMemoryFont(ToastFonts::kPoppinsSemiBold,
                static_cast<INT>(ToastFonts::kPoppinsSemiBoldSize));

            const INT count = m_collection->GetFamilyCount();
            m_families.reset(new Gdiplus::FontFamily[count > 0 ? count : 1]);
            INT found = 0;
            if (count > 0)
                m_collection->GetFamilies(count, m_families.get(), &found);

            m_fallback = std::make_unique<Gdiplus::FontFamily>(L"Segoe UI");
            const Gdiplus::FontFamily* titleFamily = m_fallback.get();
            const Gdiplus::FontFamily* bodyFamily = m_fallback.get();
            INT titleStyle = Gdiplus::FontStyleBold;

            for (INT i = 0; i < found; ++i)
            {
                WCHAR name[LF_FACESIZE] = {};
                m_families[i].GetFamilyName(name);

                if (wcscmp(name, L"Poppins SemiBold") == 0)
                {
                    titleFamily = &m_families[i];
                    titleStyle = Gdiplus::FontStyleRegular;
                }
                else if (wcscmp(name, L"Poppins") == 0)
                {
                    bodyFamily = &m_families[i];
                }
            }

            m_title = std::make_unique<Gdiplus::Font>(titleFamily, kTitleSize, titleStyle, Gdiplus::UnitPixel);
            m_body = std::make_unique<Gdiplus::Font>(bodyFamily, kBodySize, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

            return titleFamily != m_fallback.get() && bodyFamily != m_fallback.get();
        }

        const Gdiplus::Font& Title() const { return *m_title; }
        const Gdiplus::Font& Body() const { return *m_body; }

    private:
        std::unique_ptr<Gdiplus::PrivateFontCollection> m_collection;
        std::unique_ptr<Gdiplus::FontFamily[]> m_families;
        std::unique_ptr<Gdiplus::FontFamily> m_fallback;
        std::unique_ptr<Gdiplus::Font> m_title;
        std::unique_ptr<Gdiplus::Font> m_body;
    };

    class Logo
    {
    public:
        bool Load()
        {
            if (m_bitmap)
                return true;

            IStream* stream = SHCreateMemStream(ToastLogo::kSupqLogoPng, ToastLogo::kSupqLogoPngSize);
            if (!stream)
                return false;

            auto temp = std::make_unique<Gdiplus::Bitmap>(stream);
            if (temp && temp->GetLastStatus() == Gdiplus::Ok)
            {
                m_bitmap.reset(temp->Clone(0, 0, temp->GetWidth(), temp->GetHeight(), PixelFormat32bppPARGB));
            }
            stream->Release();

            return m_bitmap && m_bitmap->GetLastStatus() == Gdiplus::Ok;
        }

        Gdiplus::Bitmap* Bitmap() const { return m_bitmap.get(); }

    private:
        std::unique_ptr<Gdiplus::Bitmap> m_bitmap;
    };

    inline void AddRoundRect(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float r)
    {
        const float d = r * 2.0f;
        path.AddArc(x, y, d, d, 180.0f, 90.0f);
        path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
        path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
        path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    }

    inline float MeasureCardHeight(Gdiplus::Graphics& g, const Fonts& fonts, const std::wstring& content)
    {
        if (content.empty())
            return kMinHeight;

        const float titleH = fonts.Title().GetHeight(&g);
        const float lineH = fonts.Body().GetHeight(&g);

        Gdiplus::StringFormat wrap;
        wrap.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);

        Gdiplus::RectF layout(0.0f, 0.0f, kTextWidth, lineH * kBodyLines);
        Gdiplus::RectF bounds;
        g.MeasureString(content.c_str(), -1, &fonts.Body(), layout, &wrap, &bounds);

        const float bodyH = (std::min)(bounds.Height, lineH * kBodyLines);
        const float h = kTextTop + titleH + kTitleGap + bodyH + kTextBottom;
        return (std::max)(kMinHeight, std::ceil(h));
    }

    inline void PaintGlyph(Gdiplus::Graphics& g, ImGuiToastType type, float bx, float by, const Gdiplus::Color& color)
    {
        Gdiplus::Pen pen(color, 1.7f);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::SolidBrush fill(color);

        auto at = [&](float px, float py) { return Gdiplus::PointF(bx + px, by + py); };
        auto dot = [&](float cx, float cy, float r) {
            g.FillEllipse(&fill, bx + cx - r, by + cy - r, r * 2.0f, r * 2.0f);
        };

        switch (type)
        {
        case ImGuiToastType::Success:
        {
            const Gdiplus::PointF sPts[] = { at(14.0f, 11.5f), at(9.5f, 11.5f), at(9.5f, 15.0f), at(14.0f, 15.0f), at(14.0f, 18.5f), at(9.5f, 18.5f) };
            g.DrawLines(&pen, sPts, 6);
            g.DrawRectangle(&pen, bx + 16.0f, by + 11.5f, 5.0f, 7.0f);
            g.DrawLine(&pen, at(19.0f, 17.0f), at(22.0f, 20.0f));
            break;
        }
        case ImGuiToastType::Error:
            g.DrawLine(&pen, at(11.0f, 11.0f), at(19.0f, 19.0f));
            g.DrawLine(&pen, at(19.0f, 11.0f), at(11.0f, 19.0f));
            break;
        case ImGuiToastType::Warning:
            g.DrawLine(&pen, at(15.0f, 9.5f), at(15.0f, 16.3f));
            dot(15.0f, 20.4f, 1.2f);
            break;
        case ImGuiToastType::Info:
            dot(15.0f, 9.8f, 1.2f);
            g.DrawLine(&pen, at(15.0f, 13.6f), at(15.0f, 20.5f));
            break;
        default:
            dot(15.0f, 15.0f, 3.0f);
            break;
        }
    }

    inline void PaintCard(Gdiplus::Graphics& g, const Fonts& fonts, const Logo* logo,
        const std::wstring& title, const std::wstring& content, ImGuiToastType type,
        float x, float y, float h, float opacity, float progress)
    {
        const Tone tone = ToneFor(type);
        const float w = kCardWidth;

        for (int k = kShadowLayers; k >= 1; --k)
        {
            const float spread = static_cast<float>(k) * 1.3f;
            const float falloff = 1.0f - static_cast<float>(k - 1) / static_cast<float>(kShadowLayers);
            Gdiplus::GraphicsPath ring;
            AddRoundRect(ring, x - spread, y - spread + kShadowDepth,
                w + spread * 2.0f, h + spread * 2.0f, kRadius + spread);
            Gdiplus::SolidBrush brush(Gdiplus::Color(Alpha(opacity * 0.075f * falloff * falloff), 0, 0, 0));
            g.FillPath(&brush, &ring);
        }

        Gdiplus::GraphicsPath card;
        AddRoundRect(card, x, y, w, h, kRadius);

        Gdiplus::SolidBrush surface(Argb(opacity * 0.98f, kRail));
        g.FillPath(&surface, &card);

        g.SetClip(&card);
        {
            const float washW = w * 0.62f;
            Gdiplus::LinearGradientBrush wash(
                Gdiplus::PointF(x - 1.0f, y), Gdiplus::PointF(x + washW + 1.0f, y),
                Argb(opacity * 0.95f, tone.fill), Argb(0.0f, tone.fill));
            g.FillRectangle(&wash, x, y, washW, h);

            Gdiplus::SolidBrush track(Argb(opacity, kStrokeSoft));
            g.FillRectangle(&track, x, y + h - kProgress, w, kProgress);

            if (progress > 0.0f)
            {
                Gdiplus::SolidBrush bar(Argb(opacity * 0.9f, tone.text));
                g.FillRectangle(&bar, x, y + h - kProgress, w * (std::min)(progress, 1.0f), kProgress);
            }
        }
        g.ResetClip();

        Gdiplus::GraphicsPath edge;
        AddRoundRect(edge, x + 0.5f, y + 0.5f, w - 1.0f, h - 1.0f, kRadius - 0.5f);
        Gdiplus::Pen border(Argb(opacity, kStroke), 1.0f);
        g.DrawPath(&border, &edge);

        const float bx = x + kPad;
        const float by = y + kPad;

        Gdiplus::GraphicsPath badge;
        AddRoundRect(badge, bx, by, kBadge, kBadge, kBadgeRadius);

        if (type == ImGuiToastType::Success && logo && logo->Bitmap())
        {
            Gdiplus::ImageAttributes attr;
            Gdiplus::ColorMatrix matrix = {
                1.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 0.0f, opacity, 0.0f,
                0.0f, 0.0f, 0.0f, 0.0f, 1.0f
            };
            attr.SetColorMatrix(&matrix, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);

            g.DrawImage(logo->Bitmap(), Gdiplus::RectF(bx, by, kBadge, kBadge),
                0.0f, 0.0f, static_cast<float>(logo->Bitmap()->GetWidth()), static_cast<float>(logo->Bitmap()->GetHeight()),
                Gdiplus::UnitPixel, &attr);

            Gdiplus::GraphicsPath badgeEdge;
            AddRoundRect(badgeEdge, bx + 0.5f, by + 0.5f, kBadge - 1.0f, kBadge - 1.0f, kBadgeRadius - 0.5f);
            Gdiplus::Pen badgeLine(Argb(opacity, tone.line), 1.0f);
            g.DrawPath(&badgeLine, &badgeEdge);
        }
        else
        {
            Gdiplus::SolidBrush badgeFill(Argb(opacity, tone.fill));
            g.FillPath(&badgeFill, &badge);

            Gdiplus::GraphicsPath badgeEdge;
            AddRoundRect(badgeEdge, bx + 0.5f, by + 0.5f, kBadge - 1.0f, kBadge - 1.0f, kBadgeRadius - 0.5f);
            Gdiplus::Pen badgeLine(Argb(opacity, tone.line), 1.0f);
            g.DrawPath(&badgeLine, &badgeEdge);

            PaintGlyph(g, type, bx, by, Argb(opacity, tone.text));
        }

        const float tx = x + kTextX;
        Gdiplus::SolidBrush titleBrush(Argb(opacity, kTextBright));

        Gdiplus::StringFormat single;
        single.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
        single.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

        if (content.empty())
        {
            single.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::RectF rect(tx, by + 1.0f, kTextWidth, kBadge);
            g.DrawString(title.c_str(), -1, &fonts.Title(), rect, &single, &titleBrush);
            return;
        }

        const float titleH = fonts.Title().GetHeight(&g);
        const float lineH = fonts.Body().GetHeight(&g);

        Gdiplus::RectF titleRect(tx, y + kTextTop, kTextWidth, titleH);
        g.DrawString(title.c_str(), -1, &fonts.Title(), titleRect, &single, &titleBrush);

        Gdiplus::StringFormat wrap;
        wrap.SetTrimming(Gdiplus::StringTrimmingEllipsisWord);

        Gdiplus::SolidBrush bodyBrush(Argb(opacity, kTextMuted));
        Gdiplus::RectF bodyRect(tx, y + kTextTop + titleH + kTitleGap, kTextWidth, lineH * kBodyLines);
        g.DrawString(content.c_str(), -1, &fonts.Body(), bodyRect, &wrap, &bodyBrush);
    }

    inline void PaintCard(Gdiplus::Graphics& g, const Fonts& fonts,
        const std::wstring& title, const std::wstring& content, ImGuiToastType type,
        float x, float y, float h, float opacity, float progress)
    {
        PaintCard(g, fonts, nullptr, title, content, type, x, y, h, opacity, progress);
    }
}
