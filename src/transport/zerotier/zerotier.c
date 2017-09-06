//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifdef NNG_HAVE_ZEROTIER
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"

const char *nng_opt_zt_home = "zt:home";
const char *nng_opt_zt_nwid = "zt:nwid";
const char *nng_opt_zt_node = "zt:node";

int nng_optid_zt_home = -1;
int nng_optid_zt_nwid = -1;

#ifndef _WIN32
#include <unistd.h>
#endif

#include <ZeroTierOne.h>

// ZeroTier Transport.  This sits on the ZeroTier L2 network, which itself
// is implemented on top of UDP.  This requires the 3rd party
// libzerotiercore library (which is GPLv3!) and platform specific UDP
// functionality to be built in.  Note that care must be taken to link
// dynamically if one wishes to avoid making your entire application GPL3.
// (Alternatively ZeroTier offers commercial licenses which may prevent
// this particular problem.)  This implementation does not make use of
// certain advanced capabilities in ZeroTier such as more sophisticated
// route management and TCP fallback.  You need to have connectivity
// to the Internet to use this.  (Or at least to your Planetary root.)
//
// Because ZeroTier takes a while to establish connectivity, it is even
// more important that applicaitons using the ZeroTier transport not
// assume that a connection will be immediately available.  It can take
// quite a few seconds for peer-to-peer connectivity to be established.
//
// The ZeroTier transport was funded by Capitar IT Group, BV.
//
// This transport is highly experimental.

// ZeroTier and UDP are connectionless, but nng is designed around
// connection oriented paradigms.  Therefore we will emulate a connection
// as follows:
//
// xxx...
//
typedef struct zt_pipe zt_pipe;
typedef struct zt_ep   zt_ep;
typedef struct zt_node zt_node;

#define NZT_ETHER 0x0901 // We use this ethertype

// opcodes.  As these are encoded with the 4 bit flags field, we just
// shift them up one nibble.  The flags will be set in the lower nibble.
#define NZT_OP_DAT 0x00 // Data message
#define NZT_OP_CON 0x10 // Connection request
#define NZT_OP_DIS 0x20 // Disconnect request
#define NZT_OP_PNG 0x30 // Ping (keep alive)
#define NZT_OP_ERR 0x40 // Error

#define NZT_FLAG_MF 0x01 // more fragments
#define NZT_FLAG_AK 0x02 // acknowledgement

#define NZT_OP_CON_REQ (NZT_OP_CON)
#define NZT_OP_CON_ACK (NZT_OP_CON | NZT_FLAG_AK)
#define NZT_OP_PNG_REQ (NZT_OP_PNG)
#define NZT_OP_PNG_ACK (NZT_OP_PNG | NZT_FLAG_AK)

#define NZT_VERSION 0x01 // specified per RFC

#define NZT_OFFS_OPFLAGS 0
#define NZT_OFFS_VERSION 1
#define NZT_OFFS_DST_PORT 2 // includes reserved high order bits
#define NZT_OFFS_SRC_PORT 6 // includes reserved high order bits

#define NZT_OFFS_CON_PROT 10
#define NZT_SIZE_CON 12

#define NZT_OFFS_DAT_MSGID 10
#define NZT_OFFS_DAT_FRLEN 12
#define NZT_OFFS_DAT_FROFF 14
#define NZT_OFFS_DAT_DATA 16
#define NZT_SIZE_DAT 16 // does not include the user data

#define NZT_OFFS_ERR_CODE 10
#define NZT_OFFS_ERR_MSG 11

#define NZT_EREFUSED 0x01 // Nothing there, connection refused
#define NZT_ENOTCONN 0x02 // Connection does not exist
#define NZT_EWRONGSP 0x03 // Wrong SP number
#define NZT_EPROTERR 0x04 // Other protocol error
#define NZT_EMSGSIZE 0x05 // Message too large
#define NZT_EUNKNOWN 0xff // Other errors

// Ephemeral ports are those with the high order bit set.  There are
// about 8 million ephemeral ports, and about 8 million static ports.
// We restrict ourselves to just 24 bit port numbers.  This lets us
// construct 64-bit conversation IDs by combining the 24-bit port
// number with the 40-bit node address.  This means that 64-bits can
// be used to uniquely identify any address.
#define NZT_EPHEMERAL (1U << 23)
#define NZT_MAX_PORT ((1U << 24) - 1)

// We queue UDP for transmit asynchronously.  In order to avoid consuming
// all RAM, we limit the UDP queue len to this many frames.   (Note that
// this queue should pretty well always be empty.)
#define NZT_UDP_MAXQ 16

// Connection timeout maximum.  Basically we expect that a remote host
// will respond within this many usecs to a connection request.  Note
// that on Linux TCP connection timeouts are about 20s, and on other systems
// they are about 72s.  This seems rather ridiculously long.  Modern
// Internet latencies generally never exceed 500ms, and processes should
// not be MIA for more than a few hundred ms as well.  Having said that, it
// can take a number of seconds for ZT topology to settle out.

// Connection retry and timeouts.  We send a connection attempt up to
// 12 times, before giving up and reporting to the user.  Each attempt
// is separated from the previous by five seconds.  We need long timeouts
// because it can take time for ZT to stabilize.  This gives us 60 seconds
// to get a good connection.
#define NZT_CONN_ATTEMPTS (12 * 5)
#define NZT_CONN_INTERVAL (5000000)

// It can take a while to establish the network connection.  This is because
// ZT is doing crypto operations, and discovery of network topology (both
// physical and virtual).  We assume this should complete within half a
// minute or so though.
#define NZT_JOIN_MAXTIME (30000000)

// In theory UDP can send/recv 655507, but ZeroTier won't do more
// than the ZT_MAX_MTU for it's virtual networks  So we need to add some
// extra space for ZT overhead, which is currently 52 bytes, but we want
// to leave room for future growth; 128 bytes seems sufficient.  The vast
// majority of frames will be far far smaller -- typically Ethernet MTUs
// are 1500 bytes.
#define NZT_MAX_HEADROOM 128
#define NZT_RCV_BUFSZ (ZT_MAX_MTU + NZT_MAX_HEADROOM)

// This node structure is wrapped around the ZT_node; this allows us to
// have multiple endpoints referencing the same ZT_node, but also to
// support different nodes (identities) based on different homedirs.
// This means we need to stick these on a global linked list, manage
// them with a reference count, and uniquely identify them using the
// homedir.
struct zt_node {
	char          zn_path[NNG_MAXADDRLEN]; // ought to be sufficient
	ZT_Node *     zn_znode;
	uint64_t      zn_self;
	nni_list_node zn_link;
	int           zn_closed;
	nni_plat_udp *zn_udp4;
	nni_plat_udp *zn_udp6;
	nni_list      zn_eplist;
	nni_list      zn_plist;
	nni_idhash *  zn_ports;
	nni_idhash *  zn_eps;
	nni_idhash *  zn_pipes;
	nni_aio *     zn_rcv4_aio;
	char *        zn_rcv4_buf;
	nng_sockaddr  zn_rcv4_addr;
	nni_aio *     zn_rcv6_aio;
	char *        zn_rcv6_buf;
	nng_sockaddr  zn_rcv6_addr;
	nni_thr       zn_bgthr;
	nni_time      zn_bgtime;
	nni_cv        zn_bgcv;
	nni_cv        zn_snd6_cv;
};

struct zt_pipe {
	nni_list_node zp_link;
	const char *  zp_addr;
	uint16_t      zp_peer;
	uint16_t      zp_proto;
	size_t        zp_rcvmax;
	nni_aio *     zp_user_txaio;
	nni_aio *     zp_user_rxaio;
	uint32_t      zp_src_port;
	uint32_t      zp_dst_port;
	uint64_t      zp_dst_node;
	uint64_t      zp_src_node;
	zt_node *     zp_ztn;

	// XXX: fraglist
	nni_sockaddr zp_remaddr;
	nni_sockaddr zp_locaddr;

	nni_aio *zp_txaio;
	nni_aio *zp_rxaio;
	nni_aio *zp_negaio;
	nni_msg *zp_rxmsg;
};

