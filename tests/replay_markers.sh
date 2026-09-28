#!/bin/sh
#
# Regression test (v0.9.9): with DTACH_REV_STRIP_MARKERS=1 the attach client
# keeps the replay markers off the terminal it draws on.
#
# The markers are a protocol for clients that parse them (TerminaLLM finds
# the replay with them), so by default they still pass through. A human
# terminal has no use for them, and one whose OSC parser gives up on a
# non-numeric selector consumes ESC ] d and prints the rest: after 0.9.8's
# fence, "tach-rev;replay-end" showed up on every "Open Tab on Server"
# attach. Checks:
#
#   1. stripped reattach: the replay, the fence and other OSC sequences reach
#      the terminal; no byte of a replay marker does, including a live marker
#      a program prints in two writes 300 ms apart (split across reads)
#   2. default reattach: the markers still reach the terminal unchanged, so a
#      parsing client (the app) keeps working
#
# Usage: tests/replay_markers.sh [path-to-dtach]
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

TAG=dtachtest-markers-$$
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

cleanup() {
	for pid in $(ps_pid_args | awk -v b="$DTACH" -v t="$TAG" \
		'$2 == b && index($0, t) > 0 { print $1 }'); do
		kill "$pid" 2>/dev/null
	done
	rm -f "$SOCK"
	rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# The program under dtach: prints scrollback with an OSC 0 title, and on
# "M" prints a replay-start marker split over two writes, then a
# replay-abort marker, then "after".
cat > "$WORK/session.py" <<'PYEOF'
import os
import time
import tty

tty.setraw(0)
os.write(1, b"agent starting\r\n\x1b]0;my title\x07prompt$ ")
start = b"\x1b]dtach-rev;replay-start\x07"
while True:
    data = os.read(0, 1024)
    if not data:
        break
    if b"M" in data:
        os.write(1, start[:9])
        time.sleep(0.3)
        os.write(1, start[9:] + b"x\x1b]dtach-rev;replay-abort\x07after\r\n")
PYEOF

has() {
	python3 - "$1" "$2" <<'PYEOF'
import sys
data = open(sys.argv[1], "rb").read()
needle = sys.argv[2].encode().decode("unicode_escape").encode("latin1")
sys.exit(0 if needle in data else 1)
PYEOF
}

"$DTACH" -n "$SOCK" -r none -b 256k python3 "$WORK/session.py"
sleep 0.5

##############################################################################
echo "== 1: DTACH_REV_STRIP_MARKERS=1 reattach =="
python3 "$DRIVER" "$WORK/stripped.out" \
	"env DTACH_REV_STRIP_MARKERS=1 $DTACH -a $SOCK -r none" \
	"answer:1.5" \
	"send:M" \
	"answer:1.5" \
	"detach" \
	"read:1"

if has "$WORK/stripped.out" 'agent starting'; then
	note_pass "replayed scrollback reached the terminal"
else
	note_fail "replayed scrollback did not reach the terminal"
fi
if has "$WORK/stripped.out" '\x1b[5n'; then
	note_pass "the A19 fence still followed the replay"
else
	note_fail "no fence after the replay"
fi
if has "$WORK/stripped.out" '\x1b]0;my title\x07'; then
	note_pass "another OSC sequence passed unchanged"
else
	note_fail "the OSC 0 title sequence was altered or lost"
fi
if has "$WORK/stripped.out" 'xafter'; then
	note_pass "live output around a split live marker arrived"
else
	note_fail "live output around the live markers is missing"
fi
bad=
for frag in 'dtach-rev' 'tach-rev;replay' 'replay-end' 'replay-start' \
	'replay-abort' '\x1b]d'; do
	if has "$WORK/stripped.out" "$frag"; then
		bad="$bad $frag"
	fi
done
if [ -z "$bad" ]; then
	note_pass "no replay marker byte reached the terminal"
else
	note_fail "marker bytes reached the terminal:$bad"
fi

##############################################################################
echo "== 2: default reattach keeps the markers for parsing clients =="
python3 "$DRIVER" "$WORK/default.out" \
	"$DTACH -a $SOCK -r none" \
	"answer:1.5" \
	"detach" \
	"read:1"
if has "$WORK/default.out" '\x1b]dtach-rev;replay-start\x07' &&
	has "$WORK/default.out" '\x1b]dtach-rev;replay-end\x07'; then
	note_pass "replay markers passed through unchanged"
else
	note_fail "replay markers missing without DTACH_REV_STRIP_MARKERS"
fi

##############################################################################
echo "== result =="
if [ "$rc" -eq 0 ]; then
	echo "PASS: replay markers stay off a terminal that asked for it"
else
	echo "FAIL: see the failures above"
	LC_ALL=C tr -c '[:print:]' '.' < "$WORK/stripped.out"
	echo
fi
exit $rc
