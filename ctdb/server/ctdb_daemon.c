/*
   ctdb daemon code

   Copyright (C) Andrew Tridgell  2006

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include "replace.h"
#include "system/network.h"
#include "system/filesys.h"
#include "system/wait.h"
#include "system/time.h"

#include <errno.h>
#include <talloc.h>
/* Allow use of deprecated function tevent_loop_allow_nesting() */
#define TEVENT_DEPRECATED
#include <tevent.h>
#include <tdb.h>

#include "lib/tdb_wrap/tdb_wrap.h"
#include "lib/util/dlinklist.h"
#include "lib/util/debug.h"
#include "lib/util/time.h"
#include "lib/util/blocking.h"
#include "lib/util/become_daemon.h"

#include "version.h"
#include "ctdb_private.h"
#include "ctdb_client.h"

#include "protocol/protocol.h"
#include "protocol/protocol_basic.h"
#include "protocol/protocol_api.h"

#include "common/rb_tree.h"
#include "common/reqid.h"
#include "common/system.h"
#include "common/common.h"
#include "common/logging.h"
#include "common/pidfile.h"
#include "common/sock_io.h"
#include "common/srvid.h"

#include "conf/ctdb_config.h"
#include "conf/node.h"

struct ctdb_client_pid_list {
	struct ctdb_client_pid_list *next, *prev;
	struct ctdb_context *ctdb;
	pid_t pid;
	struct ctdb_client *client;
};

const char *ctdbd_pidfile = NULL;
static struct pidfile_context *ctdbd_pidfile_ctx = NULL;

static void daemon_incoming_packet(void *, struct ctdb_req_header *);

static pid_t __ctdbd_pid;

static void print_exit_message(void)
{
	if (getpid() == __ctdbd_pid) {
		DEBUG(DEBUG_NOTICE,("CTDB daemon shutting down\n"));

		/* Wait a second to allow pending log messages to be flushed */
		sleep(1);
	}
}

#ifdef HAVE_GETRUSAGE

struct cpu_check_threshold_data {
	unsigned short percent;
	struct timeval timeofday;
	struct timeval ru_time;
};

static void ctdb_cpu_check_threshold(struct tevent_context *ev,
				     struct tevent_timer *te,
				     struct timeval tv,
				     void *private_data)
{
	struct ctdb_context *ctdb = talloc_get_type_abort(
		private_data, struct ctdb_context);
	uint32_t interval = 60;

	static unsigned short threshold = 0;
	static struct cpu_check_threshold_data prev = {
		.percent = 0,
		.timeofday = { .tv_sec = 0 },
		.ru_time = { .tv_sec = 0 },
	};

	struct rusage usage;
	struct cpu_check_threshold_data curr = {
		.percent = 0,
	};
	int64_t ru_time_diff, timeofday_diff;
	bool first;
	int ret;

	/*
	 * Cache the threshold so that we don't waste time checking
	 * the environment variable every time
	 */
	if (threshold == 0) {
		const char *t;

		threshold = 90;

		t = getenv("CTDB_TEST_CPU_USAGE_THRESHOLD");
		if (t != NULL) {
			int th;

			th = atoi(t);
			if (th <= 0 || th > 100) {
				DBG_WARNING("Failed to parse env var: %s\n", t);
			} else {
				threshold = th;
			}
		}
	}

	ret = getrusage(RUSAGE_SELF, &usage);
	if (ret != 0) {
		DBG_WARNING("rusage() failed: %d\n", ret);
		goto next;
	}

	/* Sum the system and user CPU usage */
	curr.ru_time = timeval_sum(&usage.ru_utime, &usage.ru_stime);

	curr.timeofday = tv;

	first = timeval_is_zero(&prev.timeofday);
	if (first) {
		/* No previous values recorded so no calculation to do */
		goto done;
	}

	timeofday_diff = usec_time_diff(&curr.timeofday, &prev.timeofday);
	if (timeofday_diff <= 0) {
		/*
		 * Time went backwards or didn't progress so no (sane)
		 * calculation can be done
		 */
		goto done;
	}

	ru_time_diff = usec_time_diff(&curr.ru_time, &prev.ru_time);

	curr.percent = ru_time_diff * 100 / timeofday_diff;

	if (curr.percent >= threshold) {
		/* Log only if the utilisation changes */
		if (curr.percent != prev.percent) {
			D_WARNING("WARNING: CPU utilisation %hu%% >= "
				  "threshold (%hu%%)\n",
				  curr.percent,
				  threshold);
		}
	} else {
		/* Log if the utilisation falls below the threshold */
		if (prev.percent >= threshold) {
			D_WARNING("WARNING: CPU utilisation %hu%% < "
				  "threshold (%hu%%)\n",
				  curr.percent,
				  threshold);
		}
	}

done:
	prev = curr;

next:
	tevent_add_timer(ctdb->ev, ctdb,
			 timeval_current_ofs(interval, 0),
			 ctdb_cpu_check_threshold,
			 ctdb);
}

static void ctdb_start_cpu_check_threshold(struct ctdb_context *ctdb)
{
	tevent_add_timer(ctdb->ev, ctdb,
			 timeval_current(),
			 ctdb_cpu_check_threshold,
			 ctdb);
}
#endif /* HAVE_GETRUSAGE */

static void ctdb_time_tick(struct tevent_context *ev, struct tevent_timer *te,
				  struct timeval t, void *private_data)
{
	struct ctdb_context *ctdb = talloc_get_type(private_data, struct ctdb_context);

	if (getpid() != ctdb->ctdbd_pid) {
		return;
	}

	tevent_add_timer(ctdb->ev, ctdb,
			 timeval_current_ofs(1, 0),
			 ctdb_time_tick, ctdb);
}

/* Used to trigger a dummy event once per second, to make
 * detection of hangs more reliable.
 */
static void ctdb_start_time_tickd(struct ctdb_context *ctdb)
{
	tevent_add_timer(ctdb->ev, ctdb,
			 timeval_current_ofs(1, 0),
			 ctdb_time_tick, ctdb);
}

static void ctdb_start_periodic_events(struct ctdb_context *ctdb)
{
	/* start monitoring for connected/disconnected nodes */
	ctdb_start_keepalive(ctdb);

	/* start periodic update of tcp tickle lists */
       	ctdb_start_tcp_tickle_update(ctdb);

	/* start listening for recovery daemon pings */
	ctdb_control_recd_ping(ctdb);

	/* start listening to timer ticks */
	ctdb_start_time_tickd(ctdb);

#ifdef HAVE_GETRUSAGE
	ctdb_start_cpu_check_threshold(ctdb);
#endif /* HAVE_GETRUSAGE */
}

static void ignore_signal(int signum)
{
	struct sigaction act;

	memset(&act, 0, sizeof(act));

	act.sa_handler = SIG_IGN;
	sigemptyset(&act.sa_mask);
	sigaddset(&act.sa_mask, signum);
	sigaction(signum, &act, NULL);
}


/*
  send a packet to a client
 */
static int daemon_queue_send(struct ctdb_client *client, struct ctdb_req_header *hdr)
{
	CTDB_INCREMENT_STAT(client->ctdb, client_packets_sent);
	if (hdr->operation == CTDB_REQ_MESSAGE) {
		if (ctdb_queue_length(client->queue) > client->ctdb->tunable.max_queue_depth_drop_msg) {
			DEBUG(DEBUG_ERR,("CTDB_REQ_MESSAGE queue full - killing client connection.\n"));
			talloc_free(client);
			return -1;
		}
	}
	return ctdb_queue_send(client->queue, (uint8_t *)hdr, hdr->length);
}

/*
  message handler for when we are in daemon mode. This redirects the message
  to the right client
 */
static void daemon_message_handler(uint64_t srvid, TDB_DATA data,
				   void *private_data)
{
	struct ctdb_client *client = talloc_get_type(private_data, struct ctdb_client);
	struct ctdb_req_message_old *r;
	int len;

	/* construct a message to send to the client containing the data */
	len = offsetof(struct ctdb_req_message_old, data) + data.dsize;
	r = ctdbd_allocate_pkt(client->ctdb, client->ctdb, CTDB_REQ_MESSAGE,
			       len, struct ctdb_req_message_old);
	CTDB_NO_MEMORY_VOID(client->ctdb, r);

	talloc_set_name_const(r, "req_message packet");

	r->srvid         = srvid;
	r->datalen       = data.dsize;
	memcpy(&r->data[0], data.dptr, data.dsize);

	daemon_queue_send(client, &r->hdr);

	talloc_free(r);
}

/*
  this is called when the ctdb daemon received a ctdb request to
  set the srvid from the client
 */
int daemon_register_message_handler(struct ctdb_context *ctdb, uint32_t client_id, uint64_t srvid)
{
	struct ctdb_client *client = reqid_find(ctdb->idr, client_id, struct ctdb_client);
	int res;
	if (client == NULL) {
		DEBUG(DEBUG_ERR,("Bad client_id in daemon_request_register_message_handler\n"));
		return -1;
	}
	res = srvid_register(ctdb->srv, client, srvid, daemon_message_handler,
			     client);
	if (res != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to register handler %llu in daemon\n",
			 (unsigned long long)srvid));
	} else {
		DEBUG(DEBUG_INFO,(__location__ " Registered message handler for srvid=%llu\n",
			 (unsigned long long)srvid));
	}

	return res;
}

/*
  this is called when the ctdb daemon received a ctdb request to
  remove a srvid from the client
 */
int daemon_deregister_message_handler(struct ctdb_context *ctdb, uint32_t client_id, uint64_t srvid)
{
	struct ctdb_client *client = reqid_find(ctdb->idr, client_id, struct ctdb_client);
	if (client == NULL) {
		DEBUG(DEBUG_ERR,("Bad client_id in daemon_request_deregister_message_handler\n"));
		return -1;
	}
	return srvid_deregister(ctdb->srv, srvid, client);
}

