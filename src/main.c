/*
 * bi2xtest: bring up a BI2X board without any Konami DLLs loaded and show
 * its inputs. Lamps follow the buttons so every lamp can be checked too.
 *
 * usage: bi2xtest [-p COM5] [-n] [-v] [-m module_dir]
 *   -p PORT   use this COM port instead of searching for USB 1CCF:8050
 *   -n        skip the 8 second line-break reset of the board
 *   -v        verbose protocol logging
 *   -m DIR    where bi2x_tdj.bin/bi2x_sci.bin or libaio-iob*.dll are
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#endif

#include "bi2x/bi2x_log.h"
#include "bi2x/bi2x_port.h"
#include "bi2x/bi2x_tdj.h"

static int key_pressed(void)
{
#ifdef _WIN32
    if (_kbhit()) {
        _getch();
        return 1;
    }
#endif
    return 0;
}

static void usage(void)
{
    fprintf(
        stderr,
        "usage: bi2xtest [-p COM5] [-n] [-v] [-m module_dir]\n"
        "  -p PORT  COM port (default: find USB %04X:%04X)\n"
        "  -n       skip the line-break board reset\n"
        "  -v       verbose logging\n"
        "  -m DIR   directory with bi2x_*.bin or libaio-iob*.dll\n",
        BI2X_USB_VID, BI2X_USB_PID);
}

int main(int argc, char **argv)
{
    struct bi2x_tdj_config cfg;
    struct bi2x_tdj_node_info info;
    struct bi2x_tdj_input in;
    struct bi2x_tdj *tdj = NULL;
    const char *dirs[1];
    uint8_t rgb[3 * 128];
    uint8_t last_seq = 0;
    unsigned int frame = 0;
    int i;
    int rc;

    memset(&cfg, 0, sizeof(cfg));
    cfg.poll_interval_ms = 1;
    cfg.connect_timeout_ms = 60000;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            cfg.port = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0) {
            cfg.skip_reset = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            bi2x_log_set_level(BI2X_LOG_MISC);
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            dirs[0] = argv[++i];
            cfg.module_dirs = dirs;
            cfg.module_dir_count = 1;
        } else {
            usage();
            return 2;
        }
    }

    printf("[*] Bringing up BI2X...\n");

    rc = bi2x_tdj_open(&cfg, &tdj);

    if (rc != 0) {
        printf("[!] BI2X did not come online\n");
        bi2x_tdj_close(tdj);
        return 1;
    }

    if (bi2x_tdj_get_node_info(tdj, &info) == 0) {
        printf(
            "[*] Node %u, type %08X, protocol %u.%u, firmware %02X%02X\n",
            info.node, (unsigned int) info.type, info.firminfo[5],
            info.firminfo[6], info.firminfo[0x1E], info.firminfo[0x1F]);
    }

    /* dim white pillars, coloured RGB bits so they can be checked */
    for (i = 0; i < (int) (sizeof(rgb) / 3); i++) {
        rgb[i * 3 + 0] = 0x40;
        rgb[i * 3 + 1] = 0x40;
        rgb[i * 3 + 2] = 0x40;
    }

    for (i = 0; i < BI2X_TDJ_TAPE_SECTIONS; i++) {
        bi2x_tdj_set_tape_led_data(tdj, (unsigned int) i, rgb);
    }

    bi2x_tdj_set_woofer_led(tdj, 0x400040);
    bi2x_tdj_set_iccr_led(tdj, 0, 0x400000);
    bi2x_tdj_set_iccr_led(tdj, 1, 0x000040);

    printf("[*] Online. Lamps follow buttons. Press any key to quit.\n");

    for (;;) {
        unsigned int b;

        bi2x_tdj_update(tdj);
        bi2x_tdj_get_input(tdj, &in);

        /* lamps mirror the buttons */
        for (b = 0; b < 7; b++) {
            bi2x_tdj_set_player_button_lamp(tdj, 0, b, (in.keys[0] >> b) & 1);
            bi2x_tdj_set_player_button_lamp(tdj, 1, b, (in.keys[1] >> b) & 1);
        }

        bi2x_tdj_set_start_lamp(tdj, 0, in.start[0]);
        bi2x_tdj_set_start_lamp(tdj, 1, in.start[1]);
        bi2x_tdj_set_vefx_button_lamp(tdj, in.vefx);
        bi2x_tdj_set_effect_button_lamp(tdj, in.effect);

        /* turntable LEDs follow the platter position */
        bi2x_tdj_set_turntable_led(
            tdj, 0, (uint32_t) in.turntable[0] << 16 | (255u - in.turntable[0]));
        bi2x_tdj_set_turntable_led(
            tdj, 1, (uint32_t) in.turntable[1] << 8 | (255u - in.turntable[1]));

        if (in.seq != last_seq && (frame++ % 8) == 0) {
            printf(
                "\rseq %3u %s | test %u svc %u coin %u | start %u%u vefx %u "
                "eff %u | tt %3u %3u | 1P %u%u%u%u%u%u%u 2P %u%u%u%u%u%u%u  ",
                in.seq, bi2x_tdj_is_online(tdj) ? "on " : "OFF", in.test,
                in.service, in.coin, in.start[0], in.start[1], in.vefx,
                in.effect, in.turntable[0], in.turntable[1],
                in.keys[0] & 1, (in.keys[0] >> 1) & 1, (in.keys[0] >> 2) & 1,
                (in.keys[0] >> 3) & 1, (in.keys[0] >> 4) & 1,
                (in.keys[0] >> 5) & 1, (in.keys[0] >> 6) & 1, in.keys[1] & 1,
                (in.keys[1] >> 1) & 1, (in.keys[1] >> 2) & 1,
                (in.keys[1] >> 3) & 1, (in.keys[1] >> 4) & 1,
                (in.keys[1] >> 5) & 1, (in.keys[1] >> 6) & 1);
            fflush(stdout);
        }

        last_seq = in.seq;

        if (key_pressed()) {
            break;
        }

        bi2x_sleep_ms(4);
    }

    printf("\n[*] Shutting down\n");
    bi2x_tdj_close(tdj);

    return 0;
}
