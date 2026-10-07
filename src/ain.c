/*
 * ain.c — Native C reimplementation of Transas AIN 2.32 Archiver
 *
 * Full bit-exact compressor and decompressor for Transas AIN archives.
 * Reverse-engineered from AIN_UNP.EXE and AINEXT.EXE (Borland C++ 1991/1996).
 *
 * pure C reimplementation by seb3773
 * https://github.com/seb3773/AIN
 *
 * Supported commands:
 *   a      Add / create archive (-m1..-m4, -r recursive)
 *   x      eXtract files with full directory tree
 *   e      Extract files flat (without directories)
 *   l      List archive contents (filenames, sizes, date/time)
 *   v      Verbosely list archive contents (full paths, ratios, CRC)
 *   t      Test archive integrity and verify checksums
 *   --exe  Extract AIN2-packed SFX executable
 *
 * Build:
 *   gcc -O2 -Wall -Wextra -o ain ain.c
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <utime.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>
#include <limits.h>
#include <fnmatch.h>
#include "ain_sfx_stub.h"
#include "ain_sfx_linux_stub.h"

#define AIN_VERSION_STR    "2.32"
#define MAGIC_AIN          0x21u
#define OVERRUN_SIZE       0x0100u
#define BUFFER_LIMIT       (WINDOW_SIZE + OVERRUN_SIZE)

static void show_banner(void)
{
    printf("AIN Archiver Version 2.32\n");
    printf("(Historical Transas Marine Archiver)\n");
    printf("pure C reimplementation by seb3773\n");
    printf("https://github.com/seb3773/AIN\n\n");
}

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

/* ======================================================================
 * Bitstream Reader (BS) — from ain2unpack.c
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
    if (bs->nbits < n) bs->nbits = n;          /* clamp on exhaustion */
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
 * Growable Buffer (GrowBuf) — from ain2unpack.c
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


/* ======================================================================
 * Compression Engine & Shared Huffman Core — from ain2pack.c
 * ==================================================================== */
static int g_debug = 0;  /* set AIN_DEBUG=1 to enable */
static int g_block_no = 0;

/* ======================================================================
 * Constants — identical to ain2unpack.c
 * ==================================================================== */
#define N_SYMS_LIT    272u
#define N_SYMS_DIST   254u
#define N_SYMS_PRE     19u
#define WINDOW_SIZE  0x8000u
#define MAX_MATCH     256u
#define MIN_MATCH_M3    4u   /* DOS mode 3: min match = 4 (D114: cmp cx,4 / jc literal) */
#define MIN_MATCH_M12   3u   /* DOS modes 1/2: min match = 3 */
#define MIN_MATCH       4u   /* conservative default */

/* DOS block flush threshold (from disasm):
 *
 * There are TWO separate flush mechanisms in the DOS compressor:
 *   1. D5A0 "pack tokens → output buffer": triggered when SP < 0xCBCA (≈195 tokens).
 *      This does NOT end a Huffman block; it merely encodes pending tokens into
 *      the output buffer using the CURRENT Huffman tables. The block stays open.
 *   2. D659 "flush Huffman block": triggered when output buffer ≥ 0x4DF4 bytes.
 *      This ends the current block, writes Huffman tables + all tokens.
 *
 * In our architecture, we always write complete Huffman blocks (one call to
 * write_block per flush). The two-level distinction doesn't apply — we only
 * need to emulate the D659 boundary (output ≥ 0x4DF4 bytes).
 *
 * TOKEN_STACK_LIMIT is a safety cap only — must be large enough that BLOCK_OUTPUT_LIMIT
 * fires first for any file. For small files (output < 14960 bytes) the whole file goes
 * into ONE block regardless of token count, matching DOS behaviour. 32768 tokens at
 * ~1.5 bytes/token = ~49152 bytes >> 14960, so BLOCK_OUTPUT_LIMIT always fires first. */
#define TOKEN_STACK_LIMIT  32768u /* safety cap — BLOCK_OUTPUT_LIMIT fires first */
#define BLOCK_OUTPUT_LIMIT 14960u /* 0x4DF4 bytes: D1AC triggers D659 */
/* DOS M3 stack depth thresholds (D07B/D09A/D12E/D14D in AIN_UNP.EXE):
 *   After literal (2B push): flush when sp < 0xCBCA → stack_used > S0-0xCBCA
 *   After match  (4B push): flush when sp < 0xCBC2 → stack_used > S0-0xCBC2
 * Initial SP set to 0xCD52 at CE15 (hardcoded). After call D02D + call D234 + push:
 *   batch-start SP = 0xCD4E → [0xc84a] = 0xCD4E
 * DOS_STACK_LIT = 0xCD4E - 0xCBCA = 0x0184 = 388
 * DOS_STACK_MAT = 0xCD4E - 0xCBC2 = 0x018C = 396  (= LIT + 8) */
#define DOS_STACK_LIT  388u   /* literal flush: stack_used > 388 → D5A0 */
#define DOS_STACK_MAT  396u   /* match flush:   stack_used > 396 → D5A0 */
/* DOS M1/M2 stack depth threshold (CE7C in AIN_UNP.EXE):
 *   Before each token: flush when sp < 0xCBD2 → stack_used > 0xCD52 - 0xCBD2 = 0x0180 = 384
 * Unlike M3 (which checks after push), M1/M2 checks BEFORE the current token's push.
 * This check fires when accumulated stack bytes from previous tokens exceed 384.
 * Initial SP = 0xCD52 (same as M3). Threshold: 0xCBD2.
 * DOS_STACK_M12 = 0xCD52 - 0xCBD2 = 0x0180 = 384 */
#define DOS_STACK_M12  384u   /* M1/M2 flush: stack_used > 384 → D5A0 (CE7C check) */

/* ======================================================================
 * Huffman table — exact copy from ain2unpack.c
 * (build_huffman_table + fill_table_ain give us the canonical code for
 *  each symbol which we then read back from fast[] for encoding)
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
        uint8_t bl  = (uint8_t)~lengths[i];
        chains[i]   = buckets[bl];
        buckets[bl] = (uint16_t)i;
    }
    int out = 0;
    for (int bi = 0; bi < 256; bi++) {
        uint16_t idx = buckets[bi];
        while (idx != 0xFFFFu && (int)idx < n_sym) {
            sorted[out++] = idx;
            uint16_t ni = chains[idx];
            chains[idx] = 0xFFFFu;
            idx = ni;
        }
    }
    int n_zero = 0;
    for (int i = 0; i < n_sym; i++) if (!lengths[i]) n_zero++;
    *n_active_out = n_sym - n_zero;
}

static uint16_t rol16(uint16_t v, int n)
{
    n &= 15;
    return n ? (uint16_t)((v << n) | (v >> (16 - n))) : v;
}

typedef struct {
    HuffTable      *tbl;
    const uint16_t *ss;
    int             si;
    int             dx;
    int             count[17];
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
        fill_table_ain(ctx, bx+2, dx,   (uint16_t)(ax >> 1),             depth+1);
        fill_table_ain(ctx, bx+2, dx+2, (uint16_t)((ax >> 1) | 0x8000u), depth+1);
        return;
    }
    if (ctx->si < 0) return;
    int sym = (int)ctx->ss[ctx->si--];
    if (di >> 1 < 8192) ctx->tbl->tree[di >> 1] = (int16_t)(-(int16_t)sym);
    if (d > 8) return;
    int      cl   = d + 1;
    uint16_t di_s = rol16(ax, cl);
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
    /* Do NOT clear fast[] — matches DOS BSS behaviour: fast[] is zeroed once
     * per session (at session start), never between blocks.  Only tree[] is
     * cleared per block.  This is critical: extract_code() reads codes from
     * fast[], which must reflect exactly what the decompressor sees. */
    memset(tbl->tree, 0,    sizeof(tbl->tree));
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
    ctx.tbl  = tbl;
    ctx.ss   = ss;
    ctx.si   = n_active - 1;
    ctx.dx   = 2;
    memcpy(ctx.count, freq, sizeof(ctx.count));
    ctx.count[0] = 0;

    fill_table_ain(&ctx, 0, 0, 0, 0);
}

/*
 * Extract the LSB-first code for symbol `sym` from a built HuffTable.
 *
 * From fill_table_ain: for a leaf of depth d (code length = d+1... wait,
 * actually depth d means the symbol has length d+1 only if count was
 * consumed at depth d.  More precisely: the symbol lands at depth d when
 * count[d] was still >= 0 before decrement — so its code length is d+1.
 * But wait: d = bx>>1 starts at 0 at the root and the code length used
 * when writing is clen = entry >> 11.  Let's read it back from fast[].
 *
 * For symbols with code length <= 8 bits, fast[peek] entry encodes:
 *   clen  = entry >> 11        (5 bits)
 *   symbol = entry & 0x7FF
 *   peek  = the 8-bit prefix that matches this code
 *
 * The fast[] index written by fill_table_ain for a depth-d leaf is:
 *   di_s = rol16(ax, cl)  with cl = d+1
 *   wi   = di_s >> 1      (index into fast[], because fast has 256 entries
 *                          but idx runs 0..0x1FF in steps of stride)
 *
 * So: peek byte = di_s >> 1 = (some 8-bit value).
 * The LSB-first code of length `clen` is the low `clen` bits of that byte.
 *
 * For symbols with code length > 8 bits (slow path), we traverse the tree
 * from the slow-path entry in fast[] and record the bit path.
 *
 * The simplest correct approach: scan all 256 fast[] entries for the symbol,
 * read clen and code from the first match.
 */
/* extract_code — read the LSB-first code for symbol `sym` from a built HuffTable.
 *
 * `expected_len` must equal the code length assigned to `sym` by huffman_lengths().
 * It is needed to disambiguate symbol 0: tree[] stores -(int16_t)sym, so sym=0
 * produces tree[...]=0 which is indistinguishable from an uninitialised (memset=0)
 * tree slot.  Using expected_len prevents false early termination on those zeros.
 */
static void extract_code(const HuffTable *tbl, int sym, int expected_len,
                         uint16_t *code_out, int *len_out)
{
    /* Fast path: symbol has code length <= 8 bits — check all fast[] entries */
    for (int peek = 0; peek < 256; peek++) {
        uint16_t e = tbl->fast[peek];
        if (e == 0xFFFFu) continue;
        if (e <= 0x4777u) {
            int s    = (int)(e & 0x7FFu);
            int clen = (int)(e >> 11) & 0x1F;
            if (s == sym && clen == expected_len) {
                *code_out = (uint16_t)(peek & ((1 << clen) - 1));
                *len_out  = clen;
                return;
            }
        }
    }

    /* Slow path: symbol has code length > 8 bits.
     * Walk the tree from every slow-path fast[] entry, collecting bits.
     *
     * sym=0 bug: tree[] stores -(int16_t)sym, so leaf sym=0 writes tree[...]=0.
     * Uninitialised tree[] slots (from memset) are also 0.  We disambiguate by
     * only accepting a c==0 leaf when depth matches expected_len-8 exactly. */
    int extra_depth = expected_len - 8;   /* number of tree levels below fast[] */
    if (extra_depth <= 0) goto fallback;  /* should have been found in fast path */

    for (int peek = 0; peek < 256; peek++) {
        uint16_t e = tbl->fast[peek];
        if (e == 0xFFFFu || e <= 0x4777u) continue;

        int curr = (int)(e & 0x7FFu);

        for (int extra_bits = 0; extra_bits < (1 << extra_depth); extra_bits++) {
            int c     = curr;
            int depth = 0;

            while (depth < extra_depth) {
                int bit = (extra_bits >> depth) & 1;
                c       = (int)tbl->tree[(c >> 1) + bit];
                depth++;
                /* Non-zero symbols: c<0 is an unambiguous leaf at any depth.
                 * sym=0: c==0 is only a valid leaf at exactly expected depth. */
                if (c < 0) break;
                if (c == 0 && depth == extra_depth) break;
                if (c == 0) break;  /* uninitialised slot — wrong path, stop */
            }

            if (depth == extra_depth && c <= 0) {
                int s = (int)(uint16_t)(-(int16_t)c);
                if (s == sym) {
                    uint16_t full_code = (uint16_t)(peek | ((uint16_t)extra_bits << 8));
                    *code_out = (uint16_t)(full_code & ((1 << expected_len) - 1));
                    *len_out  = expected_len;
                    return;
                }
            }
        }
    }

fallback:
    /* Symbol not in table (frequency=0) — should never happen */
    *code_out = 0;
    *len_out  = 1;
}

/* ======================================================================
 * DOS-exact Huffman length assignment — DA48–DAB8 pairing algorithm
 *
 * Mirrors AIN_UNP.EXE 0xD8EA–0xDB88 exactly:
 *
 * Step 1 — Counting-sort (D8EA): 2-pass radix sort by (freq_lo, freq_hi).
 *   Symbols inserted from (n-1) downward → equal-freq ties sorted ascending
 *   by symbol index.  sorted[0] = lowest-freq, sorted[n-1] = highest-freq.
 *
 * Step 2 — DOS tree builder (DA48–DAB8):
 *   Pool of 4-byte nodes: [freq:u16, link:u16] where link=0xFFFF=leaf,
 *   link=byte_offset → internal (left child at link, right at link+4).
 *   di = pool write ptr (bytes), si = Q2 read ptr (bytes).
 *   prev_internal ([0x632]) = pool[si].freq + pool[si+4].freq (Q2 pair sum).
 *
 *   Loop (di < (2n-1)×4):
 *     if q1_front > prev_internal:         // Q2 branch (DA9B)
 *       pool[di] = (prev_internal, si);
 *       si += 8;
 *     else:                                 // Q1 branch (DA6B)
 *       pool[di] = (q1_front, LEAF);
 *       advance Q1;
 *       if (di - si) != 4: di += 4; continue;
 *     // daa6: update prev_internal from new Q2 front
 *     prev_internal = (si < di) ? pool[si]+pool[si+4] : 0xFFFF;
 *     di += 4;
 *
 *   NOTE: this is NOT standard Moffat — Q2 consumes PAIRS (8 bytes) at once,
 *   and the comparison is against the combined pair frequency, not a single
 *   node.  This produces a different (non-optimal) tree from standard Huffman.
 *
 * Step 3 — Tree walker (D996): recursive depth count.
 *   depth_count[d]++ for each leaf at depth d (capped at max_depth).
 *
 * Step 4 — Kraft fixup (DAEE–DB02): while Kraft > 2^15:
 *   find deepest non-empty depth d < max_depth, count[d]--, count[d+1]+=2.
 *
 * Step 5 — Assign lengths (DB2C): outer d=0..15, inner: assign length (d+1)
 *   to depth_count[d] symbols walking sorted[] from end (highest freq) down.
 *
 * max_depth = 15 for sym/dist, 7 for pre-code.
 * ==================================================================== */

/* Counting-sort: sort active symbols by (freq_lo, freq_hi) ascending.
 * Equal-freq ties break by ascending symbol index (lower sym first).
 * sorted[0] = lowest-freq, sorted[n-1] = highest-freq.
 *
 * DOS D8EA: 2-pass bucket sort using low byte then high byte of freq.
 * Symbols inserted from i=(n-1) downward → within same freq, chain order
 * is lowest-sym-first (head) → highest-sym-last (tail). */
static void dos_counting_sort(const uint32_t *freq, const int *syms, int n,
                              int *sorted)
{
    /* Replicates DOS D8EA radix sort tie-breaking:
     * ascending freq, ascending sym index on tie. */
    for (int i = 0; i < n; i++) sorted[i] = syms[i];
    for (int i = 1; i < n; i++) {
        int tmp = sorted[i];
        int j = i - 1;
        while (j >= 0) {
            uint32_t fj = freq[sorted[j]], ft = freq[tmp];
            if (fj > ft || (fj == ft && sorted[j] > tmp)) {
                sorted[j + 1] = sorted[j]; j--;
            } else break;
        }
        sorted[j + 1] = tmp;
    }
}

/* DOS DA48–DAB8 tree builder.
 * sorted[0..n-1]: active symbols, freq ascending.
 * Fills depth_count[0..max_depth] with leaf counts per depth.
 * Pool nodes: 4 bytes each — [freq:u16][link:u16].
 *   link=0xFFFF → leaf; link=byte_offset → internal (children at link, link+4). */
static void dos_build_depth_count(const uint32_t *freq, const int *sorted,
                                  int n, int *depth_count, int max_depth)
{
    if (n <= 0) return;
    if (n == 1) { depth_count[0 < max_depth ? 0 : max_depth]++; return; }

    /* Pool: (2n-1) nodes × 4 bytes each. */
    int pool_nodes = 2 * n - 1;
    uint16_t *pfreq = malloc((size_t)pool_nodes * sizeof(uint16_t));
    uint16_t *plink = malloc((size_t)pool_nodes * sizeof(uint16_t));
    if (!pfreq || !plink) { free(pfreq); free(plink); return; }

    /* di = write ptr (node index), si = Q2 read ptr (node index).
     * In DOS these are byte offsets (×4); we use node indices here and
     * scale the di-si==4 check to di-si==1 node. */
    int di = 0, si = 0;
    int q1r = 0;                          /* Q1 read index into sorted[] */
    uint32_t prev_internal = 0xFFFF;      /* [0x632] = Q2 pair combined freq */
    uint32_t q1_front = (uint32_t)freq[sorted[0]];  /* [0x634] */

    while (di < pool_nodes) {
        if (q1_front > prev_internal) {
            /* Q2 branch (DA9B): consume pair (si, si+1) as children */
            pfreq[di] = (uint16_t)prev_internal;
            plink[di] = (uint16_t)si;   /* left child node index */
            si += 2;                     /* consume 2 nodes from Q2 */
            /* fall to daa6 */
        } else {
            /* Q1 branch (DA6B): place leaf at pool[di] */
            pfreq[di] = (uint16_t)q1_front;
            plink[di] = 0xFFFF;          /* leaf */
            q1r++;
            q1_front = (q1r < n) ? (uint32_t)freq[sorted[q1r]] : 0xFFFFu;
            /* DA8D–DA94: if (di - si) == 1 node (= 4 bytes in DOS), pair complete */
            if ((di - si) != 1) {
                di++;
                continue;
            }
            /* pair complete (di == si+1): fall to daa6 */
        }
        /* daa6: update prev_internal from new Q2 front pair (DAA9: cmp si,di) */
        prev_internal = (si < di)
            ? (uint32_t)pfreq[si] + (uint32_t)pfreq[si + 1]
            : 0xFFFFu;
        di++;
    }

    /* Walk tree from root (pool[pool_nodes-1]) counting leaf depths.
     * Mirrors D996: left child at link, right child at link+1.
     * Iterative DFS to avoid stack overflow for deep trees. */
    memset(depth_count, 0, (size_t)(max_depth + 1) * sizeof(int));
    /* Stack: (node_index, depth) pairs */
    int  stk_cap = pool_nodes * 2 + 4;
    int *stk_node  = malloc((size_t)stk_cap * sizeof(int));
    int *stk_depth = malloc((size_t)stk_cap * sizeof(int));
    int top = 0;
    /* D996 walker starts with si=0xFFFE (-2), so root is at "depth -1".
     * Children of root get si=0 (depth 0), depth_count[0]++, code_len = 0+1 = 1.
     * Leaves at depth k from root: depth_count[k-1]++, code_len = k.
     * We mirror this by starting at depth=-1. */
    stk_node[top] = pool_nodes - 1; stk_depth[top] = -1; top++;

    while (top > 0) {
        top--;
        int nd = stk_node[top], d = stk_depth[top];
        if (plink[nd] == 0xFFFF) {
            /* leaf */
            depth_count[d > max_depth ? max_depth : d]++;
        } else {
            int lc = (int)plink[nd];     /* left child node index */
            /* push right then left so left is processed first (matches D996) */
            stk_node[top] = lc + 1; stk_depth[top] = d + 1; top++;
            stk_node[top] = lc;     stk_depth[top] = d + 1; top++;
        }
    }
    free(stk_node);
    free(stk_depth);
    free(pfreq);
    free(plink);
}

