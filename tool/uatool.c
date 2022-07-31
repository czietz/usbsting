/*
 *  uatool: control program for USB_NET.STX
 *
 *  syntax: uatool [-c[a][t]] [filename]
 *      default: report statistics, plus arp cache contents, plus trace table (if present)
 *      -c  clears the statistics counters instead
 *      -ca clears counters & arp cache
 *      -ct clears counters & trace
 *      -cat clears everything
 *      output is to stdout, unless a filename is present, in which
 *      case the report will be written to it instead
 *
 *  v0.40   Jun/2018    roger burrows
 *      Initial version, based on unpublished SCSILINK code
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <osbind.h>
#include <string.h>             /* for memcpy(), memset(), strcmp() [builtin versions] */
#include <unistd.h>
#include "usbsting.h"

#define min(a,b)    ((a)<(b)?(a):(b))

#define PROGRAM "uatool"
#define VERSION "v0.40"

#define PORTNAME_LENGTH 20
#define ETH_HDR_LEN     (2*ETH_ALEN+2)

/*
 *  internal function prototypes
 */
static int display_arp(char *portname,USBNET_STATS *stats);
static void display_statistics(char *portname,USBNET_STATS *stats);
static int display_trace(char *portname,USBNET_STATS *stats);
static void display_trace_entry(USBNET_TRACE *t);
static int find_first_entry(int entries,USBNET_TRACE *table);
static long get_sting_cookie(void);
static void quit(char *s);
static void usage(void);

static void display_hex(FILE *report,uchar *start,uchar *end);
static void display_ip_header(FILE *report,uchar *start,uchar *end);
static void display_packet(FILE *report,uchar *start,uchar *end);
static char *format_macaddr(uchar *macaddr);


/*
 *  globals
 */
USBNET_STATS stats;
ARP_INFO *arp;
USBNET_TRACE *trace;
int clear_stats = 0;
int clear_arp = 0;
int clear_trace = 0;
FILE *report = NULL;
char driver_version[10] = "??.??";
char *portname = BASE_PORTNAME;

TPL *tpl;
STX *stx;


/************************************
*                                   *
*       INITIALISATION ROUTINES     *
*                                   *
************************************/

int main(int argc,char **argv)
{
PORT *ports;
DRV_LIST *sting_drivers;
char *p;
int n, rc, rc2;

    fprintf(stderr,"%s %s: Copyright 2018 by Roger Burrows\r\n",PROGRAM,VERSION);

    while((n=getopt(argc,argv,"c::")) != -1) {
        switch(n) {
        case 'c':
            if (optarg) {
                for (p = optarg; *p; p++) {
                    *p = *p | 0x20; /* tolower(*p); */
                    if (*p == 'a')
                        clear_arp++;
                    else if (*p == 't')
                        clear_trace++;
                }
            }
            clear_stats++;
            break;
        default:
            usage();
        }
    }

    if ((sting_drivers=(DRV_LIST *)Supexec(get_sting_cookie)) == NULL)
        quit("cannot find STinG cookie");

    if (strcmp(sting_drivers->magic,MAGIC) != 0)
        quit("STinG cookie points to invalid structure");

    tpl = (TPL *) (*sting_drivers->get_dftab)(TRANSPORT_DRIVER);
    stx = (STX *) (*sting_drivers->get_dftab)(MODULE_DRIVER);

    if (!tpl || !stx)
        quit("cannot get pointers to TPL/STX");

    /*
     *  find driver version
     */
    query_chains((void **)&ports,NULL,NULL);

    for ( ; ports; ports = ports->next) {
        if (stricmp(portname,ports->name) == 0) {
            strcpy(driver_version,ports->driver->version);
            break;
        }
    }

    if (optind < argc)
        report = fopen(argv[optind],"wa");

    if (!report)
        report = stdout;

    if (clear_stats) {
        rc = cntrl_port(portname,0L,CTL_ETHER_CLR_STAT);
        if (rc == 0)
            fprintf(report,"%s: statistics have been cleared\r\n",portname);
        else fprintf(report,"%s: cannot clear statistics\r\n",portname);
        rc2 = 0;
        if (clear_arp) {
            rc2 = cntrl_port(portname,0L,CTL_ETHER_CLR_ARPTABLE);
            if (rc2 == 0)
                fprintf(report,"ARP cache has been cleared\r\n");
            else fprintf(report,"Cannot clear ARP cache\r\n");
        }
        if (clear_trace) {
            rc2 = cntrl_port(portname,0L,CTL_ETHER_CLR_TRACE);
            if (rc2 == 0)
                fprintf(report,"Trace has been cleared\r\n");
            else fprintf(report,"Cannot clear trace\r\n");
        }
        return min(rc,rc2);
    }

    rc = cntrl_port(portname,(long)&stats,CTL_ETHER_GET_STAT);
    if (rc != 0) {
        fprintf(report,"%s: cannot get statistics\r\n",portname);
        return rc;
    }

    display_statistics(portname,&stats);

    rc2 = display_arp(portname,&stats);
    rc = min(rc,rc2);

    rc2 = display_trace(portname,&stats);
    rc = min(rc,rc2);

    return rc;
}

