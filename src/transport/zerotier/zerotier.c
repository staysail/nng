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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"

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

// This node structure is wrapped around the ZT_node; this allows us to
// have multiple endpoints referencing the same ZT_node, but also to
// support different nodes (identities) based on different homedirs.
// This means we need to stick these on a global linked list, manage
// them with a reference count, and uniquely identify them using the
// homedir.

struct nni_zt_node {
	char          zn_path[NNG_MAXADDRLEN + 1]; // ought to be sufficient
	ZT_Node *     zn_znode;
	nni_list_node zn_link;
	int           zn_refcnt;
	int           zn_closed;
	nni_list      zn_eps;
	nni_list      zn_pipes;
	nni_plat_udp *zn_udp;
	nni_aio       zn_rcv_aio;
	nni_aio       zn_snd_aio;
	nni_mtx       zn_lk;
};

struct nni_zt_pipe {
	nni_list_node zp_link;
	const char *  zp_addr;
	uint16_t      zp_peer;
	uint16_t      zp_proto;
	size_t        zp_rcvmax;
	nni_aio *     zp_user_txaio;
	nni_aio *     zp_user_rxaio;
	nni_aio *     zp_user_negaio;

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
	char          ze_url[NNG_MAXADDRLEN + 1];
	char          ze_home[NNG_MAXADDRLEN + 1]; // should be enough
	ZT_Node *     ze_ztnode;
	uint64_t      ze_nwid;
	int           ze_mode;
	nni_sockaddr  ze_locaddr;
	int           ze_closed;
	uint16_t      ze_proto;
	size_t        ze_rcvmax;
	nni_aio       ze_aio;
	nni_aio *     ze_user_aio;
	nni_mtx       ze_lk;
};

static nni_mtx  nni_zt_lk;
static nni_list nni_zt_nodes;

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
	NNI_ARG_UNUSED(node);
	NNI_ARG_UNUSED(userptr);
	NNI_ARG_UNUSED(threadptr);
	NNI_ARG_UNUSED(netid);
	NNI_ARG_UNUSED(netptr);
	NNI_ARG_UNUSED(op);
	NNI_ARG_UNUSED(config);
	// Maybe we don't have to create taps or anything like that.
	// We do get our mac and MTUs from this, so there's that.
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

	NNI_ARG_UNUSED(objid); // only use global files

	if ((objtype > ZT_STATE_OBJECT_NETWORK_CONFIG) ||
	    ((fname = zt_files[(int) objtype]) == NULL)) {
		return;
	}

	(void) snprintf(
	    path, sizeof(path), "%s%s%s", ztn->zn_path, pathsep, fname);

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

	NNI_ARG_UNUSED(objid); // only use global files

	if ((objtype > ZT_STATE_OBJECT_NETWORK_CONFIG) ||
	    ((fname = zt_files[(int) objtype]) == NULL)) {
		return (-1);
	}

	(void) snprintf(
	    path, sizeof(path), "%s%s%s", ztn->zn_path, pathsep, fname);

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

	nni_plat_udp_send(ztn->zn_udp, &aio);

	// nni_aio_wait(&aio);
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

static int
nni_zt_find_node(nni_zt_node **ztnp, const char *path)
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
	if ((ztn = NNI_ALLOC_STRUCT(ztn)) == NULL) {
		nni_mtx_unlock(&nni_zt_lk);
		return (NNG_ENOMEM);
	}

	// We want to bind to any address we can (for now).  Note that
	// at the moment we only support IPv4.  Its unclear how we are meant
	// to handle underlying IPv6 in ZeroTier.
	memset(&sa, 0, sizeof(sa));
	sa.s_un.s_in.sa_family = NNG_AF_INET;

	if ((rv = nni_plat_udp_open(&ztn->zn_udp, &sa)) != 0) {
		nni_mtx_unlock(&nni_zt_lk);
		NNI_FREE_STRUCT(ztn);
		return (rv);
	}

	(void) snprintf(ztn->zn_path, sizeof(ztn->zn_path), "%s", path);
	zrv = ZT_Node_new(
	    &ztn->zn_znode, ztn, NULL, &nni_zt_callbacks, nni_clock() / 1000);
	if (zrv != ZT_RESULT_OK) {
		nni_mtx_unlock(&nni_zt_lk);
		nni_plat_udp_close(ztn->zn_udp);
		NNI_FREE_STRUCT(ztn);
		return (nni_zt_result(zrv));
	}
	ztn->zn_refcnt++;
	*ztnp = ztn;
	NNI_LIST_INIT(&ztn->zn_eps, nni_zt_ep, ze_link);
	NNI_LIST_INIT(&ztn->zn_pipes, nni_zt_pipe, zp_link);
	nni_list_append(&nni_zt_nodes, ztn);
	nni_mtx_unlock(&nni_zt_lk);
	return (0);
}

static void
nni_zt_rele_node(nni_zt_node *ztn)
{
	nni_mtx_lock(&nni_zt_lk);
	ztn->zn_refcnt--;
	if (ztn->zn_refcnt == 0) {
		nni_mtx_lock(&nni_zt_lk);
		ztn->zn_closed = 1;
		nni_mtx_unlock(&nni_zt_lk);
		nni_list_remove(&nni_zt_nodes, ztn);
	}
	nni_mtx_unlock(&nni_zt_lk);

	nni_aio_cancel(&ztn->zn_rcv_aio, NNG_ECLOSED);
	nni_plat_udp_close(ztn->zn_udp);
	ZT_Node_delete(ztn->zn_znode);
	NNI_FREE_STRUCT(ztn);
}

static int
nni_zt_chkopt(int opt, const void *dat, size_t sz)
{
	switch (opt) {
	case NNG_OPT_RCVMAXSZ:
	// case NNG_OPT_TRANSPORT(NNG_TRAN_ZT, xyz):
	// XXX: We need a way to specify user options, like the path
	// for the identity files.
	default:
		return (NNG_ENOTSUP);
	}
}

static int
nni_zt_tran_init(void)
{
	nni_mtx_init(&nni_zt_lk);
	NNI_LIST_INIT(&nni_zt_nodes, nni_zt_node, zn_link);
	return (0);
}

static void
nni_zt_tran_fini(void)
{
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
}

static int
nni_zt_ep_init(void **epp, const char *url, nni_sock *sock, int mode)
{
	nni_zt_ep *ep;

	if ((ep = NNI_ALLOC_STRUCT(ep)) == NULL) {
		return (NNG_ENOMEM);
	}
	// URL parsing...
	// URL is form zt://<nwid>[/<remoteaddr>]:<port>
	// The <remoteaddr> part is required for remote dialers, but
	// is not used at all for listeners.  (We have no notion of binding
	// to different node addresses.)
	ep->ze_mode = mode;
	(void) snprintf(ep->ze_url, sizeof(url), "%s", url);
	*epp = ep;
	nni_mtx_init(&ep->ze_lk);
	return (0);
}

static void
nni_zt_ep_close(void *arg)
{
}

static int
nni_zt_ep_bind(void *arg)
{
	return (NNG_ENOTSUP);
}

static void
nni_zt_ep_finish(nni_zt_ep *ep)
{
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
	.ep_setopt  = NULL, // XXX: we need the ability to set homepath, etc.
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

#endif