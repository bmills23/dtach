#!/bin/sh
#
# Regression test (A19, v0.9.8 corrective round): the replay gate drops a
# reattaching terminal's answers to replayed queries, and nothing else.
#
# tests/replay_answerback.sh covers the original stale answer. This covers the
# ways the first version of the fix went wrong, and the shapes it never tried:
#
#   1. live marker: a program prints the replay-start marker plus 40 bytes and
#      no end (a log being cat'ed, a hostile program). Keystrokes sent 2 s and
#      5 s later MUST reach it, and so must a live query's answer. The first
#      version dropped every keystroke until the client exited
#   2. replay-abort: the same with start + data + replay-abort (a nested
#      dtach client cut off mid-replay). Input stays live; a later reattach,
#      whose replay now holds those nested markers, still drops the stale
#      answer and forwards the live one
#   3. late answer: the terminal answers 1.5 s after the query (slow link or
#      renderer). The stale answer must still be dropped: the first version
#      stopped dropping 500 ms after the local pty accepted the replay
#   4. -r winch: a program that queries the terminal on SIGWINCH gets its
#      live answer on reattach, and only that one
#   5. multi-chunk: a 200KB replay with queries at its start, middle and end,
#      and typing sent while it streams. No stale answer gets through, and
#      the typing does
#   6. a terminal that never answers the fence (no DSR): the stale answer is
#      still dropped, typing passes, and a live answer after the bounded
#      fence window passes
#
# Usage: tests/replay_gate.sh [path-to-dtach]
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

