//
// Copyright 2025 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
// Copyright 2019 Devolutions <info@devolutions.net>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nng_impl.h"

#include "tls_common.h"
#include "tls_engine.h"

// NNG_TLS_MAX_SEND_SIZE limits the amount of data we will buffer for sending,
// exerting back-pressure if this size is exceeded.  The 16K is aligned to the
// maximum TLS record size.
#ifndef NNG_TLS_MAX_SEND_SIZE
#define NNG_TLS_MAX_SEND_SIZE 16384
#endif

// NNG_TLS_MAX_RECV_SIZE limits the amount of data we will receive in a single
// operation.  As we have to buffer data, this drives the size of our
// intermediary buffer.  The 16K is aligned to the maximum TLS record size.
#ifndef NNG_TLS_MAX_RECV_SIZE
#define NNG_TLS_MAX_RECV_SIZE 16384
#endif

// This file contains common code for TLS, and is only compiled if we
// have TLS configured in the system.  In particular, this provides the
// parts of TLS support that are invariant relative to different TLS
// libraries, such as dialer and listener support.

static nni_atomic_ptr tls_engine;

static void tls_bio_send_cb(void *arg);
static void tls_bio_recv_cb(void *arg);
static void tls_do_send(nni_tls_conn *);
static void tls_do_recv(nni_tls_conn *);
static void tls_bio_send_start(nni_tls_conn *);
static void tls_bio_error(nni_tls_conn *, nng_err);

static void
tls_cancel(nni_aio *aio, void *arg, nng_err rv)
{
	nni_tls_conn *conn = arg;
	nni_mtx_lock(&conn->lock);
	if (aio == nni_list_first(&conn->recv_queue)) {
		nni_aio_abort(&conn->bio_recv, rv);
	} else if (aio == nni_list_first(&conn->send_queue)) {
		nni_aio_abort(&conn->bio_send, rv);
	} else if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&conn->lock);
}

// tls_send implements the upper layer send operation.
void
nni_tls_send(nni_tls_conn *conn, nni_aio *aio)
{
	nni_aio_reset(aio);
	nni_mtx_lock(&conn->lock);
	if (!nni_aio_start(aio, tls_cancel, conn)) {
		nni_mtx_unlock(&conn->lock);
		return;
	}
	if (conn->closed) {
		nni_mtx_unlock(&conn->lock);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}
	nni_list_append(&conn->send_queue, aio);
	tls_do_send(conn);
	nni_mtx_unlock(&conn->lock);
}

void
nni_tls_recv(nni_tls_conn *conn, nni_aio *aio)
{
	nni_aio_reset(aio);
	nni_mtx_lock(&conn->lock);
	if (!nni_aio_start(aio, tls_cancel, conn)) {
		nni_mtx_unlock(&conn->lock);
		return;
	}
	if (conn->closed) {
		nni_mtx_unlock(&conn->lock);
		nni_aio_finish_error(aio, NNG_ECLOSED);
		return;
	}

	nni_list_append(&conn->recv_queue, aio);
	tls_do_recv(conn);
	nni_mtx_unlock(&conn->lock);
}

void
nni_tls_close(nni_tls_conn *conn)
{
	if (!nni_atomic_flag_test_and_set(&conn->did_close)) {
		nni_mtx_lock(&conn->lock);
		conn->ops.close((void *) (conn + 1));
		nni_mtx_unlock(&conn->lock);
		nni_mtx_lock(&conn->bio_lock);
		tls_bio_error(conn, NNG_ECLOSED);
		nni_mtx_unlock(&conn->bio_lock);
	}
}

void
nni_tls_stop(nni_tls_conn *conn)
{
	nni_tls_close(conn);
	if (conn->bio != NULL) {
		conn->bio_ops.bio_stop(conn->bio);
	}
	nni_aio_stop(&conn->bio_send);
	nni_aio_stop(&conn->bio_recv);
}

bool
nni_tls_verified(nni_tls_conn *conn)
{
	bool result;
	nni_mtx_lock(&conn->lock);
	result = conn->ops.verified((void *) (conn + 1));
	nni_mtx_unlock(&conn->lock);
	return result;
}

const char *
nni_tls_peer_cn(nni_tls_conn *conn)
{
	const char *result;
	nni_mtx_lock(&conn->lock);
	result = conn->ops.peer_cn((void *) (conn + 1));
	nni_mtx_unlock(&conn->lock);
	return result;
}

