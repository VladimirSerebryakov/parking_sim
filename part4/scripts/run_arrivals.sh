#!/usr/bin/env bash
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
exec "${PARKING_TCP_BIN:-$ROOT/build/bin/part4}/parking_arrivals" "$@"
