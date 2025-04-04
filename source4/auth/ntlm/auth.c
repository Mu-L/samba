/*
   Unix SMB/CIFS implementation.
   Password and authentication handling
   Copyright (C) Andrew Bartlett         2001-2002
   Copyright (C) Stefan Metzmacher       2005

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

#include "includes.h"
#include <tevent.h>
#include "../lib/util/tevent_ntstatus.h"
#include "../lib/util/dlinklist.h"
#include "auth/auth.h"
#include "auth/ntlm/auth_proto.h"
#include "param/param.h"
#include "dsdb/samdb/samdb.h"
#include "libcli/wbclient/wbclient.h"
#include "lib/util/samba_modules.h"
#include "auth/credentials/credentials.h"
#include "system/kerberos.h"
#include "auth/kerberos/kerberos.h"
#include "auth/kerberos/kerberos_util.h"
#include "libds/common/roles.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_AUTH

static NTSTATUS auth_generate_session_info_wrapper(struct auth4_context *auth_context,
						   TALLOC_CTX *mem_ctx,
                                                  void *server_returned_info,
						   const char *original_user_name,
						   uint32_t session_info_flags,
						   struct auth_session_info **session_info);

/***************************************************************************
 Set a fixed challenge
***************************************************************************/
_PUBLIC_ NTSTATUS auth_context_set_challenge(struct auth4_context *auth_ctx, const uint8_t chal[8], const char *set_by)
{
	auth_ctx->challenge.set_by = talloc_strdup(auth_ctx, set_by);
	NT_STATUS_HAVE_NO_MEMORY(auth_ctx->challenge.set_by);

	auth_ctx->challenge.data = data_blob_talloc(auth_ctx, chal, 8);
	NT_STATUS_HAVE_NO_MEMORY(auth_ctx->challenge.data.data);

	return NT_STATUS_OK;
}

/****************************************************************************
 Try to get a challenge out of the various authentication modules.
 Returns a const char of length 8 bytes.
****************************************************************************/
_PUBLIC_ NTSTATUS auth_get_challenge(struct auth4_context *auth_ctx, uint8_t chal[8])
{

	if (auth_ctx->challenge.data.length == 8) {
		DEBUG(5, ("auth_get_challenge: returning previous challenge by module %s (normal)\n",
			  auth_ctx->challenge.set_by));
		memcpy(chal, auth_ctx->challenge.data.data, 8);
		return NT_STATUS_OK;
	}

	if (!auth_ctx->challenge.set_by) {
		generate_random_buffer(chal, 8);

		auth_ctx->challenge.data		= data_blob_talloc(auth_ctx, chal, 8);
		NT_STATUS_HAVE_NO_MEMORY(auth_ctx->challenge.data.data);
		auth_ctx->challenge.set_by		= "random";
	}

	DEBUG(10,("auth_get_challenge: challenge set by %s\n",
		 auth_ctx->challenge.set_by));

	return NT_STATUS_OK;
}

/**
 * Check a user's Plaintext, LM or NTLM password.
 * (sync version)
 *
 * Check a user's password, as given in the user_info struct and return various
 * interesting details in the user_info_dc struct.
 *
 * The return value takes precedence over the contents of the user_info_dc
 * struct.  When the return is other than NT_STATUS_OK the contents
 * of that structure is undefined.
 *
 * @param auth_ctx Supplies the challenges and some other data.
 *                  Must be created with auth_context_create(), and the challenges should be
 *                  filled in, either at creation or by calling the challenge generation
 *                  function auth_get_challenge().
 *
 * @param user_info Contains the user supplied components, including the passwords.
 *
 * @param mem_ctx The parent memory context for the user_info_dc structure
 *
 * @param user_info_dc If successful, contains information about the authentication,
 *                    including a SAM_ACCOUNT struct describing the user.
 *
 * @return An NTSTATUS with NT_STATUS_OK or an appropriate error.
 *
 **/

