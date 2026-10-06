/*
 * ain_sfx_linux_stub.c — Lightweight Linux ELF SFX Extractor Stub for AIN Archives
 *
 * Standalone Linux self-extractor stub.
 * Reverse-engineered from AINEXT.EXE (Transas Marine Ltd.).
 *
 * pure C reimplementation by seb3773
 * https://github.com/seb3773/AIN
 *
 * Usage when run directly:
 *   ./archive.sfx              extract all files to current dir
 *   ./archive.sfx -o <dir>     extract all files to <dir>
 *   ./archive.sfx -l           list files in archive
 *   ./archive.sfx -t           test archive integrity (CRC16)
 *   ./archive.sfx -h, --help   show usage
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <utime.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <fnmatch.h>

/* Check for path traversal: reject absolute paths, Windows drive letters, and components navigating above root */
static int is_safe_relpath(const char *path)
{
    if (!path || !path[0]) return 0;
    if (path[0] == '/' || path[0] == '\\') return 0;
    if (isalpha((unsigned char)path[0]) && path[1] == ':') return 0;

    int depth = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/' || *p == '\\') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/' && *p != '\\') p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') {
            continue;
        }
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            depth--;
            if (depth < 0) return 0;
        } else {
            depth++;
        }
    }
    return 1;
}

static int match_pattern(const char *pattern, const char *path, const char *filename)
{
    if (!pattern || !pattern[0]) return 1;

    char pat_norm[512];
    size_t plen = strlen(pattern);
    if (plen >= sizeof(pat_norm)) plen = sizeof(pat_norm) - 1;
    for (size_t i = 0; i < plen; i++) {
        char c = pattern[i];
        if (c == '\\') c = '/';
        pat_norm[i] = (char)tolower((unsigned char)c);
    }
    pat_norm[plen] = '\0';

    char path_norm[512];
    size_t path_len = strlen(path);
    if (path_len >= sizeof(path_norm)) path_len = sizeof(path_norm) - 1;
    for (size_t i = 0; i < path_len; i++) {
        char c = path[i];
        if (c == '\\') c = '/';
        path_norm[i] = (char)tolower((unsigned char)c);
    }
    path_norm[path_len] = '\0';

    char fname_norm[512];
    size_t flen = strlen(filename);
    if (flen >= sizeof(fname_norm)) flen = sizeof(fname_norm) - 1;
    for (size_t i = 0; i < flen; i++) {
        char c = filename[i];
        fname_norm[i] = (char)tolower((unsigned char)c);
    }
    fname_norm[flen] = '\0';

    if (fnmatch(pat_norm, path_norm, 0) == 0) return 1;
    if (fnmatch(pat_norm, fname_norm, 0) == 0) return 1;
    if (strcmp(pat_norm, path_norm) == 0) return 1;
    if (strcmp(pat_norm, fname_norm) == 0) return 1;

    return 0;
}

static int match_filters(const char *path, const char *filename, char **patterns, int n_patterns)
{
    if (n_patterns <= 0) return 1;
    for (int i = 0; i < n_patterns; i++) {
        if (match_pattern(patterns[i], path, filename)) return 1;
    }
    return 0;
}

#define N_SYMS_LIT   272u
#define N_SYMS_DIST  254u
#define N_SYMS_PRE    19u
#define WINDOW_SIZE  0x8000u
#define OVERRUN_SIZE 0x0100u
#define BUFFER_LIMIT (WINDOW_SIZE + OVERRUN_SIZE)
#define MAGIC_AIN    0x21u

/* ======================================================================
 * Bitstream — LSB-first, 32-bit buffer, refilled 8 bits at a time
 * ==================================================================== */
typedef struct {
    const uint8_t *data;
    size_t         size;
    size_t         pos;
    uint32_t       buf;
    int            nbits;
    int            exhausted;
} BS;

static void bs_init(BS *bs, const uint8_t *data, size_t size)
{
    bs->data = data; bs->size = size;
    bs->pos  = 0;    bs->buf  = 0; bs->nbits = 0; bs->exhausted = 0;
}

static inline void bs_refill(BS *bs)
{
    if (bs->pos >= bs->size) { bs->exhausted = 1; return; }
    bs->buf   |= (uint32_t)bs->data[bs->pos++] << bs->nbits;
    bs->nbits += 8;
}

