#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

variant=${1:-upstream}
host=${HOST:-0.0.0.0}
port=${PORT:-7000}
count=${COUNT:-100000}
backlog=${BACKLOG:-4096}
quiet=${QUIET:-0}
output=${OUTPUT:-$root/results/server-output.txt}

server="$root/bench/server-$variant"
if [ ! -x "$server" ]; then
  echo "Unknown or unbuilt variant: $variant" >&2
  exit 2
fi

bench() {
  date '+%Y-%m-%dT%H:%M:%S'
  echo "server variant=$variant host=$host port=$port count=$count backlog=$backlog"
  if [ "$quiet" = "1" ]; then
    "$server" \
      --host "$host" \
      --port "$port" \
      --count "$count" \
      --backlog "$backlog" \
      --quiet
  else
    "$server" \
      --host "$host" \
      --port "$port" \
      --count "$count" \
      --backlog "$backlog"
  fi
}

mkdir -p "$(dirname "$output")"
tmp=$(mktemp)
if bench > "$tmp" 2>&1; then
  status=0
else
  status=$?
fi
tee -a "$output" < "$tmp"
rm -f "$tmp"
exit "$status"
