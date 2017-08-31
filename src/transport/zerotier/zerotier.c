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
// The ZeroTier transport was funded by Capitar IT Group, BV.
//
// This transport is highly experimental.

// ZeroTier and UDP are connectionless, but nng is designed around
// connection oriented paradigms.  Therefore we will emulate a connection
// as follows:
//
// xxx...
//
typedef struct nni_zt_pipe nni_zt_pipe;
typedef struct nni_zt_ep   nni_zt_ep;
typedef struct nni_zt_node nni_zt_node;

#define NNI_ZT_ETHER 0x0901 // We use this ethertype

// opcodes.  As these are encoded with the 4 bit flags field, we just
// shift them up one nibble.  The flags will be set in the lower nibble.
#define NNI_ZT_OP_DAT 0x00 // Data message
#define NNI_ZT_OP_CON 0x10 // Connection request
#define NNI_ZT_OP_DIS 0x20 // Disconnect request
#define NNI_ZT_OP_PNG 0x30 // Ping (keep alive)
#define NNI_ZT_OP_ERR 0x40 // Error

#define NNI_ZT_FLAG_MF 0x01 // more fragments
#define NNI_ZT_FLAG_AK 0x02 // acknowledgement

#define NNI_ZT_OP_CON_REQ (NNI_ZT_OP_CON)
#define NNI_ZT_OP_CON_ACK (NNI_ZT_OP_CON | NNI_ZT_FLAG_AK)
#define NNI_ZT_OP_PNG_REQ (NNI_ZT_OP_PNG)
#define NNI_ZT_OP_PNG_ACK (NNI_ZT_OP_PNG | NNI_ZT_FLAG_AK)

#define NNI_ZT_VERSION 0x01 // specified per RFC

#define NNI_ZT_OFFS_OPFLAGS 0
#define NNI_ZT_OFFS_VERSION 1
#define NNI_ZT_OFFS_DST_PORT 2 // includes reserved high order bits
#define NNI_ZT_OFFS_SRC_PORT 6 // includes reserved high order bits

#define NNI_ZT_OFFS_CON_PROT 10
#define NNI_ZT_SIZE_CON 12

#define NNI_ZT_OFFS_DAT_MSGID 10
#define NNI_ZT_OFFS_DAT_FRLEN 12
#define NNI_ZT_OFFS_DAT_FROFF 14
#define NNI_ZT_OFFS_DAT_DATA 16
#define NNI_ZT_SIZE_DAT 16 // does not include the user data

#define NNI_ZT_OFFS_ERR_CODE 10
#define NNI_ZT_OFFS_ERR_MSG 11

#define NNI_ZT_EREFUSED 0x01 // Nothing there, connection refused
#define NNI_ZT_ENOTCONN 0x02 // Connection does not exist
#define NNI_ZT_EWRONGSP 0x03 // Wrong SP number
#define NNI_ZT_EPROTERR 0x04 // Other protocol error
#define NNI_ZT_EMSGSIZE 0x05 // Message too large
#define NNI_ZT_EUNKNOWN 0xff // Other errors

// Ephemeral ports are those with the high order bit set.  There are
// about 8 million ephemeral ports, and about 8 million static ports.
// We restrict ourselves to just 24 bit port numbers.  This lets us
// construct 64-bit conversation IDs by combining the 24-bit port
// number with the 40-bit node address.  This means that 64-bits can
// be used to uniquely identify any address.
#define NNI_ZT_EPHEMERAL (1U << 23)
#define NNI_ZT_MAX_PORT ((1U << 24) - 1)

// Connection timeout maximum.  Basically we expect that a remote host
// will respond within this many usecs to a connection request.  Note
// that on Linux TCP connection timeouts are about 20s, and on othe systems
// they are about 72s.  This seems rather ridiculously long.  Modern
// Internet latencies generally never exceed 500ms, and processes should
// not be MIA for more than a few hundred ms as well.

// Connection retry and timeouts.  We send a connection attempt up to
// five times, before giving up and reporting to the user.  Each attempt
// is separated from the previous by one second.
#define NNI_ZT_CONN_ATTEMPTS (5)
#define NNI_ZT_CONN_INTERVAL (1000000)
#define NNI_ZT_CONN_MAXTIME ((NNI_ZT_CONN_ATTEMPTS + 1) * NNI_ZT_CONN_INTERVAL)

// In theory UDP can send/recv 655507, but ZeroTier won't do more
// than the ZT_MAX_MTU for it's virtual networks  So we need to add some
// extra space for ZT overhead, which is currently 52 bytes, but we want
// to leave room for future growth; 128 bytes seems sufficient.  The vast
// majority of frames will be far far smaller -- typically Ethernet MTUs
// are 1500 bytes.
#define NNI_ZT_MAX_HEADROOM 128
#define NNI_ZT_RCV_BUFSZ (ZT_MAX_MTU + NNI_ZT_MAX_HEADROOM)

// This node structure is wrapped around the ZT_node; this allows us to
// have multiple endpoints referencing the same ZT_node, but also to
// support different nodes (identities) based on different homedirs.
// This means we need to stick these on a global linked list, manage
// them with a reference count, and uniquely identify them using the
// homedir.

struct nni_zt_node {
	char          zn_path[NNG_MAXADDRLEN]; // ought to be sufficient
	ZT_Node *     zn_znode;
	uint64_t      zn_self;
	nni_list_node zn_link;
	int           zn_refcnt;
	int           zn_closed;
	nni_plat_udp *zn_udp4;
	nni_plat_udp *zn_udp6;
	nni_list      zn_eplist;
	nni_list      zn_plist;
	nni_idhash *  zn_eps;
	nni_idhash *  zn_pipes;
	nni_aio       zn_rcv4_aio;
	char *        zn_rcv4_buf;
	nng_sockaddr  zn_rcv4_addr;
	nni_aio       zn_rcv6_aio;
	char *        zn_rcv6_buf;
	nng_sockaddr  zn_rcv6_addr;
	nni_thr       zn_bgthr;
	nni_time      zn_bgtime;
	nni_cv        zn_bgcv;
};

struct nni_zt_pipe {
	nni_list_node zp_link;
	const char *  zp_addr;
	uint16_t      zp_peer;
	uint16_t      zp_proto;
	size_t        zp_rcvmax;
	nni_aio *     zp_user_txaio;
	nni_aio *     zp_user_rxaio;
	uint32_t      zp_src_port;
	uint32_t      zp_dst_port;
	uint64_t      zp_self_conv;
	uint64_t      zp_peer_conv;

