/*
 * Vhost-user filesystem virtio device
 *
 * Copyright 2018-2019 Red Hat, Inc.
 *
 * Authors:
 *  Stefan Hajnoczi <stefanha@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version.  See the COPYING file in the
 * top-level directory.
 */

#include "qemu/osdep.h"
#include <sys/ioctl.h>
#include "standard-headers/linux/virtio_fs.h"
#include "qapi/error.h"
#include "hw/qdev-properties.h"
#include "hw/virtio/virtio-bus.h"
#include "hw/virtio/virtio-access.h"
#include "qemu/error-report.h"
#include "hw/virtio/vhost-user-fs.h"
#include "monitor/monitor.h"
#include "exec/address-spaces.h"
#include "trace.h"

uint64_t vhost_user_fs_slave_map(struct vhost_dev *dev, VhostUserFSSlaveMsg *sm,
                                 int fd)
{
    VHostUserFS *fs = VHOST_USER_FS(dev->vdev);
    if (!fs) {
        /* Shouldn't happen - but has a habit of doing when things are failing */
        fprintf(stderr, "%s: Bad fs ptr\n", __func__);
        return (uint64_t)-1;
    }
    size_t cache_size = fs->conf.cache_size;
    if (!cache_size) {
        fprintf(stderr, "%s: map when DAX cache not present\n", __func__);
        return (uint64_t)-1;
    }
    void *cache_host = memory_region_get_ram_ptr(&fs->cache);

    unsigned int i;
    int res = 0;

    if (fd < 0) {
        fprintf(stderr, "%s: Bad fd for map\n", __func__);
        return (uint64_t)-1;
    }

    for (i = 0; i < VHOST_USER_FS_SLAVE_ENTRIES; i++) {
        if (sm->len[i] == 0) {
            continue;
        }

        if ((sm->c_offset[i] + sm->len[i]) < sm->len[i] ||
            (sm->c_offset[i] + sm->len[i]) > cache_size) {
            fprintf(stderr, "%s: Bad offset/len for map [%d] %"
                            PRIx64 "+%" PRIx64 "\n", __func__,
                            i, sm->c_offset[i], sm->len[i]);
            res = -1;
            break;
        }

        if (mmap(cache_host + sm->c_offset[i], sm->len[i],
                 ((sm->flags[i] & VHOST_USER_FS_FLAG_MAP_R) ? PROT_READ : 0) |
                 ((sm->flags[i] & VHOST_USER_FS_FLAG_MAP_W) ? PROT_WRITE : 0),
                 MAP_SHARED | MAP_FIXED,
                 fd, sm->fd_offset[i]) != (cache_host + sm->c_offset[i])) {
            res = -errno;
            fprintf(stderr, "%s: map failed err %d [%d] %"
                            PRIx64 "+%" PRIx64 " from %" PRIx64 "\n", __func__,
                            errno, i, sm->c_offset[i], sm->len[i],
                            sm->fd_offset[i]);
            break;
        }
    }

    if (res) {
        /* Something went wrong, unmap them all */
        vhost_user_fs_slave_unmap(dev, sm);
    }
    return (uint64_t)res;
}

