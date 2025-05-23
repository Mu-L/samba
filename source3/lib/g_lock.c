/*
   Unix SMB/CIFS implementation.
   global locks based on dbwrap and messaging
   Copyright (C) 2009 by Volker Lendecke

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "replace.h"
#include "system/filesys.h"
#include "lib/util/server_id.h"
#include "lib/util/debug.h"
#include "lib/util/talloc_stack.h"
#include "lib/util/samba_util.h"
#include "lib/util_path.h"
#include "dbwrap/dbwrap.h"
#include "dbwrap/dbwrap_open.h"
#include "dbwrap/dbwrap_watch.h"
#include "g_lock.h"
#include "util_tdb.h"
#include "../lib/util/tevent_ntstatus.h"
#include "messages.h"
#include "serverid.h"

struct g_lock_ctx {
	struct db_context *db;
	struct messaging_context *msg;
	enum dbwrap_lock_order lock_order;
	bool busy;
};

struct g_lock {
	struct server_id exclusive;
	size_t num_shared;
	uint8_t *shared;
	uint64_t unique_lock_epoch;
	uint64_t unique_data_epoch;
	size_t datalen;
	uint8_t *data;
};

static bool g_lock_parse(uint8_t *buf, size_t buflen, struct g_lock *lck)
{
	struct server_id exclusive;
	size_t num_shared, shared_len, data_len;
	uint64_t unique_lock_epoch;
	uint64_t unique_data_epoch;

	if (buflen < (SERVER_ID_BUF_LENGTH + /* exclusive */
		      sizeof(uint64_t) +     /* unique_lock_epoch */
		      sizeof(uint64_t) +     /* unique_data_epoch */
		      sizeof(uint32_t))) {   /* num_shared */
		struct g_lock ret = {
			.exclusive.pid = 0,
			.unique_lock_epoch = generate_unique_u64(0),
			.unique_data_epoch = generate_unique_u64(0),
		};
		*lck = ret;
		return true;
	}

	server_id_get(&exclusive, buf);
	buf += SERVER_ID_BUF_LENGTH;
	buflen -= SERVER_ID_BUF_LENGTH;

	unique_lock_epoch = BVAL(buf, 0);
	buf += sizeof(uint64_t);
	buflen -= sizeof(uint64_t);

	unique_data_epoch = BVAL(buf, 0);
	buf += sizeof(uint64_t);
	buflen -= sizeof(uint64_t);

	num_shared = IVAL(buf, 0);
	buf += sizeof(uint32_t);
	buflen -= sizeof(uint32_t);

	if (num_shared > buflen/SERVER_ID_BUF_LENGTH) {
		DBG_DEBUG("num_shared=%zu, buflen=%zu\n",
			  num_shared,
			  buflen);
		return false;
	}

	shared_len = num_shared * SERVER_ID_BUF_LENGTH;
	data_len = buflen - shared_len;

	*lck = (struct g_lock) {
		.exclusive = exclusive,
		.num_shared = num_shared,
		.shared = num_shared == 0 ? NULL : buf,
		.unique_lock_epoch = unique_lock_epoch,
		.unique_data_epoch = unique_data_epoch,
		.datalen = data_len,
		.data = data_len == 0 ? NULL : buf + shared_len,
	};

	return true;
}

static void g_lock_get_shared(const struct g_lock *lck,
			      size_t i,
			      struct server_id *shared)
{
	if (i >= lck->num_shared) {
		abort();
	}
	server_id_get(shared, lck->shared + i*SERVER_ID_BUF_LENGTH);
}

static void g_lock_del_shared(struct g_lock *lck, size_t i)
{
	if (i >= lck->num_shared) {
		abort();
	}
	lck->num_shared -= 1;
	if (i < lck->num_shared) {
		memcpy(lck->shared + i*SERVER_ID_BUF_LENGTH,
		       lck->shared + lck->num_shared*SERVER_ID_BUF_LENGTH,
		       SERVER_ID_BUF_LENGTH);
	}
}

static NTSTATUS g_lock_store(
	struct db_record *rec,
	struct g_lock *lck,
	struct server_id *new_shared,
	const TDB_DATA *new_dbufs,
	size_t num_new_dbufs)
{
	uint8_t exclusive[SERVER_ID_BUF_LENGTH];
	uint8_t seqnum_buf[sizeof(uint64_t)*2];
	uint8_t sizebuf[sizeof(uint32_t)];
	uint8_t new_shared_buf[SERVER_ID_BUF_LENGTH];

	struct TDB_DATA dbufs[6 + num_new_dbufs];

	dbufs[0] = (TDB_DATA) {
		.dptr = exclusive, .dsize = sizeof(exclusive),
	};
	dbufs[1] = (TDB_DATA) {
		.dptr = seqnum_buf, .dsize = sizeof(seqnum_buf),
	};
	dbufs[2] = (TDB_DATA) {
		.dptr = sizebuf, .dsize = sizeof(sizebuf),
	};
	dbufs[3] = (TDB_DATA) {
		.dptr = lck->shared,
		.dsize = lck->num_shared * SERVER_ID_BUF_LENGTH,
	};
	dbufs[4] = (TDB_DATA) { 0 };
	dbufs[5] = (TDB_DATA) {
		.dptr = lck->data, .dsize = lck->datalen,
	};

	if (num_new_dbufs != 0) {
		memcpy(&dbufs[6],
		       new_dbufs,
		       num_new_dbufs * sizeof(TDB_DATA));
	}

	server_id_put(exclusive, lck->exclusive);
	SBVAL(seqnum_buf, 0, lck->unique_lock_epoch);
	SBVAL(seqnum_buf, 8, lck->unique_data_epoch);

	if (new_shared != NULL) {
		if (lck->num_shared >= UINT32_MAX) {
			return NT_STATUS_BUFFER_OVERFLOW;
		}

		server_id_put(new_shared_buf, *new_shared);

		dbufs[4] = (TDB_DATA) {
			.dptr = new_shared_buf,
			.dsize = sizeof(new_shared_buf),
		};

		lck->num_shared += 1;
	}

	SIVAL(sizebuf, 0, lck->num_shared);

	return dbwrap_record_storev(rec, dbufs, ARRAY_SIZE(dbufs), 0);
}

struct g_lock_ctx *g_lock_ctx_init_backend(
	TALLOC_CTX *mem_ctx,
	struct messaging_context *msg,
	struct db_context **backend)
{
	struct g_lock_ctx *result;

	result = talloc_zero(mem_ctx, struct g_lock_ctx);
	if (result == NULL) {
		return NULL;
	}
	result->msg = msg;
	result->lock_order = DBWRAP_LOCK_ORDER_NONE;

	result->db = db_open_watched(result, backend, msg);
	if (result->db == NULL) {
		DBG_WARNING("db_open_watched failed\n");
		TALLOC_FREE(result);
		return NULL;
	}
	return result;
}

void g_lock_set_lock_order(struct g_lock_ctx *ctx,
			   enum dbwrap_lock_order lock_order)
{
	ctx->lock_order = lock_order;
}

struct g_lock_ctx *g_lock_ctx_init(TALLOC_CTX *mem_ctx,
				   struct messaging_context *msg)
{
	char *db_path = NULL;
	struct db_context *backend = NULL;
	struct g_lock_ctx *ctx = NULL;

	db_path = lock_path(mem_ctx, "g_lock.tdb");
	if (db_path == NULL) {
		return NULL;
	}

	backend = db_open(
		mem_ctx,
		db_path,
		0,
		TDB_CLEAR_IF_FIRST|TDB_INCOMPATIBLE_HASH|TDB_VOLATILE,
		O_RDWR|O_CREAT,
		0600,
		DBWRAP_LOCK_ORDER_3,
		DBWRAP_FLAG_NONE);
	TALLOC_FREE(db_path);
	if (backend == NULL) {
		DBG_WARNING("Could not open g_lock.tdb\n");
		return NULL;
	}

