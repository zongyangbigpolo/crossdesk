#!/bin/sh
# Copy libcrossdesk_core.dylib into the .app's Frameworks dir and re-codesign.
# Invoked as an Xcode Run Script build phase on the Runner target.
#
# Source location resolution order:
#   1. $CROSSDESK_CORE_DYLIB (explicit override)
#   2. xmake release output: ../../build/macosx/$ARCH/release/libcrossdesk_core.dylib
#   3. xmake debug output:   ../../build/macosx/$ARCH/debug/libcrossdesk_core.dylib
#
# $ARCH is derived from Xcode's $ARCHS (we only support single-arch builds for now).
set -eu

# Inside Xcode these are set; outside, fall back to standalone debug-build defaults
# so the script can be sanity-checked from a terminal.
: "${BUILT_PRODUCTS_DIR:=$PWD/build/macos/Build/Products/Debug}"
: "${FRAMEWORKS_FOLDER_PATH:=crossdesk_shell.app/Contents/Frameworks}"
: "${ARCHS:=arm64}"
: "${CONFIGURATION:=Debug}"
: "${EXPANDED_CODE_SIGN_IDENTITY:=-}"

ARCH="$(echo "$ARCHS" | awk '{print $1}')"
case "$ARCH" in
    arm64) XMAKE_ARCH=arm64 ;;
    x86_64) XMAKE_ARCH=x86_64 ;;
    *) echo "warning: unsupported ARCH=$ARCH; defaulting to arm64"; XMAKE_ARCH=arm64 ;;
esac

repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
candidates=""
if [ -n "${CROSSDESK_CORE_DYLIB:-}" ]; then
    candidates="$CROSSDESK_CORE_DYLIB"
fi
candidates="$candidates
$repo_root/build/macosx/$XMAKE_ARCH/release/libcrossdesk_core.dylib
$repo_root/build/macosx/$XMAKE_ARCH/debug/libcrossdesk_core.dylib"

src=""
for c in $candidates; do
    [ -z "$c" ] && continue
    if [ -f "$c" ]; then src="$c"; break; fi
done

if [ -z "$src" ]; then
    echo "error: libcrossdesk_core.dylib not found. Build with:"
    echo "  (cd $repo_root && xmake f -m release && xmake build crossdesk_core)"
    exit 1
fi

dst_dir="$BUILT_PRODUCTS_DIR/$FRAMEWORKS_FOLDER_PATH"
mkdir -p "$dst_dir"
cp -f "$src" "$dst_dir/libcrossdesk_core.dylib"
chmod u+w "$dst_dir/libcrossdesk_core.dylib"

# Re-sign so Gatekeeper/SIP doesn't reject the embedded binary.
codesign --force --sign "$EXPANDED_CODE_SIGN_IDENTITY" --timestamp=none \
    "$dst_dir/libcrossdesk_core.dylib"

echo "bundled $src -> $dst_dir/libcrossdesk_core.dylib"