	// XXX: fraglist
	nni_sockaddr zp_remaddr;
	nni_sockaddr zp_locaddr;

	nni_aio  zp_txaio;
	nni_aio  zp_rxaio;
	nni_msg *zp_rxmsg;
};

typedef struct nni_zt_creq nni_zt_creq;
struct nni_zt_creq {
	uint64_t cr_time;
	uint64_t cr_peer_addr;
	uint32_t cr_peer_port;
	uint16_t cr_proto;
};

#define NNI_ZT_LISTENQ 128
#define NNI_ZT_LISTEN_EXPIRE 60 // seconds before we give up in the backlog

struct nni_zt_ep {
	nni_list_node ze_link;
	char          ze_url[NNG_MAXADDRLEN];
	char          ze_home[NNG_MAXADDRLEN]; // should be enough
	nni_zt_node * ze_ztnode;
	uint64_t      ze_nwid;
	uint64_t      ze_self;
	uint64_t      ze_mac;  // our own mac address
	uint64_t      ze_node; // remote node address normally
	int           ze_mode;
	nni_sockaddr  ze_addr;
	uint32_t      ze_rport;
	uint32_t      ze_lport;
	uint16_t      ze_proto;
	size_t        ze_rcvmax;
	nni_aio       ze_aio;
	nni_list      ze_aios;
	int           ze_maxmtu;
	int           ze_phymtu;

