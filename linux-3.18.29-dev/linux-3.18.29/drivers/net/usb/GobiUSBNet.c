/*===========================================================================
FILE:
   GobiUSBNet.c

DESCRIPTION:
   Qualcomm USB Network device for Gobi 3000
   
FUNCTIONS:
   GobiNetSuspend
   GobiNetResume
   GobiNetDriverBind
   GobiNetDriverUnbind
   GobiUSBNetURBCallback
   GobiUSBNetTXTimeout
   GobiUSBNetAutoPMThread
   GobiUSBNetStartXmit
   GobiUSBNetOpen
   GobiUSBNetStop
   GobiUSBNetProbe
   GobiUSBNetModInit
   GobiUSBNetModExit

Copyright (c) 2011, Code Aurora Forum. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of Code Aurora Forum nor
      the names of its contributors may be used to endorse or promote
      products derived from this software without specific prior written
      permission.


THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
===========================================================================*/

//---------------------------------------------------------------------------
// Include Files
//---------------------------------------------------------------------------

#include "Structs.h"
#include "QMIDevice.h"
#include "QMI.h"
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/module.h>
#include <net/arp.h>
#include <linux/ip.h>
//#include <linux/udp.h>
#ifdef CONFIG_DT_SF16A18_FULLMASK_CPE
#include <linux/of_gpio.h>
#endif

//-----------------------------------------------------------------------------
// Definitions
//-----------------------------------------------------------------------------

// Version Information
#define DRIVER_VERSION "Quectel_WCDMA&LTE_Linux&Android_GobiNet_Driver_V1.3.0"
#define DRIVER_AUTHOR "Qualcomm Innovation Center"
#define DRIVER_DESC "GobiNet"

// Debug flag
int debug = 0;

// Allow user interrupts
int interruptible = 1;

// Number of IP packets which may be queued up for transmit
int txQueueLength = 100;

// Class should be created during module init, so needs to be global
static struct class * gpClass;

static const unsigned char ec20_mac[ETH_ALEN] = {0x02, 0x50, 0xf3, 0x00, 0x00, 0x00};
//static const u8 broadcast_addr[ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

//#define QUECTEL_WWAN_MULTI_PACKAGES

#ifdef QUECTEL_WWAN_MULTI_PACKAGES
static uint __read_mostly rx_packets = 10;
module_param( rx_packets, uint, S_IRUGO | S_IWUSR );

#define USB_CDC_SET_MULTI_PACKAGE_COMMAND (0x5C)
#define QUEC_NET_MSG_SPEC		(0x80)
#define QUEC_NET_MSG_ID_IP_DATA		(0x00)

struct multi_package_config {
	__le32 enable;
	__le32 package_max_len;
	__le32 package_max_count_in_queue;
	__le32 timeout;
} __packed;

struct quec_net_package_header {
	unsigned char msg_spec;
	unsigned char msg_id;
	unsigned short payload_len;
	unsigned char reserve[16];
} __packed;
#endif

#ifdef CONFIG_BRIDGE
static int __read_mostly bridge_mode = 0;
static uint __read_mostly bridge_ipv4 = 0;
module_param( bridge_mode, int, S_IRUGO | S_IWUSR );
module_param( bridge_ipv4, uint, S_IRUGO | S_IWUSR );

static int bridge_arp_reply(struct net_device *dev, struct sk_buff *skb) {
    struct arphdr *parp;
    u8 *arpptr, *sha;
    __be32 sip, tip, ipv4;
    struct sk_buff *reply = NULL;
        
    parp = arp_hdr(skb);

    if (parp->ar_hrd == htons(ARPHRD_ETHER)  && parp->ar_pro == htons(ETH_P_IP)
        && parp->ar_op == htons(ARPOP_REQUEST) && parp->ar_hln == 6 && parp->ar_pln == 4) {
        arpptr = (u8 *)parp + sizeof(struct arphdr);
        sha = arpptr;
        arpptr += dev->addr_len;	/* sha */
        memcpy(&sip, arpptr, sizeof(sip));
        arpptr += sizeof(sip);
        arpptr += dev->addr_len;	/* tha */
        memcpy(&tip, arpptr, sizeof(tip));

        ipv4 = ((bridge_ipv4 & 0xff)<<24) | ((bridge_ipv4 & 0xff00)<<8)
            | ((bridge_ipv4 & 0xff0000) >> 8) |((bridge_ipv4 & 0xff000000) >> 24);
        DBG("sip = 0x%08x, tip=%08x, gw=%08x\n", sip, tip, ipv4);
        if (((u8 *)&tip)[0] == ((u8 *)&ipv4)[0] && ((u8 *)&tip)[1] == ((u8 *)&ipv4)[1] && ((u8 *)&tip)[2] == ((u8 *)&ipv4)[2] && ((u8 *)&tip)[3] != ((u8 *)&ipv4)[3])
            reply = arp_create(ARPOP_REPLY, ETH_P_ARP, sip, dev, tip, sha, ec20_mac, sha);

        if (reply) {
            skb_reset_mac_header(reply);
            __skb_pull(reply, skb_network_offset(reply));
            reply->ip_summed = CHECKSUM_UNNECESSARY;
            reply->pkt_type = PACKET_HOST;

            netif_rx_ni(reply);
        }
        return 1;
    }

    return 0;
}
#endif

#ifdef CONFIG_PM
/*===========================================================================
METHOD:
   GobiNetSuspend (Public Method)

DESCRIPTION:
   Stops QMI traffic while device is suspended

PARAMETERS
   pIntf          [ I ] - Pointer to interface
   powerEvent     [ I ] - Power management event

RETURN VALUE:
   int - 0 for success
         negative errno for failure
===========================================================================*/
int GobiNetSuspend(
   struct usb_interface *     pIntf,
   pm_message_t               powerEvent )
{
   struct usbnet * pDev;
   sGobiUSBNet * pGobiDev;
   
   if (pIntf == 0)
   {
      return -ENOMEM;
   }
   
#if (LINUX_VERSION_CODE > KERNEL_VERSION( 2,6,23 ))
   pDev = usb_get_intfdata( pIntf );
#else
   pDev = (struct usbnet *)pIntf->dev.platform_data;
#endif

   if (pDev == NULL || pDev->net == NULL)
   {
      DBG( "failed to get netdevice\n" );
      return -ENXIO;
   }
   
   pGobiDev = (sGobiUSBNet *)pDev->data[0];
   if (pGobiDev == NULL)
   {
      DBG( "failed to get QMIDevice\n" );
      return -ENXIO;
   }

   // Is this autosuspend or system suspend?
   //    do we allow remote wakeup?
#if (LINUX_VERSION_CODE < KERNEL_VERSION( 2,6,33 ))
#if (LINUX_VERSION_CODE > KERNEL_VERSION( 2,6,18 ))
   if (pDev->udev->auto_pm == 0)
#else
   if (1)
#endif
#else
   if ((powerEvent.event & PM_EVENT_AUTO) == 0)
#endif
   {
      DBG( "device suspended to power level %d\n", 
           powerEvent.event );
      GobiSetDownReason( pGobiDev, DRIVER_SUSPENDED );
   }
   else
   {
      DBG( "device autosuspend\n" );
   }
     
   if (powerEvent.event & PM_EVENT_SUSPEND)
   {
      // Stop QMI read callbacks
      KillRead( pGobiDev );
#if (LINUX_VERSION_CODE > KERNEL_VERSION( 2,6,22 ))
      pDev->udev->reset_resume = 0;
#endif
      
      // Store power state to avoid duplicate resumes
      pIntf->dev.power.power_state.event = powerEvent.event;
   }
   else
   {
      // Other power modes cause QMI connection to be lost
#if (LINUX_VERSION_CODE > KERNEL_VERSION( 2,6,22 ))
      pDev->udev->reset_resume = 1;
#endif
   }
   
   // Run usbnet's suspend function
   return usbnet_suspend( pIntf, powerEvent );
}
   