_PUBLIC_ NTSTATUS auth_check_password(struct auth4_context *auth_ctx,
			     TALLOC_CTX *mem_ctx,
			     const struct auth_usersupplied_info *user_info,
			     struct auth_user_info_dc **user_info_dc,
			     uint8_t *pauthoritative)
{
	struct tevent_req *subreq;
	struct tevent_context *ev;
	bool ok;
	NTSTATUS status;

	/*TODO: create a new event context here! */
	ev = auth_ctx->event_ctx;

	/*
	 * We are authoritative by default
	 */
	*pauthoritative = 1;

	subreq = auth_check_password_send(mem_ctx,
					  ev,
					  auth_ctx,
					  user_info);
	if (subreq == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	ok = tevent_req_poll(subreq, ev);
	if (!ok) {
		return NT_STATUS_INTERNAL_ERROR;
	}

	status = auth_check_password_recv(subreq, mem_ctx,
					  user_info_dc, pauthoritative);
	TALLOC_FREE(subreq);

	return status;
}

struct auth_check_password_state {
	struct tevent_context *ev;
	struct auth4_context *auth_ctx;
	const struct auth_usersupplied_info *user_info;
	struct auth_user_info_dc *user_info_dc;
	struct auth_method_context *method;
	const struct authn_audit_info *client_audit_info;
	const struct authn_audit_info *server_audit_info;
	uint8_t authoritative;
};

static void auth_check_password_next(struct tevent_req *req);

/**
 * Check a user's Plaintext, LM or NTLM password.
 * async send hook
 *
 * Check a user's password, as given in the user_info struct and return various
 * interesting details in the user_info_dc struct.
 *
 * The return value takes precedence over the contents of the user_info_dc
 * struct.  When the return is other than NT_STATUS_OK the contents
 * of that structure is undefined.
 *
 * @param mem_ctx The memory context the request should operate on
 *
 * @param ev The tevent context the request should operate on
 *
 * @param auth_ctx Supplies the challenges and some other data.  Must
 *                 be created with make_auth_context(), and the
 *                 challenges should be filled in, either at creation
 *                 or by calling the challenge generation function
 *                 auth_get_challenge().
 *
 * @param user_info Contains the user supplied components, including the passwords.
 *
 * @return The request handle or NULL on no memory error.
 *
 **/

_PUBLIC_ struct tevent_req *auth_check_password_send(TALLOC_CTX *mem_ctx,
				struct tevent_context *ev,
				struct auth4_context *auth_ctx,
				const struct auth_usersupplied_info *user_info)
{
	struct tevent_req *req;
	struct auth_check_password_state *state;
	/* if all the modules say 'not for me' this is reasonable */
	NTSTATUS nt_status;
	uint8_t chal[8];

	DEBUG(3,("auth_check_password_send: "
		 "Checking password for unmapped user [%s]\\[%s]@[%s]\n",
		 user_info->client.domain_name, user_info->client.account_name,
		 user_info->workstation_name));

	req = tevent_req_create(mem_ctx, &state,
				struct auth_check_password_state);
	if (req == NULL) {
		return NULL;
	}

	/*
	 * We are authoritative by default.
	 */
	state->ev		= ev;
	state->auth_ctx		= auth_ctx;
	state->user_info	= user_info;
	state->authoritative	= 1;

	if (user_info->mapped.account_name == NULL) {
		struct auth_usersupplied_info *user_info_tmp;

		/*
		 * We don't really do any mapping here.
		 *
		 * It's up to the backends to do mappings
		 * for their authentication.
		 */
		user_info_tmp = talloc_zero(state, struct auth_usersupplied_info);
		if (tevent_req_nomem(user_info_tmp, req)) {
			return tevent_req_post(req, ev);
		}

		/*
		 * The lifetime of user_info is longer than
		 * user_info_tmp, so we don't need to copy the
		 * strings.
		 */
		*user_info_tmp = *user_info;
		user_info_tmp->mapped.domain_name = user_info->client.domain_name;
		user_info_tmp->mapped.account_name = user_info->client.account_name;

		user_info = user_info_tmp;
		state->user_info = user_info_tmp;
	}

	DEBUGADD(3,("auth_check_password_send: "
		    "user is: [%s]\\[%s]@[%s]\n",
		    user_info->mapped.domain_name,
		    user_info->mapped.account_name,
		    user_info->workstation_name));

	nt_status = auth_get_challenge(auth_ctx, chal);
	if (tevent_req_nterror(req, nt_status)) {
		DEBUG(0,("auth_check_password_send: "
			 "Invalid challenge (length %u) stored for "
			 "this auth context set_by %s - cannot continue: %s\n",
			(unsigned)auth_ctx->challenge.data.length,
			auth_ctx->challenge.set_by,
			nt_errstr(nt_status)));
		return tevent_req_post(req, ev);
	}

	if (auth_ctx->challenge.set_by) {
		DEBUG(10,("auth_check_password_send: "
			  "auth_context challenge created by %s\n",
			  auth_ctx->challenge.set_by));
	}

	DEBUG(10, ("auth_check_password_send: challenge is: \n"));
	dump_data(5, auth_ctx->challenge.data.data,
		  auth_ctx->challenge.data.length);

	state->method = state->auth_ctx->methods;
	auth_check_password_next(req);
	if (!tevent_req_is_in_progress(req)) {
		return tevent_req_post(req, ev);
	}

	return req;
}

static void auth_check_password_done(struct tevent_req *subreq);

static void auth_check_password_next(struct tevent_req *req)
{
	struct auth_check_password_state *state =
		tevent_req_data(req, struct auth_check_password_state);
	struct tevent_req *subreq = NULL;
	NTSTATUS status;

	if (state->method == NULL) {
		state->authoritative = 0;
		tevent_req_nterror(req, NT_STATUS_NO_SUCH_USER);
		return;
	}

	/* check if the module wants to check the password */
	status = state->method->ops->want_check(state->method, state,
						state->user_info);
	if (NT_STATUS_EQUAL(status, NT_STATUS_NOT_IMPLEMENTED)) {
		DEBUG(11,("auth_check_password_send: "
			  "%s doesn't want to check\n",
			  state->method->ops->name));
		state->method = state->method->next;
		auth_check_password_next(req);
		return;
	}

	if (tevent_req_nterror(req, status)) {
		return;
	}

	subreq = state->method->ops->check_password_send(
		state, state->ev, state->method, state->user_info);
	if (tevent_req_nomem(subreq, req)) {
		return;
	}
	tevent_req_set_callback(subreq, auth_check_password_done, req);
}

static void auth_check_password_done(struct tevent_req *subreq)
{
	struct tevent_req *req =
		tevent_req_callback_data(subreq,
		struct tevent_req);
	struct auth_check_password_state *state =
		tevent_req_data(req,
		struct auth_check_password_state);
	bool authoritative = true;
	NTSTATUS status;

	status = state->method->ops->check_password_recv(subreq, state,
							 &state->user_info_dc,
							 &state->client_audit_info,
							 &state->server_audit_info,
							 &authoritative);
	TALLOC_FREE(subreq);
	if (!authoritative ||
	    NT_STATUS_EQUAL(status, NT_STATUS_NOT_IMPLEMENTED)) {
		DEBUG(11,("auth_check_password_send: "
			  "%s passes to the next method\n",
			  state->method->ops->name));
		state->method = state->method->next;
		auth_check_password_next(req);
		return;
	}

	/* the backend has handled the request */

	if (tevent_req_nterror(req, status)) {
		return;
	}

	tevent_req_done(req);
}

/**
 * Check a user's Plaintext, LM or NTLM password.
 * async receive function
 *
 * The return value takes precedence over the contents of the user_info_dc
 * struct.  When the return is other than NT_STATUS_OK the contents
 * of that structure is undefined.
 *
 *
 * @param req The async request state
 *
 * @param mem_ctx The parent memory context for the user_info_dc structure
 *
 * @param user_info_dc If successful, contains information about the authentication,
 *                    including a SAM_ACCOUNT struct describing the user.
 *
 * @return An NTSTATUS with NT_STATUS_OK or an appropriate error.
 *
 **/

_PUBLIC_ NTSTATUS auth_check_password_recv(struct tevent_req *req,
				  TALLOC_CTX *mem_ctx,
				  struct auth_user_info_dc **user_info_dc,
				  uint8_t *pauthoritative)
{
	struct auth_check_password_state *state =
		tevent_req_data(req, struct auth_check_password_state);
	NTSTATUS status = NT_STATUS_OK;

	*pauthoritative = state->authoritative;

	if (tevent_req_is_nterror(req, &status)) {
		/*
		 * Please try not to change this string, it is probably in use
		 * in audit logging tools
		 */
		DEBUG(2,("auth_check_password_recv: "
			 "%s authentication for user [%s\\%s] "
			 "FAILED with error %s, authoritative=%u\n",
			 (state->method ? state->method->ops->name : "NO_METHOD"),
			 state->user_info->mapped.domain_name,
			 state->user_info->mapped.account_name,
			 nt_errstr(status), state->authoritative));

		log_authentication_event(state->auth_ctx->msg_ctx,
					 state->auth_ctx->lp_ctx,
					 &state->auth_ctx->start_time,
					 state->user_info, status,
					 NULL, NULL, NULL,
					 state->client_audit_info,
					 state->server_audit_info);
		tevent_req_received(req);
		return status;
	}

	DEBUG(5,("auth_check_password_recv: "
		 "%s authentication for user [%s\\%s] succeeded\n",
		 state->method->ops->name,
		 state->user_info_dc->info->domain_name,
		 state->user_info_dc->info->account_name));

	log_authentication_event(state->auth_ctx->msg_ctx,
				 state->auth_ctx->lp_ctx,
				 &state->auth_ctx->start_time,
				 state->user_info, status,
				 state->user_info_dc->info->domain_name,
				 state->user_info_dc->info->account_name,
				 &state->user_info_dc->sids[PRIMARY_USER_SID_INDEX].sid,
				 state->client_audit_info,
				 state->server_audit_info);

	/*
	 * Release our handle to state->user_info_dc.
	 * state->{client,server}_audit_info, if non-NULL, becomes the new
	 * parent.
	*/
	*user_info_dc = talloc_reparent(state, mem_ctx, state->user_info_dc);
	state->user_info_dc = NULL;

	tevent_req_received(req);
	return NT_STATUS_OK;
}

struct auth_check_password_wrapper_state {
	uint8_t authoritative;
	struct auth_user_info_dc *user_info_dc;
};

static void auth_check_password_wrapper_done(struct tevent_req *subreq);

static struct tevent_req *auth_check_password_wrapper_send(TALLOC_CTX *mem_ctx,
					struct tevent_context *ev,
					struct auth4_context *auth_ctx,
					const struct auth_usersupplied_info *user_info)
{
	struct tevent_req *req = NULL;
	struct auth_check_password_wrapper *state = NULL;
	struct tevent_req *subreq = NULL;

	req = tevent_req_create(mem_ctx, &state,
				struct auth_check_password_wrapper_state);
	if (req == NULL) {
		return NULL;
	}

	subreq = auth_check_password_send(state, ev, auth_ctx, user_info);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq,
				auth_check_password_wrapper_done,
				req);

	return req;
}

