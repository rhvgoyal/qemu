/*
 * virtio-fs glue for FUSE
 * Copyright (C) 2018 Red Hat, Inc. and/or its affiliates
 *
 * Authors:
 *   Dave Gilbert  <dgilbert@redhat.com>
 *
 * Implements the glue between libfuse and libvhost-user
 *
 * This program can be distributed under the terms of the GNU LGPLv2.
 * See the file COPYING.LIB
 */

#include "qemu/osdep.h"
#include "qemu/iov.h"
#include "qapi/error.h"
#include "standard-headers/linux/virtio_fs.h"
#include "fuse_i.h"
#include "fuse_kernel.h"
#include "fuse_misc.h"
#include "fuse_opt.h"
#include "fuse_virtio.h"

#include <assert.h>
#include <errno.h>
#include <glib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "contrib/libvhost-user/libvhost-user.h"

struct fv_VuDev;
struct fv_QueueInfo {
    pthread_t thread;
    /*
     * This lock protects the VuVirtq preventing races between
     * fv_queue_thread() and fv_queue_worker().
     */
    pthread_mutex_t vq_lock;

    struct fv_VuDev *virtio_dev;

    /* Our queue index, corresponds to array position */
    int qidx;
    int kick_fd;
    int kill_fd; /* For killing the thread */

    /* The element for the command currently being processed */
    VuVirtqElement *qe;
    /* If any of the qe vec elements (towards vmm) are unmappable */
    unsigned int elem_bad_in;
    bool reply_sent;
};

/* A FUSE request */
typedef struct {
    VuVirtqElement elem;
    struct fuse_chan ch;

    /* Number of unmappable iovecs */
    unsigned int bad_in_num;
    unsigned int bad_out_num;

    /* Used to complete requests that involve no reply */
    bool reply_sent;
} FVRequest;

/*
 * We pass the dev element into libvhost-user
 * and then use it to get back to the outer
 * container for other data.
 */
struct fv_VuDev {
    VuDev dev;
    struct fuse_session *se;

    /*
     * Either handle virtqueues or vhost-user protocol messages.  Don't do
     * both at the same time since that could lead to race conditions if
     * virtqueues or memory tables change while another thread is accessing
     * them.
     *
     * The assumptions are:
     * 1. fv_queue_thread() reads/writes to virtqueues and only reads VuDev.
     * 2. virtio_loop() reads/writes virtqueues and VuDev.
     */
    pthread_rwlock_t vu_dispatch_rwlock;

    /*
     * The following pair of fields are only accessed in the main
     * virtio_loop
     */
    size_t nqueues;
    struct fv_QueueInfo **qi;
    /* True if notification queue is being used */
    bool notify_enabled;
};

/* Callback from libvhost-user */
static uint64_t fv_get_features(VuDev *dev)
{
    uint64_t features;

    features = 1ull << VIRTIO_F_VERSION_1 |
               1ull << VIRTIO_FS_F_NOTIFICATION |
               1ull << VHOST_USER_F_PROTOCOL_FEATURES;

    return features;
}

/* Callback from libvhost-user */
static void fv_set_features(VuDev *dev, uint64_t features)
{
    struct fv_VuDev *vud = container_of(dev, struct fv_VuDev, dev);
    struct fuse_session *se = vud->se;

    if ((1 << VIRTIO_FS_F_NOTIFICATION) & features) {
        vud->notify_enabled = true;
        se->notify_enabled = true;
    }
}

/*
 * Callback from libvhost-user if there's a new fd we're supposed to listen
 * to, typically a queue kick?
 */
static void fv_set_watch(VuDev *dev, int fd, int condition, vu_watch_cb cb,
                         void *data)
{
    fuse_log(FUSE_LOG_WARNING, "%s: TODO! fd=%d\n", __func__, fd);
}

/*
 * Callback from libvhost-user if we're no longer supposed to listen on an fd
 */
static void fv_remove_watch(VuDev *dev, int fd)
{
    fuse_log(FUSE_LOG_WARNING, "%s: TODO! fd=%d\n", __func__, fd);
}

/* Callback from libvhost-user to panic */
static void fv_panic(VuDev *dev, const char *err)
{
    fuse_log(FUSE_LOG_ERR, "%s: libvhost-user: %s\n", __func__, err);
    /* TODO: Allow reconnects?? */
    exit(EXIT_FAILURE);
}

/*
 * Copy from an iovec into a fuse_buf (memory only)
 * Caller must ensure there is space
 */
static void copy_from_iov(struct fuse_buf *buf, size_t out_num,
                          const struct iovec *out_sg)
{
    void *dest = buf->mem;

    while (out_num) {
        size_t onelen = out_sg->iov_len;
        memcpy(dest, out_sg->iov_base, onelen);
        dest += onelen;
        out_sg++;
        out_num--;
    }
}

/*
 * Copy from one iov to another, the given number of bytes
 * The caller must have checked sizes.
 */
static void copy_iov(struct iovec *src_iov, int src_count,
                     struct iovec *dst_iov, int dst_count, size_t to_copy)
{
    size_t dst_offset = 0;
    /* Outer loop copies 'src' elements */
    while (to_copy) {
        assert(src_count);
        size_t src_len = src_iov[0].iov_len;
        size_t src_offset = 0;

        if (src_len > to_copy) {
            src_len = to_copy;
        }
        /* Inner loop copies contents of one 'src' to maybe multiple dst. */
        while (src_len) {
            assert(dst_count);
            size_t dst_len = dst_iov[0].iov_len - dst_offset;
            if (dst_len > src_len) {
                dst_len = src_len;
            }

            memcpy(dst_iov[0].iov_base + dst_offset,
                   src_iov[0].iov_base + src_offset, dst_len);
            src_len -= dst_len;
            to_copy -= dst_len;
            src_offset += dst_len;
            dst_offset += dst_len;

            assert(dst_offset <= dst_iov[0].iov_len);
            if (dst_offset == dst_iov[0].iov_len) {
                dst_offset = 0;
                dst_iov++;
                dst_count--;
            }
        }
        src_iov++;
        src_count--;
    }
}

