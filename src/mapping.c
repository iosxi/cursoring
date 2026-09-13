/* ピクセル座標 <-> 物理座標(mm) の変換と、モニタ境界を越えるときの移動先計算。
 * Win32 の状態に触れない純粋な計算だけを置き、単体テストできるようにしている。 */
#include "lbmlite.h"
#include <math.h>

int Map_MonitorAt(const Config *cfg, POINT pt)
{
    for (int i = 0; i < cfg->count; i++)
        if (PtInRect(&cfg->mon[i].px, pt)) return i;
    return -1;
}

static double ScaleX(const Monitor *m) { return m->wmm / (double)(m->px.right - m->px.left); }
static double ScaleY(const Monitor *m) { return m->hmm / (double)(m->px.bottom - m->px.top); }

static BOOL InPhys(const Monitor *m, double fx, double fy)
{
    return fx >= m->xmm && fx < m->xmm + m->wmm && fy >= m->ymm && fy < m->ymm + m->hmm;
}

static LONG Clamp(LONG v, LONG lo, LONG hi) { return v < lo ? lo : v > hi ? hi : v; }

BOOL Map_PixelToMM(const Config *cfg, POINT pt, double *fx, double *fy)
{
    int i = Map_MonitorAt(cfg, pt);
    if (i < 0) return FALSE;
    const Monitor *m = &cfg->mon[i];
    *fx = m->xmm + (pt.x - m->px.left + 0.5) * ScaleX(m);
    *fy = m->ymm + (pt.y - m->px.top + 0.5) * ScaleY(m);
    return TRUE;
}

/* 物理的な隙間を挟んだ先にあるモニタを探す。dir: +1/-1、horiz: 横方向か */
static int FindAcrossGap(const Config *cfg, int cur, BOOL horiz, int dir, double fx, double fy,
                         double *ex, double *ey)
{
    const Monitor *c = &cfg->mon[cur];
    int best = -1;
    double bestDist = 1e18;
    for (int i = 0; i < cfg->count; i++) {
        if (i == cur) continue;
        const Monitor *m = &cfg->mon[i];
        double dist;
        if (horiz) {
            if (fy < m->ymm || fy >= m->ymm + m->hmm) continue;
            dist = dir > 0 ? m->xmm - (c->xmm + c->wmm) : c->xmm - (m->xmm + m->wmm);
        } else {
            if (fx < m->xmm || fx >= m->xmm + m->wmm) continue;
            dist = dir > 0 ? m->ymm - (c->ymm + c->hmm) : c->ymm - (m->ymm + m->hmm);
        }
        if (dist < -0.01 || dist >= bestDist) continue;
        bestDist = dist;
        best = i;
    }
    if (best >= 0) {
        const Monitor *m = &cfg->mon[best];
        *ex = fx; *ey = fy;
        if (horiz) *ex = dir > 0 ? m->xmm : m->xmm + m->wmm - 1e-6;
        else       *ey = dir > 0 ? m->ymm : m->ymm + m->hmm - 1e-6;
    }
    return best;
}

MapResult Map_Translate(const Config *cfg, int cur, POINT pt, POINT *out)
{
    if (cur < 0 || cur >= cfg->count) return MAP_PASS;
    const Monitor *c = &cfg->mon[cur];
    if (PtInRect(&c->px, pt)) return MAP_PASS;

    /* ペンタブレット等の絶対座標入力は遠くへ一気に飛ぶので、補正対象外にする */
    LONG over = max(max(c->px.left - pt.x, pt.x - (c->px.right - 1)),
                    max(c->px.top - pt.y, pt.y - (c->px.bottom - 1)));
    if (cfg->maxJumpPx > 0 && over > cfg->maxJumpPx) return MAP_PASS;

    /* 現在のモニタの縮尺のまま、はみ出した位置を物理座標へ延長する */
    double fx = c->xmm + (pt.x - c->px.left + 0.5) * ScaleX(c);
    double fy = c->ymm + (pt.y - c->px.top + 0.5) * ScaleY(c);

    int t = -1;
    for (int i = 0; i < cfg->count; i++) {
        if (i != cur && InPhys(&cfg->mon[i], fx, fy)) { t = i; break; }
    }

    if (t < 0 && cfg->jumpGaps) {
        int dx = pt.x < c->px.left ? -1 : pt.x >= c->px.right ? 1 : 0;
        int dy = pt.y < c->px.top ? -1 : pt.y >= c->px.bottom ? 1 : 0;
        double ex = fx, ey = fy;
        if (dx) t = FindAcrossGap(cfg, cur, TRUE, dx, fx, fy, &ex, &ey);
        if (t < 0 && dy) t = FindAcrossGap(cfg, cur, FALSE, dy, fx, fy, &ex, &ey);
        if (t >= 0) { fx = ex; fy = ey; }
    }

    if (t >= 0) {
        const Monitor *m = &cfg->mon[t];
        LONG nx = m->px.left + (LONG)floor((fx - m->xmm) / ScaleX(m));
        LONG ny = m->px.top  + (LONG)floor((fy - m->ymm) / ScaleY(m));
        out->x = Clamp(nx, m->px.left, m->px.right - 1);
        out->y = Clamp(ny, m->px.top, m->px.bottom - 1);
        return MAP_MOVE;
    }

    /* 物理的に続きが無い方向へは出さず、現在のモニタの端に留める */
    out->x = Clamp(pt.x, c->px.left, c->px.right - 1);
    out->y = Clamp(pt.y, c->px.top, c->px.bottom - 1);
    return MAP_BLOCK;
}
