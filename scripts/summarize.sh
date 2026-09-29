#!/usr/bin/env sh
set -eu

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 results/compare.csv" >&2
  exit 2
fi

awk -F, '
function sort(a, n, i, j, t) {
  for (i = 1; i <= n; i++)
    for (j = i + 1; j <= n; j++)
      if (a[j] < a[i]) {
        t = a[i]
        a[i] = a[j]
        a[j] = t
      }
}

function median(a, n) {
  sort(a, n)
  return n % 2 ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2
}

NR > 1 {
  variant = $1
  run = $2

  count[variant]++
  i = count[variant]

  server_seconds[variant] += $5
  accepts_per_second[variant] += $6
  client_seconds[variant] += $9
  connects_per_second[variant] += $10

  if (variant == "upstream") {
    us[i] = $5
    ua[i] = $6
    upstream_server[run] = $5
    upstream_accepts[run] = $6
  } else if (variant == "patched") {
    ps[i] = $5
    pa[i] = $6
    patched_server[run] = $5
    patched_accepts[run] = $6
  }
}

END {
  printf "variant,n,mean_server_seconds,median_server_seconds,mean_accepts_per_second,median_accepts_per_second,mean_client_seconds,mean_connects_per_second\n"
  printf "upstream,%d,%.9f,%.9f,%.6f,%.6f,%.9f,%.6f\n",
         count["upstream"],
         server_seconds["upstream"] / count["upstream"],
         median(us, count["upstream"]),
         accepts_per_second["upstream"] / count["upstream"],
         median(ua, count["upstream"]),
         client_seconds["upstream"] / count["upstream"],
         connects_per_second["upstream"] / count["upstream"]
  printf "patched,%d,%.9f,%.9f,%.6f,%.6f,%.9f,%.6f\n",
         count["patched"],
         server_seconds["patched"] / count["patched"],
         median(ps, count["patched"]),
         accepts_per_second["patched"] / count["patched"],
         median(pa, count["patched"]),
         client_seconds["patched"] / count["patched"],
         connects_per_second["patched"] / count["patched"]

  n = 0
  patched_faster = 0
  for (run in upstream_server) {
    if (!(run in patched_server))
      continue

    n++
    diff_seconds[n] = upstream_server[run] - patched_server[run]
    diff_accepts[n] = patched_accepts[run] - upstream_accepts[run]
    sum_diff_seconds += diff_seconds[n]
    sum_diff_accepts += diff_accepts[n]
    if (diff_seconds[n] > 0)
      patched_faster++
  }

  printf "\npaired,n,mean_upstream_minus_patched_seconds,median_upstream_minus_patched_seconds,mean_patched_minus_upstream_accepts_per_second,median_patched_minus_upstream_accepts_per_second,patched_faster_runs\n"
  printf "paired,%d,%.9f,%.9f,%.6f,%.6f,%d\n",
         n,
         sum_diff_seconds / n,
         median(diff_seconds, n),
         sum_diff_accepts / n,
         median(diff_accepts, n),
         patched_faster
}
' "$1"
