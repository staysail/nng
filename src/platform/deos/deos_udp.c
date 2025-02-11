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
#include "deos_impl.h"

#include <sys/errno.h>
#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// UDP support.

static nni_mtx  udp_lock = NNI_MTX_INITIALIZER;
static nni_list udp_list;

struct nni_plat_udp {
	int           udp_fd;
	nni_list      udp_recvq;
	nni_list      udp_sendq;
	nni_list_node udp_node;
};

static void
nni_udp_doerror(nni_plat_udp *udp, int rv)
{
	nni_aio *aio;

	while (((aio = nni_list_first(&udp->udp_recvq)) != NULL) ||
	    ((aio = nni_list_first(&udp->udp_sendq)) != NULL)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
}

static void
nni_udp_doclose(nni_plat_udp *udp)
{
	nni_udp_doerror(udp, NNG_ECLOSED);
}

// Deos needs a bouncebuf since it does not support sendmsg or recvmsg,
// but it is limited to strictly ordinary MTUs.
static uint8_t bouncebuf[1600];

static int
copy_to_bounce(nng_iov *iov, int niov)
{
	int      room = sizeof(bouncebuf);
	uint8_t *buf  = bouncebuf;
	int      len  = 0;

	for (int i = 0; i < niov && room; i++) {
		int n = iov[i].iov_len;
		if (n > room) {
			n = room;
		}
		memcpy(buf, iov[i].iov_buf, n);
		room -= n;
		buf += n;
		len += n;
	}
	return (len);
}

static void
copy_from_bounce(nng_iov *iov, int niov, int len)
{
	uint8_t *buf = bouncebuf;
	for (int i = 0; i < niov && len; i++) {
		int n = iov[i].iov_len;
		if (n > len) {
			n = len;
		}
		memcpy(iov[i].iov_buf, buf, n);
		len -= n;
		buf += n;
	}
}

static void
nni_udp_dorecv(nni_plat_udp *udp)
{
	nni_aio  *aio;
	nni_list *q = &udp->udp_recvq;
	// While we're able to recv, do so.
	while ((aio = nni_list_first(q)) != NULL) {
		unsigned           niov;
		nng_iov           *aiov;
		struct sockaddr_in ss;
		nng_sockaddr      *sa;
		int                rv  = 0;
		int                cnt = 0;

		nni_aio_get_iov(aio, &niov, &aiov);
		NNI_ASSERT(niov <= NNI_AIO_MAX_IOV);

		// Here we have to use a bounce buffer
		uint8_t  *buf;
		size_t    len;
		socklen_t salen;
		if (niov == 1) {
			buf = aiov[0].iov_buf;
			len = aiov[0].iov_len;
		} else {
			buf = bouncebuf;
			len = sizeof(bouncebuf);
		}
		salen = sizeof(ss);
		if ((cnt = recvfrom(udp->udp_fd, buf, len, 0, (void *) &ss,
		         &salen)) < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return;
			}
			rv = nni_plat_errno(errno);
		} else if ((sa = nni_aio_get_input(aio, 0)) != NULL) {
			nni_deos_sockaddr2nn(sa, (void *) &ss, salen);
		}
		if (niov != 1) {
			copy_from_bounce(aiov, niov, cnt);
		}
		nni_list_remove(q, aio);
		nni_aio_finish(aio, rv, cnt);
	}
}

static void
nni_udp_dosend(nni_plat_udp *udp)
{
	nni_aio  *aio;
	nni_list *q = &udp->udp_sendq;

	// While we're able to send, do so.
	while ((aio = nni_list_first(q)) != NULL) {
		struct sockaddr_in ss;

		int      salen;
		int      rv  = 0;
		int      cnt = 0;
		unsigned niov;
		nni_iov *aiov;

		nni_aio_get_iov(aio, &niov, &aiov);
		NNI_ASSERT(niov <= NNI_AIO_MAX_IOV);
		if ((salen = nni_deos_nn2sockaddr(
		         &ss, nni_aio_get_input(aio, 0))) < 1) {
			rv = NNG_EADDRINVAL;
		} else {
			uint8_t *buf;
			size_t   len;
			if (niov == 1) {
				buf = aiov[0].iov_buf;
				len = aiov[0].iov_len;
			} else {
				len = copy_to_bounce(aiov, niov);
				buf = bouncebuf;
			}
			cnt = sendto(
			    udp->udp_fd, buf, len, 0, (void *) &ss, salen);
			if (cnt < 0) {
				if ((errno == EAGAIN) ||
				    (errno == EWOULDBLOCK)) {
					// Cannot send now, leave.
					return;
				}
				rv = nni_plat_errno(errno);
			}
		}

		nni_list_remove(q, aio);
		nni_aio_finish(aio, rv, cnt);
	}
}

