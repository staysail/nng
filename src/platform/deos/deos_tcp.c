//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.

#ifdef NNG_PLATFORM_DEOS

// We don't support IPC (UNIX domain sockets) on this platform.
#include "core/nng_impl.h"

int
nni_tcp_listener_alloc(nng_stream_listener **lp, const nng_url *url)
{
	NNI_ARG_UNUSED(lp);
	NNI_ARG_UNUSED(url);
	return (NNG_ENOTSUP);
}

int
nni_tcp_dialer_init(nni_tcp_dialer **dp)
{
	NNI_ARG_UNUSED(dp);
	return (NNG_ENOTSUP);
}

void
nni_tcp_dialer_fini(nni_tcp_dialer *d)
{
	NNI_ARG_UNUSED(d);
}

void
nni_tcp_dialer_close(nni_tcp_dialer *d)
{
	NNI_ARG_UNUSED(d);
}

void
nni_tcp_dialer_stop(nni_tcp_dialer *d)
{
	NNI_ARG_UNUSED(d);
}

void
nni_tcp_dial(nni_tcp_dialer *d, const nng_sockaddr *sa, nni_aio *aio)
{
	// NB: There is no sensible way this should be called since
	// nni_tcp_dialer_init fails.
	NNI_ARG_UNUSED(d);
	NNI_ARG_UNUSED(sa);
	NNI_ARG_UNUSED(aio);
}

int
nni_tcp_dialer_set(
    nni_tcp_dialer *d, const char *n, const void *v, size_t sz, nni_type t)
{
	NNI_ARG_UNUSED(d);
	NNI_ARG_UNUSED(n);
	NNI_ARG_UNUSED(v);
	NNI_ARG_UNUSED(sz);
	NNI_ARG_UNUSED(t);
}

int
nni_tcp_dialer_get(
    nni_tcp_dialer *d, const char *n, void *v, size_t *sz, nni_type t)
{
	NNI_ARG_UNUSED(d);
	NNI_ARG_UNUSED(n);
	NNI_ARG_UNUSED(v);
	NNI_ARG_UNUSED(sz);
	NNI_ARG_UNUSED(t);
}

#endif
