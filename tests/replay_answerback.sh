#!/bin/sh
#
# Regression test (A19): a reattaching terminal's answers to device-attribute
# queries buried in the scrollback replay must never reach the session.
#
# A program running under dtach-rev (an agent CLI, say) asks the terminal a
# question such as DA1 (ESC [ c) and gets its answer, live, once. The query
# stays in the scrollback. When a real terminal emulator (Terminal.app, via
# TerminaLLM's "Open Tab on Mac") later reattaches with -a, the master replays
# that scrollback, the terminal parses the replayed query as if it were new
# and answers it again, and the attach client used to forward that stale
# answer ("ESC [ ? 1 ; 2 c") into the session as keystrokes, corrupting the
# next command.
#
# The fix: the attach client drops keyboard input while a non-empty replay is
# streaming and for a short grace window after its replay-end marker. The
# replayed bytes still render, and a live query issued after the window is
# answered and forwarded as before.
#
# Phases:
#   1. create a session with -A under a terminal that answers DA1. The program
#      queries at startup and the creator's answer must reach it: the master
#      does not read the pty until the creating client is attached, so that
#      client's replay is empty and it is never gated
#   2. detach, then reattach with -a under a terminal that answers every DA1
#      it sees, replayed or not. The replayed query is answered, and that
#      answer must NOT reach the session. Past the grace window, ask the
#      program to query again: that live answer must reach it
#   3. a session with no scrollback (-b 0) has an empty replay: input sent
#      right after attach, including a live DA1 answer, reaches it unchanged
#
# Usage: tests/replay_answerback.sh [path-to-dtach]
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

TAG=dtachtest-answerback-$$
SOCK=/tmp/$TAG.sock
SOCK0=/tmp/$TAG-empty.sock
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
	ps_pid_args | awk -v b="$DTACH" -v t="$TAG" \
		'$2 == b && index($0, t) > 0 { print $1 }'
}