void daemon_tunnel_handler(uint64_t tunnel_id, TDB_DATA data,
			   void *private_data)
{
	struct ctdb_client *client =
		talloc_get_type_abort(private_data, struct ctdb_client);
	struct ctdb_req_tunnel_old *c, *pkt;
	size_t len;

	pkt = (struct ctdb_req_tunnel_old *)data.dptr;

	len = offsetof(struct ctdb_req_tunnel_old, data) + pkt->datalen;
	c = ctdbd_allocate_pkt(client->ctdb, client->ctdb, CTDB_REQ_TUNNEL,
			       len, struct ctdb_req_tunnel_old);
	if (c == NULL) {
		DEBUG(DEBUG_ERR, ("Memory error in daemon_tunnel_handler\n"));
		return;
	}

	talloc_set_name_const(c, "req_tunnel packet");

	c->tunnel_id = tunnel_id;
	c->flags = pkt->flags;
	c->datalen = pkt->datalen;
	memcpy(c->data, pkt->data, pkt->datalen);

	daemon_queue_send(client, &c->hdr);

	talloc_free(c);
}

/*
  destroy a ctdb_client
*/
static int ctdb_client_destructor(struct ctdb_client *client)
{
	struct ctdb_db_context *ctdb_db;

	ctdb_takeover_client_destructor_hook(client);
	reqid_remove(client->ctdb->idr, client->client_id);
	client->ctdb->num_clients--;

	if (client->num_persistent_updates != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Client disconnecting with %u persistent updates in flight. Starting recovery\n", client->num_persistent_updates));
		client->ctdb->recovery_mode = CTDB_RECOVERY_ACTIVE;
	}
	ctdb_db = find_ctdb_db(client->ctdb, client->db_id);
	if (ctdb_db) {
		DEBUG(DEBUG_ERR, (__location__ " client exit while transaction "
				  "commit active. Forcing recovery.\n"));
		client->ctdb->recovery_mode = CTDB_RECOVERY_ACTIVE;

		/*
		 * trans3 transaction state:
		 *
		 * The destructor sets the pointer to NULL.
		 */
		talloc_free(ctdb_db->persistent_state);
	}

	return 0;
}


/*
  this is called when the ctdb daemon received a ctdb request message
  from a local client over the unix domain socket
 */
static void daemon_request_message_from_client(struct ctdb_client *client,
					       struct ctdb_req_message_old *c)
{
	TDB_DATA data;
	int res;

	if (c->hdr.destnode == CTDB_CURRENT_NODE) {
		c->hdr.destnode = ctdb_get_pnn(client->ctdb);
	}

	/* maybe the message is for another client on this node */
	if (ctdb_get_pnn(client->ctdb)==c->hdr.destnode) {
		ctdb_request_message(client->ctdb, (struct ctdb_req_header *)c);
		return;
	}

	/* its for a remote node */
	data.dptr = &c->data[0];
	data.dsize = c->datalen;
	res = ctdb_daemon_send_message(client->ctdb, c->hdr.destnode,
				       c->srvid, data);
	if (res != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to send message to remote node %u\n",
			 c->hdr.destnode));
	}
}


struct daemon_call_state {
	struct ctdb_client *client;
	uint32_t reqid;
	struct ctdb_call *call;
	struct timeval start_time;

	/* readonly request ? */
	uint32_t readonly_fetch;
	uint32_t client_callid;
};

/*
   complete a call from a client
*/
static void daemon_call_from_client_callback(struct ctdb_call_state *state)
{
	struct daemon_call_state *dstate = talloc_get_type(state->async.private_data,
							   struct daemon_call_state);
	struct ctdb_reply_call_old *r;
	int res;
	uint32_t length;
	struct ctdb_client *client = dstate->client;
	struct ctdb_db_context *ctdb_db = state->ctdb_db;

	talloc_steal(client, dstate);
	talloc_steal(dstate, dstate->call);

	res = ctdb_daemon_call_recv(state, dstate->call);
	if (res != 0) {
		DEBUG(DEBUG_ERR, (__location__ " ctdbd_call_recv() returned error\n"));
		CTDB_DECREMENT_STAT(client->ctdb, pending_calls);

		CTDB_UPDATE_LATENCY(client->ctdb, ctdb_db, "call_from_client_cb 1", call_latency, dstate->start_time);
		return;
	}

	length = offsetof(struct ctdb_reply_call_old, data) + dstate->call->reply_data.dsize;
	/* If the client asked for readonly FETCH, we remapped this to
	   FETCH_WITH_HEADER when calling the daemon. So we must
	   strip the extra header off the reply data before passing
	   it back to the client.
	*/
	if (dstate->readonly_fetch
	&& dstate->client_callid == CTDB_FETCH_FUNC) {
		length -= sizeof(struct ctdb_ltdb_header);
	}

	r = ctdbd_allocate_pkt(client->ctdb, dstate, CTDB_REPLY_CALL,
			       length, struct ctdb_reply_call_old);
	if (r == NULL) {
		DEBUG(DEBUG_ERR, (__location__ " Failed to allocate reply_call in ctdb daemon\n"));
		CTDB_DECREMENT_STAT(client->ctdb, pending_calls);
		CTDB_UPDATE_LATENCY(client->ctdb, ctdb_db, "call_from_client_cb 2", call_latency, dstate->start_time);
		return;
	}
	r->hdr.reqid        = dstate->reqid;
	r->status           = dstate->call->status;

	if (dstate->readonly_fetch
	&& dstate->client_callid == CTDB_FETCH_FUNC) {
		/* client only asked for a FETCH so we must strip off
		   the extra ctdb_ltdb header
		*/
		r->datalen          = dstate->call->reply_data.dsize - sizeof(struct ctdb_ltdb_header);
		memcpy(&r->data[0], dstate->call->reply_data.dptr + sizeof(struct ctdb_ltdb_header), r->datalen);
	} else {
		r->datalen          = dstate->call->reply_data.dsize;
		memcpy(&r->data[0], dstate->call->reply_data.dptr, r->datalen);
	}

	res = daemon_queue_send(client, &r->hdr);
	if (res == -1) {
		/* client is dead - return immediately */
		return;
	}
	if (res != 0) {
		DEBUG(DEBUG_ERR, (__location__ " Failed to queue packet from daemon to client\n"));
	}
	CTDB_UPDATE_LATENCY(client->ctdb, ctdb_db, "call_from_client_cb 3", call_latency, dstate->start_time);
	CTDB_DECREMENT_STAT(client->ctdb, pending_calls);
	talloc_free(dstate);
}

struct ctdb_daemon_packet_wrap {
	struct ctdb_context *ctdb;
	uint32_t client_id;
};

/*
  a wrapper to catch disconnected clients
 */
static void daemon_incoming_packet_wrap(void *p, struct ctdb_req_header *hdr)
{
	struct ctdb_client *client;
	struct ctdb_daemon_packet_wrap *w = talloc_get_type(p,
							    struct ctdb_daemon_packet_wrap);
	if (w == NULL) {
		DEBUG(DEBUG_CRIT,(__location__ " Bad packet type '%s'\n", talloc_get_name(p)));
		return;
	}

	client = reqid_find(w->ctdb->idr, w->client_id, struct ctdb_client);
	if (client == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " Packet for disconnected client %u\n",
			 w->client_id));
		talloc_free(w);
		return;
	}
	talloc_free(w);

	/* process it */
	daemon_incoming_packet(client, hdr);
}

struct ctdb_deferred_fetch_call {
	struct ctdb_deferred_fetch_call *next, *prev;
	struct ctdb_req_call_old *c;
	struct ctdb_daemon_packet_wrap *w;
};

struct ctdb_deferred_fetch_queue {
	struct ctdb_deferred_fetch_call *deferred_calls;
};

struct ctdb_deferred_requeue {
	struct ctdb_deferred_fetch_call *dfc;
	struct ctdb_client *client;
};

/* called from a timer event and starts reprocessing the deferred call.*/
static void reprocess_deferred_call(struct tevent_context *ev,
				    struct tevent_timer *te,
				    struct timeval t, void *private_data)
{
	struct ctdb_deferred_requeue *dfr = (struct ctdb_deferred_requeue *)private_data;
	struct ctdb_client *client = dfr->client;

	talloc_steal(client, dfr->dfc->c);
	daemon_incoming_packet(client, (struct ctdb_req_header *)dfr->dfc->c);
	talloc_free(dfr);
}

/* the referral context is destroyed either after a timeout or when the initial
   fetch-lock has finished.
   at this stage, immediately start reprocessing the queued up deferred
   calls so they get reprocessed immediately (and since we are dmaster at
   this stage, trigger the waiting smbd processes to pick up and acquire the
   record right away.
*/
static int deferred_fetch_queue_destructor(struct ctdb_deferred_fetch_queue *dfq)
{

	/* need to reprocess the packets from the queue explicitly instead of
	   just using a normal destructor since we need to
	   call the clients in the same order as the requests queued up
	*/
	while (dfq->deferred_calls != NULL) {
		struct ctdb_client *client;
		struct ctdb_deferred_fetch_call *dfc = dfq->deferred_calls;
		struct ctdb_deferred_requeue *dfr;

		DLIST_REMOVE(dfq->deferred_calls, dfc);

		client = reqid_find(dfc->w->ctdb->idr, dfc->w->client_id, struct ctdb_client);
		if (client == NULL) {
			DEBUG(DEBUG_ERR,(__location__ " Packet for disconnected client %u\n",
				 dfc->w->client_id));
			continue;
		}

		/* process it by pushing it back onto the eventloop */
		dfr = talloc(client, struct ctdb_deferred_requeue);
		if (dfr == NULL) {
			DEBUG(DEBUG_ERR,("Failed to allocate deferred fetch requeue structure\n"));
			continue;
		}

		dfr->dfc    = talloc_steal(dfr, dfc);
		dfr->client = client;

		tevent_add_timer(dfc->w->ctdb->ev, client, timeval_zero(),
				 reprocess_deferred_call, dfr);
	}

	return 0;
}

/* insert the new deferral context into the rb tree.
   there should never be a pre-existing context here, but check for it
   warn and destroy the previous context if there is already a deferral context
   for this key.
*/
static void *insert_dfq_callback(void *parm, void *data)
{
        if (data) {
		DEBUG(DEBUG_ERR,("Already have DFQ registered. Free old %p and create new %p\n", data, parm));
                talloc_free(data);
        }
        return parm;
}

