# dtach-rev

**A fork of [dtach](https://github.com/cribbe/dtach) with a 256KB scrollback buffer for terminal session reconnection context.**

dtach-rev extends the original dtach with a circular scrollback buffer that captures terminal output and replays it to newly attaching clients. When you detach from a session and reattach later, you get the recent output history instead of a blank screen. This is particularly useful for AI coding assistants (Claude Code, Aider, etc.) running over SSH, where reconnection context is critical.

## Attribution

dtach was originally written by **Ned T. Crigler** and is Copyright (C) 2004-2016 Ned T. Crigler. The original project is available at [github.com/cribbe/dtach](https://github.com/cribbe/dtach).

This fork adds the scrollback buffer feature described below. All original dtach functionality is preserved. Both the original and this fork are licensed under the [GNU General Public License v2.0](COPYING).

## What Changed

The following modifications were made to the original dtach v0.9:

- **Scrollback buffer** (`master.c`): A circular buffer (default 256KB) captures all PTY output. When a client attaches, the buffer contents are replayed so the client has session context.
- **OSC-delimited replay** (`master.c`): Replayed content is wrapped in `\033]dtach-rev;replay-start\007` / `\033]dtach-rev;replay-end\007` OSC sequences so smart clients can distinguish replayed content from live output.
- **`-b` flag** (`main.c`): Configurable scrollback buffer size at runtime. Supports K/M suffixes (e.g., `-b 512K`, `-b 1M`). Set to `0` to disable.
- **Header updates** (`dtach.h`): Added `scrollback_size` global, `DEFAULT_SCROLLBACK_SIZE` constant, and `MSG_CONTENT` enum value.

No other behavior was changed. dtach-rev is a drop-in replacement for dtach.

## Installation

### Homebrew (macOS / Linux)

```bash
brew tap bmills23/terminallm
brew install dtach-rev
```

> **Note:** dtach-rev conflicts with the standard `dtach` package since both install a `dtach` binary. Uninstall one before installing the other.

### From Source

```bash
git clone https://github.com/bmills23/dtach-rev.git
cd dtach-rev
./configure
make
sudo cp dtach /usr/local/bin/
```

## Usage

dtach-rev is used identically to dtach. All original modes and options work the same way.

### Quick Start

```bash
# Create a new session (or attach to existing)
dtach -A /tmp/my-session bash

# Detach: press Ctrl-\

# Reattach (scrollback buffer replays recent output)
dtach -a /tmp/my-session
```

### New Option: Scrollback Buffer Size

```bash
# Default: 256KB scrollback buffer
dtach -A /tmp/my-session bash

# Custom size (512KB)
dtach -A /tmp/my-session -b 512K bash

# 1MB buffer
dtach -A /tmp/my-session -b 1M bash

# Disable scrollback (original dtach behavior)
dtach -A /tmp/my-session -b 0 bash
```

### All Modes

| Mode | Description |
|------|-------------|
| `-a <socket>` | Attach to an existing session |
| `-A <socket> <cmd>` | Attach or create if it doesn't exist |
| `-c <socket> <cmd>` | Create a new session and attach |
| `-n <socket> <cmd>` | Create a new session, detached (daemonize) |
| `-N <socket> <cmd>` | Create a new session, detached (foreground) |
| `-p <socket>` | Pipe stdin to an existing session |

### Options

| Option | Description |
|--------|-------------|
| `-e <char>` | Set detach character (default: `^\`) |
| `-E` | Disable detach character |
| `-r <method>` | Redraw method: `none`, `ctrl_l`, `winch` |
| `-b <size>` | Scrollback buffer size (default: `256K`). Supports `K`/`M` suffixes. `0` to disable. |
| `-z` | Disable suspend key processing |

## Why This Fork Exists

The standard dtach does not retain any terminal output between sessions. When you detach and reattach, you get a blank screen. This is fine for interactive shells where you can press Enter or run a command to see where you are.

However, for AI coding assistants running over SSH from mobile devices, losing context on reconnection is a significant problem. The AI assistant's conversation, file diffs, and command output disappear. dtach-rev solves this by buffering the last 256KB of output and replaying it when you reconnect.

## License

This program is free software; you can redistribute it and/or modify it under the terms of the [GNU General Public License v2.0](COPYING) as published by the Free Software Foundation.

The original dtach is Copyright (C) 2004-2016 Ned T. Crigler.
Modifications are Copyright (C) 2026 Bryan Mills.

See [COPYING](COPYING) for the full license text.
