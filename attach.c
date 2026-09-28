/*
    dtach - A simple program that emulates the detach feature of screen.
    Copyright (C) 2004-2016 Ned T. Crigler

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "dtach.h"

#ifndef VDISABLE
#ifdef _POSIX_VDISABLE
#define VDISABLE _POSIX_VDISABLE
#else
#define VDISABLE 0377
#endif
#endif

/*
** The current terminal settings. After coming back from a suspend, we
** restore this.
*/
static struct termios cur_term;
/* 1 if the window size changed */
static int win_changed;

/*
** The markers the master brackets a scrollback replay with, and the one this
** client prints itself when the master cuts a replay short. A replay is the
** one thing the master can cut short without the session being over: it
** drops a client that stalls or trickles rather than letting it hold the
** session, and a dropped client just sees its socket close. EOF alone would
** be reported as "dtach terminating", telling the user the session is gone
** while the master is still running it, so the gate below tracks whether we
** are inside a replay and the EOF path says something true instead.
*/
static const char replay_start_marker[] = "\033]dtach-rev;replay-start\007";
static const char replay_end_marker[] = "\033]dtach-rev;replay-end\007";
static const char replay_abort_marker[] = "\033]dtach-rev;replay-abort\007";
/*
** DSR "report operating status". Every VT100-compatible terminal answers it
** with ESC [ 0 n (ESC [ 3 n for "malfunction"). It is the fence this client
** writes after a replay, and the replayed query it counts; see below.
*/
static const char dsr_query[] = "\033[5n";

#define MARKER_LEN(m) (sizeof(m) - 1)

/*
** Advance a naive matcher for `marker` by one byte, returning 1 if the marker
** completed on it. The match state lives in *pos across calls, so a marker
** split across two reads is still seen. Exact for every marker here, since
** none of them contains its own first byte (ESC) again.
*/
static int
marker_step(const char *marker, size_t *pos, unsigned char c)
{
	if (c == (unsigned char)marker[*pos])
	{
		if (marker[++(*pos)] == '\0')
		{
			*pos = 0;
			return 1;
		}
		return 0;
	}
	*pos = (c == (unsigned char)marker[0]) ? 1 : 0;
	return 0;
}

/*
** A19: stale answers to replayed terminal queries.
**
** The replayed scrollback can hold terminal queries (DA1 "ESC [ c", cursor
** position requests and the like) that a program asked long ago and that
** were answered, live, back then. A full terminal emulator attached through
** this client (Terminal.app, via TerminaLLM's "Open Tab on Mac") parses the
** replay as if it were new and answers those queries again, and the answers
** used to be forwarded to the session as keystrokes, corrupting the next
** command. The gate drops those answers and nothing else.
**
** 1. Only a replay this client asked for arms the gate. Writing MSG_ATTACH
**    sets `expect`. On the first attach the master sends this client nothing
**    before the replay, so the replay-start marker must be the very first
**    bytes received (GATE_EXPECT_FIRST): any other first byte (a master
**    without markers) cancels the expectation. After a suspend and resume
**    the master may still have live output queued ahead of the new replay,
**    so the next replay-start anywhere is honoured (GATE_EXPECT_ANY). One
**    expectation honours one replay-start. A replay-start in live output
**    with no expectation (a program printing one, a `cat` of a log, a
**    nested dtach client) is ignored and never touches input.
**
** 2. Inside an honoured replay, replay-start markers nest (the scrollback of
**    a session that once ran a dtach client of its own holds that client's
**    markers) and replay-end or replay-abort closes one level. When the
**    outermost level closes and the replay carried data, this client writes
**    the replay up to and including that marker, then the fence query
**    ESC [ 5 n, then anything that followed in the same read.
**
** 3. A terminal answers what it parses in order. Every answer to a replayed
**    query therefore reaches this client BEFORE the answer to the fence,
**    however long the path through sshd, the network and the terminal's own
**    renderer takes. The fence is met when every status answer owed has
**    arrived: one for the fence plus one for every ESC [ 5 n inside the
**    replay (those are answered too, with the same bytes, so they are
**    counted rather than mistaken for the fence).
**
** 4. From the honoured replay-start until the fence is met, input is
**    filtered, not blocked: only complete terminal-report sequences are
**    dropped (see classify_input for the exact shapes). Keystrokes,
**    including arrows, function keys, mouse and paste, pass through, and so
**    do the detach and suspend keys. After the fence, everything passes, so
**    a program that queries the terminal when a reattach redraws it (-r
**    winch) gets its live answer.
**
** 5. Every stage is bounded. A replay whose end never arrives (nested
**    markers that do not balance) releases GATE_REPLAY_BASE_MS plus the
**    replay's size at GATE_MIN_RATE after it began, the same rule the
**    master uses to drop a replay that takes too long. A fence a terminal
**    never answers releases GATE_FENCE_BASE_MS plus the replay's size at
**    GATE_MIN_RATE after it was written; after that one late status answer
**    per fence is still swallowed for GATE_LATE_PROBE_MS, so it never lands
**    in the program as typed text. An ESC-prefixed input fragment that could
**    be the start of a report is held at most GATE_HOLD_MS for the rest of
**    it, then forwarded unchanged.
**
** An empty replay (no scrollback, or nothing printed yet) writes no fence
** and leaves nothing armed. That includes the client that creates a session
** with -c or -A: the master does not read the pty until that client is
** attached, so its replay is always empty and any query the program asks at
** startup is answered and forwarded as before. The replayed bytes always
** render unchanged. Mirrors replay_gate.go in terminallm-daemon.
*/
#define GATE_EXPECT_NONE	0
#define GATE_EXPECT_FIRST	1
#define GATE_EXPECT_ANY		2

