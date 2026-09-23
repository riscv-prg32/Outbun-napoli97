/*
 * OutBun host harness.
 *
 * Compiles the real cartridge source against a small software PRG32: an
 * indexed 320x200 framebuffer, the 256-entry palette, a millisecond clock, a
 * scripted joystick and a scriptable multiplayer relay. It then plays the game
 * through its screens, drives the full Napoli -> Vietri route with a simple
 * bot, exercises the network lobby and peer takeover, and optionally dumps
 * frames (OUTBUN_SHOTS=dir) that tools/render_screens.py turns into PNGs.
 *
 * Every rect call is bounds-checked; the number of calls per frame is the
 * performance proxy reported at the end (each call is one span fill on the
 * ESP32-C6 indexed framebuffer).
 */
#define OUTBUN_HOST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/game.c"

static uint8_t fb[H][W];
static uint16_t pal[256];
static uint32_t now_ms = 1000, joy, frame_calls, max_calls, sum_calls, counted_frames;
static int mp_available = 1, mp_joined, mp_peers;
static prg32_player_state_t mp_state[3];
static int16_t pub_x, pub_y; static uint16_t pub_sprite, pub_flags; static uint32_t pub_input;
static int failures;

#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static uint32_t saved_score; static int submits;
int prg32_score_submit_current_player(const char *game, uint32_t sc) { CHECK(strcmp(game, "OutBun-napoli97") == 0, "score game"); submits++; if (sc > saved_score) saved_score = sc; return 0; }
int prg32_score_count(const char *game) { (void)game; return saved_score ? 1 : 0; }
int prg32_score_get(const char *game, int i, prg32_score_t *o) { (void)game; if (i || !saved_score) return -1; memset(o, 0, sizeof *o); strcpy(o->player, "RAF"); o->score = saved_score; return 0; }
uint32_t prg32_ticks_ms(void) { return now_ms; }
uint32_t prg32_input_read(void) { return joy; }
void prg32_gfx_rect_indexed(int x, int y, int w, int h, uint8_t index) {
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > W || y + h > H) {
        printf("FAIL: rect out of bounds %d,%d %dx%d\n", x, y, w, h);
        failures++;
        return;
    }
    if (index < 16 || index > 231) { printf("FAIL: system palette index %u used\n", index); failures++; }
    frame_calls++;
    for (int j = y; j < y + h; j++) memset(&fb[j][x], index, (size_t)w);
}
void prg32_gfx_clear_indexed(uint8_t index) { memset(fb, index, sizeof fb); }
void prg32_palette_set(uint8_t index, uint16_t rgb565) { pal[index] = rgb565; }
void prg32_band_set_game_info(const char *text) { (void)text; }
void prg32_audio_play_track(uint16_t track_id) { CHECK(track_id < 6, "track id"); }
void prg32_audio_stop_track(void) {}
void prg32_audio_note(uint8_t c, uint8_t i, uint8_t n, uint8_t v, uint32_t d) { (void)c; (void)i; (void)n; (void)v; (void)d; }
void prg32_audio_note_on(uint8_t c, uint8_t i, uint8_t n, uint8_t v) { (void)c; (void)i; (void)n; (void)v; }
void prg32_audio_note_off(uint8_t c) { (void)c; }
void prg32_multiplayer_init(void) {}
bool prg32_multiplayer_available(void) { return mp_available; }
int prg32_multiplayer_join(const char *sig, uint32_t flags) { (void)flags; CHECK(strcmp(sig, "outbun-napoli97:v1") == 0, "room"); mp_joined = 1; return 0; }
int prg32_multiplayer_leave(void) { mp_joined = 0; return 0; }
void prg32_multiplayer_tick(void) {}
int prg32_multiplayer_set_local_state(int16_t x, int16_t y, uint16_t s, uint16_t f) { pub_x = x; pub_y = y; pub_sprite = s; pub_flags = f; return 0; }
int prg32_multiplayer_set_input(uint32_t in) { pub_input = in; return 0; }
int prg32_multiplayer_get_peer_count(void) { return mp_joined ? mp_peers : 0; }
int prg32_multiplayer_get_peer(int i, prg32_player_state_t *o) { if (i < 0 || i >= mp_peers) return -1; *o = mp_state[i]; return 0; }

