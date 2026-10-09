/*
 */

#ifndef __COMMON_H__
#define __COMMON_H__

/* IVSHMEM memory layout                                    */
/*                                                          */
/*                       +------------------+ <- End of     */
/*                       |                  |    IVSHMEM    */
/*                       |                  |               */
/*                       |                  |               */
/*                       |  VRING BUFFER    |               */
/*                       |                  |               */
/*                       |                  |               */
/*                       |                  |               */
/*                       +------------------+ <- End of     */
/*                    ^  |                  |    IVSHMEM    */
/*     VRING_SIZE     |  |    VRING TX      |               */
/*                    v  |                  |               */
/*                       +------------------+               */
/*                    ^  |                  |               */
/*     VRING_SIZE     |  |    VRING RX      |               */
/*                    v  |                  |               */
/*                       +------------------+               */
/*                    ^  |                  |               */
/*     RESOURCE_SIZE  |  |  RESOURCE TABLE  |               */
/*                    v  |                  |               */
/*                       +------------------+  <- Start of  */
/*                                                IVSHMEM   */

#define RESOURCE_SIZE     0x1000

#define VRING_SIZE        0x1000
#define VRING_ALIGNMENT   32
#define VRING_NUM_DESCRS  16

#define VRING_TX           0
#define VRING_RX           1
#define VRING_COUNT		   2

#define RPMSG_SERVICE_NAME "rpmsg-raw"


#endif /* __COMMON_H__ */