/*===========================================================================
METHOD:
   GobiNetResume (Public Method)

DESCRIPTION:
   Resume QMI traffic or recreate QMI device

PARAMETERS
   pIntf          [ I ] - Pointer to interface

RETURN VALUE:
   int - 0 for success
         negative errno for failure
===========================================================================*/
int GobiNetResume( struct usb_interface * pIntf )
{
   struct usbnet * pDev;
   sGobiUSBNet * pGobiDev;
   int nRet;
   int oldPowerState;
   
   if (pIntf == 0)
   {
      return -ENOMEM;
   }
   
#if (LINUX_VERSION_CODE > KERNEL_VERSION( 2,6,23 ))
   pDev = usb_get_intfdata( pIntf );
#else
   pDev = (struct usbnet *)pIntf->dev.platform_data;
#endif

   if (pDev == NULL || pDev->net == NULL)
   {
      DBG( "failed to get netdevice\n" );
      return -ENXIO;
   }
   
   pGobiDev = (sGobiUSBNet *)pDev->data[0];
   if (pGobiDev == NULL)
   {
      DBG( "failed to get QMIDevice\n" );
      return -ENXIO;
   }

   oldPowerState = pIntf->dev.power.power_state.event;
   pIntf->dev.power.power_state.event = PM_EVENT_ON;
   DBG( "resuming from power mode %d\n", oldPowerState );

   if (oldPowerState & PM_EVENT_SUSPEND)
   {
      // It doesn't matter if this is autoresume or system resume
      GobiClearDownReason( pGobiDev, DRIVER_SUSPENDED );
   
      nRet = usbnet_resume( pIntf );
      if (nRet != 0)
      {
         DBG( "usbnet_resume error %d\n", nRet );
         return nRet;
      }

      // Restart QMI read callbacks
      nRet = StartRead( pGobiDev );
      if (nRet != 0)
      {
         DBG( "StartRead error %d\n", nRet );
         return nRet;
      }

#ifdef CONFIG_PM
   #if (LINUX_VERSION_CODE < KERNEL_VERSION( 2,6,29 ))   
      // Kick Auto PM thread to process any queued URBs
      complete( &pGobiDev->mAutoPM.mThreadDoWork );
    #endif
#endif /* CONFIG_PM */
   }
   else
   {
      DBG( "nothing to resume\n" );
      return 0;
   }
   
   return nRet;
}
#endif /* CONFIG_PM */

/*===========================================================================
METHOD:
   GobiNetDriverBind (Public Method)

DESCRIPTION:
   Setup in and out pipes

PARAMETERS
   pDev           [ I ] - Pointer to usbnet device
   pIntf          [ I ] - Pointer to interface

RETURN VALUE:
   int - 0 for success
         Negative errno for error
===========================================================================*/
static int GobiNetDriverBind( 
   struct usbnet *         pDev, 
   struct usb_interface *  pIntf )
{
   int numEndpoints;
   int endpointIndex;
   struct usb_host_endpoint * pEndpoint = NULL;
   struct usb_host_endpoint * pIn = NULL;
   struct usb_host_endpoint * pOut = NULL;
  
   // Verify one altsetting
   if (pIntf->num_altsetting != 1)
   {
      DBG( "invalid num_altsetting %u\n", pIntf->num_altsetting );
      return -ENODEV;
   }

   // Verify correct interface (4 for UC20)
   if ( !test_bit(pIntf->cur_altsetting->desc.bInterfaceNumber, &pDev->driver_info->data))
   {
      DBG( "invalid interface %d\n", 
           pIntf->cur_altsetting->desc.bInterfaceNumber );
      return -ENODEV;
   }
   
   if ( pIntf->cur_altsetting->desc.bInterfaceClass != 0xff)
   {
      struct usb_interface_descriptor *desc = &pIntf->cur_altsetting->desc;
      const char *qcfg_usbnet = "UNKNOW";
      
      if (desc->bInterfaceClass == 2 && desc->bInterfaceSubClass == 0x0e) {
         qcfg_usbnet = "MBIM";
      } else if (desc->bInterfaceClass == 2 && desc->bInterfaceSubClass == 0x06) {
         qcfg_usbnet = "ECM";
      } else if (desc->bInterfaceClass == 0xe0 && desc->bInterfaceSubClass == 1 && desc->bInterfaceProtocol == 3) {
         qcfg_usbnet = "RNDIS";
      }

      INFO( "usbnet is %s not NDIS/RMNET!\n", qcfg_usbnet);

      return -ENODEV;
   }
   
   // Collect In and Out endpoints
   numEndpoints = pIntf->cur_altsetting->desc.bNumEndpoints;
   for (endpointIndex = 0; endpointIndex < numEndpoints; endpointIndex++)
   {
      pEndpoint = pIntf->cur_altsetting->endpoint + endpointIndex;
      if (pEndpoint == NULL)
      {
         DBG( "invalid endpoint %u\n", endpointIndex );
         return -ENODEV;
      }

      if (usb_endpoint_dir_in( &pEndpoint->desc ) == true
      &&  usb_endpoint_xfer_int( &pEndpoint->desc ) == false)
      {
         pIn = pEndpoint;
      }
      else if (usb_endpoint_dir_out( &pEndpoint->desc ) == true)
      {
         pOut = pEndpoint;
      }
   }
   
   if (pIn == NULL || pOut == NULL)
   {
      DBG( "invalid endpoints\n" );
      return -ENODEV;
   }

   if (usb_set_interface( pDev->udev, 
                          pIntf->cur_altsetting->desc.bInterfaceNumber,
                          0 ) != 0)
   {
      DBG( "unable to set interface\n" );
      return -ENODEV;
   }

   pDev->in = usb_rcvbulkpipe( pDev->udev,
                   pIn->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK );
   pDev->out = usb_sndbulkpipe( pDev->udev,
                   pOut->desc.bEndpointAddress & USB_ENDPOINT_NUMBER_MASK );

#ifdef QUECTEL_WWAN_MULTI_PACKAGES
    if (rx_packets && pDev->udev->descriptor.idVendor == cpu_to_le16(0x2C7C)) {
        struct multi_package_config rx_config = {
            .enable = cpu_to_le32(1),
            .package_max_len = cpu_to_le32((1500 + sizeof(struct quec_net_package_header)) * rx_packets),
            .package_max_count_in_queue = cpu_to_le32(rx_packets), 
            .timeout = cpu_to_le32(10*1000), //10ms
        };
        int ret = 0;
        
    	ret = usb_control_msg(
    		interface_to_usbdev(pIntf),
    		usb_sndctrlpipe(interface_to_usbdev(pIntf), 0),
    		USB_CDC_SET_MULTI_PACKAGE_COMMAND,
    		0x21, //USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE
    		1,
    		pIntf->cur_altsetting->desc.bInterfaceNumber,
    		&rx_config, sizeof(rx_config), 100);

        DBG( "Quectel EC21&EC25 rx_packets=%d, ret=%d\n", rx_packets, ret);        
        if (ret == sizeof(rx_config)) {
           pDev->rx_urb_size = le32_to_cpu(rx_config.package_max_len);
        } else {
            rx_packets = 0;
        }
    }
#endif

#if 1 //def DATA_MODE_RP
    /* make MAC addr easily distinguishable from an IP header */
    if ((pDev->net->dev_addr[0] & 0xd0) == 0x40) {
        /*clear this bit wil make usbnet apdater named as usbX(instead if ethX)*/
        pDev->net->dev_addr[0] |= 0x02;	/* set local assignment bit */
        pDev->net->dev_addr[0] &= 0xbf;	/* clear "IP" bit */
    }
#endif
                   
   DBG( "in %x, out %x\n", 
        pIn->desc.bEndpointAddress, 
        pOut->desc.bEndpointAddress );

   // In later versions of the kernel, usbnet helps with this
#if (LINUX_VERSION_CODE <= KERNEL_VERSION( 2,6,23 ))
   pIntf->dev.platform_data = (void *)pDev;
#endif

   return 0;
}

