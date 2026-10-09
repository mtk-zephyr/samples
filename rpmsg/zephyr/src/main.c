/*
 * Copyright (c) 2018, NXP
 * Copyright (c) 2018, Nordic Semiconductor ASA
 * Copyright (c) 2018-2019, Linaro Limited
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/drivers/virtualization/ivshmem.h>
#include <zephyr/kernel.h>
#include <zephyr/arch/arch_interface.h>	/* arch_page_phys_get() */

#include <openamp/open_amp.h>
#include <metal/device.h>

#include "common.h"


#define ERROR_STR  "[ERROR] "
#define INFO_STR   "[INFO]  "

#define MSG_STR_SIZE 64

#define MSG_STR "Hello from Zephyr!"

#define RPMSG_THREAD_STACK_SIZE    2048
#define DOORBELL_THREAD_STACK_SIZE 2048


typedef enum{
    RSC_OFFS_VDEV = 0,
    RSC_OFFS_END
}   rsc_offs_t;

typedef struct __attribute__ ((packed)) {
    struct resource_table hdr;

	uint32_t offset [RSC_OFFS_END];

    struct fw_rsc_vdev vdev;

    struct fw_rsc_vdev_vring vring [VRING_COUNT];
}   rsc_tbl_t;

typedef struct {
	/* IVSHMEM device */
	const struct device* dev;

	/* Virtual and physical address of IVSHMEM RW memory region. */
	uintptr_t ivshmem_paddr;
	uintptr_t ivshmem_vaddr;

	/* Size of IVSHMEM RW memory region. */
	size_t ivshmem_size;

	uint32_t id;
	uint32_t target;
}   rpmsg_ctx_t;


static int msg_cnt = 0;

static char msg_tx [MSG_STR_SIZE];

static rpmsg_ctx_t rpmsg_ctx;

/* Static part of the resource table. The dynamic   */
/* part gets set during application initialization. */
static const rsc_tbl_t rsc_tbl = {
    .hdr = {
        .ver = 1,
        .num = RSC_OFFS_END,

        .reserved = {0, 0},
    },

    .offset = {
        [RSC_OFFS_VDEV] = offsetof (rsc_tbl_t, vdev),
    },

    .vdev = {
        .type = RSC_VDEV,
        .id   = VIRTIO_ID_RPMSG,

        .notifyid   = 0,
        .dfeatures  = 1 << VIRTIO_RPMSG_F_NS,
        .gfeatures  = 0,
        .config_len = 0,
        .status     = 0,

        .num_of_vrings = VRING_COUNT,
        
        .reserved = {0, 0}
    },

    .vring = {
        [VRING_TX] = {
            .align    = VRING_ALIGNMENT,
            .num      = VRING_NUM_DESCRS,
            .notifyid = VRING_TX + 1,
            .reserved = 0,
        },
        [VRING_RX] = {
            .align    = VRING_ALIGNMENT,
            .num      = VRING_NUM_DESCRS,
            .notifyid = VRING_RX + 1,
            .reserved = 0,
        },
    }
};


static uint32_t rpmsg_dest = UINT32_MAX;

static struct metal_io_region metal_io;

static metal_phys_addr_t rpmsg_paddr_map;

static struct rpmsg_virtio_shm_pool shm_pool;

static struct virtqueue* vq [VRING_COUNT];

static struct virtio_vring_info vrings [VRING_COUNT];

static struct virtio_device vdev;

static struct rpmsg_virtio_device rpmsg_vdev;

static struct rpmsg_device* rpmsg_dev;

static struct rpmsg_endpoint rpmsg_ept;

static struct k_thread rpmsg_thread;
static struct k_thread doorbell_thread;

static K_THREAD_STACK_DEFINE (rpmsg_stack, RPMSG_THREAD_STACK_SIZE);
static K_THREAD_STACK_DEFINE (doorbell_stack, DOORBELL_THREAD_STACK_SIZE);

static K_SEM_DEFINE (virtio_config_sem, 0, 1);

static struct k_poll_signal doorbell_sig = K_POLL_SIGNAL_INITIALIZER (doorbell_sig);

