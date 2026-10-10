# HISAT2 — patched, cross-platform build (work in progress)

A minimal patched copy of [**HISAT2**](https://github.com/DaehwanKimLab/hisat2)
(2.2.3 / git-master base) that builds and runs natively on **Linux, macOS
(Intel + Apple Silicon), and Windows (x64 + ARM64)**, and reads
**gzip- and bzip2-compressed FASTQ directly** — no Perl wrapper, no `gzip.exe`,
no temp files.

For HISAT2's documentation, options, and canonical source, see the upstream
repository: <https://github.com/DaehwanKimLab/hisat2>. **This README covers only
how to build this tree and what was changed** — everything else is unchanged
from upstream.

> The **git-master** base matters: the `hisat2-2.2.x` release *tarball* has an
> old Makefile with no ARM support (it emits `-msse2` unconditionally and fails
> on ARM64). Master carries the arch-gated SIMD path (`third_party/sse2neon.h`)
> this tree relies on.

---

## Building from source

HISAT2 links only `-lpthread` and (now) `-lz` and `-lbz2`; there is no separate library to
build first. Run every command **from this directory** (the one containing
`Makefile`). On Windows the build is **native MinGW** — *not* MSVC, and *not*
the Cygwin-style "MSYS2 MSYS" shell.

### Toolchain / shell per platform

| Platform | Shell / prompt | CC / CXX | SIMD | C++ runtime | `ARCH` |
|---|---|---|---|---|---|
| Linux x64 | your terminal | `gcc` / `g++` | SSE2 | libstdc++ | *(auto)* |
| Linux ARM64 | your terminal | `gcc` / `g++` | NEON | libstdc++ | *(auto)* |
| macOS Intel | Terminal | `clang` / `clang++` | SSE2 | libc++ | *(auto)* |
| macOS Apple Silicon | Terminal | `clang` / `clang++` | NEON | libc++ | *(auto)* |
| Windows x64 | **MSYS2 UCRT64** | `gcc` / `g++` | SSE2 | libstdc++ | *(auto)* |
| Windows ARM64 | **MSYS2 CLANGARM64** | `clang` / `clang++` | NEON | libc++ | **`aarch64`** |

The Makefile picks the SIMD path from `ARCH ?= $(shell uname -m)`. On every
platform except **Windows ARM64** that is correct automatically. See that
section for why it must be forced there.

### Linux — x86-64 / ARM64

```bash
sudo apt install build-essential make zlib1g-dev libbz2-dev   # Debian/Ubuntu
make
```

The `ARCH` detection selects SSE2 on x86-64 and NEON on aarch64 with no extra
flags. This builds the `-s`/`-l` (small/large-index) align, build, and inspect
binaries plus `hisat2-repeat`.

### macOS — Intel / Apple Silicon

zlib and libbz2 ship with the system SDK; Apple clang is fine.

```bash
make
```

Apple Silicon is detected as `arm64` and takes the NEON path automatically.

### Windows — x64  (MSYS2 **UCRT64** shell)

Install MSYS2 (<https://www.msys2.org>) and open the **"MSYS2 UCRT64"** shell
(*not* "MSYS2 MSYS"). The build tool is `mingw32-make`.

```bash
pacman -S --needed \
  mingw-w64-ucrt-x86_64-gcc  mingw-w64-ucrt-x86_64-make \
  mingw-w64-ucrt-x86_64-zlib mingw-w64-ucrt-x86_64-bzip2 mingw-w64-ucrt-x86_64-git

mingw32-make EXTRA_FLAGS="-static -static-libgcc -static-libstdc++"

strip hisat2.exe hisat2-build.exe hisat2-inspect.exe \
      hisat2-align-s.exe hisat2-align-l.exe hisat2-build-s.exe hisat2-build-l.exe \
      hisat2-inspect-s.exe hisat2-inspect-l.exe hisat2-repeat.exe
```

Every binary reads bzip2 input, and the three launchers (`hisat2.exe`,
`hisat2-build.exe`, `hisat2-inspect.exe`; see below) also write it, which is why
bzip2 is needed. `-static -static-libgcc -static-libstdc++` folds the
GCC/libstdc++/winpthread, zlib **and bzip2** runtimes into each `.exe`, so nothing but stock Windows DLLs is
needed on a clean machine. The Makefile adds its own `-std=c++11` and (x64)
`-DPOPCNT_CAPABILITY` to a command-line `EXTRA_FLAGS` (`override`), so the
static recipe keeps the POPCNT path. The Makefile builds with `-g3`; stripping
removes ~90% of the size.

This static build is the recipe for a self-contained copy. The ngs-tools
installer builds **dynamically** instead (plain `mingw32-make <target>`, no
`-static`) and ships the toolchain, zlib and bzip2 DLLs (`libstdc++-6.dll`,
`zlib1.dll`, `libbz2-1.dll`, ...) next to the executables; see
"Redistribution & licensing" below.

### Windows — ARM64  (MSYS2 **CLANGARM64** shell)

Open the **"MSYS2 CLANGARM64"** shell.

```bash
pacman -S --needed \
  mingw-w64-clang-aarch64-clang mingw-w64-clang-aarch64-make \
  mingw-w64-clang-aarch64-zlib  mingw-w64-clang-aarch64-bzip2 \
  mingw-w64-clang-aarch64-git \
  mingw-w64-clang-aarch64-gcc-compat

mingw32-make ARCH=aarch64 EXTRA_FLAGS="-static"

strip hisat2.exe hisat2-build.exe hisat2-inspect.exe \
      hisat2-align-s.exe hisat2-align-l.exe hisat2-build-s.exe hisat2-build-l.exe \
      hisat2-inspect-s.exe hisat2-inspect-l.exe hisat2-repeat.exe
```

**`ARCH=aarch64` is mandatory here (the emulation trap).** Even in the
CLANGARM64 shell, MSYS2's own `bash`/`make`/`uname` frequently run as *emulated
x86_64*, so `uname -m` reports `x86_64` and the Makefile would pick the x86 SIMD
path — while clang targets aarch64 — giving `error: unsupported option '-msse2'`.
The CLI override wins (`ARCH ?=`). `gcc-compat` supplies the `g++` the Makefile
invokes; the GCC-specific `-static-lib*` flags don't apply under clang/libc++, so
plain `-static` is correct.

### Verify (any platform)

```bash
./hisat2-build-s ref.fa idx
./hisat2-align-s -x idx -U reads.fq.gz -S out.sam    # .gz read directly — no wrapper, no temp
grep -c -v '^@' out.sam                              # expect the mapped-read count
```

Running `hisat2-align-s`/`-l` **directly** is the point of the native-gzip patch:
`.gz` (and plain) reads — from files **or piped on stdin (`-U -`)** — stream in
with no `hisat2` wrapper, no `gzip.exe`, and no intermediate decompressed file.
bzip2 read files stream in the same way; they are recognised by their first
bytes, not their name. A pipe (stdin or a named pipe) is read as gzip or plain
only, because it cannot be rewound after its first bytes are checked.
The binaries are self-contained — check with `ldd` (Linux/Windows) or `otool -L`
(macOS); only system libraries should appear.

### The `hisat2`, `hisat2-build` and `hisat2-inspect` launchers

Upstream runs the aligner and index tools through wrapper scripts: `hisat2` is
**Perl**, `hisat2-build` and `hisat2-inspect` are **Python**. On Windows a script
is not a runnable command (cmd, Explorer and other programs only start `.exe`
files), so on Windows the Makefile builds native launchers instead:
`hisat2.exe`, `hisat2-build.exe` and `hisat2-inspect.exe`, all from
[`hisat2_launcher.c`](hisat2_launcher.c). They need no Perl, Python, `gzip.exe`
or PowerShell, and are used exactly like the scripts:

```bat
hisat2 -x <index> -1 r1.fq.gz -2 r2.fq.gz -S out.sam
hisat2 -x <index> -U reads.fq.gz --un-gz unaligned.fq -S out.sam
hisat2-build ref.fa <index>
hisat2-inspect -s <index>
```

Like the scripts, each launcher picks the small- or large-index binary
(`-s`/`-l`, or the `-debug` variant with `--debug`), adds `--wrapper basic-0`,
and runs the binary from its own directory. `hisat2` also does what the Perl
wrapper does on top: the read-length check (`--read-lengths`, from the first
10000 reads; `--skip-read-lengths` turns it off), the `--un` / `--al` /
`--un-conc` / `--al-conc` / `--al-conc-disc` outputs (plain, `-gz` or `-bz2`)
and `--no-unal` through the binary's `--passthrough` mode, the read-file and
index checks, and the same `(INFO)` / `(ERR)` messages and exit codes. As with
the script, `--` splits the command line: the arguments before it are for the
wrapper only (read files given as `-U`/`--reads`/`-1`/`-2`, which are not split
at commas there, `--verbose` and `--log-file`; anything else before `--` is
ignored) and those after it go to the binary. With no arguments, `-h` or
`--version` the aligner prints its own usage or version, as with the script.

The launchers were compared with the scripts on 50 command lines
([`tests/`](tests/README.md); `tests/run_parity.sh` reruns it), covering
those options, gzip, bzip2 and FASTA input, stdin, small and large indexes, every
error path and index building. Output files, stdout, stderr and exit codes are
identical, except for these deliberate differences:

- `--verbose` prints the `(INFO)` lines whenever it is given. The Perl wrapper
  drops them as soon as a read file is on the command line (an option-parsing
  quirk).
- Gzip and bzip2 read files are passed to the binary as they are, because it
  reads them natively. The Perl wrapper decompresses them through a temporary
  pipe, so only the command line recorded in the SAM `@PG` header differs.
- Paths containing spaces work. The Perl wrapper runs the binary through an
  unquoted shell command line, which splits them.
- `--un=<file>`, `-S<file>` and `--output=<file>` forms are honoured; the Perl
  wrapper mishandles some of them.
- When a binary cannot be started (e.g. `--debug` without a `-debug` build), the
  error comes from the launcher rather than from `sh`.
- `--ref-string` / `--reference-string` (index from a sequence on the command
  line) is not supported: the launcher stops with an `(ERR)` message. The Perl
  wrapper builds a temporary index with `hisat2-build` from `PATH`.
- A failed write or close of an output the launcher writes itself (`-S` with
  `--un*`/`--al*`/`--no-unal`, and those read files) stops it with
  `(ERR): Could not write ...` and a nonzero exit code (the system error
  number, as for its other errors). The Perl wrapper ignores such errors, so a
  full disk gives truncated files and exit code 0.
- On Windows, if the aligner crashes, the launcher reports the Windows
  exception code (`(ERR): hisat2-align died with exception 0xC0000005 (access
  violation)`) and exits with it. If the launcher itself is killed (e.g. by a
  GUI's Cancel, `TerminateProcess`), the binary it started is killed too (job
  object). On Linux/macOS signals are reported as by the script.

On Linux and macOS the upstream scripts are used as before (the launchers can
be built there too, e.g. for testing: `cc -DLAUNCH_ALIGN -o hisat2
hisat2_launcher.c -lbz2 -lz`).

### Known limitations on Windows

- **Very many distinct read lengths.** `--read-lengths` lists every distinct
  length among the first 10000 reads, and a Windows command line is limited to
  32767 characters. Ordinary (even trimmed) short reads have at most a few
  hundred distinct lengths, well within the limit; a read file with thousands of
  different lengths can make the launcher fail to start the binary. Use
  `--skip-read-lengths` there.

- **`hisat2-build` reads plain FASTA.** Reference gzip streaming is not wired up
  (only the read/aligner path is); pass an uncompressed reference, or ship a
  prebuilt index.
- **Non-ASCII paths** (outside the Windows ANSI code page) need a UTF-8
  application manifest next to each `.exe`, because the binaries use the
  narrow-character file APIs. The ngs-tools installer embeds one in every
  executable (declaring `<activeCodePage>UTF-8</activeCodePage>`, Windows 10
  1903+); without it such paths are misread. A side-by-side `<tool>.exe.manifest`
  only works for a build that embeds no manifest of its own; UCRT64 GCC embeds a
  default one, so Windows ignores the side-by-side file there.

---

## Patches (vs upstream 2.2.3)

Windows text/line-ending fixes are `#ifdef _WIN32`-guarded; the `auto_ptr` and
native-gzip changes are correct on every platform and are deliberately
unguarded. Output line-ending fixes use `"wb"` / `ios::binary`, which are no-ops
on Linux/macOS, so they too are portable and applied unconditionally.

| File | Change | Reason |
|------|--------|--------|
| `gfm.h`, `hisat2.cpp`, `hisat_bp.cpp` | `std::auto_ptr` → `std::unique_ptr` | `auto_ptr` was removed in C++17 and is gone from newer libc++ (Windows CLANGARM64: `use of undeclared identifier 'auto_ptr'`). Drop-in here (only `.get()` used); future-proofs libstdc++ too. |
| `filebuf.h` | `OutFileBuf` opens output files `"wb"` (all three ctor/`setFile` paths) | Windows text mode rewrites `\n`→`\r\n`; CRLF in a SAM breaks the `hisat2 \| samtools view -b -` pipe (a stray `\r` glues onto the last field). |
| `hisat2_main.cpp`, `hisat2_build_main.cpp`, `hisat2_repeat_main.cpp`, `hisat2_inspect.cpp` | `#ifdef _WIN32`: `_setmode(_fileno(stdout/stderr), _O_BINARY)` at the top of `main()` (+ `<io.h>`/`<fcntl.h>`/`<stdio.h>`) | The piped SAM on stdout and all text logs must be LF-only for the same reason; the pre-opened std streams need `_setmode` even after files are handled. |
| `hisat2_main.cpp` | `#ifdef _WIN32`: also `_setmode(_fileno(stdin), _O_BINARY)` | A `.gz` (or plain) stream piped on stdin (`-U -`) must be read as raw binary, else text-mode CRLF translation corrupts the compressed bytes. |
| `hisat2.cpp` | Alignment-summary and novel-splice-site `ofstream` opened `ios::binary` | Same CRLF fix for the two remaining text sidecar files the aligner writes. |
| `repeat_builder.cpp` | `.rep.*.seed` and the `saveRepeats` outputs opened with `ios_base::binary` | Same CRLF fix for `hisat2-repeat`'s text outputs. |
| `gzip_reader.h` / `gzip_reader.cpp` *(new)* | A thin zlib/libbz2 wrapper (`gzr_open`/`gzr_dopen`/`read`/`rewind`/`close`) behind an opaque handle. `gzr_open` checks a rewindable file's first bytes for the bzip2 header and reads such a file through libbz2 (concatenated streams, as `bzip2 -dc`); everything else goes through zlib. `<zlib.h>` and `<bzlib.h>` are confined to this one translation unit; the header is C/C++-portable (`extern "C"` under `__cplusplus`). `gzr_read` fills its buffer in a loop and checks `gzerror` at end-of-stream. | **Adds native streaming gzip and bzip2 input** with no header-wide zlib/libbz2 dependency. The fill-loop + `gzerror` check is required: a truncated/corrupt stream must fail loudly, not be silently treated as EOF (which would drop the tail of the input and still exit 0). `gzr_dopen` (dup + `gzdopen`) extends the same path to a piped stdin. |
| `filebuf.h` | `FileBuf` gains a 4th backend (`void* _gz`) alongside `FILE*`/`ifstream`/`istream`, wired at the single `peek()` refill point plus `init/isOpen/close/reset/newGzFile`; the `get()`/`peek()` "a stream is open" assertions accept it | Routes read input through the zlib reader without disturbing any other `FileBuf` user (index/reference readers keep the untouched `fread` path). |
| `pat.h` | `PatternSource::open()` opens read files via `gzr_open`, and stdin (`-U -`) via `gzr_dopen(fileno(stdin))`; zlib reads gzip **and** plain transparently | Single choke point: gzip decompression happens inside the aligner, so `.gz` reads (from files *or* stdin) need no external step. Only this call site changed. |
| `Makefile` | `gzip_reader.cpp` added to `SHARED_CPPS`; `-lbz2 -lz` added to `LIBS` | Compile/link the reader into every binary. `-static` folds `libz.a` and `libbz2.a` in on Windows. |
| `Makefile` | Disable `BOWTIE_MM`/`BOWTIE_SHARED_MEM` when `$(OS)==Windows_NT` | Upstream only disables them when `uname` contains `MINGW`, which misses MSYS2 **CLANGARM64** (`uname`→`CLANG*_NT`) and any build driven from an MSYS/SSH shell (`MSYS_NT`); otherwise `reference.h`/`gfm.h` pull `<sys/mman.h>` (absent under MinGW) and the build fails. Keying off the shell-independent `$(OS)` fixes it. Verified on the ARM64 VM. |
| `hisat2_launcher.c` *(new)* | Native C versions of the `hisat2` (Perl), `hisat2-build` and `hisat2-inspect` (Python) wrapper scripts, one source compiled per tool (`-DLAUNCH_ALIGN/BUILD/INSPECT`); the align launcher links zlib and libbz2 | On Windows a script cannot be run as a command, and the scripts also need Perl/Python and `gzip.exe`. The launchers build the same binary command lines and print the same messages (see "The launchers" above). |
| `Makefile` | On `$(OS)==Windows_NT`, build `hisat2.exe`, `hisat2-build.exe`, `hisat2-inspect.exe` instead of upstream's `hisat2.bat` / `hisat2-build.bat` / `hisat2-inspect.bat`; `hisat2-bin` zips them without adding a second `.exe` | The `.bat` files only called `perl` / `python` on the scripts. |
| `Makefile` | `BUILD_HOST ?= $(shell hostname)`, passed to `-DBUILD_HOST` (was a fixed `` `hostname` ``) | `--version` prints "Built on <host>". Distributed builds pass `BUILD_HOST=<neutral name>` (the ngs-tools installer uses `ngs-tools`) so the binaries do not carry the build machine's name. Default output unchanged. |
| `Makefile` | `override EXTRA_FLAGS += -std=c++11` and `override EXTRA_FLAGS += -DPOPCNT_CAPABILITY` | A command-line `EXTRA_FLAGS` (the Windows `-static` recipe) replaced both, silently dropping the x64 POPCNT path. |

### Patch layout and maintenance

All portability source and build changes are contained in this tree. Keep edits
minimal and concentrated, `#ifdef _WIN32`-guarded where they are genuinely
Windows-specific, and prefer isolating new functionality in its own translation
unit over threading it through shared headers:

- **New functionality lives in its own unit.** `gzip_reader.{h,cpp}` holds all
  zlib and libbz2 code; `filebuf.h` sees only an opaque `void*` and four
  function decls, so `<zlib.h>` and `<bzlib.h>` never enter the broadly-included
  headers.
- **Integration edits stay at one choke point.** Gzip input is wired in only at
  `FileBuf::peek()`'s refill and the single `pat.h` open site; no other read-file
  call site changed.
- **Line-ending fixes** use `"wb"` / `ios::binary` (portable no-ops off Windows)
  plus one `_setmode` block per `main()`. They are not hidden behind broad macros.
- **`auto_ptr` → `unique_ptr`** is a direct, unguarded modernization.

The from-scratch build runbook (toolchain install, the `auto_ptr`/CRLF perl
one-liners against a *fresh* upstream clone, ARM emulation trap, static-link and
strip details, and the CRLF regression guard `check_hisat2_crlf.sh`) is in
[`../WINDOWS_BUILD.md`](../WINDOWS_BUILD.md) §3.

---

## Redistribution & licensing

The Windows recipes above produce a **static** binary intended for personal use.

- **Own use:** the static binary is fine as-is.
- **Redistributing the binary:** HISAT2 is **GPL-3.0**; bundle its license and
  source (or a written offer) along with the notices of everything compiled in —
  zlib, bzip2, and on Windows the statically linked GCC/libstdc++/winpthread runtimes.
  The simplest way to satisfy the runtime-relink terms is to build **without
  `-static`** so those become replaceable DLLs, then ship each DLL's license.
