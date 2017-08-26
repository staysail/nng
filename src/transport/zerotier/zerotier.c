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

int nng_optid_zt_home = -1;
int nng_optid_zt_nwid = -1;

#ifndef _WIN32
#include <unistd.h>
#endif

#include <ZeroTierOne.h>

// ZeroTier Transport.  This sits on the ZeroTier L2 network, which itself
// is implemented on top of L3 (mostly UDP).  This requires the 3rd party
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

#define NNI_ZT_VERSION 0x01 // specified per RFC

#define NNI_ZT_EREFUSED 0x01 // Nothing there, connection refused
#define NNI_ZT_ENOTCONN 0x02 // Connection does not exist
#define NNI_ZT_EWRONGSP 0x03 // Wrong SP number
#define NNI_ZT_EPROTERR 0x04 // Other protocol error
#define NNI_ZT_EMSGSIZE 0x05 // Message too large
#define NNI_ZT_EUNKNOWN 0xff // Other errors

#define NNI_ZT_EPHEMERAL_PORT (1U << 31)

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
	uint64_t      zn_nwid;
	int           zn_maxmtu;
	int           zn_phymtu;
	nni_list_node zn_link;
	int           zn_refcnt;
	int           zn_closed;
	nni_mtx       zn_lk;
	nni_plat_udp *zn_udp;
	nni_list      zn_eplist;
	nni_list      zn_plist;
	nni_idhash *  zn_eps;
	nni_idhash *  zn_pipes;
	nni_aio       zn_snd_aio;
	nni_aio       zn_rcv_aio;
	char *        zn_rcv_buf;
	nng_sockaddr  zn_rcv_addr;
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
	uint64_t      zp_conv_id;
	uint32_t      zp_src_port;
	uint32_t      zp_dst_port;

	// XXX: fraglist
	nni_sockaddr zp_remaddr;
	nni_sockaddr zp_locaddr;

	nni_aio  zp_txaio;
	nni_aio  zp_rxaio;
	nni_msg *zp_rxmsg;
	nni_mtx  zp_lk;
};

struct nni_zt_ep {
	nni_list_node ze_link;
	char          ze_url[NNG_MAXADDRLEN];
	char          ze_home[NNG_MAXADDRLEN]; // should be enough
	ZT_Node *     ze_ztnode;
	uint64_t      ze_nwid;
	uint64_t      ze_node; // remote node address normally
	int           ze_mode;
	nni_sockaddr  ze_addr;
	uint32_t      ze_port;
	uint16_t      ze_proto;
	size_t        ze_rcvmax;
	nni_aio       ze_aio;
	nni_aio *     ze_user_aio;
	nni_mtx       ze_lk;
};

static nni_mtx  nni_zt_lk;
static nni_list nni_zt_nodes;

static void
nni_zt_bgthr(void *arg)
{
	nni_zt_node *ztn = arg;
	nni_time     now;

	nni_mtx_lock(&ztn->zn_lk);
	for (;;) {
		now = nni_clock() / 1000; // msec

		if (ztn->zn_closed) {
			break;
		}

		if (now < ztn->zn_bgtime) {
			nni_cv_until(&ztn->zn_bgcv, ztn->zn_bgtime);
			continue;
		}

		nni_mtx_unlock(&ztn->zn_lk);
		ZT_Node_processBackgroundTasks(ztn->zn_znode, NULL, now, &now);
		nni_mtx_lock(&ztn->zn_lk);

		ztn->zn_bgtime = now * 1000; // usec
	}
	nni_mtx_unlock(&ztn->zn_lk);
}

static void
nni_zt_node_resched(nni_zt_node *ztn, uint64_t msec)
{
	nni_mtx_lock(&ztn->zn_lk);
	ztn->zn_bgtime = msec * 1000; // convert to usec
	nni_cv_wake1(&ztn->zn_bgcv);
	nni_mtx_unlock(&ztn->zn_lk);
}

