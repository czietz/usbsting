/*
 * main line code for STinG port driver, for USB ethernet devices
 * using the ASIX chip set and the PicoWifi adapter
 *
 * Module to install and activate the port and to interface with,
 * transmit to, and receive from, the STinG kernel.
 *
 * Copyright Roger Burrows (June 2018), based on unpublished SCSILINK code
 *
 * Modified for PicoWifi adapter by Christian Zietz 2022.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

/*
 * IMPORTANT: you must compile with default short ints because the
 * STinG & USB APIs expect this ...
 */
#if __SIZEOF_INT__ != 2
# error you must compile with short ints!
#endif

#include <stdio.h>
#include <string.h>
#include <osbind.h>

#include "usb.h"        /* 'standard' USB stuff */
#include "usb_api.h"

#include "usbsting.h"   /* application-specific */
#include "arpcache.h"
#include "asix.h"
#include "picowifi.h"

/*
 *  program parameters
 */
#define DRIVER_NAME     "USB_NET.STX"
                                /* the following values are returned to STinG */
#define MODULE_NAME     "USB Network"
#define MODULE_VERSION  "00.50"
#define MODULE_DAY      31
#define MODULE_MONTH    07
#define MODULE_YEAR     2022
#define MODULE_DATE     (((MODULE_YEAR-1980)<<9)|(MODULE_MONTH<<5)|(MODULE_DAY))    /* GEMDOS internal format */
#define MODULE_AUTHOR   "Roger Burrows & Christian Zietz"

#ifdef TRACE
  #define TRACE_ENTRIES 1000
#else
  #define TRACE_ENTRIES 0
#endif

/*
 *  debug section
 */
#ifdef ENABLE_DEBUG
# define DEBUG(x) printf x
#else
# define DEBUG(x)
#endif

/*
 *  structures
 */
struct extended_port {              /* extended PORT structure */
    PORT port;                          /* MUST be first entry in structure, so we can cast the address */
    long magic;                         /* for verification */
#define EXTPORT_MAGIC   0x01071867L
    IP_DGRAM *arpwait;                  /* queue for dgrams waiting for address resolution */
    char unused;
    char interface_up;
    char hwaddr[ETH_ALEN];              /* set from hardware */
    char macaddr[ETH_ALEN];             /* initially the same as hwaddr[], updated by CTL_ETHER_SET_MAC */
    USBNET_STATS stats;
    char name[16];
#ifdef TRACE
    struct {                            /* trace table */
        USBNET_TRACE *next;
        USBNET_TRACE *first;
        USBNET_TRACE *last;
        USBNET_TRACE entry[TRACE_ENTRIES];
    } trace;
#endif
};

/*
 * USB API
 */
static struct usb_module_api *api;
static struct ueth_data ueth_dev;

/*
 *  other strings
 */
#define BROADCAST_ADDR  "\xff\xff\xff\xff\xff\xff"

/*
 *  error messages
 */
#define BADSTART        ": STinG extension module. Must only be started by STinG!\n"
#define NOSTINGCOOKIE   " not installed: cannot find STinG cookie\n"
#define NOMAGIC         " not installed: STinG cookie points to invalid structure\n"
#define NODRIVERS       " not installed: cannot get pointers to TPL/STX\n"
#define NOUSBCOOKIE     " not installed: cannot find _USB cookie\n"
#define NOREGISTER      " not installed: cannot register USB device\n"


/*
 *  internal function prototypes
 */
