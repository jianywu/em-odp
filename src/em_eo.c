/*
 *   Copyright (c) 2015, Nokia Solutions and Networks
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "em_include.h"

/**
 * Params for eo_local_func_call_req().
 * Init params with eo_local_func_call_param_init() before usage.
 */
typedef struct {
	eo_elem_t *eo_elem;
	queue_elem_t *q_elem;
	int delete_queues;
	uint64_t ev_id;
	void (*f_done_callback)(void *arg_ptr);
	int num_notif;
	const em_notif_t *notif_tbl; /* notif_tbl[num_notif] */
	int exclude_current_core;
	bool sync_operation;
} eo_local_func_call_param_t;

static void
eo_local_func_call_param_init(eo_local_func_call_param_t *param);
static em_status_t
eo_local_func_call_req(const eo_local_func_call_param_t *param);

static em_status_t
check_eo_local_status(const loc_func_retval_t *loc_func_retvals);

static void
eo_start_done_callback(void *args);
static void
eo_start_sync_done_callback(void *args);

static void
eo_stop_done_callback(void *args);
static void
eo_stop_sync_done_callback(void *args);

static em_status_t
eo_remove_queue_local(const eo_elem_t *eo_elem, const queue_elem_t *q_elem);
static void
eo_remove_queue_done_callback(void *args);

static em_status_t
eo_remove_queue_sync_local(const eo_elem_t *eo_elem,
			   const queue_elem_t *q_elem);
static void
eo_remove_queue_sync_done_callback(void *args);

static em_status_t
eo_remove_queue_all_local(const eo_elem_t *eo_elem, int delete_queues);
static void
eo_remove_queue_all_done_callback(void *args);

static em_status_t
eo_remove_queue_all_sync_local(const eo_elem_t *eo_elem, int delete_queues);
static void
eo_remove_queue_all_sync_done_callback(void *args);

static inline eo_elem_t *
eo_poolelem2eo(const objpool_elem_t *const eo_pool_elem)
{
	return (eo_elem_t *)((uintptr_t)eo_pool_elem -
			     offsetof(eo_elem_t, eo_pool_elem));
}

em_status_t
eo_init(eo_tbl_t eo_tbl[], eo_pool_t *eo_pool)
{
	int ret;
	const int cores = em_core_count();

	memset(eo_tbl, 0, sizeof(eo_tbl_t));
	memset(eo_pool, 0, sizeof(eo_pool_t));

	for (int i = 0; i < EM_MAX_EOS; i++) {
		eo_elem_t *const eo_elem = &eo_tbl->eo_elem[i];
		/* Store EO handle */
		eo_elem->eo = eo_idx2hdl(i);
		/* Initialize empty EO-queue list */
		env_spinlock_init(&eo_elem->lock);
		list_init(&eo_elem->queue_list);
		list_init(&eo_elem->startfn_evlist);
	}

	ret = objpool_init(&eo_pool->objpool, cores);
	if (ret != 0)
		return EM_ERR_LIB_FAILED;

	for (int i = 0; i < EM_MAX_EOS; i++)
		objpool_add(&eo_pool->objpool, i % cores,
			    &eo_tbl->eo_elem[i].eo_pool_elem);

	env_atomic32_init(&em_shm->eo_count);

	return EM_OK;
}

em_eo_t
eo_alloc(void)
{
	const eo_elem_t *eo_elem;
	const objpool_elem_t *eo_pool_elem;

	eo_pool_elem = objpool_rem(&em_shm->eo_pool.objpool, em_core_id());
	if (unlikely(eo_pool_elem == NULL))
		return EM_EO_UNDEF;

	eo_elem = eo_poolelem2eo(eo_pool_elem);
	env_atomic32_inc(&em_shm->eo_count);

	return eo_elem->eo;
}

em_status_t
eo_free(em_eo_t eo)
{
	eo_elem_t *eo_elem = eo_elem_get(eo);

	if (unlikely(eo_elem == NULL))
		return EM_ERR_BAD_ID;

	eo_elem->state = EM_EO_STATE_UNDEF;

	objpool_add(&em_shm->eo_pool.objpool,
		    eo_elem->eo_pool_elem.subpool_idx, &eo_elem->eo_pool_elem);
	env_atomic32_dec(&em_shm->eo_count);

	return EM_OK;
}

/**
 * Add a queue to an EO
 */
em_status_t
eo_add_queue(eo_elem_t *const eo_elem, queue_elem_t *const q_elem)
{
	queue_state_t old_state = q_elem->state;
	queue_state_t new_state = EM_QUEUE_STATE_BIND;
	em_status_t err;

	err = queue_state_change__check(old_state, new_state, 1/*is_setup*/);
	if (unlikely(err != EM_OK))
		return err;

	q_elem->use_multi_rcv = eo_elem->use_multi_rcv;
	q_elem->max_events = eo_elem->max_events;
	q_elem->receive_func = eo_elem->receive_func;
	q_elem->receive_multi_func = eo_elem->receive_multi_func;

	q_elem->eo = eo_elem->eo;
	q_elem->eo_ctx = eo_elem->eo_ctx;
	q_elem->eo_elem = eo_elem;
	q_elem->state = new_state;

	/* Link the new queue into the EO's queue-list */
	env_spinlock_lock(&eo_elem->lock);
	list_add(&eo_elem->queue_list, &q_elem->queue_node);
	env_atomic32_inc(&eo_elem->num_queues);
	env_spinlock_unlock(&eo_elem->lock);

	return EM_OK;
}