static void
nni_zt_node_rcv_cb(void *arg)
{
	nni_zt_node *            ztn = arg;
	nni_aio *                aio = &ztn->zn_rcv_aio;
	struct sockaddr_storage  sa;
	struct sockaddr_in *     sin;
	struct sockaddr_in6 *    sin6;
	struct nng_sockaddr_in * nsin;
	struct nng_sockaddr_in6 *nsin6;
	uint64_t                 now;

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
	switch (ztn->zn_rcv_addr.s_un.s_family) {
	case NNG_AF_INET:
		sin                  = (void *) &sa;
		nsin                 = &ztn->zn_rcv_addr.s_un.s_in;
		sin->sin_family      = AF_INET;
		sin->sin_port        = nsin->sa_port;
		sin->sin_addr.s_addr = nsin->sa_addr;
		break;
	case NNG_AF_INET6:
		sin6              = (void *) &sa;
		nsin6             = &ztn->zn_rcv_addr.s_un.s_in6;
		sin6->sin6_family = AF_INET6;
		sin6->sin6_port   = nsin6->sa_port;
		memcpy(&sin6->sin6_addr, nsin6->sa_addr, 16);
	default:
		NNI_ASSERT(0); // bad family!
		goto skip;
	}

	now = nni_clock() / 1000; // msec

	// We are not going to perform any validation of the data; we
	// just pass this straight into the ZeroTier core.
	// XXX: CHECK THIS, if it fails then we have a fatal error with
	// the znode, and have to shut everything down.
	ZT_Node_processWirePacket(ztn->zn_znode, NULL, now, 0, (void *) &sa,
	    ztn->zn_rcv_buf, aio->a_count, &now);

	// Schedule background work
	nni_zt_node_resched(ztn, now);

skip:
	// Schedule another receive.
	nni_mtx_lock(&ztn->zn_lk);
	if ((!ztn->zn_closed) && (ztn->zn_udp != NULL)) {
		aio->a_count = 0;
		nni_plat_udp_recv(ztn->zn_udp, &ztn->zn_rcv_aio);
	}
	nni_mtx_unlock(&ztn->zn_lk);
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
    uint64_t netid, void **netptr, enum ZT_VirtualNetworkConfigOperation op,
    const ZT_VirtualNetworkConfig *config)
{
	nni_zt_node *ztn = userptr;

	NNI_ARG_UNUSED(node);
	NNI_ARG_UNUSED(threadptr);
	NNI_ARG_UNUSED(netid);
	NNI_ARG_UNUSED(netptr);

	// Maybe we don't have to create taps or anything like that.
	// We do get our mac and MTUs from this, so there's that.
	nni_mtx_lock(&ztn->zn_lk);
	switch (op) {
	case ZT_VIRTUAL_NETWORK_CONFIG_OPERATION_UP:
	case ZT_VIRTUAL_NETWORK_CONFIG_OPERATION_CONFIG_UPDATE:
		ztn->zn_self   = config->mac;
		ztn->zn_nwid   = config->nwid;
		ztn->zn_maxmtu = config->mtu;
		ztn->zn_phymtu = config->physicalMtu;
		break;
	default:
		break;
	}
	nni_mtx_unlock(&ztn->zn_lk);
	return (0);
}

// This function is called when a frame arrives on the *virtual*
// network.
static void
nni_zt_virtual_network_frame(ZT_Node *node, void *userptr, void *threadptr,
    uint64_t netid, void **netptr, uint64_t srcmac, uint64_t dstmac,
    unsigned int ethertype, unsigned int vlanid, const void *data,
    unsigned int len)
{
	nni_zt_node *ztn = userptr;

	if (ethertype != NNI_ZT_ETHER) {
		// This is a weird frame we can't use, just throw it away.
		printf("DEBUG: WRONG ETHERTYPE %x\n", ethertype);
		return;
	}
	nni_mtx_lock(&ztn->zn_lk);
	if ((ztn->zn_self != dstmac) || (ztn->zn_nwid != netid)) {
		nni_mtx_unlock(&ztn->zn_lk);
		return;
	}
	nni_mtx_unlock(&ztn->zn_lk);
	// XXX: arguably we should check the dstmac...

	// XXX: check frame type.
}

static void
nni_zt_event_cb(ZT_Node *node, void *userptr, void *threadptr,
    enum ZT_Event event, const void *payload)
{
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
	printf("WROTE TO %s\n", path);
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

