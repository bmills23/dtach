/*
** Unit test for the A19 replay gate in attach.c: the replay markers split
** across reads at every byte, multi-chunk replays, nesting and replay-abort,
** forged markers in live output, the DSR fence and its counting, the input
** report filter (split at every byte), holds and every time bound, and the
** opt-in replay marker stripping on the terminal side (split at every byte,
** with no byte held past a diverging one), and the client's abort notice.
**
** Build and run from the source directory, after ./configure && make:
**   cc -I. -o tests/gate_unit tests/gate_unit.c && tests/gate_unit
** It includes attach.c itself, so the static gate functions are the real
** ones, not copies. Exit status: 0 = PASS, 1 = FAIL.
*/
#include "../attach.c"

char *progname = "gate_unit", *sockname = "";
int detach_char = '\\' - 64, no_suspend, redraw_method;
struct termios orig_term;
int dont_have_tty;
size_t scrollback_size;
int idle_timeout;
char *idle_callback;

/* What emit_output wrote to the terminal (fd 1). */
static unsigned char term[400000];
static size_t term_len;

void
write_buf_or_fail(int fd, const void *buf, size_t count)
{
	if (fd == 1 && term_len + count <= sizeof(term))
	{
		memcpy(term + term_len, buf, count);
		term_len += count;
	}
}

void
write_packet_or_fail(int fd, const struct packet *pkt)
{
	(void)fd; (void)pkt;
}

static int failures;

#define CHECK(cond, what) do { \
	if (!(cond)) { \
		printf("   FAIL: %s (line %d)\n", what, __LINE__); \
		++failures; \
	} } while (0)

#define START replay_start_marker
#define END replay_end_marker
#define ABORT replay_abort_marker

static unsigned char stream[300000];
static size_t stream_len;

static void
s_reset(void)
{
	stream_len = 0;
}

static void
s_add(const char *p, size_t n)
{
	memcpy(stream + stream_len, p, n);
	stream_len += n;
}

#define S_ADD(str) s_add(str, strlen(str))

/*
** Feed stream[] through gate_output the way attach_main does, cut into
** reads at the given boundaries (ascending, terminated by 0), and record
** the stream offsets after which the fence was written.
*/
static size_t fences[8];
static int nfences;

static void
feed(struct replay_gate *g, const size_t *cuts, long long now)
{
	size_t from = 0, k = 0;

	nfences = 0;
	for (;;)
	{
		size_t to = cuts[k] ? cuts[k] : stream_len;
		size_t off = from;

		if (to > stream_len)
			to = stream_len;
		while (off < to)
		{
			off += gate_output(g, stream + off, to - off, now);
			if (g->probe_due)
			{
				if (nfences < 8)
					fences[nfences++] = off;
				gate_fence_sent(g, now);
			}
		}
		from = to;
		if (!cuts[k] || from >= stream_len)
			break;
		++k;
	}
}

static void
fresh(struct replay_gate *g, int expect)
{
	memset(g, 0, sizeof(*g));
	if (expect)
		gate_expect(g, expect);
}

/* Run the stream split once at every position, then byte by byte, and check
** that the fence lands at `want` (0: no fence) every time. */
static void
every_split(const char *what, int expect, size_t want, int want_owed)
{
	struct replay_gate g;
	size_t cut, cuts[2];
	int bad = 0;

	for (cut = 0; cut <= stream_len; ++cut)
	{
		cuts[0] = cut;
		cuts[1] = 0;
		fresh(&g, expect);
		feed(&g, cut ? cuts : (size_t[]){ 0 }, 1000);
		if ((want ? (nfences != 1 || fences[0] != want) :
		     nfences != 0) || (int)g.status_owed != want_owed ||
		    g.depth != 0)
			bad = 1;
	}
	{
		/* Byte by byte. */
		static size_t bytes[300001];
		size_t i;

		for (i = 0; i < stream_len; ++i)
			bytes[i] = i + 1;
		bytes[stream_len] = 0;
		fresh(&g, expect);
		feed(&g, bytes, 1000);
		if ((want ? (nfences != 1 || fences[0] != want) :
		     nfences != 0) || (int)g.status_owed != want_owed)
			bad = 1;
	}
	CHECK(!bad, what);
	if (!bad)
		printf("   PASS: %s\n", what);
}

