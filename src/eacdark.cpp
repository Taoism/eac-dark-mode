// eacdark - dark mode for Exact Audio Copy, loaded as an msacm32.dll proxy.
//
// How it works:
//  * A per-thread WH_CBT hook subclasses every window as it is created.
//  * Controls get the Windows dark visual styles (DarkMode_Explorer / DarkMode_CFD / ItemsView),
//    parents answer WM_CTLCOLOR* with dark brushes, and controls without a dark theme
//    (checkbox/radio text, group boxes, tabs, status bar, menu bar) are painted here.
//  * GetSysColor / GetSysColorBrush are hooked in EAC.exe and comctl32 so EAC's own drawing
//    and comctl32's classic parts (grid lines, borders) follow the dark palette.
//  * Owner-drawn EAC buttons are rendered into a buffer and their grey background / black
//    glyphs are recoloured.
//  * Tools > "Dark mode" toggles everything live; the choice is stored in eacdark.ini.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <richedit.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <commdlg.h>
#include <shlobj.h>
#include <oleacc.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cwchar>
#include <cstdio>

#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")

bool ProxyInit();
bool HasEacIcon(int id);                                       // icons.cpp
bool DrawEacIcon(HDC dc, int w, int h, int id, int state);

// ---------------------------------------------------------------- palette
static const COLORREF C_BG     = RGB(32, 32, 32);    // window / dialog background
static const COLORREF C_CTRL   = RGB(43, 43, 43);    // edit / combo / listbox background
static const COLORREF C_LIST   = RGB(25, 25, 25);    // list view / log background
static const COLORREF C_TEXT   = RGB(230, 230, 230);
static const COLORREF C_DIS    = RGB(128, 128, 128);
static const COLORREF C_DISLBL = RGB(165, 165, 165);   // disabled text labels: dimmed but readable
static const COLORREF C_BORDER = RGB(70, 70, 70);
static const COLORREF C_HOT    = RGB(61, 61, 61);
static const COLORREF C_PRESS  = RGB(80, 80, 80);
static const COLORREF C_TABSEL = RGB(48, 48, 48);
static const UINT     ID_DARKMODE = 0xDA7A;
static const UINT     ID_REDRAW   = 0xDA7B;   // posted to ourselves after a toggle

// ---------------------------------------------------------------- globals
static HINSTANCE g_hInst;
static volatile bool g_dark = true;
static bool g_startedDark = true;   // EAC caches colours at startup: light mode then needs undoing
static bool g_debug = false;
static int g_stage = 9;   // debug: 0 proxy only, 1 CBT hook only, 2 + subclass, 9 full
static wchar_t g_ini[MAX_PATH];
static wchar_t g_log[MAX_PATH];
static INIT_ONCE g_initOnce = INIT_ONCE_STATIC_INIT;
static DWORD g_tlsHook = TLS_OUT_OF_INDEXES;
static thread_local int t_suppress = 0;
static thread_local std::vector<HWND> t_pending;   // WinForms windows waiting to be subclassed   // >0 while a common dialog runs on this thread

using fnAllowDarkModeForWindow = bool (WINAPI*)(HWND, bool);
using fnSetPreferredAppMode    = int  (WINAPI*)(int);
using fnVoid                   = void (WINAPI*)();
static fnAllowDarkModeForWindow pAllowDarkModeForWindow;
static fnSetPreferredAppMode    pSetPreferredAppMode;
static fnVoid pFlushMenuThemes, pRefreshImmersiveColorPolicyState;

using fnGetSysColor      = DWORD  (WINAPI*)(int);
using fnGetSysColorBrush = HBRUSH (WINAPI*)(int);
static fnGetSysColor      real_GetSysColor;
using fnFillRect = int (WINAPI*)(HDC, const RECT*, HBRUSH);
static fnFillRect real_FillRect;
static bool OurSysBrush(HBRUSH b, int& idx);
static fnGetSysColorBrush real_GetSysColorBrush;

static HBRUSH g_brBg, g_brCtrl, g_brList, g_brBorder, g_brHot, g_brPress, g_brTabSel, g_brFocus;
static HBRUSH g_sysBrush[64];
static COLORREF UnmapColor(COLORREF c, int textIdx);

static void Log(const wchar_t* fmt, ...)
{
    if (!g_debug) return;
    wchar_t buf[1024];
    va_list ap; va_start(ap, fmt); _vsnwprintf_s(buf, _TRUNCATE, fmt, ap); va_end(ap);
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_log, L"a, ccs=UTF-8") == 0 && f) { fwprintf(f, L"[%lu] %s\n", GetCurrentThreadId(), buf); fclose(f); }
}

// ---------------------------------------------------------------- window registry
enum Kind {
    K_GENERIC, K_BUTTON, K_EDIT, K_COMBO, K_COMBOLBOX, K_LISTBOX, K_LISTVIEW, K_HEADER, K_TREE,
    K_TAB, K_STATUS, K_TOOLBAR, K_TRACKBAR, K_PROGRESS, K_RICHEDIT, K_TOOLTIP, K_SCROLLBAR,
    K_STATIC, K_UPDOWN, K_REBAR, K_FOREIGN
};

struct WinInfo {
    WNDPROC orig = nullptr;
    Kind kind = K_GENERIC;
    bool top = false;          // top-level window with a caption
    bool applying = false;     // our own messages pass straight through
    bool lvSaved = false;
    bool wf = false;           // .NET WinForms window (metadata plugins)
    int role = 0;              // WinForms owner-drawn button: MSAA role, -1 unknown
    COLORREF lvBk = CLR_DEFAULT, lvTextBk = CLR_DEFAULT, lvText = CLR_DEFAULT;
    HIMAGELIST lvImgOrig = nullptr, lvImgDark = nullptr;   // list view small icons: EAC's and our dark copy
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::unordered_map<HWND, WinInfo>* g_wins;
static std::unordered_set<HWND>* g_foreign;   // top-level windows hosting shell UI: leave alone

static bool GetInfo(HWND h, WinInfo& out)
{
    AcquireSRWLockShared(&g_lock);
    auto it = g_wins->find(h);
    bool ok = it != g_wins->end();
    if (ok) out = it->second;
    ReleaseSRWLockShared(&g_lock);
    return ok;
}

static void SetApplying(HWND h, bool v)
{
    AcquireSRWLockExclusive(&g_lock);
    auto it = g_wins->find(h);
    if (it != g_wins->end()) it->second.applying = v;
    ReleaseSRWLockExclusive(&g_lock);
}

static bool IsForeignRoot(HWND h)
{
    HWND root = GetAncestor(h, GA_ROOT);
    AcquireSRWLockShared(&g_lock);
    bool f = g_foreign->count(root) != 0;
    ReleaseSRWLockShared(&g_lock);
    return f;
}

static bool IsWinFormsClass(HWND h)
{
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    return !_wcsnicmp(cls, L"WindowsForms10.", 15);
}

static Kind KindFromClass(HWND h)
{
    wchar_t cls[128] = {}, real[128] = {};
    GetClassNameW(h, cls, 128);
    RealGetWindowClassW(h, real, 128);
    // EAC superclasses system controls as mybutton/myedit/mycombo/mylistbox; RealGetWindowClass
    // doesn't resolve those, so map them by name.
    static const struct { const wchar_t* my; const wchar_t* sys; } alias[] = {
        { L"mybutton", L"Button" }, { L"myedit", L"Edit" }, { L"mycombo", L"ComboBox" },
        { L"mylistbox", L"ListBox" }, { L"mystatic", L"Static" } };
    for (auto& a : alias) if (!_wcsicmp(cls, a.my)) { wcscpy_s(real, a.sys); break; }
    auto is = [&](const wchar_t* n) { return !_wcsicmp(cls, n) || !_wcsicmp(real, n); };
    // WinForms: WindowsForms10.<BASECLASS>.app.0.<id>
    if (!_wcsnicmp(cls, L"WindowsForms10.", 15)) {
        const wchar_t* b = cls + 15;
        const wchar_t* e = wcsstr(b, L".app.");
        size_t n = e ? (size_t)(e - b) : wcslen(b);
        if (n < 127) { wmemcpy(real, b, n); real[n] = 0; }
        if (!_wcsicmp(real, L"Window")) return K_GENERIC;
    }
    if (is(L"Button")) return K_BUTTON;
    if (is(L"Edit")) return K_EDIT;
    if (is(L"ComboBox") || is(L"ComboBoxEx32")) return K_COMBO;
    if (is(L"ComboLBox")) return K_COMBOLBOX;
    if (is(L"ListBox")) return K_LISTBOX;
    if (is(L"SysListView32")) return K_LISTVIEW;
    if (is(L"SysHeader32")) return K_HEADER;
    if (is(L"SysTreeView32")) return K_TREE;
    if (is(L"SysTabControl32")) return K_TAB;
    if (is(L"msctls_statusbar32")) return K_STATUS;
    if (is(L"ToolbarWindow32")) return K_TOOLBAR;
    if (is(L"msctls_trackbar32")) return K_TRACKBAR;
    if (is(L"msctls_progress32")) return K_PROGRESS;
    if (is(L"tooltips_class32")) return K_TOOLTIP;
    if (is(L"ScrollBar")) return K_SCROLLBAR;
    if (is(L"Static")) return K_STATIC;
    if (is(L"msctls_updown32")) return K_UPDOWN;
    if (is(L"ReBarWindow32")) return K_REBAR;
    if (!_wcsnicmp(cls, L"RichEdit", 8) || !_wcsnicmp(real, L"RichEdit", 8)) return K_RICHEDIT;
    return K_GENERIC;
}

// ---------------------------------------------------------------- colour helpers
static int Lum(COLORREF c) { return (GetRValue(c) * 299 + GetGValue(c) * 587 + GetBValue(c) * 114) / 1000; }
static bool Colorful(COLORREF c)
{
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    return std::max({ r, g, b }) - std::min({ r, g, b }) > 48;
}
static COLORREF Mix(COLORREF a, COLORREF b, int pctB)
{
    auto m = [&](int x, int y) { return (x * (100 - pctB) + y * pctB) / 100; };
    return RGB(m(GetRValue(a), GetRValue(b)), m(GetGValue(a), GetGValue(b)), m(GetBValue(a), GetBValue(b)));
}
static HBRUSH BrushFor(COLORREF c)
{
    if (c == C_CTRL) return g_brCtrl;
    if (c == C_LIST) return g_brList;
    return g_brBg;
}

static bool MapSysColor(int i, COLORREF& c)
{
    switch (i) {
    case COLOR_WINDOW:        c = C_CTRL; return true;
    case COLOR_WINDOWTEXT:    case COLOR_BTNTEXT: case COLOR_MENUTEXT:
    case COLOR_INFOTEXT:      case COLOR_CAPTIONTEXT: c = C_TEXT; return true;
    case COLOR_BTNFACE:       case COLOR_MENU: case COLOR_MENUBAR:
    case COLOR_APPWORKSPACE:  c = C_BG; return true;
    case COLOR_BTNSHADOW:     c = RGB(70, 70, 70); return true;
    case COLOR_3DDKSHADOW:    c = RGB(18, 18, 18); return true;
    case COLOR_BTNHIGHLIGHT:  c = RGB(85, 85, 85); return true;
    case COLOR_3DLIGHT:       c = RGB(60, 60, 60); return true;
    case COLOR_GRAYTEXT:      c = C_DIS; return true;
    case COLOR_INFOBK:        c = C_CTRL; return true;
    case COLOR_WINDOWFRAME:   c = C_BORDER; return true;
    default: return false;
    }
}

static DWORD WINAPI Hook_GetSysColor(int i)
{
    COLORREF c;
    if (g_dark && t_suppress == 0 && MapSysColor(i, c)) return c;
    return real_GetSysColor(i);
}

static HBRUSH WINAPI Hook_GetSysColorBrush(int i)
{
    COLORREF c;
    if (g_dark && t_suppress == 0 && i >= 0 && i < 64 && MapSysColor(i, c)) {
        if (!g_sysBrush[i]) g_sysBrush[i] = CreateSolidBrush(c);   // benign race: worst case one leaked brush
        return g_sysBrush[i];
    }
    return real_GetSysColorBrush(i);
}

// EAC fills its main window with a brush it created from GetSysColor at startup; after a live
// switch to light mode, swap such dark brushes for the real system brush.
static int WINAPI Hook_FillRect(HDC dc, const RECT* rc, HBRUSH b)
{
    int idx;
    if (!g_dark) {
        if (OurSysBrush(b, idx)) b = real_GetSysColorBrush(idx);
    } else if (t_suppress == 0 && b) {
        // EAC started in light mode and cached the real button-face brush: darken it.
        bool face = b == real_GetSysColorBrush(COLOR_BTNFACE) || (ULONG_PTR)b == COLOR_BTNFACE + 1;
        LOGBRUSH lb{};
        if (!face && (ULONG_PTR)b > 64 && GetObjectType(b) == OBJ_BRUSH && GetObjectW(b, sizeof lb, &lb) &&
            lb.lbStyle == BS_SOLID && lb.lbColor == real_GetSysColor(COLOR_BTNFACE)) face = true;
        if (face) b = Hook_GetSysColorBrush(COLOR_BTNFACE);
    }
    return real_FillRect(dc, rc, b);
}

// ---------------------------------------------------------------- common dialog guards
using fnOFN = BOOL (WINAPI*)(LPOPENFILENAMEW);
using fnOFNA = BOOL (WINAPI*)(LPOPENFILENAMEA);
using fnBFF = PIDLIST_ABSOLUTE (WINAPI*)(LPBROWSEINFOW);
using fnBFFA = PIDLIST_ABSOLUTE (WINAPI*)(LPBROWSEINFOA);
static fnOFN real_GetOpenFileNameW, real_GetSaveFileNameW;
static fnOFNA real_GetOpenFileNameA, real_GetSaveFileNameA;
static fnBFF real_SHBrowseForFolderW;
static fnBFFA real_SHBrowseForFolderA;
struct Suppress { Suppress() { ++t_suppress; } ~Suppress() { --t_suppress; } };
static BOOL WINAPI Hook_GetOpenFileNameW(LPOPENFILENAMEW p) { Suppress s; return real_GetOpenFileNameW(p); }
static BOOL WINAPI Hook_GetSaveFileNameW(LPOPENFILENAMEW p) { Suppress s; return real_GetSaveFileNameW(p); }
static BOOL WINAPI Hook_GetOpenFileNameA(LPOPENFILENAMEA p) { Suppress s; return real_GetOpenFileNameA(p); }
static BOOL WINAPI Hook_GetSaveFileNameA(LPOPENFILENAMEA p) { Suppress s; return real_GetSaveFileNameA(p); }
static PIDLIST_ABSOLUTE WINAPI Hook_SHBrowseForFolderW(LPBROWSEINFOW p) { Suppress s; return real_SHBrowseForFolderW(p); }
static PIDLIST_ABSOLUTE WINAPI Hook_SHBrowseForFolderA(LPBROWSEINFOA p) { Suppress s; return real_SHBrowseForFolderA(p); }

// ---------------------------------------------------------------- IAT patching
static int PatchIAT(HMODULE mod, void* target, void* repl)
{
    if (!mod || !target) return 0;
    auto base = (BYTE*)mod;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int n = 0;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        for (auto th = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk); th->u1.Function; ++th) {
            if ((void*)th->u1.Function != target) continue;
            DWORD old;
            if (VirtualProtect(&th->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                th->u1.Function = (ULONG_PTR)repl;
                VirtualProtect(&th->u1.Function, sizeof(void*), old, &old);
                ++n;
            }
        }
    }
    return n;
}

