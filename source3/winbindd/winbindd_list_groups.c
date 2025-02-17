/*
   Unix SMB/CIFS implementation.
   async implementation of WINBINDD_LIST_GROUPS
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
#include "util/debug.h"
#include "winbindd.h"
#include "librpc/gen_ndr/ndr_winbind_c.h"

struct winbindd_list_groups_domstate {
	struct tevent_req *subreq;
	struct winbindd_domain_ref domain;
	struct wbint_Principals groups;
};

struct winbindd_list_groups_state {
	uint32_t num_received;
	/* All domains */
	uint32_t num_domains;
	struct winbindd_list_groups_domstate *domains;
};

static void winbindd_list_groups_done(struct tevent_req *subreq);

struct tevent_req *winbindd_list_groups_send(TALLOC_CTX *mem_ctx,
					     struct tevent_context *ev,
					     struct winbindd_cli_state *cli,
					     struct winbindd_request *request)
{
	struct tevent_req *req;
	struct winbindd_list_groups_state *state;
	struct winbindd_domain *domain;
	uint32_t i;

	req = tevent_req_create(mem_ctx, &state,
				struct winbindd_list_groups_state);
	if (req == NULL) {
		return NULL;
	}

	D_NOTICE("[%s (%u)] Winbind external command LIST_GROUPS start.\n"
		 "WBFLAG_FROM_NSS is %s, winbind enum groups is %d.\n",
		 cli->client_name,
		 (unsigned int)cli->pid,
		 request->wb_flags & WBFLAG_FROM_NSS ? "Set" : "Unset",
		 lp_winbind_enum_groups());

	if (request->wb_flags & WBFLAG_FROM_NSS && !lp_winbind_enum_groups()) {
		tevent_req_done(req);
		return tevent_req_post(req, ev);
	}

	/* Ensure null termination */
	request->domain_name[sizeof(request->domain_name)-1]='\0';

	if (request->domain_name[0] != '\0') {
		state->num_domains = 1;
		D_DEBUG("List groups for domain %s.\n", request->domain_name);
	} else {
		state->num_domains = 0;
		for (domain = domain_list(); domain; domain = domain->next) {
			state->num_domains += 1;
		}
		D_DEBUG("List groups for %"PRIu32" domain(s).\n", state->num_domains);
	}

	state->domains = talloc_array(state,
				      struct winbindd_list_groups_domstate,
				      state->num_domains);
	if (tevent_req_nomem(state->domains, req)) {
		return tevent_req_post(req, ev);
	}

	if (request->domain_name[0] != '\0') {
		ZERO_STRUCT(state->domains[0].groups);

		domain = find_domain_from_name_noinit(
			request->domain_name);
		if (domain == NULL) {
			tevent_req_nterror(req, NT_STATUS_NO_SUCH_DOMAIN);
			return tevent_req_post(req, ev);
		}
		winbindd_domain_ref_set(&state->domains[0].domain,
					domain);
	} else {
		i = 0;
		for (domain = domain_list(); domain; domain = domain->next) {
			ZERO_STRUCT(state->domains[i].groups);

			winbindd_domain_ref_set(&state->domains[i].domain,
						domain);
			i++;
		}
	}

	for (i=0; i<state->num_domains; i++) {
		struct winbindd_list_groups_domstate *d = &state->domains[i];
		bool valid;

		/*
		 * We set the ref a few lines above, it must be valid!
		 */
		valid = winbindd_domain_ref_get(&d->domain, &domain);
		SMB_ASSERT(valid);

		d->subreq = dcerpc_wbint_QueryGroupList_send(
			state->domains, ev, dom_child_handle(domain),
			&d->groups);
		if (tevent_req_nomem(d->subreq, req)) {
			TALLOC_FREE(state->domains);
			return tevent_req_post(req, ev);
		}
		tevent_req_set_callback(d->subreq, winbindd_list_groups_done,
					req);
	}
	state->num_received = 0;
	return req;
}

