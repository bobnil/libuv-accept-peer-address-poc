# libuv stored peer address proof of concept

For the repository overview and the other proof of concept, see
[README.md](README.md).

This note describes the first proof of concept in this repository: a libuv
patch that asks the Unix `accept()` syscall for the peer address and stores it
on the accepted TCP handle for later `uv_tcp_getpeername()` calls.

The relevant patch file is:

- `save-peer-in-accept.patch`: standalone libuv proof of concept

## Background

On Unix, `accept()` / `accept4()` can return the peer address at the same time
as the accepted socket. libuv currently passes `NULL, NULL` for that address in
its Unix TCP accept path, so the address is not retained. When needed,
`uv_tcp_getpeername()` instead queries the accepted socket with
`getpeername()`.

If an established connection is reset before the peer is queried, `accept()`
can still provide its stored peer address while a later `getpeername()` can
fail with `ENOTCONN`. The reset can occur before userspace calls `accept()`. If
libuv does not retain the address returned at accept time, that peer identity
may no longer be recoverable.

This POC preserves the peer address returned by `accept()` in libuv's TCP
handle state. It is useful for demonstrating the behavior and measuring the
cost of always asking the kernel for the peer address in the TCP accept path.

## Layout

- `libuv-upstream/`: unmodified `libuv/libuv` at v1.53.0
- `libuv-patched/`: same commit with `save-peer-in-accept.patch` applied
- `bench/server.c`: benchmark server source used by `upstream` and `patched`
- `bench/client.c`: socket client load generator
- `repro/`: Node.js and Python repro for observing the missing peer address
- `scripts/`: build and benchmark helpers
- `results/`: CSV output from repeated runs

## Reproducer

The files in `repro/` demonstrate why preserving the address returned by
`accept()` matters for Node.js.

`repro/http_server.js` starts a Node.js HTTP server and counts accepted TCP
connections where `remoteAddress` is available or unavailable.

`repro/tcpreset.py` connects to the server and then immediately resets the TCP
connection. On Linux, it does this with `SO_LINGER` enabled with timeout 0,
which sends RST after the TCP handshake has completed. This exercises the case
where the peer address was available to `accept()`, but libuv did not retain it
and a later lookup may fail.

Example:

```sh
node repro/http_server.js 127.0.0.1 <server-port>
./repro/tcpreset.py 127.0.0.1 <server-port> --count 1000
```

These commands use the `node` executable on your `PATH` to reproduce the
behavior. `./install.sh` builds the standalone libuv variants used by the
benchmark; it does not build Node.js. To test the proof-of-concept change
through Node.js, download the Node.js source, apply the change to its bundled
libuv, and build Node.js separately. That build is left out of this repository
because the change belongs in libuv, while downloading and building Node.js adds
a substantially larger dependency and takes a long time to compile.

## Build

```sh
./install.sh
```

The install helper requires `git`, `cmake`, `make`, and `cc`. It clones libuv,
checks out commit `840404ce8ba7cc0204be52389a6cfff9f2c90fb6`, applies
`save-peer-in-accept.patch` to `libuv-patched`, builds the libuv variants, and
then builds the benchmark programs.

## Smoke Test

```sh
COUNT=10000 PARALLEL=128 ./scripts/run-once.sh upstream
COUNT=10000 PARALLEL=128 ./scripts/run-once.sh patched
```

## Two-Machine Run

Run one variant at a time. Start the upstream server on the server machine:

```sh
COUNT=500000 PORT=<server-port> ./scripts/server-only.sh upstream
```

While it is running, start the load generator on the client machine:

```sh
HOST=<server-ip> COUNT=500000 PARALLEL=256 PORT=<server-port> ./scripts/client-only.sh
```

After that run finishes, repeat the same sequence with `patched` on the server
machine and the same client command:

```sh
COUNT=500000 PORT=<server-port> ./scripts/server-only.sh patched
```

`server-only.sh` binds to `0.0.0.0` by default. Set `HOST=<bind-address>` if you
want to bind a specific interface. Add `QUIET=1` for CSV-style output.

## Compare

```sh
RUNS=20 COUNT=100000 PARALLEL=256 ./scripts/compare.sh
```

The CSV columns include the server-side accept rate and the client-side connect
rate. The server-side `accepts_per_second` column is the primary comparison.
The compare script alternates run order by iteration to reduce ordering bias.

Summarize a CSV with:

```sh
./scripts/summarize.sh results/compare-YYYYMMDD-HHMMSS.csv
```

For the benchmark methodology and current result summary, see
[BENCHMARK.md](BENCHMARK.md). For an in-depth account of the socket API and
Node.js/libuv history, see [HISTORICAL_CONTEXT.md](HISTORICAL_CONTEXT.md).