static inline uint16_t bs_read(BS *bs, int n)
{
    while (bs->nbits < n && !bs->exhausted) bs_refill(bs);
    if (bs->nbits < n) bs->nbits = n;
    uint16_t r = (uint16_t)(bs->buf & ((1u << n) - 1u));
    bs->buf   >>= n;
    bs->nbits  -= n;
    return r;
}

static inline uint8_t bs_peek8(BS *bs)
{
    while (bs->nbits < 8 && !bs->exhausted) bs_refill(bs);
    return (uint8_t)(bs->buf & 0xFFu);
}

/* ======================================================================
 * Huffman Table
 * ==================================================================== */
typedef struct {
    uint16_t fast[256];
    int16_t  tree[8192];
    int      tree_next;
} HuffTable;

static void sort_symbols_ain(const uint8_t *lengths, int n_sym,
                             uint16_t *sorted, int *n_active_out)
{
    uint16_t buckets[256], chains[272 + 254 + 19 + 4];
    memset(buckets, 0xFF, 256 * sizeof(uint16_t));
    memset(chains,  0xFF, (size_t)(n_sym + 1) * sizeof(uint16_t));
    for (int i = n_sym - 1; i >= 0; i--) {
        uint8_t bl   = (uint8_t)~lengths[i];
        chains[i]    = buckets[bl];
        buckets[bl]  = (uint16_t)i;
    }
    int out = 0;
    for (int bi = 0; bi < 256; bi++) {
        uint16_t idx = buckets[bi];
        while (idx != 0xFFFFu && (int)idx < n_sym) {
            sorted[out++] = idx;
            uint16_t ni  = chains[idx];
            chains[idx]  = 0xFFFFu;
            idx          = ni;
        }
    }
    int n_zero = 0;
    for (int i = 0; i < n_sym; i++) if (!lengths[i]) n_zero++;
    *n_active_out = n_sym - n_zero;
}

static inline uint16_t rol16(uint16_t v, int n)
{
    n &= 15;
    return n ? (uint16_t)((v << n) | (v >> (16 - n))) : v;
}

typedef struct {
    HuffTable       *tbl;
    const uint16_t  *ss;
    int              si;
    int              dx;
    int              count[17];
} FillCtx;

static void fill_table_ain(FillCtx *ctx, int bx, int di,
                           uint16_t ax, int depth)
{
    if (depth > 16) return;
    int d = bx >> 1;
    ctx->count[d]--;
    if (ctx->count[d] < 0) {
        int dx = ctx->dx;
        ctx->dx += 4;
        if (di >> 1 < 8192) ctx->tbl->tree[di >> 1] = (int16_t)dx;
        if (d == 8) {
            int wi = (int)(rol16(ax, 8) & 0xFFu);
            ctx->tbl->fast[wi] = (uint16_t)((unsigned)dx | 0xF800u);
        }
        fill_table_ain(ctx, bx + 2, dx,     (uint16_t)(ax >> 1),            depth + 1);
        fill_table_ain(ctx, bx + 2, dx + 2, (uint16_t)((ax >> 1) | 0x8000u), depth + 1);
        return;
    }
    if (ctx->si < 0) return;
    int sym = (int)ctx->ss[ctx->si--];
    if (di >> 1 < 8192) ctx->tbl->tree[di >> 1] = (int16_t)(-(int16_t)sym);
    if (d > 8) return;
    int      cl    = d + 1;
    uint16_t di_s  = rol16(ax, cl);
    uint16_t entry = (uint16_t)((((uint16_t)sym >> 8) | (uint8_t)(8 * d)) << 8)
                   | (uint8_t)(sym & 0xFF);
    int stride = 1 << cl;
    for (int idx = (int)di_s; idx < 0x200; idx += stride) {
        int wi = idx >> 1;
        if (wi < 256) ctx->tbl->fast[wi] = entry;
    }
}

static void build_huffman_table(HuffTable *tbl,
                                const uint8_t *lengths, int n_syms)
{
    memset(tbl->tree, 0, sizeof(tbl->tree));
    tbl->tree_next = 2;

    int freq[17] = {0};
    for (int i = 0; i < n_syms; i++)
        if (lengths[i] && lengths[i] <= 16) freq[lengths[i]]++;
    int total = 0;
    for (int l = 1; l <= 16; l++) total += freq[l];
    if (total == 0) return;

    static uint16_t ss[272 + 254 + 19 + 4];
    int n_active;
    sort_symbols_ain(lengths, n_syms, ss, &n_active);

    FillCtx ctx;
    ctx.tbl = tbl;
    ctx.ss  = ss;
    ctx.si  = n_active - 1;
    ctx.dx  = 2;
    memcpy(ctx.count, freq, sizeof(ctx.count));
    ctx.count[0] = 0;

    fill_table_ain(&ctx, 0, 0, 0, 0);
}

