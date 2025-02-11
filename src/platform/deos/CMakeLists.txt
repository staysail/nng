#
# Copyright 2025 Staysail Systems, Inc. <info@staystail.tech>
#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#

#  Deos.  This is a special platform that only supports UDP, and has
# a passing (only) resemblance to POSIX.

# We cannot use nng_sources_if because these tests don't go into
# the static library unless they also go into the dynamic.
if (NNG_PLATFORM_DEOS)

    # NB: This also depends on libethernet and libarch_
    nng_link_libraries(libdwsockets)

    # Unconditionally declare the following feature test macros.  These are
    # needed for some platforms (glibc and SunOS/illumos) and are harmless
    # on the others.
    nng_defines(_REENTRANT)
    nng_defines(_THREAD_SAFE)
    nng_defines(_POSIX_PTHREAD_SEMANTICS)

    nng_sources(
            deos_impl.h

            deos_alloc.c
            deos_atomic.c
            deos_clock.c
            deos_debug.c
            deos_file.c
            deos_ipc.c
            deos_pipe.c
            deos_random.c
            deos_resolv.c
            deos_sockaddr.c
            deos_sockfd.c
            deos_socketpair.c
            deos_tcp.c
            deos_thread.c
            deos_udp.c
    )

endif ()
