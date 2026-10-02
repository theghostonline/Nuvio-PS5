/*
 * Module: evo_readdir - see include/evo_readdir.h.
 */

#include "evo_readdir.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <sys/stat.h>
#include "evo_boot_trace.h"

#ifdef EVO_APP_MODULE

#ifndef O_DIRECTORY
#define O_DIRECTORY 0x00020000
#endif

/* FreeBSD getdents(2) and getdirentries(2), wrapped by libc / libkernel with no
 * fstatfs/malloc/fdopendir in the way. Declared here because <dirent.h> only
 * exposes them under _WANT_ macros on some SDKs. */
int getdents(int fd, char *buf, int nbytes);
int getdirentries(int fd, char *buf, int nbytes, long *basep);

/* UFS / APFS on PS5 uses 64KB block size for /data. FreeBSD getdirentries(2) /
 * getdents(2) requires the buffer to be at least as large as the filesystem's
 * block size (st_blksize), or returns EINVAL. */
#define EVO_READDIR_MIN_BUF_SIZE  65536

struct evo_dir {
    char   *buf;
    size_t  buf_size;
    int     fd;
    int     len;   /* valid bytes in buf */
    int     pos;   /* cursor into buf    */
    long    base;
};

evo_dir_t *evo_opendir(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0) {
        /* Some filesystems or symlinks fail with O_DIRECTORY, retry plain O_RDONLY */
        fd = open(path, O_RDONLY);
    }
    if (fd < 0) {
        evo_bt("evo_opendir: open('%s') failed (errno=%d)", path, errno);
        return NULL;
    }

    evo_dir_t *d = calloc(1, sizeof *d);
    if (!d) {
        int e = errno;
        close(fd);
        errno = e;
        return NULL;
    }

    size_t bsize = EVO_READDIR_MIN_BUF_SIZE;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_blksize > (blksize_t)bsize) {
        bsize = (size_t)st.st_blksize;
    }

    d->buf = calloc(1, bsize);
    if (!d->buf) {
        int e = errno;
        close(fd);
        free(d);
        errno = e;
        return NULL;
    }

    d->buf_size = bsize;
    d->fd = fd;
    d->base = 0;
    evo_bt("evo_opendir('%s') -> fd=%d bsize=%zu", path, fd, bsize);
    return d;
}

struct dirent *evo_readdir(evo_dir_t *d)
{
    if (!d)
        return NULL;

    for (;;) {
        if (d->pos >= d->len) {
            errno = 0;
            int n = getdents(d->fd, d->buf, (int)d->buf_size);
            if (n <= 0) {
                /* Fallback to getdirentries if getdents returned error or 0 */
                errno = 0;
                n = getdirentries(d->fd, d->buf, (int)d->buf_size, &d->base);
            }
            if (n <= 0) {
                evo_bt("evo_readdir: fd=%d read returned %d (errno=%d)",
                       d->fd, n, errno);
                return NULL;   /* end of directory, or error */
            }
            d->len = n;
            d->pos = 0;
        }

        struct dirent *de = (struct dirent *)(d->buf + d->pos);
        if (de->d_reclen == 0) {
            evo_bt("evo_readdir: fd=%d malformed entry d_reclen=0", d->fd);
            return NULL;       /* malformed block - stop rather than spin */
        }
        d->pos += de->d_reclen;

        /* getdents can leave empty slots (d_type == 0 && d_namlen == 0) in the
         * stream; readdir() hides them. Real entries always have a name. */
        if (de->d_namlen == 0)
            continue;

        return de;
    }
}

void evo_closedir(evo_dir_t *d)
{
    if (!d)
        return;
    if (d->fd >= 0)
        close(d->fd);
    if (d->buf)
        free(d->buf);
    free(d);
}

#else /* !EVO_APP_MODULE - plain libc, unsandboxed elfldr payload */

struct evo_dir {
    DIR *dir;
};

evo_dir_t *evo_opendir(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir)
        return NULL;

    evo_dir_t *d = calloc(1, sizeof *d);
    if (!d) {
        int e = errno;
        closedir(dir);
        errno = e;
        return NULL;
    }
    d->dir = dir;
    return d;
}

struct dirent *evo_readdir(evo_dir_t *d)
{
    return d ? readdir(d->dir) : NULL;
}

void evo_closedir(evo_dir_t *d)
{
    if (!d)
        return;
    if (d->dir)
        closedir(d->dir);
    free(d);
}

#endif /* EVO_APP_MODULE */