uint64_t vhost_user_fs_slave_unmap(struct vhost_dev *dev, VhostUserFSSlaveMsg *sm)
{
    VHostUserFS *fs = VHOST_USER_FS(dev->vdev);
    if (!fs) {
        /* Shouldn't happen - but has a habit of doing when things are failing */
        fprintf(stderr, "%s: Bad fs ptr\n", __func__);
        return -1;
    }
    size_t cache_size = fs->conf.cache_size;
    if (!cache_size) {
        /*
         * Since dax cache is disabled, there should be no unmap request.
         * Howerver we still receives whole range unmap request during umount
         * for cleanup. Ignore it.
         */
        if (sm->len[0] == ~(uint64_t)0) {
            return 0;
        }

        fprintf(stderr, "%s: unmap when DAX cache not present\n", __func__);
        return (uint64_t)-1;
    }
    void *cache_host = memory_region_get_ram_ptr(&fs->cache);

    unsigned int i;
    int res = 0;

    /* Note even if one unmap fails we try the rest, since the effect
     * is to clean up as much as possible.
     */
    for (i = 0; i < VHOST_USER_FS_SLAVE_ENTRIES; i++) {
        void *ptr;
        if (sm->len[i] == 0) {
            continue;
        }

        if (sm->len[i] == ~(uint64_t)0) {
            /* Special case meaning the whole arena */
            sm->len[i] = cache_size;
        }

        if ((sm->c_offset[i] + sm->len[i]) < sm->len[i] ||
            (sm->c_offset[i] + sm->len[i]) > cache_size) {
            fprintf(stderr, "%s: Bad offset/len for unmap [%d] %"
                            PRIx64 "+%" PRIx64 "\n", __func__,
                            i, sm->c_offset[i], sm->len[i]);
            res = -1;
            continue;
        }

        ptr = mmap(cache_host + sm->c_offset[i], sm->len[i],
                PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (ptr != (cache_host + sm->c_offset[i])) {
            res = -errno;
            fprintf(stderr, "%s: mmap failed (%s) [%d] %"
                            PRIx64 "+%" PRIx64 " from %" PRIx64 " res: %p\n",
                            __func__,
                            strerror(errno),
                            i, sm->c_offset[i], sm->len[i],
                            sm->fd_offset[i], ptr);
        }
    }

    return (uint64_t)res;
}

uint64_t vhost_user_fs_slave_sync(struct vhost_dev *dev, VhostUserFSSlaveMsg *sm)
{
    VHostUserFS *fs = VHOST_USER_FS(dev->vdev);
    size_t cache_size = fs->conf.cache_size;
    if (!cache_size) {
        fprintf(stderr, "%s: sync when DAX cache not present\n", __func__);
        return (uint64_t)-1;
    }
    void *cache_host = memory_region_get_ram_ptr(&fs->cache);

    unsigned int i;
    int res = 0;

    /* Note even if one sync fails we try the rest */
    for (i = 0; i < VHOST_USER_FS_SLAVE_ENTRIES; i++) {
        if (sm->len[i] == 0) {
            continue;
        }

        if ((sm->c_offset[i] + sm->len[i]) < sm->len[i] ||
            (sm->c_offset[i] + sm->len[i]) > cache_size) {
            fprintf(stderr, "%s: Bad offset/len for sync [%d] %"
                            PRIx64 "+%" PRIx64 "\n", __func__,
                            i, sm->c_offset[i], sm->len[i]);
            res = -1;
            continue;
        }

        if (msync(cache_host + sm->c_offset[i], sm->len[i],
                  MS_SYNC /* ?? */)) {
            res = -errno;
            fprintf(stderr, "%s: msync failed (%s) [%d] %"
                            PRIx64 "+%" PRIx64 " from %" PRIx64 "\n", __func__,
                            strerror(errno),
                            i, sm->c_offset[i], sm->len[i],
                            sm->fd_offset[i]);
        }
    }

    return (uint64_t)res;
}

uint64_t vhost_user_fs_slave_io(struct vhost_dev *dev, VhostUserFSSlaveMsg *sm,
                                int fd)
{
    VHostUserFS *fs = VHOST_USER_FS(dev->vdev);
    if (!fs) {
        /* Shouldn't happen - but seen it in error paths */
        fprintf(stderr, "%s: Bad fs ptr\n", __func__);
        return (uint64_t)-1;
    }

    unsigned int i;
    int res = 0;
    size_t done = 0;

    if (fd < 0) {
        fprintf(stderr, "%s: Bad fd for map\n", __func__);
        return (uint64_t)-1;
    }

    for (i = 0; i < VHOST_USER_FS_SLAVE_ENTRIES && !res; i++) {
        if (sm->len[i] == 0) {
            continue;
        }

        size_t len = sm->len[i];
        hwaddr gpa = sm->c_offset[i];

        while (len && !res) {
            MemoryRegionSection mrs = memory_region_find(get_system_memory(),
                                                         gpa, len);
            if (!mrs.size) {
                fprintf(stderr,
                        "%s: No guest region found for 0x%" HWADDR_PRIx "\n",
                        __func__, gpa);
                res = -EFAULT;
                break;
            }

            trace_vhost_user_fs_slave_io_loop(mrs.mr->name,
                                          (uint64_t)mrs.offset_within_region,
                                          memory_region_is_ram(mrs.mr),
                                          memory_region_is_romd(mrs.mr),
                                          (size_t)mrs.size);

            void *hostptr = qemu_map_ram_ptr(mrs.mr->ram_block,
                                             mrs.offset_within_region);
            ssize_t transferred;
            if (sm->flags[i] & VHOST_USER_FS_FLAG_MAP_R) {
                /* Read from file into RAM */
                if (mrs.mr->readonly) {
                    res = -EFAULT;
                    break;
                }
                transferred = pread(fd, hostptr, mrs.size, sm->fd_offset[i]);
            } else {
                /* Write into file from RAM */
                assert((sm->flags[i] & VHOST_USER_FS_FLAG_MAP_W));
                transferred = pwrite(fd, hostptr, mrs.size, sm->fd_offset[i]);
            }
            trace_vhost_user_fs_slave_io_loop_res(transferred);
            if (transferred < 0) {
                res = -errno;
                break;
            }
            if (!transferred) {
                /* EOF */
                break;
            }

            done += transferred;
            len -= transferred;
        }
    }
    close(fd);

    trace_vhost_user_fs_slave_io_exit(res, done);
    if (res < 0) {
        return (uint64_t)res;
    }
    return (uint64_t)done;
}


static void vuf_get_config(VirtIODevice *vdev, uint8_t *config)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);
    struct virtio_fs_config fscfg = {};

    memcpy((char *)fscfg.tag, fs->conf.tag,
           MIN(strlen(fs->conf.tag) + 1, sizeof(fscfg.tag)));

    virtio_stl_p(vdev, &fscfg.num_request_queues, fs->conf.num_request_queues);

    memcpy(config, &fscfg, sizeof(fscfg));
}

