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

# The six lines that cannot be shared - see orbis-porting-kit/scripts/ps4/orbis-env.sh. Sibling
# directory first, because that is what a fresh clone of the orbis-ports organisation looks like.
#
# ⚠ THE OVERLAY IS FOUND BY A HEADER IT OWNS, NOT BY THIS SCRIPT'S PROLOGUE. Until 2026-09-18
# both blocks below were one, and it probed for scripts/ps4/orbis-env.sh inside orbis-compat. That
# file moved to the porting kit that day, so the probe matched nothing, every candidate was rejected
# and the script exited 1 with "orbis-compat not found" against a checkout that was sitting right
# there. MEASURED: cores.yml run 35391145504, job `fork`, 26 seconds in.
for _c in "${ORBIS_COMPAT_DIR:-}" "$ROOT/../orbis-compat" "$HOME/src-ps4/orbis-compat"; do
  [[ -n "$_c" && -f "$_c/include/orbis_prefix.h" ]] && { ORBIS_COMPAT_DIR="$_c"; break; }
done
[[ -n "${ORBIS_COMPAT_DIR:-}" ]] || {
  echo "build: orbis-compat not found - clone https://github.com/orbis-ports/orbis-compat next to" >&2
  echo "       this repository, or set ORBIS_COMPAT_DIR" >&2
  exit 1
}

# ⚠ TWO REPOSITORIES SINCE 2026-09-18: orbis-compat is include/ and the archive, the porting kit
# is the toolchain file, the loader shim and these scripts. The kit's last candidate is the overlay
# itself, which carried them until that date - so a pinned checkout older than the move still works.
for _k in "${ORBIS_KIT_DIR:-}" "$ROOT/../orbis-porting-kit" "$HOME/src-ps4/orbis-porting-kit" "${ORBIS_COMPAT_DIR}"; do
  [[ -n "$_k" && -f "$_k/scripts/ps4/orbis-env.sh" ]] && { ORBIS_KIT_DIR="$_k"; break; }
done
[[ -n "${ORBIS_KIT_DIR:-}" ]] || {
  echo "build: orbis-porting-kit not found - clone https://github.com/orbis-ports/orbis-porting-kit" >&2
  echo "       next to this repository, or set ORBIS_KIT_DIR" >&2
  exit 1
}
export ORBIS_COMPAT_DIR ORBIS_KIT_DIR
# shellcheck source=/dev/null
. "$ORBIS_KIT_DIR/scripts/ps4/orbis-env.sh"
TOOLCHAIN="$OO_PS4_TOOLCHAIN"

OUT_DIR="$ROOT"
# ⚠ NOT `nproc`. It is GNU coreutils and macOS does not ship it, so this line exited 127 and
# took the whole script with it under `set -e`:
#
#     ./ps4/build.sh: line 31: nproc: command not found
#
# Measured by bundle-gate.sh stage 5, 2026-09-17. getconf is POSIX and answers on both Linux
# and macOS; nproc and sysctl are kept behind it for the platforms where it does not.
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
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

# ⚠ TWO THINGS THIS TREE'S DEPENDENCY TRACKING CANNOT SEE, and both have already shipped a
# binary that was not what it claimed to be.
#
#   HAVE_LIGHTREC   a -D define, and no object records which way it was built. A tree built the
#                   other way round is silently reused; the link then fails on lightrec_destroy,
#                   or worse, does not.
#
#   the overlay     orbis-compat comes in on -isystem, and -MMD OMITS SYSTEM HEADERS FROM THE
#                   .d FILES BY DESIGN. So a change to orbis-compat/include is invisible to
#                   make: every object stays "up to date" against a header that no longer says
#                   what it said. Measured 2026-08-23 - sys/sysctl.h was fixed to answer
#                   hw.ncpu, the core was rebuilt, uploaded and run, and Lightrec still reported
#                   "Threaded recompiler started with 1 workers" because recompiler.o was
#                   thirty-three minutes older than the header it was supposed to have read.
#                   The .prx even came out byte-identical in size, which is what a rebuild that
#                   rebuilt nothing looks like from the outside.
#
# The stamp therefore carries both: the flag, and the newest mtime anywhere under the overlay's
# include tree.
STAMP="$ROOT/.ps4-lightrec"
# ⚠ NOT `find -printf`. It is a GNU extension and BSD find has no such option, so on macOS this
# line printed a usage error to a discarded stderr and yielded the empty string - the stamp then read
# "overlay:unknown" on every run, which compares equal to itself and quietly stops noticing that the
# overlay changed. cksum over the mtimes is POSIX and answers the same question on both hosts.
OVERLAY_STAMP="$( (cd "$ORBIS_COMPAT_DIR/include" && find . -type f -exec ls -lT {} + 2>/dev/null \
                   || find . -type f -exec ls -l --time-style=full-iso {} + 2>/dev/null) \
                 | LC_ALL=C sort | cksum | cut -d' ' -f1)"
WANT="$LIGHTREC overlay:${OVERLAY_STAMP:-unknown}"
if [[ ! -f "$STAMP" || "$(cat "$STAMP")" != "$WANT" ]]; then
  CLEAN=1
fi
if [[ $CLEAN -eq 1 ]]; then
  echo "== cleaning ($WANT)"
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
# ⚠ HOST FIRST, NOT NAME FIRST. bin/linux/create-fself exists on a Mac too - it is a Linux ELF
# the kernel cannot run - and `[[ -x ]]` says yes to it, so naming that path outright produced
# "Exec format error" after a full compile and link, at the most expensive step to fail at. Ordered
# by the host running this script, the same way RetroArch's ps4/build-core.sh does it.
case "$(uname -s)" in
  Darwin) _fself_order=("$TOOLCHAIN/bin/macos/create-fself-macos" "$TOOLCHAIN/bin/macos/create-fself"
                        "$TOOLCHAIN/bin/linux/create-fself") ;;
  *)      _fself_order=("$TOOLCHAIN/bin/linux/create-fself" "$TOOLCHAIN/bin/macos/create-fself-macos"
                        "$TOOLCHAIN/bin/macos/create-fself") ;;
esac
_fself=""
for _c in "${_fself_order[@]}"; do
  [[ -x "$_c" ]] && { _fself="$_c"; break; }
done
[[ -n "$_fself" ]] || { echo "build: no create-fself in $TOOLCHAIN/bin/{linux,macos}" >&2; exit 1; }

( cd "$WORK" && OO_PS4_TOOLCHAIN="$TOOLCHAIN" "$_fself" \
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
# ⚠ WITHOUT THIS LINE THE SCANNER FINDS NOTHING TO ATTACH A PLAYLIST TO. It names the .rdb in
# RetroArch's database directory that identifies this system's discs; the file is
# "Sony - PlayStation.rdb" from libretro-database and the name must match it exactly.
database = "Sony - PlayStation"
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