static void huffman_lengths(const uint32_t *freq, int n_syms,
                            uint8_t *lengths, int max_depth)
{
    memset(lengths, 0, (size_t)n_syms);

    /* Collect active symbols */
    int syms[544], n = 0;
    for (int i = 0; i < n_syms; i++)
        if (freq[i]) syms[n++] = i;

    if (n == 0) return;
    if (n == 1) { lengths[syms[0]] = 1; return; }

    /* Step 1: Counting-sort — produces sorted[0..n-1] freq ascending,
     * ties broken by descending symbol index. */
    int sorted[544];
    dos_counting_sort(freq, syms, n, sorted);

    /* Step 2+3: DOS DA48-DAB8 tree builder → depth_count[0..max_depth].
     * depth d = tree depth from root (root=0); leaf at depth d → code length d. */
    int depth_count[17] = {0};   /* max_depth ≤ 16 */
    dos_build_depth_count(freq, sorted, n, depth_count, max_depth);

    /* Step 4: Kraft fixup (DAEE–DB02).
     * DOS computes Kraft in 16-bit: dx = sum(depth_count[d] × 2^(15-d)) mod 2^16.
     * A valid full Huffman tree has real Kraft = 2^16 → wraps to 0 in 16-bit → no fixup.
     * If dx != 0 (over-full tree due to depth capping): split dx times from depth 14→15.
     * Note: max_depth must be ≤ 15; for precode (max_depth=7) the same formula applies
     * and 2^(15-d) is used (not 2^(7-d)), but the tree depth is naturally ≤ 7 so
     * wrapping to 0 still occurs for a valid precode tree. */
    {
        uint16_t kraft = 0;
        for (int d = 0; d <= 15; d++)
            kraft += (uint16_t)((uint16_t)(depth_count[d]) * (uint16_t)(1u << (15 - d)));
        /* loop kraft times: each iteration splits one leaf at depth 14→15 */
        while (kraft-- != 0) {
            int d = max_depth - 1;
            while (d > 0 && depth_count[d] == 0) d--;
            if (d < 1) break;
            depth_count[d]--;
            depth_count[d + 1] += 2;
        }
    }

    /* Step 5: Assign lengths (DB2C–DB88).
     * depth_count[d] symbols get code length d+1 (DOS: cx = dx+1).
     * sorted[] walked from n-1 downward → highest-freq get shortest codes. */
    int si = n - 1;
    for (int d = 0; d <= max_depth && si >= 0; d++) {
        int cnt = depth_count[d];
        for (int c = 0; c < cnt && si >= 0; c++, si--)
            lengths[sorted[si]] = (uint8_t)(d + 1);
    }
}

/* ======================================================================
 * Bitstream writer — LSB-first, exact mirror of bs_read() in ain2unpack.c
 * ==================================================================== */
typedef struct {
    uint8_t *data;
    size_t   cap;
    size_t   pos;
    uint32_t buf;
    int      nbits;
} BW;

static void bw_init(BW *bw)
{
    bw->cap   = 65536;
    bw->data  = malloc(bw->cap);
    bw->pos   = 0;
    bw->buf   = 0;
    bw->nbits = 0;
}

static void bw_ensure(BW *bw, size_t extra)
{
    if (bw->pos + extra + 4 > bw->cap) {
        bw->cap = (bw->cap + extra + 4) * 2;
        bw->data = realloc(bw->data, bw->cap);
    }
}

static void bw_write(BW *bw, uint32_t val, int n)
{
    if (n == 0) return;
    bw->buf   |= (val & ((1u << n) - 1u)) << bw->nbits;
    bw->nbits += n;
    while (bw->nbits >= 8) {
        bw_ensure(bw, 1);
        bw->data[bw->pos++] = (uint8_t)(bw->buf & 0xFFu);
        bw->buf   >>= 8;
        bw->nbits  -= 8;
    }
}

static void bw_flush(BW *bw)
{
    if (bw->nbits > 0) {
        bw_ensure(bw, 1);
        bw->data[bw->pos++] = (uint8_t)(bw->buf & 0xFFu);
        bw->buf = 0; bw->nbits = 0;
    }
}

/* ======================================================================
 * Pre-code writer — mirrors read_precode() exactly
 *
 * Encoding:
 *   5 bits : n_trailing  (number of trailing zero lengths, 0..19)
 *   for each active length value pre_lens[i]:
 *     if value <= 6 : write 3-bit value
 *     if value == 7 : write 3-bit 7, then (value-7) one-bits, then 0-bit
 *     (max pre-code length is 7 since we cap at 7 in huffman_lengths)
 * ==================================================================== */
static void write_precode(BW *bw, const uint8_t *pre_lens)
{
    int n_trailing = 0;
    for (int i = (int)N_SYMS_PRE - 1; i >= 0; i--) {
        if (pre_lens[i] == 0) n_trailing++;
        else break;
    }
    bw_write(bw, (uint32_t)n_trailing, 5);
    int to_write = (int)N_SYMS_PRE - n_trailing;
    for (int i = 0; i < to_write; i++) {
        int v = pre_lens[i];
        if (v <= 6) {
            bw_write(bw, (uint32_t)v, 3);
        } else {
            bw_write(bw, 7u, 3);
            for (int k = 0; k < v - 7; k++) bw_write(bw, 1u, 1);
            bw_write(bw, 0u, 1);
        }
    }
}

/* ======================================================================
 * Block table writer — mirrors read_block_table() exactly
 *
 * Format:
 *   write_precode(pre_table)
 *   9 bits : nm  (number of trailing zero-length symbols)
 *   for each active length via RLE using the pre-code:
 *     sym 0         : literal zero length (1 symbol)
 *     sym 1 + 4bits : run of (bits+3) zeros (3..18)
 *     sym 2 + 9bits : run of (bits+20) zeros (20..531)
 *     sym 3..18     : code length 1..16
 * ==================================================================== */

/* RLE token for the code-length sequence */
typedef struct { uint8_t sym; uint16_t extra; uint8_t ebits; } RLESym;

static int encode_lengths_rle(const uint8_t *lengths, int active,
                              RLESym *out, uint32_t *pre_freq)
{
    int n = 0, i = 0;
    while (i < active) {
        if (lengths[i] == 0) {
            /* Count run of zeros, but cap at 531 (sym=2 max) */
            int run = 1;
            while (i+run < active && lengths[i+run] == 0 && run < 531) run++;
            /* Emit run: sym=2 (20..531), sym=1 (3..18), or sym=0 (1..2).
             * Note: run=19 is NOT directly representable — sym=1 max is 18,
             * sym=2 min is 20.  Emit 18+1 or 18+sym=0 for a run of 19. */
            while (run > 0) {
                if (run >= 20) {
                    /* Clamp to 531 (sym=2 field is 9 bits: 0..511, run=20+extra) */
                    int this_run = (run > 531) ? 531 : run;
                    out[n].sym = 2; out[n].extra = (uint16_t)(this_run-20); out[n].ebits = 9;
                    pre_freq[2]++; n++; i += this_run; run -= this_run;
                } else if (run >= 3 && run <= 18) {
                    out[n].sym = 1; out[n].extra = (uint16_t)(run-3); out[n].ebits = 4;
                    pre_freq[1]++; n++; i += run; run = 0;
                } else {
                    /* run == 1, 2, or 19 (gap between sym=1 and sym=2 ranges) */
                    out[n].sym = 0; out[n].extra = 0; out[n].ebits = 0;
                    pre_freq[0]++; n++; i++; run--;
                }
            }
        } else {
            uint8_t sym = (uint8_t)(lengths[i] + 2);
            out[n].sym = sym; out[n].extra = 0; out[n].ebits = 0;
            pre_freq[sym]++; n++; i++;
        }
    }
    return n;
}

/* Session-level Huffman tables — fast[] zeroed once per session, never between
 * blocks (mirrors DOS BSS/decompress_stream behaviour exactly). */
static HuffTable g_pre_tbl;
static HuffTable g_sym_tbl;
static HuffTable g_dist_tbl;

static void session_tables_reset(void)
{
    memset(g_pre_tbl.fast,  0, sizeof(g_pre_tbl.fast));
    memset(g_sym_tbl.fast,  0, sizeof(g_sym_tbl.fast));
    memset(g_dist_tbl.fast, 0, sizeof(g_dist_tbl.fast));
}

static void write_block_table(BW *bw, const uint8_t *lengths, int n_syms)
{
    /* Strip trailing zeros to get active count */
    int active = n_syms;
    while (active > 0 && lengths[active-1] == 0) active--;
    int nm = n_syms - active;

    /* Build RLE sequence and count pre-code symbol frequencies */
    uint32_t pre_freq[N_SYMS_PRE];
    memset(pre_freq, 0, sizeof(pre_freq));
    RLESym *rle = malloc((size_t)(active + 4) * sizeof(RLESym));
    int     nrle = encode_lengths_rle(lengths, active, rle, pre_freq);

    /* Ensure at least one active pre-code symbol to avoid empty table */
    int any = 0;
    for (int i = 0; i < (int)N_SYMS_PRE; i++) if (pre_freq[i]) { any=1; break; }
    if (!any) pre_freq[0] = 1;

    /* Compute pre-code lengths (max 7 bits) */
    uint8_t pre_lens[N_SYMS_PRE];
    huffman_lengths(pre_freq, (int)N_SYMS_PRE, pre_lens, 7);

    /* Build pre-code decode table (same algorithm as decompressor) */
    build_huffman_table(&g_pre_tbl, pre_lens, (int)N_SYMS_PRE);

    /* Write pre-code into stream */
    write_precode(bw, pre_lens);

    /* Write nm (9 bits) */
    bw_write(bw, (uint32_t)nm, 9);

    /* Write each RLE symbol using the pre-code */
    for (int k = 0; k < nrle; k++) {
        uint16_t code; int clen;
        extract_code(&g_pre_tbl, rle[k].sym, pre_lens[rle[k].sym], &code, &clen);
        bw_write(bw, code, clen);
        if (rle[k].ebits)
            bw_write(bw, rle[k].extra, rle[k].ebits);
    }

    free(rle);
}

/* ======================================================================
 * LZ77 — DOS-exact mode 3 hash chain
 *
 * RE from AIN_UNP.EXE disassembly 0xD0A0–0xD19F (mode 3 path).
 *
 * Memory layout (mirrored here):
 *   window[0..0x8000)  = DS window (bp indexes into this)
 *   bst[0..0x4000)     = 4096 slots × 4 bytes each (ES in DOS)
 *     slot[h].head     = most recently inserted position for hash h
 *     slot[h].next     = previous head (one step back)
 *     sentinel         = 0x8000 (signed-negative, flags end-of-chain)
 *
 * Hash (D0A0–D0AF):
 *   word1 = window[bp]   (16-bit LE)
 *   word2 = window[bp+2] (16-bit LE)
 *   bx = ror16(word2, 1)
 *   bx -= word1           (with sbb bl,bh for the borrow byte)
 *   bx *= 4               → byte offset into bst[]
 *   slot index h = bx/4 ∈ [0..4095]
 *
 * Walk: exactly 2 candidates — head (old_head) then next (old_next).
 *
 * Match extension (D0CF–D0F5, mirrored in dos_m3_extend):
 *   After the mandatory 4-byte match, extend word-by-word.
 *   On word mismatch: if low byte matched (al==0 after sub), count +1.
 *   Max length = 256 bytes.
 *
 * Token encoding (D119–D127):
 *   token_len  = match_length - 3   (pushed as cx after add cx,0xFD)
 *   token_dist = bp - match_pos - 1 (0-based: 0 = distance 1)
 *
 * ==================================================================== */

/* The DOS hash bx = (ror16(w2,1) - w1 sbb bl,bh) * 4 is a full 16-bit
 * value used as a byte offset directly into a 64KB ES segment.
 * bx & 3 == 0 always (mul *4), so there are 65536/4 = 16384 possible slots.
 * Table size: 16384 slots × 2 words = 65536 bytes = full 64KB segment. */
#define BST_SLOTS  16384u
#define BST_NIL    0x8000u   /* sentinel: signed-negative in 16-bit */

typedef struct {
    uint16_t head[BST_SLOTS];  /* most recent position per hash slot */
    uint16_t next[BST_SLOTS];  /* second position per hash slot */
} BstTable;

/* DOS mode 3 hash: ror16(word2,1) - word1 (sbb bl,bh) * 4
 * Returns slot index h = bx/4 in [0..16383].
 * bx is a 16-bit byte offset into the 64KB ES segment; bx&3 == 0 always. */
static uint16_t lz_hash_m3(const uint8_t *p)
{
    uint16_t word1 = (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
    uint16_t word2 = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));
    /* ror16(word2, 1) */
    uint16_t ror_w2 = (uint16_t)((word2 >> 1) | (word2 << 15));
    /* sub bx, word1 */
    uint16_t bx = (uint16_t)(ror_w2 - word1);
    /* sbb bl, bh: bl -= bh + CF; CF=1 iff ror_w2 < word1 (unsigned borrow) */
    uint8_t bl = (uint8_t)(bx & 0xFF);
    uint8_t bh = (uint8_t)(bx >> 8);
    uint8_t cf = (ror_w2 < word1) ? 1u : 0u;
    bl = (uint8_t)(bl - bh - cf);
    bx = (uint16_t)(((uint16_t)bh << 8) | bl);
    /* add bx,bx twice → bx *= 4 (16-bit wraparound, bx&3 == 0) */
    bx = (uint16_t)(bx * 4u);
    /* slot index = byte offset / 4; bx is already 16-bit so [0..16383] */
    return (uint16_t)(bx >> 2);
}

/* Extend match using physical (absolute) indices into src[].
 * phys_bp:   physical index of current position in src[].
 * phys_cand: physical index of match candidate (= phys_bp - dist_back).
 * Mirrors the DOS loop D0D4–D0F0 exactly:
 *   - compare word-by-word (2 bytes at a time)
 *   - on mismatch: sub al,1 / adc di,0  (count +1 if low byte matched)
 *   - stop when di >= max_di (= MIN(avail, 256))
 * Returns total match length (including the 4 initial bytes). */
static int dos_m3_extend_phys(const uint8_t *src, size_t src_size,
                               uint32_t phys_bp, uint32_t phys_cand, int avail)
{
    int di     = 4;              /* 4 bytes already matched */
    int max_di = (avail < (int)MAX_MATCH) ? avail : (int)MAX_MATCH;

    while (di < max_di) {
        uint32_t bi = phys_bp   + (unsigned)di;
        uint32_t ci = phys_cand + (unsigned)di;
        uint8_t  b0 = (bi   < src_size) ? src[bi]   : 0u;
        uint8_t  b1 = (bi+1 < src_size) ? src[bi+1] : 0u;
        uint8_t  c0 = (ci   < src_size) ? src[ci]   : 0u;
        uint8_t  c1 = (ci+1 < src_size) ? src[ci+1] : 0u;
        uint16_t wbp   = (uint16_t)(b0 | ((uint16_t)b1 << 8));
        uint16_t wcand = (uint16_t)(c0 | ((uint16_t)c1 << 8));
        if (wbp != wcand) {
            /* sub al,1 / adc di,0: +1 if low byte matched */
            if ((uint8_t)(wbp & 0xFF) == (uint8_t)(wcand & 0xFF)) di++;
            break;
        }
        di += 2;
    }
    if (di > max_di) di = max_di;
    return di;
}

/* ======================================================================
 * LZ state: mirrors the DOS compressor DS/ES segment model.
 *
 * DOS model (M3, AIN_UNP.EXE D02D–D230):
 *   bp    = absolute 16-bit position counter, NEVER decremented.
 *           Starts at 0, grows monotonically. Wraps at 0x10000 for large files.
 *   BST   = 64KB ES segment storing 16-bit bp values.
 *   Slide = triggered when bp reaches [0xC408] (starts at 0x8000, then 0x0000, 0x8000...).
 *           On slide: BST sweep only (no bp change, no window reload).
 *           [0xC408] += 0x8000 (16-bit, alternates 0x8000↔0x0000).
 *
 * For our in-memory implementation:
 *   src[] holds the full input. Access data as src[lap_base + bp].
 *   lap_base = 65536 × (number of full bp wraps), advanced when bp wraps to 0.
 *   All BST entries are raw 16-bit bp values (same lap only; cross-lap entries
 *   are invalidated by the signed check (int16_t)(bp - cand) < 0).
 * ==================================================================== */

typedef struct {
    const uint8_t *src;         /* original input data */
    size_t         src_size;
    uint32_t       lap_base;    /* 65536 × wrap count: 0, 65536, 131072, ... */
    BstTable       bst;
    uint16_t       slide_sentinel; /* [0xC408]: 0x8000 initially, +=0x8000 each slide */
    int            slide_count;    /* number of slides done so far (for debug) */
} LZState;

static void lz_init(LZState *lz, const uint8_t *data, size_t size)
{
    lz->src            = data;
    lz->src_size       = size;
    lz->lap_base       = 0;
    lz->slide_sentinel = BST_NIL;  /* [0xC408] = 0x8000 at D037 */
    lz->slide_count    = 0;
    /* Init BST: all slots = sentinel 0x8000 (mirrors D035: rep stosw with ax=0x8000) */
    for (int i = 0; i < (int)BST_SLOTS; i++) {
        lz->bst.head[i] = BST_NIL;
        lz->bst.next[i] = BST_NIL;
    }
}

/* Slide: triggered when (int16_t)(bp - slide_sentinel) >= 0.
 *
 * DOS D1F8:
 *   ax = [C408] + 0x8000   (16-bit, wraps: 0x8000+0x8000=0x0000 on odd slides)
 *   [C408] = ax
 *   For every word in BST: if (int16_t)word < (int16_t)ax → word = ax.
 *
 * Result:
 *   Odd  slides (1,3,...): ax=0x0000 → entries with bit15=1 (≥0x8000) set to 0.
 *   Even slides (2,4,...): ax=0x8000 → entries < 0x8000 signed (i.e. <0 signed,
 *                          i.e. ≥0x8000 unsigned) set to 0x8000 — restores NIL.
 *
 * bp is NOT modified here (DOS bp stays absolute).
 * lap_base is advanced on even slides (when bp wraps past 0xFFFF to 0x0000). */
static void lz_slide(LZState *lz, uint16_t bp)
{
    lz->slide_count++;

    /* [0xC408] += 0x8000 (16-bit, wraps) */
    lz->slide_sentinel = (uint16_t)(lz->slide_sentinel + 0x8000u);
    uint16_t ax = lz->slide_sentinel;

    if (g_debug) {
        uint16_t h0_head = lz->bst.head[0], h0_next = lz->bst.next[0];
        fprintf(stderr, "  SLIDE #%d: bp=%u sentinel=%04x ax=%04x h0={%04x,%04x}",
                lz->slide_count, (unsigned)bp, ax, ax, h0_head, h0_next);
    }

    /* BST sweep: if (int16_t)w < (int16_t)ax → w = ax */
    uint16_t *words = (uint16_t *)&lz->bst;
    int nwords = (int)(sizeof(lz->bst) / sizeof(uint16_t)); /* 2*BST_SLOTS = 32768 */
    for (int i = 0; i < nwords; i++) {
        if ((int16_t)words[i] < (int16_t)ax)
            words[i] = ax;
    }

    /* On even slides (ax=0x8000): bp just wrapped past 0xFFFF → advance lap_base.
     * Even slide: old sentinel was 0x0000, new = 0x8000 (ax=0x8000). */
    if (ax == (uint16_t)BST_NIL) {
        lz->lap_base += 0x10000u;
    }

    if (g_debug) {
        uint16_t h0_head = lz->bst.head[0], h0_next = lz->bst.next[0];
        fprintf(stderr, " → lap_base=%u h0={%04x,%04x}\n",
                lz->lap_base, h0_head, h0_next);
    }
}

/* DOS mode 3: find best match at absolute position bp, insert bp into BST.
 * bp is a 16-bit absolute position counter (never decremented).
 * lz->lap_base is the base input offset for the current lap (0, 65536, ...).
 * Returns match length (>= MIN_MATCH_M3=4) or 0 if no match.
 * *match_dist_out = token_dist = (uint16_t)(bp - match_pos) - 1 (0-based).
 *
 * Physical position of a candidate: phys_cand = phys_bp - dist_back.
 * (NOT lap_base + cand — that fails for cross-lap candidates from previous laps.) */
