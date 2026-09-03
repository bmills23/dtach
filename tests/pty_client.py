#!/usr/bin/env python3
"""Run a dtach client under a real pty and record what it prints.

dtach refuses to attach without a terminal, and the bugs these tests cover
only appear when a client stops draining its terminal, so the tests need a
pty they can deliberately stop reading.

Usage: pty_client.py <outfile> <cmdline> <action> [<action> ...]

Actions:
  stall:<secs>            do not read the pty at all for <secs> seconds, so
                          the tty buffer and then the socket buffer fill up
  read:<secs>             read for <secs> seconds, appending to <outfile>
  quiet:<idle>:<max>      read until <idle> seconds pass with no data
                          arriving, or <max> seconds elapse
  trickle:<secs>:<rate>[:<idle>]
                          read at most <rate> bytes per second for <secs>
                          seconds, so the client makes steady but throttled
                          progress. With <idle>, stop early once <idle>
                          seconds pass with no data arriving, which lets a
                          throttled client finish a replay and move on
                          instead of idling out the rest of <secs>
  send:<text>             write <text> to the pty ("\\n" and "\\x1c" understood)
  detach                  send the dtach detach character (^\\, 0x1c)
"""
import os
import pty
import select
import shlex
import signal
import sys
import time

outfile = sys.argv[1]
cmdline = sys.argv[2]
actions = sys.argv[3:]
argv = shlex.split(cmdline)

pid, fd = pty.fork()
if pid == 0:
    os.execvp(argv[0], argv)
    os._exit(127)

out = open(outfile, "ab", 0)
state = {"eof": False}


def read_once(timeout, size=65536):
    """Read at most `size` bytes. Returns the byte count read."""
    try:
        ready, _, _ = select.select([fd], [], [], timeout)
    except (OSError, ValueError):
        state["eof"] = True
        return 0
    if not ready:
        return 0
    try:
        data = os.read(fd, size)
    except OSError:
        state["eof"] = True
        return 0
    if not data:
        state["eof"] = True
        return 0
    out.write(data)
    return len(data)


def pump(limit, idle=None):
    """Read for up to `limit` seconds, stopping early after `idle` quiet."""
    start = time.time()
    last = start
    while not state["eof"]:
        now = time.time()
        if now - start >= limit:
            return
        if idle is not None and now - last >= idle:
            return
        if read_once(0.2):
            last = time.time()


def trickle(limit, rate, idle=None):
    """Read `rate` bytes per second, and no more, for `limit` seconds.

    With `idle` set, return early once that many seconds pass without any
    data arriving: a throttled but healthy client that has been served its
    whole replay should stop waiting rather than burn the rest of `limit`.
    """
    start = time.time()
    last = start
    slot = 0
    while not state["eof"]:
        now = time.time()
        if now - start >= limit:
            return
        if idle is not None and now - last >= idle:
            return
        got = 0
        while got < rate and not state["eof"]:
            n = read_once(0.05, size=min(rate - got, 4096))
            if n:
                got += n
                last = time.time()
            if time.time() - start >= slot + 1:
                break
        slot += 1
        remaining = start + slot - time.time()
        if remaining > 0:
            time.sleep(remaining)


def unescape(text):
    return text.replace("\\n", "\n").replace("\\r", "\r").replace("\\x1c", "\x1c")


for action in actions:
    if state["eof"]:
        break
    if action.startswith("stall:"):
        time.sleep(float(action.split(":", 1)[1]))
    elif action.startswith("read:"):
        pump(float(action.split(":", 1)[1]))
    elif action.startswith("quiet:"):
        _, idle, limit = action.split(":", 2)
        pump(float(limit), idle=float(idle))
    elif action.startswith("trickle:"):
        parts = action.split(":")
        limit, rate = float(parts[1]), int(parts[2])
        trickle(limit, rate, idle=float(parts[3]) if len(parts) > 3 else None)
    elif action.startswith("send:"):
        os.write(fd, unescape(action[len("send:"):]).encode())
    elif action == "detach":
        os.write(fd, b"\x1c")
    else:
        sys.stderr.write("pty_client: unknown action %r\n" % action)
        sys.exit(2)

# Give the client a moment to exit on its own, then make sure it is gone.
for _ in range(20):
    done, _ = os.waitpid(pid, os.WNOHANG)
    if done == pid:
        break
    pump(0.1)
else:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)

out.close()
