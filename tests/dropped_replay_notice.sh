#!/bin/sh
#
# Regression test: a client dropped part-way through a scrollback replay is
# told the truth about it.
#
# The master protects the session from a client that stalls: rather than
# blocking forever on the write, it bounds the replay and drops the client by
# closing its socket. attach.c used to read that close as plain EOF and print
# "[EOF - dtach terminating]" before exiting 0, so a user whose reattach was
# cut off was told the session was gone while the master was still running
# it. The client had also received a replay-start marker with no replay-end,
# leaving anything parsing those markers unable to tell a dropped replay from
# a dead session.
#
# So a client that hits EOF between replay-start and replay-end must say it
# was interrupted, emit a replay-abort marker for parsers, and exit non-zero,
# and it must not claim the session terminated.
#
# Phases:
#   1. create a session and fill its scrollback well past the socket send
#      buffer, so a replay to a client that is not reading must block
#   2. reattach with a client that stops reading for longer than the per-wait
#      bound, so the master drops it mid-replay, then let it drain: it must
#      report the interruption and never claim the session terminated
#   3. the session is untouched: master alive, socket present, and a healthy
#      client still gets a complete replay with no interruption notice
#
# Usage: tests/dropped_replay_notice.sh [path-to-dtach]
# Exit status: 0 = PASS, 1 = FAIL.

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRCDIR=$(cd "$HERE/.." && pwd)
DRIVER=$HERE/pty_client.py

DTACH=${1:-$SRCDIR/dtach}
case "$DTACH" in
/*) ;;
*) DTACH=$(cd "$(dirname "$DTACH")" && pwd)/$(basename "$DTACH") ;;
esac

if [ ! -x "$DTACH" ]; then
	echo "FAIL: no dtach binary at $DTACH (run ./configure && make first)"
	exit 1
fi
if [ ! -f "$DRIVER" ]; then
	echo "FAIL: missing pty driver $DRIVER"
	exit 1
fi

# How long the master waits on a single stalled write before dropping the
# client, read from the source so the test follows the implementation.
PER_WAIT=$(sed -n 's/^#define CLIENT_WRITE_TIMEOUT[ 	]*\([0-9][0-9]*\).*/\1/p' \
	"$SRCDIR/master.c" 2>/dev/null | head -1)
[ -n "${PER_WAIT:-}" ] || PER_WAIT=10

# Lines to feed to seq. 200000 lines is about 1.3 MB once the tty turns each
# newline into CRLF, several times the largest default unix socket send
# buffer we know of, so the replay is certain to block part-way through.
LINES=200000
# Stall for longer than the per-wait bound, so the drop is the master's
# doing and not the test giving up.
LONG_STALL=$((PER_WAIT + 4))

TAG=dtachtest-drop-$$
SOCK=/tmp/$TAG.sock
WORK=/tmp/$TAG.d

rm -rf "$WORK"
mkdir -p "$WORK"

rc=0
note_fail() {
	echo "   FAIL: $1"
	rc=1
}
note_pass() {
	echo "   PASS: $1"
}

ps_pid_args() {
	ps -A -o pid= -o args= 2>/dev/null && return 0
	ps -axo pid=,args= 2>/dev/null && return 0
	return 1
}

our_dtach_pids() {
	ps_pid_args | awk -v b="$DTACH" -v s="$SOCK" \
		'$2 == b && index($0, s) > 0 { print $1 }'
}

