/* 常駐部分: タスクトレイ、低レベルマウスフック、多重起動防止 */
#include "cursoring.h"
#include <shellapi.h>
#include <wtsapi32.h>

#define WM_TRAY        (WM_APP + 1)
#define WM_OPEN_EDITOR (WM_APP + 2)
#define TIMER_RELOAD   1
#define TIMER_WATCH    2
#define TIMER_SIMLOSS  3
#define WATCH_MS       1000
#define STATS_TICKS    600   /* 10 分ごとに補正回数をログへ */

enum { ID_ENABLE = 100, ID_JUMP, ID_EDITOR, ID_RELOAD, ID_OPENINI, ID_EXIT };

/* ペン/タッチから生成されたマウスメッセージの識別子 (MI_WP_SIGNATURE) */
#define PEN_SIGNATURE_MASK 0xFFFFFF00
#define PEN_SIGNATURE      0xFF515700

Config g_cfg;
static HINSTANCE g_inst;
static HWND  g_wnd;
static HHOOK g_hook;
static HICON g_iconOn, g_iconOff;
static UINT  g_msgTaskbarCreated;

/* フックと監視タイマーは同じスレッドで動くので排他は不要 */
static ULONG g_hookCalls, g_moved, g_blocked;

static BOOL CursorIsClipped(void)
{
    RECT rc;
    if (!GetClipCursor(&rc)) return FALSE;
    return rc.left > GetSystemMetrics(SM_XVIRTUALSCREEN) ||
           rc.top > GetSystemMetrics(SM_YVIRTUALSCREEN) ||
           rc.right < GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) ||
           rc.bottom < GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

/* フックに届く pt は Windows が画面内へ切り詰める前の値なので、はみ出しを検出できる */
static LRESULT CALLBACK LowLevelMouseProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) g_hookCalls++;
    if (code == HC_ACTION && wp == WM_MOUSEMOVE && g_cfg.enabled && g_cfg.count > 1) {
        const MSLLHOOKSTRUCT *ms = (const MSLLHOOKSTRUCT *)lp;
        BOOL skip = (ms->dwExtraInfo & PEN_SIGNATURE_MASK) == PEN_SIGNATURE ||
                    (g_cfg.ignoreInjected && (ms->flags & LLMHF_INJECTED));
        POINT cur, out;
        /* ゲーム等がカーソル範囲を制限しているときは邪魔しない */
        if (!skip && GetCursorPos(&cur) && !CursorIsClipped()) {
            MapResult r = Map_Translate(&g_cfg, Map_MonitorAt(&g_cfg, cur), ms->pt, &out);
            if (r != MAP_PASS) {
                if (r == MAP_MOVE) g_moved++; else g_blocked++;
                SetCursorPos(out.x, out.y);
                return 1;
            }
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static void InstallHook(const WCHAR *reason)
{
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, g_inst, 0);
    if (g_hook) Log_Write(L"hook installed (%s)", reason);
    else        Log_Write(L"hook install FAILED (%s) err=%lu", reason, GetLastError());
}

/* 1 秒ごとの監視。
 * - カーソルが動いているのにフックが一度も呼ばれていなければ、フックが失われたとみなして付け直す
 * - 変更通知を取りこぼしてもモニタ構成の変化に追従する */
static void Watch(void)
{
    static POINT lastPos;
    static ULONG lastCalls, ticks, reinstalls, lastMoved, lastBlocked;
    static int silent;

    POINT pos;
    if (GetCursorPos(&pos)) {
        BOOL moved = pos.x != lastPos.x || pos.y != lastPos.y;
        if (moved && g_hookCalls == lastCalls) {
            if (++silent >= 3) {
                silent = 0;
                reinstalls++;
                /* ペン入力などフックを通らない移動で繰り返し起きうるので、ログは間引く */
                if (reinstalls <= 3 || reinstalls % 100 == 0) {
                    Log_Write(L"watchdog: cursor moved for 3s without hook calls (#%lu)", reinstalls);
                    InstallHook(L"watchdog");
                } else {
                    if (g_hook) UnhookWindowsHookEx(g_hook);
                    g_hook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, g_inst, 0);
                }
            }
        } else {
            silent = 0;
        }
        lastPos = pos;
        lastCalls = g_hookCalls;
    }

    ticks++;
    if (ticks % 2 == 0 && Cfg_LayoutChanged(&g_cfg)) {
        Log_Write(L"layout check: monitor configuration differs from loaded config -> reload");
        KillTimer(g_wnd, TIMER_RELOAD);
        Cfg_Load(&g_cfg);
        if (Editor_Window()) PostMessageW(Editor_Window(), WM_APP, 0, 0);
    }
    if (ticks % STATS_TICKS == 0 && (g_moved != lastMoved || g_blocked != lastBlocked)) {
        Log_Write(L"stats: moved=%lu blocked=%lu hookCalls=%lu", g_moved, g_blocked, g_hookCalls);
        lastMoved = g_moved;
        lastBlocked = g_blocked;
    }
}