/*===========================================================================
METHOD:
   GobiNetDriverUnbind (Public Method)

DESCRIPTION:
   Deregisters QMI device (Registration happened in the probe function)

PARAMETERS
   pDev           [ I ] - Pointer to usbnet device
   pIntfUnused    [ I ] - Pointer to interface

RETURN VALUE:
   None
===========================================================================*/
static void GobiNetDriverUnbind( 
   struct usbnet *         pDev, 
   struct usb_interface *  pIntf)
{
   sGobiUSBNet * pGobiDev = (sGobiUSBNet *)pDev->data[0];

   // Should already be down, but just in case...
   netif_carrier_off( pDev->net );

   DeregisterQMIDevice( pGobiDev );
   
#if (LINUX_VERSION_CODE >= KERNEL_VERSION( 2,6,29 ))
   kfree( pDev->net->netdev_ops );
   pDev->net->netdev_ops = NULL;
#endif

#if (LINUX_VERSION_CODE <= KERNEL_VERSION( 2,6,23 ))
   pIntf->dev.platform_data = NULL;
#endif

#if (LINUX_VERSION_CODE >= KERNEL_VERSION( 2,6,19 ))
   pIntf->needs_remote_wakeup = 0;
#endif

   if (atomic_dec_and_test(&pGobiDev->refcount))
      kfree( pGobiDev );
   else
      DBG("memory leak!\n");
}

#if 1 //def DATA_MODE_RP
/*===========================================================================
METHOD:
   GobiNetDriverTxFixup (Public Method)

DESCRIPTION:
   Handling data format mode on transmit path

PARAMETERS
   pDev           [ I ] - Pointer to usbnet device
   pSKB           [ I ] - Pointer to transmit packet buffer
   flags          [ I ] - os flags

RETURN VALUE:
   None
===========================================================================*/
struct sk_buff *GobiNetDriverTxFixup(struct usbnet *dev, struct sk_buff *skb, gfp_t flags)
{
    sGobiUSBNet * pGobiDev = (sGobiUSBNet *)dev->data[0];

    if (!pGobiDev) {
        DBG( "failed to get QMIDevice\n" );
        dev_kfree_skb_any(skb);
        return NULL;       
    }

    if (!pGobiDev->mbRawIPMode)
        return skb;

#ifdef CONFIG_BRIDGE
    if (bridge_mode) {
        struct ethhdr *ehdr = eth_hdr(skb);
        const struct iphdr *iph;
        
//debug = 1;
//        DBG("ethhdr: ");
//        PrintHex(ehdr, sizeof(struct ethhdr));
        
        if (ehdr->h_proto == htons(ETH_P_ARP)) {
            bridge_arp_reply(dev->net, skb);
            dev_kfree_skb_any(skb);
            return NULL;
        }

        iph = ip_hdr(skb);
        //DBG("iphdr: ");
        //PrintHex((void *)iph, sizeof(struct iphdr));

// 1	0.000000000	0.0.0.0	255.255.255.255	DHCP	362	DHCP Request  - Transaction ID 0xe7643ad7        
        if (ehdr->h_proto == htons(ETH_P_IP) && iph->protocol == IPPROTO_UDP && iph->saddr == 0x00000000 && iph->daddr == 0xFFFFFFFF) {
            //DBG("udphdr: ");
            //PrintHex(udp_hdr(skb), sizeof(struct udphdr));
            
            //if (udp_hdr(skb)->dest == htons(67)) //DHCP Request
            {
                memcpy(pGobiDev->mHostMAC, ehdr->h_source, ETH_ALEN);
                DBG("PC Mac Address: ");
                PrintHex(pGobiDev->mHostMAC, ETH_ALEN);   
            }
        }

#if 0
//Ethernet II, Src: DellInc_de:14:09 (f8:b1:56:de:14:09), Dst: IPv4mcast_7f:ff:fa (01:00:5e:7f:ff:fa)
//126	85.213727000	10.184.164.175	239.255.255.250	SSDP	175	M-SEARCH * HTTP/1.1 
//Ethernet II, Src: DellInc_de:14:09 (f8:b1:56:de:14:09), Dst: IPv6mcast_16 (33:33:00:00:00:16)
//160	110.305488000	fe80::6819:38ad:fcdc:2444	ff02::16	ICMPv6	90	Multicast Listener Report Message v2        
        if (memcmp(ehdr->h_dest, ec20_mac, ETH_ALEN) && memcmp(ehdr->h_dest, broadcast_addr, ETH_ALEN)) {
            DBG("Drop h_dest: ");
            PrintHex(ehdr, sizeof(struct ethhdr));
            dev_kfree_skb_any(skb);
            return NULL;
        }
#endif
        
        if (memcmp(ehdr->h_source, pGobiDev->mHostMAC, ETH_ALEN)) {
            DBG("Drop h_source: ");
            PrintHex(ehdr, sizeof(struct ethhdr));
            dev_kfree_skb_any(skb);
            return NULL;
        }
        
//debug = 0;
    }  
#endif
        
    // Skip Ethernet header from message
    if (skb_pull(skb, ETH_HLEN)) {
        return skb;
    } else {
#if (LINUX_VERSION_CODE > KERNEL_VERSION( 2,6,22 ))
        dev_err(&dev->intf->dev,  "Packet Dropped ");
#elif (LINUX_VERSION_CODE > KERNEL_VERSION( 2,6,18 ))
        dev_err(dev->net->dev.parent,  "Packet Dropped ");
#else
        INFO("Packet Dropped ");
#endif
    }
    
   // Filter the packet out, release it
   dev_kfree_skb_any(skb);
   return NULL;
}

/*===========================================================================
METHOD:
   GobiNetDriverRxFixup (Public Method)

DESCRIPTION:
   Handling data format mode on receive path

PARAMETERS
   pDev           [ I ] - Pointer to usbnet device
   pSKB           [ I ] - Pointer to received packet buffer

RETURN VALUE:
   None
===========================================================================*/
static int GobiNetDriverRxFixup(struct usbnet *dev, struct sk_buff *skb)
{
    __be16 proto;
    sGobiUSBNet * pGobiDev = (sGobiUSBNet *)dev->data[0];

    if (!pGobiDev->mbRawIPMode)
        return 1;

    /* This check is no longer done by usbnet */
    if (skb->len < dev->net->hard_header_len)
        return 0;

    switch (skb->data[0] & 0xf0) {
    case 0x40:
    	proto = htons(ETH_P_IP);
    	break;
    case 0x60:
    	proto = htons(ETH_P_IPV6);
    	break;
    case 0x00:
    	if (is_multicast_ether_addr(skb->data))
    		return 1;
    	/* possibly bogus destination - rewrite just in case */
    	skb_reset_mac_header(skb);
    	goto fix_dest;
    default:
    	/* pass along other packets without modifications */
    	return 1;
    }
    if (skb_headroom(skb) < ETH_HLEN && pskb_expand_head(skb, ETH_HLEN, 0, GFP_ATOMIC)) {
        DBG("%s: couldn't pskb_expand_head\n", __func__);
        return 0;
    }
    skb_push(skb, ETH_HLEN);
    skb_reset_mac_header(skb);
    eth_hdr(skb)->h_proto = proto;
    memcpy(eth_hdr(skb)->h_source, ec20_mac, ETH_ALEN);
fix_dest:
#ifdef CONFIG_BRIDGE
   if (bridge_mode) {
      memcpy(eth_hdr(skb)->h_dest, pGobiDev->mHostMAC, ETH_ALEN);
      //memcpy(eth_hdr(skb)->h_dest, broadcast_addr, ETH_ALEN);
    } else
#endif
    memcpy(eth_hdr(skb)->h_dest, dev->net->dev_addr, ETH_ALEN);

#ifdef CONFIG_BRIDGE
#if 0
    if (bridge_mode) {
        struct ethhdr *ehdr = eth_hdr(skb);
debug = 1;
        DBG(": ");
        PrintHex(ehdr, sizeof(struct ethhdr));
debug = 0;
    }
#endif
#endif
       
    return 1;
}

