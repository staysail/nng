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
			item->ri_sa->s_in.sa_port = htons(item->ri_port);
			nng_aio_finish(aio, 0);
			return;
		}
		nng_aio_finish_error(aio, NNG_EADDRINVAL);
		return;
	}

	char ip_str[16];
	if (strlen(item->ri_host > 15) {
		nng_aio_finish_error(aio, NNG_EADDRINVAL);
	}
	strncpy(ip_str, item->host, sizeof (item->host));
	ip_str[sizeof(ip_str)-1] = 0;

	char *sep = ".";
	char *last = NULL;
	char *str = item->ri_host;
	uint8_t bytes[4] ;

	item->ri_sa.sa_addr = 0;
	for (int i = 0; i < 4; i++) {
		char *octet = strtok_r(str, ".", &last);
		long  value;
		if (octet == NULL) {
			nng_aio_finish_error(aio, NNG_EADDRINVAL);
			return;
		}
		char *endp = NULL;
		value      = strtol(octet, &endp, 10);
		if ((*endp != 0) || (value < 0) || (value > 255)) {
			nng_aio_finish_error(aio, NNG_EADDRINVAL);
		}
		bytes[i] = value;
		str      = NULL;
	}

	// inherently network order
	memcpy(&item->ri_sa.sa_addr, bytes, 4);

	nni_aio_finish(aio, 0, 0);
}

#endif
