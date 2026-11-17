# D58: the IRC client (`irc`)

Revision 1, 2026-11-17, built on branch `irc` (on the PC and in FS-UAE). Approved by the partner on 2026-11-17
(docs/decisions.md, "New approvals", item 2: "we want an IRC client"); the details delegated (in the brief: plain or
TLS with certificates checked as wget does, SASL, the usual commands, windows switched with keys, scrollback, nick
highlighting, ISO-8859-1 and UTF-8, a settings file, configurable keys, a reconnect, 40-60 KB). No kernel change, no
new system call, message, port or claimed number; one new file format (the settings, section 4).

## 1. In plain words

- **`irc` is a terminal program, 80 columns first:** a title row (the window's name and topic), the window's lines
  with their times, a status row (the time, your nick, the window, its members, what is new in other windows, the
  lag), and the input row. Windows: the status window, one for each channel, one for each private conversation;
  F1-F10, Ctrl-N/Ctrl-P or Shift-Left/Right switch them, Ctrl-X goes to the next one with something new.
- **TLS (port 6697) as wget does it**, the certificate checked against `/etc/ssl/anchors` and your own roots in
  `/etc/ssl/local/`. The TLS code lives in a helper, `/lib/tls/tlspipe` (`/lib/irc/tlspipe` until D59 shared it with
  the e-mail client; the package `tls`), loaded only for a TLS server: `irc` itself
  is 53 KB on the disk; with TLS the helper adds 107 KB while connected.
- **A settings file** (`~/.config/irc/config`: server, port, tls, nick, SASL, autojoin, highlight words, charset...),
  written with every setting commented out the first time; **every key** in `~/.config/irc/keys`, the format of
  `files`, `show` and `modplay`.
- **A lost connection is taken up again by itself** (after 5, 10, 30, then every 60 s), the channels joined again.

## 2. Existing work first (the partner's rule)

| Client | Licence | Size | Why not |
|---|---|---|---|
| sic (suckless) | MIT | 336 lines of C | one line in, the server's lines out: no windows, no status, no TLS of ours |
| ii (suckless) | MIT | 966 lines | a FIFO file tree per channel, no screen at all; another program would have to be the client |
| kirc | MIT | about 5,000 lines | one window; POSIX terminal and socket code to replace; grew past the size we want |
| catgirl | GPL-3.0+ | | not compatible with GPL-2.0-only; LibreSSL's libtls and curses |
| irssi | GPL-2.0+ | over 100,000 lines with glib | far too big for the stock A1200 |

None gives the screen, the keys or the TLS Zenix has, and the protocol part is small (a parser and some fifty
messages), so `irc` is our own: written from RFC 1459/2812 and the IRCv3 CAP and SASL documents, no client's source
copied (sic, ii and kirc were read for their size and their way, not their code).

## 3. The program

```
irc [-c FILE] [-n NICK] [-p PORT] [-t] [SERVER [#CHANNEL...]]      irc -K
```

- `SERVER` replaces the settings' server (plain, port 6667, unless `-t` or `-p`); channels after it replace the
  autojoin list; `-n` the nick; `-c` another settings file; `-K` prints the default keys.
- **Two parts, as telnet's:** the core `net/irc/` (the protocol, the windows and their scrollback, the commands; no
  system calls, built on the PC too) and `user/irc.c` (the screen, the keys, the socket, the helper, the reconnect).
  One process, one loop: `poll` on the keys and the server, a second's tick for the clock, the keepalive and the
  reconnect.
- **The screen** draws a new line by scrolling the region between the title and the status rows (`ESC[r`, one
  line feed): a message costs one row of output, not a screen. A window switch, a resize (SIGWINCH) or Ctrl-L
  redraw the whole. Lines wrap at the width with the time's indent; nicks in one of six colours by a hash, your own
  bold, highlights bold yellow, events cyan, errors red, notices green. The console's own scrollback is not used.
- **Commands:** `/join /part /msg /query /me /notice /nick /names /topic /whois /close /window N /connect /quote
  /clear /help /quit`; any other `/COMMAND` goes to the server as it is (`/mode`, `/kick`, `/away`, `/list`...);
  `//text` sends text starting with a slash. Long text is sent in pieces of 400 bytes of whole characters.
- **The server's side:** registration (a nick in use: `_` added, nine tries), SASL PLAIN (`CAP REQ :sasl`, then
  `CAP END`; a refusal said, the login goes on), PING/PONG and our own PING after 120 s of silence (the lag in the
  status row; 300 s of silence is a lost connection), 005's PREFIX, CHANTYPES and CHANMODES (member modes followed
  through MODE), names, topics, JOIN/PART/KICK/QUIT/NICK in every window they concern, CTCP ACTION, VERSION and PING
  (at most one answer in 2 s), whois and errors in the current window, other numerics in the status window.
- **Pacing:** lines to the server go through a queue, a burst of five and then one every 2 s (the servers' flood
  rule: a pasted text does not get you thrown out); PONG and QUIT go ahead of the queue.
- **Windows a stranger opens:** a private message from a new nick opens a query only while fewer than 16 windows
  are open (4 stay for channels; the rest is shown in the status window as `<nick:you>`).