typedef struct zt_creq zt_creq;
struct zt_creq {
	uint64_t cr_time;
	uint64_t cr_peer_addr;
	uint32_t cr_peer_port;
	uint16_t cr_proto;
};

#define NZT_LISTENQ 128
#define NZT_LISTEN_EXPIRE 60 // seconds before we give up in the backlog

struct zt_ep {
	nni_list_node ze_link;
	char          ze_url[NNG_MAXADDRLEN];
	char          ze_home[NNG_MAXADDRLEN]; // should be enough
	zt_node *     ze_ztn;
	uint64_t      ze_nwid;
	uint64_t      ze_mac; // our own mac address
	int           ze_mode;
	nni_sockaddr  ze_addr;
	uint64_t      ze_rnode; // remote node address
	uint64_t      ze_lnode; // local node address
	uint32_t      ze_rport;
	uint32_t      ze_lport;
	uint16_t      ze_proto;
	size_t        ze_rcvmax;
	nni_aio *     ze_aio;
	nni_aio *     ze_creq_aio;
	int           ze_creq_try;
	nni_list      ze_aios;
	int           ze_maxmtu;
	int           ze_phymtu;

	// Incoming connection requests (server only).  We only
	// only have "accepted" requests -- that is we won't have an
	// established connection/pipe unless the application calls
	// accept.  Since the "application" is our library, that should
	// be pretty much as fast we can run.
	zt_creq ze_creqs[NZT_LISTENQ];
	int     ze_creq_head;
	int     ze_creq_tail;
};

// Locking strategy.  At present the ZeroTier core is not reentrant or fully
// threadsafe.  (We expect this will be fixed.)  Furthermore, there are
// some significant challenges in dealing with locks associated with the
// callbacks, etc.  So we take a big-hammer approach, and just use a single
// global lock for everything.  We hold this lock when calling into the
// ZeroTier framework.  Since ZeroTier has no independent threads, that
// means that it will always hold this lock in its core, and the lock will
// also be held automatically in any of our callbacks.  We never hold any
// other locks across ZeroTier core calls. We may not acquire the global
// lock in callbacks (they will already have it held). Any other locks
// can be acquired as long as they are not held during calls into ZeroTier.
//
// This will have a detrimental impact on performance, but to be completely
// honest we don't think anyone will be using the ZeroTier transport in
// excessively performance critical applications; scalability may become
// a factor for large servers sitting in a ZeroTier hub situation.  (Then
// again, since only the zerotier procesing is single threaded, it may not
// be that much of a bottleneck -- really depends on how expensive these
// operations are.  We can use lockstat or other lock-hotness tools to
// check for this later.)

static nni_mtx  zt_lk;
static nni_list zt_nodes;

static void zt_ep_send_creq(zt_ep *);
static void zt_ep_creq_cb(void *);

static void
zt_bgthr(void *arg)
{
	zt_node *ztn = arg;
	nni_time now;

	nni_mtx_lock(&zt_lk);
	for (;;) {
		now = nni_clock(); // msecmsec

		if (ztn->zn_closed) {
			break;
		}

		if (now < ztn->zn_bgtime) {
			nni_cv_until(&ztn->zn_bgcv, ztn->zn_bgtime);
			continue;
		}

		now /= 1000; // usec -> msec
		ZT_Node_processBackgroundTasks(ztn->zn_znode, NULL, now, &now);

		ztn->zn_bgtime = now * 1000; // usec
	}
	nni_mtx_unlock(&zt_lk);
}

static void
zt_node_resched(zt_node *ztn, uint64_t msec)
{
	ztn->zn_bgtime = msec * 1000; // convert to usec
	nni_cv_wake1(&ztn->zn_bgcv);
}

static void
zt_node_rcv4_cb(void *arg)
{
	zt_node *               ztn = arg;
	nni_aio *               aio = ztn->zn_rcv4_aio;
	struct sockaddr_storage sa;
	struct sockaddr_in *    sin;
	nng_sockaddr_in *       nsin;
	uint64_t                now;
	char                    abuf[64];

	if (nni_aio_result(aio) != 0) {
		// Outside of memory exhaustion, we can't really think
		// of any reason for this to legitimately fail.
		// Arguably we should inject a fallback delay, but for
		// now we just carry on.
		// XXX: REVIEW THIS.  Its clearly wrong!  If the socket
		// is closed or fails in a permanent way, then we need
		// to stop the UDP work, and forward an error to all
		// the other endpoints and pipes!
		return;
	}

	memset(&sa, 0, sizeof(sa));
	sin                  = (void *) &sa;
	nsin                 = &ztn->zn_rcv4_addr.s_un.s_in;
	sin->sin_family      = AF_INET;
	sin->sin_port        = nsin->sa_port;
	sin->sin_addr.s_addr = nsin->sa_addr;
	inet_ntop(AF_INET, &sin->sin_addr, abuf, sizeof(abuf));
	printf("NODE RECVv4 CB called RCV FROM %s %u!\n", abuf,
	    htons(sin->sin_port));

	nni_mtx_lock(&zt_lk);
	now = nni_clock() / 1000; // msec

	// We are not going to perform any validation of the data; we
	// just pass this straight into the ZeroTier core.
	// XXX: CHECK THIS, if it fails then we have a fatal error with
	// the znode, and have to shut everything down.
	ZT_Node_processWirePacket(ztn->zn_znode, NULL, now, 0, (void *) &sa,
	    ztn->zn_rcv4_buf, aio->a_count, &now);

	// Schedule background work
	zt_node_resched(ztn, now);

	// Schedule another receive.
	if ((!ztn->zn_closed) && (ztn->zn_udp4 != NULL)) {
		aio->a_count = 0;
		nni_plat_udp_recv(ztn->zn_udp4, aio);
	}
	nni_mtx_unlock(&zt_lk);
}

static void
zt_node_rcv6_cb(void *arg)
{
	zt_node *                ztn = arg;
	nni_aio *                aio = ztn->zn_rcv6_aio;
	struct sockaddr_storage  sa;
	struct sockaddr_in6 *    sin6;
	struct nng_sockaddr_in6 *nsin6;
	uint64_t                 now;
	char                     abuf[64];

	if (nni_aio_result(aio) != 0) {
		// Outside of memory exhaustion, we can't really think
		// of any reason for this to legitimately fail.
		// Arguably we should inject a fallback delay, but for
		// now we just carry on.
		// XXX: REVIEW THIS.  Its clearly wrong!  If the socket
		// is closed or fails in a permanent way, then we need
		// to stop the UDP work, and forward an error to all
		// the other endpoints and pipes!
		return;
	}

	memset(&sa, 0, sizeof(sa));
	sin6              = (void *) &sa;
	nsin6             = &ztn->zn_rcv6_addr.s_un.s_in6;
	sin6->sin6_family = AF_INET6;
	sin6->sin6_port   = nsin6->sa_port;
	memcpy(&sin6->sin6_addr, nsin6->sa_addr, 16);
	inet_ntop(AF_INET6, &sin6->sin6_addr, abuf, sizeof(abuf));
	printf("NODE RECVv6 CB called RCV FROM %s %u!\n", abuf,
	    htons(sin6->sin6_port));

	nni_mtx_lock(&zt_lk);
	now = nni_clock() / 1000; // msec

	// We are not going to perform any validation of the data; we
	// just pass this straight into the ZeroTier core.
	// XXX: CHECK THIS, if it fails then we have a fatal error with
	// the znode, and have to shut everything down.
	ZT_Node_processWirePacket(ztn->zn_znode, NULL, now, 0, (void *) &sa,
	    ztn->zn_rcv6_buf, aio->a_count, &now);

	// Schedule background work
	zt_node_resched(ztn, now);

	// Schedule another receive.
	if ((!ztn->zn_closed) && (ztn->zn_udp6 != NULL)) {
		aio->a_count = 0;
		nni_plat_udp_recv(ztn->zn_udp6, aio);
	}
	nni_mtx_unlock(&zt_lk);
}