static int decode_symbol(BS *bs, const HuffTable *tbl)
{
    uint8_t  peek  = bs_peek8(bs);
    uint16_t entry = tbl->fast[peek];

    if (entry == 0xFFFFu) return -1;

    if (entry <= 0x4777u) {
        int clen = (int)(entry >> 11) & 0x1F;
        bs_read(bs, clen);
        return (int)(entry & 0x7FFu);
    }

    bs_read(bs, 8);
    int curr = (int)(entry & 0x7FFu);
    while (curr > 0) {
        int bit = (int)bs_read(bs, 1);
        curr = (int)tbl->tree[(curr >> 1) + bit];
    }
    return (int)(uint16_t)(-(int16_t)curr);
}

static void read_precode(BS *bs, HuffTable *tbl)
{
    uint8_t pre_lens[N_SYMS_PRE] = {0};
    int n_trailing = (int)bs_read(bs, 5);
    if (n_trailing > (int)N_SYMS_PRE) n_trailing = (int)N_SYMS_PRE;
    int to_read = (int)N_SYMS_PRE - n_trailing;

    for (int i = 0; i < to_read; i++) {
        int v = (int)bs_read(bs, 3);
        if (v > 6) {
            while (bs_read(bs, 1)) v++;
        }
        pre_lens[i] = (uint8_t)v;
    }
    build_huffman_table(tbl, pre_lens, (int)N_SYMS_PRE);
}

static void read_code_lengths(BS *bs, const HuffTable *pre_tbl,
                              uint8_t *lengths, int n)
{
    memset(lengths, 0, (size_t)n);
    for (int i = 0; i < n; ) {
        int s = decode_symbol(bs, pre_tbl);
        if (s < 0) break;
        if (s == 0) {
            lengths[i++] = 0;
        } else if (s == 1) {
            int run = (int)bs_read(bs, 4) + 3;
            if (i + run > n) run = n - i;
            i += run;
        } else if (s == 2) {
            int run = (int)bs_read(bs, 9) + 20;
            if (i + run > n) run = n - i;
            i += run;
        } else {
            lengths[i++] = (uint8_t)(s - 2);
        }
    }
}

static void read_block_table(BS *bs, HuffTable *tbl, int n_syms,
                             HuffTable *pre_tbl)
{
    static uint8_t sym_lens[N_SYMS_LIT];

    read_precode(bs, pre_tbl);

    int nm = (int)bs_read(bs, 9);
    if (nm > n_syms) nm = n_syms;
    int active = n_syms - nm;

    memset(sym_lens, 0, (size_t)n_syms);
    if (active > 0)
        read_code_lengths(bs, pre_tbl, sym_lens, active);

    build_huffman_table(tbl, sym_lens, n_syms);
}

/* ======================================================================
 * Decompression Engine
 * ==================================================================== */
typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   cap;
} GrowBuf;

static int gbuf_append(GrowBuf *g, const uint8_t *src, size_t n)
{
    if (g->size + n > g->cap) {
        size_t newcap = g->cap ? g->cap * 2 : (1u << 20);
        while (newcap < g->size + n) newcap *= 2;
        uint8_t *p = realloc(g->data, newcap);
        if (!p) return -1;
        g->data = p; g->cap = newcap;
    }
    memcpy(g->data + g->size, src, n);
    g->size += n;
    return 0;
}

