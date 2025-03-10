//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifdef NNG_PLATFORM_DEOS

// We don't support actual name resolution. Only IP address literals are
// supported. Technically FACE would let us use getaddrinfo freely, but that
// would mean bringing in the platform sockets headers, and need to absolutely
// avoid this.
//
// For now we only support IPv4 resolution.

#include "core/defs.h"
#include "core/nng_impl.h"
#include "deos_impl.h"

#include <string.h>

void
nni_resolv(nni_resolv_item *item, nni_aio *aio)
{
	// We only support resolving IPv4 addresses for now.
	if (item->ri_family == NNG_AF_UNSPEC) {
		item->ri_family = NNG_AF_INET;
	}
	if (item->ri_family != NNG_AF_INET) {
		nni_aio_finish_error(aio, NNG_ENOTSUP);
	}
	if (item->ri_host == NULL) {
		if (item->ri_passive) {
			item->ri_sa->s_in.sa_addr = 0; // INADDR_ANY
			item->ri_sa->s_in.sa_port = nni_htons(item->ri_port);
			nng_aio_finish(aio, 0);
			return;
		}
		nng_aio_finish(aio, NNG_EADDRINVAL);
		return;
	}

	// NB: There is no validation of the address here.  We could
	// check it, but given limitations of this platform, it is
	// probably better not to bother.  Note also DEOS uses native
	// byte order in their socket addresses, but NNG follows the
	// more usual convention of network byte order here.  This will
	// result in a double conversion, but it ensures that NNG applications
	// behave as expected.
	item->ri_sa->s_in.sa_addr = nni_htonl(inet_addr(item->ri_host));
	item->ri_sa->s_in.sa_port = nni_htons(item->ri_port);
	if (item->ri_passive) {
		// Bad parse may return INADDR_NONE, which we
		// absolutely cannot bind to.  Just fail.
		if (item->ri_sa->s_in.sa_addr == 0xFFFFFFFF) {
			nng_aio_finish(aio, NNG_EADDRINVAL);
			return;
		}
	} else {
		// We cannot send to an unspecified address either,
		// but we might be able to send to the all 1's broadcast...
		if (item->ri_sa->s_in.sa_addr == 0) {
			nng_aio_finish(aio, NNG_EADDRINVAL);
			return;
		}
	}
	nng_aio_finish(aio, 0);
}

int
nni_parse_ip(const char *addr, nng_sockaddr *sa)
{
	sa->s_in.sa_addr = nni_htonl(inet_addr(addr));
}

int
nni_parse_ip_port(const char *addr, nni_sockaddr *sa)
{
	char  ipbuf[32]; // only IPv4 (15), plus ":port" (6)
	char *colon;

	strncpy(ipbuf, addr, sizeof(ipbuf));
	if ((colon = strchr(ipbuf, ":")) == NULL) {
		return (NNG_EADDRINVAL);
	}
	if (colon != NULL) {
		*colon = 0;
		colon++;
		sa->s_in.sa_port = nni_htons(atoi(colon));
	}
	sa->s_in.sa_addr = nni_htonl(inet_addr(ipbuf));
	return (0);
}

int
nni_get_port_by_name(const char *name, uint32_t *portp)
{
	*portp = atoi(name) & 0xffff;
	return (0);
}

#endif
