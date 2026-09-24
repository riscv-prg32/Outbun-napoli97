/*
 * OutBun - Napoli '97
 *
 * 1997, around Naples: the quest for a bun sandwich on the rocky shores of the
 * Sorrento and Amalfi Coasts is merely an excuse to put the pedal to the metal
 * in a souped-up classic white Fiat 500.
 *
 * PRG32 portable cartridge. Pseudo-3D "segment" road renderer in integer
 * arithmetic, everything drawn with prg32_gfx_rect_indexed() spans against a
 * cartridge-programmed palette (indices 16..231), so ESP32-C6 hardware and QEMU
 * show identical colours. Assets come from tools/generate_assets.py.
 *
 * Portable-cartridge rules honoured here: no libc, no heap, no initialised
 * pointer tables (the image may be relocated), no jump tables.
 */
#include "prg32.h"
#include "assets.h"
#include "route.h"

/* GCC may lower struct copies / zeroing to these even when freestanding. */
#ifndef OUTBUN_HOST
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}
void *memset(void *dst, int v, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)v;
    return dst;
}
#else
#include <string.h>
#endif

/* ------------------------------------------------------------------------ */
/* Constants                                                                */
/* ------------------------------------------------------------------------ */
#define W 320
#define H 200
#define HORIZON 92
#define SEG_LEN 200
#define ROAD_W 1000          /* half road width, world units                */
#define CAM_H 1000
#define KX 134               /* 160 * cot(50 deg)                           */
#define KY 84                /* 100 * cot(50 deg)                           */
#define PLAYER_DZ 839        /* CAM_H * cot(50 deg): car drawn 1:1 here     */
#define DRAW_SEGS 100
#define TUNNEL_H 1500
#define LEGS 9
#define PICKUPS 12
#define MAX_TRAFFIC 6
#define RACERS 4
#define NET_SIGNATURE "outbun-napoli97:v1"
#define SCORE_GAME "OutBun-napoli97"
#define PEER_TIMEOUT_MS 4000
#define START_SEG 30         /* start gantry on Via Marina, Napoli          */

#define GEAR_LO 0
#define GEAR_HI 1

enum { ST_TITLE, ST_MODE, ST_CAR, ST_LOBBY, ST_INTRO, ST_RACE, ST_PAUSE, ST_PANINO, ST_RESULTS };
enum { PH_COUNTDOWN, PH_RACE, PH_TIMEUP, PH_GOAL };
enum { RK_NONE, RK_LOCAL, RK_AI, RK_PEER };
enum { TR_APE, TR_VESPA, TR_BUS };

/* music: tracks and synth channels (see tools/generate_audio.py) */
#define TRK_TITLE 0
#define TRK_RACE 1
#define TRK_CHECK 2
#define TRK_GOAL 3
#define TRK_OVER 4
#define TRK_RACE2 5          /* tarantella race theme, Sorrento to Vietri    */
#define CH_BELL 5
#define CH_ENGINE 6
#define CH_FX 7

typedef struct {
    int32_t z;            /* world position of the car along the route; for
                             the local player this is the camera, the car
                             itself sits PLAYER_DZ ahead (see car_z)        */
    int16_t x;            /* lateral, Q8: +-256 = road edges                */
    int16_t speed;        /* world units per tick * 16                      */
    uint8_t kind, car, lane, brake;
    int8_t lean;
    uint8_t finished, got, gear;
    uint32_t peer_id, seen_ms;
    int16_t skill;
} racer_t;

typedef struct { int32_t z; int16_t x, speed; uint8_t type, lane; } traffic_t;

/* car handling: top speed (units*16/tick), acceleration, grip (16 = 1.0) */
static const int16_t car_top[4] = {3200, 3120, 3060, 3280};
static const uint8_t car_accel[4] = {18, 20, 17, 16};
static const uint8_t car_grip[4] = {17, 16, 20, 15};
static const char car_names[4][14] = {"FIAT 500 L", "FIAT 126", "CITROEN DYANE", "VW MAGGIOLINO"};
static const char car_tag[4][4] = {"500", "126", "DYA", "VW"};
/* left tail lamp {x, y, w, h} in sprite pixels; the right lamp is mirrored */
static const uint8_t car_lamp[4][4] = {{5, 35, 3, 5}, {5, 15, 8, 3}, {7, 17, 5, 5}, {8, 15, 4, 6}};
static const uint8_t car_dot[4] = {C_WHITE, C_RED, C_YELLOW, C_CYAN};

/* ------------------------------------------------------------------------ */
/* State                                                                    */
/* ------------------------------------------------------------------------ */
static uint8_t state, phase, net_mode, attract;
static uint32_t last_ms, tick_acc, frame, state_ticks, phase_ticks, rng;
static uint32_t input_now, input_prev, input_edge;

static racer_t rc[RACERS];
static traffic_t traffic[MAX_TRAFFIC];
static int16_t sec_start[SECTION_COUNT + 1];
static int32_t sec_y0[SECTION_COUNT + 1];
static int32_t route_segs;
static int16_t leg_start[LEGS + 1];

static int32_t time_left;      /* ticks */
static uint32_t score, best_score;
static uint8_t cur_leg, got[LEGS], taken[LEGS][2];
static uint8_t chosen_car, menu_sel, local_ready;
static int16_t bump_ticks, shake, checkpoint_ticks, jingle_ticks, spark_ticks;
static int32_t bg_scroll;      /* Q8 pixels */
static uint8_t engine_note;
static uint8_t final_place, panino_layers;
static char best_player[24];

/* theme blending (palette slots 192..231) */
static uint8_t theme_from, theme_to, scenery_idx;
static int16_t theme_t;

/* background, rebuilt per leg */
static uint8_t far_h[512], near_h[512], near_dot[512];

/* projected segments, index n = distance from the camera segment */
static int16_t ps_x1[DRAW_SEGS], ps_y1[DRAW_SEGS], ps_w1[DRAW_SEGS];
static int16_t ps_x2[DRAW_SEGS], ps_y2[DRAW_SEGS], ps_w2[DRAW_SEGS];
static int16_t ps_clip[DRAW_SEGS], ps_top[DRAW_SEGS], ps_left[DRAW_SEGS], ps_right[DRAW_SEGS];
static int32_t ps_z1[DRAW_SEGS];
static uint8_t ps_ok[DRAW_SEGS];

/* network peers */
static prg32_player_state_t peers[3];
static uint8_t peer_count, net_ok;

static int32_t car_z(const racer_t *r) { return r->kind == RK_LOCAL ? r->z + PLAYER_DZ : r->z; }

/* drawing clip rectangle */
static int clip_x0, clip_y0, clip_x1 = W, clip_y1 = H;

/* ------------------------------------------------------------------------ */
/* Utilities                                                                */
/* ------------------------------------------------------------------------ */
static uint32_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return rng;
}
static uint32_t hash32(uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
    return v;
}
static int iabs(int v) { return v < 0 ? -v : v; }
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void set_clip(int x0, int y0, int x1, int y1) {
    clip_x0 = imax(x0, 0); clip_y0 = imax(y0, 0);
    clip_x1 = imin(x1, W); clip_y1 = imin(y1, H);
}
static void reset_clip(void) { clip_x0 = 0; clip_y0 = 0; clip_x1 = W; clip_y1 = H; }

static void fill(int x, int y, int w, int h, uint8_t c) {
    if (x < clip_x0) { w -= clip_x0 - x; x = clip_x0; }
    if (y < clip_y0) { h -= clip_y0 - y; y = clip_y0; }
    if (x + w > clip_x1) w = clip_x1 - x;
    if (y + h > clip_y1) h = clip_y1 - y;
    if (w > 0 && h > 0) prg32_gfx_rect_indexed(x, y, w, h, c);
}

/* ---- text: 5x7 column font, drawn as vertical runs ---- */
static int str_len(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int text_w(const char *s, int sc) { return str_len(s) * 6 * sc - sc; }

static void draw_char(int x, int y, char ch, int sc, uint8_t c, const uint8_t *ramp) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    if (ch < FONT_FIRST || ch > FONT_LAST) ch = '?';
    const uint8_t *g = &ob_font[(ch - FONT_FIRST) * 5];
    if (ramp) {                                  /* row-major, colour per row */
        for (int r = 0; r < 7; r++) {
            int col = 0;
            while (col < 5) {
                if ((g[col] >> r) & 1) {
                    int s = col;
                    while (col < 5 && ((g[col] >> r) & 1)) col++;
                    fill(x + s * sc, y + r * sc, (col - s) * sc, sc, ramp[r]);
                } else col++;
            }
        }
        return;
    }
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        int r = 0;
        while (r < 7) {
            if ((bits >> r) & 1) {
                int s = r;
                while (r < 7 && ((bits >> r) & 1)) r++;
                fill(x + col * sc, y + s * sc, sc, (r - s) * sc, c);
            } else r++;
        }
    }
}
static void text(int x, int y, const char *s, int sc, uint8_t c, uint8_t sh) {
    if (sh && sc > 1) {
        int xx = x + (sc > 1 ? sc / 2 + 1 : 1);
        for (const char *p = s; *p; p++, xx += 6 * sc) draw_char(xx, y + (sc > 1 ? sc / 2 + 1 : 1), *p, sc, sh, 0);
    }
    for (; *s; s++, x += 6 * sc) draw_char(x, y, *s, sc, c, 0);
}
static void text_c(int cx, int y, const char *s, int sc, uint8_t c, uint8_t sh) {
    text(cx - text_w(s, sc) / 2, y, s, sc, c, sh);
}
static void text_r(int rx, int y, const char *s, int sc, uint8_t c, uint8_t sh) {
    text(rx - text_w(s, sc), y, s, sc, c, sh);
}
static char *utoa_(uint32_t v, char *buf, int min_digits) {
    char tmp[12]; int n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n < min_digits) tmp[n++] = '0';
    int i = 0;
    while (n) buf[i++] = tmp[--n];
    buf[i] = 0;
    return buf + i;
}
static char *scat(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }

static void panel(int x, int y, int w, int h, uint8_t bg, uint8_t border) {
    fill(x, y, w, h, bg);
    fill(x, y, w, 1, border); fill(x, y + h - 1, w, 1, border);
    fill(x, y, 1, h, border); fill(x + w - 1, y, 1, h, border);
}

/* ------------------------------------------------------------------------ */
/* Palette                                                                  */
/* ------------------------------------------------------------------------ */
static uint16_t blend565(uint16_t a, uint16_t b, int t) {
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
    int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    int r = ar + ((br - ar) * t >> 8), g = ag + ((bg - ag) * t >> 8), bl = ab + ((bb - ab) * t >> 8);
    return (uint16_t)((r << 11) | (g << 5) | bl);
}
static void theme_write(void) {
    const uint16_t *a = &ob_themes[theme_from * OB_THEME_COUNT];
    const uint16_t *b = &ob_themes[theme_to * OB_THEME_COUNT];
    for (int i = 0; i < OB_THEME_COUNT; i++)
        prg32_palette_set((uint8_t)(OB_THEME_BASE + i), blend565(a[i], b[i], theme_t));
}
static void theme_set(int idx, int instant) {
    if (instant) { theme_from = theme_to = (uint8_t)idx; theme_t = 256; theme_write(); return; }
    theme_from = theme_to; theme_to = (uint8_t)idx; theme_t = 0;
}
static void palette_init(void) {
    for (int i = 0; i < OB_STATIC_COUNT; i++)
        prg32_palette_set((uint8_t)(OB_STATIC_BASE + i), ob_static_palette[i]);
}

/* ------------------------------------------------------------------------ */
/* Route geometry                                                           */
/* ------------------------------------------------------------------------ */
static void route_init(void) {
    int32_t s = 0, y = 0;
    for (int i = 0; i < SECTION_COUNT; i++) {
        sec_start[i] = (int16_t)s;
        sec_y0[i] = y;
        s += ob_sections[i].len * LEN_UNIT;
        y += ob_sections[i].hill * HILL_UNIT;
    }
    sec_start[SECTION_COUNT] = (int16_t)s;
    sec_y0[SECTION_COUNT] = y;
    route_segs = s;
    for (int l = 0; l <= LEGS; l++) leg_start[l] = sec_start[leg_first_section[l]];
}
static int section_of(int32_t seg) {
    int lo = 0, hi = SECTION_COUNT - 1;
    if (seg <= 0) return 0;
    if (seg >= route_segs) return SECTION_COUNT - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) >> 1;
        if (sec_start[mid] <= seg) lo = mid; else hi = mid - 1;
    }
    return lo;
}
static int leg_of(int32_t seg) {
    int l = 0;
    while (l < LEGS - 1 && seg >= leg_start[l + 1]) l++;
    return l;
}
/* curvature ramps in and out over 12 segments; returns curve * 12 */
static int curve12_at(int32_t seg, int sec) {
    if (seg < 0 || seg >= route_segs) return 0;
    int a = seg - sec_start[sec], b = sec_start[sec + 1] - 1 - seg;
    int r = imin(12, imin(a, b) + 1);
    return ob_sections[sec].curve * r;
}
static int32_t height_at(int32_t seg, int sec) {
    if (seg <= 0) return 0;
    if (seg >= route_segs) return sec_y0[SECTION_COUNT];
    int len = sec_start[sec + 1] - sec_start[sec];
    int t = ((seg - sec_start[sec]) << 8) / len;            /* Q8 */
    int e = (t * t * (768 - 2 * t)) >> 16;                  /* smoothstep Q8 */
    return sec_y0[sec] + ((int32_t)ob_sections[sec].hill * HILL_UNIT * e >> 8);
}
static uint8_t flags_at(int32_t seg, int sec) {
    if (seg < 0 || seg >= route_segs) return F_TOWN;
    return ob_sections[sec].flags;
}
static int32_t height_at_z(int32_t z) {
    int32_t seg = z / SEG_LEN;
    int sec = section_of(seg);
    int32_t a = height_at(seg, sec);
    int sec2 = section_of(seg + 1);
    int32_t b = height_at(seg + 1, sec2);
    return a + (b - a) * (z % SEG_LEN) / SEG_LEN;
}