/* Mirrors CLIENT_REPLAY_TIMEOUT, CLIENT_REPLAY_MIN_RATE and the scaled-term
** cap in master.c. */
#define GATE_REPLAY_BASE_MS	30000LL
#define GATE_MIN_RATE		(64 * 1024)
#define GATE_MAX_SCALED_MS	256000LL
/* Round trip plus parse time allowed for a fence, on top of the replay's
** size at GATE_MIN_RATE. */
#define GATE_FENCE_BASE_MS	5000LL
#define GATE_LATE_PROBE_MS	30000LL
/* How long an incomplete ESC-prefixed input fragment is held. */
#define GATE_HOLD_MS		100LL
/* Longest report sequences recognised; longer ones are forwarded. */
#define GATE_CSI_MAX		128
#define GATE_STR_MAX		512
#define GATE_HOLD_MAX		GATE_STR_MAX
#define GATE_READ_MAX		512
#define GATE_OUT_MAX		(2 * GATE_HOLD_MAX + GATE_READ_MAX)

struct replay_gate
{
	int expect;		/* GATE_EXPECT_* */
	size_t start_pos, end_pos, abort_pos, dsr_pos;
	int depth;		/* >0 inside an honoured replay */
	size_t replay_bytes;	/* bytes after the honoured replay-start */
	long long replay_started;
	int probe_due;		/* write the fence before any further output */
	int fence_armed;	/* a fence is written and not yet met */
	long long fence_deadline;
	unsigned int status_owed; /* status answers the terminal still owes */
	long long owed_until;
	unsigned char held[GATE_HOLD_MAX];
	size_t held_len;
	long long held_since;
};

static struct replay_gate gate;

