#ifndef RELAY_BOARD_H
#define RELAY_BOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "bi2x/bi2x_port.h"

/*
 * Driver for the sub IO ("relay board", PWB116784480000) of a legacy (LDJ)
 * cabinet. On those cabinets the BIO2 passes the 16 segment ticker,
 * spotlights and neon to this board over RS-232, and reads the five effector
 * faders back from it. Here a PC serial port takes the BIO2's place.
 *
 * Protocol (captured between a BIO2 on BI2A firmware and the board, see
 * LegacyDJ docs/relay-board.md): 115200 8N1, ACIO framing, one request
 * about every 8 ms, no handshake.
 *
 *   request  AA 00 01 12 00 0C <12 data bytes> <checksum>
 *            [0] panel lamps  [1..9] ticker  [10] spotlights  [11] neon
 *            all active low
 *   reply    AA AA 80 01 12 00 0D <13 data bytes> <checksum>
 *            [6..10] faders 1..5, 0x01..0xFF
 *
 * Every byte after the leading AA that is AA or FF goes out as FF followed
 * by its complement; the checksum is the sum of the bytes from the address
 * on.
 */

#define RELAY_BOARD_TICKER_SIZE 9
#define RELAY_BOARD_FADERS 5

struct relay_board;

struct relay_board_config {
    /* e.g. "COM1" */
    const char *port;
    /* ms between requests, 0 = 8 like the BIO2 */
    unsigned int interval_ms;
    /* send the panel lamps to the relay board (unverified on a cabinet) */
    bool panel_lamps;
    /* optional thread API (bemanitools) */
    const struct bi2x_thread_api *threads;
};

/* Starts a background thread that keeps (re)opening the port and talking to
   the board. Returns NULL only if the thread could not be started. */
struct relay_board *relay_board_open(const struct relay_board_config *cfg);
void relay_board_close(struct relay_board *rb);

/* Nine characters, passed on as they are. Shorter strings are padded with
   spaces. */
void relay_board_set_ticker(struct relay_board *rb, const char *text);

/* bit n = spotlight n from the left, as in bemanitools' iidx_io_top_lamp */
void relay_board_set_spotlights(struct relay_board *rb, uint8_t spotlights);
void relay_board_set_neon(struct relay_board *rb, bool on);

/* bit 0 p1 start, 1 p2 start, 2 vefx, 3 effect (bemanitools order) */
void relay_board_set_panel_lamps(struct relay_board *rb, uint8_t lamps);

/* Fader position 0 (bottom) .. 15 (top), or `fallback` if the board has not
   answered yet. */
uint8_t relay_board_get_fader(
    struct relay_board *rb, unsigned int fader, uint8_t fallback);

#endif
