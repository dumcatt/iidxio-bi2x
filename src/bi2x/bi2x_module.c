#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bi2x_log.h"
#include "bi2x_lz.h"
#include "bi2x_module.h"

/* ------------------------------------------------------------------------ */
/* Minimal PE32+ reader                                                      */
/* ------------------------------------------------------------------------ */

#define PE_MAX_SECTIONS 64
#define PE_SCN_EXECUTE 0x20000000u

struct pe_section {
    uint32_t va;
    uint32_t vsize;
    uint32_t raw;
    uint32_t raw_size;
    uint32_t chars;
};

struct pe_image {
    const uint8_t *file;
    size_t size;
    uint64_t base;
    int nsec;
    struct pe_section sec[PE_MAX_SECTIONS];
    uint32_t exp_rva;
    uint32_t exp_size;
};

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t) (p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
        ((uint32_t) p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t) rd32(p) | ((uint64_t) rd32(p + 4) << 32);
}

static int pe_parse(struct pe_image *img, const uint8_t *file, size_t size)
{
    uint32_t e;
    uint16_t nsec;
    uint16_t optsz;
    uint32_t opt;
    uint32_t so;
    int i;

    memset(img, 0, sizeof(*img));
    img->file = file;
    img->size = size;

    if (size < 0x40 || file[0] != 'M' || file[1] != 'Z') {
        return -1;
    }

    e = rd32(file + 0x3C);

    if ((size_t) e + 24 > size || memcmp(file + e, "PE\0\0", 4) != 0) {
        return -1;
    }

    nsec = rd16(file + e + 6);
    optsz = rd16(file + e + 20);
    opt = e + 24;

    if ((size_t) opt + optsz > size || optsz < 112 + 8 ||
        rd16(file + opt) != 0x20B /* PE32+ */) {
        return -1;
    }

    img->base = rd64(file + opt + 24);
    img->exp_rva = rd32(file + opt + 112);
    img->exp_size = rd32(file + opt + 116);

    so = opt + optsz;

    if (nsec > PE_MAX_SECTIONS || (size_t) so + (size_t) nsec * 40 > size) {
        return -1;
    }

    img->nsec = nsec;

    for (i = 0; i < nsec; i++) {
        const uint8_t *s = file + so + i * 40;

        img->sec[i].vsize = rd32(s + 8);
        img->sec[i].va = rd32(s + 12);
        img->sec[i].raw_size = rd32(s + 16);
        img->sec[i].raw = rd32(s + 20);
        img->sec[i].chars = rd32(s + 36);
    }

    return 0;
}

static const struct pe_section *pe_section_of(
    const struct pe_image *img, uint32_t rva)
{
    int i;

    for (i = 0; i < img->nsec; i++) {
        const struct pe_section *s = &img->sec[i];

        if (rva >= s->va && rva < s->va + s->raw_size) {
            return s;
        }
    }

    return NULL;
}

/* Pointer to n file-backed bytes at rva, or NULL. */
static const uint8_t *pe_rva_ptr(
    const struct pe_image *img, uint32_t rva, size_t n)
{
    const struct pe_section *s = pe_section_of(img, rva);
    size_t off;

    if (s == NULL || (uint64_t) rva + n > (uint64_t) s->va + s->raw_size) {
        return NULL;
    }

    off = (size_t) s->raw + (rva - s->va);

    if (off + n > img->size) {
        return NULL;
    }

    return img->file + off;
}

static const uint8_t *pe_va_ptr(
    const struct pe_image *img, uint64_t va, size_t n)
{
    if (va < img->base || va - img->base > 0xFFFFFFFFu) {
        return NULL;
    }

    return pe_rva_ptr(img, (uint32_t) (va - img->base), n);
}

static int pe_va_is_exec(const struct pe_image *img, uint64_t va)
{
    const struct pe_section *s;

    if (va < img->base || va - img->base > 0xFFFFFFFFu) {
        return 0;
    }

    s = pe_section_of(img, (uint32_t) (va - img->base));

    return s != NULL && (s->chars & PE_SCN_EXECUTE) != 0;
}

