# ain — Transas AIN 2.32 Archiver (Native C Reimplementation)

Pure C reimplementation by **seb3773** (https://github.com/seb3773/AIN) of the historical **Transas AIN 2.32 archiver** (1993–1996, Transas Marine Ltd.).

> **Reverse Engineering & Legal Notice:** This work was conducted strictly for the purposes of software interoperability and digital data preservation (under European Directive 2009/24/EC) to reconstruct the prediction, compression, and decompression algorithms of an obsolete historical archive format.

This project is a complete, standalone, freestanding C tool combining both **compressor** and **decompressor** into a single binary (`ain`), achieving **100% bidirectional bit-exact compatibility** with the original 16-bit DOS archiver (`AIN.EXE` / `AINEXT.EXE`).

---

## Highlights

- **100% Bidirectional Interoperability**:
  - Archives created by `ain` are recognized, verified (`t`), listed (`l`/`v`), and extracted (`x`) without error by the historical DOS `AIN.EXE` under DOSBox.
  - Archives created by DOS `AIN.EXE` (including multi-file and directory-tree archives) are extracted bit-identically by `ain`.
- **Bit-Exact Historical Compression Engine**:
  - Faithful reimplementation of the original 1993 LZ77 matching algorithms (hash-chain lookups, 16-bit signed arithmetic, sliding window sentinel sweeps, and dynamic canonical Huffman coders).
  - Validated token-by-token and byte-by-byte against execution traces of the original Borland C++ 3.1 DOS binary.
- **Unified Single Binary**:
  - Combines archive creation (`a`), extraction (`x`, `e`), listing (`l`, `v`), integrity testing (`t`), and SFX extraction (`--exe`).
- **Zero External Dependencies**:
  - Clean ANSI C99 / POSIX. Compiles with `-O2 -Wall -Wextra` with zero warnings on modern Linux / Unix systems.

---

## Build

Using the build script:

```sh
./build.sh            # Production release build -> build/ain
./build.sh debug      # Debug build with symbols
./build.sh test       # Run test suite against reference archives
./build.sh deb        # Build standalone Debian .deb package
```

Or using `make`:

```sh
make                  # Compile production binary into build/ain
make test             # Run test suite
make deb              # Build Debian package
sudo make install     # Install binary to /usr/local/bin
```

Or manually:

```sh
mkdir -p build
gcc -O2 -Wall -Wextra -Isrc -o build/ain src/ain.c
```

---

## Usage

```
ain <command> [options] <archive[.ain]> [files / dirs / patterns...]
```

### Commands

| Command | Action | Description |
|---|---|---|
| `a` | **Add** | Create an archive or add files (`-m1`..`-m4`, `-r` for recursion) |
| `x` | **eXtract** | Extract files restoring full directory hierarchies (supports selective file/wildcard patterns) |
| `e` | **Extract flat** | Extract files into destination directory without subdirectories (supports selective patterns) |
| `l` | **List** | List archive contents (filenames, sizes, DOS dates; filterable by patterns) |
| `v` | **Verbose list** | Detailed listing (full paths, packed sizes, ratios, CRC16; filterable by patterns) |
| `t` | **Test** | Verify archive integrity (all files or matching patterns) |
| `--exe` | **Extract SFX** | Decompress AIN2 self-extracting DOS executables (`.exe`) to raw binary |

### Options

| Switch | Description |
|---|---|
| `-m1` | Maximum compression (Ultra, default) |
| `-m2` | Normal compression |
| `-m3` | Fast compression |
| `-m4` | Store (no compression) |
| `-f<size>`, `--volume=<size>` | Create **multi-volume archives** / fragments (`.ain`, `.a01`, `.a02`...) of specified size (e.g. `1.44`, `720`, `50k`, `10m`) |
| `-sl[stub]`, `--sfx-linux` | Create **Linux native self-extracting archive** (`.sfx`, 64-bit ELF, chmod 0755) |
| `-s[stub]`, `-sdos` | Create **DOS self-extracting archive** (`.exe`) using embedded DOS stub (`AINEXT.EXE`) |
| `-r` | Recurse into subdirectories (for `a`) |
| `-o<dir>` | Target output directory for extraction (e.g. `-o extracted/` or `-oextracted/`) |
| `-u`, `--unix` | Format paths as Unix relative paths (`/` slashes, lowercase) |
| `-1`, `--bare` | Bare output format (one path per line, ideal for scripts, `xargs`, `grep`) |
| `-p` | Pipe extracted data stream directly to `stdout` |
| `-v` | Verbose debug output (or environment variable `AIN_DEBUG=1`) |
| `-h` | Display help banner and usage instructions |

### Examples

```sh
# Create an archive with maximum compression (M1)
./build/ain a archive.ain file1.txt file2.bin

# Create a modern Linux self-extracting archive (.sfx, executable out-of-the-box)
./build/ain a -sl setup.sfx file1.txt file2.bin
# or simply by file extension:
./build/ain a setup.sfx file1.txt file2.bin

# Run the Linux SFX archive directly!
./setup.sfx -l                   # List contents
./setup.sfx -t                   # Verify integrity (CRC16)
./setup.sfx -o destination/      # Extract to destination directory

# Create a historical DOS self-extracting archive (.exe, runnable under DOS / DOSBox)
./build/ain a -s setup.exe file1.txt file2.bin

# Create a multi-volume archive (floppy disk split or custom sizes)
./build/ain a -f1.44 backup.ain myfolder/       # 1.44 MB floppy disk fragments (.ain, .a01, .a02...)
./build/ain a -f50k backup.ain myfolder/        # 50 KB fragments

# Multi-volume with Linux SFX (.sfx + .a01, .a02...) or DOS SFX (.exe + .a01, .a02...)
./build/ain a -sl -f100k setup.sfx myfolder/
./build/ain a -s -f1.44 setup.exe myfolder/

# Run the Linux multi-volume SFX binary directly (auto-chains .a01, .a02... in same directory)
./setup.sfx -l
./setup.sfx -o destination/

# Create an archive recursively from a directory
./build/ain a -m1 -r backup.ain myfolder/
./build/ain a -sl -r backup.sfx myfolder/

# List archive contents (transparently handles .ain, multi-volume fragments, Linux .sfx, and DOS SFX .exe)
./build/ain l archive.ain
./build/ain l backup.a01                # Can be invoked directly on any fragment!
./build/ain l setup.sfx
./build/ain l setup.exe

# List archive contents with Unix paths ('/' separators)
./build/ain l -u archive.ain

# Detailed listing with compression ratios, full paths, and CRC
./build/ain v archive.ain
./build/ain v setup.sfx
./build/ain v setup.exe

# Bare listing for scripting and shell pipelines (one file per line)
./build/ain l -1 -u archive.ain | grep '\.c$'

# Or shorthand combined switch (default command is list)
./build/ain -1u archive.ain | xargs -n1 echo "Processing"

# Test archive integrity across all volumes (header CRC, index CRC, stream CRC)
./build/ain t -u archive.ain
./build/ain t setup.sfx
./build/ain t setup.exe

# Extract with full directory tree (works identically for .ain, multi-volume fragments, .sfx, and .exe)
./build/ain x archive.ain -o destination/
./build/ain x backup.a02 -o destination/    # Resolves all volumes automatically
./build/ain x setup.sfx -o destination/
./build/ain x setup.exe -o destination/

# Extract flat (all files directly in destination)
./build/ain e archive.ain -o flat_output/

# Selective extraction and wildcard pattern filtering (works with x, e, l, v, t)
./build/ain l archive.ain "*.txt"               # List only matching files
./build/ain x archive.ain hello.txt             # Extract a single file
./build/ain x archive.ain "*.txt" "*.bin"       # Extract multiple patterns
./build/ain x archive.ain "NESTED/*" -o dest/   # Extract a specific directory subtree
./build/ain e archive.ain "*.txt" -o flat/      # Flat-extract matching files
./build/ain t archive.ain "*.bin"               # Verify integrity of matching files

# Selective operations on Linux native SFX executables
./setup.sfx "*.txt" -o destination/       # Extract matching patterns directly
./setup.sfx -l "*.c"                      # Filtered listing from SFX
./setup.sfx -t "hello.*"                  # Filtered integrity test from SFX

# Decompress an AIN2-packed DOS executable (e.g. AIN.exe SFX)
./build/ain --exe historical_dos/AIN.exe decompressed_binary.bin
```

---

### Self-Extracting Archives (SFX) & Multi-Volume Architecture

`ain` supports dual SFX generation, multi-volume archives, and hardened extraction with **zero external dependencies** and **zero temporary files**:

1. **Linux Native SFX (`.sfx`)**:
   - Prepends a freestanding 64-bit ELF extractor stub ([`ain_sfx_linux_stub.c`](src/ain_sfx_linux_stub.c)) that inspects `/proc/self/exe` to locate the appended AIN payload via ELF Program Headers.
   - Automatically sets executable permissions (`chmod 0755`).
   - Runs out-of-the-box on modern Linux distributions (`./archive.sfx -o dest/`, `./archive.sfx -l`, `./archive.sfx -t`).
   - Supports **multi-volume chaining**: if the archive is split across volumes (`.sfx`, `.a01`, `.a02`...), the standalone SFX binary automatically discovers and chains neighboring `.a01`, `.a02`... files in its directory!
   - Supports selective extraction patterns and path traversal protection.

2. **Historical DOS SFX (`.exe`)**:
   - Prepends the original 16-bit real-mode DOS stub ([`AINEXT.EXE`](AINEXT.EXE), Transas Marine Ltd.).
   - Runs under DOSBox, DOSEMU, FreeDOS, or bare metal MS-DOS.
   - Fully compatible with multi-volume DOS archives (`.exe`, `.a01`, `.a02`...).

3. **In-Memory Stub Compression (Clever Self-Optimization)**:
   - Rather than storing raw executable binaries inside `ain`, both stubs are **pre-compressed with AIN Mode M1** and embedded as C byte arrays in the build pipeline (`Makefile`):
     - Linux stub: **18,808 bytes** $\rightarrow$ **7,846 bytes** (58% smaller)
     - DOS stub: **32,374 bytes** $\rightarrow$ **19,597 bytes** (40% smaller)
     - Total embedded footprint: **51,182 bytes** $\rightarrow$ **27,443 bytes** (-46% space saved in the `ain` binary).
   - When `-s` or `-sl` is invoked, `ain` inflates the required stub in RAM in under a millisecond using its internal `decompress_stream()` engine and streams it directly to the output executable. No files are ever written to `/tmp` or the working directory.

4. **Multi-Volume Fragment System (`.ain` / `.sfx` / `.exe` + `.a01`, `.a02`...)**:
   - Split creation via `-f<size>` supporting standard floppy formats (`360`, `720`, `1.2`, `1.44`, `2.88`), human-readable units (`50k`, `10m`, `1g`), or raw bytes.
   - Continuous solid stream is sliced across volume payloads (`24-byte header + slice`).
   - The compressed index section is appended to the final volume.
   - Volume discovery is transparent: providing either the base archive name or any fragment (`.a01`, `.a02`...) to `ain l`, `ain v`, `ain t`, or `ain x` automatically locates and binds all volumes in the set.

5. **Path Traversal & Zip-Slip Protection**:
   - Both `ain` and the native Linux SFX stub validate all extraction paths with `is_safe_relpath()`.
   - Prevents directory traversal attacks by continuously tracking directory nesting depth: any attempt to ascend above the destination root via `../` (or `..\`) is intercepted and safely rejected with a security warning (`Security warning: skipping unsafe path traversal`).
   - Rejects leading absolute slashes (`/`, `\`) and DOS drive identifiers (`C:`).
   - Automatically sanitizes and normalizes historical DOS backslashes (`DIR\SUBDIR\FILE.TXT`) to forward slashes `/` on POSIX systems, ensuring safe, seamless directory hierarchy recreation.

---

## Archive Format Specifications

Reverse-engineered from `AIN_UNP.EXE` and verified across all historical archives in the test corpus:

### Overall File Structure

```
[0x00 .. 0x17]            24-byte Archive Header
[0x18 .. idx_pos - 1]     Compressed Data Stream (solid stream for all files)
[idx_pos .. EOF]          Compressed Index Stream (M1 Huffman session)
```

### 24-Byte Archive Header

| Offset | Size | Type | Field / Description |
|---|---|---|---|
| `0x00` | 1 | `uint8` | Magic byte (`0x21`) |
| `0x01` | 1 | `uint8` | Method: `0x11` (M1), `0x12` (M2), `0x13` (M3), `0x14` (M4 Store) |
| `0x02` | 1 | `uint8` | Reserved (zeros) |
| `0x03` | 1 | `uint8` | Flags (`0x40`: more volumes / fragments follow in multi-volume archive) |
| `0x04..0x05` | 2 | bytes | Reserved (zeros) |
| `0x06..0x07` | 2 | `uint16 LE` | Volume sequence index (`0` for volume 0, `1` for `.a01`, `2` for `.a02`...) |
| `0x08..0x09` | 2 | `uint16 LE` | Number of files in archive |
| `0x0A..0x0D` | 4 | `uint32 LE` | Archive creation timestamp (DOS format) |
| `0x0E..0x11` | 4 | `uint32 LE` | `idx_pos`: Byte offset where compressed index begins (`0` in intermediate volumes) |
| `0x12..0x13` | 2 | `uint16 LE` | Index checksum: `sum(compressed_index_bytes) & 0xFFFF` (`0` in intermediate volumes) |
| `0x14..0x15` | 2 | bytes | Reserved (zeros) |
| `0x16..0x17` | 2 | `uint16 LE` | Header checksum: `(sum(hdr[0..21]) & 0xFFFF) ^ 0x5555` |

> [!NOTE]
> The historical DOS `AIN.EXE` strictly validates `hdr[22..23]`. If `(sum(hdr[0..21]) ^ 0x5555)` does not match, DOS `AIN.EXE` aborts immediately with `%s is not an AIN archive`. For multi-volume archives, intermediate volumes have flag `0x40` set, `idx_pos = 0`, and the compressed index is appended only to the final volume.

### Index Record Layout (Decompressed)

The index section is itself compressed as a standard Method 1 session. When decompressed, it consists of concatenated records:

| Offset | Size | Type | Field / Description |
|---|---|---|---|
| `0x00` | 1 | `uint8` | Entry flag (`0x20` for first entry, `0x00` for subsequent entries) |
| `0x01..0x04` | 4 | `uint32 LE` | DOS file timestamp |
| `0x05..0x08` | 4 | `uint32 LE` | Original (uncompressed) file size |
| `0x09..0x0C` | 4 | `uint32 LE` | Compressed stream size (`0` for intermediate files, total for last file) |
| `0x0D` | 1 | `uint8` | Flags (`0x18` for first entry, `0x00` for subsequent entries) |
| `0x0E..0x15` | 8 | bytes | Reserved (zeros) |
| `0x16` | 1 | `uint8` | Attribute flags (`0x18` for single file, `0x10` for first, `0x08` for last) |
| `0x17..0x1A` | 4 | bytes | Reserved (zeros) |
| `0x1B..0x1C` | 2 | `uint16 LE` | Stream checksum: `sum(compressed_stream) & 0xFFFF` (on last file) |
| `0x1D..` | var | `string` | NUL-terminated DOS filename/path |
| `EOF` | 1 | `uint8` | One trailing NUL terminator |

---

## Compression & Decompression Algorithms

The core algorithm is a specialized variant of LZ77 coupled with per-block dynamic canonical Huffman coding:

| Parameter | Specification |
|---|---|
| **Sliding Window** | 32,768 bytes (`0x8000`) with a 256-byte (`0x0100`) overrun buffer |
| **Literal / Match Table** | 272 symbols (0..255: literals; 256..271: distance code classes `D0..D15`) |
| **Length Table** | 254 symbols (match length = `symbol + 3`, lengths 3 to 256) |
| **Pre-code Table** | 19 symbols (encodes the Huffman code-length distribution) |
| **End-of-Block (EOB)** | Distance code `D = 15` with extra bits `0x3FFF` (14 ones) |
| **Bitstream Order** | LSB-first bit packer and reader |

### Sliding Window & Hash Chain Authenticity

During compression, the original DOS archiver uses 16-bit signed offsets and slide sentinels (`0x8000` boundary, sentinel sweeping at `0x0000`). To achieve 100% bit-exact parity, `ain` models this exact 16-bit behavior:
- Sliding hash entries when reaching window boundaries.
- Identical match-finding tie-breaking and fast-forward skip loops.
- Replicating the exact historical token streams (`6,229 / 6,229` tokens identical on `image.raw`, `2,037 / 2,037` on `eng.txt`, etc.).

---

## Reverse-Engineering the Historical DOS Binary (`AIN.EXE`)

The original compressor binary was distributed as an AIN2 self-extracting archive (`AIN.EXE`, 43,675 bytes). Because contemporary DOS unpackers (UNP 4.12, X-TRACT 1.51) did not support AIN2 SFX, the binary was extracted through a memory capture technique:

1. **SFX Container Analysis**:
   - MZ DOS header (`0x00..0x1F`) pointing to the real-mode stub at the end of the file.
   - `AIN2` signature and relocation header at `0x20..0x33`.
   - Compressed executable stream starting at file offset **`0x34`** (52 decimal).
2. **Memory Capture via `/proc/<pid>/mem`**:
   - Pausing DOSBox while `AIN.EXE` displayed its shareware screen (*"Press Enter to continue"*).
     *(Ironic historical twist: the ubiquitous shareware nag screen that annoyed so many users in the 1990s—hardly anyone had registered copies back then—turned out to be the golden ticket for reverse engineering! Because the SFX stub had fully decompressed the payload into memory before halting to display the prompt, the raw binary sat frozen and intact in RAM, waiting to be dumped.)*
   - Scanning the DOSBox emulated 1 MB conventional memory block.
   - Extracting the decompressed binary and reconstructing a clean MZ executable header ([`dump_dosbox_sfx.py`](_work/traces/dump_dosbox_sfx.py)).
3. **Disassembly & Reconstruction**:
   - Disassembling the resulting 78,640-byte binary ([`AIN_UNP.EXE`](AIN_UNP.EXE)) with `ndisasm` into [`ain_unpacked_disasm.txt`](_work/docs/ain_unpacked_disasm.txt).
   - Mapping out the compressor's data structures, hash functions, and block boundary heuristics.

---

## Validation & Test Corpus: 100% Bit-Exact Reproducibility

Every archive mode (M1 Ultra, M2 Normal, M3 Fast, M4 Store) achieves **100% bit-for-bit and token-for-token identity** matching the original 1993 Transas DOS `AIN.EXE` byte-by-byte:

| Corpus File | Mode | Tokens Decoded | Token Match | Payload Byte Match |
|---|---|:---:|:---:|:---:|
| `eng.txt` | **M1** (Ultra) | 2,037 / 2,037 | **100% Identical** | **EXACT (4,222 B)** |
| `image.raw` | **M1** (Ultra) | 6,229 / 6,229 | **100% Identical** | **EXACT (12,313 B)** |
| `hello.txt` | **M1** (Ultra) | 59 / 59 | **100% Identical** | **EXACT (69 B stream)** |
| `eng.txt` | **M2** (Normal) | 2,084 / 2,084 | **100% Identical** | **EXACT (4,191 B)** |
| `mixed.bin` | **M2** (Normal) | 5,134 / 5,134 | **100% Identical** | **EXACT (5,233 B)** |
| `hello.txt` | **M2** (Normal) | 59 / 59 | **100% Identical** | **EXACT (69 B stream)** |
| `eng.txt` | **M3** (Fast) | 3,662 / 3,662 | **100% Identical** | **EXACT (5,491 B)** |
| `image.raw` | **M3** (Fast) | 15,877 / 15,877 | **100% Identical** | **EXACT (23,821 B)** |
| `mixed.bin` | **M3** (Fast) | 5,136 / 5,136 | **100% Identical** | **EXACT (5,431 B)** |
| `zeros.bin` | **M3** (Fast) | 783 / 783 | **100% Identical** | **EXACT (973 B)** |
| `hello.txt` | **M3** (Fast) | 61 / 61 | **100% Identical** | **EXACT (69 B stream)** |
| `eng.txt` | **M4** (Store) | 100,000 / 100,000 | **100% Identical** | **EXACT (20,429 B)** |
| `zeros.bin` | **M4** (Store) | 100,000 / 100,000 | **100% Identical** | **EXACT (200,072 B)** |
| `hello.txt` | **M4** (Store) | 100,000 / 100,000 | **100% Identical** | **EXACT (68 B stream)** |

Multi-file archives and full directory trees also extract and test with 100% round-trip fidelity:
```
PASS  NESTED.AIN  (7 files, directory tree, 895,374 bytes)
PASS  CORPACK.AIN (13 files, multi-file corpus, 1,136,674 bytes)
```

Both extraction and creation cross-verified under DOSBox with Transas `AIN.EXE`:
```
AIN 2.32  Copyright (c) 1993-96  Transas Marine Ltd.
0 invalid files
```

### The Mode M2 3-Way Loop Breakthrough

Achieving 100% bit-exact parity on Mode M2 required solving a subtle artifact of 16-bit Borland C++ optimization:
1. **3-Way Loop Unrolling (`D330` & `D420`)**:
   In the DOS binary, the inner search routines are unrolled into blocks of 3 candidates. Only Candidate 1 checks `jl exit` on `dx < 0`. Candidates 2 and 3 decrement `dx` and execute the word equality check without an exit test. When `max_chain` (32) depletes, Candidate 2 tests at `dx = -1` and Candidate 3 tests at `dx = -2`.
2. **Early Match Offset Filter (`si = best - 1`)**:
   Once an initial match of length $L \ge 2$ is found, the word comparison offset shifts to $si = L - 1$. Candidates that do not match at this boundary are rejected in 3 CPU cycles without string scanning.
3. **Exact Fallback Preservation**:
   Preserving both the 3-way unrolling and the exact `D3CC` / `D40C` transition rules enabled all 2,084 tokens on `eng.txt` and all 5,134 tokens on `mixed.bin` to match the historical DOS output byte-for-byte.

---

## File Structure

- [`build/ain`](build/ain) — Compiled unified native binary (compressor + decompressor + SFX + multi-volumes).
- [`src/ain.c`](src/ain.c) — Unified native C archiver source.
- [`src/ain_sfx_linux_stub.c`](src/ain_sfx_linux_stub.c) — Standalone Linux ELF x86_64 SFX extractor stub source.
- [`src/ain_sfx_linux_stub.h`](src/ain_sfx_linux_stub.h) — Embedded Linux ELF SFX extraction stub (AIN M1 compressed).
- [`src/ain_sfx_stub.h`](src/ain_sfx_stub.h) — Embedded 16-bit DOS SFX extraction stub (`AINEXT.EXE`, AIN M1 compressed).
- [`historical_dos/`](historical_dos/) — Original historical DOS archiver binaries (`AIN.exe`, `AINEXT.EXE`, `AIN_UNP.EXE`, `AUTHOR.NFO`).
- [`build.sh`](build.sh) — One-click production / debug build script.
- [`build_deb.sh`](build_deb.sh) — Standalone Debian `.deb` packaging script.
- [`FORMAT.md`](FORMAT.md) — Comprehensive technical specification of the AIN 2.32 format, algorithms, and engine.
- [`Makefile`](Makefile) — Build configuration (zero external dependencies).
- [`corpus/`](corpus/) — Uncompressed test files.
- [`packed_corpus/`](packed_corpus/) — Reference archives packed with historical DOS AIN.
- [`_work/`](_work/) — Reverse-engineering research materials, disassembly, dynamic traces, and experimental scripts.

---

## License

Historical format and original DOS binary copyright (c) 1993–1996 Transas Marine Ltd.  
Clean-room reverse-engineered C reimplementation created for interoperability and digital preservation.
