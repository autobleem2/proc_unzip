#!/usr/bin/env bash
# The launcher's tools/proc_check.py over this processor, the way an author runs it before publishing: the
# native build (build_linux/, from `ci/build.sh native`) laid out as a processor folder for this machine's key,
# checked against generated sample trees - a zipped game with a top folder, a game already unpacked, a zip that
# is no game, and a Mega Drive ROM. The checker comes from the launcher's develop branch (PROC_CHECK_URL, or a
# local copy in PROC_CHECK).
set -euo pipefail
cd "$(dirname "$0")/.."

URL="${PROC_CHECK_URL:-https://raw.githubusercontent.com/autobleem2/autobleem/develop/tools/proc_check.py}"
WORK=build_linux/proc_check
rm -rf "$WORK"
mkdir -p "$WORK"
if [ -n "${PROC_CHECK:-}" ]; then
    cp "$PROC_CHECK" "$WORK/proc_check.py"
else
    curl -fsSL "$URL" -o "$WORK/proc_check.py"
fi

key="linux-$(uname -m | sed 's/x86_64/x86_64/; s/aarch64/arm64/; s/i.86/i386/')"
mkdir -p "$WORK/unzip/bin/$key"
cp package/processor.ini "$WORK/unzip/"
cp build_linux/unzip "$WORK/unzip/bin/$key/"

python3 - "$WORK" <<'EOF'
import os, sys, zipfile
work = sys.argv[1]
games = os.path.join(work, "Games")
os.makedirs(os.path.join(games, "Spyro"))
open(os.path.join(games, "Spyro", "Spyro.bin"), "wb").write(b"x" * 4096)
open(os.path.join(games, "Spyro", "Spyro.cue"), "w").write('FILE "Spyro.bin" BINARY\n')
with zipfile.ZipFile(os.path.join(games, "Crash (USA).zip"), "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("Crash (USA)/Crash (USA).cue", 'FILE "Crash (USA).bin" BINARY\n')
    # big enough that the stop-and-restart check catches it in the middle
    z.writestr("Crash (USA)/Crash (USA).bin", os.urandom(64 * 1024 * 1024))
with zipfile.ZipFile(os.path.join(games, "docs.zip"), "w") as z:
    z.writestr("readme.txt", "not a game")
roms = os.path.join(work, "roms", "Sega - Mega Drive - Genesis")
os.makedirs(roms)
with zipfile.ZipFile(os.path.join(roms, "Sonic.zip"), "w") as z:
    z.writestr("Sonic (World).md", "rom")
EOF

python3 "$WORK/proc_check.py" "$WORK/unzip" --games "$WORK/Games" --roms "$WORK/roms" --key "$key" -v
