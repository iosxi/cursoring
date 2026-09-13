/* 動作ログ: exe と同じフォルダの cursoring.log に、起動・モニタ構成・フック状態などの
 * 出来事だけを追記する (マウス移動ごとには書かない) */
#include "cursoring.h"
#include <stdarg.h>
#include <stdio.h>

#define LOG_MAX_BYTES (512 * 1024)

static BOOL  s_enabled;
static WCHAR s_path[MAX_PATH];

void Log_Init(void)
{
    WCHAR ini[MAX_PATH];
    Cfg_SidePath(ini, MAX_PATH, L".ini");
    s_enabled = GetPrivateProfileIntW(L"General", L"Log", 1, ini) != 0;
    if (!s_enabled) return;

    Cfg_SidePath(s_path, MAX_PATH, L".log");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(s_path, GetFileExInfoStandard, &fa) &&
        (((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) > LOG_MAX_BYTES) {
        WCHAR old[MAX_PATH];
        Cfg_SidePath(old, MAX_PATH, L".log.old");
        MoveFileExW(s_path, old, MOVEFILE_REPLACE_EXISTING);
    }
}

void Log_Write(const WCHAR *fmt, ...)
{
    if (!s_enabled) return;

    WCHAR line[1024];
    SYSTEMTIME st;
    GetLocalTime(&st);
    int n = swprintf_s(line, 1024, L"%04u-%02u-%02u %02u:%02u:%02u.%03u  ",
                       st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    int m = _vsnwprintf_s(line + n, 1024 - n - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (m < 0) m = (int)wcslen(line + n);
    wcscpy_s(line + n + m, 1024 - n - m, L"\r\n");

    char utf8[3072];
    int len = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
    if (len <= 1) return;

    HANDLE h = CreateFileW(s_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(h, utf8, (DWORD)(len - 1), &written, NULL);
    CloseHandle(h);
}