#ifdef QUECTEL_WWAN_MULTI_PACKAGES
static int GobiNetDriverRxPktsFixup(struct usbnet *dev, struct sk_buff *skb)
{
    sGobiUSBNet * pGobiDev = (sGobiUSBNet *)dev->data[0];

    if (!pGobiDev->mbRawIPMode)
        return 1;

    /* This check is no longer done by usbnet */
    if (skb->len < dev->net->hard_header_len)
        return 0;

    if (!rx_packets) {
        return GobiNetDriverRxFixup(dev, skb);
    }

    while (likely(skb->len)) {
        struct sk_buff* new_skb;
        struct quec_net_package_header package_header;

        if (skb->len < sizeof(package_header))
            return 0;

        memcpy(&package_header, skb->data, sizeof(package_header));
        package_header.payload_len = be16_to_cpu(package_header.payload_len);

        if (package_header.msg_spec != QUEC_NET_MSG_SPEC || package_header.msg_id != QUEC_NET_MSG_ID_IP_DATA)
            return 0;

        if (skb->len < (package_header.payload_len + sizeof(package_header)))
            return 0;

        skb_pull(skb, sizeof(package_header));

        if (skb->len == package_header.payload_len)
            return GobiNetDriverRxFixup(dev, skb);

        new_skb = skb_clone(skb, GFP_ATOMIC);
        if (new_skb) {
            skb_trim(new_skb, package_header.payload_len);
            if (GobiNetDriverRxFixup(dev, new_skb))
                usbnet_skb_return(dev, new_skb);
            else
                return 0;
        }

        skb_pull(skb, package_header.payload_len);
    }

    return 0;
}
#endif
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION( 2,6,29 ))
#ifdef CONFIG_PM
/*===========================================================================
METHOD:
   GobiUSBNetURBCallback (Public Method)

DESCRIPTION:
   Write is complete, cleanup and signal that we're ready for next packet

PARAMETERS
   pURB     [ I ] - Pointer to sAutoPM struct

RETURN VALUE:
   None
===========================================================================*/
#if (LINUX_VERSION_CODE > KERNEL_VERSION( 2,6,18 ))
void GobiUSBNetURBCallback( struct urb * pURB )
#else
void GobiUSBNetURBCallback(struct urb *pURB, struct pt_regs *regs)
#endif
{
   unsigned long activeURBflags;
   sAutoPM * pAutoPM = (sAutoPM *)pURB->context;
   if (pAutoPM == NULL)
   {
      // Should never happen
      DBG( "bad context\n" );
      return;
   }

   if (pURB->status != 0)
   {
      // Note that in case of an error, the behaviour is no different
      DBG( "urb finished with error %d\n", pURB->status );
   }

   // Remove activeURB (memory to be freed later)
   spin_lock_irqsave( &pAutoPM->mActiveURBLock, activeURBflags );

   // EAGAIN used to signify callback is done
   pAutoPM->mpActiveURB = ERR_PTR( -EAGAIN );

   spin_unlock_irqrestore( &pAutoPM->mActiveURBLock, activeURBflags );

   complete( &pAutoPM->mThreadDoWork );
   
#ifdef URB_FREE_BUFFER_BY_SELF
    if (pURB->transfer_flags & URB_FREE_BUFFER)
        kfree(pURB->transfer_buffer);
#endif
   usb_free_urb( pURB );
}

/*===========================================================================
METHOD:
   GobiUSBNetTXTimeout (Public Method)

DESCRIPTION:
   Timeout declared by the net driver.  Stop all transfers

PARAMETERS
   pNet     [ I ] - Pointer to net device

RETURN VALUE:
   None
===========================================================================*/
void GobiUSBNetTXTimeout( struct net_device * pNet )
{
   struct sGobiUSBNet * pGobiDev;
   sAutoPM * pAutoPM;
   sURBList * pURBListEntry;
   unsigned long activeURBflags, URBListFlags;
   struct usbnet * pDev = netdev_priv( pNet );
   struct urb * pURB;

   if (pDev == NULL || pDev->net == NULL)
   {
      DBG( "failed to get usbnet device\n" );
      return;
   }
   
   pGobiDev = (sGobiUSBNet *)pDev->data[0];
   if (pGobiDev == NULL)
   {
      DBG( "failed to get QMIDevice\n" );
      return;
   }
   pAutoPM = &pGobiDev->mAutoPM;

   DBG( "\n" );

   // Grab a pointer to active URB
   spin_lock_irqsave( &pAutoPM->mActiveURBLock, activeURBflags );
   pURB = pAutoPM->mpActiveURB;
   spin_unlock_irqrestore( &pAutoPM->mActiveURBLock, activeURBflags );
   // Stop active URB
   if (pURB != NULL)
   {
      usb_kill_urb( pURB );
   }

   // Cleanup URB List
   spin_lock_irqsave( &pAutoPM->mURBListLock, URBListFlags );

   pURBListEntry = pAutoPM->mpURBList;
   while (pURBListEntry != NULL)
   {
      pAutoPM->mpURBList = pAutoPM->mpURBList->mpNext;
      atomic_dec( &pAutoPM->mURBListLen );
      usb_free_urb( pURBListEntry->mpURB );
      kfree( pURBListEntry );
      pURBListEntry = pAutoPM->mpURBList;
   }

   spin_unlock_irqrestore( &pAutoPM->mURBListLock, URBListFlags );

   complete( &pAutoPM->mThreadDoWork );

   return;
}

