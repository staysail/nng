//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifdef NNG_PLATFORM_DEOS

#include "core/nng_impl.h"
#include <dw_socket.h>

// htonl and htons variants supplied as we don't know what the system offers.
// Note also, that DEOS uses *NATIVE* byte order in sockaddrs, unlike the
// entire rest of the BSD Socket using universe.

uint16_t
nni_htons(uint16_t in)
{
	in = ((in / 0x100) + ((in % 0x100) * 0x100));
	return (in);
}

uint32_t
nni_htonl(uint32_t in)
{
	in = ((in >> 24u) & 0xffu) | ((in >> 8u) & 0xff00u) |
	    ((in << 8u) & 0xff0000u) | ((in << 24u) & 0xff000000u);
	return (in);
}

// For DEOS, we only support IPv4 address (used for UDP only).

size_t
nni_deos_nn2sockaddr(void *sa, const nni_sockaddr *na)
{
	struct sockaddr_in *sin;
	size_t              sz;

	if ((sa == NULL) || (na == NULL)) {
		return (0);
	}
	switch (na->s_family) {
	case NNG_AF_INET:
		sin  = (void *) sa;
		nsin = &na->s_in;
		memset(sin, 0, sizeof(*sin));
		sin->sin_family      = AF_INET;
		sin->sin_port        = nni_ntohs(nsin->sa_port);
		sin->sin_addr.s_addr = nni_ntohl(nsin->sa_addr);
		return (sizeof(*sin));
	}
	return (0);
}

int
nni_deos_sockaddr2nn(nni_sockaddr *na, const void *sa, size_t sz)
{
	const struct sockaddr_in *sin;
	nng_sockaddr_in          *nsin;

	switch (((const struct sockaddr *) sa)->sa_family) {
	case AF_INET:
		if (sz < sizeof(*sin)) {
			return (-1);
		}
		sin             = (void *) sa;
		nsin            = &na->s_in;
		nsin->sa_family = NNG_AF_INET;
		nsin->sa_port   = nni_htons(sin->sin_port);
		nsin->sa_addr   = nni_htonl(sin->sin_addr.s_addr);
		break;

	default:
		// We should never see this - the OS should always be
		// specific about giving us either AF_INET or AF_INET6.
		// Other address families are not handled here.
		return (-1);
	}
	return (0);
}

#endif // NNG_PLATFORM_DEOS