static struct k_poll_event doorbell_evt = K_POLL_EVENT_INITIALIZER (K_POLL_TYPE_SIGNAL, K_POLL_MODE_NOTIFY_ONLY, &doorbell_sig);

static volatile bool tx_msg = false;


static unsigned char vdev_get_status (struct virtio_device* vdev);
static void vdev_set_status (struct virtio_device* vdev,
                             unsigned char         status);
static uint32_t vdev_get_features (struct virtio_device* vdev);
static void vdev_set_features (struct virtio_device* vdev,
                               uint32_t              features);
static void vdev_nfy (struct virtqueue* vq);

static void rpmsg_ns_nfy (struct rpmsg_device* rdev,
                          const char*          name,
                          uint32_t             dest);
static int rpmsg_ept_nfy (struct rpmsg_endpoint* ept,
                          void*                  data,
		                  size_t                 len,
                          uint32_t               src,
                          void*                  priv);
static void rpmsg_service_unbind(struct rpmsg_endpoint* ept);

static struct fw_rsc_vdev* get_rsc_tbl_vdev (uintptr_t ptr);
static struct fw_rsc_vdev_vring* get_rsc_tbl_vring (uintptr_t  ptr,
                                                    size_t     idx);

static void doorbell_task (void* arg1,
                           void* arg2,
                           void* arg3);
static void rpmsg_task (void* arg1,
                        void* arg2,
                        void* arg3);


static const struct virtio_dispatch vdispatch = {
	.get_status   = vdev_get_status,
	.set_status   = vdev_set_status,
	.get_features = vdev_get_features,
	.set_features = vdev_set_features,
	.notify       = vdev_nfy,
};

static unsigned char vdev_get_status (struct virtio_device* vdev)
{
    volatile struct fw_rsc_vdev* rsc_tbl_vdev = get_rsc_tbl_vdev (rpmsg_ctx.ivshmem_vaddr);


    return (rsc_tbl_vdev->status);
}

static void vdev_set_status (struct virtio_device* vdev,
                             unsigned char         status)
{
    printf (INFO_STR "vdev_set_status  status: 0x%02x\n", status);
}

static uint32_t vdev_get_features (struct virtio_device* vdev)
{
    uint32_t features = 1 << VIRTIO_RPMSG_F_NS;


    printf (INFO_STR "vdev_get_features  features: 0x%08x\n", features);

	return (features);
}

static void vdev_set_features (struct virtio_device* vdev,
                               uint32_t              features)
{
    printf (INFO_STR "vdev_set_features  features: 0x%08x\n", features);
}

static void vdev_nfy (struct virtqueue* vq)
{
    /* For the moment, the interrupt number is always '0'. */
    ivshmem_int_peer (rpmsg_ctx.dev, rpmsg_ctx.target, 0);
}

static void rpmsg_ns_nfy (struct rpmsg_device* rdev,
                          const char*          name,
                          uint32_t             dest)
{
    printf (INFO_STR "rpmsg_ns_nfy  name: %s  dest: 0x%08x\n", name, dest);

	rpmsg_dev  = rdev;
	rpmsg_dest = dest;
}

static int rpmsg_ept_nfy (struct rpmsg_endpoint* ept,
                          void*                  data,
		                  size_t                 len,
                          uint32_t               src,
                          void*                  priv)
{
    printf (INFO_STR "RPMSG RX  len: %d  msg: \'%s\'\n", (int) len, (const char*) data);

    tx_msg = true;

	return (RPMSG_SUCCESS);
}

static void rpmsg_service_unbind (struct rpmsg_endpoint* ept)
{
    printf (INFO_STR "rpmsg_service_unbind\n");

	rpmsg_destroy_ept (ept);
}

static int send_message (void)
{
    msg_tx [0] = '\0';
    msg_cnt++;

    snprintf (msg_tx, sizeof (msg_tx), "%s  %d", MSG_STR, msg_cnt);

	printf(INFO_STR "send_message  Size: %zu\n", strlen (msg_tx) + 1);

	return (rpmsg_send (&rpmsg_ept, msg_tx, strlen (msg_tx) + 1));
}