static int lz_find_and_insert(LZState *lz, uint16_t bp, int *match_dist_out)
{
    /* Physical input position for current bp */
    uint32_t phys_bp = lz->lap_base + bp;
    size_t avail = lz->src_size > phys_bp ? lz->src_size - phys_bp : 0;

    if (avail < 4) return 0;

    /* Hash: read from src[] directly at lap_base + bp */
    uint16_t h = lz_hash_m3(lz->src + phys_bp);

    /* Insert: slot.head = bp, slot.next = old_head (D0B1–D0B8) */
    uint16_t old_head = lz->bst.head[h];
    uint16_t old_next = lz->bst.next[h];
    lz->bst.head[h] = bp;
    lz->bst.next[h] = old_head;

    /* D0BC: cmp bp, old_head (signed 16-bit); js → literal.
     * Catches BST_NIL=0x8000 (negative signed) and stale future positions. */
    if ((int16_t)((uint16_t)(bp - old_head)) < 0) return 0;

    const uint8_t *src    = lz->src;
    size_t         src_sz = lz->src_size;
    int best_len = 0;
    uint16_t best_pos = 0;

    /* Test old_head.
     * Physical position of candidate: phys_cand = phys_bp - dist_back.
     * This is correct even for cross-lap candidates (dist_back is always a
     * positive 16-bit value in [1..WINDOW_SIZE]). */
    {
        uint16_t cand = old_head;
        int dist_back = (int)(uint16_t)(bp - cand);   /* always > 0 after D0BC check */
        if (dist_back > 0 && dist_back <= (int)WINDOW_SIZE) {
            uint32_t pc = phys_bp - (unsigned)dist_back;
            if (phys_bp >= (unsigned)dist_back &&
                pc+3 < src_sz &&
                src[pc]==src[phys_bp] && src[pc+1]==src[phys_bp+1] &&
                src[pc+2]==src[phys_bp+2] && src[pc+3]==src[phys_bp+3])
            {
                int len = dos_m3_extend_phys(src, src_sz, phys_bp, pc, (int)avail);
                if (len > best_len) { best_len = len; best_pos = cand; }
            }
        }
    }

    /* D0F7/D0FB/D105 — test old_next.
     *
     * Two distinct paths in DOS code:
     *
     * Path A (D0F5→D0F7→D0FB→D103→D15E): old_head DID match 4 bytes.
     *   - D0F7: cmp bp,old_next (signed); js D10E → skip old_next
     *   - D0FB: lookahead check — src[phys_bp+best_len-1] must match
     *   - Then jump to D15E to test old_next's 4-gram
     *
     * Path B (D105→D109→D15E): old_head did NOT match 4 bytes.
     *   - D105: bx=old_next; cmp bp,bx (signed); jns D15E; jmp D088
     *   - No D0FB check — test old_next directly
     */
    if ((int16_t)((uint16_t)(bp - old_next)) >= 0) {
        uint16_t cand = old_next;
        int dist_back = (int)(uint16_t)(bp - cand);
        if (dist_back > 0 && dist_back <= (int)WINDOW_SIZE) {
            uint32_t pc = phys_bp - (unsigned)dist_back;
            if (phys_bp >= (unsigned)dist_back) {
                int do_test = 1;
                if (best_len > 0) {
                    /* Path A: D0FB lookahead check required */
                    uint32_t bp_chk   = phys_bp + (unsigned)(best_len - 1);
                    uint32_t cand_chk = pc      + (unsigned)(best_len - 1);
                    uint8_t  bb = (bp_chk   < src_sz) ? src[bp_chk]   : 0u;
                    uint8_t  cb = (cand_chk < src_sz) ? src[cand_chk] : 0u;
                    if (bb != cb) do_test = 0;
                }
                /* Path B: no D0FB check — do_test stays 1 */
                if (do_test &&
                    pc+3 < src_sz &&
                    src[pc]==src[phys_bp] && src[pc+1]==src[phys_bp+1] &&
                    src[pc+2]==src[phys_bp+2] && src[pc+3]==src[phys_bp+3])
                {
                    int len = dos_m3_extend_phys(src, src_sz, phys_bp, pc, (int)avail);
                    if (len > best_len) { best_len = len; best_pos = cand; }
                }
            }
        }
    }

    if (best_len < (int)MIN_MATCH_M3) return 0;

    /* token_dist = (uint16_t)(bp - best_pos) - 1  (0-based, 16-bit arithmetic) */
    *match_dist_out = (int)(uint16_t)(bp - best_pos) - 1;
    return best_len;
}

/* ======================================================================
 * M1/M2 LZ state — mirrors AIN_UNP.EXE CE34–CF35 + D2C0–D44C
 *
 * DOS buffer model (M1/M2):
 *   bp         = buffer position counter, starts at 0x100.
 *                buf[bp - 0x100] = current input byte (buf[0]=file[0]).
 *   di=[C406]  = window_base (0 initially, advances during compaction).
 *   [C402]     = total bytes in buffer.
 *   [C408]     = slide_sentinel: initially 0x8100, sweeps hash on slide.
 *   [C834]     = max_chain countdown (method-dependent).
 *   [C836]     = current search best length.
 *   [C838]     = current search best candidate bp.
 *   [C83A]     = PREVIOUS iteration's [C836] (saved at CEA9).
 *   [C83C]     = PREVIOUS iteration's [C838] (saved at CEA9).
 *   [C400]     = rolling hash slot index bx.
 *
 * CRITICAL: This is a DEFERRED OUTPUT scheme (lazy matching):
 *   - At each position bp, we compute the new best match and store in [C836]/[C838].
 *   - The OUTPUT decision uses the PREVIOUS position's best ([C83A]/[C83C]).
 *   - If prev_best_len >= 3 AND new_best_len <= prev_best_len: emit previous match.
 *   - Otherwise: emit literal buf[bp-0x100], advance bp by 1.
 *   - This means the output position lags bp by one step.
 *
 * Hash table (DS offset 0x4800): slot_index = ((prev_bx<<3)+buf[bp+2]) & 0x1FFF.
 *   hash[slot] ← bp (xchg), old value → chain entry.
 *
 * Chain array (ES 64KB): chain[bp] = previous bp with same hash context.
 *   ES:[bp*2] = ax (stored after insert).
 *
 * Match search: D318 path uses chain[(bp-0xFF)] as starting candidate,
 *   compares buf[cand] against buf[bp-0xFF], extends via repe cmpsw.
 *   Match covers buf[cand..] matching buf[bp-0xFF..], length >= 3.
 *   token_dist = bp - 0x101 - cand   (where cand is the best bp position).
 *   token_dist = (bp - 0x100) - 1 - (cand - 0x100 + 0x100 - 0x100) ... simplifies to
 *              = (current_file_pos) - 1 - (cand - 0x100) = dist - 1 in std LZ terms
 *              PROVIDED the match encoding position = bp-0xFF (not bp-0x100).
 *
 * ==================================================================== */

#define M12_HASH_SLOTS   0x1000u  /* 4096 hash slots (CD20: 0x1000 words of 0x8100) */
#define M12_CHAIN_SIZE   65536u   /* chain[0..65535], ES:[bp*2] — full 16-bit indexed */
#define M12_NIL          0x8100u  /* initial sentinel (signed-negative, 0x8100) */

/* max_chain per method — from AIN_UNP.EXE binary at CS:method*2:
 *   method 1: 0x2E10, method 2: 0x1689  */
static const uint16_t M12_MAX_CHAIN[5] = { 0x0200u, 0x0200u, 0x0020u, 0x0005u, 0x0000u };

typedef struct {
    const uint8_t *src;
    size_t         src_size;
    uint16_t       hash[M12_HASH_SLOTS]; /* hash table: slot_byte_offset/2 → bp */
    uint16_t       chain[M12_CHAIN_SIZE];/* chain: bp → prev bp with same context */
    uint16_t       bx_slot;   /* [C400]: slot BYTE-OFFSET (= slot_index * 2) */
    uint16_t       slide_sentinel; /* [C408] */
    uint16_t       cur_len;   /* [C836]: best match length found this iteration */
    uint16_t       cur_pos;   /* [C838]: best match bp found this iteration */
    uint16_t       prv_len;   /* [C83A]: best match length from previous iteration */
    uint16_t       prv_pos;   /* [C83C]: best match bp from previous iteration */
    int            method;
} LZ12State;

/* DOS raw-address read: DS:[addr] where file is loaded at DS:0x100.
 * So DS:[addr] = file[addr - 0x100] = src[addr - 0x100].
 * bp starts at 0x200 (= file_pos=0 → DS:[0x200-0x100]=file[0]).
 * DS:[bp+2] for hash: file[(bp+2)-0x100] = file[bp-0xFE].
 * DS:[bx]   for match comparison: file[bx-0x100].
 * DS:[bp-0x100] for literal emit: file[bp-0x200] = file[file_pos]. */
static inline uint8_t lz12_raw(const LZ12State *lz, uint16_t addr)
{
    uint32_t phys = (uint32_t)((uint16_t)(addr - 0x100u));
    return (phys < lz->src_size) ? lz->src[phys] : 0u;
}

/* Literal byte at current position: DS:[bp-0x100] = file[bp-0x200] (CEC6: mov bl,[bp+di-0x100]) */
static inline uint8_t lz12_lit(const LZ12State *lz, uint16_t bp)
{
    uint32_t phys = (uint32_t)((uint16_t)(bp - 0x200u));
    return (phys < lz->src_size) ? lz->src[phys] : 0u;
}

static void lz12_init(LZ12State *lz, const uint8_t *data, size_t size, int method);
static void lz12_insert(LZ12State *lz, uint16_t bp);

static void lz12_init(LZ12State *lz, const uint8_t *data, size_t size, int method)
{
    lz->src            = data;
    lz->src_size       = size;
    lz->method         = method;
    lz->bx_slot        = 0;  /* [C400]=0: initial slot byte-offset */
    lz->slide_sentinel = M12_NIL;
    lz->cur_len = lz->cur_pos = 0;
    lz->prv_len = lz->prv_pos = 0;
    /* CD1D: hash table filled with 0x8100 (M12_NIL), 0x1000 entries */
    for (unsigned i = 0; i < M12_HASH_SLOTS; i++)
        lz->hash[i] = (uint16_t)M12_NIL;
    /* CD25: chain[0..0x7F] = 0 (0x80 words zeroed at ES:0x0000).
     * Rest of chain (0x80..0x7FFF) uninitialized = 0.
     * CE5A: chain[0xFF] = 0x80FF explicitly. */
    for (unsigned i = 0; i < M12_CHAIN_SIZE; i++)
        lz->chain[i] = 0;
    lz->chain[0x00FFu] = 0x80FFu;

    /* CE61–CF19: pre-scan loop.
     * CE3A: bp=0x100. CE61: bp-=3 → bp=0xFD. CE57: dx=0x103.
     * Loop (CEEB–CF19): dec dx (0x103→0x102 first, ≠0 so no jz); inc bp (0xFD→0xFE);
     *   insert bp. Repeats 0x102 times until dec dx→0x000 (jz CF1B = D318, no insert).
     * Result: 0x102 inserts for bp=0x00FE..0x01FF.
     * File is at DS:0x100, so DS:[bp+2] = file[(bp+2)-0x100] = file[bp-0xFE].
     * Byte2 sequence: file[0], file[1], ..., file[0x101]. */
    uint16_t bp = 0x00FEu;
    for (int iter = 0; iter < 0x102; iter++, bp = (uint16_t)(bp + 1u))
        lz12_insert(lz, bp);
}

/* M1/M2 slide (CF51–CF74): triggered when (sentinel - bp) <= 0x100 (unsigned).
 * CF51: ax = bp - 0x8000; [C408] = ax
 * CF58-CF74: sweep hash[0..0x1FFF]: if (int16_t)h < (int16_t)ax → h = ax
 * (Only the hash table is swept, not the chain.) */
static void lz12_slide(LZ12State *lz, uint16_t bp)
{
    uint16_t ax = (uint16_t)(bp - 0x8000u);
    lz->slide_sentinel = ax;
    for (unsigned i = 0; i < M12_HASH_SLOTS; i++) {
        if ((int16_t)((uint16_t)(lz->hash[i] - ax)) < 0)
            lz->hash[i] = ax;
    }
}

/* Hash-insert only (no match search) — used to fast-forward bp during pre-scan.
 * Mirrors CE8B–CEA6: insert bp into hash, update chain. */
static void lz12_insert(LZ12State *lz, uint16_t bp)
{
    /* CE82–CE90: DOS hash computation.
     * [C400] stores the slot BYTE-OFFSET (= slot_index * 2).
     * bx = [C400];  bx <<= 3;  bl += buf[bp+2];  bh &= 0x0F;  bx *= 2.
     * The shl-3 on the byte-offset gives an effective *16 multiplier on the slot index.
     * byte2 = DS:[bp+2] (raw, wraps as uint16_t) = file[bp+2] if in range. */
    uint8_t  byte2 = lz12_raw(lz, (uint16_t)(bp + 2u));
    uint16_t bx    = lz->bx_slot;                   /* doubled slot (byte offset) */
    bx = (uint16_t)(bx << 3);                       /* shl bx, 3 */
    uint8_t  bl    = (uint8_t)bx + byte2;           /* add bl,[bp+di+2]  — 8-bit, no carry */
    uint8_t  bh    = (uint8_t)(bx >> 8) & 0x0Fu;   /* and bh, ch (ch=0x0F) */
    bx = ((uint16_t)bh << 8) | bl;                  /* recombine */
    bx = (uint16_t)(bx + bx);                       /* add bx,bx → make byte-offset */
    /* bx is now the new slot byte-offset; slot_index = bx / 2 */
    uint16_t slot_idx = bx >> 1;                    /* 0..0xFFF (4096 entries) */
    uint16_t ax    = lz->hash[slot_idx];
    lz->hash[slot_idx] = bp;
    lz->bx_slot    = bx;                             /* [C400] = new byte-offset */
    /* CEA4–CEA6: chain[bp] = old_head (or NIL if old_head is "newer" than bp) */
    if ((int16_t)((uint16_t)(bp - ax)) < 0)
        ax = (uint16_t)(bp - 0x8000u);
    lz->chain[bp & (M12_CHAIN_SIZE - 1u)] = ax;
}

/* DOS M1/M2 match search — D318 path (normal) or D420 path (D2C0 offset walk).
 *
 * D318 (normal, si=0):
 *   bx = chain[bp - 0xFF]; ax = src[pos_emit]
 *   loop: cmp src[bx+si], ax; if match → extend from [bx+0..255] vs [pos_emit..+255]
 *
 * D420 (D2C0 path, si=cx, cx is a negative 16-bit offset):
 *   bx = best chain_entry from D2C0 scan
 *   loop: cmp src[bx+si], ax; if match → extend from [bx+si+2..] vs [pos_emit+2..]
 *   match_pos = bx + si; length = 1 + bytes_from_extension - CF_adjust
 *
 * pos_emit = pos + 1 (64-bit). actual_dist = (uint16_t)(pos_raw - bx) [16-bit].
 * phys_cand = pos_emit - actual_dist (64-bit candidate physical position).
 */

/* D360 / D468: extend a match and update cur_len/cur_pos if better.
 *
 * For D360 (d420=false, si=0): compare src[phys_cand..] vs src[pos_emit..]
 *   phys_cand = physical address of chain candidate start.
 *   length = bytes matched from offset 0.
 *
 * For D468 (d420=true, si=cx): compare src[phys_cand+2..] vs src[pos_emit+2..]
 *   byte 0 was confirmed at D423; byte 1 is skipped (D2C0 invariant).
 *   length = 1 + bytes matched from offset 2 (mirrors DOS cx formula).
 *   match_pos = bx (= original chain_entry, adjusted by cx via phys_cand).
 *
 * Uses word-by-word comparison matching the DOS repe cmpsw, max 256 bytes.
 */
static void lz12_extend(LZ12State *lz,
                         size_t phys_cand,   /* physical candidate start */
                         size_t pos_emit,    /* physical pos_emit */
                         uint16_t bx_pos,   /* raw DOS addr stored as cur_pos */
                         int *best,
                         int d420)           /* 0=D360 (from offset 0), 1=D468 (from offset 2) */
{
    size_t sz   = lz->src_size;
    int    from = d420 ? 2 : 0;             /* byte offset to start comparing */
    int    max_words = d420 ? 0x7F : 0x80;  /* D360: 128 words, D468: 127 words */

    int avail_c = (int)(sz - phys_cand);
    int avail_p = (int)(sz - pos_emit);
    int avail   = (avail_c < avail_p) ? avail_c : avail_p;
    if (avail > 256) avail = 256;

    if (d420 && avail < 1) return; /* need at least byte 0 (already confirmed) */

    /* D420 path: DOS skips byte 1 (structural invariant in archive output).
     * For our compressor to produce correct round-trips, we must verify byte 1 too.
     * If byte 1 doesn't match, the match length stays at 1 (below MIN_MATCH=3),
     * so the match is rejected. We achieve this by starting extension at byte 1
     * instead of byte 2, giving from=1, max_words=0x7F+1=128. */
    if (d420) {
        /* Verify byte 1 (offset 1 from match start = pc+1 vs pos_emit+1) */
        if (avail < 2) return;
        if (lz->src[phys_cand + 1] != lz->src[pos_emit + 1]) return; /* byte 1 mismatch → no match */
        /* byte 1 matched: proceed with extension from byte 2 (same as DOS) */
    }

    /* Extension: compare word by word from 'from', up to max_words pairs. */
    int len = from; /* start of extension */
    int max_ext = from + max_words * 2;
    if (max_ext > avail) max_ext = avail;

    /* Word-by-word comparison (mirrors repe cmpsw). */
    while (len + 1 < max_ext) {
        if (lz->src[phys_cand + len]     != lz->src[pos_emit + len])     break;
        if (lz->src[phys_cand + len + 1] != lz->src[pos_emit + len + 1]) { len++; break; }
        len += 2;
    }
    /* Single-byte check at end (mirrors the cl/sbb byte-adjust in D373). */
    if (len < max_ext && lz->src[phys_cand + len] == lz->src[pos_emit + len])
        len++;

    /* For d420: len = stopping position from offset 0 (starts at 2, counts from there).
     * DOS formula (D487): cx = 1 + 2N - CF = total match length including byte 0.
     * When extension starts at offset 2 and N pairs match: len = 2 + 2N (or 2+2N+1 via CF).
     * That gives len = 2N+2 or 2N+3; DOS cx = 2N+2 or 2N+3 → match_len = len. ✓
     * For d360: len = total bytes from offset 0. match_len = len. ✓ */
    int match_len = len;  /* same formula for both d420 and d360 */

    if (match_len >= 3 && match_len > *best) {
        *best = match_len;
        lz->cur_len = (uint16_t)match_len;
        lz->cur_pos = bx_pos;
    }
}

/* D420 chain walk: compare at offset si=cx_d2c0 (signed negative), extend from +2.
 * Includes D4A8–D4E0 secondary chain lookup after each match update.
 *
 * Register mapping (DOS → C):
 *   bx  → bx       chain entry (16-bit bp value, word-index into chain[])
 *   si  → si       comparison offset (signed; cx_d2c0 initially, may change)
 *   ax  → ax_byte  comparison byte = src[pos_emit], reloaded after each branch
 *   dx  → dx       chain depth counter
 *   bp  → bp       current position, never modified
 *   di  → 0        window_base; di=0 in our model (phys = bp + di - 0x100, di cancels)
 *
 * Memory: chain[n] = lz->chain[n & (M12_CHAIN_SIZE-1)].
 * D4BD uses si_work = 2*(ax_saved + cx_DOS) where cx_DOS = cx_match - 1,
 *   then shr→sub→add restores si to ax_saved = cx_d2c0. So D4D6 always resets si.
 */