	ctx = g_lock_ctx_init_backend(mem_ctx, msg, &backend);
	return ctx;
}

static void g_lock_cleanup_dead(
	struct g_lock *lck,
	struct server_id *dead_blocker)
{
	bool exclusive_died;
	struct server_id_buf tmp;

	if (dead_blocker == NULL) {
		return;
	}

	exclusive_died = server_id_equal(dead_blocker, &lck->exclusive);

	if (exclusive_died) {
		DBG_DEBUG("Exclusive holder %s died\n",
			  server_id_str_buf(lck->exclusive, &tmp));
		lck->exclusive.pid = 0;
	}

	if (lck->num_shared != 0) {
		bool shared_died;
		struct server_id shared;

		g_lock_get_shared(lck, 0, &shared);
		shared_died = server_id_equal(dead_blocker, &shared);

		if (shared_died) {
			DBG_DEBUG("Shared holder %s died\n",
				  server_id_str_buf(shared, &tmp));
			g_lock_del_shared(lck, 0);
		}
	}
}

static ssize_t g_lock_find_shared(
	struct g_lock *lck,
	const struct server_id *self)
{
	size_t i;

	for (i=0; i<lck->num_shared; i++) {
		struct server_id shared;
		bool same;

		g_lock_get_shared(lck, i, &shared);

		same = server_id_equal(self, &shared);
		if (same) {
			return i;
		}
	}

	return -1;
}

static void g_lock_cleanup_shared(struct g_lock *lck)
{
	size_t i;
	struct server_id check;
	bool exists;

	if (lck->num_shared == 0) {
		return;
	}

	/*
	 * Read locks can stay around forever if the process dies. Do
	 * a heuristic check for process existence: Check one random
	 * process for existence. Hopefully this will keep runaway
	 * read locks under control.
	 */
	i = generate_random() % lck->num_shared;
	g_lock_get_shared(lck, i, &check);

	exists = serverid_exists(&check);
	if (!exists) {
		struct server_id_buf tmp;
		DBG_DEBUG("Shared locker %s died -- removing\n",
			  server_id_str_buf(check, &tmp));
		g_lock_del_shared(lck, i);
	}
}

struct g_lock_lock_cb_state {
	struct g_lock_ctx *ctx;
	struct db_record *rec;
	struct g_lock *lck;
	struct server_id *new_shared;
	g_lock_lock_cb_fn_t cb_fn;
	void *cb_private;
	TALLOC_CTX *update_mem_ctx;
	TDB_DATA updated_data;
	bool existed;
	bool modified;
	bool unlock;
};

NTSTATUS g_lock_lock_cb_dump(struct g_lock_lock_cb_state *cb_state,
			     void (*fn)(struct server_id exclusive,
					size_t num_shared,
					const struct server_id *shared,
					const uint8_t *data,
					size_t datalen,
					void *private_data),
			     void *private_data)
{
	struct g_lock *lck = cb_state->lck;

	/* We allow a cb_fn only for G_LOCK_WRITE for now... */
	SMB_ASSERT(lck->num_shared == 0);

	fn(lck->exclusive,
	   0, /* num_shared */
	   NULL, /* shared */
	   lck->data,
	   lck->datalen,
	   private_data);

	return NT_STATUS_OK;
}

NTSTATUS g_lock_lock_cb_writev(struct g_lock_lock_cb_state *cb_state,
			       const TDB_DATA *dbufs,
			       size_t num_dbufs)
{
	NTSTATUS status;

	status = dbwrap_merge_dbufs(&cb_state->updated_data,
				    cb_state->update_mem_ctx,
				    dbufs, num_dbufs);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	cb_state->modified = true;
	cb_state->lck->data = cb_state->updated_data.dptr;
	cb_state->lck->datalen = cb_state->updated_data.dsize;

	return NT_STATUS_OK;
}

void g_lock_lock_cb_unlock(struct g_lock_lock_cb_state *cb_state)
{
	cb_state->unlock = true;
}

struct g_lock_lock_cb_watch_data_state {
	struct tevent_context *ev;
	struct g_lock_ctx *ctx;
	TDB_DATA key;
	struct server_id blocker;
	bool blockerdead;
	uint64_t unique_lock_epoch;
	uint64_t unique_data_epoch;
	uint64_t watch_instance;
	NTSTATUS status;
};

static void g_lock_lock_cb_watch_data_done(struct tevent_req *subreq);

struct tevent_req *g_lock_lock_cb_watch_data_send(
	TALLOC_CTX *mem_ctx,
	struct tevent_context *ev,
	struct g_lock_lock_cb_state *cb_state,
	struct server_id blocker)
{
	struct tevent_req *req = NULL;
	struct g_lock_lock_cb_watch_data_state *state = NULL;
	struct tevent_req *subreq = NULL;
	TDB_DATA key = dbwrap_record_get_key(cb_state->rec);

	req = tevent_req_create(
		mem_ctx, &state, struct g_lock_lock_cb_watch_data_state);
	if (req == NULL) {
		return NULL;
	}
	state->ev = ev;
	state->ctx = cb_state->ctx;
	state->blocker = blocker;

	state->key = tdb_data_talloc_copy(state, key);
	if (tevent_req_nomem(state->key.dptr, req)) {
		return tevent_req_post(req, ev);
	}

	state->unique_lock_epoch = cb_state->lck->unique_lock_epoch;
	state->unique_data_epoch = cb_state->lck->unique_data_epoch;

	DBG_DEBUG("state->unique_data_epoch=%"PRIu64"\n", state->unique_data_epoch);

	subreq = dbwrap_watched_watch_send(
		state, state->ev, cb_state->rec, 0, state->blocker);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, g_lock_lock_cb_watch_data_done, req);

	return req;
}

static void g_lock_lock_cb_watch_data_done_fn(
	struct db_record *rec,
	TDB_DATA value,
	void *private_data)
{
	struct tevent_req *req = talloc_get_type_abort(
		private_data, struct tevent_req);
	struct g_lock_lock_cb_watch_data_state *state = tevent_req_data(
		req, struct g_lock_lock_cb_watch_data_state);
	struct tevent_req *subreq = NULL;
	struct g_lock lck;
	bool ok;

	ok = g_lock_parse(value.dptr, value.dsize, &lck);
	if (!ok) {
		dbwrap_watched_watch_remove_instance(rec, state->watch_instance);
		state->status = NT_STATUS_INTERNAL_DB_CORRUPTION;
		return;
	}

	if (lck.unique_data_epoch != state->unique_data_epoch) {
		dbwrap_watched_watch_remove_instance(rec, state->watch_instance);
		DBG_DEBUG("lck.unique_data_epoch=%"PRIu64", "
			  "state->unique_data_epoch=%"PRIu64"\n",
			  lck.unique_data_epoch,
			  state->unique_data_epoch);
		state->status = NT_STATUS_OK;
		return;
	}

	/*
	 * The lock epoch changed, so we better
	 * remove ourself from the waiter list
	 * (most likely the first position)
	 * and re-add us at the end of the list.
	 *
	 * This gives other lock waiters a change
	 * to make progress.
	 *
	 * Otherwise we'll keep our waiter instance alive,
	 * keep waiting (most likely at first position).
	 */
	if (lck.unique_lock_epoch != state->unique_lock_epoch) {
		dbwrap_watched_watch_remove_instance(rec, state->watch_instance);
		state->watch_instance = dbwrap_watched_watch_add_instance(rec);
		state->unique_lock_epoch = lck.unique_lock_epoch;
	}

	subreq = dbwrap_watched_watch_send(
		state, state->ev, rec, state->watch_instance, state->blocker);
	if (subreq == NULL) {
		dbwrap_watched_watch_remove_instance(rec, state->watch_instance);
		state->status = NT_STATUS_NO_MEMORY;
		return;
	}
	tevent_req_set_callback(subreq, g_lock_lock_cb_watch_data_done, req);

	state->status = NT_STATUS_EVENT_PENDING;
}