static uint64_t
zt_mac_to_node(uint64_t mac, uint64_t nwid)
{
	uint64_t node;
	// This extracts a node address from a mac addres.  The
	// network ID is mixed in, and has to be extricated.  We
	// the node ID is located in the lower 40 bits, and scrambled
	// against the nwid.
	node = mac & 0xffffffffffull;
	node ^= ((nwid >> 8) & 0xff) << 32;
	node ^= ((nwid >> 16) & 0xff) << 24;
	node ^= ((nwid >> 24) & 0xff) << 16;
	node ^= ((nwid >> 32) & 0xff) << 8;
	node ^= (nwid >> 40) & 0xff;
	return (node);
}

static uint64_t
zt_node_to_mac(uint64_t node, uint64_t nwid)
{
	uint64_t mac;
	// We use LSB of network ID, and make sure that we clear
	// multicast and set local administration -- this is the first
	// octet of the 48 bit mac address.  We also avoid 0x52, which
	// is known to be used in KVM, libvirt, etc.
	mac = ((uint8_t)(nwid & 0xfe) | 0x02);
	if (mac == 0x52) {
		mac = 0x32;
	}
	mac <<= 40;
	mac |= node;
	// The rest of the network ID is XOR'd in, in reverse byte
	// order.
	mac ^= ((nwid >> 8) & 0xff) << 32;
	mac ^= ((nwid >> 16) & 0xff) << 24;
	mac ^= ((nwid >> 24) & 0xff) << 16;
	mac ^= ((nwid >> 32) & 0xff) << 8;
	mac ^= (nwid >> 40) & 0xff;
	return (mac);
}

static int
zt_result(enum ZT_ResultCode rv)
{
	switch (rv) {
	case ZT_RESULT_OK:
		return (0);
	case ZT_RESULT_OK_IGNORED:
		return (0);
	case ZT_RESULT_FATAL_ERROR_OUT_OF_MEMORY:
		return (NNG_ENOMEM);
	case ZT_RESULT_FATAL_ERROR_DATA_STORE_FAILED:
		return (NNG_EPERM);
	case ZT_RESULT_FATAL_ERROR_INTERNAL:
		return (NNG_EINTERNAL);
	case ZT_RESULT_ERROR_NETWORK_NOT_FOUND:
		return (NNG_EADDRINVAL);
	case ZT_RESULT_ERROR_UNSUPPORTED_OPERATION:
		return (NNG_ENOTSUP);
	case ZT_RESULT_ERROR_BAD_PARAMETER:
		return (NNG_EINVAL);
	default:
		return (NNG_ETRANERR + (int) rv);
	}
}

// ZeroTier Node API callbacks
static int
zt_virtual_network_config(ZT_Node *node, void *userptr, void *thr,
    uint64_t nwid, void **netptr, enum ZT_VirtualNetworkConfigOperation op,
    const ZT_VirtualNetworkConfig *config)
{
	zt_node *ztn = userptr;
	zt_ep *  ep;

	NNI_ARG_UNUSED(node);
	NNI_ARG_UNUSED(thr);
	NNI_ARG_UNUSED(netptr);

	// Maybe we don't have to create taps or anything like that.
	// We do get our mac and MTUs from this, so there's that.
	switch (op) {
	case ZT_VIRTUAL_NETWORK_CONFIG_OPERATION_UP:
	case ZT_VIRTUAL_NETWORK_CONFIG_OPERATION_CONFIG_UPDATE:

		// We only really care about changes to the MTU.  From
		// an API perspective the MAC could change, but that
		// cannot really happen because the node identity and
		// the nwid are fixed.
		NNI_LIST_FOREACH (&ztn->zn_eplist, ep) {
			NNI_ASSERT(nwid == config->nwid);
			if (ep->ze_nwid != config->nwid) {
				continue;
			}
			ep->ze_maxmtu = config->mtu;
			ep->ze_phymtu = config->physicalMtu;
			NNI_ASSERT(ep->ze_mac == config->mac);

			if ((ep->ze_mode == NNI_EP_MODE_DIAL) &&
			    (nni_list_first(&ep->ze_aios) != NULL)) {
				printf("SENDING A CREQ because we are up!\n");
				zt_ep_send_creq(ep);
			}
			// if (ep->ze_mode == NNI_EP
			//	zt_send_
			//	nni_aio_finish(ep->ze_join_aio, 0);
			// }
			// XXX: schedule creqs if needed!
		}
		break;
	case ZT_VIRTUAL_NETWORK_CONFIG_OPERATION_DESTROY:
	case ZT_VIRTUAL_NETWORK_CONFIG_OPERATION_DOWN:
	// XXX: tear down endpoints?
	default:
		break;
	}
	return (0);
}

static void
zt_send_err(zt_node *ztn, uint64_t nwid, uint64_t dstnode, uint32_t srcport,
    uint32_t dstport, uint8_t err, char *msg)
{
	uint8_t  data[128];
	uint64_t srcmac = zt_node_to_mac(ztn->zn_self, nwid);
	uint64_t dstmac = zt_node_to_mac(dstnode, nwid);
	uint64_t now;

	NNI_ASSERT((strlen(msg) + NZT_OFFS_ERR_MSG) < sizeof(data));

	data[NZT_OFFS_OPFLAGS] = NZT_OP_ERR;
	data[NZT_OFFS_VERSION] = NZT_VERSION;
	NNI_PUT32(data + NZT_OFFS_DST_PORT, dstport);
	NNI_PUT32(data + NZT_OFFS_SRC_PORT, srcport);
	data[NZT_OFFS_ERR_CODE] = err;
	nni_strlcpy((char *) data + NZT_OFFS_ERR_MSG, msg,
	    sizeof(data) - NZT_OFFS_ERR_MSG);

	now = nni_clock() / 1000;
	ZT_Node_processVirtualNetworkFrame(ztn->zn_znode, NULL, now, nwid,
	    srcmac, dstmac, NZT_ETHER, 0, data, strlen(msg) + NZT_OFFS_ERR_MSG,
	    &now);

	zt_node_resched(ztn->zn_znode, now);
}

static void
zt_send_cack(zt_ep *ep, uint64_t rnode, uint32_t rport)
{
	uint8_t            data[NZT_OFFS_CON_PROT + sizeof(uint16_t)];
	uint64_t           srcmac = zt_node_to_mac(ep->ze_lnode, ep->ze_nwid);
	uint64_t           dstmac = zt_node_to_mac(rnode, ep->ze_nwid);
	ZT_Node *          znode  = ep->ze_ztn->zn_znode;
	uint64_t           now    = nni_clock();
	enum ZT_ResultCode zrv;

	printf("************ SENDING CACK TO %llx FROM %llx on %llx/\n", rnode,
	    ZT_Node_address(znode), ep->ze_nwid);

	data[NZT_OFFS_OPFLAGS] = NZT_OP_CON_ACK;
	data[NZT_OFFS_VERSION] = NZT_VERSION;
	NNI_PUT32(data + NZT_OFFS_DST_PORT, rport);
	NNI_PUT32(data + NZT_OFFS_SRC_PORT, ep->ze_lport);
	NNI_PUT16(data + NZT_OFFS_CON_PROT, ep->ze_proto);

	zrv = ZT_Node_processVirtualNetworkFrame(znode, NULL, now / 1000,
	    ep->ze_nwid, srcmac, dstmac, NZT_ETHER, 0, data,
	    NZT_OFFS_CON_PROT + sizeof(uint16_t), &now);

	zt_node_resched(ep->ze_ztn, now);
}

static void
zt_ep_send_creq(zt_ep *ep)
{
	uint8_t  data[NZT_OFFS_CON_PROT + sizeof(uint16_t)];
	uint64_t srcmac = zt_node_to_mac(ep->ze_lnode, ep->ze_nwid);
	uint64_t dstmac = zt_node_to_mac(ep->ze_rnode, ep->ze_nwid);
	uint64_t now    = nni_clock();
	ZT_Node *znode  = ep->ze_ztn->zn_znode;

	printf("==> SENDING CREQ TO %llx (%llx) FROM %llx (%llx) on %llx\n",
	    ep->ze_rnode, dstmac, ep->ze_lnode, srcmac, ep->ze_nwid);

	data[NZT_OFFS_OPFLAGS] = NZT_OP_CON_REQ;
	data[NZT_OFFS_VERSION] = NZT_VERSION;
	NNI_PUT32(data + NZT_OFFS_DST_PORT, ep->ze_rport);
	NNI_PUT32(data + NZT_OFFS_SRC_PORT, ep->ze_lport);
	NNI_PUT16(data + NZT_OFFS_CON_PROT, ep->ze_proto);

	(void) ZT_Node_processVirtualNetworkFrame(znode, NULL, now / 1000,
	    ep->ze_nwid, srcmac, dstmac, NZT_ETHER, 0, data,
	    NZT_OFFS_CON_PROT + sizeof(uint16_t), &now);

	zt_node_resched(ep->ze_ztn, now);
}