static void *allocmem(long size);
static int16 close_device(struct extended_port *x);
static int16 control_device(PORT *port,uint32 argument,int16 code);
static IP_DGRAM *dequeue_dgram(IP_DGRAM **queue);
static void display_message(char *s);
static void empty_queue(IP_DGRAM **queue);
static int16 get_mac_address(struct extended_port *x,char *macaddr);
static int32 get_frb_cookie(void);
static int32 get_sting_cookie(void);
static int32 get_usb_cookie(void);
static void init_ext_port(struct extended_port *x);
static void install(BASPAG *);
static int16 open_device(struct extended_port *x);
static int16 process_arp(struct extended_port *x,ARP *arp);
static int16 process_ip(struct extended_port *x,IP_HDR *ip_hdr,int length);
static int16 process_output(struct extended_port *x,IP_DGRAM *dgram);
static void queue_dgram(IP_DGRAM **queue,IP_DGRAM *dgram);
static void quit(char *s);
static int16 read_device(struct extended_port *x,ENET_PACKET *sp);
static void receive_dgrams(PORT *port);
static int16 send_arp(struct extended_port *x);
static void send_dgrams(PORT *port);
static int16 set_device_state(PORT *port,int16 state);
static int16 write_device(struct extended_port *x,char *buffer,int length);

#ifdef TRACE
static void trace(struct extended_port *x,char type,long rc,int length,char *data);
static void trace_init(struct extended_port *x);
#else
#define trace(a,b,c,d,e)
#define trace_init(a)
#endif

#define min(a,b)    ((a)<(b)?(a):(b))

/*
 *  the key STinG variables
 */
int16 arpcache_entries = 0;         /* returned by arp_init() */

struct extended_port *xbase;        /* ptr to extended port structure */

DRIVER usbnet_driver =              /* this is hooked into STinG's driver chain */
{
    set_device_state, control_device, send_dgrams, receive_dgrams,
    MODULE_NAME, MODULE_VERSION, MODULE_DATE, MODULE_AUTHOR,
    NULL, NULL
};

TPL *tpl;
STX *stx;

/*
 *  supported hardware variants: when control_device() is called with a code
 *  of CTL_ETHER_INQ_SUPPTYPE, it returns a pointer to this structure.
 */
static char *suppHardware[] =
{
    "No selection",
    "USB Network",
    NULL
};

/*
 *  the Ethernet packet sent on ARP request or answer
 */
static ARP_PACKET arp_enet_pkt;

/*
 *  Ethernet packet sent for IP.  Ethernet header, IP header, IP options and
 *  IP data of STinG IP datagrams get copied here one after the other.
 */
static ENET_PACKET op;

/*
 *  Input packet
 */
static ENET_PACKET ip;

/*
 * MAC address
 */
static unsigned char mac[ETH_ALEN];


/************************************
*                                   *
*       USB DEVICE INTERFACE        *
*                                   *
************************************/

static int asix_found = 0;
static int picowifi_found = 0;

static long ethernet_probe(struct usb_device *dev, unsigned short ifnum);
static long ethernet_disconnect(struct usb_device *dev);
static long ethernet_ioctl(struct uddif *, short, long);

static char lname[] = "USB ethernet class driver";

static struct uddif eth_uif =
{
    0,                  /* *next */
    USB_API_VERSION,    /* API */
    USB_DEVICE,         /* class */
    lname,              /* lname */
    "eth",              /* name */
    0,                  /* unit */
    0,                  /* flags */
    ethernet_probe,     /* probe */
    ethernet_disconnect,/* disconnect */
    0,                  /* resrvd1 */
    ethernet_ioctl,     /* ioctl */
    0,                  /* resrvd2 */
};

static long ethernet_probe(struct usb_device *dev, unsigned short ifnum)
{
    long old_async;
    long rc = -1L;

    if (dev == NULL)
        return rc;

    old_async = usb_disable_asynch(1);  /* asynch transfer not allowed */

    asix_eth_before_probe(api);
    picowifi_eth_before_probe(api);
    if (asix_eth_probe(dev, ifnum, &ueth_dev)) {
        if (asix_eth_get_info(dev, &ueth_dev, mac)) {
            if (xbase != NULL) {
                memcpy(xbase->hwaddr,mac,ETH_ALEN);
                memcpy(xbase->macaddr,mac,ETH_ALEN);
            }
            asix_found = 1;
            rc = 0L;
        }
    } else if (picowifi_eth_probe(dev, ifnum, &ueth_dev)) {
        if (picowifi_eth_get_info(dev, &ueth_dev, mac)) {
            if (xbase != NULL) {
                memcpy(xbase->hwaddr,mac,ETH_ALEN);
                memcpy(xbase->macaddr,mac,ETH_ALEN);
            }
            picowifi_found = 1;
            rc = 0L;
        }
    } 

    usb_disable_asynch(old_async);      /* restore asynch value */

    return rc;
}

