/*
   Unix SMB/CIFS implementation.
   async lookupsids
   Copyright (C) Volker Lendecke 2011

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
#include "lib/util_unixsids.h"
#include "librpc/gen_ndr/ndr_winbind_c.h"
#include "../libcli/security/security.h"
#include "passdb/machine_sid.h"
#include "lsa.h"

struct wb_lookupsids_domain {
	struct winbindd_domain_ref domain;

	/*
	 * Array of sids to be passed into wbint_LookupSids. Preallocated with
	 * num_sids.
	 */
	struct lsa_SidArray sids;

	/*
	 * Indexes into wb_lookupsids_state->sids and thus
	 * wb_lookupsids_state->res_names. Preallocated with num_sids.
	 */
	uint32_t *sid_indexes;
};

struct wb_translated_name {
	const char *domain_name;
	const char *name;
	enum lsa_SidType type;
};

static struct wb_lookupsids_domain *wb_lookupsids_get_domain(
	const struct dom_sid *sid, TALLOC_CTX *mem_ctx,
	struct wb_lookupsids_domain **domains, uint32_t num_sids);

struct wb_lookupsids_state {
	struct tevent_context *ev;

	/*
	 * SIDs passed in
	 */
	struct dom_sid *sids;
	uint32_t num_sids;

	/*
	 * The domains we're using for bulk lookup via wbint_LookupRids or
	 * wbint_LookupSids. We expect very few domains, so we do a
	 * talloc_realloc and rely on talloc_array_length.
	 */
	struct wb_lookupsids_domain *domains;
	uint32_t domains_done;

	/*
	 * These SIDs are looked up individually via
	 * wbint_LookupSid. Preallocated with num_sids.
	 */
	uint32_t *single_sids;
	uint32_t num_single_sids;
	uint32_t single_sids_done;

	/*
	 * Intermediate store for wbint_LookupRids to passdb. These are
	 * spliced into res_domains/res_names in wb_lookupsids_move_name.
	 */
	struct wbint_RidArray rids;
	const char *domain_name;
	struct wbint_Principals rid_names;

	/*
	 * Intermediate results for wbint_LookupSids. These results are
	 * spliced into res_domains/res_names in wb_lookupsids_move_name.
	 */
	struct lsa_RefDomainList tmp_domains;
	struct lsa_TransNameArray tmp_names;

	/*
	 * Results
	 */
	struct lsa_RefDomainList *res_domains;
	/*
	 * Indexed as "sids" in this structure
	 */
	struct lsa_TransNameArray *res_names;
};

static bool wb_lookupsids_next(struct tevent_req *req,
			       struct wb_lookupsids_state *state);
static void wb_lookupsids_single_done(struct tevent_req *subreq);
static void wb_lookupsids_lookuprids_done(struct tevent_req *subreq);
static void wb_lookupsids_done(struct tevent_req *subreq);

struct tevent_req *wb_lookupsids_send(TALLOC_CTX *mem_ctx,
				      struct tevent_context *ev,
				      struct dom_sid *sids,
				      uint32_t num_sids)
{
	struct tevent_req *req;
	struct wb_lookupsids_state *state;
	uint32_t i;

	req = tevent_req_create(mem_ctx, &state, struct wb_lookupsids_state);
	if (req == NULL) {
		return NULL;
	}

	D_INFO("WB command lookupsids start.\nLooking up %"PRIu32" SID(s)\n",
	       num_sids);
	if (CHECK_DEBUGLVL(DBGLVL_INFO)) {
		for (i = 0; i < num_sids; i++) {
			struct dom_sid_buf buf;
			D_INFO("%"PRIu32": %s\n",
			       i, dom_sid_str_buf(&sids[i], &buf));
		}
	}

	state->ev = ev;
	state->sids = sids;
	state->num_sids = num_sids;

	state->single_sids = talloc_zero_array(state, uint32_t, num_sids);
	if (tevent_req_nomem(state->single_sids, req)) {
		return tevent_req_post(req, ev);
	}

	state->res_domains = talloc_zero(state, struct lsa_RefDomainList);
	if (tevent_req_nomem(state->res_domains, req)) {
		return tevent_req_post(req, ev);
	}
	state->res_domains->domains = talloc_zero_array(
		state->res_domains, struct lsa_DomainInfo, num_sids);
	if (tevent_req_nomem(state->res_domains->domains, req)) {
		return tevent_req_post(req, ev);
	}

	state->res_names = talloc_zero(state, struct lsa_TransNameArray);
	if (tevent_req_nomem(state->res_names, req)) {
		return tevent_req_post(req, ev);
	}
	state->res_names->names = talloc_zero_array(
		state->res_names, struct lsa_TranslatedName, num_sids);
	if (tevent_req_nomem(state->res_names->names, req)) {
		return tevent_req_post(req, ev);
	}

	if (num_sids == 0) {
		tevent_req_done(req);
		return tevent_req_post(req, ev);
	}

	for (i=0; i<num_sids; i++) {
		struct wb_lookupsids_domain *d;

		d = wb_lookupsids_get_domain(&sids[i], state, &state->domains,
					     num_sids);
		if (d != NULL) {
			d->sids.sids[d->sids.num_sids].sid = &sids[i];
			d->sid_indexes[d->sids.num_sids] = i;
			d->sids.num_sids += 1;
		} else {
			state->single_sids[state->num_single_sids] = i;
			state->num_single_sids += 1;
		}
	}

	if (!wb_lookupsids_next(req, state)) {
		return tevent_req_post(req, ev);
	}
	return req;
}

