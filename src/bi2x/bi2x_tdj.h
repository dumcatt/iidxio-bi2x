#ifndef BI2X_TDJ_H
#define BI2X_TDJ_H

#include <stddef.h>
#include <stdint.h>

#include "bi2x_port.h"

/*
 * Stand-alone driver for a BIO2 board running BI2X firmware in an IIDX (TDJ)
 * cabinet. Replaces libaio.dll + libaio-iob.dll + libaio-iob2_video.dll
 * (AIO_IOB2_BI2X_TDJ and everything underneath it).
 *
 * Usage mirrors the original API: call the setters, then bi2x_tdj_update()
 * (== aioNodeCtl_UpdateDevicesStatus) to commit outputs and latch the most
 * recent inputs, then read them back with bi2x_tdj_get_input() or
 * bi2x_tdj_get_device_status(). A background thread talks to the board.
 */

struct bi2x_tdj;

struct bi2x_tdj_config {
    /* NULL or "" = find USB 1CCF:8050 like libaio does; else e.g. "COM5" */
    const char *port;
    /* where to look for bi2x_tdj.bin / bi2x_sci.bin or the libaio DLLs to
       extract them from (each entry ends with a path separator, "" = cwd).
       NULL = the directory of this module, then the current directory */
    const char *const *module_dirs;
    size_t module_dir_count;
    /* skip the 8 s line-break reset libaio does before talking to the board */
    int skip_reset;
    /* pause between I/O polls; 0 = poll back to back */
    unsigned int poll_interval_ms;
    /* how long bi2x_tdj_open() waits for the board to come online.
       0 = return immediately and let the thread connect in the background */
    unsigned int connect_timeout_ms;
    /* optional thread API (bemanitools) */
    const struct bi2x_thread_api *threads;
};

/* raw layout of AIO_IOB2_BI2X_TDJ::DEVSTATUS, see docs/PROTOCOL.md */
#define BI2X_TDJ_DEVSTATUS_SIZE 0xCA
#define BI2X_TDJ_RAW_STATUS_SIZE 0x4A
#define BI2X_TDJ_TAPE_SECTIONS 17
#define BI2X_TDJ_TAPE_LEDS 707

struct bi2x_tdj_input {
    uint8_t seq; /* increments with every status poll */
    uint8_t test;
    uint8_t service;
    uint8_t coin;
    uint8_t start[2];
    uint8_t vefx;
    uint8_t effect;
    uint8_t turntable[2]; /* absolute position, 0-255 */
    uint8_t keys[2];      /* bit n = key n+1 */
    uint8_t raw[BI2X_TDJ_RAW_STATUS_SIZE];
};

struct bi2x_tdj_node_info {
    uint8_t node;
    uint32_t type;
    uint8_t firminfo[32];
};

int bi2x_tdj_open(const struct bi2x_tdj_config *cfg, struct bi2x_tdj **out);
void bi2x_tdj_close(struct bi2x_tdj *t);

int bi2x_tdj_is_online(struct bi2x_tdj *t);

/* Info about the node we attached to (valid once online). */
int bi2x_tdj_get_node_info(struct bi2x_tdj *t, struct bi2x_tdj_node_info *info);

/* == aioNodeCtl_UpdateDevicesStatus(): commit outputs, latch inputs */
void bi2x_tdj_update(struct bi2x_tdj *t);

void bi2x_tdj_get_input(struct bi2x_tdj *t, struct bi2x_tdj_input *in);
void bi2x_tdj_get_device_status(
    struct bi2x_tdj *t, uint8_t out[BI2X_TDJ_DEVSTATUS_SIZE]);

/* Outputs. Players / sides are 0 (1P) and 1 (2P). Colours are 0xRRGGBB and
   are reduced to RGB555 on the wire. */
void bi2x_tdj_io_reset(struct bi2x_tdj *t, uint32_t flags);
void bi2x_tdj_set_watchdog_timer(struct bi2x_tdj *t, uint8_t value);
void bi2x_tdj_control_coin_blocker(struct bi2x_tdj *t, unsigned int slot, int block);
void bi2x_tdj_add_counter(struct bi2x_tdj *t, unsigned int slot, unsigned int count);
void bi2x_tdj_set_start_lamp(struct bi2x_tdj *t, unsigned int player, int on);
void bi2x_tdj_set_vefx_button_lamp(struct bi2x_tdj *t, int on);
void bi2x_tdj_set_effect_button_lamp(struct bi2x_tdj *t, int on);
void bi2x_tdj_set_player_button_lamp(
    struct bi2x_tdj *t, unsigned int player, unsigned int button, int on);
void bi2x_tdj_set_woofer_led(struct bi2x_tdj *t, uint32_t rgb);
void bi2x_tdj_set_iccr_led(struct bi2x_tdj *t, unsigned int player, uint32_t rgb);
void bi2x_tdj_set_turntable_led(
    struct bi2x_tdj *t, unsigned int player, uint32_t rgb);
void bi2x_tdj_set_turntable_resist(
    struct bi2x_tdj *t, unsigned int player, uint8_t value);

/* Tape (pillar etc.) LEDs: 17 sections, rgb holds 3 bytes (R, G, B) per LED
   of the section. The overall brightness is capped like the original
   library does (defaults: max 0xFF, average limit 0x55). */
unsigned int bi2x_tdj_tape_led_count(unsigned int section);
void bi2x_tdj_set_tape_led_data(
    struct bi2x_tdj *t, unsigned int section, const uint8_t *rgb);
void bi2x_tdj_set_tape_led_data_limit(
    struct bi2x_tdj *t, uint8_t max, uint8_t average);

#endif
