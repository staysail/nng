//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifdef NNG_PLATFORM_DEOS

nng_err
nni_socket_pair(int fds[2])
{
	NNI_ARG_UNUSED(fds);
	return (NNG_ENOTSUP);
}

#endif