static bool wb_lookupsids_next(struct tevent_req *req,
			       struct wb_lookupsids_state *state)
{
	struct tevent_req *subreq;

	if (state->domains_done < talloc_array_length(state->domains)) {
		struct wb_lookupsids_domain *d =
			&state->domains[state->domains_done];
		uint32_t i;
		struct winbindd_domain *d_domain = NULL;
		bool valid;

		valid = winbindd_domain_ref_get(&d->domain, &d_domain);
		if (!valid) {
			/*
			 * winbindd_domain_ref_get() already generated
			 * a debug message for the stale domain!
			 *
			 * Just ignore the domain
			 */
			state->domains_done += 1;
			return wb_lookupsids_next(req, state);
		}

		if (d_domain->internal) {
			/*
			 * This is only our local SAM,
			 * see wb_lookupsids_bulk() and
			 * wb_lookupsids_get_domain().
			 */
			state->rids.num_rids = d->sids.num_sids;
			state->rids.rids = talloc_array(state, uint32_t,
							state->rids.num_rids);
			if (tevent_req_nomem(state->rids.rids, req)) {
				return false;
			}
			for (i=0; i<state->rids.num_rids; i++) {
				sid_peek_rid(d->sids.sids[i].sid,
					     &state->rids.rids[i]);
			}
			subreq = dcerpc_wbint_LookupRids_send(
				state, state->ev, dom_child_handle(d_domain),
				&d_domain->sid, &state->rids, &state->domain_name,
				&state->rid_names);
			if (tevent_req_nomem(subreq, req)) {
				return false;
			}
			tevent_req_set_callback(
				subreq, wb_lookupsids_lookuprids_done, req);
			return true;
		}

		subreq = dcerpc_wbint_LookupSids_send(
			state, state->ev, dom_child_handle(d_domain),
			&d->sids, &state->tmp_domains,	&state->tmp_names);
		if (tevent_req_nomem(subreq, req)) {
			return false;
		}
		tevent_req_set_callback(subreq, wb_lookupsids_done, req);
		return true;
	}

	if (state->single_sids_done < state->num_single_sids) {
		uint32_t sid_idx;
		const struct dom_sid *sid;

		sid_idx = state->single_sids[state->single_sids_done];
		sid = &state->sids[sid_idx];

		subreq = wb_lookupsid_send(state, state->ev, sid);
		if (tevent_req_nomem(subreq, req)) {
			return false;
		}
		tevent_req_set_callback(subreq, wb_lookupsids_single_done,
					req);
		return true;
	}

	tevent_req_done(req);
	return false;
}

/*
 * Decide whether to do bulk lookupsids. We have optimizations for
 * passdb via lookuprids and to remote DCs via lookupsids.
 */

