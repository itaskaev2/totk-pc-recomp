// The loading window on Win32 (loadwin.h): a borderless panel in the middle of the primary monitor's work area, drawn
// with GDI on a thread of its own, so it keeps moving while the main and prewarm threads work. It shows the program's
// icon, its name and version, the stage, a bar with the share done (an animated bar when nothing is counted), the counts
// and times under it, and a Details button that expands an edit control with the log. The panel can be dragged by any
// free spot; its close button ends the game, as closing the console did.
//
// The process is per-monitor DPI aware (totk.manifest): the layout below is in 96-DPI units scaled by the panel's DPI.
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <dwmapi.h>

#include "host.h"
#include "loadwin.h"
#include "platform/platform.h"
#include "ui_text.h"

enum { PANEL_W = 600, PANEL_H = 372, LOG_H = 300, PAD = 28, BTN_W = 150, BTN_H = 32 };
enum { ID_DETAILS = 101, ID_COPY, ID_CLOSE, ID_LOG };
enum { WM_LW_CLOSE = WM_APP + 1, WM_LW_FATAL };
enum { TIMER_ID = 1, TIMER_MS = 50 };

#define C_BG RGB(16, 13, 10)
#define C_EDGE RGB(74, 60, 40)
#define C_TEXT RGB(243, 234, 216)
#define C_DIM RGB(168, 159, 142)
#define C_GOLD RGB(190, 152, 92)
#define C_BAR_BG RGB(26, 21, 16)
#define C_BAR_EDGE RGB(92, 74, 51)
#define C_BAR_FILL RGB(150, 116, 62)
#define C_LOG_BG RGB(10, 8, 6)
#define C_LOG_TEXT RGB(214, 206, 190)
#define C_BTN RGB(36, 29, 22)
#define C_BTN_DOWN RGB(56, 45, 33)
#define C_FAIL RGB(222, 128, 100)

static struct {
    HWND hwnd, details, copy, close, log;
    DWORD thread_id;
    PlatEvent ready, gone;
    int ok, background, expanded, by_player, copied_ticks;
    UINT dpi;
    unsigned anim;
    HFONT f_title, f_ver, f_stage, f_body, f_small, f_btn, f_log;
    HBRUSH br_log;
    HICON icon;
    WNDPROC edit_proc;
    LwView v;
} G;

static int S(int x) { return MulDiv(x, (int)G.dpi, 96); }

static HFONT font(int px, int weight, const wchar_t *face) {
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, face);
}

static void fonts_free(void) {
    HFONT *f[] = {&G.f_title, &G.f_ver, &G.f_stage, &G.f_body, &G.f_small, &G.f_btn, &G.f_log};
    for (size_t i = 0; i < sizeof f / sizeof *f; i++)
        if (*f[i]) DeleteObject(*f[i]), *f[i] = NULL;
    if (G.icon) DestroyIcon(G.icon), G.icon = NULL;
}

// Fonts and the icon at the panel's DPI. Text with Chinese glyphs falls back from Segoe UI to the system's CJK font
// through GDI font linking.
static void fonts_make(void) {
    fonts_free();
    G.f_title = font(30, FW_BOLD, L"Georgia");
    G.f_ver = font(13, FW_NORMAL, L"Segoe UI");
    G.f_stage = font(21, FW_SEMIBOLD, L"Segoe UI");
    G.f_body = font(15, FW_NORMAL, L"Segoe UI");
    G.f_small = font(13, FW_NORMAL, L"Segoe UI");
    G.f_btn = font(14, FW_NORMAL, L"Segoe UI");
    G.f_log = font(13, FW_NORMAL, L"Consolas");
    // Icon resource 1 of totk.rc (runtime/totk.ico), the size the panel draws it at
    G.icon = LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, S(72), S(72), 0);
    if (G.log) SendMessageW(G.log, WM_SETFONT, (WPARAM)G.f_log, TRUE);
}

// ---- text
static void draw_u8(HDC dc, const char *u8, RECT r, UINT fmt, HFONT f, COLORREF c) {
    wchar_t w[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, (int)(sizeof w / sizeof *w))) return;
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, w, -1, &r, fmt | DT_NOPREFIX);
}