/* Milliseconds from a clock that never steps backwards when one exists. */
static long long
now_ms(void)
{
	struct timeval tv;
#ifdef CLOCK_MONOTONIC
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
	gettimeofday(&tv, NULL);
	return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* Time to move `bytes` at GATE_MIN_RATE, capped like the master's. */
static long long
gate_scaled_ms(size_t bytes)
{
	long long ms = (long long)(bytes / GATE_MIN_RATE) * 1000 +
		(long long)(bytes % GATE_MIN_RATE) * 1000 / GATE_MIN_RATE;

	return ms > GATE_MAX_SCALED_MS ? GATE_MAX_SCALED_MS : ms;
}

/* A MSG_ATTACH was written: expect the replay it starts. */
static void
gate_expect(struct replay_gate *g, int mode)
{
	g->expect = mode;
	if (mode == GATE_EXPECT_FIRST)
		g->start_pos = 0;
}

/* Expire whatever has outlived its bound. */
static void
gate_tick(struct replay_gate *g, long long now)
{
	if (g->depth > 0 && now >= g->replay_started + GATE_REPLAY_BASE_MS +
	    gate_scaled_ms(g->replay_bytes))
	{
		g->depth = 0;
		if (!g->fence_armed)
			g->status_owed = 0;
	}
	if (g->fence_armed &&
	    (g->status_owed == 0 || now >= g->fence_deadline))
		g->fence_armed = 0;
	if (!g->fence_armed && g->depth == 0 && g->status_owed != 0 &&
	    now >= g->owed_until)
		g->status_owed = 0;
}

/* 1 while terminal reports on the input are dropped. */
static int
gate_filtering(const struct replay_gate *g)
{
	return g->depth > 0 || g->fence_armed;
}

/* 1 if keyboard input must go through gate_input right now. */
static int
gate_input_needed(struct replay_gate *g, long long now)
{
	gate_tick(g, now);
	return g->held_len != 0 || gate_filtering(g) || g->status_owed != 0;
}

/*
** Note the replay markers in data received from the master. Returns how many
** bytes of `buf` the caller should write to the terminal now. When a replay
** that carried data has just ended, that count stops right after its closing
** marker and probe_due is set: the caller writes the fence, calls
** gate_fence_sent, and passes the rest of the buffer back in.
*/
static size_t
gate_output(struct replay_gate *g, const unsigned char *buf, size_t len,
	    long long now)
{
	size_t i;

	for (i = 0; i < len; ++i)
	{
		unsigned char c = buf[i];
		int started, ended, aborted;

		/* On the first attach the replay must lead the stream. */
		if (g->expect == GATE_EXPECT_FIRST && g->depth == 0 &&
		    c != (unsigned char)replay_start_marker[g->start_pos])
			g->expect = GATE_EXPECT_NONE;

		started = marker_step(replay_start_marker, &g->start_pos, c);
		ended = marker_step(replay_end_marker, &g->end_pos, c);
		aborted = marker_step(replay_abort_marker, &g->abort_pos, c);
		if (g->depth > 0)
		{
			++g->replay_bytes;
			if (marker_step(dsr_query, &g->dsr_pos, c))
				++g->status_owed;
		}

		if (started)
		{
			if (g->depth > 0)
				++g->depth;
			else if (g->expect != GATE_EXPECT_NONE)
			{
				g->expect = GATE_EXPECT_NONE;
				g->depth = 1;
				g->replay_bytes = 0;
				g->replay_started = now;
				g->dsr_pos = 0;
			}
		}
		else if ((ended || aborted) && g->depth > 0 && --g->depth == 0)
		{
			size_t mlen = ended ? MARKER_LEN(replay_end_marker) :
				MARKER_LEN(replay_abort_marker);

			if (g->replay_bytes > mlen)
			{
				g->probe_due = 1;
				return i + 1;
			}
		}
	}
	return len;
}

/* The fence query has been written to the terminal. */
static void
gate_fence_sent(struct replay_gate *g, long long now)
{
	long long deadline = now + GATE_FENCE_BASE_MS +
		gate_scaled_ms(g->replay_bytes);

	g->probe_due = 0;
	++g->status_owed;
	if (!g->fence_armed || deadline > g->fence_deadline)
		g->fence_deadline = deadline;
	g->fence_armed = 1;
	g->owed_until = g->fence_deadline + GATE_LATE_PROBE_MS;
}

/*
** Replay marker stripping (opt-in, DTACH_REV_STRIP_MARKERS=1).
**
** The replay markers are a protocol between the master and a client that
** parses them (TerminaLLM reads them from this client's output to find the
** replay), so by default they pass through untouched. A human terminal has
** no use for them, and one whose OSC parser gives up on a non-numeric
** selector (the Linux console's ESC ] handling, for one) consumes ESC ] d
** and prints the rest: "tach-rev;replay-end". With
** DTACH_REV_STRIP_MARKERS=1 in the environment, complete start, end and
** abort markers are removed from what this client writes to the terminal.
** gate_output still sees every raw byte, so the gate, its fence and input
** filtering are unchanged.
**
** A marker can be split across reads, so a trailing prefix of one is held
** until the next read decides it. The hold is at most one marker length
** minus one byte, and only while the held bytes are still a prefix of a
** marker; every marker starts with ESC and holds no other ESC, so held bytes
** are an unterminated ESC or OSC a terminal would not render before its
** next byte anyway. Held bytes that turn out not to be a marker are written
** unchanged and in order, and strip_flush writes whatever is still held when
** the stream ends.
*/
struct marker_strip
{
	int enabled;
	unsigned char held[MARKER_LEN(replay_abort_marker)];
	size_t held_len;
};

static struct marker_strip strip;

/* 1 if held[0..n) is a prefix of a marker; *whole set if it is one. */
static int
strip_match(const unsigned char *held, size_t n, int *whole)
{
	const char *markers[3];
	int k, prefix = 0;

	markers[0] = replay_start_marker;
	markers[1] = replay_end_marker;
	markers[2] = replay_abort_marker;
	*whole = 0;
	for (k = 0; k < 3; ++k)
	{
		size_t mlen = strlen(markers[k]);

		if (n <= mlen && memcmp(markers[k], held, n) == 0)
		{
			prefix = 1;
			if (n == mlen)
				*whole = 1;
		}
	}
	return prefix;
}

/*
** Copy buf to out with every complete marker removed, resolving any held
** prefix first. out must have room for len + sizeof(st->held) bytes.
** Returns the number of bytes written to out.
*/
static size_t
strip_output(struct marker_strip *st, const unsigned char *buf, size_t len,
	     unsigned char *out)
{
	size_t i, o = 0;

	for (i = 0; i < len; ++i)
	{
		unsigned char c = buf[i];
		int whole;

		if (st->held_len == 0)
		{
			if (c == 0x1b)
				st->held[st->held_len++] = c;
			else
				out[o++] = c;
			continue;
		}
		if (st->held_len < sizeof(st->held))
		{
			st->held[st->held_len] = c;
			if (strip_match(st->held, st->held_len + 1, &whole))
			{
				if (whole)
					st->held_len = 0;
				else
					++st->held_len;
				continue;
			}
		}
		/* Not a marker: release what was held. This byte may start one. */
		memcpy(out + o, st->held, st->held_len);
		o += st->held_len;
		st->held_len = 0;
		if (c == 0x1b)
			st->held[st->held_len++] = c;
		else
			out[o++] = c;
	}
	return o;
}

/* The stream is ending: whatever is held was data, write it. */
static void
strip_flush(struct marker_strip *st)
{
	if (st->held_len != 0)
		write_buf_or_fail(1, st->held, st->held_len);
	st->held_len = 0;
}

/*
** Send data from the master to the terminal, with the A19 fence right after
** a replay that carried data, and with the replay markers removed when
** stripping is on.
*/
static void
emit_output(struct replay_gate *g, struct marker_strip *st,
	    const unsigned char *buf, size_t len, long long now)
{
	unsigned char sbuf[BUFSIZE + sizeof(st->held)];
	size_t off;

	for (off = 0; off < len; )
	{
		size_t cut = gate_output(g, buf + off, len - off, now);

		if (!st->enabled)
			write_buf_or_fail(1, buf + off, cut);
		else
		{
			/* Split so sbuf always has room for cut + held. */
			size_t done = 0;

			while (done < cut)
			{
				size_t n = cut - done;

				if (n > BUFSIZE)
					n = BUFSIZE;
				write_buf_or_fail(1, sbuf, strip_output(st,
					buf + off + done, n, sbuf));
				done += n;
			}
		}
		off += cut;
		if (g->probe_due)
		{
			write_buf_or_fail(1, dsr_query, MARKER_LEN(dsr_query));
			gate_fence_sent(g, now);
		}
	}
}

#define SEQ_TEXT	0	/* forward the first byte as is */
#define SEQ_KEY		1	/* a complete sequence that is not a report */
#define SEQ_ANSWER	2	/* a complete terminal report */
#define SEQ_STATUS	3	/* a complete DSR status report, ESC [ Pn n */
#define SEQ_PARTIAL	4	/* could still become a report */

/*
** Classify the input at p[0..n), where p[0] is ESC, setting *seqlen to the
** bytes it covers. Terminal reports, the only things ever dropped:
**
**   ESC [ ... c          DA1, DA2 and DA3-style device attributes
**   ESC [ ... n          DSR answers (SEQ_STATUS when it has no private
**                        marker or intermediate, like ESC [ 0 n)
**   ESC [ ... R          cursor position reports (xterm also sends ESC [ 1 ;
**                        <mod> R for a modified F3, which is dropped too)
**   ESC [ ... t          window reports
**   ESC [ ... x          DECREPTPARM
**   ESC [ ... $ y        DECRPM mode reports
**   ESC [ ? ... u        kitty keyboard flag reports
**   ESC P ... ST/BEL     DCS reports (DECRQSS, XTGETTCAP, XTVERSION, DA3)
**   ESC ] ... ST/BEL     OSC reports (colour queries and the like)
**   ESC _ ... ST/BEL     APC reports
**
** Everything else (arrows, function keys, mouse and focus events, bracketed
** paste, Alt-modified keys, text) is not a report. Only 7-bit ESC counts:
** 0x9b is a UTF-8 continuation byte far more often than an 8-bit CSI.
*/
static int
classify_input(const unsigned char *p, size_t n, size_t *seqlen)
{
	size_t j;

	*seqlen = 1;
	if (n < 2)
		return SEQ_PARTIAL;
	if (p[1] == '[')
	{
		unsigned char priv = 0;
		int dollar = 0, inter = 0;

		j = 2;
		if (j < n && p[j] >= 0x3c && p[j] <= 0x3f)
			priv = p[j];
		while (j < n && j < GATE_CSI_MAX && p[j] >= 0x30 && p[j] <= 0x3f)
			++j;
		while (j < n && j < GATE_CSI_MAX && p[j] >= 0x20 && p[j] <= 0x2f)
		{
			if (p[j] == '$')
				dollar = 1;
			inter = 1;
			++j;
		}
		if (j >= GATE_CSI_MAX)
			return SEQ_TEXT;
		if (j >= n)
			return SEQ_PARTIAL;
		if (p[j] < 0x40 || p[j] > 0x7e)
			return SEQ_TEXT;
		*seqlen = j + 1;
		switch (p[j])
		{
		case 'n':
			return (priv == 0 && !inter && j > 2) ? SEQ_STATUS :
				SEQ_ANSWER;
		case 'c': case 'R': case 't': case 'x':
			return SEQ_ANSWER;
		case 'y':
			return dollar ? SEQ_ANSWER : SEQ_KEY;
		case 'u':
			return priv == '?' ? SEQ_ANSWER : SEQ_KEY;
		default:
			return SEQ_KEY;
		}
	}
	if (p[1] == 'P' || p[1] == ']' || p[1] == '_')
	{
		for (j = 2; j < n && j < GATE_STR_MAX; ++j)
		{
			if (p[j] == 0x07)
			{
				*seqlen = j + 1;
				return SEQ_ANSWER;
			}
			if (p[j] == 0x1b)
			{
				if (j + 1 >= n)
					return SEQ_PARTIAL;
				if (p[j + 1] != '\\')
					return SEQ_TEXT;
				*seqlen = j + 2;
				return SEQ_ANSWER;
			}
		}
		return j >= GATE_STR_MAX ? SEQ_TEXT : SEQ_PARTIAL;
	}
	return SEQ_TEXT;
}

/*
** Filter keyboard input through the gate. Writes what should reach the
** session to `out` (at least GATE_OUT_MAX bytes) and returns its length. An
** incomplete sequence at the end of the input is held for the next call.
*/
static size_t
gate_input(struct replay_gate *g, const unsigned char *in, size_t len,
	   unsigned char *out, long long now)
{
	unsigned char work[GATE_HOLD_MAX + GATE_READ_MAX];
	long long since = g->held_since;
	size_t n = 0, i = 0, o = 0;
	int was_held;

	if (len > GATE_READ_MAX)
		len = GATE_READ_MAX;
	/* A hold that outlived its window was keys, not a report. */
	if (g->held_len != 0 && now - g->held_since >= GATE_HOLD_MS)
	{
		memcpy(out, g->held, g->held_len);
		o = g->held_len;
		g->held_len = 0;
	}
	was_held = g->held_len != 0;
	memcpy(work, g->held, g->held_len);
	n = g->held_len;
	g->held_len = 0;
	memcpy(work + n, in, len);
	n += len;

	while (i < n)
	{
		size_t sl;
		int kind;

		if (work[i] != 0x1b)
		{
			out[o++] = work[i++];
			continue;
		}
		kind = classify_input(work + i, n - i, &sl);
		if (kind == SEQ_PARTIAL)
		{
			memcpy(g->held, work + i, n - i);
			g->held_len = n - i;
			g->held_since = (was_held && i == 0) ? since : now;
			return o;
		}
		gate_tick(g, now);
		if (kind == SEQ_STATUS && g->status_owed != 0)
		{
			/* An answer to a replayed ESC [ 5 n or to a fence. */
			--g->status_owed;
			gate_tick(g, now);
		}
		else if (kind < SEQ_ANSWER || !gate_filtering(g))
		{
			memcpy(out + o, work + i, sl);
			o += sl;
		}
		i += sl;
	}
	return o;
}

/* Release a held fragment whose window ran out. Returns the bytes written to
** `out`. */
static size_t
gate_flush(struct replay_gate *g, unsigned char *out, long long now)
{
	size_t n = g->held_len;

	if (n == 0 || now - g->held_since < GATE_HOLD_MS)
		return 0;
	memcpy(out, g->held, n);
	g->held_len = 0;
	return n;
}

/* Restores the original terminal settings. */
static void
restore_term(void)
{
	tcsetattr(0, TCSADRAIN, &orig_term);

	/* Make cursor visible. Assumes VT100. */
	printf("\033[?25h");
	fflush(stdout);
}

/* Connects to a unix domain socket */
static int
connect_socket(char *name)
{
	int s;
	struct sockaddr_un sockun;

	if (strlen(name) > sizeof(sockun.sun_path) - 1)
	{
		errno = ENAMETOOLONG;
		return -1;
	}

	s = socket(PF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	sockun.sun_family = AF_UNIX;
	strcpy(sockun.sun_path, name);
	if (connect(s, (struct sockaddr *)&sockun, sizeof(sockun)) < 0)
	{
		close(s);

		/* ECONNREFUSED is also returned for regular files, so make
		** sure we are trying to connect to a socket. */
		if (errno == ECONNREFUSED)
		{
			struct stat st;

			if (stat(name, &st) < 0)
				return -1;
			else if (!S_ISSOCK(st.st_mode) || S_ISREG(st.st_mode))
				errno = ENOTSOCK;
		}
		return -1;
	}
	return s;
}

/* Signal */
static RETSIGTYPE
die(int sig)
{
	/* Print a nice pretty message for some things. */
	if (sig == SIGHUP || sig == SIGINT)
		printf(EOS "\r\n[detached]\r\n");
	else
		printf(EOS "\r\n[got signal %d - dying]\r\n", sig);
	exit(1);
}

/* Window size change. */
static RETSIGTYPE
win_change(ATTRIBUTE_UNUSED int sig)
{
	signal(SIGWINCH, win_change);
	win_changed = 1;
}

/* Handles input from the keyboard. */
static void
process_kbd(int s, struct packet *pkt)
{
	/* Suspend? */
	if (!no_suspend && (pkt->u.buf[0] == cur_term.c_cc[VSUSP]))
	{
		/* Tell the master that we are suspending. */
		pkt->type = MSG_DETACH;
		write_packet_or_fail(s, pkt);

		/* And suspend... */
		tcsetattr(0, TCSADRAIN, &orig_term);
		printf(EOS "\r\n");
		kill(getpid(), SIGTSTP);
		tcsetattr(0, TCSADRAIN, &cur_term);

		/* Tell the master that we are returning. Live output may
		** still be queued ahead of the replay this asks for. */
		pkt->type = MSG_ATTACH;
		write_packet_or_fail(s, pkt);
		gate_expect(&gate, GATE_EXPECT_ANY);

		/* We would like a redraw, too. */
		pkt->type = MSG_REDRAW;
		pkt->len = redraw_method;
		ioctl(0, TIOCGWINSZ, &pkt->u.ws);
		write_packet_or_fail(s, pkt);
		return;
	}
	/* Detach char? */
	else if (pkt->u.buf[0] == detach_char)
	{
		printf(EOS "\r\n[detached]\r\n");
		exit(0);
	}
	/* Just in case something pukes out. */
	else if (pkt->u.buf[0] == '\f')
		win_changed = 1;

	/* Push it out */
	write_packet_or_fail(s, pkt);
}

/* 1 for a byte process_kbd acts on locally when it leads a packet. */
static int
is_local_key(unsigned char c)
{
	return (int)c == detach_char ||
		(!no_suspend && c == cur_term.c_cc[VSUSP]);
}

/*
** Send gate-filtered keyboard input to the master, in packets as small as
** the protocol's. A detach or suspend key always travels alone in its own
** packet, since process_kbd only looks at the first byte of each and acts
** on the whole packet.
*/
static void
push_filtered(int s, const unsigned char *p, size_t n)
{
	struct packet pkt;
	size_t i = 0;

	while (i < n)
	{
		size_t k = 0;

		memset(&pkt, 0, sizeof(struct packet));
		pkt.type = MSG_PUSH;
		do
			pkt.u.buf[k++] = p[i++];
		while (i < n && k < sizeof(pkt.u.buf) &&
		       !is_local_key(pkt.u.buf[0]) && !is_local_key(p[i]));
		pkt.len = k;
		process_kbd(s, &pkt);
	}
}

int
attach_main(int noerror)
{
	struct packet pkt;
	unsigned char buf[BUFSIZE];
	fd_set readfds;
	int s;

	/* Attempt to open the socket. Don't display an error if noerror is
	** set. */
	s = connect_socket(sockname);
	if (s < 0 && errno == ENAMETOOLONG)
	{
		char *slash = strrchr(sockname, '/');

		/* Try to shorten the socket's path name by using chdir. */
		if (slash)
		{
			int dirfd = open(".", O_RDONLY);

			if (dirfd >= 0)
			{
				*slash = '\0';
				if (chdir(sockname) >= 0)
				{
					s = connect_socket(slash + 1);
					if (s >= 0 && fchdir(dirfd) < 0)
					{
						close(s);
						s = -1;
					}
				}
				*slash = '/';
				close(dirfd);
			}
		}
	}
	if (s < 0)
	{
		if (!noerror)
			printf("%s: %s: %s\n", progname, sockname,
			       strerror(errno));
		return 1;
	}

	/* The current terminal settings are equal to the original terminal
	** settings at this point. */
	cur_term = orig_term;

	/* Set a trap to restore the terminal when we die. */
	atexit(restore_term);

	/* Set some signals. */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGXFSZ, SIG_IGN);
	signal(SIGHUP, die);
	signal(SIGTERM, die);
	signal(SIGINT, die);
	signal(SIGQUIT, die);
	signal(SIGWINCH, win_change);

	/* Keep the replay markers off a human terminal (see strip_output). */
	{
		const char *e = getenv("DTACH_REV_STRIP_MARKERS");

		strip.enabled = e && strcmp(e, "1") == 0;
	}

	/* Set raw mode. */
	cur_term.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL);
	cur_term.c_iflag &= ~(IXON|IXOFF);
	cur_term.c_oflag &= ~(OPOST);
	cur_term.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	cur_term.c_cflag &= ~(CSIZE|PARENB);
	cur_term.c_cflag |= CS8;
	cur_term.c_cc[VLNEXT] = VDISABLE;
	cur_term.c_cc[VMIN] = 1;
	cur_term.c_cc[VTIME] = 0;
	tcsetattr(0, TCSADRAIN, &cur_term);

	/* Tell the master that we want to attach. The master will replay
	** the scrollback buffer before marking us as attached, so we skip
	** the old screen clear to preserve the replayed content. */
	memset(&pkt, 0, sizeof(struct packet));
	pkt.type = MSG_ATTACH;
	write_packet_or_fail(s, &pkt);
	gate_expect(&gate, GATE_EXPECT_FIRST);

	/* We would like a redraw, too. */
	pkt.type = MSG_REDRAW;
	pkt.len = redraw_method;
	ioctl(0, TIOCGWINSZ, &pkt.u.ws);
	write_packet_or_fail(s, &pkt);

	/* Wait for things to happen */
	while (1)
	{
		int n;
		struct timeval tv, *tvp = NULL;

		/* Wake up to release a held input fragment on time. */
		if (gate.held_len != 0)
		{
			long long wait = gate.held_since + GATE_HOLD_MS -
				now_ms();

			if (wait < 0)
				wait = 0;
			tv.tv_sec = (time_t)(wait / 1000);
			tv.tv_usec = (suseconds_t)((wait % 1000) * 1000);
			tvp = &tv;
		}

		FD_ZERO(&readfds);
		FD_SET(0, &readfds);
		FD_SET(s, &readfds);
		n = select(s + 1, &readfds, NULL, NULL, tvp);
		if (n < 0 && errno != EINTR && errno != EAGAIN)
		{
			printf(EOS "\r\n[select failed]\r\n");
			exit(1);
		}

		/* Pty activity */
		if (n > 0 && FD_ISSET(s, &readfds))
		{
			ssize_t len = read(s, buf, sizeof(buf));

			if (len == 0)
			{
				strip_flush(&strip);
				/* Cut off mid-replay: the master dropped
				** us, but the session is still running. */
				if (gate.depth > 0)
				{
					printf(EOS "\r\n"
					       "\033]dtach-rev;replay-abort\007"
					       "[reattach interrupted - the "
					       "session is still running, "
					       "attach again]\r\n");
					exit(1);
				}
				printf(EOS "\r\n[EOF - dtach terminating]"
				       "\r\n");
				exit(0);
			}
			else if (len < 0)
			{
				strip_flush(&strip);
				printf(EOS "\r\n[read returned an error]\r\n");
				exit(1);
			}
			/* Send the data to the terminal, with the A19 fence
			** right after a replay that carried data. */
			emit_output(&gate, &strip, buf, (size_t)len, now_ms());
			n--;
		}
		/* stdin activity */
		if (n > 0 && FD_ISSET(0, &readfds) &&
		    gate_input_needed(&gate, now_ms()))
		{
			/* A19: drop terminal reports to replayed queries. */
			unsigned char in[GATE_READ_MAX], out[GATE_OUT_MAX];
			ssize_t len = read(0, in, sizeof(in));

			if (len <= 0)
				exit(1);
			push_filtered(s, out, gate_input(&gate, in,
				(size_t)len, out, now_ms()));
			n--;
		}
		else if (n > 0 && FD_ISSET(0, &readfds))
		{
			ssize_t len;

			pkt.type = MSG_PUSH;
			memset(pkt.u.buf, 0, sizeof(pkt.u.buf));
			len = read(0, pkt.u.buf, sizeof(pkt.u.buf));

			if (len <= 0)
				exit(1);

			pkt.len = len;
			process_kbd(s, &pkt);
			n--;
		}
		/* A held input fragment whose window ran out was keys. */
		if (gate.held_len != 0)
		{
			unsigned char out[GATE_HOLD_MAX];
			size_t k = gate_flush(&gate, out, now_ms());

			if (k != 0)
				push_filtered(s, out, k);
		}

		/* Window size changed? */
		if (win_changed)
		{
			win_changed = 0;

			pkt.type = MSG_WINCH;
			ioctl(0, TIOCGWINSZ, &pkt.u.ws);
			write_packet_or_fail(s, &pkt);
		}
	}
	return 0;
}