static bool wb_lookupsids_bulk(const struct dom_sid *sid)
{
	struct dom_sid_buf sidbuf;

	if (sid->num_auths != 5) {
		/*
		 * Only do "S-1-5-21-x-y-z-rid" domains via bulk
		 * lookup
		 */
		DBG_DEBUG("No bulk setup for SID %s with %"PRIi8" subauths\n",
			  dom_sid_str_buf(sid, &sidbuf),
			  sid->num_auths);
		return false;
	}

	if (sid_check_is_in_our_sam(sid)) {
		/*
		 * Passdb lookup via lookuprids
		 */
		DBG_DEBUG("%s is in our domain\n",
			  dom_sid_str_buf(sid, &sidbuf));
		return true;
	}

	if (IS_DC) {
		/*
		 * Bulk lookups to trusted DCs
		 */
		return (find_domain_from_sid_noinit(sid) != NULL);
	}

	if (lp_server_role() != ROLE_DOMAIN_MEMBER) {
		/*
		 * Don't do bulk lookups as standalone, the only bulk
		 * lookup left is for domain members.
		 */
		return false;
	}

	if (sid_check_is_in_unix_groups(sid) ||
	    sid_check_is_unix_groups(sid) ||
	    sid_check_is_in_unix_users(sid) ||
	    sid_check_is_unix_users(sid) ||
	    sid_check_is_in_builtin(sid) ||
	    sid_check_is_builtin(sid) ||
	    sid_check_is_wellknown_domain(sid, NULL) ||
	    sid_check_is_in_wellknown_domain(sid))
	{
		/*
		 * These are locally done piece by piece anyway, no
		 * need for bulk optimizations.
		 */
		return false;
	}

	/*
	 * All other SIDs are sent to the DC we're connected to as
	 * member via a single lsa_lookupsids call.
	 */
	return true;
}

static struct wb_lookupsids_domain *wb_lookupsids_get_domain(
	const struct dom_sid *sid, TALLOC_CTX *mem_ctx,
	struct wb_lookupsids_domain **pdomains, uint32_t num_sids)
{
	struct wb_lookupsids_domain *domains, *domain;
	struct winbindd_domain *wb_domain;
	uint32_t i, num_domains;

	if (!wb_lookupsids_bulk(sid)) {
		D_DEBUG("wb_lookupsids_bulk() is FALSE\n");
		return NULL;
	}
	D_DEBUG("wb_lookupsids_bulk() is TRUE\n");

	domains = *pdomains;
	num_domains = talloc_array_length(domains);

	wb_domain = find_lookup_domain_from_sid(sid);
	if (wb_domain == NULL) {
		return NULL;
	}

	D_DEBUG("Searching %"PRIu32" domain(s) for domain '%s'\n",
		num_domains, wb_domain->name);
	for (i=0; i<num_domains; i++) {
		struct wb_lookupsids_domain *d = &domains[i];
		struct winbindd_domain *d_domain = NULL;
		bool valid;

		valid = winbindd_domain_ref_get(&d->domain, &d_domain);
		if (!valid) {
			/*
			 * winbindd_domain_ref_get() already generated
			 * a debug message for the stale domain!
			 *
			 * Just ignore the domain
			 */
			continue;
		}

		if (d_domain != wb_domain) {
			continue;
		}

		if (!d_domain->internal) {
			/*
			 * If it's not our local sam,
			 * we can re-use the domain without
			 * checking the sid.
			 *
			 * Note the wb_lookupsids_bulk() above
			 * already caught special SIDs,
			 * e.g. the unix and builtin domains.
			 */
			return d;
		}

		if (dom_sid_compare_domain(sid, &d_domain->sid) == 0) {
			/*
			 * If it's out local sam we can also use it.
			 */
			return d;
		}

		/*
		 * I'm not sure if this can be triggered,
		 * as wb_lookupsids_bulk() should also catch this,
		 * but we need to make sure that we don't use
		 * wbint_LookupRids() without a SID match.
		 */
		return NULL;
	}

	domains = talloc_realloc(
		mem_ctx, domains, struct wb_lookupsids_domain, num_domains+1);
	if (domains == NULL) {
		return NULL;
	}
	*pdomains = domains;

	domain = &domains[num_domains];
	winbindd_domain_ref_set(&domain->domain, wb_domain);

	domain->sids.sids = talloc_zero_array(domains, struct lsa_SidPtr, num_sids);
	if (domains->sids.sids == NULL) {
		goto fail;
	}
	domain->sids.num_sids = 0;