static int virtio_send_notify_msg(struct fuse_session *se, struct iovec *iov,
				  int count)
{
    struct fv_QueueInfo *qi;
    VuDev *dev = &se->virtio_dev->dev;
    VuVirtq *q;
    FVRequest *req;
    VuVirtqElement *elem;
    unsigned int in_num, bad_in_num = 0, bad_out_num = 0;
    struct fuse_out_header *out = iov[0].iov_base;
    size_t in_len, tosend_len = iov_size(iov, count);
    struct iovec *in_sg;
    int ret = 0;

    /* Notifications have unique == 0 */
    assert (!out->unique);

    if (!se->notify_enabled)
        return -EOPNOTSUPP;

    /* If notifications are enabled, queue index 1 is notification queue */
    qi = se->virtio_dev->qi[1];
    q = vu_get_queue(dev, qi->qidx);

    pthread_rwlock_rdlock(&qi->virtio_dev->vu_dispatch_rwlock);
    pthread_mutex_lock(&qi->vq_lock);
    /* Pop an element from queue */
    req = vu_queue_pop(dev, q, sizeof(FVRequest), &bad_in_num, &bad_out_num);
    if (!req) {
        /* TODO: Implement some sort of ring buffer and queue notifications
	 * on that and send these later when notification queue has space
	 * available.
	 */
        return -ENOSPC;
    }
    pthread_mutex_unlock(&qi->vq_lock);
    pthread_rwlock_unlock(&qi->virtio_dev->vu_dispatch_rwlock);

    out->len = tosend_len;
    elem = &req->elem;
    in_num = elem->in_num;
    in_sg = elem->in_sg;
    in_len = iov_size(in_sg, in_num);
    fuse_log(FUSE_LOG_DEBUG, "%s: elem %d: with %d in desc of length %zd\n",
             __func__, elem->index, in_num,  in_len);

    if (in_len < sizeof(struct fuse_out_header)) {
        fuse_log(FUSE_LOG_ERR, "%s: elem %d too short for out_header\n",
                 __func__, elem->index);
        ret = -E2BIG;
        goto out;
    }

    if (in_len < tosend_len) {
        fuse_log(FUSE_LOG_ERR, "%s: elem %d too small for data len"
                 " %zd\n", __func__, elem->index, tosend_len);
        ret = -E2BIG;
        goto out;
    }

    /* First copy the header data from iov->in_sg */
    copy_iov(iov, count, in_sg, in_num, tosend_len);

    /* TODO: Add bad_innum handling */
    pthread_rwlock_rdlock(&qi->virtio_dev->vu_dispatch_rwlock);
    pthread_mutex_lock(&qi->vq_lock);
    vu_queue_push(dev, q, elem, tosend_len);
    vu_queue_notify(dev, q);
    pthread_mutex_unlock(&qi->vq_lock);
    pthread_rwlock_unlock(&qi->virtio_dev->vu_dispatch_rwlock);
out:
    free(req);
    return ret;
}

/*
 * Called back by ll whenever it wants to send a reply/message back
 * The 1st element of the iov starts with the fuse_out_header
 * 'unique'==0 means it's a notify message.
 */
int virtio_send_msg(struct fuse_session *se, struct fuse_chan *ch,
                    struct iovec *iov, int count)
{
    FVRequest *req;
    struct fv_QueueInfo *qi;
    VuDev *dev = &se->virtio_dev->dev;
    VuVirtq *q;
    VuVirtqElement *elem;
    int ret = 0;

    assert(count >= 1);
    assert(iov[0].iov_len >= sizeof(struct fuse_out_header));

    struct fuse_out_header *out = iov[0].iov_base;
    /* TODO: Endianness! */

    size_t tosend_len = iov_size(iov, count);

    /* unique == 0 is notification */
    if (!out->unique)
        return virtio_send_notify_msg(se, iov, count);

    assert(ch);
    req = container_of(ch, FVRequest, ch);
    elem = &req->elem;
    qi = ch->qi;
    q = vu_get_queue(dev, qi->qidx);
    assert(!req->reply_sent);

    /* The 'in' part of the elem is to qemu */
    unsigned int in_num = elem->in_num;
    struct iovec *in_sg = elem->in_sg;
    size_t in_len = iov_size(in_sg, in_num);
    fuse_log(FUSE_LOG_DEBUG, "%s: elem %d: with %d in desc of length %zd\n",
             __func__, elem->index, in_num, in_len);

    /*
     * The elem should have room for a 'fuse_out_header' (out from fuse)
     * plus the data based on the len in the header.
     */
    if (in_len < sizeof(struct fuse_out_header)) {
        fuse_log(FUSE_LOG_ERR, "%s: elem %d too short for out_header\n",
                 __func__, elem->index);
        ret = -E2BIG;
        goto err;
    }
    if (in_len < tosend_len) {
        fuse_log(FUSE_LOG_ERR, "%s: elem %d too small for data len %zd\n",
                 __func__, elem->index, tosend_len);
        ret = -E2BIG;
        goto err;
    }

    copy_iov(iov, count, in_sg, in_num, tosend_len);

    pthread_rwlock_rdlock(&qi->virtio_dev->vu_dispatch_rwlock);
    pthread_mutex_lock(&qi->vq_lock);
    vu_queue_push(dev, q, elem, tosend_len);
    vu_queue_notify(dev, q);
    pthread_mutex_unlock(&qi->vq_lock);
    pthread_rwlock_unlock(&qi->virtio_dev->vu_dispatch_rwlock);

    req->reply_sent = true;

err:
    return ret;
}

/*
 * Callback from fuse_send_data_iov_* when it's virtio and the buffer
 * is a single FD with FUSE_BUF_IS_FD | FUSE_BUF_FD_SEEK
 * We need send the iov and then the buffer.
 * Return 0 on success
 */