static int decompress_stream(BS *bs, GrowBuf *out)
{
    static uint8_t    window[BUFFER_LIMIT + 256];
    static HuffTable  sym_tbl, dist_tbl, pre_tbl;

    memset(sym_tbl.fast,  0, sizeof(sym_tbl.fast));
    memset(dist_tbl.fast, 0, sizeof(dist_tbl.fast));
    memset(pre_tbl.fast,  0, sizeof(pre_tbl.fast));

    for (;;) {
        if (bs->exhausted && bs->nbits <= 0) return 0;

        memset(window, 0, sizeof(window));
        bs_read(bs, 1);   /* session bit */
        int di = 0;

        for (;;) {
            read_block_table(bs, &sym_tbl,  (int)N_SYMS_LIT,  &pre_tbl);
            read_block_table(bs, &dist_tbl, (int)N_SYMS_DIST, &pre_tbl);

            int block_done = 0;
            while (!block_done) {
                if ((unsigned)di >= BUFFER_LIMIT) {
                    if (gbuf_append(out, window, WINDOW_SIZE) < 0) return -1;
                    int overrun = di - (int)WINDOW_SIZE;
                    if (overrun > 0)
                        memmove(window, window + WINDOW_SIZE, (size_t)overrun);
                    di = overrun;
                }

                if (bs->exhausted && bs->nbits <= 0) goto final_flush;

                int sym = decode_symbol(bs, &sym_tbl);
                if (sym < 0) goto final_flush;

                if (sym < 256) {
                    window[di++] = (uint8_t)sym;
                } else {
                    int      D = sym - 256;
                    uint32_t dist;

                    if (D == 0) {
                        dist = 1;
                    } else if (D == 1) {
                        dist = 2;
                    } else {
                        uint16_t extra = bs_read(bs, D - 1);
                        if (D == 15 && extra == 0x3FFFu) {
                            block_done = 1;
                            break;
                        }
                        dist = ((1u << (D - 1)) | extra) + 1u;
                    }

                    int length_sym = decode_symbol(bs, &dist_tbl);
                    if (length_sym < 0) goto final_flush;
                    int length = length_sym + 3;

                    int src = (int)di - (int)dist;
                    if (src < 0) src += (int)WINDOW_SIZE;
                    for (int k = 0; k < length; k++)
                        window[di + k] = window[src + k];
                    di += length;
                }
            }

            int end_bit = (int)bs_read(bs, 1);
            if (end_bit || (bs->exhausted && bs->nbits <= 0)) {
                if (di > 0 && gbuf_append(out, window, (size_t)di) < 0) return -1;
                break;
            }
        }
        continue;

final_flush:
        if (di > 0 && gbuf_append(out, window, (size_t)di) < 0) return -1;
        return 0;
    }
}

/* ======================================================================
 * Index Parsing & Utility Functions
 * ==================================================================== */
#define IDX_HDR_SIZE 29

typedef struct {
    char     path[512];
    char     filename[260];
    uint32_t timestamp;
    uint32_t orig_size;
    uint32_t comp_size;
    uint16_t checksum;
} FileEntry;

static int parse_index(const uint8_t *idx, size_t idx_len, int n_files,
                       FileEntry *entries)
{
    size_t pos = 0;
    for (int i = 0; i < n_files; i++) {
        if (pos + IDX_HDR_SIZE > idx_len) return -1;
        const uint8_t *hdr = idx + pos;
        entries[i].timestamp = (uint32_t)(hdr[1] | hdr[2]<<8 | hdr[3]<<16 | hdr[4]<<24);
        entries[i].orig_size = (uint32_t)(hdr[5] | hdr[6]<<8 | hdr[7]<<16 | hdr[8]<<24);
        entries[i].comp_size = (uint32_t)(hdr[9] | hdr[10]<<8| hdr[11]<<16| hdr[12]<<24);
        entries[i].checksum  = (uint16_t)(hdr[27]| hdr[28]<<8);

        const uint8_t *fname_start = idx + pos + IDX_HDR_SIZE;
        size_t max_fname = idx_len - (pos + IDX_HDR_SIZE);
        size_t fname_len = strnlen((const char *)fname_start, max_fname);

        char norm[512];
        size_t cp_len = fname_len < sizeof(norm) - 1 ? fname_len : sizeof(norm) - 1;
        for (size_t k = 0; k < cp_len; k++) {
            char c = (char)fname_start[k];
            if (c == '\\') c = '/';
            norm[k] = (char)tolower((unsigned char)c);
        }
        norm[cp_len] = '\0';
        const char *pnorm = norm;
        while (*pnorm == '/') pnorm++;
        snprintf(entries[i].path, sizeof(entries[i].path), "%s", pnorm);
        entries[i].path[sizeof(entries[i].path) - 1] = '\0';

        const char *plast = strrchr(entries[i].path, '/');
        const char *pbase = plast ? plast + 1 : entries[i].path;
        strncpy(entries[i].filename, pbase, sizeof(entries[i].filename) - 1);
        entries[i].filename[sizeof(entries[i].filename) - 1] = '\0';

        pos += IDX_HDR_SIZE + fname_len + 1;
        if (pos < idx_len && idx[pos] == 0) pos++;
    }
    return 0;
}

static uint16_t ain_checksum(const uint8_t *buf, size_t len)
{
    uint16_t s = 0;
    for (size_t i = 0; i < len; i++) s = (uint16_t)(s + buf[i]);
    return s;
}

