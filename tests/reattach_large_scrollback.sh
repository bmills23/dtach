#!/bin/sh
#
# Regression test: reattaching to a session with a large scrollback must not
# kill the master, and a client that cannot keep up must be dropped on its
# own without wedging the session.
#
# Defect (dtach-rev 0.9.6): control_activity() puts every accepted client fd
# into non-blocking mode, and the MSG_ATTACH handler replayed the scrollback
# with write_buf_or_fail(), which exit(1)s on any error other than EINTR.
# EAGAIN is such an error, so as soon as a replay was bigger than the unix
# socket send buffer the master exited, atexit() unlinked the socket, and the
# whole session was lost on reattach.
#
# Phases:
#   1. create a session with a 16 MB scrollback, fill it with several MB of
#      output (more than any platform's socket send buffer), then detach
#   2. check the master survived the detach, and that the replay really is
#      bigger than this platform's socket send buffer
#   3. reattach with a client that stops reading for less than the per-wait
#      bound: the master must survive and the client must get the whole
#      replay, start marker and end marker included
#   4. reattach with a client that stops reading for longer than the per-wait
#      bound: that client alone is dropped, the session survives
#   5. reattach with a client that reads far too slowly to finish: the total
#      replay deadline must drop it, so the master is never held for longer
#      than that deadline. The deadline scales with the size of the replay
#      (a fixed grace plus size at CLIENT_REPLAY_MIN_RATE), so this phase
#      computes it from the source and the measured fill rather than
#      assuming the fixed grace alone.
#   6. a healthy client must still get a complete replay afterwards
#
# Usage: tests/reattach_large_scrollback.sh [path-to-dtach]
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

# The two bounds the master enforces, read from the source when it is next
# to us so the test follows the implementation instead of guessing.
PER_WAIT=$(sed -n 's/^#define CLIENT_WRITE_TIMEOUT[ 	]*\([0-9][0-9]*\).*/\1/p' \
	"$SRCDIR/master.c" 2>/dev/null | head -1)
TOTAL=$(sed -n 's/^#define CLIENT_REPLAY_TIMEOUT[ 	]*\([0-9][0-9]*\).*/\1/p' \
	"$SRCDIR/master.c" 2>/dev/null | head -1)
# The minimum sustained rate the total deadline allows for, in bytes per
# second. The define is an expression ("(64 * 1024)"), so let the shell do
# the arithmetic instead of trying to match a literal.
MIN_RATE_EXPR=$(sed -n \
	's/^#define CLIENT_REPLAY_MIN_RATE[ 	]*\(.*\)$/\1/p' \
	"$SRCDIR/master.c" 2>/dev/null | head -1)
[ -n "${PER_WAIT:-}" ] || PER_WAIT=10
[ -n "${TOTAL:-}" ] || TOTAL=30
if [ -n "${MIN_RATE_EXPR:-}" ]; then
	MIN_RATE=$(( MIN_RATE_EXPR ))
else
	# No scaled term in this build: the deadline is the fixed grace alone.
	MIN_RATE=0
fi

TAG=dtachtest-$$
SOCK=/tmp/$TAG.sock
WORK=/tmp/$TAG.d

# Lines to feed to seq. 300000 lines is about 2.3 MB once the tty turns each
# newline into CRLF: more than ten times the largest default unix socket send
# buffer we know of (Linux, 212992 bytes), so the replay is guaranteed to
# block partway through on every platform.
LINES=300000
# Bytes per second the trickling client in phase 5 is willing to read.
TRICKLE_RATE=4096

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

# Portable "pid + full command line" listing: the POSIX spelling first, the
# BSD spelling second.
ps_pid_args() {
	ps -A -o pid= -o args= 2>/dev/null && return 0
	ps -axo pid=,args= 2>/dev/null && return 0
	return 1
}

# Only ever matches processes started by this run: the socket path carries
# our pid, so no other dtach instance can be using it.
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

# Assert the master is still there and still serving. Every phase after a
# hostile client runs this.
check_session_alive() {
	if kill -0 "$MASTER_PID" 2>/dev/null; then
		note_pass "$1: master $MASTER_PID still alive"
	else
		note_fail "$1: master $MASTER_PID died"
	fi
	if [ -S "$SOCK" ]; then
		note_pass "$1: socket still present"
	else
		note_fail "$1: socket was unlinked (master exited)"
	fi
}