static void lz12_d420(LZ12State *lz, uint16_t bp, size_t pos_emit,
                      uint16_t bx0, int dx, uint16_t ax_word, int16_t cx_d2c0)
{
    size_t   sz      = lz->src_size;
    uint16_t pos_raw = (uint16_t)(bp - 0x00FFu);
    int      best    = (int)lz->cur_len;
    uint16_t bx      = bx0;
    int16_t  si      = cx_d2c0;

d420_loop:
    for (;;) {
        /* Hop 1: 0000D420 dec dx; jl 0xd44c */
        dx--;
        if (dx < 0) return;

        {
            uint16_t dist = (uint16_t)(pos_raw - bx);
            if (dist > 0u && dist <= 0x7FFFu) {
                ptrdiff_t cand_s = (ptrdiff_t)(pos_emit - dist) + (ptrdiff_t)si;
                if (cand_s >= 0 && (size_t)cand_s + 1 < sz && pos_emit + 1 < sz) {
                    uint16_t cw = (uint16_t)lz->src[(size_t)cand_s] | ((uint16_t)lz->src[(size_t)cand_s + 1] << 8);
                    if (cw == ax_word) goto d468;
                } else if (cand_s >= 0 && (size_t)cand_s < sz && lz->src[(size_t)cand_s] == (uint8_t)ax_word) {
                    goto d468;
                }
            }
        }

        /* Hop 2: 0000D427 bx = chain[bx]; cmp bp, bx; js 0xd44c */
        bx = lz->chain[bx & (M12_CHAIN_SIZE - 1u)];
        if ((int16_t)((uint16_t)(bp - bx)) < 0) return;
        dx--;

        {
            uint16_t dist = (uint16_t)(pos_raw - bx);
            if (dist > 0u && dist <= 0x7FFFu) {
                ptrdiff_t cand_s = (ptrdiff_t)(pos_emit - dist) + (ptrdiff_t)si;
                if (cand_s >= 0 && (size_t)cand_s + 1 < sz && pos_emit + 1 < sz) {
                    uint16_t cw = (uint16_t)lz->src[(size_t)cand_s] | ((uint16_t)lz->src[(size_t)cand_s + 1] << 8);
                    if (cw == ax_word) goto d468;
                } else if (cand_s >= 0 && (size_t)cand_s < sz && lz->src[(size_t)cand_s] == (uint8_t)ax_word) {
                    goto d468;
                }
            }
        }

        /* Hop 3: 0000D435 bx = chain[bx]; cmp bp, bx; js 0xd44c */
        bx = lz->chain[bx & (M12_CHAIN_SIZE - 1u)];
        if ((int16_t)((uint16_t)(bp - bx)) < 0) return;
        dx--;

        {
            uint16_t dist = (uint16_t)(pos_raw - bx);
            if (dist > 0u && dist <= 0x7FFFu) {
                ptrdiff_t cand_s = (ptrdiff_t)(pos_emit - dist) + (ptrdiff_t)si;
                if (cand_s >= 0 && (size_t)cand_s + 1 < sz && pos_emit + 1 < sz) {
                    uint16_t cw = (uint16_t)lz->src[(size_t)cand_s] | ((uint16_t)lz->src[(size_t)cand_s + 1] << 8);
                    if (cw == ax_word) goto d468;
                } else if (cand_s >= 0 && (size_t)cand_s < sz && lz->src[(size_t)cand_s] == (uint8_t)ax_word) {
                    goto d468;
                }
            }
        }

        /* End of 3-hop block: 0000D443 bx = chain[bx]; cmp bp, bx; jns 0xd420; ret */
        bx = lz->chain[bx & (M12_CHAIN_SIZE - 1u)];
        if ((int16_t)((uint16_t)(bp - bx)) < 0) return;
    }

d468:
    {
        /* D46C: save si as ax_saved */
        int16_t  ax_saved = si;
        /* Compute phys_cand = phys(bx) + ax_saved */
        uint16_t dist2     = (uint16_t)(pos_raw - bx);
        ptrdiff_t pc_s     = (ptrdiff_t)(pos_emit - dist2) + (ptrdiff_t)ax_saved;
        if (pc_s < 0) goto d4e4;
        size_t   pc        = (size_t)pc_s;
        uint16_t bx_pos_u  = (uint16_t)((uint16_t)bx + (uint16_t)(uint16_t)ax_saved);

        /* D468–D4A7: extend + update best */
        int old_best = best;
        lz12_extend(lz, pc, pos_emit, bx_pos_u, &best, 1 /* d420 */);
        if (lz->cur_len >= 256) return;                    /* D44D: max-length, ret */

        int cx_match;
        {
            int avail_c = (sz > pc)        ? (int)(sz - pc)        : 0;
            int avail_p = (sz > pos_emit)  ? (int)(sz - pos_emit)  : 0;
            int avail   = (avail_c < avail_p ? avail_c : avail_p);
            if (avail > 256) avail = 256;
            int len = 2, max_ext = 2 + 0x7F * 2;
            if (max_ext > avail) max_ext = avail;
            while (len + 1 < max_ext) {
                if (lz->src[pc + len]     != lz->src[pos_emit + len])     break;
                if (lz->src[pc + len + 1] != lz->src[pos_emit + len + 1]) { len++; break; }
                len += 2;
            }
            if (len < max_ext && len < 256 &&
                    lz->src[pc + len] == lz->src[pos_emit + len]) len++;
            cx_match = len;
        }

        if (best == old_best) goto d4e4;  /* not better: D497 jng D4E4 */

        /* ---- D4A8–D4E0: secondary chain lookup ---- */
        uint16_t sec_slot = (uint16_t)((uint16_t)bp + (uint16_t)cx_match - 257u);
        uint16_t ax_sec   = lz->chain[sec_slot & (M12_CHAIN_SIZE - 1u)];

        if ((int16_t)((uint16_t)(bp - ax_sec)) < 0) return;

        if ((int16_t)((uint16_t)(ax_sec - bx)) < 0) {
            bx  = ax_sec;
            si  = (int16_t)(2 - cx_match);
            ax_word = (pos_emit + 1 < sz) ? ((uint16_t)lz->src[pos_emit] | ((uint16_t)lz->src[pos_emit+1] << 8)) : lz->src[pos_emit];
            goto d420_loop;
        }

        uint16_t sec2_slot = (uint16_t)((uint16_t)bx
                                      + (uint16_t)(int16_t)ax_saved
                                      + (uint16_t)cx_match - 3u);
        uint16_t ax_c = lz->chain[sec2_slot & (M12_CHAIN_SIZE - 1u)];

        if ((int16_t)((uint16_t)(bp - ax_c)) < 0) return;

        uint16_t bx_next = lz->chain[bx & (M12_CHAIN_SIZE - 1u)];
        if ((int16_t)((uint16_t)(bp - bx_next)) < 0) return;
        bx = bx_next;

        if ((int16_t)((uint16_t)(ax_c - bx)) < 0) {
            bx  = ax_c;
            si  = (int16_t)(3 - cx_match);
            ax_word = (pos_emit + 1 < sz) ? ((uint16_t)lz->src[pos_emit] | ((uint16_t)lz->src[pos_emit+1] << 8)) : lz->src[pos_emit];
            goto d420_loop;
        }

        si  = ax_saved;
        ax_word = (pos_emit + 1 < sz) ? ((uint16_t)lz->src[pos_emit] | ((uint16_t)lz->src[pos_emit+1] << 8)) : lz->src[pos_emit];
        goto d420_loop;
    }

d4e4:
    bx = lz->chain[bx & (M12_CHAIN_SIZE - 1u)];
    if ((int16_t)((uint16_t)(bp - bx)) < 0) return;
    ax_word = (pos_emit + 1 < sz) ? ((uint16_t)lz->src[pos_emit] | ((uint16_t)lz->src[pos_emit+1] << 8)) : lz->src[pos_emit];
    goto d420_loop;
}

/* D318 chain walk: normal search from chain[bp-0xFF], compare at offset si=0.
 * pdx: optional shared chain-depth counter (NULL = allocate fresh from max_chain). */
static void lz12_d318_dx(LZ12State *lz, uint16_t bp, size_t pos_emit, int *pdx, uint16_t bx_start)
{
    size_t   sz      = lz->src_size;
    int      dx_own  = (int)(uint16_t)M12_MAX_CHAIN[lz->method < 4 ? lz->method : 3];
    int     *pdx_use = pdx ? pdx : &dx_own;
    int      best    = 2; /* local best in D318, tracks match length (L >= 2) */
    int      si      = 0; /* DOS si: tracks best - 1, initially 0 */
    uint16_t pos_raw = (uint16_t)(bp - 0x00FFu);

    /* D318 (bx_start==0) or D330 from D2C0 (bx_start!=0) */
    uint16_t bx = bx_start ? bx_start : lz->chain[pos_raw & (M12_CHAIN_SIZE - 1u)];
    /* D327: cmp bp, bx; js exit */
    if ((int16_t)((uint16_t)(bp - bx)) < 0) return;
    if (pos_emit >= sz) return;

    /* D32B: ax = word at [bp + di - 0xff] */
    uint16_t ax_word = (pos_emit + 1 < sz) ?
        ((uint16_t)lz->src[pos_emit] | ((uint16_t)lz->src[pos_emit + 1] << 8)) :
        lz->src[pos_emit];

d330_entry:
    for (;;) {
        /* Hop 1: 0000D330 dec dx; jl 0xd3a8 */
        (*pdx_use)--;
        if (*pdx_use < 0) return;

        {
            uint16_t dist = (uint16_t)(pos_raw - bx);
            if (dist > 0u && dist <= 0x7FFFu) {
                size_t phys_cand = pos_emit - (size_t)dist;
                if (phys_cand + (size_t)si < sz) {
                    uint16_t cand_w = (phys_cand + (size_t)si + 1 < sz) ?
                        ((uint16_t)lz->src[phys_cand + si] | ((uint16_t)lz->src[phys_cand + si + 1] << 8)) :
                        lz->src[phys_cand + si];
                    if (cand_w == ax_word) {
                        goto d360;
                    }
                }
            }
        }

        /* Hop 2: 0000D337 bx = chain[bx]; cmp bp, bx; js 0xd3a8 */
        bx = lz->chain[bx & (M12_CHAIN_SIZE - 1u)];
        if ((int16_t)((uint16_t)(bp - bx)) < 0) return;
        (*pdx_use)--;
        /* Note: DOS does NOT test jl 0xd3a8 here! */

        {
            uint16_t dist = (uint16_t)(pos_raw - bx);
            if (dist > 0u && dist <= 0x7FFFu) {
                size_t phys_cand = pos_emit - (size_t)dist;
                if (phys_cand + (size_t)si < sz) {
                    uint16_t cand_w = (phys_cand + (size_t)si + 1 < sz) ?
                        ((uint16_t)lz->src[phys_cand + si] | ((uint16_t)lz->src[phys_cand + si + 1] << 8)) :
                        lz->src[phys_cand + si];
                    if (cand_w == ax_word) {
                        goto d360;
                    }
                }
            }
        }

        /* Hop 3: 0000D345 bx = chain[bx]; cmp bp, bx; js 0xd3a8 */
        bx = lz->chain[bx & (M12_CHAIN_SIZE - 1u)];
        if ((int16_t)((uint16_t)(bp - bx)) < 0) return;
        (*pdx_use)--;
        /* Note: DOS does NOT test jl 0xd3a8 here! */

        {
            uint16_t dist = (uint16_t)(pos_raw - bx);
            if (dist > 0u && dist <= 0x7FFFu) {
                size_t phys_cand = pos_emit - (size_t)dist;
                if (phys_cand + (size_t)si < sz) {
                    uint16_t cand_w = (phys_cand + (size_t)si + 1 < sz) ?
                        ((uint16_t)lz->src[phys_cand + si] | ((uint16_t)lz->src[phys_cand + si + 1] << 8)) :
                        lz->src[phys_cand + si];
                    if (cand_w == ax_word) {
                        goto d360;
                    }
                }
            }
        }

        /* End of 3-hop block: 0000D353 bx = chain[bx]; cmp bp, bx; jns 0xd330; jmp 0xd3a8 */
        bx = lz->chain[bx & (M12_CHAIN_SIZE - 1u)];
        if ((int16_t)((uint16_t)(bp - bx)) < 0) return;
    }

d360:
    {
        size_t cand_phys = pos_emit - (size_t)(uint16_t)(pos_raw - bx);
        int cand_len = 0;
        while (cand_len < 256 && cand_phys + cand_len < sz && pos_emit + cand_len < sz &&
               lz->src[cand_phys + cand_len] == lz->src[pos_emit + cand_len]) {
            cand_len++;
        }
        if (cand_len >= 256) {
            lz->cur_len = 256;
            lz->cur_pos = bx;
            return;
        }

        if (cand_len >= 3 && cand_len > best) {
            /* D3CC: New best match >= 3 */
            best = cand_len;
            lz->cur_len = (uint16_t)best;
            lz->cur_pos = bx;

            /* D3D6: cx = chain[bp + best - 1 - 0x100] */
            uint16_t end_slot = (uint16_t)(bp + (uint16_t)best - 1u - 0x0100u);
            uint16_t cx = lz->chain[end_slot & (M12_CHAIN_SIZE - 1u)];
            if ((int16_t)((uint16_t)(bp - cx)) < 0) return; /* D3DF: js D3C2 */

            if ((int16_t)((uint16_t)(cx - bx)) < 0) {
                /* D3E6: js D40C with ax=2: si = -(best - 2) = 2 - best */
                int16_t si_d420 = (int16_t)(2 - best);
                uint16_t aw = (pos_emit + 1 < sz) ? ((uint16_t)lz->src[pos_emit] | ((uint16_t)lz->src[pos_emit+1] << 8)) : lz->src[pos_emit];
                lz12_d420(lz, bp, pos_emit, cx, *pdx_use, aw, si_d420);
                return;
            }

            /* Secondary check at D3E8 */
            uint16_t end_slot2 = (uint16_t)(bx + (uint16_t)best - 3u);
            cx = lz->chain[end_slot2 & (M12_CHAIN_SIZE - 1u)];
            if ((int16_t)((uint16_t)(bp - cx)) < 0) return; /* D3EF: js D3C2 */

            uint16_t step_bx = lz->chain[bx & (M12_CHAIN_SIZE - 1u)];
            if ((int16_t)((uint16_t)(bp - step_bx)) < 0) return; /* D3F8: js D3C2 */

            if ((int16_t)((uint16_t)(cx - step_bx)) < 0) {
                /* D3FC: js D40C with ax=3: si = -(best - 3) = 3 - best */
                int16_t si_d420 = (int16_t)(3 - best);
                uint16_t aw = (pos_emit + 1 < sz) ? ((uint16_t)lz->src[pos_emit] | ((uint16_t)lz->src[pos_emit+1] << 8)) : lz->src[pos_emit];
                lz12_d420(lz, bp, pos_emit, cx, *pdx_use, aw, si_d420);
                return;
            }

            /* D3FE..D406: neither branched to D40C */
            si = best - 1;
            ax_word = (pos_emit + (size_t)si + 1 < sz) ?
                ((uint16_t)lz->src[pos_emit + si] | ((uint16_t)lz->src[pos_emit + si + 1] << 8)) :
                lz->src[pos_emit + si];
            bx = step_bx;
            goto d330_entry;
        }

        /* D394: cand_len did NOT beat best */
        bx = lz->chain[bx & (M12_CHAIN_SIZE - 1u)];
        if ((int16_t)((uint16_t)(bp - bx)) < 0) return;
        ax_word = (pos_emit + (size_t)si + 1 < sz) ?
            ((uint16_t)lz->src[pos_emit + si] | ((uint16_t)lz->src[pos_emit + si + 1] << 8)) :
            lz->src[pos_emit + si];
        goto d330_entry;
    }
}

/* D318 entry point: always uses fresh max_chain dx (direct D318 path). */
static void lz12_d318(LZ12State *lz, uint16_t bp, size_t pos_emit)
{
    lz12_d318_dx(lz, bp, pos_emit, NULL, 0);
}

/* D2C0: optimization path when prv_len >= 3.
 * Scans chain entries near the END of the previous match window to find the
 * most-distant candidate, then starts D420 chain walk at offset cx from that entry.
 *
 * D2C5: si = 2 * (bp - 0x101 + prv_len)  [byte offset into chain]
 * Scans entries at offsets prv_len, prv_len-1, prv_len-2, ... down to 1
 * (2 initial comparisons + inner loop covering prv_len-3 more pairs).
 * Tracks maximum distance bx from bp; cx = signed offset index of maximum.
 *
 * D303: recover chain_entry from distance; ax_byte = src[pos_emit].
 * D313: jcxz D330 (si=0: normal D318-style compare at offset 0).
 * D315: jmp D420 (si=cx≠0: compare at offset cx, extend from cx+2).
 */
static void lz12_d2c0(LZ12State *lz, uint16_t bp, size_t pos_emit, uint16_t prv_len)
{
    /* D2C0: if prv_len < 3, fallback to D318. */
    if (prv_len < 3u) {
        lz12_d318(lz, bp, pos_emit);
        return;
    }

    /* D2C5–D2CB: si = 2*(bp - 0x101 + prv_len) [byte offset into ES chain].
     * Chain slot index = bp - 0x101 + prv_len (full 16-bit, wraps mod 65536). */
    uint16_t si_base = (uint16_t)(bp - 0x0101u + prv_len); /* chain entry at end of prev match */

    /* Read two initial candidates (D2CD, D2D4). */
    uint16_t c0 = lz->chain[si_base];                  /* chain[bp - 0x101 + prv_len] */
    if ((int16_t)((uint16_t)(bp - c0)) < 0) {          /* D2D2: js → ret */
        return;
    }
    uint16_t c1 = lz->chain[(uint16_t)(si_base - 1u)]; /* chain[bp - 0x102 + prv_len] */
    if ((int16_t)((uint16_t)(bp - c1)) < 0) {          /* D2DA: js → ret */
        return;
    }

    /* D2DC: ax = prv_len - 2; neg → ax = 2 - prv_len (negative for prv_len > 2). */
    int16_t ax = (int16_t)(2 - (int)prv_len);   /* = 2 - prv_len ≤ -1 for prv_len ≥ 3 */
    int16_t cx = ax;                              /* cx = ax (D2E1) */

    /* bx = max distance, dx = second candidate distance. */
    uint16_t bx_dist = (uint16_t)(bp - c0);
    uint16_t dx_dist = (uint16_t)(bp - c1);

    /* D2E3: cmp dx,bx; jl D2EA → update bx=dx when dx >= bx (signed, NOT strictly greater). */
    if ((int16_t)dx_dist >= (int16_t)bx_dist) {
        bx_dist = dx_dist;
        cx = (int16_t)(ax + 1);
    }

    /* D2EA–D301 inner loop:
     * Entry: D2EA: si -= 4 (2 slots back); D2ED: ax += 1; D2EE: ax += 1.
     * Each pass through D2EE increments ax by 1. The outer step (D2EA→D2ED→D2EE)
     * increments by 2 total before the first D2EF check.
     * After D2F1 comparison, the loop goes back to D2EE (one more +1).
     *
     * The net effect: scans entries at chain slots (si_base-2, si_base-3, si_base-4, ...)
     * Each D2EE pass: ax++; if ax > 0 → done. Otherwise: read chain[si], si-=1, compare.
     */
    si_base = (uint16_t)(si_base - 2u); /* D2EA: si -= 4 → -2 slots from si_base */
    ax = (int16_t)(ax + 1);             /* D2ED: ax += 1 */
    for (;;) {
        ax = (int16_t)(ax + 1);         /* D2EE: ax += 1 */
        if (ax > 0) break;              /* D2EF: jg D303 */

        /* D2F1: dx = bp - chain[si/2]. D2F6: si -= 2 (one slot). */
        uint16_t ca = lz->chain[si_base];
        si_base = (uint16_t)(si_base - 1u);  /* -1 slot */

        uint16_t da_dist = (uint16_t)(bp - ca);
        /* D2F9: cmp bx, dx; jg D2EE (bx wins: don't update). */
        if (!((int16_t)bx_dist > (int16_t)da_dist)) {
            /* D2FD: bx = dx; cx = ax. D301: goto D2EE. */
            bx_dist = da_dist;
            cx = ax;
        }
        /* D2FB/D301: goto D2EE (loop back, ax++ at top). */
    }

    /* D303: recover chain_entry from distance: chain_entry = bp - bx_dist. */
    uint16_t bx_entry = (uint16_t)(bp - bx_dist);
    /* D307: ax_byte = src[bp - 0xFF] = src[pos_emit]. */
    if (pos_emit >= lz->src_size) return;
    uint16_t ax_word = (pos_emit + 1 < lz->src_size) ? ((uint16_t)lz->src[pos_emit] | ((uint16_t)lz->src[pos_emit+1] << 8)) : lz->src[pos_emit];
    /* D30F: si = cx. D313: jcxz D330. */
    int      dx_chain = (int)(uint16_t)M12_MAX_CHAIN[lz->method < 4 ? lz->method : 3];
    if (cx == 0) {
        /* D313: jcxz D330 — enter D330 with bx = bx_entry */
        int dx = dx_chain;
        lz12_d318_dx(lz, bp, pos_emit, &dx, bx_entry);
    } else {
        /* D315: jmp D420 — enter D420 with bx = bx_entry */
        lz12_d420(lz, bp, pos_emit, bx_entry, dx_chain, ax_word, cx);
    }
}

