//
// Copyright 2024 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef PLATFORM_POSIX_POLLQ_POLL_H
#define PLATFORM_POSIX_POLLQ_POLL_H

#include <poll.h>

// nni_posix_pfd is the handle used by the poller.  It's internals are private
// to the poller.
struct nni_posix_pfd {
	struct nni_posix_pollq *pq;
	int                     fd;
	nni_list_node           node;
	nni_list_node           reap;
	nni_mtx                 mtx;
	unsigned                events;
	nni_posix_pfd_cb        cb;
	void                   *arg;
	bool                    reaped;
};

#define NNI_POLL_IN ((unsigned) POLLIN)
#define NNI_POLL_OUT ((unsigned) POLLOUT)
#define NNI_POLL_HUP ((unsigned) POLLHUP)
#define NNI_POLL_ERR ((unsigned) POLLERR)
#define NNI_POLL_INVAL ((unsigned) POLLNVAL)

#endif // PLATFORM_POSIX_POLLQ_POLL_H
