#!/usr/bin/env bash
set -Eeuo pipefail

OPTIONS_FILE="${OPTIONS_FILE:-/data/options.json}"

main() {
  local greeting
  greeting=$(jq --raw-output '.greeting' "$OPTIONS_FILE")
  echo "$greeting"
}

# Run only when executed; sourcing the file lets tests call main() directly.
if [[ ${BASH_SOURCE[0]} == "$0" ]]; then
  main "$@"
fi