	if ((file = fopen(path, "wb")) == NULL) {
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
		break;
	case AF_INET6:
		// This probably will not work, since the underlying
		// UDP socket is IPv4 only.  (In the future we can
		// consider using a separate IPv6 socket.)
		addr.s_un.s_in6.sa_family = NNG_AF_INET6;
		addr.s_un.s_in6.sa_port   = sin6->sin6_port;
		memcpy(addr.s_un.s_in6.sa_addr, sin6->sin6_addr.s6_addr, 16);
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

	nni_mtx_lock(&ztn->zn_lk);
	if ((!ztn->zn_closed) && (ztn->zn_udp != NULL)) {
		// This should be non-blocking/best-effort, so while not
		// great that we're holding the lock, also not tragic.
		nni_plat_udp_send(ztn->zn_udp, &aio);
	}
	nni_mtx_unlock(&ztn->zn_lk);

	nni_aio_wait(&aio);
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
	nni_aio_stop(&ztn->zn_snd_aio);
	nni_aio_stop(&ztn->zn_rcv_aio);

	// We will already have been marked for close, so the
	// bgthread should already have wound down, or be doing so.
	nni_thr_fini(&ztn->zn_bgthr);

	if (ztn->zn_znode != NULL) {
		ZT_Node_delete(ztn->zn_znode);
	}
	if (ztn->zn_udp != NULL) {
		nni_plat_udp_close(ztn->zn_udp);
	}
	if (ztn->zn_rcv_buf != NULL) {
		nni_free(ztn->zn_rcv_buf, NNI_ZT_RCV_BUFSZ);
	}
	nni_aio_fini(&ztn->zn_snd_aio);
	nni_aio_fini(&ztn->zn_rcv_aio);
	nni_idhash_fini(ztn->zn_eps);
	nni_idhash_fini(ztn->zn_pipes);
	nni_cv_fini(&ztn->zn_bgcv);
	nni_mtx_fini(&ztn->zn_lk);
	NNI_FREE_STRUCT(ztn);
}

static int
nni_zt_node_create(nni_zt_node **ztnp, const char *path)
{
	nni_zt_node *      ztn;
	nng_sockaddr       sa;
	int                rv;
	enum ZT_ResultCode zrv;

	// We want to bind to any address we can (for now).  Note that
	// at the moment we only support IPv4.  Its unclear how we are meant
	// to handle underlying IPv6 in ZeroTier.  Probably we can use
	// IPv6 dual stock sockets if they exist, but not all platforms
	// support dual-stack.  Furhtermore, IPv6 is not available
	// everywhere, and the root servers may be IPv4 only.
	memset(&sa, 0, sizeof(sa));
	sa.s_un.s_in.sa_family = NNG_AF_INET;

	if ((ztn = NNI_ALLOC_STRUCT(ztn)) == NULL) {
		return (NNG_ENOMEM);
	}
	NNI_LIST_INIT(&ztn->zn_eplist, nni_zt_ep, ze_link);
	NNI_LIST_INIT(&ztn->zn_plist, nni_zt_pipe, zp_link);
	nni_mtx_init(&ztn->zn_lk);
	nni_cv_init(&ztn->zn_bgcv, &ztn->zn_lk);
	nni_aio_init(&ztn->zn_rcv_aio, nni_zt_node_rcv_cb, ztn);

	if ((ztn->zn_rcv_buf = nni_alloc(NNI_ZT_RCV_BUFSZ)) == NULL) {
		nni_zt_node_destroy(ztn);
		return (NNG_ENOMEM);
	}
	if (((rv = nni_idhash_init(&ztn->zn_eps)) != 0) ||
	    ((rv = nni_idhash_init(&ztn->zn_pipes)) != 0) ||
	    ((rv = nni_thr_init(&ztn->zn_bgthr, nni_zt_bgthr, ztn)) != 0) ||
	    ((rv = nni_plat_udp_open(&ztn->zn_udp, &sa)) != 0)) {
		nni_zt_node_destroy(ztn);
		return (rv);
	}

	// Setup for dynamic ephemeral port allocations.
	nni_idhash_set_limits(ztn->zn_eps, NNI_ZT_EPHEMERAL_PORT, 0xffffffffU,
	    NNI_ZT_EPHEMERAL_PORT);

	(void) snprintf(ztn->zn_path, sizeof(ztn->zn_path), "%s", path);
	zrv = ZT_Node_new(
	    &ztn->zn_znode, ztn, NULL, &nni_zt_callbacks, nni_clock() / 1000);
	if (zrv != ZT_RESULT_OK) {
		nni_zt_node_destroy(ztn);
		return (nni_zt_result(zrv));
	}

	nni_thr_run(&ztn->zn_bgthr);

	// Schedule an initial background run.
	nni_zt_node_resched(ztn, 1);
	*ztnp = ztn;
	return (0);
}

static int
nni_zt_node_find(nni_zt_node **ztnp, const char *path)
{
	nni_zt_node *      ztn;
	enum ZT_ResultCode zrv;
	int                rv;
	nng_sockaddr       sa;

	nni_mtx_lock(&nni_zt_lk);
	NNI_LIST_FOREACH (&nni_zt_nodes, ztn) {
		if (strcmp(path, ztn->zn_path) == 0) {
			ztn->zn_refcnt++;
			*ztnp = ztn;
			nni_mtx_unlock(&nni_zt_lk);
			return (0);
		}
		if (ztn->zn_closed) {
			nni_mtx_unlock(&nni_zt_lk);
			return (NNG_ECLOSED);
		}
	}

	// We didn't find a node, so make one.  And try to initialize it.
	if ((rv = nni_zt_node_create(&ztn, path)) != 0) {
		nni_mtx_unlock(&nni_zt_lk);
		return (rv);
	}
	ztn->zn_refcnt++;
	*ztnp = ztn;
	nni_list_append(&nni_zt_nodes, ztn);
	nni_mtx_unlock(&nni_zt_lk);
	return (0);
}

static void
nni_zt_node_rele(nni_zt_node *ztn)
{
	nni_mtx_lock(&nni_zt_lk);
	ztn->zn_refcnt--;
	if (ztn->zn_refcnt != 0) {
		nni_mtx_unlock(&nni_zt_lk);
		return;
	}
	ztn->zn_closed = 1;
	nni_cv_wake(&ztn->zn_bgcv);
	nni_list_remove(&nni_zt_nodes, ztn);
	nni_mtx_unlock(&nni_zt_lk);

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
	nni_mtx_fini(&ep->ze_lk);
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
	*sp = s;
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
	ep->ze_mode = mode;
	sz          = sizeof(ep->ze_url);

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
		// The remotenode must be a 40 bit address (max), and
		// we require a non-zero port to connect to.
		if ((nni_zt_parsehex(&u, &nwid) != 0) || (*u++ != '/') ||
		    (nni_zt_parsehex(&u, &node) != 0) ||
		    (node > 0xffffffffff) || (*u++ != ':') ||
		    (nni_zt_parsedec(&u, &port) != 0) || (*u != '\0') ||
		    (port == 0)) {
			return (NNG_EADDRINVAL);
		}
		break;
	case NNI_EP_MODE_LISTEN:
		// Listen mode is just zt://<nwid>:<port>.  The port
		// may be zero in this case, to indicate that the server
		// should allocate an ephemeral port.
		if ((nni_zt_parsehex(&u, &nwid) != 0) || (*u++ != ':') ||
		    (nni_zt_parsedec(&u, &port) != 0) || (*u != '\0')) {
			return (NNG_EADDRINVAL);
		}
		node = 0;
		break;
	default:
		NNI_ASSERT(0);
		break;
	}

