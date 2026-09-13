/* モニタ列挙、EDID からの物理サイズ取得、ini の読み書き */
#include "cursoring.h"
#include <shellscalingapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

void Cfg_IniPath(WCHAR *buf, DWORD cch)
{
    DWORD n = GetModuleFileNameW(NULL, buf, cch);
    WCHAR *dot = NULL;
    for (DWORD i = 0; i < n; i++) {
        if (buf[i] == L'.') dot = buf + i;
        else if (buf[i] == L'\\') dot = NULL;
    }
    if (dot) *dot = 0;
    wcscat_s(buf, cch, L".ini");
}

typedef struct { const Config *cfg; int seen; BOOL changed; } CheckCtx;

static BOOL CALLBACK CheckProc(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    CheckCtx *ctx = (CheckCtx *)lp;
    (void)dc; (void)rc;
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, (MONITORINFO *)&mi)) return TRUE;
    ctx->seen++;
    for (int i = 0; i < ctx->cfg->count; i++) {
        const Monitor *m = &ctx->cfg->mon[i];
        if (wcscmp(m->device, mi.szDevice) == 0) {
            if (!EqualRect(&m->px, &mi.rcMonitor)) ctx->changed = TRUE;
            return TRUE;
        }
    }
    ctx->changed = TRUE;  /* 知らないモニタがある */
    return TRUE;
}

BOOL Cfg_LayoutChanged(const Config *cfg)
{
    CheckCtx ctx = { cfg, 0, FALSE };
    EnumDisplayMonitors(NULL, NULL, CheckProc, (LPARAM)&ctx);
    return ctx.changed || ctx.seen != cfg->count;
}

/* "\\?\DISPLAY#GSM7707#5&3568f48a&0&UID4353#{...}" から機種コードとインスタンスを取り出す */
static BOOL ParseInterfaceName(const WCHAR *s, WCHAR *model, size_t cm, WCHAR *inst, size_t ci)
{
    const WCHAR *p = s;
    for (; *p; p++)
        if (_wcsnicmp(p, L"DISPLAY#", 8) == 0) break;
    if (!*p) return FALSE;
    p += 8;
    const WCHAR *q = wcschr(p, L'#');
    if (!q || (size_t)(q - p) >= cm) return FALSE;
    wcsncpy_s(model, cm, p, q - p);
    p = q + 1;
    q = wcschr(p, L'#');
    if (!q || (size_t)(q - p) >= ci) return FALSE;
    wcsncpy_s(inst, ci, p, q - p);
    return TRUE;
}

/* レジストリは読むだけ (KEY_READ 相当の RegGetValue) */
static DWORD ReadEdid(const WCHAR *model, const WCHAR *inst, BYTE *edid, DWORD cap)
{
    WCHAR path[512];
    swprintf_s(path, 512, L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\%s\\%s\\Device Parameters", model, inst);
    DWORD len = cap;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, path, L"EDID", RRF_RT_REG_BINARY, NULL, edid, &len) != ERROR_SUCCESS)
        return 0;
    return len;
}

static void ParseEdid(const BYTE *e, DWORD len, double *wmm, double *hmm, WCHAR *name, size_t cn)
{
    *wmm = *hmm = 0;
    if (len < 128) return;
    /* 第1詳細タイミング記述子の画像サイズ (mm 単位で精度が高い) */
    int w = e[66] | (e[68] >> 4) << 8;
    int h = e[67] | (e[68] & 0x0F) << 8;
    if (w > 0 && h > 0) { *wmm = w; *hmm = h; }
    else if (e[21] && e[22]) { *wmm = e[21] * 10.0; *hmm = e[22] * 10.0; }

    for (int off = 54; off <= 108; off += 18) {
        if (e[off] == 0 && e[off + 1] == 0 && e[off + 3] == 0xFC) {
            int k = 0;
            for (int i = 5; i < 18 && k < (int)cn - 1; i++) {
                if (e[off + i] == 0x0A) break;
                name[k++] = (WCHAR)e[off + i];
            }
            while (k > 0 && name[k - 1] == L' ') k--;
            name[k] = 0;
            break;
        }
    }
}

