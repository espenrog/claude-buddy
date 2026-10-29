#!/bin/sh
# Creates the Ed25519 key that signs firmware images for MCUboot (nRF54L devices), outside this repository.
# Usage: tools/new-signing-key.sh <NCS directory> [output .pem]
# Set BUDDY_TOOLCHAIN to a toolchain bundle id (nrfutil sdk-manager list) if the default one is not installed; the
# default is the bundle tools/build-xiao.sh uses. Only its Python (imgtool needs the cryptography package) is used.
# KEEP THE KEY SAFE AND BACKED UP. Anyone with it can sign firmware your buddies accept, and without it you can
# never make an update for a buddy that has its public half locked in. Never commit it.
set -eu
ncs="${1:?usage: $0 <NCS directory> [output .pem]}"
[ -d "$ncs" ] || { echo "no NCS directory at $ncs (usage: $0 <NCS directory> [output .pem]; the NCS version argument is gone)" >&2; exit 1; }
ncs="$(cd "$ncs" && pwd)"
out="${2:-$HOME/.config/claude-buddy/signing/claude-buddy-ed25519.pem}"
case "$out" in /*) ;; *) out="$PWD/$out" ;; esac
[ -e "$out" ] && { echo "$out already exists; not overwriting" >&2; exit 1; }
mkdir -p "$(dirname "$out")"
umask 077
nrfutil sdk-manager toolchain launch --toolchain-bundle-id "${BUDDY_TOOLCHAIN:-fbf7391cab}" --chdir "$ncs" -- \
    python3 "$ncs/bootloader/mcuboot/scripts/imgtool.py" keygen -t ed25519 -k "$out"
echo "wrote $out (back it up somewhere safe, and do not put it in git)"
