/* 
   Unix SMB/CIFS implementation.

   dcerpc connect functions

   Copyright (C) Andrew Tridgell 2003
   Copyright (C) Jelmer Vernooij 2004
   Copyright (C) Andrew Bartlett <abartlet@samba.org> 2005-2007
   Copyright (C) Rafal Szczesniak  2005
   
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

#define SOURCE4_LIBRPC_INTERNALS 1

#include "includes.h"
#include "libcli/composite/composite.h"
#include "lib/events/events.h"
#include "librpc/rpc/dcerpc.h"
#include "librpc/rpc/dcerpc_proto.h"
#include "auth/credentials/credentials.h"
#include "param/param.h"
#include "libcli/resolve/resolve.h"
#include "lib/util/util_net.h"

struct sec_conn_state {
	struct dcerpc_pipe *pipe;
	struct dcerpc_pipe *pipe2;
	struct dcerpc_binding *binding;
};


static void continue_open_smb(struct composite_context *ctx);
static void continue_open_tcp(struct composite_context *ctx);
static void continue_open_ncalrpc(struct composite_context *ctx);
static void continue_open_ncacn_unix(struct composite_context *ctx);
static void continue_pipe_open(struct composite_context *c);


/*
  Send request to create a secondary dcerpc connection from a primary
  connection
*/
_PUBLIC_ struct composite_context* dcerpc_secondary_connection_send(struct dcerpc_pipe *p,
							const struct dcerpc_binding *b)
{
	struct composite_context *c;
	struct sec_conn_state *s;
	struct composite_context *pipe_smb_req;
	struct composite_context *pipe_tcp_req;
	const char *localaddress = NULL;
	struct composite_context *pipe_ncalrpc_req;
	const char *ncalrpc_dir = NULL;
	struct composite_context *pipe_unix_req;
	const char *host;
	const char *target_hostname;
	const char *endpoint;

	/* composite context allocation and setup */
	c = composite_create(p, p->conn->event_ctx);
	if (c == NULL) return NULL;

	s = talloc_zero(c, struct sec_conn_state);
	if (composite_nomem(s, c)) return c;
	c->private_data = s;

	s->pipe     = p;
	s->binding  = dcerpc_binding_dup(s, b);
	if (composite_nomem(s->binding, c)) return c;

	/* initialise second dcerpc pipe based on primary pipe's event context */
	s->pipe2 = dcerpc_pipe_init(c, s->pipe->conn->event_ctx);
	if (composite_nomem(s->pipe2, c)) return c;

	if (DEBUGLEVEL >= 10)
		s->pipe2->conn->packet_log_dir = s->pipe->conn->packet_log_dir;

	host = dcerpc_binding_get_string_option(s->binding, "host");
	if (host == NULL) {
		/*
		 * We may fallback to the host of the given connection
		 */
		host = dcerpc_binding_get_string_option(s->pipe->binding,
							"host");
	}
	target_hostname = dcerpc_binding_get_string_option(s->binding, "target_hostname");
	if (target_hostname == NULL) {
		/*
		 * We may fallback to the target_hostname of the given connection
		 */
		target_hostname = dcerpc_binding_get_string_option(s->pipe->binding,
								   "target_hostname");
	}
	endpoint = dcerpc_binding_get_string_option(s->binding, "endpoint");
	if (endpoint == NULL) {
		/*
		 * We may fallback to the endpoint of the given connection
		 */
		endpoint = dcerpc_binding_get_string_option(s->pipe->binding, "endpoint");
	}
	if (endpoint == NULL) {
		composite_error(c, NT_STATUS_INVALID_PARAMETER_MIX);
		return c;
	}

	/* open second dcerpc pipe using the same transport as for primary pipe */
	switch (s->pipe->conn->transport.transport) {
	case NCACN_NP:
		pipe_smb_req = dcerpc_secondary_smb_send(s->pipe->conn,
							 s->pipe2->conn,
							 endpoint);
		composite_continue(c, pipe_smb_req, continue_open_smb, c);
		return c;

	case NCACN_IP_TCP:
		if (host == NULL) {
			composite_error(c, NT_STATUS_INVALID_PARAMETER_MIX);
			return c;
		}

		if (!is_ipaddress(host)) {
			/*
			 * We may fallback to the host of the given connection
			 */
			host = dcerpc_binding_get_string_option(s->pipe->binding,
								"host");
			if (host == NULL) {
				composite_error(c, NT_STATUS_INVALID_PARAMETER_MIX);
				return c;
			}
			if (!is_ipaddress(host)) {
				composite_error(c, NT_STATUS_INVALID_PARAMETER_MIX);
				return c;
			}
		}

		localaddress = dcerpc_binding_get_string_option(s->binding,
								"localaddress");
		if (localaddress == NULL) {
			/*
			 * We may fallback to the localaddress of the given connection
			 */
			localaddress = dcerpc_binding_get_string_option(s->pipe->binding,
									"localaddress");
		}

		pipe_tcp_req = dcerpc_pipe_open_tcp_send(s->pipe2->conn,
							 localaddress,
							 host,
							 target_hostname,
							 atoi(endpoint),
							 resolve_context_init(s));
		composite_continue(c, pipe_tcp_req, continue_open_tcp, c);
		return c;

	case NCALRPC:
		ncalrpc_dir = dcerpc_binding_get_string_option(s->binding,
							       "ncalrpc_dir");
		if (ncalrpc_dir == NULL) {
			ncalrpc_dir = dcerpc_binding_get_string_option(s->pipe->binding,
								"ncalrpc_dir");
		}
		if (ncalrpc_dir == NULL) {
			composite_error(c, NT_STATUS_INVALID_PARAMETER_MIX);
			return c;
		}

		pipe_ncalrpc_req = dcerpc_pipe_open_pipe_send(s->pipe2->conn,
							      ncalrpc_dir,
							      endpoint);
		composite_continue(c, pipe_ncalrpc_req, continue_open_ncalrpc, c);
		return c;

	case NCACN_UNIX_STREAM:
		pipe_unix_req = dcerpc_pipe_open_unix_stream_send(s->pipe2->conn,
								  endpoint);
		composite_continue(c, pipe_unix_req, continue_open_ncacn_unix, c);
		return c;

	default:
		/* looks like a transport we don't support */
		composite_error(c, NT_STATUS_NOT_SUPPORTED);
	}

	return c;
}


