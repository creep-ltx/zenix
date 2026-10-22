/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include "zdev.h"

static int m_read(void *c, uint32_t blk, void *buf)
{
    struct zmem *m = c;
    if (blk >= m->dev.nblocks) return -1;
    m->reads++;
    memcpy(buf, m->img + ((size_t)blk << m->dev.bshift), (size_t)1 << m->dev.bshift);
    return 0;
}
static int m_write(void *c, uint32_t blk, const void *buf)
{
    struct zmem *m = c;
    size_t B = (size_t)1 << m->dev.bshift;
    if (blk >= m->dev.nblocks) return -1;
    m->writes++;
    if (m->record) {
        if (m->nlog == m->caplog) { m->caplog = m->caplog ? m->caplog * 2 : 1024; m->log = realloc(m->log, m->caplog * sizeof *m->log); }
        struct zlog *e = &m->log[m->nlog++];
        e->is_flush = 0; e->blk = blk; e->data = malloc(B); memcpy(e->data, buf, B);
    }
    memcpy(m->img + ((size_t)blk << m->dev.bshift), buf, B);
    return 0;
}
static int m_flush(void *c)
{
    struct zmem *m = c;
    m->flushes++;
    if (m->record) {
        if (m->nlog == m->caplog) { m->caplog = m->caplog ? m->caplog * 2 : 1024; m->log = realloc(m->log, m->caplog * sizeof *m->log); }
        struct zlog *e = &m->log[m->nlog++];
        e->is_flush = 1; e->blk = 0; e->data = NULL;
    }
    return 0;
}
struct zmem *zmem_new(uint32_t bshift, uint32_t nblocks, int record)
{
    struct zmem *m = calloc(1, sizeof *m);
    m->img = calloc(nblocks, (size_t)1 << bshift);
    m->record = record;
    m->dev.ctx = m; m->dev.read = m_read; m->dev.write = m_write; m->dev.flush = m_flush;
    m->dev.bshift = bshift; m->dev.nblocks = nblocks;
    return m;
}
struct zmem *zmem_clone(const struct zmem *s)
{
    struct zmem *m = zmem_new(s->dev.bshift, s->dev.nblocks, 0);
    memcpy(m->img, s->img, (size_t)s->dev.nblocks << s->dev.bshift);
    return m;
}
void zmem_apply(struct zmem *m, const struct zlog *e)
{
    if (!e->is_flush) memcpy(m->img + ((size_t)e->blk << m->dev.bshift), e->data, (size_t)1 << m->dev.bshift);
}
void zmem_free(struct zmem *m)
{
    if (!m) return;
    for (size_t i = 0; i < m->nlog; i++) free(m->log[i].data);
    free(m->log); free(m->img); free(m);
}

static int f_read(void *c, uint32_t blk, void *buf)
{
    struct zfile *f = c; size_t B = (size_t)1 << f->dev.bshift;
    return pread(f->fd, buf, B, (off_t)blk * B) == (ssize_t)B ? 0 : -1;
}
static int f_write(void *c, uint32_t blk, const void *buf)
{
    struct zfile *f = c; size_t B = (size_t)1 << f->dev.bshift;
    return pwrite(f->fd, buf, B, (off_t)blk * B) == (ssize_t)B ? 0 : -1;
}
static int f_flush(void *c) { struct zfile *f = c; return fsync(f->fd); }
int zfile_open(struct zfile *f, const char *path, uint32_t bshift, int create_blocks)
{
    f->fd = open(path, O_RDWR | (create_blocks ? O_CREAT | O_TRUNC : 0), 0644);
    if (f->fd < 0) return -1;
    if (create_blocks && ftruncate(f->fd, (off_t)create_blocks << bshift) < 0) return -1;
    struct stat st; fstat(f->fd, &st);
    f->dev.ctx = f; f->dev.read = f_read; f->dev.write = f_write; f->dev.flush = f_flush;
    f->dev.bshift = bshift; f->dev.nblocks = (uint32_t)(st.st_size >> bshift);
    return 0;
}
void zfile_close(struct zfile *f) { close(f->fd); }