static void winbindd_list_groups_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct winbindd_list_groups_state *state = tevent_req_data(
		req, struct winbindd_list_groups_state);
	NTSTATUS status, result;
	uint32_t i;

	status = dcerpc_wbint_QueryGroupList_recv(subreq, state->domains,
						  &result);

	for (i=0; i<state->num_domains; i++) {
		if (subreq == state->domains[i].subreq) {
			break;
		}
	}
	if (i < state->num_domains) {
		struct winbindd_list_groups_domstate *d = &state->domains[i];
		struct winbindd_domain *domain = NULL;
		bool valid;

		valid = winbindd_domain_ref_get(&d->domain, &domain);
		if (!valid) {
			/*
			 * winbindd_domain_ref_get() already generated
			 * a debug message for the stale domain!
			 */
			d->subreq = NULL;
			d->groups.num_principals = 0;
			goto skip;
		}

		D_DEBUG("Domain %s returned %"PRIu32" groups\n", domain->name,
			   d->groups.num_principals);

		d->subreq = NULL;

		if (!NT_STATUS_IS_OK(status) || !NT_STATUS_IS_OK(result)) {
			D_WARNING("list_groups for domain %s failed\n",
				   domain->name);
			d->groups.num_principals = 0;
		}
	}

skip:
	TALLOC_FREE(subreq);

	state->num_received += 1;

	if (state->num_received >= state->num_domains) {
		tevent_req_done(req);
	}
}

NTSTATUS winbindd_list_groups_recv(struct tevent_req *req,
				   struct winbindd_response *response)
{
	struct winbindd_list_groups_state *state = tevent_req_data(
		req, struct winbindd_list_groups_state);
	NTSTATUS status;
	char *result;
	uint32_t i, j, num_entries = 0;
	size_t len;

	D_NOTICE("Winbind external command LIST_GROUPS end.\n");
	if (tevent_req_is_nterror(req, &status)) {
		D_WARNING("Failed with %s.\n", nt_errstr(status));
		return status;
	}

	len = 0;
	response->data.num_entries = 0;
	for (i=0; i<state->num_domains; i++) {
		struct winbindd_list_groups_domstate *d = &state->domains[i];
		struct winbindd_domain *domain = NULL;
		bool valid;

		valid = winbindd_domain_ref_get(&d->domain, &domain);
		if (!valid) {
			/*
			 * winbindd_domain_ref_get() already generated
			 * a debug message for the stale domain!
			 */
			d->groups.num_principals = 0;
			continue;
		}

		for (j=0; j<d->groups.num_principals; j++) {
			const char *name;
			name = fill_domain_username_talloc(response, domain->name,
					     d->groups.principals[j].name,
					     True);
			if (name == NULL) {
				return NT_STATUS_NO_MEMORY;
			}
			len += strlen(name)+1;
		}
		response->data.num_entries += d->groups.num_principals;
	}

	result = talloc_array(response, char, len+1);
	if (result == 0) {
		return NT_STATUS_NO_MEMORY;
	}

	len = 0;
	for (i=0; i<state->num_domains; i++) {
		struct winbindd_list_groups_domstate *d = &state->domains[i];
		struct winbindd_domain *domain = NULL;
		bool valid;

		valid = winbindd_domain_ref_get(&d->domain, &domain);
		if (!valid) {
			/*
			 * winbindd_domain_ref_get() already generated
			 * a debug message for the stale domain!
			 */
			d->groups.num_principals = 0;
			continue;
		}

		for (j=0; j<d->groups.num_principals; j++) {
			const char *name;
			size_t this_len;
			name = fill_domain_username_talloc(response, domain->name,
					     d->groups.principals[j].name,
					     True);
			if (name == NULL) {
				return NT_STATUS_NO_MEMORY;
			}
			this_len = strlen(name);
			memcpy(result+len, name, this_len);
			len += this_len;
			result[len] = ',';
			len += 1;
			num_entries++;
		}
	}
	result[len-1] = '\0';

	response->data.num_entries = num_entries;
	response->extra_data.data = result;
	response->length += len;

	return NT_STATUS_OK;
}
