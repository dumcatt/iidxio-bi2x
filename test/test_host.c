/* End-to-end test of the host stack against test/sim_bi2x.py (POSIX only).
   usage: BI2X_PORT=/dev/pts/N test_host <module dir> */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bi2x/bi2x_log.h"
#include "bi2x/bi2x_port.h"
#include "bi2x/bi2x_tdj.h"

int main(int argc, char **argv)
{
    const char *dirs[1];
    struct bi2x_tdj_config cfg;
    struct bi2x_tdj *tdj;
    struct bi2x_tdj_input in;
    uint8_t rgb[3 * 64];
    uint64_t end;
    unsigned int i;
    int fails = 0;
    int polls = 0;

    dirs[0] = argc > 1 ? argv[1] : "";

    bi2x_log_set_level(getenv("BI2X_VERBOSE") ? BI2X_LOG_MISC : BI2X_LOG_INFO);

    memset(&cfg, 0, sizeof(cfg));
    cfg.module_dirs = dirs;
    cfg.module_dir_count = 1;
    cfg.skip_reset = 1;
    cfg.poll_interval_ms = 1;
    cfg.connect_timeout_ms = 15000;

    if (bi2x_tdj_open(&cfg, &tdj) != 0) {
        fprintf(stderr, "FAIL: board did not come online\n");
        if (tdj != NULL) {
            bi2x_tdj_close(tdj);
        }
        return 1;
    }

    bi2x_tdj_set_start_lamp(tdj, 0, 1);
    bi2x_tdj_set_effect_button_lamp(tdj, 1);
    bi2x_tdj_set_player_button_lamp(tdj, 0, 0, 1);
    bi2x_tdj_set_player_button_lamp(tdj, 0, 2, 1);
    bi2x_tdj_set_woofer_led(tdj, 0xFF00FF);
    bi2x_tdj_set_iccr_led(tdj, 0, 0xFF0000);
    bi2x_tdj_set_turntable_led(tdj, 1, 0x0000FF);
    bi2x_tdj_set_turntable_resist(tdj, 0, 0x30);

    for (i = 0; i < bi2x_tdj_tape_led_count(0); i++) {
        rgb[i * 3 + 0] = (uint8_t) (i * 8);
        rgb[i * 3 + 1] = 0;
        rgb[i * 3 + 2] = 0;
    }
    bi2x_tdj_set_tape_led_data(tdj, 0, rgb);

    for (i = 0; i < bi2x_tdj_tape_led_count(1); i++) {
        rgb[i * 3 + 0] = (uint8_t) (i * 8);
        rgb[i * 3 + 1] = 0xFF;
        rgb[i * 3 + 2] = 0;
    }
    bi2x_tdj_set_tape_led_data(tdj, 1, rgb);

    bi2x_tdj_io_reset(tdj, 0x1);
    bi2x_tdj_update(tdj);

    end = bi2x_time_ms() + 3000;

    while (bi2x_time_ms() < end) {
        uint8_t last = in.seq;

        bi2x_tdj_update(tdj);
        bi2x_tdj_get_input(tdj, &in);

        if (in.seq != last) {
            polls++;
        }

        bi2x_sleep_ms(5);
    }

    printf(
        "inputs: start %d/%d effect %d vefx %d keys %02X/%02X tt %02X/%02X "
        "raw[10] %02X online %d polls seen %d\n",
        in.start[0], in.start[1], in.effect, in.vefx, in.keys[0], in.keys[1],
        in.turntable[0], in.turntable[1], in.raw[10],
        bi2x_tdj_is_online(tdj), polls);

    fails += !(in.start[0] == 1 && in.start[1] == 0);
    fails += !(in.effect == 1 && in.vefx == 0);
    fails += !(in.keys[0] == 0x55 && in.keys[1] == 0x02);
    fails += !(in.turntable[1] == 0x80);
    fails += !(in.raw[10] == 10);
    fails += !bi2x_tdj_is_online(tdj);
    fails += polls < 50;

    bi2x_tdj_close(tdj);

    printf(fails ? "HOST FAIL (%d)\n" : "HOST PASS\n", fails);

    return fails ? 1 : 0;
}