	// Incoming connection requests (server only).  We only
	// only have "accepted" requests -- that is we won't have an
	// established connection/pipe unless the application calls
	// accept.  Since the "application" is our library, that should
	// be pretty much as fast we can run.
	nni_zt_creq ze_creqs[NNI_ZT_LISTENQ];
	int         ze_creq_head;
	int         ze_creq_tail;
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

static nni_mtx  nni_zt_lk;
static nni_list nni_zt_nodes;

static void nni_zt_node_rele(nni_zt_node *);

static void
nni_zt_bgthr(void *arg)
{
	nni_zt_node *ztn = arg;
	nni_time     now;

	nni_mtx_lock(&nni_zt_lk);
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
	nni_mtx_unlock(&nni_zt_lk);
}

static void
nni_zt_node_resched(nni_zt_node *ztn, uint64_t msec)
{
	ztn->zn_bgtime = msec * 1000; // convert to usec
	nni_cv_wake1(&ztn->zn_bgcv);
}

static void
nni_zt_node_rcv4_cb(void *arg)
{
	nni_zt_node *           ztn = arg;
	nni_aio *               aio = &ztn->zn_rcv4_aio;
	struct sockaddr_storage sa;
	struct sockaddr_in *    sin;
	struct nng_sockaddr_in *nsin;
	uint64_t                now;
	char                    abuf[64];

	if (nni_aio_result(aio) != 0) {
		// Outside of memory exhaustion, we can't really think
		// of any reason for this to legitimately fail.  Arguably
		// we should inject a fallback delay, but for now we just
		// carry on.
		// XXX: REVIEW THIS.  Its clearly wrong!  If the socket is
		// closed or fails in a permanent way, then we need to stop
		// the UDP work, and forward an error to all the other
		// endpoints and pipes!
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

	nni_mtx_lock(&nni_zt_lk);
	now = nni_clock() / 1000; // msec

	// We are not going to perform any validation of the data; we
	// just pass this straight into the ZeroTier core.
	// XXX: CHECK THIS, if it fails then we have a fatal error with
	// the znode, and have to shut everything down.
	ZT_Node_processWirePacket(ztn->zn_znode, NULL, now, 0, (void *) &sa,
	    ztn->zn_rcv4_buf, aio->a_count, &now);

	// Schedule background work
	nni_zt_node_resched(ztn, now);

	// Schedule another receive.
	if ((!ztn->zn_closed) && (ztn->zn_udp4 != NULL)) {
		aio->a_count = 0;
		nni_plat_udp_recv(ztn->zn_udp4, &ztn->zn_rcv4_aio);
	}
	nni_mtx_unlock(&nni_zt_lk);
}

static void
nni_zt_node_rcv6_cb(void *arg)
{
	nni_zt_node *            ztn = arg;
	nni_aio *                aio = &ztn->zn_rcv6_aio;
	struct sockaddr_storage  sa;
	struct sockaddr_in6 *    sin6;
	struct nng_sockaddr_in6 *nsin6;
	uint64_t                 now;
	char                     abuf[64];

	if (nni_aio_result(aio) != 0) {
		// Outside of memory exhaustion, we can't really think
		// of any reason for this to legitimately fail.  Arguably
		// we should inject a fallback delay, but for now we just
		// carry on.
		// XXX: REVIEW THIS.  Its clearly wrong!  If the socket is
		// closed or fails in a permanent way, then we need to stop
		// the UDP work, and forward an error to all the other
		// endpoints and pipes!
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

	nni_mtx_lock(&nni_zt_lk);
	now = nni_clock() / 1000; // msec

	// We are not going to perform any validation of the data; we
	// just pass this straight into the ZeroTier core.
	// XXX: CHECK THIS, if it fails then we have a fatal error with
	// the znode, and have to shut everything down.
	ZT_Node_processWirePacket(ztn->zn_znode, NULL, now, 0, (void *) &sa,
	    ztn->zn_rcv6_buf, aio->a_count, &now);

	// Schedule background work
	nni_zt_node_resched(ztn, now);

	// Schedule another receive.
	if ((!ztn->zn_closed) && (ztn->zn_udp6 != NULL)) {
		aio->a_count = 0;
		nni_plat_udp_recv(ztn->zn_udp6, &ztn->zn_rcv6_aio);
	}
	nni_mtx_unlock(&nni_zt_lk);
}

static uint64_t
nni_zt_mac_to_node(uint64_t mac, uint64_t nwid)
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
nni_zt_node_to_mac(uint64_t node, uint64_t nwid)
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
	// The rest of the network ID is XOR'd in, in reverse byte order.
	mac ^= ((nwid >> 8) & 0xff) << 32;
	mac ^= ((nwid >> 16) & 0xff) << 24;
	mac ^= ((nwid >> 24) & 0xff) << 16;
	mac ^= ((nwid >> 32) & 0xff) << 8;
	mac ^= (nwid >> 40) & 0xff;
	return (mac);
}

static int
nni_zt_result(enum ZT_ResultCode rv)
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
nni_zt_virtual_network_config(ZT_Node *node, void *userptr, void *threadptr,
    uint64_t nwid, void **netptr, enum ZT_VirtualNetworkConfigOperation op,
    const ZT_VirtualNetworkConfig *config)
{
	nni_zt_node *ztn = userptr;
	nni_zt_ep *  ep;

	NNI_ARG_UNUSED(node);
	NNI_ARG_UNUSED(threadptr);
	NNI_ARG_UNUSED(netptr);

	// Maybe we don't have to create taps or anything like that.
	// We do get our mac and MTUs from this, so there's that.
	switch (op) {
	case ZT_VIRTUAL_NETWORK_CONFIG_OPERATION_UP:
	case ZT_VIRTUAL_NETWORK_CONFIG_OPERATION_CONFIG_UPDATE:

		// We only really care about changes to the MTU.  From
		// an API perspective the MAC could change, but that cannot
		// really happen because the node identity and the nwid are
		// fixed.
		NNI_LIST_FOREACH (&ztn->zn_eplist, ep) {
			NNI_ASSERT(nwid == config->nwid);
			if (ep->ze_nwid != config->nwid) {
				continue;
			}
			ep->ze_maxmtu = config->mtu;
			ep->ze_phymtu = config->physicalMtu;
			NNI_ASSERT(ep->ze_mac == config->mac);
		}
		printf("NETWORK %llx / %llx GOING UP / UPDATE\n",
		    ZT_Node_address(node), nwid);
		break;
	default:
		printf("OTHER NETWORK CONFIG OP %x on %llx/%llx\n", op,
		    ZT_Node_address(node), nwid);
		break;
	}
	return (0);
}

static void
nni_zt_send_err(nni_zt_node *ztn, uint64_t nwid, uint64_t dstnode,
    uint32_t srcport, uint32_t dstport, uint8_t err, char *msg)
{
	uint8_t  data[128];
	uint64_t srcmac = nni_zt_node_to_mac(ztn->zn_self, nwid);
	uint64_t dstmac = nni_zt_node_to_mac(dstnode, nwid);
	uint64_t now;

	NNI_ASSERT((strlen(msg) + NNI_ZT_OFFS_ERR_MSG) < sizeof(data));

	data[NNI_ZT_OFFS_OPFLAGS] = NNI_ZT_OP_ERR;
	data[NNI_ZT_OFFS_VERSION] = NNI_ZT_VERSION;
	NNI_PUT32(data + NNI_ZT_OFFS_DST_PORT, dstport);
	NNI_PUT32(data + NNI_ZT_OFFS_SRC_PORT, srcport);
	data[NNI_ZT_OFFS_ERR_CODE] = err;
	nni_strlcpy((char *) data + NNI_ZT_OFFS_ERR_MSG, msg,
	    sizeof(data) - NNI_ZT_OFFS_ERR_MSG);

	now = nni_clock() / 1000;
	ZT_Node_processVirtualNetworkFrame(ztn->zn_znode, NULL, now, nwid,
	    srcmac, dstmac, NNI_ZT_ETHER, 0, data,
	    strlen(msg) + NNI_ZT_OFFS_ERR_MSG, &now);

	nni_zt_node_resched(ztn->zn_znode, now);
}

static void
nni_zt_send_creq(nni_zt_ep *ep, uint64_t dstnode, uint32_t dstport, int ack)
{
	uint8_t  data[NNI_ZT_OFFS_CON_PROT + sizeof(uint16_t)];
	uint64_t srcmac = nni_zt_node_to_mac(ep->ze_self, ep->ze_nwid);
	uint64_t dstmac = nni_zt_node_to_mac(dstnode, ep->ze_nwid);
	uint64_t now;
	enum ZT_ResultCode       zrv;
	ZT_VirtualNetworkConfig *config;

	printf("************ SENDING TO %llx FROM %llx on %llx/\n", dstnode,
	    ZT_Node_address(ep->ze_ztnode->zn_znode), ep->ze_nwid);

	if ((config = ZT_Node_networkConfig(
	         ep->ze_ztnode->zn_znode, ep->ze_nwid)) != NULL) {
		printf("GOT A CONFIG: %d [%s] status %d\n", config->mtu,
		    config->name, config->status);
	} else {
		printf("MISSING CONFIG!\n");
	}

	data[NNI_ZT_OFFS_OPFLAGS] =
	    ack ? NNI_ZT_OP_CON_ACK : NNI_ZT_OP_CON_REQ;
	data[NNI_ZT_OFFS_VERSION] = NNI_ZT_VERSION;
	NNI_PUT32(data + NNI_ZT_OFFS_DST_PORT, dstport);
	NNI_PUT32(data + NNI_ZT_OFFS_SRC_PORT, ep->ze_lport);
	NNI_PUT16(data + NNI_ZT_OFFS_CON_PROT, ep->ze_proto);

	printf("sending to mac %llx\n", dstmac);
	now = nni_clock() / 1000;
	zrv = ZT_Node_processVirtualNetworkFrame(ep->ze_ztnode->zn_znode, NULL,
	    now, ep->ze_nwid, srcmac, dstmac, NNI_ZT_ETHER, 0, data,
	    NNI_ZT_OFFS_CON_PROT + sizeof(uint16_t), &now);
	printf("SAYS %d\n", zrv);

	printf("Rescheduling.... for %llx", now);
	nni_zt_node_resched(ep->ze_ztnode, now);
}

// This function is called when a frame arrives on the *virtual*
// network.
static void
nni_zt_virtual_network_frame(ZT_Node *node, void *userptr, void *threadptr,
    uint64_t netid, void **netptr, uint64_t srcmac, uint64_t dstmac,
    unsigned int ethertype, unsigned int vlanid, const void *data,
    unsigned int len)
{
	nni_zt_node *  ztn = userptr;
	uint8_t        opflags;
	const uint8_t *p = data;
	uint16_t       proto;
	uint32_t       srcport;
	uint32_t       dstport;
	uint64_t       srcconv;
	uint64_t       dstconv;
	uint64_t       srcnode;
	uint64_t       dstnode;

	dstnode = nni_zt_mac_to_node(dstmac, netid);
	srcnode = nni_zt_mac_to_node(srcmac, netid);

	printf("VIRTUAL NET FRAME RECVD\n");
	if (ethertype != NNI_ZT_ETHER) {
		// This is a weird frame we can't use, just throw it away.
		printf("DEBUG: WRONG ETHERTYPE %x\n", ethertype);
		return;
	}

#if 0
	// We should not need to check this, because ZT should ensure we
	// only get messages that are actually directed to us.  We do need
	// to accept that we may have multiple network ids.
	if ((ztn->zn_self != dstmac) || (ztn->zn_nwid != netid)) {
	n		return;
	}
#endif

	if (len < (NNI_ZT_OFFS_SRC_PORT + 4)) {
		printf("DEBUG: RUNT len %d", len);
		return;
	}

	// XXX: arguably we should check the dstmac...
	// XXX: check frame type.

	opflags = p[NNI_ZT_OFFS_OPFLAGS];
	if (p[NNI_ZT_OFFS_VERSION] != NNI_ZT_VERSION) {
		// Wrong version, drop it.  (Log?)
		printf("DEBUG: BAD ZT_VERSION %2x", p[1]);
		return;
	}

	NNI_GET32(p + NNI_ZT_OFFS_DST_PORT, dstport);
	if ((dstport > NNI_ZT_MAX_PORT) || (dstport < 1)) {
		printf("DEBUG: INVALID destination port\n");
		return;
	}
	NNI_GET32(p + NNI_ZT_OFFS_SRC_PORT, srcport);
	if ((srcport > NNI_ZT_MAX_PORT) || (srcport < 1)) {
		printf("DEBUG: INVALID source port\n");
		return;
	}

	dstconv = (dstnode << 24) | dstport;
	srcconv = (srcnode << 24) | srcport;

	switch (opflags) {
	case NNI_ZT_OP_CON_REQ:
	// Look for a matching listener.  If one is found, create a
	// pipe, send an ack.
	case NNI_ZT_OP_CON_ACK:
	// Three cases:
	// 1. Matching waiting dialer.  In this case, create the pipe,
	//    (ready for sending), and send an ack to the listener.
	// 2. Matching waiting listener.  In this case a pipe should
	//    exist, so just mark it ready for sending.  (It could
	//    already receive.)  Arguably if we received a message,
	//    we wouldn't need the CON_ACK.
	// 3. No endpoint.  Send an error.
	case NNI_ZT_OP_PNG_REQ:
	// Look for matching pipe.  If none found, send an error, otherwise
	// send an ack.  Update timestamps.
	case NNI_ZT_OP_PNG_ACK:
	// Update timestamps, no further action.
	case NNI_ZT_OP_DIS:
	// Look for matching pipe.  If found, disconnect.  (Maybe disconnect
	// pending connreq too?)
	case NNI_ZT_OP_DAT:
	case NNI_ZT_OP_DAT | NNI_ZT_FLAG_MF:
	case NNI_ZT_OP_ERR:
	default:
		printf("DEBUG: BAD ZT_OP %x", opflags);
	}

	switch (opflags & 0xf0) {
	case NNI_ZT_OP_CON:
		if (len < NNI_ZT_OFFS_CON_PROT + 2) {
			printf("DEBUG: Missing protocol number in CON");
			return;
		}
		NNI_GET16(p + NNI_ZT_OFFS_CON_PROT, proto);

		// Lets see if we have an endpoint..
		break;
	// XXX: incoming connection request.
	case NNI_ZT_OP_DIS:
	// XXX: look for a matching convo, and close it.
	case NNI_ZT_OP_ERR:
	// XXX: look for a matching convo, and fail it, or if we have
	// a connect request pending, fail that
	case NNI_ZT_OP_PNG:
	// XXX: look for a matching convo, and send a ping ack (updating
	// things).
	default:
		printf("DEBUG: BAD ZT_OP %x", opflags);
		return;
	}
}

static void
nni_zt_event_cb(ZT_Node *node, void *userptr, void *threadptr,
    enum ZT_Event event, const void *payload)
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
nni_zt_state_put(ZT_Node *node, void *userptr, void *threadptr,
    enum ZT_StateObjectType objtype, const uint64_t objid[2], const void *data,
    int len)
{
	FILE *       file;
	nni_zt_node *ztn = userptr;
	char         path[NNG_MAXADDRLEN + 1];
	const char * fname;
	size_t       sz;

	NNI_ARG_UNUSED(objid); // only use global files

	if ((objtype > ZT_STATE_OBJECT_NETWORK_CONFIG) ||
	    ((fname = zt_files[(int) objtype]) == NULL)) {
		return;
	}

	sz = sizeof(path);
	if (snprintf(path, sz, "%s%s%s", ztn->zn_path, pathsep, fname) >= sz) {
		// If the path is too long, we can't cope.  We just
		// decline to store anything.
		return;
	}

	// We assume that everyone can do standard C I/O.
	// This may be a bad assumption.  If that's the case,
	// the platform should supply an alternative implementation.
	// We are also assuming that we don't need to worry about
	// atomic updates.  As these items (keys, etc.)  pretty much
	// don't change, this should be fine.

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
nni_zt_state_get(ZT_Node *node, void *userptr, void *threadptr,
    enum ZT_StateObjectType objtype, const uint64_t objid[2], void *data,
    unsigned int len)
{
	FILE *       file;
	nni_zt_node *ztn = userptr;
	char         path[NNG_MAXADDRLEN + 1];
	const char * fname;
	int          nread;
	size_t       sz;

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
	// the platform should supply an alternative implementation.
	// We are also assuming that we don't need to worry about
	// atomic updates.  As these items (keys, etc.)  pretty much
	// don't change, this should be fine.

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

	printf("*** >>> >>>> READ %d bytes FROM %s\n", nread, path);
	return (nread);
}

// This function is called when ZeroTier desires to send a physical frame.
// The data is a UDP payload, the rest of the payload should be set over
// vanilla UDP.
static int
nni_zt_wire_packet_send(ZT_Node *node, void *userptr, void *threadptr,
    int64_t socket, const struct sockaddr_storage *remaddr, const void *data,
    unsigned int len, unsigned int ttl)
{
	nni_aio              aio;
	nni_sockaddr         addr;
	struct sockaddr_in * sin  = (void *) remaddr;
	struct sockaddr_in6 *sin6 = (void *) remaddr;
	nni_zt_node *        ztn  = userptr;
	nni_plat_udp *       udp;
	char                 abuf[64];
	uint16_t             port;

	NNI_ARG_UNUSED(threadptr);
	NNI_ARG_UNUSED(socket);
	NNI_ARG_UNUSED(ttl);

	// Kind of unfortunate, but we have to convert the sockaddr to
	// a neutral form, and then back again in the platform layer.
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

	nni_aio_init(&aio, NULL, NULL);
	aio.a_addr           = &addr;
	aio.a_niov           = 1;
	aio.a_iov[0].iov_buf = (void *) data;
	aio.a_iov[0].iov_len = len;

	printf("SENDING UDP FRAME TO %s %d\n", abuf, port);
	if ((!ztn->zn_closed) && (udp != NULL)) {
		// This should be non-blocking/best-effort, so while
		// not great that we're holding the lock, also not tragic.
		nni_plat_udp_send(udp, &aio);
	}

	// Arguably, we don't need to wait for the AIO, but we need to
	// have the buffer arranged so that a callback can clean up.
	nni_mtx_unlock(&nni_zt_lk);
	nni_aio_wait(&aio);
	nni_mtx_lock(&nni_zt_lk);
	printf("SEND DONE\n");

	if (nni_aio_result(&aio) != 0) {
		return (-1);
	}

	return (0);
}

static struct ZT_Node_Callbacks nni_zt_callbacks = {
	.version                      = 0,
	.statePutFunction             = nni_zt_state_put,
	.stateGetFunction             = nni_zt_state_get,
	.wirePacketSendFunction       = nni_zt_wire_packet_send,
	.virtualNetworkFrameFunction  = nni_zt_virtual_network_frame,
	.virtualNetworkConfigFunction = nni_zt_virtual_network_config,
	.eventCallback                = nni_zt_event_cb,
	.pathCheckFunction            = NULL,
	.pathLookupFunction           = NULL,
};

static void
nni_zt_node_destroy(nni_zt_node *ztn)
{
	nni_aio_stop(&ztn->zn_rcv4_aio);
	nni_aio_stop(&ztn->zn_rcv6_aio);

	nni_mtx_unlock(&nni_zt_lk);
	// Wait for background thread to exit!
	nni_thr_fini(&ztn->zn_bgthr);
	nni_mtx_lock(&nni_zt_lk);

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
		nni_free(ztn->zn_rcv4_buf, NNI_ZT_RCV_BUFSZ);
	}
	if (ztn->zn_rcv6_buf != NULL) {
		nni_free(ztn->zn_rcv6_buf, NNI_ZT_RCV_BUFSZ);
	}
	nni_aio_fini(&ztn->zn_rcv4_aio);
	nni_aio_fini(&ztn->zn_rcv6_aio);
	nni_idhash_fini(ztn->zn_eps);
	nni_idhash_fini(ztn->zn_pipes);
	nni_cv_fini(&ztn->zn_bgcv);
	NNI_FREE_STRUCT(ztn);
}

static int
nni_zt_node_create(nni_zt_node **ztnp, const char *path)
{
	nni_zt_node *      ztn;
	nng_sockaddr       sa4;
	nng_sockaddr       sa6;
	int                rv;
	enum ZT_ResultCode zrv;

	// We want to bind to any address we can (for now).  Note that
	// at the moment we only support IPv4.  Its unclear how we are meant
	// to handle underlying IPv6 in ZeroTier.  Probably we can use
	// IPv6 dual stock sockets if they exist, but not all platforms
	// support dual-stack.  Furhtermore, IPv6 is not available
	// everywhere, and the root servers may be IPv4 only.
	memset(&sa4, 0, sizeof(sa4));
	sa4.s_un.s_in.sa_family = NNG_AF_INET;
	memset(&sa6, 0, sizeof(sa6));
	sa6.s_un.s_in6.sa_family = NNG_AF_INET6;

	if ((ztn = NNI_ALLOC_STRUCT(ztn)) == NULL) {
		return (NNG_ENOMEM);
	}
	NNI_LIST_INIT(&ztn->zn_eplist, nni_zt_ep, ze_link);
	NNI_LIST_INIT(&ztn->zn_plist, nni_zt_pipe, zp_link);
	nni_cv_init(&ztn->zn_bgcv, &nni_zt_lk);
	nni_aio_init(&ztn->zn_rcv4_aio, nni_zt_node_rcv4_cb, ztn);
	nni_aio_init(&ztn->zn_rcv6_aio, nni_zt_node_rcv6_cb, ztn);

	if (((ztn->zn_rcv4_buf = nni_alloc(NNI_ZT_RCV_BUFSZ)) == NULL) ||
	    ((ztn->zn_rcv6_buf = nni_alloc(NNI_ZT_RCV_BUFSZ)) == NULL)) {
		nni_zt_node_destroy(ztn);
		return (NNG_ENOMEM);
	}
	if (((rv = nni_idhash_init(&ztn->zn_eps)) != 0) ||
	    ((rv = nni_idhash_init(&ztn->zn_pipes)) != 0) ||
	    ((rv = nni_thr_init(&ztn->zn_bgthr, nni_zt_bgthr, ztn)) != 0) ||
	    ((rv = nni_plat_udp_open(&ztn->zn_udp4, &sa4)) != 0) ||
	    ((rv = nni_plat_udp_open(&ztn->zn_udp6, &sa6)) != 0)) {
		nni_zt_node_destroy(ztn);
		return (rv);
	}

	// Setup for dynamic ephemeral port allocations.  We set the range
	// to allow for ephemeral ports, but not higher than the max port,
	// and starting with an initial random value.  Note that this should
	// give us about 8 million possible ephemeral ports.
	nni_idhash_set_limits(ztn->zn_eps, NNI_ZT_EPHEMERAL, NNI_ZT_MAX_PORT,
	    (nni_random() % (NNI_ZT_MAX_PORT - NNI_ZT_EPHEMERAL)) +
	        NNI_ZT_EPHEMERAL);

	(void) snprintf(ztn->zn_path, sizeof(ztn->zn_path), "%s", path);
	zrv = ZT_Node_new(
	    &ztn->zn_znode, ztn, NULL, &nni_zt_callbacks, nni_clock() / 1000);
	if (zrv != ZT_RESULT_OK) {
		nni_zt_node_destroy(ztn);
		return (nni_zt_result(zrv));
	}

	ztn->zn_self = ZT_Node_address(ztn->zn_znode);
	printf("MY NODE ADDRESS IS %llx\n", ztn->zn_self);

	nni_thr_run(&ztn->zn_bgthr);

	// Schedule an initial background run.
	nni_zt_node_resched(ztn, 1);

	// Schedule receive
	ztn->zn_rcv4_aio.a_niov           = 1;
	ztn->zn_rcv4_aio.a_iov[0].iov_buf = ztn->zn_rcv4_buf;
	ztn->zn_rcv4_aio.a_iov[0].iov_len = NNI_ZT_RCV_BUFSZ;
	ztn->zn_rcv4_aio.a_addr           = &ztn->zn_rcv4_addr;
	ztn->zn_rcv4_aio.a_count          = 0;
	ztn->zn_rcv6_aio.a_niov           = 1;
	ztn->zn_rcv6_aio.a_iov[0].iov_buf = ztn->zn_rcv6_buf;
	ztn->zn_rcv6_aio.a_iov[0].iov_len = NNI_ZT_RCV_BUFSZ;
	ztn->zn_rcv6_aio.a_addr           = &ztn->zn_rcv6_addr;
	ztn->zn_rcv6_aio.a_count          = 0;

	nni_plat_udp_recv(ztn->zn_udp4, &ztn->zn_rcv4_aio);
	nni_plat_udp_recv(ztn->zn_udp6, &ztn->zn_rcv6_aio);

	printf("LOOKING GOOD?\n");
	*ztnp = ztn;
	return (0);
}

static int
nni_zt_node_find(nni_zt_node **ztnp, const char *path)
{
	nni_zt_node *ztn;
	int          rv;
	nng_sockaddr sa;

	NNI_LIST_FOREACH (&nni_zt_nodes, ztn) {
		if (strcmp(path, ztn->zn_path) == 0) {
			ztn->zn_refcnt++;
			*ztnp = ztn;
			return (0);
		}
		if (ztn->zn_closed) {
			return (NNG_ECLOSED);
		}
	}

	// We didn't find a node, so make one.  And try to initialize it.
	if ((rv = nni_zt_node_create(&ztn, path)) != 0) {
		return (rv);
	}
	ztn->zn_refcnt++;
	*ztnp = ztn;
	nni_list_append(&nni_zt_nodes, ztn);

	return (0);
}

static void
nni_zt_node_rele(nni_zt_node *ztn)
{
	ztn->zn_refcnt--;
	if (ztn->zn_refcnt != 0) {
		return;
	}
	ztn->zn_closed = 1;
	nni_cv_wake(&ztn->zn_bgcv);
	nni_list_remove(&nni_zt_nodes, ztn);

	printf("DESTROYING NODE %p\n", ztn);
	nni_zt_node_destroy(ztn);
}

static int
nni_zt_chkopt(int opt, const void *dat, size_t sz)
{
	if (opt == nng_optid_recvmaxsz) {
		// We cannot deal with message sizes larger than 64k.
		return (nni_chkopt_size(dat, sz, 0, 0xffffffffU));
	}
	if (opt == nng_optid_zt_home) {
		size_t l = nni_strnlen(dat, sz);
		if ((l >= sz) || (l >= NNG_MAXADDRLEN)) {
			return (NNG_EINVAL);
		}
		// XXX: should we apply additional security checks?
		// home path is not null terminated
		return (0);
	}
	return (NNG_ENOTSUP);
}

static int
nni_zt_tran_init(void)
{
	int rv;
	if (((rv = nni_option_register(nng_opt_zt_home, &nng_optid_zt_home)) !=
	        0) ||
	    ((rv = nni_option_register(nng_opt_zt_nwid, &nng_optid_zt_nwid)) !=
	        0)) {
		return (rv);
	}
	nni_mtx_init(&nni_zt_lk);
	NNI_LIST_INIT(&nni_zt_nodes, nni_zt_node, zn_link);
	return (0);
}

static void
nni_zt_tran_fini(void)
{
	nng_optid_zt_home = -1;
	nng_optid_zt_nwid = -1;

	NNI_ASSERT(nni_list_empty(&nni_zt_nodes));
	nni_mtx_fini(&nni_zt_lk);
}

static void
nni_zt_pipe_close(void *arg)
{
	// This can start the tear down of the connection.
	// It should send an asynchronous DISC message to let the
	// peer know we are shutting down.
}

static void
nni_zt_pipe_fini(void *arg)
{
	// This tosses the connection details and all state.
}

static int
nni_zt_pipe_init(nni_zt_pipe **pipep, nni_zt_ep *ep, void *tpp)
{
	// TCP version of this takes a platform specific structure
	// and creates a pipe.  We should rethink this for ZT.
	return (NNG_ENOTSUP);
}

static void
nni_zt_pipe_send(void *arg, nni_aio *aio)
{
	nni_aio_finish(aio, NNG_ENOTSUP, 0);
}

static void
nni_zt_pipe_recv(void *arg, nni_aio *aio)
{
	nni_aio_finish(aio, NNG_ENOTSUP, 0);
}

static uint16_t
nni_zt_pipe_peer(void *arg)
{
	nni_zt_pipe *pipe = arg;

	return (pipe->zp_peer);
}

static int
nni_zt_pipe_getopt(void *arg, int option, void *buf, size_t *szp)
{
	return (NNG_ENOTSUP);
}

static void
nni_zt_pipe_start(void *arg, nni_aio *aio)
{
	nni_aio_finish(aio, NNG_ENOTSUP, 0);
}

static void
nni_zt_ep_fini(void *arg)
{
	nni_zt_ep *ep = arg;
	NNI_FREE_STRUCT(ep);
}

static int
nni_zt_parsehex(const char **sp, uint64_t *valp)
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
nni_zt_parsedec(const char **sp, uint64_t *valp)
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
nni_zt_ep_init(void **epp, const char *url, nni_sock *sock, int mode)
{
	nni_zt_ep * ep;
	size_t      sz;
	uint64_t    nwid;
	uint64_t    node;
	uint64_t    port;
	int         n;
	char        c;
	const char *u;

	if ((ep = NNI_ALLOC_STRUCT(ep)) == NULL) {
		return (NNG_ENOMEM);
	}
	// URL parsing...
	// URL is form zt://<nwid>[/<remoteaddr>]:<port>
	// The <remoteaddr> part is required for remote dialers, but
	// is not used at all for listeners.  (We have no notion of binding
	// to different node addresses.)
	ep->ze_mode   = mode;
	ep->ze_maxmtu = ZT_MAX_MTU;
	ep->ze_phymtu = ZT_MIN_MTU;
	sz            = sizeof(ep->ze_url);

	nni_aio_list_init(&ep->ze_aios);

	if ((strncmp(url, "zt://", strlen("zt://")) != 0) ||
	    (nni_strlcpy(ep->ze_url, url, sz) >= sz)) {
		return (NNG_EADDRINVAL);
	}
	*epp = ep;
	u    = url + strlen("zt://");
	// Parse the URL.

	switch (mode) {
	case NNI_EP_MODE_DIAL:
		// We require zt://<nwid>/<remotenode>:<port>
		// The remote node must be a 40 bit address (max), and
		// we require a non-zero port to connect to.
		if ((nni_zt_parsehex(&u, &nwid) != 0) || (*u++ != '/') ||
		    (nni_zt_parsehex(&u, &node) != 0) ||
		    (node > 0xffffffffffull) || (*u++ != ':') ||
		    (nni_zt_parsedec(&u, &port) != 0) || (*u != '\0') ||
		    (port > NNI_ZT_MAX_PORT) || (port == 0)) {
			return (NNG_EADDRINVAL);
		}
		ep->ze_lport = 0;
		ep->ze_rport = (uint32_t) port;
		break;
	case NNI_EP_MODE_LISTEN:
		// Listen mode is just zt://<nwid>:<port>.  The port
		// may be zero in this case, to indicate that the server
		// should allocate an ephemeral port.
		if ((nni_zt_parsehex(&u, &nwid) != 0) || (*u++ != ':') ||
		    (nni_zt_parsedec(&u, &port) != 0) || (*u != '\0') ||
		    (port > NNI_ZT_MAX_PORT)) {
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

	ep->ze_node = node;
	ep->ze_nwid = nwid;

	return (0);
}

static void
nni_zt_ep_close(void *arg)
{
	nni_zt_ep *  ep = arg;
	nni_zt_node *ztn;
	nni_aio *    aio;

	// Cancel any outstanding user operation(s)
	nni_mtx_lock(&nni_zt_lk);
	while ((aio = nni_list_first(&ep->ze_aios)) != NULL) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, NNG_ECLOSED);
	}

	// Endpoint framework guarantees to only call us once, and to not
	// call other things while we are closed.
	ztn = ep->ze_ztnode;
	// If we're on the ztn node list, pull us off.
	if (ztn != NULL) {
		nni_list_node_remove(&ep->ze_link);
		nni_idhash_remove(ztn->zn_eps, ep->ze_lport);
	}

	if (ztn != NULL) {
		nni_zt_node_rele(ztn);
		ep->ze_ztnode = NULL;
	}
	nni_mtx_unlock(&nni_zt_lk);
}

static int
nni_zt_ep_join(nni_zt_ep *ep)
{
	enum ZT_ResultCode       zrv;
	nni_zt_node *            ztn = ep->ze_ztnode;
	ZT_VirtualNetworkConfig *config;

	if ((zrv = ZT_Node_join(ztn->zn_znode, ep->ze_nwid, ztn, NULL)) !=
	    ZT_RESULT_OK) {
		nni_zt_node_rele(ztn);
		return (nni_zt_result(zrv));
	}

	if ((config = ZT_Node_networkConfig(ztn->zn_znode, ep->ze_nwid)) !=
	    NULL) {
		NNI_ASSERT(config->nwid == ep->ze_nwid);
		ep->ze_maxmtu = config->mtu;
		ep->ze_phymtu = config->physicalMtu;
		NNI_ASSERT(ep->ze_mac == config->mac);
		ZT_Node_freeQueryResult(ztn->zn_znode, config);
		printf("GOT A NETWORK CONFIG MTU %d\n", ep->ze_maxmtu);
	} else {
		printf("NETWORK CONFIG IS NULL!!!!\n");
	}
	return (0);
}

static int
nni_zt_ep_bind(void *arg)
{
	int          rv;
	nni_zt_ep *  ep = arg;
	nni_zt_node *ztn;

	nni_mtx_lock(&nni_zt_lk);
	if ((rv = nni_zt_node_find(&ztn, ep->ze_home)) != 0) {
		nni_mtx_unlock(&nni_zt_lk);
		return (rv);
	}

	if (ep->ze_lport == 0) {
		uint64_t port;
		// ask for an ephemeral port
		rv = nni_idhash_alloc(ztn->zn_eps, &port, ep);
		if (rv == 0) {
			NNI_ASSERT(port < NNI_ZT_MAX_PORT);
			NNI_ASSERT(port & NNI_ZT_EPHEMERAL);
			ep->ze_lport = (uint32_t) port;
		}
	} else {
		nni_zt_ep *srch;
		// Make sure our port is not already in use.
		if (nni_idhash_find(
		        ztn->zn_eps, ep->ze_lport, (void **) &srch) == 0) {
			nni_zt_node_rele(ztn);
			nni_mtx_unlock(&nni_zt_lk);
			return (NNG_EADDRINUSE);
		}

		// we have a specific port to use
		rv = nni_idhash_insert(ztn->zn_eps, ep->ze_lport, ep);
	}
	if (rv != 0) {
		nni_zt_node_rele(ztn);
		nni_mtx_unlock(&nni_zt_lk);
		return (rv);
	}
	nni_list_append(&ztn->zn_eplist, ep);
	ep->ze_self   = ztn->zn_self;
	ep->ze_mac    = nni_zt_node_to_mac(ep->ze_self, ep->ze_nwid);
	ep->ze_ztnode = ztn;

	(void) nni_zt_ep_join(ep);
	nni_mtx_unlock(&nni_zt_lk);

	return (0);
}

static void
nni_zt_ep_cancel(nni_aio *aio, int rv)
{
	nni_zt_ep *ep = aio->a_prov_data;

	nni_mtx_lock(&nni_zt_lk);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&nni_zt_lk);
}

static void
nni_zt_ep_doaccept(nni_zt_ep *ep)
{
	// Call with ep lock held.
	nni_time now;

	now = nni_clock();
	// Consume any timedout connect requests.
	while (ep->ze_creq_tail != ep->ze_creq_head) {
		nni_zt_creq creq;
		nni_aio *   aio;

		creq = ep->ze_creqs[ep->ze_creq_tail % NNI_ZT_LISTENQ];
		// Discard old connection requests.
		if (creq.cr_time < now) {
			ep->ze_creq_tail++;
			continue;
		}

		if ((aio = nni_list_first(&ep->ze_aios)) == NULL) {
			// No outstanding accept.  We're done.
			break;
		}

		// We have both a connection request, and a place to
		// accept it.

		// Advance the tail.
		ep->ze_creq_tail++;

		// We remove this AIO.  This keeps it from being canceled.
		nni_aio_list_remove(aio);

		// Now we need to create a pipe, send the notice to
		// the user, and finish the request.  For now we are pretty
		// lame and just return NNG_EINTERNAL.

		nni_aio_finish_error(aio, NNG_EINTERNAL);
	}
}

static void
nni_zt_ep_accept(void *arg, nni_aio *aio)
{
	nni_zt_ep *ep = arg;

	nni_mtx_lock(&nni_zt_lk);
	if (nni_aio_start(aio, nni_zt_ep_cancel, ep) == 0) {
		nni_aio_list_append(&ep->ze_aios, aio);
		nni_zt_ep_doaccept(ep);
	}
	nni_mtx_unlock(&nni_zt_lk);
}

static void
nni_zt_ep_connect(void *arg, nni_aio *aio)
{
	nni_zt_ep *  ep = arg;
	uint64_t     port;
	nni_zt_node *ztn;
	int          rv;

	nni_mtx_lock(&nni_zt_lk);
	if ((rv = nni_zt_node_find(&ztn, ep->ze_home)) != 0) {
		nni_mtx_unlock(&nni_zt_lk);
		nni_aio_finish_error(aio, rv);
		return;
	}

	// Allocate an ephemeral port.  We will keep this one forever.
	if (ep->ze_lport == 0) {
		// ask for an ephemeral port
		rv = nni_idhash_alloc(ztn->zn_eps, &port, ep);
		if (rv == 0) {
			NNI_ASSERT(port <= NNI_ZT_MAX_PORT);
			NNI_ASSERT(port & NNI_ZT_EPHEMERAL);
			ep->ze_lport = (uint32_t) port;
		}
		nni_list_append(&ztn->zn_eplist, ep);
	}

	// Force a maximum timeout for connect.
	// XXX: This should probably be tunable.
	if (aio->a_expire > (nni_clock() + NNI_ZT_CONN_MAXTIME)) {
		aio->a_expire = nni_clock() + NNI_ZT_CONN_MAXTIME;
	}
	if (nni_aio_start(aio, nni_zt_ep_cancel, ep) == 0) {
		nni_aio_list_append(&ep->ze_aios, aio);

		ep->ze_ztnode = ztn;
		ep->ze_self   = ztn->zn_self;
		ep->ze_mac    = nni_zt_node_to_mac(ep->ze_self, ep->ze_nwid);

		(void) nni_zt_ep_join(ep);
		nni_mtx_unlock(&nni_zt_lk);

		printf("SLEEPING FOR 20 s\n");
		nni_usleep(20000000); // maybe?
		printf("WE'RE BAAAACK....!!!!\n");
		nni_mtx_lock(&nni_zt_lk);
		nni_zt_send_creq(ep, ep->ze_node, ep->ze_rport, 0);
		// XXX: send out the connect message.
	}
	nni_mtx_unlock(&nni_zt_lk);
}

static int
nni_zt_ep_setopt(void *arg, int opt, const void *data, size_t size)
{
	nni_zt_ep *ep = arg;
	int        i;
	int        rv = NNG_ENOTSUP;

	if (opt == nng_optid_recvmaxsz) {
		nni_mtx_lock(&nni_zt_lk);
		rv = nni_setopt_size(
		    &ep->ze_rcvmax, data, size, 0, 0xffffffffu);
		nni_mtx_unlock(&nni_zt_lk);
	} else if (opt == nng_optid_zt_home) {
		// XXX: check to make sure not started...
		i = nni_strnlen((const char *) data, size);
		if ((i >= size) || (i >= NNG_MAXADDRLEN)) {
			return (NNG_EINVAL);
		}
		nni_mtx_lock(&nni_zt_lk);
		(void) snprintf(ep->ze_home, sizeof(ep->ze_home), "%s", data);
		nni_mtx_unlock(&nni_zt_lk);
		rv = 0;
	}
	return (rv);
}

static int
nni_zt_ep_getopt(void *arg, int opt, void *data, size_t *sizep)
{
	nni_zt_ep *ep = arg;
	int        rv = NNG_ENOTSUP;

	if (opt == nng_optid_recvmaxsz) {
		nni_mtx_lock(&nni_zt_lk);
		rv = nni_getopt_size(&ep->ze_rcvmax, data, sizep);
		nni_mtx_unlock(&nni_zt_lk);
	} else if (opt == nng_optid_zt_home) {
		nni_mtx_lock(&nni_zt_lk);
		rv = nni_getopt_str(ep->ze_home, data, sizep);
		nni_mtx_unlock(&nni_zt_lk);
	} else if (opt == nng_optid_zt_nwid) {
		nni_mtx_lock(&nni_zt_lk);
		rv = nni_getopt_u64(ep->ze_nwid, data, sizep);
		nni_mtx_unlock(&nni_zt_lk);
	}
	return (rv);
}

static nni_tran_pipe nni_zt_pipe_ops = {
	.p_fini   = nni_zt_pipe_fini,
	.p_start  = nni_zt_pipe_start,
	.p_send   = nni_zt_pipe_send,
	.p_recv   = nni_zt_pipe_recv,
	.p_close  = nni_zt_pipe_close,
	.p_peer   = nni_zt_pipe_peer,
	.p_getopt = nni_zt_pipe_getopt,
};

static nni_tran_ep nni_zt_ep_ops = {
	.ep_init    = nni_zt_ep_init,
	.ep_fini    = nni_zt_ep_fini,
	.ep_connect = nni_zt_ep_connect,
	.ep_bind    = nni_zt_ep_bind,
	.ep_accept  = nni_zt_ep_accept,
	.ep_close   = nni_zt_ep_close,
	.ep_setopt  = nni_zt_ep_setopt,
	.ep_getopt  = NULL,
};

// This is the ZeroTier transport linkage, and should be the only global
// symbol in this entire file.
static struct nni_tran nni_zt_tran = {
	.tran_version = NNI_TRANSPORT_VERSION,
	.tran_scheme  = "zt",
	.tran_ep      = &nni_zt_ep_ops,
	.tran_pipe    = &nni_zt_pipe_ops,
	.tran_chkopt  = nni_zt_chkopt,
	.tran_init    = nni_zt_tran_init,
	.tran_fini    = nni_zt_tran_fini,
};

int
nng_zt_register(void)
{
	return (nni_tran_register(&nni_zt_tran));
}

#endif