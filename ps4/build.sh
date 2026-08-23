#!/usr/bin/env bash
# Build Beetle PSX HW as a PlayStation 4 libretro module.
#
#   ps4/build.sh [--out <dir>] [--jobs <n>] [--no-lightrec] [--clean]
#
# ⚠ WHY THIS SCRIPT EXISTS AT ALL. The .prx that ran Spyro 3 on hardware for a day could not
# be reproduced from this repository: the platform arm was uncommitted, and the link was done
# by hand. This is that link, written down.
#
# ⚠ AND WHY IT DOES NOT USE `make`'s OWN LINK STEP. $(LD) in the core's Makefile is $(CXX),
# the host compiler driver, and a module here is ld.lld against the OpenOrbis toolchain
# followed by create-fself --lib. `make objects` builds the objects and stops.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

# The six lines that cannot be shared - see orbis-compat/scripts/ps4/orbis-env.sh. Sibling
# directory first, because that is what a fresh clone of the orbis-ports organisation looks like.
for _c in "${ORBIS_COMPAT_DIR:-}" "$ROOT/../orbis-compat" "$HOME/src-ps4/orbis-compat"; do
  [[ -n "$_c" && -f "$_c/scripts/ps4/orbis-env.sh" ]] && { ORBIS_COMPAT_DIR="$_c"; break; }
done
[[ -n "${ORBIS_COMPAT_DIR:-}" ]] || {
  echo "build: orbis-compat not found - clone it next to this repository, or set ORBIS_COMPAT_DIR" >&2
  exit 1
}
# shellcheck source=/dev/null
. "$ORBIS_COMPAT_DIR/scripts/ps4/orbis-env.sh"
TOOLCHAIN="$OO_PS4_TOOLCHAIN"

OUT_DIR="$ROOT"
JOBS="$(nproc)"
LIGHTREC=1
CLEAN=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --out)          OUT_DIR="$2"; shift 2 ;;
    --jobs|-j)      JOBS="$2";    shift 2 ;;
    --no-lightrec)  LIGHTREC=0;   shift ;;
    --clean)        CLEAN=1;      shift ;;
    *) echo "build: unknown argument: $1" >&2; exit 2 ;;
  esac
done

NAME="mednafen_psx_hw_libretro"
PRX="$OUT_DIR/$NAME.prx"
INFO="$OUT_DIR/$NAME.info"

# ⚠ HAVE_LIGHTREC IS A -D DEFINE AND THIS TREE HAS NO HEADER DEPENDENCY ON IT, so a tree built
# the other way round is silently reused and the link then fails on lightrec_destroy - or
# worse, does not. Switching it always cleans.
STAMP="$ROOT/.ps4-lightrec"
WANT="$LIGHTREC"
if [[ ! -f "$STAMP" || "$(cat "$STAMP")" != "$WANT" ]]; then
  CLEAN=1
fi
if [[ $CLEAN -eq 1 ]]; then
  echo "== cleaning (HAVE_LIGHTREC=$WANT)"
  find "$ROOT" \( -name '*.o' -o -name '*.d' \) -type f -delete
fi
echo "$WANT" > "$STAMP"

echo "== compiling (platform=orbis, HAVE_LIGHTREC=$LIGHTREC, -j$JOBS)"
make -C "$ROOT" platform=orbis HAVE_LIGHTREC="$LIGHTREC" -j"$JOBS" objects

mapfile -t OBJS < <(find "$ROOT" -name '*.o' -type f | sort)
[[ ${#OBJS[@]} -gt 0 ]] || { echo "build: no objects" >&2; exit 1; }
echo "== linking ${#OBJS[@]} objects"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# crtlib.o rather than crt1.o and --lib rather than --eboot: the two differences that make a
# module instead of an executable. "cannot find entry symbol _start" is expected - a module has
# no entry point, which is the whole distinction.
# ⚠ THE LINKER'S OUTPUT IS KEPT AND FILTERED, NOT PIPED THROUGH grep. Piping puts grep's exit
# status at the end of the pipeline, which is 1 when it matches nothing - so `|| true` has to
# follow, and that swallows every real diagnostic along with the harmless one.
set +e
ld.lld "${OBJS[@]}" -o "$WORK/core.elf" \
  -m elf_x86_64 -pie --script "$TOOLCHAIN/link.x" --eh-frame-hdr --no-rosegment \
  -L"$TOOLCHAIN/lib" -L"$ORBIS_COMPAT_DIR/build" \
  -lorbis-compat -lc -lkernel -lc++ -lSceVideoOut -lSceGnmDriver \
  "$TOOLCHAIN/lib/crtlib.o" > "$WORK/link.log" 2>&1
LD_RC=$?
set -e
# Expected and not an error: a module has no entry point, which is the whole difference.
grep -v "cannot find entry symbol _start" "$WORK/link.log" >&2 || true
if [[ $LD_RC -ne 0 || ! -f "$WORK/core.elf" ]]; then
  echo "build: the module did not link (ld.lld exit $LD_RC)" >&2
  exit 1
fi

# create-fself writes --lib relative to the working directory, and reads OO_PS4_TOOLCHAIN from
# the environment even when invoked by absolute path out of that very toolchain.
( cd "$WORK" && OO_PS4_TOOLCHAIN="$TOOLCHAIN" "$TOOLCHAIN/bin/linux/create-fself" \
    -in=core.elf -out=core.oelf --lib="$NAME.prx" --paid 0x3800000000000011 >/dev/null )

mkdir -p "$OUT_DIR"
cp "$WORK/$NAME.prx" "$PRX"

# ⚠ THE .info IS WHAT MAKES THE CORE VISIBLE. RetroArch shows the filename until it finds
# <core>.info in its core-info directory, and a core with no info has no extension filter -
# which then presents as an EMPTY CONTENT BROWSER rather than as a missing file.
cat > "$INFO" <<INFO
display_name = "Sony - PlayStation (Beetle PSX HW)"
corename = "Beetle PSX HW"
systemname = "Sony - PlayStation"
manufacturer = "Sony"
categories = "Emulator"
authors = "Mednafen Team"
supported_extensions = "cue|toc|m3u|ccd|exe|pbp|chd"
license = "GPLv2"
permissions = ""
display_version = "$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
firmware_count = 3
firmware0_desc = "scph5500.bin (PS1 JP BIOS)"
firmware0_path = "scph5500.bin"
firmware1_desc = "scph5501.bin (PS1 US BIOS)"
firmware1_path = "scph5501.bin"
firmware2_desc = "scph5502.bin (PS1 EU BIOS)"
firmware2_path = "scph5502.bin"
INFO

echo "prx:  $PRX ($(du -h "$PRX" | cut -f1))"
echo "info: $INFO"
echo
echo "⚠ on the console: .prx needs mode 777 (it is loaded as a module, 666 fails silently),"
echo "  the .info goes in /data/retroarch/info/, and /data/retroarch/info/core_info.cache"
echo "  must be DELETED afterwards or the menu keeps saying No cores available."