/* Main M1/M2 search entry point — mirrors CEB5 call to D2C0.
 * CEB5: if prv_len < 3 → D318; else → D2C0 (which may call D420). */
static void lz12_search(LZ12State *lz, uint16_t bp, size_t pos_emit)
{
    if (lz->prv_len >= (uint16_t)MIN_MATCH_M12)
        lz12_d2c0(lz, bp, pos_emit, lz->prv_len);
    else
        lz12_d318(lz, bp, pos_emit);
}

/* One iteration of the M1/M2 main loop (CE82–CF35):
 *   1. Insert bp into hash (CE82–CEA6).
 *   2. Save [C836]→[C83A], [C838]→[C83C] (CEA9–CEB2).
 *   3. Call D2C0 or D318 to find new best match → [C836]/[C838] (CEB5).
 *   4. Return decision based on [C83A] (PREVIOUS best):
 *      - If prv_len < 3: caller should emit literal buf[bp - 0x100], advance bp++.
 *      - If new_len <= prv_len: caller should emit match (prv_len, prv_pos), advance bp+=prv_len.
 *      - Else (new_len > prv_len): emit literal, continue scanning.
 *
 * This function does the insert + search + save. The caller handles output.
 * After emitting a match of prv_len bytes, caller must fast-forward bp by prv_len-1
 * (one step was already taken), calling lz12_insert() for each skipped position. */
static void lz12_step(LZ12State *lz, uint16_t bp, size_t pos_emit)
{
    /* CE82–CEA6: insert bp into hash */
    lz12_insert(lz, bp);

    /* CEA9: save current → previous */
    lz->prv_len = lz->cur_len;
    lz->prv_pos = lz->cur_pos;

    /* In DOS: [C836] and [C838] are NOT zeroed before CEB5 call!
     * If prv_len < 3, D2C0 calls D318 which initializes its own best to 2.
     * If prv_len >= 3, D2C0 / D420 compares against [C836] which is prv_len! */
    if (lz->prv_len < (uint16_t)MIN_MATCH_M12) {
        lz->cur_len = 0;
        lz->cur_pos = 0;
    }

    /* CEB5: D2C0 / D318 — find new best match */
    lz12_search(lz, bp, pos_emit);
}

/* ======================================================================
 * Token buffer
 * ==================================================================== */
typedef struct {
    uint8_t  is_match;
    uint8_t  lit;
    uint16_t length;
    uint16_t dist;
} Token;

/* ======================================================================
 * Distance class encoding — mirrors ain2unpack.c decompress_stream exactly
 *
 * The Token.dist field stores token_dist = actual_dist - 1  (0-based),
 * matching the DOS compressor output (D122–D125: sub/inc/neg → dist-1).
 *
 * Decompressor decodes (1-based dist):
 *   D==0 → dist=1
 *   D==1 → dist=2
 *   D>=2 → extra = bs_read(D-1); dist = (1<<(D-1)) | extra + 1
 *
 * Inverse (token_dist = actual_dist - 1, 0-based):
 *   token_dist==0 → actual_dist=1 → D=0, extra=0, ebits=0
 *   token_dist==1 → actual_dist=2 → D=1, extra=0, ebits=0
 *   token_dist>=2 → actual_dist>=3:
 *     find D s.t. (1<<(D-1)) < actual_dist <= (1<<D)
 *     extra = actual_dist - (1<<(D-1)) - 1, ebits = D-1
 * ==================================================================== */
static void dist_to_D(int token_dist, int *D_out, uint16_t *extra_out, int *ebits_out)
{
    int dist = token_dist + 1;   /* convert 0-based back to 1-based */
    if (dist == 1) { *D_out=0; *extra_out=0; *ebits_out=0; return; }
    if (dist == 2) { *D_out=1; *extra_out=0; *ebits_out=0; return; }
    int D = 2;
    while ((1 << D) < dist) D++;
    *D_out     = D;
    *extra_out = (uint16_t)(dist - (1 << (D-1)) - 1);
    *ebits_out = D - 1;
}

/* ======================================================================
 * Write one compressed block
 * ==================================================================== */
static void write_block(BW *bw,
                        const Token *tokens, int ntok,
                        int is_last_block)
{
    /* Count symbol and distance frequencies */
    uint32_t sym_freq[N_SYMS_LIT];
    uint32_t dist_freq[N_SYMS_DIST];
    memset(sym_freq,  0, sizeof(sym_freq));
    memset(dist_freq, 0, sizeof(dist_freq));

    for (int i = 0; i < ntok; i++) {
        if (!tokens[i].is_match) {
            sym_freq[tokens[i].lit]++;
        } else {
            int D; uint16_t extra; int ebits;
            dist_to_D(tokens[i].dist, &D, &extra, &ebits);
            sym_freq[256 + D]++;
            /* lsym = length - 3: symbol 0 = length 3, symbol k = length k+3.
             * MIN_MATCH_M3=4 so we never emit lsym=0, but the table includes it. */
            int lsym = (int)tokens[i].length - 3;
            if (lsym < 0) lsym = 0;
            if (lsym >= (int)N_SYMS_DIST) lsym = (int)N_SYMS_DIST - 1;
            dist_freq[lsym]++;
        }
    }
    /* EOB sentinel: D=15, extra=0x3FFF → sym=271 */
    sym_freq[256 + 15]++;
    /* Ensure dist table is not empty (even for literal-only blocks) */
    if (dist_freq[0] == 0) dist_freq[0] = 1;

    /* Compute Huffman code lengths */
    uint8_t sym_lens[N_SYMS_LIT];
    uint8_t dist_lens[N_SYMS_DIST];
    huffman_lengths(sym_freq,  (int)N_SYMS_LIT,  sym_lens,  16);
    huffman_lengths(dist_freq, (int)N_SYMS_DIST, dist_lens, 16);

    if (g_debug) {
        fprintf(stderr, "BLOCK %d: ntok=%d last=%d\n", g_block_no, ntok, is_last_block);
        fprintf(stderr, "  sym_freq (non-zero):");
        for (int i=0; i<(int)N_SYMS_LIT; i++)
            if (sym_freq[i]) fprintf(stderr, " sym%d=%u", i, sym_freq[i]);
        fprintf(stderr, "\n  sym_lens (non-zero):");
        for (int i=0; i<(int)N_SYMS_LIT; i++)
            if (sym_lens[i]) fprintf(stderr, " sym%d=%u", i, sym_lens[i]);
        fprintf(stderr, "\n  dist_freq (non-zero):");
        for (int i=0; i<(int)N_SYMS_DIST; i++)
            if (dist_freq[i]) fprintf(stderr, " d%d=%u", i, dist_freq[i]);
        fprintf(stderr, "\n  dist_lens (non-zero):");
        for (int i=0; i<(int)N_SYMS_DIST; i++)
            if (dist_lens[i]) fprintf(stderr, " d%d=%u", i, dist_lens[i]);
        fprintf(stderr, "\n  first 5 tokens:");
        for (int i=0; i<ntok && i<5; i++) {
            if (!tokens[i].is_match)
                fprintf(stderr, " lit(%02X)", tokens[i].lit);
            else
                fprintf(stderr, " match(len=%d,dist=%d)", tokens[i].length, tokens[i].dist+1);
        }
        fprintf(stderr, "\n  last 10 tokens:");
        int start = ntok > 10 ? ntok-10 : 0;
        for (int i=start; i<ntok; i++) {
            if (!tokens[i].is_match)
                fprintf(stderr, " lit(%02X)", tokens[i].lit);
            else
                fprintf(stderr, " match(len=%d,dist=%d)", tokens[i].length, tokens[i].dist+1);
        }
        fprintf(stderr, "\n");
        g_block_no++;
    }

    /* Write block header tables */
    write_block_table(bw, sym_lens,  (int)N_SYMS_LIT);
    write_block_table(bw, dist_lens, (int)N_SYMS_DIST);

    /* Build decode tables (global, fast[] preserved between blocks) */
    build_huffman_table(&g_sym_tbl,  sym_lens,  (int)N_SYMS_LIT);
    build_huffman_table(&g_dist_tbl, dist_lens, (int)N_SYMS_DIST);

    /* Write tokens */
    for (int i = 0; i < ntok; i++) {
        uint16_t code; int clen;
        if (!tokens[i].is_match) {
            extract_code(&g_sym_tbl, tokens[i].lit, sym_lens[tokens[i].lit], &code, &clen);
            bw_write(bw, code, clen);
        } else {
            int D; uint16_t extra; int ebits;
            dist_to_D(tokens[i].dist, &D, &extra, &ebits);

            extract_code(&g_sym_tbl, 256 + D, sym_lens[256 + D], &code, &clen);
            bw_write(bw, code, clen);
            if (ebits) bw_write(bw, extra, ebits);

            int lsym = (int)tokens[i].length - 3;
            if (lsym < 0) lsym = 0;
            if (lsym >= (int)N_SYMS_DIST) lsym = (int)N_SYMS_DIST - 1;
            extract_code(&g_dist_tbl, lsym, dist_lens[lsym], &code, &clen);
            bw_write(bw, code, clen);
        }
    }

    /* Write EOB: sym=271 (D=15), then 14 bits = 0x3FFF */
    {
        uint16_t code; int clen;
        extract_code(&g_sym_tbl, 256+15, sym_lens[256+15], &code, &clen);
        bw_write(bw, code, clen);
        bw_write(bw, 0x3FFFu, 14);
    }

    /* End-of-session bit: 1 = last block, 0 = more blocks follow */
    bw_write(bw, is_last_block ? 1u : 0u, 1);
}

/* ======================================================================
 * Compress one file as one session into the bitstream writer.
 * method: 1=M1, 2=M2, 3=M3  (M4 is handled separately, never calls here)
 * ==================================================================== */
static void compress_session(BW *bw, const uint8_t *data, size_t size, int method)
{
    /* Zero fast[] once per session — mirrors decompress_stream() memset */
    session_tables_reset();

    /* Session bit (always 0) */
    bw_write(bw, 0u, 1);

    if (size == 0) {
        /* Empty file: single empty block with only EOB.
         * DOS quirk: with all-zero frequencies, DA0D-DA19 treats the last 2
         * entries of sorted[] as active (n_active=2), which are sym271 and sym270
         * (sorted by ascending freq with ties broken by ascending sym index →
         * sym270 at sorted[270], sym271 at sorted[271]).  Both get len=1.
         * Replicate by giving sym270 (D=14) a non-zero freq alongside sym271 (EOB).
         * For dist table: same logic gives dist[0] and dist[253] as active. */
        uint32_t zf[N_SYMS_LIT], zd[N_SYMS_DIST];
        memset(zf, 0, sizeof(zf)); memset(zd, 0, sizeof(zd));
        zf[256+14] = 1; zf[256+15] = 1; zd[0] = 1; zd[253] = 1;
        uint8_t sl[N_SYMS_LIT], dl[N_SYMS_DIST];
        huffman_lengths(zf, (int)N_SYMS_LIT,  sl, 16);
        huffman_lengths(zd, (int)N_SYMS_DIST, dl, 16);
        write_block_table(bw, sl, (int)N_SYMS_LIT);
        write_block_table(bw, dl, (int)N_SYMS_DIST);
        uint16_t code; int clen;
        extract_code(&g_sym_tbl, 256+15, sl[256+15], &code, &clen);
        bw_write(bw, code, clen);
        bw_write(bw, 0x3FFFu, 14);
        bw_write(bw, 1u, 1); /* last block */
        return;
    }

    /* Token buffer.
     * M3: intermediate buffer threshold fires before TOKEN_STACK_LIMIT, so we only
     * need a buffer sized to one block's worth of tokens (~14000 max for all-literals).
     * M1/M2: TOKEN_STACK_LIMIT is the DOS stack depth limit; BLOCK_OUTPUT_LIMIT fires first. */
    Token *tokens = malloc((TOKEN_STACK_LIMIT + 4) * sizeof(Token));

    if (method == 3) {
        /* ---- M3: 4-gram hash chain (D0A0–D230) ---- */
        /* DOS block-split model (D1AC/D659):
         * DOS accumulates tokens in an intermediate buffer (D5A0).  Each literal
         * contributes 1 byte, each match contributes 3 bytes, and there is 1 flag
         * byte per 8 tokens.  When the buffer size = ceil(N/8) + L + 3*M >= 14960,
         * DOS calls D659 to flush the block (write Huffman header + encoded tokens).
         * We replicate this split trigger exactly. */
        LZState *lz = malloc(sizeof(LZState));
        lz_init(lz, data, size);

        uint16_t bp  = 0;
        size_t   pos = 0;

        int    ntok       = 0;   /* tokens in current block */
        size_t blk_L      = 0;   /* literals in current block */
        size_t blk_M      = 0;   /* matches in current block */
        size_t stack_bytes = 0;  /* DOS stack usage within current batch */

        while (pos < size) {
            while ((int16_t)((uint16_t)(bp - lz->slide_sentinel)) >= 0)
                lz_slide(lz, bp);

            int token_dist = 0;
            int len = lz_find_and_insert(lz, bp, &token_dist);

            if (len >= (int)MIN_MATCH_M3) {
                tokens[ntok].is_match = 1;
                tokens[ntok].length   = (uint16_t)len;
                tokens[ntok].dist     = (uint16_t)token_dist;
                ntok++;
                blk_M++;
                stack_bytes += 4u;   /* match pushes 4 bytes onto DOS stack */
                bp  = (uint16_t)(bp + len);
                pos += (size_t)len;
                /* D12E/D14D: after match, flush when sp < 0xCBC2 (i.e. stack_used > DOS_STACK_MAT) */
                if (stack_bytes > DOS_STACK_MAT) {
                    stack_bytes = 0;
                    size_t inter = (size_t)((ntok + 7) / 8) + blk_L + 3 * blk_M;
                    if (inter >= BLOCK_OUTPUT_LIMIT) {
                        write_block(bw, tokens, ntok, 0 /* not last */);
                        ntok = 0; blk_L = 0; blk_M = 0;
                    }
                }
            } else {
                tokens[ntok].is_match = 0;
                tokens[ntok].lit      = data[pos];
                ntok++;
                blk_L++;
                stack_bytes += 2u;   /* literal pushes 2 bytes onto DOS stack */
                bp = (uint16_t)(bp + 1u);
                pos++;
                /* D07B/D09A: after literal, flush when sp < 0xCBCA (i.e. stack_used > DOS_STACK_LIT) */
                if (stack_bytes > DOS_STACK_LIT) {
                    stack_bytes = 0;
                    size_t inter = (size_t)((ntok + 7) / 8) + blk_L + 3 * blk_M;
                    if (inter >= BLOCK_OUTPUT_LIMIT) {
                        write_block(bw, tokens, ntok, 0 /* not last */);
                        ntok = 0; blk_L = 0; blk_M = 0;
                    }
                }
            }
        }
        /* Flush final block (D659 called at session end, no inter check needed) */
        write_block(bw, tokens, ntok, 1 /* last */);
        free(lz);

    } else {
        /* ---- M1/M2: rolling 1-byte hash + chain walk (CE67–CF35 + D2C0–D51C) ----
         *
         * bp starts at 0x100, not 0. Physical src position = bp - 0x100.
         * Slide trigger: [0xC408] - bp <= 0x100  (CF47: sub ax,[0xC408]; cmp ax,0x100)
         * Token dist = bp - 0x101 - best_pos  (CEEA)
         * bp advances: bp++ for literal (CED3: inc bp), bp += len for match (D4A8).
         *
         * The first 3 positions (bp=0xFD,0xFE,0xFF) are pre-inserted in the init
         * loop (CE61–CE64). We start collecting output tokens from bp=0x100 onwards.
         */
        LZ12State *lz12 = malloc(sizeof(LZ12State));
        lz12_init(lz12, data, size, method);

        /* bp starts at 0x100; output position pos = bp - 0x100.
         * DEFERRED OUTPUT: we decide at each bp using PREVIOUS best (prv_len/prv_pos).
         *
         * Main loop logic (mirrors CE7C–CF35):
         *   1. Slide check.
         *   2. lz12_step(bp): insert bp, save cur→prv, search for new cur.
         *   3. Decision based on prv_len:
         *      a. prv_len < 3: literal output buf[bp-0x100], bp++, pos++.
         *      b. prv_len >= 3 AND cur_len <= prv_len:
         *         emit match (prv_len, token_dist=bp-0x101-prv_pos), bp+=prv_len, pos+=prv_len.
         *         fast-forward: insert intermediate positions without searching.
         *      c. prv_len >= 3 AND cur_len > prv_len: still searching, emit literal.
         *
         * Token dist formula (CEEA): bp - 0x101 - prv_pos (raw DOS address).
         * File at DS:0x100, bp starts at 0x200 (file_pos 0 = DS:0x200-0x100).
         * prv_pos = raw DOS bp of best candidate.
         * actual_dist = bp - 0x100 - prv_pos = (0x200+file_pos) - 0x100 - prv_pos.
         * token_dist = actual_dist - 1 = bp - 0x101 - prv_pos.
         * Decompressor: copy from output[file_pos - actual_dist] = output[prv_pos - 0x100]. */
        uint16_t bp  = 0x0200u;
        size_t   pos = 0;
        /* DOS M1/M2 block-split state — mirrors CE7C/D5A0/D659 mechanism.
         * CE7C: cmp sp,0xCBD2; jc CE67 → flush D5A0 when stack_bytes > DOS_STACK_M12=384.
         * Flush fires BEFORE the current token's push (unlike M3 which fires after). */
        size_t   stack_bytes = 0;
        size_t   blk_L       = 0;
        size_t   blk_M       = 0;
        int      ntok        = 0;

        while (pos < size) {
            /* CE7C: check stack BEFORE processing this token's push.
             * If stack accumulated from previous tokens > 384 bytes: D5A0 flush.
             * After D5A0: check intermediate buffer ≥ 14960 → D659 block flush. */
            if (stack_bytes > DOS_STACK_M12) {
                stack_bytes = 0;
                size_t inter = (size_t)((ntok + 7) / 8) + blk_L + 3 * blk_M;
                if (inter >= BLOCK_OUTPUT_LIMIT) {
                    write_block(bw, tokens, ntok, 0 /* not last */);
                    ntok = 0; blk_L = 0; blk_M = 0;
                }
            }
            /* Slide trigger: (uint16_t)(sentinel - bp) <= 0x100
             * DOS CF47: cmp [0xc408]-bp, 0x100; ja CF76
             * "ja" = unsigned above → skip slide when (sentinel-bp) > 0x100.
             * Slide fires when NOT ja, i.e., (sentinel-bp) <= 0x100.
             * Confirmed by trace: bp=0x8000, sentinel=0x8100 → 0x100 ≤ 0x100 → fires.
             * New sentinel = bp-0x8000 = 0x0000.
             *
             * CRITICAL: slide fires at the CF36 path BEFORE insert(bp).
             * In DOS, the sequence is:
             *   previous token advances bp → cmp [C404],bp → exceeds → CF36
             *   CF47: slide check → slide(bp) → update sentinel/[C404]
             *   jmp CE7C → CE82: insert(bp)  ← insert happens AFTER slide
             * The fast-forward loop (CEEB) has no slide check inside it.
             * All fast-forward inserts use the pre-slide hash. The slide fires
             * at the TOP of the next main-loop iteration (before lz12_step/insert). */
            while ((uint16_t)(lz12->slide_sentinel - bp) <= 0x100u)
                lz12_slide(lz12, bp);

            /* Step: insert bp, search for cur.
             * pos_emit = pos + 1: the search covers DS:[bp-0xFF] = file[pos_emit]. */
            lz12_step(lz12, bp, pos + 1);

            /* Decision using prv (previous iteration's best) */
            if (lz12->prv_len >= (uint16_t)MIN_MATCH_M12 &&
                lz12->cur_len <= lz12->prv_len)
            {
                /* Emit previous match.
                 * prv_pos is the raw DOS candidate address (from lz12_search).
                 * token_dist = bp - 0x101 - prv_pos (CEEA: lea ax,[bp-0x101]; sub ax,[C83C]).
                 * actual_dist = token_dist + 1 = bp - 0x100 - prv_pos = pos - prv_pos. */
                int   mlen  = (int)lz12->prv_len;
                int   tdist = (int)(uint16_t)(bp - 0x0101u - lz12->prv_pos);
                tokens[ntok].is_match = 1;
                tokens[ntok].length   = (uint16_t)mlen;
                tokens[ntok].dist     = (uint16_t)tdist;
                ntok++;
                blk_M++;
                stack_bytes += 4u;   /* match pushes 4 bytes onto DOS stack (CEDC–CEEA) */
                /* Advance bp by mlen. Fast-forward: insert bp+1..bp+mlen-1.
                 * Mirrors CEEB loop (CEF4..CF19). NO slide check inside the fast-forward:
                 * DOS CEEB loop only does inc bp + insert, never checks [C404] or slide.
                 * The slide fires only AFTER the fast-forward via CF22→CF23→CF2C→CF36→CF47.
                 * CF1F: D318 at bp_last. CF22: inc bp (already done). */
                uint16_t bp_before = bp;
                bp  = (uint16_t)(bp + (uint16_t)mlen);
                pos += (size_t)mlen;
                uint16_t skip_bp = (uint16_t)(bp_before + 1u);
                while (skip_bp != bp) {
                    lz12_insert(lz12, skip_bp);
                    skip_bp = (uint16_t)(skip_bp + 1u);
                }
                /* CF1F: D318 at bp_last = bp - 1. */
                lz12->cur_len = 0; lz12->cur_pos = 0;
                lz12_d318(lz12, (uint16_t)(bp - 1u), pos);
                lz12->prv_len = 0; lz12->prv_pos = 0;
            } else {
                /* Emit literal buf[bp - 0x100 - 1] = data[pos]
                 * (the literal output is for the PREVIOUS bp = bp-1 after step) */
                if (pos < size) {
                    tokens[ntok].is_match = 0;
                    tokens[ntok].lit      = data[pos];
                    ntok++;
                    blk_L++;
                    stack_bytes += 2u;   /* literal pushes 2 bytes onto DOS stack (CECC) */
                    pos++;
                }
                bp = (uint16_t)(bp + 1u);
            }

            if (ntok >= (int)TOKEN_STACK_LIMIT) break;
        }
        /* Flush final block (D659 called at session end) */
        write_block(bw, tokens, ntok, 1 /* last */);
        free(lz12);
    }

    free(tokens);
}

