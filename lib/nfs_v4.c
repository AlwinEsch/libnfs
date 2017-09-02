/* -*-  mode:c; tab-width:8; c-basic-offset:8; indent-tabs-mode:nil;  -*- */
/*
   Copyright (C) 2017 by Ronnie Sahlberg <ronniesahlberg@gmail.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation; either version 2.1 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/
/*
 * High level api to nfsv4 filesystems
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef AROS
#include "aros_compat.h"
#endif

#ifdef WIN32
#include "win32_compat.h"
#endif

#ifdef HAVE_INTTYPES_H
#include <inttypes.h>
#else
#define PRIu64 "llu"
#endif

#ifdef HAVE_UTIME_H
#include <utime.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif

#ifdef HAVE_SYS_STATVFS_H
#include <sys/statvfs.h>
#endif

#if defined(__ANDROID__) && !defined(HAVE_SYS_STATVFS_H)
#define statvfs statfs
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#ifdef MAJOR_IN_MKDEV
#include <sys/mkdev.h>
#endif

#ifdef HAVE_SYS_SYSMACROS_H
#include <sys/sysmacros.h>
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "libnfs-zdr.h"
#include "slist.h"
#include "libnfs.h"
#include "libnfs-raw.h"
#include "libnfs-private.h"

#ifndef discard_const
#define discard_const(ptr) ((void *)((intptr_t)(ptr)))
#endif

struct nfs4_cb_data;
typedef int (*op_filler)(struct nfs4_cb_data *data, nfs_argop4 *op);

struct lookup_link_data {
        unsigned int idx;
};

typedef void (*blob_free)(void *);

struct nfs4_blob {
        int       len;
        void     *val;
        blob_free free;
};

/* Function and arguments to append the requested operations we want
 * for the resolved path.
 */
struct lookup_filler {
        op_filler func;
        int max_op;
        int flags;
        void *data;  /* Freed by nfs4_cb_data destructor */

        struct nfs4_blob blob0;
        struct nfs4_blob blob1;
        struct nfs4_blob blob2;
        struct nfs4_blob blob3;
};

struct rw_data {
        uint64_t offset;
        int update_pos;
};

struct nfs4_cb_data {
        struct nfs_context *nfs;
/* Do not follow symlinks for the final component on a lookup.
 * I.e. stat vs lstat
 */
#define LOOKUP_FLAG_NO_FOLLOW 0x0001
        int flags;

        /* Internal callback for open-with-continuation use */
        rpc_cb open_cb;

        /* Application callback and data */
        nfs_cb cb;
        void *private_data;

        /* internal callback */
        rpc_cb continue_cb;

        char *path; /* path to lookup */
        struct lookup_filler filler;

        /* Data we need when resolving a symlink in the path */
        struct lookup_link_data link;

        /* Data we need for updating offset in read/write */
        struct rw_data rw_data;
};

static uint32_t standard_attributes[2] = {
        (1 << FATTR4_TYPE |
         1 << FATTR4_SIZE |
         1 << FATTR4_FILEID),
        (1 << (FATTR4_MODE - 32) |
         1 << (FATTR4_NUMLINKS - 32) |
         1 << (FATTR4_OWNER - 32) |
         1 << (FATTR4_OWNER_GROUP - 32) |
         1 << (FATTR4_SPACE_USED - 32) |
         1 << (FATTR4_TIME_ACCESS - 32) |
         1 << (FATTR4_TIME_METADATA - 32) |
         1 << (FATTR4_TIME_MODIFY - 32))
};
static uint32_t statvfs_attributes[2] = {
        (1 << FATTR4_FSID |
         1 << FATTR4_FILES_AVAIL |
         1 << FATTR4_FILES_FREE |
         1 << FATTR4_FILES_TOTAL |
         1 << FATTR4_MAXNAME),
        (1 << (FATTR4_SPACE_AVAIL - 32) |
         1 << (FATTR4_SPACE_FREE - 32) |
         1 << (FATTR4_SPACE_TOTAL - 32))
};

static int
nfs4_open_async_internal(struct nfs_context *nfs, struct nfs4_cb_data *data,
                         int flags, int mode);

/* Caller will free the returned path. */
static char *
nfs4_resolve_path(struct nfs_context *nfs, const char *path)
{
        char *new_path = NULL;

        /* Absolute paths we just use as is.
         * Relateive paths have cwd prepended to them and then become
         * absolute paths too.
         */
        if (path[0] == '/') {
                new_path = strdup(path);
        } else {
                new_path = malloc(strlen(path) + strlen(nfs->cwd) + 2);
                if (new_path != NULL) {
                        sprintf(new_path, "%s/%s", nfs->cwd, path);
                }
        }
        if (new_path == NULL) {
                nfs_set_error(nfs, "Out of memory: failed to "
                              "allocate path string");
                return NULL;
        }

        if (nfs_normalize_path(nfs, new_path)) {
                nfs_set_error(nfs, "Failed to normalize real path. %s",
                              nfs_get_error(nfs));
                free(new_path);
                return NULL;
        }

        return new_path;
}

static void
free_nfs4_cb_data(struct nfs4_cb_data *data)
{
        free(data->path);
        free(data->filler.data);
        if (data->filler.blob0.val && data->filler.blob0.free) {
                data->filler.blob0.free(data->filler.blob0.val);
        }
        if (data->filler.blob1.val && data->filler.blob1.free) {
                data->filler.blob1.free(data->filler.blob1.val);
        }
        if (data->filler.blob2.val && data->filler.blob2.free) {
                data->filler.blob2.free(data->filler.blob2.val);
        }
        if (data->filler.blob3.val && data->filler.blob3.free) {
                data->filler.blob3.free(data->filler.blob3.val);
        }
        free(data);
}

static struct nfs4_cb_data *
init_cb_data_full_path(struct nfs_context *nfs, const char *path)
{
        struct nfs4_cb_data *data;

        data = malloc(sizeof(*data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "cb data");
                return NULL;
        }
        memset(data, 0, sizeof(*data));

        data->nfs          = nfs;
        data->path = nfs4_resolve_path(nfs, path);
        if (data->path == NULL) {
                free_nfs4_cb_data(data);
                return NULL;
        }

        return data;
}

static void
data_split_path(struct nfs4_cb_data *data)
{
        char *path;
        path = strrchr(data->path, '/');
        if (path == data->path) {
                char *ptr;

                for (ptr = data->path; *ptr; ptr++) {
                        *ptr = *(ptr + 1);
                }
                /* No path to lookup */
                data->filler.data = data->path;
                data->path = strdup("/");
        } else {
                *path++ = 0;
                data->filler.data = strdup(path);
        }
}

static struct nfs4_cb_data *
init_cb_data_split_path(struct nfs_context *nfs, const char *orig_path)
{
        struct nfs4_cb_data *data;

        data = init_cb_data_full_path(nfs, orig_path);
        if (data == NULL) {
                return NULL;
        }

        data_split_path(data);
        return data;
}

