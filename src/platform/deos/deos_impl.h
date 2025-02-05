//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef PLATFORM_DEOS_IMPL_H
#define PLATFORM_DEOS_IMPL_H

#ifdef NNG_PLATFORM_DEOS
#include "core/defs.h"
#endif

#ifdef NNG_PLATFORM_POSIX_SOCKADDR
#include <sys/socket.h>
extern int    nni_posix_sockaddr2nn(nni_sockaddr *, const void *, size_t);
extern size_t nni_posix_nn2sockaddr(void *, const nni_sockaddr *);
#endif

extern int nni_plat_errno(int);

// Define types that this platform uses.
#include <pthread.h>

// These types are provided for here, to permit them to be directly inlined
// elsewhere.

struct nni_plat_mtx {
	pthread_mutex_t mtx;
};

#define NNI_MTX_INITIALIZER               \
	{                                 \
		PTHREAD_MUTEX_INITIALIZER \
	}

// No static form of CV initialization because of the need to use
// attributes to set the clock type.
struct nni_plat_cv {
	pthread_cond_t   cv;
	pthread_mutex_t *mtx;
};

struct nni_plat_thr {
	pthread_t tid;
	void (*func)(void *);
	void *arg;
};

struct nni_plat_flock {
	int fd;
};

#define NNG_PLATFORM_DIR_SEP "/"

#ifndef NNG_HAVE_STDATOMIC
#define NNG_HAVE_STDATOMIC
#endif // NNG_HAVE_STDATOMIC

#include <stdatomic.h>

struct nni_atomic_flag {
	atomic_flag f;
};

struct nni_atomic_int {
	atomic_int v;
};

struct nni_atomic_u64 {
	atomic_uint_fast64_t v;
};

struct nni_atomic_bool {
	atomic_bool v;
};

struct nni_atomic_ptr {
	atomic_uintptr_t v;
};

#else // NNG_HAVE_C11_ATOMIC
struct nni_atomic_flag {
	bool f;
};

struct nni_atomic_bool {
	bool b;
};

struct nni_atomic_int {
	int v;
};

struct nni_atomic_u64 {
	uint64_t v;
};

struct nni_atomic_ptr {
	void *v;
};

void nni_udp_init(void *arg);

#endif // PLATFORM_DEOS_IMPL_H
