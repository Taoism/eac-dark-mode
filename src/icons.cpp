// icons.cpp - flat vector icons for EAC's main-window image buttons in dark mode (GDI+, anti-aliased).
// EAC's own icons are 3D grey Windows 98 style bitmaps that look washed out on a dark background.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus { using std::min; using std::max; }
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

namespace {

ULONG_PTR g_token;
bool g_ready, g_failed;

bool Ready()
{
    if (g_ready) return true;
    if (g_failed) return false;
    GdiplusStartupInput in;
    g_ready = GdiplusStartup(&g_token, &in, nullptr) == Ok;   // never from DllMain: first paint only
    g_failed = !g_ready;
    return g_ready;
}

const Color kBg(32, 32, 32), kHot(58, 58, 58), kPress(74, 74, 74);
Color g_under(32, 32, 32);   // colour behind the glyph (changes on hover/press): used for cut-outs

void Poly(Graphics& g, const Brush& b, std::initializer_list<PointF> pts)
{
    g.FillPolygon(&b, pts.begin(), (INT)pts.size());
}

void RoundRectPath(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r)
{
    REAL d = r * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

// ---- 26x26 transport / metadata glyphs, drawn in a 26 unit box centred in the button
void Transport(Graphics& g, int id, const Color& c)
{
    SolidBrush b(c);
    switch (id) {
    case 506: Poly(g, b, { {9, 7}, {9, 19}, {19.5f, 13} }); break;                        // play
    case 508: g.FillRectangle(&b, 8.f, 7.5f, 3.5f, 11.f); g.FillRectangle(&b, 14.5f, 7.5f, 3.5f, 11.f); break; // pause
    case 507: { GraphicsPath p; RoundRectPath(p, 8, 8, 10, 10, 1.5f); g.FillPath(&b, &p); break; } // stop
    case 512: g.FillRectangle(&b, 7.f, 8.f, 2.2f, 10.f); Poly(g, b, { {19, 8}, {19, 18}, {10, 13} }); break;   // previous track
    case 511: g.FillRectangle(&b, 16.8f, 8.f, 2.2f, 10.f); Poly(g, b, { {7, 8}, {7, 18}, {16, 13} }); break;   // next track
    case 510: Poly(g, b, { {13, 8}, {13, 18}, {6, 13} }); Poly(g, b, { {20, 8}, {20, 18}, {13, 13} }); break;  // fast backward
    case 509: Poly(g, b, { {6, 8}, {6, 18}, {13, 13} }); Poly(g, b, { {13, 8}, {13, 18}, {20, 13} }); break;  // fast forward
    case 513: Poly(g, b, { {6.5f, 15}, {19.5f, 15}, {13, 7.5f} }); g.FillRectangle(&b, 6.5f, 17.f, 13.f, 2.5f); break; // eject
    }
}

void Cloud(Graphics& g, const Color& c, bool up)
{
    SolidBrush b(c);
    GraphicsPath p;
    p.AddEllipse(3.5f, 10.5f, 9.f, 9.f);
    p.AddEllipse(8.f, 5.5f, 11.f, 11.f);
    p.AddEllipse(14.5f, 10.5f, 8.f, 8.f);
    p.AddRectangle(RectF(8.f, 13.f, 10.f, 6.5f));
    p.SetFillMode(FillModeWinding);
    g.FillPath(&b, &p);
    // arrow cut out of the cloud
    SolidBrush hole(g_under);
    Pen stem(g_under, 2.2f);
    if (up) {
        g.DrawLine(&stem, 13.f, 18.f, 13.f, 11.5f);
        Poly(g, hole, { {9.5f, 13}, {16.5f, 13}, {13, 9} });
    } else {
        g.DrawLine(&stem, 13.f, 9.f, 13.f, 15.f);
        Poly(g, hole, { {9.5f, 14}, {16.5f, 14}, {13, 18} });
    }
}

// ---- 56x56 action buttons: disc -> file (extract) or file -> disc (write)
void Disc(Graphics& g, REAL cx, REAL cy, REAL r, const Color& face, const Color& ring)
{
    SolidBrush f(face);
    g.FillEllipse(&f, cx - r, cy - r, r * 2, r * 2);
    Pen sheen(Color(70, 255, 255, 255), 1.2f);
    g.DrawArc(&sheen, cx - r * 0.7f, cy - r * 0.7f, r * 1.4f, r * 1.4f, 200, 70);
    SolidBrush rg(ring);
    REAL rr = r * 0.42f;
    g.FillEllipse(&rg, cx - rr, cy - rr, rr * 2, rr * 2);
    SolidBrush hole(g_under);
    REAL hr = r * 0.17f;
    g.FillEllipse(&hole, cx - hr, cy - hr, hr * 2, hr * 2);
}

void File(Graphics& g, REAL x, REAL y, REAL w, REAL h, const Color& line)
{
    REAL fold = 6;
    GraphicsPath p;
    p.AddLine(x, y, x + w - fold, y);
    p.AddLine(x + w - fold, y, x + w, y + fold);
    p.AddLine(x + w, y + fold, x + w, y + h);
    p.AddLine(x + w, y + h, x, y + h);
    p.CloseFigure();
    SolidBrush f(Color(52, 52, 52));
    g.FillPath(&f, &p);
    Pen pen(line, 1.3f);
    g.DrawPath(&pen, &p);
    g.DrawLine(&pen, x + w - fold, y, x + w - fold, y + fold);
    g.DrawLine(&pen, x + w - fold, y + fold, x + w, y + fold);
}

void Arrow(Graphics& g, REAL x1, REAL y1, REAL x2, REAL y2, const Color& c)
{
    Pen pen(c, 2.6f);
    AdjustableArrowCap cap(3.2f, 3.2f, TRUE);
    pen.SetCustomEndCap(&cap);
    pen.SetStartCap(LineCapRound);
    g.DrawLine(&pen, x1, y1, x2, y2);
}

void Badge(Graphics& g, REAL x, REAL y, REAL w, REAL h, const wchar_t* text, const Color& fill)
{
    GraphicsPath p;
    RoundRectPath(p, x, y, w, h, 3);
    SolidBrush f(fill);
    g.FillPath(&f, &p);
    FontFamily fam(L"Segoe UI");
    Font font(&fam, 9.5f, FontStyleBold, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(StringAlignmentCenter);
    sf.SetLineAlignment(StringAlignmentCenter);
    SolidBrush tb(Color(24, 24, 24));
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.DrawString(text, -1, &font, RectF(x, y + 0.5f, w, h), &sf, &tb);
}

void Action(Graphics& g, int id, bool en)
{
    Color accent;
    const wchar_t* label;
    switch (id) {
    case 502: accent = Color(96, 165, 250); label = L"WAV"; break;
    case 770: accent = Color(110, 200, 125); label = L"CMP"; break;
    case 466: accent = Color(240, 172, 82); label = L"IMG"; break;
    default:  accent = Color(240, 112, 112); label = L"WRI"; break;
    }
    Color discFace = en ? Color(150, 156, 166) : Color(78, 80, 84);
    Color discRing = en ? Color(196, 200, 208) : Color(96, 98, 102);
    Color fileLine = en ? Color(200, 200, 200) : Color(100, 100, 100);
    if (!en) accent = Color(96, 96, 96);
    if (id == 574) {   // write: cue sheet file -> disc
        File(g, 6, 6, 20, 25, fileLine);
        Arrow(g, 11, 13, 20.5f, 23, accent);
        Disc(g, 38, 38, 15, discFace, discRing);
        SolidBrush ab(accent);
        g.FillEllipse(&ab, 34.5f, 34.5f, 7.f, 7.f);   // accent hub: "burn"
        SolidBrush hole(g_under);
        g.FillEllipse(&hole, 36.4f, 36.4f, 3.2f, 3.2f);
        Badge(g, 3, 40, 26, 13, label, accent);
    } else {           // extract: disc -> file
        Disc(g, 18, 18, 15, discFace, discRing);
        File(g, 30, 20, 21, 27, fileLine);
        Arrow(g, 40.5f, 25, 40.5f, 37, accent);
        Badge(g, 26, 40, 28, 13, label, accent);
    }
}

} // namespace

bool HasEacIcon(int id)
{
    switch (id) { case 502: case 770: case 466: case 574: case 506: case 507: case 508: case 509:
    case 510: case 511: case 512: case 513: case 525: case 544: return true; }
    return false;
}

// Returns true when it drew the icon for this control id (state: 0 normal, 1 hot, 2 pressed, 3 disabled).
bool DrawEacIcon(HDC dc, int w, int h, int id, int state)
{
    bool transport = id == 506 || id == 507 || id == 508 || id == 509 || id == 510 || id == 511 || id == 512 || id == 513;
    bool meta = id == 525 || id == 544;
    bool action = id == 502 || id == 770 || id == 466 || id == 574;
    if (!transport && !meta && !action) return false;
    if (!Ready()) return false;
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    SolidBrush bg(kBg);
    g.FillRectangle(&bg, 0, 0, w, h);
    g_under = state == 2 ? kPress : state == 1 ? kHot : kBg;
    if (state == 1 || state == 2) {
        GraphicsPath p;
        RoundRectPath(p, 0.5f, 0.5f, (REAL)w - 1, (REAL)h - 1, action ? 5.f : 4.f);
        SolidBrush hb(state == 2 ? kPress : kHot);
        g.FillPath(&hb, &p);
    }
    bool en = state != 3;
    // centre the design box in the button (transport/meta designs are 26 units, actions 56)
    REAL box = action ? 56.f : 26.f;
    REAL s = std::min(w, h) / box;
    g.TranslateTransform((w - box * s) / 2, (h - box * s) / 2);
    g.ScaleTransform(s, s);
    if (state == 2) g.TranslateTransform(0.5f, 0.5f);
    Color glyph = !en ? Color(92, 92, 92) : state ? Color(235, 235, 235) : Color(150, 150, 150);   // muted until hovered
    if (transport) Transport(g, id, glyph);
    else if (meta) Cloud(g, !en ? glyph : Color(110, 175, 250), id == 544);
    else Action(g, id, en);
    return true;
}
