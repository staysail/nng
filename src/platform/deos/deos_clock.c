//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

// POSIX clock stuff.
#include "core/nng_impl.h"

#ifdef NNG_PLATFORM_DEOS

// DEOS uses clock_gettime.
// All APIs used here are permited in all FACE profiles.

#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

int
nni_time_get(uint64_t *sec, uint32_t *nsec)
{
	struct timespec ts;

	// Per standard, this doesn't fail if it is given correct inputs.
	// To keep it conformant to FACE, we avoid abort() and just assume success.
	clock_gettime(CLOCK_REALTIME, &ts);
	*sec  = ts.tv_sec;
	*nsec = ts.tv_nsec;
	return (0);
}

// Use POSIX realtime stuff
nni_time
nni_clock(void)
{
	struct timespec ts;
	nni_time        msec;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	msec = ts.tv_sec;
	msec *= 1000;
	msec += (ts.tv_nsec / 1000000);
	return (msec);
}

void
nni_msleep(nni_duration ms)
{
	struct timespec ts;

	ts.tv_sec  = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000;

	// Do this in a loop, so that interrupts don't actually wake
	// us.
	while (ts.tv_sec || ts.tv_nsec) {
		if (nanosleep(&ts, &ts) == 0) {
			break;
		}
	}
}

#endif // NNG_PLATFORM_DEOS
