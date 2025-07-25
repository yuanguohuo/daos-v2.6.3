/**
 * (C) Copyright 2016-2024 Intel Corporation.
 * (C) Copyright 2025 Hewlett Packard Enterprise Development LP
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */

#define D_LOGFAC       DD_FAC(server)

#include <abt.h>
#include <daos/common.h>
#include <daos_errno.h>
#include "srv_internal.h"

/* ============== Thread collective functions ============================ */

/** The maximum number of credits for each IO chore queue. That is per helper XS. */
uint32_t dss_chore_credits;

struct aggregator_arg_type {
	struct dss_stream_arg_type	at_args;
	void				(*at_reduce)(void *a_args,
						     void *s_args);
	int				at_rc;
	int				at_xs_nr;
};

/**
 * Collective operations among all server xstreams
 */
struct dss_future_arg {
	ABT_future	dfa_future;
	int		(*dfa_func)(void *);
	void		*dfa_arg;
	/** User callback for asynchronous mode */
	void		(*dfa_comp_cb)(void *);
	/** Argument for the user callback */
	void		*dfa_comp_arg;
	int		dfa_status;
	bool		dfa_async;
};

struct collective_arg {
	struct dss_future_arg		ca_future;
};

static void
collective_func(void *varg)
{
	struct dss_stream_arg_type	*a_args	= varg;
	struct collective_arg		*carg	= a_args->st_coll_args;
	struct dss_future_arg		*f_arg	= &carg->ca_future;
	int				rc;

	/** Update just the rc value */
	a_args->st_rc = f_arg->dfa_func(f_arg->dfa_arg);

	rc = ABT_future_set(f_arg->dfa_future, (void *)a_args);
	if (rc != ABT_SUCCESS)
		D_ERROR("future set failure %d\n", rc);
}

/* Reduce the return codes into the first element. */
static void
collective_reduce(void **arg)
{
	struct aggregator_arg_type	*aggregator;
	struct dss_stream_arg_type	*stream;
	int				*nfailed;
	int				 i;

	aggregator = (struct aggregator_arg_type *)arg[0];
	nfailed = &aggregator->at_args.st_rc;

	for (i = 1; i < aggregator->at_xs_nr + 1; i++) {
		stream = (struct dss_stream_arg_type *)arg[i];
		if (stream->st_rc != 0) {
			if (aggregator->at_rc == 0)
				aggregator->at_rc = stream->st_rc;
			(*nfailed)++;
		}

		/** optional custom aggregator call provided across streams */
		if (aggregator->at_reduce)
			aggregator->at_reduce(aggregator->at_args.st_arg,
					      stream->st_arg);
	}
}

