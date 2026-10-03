# libuv accept-time peer address proof of concepts

This repository explores a libuv/Node.js edge case around TCP peer identity.
On Unix, `accept()` / `accept4()` can return the peer address at the same time
as the accepted socket. libuv's current Unix TCP accept path does not ask for
that address, so callers that need the peer later usually rely on
`uv_tcp_getpeername()`.

If an established connection is reset before the peer is queried, `accept()`
can still have had the address while a later `getpeername()` can fail with
`ENOTCONN`. The experiments here test ways to preserve that accept-time peer
identity.

libuv is a separate project distributed under the MIT License. The patches in
this repository are experimental proof-of-concept patches, not
submission-ready production changes.

## Proofs of Concept

### POC1: store the accept-time peer address

The first POC changes libuv's Unix TCP accept path so accepted TCP handles keep
a copy of the peer address returned by `accept()`. This makes later
`uv_tcp_getpeername()` calls resilient to reset-before-query cases.

- Details: [README_POC1.md](README_POC1.md)
- Patch: `save-peer-in-accept.patch`
- Benchmark notes: [BENCHMARK.md](BENCHMARK.md)

### POC2: add an opt-in `uv_listen2()` API

The second POC explores a more targeted API shape: `uv_listen()` keeps its
current fast path, while a new `uv_listen2()` callback receives the peer
address captured by `accept()`. A Node.js proof of concept then uses that
address for `net.Socket` peer information.

- Details: [README_LISTEN2.md](README_LISTEN2.md)
- libuv patch: `poc-libuv-listen2.patch`
- Node.js patch: `poc-node-listen2.patch`
- Combined Node.js patch, including bundled libuv: `poc-node-uv-listen2.patch`

## Layout

- `libuv-upstream/`: unmodified `libuv/libuv` checkout
- `libuv-patched/`: POC1 checkout with `save-peer-in-accept.patch` applied
- `libuv-listen2/`: POC2 checkout with `poc-libuv-listen2.patch` applied
- `bench/`: standalone libuv benchmark servers and client
- `repro/`: Node.js and Python repro for observing the missing peer address
- `scripts/`: build and benchmark helpers
- `results/`: CSV output from repeated runs
- `research2/`: working notes for the `uv_listen2()` direction

## Build

```sh
./install.sh
```

The install helper requires `git`, `cmake`, `make`, and `cc`. It clones libuv,
checks out commit `840404ce8ba7cc0204be52389a6cfff9f2c90fb6`, applies the POC
patches to their respective checkouts, builds the libuv variants, and then
builds the benchmark programs.

## Quick Checks

Run the POC1 benchmark variants:

```sh
COUNT=10000 PARALLEL=128 ./scripts/run-once.sh upstream
COUNT=10000 PARALLEL=128 ./scripts/run-once.sh patched
```

The `listen2` benchmark server is also built and can be run through the same
helper:

```sh
COUNT=10000 PARALLEL=128 ./scripts/run-once.sh listen2
```

For the benchmark methodology and current result summary, see
[BENCHMARK.md](BENCHMARK.md). For an in-depth account of the socket API and
Node.js/libuv history, see [HISTORICAL_CONTEXT.md](HISTORICAL_CONTEXT.md).
