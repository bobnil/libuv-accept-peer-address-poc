#!/usr/bin/env python3

import argparse
import socket
import struct


def reset_connection(host, port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        # Anslut normalt: SYN -> SYN/ACK -> ACK
        s.connect((host, port))

        # SO_LINGER med timeout 0 gör close() "abortive",
        # vilket på Linux skickar TCP RST istället för FIN.
        s.setsockopt(
            socket.SOL_SOCKET,
            socket.SO_LINGER,
            struct.pack("ii", 1, 0),
        )


def parse_args():
    parser = argparse.ArgumentParser(
        description="Connect to a TCP server and immediately reset the connection."
    )
    parser.add_argument("host")
    parser.add_argument("port", type=int)
    parser.add_argument("--count", type=int, default=1)
    args = parser.parse_args()

    if args.count < 1:
        parser.error("--count must be 1 or greater")

    return args


def main():
    args = parse_args()
    failures = 0
    first_error = None

    for _ in range(args.count):
        try:
            reset_connection(args.host, args.port)
        except OSError as exc:
            failures += 1
            if first_error is None:
                first_error = exc

    successes = args.count - failures
    print(
        f"Completed {args.count} reset attempt(s) to {args.host}:{args.port}: "
        f"{successes} succeeded, {failures} failed"
    )
    if first_error is not None:
        print(f"First error: {first_error}")

    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