/* if the original fetch-lock did not complete within a reasonable time,
   free the context and context for all deferred requests to cause them to be
   re-inserted into the event system.
*/
static void dfq_timeout(struct tevent_context *ev, struct tevent_timer *te,
			struct timeval t, void *private_data)
{
	talloc_free(private_data);
}

/* This function is used in the local daemon to register a KEY in a database
   for being "fetched"
   While the remote fetch is in-flight, any further attempts to re-fetch the
   same record will be deferred until the fetch completes.
*/
static int setup_deferred_fetch_locks(struct ctdb_db_context *ctdb_db, struct ctdb_call *call)
{
	uint32_t *k;
	struct ctdb_deferred_fetch_queue *dfq;

	k = ctdb_key_to_idkey(call, call->key);
	if (k == NULL) {
		DEBUG(DEBUG_ERR,("Failed to allocate key for deferred fetch\n"));
		return -1;
	}

	dfq  = talloc(call, struct ctdb_deferred_fetch_queue);
	if (dfq == NULL) {
		DEBUG(DEBUG_ERR,("Failed to allocate key for deferred fetch queue structure\n"));
		talloc_free(k);
		return -1;
	}
	dfq->deferred_calls = NULL;

	trbt_insertarray32_callback(ctdb_db->deferred_fetch, k[0], &k[0], insert_dfq_callback, dfq);

	talloc_set_destructor(dfq, deferred_fetch_queue_destructor);

	/* If the fetch hasn't completed in 30 seconds, just tear it all down
	   and let it try again as the events are reissued */
	tevent_add_timer(ctdb_db->ctdb->ev, dfq, timeval_current_ofs(30, 0),
			 dfq_timeout, dfq);

	talloc_free(k);
	return 0;
}

/* check if this is a duplicate request to a fetch already in-flight
   if it is, make this call deferred to be reprocessed later when
   the in-flight fetch completes.
*/
static int requeue_duplicate_fetch(struct ctdb_db_context *ctdb_db, struct ctdb_client *client, TDB_DATA key, struct ctdb_req_call_old *c)
{
	uint32_t *k;
	struct ctdb_deferred_fetch_queue *dfq;
	struct ctdb_deferred_fetch_call *dfc;

	k = ctdb_key_to_idkey(c, key);
	if (k == NULL) {
		DEBUG(DEBUG_ERR,("Failed to allocate key for deferred fetch\n"));
		return -1;
	}

	dfq = trbt_lookuparray32(ctdb_db->deferred_fetch, k[0], &k[0]);
	if (dfq == NULL) {
		talloc_free(k);
		return -1;
	}


	talloc_free(k);

	dfc = talloc(dfq, struct ctdb_deferred_fetch_call);
	if (dfc == NULL) {
		DEBUG(DEBUG_ERR, ("Failed to allocate deferred fetch call structure\n"));
		return -1;
	}

	dfc->w = talloc(dfc, struct ctdb_daemon_packet_wrap);
	if (dfc->w == NULL) {
		DEBUG(DEBUG_ERR,("Failed to allocate deferred fetch daemon packet wrap structure\n"));
		talloc_free(dfc);
		return -1;
	}

	dfc->c = talloc_steal(dfc, c);
	dfc->w->ctdb = ctdb_db->ctdb;
	dfc->w->client_id = client->client_id;

	DLIST_ADD_END(dfq->deferred_calls, dfc);

	return 0;
}


/*
  this is called when the ctdb daemon received a ctdb request call
  from a local client over the unix domain socket
 */
static void daemon_request_call_from_client(struct ctdb_client *client,
					    struct ctdb_req_call_old *c)
{
	struct ctdb_call_state *state;
	struct ctdb_db_context *ctdb_db;
	struct daemon_call_state *dstate;
	struct ctdb_call *call;
	struct ctdb_ltdb_header header;
	TDB_DATA key, data;
	int ret;
	struct ctdb_context *ctdb = client->ctdb;
	struct ctdb_daemon_packet_wrap *w;

	CTDB_INCREMENT_STAT(ctdb, total_calls);
	CTDB_INCREMENT_STAT(ctdb, pending_calls);

	ctdb_db = find_ctdb_db(client->ctdb, c->db_id);
	if (!ctdb_db) {
		DEBUG(DEBUG_ERR, (__location__ " Unknown database in request. db_id==0x%08x\n",
			  c->db_id));
		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		return;
	}

	if (ctdb_db->unhealthy_reason) {
		/*
		 * this is just a warning, as the tdb should be empty anyway,
		 * and only persistent databases can be unhealthy, which doesn't
		 * use this code patch
		 */
		DEBUG(DEBUG_WARNING,("warn: db(%s) unhealty in daemon_request_call_from_client(): %s\n",
				     ctdb_db->db_name, ctdb_db->unhealthy_reason));
	}

	key.dptr = c->data;
	key.dsize = c->keylen;

	w = talloc(ctdb, struct ctdb_daemon_packet_wrap);
	CTDB_NO_MEMORY_VOID(ctdb, w);

	w->ctdb = ctdb;
	w->client_id = client->client_id;

	ret = ctdb_ltdb_lock_fetch_requeue(ctdb_db, key, &header,
					   (struct ctdb_req_header *)c, &data,
					   daemon_incoming_packet_wrap, w, true);
	if (ret == -2) {
		/* will retry later */
		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		return;
	}

	talloc_free(w);

	if (ret != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Unable to fetch record\n"));
		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		return;
	}


	/* check if this fetch request is a duplicate for a
	   request we already have in flight. If so defer it until
	   the first request completes.
	*/
	if (ctdb->tunable.fetch_collapse == 1) {
		if (requeue_duplicate_fetch(ctdb_db, client, key, c) == 0) {
			ret = ctdb_ltdb_unlock(ctdb_db, key);
			if (ret != 0) {
				DEBUG(DEBUG_ERR,(__location__ " ctdb_ltdb_unlock() failed with error %d\n", ret));
			}
			CTDB_DECREMENT_STAT(ctdb, pending_calls);
			talloc_free(data.dptr);
			return;
		}
	}

	/* Dont do READONLY if we don't have a tracking database */
	if ((c->flags & CTDB_WANT_READONLY) && !ctdb_db_readonly(ctdb_db)) {
		c->flags &= ~CTDB_WANT_READONLY;
	}

	if (header.flags & CTDB_REC_RO_REVOKE_COMPLETE) {
		header.flags &= ~CTDB_REC_RO_FLAGS;
		CTDB_INCREMENT_STAT(ctdb, total_ro_revokes);
		CTDB_INCREMENT_DB_STAT(ctdb_db, db_ro_revokes);
		if (ctdb_ltdb_store(ctdb_db, key, &header, data) != 0) {
			ctdb_fatal(ctdb, "Failed to write header with cleared REVOKE flag");
		}
		/* and clear out the tracking data */
		if (tdb_delete(ctdb_db->rottdb, key) != 0) {
			DEBUG(DEBUG_ERR,(__location__ " Failed to clear out trackingdb record\n"));
		}
	}

	/* if we are revoking, we must defer all other calls until the revoke
	 * had completed.
	 */
	if (header.flags & CTDB_REC_RO_REVOKING_READONLY) {
		talloc_free(data.dptr);
		ret = ctdb_ltdb_unlock(ctdb_db, key);

		if (ctdb_add_revoke_deferred_call(ctdb, ctdb_db, key, (struct ctdb_req_header *)c, daemon_incoming_packet, client) != 0) {
			ctdb_fatal(ctdb, "Failed to add deferred call for revoke child");
		}
		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		return;
	}

	if ((header.dmaster == ctdb->pnn)
	&& (!(c->flags & CTDB_WANT_READONLY))
	&& (header.flags & (CTDB_REC_RO_HAVE_DELEGATIONS|CTDB_REC_RO_HAVE_READONLY)) ) {
		header.flags   |= CTDB_REC_RO_REVOKING_READONLY;
		if (ctdb_ltdb_store(ctdb_db, key, &header, data) != 0) {
			ctdb_fatal(ctdb, "Failed to store record with HAVE_DELEGATIONS set");
		}
		ret = ctdb_ltdb_unlock(ctdb_db, key);

		if (ctdb_start_revoke_ro_record(ctdb, ctdb_db, key, &header, data) != 0) {
			ctdb_fatal(ctdb, "Failed to start record revoke");
		}
		talloc_free(data.dptr);

		if (ctdb_add_revoke_deferred_call(ctdb, ctdb_db, key, (struct ctdb_req_header *)c, daemon_incoming_packet, client) != 0) {
			ctdb_fatal(ctdb, "Failed to add deferred call for revoke child");
		}

		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		return;
	}

	dstate = talloc(client, struct daemon_call_state);
	if (dstate == NULL) {
		ret = ctdb_ltdb_unlock(ctdb_db, key);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,(__location__ " ctdb_ltdb_unlock() failed with error %d\n", ret));
		}

		DEBUG(DEBUG_ERR,(__location__ " Unable to allocate dstate\n"));
		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		return;
	}
	dstate->start_time = timeval_current();
	dstate->client = client;
	dstate->reqid  = c->hdr.reqid;
	talloc_steal(dstate, data.dptr);

	call = dstate->call = talloc_zero(dstate, struct ctdb_call);
	if (call == NULL) {
		ret = ctdb_ltdb_unlock(ctdb_db, key);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,(__location__ " ctdb_ltdb_unlock() failed with error %d\n", ret));
		}

		DEBUG(DEBUG_ERR,(__location__ " Unable to allocate call\n"));
		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		CTDB_UPDATE_LATENCY(ctdb, ctdb_db, "call_from_client 1", call_latency, dstate->start_time);
		return;
	}

	dstate->readonly_fetch = 0;
	call->call_id = c->callid;
	call->key = key;
	call->call_data.dptr = c->data + c->keylen;
	call->call_data.dsize = c->calldatalen;
	call->flags = c->flags;

	if (c->flags & CTDB_WANT_READONLY) {
		/* client wants readonly record, so translate this into a
		   fetch with header. remember what the client asked for
		   so we can remap the reply back to the proper format for
		   the client in the reply
		 */
		dstate->client_callid = call->call_id;
		call->call_id = CTDB_FETCH_WITH_HEADER_FUNC;
		dstate->readonly_fetch = 1;
	}

	if (header.dmaster == ctdb->pnn) {
		state = ctdb_call_local_send(ctdb_db, call, &header, &data);
	} else {
		state = ctdb_daemon_call_send_remote(ctdb_db, call, &header);
		if (ctdb->tunable.fetch_collapse == 1) {
			/* This request triggered a remote fetch-lock.
			   set up a deferral for this key so any additional
			   fetch-locks are deferred until the current one
			   finishes.
			 */
			setup_deferred_fetch_locks(ctdb_db, call);
		}
	}

	ret = ctdb_ltdb_unlock(ctdb_db, key);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,(__location__ " ctdb_ltdb_unlock() failed with error %d\n", ret));
	}

	if (state == NULL) {
		DEBUG(DEBUG_ERR,(__location__ " Unable to setup call send\n"));
		CTDB_DECREMENT_STAT(ctdb, pending_calls);
		CTDB_UPDATE_LATENCY(ctdb, ctdb_db, "call_from_client 2", call_latency, dstate->start_time);
		return;
	}
	talloc_steal(state, dstate);
	talloc_steal(client, state);

	state->async.fn = daemon_call_from_client_callback;
	state->async.private_data = dstate;
}


