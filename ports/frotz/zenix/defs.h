/* SPDX-License-Identifier: GPL-2.0-only */
/* Frotz's build-time settings (upstream's Makefile writes this file as src/common/defs.h; ports/frotz/README):
 * upstream's values for a Unix build, and no Blorb (pictures and sounds; the dumb front end shows neither). Undo
 * keeps 25 turns by default (upstream's DEFAULT_UNDO_SLOTS), each the bytes that changed since the turn before. */
#ifndef COMMON_DEFINES_H
#define COMMON_DEFINES_H
#define RELEASE_NOTES "Official release."
#define UNIX
#define MAX_UNDO_SLOTS 500
#define MAX_FILE_NAME 80
#define TEXT_BUFFER_SIZE 512
#define INPUT_BUFFER_SIZE 200
#define STACK_SIZE 1024
#define USE_UTF8
#define NO_BLORB
#endif /* COMMON_DEFINES_H */