// 12345 with the menu language's digit group separator: "12,345", "12 345", "12.345"
static void grouped(uint64_t v, char *out, size_t cap) {
    char d[32];
    int n = snprintf(d, sizeof d, "%llu", (unsigned long long)v);
    const char *sep = menu_text(UI_LW_THOUSANDS);
    size_t sl = strlen(sep), o = 0;
    for (int i = 0; i < n && o + sl + 2 < cap; i++) {
        if (i && (n - i) % 3 == 0) memcpy(out + o, sep, sl), o += sl;
        out[o++] = d[i];
    }
    out[o] = 0;
}

// Seconds left as the panel says them: a few seconds under 10, 5-second steps under a minute, whole minutes above
static void left_text(double s, char *out, size_t cap) {
    if (s < 10) snprintf(out, cap, "%s", menu_text(UI_LW_LEFT_FEW));
    else if (s < 57.5) snprintf(out, cap, menu_text(UI_LW_LEFT_SEC), (int)((s + 4.999) / 5) * 5);
    else snprintf(out, cap, menu_text(UI_LW_LEFT_MIN), s < 90 ? 1 : (int)(s / 60 + 0.5));
}

static void game_text(double s, char *out, size_t cap) {
    if (s < 3) snprintf(out, cap, "%s", menu_text(UI_LW_GAME_SOON));
    else if (s < 57.5) snprintf(out, cap, menu_text(UI_LW_GAME_SEC), (int)((s + 4.999) / 5) * 5);
    else snprintf(out, cap, menu_text(UI_LW_GAME_MIN), s < 90 ? 1 : (int)(s / 60 + 0.5));
}

static const char *stage_name(int s) {
    static const UiTextId ids[LW_STAGES] = {UI_LW_STAGE_START, UI_LW_STAGE_GPU, UI_LW_STAGE_FILES,
                                            UI_LW_STAGE_SHADERS, UI_LW_STAGE_PIPELINES, UI_LW_STAGE_GAME};
    return menu_text(s >= 0 && s < LW_STAGES ? ids[s] : UI_LW_STAGE_START);
}

static void count_text(uint64_t done, uint64_t total, char *out, size_t cap) {
    char a[40], b[40];
    grouped(done, a, sizeof a);
    grouped(total, b, sizeof b);
    snprintf(out, cap, menu_text(UI_LW_COUNT), a, b);
}

// ---- drawing
static void fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void frame(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, &r, b);
    DeleteObject(b);
}