int virtio_send_data_iov(struct fuse_session *se, struct fuse_chan *ch,
                         struct iovec *iov, int count, struct fuse_bufvec *buf,
                         size_t len)
{
    FVRequest *req = container_of(ch, FVRequest, ch);
    struct fv_QueueInfo *qi = ch->qi;
    VuDev *dev = &se->virtio_dev->dev;
    VuVirtq *q = vu_get_queue(dev, qi->qidx);
    VuVirtqElement *elem = &req->elem;
    int ret = 0;

    assert(count >= 1);
    assert(iov[0].iov_len >= sizeof(struct fuse_out_header));

    struct fuse_out_header *out = iov[0].iov_base;
    /* TODO: Endianness! */

    size_t iov_len = iov_size(iov, count);
    size_t tosend_len = iov_len + len;

    out->len = tosend_len;

    fuse_log(FUSE_LOG_DEBUG, "%s: count=%d len=%zd iov_len=%zd\n", __func__,
             count, len, iov_len);

    /* unique == 0 is notification which we don't support */
    assert(out->unique);

    assert(!req->reply_sent);

    /* The 'in' part of the elem is to qemu */
    unsigned int in_num = elem->in_num;
    unsigned int bad_in_num = req->bad_in_num;
    struct iovec *in_sg = elem->in_sg;
    size_t in_len = iov_size(in_sg, in_num);
    size_t in_len_writeable = iov_size(in_sg, in_num - bad_in_num);
    fuse_log(FUSE_LOG_DEBUG, "%s: elem %d: with %d in desc of length %zd\n",
             __func__, elem->index, in_num, in_len);

    /*
     * The elem should have room for a 'fuse_out_header' (out from fuse)
     * plus the data based on the len in the header.
     */
    if (in_len_writeable < sizeof(struct fuse_out_header)) {
        fuse_log(FUSE_LOG_ERR, "%s: elem %d too short for out_header\n",
                 __func__, elem->index);
        ret = -E2BIG;
        goto err;
    }
    if (in_len < tosend_len) {
        fuse_log(FUSE_LOG_ERR, "%s: elem %d too small for data len %zd\n",
                 __func__, elem->index, tosend_len);
        ret = -E2BIG;
        goto err;
    }

    /* TODO: Limit to 'len' */

    /* First copy the header data from iov->in_sg */
    copy_iov(iov, count, in_sg, in_num, iov_len);

    /*
     * Build a copy of the the in_sg iov so we can skip bits in it,
     * including changing the offsets
     */
    struct iovec *in_sg_cpy = calloc(sizeof(struct iovec), in_num);
    memcpy(in_sg_cpy, in_sg, sizeof(struct iovec) * in_num);
    /* These get updated as we skip */
    struct iovec *in_sg_ptr = in_sg_cpy;
    int in_sg_cpy_count = in_num - bad_in_num;

    /* skip over parts of in_sg that contained the header iov */
    size_t skip_size = iov_len;

    size_t in_sg_left = 0;
    do {
        while (skip_size != 0 && in_sg_cpy_count) {
            if (skip_size >= in_sg_ptr[0].iov_len) {
                skip_size -= in_sg_ptr[0].iov_len;
                in_sg_ptr++;
                in_sg_cpy_count--;
            } else {
                in_sg_ptr[0].iov_len -= skip_size;
                in_sg_ptr[0].iov_base += skip_size;
                break;
            }
        }

        int i;
        for (i = 0, in_sg_left = 0; i < in_sg_cpy_count; i++) {
            in_sg_left += in_sg_ptr[i].iov_len;
        }
        fuse_log(FUSE_LOG_DEBUG,
                 "%s: after skip skip_size=%zd in_sg_cpy_count=%d "
                 "in_sg_left=%zd\n",
                 __func__, skip_size, in_sg_cpy_count, in_sg_left);
        ret = preadv(buf->buf[0].fd, in_sg_ptr, in_sg_cpy_count,
                     buf->buf[0].pos);

        fuse_log(FUSE_LOG_DEBUG, "%s: preadv_res=%d(%m) len=%zd\n",
                 __func__, ret, len);
        if (ret == -1) {
            ret = errno;
            free(in_sg_cpy);
            goto err;
        }
        if (ret < len && ret) {
            fuse_log(FUSE_LOG_DEBUG, "%s: ret < len\n", __func__);
            /* Skip over this much next time around */
            skip_size = ret;
            buf->buf[0].pos += ret;
            len -= ret;

            /* Lets do another read */
            continue;
        }
        if (!ret) {
            /* EOF case? */
            fuse_log(FUSE_LOG_DEBUG, "%s: !ret in_sg_left=%zd\n", __func__,
                     in_sg_left);
            break;
        }
        if (ret != len) {
            fuse_log(FUSE_LOG_DEBUG, "%s: ret!=len\n", __func__);
            ret = EIO;
            free(in_sg_cpy);
            goto err;
        }
        in_sg_left -= ret;
        len -= ret;
    } while (in_sg_left);

    if (bad_in_num) {
        while (len && bad_in_num) {
            VhostUserFSSlaveMsg msg = { 0 };
            msg.flags[0] = VHOST_USER_FS_FLAG_MAP_R;
            msg.fd_offset[0] = buf->buf[0].pos;
            msg.c_offset[0] = (uint64_t)(uintptr_t)in_sg_ptr[0].iov_base;
            msg.len[0] = in_sg_ptr[0].iov_len;
            if (len < msg.len[0]) {
                msg.len[0] = len;
            }
            int64_t req_res = fuse_virtio_io(se, &msg, buf->buf[0].fd);
            fuse_log(FUSE_LOG_DEBUG,
                     "%s: bad loop; len=%zd bad_in_num=%d fd_offset=%zd "
                     "c_offset=%p req_res=%ld\n",
                     __func__, len, bad_in_num, buf->buf[0].pos,
                     in_sg_ptr[0].iov_base, req_res);
            if (req_res > 0) {
                len -= msg.len[0];
                buf->buf[0].pos += msg.len[0];
                in_sg_ptr++;
                bad_in_num--;
            } else if (req_res == 0) {
                break;
            } else {
                ret = req_res;
                free(in_sg_cpy);
                goto err;
            }
        }
    }
    free(in_sg_cpy);

    /* Need to fix out->len on EOF */
    if (len) {
        struct fuse_out_header *out_sg = in_sg[0].iov_base;

        tosend_len -= len;
        out_sg->len = tosend_len;
    }

    ret = 0;

    pthread_rwlock_rdlock(&qi->virtio_dev->vu_dispatch_rwlock);
    pthread_mutex_lock(&qi->vq_lock);
    vu_queue_push(dev, q, elem, tosend_len);
    vu_queue_notify(dev, q);
    pthread_mutex_unlock(&qi->vq_lock);
    pthread_rwlock_unlock(&qi->virtio_dev->vu_dispatch_rwlock);

err:
    if (ret == 0) {
        req->reply_sent = true;
    }

    return ret;
}