// x86 hot-patch: "mov edi,edi" + 5 bytes of padding before the function become
// "jmp hook" (in the padding) and "jmp short -7". Returns the address that continues the
// original function (fn + 2), or nullptr when the prologue isn't patchable.
static void* HotPatch(void* fn, void* hook)
{
    auto p = (BYTE*)fn;
    if (!p || p[0] != 0x8B || p[1] != 0xFF) return nullptr;
    for (int i = 1; i <= 5; ++i) if (p[-i] != 0xCC && p[-i] != 0x90) return nullptr;
    DWORD old;
    if (!VirtualProtect(p - 5, 7, PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    p[-5] = 0xE9;
    *(INT32*)(p - 4) = (INT32)((BYTE*)hook - p);
    *(volatile WORD*)p = 0xF9EB;   // EB F9 = jmp short p-5
    VirtualProtect(p - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p - 5, 7);
    Log(L"hot-patched %p", fn);
    return p + 2;
}

// WinFsp registers its x64 DLL as the network provider for all processes. A 32-bit process (EAC)
// that opens a modern file dialog makes mpr.dll load that x64 DLL and call a garbage NPGetCaps,
// which crashes EAC (CD Layout Editor > Load CUE Sheet). Load the x86 build next to it instead.
typedef HMODULE(WINAPI* fnLoadLibraryExW)(LPCWSTR, HANDLE, DWORD);
static fnLoadLibraryExW real_LoadLibraryExW;
static HMODULE WINAPI Hook_LoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags)
{
    static const wchar_t tail[] = L"winfsp-x64.dll";
    const size_t tl = _countof(tail) - 1;
    if (name) {
        size_t n = wcslen(name);
        if (n >= tl && n < MAX_PATH && !_wcsicmp(name + n - tl, tail)) {
            wchar_t fixed[MAX_PATH];
            wcscpy_s(fixed, name);
            wcsncpy_s(fixed + n - tl, tl + 1, L"winfsp-x86.dll", _TRUNCATE);
            if (GetFileAttributesW(fixed) != INVALID_FILE_ATTRIBUTES) {
                Log(L"LoadLibrary %s -> x86 build", name);
                return real_LoadLibraryExW(fixed, file, flags);
            }
        }
    }
    return real_LoadLibraryExW(name, file, flags);
}

static void* HotPatch(void* fn, void* hook);
static void InstallLoaderFix()
{
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    void* fn = kb ? (void*)GetProcAddress(kb, "LoadLibraryExW") : nullptr;
    if (void* p = HotPatch(fn, (void*)Hook_LoadLibraryExW)) real_LoadLibraryExW = (fnLoadLibraryExW)p;
}

// ---- EAC's tooltip window paints a fixed pale yellow face. While it paints, BeginPaint hands EAC a
// memory DC; EndPaint recolours that to the dark palette and copies it to the window in one blit,
// so the yellow never reaches the screen (recolouring on screen afterwards flashed).
static bool RecolorTooltipBits(DWORD* px, int n);   // below
using fnBeginPaint = HDC (WINAPI*)(HWND, LPPAINTSTRUCT);
using fnEndPaint = BOOL (WINAPI*)(HWND, const PAINTSTRUCT*);
static fnBeginPaint real_BeginPaint;
static fnEndPaint real_EndPaint;
struct TipPaint { HWND h; HDC real; HDC mem; HBITMAP bmp; HGDIOBJ old; DWORD* bits; int w, ht; bool done; };
static thread_local TipPaint t_tip;
static thread_local HWND t_tipWanted;   // set while our WM_PAINT handler runs EAC's tooltip paint

static HDC WINAPI Hook_BeginPaint(HWND h, LPPAINTSTRUCT ps)
{
    if (!h || h != t_tipWanted || t_tip.h) return real_BeginPaint(h, ps);
    InvalidateRect(h, nullptr, FALSE);   // EAC repaints the whole face, so the buffer is complete
    HDC real = real_BeginPaint(h, ps);
    if (!real) return real;
    RECT rc; GetClientRect(h, &rc);
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = std::max(1L, rc.right); bi.bmiHeader.biHeight = -std::max(1L, rc.bottom);
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(real);
    HBITMAP bmp = mem ? CreateDIBSection(real, &bi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bmp) { if (mem) DeleteDC(mem); return real; }
    t_tip = { h, real, mem, bmp, SelectObject(mem, bmp), (DWORD*)bits, (int)bi.bmiHeader.biWidth, (int)-bi.bmiHeader.biHeight, false };
    ps->hdc = mem;
    return mem;
}

static BOOL WINAPI Hook_EndPaint(HWND h, const PAINTSTRUCT* ps)
{
    if (h && h == t_tip.h) {
        GdiFlush();
        RecolorTooltipBits(t_tip.bits, t_tip.w * t_tip.ht);
        BitBlt(t_tip.real, 0, 0, t_tip.w, t_tip.ht, t_tip.mem, 0, 0, SRCCOPY);
        SelectObject(t_tip.mem, t_tip.old); DeleteObject(t_tip.bmp); DeleteDC(t_tip.mem);
        PAINTSTRUCT real = *ps; real.hdc = t_tip.real;
        t_tip = TipPaint{}; t_tip.done = true;
        return real_EndPaint(h, &real);
    }
    return real_EndPaint(h, ps);
}

static void InstallHooks()
{
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    real_GetSysColor = (fnGetSysColor)GetProcAddress(user32, "GetSysColor");
    real_GetSysColorBrush = (fnGetSysColorBrush)GetProcAddress(user32, "GetSysColorBrush");

    std::vector<HMODULE> mods{ GetModuleHandleW(nullptr) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{ sizeof me };
        for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
            if (!_wcsicmp(me.szModule, L"comctl32.dll")) mods.push_back(me.hModule);
        CloseHandle(snap);
    }
    for (HMODULE m : mods) {
        int a = PatchIAT(m, real_GetSysColor, Hook_GetSysColor);
        int b = PatchIAT(m, real_GetSysColorBrush, Hook_GetSysColorBrush);
        Log(L"patched module %p: GetSysColor=%d GetSysColorBrush=%d", m, a, b);
    }
    // Also hot-patch the exports so callers that resolve them at runtime (.NET P/Invoke used by
    // the WinForms metadata plugins, uxtheme, ...) get the dark palette too.
    if (void* p = HotPatch((void*)real_GetSysColor, (void*)Hook_GetSysColor)) real_GetSysColor = (fnGetSysColor)p;
    if (void* p = HotPatch((void*)real_GetSysColorBrush, (void*)Hook_GetSysColorBrush)) real_GetSysColorBrush = (fnGetSysColorBrush)p;

    HMODULE exe = GetModuleHandleW(nullptr);
    real_FillRect = (fnFillRect)GetProcAddress(user32, "FillRect");
    PatchIAT(exe, real_FillRect, Hook_FillRect);
    real_BeginPaint = (fnBeginPaint)GetProcAddress(user32, "BeginPaint");
    real_EndPaint = (fnEndPaint)GetProcAddress(user32, "EndPaint");
    int bp = PatchIAT(exe, real_BeginPaint, Hook_BeginPaint), ep = PatchIAT(exe, real_EndPaint, Hook_EndPaint);
    Log(L"patched EAC.exe BeginPaint=%d EndPaint=%d", bp, ep);
    if (HMODULE cd = GetModuleHandleW(L"comdlg32.dll")) {
        real_GetOpenFileNameW = (fnOFN)GetProcAddress(cd, "GetOpenFileNameW");
        real_GetSaveFileNameW = (fnOFN)GetProcAddress(cd, "GetSaveFileNameW");
        real_GetOpenFileNameA = (fnOFNA)GetProcAddress(cd, "GetOpenFileNameA");
        real_GetSaveFileNameA = (fnOFNA)GetProcAddress(cd, "GetSaveFileNameA");
        PatchIAT(exe, real_GetOpenFileNameW, Hook_GetOpenFileNameW);
        PatchIAT(exe, real_GetSaveFileNameW, Hook_GetSaveFileNameW);
        PatchIAT(exe, real_GetOpenFileNameA, Hook_GetOpenFileNameA);
        PatchIAT(exe, real_GetSaveFileNameA, Hook_GetSaveFileNameA);
    }
    if (HMODULE sh = GetModuleHandleW(L"shell32.dll")) {
        real_SHBrowseForFolderW = (fnBFF)GetProcAddress(sh, "SHBrowseForFolderW");
        real_SHBrowseForFolderA = (fnBFFA)GetProcAddress(sh, "SHBrowseForFolderA");
        PatchIAT(exe, real_SHBrowseForFolderW, Hook_SHBrowseForFolderW);
        PatchIAT(exe, real_SHBrowseForFolderA, Hook_SHBrowseForFolderA);
    }
}

static void ApplyAppMode()
{
    if (pSetPreferredAppMode) pSetPreferredAppMode(g_dark ? 2 /*ForceDark*/ : 0 /*Default*/);
    if (pRefreshImmersiveColorPolicyState) pRefreshImmersiveColorPolicyState();
    if (pFlushMenuThemes) pFlushMenuThemes();
}

static BOOL CALLBACK InitOnceProc(PINIT_ONCE, PVOID, PVOID*)
{
    g_brBg = CreateSolidBrush(C_BG);       g_brCtrl = CreateSolidBrush(C_CTRL);
    g_brList = CreateSolidBrush(C_LIST);   g_brBorder = CreateSolidBrush(C_BORDER);
    g_brHot = CreateSolidBrush(C_HOT);     g_brPress = CreateSolidBrush(C_PRESS);
    g_brFocus = CreateSolidBrush(RGB(110, 110, 110));
    g_brTabSel = CreateSolidBrush(C_TABSEL);
    if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        pRefreshImmersiveColorPolicyState = (fnVoid)GetProcAddress(ux, MAKEINTRESOURCEA(104));
        pAllowDarkModeForWindow = (fnAllowDarkModeForWindow)GetProcAddress(ux, MAKEINTRESOURCEA(133));
        pSetPreferredAppMode = (fnSetPreferredAppMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
        pFlushMenuThemes = (fnVoid)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    }
    InstallHooks();
    ApplyAppMode();
    Log(L"init done, dark=%d", (int)g_dark);
    return TRUE;
}

// ---------------------------------------------------------------- applying themes
static void ThemeOf(HWND h, const wchar_t* darkName)
{
    SetWindowTheme(h, g_dark ? darkName : nullptr, nullptr);
}

// ---- list view icons (the track list's note icons are black on grey, made for a white list)
// HIMAGELIST handles belong to one comctl32 version (5 or 6), so call the image list functions of
// the comctl32 that owns the list view's window class.
struct ImageListApi {
    HIMAGELIST (WINAPI* Duplicate)(HIMAGELIST);
    int        (WINAPI* GetImageCount)(HIMAGELIST);
    HICON      (WINAPI* GetIcon)(HIMAGELIST, int, UINT);
    int        (WINAPI* ReplaceIcon)(HIMAGELIST, int, HICON);
    BOOL       (WINAPI* Destroy)(HIMAGELIST);
};

static bool GetImageListApi(HWND lv, ImageListApi& api)
{
    HMODULE m = nullptr;
    void* proc = (void*)GetClassLongPtrW(lv, GCLP_WNDPROC);
    if (!proc || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)proc, &m))
        return false;
    api.Duplicate = (decltype(api.Duplicate))GetProcAddress(m, "ImageList_Duplicate");
    api.GetImageCount = (decltype(api.GetImageCount))GetProcAddress(m, "ImageList_GetImageCount");
    api.GetIcon = (decltype(api.GetIcon))GetProcAddress(m, "ImageList_GetIcon");
    api.ReplaceIcon = (decltype(api.ReplaceIcon))GetProcAddress(m, "ImageList_ReplaceIcon");
    api.Destroy = (decltype(api.Destroy))GetProcAddress(m, "ImageList_Destroy");
    return api.Duplicate && api.GetImageCount && api.GetIcon && api.ReplaceIcon && api.Destroy;
}