static int
check_nfs4_error(struct nfs_context *nfs, int status,
                 struct nfs4_cb_data *data, void *command_data,
                 char *op_name)
{
        COMPOUND4res *res = command_data;

        if (status == RPC_STATUS_ERROR) {
                data->cb(-EFAULT, nfs, res, data->private_data);
                free_nfs4_cb_data(data);
                return 1;
        }
        if (status == RPC_STATUS_CANCEL) {
                data->cb(-EINTR, nfs, "Command was cancelled",
                         data->private_data);
                free_nfs4_cb_data(data);
                return 1;
        }
        if (status == RPC_STATUS_TIMEOUT) {
                data->cb(-EINTR, nfs, "Command timed out",
                         data->private_data);
                free_nfs4_cb_data(data);
                return 1;
        }
        if (res && res->status != NFS4_OK) {
                nfs_set_error(nfs, "NFS4: %s (path %s) failed with "
                              "%s(%d)", op_name,
                              data->path,
                              nfsstat4_to_str(res->status),
                              nfsstat4_to_errno(res->status));
                data->cb(nfsstat3_to_errno(res->status), nfs,
                         nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return 1;
        }

        return 0;
}

static int
nfs4_find_op(struct nfs_context *nfs, struct nfs4_cb_data *data,
             COMPOUND4res *res, int op, const char *op_name)
{
        int i;

        for (i = 0; i < (int)res->resarray.resarray_len; i++) {
                if (res->resarray.resarray_val[i].resop == op) {
                        break;
                }
        }
        if (i == res->resarray.resarray_len) {
                nfs_set_error(nfs, "No %s result.", op_name);
                data->cb(-EINVAL, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return -1;
        }

        return i;
}

static uint64_t
nfs_hton64(uint64_t val)
{
        int i;
        uint64_t res;
        unsigned char *ptr = (void *)&res;

        for (i = 0; i < 8; i++) {
                ptr[7 - i] = val & 0xff;
                val >>= 8;
        }
        return res;
}

static uint64_t
nfs_ntoh64(uint64_t val)
{
        int i;
        uint64_t res;
        unsigned char *ptr = (void *)&res;

        for (i = 0; i < 8; i++) {
                ptr[7 - i] = val & 0xff;
                val >>= 8;
        }
        return res;
}

static uint64_t
nfs_pntoh64(const uint32_t *buf)
{
        uint64_t val;

        val   = ntohl(*(uint32_t *)(void *)buf++);
        val <<= 32;
        val  |= ntohl(*(uint32_t *)(void *)buf);

        return val;
}

#define CHECK_GETATTR_BUF_SPACE(len, size)                              \
    if (len < size) {                                                   \
        nfs_set_error(nfs, "Not enough data in fattr4");                \
        return -1;                                                      \
    }

static int
nfs_parse_attributes(struct nfs_context *nfs, struct nfs4_cb_data *data,
                     struct nfs_stat_64 *st, const char *buf, int len)
{
        int type, slen, pad;

        /* Type */
        CHECK_GETATTR_BUF_SPACE(len, 4);
        type = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        /* Size */
        CHECK_GETATTR_BUF_SPACE(len, 8);
        st->nfs_size = nfs_pntoh64((uint32_t *)(void *)buf);
        buf += 8;
        len -= 8;
        /* Inode */
        CHECK_GETATTR_BUF_SPACE(len, 8);
        st->nfs_ino = nfs_pntoh64((uint32_t *)(void *)buf);
        buf += 8;
        len -= 8;
        /* Mode */
        CHECK_GETATTR_BUF_SPACE(len, 4);
        st->nfs_mode = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        switch (type) {
        case NF4REG:
                st->nfs_mode |= S_IFREG;
                break;
        case NF4DIR:
                st->nfs_mode |= S_IFDIR;
                break;
        case NF4BLK:
                st->nfs_mode |= S_IFBLK;
                break;
        case NF4CHR:
                st->nfs_mode |= S_IFCHR;
                break;
        case NF4LNK:
                st->nfs_mode |= S_IFLNK;
                break;
        case NF4SOCK:
                st->nfs_mode |= S_IFSOCK;
                break;
        case NF4FIFO:
                st->nfs_mode |= S_IFIFO;
                break;
        default:
                break;
        }
        /* Num Links */
        CHECK_GETATTR_BUF_SPACE(len, 4);
        st->nfs_nlink = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        /* Owner */
        CHECK_GETATTR_BUF_SPACE(len, 4);
        slen = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        pad = (4 - (slen & 0x03)) & 0x03;
        CHECK_GETATTR_BUF_SPACE(len, slen);
        while (slen) {
                if (isdigit(*buf)) {
                        st->nfs_uid *= 10;
                        st->nfs_uid += *buf - '0';
                } else {
                        nfs_set_error(nfs, "Bad digit in fattr3 uid");
                        return -1;
                }
                buf++;
                slen--;
        }
        CHECK_GETATTR_BUF_SPACE(len, pad);
        buf += pad;
        len -= pad;
        /* Group */
        CHECK_GETATTR_BUF_SPACE(len, 4);
        slen = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        pad = (4 - (slen & 0x03)) & 0x03;
        CHECK_GETATTR_BUF_SPACE(len, slen);
        while (slen) {
                if (isdigit(*buf)) {
                        st->nfs_gid *= 10;
                        st->nfs_gid += *buf - '0';
                } else {
                        nfs_set_error(nfs, "Bad digit in fattr3 gid");
                        return -1;
                }
                buf++;
                slen--;
        }
        CHECK_GETATTR_BUF_SPACE(len, pad);
        buf += pad;
        len -= pad;
        /* Space Used */
        CHECK_GETATTR_BUF_SPACE(len, 8);
        st->nfs_used = nfs_pntoh64((uint32_t *)(void *)buf);
        buf += 8;
        len -= 8;
        /* ATime */
        CHECK_GETATTR_BUF_SPACE(len, 12);
        st->nfs_atime = nfs_pntoh64((uint32_t *)(void *)buf);
        buf += 8;
        len -= 8;
        st->nfs_atime_nsec = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        /* CTime */
        CHECK_GETATTR_BUF_SPACE(len, 12);
        st->nfs_ctime = nfs_pntoh64((uint32_t *)(void *)buf);
        buf += 8;
        len -= 8;
        st->nfs_ctime_nsec = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;
        /* MTime */
        CHECK_GETATTR_BUF_SPACE(len, 12);
        st->nfs_mtime = nfs_pntoh64((uint32_t *)(void *)buf);
        buf += 8;
        len -= 8;
        st->nfs_mtime_nsec = ntohl(*(uint32_t *)(void *)buf);
        buf += 4;
        len -= 4;

        st->nfs_blksize = NFS_BLKSIZE;
        st->nfs_blocks  = (st->nfs_used + NFS_BLKSIZE -1) / NFS_BLKSIZE;

        return 0;
}

static int
nfs4_num_path_components(struct nfs_context *nfs, const char *path)
{
        int i;

        for (i = 0; (path = strchr(path, '/')); path++, i++)
                ;

        return i;
}

static int
nfs4_op_create(struct nfs_context *nfs, nfs_argop4 *op, const char *name,
               nfs_ftype4 type, struct nfs4_blob *attrmask,
               struct nfs4_blob *attr_vals, const char *linkdata, int dev)
{
        CREATE4args *cargs;

        op[0].argop = OP_CREATE;
        cargs = &op[0].nfs_argop4_u.opcreate;
        memset(cargs, 0, sizeof(*cargs));
        cargs->objtype.type = type;
        cargs->objname.utf8string_len = strlen(name);
        cargs->objname.utf8string_val = discard_const(name);
        if (attrmask) {
                cargs->createattrs.attrmask.bitmap4_len = attrmask->len;
                cargs->createattrs.attrmask.bitmap4_val = attrmask->val;
        }
        if (attr_vals) {
                cargs->createattrs.attr_vals.attrlist4_len = attr_vals->len;
                cargs->createattrs.attr_vals.attrlist4_val = attr_vals->val;
        }
        if (linkdata) {
                cargs->objtype.createtype4_u.linkdata.utf8string_len =
                        strlen(linkdata);
                cargs->objtype.createtype4_u.linkdata.utf8string_val =
                        discard_const(linkdata);
        }
        switch (type) {
        case NF4CHR:
                cargs->objtype.type = NF4CHR;
                cargs->objtype.createtype4_u.devdata.specdata1 = major(dev);
                cargs->objtype.createtype4_u.devdata.specdata2 = minor(dev);
                break;
        case NF4BLK:
                cargs->objtype.type = NF4BLK;
                cargs->objtype.createtype4_u.devdata.specdata1 = major(dev);
                cargs->objtype.createtype4_u.devdata.specdata2 = minor(dev);
                break;
        default:
                ;
        }
        return 1;
}

static int
nfs4_op_commit(struct nfs_context *nfs, nfs_argop4 *op)
{
        COMMIT4args *coargs;

        op[0].argop = OP_COMMIT;
        coargs = &op[0].nfs_argop4_u.opcommit;
        coargs->offset = 0;
        coargs->count = 0;

        return 1;
}

static int
nfs4_op_close(struct nfs_context *nfs, nfs_argop4 *op, struct nfsfh *fh)
{
        CLOSE4args *clargs;
        int i = 0;

        if (fh->is_dirty) {
                i += nfs4_op_commit(nfs, &op[i]);
        }

        op[i].argop = OP_CLOSE;
        clargs = &op[i++].nfs_argop4_u.opclose;
        clargs->seqid = nfs->seqid;
        clargs->open_stateid.seqid = fh->stateid.seqid;
        memcpy(clargs->open_stateid.other, fh->stateid.other, 12);

        return i;
}

static int
nfs4_op_access(struct nfs_context *nfs, nfs_argop4 *op, uint32_t access_mask)
{
        ACCESS4args *aargs;

        op[0].argop = OP_ACCESS;
        aargs = &op[0].nfs_argop4_u.opaccess;
        memset(aargs, 0, sizeof(*aargs));
        aargs->access = access_mask;

        return 1;
}

static int
nfs4_op_setclientid(struct nfs_context *nfs, nfs_argop4 *op, verifier4 verifier,
                    const char *client_name)
{
        SETCLIENTID4args *scidargs;

        op[0].argop = OP_SETCLIENTID;
        scidargs = &op[0].nfs_argop4_u.opsetclientid;
        memcpy(scidargs->client.verifier, verifier, sizeof(verifier4));
        scidargs->client.id.id_len = strlen(client_name);
        scidargs->client.id.id_val = discard_const(client_name);
        /* TODO: Decide what we should do here. As long as we only
         * expose a single FD to the application we will not be able to
         * do NFSv4 callbacks easily.
         * Just give it garbage for now until we figure out how we should
         * solve this. Until then we will just have to avoid doing things
         * that require a callback.
         * ( Clients (i.e. Linux) ignore this anyway and just call back to
         *   the originating address and program anyway. )
         */
        scidargs->callback.cb_program = 0; /* NFS4_CALLBACK */
        scidargs->callback.cb_location.r_netid = "tcp";
        scidargs->callback.cb_location.r_addr = "0.0.0.0.0.0";
        scidargs->callback_ident = 0x00000001;

        return 1;
}

static int
nfs4_op_open_confirm(struct nfs_context *nfs, nfs_argop4 *op, uint32_t seqid,
                     struct nfsfh *fh)
{
        OPEN_CONFIRM4args *ocargs;

        op[0].argop = OP_OPEN_CONFIRM;
        ocargs = &op[0].nfs_argop4_u.opopen_confirm;
        ocargs->open_stateid.seqid = fh->stateid.seqid;
        memcpy(&ocargs->open_stateid.other, fh->stateid.other, 12);
        ocargs->seqid = seqid;

        return 1;
}

static int
nfs4_op_truncate(struct nfs_context *nfs, nfs_argop4 *op, struct nfsfh *fh,
                 void *sabuf)
{
        SETATTR4args *saargs;
        static uint32_t mask[2] = {1 << (FATTR4_SIZE),
                                   1 << (FATTR4_TIME_MODIFY_SET - 32)};

        op[0].argop = OP_SETATTR;
        saargs = &op[0].nfs_argop4_u.opsetattr;
        saargs->stateid.seqid = fh->stateid.seqid;
        memcpy(saargs->stateid.other, fh->stateid.other, 12);

        saargs->obj_attributes.attrmask.bitmap4_len = 2;
        saargs->obj_attributes.attrmask.bitmap4_val = mask;

        saargs->obj_attributes.attr_vals.attrlist4_len = 12;
        saargs->obj_attributes.attr_vals.attrlist4_val = sabuf;

        return 1;
}

static int
nfs4_op_chmod(struct nfs_context *nfs, nfs_argop4 *op, struct nfsfh *fh,
              void *sabuf)
{
        SETATTR4args *saargs;
        static uint32_t mask[2] = {0,
                                   1 << (FATTR4_MODE - 32)};

        op[0].argop = OP_SETATTR;
        saargs = &op[0].nfs_argop4_u.opsetattr;
        saargs->stateid.seqid = fh->stateid.seqid;
        memcpy(saargs->stateid.other, fh->stateid.other, 12);

        saargs->obj_attributes.attrmask.bitmap4_len = 2;
        saargs->obj_attributes.attrmask.bitmap4_val = mask;

        saargs->obj_attributes.attr_vals.attrlist4_len = 4;
        saargs->obj_attributes.attr_vals.attrlist4_val = sabuf;

        return 1;
}

static int
nfs4_op_chown(struct nfs_context *nfs, nfs_argop4 *op, struct nfsfh *fh,
              void *sabuf, int len)
{
        SETATTR4args *saargs;
        static uint32_t mask[2] = {0,
                                   1 << (FATTR4_OWNER - 32) |
                                   1 << (FATTR4_OWNER_GROUP - 32)};

        op[0].argop = OP_SETATTR;
        saargs = &op[0].nfs_argop4_u.opsetattr;
        saargs->stateid.seqid = fh->stateid.seqid;
        memcpy(saargs->stateid.other, fh->stateid.other, 12);

        saargs->obj_attributes.attrmask.bitmap4_len = 2;
        saargs->obj_attributes.attrmask.bitmap4_val = mask;

        saargs->obj_attributes.attr_vals.attrlist4_len = len;
        saargs->obj_attributes.attr_vals.attrlist4_val = sabuf;

        return 1;
}

static int
nfs4_op_utimes(struct nfs_context *nfs, nfs_argop4 *op, struct nfsfh *fh,
               void *sabuf, int len)
{
        SETATTR4args *saargs;
        static uint32_t mask[2] = {0,
                                   1 << (FATTR4_TIME_ACCESS_SET - 32) |
                                   1 << (FATTR4_TIME_MODIFY_SET - 32)};

        op[0].argop = OP_SETATTR;
        saargs = &op[0].nfs_argop4_u.opsetattr;
        saargs->stateid.seqid = fh->stateid.seqid;
        memcpy(saargs->stateid.other, fh->stateid.other, 12);

        saargs->obj_attributes.attrmask.bitmap4_len = 2;
        saargs->obj_attributes.attrmask.bitmap4_val = mask;

        saargs->obj_attributes.attr_vals.attrlist4_len = len;
        saargs->obj_attributes.attr_vals.attrlist4_val = sabuf;

        return 1;
}

static int
nfs4_op_readdir(struct nfs_context *nfs, nfs_argop4 *op, uint64_t cookie)
{
        READDIR4args *rdargs;

        op[0].argop = OP_READDIR;
        rdargs = &op[0].nfs_argop4_u.opreaddir;
        memset(rdargs, 0, sizeof(*rdargs));

        rdargs->cookie = cookie;
        rdargs->dircount = 8192;
        rdargs->maxcount = 8192;
        rdargs->attr_request.bitmap4_len = 2;
        rdargs->attr_request.bitmap4_val = standard_attributes;

        return 1;
}

static int
nfs4_op_rename(struct nfs_context *nfs, nfs_argop4 *op, const char *oldname,
               const char *newname)
{
        RENAME4args *rargs;

        op[0].argop = OP_RENAME;
        rargs = &op[0].nfs_argop4_u.oprename;
        memset(rargs, 0, sizeof(*rargs));
        rargs->oldname.utf8string_len = strlen(oldname);
        rargs->oldname.utf8string_val = discard_const(oldname);
        rargs->newname.utf8string_len = strlen(newname);
        rargs->newname.utf8string_val = discard_const(newname);

        return 1;
}

static int
nfs4_op_read(struct nfs_context *nfs, nfs_argop4 *op, struct nfsfh *fh,
             uint64_t offset, size_t count)
{
        READ4args *rargs;

        op[0].argop = OP_READ;
        rargs = &op[0].nfs_argop4_u.opread;
        rargs->stateid.seqid = fh->stateid.seqid;
        memcpy(rargs->stateid.other, fh->stateid.other, 12);
        rargs->offset = offset;
        rargs->count = count;

        return 1;
}

static int
nfs4_op_write(struct nfs_context *nfs, nfs_argop4 *op, struct nfsfh *fh,
              uint64_t offset, size_t count, const char *buf)
{
        WRITE4args *wargs;

        op[0].argop = OP_WRITE;
        wargs = &op[0].nfs_argop4_u.opwrite;
        wargs->stateid.seqid = fh->stateid.seqid;
        memcpy(wargs->stateid.other, fh->stateid.other, 12);
        wargs->offset = offset;
        if (fh->is_sync) {
                wargs->stable = DATA_SYNC4;
        } else {
                wargs->stable = UNSTABLE4;
                fh->is_dirty = 1;
        }
        wargs->data.data_len = count;
        wargs->data.data_val = discard_const(buf);

        return 1;
}

static int
nfs4_op_getfh(struct nfs_context *nfs, nfs_argop4 *op)
{
        op[0].argop = OP_GETFH;

        return 1;
}

static int
nfs4_op_savefh(struct nfs_context *nfs, nfs_argop4 *op)
{
        op[0].argop = OP_SAVEFH;

        return 1;
}

static int
nfs4_op_link(struct nfs_context *nfs, nfs_argop4 *op, const char *newname)
{
        LINK4args *largs;

        op[0].argop = OP_LINK;
        largs = &op[0].nfs_argop4_u.oplink;
        memset(largs, 0, sizeof(*largs));
        largs->newname.utf8string_len = strlen(newname);
        largs->newname.utf8string_val = discard_const(newname);

        return 1;
}

static int
nfs4_op_putfh(struct nfs_context *nfs, nfs_argop4 *op, struct nfsfh *nfsfh)
{
        PUTFH4args *pfargs;
        op[0].argop = OP_PUTFH;

        pfargs = &op[0].nfs_argop4_u.opputfh;
        pfargs->object.nfs_fh4_len = nfsfh->fh.len;
        pfargs->object.nfs_fh4_val = nfsfh->fh.val;

        return 1;
}

static int
nfs4_op_lookup(struct nfs_context *nfs, nfs_argop4 *op, const char *path)
{
        LOOKUP4args *largs;

        op[0].argop = OP_LOOKUP;
        largs = &op[0].nfs_argop4_u.oplookup;
        largs->objname.utf8string_len = strlen(path);
        largs->objname.utf8string_val = discard_const(path);

        return 1;
}

static int
nfs4_op_setclientid_confirm(struct nfs_context *nfs, struct nfs_argop4 *op,
                            uint64_t clientid, verifier4 verifier)
{
        SETCLIENTID_CONFIRM4args *scidcargs;

        op[0].argop = OP_SETCLIENTID_CONFIRM;
        scidcargs = &op[0].nfs_argop4_u.opsetclientid_confirm;
        scidcargs->clientid = clientid;
        memcpy(scidcargs->setclientid_confirm, verifier, NFS4_VERIFIER_SIZE);

        return 1;
}

static int
nfs4_op_putrootfh(struct nfs_context *nfs, nfs_argop4 *op)
{
        op[0].argop = OP_PUTROOTFH;

        return 1;
}

static int
nfs4_op_readlink(struct nfs_context *nfs, nfs_argop4 *op)
{
        op[0].argop = OP_READLINK;

        return 1;
}

static int
nfs4_op_remove(struct nfs_context *nfs, nfs_argop4 *op, const char *name)
{
        REMOVE4args *rmargs;

        op[0].argop = OP_REMOVE;
        rmargs = &op[0].nfs_argop4_u.opremove;
        memset(rmargs, 0, sizeof(*rmargs));
        rmargs->target.utf8string_len = strlen(name);
        rmargs->target.utf8string_val = discard_const(name);

        return 1;
}

static int
nfs4_op_getattr(struct nfs_context *nfs, nfs_argop4 *op,
                uint32_t *attributes, int count)
{
        GETATTR4args *gaargs;

        op[0].argop = OP_GETATTR;
        gaargs = &op[0].nfs_argop4_u.opgetattr;
        memset(gaargs, 0, sizeof(*gaargs));

        gaargs->attr_request.bitmap4_val = attributes;
        gaargs->attr_request.bitmap4_len = count;

        return 1;
}

/*
 * Allocate op and populate the path components.
 * Will mutate path.
 *
 * Returns:
 *     -1 : On error.
 *  <idx> : On success. Idx represents the next free index in op.
 *          Caller must free op.
 */
static int
nfs4_allocate_op(struct nfs_context *nfs, nfs_argop4 **op,
                 char *path, int num_extra)
{
        char *ptr;
        int i, count;

        *op = NULL;

        count = nfs4_num_path_components(nfs, path);

        *op = malloc(sizeof(**op) * (2 + 2 * count + num_extra));
        if (*op == NULL) {
                nfs_set_error(nfs, "Failed to allocate op array");
                return -1;
        }

        i = 0;
        if (nfs->rootfh.len) {
                struct nfsfh fh;

                fh.fh.len = nfs->rootfh.len;
                fh.fh.val = nfs->rootfh.val;
                i += nfs4_op_putfh(nfs, &(*op)[i], &fh);
        } else {
                i += nfs4_op_putrootfh(nfs, &(*op)[i]);
        }

        ptr = &path[1];
        while (ptr && *ptr != 0) {
                char *tmp;

                tmp = strchr(ptr, '/');
                if (tmp) {
                        *tmp = 0;
                        tmp = tmp + 1;
                }
                i += nfs4_op_lookup(nfs, &(*op)[i], ptr); 

                ptr = tmp;
        }                

        i += nfs4_op_getattr(nfs, &(*op)[i], standard_attributes, 2);

        return i;
}

static int
nfs4_lookup_path_async(struct nfs_context *nfs,
                       struct nfs4_cb_data *data,
                       rpc_cb cb);

static void
nfs4_lookup_path_2_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        READLINK4res *rlres = NULL;
        char *path, *tmp, *end;
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "READLINK")) {
                return;
        }

        path = strdup(data->path);
        if (path == NULL) {
                nfs_set_error(nfs, "Out of memory duplicating path.");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs),
                         data->private_data);
                free_nfs4_cb_data(data);
                return;
        }

        tmp = &path[0];
        while (data->link.idx-- > 1) {
                tmp = strchr(tmp + 1, '/');
        }
        *tmp++ = 0;
        end = strchr(tmp, '/');
        if (end == NULL) {
                /* Symlink was the last component. */
                end = "";
        } else {
                *end++ = 0;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_READLINK, "READLINK")) < 0) {
                free(path);
                return;
        }
        rlres = &res->resarray.resarray_val[i].nfs_resop4_u.opreadlink;
        
        tmp = malloc(strlen(data->path) + 3 + strlen(rlres->READLINK4res_u.resok4.link.utf8string_val));
        if (tmp == NULL) {
                nfs_set_error(nfs, "Out of memory duplicating path.");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs),
                         data->private_data);
                free_nfs4_cb_data(data);
                free(path);
                return;
        }

        sprintf(tmp, "%s/%s/%s", path, rlres->READLINK4res_u.resok4.link.utf8string_val, end);
        free(path);
        free(data->path);
        data->path = tmp;

        if (nfs4_lookup_path_async(nfs, data, data->continue_cb) < 0) {
                data->cb(-ENOMEM, nfs, res, data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
}

static int
nfs4_open_readlink(struct rpc_context *rpc, COMPOUND4res *res,
                   struct nfs4_cb_data *data);

static void
nfs4_lookup_path_1_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4args args;
        nfs_argop4 *op;
        COMPOUND4res *res = command_data;
        int i;
        int resolve_link = 0;
        char *path, *tmp;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (status == RPC_STATUS_ERROR) {
                data->cb(-EFAULT, nfs, res, data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        if (status == RPC_STATUS_CANCEL) {
                data->cb(-EINTR, nfs, "Command was cancelled",
                         data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        if (status == RPC_STATUS_TIMEOUT) {
                data->cb(-EINTR, nfs, "Command timed out",
                         data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        if (res->status != NFS4_OK &&
            res->status != NFS4ERR_SYMLINK) {
                nfs_set_error(nfs, "NFS4: (path %s) failed with "
                              "%s(%d)",
                              data->path,
                              nfsstat4_to_str(res->status),
                              nfsstat4_to_errno(res->status));
                data->cb(nfsstat3_to_errno(res->status), nfs,
                         nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }

        for (i = 0; i < (int)res->resarray.resarray_len; i++) {
                if (res->resarray.resarray_val[i].resop == OP_GETATTR) {
                        GETATTR4resok *garesok;
                        struct nfs_stat_64 st;

                        garesok = &res->resarray.resarray_val[i].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;

                        memset(&st, 0, sizeof(st));
                        if (nfs_parse_attributes(nfs, data, &st,
                                 garesok->obj_attributes.attr_vals.attrlist4_val,
                                 garesok->obj_attributes.attr_vals.attrlist4_len) < 0) {
                                data->cb(-EINVAL, nfs, nfs_get_error(nfs), data->private_data);
                                free_nfs4_cb_data(data);
                                return;
                        }
                        if ((st.nfs_mode & S_IFMT) == S_IFLNK) {
                                /* The final component of the path was a
                                 * symlink so we may need to resolve it.
                                 */
                                resolve_link = 1;
                        }
                }
        }

        /* Open/create is special since the final component for the file
         * object is sent as part of the OP_OPEN command. So even if the
         * directory path is all good and resolved, we still need to check
         * the attributes for the final component and resolve it if it too
         * is a symlink.
         */
        if (!resolve_link) {
                if (nfs4_open_readlink(rpc, res, data) < 0) {
                        /* It was a symlink and we have started trying to
                         * resolve it. Nothing more to do here.
                         */
                        return;
                }
        }

        if (data->flags & LOOKUP_FLAG_NO_FOLLOW) {
                /* Do not resolve the final component of the path
                 * if it is a symlink.
                 */
                resolve_link = 0;
        }

        /* Everything is good so we can just pass it on to the next
         * phase.
         */
        if (res->status == NFS4_OK && !resolve_link) {
                data->continue_cb(rpc, NFS4_OK, res, data);
                return;
        }

        /* Find the lookup that failed and the associated fh */
        data->link.idx = 0;
        for (i = 0; i < (int)res->resarray.resarray_len; i++) {
                if (res->resarray.resarray_val[i].resop == OP_LOOKUP) {
                        if (res->resarray.resarray_val[i].nfs_resop4_u.oplookup.status == NFS4ERR_SYMLINK) {
                                break;
                        }
                        data->link.idx++;
                }
        }

        if (!resolve_link && i == res->resarray.resarray_len) {
                nfs_set_error(nfs, "Symlink not found during lookup.");
                data->cb(-EFAULT, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }

        /* Build a new path that strips of everything after the symlink. */
        path = strdup(data->path);
        if (path == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to duplicate "
                              "path.");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs),
                         data->private_data);
                free_nfs4_cb_data(data);
                return;
        }

        /* The symlink is not the last component, so find the '/' before
         * the symlink and zero it out.
         */
        if (!resolve_link) {
                tmp = path;
                for (i = 0; i < data->link.idx; i++) {
                        tmp = strchr(tmp + 1, '/');
                }
                *tmp = 0;
        }

        /* We need to resolve the symlink */
        if ((i = nfs4_allocate_op(nfs, &op, path, 1)) < 0) {
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                free(path);
                return;
        }

        /* Append a READLINK command */
        i += nfs4_op_readlink(nfs, &op[i]);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_lookup_path_2_cb, &args,
                                    data) != 0) {
                nfs_set_error(nfs, "Failed to queue READLINK command. %s",
                              nfs_get_error(nfs));
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                free(path);
                return;
        }
        free(path);
}

static int
nfs4_lookup_path_async(struct nfs_context *nfs,
                       struct nfs4_cb_data *data,
                       rpc_cb cb)
{
        COMPOUND4args args;
        nfs_argop4 *op;
        char *path;
        int i, num_op;

        path = nfs4_resolve_path(nfs, data->path);
        if (path == NULL) {
                return -1;
        }
        free(data->path);
        data->path = path;

        path = strdup(path);
        if (path == NULL) {
                return -1;
        }

        if ((i = nfs4_allocate_op(nfs, &op, path, data->filler.max_op)) < 0) {
                free(path);
                return -1;
        }

        num_op = data->filler.func(data, &op[i]);
        data->continue_cb = cb;

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i + num_op;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_lookup_path_1_cb, &args,
                                    data) != 0) {
                nfs_set_error(nfs, "Failed to queue LOOKUP command. %s",
                              nfs_get_error(nfs));
                free(path);
                free(op);
                return -1;
        }

        free(path);
        free(op);
        return 0;
}

static int
nfs4_populate_getfh(struct nfs4_cb_data *data, nfs_argop4 *op)
{
        return nfs4_op_getfh(data->nfs, op);
}

static int
nfs4_populate_getattr(struct nfs4_cb_data *data, nfs_argop4 *op)
{
        return nfs4_op_getfh(data->nfs, op);
}

static int
nfs4_populate_access(struct nfs4_cb_data *data, nfs_argop4 *op)
{
        uint32_t mode;

        memcpy(&mode, data->filler.blob3.val, sizeof(uint32_t));

        return nfs4_op_access(data->nfs, op, mode);
}

static void
nfs4_mount_4_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        GETFH4resok *gfhresok;
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "GETFH")) {
                return;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_GETFH, "GETFH")) < 0) {
                return;
        }
        gfhresok = &res->resarray.resarray_val[i].nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

        nfs->rootfh.len = gfhresok->object.nfs_fh4_len;
        nfs->rootfh.val = malloc(nfs->rootfh.len);
        if (nfs->rootfh.val == NULL) {
                nfs_set_error(nfs, "%s: %s", __FUNCTION__, nfs_get_error(nfs));
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        memcpy(nfs->rootfh.val,
               gfhresok->object.nfs_fh4_val,
               nfs->rootfh.len);


        data->cb(0, nfs, NULL, data->private_data);
        free_nfs4_cb_data(data);
}