/* 大小2つのモニタを並べた図柄のアイコンを実行時に描く (リソースファイル不要) */
static HICON MakeIcon(int size, BOOL on)
{
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    DWORD *px;
    HDC dc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    ReleaseDC(NULL, dc);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, NULL);
    if (!color || !mask) return LoadIconW(NULL, IDI_APPLICATION);

    DWORD frame = on ? 0xFF1E3A5F : 0xFF505050;
    DWORD fill  = on ? 0xFF4FA3E0 : 0xFFA0A0A0;
    DWORD fill2 = on ? 0xFF7CD67C : 0xFFB8B8B8;
    /* 単位は size/16 */
    struct { int l, t, r, b; DWORD f; } rects[2] = {
        { 0, 2, 10, 12, fill },
        { 10, 7, 16, 12, fill2 },
    };
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            DWORD c = 0;
            for (int k = 0; k < 2; k++) {
                int l = rects[k].l * size / 16, t = rects[k].t * size / 16;
                int r = rects[k].r * size / 16, b = rects[k].b * size / 16;
                if (x >= l && x < r && y >= t && y < b) {
                    BOOL edge = x == l || x == r - 1 || y == t || y == b - 1;
                    c = edge ? frame : rects[k].f;
                }
            }
            /* 台座 */
            if (y >= 13 * size / 16 && y < 14 * size / 16 + 1 && x >= 3 * size / 16 && x < 7 * size / 16)
                c = frame;
            px[y * size + x] = c;
        }
    }
    ICONINFO ii = { TRUE, 0, 0, mask, color };
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

static void TrayUpdate(DWORD msg)
{
    NOTIFYICONDATAW nid = {0};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_wnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_cfg.enabled ? g_iconOn : g_iconOff;
    wcscpy_s(nid.szTip, ARRAYSIZE(nid.szTip),
             g_cfg.enabled ? APP_NAME L" " APP_VERSION L" - 補正 有効" : APP_NAME L" " APP_VERSION L" - 補正 無効");
    if (!Shell_NotifyIconW(msg, &nid) && msg == NIM_ADD)
        Log_Write(L"tray icon add failed (taskbar not ready?)");
}

static void ShowTrayMenu(void)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (g_cfg.enabled ? MF_CHECKED : 0), ID_ENABLE, L"カーソル補正を有効にする(&E)");
    AppendMenuW(m, MF_STRING | (g_cfg.jumpGaps ? MF_CHECKED : 0), ID_JUMP, L"モニタ間の隙間を飛び越える(&J)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EDITOR, L"レイアウト設定(&L)...");
    AppendMenuW(m, MF_STRING, ID_RELOAD, L"モニタ再検出 / ini 再読み込み(&R)");
    AppendMenuW(m, MF_STRING, ID_OPENINI, L"ini ファイルを開く(&O)");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"終了(&X)");
    SetMenuDefaultItem(m, ID_EDITOR, FALSE);

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, NULL);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

void App_ApplyConfig(const Config *cfg)
{
    g_cfg = *cfg;
    Cfg_Save(&g_cfg);
    TrayUpdate(NIM_MODIFY);
}

