/* cursoring - 物理サイズを考慮してモニタ間のカーソル移動を補正する軽量常駐ツール */
#ifndef CURSORING_H
#define CURSORING_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define APP_NAME     L"cursoring"
#define APP_VERSION  L"v2"
#define MAX_MONITORS 16

typedef struct Monitor {
    WCHAR  id[128];      /* ini のセクション名に使う識別子 (機種コード.インスタンス) */
    WCHAR  name[64];     /* EDID のモデル名 (無ければデバイス名) */
    WCHAR  device[32];   /* \\.\DISPLAY1 など */
    BOOL   primary;
    RECT   px;           /* ピクセル座標 (仮想スクリーン) */
    double wmm, hmm;     /* 物理サイズ (mm) */
    double xmm, ymm;     /* 物理位置 (mm) */
} Monitor;

typedef struct Config {
    BOOL enabled;
    BOOL jumpGaps;       /* 物理的な隙間(ベゼル等)を飛び越えて隣のモニタへ移る */
    BOOL ignoreInjected; /* ソフトウェアが送った入力(タブレットドライバ等)は補正しない */
    int  maxJumpPx;      /* 現在のモニタからこれ以上離れた移動は絶対座標入力とみなし補正しない */
    int  count;
    Monitor mon[MAX_MONITORS];
} Config;

/* monitors.c */
void  Cfg_SidePath(WCHAR *buf, DWORD cch, const WCHAR *ext);  /* exe と同じ場所・同じ名前で拡張子だけ変えたパス */
void  Cfg_IniPath(WCHAR *buf, DWORD cch);
BOOL  Cfg_LayoutChanged(const Config *cfg);  /* 実際のモニタ構成が cfg と食い違っているか */
void  Cfg_Load(Config *cfg);           /* モニタ列挙 + EDID + ini 読み込み */
void  Cfg_Save(const Config *cfg);
void  Cfg_AutoLayout(Config *cfg);      /* ピクセル配置から物理配置を作り直す */
void  Cfg_Normalize(Config *cfg);

/* mapping.c */
typedef enum { MAP_PASS = 0, MAP_MOVE, MAP_BLOCK } MapResult;
int       Map_MonitorAt(const Config *cfg, POINT pt);
MapResult Map_Translate(const Config *cfg, int cur, POINT pt, POINT *out);
BOOL      Map_PixelToMM(const Config *cfg, POINT pt, double *fx, double *fy);

/* log.c */
void Log_Init(void);
void Log_Write(const WCHAR *fmt, ...);

/* editor.c */
void Editor_Open(HINSTANCE hinst, HICON icon);
HWND Editor_Window(void);

/* main.c */
extern Config g_cfg;
void App_ApplyConfig(const Config *cfg);  /* 編集結果を常駐側に反映して保存 */

#endif