static void vuf_start(VirtIODevice *vdev)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int ret;
    int i;

    if (!k->set_guest_notifiers) {
        error_report("binding does not support guest notifiers");
        return;
    }

    ret = vhost_dev_enable_notifiers(&fs->vhost_dev, vdev);
    if (ret < 0) {
        error_report("Error enabling host notifiers: %d", -ret);
        return;
    }

    ret = k->set_guest_notifiers(qbus->parent, fs->vhost_dev.nvqs, true);
    if (ret < 0) {
        error_report("Error binding guest notifier: %d", -ret);
        goto err_host_notifiers;
    }

    fs->vhost_dev.acked_features = vdev->guest_features;
    ret = vhost_dev_start(&fs->vhost_dev, vdev);
    if (ret < 0) {
        error_report("Error starting vhost: %d", -ret);
        goto err_guest_notifiers;
    }

    /*
     * guest_notifier_mask/pending not used yet, so just unmask
     * everything here.  virtio-pci will do the right thing by
     * enabling/disabling irqfd.
     */
    for (i = 0; i < fs->vhost_dev.nvqs; i++) {
        vhost_virtqueue_mask(&fs->vhost_dev, vdev, i, false);
    }

    return;

err_guest_notifiers:
    k->set_guest_notifiers(qbus->parent, fs->vhost_dev.nvqs, false);
err_host_notifiers:
    vhost_dev_disable_notifiers(&fs->vhost_dev, vdev);
}

static void vuf_stop(VirtIODevice *vdev)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);
    BusState *qbus = BUS(qdev_get_parent_bus(DEVICE(vdev)));
    VirtioBusClass *k = VIRTIO_BUS_GET_CLASS(qbus);
    int ret;

    if (!k->set_guest_notifiers) {
        return;
    }

    vhost_dev_stop(&fs->vhost_dev, vdev);

    ret = k->set_guest_notifiers(qbus->parent, fs->vhost_dev.nvqs, false);
    if (ret < 0) {
        error_report("vhost guest notifier cleanup failed: %d", ret);
        return;
    }

    vhost_dev_disable_notifiers(&fs->vhost_dev, vdev);
}