static void
nfs4_mount_3_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "SETCLIENTID_CONFIRM")) {
                return;
        }

        data->filler.func = nfs4_populate_getfh;
        data->filler.max_op = 1;
        data->filler.data = malloc(2 * sizeof(uint32_t));
        if (data->filler.data == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "data structure.");
                data->cb(-ENOMEM, nfs, res, data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        memset(data->filler.data, 0, 2 * sizeof(uint32_t));


        if (nfs4_lookup_path_async(nfs, data, nfs4_mount_4_cb) < 0) {
                data->cb(-ENOMEM, nfs, res, data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
}

static void
nfs4_mount_2_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        COMPOUND4args args;
        nfs_argop4 op[1];
        SETCLIENTID4resok *scidresok;
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "SETCLIENTID")) {
                return;
        }

        scidresok = &res->resarray.resarray_val[0].nfs_resop4_u.opsetclientid.SETCLIENTID4res_u.resok4;
        nfs->clientid = scidresok->clientid;
        memcpy(nfs->setclientid_confirm,
               scidresok->setclientid_confirm,
               NFS4_VERIFIER_SIZE);

        memset(op, 0, sizeof(op));

        i = nfs4_op_setclientid_confirm(nfs, &op[0], nfs->clientid,
                                        nfs->setclientid_confirm);
               
        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(rpc, nfs4_mount_3_cb, &args,
                                    private_data) != 0) {
                nfs_set_error(nfs, "Failed to queue SETCLIENTID_CONFIRM. %s",
                              nfs_get_error(nfs));
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
}

static void
nfs4_mount_1_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4args args;
        nfs_argop4 op[1];
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, NULL, "CONNECT")) {
                return;
        }

        memset(op, 0, sizeof(op));

        i = nfs4_op_setclientid(nfs, &op[0], nfs->verifier, nfs->client_name);
        
        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(rpc, nfs4_mount_2_cb, &args, data) != 0) {
                nfs_set_error(nfs, "Failed to queue SETCLIENTID. %s",
                              nfs_get_error(nfs));
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
}

int
nfs4_mount_async(struct nfs_context *nfs, const char *server,
                 const char *export, nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;
        char *new_server, *new_export;

        new_server = strdup(server);
        free(nfs->server);
        nfs->server = new_server;

        new_export = strdup(export);
        if (nfs_normalize_path(nfs, new_export)) {
                nfs_set_error(nfs, "Bad export path. %s",
                              nfs_get_error(nfs));
                free(new_export);
                return -1;
        }
        free(nfs->export);
        nfs->export = new_export;


        data = malloc(sizeof(*data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "memory for nfs mount data");
                return -1;
        }
        memset(data, 0, sizeof(*data));

        data->nfs          = nfs;
        data->cb           = cb;
        data->private_data = private_data;
        data->path         = strdup(new_export);

        if (rpc_connect_program_async(nfs->rpc, server,
                                      NFS4_PROGRAM, NFS_V4,
                                      nfs4_mount_1_cb, data) != 0) {
                nfs_set_error(nfs, "Failed to start connection");
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_chdir_1_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "CHDIR")) {
                return;
        }

        /* Ok, all good. Lets steal the path string. */
        free(nfs->cwd);
        nfs->cwd = data->path;
        data->path = NULL;

        data->cb(0, nfs, NULL, data->private_data);
        free_nfs4_cb_data(data);
}

int nfs4_chdir_async(struct nfs_context *nfs, const char *path,
                     nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;

        data = init_cb_data_full_path(nfs, path);
        if (data == NULL) {
                return -1;
        }

        data->cb            = cb;
        data->private_data  = private_data;
        data->filler.func   = nfs4_populate_getattr;
        data->filler.max_op = 1;
        data->filler.data   = malloc(2 * sizeof(uint32_t));
        if (data->filler.data == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "data structure.");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return -1;
        }
        memset(data->filler.data, 0, 2 * sizeof(uint32_t));

        if (nfs4_lookup_path_async(nfs, data, nfs4_chdir_1_cb) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_xstat64_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        GETATTR4resok *garesok;
        struct nfs_stat_64 st;
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "STAT64")) {
                return;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_GETATTR, "GETATTR")) < 0) {
                return;
        }
        garesok = &res->resarray.resarray_val[i].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;

        memset(&st, 0, sizeof(st));
        if (nfs_parse_attributes(nfs, data, &st,
                                 garesok->obj_attributes.attr_vals.attrlist4_val,
                                 garesok->obj_attributes.attr_vals.attrlist4_len) < 0) {
                data->cb(-EINVAL, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
        }

        data->cb(0, nfs, &st, data->private_data);
        free_nfs4_cb_data(data);
}

