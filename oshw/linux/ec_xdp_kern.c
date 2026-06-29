/* SPDX-License-Identifier: GPL-2.0 */

/* XDP/eBPF kernel program for SOEM AF_XDP backend.
 *
 * Only EtherCAT frames (EtherType 0x88A4) are redirected to the bound
 * AF_XDP socket. Every other frame is passed up to the normal kernel
 * network stack via XDP_PASS, so management traffic (SSH, ICMP, ...)
 * sharing the same NIC is never stolen.
 */

#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

#define ETH_P_ECAT 0x88A4

struct
{
   __uint(type, BPF_MAP_TYPE_XSKMAP);
   __uint(max_entries, 64);
   __type(key, __u32);
   __type(value, __u32);
} xsks_map SEC(".maps");

SEC("xdp")
int ec_xdp_redirect(struct xdp_md *ctx)
{
   void *data = (void *)(long)ctx->data;
   void *data_end = (void *)(long)ctx->data_end;
   struct ethhdr *eth = data;

   /* Verifier-required bounds check */
   if ((void *)(eth + 1) > data_end)
      return XDP_PASS;

   /* Only steal EtherCAT frames; pass everything else to the kernel */
   if (eth->h_proto != bpf_htons(ETH_P_ECAT))
      return XDP_PASS;

   __u32 index = ctx->rx_queue_index;
   if (bpf_map_lookup_elem(&xsks_map, &index))
      return bpf_redirect_map(&xsks_map, index, 0);

   return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