static long ethernet_disconnect(struct usb_device *dev)
{
    ueth_dev.pusb_dev = 0;
    return 0L;
}

static long ethernet_ioctl(struct uddif *u, short cmd, long arg)
{
    return 0L;
}


/************************************
*                                   *
*       INITIALISATION ROUTINES     *
*                                   *
************************************/

/*
 *  Note that this program does NOT use the standard startup code
 *  _init() is jumped to by init.s
 */
void _init(BASPAG *bp)
{
DRV_LIST *sting_drivers;
long PgmSize;

    /* calculate size of TPA */
    PgmSize = (long)bp->p_bbase + bp->p_blen - (long)bp;

    /* change CR in cmdline to '\0' */
    bp->p_cmdlin[1+bp->p_cmdlin[0]] = '\0';

    if (strcmp(bp->p_cmdlin+1,"STinG_Load") != 0)
        quit(BADSTART);

    sting_drivers = (DRV_LIST *)Supexec(get_sting_cookie);
    if (!sting_drivers)
        quit(NOSTINGCOOKIE);

    if (strcmp(sting_drivers->magic,MAGIC) != 0)
        quit(NOMAGIC);

    tpl = (TPL *) (*sting_drivers->get_dftab)(TRANSPORT_DRIVER);
    stx = (STX *) (*sting_drivers->get_dftab)(MODULE_DRIVER);

    if (!tpl || !stx)
        quit(NODRIVERS);

    api = (struct usb_module_api *)Supexec(get_usb_cookie);
    if (!api)
        quit(NOUSBCOOKIE);

    if (udd_register(&eth_uif))
        quit(NOREGISTER);

    install(bp);

    Ptermres(PgmSize,0);
}

static int32 get_cookie(int32 cookie)
{
int32 *p;

    for (p = *(int32 **)_p_cookie; *p; p += 2)
        if (*p == cookie)
            return *++p;

    return 0L;
}

static int32 get_frb_cookie(void)
{
    return get_cookie(FRB_COOKIE);
}

static int32 get_sting_cookie(void)
{
    return get_cookie(STING_COOKIE);
}

static int32 get_usb_cookie(void)
{
    return get_cookie(USB_COOKIE);
}

static void install(BASPAG *BasPag)
{
PORT *ports;
DRIVER *driver;

    query_chains((void **)&ports,(void **)&driver,NULL);

    while (ports->next)         /* find end of port chain */
        ports = ports->next;

    /*
     *  process device (we assume only one)
     */
    xbase = allocmem(sizeof(struct extended_port)); /* get memory for one device */
    init_ext_port(xbase);               /* initialise extended port structure */
    memcpy(xbase->hwaddr,mac,ETH_ALEN);
    memcpy(xbase->macaddr,mac,ETH_ALEN);
    ports->next = &xbase->port;         /* add port to end of chain */

    DEBUG(("xbase = %p, mac = %02x:%02x:%02x:%02x:%02x:%02x\n",
            xbase, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]));

    /*
     *  add driver to chain
     */
    while (driver->next)                /* find end of driver chain */
        driver = driver->next;
    usbnet_driver.basepage = BasPag;
    driver->next = &usbnet_driver;        /* add driver to end of chain */

    /*
     *  initialise arp stuff
     */
    memset(&arp_enet_pkt, 0, sizeof(ARP_PACKET));
    arp_enet_pkt.eh.type = ENET_TYPE_ARP;
    arp_enet_pkt.arp.hardware_space = ARP_HARD_ETHER;
    arp_enet_pkt.arp.protocol_space = ENET_TYPE_IP;
    arp_enet_pkt.arp.hardware_len = ETH_ALEN;
    arp_enet_pkt.arp.protocol_len = 4;

    arpcache_entries = arp_init();
}

static void *allocmem(long size)
{
static long frb = -1;

    if (frb < 0)
        frb = Supexec(get_frb_cookie);

    return frb?(void *)Mxalloc(size,3):(void *)Malloc(size);
}

