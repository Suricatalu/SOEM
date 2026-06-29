/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/** \file
 * \brief AF_XDP (XSK/UMEM) wrapper implementation, no global state.
 *
 * Derived from the legacy SOEM_XDP af_xdp_lib but refactored so that all
 * state lives in a caller-owned ec_xsk_t. Frame allocation, TX completion
 * reaping and RX refilling all operate on the instance passed in.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/mman.h>

#include <bpf/bpf.h>

#include "ec_xsk.h"

/* Frame allocator: operates on the instance's UMEM, no global state */
static uint64_t ec_xsk_alloc_frame(ec_xsk_umem_t *u)
{
   uint64_t frame;

   if (u->frame_free == 0)
   {
      return EC_XSK_INVALID_FRAME;
   }
   frame = u->frame_addr[--u->frame_free];
   u->frame_addr[u->frame_free] = EC_XSK_INVALID_FRAME;
   return frame;
}

static void ec_xsk_free_frame(ec_xsk_umem_t *u, uint64_t frame)
{
   u->frame_addr[u->frame_free++] = frame;
}

/* Reclaim completed TX descriptors back into the free frame pool */
static void ec_xsk_complete_tx(ec_xsk_t *self)
{
   unsigned int completed;
   uint32_t idx_cq = 0;

   if (self->outstanding_tx == 0)
   {
      return;
   }

   /* Kick the kernel to process the TX ring only when it asks for a wakeup */
   if (xsk_ring_prod__needs_wakeup(&self->tx))
   {
      sendto(xsk_socket__fd(self->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);
   }

   completed = xsk_ring_cons__peek(&self->umem.cq,
                                   XSK_RING_CONS__DEFAULT_NUM_DESCS,
                                   &idx_cq);
   if (completed > 0)
   {
      unsigned int i;
      for (i = 0; i < completed; i++)
      {
         ec_xsk_free_frame(&self->umem,
                           *xsk_ring_cons__comp_addr(&self->umem.cq, idx_cq++));
      }
      xsk_ring_cons__release(&self->umem.cq, completed);
      self->outstanding_tx -= completed < self->outstanding_tx
                                  ? completed
                                  : self->outstanding_tx;
   }
}

/* Configure UMEM on top of a caller-allocated buffer */
static int ec_xsk_configure_umem(ec_xsk_t *self, void *buffer, uint64_t size)
{
   int ret;

   ret = xsk_umem__create(&self->umem.umem, buffer, size,
                          &self->umem.fq, &self->umem.cq, NULL);
   if (ret)
   {
      errno = -ret;
      return -1;
   }
   self->umem.buffer = buffer;
   return 0;
}

/* Load and attach a custom XDP program, retrieve the xsks_map fd */
static int ec_xsk_setup_program(ec_xsk_t *self, const char *prog_path,
                                const char *prog_name)
{
   DECLARE_LIBBPF_OPTS(bpf_object_open_opts, opts);
   DECLARE_LIBXDP_OPTS(xdp_program_opts, xdp_opts, 0);
   struct bpf_map *map;
   int err;

   if (prog_path == NULL || prog_path[0] == '\0')
   {
      return 0; /* No custom program, rely on a pre-loaded one */
   }

   xdp_opts.open_filename = prog_path;
   xdp_opts.prog_name = prog_name;
   xdp_opts.opts = &opts;

   if (prog_name != NULL && prog_name[0] != '\0')
   {
      self->prog = xdp_program__create(&xdp_opts);
   }
   else
   {
      self->prog = xdp_program__open_file(prog_path, NULL, &opts);
   }
   if (libxdp_get_error(self->prog))
   {
      self->prog = NULL;
      return -1;
   }

   err = xdp_program__attach(self->prog, self->ifindex, XDP_MODE_UNSPEC, 0);
   if (err)
   {
      xdp_program__close(self->prog);
      self->prog = NULL;
      return -1;
   }

   map = bpf_object__find_map_by_name(xdp_program__bpf_obj(self->prog),
                                      "xsks_map");
   self->xsk_map_fd = bpf_map__fd(map);
   if (self->xsk_map_fd < 0)
   {
      return -1;
   }

   return 0;
}

int ec_xsk_open(ec_xsk_t *self, const char *ifname, uint32_t queue_id,
                const char *prog_path, const char *prog_name)
{
   struct rlimit rlim = {RLIM_INFINITY, RLIM_INFINITY};
   struct xsk_socket_config xsk_cfg;
   void *buffer = NULL;
   uint64_t buffer_size;
   uint32_t idx = 0;
   int ret;
   int i;

   memset(self, 0, sizeof(*self));
   self->xsk_map_fd = -1;

   /* Copy ifname safely (fixes legacy malloc/strncpy size bug) */
   strncpy(self->ifname, ifname, sizeof(self->ifname) - 1);
   self->ifname[sizeof(self->ifname) - 1] = '\0';
   self->queue_id = queue_id;
   self->ifindex = if_nametoindex(ifname);
   if (self->ifindex == 0)
   {
      return -1;
   }

   /* Allow unlimited locked memory for UMEM */
   if (setrlimit(RLIMIT_MEMLOCK, &rlim))
   {
      return -1;
   }

   /* Allocate page-aligned UMEM backing buffer */
   buffer_size = (uint64_t)EC_XSK_NUM_FRAMES * EC_XSK_FRAME_SIZE;
   if (posix_memalign(&buffer, (size_t)getpagesize(), (size_t)buffer_size))
   {
      return -1;
   }

   if (ec_xsk_configure_umem(self, buffer, buffer_size) != 0)
   {
      free(buffer);
      return -1;
   }

   /* Optionally load and attach our own XDP program */
   if (ec_xsk_setup_program(self, prog_path, prog_name) != 0)
   {
      ec_xsk_close(self);
      return -1;
   }

   /* Create the AF_XDP socket */
   memset(&xsk_cfg, 0, sizeof(xsk_cfg));
   xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
   xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
   xsk_cfg.xdp_flags = 0;
   /* XDP_USE_NEED_WAKEUP enables the need-wakeup contract: kernel sets a flag
    * on the rings, and we only wake the driver (recvfrom/sendto) when asked.
    * This is the prerequisite for NAPI busy polling below. */
   xsk_cfg.bind_flags =
       (EC_XSK_ZEROCOPY ? XDP_ZEROCOPY : XDP_COPY) | XDP_USE_NEED_WAKEUP;
   /* If we attached our own program above, inhibit libbpf's default loader */
   xsk_cfg.libbpf_flags =
       (self->prog != NULL) ? XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD : 0;

   ret = xsk_socket__create(&self->xsk, self->ifname, self->queue_id,
                            self->umem.umem, &self->rx, &self->tx, &xsk_cfg);
   if (ret)
   {
      errno = -ret;
      ec_xsk_close(self);
      return -1;
   }

   /* Enable NAPI busy polling on the XSK fd: recvfrom()/poll() will then poll
    * the NIC driver inline (instead of waiting for IRQ/softirq), cutting the
    * RX latency floor. busy_us is how long a poll may busy-spin NAPI; budget
    * is how many packets it may pull per iteration. */
   {
      int fd = xsk_socket__fd(self->xsk);
      int one = 1;
      int busy_us = EC_XSK_BUSYPOLL_US;
      int budget = EC_XSK_BUSYPOLL_BUDGET;
/* These socket options may be missing from older libc headers; define the
 * stable kernel ABI values as a fallback so the build does not break. */
#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL 69
#endif
#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET 70
#endif
      setsockopt(fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &one, sizeof(one));
      setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
      setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL_BUDGET, &budget, sizeof(budget));
   }

   /* Bind our XSK into the xsks_map so the XDP program can redirect to it */
   if (self->prog != NULL && self->xsk_map_fd >= 0)
   {
      ret = xsk_socket__update_xskmap(self->xsk, self->xsk_map_fd);
      if (ret)
      {
         ec_xsk_close(self);
         return -1;
      }
   }

   /* Initialize the free frame pool */
   for (i = 0; i < EC_XSK_NUM_FRAMES; i++)
   {
      self->umem.frame_addr[i] = (uint64_t)i * EC_XSK_FRAME_SIZE;
   }
   self->umem.frame_free = EC_XSK_NUM_FRAMES;

   /* Stuff the fill ring with buffers for the RX path */
   ret = xsk_ring_prod__reserve(&self->umem.fq,
                                XSK_RING_PROD__DEFAULT_NUM_DESCS, &idx);
   if (ret != XSK_RING_PROD__DEFAULT_NUM_DESCS)
   {
      ec_xsk_close(self);
      return -1;
   }
   for (i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS; i++)
   {
      *xsk_ring_prod__fill_addr(&self->umem.fq, idx++) =
          ec_xsk_alloc_frame(&self->umem);
   }
   xsk_ring_prod__submit(&self->umem.fq, XSK_RING_PROD__DEFAULT_NUM_DESCS);

   return 0;
}

