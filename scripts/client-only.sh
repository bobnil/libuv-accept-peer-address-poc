#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

host=${HOST:-}
port=${PORT:-7000}
count=${COUNT:-100000}
parallel=${PARALLEL:-256}
quiet=${QUIET:-0}
output=${OUTPUT:-$root/results/client-output.txt}

if [ -z "$host" ]; then
  echo "Set HOST to the server address, for example: HOST=192.0.2.10 $0" >&2
  exit 2
fi

if [ ! -x "$root/bench/client" ]; then
  echo "Client is not built. Run ./install.sh first." >&2
  exit 2
fi

bench() {
  date '+%Y-%m-%dT%H:%M:%S'
  echo "client host=$host port=$port count=$count parallel=$parallel"
  if [ "$quiet" = "1" ]; then
    "$root/bench/client" \
      --host "$host" \
      --port "$port" \
      --count "$count" \
      --parallel "$parallel" \
      --quiet
  else
    "$root/bench/client" \
      --host "$host" \
      --port "$port" \
      --count "$count" \
      --parallel "$parallel"
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
