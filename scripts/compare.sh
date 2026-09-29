#!/usr/bin/env sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
runs=${RUNS:-10}
host=${HOST:-127.0.0.1}
port=${PORT:-7000}
count=${COUNT:-100000}
parallel=${PARALLEL:-256}
out=${OUT:-"$root/results/compare-$(date +%Y%m%d-%H%M%S).csv"}

mkdir -p "$root/results"
printf 'variant,run,accepted,errors,server_seconds,accepts_per_second,completed,failed,client_seconds,connects_per_second\n' > "$out"

run_variant() {
  variant=$1
  run=$2
  server="$root/bench/server-$variant"
  server_log=$(mktemp)
  client_log=$(mktemp)

  "$server" --host "$host" --port "$port" --count "$count" --quiet > "$server_log" &
  server_pid=$!

  cleanup() {
    kill "$server_pid" 2>/dev/null || true
    rm -f "$server_log" "$client_log"
  }
  trap cleanup INT TERM EXIT

  sleep 0.2
  "$root/bench/client" --host "$host" --port "$port" --count "$count" --parallel "$parallel" --quiet > "$client_log"
  wait "$server_pid"

  printf '%s,%s,%s,%s\n' "$variant" "$run" "$(cat "$server_log")" "$(cat "$client_log")" >> "$out"
  rm -f "$server_log" "$client_log"
  trap - INT TERM EXIT
}

i=1
while [ "$i" -le "$runs" ]; do
  if [ $((i % 2)) -eq 1 ]; then
    run_variant upstream "$i"
    run_variant patched "$i"
  else
    run_variant patched "$i"
    run_variant upstream "$i"
  fi
  i=$((i + 1))
done

echo "$out"