static time_t dos_to_unix_time(uint32_t dos_dt)
{
    if (!dos_dt) return 0;
    uint16_t date = (uint16_t)(dos_dt >> 16);
    uint16_t time = (uint16_t)(dos_dt & 0xFFFFu);
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = ((date >> 9) & 0x7F) + 80;
    tm.tm_mon  = ((date >> 5) & 0x0F) - 1;
    tm.tm_mday = (date & 0x1F);
    tm.tm_hour = (time >> 11) & 0x1F;
    tm.tm_min  = (time >> 5)  & 0x3F;
    tm.tm_sec  = (time & 0x1F) * 2;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return (t == (time_t)-1) ? 0 : t;
}

static void mkdirs(const char *path)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* ======================================================================
 * Self-Archive Locator via ELF Header Parsing
 * ==================================================================== */
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1]<<8);
}
static inline uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int locate_sfx_archive(const uint8_t *data, size_t file_size, size_t *out_offset)
{
    if (file_size >= 64 && data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F') {
        size_t sfx_offset = 0;
        if (data[4] == 2) { /* 64-bit ELF */
            uint64_t phoff = rd64(data + 0x20);
            uint64_t shoff = rd64(data + 0x28);
            uint16_t phentsize = rd16(data + 0x36);
            uint16_t phnum = rd16(data + 0x38);
            uint16_t shentsize = rd16(data + 0x3A);
            uint16_t shnum = rd16(data + 0x3C);
            sfx_offset = 64;
            for (uint16_t i = 0; i < phnum; i++) {
                size_t po = (size_t)(phoff + (uint64_t)i * phentsize);
                if (po + 40 <= file_size) {
                    uint64_t p_offset = rd64(data + po + 8);
                    uint64_t p_filesz = rd64(data + po + 32);
                    if (p_offset + p_filesz > sfx_offset) sfx_offset = (size_t)(p_offset + p_filesz);
                }
            }
            if (shoff > 0 && shnum > 0) {
                uint64_t shend = shoff + (uint64_t)shentsize * shnum;
                if (shend > sfx_offset) sfx_offset = (size_t)(shend);
            }
        } else if (data[4] == 1) { /* 32-bit ELF */
            uint32_t phoff = rd32(data + 0x1C);
            uint32_t shoff = rd32(data + 0x20);
            uint16_t phentsize = rd16(data + 0x2A);
            uint16_t phnum = rd16(data + 0x2C);
            uint16_t shentsize = rd16(data + 0x2E);
            uint16_t shnum = rd16(data + 0x30);
            sfx_offset = 52;
            for (uint16_t i = 0; i < phnum; i++) {
                size_t po = (size_t)(phoff + (uint32_t)i * phentsize);
                if (po + 20 <= file_size) {
                    uint32_t p_offset = rd32(data + po + 4);
                    uint32_t p_filesz = rd32(data + po + 16);
                    if ((size_t)(p_offset + p_filesz) > sfx_offset) sfx_offset = (size_t)(p_offset + p_filesz);
                }
            }
            if (shoff > 0 && shnum > 0) {
                uint32_t shend = shoff + (uint32_t)shentsize * shnum;
                if ((size_t)shend > sfx_offset) sfx_offset = (size_t)shend;
            }
        }
        if (sfx_offset + 24 <= file_size && data[sfx_offset] == MAGIC_AIN) {
            uint16_t hdr_crc = rd16(data + sfx_offset + 22);
            if (((ain_checksum(data + sfx_offset, 22) ^ 0x5555u) & 0xFFFFu) == hdr_crc) {
                *out_offset = sfx_offset;
                return 0;
            }
        }
        /* Fallback: scan starting from sfx_offset for valid AIN header */
        for (size_t o = sfx_offset; o + 24 <= file_size; o++) {
            if (data[o] == MAGIC_AIN) {
                uint16_t hdr_crc = rd16(data + o + 22);
                if (((ain_checksum(data + o, 22) ^ 0x5555u) & 0xFFFFu) == hdr_crc) {
                    *out_offset = o;
                    return 0;
                }
            }
        }
    }
    return -1;
}

static uint8_t *slurp_file(const char *path, size_t *out_sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf); return NULL;
    }
    fclose(f);
    *out_sz = (size_t)sz;
    return buf;
}