static void g_lock_lock_cb_watch_data_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct g_lock_lock_cb_watch_data_state *state = tevent_req_data(
		req, struct g_lock_lock_cb_watch_data_state);
	NTSTATUS status;
	uint64_t instance = 0;

	status = dbwrap_watched_watch_recv(
		subreq, &instance, &state->blockerdead, &state->blocker);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		DBG_DEBUG("dbwrap_watched_watch_recv returned %s\n",
			  nt_errstr(status));
		return;
	}

	state->watch_instance = instance;

	status = dbwrap_do_locked(
		state->ctx->db, state->key, g_lock_lock_cb_watch_data_done_fn, req);
	if (tevent_req_nterror(req, status)) {
		DBG_DEBUG("dbwrap_do_locked returned %s\n", nt_errstr(status));
		return;
	}
	if (NT_STATUS_EQUAL(state->status, NT_STATUS_EVENT_PENDING)) {
		return;
	}
	if (tevent_req_nterror(req, state->status)) {
		return;
	}
	tevent_req_done(req);
}

NTSTATUS g_lock_lock_cb_watch_data_recv(
	struct tevent_req *req,
	bool *blockerdead,
	struct server_id *blocker)
{
	struct g_lock_lock_cb_watch_data_state *state = tevent_req_data(
		req, struct g_lock_lock_cb_watch_data_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	if (blockerdead != NULL) {
		*blockerdead = state->blockerdead;
	}
	if (blocker != NULL) {
		*blocker = state->blocker;
	}

	return NT_STATUS_OK;
}

void g_lock_lock_cb_wake_watchers(struct g_lock_lock_cb_state *cb_state)
{
	struct g_lock *lck = cb_state->lck;

	lck->unique_data_epoch = generate_unique_u64(lck->unique_data_epoch);
	cb_state->modified = true;
}

static NTSTATUS g_lock_lock_cb_run_and_store(struct g_lock_lock_cb_state *cb_state)
{
	struct g_lock *lck = cb_state->lck;
	NTSTATUS success_status = NT_STATUS_OK;
	NTSTATUS status;

	if (cb_state->cb_fn != NULL) {

		SMB_ASSERT(lck->num_shared == 0);
		SMB_ASSERT(cb_state->new_shared == NULL);

		if (cb_state->ctx->lock_order != DBWRAP_LOCK_ORDER_NONE) {
			const char *name = dbwrap_name(cb_state->ctx->db);
			dbwrap_lock_order_lock(name, cb_state->ctx->lock_order);
		}

		cb_state->ctx->busy = true;
		cb_state->cb_fn(cb_state, cb_state->cb_private);
		cb_state->ctx->busy = false;

		if (cb_state->ctx->lock_order != DBWRAP_LOCK_ORDER_NONE) {
			const char *name = dbwrap_name(cb_state->ctx->db);
			dbwrap_lock_order_unlock(name, cb_state->ctx->lock_order);
		}
	}

	if (cb_state->unlock) {
		/*
		 * Unlocked should wake up watchers.
		 *
		 * We no longer need the lock, so
		 * force a wakeup of the next watchers,
		 * even if we don't do any update.
		 */
		dbwrap_watched_watch_reset_alerting(cb_state->rec);
		dbwrap_watched_watch_force_alerting(cb_state->rec);
		if (!cb_state->modified) {
			/*
			 * The record was not changed at
			 * all, so we can also avoid
			 * storing the lck.unique_lock_epoch
			 * change
			 */
			return NT_STATUS_WAS_UNLOCKED;
		}
		lck->exclusive = (struct server_id) { .pid = 0 };
		cb_state->new_shared = NULL;

		if (lck->datalen == 0) {
			if (!cb_state->existed) {
				return NT_STATUS_WAS_UNLOCKED;
			}

			status = dbwrap_record_delete(cb_state->rec);
			if (!NT_STATUS_IS_OK(status)) {
				DBG_WARNING("dbwrap_record_delete() failed: %s\n",
				    nt_errstr(status));
				return status;
			}
			return NT_STATUS_WAS_UNLOCKED;
		}

		success_status = NT_STATUS_WAS_UNLOCKED;
	}

	status = g_lock_store(cb_state->rec,
			      cb_state->lck,
			      cb_state->new_shared,
			      NULL, 0);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_WARNING("g_lock_store() failed: %s\n",
			    nt_errstr(status));
		return status;
	}

	return success_status;
}

struct g_lock_lock_state {
	struct tevent_context *ev;
	struct g_lock_ctx *ctx;
	TDB_DATA key;
	enum g_lock_type type;
	bool retry;
	g_lock_lock_cb_fn_t cb_fn;
	void *cb_private;
};

struct g_lock_lock_fn_state {
	struct g_lock_lock_state *req_state;
	struct server_id *dead_blocker;

	struct tevent_req *watch_req;
	uint64_t watch_instance;
	NTSTATUS status;
};

static int g_lock_lock_state_destructor(struct g_lock_lock_state *s);

static NTSTATUS g_lock_trylock(
	struct db_record *rec,
	struct g_lock_lock_fn_state *state,
	TDB_DATA data,
	struct server_id *blocker)
{
	struct g_lock_lock_state *req_state = state->req_state;
	struct server_id self = messaging_server_id(req_state->ctx->msg);
	enum g_lock_type type = req_state->type;
	bool retry = req_state->retry;
	struct g_lock lck = { .exclusive.pid = 0 };
	struct g_lock_lock_cb_state cb_state = {
		.ctx = req_state->ctx,
		.rec = rec,
		.lck = &lck,
		.cb_fn = req_state->cb_fn,
		.cb_private = req_state->cb_private,
		.existed = data.dsize != 0,
		.update_mem_ctx = talloc_tos(),
	};
	struct server_id_buf tmp;
	NTSTATUS status;
	bool ok;

	ok = g_lock_parse(data.dptr, data.dsize, &lck);
	if (!ok) {
		dbwrap_watched_watch_remove_instance(rec, state->watch_instance);
		DBG_DEBUG("g_lock_parse failed\n");
		return NT_STATUS_INTERNAL_DB_CORRUPTION;
	}

	g_lock_cleanup_dead(&lck, state->dead_blocker);

	lck.unique_lock_epoch = generate_unique_u64(lck.unique_lock_epoch);

