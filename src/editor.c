/* レイアウト設定画面: 物理サイズで描いたモニタをドラッグして実際の配置に合わせる */
#include "cursoring.h"
#include <math.h>
#include <stdio.h>
#include <wchar.h>

#define PANEL_DIP  116
#define MARGIN_DIP 28
#define SNAP_DIP   10
#define TIMER_CURSOR 1

enum { IDC_INFO = 200, IDC_LW, IDC_W, IDC_LH, IDC_H, IDC_LX, IDC_X, IDC_LY, IDC_Y,
       IDC_APPLY, IDC_AUTO, IDC_HINT, IDC_SAVE, IDC_CLOSE };

static HWND   s_wnd;
static HFONT  s_font;
static Config s_edit;
static int    s_sel = -1;
static BOOL   s_dirty;
static BOOL   s_drag;
static double s_grabX, s_grabY;             /* つかんだ位置のモニタ左上からのずれ (mm) */
static double s_scale = 1, s_ox, s_oy;      /* 画面 = o + mm * scale */
static POINT  s_lastCursor;

HWND Editor_Window(void) { return s_wnd; }

static int Dpi(void) { return s_wnd ? (int)GetDpiForWindow(s_wnd) : 96; }
static int S(int dip) { return MulDiv(dip, Dpi(), 96); }

static void CanvasRect(RECT *rc)
{
    GetClientRect(s_wnd, rc);
    rc->bottom -= S(PANEL_DIP);
    if (rc->bottom < rc->top) rc->bottom = rc->top;
}

static void Fit(void)
{
    RECT rc;
    CanvasRect(&rc);
    if (s_edit.count == 0) return;
    double l = 1e18, t = 1e18, r = -1e18, b = -1e18;
    for (int i = 0; i < s_edit.count; i++) {
        const Monitor *m = &s_edit.mon[i];
        l = min(l, m->xmm); t = min(t, m->ymm);
        r = max(r, m->xmm + m->wmm); b = max(b, m->ymm + m->hmm);
    }
    double cw = rc.right - 2.0 * S(MARGIN_DIP), ch = rc.bottom - 2.0 * S(MARGIN_DIP);
    if (cw < 10 || ch < 10) return;
    s_scale = min(cw / (r - l), ch / (b - t));
    s_ox = (rc.right - (r - l) * s_scale) / 2 - l * s_scale;
    s_oy = (rc.bottom - (b - t) * s_scale) / 2 - t * s_scale;
}

static void InvalidateCanvas(void)
{
    RECT rc;
    CanvasRect(&rc);
    InvalidateRect(s_wnd, &rc, FALSE);
}

static void SetNum(int id, double v)
{
    WCHAR buf[32];
    swprintf_s(buf, 32, L"%.1f", v);
    SetDlgItemTextW(s_wnd, id, buf);
}

static BOOL GetNum(int id, double *v)
{
    WCHAR buf[32], *end;
    GetDlgItemTextW(s_wnd, id, buf, 32);
    *v = wcstod(buf, &end);
    return end != buf;
}

static void UpdateFields(void)
{
    BOOL has = s_sel >= 0 && s_sel < s_edit.count;
    WCHAR info[256];
    if (has) {
        const Monitor *m = &s_edit.mon[s_sel];
        LONG pw = m->px.right - m->px.left, ph = m->px.bottom - m->px.top;
        swprintf_s(info, 256, L"%d: %s%s   %s   %ld×%ld px   %.1f DPI",
                   s_sel + 1, m->name, m->primary ? L" (メイン)" : L"", m->device, pw, ph,
                   pw / (m->wmm / 25.4));
        SetNum(IDC_W, m->wmm); SetNum(IDC_H, m->hmm);
        SetNum(IDC_X, m->xmm); SetNum(IDC_Y, m->ymm);
    } else {
        wcscpy_s(info, 256, L"モニタをクリックすると選択できます。寸法は数値でも指定できます (単位 mm)。");
        for (int id = IDC_W; id <= IDC_Y; id += 2) SetDlgItemTextW(s_wnd, id, L"");
    }
    SetDlgItemTextW(s_wnd, IDC_INFO, info);
    for (int id = IDC_LW; id <= IDC_APPLY; id++) EnableWindow(GetDlgItem(s_wnd, id), has);
}

static void UpdateTitle(void)
{
    SetWindowTextW(s_wnd, s_dirty ? APP_NAME L" レイアウト設定 *" : APP_NAME L" レイアウト設定");
}

static void MarkDirty(void)
{
    if (!s_dirty) { s_dirty = TRUE; UpdateTitle(); }
}