static void display_message(char *s)
{
    while(*s)
    {
        if (*s == '\n')
            Bconout(2,'\r');
        Bconout(2,*s++);
    }
}

static void init_ext_port(struct extended_port *x)
{
    memset((char *)x,0,sizeof(struct extended_port));

    x->port.name = x->name;
    x->port.type = L_SER_BUS;
    x->port.active = FALSE;
    x->port.flags = 0L;
    x->port.ip_addr = 0xffffffffUL;
    x->port.sub_mask = 0xffffffffUL;
    x->port.mtu = 1500;
    x->port.max_mtu = 1500;
    x->port.stat_sd_data = 0L;
    x->port.send = NULL;
    x->port.stat_rcv_data = 0L;
    x->port.receive = NULL;
    x->port.stat_dropped = 0;
    x->port.driver = &usbnet_driver;
    x->port.next = NULL;
    x->magic = EXTPORT_MAGIC;
    x->unused = '\0';
    strcpy(x->name,BASE_PORTNAME);
#ifdef TRACE
    x->trace.next = x->trace.first = x->trace.entry;
    x->trace.last = x->trace.first + TRACE_ENTRIES;
    trace_init(x);
#endif
}

static void quit(char *s)
{
    display_message("\n" DRIVER_NAME);
    display_message(s);
    Pterm(-1);
}


/************************************
*                                   *
*       HIGH-LEVEL ROUTINES         *
*                                   *
************************************/

/*
 *  sends all pending dgrams
 */
static void send_dgrams(PORT *port)
{
struct extended_port *x = (struct extended_port *)port;
IP_DGRAM *dgram;
int16 length;

    /* do nothing if it is not for this port */
    if ((x->magic != EXTPORT_MAGIC) || !port->active)
        return;

    /* likewise if there is no send queue */
    if (!port->send)
        return;

    /*
     *  we need to send a datagram
     */
    while((dgram=dequeue_dgram(&port->send)))       /* process entire queue */
    {
        x->stats.send.dequeued++;
        switch(length=process_output(x,dgram)) {
        case 0:
            /*
             * we couldn't send the dgram, so we need to requeue it.  we queue
             * it to our own queue of dgrams waiting for address resolution.
             * this queue is processed in process_arp() whenever we get an
             * ARP response.
             */
            queue_dgram(&x->arpwait,dgram);
            x->stats.arp.wait_queued++;
            break;
        case -1:
            IP_discard(dgram,TRUE);
            port->stat_dropped++;
            break;
        default:
            IP_discard(dgram,TRUE);
            port->stat_sd_data += length;
            break;
        }
    }
}

/*
 *  receives all pending datagrams and queues them
 */
static void receive_dgrams(PORT *port)
{
struct extended_port *x = (struct extended_port *)port;
int16 length;
int rc = 0;

    /* do nothing if it is not for this port */
    if ((x->magic != EXTPORT_MAGIC) || !port->active)
        return;

    while((length=read_device(x,&ip)) > 0)
    {
        x->stats.receive.total_packets++;
        switch(ip.eh.type) {
        case ENET_TYPE_IP:
            x->stats.receive.good_packets++;
            if (memcmp(ip.eh.destination,BROADCAST_ADDR,ETH_ALEN) == 0)
            {
                x->stats.process.broadcast_ip_packets++;
                break;
            }
            x->stats.process.normal_ip_packets++;
            if ((rc=process_ip(x,(IP_HDR *)ip.ed,length)) != 0)
                x->stats.process.bad_ip_packets++;
            break;
        case ENET_TYPE_ARP:
            x->stats.receive.good_packets++;
            x->stats.process.arp_packets++;
            if ((rc=process_arp(x,(ARP *)ip.ed)) != 0)
                x->stats.process.bad_arp_packets++;
            break;
        default:
            x->stats.receive.bad_packets++;
            rc = -1;
            break;
        }

        if (rc == 0)
            port->stat_rcv_data += length;
        else port->stat_dropped++;
    }
}


