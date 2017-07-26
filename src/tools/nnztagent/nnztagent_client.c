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
// nnztagent_client.c
//
// Client code for talking to the agent.  This is intended to be linked
// into libnng, and so it uses nng internals.
//

#include "nng_impl.h"
#include "nnztagent.h"

struct nnzt_agent {

	// We will use either IPC or TCP.  We don't much care which.
	nni_plat_ipc_ep *  ipc_ep;
	nni_plat_ipc_pipe *ipc_pipe;
	nni_plat_tcp_ep *  tcp_ep;
	nni_plat_tcp_pipe *tcp_pipe;

	char keyfile[256];
	char homedir[256];
};

static void
nnzt_agent_homedir(char *homedir, int size)
{
	char *home;

	// Always honor the NNZTAGENTHOME first.  No underscores for
	// maximum portability.
	if ((home = getenv("NNZTAGENTHOME")) != NULL) {
		(void) snprintf(homedir, size, "%s", home);
		return;
	}

#ifdef _WIN32
	(void) snprintf(homedir, size, "%s%s/.nnztagent", getenv("HOMEDRIVE"),
	    getenv("HOMEPATH"));
#else
	// HOME is required by POSIX.
	if ((home = getenv("HOME")) != NULL) {
		(void) snprintf(homedir, size, "%s/.nnztagent", home);
		return;
	}
	(void) snprintf(homedir, size, "/.nnztagent");
#endif
}

// We only support connecting to looopback or IPC addresses; others are
// subject to observation or tampering or both.
#define TCP4_LOOP "tcp://127.0.0.1:"
#define TCP6_LOOP "tcp://::1:"
#define IPC_ADDR "ipc://"

int
nnzt_agent_open(nnzt_agent_t **ap, const char *url, const char *keyfile)
{
	nnzt_agent * agent;
	nng_sockaddr sa;

	if (url == NULL) {
		// READ the URL from .nnztagent/address
	}
	if ((agent = NNI_ALLOC_STRUCT(agent)) == NULL) {
		return (NNG_ENOMEM);
	}

	nnzt_agent_homedir(agent->homedir, sizeof(agent->homedir));
	if (keyfile == NULL) {
		// Windows is fine with "/" directory seperators.
		snprintf(agent->keyfile, "%s/%s", agent->homedir, "authkey");
	}

	(&sa, 0, sizeof(sa));
	if (strncmp(url, TCP4_LOOP, strlen(TCP4_LOOP)) == 0) {
		sa.s_in.sa_family = NNG_AF_INET;
		sa.s_in.sa_addr   = INADDR_LOOPBACK;
		sa.s_in.sa_port   = htons(atoi(url + strlen(TCP4_LOOP)));
		if (sa.s_in.sa_port == 0) {
			NNI_FREE_STRUCT(agent);
			return (NNG_EADDRINVAL);
		}
	} else if (strncmp(url, TCP6_LOOP, strlen(TCP6_LOOP)) == 0) {
		sa.s_in6.sa_family   = NNG_AF_INET6;
		sa.s_in6.sa_addr[15] = 1; // all others already zero
		sa.s_in6.sa_port     = htons(atoi(url + strlen(TCP6_LOOP)));
		if (sa.s_in6.sa_port == 0) {
			NNI_FREE_STRUCT(agent);
			return (NNG_EADDRINVAL);
		}
	} else if (strncmp(url, IPC_ADDR, strlen(IPC_ADDR)) == 0) {
		sa.s_path.sa_family = NNG_AF_IPC;
		(void) snprintf(sa.s_path.sa_path, sizeof(sa.s_path.sa_path),
		    "%s", url + strlen(IPC_ADDR));
	} else {
		NNI_FREE_STRUCT(agent);
		return (NNG_EADDRINVAL);
	}
}

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