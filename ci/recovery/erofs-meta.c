/* SPDX-License-Identifier: GPL-2.0-or-later
 * Export exact inode metadata without applying ownership or xattrs to the host.
 * File payloads are decoded and checked separately by fsck.erofs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "erofs/config.h"
#include "erofs/dir.h"
#include "erofs/inode.h"
#include "erofs/io.h"
#include "erofs/xattr.h"

static unsigned long count;
struct walk { struct erofs_dir_context ctx; const char *path; unsigned depth; };
static int visit(erofs_nid_t nid, const char *path, unsigned depth);

static void hex(const void *p, size_t n)
{
    const unsigned char *s = p;
    putchar('"');
    while (n--) printf("%02x", *s++);
    putchar('"');
}

static int child(struct erofs_dir_context *ctx)
{
    struct walk *w = (struct walk *)ctx;
    char *path;
    int ret;
    if (ctx->dot_dotdot) return 0;
    if (!ctx->de_namelen || memchr(ctx->dname, '/', ctx->de_namelen) ||
        memchr(ctx->dname, '\0', ctx->de_namelen)) return -EINVAL;
    if (asprintf(&path, "%s%s%.*s", w->path, *w->path ? "/" : "",
                 ctx->de_namelen, ctx->dname) < 0) return -ENOMEM;
    ret = visit(ctx->de_nid, path, w->depth + 1);
    free(path);
    return ret;
}

static int visit(erofs_nid_t nid, const char *path, unsigned depth)
{
    struct erofs_inode inode = { .sbi = &g_sbi, .nid = nid };
    struct walk walk = { .ctx = { .dir = &inode, .cb = child },
                         .path = path, .depth = depth };
    char *names = NULL;
    int ret, len;
    if (depth > 64 || ++count > 1000000) return -ELOOP;
    ret = erofs_read_inode_from_disk(&inode);
    if (ret) return ret;
    if (!S_ISREG(inode.i_mode) && !S_ISDIR(inode.i_mode) && !S_ISLNK(inode.i_mode))
        return -EOPNOTSUPP;
    printf("{\"path_hex\":"); hex(path, strlen(path));
    printf(",\"nid\":%llu,\"mode\":%u,\"uid\":%u,\"gid\":%u,"
           "\"size\":%llu,\"mtime\":%llu,\"mtime_nsec\":%u,\"xattrs\":{",
           (unsigned long long)nid, inode.i_mode, inode.i_uid, inode.i_gid,
           (unsigned long long)inode.i_size, (unsigned long long)inode.i_mtime,
           inode.i_mtime_nsec);
    len = erofs_listxattr(&inode, NULL, 0);
    if (len < 0) return len;
    if (len) {
        names = malloc(len);
        if (!names) return -ENOMEM;
        ret = erofs_listxattr(&inode, names, len);
        if (ret != len) { free(names); return ret < 0 ? ret : -EIO; }
        for (int off = 0, first = 1; off < len;) {
            size_t keylen = strnlen(names + off, len - off);
            ssize_t size;
            void *value;
            if (!keylen || keylen == (size_t)(len - off)) { free(names); return -EINVAL; }
            size = erofs_getxattr(&inode, names + off, NULL, 0);
            if (size < 0 || size > 16 * 1024 * 1024) { free(names); return -EINVAL; }
            value = malloc(size ? size : 1);
            if (!value) { free(names); return -ENOMEM; }
            ret = erofs_getxattr(&inode, names + off, value, size);
            if (ret != size) { free(value); free(names); return -EIO; }
            if (!first) putchar(','); first = 0;
            hex(names + off, keylen); putchar(':'); hex(value, size);
            free(value); off += keylen + 1;
        }
        free(names);
    }
    puts("}}");
    return S_ISDIR(inode.i_mode) ? erofs_iterate_dir(&walk.ctx, true) : 0;
}

int main(int argc, char **argv)
{
    int ret;
    if (argc != 2) { fputs("usage: erofs-meta IMAGE\n", stderr); return 2; }
    erofs_init_configure();
    cfg.c_dbg_lvl = 0;
    ret = erofs_dev_open(&g_sbi, argv[1], O_RDONLY);
    if (!ret) ret = erofs_read_superblock(&g_sbi);
    if (!ret) {
        printf("{\"filesystem\":{\"uuid_hex\":"); hex(g_sbi.uuid, sizeof(g_sbi.uuid));
        printf(",\"label_hex\":"); hex(g_sbi.volume_name, sizeof(g_sbi.volume_name));
        printf(",\"block_size\":%u}}\n", 1U << g_sbi.blkszbits);
        ret = visit(g_sbi.root_nid, "", 0);
    }
    if (ret) fprintf(stderr, "EROFS metadata export failed: %d\n", ret);
    erofs_put_super(&g_sbi);
    erofs_dev_close(&g_sbi);
    erofs_exit_configure();
    return ret || ferror(stdout) ? 1 : 0;
}