static int
dss_collective_reduce_internal(struct dss_coll_ops *ops,
			       struct dss_coll_args *args, bool create_ult,
			       unsigned int flags)
{
	struct collective_arg		carg;
	struct dss_coll_stream_args	*stream_args;
	struct dss_stream_arg_type	*stream;
	struct aggregator_arg_type	aggregator;
	struct dss_xstream		*dx;
	ABT_future			future;
	int				xs_nr;
	int				rc;
	int				tid;
	int				tgt_id = dss_get_module_info()->dmi_tgt_id;
	uint32_t			bm_len;
	bool				self = false;

	if (ops == NULL || args == NULL || ops->co_func == NULL) {
		D_DEBUG(DB_MD, "mandatory args missing dss_collective_reduce");
		return -DER_INVAL;
	}

	if (ops->co_reduce_arg_alloc != NULL &&
	    ops->co_reduce_arg_free == NULL) {
		D_DEBUG(DB_MD, "Free callback missing for reduce args\n");
		return -DER_INVAL;
	}

	if (dss_tgt_nr == 0) {
		/* May happen when the server is shutting down. */
		D_DEBUG(DB_TRACE, "no xstreams\n");
		return -DER_CANCELED;
	}

	bm_len = args->ca_tgt_bitmap_sz << 3;
	xs_nr = dss_tgt_nr;
	stream_args = &args->ca_stream_args;
	D_ALLOC_ARRAY(stream_args->csa_streams, xs_nr);
	if (stream_args->csa_streams == NULL)
		return -DER_NOMEM;

	/*
	 * Use the first, extra element of the value array to store the number
	 * of failed tasks.
	 */
	rc = ABT_future_create(xs_nr + 1, collective_reduce, &future);
	if (rc != ABT_SUCCESS)
		D_GOTO(out_streams, rc = dss_abterr2der(rc));

	carg.ca_future.dfa_future = future;
	carg.ca_future.dfa_func	= ops->co_func;
	carg.ca_future.dfa_arg	= args->ca_func_args;
	carg.ca_future.dfa_status = 0;

	memset(&aggregator, 0, sizeof(aggregator));
	aggregator.at_xs_nr = xs_nr;
	if (ops->co_reduce) {
		aggregator.at_args.st_arg = args->ca_aggregator;
		aggregator.at_reduce	  = ops->co_reduce;
	}

	if (ops->co_reduce_arg_alloc)
		for (tid = 0; tid < xs_nr; tid++) {
			stream = &stream_args->csa_streams[tid];
			rc = ops->co_reduce_arg_alloc(stream,
						     aggregator.at_args.st_arg);
			if (rc)
				D_GOTO(out_future, rc);
		}

	rc = ABT_future_set(future, (void *)&aggregator);
	D_ASSERTF(rc == ABT_SUCCESS, "%d\n", rc);
	for (tid = 0; tid < xs_nr; tid++) {
		stream			= &stream_args->csa_streams[tid];
		stream->st_coll_args	= &carg;

		if (args->ca_tgt_bitmap != NULL) {
			if (tid >= bm_len || isclr(args->ca_tgt_bitmap, tid)) {
				D_DEBUG(DB_TRACE, "Skip tgt %d\n", tid);
				rc = ABT_future_set(future, (void *)stream);
				D_ASSERTF(rc == ABT_SUCCESS, "%d\n", rc);
				continue;
			}

			if (tgt_id == tid && flags & DSS_USE_CURRENT_ULT) {
				self = true;
				continue;
			}
		}

		dx = dss_get_xstream(DSS_MAIN_XS_ID(tid));
		if (create_ult) {
			ABT_thread_attr		attr;
			int			rc1;

			if (flags & DSS_ULT_DEEP_STACK) {
				rc1 = ABT_thread_attr_create(&attr);
				if (rc1 != ABT_SUCCESS)
					D_GOTO(next, rc = dss_abterr2der(rc1));

				rc1 = ABT_thread_attr_set_stacksize(attr, DSS_DEEP_STACK_SZ);
				D_ASSERT(rc1 == ABT_SUCCESS);

				D_DEBUG(DB_TRACE, "Create collective ult with stacksize %d\n",
					DSS_DEEP_STACK_SZ);

			} else {
				attr = ABT_THREAD_ATTR_NULL;
			}

			rc = sched_create_thread(dx, collective_func, stream, attr, NULL, flags);
			if (attr != ABT_THREAD_ATTR_NULL) {
				rc1 = ABT_thread_attr_free(&attr);
				D_ASSERT(rc1 == ABT_SUCCESS);
			}
		} else {
			rc = sched_create_task(dx, collective_func, stream,
					       NULL, flags);
		}

		if (rc != 0) {
next:
			stream->st_rc = rc;
			rc = ABT_future_set(future, (void *)stream);
			D_ASSERTF(rc == ABT_SUCCESS, "%d\n", rc);
		}
	}

	if (self) {
		stream = &stream_args->csa_streams[tgt_id];
		stream->st_coll_args = &carg;
		collective_func(stream);
	}

	ABT_future_wait(future);

	rc = aggregator.at_rc;

out_future:
	ABT_future_free(&future);

	if (ops->co_reduce_arg_free)
		for (tid = 0; tid < xs_nr; tid++)
			ops->co_reduce_arg_free(&stream_args->csa_streams[tid]);

out_streams:
	D_FREE(args->ca_stream_args.csa_streams);

	return rc;
}