int
nni_tls_init(nni_tls_conn *conn, nng_tls_config *cfg)
{
	const nng_tls_engine *eng;

	eng = cfg->engine;

	nni_mtx_lock(&cfg->lock);
	cfg->busy = true;
	nni_mtx_unlock(&cfg->lock);

	if (((conn->bio_send_buf = nni_zalloc(NNG_TLS_MAX_SEND_SIZE)) ==
	        NULL) ||
	    ((conn->bio_recv_buf = nni_zalloc(NNG_TLS_MAX_RECV_SIZE)) ==
	        NULL)) {
		return (NNG_ENOMEM);
	}
	conn->ops    = *eng->conn_ops;
	conn->engine = eng;
	conn->cfg    = cfg;

	nni_aio_init(&conn->bio_recv, tls_bio_recv_cb, conn);
	nni_aio_init(&conn->bio_send, tls_bio_send_cb, conn);
	nni_aio_list_init(&conn->send_queue);
	nni_aio_list_init(&conn->recv_queue);
	nni_mtx_init(&conn->lock);
	nni_mtx_init(&conn->bio_lock);
	nni_aio_set_timeout(&conn->bio_send, NNG_DURATION_INFINITE);
	nni_aio_set_timeout(&conn->bio_recv, NNG_DURATION_INFINITE);
	nni_atomic_flag_reset(&conn->did_close);

	nng_tls_config_hold(cfg);
	return (0);
}

void
nni_tls_fini(nni_tls_conn *conn)
{
	nni_tls_stop(conn);
	conn->ops.fini((void *) (conn + 1));
	nni_aio_fini(&conn->bio_send);
	nni_aio_fini(&conn->bio_recv);
	if (conn->cfg != NULL) {
		nng_tls_config_free(conn->cfg); // this drops our hold on it
	}
	if (conn->bio_send_buf != NULL) {
		nni_free(conn->bio_send_buf, NNG_TLS_MAX_SEND_SIZE);
	}
	if (conn->bio_recv_buf != NULL) {
		nni_free(conn->bio_recv_buf, NNG_TLS_MAX_RECV_SIZE);
	}
	if (conn->bio != NULL) {
		conn->bio_ops.bio_free(conn->bio);
	}
	nni_mtx_fini(&conn->bio_lock);
	nni_mtx_fini(&conn->lock);
}

int
nni_tls_start(nni_tls_conn *conn, const nni_tls_bio_ops *biops, void *bio,
    const nng_sockaddr *sa)
{
	nng_tls_engine_config *cfg;
	nng_tls_engine_conn   *econ;

	cfg  = (void *) (conn->cfg + 1);
	econ = (void *) (conn + 1);

	conn->bio_ops = *biops;
	conn->bio     = bio;

	return (conn->ops.init(econ, conn, cfg, sa));
}

static void
tls_conn_err(nni_tls_conn *conn, nng_err rv)
{
	nni_aio *aio;
	nni_mtx_lock(&conn->lock);
	while (((aio = nni_list_first(&conn->send_queue)) != NULL) ||
	    ((aio = nni_list_first(&conn->recv_queue)) != NULL)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&conn->lock);
}
static void
tls_bio_error(nni_tls_conn *conn, nng_err rv)
{
	// An error here is fatal.  Shut it all down.
	if (!conn->bio_closed) {
		conn->bio_closed = true;
		conn->bio_err    = rv;
		if (conn->bio_send_active)
			nni_aio_abort(&conn->bio_send, conn->bio_err);
		if (conn->bio_recv_pend)
			nni_aio_abort(&conn->bio_recv, conn->bio_err);
		if (conn->bio != NULL) {
			conn->bio_ops.bio_close(conn->bio);
		}

		nni_aio_close(&conn->bio_send);
		nni_aio_close(&conn->bio_recv);
	}
}

static nng_err
tls_handshake(nni_tls_conn *conn)
{
	int rv;
	if (conn->hs_done) {
		return (NNG_OK);
	}
	rv = conn->ops.handshake((void *) (conn + 1));
	if (rv == NNG_EAGAIN) {
		// We need more data.
		return (rv);
	}
	if (rv == NNG_OK) {
		conn->hs_done = true;
		return (rv);
	}
	return (rv);
}