static inline em_status_t
eo_rem_queue_locked(eo_elem_t *const eo_elem, queue_elem_t *const q_elem)
{
	queue_state_t old_state = q_elem->state;
	queue_state_t new_state = EM_QUEUE_STATE_INIT;
	em_status_t err;

	err = queue_state_change__check(old_state, new_state, 0/*!is_setup*/);
	if (unlikely(err != EM_OK))
		return err;

	list_rem(&eo_elem->queue_list, &q_elem->queue_node);
	env_atomic32_dec(&eo_elem->num_queues);

	q_elem->state = new_state;
	q_elem->eo = EM_EO_UNDEF;
	q_elem->eo_elem = NULL;

	return EM_OK;
}

/**
 * Remove a queue from an EO
 */
em_status_t
eo_rem_queue(eo_elem_t *const eo_elem, queue_elem_t *const q_elem)
{
	em_status_t err;

	env_spinlock_lock(&eo_elem->lock);
	err = eo_rem_queue_locked(eo_elem, q_elem);
	env_spinlock_unlock(&eo_elem->lock);

	if (unlikely(err != EM_OK))
		return err;

	return EM_OK;
}

/*
 * Remove all queues associated with the EO.
 * Note: does not delete the queues.
 */
em_status_t
eo_rem_queue_all(eo_elem_t *const eo_elem)
{
	em_status_t err = EM_OK;
	queue_elem_t *q_elem;

	list_node_t *pos;
	const list_node_t *list_node;

	env_spinlock_lock(&eo_elem->lock);

	/* Loop through all queues associated with the EO */
	list_for_each(&eo_elem->queue_list, pos, list_node) {
		q_elem = list_node_to_queue_elem(list_node);
		/* remove the queue from the EO */
		err = eo_rem_queue_locked(eo_elem, q_elem);
		if (unlikely(err != EM_OK))
			break;
	} /* end loop */

	env_spinlock_unlock(&eo_elem->lock);

	return err;
}

/*
 * Delete all queues associated with the EO.
 * The queue needs to be removed from the EO before the actual delete.
 */
em_status_t
eo_delete_queue_all(eo_elem_t *const eo_elem)
{
	em_status_t err = EM_OK;
	queue_elem_t *q_elem;

	list_node_t *pos;
	const list_node_t *list_node;

	env_spinlock_lock(&eo_elem->lock);

	/* Loop through all queues associated with the EO */
	list_for_each(&eo_elem->queue_list, pos, list_node) {
		q_elem = list_node_to_queue_elem(list_node);
		/* remove the queue from the EO */
		err = eo_rem_queue_locked(eo_elem, q_elem);
		if (unlikely(err != EM_OK))
			break;
		/* delete the queue */
		err = queue_delete(q_elem);
		if (unlikely(err != EM_OK))
			break;
	} /* end loop */

	env_spinlock_unlock(&eo_elem->lock);

	return err;
}

em_status_t
eo_start_local_req(eo_elem_t *const eo_elem,
		   int num_notif, const em_notif_t notif_tbl[])
{
	eo_local_func_call_param_t param;

	eo_local_func_call_param_init(&param);
	param.eo_elem = eo_elem;
	param.q_elem = NULL; /* no q_elem */
	param.delete_queues = EM_FALSE;
	param.ev_id = EO_START_LOCAL_REQ;
	param.f_done_callback = eo_start_done_callback;
	param.num_notif = num_notif;
	param.notif_tbl = notif_tbl;
	param.exclude_current_core = EM_FALSE; /* all cores */
	param.sync_operation = false;

	return eo_local_func_call_req(&param);
}

/**
 * Callback function run when all start_local functions are finished,
 * triggered by calling em_eo_start() when using local-start functions
 */
static void
eo_start_done_callback(void *args)
{
	const loc_func_retval_t *loc_func_retvals = args;
	eo_elem_t *const eo_elem = loc_func_retvals->eo_elem;
	em_status_t ret;

	if (unlikely(eo_elem == NULL)) {
		INTERNAL_ERROR(EM_FATAL(EM_ERR_BAD_POINTER),
			       EM_ESCOPE_EO_START_DONE_CB,
			       "eo_elem is NULL!");
		return;
	}

	if (check_eo_local_status(loc_func_retvals) == EM_OK) {
		ret = queue_enable_all(eo_elem); /* local starts OK */
		if (ret == EM_OK)
			eo_elem->state = EM_EO_STATE_RUNNING;
	}

	/* free the storage for local func return values */
	em_free(loc_func_retvals->event);

	/* Send events buffered during the EO-start/local-start functions */
	eo_start_send_buffered_events(eo_elem);
}