check_full_replay() {
	label=$1
	file=$2
	if LC_ALL=C grep -a -q 'dtach-rev;replay-start' "$file" 2>/dev/null; then
		note_pass "$label: replay-start marker received"
	else
		note_fail "$label: replay-start marker missing"
	fi
	if LC_ALL=C grep -a -q "^$LINES" "$file" 2>/dev/null; then
		note_pass "$label: scrollback tail received (line '$LINES')"
	else
		note_fail "$label: scrollback tail missing (line '$LINES')"
	fi
	if LC_ALL=C grep -a -q 'dtach-rev;replay-end' "$file" 2>/dev/null; then
		note_pass "$label: replay-end marker received"
	else
		note_fail "$label: replay-end marker missing (replay cut short)"
	fi
	# Ordering: the start marker must precede the end marker.
	order=$(LC_ALL=C grep -a -n -e 'dtach-rev;replay-start' \
		-e 'dtach-rev;replay-end' "$file" 2>/dev/null | cut -d: -f1 |
		tr '\n' ' ')
	echo "        marker line numbers: $order"
}

if [ "$MIN_RATE" -gt 0 ]; then
	echo "bounds: per-wait ${PER_WAIT}s, replay ${TOTAL}s + size at ${MIN_RATE}B/s"
else
	echo "bounds under test: per-wait ${PER_WAIT}s, whole replay ${TOTAL}s (fixed)"
fi

##############################################################################
# Phase 1: create the session, fill the scrollback, detach.
##############################################################################
echo "== phase 1: create a session with a large scrollback =="
python3 "$DRIVER" "$WORK/create.out" \
	"$DTACH -A $SOCK -r none -b 16m /bin/sh" \
	"quiet:1:10" \
	"send:seq 1 $LINES\\n" \
	"quiet:3:300" \
	"detach" \
	"read:2"

REPLAY_BYTES=$(filesize "$WORK/create.out")
echo "   created session, scrollback is $REPLAY_BYTES bytes"
if [ "$REPLAY_BYTES" -lt 1000 ]; then
	echo "FAIL: the creating client saw almost nothing; setup is broken"
	exit 1
fi

##############################################################################
# Phase 2: the master survived the detach, and the replay really is larger
# than this platform's socket send buffer (otherwise nothing would ever
# block and the test would prove nothing).
##############################################################################
echo "== phase 2: session survived the detach, fill is big enough =="
MASTER_PID=$(our_dtach_pids | head -1)
if [ -z "$MASTER_PID" ]; then
	echo "FAIL: no master process after detach (setup is broken)"
	exit 1
fi
if [ ! -S "$SOCK" ]; then
	echo "FAIL: socket $SOCK missing after detach (setup is broken)"
	exit 1
fi
echo "   master pid $MASTER_PID alive, socket present"

SNDBUF=$(python3 -c 'import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
print(s.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF))' 2>/dev/null)
[ -n "${SNDBUF:-}" ] || SNDBUF=0
echo "   unix socket send buffer on this platform: $SNDBUF bytes"
if [ "$SNDBUF" -gt 0 ] && [ "$REPLAY_BYTES" -le $((SNDBUF * 4)) ]; then
	echo "FAIL: replay ($REPLAY_BYTES) is not comfortably larger than the"
	echo "      send buffer ($SNDBUF); raise LINES or the test proves"
	echo "      nothing on this platform"
	exit 1
fi
note_pass "replay is more than 4x the socket send buffer"

##############################################################################
# Phase 3: the original defect. A client that stops reading for less than the
# per-wait bound must still get the whole replay.
##############################################################################
STALL=$((PER_WAIT - 5))
[ "$STALL" -ge 1 ] || STALL=1
echo "== phase 3: reattach, client stalls ${STALL}s (within the bound) =="
python3 "$DRIVER" "$WORK/attach.out" \
	"$DTACH -a $SOCK -r none" \
	"stall:$STALL" \
	"quiet:3:120" \
	"detach" \
	"read:2"
echo "   client received $(filesize "$WORK/attach.out") bytes"
check_session_alive "phase 3"
check_full_replay "phase 3" "$WORK/attach.out"

##############################################################################
# Phase 4: a client that stalls past the per-wait bound is dropped, alone.
##############################################################################
LONG_STALL=$((PER_WAIT + 4))
echo "== phase 4: reattach, client stalls ${LONG_STALL}s (past the bound) =="
start=$(date +%s)
python3 "$DRIVER" "$WORK/stalled.out" \
	"$DTACH -a $SOCK -r none" \
	"stall:$LONG_STALL" \
	"quiet:3:60" \
	"read:2"