/*===========================================================================
METHOD:
   GobiUSBNetAutoPMThread (Public Method)

DESCRIPTION:
   Handle device Auto PM state asynchronously
   Handle network packet transmission asynchronously

PARAMETERS
   pData     [ I ] - Pointer to sAutoPM struct

RETURN VALUE:
   int - 0 for success
         Negative errno for error
===========================================================================*/
static int GobiUSBNetAutoPMThread( void * pData )
{
   unsigned long activeURBflags, URBListFlags;
   sURBList * pURBListEntry;
   int status;
   struct usb_device * pUdev;
   sAutoPM * pAutoPM = (sAutoPM *)pData;
   struct urb * pURB;

   if (pAutoPM == NULL)
   {
      DBG( "passed null pointer\n" );
      return -EINVAL;
   }
   
   pUdev = interface_to_usbdev( pAutoPM->mpIntf );

   DBG( "traffic thread started\n" );

   while (pAutoPM->mbExit == false)
   {
      // Wait for someone to poke us
      wait_for_completion_interruptible( &pAutoPM->mThreadDoWork );

      // Time to exit?
      if (pAutoPM->mbExit == true)
      {
         // Stop activeURB
         spin_lock_irqsave( &pAutoPM->mActiveURBLock, activeURBflags );
         pURB = pAutoPM->mpActiveURB;
         spin_unlock_irqrestore( &pAutoPM->mActiveURBLock, activeURBflags );

         // EAGAIN used to signify callback is done
         if (IS_ERR( pAutoPM->mpActiveURB )
                 &&  PTR_ERR( pAutoPM->mpActiveURB ) == -EAGAIN )
         {
             pURB = NULL;
         }

         if (pURB != NULL)
         {
            usb_kill_urb( pURB );
         }
         // Will be freed in callback function

         // Cleanup URB List
         spin_lock_irqsave( &pAutoPM->mURBListLock, URBListFlags );

         pURBListEntry = pAutoPM->mpURBList;
         while (pURBListEntry != NULL)
         {
            pAutoPM->mpURBList = pAutoPM->mpURBList->mpNext;
            atomic_dec( &pAutoPM->mURBListLen );
            usb_free_urb( pURBListEntry->mpURB );
            kfree( pURBListEntry );
            pURBListEntry = pAutoPM->mpURBList;
         }

         spin_unlock_irqrestore( &pAutoPM->mURBListLock, URBListFlags );

         break;
      }
      
      // Is our URB active?
      spin_lock_irqsave( &pAutoPM->mActiveURBLock, activeURBflags );

      // EAGAIN used to signify callback is done
      if (IS_ERR( pAutoPM->mpActiveURB ) 
      &&  PTR_ERR( pAutoPM->mpActiveURB ) == -EAGAIN )
      {
         pAutoPM->mpActiveURB = NULL;

         // Restore IRQs so task can sleep
         spin_unlock_irqrestore( &pAutoPM->mActiveURBLock, activeURBflags );
         
         // URB is done, decrement the Auto PM usage count
         usb_autopm_put_interface( pAutoPM->mpIntf );

         // Lock ActiveURB again
         spin_lock_irqsave( &pAutoPM->mActiveURBLock, activeURBflags );
      }

      if (pAutoPM->mpActiveURB != NULL)
      {
         // There is already a URB active, go back to sleep
         spin_unlock_irqrestore( &pAutoPM->mActiveURBLock, activeURBflags );
         continue;
      }
      
      // Is there a URB waiting to be submitted?
      spin_lock_irqsave( &pAutoPM->mURBListLock, URBListFlags );
      if (pAutoPM->mpURBList == NULL)
      {
         // No more URBs to submit, go back to sleep
         spin_unlock_irqrestore( &pAutoPM->mURBListLock, URBListFlags );
         spin_unlock_irqrestore( &pAutoPM->mActiveURBLock, activeURBflags );
         continue;
      }

      // Pop an element
      pURBListEntry = pAutoPM->mpURBList;
      pAutoPM->mpURBList = pAutoPM->mpURBList->mpNext;
      atomic_dec( &pAutoPM->mURBListLen );
      spin_unlock_irqrestore( &pAutoPM->mURBListLock, URBListFlags );

      // Set ActiveURB
      pAutoPM->mpActiveURB = pURBListEntry->mpURB;
      spin_unlock_irqrestore( &pAutoPM->mActiveURBLock, activeURBflags );

      // Tell autopm core we need device woken up
      status = usb_autopm_get_interface( pAutoPM->mpIntf );
      if (status < 0)
      {
         DBG( "unable to autoresume interface: %d\n", status );

         // likely caused by device going from autosuspend -> full suspend
         if (status == -EPERM)
         {
#if (LINUX_VERSION_CODE < KERNEL_VERSION( 2,6,33 ))
#if (LINUX_VERSION_CODE > KERNEL_VERSION( 2,6,18 ))
            pUdev->auto_pm = 0;
#else
             pUdev = pUdev;
#endif
#endif
            GobiNetSuspend( pAutoPM->mpIntf, PMSG_SUSPEND );
         }

         // Add pURBListEntry back onto pAutoPM->mpURBList
         spin_lock_irqsave( &pAutoPM->mURBListLock, URBListFlags );
         pURBListEntry->mpNext = pAutoPM->mpURBList;
         pAutoPM->mpURBList = pURBListEntry;
         atomic_inc( &pAutoPM->mURBListLen );
         spin_unlock_irqrestore( &pAutoPM->mURBListLock, URBListFlags );
         
         spin_lock_irqsave( &pAutoPM->mActiveURBLock, activeURBflags );
         pAutoPM->mpActiveURB = NULL;
         spin_unlock_irqrestore( &pAutoPM->mActiveURBLock, activeURBflags );
         
         // Go back to sleep
         continue;
      }

      // Submit URB
      status = usb_submit_urb( pAutoPM->mpActiveURB, GFP_KERNEL );
      if (status < 0)
      {
         // Could happen for a number of reasons
         DBG( "Failed to submit URB: %d.  Packet dropped\n", status );
         spin_lock_irqsave( &pAutoPM->mActiveURBLock, activeURBflags );
         usb_free_urb( pAutoPM->mpActiveURB );
         pAutoPM->mpActiveURB = NULL;
         spin_unlock_irqrestore( &pAutoPM->mActiveURBLock, activeURBflags );
         usb_autopm_put_interface( pAutoPM->mpIntf );

         // Loop again
         complete( &pAutoPM->mThreadDoWork );
      }
      
      kfree( pURBListEntry );
   }   
   
   DBG( "traffic thread exiting\n" );
   pAutoPM->mpThread = NULL;
   return 0;
}      

/*===========================================================================
METHOD:
   GobiUSBNetStartXmit (Public Method)

DESCRIPTION:
   Convert sk_buff to usb URB and queue for transmit

PARAMETERS
   pNet     [ I ] - Pointer to net device

RETURN VALUE:
   NETDEV_TX_OK on success
   NETDEV_TX_BUSY on error
===========================================================================*/
int GobiUSBNetStartXmit( 
   struct sk_buff *     pSKB,
   struct net_device *  pNet )
{
   unsigned long URBListFlags;
   struct sGobiUSBNet * pGobiDev;
   sAutoPM * pAutoPM;
   sURBList * pURBListEntry, ** ppURBListEnd;
   void * pURBData;
   struct usbnet * pDev = netdev_priv( pNet );
   
   //DBG( "\n" );
   
   if (pDev == NULL || pDev->net == NULL)
   {
      DBG( "failed to get usbnet device\n" );
      return NETDEV_TX_BUSY;
   }
   
   pGobiDev = (sGobiUSBNet *)pDev->data[0];
   if (pGobiDev == NULL)
   {
      DBG( "failed to get QMIDevice\n" );
      return NETDEV_TX_BUSY;
   }
   pAutoPM = &pGobiDev->mAutoPM;
   
   if( NULL == pSKB )
   {
       DBG( "Buffer is NULL \n" );
       return NETDEV_TX_BUSY;
   }

   if (GobiTestDownReason( pGobiDev, DRIVER_SUSPENDED ) == true)
   {
      // Should not happen
      DBG( "device is suspended\n" );
      dump_stack();
      return NETDEV_TX_BUSY;
   }
   
   if (GobiTestDownReason( pGobiDev, NO_NDIS_CONNECTION ))
   {
      //netif_carrier_off( pGobiDev->mpNetDev->net );
      //DBG( "device is disconnected\n" );
      //dump_stack();
      return NETDEV_TX_BUSY;
   }
   
   // Convert the sk_buff into a URB

   // Check if buffer is full
   if ( atomic_read( &pAutoPM->mURBListLen ) >= txQueueLength)
   {
      DBG( "not scheduling request, buffer is full\n" );
      return NETDEV_TX_BUSY;
   }

   // Allocate URBListEntry
   pURBListEntry = kmalloc( sizeof( sURBList ), GFP_ATOMIC );
   if (pURBListEntry == NULL)
   {
      DBG( "unable to allocate URBList memory\n" );
      return NETDEV_TX_BUSY;
   }
   pURBListEntry->mpNext = NULL;

   // Allocate URB
   pURBListEntry->mpURB = usb_alloc_urb( 0, GFP_ATOMIC );
   if (pURBListEntry->mpURB == NULL)
   {
      DBG( "unable to allocate URB\n" );
      // release all memory allocated by now 
      if (pURBListEntry)
         kfree( pURBListEntry );
      return NETDEV_TX_BUSY;
   }

#if 1 //def DATA_MODE_RP
   GobiNetDriverTxFixup(pDev, pSKB, GFP_ATOMIC);	
#endif

   // Allocate URB transfer_buffer
   pURBData = kmalloc( pSKB->len, GFP_ATOMIC );
   if (pURBData == NULL)
   {
      DBG( "unable to allocate URB data\n" );
      // release all memory allocated by now
      if (pURBListEntry)
      {
         usb_free_urb( pURBListEntry->mpURB );
         kfree( pURBListEntry );
      }
      return NETDEV_TX_BUSY;
   }
   // Fill with SKB's data
   memcpy( pURBData, pSKB->data, pSKB->len );

   usb_fill_bulk_urb( pURBListEntry->mpURB,
                      pGobiDev->mpNetDev->udev,
                      pGobiDev->mpNetDev->out,
                      pURBData,
                      pSKB->len,
                      GobiUSBNetURBCallback,
                      pAutoPM );

   /* Handle the need to send a zero length packet and release the
    * transfer buffer
    */
    pURBListEntry->mpURB->transfer_flags |= (URB_ZERO_PACKET | URB_FREE_BUFFER);

