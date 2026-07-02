/** \file
 * \brief Example code for Simple Open EtherCAT master
 *
 * Usage: simple_ng IFNAME1
 * IFNAME1 is the NIC interface name, e.g. 'eth0'
 *
 * This is a minimal test.
 */

#include "soem/soem.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Number of measurement iterations (also bounds the sample buffer). */
#define SIMPLE_NG_ITERATIONS 10000

typedef struct
{
   ecx_contextt context;
   char *iface;
   uint8 group;
   int roundtrip_time;
   uint8 map[4096];
} Fieldbus;

static void
fieldbus_initialize(Fieldbus *fieldbus, char *iface)
{
   /* Let's start by 0-filling `fieldbus` to avoid surprises */
   memset(fieldbus, 0, sizeof(*fieldbus));

   fieldbus->iface = iface;
   fieldbus->group = 0;
   fieldbus->roundtrip_time = 0;
}

static int
fieldbus_roundtrip(Fieldbus *fieldbus)
{
   ecx_contextt *context;
   ec_timet start, end, diff;
   int wkc;

   context = &fieldbus->context;

   start = osal_current_time();
   ecx_send_processdata(context);
   wkc = ecx_receive_processdata(context, EC_TIMEOUTRET);
   end = osal_current_time();
   osal_time_diff(&start, &end, &diff);
   fieldbus->roundtrip_time = (int)(diff.tv_sec * 1000000 + diff.tv_nsec / 1000);

   return wkc;
}

static boolean
fieldbus_start(Fieldbus *fieldbus)
{
   ecx_contextt *context;
   ec_groupt *grp;
   ec_slavet *slave;
   int i;

   context = &fieldbus->context;
   grp = context->grouplist + fieldbus->group;

   printf("Initializing SOEM on '%s'... ", fieldbus->iface);
   if (!ecx_init(context, fieldbus->iface))
   {
      printf("no socket connection\n");
      return FALSE;
   }
   printf("done\n");

   printf("Finding autoconfig slaves... ");
   if (ecx_config_init(context) <= 0)
   {
      printf("no slaves found\n");
      return FALSE;
   }
   printf("%d slaves found\n", context->slavecount);

   printf("Sequential mapping of I/O... ");
   ecx_config_map_group(context, fieldbus->map, fieldbus->group);
   printf("mapped %dO+%dI bytes from %d segments",
          grp->Obytes, grp->Ibytes, grp->nsegments);
   if (grp->nsegments > 1)
   {
      /* Show how slaves are distributed */
      for (i = 0; i < grp->nsegments; ++i)
      {
         printf("%s%d", i == 0 ? " (" : "+", grp->IOsegment[i]);
      }
      printf(" slaves)");
   }
   printf("\n");

   printf("Configuring distributed clock... ");
   ecx_configdc(context);
   printf("done\n");

   printf("Waiting for all slaves in safe operational... ");
   ecx_statecheck(context, 0, EC_STATE_SAFE_OP, EC_TIMEOUTSTATE * 4);
   printf("done\n");

   printf("Send a roundtrip to make outputs in slaves happy... ");
   fieldbus_roundtrip(fieldbus);
   printf("done\n");

   printf("Setting operational state..");
   /* Act on slave 0 (a virtual slave used for broadcasting) */
   slave = context->slavelist;
   slave->state = EC_STATE_OPERATIONAL;
   ecx_writestate(context, 0);
   /* Poll the result ten times before giving up */
   for (i = 0; i < 10; ++i)
   {
      printf(".");
      fieldbus_roundtrip(fieldbus);
      ecx_statecheck(context, 0, EC_STATE_OPERATIONAL, EC_TIMEOUTSTATE / 10);
      if (slave->state == EC_STATE_OPERATIONAL)
      {
         printf(" all slaves are now operational\n");
         return TRUE;
      }
   }

   printf(" failed,");
   ecx_readstate(context);
   for (i = 1; i <= context->slavecount; ++i)
   {
      slave = context->slavelist + i;
      if (slave->state != EC_STATE_OPERATIONAL)
      {
         printf(" slave %d is 0x%04X (AL-status=0x%04X %s)",
                i, slave->state, slave->ALstatuscode,
                ec_ALstatuscode2string(slave->ALstatuscode));
      }
   }
   printf("\n");

   return FALSE;
}