static void daemon_request_control_from_client(struct ctdb_client *client,
					       struct ctdb_req_control_old *c);
static void daemon_request_tunnel_from_client(struct ctdb_client *client,
					      struct ctdb_req_tunnel_old *c);

/* data contains a packet from the client */
static void daemon_incoming_packet(void *p, struct ctdb_req_header *hdr)
{
	struct ctdb_client *client = talloc_get_type(p, struct ctdb_client);
	TALLOC_CTX *tmp_ctx;
	struct ctdb_context *ctdb = client->ctdb;

	/* place the packet as a child of a tmp_ctx. We then use
	   talloc_free() below to free it. If any of the calls want
	   to keep it, then they will steal it somewhere else, and the
	   talloc_free() will be a no-op */
	tmp_ctx = talloc_new(client);
	talloc_steal(tmp_ctx, hdr);

	if (hdr->ctdb_magic != CTDB_MAGIC) {
		ctdb_set_error(client->ctdb, "Non CTDB packet rejected in daemon\n");
		goto done;
	}

	if (hdr->ctdb_version != CTDB_PROTOCOL) {
		ctdb_set_error(client->ctdb, "Bad CTDB version 0x%x rejected in daemon\n", hdr->ctdb_version);
		goto done;
	}

	switch (hdr->operation) {
	case CTDB_REQ_CALL:
		CTDB_INCREMENT_STAT(ctdb, client.req_call);
		daemon_request_call_from_client(client, (struct ctdb_req_call_old *)hdr);
		break;

	case CTDB_REQ_MESSAGE:
		CTDB_INCREMENT_STAT(ctdb, client.req_message);
		daemon_request_message_from_client(client, (struct ctdb_req_message_old *)hdr);
		break;

	case CTDB_REQ_CONTROL:
		CTDB_INCREMENT_STAT(ctdb, client.req_control);
		daemon_request_control_from_client(client, (struct ctdb_req_control_old *)hdr);
		break;

	case CTDB_REQ_TUNNEL:
		CTDB_INCREMENT_STAT(ctdb, client.req_tunnel);
		daemon_request_tunnel_from_client(client, (struct ctdb_req_tunnel_old *)hdr);
		break;

	default:
		DEBUG(DEBUG_CRIT,(__location__ " daemon: unrecognized operation %u\n",
			 hdr->operation));
	}

done:
	talloc_free(tmp_ctx);
}

/*
  called when the daemon gets a incoming packet
 */
static void ctdb_daemon_read_cb(uint8_t *data, size_t cnt, void *args)
{
	struct ctdb_client *client = talloc_get_type(args, struct ctdb_client);
	struct ctdb_req_header *hdr;

	if (cnt == 0) {
		talloc_free(client);
		return;
	}

	CTDB_INCREMENT_STAT(client->ctdb, client_packets_recv);

	if (cnt < sizeof(*hdr)) {
		ctdb_set_error(client->ctdb, "Bad packet length %u in daemon\n",
			       (unsigned)cnt);
		return;
	}
	hdr = (struct ctdb_req_header *)data;

	if (hdr->ctdb_magic != CTDB_MAGIC) {
		ctdb_set_error(client->ctdb, "Non CTDB packet rejected\n");
		goto err_out;
	}

	if (hdr->ctdb_version != CTDB_PROTOCOL) {
		ctdb_set_error(client->ctdb, "Bad CTDB version 0x%x rejected in daemon\n", hdr->ctdb_version);
		goto err_out;
	}

	DEBUG(DEBUG_DEBUG,(__location__ " client request %u of type %u length %u from "
		 "node %u to %u\n", hdr->reqid, hdr->operation, hdr->length,
		 hdr->srcnode, hdr->destnode));

	/* it is the responsibility of the incoming packet function to free 'data' */
	daemon_incoming_packet(client, hdr);
	return;

err_out:
	TALLOC_FREE(data);
}


static int ctdb_clientpid_destructor(struct ctdb_client_pid_list *client_pid)
{
	if (client_pid->ctdb->client_pids != NULL) {
		DLIST_REMOVE(client_pid->ctdb->client_pids, client_pid);
	}

	return 0;
}

static int get_new_client_id(struct reqid_context *idr,
			     struct ctdb_client *client,
			     uint32_t *out)
{
	uint32_t client_id;

	client_id = reqid_new(idr, client);
	/*
	 * Some places in the code (e.g. ctdb_control_db_attach(),
	 * ctdb_control_db_detach()) assign a special meaning to
	 * client_id 0.  The assumption is that if client_id is 0 then
	 * the control has come from another daemon.  Therefore, we
	 * should never return client_id == 0.
	 */
	if (client_id == 0) {
		/*
		 * Don't leak ID 0.  This is safe because the ID keeps
		 * increasing.  A test will be added to ensure that
		 * this doesn't change.
		 */
		reqid_remove(idr, 0);

		client_id = reqid_new(idr, client);
	}

	if (client_id == REQID_INVALID) {
		return EINVAL;
	}

	if (client_id == 0) {
		/* Every other ID must have been used and we can't use 0 */
		reqid_remove(idr, 0);
		return EINVAL;
	}

	*out = client_id;
	return 0;
}

static void ctdb_accept_client(struct tevent_context *ev,
			       struct tevent_fd *fde, uint16_t flags,
			       void *private_data)
{
	struct sockaddr_un addr;
	socklen_t len;
	int fd;
	struct ctdb_context *ctdb = talloc_get_type(private_data, struct ctdb_context);
	struct ctdb_client *client;
	struct ctdb_client_pid_list *client_pid;
	pid_t peer_pid = 0;
	int ret;

	memset(&addr, 0, sizeof(addr));
	len = sizeof(addr);
	fd = accept(ctdb->daemon.sd, (struct sockaddr *)&addr, &len);
	if (fd == -1) {
		return;
	}
	smb_set_close_on_exec(fd);

	ret = set_blocking(fd, false);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,
		      (__location__
		       " failed to set socket non-blocking (%s)\n",
		       strerror(errno)));
		close(fd);
		return;
	}

	set_close_on_exec(fd);

	DEBUG(DEBUG_DEBUG,(__location__ " Created SOCKET FD:%d to connected child\n", fd));

	client = talloc_zero(ctdb, struct ctdb_client);
	if (ctdb_get_peer_pid(fd, &peer_pid) == 0) {
		DEBUG(DEBUG_INFO,("Connected client with pid:%u\n", (unsigned)peer_pid));
	}

	client->ctdb = ctdb;
	client->fd = fd;

	ret = get_new_client_id(ctdb->idr, client, &client->client_id);
	if (ret != 0) {
		DBG_ERR("Unable to get client ID (%d)\n", ret);
		close(fd);
		talloc_free(client);
		return;
	}

	client->pid = peer_pid;

	client_pid = talloc(client, struct ctdb_client_pid_list);
	if (client_pid == NULL) {
		DEBUG(DEBUG_ERR,("Failed to allocate client pid structure\n"));
		close(fd);
		talloc_free(client);
		return;
	}
	client_pid->ctdb   = ctdb;
	client_pid->pid    = peer_pid;
	client_pid->client = client;

	DLIST_ADD(ctdb->client_pids, client_pid);

	client->queue = ctdb_queue_setup(ctdb, client, fd, CTDB_DS_ALIGNMENT,
					 ctdb_daemon_read_cb, client,
					 "client-%u", client->pid);

	talloc_set_destructor(client, ctdb_client_destructor);
	talloc_set_destructor(client_pid, ctdb_clientpid_destructor);
	ctdb->num_clients++;
}



/*
 * Create a unix domain socket, bind it, secure it and listen.  Return
 * the file descriptor for the socket.
 */
static int ux_socket_bind(struct ctdb_context *ctdb, bool test_mode_enabled)
{
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	int ret;

	ctdb->daemon.sd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (ctdb->daemon.sd == -1) {
		return -1;
	}

	strncpy(addr.sun_path, ctdb->daemon.name, sizeof(addr.sun_path)-1);

	if (! sock_clean(ctdb->daemon.name)) {
		return -1;
	}

	set_close_on_exec(ctdb->daemon.sd);

	ret = set_blocking(ctdb->daemon.sd, false);
	if (ret != 0) {
		DBG_ERR("Failed to set socket non-blocking (%s)\n",
			strerror(errno));
		goto failed;
	}

	ret = bind(ctdb->daemon.sd, (struct sockaddr *)&addr, sizeof(addr));
	if (ret == -1) {
		D_ERR("Unable to bind on ctdb socket '%s'\n", ctdb->daemon.name);
		goto failed;
	}

	if (!test_mode_enabled) {
		ret = chown(ctdb->daemon.name, geteuid(), getegid());
		if (ret != 0 && !test_mode_enabled) {
			D_ERR("Unable to secure (chown) ctdb socket '%s'\n",
			      ctdb->daemon.name);
			goto failed;
		}
	}

	ret = chmod(ctdb->daemon.name, 0700);
	if (ret != 0) {
		D_ERR("Unable to secure (chmod) ctdb socket '%s'\n",
		      ctdb->daemon.name);
		goto failed;
	}


	ret = listen(ctdb->daemon.sd, 100);
	if (ret != 0) {
		D_ERR("Unable to listen on ctdb socket '%s'\n",
		      ctdb->daemon.name);
		goto failed;
	}

	D_NOTICE("Listening to ctdb socket %s\n", ctdb->daemon.name);
	return 0;

failed:
	close(ctdb->daemon.sd);
	ctdb->daemon.sd = -1;
	return -1;
}