/*
  Stage 2 of secondary_connection: Receive result of pipe open request on smb
*/
static void continue_open_smb(struct composite_context *ctx)
{
	struct composite_context *c = talloc_get_type(ctx->async.private_data,
						      struct composite_context);
	
	c->status = dcerpc_secondary_smb_recv(ctx);
	if (!composite_is_ok(c)) return;

	continue_pipe_open(c);
}


/*
  Stage 2 of secondary_connection: Receive result of pipe open request on tcp/ip
*/
static void continue_open_tcp(struct composite_context *ctx)
{
	struct composite_context *c = talloc_get_type(ctx->async.private_data,
						      struct composite_context);
	struct sec_conn_state *s = talloc_get_type_abort(c->private_data,
							 struct sec_conn_state);
	char *localaddr = NULL;
	char *remoteaddr = NULL;

	c->status = dcerpc_pipe_open_tcp_recv(ctx, s, &localaddr, &remoteaddr);
	if (!composite_is_ok(c)) return;

	c->status = dcerpc_binding_set_string_option(s->binding,
						     "localaddress",
						     localaddr);
	if (!composite_is_ok(c)) return;

	c->status = dcerpc_binding_set_string_option(s->binding,
						     "host",
						     remoteaddr);
	if (!composite_is_ok(c)) return;

	continue_pipe_open(c);
}

/*
  Stage 2 of secondary_connection: Receive result of pipe open request on ncalrpc
*/
static void continue_open_ncalrpc(struct composite_context *ctx)
{
	struct composite_context *c = talloc_get_type(ctx->async.private_data,
						      struct composite_context);

	c->status = dcerpc_pipe_open_pipe_recv(ctx);
	if (!composite_is_ok(c)) return;

	continue_pipe_open(c);
}

/*
  Stage 2 of secondary_connection: Receive result of pipe open request on ncacn_unix
*/
static void continue_open_ncacn_unix(struct composite_context *ctx)
{
	struct composite_context *c = talloc_get_type(ctx->async.private_data,
						      struct composite_context);

	c->status = dcerpc_pipe_open_unix_stream_recv(ctx);
	if (!composite_is_ok(c)) return;

	continue_pipe_open(c);
}


/*
  Stage 3 of secondary_connection: Get binding data and flags from primary pipe
  and say if we're done ok.
*/
static void continue_pipe_open(struct composite_context *c)
{
	struct sec_conn_state *s;

	s = talloc_get_type(c->private_data, struct sec_conn_state);

	s->pipe2->conn->flags = s->pipe->conn->flags;
	s->pipe2->binding     = dcerpc_binding_dup(s->pipe2, s->binding);
	if (composite_nomem(s->pipe2->binding, c)) {
		return;
	}

	composite_done(c);
}


/*
  Receive result of secondary rpc connection request and return
  second dcerpc pipe.
*/
_PUBLIC_ NTSTATUS dcerpc_secondary_connection_recv(struct composite_context *c,
					  struct dcerpc_pipe **p2)
{
	NTSTATUS status = composite_wait(c);
	struct sec_conn_state *s;