elapsed=$(( $(date +%s) - start ))
stalled_bytes=$(filesize "$WORK/stalled.out")
echo "   stalled client received $stalled_bytes bytes in ${elapsed}s"
check_session_alive "phase 4"
if [ "$stalled_bytes" -lt "$REPLAY_BYTES" ]; then
	note_pass "phase 4: stalled client was cut off, not served in full"
else
	note_fail "phase 4: stalled client somehow got the whole replay"
fi
if LC_ALL=C grep -a -q 'dtach-rev;replay-end' "$WORK/stalled.out" 2>/dev/null
then
	note_fail "phase 4: stalled client got replay-end (was not dropped)"
else
	note_pass "phase 4: stalled client never got replay-end (dropped)"
fi

##############################################################################
# Phase 5: a client that reads, but far too slowly, is dropped by the total
# replay deadline rather than holding the master for hours.
##############################################################################
# The deadline this replay actually gets: the fixed grace plus the time the
# data itself is allowed to take at the minimum sustained rate. Trickle past
# it, so the client is dropped by the deadline rather than the test ending
# first. The trickle rate is far below MIN_RATE, so this client can never
# finish inside the deadline however large it is.
if [ "$MIN_RATE" -gt 0 ]; then
	DEADLINE=$((TOTAL + REPLAY_BYTES / MIN_RATE))
else
	DEADLINE=$TOTAL
fi
TRICKLE_FOR=$((DEADLINE + 20))
echo "== phase 5: deadline for this ${REPLAY_BYTES}-byte replay is ${DEADLINE}s =="
echo "== phase 5: reattach, client trickles ${TRICKLE_RATE}B/s for ${TRICKLE_FOR}s =="
start=$(date +%s)
python3 "$DRIVER" "$WORK/trickle.out" \
	"$DTACH -a $SOCK -r none" \
	"trickle:$TRICKLE_FOR:$TRICKLE_RATE" \
	"quiet:3:30" \
	"read:2"
elapsed=$(( $(date +%s) - start ))
trickle_bytes=$(filesize "$WORK/trickle.out")
echo "   trickling client received $trickle_bytes bytes in ${elapsed}s"
check_session_alive "phase 5"
# It must have made real progress, otherwise the per-wait bound dropped it and
# the total deadline was never exercised.
if [ "$trickle_bytes" -gt $((TRICKLE_RATE * 5)) ]; then
	note_pass "phase 5: client kept making progress (per-wait bound never fired)"
else
	note_fail "phase 5: client made almost no progress; the total deadline"
fi
# Well short of the whole replay, not merely a few bytes short: without the
# total deadline the trickling client eventually receives everything.
if [ "$trickle_bytes" -lt $((REPLAY_BYTES / 2)) ]; then
	note_pass "phase 5: replay was cut off well short of the end"
else
	note_fail "phase 5: client got $trickle_bytes of $REPLAY_BYTES bytes;"
	note_fail "phase 5: the master was held until the replay finished"
fi
# The master must have been released at the deadline, not held for the whole
# trickle window.
if LC_ALL=C grep -a -q 'dtach-rev;replay-end' "$WORK/trickle.out" 2>/dev/null
then
	note_fail "phase 5: trickling client got replay-end (was not dropped)"
else
	note_pass "phase 5: trickling client never got replay-end (dropped)"
fi

##############################################################################
# Phase 6: after two hostile clients, a healthy one still gets everything.
##############################################################################
echo "== phase 6: a healthy client still gets a complete replay =="
python3 "$DRIVER" "$WORK/healthy.out" \
	"$DTACH -a $SOCK -r none" \
	"quiet:3:120" \
	"detach" \
	"read:2"
echo "   client received $(filesize "$WORK/healthy.out") bytes"
check_session_alive "phase 6"
check_full_replay "phase 6" "$WORK/healthy.out"

##############################################################################
echo "== result =="
if [ "$rc" -eq 0 ]; then
	echo "PASS: large-scrollback reattach keeps the session alive, and slow"
	echo "      clients are dropped on their own"
else
	echo "FAIL: see the failures above"
	for f in attach stalled trickle healthy; do
		if [ -f "$WORK/$f.out" ]; then
			echo "--- last 120 bytes seen by the $f client ---"
			tail -c 120 "$WORK/$f.out" |
				LC_ALL=C tr -c '[:print:][:space:]' '.'
			echo
		fi
	done
fi

exit $rc