	domain->sid_indexes = talloc_zero_array(domains, uint32_t, num_sids);
	if (domain->sid_indexes == NULL) {
		TALLOC_FREE(domain->sids.sids);
		goto fail;
	}
	return domain;

fail:
	/*
	 * Realloc to the state it was in before
	 */
	*pdomains = talloc_realloc(
		mem_ctx, domains, struct wb_lookupsids_domain, num_domains);
	return NULL;
}

static bool wb_lookupsids_find_dom_idx(struct lsa_DomainInfo *domain,
				       struct lsa_RefDomainList *list,
				       uint32_t *idx)
{
	uint32_t i;
	struct lsa_DomainInfo *new_domain;

	for (i=0; i<list->count; i++) {
		if (dom_sid_equal(domain->sid, list->domains[i].sid)) {
			*idx = i;
			return true;
		}
	}

	new_domain = &list->domains[list->count];

	new_domain->name.string = talloc_strdup(
		list->domains, domain->name.string);
	if (new_domain->name.string == NULL) {
		return false;
	}

	new_domain->sid = dom_sid_dup(list->domains, domain->sid);
	if (new_domain->sid == NULL) {
		return false;
	}

	*idx = list->count;
	list->count += 1;
	return true;
}

static bool wb_lookupsids_move_name(struct lsa_RefDomainList *src_domains,
				    struct lsa_TranslatedName *src_name,
				    struct lsa_RefDomainList *dst_domains,
				    struct lsa_TransNameArray *dst_names,
				    uint32_t dst_name_index)
{
	struct lsa_TranslatedName *dst_name;
	struct lsa_DomainInfo *src_domain;
	uint32_t src_domain_index;
	uint32_t dst_domain_index = UINT32_MAX;
	bool ok;

	src_domain_index = src_name->sid_index;
	if ((src_domain_index != UINT32_MAX) && (src_domains != NULL)) {
		if (src_domain_index >= src_domains->count) {
			return false;
		}
		src_domain = &src_domains->domains[src_domain_index];

		ok = wb_lookupsids_find_dom_idx(src_domain,
						dst_domains,
						&dst_domain_index);
		if (!ok) {
			return false;
		}
	}

	dst_name = &dst_names->names[dst_name_index];

	dst_name->sid_type = src_name->sid_type;
	dst_name->name.string = talloc_move(dst_names->names,
					    &src_name->name.string);
	dst_name->sid_index = dst_domain_index;
	dst_names->count += 1;

	return true;
}

static void wb_lookupsids_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct wb_lookupsids_state *state = tevent_req_data(
		req, struct wb_lookupsids_state);
	struct wb_lookupsids_domain *d;
	uint32_t i;

	NTSTATUS status, result;

	status = dcerpc_wbint_LookupSids_recv(subreq, state, &result);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}
	if (NT_STATUS_LOOKUP_ERR(result)) {
		tevent_req_nterror(req, result);
		return;
	}

	/*
	 * Look at the individual states in the translated names.
	 */

	d = &state->domains[state->domains_done];

	for (i=0; i<state->tmp_names.count; i++) {
		uint32_t res_sid_index = d->sid_indexes[i];

		if (!wb_lookupsids_move_name(
			    &state->tmp_domains, &state->tmp_names.names[i],
			    state->res_domains, state->res_names,
			    res_sid_index)) {
			tevent_req_oom(req);
			return;
		}
	}
	state->domains_done += 1;
	wb_lookupsids_next(req, state);
}

static void wb_lookupsids_single_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct wb_lookupsids_state *state = tevent_req_data(
		req, struct wb_lookupsids_state);
	const char *domain_name = NULL;
	const char *name = NULL;
	enum lsa_SidType type = SID_NAME_UNKNOWN;
	uint32_t res_sid_index;
	uint32_t src_rid;

	struct dom_sid src_domain_sid;
	struct lsa_DomainInfo src_domain;
	struct lsa_RefDomainList src_domains;
	struct lsa_RefDomainList *psrc_domains = NULL;
	struct lsa_TranslatedName src_name;

	uint32_t domain_idx = UINT32_MAX;
	NTSTATUS status;
	bool ok;

	status = wb_lookupsid_recv(subreq, talloc_tos(), &type,
				   &domain_name, &name);
	TALLOC_FREE(subreq);
	if (NT_STATUS_LOOKUP_ERR(status)) {
		tevent_req_nterror(req, status);
		return;
	}

	res_sid_index = state->single_sids[state->single_sids_done];

	if ((domain_name != NULL) && (domain_name[0] != '\0')) {
		/*
		 * Build structs with the domain name for
		 * wb_lookupsids_move_name(). If we didn't get a name, we will
		 * pass NULL and UINT32_MAX.
		 */

		sid_copy(&src_domain_sid, &state->sids[res_sid_index]);
		if (type != SID_NAME_DOMAIN) {
			sid_split_rid(&src_domain_sid, &src_rid);
		}

		src_domain.name.string = domain_name;
		src_domain.sid = &src_domain_sid;

		src_domains.count = 1;
		src_domains.domains = &src_domain;
		psrc_domains = &src_domains;

		domain_idx = 0;
	}

	src_name.sid_type = type;
	src_name.name.string = name;
	src_name.sid_index = domain_idx;

	ok = wb_lookupsids_move_name(psrc_domains,
				     &src_name,
				     state->res_domains,
				     state->res_names,
				     res_sid_index);
	if (!ok) {
		tevent_req_oom(req);
		return;
	}
	state->single_sids_done += 1;
	wb_lookupsids_next(req, state);
}

