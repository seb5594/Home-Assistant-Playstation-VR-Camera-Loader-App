#!/bin/sh
# Builds RNNoise (https://github.com/xiph/rnnoise) as a static library.
#
# usage: build-rnnoise.sh <rnnoise source folder> <install prefix>
#
# The source folder is the git submodule src/c/rnnoise, pinned to upstream
# release v0.2. Git does not contain the trained model (rnnoise_data.c/.h,
# about 5 MB); upstream's download_model.sh fetches it from media.xiph.org,
# and so does this script. The model files are checked against the ones in
# the official v0.2 release archive.
#
# If the submodule is missing (an install that was copied without git, or a
# store that did not fetch submodules), the official release archive is used
# as a whole instead. Both ways build the very same code.
set -eu

SOURCE=$1
PREFIX=$2
RELEASE_URL=https://github.com/xiph/rnnoise/releases/download/v0.2/rnnoise-0.2.tar.gz
RELEASE_SHA256=90fce4b00b9ff24c08dbfe31b82ffd43bae383d85c5535676d28b0a2b11c0d37
MODEL_DATA_SHA256=dece58eabca6a722a0bd9dbc8dfadba27abac1271760fad65e1071f54717dc78
MODEL_HEADER_SHA256=09ff880bddd0fc74a2ae0e5ec6c8d65714031b08d0c3f672493acd9e189c5855
WORK=$(mktemp -d)

download() {   # download <url> <file> [sha256]
    curl -fsSL --retry 3 "$1" -o "$2"
    if [ -n "${3:-}" ]; then
        echo "$3  $2" | sha256sum -c -
    fi
}

model_is_known() {
    echo "$MODEL_DATA_SHA256  $SOURCE/src/rnnoise_data.c" | sha256sum -c - >/dev/null 2>&1 &&
        echo "$MODEL_HEADER_SHA256  $SOURCE/src/rnnoise_data.h" | sha256sum -c - >/dev/null 2>&1
}

release_archive() {
    download "$RELEASE_URL" "$WORK/release.tar.gz" "$RELEASE_SHA256"
    tar -xzf "$WORK/release.tar.gz" -C "$WORK"
}

if [ ! -f "$SOURCE/src/denoise.c" ]; then
    echo "RNNoise submodule not present; using the official v0.2 release archive"
    release_archive
    mkdir -p "$SOURCE"
    cp -a "$WORK/rnnoise-0.2/." "$SOURCE/"
fi

cd "$SOURCE"
if ! model_is_known; then
    model="rnnoise_data-$(cat model_version).tar.gz"
    echo "Fetching the RNNoise model $model"
    if download "https://media.xiph.org/rnnoise/models/$model" "$WORK/$model" &&
        tar -xzf "$WORK/$model" -C "$SOURCE" src/rnnoise_data.c src/rnnoise_data.h && model_is_known; then
        echo "Model matches the v0.2 release"
    else
        echo "Model download unavailable or different; taking it from the v0.2 release archive"
        [ -d "$WORK/rnnoise-0.2" ] || release_archive
        cp "$WORK/rnnoise-0.2/src/rnnoise_data.c" "$WORK/rnnoise-0.2/src/rnnoise_data.h" src/
        model_is_known
    fi
fi

# RNNoise 0.2 compiles its NEON code on aarch64, and that code includes Opus' os_support.h, which
# neither the release nor the git checkout contains. This stand-in provides what it uses.
if [ ! -f src/os_support.h ]; then
    cat > src/os_support.h <<'EOF'
#ifndef OS_SUPPORT_H
#define OS_SUPPORT_H
#include "common.h"
#define OPUS_CLEAR(dst, n) RNN_CLEAR(dst, n)
#endif
EOF
fi

# The release archive ships a generated configure; a git checkout needs autotools.
if [ ! -x configure ]; then
    [ -f package_version ] || echo 'PACKAGE_VERSION="0.2"' > package_version   # no git inside the image build
    autoreconf -isf
fi
case "$(uname -m)" in
    x86_64|i?86) cpu_dispatch=--enable-x86-rtcd ;;   # AVX2 / SSE4.1 picked at run time
    *) cpu_dispatch= ;;                              # aarch64 uses NEON automatically
esac
# shellcheck disable=SC2086
./configure --prefix="$PREFIX" --disable-shared --enable-static --disable-examples --disable-doc \
    --disable-maintainer-mode $cpu_dispatch
make -j"$(nproc)"
make install
rm -rf "$WORK"