static BOOL CALLBACK EnumProc(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp)
{
    Config *cfg = (Config *)lp;
    (void)dc; (void)rc;
    if (cfg->count >= MAX_MONITORS) return FALSE;

    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hm, (MONITORINFO *)&mi)) return TRUE;

    Monitor *m = &cfg->mon[cfg->count++];
    ZeroMemory(m, sizeof(*m));
    m->px = mi.rcMonitor;
    m->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    wcscpy_s(m->device, 32, mi.szDevice);
    wcscpy_s(m->name, 64, mi.szDevice + (wcsncmp(mi.szDevice, L"\\\\.\\", 4) == 0 ? 4 : 0));
    wcscpy_s(m->id, 128, m->name);

    DISPLAY_DEVICEW dd;
    WCHAR model[64], inst[96];
    BOOL found = FALSE;
    for (DWORD i = 0;; i++) {
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(mi.szDevice, i, &dd, EDD_GET_DEVICE_INTERFACE_NAME)) break;
        if (ParseInterfaceName(dd.DeviceID, model, 64, inst, 96)) {
            found = TRUE;
            if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE) break;
        }
    }

    int pw = m->px.right - m->px.left, ph = m->px.bottom - m->px.top;
    if (found) {
        BYTE edid[512];
        DWORD len = ReadEdid(model, inst, edid, sizeof(edid));
        swprintf_s(m->id, 128, L"%s.%s", model, inst);
        ParseEdid(edid, len, &m->wmm, &m->hmm, m->name, 64);
    }
    if (m->wmm <= 0 || m->hmm <= 0) {
        /* EDID が読めない場合は Windows のスケーリング設定から概算 */
        UINT dx = 96, dy = 96;
        if (FAILED(GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy)) || dx == 0) dx = 96;
        m->wmm = pw * 25.4 / dx;
        m->hmm = ph * 25.4 / dx;
    }
    /* 縦置き回転時は EDID の縦横が逆になる */
    if ((pw > ph) != (m->wmm > m->hmm) && pw != ph) {
        double t = m->wmm; m->wmm = m->hmm; m->hmm = t;
    }
    return TRUE;
}

static int CmpDevice(const void *a, const void *b)
{
    const Monitor *x = a, *y = b;
    size_t lx = wcslen(x->device), ly = wcslen(y->device);
    if (lx != ly) return lx < ly ? -1 : 1;
    return wcscmp(x->device, y->device);
}

static void Section(const Monitor *m, WCHAR *buf, size_t cch)
{
    swprintf_s(buf, cch, L"Monitor.%s", m->id);
}

static BOOL IniDouble(const WCHAR *sec, const WCHAR *key, const WCHAR *ini, double *out)
{
    WCHAR buf[64];
    GetPrivateProfileStringW(sec, key, L"", buf, 64, ini);
    if (!buf[0]) return FALSE;
    WCHAR *end;
    double v = wcstod(buf, &end);
    if (end == buf) return FALSE;
    *out = v;
    return TRUE;
}

static void AutoLayout(Config *cfg, BOOL *placed)
{
    int n = cfg->count, any = 0;
    for (int i = 0; i < n; i++) any |= placed[i];
    if (!any && n > 0) {
        int p = 0;
        for (int i = 0; i < n; i++) if (cfg->mon[i].primary) p = i;
        cfg->mon[p].xmm = cfg->mon[p].ymm = 0;
        placed[p] = TRUE;
    }

    for (;;) {
        BOOL progress = FALSE, remain = FALSE;
        for (int i = 0; i < n; i++) {
            if (placed[i]) continue;
            remain = TRUE;
            Monitor *m = &cfg->mon[i];
            double mh = m->px.bottom - m->px.top, mw = m->px.right - m->px.left;
            for (int j = 0; j < n && !placed[i]; j++) {
                if (!placed[j]) continue;
                const Monitor *a = &cfg->mon[j];
                double ah = a->px.bottom - a->px.top, aw = a->px.right - a->px.left;
                LONG vov = min(m->px.bottom, a->px.bottom) - max(m->px.top, a->px.top);
                LONG hov = min(m->px.right, a->px.right) - max(m->px.left, a->px.left);
                /* 共有する辺に沿ったずれは、内側にある側の縮尺で換算する */
                double ay = m->px.top >= a->px.top
                    ? a->ymm + (m->px.top - a->px.top) * a->hmm / ah
                    : a->ymm - (a->px.top - m->px.top) * m->hmm / mh;
                double ax = m->px.left >= a->px.left
                    ? a->xmm + (m->px.left - a->px.left) * a->wmm / aw
                    : a->xmm - (a->px.left - m->px.left) * m->wmm / mw;
                if (vov > 0 && m->px.left == a->px.right)       { m->xmm = a->xmm + a->wmm; m->ymm = ay; placed[i] = TRUE; }
                else if (vov > 0 && m->px.right == a->px.left)  { m->xmm = a->xmm - m->wmm; m->ymm = ay; placed[i] = TRUE; }
                else if (hov > 0 && m->px.top == a->px.bottom)  { m->ymm = a->ymm + a->hmm; m->xmm = ax; placed[i] = TRUE; }
                else if (hov > 0 && m->px.bottom == a->px.top)  { m->ymm = a->ymm - m->hmm; m->xmm = ax; placed[i] = TRUE; }
            }
            if (placed[i]) progress = TRUE;
        }
        if (!remain) break;
        if (!progress) {
            /* ピクセル上で隣接していないモニタは右端に並べておく */
            double right = 0;
            for (int i = 0; i < n; i++)
                if (placed[i] && cfg->mon[i].xmm + cfg->mon[i].wmm > right) right = cfg->mon[i].xmm + cfg->mon[i].wmm;
            for (int i = 0; i < n; i++)
                if (!placed[i]) { cfg->mon[i].xmm = right; cfg->mon[i].ymm = 0; placed[i] = TRUE; break; }
        }
    }
}

