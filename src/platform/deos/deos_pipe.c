//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.

#ifdef NNG_PLATFORM_DEOS

// These interfaces are not supported by the platform, and we do not
// need them as they are only used for integration into poll or select
// loops, which is not a feature we support for DEOS.

#include "core/nng_impl.h"

int
nni_plat_pipe_open(int *wfd, int *rfd)
{
	NNI_ARG_UNUSED(wfd);
	NNI_ARG_UNUSED(rfd);
	return (NNG_ENOTSUP);
}

void
nni_plat_pipe_raise(int wfd)
{
	NNI_ARG_UNUSED(wfd);
}

void
nni_plat_pipe_clear(int wfd)
{
	NNI_ARG_UNUSED(wfd);
}

void
nni_plat_pipe_close(int wfd, int rfd)
{
	NNI_ARG_UNUSED(wfd);
	NNI_ARG_UNUSED(rfd);
}

#endif // NNG_PLATFORM_DEOS