/* ======================================================================
 * AIN checksum — 16-bit sum of all bytes (from index entry format)
 * ==================================================================== */
static uint16_t ain_checksum(const uint8_t *data, size_t size)
{
    uint16_t s = 0;
    for (size_t i = 0; i < size; i++) s = (uint16_t)(s + data[i]);
    return s;
}

/* ======================================================================
 * DOS timestamp from Unix time_t
 * ==================================================================== */
static uint32_t unix_to_dos_time(time_t t)
{
    struct tm *tm = localtime(&t);
    if (!tm) return 0;
    uint16_t date = (uint16_t)(((tm->tm_year - 80) << 9)
                              | ((tm->tm_mon  +  1) << 5)
                              |   tm->tm_mday);
    uint16_t time = (uint16_t)((tm->tm_hour << 11)
                              | (tm->tm_min  <<  5)
                              | (tm->tm_sec  >>  1));
    return ((uint32_t)date << 16) | time;
}

/* ======================================================================
 * Uppercase DOS 8.3 basename
 * ==================================================================== */
static void to_dos_name(const char *path, char *out, size_t outsz)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    if (n >= outsz) n = outsz - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)toupper((unsigned char)base[i]);
    out[n] = '\0';
}

/* ======================================================================
 * Slurp file
 * ==================================================================== */
static uint8_t *slurp(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    uint8_t *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    *out_size = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    return buf;
}

/* ======================================================================
 * Write a uint16 LE / uint32 LE into a byte buffer
 * ==================================================================== */
static void wr16(uint8_t *p, uint16_t v)
{ p[0]=(uint8_t)(v&0xFF); p[1]=(uint8_t)(v>>8); }
static void wr32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)(v&0xFF); p[1]=(uint8_t)((v>>8)&0xFF);
  p[2]=(uint8_t)((v>>16)&0xFF); p[3]=(uint8_t)(v>>24); }


/* ======================================================================
 * Huffman Symbol Decoding — from ain2unpack.c
 * ==================================================================== */
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

/* ======================================================================
 * Decompression Table Reading — from ain2unpack.c
 * ==================================================================== */
/* ======================================================================
 * read_precode — read 19-symbol pre-code from bitstream
 * ==================================================================== */
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

/* ======================================================================
 * read_code_lengths — decode n lengths via pre-code RLE
 * ==================================================================== */
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

/* ======================================================================
 * read_block_table — read one Huffman table for a block
 * ==================================================================== */
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
 * Decompress Stream — from ain2unpack.c
 * ==================================================================== */
static int decompress_stream(BS *bs, GrowBuf *out)
{
    static uint8_t    window[BUFFER_LIMIT + 256];
    static HuffTable  sym_tbl, dist_tbl, pre_tbl;

    /*
     * Zero fast[] once per call — matches DOS BSS behaviour: tables are
     * zero-initialised at program start (one segment per decompress call),
     * then never cleared between blocks.  Making fast[] zero at call entry
     * ensures the index-stream call cannot contaminate the data-stream call.
     *
     * tree[] is cleared per-block inside build_huffman_table, so it needs
     * no special treatment here.
     */
    memset(sym_tbl.fast,  0, sizeof(sym_tbl.fast));
    memset(dist_tbl.fast, 0, sizeof(dist_tbl.fast));
    memset(pre_tbl.fast,  0, sizeof(pre_tbl.fast));

    /*
     * Outer loop: one iteration per compression session.
     *
     * Multi-file archives contain multiple back-to-back sessions in one
     * stream: each session ends with end_bit=1, then immediately the next
     * session starts with its own session-bit.  Single-file archives have
     * exactly one session.  We keep restarting until the bitstream is
     * fully consumed.
     */
    for (;;) {
        if (bs->exhausted && bs->nbits <= 0) return 0;

        memset(window, 0, sizeof(window));
        bs_read(bs, 1);   /* session bit — consumed and ignored */
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

                    /* LZ copy — circular 32KB history window.
                     * Plain subtraction + explicit wrap (no & 0x7FFF mask):
                     * (di - dist) & 0x7FFF would corrupt addresses in [0x8000..0x8100].
                     * window[] is oversized to BUFFER_LIMIT+256=33152 so no per-byte
                     * wrap needed in the copy loop (max src+len = 32767+258 = 33025). */
                    int src = (int)di - (int)dist;
                    if (src < 0) src += (int)WINDOW_SIZE;
                    for (int k = 0; k < length; k++)
                        window[di + k] = window[src + k];
                    di += length;
                }
            }

            int end_bit = (int)bs_read(bs, 1);
            if (end_bit || (bs->exhausted && bs->nbits <= 0)) {
                /* End of this compression session */
                if (di > 0 && gbuf_append(out, window, (size_t)di) < 0) return -1;
                break;   /* break inner loop → restart outer loop for next session */
            }
        }
        continue;   /* next session */

final_flush:
        if (di > 0 && gbuf_append(out, window, (size_t)di) < 0) return -1;
        return 0;
    }
}

/* ======================================================================
 * Enhanced Index Parsing & Directory / Timestamp Utilities
 * ==================================================================== */

#define IDX_HDR_SIZE 29

typedef struct {
    char     path[512];       /* relative path with '/' for extraction with -x */
    char     filename[260];   /* basename only for flat extraction with -e */
    char     raw_dos[260];    /* original DOS path as stored in index */
    uint32_t timestamp;       /* DOS timestamp */
    uint32_t orig_size;       /* original (uncompressed) size */
    uint32_t comp_size;       /* compressed stream size (0 for multi-file except last) */
    uint16_t checksum;        /* entry checksum */
} FileEntry;

static time_t dos_to_unix_time(uint32_t dos)
{
    if (dos == 0) return 0;
    uint16_t date = (uint16_t)(dos >> 16);
    uint16_t time = (uint16_t)(dos & 0xFFFFu);

    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = ((date >> 9) & 0x7Fu) + 80;
    tm.tm_mon  = ((date >> 5) & 0x0Fu) - 1;
    tm.tm_mday = (date & 0x1Fu);
    tm.tm_hour = (time >> 11) & 0x1Fu;
    tm.tm_min  = (time >> 5) & 0x3Fu;
    tm.tm_sec  = (time & 0x1Fu) * 2;
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

        strncpy(entries[i].raw_dos, (const char *)fname_start, sizeof(entries[i].raw_dos) - 1);
        entries[i].raw_dos[sizeof(entries[i].raw_dos) - 1] = '\0';

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

static int decompress_index_section(const uint8_t *data, size_t file_size,
                                    uint32_t idx_pos, GrowBuf *idx_buf)
{
    if (idx_pos >= (uint32_t)file_size) return 0;
    BS bs;
    size_t idx_stream_size = file_size - (size_t)idx_pos;
    bs_init(&bs, data + idx_pos, idx_stream_size);
    return decompress_stream(&bs, idx_buf);
}

/* ======================================================================
 * File Scanning for Archiving
 * ==================================================================== */

typedef struct {
    char     dospath[260];
    char     realpath[512];
    uint32_t timestamp;
    size_t   size;
    uint8_t *data;
} InputFile;

static void scan_path(const char *path, const char *dos_prefix, int recursive,
                      InputFile **pfiles, int *pcount, int *pcap)
{
    struct stat st;
    if (stat(path, &st) < 0) {
        perror(path);
        return;
    }

    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            fprintf(stderr, "Skipping directory: %s (use -r to recurse)\n", path);
            return;
        }
        DIR *dir = opendir(path);
        if (!dir) { perror(path); return; }
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            char sub_real[512];
            if (strcmp(path, ".") == 0 || strcmp(path, "./") == 0)
                snprintf(sub_real, sizeof(sub_real), "%s", de->d_name);
            else
                snprintf(sub_real, sizeof(sub_real), "%s/%s", path, de->d_name);

            char dos_item[64];
            to_dos_name(de->d_name, dos_item, sizeof(dos_item));
            char sub_dos[260];
            if (dos_prefix && dos_prefix[0])
                snprintf(sub_dos, sizeof(sub_dos), "%s\\%s", dos_prefix, dos_item);
            else
                snprintf(sub_dos, sizeof(sub_dos), "%s", dos_item);

            scan_path(sub_real, sub_dos, recursive, pfiles, pcount, pcap);
        }
        closedir(dir);
    } else if (S_ISREG(st.st_mode)) {
        if (*pcount >= *pcap) {
            *pcap = *pcap ? (*pcap * 2) : 64;
            *pfiles = realloc(*pfiles, (size_t)*pcap * sizeof(InputFile));
        }
        InputFile *f = &(*pfiles)[*pcount];
        memset(f, 0, sizeof(*f));
        strncpy(f->realpath, path, sizeof(f->realpath) - 1);
        if (dos_prefix && dos_prefix[0]) {
            strncpy(f->dospath, dos_prefix, sizeof(f->dospath) - 1);
        } else {
            to_dos_name(path, f->dospath, sizeof(f->dospath));
        }
        f->timestamp = unix_to_dos_time(st.st_mtime);
        f->data = slurp(path, &f->size);
        if (f->data) {
            (*pcount)++;
        }
    }
}

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int has_exe_extension(const char *name)
{
    if (!name) return 0;
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return (strcasecmp(dot, ".exe") == 0);
}

static int has_sfx_extension(const char *name)
{
    if (!name) return 0;
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return (strcasecmp(dot, ".sfx") == 0);
}

static int ain_locate_archive(const uint8_t *data, size_t file_size, size_t *out_offset, int *is_sfx)
{
    if (file_size >= 24 && data[0] == MAGIC_AIN) {
        if (out_offset) *out_offset = 0;
        if (is_sfx) *is_sfx = 0;
        return 0;
    }
    if (file_size >= 64 && data[0] == 'M' && data[1] == 'Z') {
        uint16_t cblp = (uint16_t)(data[2] | (data[3] << 8));
        uint16_t cp   = (uint16_t)(data[4] | (data[5] << 8));
        size_t sfx_offset = (cblp == 0) ? ((size_t)cp * 512) : ((size_t)(cp - 1) * 512 + cblp);
        if (sfx_offset + 24 <= file_size && data[sfx_offset] == MAGIC_AIN) {
            if (out_offset) *out_offset = sfx_offset;
            if (is_sfx) *is_sfx = 1;
            return 0;
        }
    }
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
                if (out_offset) *out_offset = sfx_offset;
                if (is_sfx) *is_sfx = 2;
                return 0;
            }
        }
        /* Fallback: scan starting from sfx_offset for valid AIN header */
        for (size_t o = sfx_offset; o + 24 <= file_size; o++) {
            if (data[o] == MAGIC_AIN) {
                uint16_t hdr_crc = rd16(data + o + 22);
                if (((ain_checksum(data + o, 22) ^ 0x5555u) & 0xFFFFu) == hdr_crc) {
                    if (out_offset) *out_offset = o;
                    if (is_sfx) *is_sfx = 2;
                    return 0;
                }
            }
        }
    }
    return -1;
}

/* ======================================================================
 * Command Handlers
 * ==================================================================== */

static size_t parse_volume_size(const char *s)
{
    if (!s || !s[0]) return 0;
    while (*s == ' ' || *s == '=') s++;
    if (!*s) return 0;

    /* Standard floppy formats */
    if (strcasecmp(s, "360") == 0 || strcasecmp(s, "360k") == 0) return 360u * 1024u;
    if (strcasecmp(s, "720") == 0 || strcasecmp(s, "720k") == 0) return 720u * 1024u;
    if (strcasecmp(s, "1.2") == 0 || strcasecmp(s, "1200") == 0 || strcasecmp(s, "1.2m") == 0 || strcasecmp(s, "1200k") == 0) return 1200u * 1024u;
    if (strcasecmp(s, "1.44") == 0 || strcasecmp(s, "1440") == 0 || strcasecmp(s, "1.44m") == 0 || strcasecmp(s, "1440k") == 0) return 1440u * 1024u;
    if (strcasecmp(s, "2.88") == 0 || strcasecmp(s, "2880") == 0 || strcasecmp(s, "2.88m") == 0 || strcasecmp(s, "2880k") == 0) return 2880u * 1024u;

    char *end = NULL;
    double val = strtod(s, &end);
    if (end && *end) {
        char unit = (char)tolower((unsigned char)*end);
        if (unit == 'k') return (size_t)(val * 1024.0);
        if (unit == 'm') return (size_t)(val * 1024.0 * 1024.0);
        if (unit == 'g') return (size_t)(val * 1024.0 * 1024.0 * 1024.0);
        if (unit == 'b') return (size_t)val;
    }
    return (size_t)val;
}