static void auth_check_password_wrapper_done(struct tevent_req *subreq)
{
	struct tevent_req *req =
		tevent_req_callback_data(subreq,
		struct tevent_req);
	struct auth_check_password_wrapper_state *state =
		tevent_req_data(req,
		struct auth_check_password_wrapper_state);
	NTSTATUS status;

	status = auth_check_password_recv(subreq, state,
					  &state->user_info_dc,
					  &state->authoritative);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}

	tevent_req_done(req);
}

static NTSTATUS auth_check_password_wrapper_recv(struct tevent_req *req,
					TALLOC_CTX *mem_ctx,
					uint8_t *pauthoritative,
					void **server_returned_info,
					DATA_BLOB *user_session_key,
					DATA_BLOB *lm_session_key)
{
	struct auth_check_password_wrapper_state *state =
		tevent_req_data(req,
		struct auth_check_password_wrapper_state);
	struct auth_user_info_dc *user_info_dc = state->user_info_dc;
	NTSTATUS status = NT_STATUS_OK;

	*pauthoritative = state->authoritative;

	if (tevent_req_is_nterror(req, &status)) {
		tevent_req_received(req);
		return status;
	}

	talloc_steal(mem_ctx, user_info_dc);
	*server_returned_info = user_info_dc;

	if (user_session_key) {
		DEBUG(10, ("Got NT session key of length %u\n",
			   (unsigned)user_info_dc->user_session_key.length));
		*user_session_key = user_info_dc->user_session_key;
		talloc_steal(mem_ctx, user_session_key->data);
		user_info_dc->user_session_key = data_blob_null;
	}

	if (lm_session_key) {
		DEBUG(10, ("Got LM session key of length %u\n",
			   (unsigned)user_info_dc->lm_session_key.length));
		*lm_session_key = user_info_dc->lm_session_key;
		talloc_steal(mem_ctx, lm_session_key->data);
		user_info_dc->lm_session_key = data_blob_null;
	}

	tevent_req_received(req);
	return NT_STATUS_OK;
}

 /* Wrapper because we don't want to expose all callers to needing to
  * know that session_info is generated from the main ldb, and because
  * we need to break a dependency loop between the DCE/RPC layer and the
  * generation of unix tokens via IRPC */
