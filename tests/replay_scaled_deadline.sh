#!/bin/sh
#
# Regression test: the total replay deadline scales with the size of the
# replay, so a healthy client on a merely slow link is not dropped just for
# having a large scrollback.
#
# The master bounds a whole scrollback replay by
#
#     started + CLIENT_REPLAY_TIMEOUT + bytes / CLIENT_REPLAY_MIN_RATE
#
# A fixed deadline would cut off any replay that takes longer than the grace
# period, however fast the client is actually reading; the scaled term makes
# the bound a minimum sustained rate instead. This test drives exactly that
# case: a client that reads steadily at well above CLIENT_REPLAY_MIN_RATE but
# still needs longer than CLIENT_REPLAY_TIMEOUT to swallow the whole replay.
# With the scaled term it gets everything; with a fixed deadline it is cut
# off mid-replay and never sees the end marker.
#
# Phases:
#   1. create a session whose scrollback is big enough that reading it at the
#      throttled rate below must take longer than the fixed grace period
#   2. reattach with a client throttled to READ_RATE bytes per second, and
#      require the complete replay: both OSC markers and the scrollback tail
#   3. the master must still be alive with its socket in place, and a second
#      plain attach must still get a complete replay
#
# Usage: tests/replay_scaled_deadline.sh [path-to-dtach]
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

# The fixed grace period, read from the source so the test follows the
# implementation. The scaled term is what this test is about, so its absence
# is not defaulted away: MIN_RATE 0 means "no scaling in this build", and the
# assertions below then expect the replay to be cut short.
TOTAL=$(sed -n 's/^#define CLIENT_REPLAY_TIMEOUT[ 	]*\([0-9][0-9]*\).*/\1/p' \
	"$SRCDIR/master.c" 2>/dev/null | head -1)
[ -n "${TOTAL:-}" ] || TOTAL=30
MIN_RATE_EXPR=$(sed -n \
	's/^#define CLIENT_REPLAY_MIN_RATE[ 	]*\(.*\)$/\1/p' \
	"$SRCDIR/master.c" 2>/dev/null | head -1)
if [ -n "${MIN_RATE_EXPR:-}" ]; then
	MIN_RATE=$(( MIN_RATE_EXPR ))
else
	MIN_RATE=0
fi

# Bytes per second the reattaching client is willing to read. Comfortably
# above the minimum sustained rate the deadline allows for, so this client is
# healthy by the rule and must never be dropped.
READ_RATE=122880
# Lines of output to fill the scrollback with. seq 1 600000 through a tty is
# about 4.69 MB, which at READ_RATE takes roughly 38 s: longer than the fixed
# grace period, and well inside the scaled deadline (about 101 s).
LINES=600000
# Upper bound on the throttled read, and how long without data before it
# gives up waiting for more.
READ_FOR=75
READ_IDLE=3

TAG=dtachtest-scaled-$$
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
}

if [ "$MIN_RATE" -gt 0 ]; then
	echo "deadline under test: ${TOTAL}s + size at ${MIN_RATE}B/s"
else
	echo "deadline under test: ${TOTAL}s, fixed (no scaled term in master.c)"
fi

##############################################################################
# Phase 1: build a scrollback that cannot be read at READ_RATE inside the
# fixed grace period.
##############################################################################
echo "== phase 1: create a session with a large scrollback =="
python3 "$DRIVER" "$WORK/create.out" \
	"$DTACH -A $SOCK -r none -b 16m /bin/sh" \
	"quiet:1:10" \
	"send:seq 1 $LINES\\n" \
	"quiet:3:180" \
	"detach" \
	"read:2"

REPLAY_BYTES=$(filesize "$WORK/create.out")
MASTER_PID=$(our_dtach_pids | head -1)
if [ -z "$MASTER_PID" ] || [ ! -S "$SOCK" ]; then
	echo "FAIL: no live session after setup"
	exit 1
fi
echo "   master $MASTER_PID alive, scrollback about $REPLAY_BYTES bytes"

# The whole point of the test is a replay that outlives the fixed grace
# period at this reading rate. If the fill came up short the test would pass
# with or without the scaled term, so refuse to run rather than prove nothing.
NEEDED=$(( READ_RATE * (TOTAL + 5) ))
if [ "$REPLAY_BYTES" -lt "$NEEDED" ]; then
	echo "FAIL: scrollback is $REPLAY_BYTES bytes; at ${READ_RATE}B/s that"
	echo "      finishes inside the ${TOTAL}s grace period, so the scaled"
	echo "      deadline would never be exercised. Need $NEEDED bytes:"
	echo "      raise LINES."
	exit 1
fi
note_pass "replay needs about $((REPLAY_BYTES / READ_RATE))s at ${READ_RATE}B/s, past the ${TOTAL}s grace"

##############################################################################
# Phase 2: the case the scaled deadline exists for. Steady reading, above the
# minimum rate, but longer than the fixed grace period.
##############################################################################
echo "== phase 2: reattach, client reads steadily at ${READ_RATE}B/s =="
start=$(date +%s)
python3 "$DRIVER" "$WORK/throttled.out" \
	"$DTACH -a $SOCK -r none" \
	"trickle:$READ_FOR:$READ_RATE:$READ_IDLE" \
	"detach" \
	"read:2"
elapsed=$(( $(date +%s) - start ))
got=$(filesize "$WORK/throttled.out")
[ "$elapsed" -gt 0 ] || elapsed=1
echo "   throttled client received $got bytes in ${elapsed}s ($((got / elapsed))B/s)"

# The replay really did outlast the fixed grace period: without the scaled
# term this client would already have been dropped.
if [ "$elapsed" -gt "$TOTAL" ]; then
	note_pass "replay took ${elapsed}s, longer than the ${TOTAL}s grace period"
else
	note_fail "replay finished in ${elapsed}s, inside the ${TOTAL}s grace"
	note_fail "period, so this run proves nothing about the scaled term"
fi
# And it stayed above the minimum sustained rate, so it is a client the
# deadline is meant to keep, not one it is meant to drop.
if [ "$MIN_RATE" -gt 0 ] && [ $((got / elapsed)) -le "$MIN_RATE" ]; then
	note_fail "client averaged $((got / elapsed))B/s, at or below the"
	note_fail "${MIN_RATE}B/s minimum; it is not a healthy client"
else
	note_pass "client stayed above the ${MIN_RATE}B/s minimum"
fi
check_full_replay "phase 2" "$WORK/throttled.out"

##############################################################################
# Phase 3: the session is untouched by all of that.
##############################################################################
echo "== phase 3: master survived, and a plain attach still works =="
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

python3 "$DRIVER" "$WORK/plain.out" \
	"$DTACH -a $SOCK -r none" \
	"quiet:3:120" \
	"detach" \
	"read:2"
echo "   plain client received $(filesize "$WORK/plain.out") bytes"
check_full_replay "phase 3" "$WORK/plain.out"

##############################################################################
echo "== result =="
if [ "$rc" -eq 0 ]; then
	echo "PASS: a steady client above the minimum rate gets its whole replay"
	echo "      even when it takes longer than the fixed grace period"
else
	echo "FAIL: see the failures above"
	for f in throttled plain; do
		if [ -f "$WORK/$f.out" ]; then
			echo "--- last 120 bytes seen by the $f client ---"
			tail -c 120 "$WORK/$f.out" |
				LC_ALL=C tr -c '[:print:][:space:]' '.'
			echo
		fi
	done
fi

exit $rc