/* ------------------------------------------------------------------------ */
/* Pickups: 12 per leg at fixed segments; index 6 is a golden triple        */
/* ------------------------------------------------------------------------ */
static int32_t pickup_seg(int leg, int i) {
    int len = leg_start[leg + 1] - leg_start[leg];
    return leg_start[leg] + (len * (i + 1)) / (PICKUPS + 2) + 20;
}
static int pickup_x(int leg, int i) {
    uint32_t h = hash32((uint32_t)(leg * 31 + i * 7 + 5));
    return (int)(h % 3) * 128 - 128;
}
static int pickup_taken(int leg, int i) { return (taken[leg][i >> 3] >> (i & 7)) & 1; }
static void pickup_take(int leg, int i) { taken[leg][i >> 3] |= (uint8_t)(1u << (i & 7)); }

/* ------------------------------------------------------------------------ */
/* Sprite rendering from nibble-RLE rows                                    */
/* ------------------------------------------------------------------------ */
/* emit one destination row band of a sprite row group */
static void spr_band(const uint8_t *q, int sw, int dw, int x0, int y, int hh, int mirror, const uint8_t *lut) {
    int pc = 0, pa = 0, pe = 0;                  /* pending span: colour, [a, e) */
    if (dw >= sw) {                              /* enlarged: one span per run */
        int32_t xs = ((int32_t)dw << 16) / sw;
        int sx = 0;
        while (sx < sw) {
            uint8_t b = *q++;
            int c = b >> 4, n = (b & 15) + 1;
            int a = sx, e = sx + n;
            if (mirror) { a = sw - e; e = sw - sx; }
            int X0 = (int)((a * xs) >> 16), X1 = (int)((e * xs) >> 16);
            sx += n;
            if (X1 <= X0) continue;
            if (c == pc && (X0 == pe || X1 == pa)) { pa = imin(pa, X0); pe = imax(pe, X1); continue; }
            if (pc) fill(x0 + pa, y, pe - pa, hh, lut[pc - 1]);
            pc = c; pa = X0; pe = X1;
        }
        if (pc) fill(x0 + pa, y, pe - pa, hh, lut[pc - 1]);
        return;
    }
    /* reduced: sample the runs at destination resolution */
    int32_t step = ((int32_t)sw << 16) / dw, srcx = 0;
    int run_end = 0, c = 0, k0 = 0;
    for (int k = 0; k <= dw; k++, srcx += step) {
        int cc = 0;
        if (k < dw) {
            int sxp = (int)(srcx >> 16);
            while (sxp >= run_end) { uint8_t b = *q++; c = b >> 4; run_end += (b & 15) + 1; }
            cc = c;
        }
        if (k == dw || cc != pc) {
            if (pc && k > k0) {
                int a = mirror ? dw - k : k0, e = mirror ? dw - k0 : k;
                fill(x0 + a, y, e - a, hh, lut[pc - 1]);
            }
            pc = cc; k0 = k;
        }
    }
}

/*
 * Draw sprite `id` bottom-centred at (cx, by), `dw` pixels wide. The data is
 * a list of row groups (repeat count + runs); each group becomes one band of
 * rectangles whatever the scale. `lean` shears the top of the sprite
 * sideways (cars in bends), which needs one band per destination row.
 */
static void spr(int id, int lut_index, int cx, int by, int dw, int mirror, int lean) {
    const ob_sprite_t *sp = &ob_sprites[id];
    int sw = sp->w, sh = sp->h;
    if (dw < 3 || dw > 1400) return;
    int dh = sh * dw / sw;
    if (dh < 1) dh = 1;
    int x0 = cx - dw / 2, y0 = by - dh;
    if (y0 >= clip_y1 || by <= clip_y0 || x0 - 4 >= clip_x1 || x0 + dw + 4 <= clip_x0) return;
    const uint8_t *lut = &ob_luts[(lut_index < 0 ? sp->lut : lut_index) * 15];
    const uint8_t *p = &ob_rle[sp->off];
    int first = 0;
    while (first < sh) {
        int rows = *p++;
        const uint8_t *runs = p;
        int sx = 0;
        while (sx < sw) sx += (*p++ & 15) + 1;   /* p -> next group */
        int dya = (int)(((int32_t)first * dh + sh - 1) / sh);
        int dyb = (int)(((int32_t)(first + rows) * dh + sh - 1) / sh);
        first += rows;
        if (dyb <= dya || y0 + dya >= clip_y1) continue;
        if (y0 + dyb <= clip_y0) continue;
        if (!lean) { spr_band(runs, sw, dw, x0, y0 + dya, dyb - dya, mirror, lut); continue; }
        for (int dy = dya; dy < dyb;) {          /* bands of equal shear */
            int shift = lean * (dh - dy) / dh, dy2 = dy + 1;
            while (dy2 < dyb && lean * (dh - dy2) / dh == shift) dy2++;
            spr_band(runs, sw, dw, x0 + shift, y0 + dy, dy2 - dy, mirror, lut);
            dy = dy2;
        }
    }
}

/* brake lamps glow on top of a car sprite drawn with spr() */
static void car_lamps(int car, int cx, int by, int dw, int lean) {
    int ch = ob_sprites[SPR_CAR_500 + car].h;   /* the 500 L is taller than the rivals */
    int dh = ch * dw / 56, x0 = cx - dw / 2, y0 = by - dh;
    const uint8_t *l = car_lamp[car];
    int lx = l[0] * dw / 56, ly = l[1] * dh / ch, lw = imax(1, l[2] * dw / 56), lh = imax(1, l[3] * dh / ch);
    int sh = lean * (dh - ly) / dh;
    fill(x0 + lx + sh, y0 + ly, lw, lh, C_BRAKE);
    fill(x0 + dw - lx - lw + sh, y0 + ly, lw, lh, C_BRAKE);
    if (dw >= 40) {
        fill(x0 + lx + sh, y0 + ly, lw / 2 + 1, lh / 2 + 1, C_BRAKE_HI);
        fill(x0 + dw - lx - lw + sh, y0 + ly, lw / 2 + 1, lh / 2 + 1, C_BRAKE_HI);
    }
}

/* ------------------------------------------------------------------------ */
/* Background panorama                                                      */
/* ------------------------------------------------------------------------ */
static int isin(int a) {  /* a in 0..1023 -> -127..127 */
    static const int8_t q[17] = {0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126, 127};
    a &= 1023;
    int quad = a >> 8, t = a & 255;
    int i = (quad & 1) ? (256 - t) >> 4 : t >> 4;
    int v = q[i];
    return quad >= 2 ? -v : v;
}
static void scenery_build(int idx) {
    const ob_scenery_t *sc = &leg_scenery[idx];
    scenery_idx = (uint8_t)idx;
    for (int i = 0; i < 512; i++) {           /* far layer, 2 px columns */
        int x = i * 2, h = 0;
        if (x < 600) {
            int env = x < 60 ? x : x > 480 ? (600 - x) : 60;
            int v = 90 + isin(x * 2 + sc->seed * 16) / 2 + isin(x * 5 + sc->seed * 40) / 4 + isin(x * 11 + sc->seed * 9) / 8;
            h = v * sc->mtn_amp * env / (60 * 128);
        }
        for (int k = 0; k < 2; k++) {
            const uint8_t *lm = sc->lm[k];
            int sc2 = lm[2], n = lm_len[lm[0]];
            int rel = x - lm[1] * 4;                   /* px from the landmark start */
            int span = 4 * sc2;                        /* px between height samples */
            if (rel >= 0 && rel < (n - 1) * span) {
                const uint8_t *hs = lm_heights[lm[0]];
                int j = rel / span, f = rel % span;
                int lh = (hs[j] * (span - f) + hs[j + 1] * f) * sc2 / span;
                if (lh > h) h = lh;
            }
        }
        far_h[i] = (uint8_t)clampi(h, 0, 90);
    }
    for (int i = 0; i < 512; i++) {           /* near hills, 2 px columns */
        int x = i * 2, h = 0;
        if (x < 560) {
            int env = x < 50 ? x : x > 430 ? (560 - x) * 50 / 130 : 50;
            int v = 70 + isin(x * 3 + sc->seed * 21) / 3 + isin(x * 8 + sc->seed * 7) / 6 + isin(x * 17) / 12;
            h = v * sc->hill_amp * env / (50 * 100);
        }
        near_h[i] = (uint8_t)clampi(h, 0, 60);
        uint32_t hh = hash32((uint32_t)(i * 13 + sc->seed * 977));
        near_dot[i] = 0;
        if (h > 5 && (int)(hh % 100) < sc->town_density)
            near_dot[i] = (uint8_t)(0x80 | ((hh >> 8) % 6) << 4 | ((hh >> 12) % (unsigned)(h > 12 ? 12 : h - 3)));
    }
}

static void draw_sky(void) {
    int band = (HORIZON + 7) / 8;
    for (int i = 0; i < 8; i++) {
        int y = i * band;
        fill(0, y, W, band, (uint8_t)(T_SKY0 + i));
        if (i < 7) {  /* scanline dither between bands */
            fill(0, y + band - 3, W, 1, (uint8_t)(T_SKY0 + i + 1));
            fill(0, y + band - 1, W, 1, (uint8_t)(T_SKY0 + i + 1));
        }
    }
}
static void draw_sun(void) {
    const ob_scenery_t *sc = &leg_scenery[scenery_idx];
    int cx = ((sc->sun_x4 * 4 - (bg_scroll >> 10)) & 1023);
    if (cx > 700) cx -= 1024;
    int cy = sc->sun_y, r = 11;
    if (cx < -r || cx > W + r) return;
    for (int dy = -r; dy <= r; dy += 2) {
        int dx = r;
        while (dx * dx + dy * dy > r * r) dx--;
        if (cy + dy > HORIZON) break;
        if (scenery_idx >= 8 && dy > 2 && ((dy >> 1) & 1)) continue; /* sunset stripes */
        fill(cx - dx, cy + dy, dx * 2 + 1, 2, dy < -r / 2 ? T_SUN_HI : T_SUN);
    }
}
static void draw_clouds(void) {
    static const int16_t cx0[4] = {80, 330, 610, 860};
    static const uint8_t cy0[4] = {22, 36, 14, 30};
    for (int k = 0; k < 4; k++) {
        int x = ((cx0[k] - (bg_scroll >> 10) - (int)(frame >> 4)) & 1023);
        if (x > 800) x -= 1024;
        int y = cy0[k];
        if (x < -60 || x > W + 20) continue;
        fill(x + 8, y, 22, 3, T_CLOUD);
        fill(x + 2, y + 3, 40, 4, T_CLOUD);
        fill(x + 20, y - 3, 14, 3, T_CLOUD);
        fill(x - 4, y + 7, 56, 3, T_CLOUD);
        fill(x - 2, y + 10, 50, 2, T_CLOUD_SH);
    }
}
static void draw_background(void) {
    draw_sky();
    draw_sun();
    draw_clouds();
    int far_off = (bg_scroll >> 9), near_off = (bg_scroll >> 8);
    /* far layer (half parallax), 2 px columns merged when level */
    for (int x = 0; x < W;) {
        int h = far_h[((x + far_off + 360) >> 1) & 511], x2 = x + 2;
        while (x2 < W && far_h[((x2 + far_off + 360) >> 1) & 511] == h) x2 += 2;
        if (h) {
            fill(x, HORIZON - h, x2 - x, h, T_MTN_FAR);
            fill(x, HORIZON - h, x2 - x, 1, T_MTN_FAR_HI);
        }
        x = x2;
    }
    fill(0, HORIZON - 3, W, 3, T_HAZE);
    /* near hills with pastel villages */
    for (int x = 0; x < W; x += 2) {
        int i = ((x + near_off + 360) >> 1) & 511;
        int h = near_h[i];
        if (!h) continue;
        fill(x, HORIZON - h, 2, h, T_MTN_NEAR);
        fill(x, HORIZON - h, 2, 1, T_MTN_NEAR_HI);
        uint8_t d = near_dot[i];
        if (d) fill(x, HORIZON - h + 2 + (d & 15), 2, 2, (uint8_t)(C_PASTEL0 + ((d >> 4) & 7)));
    }
    /* coastline strip just below the horizon follows the near-hill mask */
    for (int x = 0; x < W;) {
        int land = near_h[((x + near_off + 360) >> 1) & 511] != 0, x2 = x + 4;
        while (x2 < W && (near_h[((x2 + near_off + 360) >> 1) & 511] != 0) == land) x2 += 4;
        fill(x, HORIZON, x2 - x, 4, land ? T_LAND_FAR : T_SEA_FAR);
        x = x2;
    }
}