struct ctdb_node *ctdb_find_node(struct ctdb_context *ctdb, uint32_t pnn)
{
	struct ctdb_node *node = NULL;
	unsigned int i;

	if (pnn == CTDB_CURRENT_NODE) {
		pnn = ctdb->pnn;
	}

	/* Always found: PNN correctly set just before this is called */
	for (i = 0; i < ctdb->num_nodes; i++) {
		node = ctdb->nodes[i];
		if (pnn == node->pnn) {
			return node;
		}
	}

	return NULL;
}

static void initialise_node_flags (struct ctdb_context *ctdb)
{
	struct ctdb_node *node = NULL;

	node = ctdb_find_node(ctdb, CTDB_CURRENT_NODE);
	/*
	 * PNN correctly set just before this is called so always
	 * found but keep static analysers happy...
	 */
	if (node == NULL) {
		DBG_ERR("Unable to find current node\n");
		return;
	}

	node->flags &= ~NODE_FLAGS_DISCONNECTED;

	/* do we start out in DISABLED mode? */
	if (ctdb->start_as_disabled != 0) {
		D_ERR("This node is configured to start in DISABLED state\n");
		node->flags |= NODE_FLAGS_PERMANENTLY_DISABLED;
	}
	/* do we start out in STOPPED mode? */
	if (ctdb->start_as_stopped != 0) {
		D_ERR("This node is configured to start in STOPPED state\n");
		node->flags |= NODE_FLAGS_STOPPED;
	}
}

static void ctdb_setup_event_callback(struct ctdb_context *ctdb, int status,
				      void *private_data)
{
	if (status != 0) {
		ctdb_die(ctdb, "Failed to run setup event");
	}
	ctdb_run_notification_script(ctdb, "setup");

	/* Start the recovery daemon */
	if (ctdb_start_recoverd(ctdb) != 0) {
		DEBUG(DEBUG_ALERT,("Failed to start recovery daemon\n"));
		exit(11);
	}

	ctdb_start_periodic_events(ctdb);

	ctdb_wait_for_first_recovery(ctdb);
}

static struct timeval tevent_before_wait_ts;
static struct timeval tevent_after_wait_ts;

static void ctdb_tevent_trace_init(void)
{
	struct timeval now;

	now = timeval_current();

	tevent_before_wait_ts = now;
	tevent_after_wait_ts = now;
}

static void ctdb_tevent_trace(enum tevent_trace_point tp,
			      void *private_data)
{
	struct timeval diff;
	struct timeval now;
	struct ctdb_context *ctdb =
		talloc_get_type(private_data, struct ctdb_context);

	if (getpid() != ctdb->ctdbd_pid) {
		return;
	}

	now = timeval_current();

	switch (tp) {
	case TEVENT_TRACE_BEFORE_WAIT:
		diff = tevent_timeval_until(&tevent_after_wait_ts, &now);
		if (diff.tv_sec > 3) {
			DEBUG(DEBUG_ERR,
			      ("Handling event took %ld seconds!\n",
			       (long)diff.tv_sec));
		}
		tevent_before_wait_ts = now;
		break;

	case TEVENT_TRACE_AFTER_WAIT:
		diff = tevent_timeval_until(&tevent_before_wait_ts, &now);
		if (diff.tv_sec > 3) {
			DEBUG(DEBUG_ERR,
			      ("No event for %ld seconds!\n",
			       (long)diff.tv_sec));
		}
		tevent_after_wait_ts = now;
		break;

	default:
		/* Do nothing for future tevent trace points */ ;
	}
}

static void ctdb_remove_pidfile(void)
{
	TALLOC_FREE(ctdbd_pidfile_ctx);
}

static void ctdb_create_pidfile(TALLOC_CTX *mem_ctx)
{
	if (ctdbd_pidfile != NULL) {
		int ret = pidfile_context_create(mem_ctx, ctdbd_pidfile,
						 &ctdbd_pidfile_ctx);
		if (ret != 0) {
			DEBUG(DEBUG_ERR,
			      ("Failed to create PID file %s\n",
			       ctdbd_pidfile));
			exit(11);
		}

		DEBUG(DEBUG_NOTICE, ("Created PID file %s\n", ctdbd_pidfile));
		atexit(ctdb_remove_pidfile);
	}
}

static void ctdb_initialise_vnn_map(struct ctdb_context *ctdb)
{
	unsigned int i, j, count;

	/* initialize the vnn mapping table, skipping any deleted nodes */
	ctdb->vnn_map = talloc(ctdb, struct ctdb_vnn_map);
	CTDB_NO_MEMORY_FATAL(ctdb, ctdb->vnn_map);

	count = 0;
	for (i = 0; i < ctdb->num_nodes; i++) {
		if ((ctdb->nodes[i]->flags & NODE_FLAGS_DELETED) == 0) {
			count++;
		}
	}

	ctdb->vnn_map->generation = INVALID_GENERATION;
	ctdb->vnn_map->size = count;
	ctdb->vnn_map->map = talloc_array(ctdb->vnn_map, uint32_t, ctdb->vnn_map->size);
	CTDB_NO_MEMORY_FATAL(ctdb, ctdb->vnn_map->map);

	for(i=0, j=0; i < ctdb->vnn_map->size; i++) {
		if (ctdb->nodes[i]->flags & NODE_FLAGS_DELETED) {
			continue;
		}
		ctdb->vnn_map->map[j] = i;
		j++;
	}
}

static void ctdb_set_my_pnn(struct ctdb_context *ctdb)
{
	if (ctdb->address == NULL) {
		ctdb_fatal(ctdb,
			   "Can not determine PNN - node address is not set\n");
	}

	ctdb->pnn = ctdb_ip_to_pnn(ctdb, ctdb->address);
	if (ctdb->pnn == CTDB_UNKNOWN_PNN) {
		ctdb_fatal(ctdb,
			   "Can not determine PNN - unknown node address\n");
	}

	D_NOTICE("PNN is %u\n", ctdb->pnn);
}

static void stdin_handler(struct tevent_context *ev,
			  struct tevent_fd *fde,
			  uint16_t flags,
			  void *private_data)
{
	struct ctdb_context *ctdb = talloc_get_type_abort(
		private_data, struct ctdb_context);
	ssize_t nread;
	char c;

	nread = read(STDIN_FILENO, &c, 1);
	if (nread != 1) {
		D_ERR("stdin closed, exiting\n");
		talloc_free(fde);
		ctdb_shutdown_sequence(ctdb, EPIPE);
	}
}

static int setup_stdin_handler(struct ctdb_context *ctdb)
{
	struct tevent_fd *fde;
	struct stat st;
	int ret;

	ret = fstat(STDIN_FILENO, &st);
	if (ret != 0) {
		/* Problem with stdin, ignore... */
		DBG_INFO("Can't fstat() stdin\n");
		return 0;
	}

	if (!S_ISFIFO(st.st_mode)) {
		DBG_INFO("Not a pipe...\n");
		return 0;
	}

	fde = tevent_add_fd(ctdb->ev,
			    ctdb,
			    STDIN_FILENO,
			    TEVENT_FD_READ,
			    stdin_handler,
			    ctdb);
	if (fde == NULL) {
		return ENOMEM;
	}

	DBG_INFO("Set up stdin handler\n");
	return 0;
}

static void fork_only(void)
{
	pid_t pid;

	pid = fork();
	if (pid == -1) {
		D_ERR("Fork failed (errno=%d)\n", errno);
		exit(1);
	}

	if (pid != 0) {
		/* Parent simply exits... */
		exit(0);
	}
}

static void sighup_hook(void *private_data)
{
	struct ctdb_context *ctdb = talloc_get_type_abort(private_data,
							  struct ctdb_context);

	if (ctdb->recoverd_pid > 0) {
		kill(ctdb->recoverd_pid, SIGHUP);
	}
	ctdb_event_reopen_logs(ctdb);
}