// This function is called when a frame arrives on the
// *virtual* network.
static void
zt_virtual_network_frame(ZT_Node *node, void *userptr, void *thr,
    uint64_t netid, void **netptr, uint64_t srcmac, uint64_t dstmac,
    unsigned int ethertype, unsigned int vlanid, const void *data,
    unsigned int len)
{
	zt_node *      ztn = userptr;
	uint8_t        opflags;
	const uint8_t *p = data;
	uint16_t       proto;
	uint32_t       srcport;
	uint32_t       dstport;
	uint64_t       srcconv;
	uint64_t       dstconv;
	uint64_t       srcnode;
	uint64_t       dstnode;

	dstnode = zt_mac_to_node(dstmac, netid);
	srcnode = zt_mac_to_node(srcmac, netid);

	printf("VIRTUAL NET FRAME RECVD\n");
	if (ethertype != NZT_ETHER) {
		// This is a weird frame we can't use, just
		// throw it away.
		printf("DEBUG: WRONG ETHERTYPE %x\n", ethertype);
		return;
	}

	if (len < (NZT_OFFS_SRC_PORT + 4)) {
		printf("DEBUG: RUNT len %d", len);
		return;
	}

	// XXX: arguably we should check the dstmac...
	// XXX: check frame type.

	opflags = p[NZT_OFFS_OPFLAGS];
	if (p[NZT_OFFS_VERSION] != NZT_VERSION) {
		// Wrong version, drop it.  (Log?)
		printf("DEBUG: BAD ZT_VERSION %2x", p[1]);
		return;
	}

	NNI_GET32(p + NZT_OFFS_DST_PORT, dstport);
	if ((dstport > NZT_MAX_PORT) || (dstport < 1)) {
		printf("DEBUG: INVALID destination port\n");
		return;
	}
	NNI_GET32(p + NZT_OFFS_SRC_PORT, srcport);
	if ((srcport > NZT_MAX_PORT) || (srcport < 1)) {
		printf("DEBUG: INVALID source port\n");
		return;
	}

	dstconv = (dstnode << 24) | dstport;
	srcconv = (srcnode << 24) | srcport;

	switch (opflags) {
	case NZT_OP_CON_REQ:
	// Look for a matching listener.  If one is found,
	// create a pipe, send an ack.
	case NZT_OP_CON_ACK:
	// Three cases:
	// 1. Matching waiting dialer.  In this case, create
	// the pipe,
	//    (ready for sending), and send an ack to the
	//    listener.
	// 2. Matching waiting listener.  In this case a pipe
	// should
	//    exist, so just mark it ready for sending.  (It
	//    could already receive.)  Arguably if we received
	//    a message, we wouldn't need the CON_ACK.
	// 3. No endpoint.  Send an error.
	case NZT_OP_PNG_REQ:
	// Look for matching pipe.  If none found, send an
	// error, otherwise send an ack.  Update timestamps.
	case NZT_OP_PNG_ACK:
	// Update timestamps, no further action.
	case NZT_OP_DIS:
	// Look for matching pipe.  If found, disconnect.
	// (Maybe disconnect pending connreq too?)
	case NZT_OP_DAT:
	case NZT_OP_DAT | NZT_FLAG_MF:
	case NZT_OP_ERR:
	default:
		printf("DEBUG: BAD ZT_OP %x", opflags);
	}

	switch (opflags & 0xf0) {
	case NZT_OP_CON:
		if (len < NZT_OFFS_CON_PROT + 2) {
			printf("DEBUG: Missing protocol number in CON");
			return;
		}
		NNI_GET16(p + NZT_OFFS_CON_PROT, proto);

		// Lets see if we have an endpoint..
		break;
	// XXX: incoming connection request.
	case NZT_OP_DIS:
	// XXX: look for a matching convo, and close it.
	case NZT_OP_ERR:
	// XXX: look for a matching convo, and fail it, or if
	// we have a connect request pending, fail that
	case NZT_OP_PNG:
	// XXX: look for a matching convo, and send a ping ack
	// (updating things).
	default:
		printf("DEBUG: BAD ZT_OP %x", opflags);
		return;
	}
}

static void
zt_event_cb(ZT_Node *node, void *userptr, void *thr, enum ZT_Event event,
    const void *payload)
{
	switch (event) {
	case ZT_EVENT_ONLINE:
		printf("EVENT ONLINE!\n");
		break;
	case ZT_EVENT_UP:
		printf("EVENT UP!\n");
		break;
	case ZT_EVENT_DOWN:
		printf("EVENT DOWN!\n");
		break;
	case ZT_EVENT_OFFLINE:
		printf("EVENT OFFLINE!\n");
		break;
	case ZT_EVENT_TRACE:
		printf("TRACE: %s\n", (const char *) payload);
		break;
	case ZT_EVENT_REMOTE_TRACE:
		printf("REMOTE TRACE\n");
		break;
	default:
		printf("OTHER EVENT %d\n", event);
		break;
	}
}

static const char *zt_files[] = {
	// clang-format off
	NULL, // none, i.e. not used at all
	"identity.public",
	"identity.secret",
	"planet",
	NULL, // moon, e.g. moons.d/<ID>.moon -- we don't persist it
	NULL, // peer, e.g. peers.d/<ID> -- we don't persist this
	NULL, // network, e.g. networks.d/<ID>.conf -- we don't persist
	// clang-format on
};

#ifdef _WIN32
#define unlink DeleteFile
#define pathsep "\\"
#else
#define pathsep "/"
#endif

static void
zt_state_put(ZT_Node *node, void *userptr, void *thr,
    enum ZT_StateObjectType objtype, const uint64_t objid[2], const void *data,
    int len)
{
	FILE *      file;
	zt_node *   ztn = userptr;
	char        path[NNG_MAXADDRLEN + 1];
	const char *fname;
	size_t      sz;

	NNI_ARG_UNUSED(objid); // only use global files

	if ((objtype > ZT_STATE_OBJECT_NETWORK_CONFIG) ||
	    ((fname = zt_files[(int) objtype]) == NULL)) {
		return;
	}

	sz = sizeof(path);
	if (snprintf(path, sz, "%s%s%s", ztn->zn_path, pathsep, fname) >= sz) {
		// If the path is too long, we can't cope.  We
		// just decline to store anything.
		return;
	}

	// We assume that everyone can do standard C I/O.
	// This may be a bad assumption.  If that's the case,
	// the platform should supply an alternative
	// implementation. We are also assuming that we don't
	// need to worry about atomic updates.  As these items
	// (keys, etc.)  pretty much don't change, this should
	// be fine.

	if (len < 0) {
		(void) unlink(path);
		return;
	}

	if ((file = fopen(path, "wb")) == NULL) {
		return;
	}

	if (fwrite(data, 1, len, file) != len) {
		fclose(file);
		(void) unlink(path);
	}
	(void) fclose(file);
}

static int
zt_state_get(ZT_Node *node, void *userptr, void *thr,
    enum ZT_StateObjectType objtype, const uint64_t objid[2], void *data,
    unsigned int len)
{
	FILE *      file;
	zt_node *   ztn = userptr;
	char        path[NNG_MAXADDRLEN + 1];
	const char *fname;
	int         nread;
	size_t      sz;

	NNI_ARG_UNUSED(objid); // we only use global files

	if ((objtype > ZT_STATE_OBJECT_NETWORK_CONFIG) ||
	    ((fname = zt_files[(int) objtype]) == NULL)) {
		return (-1);
	}

	sz = sizeof(path);
	if (snprintf(path, sz, "%s%s%s", ztn->zn_path, pathsep, fname) >= sz) {
		// If the path is too long, we can't cope.
		return (-1);
	}

	// We assume that everyone can do standard C I/O.
	// This may be a bad assumption.  If that's the case,
	// the platform should supply an alternative
	// implementation. We are also assuming that we don't
	// need to worry about atomic updates.  As these items
	// (keys, etc.)  pretty much don't change, this should
	// be fine.

	if ((file = fopen(path, "rb")) == NULL) {
		return (-1);
	}

	// seek to end of file
	(void) fseek(file, 0, SEEK_END);
	if (ftell(file) > len) {
		fclose(file);
		return (-1);
	}
	(void) fseek(file, 0, SEEK_SET);

	nread = (int) fread(data, 1, len, file);
	(void) fclose(file);

	return (nread);
}