int
nfs4_stat64_async(struct nfs_context *nfs, const char *path,
                  int no_follow, nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;

        data = init_cb_data_full_path(nfs, path);
        if (data == NULL) {
                return -1;
        }

        if (no_follow) {
                data->flags |= LOOKUP_FLAG_NO_FOLLOW;
        }
        data->cb            = cb;
        data->private_data  = private_data;
        data->filler.func   = nfs4_populate_getattr;
        data->filler.max_op = 1;
        data->filler.data   = malloc(2 * sizeof(uint32_t));
        if (data->filler.data == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "data structure.");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return -1;
        }
        memset(data->filler.data, 0, 2 * sizeof(uint32_t));

        if (nfs4_lookup_path_async(nfs, data, nfs4_xstat64_cb) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

/* Takes object name as filler.data
 * blob0 as the fattr4 attribute mask
 * blob1 as the fattr4 attribute list
 */
static int
nfs4_populate_mkdir(struct nfs4_cb_data *data, nfs_argop4 *op)
{
        struct nfs_context *nfs = data->nfs;

        return nfs4_op_create(nfs, op, data->filler.data, NF4DIR,
                              &data->filler.blob0, &data->filler.blob1,
                              NULL, 0);
}

static void
nfs4_mkdir_cb(struct rpc_context *rpc, int status, void *command_data,
              void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "MKDIR")) {
                return;
        }

        data->cb(0, nfs, NULL, data->private_data);
        free_nfs4_cb_data(data);
}

int
nfs4_mkdir2_async(struct nfs_context *nfs, const char *path, int mode,
                 nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;
        uint32_t *u32ptr;

        data = init_cb_data_split_path(nfs, path);
        if (data == NULL) {
                return -1;
        }

        data->cb           = cb;
        data->private_data = private_data;
        data->filler.func = nfs4_populate_mkdir;
        data->filler.max_op = 1;
        
        /* attribute mask */
        u32ptr = malloc(2 * sizeof(uint32_t));
        if (u32ptr == NULL) {
                nfs_set_error(nfs, "Out of memory allocating bitmap");
                free_nfs4_cb_data(data);
                return -1;
        }
        u32ptr[0] = 0;
        u32ptr[1] = 1 << (FATTR4_MODE - 32);
        data->filler.blob0.len  = 2;
        data->filler.blob0.val  = u32ptr;
        data->filler.blob0.free = free;

        /* attribute values */
        u32ptr = malloc(1 * sizeof(uint32_t));
        if (u32ptr == NULL) {
                nfs_set_error(nfs, "Out of memory allocating attributes");
                free_nfs4_cb_data(data);
                return -1;
        }
        u32ptr[0] = htonl(mode);
        data->filler.blob1.len  = 4;
        data->filler.blob1.val  = u32ptr;
        data->filler.blob1.free = free;

        if (nfs4_lookup_path_async(nfs, data, nfs4_mkdir_cb) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

/* Takes object name as filler.data
 */
static int
nfs4_populate_remove(struct nfs4_cb_data *data, nfs_argop4 *op)
{
        struct nfs_context *nfs = data->nfs;

        return nfs4_op_remove(nfs, op, data->filler.data);
}

static void
nfs4_remove_cb(struct rpc_context *rpc, int status, void *command_data,
               void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "REMOVE")) {
                return;
        }

        data->cb(0, nfs, NULL, data->private_data);
        free_nfs4_cb_data(data);
}

static int
nfs4_remove_async(struct nfs_context *nfs, const char *path,
                  nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;

        data = init_cb_data_split_path(nfs, path);
        if (data == NULL) {
                return -1;
        }

        data->cb           = cb;
        data->private_data = private_data;
        data->filler.func = nfs4_populate_remove;
        data->filler.max_op = 1;

        if (nfs4_lookup_path_async(nfs, data, nfs4_remove_cb) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

int
nfs4_rmdir_async(struct nfs_context *nfs, const char *path,
                 nfs_cb cb, void *private_data)
{
        return nfs4_remove_async(nfs, path, cb, private_data);
}
    
static void
nfs_increment_seqid(struct nfs_context *nfs, uint32_t status)
{
        /* RFC3530 8.1.5 */
        switch (status) {
        case NFS4ERR_STALE_CLIENTID:
        case NFS4ERR_STALE_STATEID:
        case NFS4ERR_BAD_STATEID:
        case NFS4ERR_BAD_SEQID:
        case NFS4ERR_BADZDR:
        case NFS4ERR_RESOURCE:
        case NFS4ERR_NOFILEHANDLE:
                break;
        default:
                nfs->seqid++;
        }
}

static void
nfs4_open_setattr_cb(struct rpc_context *rpc, int status, void *command_data,
                     void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        struct nfsfh *fh;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "SETATTR")) {
                return;
        }

        fh = data->filler.blob0.val;
        data->filler.blob0.val = NULL;
        data->cb(0, nfs, fh, data->private_data);
        free_nfs4_cb_data(data);
}

static void
nfs4_open_truncate_cb(struct rpc_context *rpc, int status, void *command_data,
                      void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        struct nfsfh *fh = data->filler.blob0.val;
        COMPOUND4res *res = command_data;
        COMPOUND4args args;
        nfs_argop4 op[2];
        int i;

        if (check_nfs4_error(nfs, status, data, res, "OPEN")) {
                return;
        }

        i = nfs4_op_putfh(nfs, op, fh);
        i += nfs4_op_truncate(nfs, &op[i], fh, data->filler.blob3.val);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_open_setattr_cb, &args,
                                    data) != 0) {
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
}

static void
nfs4_open_confirm_cb(struct rpc_context *rpc, int status, void *command_data,
                     void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        OPEN_CONFIRM4resok *ocresok;
        int i;
        struct nfsfh *fh;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (res) {
                nfs_increment_seqid(nfs, res->status);
        }

        if (check_nfs4_error(nfs, status, data, res, "OPEN_CONFIRM")) {
                return;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_OPEN_CONFIRM,
                              "OPEN_CONFIRM")) < 0) {
                return;
        }
        ocresok = &res->resarray.resarray_val[i].nfs_resop4_u.opopen_confirm.OPEN_CONFIRM4res_u.resok4;

        fh = data->filler.blob0.val;

        fh->stateid.seqid = ocresok->open_stateid.seqid;
        memcpy(fh->stateid.other, ocresok->open_stateid.other, 12);

        if (data->open_cb) {
                data->open_cb(rpc, status, command_data, private_data);
                return;
        }
        data->filler.blob0.val = NULL;
        data->cb(0, nfs, fh, data->private_data);
        free_nfs4_cb_data(data);
}

static void
nfs4_open_cb(struct rpc_context *rpc, int status, void *command_data,
              void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        ACCESS4resok *aresok;
        OPEN4resok *oresok;
        GETFH4resok *gresok;
        int i;
        struct nfsfh *fh;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (res) {
                nfs_increment_seqid(nfs, res->status);
        }

        if (check_nfs4_error(nfs, status, data, res, "OPEN")) {
                return;
        }

        /* Parse Access and check that we have the access that we need */
        if ((i = nfs4_find_op(nfs, data, res, OP_ACCESS, "ACCESS")) < 0) {
                return;
        }
        aresok = &res->resarray.resarray_val[i].nfs_resop4_u.opaccess.ACCESS4res_u.resok4;
        if (aresok->supported != aresok->access) {
                nfs_set_error(nfs, "Insufficient ACCESS. Wanted %08x but "
                              "got %08x.", aresok->access, aresok->supported);
                data->cb(-EINVAL, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }

        /* Parse GetFH */
        if ((i = nfs4_find_op(nfs, data, res, OP_GETFH, "GETFH")) < 0) {
                return;
        }
        gresok = &res->resarray.resarray_val[i].nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

        fh = malloc(sizeof(*fh));
        if (fh == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "nfsfh");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        memset(fh, 0 , sizeof(*fh));

        data->filler.blob0.val  = fh;
        data->filler.blob0.free = (blob_free)nfs_free_nfsfh;

        fh->fh.len = gresok->object.nfs_fh4_len;
        fh->fh.val = malloc(fh->fh.len);
        if (fh->fh.val == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "nfsfh");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        memcpy(fh->fh.val, gresok->object.nfs_fh4_val, fh->fh.len);

        if (data->filler.flags & O_SYNC) {
                fh->is_sync = 1;
        }

        if (data->filler.flags & O_APPEND) {
                fh->is_append = 1;
        }

        /* Parse Open */
        if ((i = nfs4_find_op(nfs, data, res, OP_OPEN, "OPEN")) < 0) {
                return;
        }
        oresok = &res->resarray.resarray_val[i].nfs_resop4_u.opopen.OPEN4res_u.resok4;
        fh->stateid.seqid = oresok->stateid.seqid;
        memcpy(fh->stateid.other, oresok->stateid.other, 12);


        if (oresok->rflags & OPEN4_RESULT_CONFIRM) {
                COMPOUND4args args;
                nfs_argop4 op[2];

                memset(op, 0, sizeof(op));
                i = nfs4_op_putfh(nfs, &op[0], fh);
                i += nfs4_op_open_confirm(nfs, &op[i], nfs->seqid, fh);

                memset(&args, 0, sizeof(args));
                args.argarray.argarray_len = i;
                args.argarray.argarray_val = op;

                if (rpc_nfs4_compound_async(rpc, nfs4_open_confirm_cb, &args,
                                            private_data) != 0) {
                        data->cb(-ENOMEM, nfs, nfs_get_error(nfs),
                                 data->private_data);
                        free_nfs4_cb_data(data);
                        return;
                }
                return;
        }

        if (data->open_cb) {
                data->open_cb(rpc, status, command_data, private_data);
                return;
        }
        data->filler.blob0.val = NULL;
        data->cb(0, nfs, fh, data->private_data);
        free_nfs4_cb_data(data);
}

/* filler.flags are the open flags
 * filler.data is the object name
 */
static int
nfs4_populate_open(struct nfs4_cb_data *data, nfs_argop4 *op)
{
        struct nfs_context *nfs = data->nfs;
        OPEN4args *oargs;
        uint32_t access_mask = 0;
        int i;

        if (data->filler.flags & O_WRONLY) {
                access_mask |= ACCESS4_MODIFY;
        }
        if (data->filler.flags & O_RDWR) {
                access_mask |= ACCESS4_READ|ACCESS4_MODIFY;
        }
        if (!(data->filler.flags & (O_WRONLY|O_RDWR))) {
                access_mask |= ACCESS4_READ;
        }
        
        /* Access */
        i = nfs4_op_access(nfs, &op[0], access_mask);

        /* Open */
        op[i].argop = OP_OPEN;
        oargs = &op[i++].nfs_argop4_u.opopen;
        memset(oargs, 0, sizeof(*oargs));

        oargs->seqid = nfs->seqid;
        if (access_mask & ACCESS4_READ) {
                oargs->share_access |= OPEN4_SHARE_ACCESS_READ;
        }
        if (access_mask & ACCESS4_MODIFY) {
                oargs->share_access |= OPEN4_SHARE_ACCESS_WRITE;
        }
        oargs->share_deny = OPEN4_SHARE_DENY_NONE;
        oargs->owner.clientid = nfs->clientid;
        oargs->owner.owner.owner_len = strlen(nfs->client_name);
        oargs->owner.owner.owner_val = nfs->client_name;
        if (data->filler.flags & O_CREAT) {
                createhow4 *ch;
                fattr4 *fa;

                ch = &oargs->openhow.openflag4_u.how;
                fa = &ch->createhow4_u.createattrs;

                oargs->openhow.opentype = OPEN4_CREATE;
                ch->mode = UNCHECKED4;
                fa->attrmask.bitmap4_len = data->filler.blob1.len;
                fa->attrmask.bitmap4_val = data->filler.blob1.val;

                fa->attr_vals.attrlist4_len = data->filler.blob2.len;
                fa->attr_vals.attrlist4_val = data->filler.blob2.val;
        } else {
                oargs->openhow.opentype = OPEN4_NOCREATE;
        }
        oargs->claim.claim = CLAIM_NULL;
        oargs->claim.open_claim4_u.file.utf8string_len =
                strlen(data->filler.data);
        oargs->claim.open_claim4_u.file.utf8string_val =
                data->filler.data;

        /* GetFH */
        i += nfs4_op_getfh(nfs, &op[i]);

        return i;
}

static void
nfs4_open_readlink_cb(struct rpc_context *rpc, int status, void *command_data,
                 void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        READLINK4resok *rlresok;
        int i;
        char *path;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "READLINK")) {
                return;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_READLINK, "READLINK")) < 0) {
                return;
        }

        rlresok = &res->resarray.resarray_val[i].nfs_resop4_u.opreadlink.READLINK4res_u.resok4;

        path = malloc(2 + strlen(data->path) +
                      strlen(rlresok->link.utf8string_val));
        if (path == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "path");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs),
                         data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        sprintf(path, "%s/%s", data->path, rlresok->link.utf8string_val);



        free(data->path);
        data->path = NULL;
        free(data->filler.data);
        data->filler.data = NULL;

        data->path = nfs4_resolve_path(nfs, path);
        free(path);
        if (data->path == NULL) {
                data->cb(-EINVAL, nfs, nfs_get_error(nfs),
                         data->private_data);
                free_nfs4_cb_data(data);
                return;
        }

        data_split_path(data);

        data->filler.func = nfs4_populate_open;
        data->filler.max_op = 3;
 
        if (nfs4_lookup_path_async(nfs, data, nfs4_open_cb) < 0) {
                data->cb(-ENOMEM, nfs, res, data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
}

static int
nfs4_populate_lookup_readlink(struct nfs4_cb_data *data, nfs_argop4 *op)
{
        struct nfs_context *nfs = data->nfs;
        int i;

        i = nfs4_op_lookup(nfs, &op[0], data->filler.data);
        i += nfs4_op_readlink(nfs, &op[i]);

        return i;
}

/* If the final component in the open was a symlink we need to resolve it and
 * re-try the nfs4_open_async()
 */
static int
nfs4_open_readlink(struct rpc_context *rpc, COMPOUND4res *res,
                   struct nfs4_cb_data *data)
{
        struct nfs_context *nfs = data->nfs;
        int i;

        for (i = 0; i < (int)res->resarray.resarray_len; i++) {
                OPEN4res *ores;

                if (res->resarray.resarray_val[i].resop != OP_OPEN) {
                        continue;
                }
                ores = &res->resarray.resarray_val[i].nfs_resop4_u.opopen;

                if (ores->status != NFS4ERR_SYMLINK) {
                        continue;
                }

                if (data->filler.flags & O_NOFOLLOW) {
                        nfs_set_error(nfs, "Symlink encountered during "
                                      "open(O_NOFOLLOW)");
                        data->cb(-ELOOP, nfs, nfs_get_error(nfs),
                                 data->private_data);
                        return -1;
                }

                /* The object we need to do readlink on is already stored in
                 * data->filler.data so *populate* can just grab it from there.
                 */
                data->filler.func = nfs4_populate_lookup_readlink;
                data->filler.max_op = 2;

                if (nfs4_lookup_path_async(nfs, data,
                                           nfs4_open_readlink_cb) < 0) {
                        data->cb(-ENOMEM, nfs, nfs_get_error(nfs),
                                 data->private_data);
                        free_nfs4_cb_data(data);
                        return -1;
                }
                return -1;
        }

        return 0;
}

/*
 * data.blob0 is used for nfsfh
 * data.blob1 is used for the attribute mask in case on O_CREAT
 * data.blob2 is the attribute value in case of O_CREAT
 */
static int
nfs4_open_async_internal(struct nfs_context *nfs, struct nfs4_cb_data *data,
                         int flags, int mode)
{
        if (flags & O_APPEND && !(flags & (O_RDWR|O_WRONLY))) {
                flags &= ~O_APPEND;
        }

        if (flags & O_CREAT) {
                uint32_t *d;

                /* Attribute mask */
                d = malloc(2 * sizeof(uint32_t));
                if (d == NULL) {
                        nfs_set_error(nfs, "Out of memory");
                        free_nfs4_cb_data(data);
                        return -1;
                }
                d[0] = 0;
                d[1] = 1 << (FATTR4_MODE - 32);

                data->filler.blob1.val  = d;
                data->filler.blob1.len  = 2;
                data->filler.blob1.free = free;

                /* Attribute value */
                d = malloc(sizeof(uint32_t));
                if (d == NULL) {
                        nfs_set_error(nfs, "Out of memory");
                        free_nfs4_cb_data(data);
                        return -1;
                }

                *d = htonl(mode);

                data->filler.blob2.val  = d;
                data->filler.blob2.len  = 4;
                data->filler.blob2.free = free;
        }

        data->filler.func = nfs4_populate_open;
        data->filler.max_op = 3;
        data->filler.flags = flags;

        if (nfs4_lookup_path_async(nfs, data, nfs4_open_cb) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

int
nfs4_open_async(struct nfs_context *nfs, const char *path, int flags,
                int mode, nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;

        data = init_cb_data_split_path(nfs, path);
        if (data == NULL) {
                return -1;
        }

        data->cb           = cb;
        data->private_data = private_data;

        /* O_TRUNC is only valid for O_RDWR or O_WRONLY */
        if (flags & O_TRUNC && !(flags & (O_RDWR|O_WRONLY))) {
                flags &= ~O_TRUNC;
        }

        if (flags & O_TRUNC) {
                data->open_cb = nfs4_open_truncate_cb;

                data->filler.blob3.val = malloc(12);
                if (data->filler.blob3.val == NULL) {
                        nfs_set_error(nfs, "Out of memory");
                        free_nfs4_cb_data(data);
                        return -1;
                }
                data->filler.blob3.free = free;

                memset(data->filler.blob3.val, 0, 12);
        }

        return nfs4_open_async_internal(nfs, data, flags, mode);
}

int
nfs4_fstat64_async(struct nfs_context *nfs, struct nfsfh *nfsfh, nfs_cb cb,
                   void *private_data)
{
        COMPOUND4args args;
        nfs_argop4 op[2];
        struct nfs4_cb_data *data;
        int i;

        data = malloc(sizeof(*data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "cb data");
                return -1;
        }
        memset(data, 0, sizeof(*data));

        data->nfs          = nfs;
        data->cb           = cb;
        data->private_data = private_data;

        i = nfs4_op_putfh(nfs, &op[0], nfsfh);
        i += nfs4_op_getattr(nfs, &op[i], standard_attributes, 2);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_xstat64_cb, &args,
                                    data) != 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_close_cb(struct rpc_context *rpc, int status, void *command_data,
              void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (res) {
                nfs_increment_seqid(nfs, res->status);
        }

        if (check_nfs4_error(nfs, status, data, res, "CLOSE")) {
                return;
        }

        data->cb(0, nfs, NULL, data->private_data);
        free_nfs4_cb_data(data);
}

int
nfs4_close_async(struct nfs_context *nfs, struct nfsfh *nfsfh, nfs_cb cb,
                 void *private_data)
{
        COMPOUND4args args;
        nfs_argop4 op[3];
        struct nfs4_cb_data *data;
        int i;

        data = malloc(sizeof(*data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "cb data");
                return -1;
        }
        memset(data, 0, sizeof(*data));

        data->nfs          = nfs;
        data->cb           = cb;
        data->private_data = private_data;

        memset(op, 0, sizeof(op));

        i = nfs4_op_putfh(nfs, &op[0], nfsfh);
        i += nfs4_op_close(nfs, &op[i], nfsfh);

        data->filler.blob0.val  = nfsfh;
        data->filler.blob0.free = (blob_free)nfs_free_nfsfh;

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_close_cb, &args,
                                    data) != 0) {
                data->filler.blob0.val = NULL;
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_pread_cb(struct rpc_context *rpc, int status, void *command_data,
              void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        READ4resok *rres = NULL;
        struct nfsfh *nfsfh;
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        nfsfh = data->filler.blob0.val;

        if (check_nfs4_error(nfs, status, data, res, "READ")) {
                return;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_READ, "READ")) < 0) {
                return;
        }
        rres = &res->resarray.resarray_val[i].nfs_resop4_u.opread.READ4res_u.resok4;

        if (data->rw_data.update_pos) {
                nfsfh->offset = data->rw_data.offset + rres->data.data_len;
        }

        data->cb(rres->data.data_len, nfs, rres->data.data_val,
                 data->private_data);
        free_nfs4_cb_data(data);
}

int
nfs4_pread_async_internal(struct nfs_context *nfs, struct nfsfh *nfsfh,
                          uint64_t offset, size_t count, nfs_cb cb,
                          void *private_data, int update_pos)
{
        COMPOUND4args args;
        nfs_argop4 op[2];
        struct nfs4_cb_data *data;
        int i;

        data = malloc(sizeof(*data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "cb data");
                return -1;
        }
        memset(data, 0, sizeof(*data));

        data->nfs          = nfs;
        data->cb           = cb;
        data->private_data = private_data;

        data->filler.blob0.val  = nfsfh;
        data->filler.blob0.free = NULL;
        data->rw_data.offset = offset;
        data->rw_data.update_pos = update_pos;
        
        memset(op, 0, sizeof(op));

        i = nfs4_op_putfh(nfs, &op[0], nfsfh);
        i += nfs4_op_read(nfs, &op[i], nfsfh, offset, count);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_pread_cb, &args,
                                    data) != 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_symlink_cb(struct rpc_context *rpc, int status, void *command_data,
              void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "SYMLINK")) {
                return;
        }

        data->cb(0, nfs, NULL, data->private_data);
        free_nfs4_cb_data(data);
}