/*
  start the protocol going as a daemon
*/
int ctdb_start_daemon(struct ctdb_context *ctdb,
		      bool interactive,
		      bool test_mode_enabled)
{
	bool status;
	int ret;
	struct tevent_fd *fde;

	/* Fork if not interactive */
	if (!interactive) {
		if (test_mode_enabled) {
			/* Keep stdin open */
			fork_only();
		} else {
			/* Fork, close stdin, start a session */
			become_daemon(true, false, false);
		}
	}

	ignore_signal(SIGPIPE);
	ignore_signal(SIGUSR1);

	ctdb->ctdbd_pid = getpid();
	DEBUG(DEBUG_ERR, ("Starting CTDBD (Version %s) as PID: %u\n",
			  SAMBA_VERSION_STRING, ctdb->ctdbd_pid));
	ctdb_create_pidfile(ctdb);

	/* create a unix domain stream socket to listen to */
	ret = ux_socket_bind(ctdb, test_mode_enabled);
	if (ret != 0) {
		D_ERR("Cannot continue.  Exiting!\n");
		exit(10);
	}

	/* Make sure we log something when the daemon terminates.
	 * This must be the first exit handler to run (so the last to
	 * be registered.
	 */
	__ctdbd_pid = getpid();
	atexit(print_exit_message);

	if (ctdb->do_setsched) {
		/* try to set us up as realtime */
		if (!set_scheduler()) {
			exit(1);
		}
		DEBUG(DEBUG_NOTICE, ("Set real-time scheduler priority\n"));
	}

	ctdb->ev = tevent_context_init(NULL);
	if (ctdb->ev == NULL) {
		DEBUG(DEBUG_ALERT,("tevent_context_init() failed\n"));
		exit(1);
	}
	tevent_loop_allow_nesting(ctdb->ev);
	ctdb_tevent_trace_init();
	tevent_set_trace_callback(ctdb->ev, ctdb_tevent_trace, ctdb);

	status = logging_setup_sighup_handler(ctdb->ev,
					      ctdb,
					      sighup_hook,
					      ctdb);
	if (!status) {
		D_ERR("Failed to set up signal handler for SIGHUP\n");
		exit(1);
	}

	/* set up a handler to pick up sigchld */
	if (ctdb_init_sigchld(ctdb) == NULL) {
		DEBUG(DEBUG_CRIT,("Failed to set up signal handler for SIGCHLD\n"));
		exit(1);
	}

	if (!interactive) {
		ctdb_set_child_logging(ctdb);
	}

	/* Exit if stdin is closed */
	if (test_mode_enabled) {
		ret = setup_stdin_handler(ctdb);
		if (ret != 0) {
			DBG_ERR("Failed to setup stdin handler\n");
			exit(1);
		}
	}

	TALLOC_FREE(ctdb->srv);
	if (srvid_init(ctdb, &ctdb->srv) != 0) {
		DEBUG(DEBUG_CRIT,("Failed to setup message srvid context\n"));
		exit(1);
	}

	TALLOC_FREE(ctdb->tunnels);
	if (srvid_init(ctdb, &ctdb->tunnels) != 0) {
		DEBUG(DEBUG_ERR, ("Failed to setup tunnels context\n"));
		exit(1);
	}

	/* initialize statistics collection */
	ctdb_statistics_init(ctdb);

	/* force initial recovery for election */
	ctdb->recovery_mode = CTDB_RECOVERY_ACTIVE;

	if (ctdb_start_eventd(ctdb) != 0) {
		DEBUG(DEBUG_ERR, ("Failed to start event daemon\n"));
		exit(1);
	}

	ctdb_set_runstate(ctdb, CTDB_RUNSTATE_INIT);
	ret = ctdb_event_script(ctdb, CTDB_EVENT_INIT);
	if (ret != 0) {
		ctdb_die(ctdb, "Failed to run init event\n");
	}
	ctdb_run_notification_script(ctdb, "init");

	if (strcmp(ctdb->transport, "tcp") == 0) {
		ret = ctdb_tcp_init(ctdb);
	}
#ifdef USE_INFINIBAND
	if (strcmp(ctdb->transport, "ib") == 0) {
		ret = ctdb_ibw_init(ctdb);
	}
#endif
	if (ret != 0) {
		DEBUG(DEBUG_ERR,("Failed to initialise transport '%s'\n", ctdb->transport));
		return -1;
	}

	if (ctdb->methods == NULL) {
		DEBUG(DEBUG_ALERT,(__location__ " Can not initialize transport. ctdb->methods is NULL\n"));
		ctdb_fatal(ctdb, "transport is unavailable. can not initialize.");
	}

	/* Initialise the transport.  This sets the node address if it
	 * was not set via the command-line. */
	if (ctdb->methods->initialise(ctdb) != 0) {
		ctdb_fatal(ctdb, "transport failed to initialise");
	}

	ctdb_set_my_pnn(ctdb);

	initialise_node_flags(ctdb);

	ret = ctdb_set_public_addresses(ctdb);
	if (ret == -1) {
		D_ERR("Unable to setup public IP addresses\n");
		exit(1);
	}

	ctdb_initialise_vnn_map(ctdb);

	/* attach to existing databases */
	if (ctdb_attach_databases(ctdb) != 0) {
		ctdb_fatal(ctdb, "Failed to attach to databases\n");
	}

	/* start frozen, then let the first election sort things out */
	if (!ctdb_blocking_freeze(ctdb)) {
		ctdb_fatal(ctdb, "Failed to get initial freeze\n");
	}

	/* now start accepting clients, only can do this once frozen */
	fde = tevent_add_fd(ctdb->ev, ctdb, ctdb->daemon.sd, TEVENT_FD_READ,
			    ctdb_accept_client, ctdb);
	if (fde == NULL) {
		ctdb_fatal(ctdb, "Failed to add daemon socket to event loop");
	}
	tevent_fd_set_auto_close(fde);

	/* Start the transport */
	if (ctdb->methods->start(ctdb) != 0) {
		DEBUG(DEBUG_ALERT,("transport failed to start!\n"));
		ctdb_fatal(ctdb, "transport failed to start");
	}

	/* Recovery daemon and timed events are started from the
	 * callback, only after the setup event completes
	 * successfully.
	 */
	ctdb_set_runstate(ctdb, CTDB_RUNSTATE_SETUP);
	ret = ctdb_event_script_callback(ctdb,
					 ctdb,
					 ctdb_setup_event_callback,
					 ctdb,
					 CTDB_EVENT_SETUP,
					 "%s",
					 "");
	if (ret != 0) {
		DEBUG(DEBUG_CRIT,("Failed to set up 'setup' event\n"));
		exit(1);
	}

	lockdown_memory(ctdb->valgrinding);

	/* go into a wait loop to allow other nodes to complete */
	tevent_loop_wait(ctdb->ev);

	DEBUG(DEBUG_CRIT,("event_loop_wait() returned. this should not happen\n"));
	exit(1);
}

/*
  allocate a packet for use in daemon<->daemon communication
 */
struct ctdb_req_header *_ctdb_transport_allocate(struct ctdb_context *ctdb,
						 TALLOC_CTX *mem_ctx,
						 enum ctdb_operation operation,
						 size_t length, size_t slength,
						 const char *type)
{
	int size;
	struct ctdb_req_header *hdr;

	length = MAX(length, slength);
	size = (length+(CTDB_DS_ALIGNMENT-1)) & ~(CTDB_DS_ALIGNMENT-1);

	if (ctdb->methods == NULL) {
		DEBUG(DEBUG_INFO,(__location__ " Unable to allocate transport packet for operation %u of length %u. Transport is DOWN.\n",
			 operation, (unsigned)length));
		return NULL;
	}

	hdr = (struct ctdb_req_header *)ctdb->methods->allocate_pkt(mem_ctx, size);
	if (hdr == NULL) {
		DEBUG(DEBUG_ERR,("Unable to allocate transport packet for operation %u of length %u\n",
			 operation, (unsigned)length));
		return NULL;
	}
	talloc_set_name_const(hdr, type);
	memset(hdr, 0, slength);
	hdr->length       = length;
	hdr->operation    = operation;
	hdr->ctdb_magic   = CTDB_MAGIC;
	hdr->ctdb_version = CTDB_PROTOCOL;
	hdr->generation   = ctdb->vnn_map->generation;
	hdr->srcnode      = ctdb->pnn;

	return hdr;
}

struct daemon_control_state {
	struct daemon_control_state *next, *prev;
	struct ctdb_client *client;
	struct ctdb_req_control_old *c;
	uint32_t reqid;
	struct ctdb_node *node;
};

/*
  callback when a control reply comes in
 */
static void daemon_control_callback(struct ctdb_context *ctdb,
				    int32_t status, TDB_DATA data,
				    const char *errormsg,
				    void *private_data)
{
	struct daemon_control_state *state = talloc_get_type(private_data,
							     struct daemon_control_state);
	struct ctdb_client *client = state->client;
	struct ctdb_reply_control_old *r;
	size_t len;
	int ret;

	/* construct a message to send to the client containing the data */
	len = offsetof(struct ctdb_reply_control_old, data) + data.dsize;
	if (errormsg) {
		len += strlen(errormsg);
	}
	r = ctdbd_allocate_pkt(ctdb, state, CTDB_REPLY_CONTROL, len,
			       struct ctdb_reply_control_old);
	CTDB_NO_MEMORY_VOID(ctdb, r);

	r->hdr.reqid     = state->reqid;
	r->status        = status;
	r->datalen       = data.dsize;
	r->errorlen = 0;
	memcpy(&r->data[0], data.dptr, data.dsize);
	if (errormsg) {
		r->errorlen = strlen(errormsg);
		memcpy(&r->data[r->datalen], errormsg, r->errorlen);
	}

	ret = daemon_queue_send(client, &r->hdr);
	if (ret != -1) {
		talloc_free(state);
	}
}

/*
  fail all pending controls to a disconnected node
 */
void ctdb_daemon_cancel_controls(struct ctdb_context *ctdb, struct ctdb_node *node)
{
	struct daemon_control_state *state;
	while ((state = node->pending_controls)) {
		DLIST_REMOVE(node->pending_controls, state);
		daemon_control_callback(ctdb, (uint32_t)-1, tdb_null,
					"node is disconnected", state);
	}
}

/*
  destroy a daemon_control_state
 */
static int daemon_control_destructor(struct daemon_control_state *state)
{
	if (state->node) {
		DLIST_REMOVE(state->node->pending_controls, state);
	}
	return 0;
}

/*
  this is called when the ctdb daemon received a ctdb request control
  from a local client over the unix domain socket
 */
static void daemon_request_control_from_client(struct ctdb_client *client,
					       struct ctdb_req_control_old *c)
{
	TDB_DATA data;
	int res;
	struct daemon_control_state *state;
	TALLOC_CTX *tmp_ctx = talloc_new(client);

	if (c->hdr.destnode == CTDB_CURRENT_NODE) {
		c->hdr.destnode = client->ctdb->pnn;
	}

	state = talloc(client, struct daemon_control_state);
	CTDB_NO_MEMORY_VOID(client->ctdb, state);

	state->client = client;
	state->c = talloc_steal(state, c);
	state->reqid = c->hdr.reqid;
	if (ctdb_validate_pnn(client->ctdb, c->hdr.destnode)) {
		state->node = client->ctdb->nodes[c->hdr.destnode];
		DLIST_ADD(state->node->pending_controls, state);
	} else {
		state->node = NULL;
	}

	talloc_set_destructor(state, daemon_control_destructor);

	if (c->flags & CTDB_CTRL_FLAG_NOREPLY) {
		talloc_steal(tmp_ctx, state);
	}