typedef struct zt_send_hdr {
	nni_sockaddr sa;
	size_t       len;
} zt_send_hdr;

static void
zt_wire_packet_send_cb(void *arg)
{
	// We don't actually care much about the results, we
	// just need to release the resources.
	nni_aio *    aio = arg;
	zt_send_hdr *hdr;

	hdr = nni_aio_get_data(aio);
	nni_free(hdr, hdr->len + sizeof(*hdr));
	nni_aio_fini_cb(aio);
}

// This function is called when ZeroTier desires to send a
// physical frame. The data is a UDP payload, the rest of the
// payload should be set over vanilla UDP.
static int
zt_wire_packet_send(ZT_Node *node, void *userptr, void *thr, int64_t socket,
    const struct sockaddr_storage *remaddr, const void *data, unsigned int len,
    unsigned int ttl)
{
	nni_aio *            aio;
	nni_sockaddr         addr;
	struct sockaddr_in * sin  = (void *) remaddr;
	struct sockaddr_in6 *sin6 = (void *) remaddr;
	zt_node *            ztn  = userptr;
	nni_plat_udp *       udp;
	char                 abuf[64];
	uint16_t             port;
	char *               buf;
	zt_send_hdr *        hdr;

	NNI_ARG_UNUSED(thr);
	NNI_ARG_UNUSED(socket);
	NNI_ARG_UNUSED(ttl);

	// Kind of unfortunate, but we have to convert the
	// sockaddr to a neutral form, and then back again in
	// the platform layer.
	switch (sin->sin_family) {
	case AF_INET:
		addr.s_un.s_in.sa_family = NNG_AF_INET;
		addr.s_un.s_in.sa_port   = sin->sin_port;
		addr.s_un.s_in.sa_addr   = sin->sin_addr.s_addr;
		udp                      = ztn->zn_udp4;
		inet_ntop(AF_INET, &sin->sin_addr, abuf, sizeof(abuf));
		port = htons(sin->sin_port);
		break;
	case AF_INET6:
		addr.s_un.s_in6.sa_family = NNG_AF_INET6;
		addr.s_un.s_in6.sa_port   = sin6->sin6_port;
		udp                       = ztn->zn_udp6;
		memcpy(addr.s_un.s_in6.sa_addr, sin6->sin6_addr.s6_addr, 16);
		inet_ntop(AF_INET6, &sin6->sin6_addr, abuf, sizeof(abuf));
		port = htons(sin6->sin6_port);
		break;
	default:
		// No way to understand the address.
		return (-1);
	}

	if (nni_aio_init(&aio, zt_wire_packet_send_cb, NULL) != 0) {
		// Out of memory
		return (-1);
	}
	if ((buf = nni_alloc(sizeof(*hdr) + len)) == NULL) {
		nni_aio_fini(aio);
		return (-1);
	}

	hdr = (void *) buf;
	buf += sizeof(*hdr);

	memcpy(buf, data, len);
	nni_aio_set_data(aio, hdr);
	hdr->sa  = addr;
	hdr->len = len;

	aio->a_addr           = &hdr->sa;
	aio->a_niov           = 1;
	aio->a_iov[0].iov_buf = buf;
	aio->a_iov[0].iov_len = len;

	printf("SENDING UDP FRAME TO %s %d\n", abuf, port);
	// This should be non-blocking/best-effort, so while
	// not great that we're holding the lock, also not
	// tragic.
	nni_aio_set_synch(aio);
	nni_plat_udp_send(udp, aio);

	return (0);
}

static struct ZT_Node_Callbacks zt_callbacks = {
	.version                      = 0,
	.statePutFunction             = zt_state_put,
	.stateGetFunction             = zt_state_get,
	.wirePacketSendFunction       = zt_wire_packet_send,
	.virtualNetworkFrameFunction  = zt_virtual_network_frame,
	.virtualNetworkConfigFunction = zt_virtual_network_config,
	.eventCallback                = zt_event_cb,
	.pathCheckFunction            = NULL,
	.pathLookupFunction           = NULL,
};

static void
zt_node_destroy(zt_node *ztn)
{
	nni_aio_stop(ztn->zn_rcv4_aio);
	nni_aio_stop(ztn->zn_rcv6_aio);

	// Wait for background thread to exit!
	nni_thr_fini(&ztn->zn_bgthr);

	if (ztn->zn_znode != NULL) {
		ZT_Node_delete(ztn->zn_znode);
	}

	if (ztn->zn_udp4 != NULL) {
		nni_plat_udp_close(ztn->zn_udp4);
	}
	if (ztn->zn_udp6 != NULL) {
		nni_plat_udp_close(ztn->zn_udp6);
	}

	if (ztn->zn_rcv4_buf != NULL) {
		nni_free(ztn->zn_rcv4_buf, NZT_RCV_BUFSZ);
	}
	if (ztn->zn_rcv6_buf != NULL) {
		nni_free(ztn->zn_rcv6_buf, NZT_RCV_BUFSZ);
	}
	nni_aio_fini(ztn->zn_rcv4_aio);
	nni_aio_fini(ztn->zn_rcv6_aio);
	nni_idhash_fini(ztn->zn_eps);
	nni_idhash_fini(ztn->zn_pipes);
	nni_cv_fini(&ztn->zn_bgcv);
	NNI_FREE_STRUCT(ztn);
}

