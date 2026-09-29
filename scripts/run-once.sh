#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

variant=${1:-upstream}
host=${HOST:-127.0.0.1}
port=${PORT:-7000}
count=${COUNT:-100000}
parallel=${PARALLEL:-256}

server="$root/bench/server-$variant"
if [ ! -x "$server" ]; then
  echo "Unknown or unbuilt variant: $variant" >&2
  exit 2
fi

"$server" --host "$host" --port "$port" --count "$count" &
server_pid=$!

cleanup() {
  kill "$server_pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

sleep 0.2
"$root/bench/client" --host "$host" --port "$port" --count "$count" --parallel "$parallel"

wait "$server_pid"
trap - EXIT INT TERM