em_status_t
eo_start_sync_local_req(eo_elem_t *const eo_elem)
{
	eo_local_func_call_param_t param;

	eo_local_func_call_param_init(&param);
	param.eo_elem = eo_elem;
	param.q_elem = NULL; /* no q_elem */
	param.delete_queues = EM_FALSE;
	param.ev_id = EO_START_SYNC_LOCAL_REQ;
	param.f_done_callback = eo_start_sync_done_callback;
	param.num_notif = 0;
	param.notif_tbl = NULL;
	param.exclude_current_core = EM_TRUE; /* exclude this core */
	param.sync_operation = true;

	return eo_local_func_call_req(&param);
}

/**
 * Callback function run when all start_local functions are finished,
 * triggered by calling em_eo_start_sync() when using local-start functions
 */
static void
eo_start_sync_done_callback(void *args)
{
	em_locm_t *const locm = &em_locm;
	const loc_func_retval_t *loc_func_retvals = args;
	eo_elem_t *const eo_elem = loc_func_retvals->eo_elem;
	em_status_t ret;

	if (unlikely(eo_elem == NULL)) {
		INTERNAL_ERROR(EM_FATAL(EM_ERR_BAD_POINTER),
			       EM_ESCOPE_EO_START_SYNC_DONE_CB,
			       "eo_elem is NULL!");
		return;
	}

	if (check_eo_local_status(loc_func_retvals) == EM_OK) {
		ret = queue_enable_all(eo_elem); /* local starts OK */
		if (ret == EM_OK)
			eo_elem->state = EM_EO_STATE_RUNNING;
	}

	/* free the storage for local func return values */
	em_free(loc_func_retvals->event);

	/* Enable the caller of the sync API func to proceed (on this core) */
	locm->sync_api.in_progress = false;

	/*
	 * Events buffered during the EO-start/local-start functions are sent
	 * from em_eo_start_sync() after this.
	 */
}

/**
 * Called by em_send() & variants during an EO start-function.
 *
 * Events sent from within the EO-start functions are buffered and sent
 * after the start-operation has completed. Otherwise it would not be
 * possible to reliably send events from the start-functions to the
 * EO's own queues.
 */
int
eo_start_buffer_events(const em_event_t events[], int num, em_queue_t queue,
		       em_event_group_t event_group)
{
	event_hdr_t *ev_hdrs[num];
	eo_elem_t *const eo_elem = em_locm.start_eo_elem;
	int i;

	if (unlikely(eo_elem == NULL))
		return 0;

	event_to_hdr_multi(events, ev_hdrs, num);

	env_spinlock_lock(&eo_elem->lock);

	for (i = 0; i < num; i++) {
		ev_hdrs[i]->egrp = event_group;
		ev_hdrs[i]->queue = queue;
		list_add(&eo_elem->startfn_evlist, &ev_hdrs[i]->start_node);
	}

	env_spinlock_unlock(&eo_elem->lock);

	return num;
}

/**
 * Send the buffered events at the end of the EO-start operation.
 *
 * Events sent from within the EO-start functions are buffered and sent
 * after the start-operation has completed. Otherwise it would not be
 * possible to reliably send events from the start-functions to the
 * EO's own queues.
 */
void
eo_start_send_buffered_events(eo_elem_t *const eo_elem)
{
	list_node_t *pos;
	list_node_t *start_node;
	const event_hdr_t *ev_hdr;
	const event_hdr_t *tmp_hdr;
	em_event_t event;
	em_event_t tmp_event;
	em_queue_t queue;
	em_queue_t tmp_queue;
	em_event_group_t event_group;
	em_event_group_t tmp_evgrp;
	unsigned int ev_cnt;
	unsigned int num_sent;
	unsigned int i;
	/* max events to send in a burst */
	const unsigned int max_ev = 32;
	/* event burst storage, taken from stack, keep size reasonable */
	em_event_t events[max_ev];

	env_spinlock_lock(&eo_elem->lock);

	/*
	 * Send the buffered events in bursts into the destination queue.
	 *
	 * This is startup: we can use some extra cycles to create the
	 * event-arrays to send in bursts.
	 */
	while (!list_is_empty(&eo_elem->startfn_evlist)) {
		/*
		 * The first event of the burst determines the destination queue
		 * and the event group to use in em_send_group_multi() later on.
		 */
		start_node = list_rem_first(&eo_elem->startfn_evlist);
		ev_hdr = start_node_to_event_hdr(start_node);
		event_group = ev_hdr->egrp;
		queue = ev_hdr->queue;
		event = event_hdr_to_event(ev_hdr);
		ev_cnt = 1;

		/* count events sent to the same queue with same event group */
		list_for_each(&eo_elem->startfn_evlist, pos, start_node) {
			tmp_hdr = start_node_to_event_hdr(start_node);
			tmp_evgrp = tmp_hdr->egrp;
			tmp_queue = tmp_hdr->queue;
			if (tmp_evgrp != event_group ||
			    tmp_queue != queue)
				break;
			/* increment the event burst count and break on max */
			ev_cnt++;
			if (ev_cnt == max_ev)
				break;
		}

		/*
		 * fill the array of events to be sent to the same queue
		 *	note: ev_cnt <= max_ev
		 */
		events[0] = event;
		for (i = 1; i < ev_cnt; i++) {
			start_node = list_rem_first(&eo_elem->startfn_evlist);
			tmp_hdr = start_node_to_event_hdr(start_node);
			tmp_event = event_hdr_to_event(tmp_hdr);
			events[i] = tmp_event;
		}
		/* send events with same destination queue and event group */
		if (event_group == EM_EVENT_GROUP_UNDEF)
			num_sent = em_send_multi(events, ev_cnt, queue);
		else
			num_sent = em_send_group_multi(events, ev_cnt, queue,
						       event_group);
		if (unlikely(num_sent != ev_cnt)) {
			/* User's eo-start saw successful em_send, free here */
			for (i = num_sent; i < ev_cnt; i++)
				em_free(events[i]);
			INTERNAL_ERROR(EM_ERR_LIB_FAILED, EM_ESCOPE_EO_START,
				       "Q:%" PRI_QUEUE " req:%u sent:%u",
				       queue, ev_cnt, num_sent);
		}
	}

	list_init(&eo_elem->startfn_evlist); /* reset list for this eo_elem */
	env_spinlock_unlock(&eo_elem->lock);
}