static void ApplyFields(void)
{
    if (s_sel < 0) return;
    Monitor *m = &s_edit.mon[s_sel];
    double w, h, x, y;
    if (!GetNum(IDC_W, &w) || !GetNum(IDC_H, &h) || !GetNum(IDC_X, &x) || !GetNum(IDC_Y, &y) || w < 1 || h < 1) {
        MessageBeep(MB_ICONWARNING);
        return;
    }
    m->wmm = w; m->hmm = h; m->xmm = x; m->ymm = y;
    MarkDirty();
    Fit();
    InvalidateCanvas();
}

static void Save(void)
{
    /* 全般設定は編集中にトレイから変更されている可能性があるので常駐側の値を使う */
    s_edit.enabled = g_cfg.enabled;
    s_edit.jumpGaps = g_cfg.jumpGaps;
    s_edit.ignoreInjected = g_cfg.ignoreInjected;
    s_edit.maxJumpPx = g_cfg.maxJumpPx;
    Cfg_Normalize(&s_edit);
    App_ApplyConfig(&s_edit);
    s_dirty = FALSE;
    UpdateTitle();
    UpdateFields();
    Fit();
    InvalidateCanvas();
}

static void Paint(HDC hdc)
{
    RECT rc;
    CanvasRect(&rc);
    int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(246, 247, 249));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    POINT cur;
    GetCursorPos(&cur);
    int curMon = Map_MonitorAt(&s_edit, cur);

    HGDIOBJ oldFont = SelectObject(mem, s_font);
    SetBkMode(mem, TRANSPARENT);
    for (int i = 0; i < s_edit.count; i++) {
        const Monitor *m = &s_edit.mon[i];
        RECT r = {
            (LONG)(s_ox + m->xmm * s_scale), (LONG)(s_oy + m->ymm * s_scale),
            (LONG)(s_ox + (m->xmm + m->wmm) * s_scale), (LONG)(s_oy + (m->ymm + m->hmm) * s_scale)
        };
        COLORREF fill = i == s_sel ? RGB(190, 222, 250) : RGB(226, 229, 234);
        COLORREF edge = i == curMon ? RGB(230, 120, 20) : RGB(70, 80, 95);
        HBRUSH br = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_INSIDEFRAME, S(i == curMon ? 3 : 2), edge);
        HGDIOBJ ob = SelectObject(mem, br), op = SelectObject(mem, pen);
        Rectangle(mem, r.left, r.top, r.right, r.bottom);
        SelectObject(mem, ob); SelectObject(mem, op);
        DeleteObject(br); DeleteObject(pen);

        WCHAR text[256];
        swprintf_s(text, 256, L"%d%s\n%s\n%ld×%ld px\n%.1f × %.1f mm",
                   i + 1, m->primary ? L" (メイン)" : L"", m->name,
                   m->px.right - m->px.left, m->px.bottom - m->px.top, m->wmm, m->hmm);
        RECT tr = r;
        InflateRect(&tr, -S(6), -S(6));
        RECT calc = tr;
        DrawTextW(mem, text, -1, &calc, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
        int th = calc.bottom - calc.top;
        if (th < tr.bottom - tr.top) tr.top += (tr.bottom - tr.top - th) / 2;
        SetTextColor(mem, RGB(30, 35, 45));
        DrawTextW(mem, text, -1, &tr, DT_CENTER | DT_WORDBREAK | DT_END_ELLIPSIS);
    }

    /* 現在のカーソル位置を物理配置上に表示 */
    double fx, fy;
    if (Map_PixelToMM(&s_edit, cur, &fx, &fy)) {
        int cx = (int)(s_ox + fx * s_scale), cy = (int)(s_oy + fy * s_scale), rad = S(5);
        HBRUSH br = CreateSolidBrush(RGB(220, 30, 40));
        HGDIOBJ ob = SelectObject(mem, br), op = SelectObject(mem, GetStockObject(NULL_PEN));
        Ellipse(mem, cx - rad, cy - rad, cx + rad, cy + rad);
        SelectObject(mem, ob); SelectObject(mem, op);
        DeleteObject(br);
    }
    SelectObject(mem, oldFont);

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static int HitTest(int x, int y)
{
    double fx = (x - s_ox) / s_scale, fy = (y - s_oy) / s_scale;
    for (int i = s_edit.count - 1; i >= 0; i--) {
        const Monitor *m = &s_edit.mon[i];
        if (fx >= m->xmm && fx < m->xmm + m->wmm && fy >= m->ymm && fy < m->ymm + m->hmm) return i;
    }
    return -1;
}

