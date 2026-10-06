/*
 * bi2x-modextract: pull the BI2X firmware modules out of libaio-iob.dll and
 * libaio-iob2_video.dll and write them as bi2x_sci.bin / bi2x_tdj.bin.
 *
 * The DLLs are only read as data. Once extracted, iidxio-bi2x.dll and
 * bi2xtest.exe no longer need the DLLs at all.
 *
 * usage: bi2x-modextract [dll directory] [output directory]
 */

#include <stdio.h>
#include <string.h>

#include "bi2x/bi2x_crc.h"
#include "bi2x/bi2x_log.h"
#include "bi2x/bi2x_module.h"

static void join(char *out, size_t cap, const char *dir, const char *name)
{
    size_t n = strlen(dir);

    if (n == 0) {
        snprintf(out, cap, "%s", name);
    } else if (dir[n - 1] == '/' || dir[n - 1] == '\\') {
        snprintf(out, cap, "%s%s", dir, name);
    } else {
        snprintf(out, cap, "%s/%s", dir, name);
    }
}

int main(int argc, char **argv)
{
    const char *src = argc > 1 ? argv[1] : ".";
    const char *dst = argc > 2 ? argv[2] : ".";
    enum bi2x_module_id ids[2] = {BI2X_MODULE_TDJ, BI2X_MODULE_SCI};
    int failed = 0;
    int i;

    bi2x_log_set_level(BI2X_LOG_MISC);

    for (i = 0; i < 2; i++) {
        struct bi2x_module mod;
        char in_path[1024];
        char out_path[1024];
        FILE *f;

        join(in_path, sizeof(in_path), src, bi2x_module_dll_name(ids[i]));
        join(out_path, sizeof(out_path), dst, bi2x_module_bin_name(ids[i]));

        if (bi2x_module_extract_from_dll(ids[i], in_path, &mod) != 0) {
            fprintf(stderr, "failed to extract module from %s\n", in_path);
            failed = 1;
            continue;
        }

        f = fopen(out_path, "wb");

        if (f == NULL || fwrite(mod.data, 1, mod.size, f) != mod.size) {
            fprintf(stderr, "failed to write %s\n", out_path);
            failed = 1;
        } else {
            printf(
                "%s -> %s (%u bytes, crc16 %04X)\n", in_path, out_path,
                mod.size, bi2x_crc16(0xFFFF, mod.data, mod.size));
        }

        if (f != NULL) {
            fclose(f);
        }

        bi2x_module_free(&mod);
    }

    return failed;
}