   // Aquire lock on URBList
   spin_lock_irqsave( &pAutoPM->mURBListLock, URBListFlags );
   
   // Add URB to end of list
   ppURBListEnd = &pAutoPM->mpURBList;
   while ((*ppURBListEnd) != NULL)
   {
      ppURBListEnd = &(*ppURBListEnd)->mpNext;
   }
   *ppURBListEnd = pURBListEntry;
   atomic_inc( &pAutoPM->mURBListLen );

   spin_unlock_irqrestore( &pAutoPM->mURBListLock, URBListFlags );

   complete( &pAutoPM->mThreadDoWork );

   // Start transfer timer
   pNet->trans_start = jiffies;
   // Free SKB
   if (pSKB)
      dev_kfree_skb_any( pSKB );

   return NETDEV_TX_OK;
}
#endif
static int (*local_usbnet_start_xmit) (struct sk_buff *skb, struct net_device *net);
#endif

static int GobiUSBNetStartXmit2( struct sk_buff *pSKB, struct net_device *pNet ){
   struct sGobiUSBNet * pGobiDev;
   struct usbnet * pDev = netdev_priv( pNet );
   
   //DBG( "\n" );
   
   if (pDev == NULL || pDev->net == NULL)
   {
      DBG( "failed to get usbnet device\n" );
      return NETDEV_TX_BUSY;
   }
   
   pGobiDev = (sGobiUSBNet *)pDev->data[0];
   if (pGobiDev == NULL)
   {
      DBG( "failed to get QMIDevice\n" );
      return NETDEV_TX_BUSY;
   }
   
   if( NULL == pSKB )
   {
       DBG( "Buffer is NULL \n" );
       return NETDEV_TX_BUSY;
   }

   if (GobiTestDownReason( pGobiDev, DRIVER_SUSPENDED ) == true)
   {
      // Should not happen
      DBG( "device is suspended\n" );
      dump_stack();
      return NETDEV_TX_BUSY;
   }
   
   if (GobiTestDownReason( pGobiDev, NO_NDIS_CONNECTION ))
   {
      //netif_carrier_off( pGobiDev->mpNetDev->net );
      //DBG( "device is disconnected\n" );
      //dump_stack();
      return NETDEV_TX_BUSY;
   }

#if (LINUX_VERSION_CODE < KERNEL_VERSION( 2,6,29 ))
   return local_usbnet_start_xmit(pSKB, pNet);
#else
   return usbnet_start_xmit(pSKB, pNet);
#endif
}

/*===========================================================================
METHOD:
   GobiUSBNetOpen (Public Method)

DESCRIPTION:
   Wrapper to usbnet_open, correctly handling autosuspend
   Start AutoPM thread (if CONFIG_PM is defined)

PARAMETERS
   pNet     [ I ] - Pointer to net device

RETURN VALUE:
   int - 0 for success
         Negative errno for error
===========================================================================*/
int GobiUSBNetOpen( struct net_device * pNet )
{
   int status = 0;
   struct sGobiUSBNet * pGobiDev;
   struct usbnet * pDev = netdev_priv( pNet );
 
   if (pDev == NULL)
   {
      DBG( "failed to get usbnet device\n" );
      return -ENXIO;
   }
   
   pGobiDev = (sGobiUSBNet *)pDev->data[0];
   if (pGobiDev == NULL)
   {
      DBG( "failed to get QMIDevice\n" );
      return -ENXIO;
   }

   DBG( "\n" );

#ifdef CONFIG_PM
   #if (LINUX_VERSION_CODE < KERNEL_VERSION( 2,6,29 ))
   // Start the AutoPM thread
   pGobiDev->mAutoPM.mpIntf = pGobiDev->mpIntf;
   pGobiDev->mAutoPM.mbExit = false;
   pGobiDev->mAutoPM.mpURBList = NULL;
   pGobiDev->mAutoPM.mpActiveURB = NULL;
   spin_lock_init( &pGobiDev->mAutoPM.mURBListLock );
   spin_lock_init( &pGobiDev->mAutoPM.mActiveURBLock );
   atomic_set( &pGobiDev->mAutoPM.mURBListLen, 0 );
   init_completion( &pGobiDev->mAutoPM.mThreadDoWork );
   
   pGobiDev->mAutoPM.mpThread = kthread_run( GobiUSBNetAutoPMThread, 
                                               &pGobiDev->mAutoPM, 
                                               "GobiUSBNetAutoPMThread" );
   if (IS_ERR( pGobiDev->mAutoPM.mpThread ))
   {
      DBG( "AutoPM thread creation error\n" );
      return PTR_ERR( pGobiDev->mAutoPM.mpThread );
   }
   #endif
#endif /* CONFIG_PM */

   // Allow traffic
   GobiClearDownReason( pGobiDev, NET_IFACE_STOPPED );

   // Pass to usbnet_open if defined
   if (pGobiDev->mpUSBNetOpen != NULL)
   {
      status = pGobiDev->mpUSBNetOpen( pNet );
#ifdef CONFIG_PM
      // If usbnet_open was successful enable Auto PM
      if (status == 0)
      {
#if (LINUX_VERSION_CODE < KERNEL_VERSION( 2,6,33 ))
         usb_autopm_enable( pGobiDev->mpIntf );
#else
         usb_autopm_put_interface( pGobiDev->mpIntf );
#endif
      }
#endif /* CONFIG_PM */
   }
   else
   {
      DBG( "no USBNetOpen defined\n" );
   }
   
   return status;
}

/*===========================================================================
METHOD:
   GobiUSBNetStop (Public Method)

DESCRIPTION:
   Wrapper to usbnet_stop, correctly handling autosuspend
   Stop AutoPM thread (if CONFIG_PM is defined)

PARAMETERS
   pNet     [ I ] - Pointer to net device

RETURN VALUE:
   int - 0 for success
         Negative errno for error
===========================================================================*/
int GobiUSBNetStop( struct net_device * pNet )
{
   struct sGobiUSBNet * pGobiDev;
   struct usbnet * pDev = netdev_priv( pNet );

   if (pDev == NULL || pDev->net == NULL)
   {
      DBG( "failed to get netdevice\n" );
      return -ENXIO;
   }
   
   pGobiDev = (sGobiUSBNet *)pDev->data[0];
   if (pGobiDev == NULL)
   {
      DBG( "failed to get QMIDevice\n" );
      return -ENXIO;
   }

   // Stop traffic
   GobiSetDownReason( pGobiDev, NET_IFACE_STOPPED );

#ifdef CONFIG_PM
   #if (LINUX_VERSION_CODE < KERNEL_VERSION( 2,6,29 ))
   // Tell traffic thread to exit
   pGobiDev->mAutoPM.mbExit = true;
   complete( &pGobiDev->mAutoPM.mThreadDoWork );
   
   // Wait for it to exit
   while( pGobiDev->mAutoPM.mpThread != NULL )
   {
      msleep( 100 );
   }
   DBG( "thread stopped\n" );
   #endif
#endif /* CONFIG_PM */

   // Pass to usbnet_stop, if defined
   if (pGobiDev->mpUSBNetStop != NULL)
   {
      return pGobiDev->mpUSBNetStop( pNet );
   }
   else
   {
      return 0;
   }
}

/*=========================================================================*/
// Struct driver_info
/*=========================================================================*/
static const struct driver_info GobiNetInfo = 
{
   .description   = "GobiNet Ethernet Device",
#ifdef CONFIG_ANDROID
   .flags         = FLAG_ETHER | FLAG_POINTTOPOINT, //usb0
#else
   .flags         = FLAG_ETHER,
#endif
   .bind          = GobiNetDriverBind,
   .unbind        = GobiNetDriverUnbind,
#if 1 //def DATA_MODE_RP
#ifdef QUECTEL_WWAN_MULTI_PACKAGES
   .rx_fixup      = GobiNetDriverRxPktsFixup,
#else
   .rx_fixup      = GobiNetDriverRxFixup,
#endif
   .tx_fixup      = GobiNetDriverTxFixup,
#endif
   .data          = (1 << 4),
};

