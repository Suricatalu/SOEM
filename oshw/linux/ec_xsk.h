/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/** \file
 * \brief AF_XDP (XSK/UMEM) wrapper with no global state.
 *
 * All state is owned by the caller through ec_xsk_t. Each SOEM port
 * (primary or redundant) holds its own ec_xsk_t instance, so multiple
 * EtherCAT contexts and redundant dual-path setups are supported
 * without any process-wide singletons.
 */

#ifndef _ec_xsk_h_
#define _ec_xsk_h_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <xdp/xsk.h>
#include <xdp/libxdp.h>
#include "soem/ec_options.h"

/* UMEM frame bookkeeping for one XSK socket */
typedef struct
{
   struct xsk_umem *umem;   /* UMEM handle */
   void *buffer;            /* UMEM backing memory */
   struct xsk_ring_prod fq; /* fill ring   (app -> kernel) */
   struct xsk_ring_cons cq; /* completion  (kernel -> app) */
   uint64_t frame_addr[EC_XSK_NUM_FRAMES];
   uint32_t frame_free;
} ec_xsk_umem_t;

/* One AF_XDP socket instance (per SOEM port) */
typedef struct
{
   struct xsk_socket *xsk;  /* AF_XDP socket */
   struct xsk_ring_cons rx; /* RX ring */
   struct xsk_ring_prod tx; /* TX ring */
   ec_xsk_umem_t umem;      /* this socket's UMEM */
   struct xdp_program *prog; /* attached eBPF program (NULL if none) */
   int xsk_map_fd;
   uint32_t outstanding_tx;
   uint32_t queue_id;
   uint32_t ifindex;
   char ifname[EC_MAXLEN_ADAPTERNAME];
} ec_xsk_t;

/* Lifecycle */
int ec_xsk_open(ec_xsk_t *self, const char *ifname, uint32_t queue_id,
                const char *prog_path, const char *prog_name);
void ec_xsk_close(ec_xsk_t *self);

/* Data path */
int ec_xsk_send(ec_xsk_t *self, const void *data, uint32_t len);
int ec_xsk_recv(ec_xsk_t *self, void *buf, uint32_t buf_len);

#ifdef __cplusplus
}
#endif

#endif