static int
zt_node_create(zt_node **ztnp, const char *path)
{
	zt_node *          ztn;
	nng_sockaddr       sa4;
	nng_sockaddr       sa6;
	int                rv;
	enum ZT_ResultCode zrv;

	// We want to bind to any address we can (for now).
	// Note that at the moment we only support IPv4.  Its
	// unclear how we are meant to handle underlying IPv6
	// in ZeroTier.  Probably we can use IPv6 dual stock
	// sockets if they exist, but not all platforms support
	// dual-stack.  Furhtermore, IPv6 is not available
	// everywhere, and the root servers may be IPv4 only.
	memset(&sa4, 0, sizeof(sa4));
	sa4.s_un.s_in.sa_family = NNG_AF_INET;
	memset(&sa6, 0, sizeof(sa6));
	sa6.s_un.s_in6.sa_family = NNG_AF_INET6;

	if ((ztn = NNI_ALLOC_STRUCT(ztn)) == NULL) {
		return (NNG_ENOMEM);
	}
	NNI_LIST_INIT(&ztn->zn_eplist, zt_ep, ze_link);
	NNI_LIST_INIT(&ztn->zn_plist, zt_pipe, zp_link);
	nni_cv_init(&ztn->zn_bgcv, &zt_lk);
	nni_aio_init(&ztn->zn_rcv4_aio, zt_node_rcv4_cb, ztn);
	nni_aio_init(&ztn->zn_rcv6_aio, zt_node_rcv6_cb, ztn);

	if (((ztn->zn_rcv4_buf = nni_alloc(NZT_RCV_BUFSZ)) == NULL) ||
	    ((ztn->zn_rcv6_buf = nni_alloc(NZT_RCV_BUFSZ)) == NULL)) {
		zt_node_destroy(ztn);
		return (NNG_ENOMEM);
	}
	if (((rv = nni_idhash_init(&ztn->zn_ports)) != 0) ||
	    ((rv = nni_idhash_init(&ztn->zn_eps)) != 0) ||
	    ((rv = nni_idhash_init(&ztn->zn_pipes)) != 0) ||
	    ((rv = nni_thr_init(&ztn->zn_bgthr, zt_bgthr, ztn)) != 0) ||
	    ((rv = nni_plat_udp_open(&ztn->zn_udp4, &sa4)) != 0) ||
	    ((rv = nni_plat_udp_open(&ztn->zn_udp6, &sa6)) != 0)) {
		zt_node_destroy(ztn);
		return (rv);
	}

	// Setup for dynamic ephemeral port allocations.  We
	// set the range to allow for ephemeral ports, but not
	// higher than the max port, and starting with an
	// initial random value.  Note that this should give us
	// about 8 million possible ephemeral ports.
	nni_idhash_set_limits(ztn->zn_ports, NZT_EPHEMERAL, NZT_MAX_PORT,
	    (nni_random() % (NZT_MAX_PORT - NZT_EPHEMERAL)) + NZT_EPHEMERAL);

	(void) snprintf(ztn->zn_path, sizeof(ztn->zn_path), "%s", path);
	zrv = ZT_Node_new(
	    &ztn->zn_znode, ztn, NULL, &zt_callbacks, nni_clock() / 1000);
	if (zrv != ZT_RESULT_OK) {
		zt_node_destroy(ztn);
		return (zt_result(zrv));
	}

	ztn->zn_self = ZT_Node_address(ztn->zn_znode);
	printf("MY NODE ADDRESS IS %llx\n", ztn->zn_self);

	nni_thr_run(&ztn->zn_bgthr);

	// Schedule an initial background run.
	zt_node_resched(ztn, 1);

	// Schedule receive
	ztn->zn_rcv4_aio->a_niov           = 1;
	ztn->zn_rcv4_aio->a_iov[0].iov_buf = ztn->zn_rcv4_buf;
	ztn->zn_rcv4_aio->a_iov[0].iov_len = NZT_RCV_BUFSZ;
	ztn->zn_rcv4_aio->a_addr           = &ztn->zn_rcv4_addr;
	ztn->zn_rcv4_aio->a_count          = 0;
	ztn->zn_rcv6_aio->a_niov           = 1;
	ztn->zn_rcv6_aio->a_iov[0].iov_buf = ztn->zn_rcv6_buf;
	ztn->zn_rcv6_aio->a_iov[0].iov_len = NZT_RCV_BUFSZ;
	ztn->zn_rcv6_aio->a_addr           = &ztn->zn_rcv6_addr;
	ztn->zn_rcv6_aio->a_count          = 0;

	nni_plat_udp_recv(ztn->zn_udp4, ztn->zn_rcv4_aio);
	nni_plat_udp_recv(ztn->zn_udp6, ztn->zn_rcv6_aio);

	printf("LOOKING GOOD?\n");
	*ztnp = ztn;
	return (0);
}

static int
zt_node_find(zt_ep *ep)
{
	zt_node *                ztn;
	int                      rv;
	nng_sockaddr             sa;
	ZT_VirtualNetworkConfig *cf;

	NNI_LIST_FOREACH (&zt_nodes, ztn) {
		if (strcmp(ep->ze_home, ztn->zn_path) == 0) {
			goto done;
		}
		if (ztn->zn_closed) {
			return (NNG_ECLOSED);
		}
	}

	// We didn't find a node, so make one.  And try to
	// initialize it.
	if ((rv = zt_node_create(&ztn, ep->ze_home)) != 0) {
		return (rv);
	}

done:

	ep->ze_ztn   = ztn;
	ep->ze_lnode = ztn->zn_self;
	ep->ze_mac   = zt_node_to_mac(ep->ze_lnode, ep->ze_nwid);
	nni_list_append(&ztn->zn_eplist, ep);
	nni_list_append(&zt_nodes, ztn);

	(void) ZT_Node_join(ztn->zn_znode, ep->ze_nwid, ztn, NULL);

	if ((cf = ZT_Node_networkConfig(ztn->zn_znode, ep->ze_nwid)) != NULL) {
		NNI_ASSERT(cf->nwid == ep->ze_nwid);
		ep->ze_maxmtu = cf->mtu;
		ep->ze_phymtu = cf->physicalMtu;
		NNI_ASSERT(ep->ze_mac == cf->mac);
		ZT_Node_freeQueryResult(ztn->zn_znode, cf);
	}

	return (0);
}

static int
zt_chkopt(int opt, const void *dat, size_t sz)
{
	if (opt == nng_optid_recvmaxsz) {
		// We cannot deal with message sizes larger
		// than 64k.
		return (nni_chkopt_size(dat, sz, 0, 0xffffffffU));
	}
	if (opt == nng_optid_zt_home) {
		size_t l = nni_strnlen(dat, sz);
		if ((l >= sz) || (l >= NNG_MAXADDRLEN)) {
			return (NNG_EINVAL);
		}
		// XXX: should we apply additional security
		// checks? home path is not null terminated
		return (0);
	}
	return (NNG_ENOTSUP);
}

static int
zt_tran_init(void)
{
	int rv;
	if (((rv = nni_option_register(nng_opt_zt_home, &nng_optid_zt_home)) !=
	        0) ||
	    ((rv = nni_option_register(nng_opt_zt_nwid, &nng_optid_zt_nwid)) !=
	        0)) {
		return (rv);
	}
	nni_mtx_init(&zt_lk);
	NNI_LIST_INIT(&zt_nodes, zt_node, zn_link);
	return (0);
}

static void
zt_tran_fini(void)
{
	nng_optid_zt_home = -1;
	nng_optid_zt_nwid = -1;
	zt_node *ztn;

	nni_mtx_lock(&zt_lk);
	while ((ztn = nni_list_first(&zt_nodes)) != 0) {
		nni_list_remove(&zt_nodes, ztn);
		nni_mtx_unlock(&zt_lk);

		zt_node_destroy(ztn);
	}
	nni_mtx_unlock(&zt_lk);

	NNI_ASSERT(nni_list_empty(&zt_nodes));
	nni_mtx_fini(&zt_lk);
}

static void
zt_pipe_close(void *arg)
{
	// This can start the tear down of the connection.
	// It should send an asynchronous DISC message to let
	// the peer know we are shutting down.
}

static void
zt_pipe_fini(void *arg)
{
	zt_pipe *p = arg;
	uint64_t port;
	zt_node *ztn = p->zp_ztn;

	nni_aio_stop(p->zp_rxaio);
	nni_aio_stop(p->zp_txaio);
	nni_aio_stop(p->zp_negaio);

	port = p->zp_src_port;
	if (port != 0) {
		// This tosses the connection details and all state.
		nni_mtx_lock(&zt_lk);
		nni_idhash_remove(ztn->zn_pipes, port);
		nni_idhash_remove(ztn->zn_ports, port);
		nni_mtx_unlock(&zt_lk);
	}
	NNI_FREE_STRUCT(p);
}

static void
zt_pipe_send_cb(void *arg)
{
}

static void
zt_pipe_recv_cb(void *arg)
{
}

static void
zt_pipe_nego_cb(void *arg)
{
}

static int
zt_pipe_init(zt_pipe **pipep, zt_ep *ep, uint64_t rnode, uint32_t rport)
{
	zt_pipe *p;
	uint64_t port = 0;
	int      rv;
	zt_node *ztn = ep->ze_ztn;

	if ((p = NNI_ALLOC_STRUCT(p)) == NULL) {
		return (NNG_ENOMEM);
	}
	p->zp_ztn      = ztn;
	p->zp_dst_port = rport;
	p->zp_dst_node = rnode;
	p->zp_proto    = ep->ze_proto;
	p->zp_src_node = ep->ze_lnode;

	if (((rv = nni_aio_init(&p->zp_txaio, zt_pipe_send_cb, p)) != 0) ||
	    ((rv = nni_aio_init(&p->zp_rxaio, zt_pipe_recv_cb, p)) != 0) ||
	    ((rv = nni_aio_init(&p->zp_negaio, zt_pipe_nego_cb, p)) != 0) ||
	    ((rv = nni_idhash_alloc(ztn->zn_ports, &port, p)) != 0) ||
	    ((rv = nni_idhash_insert(ztn->zn_pipes, port, p)) != 0)) {
		p->zp_src_port = (uint32_t) port;
		zt_pipe_fini(p);
	}
	p->zp_src_port = (uint32_t) port;

	*pipep = p;
	return (0);
}