static void paint(HDC dc, RECT cr) {
    const LwView *v = &G.v;
    int w = cr.right, cx = w / 2;
    fill(dc, cr, C_BG);
    SetBkMode(dc, TRANSPARENT);
    if (G.icon) DrawIconEx(dc, cx - S(36), S(22), G.icon, S(72), S(72), 0, NULL, DI_NORMAL);

    // The name in spaced capitals, centred by its measured width: DrawText's DT_CENTER leaves the spacing out of the
    // width it centres, and the spacing after the last letter is not part of the word
    {
        static const wchar_t name[] = L"TOTK RECOMP";
        int extra = S(4), n = (int)(sizeof name / sizeof *name) - 1;
        SelectObject(dc, G.f_title);
        SetTextCharacterExtra(dc, extra);
        SetTextColor(dc, C_TEXT);
        SIZE sz;
        GetTextExtentPoint32W(dc, name, n, &sz);
        TextOutW(dc, cx - (sz.cx - extra) / 2, S(120) - sz.cy / 2, name, n);
        SetTextCharacterExtra(dc, 0);
    }

    // A rule with a small diamond in its middle
    HPEN pen = CreatePen(PS_SOLID, 1, C_BAR_EDGE), old_pen = SelectObject(dc, pen);
    int y = S(150);
    MoveToEx(dc, cx - S(150), y, NULL), LineTo(dc, cx - S(12), y);
    MoveToEx(dc, cx + S(12), y, NULL), LineTo(dc, cx + S(150), y);
    SelectObject(dc, old_pen);
    DeleteObject(pen);
    HBRUSH gold = CreateSolidBrush(C_GOLD), old_brush = SelectObject(dc, gold);
    HPEN gpen = CreatePen(PS_SOLID, 1, C_GOLD);
    old_pen = SelectObject(dc, gpen);
    POINT dia[4] = {{cx, y - S(4)}, {cx + S(4), y}, {cx, y + S(4)}, {cx - S(4), y}};
    Polygon(dc, dia, 4);
    SelectObject(dc, old_brush), SelectObject(dc, old_pen);
    DeleteObject(gold), DeleteObject(gpen);

    char line[512], a[160], b[160];
    char game[16];
    snprintf(game, sizeof game, "%d.%d.%d", rt_game_version / 100, rt_game_version / 10 % 10, rt_game_version % 10);
    snprintf(line, sizeof line, menu_text(UI_LW_VERSION), rt_port_version, game);
    draw_u8(dc, line, (RECT){0, S(157), w, S(177)}, DT_CENTER | DT_SINGLELINE | DT_VCENTER, G.f_ver, C_DIM);

    draw_u8(dc, v->failed ? menu_text(UI_LW_FAILED) : stage_name(v->stage), (RECT){S(PAD), S(188), w - S(PAD), S(220)},
            DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, G.f_stage, v->failed ? C_FAIL : C_TEXT);

    if (v->failed) {   // the reason in place of the bar, what to do under it
        draw_u8(dc, v->why[0] ? v->why : menu_text(UI_LW_FAILED_HINT), (RECT){S(PAD), S(226), w - S(PAD), S(282)},
                DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS, G.f_body, C_TEXT);
        if (v->why[0])
            draw_u8(dc, menu_text(UI_LW_FAILED_HINT), (RECT){S(PAD), S(284), w - S(PAD), S(316)}, DT_LEFT | DT_WORDBREAK,
                    G.f_small, C_DIM);
    }
    // The bar: the share done, or a moving block while nothing is counted
    RECT bar = {S(PAD), S(228), w - S(PAD), S(254)};
    if (!v->failed) fill(dc, bar, C_BAR_BG);
    int bw = bar.right - bar.left;
    if (!v->failed && v->frac >= 0) {
        RECT f = bar;
        f.right = bar.left + (int)(bw * (v->frac > 1 ? 1 : v->frac));
        fill(dc, f, C_BAR_FILL);
    } else if (!v->failed) {
        int seg = bw / 4, span = bw + seg, at = (int)((G.anim * 9u) % (unsigned)span) - seg;
        RECT f = {bar.left + (at < 0 ? 0 : at), bar.top, bar.left + (at + seg > bw ? bw : at + seg), bar.bottom};
        if (f.right > f.left) fill(dc, f, C_BAR_FILL);
    }
    if (!v->failed) frame(dc, bar, C_BAR_EDGE);
    if (!v->failed && v->frac >= 0) {
        snprintf(line, sizeof line, "%d%%", (int)(v->frac * 100));
        draw_u8(dc, line, bar, DT_CENTER | DT_SINGLELINE | DT_VCENTER, G.f_body, C_TEXT);
    }

    // Under the bar: the counts and the time left, or what to do after a failure
    RECT r1 = {S(PAD), S(262), w - S(PAD), S(284)}, r2 = {S(PAD), S(285), w - S(PAD), S(305)};
    if (!v->failed) {
        if (v->total) {
            count_text(v->done, v->total, a, sizeof a);
            grouped(v->total - v->done, b, sizeof b);
            int n = snprintf(line, sizeof line, "%s  \xc2\xb7  ", a);
            n += snprintf(line + n, sizeof line - n, menu_text(UI_LW_LEFT_ITEMS), b);
            if (v->task_left_s >= 0 && v->done < v->total) {
                left_text(v->task_left_s, a, sizeof a);
                snprintf(line + n, sizeof line - n, "  \xc2\xb7  %s", a);
            }
            draw_u8(dc, line, r1, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, G.f_body, C_TEXT);
        }
        if (v->other >= 0) {
            char what[256];
            if (v->other_total) {
                count_text(v->other_done, v->other_total, a, sizeof a);
                snprintf(what, sizeof what, "%s: %s", stage_name(v->other), a);
            } else {
                snprintf(what, sizeof what, "%s", stage_name(v->other));
            }
            snprintf(line, sizeof line, menu_text(UI_LW_ALSO), what);
            draw_u8(dc, line, r2, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, G.f_small, C_DIM);
        }
    }

    // Bottom row: the time so far and until the game starts, left of the buttons
    int secs = (int)v->elapsed_s;
    int n = snprintf(line, sizeof line, menu_text(UI_LW_ELAPSED), secs / 60, secs % 60);
    if (!v->failed && v->game_left_s >= 0) {
        game_text(v->game_left_s, a, sizeof a);
        snprintf(line + n, sizeof line - n, "  \xc2\xb7  %s", a);
    }
    int right = w - S(PAD) - S(BTN_W) - S(12);
    draw_u8(dc, line, (RECT){S(PAD), S(320), right, S(320 + BTN_H)}, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS,
            G.f_small, C_DIM);
    frame(dc, cr, C_EDGE);
}