static int cmd_add(const char *arcname, int method, int recursive,
                   int sfx, const char *sfx_stub_path,
                   size_t vol_size,
                   int n_inputs, char **inputs)
{
    InputFile *files = NULL;
    int n_files = 0;
    int f_cap = 0;

    for (int i = 0; i < n_inputs; i++) {
        char clean_input[512];
        strncpy(clean_input, inputs[i], sizeof(clean_input) - 1);
        clean_input[sizeof(clean_input) - 1] = '\0';
        size_t slen = strlen(clean_input);
        while (slen > 1 && clean_input[slen - 1] == '/') {
            clean_input[--slen] = '\0';
        }

        struct stat st;
        if (stat(clean_input, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (strcmp(clean_input, ".") == 0) {
                scan_path(clean_input, NULL, recursive, &files, &n_files, &f_cap);
            } else {
                const char *base = strrchr(clean_input, '/');
                const char *pname = base ? base + 1 : clean_input;
                char dos_root[64];
                to_dos_name(pname, dos_root, sizeof(dos_root));
                scan_path(clean_input, dos_root, recursive, &files, &n_files, &f_cap);
            }
        } else {
            scan_path(clean_input, NULL, recursive, &files, &n_files, &f_cap);
        }
    }

    if (n_files == 0) {
        fprintf(stderr, "No files found to archive.\n");
        free(files);
        return 1;
    }

    show_banner();
    printf("Creating archive %s\n", arcname);
    for (int i = 0; i < n_files; i++) {
        printf("Adding %s\n", files[i].dospath);
    }

    uint8_t *stream_data = NULL;
    size_t   stream_size = 0;

    if (method == 4) {
        for (int i = 0; i < n_files; i++) stream_size += files[i].size;
        stream_data = malloc(stream_size ? stream_size : 1);
        size_t off = 0;
        for (int i = 0; i < n_files; i++) {
            if (files[i].size)
                memcpy(stream_data + off, files[i].data, files[i].size);
            off += files[i].size;
        }
    } else {
        BW bw; bw_init(&bw);
        for (int i = 0; i < n_files; i++)
            compress_session(&bw, files[i].data, files[i].size, method);
        bw_flush(&bw);
        stream_data = bw.data;
        stream_size = bw.pos;
    }

    size_t   raw_idx_cap  = 4096;
    size_t   raw_idx_size = 0;
    uint8_t *raw_idx      = malloc(raw_idx_cap);

#define IDX_BYTE(b) do { \
    if (raw_idx_size >= raw_idx_cap) { raw_idx_cap*=2; raw_idx=realloc(raw_idx,raw_idx_cap); } \
    raw_idx[raw_idx_size++] = (uint8_t)(b); } while(0)
#define IDX_U16(v) do { IDX_BYTE((v)&0xFF); IDX_BYTE(((v)>>8)&0xFF); } while(0)
#define IDX_U32(v) do { IDX_BYTE((v)&0xFF); IDX_BYTE(((v)>>8)&0xFF); \
                        IDX_BYTE(((v)>>16)&0xFF); IDX_BYTE(((v)>>24)&0xFF); } while(0)

    for (int i = 0; i < n_files; i++) {
        uint32_t csz = (n_files == 1 || i == n_files - 1) ? (uint32_t)stream_size : 0u;
        uint16_t csum = (n_files == 1 || i == n_files - 1) ? ain_checksum(stream_data, stream_size) : 0u;
        IDX_BYTE(0x20);
        IDX_U32(files[i].timestamp);
        IDX_U32((uint32_t)files[i].size);
        IDX_U32(csz);
        IDX_BYTE(0x18);
        for (int k = 0; k < 8; k++) IDX_BYTE(0x00);
        IDX_BYTE(0x18);
        for (int k = 0; k < 4; k++) IDX_BYTE(0x00);
        IDX_U16(csum);
        for (const char *p = files[i].dospath; *p; p++) IDX_BYTE((uint8_t)*p);
        IDX_BYTE(0x00);
        IDX_BYTE(0x00);
    }

    BW idx_bw; bw_init(&idx_bw);
    compress_session(&idx_bw, raw_idx, raw_idx_size, 1);
    bw_flush(&idx_bw);

    const uint8_t *stub_data = NULL;
    size_t stub_size = 0;
    uint8_t *custom_stub = NULL;
    GrowBuf decomp_stub = {0};

    if (sfx) {
        if (sfx_stub_path && sfx_stub_path[0]) {
            size_t csz = 0;
            custom_stub = slurp(sfx_stub_path, &csz);
            if (!custom_stub) {
                fprintf(stderr, "Self-extracting module %s not found\n", sfx_stub_path);
                free(raw_idx); free(stream_data); free(idx_bw.data);
                for (int i = 0; i < n_files; i++) free(files[i].data);
                free(files);
                return 1;
            }
            stub_data = custom_stub;
            stub_size = csz;
        } else if (sfx == 2) {
            BS bs;
            bs_init(&bs, ain_compressed_linux_sfx_stub, sizeof(ain_compressed_linux_sfx_stub));
            if (decompress_stream(&bs, &decomp_stub) < 0 || !decomp_stub.data) {
                fprintf(stderr, "Error: failed to decompress embedded Linux SFX stub\n");
                free(raw_idx); free(stream_data); free(idx_bw.data);
                for (int i = 0; i < n_files; i++) free(files[i].data);
                free(files);
                return 1;
            }
            stub_data = decomp_stub.data;
            stub_size = decomp_stub.size;
        } else if (sfx == 1) {
            BS bs;
            bs_init(&bs, ain_compressed_sfx_stub, sizeof(ain_compressed_sfx_stub));
            if (decompress_stream(&bs, &decomp_stub) < 0 || !decomp_stub.data) {
                fprintf(stderr, "Error: failed to decompress embedded DOS SFX stub\n");
                free(raw_idx); free(stream_data); free(idx_bw.data);
                for (int i = 0; i < n_files; i++) free(files[i].data);
                free(files);
                return 1;
            }
            stub_data = decomp_stub.data;
            stub_size = decomp_stub.size;
        }
    }

    int is_multi = (vol_size > 0 && (stub_size + 24u + stream_size + idx_bw.pos > vol_size));

    if (!is_multi) {
        uint8_t hdr[24];
        memset(hdr, 0, sizeof(hdr));
        hdr[0] = (uint8_t)MAGIC_AIN;
        hdr[1] = (uint8_t)(0x10 + method);
        wr16(hdr + 8,  (uint16_t)n_files);
        wr32(hdr + 10, unix_to_dos_time(time(NULL)));
        wr32(hdr + 14, (uint32_t)(24u + stream_size));
        wr16(hdr + 18, ain_checksum(idx_bw.data, idx_bw.pos));
        uint16_t hdr_crc = (uint16_t)(ain_checksum(hdr, 22) ^ 0x5555u);
        wr16(hdr + 22, hdr_crc);

        FILE *out = fopen(arcname, "wb");
        if (!out) {
            perror(arcname);
            if (custom_stub) free(custom_stub);
            if (decomp_stub.data) free(decomp_stub.data);
            free(raw_idx); free(stream_data); free(idx_bw.data);
            for (int i = 0; i < n_files; i++) free(files[i].data);
            free(files);
            return 1;
        }
        if (sfx) {
            if (fwrite(stub_data, 1, stub_size, out) != stub_size) {
                perror("Failed to write SFX stub");
                fclose(out);
                if (custom_stub) free(custom_stub);
                if (decomp_stub.data) free(decomp_stub.data);
                free(raw_idx); free(stream_data); free(idx_bw.data);
                for (int i = 0; i < n_files; i++) free(files[i].data);
                free(files);
                return 1;
            }
        }
        fwrite(hdr,         1, 24,          out);
        fwrite(stream_data, 1, stream_size, out);
        fwrite(idx_bw.data, 1, idx_bw.pos,  out);
        fclose(out);
        if (sfx == 2) {
            chmod(arcname, 0755);
        }
    } else {
        /* Multi-volume creation */
        size_t cap0 = (vol_size > stub_size + 24u) ? (vol_size - stub_size - 24u) : 0;
        size_t capK = (vol_size > 24u) ? (vol_size - 24u) : 0;
        if (cap0 == 0 || capK == 0) {
            fprintf(stderr, "Error: volume size %zu is too small for stub and AIN header\n", vol_size);
            if (custom_stub) free(custom_stub);
            if (decomp_stub.data) free(decomp_stub.data);
            free(raw_idx); free(stream_data); free(idx_bw.data);
            for (int i = 0; i < n_files; i++) free(files[i].data);
            free(files);
            return 1;
        }

        char base_name[PATH_MAX];
        strncpy(base_name, arcname, sizeof(base_name) - 1);
        base_name[sizeof(base_name) - 1] = '\0';
        char *slash = strrchr(base_name, '/');
        char *dot = strrchr(base_name, '.');
        int uppercase_ext = 0;
        if (dot && (!slash || dot > slash)) {
            if (isupper((unsigned char)dot[1])) uppercase_ext = 1;
            *dot = '\0';
        }

        uint32_t arc_ts = unix_to_dos_time(time(NULL));
        size_t slice0 = (stream_size > cap0) ? cap0 : stream_size;

        /* Write Volume 0 */
        FILE *out0 = fopen(arcname, "wb");
        if (!out0) {
            perror(arcname);
            if (custom_stub) free(custom_stub);
            if (decomp_stub.data) free(decomp_stub.data);
            free(raw_idx); free(stream_data); free(idx_bw.data);
            for (int i = 0; i < n_files; i++) free(files[i].data);
            free(files);
            return 1;
        }
        if (sfx) {
            fwrite(stub_data, 1, stub_size, out0);
        }
        uint8_t hdr0[24];
        memset(hdr0, 0, sizeof(hdr0));
        hdr0[0] = (uint8_t)MAGIC_AIN;
        hdr0[1] = (uint8_t)(0x10 + method);
        hdr0[3] = 0x40; /* more fragments follow */
        wr16(hdr0 + 6, 0);
        wr16(hdr0 + 8, (uint16_t)n_files);
        wr32(hdr0 + 10, arc_ts);
        wr32(hdr0 + 14, 0);
        wr16(hdr0 + 18, 0);
        wr16(hdr0 + 22, (uint16_t)(ain_checksum(hdr0, 22) ^ 0x5555u));

        fwrite(hdr0, 1, 24, out0);
        if (slice0 > 0) {
            fwrite(stream_data, 1, slice0, out0);
        }
        fclose(out0);
        if (sfx == 2) chmod(arcname, 0755);
        printf("Creating fragment %s (volume 0)\n", arcname);

        size_t stream_offset = slice0;
        uint16_t vol_k = 1;

        while (1) {
            size_t rem_stream = stream_size - stream_offset;
            char vol_path[PATH_MAX + 32];
            snprintf(vol_path, sizeof(vol_path), "%s.%c%02u",
                     base_name, uppercase_ext ? 'A' : 'a', (unsigned)vol_k);

            if (rem_stream + idx_bw.pos <= capK) {
                /* Final volume */
                FILE *outK = fopen(vol_path, "wb");
                if (!outK) { perror(vol_path); break; }
                uint8_t hdrK[24];
                memset(hdrK, 0, sizeof(hdrK));
                hdrK[0] = (uint8_t)MAGIC_AIN;
                hdrK[1] = (uint8_t)(0x10 + method);
                hdrK[3] = 0x00; /* final volume */
                wr16(hdrK + 6, vol_k);
                wr16(hdrK + 8, (uint16_t)n_files);
                wr32(hdrK + 10, arc_ts);
                wr32(hdrK + 14, (uint32_t)(24u + rem_stream));
                wr16(hdrK + 18, ain_checksum(idx_bw.data, idx_bw.pos));
                wr16(hdrK + 22, (uint16_t)(ain_checksum(hdrK, 22) ^ 0x5555u));

                fwrite(hdrK, 1, 24, outK);
                if (rem_stream > 0) {
                    fwrite(stream_data + stream_offset, 1, rem_stream, outK);
                }
                fwrite(idx_bw.data, 1, idx_bw.pos, outK);
                fclose(outK);
                printf("Creating fragment %s (volume %u - final)\n", vol_path, (unsigned)vol_k);
                break;
            } else {
                /* Intermediate volume */
                size_t sliceK = (rem_stream > capK) ? capK : rem_stream;
                FILE *outK = fopen(vol_path, "wb");
                if (!outK) { perror(vol_path); break; }
                uint8_t hdrK[24];
                memset(hdrK, 0, sizeof(hdrK));
                hdrK[0] = (uint8_t)MAGIC_AIN;
                hdrK[1] = (uint8_t)(0x10 + method);
                hdrK[3] = 0x40; /* more fragments follow */
                wr16(hdrK + 6, vol_k);
                wr16(hdrK + 8, (uint16_t)n_files);
                wr32(hdrK + 10, arc_ts);
                wr32(hdrK + 14, 0);
                wr16(hdrK + 18, 0);
                wr16(hdrK + 22, (uint16_t)(ain_checksum(hdrK, 22) ^ 0x5555u));

                fwrite(hdrK, 1, 24, outK);
                if (sliceK > 0) {
                    fwrite(stream_data + stream_offset, 1, sliceK, outK);
                    stream_offset += sliceK;
                }
                fclose(outK);
                printf("Creating fragment %s (volume %u)\n", vol_path, (unsigned)vol_k);
                vol_k++;
            }
        }
    }

    if (custom_stub) free(custom_stub);
    if (decomp_stub.data) free(decomp_stub.data);

    printf("%d files processed\n", n_files);

    free(raw_idx);
    free(stream_data);
    free(idx_bw.data);
    for (int i = 0; i < n_files; i++) free(files[i].data);
    free(files);
    return 0;
}

typedef struct {
    char path[PATH_MAX + 256];
    uint8_t *raw_data;
    size_t file_size;
    size_t arc_offset;
    size_t arc_size;
    uint16_t vol_num;
    uint8_t flags;
    uint32_t idx_pos;
} ArchiveVolume;

typedef struct {
    ArchiveVolume *vols;
    int n_vols;
    int is_sfx;             /* 0 = ain, 1 = dos sfx, 2 = linux sfx */
    uint8_t method;
    uint16_t n_files;
    uint32_t arc_ts;
    uint8_t *stream_data;
    size_t stream_size;
    GrowBuf idx_buf;
    FileEntry *entries;
} ArchiveSet;

static void free_archive_set(ArchiveSet *set)
{
    if (!set) return;
    if (set->vols) {
        for (int i = 0; i < set->n_vols; i++) {
            if (set->vols[i].raw_data) free(set->vols[i].raw_data);
        }
        free(set->vols);
        set->vols = NULL;
    }
    if (set->stream_data) {
        free(set->stream_data);
        set->stream_data = NULL;
    }
    if (set->idx_buf.data) {
        free(set->idx_buf.data);
        set->idx_buf.data = NULL;
    }
    if (set->entries) {
        free(set->entries);
        set->entries = NULL;
    }
    set->n_vols = 0;
}

static int load_archive_set(const char *arcpath, ArchiveSet *set)
{
    memset(set, 0, sizeof(*set));

    /* Check if arcpath points to a fragment (e.g. .a01, .A01, .001) */
    char vol0_path[PATH_MAX + 256];
    snprintf(vol0_path, sizeof(vol0_path), "%s", arcpath);

    char *dot = strrchr(vol0_path, '.');
    char *slash = strrchr(vol0_path, '/');
    if (dot && (!slash || dot > slash)) {
        if ((dot[1] == 'a' || dot[1] == 'A') && isdigit((unsigned char)dot[2])) {
            char base[PATH_MAX];
            size_t blen = (size_t)(dot - vol0_path);
            if (blen < sizeof(base)) {
                memcpy(base, vol0_path, blen);
                base[blen] = '\0';
                char cand[PATH_MAX + 16];
                snprintf(cand, sizeof(cand), "%s.ain", base);
                if (access(cand, F_OK) == 0) snprintf(vol0_path, sizeof(vol0_path), "%s", cand);
                else {
                    snprintf(cand, sizeof(cand), "%s.AIN", base);
                    if (access(cand, F_OK) == 0) snprintf(vol0_path, sizeof(vol0_path), "%s", cand);
                    else {
                        snprintf(cand, sizeof(cand), "%s.sfx", base);
                        if (access(cand, F_OK) == 0) snprintf(vol0_path, sizeof(vol0_path), "%s", cand);
                        else {
                            snprintf(cand, sizeof(cand), "%s.EXE", base);
                            if (access(cand, F_OK) == 0) snprintf(vol0_path, sizeof(vol0_path), "%s", cand);
                        }
                    }
                }
            }
        }
    }

    size_t fsz0 = 0;
    uint8_t *raw0 = slurp(vol0_path, &fsz0);
    if (!raw0) {
        char try_ain[PATH_MAX];
        snprintf(try_ain, sizeof(try_ain), "%s.AIN", arcpath);
        raw0 = slurp(try_ain, &fsz0);
        if (raw0) {
            snprintf(vol0_path, sizeof(vol0_path), "%s", try_ain);
        } else {
            snprintf(try_ain, sizeof(try_ain), "%s.ain", arcpath);
            raw0 = slurp(try_ain, &fsz0);
            if (raw0) snprintf(vol0_path, sizeof(vol0_path), "%s", try_ain);
            else return -1;
        }
    }

    size_t arc_offset = 0;
    int is_sfx = 0;
    if (ain_locate_archive(raw0, fsz0, &arc_offset, &is_sfx) < 0) {
        fprintf(stderr, "Error: %s is not an AIN archive\n", vol0_path);
        free(raw0);
        return -1;
    }

    const uint8_t *hdr0 = raw0 + arc_offset;
    size_t arc_sz0 = fsz0 - arc_offset;
    if (arc_sz0 < 24u || hdr0[0] != (uint8_t)MAGIC_AIN) {
        fprintf(stderr, "Error: invalid AIN header in %s\n", vol0_path);
        free(raw0);
        return -1;
    }

    set->is_sfx = is_sfx;
    set->method = hdr0[1];
    set->n_files = (uint16_t)(hdr0[8] | (hdr0[9] << 8));
    set->arc_ts = (uint32_t)(hdr0[10] | (hdr0[11] << 8) | (hdr0[12] << 16) | (hdr0[13] << 24));
    uint8_t flags0 = hdr0[3];
    uint32_t idx_pos0 = (uint32_t)(hdr0[14] | (hdr0[15] << 8) | (hdr0[16] << 16) | (hdr0[17] << 24));

    set->vols = calloc(16, sizeof(ArchiveVolume));
    int vols_cap = 16;
    snprintf(set->vols[0].path, sizeof(set->vols[0].path), "%s", vol0_path);
    set->vols[0].raw_data = raw0;
    set->vols[0].file_size = fsz0;
    set->vols[0].arc_offset = arc_offset;
    set->vols[0].arc_size = arc_sz0;
    set->vols[0].vol_num = (uint16_t)(hdr0[6] | (hdr0[7] << 8));
    set->vols[0].flags = flags0;
    set->vols[0].idx_pos = idx_pos0;
    set->n_vols = 1;

    /* AIN 2.32 multi-volume slices have flags & 0x40 and idx_pos == 0 on intermediate volumes.
     * Earlier AIN versions (2.2 / 2.22 /F fragments) also set flags & 0x40, but write a
     * self-contained index (idx_pos != 0) on each volume, treated as a standalone volume archive. */
    int is_multi = ((flags0 & 0x40u) != 0) && (idx_pos0 == 0 || idx_pos0 >= arc_sz0);

    GrowBuf stream_buf = {0};

    if (!is_multi) {
        /* Single volume */
        if (decompress_index_section(hdr0, arc_sz0, idx_pos0, &set->idx_buf) < 0 || !set->idx_buf.data) {
            fprintf(stderr, "Error: failed to decompress archive index in %s\n", vol0_path);
            free_archive_set(set);
            return -1;
        }

        size_t stream_size = (idx_pos0 > 24u && idx_pos0 <= arc_sz0) ? (size_t)(idx_pos0 - 24u) : 0u;
        if (stream_size > 0) {
            set->stream_data = malloc(stream_size);
            if (set->stream_data) {
                memcpy(set->stream_data, hdr0 + 24u, stream_size);
                set->stream_size = stream_size;
            }
        } else {
            set->stream_data = malloc(1);
            set->stream_size = 0;
        }
    } else {
        /* Multi-volume fragment archive */
        char base_path[PATH_MAX];
        strncpy(base_path, vol0_path, sizeof(base_path) - 1);
        base_path[sizeof(base_path) - 1] = '\0';
        char *vslash = strrchr(base_path, '/');
        char *vdot = strrchr(base_path, '.');
        if (vdot && (!vslash || vdot > vslash)) {
            *vdot = '\0';
        }

        /* Volume 0 payload chunk */
        if (arc_sz0 > 24u) {
            gbuf_append(&stream_buf, hdr0 + 24u, arc_sz0 - 24u);
        }

        for (uint16_t vol = 1; ; vol++) {
            char frag_path[PATH_MAX + 32];
            snprintf(frag_path, sizeof(frag_path), "%s.a%02u", base_path, (unsigned)vol);
            if (access(frag_path, F_OK) != 0)
                snprintf(frag_path, sizeof(frag_path), "%s.A%02u", base_path, (unsigned)vol);
            if (access(frag_path, F_OK) != 0)
                snprintf(frag_path, sizeof(frag_path), "%s.%03u", base_path, (unsigned)vol);

            size_t frag_sz = 0;
            uint8_t *frag_raw = slurp(frag_path, &frag_sz);
            if (!frag_raw) {
                fprintf(stderr, "Error: fragment %s not found\n", frag_path);
                free(stream_buf.data);
                free_archive_set(set);
                return -1;
            }
            if (frag_sz < 24u || frag_raw[0] != (uint8_t)MAGIC_AIN) {
                fprintf(stderr, "Error: fragment %s is corrupt or not an AIN archive\n", frag_path);
                free(frag_raw); free(stream_buf.data);
                free_archive_set(set);
                return -1;
            }
            uint16_t frag_crc = (uint16_t)(frag_raw[22] | (frag_raw[23] << 8));
            if (((ain_checksum(frag_raw, 22) ^ 0x5555u) & 0xFFFFu) != frag_crc) {
                fprintf(stderr, "Error: header checksum mismatch in %s\n", frag_path);
                free(frag_raw); free(stream_buf.data);
                free_archive_set(set);
                return -1;
            }
            uint16_t frag_vol = (uint16_t)(frag_raw[6] | (frag_raw[7] << 8));
            if (frag_vol != vol) {
                fprintf(stderr, "Error: volume sequence mismatch in %s (expected %u, got %u)\n",
                        frag_path, (unsigned)vol, (unsigned)frag_vol);
                free(frag_raw); free(stream_buf.data);
                free_archive_set(set);
                return -1;
            }

            if (set->n_vols >= vols_cap) {
                vols_cap *= 2;
                set->vols = realloc(set->vols, (size_t)vols_cap * sizeof(ArchiveVolume));
            }
            int idx = set->n_vols++;
            snprintf(set->vols[idx].path, sizeof(set->vols[idx].path), "%s", frag_path);
            set->vols[idx].raw_data = frag_raw;
            set->vols[idx].file_size = frag_sz;
            set->vols[idx].arc_offset = 0;
            set->vols[idx].arc_size = frag_sz;
            set->vols[idx].vol_num = frag_vol;
            set->vols[idx].flags = frag_raw[3];
            set->vols[idx].idx_pos = (uint32_t)(frag_raw[14] | (frag_raw[15] << 8) | (frag_raw[16] << 16) | (frag_raw[17] << 24));

            uint8_t frag_flags = frag_raw[3];
            if (frag_flags & 0x40u) {
                /* Intermediate volume */
                if (frag_sz > 24u) {
                    gbuf_append(&stream_buf, frag_raw + 24u, frag_sz - 24u);
                }
            } else {
                /* Final volume: payload slice + compressed index */
                uint32_t final_idx_pos = set->vols[idx].idx_pos;
                size_t final_slice = (final_idx_pos > 24u && final_idx_pos <= frag_sz)
                                   ? (size_t)(final_idx_pos - 24u) : 0u;
                if (final_slice > 0) {
                    gbuf_append(&stream_buf, frag_raw + 24u, final_slice);
                }
                if (decompress_index_section(frag_raw, frag_sz, final_idx_pos, &set->idx_buf) < 0 || !set->idx_buf.data) {
                    fprintf(stderr, "Error: failed to decompress archive index in %s\n", frag_path);
                    free(stream_buf.data);
                    free_archive_set(set);
                    return -1;
                }
                break;
            }
        }

        set->stream_data = stream_buf.data;
        set->stream_size = stream_buf.size;
    }

    set->entries = calloc((size_t)set->n_files, sizeof(FileEntry));
    if (!set->entries || parse_index(set->idx_buf.data, set->idx_buf.size, (int)set->n_files, set->entries) < 0) {
        fprintf(stderr, "Error: corrupt index\n");
        free_archive_set(set);
        return -1;
    }

    return 0;
}