static int16 set_device_state(PORT *port,int16 state)
{
struct extended_port *x = (struct extended_port *)port;

    /* do nothing if it is not for this port */
    if (x->magic != EXTPORT_MAGIC)
        return FALSE;

    if (state)
    {
        if (open_device(x) < 0)
            return FALSE;                       // is this OK?
    }
    else
    {
        if (close_device(x) < 0)
            return FALSE;                       // is this OK?
        empty_queue(&port->send);
        empty_queue(&port->receive);
    }

    return TRUE;
}


static int16 control_device(PORT *port,uint32 argument,int16 code)
{
struct extended_port *x = (struct extended_port *)port;
int16 result = E_NORMAL;
static int16 type = -1;

    /* do nothing if it is not for this port */
    if (x->magic != EXTPORT_MAGIC)
        return E_PARAMETER;

    switch(code) {
    /* CTL_ETHER_SET_MAC is not available */
    case CTL_ETHER_GET_MAC:
        result = get_mac_address(x,x->macaddr);         /* as a precaution, we ask the actual hardware first */
        memcpy((char *)argument,x->macaddr,ETH_ALEN);   /* (if it didn't work, we'll use what WE think it is) */
        break;
    case CTL_ETHER_INQ_SUPPTYPE:
        *((char ***) argument) = suppHardware;
        break;
    case CTL_ETHER_SET_TYPE:
        type = ((int16)argument) & 7;       /* the lowest 3 bits select from suppHardware */
        break;
    case CTL_ETHER_GET_TYPE:
        *((int16 *) argument) = type;
        break;
    case CTL_ETHER_GET_STAT:                /* returns a copy of USBNET_STATS */
        memcpy(x->stats.hwaddr,x->hwaddr,ETH_ALEN);
        memcpy(x->stats.macaddr,x->macaddr,ETH_ALEN);
        x->stats.arp_entries = arp_count(); /* get entry counts */
        x->stats.trace_entries = TRACE_ENTRIES;
        *((USBNET_STATS *)argument) = x->stats;
        break;
    case CTL_ETHER_CLR_STAT:                /* sets all entries in USBNET_STATS to 0 */
        memset((char *)&x->stats,0,sizeof(USBNET_STATS));
        break;
    case CTL_ETHER_GET_ARPTABLE:            /* returns ARP table */
        arp_table((ARP_INFO *)argument);
        break;
    case CTL_ETHER_CLR_ARPTABLE:            /* clears ARP table */
        arp_init();
        break;
#ifdef TRACE
    case CTL_ETHER_GET_TRACE:               /* returns trace table */
        memcpy((char *)argument,x->trace.first,TRACE_ENTRIES*sizeof(USBNET_TRACE));
        break;
    case CTL_ETHER_CLR_TRACE:               /* clears trace */
        trace_init(x);
        break;
#endif
    default:
        result = E_FNAVAIL;
    }

    return result;
}


/************************************
*                                   *
*       SECOND LEVEL ROUTINES       *
*                                   *
************************************/

/*
 *  dequeue & return first unexpired dgram from queue
 *  (leading expired dgrams are dropped)
 */
static IP_DGRAM *dequeue_dgram(IP_DGRAM **queue)
{
IP_DGRAM *dgram;

    do
    {
        if (!(dgram=*queue))                        /* anything in queue? */
            return NULL;                            /* no */
        *queue = dgram->next;                       /* else dequeue it */
    } while(check_dgram_ttl(dgram) != E_NORMAL);    /* if expired, discard & try again */

    return dgram;       /* return pointer to first unexpired dgram, now dequeued */
}

/*
 *  queue dgram to specified queue
 */
static void queue_dgram(IP_DGRAM **queue,IP_DGRAM *dgram)
{
IP_DGRAM *walk, **prevptr;

    for (walk = *(prevptr=queue); walk; walk = *(prevptr=&walk->next))
        ;
    *prevptr = dgram;
    dgram->next = NULL;
}

/*
 *  process one output IP packet
 *      returns >0 if packet ok, sent (value is length of packet)
 *              0 if packet ok, not sent
 *          or -1 if error
 */