static struct fw_rsc_vdev* get_rsc_tbl_vdev (uintptr_t ptr)
{
    return ((struct fw_rsc_vdev*) &(((rsc_tbl_t*) ptr)->vdev));
}

static struct fw_rsc_vdev_vring* get_rsc_tbl_vring (uintptr_t  ptr,
                                                    size_t     idx)
{
    return ((struct fw_rsc_vdev_vring*) &(((rsc_tbl_t*) ptr)->vring [idx]));
}                                                    

static void doorbell_task (void* arg1,
                           void* arg2,
                           void* arg3)
{
    bool virtio_ready = false;

	ARG_UNUSED (arg1);
	ARG_UNUSED (arg2);
	ARG_UNUSED (arg3);


    printf (INFO_STR "DOORBELL_TASK started ...\n");

	/* Signal that the IVSHMEM vPCI device is ready for initialization. */
	ivshmem_set_state (rpmsg_ctx.dev, 1);

	while (1) {
		int          vector;
		unsigned int signaled;

		k_poll (&doorbell_evt, 1, K_FOREVER);

		k_poll_signal_check (&doorbell_sig, &signaled, &vector);
		if (signaled == 0) {
			continue;
		}

        if (! (virtio_ready)) {
            volatile struct fw_rsc_vdev* rsc_tbl_vdev = get_rsc_tbl_vdev (rpmsg_ctx.ivshmem_vaddr);

            while ((rsc_tbl_vdev->status & VIRTIO_CONFIG_STATUS_DRIVER_OK) == 0) {
                /* Wait for the driver status to become ok. */
            }

            virtio_ready = true;

            k_sem_give (&virtio_config_sem);
        } else {
            virtqueue_notification (vq [VRING_RX]);
        }

		k_poll_signal_init (&doorbell_sig);
	}
}