/* ドラッグ中のモニタの辺を、近くにある他モニタの辺へ吸着させる */
static void Snap(double *pos, double size, BOOL horiz, int self)
{
    double tol = S(SNAP_DIP) / s_scale, best = tol, delta = 0;
    for (int j = 0; j < s_edit.count; j++) {
        if (j == self) continue;
        const Monitor *o = &s_edit.mon[j];
        double a = horiz ? o->xmm : o->ymm;
        double b = a + (horiz ? o->wmm : o->hmm);
        double cand[4][2] = { { *pos, a }, { *pos, b }, { *pos + size, a }, { *pos + size, b } };
        for (int k = 0; k < 4; k++) {
            double d = cand[k][1] - cand[k][0];
            if (fabs(d) < best) { best = fabs(d); delta = d; }
        }
    }
    *pos += delta;
}

static void DragTo(int x, int y)
{
    Monitor *m = &s_edit.mon[s_sel];
    double nx = (x - s_ox) / s_scale - s_grabX;
    double ny = (y - s_oy) / s_scale - s_grabY;
    Snap(&nx, m->wmm, TRUE, s_sel);
    Snap(&ny, m->hmm, FALSE, s_sel);
    if (nx != m->xmm || ny != m->ymm) {
        m->xmm = nx; m->ymm = ny;
        MarkDirty();
        UpdateFields();
        InvalidateCanvas();
    }
}

static HWND Ctl(const WCHAR *cls, const WCHAR *text, int id, DWORD style, DWORD ex)
{
    HWND h = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                             s_wnd, (HMENU)(INT_PTR)id, NULL, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)s_font, FALSE);
    return h;
}