	ep->ze_port = port;
	ep->ze_node = node;
	ep->ze_nwid = nwid;

	nni_mtx_init(&ep->ze_lk);
	return (0);
}

static void
nni_zt_ep_close(void *arg)
{
	nni_zt_ep *  ep = arg;
	nni_zt_node *ztn;

	// XXX: cancel AIOs...

	// Endpoint framework guarantees to only call us once, and to not
	// call other things while we are closed.
	nni_mtx_lock(&nni_zt_lk);
	ztn = ep->ze_ztnode;
	// If we're on the ztn node list, pull us off.
	if (ztn != NULL) {
		nni_list_node_remove(&ep->ze_link);
		nni_idhash_remove(ztn->zn_eps, ep->ze_port);
	}
	nni_mtx_unlock(&nni_zt_lk);

	if (ztn != NULL) {
		nni_zt_node_rele(ztn);
		ep->ze_ztnode = NULL;
	}
}

static int
nni_zt_ep_bind(void *arg)
{
	int          rv;
	nni_zt_ep *  ep = arg;
	nni_zt_ep *  srch;
	nni_zt_node *ztn;

	if ((rv = nni_zt_node_find(&ztn, ep->ze_home)) != 0) {
		return (rv);
	}
	nni_mtx_lock(&nni_zt_lk);

	// Look to ensure we have an exclusive bind.
	if (nni_idhash_find(ztn->zn_eps, ep->ze_port, (void **) &srch) == 0) {
		nni_mtx_unlock(&nni_zt_lk);
		nni_zt_node_rele(ztn);
		return (NNG_EADDRINUSE);
	}
	if (ep->ze_port == 0) {
		uint64_t port;
		// ask for an ephemeral port
		rv = nni_idhash_alloc(ztn->zn_eps, &port, ep);
		if (rv == 0) {
			NNI_ASSERT(port < (1ULL << 32));
			NNI_ASSERT(port & NNI_ZT_EPHEMERAL_PORT);
			ep->ze_port = (uint32_t) port;
		}
	} else {
		// we have a specific port to use
		rv = nni_idhash_insert(ztn->zn_eps, ep->ze_port, ep);
	}
	if (rv != 0) {
		nni_mtx_unlock(&nni_zt_lk);
		nni_zt_node_rele(ztn);
		return (rv);
	}
	nni_list_append(&ztn->zn_eplist, ep);
	nni_mtx_unlock(&nni_zt_lk);

	return (0);
}