// Grey pixels are inverted (black notes -> light, light grey box -> dark grey); coloured pixels stay.
static HICON DarkIcon(HICON icon)
{
    ICONINFO ii{};
    if (!GetIconInfo(icon, &ii)) return nullptr;
    BITMAP bm{};
    GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof bm, &bm);
    int w = bm.bmWidth, ht = ii.hbmColor ? bm.bmHeight : bm.bmHeight / 2;
    HICON result = nullptr;
    if (ii.hbmColor && w > 0 && ht > 0 && w <= 256 && ht <= 256) {
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -ht; bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        std::vector<DWORD> px(w * ht), mask(w * ht, 0);
        HDC dc = GetDC(nullptr);
        bool ok = GetDIBits(dc, ii.hbmColor, 0, ht, px.data(), &bi, DIB_RGB_COLORS) == ht;
        if (ok && ii.hbmMask) GetDIBits(dc, ii.hbmMask, 0, ht, mask.data(), &bi, DIB_RGB_COLORS);
        if (ok) {
            bool alpha = std::any_of(px.begin(), px.end(), [](DWORD p) { return (p >> 24) != 0; });
            for (int i = 0; i < w * ht; ++i) {
                DWORD p = px[i];
                if (alpha ? (p >> 24) == 0 : (mask[i] & 0xFFFFFF) != 0) continue;   // transparent
                int b = p & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
                if (std::max({ r, g, b }) - std::min({ r, g, b }) > 24) continue;   // coloured: keep
                int l = 240 - ((r + g + b) / 3) * 3 / 4;
                px[i] = (p & 0xFF000000) | (l << 16) | (l << 8) | l;
            }
            HBITMAP color = CreateCompatibleBitmap(dc, w, ht);
            SetDIBits(dc, color, 0, ht, px.data(), &bi, DIB_RGB_COLORS);
            ICONINFO ni = ii; ni.hbmColor = color;
            result = CreateIconIndirect(&ni);
            DeleteObject(color);
        }
        ReleaseDC(nullptr, dc);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return result;
}

static HIMAGELIST DarkImageList(HWND lv, HIMAGELIST src)
{
    ImageListApi api;
    if (!GetImageListApi(lv, api)) return nullptr;
    HIMAGELIST dup = api.Duplicate(src);
    if (!dup) return nullptr;
    int n = api.GetImageCount(dup);
    for (int i = 0; i < n && i < 256; ++i) {
        HICON ic = api.GetIcon(dup, i, ILD_NORMAL);
        if (!ic) continue;
        if (HICON dark = DarkIcon(ic)) { api.ReplaceIcon(dup, i, dark); DestroyIcon(dark); }
        DestroyIcon(ic);
    }
    return dup;
}

static void ApplyListViewIcons(HWND h, const WinInfo& wi, bool dark)
{
    HIMAGELIST cur = (HIMAGELIST)SendMessageW(h, LVM_GETIMAGELIST, LVSIL_SMALL, 0);
    if (dark) {
        if (!cur || cur == wi.lvImgDark) return;
        HIMAGELIST d = DarkImageList(h, cur);
        if (!d) return;
        HIMAGELIST previous = wi.lvImgDark;
        AcquireSRWLockExclusive(&g_lock);
        auto it = g_wins->find(h);
        if (it != g_wins->end()) { it->second.lvImgOrig = cur; it->second.lvImgDark = d; }
        ReleaseSRWLockExclusive(&g_lock);
        SendMessageW(h, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)d);
        ImageListApi api;
        if (previous && previous != d && GetImageListApi(h, api)) api.Destroy(previous);   // no longer used by the list
    } else if (wi.lvImgOrig && cur == wi.lvImgDark) {
        SendMessageW(h, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)wi.lvImgOrig);
    }
}

static void ApplyWindow(HWND h)
{
    WinInfo wi;
    if (!GetInfo(h, wi) || wi.kind == K_FOREIGN || IsForeignRoot(h)) return;
    bool d = g_dark;
    if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(h, d);
    if (wi.top) { BOOL v = d; DwmSetWindowAttribute(h, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &v, sizeof v); }

    SetApplying(h, true);
    switch (wi.kind) {
    case K_BUTTON: ThemeOf(h, L"DarkMode_Explorer"); break;
    case K_EDIT: {
        LONG st = GetWindowLongW(h, GWL_STYLE);
        ThemeOf(h, (st & (WS_VSCROLL | WS_HSCROLL)) ? L"DarkMode_Explorer" : L"DarkMode_CFD");
        break;
    }
    case K_COMBO: ThemeOf(h, L"DarkMode_CFD"); break;
    case K_COMBOLBOX: case K_LISTBOX: case K_SCROLLBAR: case K_TOOLTIP:
        ThemeOf(h, L"DarkMode_Explorer"); break;
    case K_TREE:
        ThemeOf(h, L"DarkMode_Explorer");
        SendMessageW(h, TVM_SETBKCOLOR, 0, d ? C_LIST : (LPARAM)-1);
        SendMessageW(h, TVM_SETTEXTCOLOR, 0, d ? C_TEXT : (LPARAM)-1);
        break;
    case K_LISTVIEW: {
        if (!wi.lvSaved) {   // never hold g_lock across SendMessage: the subclass proc takes it too
            COLORREF bk = (COLORREF)SendMessageW(h, LVM_GETBKCOLOR, 0, 0);
            COLORREF tbk = (COLORREF)SendMessageW(h, LVM_GETTEXTBKCOLOR, 0, 0);
            COLORREF tx = (COLORREF)SendMessageW(h, LVM_GETTEXTCOLOR, 0, 0);
            AcquireSRWLockExclusive(&g_lock);
            auto it = g_wins->find(h);
            if (it != g_wins->end()) {
                it->second.lvBk = bk; it->second.lvTextBk = tbk; it->second.lvText = tx;
                it->second.lvSaved = true; wi = it->second;
            }
            ReleaseSRWLockExclusive(&g_lock);
        }
        ThemeOf(h, L"DarkMode_Explorer");
        // Colours saved while dark mode was on came from the hooked GetSysColor: unmap them.
        SendMessageW(h, LVM_SETBKCOLOR, 0, d ? C_LIST : UnmapColor(wi.lvBk, COLOR_WINDOWTEXT));
        SendMessageW(h, LVM_SETTEXTBKCOLOR, 0, d ? C_LIST : UnmapColor(wi.lvTextBk, COLOR_WINDOWTEXT));
        SendMessageW(h, LVM_SETTEXTCOLOR, 0, d ? C_TEXT : UnmapColor(wi.lvText, COLOR_WINDOWTEXT));
        if (HWND hdr = (HWND)SendMessageW(h, LVM_GETHEADER, 0, 0)) ApplyWindow(hdr);
        ApplyListViewIcons(h, wi, d);
        break;
    }
    case K_HEADER: ThemeOf(h, L"ItemsView"); break;
    case K_PROGRESS:
        SetWindowTheme(h, d ? L"" : nullptr, d ? L"" : nullptr);
        SendMessageW(h, PBM_SETBKCOLOR, 0, d ? C_CTRL : CLR_DEFAULT);
        SendMessageW(h, PBM_SETBARCOLOR, 0, d ? RGB(6, 176, 37) : CLR_DEFAULT);
        break;
    case K_RICHEDIT: {
        ThemeOf(h, L"DarkMode_Explorer");
        SendMessageW(h, EM_SETBKGNDCOLOR, d ? 0 : 1, C_LIST);
        CHARFORMAT2W cf{}; cf.cbSize = sizeof cf; cf.dwMask = CFM_COLOR;
        cf.crTextColor = C_TEXT; cf.dwEffects = d ? 0 : CFE_AUTOCOLOR;
        SendMessageW(h, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
        SendMessageW(h, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
        break;
    }
    case K_TRACKBAR: case K_UPDOWN: case K_TOOLBAR: case K_REBAR:
        ThemeOf(h, L"DarkMode_Explorer"); break;
    default: break;
    }
    SetApplying(h, false);
}

// ---------------------------------------------------------------- WM_CTLCOLOR*
static LRESULT DarkCtlColor(UINT msg, WPARAM wp, LPARAM lp)
{
    HDC dc = (HDC)wp; HWND c = (HWND)lp;
    COLORREF bg = C_BG;
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX) bg = C_CTRL;
    else if (msg == WM_CTLCOLORSTATIC && c) {
        wchar_t cls[32] = {};
        RealGetWindowClassW(c, cls, 32);
        if (!_wcsicmp(cls, L"Edit")) bg = C_CTRL;
    }
    COLORREF t = GetTextColor(dc);
    if (Colorful(t)) { if (Lum(t) < 150) t = Mix(t, RGB(255, 255, 255), 45); }
    else t = (c && !IsWindowEnabled(c)) ? C_DIS : C_TEXT;
    SetTextColor(dc, t);
    SetBkColor(dc, bg);
    return (LRESULT)BrushFor(bg);
}

// Is this one of our dark brushes, or a solid brush EAC created from a hooked dark colour?
// Returns the system colour index it stands for.
static bool OurSysBrush(HBRUSH b, int& idx)
{
    if (!b) return false;
    for (int i = 0; i < 64; ++i) if (g_sysBrush[i] == b) { idx = i; return true; }
    if (b == g_brBg) { idx = COLOR_BTNFACE; return true; }
    if (b == g_brCtrl || b == g_brList) { idx = COLOR_WINDOW; return true; }
    LOGBRUSH lb{};
    if ((ULONG_PTR)b > 64 && GetObjectType(b) == OBJ_BRUSH && GetObjectW(b, sizeof lb, &lb) && lb.lbStyle == BS_SOLID) {
        if (lb.lbColor == C_BG) { idx = COLOR_BTNFACE; return true; }
        if (lb.lbColor == C_CTRL || lb.lbColor == C_LIST) { idx = COLOR_WINDOW; return true; }
    }
    return false;
}

static COLORREF UnmapColor(COLORREF c, int textIdx)
{
    if (c == C_TEXT) return real_GetSysColor(textIdx);
    if (c == C_CTRL || c == C_LIST) return real_GetSysColor(COLOR_WINDOW);
    if (c == C_BG) return real_GetSysColor(COLOR_BTNFACE);
    if (c == C_DIS) return real_GetSysColor(COLOR_GRAYTEXT);
    return c;
}

static LRESULT LightCtlColor(WPARAM wp, LRESULT r)
{
    HDC dc = (HDC)wp;
    SetTextColor(dc, UnmapColor(GetTextColor(dc), COLOR_WINDOWTEXT));
    SetBkColor(dc, UnmapColor(GetBkColor(dc), COLOR_WINDOWTEXT));
    int idx;
    if (OurSysBrush((HBRUSH)r, idx)) return (LRESULT)real_GetSysColorBrush(idx);
    return r;
}

// ---------------------------------------------------------------- owner-drawn EAC buttons
static inline bool Near(DWORD a, DWORD b, int tol)
{
    return abs((int)(a & 0xFF) - (int)(b & 0xFF)) <= tol && abs((int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF)) <= tol &&
           abs((int)((a >> 16) & 0xFF) - (int)((b >> 16) & 0xFF)) <= tol;
}

// The button face EAC paints is a flat light grey: flood-fill it from the edges with the dark
// background, then turn dark neutral glyph pixels light. Coloured pixels are left alone.
static void RecolorBits(DWORD* px, int w, int h, bool invertGlyphs)
{
    const DWORD bg = (GetRValue(C_BG) << 16) | (GetGValue(C_BG) << 8) | GetBValue(C_BG);
    std::vector<unsigned char> mark(w * h, 0);
    std::vector<int> stack;
    auto seed = [&](int x, int y) {
        int i = y * w + x; DWORD p = px[i] & 0xFFFFFF;
        int r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        if (std::max({ r, g, b }) - std::min({ r, g, b }) <= 10 && (r + g + b) / 3 >= 150 && !mark[i]) { mark[i] = 1; stack.push_back(i); }
    };
    // seed from the outer rings: some buttons (CD Layout Editor) have a 1px dark outline around the face
    for (int d = 0; d < 3 && d * 2 < w && d * 2 < h; ++d) {
        for (int x = d; x < w - d; ++x) { seed(x, d); seed(x, h - 1 - d); }
        for (int y = d; y < h - d; ++y) { seed(d, y); seed(w - 1 - d, y); }
    }
    while (!stack.empty()) {
        int i = stack.back(); stack.pop_back();
        DWORD c = px[i] & 0xFFFFFF;
        px[i] = bg;
        int x = i % w, y = i / w;
        const int nx[4] = { x - 1, x + 1, x, x }, ny[4] = { y, y, y - 1, y + 1 };
        for (int k = 0; k < 4; ++k) {
            if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
            int j = ny[k] * w + nx[k];
            if (!mark[j] && Near(px[j], c, 6)) { mark[j] = 1; stack.push_back(j); }
        }
    }
    if (!invertGlyphs) return;
    // small bevelled buttons (CD Layout Editor): their light/grey 3D edge becomes the border grey
    if (w <= 32 && h <= 32) {
        const DWORD bord = (GetRValue(C_BORDER) << 16) | (GetGValue(C_BORDER) << 8) | GetBValue(C_BORDER);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            if (x >= 2 && y >= 2 && x < w - 2 && y < h - 2) continue;
            int i = y * w + x; if (mark[i]) continue;
            DWORD p = px[i]; int b = p & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
            if (std::max({ r, g, b }) - std::min({ r, g, b }) <= 10 && (r + g + b) / 3 >= 100) { px[i] = bord; mark[i] = 1; }
        }
    }
    for (int i = 0; i < w * h; ++i) {
        if (mark[i] || Near(px[i], bg, 3)) continue;   // idempotent: never touch the dark background
        DWORD p = px[i];
        int b = p & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
        if (std::max({ r, g, b }) - std::min({ r, g, b }) > 14) continue;
        int l = (r + g + b) / 3;
        if (l < 100) { int nl = 235 - l; px[i] = (nl << 16) | (nl << 8) | nl; }
    }
}