/* Takes object name as filler.data
 * blob0 as the target
 */
static int
nfs4_populate_symlink(struct nfs4_cb_data *data, nfs_argop4 *op)
{
        struct nfs_context *nfs = data->nfs;

        return nfs4_op_create(nfs, op, data->filler.data, NF4LNK,
                              NULL, NULL, data->filler.blob0.val, 0);

        return 1;
}

int
nfs4_symlink_async(struct nfs_context *nfs, const char *target,
                   const char *linkname, nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;

        data = init_cb_data_split_path(nfs, linkname);
        if (data == NULL) {
                return -1;
        }

        data->cb           = cb;
        data->private_data = private_data;
        data->filler.func = nfs4_populate_symlink;
        data->filler.max_op = 1;

        data->filler.blob0.val  = strdup(target);
        data->filler.blob0.free = free;

        if (nfs4_lookup_path_async(nfs, data, nfs4_symlink_cb) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_readlink_cb(struct rpc_context *rpc, int status, void *command_data,
                 void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        READLINK4resok *rlresok;
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "READLINK")) {
                return;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_READLINK, "READLINK")) < 0) {
                return;
        }

        rlresok = &res->resarray.resarray_val[i].nfs_resop4_u.opreadlink.READLINK4res_u.resok4;

        data->cb(0, nfs, rlresok->link.utf8string_val, data->private_data);
        free_nfs4_cb_data(data);
}

static int
nfs4_populate_readlink(struct nfs4_cb_data *data, nfs_argop4 *op)
{
        struct nfs_context *nfs = data->nfs;
        int i;

        i = nfs4_op_readlink(nfs, &op[0]);

        return i;
}

int
nfs4_readlink_async(struct nfs_context *nfs, const char *path, nfs_cb cb,
                    void *private_data)
{
        struct nfs4_cb_data *data;

        data = init_cb_data_full_path(nfs, path);
        if (data == NULL) {
                return -1;
        }

        data->cb           = cb;
        data->private_data = private_data;
        data->filler.func = nfs4_populate_readlink;
        data->filler.max_op = 1;
        data->flags |= LOOKUP_FLAG_NO_FOLLOW;

        if (nfs4_lookup_path_async(nfs, data, nfs4_readlink_cb) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_pwrite_cb(struct rpc_context *rpc, int status, void *command_data,
               void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        WRITE4resok *wres = NULL;
        struct nfsfh *nfsfh;
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        nfsfh = data->filler.blob0.val;

        if (check_nfs4_error(nfs, status, data, res, "WRITE")) {
                return;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_WRITE, "WRITE")) < 0) {
                return;
        }
        wres = &res->resarray.resarray_val[i].nfs_resop4_u.opwrite.WRITE4res_u.resok4;

        if (data->rw_data.update_pos) {
                nfsfh->offset = data->rw_data.offset + wres->count;
        }

        data->cb(wres->count, nfs, NULL, data->private_data);
        free_nfs4_cb_data(data);
}

int
nfs4_pwrite_async_internal(struct nfs_context *nfs, struct nfsfh *nfsfh,
                           uint64_t offset, size_t count, const char *buf,
                           nfs_cb cb, void *private_data, int update_pos)
{
        COMPOUND4args args;
        nfs_argop4 op[2];
        struct nfs4_cb_data *data;
        int i;

        data = malloc(sizeof(*data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "cb data");
                return -1;
        }
        memset(data, 0, sizeof(*data));

        data->nfs          = nfs;
        data->cb           = cb;
        data->private_data = private_data;

        data->filler.blob0.val  = nfsfh;
        data->filler.blob0.free = NULL;
        data->rw_data.offset = offset;
        data->rw_data.update_pos = update_pos;

        memset(op, 0, sizeof(op));

        i = nfs4_op_putfh(nfs, &op[0], nfsfh);
        i += nfs4_op_write(nfs, &op[i], nfsfh, offset, count, buf);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_pwrite_cb, &args,
                                    data) != 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_write_append_cb(struct rpc_context *rpc, int status, void *command_data,
               void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        GETATTR4resok *garesok = NULL;
        struct nfsfh *nfsfh;
        int i;
        uint64_t offset;
        char *buf;
        uint32_t count;
        struct nfs_stat_64 st;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        nfsfh = data->filler.blob0.val;

        buf = data->filler.blob1.val;
        count = data->filler.blob1.len;

        if (check_nfs4_error(nfs, status, data, res, "GETATTR")) {
                return;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_GETATTR, "GETATTR")) < 0) {
                return;
        }


        garesok = &res->resarray.resarray_val[i].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;
        if (garesok->obj_attributes.attr_vals.attrlist4_len < 8) {
                data->cb(-EINVAL, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }

        memset(&st, 0, sizeof(st));
        nfs_parse_attributes(nfs, data, &st,
                             garesok->obj_attributes.attr_vals.attrlist4_val,
                             garesok->obj_attributes.attr_vals.attrlist4_len);
        offset = st.nfs_size;

        if (nfs4_pwrite_async_internal(nfs, nfsfh, offset,
                                       (size_t)count, buf,
                                       data->cb, data->private_data, 1) < 0) {
                free_nfs4_cb_data(data);
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                return;
        }

        free_nfs4_cb_data(data);
}

int
nfs4_write_async(struct nfs_context *nfs, struct nfsfh *nfsfh, uint64_t count,
                const void *buf, nfs_cb cb, void *private_data)
{
        if (nfsfh->is_append) {
                COMPOUND4args args;
                nfs_argop4 op[2];
                struct nfs4_cb_data *data;
                int i;

                data = malloc(sizeof(*data));
                if (data == NULL) {
                        nfs_set_error(nfs, "Out of memory. Failed to allocate "
                                      "cb data");
                        return -1;
                }
                memset(data, 0, sizeof(*data));

                data->nfs          = nfs;
                data->cb           = cb;
                data->private_data = private_data;

                data->filler.blob0.val  = nfsfh;
                data->filler.blob0.free = NULL;

                memset(op, 0, sizeof(op));

                i = nfs4_op_putfh(nfs, &op[0], nfsfh);
                i += nfs4_op_getattr(nfs, &op[i], standard_attributes, 2);

                memset(&args, 0, sizeof(args));
                args.argarray.argarray_len = i;
                args.argarray.argarray_val = op;

                data->filler.blob0.val  = nfsfh;
                data->filler.blob0.free = NULL;

                data->filler.blob1.val = discard_const(buf);
                data->filler.blob1.len = count;
                data->filler.blob1.free = NULL;

                if (rpc_nfs4_compound_async(nfs->rpc, nfs4_write_append_cb,
                                            &args, data) != 0) {
                        free_nfs4_cb_data(data);
                        return -1;
                }

                return 0;
        }

        return nfs4_pwrite_async_internal(nfs, nfsfh, nfsfh->offset,
                                          (size_t)count, buf,
                                          cb, private_data, 1);
}