static void draw_button(const DRAWITEMSTRUCT *d) {
    HDC dc = d->hDC;
    RECT r = d->rcItem;
    int down = (d->itemState & ODS_SELECTED) != 0;
    SetBkMode(dc, TRANSPARENT);
    if (d->CtlID == ID_CLOSE) {
        fill(dc, r, down ? C_BTN_DOWN : C_BG);
        draw_u8(dc, "\xc3\x97", r, DT_CENTER | DT_SINGLELINE | DT_VCENTER, G.f_stage, C_DIM);   // U+00D7
        return;
    }
    fill(dc, r, C_BG);
    HBRUSH b = CreateSolidBrush(down ? C_BTN_DOWN : C_BTN);
    HPEN p = CreatePen(PS_SOLID, 1, C_BAR_EDGE);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(8), S(8));
    SelectObject(dc, ob), SelectObject(dc, op);
    DeleteObject(b), DeleteObject(p);
    char text[160];
    if (d->CtlID == ID_DETAILS)
        snprintf(text, sizeof text, "%s %s", menu_text(G.expanded ? UI_LW_HIDE : UI_LW_DETAILS),
                 G.expanded ? "\xe2\x96\xb4" : "\xe2\x96\xbe");   // U+25B4, U+25BE
    else snprintf(text, sizeof text, "%s", menu_text(G.copied_ticks ? UI_LW_COPIED : UI_LW_COPY));
    draw_u8(dc, text, r, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS, G.f_btn, C_TEXT);
}

// ---- layout
// Expanded: the log, then the Copy button under its right edge
static int panel_height(void) { return S(PANEL_H) + (G.expanded ? S(LOG_H) + S(12) + S(BTN_H) + S(20) : 0); }