	data.dptr = &c->data[0];
	data.dsize = c->datalen;
	res = ctdb_daemon_send_control(client->ctdb, c->hdr.destnode,
				       c->srvid, c->opcode, client->client_id,
				       c->flags,
				       data, daemon_control_callback,
				       state);
	if (res != 0) {
		DEBUG(DEBUG_ERR,(__location__ " Failed to send control to remote node %u\n",
			 c->hdr.destnode));
	}

	talloc_free(tmp_ctx);
}

static void daemon_request_tunnel_from_client(struct ctdb_client *client,
					      struct ctdb_req_tunnel_old *c)
{
	TDB_DATA data;
	int ret;

	if (! ctdb_validate_pnn(client->ctdb, c->hdr.destnode)) {
		DEBUG(DEBUG_ERR, ("Invalid destination 0x%x\n",
				  c->hdr.destnode));
		return;
	}

	ret = srvid_exists(client->ctdb->tunnels, c->tunnel_id, NULL);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,
		      ("tunnel id 0x%"PRIx64" not registered, dropping pkt\n",
		       c->tunnel_id));
		return;
	}

	data = (TDB_DATA) {
		.dsize = c->datalen,
		.dptr = &c->data[0],
	};

	ret = ctdb_daemon_send_tunnel(client->ctdb, c->hdr.destnode,
				      c->tunnel_id, c->flags, data);
	if (ret != 0) {
		DEBUG(DEBUG_ERR, ("Failed to set tunnel to remote note %u\n",
				  c->hdr.destnode));
	}
}

/*
  register a call function
*/
int ctdb_daemon_set_call(struct ctdb_context *ctdb, uint32_t db_id,
			 ctdb_fn_t fn, int id)
{
	struct ctdb_registered_call *call;
	struct ctdb_db_context *ctdb_db;

	ctdb_db = find_ctdb_db(ctdb, db_id);
	if (ctdb_db == NULL) {
		return -1;
	}

	call = talloc(ctdb_db, struct ctdb_registered_call);
	call->fn = fn;
	call->id = id;

	DLIST_ADD(ctdb_db->calls, call);
	return 0;
}



/*
  this local messaging handler is ugly, but is needed to prevent
  recursion in ctdb_send_message() when the destination node is the
  same as the source node
 */
struct ctdb_local_message {
	struct ctdb_context *ctdb;
	uint64_t srvid;
	TDB_DATA data;
};

static void ctdb_local_message_trigger(struct tevent_context *ev,
				       struct tevent_timer *te,
				       struct timeval t, void *private_data)
{
	struct ctdb_local_message *m = talloc_get_type(
		private_data, struct ctdb_local_message);

	srvid_dispatch(m->ctdb->srv, m->srvid, CTDB_SRVID_ALL, m->data);
	talloc_free(m);
}

static int ctdb_local_message(struct ctdb_context *ctdb, uint64_t srvid, TDB_DATA data)
{
	struct ctdb_local_message *m;
	m = talloc(ctdb, struct ctdb_local_message);
	CTDB_NO_MEMORY(ctdb, m);

	m->ctdb = ctdb;
	m->srvid = srvid;
	m->data  = data;
	m->data.dptr = talloc_memdup(m, m->data.dptr, m->data.dsize);
	if (m->data.dptr == NULL) {
		talloc_free(m);
		return -1;
	}

	/* this needs to be done as an event to prevent recursion */
	tevent_add_timer(ctdb->ev, m, timeval_zero(),
			 ctdb_local_message_trigger, m);
	return 0;
}

/*
  send a ctdb message
*/
int ctdb_daemon_send_message(struct ctdb_context *ctdb, uint32_t pnn,
			     uint64_t srvid, TDB_DATA data)
{
	struct ctdb_req_message_old *r;
	int len;

	if (ctdb->methods == NULL) {
		DEBUG(DEBUG_INFO,(__location__ " Failed to send message. Transport is DOWN\n"));
		return -1;
	}

	/* see if this is a message to ourselves */
	if (pnn == ctdb->pnn) {
		return ctdb_local_message(ctdb, srvid, data);
	}

	len = offsetof(struct ctdb_req_message_old, data) + data.dsize;
	r = ctdb_transport_allocate(ctdb, ctdb, CTDB_REQ_MESSAGE, len,
				    struct ctdb_req_message_old);
	CTDB_NO_MEMORY(ctdb, r);

	r->hdr.destnode  = pnn;
	r->srvid         = srvid;
	r->datalen       = data.dsize;
	memcpy(&r->data[0], data.dptr, data.dsize);

	ctdb_queue_packet(ctdb, &r->hdr);

	talloc_free(r);
	return 0;
}



struct ctdb_client_notify_list {
	struct ctdb_client_notify_list *next, *prev;
	struct ctdb_context *ctdb;
	uint64_t srvid;
	TDB_DATA data;
};


static int ctdb_client_notify_destructor(struct ctdb_client_notify_list *nl)
{
	int ret;

	DEBUG(DEBUG_ERR,("Sending client notify message for srvid:%llu\n", (unsigned long long)nl->srvid));

	ret = ctdb_daemon_send_message(nl->ctdb,
				       CTDB_BROADCAST_CONNECTED,
				       nl->srvid,
				       nl->data);
	if (ret != 0) {
		DEBUG(DEBUG_ERR,("Failed to send client notify message\n"));
	}

	return 0;
}

int32_t ctdb_control_register_notify(struct ctdb_context *ctdb, uint32_t client_id, TDB_DATA indata)
{
	struct ctdb_notify_data_old *notify = (struct ctdb_notify_data_old *)indata.dptr;
        struct ctdb_client *client = reqid_find(ctdb->idr, client_id, struct ctdb_client);
	struct ctdb_client_notify_list *nl;

	DEBUG(DEBUG_INFO,("Register srvid %llu for client %d\n", (unsigned long long)notify->srvid, client_id));

	if (indata.dsize < offsetof(struct ctdb_notify_data_old, notify_data)) {
		DEBUG(DEBUG_ERR,(__location__ " Too little data in control : %d\n", (int)indata.dsize));
		return -1;
	}

	if (indata.dsize != (notify->len + offsetof(struct ctdb_notify_data_old, notify_data))) {
		DEBUG(DEBUG_ERR,(__location__ " Wrong amount of data in control. Got %d, expected %d\n", (int)indata.dsize, (int)(notify->len + offsetof(struct ctdb_notify_data_old, notify_data))));
		return -1;
	}


        if (client == NULL) {
                DEBUG(DEBUG_ERR,(__location__ " Could not find client parent structure. You can not send this control to a remote node\n"));
                return -1;
        }

	for(nl=client->notify; nl; nl=nl->next) {
		if (nl->srvid == notify->srvid) {
			break;
		}
	}
	if (nl != NULL) {
                DEBUG(DEBUG_ERR,(__location__ " Notification for srvid:%llu already exists for this client\n", (unsigned long long)notify->srvid));
                return -1;
        }

	nl = talloc(client, struct ctdb_client_notify_list);
	CTDB_NO_MEMORY(ctdb, nl);
	nl->ctdb       = ctdb;
	nl->srvid      = notify->srvid;
	nl->data.dsize = notify->len;
	nl->data.dptr  = talloc_memdup(nl, notify->notify_data,
				       nl->data.dsize);
	CTDB_NO_MEMORY(ctdb, nl->data.dptr);

	DLIST_ADD(client->notify, nl);
	talloc_set_destructor(nl, ctdb_client_notify_destructor);

	return 0;
}

int32_t ctdb_control_deregister_notify(struct ctdb_context *ctdb, uint32_t client_id, TDB_DATA indata)
{
	uint64_t srvid = *(uint64_t *)indata.dptr;
        struct ctdb_client *client = reqid_find(ctdb->idr, client_id, struct ctdb_client);
	struct ctdb_client_notify_list *nl;

	DEBUG(DEBUG_INFO,("Deregister srvid %llu for client %d\n", (unsigned long long)srvid, client_id));

        if (client == NULL) {
                DEBUG(DEBUG_ERR,(__location__ " Could not find client parent structure. You can not send this control to a remote node\n"));
                return -1;
        }

	for(nl=client->notify; nl; nl=nl->next) {
		if (nl->srvid == srvid) {
			break;
		}
	}
	if (nl == NULL) {
                DEBUG(DEBUG_ERR,(__location__ " No notification for srvid:%llu found for this client\n", (unsigned long long)srvid));
                return -1;
        }

	DLIST_REMOVE(client->notify, nl);
	talloc_set_destructor(nl, NULL);
	talloc_free(nl);

	return 0;
}

struct ctdb_client *ctdb_find_client_by_pid(struct ctdb_context *ctdb, pid_t pid)
{
	struct ctdb_client_pid_list *client_pid;

	for (client_pid = ctdb->client_pids; client_pid; client_pid=client_pid->next) {
		if (client_pid->pid == pid) {
			return client_pid->client;
		}
	}
	return NULL;
}


/* This control is used by samba when probing if a process (of a samba daemon)
   exists on the node.
   Samba does this when it needs/wants to check if a subrecord in one of the
   databases is still valid, or if it is stale and can be removed.
   If the node is in unhealthy or stopped state we just kill of the samba
   process holding this sub-record and return to the calling samba that
   the process does not exist.
   This allows us to forcefully recall subrecords registered by samba processes
   on banned and stopped nodes.
*/
int32_t ctdb_control_process_exists(struct ctdb_context *ctdb, pid_t pid)
{
        struct ctdb_client *client;

	client = ctdb_find_client_by_pid(ctdb, pid);
	if (client == NULL) {
		return -1;
	}

	if (ctdb->nodes[ctdb->pnn]->flags & NODE_FLAGS_INACTIVE) {
		DEBUG(DEBUG_NOTICE,
		      ("Killing client with pid:%d on banned/stopped node\n",
		       (int)pid));
		talloc_free(client);
		return -1;
	}

	return kill(pid, 0);
}