/* Process one FVRequest in a thread pool */
static void fv_queue_worker(gpointer data, gpointer user_data)
{
    struct fv_QueueInfo *qi = user_data;
    struct fuse_session *se = qi->virtio_dev->se;
    struct VuDev *dev = &qi->virtio_dev->dev;
    FVRequest *req = data;
    VuVirtqElement *elem = &req->elem;
    struct fuse_buf fbuf = {};
    bool allocated_bufv = false;
    struct fuse_bufvec bufv;
    struct fuse_bufvec *pbufv;

    assert(se->bufsize > sizeof(struct fuse_in_header));

    /*
     * An element contains one request and the space to send our response
     * They're spread over multiple descriptors in a scatter/gather set
     * and we can't trust the guest to keep them still; so copy in/out.
     */
    fbuf.mem = malloc(se->bufsize);
    assert(fbuf.mem);

    fuse_mutex_init(&req->ch.lock);
    req->ch.fd = (int)0xdaff0d111;
    req->ch.ctr = 1;
    req->ch.qi = qi;

    /* The 'out' part of the elem is from qemu */
    unsigned int out_num = elem->out_num;
    unsigned int out_num_readable = out_num - req->bad_out_num;
    struct iovec *out_sg = elem->out_sg;
    size_t out_len = iov_size(out_sg, out_num);
    size_t out_len_readable = iov_size(out_sg, out_num_readable);
    fuse_log(FUSE_LOG_DEBUG,
             "%s: elem %d: with %d out desc of length %zd"
             " bad_in_num=%u bad_out_num=%u\n",
             __func__, elem->index, out_num, out_len, req->bad_in_num,
             req->bad_out_num);

    /*
     * The elem should contain a 'fuse_in_header' (in to fuse)
     * plus the data based on the len in the header.
     */
    if (out_len_readable < sizeof(struct fuse_in_header)) {
        fuse_log(FUSE_LOG_ERR, "%s: elem %d too short for in_header\n",
                 __func__, elem->index);
        assert(0); /* TODO */
    }
    if (out_len > se->bufsize) {
        fuse_log(FUSE_LOG_ERR, "%s: elem %d too large for buffer\n", __func__,
                 elem->index);
        assert(0); /* TODO */
    }
    /* Copy just the first element and look at it */
    copy_from_iov(&fbuf, 1, out_sg);

    pbufv = NULL; /* Compiler thinks an unitialised path */
    if (req->bad_in_num || req->bad_out_num) {
        bool handled_unmappable = false;

        if (out_num > 2 && out_num_readable >= 2 && !req->bad_in_num &&
            out_sg[0].iov_len == sizeof(struct fuse_in_header) &&
            ((struct fuse_in_header *)fbuf.mem)->opcode == FUSE_WRITE &&
            out_sg[1].iov_len == sizeof(struct fuse_write_in)) {
            handled_unmappable = true;

            /* copy the fuse_write_in header after the fuse_in_header */
            fbuf.mem += out_sg->iov_len;
            copy_from_iov(&fbuf, 1, out_sg + 1);
            fbuf.mem -= out_sg->iov_len;
            fbuf.size = out_sg[0].iov_len + out_sg[1].iov_len;

            /* Allocate the bufv, with space for the rest of the iov */
            allocated_bufv = true;
            pbufv = malloc(sizeof(struct fuse_bufvec) +
                           sizeof(struct fuse_buf) * (out_num - 2));

            pbufv->count = 1;
            pbufv->buf[0] = fbuf;

            size_t iovindex, pbufvindex;
            iovindex = 2; /* 2 headers, separate iovs */
            pbufvindex = 1; /* 2 headers, 1 fusebuf */

            for (; iovindex < out_num; iovindex++, pbufvindex++) {
                pbufv->count++;
                pbufv->buf[pbufvindex].pos = ~0; /* Dummy */
                pbufv->buf[pbufvindex].flags =
                    (iovindex < out_num_readable) ? 0 : FUSE_BUF_PHYS_ADDR;
                pbufv->buf[pbufvindex].mem = out_sg[iovindex].iov_base;
                pbufv->buf[pbufvindex].size = out_sg[iovindex].iov_len;
            }
        }

        if (out_num == 2 && out_num_readable == 2 && req->bad_in_num &&
            out_sg[0].iov_len == sizeof(struct fuse_in_header) &&
            ((struct fuse_in_header *)fbuf.mem)->opcode == FUSE_READ &&
            out_sg[1].iov_len == sizeof(struct fuse_read_in)) {
            fuse_log(FUSE_LOG_DEBUG,
                     "Unmappable read case "
                     "in_num=%d bad_in_num=%d\n",
                     elem->in_num, req->bad_in_num);
            handled_unmappable = true;
        }

        if (!handled_unmappable) {
            fuse_log(FUSE_LOG_ERR,
                     "Unhandled unmappable element: out: %d(b:%d) in: %d(b:%d)",
                     out_num, req->bad_out_num, elem->in_num, req->bad_in_num);
            fv_panic(dev, "Unhandled unmappable element");
        }
    }

    if (!req->bad_out_num) {
        if (out_num > 2 && out_sg[0].iov_len == sizeof(struct fuse_in_header) &&
            ((struct fuse_in_header *)fbuf.mem)->opcode == FUSE_WRITE &&
            out_sg[1].iov_len == sizeof(struct fuse_write_in)) {
            /*
             * For a write we don't actually need to copy the
             * data, we can just do it straight out of guest memory
             * but we must sitll copy the headers in case the guest
             * was nasty and changed them while we were using them.
             */
            fuse_log(FUSE_LOG_DEBUG, "%s: Write special case\n", __func__);

            /* copy the fuse_write_in header afte rthe fuse_in_header */
            fbuf.mem += out_sg->iov_len;
            copy_from_iov(&fbuf, 1, out_sg + 1);
            fbuf.mem -= out_sg->iov_len;
            fbuf.size = out_sg[0].iov_len + out_sg[1].iov_len;

            /* Allocate the bufv, with space for the rest of the iov */
            allocated_bufv = true;
            pbufv = malloc(sizeof(struct fuse_bufvec) +
                           sizeof(struct fuse_buf) * (out_num - 2));

            pbufv->count = 1;
            pbufv->buf[0] = fbuf;

            size_t iovindex, pbufvindex;
            iovindex = 2; /* 2 headers, separate iovs */
            pbufvindex = 1; /* 2 headers, 1 fusebuf */

            for (; iovindex < out_num; iovindex++, pbufvindex++) {
                pbufv->count++;
                pbufv->buf[pbufvindex].pos = ~0; /* Dummy */
                pbufv->buf[pbufvindex].flags = 0;
                pbufv->buf[pbufvindex].mem = out_sg[iovindex].iov_base;
                pbufv->buf[pbufvindex].size = out_sg[iovindex].iov_len;
            }
        } else {
            /* Normal (non fast write) path */

            /* Copy the rest of the buffer */
            fbuf.mem += out_sg->iov_len;
            copy_from_iov(&fbuf, out_num - 1, out_sg + 1);
            fbuf.mem -= out_sg->iov_len;
            fbuf.size = out_len;

            /* TODO! Endianness of header */

            /* TODO: Add checks for fuse_session_exited */
            bufv.buf[0] = fbuf;
            bufv.count = 1;
            pbufv = &bufv;
        }
    }
    pbufv->idx = 0;
    pbufv->off = 0;
    fuse_session_process_buf_int(se, pbufv, &req->ch);

    if (allocated_bufv) {
        free(pbufv);
    }

    /* If the request has no reply, still recycle the virtqueue element */
    if (!req->reply_sent) {
        struct VuVirtq *q = vu_get_queue(dev, qi->qidx);

        fuse_log(FUSE_LOG_DEBUG, "%s: elem %d no reply sent\n", __func__,
                 elem->index);

        pthread_rwlock_rdlock(&qi->virtio_dev->vu_dispatch_rwlock);
        pthread_mutex_lock(&qi->vq_lock);
        vu_queue_push(dev, q, elem, 0);
        vu_queue_notify(dev, q);
        pthread_mutex_unlock(&qi->vq_lock);
        pthread_rwlock_unlock(&qi->virtio_dev->vu_dispatch_rwlock);
    }

    pthread_mutex_destroy(&req->ch.lock);
    free(fbuf.mem);
    free(req);
}

