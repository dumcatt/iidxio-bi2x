#ifndef BI2X_MODULE_H
#define BI2X_MODULE_H

#include <stddef.h>
#include <stdint.h>

/*
 * The BI2X firmware runs "modules" that the host uploads every time the node
 * is attached (AIO_NCTL_IOB2::LoadModule). The IIDX (TDJ) I/O module lives in
 * libaio-iob2_video.dll, a second "SCI" module lives in libaio-iob.dll. They
 * are Konami binaries, so they are not shipped with this project: we read
 * them straight out of the DLL files (as data -- the DLLs are never loaded or
 * executed), or from bi2x_tdj.bin / bi2x_sci.bin extracted earlier with
 * bi2x-modextract.
 */

struct bi2x_module {
    uint8_t *data;
    uint32_t size;
};

enum bi2x_module_id {
    BI2X_MODULE_TDJ = 0,
    BI2X_MODULE_SCI = 1,
};

/* Extract a module from the given DLL file (libaio-iob2_video.dll for TDJ,
   libaio-iob.dll for SCI). Returns 0 on success. */
int bi2x_module_extract_from_dll(
    enum bi2x_module_id id, const char *dll_path, struct bi2x_module *out);

/* Load a raw (already decompressed) module file. */
int bi2x_module_load_file(const char *path, struct bi2x_module *out);

/* Search dirs (each ending in a path separator, or "" for the current
   directory) for bi2x_{tdj,sci}.bin, then for the DLL to extract from. */
int bi2x_module_find(
    enum bi2x_module_id id,
    const char *const *dirs,
    size_t ndirs,
    struct bi2x_module *out);

void bi2x_module_free(struct bi2x_module *m);

const char *bi2x_module_bin_name(enum bi2x_module_id id);
const char *bi2x_module_dll_name(enum bi2x_module_id id);

#endif