static int16 process_output(struct extended_port *x,IP_DGRAM *dgram)
{
char *cachedEther;
int16 enet_length;
uint32 network, ip_address;

    /* first we validate size */
    enet_length = (uint16)(sizeof(ENET_HDR) + sizeof(IP_HDR) + dgram->opt_length + dgram->pkt_length);
    if (enet_length > ETH_MAX_LEN)
    {
        x->stats.send.bad_length++;
        return -1;
    }

    /* we check where it should go */
    network = x->port.ip_addr & x->port.sub_mask;

    /* no ip packets to "host 0 or ff" */
    if (((dgram->hdr.ip_dest & ~x->port.sub_mask) == 0L)
     || ((dgram->hdr.ip_dest & ~x->port.sub_mask) == 0xffL))
    {
        x->stats.send.bad_host++;               //FIXME: may not be an error ... must save dgram for checking
        return -1;
    }

    if ((dgram->hdr.ip_dest & x->port.sub_mask) == network)
    {
        ip_address = dgram->hdr.ip_dest;
    }
    else
    {
        if ((dgram->ip_gateway & x->port.sub_mask) == network)
        {
            ip_address = dgram->ip_gateway;
        }
        else
        {
            x->stats.send.bad_network++;
            return -1;
        }
    }

    if (!(cachedEther=arp_cache(ip_address)))
    {
        /*
         * the ethernet address is NOT in the cache: we must send an ARP query.
         */
        memset(arp_enet_pkt.eh.destination,0xff,ETH_ALEN);  /* broadcast */
        arp_enet_pkt.arp.op_code = ARP_OP_REQ;              /* we send a request */
        memset(arp_enet_pkt.arp.dest_ether,0xff,ETH_ALEN);  /* broadcast */
        arp_enet_pkt.arp.dest_ip = ip_address;
        send_arp(x);
        return 0;                   /* dgram ok, we just didn't send it */
    }

    /*
     *  we've found the ethernet address in the cache, so we try to send the dgram
     */
    memcpy(op.eh.destination,cachedEther,ETH_ALEN);
    memcpy(op.eh.source,x->macaddr,ETH_ALEN);
    op.eh.type = ENET_TYPE_IP;
    memcpy(op.ed,(char *)&dgram->hdr,sizeof(IP_HDR));
    memcpy(op.ed+sizeof(IP_HDR),dgram->options,dgram->opt_length);
    memcpy(op.ed+sizeof(IP_HDR)+dgram->opt_length,dgram->pkt_data,dgram->pkt_length);
    if (enet_length < ETH_MIN_LEN)
    {
        memset(op.ed+sizeof(IP_HDR)+dgram->opt_length+dgram->pkt_length,0,ETH_MIN_LEN-enet_length);
                                                            /* pad with zeros (for neatness) */
        enet_length = ETH_MIN_LEN;
    }
    if (write_device(x,(char *)&op,enet_length) != 0)
        return -1;
    x->stats.send.ip_packets++;

    return (int16)(sizeof(IP_HDR)+dgram->opt_length+dgram->pkt_length);
}

/*
 *  process one input IP packet
 *      returns 0 if packet accepted
 *          or -1 if error
 */
