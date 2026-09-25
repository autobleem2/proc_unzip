# proc_unzip - developer context

An AutoBleem **scanner processor** (the launcher's `docs/scanner-processors-plan.md`, CLAUDE.md "Scanner
processors"): a plain console program in `System/Processors/unzip/` that the launcher's scan runs before it
reads the games. It unpacks PS1 games packed as `.zip`, `.7z` or `.rar` in `Games/` (`--start --games`) and
single-ROM archives in `RetroArch/roms/` (`--ismine`/`--start --rom`, arcade systems left packed). It is also
**the example** other processors are copied from, so `src/main.cpp` stays one readable file: the protocol in its
header comment, a small portable filesystem layer, the `Archive` class, then the work. The name stays `unzip`
(folder, binary, `Name=`) since 1.1.0 added 7z and RAR: the appliance and the launcher's docs know it by that.

- **No AutoBleem SDK**, on purpose: the C++ standard library and vendored decoders. A processor needs nothing
  from the launcher but the protocol.
  - `third_party/miniz` (MIT, `MINIZ_NO_TIME`) - zips, and the CRC a zip entry is compared by.
  - `third_party/libarchive` - libarchive **3.8.9** (BSD-2-Clause), trimmed to the read core and the 7-Zip,
    RAR (1.5-4.x) and RAR5 readers; `third_party/xz` - liblzma **5.8.4** (0BSD), decoders only (LZMA, LZMA2,
    delta, the branch filters), which libarchive needs for a 7z's LZMA. Both are built by
    `third_party/archive.cmake` with **hand-written configuration** (`libarchive/ab_config.h`, and liblzma's
    `HAVE_*` in the cmake file) - no configure checks, so every toolchain builds them the same way, gcc-6 for
    the console included. No iconv, zlib, bzip2 or crypto library: a 7z using Deflate/BZip2 or anything with a
    password is not read (left alone). To update either: copy the new release's files over the same list
    (the source lists in `archive.cmake` are the whole set; liblzma skips every `*encoder*` but
    `lzma_encoder_presets.c`), then build and run the self-test on Windows and in the image.
  - **Names**: libarchive converts names to "the current locale", and the console has none (it is "C"). The
    one local addition, `libarchive/ab_names.c`, sets the archive's current charset to UTF-8 (a private field -
    re-check it on a libarchive update), so libarchive takes its built-in UTF-16 -> UTF-8 route.
    `archive_entry_pathname()` is then UTF-8 on Linux; on Windows `archive_entry_pathname_utf8()` is what works.
    **Do not use `pathname_utf8()` on Linux** - it converts the (already UTF-8) name again from the C locale and
    returns NULL.
  - RARLAB's UnRAR source is not an option: its licence is not GPL-compatible.
- **Archives are read as streams** (`LibArchive` in main.cpp): `open()` lists on one pass - libarchive's list
  mode decodes nothing - and `extract()` reads again, entries in order, into the `Sink` the caller picks. A
  solid 7z is never in memory (the console has 512 MB). Volumes reach libarchive through our own `FILE*`
  callbacks (UTF-8 paths on Windows; the switch callback moves between volumes). `volumesOf()` is the set:
  `Game.part1.rar`... (any digit width) or `Game.rar` + `.r00`..`.r99`; a later volume alone is nothing.
- **Known libarchive limit**: a RAR5 set whose compressed block spans more than two volumes fails ("Truncated
  input file") - only with absurdly small volumes (5 KB in a test); 1 MB volumes of a 3 MB file read fine. The
  fixture's RAR5 set is stored (`-m0`) for that reason.
- **The rules it keeps** (and every processor must): every file through `<name>.part` and a rename, the archive
  (every volume) deleted only after the last rename, a stale `.part` removed before writing, never an overwrite,
  a failed archive undone and kept, idempotent, only inside its target. A file already where an entry goes is
  a stopped run's work when it is the entry: a zip's CRC says so up front; a 7z/RAR has no CRC through
  libarchive, so the bytes are compared as they are unpacked (`CompareSink`) - a difference is a clash.
- **Tests**: `tests/selftest.cpp` builds zips with miniz, copies the 7z/RAR fixtures from `tests/data/` (made with
  7-Zip 16.02, RAR 7.01 for RAR5 and RAR 6.23 for RAR4 - RAR 7 cannot write RAR4 - in Debian containers; the
  file list and contents are in the test's header comment, every `.bin` is `binData(n)`) and runs the real
  program on them (ctest; natively and again for i386 in CI). `ci/proc_check.sh` adds a 7z and a RAR game and a
  7z ROM to the launcher's checker run.
- **Build**: `./make_win.sh` (MSYS2 UCRT64, the Windows folder in `dist/unzip/`); `ci/build.sh
  native|psc|rpi|rpi64|pcusb|win|package|all` in `ghcr.io/autobleem2/autobleem-build` (`:develop` for
  develop, `:latest` for a v* tag). The toolchain files are copies of app_terminal's (psc, rpi, rpi64, pcusb)
  and the launcher's (mingw). The package is `dist/unzip-<version>.zip`: the folder with `bin/psc`,
  `bin/linux-armhf`, `bin/linux-arm64`, `bin/linux-i386` and `bin/windows-x86_64` - the generic keys, so one
  build serves every Pi and every PC stick - plus `LICENSE.miniz`, `LICENSE.libarchive`, `LICENSE.liblzma`.
  Everything but Windows is built with `_FILE_OFFSET_BITS=64` (a disc image can pass 2 GB on armhf/i386).
- **CI**: `.github/workflows/build.yml`, gated by the repository variable `AB_CI_ENABLED` like every autobleem2
  repository; develop keeps the rolling `nightly` release, a `v*` tag makes a release.
- The version is `package/processor.ini`'s `Version=` (`AB_VERSION` in CI). The repository name follows the
  owner's rule: a processor's repository is `proc_<name>`.
