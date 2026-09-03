#!/bin/sh
#
# Regression test: a slow reattach must not be reported as an idle program.
#
# While the master replays the scrollback to a slow client it cannot read the
# pty, so wall-clock time passes with the program's output sitting unread.
# The idle check in master_process() runs at the top of the loop, before
# pty_activity(), so on the iteration right after a replay it would compare
# "now" against a last-output timestamp that is stale by exactly the length of
# the replay, and fire the idle callback even though the program had produced
# output the whole time. With -I 300 that needs a replay of five minutes;
# with -I 5, as used here, a replay held by one stalled client is enough.
#
# The fix pushes the idle clock forward by the time the master spent blocked
# in the replay, so a window in which the program could not be observed is
# never counted as idle time.
#
# Usage: tests/replay_idle_clock.sh [path-to-dtach]
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

if [ ! -x "$DTACH" ] || [ ! -f "$DRIVER" ]; then
	echo "FAIL: need a dtach binary at $DTACH and the driver at $DRIVER"
	exit 1
fi

PER_WAIT=$(sed -n 's/^#define CLIENT_WRITE_TIMEOUT[ 	]*\([0-9][0-9]*\).*/\1/p' \
	"$SRCDIR/master.c" 2>/dev/null | head -1)
[ -n "${PER_WAIT:-}" ] || PER_WAIT=10

TAG=dtachtest-idle-$$
SOCK=/tmp/$TAG.sock
WORK=/tmp/$TAG.d
MARKER=$WORK/fired
CB=$WORK/idle-callback.sh

# Idle timeout for the session, in seconds. Must be well under the time the
# master spends blocked on the stalled client below.
IDLE=5
# Enough output that the replay cannot fit in a socket send buffer.
LINES=30000

rm -rf "$WORK"
mkdir -p "$WORK"

rc=0

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

cat > "$CB" <<EOF
#!/bin/sh
echo "fired at \$(date +%s) after \$DTACH_IDLE_SECS s" >> "$MARKER"
EOF
chmod +x "$CB"

##############################################################################
# Set up: a session with a short idle timeout, a scrollback too big to replay
# in one go, and a ticker so the program is demonstrably never idle.
##############################################################################
echo "== setup: session with -I $IDLE and a ticking program =="
python3 "$DRIVER" "$WORK/create.out" \
	"$DTACH -A $SOCK -r none -b 16m -I $IDLE -C $CB /bin/sh" \
	"quiet:1:10" \
	"send:seq 1 $LINES\\n" \
	"quiet:3:120" \
	"send:while true; do echo tick; sleep 1; done &\\n" \
	"read:4" \
	"detach" \
	"read:2"

MASTER_PID=$(our_dtach_pids | head -1)
if [ -z "$MASTER_PID" ] || [ ! -S "$SOCK" ]; then
	echo "FAIL: no live session after setup"
	exit 1
fi
bytes=$(wc -c < "$WORK/create.out" | tr -d ' ')
echo "   master $MASTER_PID alive, scrollback about $bytes bytes"

# Control: the ticker prints every second, so the program is never idle for
# $IDLE seconds and the callback must not have fired yet.
if [ -f "$MARKER" ]; then
	echo "   FAIL: idle callback fired before any replay:"
	cat "$MARKER"
	rc=1
else
	echo "   PASS: no idle callback while the ticker is being read"
fi
rm -f "$MARKER"

##############################################################################
# The test: reattach with a client that stops reading, so the master is stuck
# in the replay for the per-wait bound. The ticker keeps producing output the
# whole time, so any idle callback in this window is a false one.
##############################################################################
STALL=$((PER_WAIT + 4))
echo "== reattach with a client that stalls ${STALL}s (idle timeout ${IDLE}s) =="
python3 "$DRIVER" "$WORK/stalled.out" \
	"$DTACH -a $SOCK -r none" \
	"stall:$STALL" \
	"quiet:2:30" \
	"read:2"

# Let the master get through the loop iteration right after the replay.
python3 -c 'import time; time.sleep(4)'

if kill -0 "$MASTER_PID" 2>/dev/null; then
	echo "   PASS: master still alive"
else
	echo "   FAIL: master died"
	rc=1
fi

if [ -f "$MARKER" ]; then
	echo "   FAIL: idle callback fired even though the program never"
	echo "         stopped producing output:"
	sed 's/^/         /' "$MARKER"
	rc=1
else
	echo "   PASS: no false idle callback after a blocked replay"
fi

echo "== result =="
if [ "$rc" -eq 0 ]; then
	echo "PASS: a blocked replay is not counted as idle time"
else
	echo "FAIL: see the failures above"
fi

exit $rc
