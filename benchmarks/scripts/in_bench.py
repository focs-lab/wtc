#!/usr/bin/env python3
"""Run one command inside a `bench` reservation, non-interactively.

    in_bench.py -c 2 -m 4G [-e] -- <command...>

`bench` opens an interactive shell on a pseudo-terminal inside the reserved
slice and holds the reservation until that shell exits. This drives it: it
starts `bench` on a pty, waits for the line that means the shell is up,
sends the command followed by an exit, relays the output, and returns the
command's exit code. Feeding the shell from a pipe does not work, because
the input arrives before the shell exists and its end-of-file kills the
session, which is how this script came to exist.

Everything the reservation does to the machine is undone by `bench` itself
when the shell exits, including on error, so a failing command still
releases the cores.
"""
import os
import pty
import select
import shlex
import signal
import sys
import time

READY = b"Press ^] three times within 1s to disconnect TTY."
RC_MARK = "__IN_BENCH_RC="


def main():
    argv = sys.argv[1:]
    if "--" not in argv:
        print(__doc__, file=sys.stderr)
        return 2
    i = argv.index("--")
    bench_args, cmd = argv[:i], argv[i + 1:]
    if not cmd:
        print("no command", file=sys.stderr)
        return 2
    line = " ".join(shlex.quote(a) for a in cmd)
    # The shell reports the command's status on a marked line before exiting.
    payload = f"{line}; echo {RC_MARK}$?; exit\n".encode()

    pid, fd = pty.fork()
    if pid == 0:
        os.execvp("bench", ["bench", *bench_args])
    buf = b""
    sent = False
    rc = None
    out = sys.stdout.buffer
    try:
        while True:
            r, _, _ = select.select([fd], [], [], 1.0)
            if fd in r:
                try:
                    chunk = os.read(fd, 65536)
                except OSError:
                    break
                if not chunk:
                    break
                buf += chunk
                out.write(chunk); out.flush()
                if not sent and READY in buf:
                    time.sleep(0.5)  # let the shell finish its startup files
                    os.write(fd, payload)
                    sent = True
                    buf = b""
                if sent:
                    for l in buf.decode(errors="replace").splitlines():
                        if RC_MARK in l and not l.strip().startswith("echo"):
                            try:
                                rc = int(l.split(RC_MARK, 1)[1].strip().split()[0])
                            except (ValueError, IndexError):
                                pass
                    if len(buf) > 1 << 20:
                        buf = buf[-65536:]
    finally:
        try:
            os.kill(pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        os.waitpid(pid, 0)
    if rc is None:
        print("in_bench: the command's exit code was not seen", file=sys.stderr)
        return 3
    return rc


if __name__ == "__main__":
    sys.exit(main())