/* Run input through gate_input in two reads split at `cut`. */
static size_t
filter2(struct replay_gate *g, const char *in, size_t cut, unsigned char *out,
	long long now)
{
	size_t n = strlen(in), o;

	if (cut > n)
		cut = n;
	o = gate_input(g, (const unsigned char *)in, cut, out, now);
	o += gate_input(g, (const unsigned char *)in + cut, n - cut, out + o,
			now);
	return o;
}

static void
arm_fence(struct replay_gate *g, long long now)
{
	fresh(g, GATE_EXPECT_FIRST);
	s_reset();
	S_ADD(START);
	S_ADD("old output \033[c");
	S_ADD(END);
	feed(g, (size_t[]){ 0 }, now);
}

static void
expect_out(const char *what, const unsigned char *out, size_t n,
	   const char *want)
{
	int ok = n == strlen(want) && memcmp(out, want, n) == 0;

	CHECK(ok, what);
	if (ok)
		printf("   PASS: %s\n", what);
}

/*
** How many trailing bytes of p[0..n) a correct stripper still holds: the
** longest suffix that starts with ESC and is a proper prefix of a marker.
** Every marker starts with ESC and holds no other ESC, so that suffix is
** exactly the pending hold.
*/
static size_t
pending_len(const unsigned char *p, size_t n)
{
	size_t j;
	int whole;

	for (j = n; j-- > 0; )
		if (p[j] == 0x1b && strip_match(p + j, n - j, &whole) && !whole)
			return n - j;
	return 0;
}

/*
** What the terminal should have seen for input p[0..n) with no pending
** tail: the input with every complete marker removed when stripping is on,
** and the fence after an end marker when a replay was expected. Independent
** of strip_output, so a stripper that sits on released bytes cannot pass.
*/
static size_t
oracle(const unsigned char *p, size_t n, int expect, int enabled,
       unsigned char *out)
{
	const char *m[3] = { START, END, ABORT };
	size_t i = 0, o = 0;
	int k;

	while (i < n)
	{
		for (k = 0; k < 3; ++k)
		{
			size_t ml = strlen(m[k]);

			if (i + ml <= n && memcmp(p + i, m[k], ml) == 0)
				break;
		}
		if (k == 3)
		{
			out[o++] = p[i++];
			continue;
		}
		if (!enabled)
		{
			memcpy(out + o, m[k], strlen(m[k]));
			o += strlen(m[k]);
		}
		i += strlen(m[k]);
		if (k == 1 && expect)
		{
			memcpy(out + o, "\033[5n", 4);
			o += 4;
		}
	}
	return o;
}

/*
** Feed stream[] through emit_output (the terminal side of attach_main) in
** two reads split at `cut`, then flush as the EOF path does. Returns 1 if
** the terminal received exactly `want` (want_len bytes), and if what
** reached it after the first read was exactly the oracle's output for that
** read up to its pending marker prefix: no marker fragment written early,
** and nothing else held back for a later ESC or the flush.
*/
static int
emit2(int expect, int enabled, size_t cut, const char *want, size_t want_len)
{
	static unsigned char first_want[sizeof(stream) + 64];
	struct replay_gate g;
	size_t first, fw;

	fresh(&g, expect);
	memset(&strip, 0, sizeof(strip));
	strip.enabled = enabled;
	term_len = 0;
	if (cut > stream_len)
		cut = stream_len;
	emit_output(&g, &strip, stream, cut, 1000);
	first = term_len;
	fw = oracle(stream, cut - (enabled ? pending_len(stream, cut) : 0),
		    expect, enabled, first_want);
	if (first != fw || memcmp(term, first_want, fw) != 0)
		return 0;
	emit_output(&g, &strip, stream + cut, stream_len - cut, 1000);
	strip_flush(&strip);
	return term_len == want_len && memcmp(term, want, want_len) == 0;
}

