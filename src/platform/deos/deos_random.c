//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.

#ifdef NNG_PLATFORM_DEOS

// FACE does not recognize the need for strong entropy (which means cryptography
// is basically impossibe to do securely.)  We settle for rand seeded by the
// realtime clock XOR monotonic clock.  Not great, but best we can do under
// these constraints.

#include <stdlib.h>
#include <time.h>

uint32_t
nni_random(void)
{
	static unsigned seed;
	struct timespec ts;

	// Hope is that the combination of monotonic clock, plus realtime clock, gives us a reasonable seed.

	// This may be slightly racy, but that's probably a *good* thing -- anything
	// that reduces determinism here is useful.  FACE should really adopt getentropy.

	clock_gettime(CLOCK_REALTIME, &ts);
	seed ^= (unsigned)(ts.tv_sec);
	seed ^= (unsigned)(ts.tv_nsec);
	clock_gettime(CLOCK_MONOTONIC, &ts);
	seed ^= (unsigned)(ts.tv_sec);
	seed ^= (unsigned)(ts.tv_nsec);

	return ((int)(rand_r(&seed)));
}

#endif