// Wide owner-drawn strips with a flat light face and dark text/ticks (the CD Layout Editor time ruler):
// face -> control grey, dark neutral text -> light, light 3D edges -> border grey. Colours are kept.
// The CD Layout Editor's graphical layout: black background with light green track blocks, light grey
// parts and dark blue track/index numbers. Darken the pastel blocks, lighten the blue text. Idempotent:
// the colours it produces are never matched again.
static bool RecolorGraph(DWORD* px, int n)
{
    auto pack = [](COLORREF c) { return (DWORD)((GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c)); };
    const DWORD list = pack(C_LIST), bord = pack(C_BORDER), green = pack(RGB(52, 64, 54));   // dark grey, faint green tint
    const DWORD kText = 0xE6E7E6;   // light text; exact value so a second pass leaves it alone
    bool any = false;
    for (int i = 0; i < n; ++i) {
        DWORD p = px[i]; int b = p & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
        int mx = std::max({ r, g, b }), mn = std::min({ r, g, b });
        DWORD q = p & 0xFFFFFF;
        if (q == kText) continue;   // ours from an earlier pass (the strip is recoloured twice per paint)
        if (mx < 12) q = list;                                              // black background
        else if (mn >= 150 && mx - mn <= 12) q = bord;                      // light grey parts
        else if (mn >= 150 && g > r + 12 && g > b + 12) q = green;          // light green track blocks
        else if (mn >= 150) q = pack(Mix(RGB(r, g, b), C_LIST, 75));         // other pastel blocks
        else if (b > r + 60 && b > g + 60 && mx >= 120 && mn < 60) q = kText;           // blue numbers/markers -> light text
        if (q != (p & 0xFFFFFF)) { px[i] = q; any = true; }
    }
    return any;
}

static bool RecolorStrip(DWORD* px, int n)
{
    int hist[256] = {};
    for (int i = 0; i < n; ++i) {
        DWORD p = px[i]; int b = p & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
        if (std::max({ r, g, b }) - std::min({ r, g, b }) <= 6) ++hist[(r + g + b) / 3];
    }
    int face = 0; for (int l = 1; l < 256; ++l) if (hist[l] > hist[face]) face = l;
    if (face < 30) return RecolorGraph(px, n);            // black strip: the graphical CD layout
    if (face < 150) return false;                          // already recoloured (the ruler is painted twice)
    if (hist[face] * 2 < n) return false;
    const DWORD ctrl = (GetRValue(C_CTRL) << 16) | (GetGValue(C_CTRL) << 8) | GetBValue(C_CTRL);
    const DWORD bord = (GetRValue(C_BORDER) << 16) | (GetGValue(C_BORDER) << 8) | GetBValue(C_BORDER);
    for (int i = 0; i < n; ++i) {
        DWORD p = px[i]; int b = p & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
        if (std::max({ r, g, b }) - std::min({ r, g, b }) > 14) continue;
        int l = (r + g + b) / 3;
        if (abs(l - face) <= 8) px[i] = ctrl;
        else if (l < 20) px[i] = 0xE1E1E1;           // black text and ticks
        else if (l > face + 8) px[i] = bord;         // light 3D edges
        // other greys (already dark background, shadows) stay
    }
    return true;
}

// True when the image is (almost) one flat light colour, e.g. the empty cover box.
static bool IsFlatLight(const DWORD* px, int n, int w)
{
    DWORD c = px[n / 2 + w / 2] & 0xFFFFFF;   // centre pixel (n/2 alone is the left edge of the middle row)
    if (Lum(RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF)) < 180) return false;
    int same = 0;
    for (int i = 0; i < n; i += 7) if (Near(px[i], c, 8)) ++same;
    return same * 7 >= n * 85 / 100;
}

// Light mode after starting dark: EAC's GDI+ button faces were painted with the dark colour it
// cached at startup. Flood-fill that dark face from the edges with the real button face colour.
static void RecolorBitsLight(DWORD* px, int w, int h)
{
    const DWORD dark = (GetRValue(C_BG) << 16) | (GetGValue(C_BG) << 8) | GetBValue(C_BG);
    COLORREF f = real_GetSysColor(COLOR_BTNFACE);
    const DWORD face = (GetRValue(f) << 16) | (GetGValue(f) << 8) | GetBValue(f);
    std::vector<unsigned char> mark(w * h, 0);
    std::vector<int> stack;
    // The face can be framed by a light margin, so the fill walks through both colours.
    auto ok = [&](DWORD p) { return Near(p, dark, 6) || Near(p, face, 4); };
    auto seed = [&](int x, int y) { int i = y * w + x; if (!mark[i] && ok(px[i])) { mark[i] = 1; stack.push_back(i); } };
    for (int x = 0; x < w; ++x) { seed(x, 0); seed(x, h - 1); }
    for (int y = 0; y < h; ++y) { seed(0, y); seed(w - 1, y); }
    while (!stack.empty()) {
        int i = stack.back(); stack.pop_back();
        px[i] = face;
        int x = i % w, y = i / w;
        const int nx[4] = { x - 1, x + 1, x, x }, ny[4] = { y, y, y - 1, y + 1 };
        for (int k = 0; k < 4; ++k) {
            if (nx[k] < 0 || ny[k] < 0 || nx[k] >= w || ny[k] >= h) continue;
            int j = ny[k] * w + nx[k];
            if (!mark[j] && ok(px[j])) { mark[j] = 1; stack.push_back(j); }
        }
    }
}

// True when the image is (almost) one flat dark colour (the empty cover box drawn with cached colours).
static bool IsFlatDark(const DWORD* px, int n, int w)
{
    DWORD c = px[n / 2 + w / 2] & 0xFFFFFF;
    if (Lum(RGB((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF)) > 70) return false;
    int same = 0;
    for (int i = 0; i < n; i += 7) if (Near(px[i], c, 8)) ++same;
    return same * 7 >= n * 85 / 100;
}

// EAC paints its image buttons with GDI+ on its own DC, so let it draw, then read the
// button's pixels back, recolour them and write them back.
static void RecolorWindow(HWND btn)
{
    RECT rc; GetClientRect(btn, &rc);
    int w = rc.right, ht = rc.bottom;
    bool strip = w > 400 && ht <= 40;   // e.g. the CD Layout Editor time ruler
    if (w <= 0 || ht <= 0 || (!strip && (w > 400 || ht > 400)) || ht > 400) return;
    bool large = w > 100 || ht > 100;   // cover art: only touch it when it is an empty flat box
    HDC dc = GetDC(btn);
    if (!dc) return;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -ht; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (mem && bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, w, ht, dc, 0, 0, SRCCOPY);
        GdiFlush();
        bool write = true;
        if (!g_dark) {
            if (strip) write = false;   // painted fresh from EAC's (real) colours
            else if (!large) RecolorBitsLight((DWORD*)bits, w, ht);
            else if (IsFlatDark((DWORD*)bits, w * ht, w)) {
                FillRect(mem, &rc, real_GetSysColorBrush(COLOR_BTNFACE));
                FrameRect(mem, &rc, real_GetSysColorBrush(COLOR_BTNSHADOW));
            } else write = false;
        }
        else if (strip) write = RecolorStrip((DWORD*)bits, w * ht);
        else if (!large) RecolorBits((DWORD*)bits, w, ht, true);
        else if (IsFlatLight((DWORD*)bits, w * ht, w)) { FillRect(mem, &rc, g_brCtrl); FrameRect(mem, &rc, g_brBorder); }
        else write = false;
        if (write) BitBlt(dc, 0, 0, w, ht, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(btn, dc);
}

// WinForms labels/link labels keep fixed dark colours (e.g. link blue): lighten dark coloured
// pixels after WinForms has painted.
static void LightenDarkText(HWND h)
{
    RECT rc; GetClientRect(h, &rc);
    int w = rc.right, ht = rc.bottom;
    if (w <= 0 || ht <= 0 || w > 2000 || ht > 600) return;
    HDC dc = GetDC(h);
    if (!dc) return;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -ht; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (mem && bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, w, ht, dc, 0, 0, SRCCOPY);
        GdiFlush();
        bool changed = false;
        auto px = (DWORD*)bits;
        // Only act on link-coloured text: most dark coloured pixels must be blue. Plain text only
        // has ClearType fringes, which are as much orange as blue.
        int blue = 0, other = 0;
        auto darkColoured = [](int r, int g, int b) {
            return std::max({ r, g, b }) - std::min({ r, g, b }) >= 40 && (r * 299 + g * 587 + b * 114) / 1000 < 120;
        };
        for (int i = 0; i < w * ht; ++i) {
            DWORD p = px[i]; int b = p & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
            if (!darkColoured(r, g, b)) continue;
            if (b > r + 60 && b > g + 40) ++blue; else ++other;
        }
        if (blue >= 20 && blue * 10 >= (blue + other) * 7)
        for (int i = 0; i < w * ht; ++i) {
            DWORD p = px[i];
            int b = p & 0xFF, g = (p >> 8) & 0xFF, r = (p >> 16) & 0xFF;
            if (!darkColoured(r, g, b)) continue;
            r += (255 - r) * 55 / 100; g += (255 - g) * 55 / 100; b += (255 - b) * 55 / 100;
            px[i] = (r << 16) | (g << 8) | b; changed = true;
        }
        if (changed) BitBlt(dc, 0, 0, w, ht, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(h, dc);
}

static int WfButtonRole(HWND h);

static LRESULT DarkDrawItem(HWND h, WNDPROC orig, WPARAM wp, LPARAM lp)
{
    auto* dis = (DRAWITEMSTRUCT*)lp;
    LRESULT r = CallWindowProcW(orig, h, WM_DRAWITEM, wp, lp);
    if (dis->CtlType == ODT_BUTTON && dis->hwndItem && !IsWinFormsClass(dis->hwndItem)) { GdiFlush(); RecolorWindow(dis->hwndItem); }
    return r;
}

// ---------------------------------------------------------------- custom painting
static HFONT CtlFont(HWND h)
{
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    return f ? f : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
}

static UINT PrefixFlags(HWND h)
{
    LRESULT ui = SendMessageW(h, WM_QUERYUISTATE, 0, 0);
    return (ui & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0;
}

struct MemPaint {
    HWND h; PAINTSTRUCT ps; HDC dc, mem; HBITMAP bmp; HGDIOBJ old; RECT rc;
    explicit MemPaint(HWND w) : h(w) {
        dc = BeginPaint(h, &ps); GetClientRect(h, &rc);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        old = SelectObject(mem, bmp);
    }
    ~MemPaint() {
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); EndPaint(h, &ps);
    }
};

// EAC main-window image buttons get flat vector icons (icons.cpp) instead of EAC's 3D bitmaps.
static bool IsIconButton(HWND h)
{
    if (!HasEacIcon(GetDlgCtrlID(h))) return false;
    wchar_t cls[32] = {};
    GetClassNameW(GetParent(h), cls, 32);
    return !_wcsicmp(cls, L"erstes");
}

static void PaintIconButton(HWND h)
{
    MemPaint p(h);
    bool en = IsWindowEnabled(h) != FALSE;
    bool pressed = (SendMessageW(h, BM_GETSTATE, 0, 0) & BST_PUSHED) ||
                   (GetCapture() == h && (GetKeyState(VK_LBUTTON) & 0x8000));
    bool hot = GetPropW(h, L"eacdark.hot") != nullptr;
    int state = !en ? 3 : pressed ? 2 : hot ? 1 : 0;
    if (!DrawEacIcon(p.mem, p.rc.right, p.rc.bottom, GetDlgCtrlID(h), state)) FillRect(p.mem, &p.rc, g_brBg);
}

static void PaintCheck(HWND h, LONG style)
{
    MemPaint p(h);
    HDC dc = p.mem;
    FillRect(dc, &p.rc, g_brBg);
    UINT type = style & BS_TYPEMASK;
    bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    LRESULT chk = SendMessageW(h, BM_GETCHECK, 0, 0);
    LRESULT st = SendMessageW(h, BM_GETSTATE, 0, 0);
    bool enabled = IsWindowEnabled(h) != 0;
    int base = chk == BST_CHECKED ? 4 : (chk == BST_INDETERMINATE && !radio ? 8 : 0);
    int s = !enabled ? 4 : (st & BST_PUSHED) ? 3 : (st & BST_HOT) ? 2 : 1;
    int part = radio ? BP_RADIOBUTTON : BP_CHECKBOX;

    HFONT oldf = (HFONT)SelectObject(dc, CtlFont(h));
    SIZE gs{ 13, 13 };
    HTHEME th = OpenThemeData(h, L"Button");
    if (th) GetThemePartSize(th, dc, part, base + s, nullptr, TS_DRAW, &gs);
    bool right = (style & BS_RIGHTBUTTON) != 0;
    RECT g{ 0, 0, gs.cx, gs.cy };
    int top = (p.rc.bottom - gs.cy) / 2;
    if (style & BS_MULTILINE) {} // keep vertical centre
    OffsetRect(&g, right ? p.rc.right - gs.cx : 0, top);
    if (th) DrawThemeBackground(th, dc, part, base + s, &g, nullptr);
    else DrawFrameControl(dc, &g, DFC_BUTTON, (radio ? DFCS_BUTTONRADIO : DFCS_BUTTONCHECK) | (chk ? DFCS_CHECKED : 0));
    if (th) CloseThemeData(th);

    wchar_t text[512];
    GetWindowTextW(h, text, 512);
    RECT tr = p.rc;
    int gap = gs.cx / 3 + 1;
    if (right) tr.right -= gs.cx + gap; else tr.left += gs.cx + gap;
    UINT fl = DT_NOCLIP | PrefixFlags(h);
    if (style & BS_MULTILINE) {
        RECT calc = tr; DrawTextW(dc, text, -1, &calc, fl | DT_WORDBREAK | DT_CALCRECT);
        int hh = calc.bottom - calc.top; tr.top = (p.rc.bottom - hh) / 2; tr.bottom = tr.top + hh;
        fl |= DT_WORDBREAK;
    } else fl |= DT_SINGLELINE | DT_VCENTER;
    if (right) fl |= DT_RIGHT;
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? C_TEXT : C_DIS);
    DrawTextW(dc, text, -1, &tr, fl);
    if (GetFocus() == h && !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) && text[0]) {
        RECT fr = tr; DrawTextW(dc, text, -1, &fr, (fl & ~DT_VCENTER) | DT_CALCRECT);
        int hh = fr.bottom - fr.top; fr.top = (style & BS_MULTILINE) ? tr.top : (p.rc.bottom - hh) / 2; fr.bottom = fr.top + hh;
        InflateRect(&fr, 1, 0);
        SetTextColor(dc, RGB(255, 255, 255)); SetBkColor(dc, C_BG);
        DrawFocusRect(dc, &fr);
    }
    SelectObject(dc, oldf);
}

static void PaintGroup(HWND h, bool fill = false)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    if (fill) FillRect(dc, &rc, g_brBg);   // WinForms group boxes paint their own background
    HFONT oldf = (HFONT)SelectObject(dc, CtlFont(h));
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    wchar_t text[256]; GetWindowTextW(h, text, 256);
    RECT fr = rc; fr.top += tm.tmHeight / 2;
    HPEN pen = CreatePen(PS_SOLID, 1, C_BORDER);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, fr.left, fr.top, fr.right, fr.bottom, 6, 6);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
    if (text[0]) {
        RECT tr{ rc.left + 8, rc.top, rc.right - 8, rc.top + tm.tmHeight };
        UINT fl = DT_SINGLELINE | DT_NOCLIP | PrefixFlags(h);
        DrawTextW(dc, text, -1, &tr, fl | DT_CALCRECT);
        RECT bgr = tr; InflateRect(&bgr, 3, 0);
        FillRect(dc, &bgr, g_brBg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, IsWindowEnabled(h) ? C_TEXT : C_DIS);
        DrawTextW(dc, text, -1, &tr, fl);
    }
    SelectObject(dc, oldf);
    EndPaint(h, &ps);
}