static void *fv_queue_notify_thread(void *opaque)
{
    struct fv_QueueInfo *qi = opaque;

    fuse_log(FUSE_LOG_INFO, "%s: Start for queue %d kick_fd %d\n", __func__,
             qi->qidx, qi->kick_fd);

    while (1) {
        struct pollfd pf[2];

        pf[0].fd = qi->kick_fd;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        pf[1].fd = qi->kill_fd;
        pf[1].events = POLLIN;
        pf[1].revents = 0;

        fuse_log(FUSE_LOG_DEBUG, "%s: Waiting for Queue %d event\n", __func__,
                 qi->qidx);
        int poll_res = ppoll(pf, 2, NULL, NULL);

        if (poll_res == -1) {
            if (errno == EINTR) {
                fuse_log(FUSE_LOG_INFO, "%s: ppoll interrupted, going around\n",
                         __func__);
                continue;
            }
            fuse_log(FUSE_LOG_ERR, "fv_queue_thread ppoll: %m\n");
            break;
        }
        assert(poll_res >= 1);
        if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fuse_log(FUSE_LOG_ERR, "%s: Unexpected poll revents %x Queue %d\n",
                     __func__, pf[0].revents, qi->qidx);
             break;
        }
        if (pf[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fuse_log(FUSE_LOG_ERR, "%s: Unexpected poll revents %x Queue %d"
                     "killfd\n", __func__, pf[1].revents, qi->qidx);
            break;
        }
        if (pf[1].revents) {
            fuse_log(FUSE_LOG_INFO, "%s: kill event on queue %d - quitting\n",
                     __func__, qi->qidx);
            break;
        }
        assert(pf[0].revents & POLLIN);
        fuse_log(FUSE_LOG_DEBUG, "%s: Got queue event on Queue %d\n", __func__,
                 qi->qidx);

        eventfd_t evalue;
        if (eventfd_read(qi->kick_fd, &evalue)) {
            fuse_log(FUSE_LOG_ERR, "Eventfd_read for queue: %m\n");
            break;
        }
    }
    return NULL;
}