static void
zt_pipe_send(void *arg, nni_aio *aio)
{
	nni_aio_finish(aio, NNG_ENOTSUP, 0);
}

static void
zt_pipe_recv(void *arg, nni_aio *aio)
{
	nni_aio_finish(aio, NNG_ENOTSUP, 0);
}

static uint16_t
zt_pipe_peer(void *arg)
{
	zt_pipe *pipe = arg;

	return (pipe->zp_peer);
}

static int
zt_pipe_getopt(void *arg, int option, void *buf, size_t *szp)
{
	return (NNG_ENOTSUP);
}

static void
zt_pipe_start(void *arg, nni_aio *aio)
{
	nni_aio_finish(aio, NNG_ENOTSUP, 0);
}

static void
zt_ep_fini(void *arg)
{
	zt_ep *ep = arg;
	nni_aio_stop(ep->ze_creq_aio);
	nni_aio_fini(ep->ze_creq_aio);
	NNI_FREE_STRUCT(ep);
}

static int
zt_parsehex(const char **sp, uint64_t *valp)
{
	int         n;
	const char *s = *sp;
	char        c;
	uint64_t    v;

	for (v = 0, n = 0; (n < 16) && isxdigit(c = tolower(*s)); n++, s++) {
		v *= 16;
		if (isdigit(c)) {
			v += (c - '0');
		} else {
			v += ((c - 'a') + 10);
		}
	}

	*sp   = s;
	*valp = v;
	return (n ? 0 : NNG_EINVAL);
}

static int
zt_parsedec(const char **sp, uint64_t *valp)
{
	int         n;
	const char *s = *sp;
	char        c;
	uint64_t    v;

	for (v = 0, n = 0; (n < 20) && isdigit(c = *s); n++, s++) {
		v *= 10;
		v += (c - '0');
	}
	*sp   = s;
	*valp = v;
	return (n ? 0 : NNG_EINVAL);
}

static int
zt_ep_init(void **epp, const char *url, nni_sock *sock, int mode)
{
	zt_ep *     ep;
	size_t      sz;
	uint64_t    nwid;
	uint64_t    node;
	uint64_t    port;
	int         n;
	int         rv;
	char        c;
	const char *u;

	if ((ep = NNI_ALLOC_STRUCT(ep)) == NULL) {
		return (NNG_ENOMEM);
	}
	// URL parsing...
	// URL is form zt://<nwid>[/<remoteaddr>]:<port>
	// The <remoteaddr> part is required for remote
	// dialers, but is not used at all for listeners.  (We
	// have no notion of binding to different node
	// addresses.)
	ep->ze_mode   = mode;
	ep->ze_maxmtu = ZT_MAX_MTU;
	ep->ze_phymtu = ZT_MIN_MTU;
	ep->ze_aio    = NULL;
	sz            = sizeof(ep->ze_url);

	nni_aio_list_init(&ep->ze_aios);

	if ((strncmp(url, "zt://", strlen("zt://")) != 0) ||
	    (nni_strlcpy(ep->ze_url, url, sz) >= sz)) {
		zt_ep_fini(ep);
		return (NNG_EADDRINVAL);
	}
	if ((rv = nni_aio_init(&ep->ze_creq_aio, zt_ep_creq_cb, ep)) != 0) {
		zt_ep_fini(ep);
		return (rv);
	}

	*epp = ep;
	u    = url + strlen("zt://");
	// Parse the URL.

	switch (mode) {
	case NNI_EP_MODE_DIAL:
		// We require zt://<nwid>/<remotenode>:<port>
		// The remote node must be a 40 bit address
		// (max), and we require a non-zero port to
		// connect to.
		if ((zt_parsehex(&u, &nwid) != 0) || (*u++ != '/') ||
		    (zt_parsehex(&u, &node) != 0) ||
		    (node > 0xffffffffffull) || (*u++ != ':') ||
		    (zt_parsedec(&u, &port) != 0) || (*u != '\0') ||
		    (port > NZT_MAX_PORT) || (port == 0)) {
			return (NNG_EADDRINVAL);
		}
		ep->ze_lport = 0;
		ep->ze_rport = (uint32_t) port;
		ep->ze_rnode = node;
		break;
	case NNI_EP_MODE_LISTEN:
		// Listen mode is just zt://<nwid>:<port>.  The
		// port may be zero in this case, to indicate
		// that the server should allocate an ephemeral
		// port.
		if ((zt_parsehex(&u, &nwid) != 0) || (*u++ != ':') ||
		    (zt_parsedec(&u, &port) != 0) || (*u != '\0') ||
		    (port > NZT_MAX_PORT)) {
			return (NNG_EADDRINVAL);
		}
		node         = 0;
		ep->ze_rport = 0;
		ep->ze_lport = (uint32_t) port;
		break;
	default:
		NNI_ASSERT(0);
		break;
	}

	ep->ze_nwid = nwid;

	return (0);
}

static void
zt_ep_close(void *arg)
{
	zt_ep *  ep = arg;
	zt_node *ztn;
	nni_aio *aio;

	nni_aio_cancel(ep->ze_creq_aio, NNG_ECLOSED);

	// Cancel any outstanding user operation(s) - they should have
	// been aborted by the above cancellation, but we need to be sure,
	// as the cancellation callback may not have run yet.

	nni_mtx_lock(&zt_lk);
	while ((aio = nni_list_first(&ep->ze_aios)) != NULL) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}

	// Endpoint framework guarantees to only call us once,
	// and to not call other things while we are closed.
	ztn = ep->ze_ztn;
	// If we're on the ztn node list, pull us off.
	if (ztn != NULL) {
		nni_list_node_remove(&ep->ze_link);
		nni_idhash_remove(ztn->zn_eps, ep->ze_lport);
		nni_idhash_remove(ztn->zn_ports, ep->ze_lport);
	}

	nni_mtx_unlock(&zt_lk);
}

static int
zt_ep_bind(void *arg)
{
	int      rv;
	zt_ep *  ep = arg;
	uint64_t port;
	zt_node *ztn;

	nni_mtx_lock(&zt_lk);
	if ((rv = zt_node_find(ep)) != 0) {
		nni_mtx_unlock(&zt_lk);
		return (rv);
	}

	ztn = ep->ze_ztn;

	if (ep->ze_lport == 0) {
		// ask for an ephemeral port
		if (((rv = nni_idhash_alloc(ztn->zn_ports, &port, ep)) != 0) ||
		    ((rv = nni_idhash_insert(ztn->zn_eps, port, ep)) != 0)) {
			nni_idhash_remove(ztn->zn_ports, port);
			nni_idhash_remove(ztn->zn_eps, port);
			nni_mtx_unlock(&zt_lk);
			return (rv);
		}
		NNI_ASSERT(port < NZT_MAX_PORT);
		NNI_ASSERT(port & NZT_EPHEMERAL);
		ep->ze_lport = (uint32_t) port;
	} else {
		void *conflict;
		port = ep->ze_lport;
		// Make sure our port is not already in use.
		if (nni_idhash_find(ztn->zn_ports, port, &conflict) == 0) {
			nni_mtx_unlock(&zt_lk);
			return (NNG_EADDRINUSE);
		}

		// we have a specific port to use
		if (((rv = nni_idhash_insert(ztn->zn_ports, port, ep)) != 0) ||
		    ((rv = nni_idhash_insert(ztn->zn_eps, port, ep)) != 0)) {
			nni_idhash_remove(ztn->zn_ports, port);
			nni_idhash_remove(ztn->zn_eps, port);
			nni_mtx_unlock(&zt_lk);
			return (rv);
		}
	}
	if (rv != 0) {
		nni_mtx_unlock(&zt_lk);
		return (rv);
	}

	nni_mtx_unlock(&zt_lk);

	return (0);
}

static void
zt_ep_cancel(nni_aio *aio, int rv)
{
	zt_ep *ep = aio->a_prov_data;

	nni_mtx_lock(&zt_lk);
	if (nni_aio_list_active(aio)) {
		if (ep->ze_aio != NULL) {
			nni_aio_cancel(ep->ze_aio, rv);
		}
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&zt_lk);
}