/* Run emit2 at every split point and check the result. */
static void
emit_every_split(const char *what, int expect, int enabled, const char *want,
		 size_t want_len)
{
	size_t cut;
	int bad = 0;

	for (cut = 0; cut <= stream_len; ++cut)
		if (!emit2(expect, enabled, cut, want, want_len))
			bad = 1;
	CHECK(!bad, what);
	if (!bad)
		printf("   PASS: %s\n", what);
}

/* 1 if the terminal output holds any part of a marker's name. */
static int
term_has_marker_bytes(void)
{
	static const char *bad[] = { "dtach-rev", "tach-rev;replay",
		"replay-start", "replay-end", "replay-abort", "\033]d" };
	size_t k, i;

	for (k = 0; k < sizeof(bad) / sizeof(bad[0]); ++k)
	{
		size_t n = strlen(bad[k]);

		for (i = 0; i + n <= term_len; ++i)
			if (memcmp(term + i, bad[k], n) == 0)
				return 1;
	}
	return 0;
}

int
main(void)
{
	struct replay_gate g;
	unsigned char out[GATE_OUT_MAX * 4];
	size_t n, cut;
	size_t want;

	printf("== output side: markers, nesting, forgery, splits ==\n");
	s_reset();
	S_ADD(START);
	S_ADD("prompt$ \033[c answered long ago");
	S_ADD(END);
	want = stream_len;
	S_ADD("live \033[c");
	every_split("fence right after replay-end, at every split",
		    GATE_EXPECT_FIRST, want, 1);

	s_reset();
	S_ADD(START);
	S_ADD(END);
	S_ADD("live \033[c");
	every_split("an empty replay writes no fence", GATE_EXPECT_FIRST, 0,
		    0);

	s_reset();
	S_ADD(START);
	S_ADD("x");
	S_ADD(END);
	want = stream_len;
	every_split("a one-byte replay is fenced", GATE_EXPECT_FIRST, want,
		    1);

	s_reset();
	S_ADD(START);
	S_ADD("0123456789012345678901234567890123456789");
	every_split("live start marker, no expectation: ignored", 0, 0, 0);
	fresh(&g, 0);
	feed(&g, (size_t[]){ 0 }, 1000);
	CHECK(!gate_filtering(&g) && !gate_input_needed(&g, 1000),
	      "live start marker leaves input ungated");

	s_reset();
	S_ADD("upstream dtach sends no markers\r\n");
	S_ADD(START);
	S_ADD("0123456789012345678901234567890123456789");
	every_split("first attach: a marker not leading the stream is ignored",
		    GATE_EXPECT_FIRST, 0, 0);

	s_reset();
	S_ADD("output queued before the suspend\r\n");
	S_ADD(START);
	S_ADD("scrollback \033[c");
	S_ADD(END);
	want = stream_len;
	every_split("after suspend: the next replay-start anywhere is honoured",
		    GATE_EXPECT_ANY, want, 1);

	s_reset();
	S_ADD(START);
	S_ADD("before ");
	S_ADD(START);
	S_ADD("inner replay \033[c");
	S_ADD(ABORT);
	S_ADD("[reattach interrupted] after ");
	S_ADD(START);
	S_ADD("inner two");
	S_ADD(END);
	S_ADD(" \033[c tail");
	S_ADD(END);
	want = stream_len;
	S_ADD("live");
	every_split("nested start/abort and start/end: fence after the outer end",
		    GATE_EXPECT_FIRST, want, 1);

	s_reset();
	S_ADD(START);
	S_ADD("a \033[5n b \033[5n c");
	S_ADD(END);
	want = stream_len;
	every_split("replayed DSR queries are counted into the fence",
		    GATE_EXPECT_FIRST, want, 3);

	/* Multi-chunk: a 200KB replay with queries at the start, middle and
	** end, fed in BUFSIZE reads like the real client. */
	{
		size_t cuts[80], i, k = 0;

		s_reset();
		S_ADD(START);
		S_ADD("\033[c");
		while (stream_len < 100000)
			S_ADD("line of agent output 0123456789\r\n");
		S_ADD("\033[c");
		while (stream_len < 200000)
			S_ADD("line of agent output 0123456789\r\n");
		S_ADD("\033[c");
		S_ADD(END);
		want = stream_len;
		S_ADD("live");
		for (i = BUFSIZE; i < stream_len && k < 79; i += BUFSIZE)
			cuts[k++] = i;
		cuts[k] = 0;
		fresh(&g, GATE_EXPECT_FIRST);
		feed(&g, cuts, 1000);
		CHECK(nfences == 1 && fences[0] == want,
		      "multi-chunk replay: one fence after its end");
		CHECK(g.replay_bytes == want - MARKER_LEN(START),
		      "multi-chunk replay: every byte counted");
		if (nfences == 1 && fences[0] == want)
			printf("   PASS: multi-chunk replay (%lu reads)\n",
			       (unsigned long)k + 1);
	}

	/* Two replays in one read (suspend and resume): two fences. */
	s_reset();
	S_ADD(START);
	S_ADD("one");
	S_ADD(END);
	want = stream_len;
	S_ADD(START);
	S_ADD("two");
	S_ADD(END);
	fresh(&g, GATE_EXPECT_FIRST);
	feed(&g, (size_t[]){ 0 }, 1000);
	CHECK(nfences == 1 && fences[0] == want && g.depth == 0,
	      "a second replay without a second attach is not honoured");
	fresh(&g, GATE_EXPECT_FIRST);
	nfences = 0;
	{
		size_t off = 0;
		int f = 0;

		while (off < stream_len)
		{
			off += gate_output(&g, stream + off, stream_len - off,
					   1000);
			if (g.probe_due)
			{
				++f;
				gate_fence_sent(&g, 1000);
				gate_expect(&g, GATE_EXPECT_ANY);
			}
		}
		CHECK(f == 2 && g.status_owed == 2,
		      "two replays in one read get two fences");
	}

	printf("== input side: the report filter ==\n");
	{
		static const char *drop[] = {
			"\033[?1;2c", "\033[>1;95;0c", "\033[24;80R",
			"\033[?24;80;1R", "\033[?6n", "\033[8;24;80t",
			"\033[2;1;1;120;120;1;0x", "\033[?2004;1$y",
			"\033[?1u", "\033P>|xterm(390)\033\\",
			"\033P!|00000000\033\\", "\033P1$r0m\033\\",
			"\033]11;rgb:0000/0000/0000\007",
			"\033]10;rgb:ffff/ffff/ffff\033\\",
			"\033_Gi=1;OK\033\\", NULL
		};
		static const char *keep[] = {
			"abc", "\033[A", "\033OA", "\033[1;5A", "\033[3~",
			"\033[15~", "\033[97;5u", "\033[<0;10;5M",
			"\033[<0;10;5m", "\033[I", "\033[O",
			"\033[200~pasted\033[201~", "\033x", "\033\033[B",
			"\r", "\x03", "\033[1;2P", NULL
		};
		int i, bad = 0;

		for (i = 0; drop[i]; ++i)
			for (cut = 0; cut <= strlen(drop[i]); ++cut)
			{
				arm_fence(&g, 1000);
				g.status_owed = 5; /* keep the fence up */
				n = filter2(&g, drop[i], cut, out, 1000);
				n += gate_flush(&g, out + n, 1000);
				if (n != 0)
				{
					printf("   FAIL: report %d not dropped "
					       "at split %lu\n", i,
					       (unsigned long)cut);
					bad = 1;
				}
			}
		CHECK(!bad, "every report shape dropped at every split");
		if (!bad)
			printf("   PASS: every report shape dropped at every "
			       "split\n");
		bad = 0;
		for (i = 0; keep[i]; ++i)
			for (cut = 0; cut <= strlen(keep[i]); ++cut)
			{
				arm_fence(&g, 1000);
				g.status_owed = 5;
				n = filter2(&g, keep[i], cut, out, 1000);
				n += gate_flush(&g, out + n,
						1000 + GATE_HOLD_MS);
				if (n != strlen(keep[i]) ||
				    memcmp(out, keep[i], n) != 0)
				{
					printf("   FAIL: key %d altered at "
					       "split %lu\n", i,
					       (unsigned long)cut);
					bad = 1;
				}
			}
		CHECK(!bad, "keys, mouse, paste and text pass at every split");
		if (!bad)
			printf("   PASS: keys, mouse, paste and text pass at "
			       "every split\n");
	}

	arm_fence(&g, 1000);
	{
		static const char mix[] =
			"\033[?1;2cls\033[?1;2c\033[0n\033[?1;2cQ";

		n = gate_input(&g, (const unsigned char *)mix,
			       sizeof(mix) - 1, out, 1000);
	}
	expect_out("stale answers dropped, typing kept, live answer after the "
		   "fence forwarded", out, n, "ls\033[?1;2cQ");
	CHECK(!gate_filtering(&g) && g.status_owed == 0, "fence met");

	fresh(&g, GATE_EXPECT_FIRST);
	s_reset();
	S_ADD(START);
	S_ADD("\033[5n\033[c");
	S_ADD(END);
	feed(&g, (size_t[]){ 0 }, 1000);
	n = gate_input(&g, (const unsigned char *)"\033[0n\033[?1;2c", 11,
		       out, 1000);
	CHECK(n == 0 && gate_filtering(&g),
	      "the stale DSR answer is not taken for the fence");
	n = gate_input(&g, (const unsigned char *)"\033[0n\033[?1;2c", 11,
		       out, 1000);
	expect_out("the fence answer releases the gate", out, n,
		   "\033[?1;2c");

	arm_fence(&g, 1000);
	n = gate_input(&g, (const unsigned char *)"\033", 1, out, 1000);
	CHECK(n == 0 && g.held_len == 1, "a lone ESC is held");
	CHECK(gate_flush(&g, out, 1000 + GATE_HOLD_MS - 1) == 0,
	      "held no longer than it has to be");
	n = gate_flush(&g, out, 1000 + GATE_HOLD_MS);
	expect_out("a held ESC is forwarded once its window ends", out, n,
		   "\033");
	n = gate_input(&g, (const unsigned char *)"\033[?1;2", 6, out, 1000);
	n += gate_input(&g, (const unsigned char *)"x", 1, out + n,
			1000 + GATE_HOLD_MS + 1);
	expect_out("an expired partial is forwarded unchanged, before new "
		   "input", out, n, "\033[?1;2x");

	printf("== time bounds ==\n");
	arm_fence(&g, 1000);
	{
		long long dl = g.fence_deadline;

		CHECK(dl == 1000 + GATE_FENCE_BASE_MS +
		      gate_scaled_ms(g.replay_bytes), "fence deadline rule");
		CHECK(gate_input_needed(&g, dl - 1) && gate_filtering(&g),
		      "filtering until the fence deadline");
		CHECK(!(gate_tick(&g, dl), gate_filtering(&g)),
		      "an unanswered fence releases at its deadline");
		n = gate_input(&g, (const unsigned char *)"\033[?1;2c\033[0n",
			       11, out, dl + 10);
		expect_out("after it, answers pass but a late fence answer is "
			   "still swallowed", out, n, "\033[?1;2c");
		arm_fence(&g, 1000);
		n = gate_input(&g, (const unsigned char *)"\033[0n", 4, out,
			       g.owed_until);
		expect_out("a fence answer later than that is forwarded", out,
			   n, "\033[0n");
		CHECK(!gate_input_needed(&g, g.owed_until),
		      "nothing left armed");
	}
	fresh(&g, GATE_EXPECT_FIRST);
	s_reset();
	S_ADD(START);
	S_ADD("scrollback with an inner ");
	S_ADD(START);
	S_ADD(" and no end");
	S_ADD(END);
	feed(&g, (size_t[]){ 0 }, 1000);
	{
		long long rel = 1000 + GATE_REPLAY_BASE_MS +
			gate_scaled_ms(g.replay_bytes);

		CHECK(nfences == 0 && g.depth == 1,
		      "an unbalanced inner start keeps the replay open");
		n = gate_input(&g, (const unsigned char *)"typing", 6, out,
			       2000);
		expect_out("...but typing still passes", out, n, "typing");
		CHECK(gate_input_needed(&g, rel - 1) &&
		      !gate_input_needed(&g, rel),
		      "a replay with no end releases at its bound");
	}
	CHECK(gate_scaled_ms((size_t)1 << 40) == GATE_MAX_SCALED_MS,
	      "scaled term capped");
	CHECK(gate_scaled_ms(GATE_MIN_RATE) == 1000, "scaled term rate");

	fresh(&g, 0);
	CHECK(!gate_input_needed(&g, 1000), "no replay: input path unchanged");

	printf("== terminal side: replay marker stripping (opt-in) ==\n");
	{
		static const char strip_want[] =
			"scroll\033[1mback\033[5nlive";
		static const char other[] =
			"plain\033]0;title\007\033]8;;https://x.test\033\\link"
			"\033]8;;\033\\\033[?2026h\033[2J\033\033]dtach-rev;"
			"replay-x\007\033]dtach-rev;other\007\033]dtach-rev;"
			"replay-en\033[0m\342\224\200\033]dtach-rev;replay-sta";

		s_reset();
		S_ADD(START);
		S_ADD("scroll\033[1mback");
		S_ADD(END);
		S_ADD("live");
		emit_every_split("stripped: no marker, fence kept, any split",
				 GATE_EXPECT_FIRST, 1, strip_want,
				 strlen(strip_want));
		emit2(GATE_EXPECT_FIRST, 1, 7, strip_want, strlen(strip_want));
		CHECK(!term_has_marker_bytes(), "no marker fragment reached "
		      "the terminal");
		{
			/* Byte by byte, as reads of one. */
			struct replay_gate bg;
			size_t i;

			fresh(&bg, GATE_EXPECT_FIRST);
			memset(&strip, 0, sizeof(strip));
			strip.enabled = 1;
			term_len = 0;
			for (i = 0; i < stream_len; ++i)
				emit_output(&bg, &strip, stream + i, 1, 1000);
			strip_flush(&strip);
			expect_out("stripped byte by byte", term, term_len,
				   strip_want);
		}

		s_reset();
		S_ADD("a");
		S_ADD(START);
		S_ADD("b");
		S_ADD(ABORT);
		S_ADD("c");
		S_ADD(END);
		S_ADD("d");
		emit_every_split("live start, abort and end markers stripped",
				 0, 1, "abcd", 4);

		s_reset();
		s_add(other, sizeof(other) - 1);
		emit_every_split("other bytes and lookalikes pass byte for byte",
				 0, 1, other, sizeof(other) - 1);

		{
			/* A prompt after an escape sequence is not held. */
			static const unsigned char prompt[] = "\033[0m$ ";
			unsigned char out[sizeof(prompt) + 32];
			size_t n;

			memset(&strip, 0, sizeof(strip));
			strip.enabled = 1;
			n = strip_output(&strip, prompt, sizeof(prompt) - 1,
					 out);
			CHECK(strip.held_len == 0, "prompt: nothing held");
			expect_out("prompt released without a flush", out, n,
				   (const char *)prompt);
		}

		{
			/* The client's own abort notice: marker only for
			** parsers, text alone when stripping is on. */
			static const char text[] = "[reattach interrupted - "
				"the session is still running, attach again]"
				"\r\n";
			char marked[160];
			const char *on, *off;

			snprintf(marked, sizeof(marked), "%s%s", ABORT, text);
			memset(&strip, 0, sizeof(strip));
			strip.enabled = 1;
			on = replay_abort_notice(&strip);
			strip.enabled = 0;
			off = replay_abort_notice(&strip);
			expect_out("abort notice, stripping on: no marker",
				   (const unsigned char *)on, strlen(on), text);
			expect_out("abort notice, stripping off: marker kept",
				   (const unsigned char *)off, strlen(off),
				   marked);
		}

		s_reset();
		S_ADD(START);
		S_ADD("scroll");
		S_ADD(END);
		{
			char keep[128];

			snprintf(keep, sizeof(keep), "%sscroll%s\033[5n",
				 START, END);
			emit_every_split("not enabled: markers kept for parsers",
					 GATE_EXPECT_FIRST, 0, keep,
					 strlen(keep));
		}
	}

	if (failures)
	{
		printf("FAIL: %d check(s) failed\n", failures);
		return 1;
	}
	printf("PASS: replay gate unit checks\n");
	return 0;
}
