#!/usr/bin/env python3
"""Send automation commands to a running BallanceMMO client.

The client must be started with the environment variable BMMO_COMMAND_PIPE set
to a pipe name.  Every command line gets exactly one response line of the form
"<id> <text>".

Usage:
    bmmo_ctl.py --pipe <name> <command> [args...]
    bmmo_ctl.py --pipe <name> --script commands.txt   (one command per line)
    bmmo_ctl.py --pipe <name> --wait-log <ModLoader.log> --pattern <regex> [--timeout 60]
"""
import argparse
import re
import sys
import time


def open_pipe(name, timeout=15.0):
    path = r"\\.\pipe\{}".format(name)
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            return open(path, "r+b", buffering=0)
        except OSError as error:  # pipe not created yet or busy
            last_error = error
            time.sleep(0.2)
    raise SystemExit("cannot open pipe {}: {}".format(path, last_error))


def send(pipe, line, timeout=30.0):
    pipe.write((line.rstrip("\r\n") + "\n").encode("utf-8"))
    pipe.flush()
    deadline = time.monotonic() + timeout
    buffer = b""
    while time.monotonic() < deadline:
        chunk = pipe.read(1)
        if not chunk:
            time.sleep(0.01)
            continue
        buffer += chunk
        if chunk == b"\n":
            break
    text = buffer.decode("utf-8", "replace").rstrip("\r\n")
    ident, _, body = text.partition(" ")
    return body


def wait_log(path, pattern, timeout):
    regex = re.compile(pattern)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                content = handle.read()
        except OSError:
            content = ""
        matches = regex.findall(content)
        if matches:
            return matches[-1]
        time.sleep(0.25)
    raise SystemExit("timeout waiting for /{}/ in {}".format(pattern, path))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pipe", required=False)
    parser.add_argument("--script")
    parser.add_argument("--wait-log")
    parser.add_argument("--pattern")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("command", nargs="*")
    args = parser.parse_args()

    if args.wait_log:
        print(wait_log(args.wait_log, args.pattern or ".", args.timeout))
        return
    if not args.pipe:
        parser.error("--pipe is required")
    pipe = open_pipe(args.pipe)
    lines = []
    if args.script:
        with open(args.script, "r", encoding="utf-8") as handle:
            lines = [l.strip() for l in handle if l.strip() and not l.startswith("#")]
    elif args.command:
        lines = [" ".join(args.command)]
    else:
        parser.error("no command given")
    for line in lines:
        if line.startswith("sleep "):
            time.sleep(float(line.split()[1]))
            continue
        print(send(pipe, line, args.timeout))


if __name__ == "__main__":
    main()