em_status_t
eo_stop_local_req(eo_elem_t *const eo_elem,
		  int num_notif, const em_notif_t notif_tbl[])
{
	eo_local_func_call_param_t param;

	eo_local_func_call_param_init(&param);
	param.eo_elem = eo_elem;
	param.q_elem = NULL; /* no q_elem */
	param.delete_queues = EM_FALSE;
	param.ev_id = EO_STOP_LOCAL_REQ;
	param.f_done_callback = eo_stop_done_callback;
	param.num_notif = num_notif;
	param.notif_tbl = notif_tbl;
	param.exclude_current_core = EM_FALSE; /* all cores */
	param.sync_operation = false;

	return eo_local_func_call_req(&param);
}

/**
 * Callback function run when all stop_local functions are finished,
 * triggered by calling eo_eo_stop().
 */
static void
eo_stop_done_callback(void *args)
{
	em_locm_t *const locm = &em_locm;
	const loc_func_retval_t *loc_func_retvals = args;
	eo_elem_t *const eo_elem = loc_func_retvals->eo_elem;
	void *const eo_ctx = eo_elem->eo_ctx;
	queue_elem_t *const save_q_elem = locm->current.q_elem;
	queue_elem_t tmp_q_elem;
	em_eo_t eo;
	em_status_t ret;

	if (unlikely(eo_elem == NULL)) {
		INTERNAL_ERROR(EM_FATAL(EM_ERR_BAD_POINTER),
			       EM_ESCOPE_EO_STOP_DONE_CB,
			       "eo_elem is NULL!");
		return;
	}

	eo = eo_elem->eo;
	(void)check_eo_local_status(loc_func_retvals);

	/* Change state here to allow em_eo_delete() from EO global stop */
	eo_elem->state = EM_EO_STATE_CREATED; /* == EO_STATE_STOPPED */

	/*
	 * Use a tmp q_elem as the 'current q_elem' to enable calling
	 * em_eo_current() from the EO stop functions.
	 * Before returning, restore the original 'current q_elem' from
	 * 'save_q_elem'.
	 */
	memset(&tmp_q_elem, 0, sizeof(tmp_q_elem));
	tmp_q_elem.eo = eo;

	locm->current.q_elem = &tmp_q_elem;
	/*
	 * Call the Global EO stop function now that all
	 * EO local stop functions are done.
	 */
	ret = eo_elem->stop_func(eo_ctx, eo);
	/* Restore the original 'current q_elem' */
	locm->current.q_elem = save_q_elem;

	/*
	 * Note: the EO might not be available after this if the EO global stop
	 * called em_eo_delete()!
	 */

	if (unlikely(ret != EM_OK))
		INTERNAL_ERROR(ret, EM_ESCOPE_EO_STOP_DONE_CB,
			       "EO:%" PRI_EO " stop-func failed", eo);

	/* free the storage for local func return values */
	em_free(loc_func_retvals->event);
}

em_status_t
eo_stop_sync_local_req(eo_elem_t *const eo_elem)
{
	eo_local_func_call_param_t param;

	eo_local_func_call_param_init(&param);
	param.eo_elem = eo_elem;
	param.q_elem = NULL; /* no q_elem */
	param.delete_queues = EM_FALSE;
	param.ev_id = EO_STOP_SYNC_LOCAL_REQ;
	param.f_done_callback = eo_stop_sync_done_callback;
	param.num_notif = 0;
	param.notif_tbl = NULL;
	param.exclude_current_core = EM_TRUE; /* exclude this core */
	param.sync_operation = true;

	return eo_local_func_call_req(&param);
}

/**
 * Callback function run when all stop_local functions are finished,
 * triggered by calling eo_eo_stop_sync().
 */