/* ------------------------------------------------------------------------ */
/* Road                                                                     */
/* ------------------------------------------------------------------------ */
static int32_t cam_z, cam_x, cam_y;
static int cam_base;

static void ground_colours(int32_t seg, uint8_t fl, uint8_t *lg, uint8_t *rg) {
    int light = ((seg / 3) & 1) == 0;
    if (fl & F_TUNNEL) { *lg = *rg = light ? T_WALL_L : T_WALL_D; return; }
    if (fl & F_BRIDGE) { *lg = *rg = light ? T_VALLEY_L : T_VALLEY_D; return; }
    *lg = (fl & F_TOWN) ? (light ? T_TOWN_L : T_TOWN_D) : (light ? T_LAND_L : T_LAND_D);
    *rg = (fl & F_LANDR) ? *lg : (light ? T_SEA_L : T_SEA_D);
}

/* one scanline of road over an already painted ground band */
static void road_row(int y, int cx, int w, int32_t seg, uint8_t fl) {
    int light = ((seg / 3) & 1) == 0;
    int r = w / 7 + 1;
    int le = cx - w, re = cx + w;
    uint8_t rumble = (fl & F_TUNNEL) ? (light ? C_STONE : C_STONE_D) : (light ? T_RUMBLE_L : T_RUMBLE_D);
    fill(le - r, y, r, 1, rumble);
    fill(le, y, 2 * w, 1, light ? T_ROAD_L : T_ROAD_D);
    if (light) fill(cx - (w >> 5) - 1, y, (w >> 4) + 1, 1, T_LINE);
    fill(re, y, r, 1, rumble);
    int sea = !(fl & (F_LANDR | F_TUNNEL | F_BRIDGE));
    if (sea || (fl & F_BRIDGE)) {          /* stone parapet above the sea / gorge */
        fill(re + r, y, r, 1, light ? C_STONE : C_STONE_D);
        if (fl & F_BRIDGE) fill(le - 2 * r, y, r, 1, light ? C_STONE : C_STONE_D);
        if (sea && ((y * 7 + (int)(frame >> 2) + (int)seg) % 11) == 0)
            fill(re + 2 * r + (int)(hash32((uint32_t)(y + seg)) % 160), y, 3 + (w >> 5), 1, T_SEA_GLINT);
    }
}

static void road_segment(int32_t seg, int sx1, int sy1, int w1, int sx2, int sy2, int w2, int maxy, uint8_t fl) {
    int dy = sy1 - sy2;
    if (dy <= 0) return;
    int top = imax(sy2, clip_y0), bot = imin(imin(sy1, maxy), clip_y1);
    if (top >= bot) return;
    /* ground band split at the road centre, then the road spans on top */
    uint8_t lg, rg;
    ground_colours(seg, fl, &lg, &rg);
    int32_t ix = (int32_t)(sx1 - sx2) * 256 / dy, iw = (int32_t)(w1 - w2) * 256 / dy;
    int32_t fx = (int32_t)sx2 * 256 + ix * (top - sy2), fw = (int32_t)w2 * 256 + iw * (top - sy2);
    int mid = (int)((fx + ix * (bot - top) / 2) / 256);
    fill(0, top, mid, bot - top, lg);
    fill(mid, top, W - mid, bot - top, rg);
    for (int y = top; y < bot; y++) {
        road_row(y, (int)(fx / 256), (int)(fw / 256), seg, fl);
        fx += ix; fw += iw;
    }
}

/* project a point: world x relative to camera, world height, depth */
static int proj_x(int32_t rx, int32_t z) { int32_t v = 160 + rx * KX / z; return (int)clampi(v, -30000, 30000); }
static int proj_y(int32_t ry, int32_t z) { int32_t v = HORIZON + ry * KY / z; return (int)clampi(v, -30000, 30000); }
static int proj_w(int32_t z) { int32_t v = (int32_t)ROAD_W * KX / z; return (int)imin(v, 30000); }

static void render_road(void) {
    int32_t base = cam_z / SEG_LEN;
    int32_t pct = cam_z % SEG_LEN;
    cam_base = base;
    int sec = section_of(base);
    int32_t x = 0, dx = -(curve12_at(base, sec) * 16 / 12) * pct / SEG_LEN;   /* Q4 world units */
    int maxy = H;
    int in_tunnel = 0, tunnel_open = 0;
    int ctop = 0, cleft = 0, cright = W;
    reset_clip();
    if (flags_at(base + 3, section_of(base + 3)) & F_TUNNEL) {
        in_tunnel = 1;
        fill(0, 0, W, HORIZON + 8, T_WALL_D);
    }
    for (int n = 0; n < DRAW_SEGS; n++) {
        int32_t s = base + n;
        ps_ok[n] = 0;
        if (s >= route_segs + 40) break;
        while (sec < SECTION_COUNT - 1 && s >= sec_start[sec + 1]) sec++;
        uint8_t fl = flags_at(s, sec);
        int32_t z1 = s * SEG_LEN - cam_z, z2 = z1 + SEG_LEN;
        int32_t y1 = height_at(s, sec), y2 = height_at(s + 1, section_of(s + 1));
        int c12 = curve12_at(s, sec);
        int32_t rx1 = (x >> 4) - cam_x, rx2 = ((x + dx) >> 4) - cam_x;
        x += dx;
        dx += c12 * 16 / 12;
        if (z1 <= 20) continue;
        int sx1 = proj_x(rx1, z1), sy1 = proj_y(cam_y - y1, z1), w1 = proj_w(z1);
        int sx2 = proj_x(rx2, z2), sy2 = proj_y(cam_y - y2, z2), w2 = proj_w(z2);
        ps_x1[n] = (int16_t)sx1; ps_y1[n] = (int16_t)sy1; ps_w1[n] = (int16_t)w1;
        ps_x2[n] = (int16_t)sx2; ps_y2[n] = (int16_t)sy2; ps_w2[n] = (int16_t)w2;
        ps_z1[n] = z1;
        ps_clip[n] = (int16_t)maxy; ps_top[n] = (int16_t)ctop; ps_left[n] = (int16_t)cleft; ps_right[n] = (int16_t)cright;
        ps_ok[n] = 1;
        int tunnel = (fl & F_TUNNEL) != 0;
        int cy1 = proj_y(cam_y - y1 - TUNNEL_H, z1), cy2 = proj_y(cam_y - y2 - TUNNEL_H, z2);
        if (tunnel && !in_tunnel && !tunnel_open) {
            /* portal: rock face around the tunnel mouth, then clip to it */
            int ftop = proj_y(cam_y - y1 - TUNNEL_H * 3, z1);
            int ol = sx1 - w1 - w1 / 5, orr = sx1 + w1 + w1 / 5;
            set_clip(0, ctop, W, maxy);
            fill(0, ftop, W, cy1 - ftop, C_ROCK);
            fill(0, ftop, W, 2, C_ROCK_D);
            fill(0, cy1, ol, sy1 - cy1, C_ROCK);
            fill(orr, cy1, W - orr, sy1 - cy1, C_ROCK);
            fill(ol, cy1, orr - ol, imin(sy1, maxy) - cy1, T_WALL_D);
            fill(ol - 3, cy1 - 3, orr - ol + 6, 3, C_STONE);
            tunnel_open = 1;
            ctop = imax(ctop, cy1); cleft = imax(cleft, ol); cright = imin(cright, orr);
        } else if (!tunnel && (in_tunnel || tunnel_open) && tunnel_open != 2) {
            /* light at the end of the tunnel */
            int ol = sx1 - w1 - w1 / 5, orr = sx1 + w1 + w1 / 5;
            set_clip(cleft, ctop, cright, maxy);
            fill(ol, cy1, orr - ol, sy1 - cy1 + 1, T_SKY6);
            fill(ol, sy1 - (sy1 - cy1) / 3, orr - ol, (sy1 - cy1) / 3 + 1, T_HAZE);
            tunnel_open = 2;
            ctop = imax(ctop, cy1); cleft = imax(cleft, ol); cright = imin(cright, orr);
        }
        set_clip(cleft, ctop, cright, H);
        if (tunnel && (in_tunnel || tunnel_open == 1)) {
            /* ceiling band with sodium lamps */
            int a = imax(cy1, ctop), b = imin(cy2, maxy);
            if (b > a) {
                fill(0, a, W, b - a, T_CEIL);
                if ((s & 3) == 0) fill(sx1 - w1 / 6, a, w1 / 3 + 1, imax(1, (b - a) / 2), C_LAMP);
            }
        }
        if (sy2 >= sy1 || sy2 >= maxy) continue;
        road_segment(s, sx1, sy1, w1, sx2, sy2, w2, maxy, fl);
        maxy = sy2;
        if (s == leg_start[leg_of(s) + 1] || s == leg_start[0] + START_SEG) {
            /* checkpoint / start gantry drawn over the road at its line */
            ps_ok[n] = 3;
        }
    }
    reset_clip();
}

/* ------------------------------------------------------------------------ */
/* Roadside objects (deterministic per segment)                              */
/* ------------------------------------------------------------------------ */
typedef struct { int8_t id, lut, mirror; int16_t off; } deco_t;   /* off: Q8 road half-widths */

