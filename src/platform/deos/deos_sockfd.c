//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"
#include "core/sockfd.h"

#ifdef NNG_PLATFORM_DEOS

int
nni_sfd_conn_alloc(nni_sfd_conn **cp, int fd)
{
	NNI_ARG_UNUSED(cp);
	NNI_ARG_UNUSED(fd);
	return (NNG_ENOTSUP);
}

void
nni_sfd_close_fd(int fd)
{
	NNI_ARG_UNUSED(fd);
}

#endif