static void
nni_zt_ep_accept(void *arg, nni_aio *aio)
{
	nni_aio_finish(aio, NNG_ENOTSUP, 0);
}

static void
nni_zt_ep_connect(void *arg, nni_aio *aio)
{
	nni_aio_finish(aio, NNG_ENOTSUP, 0);
}

static int
nni_zt_ep_setopt(void *arg, int opt, const void *data, size_t size)
{
	nni_zt_ep *ep = arg;
	int        i;
	int        rv = NNG_ENOTSUP;

	if (opt == nng_optid_recvmaxsz) {
		nni_mtx_lock(&ep->ze_lk);
		rv = nni_setopt_size(
		    &ep->ze_rcvmax, data, size, 0, 0xffffffffu);
		nni_mtx_unlock(&ep->ze_lk);
	} else if (opt == nng_optid_zt_home) {
		// XXX: check to make sure not started...
		for (i = 0; i < size; i++) {
			if (((const char *) data)[i] == '\0') {
				break;
			}
		}
		if ((i >= size) || (i >= NNG_MAXADDRLEN)) {
			return (NNG_EINVAL);
		}
		nni_mtx_lock(&ep->ze_lk);
		(void) snprintf(ep->ze_home, sizeof(ep->ze_home), "%s", data);
		nni_mtx_unlock(&ep->ze_lk);
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
		nni_mtx_lock(&ep->ze_lk);
		rv = nni_getopt_size(&ep->ze_rcvmax, data, sizep);
		nni_mtx_unlock(&ep->ze_lk);
	} else if (opt == nng_optid_zt_home) {
		// XXX: we really want a helper for string data.
		nni_mtx_lock(&ep->ze_lk);
		size_t sz = strlen(ep->ze_home);
		if (sz > *sizep) {
			sz = *sizep;
		}
		*sizep = strlen(ep->ze_home);
		nni_mtx_lock(&ep->ze_lk);
		memcpy(data, ep->ze_home, sz);
		rv = 0;
	} else if (opt == nng_optid_zt_nwid) {
		size_t sz = sizeof(uint64_t);
		if (sz > *sizep) {
			sz = *sizep;
		}
		*sizep = sizeof(uint64_t);
		nni_mtx_lock(&ep->ze_lk);
		memcpy(&ep->ze_nwid, data, sz);
		nni_mtx_unlock(&ep->ze_lk);
		rv = 0;
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