	if (lck.exclusive.pid != 0) {
		bool self_exclusive = server_id_equal(&self, &lck.exclusive);

		if (!self_exclusive) {
			bool exists = serverid_exists(&lck.exclusive);
			if (!exists) {
				lck.exclusive = (struct server_id) { .pid=0 };
				goto noexclusive;
			}

			DBG_DEBUG("%s has an exclusive lock\n",
				  server_id_str_buf(lck.exclusive, &tmp));

			if (type == G_LOCK_DOWNGRADE) {
				struct server_id_buf tmp2;

				dbwrap_watched_watch_remove_instance(rec,
						state->watch_instance);

				DBG_DEBUG("%s: Trying to downgrade %s\n",
					  server_id_str_buf(self, &tmp),
					  server_id_str_buf(
						  lck.exclusive, &tmp2));
				return NT_STATUS_NOT_LOCKED;
			}

			if (type == G_LOCK_UPGRADE) {
				ssize_t shared_idx;

				dbwrap_watched_watch_remove_instance(rec,
						state->watch_instance);

				shared_idx = g_lock_find_shared(&lck, &self);

				if (shared_idx == -1) {
					DBG_DEBUG("Trying to upgrade %s "
						  "without "
						  "existing shared lock\n",
						  server_id_str_buf(
							  self, &tmp));
					return NT_STATUS_NOT_LOCKED;
				}

				/*
				 * We're trying to upgrade, and the
				 * exclusive lock is taken by someone
				 * else. This means that someone else
				 * is waiting for us to give up our
				 * shared lock. If we now also wait
				 * for someone to give their shared
				 * lock, we will deadlock.
				 */

				DBG_DEBUG("Trying to upgrade %s while "
					  "someone else is also "
					  "trying to upgrade\n",
					  server_id_str_buf(self, &tmp));
				return NT_STATUS_POSSIBLE_DEADLOCK;
			}

			DBG_DEBUG("Waiting for lck.exclusive=%s\n",
				  server_id_str_buf(lck.exclusive, &tmp));

			/*
			 * We will return NT_STATUS_LOCK_NOT_GRANTED
			 * and need to monitor the record.
			 *
			 * If we don't have a watcher instance yet,
			 * we should add one.
			 */
			if (state->watch_instance == 0) {
				state->watch_instance =
					dbwrap_watched_watch_add_instance(rec);
			}

			*blocker = lck.exclusive;
			return NT_STATUS_LOCK_NOT_GRANTED;
		}

		if (type == G_LOCK_DOWNGRADE) {
			DBG_DEBUG("Downgrading %s from WRITE to READ\n",
				  server_id_str_buf(self, &tmp));

			lck.exclusive = (struct server_id) { .pid = 0 };
			goto do_shared;
		}

		if (!retry) {
			dbwrap_watched_watch_remove_instance(rec,
						state->watch_instance);

			DBG_DEBUG("%s already locked by self\n",
				  server_id_str_buf(self, &tmp));
			return NT_STATUS_WAS_LOCKED;
		}

		g_lock_cleanup_shared(&lck);

		if (lck.num_shared != 0) {
			g_lock_get_shared(&lck, 0, blocker);

			DBG_DEBUG("Continue waiting for shared lock %s\n",
				  server_id_str_buf(*blocker, &tmp));

			/*
			 * We will return NT_STATUS_LOCK_NOT_GRANTED
			 * and need to monitor the record.
			 *
			 * If we don't have a watcher instance yet,
			 * we should add one.
			 */
			if (state->watch_instance == 0) {
				state->watch_instance =
					dbwrap_watched_watch_add_instance(rec);
			}

			return NT_STATUS_LOCK_NOT_GRANTED;
		}

		/*
		 * Retry after a conflicting lock was released..
		 * All pending readers are gone so we got the lock...
		 */
		goto got_lock;
	}

noexclusive:

	if (type == G_LOCK_UPGRADE) {
		ssize_t shared_idx = g_lock_find_shared(&lck, &self);

		if (shared_idx == -1) {
			dbwrap_watched_watch_remove_instance(rec,
						state->watch_instance);

			DBG_DEBUG("Trying to upgrade %s without "
				  "existing shared lock\n",
				  server_id_str_buf(self, &tmp));
			return NT_STATUS_NOT_LOCKED;
		}

		g_lock_del_shared(&lck, shared_idx);
		type = G_LOCK_WRITE;
	}

	if (type == G_LOCK_WRITE) {
		ssize_t shared_idx = g_lock_find_shared(&lck, &self);

		if (shared_idx != -1) {
			dbwrap_watched_watch_remove_instance(rec,
						state->watch_instance);
			DBG_DEBUG("Trying to writelock existing shared %s\n",
				  server_id_str_buf(self, &tmp));
			return NT_STATUS_WAS_LOCKED;
		}

		lck.exclusive = self;

		g_lock_cleanup_shared(&lck);

		if (lck.num_shared == 0) {
			/*
			 * If we store ourself as exclusive writer,
			 * without any pending readers ...
			 */
			goto got_lock;
		}

		if (state->watch_instance == 0) {
			/*
			 * Here we have lck.num_shared != 0.
			 *
			 * We will return NT_STATUS_LOCK_NOT_GRANTED
			 * below.
			 *
			 * And don't have a watcher instance yet!
			 *
			 * We add it here before g_lock_store()
			 * in order to trigger just one
			 * low level dbwrap_do_locked() call.
			 */
			state->watch_instance =
				dbwrap_watched_watch_add_instance(rec);
		}

		status = g_lock_store(rec, &lck, NULL, NULL, 0);
		if (!NT_STATUS_IS_OK(status)) {
			DBG_DEBUG("g_lock_store() failed: %s\n",
				  nt_errstr(status));
			return status;
		}

		talloc_set_destructor(
			req_state, g_lock_lock_state_destructor);

		g_lock_get_shared(&lck, 0, blocker);

		DBG_DEBUG("Waiting for %zu shared locks, "
			  "picking blocker %s\n",
			  lck.num_shared,
			  server_id_str_buf(*blocker, &tmp));

		return NT_STATUS_LOCK_NOT_GRANTED;
	}

do_shared:

	g_lock_cleanup_shared(&lck);
	cb_state.new_shared = &self;
	goto got_lock;

got_lock:
	/*
	 * We got the lock we asked for, so we no
	 * longer need to monitor the record.
	 */
	dbwrap_watched_watch_remove_instance(rec, state->watch_instance);

	status = g_lock_lock_cb_run_and_store(&cb_state);
	if (!NT_STATUS_IS_OK(status) &&
	    !NT_STATUS_EQUAL(status, NT_STATUS_WAS_UNLOCKED))
	{
		DBG_WARNING("g_lock_lock_cb_run_and_store() failed: %s\n",
			    nt_errstr(status));
		return status;
	}

	talloc_set_destructor(req_state, NULL);
	return status;
}

static void g_lock_lock_fn(
	struct db_record *rec,
	TDB_DATA value,
	void *private_data)
{
	struct g_lock_lock_fn_state *state = private_data;
	struct server_id blocker = {0};

	/*
	 * We're trying to get a lock and if we are
	 * successful in doing that, we should not
	 * wakeup any other waiters, all they would
	 * find is that we're holding a lock they
	 * are conflicting with.
	 */
	dbwrap_watched_watch_skip_alerting(rec);

	state->status = g_lock_trylock(rec, state, value, &blocker);
	if (!NT_STATUS_IS_OK(state->status)) {
		DBG_DEBUG("g_lock_trylock returned %s\n",
			  nt_errstr(state->status));
	}
	if (!NT_STATUS_EQUAL(state->status, NT_STATUS_LOCK_NOT_GRANTED)) {
		return;
	}

	state->watch_req = dbwrap_watched_watch_send(
		state->req_state, state->req_state->ev, rec, state->watch_instance, blocker);
	if (state->watch_req == NULL) {
		state->status = NT_STATUS_NO_MEMORY;
	}
}

static int g_lock_lock_state_destructor(struct g_lock_lock_state *s)
{
	NTSTATUS status = g_lock_unlock(s->ctx, s->key);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_DEBUG("g_lock_unlock failed: %s\n", nt_errstr(status));
	}
	return 0;
}

static void g_lock_lock_retry(struct tevent_req *subreq);

