#
# Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
# Copyright 2018 Capitar IT Group BV <info@capitar.com>
#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#

# TLS transport
nng_directory(tls)

if (NNG_TRANSPORT_TLS)
    nng_sources(tls.c)
    nng_defines(NNG_TRANSPORT_TLS)
    nng_test(tls_tran_test)
endif()
