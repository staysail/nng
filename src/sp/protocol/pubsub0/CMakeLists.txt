#
# Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
# Copyright 2018 Capitar IT Group BV <info@capitar.com>
#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#

#  Pub/Sub protocol
nng_directory(pubsub0)

nng_sources_if(NNG_PROTO_PUB0 pub.c)
nng_defines_if(NNG_PROTO_PUB0 NNG_HAVE_PUB0)

nng_sources_if(NNG_PROTO_SUB0 sub.c xsub.c)
nng_defines_if(NNG_PROTO_SUB0 NNG_HAVE_SUB0)

nng_test(pub_test)
nng_test(sub_test)
nng_test(xsub_test)
