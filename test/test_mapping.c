/* Map_Translate の単体テスト。ピクセル配置は実機 (4K + 1080p 右下) と同じにしてある。 */
#include "../src/lbmlite.h"
#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void Setup(Config *c, double d2x, double d2y)
{
    ZeroMemory(c, sizeof(*c));
    c->enabled = TRUE;
    c->jumpGaps = TRUE;
    c->maxJumpPx = 400;
    c->count = 2;
    Monitor *a = &c->mon[0], *b = &c->mon[1];
    SetRect(&a->px, 0, 0, 3840, 2160);    a->wmm = 600; a->hmm = 340; a->xmm = 0;   a->ymm = 0;
    SetRect(&b->px, 3840, 1916, 5760, 2996); b->wmm = 230; b->hmm = 130; b->xmm = d2x; b->ymm = d2y;
}

static MapResult T(const Config *c, int cur, LONG x, LONG y, POINT *out)
{
    POINT pt = { x, y };
    out->x = out->y = -99999;
    return Map_Translate(c, cur, pt, out);
}

int main(void)
{
    Config c;
    POINT o;
    MapResult r;
    Setup(&c, 600, 200);

    r = T(&c, 0, 1000, 1000, &o);
    CHECK(r == MAP_PASS, "モニタ内の移動は素通し r=%d", r);

    /* D1 右端 y=1500 (物理 236mm) は D2 の物理範囲 200-330mm に入る。ピクセル上は D2 の上端より上 */
    r = T(&c, 0, 3845, 1500, &o);
    CHECK(r == MAP_MOVE, "D1->D2 へ移動 r=%d", r);
    CHECK(o.x >= 3840 && o.x < 3900, "D2 の左端付近に入る x=%ld", o.x);
    CHECK(o.y >= 2210 && o.y <= 2222, "物理的に同じ高さ y=%ld", o.y);

    /* 戻り: D2 左端から左へ出ると D1 の元の高さ付近に戻る */
    POINT back;
    r = T(&c, 1, 3835, o.y, &back);
    CHECK(r == MAP_MOVE, "D2->D1 へ移動 r=%d", r);
    CHECK(back.x <= 3839 && back.x > 3800, "D1 の右端付近 x=%ld", back.x);
    CHECK(labs(back.y - 1500) <= 3, "往復で高さがほぼ保たれる y=%ld", back.y);

    /* D1 右端の上の方には物理的な隣が無いので端で止める */
    r = T(&c, 0, 3850, 100, &o);
    CHECK(r == MAP_BLOCK && o.x == 3839 && o.y == 100, "隣が無い方向は端で停止 r=%d (%ld,%ld)", r, o.x, o.y);

    /* ピクセル上は D1 の下端より下 (y=2900) でも、物理的に D1 の高さ内なら D1 へ移る */
    r = T(&c, 1, 3830, 2900, &o);
    CHECK(r == MAP_MOVE && o.x <= 3839 && o.y > 2000 && o.y < 2160, "D2 下部から D1 へ r=%d (%ld,%ld)", r, o.x, o.y);

    /* D1 の下端は行き場が無い */
    r = T(&c, 0, 1000, 2170, &o);
    CHECK(r == MAP_BLOCK && o.y == 2159, "下端で停止 r=%d y=%ld", r, o.y);

    /* 絶対座標入力のような遠いジャンプは補正しない */
    r = T(&c, 0, 5000, 2500, &o);
    CHECK(r == MAP_PASS, "遠いジャンプは素通し r=%d", r);

    /* 10mm の隙間: JumpGaps=1 なら飛び越え、0 なら停止 */
    Setup(&c, 610, 200);
    r = T(&c, 0, 3845, 1500, &o);
    CHECK(r == MAP_MOVE && o.x == 3840, "隙間を飛び越えて左端へ r=%d x=%ld", r, o.x);
    c.jumpGaps = FALSE;
    r = T(&c, 0, 3845, 1500, &o);
    CHECK(r == MAP_BLOCK && o.x == 3839, "JumpGaps=0 は停止 r=%d x=%ld", r, o.x);

    /* 無効なモニタ番号 (カーソルがどのモニタにも無い) は素通し */
    r = T(&c, -1, 10, 10, &o);
    CHECK(r == MAP_PASS, "cur=-1 は素通し");

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("all mapping tests passed\n");
    return 0;
}
