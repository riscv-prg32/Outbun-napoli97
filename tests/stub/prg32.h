/*
 * Host-side stand-in for the PRG32 public header.
 *
 * Only the subset OutBun uses is declared; every prototype, constant and
 * structure below is copied from PRG32 main (components/prg32/include/prg32.h,
 * prg32_multiplayer.h, prg32_audio.h) so the cartridge source compiles
 * unchanged for the host harness. tests/source_checks.py cross-checks these
 * declarations against a real PRG32 checkout when PRG32_ROOT is set.
 */
#ifndef PRG32_H
#define PRG32_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define PRG32_BTN_LEFT (1u << 0)
#define PRG32_BTN_RIGHT (1u << 1)
#define PRG32_BTN_UP (1u << 2)
#define PRG32_BTN_DOWN (1u << 3)
#define PRG32_BTN_A (1u << 4)
#define PRG32_BTN_B (1u << 5)
#define PRG32_BTN_START (1u << 6)
#define PRG32_BTN_SELECT PRG32_BTN_START
#define PRG32_MP_FLAG_ENABLE (1u << 0)

typedef struct {
    uint32_t player_id;
    int16_t x;
    int16_t y;
    uint16_t sprite;
    uint16_t flags;
    uint32_t input;
    uint32_t frame;
    uint32_t last_seen_ms;
} prg32_player_state_t;

typedef struct {
    char game[24];
    char player[24];
    uint32_t score;
} prg32_score_t;

uint32_t prg32_ticks_ms(void);
int prg32_score_submit_current_player(const char *game, uint32_t score);
int prg32_score_count(const char *game);
int prg32_score_get(const char *game, int index, prg32_score_t *out_score);
uint32_t prg32_input_read(void);
void prg32_gfx_rect_indexed(int x, int y, int w, int h, uint8_t index);
void prg32_gfx_clear_indexed(uint8_t index);
void prg32_palette_set(uint8_t index, uint16_t rgb565);
void prg32_band_set_game_info(const char *text);
void prg32_audio_play_track(uint16_t track_id);
void prg32_audio_stop_track(void);
void prg32_audio_note(uint8_t channel, uint8_t instrument, uint8_t note, uint8_t volume, uint32_t duration_ms);
void prg32_audio_note_on(uint8_t channel, uint8_t instrument, uint8_t note, uint8_t volume);
void prg32_audio_note_off(uint8_t channel);
void prg32_multiplayer_init(void);
bool prg32_multiplayer_available(void);
int prg32_multiplayer_join(const char *cartridge_signature, uint32_t flags);
int prg32_multiplayer_leave(void);
void prg32_multiplayer_tick(void);
int prg32_multiplayer_set_local_state(int16_t x, int16_t y, uint16_t sprite, uint16_t flags);
int prg32_multiplayer_set_input(uint32_t input);
int prg32_multiplayer_get_peer_count(void);
int prg32_multiplayer_get_peer(int index, prg32_player_state_t *out);
#endif