static NTSTATUS auth_generate_session_info_wrapper(struct auth4_context *auth_context,
						   TALLOC_CTX *mem_ctx,
						   void *server_returned_info,
						   const char *original_user_name,
                                                  uint32_t session_info_flags,
                                                  struct auth_session_info **session_info)
{
	NTSTATUS status;
	struct auth_user_info_dc *user_info_dc = talloc_get_type_abort(server_returned_info, struct auth_user_info_dc);

	if (!(user_info_dc->info->user_flags & NETLOGON_GUEST)) {
		session_info_flags |= AUTH_SESSION_INFO_AUTHENTICATED;
	}

	status = auth_generate_session_info(mem_ctx,
					    auth_context->lp_ctx,
					    auth_context->sam_ctx,
					    user_info_dc,
					    session_info_flags,
					    session_info);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if ((session_info_flags & AUTH_SESSION_INFO_UNIX_TOKEN)
	    && NT_STATUS_IS_OK(status)) {
		status = auth_session_info_fill_unix(auth_context->lp_ctx,
						     original_user_name,
						     *session_info);
		if (!NT_STATUS_IS_OK(status)) {
			TALLOC_FREE(*session_info);
		}
	}
	return status;
}

/* Wrapper because we don't want to expose all callers to needing to
 * know anything about the PAC or auth subsystem internal structures
 * before we output a struct auth session_info */