/**
 * General case:
 * Execute \a task(\a arg) collectively on all server xstreams. Can only be
 * called by ULTs. Can only execute tasklet-compatible functions. User specified
 * reduction functions for aggregation after collective
 *
 * \param[in] ops		All dss_collective ops to work on streams
 *				include \a func(\a arg) for collective on all
 *				server xstreams.
 * \param[in] args		All arguments required for dss_collective
 *				including func args.
 * \param[in] flags		Flags of dss_ult_flags
 *
 * \return			number of failed xstreams or error code
 */
int
dss_task_collective_reduce(struct dss_coll_ops *ops,
			   struct dss_coll_args *args, unsigned int flags)
{
	return dss_collective_reduce_internal(ops, args, false, flags);
}

/**
 * General case:
 * Execute \a ULT(\a arg) collectively on all server xstreams. Can only be
 * called by ULTs. Can only execute tasklet-compatible functions. User specified
 * reduction functions for aggregation after collective
 *
 * \param[in] ops		All dss_collective ops to work on streams
 *				include \a func(\a arg) for collective on all
 *				server xstreams.
 * \param[in] args		All arguments required for dss_collective
 *				including func args.
 * \param[in] flags		Flags from dss_ult_flags
 *
 * \return			number of failed xstreams or error code
 */
int
dss_thread_collective_reduce(struct dss_coll_ops *ops,
			     struct dss_coll_args *args, unsigned int flags)
{
	return dss_collective_reduce_internal(ops, args, true, flags);
}

static int
dss_collective_internal(int (*func)(void *), void *arg, bool thread,
			unsigned int flags)
{
	int				rc;
	struct dss_coll_ops		coll_ops = { 0 };
	struct dss_coll_args		coll_args = { 0 };

	coll_ops.co_func	= func;
	coll_args.ca_func_args	= arg;

	if (thread)
		rc = dss_thread_collective_reduce(&coll_ops, &coll_args, flags);
	else
		rc = dss_task_collective_reduce(&coll_ops, &coll_args, flags);

	return rc;
}

/**
 * Execute \a func(\a arg) collectively on all server xstreams. Can only be
 * called by ULTs. Can only execute tasklet-compatible functions.
 *
 * \param[in] func	function to be executed
 * \param[in] arg	argument to be passed to \a func
 * \param[in] flags	Flags from dss_ult_flags
 *
 * \return		number of failed xstreams or error code
 */
int
dss_task_collective(int (*func)(void *), void *arg, unsigned int flags)
{
	return dss_collective_internal(func, arg, false, flags);
}

/**
 * Execute \a func(\a arg) collectively on all server xstreams. Can only be
 * called by ULTs. Can only execute tasklet-compatible functions.
 *
 * \param[in] func	function to be executed
 * \param[in] arg	argument to be passed to \a func
 * \param[in] flags	Flags from dss_ult_flags
 *
 * \return		number of failed xstreams or error code
 */

int
dss_thread_collective(int (*func)(void *), void *arg, unsigned int flags)
{
	return dss_collective_internal(func, arg, true, flags);
}