	s = talloc_get_type(c->private_data, struct sec_conn_state);

	if (NT_STATUS_IS_OK(status)) {
		*p2 = talloc_steal(s->pipe, s->pipe2);
	}

	talloc_free(c);
	return status;
}

/*
  Create a secondary DCERPC connection, then bind (and possibly
  authenticate) using the supplied credentials.

  This creates a second connection, to the same host (and on ncacn_np on the same connection) as the first
*/
struct sec_auth_conn_state {
	struct dcerpc_pipe *pipe2;
	const struct dcerpc_binding *binding;
	const struct ndr_interface_table *table;
	struct cli_credentials *credentials;
	struct composite_context *ctx;
	struct loadparm_context *lp_ctx;
};

static void dcerpc_secondary_auth_connection_bind(struct composite_context *ctx);
static void dcerpc_secondary_auth_connection_continue(struct composite_context *ctx);

_PUBLIC_ struct composite_context* dcerpc_secondary_auth_connection_send(struct dcerpc_pipe *p,
								const struct dcerpc_binding *binding,
								const struct ndr_interface_table *table,
								struct cli_credentials *credentials,
								struct loadparm_context *lp_ctx)
{

	struct composite_context *c, *secondary_conn_ctx;
	struct sec_auth_conn_state *s;
	
	/* composite context allocation and setup */
	c = composite_create(p, p->conn->event_ctx);
	if (c == NULL) return NULL;

	s = talloc_zero(c, struct sec_auth_conn_state);
	if (composite_nomem(s, c)) return c;
	c->private_data = s;
	s->ctx = c;

	s->binding  = binding;
	s->table    = table;
	s->credentials = credentials;
	s->lp_ctx = lp_ctx;
	
	secondary_conn_ctx = dcerpc_secondary_connection_send(p, binding);
	
	if (composite_nomem(secondary_conn_ctx, s->ctx)) {
		talloc_free(c);
		return NULL;
	}

	composite_continue(s->ctx, secondary_conn_ctx, dcerpc_secondary_auth_connection_bind,
			   s);
	return c;
}

/*
  Stage 2 of secondary_auth_connection: 
  Having made the secondary connection, we will need to do an (authenticated) bind
*/
static void dcerpc_secondary_auth_connection_bind(struct composite_context *ctx)
{
	struct composite_context *secondary_auth_ctx;
	struct sec_auth_conn_state *s = talloc_get_type(ctx->async.private_data,
							struct sec_auth_conn_state);
	
	s->ctx->status = dcerpc_secondary_connection_recv(ctx, &s->pipe2);
	if (!composite_is_ok(s->ctx)) return;
	
	secondary_auth_ctx = dcerpc_pipe_auth_send(s->pipe2, s->binding, s->table, s->credentials,
						   s->lp_ctx);
	composite_continue(s->ctx, secondary_auth_ctx, dcerpc_secondary_auth_connection_continue, s);
	
}

/*
  Stage 3 of secondary_auth_connection: Receive result of authenticated bind request
*/
static void dcerpc_secondary_auth_connection_continue(struct composite_context *ctx)
{
	struct sec_auth_conn_state *s = talloc_get_type(ctx->async.private_data,
							struct sec_auth_conn_state);

	s->ctx->status = dcerpc_pipe_auth_recv(ctx, s, &s->pipe2);
	if (!composite_is_ok(s->ctx)) return;
	
	composite_done(s->ctx);
}

/*
  Receive an authenticated pipe, created as a secondary connection
*/
_PUBLIC_ NTSTATUS dcerpc_secondary_auth_connection_recv(struct composite_context *c, 
					       TALLOC_CTX *mem_ctx,
					       struct dcerpc_pipe **p)
{
	NTSTATUS status = composite_wait(c);
	struct sec_auth_conn_state *s;

	s = talloc_get_type(c->private_data, struct sec_auth_conn_state);

	if (NT_STATUS_IS_OK(status)) {
		*p = talloc_steal(mem_ctx, s->pipe2);
	}

	talloc_free(c);
	return status;
}

_PUBLIC_ NTSTATUS dcerpc_secondary_auth_connection(struct dcerpc_pipe *p,
					const struct dcerpc_binding *binding,
					const struct ndr_interface_table *table,
					struct cli_credentials *credentials,
					struct loadparm_context *lp_ctx,
					TALLOC_CTX *mem_ctx,
					struct dcerpc_pipe **p2)
{
	struct composite_context *c;

	c = dcerpc_secondary_auth_connection_send(p, binding, table,
						  credentials, lp_ctx);
	return dcerpc_secondary_auth_connection_recv(c, mem_ctx, p2);
}
