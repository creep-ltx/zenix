# D59: the e-mail client (`mail`)

Revision 1, 2026-11-17, built on branch `mail` (on the PC and in FS-UAE). Approved by the partner on 2026-11-17
(docs/decisions.md, "New approvals", item 3: "email"); the details delegated (in the brief: IMAP over TLS, POP3 if
cheap, SMTP with STARTTLS or TLS and AUTH PLAIN/LOGIN; a list of folders and messages, a message read with its
encodings and charsets right, HTML made text, attachments listed and saved; compose, reply, forward in `edit` or
$EDITOR; delete, move, read marks; `~/.config/mail/config` owner-only with a password or a command; configurable
keys; 60-90 KB plus the shared TLS helper; nothing from a message reaches the console as an escape). No kernel
change, no new system call, message, port or claimed number; one new file format (the settings, section 3).

## 1. In plain words

- **`mail` is a terminal program, 80 columns first:** a title row (the folder, its counts), the index (newest first:
  unread, deleted, answered, attachments, the date, who, the subject), a message, the folders or the keys, a row with
  the main keys (read from your keys file) and a row for messages and questions.
- **Online only, like mutt with IMAP:** the messages stay on the server; mail keeps nothing on the disk but a draft
  while it is written. POP3 works too (one folder, no read marks).
- **TLS everywhere it is asked for:** IMAP on 993 and SMTP on 465 from the start, or STARTTLS (IMAP 143, POP3's STLS,
  SMTP 587); certificates checked as wget checks them. A server that will not start TLS is refused, never used plain,
  and a password is never sent without TLS unless the settings say `plain_auth on`.
- **Writing in your editor:** `m`, `r`, `g` (to everyone), `f` start `edit` (or `$EDITOR`, or the setting) on a draft
  with its headers; after it, `y` sends, `e` edits again, `q` drops it.
- **`mail -s SUBJECT ADDRESS... < TEXT`** sends from scripts, as the old Unix `mail` does.

## 2. Existing work first (the partner's rule)

| Client or library | Licence | Size (approximate, from the projects, not measured here) | Why not |
|---|---|---|---|
| mutt / NeoMutt | GPL-2.0+ | about 100,000 lines of C, with curses and OpenSSL or GnuTLS | far too big for the stock A1200; its own TLS and terminal layers to replace |
| Alpine | Apache-2.0 | several hundred thousand lines | Apache-2.0 is not compatible with GPL-2.0-only |
| s-nail / Heirloom mailx | BSD-style | tens of thousands of lines | a line-mode mailx, not a screen; OpenSSL |
| evergreen (InterLinked) | GPL-2.0 | small itself | needs a patched libetpan, ncurses and html2text: far bigger than the client |
| libetpan | BSD-3 | a large library (IMAP, POP3, SMTP, MIME) | bigger than this whole client; OpenSSL |
| libcurl | curl (MIT-like) | large | IMAP, POP3 and SMTP transport only, no MIME reading or writing |
| ssmtp | GPL-2.0+ | small | sends only; OpenSSL |
| msmtp, mpop | GPL-3.0+ | small | not compatible with GPL-2.0-only; one direction each |
| aerc, meli, himalaya | MIT, GPL-3.0, MIT | | Go and Rust: not for a 68020 without an MMU |

No small GPL-2-compatible C client with a screen exists that does not bring libraries several times the target, and
the protocols are small when only what a client needs is written. So `mail` is our own, as `irc` is (D58): written
from RFC 3501 (IMAP4rev1), 1939 and 2595 (POP3, STLS), 5321, 3207 and 4954 (SMTP, STARTTLS, AUTH), 5322 (messages),
2045-2047 and 2231 (MIME, words, parameters); no client's code copied.

## 3. The program and its files

```
mail [-c FILE] [-f FOLDER]      mail [-c FILE] -s SUBJECT ADDRESS... < TEXT      mail -K
```

- **Two parts, as irc's:** the core `net/mail/` (`proto.c` the protocols over a connection the program gives,
  `mime.c` the reader, `compose.c` the settings and the messages written; no system calls, built on the PC too) and
  `user/mail.c` (the screen, the keys, the sockets, the TLS helper, the password, the editor).