static int display_arp(char *portname,USBNET_STATS *stats)
{
ARP_INFO *info;
int i, rc;

    rc = 0;
    fprintf(report,"ARP cache\r\n");
    fprintf(report,"---------\r\n");
    fprintf(report,"Current number of entries = %ld\r\n",stats->arp_entries);
    if (stats->arp_entries > 0) {
        rc = -1;
        arp = calloc(stats->arp_entries,sizeof(ARP_INFO));
        if (arp) {
            rc = cntrl_port(portname,(long)arp,CTL_ETHER_GET_ARPTABLE);
            if (rc == 0) {
                for (i = 0, info = arp; i < stats->arp_entries; i++, info++)
                    if (info->ip_addr)
                        fprintf(report,"IP = %03ld.%03ld.%03ld.%03ld  MAC = %s\r\n",
                                info->ip_addr>>24,(info->ip_addr>>16)&0xff,(info->ip_addr>>8)&0xff,info->ip_addr&0xff,
                                format_macaddr(info->ether));
            } else fprintf(report,"Cannot get ARP cache table\r\n");
            free(arp);
        } else fprintf(report,"Cannot allocate memory for ARP cache table");
    }
    fprintf(report,"\r\n");

    return rc;
}

static void display_statistics(char *portname,USBNET_STATS *stats)
{
long n;

    fprintf(report,"%s statistics\r\n",portname);
    fprintf(report,"--------------------\r\n");

    fprintf(report,"  Driver version %s\r\n\r\n",driver_version);

    fprintf(report,"  Default MAC address: %s\r\n",format_macaddr(stats->hwaddr));
    fprintf(report,"  Current MAC address: %s\r\n\r\n",format_macaddr(stats->macaddr));

    fprintf(report,"  Input counts:\r\n");
    fprintf(report,"    %7ld reads\r\n",stats->read.total_packets);
    if (stats->read.failed)
        fprintf(report,"    *** %ld reads failed ***\r\n",stats->read.failed);
    fprintf(report,"    %7ld packets received (%ld valid, %ld invalid)\r\n",
            stats->receive.total_packets,stats->receive.good_packets,stats->receive.bad_packets);
    fprintf(report,"    %7ld packets processed (%ld broadcast IP, %ld normal IP, %ld ARP)\r\n",
            stats->process.broadcast_ip_packets+stats->process.normal_ip_packets+stats->process.arp_packets,
            stats->process.broadcast_ip_packets,stats->process.normal_ip_packets,stats->process.arp_packets);
    if (stats->process.bad_ip_packets+stats->process.bad_arp_packets) {
        if (stats->process.bad_ip_packets)
            fprintf(report,"    *** %ld invalid IP packets ***\r\n",stats->process.bad_ip_packets);
        if (stats->process.bad_arp_packets)
            fprintf(report,"    *** %ld invalid ARP packets ***\r\n",stats->process.bad_arp_packets);
    }

    fprintf(report,"  Output counts:\r\n");
    fprintf(report,"    %7ld packets queued for sending\r\n",stats->send.dequeued);
    if (stats->send.bad_length+stats->send.bad_host+stats->send.bad_network) {
        if (stats->send.bad_length)
            fprintf(report,"    *** %ld packets with invalid length ***\r\n",stats->send.bad_length);
        if (stats->send.bad_host)
            fprintf(report,"    *** %ld packets with invalid host ***\r\n",stats->send.bad_host);
        if (stats->send.bad_network)
            fprintf(report,"    *** %ld packets with invalid network ***\r\n",stats->send.bad_network);
    }
    fprintf(report,"    %7ld packets sent (%ld IP, %ld ARP)\r\n",
            stats->send.ip_packets+stats->send.arp_packets,stats->send.ip_packets,stats->send.arp_packets);
    if (stats->send.arp_packets_err)
        fprintf(report,"    *** %ld ARP packet sends failed ***\r\n",stats->send.arp_packets_err);
    fprintf(report,"    %7ld writes\r\n",stats->write.total_packets);
    if (stats->write.failed)
        fprintf(report,"    *** %ld writes failed ***\r\n",stats->write.failed);

    fprintf(report,"  ARP handling:\r\n");
    if (stats->arp.input_errors)
        fprintf(report,"    *** %ld ARP input packets with unusual contents ***\r\n",stats->arp.input_errors);
    if (stats->arp.opcode_errors)
        fprintf(report,"    *** %ld ARP input packets with unexpected opcodes ***\r\n",stats->arp.opcode_errors);
    fprintf(report,"    %7ld ARP requests received, %ld ARP answers received\r\n",stats->arp.requests_received,stats->arp.answers_received);
    fprintf(report,"    %7ld packets queued, %ld dequeued, %ld requeued (waiting for ARP)\r\n",
            stats->arp.wait_queued,stats->arp.wait_dequeued,stats->arp.wait_requeued);
    n = stats->arp.wait_queued + stats->arp.wait_requeued - stats->arp.wait_dequeued;
    if (n)
        fprintf(report,"    *** %ld packets are currently awaiting address resolution ***\r\n",n);
    fprintf(report,"\r\n");
}

