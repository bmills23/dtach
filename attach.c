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
** The markers the master brackets a scrollback replay with. A replay is the
** one thing the master can cut short without the session being over: it
** drops a client that stalls or trickles rather than letting it hold the
** session, and a dropped client just sees its socket close. EOF alone would
** be reported as "dtach terminating", telling the user the session is gone
** while the master is still running it, so track whether we are between the
** two markers and say something true instead.
*/
static const char replay_start_marker[] = "\033]dtach-rev;replay-start\007";
static const char replay_end_marker[] = "\033]dtach-rev;replay-end\007";
/* 1 while a replay-start has been seen with no replay-end after it. */
static int in_replay;

/*
** Advance a naive matcher for `marker` by one byte, returning 1 if the marker
** completed on it. The match state lives in *pos across calls, so a marker
** split across two reads is still seen.
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
** position reports and the like) that a program asked long ago and that were
** answered, live, back then. A full terminal emulator attached through this
** client (Terminal.app, via TerminaLLM's "Open Tab on Mac") parses the replay
** as if it were new and answers those queries again, and the answers used to
** be forwarded to the session as keystrokes, corrupting the next command.
**
** So keyboard input is dropped while a replay that carries data is streaming,
** and for REPLAY_INPUT_GRACE_MS after its replay-end marker has been written
** to the terminal. The replayed bytes still render, and a query issued after
** the window gets its answer forwarded as before. The detach and suspend keys
** still work while input is dropped.
**
** Nothing is dropped after an empty replay (no scrollback, or nothing
** printed yet). That includes the client that creates a session with -c or
** -A: the master does not read the pty until that client is attached
** (waitattach in master_process), so its replay is always empty and any
** query the program asks at startup is answered and forwarded as before.
** This mirrors replay_gate.go in terminallm-daemon.
*/
#define REPLAY_INPUT_GRACE_MS 500
/* Bytes received since the last replay-start, the end marker included. */
static size_t replay_bytes;
/* Monotonic ms before which keyboard input is dropped; 0 when not armed. */
static long long input_quiet_until;

#define REPLAY_END_LEN (sizeof(replay_end_marker) - 1)

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

/*
** Note the replay markers in a chunk of data received from the master.
** Returns 1 if a replay that carried data ended in this chunk, so the caller
** arms the input grace window once the chunk has reached the terminal.
*/
static int
track_replay(const unsigned char *buf, size_t len)
{
	static size_t start_pos, end_pos;
	int ended_with_data = 0;
	size_t i;

	for (i = 0; i < len; ++i)
	{
		int started = marker_step(replay_start_marker, &start_pos,
					  buf[i]);
		int ended = marker_step(replay_end_marker, &end_pos, buf[i]);

		if (in_replay)
			++replay_bytes;
		if (started)
		{
			in_replay = 1;
			replay_bytes = 0;
		}
		else if (ended)
		{
			if (in_replay && replay_bytes > REPLAY_END_LEN)
				ended_with_data = 1;
			in_replay = 0;
		}
	}
	return ended_with_data;
}

/* 1 if keyboard input should be dropped right now; see the A19 comment. */
static int
replay_input_blocked(void)
{
	/* Mid-replay, once at least one byte of replayed data has arrived:
	** only then can replay_bytes reach REPLAY_END_LEN with no end yet. */
	if (in_replay && replay_bytes >= REPLAY_END_LEN)
		return 1;
	return input_quiet_until != 0 && now_ms() < input_quiet_until;
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

		/* Tell the master that we are returning. */
		pkt->type = MSG_ATTACH;
		write_packet_or_fail(s, pkt);

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

	/* We would like a redraw, too. */
	pkt.type = MSG_REDRAW;
	pkt.len = redraw_method;
	ioctl(0, TIOCGWINSZ, &pkt.u.ws);
	write_packet_or_fail(s, &pkt);

	/* Wait for things to happen */
	while (1)
	{
		int n;

		FD_ZERO(&readfds);
		FD_SET(0, &readfds);
		FD_SET(s, &readfds);
		n = select(s + 1, &readfds, NULL, NULL, NULL);
		if (n < 0 && errno != EINTR && errno != EAGAIN)
		{
			printf(EOS "\r\n[select failed]\r\n");
			exit(1);
		}

		/* Pty activity */
		if (n > 0 && FD_ISSET(s, &readfds))
		{
			ssize_t len = read(s, buf, sizeof(buf));
			int arm;

			if (len == 0)
			{
				/* Cut off mid-replay: the master dropped
				** us, but the session is still running. */
				if (in_replay)
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
				printf(EOS "\r\n[read returned an error]\r\n");
				exit(1);
			}
			/* Send the data to the terminal. */
			arm = track_replay(buf, (size_t)len);
			write_buf_or_fail(1, buf, len);
			if (arm)
				input_quiet_until = now_ms() +
					REPLAY_INPUT_GRACE_MS;
			n--;
		}
		/* stdin activity */
		if (n > 0 && FD_ISSET(0, &readfds))
		{
			ssize_t len;

			pkt.type = MSG_PUSH;
			memset(pkt.u.buf, 0, sizeof(pkt.u.buf));
			len = read(0, pkt.u.buf, sizeof(pkt.u.buf));

			if (len <= 0)
				exit(1);

			pkt.len = len;
			/* Drop what is most likely the terminal answering a
			** replayed query (A19), but never the local detach or
			** suspend key. */
			if (!replay_input_blocked() ||
			    pkt.u.buf[0] == detach_char ||
			    (!no_suspend &&
			     pkt.u.buf[0] == cur_term.c_cc[VSUSP]))
				process_kbd(s, &pkt);
			n--;
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