static int16 process_ip(struct extended_port *x,IP_HDR *ip_hdr,int length)
{
IP_DGRAM *dgram, *walk, **prevptr;
char *p;

    if ((length < ETH_MIN_LEN) || (length > ETH_MAX_LEN))   /* validate total packet length */
        return -1;
    if (ip_hdr->length > length)                            /* validate ip length */
        return -1;
    if ((ip_hdr->hd_len*4 < sizeof(IP_HDR))                 /* validate ip header length */
     || (ip_hdr->hd_len*4 > ip_hdr->length))
        return -1;

    if ((dgram=KRmalloc(sizeof(IP_DGRAM))) == NULL)
        return -1;

    memcpy((char *)&dgram->hdr,ip_hdr,sizeof(IP_HDR));

    dgram->opt_length = (int16)(ip_hdr->hd_len*4 - sizeof(IP_HDR));
    dgram->options = KRmalloc(dgram->opt_length);
    dgram->pkt_length = ip_hdr->length - ip_hdr->hd_len*4;
    dgram->pkt_data = KRmalloc(dgram->pkt_length);
    if (!dgram->options || !dgram->pkt_data)
    {
        IP_discard(dgram,TRUE);
        return -1;
    }
    p = ((char *)ip_hdr) + sizeof(IP_HDR);
    memcpy(dgram->options,p,dgram->opt_length);
    memcpy(dgram->pkt_data,p+dgram->opt_length,dgram->pkt_length);

//  dgram->ip_gateway = ???;
    dgram->recvd = &x->port;
    dgram->next = NULL;
    set_dgram_ttl(dgram);
    for (walk = *(prevptr = &x->port.receive); walk; walk = *(prevptr = &walk->next))
        ;
    *prevptr = dgram;

    return 0;
}

/*
 *  process one input ARP packet
 *      returns 0 if packet accepted
 *          or -1 if error
 */
static int16 process_arp(struct extended_port *x,ARP *arp)
{
IP_DGRAM *dgram;
IP_DGRAM *arptemp = NULL;
char *cachedEther;
int16 length;

    /* ignore funny ARP packets */
    if ((arp->hardware_space != ARP_HARD_ETHER)
     || (arp->hardware_len != ETH_ALEN)
     || (arp->protocol_space != ENET_TYPE_IP)
     || (arp->protocol_len != 4))
    {
        x->stats.arp.input_errors++;
        return -1;
    }

    /*
     * ignore unsupported op_codes (e.g. RARP)
     */
    if ((arp->op_code != ARP_OP_REQ)
     && (arp->op_code != ARP_OP_ANS))
    {
        x->stats.arp.opcode_errors++;
        return -1;
    }

    /*
     * check if this ether source is in the cache
     *
     * note that we update the cache when we see _any_ ARP info: this
     * should reduce the number of ARP requests we have to make
     */
    if ((cachedEther=arp_cache(arp->src_ip)) == NULL)
        arp_enter(arp->src_ip,arp->src_ether);
    else memcpy(cachedEther,arp->src_ether,ETH_ALEN);

    /*
     * if this was a request to us, we'd better answer
     */
    if (arp->dest_ip == x->port.ip_addr)
    {
        if (arp->op_code == ARP_OP_REQ)
        {
            x->stats.arp.requests_received++;
            memcpy(arp_enet_pkt.eh.destination,arp->src_ether,ETH_ALEN);
            arp_enet_pkt.arp.op_code = ARP_OP_ANS;
            memcpy(arp_enet_pkt.arp.dest_ether,arp->src_ether,ETH_ALEN);
            arp_enet_pkt.arp.dest_ip = arp->src_ip;
            send_arp(x);
        }
        else
            x->stats.arp.answers_received++;
    }

    /*
     * we have some (potentially) new ARP information, so we process
     * the dgrams that are queued waiting for address resolution.
     */
    while((dgram=dequeue_dgram(&x->arpwait)))       /* process entire queue */
    {
        x->stats.arp.wait_dequeued++;
        switch(length=process_output(x,dgram)) {
        case 0:
            /*
             * we couldn't send the dgram (presumably the ARP info was for
             * a different address), so we need to requeue it (again).
             * we queue it off a temporary queue header so that we don't
             * loop for ever here.
             */
            queue_dgram(&arptemp,dgram);
            x->stats.arp.wait_requeued++;
            break;
        case -1:
            IP_discard(dgram,TRUE);
            x->port.stat_dropped++;
            break;
        default:
            IP_discard(dgram,TRUE);
            x->port.stat_sd_data += length;
            break;
        }
    }
    /*
     * the arpwait queue is empty, but there may be entries in the
     * arptemp queue.  fix this up!
     */
    x->arpwait = arptemp;

    return 0;
}