int
nni_plat_udp_open(nni_plat_udp **upp, nni_sockaddr *bindaddr)
{
	nni_plat_udp      *udp;
	int                salen;
	struct sockaddr_in sa;
	int                rv;

	if (bindaddr == NULL) {
		return (NNG_EADDRINVAL);
	}
	switch (bindaddr->s_family) {
	case NNG_AF_INET:
		break;
	default:
		return (NNG_EADDRINVAL);
	}
	salen = nni_deos_nn2sockaddr(&sa, bindaddr);
	NNI_ASSERT(salen > 1);

	// UDP opens can actually run synchronously.
	if ((udp = NNI_ALLOC_STRUCT(udp)) == NULL) {
		return (NNG_ENOMEM);
	}
	nni_aio_list_init(&udp->udp_recvq);
	nni_aio_list_init(&udp->udp_sendq);
	NNI_LIST_NODE_INIT(&udp->udp_node);

	udp->udp_fd = socket(sa.sin_family, SOCK_DGRAM, IPPROTO_UDP);
	if (udp->udp_fd < 0) {
		rv = nni_plat_errno(errno);
		NNI_FREE_STRUCT(udp);
		return (rv);
	}

	if (bind(udp->udp_fd, (void *) &sa, salen) != 0) {
		rv = nni_plat_errno(errno);
		(void) close(udp->udp_fd);
		NNI_FREE_STRUCT(udp);
		return (rv);
	}

	nni_mtx_lock(&udp_lock);
	nni_list_append(&udp_list, udp);
	nni_mtx_unlock(&udp_lock);

	*upp = udp;
	return (0);
}

void
nni_plat_udp_close(nni_plat_udp *udp)
{
	// De-register the UDP if it is waiting for a recv.
	nni_mtx_lock(&udp_lock);
	nni_list_remove(&udp_list, udp);
	nni_mtx_unlock(&udp_lock);

	nni_udp_doclose(udp);

	(void) close(udp->udp_fd);
	NNI_FREE_STRUCT(udp);
}

void
nni_plat_udp_cancel(nni_aio *aio, void *arg, int rv)
{
	nni_plat_udp *udp = arg;

	nni_mtx_lock(&udp_lock);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&udp_lock);
}

void
nni_plat_udp_recv(nni_plat_udp *udp, nni_aio *aio)
{
	int rv;
	nni_aio_reset(aio);
	nni_mtx_lock(&udp_lock);
	if (!nni_aio_start(aio, nni_plat_udp_cancel, udp)) {
		nni_mtx_unlock(&udp_lock);
		return;
	}
	nni_list_append(&udp->udp_recvq, aio);
	nni_mtx_unlock(&udp_lock);
}

void
nni_plat_udp_send(nni_plat_udp *udp, nni_aio *aio)
{
	int rv;
	nni_aio_reset(aio);
	nni_mtx_lock(&udp_lock);
	if (!nni_aio_start(aio, nni_plat_udp_cancel, udp)) {
		nni_mtx_unlock(&udp_lock);
		return;
	}
	nni_list_append(&udp->udp_sendq, aio);
	nni_mtx_unlock(&udp_lock);
}

int
nni_plat_udp_sockname(nni_plat_udp *udp, nni_sockaddr *sa)
{
	NNI_ARG_UNUSED(udp);
	NNI_ARG_UNUSED(sa);
	return (NNG_ENOTSUP);
}

int
nni_plat_udp_multicast_membership(
    nni_plat_udp *udp, nni_sockaddr *sa, bool join)
{
	NNI_ARG_UNUSED(udp);
	NNI_ARG_UNUSED(sa);
	NNI_ARG_UNUSED(join);
	return (NNG_ENOTSUP);
}

// This is a fairly simple thread.  It never shuts down!
static void *
udp_thread(void *arg)
{
	struct timespec ts;
	NNI_ARG_UNUSED(arg);
	for (;;) {
		nni_plat_udp *udp;
		nni_mtx_lock(&udp_lock);
		NNI_LIST_FOREACH (&udp_list, udp) {
			nni_udp_dorecv(udp);
			nni_udp_dosend(udp);
		}
		nni_mtx_unlock(&udp_lock);

		// We need to sleep a little bit to let other
		// threads access the lists, or they will starve.
		// 10 microseconds is a rough compromise, and in all
		// likelihood, this will be just the remainder of the tick.
		ts.tv_sec  = 0;
		ts.tv_nsec = 10000; // 10 microsends
		nanolseep(&ts, NULL);
	}
}

void
nni_udp_init(void *arg)
{
	pthread_t thr;
	NNI_LIST_INIT(&udp_list, nni_plat_udp, udp_node);
	pthread_create(&thp, NULL, udp_thread, NULL);
}

#endif // NNG_PLATFORM_POSIX
