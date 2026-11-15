# Encoder performance work — bench, method, results

Golden rule: the encoder output must stay **bit-for-bit identical** to the
legacy encoder (commit `3ab90bc`). Every change below was accepted only if it
passed the bit-exact gate **and** showed a real gain (above the measured noise
floor, or a deterministic instruction/cache-miss reduction confirmed by
timing). Machine: x86-64 with AVX-512, gcc 15.3, `-O3 -flto` (no `-march=native`).

## Tools (`tests/perf/`)

| Tool | What it does |
|---|---|
| `gate.sh [REV] [--timing]` (`make perf-gate`, `make perf-timing`) | Builds the legacy encoder from git `REV` (default `3ab90bc`) and 5 candidate variants, runs everything below. |
| `bitexact.py REF CAND` | 635 checks: `packed_corpus/` DOS streams, every `corpus/` file + ~120 generated edge-case files (sizes 0..513, 0x7FFF..0x20101 window/wrap boundaries, small alphabets, distances 1..0x10000, match lengths 254..513, pad-sensitive tails) × M1..M4, multi-file archives (`-r` too), round-trip, and **decoder** equality on every historical archive. Only the archive timestamp and its header CRC are masked. |
| `gen_corpus.py DIR` | The deterministic edge-case corpus used above. |
| `timing.py REF CAND` | Interleaved A/B wall-clock bench, lower-quartile statistic, bootstrap 95% CI, verdict GAIN/LOSS/noise (±1% threshold on the CI). Payloads: `rep_*` (corpus files replicated, as in `bench_encode.sh`), `mk_text`/`mk_code` (order-4 Markov text — realistic, non-repetitive), `mut_corpus` (mutated corpus copies), `many_small` (~140 files of 1..60 KB in one archive), `--extra FILE` for your own data. |
| `icount.py REF CAND` | Noise-free A/B under cachegrind: instructions, D1/LL misses, estimated cycles. Used to judge small changes. |
| `AIN_VERIFY_FASTPATH` build flag | Every M1/M2 match search runs twice (legacy walk, then accelerated) and aborts on any difference in `(cur_len, cur_pos)` — a per-call equivalence proof on any input. `gate.sh` runs it on the whole corpus and on 2.5 MB payloads, also with a tiny 4-set index that forces the fallback paths. |

Noise floor on this (shared) machine: ±5–10 % per row for wall-clock, so
gains below that are only accepted with a matching `icount.py` reduction.

## Profile of the legacy encoder

`lz12_d318_dx` (M1/M2 chain walk) dominated: 82 % of M1, 72 % of M2. Hop
counters showed *why*: on files > 64 KB, 75–85 % of all chain hops are the
`si = 0` walk (first word equal to the word at `pos_emit`) with a **hit rate
below 1 %** (e.g. 199 M hops for 0.84 M hits on 4 MB of Python source).
For files < 64 KB the same walk is short; there `lz12_d420` dominates.

## Accepted changes (in order)