- **The views:** the index, a message (headers, text, the attachments numbered), the folders, the keys. A message is
  fetched whole up to 512 KB (`BODY.PEEK[]`, then `\Seen` set; a bigger one's first 512 KB, said); its text is made
  once (up to 1 MB) and cut into rows at the width (words kept whole where a space is in the row's last third).
- **The operations:** read (`\Seen` set), read/unread, delete (`\Deleted`; removed by `$` or, asked, at the end;
  POP3: DELE, an undelete is RSET and the others again), move (COPY and `\Deleted`; Tab offers the folders), save an
  attachment, reply (`\Answered` set once sent), forward, a copy of what is sent APPENDed to `sent`.
- **A lost connection** (a server's idle limit) is made again at the next key: logged in again, the folder selected
  again, the message shown and the one acted on found again by their UIDs. POP3's message numbers are not UIDs: after
  a reconnect a POP3 action is not done (said; the key again does it on the index read again), so a deletion never
  hits another message.
- **`~/.config/mail/config`** (frozen from now on, docs/frozen-interfaces.md): one `SETTING VALUE` a line, `#`
  comments; settings `imap pop tls port user password passcmd smtp smtp_tls smtp_port smtp_user smtp_password
  smtp_passcmd from sent folder editor limit plain_auth`; an unknown setting is an error with its line number; `imap`
  and `pop` both an error. Written the first time with every setting commented out, mode 600 in a folder of mode 700.
  mail refuses a file that is not yours, one others may change (a `passcmd` would be theirs) and one holding a
  password others may read. The draft is created anew (exclusive create) in that private folder.
- **`~/.config/mail/keys`:** the shared parser's format (display/keys.h); actions `up down page_up page_down top
  bottom open back quit folders compose reply reply_all forward delete undelete toggle_read move save sync headers
  next prev help redraw`. The keys from a terminal are read by `keys_tty` in display/lib/keys.c, irc's reader moved
  there so that irc and mail share one copy (D13).
- **The password:** `password`, or the first line `passcmd` prints (`sh -c`), else asked on the last row (not shown);
  kept in memory only, wiped at the end (the settings' text too, and a protocol line that carried it once sent); a
  refused one is asked again. Sending uses the same unless `smtp_*` differ.

## 4. The protocols, and TLS through the shared helper

- **IMAP:** the greeting (`PREAUTH` skips the login), `STARTTLS`, `LOGIN` with quoted strings (a literal for a name
  or password with other bytes), `LIST "" "*"` (`\Noselect` left out; names in modified UTF-7 shown as UTF-8, INBOX
  first), `SELECT`, one `FETCH lo:hi (UID FLAGS RFC822.SIZE BODY.PEEK[HEADER.FIELDS (DATE FROM SUBJECT CONTENT-TYPE)])`
  for the newest `limit` messages, then everything by UID (`UID FETCH`, `UID STORE ... FLAGS.SILENT`, `UID COPY`),
  `EXPUNGE`, `APPEND` (literal, after the `+`), `LOGOUT`. A response is read whole with its literals; untagged
  `EXISTS` and `BYE` are followed.
- **POP3:** `STLS`, `USER`/`PASS`, `STAT`, `TOP n 0` for the index, `RETR`, `DELE`, `RSET`, `QUIT` (the deletions
  happen there); multi-line answers dot-unstuffed.
- **SMTP:** the greeting, `EHLO zenix` (`HELO` when refused), `STARTTLS` and `EHLO` again, `AUTH PLAIN` (or `AUTH
  LOGIN` when only that is offered), `MAIL FROM`, `RCPT TO` each (a refusal says which address), `DATA` with dots
  doubled and CR LF lines, `QUIT`.
- **TLS:** the helper `/lib/tls/tlspipe HOST PORT` of D58 section 5, unchanged in what it does: mail connects, then
  starts it with the socket as its descriptor 2 (also after a STARTTLS answer: nothing may be waiting from before it,
  RFC 3207 5, else the connection is refused), reads its first line (`ok`, `ok clock DATE`, `info TEXT`, `error
  TEXT`) and talks through two pipes. **The helper moved from `/lib/irc/tlspipe` to `/lib/tls/tlspipe`, into a package
  of its own, `tls`** (a family of files only in tools/mkzup.py), which `irc` and `mail` depend on: two packages may
  not own one file, and irc had not been released. The crypto library stays out of both programs.

## 5. A message read, and safety

Everything from a server or in a message is untrusted. A protocol line over 8 KB is cut (its end still read, a
literal's `{N}` found from the line's true tail); a literal over 16 MB, or one with ten or more digits, ends the
connection; at most 8 literals a response, 2 KB of header fields a message in the index, 200 folders, 32 attachments
listed (more counted), 8 levels of MIME, 50 recipients. The MIME reader walks the tree: `multipart/alternative` takes
its plain part, else HTML; `message/rfc822` is shown in place under its headers; other parts are attachments (their
name from `filename*`, its RFC 2231 pieces and charset, `filename`, then `name`; '/', '\' and ':' made '_', leading
dots dropped; a part's type and charset kept to a token's characters). Headers are unfolded and RFC 2047 words decoded (the space between two words dropped). Text in
ISO-8859-1, ISO-8859-15, Windows-1252 or UTF-8 is made UTF-8; another charset is read as UTF-8 where valid, else
ISO-8859-1. HTML is made text: `head`, `title`, `style` and `script` dropped, blocks and lists as lines, entities
(numeric and the common names; 128-159 read as Windows-1252, as HTML does), links as `text <URL>`. **What comes out
is clean:** no C0 control but the newline (tabs as spaces), no DEL, no C1 (also when written as UTF-8 or as an
entity, such as U+009B, which a console reads as CSI), no bidirectional overrides; a server's own words (errors,
folder names) are cleaned the same way, and the server's raw folder names are never printed. So nothing from the
network can move the cursor or change the console.

## 6. A message written

The draft's headers are read back (folded lines joined, cleaned: no line break can be smuggled into a header); `To`,
`Cc` and `Bcc` give the envelope (each address checked: no spaces, controls or specials, one `@`), `Bcc` never goes
into the message. Written: `Date` (UTC), `From`, `To`, `Cc`, `Reply-To`, `Subject` (RFC 2047 B words of whole
characters when not ASCII, folded), `Message-ID` (`<time.random@your domain>`: no host name leaks), `In-Reply-To`
and `References` for a reply, other ASCII headers as typed, `MIME-Version`, `text/plain` in `us-ascii` (7bit) or
`utf-8` (quoted-printable when not ASCII or a line is over 998 bytes). A header the user adds needs an ASCII name and
value and may not be one mail writes itself (Date, Message-ID, MIME and Content headers). `mail -s` refuses a subject
or an address with a control character (no header smuggled in from a script). A reply quotes the text with `> ` (`>` before a
line already quoted) under "On DATE, WHO wrote:"; to everyone drops you and the one in To; a forward has the text under
"-------- Forwarded message --------" with its headers.

## 7. Size and memory (as built)

| | on the disk | notes |
|---|---|---|
| `mail` | 83,004 bytes | all at -Os; 135 KB in memory with the index of 6 messages and a message read (FS-UAE) |
| `/lib/tls/tlspipe` | 107,352 bytes | the package tls, while an encrypted connection is open: 182 KB with its buffers (FS-UAE) |
| `irc` | 53,936 -> 54,008 bytes | the shared key reader is now a call into keys.c |

The index costs one allocation a message (its name and subject) plus 20 bytes; a message read its raw bytes (up to
512 KB), its text (up to 1 MB) and the rows (6 bytes each). The memory figures are FS-UAE's (tools/mail-test.sh
prints them), on the A1200 with 4 MB of fast RAM; the stock 2 MB A1200 was not tried.

The core keeps big buffers off the 16 KB stack (the deepest path, a reply to a message of 8 nested MIME levels, needs
about 8.5 KB, measured with -fstack-usage); mail runs with the default stack.

An independent review (2026-11-17) found the stack depth (big buffers, then about 16.5 KB on the deepest paths), an
attachment's raw Content-Type reaching the screen, the settings' wipe stopping at its first NUL, POP3 numbers after a
reconnect, a stale message index after a reconnect, header injection through `mail -s`, reply-to-all losing everyone
on one bad address, characters cut in two at a cap, the draft's creation, and protocol details (long commands cut,
an answer left half read after running out of memory, tags wrapping at 1,000): all fixed in the same branch, with
tests for the ones the core can show.

## 8. Tests

- `tools/host/mail-test.sh` (PC, sanitizers): the core's units (`tools/host/mail-core-test.c`: quoted-printable,
  base64, the charsets, RFC 2047, dates, a multipart message with an alternative and RFC 2231 names, HTML only, 20
  levels of nesting, a forwarded message, a part with no headers and no closing delimiter, escape sequences and C1 in
  text and headers, the settings' errors, addresses, the message built (envelope, Bcc, words, quoted-printable, dots),
  replies to everyone and forwards, modified UTF-7), then sessions of the core over sockets
  (`tools/host/mail-client.c`) against `tools/host/mail-fake.py` (`tools/host/mail-test.py`): IMAP over TLS with a
  test CA (a wrong password, the folders, the index, each message read, an attachment saved byte for byte, flags,
  copy, expunge, append), STARTTLS, another name and an unknown root refused, no password without TLS, quoting and a
  literal in LOGIN; hostile servers (20,000-byte lines and 5,000 bytes of header fields; a literal of 99,999,999,999
  bytes; garbage in LIST; BYE in a FETCH); POP3 (STLS, TOP, RETR, DELE, RSET, QUIT); SMTP (STARTTLS and implicit TLS,
  a wrong password, no STARTTLS offered, a refused recipient, the message received byte for byte, Bcc in the envelope
  only).
- `tools/mail-test.sh` (FS-UAE, the A1200 with 4 MB of fast RAM, the A2065 to the same fake servers in a namespace:
  IMAP over TLS on 993, SMTP with STARTTLS on 587): the program itself, read off the console's grid with `termtest
  screen` step by step: the index, a message with its attachments, one saved (compared byte for byte), the HTML-only
  message (its escapes shown as text, no colour from them), a reply and a new message through an editor that is a
  script, both sent, the folders, a delete and its removal at the end; then `mail -s`. Each message the fake server
  received is compared byte for byte with the same draft built on the PC by the same core (Date and Message-ID
  aside). In STOCK (tools/check.sh): its time limits run in emulated seconds.

## 9. Not in this round

Attachments in what is sent, a local mailbox or cache, search, threads, sorting, S/MIME and PGP, OAuth2 (servers that
need it refuse LOGIN), format=flowed, other charsets, IMAP IDLE, POP3 UIDL and read marks, several accounts. Never
run on the real A1200 yet.