static int cmd_list(const char *arcname, int verbose, int unix_paths, int bare_list,
                    char **patterns, int n_patterns)
{
    ArchiveSet set;
    if (load_archive_set(arcname, &set) < 0) return 1;

    int matched_count = 0;
    if (bare_list) {
        for (int i = 0; i < (int)set.n_files; i++) {
            if (!match_filters(set.entries[i].path, set.entries[i].filename, patterns, n_patterns))
                continue;
            matched_count++;
            const char *p;
            if (unix_paths) {
                p = set.entries[i].path;
            } else {
                p = set.entries[i].raw_dos[0] ? set.entries[i].raw_dos : set.entries[i].filename;
            }
            printf("%s\n", p);
        }
        if (n_patterns > 0 && matched_count == 0) {
            fprintf(stderr, "No matching files found.\n");
            free_archive_set(&set);
            return 1;
        }
        free_archive_set(&set);
        return 0;
    }

    char date_str[32] = "-";
    if (set.arc_ts) {
        time_t t = dos_to_unix_time(set.arc_ts);
        if (t) {
            struct tm *tm = localtime(&t);
            strftime(date_str, sizeof(date_str), "%Y-%m-%d %H:%M:%S", tm);
        }
    }

    const char *mstr = (set.method == 0x11) ? "M1 (Ultra)" :
                       (set.method == 0x12) ? "M2 (Normal)" :
                       (set.method == 0x13) ? "M3 (Fast)" : "M4 (Store)";

    show_banner();
    if (set.n_vols > 1) {
        printf("Archive: %s (%d volumes/fragments)\n", arcname, set.n_vols);
    } else {
        printf("Archive: %s%s\n", arcname,
               set.is_sfx == 2 ? " (Linux SFX Executable)" :
               (set.is_sfx == 1 ? " (DOS SFX Executable)" : ""));
    }
    printf("Method : %s, Files: %u, Created: %s\n\n", mstr, set.n_files, date_str);

    printf("   Original    Packed  Ratio    Date      Time    CRC16   Attr  Name\n");
    printf("  --------- --------- ------ ---------- -------- ------- ------ --------------------\n");

    uint64_t total_orig = 0;

    for (int i = 0; i < (int)set.n_files; i++) {
        if (!match_filters(set.entries[i].path, set.entries[i].filename, patterns, n_patterns))
            continue;
        matched_count++;

        char fdate[16] = "----/--/--";
        char ftime[16] = "--:--:--";
        if (set.entries[i].timestamp) {
            time_t t = dos_to_unix_time(set.entries[i].timestamp);
            if (t) {
                struct tm *tm = localtime(&t);
                strftime(fdate, sizeof(fdate), "%Y-%m-%d", tm);
                strftime(ftime, sizeof(ftime), "%H:%M:%S", tm);
            }
        }

        char csz_buf[16] = "      ---";
        char ratio_buf[16] = "  --- ";
        if (set.n_files == 1 && set.entries[i].comp_size > 0) {
            snprintf(csz_buf, sizeof(csz_buf), "%9u", set.entries[i].comp_size);
            if (set.entries[i].orig_size > 0) {
                double r = 100.0 * (1.0 - (double)set.entries[i].comp_size / set.entries[i].orig_size);
                snprintf(ratio_buf, sizeof(ratio_buf), "%5.1f%%", r);
            }
        }

        const char *display_name;
        if (unix_paths) {
            display_name = (verbose || set.entries[i].path[0]) ? set.entries[i].path : set.entries[i].filename;
        } else {
            display_name = verbose ? (set.entries[i].raw_dos[0] ? set.entries[i].raw_dos : set.entries[i].path)
                                   : set.entries[i].filename;
        }

        printf("%11u %s %s %s %s  0x%04X  --a-  %s\n",
               set.entries[i].orig_size,
               csz_buf,
               ratio_buf,
               fdate,
               ftime,
               set.entries[i].checksum,
               display_name);

        total_orig += set.entries[i].orig_size;
    }

    char tot_csz[32] = "      ---";
    char tot_ratio[32] = "  --- ";
    if (n_patterns == 0 && set.stream_size > 0) {
        snprintf(tot_csz, sizeof(tot_csz), "%9zu", set.stream_size);
        if (total_orig > 0) {
            double r = 100.0 * (1.0 - (double)set.stream_size / (double)total_orig);
            snprintf(tot_ratio, sizeof(tot_ratio), "%5.1f%%", r);
        }
    }

    printf("  --------- --------- ------                            --------------------\n");
    if (n_patterns > 0) {
        printf("%11lu %s %s       %28d file(s) (%d total)\n",
               (unsigned long)total_orig, tot_csz, tot_ratio, matched_count, set.n_files);
    } else {
        printf("%11lu %s %s       %28d file(s)\n",
               (unsigned long)total_orig, tot_csz, tot_ratio, set.n_files);
    }

    free_archive_set(&set);
    return 0;
}

static int cmd_extract_or_test(const char *arcname, const char *outdir,
                               int flat_mode, int test_only, int do_pipe,
                               int unix_paths, char **patterns, int n_patterns)
{
    ArchiveSet set;
    if (load_archive_set(arcname, &set) < 0) return 1;

    int is_stored = ((set.method & 0x0Fu) == 4u);

    uint16_t stream_csum_act = ain_checksum(set.stream_data, set.stream_size);
    uint16_t stream_csum_exp = set.entries[set.n_files - 1].checksum;
    int stream_crc_ok = (stream_csum_exp == 0 || stream_csum_act == stream_csum_exp);

    GrowBuf stream_out = {0};
    if (is_stored) {
        if (set.stream_size > 0) {
            stream_out.data = malloc(set.stream_size);
            if (stream_out.data) {
                memcpy(stream_out.data, set.stream_data, set.stream_size);
                stream_out.size = set.stream_size;
            }
        }
    } else {
        BS bs;
        bs_init(&bs, set.stream_data, set.stream_size);
        if (decompress_stream(&bs, &stream_out) < 0) {
            fprintf(stderr, "Error: decompression failed\n");
            free_archive_set(&set);
            return 1;
        }
        if (!stream_out.data) {
            stream_out.data = malloc(1);
            stream_out.size = 0;
        }
    }

    if (do_pipe) {
        fwrite(stream_out.data, 1, stream_out.size, stdout);
        free(stream_out.data);
        free_archive_set(&set);
        return 0;
    }

    if (test_only) {
        int errors = 0;
        int matched = 0;
        printf("Testing archive %s%s:\n", arcname,
               set.n_vols > 1 ? " (multi-volume)" : "");
        for (int i = 0; i < (int)set.n_files; i++) {
            if (!match_filters(set.entries[i].path, set.entries[i].filename, patterns, n_patterns))
                continue;
            matched++;
            const char *display_name = unix_paths ? set.entries[i].path :
                                      (set.entries[i].raw_dos[0] ? set.entries[i].raw_dos : set.entries[i].path);
            if (!display_name[0]) display_name = set.entries[i].filename;
            if (stream_crc_ok) {
                printf("%-30s OK\n", display_name);
            } else {
                printf("%-30s CRC ERROR\n", display_name);
                errors++;
            }
        }
        if (n_patterns > 0 && matched == 0) {
            fprintf(stderr, "Warning: no matching files found in archive\n");
            errors++;
        }
        printf("%d invalid files\n", errors);
        free(stream_out.data);
        free_archive_set(&set);
        return errors ? 1 : 0;
    }

    /* Extraction */
    if (outdir && strcmp(outdir, ".") != 0) {
        mkdirs(outdir);
    }

    size_t offset = 0;
    int errors = 0;
    int n_extracted = 0;
    int n_matched = 0;

    for (int i = 0; i < (int)set.n_files; i++) {
        uint32_t orig_sz = set.entries[i].orig_size;
        const char *fname;
        if (flat_mode) {
            fname = set.entries[i].filename;
        } else {
            fname = unix_paths ? set.entries[i].path :
                    (set.entries[i].raw_dos[0] ? set.entries[i].raw_dos : set.entries[i].path);
        }

        int selected = match_filters(set.entries[i].path, set.entries[i].filename, patterns, n_patterns);

        if (offset + orig_sz > stream_out.size) {
            if (selected) {
                fprintf(stderr, "%-30s TRUNCATED\n", fname);
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

        char norm_fname[512];
        strncpy(norm_fname, fname, sizeof(norm_fname) - 1);
        norm_fname[sizeof(norm_fname) - 1] = '\0';
        for (char *p = norm_fname; *p; p++) {
            if (*p == '\\') *p = '/';
        }

        char outpath[600];
        if (outdir && strcmp(outdir, ".") != 0) {
            snprintf(outpath, sizeof(outpath), "%s/%s", outdir, norm_fname);
        } else {
            snprintf(outpath, sizeof(outpath), "%s", norm_fname);
        }

        if (!flat_mode) {
            char tmp[600];
            snprintf(tmp, sizeof(tmp), "%s", outpath);
            char *last_slash = strrchr(tmp, '/');
            if (last_slash) {
                *last_slash = '\0';
                mkdirs(tmp);
            }
        }

        printf("Extracting %s (%u bytes)...\n", fname, orig_sz);
        FILE *outf = fopen(outpath, "wb");
        if (!outf) {
            perror(outpath);
            errors++;
        } else {
            if (orig_sz > 0) {
                fwrite(stream_out.data + offset, 1, orig_sz, outf);
            }
            fclose(outf);

            if (set.entries[i].timestamp) {
                time_t t = dos_to_unix_time(set.entries[i].timestamp);
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
        fprintf(stderr, "Warning: no matching files found in archive\n");
        errors++;
    }

    if (n_patterns > 0) {
        printf("%d file(s) extracted (%d matched)%s\n",
               n_extracted, n_matched, errors ? " (with errors)" : " successfully.");
    } else {
        printf("%d file(s) extracted%s\n",
               n_extracted, errors ? " (with errors)" : " successfully.");
    }
    free(stream_out.data);
    free_archive_set(&set);
    return errors ? 1 : 0;
}

static int cmd_exe(const char *exename, const char *outfile)
{
    size_t file_size;
    uint8_t *data = slurp(exename, &file_size);
    if (!data) return 1;

    if (file_size < 64 || data[0] != 'M' || data[1] != 'Z') {
        fprintf(stderr, "Error: not an MZ DOS executable\n");
        free(data); return 1;
    }

    if (memcmp(data + 0x20, "AIN2", 4) != 0) {
        fprintf(stderr, "Error: not an AIN2 self-extracting archive\n");
        free(data); return 1;
    }

    BS bs;
    bs_init(&bs, data + 0x34, file_size - 0x34);
    GrowBuf out = {0};
    if (decompress_stream(&bs, &out) < 0 || !out.data) {
        fprintf(stderr, "Error: SFX decompression failed\n");
        free(data); return 1;
    }

    if (outfile) {
        FILE *f = fopen(outfile, "wb");
        if (!f) { perror(outfile); free(out.data); free(data); return 1; }
        fwrite(out.data, 1, out.size, f);
        fclose(f);
        printf("SFX extracted: %s (%zu bytes)\n", outfile, out.size);
    } else {
        fwrite(out.data, 1, out.size, stdout);
    }

    free(out.data);
    free(data);
    return 0;
}

static void show_usage(const char *prog)
{
    show_banner();
    printf("Usage: %s <command> [options] <archive[.ain/.sfx/.exe]> [files / patterns / dirs...]\n\n", prog);
    printf("Commands:\n");
    printf("  a      Add files to archive\n");
    printf("  x      eXtract files with full directory tree\n");
    printf("  e      Extract files flat (without directories)\n");
    printf("  l      List archive contents (optional pattern filter)\n");
    printf("  v      Verbosely list archive contents with details\n");
    printf("  t      Test archive integrity (verify CRC16)\n");
    printf("  --exe  Decompress AIN2-packed SFX executable -> stdout / file\n\n");
    printf("Options:\n");
    printf("  -m1    Max compression (default)\n");
    printf("  -m2    Normal compression\n");
    printf("  -m3    Fast compression\n");
    printf("  -m4    Store (no compression)\n");
    printf("  -sl[stub], --sfx-linux  Create Linux native self-extracting archive (.sfx)\n");
    printf("  -s[stub], -sdos         Create DOS self-extracting archive (.exe)\n");
    printf("  -f<size> Multi-volume / fragment size (-f1.44, -f720, -f1200, -f360, -f<N>k, -f<N>m)\n");
    printf("  -r     Recurse into subdirectories\n");
    printf("  -o<dir> Output directory for extraction\n");
    printf("  -u, --unix  Display Unix-style paths ('/' separators, lowercase)\n");
    printf("  -1, --bare  Bare listing (one path per line, script/pipe friendly)\n");
    printf("  -p     Pipe extracted stream to stdout\n");
    printf("  -v     Verbose debug output (or AIN_DEBUG=1)\n");
    printf("  -h     Show this help\n\n");
    printf("Examples:\n");
    printf("  %s a archive.ain file1.txt file2.bin\n", prog);
    printf("  %s a -f1.44 floppy.ain file1.txt file2.bin\n", prog);
    printf("  %s a -sl setup.sfx file1.txt file2.bin\n", prog);
    printf("  %s a -s setup.exe file1.txt file2.bin\n", prog);
    printf("  %s a -m1 -r archive.ain myfolder/\n", prog);
    printf("  %s x archive.ain -o extracted/\n", prog);
    printf("  %s x archive.ain -o extracted/ file1.txt \"*.bin\"\n", prog);
    printf("  %s x setup.sfx -o extracted/\n", prog);
    printf("  %s l archive.ain \"*.txt\"\n", prog);
    printf("  %s l setup.sfx\n", prog);
    printf("  %s t setup.sfx file1.txt\n", prog);
}

static int is_switch(const char *arg)
{
    if (!arg || !arg[0]) return 0;
    if (arg[0] == '-') return 1;
    if (arg[0] == '/') {
        if (strchr(arg + 1, '/') != NULL) return 0;
        if (strlen(arg) > 4) return 0;
        if (access(arg, F_OK) == 0) return 0;
        return 1;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    if (getenv("AIN_DEBUG")) g_debug = atoi(getenv("AIN_DEBUG"));

    if (argc < 2) {
        show_usage(argv[0]);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "/?") == 0) {
            show_usage(argv[0]);
            return 0;
        }
    }

    if (strcmp(argv[1], "--exe") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s --exe <packed.exe> [output.raw]\n", argv[0]);
            return 1;
        }
        const char *exename = argv[2];
        const char *outfile = (argc >= 4) ? argv[3] : NULL;
        return cmd_exe(exename, outfile);
    }

    char cmd = 0;
    int method = 1;
    int recursive = 0;
    int sfx_mode = 0;
    const char *sfx_stub_path = NULL;
    size_t vol_size = 0;
    int do_pipe = 0;
    int unix_paths = 0;
    int bare_list = 0;
    const char *outdir = ".";
    const char *arcname = NULL;
    char **file_args = malloc((size_t)argc * sizeof(char *));
    int n_file_args = 0;

    int arg_idx = 1;

    if (!is_switch(argv[arg_idx]) && strlen(argv[arg_idx]) == 1) {
        cmd = (char)tolower((unsigned char)argv[arg_idx][0]);
        arg_idx++;
    }

    for (; arg_idx < argc; arg_idx++) {
        char *arg = argv[arg_idx];
        if (is_switch(arg)) {
            if (strncmp(arg, "--volume=", 9) == 0) {
                vol_size = parse_volume_size(arg + 9);
                continue;
            }
            if (strcmp(arg, "--unix") == 0) {
                unix_paths = 1;
                continue;
            }
            if (strcmp(arg, "--bare") == 0) {
                bare_list = 1;
                continue;
            }
            if (strcmp(arg, "--sfx-linux") == 0 || strcmp(arg, "--sfx") == 0) {
                sfx_mode = 2;
                continue;
            }
            if (strcmp(arg, "--sfx-dos") == 0) {
                sfx_mode = 1;
                continue;
            }
            if (strncasecmp(arg, "-slinux", 7) == 0) {
                sfx_mode = 2;
                if (arg[7] != '\0') sfx_stub_path = arg + 7;
                continue;
            }
            if (strncasecmp(arg, "-sl", 3) == 0) {
                sfx_mode = 2;
                if (arg[3] != '\0') sfx_stub_path = arg + 3;
                continue;
            }
            if (strncasecmp(arg, "-sdos", 5) == 0) {
                sfx_mode = 1;
                if (arg[5] != '\0') sfx_stub_path = arg + 5;
                continue;
            }
            char flag = (char)tolower((unsigned char)arg[1]);
            if (flag == 's') {
                sfx_mode = 1;
                if (arg[2] != '\0') {
                    sfx_stub_path = arg + 2;
                }
            } else if (flag == 'f') {
                if (arg[2] != '\0') {
                    vol_size = parse_volume_size(arg + 2);
                } else if (arg_idx + 1 < argc) {
                    vol_size = parse_volume_size(argv[++arg_idx]);
                }
            } else if (flag == 'u') {
                unix_paths = 1;
                if (arg[2] == '1' || arg[2] == 'b' || arg[2] == 'B') bare_list = 1;
            } else if (flag == '1' || flag == 'b') {
                bare_list = 1;
                if (arg[2] == 'u' || arg[2] == 'U') unix_paths = 1;
            } else if (flag == 'm') {
                if (arg[2] >= '1' && arg[2] <= '4') {
                    method = arg[2] - '0';
                } else if (arg_idx + 1 < argc) {
                    method = atoi(argv[++arg_idx]);
                }
                if (method < 1 || method > 4) method = 1;
            } else if (flag == 'r') {
                recursive = 1;
            } else if (flag == 'p') {
                do_pipe = 1;
            } else if (flag == 'v') {
                g_debug = 1;
            } else if (flag == 'x' && cmd == 0) {
                cmd = 'x';
            } else if (flag == 'e' && cmd == 0) {
                cmd = 'e';
            } else if (flag == 'l' && cmd == 0) {
                cmd = 'l';
            } else if (flag == 't' && cmd == 0) {
                cmd = 't';
            } else if (flag == 'o') {
                if (arg[2] != '\0') {
                    outdir = arg + 2;
                } else if (arg_idx + 1 < argc) {
                    outdir = argv[++arg_idx];
                }
            }
        } else {
            if (!arcname) {
                arcname = arg;
            } else {
                file_args[n_file_args++] = arg;
            }
        }
    }

    if (!arcname) {
        show_usage(argv[0]);
        free(file_args);
        return 1;
    }

    if (has_sfx_extension(arcname)) {
        sfx_mode = 2;
    } else if (has_exe_extension(arcname)) {
        sfx_mode = 1;
    }

    char arcpath[600];
    snprintf(arcpath, sizeof(arcpath), "%s", arcname);
    if (!strrchr(arcpath, '.')) {
        if (cmd == 'a') {
            snprintf(arcpath, sizeof(arcpath), "%s%s", arcname,
                     sfx_mode == 2 ? ".sfx" : (sfx_mode == 1 ? ".EXE" : ".AIN"));
        } else {
            char try_ain[600];
            char try_sfx[600];
            char try_exe[600];
            snprintf(try_ain, sizeof(try_ain), "%s.AIN", arcname);
            snprintf(try_sfx, sizeof(try_sfx), "%s.sfx", arcname);
            snprintf(try_exe, sizeof(try_exe), "%s.EXE", arcname);
            if (access(arcname, F_OK) != 0) {
                if (access(try_ain, F_OK) == 0) {
                    snprintf(arcpath, sizeof(arcpath), "%s", try_ain);
                } else if (access(try_sfx, F_OK) == 0) {
                    snprintf(arcpath, sizeof(arcpath), "%s", try_sfx);
                } else if (access(try_exe, F_OK) == 0) {
                    snprintf(arcpath, sizeof(arcpath), "%s", try_exe);
                } else {
                    snprintf(arcpath, sizeof(arcpath), "%s.AIN", arcname);
                }
            }
        }
    }

    if (cmd == 0) {
        cmd = 'l';
    }

    int ret = 0;
    switch (cmd) {
        case 'a':
            ret = cmd_add(arcpath, method, recursive, sfx_mode, sfx_stub_path, vol_size, n_file_args, file_args);
            break;
        case 'x':
            ret = cmd_extract_or_test(arcpath, outdir, 0, 0, do_pipe, unix_paths, file_args, n_file_args);
            break;
        case 'e':
            ret = cmd_extract_or_test(arcpath, outdir, 1, 0, do_pipe, unix_paths, file_args, n_file_args);
            break;
        case 'l':
            ret = cmd_list(arcpath, 0, unix_paths, bare_list, file_args, n_file_args);
            break;
        case 'v':
            ret = cmd_list(arcpath, 1, unix_paths, bare_list, file_args, n_file_args);
            break;
        case 't':
            ret = cmd_extract_or_test(arcpath, NULL, 0, 1, 0, unix_paths, file_args, n_file_args);
            break;
        default:
            fprintf(stderr, "Unknown command '%c'. Use -h for help.\n", cmd);
            ret = 1;
            break;
    }

    free(file_args);
    return ret;
}