// Disabled text statics: Windows draws them embossed (grey text + white copy 1px down-right),
// which smears on a dark background. Paint them flat in a readable grey instead.
static bool IsDisabledTextStatic(HWND h)
{
    LONG st = GetWindowLongW(h, GWL_STYLE);
    if (!(st & WS_DISABLED)) return false;
    UINT t = st & SS_TYPEMASK;
    return t == SS_LEFT || t == SS_CENTER || t == SS_RIGHT || t == SS_LEFTNOWORDWRAP || t == SS_SIMPLE;
}

static void PaintDisabledStatic(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    HWND parent = GetParent(h);
    HBRUSH br = parent ? (HBRUSH)SendMessageW(parent, WM_CTLCOLORSTATIC, (WPARAM)dc, (LPARAM)h) : nullptr;
    FillRect(dc, &rc, br ? br : g_brBg);
    LONG st = GetWindowLongW(h, GWL_STYLE);
    UINT t = st & SS_TYPEMASK;
    UINT fl = DT_NOCLIP | DT_EXPANDTABS;
    if (t == SS_CENTER) fl |= DT_CENTER | DT_WORDBREAK;
    else if (t == SS_RIGHT) fl |= DT_RIGHT | DT_WORDBREAK;
    else if (t == SS_LEFT) fl |= DT_WORDBREAK;
    else fl |= DT_SINGLELINE;
    if (st & SS_NOPREFIX) fl |= DT_NOPREFIX; else fl |= PrefixFlags(h);
    if (st & SS_CENTERIMAGE) fl = (fl & ~DT_WORDBREAK) | DT_SINGLELINE | DT_VCENTER;
    if ((st & SS_ELLIPSISMASK) == SS_ENDELLIPSIS) fl |= DT_END_ELLIPSIS;
    else if ((st & SS_ELLIPSISMASK) == SS_PATHELLIPSIS) fl |= DT_PATH_ELLIPSIS;
    else if ((st & SS_ELLIPSISMASK) == SS_WORDELLIPSIS) fl |= DT_WORD_ELLIPSIS;
    wchar_t text[512]; GetWindowTextW(h, text, 512);
    HFONT oldf = (HFONT)SelectObject(dc, CtlFont(h));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_DISLBL);
    DrawTextW(dc, text, -1, &rc, fl);
    SelectObject(dc, oldf);
    EndPaint(h, &ps);
}

// Drop-down-list combo boxes: the dark theme paints the focused state light, so draw them here.
static bool IsPlainDropList(HWND h)
{
    wchar_t cls[32] = {}; GetClassNameW(h, cls, 32);
    if (!_wcsicmp(cls, L"ComboBoxEx32")) return false;
    LONG st = GetWindowLongW(h, GWL_STYLE);
    return (st & 3) == CBS_DROPDOWNLIST && !(st & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE));
}

static void PaintCombo(HWND h)
{
    MemPaint p(h);
    HDC dc = p.mem;
    bool enabled = IsWindowEnabled(h) != 0;
    bool focus = GetFocus() == h;
    bool dropped = SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) != 0;
    FillRect(dc, &p.rc, g_brCtrl);
    HBRUSH frame = CreateSolidBrush(focus || dropped ? RGB(110, 110, 110) : C_BORDER);
    FrameRect(dc, &p.rc, frame);
    DeleteObject(frame);

    int aw = GetSystemMetrics(SM_CXVSCROLL);
    RECT tr = p.rc; InflateRect(&tr, -2, -2); tr.right -= aw;
    int sel = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
    if (sel >= 0) {
        int len = (int)SendMessageW(h, CB_GETLBTEXTLEN, sel, 0);
        if (len >= 0) {
            std::vector<wchar_t> buf(len + 2);
            SendMessageW(h, CB_GETLBTEXT, sel, (LPARAM)buf.data());
            if (focus && !dropped) { HBRUSH hb = CreateSolidBrush(RGB(11, 92, 173)); FillRect(dc, &tr, hb); DeleteObject(hb); }
            HFONT oldf = (HFONT)SelectObject(dc, CtlFont(h));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, !enabled ? C_DIS : (focus && !dropped) ? RGB(255, 255, 255) : C_TEXT);
            RECT t2 = tr; t2.left += 2;
            DrawTextW(dc, buf.data(), -1, &t2, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, oldf);
        }
    }
    // chevron
    int cx = p.rc.right - aw / 2 - 1, cy = (p.rc.top + p.rc.bottom) / 2;
    HPEN pen = CreatePen(PS_SOLID, 1, enabled ? C_TEXT : C_DIS);
    HGDIOBJ op = SelectObject(dc, pen);
    for (int i = 0; i < 2; ++i) {   // 2px thick
        POINT pts[3] = { { cx - 4, cy - 2 + i }, { cx, cy + 2 + i }, { cx + 5, cy - 3 + i } };
        Polyline(dc, pts, 3);
    }
    SelectObject(dc, op); DeleteObject(pen);
}

// Plain (not owner-drawn) list boxes such as the Status list in "Extracting Audio Data": user32 paints
// some rows and the erased background with light system colours while EAC updates the list, so draw
// the whole list here: dark background, grey selection, light text.
static bool IsPlainListBox(HWND h)
{
    LONG st = GetWindowLongW(h, GWL_STYLE);
    return !(st & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE | LBS_NODATA));
}