static void
tls_do_recv(nni_tls_conn *conn)
{
	nni_aio *aio;

	while ((aio = nni_list_first(&conn->recv_queue)) != NULL) {
		uint8_t *buf = NULL;
		size_t   len = 0;
		nni_iov *iov;
		unsigned nio;
		int      rv;

		nni_aio_get_iov(aio, &nio, &iov);

		for (unsigned i = 0; i < nio; i++) {
			if (iov[i].iov_len != 0) {
				buf = iov[i].iov_buf;
				len = iov[i].iov_len;
				break;
			}
		}
		if (len == 0 || buf == NULL) {
			// Caller has asked to receive "nothing".
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, NNG_EINVAL);
			continue;
		}

		rv = conn->ops.recv((void *) (conn + 1), buf, &len);
		if (rv == NNG_EAGAIN) {
			// Nothing more we can do, the engine doesn't
			// have anything else for us (yet).
			return;
		}

		// Unlike the send side, we want to return back to the
		// caller as *soon* as we have some data.
		nni_aio_list_remove(aio);

		if (rv != NNG_OK) {
			nni_aio_finish_error(aio, rv);
		} else {
			nni_aio_finish(aio, 0, len);
		}
	}
}

// tls_do_send attempts to send user data.
static void
tls_do_send(nni_tls_conn *conn)
{
	nni_aio *aio;

	while ((aio = nni_list_first(&conn->send_queue)) != NULL) {
		uint8_t *buf = NULL;
		size_t   len = 0;
		nni_iov *iov;
		unsigned nio;
		int      rv;

		nni_aio_get_iov(aio, &nio, &iov);

		for (unsigned i = 0; i < nio; i++) {
			if (iov[i].iov_len != 0) {
				buf = iov[i].iov_buf;
				len = iov[i].iov_len;
				break;
			}
		}
		if (len == 0 || buf == NULL) {
			nni_aio_list_remove(aio);
			// Presumably this means we've completed this
			// one, lets preserve the count, and move to the
			// next.
			nni_aio_finish(aio, 0, nni_aio_count(aio));
			continue;
		}

		// Ask the engine to send.
		rv = conn->ops.send((void *) (conn + 1), buf, &len);
		if (rv == NNG_EAGAIN) {
			// Can't send any more, wait for callback.
			return;
		}

		if (rv != 0) {
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, rv);
		} else {
			nni_aio_list_remove(aio);
			nni_aio_finish(aio, 0, len);
		}
	}
}

nng_err
nni_tls_run(nni_tls_conn *conn)
{
	nni_aio *aio;
	nng_err  rv;
	nni_mtx_lock(&conn->lock);
	switch ((rv = tls_handshake(conn))) {
	case NNG_OK:
		tls_do_recv(conn);
		tls_do_send(conn);
		break;
	case NNG_EAGAIN:
		break;
	default:
		while (((aio = nni_list_first(&conn->send_queue)) != NULL) ||
		    ((aio = nni_list_first(&conn->recv_queue)) != NULL)) {
			nni_aio_list_remove(aio);
			nni_aio_finish_error(aio, rv);
		}
		break;
	}
	nni_mtx_unlock(&conn->lock);
	return (rv);
}

static void
tls_bio_send_cb(void *arg)
{
	nni_tls_conn *conn = arg;
	nng_aio      *aio  = &conn->bio_send;
	int           rv;
	size_t        count;

	nni_mtx_lock(&conn->bio_lock);
	conn->bio_send_active = false;

	if ((rv = nni_aio_result(aio)) != 0) {
		tls_bio_error(conn, rv);
		nni_mtx_unlock(&conn->bio_lock);

		tls_conn_err(conn, rv);
		return;
	}

	count = nni_aio_count(aio);
	NNI_ASSERT(count <= conn->bio_send_len);
	conn->bio_send_len -= count;
	conn->bio_send_tail += count;
	conn->bio_send_tail %= NNG_TLS_MAX_SEND_SIZE;
	tls_bio_send_start(conn);
	nni_mtx_unlock(&conn->bio_lock);

	nni_tls_run(conn);
}