struct tevent_req *g_lock_lock_send(TALLOC_CTX *mem_ctx,
				    struct tevent_context *ev,
				    struct g_lock_ctx *ctx,
				    TDB_DATA key,
				    enum g_lock_type type,
				    g_lock_lock_cb_fn_t cb_fn,
				    void *cb_private)
{
	struct tevent_req *req;
	struct g_lock_lock_state *state;
	struct g_lock_lock_fn_state fn_state;
	NTSTATUS status;
	bool ok;

	SMB_ASSERT(!ctx->busy);

	req = tevent_req_create(mem_ctx, &state, struct g_lock_lock_state);
	if (req == NULL) {
		return NULL;
	}
	state->ev = ev;
	state->ctx = ctx;
	state->key = key;
	state->type = type;
	state->cb_fn = cb_fn;
	state->cb_private = cb_private;

	fn_state = (struct g_lock_lock_fn_state) {
		.req_state = state,
	};

	/*
	 * We allow a cb_fn only for G_LOCK_WRITE for now.
	 *
	 * It's all we currently need and it makes a few things
	 * easier to implement.
	 */
	if (unlikely(cb_fn != NULL && type != G_LOCK_WRITE)) {
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER_6);
		return tevent_req_post(req, ev);
	}

	status = dbwrap_do_locked(ctx->db, key, g_lock_lock_fn, &fn_state);
	if (tevent_req_nterror(req, status)) {
		DBG_DEBUG("dbwrap_do_locked failed: %s\n",
			  nt_errstr(status));
		return tevent_req_post(req, ev);
	}

	if (NT_STATUS_IS_OK(fn_state.status)) {
		tevent_req_done(req);
		return tevent_req_post(req, ev);
	}
	if (!NT_STATUS_EQUAL(fn_state.status, NT_STATUS_LOCK_NOT_GRANTED)) {
		tevent_req_nterror(req, fn_state.status);
		return tevent_req_post(req, ev);
	}

	if (tevent_req_nomem(fn_state.watch_req, req)) {
		return tevent_req_post(req, ev);
	}

	ok = tevent_req_set_endtime(
		fn_state.watch_req,
		state->ev,
		timeval_current_ofs(5 + generate_random() % 5, 0));
	if (!ok) {
		tevent_req_oom(req);
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(fn_state.watch_req, g_lock_lock_retry, req);

	return req;
}

static void g_lock_lock_retry(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct g_lock_lock_state *state = tevent_req_data(
		req, struct g_lock_lock_state);
	struct g_lock_lock_fn_state fn_state;
	struct server_id blocker = { .pid = 0 };
	bool blockerdead = false;
	NTSTATUS status;
	uint64_t instance = 0;

	status = dbwrap_watched_watch_recv(subreq, &instance, &blockerdead, &blocker);
	DBG_DEBUG("watch_recv returned %s\n", nt_errstr(status));
	TALLOC_FREE(subreq);

	if (!NT_STATUS_IS_OK(status) &&
	    !NT_STATUS_EQUAL(status, NT_STATUS_IO_TIMEOUT)) {
		tevent_req_nterror(req, status);
		return;
	}

	state->retry = true;

	fn_state = (struct g_lock_lock_fn_state) {
		.req_state = state,
		.dead_blocker = blockerdead ? &blocker : NULL,
		.watch_instance = instance,
	};

	status = dbwrap_do_locked(state->ctx->db, state->key,
				  g_lock_lock_fn, &fn_state);
	if (tevent_req_nterror(req, status)) {
		DBG_DEBUG("dbwrap_do_locked failed: %s\n",
			  nt_errstr(status));
		return;
	}

	if (NT_STATUS_IS_OK(fn_state.status)) {
		tevent_req_done(req);
		return;
	}
	if (!NT_STATUS_EQUAL(fn_state.status, NT_STATUS_LOCK_NOT_GRANTED)) {
		tevent_req_nterror(req, fn_state.status);
		return;
	}

	if (tevent_req_nomem(fn_state.watch_req, req)) {
		return;
	}

	if (!tevent_req_set_endtime(
		    fn_state.watch_req, state->ev,
		    timeval_current_ofs(5 + generate_random() % 5, 0))) {
		return;
	}
	tevent_req_set_callback(fn_state.watch_req, g_lock_lock_retry, req);
}

NTSTATUS g_lock_lock_recv(struct tevent_req *req)
{
	struct g_lock_lock_state *state = tevent_req_data(
		req, struct g_lock_lock_state);
	struct g_lock_ctx *ctx = state->ctx;
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		if (NT_STATUS_EQUAL(status, NT_STATUS_WAS_UNLOCKED)) {
			return NT_STATUS_OK;
		}
		return status;
	}

	if ((ctx->lock_order != DBWRAP_LOCK_ORDER_NONE) &&
	    ((state->type == G_LOCK_READ) ||
	     (state->type == G_LOCK_WRITE))) {
		const char *name = dbwrap_name(ctx->db);
		dbwrap_lock_order_lock(name, ctx->lock_order);
	}

	return NT_STATUS_OK;
}

struct g_lock_lock_simple_state {
	struct g_lock_ctx *ctx;
	struct server_id me;
	enum g_lock_type type;
	NTSTATUS status;
	g_lock_lock_cb_fn_t cb_fn;
	void *cb_private;
};

static void g_lock_lock_simple_fn(
	struct db_record *rec,
	TDB_DATA value,
	void *private_data)
{
	struct g_lock_lock_simple_state *state = private_data;
	struct server_id_buf buf;
	struct g_lock lck = { .exclusive.pid = 0 };
	struct g_lock_lock_cb_state cb_state = {
		.ctx = state->ctx,
		.rec = rec,
		.lck = &lck,
		.cb_fn = state->cb_fn,
		.cb_private = state->cb_private,
		.existed = value.dsize != 0,
		.update_mem_ctx = talloc_tos(),
	};
	bool ok;

	ok = g_lock_parse(value.dptr, value.dsize, &lck);
	if (!ok) {
		DBG_DEBUG("g_lock_parse failed\n");
		state->status = NT_STATUS_INTERNAL_DB_CORRUPTION;
		return;
	}

	if (lck.exclusive.pid != 0) {
		DBG_DEBUG("locked by %s\n",
			  server_id_str_buf(lck.exclusive, &buf));
		goto not_granted;
	}

	if (state->type == G_LOCK_WRITE) {
		if (lck.num_shared != 0) {
			DBG_DEBUG("num_shared=%zu\n", lck.num_shared);
			goto not_granted;
		}
		lck.exclusive = state->me;
	} else if (state->type == G_LOCK_READ) {
		g_lock_cleanup_shared(&lck);
		cb_state.new_shared = &state->me;
	} else {
		smb_panic(__location__);
	}

	lck.unique_lock_epoch = generate_unique_u64(lck.unique_lock_epoch);

	/*
	 * We are going to store us as owner,
	 * so we got what we were waiting for.
	 *
	 * So we no longer need to monitor the
	 * record.
	 */
	dbwrap_watched_watch_skip_alerting(rec);

	state->status = g_lock_lock_cb_run_and_store(&cb_state);
	if (!NT_STATUS_IS_OK(state->status) &&
	    !NT_STATUS_EQUAL(state->status, NT_STATUS_WAS_UNLOCKED))
	{
		DBG_WARNING("g_lock_lock_cb_run_and_store() failed: %s\n",
			    nt_errstr(state->status));
		return;
	}

	return;

not_granted:
	state->status = NT_STATUS_LOCK_NOT_GRANTED;
}