// Places the children and sizes the panel; the panel keeps its top edge unless the expanded panel would leave the
// monitor's work area, then it moves up.
static void lay_out(void) {
    int w = S(PANEL_W), h = panel_height();
    RECT wr;
    GetWindowRect(G.hwnd, &wr);
    MONITORINFO mi = {.cbSize = sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(G.hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    int top = wr.top;
    if (top + h > mi.rcWork.bottom) top = mi.rcWork.bottom - h;
    if (top < mi.rcWork.top) top = mi.rcWork.top;
    SetWindowPos(G.hwnd, NULL, wr.left, top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(G.close, NULL, w - S(40), S(8), S(32), S(32), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(G.details, NULL, w - S(PAD) - S(BTN_W), S(320), S(BTN_W), S(BTN_H), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(G.copy, NULL, w - S(PAD) - S(BTN_W), S(PANEL_H) + S(LOG_H) + S(12), S(BTN_W), S(BTN_H),
                 SWP_NOZORDER | SWP_NOACTIVATE | (G.expanded ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    SetWindowPos(G.log, NULL, S(PAD), S(PANEL_H), w - 2 * S(PAD), S(LOG_H), SWP_NOZORDER | SWP_NOACTIVATE |
                 (G.expanded ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    InvalidateRect(G.hwnd, NULL, FALSE);
    InvalidateRect(G.details, NULL, FALSE);
}

static void set_expanded(int on) {
    if (G.expanded == on) return;
    G.expanded = on;
    lay_out();
    if (on) SendMessageW(G.log, WM_VSCROLL, SB_BOTTOM, 0);
}

// ---- the log view
enum { LOG_KEEP_CHARS = 2000000, LOG_CUT_CHARS = 500000 };

// Appends UTF-8 log text, its line ends made CRLF. A reader scrolled up keeps the lines in view; one at the end
// follows the new lines. Past LOG_KEEP_CHARS the oldest lines go (the log file keeps them).
static void log_append(const char *u8, size_t n) {
    size_t nl = 0;
    for (size_t i = 0; i < n; i++) nl += u8[i] == '\n' && (i == 0 || u8[i - 1] != '\r');
    char *crlf = malloc(n + nl + 1);
    if (!crlf) return;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (u8[i] == '\n' && (i == 0 || u8[i - 1] != '\r')) crlf[o++] = '\r';
        crlf[o++] = u8[i] ? u8[i] : ' ';
    }
    crlf[o] = 0;
    int wn = MultiByteToWideChar(CP_UTF8, 0, crlf, (int)o + 1, NULL, 0);
    wchar_t *w = wn > 0 ? malloc((size_t)wn * sizeof *w) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, crlf, (int)o + 1, w, wn);
    free(crlf);
    if (!w) return;

    HWND e = G.log;
    SCROLLINFO si = {.cbSize = sizeof si, .fMask = SIF_ALL};
    GetScrollInfo(e, SB_VERT, &si);
    int follow = !G.expanded || si.nMax <= (int)si.nPage || si.nPos + (int)si.nPage > si.nMax;
    DWORD s0 = 0, s1 = 0;
    SendMessageW(e, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
    int first = (int)SendMessageW(e, EM_GETFIRSTVISIBLELINE, 0, 0);
    SendMessageW(e, WM_SETREDRAW, FALSE, 0);
    int len = GetWindowTextLengthW(e);
    if (len > LOG_KEEP_CHARS) {
        int line = (int)SendMessageW(e, EM_LINEFROMCHAR, LOG_CUT_CHARS, 0);
        int cut = (int)SendMessageW(e, EM_LINEINDEX, line + 1, 0);
        if (cut > 0) {
            SendMessageW(e, EM_SETSEL, 0, cut);
            SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)L"");
            len -= cut;
            first = first > line + 1 ? first - (line + 1) : 0;
            s0 = s0 > (DWORD)cut ? s0 - (DWORD)cut : 0;
            s1 = s1 > (DWORD)cut ? s1 - (DWORD)cut : 0;
        }
    }
    SendMessageW(e, EM_SETSEL, len, len);
    SendMessageW(e, EM_REPLACESEL, FALSE, (LPARAM)w);
    free(w);
    if (s0 != s1) SendMessageW(e, EM_SETSEL, s0, s1);
    else if (!follow) SendMessageW(e, EM_SETSEL, s0, s0);
    if (follow) {
        SendMessageW(e, WM_VSCROLL, SB_BOTTOM, 0);
    } else {
        int now = (int)SendMessageW(e, EM_GETFIRSTVISIBLELINE, 0, 0);
        SendMessageW(e, EM_LINESCROLL, 0, first - now);
    }
    SendMessageW(e, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(e, NULL, TRUE);
}

static void log_copy(void) {
    int n = GetWindowTextLengthW(G.log);
    wchar_t *w = malloc(((size_t)n + 1) * sizeof *w);
    if (!w) return;
    GetWindowTextW(G.log, w, n + 1);
    int un = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *u = un > 0 ? malloc((size_t)un) : NULL;
    if (u && WideCharToMultiByte(CP_UTF8, 0, w, -1, u, un, NULL, NULL) && plat_clipboard_set(u))
        G.copied_ticks = 2000 / TIMER_MS;
    free(u);
    free(w);
    InvalidateRect(G.copy, NULL, FALSE);
}

// The read-only edit control knows no Ctrl+A: it selects everything here, for a copy with Ctrl+C.
static LRESULT CALLBACK log_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == 'A' && GetKeyState(VK_CONTROL) < 0) {
        SendMessageW(h, EM_SETSEL, 0, -1);
        return 0;
    }
    if (m == WM_CHAR && w == 1) return 0;   // the Ctrl+A character would beep
    return CallWindowProcW(G.edit_proc, h, m, w, l);
}

// ---- the window
static void tick(void) {
    G.anim++;
    if (G.copied_ticks && !--G.copied_ticks) InvalidateRect(G.copy, NULL, FALSE);
    lw_view(&G.v);
    size_t n = 0;
    char *t = lw_log_take(&n);
    if (t) log_append(t, n), free(t);
    RECT top = {0, 0, S(PANEL_W), S(PANEL_H)};
    InvalidateRect(G.hwnd, &top, FALSE);
}

static LRESULT CALLBACK panel_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: {   // the free parts of the panel drag it
        LRESULT r = DefWindowProcW(h, m, w, l);
        return r == HTCLIENT ? HTCAPTION : r;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT cr;
        GetClientRect(h, &cr);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom), old = SelectObject(mem, bmp);
        paint(mem, cr);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (w == TIMER_ID) tick();
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == ID_DETAILS) set_expanded(!G.expanded);
        else if (LOWORD(w) == ID_COPY) log_copy();
        else if (LOWORD(w) == ID_CLOSE) PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_DRAWITEM:
        draw_button((const DRAWITEMSTRUCT *)l);
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        if ((HWND)l == G.log) {
            SetTextColor((HDC)w, C_LOG_TEXT);
            SetBkColor((HDC)w, C_LOG_BG);
            return (LRESULT)G.br_log;
        }
        break;
    case WM_DPICHANGED: {
        G.dpi = HIWORD(w);
        fonts_make();
        const RECT *s = (const RECT *)l;
        SetWindowPos(h, NULL, s->left, s->top, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSIZE);
        lay_out();
        return 0;
    }
    case WM_LW_CLOSE:
        G.by_player = 0;
        DestroyWindow(h);
        return 0;
    case WM_LW_FATAL:
        lw_view(&G.v);
        tick();
        set_expanded(1);
        ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_CLOSE:   // the close button, Alt+F4, the taskbar
        G.by_player = 1;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, TIMER_ID);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND child(const wchar_t *cls, DWORD style, int id) {
    return CreateWindowExW(0, cls, L"", WS_CHILD | style, 0, 0, 10, 10, G.hwnd, (HMENU)(INT_PTR)id,
                           GetModuleHandleW(NULL), NULL);
}

// Dark scroll bars for the log where the theme has them (uxtheme is loaded here, not linked)
static void dark_scrollbars(HWND h) {
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    typedef HRESULT(WINAPI * SetTheme)(HWND, LPCWSTR, LPCWSTR);
    SetTheme set = ux ? (SetTheme)(void (*)(void))GetProcAddress(ux, "SetWindowTheme") : NULL;
    if (set) set(h, L"DarkMode_Explorer", NULL);
}

static void panel_main(void *arg) {
    (void)arg;
    G.thread_id = GetCurrentThreadId();
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = panel_proc, .hInstance = inst,
                      .hIcon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                          GetSystemMetrics(SM_CYICON), LR_SHARED),
                      .hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                            GetSystemMetrics(SM_CYSMICON), LR_SHARED),
                      .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"TotKRecompLoading"};
    RegisterClassExW(&wc);
    G.dpi = GetDpiForSystem();
    MONITORINFO mi = {.cbSize = sizeof mi};
    GetMonitorInfoW(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
    int w = S(PANEL_W), h = panel_height();
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
    G.hwnd = CreateWindowExW(WS_EX_APPWINDOW | (G.background ? WS_EX_NOACTIVATE : 0), wc.lpszClassName, L"TotK Recomp",
                             WS_POPUP | WS_CLIPCHILDREN | WS_SYSMENU | WS_MINIMIZEBOX, x, y, w, h, NULL, NULL, inst, NULL);
    if (!G.hwnd) {
        rt_log("loadwin: CreateWindowEx failed (%lu)", GetLastError());
        plat_event_set(&G.ready);
        plat_event_set(&G.gone);
        return;
    }
    UINT dpi = GetDpiForWindow(G.hwnd);
    if (dpi) G.dpi = dpi;
    fonts_make();
    G.br_log = CreateSolidBrush(C_LOG_BG);
    // Windows 11: rounded corners and a border in the panel's colour; earlier versions ignore both
    int corner = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(G.hwnd, 33, &corner, sizeof corner);   // DWMWA_WINDOW_CORNER_PREFERENCE
    COLORREF edge = C_EDGE;
    DwmSetWindowAttribute(G.hwnd, 34, &edge, sizeof edge);       // DWMWA_BORDER_COLOR
    G.close = child(L"BUTTON", WS_VISIBLE | BS_OWNERDRAW, ID_CLOSE);
    G.details = child(L"BUTTON", WS_VISIBLE | BS_OWNERDRAW, ID_DETAILS);
    G.copy = child(L"BUTTON", BS_OWNERDRAW, ID_COPY);
    G.log = child(L"EDIT", WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_NOHIDESEL, ID_LOG);
    SendMessageW(G.log, EM_SETLIMITTEXT, 0, 0);
    SendMessageW(G.log, WM_SETFONT, (WPARAM)G.f_log, FALSE);
    SendMessageW(G.log, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(6), S(6)));
    dark_scrollbars(G.log);
    G.edit_proc = (WNDPROC)SetWindowLongPtrW(G.log, GWLP_WNDPROC, (LONG_PTR)log_proc);
    lw_view(&G.v);
    lay_out();
    if (G.background) {
        SetWindowPos(G.hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
        ShowWindow(G.hwnd, SW_SHOW);
        SetForegroundWindow(G.hwnd);
    }
    SetTimer(G.hwnd, TIMER_ID, TIMER_MS, NULL);
    G.ok = 1;
    plat_event_set(&G.ready);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    G.hwnd = NULL;
    fonts_free();
    DeleteObject(G.br_log);
    lw_closed(G.by_player);
    plat_event_set(&G.gone);
}

int lw_wsys_open(int background) {
    G.background = background;
    plat_event_init(&G.ready, 1, 0);
    plat_event_init(&G.gone, 1, 0);
    if (!plat_thread_spawn(panel_main, NULL, "loading window", 0)) return 0;
    plat_event_wait(&G.ready);
    return G.ok;
}

void lw_wsys_close(void) {
    HWND h = G.hwnd;
    if (h) PostMessageW(h, WM_LW_CLOSE, 0, 0);
}

void lw_wsys_fatal(void) {
    HWND h = G.hwnd;
    if (h) PostMessageW(h, WM_LW_FATAL, 0, 0);
}

void lw_wsys_wait_closed(void) { plat_event_wait(&G.gone); }

int lw_wsys_on_panel_thread(void) { return GetCurrentThreadId() == G.thread_id; }

// A console that only this process uses was made for it by Windows (a start from Explorer or a shortcut): leaving it
// closes its window, in conhost and in Windows Terminal alike. A console shared with a shell stays.
int lw_wsys_detach_console(void) {
    DWORD ids[2];
    if (!GetConsoleWindow() || GetConsoleProcessList(ids, 2) != 1) return 0;
    fflush(stdout);
    fflush(stderr);
    if (!FreeConsole()) return 0;
    // The C streams still hold the console's handles, now closed. The log file opened next (main.c log_open) can get
    // the same handle value, and stderr writes would then land in the log a second time: the streams go to NUL.
    FILE *f;
    if ((f = freopen("NUL", "w", stdout))) SetStdHandle(STD_OUTPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(f)));
    if ((f = freopen("NUL", "w", stderr))) SetStdHandle(STD_ERROR_HANDLE, (HANDLE)_get_osfhandle(_fileno(f)));
    if ((f = freopen("NUL", "r", stdin))) SetStdHandle(STD_INPUT_HANDLE, (HANDLE)_get_osfhandle(_fileno(f)));
    return 1;
}