static void
tls_bio_recv_cb(void *arg)
{
	nni_tls_conn *conn = arg;
	nni_aio      *aio  = &conn->bio_recv;
	int           rv;

	nni_mtx_lock(&conn->bio_lock);
	conn->bio_recv_pend = false;
	if ((rv = nni_aio_result(aio)) != 0) {
		tls_bio_error(conn, rv);
		nni_mtx_unlock(&conn->bio_lock);
		tls_conn_err(conn, rv);
		return;
	}

	NNI_ASSERT(conn->bio_recv_len == 0);
	NNI_ASSERT(conn->bio_recv_off == 0);
	conn->bio_recv_len = nni_aio_count(aio);
	nni_mtx_unlock(&conn->bio_lock);

	nni_tls_run(conn);
}

static void
tls_bio_recv_start(nni_tls_conn *conn)
{
	nng_iov iov;

	if (conn->bio_recv_len != 0) {
		// We already have data in the buffer.
		return;
	}
	if (conn->bio_recv_pend) {
		// Already have a receive in flight.
		return;
	}
	if (conn->bio_closed) {
		return;
	}
	conn->bio_recv_off = 0;
	iov.iov_len        = NNG_TLS_MAX_RECV_SIZE;
	iov.iov_buf        = conn->bio_recv_buf;

	conn->bio_recv_pend = true;
	nng_aio_set_iov(&conn->bio_recv, 1, &iov);

	conn->bio_ops.bio_recv(conn->bio, &conn->bio_recv);
}

static void
tls_bio_send_start(nni_tls_conn *conn)
{
	nni_iov  iov[2];
	unsigned nio = 0;
	size_t   len;
	size_t   tail;
	size_t   head;

	if (conn->bio_send_active) {
		return;
	}
	if (conn->bio_send_len == 0) {
		return;
	}
	if (conn->bio_closed) {
		return;
	}
	len  = conn->bio_send_len;
	head = conn->bio_send_head;
	tail = conn->bio_send_tail;

	while (len > 0) {
		size_t cnt;
		NNI_ASSERT(nio < 2);
		if (tail < head) {
			cnt = head - tail;
		} else {
			cnt = NNG_TLS_MAX_SEND_SIZE - tail;
		}
		if (cnt > len) {
			cnt = len;
		}
		iov[nio].iov_buf = conn->bio_send_buf + tail;
		iov[nio].iov_len = cnt;
		len -= cnt;
		tail += cnt;
		tail %= NNG_TLS_MAX_SEND_SIZE;
		nio++;
	}
	conn->bio_send_active = true;
	nni_aio_set_iov(&conn->bio_send, nio, iov);
	conn->bio_ops.bio_send(conn->bio, &conn->bio_send);
}

int
nng_tls_engine_send(void *arg, const uint8_t *buf, size_t *szp)
{
	nni_tls_conn *conn = arg;
	size_t        len  = *szp;
	size_t        head;
	size_t        tail;
	size_t        space;
	size_t        cnt;

	nni_mtx_lock(&conn->bio_lock);
	head  = conn->bio_send_head;
	tail  = conn->bio_send_tail;
	space = NNG_TLS_MAX_SEND_SIZE - conn->bio_send_len;

	if (space == 0) {
		nni_mtx_unlock(&conn->bio_lock);
		return (NNG_EAGAIN);
	}

	if (len > space) {
		len = space;
	}

	// We are committed at this point to sending out len bytes.
	// Update this now, so that we can use len to update.
	*szp = len;
	conn->bio_send_len += len;
	NNI_ASSERT(conn->bio_send_len <= NNG_TLS_MAX_SEND_SIZE);

	while (len > 0) {
		if (head >= tail) {
			cnt = NNG_TLS_MAX_SEND_SIZE - head;
		} else {
			cnt = tail - head;
		}
		if (cnt > len) {
			cnt = len;
		}

		memcpy(conn->bio_send_buf + head, buf, cnt);
		buf += cnt;
		head += cnt;
		head %= NNG_TLS_MAX_SEND_SIZE;
		len -= cnt;
	}

	conn->bio_send_head = head;

	tls_bio_send_start(conn);
	nni_mtx_unlock(&conn->bio_lock);
	return (0);
}