int
nfs4_create_async(struct nfs_context *nfs, const char *path, int flags,
                  int mode, nfs_cb cb, void *private_data)
{
        return nfs4_open_async(nfs, path, O_CREAT | flags, mode,
                               cb, private_data);
}

int
nfs4_unlink_async(struct nfs_context *nfs, const char *path,
                  nfs_cb cb, void *private_data)
{
        return nfs4_remove_async(nfs, path, cb, private_data);
}

static void
nfs4_link_2_cb(struct rpc_context *rpc, int status, void *command_data,
             void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "LINK")) {
                return;
        }

        data->cb(0, nfs, NULL, data->private_data);
        free_nfs4_cb_data(data);
}

static int
nfs4_populate_link(struct nfs4_cb_data *data, nfs_argop4 *op)
{
        struct nfs_context *nfs = data->nfs;
        struct nfsfh *nfsfh = data->filler.blob0.val;

        int i;

        i = nfs4_op_savefh(nfs, &op[0]);
        i += nfs4_op_putfh(nfs, &op[i], nfsfh);
        i += nfs4_op_link(nfs, &op[i], data->filler.data);

        return i;
}

static void
nfs4_link_1_cb(struct rpc_context *rpc, int status, void *command_data,
               void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        GETFH4resok *gfhresok;
        int i;
        struct nfsfh *fh;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "LINK")) {
                return;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_GETFH, "GETFH")) < 0) {
                return;
        }
        gfhresok = &res->resarray.resarray_val[i].nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

        /* oldpath fh */
        fh = malloc(sizeof(*fh));
        if (fh == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "nfsfh");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        memset(fh, 0 , sizeof(*fh));
        data->filler.blob0.val  = fh;
        data->filler.blob0.free = (blob_free)nfs_free_nfsfh;

        fh->fh.len = gfhresok->object.nfs_fh4_len;
        fh->fh.val = malloc(fh->fh.len);
        if (fh->fh.val == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "nfsfh");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        memcpy(fh->fh.val, gfhresok->object.nfs_fh4_val, fh->fh.len);


        data->filler.func = nfs4_populate_link;
        data->filler.max_op = 3;

        free(data->path);
        data->path = data->filler.blob1.val;
        data->filler.blob1.val  = NULL;
        data->filler.blob1.free = NULL;

        if (nfs4_lookup_path_async(nfs, data, nfs4_link_2_cb) < 0) {
                data->cb(-EFAULT, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
}

/*
 * filler.data is the name of the new object
 * blob0 is the filehandle for newpath parent directory.
 * blob1 is oldpath.
 */
int
nfs4_link_async(struct nfs_context *nfs, const char *oldpath,
                const char *newpath, nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;

        data = init_cb_data_split_path(nfs, newpath);
        if (data == NULL) {
                return -1;
        }

        data->cb            = cb;
        data->private_data  = private_data;
        data->filler.func   = nfs4_populate_getfh;
        data->filler.max_op = 1;

        /* oldpath */
        data->filler.blob1.val  = strdup(oldpath);
        if (data->filler.blob1.val == NULL) {
                nfs_set_error(nfs, "Out of memory");
                free_nfs4_cb_data(data);
                return -1;
        }
        data->filler.blob1.free = free;

        if (nfs4_lookup_path_async(nfs, data, nfs4_link_1_cb) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_rename_2_cb(struct rpc_context *rpc, int status, void *command_data,
                 void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "RENAME")) {
                return;
        }

        data->cb(0, nfs, NULL, data->private_data);
        free_nfs4_cb_data(data);
}

static int
nfs4_populate_rename(struct nfs4_cb_data *data, nfs_argop4 *op)
{
        struct nfs_context *nfs = data->nfs;
        struct nfsfh *nfsfh = data->filler.blob0.val;
        int i;

        i = nfs4_op_savefh(nfs, &op[0]);
        i += nfs4_op_putfh(nfs, &op[i], nfsfh);
        i += nfs4_op_rename(nfs, &op[i], data->filler.data,
                            data->filler.blob1.val);

        return i;
}

static void
nfs4_rename_1_cb(struct rpc_context *rpc, int status, void *command_data,
                 void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        GETFH4resok *gfhresok;
        int i;
        struct nfsfh *fh;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "RENAME")) {
                return;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_GETFH, "GETFH")) < 0) {
                return;
        }
        gfhresok = &res->resarray.resarray_val[i].nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

        /* newpath fh */
        fh = malloc(sizeof(*fh));
        if (fh == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "nfsfh");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        memset(fh, 0 , sizeof(*fh));
        data->filler.blob0.val  = fh;
        data->filler.blob0.free = (blob_free)nfs_free_nfsfh;

        fh->fh.len = gfhresok->object.nfs_fh4_len;
        fh->fh.val = malloc(fh->fh.len);
        if (fh->fh.val == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "nfsfh");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        memcpy(fh->fh.val, gfhresok->object.nfs_fh4_val, fh->fh.len);

        data->filler.blob1.val  = data->filler.data;
        data->filler.blob1.free = free;
        data->filler.data = NULL;

        /* Update path and data to point to the old path/name */
        free(data->path);
        data->path = nfs4_resolve_path(nfs, data->filler.blob2.val);
        if (data->path == NULL) {
                data->cb(-EINVAL, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        data_split_path(data);

        data->filler.func   = nfs4_populate_rename;
        data->filler.max_op = 3;

        if (nfs4_lookup_path_async(nfs, data, nfs4_rename_2_cb) < 0) {
                nfs_set_error(nfs, "Out of memory.");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
}

/*
 * blob0 is the filehandle for newpath parent directory.
 * blob1 is the new name
 * blob2 is oldpath.
 */
int
nfs4_rename_async(struct nfs_context *nfs, const char *oldpath,
                  const char *newpath, nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;

        data = init_cb_data_split_path(nfs, newpath);
        if (data == NULL) {
                return -1;
        }

        data->cb            = cb;
        data->private_data  = private_data;
        data->filler.func   = nfs4_populate_getfh;
        data->filler.max_op = 1;

        /* oldpath */
        data->filler.blob2.val  = strdup(oldpath);
        if (data->filler.blob2.val == NULL) {
                nfs_set_error(nfs, "Out of memory");
                free_nfs4_cb_data(data);
                return -1;
        }
        data->filler.blob2.free = free;

        if (nfs4_lookup_path_async(nfs, data, nfs4_rename_1_cb) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_mknod_cb(struct rpc_context *rpc, int status, void *command_data,
              void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "MKNOD")) {
                return;
        }

        data->cb(0, nfs, NULL, data->private_data);
        free_nfs4_cb_data(data);
}

static int
nfs4_populate_mknod(struct nfs4_cb_data *data, nfs_argop4 *op)
{
        struct nfs_context *nfs = data->nfs;
        uint32_t mode, *ptr;
        int dev;

        /* Strip off the file type before we marshall it */
        ptr = (void *)data->filler.blob1.val;
        mode = *ptr;
        *ptr = htonl(mode & ~S_IFMT);

        dev = data->filler.blob2.len;

        switch (mode & S_IFMT) {
	case S_IFBLK:
                return nfs4_op_create(nfs, op, data->filler.data, NF4BLK,
                                      &data->filler.blob0, &data->filler.blob1,
                                      NULL, dev);
	case S_IFCHR:
                return nfs4_op_create(nfs, op, data->filler.data, NF4CHR,
                                      &data->filler.blob0, &data->filler.blob1,
                                      NULL, dev);
        }

        return 1;
}

/* Takes object name as filler.data
 * blob0 as attribute mask
 * blob1 as attribute value
 * blob2.len as dev
 */
int
nfs4_mknod_async(struct nfs_context *nfs, const char *path, int mode, int dev,
                 nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;
        uint32_t *u32ptr;

        switch (mode & S_IFMT) {
	case S_IFCHR:
	case S_IFBLK:
                break;
        default:
		nfs_set_error(nfs, "Invalid file type for "
                              "MKNOD call");
		return -1;
        }

        data = init_cb_data_split_path(nfs, path);
        if (data == NULL) {
                return -1;
        }

        data->cb            = cb;
        data->private_data  = private_data;
        data->filler.func   = nfs4_populate_mknod;
        data->filler.max_op = 1;

        /* attribute mask */
        u32ptr = malloc(2 * sizeof(uint32_t));
        if (u32ptr == NULL) {
                nfs_set_error(nfs, "Out of memory allocating bitmap");
                return 0;
        }
        u32ptr[0] = 0;
        u32ptr[1] = 1 << (FATTR4_MODE - 32);
        data->filler.blob0.len  = 2;
        data->filler.blob0.val  = u32ptr;
        data->filler.blob0.free = free;

        /* attribute values */
        u32ptr = malloc(1 * sizeof(uint32_t));
        if (u32ptr == NULL) {
                nfs_set_error(nfs, "Out of memory allocating attributes");
                free_nfs4_cb_data(data);
                return -1;
        }
        u32ptr[0] = mode;
        data->filler.blob1.len  = 4;
        data->filler.blob1.val  = u32ptr;
        data->filler.blob1.free = free;

        data->filler.blob2.len  = dev;

        if (nfs4_lookup_path_async(nfs, data, nfs4_mknod_cb) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_parse_readdir(struct nfs_context *nfs, struct nfs4_cb_data *data,
                   READDIR4resok *res);

static void
nfs4_opendir_2_cb(struct rpc_context *rpc, int status, void *command_data,
                  void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        READDIR4resok *rdresok;
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "READDIR")) {
                return;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_READDIR, "READDIR")) < 0) {
                return;
        }
        rdresok = &res->resarray.resarray_val[i].nfs_resop4_u.opreaddir.READDIR4res_u.resok4;
        nfs4_parse_readdir(nfs, data, rdresok);
}

static void
nfs4_opendir_continue(struct nfs_context *nfs, struct nfs4_cb_data *data)
{
        COMPOUND4args args;
        nfs_argop4 op[2];
        struct nfsfh *fh = data->filler.blob0.val;
        uint64_t cookie;
        int i;

        memcpy(&cookie, data->filler.blob2.val, sizeof(uint64_t));

        memset(op, 0, sizeof(op));

        i = nfs4_op_putfh(nfs, &op[0], fh);
        i += nfs4_op_readdir(nfs, &op[i], cookie);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_opendir_2_cb, &args,
                                    data) != 0) {
                nfs_set_error(nfs, "Failed to queue READDIR command. %s",
                              nfs_get_error(nfs));
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
}

static void
nfs4_parse_readdir(struct nfs_context *nfs, struct nfs4_cb_data *data,
                   READDIR4resok *res)
{
	struct nfsdir *nfsdir = data->filler.blob1.val;
        struct entry4 *e;

        e = res->reply.entries;
        while (e) {
                struct nfsdirent *nfsdirent;
                struct nfs_stat_64 st;

                memcpy(data->filler.blob2.val, &e->cookie, sizeof(uint64_t));

		nfsdirent = malloc(sizeof(struct nfsdirent));
		if (nfsdirent == NULL) {
                        nfs_set_error(nfs, "Out of memory.");
                        data->cb(-ENOMEM, nfs, nfs_get_error(nfs),
                                 data->private_data);
                        free_nfs4_cb_data(data);
                        return;
                }
		nfsdirent->name = strdup(e->name.utf8string_val);
		if (nfsdirent->name == NULL) {
                        nfs_set_error(nfs, "Out of memory.");
                        data->cb(-ENOMEM, nfs, nfs_get_error(nfs),
                                 data->private_data);
                        free_nfs4_cb_data(data);
                        free(nfsdirent);
                        return;
                }

                memset(&st, 0, sizeof(st));
                if (nfs_parse_attributes(nfs, data, &st,
                                         e->attrs.attr_vals.attrlist4_val,
                                         e->attrs.attr_vals.attrlist4_len) < 0) {
                        data->cb(-EINVAL, nfs, nfs_get_error(nfs),
                                 data->private_data);
                        free_nfs4_cb_data(data);
                        free(nfsdirent->name);
                        free(nfsdirent);
                        return;
                }

                nfsdirent->mode = st.nfs_mode;
                switch (st.nfs_mode & S_IFMT) {
                case S_IFREG:
                        nfsdirent->type = NF4REG;
                        break;
                case S_IFDIR:
                        nfsdirent->type = NF4DIR;
                        break;
                case S_IFBLK:
                        nfsdirent->type = NF4BLK;
                        break;
                case S_IFCHR:
                        nfsdirent->type = NF4CHR;
                        break;
                case S_IFLNK:
                        nfsdirent->type = NF4LNK;
                        break;
                case S_IFSOCK:
                        nfsdirent->type = NF4SOCK;
                        break;
                case S_IFIFO:
                        nfsdirent->type = NF4FIFO;
                        break;
                }
                nfsdirent->size = st.nfs_size;
                nfsdirent->atime.tv_sec  = st.nfs_atime;
                nfsdirent->atime.tv_usec = st.nfs_atime_nsec/1000;
                nfsdirent->atime_nsec    = st.nfs_atime_nsec;
                nfsdirent->mtime.tv_sec  = st.nfs_mtime;
                nfsdirent->mtime.tv_usec = st.nfs_mtime_nsec/1000;
                nfsdirent->mtime_nsec    = st.nfs_mtime_nsec;
                nfsdirent->ctime.tv_sec  = st.nfs_ctime;
                nfsdirent->ctime.tv_usec = st.nfs_ctime_nsec/1000;
                nfsdirent->ctime_nsec    = st.nfs_ctime_nsec;
                nfsdirent->uid = st.nfs_uid;
                nfsdirent->gid = st.nfs_gid;
                nfsdirent->nlink = st.nfs_nlink;
                nfsdirent->dev = st.nfs_dev;
                nfsdirent->rdev = st.nfs_rdev;
                nfsdirent->blksize = NFS_BLKSIZE;
                nfsdirent->blocks = st.nfs_blocks;
                nfsdirent->used = st.nfs_used;

		nfsdirent->next  = nfsdir->entries;
		nfsdir->entries  = nfsdirent;
                e = e->nextentry;
        }

        if (res->reply.eof == 0) {
                nfs4_opendir_continue(nfs, data);
                return;
        }

        nfsdir->current = nfsdir->entries;
        data->filler.blob1.val = NULL;
        data->cb(0, nfs, nfsdir, data->private_data);
        free_nfs4_cb_data(data);
}