NTSTATUS g_lock_lock(struct g_lock_ctx *ctx, TDB_DATA key,
		     enum g_lock_type type, struct timeval timeout,
		     g_lock_lock_cb_fn_t cb_fn,
		     void *cb_private)
{
	TALLOC_CTX *frame;
	struct tevent_context *ev;
	struct tevent_req *req;
	struct timeval end;
	NTSTATUS status;

	SMB_ASSERT(!ctx->busy);

	/*
	 * We allow a cb_fn only for G_LOCK_WRITE for now.
	 *
	 * It's all we currently need and it makes a few things
	 * easier to implement.
	 */
	if (unlikely(cb_fn != NULL && type != G_LOCK_WRITE)) {
		return NT_STATUS_INVALID_PARAMETER_5;
	}

	if ((type == G_LOCK_READ) || (type == G_LOCK_WRITE)) {
		/*
		 * This is an abstraction violation: Normally we do
		 * the sync wrappers around async functions with full
		 * nested event contexts. However, this is used in
		 * very hot code paths, so avoid the event context
		 * creation for the good path where there's no lock
		 * contention. My benchmark gave a factor of 2
		 * improvement for lock/unlock.
		 */
		struct g_lock_lock_simple_state state = {
			.ctx = ctx,
			.me = messaging_server_id(ctx->msg),
			.type = type,
			.cb_fn = cb_fn,
			.cb_private = cb_private,
		};
		status = dbwrap_do_locked(
			ctx->db, key, g_lock_lock_simple_fn, &state);
		if (!NT_STATUS_IS_OK(status)) {
			DBG_DEBUG("dbwrap_do_locked() failed: %s\n",
				  nt_errstr(status));
			return status;
		}

		DBG_DEBUG("status=%s, state.status=%s\n",
			  nt_errstr(status),
			  nt_errstr(state.status));

		if (NT_STATUS_IS_OK(state.status)) {
			if (ctx->lock_order != DBWRAP_LOCK_ORDER_NONE) {
				const char *name = dbwrap_name(ctx->db);
				dbwrap_lock_order_lock(name, ctx->lock_order);
			}
			return NT_STATUS_OK;
		}
		if (NT_STATUS_EQUAL(state.status, NT_STATUS_WAS_UNLOCKED)) {
			/* without dbwrap_lock_order_lock() */
			return NT_STATUS_OK;
		}
		if (!NT_STATUS_EQUAL(
			    state.status, NT_STATUS_LOCK_NOT_GRANTED)) {
			return state.status;
		}

		if (timeval_is_zero(&timeout)) {
			return NT_STATUS_LOCK_NOT_GRANTED;
		}

		/*
		 * Fall back to the full g_lock_trylock logic,
		 * g_lock_lock_simple_fn() called above only covers
		 * the uncontended path.
		 */
	}

	frame = talloc_stackframe();
	status = NT_STATUS_NO_MEMORY;

	ev = samba_tevent_context_init(frame);
	if (ev == NULL) {
		goto fail;
	}
	req = g_lock_lock_send(frame, ev, ctx, key, type, cb_fn, cb_private);
	if (req == NULL) {
		goto fail;
	}
	end = timeval_current_ofs(timeout.tv_sec, timeout.tv_usec);
	if (!tevent_req_set_endtime(req, ev, end)) {
		goto fail;
	}
	if (!tevent_req_poll_ntstatus(req, ev, &status)) {
		goto fail;
	}
	status = g_lock_lock_recv(req);
 fail:
	TALLOC_FREE(frame);
	return status;
}

struct g_lock_unlock_state {
	struct server_id self;
	NTSTATUS status;
};

static void g_lock_unlock_fn(
	struct db_record *rec,
	TDB_DATA value,
	void *private_data)
{
	struct g_lock_unlock_state *state = private_data;
	struct server_id_buf tmp1, tmp2;
	struct g_lock lck;
	size_t i;
	bool ok, exclusive;

	ok = g_lock_parse(value.dptr, value.dsize, &lck);
	if (!ok) {
		DBG_DEBUG("g_lock_parse() failed\n");
		state->status = NT_STATUS_INTERNAL_DB_CORRUPTION;
		return;
	}

	exclusive = server_id_equal(&state->self, &lck.exclusive);

	for (i=0; i<lck.num_shared; i++) {
		struct server_id shared;
		g_lock_get_shared(&lck, i, &shared);
		if (server_id_equal(&state->self, &shared)) {
			break;
		}
	}

	if (i < lck.num_shared) {
		if (exclusive) {
			DBG_DEBUG("%s both exclusive and shared (%zu)\n",
				  server_id_str_buf(state->self, &tmp1),
				  i);
			state->status = NT_STATUS_INTERNAL_DB_CORRUPTION;
			return;
		}
		g_lock_del_shared(&lck, i);
	} else {
		if (!exclusive) {
			DBG_DEBUG("Lock not found, self=%s, lck.exclusive=%s, "
				  "num_shared=%zu\n",
				  server_id_str_buf(state->self, &tmp1),
				  server_id_str_buf(lck.exclusive, &tmp2),
				  lck.num_shared);
			state->status = NT_STATUS_NOT_FOUND;
			return;
		}
		lck.exclusive = (struct server_id) { .pid = 0 };
	}

	if ((lck.exclusive.pid == 0) &&
	    (lck.num_shared == 0) &&
	    (lck.datalen == 0)) {
		state->status = dbwrap_record_delete(rec);
		return;
	}

	if (!exclusive && lck.exclusive.pid != 0) {
		/*
		 * We only had a read lock and there's
		 * someone waiting for an exclusive lock.
		 *
		 * Don't alert the exclusive lock waiter
		 * if there are still other read lock holders.
		 */
		g_lock_cleanup_shared(&lck);
		if (lck.num_shared != 0) {
			dbwrap_watched_watch_skip_alerting(rec);
		}
	}

	lck.unique_lock_epoch = generate_unique_u64(lck.unique_lock_epoch);

	state->status = g_lock_store(rec, &lck, NULL, NULL, 0);
}

NTSTATUS g_lock_unlock(struct g_lock_ctx *ctx, TDB_DATA key)
{
	struct g_lock_unlock_state state = {
		.self = messaging_server_id(ctx->msg),
	};
	NTSTATUS status;

	SMB_ASSERT(!ctx->busy);

	status = dbwrap_do_locked(ctx->db, key, g_lock_unlock_fn, &state);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_WARNING("dbwrap_do_locked failed: %s\n",
			    nt_errstr(status));
		return status;
	}
	if (!NT_STATUS_IS_OK(state.status)) {
		DBG_WARNING("g_lock_unlock_fn failed: %s\n",
			    nt_errstr(state.status));
		return state.status;
	}

	if (ctx->lock_order != DBWRAP_LOCK_ORDER_NONE) {
		const char *name = dbwrap_name(ctx->db);
		dbwrap_lock_order_unlock(name, ctx->lock_order);
	}

	return NT_STATUS_OK;
}

struct g_lock_writev_data_state {
	TDB_DATA key;
	struct server_id self;
	const TDB_DATA *dbufs;
	size_t num_dbufs;
	NTSTATUS status;
};

static void g_lock_writev_data_fn(
	struct db_record *rec,
	TDB_DATA value,
	void *private_data)
{
	struct g_lock_writev_data_state *state = private_data;
	struct g_lock lck;
	bool exclusive;
	bool ok;

	/*
	 * We're holding an exclusive write lock.
	 *
	 * Now we're updating the content of the record.
	 *
	 * We should not wakeup any other waiters, all they
	 * would find is that we're still holding a lock they
	 * are conflicting with.
	 */
	dbwrap_watched_watch_skip_alerting(rec);

	ok = g_lock_parse(value.dptr, value.dsize, &lck);
	if (!ok) {
		DBG_DEBUG("g_lock_parse for %s failed\n",
			  tdb_data_dbg(state->key));
		state->status = NT_STATUS_INTERNAL_DB_CORRUPTION;
		return;
	}

	exclusive = server_id_equal(&state->self, &lck.exclusive);

	/*
	 * Make sure we're really exclusive. We are marked as
	 * exclusive when we are waiting for an exclusive lock
	 */
	exclusive &= (lck.num_shared == 0);

	if (!exclusive) {
		struct server_id_buf buf1, buf2;
		DBG_DEBUG("Not locked by us: self=%s, lck.exclusive=%s, "
			  "lck.num_shared=%zu\n",
			  server_id_str_buf(state->self, &buf1),
			  server_id_str_buf(lck.exclusive, &buf2),
			  lck.num_shared);
		state->status = NT_STATUS_NOT_LOCKED;
		return;
	}

	lck.unique_data_epoch = generate_unique_u64(lck.unique_data_epoch);
	lck.data = NULL;
	lck.datalen = 0;
	state->status = g_lock_store(
		rec, &lck, NULL, state->dbufs, state->num_dbufs);
}

