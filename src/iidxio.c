/*
 * Bemanitools iidxio implementation for a BIO2 board running BI2X firmware.
 *
 * Talks to the board directly (see src/bi2x/); libaio*.dll / libacc.dll are
 * not loaded. The only thing taken from the game's DLLs is the BI2X firmware
 * module the board needs uploaded on every start, which is read out of
 * libaio-iob*.dll as data (or from bi2x_tdj.bin / bi2x_sci.bin).
 */

#include <windows.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bemanitools/glue.h"
#include "bemanitools/iidxio.h"

#include "bi2x/bi2x_log.h"
#include "bi2x/bi2x_tdj.h"

#define MODULE "iidxio-bi2x"
#define CONNECT_TIMEOUT_MS 30000

static log_formatter_t log_misc;
static log_formatter_t log_info;
static log_formatter_t log_warning;
static log_formatter_t log_fatal;

static struct bi2x_thread_api threads;
static struct bi2x_tdj *bi2x;
static struct bi2x_tdj_input input;

struct led_config {
    uint32_t woofer;
    uint32_t tt[2];
    uint32_t iccr[2];
    uint32_t pillar;
    uint8_t tt_resist[2];
    int tape_limit_max;
    int tape_limit_avg;
};

static struct led_config leds;

static void log_sink(int level, const char *msg)
{
    log_formatter_t f;

    switch (level) {
        case BI2X_LOG_MISC:
            f = log_misc;
            break;
        case BI2X_LOG_INFO:
            f = log_info;
            break;
        case BI2X_LOG_WARNING:
            f = log_warning;
            break;
        default:
            /* bemanitools' fatal() terminates the process; library errors
               are recoverable, so report them as warnings */
            f = log_warning;
            break;
    }

    if (f != NULL) {
        f(MODULE, "%s", msg);
    }
}

static void ini_path(char *out, size_t cap)
{
    HMODULE self = NULL;
    char *slash;

    /* bio2video.ini next to this DLL, falling back to the working dir */
    if (GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR) (void *) ini_path, &self) &&
        GetModuleFileNameA(self, out, (DWORD) cap) > 0 &&
        (slash = strrchr(out, '\\')) != NULL) {
        snprintf(slash + 1, cap - (size_t) (slash + 1 - out), "bio2video.ini");

        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) {
            return;
        }
    }

    snprintf(out, cap, ".\\bio2video.ini");
}

static uint32_t read_hex(
    const char *section, const char *key, uint32_t def, const char *file)
{
    char buf[32];

    GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), file);

    if (buf[0] == '\0') {
        return def;
    }

    return (uint32_t) strtoul(buf, NULL, 16);
}

static void load_config(char *port, size_t port_cap, struct bi2x_tdj_config *cfg)
{
    char path[MAX_PATH];

    ini_path(path, sizeof(path));

    leds.woofer = read_hex("LED", "Woofer", 0xFFFFFF, path);
    leds.tt[0] = read_hex("LED", "TTP1", 0xFFFFFF, path);
    leds.tt[1] = read_hex("LED", "TTP2", 0xFFFFFF, path);
    leds.iccr[0] = read_hex("LED", "IccrP1", 0xFFFFFF, path);
    leds.iccr[1] = read_hex("LED", "IccrP2", 0xFFFFFF, path);
    leds.pillar = read_hex("LED", "Pillar", 0xFFFFFF, path);
    leds.tape_limit_max = (int) read_hex("LED", "TapeMax", 0xFF, path);
    leds.tape_limit_avg = (int) read_hex("LED", "TapeAverageLimit", 0x55, path);

    leds.tt_resist[0] =
        (uint8_t) GetPrivateProfileIntA("Turntable", "ResistP1", 0, path);
    leds.tt_resist[1] =
        (uint8_t) GetPrivateProfileIntA("Turntable", "ResistP2", 0, path);

    GetPrivateProfileStringA("BI2X", "Port", "", port, (DWORD) port_cap, path);
    cfg->port = port;
    cfg->skip_reset = GetPrivateProfileIntA("BI2X", "SkipReset", 0, path);
    cfg->poll_interval_ms =
        (unsigned int) GetPrivateProfileIntA("BI2X", "PollInterval", 1, path);
}

static void apply_static_leds(void)
{
    uint8_t rgb[3 * 128];
    unsigned int section;
    unsigned int i;

    bi2x_tdj_set_woofer_led(bi2x, leds.woofer);
    bi2x_tdj_set_turntable_led(bi2x, 0, leds.tt[0]);
    bi2x_tdj_set_turntable_led(bi2x, 1, leds.tt[1]);
    bi2x_tdj_set_iccr_led(bi2x, 0, leds.iccr[0]);
    bi2x_tdj_set_iccr_led(bi2x, 1, leds.iccr[1]);
    bi2x_tdj_set_turntable_resist(bi2x, 0, leds.tt_resist[0]);
    bi2x_tdj_set_turntable_resist(bi2x, 1, leds.tt_resist[1]);
    bi2x_tdj_set_tape_led_data_limit(
        bi2x, (uint8_t) leds.tape_limit_max, (uint8_t) leds.tape_limit_avg);

    for (i = 0; i < sizeof(rgb) / 3; i++) {
        rgb[i * 3 + 0] = (uint8_t) (leds.pillar >> 16);
        rgb[i * 3 + 1] = (uint8_t) (leds.pillar >> 8);
        rgb[i * 3 + 2] = (uint8_t) leds.pillar;
    }

    for (section = 0; section < BI2X_TDJ_TAPE_SECTIONS; section++) {
        bi2x_tdj_set_tape_led_data(bi2x, section, rgb);
    }

    bi2x_tdj_update(bi2x);
}

