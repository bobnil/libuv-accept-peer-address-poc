# Benchmark Methodology and Results

This document describes the benchmark data in `results/`.

The benchmark compares:

- `upstream`: unmodified libuv at commit `840404ce8ba7cc0204be52389a6cfff9f2c90fb6` (`v1.53.0`)
- `patched`: the same libuv commit with `save-peer-in-accept.patch` applied

The patch makes the Unix TCP accept path retrieve and preserve the peer address
returned by `accept()` / `accept4()`.

## Benchmark Shape

The same minimal libuv TCP server source is linked against each libuv variant.
The server:

1. listens on a TCP port,
2. accepts each connection,
3. immediately closes the accepted connection,
4. exits after the configured connection count.

The client creates a configured number of TCP connections with a configured
parallelism level. The main benchmark runs use 256 concurrent connection
attempts.

The server-side columns are the primary measurement:

- `server_seconds`
- `accepts_per_second`

The client-side columns are useful as a sanity check, but the benchmark is
intended to evaluate accept-path throughput on the server.

## Raw Data

The raw CSV files currently present are:

| File | Mode | Connections per run | Paired runs |
| --- | --- | ---: | ---: |
| `results/compare-loopback-500000.csv` | loopback, same machine | 500,000 | 30 |
| `results/compare-loopback-5000000.csv` | loopback, same machine | 5,000,000 | 30 |
| `results/compare-network-500000.csv` | two machines | 500,000 | 9 |
| `results/compare-network-5000000.csv` | two machines | 5,000,000 | 20 |
| `results/compare-network-50000000.csv` | two machines | 50,000,000 | 2 |

`loopback` files were run with client and server on the same machine.
`network` files were run with client and server on separate machines.

## Summary

In the paired columns below, positive values mean the patched build was faster
for that comparison:

```text
upstream server_seconds - patched server_seconds
```

| Dataset | Upstream mean server seconds | Patched mean server seconds | Mean paired diff | Median paired diff | Patched faster |
| --- | ---: | ---: | ---: | ---: | ---: |
| loopback, 500k | 19.445 | 19.145 | +0.300 | +0.274 | 21 / 30 |
| loopback, 5M | 197.761 | 196.933 | +0.828 | +0.015 | 15 / 30 |
| network, 500k | 19.670 | 19.660 | +0.010 | -0.338 | 4 / 9 |
| network, 5M | 181.264 | 182.429 | -1.165 | -2.686 | 7 / 20 |
| network, 50M | 1793.393 | 1790.606 | +2.787 | +2.787 | 1 / 2 |

Accept-rate summary:

| Dataset | Upstream mean accepts/s | Patched mean accepts/s | Mean paired accepts/s diff |
| --- | ---: | ---: | ---: |
| loopback, 500k | 25,761.917 | 26,135.649 | +373.732 |
| loopback, 5M | 25,302.719 | 25,401.622 | +98.903 |
| network, 500k | 25,460.111 | 25,455.444 | -4.667 |
| network, 5M | 27,618.500 | 27,435.650 | -182.850 |
| network, 50M | 27,891.000 | 27,946.000 | +55.000 |

## Interpretation

The results do not show a reproducible performance regression from retrieving
and preserving the peer address during `accept()`.

The direction changes between datasets:

- Some loopback results slightly favor the patched build.
- Some two-machine network results slightly favor upstream.
- The longest two-machine run slightly favors the patched build, but only has
  two paired runs.

The observed differences are small compared with run-to-run variation. The
5,000,000-connection loopback run is the clearest example: the mean favors the
patched build by about 0.4%, but the median paired difference is almost zero and
the patched build is faster in exactly 15 of 30 paired runs.

Overall, these measurements are consistent with "no meaningful throughput
regression observed in this environment".

They should not be read as proof that retrieving the peer address has zero cost
inside the syscall itself, or that every operating system, kernel, CPU, and TCP
workload will behave identically. This is an end-to-end TCP connection/accept
throughput benchmark for this proof of concept.

## Reproducing

Build the benchmark environment:

```sh
./install.sh
```

Run a same-machine loopback comparison:

```sh
RUNS=30 COUNT=5000000 PARALLEL=256 OUT=results/compare-loopback-5000000.csv ./scripts/compare.sh
```

Run a two-machine comparison by starting the server variant on the server
machine:

```sh
COUNT=5000000 QUIET=1 OUTPUT=results/server-output.txt ./scripts/server-only.sh upstream
COUNT=5000000 QUIET=1 OUTPUT=results/server-output.txt ./scripts/server-only.sh patched
```

and the client on the client machine:

```sh
HOST=<server-ip> COUNT=5000000 PARALLEL=256 QUIET=1 OUTPUT=results/client-output.txt ./scripts/client-only.sh
```

The two-machine CSV files in `results/` were assembled from these server/client
runs.