/*=========================================================================*/
// Qualcomm Gobi 3000 VID/PIDs
/*=========================================================================*/
#define GOBI_FIXED_INTF(vend, prod) \
    { \
          USB_DEVICE( vend, prod ), \
          .driver_info = (unsigned long)&GobiNetInfo, \
    }
static const struct usb_device_id GobiVIDPIDTable [] =
{
    GOBI_FIXED_INTF( 0x05c6, 0x9003 ), // Quectel UC20
    GOBI_FIXED_INTF( 0x05c6, 0x9215 ), // Quectel EC20
    GOBI_FIXED_INTF( 0x2c7c, 0x0125 ),  // Quectel EC25
    GOBI_FIXED_INTF( 0x2c7c, 0x0121 ), // Quectel EC21
    GOBI_FIXED_INTF( 0x2c7c, 0x0306 ), // Quectel EP06
    GOBI_FIXED_INTF( 0x2c7c, 0x0435 ), // Quectel AG35
    GOBI_FIXED_INTF( 0x2c7c, 0x0296 ), // Quectel BG96
    GOBI_FIXED_INTF( 0x2c7c, 0x0191 ), // Quectel EG91
    GOBI_FIXED_INTF( 0x2c7c, 0x0195 ), // Quectel EG95	
   //Terminating entry
   { }
};

MODULE_DEVICE_TABLE( usb, GobiVIDPIDTable );

/*===========================================================================
METHOD:
   GobiUSBNetProbe (Public Method)

DESCRIPTION:
   Run usbnet_probe
   Setup QMI device

PARAMETERS
   pIntf        [ I ] - Pointer to interface
   pVIDPIDs     [ I ] - Pointer to VID/PID table

RETURN VALUE:
   int - 0 for success
         Negative errno for error
===========================================================================*/
int GobiUSBNetProbe( 
   struct usb_interface *        pIntf, 
   const struct usb_device_id *  pVIDPIDs )
{
   int status;
   struct usbnet * pDev;
   sGobiUSBNet * pGobiDev;
#if (LINUX_VERSION_CODE >= KERNEL_VERSION( 2,6,29 ))
   struct net_device_ops * pNetDevOps;
#endif   

   status = usbnet_probe( pIntf, pVIDPIDs );
   if (status < 0)
   {
      DBG( "usbnet_probe failed %d\n", status );
	  return status; 
   }

#if (LINUX_VERSION_CODE >= KERNEL_VERSION( 2,6,19 ))
   pIntf->needs_remote_wakeup = 1;
#endif

#if (LINUX_VERSION_CODE > KERNEL_VERSION( 2,6,23 ))
   pDev = usb_get_intfdata( pIntf );
#else
   pDev = (struct usbnet *)pIntf->dev.platform_data;
#endif

   if (pDev == NULL || pDev->net == NULL)
   {
      DBG( "failed to get netdevice\n" );
      usbnet_disconnect( pIntf );
      return -ENXIO;
   }

   pGobiDev = kzalloc( sizeof( sGobiUSBNet ), GFP_KERNEL );
   if (pGobiDev == NULL)
   {
      DBG( "falied to allocate device buffers" );
      usbnet_disconnect( pIntf );
      return -ENOMEM;
   }
   
   atomic_set(&pGobiDev->refcount, 1);

   pDev->data[0] = (unsigned long)pGobiDev;
   
   pGobiDev->mpNetDev = pDev;

   // Clearing endpoint halt is a magic handshake that brings 
   // the device out of low power (airplane) mode
   usb_clear_halt( pGobiDev->mpNetDev->udev, pDev->out );

   // Overload PM related network functions
#if (LINUX_VERSION_CODE < KERNEL_VERSION( 2,6,29 ))
   pGobiDev->mpUSBNetOpen = pDev->net->open;
   pDev->net->open = GobiUSBNetOpen;
   pGobiDev->mpUSBNetStop = pDev->net->stop;
   pDev->net->stop = GobiUSBNetStop;
#if defined(CONFIG_PM) && (LINUX_VERSION_CODE > KERNEL_VERSION( 2,6,14 ))
   pDev->net->hard_start_xmit = GobiUSBNetStartXmit;
   pDev->net->tx_timeout = GobiUSBNetTXTimeout;
#else  //quectel donot send dhcp request before ndis connect for uc20
    local_usbnet_start_xmit = pDev->net->hard_start_xmit;
    pDev->net->hard_start_xmit = GobiUSBNetStartXmit2;
#endif
#else
   pNetDevOps = kmalloc( sizeof( struct net_device_ops ), GFP_KERNEL );
   if (pNetDevOps == NULL)
   {
      DBG( "falied to allocate net device ops" );
      usbnet_disconnect( pIntf );
      return -ENOMEM;
   }
   memcpy( pNetDevOps, pDev->net->netdev_ops, sizeof( struct net_device_ops ) );
   
   pGobiDev->mpUSBNetOpen = pNetDevOps->ndo_open;
   pNetDevOps->ndo_open = GobiUSBNetOpen;
   pGobiDev->mpUSBNetStop = pNetDevOps->ndo_stop;
   pNetDevOps->ndo_stop = GobiUSBNetStop;
#if 1 //quectel donot send dhcp request before ndis connect for uc20
   pNetDevOps->ndo_start_xmit = GobiUSBNetStartXmit2;
#else
   pNetDevOps->ndo_start_xmit = usbnet_start_xmit;
#endif
   pNetDevOps->ndo_tx_timeout = usbnet_tx_timeout;

   pDev->net->netdev_ops = pNetDevOps;
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION( 2,6,31 ))
   memset( &(pGobiDev->mpNetDev->stats), 0, sizeof( struct net_device_stats ) );
#else
   memset( &(pGobiDev->mpNetDev->net->stats), 0, sizeof( struct net_device_stats ) );
#endif

   pGobiDev->mpIntf = pIntf;
   memset( &(pGobiDev->mMEID), '0', 14 );
   
   DBG( "Mac Address:\n" );
   PrintHex( &pGobiDev->mpNetDev->net->dev_addr[0], 6 );

   pGobiDev->mbQMIValid = false;
   memset( &pGobiDev->mQMIDev, 0, sizeof( sQMIDev ) );
   pGobiDev->mQMIDev.mbCdevIsInitialized = false;

   pGobiDev->mQMIDev.mpDevClass = gpClass;
   
#ifdef CONFIG_PM
   #if (LINUX_VERSION_CODE < KERNEL_VERSION( 2,6,29 ))
   init_completion( &pGobiDev->mAutoPM.mThreadDoWork );
   #endif
#endif /* CONFIG_PM */
   spin_lock_init( &pGobiDev->mQMIDev.mClientMemLock );

   // Default to device down
   pGobiDev->mDownReason = 0;

//#if (LINUX_VERSION_CODE < KERNEL_VERSION( 3,11,0 ))
   GobiSetDownReason( pGobiDev, NO_NDIS_CONNECTION );
   GobiSetDownReason( pGobiDev, NET_IFACE_STOPPED );
//#endif

   // Register QMI
   pGobiDev->mbMdm9x07 |= (pDev->udev->descriptor.idVendor == cpu_to_le16(0x2c7c));
   pGobiDev->mbRawIPMode = pGobiDev->mbMdm9x07;
#ifdef CONFIG_BRIDGE
   memcpy(pGobiDev->mHostMAC, pDev->net->dev_addr, 6);
#endif
   status = RegisterQMIDevice( pGobiDev );
   if (status != 0)
   {
      // usbnet_disconnect() will call GobiNetDriverUnbind() which will call
      // DeregisterQMIDevice() to clean up any partially created QMI device
      usbnet_disconnect( pIntf );
      return status;
   }
   
   // Success
   return 0;
}

static struct usb_driver GobiNet =
{
   .name       = "GobiNet",
   .id_table   = GobiVIDPIDTable,
   .probe      = GobiUSBNetProbe,
   .disconnect = usbnet_disconnect,
#ifdef CONFIG_PM
   .suspend    = GobiNetSuspend,
   .resume     = GobiNetResume,
#if (LINUX_VERSION_CODE > KERNEL_VERSION( 2,6,18 ))   
   .supports_autosuspend = true,
#endif   
#endif /* CONFIG_PM */
};