cleanup() {
	for pid in $(our_dtach_pids); do
		kill "$pid" 2>/dev/null
	done
	rm -f "$SOCK" "$SOCK.lastoutput"
	rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

filesize() {
	if [ -f "$1" ]; then
		wc -c < "$1" | tr -d ' '
	else
		echo 0
	fi
}

saw() {
	LC_ALL=C grep -a -q "$1" "$2" 2>/dev/null
}

##############################################################################
# Phase 1: a session whose replay cannot fit in a socket buffer.
##############################################################################
echo "== phase 1: create a session with a large scrollback =="
python3 "$DRIVER" "$WORK/create.out" \
	"$DTACH -A $SOCK -r none -b 4m /bin/sh" \
	"quiet:1:10" \
	"send:seq 1 $LINES\\n" \
	"quiet:3:120" \
	"detach" \
	"read:2"

REPLAY_BYTES=$(filesize "$WORK/create.out")
MASTER_PID=$(our_dtach_pids | head -1)
if [ -z "$MASTER_PID" ] || [ ! -S "$SOCK" ]; then
	echo "FAIL: no live session after setup"
	exit 1
fi
echo "   master $MASTER_PID alive, scrollback about $REPLAY_BYTES bytes"

SNDBUF=$(python3 -c 'import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
print(s.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF))' 2>/dev/null)
[ -n "${SNDBUF:-}" ] || SNDBUF=212992
if [ "$REPLAY_BYTES" -lt $((SNDBUF * 4)) ]; then
	echo "FAIL: scrollback is $REPLAY_BYTES bytes against a ${SNDBUF}-byte"
	echo "      send buffer, so the replay might never block and the"
	echo "      client would never be dropped. Raise LINES."
	exit 1
fi
note_pass "replay is more than 4x the ${SNDBUF}-byte socket send buffer"

##############################################################################
# Phase 2: the client that gets dropped mid-replay.
##############################################################################
echo "== phase 2: reattach, client stalls ${LONG_STALL}s (past the ${PER_WAIT}s bound) =="
python3 "$DRIVER" "$WORK/dropped.out" \
	"$DTACH -a $SOCK -r none" \
	"stall:$LONG_STALL" \
	"quiet:2:20" \
	"read:2"
echo "   dropped client received $(filesize "$WORK/dropped.out") bytes"

if saw 'dtach-rev;replay-start' "$WORK/dropped.out"; then
	note_pass "client did start a replay (replay-start received)"
else
	note_fail "client never saw replay-start, so it was not dropped"
	note_fail "mid-replay and this run proves nothing"
fi
if saw 'dtach-rev;replay-end' "$WORK/dropped.out"; then
	note_fail "client got replay-end, so it was never dropped"
else
	note_pass "client never got replay-end (dropped mid-replay)"
fi
if saw 'dtach terminating' "$WORK/dropped.out"; then
	note_fail "client was told the session terminated, but the master is"
	note_fail "still running it"
else
	note_pass "client was not told the session terminated"
fi
if saw 'reattach interrupted' "$WORK/dropped.out"; then
	note_pass "client reported the reattach was interrupted"
else
	note_fail "client said nothing about the interrupted reattach"
fi
if saw 'dtach-rev;replay-abort' "$WORK/dropped.out"; then
	note_pass "replay-abort marker emitted for marker parsers"
else
	note_fail "no replay-abort marker: a parser still sees replay-start"
	note_fail "with no end and no way to tell why"
fi

##############################################################################
# Phase 3: the session and a healthy client are unaffected.
##############################################################################
echo "== phase 3: the session survived, a healthy client is unaffected =="
if kill -0 "$MASTER_PID" 2>/dev/null; then
	note_pass "master $MASTER_PID still alive"
else
	note_fail "master $MASTER_PID died"
fi
if [ -S "$SOCK" ]; then
	note_pass "socket still present"
else
	note_fail "socket was unlinked (master exited)"
fi

python3 "$DRIVER" "$WORK/healthy.out" \
	"$DTACH -a $SOCK -r none" \
	"quiet:3:60" \
	"detach" \
	"read:2"
echo "   healthy client received $(filesize "$WORK/healthy.out") bytes"
if saw 'dtach-rev;replay-end' "$WORK/healthy.out"; then
	note_pass "healthy client got a complete replay"
else
	note_fail "healthy client never got replay-end"
fi
if saw 'reattach interrupted' "$WORK/healthy.out"; then
	note_fail "healthy client was wrongly told its reattach was interrupted"
else
	note_pass "healthy client saw no interruption notice"
fi

##############################################################################
echo "== result =="
if [ "$rc" -eq 0 ]; then
	echo "PASS: a client dropped mid-replay is told the reattach was"
	echo "      interrupted, not that the session terminated"
else
	echo "FAIL: see the failures above"
	for f in dropped healthy; do
		if [ -f "$WORK/$f.out" ]; then
			echo "--- last 160 bytes seen by the $f client ---"
			tail -c 160 "$WORK/$f.out" |
				LC_ALL=C tr -c '[:print:][:space:]' '.'
			echo
		fi
	done
fi

exit $rc