int main(int argc, char *argv[])
{
    const char *outdir = ".";
    int do_list = 0;
    int do_test = 0;

    const char *patterns[64];
    int n_patterns = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("AIN Self-Extracting Archive Version 2.32 (Linux x86_64)\n");
            printf("(Historical Transas Marine Archiver)\n");
            printf("pure C reimplementation by seb3773\n");
            printf("https://github.com/seb3773/AIN\n\n");
            printf("Usage: %s [options] [patterns...]\n\n", argv[0]);
            printf("Options:\n");
            printf("  -o <dir>    Extract files into destination directory\n");
            printf("  -l          List archive contents (optional pattern filter)\n");
            printf("  -t          Test archive integrity (verify CRC16)\n");
            printf("  -h, --help  Show this help\n\n");
            printf("Examples:\n");
            printf("  %s -o dest/ file1.txt \"*.bin\"\n", argv[0]);
            printf("  %s -l \"*.txt\"\n", argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-l") == 0) {
            do_list = 1;
        } else if (strcmp(argv[i], "-t") == 0) {
            do_test = 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            outdir = argv[++i];
        } else if (strncmp(argv[i], "-o", 2) == 0 && argv[i][2] != '\0') {
            outdir = argv[i] + 2;
        } else if (argv[i][0] != '-') {
            if (n_patterns < 64) patterns[n_patterns++] = argv[i];
        }
    }

    /* Open self */
    FILE *f = fopen("/proc/self/exe", "rb");
    if (!f && argc > 0) f = fopen(argv[0], "rb");
    if (!f) {
        fprintf(stderr, "Error: unable to access self executable\n");
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return 1; }

    uint8_t *raw_file = malloc((size_t)sz);
    if (!raw_file || fread(raw_file, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(raw_file); return 1;
    }
    fclose(f);
    size_t file_size = (size_t)sz;

    size_t arc_offset = 0;
    if (locate_sfx_archive(raw_file, file_size, &arc_offset) < 0) {
        fprintf(stderr, "Error: no valid AIN payload found in executable\n");
        free(raw_file); return 1;
    }

    const uint8_t *data = raw_file + arc_offset;
    size_t arc_size = file_size - arc_offset;

    uint8_t  method  = data[1];
    uint16_t n_files = (uint16_t)(data[8] | (data[9] << 8));
    uint32_t arc_ts  = rd32(data + 10);
    uint32_t idx_pos = rd32(data + 14);
    int is_stored = ((method & 0x0Fu) == 4u);
    int is_multi = ((data[3] & 0x40u) != 0);

    GrowBuf idx_buf = {0};
    GrowBuf combined_stream = {0};

    if (!is_multi) {
        /* Single-volume archive */
        if (idx_pos < arc_size) {
            BS bs_idx;
            bs_init(&bs_idx, data + idx_pos, arc_size - (size_t)idx_pos);
            decompress_stream(&bs_idx, &idx_buf);
        }
        size_t stream_size = (idx_pos > 24u && idx_pos <= arc_size) ? (size_t)(idx_pos - 24u) : 0u;
        if (stream_size > 0) {
            gbuf_append(&combined_stream, data + 24u, stream_size);
        }
        free(raw_file);
        raw_file = NULL;
    } else {
        /* Multi-volume fragment archive */
        char exe_path[PATH_MAX];
        memset(exe_path, 0, sizeof(exe_path));
        ssize_t rlen = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
        if (rlen <= 0) {
            if (argc > 0 && argv[0]) {
                if (!realpath(argv[0], exe_path)) {
                    strncpy(exe_path, argv[0], sizeof(exe_path) - 1);
                }
            }
        }
        char base_path[PATH_MAX];
        strncpy(base_path, exe_path, sizeof(base_path) - 1);
        base_path[sizeof(base_path) - 1] = '\0';
        char *slash = strrchr(base_path, '/');
        char *dot = strrchr(base_path, '.');
        if (dot && (!slash || dot > slash)) {
            *dot = '\0';
        }

        /* Volume 0 payload chunk */
        if (arc_size > 24u) {
            gbuf_append(&combined_stream, data + 24u, arc_size - 24u);
        }
        free(raw_file);
        raw_file = NULL;

        /* Chain following fragments: .a01, .a02, ... */
        for (uint16_t vol = 1; ; vol++) {
            char frag_path[PATH_MAX + 32];
            snprintf(frag_path, sizeof(frag_path), "%s.a%02u", base_path, (unsigned)vol);
            if (access(frag_path, F_OK) != 0)
                snprintf(frag_path, sizeof(frag_path), "%s.A%02u", base_path, (unsigned)vol);
            if (access(frag_path, F_OK) != 0)
                snprintf(frag_path, sizeof(frag_path), "%s.%03u", base_path, (unsigned)vol);

            size_t frag_sz = 0;
            uint8_t *frag_data = slurp_file(frag_path, &frag_sz);
            if (!frag_data) {
                fprintf(stderr, "Error: multi-volume fragment %s not found\n", frag_path);
                free(combined_stream.data);
                return 1;
            }
            if (frag_sz < 24u || frag_data[0] != MAGIC_AIN) {
                fprintf(stderr, "Error: fragment %s is corrupt or not an AIN archive\n", frag_path);
                free(frag_data); free(combined_stream.data);
                return 1;
            }
            uint16_t frag_crc = rd16(frag_data + 22);
            if (((ain_checksum(frag_data, 22) ^ 0x5555u) & 0xFFFFu) != frag_crc) {
                fprintf(stderr, "Error: header checksum mismatch in %s\n", frag_path);
                free(frag_data); free(combined_stream.data);
                return 1;
            }
            uint16_t frag_vol = rd16(frag_data + 6);
            if (frag_vol != vol) {
                fprintf(stderr, "Error: volume sequence mismatch in %s (expected %u, got %u)\n",
                        frag_path, (unsigned)vol, (unsigned)frag_vol);
                free(frag_data); free(combined_stream.data);
                return 1;
            }

            uint8_t frag_flags = frag_data[3];
            if (frag_flags & 0x40u) {
                /* Intermediate volume */
                if (frag_sz > 24u) {
                    gbuf_append(&combined_stream, frag_data + 24u, frag_sz - 24u);
                }
                free(frag_data);
            } else {
                /* Final volume: payload slice + compressed index */
                uint32_t final_idx_pos = rd32(frag_data + 14);
                size_t final_slice = (final_idx_pos > 24u && final_idx_pos <= frag_sz)
                                   ? (size_t)(final_idx_pos - 24u) : 0u;
                if (final_slice > 0) {
                    gbuf_append(&combined_stream, frag_data + 24u, final_slice);
                }
                if (final_idx_pos < frag_sz) {
                    BS bs_idx;
                    bs_init(&bs_idx, frag_data + final_idx_pos, frag_sz - (size_t)final_idx_pos);
                    decompress_stream(&bs_idx, &idx_buf);
                }
                free(frag_data);
                break;
            }
        }
    }

    if (!idx_buf.data) {
        fprintf(stderr, "Error: failed to decompress index\n");
        free(combined_stream.data);
        return 1;
    }

    FileEntry *entries = calloc((size_t)n_files, sizeof(FileEntry));
    if (!entries || parse_index(idx_buf.data, idx_buf.size, (int)n_files, entries) < 0) {
        fprintf(stderr, "Error: corrupt index\n");
        free(entries); free(idx_buf.data); free(combined_stream.data);
        return 1;
    }
    free(idx_buf.data);

    /* Check stream CRC */
    uint16_t stream_csum_act = ain_checksum(combined_stream.data, combined_stream.size);
    uint16_t stream_csum_exp = entries[n_files - 1].checksum;
    int stream_crc_ok = (stream_csum_exp == 0 || stream_csum_act == stream_csum_exp);

    GrowBuf stream_out = {0};
    if (is_stored) {
        stream_out = combined_stream;
    } else {
        BS bs;
        bs_init(&bs, combined_stream.data, combined_stream.size);
        decompress_stream(&bs, &stream_out);
        free(combined_stream.data);
    }

    if (do_list) {
        char date_str[32] = "-";
        if (arc_ts) {
            time_t t = dos_to_unix_time(arc_ts);
            if (t) {
                struct tm *tm = localtime(&t);
                strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M:%S", tm);
            }
        }
        const char *mstr = (method == 0x11) ? "M1 (Ultra)" :
                           (method == 0x12) ? "M2 (Normal)" :
                           (method == 0x13) ? "M3 (Fast)" : "M4 (Store)";
        printf("AIN Self-Extracting Archive Version 2.32 (Linux x86_64)\n");
        printf("Method : %s, Files: %u, Created: %s\n\n", mstr, n_files, date_str);
        printf("   Original    Date      Time    CRC16   Name\n");
        printf("  --------- ---------- -------- ------- --------------------\n");
        uint64_t tot = 0;
        int matched = 0;
        for (int i = 0; i < (int)n_files; i++) {
            if (!match_filters(entries[i].path, entries[i].filename, (char **)patterns, n_patterns))
                continue;
            matched++;
            char fdate[16] = "----/--/--", ftime[16] = "--:--:--";
            if (entries[i].timestamp) {
                time_t t = dos_to_unix_time(entries[i].timestamp);
                if (t) {
                    struct tm *tm = localtime(&t);
                    strftime(fdate, sizeof(fdate), "%Y-%m-%d", tm);
                    strftime(ftime, sizeof(ftime), "%H:%M:%S", tm);
                }
            }
            printf("%11u %s %s  0x%04X  %s\n",
                   entries[i].orig_size, fdate, ftime, entries[i].checksum, entries[i].path);
            tot += entries[i].orig_size;
        }
        printf("  ---------                            --------------------\n");
        if (n_patterns > 0) {
            printf("%11lu                            %u file(s) (%u total)\n", (unsigned long)tot, matched, n_files);
        } else {
            printf("%11lu                            %u file(s)\n", (unsigned long)tot, n_files);
        }
        free(entries); free(stream_out.data);
        return 0;
    }

    if (do_test) {
        int errors = 0;
        int matched = 0;
        for (int i = 0; i < (int)n_files; i++) {
            if (!match_filters(entries[i].path, entries[i].filename, (char **)patterns, n_patterns))
                continue;
            matched++;
            if (stream_crc_ok) printf("%-30s OK\n", entries[i].path);
            else { printf("%-30s CRC ERROR\n", entries[i].path); errors++; }
        }
        if (n_patterns > 0 && matched == 0) {
            fprintf(stderr, "Warning: no matching files found\n");
            errors++;
        }
        printf("%d invalid files\n", errors);
        free(entries); free(stream_out.data);
        return errors ? 1 : 0;
    }

    /* Extraction */
    printf("AIN Self-Extracting Archive (Linux x86_64)\n");
    if (outdir && strcmp(outdir, ".") != 0) {
        printf("Target directory: %s\n", outdir);
        mkdirs(outdir);
    }

    size_t offset = 0;
    int errors = 0;
    int n_extracted = 0;
    int n_matched = 0;

    for (int i = 0; i < (int)n_files; i++) {
        uint32_t orig_sz = entries[i].orig_size;
        const char *fname = entries[i].path;

        int selected = match_filters(entries[i].path, entries[i].filename, (char **)patterns, n_patterns);

        if (offset + orig_sz > stream_out.size) {
            if (selected) {
                fprintf(stderr, "  %-30s TRUNCATED\n", fname);
                errors++;
            }
            break;
        }

        if (!selected) {
            offset += orig_sz;
            continue;
        }
        n_matched++;

        /* Path traversal protection */
        if (!is_safe_relpath(fname)) {
            fprintf(stderr, "Security warning: skipping unsafe path traversal '%s'\n", fname);
            errors++;
            offset += orig_sz;
            continue;
        }

        char outpath[600];
        if (outdir && strcmp(outdir, ".") != 0)
            snprintf(outpath, sizeof(outpath), "%s/%s", outdir, fname);
        else
            snprintf(outpath, sizeof(outpath), "%s", fname);

        char tmp[600];
        snprintf(tmp, sizeof(tmp), "%s", outpath);
        char *last_slash = strrchr(tmp, '/');
        if (last_slash) {
            *last_slash = '\0';
            mkdirs(tmp);
        }

        printf("Extracting %s (%u bytes)...\n", fname, orig_sz);
        FILE *outf = fopen(outpath, "wb");
        if (!outf) {
            perror(outpath);
            errors++;
        } else {
            if (orig_sz > 0) fwrite(stream_out.data + offset, 1, orig_sz, outf);
            fclose(outf);

            if (entries[i].timestamp) {
                time_t t = dos_to_unix_time(entries[i].timestamp);
                if (t > 0) {
                    struct utimbuf ut;
                    ut.actime = t;
                    ut.modtime = t;
                    utime(outpath, &ut);
                }
            }
            n_extracted++;
        }
        offset += orig_sz;
    }

    if (n_patterns > 0 && n_matched == 0) {
        fprintf(stderr, "Warning: no matching files found\n");
        errors++;
    }

    if (n_patterns > 0) {
        printf("%d file(s) extracted (%d matched)%s\n", n_extracted, n_matched, errors ? " (with errors)" : " successfully.");
    } else {
        printf("%d file(s) extracted%s\n", n_extracted, errors ? " (with errors)" : " successfully.");
    }
    free(entries); free(stream_out.data);
    return errors ? 1 : 0;
}