static void
eo_stop_sync_done_callback(void *args)
{
	em_locm_t *const locm = &em_locm;
	const loc_func_retval_t *loc_func_retvals = args;
	const eo_elem_t *eo_elem = loc_func_retvals->eo_elem;

	if (unlikely(eo_elem == NULL)) {
		INTERNAL_ERROR(EM_FATAL(EM_ERR_BAD_POINTER),
			       EM_ESCOPE_EO_STOP_SYNC_DONE_CB,
			       "eo_elem is NULL!");
		/* Enable the caller of the sync API func to proceed */
		locm->sync_api.in_progress = false;
		return;
	}

	(void)check_eo_local_status(loc_func_retvals);

	/* free the storage for local func return values */
	em_free(loc_func_retvals->event);

	/* Enable the caller of the sync API func to proceed (on this core) */
	locm->sync_api.in_progress = false;
}

em_status_t
eo_remove_queue_local_req(eo_elem_t *const eo_elem, queue_elem_t *const q_elem,
			  int num_notif, const em_notif_t notif_tbl[])
{
	eo_local_func_call_param_t param;

	eo_local_func_call_param_init(&param);
	param.eo_elem = eo_elem;
	param.q_elem = q_elem;
	param.delete_queues = EM_FALSE;
	param.ev_id = EO_REM_QUEUE_LOCAL_REQ;
	param.f_done_callback = eo_remove_queue_done_callback;
	param.num_notif = num_notif;
	param.notif_tbl = notif_tbl;
	param.exclude_current_core = EM_FALSE; /* all cores */
	param.sync_operation = false;

	return eo_local_func_call_req(&param);
}

static em_status_t
eo_remove_queue_local(const eo_elem_t *eo_elem, const queue_elem_t *q_elem)
{
	(void)eo_elem;
	(void)q_elem;

	return EM_OK;
}

static void
eo_remove_queue_done_callback(void *args)
{
	const loc_func_retval_t *loc_func_retvals = args;
	eo_elem_t *const eo_elem = loc_func_retvals->eo_elem;
	queue_elem_t *const q_elem = loc_func_retvals->q_elem;
	em_status_t ret;

	if (unlikely(eo_elem == NULL || q_elem == NULL)) {
		INTERNAL_ERROR(EM_FATAL(EM_ERR_BAD_POINTER),
			       EM_ESCOPE_EO_REMOVE_QUEUE_DONE_CB,
			       "eo_elem/q_elem is NULL!");
		return;
	}

	(void)check_eo_local_status(loc_func_retvals);

	/* Remove the queue from the EO */
	ret = eo_rem_queue(eo_elem, q_elem);

	if (unlikely(ret != EM_OK))
		INTERNAL_ERROR(ret, EM_ESCOPE_EO_REMOVE_QUEUE_DONE_CB,
			       "EO:%" PRI_EO " remove Q:%" PRI_QUEUE " failed",
			       eo_elem->eo, q_elem->queue);

	/* free the storage for local func return values */
	em_free(loc_func_retvals->event);
}

em_status_t
eo_remove_queue_sync_local_req(eo_elem_t *const eo_elem,
			       queue_elem_t *const q_elem)
{
	eo_local_func_call_param_t param;

	eo_local_func_call_param_init(&param);
	param.eo_elem = eo_elem;
	param.q_elem = q_elem;
	param.delete_queues = EM_FALSE;
	param.ev_id = EO_REM_QUEUE_SYNC_LOCAL_REQ;
	param.f_done_callback = eo_remove_queue_sync_done_callback;
	param.num_notif = 0;
	param.notif_tbl = NULL;
	param.exclude_current_core = EM_TRUE; /* exclude this core */
	param.sync_operation = true;

	return eo_local_func_call_req(&param);
}

static em_status_t
eo_remove_queue_sync_local(const eo_elem_t *eo_elem, const queue_elem_t *q_elem)
{
	(void)eo_elem;
	(void)q_elem;

	return EM_OK;
}

static void
eo_remove_queue_sync_done_callback(void *args)
{
	em_locm_t *const locm = &em_locm;
	const loc_func_retval_t *loc_func_retvals = args;
	eo_elem_t *const eo_elem = loc_func_retvals->eo_elem;
	queue_elem_t *const q_elem = loc_func_retvals->q_elem;
	em_status_t ret;

	if (unlikely(eo_elem == NULL || q_elem == NULL)) {
		INTERNAL_ERROR(EM_FATAL(EM_ERR_BAD_POINTER),
			       EM_ESCOPE_EO_REMOVE_QUEUE_SYNC_DONE_CB,
			       "eo_elem/q_elem is NULL!");
		/* Enable the caller of the sync API func to proceed */
		locm->sync_api.in_progress = false;
		return;
	}

	(void)check_eo_local_status(loc_func_retvals);

	/* Remove the queue from the EO */
	ret = eo_rem_queue(eo_elem, q_elem);

	if (unlikely(ret != EM_OK))
		INTERNAL_ERROR(ret,
			       EM_ESCOPE_EO_REMOVE_QUEUE_SYNC_DONE_CB,
			       "EO:%" PRI_EO " remove Q:%" PRI_QUEUE " failed",
			       eo_elem->eo, q_elem->queue);

	/* free the storage for local func return values */
	em_free(loc_func_retvals->event);

	/* Enable the caller of the sync API func to proceed (on this core) */
	locm->sync_api.in_progress = false;
}

