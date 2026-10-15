#!/bin/sh
# One-time setup: registers RNNoise (xiph/rnnoise) as a git submodule, pinned
# to the upstream release v0.2. Run it from anywhere inside your clone, then
# commit and push. Safe to run again; it only moves the pin back to v0.2.
set -eu

UPSTREAM=https://github.com/xiph/rnnoise.git
TAG=v0.2
COMMIT=904a876dce1f9ab8860c0a5000ed151f9f6eef58   # what refs/tags/v0.2 points to

cd "$(git rev-parse --show-toplevel)"
SUBMODULE=sony_playstation_vr_camera_loader/src/c/rnnoise

if [ ! -e "$SUBMODULE/.git" ]; then
    # .gitmodules may already describe the submodule; register it in the index.
    git submodule add --force "$UPSTREAM" "$SUBMODULE"
fi
git -C "$SUBMODULE" fetch --depth 1 origin "refs/tags/$TAG:refs/tags/$TAG"
git -C "$SUBMODULE" -c advice.detachedHead=false checkout "$TAG"
actual=$(git -C "$SUBMODULE" rev-parse HEAD)
if [ "$actual" != "$COMMIT" ]; then
    echo "RNNoise $TAG is $actual, expected $COMMIT; check the upstream repository" >&2
    exit 1
fi
git add .gitmodules "$SUBMODULE"
echo "RNNoise $TAG ($COMMIT) is staged as submodule. Next: git commit -m 'Add RNNoise v0.2 as submodule' && git push"
