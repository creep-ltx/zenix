/* SPDX-License-Identifier: GPL-2.0-only */
/* Between term's cell model (model.c: the kernel's term.c) and the program (term.c): plain C types only. */
#pragma once
/* the model calls the program: */
void m_cell(int row, int col, unsigned v, int attr, int colour);    /* a cell: its value, attributes, colours */
void m_fill(int row, int n, int colour);    /* n rows cleared in the colour byte's background */
void m_move(int dst, int src, int n);       /* n rows moved */
void *m_alloc(unsigned n);
void m_free(void *p);
void m_bell(void);
void m_answer(const char *s);           /* the terminal answers the program (ESC[6n): into the pty */
/* the program calls the model: */
void m_write(const char *s, unsigned n);
void m_redraw(void);
void m_rows(int rows);
int m_ckm(void);
void m_scrollback(int delta);