static void PaintListBox(HWND h)
{
    MemPaint p(h);
    HDC dc = p.mem;
    bool enabled = IsWindowEnabled(h) != 0;
    bool focus = GetFocus() == h;
    LONG st = GetWindowLongW(h, GWL_STYLE);
    bool multi = (st & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0;
    FillRect(dc, &p.rc, g_brCtrl);

    HFONT oldf = (HFONT)SelectObject(dc, CtlFont(h));
    SetBkMode(dc, TRANSPARENT);
    UINT fmt = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | ((st & LBS_USETABSTOPS) ? DT_EXPANDTABS : 0);
    int count = (int)SendMessageW(h, LB_GETCOUNT, 0, 0);
    int sel = multi ? -1 : (int)SendMessageW(h, LB_GETCURSEL, 0, 0);
    int caret = (int)SendMessageW(h, LB_GETCARETINDEX, 0, 0);
    int scroll = (st & WS_HSCROLL) ? GetScrollPos(h, SB_HORZ) : 0;
    HBRUSH selBrush = CreateSolidBrush(focus ? RGB(11, 92, 173) : C_PRESS);
    std::vector<wchar_t> buf;
    for (int i = (int)SendMessageW(h, LB_GETTOPINDEX, 0, 0); i >= 0 && i < count; ++i) {
        RECT ir{};
        if (SendMessageW(h, LB_GETITEMRECT, i, (LPARAM)&ir) == LB_ERR || ir.top >= p.rc.bottom) break;
        bool selected = multi ? SendMessageW(h, LB_GETSEL, i, 0) > 0 : i == sel;
        if (selected) FillRect(dc, &ir, selBrush);
        int len = (int)SendMessageW(h, LB_GETTEXTLEN, i, 0);
        if (len > 0) {
            buf.assign(len + 1, 0);
            SendMessageW(h, LB_GETTEXT, i, (LPARAM)buf.data());
            RECT tr = ir; tr.left += 2 - scroll;
            SetTextColor(dc, !enabled ? C_DIS : (selected && focus) ? RGB(255, 255, 255) : C_TEXT);
            DrawTextW(dc, buf.data(), -1, &tr, fmt);
        }
        if (focus && i == caret && multi) FrameRect(dc, &ir, g_brBorder);
    }
    DeleteObject(selBrush);
    SelectObject(dc, oldf);
}

// EAC's own tooltip window ("mytooltip", not the common control): a fixed pale yellow face, so with the
// dark palette its text came out light on yellow. After EAC paints it, map the face to the control grey
// and the text to light, by each pixel's distance from the face colour (keeps the anti-aliasing).
static bool IsEacTooltip(HWND h)
{
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    return !_wcsicmp(cls, L"mytooltip");
}

// Returns false when the pixels are not EAC's light face (already dark: nothing to do).
static bool RecolorTooltipBits(DWORD* px, int n)
{
    if (n <= 0) return false;
    // the face is the most common colour
    std::unordered_map<DWORD, int> count;
    for (int i = 0; i < n; i += 3) ++count[px[i] & 0xFFFFFF];
    DWORD face = 0; int best = -1;
    for (auto& c : count) if (c.second > best) { best = c.second; face = c.first; }
    int fr = (face >> 16) & 0xFF, fg = (face >> 8) & 0xFF, fb = face & 0xFF;
    if (Lum(RGB(fr, fg, fb)) <= 120) return false;
    auto dist = [&](DWORD p) { return abs((int)((p >> 16) & 0xFF) - fr) + abs((int)((p >> 8) & 0xFF) - fg) + abs((int)(p & 0xFF) - fb); };
    int maxd = 0;
    for (int i = 0; i < n; ++i) maxd = std::max(maxd, dist(px[i]));
    for (int i = 0; i < n; ++i) {
        int a = maxd < 24 ? 0 : std::min(255, dist(px[i]) * 255 / maxd);
        COLORREF c = Mix(C_CTRL, C_TEXT, a * 100 / 255);
        px[i] = (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c);
    }
    return true;
}

// Fallback when EAC painted the tooltip without going through BeginPaint: recolour it on screen.
static void RecolorTooltip(HWND h)
{
    RECT rc; GetClientRect(h, &rc);
    int w = rc.right, ht = rc.bottom;
    if (w <= 0 || ht <= 0 || w > 2000 || ht > 1000) return;
    HDC dc = GetDC(h);
    if (!dc) return;
    BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -ht; bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (mem && bmp) {
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, w, ht, dc, 0, 0, SRCCOPY);
        GdiFlush();
        if (RecolorTooltipBits((DWORD*)bits, w * ht)) BitBlt(dc, 0, 0, w, ht, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    if (mem) DeleteDC(mem);
    ReleaseDC(h, dc);
}

// ---- WinForms (metadata plugin) push buttons: WinForms draws them itself with the light theme,
// so paint them here. Radio buttons/check boxes share the class, so ask MSAA for the role.
static int WfButtonRole(HWND h)
{
    using fnAOFW = HRESULT (WINAPI*)(HWND, DWORD, REFIID, void**);
    static fnAOFW pAOFW = [] {
        HMODULE m = LoadLibraryExW(L"oleacc.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return m ? (fnAOFW)GetProcAddress(m, "AccessibleObjectFromWindow") : nullptr;
    }();
    IAccessible* acc = nullptr;
    if (!pAOFW || FAILED(pAOFW(h, OBJID_CLIENT, __uuidof(IAccessible), (void**)&acc)) || !acc) return -1;
    VARIANT self; self.vt = VT_I4; self.lVal = CHILDID_SELF;
    VARIANT role; role.vt = VT_EMPTY;
    int r = -1;
    if (SUCCEEDED(acc->get_accRole(self, &role)) && role.vt == VT_I4) r = role.lVal;
    acc->Release();
    return r;
}

static void PaintWfButton(HWND h)
{
    MemPaint p(h);
    HDC dc = p.mem;
    FillRect(dc, &p.rc, g_brBg);
    bool en = IsWindowEnabled(h) != 0;
    POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
    bool hot = en && PtInRect(&p.rc, pt);
    bool pressed = hot && GetCapture() == h && (GetKeyState(VK_LBUTTON) & 0x8000);
    bool focus = GetFocus() == h;
    COLORREF face = !en ? RGB(40, 40, 40) : pressed ? RGB(72, 72, 72) : hot ? RGB(62, 62, 62) : RGB(51, 51, 51);
    COLORREF border = focus ? RGB(110, 110, 110) : RGB(75, 75, 75);   // dark greys, no white frames
    HBRUSH fb = CreateSolidBrush(face);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, fb), op = SelectObject(dc, pen);
    RoundRect(dc, 0, 0, p.rc.right, p.rc.bottom, 6, 6);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(fb); DeleteObject(pen);
    wchar_t text[256]; GetWindowTextW(h, text, 256);
    HFONT oldf = (HFONT)SelectObject(dc, CtlFont(h));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, en ? C_TEXT : C_DIS);
    RECT tr = p.rc;
    DrawTextW(dc, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_CENTER | PrefixFlags(h));
    SelectObject(dc, oldf);
}

static void ExcludeChildren(HWND h, HDC dc)
{
    for (HWND c = GetWindow(h, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (!IsWindowVisible(c)) continue;
        RECT r; GetWindowRect(c, &r);
        MapWindowPoints(nullptr, h, (POINT*)&r, 2);
        ExcludeClipRect(dc, r.left, r.top, r.right, r.bottom);
    }
}

static void PaintStatus(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    ExcludeChildren(h, dc);
    RECT rc; GetClientRect(h, &rc);
    FillRect(dc, &rc, g_brBg);
    RECT line = rc; line.bottom = line.top + 1; FillRect(dc, &line, g_brBorder);
    HFONT oldf = (HFONT)SelectObject(dc, CtlFont(h));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, C_TEXT);
    bool simple = SendMessageW(h, SB_ISSIMPLE, 0, 0) != 0;
    int n = simple ? 1 : (int)SendMessageW(h, SB_GETPARTS, 0, 0);
    for (int i = 0; i < n; ++i) {
        int part = simple ? SB_SIMPLEID : i;
        RECT pr; if (simple) pr = rc; else SendMessageW(h, SB_GETRECT, i, (LPARAM)&pr);
        DWORD lr = (DWORD)SendMessageW(h, SB_GETTEXTLENGTHW, part, 0);
        if (HIWORD(lr) & SBT_OWNERDRAW) {
            DRAWITEMSTRUCT d{}; d.CtlID = GetDlgCtrlID(h); d.itemID = i; d.hwndItem = h; d.hDC = dc; d.rcItem = pr;
            d.itemData = (ULONG_PTR)SendMessageW(h, SB_GETTEXTW, part, 0);
            SendMessageW(GetParent(h), WM_DRAWITEM, d.CtlID, (LPARAM)&d);
        } else {
            std::vector<wchar_t> buf(LOWORD(lr) + 2);
            SendMessageW(h, SB_GETTEXTW, part, (LPARAM)buf.data());
            RECT tr = pr; tr.left += 4; tr.right -= 2;
            DrawTextW(dc, buf.data(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS | DT_EXPANDTABS);
        }
        if (!simple && i < n - 1) { RECT sep{ pr.right - 1, pr.top + 3, pr.right, pr.bottom - 3 }; FillRect(dc, &sep, g_brBorder); }
    }
    SelectObject(dc, oldf);
    EndPaint(h, &ps);
}

static void PaintTab(HWND h)
{
    MemPaint p(h);
    HDC dc = p.mem;
    FillRect(dc, &p.rc, g_brBg);
    int n = (int)SendMessageW(h, TCM_GETITEMCOUNT, 0, 0);
    int sel = (int)SendMessageW(h, TCM_GETCURSEL, 0, 0);
    int tabBottom = 0;
    for (int i = 0; i < n; ++i) { RECT r; SendMessageW(h, TCM_GETITEMRECT, i, (LPARAM)&r); tabBottom = std::max(tabBottom, (int)r.bottom); }
    RECT frame = p.rc; frame.top = tabBottom;
    FrameRect(dc, &frame, g_brBorder);
    HFONT oldf = (HFONT)SelectObject(dc, CtlFont(h));
    SetBkMode(dc, TRANSPARENT);
    UINT pf = PrefixFlags(h);
    for (int i = 0; i < n; ++i) {
        RECT r; SendMessageW(h, TCM_GETITEMRECT, i, (LPARAM)&r);
        wchar_t text[256] = {};
        TCITEMW ti{}; ti.mask = TCIF_TEXT; ti.pszText = text; ti.cchTextMax = 255;
        SendMessageW(h, TCM_GETITEMW, i, (LPARAM)&ti);
        bool s = i == sel;
        if (s) { r.top -= 2; r.bottom = tabBottom + 1; InflateRect(&r, 1, 0); }
        FillRect(dc, &r, s ? g_brTabSel : g_brBg);
        RECT top = r; top.bottom = top.top + 1; FillRect(dc, &top, g_brBorder);
        RECT lft = r; lft.right = lft.left + 1; FillRect(dc, &lft, g_brBorder);
        RECT rgt = r; rgt.left = rgt.right - 1; FillRect(dc, &rgt, g_brBorder);
        if (!s) { RECT bot = r; bot.top = bot.bottom - 1; FillRect(dc, &bot, g_brBorder); }
        SetTextColor(dc, s ? RGB(255, 255, 255) : RGB(190, 190, 190));
        DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER | pf);
    }
    SelectObject(dc, oldf);
}

// ---------------------------------------------------------------- menu bar (UAH)
#define WM_UAHDRAWMENU      0x0091
#define WM_UAHDRAWMENUITEM  0x0092
struct UAHMENU { HMENU hmenu; HDC hdc; DWORD dwFlags; };
struct UAHMENUITEMMETRICS { DWORD cx, cy; DWORD pad[6]; };
struct UAHMENUPOPUPMETRICS { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; };
struct UAHMENUITEM { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; };
struct UAHDRAWMENUITEM { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; };

static void DrawMenuBarBg(HWND h, HDC dc)
{
    MENUBARINFO mbi{ sizeof mbi };
    if (!GetMenuBarInfo(h, OBJID_MENU, 0, &mbi)) return;
    RECT rw; GetWindowRect(h, &rw);
    RECT r = mbi.rcBar; OffsetRect(&r, -rw.left, -rw.top);
    r.top -= 1;
    FillRect(dc, &r, g_brBg);
}

static void DrawMenuBarItem(UAHDRAWMENUITEM* p)
{
    wchar_t text[256] = {};
    MENUITEMINFOW mii{ sizeof mii }; mii.fMask = MIIM_STRING; mii.dwTypeData = text; mii.cch = 255;
    GetMenuItemInfoW(p->um.hmenu, p->umi.iPosition, TRUE, &mii);
    UINT st = p->dis.itemState;
    HBRUSH bg = g_brBg;
    COLORREF tc = C_TEXT;
    if (st & (ODS_INACTIVE | ODS_DEFAULT)) tc = RGB(200, 200, 200);
    if (st & ODS_HOTLIGHT) bg = g_brHot;
    if (st & ODS_SELECTED) bg = g_brPress;
    if (st & (ODS_GRAYED | ODS_DISABLED)) tc = C_DIS;
    UINT fl = DT_CENTER | DT_SINGLELINE | DT_VCENTER | ((st & ODS_NOACCEL) ? DT_HIDEPREFIX : 0);
    FillRect(p->um.hdc, &p->dis.rcItem, bg);
    SetBkMode(p->um.hdc, TRANSPARENT);
    SetTextColor(p->um.hdc, tc);
    DrawTextW(p->um.hdc, text, -1, &p->dis.rcItem, fl);
}

static void DrawMenuLine(HWND h)
{
    if (!GetMenu(h)) return;
    RECT rc; GetClientRect(h, &rc);
    MapWindowPoints(h, nullptr, (POINT*)&rc, 2);
    RECT rw; GetWindowRect(h, &rw);
    OffsetRect(&rc, -rw.left, -rw.top);
    RECT line = rc; line.bottom = line.top; line.top -= 1;
    HDC dc = GetWindowDC(h);
    FillRect(dc, &line, g_brBg);
    ReleaseDC(h, dc);
}

// WinForms tool/dialog frames draw a light 3D edge around the client area: cover it.
static void DrawClientEdge(HWND h)
{
    RECT rc; GetClientRect(h, &rc);
    MapWindowPoints(h, nullptr, (POINT*)&rc, 2);
    RECT rw; GetWindowRect(h, &rw);
    OffsetRect(&rc, -rw.left, -rw.top);
    RECT outer = rc; InflateRect(&outer, 3, 3);
    RECT win{ 0, 0, rw.right - rw.left, rw.bottom - rw.top };
    IntersectRect(&outer, &outer, &win);
    HDC dc = GetWindowDC(h);
    ExcludeClipRect(dc, rc.left, rc.top, rc.right, rc.bottom);
    // keep the caption: only paint from the client top edge downwards (plus the line above it)
    RECT ring = outer; ring.top = rc.top - 1;
    FillRect(dc, &ring, g_brBg);
    ReleaseDC(h, dc);
}

// List views and the position slider draw a light (sunken/themed) border in the non-client area:
// repaint it grey.
static void DrawNcFrame(HWND h, HBRUSH fill, HBRUSH frame = nullptr)
{
    RECT rc; GetClientRect(h, &rc);
    MapWindowPoints(h, nullptr, (POINT*)&rc, 2);
    RECT rw; GetWindowRect(h, &rw);
    int b = rc.left - rw.left;              // border width (scrollbars sit inside it, on the right/bottom)
    if (b <= 0 || b > 4) return;
    RECT win{ 0, 0, rw.right - rw.left, rw.bottom - rw.top };
    RECT in = win; InflateRect(&in, -b, -b);
    HDC dc = GetWindowDC(h);
    if (!dc) return;
    ExcludeClipRect(dc, in.left, in.top, in.right, in.bottom);
    FillRect(dc, &win, fill);
    FrameRect(dc, &win, frame ? frame : g_brBorder);
    ReleaseDC(h, dc);
}

// ---------------------------------------------------------------- Tools > Dark mode
static void SetDark(bool on);

static bool IsToolsMenu(HMENU bar, int idx, int count)
{
    wchar_t text[128] = {};
    GetMenuStringW(bar, idx, text, 127, MF_BYPOSITION);
    wchar_t clean[128]; int j = 0;
    for (int i = 0; text[i] && j < 127; ++i) if (text[i] != L'&') clean[j++] = text[i];
    clean[j] = 0;
    static const wchar_t* names[] = { L"Tools", L"Extras", L"Werkzeuge", L"Outils", L"Herramientas", L"Strumenti", L"Ferramentas", L"Narzędzia", L"Gereedschap", L"Nástroje" };
    for (auto n : names) if (!_wcsicmp(clean, n)) return true;
    // Fallback: EAC's Tools menu sits just before Help.
    return false;
}

static void OnInitMenuPopup(HWND h, HMENU pop)
{
    HMENU bar = GetMenu(h);
    if (!bar) return;
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    if (_wcsicmp(cls, L"erstes")) return;   // only EAC's main window (not the CD Layout Editor)
    int count = GetMenuItemCount(bar), idx = -1;
    for (int i = 0; i < count; ++i) if (GetSubMenu(bar, i) == pop) { idx = i; break; }
    if (idx < 0) return;
    bool tools = IsToolsMenu(bar, idx, count);
    if (!tools) {
        bool anyNamed = false;
        for (int i = 0; i < count; ++i) if (IsToolsMenu(bar, i, count)) { anyNamed = true; break; }
        tools = !anyNamed && idx == count - 2;
    }
    if (!tools) return;
    // The item names the mode it switches to.
    const wchar_t* label = g_dark ? L"&Light Mode" : L"&Dark Mode";
    if (GetMenuState(pop, ID_DARKMODE, MF_BYCOMMAND) == (UINT)-1) {
        AppendMenuW(pop, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(pop, MF_STRING, ID_DARKMODE, label);
    } else {
        MENUITEMINFOW mii{ sizeof mii };
        mii.fMask = MIIM_STRING | MIIM_STATE;
        mii.dwTypeData = (LPWSTR)label;
        mii.fState = MFS_UNCHECKED;
        SetMenuItemInfoW(pop, ID_DARKMODE, FALSE, &mii);
    }
}

// ---------------------------------------------------------------- subclass procedure
static LRESULT SubclassProcInner(HWND h, UINT msg, WPARAM wp, LPARAM lp, WinInfo& wi)
{
    WNDPROC orig = wi.orig;

    if (msg == WM_NCDESTROY) {
        LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
        AcquireSRWLockExclusive(&g_lock);
        g_wins->erase(h);
        g_foreign->erase(h);
        ReleaseSRWLockExclusive(&g_lock);
        return r;
    }
    if (wi.applying || wi.kind == K_FOREIGN || t_suppress)
        return CallWindowProcW(orig, h, msg, wp, lp);

    // our menu item works in both modes
    if (msg == WM_COMMAND && LOWORD(wp) == ID_DARKMODE && HIWORD(wp) == 0 && lp == 0) {
        SetDark(!g_dark);
        return 0;
    }
    if (msg == WM_COMMAND && LOWORD(wp) == ID_REDRAW && HIWORD(wp) == 0 && lp == 0) {
        // second repaint after a toggle: themed buttons only switch theme once the posted
        // WM_THEMECHANGED messages are handled, so the first repaint left light corners
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
        return 0;
    }
    if (msg == WM_INITMENUPOPUP) {
        LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
        if (!HIWORD(lp)) OnInitMenuPopup(h, (HMENU)wp);
        return r;
    }
    if (msg == WM_CREATE) {
        LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
        if (r != -1) {
            // Superclassed controls (EAC's mybutton/myedit/...) only report their base class once created.
            Kind k = wi.kind == K_GENERIC ? KindFromClass(h) : wi.kind;
            LONG st = GetWindowLongW(h, GWL_STYLE);
            bool top = !(st & WS_CHILD) && (st & WS_CAPTION) == WS_CAPTION;
            AcquireSRWLockExclusive(&g_lock);
            auto it = g_wins->find(h);
            if (it != g_wins->end()) { it->second.kind = k == K_FOREIGN ? K_GENERIC : k; it->second.top = top; }
            ReleaseSRWLockExclusive(&g_lock);
            if (k != wi.kind || top != wi.top) Log(L"recheck %p kind=%d top=%d", h, (int)k, (int)top);
            ApplyWindow(h);
        }
        return r;
    }

    if (!g_dark && !IsForeignRoot(h)) {
        // Light mode after a live toggle: EAC cached brushes/colours from our hooked GetSysColor*
        // while dark mode was on. Map those back to the real system colours.
        switch (msg) {
        case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
        case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
            LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
            return LightCtlColor(wp, r);
        }
        case WM_DRAWITEM:
            if (g_startedDark) return DarkDrawItem(h, orig, wp, lp);
            break;
        case WM_PAINT:
            if (g_startedDark && !wi.wf && wi.kind == K_BUTTON && (GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW) {
                LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
                GdiFlush();
                RecolorWindow(h);
                return r;
            }
            break;
        case WM_ERASEBKGND: {
            int idx;
            if (wi.kind == K_GENERIC && OurSysBrush((HBRUSH)GetClassLongPtrW(h, GCLP_HBRBACKGROUND), idx)) {
                RECT rc; GetClientRect(h, &rc); FillRect((HDC)wp, &rc, real_GetSysColorBrush(idx)); return 1;
            }
            break;
        }
        }
        return CallWindowProcW(orig, h, msg, wp, lp);
    }
    if (IsForeignRoot(h))
        return CallWindowProcW(orig, h, msg, wp, lp);

    switch (msg) {
    case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: {
        CallWindowProcW(orig, h, msg, wp, lp);
        return DarkCtlColor(msg, wp, lp);
    }
    case WM_DRAWITEM:
        return DarkDrawItem(h, orig, wp, lp);
    case WM_NOTIFY: {
        auto* nm = (NMHDR*)lp;
        if (nm->code != NM_CUSTOMDRAW) break;
        WinInfo from;
        if (!GetInfo(nm->hwndFrom, from)) break;
        auto* cd = (NMCUSTOMDRAW*)lp;
        if (from.kind == K_HEADER) {
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) { SetTextColor(cd->hdc, C_TEXT); return CDRF_DODEFAULT; }
            break;
        }
        if (from.kind == K_LISTVIEW) {
            LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
            auto* lv = (NMLVCUSTOMDRAW*)lp;
            if (cd->dwDrawStage & CDDS_ITEM) {
                if (lv->clrTextBk != CLR_DEFAULT && lv->clrTextBk != CLR_NONE && Lum(lv->clrTextBk) > 110)
                    lv->clrTextBk = Colorful(lv->clrTextBk) ? Mix(lv->clrTextBk, C_LIST, 72) : C_LIST;
                if (lv->clrText != CLR_DEFAULT && Lum(lv->clrText) < 120)
                    lv->clrText = Colorful(lv->clrText) ? Mix(lv->clrText, RGB(255, 255, 255), 45) : C_TEXT;
            }
            return r;
        }
        if (from.kind == K_TRACKBAR) {   // the position slider: grey channel and thumb instead of white
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                RECT r = cd->rc;
                bool on = IsWindowEnabled(nm->hwndFrom) != FALSE;
                if (cd->dwItemSpec == TBCD_CHANNEL) {
                    FillRect(cd->hdc, &r, g_brCtrl); FrameRect(cd->hdc, &r, g_brBorder);
                    return CDRF_SKIPDEFAULT;
                }
                if (cd->dwItemSpec == TBCD_THUMB) {
                    COLORREF c = !on ? C_BORDER : (cd->uItemState & CDIS_SELECTED) ? RGB(150, 150, 150)
                               : (cd->uItemState & CDIS_HOT) ? RGB(130, 130, 130) : RGB(110, 110, 110);
                    HBRUSH b = CreateSolidBrush(c);
                    FillRect(cd->hdc, &r, b); DeleteObject(b);
                    FrameRect(cd->hdc, &r, g_brPress);
                    return CDRF_SKIPDEFAULT;
                }
                if (cd->dwItemSpec == TBCD_TICS) { SetTextColor(cd->hdc, C_BORDER); return CDRF_NOTIFYPOSTPAINT; }
            }
            if (cd->dwDrawStage == CDDS_ITEMPOSTPAINT && cd->dwItemSpec == TBCD_TICS) {
                // ticks are drawn in a light theme colour: turn every light pixel in the tick band grey
                RECT cl; GetClientRect(nm->hwndFrom, &cl);
                RECT th{}; SendMessageW(nm->hwndFrom, TBM_GETTHUMBRECT, 0, (LPARAM)&th);
                for (int y = th.bottom; y < cl.bottom; ++y) for (int x = cl.left; x < cl.right; ++x)
                    if (Lum(GetPixel(cd->hdc, x, y)) > 150) SetPixel(cd->hdc, x, y, RGB(110, 110, 110));
            }
            break;
        }
        if (from.kind == K_TOOLBAR) {
            auto* tb = (NMTBCUSTOMDRAW*)lp;
            if (cd->dwDrawStage == CDDS_PREPAINT) { FillRect(cd->hdc, &cd->rc, g_brBg); return CDRF_NOTIFYITEMDRAW; }
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                tb->clrText = C_TEXT; tb->clrBtnFace = C_BG; tb->clrBtnHighlight = C_HOT;
                tb->clrHighlightHotTrack = C_HOT; tb->nStringBkMode = TRANSPARENT; tb->nHLStringBkMode = TRANSPARENT;
                return TBCDRF_USECDCOLORS | TBCDRF_HILITEHOTTRACK;
            }
        }
        break;
    }
    case WM_UAHDRAWMENU:
        if (GetMenu(h)) { DrawMenuBarBg(h, ((UAHMENU*)lp)->hdc); return TRUE; }
        break;
    case WM_UAHDRAWMENUITEM:
        if (GetMenu(h)) { DrawMenuBarItem((UAHDRAWMENUITEM*)lp); return TRUE; }
        break;
    case WM_NCPAINT: case WM_NCACTIVATE: {
        LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
        DrawMenuLine(h);
        if (wi.wf && wi.top) DrawClientEdge(h);
        if (wi.kind == K_LISTVIEW) DrawNcFrame(h, g_brList);
        else if (wi.kind == K_LISTBOX && !wi.wf) DrawNcFrame(h, g_brCtrl);   // WS_BORDER frame: grey, not light
        else if (wi.kind == K_GENERIC && !wi.wf && IsEacTooltip(h)) DrawNcFrame(h, g_brCtrl);
        else if (wi.kind == K_TRACKBAR) DrawNcFrame(h, g_brBg);
        else if (wi.kind == K_EDIT) DrawNcFrame(h, g_brCtrl, GetFocus() == h ? g_brFocus : g_brBorder);
        return r;
    }
    case WM_ERASEBKGND:
        if (wi.kind == K_GENERIC) {
            auto b = (ULONG_PTR)GetClassLongPtrW(h, GCLP_HBRBACKGROUND);
            bool sys = b > 0 && b <= 31;
            if (!sys && b) for (int i : { COLOR_BTNFACE, COLOR_WINDOW, COLOR_APPWORKSPACE })
                if ((HBRUSH)b == real_GetSysColorBrush(i)) sys = true;
            if (sys) { RECT rc; GetClientRect(h, &rc); FillRect((HDC)wp, &rc, g_brBg); return 1; }
        }
        if (wi.kind == K_TAB || wi.kind == K_STATUS) return 1;
        if (wi.kind == K_LISTBOX && !wi.wf && IsPlainListBox(h)) return 1;   // PaintListBox fills everything
        if (wi.kind == K_TOOLBAR) { RECT rc; GetClientRect(h, &rc); FillRect((HDC)wp, &rc, g_brBg); return 1; }
        break;
    case WM_PAINT: {
        if (wi.kind == K_BUTTON) {
            LONG st = GetWindowLongW(h, GWL_STYLE);
            UINT t = st & BS_TYPEMASK;
            if (t == BS_OWNERDRAW && wi.wf) {
                int role = wi.role;
                if (role == 0) {
                    role = WfButtonRole(h);
                    AcquireSRWLockExclusive(&g_lock);
                    auto it = g_wins->find(h); if (it != g_wins->end()) it->second.role = role;
                    ReleaseSRWLockExclusive(&g_lock);
                }
                if (role == ROLE_SYSTEM_PUSHBUTTON && GetWindowTextLengthW(h) > 0) { PaintWfButton(h); return 0; }
                break;
            }
            if (t == BS_OWNERDRAW && IsIconButton(h)) { PaintIconButton(h); return 0; }
            if (t == BS_OWNERDRAW) {
                LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
                GdiFlush();
                RecolorWindow(h);
                return r;
            }
            if (st & BS_PUSHLIKE) break;
            // plain push buttons: the dark theme gives them white frames, so paint them ourselves
            if ((t == BS_PUSHBUTTON || t == BS_DEFPUSHBUTTON) && !(st & (BS_BITMAP | BS_ICON)) && GetWindowTextLengthW(h) > 0) {
                PaintWfButton(h); return 0;
            }
            if (t == BS_CHECKBOX || t == BS_AUTOCHECKBOX || t == BS_3STATE || t == BS_AUTO3STATE ||
                t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON) { PaintCheck(h, st); return 0; }
            if (t == BS_GROUPBOX) { PaintGroup(h); return 0; }
        }
        if (wi.kind == K_COMBO && IsPlainDropList(h)) { PaintCombo(h); return 0; }
        if (wi.kind == K_LISTBOX && !wi.wf && IsPlainListBox(h)) { PaintListBox(h); return 0; }
        if (wi.kind == K_GENERIC && !wi.wf && IsEacTooltip(h)) {
            // real colours while EAC paints (dark text on its yellow face) into our buffer; EndPaint recolours
            ++t_suppress;
            t_tipWanted = h; t_tip.done = false;
            LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
            t_tipWanted = nullptr;
            --t_suppress;
            if (!t_tip.done) { GdiFlush(); RecolorTooltip(h); }
            return r;
        }
        if (wi.kind == K_STATIC && !wi.wf && IsDisabledTextStatic(h)) { PaintDisabledStatic(h); return 0; }
        if (wi.wf && (wi.kind == K_GENERIC || wi.kind == K_STATIC)) {
            int role = wi.role;
            if (role == 0) {
                role = WfButtonRole(h);
                AcquireSRWLockExclusive(&g_lock);
                auto it = g_wins->find(h); if (it != g_wins->end()) it->second.role = role;
                ReleaseSRWLockExclusive(&g_lock);
            }
            if (role == ROLE_SYSTEM_GROUPING) { PaintGroup(h, true); return 0; }
            if (wi.kind == K_STATIC) {   // link labels keep a fixed dark blue
                LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
                GdiFlush();
                LightenDarkText(h);
                return r;
            }
        }
        if (wi.kind == K_TAB) { PaintTab(h); return 0; }
        if (wi.kind == K_STATUS) { PaintStatus(h); return 0; }
        break;
    }
    case WM_MOUSEMOVE:
        if (wi.kind == K_BUTTON && !GetPropW(h, L"eacdark.hot") && IsIconButton(h)) {
            SetPropW(h, L"eacdark.hot", (HANDLE)1);
            TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        if (GetPropW(h, L"eacdark.hot")) { RemovePropW(h, L"eacdark.hot"); InvalidateRect(h, nullptr, FALSE); }
        break;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        if (wi.kind == K_BUTTON && IsIconButton(h)) {
            LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
            InvalidateRect(h, nullptr, FALSE);
            return r;
        }
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        if (wi.kind == K_EDIT) {   // the frame colour follows focus
            LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
            RedrawWindow(h, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE | RDW_NOCHILDREN);
            return r;
        }
        break;
    case WM_ENABLE: case BM_SETCHECK: case BM_SETSTATE: case WM_UPDATEUISTATE: case WM_SETTEXT:
        if (wi.kind == K_BUTTON) {
            LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
            InvalidateRect(h, nullptr, FALSE);
            return r;
        }
        if (wi.kind == K_RICHEDIT && msg == WM_SETTEXT) {
            LRESULT r = CallWindowProcW(orig, h, msg, wp, lp);
            ApplyWindow(h);
            return r;
        }
        break;
    case EM_STREAMIN:
        if (wi.kind == K_RICHEDIT) { LRESULT r = CallWindowProcW(orig, h, msg, wp, lp); ApplyWindow(h); return r; }
        break;
    }
    return CallWindowProcW(orig, h, msg, wp, lp);
}

