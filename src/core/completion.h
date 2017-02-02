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
#include "core/timer.h"

// Completions are structures that are registered to deal with asynchronous
// I/O.  Basically the caller submits a completion to a worker, which then
// notifies the caller by updating the completion and calling the callback.
// The completion will always be executed exactly once.

struct nni_compl {
	nni_list_node	c_node;
	nni_cq *	c_cq;
	int		c_sched;
	int		c_type;
	int		c_result;
	size_t		c_xferred;
	nni_timer_node	c_expire;
	nni_taskq_ent	c_tqe;

	// These are specific to different completion types.
	nni_msgq *	c_mq;
	nni_msg *	c_msg;
	nni_pipe *	c_pipe;
	nni_ep *	c_ep;
	nni_iov *	c_iov;
	int		c_iovcnt;
	const char *	c_host;
	nni_sockaddr *	c_addr;
};

// Completion types.
#define NNI_COMPL_TYPE_NONE		0
#define NNI_COMPL_TYPE_TIMER		1
#define NNI_COMPL_TYPE_READPIPE		2
#define NNI_COMPL_TYPE_WRITEPIPE	3
#define NNI_COMPL_TYPE_GETMSG		4
#define NNI_COMPL_TYPE_PUTMSG		5
#define NNI_COMPL_TYPE_CANGETMSG	6
#define NNI_COMPL_TYPE_CANPUTMSG	7
#define NNI_COMPL_TYPE_ACCEPT		8
#define NNI_COMPL_TYPE_CONNECT		9
#define NNI_COMPL_TYPE_RESOLVE		10

extern void nni_cq_run(nni_cq *, int (*)(nni_compl *, void *), void *);
extern int nni_cq_init(nni_cq **);
extern void nni_cq_fini(nni_cq *);

extern void nni_compl_cancel(nni_compl *);
extern void nni_compl_submit(nni_compl *, nni_cq *, nni_time);
extern void nni_compl_init(nni_compl *, int, nni_cb, void *);
extern void nni_compl_init_canput(nni_compl *, nni_msgq *, nni_cb, void *);
extern void nni_compl_init_canget(nni_compl *, nni_msgq *, nni_cb, void *);

#endif // CORE_COMPLETION_H