## 4. The files

- `~/.config/irc/config` (frozen from now on, docs/frozen-interfaces.md): one `SETTING VALUE` a line, `#` comments,
  settings `server port tls nick user realname password sasl quit charset scrollback reconnect bell autojoin
  highlight` (the last two up to 8 words). An unknown setting is an error with its line number. Written the first
  time with every setting commented out, mode 600; when it holds a password and others may read it, `irc` says so.
- `~/.config/irc/keys`: the shared parser's format (display/keys.h); actions `send backspace delete left right home
  end kill_start kill_end kill_word history_up history_down complete page_up page_down line_up line_down
  next_window prev_window window N active_window redraw quit` (`quit` has no key by default: Ctrl-C does nothing,
  as in irssi, so a slip does not end the session).

## 5. TLS: a helper, not a library in `irc`

The crypto library (BearSSL and Zenix's glue, D21) is about 90 KB of code. Linked into `irc` it would be 140 KB on
every machine, also for plain servers. So `irc` starts `/lib/tls/tlspipe HOST PORT` once its TCP connection is made
(the time limit, the errors and the name lookup stay in `irc`): the socket is the helper's descriptor 2, the plain
text goes in on 0 and comes out on 1 (two pipes). The helper does what wget does (`user/tlstrust.c`, moved out of
wget.c so both have one copy: the clock's rules of D21 8.3, the seed, the anchors and the owner's roots), then says
on its first line `ok`, `ok clock YYYY-MM-DD` (the clock is not trusted: dates checked only against that floor) or
`error TEXT` (before it `info TEXT` lines, such as wget's "waiting for the random pool"), and relays. The pipe
to it is non-blocking on irc's side: a stalled server fills it, the keys go on. BearSSL's one buffer serves both ways (16 KB rather than 33 KB): data that arrives while
a line waits to go out is passed on first. A certificate that is refused is not retried; a network failure is.
The helper is not a command (no page; this note and `irc`'s page name it); the e-mail client (item 3 of the same
approval, D59) uses it as it is, so it moved to `/lib/tls/tlspipe` in a package of its own, `tls`, which `irc` and
`mail` depend on (two packages may not own one file; 2026-11-17, before any release of `irc`).

## 6. Text and safety

Everything from the server is untrusted. A line over 512 bytes is cut there and said once in the status window.
Each line is made clean UTF-8 before it is parsed: valid UTF-8 kept, every other byte read as ISO-8859-1 (so a
line that mixes the two shows both right), NUL a space; C0 controls, DEL and C1 (also written as UTF-8, such as
U+009B, which a console reads as CSI) dropped; mIRC's colour and format codes dropped. So no escape sequence from a
server or another user ever reaches the console. What we send is UTF-8, or ISO-8859-1 with `charset latin1`
(characters outside it as `?`). The console draws UTF-8 (D28); widths come from libz's `zu8*`, the console's own.

## 7. Size and memory (as built)

| | on the disk | notes |
|---|---|---|
| `irc` | 53,936 bytes | all at -Os; 92 KB in memory with a channel open (FS-UAE) |
| `/lib/tls/tlspipe` | 107,352 bytes | only while connected to a TLS server: 182 KB in memory with its buffers (FS-UAE) |
| wget | 121,424 -> 121,636 bytes | the shared trust code's call through a pointer |

Scrollback: 300 lines a window by default (`scrollback`, 20-5,000), each line its own allocation of its length; at
most 20 windows. The memory figures are FS-UAE's (tools/irc-test.sh prints them), on the A1200 with 4 MB of fast
RAM; the stock 2 MB A1200 was not tried.

## 8. Tests

- `tools/host/irc-test.sh` (PC, sanitizers): the core's units (clean text, case, base64, settings errors, SASL's
  lines, autojoin, a 1,000-byte line in pieces, the ring, highlights, queries), then sessions of the core over
  sockets against `tools/host/irc-fake.py` (registration with a nick in use, a MOTD with ISO-8859-1, UTF-8, mIRC
  codes, an escape sequence, a 1,200-byte line and garbage; PING; autojoin; messages both ways; /me, query, whois,
  names, topic, modes, a flood with a CTCP VERSION, quit, nick, completion; the server dropping the connection: the
  reconnect and the rejoin; /part, /quit), SASL accepted, refused and missing, and TLS with a test CA (a good
  certificate, another name, an unknown root).
- `tools/irc-test.sh` (FS-UAE, the A1200 with 4 MB of fast RAM, the A2065 to the same fake server in a namespace):
  the program itself over TLS, read off the console's grid with `termtest screen` step by step (section 1's keys,
  the drop and the reconnect with TLS again), and a plain run from the command line; the server's log checks every
  line sent. In STOCK (tools/check.sh): its time limits run in emulated seconds.

## 9. Not in this round

DCC (file transfers and chats), several servers at once, logging to files, IRCv3 extras beyond SASL (message tags,
server-time, away-notify), a nick list beside the text (80 columns), mIRC colours shown in colour (dropped), SASL
EXTERNAL (client certificates), proxies. Never run on the real A1200 yet.
