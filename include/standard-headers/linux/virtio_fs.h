/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */

#ifndef _LINUX_VIRTIO_FS_H
#define _LINUX_VIRTIO_FS_H

#include "standard-headers/linux/types.h"
#include "standard-headers/linux/virtio_ids.h"
#include "standard-headers/linux/virtio_config.h"
#include "standard-headers/linux/virtio_types.h"

/* Feature bits */
#define VIRTIO_FS_F_NOTIFICATION	0	/* Notification queue supported */

struct virtio_fs_config {
	/* Filesystem name (UTF-8, not NUL-terminated, padded with NULs) */
	uint8_t tag[36];

	/* Number of request queues */
	uint32_t num_request_queues;
	/* Size of notification buffer */
	uint32_t notify_buf_size;
} QEMU_PACKED;

#define VIRTIO_FS_PCI_CACHE_BAR 2

/* For the id field in virtio_pci_shm_cap */
#define VIRTIO_FS_PCI_SHMCAP_ID_CACHE 0
#define VIRTIO_FS_PCI_SHMCAP_ID_VERTAB 1

#endif /* _LINUX_VIRTIO_FS_H */