static uint64_t pe_find_export_prefix(
    const struct pe_image *img, const char *prefix)
{
    const uint8_t *dir;
    uint32_t nnames;
    uint32_t funcs;
    uint32_t names;
    uint32_t ords;
    uint32_t i;
    size_t plen = strlen(prefix);

    if (img->exp_rva == 0) {
        return 0;
    }

    dir = pe_rva_ptr(img, img->exp_rva, 40);

    if (dir == NULL) {
        return 0;
    }

    nnames = rd32(dir + 24);
    funcs = rd32(dir + 28);
    names = rd32(dir + 32);
    ords = rd32(dir + 36);

    for (i = 0; i < nnames; i++) {
        const uint8_t *pn = pe_rva_ptr(img, names + i * 4, 4);
        const uint8_t *po = pe_rva_ptr(img, ords + i * 2, 2);
        const uint8_t *name;
        const uint8_t *pf;

        if (pn == NULL || po == NULL) {
            continue;
        }

        name = pe_rva_ptr(img, rd32(pn), plen);

        if (name == NULL || memcmp(name, prefix, plen) != 0) {
            continue;
        }

        pf = pe_rva_ptr(img, funcs + rd16(po) * 4, 4);

        if (pf != NULL) {
            return img->base + rd32(pf);
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------ */
/* MODDATA discovery                                                         */
/* ------------------------------------------------------------------------ */

/* struct AIO_NCTL_IOB2::MODDATA { const uint8_t *blob; size_t blob_len; } */
static int try_moddata(
    const struct pe_image *img, uint64_t va, struct bi2x_module *out)
{
    const uint8_t *md = pe_va_ptr(img, va, 16);
    const uint8_t *blob;
    uint64_t blob_va;
    uint64_t blob_len;
    size_t size;
    uint8_t *buf;

    if (md == NULL) {
        return -1;
    }

    blob_va = rd64(md);
    blob_len = rd64(md + 8);

    if (blob_len < 8 || blob_len > 0x100000) {
        return -1;
    }

    blob = pe_va_ptr(img, blob_va, (size_t) blob_len);

    if (blob == NULL) {
        return -1;
    }

    size = bi2x_module_blob_size(blob, (size_t) blob_len);

    if (size == 0 || size < blob_len / 4) {
        return -1;
    }

    buf = malloc(size);

    if (buf == NULL) {
        return -1;
    }

    if (bi2x_module_blob_decode(blob, (size_t) blob_len, buf, size) != 0) {
        free(buf);
        return -1;
    }

    out->data = buf;
    out->size = (uint32_t) size;

    bi2x_misc(
        "module data at %#llx: %u bytes packed, %u bytes unpacked",
        (unsigned long long) va, (unsigned int) blob_len,
        (unsigned int) size);

    return 0;
}

#define SCAN_FN_MAX 0x1000
#define SCAN_MAX_FUNCS 512

struct scan_ctx {
    const struct pe_image *img;
    uint64_t queue[SCAN_MAX_FUNCS];
    int depth[SCAN_MAX_FUNCS];
    int count;
};

static void scan_push(struct scan_ctx *ctx, uint64_t va, int depth)
{
    int i;

    if (ctx->count >= SCAN_MAX_FUNCS || !pe_va_is_exec(ctx->img, va)) {
        return;
    }

    for (i = 0; i < ctx->count; i++) {
        if (ctx->queue[i] == va) {
            return;
        }
    }

    ctx->queue[ctx->count] = va;
    ctx->depth[ctx->count] = depth;
    ctx->count++;
}

/* Walk functions breadth first starting from the queued roots, looking at
   every RIP-relative LEA for a MODDATA structure and following direct calls
   up to max_depth. Byte-pattern based, candidates are verified by fully
   decoding the module, so stray matches are harmless. */
static int scan_for_moddata(
    struct scan_ctx *ctx, int max_depth, struct bi2x_module *out)
{
    int qi;

    for (qi = 0; qi < ctx->count; qi++) {
        uint64_t fn = ctx->queue[qi];
        const uint8_t *code = NULL;
        size_t len = SCAN_FN_MAX;
        size_t i;

        while (len > 16 && (code = pe_va_ptr(ctx->img, fn, len)) == NULL) {
            len /= 2;
        }

        if (code == NULL) {
            continue;
        }

        for (i = 0; i + 7 <= len; i++) {
            if (code[i] == 0xC3 && i + 1 < len && code[i + 1] == 0xCC) {
                break;
            }

            if ((code[i] == 0x48 || code[i] == 0x4C) && code[i + 1] == 0x8D &&
                (code[i + 2] & 0xC7) == 0x05) {
                int32_t disp = (int32_t) rd32(code + i + 3);
                uint64_t target = fn + i + 7 + (int64_t) disp;

                if (!pe_va_is_exec(ctx->img, target) &&
                    try_moddata(ctx->img, target, out) == 0) {
                    return 0;
                }
            }

            if (code[i] == 0xE8 && ctx->depth[qi] < max_depth) {
                int32_t rel = (int32_t) rd32(code + i + 1);
                scan_push(ctx, fn + i + 5 + (int64_t) rel, ctx->depth[qi] + 1);
            }
        }
    }

    return -1;
}

/* Collect RIP-relative LEA targets inside a function. */
static int lea_targets(
    const struct pe_image *img, uint64_t fn, uint64_t *out, int max)
{
    const uint8_t *code;
    size_t len = 0x400;
    size_t i;
    int n = 0;

    while (len > 16 && (code = pe_va_ptr(img, fn, len)) == NULL) {
        len /= 2;
    }

    if (len <= 16) {
        return 0;
    }

    for (i = 0; i + 7 <= len && n < max; i++) {
        if (code[i] == 0xC3 && i + 1 < len && code[i + 1] == 0xCC) {
            break;
        }

        if ((code[i] == 0x48 || code[i] == 0x4C) && code[i + 1] == 0x8D &&
            (code[i + 2] & 0xC7) == 0x05) {
            int32_t disp = (int32_t) rd32(code + i + 3);
            out[n++] = fn + i + 7 + (int64_t) disp;
        }
    }

    return n;
}

static int extract_tdj(const struct pe_image *img, struct bi2x_module *out)
{
    struct scan_ctx *ctx;
    uint64_t ctor;
    uint64_t leas[64];
    int nleas;
    int i;
    int rc;

    /* AIO_IOB2_BI2X_TDJ::AIO_IOB2_BI2X_TDJ(...) installs the class vtable;
       the vtable's per-cycle method leads to the module-list builder, which
       references the TDJ MODDATA. */
    ctor = pe_find_export_prefix(img, "??0AIO_IOB2_BI2X_TDJ@@");

    if (ctor == 0) {
        bi2x_warn("AIO_IOB2_BI2X_TDJ constructor export not found");
        return -1;
    }

    ctx = calloc(1, sizeof(*ctx));

    if (ctx == NULL) {
        return -1;
    }

    ctx->img = img;
    nleas = lea_targets(img, ctor, leas, 64);

    for (i = 0; i < nleas; i++) {
        int slot;

        if (pe_va_is_exec(img, leas[i])) {
            continue;
        }

        for (slot = 0; slot < 8; slot++) {
            const uint8_t *p = pe_va_ptr(img, leas[i] + slot * 8, 8);

            if (p != NULL) {
                scan_push(ctx, rd64(p), 0);
            }
        }
    }

    rc = scan_for_moddata(ctx, 3, out);
    free(ctx);

    return rc;
}

static int extract_sci(const struct pe_image *img, struct bi2x_module *out)
{
    uint64_t fn;
    uint64_t leas[8];
    int n;
    int i;

    /* static const MODDATA *AIO_IOB2_BI2X::GetModuleDataSci(void)
       { return &moddata; } */
    fn = pe_find_export_prefix(img, "?GetModuleDataSci@AIO_IOB2_BI2X@@");

    if (fn == 0) {
        bi2x_warn("AIO_IOB2_BI2X::GetModuleDataSci export not found");
        return -1;
    }

    n = lea_targets(img, fn, leas, 8);

    for (i = 0; i < n; i++) {
        if (try_moddata(img, leas[i], out) == 0) {
            return 0;
        }
    }

    return -1;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long len;

    if (f == NULL) {
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) <= 0 ||
        len > 64L * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    buf = malloc((size_t) len);

    if (buf == NULL || fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *size = (size_t) len;

    return buf;
}

int bi2x_module_extract_from_dll(
    enum bi2x_module_id id, const char *dll_path, struct bi2x_module *out)
{
    struct pe_image img;
    uint8_t *file;
    size_t size;
    int rc;

    memset(out, 0, sizeof(*out));
    file = read_file(dll_path, &size);

    if (file == NULL) {
        return -1;
    }

    if (pe_parse(&img, file, size) != 0) {
        bi2x_warn("%s: not a 64-bit PE image", dll_path);
        free(file);
        return -1;
    }

    rc = id == BI2X_MODULE_TDJ ? extract_tdj(&img, out) : extract_sci(&img, out);
    free(file);

    if (rc != 0) {
        bi2x_warn("%s: module data not found", dll_path);
    }

    return rc;
}

int bi2x_module_load_file(const char *path, struct bi2x_module *out)
{
    size_t size;

    memset(out, 0, sizeof(*out));
    out->data = read_file(path, &size);

    if (out->data == NULL) {
        return -1;
    }

    out->size = (uint32_t) size;

    return 0;
}

const char *bi2x_module_bin_name(enum bi2x_module_id id)
{
    return id == BI2X_MODULE_TDJ ? "bi2x_tdj.bin" : "bi2x_sci.bin";
}

const char *bi2x_module_dll_name(enum bi2x_module_id id)
{
    return id == BI2X_MODULE_TDJ ? "libaio-iob2_video.dll" : "libaio-iob.dll";
}

int bi2x_module_find(
    enum bi2x_module_id id,
    const char *const *dirs,
    size_t ndirs,
    struct bi2x_module *out)
{
    char path[1024];
    size_t i;

    for (i = 0; i < ndirs; i++) {
        snprintf(path, sizeof(path), "%s%s", dirs[i], bi2x_module_bin_name(id));

        if (bi2x_module_load_file(path, out) == 0) {
            bi2x_info("using module %s (%u bytes)", path, out->size);
            return 0;
        }
    }

    for (i = 0; i < ndirs; i++) {
        snprintf(path, sizeof(path), "%s%s", dirs[i], bi2x_module_dll_name(id));

        if (bi2x_module_extract_from_dll(id, path, out) == 0) {
            bi2x_info(
                "extracted module from %s (%u bytes)", path, out->size);
            return 0;
        }
    }

    return -1;
}

void bi2x_module_free(struct bi2x_module *m)
{
    if (m != NULL) {
        free(m->data);
        m->data = NULL;
        m->size = 0;
    }
}