static void
nfs4_opendir_cb(struct rpc_context *rpc, int status, void *command_data,
                void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        struct nfsfh *fh;
        GETFH4resok *gresok;
        READDIR4resok *rdresok;
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "READDIR")) {
                return;
        }

        /* Parse GetFH */
        if ((i = nfs4_find_op(nfs, data, res, OP_GETFH, "GETFH")) < 0) {
                return;
        }
        gresok = &res->resarray.resarray_val[i].nfs_resop4_u.opgetfh.GETFH4res_u.resok4;

        fh = malloc(sizeof(*fh));
        if (fh == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "nfsfh");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        memset(fh, 0 , sizeof(*fh));

        data->filler.blob0.val  = fh;
        data->filler.blob0.free = (blob_free)nfs_free_nfsfh;

        fh->fh.len = gresok->object.nfs_fh4_len;
        fh->fh.val = malloc(fh->fh.len);
        if (fh->fh.val == NULL) {
                nfs_set_error(nfs, "Out of memory. Failed to allocate "
                              "nfsfh");
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
        memcpy(fh->fh.val, gresok->object.nfs_fh4_val, fh->fh.len);

        if ((i = nfs4_find_op(nfs, data, res, OP_READDIR, "READDIR")) < 0) {
                return;
        }
        rdresok = &res->resarray.resarray_val[i].nfs_resop4_u.opreaddir.READDIR4res_u.resok4;
        nfs4_parse_readdir(nfs, data, rdresok);
}

static int
nfs4_populate_readdir(struct nfs4_cb_data *data, nfs_argop4 *op)
{
        struct nfs_context *nfs = data->nfs;
        uint64_t cookie;
        int i;

        memcpy(&cookie, data->filler.blob2.val, sizeof(uint64_t));

        i = nfs4_op_getfh(nfs, &op[0]);
        i += nfs4_op_readdir(nfs, &op[i], cookie);

        return i;
}


/* blob0 is the directory filehandle
 * blob1 is nfsdir
 * blob2 is the cookie
 */
int
nfs4_opendir_async(struct nfs_context *nfs, const char *path, nfs_cb cb,
                   void *private_data)
{
        struct nfs4_cb_data *data;
	struct nfsdir *nfsdir;

        data = init_cb_data_split_path(nfs, path);
        if (data == NULL) {
                return -1;
        }

        data->cb           = cb;
        data->private_data = private_data;
        data->filler.func = nfs4_populate_readdir;
        data->filler.max_op = 2;

	nfsdir = malloc(sizeof(struct nfsdir));
	if (nfsdir == NULL) {
                free_nfs4_cb_data(data);
		nfs_set_error(nfs, "failed to allocate buffer for nfsdir");
		return -1;
	}
	memset(nfsdir, 0, sizeof(struct nfsdir));

        data->filler.blob1.val = nfsdir;
        data->filler.blob1.free = (blob_free)nfs_free_nfsdir;

	data->filler.blob2.val = malloc(sizeof(uint64_t));
	if (data->filler.blob2.val == NULL) {
                free_nfs4_cb_data(data);
		nfs_set_error(nfs, "failed to allocate buffer for cookie");
		return -1;
	}
	memset(data->filler.blob2.val, 0, sizeof(uint64_t));
        data->filler.blob2.free = (blob_free)free;

        if (nfs4_lookup_path_async(nfs, data, nfs4_opendir_cb) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_truncate_close_cb(struct rpc_context *rpc, int status, void *command_data,
                      void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (res) {
                nfs_increment_seqid(nfs, res->status);
        }

        if (check_nfs4_error(nfs, status, data, res, "CLOSE")) {
                return;
        }

        data->cb(0, nfs, NULL, data->private_data);
        free_nfs4_cb_data(data);
}

static void
nfs4_truncate_open_cb(struct rpc_context *rpc, int status, void *command_data,
                      void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        struct nfsfh *fh = data->filler.blob0.val;
        COMPOUND4res *res = command_data;
        COMPOUND4args args;
        nfs_argop4 op[4];
        int i;

        if (check_nfs4_error(nfs, status, data, res, "OPEN")) {
                return;
        }

        i = nfs4_op_putfh(nfs, &op[0], fh);
        i += nfs4_op_truncate(nfs, &op[i], fh, data->filler.blob3.val);
        i += nfs4_op_close(nfs, &op[i], fh);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_truncate_close_cb, &args,
                                    data) != 0) {
                /* Not much we can do but leak one fd on the server :( */
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
}

/*
 * data.blob3.val is a 12 byte SETATTR buffer for length+update_mtime
 */
int
nfs4_truncate_async(struct nfs_context *nfs, const char *path, uint64_t length,
                    nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;

        data = init_cb_data_split_path(nfs, path);
        if (data == NULL) {
                return -1;
        }

        data->cb           = cb;
        data->private_data = private_data;
        data->open_cb      = nfs4_truncate_open_cb;

        data->filler.blob3.val = malloc(12);
        if (data->filler.blob3.val == NULL) {
                nfs_set_error(nfs, "Out of memory");
                free_nfs4_cb_data(data);
                return -1;
        }
        data->filler.blob3.free = free;

        memset(data->filler.blob3.val, 0, 12);
        length = nfs_hton64(length);
        memcpy(data->filler.blob3.val, &length, sizeof(uint64_t));

        if (nfs4_open_async_internal(nfs, data, O_WRONLY, 0) < 0) {
                return -1;
        }

        return 0;
}

static void
nfs4_fsync_cb(struct rpc_context *rpc, int status, void *command_data,
              void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "FSYNC")) {
                return;
        }

        data->cb(0, nfs, NULL, data->private_data);
        free_nfs4_cb_data(data);
}

int
nfs4_fsync_async(struct nfs_context *nfs, struct nfsfh *fh, nfs_cb cb,
                 void *private_data)
{
        COMPOUND4args args;
        nfs_argop4 op[2];
        struct nfs4_cb_data *data;
        int i;

        data = malloc(sizeof(*data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory.");
                return -1;
        }
        memset(data, 0, sizeof(*data));

        data->nfs          = nfs;
        data->cb           = cb;
        data->private_data = private_data;

        memset(op, 0, sizeof(op));

        i = nfs4_op_putfh(nfs, &op[0], fh);
        i += nfs4_op_commit(nfs, &op[i]);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_fsync_cb, &args,
                                    data) != 0) {
                data->filler.blob0.val = NULL;
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

int
nfs4_ftruncate_async(struct nfs_context *nfs, struct nfsfh *fh,
                     uint64_t length, nfs_cb cb, void *private_data)
{
        COMPOUND4args args;
        nfs_argop4 op[2];
        struct nfs4_cb_data *data;
        int i;

        data = malloc(sizeof(*data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory.");
                return -1;
        }
        memset(data, 0, sizeof(*data));

        data->nfs          = nfs;
        data->cb           = cb;
        data->private_data = private_data;

        data->filler.blob3.val = malloc(12);
        if (data->filler.blob3.val == NULL) {
                nfs_set_error(nfs, "Out of memory");
                free_nfs4_cb_data(data);
                return -1;
        }
        data->filler.blob3.free = free;

        memset(data->filler.blob3.val, 0, 12);
        length = nfs_hton64(length);
        memcpy(data->filler.blob3.val, &length, sizeof(uint64_t));
        
        memset(op, 0, sizeof(op));

        i = nfs4_op_putfh(nfs, &op[0], fh);
        i += nfs4_op_truncate(nfs, &op[i], fh, data->filler.blob3.val);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_fsync_cb, &args,
                                    data) != 0) {
                data->filler.blob0.val = NULL;
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_lseek_cb(struct rpc_context *rpc, int status, void *command_data,
              void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        GETATTR4resok *garesok = NULL;
        struct nfsfh *fh = data->filler.blob0.val;
        struct nfs_stat_64 st;
        uint64_t offset;
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        memcpy(&offset, data->filler.blob1.val, sizeof(uint64_t));
        
        if (check_nfs4_error(nfs, status, data, res, "LSEEK")) {
                return;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_GETATTR, "GETATTR")) < 0) {
                return;
        }
        garesok = &res->resarray.resarray_val[i].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;

        memset(&st, 0, sizeof(st));
        nfs_parse_attributes(nfs, data, &st,
                             garesok->obj_attributes.attr_vals.attrlist4_val,
                             garesok->obj_attributes.attr_vals.attrlist4_len);

	if (offset < 0 &&
	    -offset > st.nfs_size) {
                nfs_set_error(nfs, "Negative offset for lseek("
                              "SEET_END)");
		data->cb(-EINVAL, nfs, &fh->offset,
                         data->private_data);
	} else {
		fh->offset = offset + st.nfs_size;
		data->cb(0, nfs, &fh->offset, data->private_data);
	}

        free_nfs4_cb_data(data);
}

/* blob0.val is nfsfh
 * blob1.val is offset
 */
int
nfs4_lseek_async(struct nfs_context *nfs, struct nfsfh *fh, int64_t offset,
                 int whence, nfs_cb cb, void *private_data)
{
        COMPOUND4args args;
        nfs_argop4 op[2];
        struct nfs4_cb_data *data;
        int i;

	if (whence == SEEK_SET) {
		if (offset < 0) {
                        nfs_set_error(nfs, "Negative offset for lseek("
                                      "SEET_SET)");
			cb(-EINVAL, nfs, &fh->offset, private_data);
		} else {
			fh->offset = offset;
			cb(0, nfs, &fh->offset, private_data);
		}
		return 0;
	}
	if (whence == SEEK_CUR) {
		if (offset < 0 &&
		    fh->offset < (uint64_t)(-offset)) {
                        nfs_set_error(nfs, "Negative offset for lseek("
                                      "SEET_CUR)");
			cb(-EINVAL, nfs, &fh->offset, private_data);
		} else {
			fh->offset += offset;
			cb(0, nfs, &fh->offset, private_data);
		}
		return 0;
	}

        data = malloc(sizeof(*data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory.");
                return -1;
        }
        memset(data, 0, sizeof(*data));

        data->nfs          = nfs;
        data->cb           = cb;
        data->private_data = private_data;

        data->filler.blob0.val  = fh;
        data->filler.blob0.free = NULL;

        data->filler.blob1.val = malloc(sizeof(uint64_t));
        if (data->filler.blob1.val == NULL) {
                nfs_set_error(nfs, "Out of memory.");
                free_nfs4_cb_data(data);
                return -1;
        }
        memcpy(data->filler.blob1.val, &offset, sizeof(uint64_t));

        i = nfs4_op_putfh(nfs, &op[0], fh);
        i += nfs4_op_getattr(nfs, &op[i], standard_attributes, 2);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_lseek_cb, &args,
                                    data) != 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static int
nfs_parse_statvfs(struct nfs_context *nfs, struct nfs4_cb_data *data,
                  struct statvfs *svfs, const char *buf, int len)
{
        uint64_t u64;
        uint32_t u32;

	svfs->f_bsize   = NFS_BLKSIZE;
	svfs->f_frsize  = NFS_BLKSIZE;

#if !defined(__ANDROID__)
	svfs->f_flag    = 0;
#endif

        /* FSID
         * NFSv4 FSID is 2*64 bit but statvfs fsid is just an
         * unsigmed long. Mix the 2*64 bits and hope for the best.
         */
        CHECK_GETATTR_BUF_SPACE(len, 16);
        memcpy(&u64, buf, 8);
	svfs->f_fsid = nfs_ntoh64(u64);
        buf += 8;
        len -= 8;
        memcpy(&u64, buf, 8);
	svfs->f_fsid |= nfs_ntoh64(u64);
        buf += 8;
        len -= 8;

        /* Files Avail */
        CHECK_GETATTR_BUF_SPACE(len, 8);
        memcpy(&u64, buf, 8);
#if !defined(__ANDROID__)
	svfs->f_favail  = nfs_ntoh64(u64);
#endif
        buf += 8;
        len -= 8;

        /* Files Free */
        CHECK_GETATTR_BUF_SPACE(len, 8);
        memcpy(&u64, buf, 8);
	svfs->f_ffree  = nfs_ntoh64(u64);
        buf += 8;
        len -= 8;

        /* Files Total */
        CHECK_GETATTR_BUF_SPACE(len, 8);
        memcpy(&u64, buf, 8);
	svfs->f_files  = nfs_ntoh64(u64);
        buf += 8;
        len -= 8;

        /* Max Name */
        CHECK_GETATTR_BUF_SPACE(len, 4);
        memcpy(&u32, buf, 4);
#if !defined(__ANDROID__)
	svfs->f_namemax  = ntohl(u32);
#endif
        buf += 4;
        len -= 4;

        /* Space Avail */
        CHECK_GETATTR_BUF_SPACE(len, 8);
        memcpy(&u64, buf, 8);
	svfs->f_bavail  = nfs_ntoh64(u64) / svfs->f_frsize;
        buf += 8;
        len -= 8;

        /* Space Free */
        CHECK_GETATTR_BUF_SPACE(len, 8);
        memcpy(&u64, buf, 8);
	svfs->f_bfree  = nfs_ntoh64(u64) / svfs->f_frsize;
        buf += 8;
        len -= 8;

        /* Space Total */
        CHECK_GETATTR_BUF_SPACE(len, 8);
        memcpy(&u64, buf, 8);
	svfs->f_blocks  = nfs_ntoh64(u64) / svfs->f_frsize;
        buf += 8;
        len -= 8;

        return 0;
}

static void
nfs4_statvfs_cb(struct rpc_context *rpc, int status, void *command_data,
              void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        GETATTR4resok *garesok;
	struct statvfs svfs;
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "STATVFS")) {
                return;
        }

        memset(&svfs, 0, sizeof(svfs));

        if ((i = nfs4_find_op(nfs, data, res, OP_GETATTR, "GETATTR")) < 0) {
                return;
        }
        garesok = &res->resarray.resarray_val[i].nfs_resop4_u.opgetattr.GETATTR4res_u.resok4;

        if (nfs_parse_statvfs(nfs, data, &svfs,
                              garesok->obj_attributes.attr_vals.attrlist4_val,
                              garesok->obj_attributes.attr_vals.attrlist4_len) < 0) {
                data->cb(-EINVAL, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }

	data->cb(0, nfs, &svfs, data->private_data);
	free_nfs4_cb_data(data);
}