static void
zt_ep_doaccept(zt_ep *ep)
{
	// Call with ep lock held.
	nni_time now;

	now = nni_clock();
	// Consume any timedout connect requests.
	while (ep->ze_creq_tail != ep->ze_creq_head) {
		zt_creq  creq;
		nni_aio *aio;

		creq = ep->ze_creqs[ep->ze_creq_tail % NZT_LISTENQ];
		// Discard old connection requests.
		if (creq.cr_time < now) {
			ep->ze_creq_tail++;
			continue;
		}

		if ((aio = nni_list_first(&ep->ze_aios)) == NULL) {
			// No outstanding accept.  We're done.
			break;
		}

		// We have both a connection request, and a
		// place to accept it.

		// Advance the tail.
		ep->ze_creq_tail++;

		// We remove this AIO.  This keeps it from
		// being canceled.
		nni_aio_list_remove(aio);

		// Now we need to create a pipe, send the
		// notice to the user, and finish the request.
		// For now we are pretty lame and just return
		// NNG_EINTERNAL.

		nni_aio_finish_error(aio, NNG_EINTERNAL);
	}
}

static void
zt_ep_accept(void *arg, nni_aio *aio)
{
	zt_ep *ep = arg;

	nni_mtx_lock(&zt_lk);
	if (nni_aio_start(aio, zt_ep_cancel, ep) == 0) {
		nni_aio_list_append(&ep->ze_aios, aio);
		zt_ep_doaccept(ep);
	}
	nni_mtx_unlock(&zt_lk);
}

static void
zt_ep_creq_cancel(nni_aio *aio, int rv)
{
	// We don't have much to do here.  The AIO will have been
	// canceled as a result of the "parent" AIO canceling.
	zt_ep *ep = aio->a_prov_data;
	nni_aio_finish_error(aio, rv);
}

static void
zt_ep_creq_cb(void *arg)
{
	zt_ep *  ep = arg;
	zt_pipe *p;
	nni_aio *aio = ep->ze_creq_aio;
	nni_aio *uaio;
	int      rv;

	NNI_ASSERT(ep->ze_mode == NNI_EP_MODE_DIAL);

	rv = nni_aio_result(aio);
	switch (rv) {
	case 0:
		// Good connect. Create a pipe, send the cack.
		nni_mtx_lock(&zt_lk);
		// Already canceled?
		if ((uaio = nni_list_first(&ep->ze_aios)) == NULL) {
			nni_mtx_unlock(&zt_lk);
			return;
		}
		nni_aio_list_remove(uaio);
		rv = zt_pipe_init(&p, ep, ep->ze_rnode, ep->ze_rport);
		if (rv != 0) {
			nni_mtx_unlock(&zt_lk);
			nni_aio_finish_error(uaio, rv);
			return;
		}
		nni_aio_finish_pipe(uaio, p);
		nni_mtx_unlock(&zt_lk);
		break;
	case NNG_ETIMEDOUT:
		nni_mtx_lock(&zt_lk);
		if (ep->ze_creq_try > NZT_CONN_ATTEMPTS) {
			// We need to give up.
			while ((uaio = nni_list_first(&ep->ze_aios)) != NULL) {
				nni_aio_list_remove(uaio);
				nni_aio_finish_error(uaio, NNG_ETIMEDOUT);
			}
			nni_mtx_unlock(&zt_lk);
			return;
		}

		// Timed out, try again. (We have more attempts.)
		ep->ze_creq_try++;
		nni_aio_set_timeout(aio, nni_clock() + NZT_CONN_INTERVAL);
		nni_aio_start(aio, zt_ep_creq_cancel, ep);
		zt_ep_send_creq(ep);
		nni_mtx_unlock(&zt_lk);
		break;
	default:
		nni_mtx_lock(&zt_lk);
		if ((uaio = nni_list_first(&ep->ze_aios)) != NULL) {
			nni_aio_list_remove(uaio);
			nni_aio_finish_error(uaio, rv);
		}
		nni_mtx_unlock(&zt_lk);
		break;
	}
}

static void
zt_ep_connect(void *arg, nni_aio *aio)
{
	zt_ep *  ep = arg;
	uint64_t port;
	zt_pipe *p;
	int      rv;

	nni_mtx_lock(&zt_lk);
	if ((rv = zt_node_find(ep)) != 0) {
		nni_mtx_unlock(&zt_lk);
		nni_aio_finish_error(aio, rv);
		return;
	}

	if (nni_aio_start(aio, zt_ep_cancel, ep) == 0) {
		nni_time now = nni_clock();

		nni_aio_list_append(&ep->ze_aios, aio);

		ep->ze_creq_try = 1;

		nni_aio_set_timeout(ep->ze_creq_aio, now + NZT_CONN_INTERVAL);
		// This can't fail -- the only way the ze_creq_aio gets
		// terminated would have required us to have also canceled
		// the user AIO and held the lock.
		(void) nni_aio_start(ep->ze_creq_aio, zt_ep_creq_cancel, ep);

		// We send out the first connect message; it we are not
		// yet connected the message will be dropped.
		zt_ep_send_creq(ep);
	}
	nni_mtx_unlock(&zt_lk);
}

static int
zt_ep_setopt(void *arg, int opt, const void *data, size_t size)
{
	zt_ep *ep = arg;
	int    i;
	int    rv = NNG_ENOTSUP;

	if (opt == nng_optid_recvmaxsz) {
		nni_mtx_lock(&zt_lk);
		rv = nni_setopt_size(
		    &ep->ze_rcvmax, data, size, 0, 0xffffffffu);
		nni_mtx_unlock(&zt_lk);
	} else if (opt == nng_optid_zt_home) {
		// XXX: check to make sure not started...
		i = nni_strnlen((const char *) data, size);
		if ((i >= size) || (i >= NNG_MAXADDRLEN)) {
			return (NNG_EINVAL);
		}
		nni_mtx_lock(&zt_lk);
		(void) snprintf(ep->ze_home, sizeof(ep->ze_home), "%s", data);
		nni_mtx_unlock(&zt_lk);
		rv = 0;
	}
	return (rv);
}

static int
zt_ep_getopt(void *arg, int opt, void *data, size_t *sizep)
{
	zt_ep *ep = arg;
	int    rv = NNG_ENOTSUP;

	if (opt == nng_optid_recvmaxsz) {
		nni_mtx_lock(&zt_lk);
		rv = nni_getopt_size(&ep->ze_rcvmax, data, sizep);
		nni_mtx_unlock(&zt_lk);
	} else if (opt == nng_optid_zt_home) {
		nni_mtx_lock(&zt_lk);
		rv = nni_getopt_str(ep->ze_home, data, sizep);
		nni_mtx_unlock(&zt_lk);
	} else if (opt == nng_optid_zt_nwid) {
		nni_mtx_lock(&zt_lk);
		rv = nni_getopt_u64(ep->ze_nwid, data, sizep);
		nni_mtx_unlock(&zt_lk);
	}
	return (rv);
}

static nni_tran_pipe zt_pipe_ops = {
	.p_fini   = zt_pipe_fini,
	.p_start  = zt_pipe_start,
	.p_send   = zt_pipe_send,
	.p_recv   = zt_pipe_recv,
	.p_close  = zt_pipe_close,
	.p_peer   = zt_pipe_peer,
	.p_getopt = zt_pipe_getopt,
};

static nni_tran_ep zt_ep_ops = {
	.ep_init    = zt_ep_init,
	.ep_fini    = zt_ep_fini,
	.ep_connect = zt_ep_connect,
	.ep_bind    = zt_ep_bind,
	.ep_accept  = zt_ep_accept,
	.ep_close   = zt_ep_close,
	.ep_setopt  = zt_ep_setopt,
	.ep_getopt  = zt_ep_getopt,
};

// This is the ZeroTier transport linkage, and should be the
// only global symbol in this entire file.
static struct nni_tran zt_tran = {
	.tran_version = NNI_TRANSPORT_VERSION,
	.tran_scheme  = "zt",
	.tran_ep      = &zt_ep_ops,
	.tran_pipe    = &zt_pipe_ops,
	.tran_chkopt  = zt_chkopt,
	.tran_init    = zt_tran_init,
	.tran_fini    = zt_tran_fini,
};

int
nng_zt_register(void)
{
	return (nni_tran_register(&zt_tran));
}

#endif