cleanup() {
	for pid in $(our_dtach_pids); do
		kill "$pid" 2>/dev/null
	done
	rm -f "$SOCK" "$SOCK0"
	rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# The program under dtach: raw mode, logs every input byte, prints a DA1
# query at startup (argv[2] == "query") and again whenever it reads a "Q".
cat > "$WORK/session.py" <<'PYEOF'
import os
import sys
import tty

tty.setraw(0)
log = open(sys.argv[1], "ab", 0)
if sys.argv[2] == "query":
    os.write(1, b"agent starting\r\n\x1b[c")
os.write(1, b"ready\r\n")
while True:
    data = os.read(0, 1024)
    if not data:
        break
    log.write(data)
    if b"Q" in data:
        os.write(1, b"live query\r\n\x1b[c")
PYEOF

# Print the number of stale DA1 answers ("1;2c") in a byte range of a file.
count_answers() {
	python3 - "$1" "$2" "$3" <<'PYEOF'
import sys
data = open(sys.argv[1], "rb").read()
lo, hi = int(sys.argv[2]), sys.argv[3]
seg = data[lo:] if hi == "end" else data[lo:int(hi)]
print(seg.count(b"\x1b[?1;2c"))
PYEOF
}
offset_of_q() {
	python3 - "$1" "$2" <<'PYEOF'
import sys
data = open(sys.argv[1], "rb").read()
print(data.find(b"Q", int(sys.argv[2])))
PYEOF
}
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
nth_answers() {
	sed -n "${2}p" "$1.answers" 2>/dev/null
}

LOG=$WORK/input.log
: > "$LOG"

##############################################################################
echo "== phase 1: create with -A; the creator's live answer reaches the program =="
python3 "$DRIVER" "$WORK/create.out" \
	"$DTACH -A $SOCK -r none -b 256k python3 $WORK/session.py $LOG query" \
	"answer:2" \
	"detach" \
	"read:1"

if [ ! -S "$SOCK" ]; then
	echo "FAIL: no live session after phase 1"
	exit 1
fi
A1=$(nth_answers "$WORK/create.out" 1)
if [ "${A1:-0}" -ge 1 ]; then
	note_pass "creator terminal answered the startup DA1 query"
else
	note_fail "creator terminal never saw the startup DA1 query"
fi
N1=$(count_answers "$LOG" 0 end)
if [ "$N1" -eq 1 ]; then
	note_pass "the program received the creator's live answer"
else
	note_fail "the program received $N1 answers from the creator, want 1"
fi
PHASE1_END=$(filesize "$LOG")

##############################################################################
echo "== phase 2: reattach with -a; the replayed query's answer is dropped =="
python3 "$DRIVER" "$WORK/reattach.out" \
	"$DTACH -a $SOCK -r none" \
	"answer:2" \
	"send:Q" \
	"answer:2" \
	"detach" \
	"read:1"

if saw 'dtach-rev;replay-end' "$WORK/reattach.out"; then
	note_pass "reattach got a complete replay"
else
	note_fail "reattach never got replay-end"
fi
if saw 'agent starting' "$WORK/reattach.out"; then
	note_pass "replayed scrollback still rendered"
else
	note_fail "replayed scrollback did not reach the terminal"
fi
A2=$(nth_answers "$WORK/reattach.out" 1)
if [ "${A2:-0}" -ge 1 ]; then
	note_pass "reattaching terminal answered the replayed DA1 query (${A2}x)"
else
	note_fail "reattaching terminal never answered the replayed query, so"
	note_fail "this run proves nothing"
fi
QPOS=$(offset_of_q "$LOG" "$PHASE1_END")
if [ "$QPOS" -lt 0 ]; then
	note_fail "the program never received the live keystroke Q"
	QPOS=$(filesize "$LOG")
else
	note_pass "live keystroke after the grace window reached the program"
fi
STALE=$(count_answers "$LOG" "$PHASE1_END" "$QPOS")
if [ "$STALE" -eq 0 ]; then
	note_pass "no stale answer reached the program"
else
	note_fail "$STALE stale DA1 answer(s) reached the program (A19)"
fi
LIVE=$(count_answers "$LOG" "$QPOS" end)
if [ "$LIVE" -eq 1 ]; then
	note_pass "the live DA1 answer after the window reached the program"
else
	note_fail "the program got $LIVE answers to its live query, want 1"
fi

##############################################################################
echo "== phase 3: an empty replay (-b 0) gates nothing =="
LOG0=$WORK/input-empty.log
: > "$LOG0"
"$DTACH" -n "$SOCK0" -r none -b 0 python3 "$WORK/session.py" "$LOG0" noquery
python3 "$DRIVER" "$WORK/empty.out" \
	"$DTACH -a $SOCK0 -r none" \
	"answer:0.1" \
	"send:Q" \
	"answer:2" \
	"detach" \
	"read:1"
if saw 'dtach-rev;replay-end' "$WORK/empty.out"; then
	note_pass "empty-scrollback attach still got the replay markers"
else
	note_fail "empty-scrollback attach never got replay-end"
fi
if [ "$(offset_of_q "$LOG0" 0)" -ge 0 ]; then
	note_pass "keystroke sent right after attach reached the program"
else
	note_fail "keystroke sent right after attach was dropped"
fi
N0=$(count_answers "$LOG0" 0 end)
if [ "$N0" -eq 1 ]; then
	note_pass "live DA1 answer right after attach reached the program"
else
	note_fail "the program got $N0 answers right after attach, want 1"
fi

##############################################################################
echo "== result =="
if [ "$rc" -eq 0 ]; then
	echo "PASS: stale answers to replayed terminal queries never reach the"
	echo "      session; live input and live answers still do"
else
	echo "FAIL: see the failures above"
	for f in "$LOG" "$LOG0"; do
		echo "--- $(basename "$f") ---"
		LC_ALL=C tr -c '[:print:]' '.' < "$f"
		echo
	done
fi

exit $rc