/* Add node /proc/usb-ec20 to reset ec20 module */
#ifdef CONFIG_DT_SF16A18_FULLMASK_CPE

#include <linux/proc_fs.h>
static int ec20_reset_show(struct seq_file *m, void *v)
{
	return 0;
}
static int ec20_reset_open(struct inode *inode, struct file *file)
{
	return single_open(file, ec20_reset_show, PDE_DATA(inode));
}

static ssize_t ec20_reset_read(struct file *file, char __user *buffer,
						size_t count, loff_t *f_ops)
{
	struct device_node *ec20;
	char *buff;
	int ret, pwrkey, reset_n;

	if(*f_ops > 0){
		return 0;
	}

	buff = kmalloc(sizeof(char) * count, GFP_KERNEL);
	if(!buff)
		return -ENOMEM;

	ec20 = of_find_compatible_node(NULL, NULL, "siflower,sfax8-ec20");
	if (ec20) {
		pwrkey = of_get_named_gpio(ec20, "pwrkey", 0);
		reset_n = of_get_named_gpio(ec20, "reset_n", 0);
	}

	ret = snprintf(buff, count, "reset_n %d, pwrkey %d\n",
			gpio_get_value(reset_n), gpio_get_value(pwrkey));
	if(copy_to_user(buffer, buff, ret))
		ret = -EFAULT;
	*f_ops += ret;

	kfree(buff);
	return ret;
}

static int do_ec20_reset(void)
{
	struct device_node *ec20;
	int reset_n = 0;

	ec20 = of_find_compatible_node(NULL, NULL, "siflower,sfax8-ec20");
	if (ec20)
		reset_n = of_get_named_gpio(ec20, "reset_n", 0);

	if (reset_n) {
		gpio_set_value(reset_n, 1);
		mdelay(400); /* 150ms <= reset_n <= 460ms*/
		gpio_set_value(reset_n, 0);
	}

	return 0;
}

static int do_ec20_pwrup(void)
{
	struct device_node *ec20;
	int pwrkey = 0;

	ec20 = of_find_compatible_node(NULL, NULL, "siflower,sfax8-ec20");
	if (ec20)
		pwrkey = of_get_named_gpio(ec20, "pwrkey", 0);

	if (pwrkey) {
		gpio_set_value(pwrkey, 1);
		mdelay(800); /* pwrkey >= 650ms */
		gpio_set_value(pwrkey, 0);
	}

	return 0;
}

static int do_ec20_pwrdown(void)
{
	/* same to pwrup */
	do_ec20_pwrup();
	printk("please wait 30s to powerdonw ec20 module!\n");
	return 0;
}

static ssize_t ec20_reset_write(struct file *file, const char __user *buffer,
							size_t count, loff_t *f_ops)
{
	unsigned int value;

	sscanf(buffer, "%u", &value);

	switch (value) {
	case 1:
		do_ec20_reset();
		break;
	case 2:
		do_ec20_pwrup();
		break;
	case 3:
		do_ec20_pwrdown();
		break;
	default:
		break;
	}

	return count;
}

static struct file_operations ec20_reset_ops = {
	.owner		= THIS_MODULE,
	.open		= ec20_reset_open,
	.read		= ec20_reset_read,
	.write		= ec20_reset_write,
	.release	= single_release,
	.llseek		= seq_lseek,
};

static int create_ec20_reset_procfs(void)
{
	if (!proc_create_data("usb-ec20", 0644, NULL, &ec20_reset_ops, NULL))
		printk("failed to create /proc/usb-ec20\n");
	return 0;
}
#endif /* CONFIG_DT_SF16A18_FULLMASK_CPE */

/*===========================================================================
METHOD:
   GobiUSBNetModInit (Public Method)

DESCRIPTION:
   Initialize module
   Create device class
   Register out usb_driver struct

RETURN VALUE:
   int - 0 for success
         Negative errno for error
===========================================================================*/
static int __init GobiUSBNetModInit( void )
{
#ifdef CONFIG_DT_SF16A18_FULLMASK_CPE
   int reset_n = 0, pwrkey = 0;
   struct device_node *ec20;
#endif
   gpClass = class_create( THIS_MODULE, "GobiQMI" );
   if (IS_ERR( gpClass ) == true)
   {
      DBG( "error at class_create %ld\n",
           PTR_ERR( gpClass ) );
      return -ENOMEM;
   }

#ifdef CONFIG_DT_SF16A18_FULLMASK_CPE
   /* reset EC20 by reset_n(gpio 26) && pwrkey(gpio 29) */
   ec20 = of_find_compatible_node(NULL, NULL, "siflower,sfax8-ec20");
   if (ec20) {
	   reset_n = of_get_named_gpio(ec20, "reset_n", 0);
	   pwrkey = of_get_named_gpio(ec20, "pwrkey", 0);
   } else {
	   DBG("pwrkey gpio not found in dts!\n");
   }

   if (reset_n) {
	   if (gpio_request(reset_n, "reset_n") == 0)
		   gpio_direction_output(reset_n, 0);
	   else
		   printk("failed to request gpio %d\n", reset_n);
   }
   if (pwrkey) {
	   if (gpio_request(pwrkey, "pwrkey") == 0) {
		   gpio_direction_output(pwrkey, 1);
		   mdelay(600); /* pwrkey >= 500ms */
		   gpio_direction_output(pwrkey, 0);
	   } else {
		   printk("failed to request gpio %d\n", pwrkey);
	   }
   }
   if (reset_n) {
	   gpio_direction_output(reset_n, 1);
	   mdelay(400); /* 150 <= reset_n <= 460*/
	   gpio_direction_output(reset_n, 0);
   }
   create_ec20_reset_procfs();
#endif

   // This will be shown whenever driver is loaded
   printk( KERN_INFO "%s: %s\n", DRIVER_DESC, DRIVER_VERSION );

   return usb_register( &GobiNet );
}
module_init( GobiUSBNetModInit );

/*===========================================================================
METHOD:
   GobiUSBNetModExit (Public Method)

DESCRIPTION:
   Deregister module
   Destroy device class

RETURN VALUE:
   void
===========================================================================*/
static void __exit GobiUSBNetModExit( void )
{
#ifdef CONFIG_DT_SF16A18_FULLMASK_CPE
   int pwrkey = 0;
   struct device_node *ec20;
#endif
   usb_deregister( &GobiNet );

   class_destroy( gpClass );

#ifdef CONFIG_DT_SF16A18_FULLMASK_CPE
   /* reset EC20 by reset_n(gpio 26) && pwrkey(gpio 29) */
   ec20 = of_find_compatible_node(NULL, NULL, "siflower,sfax8-ec20");
   if (ec20)
	   pwrkey = of_get_named_gpio(ec20, "pwrkey", 0);
   if (pwrkey) {
	   gpio_direction_output(pwrkey, 1);
	   mdelay(800); /* pwrkey >= 650ms */
	   gpio_direction_output(pwrkey, 0);
   } else {
	   DBG("pwrkey gpio not found in dts!\n");
   }
#endif

}
module_exit( GobiUSBNetModExit );

MODULE_VERSION( DRIVER_VERSION );
MODULE_AUTHOR( DRIVER_AUTHOR );
MODULE_DESCRIPTION( DRIVER_DESC );
MODULE_LICENSE("Dual BSD/GPL");

#ifdef bool
#undef bool
#endif

module_param( debug, int, S_IRUGO | S_IWUSR );
MODULE_PARM_DESC( debug, "Debuging enabled or not" );

module_param( interruptible, int, S_IRUGO | S_IWUSR );
MODULE_PARM_DESC( interruptible, "Listen for and return on user interrupt" );
module_param( txQueueLength, int, S_IRUGO | S_IWUSR );
MODULE_PARM_DESC( txQueueLength, 
                  "Number of IP packets which may be queued up for transmit" );

