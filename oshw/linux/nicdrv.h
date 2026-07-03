/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/** \file
 * \brief
 * Headerfile for nicdrv.c
 */

#ifndef _nicdrvh_
#define _nicdrvh_

#ifdef __cplusplus
extern "C" {
#endif

#include <pthread.h>
#if defined(EC_USE_XDP)
#include "ec_xsk.h"
#endif

/** pointer structure to Tx and Rx stacks */
typedef struct
{
   /** socket connection used */
   int *sock;
   /** tx buffer */
   ec_bufT (*txbuf)[EC_MAXBUF];
   /** tx buffer lengths */
   int (*txbuflength)[EC_MAXBUF];
   /** temporary receive buffer */
   ec_bufT *tempbuf;
   /** rx buffers */
   ec_bufT (*rxbuf)[EC_MAXBUF];
   /** rx buffer status fields */
   int (*rxbufstat)[EC_MAXBUF];
   /** received MAC source address (middle word) */
   int (*rxsa)[EC_MAXBUF];
   /** number of received frames */
   uint64 rxcnt;
} ec_stackT;

/** pointer structure to buffers for redundant port */
typedef struct
{
   ec_stackT stack;
   int sockhandle;
   /** rx buffers */
   ec_bufT rxbuf[EC_MAXBUF];
   /** rx buffer status */
   int rxbufstat[EC_MAXBUF];
   /** rx MAC source address */
   int rxsa[EC_MAXBUF];
   /** temporary rx buffer */
   ec_bufT tempinbuf;
#if defined(EC_USE_XDP)
   /** per-redundant-port AF_XDP backend state */
   ec_xsk_t xsk;
#endif
} ecx_redportt;

/** pointer structure to buffers, vars and mutexes for port instantiation */
typedef struct
{
   ec_stackT stack;
   int sockhandle;
   /** rx buffers */
   ec_bufT rxbuf[EC_MAXBUF];
   /** rx buffer status */
   int rxbufstat[EC_MAXBUF];
   /** rx MAC source address */
   int rxsa[EC_MAXBUF];
   /** temporary rx buffer */
   ec_bufT tempinbuf;
   /** temporary rx buffer status */
   int tempinbufs;
   /** transmit buffers */
   ec_bufT txbuf[EC_MAXBUF];
   /** transmit buffer lengths */
   int txbuflength[EC_MAXBUF];
   /** temporary tx buffer */
   ec_bufT txbuf2;
   /** temporary tx buffer length */
   int txbuflength2;
   /** last used frame index */
   uint8 lastidx;
   /** current redundancy state */
   int redstate;
   /** selected NIC backend for this port (AF_PACKET / AF_XDP) **/
   int nicbackend;
   /** pointer to redundancy port and buffers */
   ecx_redportt *redport;
   pthread_mutex_t getindex_mutex;
   pthread_mutex_t tx_mutex;
   pthread_mutex_t rx_mutex;
#if defined(EC_USE_XDP)
   /** per-port AF_XDP backend state */
   ec_xsk_t xsk;
#endif
} ecx_portt;

extern const uint16 priMAC[3];
extern const uint16 secMAC[3];

void ec_setupheader(void *p);
int ecx_setupnic(ecx_portt *port, const char *ifname, int secondary);
int ecx_closenic(ecx_portt *port);
void ecx_setbufstat(ecx_portt *port, uint8 idx, int bufstat);
uint8 ecx_getindex(ecx_portt *port);
int ecx_outframe(ecx_portt *port, uint8 idx, int sock);
int ecx_outframe_red(ecx_portt *port, uint8 idx);
int ecx_waitinframe(ecx_portt *port, uint8 idx, int timeout);
int ecx_srconfirm(ecx_portt *port, uint8 idx, int timeout);

#ifdef __cplusplus
}
#endif

#endif