static NTSTATUS auth_generate_session_info_pac(struct auth4_context *auth_ctx,
					       TALLOC_CTX *mem_ctx,
					       struct smb_krb5_context *smb_krb5_context,
					       DATA_BLOB *pac_blob,
					       const char *principal_name,
					       const struct tsocket_address *remote_address,
					       uint32_t session_info_flags,
					       struct auth_session_info **session_info)
{
	NTSTATUS status;
	struct auth_user_info_dc *user_info_dc;
	TALLOC_CTX *tmp_ctx;

	if (!pac_blob) {
		/*
		 * This should already have been caught at the main
		 * gensec layer, but better check twice
		 */
		return NT_STATUS_INTERNAL_ERROR;
	}

	tmp_ctx = talloc_named(mem_ctx, 0, "gensec_gssapi_session_info context");
	NT_STATUS_HAVE_NO_MEMORY(tmp_ctx);

	/*
	 * FIXME: To correctly create the security token, we also need to get the
	 * claims info, device info, and device claims info from the PAC. For now,
	 * we support claims only in the KDC.
	 */
	status = kerberos_pac_blob_to_user_info_dc(tmp_ctx,
						   *pac_blob,
						   smb_krb5_context->krb5_context,
						   &user_info_dc, NULL, NULL);
	if (!NT_STATUS_IS_OK(status)) {
		talloc_free(tmp_ctx);
		return status;
	}

	if (!(user_info_dc->info->user_flags & NETLOGON_GUEST)) {
		session_info_flags |= AUTH_SESSION_INFO_AUTHENTICATED;
	}

	status = auth_generate_session_info_wrapper(auth_ctx, mem_ctx,
						    user_info_dc,
						    user_info_dc->info->account_name,
						    session_info_flags, session_info);
	talloc_free(tmp_ctx);
	return status;
}