em_status_t
eo_remove_queue_all_local_req(eo_elem_t *const eo_elem, int delete_queues,
			      int num_notif, const em_notif_t notif_tbl[])
{
	eo_local_func_call_param_t param;

	eo_local_func_call_param_init(&param);
	param.eo_elem = eo_elem;
	param.q_elem = NULL; /* no q_elem */
	param.delete_queues = delete_queues;
	param.ev_id = EO_REM_QUEUE_ALL_LOCAL_REQ;
	param.f_done_callback = eo_remove_queue_all_done_callback;
	param.num_notif = num_notif;
	param.notif_tbl = notif_tbl;
	param.exclude_current_core = EM_FALSE; /* all cores */
	param.sync_operation = false;

	return eo_local_func_call_req(&param);
}

static em_status_t
eo_remove_queue_all_local(const eo_elem_t *eo_elem, int delete_queues)
{
	(void)eo_elem;
	(void)delete_queues;

	return EM_OK;
}

static void
eo_remove_queue_all_done_callback(void *args)
{
	const loc_func_retval_t *loc_func_retvals = args;
	eo_elem_t *const eo_elem = loc_func_retvals->eo_elem;
	int delete_queues = loc_func_retvals->delete_queues;
	em_status_t ret;

	if (unlikely(eo_elem == NULL)) {
		INTERNAL_ERROR(EM_FATAL(EM_ERR_BAD_POINTER),
			       EM_ESCOPE_EO_REMOVE_QUEUE_ALL_DONE_CB,
			       "eo_elem is NULL!");
		return;
	}

	(void)check_eo_local_status(loc_func_retvals);

	/* Remove or delete all the EO's queues */
	if (delete_queues)
		ret = eo_delete_queue_all(eo_elem);
	else
		ret = eo_rem_queue_all(eo_elem);

	if (unlikely(ret != EM_OK))
		INTERNAL_ERROR(ret, EM_ESCOPE_EO_REMOVE_QUEUE_ALL_DONE_CB,
			       "EO:%" PRI_EO " removing all queues failed",
			       eo_elem->eo);

	/* free the storage for local func return values */
	em_free(loc_func_retvals->event);
}

em_status_t
eo_remove_queue_all_sync_local_req(eo_elem_t *const eo_elem, int delete_queues)
{
	eo_local_func_call_param_t param;

	eo_local_func_call_param_init(&param);
	param.eo_elem = eo_elem;
	param.q_elem = NULL; /* no q_elem */
	param.delete_queues = delete_queues;
	param.ev_id = EO_REM_QUEUE_ALL_SYNC_LOCAL_REQ;
	param.f_done_callback = eo_remove_queue_all_sync_done_callback;
	param.num_notif = 0;
	param.notif_tbl = NULL;
	param.exclude_current_core = EM_TRUE; /* exclude this core */
	param.sync_operation = true;

	return eo_local_func_call_req(&param);
}

static em_status_t
eo_remove_queue_all_sync_local(const eo_elem_t *eo_elem, int delete_queues)
{
	(void)eo_elem;
	(void)delete_queues;

	return EM_OK;
}

static void
eo_remove_queue_all_sync_done_callback(void *args)
{
	em_locm_t *const locm = &em_locm;
	const loc_func_retval_t *loc_func_retvals = args;
	eo_elem_t *const eo_elem = loc_func_retvals->eo_elem;
	int delete_queues = loc_func_retvals->delete_queues;
	em_status_t ret;

	if (unlikely(eo_elem == NULL)) {
		INTERNAL_ERROR(EM_FATAL(EM_ERR_BAD_POINTER),
			       EM_ESCOPE_EO_REMOVE_QUEUE_ALL_SYNC_DONE_CB,
			       "eo_elem is NULL!");
		/* Enable the caller of the sync API func to proceed */
		locm->sync_api.in_progress = false;
		return;
	}

	(void)check_eo_local_status(loc_func_retvals);

	/* Remove or delete all the EO's queues */
	if (delete_queues)
		ret = eo_delete_queue_all(eo_elem);
	else
		ret = eo_rem_queue_all(eo_elem);

	if (unlikely(ret != EM_OK))
		INTERNAL_ERROR(ret,
			       EM_ESCOPE_EO_REMOVE_QUEUE_ALL_SYNC_DONE_CB,
			       "EO:%" PRI_EO " removing all queues failed",
			       eo_elem->eo);

	/* free the storage for local func return values */
	em_free(loc_func_retvals->event);

	/* Enable the caller of the sync API func to proceed (on this core) */
	locm->sync_api.in_progress = false;
}