static void rpmsg_task (void* arg1,
                        void* arg2,
                        void* arg3)
{
    int                      ret;
    uint32_t                 max_peers;
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;


	ARG_UNUSED (arg1);
	ARG_UNUSED (arg2);
	ARG_UNUSED (arg3);


    printf (INFO_STR "RPMSG_TASK started ...\n");

    /* Retrieve the IVSHMEM memory region. */
	rpmsg_ctx.dev = DEVICE_DT_GET (DT_NODELABEL (ivshmem0));
	if (! (device_is_ready (rpmsg_ctx.dev))) {
		printf (ERROR_STR "Could not get IVSHMEM device\n");

		return;
	}

    /* Retrieve the IVSHMEM RW memory section. */
	rpmsg_ctx.ivshmem_size = ivshmem_get_rw_mem_section (rpmsg_ctx.dev, &rpmsg_ctx.ivshmem_vaddr);
	if (rpmsg_ctx.ivshmem_size == 0) {
		printf (ERROR_STR "IVSHMEM RW size cannot be 0\n");
		
        return;
	}

	if (((void*) rpmsg_ctx.ivshmem_vaddr) == NULL) {
		printf (ERROR_STR "IVSHMEM virtual memory address cannot be null\n");
		
        return;
	}

    if (arch_page_phys_get ((void*) rpmsg_ctx.ivshmem_vaddr, &rpmsg_ctx.ivshmem_paddr) != 0) {
        printf (ERROR_STR "Failed to get physical address for IVSHMEM memory\n");
        
        return;
    }

    /* Retrieve the IVSHMEM device id. */
	rpmsg_ctx.id = ivshmem_get_id (rpmsg_ctx.dev);

    /* Calculate the target id. */
	max_peers = ivshmem_get_max_peers (rpmsg_ctx.dev);
    if (max_peers == 0) {
        printf (ERROR_STR "Invalid max_peers value: %d\n", max_peers);
        
        return;
    }

    rpmsg_ctx.target = rpmsg_ctx.id + 1;
    if (rpmsg_ctx.target >= max_peers) {
        rpmsg_ctx.target = 0;
    }

	printf (INFO_STR "Id: %d  Target: %d\n", rpmsg_ctx.id, rpmsg_ctx.target);

    /* Clear the IVSHMEM memory region. */
    memset ((void*) rpmsg_ctx.ivshmem_vaddr, 0, rpmsg_ctx.ivshmem_size);

    /* Set the resource table in the IVSHMEM's VDEV area. */
	printf (INFO_STR "Setting resource table\n");
    memcpy ((void*) rpmsg_ctx.ivshmem_vaddr, &rsc_tbl, sizeof (rsc_tbl));

    get_rsc_tbl_vring (rpmsg_ctx.ivshmem_vaddr, VRING_TX)->da  = ((uint32_t) rpmsg_ctx.ivshmem_paddr) + (RESOURCE_SIZE + (VRING_TX * VRING_SIZE));
    get_rsc_tbl_vring (rpmsg_ctx.ivshmem_vaddr, VRING_RX)->da  = ((uint32_t) rpmsg_ctx.ivshmem_paddr) + (RESOURCE_SIZE + (VRING_RX * VRING_SIZE));

    /* Start the IVSHMEM doorbell thread handler. */    
	ret = ivshmem_register_handler (rpmsg_ctx.dev, &doorbell_sig, 0);
    if (ret != 0) {
		printf (ERROR_STR "ivshmem_register_handler () failed with: %d\n", ret);

		return;
	}

    /* Create the doorbell handler. */
    k_thread_create (&doorbell_thread, doorbell_stack, DOORBELL_THREAD_STACK_SIZE, doorbell_task, NULL, NULL, NULL, K_PRIO_COOP (2), 0, K_NO_WAIT);

	ret = ivshmem_enable_interrupts (rpmsg_ctx.dev, true);
    if (ret != 0) {
		printf (ERROR_STR "ivshmem_enable_interrupts () failed with: %d\n", ret);

		return;
	}

	printf (INFO_STR "Awaiting VIRTIO config ready ...\n");

    /* Wait for the other side to have initialized the IVSHMEM layer. */
	k_sem_take (&virtio_config_sem, K_FOREVER);

	printf (INFO_STR "VIRTIO config is ready ...\n");

    /* Initialize OPEN-AMP's METAL layer. */
	/* metal_params.log_level = METAL_LOG_DEBUG; */
	ret = metal_init (&metal_params);
	if (ret < 0) {
		printf (ERROR_STR "metal_init () failed with: %d\n", ret);

		return;
	}

	/* Declare shared memory region */
    rpmsg_paddr_map = (metal_phys_addr_t) rpmsg_ctx.ivshmem_paddr;

	metal_io_init (&metal_io, (void*) rpmsg_ctx.ivshmem_vaddr, &rpmsg_paddr_map, rpmsg_ctx.ivshmem_size, -1, 0, NULL);

	printf (INFO_STR "State: %d\n", ivshmem_get_state (rpmsg_ctx.dev, rpmsg_ctx.id));

	/* Setup RPMSG queues */
	vq [VRING_TX] = virtqueue_allocate (VRING_NUM_DESCRS);
	if (vq [VRING_TX] == NULL) {
		printf (ERROR_STR "virtqueue_allocate (num_desc_extra: %d) failed with: %d\n", VRING_NUM_DESCRS, -ENOMEM);

        return;
    }

	vq [VRING_RX] = virtqueue_allocate (VRING_NUM_DESCRS);
	if (vq [VRING_RX] == NULL) {
		printf (ERROR_STR "virtqueue_allocate (num_desc_extra: %d) failed with: %d\n", VRING_NUM_DESCRS, -ENOMEM);

        return;
    }

	vrings [VRING_TX].io             = &metal_io;
	vrings [VRING_TX].info.vaddr     = (void*) (rpmsg_ctx.ivshmem_vaddr + (RESOURCE_SIZE + (VRING_TX * VRING_SIZE)));
	vrings [VRING_TX].info.num_descs = VRING_NUM_DESCRS;
	vrings [VRING_TX].info.align     = VRING_ALIGNMENT;
	vrings [VRING_TX].vq             = vq [VRING_TX];

	vrings [VRING_RX].io             = &metal_io;
	vrings [VRING_RX].info.vaddr     = (void*) (rpmsg_ctx.ivshmem_vaddr + (RESOURCE_SIZE + (VRING_RX * VRING_SIZE)));
	vrings [VRING_RX].info.num_descs = VRING_NUM_DESCRS;
	vrings [VRING_RX].info.align     = VRING_ALIGNMENT;
	vrings [VRING_RX].vq             = vq [VRING_RX];

	vdev.role        = RPMSG_REMOTE;
	vdev.vrings_num  = VRING_COUNT;
	vdev.func        = &vdispatch;
	vdev.vrings_info = vrings;

	printf (INFO_STR "IVSHMEM      Phy: 0x%016" PRIxPTR "  Virt: 0x%016" PRIxPTR "  Size: 0x%08x\n",
        rpmsg_ctx.ivshmem_paddr, rpmsg_ctx.ivshmem_vaddr, (unsigned int) rpmsg_ctx.ivshmem_size);
	printf (INFO_STR "VRING [TX]   Phy: 0x%016" PRIxPTR "  Virt: 0x%016" PRIxPTR "  Size: 0x%08x\n",
        (rpmsg_ctx.ivshmem_paddr + (RESOURCE_SIZE + (VRING_TX * VRING_SIZE))), (uintptr_t) vrings [VRING_TX].info.vaddr, VRING_SIZE);
	printf (INFO_STR "VRING [RX]   Phy: 0x%016" PRIxPTR "  Virt: 0x%016" PRIxPTR "  Size: 0x%08x\n",
        (rpmsg_ctx.ivshmem_paddr + (RESOURCE_SIZE + (VRING_RX * VRING_SIZE))), (uintptr_t) vrings [VRING_RX].info.vaddr, VRING_SIZE);

	rpmsg_virtio_init_shm_pool (&shm_pool, (void*) rpmsg_ctx.ivshmem_vaddr, rpmsg_ctx.ivshmem_size);

    /* Initialize the RPMSG VDEV. */
	ret = rpmsg_init_vdev (&rpmsg_vdev, &vdev, rpmsg_ns_nfy, &metal_io, &shm_pool);
	if (ret != 0) {
		printf (ERROR_STR "rpmsg_init_vdev () failed with: %d\n", ret);

		return;
	}

	rpmsg_dev = rpmsg_virtio_get_rpmsg_device (&rpmsg_vdev);
    if (! (rpmsg_dev)) {
        printf (ERROR_STR "rpmsg_virtio_get_rpmsg_device failed\n");

        return;
    }

    /* Create endpoint with the other side. */
	ret = rpmsg_create_ept (&rpmsg_ept, rpmsg_dev, RPMSG_SERVICE_NAME, RPMSG_ADDR_ANY, RPMSG_ADDR_ANY, rpmsg_ept_nfy, rpmsg_service_unbind);
	if (ret != 0) {
		printf (ERROR_STR "rpmsg_create_ept failed with: %d\n", ret);

		return;
	}

	printf (INFO_STR "Created endpoint for service \'%s\'\n", RPMSG_SERVICE_NAME);

    /* Wait for endpoint to become read. */
    while (! (is_rpmsg_ept_ready(&rpmsg_ept))){
        k_msleep(100);
    };

    printf (INFO_STR "Endpoint for service \'%s\' is connected\n", RPMSG_SERVICE_NAME);

    /* And now, send / receive messages. */
	while (1) {
        while (! (tx_msg)) {
            k_msleep(100);
        }

		ret = send_message ();
		if (ret < 0) {
            printf (ERROR_STR "rpmsg_send () failed with: %d\n", ret);

			return;
		}

        tx_msg = false;
	}

    while (1) {};
}

int main (void)
{
    printf ("RPMSG-TEST ...\n");

	k_thread_create (&rpmsg_thread, rpmsg_stack, RPMSG_THREAD_STACK_SIZE, rpmsg_task, NULL, NULL, NULL, K_PRIO_COOP (7), 0, K_NO_WAIT);

	return (0);
}