static void vuf_set_status(VirtIODevice *vdev, uint8_t status)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);
    bool should_start = status & VIRTIO_CONFIG_S_DRIVER_OK;

    if (!vdev->vm_running) {
        should_start = false;
    }

    if (fs->vhost_dev.started == should_start) {
        return;
    }

    if (should_start) {
        vuf_start(vdev);
    } else {
        vuf_stop(vdev);
    }
}

static uint64_t vuf_get_features(VirtIODevice *vdev,
                                      uint64_t requested_features,
                                      Error **errp)
{
    /* No feature bits used yet */
    return requested_features;
}

static void vuf_handle_output(VirtIODevice *vdev, VirtQueue *vq)
{
    /*
     * Not normally called; it's the daemon that handles the queue;
     * however virtio's cleanup path can call this.
     */
}

static void vuf_guest_notifier_mask(VirtIODevice *vdev, int idx,
                                            bool mask)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);

    vhost_virtqueue_mask(&fs->vhost_dev, vdev, idx, mask);
}

static bool vuf_guest_notifier_pending(VirtIODevice *vdev, int idx)
{
    VHostUserFS *fs = VHOST_USER_FS(vdev);

    return vhost_virtqueue_pending(&fs->vhost_dev, idx);
}

static void vuf_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserFS *fs = VHOST_USER_FS(dev);
    void *cache_ptr;
    unsigned int i;
    size_t len;
    int ret;
    int mdvtfd = -1;

    if (!fs->conf.chardev.chr) {
        error_setg(errp, "missing chardev");
        return;
    }

    if (!fs->conf.tag) {
        error_setg(errp, "missing tag property");
        return;
    }
    len = strlen(fs->conf.tag);
    if (len == 0) {
        error_setg(errp, "tag property cannot be empty");
        return;
    }
    if (len > sizeof_field(struct virtio_fs_config, tag)) {
        error_setg(errp, "tag property must be %zu bytes or less",
                   sizeof_field(struct virtio_fs_config, tag));
        return;
    }

    if (fs->conf.num_request_queues == 0) {
        error_setg(errp, "num-request-queues property must be larger than 0");
        return;
    }

    if (!is_power_of_2(fs->conf.queue_size)) {
        error_setg(errp, "queue-size property must be a power of 2");
        return;
    }

    if (fs->conf.queue_size > VIRTQUEUE_MAX_SIZE) {
        error_setg(errp, "queue-size property must be %u or smaller",
                   VIRTQUEUE_MAX_SIZE);
        return;
    }
    if (fs->conf.cache_size &&
        (!is_power_of_2(fs->conf.cache_size) ||
          fs->conf.cache_size < sysconf(_SC_PAGESIZE))) {
        error_setg(errp, "cache-size property must be a power of 2 "
                         "no smaller than the page size");
        return;
    }
    if (fs->conf.mdvtpath) {
        struct stat statbuf;

        mdvtfd = open(fs->conf.mdvtpath, O_RDWR);
        if (mdvtfd < 0) {
            error_setg_errno(errp, errno,
                             "Failed to open meta-data version table '%s'",
                             fs->conf.mdvtpath);

            return;
        }
        if (fstat(mdvtfd, &statbuf) == -1) {
            error_setg_errno(errp, errno,
                             "Failed to stat meta-data version table '%s'",
                             fs->conf.mdvtpath);
            close(mdvtfd);
            return;
        }

        fs->mdvt_size = statbuf.st_size;
    }

    if (fs->conf.cache_size) {
        /* Anonymous, private memory is not counted as overcommit */
        cache_ptr = mmap(NULL, fs->conf.cache_size, PROT_NONE,
                         MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (cache_ptr == MAP_FAILED) {
            error_setg(errp, "Unable to mmap blank cache");
            return;
        }

        memory_region_init_ram_ptr(&fs->cache, OBJECT(vdev),
                                   "virtio-fs-cache",
                                   fs->conf.cache_size, cache_ptr);
    }

    if (mdvtfd) {
        memory_region_init_ram_from_fd(&fs->mdvt, OBJECT(vdev),
                       "virtio-fs-mdvt",
                       fs->mdvt_size, true, mdvtfd, NULL);
        /* The version table is read-only by the guest */
        memory_region_set_readonly(&fs->mdvt, true);
    }

    if (!vhost_user_init(&fs->vhost_user, &fs->conf.chardev, errp)) {
        return;
    }

    virtio_init(vdev, "vhost-user-fs", VIRTIO_ID_FS,
                sizeof(struct virtio_fs_config));

    /* Hiprio queue */
    virtio_add_queue(vdev, fs->conf.queue_size, vuf_handle_output);

    /* Request queues */
    for (i = 0; i < fs->conf.num_request_queues; i++) {
        virtio_add_queue(vdev, fs->conf.queue_size, vuf_handle_output);
    }

    /* 1 high prio queue, plus the number configured */
    fs->vhost_dev.nvqs = 1 + fs->conf.num_request_queues;
    fs->vhost_dev.vqs = g_new0(struct vhost_virtqueue, fs->vhost_dev.nvqs);
    ret = vhost_dev_init(&fs->vhost_dev, &fs->vhost_user,
                         VHOST_BACKEND_TYPE_USER, 0);
    if (ret < 0) {
        error_setg_errno(errp, -ret, "vhost_dev_init failed");
        goto err_virtio;
    }

    return;

err_virtio:
    vhost_user_cleanup(&fs->vhost_user);
    virtio_cleanup(vdev);
    g_free(fs->vhost_dev.vqs);
    return;
}