static void wb_lookupsids_lookuprids_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(
		subreq, struct tevent_req);
	struct wb_lookupsids_state *state = tevent_req_data(
		req, struct wb_lookupsids_state);
	struct dom_sid src_domain_sid;
	struct lsa_DomainInfo src_domain;
	struct lsa_RefDomainList src_domains;
	NTSTATUS status, result;
	struct wb_lookupsids_domain *d;
	uint32_t i;

	status = dcerpc_wbint_LookupRids_recv(subreq, state, &result);
	TALLOC_FREE(subreq);
	if (tevent_req_nterror(req, status)) {
		return;
	}
	if (NT_STATUS_LOOKUP_ERR(result)) {
		tevent_req_nterror(req, result);
		return;
	}

	/*
	 * Look at the individual states in the translated names.
	 */

	d = &state->domains[state->domains_done];

	sid_copy(&src_domain_sid, get_global_sam_sid());
	src_domain.name.string = get_global_sam_name();
	src_domain.sid = &src_domain_sid;
	src_domains.count = 1;
	src_domains.domains = &src_domain;

	for (i=0; i<state->rid_names.num_principals; i++) {
		struct lsa_TranslatedName src_name;
		uint32_t res_sid_index;

		/*
		 * Fake up structs for wb_lookupsids_move_name
		 */
		res_sid_index = d->sid_indexes[i];

		src_name.sid_type = state->rid_names.principals[i].type;
		src_name.name.string = state->rid_names.principals[i].name;
		src_name.sid_index = 0;

		if (!wb_lookupsids_move_name(
			    &src_domains, &src_name,
			    state->res_domains, state->res_names,
			    res_sid_index)) {
			tevent_req_oom(req);
			return;
		}
	}

	state->domains_done += 1;
	wb_lookupsids_next(req, state);
}

NTSTATUS wb_lookupsids_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
			    struct lsa_RefDomainList **domains,
			    struct lsa_TransNameArray **names)
{
	struct wb_lookupsids_state *state = tevent_req_data(
		req, struct wb_lookupsids_state);
	NTSTATUS status;

	D_INFO("WB command lookupsids end.\n");
	if (tevent_req_is_nterror(req, &status)) {
		D_WARNING("Failed with %s.\n", nt_errstr(status));
		return status;
	}

	/*
	 * The returned names need to match the given sids,
	 * if not we have a bug in the code!
	 */
	if (state->res_names->count != state->num_sids) {
		D_WARNING("Got %"PRIu32" returned name(s), but expected %"PRIu32"!\n",
			  state->res_names->count, state->num_sids);
		return NT_STATUS_INTERNAL_ERROR;
	}

	/*
	 * Not strictly needed, but it might make debugging in the callers
	 * easier in future, if the talloc_array_length() returns the
	 * expected result...
	 */
	state->res_domains->domains = talloc_realloc(state->res_domains,
						     state->res_domains->domains,
						     struct lsa_DomainInfo,
						     state->res_domains->count);

	*domains = talloc_move(mem_ctx, &state->res_domains);
	*names = talloc_move(mem_ctx, &state->res_names);
	D_INFO("Returning %"PRIu32" domain(s) and %"PRIu32" name(s).\n",
	       (*domains)->count,
	       (*names)->count);
	return NT_STATUS_OK;
}