static int deco_at(int32_t s, int side, deco_t *d) {
    int sec = section_of(s);
    const ob_section_t *se = &ob_sections[sec];
    uint32_t h = hash32((uint32_t)(s * 2 + side + 99));
    d->lut = -1; d->mirror = (int8_t)(side < 0); d->off = 0;
    if (s < 0 || s >= route_segs) return 0;
    if (se->flags & F_TUNNEL) return 0;
    int c = se->curve;
    /* chevrons on the outside of real bends */
    if (iabs(c) >= 4 && (s & 3) == 0 && ((c > 0 && side < 0) || (c < 0 && side > 0))) {
        d->id = SPR_CHEVRON; d->off = (int16_t)(side * 330); d->mirror = (int8_t)(c < 0); return 1;
    }
    if (se->flags & F_BRIDGE) {
        if ((s % 8) == 0) { d->id = SPR_LAMP; d->off = (int16_t)(side * 300); return 1; }
        return 0;
    }
    int right_sea = side > 0 && !(se->flags & F_LANDR);
    if (right_sea && se->deco == D_BEACH && (s % 4) == 1) { d->id = SPR_BEACH; d->off = (int16_t)(520 + (h % 90)); return 1; }
    if (right_sea) {
        if ((s % 7) == 0) { d->id = SPR_WALL; d->off = 360; return 1; }
        if ((s % 37) == 5) { d->id = SPR_GOZZO; d->off = (int16_t)(900 + (h % 900)); return 1; }
        if (!(se->flags & F_TOWN) && (h % 23) == 0) { d->id = se->deco == D_PALM ? SPR_PALM : SPR_PINE; d->off = 470; return 1; }
        if ((se->flags & F_TOWN) && (s % 10) == 0) { d->id = SPR_LAMP; d->off = 320; return 1; }
        return 0;
    }
    if (se->flags & F_TOWN) {
        if ((s % 10) == 0) { d->id = SPR_LAMP; d->off = (int16_t)(side * 320); return 1; }
        if (se->deco == D_OLEANDER && (s % 7) == 4) { d->id = SPR_OLEANDER; d->off = (int16_t)(side * 360); return 1; }
        if ((s % 7) == 2 && ((h >> 3) & 1) == (side > 0)) {
            if ((h % 9) == 0 && side < 0) { d->id = SPR_DOME; d->off = -560; return 1; }
            d->id = SPR_HOUSE; d->lut = (int8_t)(h % 4 ? LUT_HOUSE_ALT + (int)(h % 3) : -1);
            d->off = (int16_t)(side * (470 + (int)(h % 140))); return 1;
        }
        if (se->deco == D_PALM && (s % 7) == 5) { d->id = SPR_PALM; d->off = (int16_t)(side * 400); return 1; }
        return 0;
    }
    /* open country */
    if (side < 0 && (s % 97) == 11) { d->id = SPR_EDICOLA; d->off = -330; return 1; }   /* roadside shrine */
    if ((se->flags & F_CLIFFL) && side < 0 && (s % 4) == 1) {
        d->id = SPR_ROCK; d->off = (int16_t)(-(430 + (int)(h % 120))); d->mirror = (int8_t)(h & 1); return 1;
    }
    if ((s % 6) == 3 || (h % 19) == 0) {
        switch (se->deco) {   /* compiled as compare chain: no jump tables */
        case D_PINE: d->id = SPR_PINE; break;
        case D_PALM: d->id = SPR_PALM; break;
        case D_LEMON: d->id = SPR_LEMON; break;
        case D_AGAVE: d->id = (h & 2) ? SPR_AGAVE : SPR_PINE; break;
        case D_TOWER: d->id = (s % 60) == 3 ? SPR_TOWER : SPR_AGAVE; break;
        case D_CYPRESS: d->id = SPR_CYPRESS; break;
        case D_OLEANDER: d->id = (h & 8) ? SPR_OLEANDER : SPR_CYPRESS; break;
        case D_BEACH: d->id = SPR_PALM; break;
        default: d->id = (h & 4) ? SPR_AGAVE : SPR_CYPRESS; break;
        }
        d->off = (int16_t)(side * (400 + (int)(h % 200)));
        if (d->id == SPR_TOWER) d->off = (int16_t)(side * 600);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Sprite pass: far to near                                                 */
/* ------------------------------------------------------------------------ */
static void seg_clip(int n) { set_clip(ps_left[n], ps_top[n], ps_right[n], ps_clip[n]); }

static void place(int n, int32_t frac, int16_t off, int *sx, int *sy, int32_t *z) {
    int x1 = ps_x1[n] + (int)((int32_t)off * ps_w1[n] >> 8);
    int x2 = ps_x2[n] + (int)((int32_t)off * ps_w2[n] >> 8);
    *sx = x1 + (int)((x2 - x1) * frac / SEG_LEN);
    *sy = ps_y1[n] + (int)((ps_y2[n] - ps_y1[n]) * frac / SEG_LEN);
    *z = ps_z1[n] + frac;
}
static int scaled_w(int id, int32_t z) {
    return (int)((int32_t)ob_sprites[id].w * ob_sprites[id].mult * PLAYER_DZ / 16 / imax(1, z));
}

static void draw_gantry(int n, int final_line) {
    int sx, sy; int32_t z;
    place(n, 0, 0, &sx, &sy, &z);
    int w = ps_w1[n] + ps_w1[n] / 4;
    int ph = (int)((int32_t)2200 * KY / imax(z, 1));
    int bh = imax(2, ph / 4), pw = imax(1, w / 16);
    fill(sx - w - pw, sy - ph, pw, ph, C_DKGREY);
    fill(sx + w, sy - ph, pw, ph, C_DKGREY);
    fill(sx - w - pw, sy - ph - bh, 2 * w + 2 * pw, bh, final_line ? C_RED : C_BLUE);
    fill(sx - w - pw, sy - ph - bh, 2 * w + 2 * pw, imax(1, bh / 6), C_WHITE);
    int leg = leg_of(cam_base + n);
    const char *name = town_names[final_line ? leg + 1 : 0];
    int sc = bh >= 22 ? 2 : 1;
    if (bh >= 9 && text_w(name, sc) < 2 * w) text_c(sx, sy - ph - bh + (bh - 7 * sc) / 2, name, sc, C_WHITE, 0);
    /* chequered line on the road */
    int cw = imax(1, ps_w1[n] / 6), chh = imax(1, (ps_y1[n] - ps_y2[n]));
    for (int k = -6; k < 6; k++) fill(sx + k * cw, sy - chh, cw, chh, (k & 1) ? C_WHITE : C_BLACK);
}

static void draw_car_at(int car, int sx, int sy, int32_t z, int brake, int lean) {
    int dw = (int)((int32_t)56 * PLAYER_DZ / imax(1, z));
    spr(SPR_CAR_500 + car, -1, sx, sy + 2, dw, 0, lean * dw / 56);
    if (brake && dw > 8) car_lamps(car, sx, sy + 2, dw, lean * dw / 56);
}

static void render_sprites(void) {
    for (int n = DRAW_SEGS - 1; n >= 1; n--) {
        if (!ps_ok[n]) continue;
        int32_t s = cam_base + n;
        seg_clip(n);
        deco_t d;
        for (int side = -1; side <= 1; side += 2) {
            if (deco_at(s, side, &d)) {
                int sx, sy; int32_t z;
                place(n, 0, d.off, &sx, &sy, &z);
                int dw = scaled_w(d.id, z);
                /* beyond ~300 px a 4x-enlarged house is mostly blur: let it pass */
                if ((n < 70 || dw >= 8) && dw <= 300) spr(d.id, d.lut, sx, sy, dw, d.mirror, 0);
            }
        }
        for (int i = 0; i < LANDMARK_COUNT; i++) {
            const ob_landmark_t *lm = &ob_landmarks[i];
            int32_t at = lm->before_end < 0 ? leg_start[lm->leg] - lm->before_end : leg_start[lm->leg + 1] - lm->before_end;
            if (at != s) continue;
            static const uint8_t lm_sprite[6] = {SPR_CRANE, SPR_TOWER, SPR_DOME, SPR_DUOMO, SPR_DOME, SPR_STALL};
            int sx, sy; int32_t z;
            place(n, 0, lm->off, &sx, &sy, &z);
            int id = lm_sprite[lm->kind];
            spr(id, lm->kind == LMS_DOME_VIETRI ? LUT_DOME_VIETRI : -1, sx, sy, scaled_w(id, z), 0, 0);
        }
        if (ps_ok[n] == 3) draw_gantry(n, s != leg_start[0] + START_SEG);
        /* pickups */
        int leg = leg_of(s);
        for (int i = 0; i < PICKUPS; i++) {
            if (pickup_seg(leg, i) != s || pickup_taken(leg, i)) continue;
            int sx, sy; int32_t z;
            place(n, 100, (int16_t)pickup_x(leg, i), &sx, &sy, &z);
            int bob = isin((int)(frame * 24 + i * 100)) * 6 / 127;
            int dw = scaled_w(SPR_ING_0 + leg, z);
            int lift = dw / 2 + (dw * (bob + 6)) / 48;
            fill(sx - dw / 3, sy - 1, (dw * 2) / 3, imax(1, dw / 8), C_SHADOW);
            if (i == 6) fill(sx - dw / 2 - 1, sy - lift - dw - 1, dw + 2, dw + 2, (frame & 8) ? C_GOLD : C_YELLOW);
            spr(SPR_ING_0 + leg, -1, sx, sy - lift, dw, 0, 0);
            if (((frame >> 2) + i) % 6 == 0 && dw > 6) fill(sx + dw / 3, sy - lift - dw + 1, 2, 2, C_WHITE);
        }
        /* traffic */
        for (int k = 0; k < MAX_TRAFFIC; k++) {
            traffic_t *t = &traffic[k];
            if (t->z / SEG_LEN != s) continue;
            int sx, sy; int32_t z;
            place(n, t->z % SEG_LEN, t->x, &sx, &sy, &z);
            int id = t->type == TR_APE ? SPR_APE : t->type == TR_VESPA ? SPR_VESPA : SPR_BUS;
            int wob = t->type == TR_VESPA ? isin((int)(frame * 8 + k * 200)) / 64 : 0;
            spr(id, -1, sx, sy + 2, scaled_w(id, z), 0, wob);
        }
        /* rivals (AI and remote players) */
        for (int k = 1; k < RACERS; k++) {
            racer_t *r = &rc[k];
            if (r->kind == RK_NONE || r->z / SEG_LEN != s) continue;
            int sx, sy; int32_t z;
            place(n, r->z % SEG_LEN, r->x, &sx, &sy, &z);
            draw_car_at(r->car, sx, sy, z, r->brake, r->lean);
            if (r->kind == RK_PEER && z < 6000) {
                int dw = (int)((int32_t)56 * PLAYER_DZ / imax(1, z));
                fill(sx - 2, sy - dw * ob_sprites[SPR_CAR_500 + r->car].h / 56 - 8, 4, 4, car_dot[r->car]);
            }
        }
    }
    reset_clip();
}

/* ------------------------------------------------------------------------ */
/* Player car, effects                                                      */
/* ------------------------------------------------------------------------ */
static void draw_player(void) {
    racer_t *p = &rc[0];
    int sway = p->lean * 2;
    int bounce = 0;
    int offroad = iabs(p->x) > 270;
    if (p->speed > 400 && (offroad || bump_ticks)) bounce = (frame & 2) ? 1 : 0;
    if (shake) bounce += (frame & 1) ? 2 : -1;
    int cx = 160 + sway, by = 197 - bounce;
    spr(SPR_CAR_500 + p->car, -1, cx, by, 56, 0, p->lean * 2);
    if (p->brake) car_lamps(p->car, cx, by, 56, p->lean * 2);
    /* dust off-road, sparks on walls, tyre smoke in hard bends */
    if ((offroad && p->speed > 600) || spark_ticks) {
        for (int k = 0; k < 4; k++) {
            uint32_t h = hash32(frame * 4 + k);
            int dx = (int)(h % 60) - 30, dy = (int)((h >> 8) % 10);
            fill(cx + dx, by - 6 - dy, 3 + (int)(h >> 20) % 4, 2, spark_ticks ? ((k & 1) ? C_YELLOW : C_ORANGE) : C_DUST);
        }
    }
    if (iabs(p->lean) >= 3 && p->speed > 2200 && (frame & 2)) {
        fill(cx - 26 + (frame & 4), by - 3, 5, 3, C_GREY);
        fill(cx + 20 - (frame & 4), by - 3, 5, 3, C_GREY);
    }
}

/* ------------------------------------------------------------------------ */
/* Map of the Gulf of Naples and the Amalfi coast                           */
/* ------------------------------------------------------------------------ */
static void map_poly(const uint8_t *pts, int n, int ox, int oy, int sh, uint8_t col) {
    int ymin = 255, ymax = 0;
    for (int i = 0; i < n; i++) { ymin = imin(ymin, pts[i * 2 + 1]); ymax = imax(ymax, pts[i * 2 + 1]); }
    int step = 1 << sh;
    for (int y = ymin; y <= ymax; y += step) {
        int xs[12], k = 0;
        int yc = y * 2 + 1;               /* sample at half-pixel centres */
        for (int i = 0; i < n && k < 12; i++) {
            int j = (i + 1) % n;
            int ya = pts[i * 2 + 1] * 2, yb = pts[j * 2 + 1] * 2;
            if ((ya <= yc && yb > yc) || (yb <= yc && ya > yc)) {
                int xa = pts[i * 2], xb = pts[j * 2];
                xs[k++] = xa + (xb - xa) * (yc - ya) / (yb - ya);
            }
        }
        for (int a = 1; a < k; a++) { int v = xs[a], b = a - 1; while (b >= 0 && xs[b] > v) { xs[b + 1] = xs[b]; b--; } xs[b + 1] = v; }
        for (int a = 0; a + 1 < k; a += 2)
            fill(ox + (xs[a] >> sh), oy + (y >> sh), ((xs[a + 1] - xs[a]) >> sh) + 1, 1, col);
    }
}
static void map_line(int x0, int y0, int x1, int y1, int t, uint8_t c) {
    int dx = iabs(x1 - x0), dy = iabs(y1 - y0), n = imax(dx, dy);
    if (!n) n = 1;
    for (int i = 0; i <= n; i++)
        fill(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, t, t, c);
}
/* draws the map with the legs done / current / to come and the player dot */
static void draw_map(int ox, int oy, int sh, int hl_leg, int32_t player_seg) {
    int mw = MAP_W >> sh, mh = MAP_H >> sh;
    fill(ox, oy, mw, mh, C_MAPSEA);
    for (int y = 3; y < mh; y += 6 >> sh) fill(ox, oy + y, mw, 1, C_BLUE);
    set_clip(ox, oy, ox + mw, oy + mh);
    map_poly(map_coast, MAP_COAST_N, ox, oy, sh, C_MAPLAND);
    map_poly(map_capri, MAP_CAPRI_N, ox, oy, sh, C_MAPLAND);
    for (int i = 0; i + 1 < MAP_ROUTE_N; i++) {
        int leg = 0;
        while (leg < LEGS - 1 && i >= map_town_index[leg + 1]) leg++;
        uint8_t c = leg < hl_leg ? C_MAPROAD : leg == hl_leg ? C_YELLOW : C_WHITE;
        map_line(ox + (map_route[i * 2] >> sh), oy + (map_route[i * 2 + 1] >> sh),
                 ox + (map_route[i * 2 + 2] >> sh), oy + (map_route[i * 2 + 3] >> sh), leg == hl_leg && !sh ? 2 : 1, c);
    }
    for (int t = 0; t < 10; t++) {
        int i = map_town_index[t];
        fill(ox + (map_route[i * 2] >> sh) - 1, oy + (map_route[i * 2 + 1] >> sh) - 1, 3, 3, C_BLACK);
        fill(ox + (map_route[i * 2] >> sh), oy + (map_route[i * 2 + 1] >> sh), 1, 1, C_WHITE);
    }
    if (player_seg >= 0) {
        int leg = leg_of(player_seg);
        int a = map_town_index[leg], b = map_town_index[leg + 1];
        int len = leg_start[leg + 1] - leg_start[leg];
        int f = (int)((player_seg - leg_start[leg]) * (b - a) * 256 / len);
        int i = a + (f >> 8), fr = f & 255;
        if (i >= b) { i = b - 1; fr = 255; }
        int x = map_route[i * 2] + ((map_route[i * 2 + 2] - map_route[i * 2]) * fr >> 8);
        int y = map_route[i * 2 + 1] + ((map_route[i * 2 + 3] - map_route[i * 2 + 1]) * fr >> 8);
        if (frame & 8) fill(ox + (x >> sh) - 2, oy + (y >> sh) - 2, 5, 5, C_RED);
        fill(ox + (x >> sh) - 1, oy + (y >> sh) - 1, 3, 3, C_WHITE);
    }
    reset_clip();
    fill(ox - 1, oy - 1, mw + 2, 1, C_WHITE); fill(ox - 1, oy + mh, mw + 2, 1, C_WHITE);
    fill(ox - 1, oy, 1, mh, C_WHITE); fill(ox + mw, oy, 1, mh, C_WHITE);
}

/* ------------------------------------------------------------------------ */
/* Audio                                                                    */
/* ------------------------------------------------------------------------ */
static void music(int track) { prg32_audio_play_track((uint16_t)track); }
static void sfx_bell(int note) { prg32_audio_note(CH_BELL, CH_BELL, (uint8_t)note, 200, 120); }
static void sfx_bump(void) { prg32_audio_note(CH_FX, CH_FX, 40, 230, 140); }
static void sfx_horn(void) { prg32_audio_note(CH_FX, CH_FX, 69, 200, 220); }
static void engine_sound(int on) {
    if (!on) {
        if (engine_note) { prg32_audio_note_off(CH_ENGINE); engine_note = 0; }
        return;
    }
    racer_t *p = &rc[0];
    int top = car_top[p->car];
    int n = 28 + p->speed * (p->gear == GEAR_LO ? 30 : 20) / top + (p->gear == GEAR_HI ? 6 : 0);
    if (n != engine_note && (frame % 5) == 0) {
        engine_note = (uint8_t)n;
        prg32_audio_note_on(CH_ENGINE, CH_ENGINE, (uint8_t)n, 150);
    }
}

/* ------------------------------------------------------------------------ */
/* Network                                                                  */
/* ------------------------------------------------------------------------ */
#define NF_READY 1
#define NF_RACING 2
#define NF_FINISHED 4
#define NF_TIMEUP 8

static void net_join(void) {
    prg32_multiplayer_init();
    net_ok = prg32_multiplayer_available() && prg32_multiplayer_join(NET_SIGNATURE, PRG32_MP_FLAG_ENABLE) >= 0;
    peer_count = 0;
}
static void net_leave(void) {
    if (net_ok) prg32_multiplayer_leave();
    net_ok = 0; net_mode = 0; peer_count = 0;
}
static void net_publish(void) {
    racer_t *p = &rc[0];
    uint16_t fl = 0;
    if (local_ready) fl |= NF_READY;
    if (state == ST_RACE || state == ST_PAUSE) fl |= NF_RACING;
    if (p->finished == 1) fl |= NF_FINISHED;
    if (p->finished == 2) fl |= NF_TIMEUP;
    int32_t cz = car_z(p), seg = cz / SEG_LEN;
    uint32_t packed = (uint32_t)(cz % SEG_LEN * 255 / SEG_LEN) | ((uint32_t)(p->speed >> 4) & 255u) << 8 |
                      ((uint32_t)p->got & 127u) << 16 | ((uint32_t)cur_leg & 15u) << 24;
    prg32_multiplayer_set_local_state(p->x, (int16_t)seg, (uint16_t)(p->car | ((p->lean + 4) & 7) << 2 | (p->brake ? 32 : 0)), fl);
    prg32_multiplayer_set_input(packed);
    prg32_multiplayer_tick();
}
static void net_poll(void) {
    int n = prg32_multiplayer_get_peer_count();
    n = n < 0 ? 0 : n > 3 ? 3 : n;
    int k = 0;
    uint32_t now = prg32_ticks_ms();
    for (int i = 0; i < n; i++) {
        prg32_player_state_t st;
        if (prg32_multiplayer_get_peer(i, &st) < 0) continue;
        if (st.last_seen_ms && now - st.last_seen_ms > PEER_TIMEOUT_MS) continue;
        peers[k++] = st;
    }
    /* stable order by player id */
    for (int a = 1; a < k; a++) {
        prg32_player_state_t v = peers[a]; int b = a - 1;
        while (b >= 0 && peers[b].player_id > v.player_id) { peers[b + 1] = peers[b]; b--; }
        peers[b + 1] = v;
    }
    peer_count = (uint8_t)k;
}
/* bind remote players to racer slots 1..3; AI keeps the rest */
static void net_bind_racers(void) {
    for (int k = 1; k < RACERS; k++) if (rc[k].kind == RK_PEER) rc[k].kind = RK_AI;
    for (int i = 0; i < peer_count; i++) {
        prg32_player_state_t *st = &peers[i];
        int slot = -1;
        for (int k = 1; k < RACERS; k++) if (rc[k].peer_id == st->player_id && rc[k].peer_id) slot = k;
        if (slot < 0) for (int k = 1; k < RACERS; k++) if (rc[k].kind != RK_PEER && !rc[k].peer_id) { slot = k; break; }
        if (slot < 0) for (int k = 1; k < RACERS; k++) if (rc[k].kind != RK_PEER) { slot = k; break; }
        if (slot < 0) continue;
        racer_t *r = &rc[slot];
        r->kind = RK_PEER; r->peer_id = st->player_id; r->seen_ms = prg32_ticks_ms();
        r->car = (uint8_t)(st->sprite & 3);
        r->lean = (int8_t)(((st->sprite >> 2) & 7) - 4);
        r->brake = (st->sprite & 32) != 0;
        r->x = st->x;
        int32_t z = (int32_t)(uint16_t)st->y * SEG_LEN + (int32_t)(st->input & 255) * SEG_LEN / 256;
        r->speed = (int16_t)(((st->input >> 8) & 255) << 4);
        /* snap if far, otherwise glide to hide network jitter */
        if (iabs(z - r->z) > SEG_LEN * 30) r->z = z; else r->z += (z - r->z) / 4;
        r->got = (uint8_t)((st->input >> 16) & 127);
        r->finished = (st->flags & NF_FINISHED) ? 1 : (st->flags & NF_TIMEUP) ? 2 : 0;
    }
}

/* ------------------------------------------------------------------------ */
/* Race setup and simulation                                                */
/* ------------------------------------------------------------------------ */
static int leg_bonus_seconds(int leg) {
    int len = leg_start[leg + 1] - leg_start[leg];
    return len / 40 + 5;
}
static void traffic_spawn(traffic_t *t, int32_t around) {
    int legn = leg_of(around / SEG_LEN);
    uint32_t h = rnd();
    t->z = around + (int32_t)(90 + h % 140) * SEG_LEN;
    t->lane = (uint8_t)((h >> 8) & 1);
    t->x = t->lane ? 128 : -128;
    int kind = (int)((h >> 12) % 10);
    t->type = kind < 4 ? TR_APE : kind < 7 ? TR_VESPA : TR_BUS;
    if (legn < 4 && t->type == TR_BUS && (h & 0x10000)) t->type = TR_APE;
    t->speed = (int16_t)(t->type == TR_BUS ? 1300 : t->type == TR_APE ? 1000 + (h >> 20) % 300 : 1500 + (h >> 20) % 400);
}
static void race_setup(int32_t start_seg, int for_attract) {
    memset(rc, 0, sizeof rc);
    memset(got, 0, sizeof got);
    memset(taken, 0, sizeof taken);
    racer_t *p = &rc[0];
    p->kind = RK_LOCAL; p->car = chosen_car; p->gear = GEAR_LO;
    p->z = start_seg * SEG_LEN - PLAYER_DZ; p->x = 110;
    /* AI take the cars nobody picked */
    int k = 1;
    for (int c = 0; c < 4 && k < RACERS; c++) {
        if (c == chosen_car) continue;
        racer_t *r = &rc[k];
        r->kind = RK_AI; r->car = (uint8_t)c;

        /* 2x2 grid: two rivals on the front row, one beside the player */
        r->z = car_z(p) + (k == 3 ? 0 : 3 * SEG_LEN);
        r->x = (int16_t)(k == 2 ? 110 : -110);
        r->lane = (uint8_t)(r->x > 0);
        r->skill = (int16_t)(236 - k * 6);
        k++;
    }
    for (int i = 0; i < MAX_TRAFFIC; i++) {
        traffic_spawn(&traffic[i], car_z(p) + (int32_t)i * 40 * SEG_LEN);
        if (!for_attract && traffic[i].z < p->z + 40 * SEG_LEN) traffic[i].z += 60 * SEG_LEN;
    }
    cur_leg = (uint8_t)leg_of(start_seg);
    if (cur_leg >= LEGS) cur_leg = LEGS - 1;
    time_left = (int32_t)(leg_bonus_seconds(0) + 6) * 60;
    score = 0;
    bg_scroll = 0;
    bump_ticks = shake = checkpoint_ticks = spark_ticks = 0;
    scenery_build(for_attract ? 9 : cur_leg);
    theme_set(for_attract ? 9 : cur_leg, 1);
}

/* keep the car on the road: attract autopilot / AI steering target */
static int ai_target_x(racer_t *r, int32_t z) {
    int lane_x = r->lane ? 120 : -120;
    int32_t seg = z / SEG_LEN;
    for (int i = 0; i < MAX_TRAFFIC; i++) {
        int32_t d = traffic[i].z - z;
        if (d > 0 && d < 10 * SEG_LEN && iabs(traffic[i].x - lane_x) < 150) {
            r->lane ^= 1;
            lane_x = r->lane ? 120 : -120;
            break;
        }
    }
    if (r == &rc[0]) {   /* autopilot also picks up ingredients */
        int leg = leg_of(seg);
        for (int i = 0; i < PICKUPS; i++) {
            int32_t d = pickup_seg(leg, i) - seg;
            if (d > 0 && d < 16 && !pickup_taken(leg, i)) return pickup_x(leg, i);
        }
    }
    return lane_x;
}

static void collide_with_traffic(racer_t *r, int is_player) {
    int32_t cz = car_z(r);
    for (int i = 0; i < MAX_TRAFFIC; i++) {
        traffic_t *t = &traffic[i];
        int32_t d = t->z - cz;
        /* half widths in Q8 road units: car 45, bus 78, Ape 32, Vespa 15 */
        int reach = 45 + (t->type == TR_BUS ? 78 : t->type == TR_VESPA ? 15 : 32);
        if (d > -120 && d < 260 && iabs(t->x - r->x) < reach && r->speed > t->speed) {
            r->speed = (int16_t)(t->speed * 3 / 4);
            r->z -= cz - (t->z - 260);
            cz = car_z(r);
            r->x += (int16_t)(r->x < t->x ? -40 : 40);
            if (is_player) { bump_ticks = 20; shake = 10; sfx_bump(); if (t->type != TR_VESPA) sfx_horn(); }
        }
    }
}

static void update_player(int autopilot) {
    racer_t *p = &rc[0];
    int top = car_top[p->car];
    uint32_t in = autopilot ? 0 : input_now;
    int32_t seg = (p->z + PLAYER_DZ) / SEG_LEN;
    int sec = section_of(seg);
    int c12 = curve12_at(seg, sec);
    int accel = 0, brake = 0, steer = 0;
    if (autopilot) {
        int tx = ai_target_x(p, p->z + PLAYER_DZ);
        steer = tx > p->x + 20 ? 1 : tx < p->x - 20 ? -1 : 0;
        steer += c12 > 36 ? 1 : c12 < -36 ? -1 : 0;
        steer = clampi(steer, -1, 1);
        accel = 1;
        brake = iabs(c12) >= 66 && p->speed > top * 3 / 4;
        p->gear = p->speed > top / 2 ? GEAR_HI : GEAR_LO;
    } else if (phase == PH_RACE) {
        accel = (in & PRG32_BTN_A) != 0;
        brake = (in & PRG32_BTN_B) != 0;
        steer = (in & PRG32_BTN_RIGHT) ? 1 : (in & PRG32_BTN_LEFT) ? -1 : 0;
        if (input_edge & PRG32_BTN_UP) p->gear = GEAR_HI;
        if (input_edge & PRG32_BTN_DOWN) p->gear = GEAR_LO;
    }
    /* longitudinal: two-speed gearbox like the arcade original */
    int lim = p->gear == GEAR_LO ? top * 58 / 100 : top;
    int acc = car_accel[p->car];
    if (p->gear == GEAR_LO) acc = acc * 3 / 2;
    else if (p->speed < top * 45 / 100) acc = acc / 3;
    if (accel) {
        if (p->speed < lim) p->speed = (int16_t)imin(lim, p->speed + acc);
        else p->speed = (int16_t)imax(lim, p->speed - 14);
    } else p->speed = (int16_t)imax(0, p->speed - 7);
    if (brake) p->speed = (int16_t)imax(0, p->speed - 60);
    p->brake = (uint8_t)brake;
    int offroad = iabs(p->x) > 256;
    if (offroad && p->speed > top * 45 / 100) p->speed = (int16_t)(p->speed - 30);
    /* lateral */
    int sf = p->speed * 256 / top;                          /* Q8 speed fraction */
    if (p->speed > 80) p->x += (int16_t)(steer * (2 + 7 * sf / 256));
    /* centrifugal drift: curve (c12/12) * speed * 1.3, softened by grip */
    int push = (int)((int32_t)c12 * sf * 13 * 16 / ((int32_t)120 * 256 * car_grip[p->car]));
    p->x -= (int16_t)push;
    int8_t lt = (int8_t)clampi(steer * 3 - push / 2, -4, 4);
    p->lean = (int8_t)(p->lean + (lt > p->lean) - (lt < p->lean));
    /* walls: sea parapet on the right, rock or houses on the left */
    uint8_t fl = flags_at(seg, sec);
    int rlim = (fl & (F_LANDR | F_TOWN)) ? 380 : 330;
    int llim = (fl & F_CLIFFL) ? -340 : -380;
    if (fl & (F_TUNNEL | F_BRIDGE)) { rlim = 300; llim = -300; }
    if (p->x > rlim || p->x < llim) {
        p->x = (int16_t)(p->x > 0 ? rlim - 12 : llim + 12);
        if (p->speed > 800) {
            p->speed = (int16_t)(p->speed * 2 / 3);
            if (!autopilot) { shake = 8; spark_ticks = 10; sfx_bump(); }
        }
    }
    p->x = (int16_t)clampi(p->x, -400, 400);
    p->z += p->speed >> 4;
    collide_with_traffic(p, !autopilot);
    /* rivals: rubbing slows the faster car */
    for (int k = 1; k < RACERS; k++) {
        racer_t *r = &rc[k];
        if (r->kind != RK_AI) continue;
        int32_t d = r->z - (p->z + PLAYER_DZ);
        if (d > -150 && d < 220 && iabs(r->x - p->x) < 90) {
            if (d > 0 && p->speed > r->speed) { p->speed = (int16_t)(r->speed * 9 / 10); if (!autopilot) { shake = 6; sfx_bump(); } }
            int push2 = r->x < p->x ? 20 : -20;
            p->x += (int16_t)push2; r->x -= (int16_t)push2;
        }
    }
    bg_scroll += (int32_t)c12 * sf * 2 / 12;
    if (!autopilot) {
        /* pickups */
        int leg = leg_of(seg);
        for (int i = 0; i < PICKUPS; i++) {
            if (pickup_taken(leg, i)) continue;
            int32_t d = (int32_t)pickup_seg(leg, i) * SEG_LEN + 100 - (p->z + PLAYER_DZ);
            if (d > -140 && d < 140 && iabs(pickup_x(leg, i) - p->x) < 80) {
                pickup_take(leg, i);
                int v = i == 6 ? 3 : 1;
                got[leg] = (uint8_t)imin(20, got[leg] + v);
                p->got = (uint8_t)imin(127, p->got + v);
                score += i == 6 ? 15000 : 5000;
                sfx_bell(i == 6 ? 96 : 88);
            }
        }
        score += (uint32_t)(p->speed >> 7);
    } else {
        int leg = leg_of(seg);
        for (int i = 0; i < PICKUPS; i++) {
            int32_t d = (int32_t)pickup_seg(leg, i) * SEG_LEN - (p->z + PLAYER_DZ);
            if (d > -200 && d < 200) pickup_take(leg, i);
        }
    }
}

static void update_ai(racer_t *r) {
    racer_t *p = &rc[0];
    int top = car_top[r->car];
    int32_t seg = r->z / SEG_LEN;
    int sec = section_of(seg);
    int c12 = curve12_at(seg, sec);
    int target = top * r->skill / 256;
    if (iabs(c12) >= 60) target = target * 86 / 100;
    int32_t gap = r->z - (p->z + PLAYER_DZ);
    if (gap < -40 * SEG_LEN) target = target * 110 / 100;
    else if (gap > 50 * SEG_LEN) target = target * 88 / 100;
    if (r->finished || (state == ST_RACE && phase == PH_COUNTDOWN)) target = r->finished ? top / 3 : 0;
    int acc = car_accel[r->car];
    r->brake = 0;
    if (r->speed < target) r->speed = (int16_t)imin(target, r->speed + acc);
    else { r->speed = (int16_t)imax(target, r->speed - 40); r->brake = r->speed > target + 100; }
    int tx = ai_target_x(r, r->z);
    int lat = 1 + r->speed / 800;                /* no crabbing on the grid */
    int dx = r->speed ? clampi(tx - r->x, -lat, lat) : 0;
    r->x += (int16_t)dx;
    r->lean = (int8_t)clampi(dx - c12 / 30, -3, 3);
    collide_with_traffic(r, 0);
    r->z += r->speed >> 4;
    /* AI can snap up ingredients too */
    int leg = leg_of(seg);
    for (int i = 0; i < PICKUPS; i++) {
        int32_t d = (int32_t)pickup_seg(leg, i) * SEG_LEN + 100 - r->z;
        if (d > -120 && d < 120 && iabs(pickup_x(leg, i) - r->x) < 60 && !pickup_taken(leg, i)) {
            pickup_take(leg, i); r->got++;
        }
    }
    if (r->z / SEG_LEN >= route_segs) r->finished = 1;
}

static void update_traffic(void) {
    int32_t pz = car_z(&rc[0]);
    for (int i = 0; i < MAX_TRAFFIC; i++) {
        traffic_t *t = &traffic[i];
        t->z += t->speed >> 4;
        if (t->z < pz - 15 * SEG_LEN || t->z > pz + 400 * SEG_LEN || t->z / SEG_LEN >= route_segs - 10)
            traffic_spawn(t, pz);
    }
}

static int race_position(void) {
    int pos = 1;
    for (int k = 1; k < RACERS; k++)
        if (rc[k].kind != RK_NONE && rc[k].z > rc[0].z + PLAYER_DZ) pos++;
    return pos;
}
static int racer_count(void) {
    int n = 0;
    for (int k = 0; k < RACERS; k++) if (rc[k].kind != RK_NONE) n++;
    return n;
}

/* ------------------------------------------------------------------------ */
/* Camera + scene                                                           */
/* ------------------------------------------------------------------------ */
static void render_scene(void) {
    racer_t *p = &rc[0];
    cam_z = p->z;
    cam_x = (int32_t)p->x * ROAD_W / 256;
    cam_y = CAM_H + height_at_z(p->z + PLAYER_DZ);
    int32_t base = cam_z / SEG_LEN;
    if (!(flags_at(base + 3, section_of(base + 3)) & F_TUNNEL)) draw_background();
    fill(0, HORIZON + 4, W / 2, H - HORIZON - 4, T_LAND_FAR);
    fill(W / 2, HORIZON + 4, W / 2, H - HORIZON - 4, T_SEA_FAR);
    render_road();
    render_sprites();
    draw_player();
}

/* ------------------------------------------------------------------------ */
/* HUD                                                                      */
/* ------------------------------------------------------------------------ */
static void draw_hud(void) {
    char buf[40];
    racer_t *p = &rc[0];
    int secs = (int)((time_left + 59) / 60);
    text(6, 4, "TIME", 1, C_YELLOW, C_SHADOW);
    utoa_((uint32_t)secs, buf, 2);
    text(6, 13, buf, 3, secs <= 10 && (frame & 16) ? C_RED : C_YELLOW, C_SHADOW);
    text_r(314, 4, "SCORE", 1, C_CYAN, C_SHADOW);
    utoa_(score, buf, 1);
    text_r(314, 13, buf, 2, C_WHITE, 0);
    /* leg progress bar */
    int leg = cur_leg, len = leg_start[leg + 1] - leg_start[leg];
    int32_t ps = (p->z + PLAYER_DZ) / SEG_LEN - leg_start[leg];
    int bx = 88, bw = 144;
    text_c(160, 3, town_short[leg + 1], 1, C_WHITE, C_SHADOW);
    fill(bx, 13, bw, 5, C_PANEL);
    fill(bx, 13, (int)(clampi((int)ps, 0, len) * bw / len), 5, C_ORANGE);
    for (int k = 1; k < RACERS; k++) {
        racer_t *r = &rc[k];
        if (r->kind == RK_NONE) continue;
        int32_t rs = r->z / SEG_LEN - leg_start[leg];
        if (rs < 0 || rs > len) continue;
        fill(bx + (int)(rs * bw / len) - 1, 12, 3, 7, car_dot[r->car]);
    }
    fill(bx + (int)(clampi((int)ps, 0, len) * bw / len) - 1, 11, 3, 9, C_WHITE);

    /* position */
    if (racer_count() > 1) {
        char *q = scat(buf, "POS ");
        q = utoa_((uint32_t)race_position(), q, 1);
        q = scat(q, "/");
        utoa_((uint32_t)racer_count(), q, 1);
        text_r(314, 30, buf, 1, C_GOLD, C_SHADOW);
    }
    if (net_mode) {
        char *q = scat(buf, "NET ");
        q = utoa_((uint32_t)peer_count + 1, q, 1);
        scat(q, "P");
        text_r(314, 40, buf, 1, C_GREEN, C_SHADOW);
    }
    /* speedometer */
    int kmh = p->speed * 196 / 3200;
    utoa_((uint32_t)kmh, buf, 1);
    text_r(58, 178, buf, 2, C_WHITE, C_SHADOW);
    text(62, 185, "KM/H", 1, C_WHITE, C_SHADOW);
    panel(6, 160, 22, 12, p->gear == GEAR_HI ? C_RED : C_BLUE, C_WHITE);
    text(9, 163, p->gear == GEAR_HI ? "HI" : "LO", 1, C_WHITE, 0);
    int rpm = p->speed * 60 / car_top[p->car];
    fill(32, 164, 40, 4, C_PANEL);
    fill(32, 164, imin(40, rpm * (p->gear == GEAR_LO ? 2 : 1) * 40 / 60), 4, rpm > 50 ? C_RED : C_GREEN);
    /* ingredient basket */
    spr(SPR_ING_0 + leg, -1, 286, 196, 16, 0, 0);
    char *q = scat(buf, "X");
    utoa_(got[leg], q, 1);
    text(296, 186, buf, 1, C_WHITE, C_SHADOW);
    int total = 0;
    for (int i = 0; i < LEGS; i++) total += got[i];
    q = scat(buf, "PANINO ");
    utoa_((uint32_t)total, q, 1);
    text_r(314, 174, buf, 1, C_BUN_L, C_SHADOW);
}

static void draw_checkpoint_overlay(void) {
    char buf[40];
    int leg = cur_leg;
    int t = checkpoint_ticks;
    int y = t > 220 ? (240 - t) * 3 : 60;
    y = imin(y, 60);
    panel(20, y - 26, 280, 52, C_PANEL, C_GOLD);
    text_c(160, y - 20, town_names[leg], 1, C_GOLD, 0);
    char *q = scat(buf, "EXTENDED TIME +");
    q = utoa_((uint32_t)leg_bonus_seconds(leg), q, 1);
    text_c(160, y - 8, buf, 2, (frame & 8) ? C_YELLOW : C_WHITE, C_SHADOW);
    text_c(168, y + 12, ingredient_names[leg], 1, C_WHITE, 0);
    spr(SPR_ING_0 + leg, -1, 160 - text_w(ingredient_names[leg], 1) / 2 - 4, y + 21, 16, 0, 0);
    if (t < 200) draw_map(206, 118, 1, leg, (rc[0].z + PLAYER_DZ) / SEG_LEN);
}

/* ------------------------------------------------------------------------ */
/* Panino                                                                   */
/* ------------------------------------------------------------------------ */
static int stars_for(int n) { return n <= 0 ? 0 : n < 4 ? 1 : n < 8 ? 2 : 3; }
static int total_stars(void) { int s = 0; for (int i = 0; i < LEGS; i++) s += stars_for(got[i]); return s; }

static void half_ellipse(int cx, int cy, int rx, int ry, int top, uint8_t c) {
    for (int dy = 0; dy < ry; dy++) {
        int dx = rx;   /* widest dx inside the ellipse on this row */
        while (dx > 0 && dx * dx * ry * ry + dy * dy * rx * rx > rx * rx * ry * ry) dx--;
        fill(cx - dx, top ? cy - dy - 1 : cy + dy, dx * 2 + 1, 1, c);
    }
}
static void draw_layer(int leg, int cx, int y, int rx) {
    int n = got[leg];
    int th = imin(10, 3 + n / 2);
    switch (leg) {
    case 1:  /* pomodorini */
        for (int k = -rx + 6; k < rx - 4; k += 10) { fill(cx + k, y - 6, 9, 7, C_RED); fill(cx + k + 2, y - 5, 3, 2, C_PINK); }
        break;
    case 2:  /* provolone */
        fill(cx - rx + 2, y - th, rx * 2 - 4, th, C_BUN_L); fill(cx - rx + 2, y - th, rx * 2 - 4, 1, C_WHITE);
        break;
    case 3:  /* olive oil drizzle */
        for (int k = -rx + 4; k < rx - 4; k += 6) fill(cx + k, y - 2 - (k & 2), 5, 2, C_GOLD);
        break;
    case 4:  /* lemon zest */
        for (int k = -rx + 8; k < rx - 8; k += 14) { fill(cx + k, y - 4, 10, 4, C_YELLOW); fill(cx + k + 2, y - 3, 6, 1, C_LOGO0); }
        break;
    case 5:  /* fried zucchini */
        for (int k = -rx + 4; k < rx - 6; k += 9) { fill(cx + k, y - 5, 8, 5, C_LETTUCE); fill(cx + k + 2, y - 4, 4, 3, C_BUN_CUT); }
        break;
    case 6:  /* fior di latte */
        fill(cx - rx + 4, y - th - 2, rx * 2 - 8, th + 2, C_WHITE);
        for (int k = -rx + 10; k < rx - 10; k += 16) fill(cx + k, y - th - 4, 10, 2, C_WHITE);
        break;
    case 7:  /* anchovies */
        for (int k = -rx + 6; k < rx - 12; k += 16) { fill(cx + k, y - 4, 13, 3, C_GREY); fill(cx + k + 12, y - 5, 3, 5, C_GREY); fill(cx + k + 1, y - 4, 2, 1, C_BLACK); }
        break;
    default: /* tuna */
        for (int k = -rx + 4; k < rx - 6; k += 7) fill(cx + k, y - 5 + (k & 3), 7, 4, C_PASTEL5);
        break;
    }
}
static int layer_height(int leg) { return leg == 3 ? 3 : leg == 2 || leg == 6 ? imin(12, 5 + got[leg] / 2) : 7; }

static void draw_panino_scene(void) {
    char buf[40];
    draw_sky();
    draw_sun();
    fill(0, HORIZON, W, H - HORIZON, T_SEA_L);
    for (int y = HORIZON + 4; y < H; y += 6) fill((int)((y * 37 + frame) % 280), y, 30, 1, T_SEA_GLINT);
    /* the table: a Vietri majolica plate */
    half_ellipse(98, 164, 92, 22, 0, C_WHITE);
    half_ellipse(98, 164, 80, 17, 0, C_CYAN);
    half_ellipse(98, 164, 64, 12, 0, C_YELLOW);
    fill(6, 162, 186, 4, C_WHITE);
    int cx = 98, y = 160;
    int stale = got[0] == 0;
    uint8_t bun = stale ? C_GREY : C_BUN, bund = stale ? C_DKGREY : C_BUN_D;
    half_ellipse(cx, y - 1, 66, 10, 1, bund);
    half_ellipse(cx, y - 3, 64, 8, 1, bun);
    y -= 10;
    fill(cx - 64, y - 2, 128, 3, C_BUN_CUT);
    int shown = 0;
    for (int leg = 1; leg < LEGS; leg++) {
        if (!got[leg]) continue;
        if (shown >= panino_layers) break;
        draw_layer(leg, cx, y, 62);
        y -= layer_height(leg);
        shown++;
    }
    if (panino_layers > shown || panino_layers >= 8) {
        /* the rosetta crown with its five-petal cut */
        fill(cx - 62, y - 2, 124, 3, C_BUN_CUT);
        half_ellipse(cx, y - 2, 64, 30, 1, bund);
        half_ellipse(cx, y - 3, 61, 28, 1, bun);
        half_ellipse(cx - 14, y - 18, 22, 9, 1, stale ? C_GREY : C_BUN_L);
        for (int k = -2; k <= 2; k++) {
            fill(cx + k * 11 - 1, y - 26 + iabs(k) * 3, 3, 18 - iabs(k) * 4, bund);
        }
        fill(cx - 3, y - 24, 6, 6, stale ? C_DKGREY : C_BUN_L);
    }
    /* scorecard */
    panel(198, 8, 116, 150, C_PANEL, C_GOLD);
    text_c(256, 13, "IL PANINO", 1, C_GOLD, 0);
    for (int i = 0; i < LEGS; i++) {
        int yy = 26 + i * 14;
        spr(SPR_ING_0 + i, -1, 210, yy + 12, 12, 0, 0);
        utoa_(got[i], buf, 1);
        text(220, yy + 3, buf, 1, C_WHITE, 0);
        int st = stars_for(got[i]);
        for (int k = 0; k < 3; k++) text(244 + k * 10, yy + 3, "*", 1, k < st ? C_GOLD : C_DKGREY, 0);
    }
    int ts = total_stars();
    const char *verdict = ts >= 24 ? "IL MIGLIORE DELLA CAMPANIA!" : ts >= 18 ? "SPETTACOLARE!" : ts >= 12 ? "BUONO ASSAI" :
                          ts >= 6 ? "SI PUO' FARE..." : "CHE TRISTEZZA";
    panel(4, 170, 312, 27, C_PANEL, C_GOLD);
    char *q = scat(buf, "STELLE ");
    q = utoa_((uint32_t)ts, q, 1);
    scat(q, "/27");
    text(10, 174, buf, 1, C_GOLD, 0);
    text(10, 186, verdict, 1, (frame & 16) ? C_YELLOW : C_WHITE, 0);
    q = scat(buf, "SCORE ");
    utoa_(score, q, 1);
    text_r(310, 174, buf, 1, C_WHITE, 0);
    text_c(98, 6, rc[0].finished == 1 ? "VIETRI SUL MARE!" : "TEMPO SCADUTO!", 2, C_WHITE, C_SHADOW);
    if (state_ticks > 200 && (frame & 32)) text_r(310, 186, "A: CONTINUE", 1, C_CYAN, 0);
}

/* ------------------------------------------------------------------------ */
/* Screens                                                                  */
/* ------------------------------------------------------------------------ */
static const uint8_t logo_ramp[7] = {C_LOGO0, C_LOGO1, C_LOGO1, C_LOGO2, C_LOGO3, C_LOGO4, C_LOGO5};

static void draw_logo(int y) {
    const char *t = "OUTBUN";
    int sc = 6, x = 160 - text_w(t, sc) / 2;
    for (const char *p = t; *p; p++, x += 6 * sc) {
        draw_char(x + 3, y + 4, *p, sc, C_SHADOW, 0);
        draw_char(x, y, *p, sc, 0, logo_ramp);
    }
    fill(60, y + 45, 200, 2, C_LOGO2);
    text_c(160, y + 50, "NAPOLI '97", 2, C_WHITE, C_SHADOW);
}

static void start_race(void);
static int race_track(void) { return cur_leg >= 4 ? TRK_RACE2 : TRK_RACE; }

static void enter_state(int s) {
    state = (uint8_t)s;
    state_ticks = 0;
    menu_sel = 0;
}

static void begin_attract(void) {
    attract = 1;
    rng ^= prg32_ticks_ms() | 1u;
    int leg = (int)(rnd() % LEGS);
    chosen_car = 0;
    race_setup(leg_start[leg] + 30, 1);
    rc[0].gear = GEAR_HI;
    rc[0].speed = 2400;
    for (int k = 1; k < RACERS; k++) rc[k].speed = 2400;
    phase = PH_RACE;
    music(TRK_TITLE);
}

static void title_update(void) {
    update_player(1);
    for (int k = 1; k < RACERS; k++) update_ai(&rc[k]);
    update_traffic();
    if (rc[0].z / SEG_LEN > route_segs - 60) begin_attract();
}

static void draw_title(void) {
    render_scene();
    draw_logo(18);
    if ((state_ticks / 30) & 1) text_c(160, 136, "PRESS A", 2, C_WHITE, C_SHADOW);
    char buf[40];
    if (best_score) {
        char *q = scat(buf, "BEST ");
        q = utoa_(best_score, q, 1);
        if (best_player[0]) { q = scat(q, " "); for (int i = 0; i < 12 && best_player[i]; i++) { *q++ = best_player[i]; } *q = 0; }
        text_c(160, 156, buf, 1, C_GOLD, C_SHADOW);
    }
    text_c(160, 190, "A CLASSIC 500, NINE LEGS, ONE PANINO", 1, C_CYAN, C_SHADOW);
}

static void draw_menu_panel(const char *title, int y, int h) {
    panel(40, y, 240, h, C_PANEL, C_GOLD);
    text_c(160, y + 6, title, 1, C_GOLD, 0);
}

static void draw_mode(void) {
    render_scene();
    draw_logo(8);
    draw_menu_panel("GAME MODE", 96, 76);
    text_c(160, 116, "ARCADE  1P VS CPU", 1, menu_sel == 0 ? C_YELLOW : C_WHITE, 0);
    text_c(160, 134, "NETWORK 2-4 PLAYERS", 1, menu_sel == 1 ? C_YELLOW : C_WHITE, 0);
    fill(64, 113 + menu_sel * 18, 6, 12, C_YELLOW);
    text_c(160, 156, "A SELECT  B BACK", 1, C_GREY, 0);
}

static void draw_car_select(void) {
    render_scene();
    panel(10, 8, 300, 184, C_PANEL, C_GOLD);
    text_c(160, 14, "CHOOSE YOUR CLASSIC", 1, C_GOLD, 0);
    for (int c = 0; c < 4; c++) {
        int x = 46 + c * 76, sel = c == chosen_car;
        if (sel) panel(x - 36, 26, 72, 70, C_PANEL2, (frame & 8) ? C_YELLOW : C_WHITE);
        int bob = sel ? isin((int)frame * 16) / 64 : 0;
        spr(SPR_CAR_500 + c, -1, x, 76 + bob, sel ? 64 : 48, 0, 0);
        text_c(x, 82, car_tag[c], 1, sel ? C_YELLOW : C_GREY, 0);
    }
    text_c(160, 104, car_names[chosen_car], 2, C_WHITE, C_SHADOW);
    int top = car_top[chosen_car] - 2900, acc = car_accel[chosen_car] - 12, grip = car_grip[chosen_car] - 12;
    text(40, 128, "SPEED", 1, C_CYAN, 0);
    text(40, 142, "ACCEL", 1, C_CYAN, 0);
    text(40, 156, "GRIP", 1, C_CYAN, 0);
    fill(90, 128, 180, 7, C_DKGREY); fill(90, 128, top * 180 / 400, 7, C_ORANGE);
    fill(90, 142, 180, 7, C_DKGREY); fill(90, 142, acc * 180 / 10, 7, C_ORANGE);
    fill(90, 156, 180, 7, C_DKGREY); fill(90, 156, grip * 180 / 10, 7, C_ORANGE);
    text_c(160, 176, chosen_car == 0 ? "THE HERO CAR - BIANCA E TRUCCATA" : "SOUPED-UP AND READY", 1, C_GREY, 0);
}

static void draw_lobby(void) {
    char buf[40];
    render_scene();
    draw_menu_panel("NETWORK LOBBY", 20, 164);
    if (!net_ok) {
        text_c(160, 70, "MULTIPLAYER UNAVAILABLE", 1, C_RED, 0);
        text_c(160, 90, "CHECK WI-FI AND SERVER", 1, C_WHITE, 0);
        text_c(160, 150, "B: RACE WITH CPU", 1, C_CYAN, 0);
        return;
    }
    int y = 40;
    text(56, y, "YOU", 1, C_YELLOW, 0);
    text(110, y, car_names[chosen_car], 1, C_WHITE, 0);
    text(236, y, local_ready ? "READY" : "...", 1, local_ready ? C_GREEN : C_GREY, 0);
    for (int i = 0; i < peer_count; i++) {
        y += 16;
        char *q = scat(buf, "P");
        utoa_((uint32_t)i + 2, q, 1);
        text(56, y, buf, 1, C_CYAN, 0);
        text(110, y, car_names[peers[i].sprite & 3], 1, C_WHITE, 0);
        int rd = (peers[i].flags & (NF_READY | NF_RACING)) != 0;
        text(236, y, rd ? "READY" : "...", 1, rd ? C_GREEN : C_GREY, 0);
    }
    for (int i = peer_count; i < 3; i++) { y += 16; text(56, y, "--", 1, C_DKGREY, 0); text(110, y, "CPU", 1, C_DKGREY, 0); }
    text_c(160, 120, peer_count ? "ALL READY: THE RACE STARTS" : "WAITING FOR PLAYERS", 1, (frame & 16) ? C_WHITE : C_GREY, 0);
    text_c(160, 150, local_ready ? "A: NOT READY" : "A: READY", 1, C_CYAN, 0);
    text_c(160, 164, "B: LEAVE AND RACE THE CPU", 1, C_CYAN, 0);
}

static void draw_intro(void) {
    char buf[40];
    draw_sky();
    fill(0, HORIZON, W, H - HORIZON, T_SEA_L);
    draw_map(8, 30, 0, 0, leg_start[0] + START_SEG);
    text(8, 6, "NAPOLI > VIETRI SUL MARE", 1, C_WHITE, C_SHADOW);
    text(8, 16, "122 KM OF COAST ROAD, 1997", 1, C_YELLOW, C_SHADOW);
    panel(214, 30, 100, 154, C_PANEL, C_GOLD);
    text_c(264, 36, "THE RECIPE", 1, C_GOLD, 0);
    for (int i = 0; i < LEGS; i++) {
        spr(SPR_ING_0 + i, -1, 226, 60 + i * 13, 12, 0, 0);
        text(236, 52 + i * 13, town_short[i + 1], 1, C_WHITE, 0);
    }
    text_c(264, 172, "COLLECT ALL", 1, C_CYAN, 0);
    char *q = scat(buf, "TIME ");
    q = utoa_((uint32_t)(time_left / 60), q, 1);
    scat(q, " SEC");
    text(8, 188, buf, 1, C_WHITE, C_SHADOW);
    if (state_ticks > 40 && (frame & 16)) text_r(206, 188, "A: VIA!", 1, C_YELLOW, C_SHADOW);
}

static void draw_results(void) {
    char buf[40];
    draw_sky();
    fill(0, HORIZON, W, H - HORIZON, T_SEA_L);
    panel(20, 12, 280, 176, C_PANEL, C_GOLD);
    text_c(160, 20, "CLASSIFICA", 2, C_GOLD, 0);
    /* sort racers by progress, finishers first */
    int order[RACERS], n = 0;
    for (int k = 0; k < RACERS; k++) if (rc[k].kind != RK_NONE) order[n++] = k;
    for (int a = 1; a < n; a++) {
        int v = order[a], b = a - 1;
        while (b >= 0 && (rc[order[b]].z + (rc[order[b]].finished == 1 ? 1 << 28 : 0)) <
                         (rc[v].z + (rc[v].finished == 1 ? 1 << 28 : 0))) { order[b + 1] = order[b]; b--; }
        order[b + 1] = v;
    }
    for (int i = 0; i < n; i++) {
        racer_t *r = &rc[order[i]];
        int y = 48 + i * 20;
        utoa_((uint32_t)i + 1, buf, 1);
        text(36, y, buf, 2, C_GOLD, 0);
        text(64, y + 4, r->kind == RK_LOCAL ? "YOU" : r->kind == RK_PEER ? "PLAYER" : "CPU", 1, r->kind == RK_LOCAL ? C_YELLOW : C_WHITE, 0);
        text(118, y + 4, car_names[r->car], 1, C_WHITE, 0);
        char *q = scat(buf, "X");
        utoa_(r->kind == RK_LOCAL ? (uint32_t)rc[0].got : r->got, q, 1);
        text(236, y + 4, buf, 1, C_BUN_L, 0);
        if (r->finished == 2) text(262, y + 4, "OUT", 1, C_RED, 0);
    }
    char *q = scat(buf, "FINAL SCORE ");
    utoa_(score, q, 1);
    text_c(160, 140, buf, 1, C_WHITE, 0);
    if (score >= best_score && score) text_c(160, 154, "NEW BEST!", 1, (frame & 8) ? C_YELLOW : C_GOLD, 0);
    if (state_ticks > 90) text_c(160, 172, "A: TITLE", 1, C_CYAN, 0);
}

/* ------------------------------------------------------------------------ */
/* Race flow                                                                */
/* ------------------------------------------------------------------------ */
static void start_race(void) {
    attract = 0;
    race_setup(leg_start[0] + START_SEG - 9, 0);
    if (net_mode) {
        net_poll();
        /* remote players replace the AI; unpicked cars stay CPU */
        net_bind_racers();
    }
    phase = PH_COUNTDOWN;
    phase_ticks = 0;
    enter_state(ST_INTRO);
    prg32_audio_stop_track();
}

static void finish_race(int how) {
    racer_t *p = &rc[0];
    p->finished = (uint8_t)how;
    phase = how == 1 ? PH_GOAL : PH_TIMEUP;
    phase_ticks = 0;
    final_place = (uint8_t)race_position();
    if (how == 1) {
        score += (uint32_t)(time_left / 60) * 10000u;
        score += final_place == 1 ? 50000u : final_place == 2 ? 30000u : final_place == 3 ? 15000u : 0u;
        music(TRK_GOAL);
    } else {
        music(TRK_OVER);
    }
    score += (uint32_t)total_stars() * 20000u;
    engine_sound(0);
}

static void race_update(void) {
    racer_t *p = &rc[0];
    phase_ticks++;
    if (phase == PH_COUNTDOWN) {
        if (phase_ticks % 60 == 1 && phase_ticks < 180) sfx_bell(72);
        if (phase_ticks == 180) { phase = PH_RACE; phase_ticks = 0; sfx_bell(84); music(race_track()); }
        engine_sound(1);
    } else if (phase == PH_RACE) {
        update_player(0);
        engine_sound(1);
        int32_t seg = (p->z + PLAYER_DZ) / SEG_LEN;
        if (seg >= route_segs) finish_race(1);
        else if (--time_left <= 0) { time_left = 0; finish_race(2); }
        else if (seg >= leg_start[cur_leg + 1] && cur_leg < LEGS - 1) {
            cur_leg++;
            time_left += (int32_t)leg_bonus_seconds(cur_leg) * 60;
            checkpoint_ticks = 240;
            jingle_ticks = 150;
            music(TRK_CHECK);
            theme_set(cur_leg, 0);
            scenery_build(cur_leg);
        }
        if (input_edge & PRG32_BTN_START) { enter_state(ST_PAUSE); engine_sound(0); return; }
    } else {
        /* goal run-out or time-up coast */
        if (phase == PH_GOAL) { p->speed = (int16_t)imax(1200, p->speed - 10); p->z += p->speed >> 4; }
        else { p->speed = (int16_t)imax(0, p->speed - 18); p->z += p->speed >> 4; }
        if (phase_ticks > 200) { panino_layers = 0; enter_state(ST_PANINO); }
    }
    for (int k = 1; k < RACERS; k++) if (rc[k].kind == RK_AI) update_ai(&rc[k]);
    update_traffic();
    if (checkpoint_ticks) checkpoint_ticks--;
    if (jingle_ticks && !--jingle_ticks && phase == PH_RACE) music(race_track());
    if (bump_ticks) bump_ticks--;
    if (shake) shake--;
    if (spark_ticks) spark_ticks--;
    if (theme_t < 256) { theme_t = (int16_t)imin(256, theme_t + 4); theme_write(); }
}

static void draw_race(void) {
    char buf[8];
    render_scene();
    draw_hud();
    if (phase == PH_COUNTDOWN) {
        int n = 3 - (int)(phase_ticks / 60);
        /* the semaforo */
        panel(134, 40, 52, 22, C_BLACK, C_WHITE);
        for (int k = 0; k < 3; k++) fill(140 + k * 16, 45, 10, 12, k <= 3 - n ? C_RED : C_DKGREY);
        utoa_((uint32_t)n, buf, 1);
        text_c(160, 72, buf, 5, C_YELLOW, C_SHADOW);
    } else if (phase == PH_RACE && phase_ticks < 60) {
        panel(134, 40, 52, 22, C_BLACK, C_WHITE);
        for (int k = 0; k < 3; k++) fill(140 + k * 16, 45, 10, 12, C_GREEN);
        text_c(160, 72, "VIA!", 5, C_GREEN, C_SHADOW);
    } else if (phase == PH_TIMEUP) {
        text_c(160, 70, "TEMPO SCADUTO!", 3, (frame & 8) ? C_RED : C_WHITE, C_SHADOW);
    } else if (phase == PH_GOAL) {
        text_c(160, 64, "ARRIVO!", 4, (frame & 8) ? C_YELLOW : C_WHITE, C_SHADOW);
    }
    if (checkpoint_ticks && phase == PH_RACE) draw_checkpoint_overlay();
    else if (phase == PH_RACE && time_left < 600 && (frame & 16)) text_c(160, 40, "HURRY UP!", 2, C_RED, C_SHADOW);
}

/* ------------------------------------------------------------------------ */
/* PRG32 entry points                                                       */
/* ------------------------------------------------------------------------ */
void outbun_init(void) {
    rng = 0x5eed1997u ^ prg32_ticks_ms();
    palette_init();
    route_init();
    prg32_band_set_game_info("OUTBUN NAPOLI '97");
    prg32_gfx_clear_indexed(C_BLACK);
    enter_state(ST_TITLE);
    begin_attract();
    last_ms = 0;
    prg32_score_t top;                 /* persistent best from the scores partition */
    if (prg32_score_count(SCORE_GAME) > 0 && prg32_score_get(SCORE_GAME, 0, &top) == 0) {
        best_score = top.score;
        for (int i = 0; i < 23 && top.player[i]; i++) best_player[i] = top.player[i];
    }
}

static void step(void) {
    frame++;
    state_ticks++;
    if (net_mode && net_ok) {
        net_poll();
        net_publish();
    }
    switch (state) {
    case ST_TITLE:
        title_update();
        if (input_edge & (PRG32_BTN_A | PRG32_BTN_START)) { enter_state(ST_MODE); sfx_bell(79); }
        break;
    case ST_MODE:
        title_update();
        if (input_edge & (PRG32_BTN_UP | PRG32_BTN_DOWN)) { menu_sel ^= 1; sfx_bell(74); }
        if (input_edge & PRG32_BTN_B) enter_state(ST_TITLE);
        if (input_edge & PRG32_BTN_A) {
            net_mode = menu_sel;
            enter_state(ST_CAR);
            sfx_bell(79);
        }
        break;
    case ST_CAR:
        title_update();
        if (input_edge & PRG32_BTN_LEFT) { chosen_car = (uint8_t)((chosen_car + 3) & 3); sfx_bell(74); }
        if (input_edge & PRG32_BTN_RIGHT) { chosen_car = (uint8_t)((chosen_car + 1) & 3); sfx_bell(74); }
        if (input_edge & PRG32_BTN_B) enter_state(ST_MODE);
        if (input_edge & PRG32_BTN_A) {
            sfx_bell(84);
            if (net_mode) { local_ready = 0; net_join(); enter_state(ST_LOBBY); }
            else start_race();
        }
        break;
    case ST_LOBBY: {
        title_update();
        rc[0].car = chosen_car;
        if (input_edge & PRG32_BTN_A && net_ok) { local_ready ^= 1; sfx_bell(local_ready ? 84 : 72); }
        if (input_edge & PRG32_BTN_B) { net_leave(); start_race(); break; }
        int all = net_ok && local_ready && peer_count > 0;
        for (int i = 0; i < peer_count; i++) if (!(peers[i].flags & (NF_READY | NF_RACING))) all = 0;
        if (all) start_race();
        break;
    }
    case ST_INTRO:
        if (theme_t < 256) { theme_t = (int16_t)imin(256, theme_t + 8); theme_write(); }
        if ((state_ticks > 40 && (input_edge & PRG32_BTN_A)) || (net_mode && state_ticks > 180) || state_ticks > 600) {
            enter_state(ST_RACE);
            phase = PH_COUNTDOWN; phase_ticks = 0;
        }
        break;
    case ST_RACE:
        if (net_mode) net_bind_racers();
        race_update();
        break;
    case ST_PAUSE:
        if (input_edge & (PRG32_BTN_UP | PRG32_BTN_DOWN)) menu_sel ^= 1;
        if (input_edge & (PRG32_BTN_A | PRG32_BTN_START)) {
            if (menu_sel == 0) enter_state(ST_RACE);
            else { net_leave(); enter_state(ST_TITLE); begin_attract(); }
        }
        break;
    case ST_PANINO:
        if (net_mode) net_bind_racers();
        if (state_ticks % 24 == 0 && panino_layers < 9) { panino_layers++; sfx_bell(60 + panino_layers * 3); }
        if (state_ticks > 200 && (input_edge & PRG32_BTN_A)) {
            if (score > best_score) { best_score = score; best_player[0] = 0; }
            prg32_score_submit_current_player(SCORE_GAME, score);   /* local top 5 + Store sync */
            enter_state(ST_RESULTS);
        }
        break;
    case ST_RESULTS:
        if (net_mode) net_bind_racers();
        if (state_ticks > 90 && (input_edge & PRG32_BTN_A)) {
            net_leave();
            enter_state(ST_TITLE);
            begin_attract();
        }
        break;
    default:
        break;
    }
    input_edge = 0;
}

void outbun_update(void) {
    uint32_t now = prg32_ticks_ms();
    if (!last_ms) last_ms = now;
    uint32_t el = now - last_ms;
    last_ms = now;
    if (el > 100) el = 100;
    tick_acc += el * 60;
    input_now = prg32_input_read();
    input_edge |= input_now & ~input_prev;
    input_prev = input_now;
    int steps = 0;
    while (tick_acc >= 1000 && steps < 4) { tick_acc -= 1000; step(); steps++; }
}

void outbun_draw(void) {
    switch (state) {
    case ST_TITLE: draw_title(); break;
    case ST_MODE: draw_mode(); break;
    case ST_CAR: draw_car_select(); break;
    case ST_LOBBY: draw_lobby(); break;
    case ST_INTRO: draw_intro(); break;
    case ST_RACE: draw_race(); break;
    case ST_PAUSE:
        draw_race();
        draw_menu_panel("PAUSA", 70, 60);
        text_c(160, 92, "CONTINUE", 1, menu_sel == 0 ? C_YELLOW : C_WHITE, 0);
        text_c(160, 108, "QUIT TO TITLE", 1, menu_sel == 1 ? C_YELLOW : C_WHITE, 0);
        break;
    case ST_PANINO: draw_panino_scene(); break;
    case ST_RESULTS: draw_results(); break;
    default: break;
    }
}