/* Thread function for individual queues, created when a queue is 'started' */
static void *fv_queue_thread(void *opaque)
{
    struct fv_QueueInfo *qi = opaque;
    struct VuDev *dev = &qi->virtio_dev->dev;
    struct VuVirtq *q = vu_get_queue(dev, qi->qidx);
    struct fuse_session *se = qi->virtio_dev->se;
    GThreadPool *pool;

    pool = g_thread_pool_new(fv_queue_worker, qi, se->thread_pool_size, TRUE,
                             NULL);
    if (!pool) {
        fuse_log(FUSE_LOG_ERR, "%s: g_thread_pool_new failed\n", __func__);
        return NULL;
    }

    fuse_log(FUSE_LOG_INFO, "%s: Start for queue %d kick_fd %d\n", __func__,
             qi->qidx, qi->kick_fd);
    while (1) {
        struct pollfd pf[2];
        int ret;

        pf[0].fd = qi->kick_fd;
        pf[0].events = POLLIN;
        pf[0].revents = 0;
        pf[1].fd = qi->kill_fd;
        pf[1].events = POLLIN;
        pf[1].revents = 0;

        fuse_log(FUSE_LOG_DEBUG, "%s: Waiting for Queue %d event\n", __func__,
                 qi->qidx);
        int poll_res = ppoll(pf, 2, NULL, NULL);

        if (poll_res == -1) {
            if (errno == EINTR) {
                fuse_log(FUSE_LOG_INFO, "%s: ppoll interrupted, going around\n",
                         __func__);
                continue;
            }
            fuse_log(FUSE_LOG_ERR, "fv_queue_thread ppoll: %m\n");
            break;
        }
        assert(poll_res >= 1);
        if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fuse_log(FUSE_LOG_ERR, "%s: Unexpected poll revents %x Queue %d\n",
                     __func__, pf[0].revents, qi->qidx);
            break;
        }
        if (pf[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fuse_log(FUSE_LOG_ERR,
                     "%s: Unexpected poll revents %x Queue %d killfd\n",
                     __func__, pf[1].revents, qi->qidx);
            break;
        }
        if (pf[1].revents) {
            fuse_log(FUSE_LOG_INFO, "%s: kill event on queue %d - quitting\n",
                     __func__, qi->qidx);
            break;
        }
        assert(pf[0].revents & POLLIN);
        fuse_log(FUSE_LOG_DEBUG, "%s: Got queue event on Queue %d\n", __func__,
                 qi->qidx);

        eventfd_t evalue;
        if (eventfd_read(qi->kick_fd, &evalue)) {
            fuse_log(FUSE_LOG_ERR, "Eventfd_read for queue: %m\n");
            break;
        }
        /* Mutual exclusion with virtio_loop() */
        ret = pthread_rwlock_rdlock(&qi->virtio_dev->vu_dispatch_rwlock);
        assert(ret == 0); /* there is no possible error case */
        pthread_mutex_lock(&qi->vq_lock);
        /* out is from guest, in is too guest */
        unsigned int in_bytes, out_bytes;
        vu_queue_get_avail_bytes(dev, q, &in_bytes, &out_bytes, ~0, ~0);

        fuse_log(FUSE_LOG_DEBUG,
                 "%s: Queue %d gave evalue: %zx available: in: %u out: %u\n",
                 __func__, qi->qidx, (size_t)evalue, in_bytes, out_bytes);

        while (1) {
            unsigned int bad_in_num = 0, bad_out_num = 0;
            FVRequest *req = vu_queue_pop(dev, q, sizeof(FVRequest),
                                          &bad_in_num, &bad_out_num);
            if (!req) {
                break;
            }

            req->reply_sent = false;
            req->bad_in_num = bad_in_num;
            req->bad_out_num = bad_out_num;

            g_thread_pool_push(pool, req, NULL);
        }

        pthread_mutex_unlock(&qi->vq_lock);
        pthread_rwlock_unlock(&qi->virtio_dev->vu_dispatch_rwlock);
    }

    g_thread_pool_free(pool, FALSE, TRUE);

    return NULL;
}

/* Callback from libvhost-user on start or stop of a queue */
static void fv_queue_set_started(VuDev *dev, int qidx, bool started)
{
    struct fv_VuDev *vud = container_of(dev, struct fv_VuDev, dev);
    struct fv_QueueInfo *ourqi;
    void * (*thread_func) (void *) = fv_queue_thread;
    int valid_queues = 2; /* One hiprio queue and one request queue */

    fuse_log(FUSE_LOG_INFO, "%s: qidx=%d started=%d\n", __func__, qidx,
             started);
    assert(qidx >= 0);

    /*
     * Ignore additional request queues for now.  passthrough_ll.c must be
     * audited for thread-safety issues first.  It was written with a
     * well-behaved client in mind and may not protect against all types of
     * races yet.
     */
    if (vud->notify_enabled)
        valid_queues++;

    if (qidx >= valid_queues) {
        fuse_log(FUSE_LOG_ERR, "%s: multiple request queues not yet"
                 "implemented, please only configure 1 request queue\n",
                 __func__);
        exit(EXIT_FAILURE);
    }

    if (started) {
        /* Fire up a thread to watch this queue */
        if (qidx >= vud->nqueues) {
            vud->qi = realloc(vud->qi, (qidx + 1) * sizeof(vud->qi[0]));
            assert(vud->qi);
            memset(vud->qi + vud->nqueues, 0,
                   sizeof(vud->qi[0]) * (1 + (qidx - vud->nqueues)));
            vud->nqueues = qidx + 1;
        }
        if (!vud->qi[qidx]) {
            vud->qi[qidx] = calloc(sizeof(struct fv_QueueInfo), 1);
            assert(vud->qi[qidx]);
            vud->qi[qidx]->virtio_dev = vud;
            vud->qi[qidx]->qidx = qidx;
        } else {
            /* Shouldn't have been started */
            assert(vud->qi[qidx]->kick_fd == -1);
        }
        ourqi = vud->qi[qidx];
        ourqi->kick_fd = dev->vq[qidx].kick_fd;

        ourqi->kill_fd = eventfd(0, EFD_CLOEXEC | EFD_SEMAPHORE);
        assert(ourqi->kill_fd != -1);
        /*
         * First queue (idx = 0)  is hiprio queue. Second queue is
         * notification queue (if enabled). And rest are request
         * queues.
         */
        if (vud->notify_enabled && qidx == 1) {
            thread_func = fv_queue_notify_thread;
        }

        pthread_mutex_init(&ourqi->vq_lock, NULL);
        if (pthread_create(&ourqi->thread, NULL, thread_func, ourqi)) {
            fuse_log(FUSE_LOG_ERR, "%s: Failed to create thread for queue %d\n",
                     __func__, qidx);
            assert(0);
        }
    } else {
        int ret;
        assert(qidx < vud->nqueues);
        ourqi = vud->qi[qidx];

        /* Kill the thread */
        if (eventfd_write(ourqi->kill_fd, 1)) {
            fuse_log(FUSE_LOG_ERR, "Eventfd_read for queue: %m\n");
        }
        ret = pthread_join(ourqi->thread, NULL);
        if (ret) {
            fuse_log(FUSE_LOG_ERR, "%s: Failed to join thread idx %d err %d\n",
                     __func__, qidx, ret);
        }
        pthread_mutex_destroy(&ourqi->vq_lock);
        close(ourqi->kill_fd);
        ourqi->kick_fd = -1;
        free(vud->qi[qidx]);
        vud->qi[qidx] = NULL;
    }
}