int32_t ctdb_control_check_pid_srvid(struct ctdb_context *ctdb,
				     TDB_DATA indata)
{
	struct ctdb_client_pid_list *client_pid;
	pid_t pid;
	uint64_t srvid;
	int ret;

	pid = *(pid_t *)indata.dptr;
	srvid = *(uint64_t *)(indata.dptr + sizeof(pid_t));

	for (client_pid = ctdb->client_pids;
	     client_pid != NULL;
	     client_pid = client_pid->next) {
		if (client_pid->pid == pid) {
			ret = srvid_exists(ctdb->srv, srvid,
					   client_pid->client);
			if (ret == 0) {
				return 0;
			}
		}
	}

	return -1;
}

int ctdb_control_getnodesfile(struct ctdb_context *ctdb,
			      uint32_t opcode,
			      TDB_DATA indata,
			      TDB_DATA *outdata)
{
	struct ctdb_node_map *node_map = NULL;
	size_t len;
	uint8_t *buf = NULL;
	size_t npush = 0;
	int ret = -1;

	CHECK_CONTROL_DATA_SIZE(0);

	node_map = ctdb_read_nodes(ctdb, ctdb->nodes_source);
	if (node_map == NULL) {
		D_ERR("Failed to read nodes file\n");
		return -1;
	}

	len = ctdb_node_map_len(node_map);
	buf = talloc_size(ctdb, len);
	if (buf == NULL) {
		goto done;
	}

	ctdb_node_map_push(node_map, buf, &npush);
	if (len != npush) {
		talloc_free(buf);
		goto done;
	}

	outdata->dptr  = buf;
	outdata->dsize = len;
	ret = 0;
done:
	talloc_free(node_map);
	return ret;
}

/*
 * Construct a SRVID for accepting replies to this ctdbd.  The bottom
 * 24 bits of the PNN are used in the top half.  extra_mask is used in
 * the bottom half.
 */

static uint64_t ctdb_srvid_id(struct ctdb_context *ctdb, uint32_t extra_mask)
{
	uint64_t pnn_mask = (uint64_t)(ctdb->pnn & 0xFFFFFF) << 32;

	return CTDB_SRVID_SERVER_RANGE | pnn_mask | extra_mask;
}

/*
 * Do a takeover run on shutdown
 *
 * This allows for a graceful transition of resources to another node.
 * This ensures all nodes go into grace for NFS and, with an extra
 * timeout, allows data transfer for SMB durable handles.
 *
 * Nodes need to be in CTDB_RUNSTATE_RUNNING to host public IP
 * addresses.  So, this node will release all IPs.  The good news is
 * that a node can remain leader when in CTDB_RUNSTATE_SHUTDOWN, so
 * shutting down the cluster will not be adversely delayed by this.
 * The only issue to guard against is delaying shutdown of this node
 * if it is the only node and doesn't have CTDB_CAP_RECMASTER, in
 * which case there is no node to do the takeover run.  Hence, the
 * timeout.
 */

struct shutdown_takeover_state {
	bool takeover_done;
	bool timed_out;
	struct tevent_timer *te;
	unsigned int leader_broadcast_count;
};

static void shutdown_takeover_handler(uint64_t srvid,
				      TDB_DATA data,
				      void *private_data)
{
	struct shutdown_takeover_state *state = private_data;
	int32_t result = 0;
	size_t count = 0;
	int ret = 0;

	ret = ctdb_int32_pull(data.dptr, data.dsize, &result, &count);
	if (ret == EMSGSIZE) {
		/*
		 * Can't happen unless there's bug somewhere else, so
		 * just ignore - ctdb_shutdown_takeover() will
		 * probably time out...
		 */
		DBG_WARNING("Wrong size for result\n");
		return;
	}

	if (result == -1) {
		/*
		 * No early return - can't afford endless retries
		 * during shutdown...
		 */
		DBG_WARNING("Takeover run failed\n");
	} else {
		DBG_NOTICE("Takeover run successful by node=%"PRIi32"\n",
			   result);
	}

	state->takeover_done = true;
}

static void shutdown_timeout_handler(struct tevent_context *ev,
				     struct tevent_timer *te,
				     struct timeval yt,
				     void *private_data)
{
	struct shutdown_takeover_state *state = private_data;

	TALLOC_FREE(state->te);
	state->timed_out = true;
}

static void shutdown_leader_handler(uint64_t srvid,
				    TDB_DATA data,
				    void *private_data)
{
	struct shutdown_takeover_state *state = private_data;
	uint32_t pnn = 0;
	size_t count = 0;
	int ret = 0;

	ret = ctdb_uint32_pull(data.dptr, data.dsize, &pnn, &count);
	if (ret == EMSGSIZE) {
		/*
		 * Can't happen unless there's bug somewhere else, so
		 * just ignore
		 */
		DBG_WARNING("Wrong size for result\n");
		return;
	}

	DBG_DEBUG("Leader broadcast received from node=%"PRIu32"\n", pnn);
	state->leader_broadcast_count++;
}

static void ctdb_shutdown_takeover(struct ctdb_context *ctdb)
{
	struct shutdown_takeover_state state = {
		.takeover_done = false,
		.timed_out = false,
		.te = NULL,
		.leader_broadcast_count = 0,
	};
	/*
	 * This one is memcpy()ed onto the wire, so initialise below
	 * after ZERO_STRUCT(), to keep things valgrind clean
	 */
	struct ctdb_srvid_message rd;
	struct TDB_DATA rddata = {
		.dptr = (uint8_t *)&rd,
		.dsize = sizeof(rd),
	};
	int ret = 0;

	if (ctdb_config.shutdown_failover_timeout <= 0) {
		return;
	}

	ZERO_STRUCT(rd);
	rd = (struct ctdb_srvid_message) {
		.pnn = ctdb->pnn,
		.srvid = ctdb_srvid_id(ctdb, 0),
	};

	ret = srvid_register(ctdb->srv,
			     ctdb->srv,
			     rd.srvid,
			     shutdown_takeover_handler,
			     &state);
	if (ret != 0) {
		DBG_WARNING("Failed to register takeover run handler\n");
		return;
	}

	state.te = tevent_add_timer(
		ctdb->ev,
		ctdb->srv,
		timeval_current_ofs(ctdb_config.shutdown_failover_timeout, 0),
		shutdown_timeout_handler,
		&state);
	if (state.te == NULL) {
		DBG_WARNING("Failed to set shutdown timeout\n");
		goto done;
	}

	ret = srvid_register(ctdb->srv,
			     ctdb->srv,
			     CTDB_SRVID_LEADER,
			     shutdown_leader_handler,
			     &state);
	if (ret != 0) {
		/* Leader broadcasts provide extra information, so no
		 * problem if they can't be monitored...
		 */
		DBG_WARNING("Failed to register leader handler\n");
	}

	ret = ctdb_daemon_send_message(ctdb,
				       CTDB_BROADCAST_CONNECTED,
				       CTDB_SRVID_TAKEOVER_RUN,
				       rddata);
	if (ret != 0) {
		DBG_WARNING("Failed to send IP takeover run request\n");
		goto done;
	}

	while (!state.takeover_done && !state.timed_out) {
		tevent_loop_once(ctdb->ev);
	}

	if (state.takeover_done) {
		goto done;
	}

	if (state.timed_out) {
		DBG_WARNING("Timed out waiting for takeover run "
			    "(%u leader broadcasts received)\n",
			    state.leader_broadcast_count);
	}
done:
	srvid_deregister(ctdb->srv, CTDB_SRVID_TAKEOVER_RUN, &state);
	srvid_deregister(ctdb->srv, CTDB_SRVID_LEADER, &state);
	TALLOC_FREE(state.te);

	if (!state.takeover_done || ctdb_config.shutdown_extra_timeout <= 0) {
		return;
	}

	state.timed_out = false;
	state.te = tevent_add_timer(
		ctdb->ev,
		ctdb->srv,
		timeval_current_ofs(ctdb_config.shutdown_extra_timeout, 0),
		shutdown_timeout_handler,
		&state);
	if (state.te == NULL) {
		DBG_WARNING("Failed to set extra timeout\n");
		return;
	}

	DBG_NOTICE("Waiting %ds for shutdown extra timeout\n",
		   ctdb_config.shutdown_extra_timeout);
	while (!state.timed_out) {
		tevent_loop_once(ctdb->ev);
	}
	DBG_INFO("shutdown extra timeout complete\n");
}

void ctdb_shutdown_sequence(struct ctdb_context *ctdb, int exit_code)
{
	if (ctdb->runstate == CTDB_RUNSTATE_SHUTDOWN) {
		D_NOTICE("Already shutting down so will not proceed.\n");
		return;
	}

	D_ERR("Shutdown sequence commencing.\n");
	ctdb_set_runstate(ctdb, CTDB_RUNSTATE_SHUTDOWN);
	ctdb_shutdown_takeover(ctdb);
	ctdb_stop_recoverd(ctdb);
	ctdb_stop_keepalive(ctdb);
	ctdb_stop_monitoring(ctdb);
	ctdb_event_script(ctdb, CTDB_EVENT_SHUTDOWN);
	ctdb_stop_eventd(ctdb);
	if (ctdb->methods != NULL && ctdb->methods->shutdown != NULL) {
		ctdb->methods->shutdown(ctdb);
	}

	D_ERR("Shutdown sequence complete, exiting.\n");
	exit(exit_code);
}

/* When forking the main daemon and the child process needs to connect
 * back to the daemon as a client process, this function can be used
 * to change the ctdb context from daemon into client mode.  The child
 * process must be created using ctdb_fork() and not fork() -
 * ctdb_fork() does some necessary housekeeping.
 */
int switch_from_server_to_client(struct ctdb_context *ctdb)
{
	int ret;

	if (ctdb->daemon.sd != -1) {
		close(ctdb->daemon.sd);
		ctdb->daemon.sd = -1;
	}

	/* get a new event context */
	ctdb->ev = tevent_context_init(ctdb);
	if (ctdb->ev == NULL) {
		DEBUG(DEBUG_ALERT,("tevent_context_init() failed\n"));
		exit(1);
	}
	tevent_loop_allow_nesting(ctdb->ev);

	/* Connect to main CTDB daemon */
	ret = ctdb_socket_connect(ctdb);
	if (ret != 0) {
		DEBUG(DEBUG_ALERT, (__location__ " Failed to init ctdb client\n"));
		return -1;
	}

	ctdb->can_send_controls = true;

	return 0;
}