void ec_xsk_close(ec_xsk_t *self)
{
   if (self == NULL)
   {
      return;
   }

   if (self->xsk)
   {
      xsk_socket__delete(self->xsk);
      self->xsk = NULL;
   }
   if (self->umem.umem)
   {
      xsk_umem__delete(self->umem.umem);
      self->umem.umem = NULL;
   }
   if (self->umem.buffer)
   {
      free(self->umem.buffer);
      self->umem.buffer = NULL;
   }
   if (self->prog)
   {
      xdp_program__detach(self->prog, self->ifindex, XDP_MODE_UNSPEC, 0);
      xdp_program__close(self->prog);
      self->prog = NULL;
   }
}

int ec_xsk_send(ec_xsk_t *self, const void *data, uint32_t len)
{
   uint64_t frame;
   uint8_t *dst;
   uint32_t tx_idx = 0;
   int retry = 0;

   if (self == NULL || data == NULL)
   {
      return -EINVAL;
   }
   if (len == 0 || len > EC_XSK_FRAME_SIZE)
   {
      return -EMSGSIZE;
   }

retry_alloc:
   frame = ec_xsk_alloc_frame(&self->umem);
   if (frame == EC_XSK_INVALID_FRAME)
   {
      ec_xsk_complete_tx(self);
      if (retry++ < 2)
      {
         goto retry_alloc;
      }
      return -EAGAIN;
   }

   /* Copy the frame into the UMEM slot */
   dst = xsk_umem__get_data(self->umem.buffer, frame);
   memcpy(dst, data, len);

   /* Reserve a TX descriptor; reclaim and retry once if the ring is full */
   if (xsk_ring_prod__reserve(&self->tx, 1, &tx_idx) != 1)
   {
      ec_xsk_complete_tx(self);
      if (xsk_ring_prod__reserve(&self->tx, 1, &tx_idx) != 1)
      {
         ec_xsk_free_frame(&self->umem, frame);
         return -EAGAIN;
      }
   }

   xsk_ring_prod__tx_desc(&self->tx, tx_idx)->addr = frame;
   xsk_ring_prod__tx_desc(&self->tx, tx_idx)->len = len;
   xsk_ring_prod__submit(&self->tx, 1);
   self->outstanding_tx++;

   /* Kick the kernel and reap completions immediately (low-latency path) */
   ec_xsk_complete_tx(self);

   return 0;
}