static void MakeFont(void)
{
    if (s_font) DeleteObject(s_font);
    s_font = CreateFontW(-MulDiv(9, Dpi(), 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, 0, L"Yu Gothic UI");
    for (int id = IDC_INFO; id <= IDC_CLOSE; id++)
        SendDlgItemMessageW(s_wnd, id, WM_SETFONT, (WPARAM)s_font, TRUE);
}

static void LayoutControls(void)
{
    RECT rc;
    GetClientRect(s_wnd, &rc);
    int top = rc.bottom - S(PANEL_DIP), pad = S(12), rowH = S(26);
    int y1 = top + S(10), y2 = y1 + S(34), y3 = y2 + S(38);
    int x = pad;
    MoveWindow(GetDlgItem(s_wnd, IDC_INFO), pad, y1, rc.right - 2 * pad, S(22), TRUE);

    static const int ids[4][2] = { { IDC_LW, IDC_W }, { IDC_LH, IDC_H }, { IDC_LX, IDC_X }, { IDC_LY, IDC_Y } };
    for (int i = 0; i < 4; i++) {
        MoveWindow(GetDlgItem(s_wnd, ids[i][0]), x, y2 + S(4), S(56), S(22), TRUE);
        x += S(58);
        MoveWindow(GetDlgItem(s_wnd, ids[i][1]), x, y2, S(76), rowH, TRUE);
        x += S(88);
    }
    MoveWindow(GetDlgItem(s_wnd, IDC_APPLY), x, y2, S(96), rowH, TRUE);

    MoveWindow(GetDlgItem(s_wnd, IDC_AUTO), pad, y3, S(110), rowH, TRUE);
    int bw = S(90);
    MoveWindow(GetDlgItem(s_wnd, IDC_CLOSE), rc.right - pad - bw, y3, bw, rowH, TRUE);
    MoveWindow(GetDlgItem(s_wnd, IDC_SAVE), rc.right - pad - 2 * bw - S(8), y3, bw, rowH, TRUE);
    int hx = pad + S(122);
    MoveWindow(GetDlgItem(s_wnd, IDC_HINT), hx, y3 + S(4), max(0, rc.right - pad - 2 * bw - S(16) - hx), S(22), TRUE);
}

static BOOL ConfirmClose(void)
{
    if (!s_dirty) return TRUE;
    int r = MessageBoxW(s_wnd, L"変更を保存しますか?", APP_NAME, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return FALSE;
    if (r == IDYES) Save();
    return TRUE;
}

static LRESULT CALLBACK EditorProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        s_wnd = hwnd;
        MakeFont();
        Ctl(L"STATIC", L"", IDC_INFO, SS_LEFTNOWORDWRAP, 0);
        Ctl(L"STATIC", L"幅 mm", IDC_LW, SS_RIGHT, 0);
        Ctl(L"EDIT", L"", IDC_W, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        Ctl(L"STATIC", L"高さ mm", IDC_LH, SS_RIGHT, 0);
        Ctl(L"EDIT", L"", IDC_H, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        Ctl(L"STATIC", L"X mm", IDC_LX, SS_RIGHT, 0);
        Ctl(L"EDIT", L"", IDC_X, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        Ctl(L"STATIC", L"Y mm", IDC_LY, SS_RIGHT, 0);
        Ctl(L"EDIT", L"", IDC_Y, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE);
        Ctl(L"BUTTON", L"数値を適用", IDC_APPLY, WS_TABSTOP | BS_PUSHBUTTON, 0);
        Ctl(L"BUTTON", L"自動配置に戻す", IDC_AUTO, WS_TABSTOP | BS_PUSHBUTTON, 0);
        Ctl(L"STATIC", L"ドラッグで実際の配置に合わせてください。赤い点は現在のカーソル位置です。",
            IDC_HINT, SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0);
        Ctl(L"BUTTON", L"保存", IDC_SAVE, WS_TABSTOP | BS_PUSHBUTTON, 0);
        Ctl(L"BUTTON", L"閉じる", IDC_CLOSE, WS_TABSTOP | BS_PUSHBUTTON, 0);
        s_edit = g_cfg;
        s_sel = -1;
        s_dirty = FALSE;
        UpdateFields();
        SetTimer(hwnd, TIMER_CURSOR, 33, NULL);
        return 0;

    case WM_SIZE:
        LayoutControls();
        if (!s_drag) Fit();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(720);
        mm->ptMinTrackSize.y = S(460);
        return 0;
    }

    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        MakeFont();
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }

    case WM_TIMER: {
        POINT p;
        GetCursorPos(&p);
        if (p.x != s_lastCursor.x || p.y != s_lastCursor.y) {
            s_lastCursor = p;
            InvalidateCanvas();
        }
        return 0;
    }

    case WM_ERASEBKGND: {
        /* キャンバス部分は WM_PAINT で全面描画するので、下のパネルだけ塗る */
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.top = rc.bottom - S(PANEL_DIP);
        FillRect((HDC)wp, &rc, GetSysColorBrush(COLOR_BTNFACE));
        return 1;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        Paint(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_LBUTTONDOWN: {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp);
        RECT rc;
        CanvasRect(&rc);
        if (y >= rc.bottom) return 0;
        SetFocus(hwnd);
        s_sel = HitTest(x, y);
        if (s_sel >= 0) {
            const Monitor *m = &s_edit.mon[s_sel];
            s_grabX = (x - s_ox) / s_scale - m->xmm;
            s_grabY = (y - s_oy) / s_scale - m->ymm;
            s_drag = TRUE;
            SetCapture(hwnd);
        }
        UpdateFields();
        InvalidateCanvas();
        return 0;
    }

    case WM_MOUSEMOVE:
        if (s_drag && s_sel >= 0) DragTo((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (s_drag) {
            s_drag = FALSE;
            if (msg == WM_LBUTTONUP) ReleaseCapture();
            Fit();
            InvalidateCanvas();
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
        case IDC_APPLY: ApplyFields(); break;
        case IDC_AUTO:
            Cfg_AutoLayout(&s_edit);
            MarkDirty();
            UpdateFields();
            Fit();
            InvalidateCanvas();
            break;
        case IDC_SAVE: Save(); break;
        case IDCANCEL:
        case IDC_CLOSE: if (ConfirmClose()) DestroyWindow(hwnd); break;
        }
        return 0;

    case WM_APP:  /* 常駐側でモニタ構成を読み直した */
        s_edit = g_cfg;
        s_sel = -1;
        s_dirty = FALSE;
        UpdateTitle();
        UpdateFields();
        Fit();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;

    case WM_CLOSE:
        if (ConfirmClose()) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_CURSOR);
        s_wnd = NULL;
        if (s_font) { DeleteObject(s_font); s_font = NULL; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Editor_Open(HINSTANCE hinst, HICON icon)
{
    if (s_wnd) {
        if (IsIconic(s_wnd)) ShowWindow(s_wnd, SW_RESTORE);
        SetForegroundWindow(s_wnd);
        return;
    }
    static BOOL registered;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(wc) };
        wc.lpfnWndProc = EditorProc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = icon;
        wc.hIconSm = icon;
        wc.lpszClassName = APP_NAME L"_Editor";
        RegisterClassExW(&wc);
        registered = TRUE;
    }
    UINT dpi = GetDpiForSystem();
    HWND h = CreateWindowExW(WS_EX_CONTROLPARENT, APP_NAME L"_Editor", APP_NAME L" レイアウト設定",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                             MulDiv(900, dpi, 96), MulDiv(620, dpi, 96), NULL, NULL, hinst, NULL);
    ShowWindow(h, SW_SHOWNORMAL);
    SetForegroundWindow(h);
}
