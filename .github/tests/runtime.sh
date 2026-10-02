#!/usr/bin/env bash
set -Eeuo pipefail

# Runs inside the built image: source the app and call main() against sample options.
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT
printf '{"greeting": "Hello CI"}\n' > "$fixture/options.json"
export OPTIONS_FILE="$fixture/options.json"
# shellcheck source=example-app/run.sh
source "${RUN_SCRIPT:-/run.sh}"

test "$(main)" = "Hello CI"
echo 'PASS: greeting is read from the app options'