static em_status_t
check_eo_local_status(const loc_func_retval_t *loc_func_retvals)
{
	const int cores = em_core_count();
	static const char core_err[] = "coreXX:0x12345678 ";
	char errmsg[cores * sizeof(core_err)];
	int n = 0;
	int c = 0;
	int local_fail = 0;
	em_status_t err;

	for (int i = 0; i < cores; i++) {
		err = loc_func_retvals->core[i];
		if (err != EM_OK) {
			local_fail = 1;
			break;
		}
	}

	if (!local_fail)
		return EM_OK;

	for (int i = 0; i < cores; i++) {
		err = loc_func_retvals->core[i];
		if (err != EM_OK) {
			n = snprintf(&errmsg[c], sizeof(core_err),
				     "core%02d:0x%08X ", i, err);
			if ((unsigned int)n >= sizeof(core_err))
				break;
			c += n;
		}
	}
	errmsg[cores * sizeof(core_err) - 1] = '\0';

	INTERNAL_ERROR(EM_ERR, EM_ESCOPE_EVENT_INTERNAL_LFUNC_CALL,
		       "\nLocal start function failed on cores:\n"
		       "%s", errmsg);
	return EM_ERR;
}

static void
eo_local_func_call_param_init(eo_local_func_call_param_t *param)
{
	memset(param, 0, sizeof(*param));
}

/**
 * Request a function to be run on each core and call 'f_done_callback(arg_ptr)'
 * when all those functions have completed.
 */
static em_status_t
eo_local_func_call_req(const eo_local_func_call_param_t *param)
{
	int err;
	em_event_t event;
	em_event_t tmp;
	internal_event_t *i_event;
	int core_count;
	int free_count;
	em_core_mask_t core_mask;
	loc_func_retval_t *loc_func_retvals;
	void *f_done_arg_ptr;

	core_count = em_core_count();
	em_core_mask_zero(&core_mask);
	em_core_mask_set_count(core_count, &core_mask);
	free_count = core_count + 1; /* all cores + 'done' event */
	if (param->exclude_current_core) {
		/* EM _sync API func: exclude the calling core */
		em_core_mask_clr(em_core_id(), &core_mask);
		free_count -= 1;
	}

	event = em_alloc(sizeof(internal_event_t),
			 EM_EVENT_TYPE_SW, EM_POOL_DEFAULT);
	RETURN_ERROR_IF(event == EM_EVENT_UNDEF,
			EM_ERR_ALLOC_FAILED, EM_ESCOPE_EO_LOCAL_FUNC_CALL_REQ,
			"Internal event (%u) allocation failed", param->ev_id);
	i_event = em_event_pointer(event);
	i_event->id = param->ev_id;
	i_event->loc_func.eo_elem = param->eo_elem;
	i_event->loc_func.q_elem = param->q_elem;
	i_event->loc_func.delete_queues = param->delete_queues;

	tmp = em_alloc(sizeof(loc_func_retval_t),
		       EM_EVENT_TYPE_SW, EM_POOL_DEFAULT);
	RETURN_ERROR_IF(tmp == EM_EVENT_UNDEF,
			EM_ERR_ALLOC_FAILED, EM_ESCOPE_EO_LOCAL_FUNC_CALL_REQ,
			"Internal loc_func_retval_t allocation failed");
	loc_func_retvals = em_event_pointer(tmp);
	loc_func_retvals->eo_elem = param->eo_elem;
	loc_func_retvals->q_elem = param->q_elem;
	loc_func_retvals->delete_queues = param->delete_queues;
	loc_func_retvals->event = tmp; /* store event handle for em_free() */
	env_atomic32_init(&loc_func_retvals->free_at_zero);
	env_atomic32_set(&loc_func_retvals->free_at_zero, free_count);
	for (int i = 0; i < core_count; i++)
		loc_func_retvals->core[i] = EM_OK;

	/* ptr to retval storage so loc func calls can record retval there */
	i_event->loc_func.retvals = loc_func_retvals;

	/* Give ptr to retval storage also to 'done' function */
	f_done_arg_ptr = loc_func_retvals;

	if (em_core_mask_iszero(&core_mask)) {
		/*
		 * Special handling when calling sync APIs with one core in use.
		 * Need to call both local- and done-funcs here and return.
		 */
		env_atomic32_inc(&loc_func_retvals->free_at_zero);
		i_event__eo_local_func_call_req(i_event);
		em_free(event);
		param->f_done_callback(f_done_arg_ptr);

		return EM_OK;
	}

	err = send_core_ctrl_events(&core_mask, event,
				    param->f_done_callback, f_done_arg_ptr,
				    param->num_notif, param->notif_tbl,
				    param->sync_operation);
	if (unlikely(err)) {
		char core_mask_str[EM_CORE_MASK_STRLEN];
		uint32_t unsent_cnt = err;
		uint32_t cnt;

		em_free(event);
		cnt = env_atomic32_sub_return(&loc_func_retvals->free_at_zero,
					      unsent_cnt + 1);
		if (cnt == 0)
			em_free(tmp);

		em_core_mask_tostr(core_mask_str, EM_CORE_MASK_STRLEN,
				   &core_mask);
		return INTERNAL_ERROR(EM_ERR_LIB_FAILED,
				      EM_ESCOPE_EO_LOCAL_FUNC_CALL_REQ,
				      "send_core_ctrl_events(mask=%s) failed",
				      core_mask_str);
	}

	return EM_OK;
}

/**
 * EM internal event handler (see em_internal_event.c&h)
 * Handle the internal event requesting a local function call.
 */