int
nng_tls_engine_recv(void *arg, uint8_t *buf, size_t *szp)
{
	nni_tls_conn *conn = arg;
	size_t        len  = *szp;

	nni_mtx_lock(&conn->bio_lock);
	if (conn->bio_recv_len == 0) {
		tls_bio_recv_start(conn);
		nni_mtx_unlock(&conn->bio_lock);
		return (NNG_EAGAIN);
	}
	if (len > conn->bio_recv_len) {
		len = conn->bio_recv_len;
	}
	memcpy(buf, conn->bio_recv_buf + conn->bio_recv_off, len);
	conn->bio_recv_off += len;
	conn->bio_recv_len -= len;

	// If we still have data left in the buffer, then the following
	// call is a no-op.
	tls_bio_recv_start(conn);
	nni_mtx_unlock(&conn->bio_lock);

	*szp = len;
	return (0);
}

int
nng_tls_config_cert_key_file(
    nng_tls_config *cfg, const char *path, const char *pass)
{
	int    rv;
	void  *data;
	size_t size;
	char  *pem;

	if ((rv = nni_file_get(path, &data, &size)) != 0) {
		return (rv);
	}
	if ((pem = nni_zalloc(size + 1)) == NULL) {
		nni_free(data, size);
		return (NNG_ENOMEM);
	}
	memcpy(pem, data, size);
	nni_free(data, size);
	rv = nng_tls_config_own_cert(cfg, pem, pem, pass);
	nni_free(pem, size + 1);
	return (rv);
}

int
nng_tls_config_ca_file(nng_tls_config *cfg, const char *path)
{
	int    rv;
	void  *data;
	size_t size;
	char  *pem;

	if ((rv = nni_file_get(path, &data, &size)) != 0) {
		return (rv);
	}
	if ((pem = nni_zalloc(size + 1)) == NULL) {
		nni_free(data, size);
		return (NNG_ENOMEM);
	}
	memcpy(pem, data, size);
	nni_free(data, size);
	if (strstr(pem, "-----BEGIN X509 CRL-----") != NULL) {
		rv = nng_tls_config_ca_chain(cfg, pem, pem);
	} else {
		rv = nng_tls_config_ca_chain(cfg, pem, NULL);
	}
	nni_free(pem, size + 1);
	return (rv);
}

int
nng_tls_config_version(
    nng_tls_config *cfg, nng_tls_version min_ver, nng_tls_version max_ver)
{
	int rv;

	nni_mtx_lock(&cfg->lock);
	if (cfg->busy) {
		rv = NNG_EBUSY;
	} else {
		rv = cfg->ops.version((void *) (cfg + 1), min_ver, max_ver);
	}
	nni_mtx_unlock(&cfg->lock);
	return (rv);
}

int
nng_tls_config_server_name(nng_tls_config *cfg, const char *name)
{
	int rv;

	nni_mtx_lock(&cfg->lock);
	if (cfg->busy) {
		rv = NNG_EBUSY;
	} else {
		rv = cfg->ops.server((void *) (cfg + 1), name);
	}
	nni_mtx_unlock(&cfg->lock);
	return (rv);
}

int
nng_tls_config_ca_chain(
    nng_tls_config *cfg, const char *certs, const char *crl)
{
	int rv;

	nni_mtx_lock(&cfg->lock);
	if (cfg->busy) {
		rv = NNG_EBUSY;
	} else {
		rv = cfg->ops.ca_chain((void *) (cfg + 1), certs, crl);
	}
	nni_mtx_unlock(&cfg->lock);
	return (rv);
}

int
nng_tls_config_own_cert(
    nng_tls_config *cfg, const char *cert, const char *key, const char *pass)
{
	int rv;
	nni_mtx_lock(&cfg->lock);
	// NB: we cannot set the key if we already have done so.
	// This is because some lower layers create a "stack" of keys
	// and certificates, and this will almost certainly lead to confusion.
	if (cfg->busy || cfg->key_is_set) {
		rv = NNG_EBUSY;
	} else {
		rv = cfg->ops.own_cert((void *) (cfg + 1), cert, key, pass);
		if (rv == 0) {
			cfg->key_is_set = true;
		}
	}
	nni_mtx_unlock(&cfg->lock);
	return (rv);
}

int
nng_tls_config_psk(nng_tls_config *cfg, const char *identity,
    const uint8_t *key, size_t key_len)
{
	int rv;
	nni_mtx_lock(&cfg->lock);
	if (cfg->busy) {
		rv = NNG_EBUSY;
	} else {
		rv = cfg->ops.psk((void *) (cfg + 1), identity, key, key_len);
	}
	nni_mtx_unlock(&cfg->lock);
	return (rv);
}