int
nfs4_statvfs_async(struct nfs_context *nfs, const char *path, nfs_cb cb,
                   void *private_data)
{
        struct nfs4_cb_data *data;
        COMPOUND4args args;
        struct nfsfh fh;
        nfs_argop4 op[2];
        int i;

        data = malloc(sizeof(*data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory.");
                return -1;
        }
        memset(data, 0, sizeof(*data));

        data->nfs          = nfs;
        data->cb           = cb;
        data->private_data = private_data;

        fh.fh.len = nfs->rootfh.len;
        fh.fh.val = nfs->rootfh.val;

        i = nfs4_op_putfh(nfs, &op[0], &fh);
        i += nfs4_op_getattr(nfs, &op[i], statvfs_attributes, 2);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_statvfs_cb, &args,
                                    data) != 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_chmod_open_cb(struct rpc_context *rpc, int status, void *command_data,
                   void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        struct nfsfh *fh = data->filler.blob0.val;
        COMPOUND4res *res = command_data;
        COMPOUND4args args;
        nfs_argop4 op[4];
        int i;

        if (check_nfs4_error(nfs, status, data, res, "OPEN")) {
                return;
        }

        i = nfs4_op_putfh(nfs, &op[0], fh);
        i += nfs4_op_chmod(nfs, &op[i], fh, data->filler.blob3.val);
        i += nfs4_op_close(nfs, &op[i], fh);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_close_cb, &args,
                                    data) != 0) {
                /* Not much we can do but leak one fd on the server :( */
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
}

int
nfs4_chmod_async_internal(struct nfs_context *nfs, const char *path,
                          int no_follow, int mode, nfs_cb cb,
                          void *private_data)
{
        struct nfs4_cb_data *data;
        uint32_t m;

        data = init_cb_data_split_path(nfs, path);
        if (data == NULL) {
                return -1;
        }

        data->cb           = cb;
        data->private_data = private_data;
        data->open_cb      = nfs4_chmod_open_cb;

        if (no_follow) {
                data->flags |= LOOKUP_FLAG_NO_FOLLOW;
        }

        data->filler.blob3.val = malloc(sizeof(uint32_t));
        if (data->filler.blob3.val == NULL) {
                nfs_set_error(nfs, "Out of memory");
                free_nfs4_cb_data(data);
                return -1;
        }
        data->filler.blob3.free = free;

        m = htonl(mode);
        memcpy(data->filler.blob3.val, &m, sizeof(uint32_t));

        if (nfs4_open_async_internal(nfs, data, O_WRONLY, 0) < 0) {
                return -1;
        }

        return 0;
}

int
nfs4_fchmod_async(struct nfs_context *nfs, struct nfsfh *fh, int mode,
                  nfs_cb cb, void *private_data)
{
        COMPOUND4args args;
        nfs_argop4 op[2];
        struct nfs4_cb_data *data;
        uint32_t m;
        int i;

        data = malloc(sizeof(*data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory.");
                return -1;
        }
        memset(data, 0, sizeof(*data));

        data->nfs          = nfs;
        data->cb           = cb;
        data->private_data = private_data;

        data->filler.blob3.val = malloc(sizeof(uint32_t));
        if (data->filler.blob3.val == NULL) {
                nfs_set_error(nfs, "Out of memory");
                free_nfs4_cb_data(data);
                return -1;
        }
        data->filler.blob3.free = free;

        m = htonl(mode);
        memcpy(data->filler.blob3.val, &m, sizeof(uint32_t));
        
        memset(op, 0, sizeof(op));

        i = nfs4_op_putfh(nfs, &op[0], fh);
        i += nfs4_op_chmod(nfs, &op[i], fh, data->filler.blob3.val);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_fsync_cb, &args,
                                    data) != 0) {
                data->filler.blob0.val = NULL;
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

#define CHOWN_BLOB_SIZE 64
static int
nfs4_create_chown_buffer(struct nfs_context *nfs, struct nfs4_cb_data *data,
                         int uid, int gid)
{
        char *str;
        int i, l;
        uint32_t len;

        data->filler.blob3.val = malloc(CHOWN_BLOB_SIZE);
        if (data->filler.blob3.val == NULL) {
                nfs_set_error(nfs, "Out of memory");
                return -1;
        }
        data->filler.blob3.free = free;
        memset(data->filler.blob3.val, 0, CHOWN_BLOB_SIZE);
        
        i = 0;
        str = data->filler.blob3.val;
        /* UID */
        l = snprintf(&str[i + 4], CHOWN_BLOB_SIZE - 4 - i,
                     "%d", uid);
        if (l < 0) {
                nfs_set_error(nfs, "snprintf failed");
                return -1;
        }
        len = htonl(l);
        /* UID length prefix */
        memcpy(&str[i], &len, sizeof(uint32_t));
        i += 4 + l;
        i = (i + 3) & ~0x03;

        /* GID */
        l = snprintf(&str[i + 4], CHOWN_BLOB_SIZE - 4 - i,
                     "%d", gid);
        if (l < 0) {
                nfs_set_error(nfs, "snprintf failed");
                return -1;
        }
        len = htonl(l);
        /* GID length prefix */
        memcpy(&str[i], &len, sizeof(uint32_t));
        i += 4 + l;
        i = (i + 3) & ~0x03;

        data->filler.blob3.len = i;

        return 0;
}

static void
nfs4_chown_open_cb(struct rpc_context *rpc, int status, void *command_data,
                   void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        struct nfsfh *fh = data->filler.blob0.val;
        COMPOUND4res *res = command_data;
        COMPOUND4args args;
        nfs_argop4 op[4];
        int i;

        if (check_nfs4_error(nfs, status, data, res, "OPEN")) {
                return;
        }

        i = nfs4_op_putfh(nfs, &op[0], fh);
        i += nfs4_op_chown(nfs, &op[i], fh, data->filler.blob3.val,
                           data->filler.blob3.len);
        i += nfs4_op_close(nfs, &op[i], fh);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_close_cb, &args,
                                    data) != 0) {
                /* Not much we can do but leak one fd on the server :( */
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
}

int
nfs4_chown_async_internal(struct nfs_context *nfs, const char *path,
                          int no_follow, int uid, int gid,
                          nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;

        data = init_cb_data_split_path(nfs, path);
        if (data == NULL) {
                return -1;
        }

        data->cb           = cb;
        data->private_data = private_data;
        data->open_cb      = nfs4_chown_open_cb;

        if (no_follow) {
                data->flags |= LOOKUP_FLAG_NO_FOLLOW;
        }

        if (nfs4_create_chown_buffer(nfs, data, uid, gid) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        if (nfs4_open_async_internal(nfs, data, O_WRONLY, 0) < 0) {
                return -1;
        }

        return 0;
}

int
nfs4_fchown_async(struct nfs_context *nfs, struct nfsfh *fh, int uid, int gid,
                  nfs_cb cb, void *private_data)
{
        COMPOUND4args args;
        nfs_argop4 op[2];
        struct nfs4_cb_data *data;
        int i;

        data = malloc(sizeof(*data));
        if (data == NULL) {
                nfs_set_error(nfs, "Out of memory.");
                return -1;
        }
        memset(data, 0, sizeof(*data));

        data->nfs          = nfs;
        data->cb           = cb;
        data->private_data = private_data;

        if (nfs4_create_chown_buffer(nfs, data, uid, gid) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }
        
        memset(op, 0, sizeof(op));

        i = nfs4_op_putfh(nfs, &op[0], fh);
        i += nfs4_op_chown(nfs, &op[i], fh, data->filler.blob3.val,
                           data->filler.blob3.len);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_fsync_cb, &args,
                                    data) != 0) {
                data->filler.blob0.val = NULL;
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

static void
nfs4_access_cb(struct rpc_context *rpc, int status, void *command_data,
               void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        COMPOUND4res *res = command_data;
        ACCESS4resok *aresok;
        int i;

        assert(rpc->magic == RPC_CONTEXT_MAGIC);

        if (check_nfs4_error(nfs, status, data, res, "ACCESS")) {
                return;
        }

        if ((i = nfs4_find_op(nfs, data, res, OP_ACCESS, "ACCESS")) < 0) {
                return;
        }

        aresok = &res->resarray.resarray_val[i].nfs_resop4_u.opaccess.ACCESS4res_u.resok4;

        /* access2 */
        if (data->filler.flags) {
                int mode = 0;

                if (aresok->access & ACCESS4_READ) {
                        mode |= R_OK;
                }
                if (aresok->access & ACCESS4_MODIFY) {
                        mode |= W_OK;
                }
                if (aresok->access & ACCESS4_EXECUTE) {
                        mode |= X_OK;
                }
                data->cb(mode, nfs, NULL, data->private_data);
                free_nfs4_cb_data(data);
                return;
        }

        if (aresok->supported != aresok->access) {
                data->cb(-EACCES, nfs, NULL, data->private_data);
                free_nfs4_cb_data(data);
                return;
        }

        data->cb(0, nfs, NULL, data->private_data);
        free_nfs4_cb_data(data);
}

static int
nfs4_access_internal(struct nfs_context *nfs, const char *path, int mode,
                     int is_access2, nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;
        uint32_t m;

        data = init_cb_data_full_path(nfs, path);
        if (data == NULL) {
                return -1;
        }

        data->cb            = cb;
        data->private_data  = private_data;
        data->filler.func   = nfs4_populate_access;
        data->filler.max_op = 1;
        data->filler.flags = is_access2;

        data->filler.blob3.val = malloc(sizeof(uint32_t));
        if (data->filler.blob3.val == NULL) {
                nfs_set_error(nfs, "Out of memory");
                return -1;
        }
        data->filler.blob3.free = free;

        m = 0;
        if (mode & R_OK) {
                m |= ACCESS4_READ;
        }
        if (mode & W_OK) {
                m |= ACCESS4_MODIFY;
        }
        if (mode & X_OK) {
                m |= ACCESS4_EXECUTE;
        }
        memcpy(data->filler.blob3.val, &m, sizeof(uint32_t));

        if (nfs4_lookup_path_async(nfs, data, nfs4_access_cb) < 0) {
                free_nfs4_cb_data(data);
                return -1;
        }

        return 0;
}

int
nfs4_access_async(struct nfs_context *nfs, const char *path, int mode,
                  nfs_cb cb, void *private_data)
{
        return nfs4_access_internal(nfs, path, mode, 0,
                                    cb, private_data);
}

int
nfs4_access2_async(struct nfs_context *nfs, const char *path, nfs_cb cb,
                   void *private_data)
{
        return nfs4_access_internal(nfs, path, R_OK|W_OK|X_OK, 1,
                                    cb, private_data);
}

static void
nfs4_utimes_open_cb(struct rpc_context *rpc, int status, void *command_data,
                   void *private_data)
{
        struct nfs4_cb_data *data = private_data;
        struct nfs_context *nfs = data->nfs;
        struct nfsfh *fh = data->filler.blob0.val;
        COMPOUND4res *res = command_data;
        COMPOUND4args args;
        nfs_argop4 op[4];
        int i;

        if (check_nfs4_error(nfs, status, data, res, "OPEN")) {
                return;
        }

        i = nfs4_op_putfh(nfs, &op[0], fh);
        i += nfs4_op_utimes(nfs, &op[i], fh, data->filler.blob3.val,
                            data->filler.blob3.len);
        i += nfs4_op_close(nfs, &op[i], fh);

        memset(&args, 0, sizeof(args));
        args.argarray.argarray_len = i;
        args.argarray.argarray_val = op;

        if (rpc_nfs4_compound_async(nfs->rpc, nfs4_close_cb, &args,
                                    data) != 0) {
                /* Not much we can do but leak one fd on the server :( */
                data->cb(-ENOMEM, nfs, nfs_get_error(nfs), data->private_data);
                free_nfs4_cb_data(data);
                return;
        }
}

int
nfs4_utimes_async_internal(struct nfs_context *nfs, const char *path,
                           int no_follow, struct timeval *times,
                           nfs_cb cb, void *private_data)
{
        struct nfs4_cb_data *data;
        char *buf;
        uint32_t u32;
        uint64_t u64;

        data = init_cb_data_split_path(nfs, path);
        if (data == NULL) {
                return -1;
        }

        data->cb           = cb;
        data->private_data = private_data;
        data->open_cb      = nfs4_utimes_open_cb;

        if (no_follow) {
                data->flags |= LOOKUP_FLAG_NO_FOLLOW;
        }

        data->filler.blob3.len = 2 * (4 + 8 + 4);
        buf = data->filler.blob3.val = malloc(data->filler.blob3.len);
        if (data->filler.blob3.val == NULL) {
                nfs_set_error(nfs, "Out of memory");
                return -1;
        }
        data->filler.blob3.free = free;

        /* atime */
        u32 = htonl(SET_TO_CLIENT_TIME4);
        memcpy(buf, &u32, sizeof(uint32_t));
        u64 = nfs_hton64(times[0].tv_sec);
        memcpy(buf + 4, &u64, sizeof(uint64_t));
        u32 = htonl(times[0].tv_usec * 1000);
        memcpy(buf + 12, &u32, sizeof(uint32_t));
        buf += 16;
        /* mtime */
        u32 = htonl(SET_TO_CLIENT_TIME4);
        memcpy(buf, &u32, sizeof(uint32_t));
        u64 = nfs_hton64(times[1].tv_sec);
        memcpy(buf + 4, &u64, sizeof(uint64_t));
        u32 = htonl(times[1].tv_usec * 1000);
        memcpy(buf + 12, &u32, sizeof(uint32_t));

        if (nfs4_open_async_internal(nfs, data, O_WRONLY, 0) < 0) {
                return -1;
        }

        return 0;
}
