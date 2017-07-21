//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"

// ZeroTier Transport.  This sits on the ZeroTier L2 network, which itself
// is implemented on top of L3 (mostly UDP).  This requires the 3rd party
// libzerotiercore library (which is GPL3!) and platform specific UDP
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

struct nni_zt_pipe {
	const char *addr;
	uint16_t    peer;
	uint16_t    proto;
	size_t      rcvmax;
	nni_aio *   user_txaio;
	nni_aio *   user_rxaio;
	nni_aio *   user_negaio;

	// XXX: fraglist
	nni_sockaddr remaddr;
	nni_sockaddr locaddr;

	nni_aio  txaio;
	nni_aio  rxaio;
	nni_msg *rxmsg;
	nni_mtx  mtx;
};

struct nni_zt_ep {
	char         addr[NNG_MAXADDRLEN + 1];
	nni_sockaddr locaddr;
	int          closed;
	uint16_t     proto;
	size_t       rcvmax;
	nni_aio      aio;
	nni_aio *    user_aio;
	nni_mtx      mtx;
};

static int
nni_zt_tran_init(void)
{
	// XXX: This needs to set up the initial global listener.  It
	// quite likely shouldn't actually start listening yet, but
	// everything should be ready for when the first endpoint listen
	// comes in.
	return (NNG_ENOTSUP);
}

static void
nni_zt_tran_fini(void)
{
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

	return (pipe->peer);
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
}

static int
nni_zt_ep_init(void **epp, const char *url, nni_sock *sock, int mode)
{
	return (NNG_ENOTSUP);
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
	.ep_setopt  = NULL,
	.ep_getopt  = NULL,
};

// This is the ZeroTier transport linkage, and should be the only global
// symbol in this entire file.
struct nni_tran nni_zt_tran = {
	.tran_scheme = "zt",
	.tran_ep     = &nni_zt_ep_ops,
	.tran_pipe   = &nni_zt_pipe_ops,
	.tran_init   = nni_zt_tran_init,
	.tran_fini   = nni_zt_tran_fini,
};