static bool fv_queue_order(VuDev *dev, int qidx)
{
    return false;
}

static uint64_t fv_get_protocol_features(VuDev *dev)
{
	return 1ull << VHOST_USER_PROTOCOL_F_CONFIG;
}

static int fv_get_config(VuDev *dev, uint8_t *config, uint32_t len)
{
	struct virtio_fs_config fscfg = {};

	fuse_log(FUSE_LOG_DEBUG, "%s:Setting notify_buf_size=%d\n", __func__,
                 sizeof(struct fuse_notify_lock_out));
	/*
	 * As of now only notification related to lock is supported. As more
	 * notification types are supported, bump up the size accordingly.
	 */
	fscfg.notify_buf_size = sizeof(struct fuse_notify_lock_out);

	memcpy(config, &fscfg, len);
	return 0;
}

static const VuDevIface fv_iface = {
    .get_features = fv_get_features,
    .set_features = fv_set_features,

    /* Don't need process message, we've not got any at vhost-user level */
    .queue_set_started = fv_queue_set_started,

    .queue_is_processed_in_order = fv_queue_order,
    .get_protocol_features = fv_get_protocol_features,
    .get_config = fv_get_config,
};

/*
 * Main loop; this mostly deals with events on the vhost-user
 * socket itself, and not actual fuse data.
 */
int virtio_loop(struct fuse_session *se)
{
    fuse_log(FUSE_LOG_INFO, "%s: Entry\n", __func__);

    while (!fuse_session_exited(se)) {
        struct pollfd pf[1];
        bool ok;
        int ret;
        pf[0].fd = se->vu_socketfd;
        pf[0].events = POLLIN;
        pf[0].revents = 0;

        fuse_log(FUSE_LOG_DEBUG, "%s: Waiting for VU event\n", __func__);
        int poll_res = ppoll(pf, 1, NULL, NULL);

        if (poll_res == -1) {
            if (errno == EINTR) {
                fuse_log(FUSE_LOG_INFO, "%s: ppoll interrupted, going around\n",
                         __func__);
                continue;
            }
            fuse_log(FUSE_LOG_ERR, "virtio_loop ppoll: %m\n");
            break;
        }
        assert(poll_res == 1);
        if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fuse_log(FUSE_LOG_ERR, "%s: Unexpected poll revents %x\n", __func__,
                     pf[0].revents);
            break;
        }
        assert(pf[0].revents & POLLIN);
        fuse_log(FUSE_LOG_DEBUG, "%s: Got VU event\n", __func__);
        /* Mutual exclusion with fv_queue_thread() */
        ret = pthread_rwlock_wrlock(&se->virtio_dev->vu_dispatch_rwlock);
        assert(ret == 0); /* there is no possible error case */

        ok = vu_dispatch(&se->virtio_dev->dev);

        pthread_rwlock_unlock(&se->virtio_dev->vu_dispatch_rwlock);

        if (!ok) {
            fuse_log(FUSE_LOG_ERR, "%s: vu_dispatch failed\n", __func__);
            break;
        }
    }

    fuse_log(FUSE_LOG_INFO, "%s: Exit\n", __func__);

    return 0;
}

static void strreplace(char *s, char old, char new)
{
    for (; *s; ++s) {
        if (*s == old) {
            *s = new;
        }
    }
}

static int fv_socket_lock(struct fuse_session *se)
{
    char *dir, *sk_name;
    Error *local_err = NULL;
    int ret = -1;

    dir = qemu_get_local_state_pathname("run/virtiofsd");

    if (g_mkdir_with_parents(dir, S_IRWXU) < -1) {
        fuse_log(FUSE_LOG_ERR, "%s: Failed to create directory %s: %s",
                 __func__, dir, strerror(errno));
        g_free(dir);
        return ret;
    }

    sk_name = g_strdup(se->vu_socket_path);
    strreplace(sk_name, '/', '.');
    se->vu_socket_lock = g_strdup_printf("%s/%s.pid", dir, sk_name);

    if (!qemu_write_pidfile(se->vu_socket_lock, &local_err)) {
        error_report_err(local_err);
    } else {
        ret = 0;
    }

    g_free(sk_name);
    g_free(dir);

    return ret;
}