static void vuf_device_unrealize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VHostUserFS *fs = VHOST_USER_FS(dev);

    /* This will stop vhost backend if appropriate. */
    vuf_set_status(vdev, 0);

    vhost_dev_cleanup(&fs->vhost_dev);

    vhost_user_cleanup(&fs->vhost_user);

    virtio_cleanup(vdev);
    g_free(fs->vhost_dev.vqs);
    fs->vhost_dev.vqs = NULL;
}

static const VMStateDescription vuf_vmstate = {
    .name = "vhost-user-fs",
    .unmigratable = 1,
};

static Property vuf_properties[] = {
    DEFINE_PROP_CHR("chardev", VHostUserFS, conf.chardev),
    DEFINE_PROP_STRING("tag", VHostUserFS, conf.tag),
    DEFINE_PROP_UINT16("num-request-queues", VHostUserFS,
                       conf.num_request_queues, 1),
    DEFINE_PROP_UINT16("queue-size", VHostUserFS, conf.queue_size, 128),
    DEFINE_PROP_STRING("vhostfd", VHostUserFS, conf.vhostfd),
    DEFINE_PROP_SIZE("cache-size", VHostUserFS, conf.cache_size, 1ull << 30),
    DEFINE_PROP_STRING("versiontable", VHostUserFS, conf.mdvtpath),
    DEFINE_PROP_END_OF_LIST(),
};

static void vuf_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    dc->props = vuf_properties;
    dc->vmsd = &vuf_vmstate;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
    vdc->realize = vuf_device_realize;
    vdc->unrealize = vuf_device_unrealize;
    vdc->get_features = vuf_get_features;
    vdc->get_config = vuf_get_config;
    vdc->set_status = vuf_set_status;
    vdc->guest_notifier_mask = vuf_guest_notifier_mask;
    vdc->guest_notifier_pending = vuf_guest_notifier_pending;
}

static const TypeInfo vuf_info = {
    .name = TYPE_VHOST_USER_FS,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VHostUserFS),
    .class_init = vuf_class_init,
};

static void vuf_register_types(void)
{
    type_register_static(&vuf_info);
}

type_init(vuf_register_types)