int
push_main()
{
	struct packet pkt;
	int s;

	/* Attempt to open the socket. */
	s = connect_socket(sockname);
	if (s < 0 && errno == ENAMETOOLONG)
	{
		char *slash = strrchr(sockname, '/');

		/* Try to shorten the socket's path name by using chdir. */
		if (slash)
		{
			int dirfd = open(".", O_RDONLY);

			if (dirfd >= 0)
			{
				*slash = '\0';
				if (chdir(sockname) >= 0)
				{
					s = connect_socket(slash + 1);
					if (s >= 0 && fchdir(dirfd) < 0)
					{
						close(s);
						s = -1;
					}
				}
				*slash = '/';
				close(dirfd);
			}
		}
	}
	if (s < 0)
	{
		printf("%s: %s: %s\n", progname, sockname, strerror(errno));
		return 1;
	}

	/* Set some signals. */
	signal(SIGPIPE, SIG_IGN);

	/* Push the contents of standard input to the socket. */
	pkt.type = MSG_PUSH;
	for (;;)
	{
		ssize_t len;

		memset(pkt.u.buf, 0, sizeof(pkt.u.buf));
		len = read(0, pkt.u.buf, sizeof(pkt.u.buf));

		if (len == 0)
			return 0;
		else if (len < 0)
		{
			printf("%s: %s: %s\n", progname, sockname,
			       strerror(errno));
			return 1;
		}

		pkt.len = len;
		len = write(s, &pkt, sizeof(struct packet));
		if (len != sizeof(struct packet))
		{
			if (len >= 0)
				errno = EPIPE;

			printf("%s: %s: %s\n", progname, sockname,
			       strerror(errno));
			return 1;
		}
	}
}