static void Reload(void)
{
    Cfg_Load(&g_cfg);
    TrayUpdate(NIM_MODIFY);
    if (Editor_Window()) PostMessageW(Editor_Window(), WM_APP, 0, 0);  /* 編集画面にも再読込を通知 */
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) ShowTrayMenu();
        else if (lp == WM_LBUTTONDBLCLK) Editor_Open(g_inst, g_iconOn);
        return 0;
    case WM_OPEN_EDITOR:
        Editor_Open(g_inst, g_iconOn);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_ENABLE:  g_cfg.enabled = !g_cfg.enabled; Cfg_Save(&g_cfg); TrayUpdate(NIM_MODIFY); break;
        case ID_JUMP:    g_cfg.jumpGaps = !g_cfg.jumpGaps; Cfg_Save(&g_cfg); break;
        case ID_EDITOR:  Editor_Open(g_inst, g_iconOn); break;
        case ID_RELOAD:  Reload(); break;
        case ID_OPENINI: {
            WCHAR ini[MAX_PATH];
            Cfg_IniPath(ini, MAX_PATH);
            ShellExecuteW(NULL, L"open", L"notepad.exe", ini, NULL, SW_SHOWNORMAL);
            break;
        }
        case ID_EXIT:    DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DISPLAYCHANGE:
        Log_Write(L"WM_DISPLAYCHANGE %ux%u", LOWORD(lp), HIWORD(lp));
        /* 接続/解像度変更の直後は構成が落ち着くまで少し待つ */
        SetTimer(hwnd, TIMER_RELOAD, 1500, NULL);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_RELOAD) { KillTimer(hwnd, TIMER_RELOAD); Reload(); }
        else if (wp == TIMER_WATCH) Watch();
        else if (wp == TIMER_SIMLOSS) {
            /* 検証用: ハンドルは残したままフックだけ外し、Windows に黙って外された状態を再現する */
            KillTimer(hwnd, TIMER_SIMLOSS);
            UnhookWindowsHookEx(g_hook);
            Log_Write(L"simulate-hook-loss: hook removed silently");
        }
        return 0;
    case WM_POWERBROADCAST:
        if (wp == PBT_APMSUSPEND) Log_Write(L"power: suspend");
        else if (wp == PBT_APMRESUMEAUTOMATIC) Log_Write(L"power: resume");
        return TRUE;
    case WM_WTSSESSION_CHANGE: {
        static const WCHAR *names[] = { L"?", L"console connect", L"console disconnect", L"remote connect",
                                        L"remote disconnect", L"logon", L"logoff", L"lock", L"unlock" };
        Log_Write(L"session: %s", wp < ARRAYSIZE(names) ? names[wp] : L"other");
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_WATCH);
        WTSUnRegisterSessionNotification(hwnd);
        TrayUpdate(NIM_DELETE);
        PostQuitMessage(0);
        return 0;
    default:
        if (msg == g_msgTaskbarCreated && msg != 0) {
            Log_Write(L"TaskbarCreated -> re-add tray icon");
            TrayUpdate(NIM_ADD);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE prev, PWSTR cmd, int show)
{
    (void)prev; (void)show;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\" APP_NAME L"_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* 2回目の起動は既存インスタンスの設定画面を開くだけ */
        HWND other = FindWindowW(APP_NAME L"_Tray", NULL);
        if (other) PostMessageW(other, WM_OPEN_EDITOR, 0, 0);
        return 0;
    }

    g_inst = hinst;
    Log_Init();
    Log_Write(L"===== start %s %s pid=%lu uptime=%llus", APP_NAME, APP_VERSION,
              GetCurrentProcessId(), GetTickCount64() / 1000);
    g_iconOn  = MakeIcon(GetSystemMetrics(SM_CXSMICON), TRUE);
    g_iconOff = MakeIcon(GetSystemMetrics(SM_CXSMICON), FALSE);
    Cfg_Load(&g_cfg);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = APP_NAME L"_Tray";
    wc.hIcon = g_iconOn;
    RegisterClassW(&wc);
    /* WM_DISPLAYCHANGE と TaskbarCreated を受けるため、メッセージ専用ではなく非表示のトップレベル窓にする */
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, APP_NAME, WS_POPUP,
                            0, 0, 0, 0, NULL, NULL, hinst, NULL);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    TrayUpdate(NIM_ADD);
    WTSRegisterSessionNotification(g_wnd, NOTIFY_FOR_THIS_SESSION);

    InstallHook(L"startup");
    if (!g_hook)
        MessageBoxW(NULL, L"マウスフックを設定できませんでした。", APP_NAME, MB_ICONERROR);
    SetTimer(g_wnd, TIMER_WATCH, WATCH_MS, NULL);
    if (wcsstr(cmd, L"/simulate-hook-loss")) SetTimer(g_wnd, TIMER_SIMLOSS, 2000, NULL);

    if (wcsstr(cmd, L"/config")) Editor_Open(hinst, g_iconOn);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        HWND ed = Editor_Window();
        if (ed && IsDialogMessageW(ed, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook) UnhookWindowsHookEx(g_hook);
    Log_Write(L"exit (moved=%lu blocked=%lu)", g_moved, g_blocked);
    if (mutex) CloseHandle(mutex);
    return 0;
}