void Cfg_AutoLayout(Config *cfg)
{
    BOOL placed[MAX_MONITORS] = {0};
    AutoLayout(cfg, placed);
    Cfg_Normalize(cfg);
}

void Cfg_Normalize(Config *cfg)
{
    if (cfg->count == 0) return;
    double mx = cfg->mon[0].xmm, my = cfg->mon[0].ymm;
    for (int i = 1; i < cfg->count; i++) {
        if (cfg->mon[i].xmm < mx) mx = cfg->mon[i].xmm;
        if (cfg->mon[i].ymm < my) my = cfg->mon[i].ymm;
    }
    for (int i = 0; i < cfg->count; i++) {
        cfg->mon[i].xmm -= mx;
        cfg->mon[i].ymm -= my;
    }
}

void Cfg_Load(Config *cfg)
{
    WCHAR ini[MAX_PATH];
    Cfg_IniPath(ini, MAX_PATH);

    cfg->count = 0;
    EnumDisplayMonitors(NULL, NULL, EnumProc, (LPARAM)cfg);
    qsort(cfg->mon, cfg->count, sizeof(Monitor), CmpDevice);

    cfg->enabled  = GetPrivateProfileIntW(L"General", L"Enabled", 1, ini) != 0;
    cfg->jumpGaps = GetPrivateProfileIntW(L"General", L"JumpGaps", 1, ini) != 0;
    cfg->ignoreInjected = GetPrivateProfileIntW(L"General", L"IgnoreInjected", 1, ini) != 0;
    cfg->maxJumpPx = (int)GetPrivateProfileIntW(L"General", L"MaxJumpPx", 400, ini);

    BOOL placed[MAX_MONITORS] = {0}, missing = FALSE;
    for (int i = 0; i < cfg->count; i++) {
        Monitor *m = &cfg->mon[i];
        WCHAR sec[160];
        Section(m, sec, 160);
        double v;
        if (IniDouble(sec, L"WidthMM", ini, &v) && v > 1) m->wmm = v;
        if (IniDouble(sec, L"HeightMM", ini, &v) && v > 1) m->hmm = v;
        double x, y;
        if (IniDouble(sec, L"X", ini, &x) && IniDouble(sec, L"Y", ini, &y)) {
            m->xmm = x; m->ymm = y; placed[i] = TRUE;
        } else {
            missing = TRUE;
        }
    }
    AutoLayout(cfg, placed);
    if (missing) {
        Cfg_Normalize(cfg);
        Cfg_Save(cfg);  /* 初回起動・新しいモニタ接続時に ini を作成/追記 */
    }
}

static void WriteDouble(const WCHAR *sec, const WCHAR *key, double v, const WCHAR *ini)
{
    WCHAR buf[64];
    swprintf_s(buf, 64, L"%.2f", v);
    WritePrivateProfileStringW(sec, key, buf, ini);
}

void Cfg_Save(const Config *cfg)
{
    WCHAR ini[MAX_PATH], buf[64];
    Cfg_IniPath(ini, MAX_PATH);
    WritePrivateProfileStringW(L"General", L"Enabled", cfg->enabled ? L"1" : L"0", ini);
    WritePrivateProfileStringW(L"General", L"JumpGaps", cfg->jumpGaps ? L"1" : L"0", ini);
    WritePrivateProfileStringW(L"General", L"IgnoreInjected", cfg->ignoreInjected ? L"1" : L"0", ini);
    swprintf_s(buf, 64, L"%d", cfg->maxJumpPx);
    WritePrivateProfileStringW(L"General", L"MaxJumpPx", buf, ini);
    WritePrivateProfileStringW(L"General", L"Log", NULL, ini);  /* v2 が書いた不要な項目を消す */
    for (int i = 0; i < cfg->count; i++) {
        const Monitor *m = &cfg->mon[i];
        WCHAR sec[160];
        Section(m, sec, 160);
        WritePrivateProfileStringW(sec, L"Name", m->name, ini);
        swprintf_s(buf, 64, L"%ldx%ld", m->px.right - m->px.left, m->px.bottom - m->px.top);
        WritePrivateProfileStringW(sec, L"Resolution", buf, ini);
        WriteDouble(sec, L"WidthMM", m->wmm, ini);
        WriteDouble(sec, L"HeightMM", m->hmm, ini);
        WriteDouble(sec, L"X", m->xmm, ini);
        WriteDouble(sec, L"Y", m->ymm, ini);
    }
    WritePrivateProfileStringW(NULL, NULL, NULL, ini);  /* キャッシュをフラッシュ */
}