/***************************************************************************
 Make a auth_info struct for the auth subsystem
 - Allow the caller to specify the methods to use, including optionally the SAM to use
***************************************************************************/
_PUBLIC_ NTSTATUS auth_context_create_methods(TALLOC_CTX *mem_ctx, const char * const *methods,
					      struct tevent_context *ev,
					      struct imessaging_context *msg,
					      struct loadparm_context *lp_ctx,
					      struct ldb_context *sam_ctx,
					      struct auth4_context **auth_ctx)
{
	int i;
	struct auth4_context *ctx;

	auth4_init();

	if (!ev) {
		DEBUG(0,("auth_context_create: called with out event context\n"));
		return NT_STATUS_INTERNAL_ERROR;
	}

	ctx = talloc_zero(mem_ctx, struct auth4_context);
	NT_STATUS_HAVE_NO_MEMORY(ctx);
	ctx->challenge.data		= data_blob(NULL, 0);
	ctx->methods			= NULL;
	ctx->event_ctx			= ev;
	ctx->msg_ctx			= msg;
	ctx->lp_ctx			= lp_ctx;
	ctx->start_time                 = timeval_current();

	if (sam_ctx) {
		ctx->sam_ctx = sam_ctx;
	} else {
		ctx->sam_ctx = samdb_connect(ctx,
					     ctx->event_ctx,
					     ctx->lp_ctx,
					     system_session(ctx->lp_ctx),
					     NULL,
					     0);
	}

	for (i=0; methods && methods[i] ; i++) {
		struct auth_method_context *method;

		method = talloc(ctx, struct auth_method_context);
		NT_STATUS_HAVE_NO_MEMORY(method);

		method->ops = auth_backend_byname(methods[i]);
		if (!method->ops) {
			DEBUG(1,("auth_context_create: failed to find method=%s\n",
				methods[i]));
			return NT_STATUS_INTERNAL_ERROR;
		}
		method->auth_ctx	= ctx;
		method->depth		= i;
		DLIST_ADD_END(ctx->methods, method);
	}

	ctx->check_ntlm_password_send = auth_check_password_wrapper_send;
	ctx->check_ntlm_password_recv = auth_check_password_wrapper_recv;
	ctx->get_ntlm_challenge = auth_get_challenge;
	ctx->set_ntlm_challenge = auth_context_set_challenge;
	ctx->generate_session_info = auth_generate_session_info_wrapper;
	ctx->generate_session_info_pac = auth_generate_session_info_pac;

	*auth_ctx = ctx;

	return NT_STATUS_OK;
}

const char **auth_methods_from_lp(TALLOC_CTX *mem_ctx, struct loadparm_context *lp_ctx)
{
	char **auth_methods = NULL;

	switch (lpcfg_server_role(lp_ctx)) {
	case ROLE_STANDALONE:
		auth_methods = str_list_make(mem_ctx, "anonymous sam_ignoredomain", NULL);
		break;
	case ROLE_DOMAIN_MEMBER:
	case ROLE_DOMAIN_BDC:
	case ROLE_DOMAIN_PDC:
	case ROLE_ACTIVE_DIRECTORY_DC:
	case ROLE_IPA_DC:
		auth_methods = str_list_make(mem_ctx, "anonymous sam winbind sam_ignoredomain", NULL);
		break;
	}
	return discard_const_p(const char *, auth_methods);
}

/***************************************************************************
 Make a auth_info struct for the auth subsystem
 - Uses default auth_methods, depending on server role and smb.conf settings
***************************************************************************/
_PUBLIC_ NTSTATUS auth_context_create(TALLOC_CTX *mem_ctx,
			     struct tevent_context *ev,
			     struct imessaging_context *msg,
			     struct loadparm_context *lp_ctx,
			     struct auth4_context **auth_ctx)
{
	NTSTATUS status;
	const char **auth_methods;
	TALLOC_CTX *tmp_ctx = talloc_new(mem_ctx);
	if (!tmp_ctx) {
		return NT_STATUS_NO_MEMORY;
	}