static int display_trace(char *portname,USBNET_STATS *stats)
{
USBNET_TRACE *t;
int i, first_entry, rc = -1;

    if (stats->trace_entries == 0L)
        return 0;

    fprintf(report,"Trace table\r\n");
    fprintf(report,"-----------\r\n");
    fprintf(report,"Size = %ld entries\r\n\r\n",stats->trace_entries);

    trace = calloc(stats->trace_entries,sizeof(USBNET_TRACE));
    if (trace) {
        rc = cntrl_port(portname,(long)trace,CTL_ETHER_GET_TRACE);
        if (rc == 0) {
            first_entry = find_first_entry(stats->trace_entries,trace); /* look for lowest time */
            for (i = first_entry, t = trace+i; i < stats->trace_entries; i++, t++)
                if (t->time)
                    display_trace_entry(t);
            for (i = 0, t = trace; i < first_entry; i++, t++)
                if (t->time)
                    display_trace_entry(t);
            fprintf(report,"(end of trace)\r\n");
        } else fprintf(report,"Cannot get trace table\r\n");
        free(trace);
    } else fprintf(report,"Cannot allocate memory for trace table");

    return rc;
}

/*
 *  utility routines
 */
static long get_sting_cookie(void)
{
long *p;

    for (p = *(long **)_p_cookie; *p; p += 2)
        if (*p == STING_COOKIE)
            return *++p;

    return 0L;
}

static void quit(char *s)
{
    if (s)
        fprintf(stderr,"%s: %s\r\n",PROGRAM,s);
    exit(-1);
}

static void usage(void)
{
    fprintf(stderr,"uatool [-c[a][t]] [filename]\r\n");
    fprintf(stderr,"   default: report statistics plus ARP cache contents\r\n");
    fprintf(stderr,"            (plus trace if active)\r\n");
    fprintf(stderr,"   -c   clears the statistics counters instead\r\n");
    fprintf(stderr,"   -ca  clears counters & arp cache\r\n");
    fprintf(stderr,"   -ct  clears counters & trace\r\n");
    fprintf(stderr,"   -cat clears everything\r\n");
    fprintf(stderr,"   output is to stdout, unless a filename is present, in\r\n");
    fprintf(stderr,"   which case all output will be written to it instead\r\n");
    quit(NULL);
}

/*
 *  trace display routines (very similar to MintNet version!)
 */
static void display_trace_entry(USBNET_TRACE *t)
{
uchar *end;

    end = t->data + min(t->length,USBNET_TRACE_LEN);

    fprintf(report,"%08lx %c %5ld %4d ",t->time,t->type,t->rc,t->length);

    if ((t->type == TRACE_READ) || (t->type == TRACE_WRITE))
        display_packet(report,t->data,end);
    else display_hex(report,t->data,end);
}

static int find_first_entry(int entries,USBNET_TRACE *table)
{
USBNET_TRACE *t;
unsigned long lowest_time = ULONG_MAX;
int i, n = 0;

    for (i = 0, t = table; i < entries; i++, t++) {
        if (t->time < lowest_time) {
            lowest_time = t->time;
            n = i;
        }
    }

    return n;
}

static void display_packet(FILE *report,uchar *start,uchar *end)
{
char *type;
uchar *p;
int i;

    fprintf(report," %s",format_macaddr(start));        /* ethernet header */
    fprintf(report," <- %s",format_macaddr(start+ETH_ALEN));
    i = *(short *)(start+2*ETH_ALEN);
    switch(i) {
    case 0x0800:
        type = "IP";
        break;
    case 0x0806:
        type = "ARP";
        break;
    default:
        type = "???";
    }
    fprintf(report," %04x (%s)",i,type);
    p = start + ETH_HDR_LEN;
    if (i == 0x0800) {
        display_ip_header(report,p,end);
        p += (*p&0x0f)*4;
    }

    display_hex(report,p,end);
}

static void display_ip_header(FILE *report,uchar *start,uchar *end)
{
uchar *p;
int len;

    len = (*start & 0x0f) * 4;
    if (len == 0)
        return;
    if (start+len < end)
        end = start + len;

    fprintf(report,"\r\n        ");
    for (p = start; p < end; p++)
        fprintf(report," %02x",*p);
}

static void display_hex(FILE *report,uchar *start,uchar *end)
{
int i;
uchar *p;

    for (i = 0, p = start; p < end; i++) {
        if (i%32 == 0)
            fprintf(report,"\r\n        ");
        fprintf(report," %02x",*p++);
    }

    fprintf(report,"\r\n\n");
}

static char *format_macaddr(uchar *macaddr)
{
static char s[50];

    sprintf(s,"%02x:%02x:%02x:%02x:%02x:%02x",
            macaddr[0],macaddr[1],macaddr[2],macaddr[3],macaddr[4],macaddr[5]);
    return s;
}