// Records EAC's own list view colours (so light mode can restore them) and substitutes dark ones.
static bool HandleListViewColor(HWND h, WNDPROC orig, UINT msg, LPARAM lp, LRESULT& out)
{
    AcquireSRWLockExclusive(&g_lock);
    auto& w = (*g_wins)[h];
    if (msg == LVM_SETBKCOLOR) w.lvBk = (COLORREF)lp;
    else if (msg == LVM_SETTEXTBKCOLOR) w.lvTextBk = (COLORREF)lp;
    else w.lvText = (COLORREF)lp;
    w.lvSaved = true;
    ReleaseSRWLockExclusive(&g_lock);
    if (!g_dark) return false;
    out = CallWindowProcW(orig, h, msg, 0, msg == LVM_SETTEXTCOLOR ? C_TEXT : C_LIST);
    return true;
}

static LRESULT CALLBACK SubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WinInfo wi;
    if (!GetInfo(h, wi)) return DefWindowProcW(h, msg, wp, lp);
    if (!wi.applying && wi.kind == K_LISTVIEW &&
        (msg == LVM_SETBKCOLOR || msg == LVM_SETTEXTBKCOLOR || msg == LVM_SETTEXTCOLOR)) {
        LRESULT r;
        if (HandleListViewColor(h, wi.orig, msg, lp, r)) return r;
    }
    if (!wi.applying && wi.kind == K_LISTVIEW && msg == LVM_SETIMAGELIST && wp == LVSIL_SMALL && g_dark) {
        // EAC sets the track icons when a CD is read: give the list our dark copy instead
        LRESULT r = CallWindowProcW(wi.orig, h, msg, wp, lp);
        SetApplying(h, true);
        WinInfo now;
        if (GetInfo(h, now)) ApplyListViewIcons(h, now, true);
        SetApplying(h, false);
        return r == (LRESULT)wi.lvImgDark ? (LRESULT)wi.lvImgOrig : r;   // EAC gets back its own list, never ours
    }
    return SubclassProcInner(h, msg, wp, lp, wi);
}