void iidx_io_set_loggers(
    log_formatter_t misc,
    log_formatter_t info,
    log_formatter_t warning,
    log_formatter_t fatal)
{
    log_misc = misc;
    log_info = info;
    log_warning = warning;
    log_fatal = fatal;

    bi2x_log_set_sink(log_sink);
    bi2x_log_set_level(BI2X_LOG_INFO);
}

bool iidx_io_init(
    thread_create_t thread_create,
    thread_join_t thread_join,
    thread_destroy_t thread_destroy)
{
    struct bi2x_tdj_config cfg;
    static char port[256];
    int rc;

    threads.create = (bi2x_thread_create_t) thread_create;
    threads.join = (bi2x_thread_join_t) thread_join;
    threads.destroy = (bi2x_thread_destroy_t) thread_destroy;

    memset(&cfg, 0, sizeof(cfg));
    load_config(port, sizeof(port), &cfg);
    cfg.threads = &threads;
    cfg.connect_timeout_ms = CONNECT_TIMEOUT_MS;

    if (log_info != NULL) {
        log_info(MODULE, "starting (stand-alone BI2X driver)");
    }

    rc = bi2x_tdj_open(&cfg, &bi2x);

    if (rc < 0 || bi2x == NULL) {
        if (log_fatal != NULL) {
            log_fatal(MODULE, "BI2X driver failed to start");
        }
        return false;
    }

    if (rc > 0) {
        if (log_warning != NULL) {
            log_warning(
                MODULE,
                "BI2X not online after %d s, continuing to retry in the "
                "background", CONNECT_TIMEOUT_MS / 1000);
        }
    }

    apply_static_leds();

    return true;
}

void iidx_io_fini(void)
{
    if (bi2x != NULL) {
        bi2x_tdj_close(bi2x);
        bi2x = NULL;
    }
}

void iidx_io_ep1_set_deck_lights(uint16_t deck_lights)
{
    unsigned int player;
    unsigned int button;

    for (player = 0; player < 2; player++) {
        for (button = 0; button < 7; button++) {
            bi2x_tdj_set_player_button_lamp(
                bi2x, player, button,
                (deck_lights >> (player * 7 + button)) & 1);
        }
    }
}

void iidx_io_ep1_set_panel_lights(uint8_t panel_lights)
{
    bi2x_tdj_set_start_lamp(
        bi2x, 0, (panel_lights >> IIDX_IO_PANEL_LIGHT_P1_START) & 1);
    bi2x_tdj_set_start_lamp(
        bi2x, 1, (panel_lights >> IIDX_IO_PANEL_LIGHT_P2_START) & 1);
    bi2x_tdj_set_vefx_button_lamp(
        bi2x, (panel_lights >> IIDX_IO_PANEL_LIGHT_VEFX) & 1);
    bi2x_tdj_set_effect_button_lamp(
        bi2x, (panel_lights >> IIDX_IO_PANEL_LIGHT_EFFECT) & 1);
}

void iidx_io_ep1_set_top_lamps(uint8_t top_lamps)
{
    /* no spot lamps on a TDJ cabinet */
    (void) top_lamps;
}

void iidx_io_ep1_set_top_neons(bool top_neons)
{
    (void) top_neons;
}

bool iidx_io_ep1_send(void)
{
    bi2x_tdj_update(bi2x);
    return true;
}

bool iidx_io_ep2_recv(void)
{
    bi2x_tdj_update(bi2x);
    bi2x_tdj_get_input(bi2x, &input);

    /* keep the game running through a reconnect; inputs just freeze */
    return true;
}

uint8_t iidx_io_ep2_get_turntable(uint8_t player_no)
{
    return player_no < 2 ? input.turntable[player_no] : 0;
}

uint8_t iidx_io_ep2_get_slider(uint8_t slider_no)
{
    /* no sliders on BI2X */
    (void) slider_no;
    return 15;
}

uint8_t iidx_io_ep2_get_sys(void)
{
    return (uint8_t) ((input.test << IIDX_IO_SYS_TEST) |
                      (input.service << IIDX_IO_SYS_SERVICE) |
                      (input.coin << IIDX_IO_SYS_COIN));
}

uint8_t iidx_io_ep2_get_panel(void)
{
    return (uint8_t) ((input.start[0] << IIDX_IO_PANEL_P1_START) |
                      (input.start[1] << IIDX_IO_PANEL_P2_START) |
                      (input.vefx << IIDX_IO_PANEL_VEFX) |
                      (input.effect << IIDX_IO_PANEL_EFFECT));
}

uint16_t iidx_io_ep2_get_keys(void)
{
    uint16_t keys = 0;
    unsigned int i;

    for (i = 0; i < 7; i++) {
        keys |= (uint16_t) (((input.keys[0] >> i) & 1) << (IIDX_IO_KEY_P1_1 + i));
        keys |= (uint16_t) (((input.keys[1] >> i) & 1) << (IIDX_IO_KEY_P2_1 + i));
    }

    return keys;
}

bool iidx_io_ep3_write_16seg(const char *text)
{
    /* no 16 segment display either */
    (void) text;
    return true;
}