static int16 send_arp(struct extended_port *x)
{
int16 rc;

    memcpy(arp_enet_pkt.eh.source,x->macaddr,ETH_ALEN);     /* set source address info into ARP outgoing packets */
    memcpy(arp_enet_pkt.arp.src_ether,x->macaddr,ETH_ALEN);
    arp_enet_pkt.arp.src_ip = x->port.ip_addr;

    rc = write_device(x,(char *)&arp_enet_pkt,sizeof(arp_enet_pkt));
    x->stats.send.arp_packets++;

    if (rc == 0)
        x->port.stat_sd_data += sizeof(arp_enet_pkt);
    else x->stats.send.arp_packets_err++;

    return rc;
}

static void empty_queue(IP_DGRAM **queue)
{
IP_DGRAM *walk, *next;

    for (walk = *queue; walk; walk = next)
    {
        next = walk->next;
        IP_discard(walk,TRUE);
    }

    *queue = NULL;
}


/************************************************
*                                               *
*       A S I X / U S B   I N T E R F A C E     *
*                                               *
************************************************/

static int16 open_device(struct extended_port *x)
{
    x->interface_up = TRUE;

    return 0;
}

static int16 close_device(struct extended_port *x)
{
    x->interface_up = FALSE;

    return 0;
}

/*
 *  write a packet
 *      returns 0: ok
 *              -1: error
 */
static int16 write_device(struct extended_port *x, char *buffer, int length)
{
long rc = -1;

    x->stats.write.total_packets++;

    if (asix_found)
        rc = asix_send(&ueth_dev, buffer, length);
    else if (picowifi_found)
        rc = picowifi_send(&ueth_dev, buffer, length);

    trace(x, TRACE_WRITE, rc, length, buffer);

    if (rc < 0L)
    {
        x->stats.write.failed++;
        return -1;
    }

    return 0;
}

/*
 *  read a packet
 *      returns >0: length, more to do
 *              0: no more
 *              -1: error
 */
static int16 read_device(struct extended_port *x,ENET_PACKET *ip)
{
long rc = -1;

    x->stats.read.total_packets++;

    if (asix_found)
        rc = asix_recv(&ueth_dev,(unsigned char *)ip,ETH_MAX_LEN);
    else if (picowifi_found)
        rc = picowifi_recv(&ueth_dev,(unsigned char *)ip,ETH_MAX_LEN);

    if (rc)
        trace(x,TRACE_READ,rc,rc,(char *)ip);

    if (rc < 0L) {
        x->stats.read.failed++;
        return -1;
    }

    return rc;
}

/*
 *  get MAC address: callable from user & supervisor mode
 */
static int16 get_mac_address(struct extended_port *x,char *macaddr)
{
int16 super, rc=-1;
char *oldstack;

    /*
     *  error if it is not for this port
     */
    if (x->magic != EXTPORT_MAGIC)
        return -1;

    super = (int16)Super((void *)1L);
    if (!super)                         /* not supervisor: switch */
        oldstack = (char *)Super((void *)0L);
    
    if (asix_found)
        rc = (int16)asix_read_mac(&ueth_dev,(unsigned char *)macaddr);
    else if (picowifi_found)
        rc = (int16)picowifi_read_mac(&ueth_dev,(unsigned char *)macaddr);

    trace(x,TRACE_MAC_GET,rc,ETH_ALEN,macaddr);

    if (!super)                         /* switch back */
        SuperToUser(oldstack);

    return rc;
}

#ifdef TRACE
/*
 *  initialise trace table
 */
static void trace_init(struct extended_port *x)
{
USBNET_TRACE *t;
int32 i;

    for (i = 0, t = x->trace.entry; i < TRACE_ENTRIES; i++, t++)
        t->time = 0L;
}

/*
 *  make a trace entry (assumed to be called from supervisor mode)
 */
static void trace(struct extended_port *x,char type,long rc,int length,char *data)
{
USBNET_TRACE *t;

    t = x->trace.next++;
    t->time = hz_200;
    t->rc = rc;
    t->type = type;
    t->length = length;
    if (length > 0)
        memcpy(t->data,data,min(USBNET_TRACE_LEN,length));
    if (x->trace.next >= x->trace.last)
        x->trace.next = x->trace.first;
}
#endif