// ---------------------------------------------------------------- live toggle
static BOOL CALLBACK EnumTopProc(HWND h, LPARAM lp)
{
    DWORD pid; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) ((std::vector<HWND>*)lp)->push_back(h);
    return TRUE;
}

static void SetDark(bool on)
{
    g_dark = on;
    WritePrivateProfileStringW(L"EacDark", L"Enabled", on ? L"1" : L"0", g_ini);
    ApplyAppMode();
    std::vector<HWND> all;
    AcquireSRWLockShared(&g_lock);
    for (auto& kv : *g_wins) all.push_back(kv.first);
    ReleaseSRWLockShared(&g_lock);
    for (HWND h : all) if (IsWindow(h)) ApplyWindow(h);
    std::vector<HWND> tops;
    EnumWindows(EnumTopProc, (LPARAM)&tops);
    // Let comctl32 and .NET (SystemColors) drop their cached colours.
    for (HWND t : tops) SendMessageTimeoutW(t, WM_SYSCOLORCHANGE, 0, 0, SMTO_ABORTIFHUNG, 500, nullptr);
    for (HWND t : tops) {
        if (!IsWindowVisible(t)) continue;
        if (GetMenu(t)) DrawMenuBar(t);
        if (GetActiveWindow() == t) { SendMessageW(t, WM_NCACTIVATE, FALSE, 0); SendMessageW(t, WM_NCACTIVATE, TRUE, 0); }
        RedrawWindow(t, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
        WinInfo ti;
        if (GetInfo(t, ti)) PostMessageW(t, WM_COMMAND, ID_REDRAW, 0);   // only windows we subclassed handle it
    }
    Log(L"dark mode -> %d (%zu windows)", (int)on, all.size());
}

// ---------------------------------------------------------------- CBT hook
static bool SkipClass(const wchar_t* cls)
{
    static const wchar_t* skip[] = { L"IME", L"MSCTFIME UI", L"CicMarshalWndClass", L"OleMainThreadWndClass",
        L"OleDdeWndClass", L"tooltips_class32_unused", L"Shell_TrayWnd", L"GDI+ Hook Window Class" };
    for (auto s : skip) if (!_wcsicmp(cls, s)) return true;
    return false;
}

static bool IsShellUiClass(const wchar_t* cls)
{
    static const wchar_t* shell[] = { L"DirectUIHWND", L"DUIViewWndClassName", L"SHELLDLL_DefView",
        L"NamespaceTreeControl", L"Breadcrumb Parent", L"ReBarWindow32_shell_unused" };
    for (auto s : shell) if (!_wcsicmp(cls, s)) return true;
    return false;
}

static void OnCreateWnd(HWND h, CREATESTRUCTW* cs)
{
    wchar_t cls[128] = {};
    GetClassNameW(h, cls, 128);
    if (t_suppress || SkipClass(cls) || cs->hwndParent == HWND_MESSAGE) return;
    if (IsShellUiClass(cls)) {
        HWND root = cs->hwndParent ? GetAncestor(cs->hwndParent, GA_ROOT) : h;
        AcquireSRWLockExclusive(&g_lock); g_foreign->insert(root); ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    WinInfo wi;
    wi.kind = KindFromClass(h);
    // Hidden helper windows (input/caret/tablet message windows the file dialog's worker threads
    // create and tear down) never need dark mode, and subclassing them crashed Load CUE Sheet.
    if (!(cs->style & (WS_CHILD | WS_CAPTION)) && wi.kind == K_GENERIC &&
        _wcsicmp(cls, L"#32770") && _wcsicmp(cls, L"#32768") && _wcsicmp(cls, L"erstes") && !IsWinFormsClass(h))
        return;
    wi.top = !(cs->style & WS_CHILD) && (cs->style & WS_CAPTION) == WS_CAPTION;
    wi.wf = IsWinFormsClass(h);
    if (wi.wf) {
        // WinForms installs its own window procedure right after creation, replacing anything set
        // now; subclass it once the form is shown (HCBT_ACTIVATE / HCBT_SETFOCUS).
        AcquireSRWLockExclusive(&g_lock);
        (*g_wins)[h] = wi;
        ReleaseSRWLockExclusive(&g_lock);
        t_pending.push_back(h);
        return;
    }
    wi.orig = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
    if (!wi.orig) return;
    AcquireSRWLockExclusive(&g_lock);
    (*g_wins)[h] = wi;
    ReleaseSRWLockExclusive(&g_lock);
    SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)SubclassProc);
    Log(L"subclass %p '%s' kind=%d top=%d", h, cls, (int)wi.kind, (int)wi.top);
}

static void ProcessPending()
{
    if (t_pending.empty()) return;
    std::vector<HWND> list;
    list.swap(t_pending);
    for (HWND h : list) {
        if (!IsWindow(h)) {
            AcquireSRWLockExclusive(&g_lock); g_wins->erase(h); ReleaseSRWLockExclusive(&g_lock);
            continue;
        }
        WNDPROC cur = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC);
        if (!cur || cur == SubclassProc) continue;
        LONG st = GetWindowLongW(h, GWL_STYLE);
        AcquireSRWLockExclusive(&g_lock);
        auto it = g_wins->find(h);
        if (it != g_wins->end()) {
            it->second.orig = cur;
            it->second.top = !(st & WS_CHILD) && (st & WS_CAPTION) == WS_CAPTION;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (it == g_wins->end()) continue;
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)SubclassProc);
        ApplyWindow(h);
        InvalidateRect(h, nullptr, TRUE);
        Log(L"late subclass %p", h);
    }
}

static LRESULT CALLBACK CbtProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HCBT_ACTIVATE || code == HCBT_SETFOCUS) ProcessPending();
    if (code == HCBT_CREATEWND) {
        if (g_stage >= 2) InitOnceExecuteOnce(&g_initOnce, InitOnceProc, nullptr, nullptr);
        if (g_stage >= 2) OnCreateWnd((HWND)wp, ((CBT_CREATEWNDW*)lp)->lpcs);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void HookThread()
{
    if (g_tlsHook == TLS_OUT_OF_INDEXES || TlsGetValue(g_tlsHook)) return;
    HHOOK hk = SetWindowsHookExW(WH_CBT, CbtProc, g_hInst, GetCurrentThreadId());
    TlsSetValue(g_tlsHook, hk);
}

static void UnhookThread()
{
    if (g_tlsHook == TLS_OUT_OF_INDEXES) return;
    if (HHOOK hk = (HHOOK)TlsGetValue(g_tlsHook)) { UnhookWindowsHookEx(hk); TlsSetValue(g_tlsHook, nullptr); }
}

BOOL APIENTRY DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        g_hInst = inst;
        if (!ProxyInit()) return FALSE;
        GetModuleFileNameW(inst, g_ini, MAX_PATH);
        wchar_t* slash = wcsrchr(g_ini, L'\\');
        if (slash) slash[1] = 0;
        wcscpy_s(g_log, g_ini); wcscat_s(g_log, L"eacdark.log");
        wcscat_s(g_ini, L"eacdark.ini");
        g_dark = GetPrivateProfileIntW(L"EacDark", L"Enabled", 1, g_ini) != 0;
        g_startedDark = g_dark;
        g_debug = GetPrivateProfileIntW(L"EacDark", L"Debug", 0, g_ini) != 0;
        g_stage = GetPrivateProfileIntW(L"EacDark", L"Stage", 9, g_ini);
        InstallLoaderFix();   // crash fix, independent of dark mode
        if (g_stage == 0) break;
        g_wins = new std::unordered_map<HWND, WinInfo>();
        g_foreign = new std::unordered_set<HWND>();
        g_tlsHook = TlsAlloc();
        HookThread();
        break;
    }
    case DLL_THREAD_ATTACH: if (g_stage >= 1) HookThread(); break;
    case DLL_THREAD_DETACH: UnhookThread(); break;
    case DLL_PROCESS_DETACH: break;
    }
    return TRUE;
}
