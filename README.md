# proc_unzip

An AutoBleem **scanner processor**: before every scan it unpacks PS1 games packed as `.zip`, `.7z` or `.rar`
in `Games/`, and it unpacks packed ROMs in `RetroArch/roms/` one at a time (arcade sets stay packed - their
cores read them that way).

It is also **the example to copy** when you write a processor of your own. It is one C++ file,
[`src/main.cpp`](src/main.cpp), with nothing but the standard library and the vendored decoders -
[miniz](third_party/miniz) for zips, [libarchive](third_party/libarchive) with [liblzma](third_party/xz) for
7z and RAR: read it top to bottom and you have the whole protocol.

## Installing

Copy the folder to the stick so that it looks like this:

```
System/Processors/unzip/
    processor.ini
    bin/psc/unzip                  <- the PlayStation Classic
    bin/linux-armhf/unzip          <- any 32-bit Raspberry Pi
    bin/linux-arm64/unzip          <- any 64-bit Raspberry Pi
    bin/linux-i386/unzip           <- the PC stick
    bin/windows-x86_64/unzip.exe   <- Windows
```

Only the `bin/` folders for your machines are needed. The next scan runs it. The System menu's
**Scanner processors** item shows it, and lets you put it in order with the others or switch it off.

## What it does

| it is given | it does |
|---|---|
| `--start --games <Games>` | every archive holding a disc image (`.cue`, `.bin`, `.img`, `.iso`, `.chd`, `.pbp`, `.ecm`, `.m3u`, `.ccd`): one loose in `Games/` becomes a folder named after it, one in a folder is unpacked there. An archive with one top folder loses it. An archive that is not a game is left alone. |
| `--ismine --rom <file> --system <name>` | "mine" (exit 0) for an archive with exactly one file, unless the system is an arcade one |
| `--start --rom <file> --system <name>` | the ROM next to where the archive was |

The archives it reads:

- `.zip` - stored or deflated.
- `.7z` - LZMA, LZMA2, PPMd, BCJ/BCJ2 and the other branch filters, solid or not. It is read as a stream, so
  a solid 7z of a whole disc is never held in memory. (Not Deflate or BZip2 inside a 7z - 7-Zip uses those
  only when told to.)
- `.rar` - RAR 1.5 to 4.x and RAR5, and a set of volumes: `Game.part1.rar`, `Game.part2.rar`, ... or the older
  `Game.rar`, `Game.r00`, `Game.r01`, ... - read together, deleted together. A later volume on its own is not
  touched.
- Not: a split 7z (`Game.7z.001`), or anything with a password - those are left as they are.

Every file is written as `<name>.part` and renamed when it is complete; the archive is deleted only after the
last rename. It never overwrites a file that is already there - that archive fails, with a warning, and is
kept. A file that *is* the entry (the work of a run that was stopped) is kept: for a zip the CRC says so, for a
7z or a RAR the bytes are compared as they are unpacked.

## The protocol, in short

```
proc --version                      #Unzip V1.1.0 - Unpacks PS1 games and ROMs from .zip, .7z and .rar
proc --ismine --ps1 <folder>        exit 0 = mine, 1 = not mine
proc --ismine --rom <file> --system "<name>"
proc --start --games <dir>          the whole Games tree
proc --start --roms <dir>           the whole roms tree
proc --start --ps1 <folder>         one game folder
proc --start --rom <file> --system "<name>"
```

While it works it prints, one flushed line at a time: `#Starting - <title>` first, `#<stage>` for each
new stage, `0`..`100` for the percent, `n/m` for a counter, `#WARN - <text>`, and at the end `#DONE` (exit 0)
or `#ERROR - <text>` (exit 1). The environment carries `AB_PROCESSOR_PROTOCOL=1`, `AB_ROOT`,
`AB_GAMES_DIR`, `AB_ROMS_DIR`, `AB_RDB_DIR`, `AB_TMP` (a scratch folder off the stick - RAM on the console,
small files only), `AB_PLATFORM`, `AB_PLATFORM_KEYS`, `AB_LANGUAGE` and `AB_VERSION`.

The rules: atomic (`.part`, then rename), idempotent (a second run on its own output does nothing), stay
inside what you were given, say something at least every `Timeout=` seconds. The launcher's
`docs/scanner-processors-plan.md` is the whole design.

**Check yours before you publish it** with the launcher's `tools/proc_check.py`:

```
python tools/proc_check.py path/to/yourproc --games sample/Games --roms sample/roms
```

It runs your processor on scratch copies of the samples and reports every rule it can see broken: the
protocol, `*.part` files left behind, writes outside the target, a second run that still has work to do,
and a run stopped in the middle that cannot finish when started again.

## Building

- Every target at once, as CI does it: `ci/build.sh all` in the `ghcr.io/autobleem2/autobleem-build:develop`
  image - `dist/unzip-<version>.zip` with the binaries for the console, both Pis, the PC stick and Windows.
- Windows by hand: `./make_win.sh` in an MSYS2 UCRT64 shell - builds, runs the self-test, and lays out
  `dist/unzip/`.
- Anything else: `cmake -S . -B build && cmake --build build && (cd build && ctest)`.

Every push to `develop` builds the rolling `nightly` release; a `v*` tag makes a release. CI also runs the
launcher's `tools/proc_check.py` over the native build (`ci/proc_check.sh`) - copy that step into yours.

GPL-3.0-or-later (`LICENSE`); miniz is MIT (`third_party/miniz/LICENSE`), libarchive BSD-2-Clause
(`third_party/libarchive/COPYING`), liblzma 0BSD (`third_party/xz/COPYING.0BSD`). The package carries all three
next to the program. RARLAB's own UnRAR source is deliberately not used: its licence is not GPL-compatible.