static void
fieldbus_stop(Fieldbus *fieldbus)
{
   ecx_contextt *context;
   ec_slavet *slave;

   context = &fieldbus->context;
   /* Act on slave 0 (a virtual slave used for broadcasting) */
   slave = context->slavelist;

   printf("Requesting init state on all slaves... ");
   slave->state = EC_STATE_INIT;
   ecx_writestate(context, 0);
   printf("done\n");

   printf("Close socket... ");
   ecx_close(context);
   printf("done\n");
}

static boolean
fieldbus_dump(Fieldbus *fieldbus)
{
   ecx_contextt *context;
   ec_groupt *grp;
   uint32 n;
   int wkc, expected_wkc;

   context = &fieldbus->context;
   grp = context->grouplist + fieldbus->group;

   wkc = fieldbus_roundtrip(fieldbus);
   expected_wkc = grp->outputsWKC * 2 + grp->inputsWKC;
   printf("%6d usec  WKC %d", fieldbus->roundtrip_time, wkc);
   if (wkc < expected_wkc)
   {
      printf(" wrong (expected %d)\n", expected_wkc);
      return FALSE;
   }

   printf("  O:");
   for (n = 0; n < grp->Obytes; ++n)
   {
      printf(" %02X", grp->outputs[n]);
   }
   printf("  I:");
   for (n = 0; n < grp->Ibytes; ++n)
   {
      printf(" %02X", grp->inputs[n]);
   }
   printf("  T: %lld\r", (long long)context->DCtime);
   return TRUE;
}

static void
fieldbus_check_state(Fieldbus *fieldbus)
{
   ecx_contextt *context;
   ec_groupt *grp;
   ec_slavet *slave;
   int i;

   context = &fieldbus->context;
   grp = context->grouplist + fieldbus->group;
   grp->docheckstate = FALSE;
   ecx_readstate(context);
   for (i = 1; i <= context->slavecount; ++i)
   {
      slave = context->slavelist + i;
      if (slave->group != fieldbus->group)
      {
         /* This slave is part of another group: do nothing */
      }
      else if (slave->state != EC_STATE_OPERATIONAL)
      {
         grp->docheckstate = TRUE;
         if (slave->state == EC_STATE_SAFE_OP + EC_STATE_ERROR)
         {
            printf("* Slave %d is in SAFE_OP+ERROR, attempting ACK\n", i);
            slave->state = EC_STATE_SAFE_OP + EC_STATE_ACK;
            ecx_writestate(context, i);
         }
         else if (slave->state == EC_STATE_SAFE_OP)
         {
            printf("* Slave %d is in SAFE_OP, change to OPERATIONAL\n", i);
            slave->state = EC_STATE_OPERATIONAL;
            ecx_writestate(context, i);
         }
         else if (slave->state > EC_STATE_NONE)
         {
            if (ecx_reconfig_slave(context, i, EC_TIMEOUTRET))
            {
               slave->islost = FALSE;
               printf("* Slave %d reconfigured\n", i);
            }
         }
         else if (!slave->islost)
         {
            ecx_statecheck(context, i, EC_STATE_OPERATIONAL, EC_TIMEOUTRET);
            if (slave->state == EC_STATE_NONE)
            {
               slave->islost = TRUE;
               printf("* Slave %d lost\n", i);
            }
         }
      }
      else if (slave->islost)
      {
         if (slave->state != EC_STATE_NONE)
         {
            slave->islost = FALSE;
            printf("* Slave %d found\n", i);
         }
         else if (ecx_recover_slave(context, i, EC_TIMEOUTRET))
         {
            slave->islost = FALSE;
            printf("* Slave %d recovered\n", i);
         }
      }
   }

   if (!grp->docheckstate)
   {
      printf("All slaves resumed OPERATIONAL\n");
   }
}

/* Ascending integer comparator for qsort (used by the latency percentiles). */
static int
cmp_int_asc(const void *a, const void *b)
{
   int ia = *(const int *)a;
   int ib = *(const int *)b;
   return (ia > ib) - (ia < ib);
}

/* Nearest-rank percentile from an ascending-sorted array (p in 0..100).
 * Latency is heavy-tailed, so percentiles (P50/P99) describe it far better
 * than mean/stddev. */
static int
percentile_usec(const int *sorted, int n, double p)
{
   int rank;
   if (n <= 0)
   {
      return 0;
   }
   rank = (int)ceil(p / 100.0 * n);
   if (rank < 1)
   {
      rank = 1;
   }
   if (rank > n)
   {
      rank = n;
   }
   return sorted[rank - 1];
}

