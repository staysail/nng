//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef CORE_CPORT_H
#define CORE_CPORT_H

#include "core/defs.h"
#include "core/list.h"
#include "core/msgqueue.h"
#include "core/taskq.h"

// Completions are structures that are registered to deal with asynchronous
// I/O.  Basically the caller submits a completion to a worker, which then
// notifies the caller by updating the completion and calling the callback.
// The completion will always be executed exactly once.

struct nni_completion_base {
	nni_list_node	cbase_node;
	int		cbase_type;
	int		cbase_result;
	size_t		cbase_xferred;
	nni_time	cbase_expire;
	nni_taskq_ent	cbase_tqe;
};
typedef struct nni_completion_base nni_completion_base;

#define comp_type	comp_base.cbase_type
#define comp_node	comp_base.cbase_node
#define comp_cb		comp_base.cbase_tqe.tqe_cb
#define comp_arg	comp_base.cbase_tqe.tqe_arg
#define comp_result	comp_base.cbase_result
#define comp_xferred	comp_base.cbase_xferred
#define comp_expire	comp_base.cbase_expire
#define comp_tqe	comp_base.cbase_tqe

struct nni_completion {
	nni_completion_base comp_base;
};

struct nni_completion_timer {
	nni_completion_base comp_base;
};

struct nni_completion_rwpipe {  // Shared by READPIPE and READPIPE
	nni_completion_base	comp_base;
	nni_pipe *		comp_pipe;
	nni_iov *		comp_iov;
	int			comp_iovcnt;
};

struct nni_completion_getmsg {
	nni_completion_base	comp_base;
	nni_msgq *		comp_msgq;
	nni_msg **		comp_msgp;
};

struct nni_completion_putmsg {
	nni_completion_base	comp_base;
	nni_msgq *		comp_msgq;
	nni_msg *		comp_msg;
};

struct nni_completion_cangetmsg {
	nni_completion_base	comp_base;
	nni_msgq *		comp_msgq;
};

struct nni_completion_canputmsg {
	nni_completion_base	comp_base;
	nni_msgq *		comp_msgq;
};

struct nni_completion_accept {
	nni_completion_base	comp_base;
	nni_ep *		comp_ep;
	nni_pipe **		comp_pipep;
};

struct nni_completion_connect {
	nni_completion_base	comp_base;
	nni_ep *		comp_ep;
	nni_pipe **		comp_pipep;
};

struct nni_completion_resolve {
	nni_completion_base	comp_base;
	const char *		comp_host;
	nni_sockaddr *		comp_addr;
};

// Completion types.
#define NNI_COMPLETION_TYPE_NONE		0
#define NNI_COMPLETION_TYPE_TIMER		1       // nni_cp_timer
#define NNI_COMPLETION_TYPE_READPIPE		2       // nni_cp_rwpipe
#define NNI_COMPLETION_TYPE_WRITEPIPE		3       // nni_cp_rwpipe
#define NNI_COMPLETION_TYPE_GETMSG		4       // nni_cp_getmsg
#define NNI_COMPLETION_TYPE_PUTMSG		5       // nni_cp_putmsg
#define NNI_COMPLETION_TYPE_CANGETMSG		6       // nni_cp_canget
#define NNI_COMPLETION_TYPE_CANPUTMSG		7       // nni_cp_canput
#define NNI_COMPLETION_TYPE_ACCEPT		8       // nni_cp_accept
#define NNI_COMPLETION_TYPE_CONNECT		9       // nni_cp_connect
#define NNI_COMPLETION_TYPE_RESOLVE		10      // nni_cp_resolve

#endif // CORE_COMPLETION_H