NTSTATUS g_lock_writev_data(
	struct g_lock_ctx *ctx,
	TDB_DATA key,
	const TDB_DATA *dbufs,
	size_t num_dbufs)
{
	struct g_lock_writev_data_state state = {
		.key = key,
		.self = messaging_server_id(ctx->msg),
		.dbufs = dbufs,
		.num_dbufs = num_dbufs,
	};
	NTSTATUS status;

	SMB_ASSERT(!ctx->busy);

	status = dbwrap_do_locked(
		ctx->db, key, g_lock_writev_data_fn, &state);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_WARNING("dbwrap_do_locked failed: %s\n",
			    nt_errstr(status));
		return status;
	}
	if (!NT_STATUS_IS_OK(state.status)) {
		DBG_WARNING("g_lock_writev_data_fn failed: %s\n",
			    nt_errstr(state.status));
		return state.status;
	}

	return NT_STATUS_OK;
}

NTSTATUS g_lock_write_data(struct g_lock_ctx *ctx, TDB_DATA key,
			   const uint8_t *buf, size_t buflen)
{
	TDB_DATA dbuf = {
		.dptr = discard_const_p(uint8_t, buf),
		.dsize = buflen,
	};
	return g_lock_writev_data(ctx, key, &dbuf, 1);
}

struct g_lock_locks_state {
	int (*fn)(TDB_DATA key, void *private_data);
	void *private_data;
};

static int g_lock_locks_fn(struct db_record *rec, void *priv)
{
	TDB_DATA key;
	struct g_lock_locks_state *state = (struct g_lock_locks_state *)priv;

	key = dbwrap_record_get_key(rec);
	return state->fn(key, state->private_data);
}

int g_lock_locks_read(struct g_lock_ctx *ctx,
		      int (*fn)(TDB_DATA key, void *private_data),
		      void *private_data)
{
	struct g_lock_locks_state state;
	NTSTATUS status;
	int count;

	SMB_ASSERT(!ctx->busy);

	state.fn = fn;
	state.private_data = private_data;

	status = dbwrap_traverse_read(ctx->db,
				      g_lock_locks_fn,
				      &state,
				      &count);
	if (!NT_STATUS_IS_OK(status)) {
		return -1;
	}
	return count;
}

int g_lock_locks(struct g_lock_ctx *ctx,
		 int (*fn)(TDB_DATA key, void *private_data),
		 void *private_data)
{
	struct g_lock_locks_state state;
	NTSTATUS status;
	int count;

	SMB_ASSERT(!ctx->busy);

	state.fn = fn;
	state.private_data = private_data;

	status = dbwrap_traverse(ctx->db, g_lock_locks_fn, &state, &count);
	if (!NT_STATUS_IS_OK(status)) {
		return -1;
	}
	return count;
}

struct g_lock_dump_state {
	TALLOC_CTX *mem_ctx;
	TDB_DATA key;
	void (*fn)(struct server_id exclusive,
		   size_t num_shared,
		   const struct server_id *shared,
		   const uint8_t *data,
		   size_t datalen,
		   void *private_data);
	void *private_data;
	NTSTATUS status;
	enum dbwrap_req_state req_state;
};

static void g_lock_dump_fn(TDB_DATA key, TDB_DATA data,
			   void *private_data)
{
	struct g_lock_dump_state *state = private_data;
	struct g_lock lck = (struct g_lock) { .exclusive.pid = 0 };
	struct server_id *shared = NULL;
	size_t i;
	bool ok;

	ok = g_lock_parse(data.dptr, data.dsize, &lck);
	if (!ok) {
		DBG_DEBUG("g_lock_parse failed for %s\n",
			  tdb_data_dbg(state->key));
		state->status = NT_STATUS_INTERNAL_DB_CORRUPTION;
		return;
	}

	if (lck.num_shared > 0) {
		shared = talloc_array(
			state->mem_ctx, struct server_id, lck.num_shared);
		if (shared == NULL) {
			DBG_DEBUG("talloc failed\n");
			state->status = NT_STATUS_NO_MEMORY;
			return;
		}
	}

	for (i=0; i<lck.num_shared; i++) {
		g_lock_get_shared(&lck, i, &shared[i]);
	}

	state->fn(lck.exclusive,
		  lck.num_shared,
		  shared,
		  lck.data,
		  lck.datalen,
		  state->private_data);

	TALLOC_FREE(shared);

	state->status = NT_STATUS_OK;
}

NTSTATUS g_lock_dump(struct g_lock_ctx *ctx, TDB_DATA key,
		     void (*fn)(struct server_id exclusive,
				size_t num_shared,
				const struct server_id *shared,
				const uint8_t *data,
				size_t datalen,
				void *private_data),
		     void *private_data)
{
	struct g_lock_dump_state state = {
		.mem_ctx = ctx, .key = key,
		.fn = fn, .private_data = private_data
	};
	NTSTATUS status;

	SMB_ASSERT(!ctx->busy);

	status = dbwrap_parse_record(ctx->db, key, g_lock_dump_fn, &state);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_DEBUG("dbwrap_parse_record returned %s\n",
			  nt_errstr(status));
		return status;
	}
	if (!NT_STATUS_IS_OK(state.status)) {
		DBG_DEBUG("g_lock_dump_fn returned %s\n",
			  nt_errstr(state.status));
		return state.status;
	}
	return NT_STATUS_OK;
}

static void g_lock_dump_done(struct tevent_req *subreq);

struct tevent_req *g_lock_dump_send(
	TALLOC_CTX *mem_ctx,
	struct tevent_context *ev,
	struct g_lock_ctx *ctx,
	TDB_DATA key,
	void (*fn)(struct server_id exclusive,
		   size_t num_shared,
		   const struct server_id *shared,
		   const uint8_t *data,
		   size_t datalen,
		   void *private_data),
	void *private_data)
{
	struct tevent_req *req = NULL, *subreq = NULL;
	struct g_lock_dump_state *state = NULL;

	SMB_ASSERT(!ctx->busy);

	req = tevent_req_create(mem_ctx, &state, struct g_lock_dump_state);
	if (req == NULL) {
		return NULL;
	}
	state->mem_ctx = state;
	state->key = key;
	state->fn = fn;
	state->private_data = private_data;

	SMB_ASSERT(!ctx->busy);

	subreq = dbwrap_parse_record_send(
		state,
		ev,
		ctx->db,
		key,
		g_lock_dump_fn,
		state,
		&state->req_state);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, g_lock_dump_done, req);
	return req;
}

static void g_lock_dump_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct g_lock_dump_state *state = tevent_req_data(
		req, struct g_lock_dump_state);
	NTSTATUS status;

	status = dbwrap_parse_record_recv(subreq);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status) ||
	    tevent_req_nterror(req, state->status)) {
		return;
	}
	tevent_req_done(req);
}

NTSTATUS g_lock_dump_recv(struct tevent_req *req)
{
	return tevent_req_simple_recv_ntstatus(req);
}

int g_lock_seqnum(struct g_lock_ctx *ctx)
{
	return dbwrap_get_seqnum(ctx->db);
}

