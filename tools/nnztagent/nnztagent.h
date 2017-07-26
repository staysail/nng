//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

//
// nnztagent.h
//
// This file provides some documentation of the protocol as well as
// definitions suitable for use in the nng library which acts as a
// client.
//

#ifndef NNZTAGENT_H
#define NNZTAGENT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	NNZT_CODE_OK           = 0x00,
	NNZT_CODE_ERR          = 0x0f,
	NNZT_CODE_HELLO        = 0x01,
	NNZT_CODE_IDENTIFY     = 0x02,
	NNZT_CODE_JOIN         = 0x03,
	NNZT_CODE_BIND6P       = 0x61,
	NNZT_CODE_CONNECT6P    = 0x62,
	NNZT_CODE_ACCEPT6P     = 0x63,
	NNZT_CODE_SELFNAME     = 0x64,
	NNZT_CODE_PEERNAME     = 0x65,
	NNZT_CODE_DISCONNECT6P = 0x66,
	NNZT_CODE_DATA6P       = 0x67,
} nnzt_code_t;

typedef enum {
	NNZT_OK           = 0x00,
	NNZT_ESTATE       = 0x01,
	NNZT_ECONNREFUSED = 0x02,
	NNZT_EADDRINUSE   = 0x03,
	NNZT_EADDRINVAL   = 0x04,
	NNZT_ECLOSED      = 0x05,
	NNZT_EBADCONV     = 0x06,
} nnzt_err_t;

typedef struct {
	nnzt_err_t err_code;
	char       err_message[1];
} nnzt_err_response_t;

typedef struct {
	uint64_t hello_timestamp;
	uint64_t hello_nonce;
} nnzt_hello_request_t;

typedef struct {
	uchar_t hello_hash[32];
} nnzt_hello_response_t;

typedef struct {
} nnzt_identify_request_t;

typedef struct {
	uint64_t identify_node_id;
} nnzt_identify_response_t;

typedef struct {
	uint64_t join_network_id;
} nnzt_join_request_t;

typedef struct {
} nnzt_join_response_t;

typedef struct {
	uint16_t bind_port;
	uint16_t bind_listen_depth;
} nnzt_bind6p_request_t;

typedef struct {
} nnzt_bind6p_response_t;

typedef struct {
	uint64_t connect_remote_id;
	uint16_t connect_remote_port;
} nnzt_connect6p_request_t;

typedef struct {
	uint32_t connect_conv_id;
} nnzt_connect6p_response_t;

typedef struct {
	uint32_t accept_conv_id;
} nnzt_accept6p_request_t;

typedef struct {
} nnzt_accept_response_t;

typedef struct {
	uint32_t self_conv_id;
} nnzt_selfname_request_t;

typedef struct {
	uint64_t self_node_id;
	uint16_t self_port;
} nnzt_selfname_response_t;

typedef struct {
	uint32_t peer_conv_id;
} nnzt_peername_request_t;

typedef struct {
	uint64_t peer_node_id;
	uint16_t peer_port;
} nnzt_peername_response_t;

typedef struct {
	uint32_t disconnect_conv_id;
} nnzt_disconnect_request_t;

typedef struct {
} nnzt_disconnect_response_t;

typedef struct {
	uint32_t data_conv_id;
	char     data_data[1];
} nnzt_data6p_request_t;

typedef struct {
} nnzt_data6p_request_t;

typedef struct nnzt_agent nnzt_agent_t;

// nnzt_agent_open opens a connect to the agent.  The URL should be
// a URL where the agent can be reached, and the keyfile should be
// the location of the shared key.  Either of these can be NULL, in
// which case reasonable defaults will be attempted, by reading the
// contents of files located in ~/.nnztagent or similar.
//
// This performs the HELLO authentication as well; this will block
// until the operation is complete.  The agent responds quickly,
// and should be connected over a fast loopback connection.
extern int nnzt_agent_open(
    nnzt_agent_t **, const char *url, const char *keyfile);

// nnzt_agent_close closes the connecto the agent.  Any open conversations
// on the agent are closed as well.
extern void nnzt_agent_close(nnzt_agent_t *);

// nnzt_agent_mk6plane makes a 6PLANE IPv6 address from the network ID
// and the node ID.
extern void nnzt_agent_mk6plane(uint64_t nwid, uint64_t nodeid, void *addr);

// nnzt_agent_join joins the network requested.  This operation can take
// some time, as this can involve commnicating with root nodes, etc.
extern int nnzt_agent_join(nnzt_agent_t *, uint64_t);

// nnzt_bind6p gets a local socket in the associated network, bound
// the associated address.  This can take some time.
extern void nnzt_agent_bind6p(nnzt_agent_t *, uint16_t, nni_aio *);
extern void nnzt_agent_connect6p(
    nnzt_agent_t *, uint64_t, uint16_t, nni_aio *);
extern void nnzt_agent_accept6p(nnzt_agent_t *, nni_aio *);
extern void nnzt_agent_send6p(nnzt_agent_t *, uint32_t, nni_aio *);
extern void nnzt_agent_recv6p(nnzt_agent_t *, uint32_t, nni_aio *);
extern void nnzt_agent_disconnect6p(nnzt_agent_t *, uint32_t);

#ifdef __cplusplus
}
#endif

#endif