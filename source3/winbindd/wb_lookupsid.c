/*
   Unix SMB/CIFS implementation.
   async lookupsid
   Copyright (C) Volker Lendecke 2009

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
#include "winbindd.h"
#include "librpc/gen_ndr/ndr_winbind_c.h"
#include "../libcli/security/security.h"

struct wb_lookupsid_state {
	struct tevent_context *ev;
	struct dom_sid sid;
	enum lsa_SidType type;
	const char *domname;
	const char *name;
};

static void wb_lookupsid_done(struct tevent_req *subreq);

struct tevent_req *wb_lookupsid_send(TALLOC_CTX *mem_ctx,
				     struct tevent_context *ev,
				     const struct dom_sid *sid)
{
	struct tevent_req *req, *subreq;
	struct wb_lookupsid_state *state;
	struct winbindd_domain *lookup_domain = NULL;
	struct dom_sid_buf buf;

	req = tevent_req_create(mem_ctx, &state, struct wb_lookupsid_state);
	if (req == NULL) {
		return NULL;
	}

	D_INFO("WB command lookupsid start.\n");
	sid_copy(&state->sid, sid);
	state->ev = ev;

	lookup_domain = find_lookup_domain_from_sid(sid);
	if (lookup_domain == NULL) {
		D_WARNING("Could not find domain for sid %s\n",
			  dom_sid_str_buf(sid, &buf));
		tevent_req_nterror(req, NT_STATUS_NONE_MAPPED);
		return tevent_req_post(req, ev);
	}

	D_DEBUG("Looking up SID %s in domain %s.\n",
		dom_sid_str_buf(&state->sid, &buf),
		lookup_domain->name);
	subreq = dcerpc_wbint_LookupSid_send(
		state, ev, dom_child_handle(lookup_domain),
		&state->sid, &state->type, &state->domname, &state->name);
	if (tevent_req_nomem(subreq, req)) {
		return tevent_req_post(req, ev);
	}
	tevent_req_set_callback(subreq, wb_lookupsid_done, req);
	return req;
}

static void wb_lookupsid_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct wb_lookupsid_state *state = tevent_req_data(
		req, struct wb_lookupsid_state);
	NTSTATUS status, result;

	status = dcerpc_wbint_LookupSid_recv(subreq, state, &result);
	TALLOC_FREE(subreq);
	if (any_nt_status_not_ok(status, result, &status)) {
		tevent_req_nterror(req, status);
		return;
	}
	tevent_req_done(req);
}

NTSTATUS wb_lookupsid_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
			   enum lsa_SidType *type, const char **domain,
			   const char **name)
{
	struct wb_lookupsid_state *state = tevent_req_data(
		req, struct wb_lookupsid_state);
	NTSTATUS status;
	struct dom_sid_buf buf;

	D_INFO("WB command lookupsid end.\n");
	if (tevent_req_is_nterror(req, &status)) {
		D_WARNING("Failed with %s.\n", nt_errstr(status));
		return status;
	}
	*type = state->type;
	*domain = talloc_move(mem_ctx, &state->domname);
	*name = talloc_move(mem_ctx, &state->name);
	D_INFO("SID %s has name '%s' with type '%d' in domain '%s'.\n",
	       dom_sid_str_buf(&state->sid, &buf),
	       *name,
	       *type,
	       *domain);
	return NT_STATUS_OK;
}