struct g_lock_watch_data_state {
	struct tevent_context *ev;
	struct g_lock_ctx *ctx;
	TDB_DATA key;
	struct server_id blocker;
	bool blockerdead;
	uint64_t unique_lock_epoch;
	uint64_t unique_data_epoch;
	uint64_t watch_instance;
	NTSTATUS status;
};

static void g_lock_watch_data_done(struct tevent_req *subreq);

static void g_lock_watch_data_send_fn(
	struct db_record *rec,
	TDB_DATA value,
	void *private_data)
{
	struct tevent_req *req = talloc_get_type_abort(
		private_data, struct tevent_req);
	struct g_lock_watch_data_state *state = tevent_req_data(
		req, struct g_lock_watch_data_state);
	struct tevent_req *subreq = NULL;
	struct g_lock lck;
	bool ok;

	ok = g_lock_parse(value.dptr, value.dsize, &lck);
	if (!ok) {
		state->status = NT_STATUS_INTERNAL_DB_CORRUPTION;
		return;
	}
	state->unique_lock_epoch = lck.unique_lock_epoch;
	state->unique_data_epoch = lck.unique_data_epoch;

	DBG_DEBUG("state->unique_data_epoch=%"PRIu64"\n", state->unique_data_epoch);

	subreq = dbwrap_watched_watch_send(
		state, state->ev, rec, 0, state->blocker);
	if (subreq == NULL) {
		state->status = NT_STATUS_NO_MEMORY;
		return;
	}
	tevent_req_set_callback(subreq, g_lock_watch_data_done, req);

	state->status = NT_STATUS_EVENT_PENDING;
}

struct tevent_req *g_lock_watch_data_send(
	TALLOC_CTX *mem_ctx,
	struct tevent_context *ev,
	struct g_lock_ctx *ctx,
	TDB_DATA key,
	struct server_id blocker)
{
	struct tevent_req *req = NULL;
	struct g_lock_watch_data_state *state = NULL;
	NTSTATUS status;

	SMB_ASSERT(!ctx->busy);

	req = tevent_req_create(
		mem_ctx, &state, struct g_lock_watch_data_state);
	if (req == NULL) {
		return NULL;
	}
	state->ev = ev;
	state->ctx = ctx;
	state->blocker = blocker;

	state->key = tdb_data_talloc_copy(state, key);
	if (tevent_req_nomem(state->key.dptr, req)) {
		return tevent_req_post(req, ev);
	}

	status = dbwrap_do_locked(
		ctx->db, key, g_lock_watch_data_send_fn, req);
	if (tevent_req_nterror(req, status)) {
		DBG_DEBUG("dbwrap_do_locked returned %s\n", nt_errstr(status));
		return tevent_req_post(req, ev);
	}

	if (NT_STATUS_EQUAL(state->status, NT_STATUS_EVENT_PENDING)) {
		return req;
	}
	if (tevent_req_nterror(req, state->status)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_done(req);
	return tevent_req_post(req, ev);
}

static void g_lock_watch_data_done_fn(
	struct db_record *rec,
	TDB_DATA value,
	void *private_data)
{
	struct tevent_req *req = talloc_get_type_abort(
		private_data, struct tevent_req);
	struct g_lock_watch_data_state *state = tevent_req_data(
		req, struct g_lock_watch_data_state);
	struct tevent_req *subreq = NULL;
	struct g_lock lck;
	bool ok;

	ok = g_lock_parse(value.dptr, value.dsize, &lck);
	if (!ok) {
		dbwrap_watched_watch_remove_instance(rec, state->watch_instance);
		state->status = NT_STATUS_INTERNAL_DB_CORRUPTION;
		return;
	}

	if (lck.unique_data_epoch != state->unique_data_epoch) {
		dbwrap_watched_watch_remove_instance(rec, state->watch_instance);
		DBG_DEBUG("lck.unique_data_epoch=%"PRIu64", "
			  "state->unique_data_epoch=%"PRIu64"\n",
			  lck.unique_data_epoch,
			  state->unique_data_epoch);
		state->status = NT_STATUS_OK;
		return;
	}

	/*
	 * The lock epoch changed, so we better
	 * remove ourself from the waiter list
	 * (most likely the first position)
	 * and re-add us at the end of the list.
	 *
	 * This gives other lock waiters a change
	 * to make progress.
	 *
	 * Otherwise we'll keep our waiter instance alive,
	 * keep waiting (most likely at first position).
	 */
	if (lck.unique_lock_epoch != state->unique_lock_epoch) {
		dbwrap_watched_watch_remove_instance(rec, state->watch_instance);
		state->watch_instance = dbwrap_watched_watch_add_instance(rec);
		state->unique_lock_epoch = lck.unique_lock_epoch;
	}

	subreq = dbwrap_watched_watch_send(
		state, state->ev, rec, state->watch_instance, state->blocker);
	if (subreq == NULL) {
		dbwrap_watched_watch_remove_instance(rec, state->watch_instance);
		state->status = NT_STATUS_NO_MEMORY;
		return;
	}
	tevent_req_set_callback(subreq, g_lock_watch_data_done, req);

	state->status = NT_STATUS_EVENT_PENDING;
}

static void g_lock_watch_data_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct g_lock_watch_data_state *state = tevent_req_data(
		req, struct g_lock_watch_data_state);
	NTSTATUS status;
	uint64_t instance = 0;

	status = dbwrap_watched_watch_recv(
		subreq, &instance, &state->blockerdead, &state->blocker);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		DBG_DEBUG("dbwrap_watched_watch_recv returned %s\n",
			  nt_errstr(status));
		return;
	}

	state->watch_instance = instance;

	status = dbwrap_do_locked(
		state->ctx->db, state->key, g_lock_watch_data_done_fn, req);
	if (tevent_req_nterror(req, status)) {
		DBG_DEBUG("dbwrap_do_locked returned %s\n", nt_errstr(status));
		return;
	}
	if (NT_STATUS_EQUAL(state->status, NT_STATUS_EVENT_PENDING)) {
		return;
	}
	if (tevent_req_nterror(req, state->status)) {
		return;
	}
	tevent_req_done(req);
}

NTSTATUS g_lock_watch_data_recv(
	struct tevent_req *req,
	bool *blockerdead,
	struct server_id *blocker)
{
	struct g_lock_watch_data_state *state = tevent_req_data(
		req, struct g_lock_watch_data_state);
	NTSTATUS status;

	if (tevent_req_is_nterror(req, &status)) {
		return status;
	}
	if (blockerdead != NULL) {
		*blockerdead = state->blockerdead;
	}
	if (blocker != NULL) {
		*blocker = state->blocker;
	}

	return NT_STATUS_OK;
}

static void g_lock_wake_watchers_fn(
	struct db_record *rec,
	TDB_DATA value,
	void *private_data)
{
	struct g_lock lck = { .exclusive.pid = 0 };
	NTSTATUS status;
	bool ok;

	ok = g_lock_parse(value.dptr, value.dsize, &lck);
	if (!ok) {
		DBG_WARNING("g_lock_parse failed\n");
		return;
	}

	lck.unique_data_epoch = generate_unique_u64(lck.unique_data_epoch);

	status = g_lock_store(rec, &lck, NULL, NULL, 0);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_WARNING("g_lock_store failed: %s\n", nt_errstr(status));
		return;
	}
}

void g_lock_wake_watchers(struct g_lock_ctx *ctx, TDB_DATA key)
{
	NTSTATUS status;

	SMB_ASSERT(!ctx->busy);

	status = dbwrap_do_locked(ctx->db, key, g_lock_wake_watchers_fn, NULL);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_DEBUG("dbwrap_do_locked returned %s\n",
			  nt_errstr(status));
	}
}
