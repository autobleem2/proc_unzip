# proc_unzip - developer context

An AutoBleem **scanner processor** (the launcher's `docs/scanner-processors-plan.md`, CLAUDE.md "Scanner
processors"): a plain console program in `System/Processors/unzip/` that the launcher's scan runs before it
reads the games. It unpacks zipped PS1 games in `Games/` (`--start --games`) and single-ROM zips in
`RetroArch/roms/` (`--ismine`/`--start --rom`, arcade systems left zipped). It is also **the example** other
processors are copied from, so `src/main.cpp` stays one readable file: the protocol in its header comment, a
small portable filesystem layer, then the work.

- **No AutoBleem SDK**, on purpose: the C++ standard library and vendored miniz (`third_party/miniz`, MIT,
  `MINIZ_NO_TIME`). A processor needs nothing from the launcher but the protocol.
- **The rules it keeps** (and every processor must): every file through `<name>.part` and a rename, the zip
  deleted only after the last rename, a stale `.part` removed before writing, never an overwrite, a failed zip
  undone and kept, idempotent, only inside its target.
- **Tests**: `tests/selftest.cpp` builds zips with miniz and runs the real program on them (ctest; natively and
  again for i386 in CI).
- **Build**: `./make_win.sh` (MSYS2 UCRT64, the Windows folder in `dist/unzip/`); `ci/build.sh
  native|psc|rpi|rpi64|pcusb|win|package|all` in `ghcr.io/autobleem2/autobleem-build` (`:develop` for
  develop, `:latest` for a v* tag). The toolchain files are copies of app_terminal's (psc, rpi, rpi64, pcusb)
  and the launcher's (mingw). The package is `dist/unzip-<version>.zip`: the folder with `bin/psc`,
  `bin/linux-armhf`, `bin/linux-arm64`, `bin/linux-i386` and `bin/windows-x86_64` - the generic keys, so one
  build serves every Pi and every PC stick.
- **CI**: `.github/workflows/build.yml`, gated by the repository variable `AB_CI_ENABLED` like every autobleem2
  repository; develop keeps the rolling `nightly` release, a `v*` tag makes a release.
- The version is `package/processor.ini`'s `Version=` (`AB_VERSION` in CI). The repository name follows the
  owner's rule: a processor's repository is `proc_<name>`.