static const char *shot_dir;
static void shot(const char *name) {
    if (!shot_dir) return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", shot_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint16_t c = pal[fb[y][x]];
            uint8_t rgb[3] = {(uint8_t)((c >> 11) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63), (uint8_t)((c & 31) * 255 / 31)};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static int tick_parity;
static void frame_(void) {
    now_ms += (tick_parity++ % 3 == 2) ? 16 : 17;
    outbun_update();
    frame_calls = 0;
    outbun_draw();
    if (state == ST_RACE) {
        sum_calls += frame_calls; counted_frames++;
        if (frame_calls > max_calls) max_calls = frame_calls;
    }
}
static void frames(int n) { while (n--) frame_(); }
static void press(uint32_t b) { joy = b; frame_(); frame_(); joy = 0; frame_(); }

/* competent bot: keeps the lane whose nearest obstacle is farthest away,
   grabs ingredients in that lane, brakes for hairpins and shifts up */
static int bot_lane = 1;
static int32_t lane_clear(int lane_x, int32_t cz) {
    int32_t best = 1 << 30;
    for (int k = 0; k < MAX_TRAFFIC; k++) {
        int32_t d = traffic[k].z - cz;
        if (d > -150 && iabs(traffic[k].x - lane_x) < 110 && d < best) best = d;
    }
    for (int k = 1; k < RACERS; k++) {
        int32_t d = rc[k].z - cz;
        if (rc[k].kind != RK_NONE && d > -150 && iabs(rc[k].x - lane_x) < 100 && d < best) best = d;
    }
    return best;
}
static void bot_input(void) {
    racer_t *p = &rc[0];
    int32_t cz = p->z + PLAYER_DZ;
    int32_t seg = cz / SEG_LEN;
    int c12 = curve12_at(seg + 6, section_of(seg + 6));
    int leg = leg_of(seg);
    int32_t here = lane_clear(bot_lane ? 128 : -128, cz), there = lane_clear(bot_lane ? -128 : 128, cz);
    if (here < 30 * SEG_LEN && there > here + 6 * SEG_LEN) bot_lane ^= 1;
    int tx = bot_lane ? 128 : -128;
    for (int i = 0; i < PICKUPS; i++) {
        int32_t d = pickup_seg(leg, i) - seg;
        if (d > 0 && d < 18 && !pickup_taken(leg, i) && iabs(pickup_x(leg, i) - tx) <= 128 &&
            lane_clear(pickup_x(leg, i), cz) > d * SEG_LEN + 400) { tx = pickup_x(leg, i); break; }
    }
    tx = clampi(tx + c12 * 2, -200, 200);
    uint32_t in = PRG32_BTN_A;
    if (tx > p->x + 10) in |= PRG32_BTN_RIGHT;
    if (tx < p->x - 10) in |= PRG32_BTN_LEFT;
    int32_t block = lane_clear(p->x, cz);
    if ((iabs(c12) >= 66 && p->speed > car_top[p->car] * 80 / 100) || block < 3 * SEG_LEN)
        in = (in & ~PRG32_BTN_A) | PRG32_BTN_B;
    if (p->gear == GEAR_LO && p->speed > car_top[p->car] * 50 / 100) in |= PRG32_BTN_UP;
    joy = in;
}

static void teleport(int leg, int into) {
    int32_t s = leg_start[leg] + into;
    rc[0].z = s * SEG_LEN - PLAYER_DZ;
    rc[0].speed = 2600; rc[0].gear = GEAR_HI;
    for (int k = 1; k < RACERS; k++) { rc[k].z = rc[0].z + (k * 7 + 4) * SEG_LEN; rc[k].speed = 2500; }
    for (int i = 0; i < MAX_TRAFFIC; i++) traffic_spawn(&traffic[i], rc[0].z - 30 * SEG_LEN);
    cur_leg = (uint8_t)leg;
    scenery_build(leg);
    theme_set(leg, 1);
    time_left = 60 * 60;
    checkpoint_ticks = 0;
    phase_ticks = 100;
}
static int32_t find_section_seg(int leg, uint8_t flag, int before) {
    for (int i = leg_first_section[leg]; i < leg_first_section[leg + 1]; i++)
        if (ob_sections[i].flags & flag) return sec_start[i] - leg_start[leg] - before;
    return 40;
}

int main(void) {
    shot_dir = getenv("OUTBUN_SHOTS");
    outbun_init();
    CHECK(route_segs > 9000 && route_segs < 14000, "route length");
    printf("route: %ld segments, %d sections\n", (long)route_segs, SECTION_COUNT);
    for (int l = 0; l < LEGS; l++)
        printf("  leg %d %-14s -> %-24s %4d segs  bonus %2ds  %2d km\n", l + 1, town_short[l], town_names[l + 1],
               leg_start[l + 1] - leg_start[l], leg_bonus_seconds(l), leg_km[l]);

    /* ---- title attract ---- */
    frames(420);
    CHECK(state == ST_TITLE, "title");
    shot("01-title");
    press(PRG32_BTN_A);
    CHECK(state == ST_MODE, "mode screen");
    frames(20); shot("02-mode");
    press(PRG32_BTN_A);
    CHECK(state == ST_CAR && net_mode == 0, "car select");
    press(PRG32_BTN_RIGHT); press(PRG32_BTN_LEFT);
    CHECK(chosen_car == 0, "500 is the default hero car");
    frames(30); shot("03-car-select");
    press(PRG32_BTN_A);
    CHECK(state == ST_INTRO, "intro");
    frames(60); shot("04-intro-map");
    press(PRG32_BTN_A);
    CHECK(state == ST_RACE && phase == PH_COUNTDOWN, "countdown");
    frames(100); shot("05-start-grid");
    frames(90);
    CHECK(phase == PH_RACE, "race started");
    CHECK(rc[1].kind == RK_AI && rc[2].kind == RK_AI && rc[3].kind == RK_AI, "three AI rivals");
    CHECK(rc[1].car != 0 && rc[2].car != 0 && rc[3].car != 0, "AI drive 126/Dyane/Beetle");

    /* ---- drive the whole route with the bot ---- */
    int checkpoints = 0, last_leg = 0, min_left = 1 << 30, frames_run = 0, bumps = 0;
    long spd_sum = 0, spd_n = 0;
    while (state == ST_RACE && frames_run < 60 * 60 * 8) {
        bot_input();
        frame_();
        frames_run++;
        if (cur_leg != last_leg) {
            checkpoints++;
            last_leg = cur_leg;
            printf("  checkpoint %-24s at %3ds, time left %2lds, got %d\n", town_names[cur_leg], frames_run / 60,
                   (long)(time_left / 60), got[cur_leg - 1]);
            if (cur_leg == 2) { frames(30); shot("10-checkpoint"); }
        }
        if (phase == PH_RACE && time_left < min_left) min_left = time_left;
        if (phase == PH_RACE) { spd_sum += rc[0].speed; spd_n++; if (shake == 5) bumps++; }
    }
    joy = 0;
    printf("bot: avg speed %ld km/h, %d bumps\n", spd_n ? spd_sum / spd_n * 196 / 3200 : 0, bumps);
    printf("bot: %d checkpoints, %d s driving, min time left %d s, stars %d/27, score %lu\n", checkpoints,
           frames_run / 60, min_left / 60, total_stars(), (unsigned long)score);
    CHECK(checkpoints == 8, "all eight intermediate checkpoints");
    CHECK(rc[0].finished == 1 && min_left > 0, "bot reaches Vietri sul Mare in time");
    CHECK(state == ST_PANINO, "panino screen");
    frames(260); shot("11-panino");
    press(PRG32_BTN_A);
    CHECK(state == ST_RESULTS && submits == 1 && saved_score == score, "results and persistent score");
    frames(100); shot("12-results");
    press(PRG32_BTN_A);
    CHECK(state == ST_TITLE, "back to title");

    /* ---- scenic shots on the real route ---- */
    chosen_car = 0;
    start_race();
    frames(50);
    press(PRG32_BTN_A);
    frames(200);
    CHECK(state == ST_RACE && phase == PH_RACE, "second race running");
    teleport(1, (int)find_section_seg(1, F_TUNNEL, 30)); frames(8); shot("06-pozzano-tunnel");
    teleport(1, (int)find_section_seg(1, F_TUNNEL, -30)); frames(8); shot("07-inside-galleria");
    teleport(2, (int)find_section_seg(2, F_BRIDGE, -20)); frames(8); shot("08-seiano-viaduct");
    teleport(4, 300); frames(8); shot("09-capri-from-massa");
    teleport(6, 60); frames(8); shot("13-positano");
    teleport(6, (int)find_section_seg(6, F_BRIDGE, 10)); frames(8); shot("14-furore");
    teleport(8, 150); for (int i = 0; i < 12; i++) { bot_input(); frame_(); } shot("15-vietri-sunset");
    teleport(0, 400); for (int i = 0; i < 12; i++) { bot_input(); frame_(); } shot("16-vesuvio");
    teleport(0, leg_start[1] - leg_start[0] - 150); frames(8); shot("19-castellammare-shipyard");
    teleport(6, leg_start[7] - leg_start[6] - 75); frames(8); shot("20-amalfi-duomo");
    teleport(7, 560); frames(8); shot("21-maiori-beach");
    /* time-up path */
    joy = 0;
    time_left = 3;
    frames(10);
    CHECK(phase == PH_TIMEUP, "time up");
    frames(260);
    CHECK(state == ST_PANINO && rc[0].finished == 2, "time-up panino");
    frames(220); press(PRG32_BTN_A); frames(100); press(PRG32_BTN_A);
    CHECK(state == ST_TITLE, "title after time up");

    /* ---- network lobby, start, peer binding and AI takeover ---- */
    press(PRG32_BTN_A);            /* mode */
    press(PRG32_BTN_DOWN);
    press(PRG32_BTN_A);            /* network */
    CHECK(net_mode == 1 && state == ST_CAR, "network car select");
    press(PRG32_BTN_RIGHT);        /* take the 126 */
    press(PRG32_BTN_A);
    CHECK(state == ST_LOBBY && net_ok && mp_joined, "joined lobby");
    mp_peers = 1;
    memset(mp_state, 0, sizeof mp_state);
    mp_state[0].player_id = 42; mp_state[0].sprite = 0; mp_state[0].flags = 0; mp_state[0].last_seen_ms = now_ms;
    frames(10);
    CHECK(peer_count == 1 && state == ST_LOBBY, "peer visible, not ready");
    shot("17-lobby");
    press(PRG32_BTN_A);
    CHECK(local_ready && (pub_flags & NF_READY), "local ready published");
    CHECK(state == ST_LOBBY, "waits for the peer");
    mp_state[0].flags = NF_READY; mp_state[0].last_seen_ms = now_ms;
    frames(3);
    CHECK(state == ST_INTRO, "all ready starts the race");
    frames(200);
    CHECK(state == ST_RACE, "network intro auto-advances");
    int peer_slot = -1;
    for (int k = 1; k < RACERS; k++) if (rc[k].kind == RK_PEER) peer_slot = k;
    CHECK(peer_slot > 0 && rc[peer_slot].car == 0, "remote 500 bound to a racer slot");
    int ai = 0; for (int k = 1; k < RACERS; k++) if (rc[k].kind == RK_AI) ai++;
    CHECK(ai == 2, "two CPU fill the free slots");
    CHECK(rc[0].car == 1, "local player drives the 126");
    if (peer_slot < 0) peer_slot = 1;
    mp_state[0].y = (int16_t)(car_z(&rc[0]) / SEG_LEN + 5); mp_state[0].x = -100; mp_state[0].flags = NF_RACING;
    mp_state[0].input = (uint32_t)(2400 >> 4) << 8; mp_state[0].last_seen_ms = now_ms;
    frames(200);
    CHECK(iabs((int)(rc[peer_slot].z / SEG_LEN - car_z(&rc[0]) / SEG_LEN)) < 40, "peer placed on the road");
    CHECK((pub_flags & NF_RACING) && pub_y == (int16_t)(car_z(&rc[0]) / SEG_LEN), "local racing state published");
    shot("18-network-race");
    now_ms += PEER_TIMEOUT_MS + 100;   /* the peer vanishes */
    frames(5);
    CHECK(rc[peer_slot].kind == RK_AI, "AI takes over a vanished peer");
    joy = PRG32_BTN_START; frame_(); joy = 0; frame_();
    CHECK(state == ST_PAUSE, "pause");
    press(PRG32_BTN_DOWN); press(PRG32_BTN_A);
    CHECK(state == ST_TITLE && !mp_joined, "quit leaves the room");

    /* ---- offline network (QEMU stub / no Wi-Fi) ---- */
    mp_available = 0;
    press(PRG32_BTN_A); press(PRG32_BTN_DOWN); press(PRG32_BTN_A); press(PRG32_BTN_A);
    CHECK(state == ST_LOBBY && !net_ok, "unavailable network reported");
    press(PRG32_BTN_B);
    CHECK(state == ST_INTRO && net_mode == 0, "fallback to CPU race");

    printf("draw calls per race frame: avg %lu, max %lu\n", (unsigned long)(sum_calls / (counted_frames ? counted_frames : 1)),
           (unsigned long)max_calls);
    /* ~2-3 us per span on the ESP32-C6: keep the average under ~12 ms */
    CHECK(sum_calls / (counted_frames ? counted_frames : 1) < 4500 && max_calls < 8000, "draw-call budget");
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("behavioural harness: OK\n");
    return 0;
}