| # | Change | Effect (A/B vs previous step) |
|---|---|---|
| 1 | **Exact "same-pair" skip index for the M1/M2 walks** (`struct M12FastIdx`). For every inserted node: `kn` = most recent older node with the same (hash bucket, first word) pair, `cnt` = per-bucket insert ordinal. A D318 `si=0` walk can only hit nodes of pos_raw's pair, so the first hit is `kn[pos_raw]`; its hop index `cnt[p]-cnt[t]` replays the DOS `dx` budget exactly (including "only hop 1 of each 3-hop group tests dx"). Chain breaks add a counter gap > any budget; reachability uses absolute ages. The pair map is an 8-way set-associative cache; if a live entry was evicted the node is `KN_UNKNOWN` and the legacy loop runs (exact by construction). | M1 ×2 on large files |
| 2 | Same index for the D318 `si>0` phase and the D2C0→D330 entry: an *improving* node must match `best+1` bytes, hence belongs to the pair; other filter hits only reset the 3-hop phase, which can change reachability only when the candidate sits at hop D+1/D+2 → then the legacy loop replays that entry. | M1 ×3 total |
| 3 | Same for **D420** (anchor = the node whose chain is walked: `pos_raw - si`, `end_slot`, `sec_slot`, ...; improving hits share the anchor's pair because `-si+1 < best`). Not used when `best >= 256` (then any hit returns). | M1 ×3.5, M2 ×1.3 |
| 4 | SSE2 tag compare / free-way search in the pair map (scalar fallback for non-SSE2 targets). | M1/M2 +10–17 % |
| 5 | Bit writer: 64-bit accumulator flushed 32 bits at a time; distance class by bit-scan; class code + extra bits in one write. | M3 +17 %, M1 +9 % |
| 6 | Huffman lengths: O(n²) insertion sort → stable LSD radix (identical permutation: (freq, sym) is a total order); stack buffers instead of 4–5 mallocs per call; tree clear limited to the written high-water mark. | M3 −3..11 % instructions |
| 7 | M3: branch-free slide sweep (now vectorised `pmaxsw`), 32-bit 4-byte candidate compare. | M3 −6..22 % instructions |
| 8 | Legacy walk loops (used for files < 64 KB and fallbacks): bounds checks reduced to the provably equivalent forms, thanks to the zero padding (`dist ∈ [1,0x7FFF] && dist <= pos_emit+si`, padded word compare; D420: `si <= 0` ⇒ `cand_s+1 < sz` always). | −9 % instructions on small files |
| 9 | Encoder work buffers (LZ state, tokens, padded copy, index) allocated once and reused across files: after step 6 glibc started trimming/re-growing the heap for every file (2× kernel time on archives of small files). | many_small M1/M2 back above legacy |

## Rejected (measured, not kept)

* Software prefetch of the pair-map set 8/16/32 inserts ahead: no gain (one row −18 %), removed.
* Index enabled for files < 64 KB: up to 2× *more* instructions — with a correct hash the bucket ≈ the pair, so there is nothing to skip; threshold `M12F_MIN_SIZE = 64 KB` kept (exactness does not depend on it; the gate forces 0).
* PGO (`make pgo`) on top of the final code: within noise on every row.

## Final result (legacy `3ab90bc` vs this tree, 4 MB payloads, q25 of 7 interleaved runs)

| payload | M1 | M2 | M3 |
|---|---|---|---|
| mk_text (Markov English) | 7.10× | 2.06× | 1.73× |
| mk_code (Markov C) | 8.26× | 1.55× | 1.37× |
| mut_corpus | 2.95× | 1.58× | 1.36× |
| corpus (replicated) | 2.93× | 1.73× | 1.41× |
| rep_biased | 7.21× | 1.80× | 1.84× |
| rep_image24 | 1.96× | 2.04× | 1.20× |
| rep_random | 2.13× | 2.25× | 1.25× |
| rep_records | 2.37× | 1.77× | 1.40× |
| many_small (140 files) | 1.02× (noise) | 1.05× | 1.29× |
| Python stdlib sources (real) | 4.14× | 1.65× | 1.61× |
| libvpx.so slice (real) | 3.05× | 1.25× | 1.44× |
| **sum over payloads** | **4.33×** | **1.67×** | **1.43×** |

M4 (store) is I/O only and unchanged. Builds checked: gcc 15 (`-O3 -flto`),
clang (zig cc), Windows x86-64 (zig cc mingw target), scalar (`-U__SSE2__`);
all bit-exact where runnable.

## Finding outside the scope (not changed — would break bit-exactness)

For M1/M2 the rolling hash reads its byte through `lz12_raw()`, which maps the
16-bit DOS address back with `(uint16_t)(addr - 0x100)` — i.e. **after the first
64 KB of a file the hash is computed from the bytes of the first 64 KB**, not
from the current position. Matches are still verified byte by byte, so
archives decode correctly, but chains become noise:

| input (M1) | legacy | hash on real bytes |
|---|---|---|
| 4 MB Python sources | 2,151,208 B | 979,016 B |
| 4 MB libvpx.so | 2,464,611 B | 1,435,483 B |
| corpus/records.bin (240 KB, M2) | 169,672 B | 158,879 B |
| `packed_corpus/RECORDS.AIN` (DOS, M2) | — | **158,903 B** |

So above 64 KB, M1 ("max") compresses ~2× worse than M3, and the legacy
encoder probably does not match DOS `AIN.EXE` there (RECORDS.AIN is 25 bytes
from the corrected-hash output, 10.7 KB from the legacy one). Fixing it needs
a DOSBox trace at the 64 KB boundary and a new golden reference; with a
correct hash, chains are short again and the legacy walks get faster by
themselves (the index would then matter much less).