static int fv_create_listen_socket(struct fuse_session *se)
{
    struct sockaddr_un un;

    /* Nothing to do if fd is already initialized */
    if (se->vu_listen_fd >= 0) {
        return 0;
    }

    if (strlen(se->vu_socket_path) >= sizeof(un.sun_path)) {
        fuse_log(FUSE_LOG_ERR, "Socket path too long\n");
        return -1;
    }

    if (!strlen(se->vu_socket_path)) {
        fuse_log(FUSE_LOG_ERR, "Socket path is empty\n");
        return -1;
    }

    /* Check the vu_socket_path is already used */
    if (fv_socket_lock(se) == -1) {
        fuse_log(FUSE_LOG_ERR, "%s: Socket lock file creation failed\n",
                 __func__);
        return -1;
    }

    /*
     * Create the Unix socket to communicate with qemu
     * based on QEMU's vhost-user-bridge
     */
    unlink(se->vu_socket_path);
    strcpy(un.sun_path, se->vu_socket_path);
    size_t addr_len = sizeof(un.sun_family) + strlen(se->vu_socket_path);

    int listen_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_sock == -1) {
        fuse_log(FUSE_LOG_ERR, "vhost socket creation: %m\n");
        return -1;
    }
    un.sun_family = AF_UNIX;

    if (bind(listen_sock, (struct sockaddr *)&un, addr_len) == -1) {
        fuse_log(FUSE_LOG_ERR, "vhost socket bind: %m\n");
        return -1;
    }

    if (listen(listen_sock, 1) == -1) {
        fuse_log(FUSE_LOG_ERR, "vhost socket listen: %m\n");
        return -1;
    }

    se->vu_listen_fd = listen_sock;
    return 0;
}

int virtio_session_mount(struct fuse_session *se)
{
    int ret;

    ret = fv_create_listen_socket(se);
    if (ret < 0) {
        return ret;
    }

    /*
     * Poison the fuse FD so we spot if we accidentally use it;
     * DO NOT check for this value, check fuse_lowlevel_is_virtio()
     */
    se->fd = 0xdaff0d11;

    fuse_log(FUSE_LOG_INFO, "%s: Waiting for vhost-user socket connection...\n",
             __func__);
    int data_sock = accept(se->vu_listen_fd, NULL, NULL);
    if (data_sock == -1) {
        fuse_log(FUSE_LOG_ERR, "vhost socket accept: %m\n");
        close(se->vu_listen_fd);
        return -1;
    }
    close(se->vu_listen_fd);
    se->vu_listen_fd = -1;
    fuse_log(FUSE_LOG_INFO, "%s: Received vhost-user socket connection\n",
             __func__);
    se->vu_socketfd = data_sock;

    /* TODO: Some cleanup/deallocation! */
    se->virtio_dev = calloc(sizeof(struct fv_VuDev), 1);
    se->virtio_dev->se = se;
    pthread_rwlock_init(&se->virtio_dev->vu_dispatch_rwlock, NULL);
    vu_init(&se->virtio_dev->dev, 3, se->vu_socketfd, fv_panic, fv_set_watch,
            fv_remove_watch, &fv_iface);

    return 0;
}

void virtio_session_close(struct fuse_session *se)
{
    close(se->vu_socketfd);

    if (!se->virtio_dev) {
        return;
    }

    close(se->vu_socketfd);
    free(se->virtio_dev->qi);
    pthread_rwlock_destroy(&se->virtio_dev->vu_dispatch_rwlock);
    free(se->virtio_dev);
    se->virtio_dev = NULL;
}

int64_t fuse_virtio_map(fuse_req_t req, VhostUserFSSlaveMsg *msg, int fd)
{
    if (!req->se->virtio_dev) {
        return -ENODEV;
    }
    return vu_fs_cache_request(&req->se->virtio_dev->dev,
                               VHOST_USER_SLAVE_FS_MAP, fd, msg);
}

int64_t fuse_virtio_unmap(struct fuse_session *se, VhostUserFSSlaveMsg *msg)
{
    if (!se->virtio_dev) {
        return -ENODEV;
    }
    return vu_fs_cache_request(&se->virtio_dev->dev, VHOST_USER_SLAVE_FS_UNMAP,
                               -1, msg);
}

int64_t fuse_virtio_sync(fuse_req_t req, VhostUserFSSlaveMsg *msg)
{
    if (!req->se->virtio_dev) {
        return -ENODEV;
    }
    return vu_fs_cache_request(&req->se->virtio_dev->dev,
                               VHOST_USER_SLAVE_FS_SYNC, -1, msg);
}

int64_t fuse_virtio_io(struct fuse_session *se, VhostUserFSSlaveMsg *msg,
                       int fd)
{
    if (!se->virtio_dev) {
        return -ENODEV;
    }
    return vu_fs_cache_request(&se->virtio_dev->dev, VHOST_USER_SLAVE_FS_IO, fd,
                               msg);
}

/*
 * Write to a file (dst) from an area of guest GPA (src) that probably
 * isn't visible to the daemon.
 */
ssize_t fuse_virtio_write(fuse_req_t req, const struct fuse_buf *dst,
                          size_t dst_off, const struct fuse_buf *src,
                          size_t src_off, size_t len)
{
    VhostUserFSSlaveMsg msg = { 0 };

    if (dst->flags & FUSE_BUF_FD_SEEK) {
        msg.fd_offset[0] = dst->pos + dst_off;
    } else {
        off_t cur = lseek(dst->fd, 0, SEEK_CUR);
        if (cur == (off_t)-1) {
            return -errno;
        }
        msg.fd_offset[0] = cur;
    }
    msg.c_offset[0] = (uintptr_t)src->mem + src_off;
    msg.len[0] = len;
    msg.flags[0] = VHOST_USER_FS_FLAG_MAP_W;

    int64_t result = fuse_virtio_io(req->se, &msg, dst->fd);
    fuse_log(FUSE_LOG_DEBUG, "%s: result=%ld\n", __func__, result);
    return result;
}