int main(int argc, char *argv[])
{
   Fieldbus fieldbus;

   if (argc != 2)
   {
      ec_adaptert *adapter = NULL;
      ec_adaptert *head = NULL;
      printf("Usage: simple_ng IFNAME1\n"
             "IFNAME1 is the NIC interface name, e.g. 'eth0'\n");

      printf("\nAvailable adapters:\n");
      head = adapter = ec_find_adapters();
      while (adapter != NULL)
      {
         printf("    - %s  (%s)\n", adapter->name, adapter->desc);
         adapter = adapter->next;
      }
      ec_free_adapters(head);
      return 1;
   }

   fieldbus_initialize(&fieldbus, argv[1]);

#if EC_RT_FIFO
   /* Promote this thread to real-time BEFORE the measurement loop. Requires
    * root/CAP_SYS_NICE; on failure we only warn and continue (non-fatal). */
   if (!osal_thread_set_realtime(EC_RT_FIFO_PRIO))
   {
      printf("warning: SCHED_FIFO not enabled (need root/CAP_SYS_NICE)\n");
   }
#endif

   if (fieldbus_start(&fieldbus))
   {
      int i, min_time, max_time;
      ec_groupt *grp;
      /* Running accumulators for mean/stddev over successful roundtrips.
       * Kept separate from the timing logic so measurement is unaffected. */
      double sum_time = 0.0;
      double sum_sq_time = 0.0;
      int sample_count = 0;
      /* Per-sample buffer so we can compute percentiles (P50/P99) at the end. */
      int samples[SIMPLE_NG_ITERATIONS];
      boolean dump_ok;
      min_time = max_time = 0;
      grp = fieldbus.context.grouplist + fieldbus.group;
      for (i = 1; i <= SIMPLE_NG_ITERATIONS; ++i)
      {
         /* Cycle through 8 output bits: light one bit at a time */
         if (grp->Obytes > 0)
         {
            memset(grp->outputs, 0, grp->Obytes);
            grp->outputs[0] = (uint8)(1 << ((i - 1) % 8));
         }
         printf("Iteration %4d:", i);
         /* Call fieldbus_dump() exactly once (it performs the roundtrip);
          * capture its result so statistics can reuse the same sample without
          * triggering an extra measurement. */
         dump_ok = fieldbus_dump(&fieldbus);
         if (!dump_ok)
         {
            fieldbus_check_state(&fieldbus);
         }
         else if (i == 1)
         {
            min_time = max_time = fieldbus.roundtrip_time;
         }
         else if (fieldbus.roundtrip_time < min_time)
         {
            min_time = fieldbus.roundtrip_time;
         }
         else if (fieldbus.roundtrip_time > max_time)
         {
            max_time = fieldbus.roundtrip_time;
         }

         /* Accumulate statistics only for valid roundtrips (same successful
          * path used by the min/max logic above). This does not alter the
          * measured roundtrip_time in any way. */
         if (dump_ok)
         {
            double t = (double)fieldbus.roundtrip_time;
            sum_time += t;
            sum_sq_time += t * t;
            samples[sample_count] = fieldbus.roundtrip_time;
            ++sample_count;
         }
         osal_usleep(2000);
      }
      printf("\nRoundtrip time (usec): min %d max %d\n", min_time, max_time);
      if (sample_count > 0)
      {
         double mean = sum_time / sample_count;
         double stddev = 0.0;
         if (sample_count > 1)
         {
            /* Sample variance (N-1); clamp tiny negatives from rounding. */
            double variance =
                (sum_sq_time - sum_time * mean) / (sample_count - 1);
            if (variance < 0.0)
            {
               variance = 0.0;
            }
            stddev = sqrt(variance);
         }
         printf("Roundtrip time (usec): mean %.2f stddev %.2f over %d samples\n",
                mean, stddev, sample_count);

         /* Percentiles describe the heavy-tailed latency far better than
          * mean/stddev. Sort in place (original order is no longer needed). */
         qsort(samples, sample_count, sizeof(samples[0]), cmp_int_asc);
         printf("Roundtrip time (usec): P50 %d P99 %d over %d samples\n",
                percentile_usec(samples, sample_count, 50.0),
                percentile_usec(samples, sample_count, 99.0),
                sample_count);
      }
      fieldbus_stop(&fieldbus);
   }

   return 0;
}
