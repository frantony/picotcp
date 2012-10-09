#include "pico_config.h"
#include "pico_frame.h"
#include "pico_device.h"
#include "pico_protocol.h"
#include "pico_addressing.h"

#include "pico_eth.h"
#include "pico_arp.h"
#include "pico_ipv4.h"
#include "pico_ipv6.h"





/* SOCKET LEVEL: interface towards transport */
int pico_socket_receive(struct pico_frame *f)
{
  /* TODO: recognize the correspondant socket */
  return 0;
}

/* TRANSPORT LEVEL: interface towards network */
int pico_transport_receive(struct pico_frame *f)
{
  /* TODO: identify transport level, deliver packet to the 
   * correct destination (e.g. socket)*/
  return 0;
}

int pico_network_receive(struct pico_frame *f)
{
  if (IS_IPV4(f))
    pico_enqueue(pico_proto_ipv4.q_in, f);
  else if (IS_IPV6(f))
    pico_enqueue(pico_proto_ipv6.q_in, f);
  else {
    pico_frame_discard(f);
    return -1;
  }
  return f->buffer_len;
}


/* DATALINK LEVEL: interface from network to the device
 * and vice versa.
 */

/* The pico_ethernet_receive() function is used by 
 * those devices supporting ETH in order to push packets up 
 * into the stack. 
 */
int pico_ethernet_receive(struct pico_frame *f)
{
  struct pico_eth_hdr *hdr;
  if (!f || !f->dev || f->datalink_hdr)
    goto discard;
  hdr = (struct pico_eth_hdr *) f->datalink_hdr;
  f->datalink_len = sizeof(struct pico_eth_hdr);
  if ( (memcmp(hdr->daddr, f->dev->eth->mac.addr, PICO_SIZE_ETH) != 0) && 
    (memcmp(hdr->daddr, PICO_ETHADDR_ANY, PICO_SIZE_ETH) != 0) )
    goto discard;
  f->net_hdr = (uint8_t *)f->datalink_hdr + f->datalink_len;
  if (hdr->proto == PICO_IDETH_ARP)
    return pico_arp_receive(f);
  if ((hdr->proto == PICO_IDETH_IPV4) || (hdr->proto == PICO_IDETH_IPV6))
    return pico_network_receive(f);
discard:
  pico_frame_discard(f);
  return -1;
}

/* This is called by dev loop in order to ensure correct ethernet addressing.
 * Returns 0 if the destination is unknown, and -1 if the packet is not deliverable
 * due to ethernet addressing (i.e., no arp association was possible. 
 *
 * Only IP packets must pass by this. ARP will always use direct dev->send() function, so
 * we assume IP is used.
 */
static int pico_ethernet_send(struct pico_frame *f)
{
  struct pico_arp *a4 = NULL;
  struct pico_eth *dstmac = NULL;


  if (IS_IPV6(f)) {
    /*TODO: Neighbor solicitation */
    dstmac = NULL;
  }

  else if (IS_IPV4(f)) {
    if (IS_BCAST(f)) {
     dstmac = (struct pico_eth *) PICO_ETHADDR_ANY;
    } else {
      a4 = pico_arp_get(f);
      if (!a4) {
       if (++ f->failure_count < 3) {
         pico_arp_query(f);
          return 0;
       } else return -1;
      }
      dstmac = (struct pico_eth *) a4;
    }
    /* This sets destination and source address, then pushes the packet to the device. */
    if (dstmac && (f->start > f->buffer) && ((f->start - f->buffer) >= PICO_SIZE_ETHHDR)) {
      struct pico_eth_hdr *hdr;
      f->start -= PICO_SIZE_ETHHDR;
      f->len += PICO_SIZE_ETHHDR;
      f->datalink_hdr = f->start;
      f->datalink_len = PICO_SIZE_ETHHDR;
      hdr = (struct pico_eth_hdr *) f->datalink_hdr;
      memcpy(hdr->saddr, f->dev->eth->mac.addr, PICO_SIZE_ETH);
      memcpy(hdr->daddr, dstmac, PICO_SIZE_ETH);
      hdr->proto = PICO_IDETH_IPV4;
      return f->dev->send(f->dev, f->start, f->len);
    } else return -1;
  } /* End IPV4 ethernet addressing */
  return -1;
}


/* LOWEST LEVEL: interface towards devices. */
/* Device driver will call this function which returns immediately.
 * Incoming packet will be processed later on in the dev loop.
 */
int picotcp_stack_recv(struct pico_device *dev, uint8_t *buffer, int len)
{
  struct pico_frame *f;
  if (len <= 0)
    return -1;
  f = pico_frame_alloc(len);
  if (!f)
    return -1;

  /* Association to the device that just received the frame. */
  f->dev = dev;

  /* Setup the start pointer, lenght. */
  f->start = f->buffer;
  f->len = f->buffer_len;

  memcpy(f->buffer, buffer, len);
  return pico_enqueue(dev->q_in, f);
}

int pico_sendto_dev(struct pico_frame *f)
{
  if (!f->dev) {
    pico_frame_discard(f);
    return -1;
  } else {
    return pico_enqueue(f->dev->q_out, f);
  }
}

void pico_dev_loop(struct pico_device *dev, int loop_score)
{
  struct pico_frame *f;
  while(loop_score > 0) {
    if (dev->q_in->frames + dev->q_out->frames <= 0)
      break;

    /* Device dequeue + send */
    f = pico_dequeue(dev->q_out);
    if (f) {
      if (dev->eth) {
        if (0 == pico_ethernet_send(f)) /* Addressing is in progress. Enqueue again. */
          pico_enqueue(dev->q_out, f);
      } else {
        dev->send(dev, f->start, f->len);
      }
      pico_frame_discard(f);
      loop_score--;
    }

    /* Receive */
    f = pico_dequeue(dev->q_in);
    if (f) {
      if (dev->eth) {
        f->datalink_hdr = f->buffer;
        pico_ethernet_receive(f);
      } else {
        f->net_hdr = f->buffer;
        pico_network_receive(f);
      }
      loop_score--;
    }
  }
}


void pico_proto_loop(struct pico_protocol *proto, int loop_score)
{
  struct pico_frame *f;
  while(loop_score >0) {
    if (proto->q_in->frames + proto->q_out->frames <= 0)
      break;

    f = pico_dequeue(proto->q_out);
    if ((f) &&(proto->process_out(proto, f) > 0)) {
      loop_score--;
    }

    f = pico_dequeue(proto->q_in);
    if ((f) &&(proto->process_in(proto, f) > 0)) {
      loop_score--;
    }
  }
}