int ec_xsk_recv(ec_xsk_t *self, void *buf, uint32_t buf_len)
{
   uint32_t idx_rx = 0;
   uint32_t idx_fq = 0;
   unsigned int rcvd = 0;
   unsigned int reserved;
   int copied = 0;

   if (self == NULL || buf == NULL)
   {
      return 0;
   }

   /* Drive NAPI busy-poll: with SO_PREFER_BUSY_POLL set, this non-blocking
    * recvfrom() makes the kernel poll the NIC driver inline and move frames
    * into the RX ring right now, instead of waiting for the next IRQ/softirq.
    * SO_BUSY_POLL bounds how long it spins, so this returns within ~budget us. */
   if (xsk_ring_prod__needs_wakeup(&self->umem.fq))
   {
      recvfrom(xsk_socket__fd(self->xsk), NULL, 0, MSG_DONTWAIT, NULL, NULL);
   }

   rcvd = xsk_ring_cons__peek(&self->rx, EC_XSK_RX_BATCH, &idx_rx);
   if (!rcvd)
   {
      return 0;
   }

   /* Reserve space in the fill ring to recycle the consumed frames */
   reserved = xsk_ring_prod__reserve(&self->umem.fq, rcvd, &idx_fq);
   if (reserved != rcvd)
   {
      xsk_ring_cons__release(&self->rx, rcvd);
      return 0;
   }

   /* Copy out only the first frame into the caller buffer; the indexed
    * buffer logic in nicdrv expects one frame per call. Any extra frames
    * in the batch are still recycled to the fill ring. */
   {
      unsigned int i;
      for (i = 0; i < rcvd; i++)
      {
         const struct xdp_desc *desc =
             xsk_ring_cons__rx_desc(&self->rx, idx_rx + i);
         if (i == 0)
         {
            uint32_t len = desc->len;
            void *src = xsk_umem__get_data(self->umem.buffer, desc->addr);
            if (len > buf_len)
            {
               len = buf_len;
            }
            memcpy(buf, src, len);
            copied = (int)len;
         }
         *xsk_ring_prod__fill_addr(&self->umem.fq, idx_fq + i) = desc->addr;
      }
   }

   xsk_ring_prod__submit(&self->umem.fq, rcvd);
   xsk_ring_cons__release(&self->rx, rcvd);

   return copied;
}