int
nng_tls_config_auth_mode(nng_tls_config *cfg, nng_tls_auth_mode mode)
{
	int rv;

	nni_mtx_lock(&cfg->lock);
	if (cfg->busy) {
		rv = NNG_EBUSY;
	} else {
		rv = cfg->ops.auth((void *) (cfg + 1), mode);
	}
	nni_mtx_unlock(&cfg->lock);
	return (rv);
}

int
nng_tls_config_alloc(nng_tls_config **cfg_p, nng_tls_mode mode)
{
	nng_tls_config       *cfg;
	const nng_tls_engine *eng;
	size_t                size;
	int                   rv;

	eng = nni_atomic_get_ptr(&tls_engine);

	if (eng == NULL) {
		return (NNG_ENOTSUP);
	}

	size = NNI_ALIGN_UP(sizeof(*cfg)) + eng->config_ops->size;

	if ((cfg = nni_zalloc(size)) == NULL) {
		return (NNG_ENOMEM);
	}

	cfg->ops    = *eng->config_ops;
	cfg->size   = size;
	cfg->engine = eng;
	cfg->ref    = 1;
	cfg->busy   = false;
	nni_mtx_init(&cfg->lock);

	if ((rv = cfg->ops.init((void *) (cfg + 1), mode)) != 0) {
		nni_free(cfg, cfg->size);
		return (rv);
	}
	*cfg_p = cfg;
	return (0);
}

void
nng_tls_config_free(nng_tls_config *cfg)
{
	nni_mtx_lock(&cfg->lock);
	cfg->ref--;
	if (cfg->ref != 0) {
		nni_mtx_unlock(&cfg->lock);
		return;
	}
	nni_mtx_unlock(&cfg->lock);
	nni_mtx_fini(&cfg->lock);
	cfg->ops.fini((void *) (cfg + 1));
	nni_free(cfg, cfg->size);
}

void
nng_tls_config_hold(nng_tls_config *cfg)
{
	nni_mtx_lock(&cfg->lock);
	cfg->ref++;
	nni_mtx_unlock(&cfg->lock);
}

const char *
nng_tls_engine_name(void)
{
	const nng_tls_engine *eng;

	eng = nni_atomic_get_ptr(&tls_engine);

	return (eng == NULL ? "none" : eng->name);
}

const char *
nng_tls_engine_description(void)
{
	const nng_tls_engine *eng;

	eng = nni_atomic_get_ptr(&tls_engine);

	return (eng == NULL ? "" : eng->description);
}

bool
nng_tls_engine_fips_mode(void)
{
	const nng_tls_engine *eng;

	eng = nni_atomic_get_ptr(&tls_engine);

	return (eng == NULL ? false : eng->fips_mode);
}

int
nng_tls_engine_register(const nng_tls_engine *engine)
{
	if (engine->version != NNG_TLS_ENGINE_VERSION) {
		nng_log_err("NNG-TLS-ENGINE-VER",
		    "TLS Engine version mismatch: %d != %d", engine->version,
		    NNG_TLS_ENGINE_VERSION);
		return (NNG_ENOTSUP);
	}
	nng_log_info("NNG-TLS-INFO", "TLS Engine: %s", engine->description);
	nni_atomic_set_ptr(&tls_engine, (void *) engine);
	return (0);
}

size_t
nni_tls_engine_conn_size(void)
{
	const nng_tls_engine *eng;

	eng = nni_atomic_get_ptr(&tls_engine);

	return (eng == NULL ? false : eng->conn_ops->size);
}

#ifdef NNG_TLS_ENGINE_INIT
extern int NNG_TLS_ENGINE_INIT(void);
#else
static int
NNG_TLS_ENGINE_INIT(void)
{
	return (0);
}
#endif

#ifdef NNG_TLS_ENGINE_FINI
extern void NNG_TLS_ENGINE_FINI(void);
#else
static void
NNG_TLS_ENGINE_FINI(void)
{
}
#endif

int
nni_tls_sys_init(void)
{
	int rv;

	rv = NNG_TLS_ENGINE_INIT();
	if (rv != 0) {
		return (rv);
	}
	return (0);
}

void
nni_tls_sys_fini(void)
{
	NNG_TLS_ENGINE_FINI();
}