	auth_methods = auth_methods_from_lp(tmp_ctx, lp_ctx);
	if (!auth_methods) {
		return NT_STATUS_INVALID_PARAMETER;
	}
	status = auth_context_create_methods(mem_ctx, auth_methods, ev, msg, lp_ctx, NULL, auth_ctx);
	talloc_free(tmp_ctx);
	return status;
}

_PUBLIC_ NTSTATUS auth_context_create_for_netlogon(TALLOC_CTX *mem_ctx,
						   struct tevent_context *ev,
						   struct imessaging_context *msg,
						   struct loadparm_context *lp_ctx,
						   struct auth4_context **auth_ctx)
{
	NTSTATUS status;
	char **_auth_methods = NULL;
	const char **auth_methods = NULL;

	/*
	 * Here we only allow 'sam winbind' instead of
	 * the 'anonymous sam winbind sam_ignoredomain'
	 * we typically use for authentication from clients.
	 */
	_auth_methods = str_list_make(mem_ctx, "sam winbind", NULL);
	if (_auth_methods == NULL) {
		return NT_STATUS_NO_MEMORY;
	}
	auth_methods = discard_const_p(const char *, _auth_methods);

	status = auth_context_create_methods(mem_ctx, auth_methods, ev, msg,
					     lp_ctx, NULL, auth_ctx);
	talloc_free(_auth_methods);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	(*auth_ctx)->for_netlogon = true;
	return NT_STATUS_OK;
}

/* the list of currently registered AUTH backends */
static struct auth_backend {
	const struct auth_operations *ops;
} *backends = NULL;
static int num_backends;

/*
  register a AUTH backend.

  The 'name' can be later used by other backends to find the operations
  structure for this backend.
*/
_PUBLIC_ NTSTATUS auth_register(TALLOC_CTX *mem_ctx,
			const struct auth_operations *ops)
{
	struct auth_operations *new_ops;

	if (auth_backend_byname(ops->name) != NULL) {
		/* its already registered! */
		DEBUG(0,("AUTH backend '%s' already registered\n",
			 ops->name));
		return NT_STATUS_OBJECT_NAME_COLLISION;
	}

	backends = talloc_realloc(mem_ctx, backends,
				  struct auth_backend, num_backends+1);
	NT_STATUS_HAVE_NO_MEMORY(backends);

	new_ops = (struct auth_operations *)talloc_memdup(backends, ops, sizeof(*ops));
	NT_STATUS_HAVE_NO_MEMORY(new_ops);
	new_ops->name = talloc_strdup(new_ops, ops->name);
	NT_STATUS_HAVE_NO_MEMORY(new_ops->name);

	backends[num_backends].ops = new_ops;

	num_backends++;

	DEBUG(3,("AUTH backend '%s' registered\n",
		 ops->name));

	return NT_STATUS_OK;
}

/*
  return the operations structure for a named backend of the specified type
*/
const struct auth_operations *auth_backend_byname(const char *name)
{
	int i;

	for (i=0;i<num_backends;i++) {
		if (strcmp(backends[i].ops->name, name) == 0) {
			return backends[i].ops;
		}
	}

	return NULL;
}

/*
  return the AUTH interface version, and the size of some critical types
  This can be used by backends to either detect compilation errors, or provide
  multiple implementations for different smbd compilation options in one module
*/
const struct auth_critical_sizes *auth_interface_version(void)
{
	static const struct auth_critical_sizes critical_sizes = {
		AUTH4_INTERFACE_VERSION,
		sizeof(struct auth_operations),
		sizeof(struct auth_method_context),
		sizeof(struct auth4_context),
		sizeof(struct auth_usersupplied_info),
		sizeof(struct auth_user_info_dc)
	};

	return &critical_sizes;
}

_PUBLIC_ NTSTATUS auth4_init(void)
{
	static bool initialized = false;
#define _MODULE_PROTO(init) extern NTSTATUS init(TALLOC_CTX *);
	STATIC_auth4_MODULES_PROTO;
	init_module_fn static_init[] = { STATIC_auth4_MODULES };

	if (initialized) return NT_STATUS_OK;
	initialized = true;

	run_init_functions(NULL, static_init);

	return NT_STATUS_OK;
}