void
i_event__eo_local_func_call_req(const internal_event_t *i_ev)
{
	em_locm_t *const locm = &em_locm;
	const uint64_t f_type = i_ev->loc_func.id;
	eo_elem_t *eo_elem = i_ev->loc_func.eo_elem;
	const queue_elem_t *q_elem = i_ev->loc_func.q_elem;
	int delete_queues = i_ev->loc_func.delete_queues;
	loc_func_retval_t *const loc_func_retvals = i_ev->loc_func.retvals;
	em_status_t status = EM_ERR;
	queue_elem_t *const save_q_elem = locm->current.q_elem;
	queue_elem_t tmp_q_elem;

	switch (f_type) {
	case EO_START_SYNC_LOCAL_REQ:
		if (em_core_count() == 1) {
			/*
			 * Special handling when calling sync API with only one
			 * core in use: start-local() func already called by
			 * em_eo_start_sync() and this func called directly from
			 * within eo_local_func_call_req().
			 */
			status = EM_OK;
			break;
		}
		/* fallthrough */
	case EO_START_LOCAL_REQ:
		/*
		 * Use a tmp q_elem as the 'current q_elem' to enable calling
		 * em_eo_current() from the EO start functions.
		 * Before returning, restore the original 'current q_elem' from
		 * 'save_q_elem'.
		 */
		memset(&tmp_q_elem, 0, sizeof(tmp_q_elem));
		tmp_q_elem.eo = eo_elem->eo;
		locm->current.q_elem = &tmp_q_elem;

		locm->start_eo_elem = eo_elem;
		status = eo_elem->start_local_func(eo_elem->eo_ctx,
						   eo_elem->eo);
		locm->start_eo_elem = NULL;
		/* Restore the original 'current q_elem' */
		locm->current.q_elem = save_q_elem;
		break;

	case EO_STOP_SYNC_LOCAL_REQ:
		if (em_core_count() == 1) {
			/*
			 * Special handling when calling sync API with only one
			 * core in use: stop-local() func already called by
			 * em_eo_stop_sync() and this func called directly from
			 * within eo_local_func_call_req().
			 */
			status = EM_OK;
			break;
		}
		/* fallthrough */
	case EO_STOP_LOCAL_REQ:
		if (eo_elem->stop_local_func != NULL) {
			/*
			 * Use a tmp q_elem as the 'current q_elem' to enable
			 * calling em_eo_current() from the EO start functions.
			 * Before returning, restore the original 'current
			 * q_elem' from 'save_q_elem'.
			 */
			memset(&tmp_q_elem, 0, sizeof(tmp_q_elem));
			tmp_q_elem.eo = eo_elem->eo;
			locm->current.q_elem = &tmp_q_elem;

			status = eo_elem->stop_local_func(eo_elem->eo_ctx,
							  eo_elem->eo);
			/* Restore the original 'current q_elem' */
			locm->current.q_elem = save_q_elem;
		} else {
			status = EM_OK; /* No local stop func given */
		}
		break;

	case EO_REM_QUEUE_LOCAL_REQ:
		status = eo_remove_queue_local(eo_elem, q_elem);
		break;
	case EO_REM_QUEUE_SYNC_LOCAL_REQ:
		status = eo_remove_queue_sync_local(eo_elem, q_elem);
		break;
	case EO_REM_QUEUE_ALL_LOCAL_REQ:
		status = eo_remove_queue_all_local(eo_elem, delete_queues);
		break;
	case EO_REM_QUEUE_ALL_SYNC_LOCAL_REQ:
		status = eo_remove_queue_all_sync_local(eo_elem, delete_queues);
		break;
	default:
		status = EM_FATAL(EM_ERR_BAD_ID);
		break;
	}

	if (status != EM_OK) {
		/* store failing status, egrp 'done' can check if all ok */
		loc_func_retvals->core[em_core_id()] = status;

		INTERNAL_ERROR(status, EM_ESCOPE_EVENT_INTERNAL_LFUNC_CALL,
			       "EO:%" PRI_EO "-%s:Local func(%" PRIx64 ")fail",
			       eo_elem->eo, eo_elem->name, f_type);
	}

	/*
	 * In case of setup error, determine if 'loc_func_retvals' should be
	 * freed here, in the setup code in eo_local_func_call_req() or
	 * normally in a successful case in the
	 * eo_start/stop_local__done_callback() function when the event group
	 * completion notif is handled.
	 */
	const uint32_t cnt =
		env_atomic32_sub_return(&loc_func_retvals->free_at_zero, 1);
	if (unlikely(cnt == 0)) {
		(void)check_eo_local_status(loc_func_retvals);
		em_free(loc_func_retvals->event);
	}
}

unsigned int
eo_count(void)
{
	return env_atomic32_get(&em_shm->eo_count);
}

size_t eo_get_name(const eo_elem_t *const eo_elem,
		   char name[/*out*/], const size_t maxlen)
{
	size_t len;

	len = strnlen(eo_elem->name, sizeof(eo_elem->name) - 1);
	if (maxlen - 1 < len)
		len = maxlen - 1;

	memcpy(name, eo_elem->name, len);
	name[len] = '\0';

	return len;
}