int
dss_build_coll_bitmap(int *exclude_tgts, uint32_t exclude_cnt, uint8_t **p_bitmap,
		      uint32_t *bitmap_sz)
{
	uint8_t		*bitmap = NULL;
	uint32_t	 size = ((dss_tgt_nr - 1) >> 3) + 1;
	uint32_t	 bits = size << 3;
	int		 rc = 0;
	int		 i;

	D_ALLOC(bitmap, size);
	if (bitmap == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	for (i = 0; i < size; i++)
		bitmap[i] = 0xff;

	for (i = dss_tgt_nr; i < bits; i++)
		clrbit(bitmap, i);

	if (exclude_tgts == NULL)
		goto out;

	for (i = 0; i < exclude_cnt; i++) {
		D_ASSERT(exclude_tgts[i] < dss_tgt_nr);
		clrbit(bitmap, exclude_tgts[i]);
	}

out:
	if (rc == 0) {
		*p_bitmap = bitmap;
		*bitmap_sz = size;
	} else {
		D_ERROR("Failed to build bitmap for collective task: "DF_RC"\n", DP_RC(rc));
	}

	return rc;
}

static inline uint32_t
sched_ult2xs_multisocket(int xs_type, int tgt_id)
{
	static __thread uint32_t offload;
	uint32_t                 socket = tgt_id / dss_tgt_per_numa_nr;
	uint32_t                 base;
	uint32_t                 target;

	if (dss_tgt_offload_xs_nr == 0) {
		if (xs_type == DSS_XS_IOFW && dss_forward_neighbor) {
			/* Keep the old forwarding behavior, but NUMA aware */
			target = (socket * dss_tgt_per_numa_nr) +
				 (tgt_id + offload) % dss_tgt_per_numa_nr;
			target = DSS_MAIN_XS_ID(target);
			goto check;
		}
		return DSS_XS_SELF;
	}

	base   = dss_sys_xs_nr + dss_tgt_nr + (socket * dss_offload_per_numa_nr);
	target = base + ((offload + tgt_id) % dss_offload_per_numa_nr);

check:
	D_ASSERT(target < DSS_XS_NR_TOTAL && target >= dss_sys_xs_nr);
	offload = target + 17; /* Seed next selection */
	return target;
}

/* ============== ULT create functions =================================== */

static inline int
sched_ult2xs(int xs_type, int tgt_id)
{
	uint32_t	xs_id;

	if (xs_type == DSS_XS_VOS || xs_type == DSS_XS_OFFLOAD || xs_type == DSS_XS_IOFW)
		D_ASSERT(tgt_id >= 0 && tgt_id < dss_tgt_nr);
	switch (xs_type) {
	case DSS_XS_SELF:
		return DSS_XS_SELF;
	case DSS_XS_SYS:
		return 0;
	case DSS_XS_SWIM:
		return 1;
	case DSS_XS_DRPC:
		return 2;
	case DSS_XS_IOFW:
		if (dss_numa_nr > 1)
			return sched_ult2xs_multisocket(xs_type, tgt_id);
		if (!dss_helper_pool) {
			if (dss_tgt_offload_xs_nr > 0)
				xs_id = DSS_MAIN_XS_ID(tgt_id) + 1;
			else
				xs_id = DSS_MAIN_XS_ID((tgt_id + 1) % dss_tgt_nr);
			break;
		}

		/*
		 * Comment from @liuxuezhao:
		 *
		 * This is the case that no helper XS, so for IOFW,
		 * we either use itself, or use neighbor XS.
		 *
		 * Why original code select neighbor XS rather than itself
		 * is because, when the code is called, I know myself is on
		 * processing IO request and need IO forwarding, now I am
		 * processing IO, so likely there is not only one IO (possibly
		 * more than one IO for specific dkey), I am busy so likely my
		 * neighbor is not busy (both busy seems only in some special
		 * multiple dkeys used at same time) can help me do the IO
		 * forwarding?
		 *
		 * But this is just original intention, you guys think it is
		 * not reasonable? prefer another way that I am processing IO
		 * and need IO forwarding, OK, just let myself do it ...
		 *
		 * Note that we first do IO forwarding and then serve local IO,
		 * ask neighbor to do IO forwarding seems is helpful to make
		 * them concurrent, right?
		 */
		if (dss_tgt_offload_xs_nr > 0)
			xs_id = dss_sys_xs_nr + dss_tgt_nr +
				rand() % min(dss_tgt_nr, dss_tgt_offload_xs_nr);
		else
			xs_id = (DSS_MAIN_XS_ID(tgt_id) + 1) % dss_tgt_nr;
		break;
	case DSS_XS_OFFLOAD:
		if (dss_numa_nr > 1)
			return sched_ult2xs_multisocket(xs_type, tgt_id);
		if (!dss_helper_pool) {
			if (dss_tgt_offload_xs_nr > 0)
				xs_id = DSS_MAIN_XS_ID(tgt_id) + dss_tgt_offload_xs_nr / dss_tgt_nr;
			else
				xs_id = DSS_MAIN_XS_ID((tgt_id + 1) % dss_tgt_nr);
			break;
		}

		if (dss_tgt_offload_xs_nr > dss_tgt_nr)
			xs_id = dss_sys_xs_nr + 2 * dss_tgt_nr +
				(tgt_id % (dss_tgt_offload_xs_nr - dss_tgt_nr));
		else if (dss_tgt_offload_xs_nr > 0)
			xs_id = dss_sys_xs_nr + dss_tgt_nr + tgt_id % dss_tgt_offload_xs_nr;
		else
			xs_id = (DSS_MAIN_XS_ID(tgt_id) + 1) % dss_tgt_nr;
		break;
	case DSS_XS_VOS:
		xs_id = DSS_MAIN_XS_ID(tgt_id);
		break;
	default:
		D_ASSERTF(0, "Invalid xstream type %d.\n", xs_type);
		return -DER_INVAL;
	}
	D_ASSERT(xs_id < DSS_XS_NR_TOTAL && xs_id >= dss_sys_xs_nr);
	return xs_id;
}

static int
ult_create_internal(void (*func)(void *), void *arg, int xs_type, int tgt_idx,
		    size_t stack_size, ABT_thread *ult, unsigned int flags)
{
	ABT_thread_attr		 attr;
	struct dss_xstream	*dx;
	int			 rc, rc1;
	int			 stream_id;

	stream_id = sched_ult2xs(xs_type, tgt_idx);
	if (stream_id == -DER_INVAL)
		return stream_id;

	dx = dss_get_xstream(stream_id);
	if (dx == NULL)
		return -DER_NONEXIST;

	if (stack_size > 0) {
		rc = ABT_thread_attr_create(&attr);
		if (rc != ABT_SUCCESS)
			return dss_abterr2der(rc);

		rc = ABT_thread_attr_set_stacksize(attr, stack_size);
		D_ASSERT(rc == ABT_SUCCESS);

		D_DEBUG(DB_TRACE, "Create ult stacksize is %zd\n", stack_size);
	} else {
		attr = ABT_THREAD_ATTR_NULL;
	}

	rc = sched_create_thread(dx, func, arg, attr, ult, flags);
	if (attr != ABT_THREAD_ATTR_NULL) {
		rc1 = ABT_thread_attr_free(&attr);
		D_ASSERT(rc1 == ABT_SUCCESS);
	}

	return rc;
}

/**
 * Create a ULT to execute \a func(\a arg). If \a ult is not NULL, the caller
 * is responsible for freeing the ULT handle with ABT_thread_free().
 *
 * \param[in]	func		function to execute
 * \param[in]	arg		argument for \a func
 * \param[in]	xs_type		xstream type
 * \param[in]	tgt_idx		VOS target index
 * \param[in]	stack_size	stacksize of the ULT, if it is 0, then create
 *				default size of ULT.
 * \param[out]	ult		ULT handle if not NULL
 */
int
dss_ult_create(void (*func)(void *), void *arg, int xs_type, int tgt_idx,
	       size_t stack_size, ABT_thread *ult)
{
	return ult_create_internal(func, arg, xs_type, tgt_idx, stack_size,
				   ult, 0);
}

int
dss_ult_periodic(void (*func)(void *), void *arg, int xs_type, int tgt_idx,
		 size_t stack_size, ABT_thread *ult)
{
	return ult_create_internal(func, arg, xs_type, tgt_idx, stack_size,
				   ult, DSS_ULT_FL_PERIODIC);
}

static void
ult_execute_cb(void *data)
{
	struct dss_future_arg	*arg = data;
	int			rc;

	rc = arg->dfa_func(arg->dfa_arg);
	arg->dfa_status = rc;

	if (!arg->dfa_async) {
		ABT_future_set(arg->dfa_future, (void *)(intptr_t)rc);
	} else {
		arg->dfa_comp_cb(arg->dfa_comp_arg);
		D_FREE(arg);
	}
}

/**
 * Execute a function in a separate ULT synchornously or asynchronously.
 *
 * Sync: wait until it has been executed.
 * Async: return and call user callback from ULT.
 * Note: This is normally used when it needs to create an ULT on other
 * xstream.
 *
 * \param[in]	func		function to execute
 * \param[in]	arg		argument for \a func
 * \param[in]	user_cb		user call back (mandatory for async mode)
 * \param[in]	arg		argument for \a user callback
 * \param[in]	xs_type		xstream type
 * \param[in]	tgt_id		target index
 * \param[in]	stack_size	stacksize of the ULT, if it is 0, then create
 *				default size of ULT.
 */
int
dss_ult_execute(int (*func)(void *), void *arg, void (*user_cb)(void *),
		void *cb_args, int xs_type, int tgt_id, size_t stack_size)
{
	struct dss_future_arg	*future_arg;
	ABT_future		future;
	int			rc;

	D_ALLOC_PTR(future_arg);
	if (future_arg == NULL)
		return -DER_NOMEM;

	future_arg->dfa_func = func;
	future_arg->dfa_arg = arg;
	future_arg->dfa_status = 0;

	if (user_cb == NULL) {
		rc = ABT_future_create(1, NULL, &future);
		if (rc != ABT_SUCCESS) {
			rc = dss_abterr2der(rc);
			D_FREE(future_arg);
			return rc;
		}
		future_arg->dfa_future = future;
		future_arg->dfa_async  = false;
	} else {
		future_arg->dfa_comp_cb	= user_cb;
		future_arg->dfa_comp_arg = cb_args;
		future_arg->dfa_async	= true;
	}

	rc = dss_ult_create(ult_execute_cb, future_arg, xs_type, tgt_id,
			    stack_size, NULL);
	if (rc)
		D_GOTO(free, rc);

	if (!future_arg->dfa_async) {
		ABT_future_wait(future);
		rc = future_arg->dfa_status;
	}
free:
	if (!future_arg->dfa_async) {
		ABT_future_free(&future);
		D_FREE(future_arg);
	} else if (rc) {
		D_FREE(future_arg);
	}
	return rc;
}

/**
 * Create an ULT on each server xstream to execute a \a func(\a arg)
 *
 * \param[in] func	function to be executed
 * \param[in] arg	argument to be passed to \a func
 * \param[in] main	only create ULT on main XS or not.
 *
 * \return		Success or negative error code
 *			0
 *			-DER_NOMEM
 *			-DER_INVAL
 */
int
dss_ult_create_all(void (*func)(void *), void *arg, bool main)
{
	struct dss_xstream      *dx;
	int			 i, rc = 0;

	for (i = 0; i < dss_xstream_cnt(); i++) {
		dx = dss_get_xstream(i);
		if (main && !dx->dx_main_xs)
			continue;

		rc = sched_create_thread(dx, func, arg, ABT_THREAD_ATTR_NULL,
					 NULL, 0);
		if (rc != 0)
			break;
	}

	return rc;
}

int
dss_offload_exec(int (*func)(void *), void *arg)
{
	struct dss_module_info *info = dss_get_module_info();

	D_ASSERT(info != NULL);
	D_ASSERT(info->dmi_xstream->dx_main_xs);

	return dss_ult_execute(func, arg, NULL, NULL, DSS_XS_OFFLOAD, info->dmi_tgt_id, 0);
}

int
dss_main_exec(void (*func)(void *), void *arg)
{
	struct dss_module_info *info = dss_get_module_info();

	D_ASSERT(info != NULL);
	D_ASSERT(info->dmi_xstream->dx_main_xs || info->dmi_xs_id == 0);

	return dss_ult_create(func, arg, DSS_XS_SELF, info->dmi_tgt_id, 0, NULL);
}

static void
dss_chore_diy_internal(struct dss_chore *chore)
{
reenter:
	D_DEBUG(DB_TRACE, "%p: status=%d\n", chore, chore->cho_status);
	chore->cho_status = chore->cho_func(chore, chore->cho_status == DSS_CHORE_YIELD);
	D_ASSERT(chore->cho_status != DSS_CHORE_NEW);
	if (chore->cho_status == DSS_CHORE_YIELD) {
		ABT_thread_yield();
		goto reenter;
	}
}

static void
dss_chore_ult(void *arg)
{
	struct dss_chore *chore = arg;

	dss_chore_diy_internal(chore);
}

/**
 * Add \a chore for \a func to the chore queue of some other xstream.
 *
 * \param[in]	chore	address of the embedded chore object
 *
 * \retval	-DER_CANCEL	chore queue stopping
 */
int
dss_chore_register(struct dss_chore *chore)
{
	struct dss_module_info *info = dss_get_module_info();
	int                     xs_id;
	struct dss_xstream     *dx;
	struct dss_chore_queue *queue;

	D_ASSERT(chore->cho_credits > 0);

	chore->cho_status = DSS_CHORE_NEW;

	/*
	 * The dss_chore_queue_ult approach may get insufficient scheduling on
	 * a "main" xstream when the chore queue is long. So we fall back to
	 * the one-ULT-per-chore approach if there's no helper xstream.
	 */
	if (dss_tgt_offload_xs_nr == 0) {
		D_INIT_LIST_HEAD(&chore->cho_link);
		return dss_ult_create(dss_chore_ult, chore, DSS_XS_IOFW, info->dmi_tgt_id,
				      0 /* stack_size */, NULL /* ult */);
	}

	/* Find the chore queue. */
	xs_id = sched_ult2xs(DSS_XS_IOFW, info->dmi_tgt_id);
	D_ASSERT(xs_id != -DER_INVAL);
	dx = dss_get_xstream(xs_id);
	D_ASSERT(dx != NULL);
	queue = &dx->dx_chore_queue;
	D_ASSERT(queue != NULL);

	ABT_mutex_lock(queue->chq_mutex);
	if (queue->chq_stop) {
		ABT_mutex_unlock(queue->chq_mutex);
		return -DER_CANCELED;
	}

	if (!chore->cho_priority && queue->chq_credits < chore->cho_credits) {
		/*
		 * Piggyback current available credits, then the caller can decide
		 * whether can shrink credit requirement and retry with more steps.
		 */
		chore->cho_credits = queue->chq_credits;
		ABT_mutex_unlock(queue->chq_mutex);
		return -DER_AGAIN;
	}

	/* queue->chq_credits can be negative temporarily because of high priority requests. */
	queue->chq_credits -= chore->cho_credits;
	chore->cho_hint = queue;
	d_list_add_tail(&chore->cho_link, &queue->chq_list);
	ABT_cond_broadcast(queue->chq_cond);
	ABT_mutex_unlock(queue->chq_mutex);

	D_DEBUG(DB_TRACE, "register chore %p on queue %p: tgt=%d -> xs=%d dx.tgt=%d, credits %u\n",
		chore, queue, info->dmi_tgt_id, xs_id, dx->dx_tgt_id, chore->cho_credits);
	return 0;
}

void
dss_chore_deregister(struct dss_chore *chore)
{
	struct dss_chore_queue *queue = chore->cho_hint;

	if (queue != NULL) {
		D_ASSERT(chore->cho_credits > 0);

		ABT_mutex_lock(queue->chq_mutex);
		queue->chq_credits += chore->cho_credits;
		ABT_mutex_unlock(queue->chq_mutex);

		D_DEBUG(DB_TRACE, "deregister chore %p from queue %p: credits %u\n", chore, queue,
			chore->cho_credits);
	}
}

/**
 * Do \a chore for \a func synchronously in the current ULT.
 *
 * \param[in]	chore	embedded chore object
 * \param[in]	func	function to be executed via \a chore
 */
void
dss_chore_diy(struct dss_chore *chore)
{
	D_INIT_LIST_HEAD(&chore->cho_link);
	chore->cho_status = DSS_CHORE_NEW;

	dss_chore_diy_internal(chore);
}

static void
dss_chore_queue_ult(void *arg)
{
	struct dss_chore_queue *queue = arg;
	d_list_t                list  = D_LIST_HEAD_INIT(list);

	D_ASSERT(queue != NULL);
	D_DEBUG(DB_TRACE, "begin\n");

	for (;;) {
		d_list_t          list_tmp = D_LIST_HEAD_INIT(list_tmp);
		struct dss_chore *chore;
		struct dss_chore *chore_tmp;
		bool              stop = false;

		/*
		 * The scheduling order shall be
		 *
		 *   [queue->chq_list] [list],
		 *
		 * where list contains chores that have returned
		 * DSS_CHORE_YIELD in the previous iteration.
		 */
		ABT_mutex_lock(queue->chq_mutex);
		for (;;) {
			if (!d_list_empty(&queue->chq_list)) {
				d_list_splice_init(&queue->chq_list, &list);
				break;
			}
			if (!d_list_empty(&list))
				break;
			if (queue->chq_stop) {
				stop = true;
				break;
			}
			sched_cond_wait_for_business(queue->chq_cond, queue->chq_mutex);
		}
		ABT_mutex_unlock(queue->chq_mutex);

		if (stop)
			break;

		d_list_for_each_entry_safe(chore, chore_tmp, &list, cho_link) {
			enum dss_chore_status status;

			/*
			 * CAUTION: When cho_func returns DSS_CHORE_DONE, chore
			 * may have been freed already!
			 */
			d_list_del_init(&chore->cho_link);
			D_DEBUG(DB_TRACE, "%p: before: status=%d\n", chore, chore->cho_status);
			status = chore->cho_func(chore, chore->cho_status == DSS_CHORE_YIELD);
			D_DEBUG(DB_TRACE, "%p: after: status=%d\n", chore, status);
			if (status == DSS_CHORE_YIELD) {
				chore->cho_status = status;
				d_list_add_tail(&chore->cho_link, &list_tmp);
			} else {
				D_ASSERTF(status == DSS_CHORE_DONE, "status=%d\n", status);
			}
			ABT_thread_yield();
		}

		d_list_splice_init(&list_tmp, &list);
	}

	D_DEBUG(DB_TRACE, "end\n");
}

int
dss_chore_queue_init(struct dss_xstream *dx)
{
	struct dss_chore_queue *queue = &dx->dx_chore_queue;
	int                     rc;

	D_INIT_LIST_HEAD(&queue->chq_list);
	queue->chq_stop = false;
	queue->chq_credits = dss_chore_credits;

	rc = ABT_mutex_create(&queue->chq_mutex);
	if (rc != ABT_SUCCESS) {
		D_ERROR("failed to create chore queue mutex: %d\n", rc);
		return dss_abterr2der(rc);
	}

	rc = ABT_cond_create(&queue->chq_cond);
	if (rc != ABT_SUCCESS) {
		D_ERROR("failed to create chore queue condition variable: %d\n", rc);
		ABT_mutex_free(&queue->chq_mutex);
		return dss_abterr2der(rc);
	}

	return 0;
}

int
dss_chore_queue_start(struct dss_xstream *dx)
{
	struct dss_chore_queue *queue = &dx->dx_chore_queue;
	int                     rc;

	rc = daos_abt_thread_create(dx->dx_sp, dss_free_stack_cb, dx->dx_pools[DSS_POOL_GENERIC],
				    dss_chore_queue_ult, queue, ABT_THREAD_ATTR_NULL,
				    &queue->chq_ult);
	if (rc != 0) {
		D_ERROR("failed to create chore queue ULT: %d\n", rc);
		return dss_abterr2der(rc);
	}

	return 0;
}

void
dss_chore_queue_stop(struct dss_xstream *dx)
{
	struct dss_chore_queue *queue = &dx->dx_chore_queue;

	ABT_mutex_lock(queue->chq_mutex);
	queue->chq_stop = true;
	ABT_cond_broadcast(queue->chq_cond);
	ABT_mutex_unlock(queue->chq_mutex);
	ABT_thread_free(&queue->chq_ult);
}

void
dss_chore_queue_fini(struct dss_xstream *dx)
{
	struct dss_chore_queue *queue = &dx->dx_chore_queue;

	ABT_cond_free(&queue->chq_cond);
	ABT_mutex_free(&queue->chq_mutex);
}