TAG=dtachtest-gate-$$
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
check() {
	if [ "$1" = "$2" ]; then
		note_pass "$3"
	else
		note_fail "$3 (got $1, want $2)"
	fi
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
	rm -f /tmp/$TAG-*.sock
	rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# The program under dtach: raw mode, logs every input byte. argv: log, then
# flags. "query" asks DA1 at startup; "winch" asks DA1 on every SIGWINCH and
# logs "<winch>". Input "Q" asks DA1, "M" prints a bare replay-start marker
# plus 40 bytes, "A" the same closed by replay-abort, "B" 200KB of output with
# DA1 queries at its start, middle and end.
cat > "$WORK/session.py" <<'PYEOF'
import os
import signal
import sys
import tty

tty.setraw(0)
log = open(sys.argv[1], "ab", 0)
flags = sys.argv[2:]
START = b"\x1b]dtach-rev;replay-start\x07"
ABORT = b"\x1b]dtach-rev;replay-abort\x07"
if "winch" in flags:
    def on_winch(sig, frame):
        log.write(b"<winch>")
        os.write(1, b"redraw\r\n\x1b[c")
    signal.signal(signal.SIGWINCH, on_winch)
if "query" in flags:
    os.write(1, b"agent starting\r\n\x1b[c")
os.write(1, b"ready\r\n")
while True:
    data = os.read(0, 1024)
    if not data:
        break
    log.write(data)
    if b"Q" in data:
        os.write(1, b"live query\r\n\x1b[c")
    if b"M" in data:
        os.write(1, START + b"x" * 40)
    if b"A" in data:
        os.write(1, START + b"y" * 40 + ABORT + b"\r\n")
    if b"B" in data:
        line = b"line of agent output 0123456789\r\n"
        os.write(1, b"\x1b[c")
        for half in range(2):
            os.write(1, line * 3100)
            os.write(1, b"\x1b[c")
PYEOF

# count <file> <from> <to|end> <bytes>: occurrences of <bytes> in a range.
count() {
	python3 - "$@" <<'PYEOF'
import sys
data = open(sys.argv[1], "rb").read()
lo, hi = int(sys.argv[2]), sys.argv[3]
seg = data[lo:] if hi == "end" else data[lo:int(hi)]
print(seg.count(sys.argv[4].encode().decode("unicode_escape").encode("latin-1")))
PYEOF
}
# offset <file> <from> <bytes>: first offset of <bytes> at or after <from>.
offset() {
	python3 - "$@" <<'PYEOF'
import sys
data = open(sys.argv[1], "rb").read()
print(data.find(sys.argv[3].encode(), int(sys.argv[2])))
PYEOF
}
filesize() {
	if [ -f "$1" ]; then wc -c < "$1" | tr -d ' '; else echo 0; fi
}
ANS='\x1b[?1;2c'

# new_session <name> <flags...>: start a detached session logging to
# $WORK/<name>.log.
new_session() {
	name=$1
	shift
	: > "$WORK/$name.log"
	"$DTACH" -n "/tmp/$TAG-$name.sock" -r none -b 256k \
		python3 "$WORK/session.py" "$WORK/$name.log" "$@"
	sleep 0.3
}
attach() {
	name=$1
	shift
	python3 "$DRIVER" "$WORK/$name.out" "$DTACH -a /tmp/$TAG-$name.sock $ATTACH_FLAGS" "$@"
}
ATTACH_FLAGS="-r none"

##############################################################################
echo "== 1: a start marker in live output never locks the keyboard =="
new_session live
attach live "answer:1" "send:M" "answer:2" "send:HELLO" "answer:3" \
	"send:WORLD" "answer:1" "send:Q" "answer:1.5" "detach" "read:0.5"
L=$WORK/live.log
[ "$(offset "$L" 0 HELLO)" -ge 0 ] && note_pass "keystroke 2 s after the marker arrived" \
	|| note_fail "keystroke 2 s after the marker was dropped"
[ "$(offset "$L" 0 WORLD)" -ge 0 ] && note_pass "keystroke 5 s after the marker arrived" \
	|| note_fail "keystroke 5 s after the marker was dropped"
Q=$(offset "$L" 0 Q)
[ "$Q" -lt 0 ] && Q=$(filesize "$L")
check "$(count "$L" "$Q" end "$ANS")" 1 "a live query after the marker got its answer"

##############################################################################
echo "== 2: replay-abort in live output, then a reattach over it =="
new_session abort query
attach abort "answer:1" "send:A" "answer:2" "send:HELLO" "answer:1" \
	"detach" "read:0.5"
L=$WORK/abort.log
[ "$(offset "$L" 0 HELLO)" -ge 0 ] && note_pass "keystroke after start+abort arrived" \
	|| note_fail "keystroke after start+abort was dropped"
MARK=$(filesize "$L")
attach abort "answer:2" "send:Q" "answer:1.5" "detach" "read:0.5"
Q=$(offset "$L" "$MARK" Q)
[ "$Q" -lt 0 ] && { note_fail "reattach: keystroke Q never arrived"; Q=$(filesize "$L"); }
check "$(count "$L" "$MARK" "$Q" "$ANS")" 0 "reattach over nested start+abort: no stale answer"
check "$(count "$L" "$Q" end "$ANS")" 1 "reattach over nested start+abort: live answer forwarded"

##############################################################################
echo "== 3: an answer that arrives 1.5 s late is still dropped =="
new_session late query
attach late "answer:4:1.5" "send:Q" "answer:3:1.5" "detach" "read:0.5"
L=$WORK/late.log
A=$(sed -n 1p "$WORK/late.out.answers")
[ "${A:-0}" -ge 1 ] && note_pass "slow terminal answered the replayed query" \
	|| note_fail "slow terminal never answered, this run proves nothing"
Q=$(offset "$L" 0 Q)
[ "$Q" -lt 0 ] && { note_fail "keystroke Q never arrived"; Q=$(filesize "$L"); }
check "$(count "$L" 0 "$Q" "$ANS")" 0 "no late stale answer reached the program"
check "$(count "$L" "$Q" end "$ANS")" 1 "the late live answer reached the program"

##############################################################################
echo "== 4: -r winch: the live answer on reattach gets through =="
new_session winch query winch
ATTACH_FLAGS="-r winch"
attach winch "answer:3" "detach" "read:0.5"
ATTACH_FLAGS="-r none"
L=$WORK/winch.log
W=$(offset "$L" 0 "<winch>")
if [ "$W" -lt 0 ]; then
	note_fail "the program never got SIGWINCH, this run proves nothing"
	W=0
fi
check "$(count "$L" 0 "$W" "$ANS")" 0 "no stale answer before the redraw"
check "$(count "$L" "$W" end "$ANS")" 1 "the redraw's live query was answered"

##############################################################################
echo "== 5: multi-chunk replay, typing while it streams =="
new_session big
attach big "answer:1" "send:B" "answer:3" "detach" "read:0.5"
L=$WORK/big.log
MARK=$(filesize "$L")
attach big "stall:1.5" "send:typed" "answer:5" "send:Q" "answer:1.5" \
	"detach" "read:0.5"
A=$(sed -n 2p "$WORK/big.out.answers")
[ "${A:-0}" -ge 3 ] && note_pass "terminal answered the replayed queries (${A}x)" \
	|| note_fail "terminal answered ${A:-0} replayed queries, want >= 3"
[ "$(offset "$L" "$MARK" typed)" -ge 0 ] && note_pass "typing during the replay arrived" \
	|| note_fail "typing during the replay was dropped"
Q=$(offset "$L" "$MARK" Q)
[ "$Q" -lt 0 ] && { note_fail "keystroke Q never arrived"; Q=$(filesize "$L"); }
check "$(count "$L" "$MARK" "$Q" "$ANS")" 0 "no stale answer from a 200KB replay"
check "$(count "$L" "$Q" end "$ANS")" 1 "the live answer after it reached the program"

##############################################################################
echo "== 6: a terminal that never answers the fence =="
new_session nodsr query
attach nodsr "answer:1:0:0" "send:hi" "answer:5.5:0:0" "send:Q" \
	"answer:1.5:0:0" "detach" "read:0.5"
L=$WORK/nodsr.log
[ "$(offset "$L" 0 hi)" -ge 0 ] && note_pass "typing inside the fence window arrived" \
	|| note_fail "typing inside the fence window was dropped"
Q=$(offset "$L" 0 Q)
[ "$Q" -lt 0 ] && { note_fail "keystroke Q never arrived"; Q=$(filesize "$L"); }
check "$(count "$L" 0 "$Q" "$ANS")" 0 "no stale answer without a fence answer"
check "$(count "$L" "$Q" end "$ANS")" 1 "live answer after the bounded window reached the program"

##############################################################################
echo "== result =="
if [ "$rc" -eq 0 ]; then
	echo "PASS: the replay gate drops stale answers and nothing else"
else
	echo "FAIL: see the failures above"
	for f in "$WORK"/*.log; do
		echo "--- $(basename "$f") ---"
		LC_ALL=C tr -c '[:print:]' '.' < "$f" | cut -c1-600
		echo
	done
fi
exit $rc
