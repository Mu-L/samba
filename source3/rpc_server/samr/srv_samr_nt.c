/*
 *  Unix SMB/CIFS implementation.
 *  RPC Pipe client / server routines
 *  Copyright (C) Andrew Tridgell                   1992-1997,
 *  Copyright (C) Luke Kenneth Casson Leighton      1996-1997,
 *  Copyright (C) Paul Ashton                       1997,
 *  Copyright (C) Marc Jacobsen			    1999,
 *  Copyright (C) Jeremy Allison                    2001-2008,
 *  Copyright (C) Jean François Micouleau           1998-2001,
 *  Copyright (C) Jim McDonough <jmcd@us.ibm.com>   2002,
 *  Copyright (C) Gerald (Jerry) Carter             2003-2004,
 *  Copyright (C) Simo Sorce                        2003.
 *  Copyright (C) Volker Lendecke		    2005.
 *  Copyright (C) Guenther Deschner		    2008.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This is the implementation of the SAMR code.
 */

#include "includes.h"
#include "system/passwd.h"
#include "../libcli/auth/libcli_auth.h"
#include "ntdomain.h"
#include "librpc/rpc/dcesrv_core.h"
#include "../librpc/gen_ndr/ndr_samr.h"
#include "../librpc/gen_ndr/ndr_samr_scompat.h"
#include "rpc_server/samr/srv_samr_util.h"
#include "secrets.h"
#include "rpc_client/init_lsa.h"
#include "../libcli/security/security.h"
#include "passdb.h"
#include "auth.h"
#include "rpc_server/srv_access_check.h"
#include "../lib/tsocket/tsocket.h"
#include "lib/util/base64.h"
#include "param/param.h"
#include "librpc/rpc/dcerpc_helper.h"
#include "librpc/rpc/dcerpc_samr.h"

#include "lib/crypto/gnutls_helpers.h"
#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>
#include "lib/global_contexts.h"
#include "nsswitch/winbind_client.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_RPC_SRV

#define SAMR_USR_RIGHTS_WRITE_PW \
		( READ_CONTROL_ACCESS		| \
		  SAMR_USER_ACCESS_CHANGE_PASSWORD	| \
		  SAMR_USER_ACCESS_SET_LOC_COM)
#define SAMR_USR_RIGHTS_CANT_WRITE_PW \
		( READ_CONTROL_ACCESS | SAMR_USER_ACCESS_SET_LOC_COM )

#define DISP_INFO_CACHE_TIMEOUT 10

#define MAX_SAM_ENTRIES_W2K 0x400 /* 1024 */
#define MAX_SAM_ENTRIES_W95 50

enum samr_handle {
	SAMR_HANDLE_CONNECT,
	SAMR_HANDLE_DOMAIN,
	SAMR_HANDLE_USER,
	SAMR_HANDLE_GROUP,
	SAMR_HANDLE_ALIAS
};

struct samr_info {
	uint32_t access_granted;
	struct dom_sid sid;
	struct disp_info *disp_info;
};

typedef struct disp_info {
	struct dom_sid sid; /* identify which domain this is. */
	struct pdb_search *users; /* querydispinfo 1 and 4 */
	struct pdb_search *machines; /* querydispinfo 2 */
	struct pdb_search *groups; /* querydispinfo 3 and 5, enumgroups */
	struct pdb_search *aliases; /* enumaliases */

	uint32_t enum_acb_mask;
	struct pdb_search *enum_users; /* enumusers with a mask */

	struct tevent_timer *cache_timeout_event; /* cache idle timeout
						  * handler. */
} DISP_INFO;

static const struct generic_mapping sam_generic_mapping = {
	GENERIC_RIGHTS_SAM_READ,
	GENERIC_RIGHTS_SAM_WRITE,
	GENERIC_RIGHTS_SAM_EXECUTE,
	GENERIC_RIGHTS_SAM_ALL_ACCESS};
static const struct generic_mapping dom_generic_mapping = {
	GENERIC_RIGHTS_DOMAIN_READ,
	GENERIC_RIGHTS_DOMAIN_WRITE,
	GENERIC_RIGHTS_DOMAIN_EXECUTE,
	GENERIC_RIGHTS_DOMAIN_ALL_ACCESS};
static const struct generic_mapping usr_generic_mapping = {
	GENERIC_RIGHTS_USER_READ,
	GENERIC_RIGHTS_USER_WRITE,
	GENERIC_RIGHTS_USER_EXECUTE,
	GENERIC_RIGHTS_USER_ALL_ACCESS};
static const struct generic_mapping usr_nopwchange_generic_mapping = {
	GENERIC_RIGHTS_USER_READ,
	GENERIC_RIGHTS_USER_WRITE,
	GENERIC_RIGHTS_USER_EXECUTE & ~SAMR_USER_ACCESS_CHANGE_PASSWORD,
	GENERIC_RIGHTS_USER_ALL_ACCESS};
static const struct generic_mapping grp_generic_mapping = {
	GENERIC_RIGHTS_GROUP_READ,
	GENERIC_RIGHTS_GROUP_WRITE,
	GENERIC_RIGHTS_GROUP_EXECUTE,
	GENERIC_RIGHTS_GROUP_ALL_ACCESS};
static const struct generic_mapping ali_generic_mapping = {
	GENERIC_RIGHTS_ALIAS_READ,
	GENERIC_RIGHTS_ALIAS_WRITE,
	GENERIC_RIGHTS_ALIAS_EXECUTE,
	GENERIC_RIGHTS_ALIAS_ALL_ACCESS};

/*******************************************************************
*******************************************************************/
static NTSTATUS create_samr_policy_handle(TALLOC_CTX *mem_ctx,
					  struct pipes_struct *p,
					  enum samr_handle type,
					  uint32_t acc_granted,
					  struct dom_sid *sid,
					  struct disp_info *disp_info,
					  struct policy_handle *handle)
{
	struct samr_info *info = NULL;
	bool ok;

	ZERO_STRUCTP(handle);

	info = talloc_zero(mem_ctx, struct samr_info);
	if (info == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	info->access_granted = acc_granted;

	if (sid != NULL) {
		sid_copy(&info->sid, sid);
	}

	if (disp_info != NULL) {
		info->disp_info = disp_info;
	}

	ok = create_policy_hnd(p, handle, type, info);
	if (!ok) {
		talloc_free(info);
		ZERO_STRUCTP(handle);
		return NT_STATUS_NO_MEMORY;
	}

	return NT_STATUS_OK;
}

static NTSTATUS samr_handle_access_check(uint32_t access_granted,
					 uint32_t access_required,
					 uint32_t *paccess_granted)
{
	if ((access_required & access_granted) != access_required) {
		if (root_mode()) {
			DBG_INFO("ACCESS should be DENIED (granted: "
				 "%#010x; required: %#010x) but overwritten "
				 "by euid == 0\n", access_granted,
				 access_required);
			goto okay;
		}
		DBG_NOTICE("ACCESS DENIED (granted: %#010x; required: "
			   "%#010x)\n", access_granted, access_required);
		return NT_STATUS_ACCESS_DENIED;
	}

okay:
	if (paccess_granted != NULL) {
		*paccess_granted = access_granted;
	}
	return NT_STATUS_OK;
}

static void *samr_policy_handle_find(struct pipes_struct *p,
				     const struct policy_handle *handle,
				     uint8_t handle_type,
				     uint32_t access_required,
				     uint32_t *access_granted,
				     NTSTATUS *pstatus)
{
	struct samr_info *info = NULL;
	NTSTATUS status;

	info = find_policy_by_hnd(p,
				  handle,
				  handle_type,
				  struct samr_info,
				  &status);
	if (!NT_STATUS_IS_OK(status)) {
		*pstatus = NT_STATUS_INVALID_HANDLE;
		return NULL;
	}

	status = samr_handle_access_check(info->access_granted,
					  access_required,
					  access_granted);
	if (!NT_STATUS_IS_OK(status)) {
		*pstatus = status;
		return NULL;
	}

	*pstatus = NT_STATUS_OK;
	return info;
}

static NTSTATUS make_samr_object_sd( TALLOC_CTX *ctx, struct security_descriptor **psd, size_t *sd_size,
                                     const struct generic_mapping *map,
				     struct dom_sid *sid, uint32_t sid_access )
{
	struct dom_sid domadmin_sid;
	struct security_ace ace[5] = {0};		/* at most 5 entries */
	size_t i = 0;

	struct security_acl *psa = NULL;

	/* basic access for Everyone */

	init_sec_ace(&ace[i++], &global_sid_World, SEC_ACE_TYPE_ACCESS_ALLOWED,
			map->generic_execute | map->generic_read, 0);

	/* add Full Access 'BUILTIN\Administrators' and 'BUILTIN\Account Operators */

	init_sec_ace(&ace[i++], &global_sid_Builtin_Administrators,
			SEC_ACE_TYPE_ACCESS_ALLOWED, map->generic_all, 0);
	init_sec_ace(&ace[i++], &global_sid_Builtin_Account_Operators,
			SEC_ACE_TYPE_ACCESS_ALLOWED, map->generic_all, 0);

	/* Add Full Access for Domain Admins if we are a DC */

	if ( IS_DC ) {
		sid_compose(&domadmin_sid, get_global_sam_sid(),
			    DOMAIN_RID_ADMINS);
		init_sec_ace(&ace[i++], &domadmin_sid,
			SEC_ACE_TYPE_ACCESS_ALLOWED, map->generic_all, 0);
	}

	/* if we have a sid, give it some special access */

	if ( sid ) {
		init_sec_ace(&ace[i++], sid, SEC_ACE_TYPE_ACCESS_ALLOWED, sid_access, 0);
	}

	/* create the security descriptor */

	if ((psa = make_sec_acl(ctx, NT4_ACL_REVISION, i, ace)) == NULL)
		return NT_STATUS_NO_MEMORY;

	if ((*psd = make_sec_desc(ctx, SECURITY_DESCRIPTOR_REVISION_1,
				  SEC_DESC_SELF_RELATIVE, NULL, NULL, NULL,
				  psa, sd_size)) == NULL)
		return NT_STATUS_NO_MEMORY;

	return NT_STATUS_OK;
}

/*******************************************************************
 Fetch or create a dispinfo struct.
********************************************************************/

static DISP_INFO *get_samr_dispinfo_by_sid(const struct dom_sid *psid)
{
	/*
	 * We do a static cache for DISP_INFO's here. Explanation can be found
	 * in Jeremy's checkin message to r11793:
	 *
	 * Fix the SAMR cache so it works across completely insane
	 * client behaviour (ie.:
	 * open pipe/open SAMR handle/enumerate 0 - 1024
	 * close SAMR handle, close pipe.
	 * open pipe/open SAMR handle/enumerate 1024 - 2048...
	 * close SAMR handle, close pipe.
	 * And on ad-nausium. Amazing.... probably object-oriented
	 * client side programming in action yet again.
	 * This change should *massively* improve performance when
	 * enumerating users from an LDAP database.
	 * Jeremy.
	 *
	 * "Our" and the builtin domain are the only ones where we ever
	 * enumerate stuff, so just cache 2 entries.
	 */

	static struct disp_info *builtin_dispinfo;
	static struct disp_info *domain_dispinfo;

	/* There are two cases to consider here:
	   1) The SID is a domain SID and we look for an equality match, or
	   2) This is an account SID and so we return the DISP_INFO* for our
	      domain */

	if (psid == NULL) {
		return NULL;
	}

	if (sid_check_is_builtin(psid) || sid_check_is_in_builtin(psid)) {
		/*
		 * Necessary only once, but it does not really hurt.
		 */
		if (builtin_dispinfo == NULL) {
			builtin_dispinfo = talloc_zero(NULL, struct disp_info);
			if (builtin_dispinfo == NULL) {
				return NULL;
			}
		}
		sid_copy(&builtin_dispinfo->sid, &global_sid_Builtin);

		return builtin_dispinfo;
	}

	if (sid_check_is_our_sam(psid) || sid_check_is_in_our_sam(psid)) {
		/*
		 * Necessary only once, but it does not really hurt.
		 */
		if (domain_dispinfo == NULL) {
			domain_dispinfo = talloc_zero(NULL, struct disp_info);
			if (domain_dispinfo == NULL) {
				return NULL;
			}
		}
		sid_copy(&domain_dispinfo->sid, get_global_sam_sid());

		return domain_dispinfo;
	}

	return NULL;
}

/*******************************************************************
 Function to free the per SID data.
 ********************************************************************/

static void free_samr_cache(DISP_INFO *disp_info)
{
	struct dom_sid_buf buf;

	DEBUG(10, ("free_samr_cache: deleting cache for SID %s\n",
		   dom_sid_str_buf(&disp_info->sid, &buf)));

	/* We need to become root here because the paged search might have to
	 * tell the LDAP server we're not interested in the rest anymore. */

	become_root();

	TALLOC_FREE(disp_info->users);
	TALLOC_FREE(disp_info->machines);
	TALLOC_FREE(disp_info->groups);
	TALLOC_FREE(disp_info->aliases);
	TALLOC_FREE(disp_info->enum_users);

	unbecome_root();
}

/*******************************************************************
 Idle event handler. Throw away the disp info cache.
 ********************************************************************/

static void disp_info_cache_idle_timeout_handler(struct tevent_context *ev_ctx,
						 struct tevent_timer *te,
						 struct timeval now,
						 void *private_data)
{
	DISP_INFO *disp_info = (DISP_INFO *)private_data;

	TALLOC_FREE(disp_info->cache_timeout_event);

	DEBUG(10, ("disp_info_cache_idle_timeout_handler: caching timed "
		   "out\n"));
	free_samr_cache(disp_info);
}

/*******************************************************************
 Setup cache removal idle event handler.
 ********************************************************************/

static void set_disp_info_cache_timeout(DISP_INFO *disp_info, time_t secs_fromnow)
{
	struct dom_sid_buf buf;

	/* Remove any pending timeout and update. */

	TALLOC_FREE(disp_info->cache_timeout_event);

	DEBUG(10,("set_disp_info_cache_timeout: caching enumeration for "
		  "SID %s for %u seconds\n",
		  dom_sid_str_buf(&disp_info->sid, &buf),
		  (unsigned int)secs_fromnow ));

	disp_info->cache_timeout_event = tevent_add_timer(
		global_event_context(), NULL,
		timeval_current_ofs(secs_fromnow, 0),
		disp_info_cache_idle_timeout_handler, (void *)disp_info);
}

/*******************************************************************
 Force flush any cache. We do this on any samr_set_xxx call.
 We must also remove the timeout handler.
 ********************************************************************/

static void force_flush_samr_cache(const struct dom_sid *sid)
{
	struct disp_info *disp_info = get_samr_dispinfo_by_sid(sid);

	if ((disp_info == NULL) || (disp_info->cache_timeout_event == NULL)) {
		return;
	}

	DEBUG(10,("force_flush_samr_cache: clearing idle event\n"));
	TALLOC_FREE(disp_info->cache_timeout_event);
	free_samr_cache(disp_info);
}

/*******************************************************************
 Ensure password info is never given out. Paranioa... JRA.
 ********************************************************************/

static void samr_clear_sam_passwd(struct samu *sam_pass)
{

	if (!sam_pass)
		return;

	/* These now zero out the old password */

	pdb_set_lanman_passwd(sam_pass, NULL, PDB_DEFAULT);
	pdb_set_nt_passwd(sam_pass, NULL, PDB_DEFAULT);
}

static uint32_t count_sam_users(struct disp_info *info, uint32_t acct_flags)
{
	struct samr_displayentry *entry;

	if (sid_check_is_builtin(&info->sid)) {
		/* No users in builtin. */
		return 0;
	}

	if (info->users == NULL) {
		info->users = pdb_search_users(info, acct_flags);
		if (info->users == NULL) {
			return 0;
		}
	}
	/* Fetch the last possible entry, thus trigger an enumeration */
	pdb_search_entries(info->users, 0xffffffff, 1, &entry);

	/* Ensure we cache this enumeration. */
	set_disp_info_cache_timeout(info, DISP_INFO_CACHE_TIMEOUT);

	return info->users->num_entries;
}

static uint32_t count_sam_groups(struct disp_info *info)
{
	struct samr_displayentry *entry;

	if (sid_check_is_builtin(&info->sid)) {
		/* No groups in builtin. */
		return 0;
	}

	if (info->groups == NULL) {
		info->groups = pdb_search_groups(info);
		if (info->groups == NULL) {
			return 0;
		}
	}
	/* Fetch the last possible entry, thus trigger an enumeration */
	pdb_search_entries(info->groups, 0xffffffff, 1, &entry);

	/* Ensure we cache this enumeration. */
	set_disp_info_cache_timeout(info, DISP_INFO_CACHE_TIMEOUT);

	return info->groups->num_entries;
}

static uint32_t count_sam_aliases(struct disp_info *info)
{
	struct samr_displayentry *entry;

	if (info->aliases == NULL) {
		info->aliases = pdb_search_aliases(info, &info->sid);
		if (info->aliases == NULL) {
			return 0;
		}
	}
	/* Fetch the last possible entry, thus trigger an enumeration */
	pdb_search_entries(info->aliases, 0xffffffff, 1, &entry);

	/* Ensure we cache this enumeration. */
	set_disp_info_cache_timeout(info, DISP_INFO_CACHE_TIMEOUT);

	return info->aliases->num_entries;
}

/*******************************************************************
 _samr_Close
 ********************************************************************/

NTSTATUS _samr_Close(struct pipes_struct *p, struct samr_Close *r)
{
	if (!close_policy_hnd(p, r->in.handle)) {
		return NT_STATUS_INVALID_HANDLE;
	}

	ZERO_STRUCTP(r->out.handle);

	return NT_STATUS_OK;
}

/*******************************************************************
 _samr_OpenDomain
 ********************************************************************/

NTSTATUS _samr_OpenDomain(struct pipes_struct *p,
			  struct samr_OpenDomain *r)
{
	struct dcesrv_call_state *dce_call = p->dce_call;
	struct auth_session_info *session_info =
		dcesrv_call_session_info(dce_call);
	struct security_descriptor *psd = NULL;
	uint32_t    acc_granted;
	uint32_t    des_access = r->in.access_mask;
	NTSTATUS  status;
	size_t    sd_size;
	uint32_t extra_access = SAMR_DOMAIN_ACCESS_CREATE_USER;
	struct disp_info *disp_info = NULL;

	/* find the connection policy handle. */
	(void)samr_policy_handle_find(p,
				      r->in.connect_handle,
				      SAMR_HANDLE_CONNECT,
				      0,
				      NULL,
				      &status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/*check if access can be granted as requested by client. */
	map_max_allowed_access(session_info->security_token,
			       session_info->unix_token,
			       &des_access);

	make_samr_object_sd( p->mem_ctx, &psd, &sd_size, &dom_generic_mapping, NULL, 0 );
	se_map_generic( &des_access, &dom_generic_mapping );

	/*
	 * Users with SeAddUser get the ability to manipulate groups
	 * and aliases.
	 */
	if (security_token_has_privilege(
		    session_info->security_token, SEC_PRIV_ADD_USERS)) {
		extra_access |= (SAMR_DOMAIN_ACCESS_CREATE_GROUP |
				SAMR_DOMAIN_ACCESS_ENUM_ACCOUNTS |
				SAMR_DOMAIN_ACCESS_OPEN_ACCOUNT |
				SAMR_DOMAIN_ACCESS_LOOKUP_ALIAS |
				SAMR_DOMAIN_ACCESS_CREATE_ALIAS);
	}

	/*
	 * Users with SeMachineAccount or SeAddUser get additional
	 * SAMR_DOMAIN_ACCESS_CREATE_USER access.
	 */

	status = access_check_object( psd, session_info->security_token,
				      SEC_PRIV_MACHINE_ACCOUNT, SEC_PRIV_ADD_USERS,
				      extra_access, des_access,
				      &acc_granted, "_samr_OpenDomain" );

	if ( !NT_STATUS_IS_OK(status) )
		return status;

	if (!sid_check_is_our_sam(r->in.sid) &&
	    !sid_check_is_builtin(r->in.sid)) {
		return NT_STATUS_NO_SUCH_DOMAIN;
	}

	disp_info = get_samr_dispinfo_by_sid(r->in.sid);

	status = create_samr_policy_handle(p->mem_ctx,
					   p,
					   SAMR_HANDLE_DOMAIN,
					   acc_granted,
					   r->in.sid,
					   disp_info,
					   r->out.domain_handle);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(5,("_samr_OpenDomain: %d\n", __LINE__));

	return NT_STATUS_OK;
}

/*******************************************************************
 _samr_GetUserPwInfo
 ********************************************************************/

NTSTATUS _samr_GetUserPwInfo(struct pipes_struct *p,
			     struct samr_GetUserPwInfo *r)
{
	const struct loadparm_substitution *lp_sub =
		loadparm_s3_global_substitution();
	struct samr_info *uinfo;
	enum lsa_SidType sid_type;
	uint32_t min_password_length = 0;
	uint32_t password_properties = 0;
	bool ret = false;
	NTSTATUS status;

	DEBUG(5,("_samr_GetUserPwInfo: %d\n", __LINE__));

	uinfo = samr_policy_handle_find(p, r->in.user_handle,
					SAMR_HANDLE_USER,
					SAMR_USER_ACCESS_GET_ATTRIBUTES,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (!sid_check_is_in_our_sam(&uinfo->sid)) {
		return NT_STATUS_OBJECT_TYPE_MISMATCH;
	}

	become_root();
	ret = lookup_sid(p->mem_ctx, &uinfo->sid, NULL, NULL, &sid_type);
	unbecome_root();
	if (ret == false) {
		return NT_STATUS_NO_SUCH_USER;
	}

	switch (sid_type) {
		case SID_NAME_USER:
			become_root();
			pdb_get_account_policy(PDB_POLICY_MIN_PASSWORD_LEN,
					       &min_password_length);
			pdb_get_account_policy(PDB_POLICY_USER_MUST_LOGON_TO_CHG_PASS,
					       &password_properties);
			unbecome_root();

			if (lp_check_password_script(talloc_tos(), lp_sub)
			    && *lp_check_password_script(talloc_tos(), lp_sub)) {
				password_properties |= DOMAIN_PASSWORD_COMPLEX;
			}

			break;
		default:
			break;
	}

	r->out.info->min_password_length = min_password_length;
	r->out.info->password_properties = password_properties;

	DEBUG(5,("_samr_GetUserPwInfo: %d\n", __LINE__));

	return NT_STATUS_OK;
}

/*******************************************************************
 _samr_SetSecurity
 ********************************************************************/

NTSTATUS _samr_SetSecurity(struct pipes_struct *p,
			   struct samr_SetSecurity *r)
{
	struct samr_info *uinfo;
	uint32_t i;
	struct security_acl *dacl;
	bool ret;
	struct samu *sampass=NULL;
	NTSTATUS status;

	uinfo = samr_policy_handle_find(p,
					r->in.handle,
					SAMR_HANDLE_USER,
					SAMR_USER_ACCESS_SET_ATTRIBUTES,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (!(sampass = samu_new( p->mem_ctx))) {
		DEBUG(0,("No memory!\n"));
		return NT_STATUS_NO_MEMORY;
	}

	/* get the user record */
	become_root();
	ret = pdb_getsampwsid(sampass, &uinfo->sid);
	unbecome_root();

	if (!ret) {
		struct dom_sid_buf buf;
		DEBUG(4, ("User %s not found\n",
			  dom_sid_str_buf(&uinfo->sid, &buf)));
		TALLOC_FREE(sampass);
		return NT_STATUS_INVALID_HANDLE;
	}

	dacl = r->in.sdbuf->sd->dacl;
	for (i=0; i < dacl->num_aces; i++) {
		if (dom_sid_equal(&uinfo->sid, &dacl->aces[i].trustee)) {
			ret = pdb_set_pass_can_change(sampass,
				(dacl->aces[i].access_mask &
				 SAMR_USER_ACCESS_CHANGE_PASSWORD) ?
						      True: False);
			break;
		}
	}

	if (!ret) {
		TALLOC_FREE(sampass);
		return NT_STATUS_ACCESS_DENIED;
	}

	become_root();
	status = pdb_update_sam_account(sampass);
	unbecome_root();

	TALLOC_FREE(sampass);

	return status;
}

/*******************************************************************
  build correct perms based on policies and password times for _samr_query_sec_obj
*******************************************************************/
static bool check_change_pw_access(TALLOC_CTX *mem_ctx, struct dom_sid *user_sid)
{
	struct samu *sampass=NULL;
	bool ret;

	if ( !(sampass = samu_new( mem_ctx )) ) {
		DEBUG(0,("No memory!\n"));
		return False;
	}

	become_root();
	ret = pdb_getsampwsid(sampass, user_sid);
	unbecome_root();

	if (ret == False) {
		struct dom_sid_buf buf;
		DEBUG(4,("User %s not found\n",
			 dom_sid_str_buf(user_sid, &buf)));
		TALLOC_FREE(sampass);
		return False;
	}

	DEBUG(3,("User:[%s]\n",  pdb_get_username(sampass) ));

	if (pdb_get_pass_can_change(sampass)) {
		TALLOC_FREE(sampass);
		return True;
	}
	TALLOC_FREE(sampass);
	return False;
}


/*******************************************************************
 _samr_QuerySecurity
 ********************************************************************/

NTSTATUS _samr_QuerySecurity(struct pipes_struct *p,
			     struct samr_QuerySecurity *r)
{
	struct samr_info *info;
	NTSTATUS status;
	struct security_descriptor * psd = NULL;
	size_t sd_size = 0;
	struct dom_sid_buf buf;

	info = samr_policy_handle_find(p,
				       r->in.handle,
				       SAMR_HANDLE_CONNECT,
				       SEC_STD_READ_CONTROL,
				       NULL,
				       &status);
	if (NT_STATUS_IS_OK(status)) {
		DEBUG(5,("_samr_QuerySecurity: querying security on SAM\n"));
		status = make_samr_object_sd(p->mem_ctx, &psd, &sd_size,
					     &sam_generic_mapping, NULL, 0);
		goto done;
	}

	info = samr_policy_handle_find(p,
				       r->in.handle,
				       SAMR_HANDLE_DOMAIN,
				       SEC_STD_READ_CONTROL,
				       NULL,
				       &status);
	if (NT_STATUS_IS_OK(status)) {
		DEBUG(5,("_samr_QuerySecurity: querying security on Domain "
			 "with SID: %s\n",
			 dom_sid_str_buf(&info->sid, &buf)));
		/*
		 * TODO: Builtin probably needs a different SD with restricted
		 * write access
		 */
		status = make_samr_object_sd(p->mem_ctx, &psd, &sd_size,
					     &dom_generic_mapping, NULL, 0);
		goto done;
	}

	info = samr_policy_handle_find(p,
				       r->in.handle,
				       SAMR_HANDLE_USER,
				       SEC_STD_READ_CONTROL,
				       NULL,
				       &status);
	if (NT_STATUS_IS_OK(status)) {
		DEBUG(10,("_samr_QuerySecurity: querying security on user "
			  "Object with SID: %s\n",
			  dom_sid_str_buf(&info->sid, &buf)));
		if (check_change_pw_access(p->mem_ctx, &info->sid)) {
			status = make_samr_object_sd(
				p->mem_ctx, &psd, &sd_size,
				&usr_generic_mapping,
				&info->sid, SAMR_USR_RIGHTS_WRITE_PW);
		} else {
			status = make_samr_object_sd(
				p->mem_ctx, &psd, &sd_size,
				&usr_nopwchange_generic_mapping,
				&info->sid, SAMR_USR_RIGHTS_CANT_WRITE_PW);
		}
		goto done;
	}

	info = samr_policy_handle_find(p,
				       r->in.handle,
				       SAMR_HANDLE_GROUP,
				       SEC_STD_READ_CONTROL,
				       NULL,
				       &status);
	if (NT_STATUS_IS_OK(status)) {
		/*
		 * TODO: different SDs have to be generated for aliases groups
		 * and users.  Currently all three get a default user SD
		 */
		DEBUG(10,("_samr_QuerySecurity: querying security on group "
			  "Object with SID: %s\n",
			  dom_sid_str_buf(&info->sid, &buf)));
		status = make_samr_object_sd(
			p->mem_ctx, &psd, &sd_size,
			&usr_nopwchange_generic_mapping,
			&info->sid, SAMR_USR_RIGHTS_CANT_WRITE_PW);
		goto done;
	}

	info = samr_policy_handle_find(p,
				       r->in.handle,
				       SAMR_HANDLE_ALIAS,
				       SEC_STD_READ_CONTROL,
				       NULL,
				       &status);
	if (NT_STATUS_IS_OK(status)) {
		/*
		 * TODO: different SDs have to be generated for aliases groups
		 * and users.  Currently all three get a default user SD
		 */
		DEBUG(10,("_samr_QuerySecurity: querying security on alias "
			  "Object with SID: %s\n",
			  dom_sid_str_buf(&info->sid, &buf)));
		status = make_samr_object_sd(
			p->mem_ctx, &psd, &sd_size,
			&usr_nopwchange_generic_mapping,
			&info->sid, SAMR_USR_RIGHTS_CANT_WRITE_PW);
		goto done;
	}

	return NT_STATUS_OBJECT_TYPE_MISMATCH;
done:
	if ((*r->out.sdbuf = make_sec_desc_buf(p->mem_ctx, sd_size, psd)) == NULL)
		return NT_STATUS_NO_MEMORY;

	return status;
}

/*******************************************************************
makes a SAM_ENTRY / UNISTR2* structure from a user list.
********************************************************************/

static NTSTATUS make_user_sam_entry_list(TALLOC_CTX *ctx,
					 struct samr_SamEntry **sam_pp,
					 uint32_t num_entries,
					 uint32_t start_idx,
					 struct samr_displayentry *entries)
{
	uint32_t i;
	struct samr_SamEntry *sam;

	*sam_pp = NULL;

	if (num_entries == 0) {
		return NT_STATUS_OK;
	}

	sam = talloc_zero_array(ctx, struct samr_SamEntry, num_entries);
	if (sam == NULL) {
		DEBUG(0, ("make_user_sam_entry_list: TALLOC_ZERO failed!\n"));
		return NT_STATUS_NO_MEMORY;
	}

	for (i = 0; i < num_entries; i++) {
#if 0
		/*
		 * usrmgr expects a non-NULL terminated string with
		 * trust relationships
		 */
		if (entries[i].acct_flags & ACB_DOMTRUST) {
			init_unistr2(&uni_temp_name, entries[i].account_name,
				     UNI_FLAGS_NONE);
		} else {
			init_unistr2(&uni_temp_name, entries[i].account_name,
				     UNI_STR_TERMINATE);
		}
#endif
		init_lsa_String(&sam[i].name, entries[i].account_name);
		sam[i].idx = entries[i].rid;
	}

	*sam_pp = sam;

	return NT_STATUS_OK;
}

#define MAX_SAM_ENTRIES MAX_SAM_ENTRIES_W2K

/*******************************************************************
 _samr_EnumDomainUsers
 ********************************************************************/

NTSTATUS _samr_EnumDomainUsers(struct pipes_struct *p,
			       struct samr_EnumDomainUsers *r)
{
	NTSTATUS status;
	struct samr_info *dinfo;
	uint32_t num_account;
	uint32_t enum_context = *r->in.resume_handle;
	enum remote_arch_types ra_type = get_remote_arch();
	int max_sam_entries = (ra_type == RA_WIN95) ? MAX_SAM_ENTRIES_W95 : MAX_SAM_ENTRIES_W2K;
	uint32_t max_entries = max_sam_entries;
	struct samr_displayentry *entries = NULL;
	struct samr_SamArray *samr_array = NULL;
	struct samr_SamEntry *samr_entries = NULL;

	DEBUG(5,("_samr_EnumDomainUsers: %d\n", __LINE__));

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					SAMR_DOMAIN_ACCESS_ENUM_ACCOUNTS,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	samr_array = talloc_zero(p->mem_ctx, struct samr_SamArray);
	if (!samr_array) {
		return NT_STATUS_NO_MEMORY;
	}
	*r->out.sam = samr_array;

	if (sid_check_is_builtin(&dinfo->sid)) {
		/* No users in builtin. */
		*r->out.resume_handle = *r->in.resume_handle;
		DEBUG(5,("_samr_EnumDomainUsers: No users in BUILTIN\n"));
		return status;
	}

	become_root();

	/* AS ROOT !!!! */

	if ((dinfo->disp_info->enum_users != NULL) &&
	    (dinfo->disp_info->enum_acb_mask != r->in.acct_flags)) {
		TALLOC_FREE(dinfo->disp_info->enum_users);
	}

	if (dinfo->disp_info->enum_users == NULL) {
		dinfo->disp_info->enum_users = pdb_search_users(
			dinfo->disp_info, r->in.acct_flags);
		dinfo->disp_info->enum_acb_mask = r->in.acct_flags;
	}

	if (dinfo->disp_info->enum_users == NULL) {
		/* END AS ROOT !!!! */
		unbecome_root();
		return NT_STATUS_ACCESS_DENIED;
	}

	num_account = pdb_search_entries(dinfo->disp_info->enum_users,
					 enum_context, max_entries,
					 &entries);

	/* END AS ROOT !!!! */

	unbecome_root();

	if (num_account == 0) {
		DEBUG(5, ("_samr_EnumDomainUsers: enumeration handle over "
			  "total entries\n"));
		*r->out.resume_handle = *r->in.resume_handle;
		return NT_STATUS_OK;
	}

	status = make_user_sam_entry_list(p->mem_ctx, &samr_entries,
					  num_account, enum_context,
					  entries);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (max_entries <= num_account) {
		status = STATUS_MORE_ENTRIES;
	} else {
		status = NT_STATUS_OK;
	}

	/* Ensure we cache this enumeration. */
	set_disp_info_cache_timeout(dinfo->disp_info, DISP_INFO_CACHE_TIMEOUT);

	DEBUG(5, ("_samr_EnumDomainUsers: %d\n", __LINE__));

	samr_array->count = num_account;
	samr_array->entries = samr_entries;

	*r->out.resume_handle = *r->in.resume_handle + num_account;
	*r->out.num_entries = num_account;

	DEBUG(5,("_samr_EnumDomainUsers: %d\n", __LINE__));

	return status;
}

/*******************************************************************
makes a SAM_ENTRY / UNISTR2* structure from a group list.
********************************************************************/

static void make_group_sam_entry_list(TALLOC_CTX *ctx,
				      struct samr_SamEntry **sam_pp,
				      uint32_t num_sam_entries,
				      struct samr_displayentry *entries)
{
	struct samr_SamEntry *sam;
	uint32_t i;

	*sam_pp = NULL;

	if (num_sam_entries == 0) {
		return;
	}

	sam = talloc_zero_array(ctx, struct samr_SamEntry, num_sam_entries);
	if (sam == NULL) {
		return;
	}

	for (i = 0; i < num_sam_entries; i++) {
		/*
		 * JRA. I think this should include the null. TNG does not.
		 */
		init_lsa_String(&sam[i].name, entries[i].account_name);
		sam[i].idx = entries[i].rid;
	}

	*sam_pp = sam;
}

/*******************************************************************
 _samr_EnumDomainGroups
 ********************************************************************/

NTSTATUS _samr_EnumDomainGroups(struct pipes_struct *p,
				struct samr_EnumDomainGroups *r)
{
	NTSTATUS status;
	struct samr_info *dinfo;
	struct samr_displayentry *groups;
	uint32_t num_groups;
	struct samr_SamArray *samr_array = NULL;
	struct samr_SamEntry *samr_entries = NULL;

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					SAMR_DOMAIN_ACCESS_ENUM_ACCOUNTS,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(5,("_samr_EnumDomainGroups: %d\n", __LINE__));

	samr_array = talloc_zero(p->mem_ctx, struct samr_SamArray);
	if (!samr_array) {
		return NT_STATUS_NO_MEMORY;
	}
	*r->out.sam = samr_array;

	if (sid_check_is_builtin(&dinfo->sid)) {
		/* No groups in builtin. */
		*r->out.resume_handle = *r->in.resume_handle;
		DEBUG(5,("_samr_EnumDomainGroups: No groups in BUILTIN\n"));
		return status;
	}

	/* the domain group array is being allocated in the function below */

	become_root();

	if (dinfo->disp_info->groups == NULL) {
		dinfo->disp_info->groups = pdb_search_groups(dinfo->disp_info);

		if (dinfo->disp_info->groups == NULL) {
			unbecome_root();
			return NT_STATUS_ACCESS_DENIED;
		}
	}

	num_groups = pdb_search_entries(dinfo->disp_info->groups,
					*r->in.resume_handle,
					MAX_SAM_ENTRIES, &groups);
	unbecome_root();

	/* Ensure we cache this enumeration. */
	set_disp_info_cache_timeout(dinfo->disp_info, DISP_INFO_CACHE_TIMEOUT);

	make_group_sam_entry_list(p->mem_ctx, &samr_entries,
				  num_groups, groups);

	if (MAX_SAM_ENTRIES <= num_groups) {
		status = STATUS_MORE_ENTRIES;
	} else {
		status = NT_STATUS_OK;
	}

	samr_array->count = num_groups;
	samr_array->entries = samr_entries;

	*r->out.num_entries = num_groups;
	*r->out.resume_handle = num_groups + *r->in.resume_handle;

	DEBUG(5,("_samr_EnumDomainGroups: %d\n", __LINE__));

	return status;
}

/*******************************************************************
 _samr_EnumDomainAliases
 ********************************************************************/

NTSTATUS _samr_EnumDomainAliases(struct pipes_struct *p,
				 struct samr_EnumDomainAliases *r)
{
	NTSTATUS status;
	struct samr_info *dinfo;
	struct samr_displayentry *aliases;
	uint32_t num_aliases = 0;
	struct samr_SamArray *samr_array = NULL;
	struct samr_SamEntry *samr_entries = NULL;
	struct dom_sid_buf buf;

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					SAMR_DOMAIN_ACCESS_ENUM_ACCOUNTS,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(5,("_samr_EnumDomainAliases: sid %s\n",
		 dom_sid_str_buf(&dinfo->sid, &buf)));

	samr_array = talloc_zero(p->mem_ctx, struct samr_SamArray);
	if (!samr_array) {
		return NT_STATUS_NO_MEMORY;
	}

	become_root();

	if (dinfo->disp_info->aliases == NULL) {
		dinfo->disp_info->aliases = pdb_search_aliases(
			dinfo->disp_info, &dinfo->sid);
		if (dinfo->disp_info->aliases == NULL) {
			unbecome_root();
			return NT_STATUS_ACCESS_DENIED;
		}
	}

	num_aliases = pdb_search_entries(dinfo->disp_info->aliases,
					 *r->in.resume_handle,
					 MAX_SAM_ENTRIES, &aliases);
	unbecome_root();

	/* Ensure we cache this enumeration. */
	set_disp_info_cache_timeout(dinfo->disp_info, DISP_INFO_CACHE_TIMEOUT);

	make_group_sam_entry_list(p->mem_ctx, &samr_entries,
				  num_aliases, aliases);

	DEBUG(5,("_samr_EnumDomainAliases: %d\n", __LINE__));

	if (MAX_SAM_ENTRIES <= num_aliases) {
		status = STATUS_MORE_ENTRIES;
	} else {
		status = NT_STATUS_OK;
	}

	samr_array->count = num_aliases;
	samr_array->entries = samr_entries;

	*r->out.sam = samr_array;
	*r->out.num_entries = num_aliases;
	*r->out.resume_handle = num_aliases + *r->in.resume_handle;

	return status;
}

/*******************************************************************
 inits a samr_DispInfoGeneral structure.
********************************************************************/

static NTSTATUS init_samr_dispinfo_1(TALLOC_CTX *ctx,
				     struct samr_DispInfoGeneral *r,
				     uint32_t num_entries,
				     uint32_t start_idx,
				     struct samr_displayentry *entries)
{
	uint32_t i;

	DEBUG(10, ("init_samr_dispinfo_1: num_entries: %d\n", num_entries));

	if (num_entries == 0) {
		return NT_STATUS_OK;
	}

	r->count = num_entries;

	r->entries = talloc_zero_array(ctx, struct samr_DispEntryGeneral, num_entries);
	if (!r->entries) {
		return NT_STATUS_NO_MEMORY;
	}

	for (i = 0; i < num_entries ; i++) {

		init_lsa_String(&r->entries[i].account_name,
				entries[i].account_name);

		init_lsa_String(&r->entries[i].description,
				entries[i].description);

		init_lsa_String(&r->entries[i].full_name,
				entries[i].fullname);

		r->entries[i].rid = entries[i].rid;
		r->entries[i].acct_flags = entries[i].acct_flags;
		r->entries[i].idx = start_idx+i+1;
	}

	return NT_STATUS_OK;
}

/*******************************************************************
 inits a samr_DispInfoFull structure.
********************************************************************/

static NTSTATUS init_samr_dispinfo_2(TALLOC_CTX *ctx,
				     struct samr_DispInfoFull *r,
				     uint32_t num_entries,
				     uint32_t start_idx,
				     struct samr_displayentry *entries)
{
	uint32_t i;

	DEBUG(10, ("init_samr_dispinfo_2: num_entries: %d\n", num_entries));

	if (num_entries == 0) {
		return NT_STATUS_OK;
	}

	r->count = num_entries;

	r->entries = talloc_zero_array(ctx, struct samr_DispEntryFull, num_entries);
	if (!r->entries) {
		return NT_STATUS_NO_MEMORY;
	}

	for (i = 0; i < num_entries ; i++) {

		init_lsa_String(&r->entries[i].account_name,
				entries[i].account_name);

		init_lsa_String(&r->entries[i].description,
				entries[i].description);

		r->entries[i].rid = entries[i].rid;
		r->entries[i].acct_flags = entries[i].acct_flags;
		r->entries[i].idx = start_idx+i+1;
	}

	return NT_STATUS_OK;
}

/*******************************************************************
 inits a samr_DispInfoFullGroups structure.
********************************************************************/

static NTSTATUS init_samr_dispinfo_3(TALLOC_CTX *ctx,
				     struct samr_DispInfoFullGroups *r,
				     uint32_t num_entries,
				     uint32_t start_idx,
				     struct samr_displayentry *entries)
{
	uint32_t i;

	DEBUG(5, ("init_samr_dispinfo_3: num_entries: %d\n", num_entries));

	if (num_entries == 0) {
		return NT_STATUS_OK;
	}

	r->count = num_entries;

	r->entries = talloc_zero_array(ctx, struct samr_DispEntryFullGroup, num_entries);
	if (!r->entries) {
		return NT_STATUS_NO_MEMORY;
	}

	for (i = 0; i < num_entries ; i++) {

		init_lsa_String(&r->entries[i].account_name,
				entries[i].account_name);

		init_lsa_String(&r->entries[i].description,
				entries[i].description);

		r->entries[i].rid = entries[i].rid;
		r->entries[i].acct_flags = entries[i].acct_flags;
		r->entries[i].idx = start_idx+i+1;
	}

	return NT_STATUS_OK;
}

/*******************************************************************
 inits a samr_DispInfoAscii structure.
********************************************************************/

static NTSTATUS init_samr_dispinfo_4(TALLOC_CTX *ctx,
				     struct samr_DispInfoAscii *r,
				     uint32_t num_entries,
				     uint32_t start_idx,
				     struct samr_displayentry *entries)
{
	uint32_t i;

	DEBUG(5, ("init_samr_dispinfo_4: num_entries: %d\n", num_entries));

	if (num_entries == 0) {
		return NT_STATUS_OK;
	}

	r->count = num_entries;

	r->entries = talloc_zero_array(ctx, struct samr_DispEntryAscii, num_entries);
	if (!r->entries) {
		return NT_STATUS_NO_MEMORY;
	}

	for (i = 0; i < num_entries ; i++) {

		init_lsa_AsciiStringLarge(&r->entries[i].account_name,
					  entries[i].account_name);

		r->entries[i].idx = start_idx+i+1;
	}

	return NT_STATUS_OK;
}

/*******************************************************************
 inits a samr_DispInfoAscii structure.
********************************************************************/

static NTSTATUS init_samr_dispinfo_5(TALLOC_CTX *ctx,
				     struct samr_DispInfoAscii *r,
				     uint32_t num_entries,
				     uint32_t start_idx,
				     struct samr_displayentry *entries)
{
	uint32_t i;

	DEBUG(5, ("init_samr_dispinfo_5: num_entries: %d\n", num_entries));

	if (num_entries == 0) {
		return NT_STATUS_OK;
	}

	r->count = num_entries;

	r->entries = talloc_zero_array(ctx, struct samr_DispEntryAscii, num_entries);
	if (!r->entries) {
		return NT_STATUS_NO_MEMORY;
	}

	for (i = 0; i < num_entries ; i++) {

		init_lsa_AsciiStringLarge(&r->entries[i].account_name,
					  entries[i].account_name);

		r->entries[i].idx = start_idx+i+1;
	}

	return NT_STATUS_OK;
}

/*******************************************************************
 _samr_QueryDisplayInfo
 ********************************************************************/

NTSTATUS _samr_QueryDisplayInfo(struct pipes_struct *p,
				struct samr_QueryDisplayInfo *r)
{
	NTSTATUS status;
	struct samr_info *dinfo;
	uint32_t struct_size=0x20; /* W2K always reply that, client doesn't care */

	uint32_t max_entries = r->in.max_entries;

	union samr_DispInfo *disp_info = r->out.info;

	uint32_t temp_size=0;
	NTSTATUS disp_ret = NT_STATUS_UNSUCCESSFUL;
	uint32_t num_account = 0;
	enum remote_arch_types ra_type = get_remote_arch();
	uint32_t max_sam_entries = (ra_type == RA_WIN95) ?
		MAX_SAM_ENTRIES_W95 : MAX_SAM_ENTRIES_W2K;
	struct samr_displayentry *entries = NULL;

	DEBUG(5,("_samr_QueryDisplayInfo: %d\n", __LINE__));

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					SAMR_DOMAIN_ACCESS_ENUM_ACCOUNTS,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (sid_check_is_builtin(&dinfo->sid)) {
		DEBUG(5,("_samr_QueryDisplayInfo: no users in BUILTIN\n"));
		return NT_STATUS_OK;
	}

	/*
	 * calculate how many entries we will return.
	 * based on
	 * - the number of entries the client asked
	 * - our limit on that
	 * - the starting point (enumeration context)
	 * - the buffer size the client will accept
	 */

	/*
	 * We are a lot more like W2K. Instead of reading the SAM
	 * each time to find the records we need to send back,
	 * we read it once and link that copy to the sam handle.
	 * For large user list (over the MAX_SAM_ENTRIES)
	 * it's a definitive win.
	 * second point to notice: between enumerations
	 * our sam is now the same as it's a snapshoot.
	 * third point: got rid of the static SAM_USER_21 struct
	 * no more intermediate.
	 * con: it uses much more memory, as a full copy is stored
	 * in memory.
	 *
	 * If you want to change it, think twice and think
	 * of the second point , that's really important.
	 *
	 * JFM, 12/20/2001
	 */

	if ((r->in.level < 1) || (r->in.level > 5)) {
		DEBUG(0,("_samr_QueryDisplayInfo: Unknown info level (%u)\n",
			 (unsigned int)r->in.level ));
		return NT_STATUS_INVALID_INFO_CLASS;
	}

	/* first limit the number of entries we will return */
	if (r->in.max_entries > max_sam_entries) {
		DEBUG(5, ("_samr_QueryDisplayInfo: client requested %d "
			  "entries, limiting to %d\n", r->in.max_entries,
			  max_sam_entries));
		max_entries = max_sam_entries;
	}

	/* calculate the size and limit on the number of entries we will
	 * return */

	temp_size=max_entries*struct_size;

	if (temp_size > r->in.buf_size) {
		max_entries = MIN((r->in.buf_size / struct_size),max_entries);
		DEBUG(5, ("_samr_QueryDisplayInfo: buffer size limits to "
			  "only %d entries\n", max_entries));
	}

	become_root();

	/* The following done as ROOT. Don't return without unbecome_root(). */

	switch (r->in.level) {
	case 1:
	case 4:
		if (dinfo->disp_info->users == NULL) {
			dinfo->disp_info->users = pdb_search_users(
				dinfo->disp_info, ACB_NORMAL);
			if (dinfo->disp_info->users == NULL) {
				unbecome_root();
				return NT_STATUS_ACCESS_DENIED;
			}
			DEBUG(10,("_samr_QueryDisplayInfo: starting user enumeration at index %u\n",
				(unsigned  int)r->in.start_idx));
		} else {
			DEBUG(10,("_samr_QueryDisplayInfo: using cached user enumeration at index %u\n",
				(unsigned  int)r->in.start_idx));
		}

		num_account = pdb_search_entries(dinfo->disp_info->users,
						 r->in.start_idx, max_entries,
						 &entries);
		break;
	case 2:
		if (dinfo->disp_info->machines == NULL) {
			dinfo->disp_info->machines = pdb_search_users(
				dinfo->disp_info, ACB_WSTRUST|ACB_SVRTRUST);
			if (dinfo->disp_info->machines == NULL) {
				unbecome_root();
				return NT_STATUS_ACCESS_DENIED;
			}
			DEBUG(10,("_samr_QueryDisplayInfo: starting machine enumeration at index %u\n",
				(unsigned  int)r->in.start_idx));
		} else {
			DEBUG(10,("_samr_QueryDisplayInfo: using cached machine enumeration at index %u\n",
				(unsigned  int)r->in.start_idx));
		}

		num_account = pdb_search_entries(dinfo->disp_info->machines,
						 r->in.start_idx, max_entries,
						 &entries);
		break;
	case 3:
	case 5:
		if (dinfo->disp_info->groups == NULL) {
			dinfo->disp_info->groups = pdb_search_groups(
				dinfo->disp_info);
			if (dinfo->disp_info->groups == NULL) {
				unbecome_root();
				return NT_STATUS_ACCESS_DENIED;
			}
			DEBUG(10,("_samr_QueryDisplayInfo: starting group enumeration at index %u\n",
				(unsigned  int)r->in.start_idx));
		} else {
			DEBUG(10,("_samr_QueryDisplayInfo: using cached group enumeration at index %u\n",
				(unsigned  int)r->in.start_idx));
		}

		num_account = pdb_search_entries(dinfo->disp_info->groups,
						 r->in.start_idx, max_entries,
						 &entries);
		break;
	default:
		unbecome_root();
		smb_panic("info class changed");
		break;
	}
	unbecome_root();


	/* Now create reply structure */
	switch (r->in.level) {
	case 1:
		disp_ret = init_samr_dispinfo_1(p->mem_ctx, &disp_info->info1,
						num_account, r->in.start_idx,
						entries);
		break;
	case 2:
		disp_ret = init_samr_dispinfo_2(p->mem_ctx, &disp_info->info2,
						num_account, r->in.start_idx,
						entries);
		break;
	case 3:
		disp_ret = init_samr_dispinfo_3(p->mem_ctx, &disp_info->info3,
						num_account, r->in.start_idx,
						entries);
		break;
	case 4:
		disp_ret = init_samr_dispinfo_4(p->mem_ctx, &disp_info->info4,
						num_account, r->in.start_idx,
						entries);
		break;
	case 5:
		disp_ret = init_samr_dispinfo_5(p->mem_ctx, &disp_info->info5,
						num_account, r->in.start_idx,
						entries);
		break;
	default:
		smb_panic("info class changed");
		break;
	}

	if (!NT_STATUS_IS_OK(disp_ret))
		return disp_ret;

	if (max_entries <= num_account) {
		status = STATUS_MORE_ENTRIES;
	} else {
		status = NT_STATUS_OK;
	}

	/* Ensure we cache this enumeration. */
	set_disp_info_cache_timeout(dinfo->disp_info, DISP_INFO_CACHE_TIMEOUT);

	DEBUG(5, ("_samr_QueryDisplayInfo: %d\n", __LINE__));

	*r->out.total_size = num_account * struct_size;
	*r->out.returned_size = num_account ? temp_size : 0;

	return status;
}

/****************************************************************
 _samr_QueryDisplayInfo2
****************************************************************/

NTSTATUS _samr_QueryDisplayInfo2(struct pipes_struct *p,
				 struct samr_QueryDisplayInfo2 *r)
{
	struct samr_QueryDisplayInfo q;

	q.in.domain_handle	= r->in.domain_handle;
	q.in.level		= r->in.level;
	q.in.start_idx		= r->in.start_idx;
	q.in.max_entries	= r->in.max_entries;
	q.in.buf_size		= r->in.buf_size;

	q.out.total_size	= r->out.total_size;
	q.out.returned_size	= r->out.returned_size;
	q.out.info		= r->out.info;

	return _samr_QueryDisplayInfo(p, &q);
}

/****************************************************************
 _samr_QueryDisplayInfo3
****************************************************************/

NTSTATUS _samr_QueryDisplayInfo3(struct pipes_struct *p,
				 struct samr_QueryDisplayInfo3 *r)
{
	struct samr_QueryDisplayInfo q;

	q.in.domain_handle	= r->in.domain_handle;
	q.in.level		= r->in.level;
	q.in.start_idx		= r->in.start_idx;
	q.in.max_entries	= r->in.max_entries;
	q.in.buf_size		= r->in.buf_size;

	q.out.total_size	= r->out.total_size;
	q.out.returned_size	= r->out.returned_size;
	q.out.info		= r->out.info;

	return _samr_QueryDisplayInfo(p, &q);
}

/*******************************************************************
 _samr_QueryAliasInfo
 ********************************************************************/

NTSTATUS _samr_QueryAliasInfo(struct pipes_struct *p,
			      struct samr_QueryAliasInfo *r)
{
	struct samr_info *ainfo;
	struct acct_info *info;
	NTSTATUS status;
	union samr_AliasInfo *alias_info = NULL;
	const char *alias_name = NULL;
	const char *alias_description = NULL;

	DEBUG(5,("_samr_QueryAliasInfo: %d\n", __LINE__));

	ainfo = samr_policy_handle_find(p,
					r->in.alias_handle,
					SAMR_HANDLE_ALIAS,
					SAMR_ALIAS_ACCESS_LOOKUP_INFO,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	alias_info = talloc_zero(p->mem_ctx, union samr_AliasInfo);
	if (!alias_info) {
		return NT_STATUS_NO_MEMORY;
	}

	info = talloc_zero(p->mem_ctx, struct acct_info);
	if (!info) {
		return NT_STATUS_NO_MEMORY;
	}

	become_root();
	status = pdb_get_aliasinfo(&ainfo->sid, info);
	unbecome_root();

	if (!NT_STATUS_IS_OK(status)) {
		TALLOC_FREE(info);
		return status;
	}

	alias_name = talloc_steal(r, info->acct_name);
	alias_description = talloc_steal(r, info->acct_desc);
	TALLOC_FREE(info);

	switch (r->in.level) {
	case ALIASINFOALL:
		alias_info->all.name.string		= alias_name;
		alias_info->all.num_members		= 1; /* ??? */
		alias_info->all.description.string	= alias_description;
		break;
	case ALIASINFONAME:
		alias_info->name.string			= alias_name;
		break;
	case ALIASINFODESCRIPTION:
		alias_info->description.string		= alias_description;
		break;
	default:
		return NT_STATUS_INVALID_INFO_CLASS;
	}

	*r->out.info = alias_info;

	DEBUG(5,("_samr_QueryAliasInfo: %d\n", __LINE__));

	return NT_STATUS_OK;
}

/*******************************************************************
 _samr_LookupNames
 ********************************************************************/

NTSTATUS _samr_LookupNames(struct pipes_struct *p,
			   struct samr_LookupNames *r)
{
	struct samr_info *dinfo;
	NTSTATUS status;
	uint32_t *rid;
	enum lsa_SidType *type;
	uint32_t i, num_rids = r->in.num_names;
	struct samr_Ids rids, types;
	uint32_t num_mapped = 0;
	struct dom_sid_buf buf;

	DEBUG(5,("_samr_LookupNames: %d\n", __LINE__));

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					0 /* Don't know the acc_bits yet */,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (num_rids > MAX_SAM_ENTRIES) {
		num_rids = MAX_SAM_ENTRIES;
		DEBUG(5,("_samr_LookupNames: truncating entries to %d\n", num_rids));
	}

	rid = talloc_array(p->mem_ctx, uint32_t, num_rids);
	NT_STATUS_HAVE_NO_MEMORY(rid);

	type = talloc_array(p->mem_ctx, enum lsa_SidType, num_rids);
	NT_STATUS_HAVE_NO_MEMORY(type);

	DEBUG(5,("_samr_LookupNames: looking name on SID %s\n",
		 dom_sid_str_buf(&dinfo->sid, &buf)));

	for (i = 0; i < num_rids; i++) {

		status = NT_STATUS_NONE_MAPPED;
	        type[i] = SID_NAME_UNKNOWN;

		rid[i] = 0xffffffff;

		if (sid_check_is_builtin(&dinfo->sid)) {
			if (lookup_builtin_name(r->in.names[i].string,
						&rid[i]))
			{
				type[i] = SID_NAME_ALIAS;
			}
		} else {
			lookup_global_sam_name(r->in.names[i].string, 0,
					       &rid[i], &type[i]);
		}

		if (type[i] != SID_NAME_UNKNOWN) {
			num_mapped++;
		}
	}

	if (num_mapped == num_rids) {
		status = NT_STATUS_OK;
	} else if (num_mapped == 0) {
		status = NT_STATUS_NONE_MAPPED;
	} else {
		status = STATUS_SOME_UNMAPPED;
	}

	rids.count = num_rids;
	rids.ids = rid;

	types.count = num_rids;
	types.ids = talloc_array(p->mem_ctx, uint32_t, num_rids);
	NT_STATUS_HAVE_NO_MEMORY(type);
	for (i = 0; i < num_rids; i++) {
		types.ids[i] = (type[i] & 0xffffffff);
	}

	*r->out.rids = rids;
	*r->out.types = types;

	DEBUG(5,("_samr_LookupNames: %d\n", __LINE__));

	return status;
}

/****************************************************************
 _samr_ChangePasswordUser.

 So old it is just not worth implementing
 because it does not supply a plaintext and so we can't do password
 complexity checking and cannot update other services that use a
 plaintext password via passwd chat/pam password change/ldap password
 sync.
****************************************************************/

NTSTATUS _samr_ChangePasswordUser(struct pipes_struct *p,
				  struct samr_ChangePasswordUser *r)
{
	return NT_STATUS_NOT_IMPLEMENTED;
}

/*******************************************************************
 _samr_ChangePasswordUser2
 ********************************************************************/

NTSTATUS _samr_ChangePasswordUser2(struct pipes_struct *p,
				   struct samr_ChangePasswordUser2 *r)
{
	struct dcesrv_call_state *dce_call = p->dce_call;
	struct dcesrv_connection *dcesrv_conn = dce_call->conn;
	const struct tsocket_address *remote_address =
		dcesrv_connection_get_remote_address(dcesrv_conn);
	struct auth_session_info *session_info =
		dcesrv_call_session_info(dce_call);
	NTSTATUS status;
	char *user_name = NULL;
	char *rhost;
	const char *wks = NULL;
	bool encrypted;

	DEBUG(5,("_samr_ChangePasswordUser2: %d\n", __LINE__));

	if (!r->in.account->string) {
		return NT_STATUS_INVALID_PARAMETER;
	}
	if (r->in.server && r->in.server->string) {
		wks = r->in.server->string;
	}

	DEBUG(5,("_samr_ChangePasswordUser2: user: %s wks: %s\n", user_name, wks));

	/*
	 * Pass the user through the NT -> unix user mapping
	 * function.
	 */

	(void)map_username(talloc_tos(), r->in.account->string, &user_name);
	if (!user_name) {
		return NT_STATUS_NO_MEMORY;
	}

	rhost = tsocket_address_inet_addr_string(remote_address,
						 talloc_tos());
	if (rhost == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	encrypted = dcerpc_is_transport_encrypted(session_info);
	if (lp_weak_crypto() == SAMBA_WEAK_CRYPTO_DISALLOWED &&
	    !encrypted) {
		return NT_STATUS_ACCESS_DENIED;
	}

	/*
	 * UNIX username case mangling not required, pass_oem_change
	 * is case insensitive.
	 */

	status = pass_oem_change(user_name,
				 rhost,
				 r->in.lm_password->data,
				 r->in.lm_verifier->hash,
				 r->in.nt_password->data,
				 r->in.nt_verifier->hash,
				 NULL);

	DEBUG(5,("_samr_ChangePasswordUser2: %d\n", __LINE__));

	if (NT_STATUS_EQUAL(status, NT_STATUS_NO_SUCH_USER)) {
		return NT_STATUS_WRONG_PASSWORD;
	}

	return status;
}

/****************************************************************
 _samr_OemChangePasswordUser2
****************************************************************/

NTSTATUS _samr_OemChangePasswordUser2(struct pipes_struct *p,
				      struct samr_OemChangePasswordUser2 *r)
{
	struct dcesrv_call_state *dce_call = p->dce_call;
	struct dcesrv_connection *dcesrv_conn = dce_call->conn;
	const struct tsocket_address *remote_address =
		dcesrv_connection_get_remote_address(dcesrv_conn);
	struct auth_session_info *session_info =
		dcesrv_call_session_info(dce_call);
	NTSTATUS status;
	char *user_name = NULL;
	const char *wks = NULL;
	char *rhost;
	bool encrypted;

	DEBUG(5,("_samr_OemChangePasswordUser2: %d\n", __LINE__));

	if (!r->in.account->string) {
		return NT_STATUS_INVALID_PARAMETER;
	}
	if (r->in.server && r->in.server->string) {
		wks = r->in.server->string;
	}

	DEBUG(5,("_samr_OemChangePasswordUser2: user: %s wks: %s\n", user_name, wks));

	/*
	 * Pass the user through the NT -> unix user mapping
	 * function.
	 */

	(void)map_username(talloc_tos(), r->in.account->string, &user_name);
	if (!user_name) {
		return NT_STATUS_NO_MEMORY;
	}

	/*
	 * UNIX username case mangling not required, pass_oem_change
	 * is case insensitive.
	 */

	if (!r->in.hash || !r->in.password) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	rhost = tsocket_address_inet_addr_string(remote_address,
						 talloc_tos());
	if (rhost == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	encrypted = dcerpc_is_transport_encrypted(session_info);
	if (lp_weak_crypto() == SAMBA_WEAK_CRYPTO_DISALLOWED &&
	    !encrypted) {
		return NT_STATUS_ACCESS_DENIED;
	}

	status = pass_oem_change(user_name,
				 rhost,
				 r->in.password->data,
				 r->in.hash->hash,
				 0,
				 0,
				 NULL);

	if (NT_STATUS_EQUAL(status, NT_STATUS_NO_SUCH_USER)) {
		return NT_STATUS_WRONG_PASSWORD;
	}

	DEBUG(5,("_samr_OemChangePasswordUser2: %d\n", __LINE__));

	return status;
}

/*******************************************************************
 _samr_ChangePasswordUser3
 ********************************************************************/

NTSTATUS _samr_ChangePasswordUser3(struct pipes_struct *p,
				   struct samr_ChangePasswordUser3 *r)
{
	struct dcesrv_call_state *dce_call = p->dce_call;
	struct dcesrv_connection *dcesrv_conn = dce_call->conn;
	const struct tsocket_address *remote_address =
		dcesrv_connection_get_remote_address(dcesrv_conn);
	NTSTATUS status;
	char *user_name = NULL;
	const char *wks = NULL;
	enum samPwdChangeReason reject_reason;
	struct samr_DomInfo1 *dominfo = NULL;
	struct userPwdChangeFailureInformation *reject = NULL;
	const struct loadparm_substitution *lp_sub =
		loadparm_s3_global_substitution();
	uint32_t tmp;
	char *rhost;

	DEBUG(5,("_samr_ChangePasswordUser3: %d\n", __LINE__));

	if (!r->in.account->string) {
		return NT_STATUS_INVALID_PARAMETER;
	}
	if (r->in.server && r->in.server->string) {
		wks = r->in.server->string;
	}

	DEBUG(5,("_samr_ChangePasswordUser3: user: %s wks: %s\n", user_name, wks));

	/*
	 * Pass the user through the NT -> unix user mapping
	 * function.
	 */

	(void)map_username(talloc_tos(), r->in.account->string, &user_name);
	if (!user_name) {
		return NT_STATUS_NO_MEMORY;
	}

	rhost = tsocket_address_inet_addr_string(remote_address,
						 talloc_tos());
	if (rhost == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	/*
	 * UNIX username case mangling not required, pass_oem_change
	 * is case insensitive.
	 */

	status = pass_oem_change(user_name,
				 rhost,
				 r->in.lm_password->data,
				 r->in.lm_verifier->hash,
				 r->in.nt_password->data,
				 r->in.nt_verifier->hash,
				 &reject_reason);
	if (NT_STATUS_EQUAL(status, NT_STATUS_NO_SUCH_USER)) {
		return NT_STATUS_WRONG_PASSWORD;
	}

	if (NT_STATUS_EQUAL(status, NT_STATUS_PASSWORD_RESTRICTION) ||
	    NT_STATUS_EQUAL(status, NT_STATUS_ACCOUNT_RESTRICTION)) {

		time_t u_expire, u_min_age;
		uint32_t account_policy_temp;

		dominfo = talloc_zero(p->mem_ctx, struct samr_DomInfo1);
		if (!dominfo) {
			return NT_STATUS_NO_MEMORY;
		}

		reject = talloc_zero(p->mem_ctx,
				struct userPwdChangeFailureInformation);
		if (!reject) {
			return NT_STATUS_NO_MEMORY;
		}

		become_root();

		/* AS ROOT !!! */

		pdb_get_account_policy(PDB_POLICY_MIN_PASSWORD_LEN, &tmp);
		dominfo->min_password_length = tmp;

		pdb_get_account_policy(PDB_POLICY_PASSWORD_HISTORY, &tmp);
		dominfo->password_history_length = tmp;

		pdb_get_account_policy(PDB_POLICY_USER_MUST_LOGON_TO_CHG_PASS,
				       &dominfo->password_properties);

		pdb_get_account_policy(PDB_POLICY_MAX_PASSWORD_AGE, &account_policy_temp);
		u_expire = account_policy_temp;

		pdb_get_account_policy(PDB_POLICY_MIN_PASSWORD_AGE, &account_policy_temp);
		u_min_age = account_policy_temp;

		/* !AS ROOT */

		unbecome_root();

		unix_to_nt_time_abs((NTTIME *)&dominfo->max_password_age, u_expire);
		unix_to_nt_time_abs((NTTIME *)&dominfo->min_password_age, u_min_age);

		if (lp_check_password_script(talloc_tos(), lp_sub)
			&& *lp_check_password_script(talloc_tos(), lp_sub)) {
			dominfo->password_properties |= DOMAIN_PASSWORD_COMPLEX;
		}

		reject->extendedFailureReason = reject_reason;

		*r->out.dominfo = dominfo;
		*r->out.reject = reject;
	}

	DEBUG(5,("_samr_ChangePasswordUser3: %d\n", __LINE__));

	return status;
}

/*******************************************************************
makes a SAMR_R_LOOKUP_RIDS structure.
********************************************************************/

static bool make_samr_lookup_rids(TALLOC_CTX *ctx, uint32_t num_names,
				  const char **names,
				  struct lsa_String **lsa_name_array_p)
{
	struct lsa_String *lsa_name_array = NULL;
	uint32_t i;

	*lsa_name_array_p = NULL;

	if (num_names != 0) {
		lsa_name_array = talloc_zero_array(ctx, struct lsa_String, num_names);
		if (!lsa_name_array) {
			return false;
		}
	}

	for (i = 0; i < num_names; i++) {
		DEBUG(10, ("names[%d]:%s\n", i, names[i] && *names[i] ? names[i] : ""));
		init_lsa_String(&lsa_name_array[i], names[i]);
	}

	*lsa_name_array_p = lsa_name_array;

	return true;
}

/*******************************************************************
 _samr_LookupRids
 ********************************************************************/

NTSTATUS _samr_LookupRids(struct pipes_struct *p,
			  struct samr_LookupRids *r)
{
	struct samr_info *dinfo;
	NTSTATUS status;
	const char **names;
	enum lsa_SidType *attrs = NULL;
	uint32_t *wire_attrs = NULL;
	int num_rids = (int)r->in.num_rids;
	int i;
	struct lsa_Strings names_array;
	struct samr_Ids types_array;
	struct lsa_String *lsa_names = NULL;

	DEBUG(5,("_samr_LookupRids: %d\n", __LINE__));

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					0 /* Don't know the acc_bits yet */,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (num_rids > 1000) {
		DEBUG(0, ("Got asked for %d rids (more than 1000) -- according "
			  "to samba4 idl this is not possible\n", num_rids));
		return NT_STATUS_UNSUCCESSFUL;
	}

	if (num_rids) {
		names = talloc_zero_array(p->mem_ctx, const char *, num_rids);
		attrs = talloc_zero_array(p->mem_ctx, enum lsa_SidType, num_rids);
		wire_attrs = talloc_zero_array(p->mem_ctx, uint32_t, num_rids);

		if ((names == NULL) || (attrs == NULL) || (wire_attrs==NULL))
			return NT_STATUS_NO_MEMORY;
	} else {
		names = NULL;
		attrs = NULL;
		wire_attrs = NULL;
	}

	become_root();  /* lookup_sid can require root privs */
	status = pdb_lookup_rids(&dinfo->sid, num_rids, r->in.rids,
				 names, attrs);
	unbecome_root();

	if (NT_STATUS_EQUAL(status, NT_STATUS_NONE_MAPPED) && (num_rids == 0)) {
		status = NT_STATUS_OK;
	}

	if (!make_samr_lookup_rids(p->mem_ctx, num_rids, names,
				   &lsa_names)) {
		return NT_STATUS_NO_MEMORY;
	}

	/* Convert from enum lsa_SidType to uint32_t for wire format. */
	for (i = 0; i < num_rids; i++) {
		wire_attrs[i] = (uint32_t)attrs[i];
	}

	names_array.count = num_rids;
	names_array.names = lsa_names;

	types_array.count = num_rids;
	types_array.ids = wire_attrs;

	*r->out.names = names_array;
	*r->out.types = types_array;

	DEBUG(5,("_samr_LookupRids: %d\n", __LINE__));

	return status;
}

/*******************************************************************
 _samr_OpenUser
********************************************************************/

NTSTATUS _samr_OpenUser(struct pipes_struct *p,
			struct samr_OpenUser *r)
{
	struct dcesrv_call_state *dce_call = p->dce_call;
	struct auth_session_info *session_info =
		dcesrv_call_session_info(dce_call);
	struct samu *sampass=NULL;
	struct dom_sid sid;
	struct samr_info *dinfo;
	struct security_descriptor *psd = NULL;
	uint32_t    acc_granted;
	uint32_t    des_access = r->in.access_mask;
	uint32_t extra_access = 0;
	size_t    sd_size;
	bool ret;
	NTSTATUS nt_status;

	/* These two privileges, if != SEC_PRIV_INVALID, indicate
	 * privileges that the user must have to complete this
	 * operation in defience of the fixed ACL */
	enum sec_privilege needed_priv_1, needed_priv_2;
	NTSTATUS status;

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					SAMR_DOMAIN_ACCESS_OPEN_ACCOUNT,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if ( !(sampass = samu_new( p->mem_ctx )) ) {
		return NT_STATUS_NO_MEMORY;
	}

	/* append the user's RID to it */

	if (!sid_compose(&sid, &dinfo->sid, r->in.rid))
		return NT_STATUS_NO_SUCH_USER;

	/* check if access can be granted as requested by client. */
	map_max_allowed_access(session_info->security_token,
			       session_info->unix_token,
			       &des_access);

	make_samr_object_sd(p->mem_ctx, &psd, &sd_size, &usr_generic_mapping, &sid, SAMR_USR_RIGHTS_WRITE_PW);
	se_map_generic(&des_access, &usr_generic_mapping);

	/*
	 * Get the sampass first as we need to check privileges
	 * based on what kind of user object this is.
	 * But don't reveal info too early if it didn't exist.
	 */

	become_root();
	ret=pdb_getsampwsid(sampass, &sid);
	unbecome_root();

	needed_priv_1 = SEC_PRIV_INVALID;
	needed_priv_2 = SEC_PRIV_INVALID;
	/*
	 * We do the override access checks on *open*, not at
	 * SetUserInfo time.
	 */
	if (ret) {
		uint32_t acb_info = pdb_get_acct_ctrl(sampass);

		if (acb_info & ACB_WSTRUST) {
			/*
			 * SeMachineAccount is needed to add
			 * GENERIC_RIGHTS_USER_WRITE to a machine
			 * account.
			 */
			needed_priv_1 = SEC_PRIV_MACHINE_ACCOUNT;
		}
		if (acb_info & ACB_NORMAL) {
			/*
			 * SeAddUsers is needed to add
			 * GENERIC_RIGHTS_USER_WRITE to a normal
			 * account.
			 */
			needed_priv_1 = SEC_PRIV_ADD_USERS;
		}
		/*
		 * Cheat - we have not set a specific privilege for
		 * server (BDC) or domain trust account, so allow
		 * GENERIC_RIGHTS_USER_WRITE if pipe user is in
		 * DOMAIN_RID_ADMINS.
		 */
		if (acb_info & (ACB_SVRTRUST|ACB_DOMTRUST)) {
			if (lp_enable_privileges() &&
			    nt_token_check_domain_rid(
				    session_info->security_token,
				    DOMAIN_RID_ADMINS)) {
				des_access &= ~GENERIC_RIGHTS_USER_WRITE;
				extra_access = GENERIC_RIGHTS_USER_WRITE;
				DEBUG(4,("_samr_OpenUser: Allowing "
					"GENERIC_RIGHTS_USER_WRITE for "
					"rid admins\n"));
			}
		}
	}

	TALLOC_FREE(sampass);

	nt_status = access_check_object(psd, session_info->security_token,
					needed_priv_1, needed_priv_2,
					GENERIC_RIGHTS_USER_WRITE, des_access,
					&acc_granted, "_samr_OpenUser");

	if ( !NT_STATUS_IS_OK(nt_status) )
		return nt_status;

	/* check that the SID exists in our domain. */
	if (ret == False) {
		return NT_STATUS_NO_SUCH_USER;
	}

	/* If we did the rid admins hack above, allow access. */
	acc_granted |= extra_access;

	status = create_samr_policy_handle(p->mem_ctx,
					   p,
					   SAMR_HANDLE_USER,
					   acc_granted,
					   &sid,
					   NULL,
					   r->out.user_handle);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	return NT_STATUS_OK;
}

/*************************************************************************
 *************************************************************************/

static NTSTATUS init_samr_parameters_string(TALLOC_CTX *mem_ctx,
					    DATA_BLOB *blob,
					    struct lsa_BinaryString **_r)
{
	struct lsa_BinaryString *r;

	if (!blob || !_r) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	r = talloc_zero(mem_ctx, struct lsa_BinaryString);
	if (!r) {
		return NT_STATUS_NO_MEMORY;
	}

	r->array = talloc_zero_array(mem_ctx, uint16_t, blob->length/2);
	if (!r->array) {
		return NT_STATUS_NO_MEMORY;
	}
	memcpy(r->array, blob->data, blob->length);
	r->size = blob->length;
	r->length = blob->length;

	if (!r->array) {
		return NT_STATUS_NO_MEMORY;
	}

	*_r = r;

	return NT_STATUS_OK;
}

/*************************************************************************
 *************************************************************************/

static struct samr_LogonHours get_logon_hours_from_pdb(TALLOC_CTX *mem_ctx,
						       struct samu *pw)
{
	struct samr_LogonHours hours;
	const int units_per_week = 168;

	ZERO_STRUCT(hours);
	hours.bits = talloc_array(mem_ctx, uint8_t, units_per_week);
	if (!hours.bits) {
		return hours;
	}

	hours.units_per_week = units_per_week;
	memset(hours.bits, 0xFF, units_per_week);

	if (pdb_get_hours(pw)) {
		memcpy(hours.bits, pdb_get_hours(pw),
		       MIN(pdb_get_hours_len(pw), units_per_week));
	}

	return hours;
}

/*************************************************************************
 get_user_info_1.
 *************************************************************************/

static NTSTATUS get_user_info_1(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo1 *r,
				struct samu *pw,
				struct dom_sid *domain_sid)
{
	const struct dom_sid *sid_group;
	uint32_t primary_gid;

	become_root();
	sid_group = pdb_get_group_sid(pw);
	unbecome_root();

	if (!sid_peek_check_rid(domain_sid, sid_group, &primary_gid)) {
		struct dom_sid_buf buf1, buf2;

		DEBUG(0, ("get_user_info_1: User %s has Primary Group SID %s, \n"
			  "which conflicts with the domain sid %s.  Failing operation.\n",
			  pdb_get_username(pw),
			  dom_sid_str_buf(sid_group, &buf1),
			  dom_sid_str_buf(domain_sid, &buf2)));
		return NT_STATUS_UNSUCCESSFUL;
	}

	r->account_name.string		= talloc_strdup(mem_ctx, pdb_get_username(pw));
	r->full_name.string		= talloc_strdup(mem_ctx, pdb_get_fullname(pw));
	r->primary_gid			= primary_gid;
	r->description.string		= talloc_strdup(mem_ctx, pdb_get_acct_desc(pw));
	r->comment.string		= talloc_strdup(mem_ctx, pdb_get_comment(pw));

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_2.
 *************************************************************************/

static NTSTATUS get_user_info_2(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo2 *r,
				struct samu *pw)
{
	r->comment.string		= talloc_strdup(mem_ctx, pdb_get_comment(pw));
	r->reserved.string		= NULL;
	r->country_code			= pdb_get_country_code(pw);
	r->code_page			= pdb_get_code_page(pw);

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_3.
 *************************************************************************/

static NTSTATUS get_user_info_3(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo3 *r,
				struct samu *pw,
				struct dom_sid *domain_sid)
{
	const struct dom_sid *sid_user, *sid_group;
	uint32_t rid, primary_gid;
	struct dom_sid_buf buf1, buf2;

	sid_user = pdb_get_user_sid(pw);

	if (!sid_peek_check_rid(domain_sid, sid_user, &rid)) {
		DEBUG(0, ("get_user_info_3: User %s has SID %s, \nwhich conflicts with "
			  "the domain sid %s.  Failing operation.\n",
			  pdb_get_username(pw),
			  dom_sid_str_buf(sid_user, &buf1),
			  dom_sid_str_buf(domain_sid, &buf2)));
		return NT_STATUS_UNSUCCESSFUL;
	}

	become_root();
	sid_group = pdb_get_group_sid(pw);
	unbecome_root();

	if (!sid_peek_check_rid(domain_sid, sid_group, &primary_gid)) {
		DEBUG(0, ("get_user_info_3: User %s has Primary Group SID %s, \n"
			  "which conflicts with the domain sid %s.  Failing operation.\n",
			  pdb_get_username(pw),
			  dom_sid_str_buf(sid_group, &buf1),
			  dom_sid_str_buf(domain_sid, &buf2)));
		return NT_STATUS_UNSUCCESSFUL;
	}

	unix_to_nt_time(&r->last_logon, pdb_get_logon_time(pw));
	unix_to_nt_time(&r->last_logoff, pdb_get_logoff_time(pw));
	unix_to_nt_time(&r->last_password_change, pdb_get_pass_last_set_time(pw));
	unix_to_nt_time(&r->allow_password_change, pdb_get_pass_can_change_time(pw));
	unix_to_nt_time(&r->force_password_change, pdb_get_pass_must_change_time(pw));

	r->account_name.string	= talloc_strdup(mem_ctx, pdb_get_username(pw));
	r->full_name.string	= talloc_strdup(mem_ctx, pdb_get_fullname(pw));
	r->home_directory.string= talloc_strdup(mem_ctx, pdb_get_homedir(pw));
	r->home_drive.string	= talloc_strdup(mem_ctx, pdb_get_dir_drive(pw));
	r->logon_script.string	= talloc_strdup(mem_ctx, pdb_get_logon_script(pw));
	r->profile_path.string	= talloc_strdup(mem_ctx, pdb_get_profile_path(pw));
	r->workstations.string	= talloc_strdup(mem_ctx, pdb_get_workstations(pw));

	r->logon_hours		= get_logon_hours_from_pdb(mem_ctx, pw);
	r->rid			= rid;
	r->primary_gid		= primary_gid;
	r->acct_flags		= pdb_get_acct_ctrl(pw);
	r->bad_password_count	= pdb_get_bad_password_count(pw);
	r->logon_count		= pdb_get_logon_count(pw);

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_4.
 *************************************************************************/

static NTSTATUS get_user_info_4(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo4 *r,
				struct samu *pw)
{
	r->logon_hours		= get_logon_hours_from_pdb(mem_ctx, pw);

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_5.
 *************************************************************************/

static NTSTATUS get_user_info_5(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo5 *r,
				struct samu *pw,
				struct dom_sid *domain_sid)
{
	const struct dom_sid *sid_user, *sid_group;
	uint32_t rid, primary_gid;
	struct dom_sid_buf buf1, buf2;

	sid_user = pdb_get_user_sid(pw);

	if (!sid_peek_check_rid(domain_sid, sid_user, &rid)) {
		DEBUG(0, ("get_user_info_5: User %s has SID %s, \nwhich conflicts with "
			  "the domain sid %s.  Failing operation.\n",
			  pdb_get_username(pw),
			  dom_sid_str_buf(sid_user, &buf1),
			  dom_sid_str_buf(domain_sid, &buf2)));
		return NT_STATUS_UNSUCCESSFUL;
	}

	become_root();
	sid_group = pdb_get_group_sid(pw);
	unbecome_root();

	if (!sid_peek_check_rid(domain_sid, sid_group, &primary_gid)) {
		DEBUG(0, ("get_user_info_5: User %s has Primary Group SID %s, \n"
			  "which conflicts with the domain sid %s.  Failing operation.\n",
			  pdb_get_username(pw),
			  dom_sid_str_buf(sid_group, &buf1),
			  dom_sid_str_buf(domain_sid, &buf2)));
		return NT_STATUS_UNSUCCESSFUL;
	}

	unix_to_nt_time(&r->last_logon, pdb_get_logon_time(pw));
	unix_to_nt_time(&r->last_logoff, pdb_get_logoff_time(pw));
	unix_to_nt_time(&r->acct_expiry, pdb_get_kickoff_time(pw));
	unix_to_nt_time(&r->last_password_change, pdb_get_pass_last_set_time(pw));

	r->account_name.string	= talloc_strdup(mem_ctx, pdb_get_username(pw));
	r->full_name.string	= talloc_strdup(mem_ctx, pdb_get_fullname(pw));
	r->home_directory.string= talloc_strdup(mem_ctx, pdb_get_homedir(pw));
	r->home_drive.string	= talloc_strdup(mem_ctx, pdb_get_dir_drive(pw));
	r->logon_script.string	= talloc_strdup(mem_ctx, pdb_get_logon_script(pw));
	r->profile_path.string	= talloc_strdup(mem_ctx, pdb_get_profile_path(pw));
	r->description.string	= talloc_strdup(mem_ctx, pdb_get_acct_desc(pw));
	r->workstations.string	= talloc_strdup(mem_ctx, pdb_get_workstations(pw));

	r->logon_hours		= get_logon_hours_from_pdb(mem_ctx, pw);
	r->rid			= rid;
	r->primary_gid		= primary_gid;
	r->acct_flags		= pdb_get_acct_ctrl(pw);
	r->bad_password_count	= pdb_get_bad_password_count(pw);
	r->logon_count		= pdb_get_logon_count(pw);

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_6.
 *************************************************************************/

static NTSTATUS get_user_info_6(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo6 *r,
				struct samu *pw)
{
	r->account_name.string	= talloc_strdup(mem_ctx, pdb_get_username(pw));
	r->full_name.string	= talloc_strdup(mem_ctx, pdb_get_fullname(pw));

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_7. Safe. Only gives out account_name.
 *************************************************************************/

static NTSTATUS get_user_info_7(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo7 *r,
				struct samu *smbpass)
{
	r->account_name.string = talloc_strdup(mem_ctx, pdb_get_username(smbpass));
	if (!r->account_name.string) {
		return NT_STATUS_NO_MEMORY;
	}

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_8.
 *************************************************************************/

static NTSTATUS get_user_info_8(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo8 *r,
				struct samu *pw)
{
	r->full_name.string	= talloc_strdup(mem_ctx, pdb_get_fullname(pw));

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_9. Only gives out primary group SID.
 *************************************************************************/

static NTSTATUS get_user_info_9(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo9 *r,
				struct samu *smbpass)
{
	r->primary_gid = pdb_get_group_rid(smbpass);

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_10.
 *************************************************************************/

static NTSTATUS get_user_info_10(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo10 *r,
				 struct samu *pw)
{
	r->home_directory.string= talloc_strdup(mem_ctx, pdb_get_homedir(pw));
	r->home_drive.string	= talloc_strdup(mem_ctx, pdb_get_dir_drive(pw));

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_11.
 *************************************************************************/

static NTSTATUS get_user_info_11(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo11 *r,
				 struct samu *pw)
{
	r->logon_script.string	= talloc_strdup(mem_ctx, pdb_get_logon_script(pw));

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_12.
 *************************************************************************/

static NTSTATUS get_user_info_12(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo12 *r,
				 struct samu *pw)
{
	r->profile_path.string	= talloc_strdup(mem_ctx, pdb_get_profile_path(pw));

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_13.
 *************************************************************************/

static NTSTATUS get_user_info_13(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo13 *r,
				 struct samu *pw)
{
	r->description.string	= talloc_strdup(mem_ctx, pdb_get_acct_desc(pw));

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_14.
 *************************************************************************/

static NTSTATUS get_user_info_14(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo14 *r,
				 struct samu *pw)
{
	r->workstations.string	= talloc_strdup(mem_ctx, pdb_get_workstations(pw));

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_16. Safe. Only gives out acb bits.
 *************************************************************************/

static NTSTATUS get_user_info_16(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo16 *r,
				 struct samu *smbpass)
{
	r->acct_flags = pdb_get_acct_ctrl(smbpass);

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_17.
 *************************************************************************/

static NTSTATUS get_user_info_17(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo17 *r,
				 struct samu *pw)
{
	unix_to_nt_time(&r->acct_expiry, pdb_get_kickoff_time(pw));

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_18. OK - this is the killer as it gives out password info.
 Ensure that this is only allowed on an encrypted connection with a root
 user. JRA.
 *************************************************************************/

static NTSTATUS get_user_info_18(struct pipes_struct *p,
				 TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo18 *r,
				 struct dom_sid *user_sid)
{
	struct dcesrv_call_state *dce_call = p->dce_call;
	struct auth_session_info *session_info =
		dcesrv_call_session_info(dce_call);
	struct samu *smbpass=NULL;
	bool ret;
	const uint8_t *nt_pass = NULL;
	const uint8_t *lm_pass = NULL;

	ZERO_STRUCTP(r);

	if (p->transport != NCALRPC) {
		return NT_STATUS_INVALID_INFO_CLASS;
	}

	if (!security_token_is_system(session_info->security_token)) {
		return NT_STATUS_ACCESS_DENIED;
	}

	/*
	 * Do *NOT* do become_root()/unbecome_root() here ! JRA.
	 */

	if ( !(smbpass = samu_new( mem_ctx )) ) {
		return NT_STATUS_NO_MEMORY;
	}

	ret = pdb_getsampwsid(smbpass, user_sid);

	if (ret == False) {
		struct dom_sid_buf buf;
		DEBUG(4, ("User %s not found\n",
			  dom_sid_str_buf(user_sid, &buf)));
		TALLOC_FREE(smbpass);
		return root_mode() ? NT_STATUS_NO_SUCH_USER : NT_STATUS_ACCESS_DENIED;
	}

	DEBUG(3,("User:[%s] 0x%x\n", pdb_get_username(smbpass), pdb_get_acct_ctrl(smbpass) ));

	if ( pdb_get_acct_ctrl(smbpass) & ACB_DISABLED) {
		TALLOC_FREE(smbpass);
		return NT_STATUS_ACCOUNT_DISABLED;
	}

	lm_pass = pdb_get_lanman_passwd(smbpass);
	if (lm_pass != NULL) {
		memcpy(r->lm_pwd.hash, lm_pass, 16);
		r->lm_pwd_active = true;
	}

	nt_pass = pdb_get_nt_passwd(smbpass);
	if (nt_pass != NULL) {
		memcpy(r->nt_pwd.hash, nt_pass, 16);
		r->nt_pwd_active = true;
	}
	r->password_expired = 0; /* FIXME */

	TALLOC_FREE(smbpass);

	return NT_STATUS_OK;
}

/*************************************************************************
 get_user_info_20
 *************************************************************************/

static NTSTATUS get_user_info_20(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo20 *r,
				 struct samu *sampass)
{
	const char *munged_dial = NULL;
	DATA_BLOB blob;
	NTSTATUS status;
	struct lsa_BinaryString *parameters = NULL;

	ZERO_STRUCTP(r);

	munged_dial = pdb_get_munged_dial(sampass);

	DEBUG(3,("User:[%s] has [%s] (length: %d)\n", pdb_get_username(sampass),
		munged_dial, (int)strlen(munged_dial)));

	if (munged_dial) {
		blob = base64_decode_data_blob(munged_dial);
	} else {
		blob = data_blob_string_const_null("");
	}

	status = init_samr_parameters_string(mem_ctx, &blob, &parameters);
	data_blob_free(&blob);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	r->parameters = *parameters;

	return NT_STATUS_OK;
}


/*************************************************************************
 get_user_info_21
 *************************************************************************/

static NTSTATUS get_user_info_21(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo21 *r,
				 struct samu *pw,
				 struct dom_sid *domain_sid,
				 uint32_t acc_granted)
{
	NTSTATUS status;
	const struct dom_sid *sid_user, *sid_group;
	uint32_t rid, primary_gid;
	NTTIME force_password_change;
	time_t must_change_time;
	struct lsa_BinaryString *parameters = NULL;
	const char *munged_dial = NULL;
	DATA_BLOB blob;
	struct dom_sid_buf buf1, buf2;

	ZERO_STRUCTP(r);

	sid_user = pdb_get_user_sid(pw);

	if (!sid_peek_check_rid(domain_sid, sid_user, &rid)) {
		DEBUG(0, ("get_user_info_21: User %s has SID %s, \nwhich conflicts with "
			  "the domain sid %s.  Failing operation.\n",
			  pdb_get_username(pw),
			  dom_sid_str_buf(sid_user, &buf1),
			  dom_sid_str_buf(domain_sid, &buf2)));
		return NT_STATUS_UNSUCCESSFUL;
	}

	become_root();
	sid_group = pdb_get_group_sid(pw);
	unbecome_root();

	if (!sid_peek_check_rid(domain_sid, sid_group, &primary_gid)) {
		DEBUG(0, ("get_user_info_21: User %s has Primary Group SID %s, \n"
			  "which conflicts with the domain sid %s.  Failing operation.\n",
			  pdb_get_username(pw),
			  dom_sid_str_buf(sid_group, &buf1),
			  dom_sid_str_buf(domain_sid, &buf2)));
		return NT_STATUS_UNSUCCESSFUL;
	}

	unix_to_nt_time(&r->last_logon, pdb_get_logon_time(pw));
	unix_to_nt_time(&r->last_logoff, pdb_get_logoff_time(pw));
	unix_to_nt_time(&r->acct_expiry, pdb_get_kickoff_time(pw));
	unix_to_nt_time(&r->last_password_change, pdb_get_pass_last_set_time(pw));
	unix_to_nt_time(&r->allow_password_change, pdb_get_pass_can_change_time(pw));

	must_change_time = pdb_get_pass_must_change_time(pw);
	if (pdb_is_password_change_time_max(must_change_time)) {
		unix_to_nt_time_abs(&force_password_change, must_change_time);
	} else {
		unix_to_nt_time(&force_password_change, must_change_time);
	}

	munged_dial = pdb_get_munged_dial(pw);
	if (munged_dial) {
		blob = base64_decode_data_blob(munged_dial);
	} else {
		blob = data_blob_string_const_null("");
	}

	status = init_samr_parameters_string(mem_ctx, &blob, &parameters);
	data_blob_free(&blob);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	r->force_password_change	= force_password_change;

	r->account_name.string		= talloc_strdup(mem_ctx, pdb_get_username(pw));
	r->full_name.string		= talloc_strdup(mem_ctx, pdb_get_fullname(pw));
	r->home_directory.string	= talloc_strdup(mem_ctx, pdb_get_homedir(pw));
	r->home_drive.string		= talloc_strdup(mem_ctx, pdb_get_dir_drive(pw));
	r->logon_script.string		= talloc_strdup(mem_ctx, pdb_get_logon_script(pw));
	r->profile_path.string		= talloc_strdup(mem_ctx, pdb_get_profile_path(pw));
	r->description.string		= talloc_strdup(mem_ctx, pdb_get_acct_desc(pw));
	r->workstations.string		= talloc_strdup(mem_ctx, pdb_get_workstations(pw));
	r->comment.string		= talloc_strdup(mem_ctx, pdb_get_comment(pw));

	r->logon_hours			= get_logon_hours_from_pdb(mem_ctx, pw);
	r->parameters			= *parameters;
	r->rid				= rid;
	r->primary_gid			= primary_gid;
	r->acct_flags			= pdb_get_acct_ctrl(pw);
	r->bad_password_count		= pdb_get_bad_password_count(pw);
	r->logon_count			= pdb_get_logon_count(pw);
	r->fields_present		= pdb_build_fields_present(pw);
	r->password_expired		= (pdb_get_pass_must_change_time(pw) == 0) ?
						PASS_MUST_CHANGE_AT_NEXT_LOGON : 0;
	r->country_code			= pdb_get_country_code(pw);
	r->code_page			= pdb_get_code_page(pw);
	r->lm_password_set		= 0;
	r->nt_password_set		= 0;

#if 0

	/*
	  Look at a user on a real NT4 PDC with usrmgr, press
	  'ok'. Then you will see that fields_present is set to
	  0x08f827fa. Look at the user immediately after that again,
	  and you will see that 0x00fffff is returned. This solves
	  the problem that you get access denied after having looked
	  at the user.
	  -- Volker
	*/

#endif


	return NT_STATUS_OK;
}

/*******************************************************************
 _samr_QueryUserInfo
 ********************************************************************/

NTSTATUS _samr_QueryUserInfo(struct pipes_struct *p,
			     struct samr_QueryUserInfo *r)
{
	NTSTATUS status;
	union samr_UserInfo *user_info = NULL;
	struct samr_info *uinfo;
	struct dom_sid domain_sid;
	uint32_t rid;
	bool ret = false;
	struct samu *pwd = NULL;
	uint32_t acc_required, acc_granted;
	struct dom_sid_buf buf;

	switch (r->in.level) {
	case 1: /* UserGeneralInformation */
		/* USER_READ_GENERAL */
		acc_required = SAMR_USER_ACCESS_GET_NAME_ETC;
		break;
	case 2: /* UserPreferencesInformation */
		/* USER_READ_PREFERENCES | USER_READ_GENERAL */
		acc_required = SAMR_USER_ACCESS_GET_LOCALE |
			       SAMR_USER_ACCESS_GET_NAME_ETC;
		break;
	case 3: /* UserLogonInformation */
		/* USER_READ_GENERAL | USER_READ_PREFERENCES | USER_READ_LOGON | USER_READ_ACCOUNT */
		acc_required = SAMR_USER_ACCESS_GET_NAME_ETC |
			       SAMR_USER_ACCESS_GET_LOCALE |
			       SAMR_USER_ACCESS_GET_LOGONINFO |
			       SAMR_USER_ACCESS_GET_ATTRIBUTES;
		break;
	case 4: /* UserLogonHoursInformation */
		/* USER_READ_LOGON */
		acc_required = SAMR_USER_ACCESS_GET_LOGONINFO;
		break;
	case 5: /* UserAccountInformation */
		/* USER_READ_GENERAL | USER_READ_PREFERENCES | USER_READ_LOGON | USER_READ_ACCOUNT */
		acc_required = SAMR_USER_ACCESS_GET_NAME_ETC |
			       SAMR_USER_ACCESS_GET_LOCALE |
			       SAMR_USER_ACCESS_GET_LOGONINFO |
			       SAMR_USER_ACCESS_GET_ATTRIBUTES;
		break;
	case 6: /* UserNameInformation */
	case 7: /* UserAccountNameInformation */
	case 8: /* UserFullNameInformation */
	case 9: /* UserPrimaryGroupInformation */
	case 13: /* UserAdminCommentInformation */
		/* USER_READ_GENERAL */
		acc_required = SAMR_USER_ACCESS_GET_NAME_ETC;
		break;
	case 10: /* UserHomeInformation */
	case 11: /* UserScriptInformation */
	case 12: /* UserProfileInformation */
	case 14: /* UserWorkStationsInformation */
		/* USER_READ_LOGON */
		acc_required = SAMR_USER_ACCESS_GET_LOGONINFO;
		break;
	case 16: /* UserControlInformation */
	case 17: /* UserExpiresInformation */
	case 20: /* UserParametersInformation */
		/* USER_READ_ACCOUNT */
		acc_required = SAMR_USER_ACCESS_GET_ATTRIBUTES;
		break;
	case 21: /* UserAllInformation */
		/* FIXME! - gd */
		acc_required = SAMR_USER_ACCESS_GET_ATTRIBUTES;
		break;
	case 18: /* UserInternal1Information */
		/* FIXME! - gd */
		acc_required = SAMR_USER_ACCESS_GET_ATTRIBUTES;
		break;
	case 23: /* UserInternal4Information */
	case 24: /* UserInternal4InformationNew */
	case 25: /* UserInternal4InformationNew */
	case 26: /* UserInternal5InformationNew */
	default:
		return NT_STATUS_INVALID_INFO_CLASS;
		break;
	}

	uinfo = samr_policy_handle_find(p,
					r->in.user_handle,
					SAMR_HANDLE_USER,
					acc_required,
					&acc_granted,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	domain_sid = uinfo->sid;

	sid_split_rid(&domain_sid, &rid);

	if (!sid_check_is_in_our_sam(&uinfo->sid))
		return NT_STATUS_OBJECT_TYPE_MISMATCH;

	DEBUG(5,("_samr_QueryUserInfo: sid:%s\n",
		 dom_sid_str_buf(&uinfo->sid, &buf)));

	user_info = talloc_zero(p->mem_ctx, union samr_UserInfo);
	if (!user_info) {
		return NT_STATUS_NO_MEMORY;
	}

	DEBUG(5,("_samr_QueryUserInfo: user info level: %d\n", r->in.level));

	if (!(pwd = samu_new(p->mem_ctx))) {
		return NT_STATUS_NO_MEMORY;
	}

	become_root();
	ret = pdb_getsampwsid(pwd, &uinfo->sid);
	unbecome_root();

	if (ret == false) {
		DEBUG(4,("User %s not found\n",
			 dom_sid_str_buf(&uinfo->sid, &buf)));
		TALLOC_FREE(pwd);
		return NT_STATUS_NO_SUCH_USER;
	}

	DEBUG(3,("User:[%s]\n", pdb_get_username(pwd)));

	samr_clear_sam_passwd(pwd);

	switch (r->in.level) {
	case 1:
		status = get_user_info_1(p->mem_ctx, &user_info->info1, pwd, &domain_sid);
		break;
	case 2:
		status = get_user_info_2(p->mem_ctx, &user_info->info2, pwd);
		break;
	case 3:
		status = get_user_info_3(p->mem_ctx, &user_info->info3, pwd, &domain_sid);
		break;
	case 4:
		status = get_user_info_4(p->mem_ctx, &user_info->info4, pwd);
		break;
	case 5:
		status = get_user_info_5(p->mem_ctx, &user_info->info5, pwd, &domain_sid);
		break;
	case 6:
		status = get_user_info_6(p->mem_ctx, &user_info->info6, pwd);
		break;
	case 7:
		status = get_user_info_7(p->mem_ctx, &user_info->info7, pwd);
		break;
	case 8:
		status = get_user_info_8(p->mem_ctx, &user_info->info8, pwd);
		break;
	case 9:
		status = get_user_info_9(p->mem_ctx, &user_info->info9, pwd);
		break;
	case 10:
		status = get_user_info_10(p->mem_ctx, &user_info->info10, pwd);
		break;
	case 11:
		status = get_user_info_11(p->mem_ctx, &user_info->info11, pwd);
		break;
	case 12:
		status = get_user_info_12(p->mem_ctx, &user_info->info12, pwd);
		break;
	case 13:
		status = get_user_info_13(p->mem_ctx, &user_info->info13, pwd);
		break;
	case 14:
		status = get_user_info_14(p->mem_ctx, &user_info->info14, pwd);
		break;
	case 16:
		status = get_user_info_16(p->mem_ctx, &user_info->info16, pwd);
		break;
	case 17:
		status = get_user_info_17(p->mem_ctx, &user_info->info17, pwd);
		break;
	case 18:
		/* level 18 is special */
		status = get_user_info_18(p, p->mem_ctx, &user_info->info18,
					  &uinfo->sid);
		break;
	case 20:
		status = get_user_info_20(p->mem_ctx, &user_info->info20, pwd);
		break;
	case 21:
		status = get_user_info_21(p->mem_ctx, &user_info->info21, pwd, &domain_sid, acc_granted);
		break;
	default:
		status = NT_STATUS_INVALID_INFO_CLASS;
		break;
	}

	if (!NT_STATUS_IS_OK(status)) {
		goto done;
	}

	*r->out.info = user_info;

 done:
	TALLOC_FREE(pwd);

	DEBUG(5,("_samr_QueryUserInfo: %d\n", __LINE__));

	return status;
}

/****************************************************************
****************************************************************/

NTSTATUS _samr_QueryUserInfo2(struct pipes_struct *p,
			      struct samr_QueryUserInfo2 *r)
{
	struct samr_QueryUserInfo u;

	u.in.user_handle	= r->in.user_handle;
	u.in.level		= r->in.level;
	u.out.info		= r->out.info;

	return _samr_QueryUserInfo(p, &u);
}

/*******************************************************************
 _samr_GetGroupsForUser
 ********************************************************************/

NTSTATUS _samr_GetGroupsForUser(struct pipes_struct *p,
				struct samr_GetGroupsForUser *r)
{
	struct samr_info *uinfo;
	struct samu *sam_pass=NULL;
	struct dom_sid *sids;
	struct samr_RidWithAttribute dom_gid;
	struct samr_RidWithAttribute *gids = NULL;
	uint32_t primary_group_rid;
	uint32_t num_groups = 0;
	gid_t *unix_gids;
	uint32_t i, num_gids;
	bool ret;
	NTSTATUS result;
	bool success = False;
	struct dom_sid_buf buf;

	struct samr_RidWithAttributeArray *rids = NULL;

	/*
	 * from the SID in the request:
	 * we should send back the list of DOMAIN GROUPS
	 * the user is a member of
	 *
	 * and only the DOMAIN GROUPS
	 * no ALIASES !!! neither aliases of the domain
	 * nor aliases of the builtin SID
	 *
	 * JFM, 12/2/2001
	 */

	DEBUG(5,("_samr_GetGroupsForUser: %d\n", __LINE__));

	uinfo = samr_policy_handle_find(p,
					r->in.user_handle,
					SAMR_HANDLE_USER,
					SAMR_USER_ACCESS_GET_GROUPS,
					NULL,
					&result);
	if (!NT_STATUS_IS_OK(result)) {
		return result;
	}

	rids = talloc_zero(p->mem_ctx, struct samr_RidWithAttributeArray);
	if (!rids) {
		return NT_STATUS_NO_MEMORY;
	}

	if (!sid_check_is_in_our_sam(&uinfo->sid))
		return NT_STATUS_OBJECT_TYPE_MISMATCH;

        if ( !(sam_pass = samu_new( p->mem_ctx )) ) {
                return NT_STATUS_NO_MEMORY;
        }

	become_root();
	ret = pdb_getsampwsid(sam_pass, &uinfo->sid);
	unbecome_root();

	if (!ret) {
		DEBUG(10, ("pdb_getsampwsid failed for %s\n",
			   dom_sid_str_buf(&uinfo->sid, &buf)));
		return NT_STATUS_NO_SUCH_USER;
	}

	sids = NULL;

	/* make both calls inside the root block */
	become_root();
	result = pdb_enum_group_memberships(p->mem_ctx, sam_pass,
					    &sids, &unix_gids, &num_groups);
	if ( NT_STATUS_IS_OK(result) ) {
		success = sid_peek_check_rid(get_global_sam_sid(),
					     pdb_get_group_sid(sam_pass),
					     &primary_group_rid);
	}
	unbecome_root();

	if (!NT_STATUS_IS_OK(result)) {
		DEBUG(10, ("pdb_enum_group_memberships failed for %s\n",
			   dom_sid_str_buf(&uinfo->sid, &buf)));
		return result;
	}

	if ( !success ) {
		DEBUG(5, ("Group sid %s for user %s not in our domain\n",
			  dom_sid_str_buf(pdb_get_group_sid(sam_pass), &buf),
			  pdb_get_username(sam_pass)));
		TALLOC_FREE(sam_pass);
		return NT_STATUS_INTERNAL_DB_CORRUPTION;
	}

	gids = NULL;
	num_gids = 0;

	dom_gid.attributes = SE_GROUP_DEFAULT_FLAGS;
	dom_gid.rid = primary_group_rid;
	ADD_TO_ARRAY(p->mem_ctx, struct samr_RidWithAttribute, dom_gid, &gids, &num_gids);

	for (i=0; i<num_groups; i++) {

		if (!sid_peek_check_rid(get_global_sam_sid(),
					&(sids[i]), &dom_gid.rid)) {
			DEBUG(10, ("Found sid %s not in our domain\n",
				   dom_sid_str_buf(&sids[i], &buf)));
			continue;
		}

		if (dom_gid.rid == primary_group_rid) {
			/* We added the primary group directly from the
			 * sam_account. The other SIDs are unique from
			 * enum_group_memberships */
			continue;
		}

		ADD_TO_ARRAY(p->mem_ctx, struct samr_RidWithAttribute, dom_gid, &gids, &num_gids);
	}

	rids->count = num_gids;
	rids->rids = gids;

	*r->out.rids = rids;

	DEBUG(5,("_samr_GetGroupsForUser: %d\n", __LINE__));

	return result;
}

/*******************************************************************
 ********************************************************************/

static uint32_t samr_get_server_role(void)
{
	uint32_t role = ROLE_DOMAIN_PDC;

	if (lp_server_role() == ROLE_DOMAIN_BDC) {
		role = ROLE_DOMAIN_BDC;
	}

	return role;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS query_dom_info_1(TALLOC_CTX *mem_ctx,
				 struct samr_DomInfo1 *r)
{
	const struct loadparm_substitution *lp_sub =
		loadparm_s3_global_substitution();
	uint32_t account_policy_temp;
	time_t u_expire, u_min_age;

	become_root();

	/* AS ROOT !!! */

	pdb_get_account_policy(PDB_POLICY_MIN_PASSWORD_LEN, &account_policy_temp);
	r->min_password_length = account_policy_temp;

	pdb_get_account_policy(PDB_POLICY_PASSWORD_HISTORY, &account_policy_temp);
	r->password_history_length = account_policy_temp;

	pdb_get_account_policy(PDB_POLICY_USER_MUST_LOGON_TO_CHG_PASS,
			       &r->password_properties);

	pdb_get_account_policy(PDB_POLICY_MAX_PASSWORD_AGE, &account_policy_temp);
	u_expire = account_policy_temp;

	pdb_get_account_policy(PDB_POLICY_MIN_PASSWORD_AGE, &account_policy_temp);
	u_min_age = account_policy_temp;

	/* !AS ROOT */

	unbecome_root();

	unix_to_nt_time_abs((NTTIME *)&r->max_password_age, u_expire);
	unix_to_nt_time_abs((NTTIME *)&r->min_password_age, u_min_age);

	if (lp_check_password_script(talloc_tos(), lp_sub) && *lp_check_password_script(talloc_tos(), lp_sub)){
		r->password_properties |= DOMAIN_PASSWORD_COMPLEX;
	}

	return NT_STATUS_OK;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS query_dom_info_2(TALLOC_CTX *mem_ctx,
				 struct samr_DomGeneralInformation *r,
				 struct samr_info *dinfo)
{
	const struct loadparm_substitution *lp_sub =
		loadparm_s3_global_substitution();
	uint32_t u_logout;
	time_t seq_num;

	become_root();

	/* AS ROOT !!! */

	r->num_users	= count_sam_users(dinfo->disp_info, ACB_NORMAL);
	r->num_groups	= count_sam_groups(dinfo->disp_info);
	r->num_aliases	= count_sam_aliases(dinfo->disp_info);

	pdb_get_account_policy(PDB_POLICY_TIME_TO_LOGOUT, &u_logout);

	unix_to_nt_time_abs(&r->force_logoff_time, u_logout);

	if (!pdb_get_seq_num(&seq_num)) {
		seq_num = time(NULL);
	}

	/* !AS ROOT */

	unbecome_root();

	r->oem_information.string	= lp_server_string(r, lp_sub);
	r->domain_name.string		= lp_workgroup();
	r->primary.string		= lp_netbios_name();
	r->sequence_num			= seq_num;
	r->domain_server_state		= DOMAIN_SERVER_ENABLED;
	r->role				= (enum samr_Role) samr_get_server_role();
	r->unknown3			= 1;

	return NT_STATUS_OK;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS query_dom_info_3(TALLOC_CTX *mem_ctx,
				 struct samr_DomInfo3 *r)
{
	uint32_t u_logout;

	become_root();

	/* AS ROOT !!! */

	{
		uint32_t ul;
		pdb_get_account_policy(PDB_POLICY_TIME_TO_LOGOUT, &ul);
		u_logout = (time_t)ul;
	}

	/* !AS ROOT */

	unbecome_root();

	unix_to_nt_time_abs(&r->force_logoff_time, u_logout);

	return NT_STATUS_OK;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS query_dom_info_4(TALLOC_CTX *mem_ctx,
				 struct samr_DomOEMInformation *r)
{
	const struct loadparm_substitution *lp_sub =
		loadparm_s3_global_substitution();

	r->oem_information.string = lp_server_string(r, lp_sub);

	return NT_STATUS_OK;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS query_dom_info_5(TALLOC_CTX *mem_ctx,
				 struct samr_DomInfo5 *r)
{
	r->domain_name.string = get_global_sam_name();

	return NT_STATUS_OK;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS query_dom_info_6(TALLOC_CTX *mem_ctx,
				 struct samr_DomInfo6 *r)
{
	/* NT returns its own name when a PDC. win2k and later
	 * only the name of the PDC if itself is a BDC (samba4
	 * idl) */
	r->primary.string = lp_netbios_name();

	return NT_STATUS_OK;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS query_dom_info_7(TALLOC_CTX *mem_ctx,
				 struct samr_DomInfo7 *r)
{
	r->role = (enum samr_Role) samr_get_server_role();

	return NT_STATUS_OK;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS query_dom_info_8(TALLOC_CTX *mem_ctx,
				 struct samr_DomInfo8 *r)
{
	time_t seq_num;

	become_root();

	/* AS ROOT !!! */

	if (!pdb_get_seq_num(&seq_num)) {
		seq_num = time(NULL);
	}

	/* !AS ROOT */

	unbecome_root();

	r->sequence_num = seq_num;
	r->domain_create_time = 0;

	return NT_STATUS_OK;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS query_dom_info_9(TALLOC_CTX *mem_ctx,
				 struct samr_DomInfo9 *r)
{
	r->domain_server_state = DOMAIN_SERVER_ENABLED;

	return NT_STATUS_OK;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS query_dom_info_11(TALLOC_CTX *mem_ctx,
				  struct samr_DomGeneralInformation2 *r,
				  struct samr_info *dinfo)
{
	NTSTATUS status;
	uint32_t account_policy_temp;
	time_t u_lock_duration, u_reset_time;

	status = query_dom_info_2(mem_ctx, &r->general, dinfo);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/* AS ROOT !!! */

	become_root();

	pdb_get_account_policy(PDB_POLICY_LOCK_ACCOUNT_DURATION, &account_policy_temp);
	u_lock_duration = account_policy_temp;
	if (u_lock_duration != -1) {
		u_lock_duration *= 60;
	}

	pdb_get_account_policy(PDB_POLICY_RESET_COUNT_TIME, &account_policy_temp);
	u_reset_time = account_policy_temp * 60;

	pdb_get_account_policy(PDB_POLICY_BAD_ATTEMPT_LOCKOUT, &account_policy_temp);
	r->lockout_threshold = account_policy_temp;

	/* !AS ROOT */

	unbecome_root();

	unix_to_nt_time_abs(&r->lockout_duration, u_lock_duration);
	unix_to_nt_time_abs(&r->lockout_window, u_reset_time);

	return NT_STATUS_OK;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS query_dom_info_12(TALLOC_CTX *mem_ctx,
				  struct samr_DomInfo12 *r)
{
	uint32_t account_policy_temp;
	time_t u_lock_duration, u_reset_time;

	become_root();

	/* AS ROOT !!! */

	pdb_get_account_policy(PDB_POLICY_LOCK_ACCOUNT_DURATION, &account_policy_temp);
	u_lock_duration = account_policy_temp;
	if (u_lock_duration != -1) {
		u_lock_duration *= 60;
	}

	pdb_get_account_policy(PDB_POLICY_RESET_COUNT_TIME, &account_policy_temp);
	u_reset_time = account_policy_temp * 60;

	pdb_get_account_policy(PDB_POLICY_BAD_ATTEMPT_LOCKOUT, &account_policy_temp);
	r->lockout_threshold = account_policy_temp;

	/* !AS ROOT */

	unbecome_root();

	unix_to_nt_time_abs(&r->lockout_duration, u_lock_duration);
	unix_to_nt_time_abs(&r->lockout_window, u_reset_time);

	return NT_STATUS_OK;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS query_dom_info_13(TALLOC_CTX *mem_ctx,
				  struct samr_DomInfo13 *r)
{
	time_t seq_num;

	become_root();

	/* AS ROOT !!! */

	if (!pdb_get_seq_num(&seq_num)) {
		seq_num = time(NULL);
	}

	/* !AS ROOT */

	unbecome_root();

	r->sequence_num = seq_num;
	r->domain_create_time = 0;
	r->modified_count_at_last_promotion = 0;

	return NT_STATUS_OK;
}

/*******************************************************************
 _samr_QueryDomainInfo
 ********************************************************************/

NTSTATUS _samr_QueryDomainInfo(struct pipes_struct *p,
			       struct samr_QueryDomainInfo *r)
{
	NTSTATUS status = NT_STATUS_OK;
	struct samr_info *dinfo;
	union samr_DomainInfo *dom_info;

	uint32_t acc_required;

	DEBUG(5,("_samr_QueryDomainInfo: %d\n", __LINE__));

	switch (r->in.level) {
	case 1: /* DomainPasswordInformation */
	case 12: /* DomainLockoutInformation */
		/* DOMAIN_READ_PASSWORD_PARAMETERS */
		acc_required = SAMR_DOMAIN_ACCESS_LOOKUP_INFO_1;
		break;
	case 11: /* DomainGeneralInformation2 */
		/* DOMAIN_READ_PASSWORD_PARAMETERS |
		 * DOMAIN_READ_OTHER_PARAMETERS */
		acc_required = SAMR_DOMAIN_ACCESS_LOOKUP_INFO_1 |
			       SAMR_DOMAIN_ACCESS_LOOKUP_INFO_2;
		break;
	case 2: /* DomainGeneralInformation */
	case 3: /* DomainLogoffInformation */
	case 4: /* DomainOemInformation */
	case 5: /* DomainReplicationInformation */
	case 6: /* DomainReplicationInformation */
	case 7: /* DomainServerRoleInformation */
	case 8: /* DomainModifiedInformation */
	case 9: /* DomainStateInformation */
	case 10: /* DomainUasInformation */
	case 13: /* DomainModifiedInformation2 */
		/* DOMAIN_READ_OTHER_PARAMETERS */
		acc_required = SAMR_DOMAIN_ACCESS_LOOKUP_INFO_2;
		break;
	default:
		return NT_STATUS_INVALID_INFO_CLASS;
	}

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					acc_required,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	dom_info = talloc_zero(p->mem_ctx, union samr_DomainInfo);
	if (!dom_info) {
		return NT_STATUS_NO_MEMORY;
	}

	switch (r->in.level) {
		case 1:
			status = query_dom_info_1(p->mem_ctx, &dom_info->info1);
			break;
		case 2:
			status = query_dom_info_2(p->mem_ctx, &dom_info->general, dinfo);
			break;
		case 3:
			status = query_dom_info_3(p->mem_ctx, &dom_info->info3);
			break;
		case 4:
			status = query_dom_info_4(p->mem_ctx, &dom_info->oem);
			break;
		case 5:
			status = query_dom_info_5(p->mem_ctx, &dom_info->info5);
			break;
		case 6:
			status = query_dom_info_6(p->mem_ctx, &dom_info->info6);
			break;
		case 7:
			status = query_dom_info_7(p->mem_ctx, &dom_info->info7);
			break;
		case 8:
			status = query_dom_info_8(p->mem_ctx, &dom_info->info8);
			break;
		case 9:
			status = query_dom_info_9(p->mem_ctx, &dom_info->info9);
			break;
		case 11:
			status = query_dom_info_11(p->mem_ctx, &dom_info->general2, dinfo);
			break;
		case 12:
			status = query_dom_info_12(p->mem_ctx, &dom_info->info12);
			break;
		case 13:
			status = query_dom_info_13(p->mem_ctx, &dom_info->info13);
			break;
		default:
			return NT_STATUS_INVALID_INFO_CLASS;
	}

	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	*r->out.info = dom_info;

	DEBUG(5,("_samr_QueryDomainInfo: %d\n", __LINE__));

	return status;
}

/* W2k3 seems to use the same check for all 3 objects that can be created via
 * SAMR, if you try to create for example "Dialup" as an alias it says
 * "NT_STATUS_USER_EXISTS". This is racy, but we can't really lock the user
 * database. */

static NTSTATUS can_create(TALLOC_CTX *mem_ctx, const char *new_name)
{
	enum lsa_SidType type;
	bool result;

	DEBUG(10, ("Checking whether [%s] can be created\n", new_name));

	become_root();
	/* Lookup in our local databases (LOOKUP_NAME_REMOTE not set)
	 * whether the name already exists */
	result = lookup_name(mem_ctx, new_name, LOOKUP_NAME_LOCAL,
			     NULL, NULL, NULL, &type);
	unbecome_root();

	if (!result) {
		DEBUG(10, ("%s does not exist, can create it\n", new_name));
		return NT_STATUS_OK;
	}

	DEBUG(5, ("trying to create %s, exists as %s\n",
		  new_name, sid_type_lookup(type)));

	if (type == SID_NAME_DOM_GRP) {
		return NT_STATUS_GROUP_EXISTS;
	}
	if (type == SID_NAME_ALIAS) {
		return NT_STATUS_ALIAS_EXISTS;
	}

	/* Yes, the default is NT_STATUS_USER_EXISTS */
	return NT_STATUS_USER_EXISTS;
}

/*******************************************************************
 _samr_CreateUser2
 ********************************************************************/

NTSTATUS _samr_CreateUser2(struct pipes_struct *p,
			   struct samr_CreateUser2 *r)
{
	struct dcesrv_call_state *dce_call = p->dce_call;
	struct auth_session_info *session_info =
		dcesrv_call_session_info(dce_call);
	const char *account = NULL;
	struct dom_sid sid;
	uint32_t acb_info = r->in.acct_flags;
	struct samr_info *dinfo;
	NTSTATUS nt_status;
	uint32_t acc_granted;
	struct security_descriptor *psd;
	size_t    sd_size;
	/* check this, when giving away 'add computer to domain' privs */
	uint32_t    des_access = GENERIC_RIGHTS_USER_ALL_ACCESS;
	bool can_add_account = False;

	/* Which privilege is needed to override the ACL? */
	enum sec_privilege needed_priv = SEC_PRIV_INVALID;

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					SAMR_DOMAIN_ACCESS_CREATE_USER,
					NULL,
					&nt_status);
	if (!NT_STATUS_IS_OK(nt_status)) {
		return nt_status;
	}

	if (sid_check_is_builtin(&dinfo->sid)) {
		DEBUG(5,("_samr_CreateUser2: Refusing user create in BUILTIN\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	if (!(acb_info == ACB_NORMAL || acb_info == ACB_DOMTRUST ||
	      acb_info == ACB_WSTRUST || acb_info == ACB_SVRTRUST)) {
		/* Match Win2k, and return NT_STATUS_INVALID_PARAMETER if
		   this parameter is not an account type */
		return NT_STATUS_INVALID_PARAMETER;
	}

	account = r->in.account_name->string;
	if (account == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	nt_status = can_create(p->mem_ctx, account);
	if (!NT_STATUS_IS_OK(nt_status)) {
		return nt_status;
	}

	/* determine which user right we need to check based on the acb_info */

	if (root_mode()) {
		can_add_account = true;
	} else if (acb_info & ACB_WSTRUST) {
		needed_priv = SEC_PRIV_MACHINE_ACCOUNT;
		can_add_account = security_token_has_privilege(
			session_info->security_token, needed_priv);
	} else if (acb_info & ACB_NORMAL &&
		  (account[strlen(account)-1] != '$')) {
		/* usrmgr.exe (and net rpc trustdom add) creates a normal user
		   account for domain trusts and changes the ACB flags later */
		needed_priv = SEC_PRIV_ADD_USERS;
		can_add_account = security_token_has_privilege(
			session_info->security_token, needed_priv);
	} else if (lp_enable_privileges()) {
		/* implicit assumption of a BDC or domain trust account here
		 * (we already check the flags earlier) */
		/* only Domain Admins can add a BDC or domain trust */
		can_add_account = nt_token_check_domain_rid(
			session_info->security_token,
			DOMAIN_RID_ADMINS );
	}

	DEBUG(5, ("_samr_CreateUser2: %s can add this account : %s\n",
		  uidtoname(session_info->unix_token->uid),
		  can_add_account ? "True":"False" ));

	if (!can_add_account) {
		return NT_STATUS_ACCESS_DENIED;
	}

	/********** BEGIN Admin BLOCK **********/

	(void)winbind_off();
	become_root();
	nt_status = pdb_create_user(p->mem_ctx, account, acb_info,
				    r->out.rid);
	unbecome_root();
	(void)winbind_on();

	/********** END Admin BLOCK **********/

	/* now check for failure */

	if ( !NT_STATUS_IS_OK(nt_status) )
		return nt_status;

	/* Get the user's SID */

	sid_compose(&sid, get_global_sam_sid(), *r->out.rid);

	map_max_allowed_access(session_info->security_token,
			       session_info->unix_token,
			       &des_access);

	make_samr_object_sd(p->mem_ctx, &psd, &sd_size, &usr_generic_mapping,
			    &sid, SAMR_USR_RIGHTS_WRITE_PW);
	se_map_generic(&des_access, &usr_generic_mapping);

	/*
	 * JRA - TESTME. We just created this user so we
	 * had rights to create them. Do we need to check
	 * any further access on this object ? Can't we
	 * just assume we have all the rights we need ?
	 */

	nt_status = access_check_object(psd, session_info->security_token,
					needed_priv, SEC_PRIV_INVALID,
					GENERIC_RIGHTS_USER_WRITE, des_access,
		&acc_granted, "_samr_CreateUser2");

	if ( !NT_STATUS_IS_OK(nt_status) ) {
		return nt_status;
	}

	nt_status = create_samr_policy_handle(p->mem_ctx,
					      p,
					      SAMR_HANDLE_USER,
					      acc_granted,
					      &sid,
					      NULL,
					      r->out.user_handle);
	if (!NT_STATUS_IS_OK(nt_status)) {
		return nt_status;
	}

	/* After a "set" ensure we have no cached display info. */
	force_flush_samr_cache(&sid);

	*r->out.access_granted = acc_granted;

	return NT_STATUS_OK;
}

/****************************************************************
****************************************************************/

NTSTATUS _samr_CreateUser(struct pipes_struct *p,
			  struct samr_CreateUser *r)
{
	struct samr_CreateUser2 c;
	uint32_t access_granted;

	c.in.domain_handle	= r->in.domain_handle;
	c.in.account_name	= r->in.account_name;
	c.in.acct_flags		= ACB_NORMAL;
	c.in.access_mask	= r->in.access_mask;
	c.out.user_handle	= r->out.user_handle;
	c.out.access_granted	= &access_granted;
	c.out.rid		= r->out.rid;

	return _samr_CreateUser2(p, &c);
}

/*******************************************************************
 _samr_Connect
 ********************************************************************/

NTSTATUS _samr_Connect(struct pipes_struct *p,
		       struct samr_Connect *r)
{
	struct dcesrv_call_state *dce_call = p->dce_call;
	struct auth_session_info *session_info =
		dcesrv_call_session_info(dce_call);
	uint32_t acc_granted;
	uint32_t    des_access = r->in.access_mask;
	NTSTATUS status;

	/* Access check */

	if (!pipe_access_check(p)) {
		DEBUG(3, ("access denied to _samr_Connect\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	/* don't give away the farm but this is probably ok.  The SAMR_ACCESS_ENUM_DOMAINS
	   was observed from a win98 client trying to enumerate users (when configured
	   user level access control on shares)   --jerry */

	map_max_allowed_access(session_info->security_token,
			       session_info->unix_token,
			       &des_access);

	se_map_generic( &des_access, &sam_generic_mapping );

	acc_granted = des_access & (SAMR_ACCESS_ENUM_DOMAINS
				    |SAMR_ACCESS_LOOKUP_DOMAIN);

	/* set up the SAMR connect_anon response */
	status = create_samr_policy_handle(p->mem_ctx,
					   p,
					   SAMR_HANDLE_CONNECT,
					   acc_granted,
					   NULL,
					   NULL,
					   r->out.connect_handle);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	return NT_STATUS_OK;
}

/*******************************************************************
 _samr_Connect2
 ********************************************************************/

NTSTATUS _samr_Connect2(struct pipes_struct *p,
			struct samr_Connect2 *r)
{
	struct dcesrv_call_state *dce_call = p->dce_call;
	struct auth_session_info *session_info =
		dcesrv_call_session_info(dce_call);
	struct security_descriptor *psd = NULL;
	uint32_t    acc_granted;
	uint32_t    des_access = r->in.access_mask;
	NTSTATUS  nt_status;
	size_t    sd_size;
	const char *fn = "_samr_Connect2";

	switch (dce_call->pkt.u.request.opnum) {
	case NDR_SAMR_CONNECT2:
		fn = "_samr_Connect2";
		break;
	case NDR_SAMR_CONNECT3:
		fn = "_samr_Connect3";
		break;
	case NDR_SAMR_CONNECT4:
		fn = "_samr_Connect4";
		break;
	case NDR_SAMR_CONNECT5:
		fn = "_samr_Connect5";
		break;
	}

	DEBUG(5,("%s: %d\n", fn, __LINE__));

	/* Access check */

	if (!pipe_access_check(p)) {
		DEBUG(3, ("access denied to %s\n", fn));
		return NT_STATUS_ACCESS_DENIED;
	}

	map_max_allowed_access(session_info->security_token,
			       session_info->unix_token,
			       &des_access);

	make_samr_object_sd(p->mem_ctx, &psd, &sd_size, &sam_generic_mapping, NULL, 0);
	se_map_generic(&des_access, &sam_generic_mapping);

	nt_status = access_check_object(psd, session_info->security_token,
					SEC_PRIV_INVALID, SEC_PRIV_INVALID,
					0, des_access, &acc_granted, fn);

	if ( !NT_STATUS_IS_OK(nt_status) )
		return nt_status;

	nt_status = create_samr_policy_handle(p->mem_ctx,
					      p,
					      SAMR_HANDLE_CONNECT,
					      acc_granted,
					      NULL,
					      NULL,
					      r->out.connect_handle);
	if (!NT_STATUS_IS_OK(nt_status)) {
		return nt_status;
	}

	DEBUG(5,("%s: %d\n", fn, __LINE__));

	return NT_STATUS_OK;
}

/****************************************************************
 _samr_Connect3
****************************************************************/

NTSTATUS _samr_Connect3(struct pipes_struct *p,
			struct samr_Connect3 *r)
{
	struct samr_Connect2 c;

	c.in.system_name	= r->in.system_name;
	c.in.access_mask	= r->in.access_mask;
	c.out.connect_handle	= r->out.connect_handle;

	return _samr_Connect2(p, &c);
}

/*******************************************************************
 _samr_Connect4
 ********************************************************************/

NTSTATUS _samr_Connect4(struct pipes_struct *p,
			struct samr_Connect4 *r)
{
	struct samr_Connect2 c;

	c.in.system_name	= r->in.system_name;
	c.in.access_mask	= r->in.access_mask;
	c.out.connect_handle	= r->out.connect_handle;

	return _samr_Connect2(p, &c);
}

/*******************************************************************
 _samr_Connect5
 ********************************************************************/

NTSTATUS _samr_Connect5(struct pipes_struct *p,
			struct samr_Connect5 *r)
{
	NTSTATUS status;
	struct samr_Connect2 c;
	struct samr_ConnectInfo1 info1;

	info1.client_version = SAMR_CONNECT_AFTER_W2K;
	info1.supported_features = 0;

	c.in.system_name	= r->in.system_name;
	c.in.access_mask	= r->in.access_mask;
	c.out.connect_handle	= r->out.connect_handle;

	*r->out.level_out = 1;

	status = _samr_Connect2(p, &c);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	r->out.info_out->info1 = info1;

	return NT_STATUS_OK;
}

/**********************************************************************
 _samr_LookupDomain
 **********************************************************************/

NTSTATUS _samr_LookupDomain(struct pipes_struct *p,
			    struct samr_LookupDomain *r)
{
	NTSTATUS status;
	const char *domain_name;
	struct dom_sid *sid = NULL;
	struct dom_sid_buf buf;

	/* win9x user manager likes to use SAMR_ACCESS_ENUM_DOMAINS here.
	   Reverted that change so we will work with RAS servers again */

	(void)samr_policy_handle_find(p,
				      r->in.connect_handle,
				      SAMR_HANDLE_CONNECT,
				      SAMR_ACCESS_LOOKUP_DOMAIN,
				      NULL,
				      &status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	domain_name = r->in.domain_name->string;
	if (!domain_name) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	sid = talloc_zero(p->mem_ctx, struct dom_sid2);
	if (!sid) {
		return NT_STATUS_NO_MEMORY;
	}

	if (strequal(domain_name, builtin_domain_name())) {
		sid_copy(sid, &global_sid_Builtin);
	} else {
		if (!secrets_fetch_domain_sid(domain_name, sid)) {
			status = NT_STATUS_NO_SUCH_DOMAIN;
		}
	}

	DEBUG(2,("Returning domain sid for domain %s -> %s\n", domain_name,
		 dom_sid_str_buf(sid, &buf)));

	*r->out.sid = sid;

	return status;
}

/**********************************************************************
 _samr_EnumDomains
 **********************************************************************/

NTSTATUS _samr_EnumDomains(struct pipes_struct *p,
			   struct samr_EnumDomains *r)
{
	NTSTATUS status;
	uint32_t num_entries = 2;
	struct samr_SamEntry *entry_array = NULL;
	struct samr_SamArray *sam;

	(void)samr_policy_handle_find(p,
				      r->in.connect_handle,
				      SAMR_HANDLE_CONNECT,
				      SAMR_ACCESS_ENUM_DOMAINS,
				      NULL,
				      &status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	sam = talloc_zero(p->mem_ctx, struct samr_SamArray);
	if (!sam) {
		return NT_STATUS_NO_MEMORY;
	}

	entry_array = talloc_zero_array(p->mem_ctx,
					struct samr_SamEntry,
					num_entries);
	if (!entry_array) {
		return NT_STATUS_NO_MEMORY;
	}

	entry_array[0].idx = 0;
	init_lsa_String(&entry_array[0].name, get_global_sam_name());

	entry_array[1].idx = 1;
	init_lsa_String(&entry_array[1].name, "Builtin");

	sam->count = num_entries;
	sam->entries = entry_array;

	*r->out.sam = sam;
	*r->out.num_entries = num_entries;

	return status;
}

/*******************************************************************
 _samr_OpenAlias
 ********************************************************************/

NTSTATUS _samr_OpenAlias(struct pipes_struct *p,
			 struct samr_OpenAlias *r)
{
	struct dcesrv_call_state *dce_call = p->dce_call;
	struct auth_session_info *session_info =
		dcesrv_call_session_info(dce_call);
	struct dom_sid sid;
	uint32_t alias_rid = r->in.rid;
	struct samr_info *dinfo;
	struct security_descriptor *psd = NULL;
	uint32_t    acc_granted;
	uint32_t    des_access = r->in.access_mask;
	size_t    sd_size;
	NTSTATUS  status;

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					SAMR_DOMAIN_ACCESS_OPEN_ACCOUNT,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/* append the alias' RID to it */

	if (!sid_compose(&sid, &dinfo->sid, alias_rid))
		return NT_STATUS_NO_SUCH_ALIAS;

	/*check if access can be granted as requested by client. */

	map_max_allowed_access(session_info->security_token,
			       session_info->unix_token,
			       &des_access);

	make_samr_object_sd(p->mem_ctx, &psd, &sd_size, &ali_generic_mapping, NULL, 0);
	se_map_generic(&des_access,&ali_generic_mapping);

	status = access_check_object(psd, session_info->security_token,
				     SEC_PRIV_ADD_USERS, SEC_PRIV_INVALID,
				     GENERIC_RIGHTS_ALIAS_ALL_ACCESS,
				     des_access, &acc_granted, "_samr_OpenAlias");

	if ( !NT_STATUS_IS_OK(status) )
		return status;

	{
		/* Check we actually have the requested alias */
		enum lsa_SidType type;
		bool result;
		gid_t gid;

		become_root();
		result = lookup_sid(NULL, &sid, NULL, NULL, &type);
		unbecome_root();

		if (!result || (type != SID_NAME_ALIAS)) {
			return NT_STATUS_NO_SUCH_ALIAS;
		}

		/* make sure there is a mapping */

		if ( !sid_to_gid( &sid, &gid ) ) {
			return NT_STATUS_NO_SUCH_ALIAS;
		}

	}

	status = create_samr_policy_handle(p->mem_ctx,
					   p,
					   SAMR_HANDLE_ALIAS,
					   acc_granted,
					   &sid,
					   NULL,
					   r->out.alias_handle);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	return NT_STATUS_OK;
}

/*******************************************************************
 set_user_info_2
 ********************************************************************/

static NTSTATUS set_user_info_2(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo2 *id2,
				struct samu *pwd)
{
	if (id2 == NULL) {
		DEBUG(5,("set_user_info_2: NULL id2\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	copy_id2_to_sam_passwd(pwd, id2);

	return pdb_update_sam_account(pwd);
}

/*******************************************************************
 set_user_info_4
 ********************************************************************/

static NTSTATUS set_user_info_4(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo4 *id4,
				struct samu *pwd)
{
	if (id4 == NULL) {
		DEBUG(5,("set_user_info_2: NULL id4\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	copy_id4_to_sam_passwd(pwd, id4);

	return pdb_update_sam_account(pwd);
}

/*******************************************************************
 set_user_info_6
 ********************************************************************/

static NTSTATUS set_user_info_6(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo6 *id6,
				struct samu *pwd)
{
	if (id6 == NULL) {
		DEBUG(5,("set_user_info_6: NULL id6\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	copy_id6_to_sam_passwd(pwd, id6);

	return pdb_update_sam_account(pwd);
}

/*******************************************************************
 set_user_info_7
 ********************************************************************/

static NTSTATUS set_user_info_7(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo7 *id7,
				struct samu *pwd)
{
	NTSTATUS rc;

	if (id7 == NULL) {
		DEBUG(5, ("set_user_info_7: NULL id7\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	if (!id7->account_name.string) {
	        DEBUG(5, ("set_user_info_7: failed to get new username\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	/* check to see if the new username already exists.  Note: we can't
	   reliably lock all backends, so there is potentially the
	   possibility that a user can be created in between this check and
	   the rename.  The rename should fail, but may not get the
	   exact same failure status code.  I think this is small enough
	   of a window for this type of operation and the results are
	   simply that the rename fails with a slightly different status
	   code (like UNSUCCESSFUL instead of ALREADY_EXISTS). */

	rc = can_create(mem_ctx, id7->account_name.string);

	/* when there is nothing to change, we're done here */
	if (NT_STATUS_EQUAL(rc, NT_STATUS_USER_EXISTS) &&
	    strequal(id7->account_name.string, pdb_get_username(pwd))) {
		return NT_STATUS_OK;
	}
	if (!NT_STATUS_IS_OK(rc)) {
		return rc;
	}

	rc = pdb_rename_sam_account(pwd, id7->account_name.string);

	return rc;
}

/*******************************************************************
 set_user_info_8
 ********************************************************************/

static NTSTATUS set_user_info_8(TALLOC_CTX *mem_ctx,
				struct samr_UserInfo8 *id8,
				struct samu *pwd)
{
	if (id8 == NULL) {
		DEBUG(5,("set_user_info_8: NULL id8\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	copy_id8_to_sam_passwd(pwd, id8);

	return pdb_update_sam_account(pwd);
}

/*******************************************************************
 set_user_info_10
 ********************************************************************/

static NTSTATUS set_user_info_10(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo10 *id10,
				 struct samu *pwd)
{
	if (id10 == NULL) {
		DEBUG(5,("set_user_info_8: NULL id10\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	copy_id10_to_sam_passwd(pwd, id10);

	return pdb_update_sam_account(pwd);
}

/*******************************************************************
 set_user_info_11
 ********************************************************************/

static NTSTATUS set_user_info_11(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo11 *id11,
				 struct samu *pwd)
{
	if (id11 == NULL) {
		DEBUG(5,("set_user_info_11: NULL id11\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	copy_id11_to_sam_passwd(pwd, id11);

	return pdb_update_sam_account(pwd);
}

/*******************************************************************
 set_user_info_12
 ********************************************************************/

static NTSTATUS set_user_info_12(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo12 *id12,
				 struct samu *pwd)
{
	if (id12 == NULL) {
		DEBUG(5,("set_user_info_12: NULL id12\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	copy_id12_to_sam_passwd(pwd, id12);

	return pdb_update_sam_account(pwd);
}

/*******************************************************************
 set_user_info_13
 ********************************************************************/

static NTSTATUS set_user_info_13(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo13 *id13,
				 struct samu *pwd)
{
	if (id13 == NULL) {
		DEBUG(5,("set_user_info_13: NULL id13\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	copy_id13_to_sam_passwd(pwd, id13);

	return pdb_update_sam_account(pwd);
}

/*******************************************************************
 set_user_info_14
 ********************************************************************/

static NTSTATUS set_user_info_14(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo14 *id14,
				 struct samu *pwd)
{
	if (id14 == NULL) {
		DEBUG(5,("set_user_info_14: NULL id14\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	copy_id14_to_sam_passwd(pwd, id14);

	return pdb_update_sam_account(pwd);
}

/*******************************************************************
 set_user_info_16
 ********************************************************************/

static NTSTATUS set_user_info_16(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo16 *id16,
				 struct samu *pwd)
{
	if (id16 == NULL) {
		DEBUG(5,("set_user_info_16: NULL id16\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	copy_id16_to_sam_passwd(pwd, id16);

	return pdb_update_sam_account(pwd);
}

/*******************************************************************
 set_user_info_17
 ********************************************************************/

static NTSTATUS set_user_info_17(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo17 *id17,
				 struct samu *pwd)
{
	if (id17 == NULL) {
		DEBUG(5,("set_user_info_17: NULL id17\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	copy_id17_to_sam_passwd(pwd, id17);

	return pdb_update_sam_account(pwd);
}

/*******************************************************************
 set_user_info_18
 ********************************************************************/

static NTSTATUS set_user_info_18(struct samr_UserInfo18 *id18,
				 TALLOC_CTX *mem_ctx,
				 DATA_BLOB *session_key,
				 struct samu *pwd)
{
	int rc;

	if (id18 == NULL) {
		DEBUG(2, ("set_user_info_18: id18 is NULL\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (id18->nt_pwd_active || id18->lm_pwd_active) {
		if (!session_key->length) {
			return NT_STATUS_NO_USER_SESSION_KEY;
		}
	}

	if (id18->nt_pwd_active) {
		DATA_BLOB in = data_blob_const(id18->nt_pwd.hash, 16);
		uint8_t outbuf[16] = { 0, };
		DATA_BLOB out = data_blob_const(outbuf, sizeof(outbuf));

		rc = sess_crypt_blob(&out, &in, session_key, SAMBA_GNUTLS_DECRYPT);
		if (rc != 0) {
			return gnutls_error_to_ntstatus(rc,
							NT_STATUS_ACCESS_DISABLED_BY_POLICY_OTHER);
		}

		if (!pdb_set_nt_passwd(pwd, out.data, PDB_CHANGED)) {
			return NT_STATUS_ACCESS_DENIED;
		}

		pdb_set_pass_last_set_time(pwd, time(NULL), PDB_CHANGED);
	}

	if (id18->lm_pwd_active) {
		DATA_BLOB in = data_blob_const(id18->lm_pwd.hash, 16);
		uint8_t outbuf[16] = { 0, };
		DATA_BLOB out = data_blob_const(outbuf, sizeof(outbuf));

		rc = sess_crypt_blob(&out, &in, session_key, SAMBA_GNUTLS_DECRYPT);
		if (rc != 0) {
			return gnutls_error_to_ntstatus(rc,
							NT_STATUS_ACCESS_DISABLED_BY_POLICY_OTHER);
		}

		if (!pdb_set_lanman_passwd(pwd, out.data, PDB_CHANGED)) {
			return NT_STATUS_ACCESS_DENIED;
		}

		pdb_set_pass_last_set_time(pwd, time(NULL), PDB_CHANGED);
	}

	copy_id18_to_sam_passwd(pwd, id18);

	return pdb_update_sam_account(pwd);
}

/*******************************************************************
 set_user_info_20
 ********************************************************************/

static NTSTATUS set_user_info_20(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo20 *id20,
				 struct samu *pwd)
{
	if (id20 == NULL) {
		DEBUG(5,("set_user_info_20: NULL id20\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	copy_id20_to_sam_passwd(pwd, id20);

	return pdb_update_sam_account(pwd);
}

/*******************************************************************
 set_user_info_21
 ********************************************************************/

static NTSTATUS set_user_info_21(struct samr_UserInfo21 *id21,
				 TALLOC_CTX *mem_ctx,
				 DATA_BLOB *session_key,
				 struct samu *pwd)
{
	NTSTATUS status;
	int rc;

	if (id21 == NULL) {
		DEBUG(5, ("set_user_info_21: NULL id21\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (id21->fields_present == 0) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (id21->fields_present & SAMR_FIELD_LAST_PWD_CHANGE) {
		return NT_STATUS_ACCESS_DENIED;
	}

	if (id21->fields_present & SAMR_FIELD_NT_PASSWORD_PRESENT) {
		if (id21->nt_password_set) {
			DATA_BLOB in = data_blob_const(
				id21->nt_owf_password.array, 16);
			uint8_t outbuf[16] = { 0, };
			DATA_BLOB out = data_blob_const(
				outbuf, sizeof(outbuf));

			if ((id21->nt_owf_password.length != 16) ||
			    (id21->nt_owf_password.size != 16)) {
				return NT_STATUS_INVALID_PARAMETER;
			}

			if (!session_key->length) {
				return NT_STATUS_NO_USER_SESSION_KEY;
			}

			rc = sess_crypt_blob(&out, &in, session_key, SAMBA_GNUTLS_DECRYPT);
			if (rc != 0) {
				return gnutls_error_to_ntstatus(rc,
								NT_STATUS_ACCESS_DISABLED_BY_POLICY_OTHER);
			}

			pdb_set_nt_passwd(pwd, out.data, PDB_CHANGED);
			pdb_set_pass_last_set_time(pwd, time(NULL), PDB_CHANGED);
		}
	}

	if (id21->fields_present & SAMR_FIELD_LM_PASSWORD_PRESENT) {
		if (id21->lm_password_set) {
			DATA_BLOB in = data_blob_const(
				id21->lm_owf_password.array, 16);
			uint8_t outbuf[16] = { 0, };
			DATA_BLOB out = data_blob_const(
				outbuf, sizeof(outbuf));

			if ((id21->lm_owf_password.length != 16) ||
			    (id21->lm_owf_password.size != 16)) {
				return NT_STATUS_INVALID_PARAMETER;
			}

			if (!session_key->length) {
				return NT_STATUS_NO_USER_SESSION_KEY;
			}

			rc = sess_crypt_blob(&out, &in, session_key, SAMBA_GNUTLS_DECRYPT);
			if (rc != 0) {
				return gnutls_error_to_ntstatus(rc,
								NT_STATUS_ACCESS_DISABLED_BY_POLICY_OTHER);
			}

			pdb_set_lanman_passwd(pwd, out.data, PDB_CHANGED);
			pdb_set_pass_last_set_time(pwd, time(NULL), PDB_CHANGED);
		}
	}

	/* we need to separately check for an account rename first */

	if (id21->account_name.string &&
	    (!strequal(id21->account_name.string, pdb_get_username(pwd))))
	{

		/* check to see if the new username already exists.  Note: we can't
		   reliably lock all backends, so there is potentially the
		   possibility that a user can be created in between this check and
		   the rename.  The rename should fail, but may not get the
		   exact same failure status code.  I think this is small enough
		   of a window for this type of operation and the results are
		   simply that the rename fails with a slightly different status
		   code (like UNSUCCESSFUL instead of ALREADY_EXISTS). */

		status = can_create(mem_ctx, id21->account_name.string);
		if (!NT_STATUS_IS_OK(status)) {
			return status;
		}

		status = pdb_rename_sam_account(pwd, id21->account_name.string);

		if (!NT_STATUS_IS_OK(status)) {
			DEBUG(0,("set_user_info_21: failed to rename account: %s\n",
				nt_errstr(status)));
			return status;
		}

		/* set the new username so that later
		   functions can work on the new account */
		pdb_set_username(pwd, id21->account_name.string, PDB_SET);
	}

	copy_id21_to_sam_passwd("INFO_21", pwd, id21);

	/*
	 * The funny part about the previous two calls is
	 * that pwd still has the password hashes from the
	 * passdb entry.  These have not been updated from
	 * id21.  I don't know if they need to be set.    --jerry
	 */

	if ( IS_SAM_CHANGED(pwd, PDB_GROUPSID) ) {
		status = pdb_set_unix_primary_group(mem_ctx, pwd);
		if ( !NT_STATUS_IS_OK(status) ) {
			return status;
		}
	}

	/* Don't worry about writing out the user account since the
	   primary group SID is generated solely from the user's Unix
	   primary group. */

	/* write the change out */
	if(!NT_STATUS_IS_OK(status = pdb_update_sam_account(pwd))) {
		return status;
	}

	return NT_STATUS_OK;
}

/*******************************************************************
 set_user_info_23
 ********************************************************************/

static NTSTATUS set_user_info_23(TALLOC_CTX *mem_ctx,
				 struct samr_UserInfo23 *id23,
				 const char *rhost,
				 struct samu *pwd)
{
	char *plaintext_buf = NULL;
	size_t len = 0;
	uint32_t acct_ctrl;
	NTSTATUS status;

	if (id23 == NULL) {
		DEBUG(5, ("set_user_info_23: NULL id23\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (id23->info.fields_present == 0) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (id23->info.fields_present & SAMR_FIELD_LAST_PWD_CHANGE) {
		return NT_STATUS_ACCESS_DENIED;
	}

	if ((id23->info.fields_present & SAMR_FIELD_NT_PASSWORD_PRESENT) ||
	    (id23->info.fields_present & SAMR_FIELD_LM_PASSWORD_PRESENT)) {

		DEBUG(5, ("Attempting administrator password change (level 23) for user %s\n",
			  pdb_get_username(pwd)));

		if (!decode_pw_buffer(mem_ctx,
				      id23->password.data,
				      &plaintext_buf,
				      &len,
				      CH_UTF16)) {
			return NT_STATUS_WRONG_PASSWORD;
		}

		if (!pdb_set_plaintext_passwd (pwd, plaintext_buf)) {
			return NT_STATUS_ACCESS_DENIED;
		}
	}

	copy_id23_to_sam_passwd(pwd, id23);

	acct_ctrl = pdb_get_acct_ctrl(pwd);

	/* if it's a trust account, don't update /etc/passwd */
	if (    ( (acct_ctrl &  ACB_DOMTRUST) == ACB_DOMTRUST ) ||
		( (acct_ctrl &  ACB_WSTRUST) ==  ACB_WSTRUST) ||
		( (acct_ctrl &  ACB_SVRTRUST) ==  ACB_SVRTRUST) ) {
		DEBUG(5, ("Changing trust account.  Not updating /etc/passwd\n"));
	} else if (plaintext_buf) {
		/* update the UNIX password */
		if (lp_unix_password_sync() ) {
			struct passwd *passwd;
			if (pdb_get_username(pwd) == NULL) {
				DEBUG(1, ("chgpasswd: User without name???\n"));
				return NT_STATUS_ACCESS_DENIED;
			}

			passwd = Get_Pwnam_alloc(pwd, pdb_get_username(pwd));
			if (passwd == NULL) {
				DEBUG(1, ("chgpasswd: Username does not exist in system !?!\n"));
			}

			if(!chgpasswd(pdb_get_username(pwd), rhost,
				      passwd, "", plaintext_buf, True)) {
				return NT_STATUS_ACCESS_DENIED;
			}
			TALLOC_FREE(passwd);
		}
	}

	BURN_STR(plaintext_buf);

	if (IS_SAM_CHANGED(pwd, PDB_GROUPSID) &&
	    (!NT_STATUS_IS_OK(status =  pdb_set_unix_primary_group(mem_ctx,
								   pwd)))) {
		return status;
	}

	if(!NT_STATUS_IS_OK(status = pdb_update_sam_account(pwd))) {
		return status;
	}

	return NT_STATUS_OK;
}

/*******************************************************************
 set_user_info_pw
 ********************************************************************/

static bool set_user_info_pw(uint8_t *pass, const char *rhost, struct samu *pwd)
{
	size_t len = 0;
	char *plaintext_buf = NULL;
	uint32_t acct_ctrl;

	DEBUG(5, ("Attempting administrator password change for user %s\n",
		  pdb_get_username(pwd)));

	acct_ctrl = pdb_get_acct_ctrl(pwd);

	if (!decode_pw_buffer(talloc_tos(),
				pass,
				&plaintext_buf,
				&len,
				CH_UTF16)) {
		return False;
	}

	if (!pdb_set_plaintext_passwd (pwd, plaintext_buf)) {
		return False;
	}

	/* if it's a trust account, don't update /etc/passwd */
	if ( ( (acct_ctrl &  ACB_DOMTRUST) == ACB_DOMTRUST ) ||
		( (acct_ctrl &  ACB_WSTRUST) ==  ACB_WSTRUST) ||
		( (acct_ctrl &  ACB_SVRTRUST) ==  ACB_SVRTRUST) ) {
		DEBUG(5, ("Changing trust account or non-unix-user password, not updating /etc/passwd\n"));
	} else {
		/* update the UNIX password */
		if (lp_unix_password_sync()) {
			struct passwd *passwd;

			if (pdb_get_username(pwd) == NULL) {
				DEBUG(1, ("chgpasswd: User without name???\n"));
				return False;
			}

			passwd = Get_Pwnam_alloc(pwd, pdb_get_username(pwd));
			if (passwd == NULL) {
				DEBUG(1, ("chgpasswd: Username does not exist in system !?!\n"));
			}

			if(!chgpasswd(pdb_get_username(pwd), rhost, passwd,
				      "", plaintext_buf, True)) {
				return False;
			}
			TALLOC_FREE(passwd);
		}
	}

	BURN_STR(plaintext_buf);

	DEBUG(5,("set_user_info_pw: pdb_update_pwd()\n"));

	return True;
}

static bool
set_user_info_pw_aes(DATA_BLOB *pw_data, const char *rhost, struct samu *pwd)
{
	uint32_t acct_ctrl;
	DATA_BLOB new_password = {
		.length = 0,
	};
	bool ok;

	DBG_NOTICE("Attempting administrator password change for user %s\n",
		   pdb_get_username(pwd));

	acct_ctrl = pdb_get_acct_ctrl(pwd);

	ok = decode_pwd_string_from_buffer514(talloc_tos(),
					      pw_data->data,
					      CH_UTF16,
					      &new_password);
	if (!ok) {
		return false;
	}

	ok = pdb_set_plaintext_passwd(pwd, (char *)new_password.data);
	if (!ok) {
		return false;
	}

	/* if it's a trust account, don't update /etc/passwd */
	if (((acct_ctrl & ACB_DOMTRUST) == ACB_DOMTRUST) ||
	    ((acct_ctrl & ACB_WSTRUST) == ACB_WSTRUST) ||
	    ((acct_ctrl & ACB_SVRTRUST) == ACB_SVRTRUST)) {
		DBG_NOTICE("Changing trust account or non-unix-user password, "
			   "not updating /etc/passwd\n");
	} else {
		/* update the UNIX password */
		if (lp_unix_password_sync()) {
			struct passwd *passwd;
			const char *username;

			username = pdb_get_username(pwd);
			if (username == NULL) {
				DBG_WARNING("User unknown\n");
				return false;
			}

			passwd = Get_Pwnam_alloc(pwd, username);
			if (passwd == NULL) {
				DBG_WARNING("chgpasswd: Username does not "
					    "exist on system !?!\n");
			}

			ok = chgpasswd(username,
				       rhost,
				       passwd,
				       "",
				       (char *)new_password.data,
				       true);
			if (!ok) {
				return false;
			}
			TALLOC_FREE(passwd);
		}
	}
	TALLOC_FREE(new_password.data);

	DBG_NOTICE("pdb_update_pwd()\n");

	return true;
}

/*******************************************************************
 set_user_info_24
 ********************************************************************/

static NTSTATUS set_user_info_24(TALLOC_CTX *mem_ctx,
				 const char *rhost,
				 struct samr_UserInfo24 *id24,
				 struct samu *pwd)
{
	NTSTATUS status;

	if (id24 == NULL) {
		DEBUG(5, ("set_user_info_24: NULL id24\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (!set_user_info_pw(id24->password.data, rhost, pwd)) {
		return NT_STATUS_WRONG_PASSWORD;
	}

	copy_id24_to_sam_passwd(pwd, id24);

	status = pdb_update_sam_account(pwd);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	return NT_STATUS_OK;
}

/*******************************************************************
 set_user_info_25
 ********************************************************************/

static NTSTATUS set_user_info_25(TALLOC_CTX *mem_ctx,
				 const char *rhost,
				 struct samr_UserInfo25 *id25,
				 struct samu *pwd)
{
	NTSTATUS status;

	if (id25 == NULL) {
		DEBUG(5, ("set_user_info_25: NULL id25\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (id25->info.fields_present == 0) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (id25->info.fields_present & SAMR_FIELD_LAST_PWD_CHANGE) {
		return NT_STATUS_ACCESS_DENIED;
	}

	if ((id25->info.fields_present & SAMR_FIELD_NT_PASSWORD_PRESENT) ||
	    (id25->info.fields_present & SAMR_FIELD_LM_PASSWORD_PRESENT)) {

		if (!set_user_info_pw(id25->password.data, rhost, pwd)) {
			return NT_STATUS_WRONG_PASSWORD;
		}
	}

	copy_id25_to_sam_passwd(pwd, id25);

	/* write the change out */
	if(!NT_STATUS_IS_OK(status = pdb_update_sam_account(pwd))) {
		return status;
	}

	/*
	 * We need to "pdb_update_sam_account" before the unix primary group
	 * is set, because the idealx scripts would also change the
	 * sambaPrimaryGroupSid using the ldap replace method. pdb_ldap uses
	 * the delete explicit / add explicit, which would then fail to find
	 * the previous primaryGroupSid value.
	 */

	if ( IS_SAM_CHANGED(pwd, PDB_GROUPSID) ) {
		status = pdb_set_unix_primary_group(mem_ctx, pwd);
		if ( !NT_STATUS_IS_OK(status) ) {
			return status;
		}
	}

	return NT_STATUS_OK;
}

/*******************************************************************
 set_user_info_26
 ********************************************************************/

static NTSTATUS set_user_info_26(TALLOC_CTX *mem_ctx,
				 const char *rhost,
				 struct samr_UserInfo26 *id26,
				 struct samu *pwd)
{
	NTSTATUS status;

	if (id26 == NULL) {
		DEBUG(5, ("set_user_info_26: NULL id26\n"));
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (!set_user_info_pw(id26->password.data, rhost, pwd)) {
		return NT_STATUS_WRONG_PASSWORD;
	}

	copy_pwd_expired_to_sam_passwd(pwd, id26->password_expired);

	status = pdb_update_sam_account(pwd);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	return NT_STATUS_OK;
}

static NTSTATUS set_user_info_31(TALLOC_CTX *mem_ctx,
				 const char *rhost,
				 DATA_BLOB *pw_data,
				 uint8_t password_expired,
				 struct samu *pwd)
{
	NTSTATUS status;
	bool ok;

	if (pw_data->length == 0 || pw_data->length > 514) {
		return NT_STATUS_WRONG_PASSWORD;
	}

	ok = set_user_info_pw_aes(pw_data, rhost, pwd);
	if (!ok) {
		return NT_STATUS_WRONG_PASSWORD;
	}

	copy_pwd_expired_to_sam_passwd(pwd, password_expired);

	status = pdb_update_sam_account(pwd);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	return NT_STATUS_OK;
}

static NTSTATUS set_user_info_32(TALLOC_CTX *mem_ctx,
				 const char *rhost,
				 DATA_BLOB *pw_data,
				 struct samr_UserInfo32 *id32,
				 struct samu *pwd)
{
	NTSTATUS status;
	bool ok;

	if (pw_data->length == 0 || pw_data->length > 514) {
		return NT_STATUS_WRONG_PASSWORD;
	}

	if (id32 == NULL) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (id32->info.fields_present == 0) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (id32->info.fields_present & SAMR_FIELD_LAST_PWD_CHANGE) {
		return NT_STATUS_ACCESS_DENIED;
	}

	if ((id32->info.fields_present & SAMR_FIELD_NT_PASSWORD_PRESENT) ||
	    (id32->info.fields_present & SAMR_FIELD_LM_PASSWORD_PRESENT)) {
		ok = set_user_info_pw_aes(pw_data, rhost, pwd);
		if (!ok) {
			return NT_STATUS_WRONG_PASSWORD;
		}
	}

	copy_id32_to_sam_passwd(pwd, id32);

	status = pdb_update_sam_account(pwd);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/*
	 * We need to "pdb_update_sam_account" before the unix primary group
	 * is set, because the idealx scripts would also change the
	 * sambaPrimaryGroupSid using the ldap replace method. pdb_ldap uses
	 * the delete explicit / add explicit, which would then fail to find
	 * the previous primaryGroupSid value.
	 */
	if (IS_SAM_CHANGED(pwd, PDB_GROUPSID)) {
		status = pdb_set_unix_primary_group(mem_ctx, pwd);
		if (!NT_STATUS_IS_OK(status)) {
			return status;
		}
	}

	return NT_STATUS_OK;
}

/*************************************************************
**************************************************************/

static uint32_t samr_set_user_info_map_fields_to_access_mask(uint32_t fields)
{
	uint32_t acc_required = 0;

	/* USER_ALL_USERNAME */
	if (fields & SAMR_FIELD_ACCOUNT_NAME)
		acc_required |= SAMR_USER_ACCESS_SET_ATTRIBUTES;
	/* USER_ALL_FULLNAME */
	if (fields & SAMR_FIELD_FULL_NAME)
		acc_required |= SAMR_USER_ACCESS_SET_ATTRIBUTES;
	/* USER_ALL_PRIMARYGROUPID */
	if (fields & SAMR_FIELD_PRIMARY_GID)
		acc_required |= SAMR_USER_ACCESS_SET_ATTRIBUTES;
	/* USER_ALL_HOMEDIRECTORY */
	if (fields & SAMR_FIELD_HOME_DIRECTORY)
		acc_required |= SAMR_USER_ACCESS_SET_ATTRIBUTES;
	/* USER_ALL_HOMEDIRECTORYDRIVE */
	if (fields & SAMR_FIELD_HOME_DRIVE)
		acc_required |= SAMR_USER_ACCESS_SET_ATTRIBUTES;
	/* USER_ALL_SCRIPTPATH */
	if (fields & SAMR_FIELD_LOGON_SCRIPT)
		acc_required |= SAMR_USER_ACCESS_SET_ATTRIBUTES;
	/* USER_ALL_PROFILEPATH */
	if (fields & SAMR_FIELD_PROFILE_PATH)
		acc_required |= SAMR_USER_ACCESS_SET_ATTRIBUTES;
	/* USER_ALL_ADMINCOMMENT */
	if (fields & SAMR_FIELD_COMMENT)
		acc_required |= SAMR_USER_ACCESS_SET_ATTRIBUTES;
	/* USER_ALL_WORKSTATIONS */
	if (fields & SAMR_FIELD_WORKSTATIONS)
		acc_required |= SAMR_USER_ACCESS_SET_ATTRIBUTES;
	/* USER_ALL_LOGONHOURS */
	if (fields & SAMR_FIELD_LOGON_HOURS)
		acc_required |= SAMR_USER_ACCESS_SET_ATTRIBUTES;
	/* USER_ALL_ACCOUNTEXPIRES */
	if (fields & SAMR_FIELD_ACCT_EXPIRY)
		acc_required |= SAMR_USER_ACCESS_SET_ATTRIBUTES;
	/* USER_ALL_USERACCOUNTCONTROL */
	if (fields & SAMR_FIELD_ACCT_FLAGS)
		acc_required |= SAMR_USER_ACCESS_SET_ATTRIBUTES;
	/* USER_ALL_PARAMETERS */
	if (fields & SAMR_FIELD_PARAMETERS)
		acc_required |= SAMR_USER_ACCESS_SET_ATTRIBUTES;
	/* USER_ALL_USERCOMMENT */
	if (fields & SAMR_FIELD_COMMENT)
		acc_required |= SAMR_USER_ACCESS_SET_LOC_COM;
	/* USER_ALL_COUNTRYCODE */
	if (fields & SAMR_FIELD_COUNTRY_CODE)
		acc_required |= SAMR_USER_ACCESS_SET_LOC_COM;
	/* USER_ALL_CODEPAGE */
	if (fields & SAMR_FIELD_CODE_PAGE)
		acc_required |= SAMR_USER_ACCESS_SET_LOC_COM;
	/* USER_ALL_NTPASSWORDPRESENT */
	if (fields & SAMR_FIELD_NT_PASSWORD_PRESENT)
		acc_required |= SAMR_USER_ACCESS_SET_PASSWORD;
	/* USER_ALL_LMPASSWORDPRESENT */
	if (fields & SAMR_FIELD_LM_PASSWORD_PRESENT)
		acc_required |= SAMR_USER_ACCESS_SET_PASSWORD;
	/* USER_ALL_PASSWORDEXPIRED */
	if (fields & SAMR_FIELD_EXPIRED_FLAG)
		acc_required |= SAMR_USER_ACCESS_SET_PASSWORD;

	return acc_required;
}

static NTSTATUS arc4_decrypt_data(DATA_BLOB session_key,
				     uint8_t *data,
				     size_t data_size)
{
	gnutls_cipher_hd_t cipher_hnd = NULL;
	gnutls_datum_t my_session_key = {
		.data = session_key.data,
		.size = session_key.length,
	};
	NTSTATUS status = NT_STATUS_INTERNAL_ERROR;
	int rc;

	rc = gnutls_cipher_init(&cipher_hnd,
				GNUTLS_CIPHER_ARCFOUR_128,
				&my_session_key,
				NULL);
	if (rc < 0) {
		status = gnutls_error_to_ntstatus(rc, NT_STATUS_CRYPTO_SYSTEM_INVALID);
		goto out;
	}

	rc = gnutls_cipher_decrypt(cipher_hnd,
				   data,
				   data_size);
	gnutls_cipher_deinit(cipher_hnd);
	if (rc < 0) {
		status = gnutls_error_to_ntstatus(rc, NT_STATUS_CRYPTO_SYSTEM_INVALID);
		goto out;
	}

	status = NT_STATUS_OK;
out:
	return status;
}

/*******************************************************************
 samr_SetUserInfo
 ********************************************************************/

NTSTATUS _samr_SetUserInfo(struct pipes_struct *p,
			   struct samr_SetUserInfo *r)
{
	struct dcesrv_call_state *dce_call = p->dce_call;
	struct dcesrv_connection *dcesrv_conn = dce_call->conn;
	const struct tsocket_address *remote_address =
		dcesrv_connection_get_remote_address(dcesrv_conn);
	struct auth_session_info *session_info =
		dcesrv_call_session_info(dce_call);
	struct samr_info *uinfo;
	NTSTATUS status;
	struct samu *pwd = NULL;
	union samr_UserInfo *info = r->in.info;
	uint32_t acc_required = 0;
	uint32_t fields = 0;
	bool ret;
	char *rhost;
	DATA_BLOB session_key;
	struct dom_sid_buf buf;
	struct loadparm_context *lp_ctx = NULL;
	bool encrypted;

	lp_ctx = loadparm_init_s3(p->mem_ctx, loadparm_s3_helpers());
	if (lp_ctx == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	/* This is tricky.  A WinXP domain join sets
	  (SAMR_USER_ACCESS_SET_PASSWORD|SAMR_USER_ACCESS_SET_ATTRIBUTES|SAMR_USER_ACCESS_GET_ATTRIBUTES)
	  The MMC lusrmgr plugin includes these perms and more in the SamrOpenUser().  But the
	  standard Win32 API calls just ask for SAMR_USER_ACCESS_SET_PASSWORD in the SamrOpenUser().
	  This should be enough for levels 18, 24, 25,& 26.  Info level 23 can set more so
	  we'll use the set from the WinXP join as the basis. */

	switch (r->in.level) {
	case 2: /* UserPreferencesInformation */
		/* USER_WRITE_ACCOUNT | USER_WRITE_PREFERENCES */
		acc_required = SAMR_USER_ACCESS_SET_ATTRIBUTES | SAMR_USER_ACCESS_SET_LOC_COM;
		break;
	case 4: /* UserLogonHoursInformation */
	case 6: /* UserNameInformation */
	case 7: /* UserAccountNameInformation */
	case 8: /* UserFullNameInformation */
	case 9: /* UserPrimaryGroupInformation */
	case 10: /* UserHomeInformation */
	case 11: /* UserScriptInformation */
	case 12: /* UserProfileInformation */
	case 13: /* UserAdminCommentInformation */
	case 14: /* UserWorkStationsInformation */
	case 16: /* UserControlInformation */
	case 17: /* UserExpiresInformation */
	case 20: /* UserParametersInformation */
		/* USER_WRITE_ACCOUNT */
		acc_required = SAMR_USER_ACCESS_SET_ATTRIBUTES;
		break;
	case 18: /* UserInternal1Information */
		/* FIXME: gd, this is a guess */
		acc_required = SAMR_USER_ACCESS_SET_PASSWORD;
		break;
	case 21: /* UserAllInformation */
		fields = info->info21.fields_present;
		acc_required = samr_set_user_info_map_fields_to_access_mask(fields);
		break;
	case 23: /* UserInternal4Information */
		fields = info->info23.info.fields_present;
		acc_required = samr_set_user_info_map_fields_to_access_mask(fields);
		break;
	case 25: /* UserInternal4InformationNew */
		fields = info->info25.info.fields_present;
		acc_required = samr_set_user_info_map_fields_to_access_mask(fields);
		break;
	case 24: /* UserInternal5Information */
	case 26: /* UserInternal5InformationNew */
	case 31: /* UserInternal5InformationNew */
		acc_required = SAMR_USER_ACCESS_SET_PASSWORD;
		break;
	case 32:
		fields = info->info32.info.fields_present;
		acc_required =
			samr_set_user_info_map_fields_to_access_mask(fields);
		break;
	default:
		return NT_STATUS_INVALID_INFO_CLASS;
	}

	uinfo = samr_policy_handle_find(p,
					r->in.user_handle,
					SAMR_HANDLE_USER,
					acc_required,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(5, ("_samr_SetUserInfo: sid:%s, level:%d\n",
		  dom_sid_str_buf(&uinfo->sid, &buf),
		  r->in.level));

	if (info == NULL) {
		DEBUG(5, ("_samr_SetUserInfo: NULL info level\n"));
		return NT_STATUS_INVALID_INFO_CLASS;
	}

	if (!(pwd = samu_new(NULL))) {
		return NT_STATUS_NO_MEMORY;
	}

	become_root();
	ret = pdb_getsampwsid(pwd, &uinfo->sid);
	unbecome_root();

	if (!ret) {
		TALLOC_FREE(pwd);
		return NT_STATUS_NO_SUCH_USER;
	}

	rhost = tsocket_address_inet_addr_string(remote_address,
						 talloc_tos());
	if (rhost == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	/* ================ BEGIN Privilege BLOCK ================ */

	become_root();

	/* ok!  user info levels (lots: see MSDEV help), off we go... */

	switch (r->in.level) {

		case 2:
			status = set_user_info_2(p->mem_ctx,
						 &info->info2, pwd);
			break;

		case 4:
			status = set_user_info_4(p->mem_ctx,
						 &info->info4, pwd);
			break;

		case 6:
			status = set_user_info_6(p->mem_ctx,
						 &info->info6, pwd);
			break;

		case 7:
			status = set_user_info_7(p->mem_ctx,
						 &info->info7, pwd);
			break;

		case 8:
			status = set_user_info_8(p->mem_ctx,
						 &info->info8, pwd);
			break;

		case 10:
			status = set_user_info_10(p->mem_ctx,
						  &info->info10, pwd);
			break;

		case 11:
			status = set_user_info_11(p->mem_ctx,
						  &info->info11, pwd);
			break;

		case 12:
			status = set_user_info_12(p->mem_ctx,
						  &info->info12, pwd);
			break;

		case 13:
			status = set_user_info_13(p->mem_ctx,
						  &info->info13, pwd);
			break;

		case 14:
			status = set_user_info_14(p->mem_ctx,
						  &info->info14, pwd);
			break;

		case 16:
			status = set_user_info_16(p->mem_ctx,
						  &info->info16, pwd);
			break;

		case 17:
			status = set_user_info_17(p->mem_ctx,
						  &info->info17, pwd);
			break;

		case 18:
			status = session_extract_session_key(
				session_info, &session_key, KEY_USE_16BYTES);
			if(!NT_STATUS_IS_OK(status)) {
				break;
			}
			/* Used by AS/U JRA. */
			status = set_user_info_18(&info->info18,
						  p->mem_ctx,
						  &session_key,
						  pwd);
			break;

		case 20:
			status = set_user_info_20(p->mem_ctx,
						  &info->info20, pwd);
			break;

		case 21:
			status = session_extract_session_key(
				session_info, &session_key, KEY_USE_16BYTES);
			if(!NT_STATUS_IS_OK(status)) {
				break;
			}
			status = set_user_info_21(&info->info21,
						  p->mem_ctx,
						  &session_key,
						  pwd);
			break;

		case 23:
			encrypted =
				dcerpc_is_transport_encrypted(session_info);
			if (lp_weak_crypto() == SAMBA_WEAK_CRYPTO_DISALLOWED &&
			    !encrypted) {
				status = NT_STATUS_ACCESS_DENIED;
				break;
			}

			status = session_extract_session_key(
				session_info, &session_key, KEY_USE_16BYTES);
			if(!NT_STATUS_IS_OK(status)) {
				break;
			}
			/*
			 * This can be allowed as it requires a session key
			 * which we only have if we have a SMB session.
			 */
			GNUTLS_FIPS140_SET_LAX_MODE();
			status = arc4_decrypt_data(session_key,
						   info->info23.password.data,
						   516);
			GNUTLS_FIPS140_SET_STRICT_MODE();
			if(!NT_STATUS_IS_OK(status)) {
				break;
			}

#ifdef DEBUG_PASSWORD
			dump_data(100, info->info23.password.data, 516);
#endif

			status = set_user_info_23(p->mem_ctx,
						  &info->info23,
						  rhost,
						  pwd);
			break;

		case 24:
			encrypted =
				dcerpc_is_transport_encrypted(session_info);
			if (lp_weak_crypto() == SAMBA_WEAK_CRYPTO_DISALLOWED &&
			    !encrypted) {
				status = NT_STATUS_ACCESS_DENIED;
				break;
			}

			status = session_extract_session_key(
				session_info, &session_key, KEY_USE_16BYTES);
			if(!NT_STATUS_IS_OK(status)) {
				break;
			}
			/*
			 * This can be allowed as it requires a session key
			 * which we only have if we have a SMB session.
			 */
			GNUTLS_FIPS140_SET_LAX_MODE();
			status = arc4_decrypt_data(session_key,
						   info->info24.password.data,
						   516);
			GNUTLS_FIPS140_SET_STRICT_MODE();
			if(!NT_STATUS_IS_OK(status)) {
				break;
			}

#ifdef DEBUG_PASSWORD
			dump_data(100, info->info24.password.data, 516);
#endif

			status = set_user_info_24(p->mem_ctx,
						  rhost,
						  &info->info24, pwd);
			break;

		case 25:
			encrypted =
				dcerpc_is_transport_encrypted(session_info);
			if (lp_weak_crypto() == SAMBA_WEAK_CRYPTO_DISALLOWED &&
			    !encrypted) {
				status = NT_STATUS_ACCESS_DENIED;
				break;
			}

			status = session_extract_session_key(
				session_info, &session_key, KEY_USE_16BYTES);
			if(!NT_STATUS_IS_OK(status)) {
				break;
			}
			/*
			 * This can be allowed as it requires a session key
			 * which we only have if we have a SMB session.
			 */
			GNUTLS_FIPS140_SET_LAX_MODE();
			status = decode_rc4_passwd_buffer(&session_key,
					&info->info25.password);
			GNUTLS_FIPS140_SET_STRICT_MODE();
			if (!NT_STATUS_IS_OK(status)) {
				break;
			}

#ifdef DEBUG_PASSWORD
			dump_data(100, info->info25.password.data, 532);
#endif

			status = set_user_info_25(p->mem_ctx,
						  rhost,
						  &info->info25, pwd);
			break;

		case 26:
			encrypted =
				dcerpc_is_transport_encrypted(session_info);
			if (lp_weak_crypto() == SAMBA_WEAK_CRYPTO_DISALLOWED &&
			    !encrypted) {
				status = NT_STATUS_ACCESS_DENIED;
				break;
			}

			status = session_extract_session_key(
				session_info, &session_key, KEY_USE_16BYTES);
			if(!NT_STATUS_IS_OK(status)) {
				break;
			}
			/*
			 * This can be allowed as it requires a session key
			 * which we only have if we have a SMB session.
			 */
			GNUTLS_FIPS140_SET_LAX_MODE();
			status = decode_rc4_passwd_buffer(&session_key,
					&info->info26.password);
			GNUTLS_FIPS140_SET_STRICT_MODE();
			if (!NT_STATUS_IS_OK(status)) {
				break;
			}

#ifdef DEBUG_PASSWORD
			dump_data(100, info->info26.password.data, 516);
#endif

			status = set_user_info_26(p->mem_ctx,
						  rhost,
						  &info->info26, pwd);
			break;
		case 31: {
			DATA_BLOB new_password = data_blob_null;
			const DATA_BLOB ciphertext = data_blob_const(
				info->info31.password.cipher,
				info->info31.password.cipher_len);
			DATA_BLOB iv = data_blob_const(
				info->info31.password.salt,
				sizeof(info->info31.password.salt));

			status = session_extract_session_key(session_info,
							     &session_key,
							     KEY_USE_16BYTES);
			if (!NT_STATUS_IS_OK(status)) {
				break;
			}

			status =
				samba_gnutls_aead_aes_256_cbc_hmac_sha512_decrypt(
					p->mem_ctx,
					&ciphertext,
					&session_key,
					&samr_aes256_enc_key_salt,
					&samr_aes256_mac_key_salt,
					&iv,
					info->info31.password.auth_data,
					&new_password);
			if (!NT_STATUS_IS_OK(status)) {
				DBG_ERR("samba_gnutls_aead_aes_256_cbc_hmac_"
					"sha512_decrypt "
					"failed with %s\n",
					nt_errstr(status));
				status = NT_STATUS_WRONG_PASSWORD;
				break;
			}

			status = set_user_info_31(p->mem_ctx,
						  rhost,
						  &new_password,
						  info->info31.password_expired,
						  pwd);
			data_blob_clear(&new_password);

			break;
		}
		case 32: {
			DATA_BLOB new_password = data_blob_null;
			const DATA_BLOB ciphertext = data_blob_const(
				info->info32.password.cipher,
				info->info32.password.cipher_len);
			DATA_BLOB iv = data_blob_const(
				info->info32.password.salt,
				sizeof(info->info32.password.salt));

			status = session_extract_session_key(session_info,
							     &session_key,
							     KEY_USE_16BYTES);
			if (!NT_STATUS_IS_OK(status)) {
				break;
			}

			status =
				samba_gnutls_aead_aes_256_cbc_hmac_sha512_decrypt(
					p->mem_ctx,
					&ciphertext,
					&session_key,
					&samr_aes256_enc_key_salt,
					&samr_aes256_mac_key_salt,
					&iv,
					info->info32.password.auth_data,
					&new_password);
			if (!NT_STATUS_IS_OK(status)) {
				status = NT_STATUS_WRONG_PASSWORD;
				break;
			}

			status = set_user_info_32(p->mem_ctx,
						  rhost,
						  &new_password,
						  &info->info32,
						  pwd);
			data_blob_clear_free(&new_password);

			break;
		}
		default:
			status = NT_STATUS_INVALID_INFO_CLASS;
	}

	TALLOC_FREE(pwd);

	unbecome_root();

	/* ================ END Privilege BLOCK ================ */

	if (NT_STATUS_IS_OK(status)) {
		force_flush_samr_cache(&uinfo->sid);
	}

	return status;
}

/*******************************************************************
 _samr_SetUserInfo2
 ********************************************************************/

NTSTATUS _samr_SetUserInfo2(struct pipes_struct *p,
			    struct samr_SetUserInfo2 *r)
{
	struct samr_SetUserInfo q;

	q.in.user_handle	= r->in.user_handle;
	q.in.level		= r->in.level;
	q.in.info		= r->in.info;

	return _samr_SetUserInfo(p, &q);
}

/*********************************************************************
 _samr_GetAliasMembership
*********************************************************************/

NTSTATUS _samr_GetAliasMembership(struct pipes_struct *p,
				  struct samr_GetAliasMembership *r)
{
	size_t num_alias_rids;
	uint32_t *alias_rids;
	struct samr_info *dinfo;
	size_t i;

	NTSTATUS status;

	struct dom_sid *members;

	DEBUG(5,("_samr_GetAliasMembership: %d\n", __LINE__));

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					SAMR_DOMAIN_ACCESS_LOOKUP_ALIAS
					| SAMR_DOMAIN_ACCESS_OPEN_ACCOUNT,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (!sid_check_is_our_sam(&dinfo->sid) &&
	    !sid_check_is_builtin(&dinfo->sid))
		return NT_STATUS_OBJECT_TYPE_MISMATCH;

	if (r->in.sids->num_sids) {
		members = talloc_array(p->mem_ctx, struct dom_sid, r->in.sids->num_sids);

		if (members == NULL)
			return NT_STATUS_NO_MEMORY;
	} else {
		members = NULL;
	}

	for (i=0; i<r->in.sids->num_sids; i++)
		sid_copy(&members[i], r->in.sids->sids[i].sid);

	alias_rids = NULL;
	num_alias_rids = 0;

	become_root();
	status = pdb_enum_alias_memberships(p->mem_ctx, &dinfo->sid, members,
					    r->in.sids->num_sids,
					    &alias_rids, &num_alias_rids);
	unbecome_root();

	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	r->out.rids->count = num_alias_rids;
	r->out.rids->ids = alias_rids;

	if (r->out.rids->ids == NULL) {
		/* Windows domain clients don't accept a NULL ptr here */
		r->out.rids->ids = talloc_zero(p->mem_ctx, uint32_t);
	}
	if (r->out.rids->ids == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	return NT_STATUS_OK;
}

/*********************************************************************
 _samr_GetMembersInAlias
*********************************************************************/

NTSTATUS _samr_GetMembersInAlias(struct pipes_struct *p,
				 struct samr_GetMembersInAlias *r)
{
	struct samr_info *ainfo;
	NTSTATUS status;
	size_t i;
	size_t num_sids = 0;
	struct lsa_SidPtr *sids = NULL;
	struct dom_sid *pdb_sids = NULL;
	struct dom_sid_buf buf;

	ainfo = samr_policy_handle_find(p,
					r->in.alias_handle,
					SAMR_HANDLE_ALIAS,
					SAMR_ALIAS_ACCESS_GET_MEMBERS,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(10, ("sid is %s\n", dom_sid_str_buf(&ainfo->sid, &buf)));

	become_root();
	status = pdb_enum_aliasmem(&ainfo->sid, talloc_tos(), &pdb_sids,
				   &num_sids);
	unbecome_root();

	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (num_sids) {
		sids = talloc_zero_array(p->mem_ctx, struct lsa_SidPtr, num_sids);
		if (sids == NULL) {
			TALLOC_FREE(pdb_sids);
			return NT_STATUS_NO_MEMORY;
		}
	}

	for (i = 0; i < num_sids; i++) {
		sids[i].sid = dom_sid_dup(p->mem_ctx, &pdb_sids[i]);
		if (!sids[i].sid) {
			TALLOC_FREE(pdb_sids);
			return NT_STATUS_NO_MEMORY;
		}
	}

	r->out.sids->num_sids = num_sids;
	r->out.sids->sids = sids;

	TALLOC_FREE(pdb_sids);

	return NT_STATUS_OK;
}

/*********************************************************************
 _samr_QueryGroupMember
*********************************************************************/

NTSTATUS _samr_QueryGroupMember(struct pipes_struct *p,
				struct samr_QueryGroupMember *r)
{
	struct samr_info *ginfo;
	size_t i, num_members;

	uint32_t *rid=NULL;
	uint32_t *attr=NULL;

	NTSTATUS status;
	struct samr_RidAttrArray *rids = NULL;
	struct dom_sid_buf buf;

	ginfo = samr_policy_handle_find(p,
					r->in.group_handle,
					SAMR_HANDLE_GROUP,
					SAMR_GROUP_ACCESS_GET_MEMBERS,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	rids = talloc_zero(p->mem_ctx, struct samr_RidAttrArray);
	if (!rids) {
		return NT_STATUS_NO_MEMORY;
	}

	DEBUG(10, ("sid is %s\n", dom_sid_str_buf(&ginfo->sid, &buf)));

	if (!sid_check_is_in_our_sam(&ginfo->sid)) {
		DEBUG(3, ("sid %s is not in our domain\n",
			  dom_sid_str_buf(&ginfo->sid, &buf)));
		return NT_STATUS_NO_SUCH_GROUP;
	}

	DEBUG(10, ("lookup on Domain SID\n"));

	become_root();
	status = pdb_enum_group_members(p->mem_ctx, &ginfo->sid,
					&rid, &num_members);
	unbecome_root();

	if (!NT_STATUS_IS_OK(status))
		return status;

	if (num_members) {
		attr=talloc_zero_array(p->mem_ctx, uint32_t, num_members);
		if (attr == NULL) {
			return NT_STATUS_NO_MEMORY;
		}
	} else {
		attr = NULL;
	}

	for (i=0; i<num_members; i++) {
		attr[i] = SE_GROUP_DEFAULT_FLAGS;
	}

	rids->count = num_members;
	rids->attributes = attr;
	rids->rids = rid;

	*r->out.rids = rids;

	return NT_STATUS_OK;
}

/*********************************************************************
 _samr_AddAliasMember
*********************************************************************/

NTSTATUS _samr_AddAliasMember(struct pipes_struct *p,
			      struct samr_AddAliasMember *r)
{
	struct samr_info *ainfo;
	struct dom_sid_buf buf;
	NTSTATUS status;

	ainfo = samr_policy_handle_find(p,
					r->in.alias_handle,
					SAMR_HANDLE_ALIAS,
					SAMR_ALIAS_ACCESS_ADD_MEMBER,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(10, ("sid is %s\n", dom_sid_str_buf(&ainfo->sid, &buf)));

	/******** BEGIN SeAddUsers BLOCK *********/

	become_root();
	status = pdb_add_aliasmem(&ainfo->sid, r->in.sid);
	unbecome_root();

	/******** END SeAddUsers BLOCK *********/

	if (NT_STATUS_IS_OK(status)) {
		force_flush_samr_cache(&ainfo->sid);
	}

	return status;
}

/*********************************************************************
 _samr_DeleteAliasMember
*********************************************************************/

NTSTATUS _samr_DeleteAliasMember(struct pipes_struct *p,
				 struct samr_DeleteAliasMember *r)
{
	struct samr_info *ainfo;
	struct dom_sid_buf buf;
	NTSTATUS status;

	ainfo = samr_policy_handle_find(p,
					r->in.alias_handle,
					SAMR_HANDLE_ALIAS,
					SAMR_ALIAS_ACCESS_REMOVE_MEMBER,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(10, ("_samr_del_aliasmem:sid is %s\n",
		   dom_sid_str_buf(&ainfo->sid, &buf)));

	/******** BEGIN SeAddUsers BLOCK *********/

	become_root();
	status = pdb_del_aliasmem(&ainfo->sid, r->in.sid);
	unbecome_root();

	/******** END SeAddUsers BLOCK *********/

	if (NT_STATUS_IS_OK(status)) {
		force_flush_samr_cache(&ainfo->sid);
	}

	return status;
}

/*********************************************************************
 _samr_AddGroupMember
*********************************************************************/

NTSTATUS _samr_AddGroupMember(struct pipes_struct *p,
			      struct samr_AddGroupMember *r)
{
	struct samr_info *ginfo;
	struct dom_sid_buf buf;
	NTSTATUS status;
	uint32_t group_rid;

	ginfo = samr_policy_handle_find(p,
					r->in.group_handle,
					SAMR_HANDLE_GROUP,
					SAMR_GROUP_ACCESS_ADD_MEMBER,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(10, ("sid is %s\n", dom_sid_str_buf(&ginfo->sid, &buf)));

	if (!sid_peek_check_rid(get_global_sam_sid(), &ginfo->sid,
				&group_rid)) {
		return NT_STATUS_INVALID_HANDLE;
	}

	/******** BEGIN SeAddUsers BLOCK *********/

	become_root();
	status = pdb_add_groupmem(p->mem_ctx, group_rid, r->in.rid);
	unbecome_root();

	/******** END SeAddUsers BLOCK *********/

	force_flush_samr_cache(&ginfo->sid);

	return status;
}

/*********************************************************************
 _samr_DeleteGroupMember
*********************************************************************/

NTSTATUS _samr_DeleteGroupMember(struct pipes_struct *p,
				 struct samr_DeleteGroupMember *r)

{
	struct samr_info *ginfo;
	NTSTATUS status;
	uint32_t group_rid;

	/*
	 * delete the group member named r->in.rid
	 * who is a member of the sid associated with the handle
	 * the rid is a user's rid as the group is a domain group.
	 */

	ginfo = samr_policy_handle_find(p,
					r->in.group_handle,
					SAMR_HANDLE_GROUP,
					SAMR_GROUP_ACCESS_REMOVE_MEMBER,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (!sid_peek_check_rid(get_global_sam_sid(), &ginfo->sid,
				&group_rid)) {
		return NT_STATUS_INVALID_HANDLE;
	}

	/******** BEGIN SeAddUsers BLOCK *********/

	become_root();
	status = pdb_del_groupmem(p->mem_ctx, group_rid, r->in.rid);
	unbecome_root();

	/******** END SeAddUsers BLOCK *********/

	force_flush_samr_cache(&ginfo->sid);

	return status;
}

/*********************************************************************
 _samr_DeleteUser
*********************************************************************/

NTSTATUS _samr_DeleteUser(struct pipes_struct *p,
			  struct samr_DeleteUser *r)
{
	struct samr_info *uinfo;
	NTSTATUS status;
	struct samu *sam_pass=NULL;
	bool ret;

	DEBUG(5, ("_samr_DeleteUser: %d\n", __LINE__));

	uinfo = samr_policy_handle_find(p,
					r->in.user_handle,
					SAMR_HANDLE_USER,
					SEC_STD_DELETE,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (!sid_check_is_in_our_sam(&uinfo->sid))
		return NT_STATUS_CANNOT_DELETE;

	/* check if the user exists before trying to delete */
	if ( !(sam_pass = samu_new( NULL )) ) {
		return NT_STATUS_NO_MEMORY;
	}

	become_root();
	ret = pdb_getsampwsid(sam_pass, &uinfo->sid);
	unbecome_root();

	if(!ret) {
		struct dom_sid_buf buf;
		DEBUG(5,("_samr_DeleteUser: User %s doesn't exist.\n",
			 dom_sid_str_buf(&uinfo->sid, &buf)));
		TALLOC_FREE(sam_pass);
		return NT_STATUS_NO_SUCH_USER;
	}

	/******** BEGIN SeAddUsers BLOCK *********/

	become_root();
	status = pdb_delete_user(p->mem_ctx, sam_pass);
	unbecome_root();

	/******** END SeAddUsers BLOCK *********/

	if ( !NT_STATUS_IS_OK(status) ) {
		DEBUG(5,("_samr_DeleteUser: Failed to delete entry for "
			 "user %s: %s.\n", pdb_get_username(sam_pass),
			 nt_errstr(status)));
		TALLOC_FREE(sam_pass);
		return status;
	}


	TALLOC_FREE(sam_pass);

	force_flush_samr_cache(&uinfo->sid);

	if (!close_policy_hnd(p, r->in.user_handle))
		return NT_STATUS_OBJECT_NAME_INVALID;

	ZERO_STRUCTP(r->out.user_handle);

	return NT_STATUS_OK;
}

/*********************************************************************
 _samr_DeleteDomainGroup
*********************************************************************/

NTSTATUS _samr_DeleteDomainGroup(struct pipes_struct *p,
				 struct samr_DeleteDomainGroup *r)
{
	struct samr_info *ginfo;
	struct dom_sid_buf buf;
	NTSTATUS status;
	uint32_t group_rid;

	DEBUG(5, ("samr_DeleteDomainGroup: %d\n", __LINE__));

	ginfo = samr_policy_handle_find(p,
					r->in.group_handle,
					SAMR_HANDLE_GROUP,
					SEC_STD_DELETE,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(10, ("sid is %s\n", dom_sid_str_buf(&ginfo->sid, &buf)));

	if (!sid_peek_check_rid(get_global_sam_sid(), &ginfo->sid,
				&group_rid)) {
		return NT_STATUS_NO_SUCH_GROUP;
	}

	/******** BEGIN SeAddUsers BLOCK *********/

	become_root();
	status = pdb_delete_dom_group(p->mem_ctx, group_rid);
	unbecome_root();

	/******** END SeAddUsers BLOCK *********/

	if ( !NT_STATUS_IS_OK(status) ) {
		DEBUG(5,("_samr_DeleteDomainGroup: Failed to delete mapping "
			 "entry for group %s: %s\n",
			 dom_sid_str_buf(&ginfo->sid, &buf),
			 nt_errstr(status)));
		return status;
	}

	force_flush_samr_cache(&ginfo->sid);

	if (!close_policy_hnd(p, r->in.group_handle))
		return NT_STATUS_OBJECT_NAME_INVALID;

	return NT_STATUS_OK;
}

/*********************************************************************
 _samr_DeleteDomAlias
*********************************************************************/

NTSTATUS _samr_DeleteDomAlias(struct pipes_struct *p,
			      struct samr_DeleteDomAlias *r)
{
	struct samr_info *ainfo;
	struct dom_sid_buf buf;
	NTSTATUS status;

	DEBUG(5, ("_samr_DeleteDomAlias: %d\n", __LINE__));

	ainfo = samr_policy_handle_find(p,
					r->in.alias_handle,
					SAMR_HANDLE_ALIAS,
					SEC_STD_DELETE,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(10, ("sid is %s\n", dom_sid_str_buf(&ainfo->sid, &buf)));

	/* Don't let Windows delete builtin groups */

	if ( sid_check_is_in_builtin( &ainfo->sid ) ) {
		return NT_STATUS_SPECIAL_ACCOUNT;
	}

	if (!sid_check_is_in_our_sam(&ainfo->sid))
		return NT_STATUS_NO_SUCH_ALIAS;

	DEBUG(10, ("lookup on Local SID\n"));

	/******** BEGIN SeAddUsers BLOCK *********/

	become_root();
	/* Have passdb delete the alias */
	status = pdb_delete_alias(&ainfo->sid);
	unbecome_root();

	/******** END SeAddUsers BLOCK *********/

	if ( !NT_STATUS_IS_OK(status))
		return status;

	force_flush_samr_cache(&ainfo->sid);

	if (!close_policy_hnd(p, r->in.alias_handle))
		return NT_STATUS_OBJECT_NAME_INVALID;

	return NT_STATUS_OK;
}

/*********************************************************************
 _samr_CreateDomainGroup
*********************************************************************/

NTSTATUS _samr_CreateDomainGroup(struct pipes_struct *p,
				 struct samr_CreateDomainGroup *r)

{
	NTSTATUS status;
	const char *name;
	struct samr_info *dinfo;
	struct dom_sid sid;

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					SAMR_DOMAIN_ACCESS_CREATE_GROUP,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (!sid_check_is_our_sam(&dinfo->sid)) {
		return NT_STATUS_ACCESS_DENIED;
	}

	name = r->in.name->string;
	if (name == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	status = can_create(p->mem_ctx, name);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/******** BEGIN SeAddUsers BLOCK *********/

	become_root();
	/* check that we successfully create the UNIX group */
	status = pdb_create_dom_group(p->mem_ctx, name, r->out.rid);
	unbecome_root();

	/******** END SeAddUsers BLOCK *********/

	/* check if we should bail out here */

	if ( !NT_STATUS_IS_OK(status) )
		return status;

	sid_compose(&sid, &dinfo->sid, *r->out.rid);

	status = create_samr_policy_handle(p->mem_ctx,
					   p,
					   SAMR_HANDLE_GROUP,
					   GENERIC_RIGHTS_GROUP_ALL_ACCESS,
					   &sid,
					   NULL,
					   r->out.group_handle);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	force_flush_samr_cache(&dinfo->sid);

	return NT_STATUS_OK;
}

/*********************************************************************
 _samr_CreateDomAlias
*********************************************************************/

NTSTATUS _samr_CreateDomAlias(struct pipes_struct *p,
			      struct samr_CreateDomAlias *r)
{
	struct dom_sid info_sid;
	const char *name = NULL;
	struct samr_info *dinfo;
	gid_t gid;
	NTSTATUS result;

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					SAMR_DOMAIN_ACCESS_CREATE_ALIAS,
					NULL,
					&result);
	if (!NT_STATUS_IS_OK(result)) {
		return result;
	}

	if (!sid_check_is_our_sam(&dinfo->sid)) {
		return NT_STATUS_ACCESS_DENIED;
	}

	name = r->in.alias_name->string;

	result = can_create(p->mem_ctx, name);
	if (!NT_STATUS_IS_OK(result)) {
		return result;
	}

	/******** BEGIN SeAddUsers BLOCK *********/

	become_root();
	/* Have passdb create the alias */
	result = pdb_create_alias(name, r->out.rid);
	unbecome_root();

	/******** END SeAddUsers BLOCK *********/

	if (!NT_STATUS_IS_OK(result)) {
		DEBUG(10, ("pdb_create_alias failed: %s\n",
			   nt_errstr(result)));
		return result;
	}

	sid_compose(&info_sid, &dinfo->sid, *r->out.rid);

	if (!sid_to_gid(&info_sid, &gid)) {
		DEBUG(10, ("Could not find alias just created\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	/* check if the group has been successfully created */
	if ( getgrgid(gid) == NULL ) {
		DEBUG(1, ("getgrgid(%u) of just created alias failed\n",
			   (unsigned int)gid));
		return NT_STATUS_ACCESS_DENIED;
	}

	result = create_samr_policy_handle(p->mem_ctx,
					   p,
					   SAMR_HANDLE_ALIAS,
					   GENERIC_RIGHTS_ALIAS_ALL_ACCESS,
					   &info_sid,
					   NULL,
					   r->out.alias_handle);
	if (!NT_STATUS_IS_OK(result)) {
		return result;
	}

	force_flush_samr_cache(&info_sid);

	return NT_STATUS_OK;
}

/*********************************************************************
 _samr_QueryGroupInfo
*********************************************************************/

NTSTATUS _samr_QueryGroupInfo(struct pipes_struct *p,
			      struct samr_QueryGroupInfo *r)
{
	struct samr_info *ginfo;
	NTSTATUS status;
	GROUP_MAP *map;
	union samr_GroupInfo *info = NULL;
	bool ret;
	uint32_t attributes = SE_GROUP_DEFAULT_FLAGS;
	const char *group_name = NULL;
	const char *group_description = NULL;

	ginfo = samr_policy_handle_find(p,
					r->in.group_handle,
					SAMR_HANDLE_GROUP,
					SAMR_GROUP_ACCESS_LOOKUP_INFO,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	map = talloc_zero(p->mem_ctx, GROUP_MAP);
	if (!map) {
		return NT_STATUS_NO_MEMORY;
	}

	become_root();
	ret = get_domain_group_from_sid(ginfo->sid, map);
	unbecome_root();
	if (!ret)
		return NT_STATUS_INVALID_HANDLE;

	group_name = talloc_move(r, &map->nt_name);
	group_description = talloc_move(r, &map->comment);

	TALLOC_FREE(map);

	info = talloc_zero(p->mem_ctx, union samr_GroupInfo);
	if (!info) {
		return NT_STATUS_NO_MEMORY;
	}

	switch (r->in.level) {
		case 1: {
			uint32_t *members;
			size_t num_members;

			become_root();
			status = pdb_enum_group_members(
				p->mem_ctx, &ginfo->sid, &members,
				&num_members);
			unbecome_root();

			if (!NT_STATUS_IS_OK(status)) {
				return status;
			}

			info->all.name.string		= group_name;
			info->all.attributes		= attributes;
			info->all.num_members		= num_members;
			info->all.description.string	= group_description;
			break;
		}
		case 2:
			info->name.string = group_name;
			break;
		case 3:
			info->attributes.attributes = attributes;
			break;
		case 4:
			info->description.string = group_description;
			break;
		case 5: {
			/*
			uint32_t *members;
			size_t num_members;
			*/

			/*
			become_root();
			status = pdb_enum_group_members(
				p->mem_ctx, &ginfo->sid, &members,
				&num_members);
			unbecome_root();

			if (!NT_STATUS_IS_OK(status)) {
				return status;
			}
			*/
			info->all2.name.string		= group_name;
			info->all2.attributes		= attributes;
			info->all2.num_members		= 0; /* num_members - in w2k3 this is always 0 */
			info->all2.description.string	= group_description;

			break;
		}
		default:
			return NT_STATUS_INVALID_INFO_CLASS;
	}

	*r->out.info = info;

	return NT_STATUS_OK;
}

/*********************************************************************
 _samr_SetGroupInfo
*********************************************************************/

NTSTATUS _samr_SetGroupInfo(struct pipes_struct *p,
			    struct samr_SetGroupInfo *r)
{
	struct samr_info *ginfo;
	GROUP_MAP *map;
	NTSTATUS status;
	bool ret;

	ginfo = samr_policy_handle_find(p,
					r->in.group_handle,
					SAMR_HANDLE_GROUP,
					SAMR_GROUP_ACCESS_SET_INFO,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	map = talloc_zero(p->mem_ctx, GROUP_MAP);
	if (!map) {
		return NT_STATUS_NO_MEMORY;
	}

	become_root();
	ret = get_domain_group_from_sid(ginfo->sid, map);
	unbecome_root();
	if (!ret)
		return NT_STATUS_NO_SUCH_GROUP;

	switch (r->in.level) {
		case 2:
			map->nt_name = talloc_strdup(map,
						     r->in.info->name.string);
			if (!map->nt_name) {
				return NT_STATUS_NO_MEMORY;
			}
			break;
		case 3:
			break;
		case 4:
			map->comment = talloc_strdup(map,
						r->in.info->description.string);
			if (!map->comment) {
				return NT_STATUS_NO_MEMORY;
			}
			break;
		default:
			return NT_STATUS_INVALID_INFO_CLASS;
	}

	/******** BEGIN SeAddUsers BLOCK *********/

	become_root();
	status = pdb_update_group_mapping_entry(map);
	unbecome_root();

	/******** End SeAddUsers BLOCK *********/

	TALLOC_FREE(map);

	if (NT_STATUS_IS_OK(status)) {
		force_flush_samr_cache(&ginfo->sid);
	}

	return status;
}

/*********************************************************************
 _samr_SetAliasInfo
*********************************************************************/

NTSTATUS _samr_SetAliasInfo(struct pipes_struct *p,
			    struct samr_SetAliasInfo *r)
{
	struct samr_info *ainfo;
	struct acct_info *info;
	NTSTATUS status;

	ainfo = samr_policy_handle_find(p,
					r->in.alias_handle,
					SAMR_HANDLE_ALIAS,
					SAMR_ALIAS_ACCESS_SET_INFO,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	info = talloc_zero(p->mem_ctx, struct acct_info);
	if (!info) {
		return NT_STATUS_NO_MEMORY;
	}

	/* get the current group information */

	become_root();
	status = pdb_get_aliasinfo(&ainfo->sid, info);
	unbecome_root();

	if ( !NT_STATUS_IS_OK(status))
		return status;

	switch (r->in.level) {
		case ALIASINFONAME:
		{
			char *group_name;

			/* We currently do not support renaming groups in the
			   the BUILTIN domain.  Refer to util_builtin.c to understand
			   why.  The eventually needs to be fixed to be like Windows
			   where you can rename builtin groups, just not delete them */

			if ( sid_check_is_in_builtin( &ainfo->sid ) ) {
				return NT_STATUS_SPECIAL_ACCOUNT;
			}

			/* There has to be a valid name (and it has to be different) */

			if ( !r->in.info->name.string )
				return NT_STATUS_INVALID_PARAMETER;

			/* If the name is the same just reply "ok".  Yes this
			   doesn't allow you to change the case of a group name. */

			if (strequal(r->in.info->name.string, info->acct_name)) {
				return NT_STATUS_OK;
			}

			talloc_free(info->acct_name);
			info->acct_name = talloc_strdup(info, r->in.info->name.string);
			if (!info->acct_name) {
				return NT_STATUS_NO_MEMORY;
			}

			/* make sure the name doesn't already exist as a user
			   or local group */

			group_name = talloc_asprintf(p->mem_ctx,
						     "%s\\%s",
						     lp_netbios_name(),
						     info->acct_name);
			if (group_name == NULL) {
				return NT_STATUS_NO_MEMORY;
			}

			status = can_create( p->mem_ctx, group_name );
			talloc_free(group_name);
			if ( !NT_STATUS_IS_OK( status ) )
				return status;
			break;
		}
		case ALIASINFODESCRIPTION:
			TALLOC_FREE(info->acct_desc);
			if (r->in.info->description.string) {
				info->acct_desc = talloc_strdup(info,
								r->in.info->description.string);
			} else {
				info->acct_desc = talloc_strdup(info, "");
			}
			if (!info->acct_desc) {
				return NT_STATUS_NO_MEMORY;
			}
			break;
		default:
			return NT_STATUS_INVALID_INFO_CLASS;
	}

        /******** BEGIN SeAddUsers BLOCK *********/

	become_root();
        status = pdb_set_aliasinfo(&ainfo->sid, info);
	unbecome_root();

        /******** End SeAddUsers BLOCK *********/

	if (NT_STATUS_IS_OK(status))
		force_flush_samr_cache(&ainfo->sid);

	return status;
}

/****************************************************************
 _samr_GetDomPwInfo
****************************************************************/

NTSTATUS _samr_GetDomPwInfo(struct pipes_struct *p,
			    struct samr_GetDomPwInfo *r)
{
	const struct loadparm_substitution *lp_sub =
		loadparm_s3_global_substitution();
	uint32_t min_password_length = 0;
	uint32_t password_properties = 0;

	/* Perform access check.  Since this rpc does not require a
	   policy handle it will not be caught by the access checks on
	   SAMR_CONNECT or SAMR_CONNECT_ANON. */

	if (!pipe_access_check(p)) {
		DEBUG(3, ("access denied to _samr_GetDomPwInfo\n"));
		return NT_STATUS_ACCESS_DENIED;
	}

	become_root();
	pdb_get_account_policy(PDB_POLICY_MIN_PASSWORD_LEN,
			       &min_password_length);
	pdb_get_account_policy(PDB_POLICY_USER_MUST_LOGON_TO_CHG_PASS,
			       &password_properties);
	unbecome_root();

	if (lp_check_password_script(talloc_tos(), lp_sub) && *lp_check_password_script(talloc_tos(), lp_sub)) {
		password_properties |= DOMAIN_PASSWORD_COMPLEX;
	}

	r->out.info->min_password_length = min_password_length;
	r->out.info->password_properties = password_properties;

	return NT_STATUS_OK;
}

/*********************************************************************
 _samr_OpenGroup
*********************************************************************/

NTSTATUS _samr_OpenGroup(struct pipes_struct *p,
			 struct samr_OpenGroup *r)

{
	struct dcesrv_call_state *dce_call = p->dce_call;
	struct auth_session_info *session_info =
		dcesrv_call_session_info(dce_call);
	struct dom_sid info_sid;
	struct dom_sid_buf buf;
	GROUP_MAP *map;
	struct samr_info *dinfo;
	struct security_descriptor         *psd = NULL;
	uint32_t            acc_granted;
	uint32_t            des_access = r->in.access_mask;
	size_t            sd_size;
	NTSTATUS          status;
	bool ret;

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					SAMR_DOMAIN_ACCESS_OPEN_ACCOUNT,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/*check if access can be granted as requested by client. */
	map_max_allowed_access(session_info->security_token,
			       session_info->unix_token,
			       &des_access);

	make_samr_object_sd(p->mem_ctx, &psd, &sd_size, &grp_generic_mapping, NULL, 0);
	se_map_generic(&des_access,&grp_generic_mapping);

	status = access_check_object(psd, session_info->security_token,
				     SEC_PRIV_ADD_USERS, SEC_PRIV_INVALID, GENERIC_RIGHTS_GROUP_ALL_ACCESS,
				     des_access, &acc_granted, "_samr_OpenGroup");

	if ( !NT_STATUS_IS_OK(status) )
		return status;

	/* this should not be hard-coded like this */

	if (!sid_check_is_our_sam(&dinfo->sid)) {
		return NT_STATUS_ACCESS_DENIED;
	}

	sid_compose(&info_sid, &dinfo->sid, r->in.rid);

	DEBUG(10, ("_samr_OpenGroup:Opening SID: %s\n",
		   dom_sid_str_buf(&info_sid, &buf)));

	map = talloc_zero(p->mem_ctx, GROUP_MAP);
	if (!map) {
		return NT_STATUS_NO_MEMORY;
	}

	/* check if that group really exists */
	become_root();
	ret = get_domain_group_from_sid(info_sid, map);
	unbecome_root();
	if (!ret)
		return NT_STATUS_NO_SUCH_GROUP;

	TALLOC_FREE(map);

	status = create_samr_policy_handle(p->mem_ctx,
					   p,
					   SAMR_HANDLE_GROUP,
					   acc_granted,
					   &info_sid,
					   NULL,
					   r->out.group_handle);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	return NT_STATUS_OK;
}

/*********************************************************************
 _samr_RemoveMemberFromForeignDomain
*********************************************************************/

NTSTATUS _samr_RemoveMemberFromForeignDomain(struct pipes_struct *p,
					     struct samr_RemoveMemberFromForeignDomain *r)
{
	struct samr_info *dinfo;
	struct dom_sid_buf buf;
	NTSTATUS		result;

	DEBUG(5,("_samr_RemoveMemberFromForeignDomain: removing SID [%s]\n",
		 dom_sid_str_buf(r->in.sid, &buf)));

	/* Find the policy handle. Open a policy on it. */

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					SAMR_DOMAIN_ACCESS_OPEN_ACCOUNT,
					NULL,
					&result);
	if (!NT_STATUS_IS_OK(result)) {
		return result;
	}

	DEBUG(8, ("_samr_RemoveMemberFromForeignDomain: sid is %s\n",
		  dom_sid_str_buf(&dinfo->sid, &buf)));

	/* we can only delete a user from a group since we don't have
	   nested groups anyways.  So in the latter case, just say OK */

	/* TODO: The above comment nowadays is bogus. Since we have nested
	 * groups now, and aliases members are never reported out of the unix
	 * group membership, the "just say OK" makes this call a no-op. For
	 * us. This needs fixing however. */

	/* I've only ever seen this in the wild when deleting a user from
	 * usrmgr.exe. domain_sid is the builtin domain, and the sid to delete
	 * is the user about to be deleted. I very much suspect this is the
	 * only application of this call. To verify this, let people report
	 * other cases. */

	if (!sid_check_is_builtin(&dinfo->sid)) {
		struct dom_sid_buf buf2;
		DBG_WARNING("domain_sid = %s, "
			    "global_sam_sid() = %s\n"
			    "please report to "
			    "samba-technical@lists.samba.org!\n",
			    dom_sid_str_buf(&dinfo->sid, &buf),
			    dom_sid_str_buf(get_global_sam_sid(), &buf2));
		return NT_STATUS_OK;
	}

	force_flush_samr_cache(&dinfo->sid);

	result = NT_STATUS_OK;

	return result;
}

/*******************************************************************
 _samr_QueryDomainInfo2
 ********************************************************************/

NTSTATUS _samr_QueryDomainInfo2(struct pipes_struct *p,
				struct samr_QueryDomainInfo2 *r)
{
	struct samr_QueryDomainInfo q;

	q.in.domain_handle	= r->in.domain_handle;
	q.in.level		= r->in.level;

	q.out.info		= r->out.info;

	return _samr_QueryDomainInfo(p, &q);
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS set_dom_info_1(TALLOC_CTX *mem_ctx,
			       struct samr_DomInfo1 *r)
{
	time_t u_expire, u_min_age;

	u_expire = nt_time_to_unix_abs((NTTIME *)&r->max_password_age);
	u_min_age = nt_time_to_unix_abs((NTTIME *)&r->min_password_age);

	pdb_set_account_policy(PDB_POLICY_MIN_PASSWORD_LEN,
			       (uint32_t)r->min_password_length);
	pdb_set_account_policy(PDB_POLICY_PASSWORD_HISTORY,
			       (uint32_t)r->password_history_length);
	pdb_set_account_policy(PDB_POLICY_USER_MUST_LOGON_TO_CHG_PASS,
			       (uint32_t)r->password_properties);
	pdb_set_account_policy(PDB_POLICY_MAX_PASSWORD_AGE, (int)u_expire);
	pdb_set_account_policy(PDB_POLICY_MIN_PASSWORD_AGE, (int)u_min_age);

	return NT_STATUS_OK;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS set_dom_info_3(TALLOC_CTX *mem_ctx,
			       struct samr_DomInfo3 *r)
{
	time_t u_logout;

	u_logout = nt_time_to_unix_abs((NTTIME *)&r->force_logoff_time);

	pdb_set_account_policy(PDB_POLICY_TIME_TO_LOGOUT, (int)u_logout);

	return NT_STATUS_OK;
}

/*******************************************************************
 ********************************************************************/

static NTSTATUS set_dom_info_12(TALLOC_CTX *mem_ctx,
			        struct samr_DomInfo12 *r)
{
	time_t u_lock_duration, u_reset_time;

	/*
	 * It is not possible to set lockout_duration < lockout_window.
	 * (The test is the other way around since the negative numbers
	 *  are stored...)
	 *
	 * This constraint is documented here for the samr rpc service:
	 * MS-SAMR 3.1.1.6 Attribute Constraints for Originating Updates
	 * http://msdn.microsoft.com/en-us/library/cc245667%28PROT.10%29.aspx
	 *
	 * And here for the ldap backend:
	 * MS-ADTS 3.1.1.5.3.2 Constraints
	 * http://msdn.microsoft.com/en-us/library/cc223462(PROT.10).aspx
	 */
	if (r->lockout_duration > r->lockout_window) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	u_lock_duration = nt_time_to_unix_abs((NTTIME *)&r->lockout_duration);
	if (u_lock_duration != -1) {
		u_lock_duration /= 60;
	}

	u_reset_time = nt_time_to_unix_abs((NTTIME *)&r->lockout_window)/60;

	pdb_set_account_policy(PDB_POLICY_LOCK_ACCOUNT_DURATION, (int)u_lock_duration);
	pdb_set_account_policy(PDB_POLICY_RESET_COUNT_TIME, (int)u_reset_time);
	pdb_set_account_policy(PDB_POLICY_BAD_ATTEMPT_LOCKOUT,
			       (uint32_t)r->lockout_threshold);

	return NT_STATUS_OK;
}

/*******************************************************************
 _samr_SetDomainInfo
 ********************************************************************/

NTSTATUS _samr_SetDomainInfo(struct pipes_struct *p,
			     struct samr_SetDomainInfo *r)
{
	NTSTATUS status;
	uint32_t acc_required = 0;

	DEBUG(5,("_samr_SetDomainInfo: %d\n", __LINE__));

	switch (r->in.level) {
	case 1: /* DomainPasswordInformation */
	case 12: /* DomainLockoutInformation */
		/* DOMAIN_WRITE_PASSWORD_PARAMETERS */
		acc_required = SAMR_DOMAIN_ACCESS_SET_INFO_1;
		break;
	case 3: /* DomainLogoffInformation */
	case 4: /* DomainOemInformation */
		/* DOMAIN_WRITE_OTHER_PARAMETERS */
		acc_required = SAMR_DOMAIN_ACCESS_SET_INFO_2;
		break;
	case 6: /* DomainReplicationInformation */
	case 9: /* DomainStateInformation */
	case 7: /* DomainServerRoleInformation */
		/* DOMAIN_ADMINISTER_SERVER */
		acc_required = SAMR_DOMAIN_ACCESS_SET_INFO_3;
		break;
	default:
		return NT_STATUS_INVALID_INFO_CLASS;
	}

	(void)samr_policy_handle_find(p,
				      r->in.domain_handle,
				      SAMR_HANDLE_DOMAIN,
				      acc_required,
				      NULL,
				      &status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(5,("_samr_SetDomainInfo: level: %d\n", r->in.level));

	switch (r->in.level) {
		case 1:
			status = set_dom_info_1(p->mem_ctx, &r->in.info->info1);
			break;
		case 3:
			status = set_dom_info_3(p->mem_ctx, &r->in.info->info3);
			break;
		case 4:
			break;
		case 6:
			break;
		case 7:
			break;
		case 9:
			break;
		case 12:
			status = set_dom_info_12(p->mem_ctx, &r->in.info->info12);
			break;
		default:
			return NT_STATUS_INVALID_INFO_CLASS;
	}

	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	DEBUG(5,("_samr_SetDomainInfo: %d\n", __LINE__));

	return NT_STATUS_OK;
}

/****************************************************************
 _samr_GetDisplayEnumerationIndex
****************************************************************/

NTSTATUS _samr_GetDisplayEnumerationIndex(struct pipes_struct *p,
					  struct samr_GetDisplayEnumerationIndex *r)
{
	struct samr_info *dinfo;
	uint32_t max_entries = (uint32_t) -1;
	uint32_t enum_context = 0;
	uint32_t i, num_account = 0;
	struct samr_displayentry *entries = NULL;
	NTSTATUS status;

	DEBUG(5,("_samr_GetDisplayEnumerationIndex: %d\n", __LINE__));

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					SAMR_DOMAIN_ACCESS_ENUM_ACCOUNTS,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if ((r->in.level < 1) || (r->in.level > 3)) {
		DEBUG(0,("_samr_GetDisplayEnumerationIndex: "
			"Unknown info level (%u)\n",
			r->in.level));
		return NT_STATUS_INVALID_INFO_CLASS;
	}

	become_root();

	/* The following done as ROOT. Don't return without unbecome_root(). */

	switch (r->in.level) {
	case 1:
		if (dinfo->disp_info->users == NULL) {
			dinfo->disp_info->users = pdb_search_users(
				dinfo->disp_info, ACB_NORMAL);
			if (dinfo->disp_info->users == NULL) {
				unbecome_root();
				return NT_STATUS_ACCESS_DENIED;
			}
			DEBUG(10,("_samr_GetDisplayEnumerationIndex: "
				"starting user enumeration at index %u\n",
				(unsigned int)enum_context));
		} else {
			DEBUG(10,("_samr_GetDisplayEnumerationIndex: "
				"using cached user enumeration at index %u\n",
				(unsigned int)enum_context));
		}
		num_account = pdb_search_entries(dinfo->disp_info->users,
						 enum_context, max_entries,
						 &entries);
		break;
	case 2:
		if (dinfo->disp_info->machines == NULL) {
			dinfo->disp_info->machines = pdb_search_users(
				dinfo->disp_info, ACB_WSTRUST|ACB_SVRTRUST);
			if (dinfo->disp_info->machines == NULL) {
				unbecome_root();
				return NT_STATUS_ACCESS_DENIED;
			}
			DEBUG(10,("_samr_GetDisplayEnumerationIndex: "
				"starting machine enumeration at index %u\n",
				(unsigned int)enum_context));
		} else {
			DEBUG(10,("_samr_GetDisplayEnumerationIndex: "
				"using cached machine enumeration at index %u\n",
				(unsigned int)enum_context));
		}
		num_account = pdb_search_entries(dinfo->disp_info->machines,
						 enum_context, max_entries,
						 &entries);
		break;
	case 3:
		if (dinfo->disp_info->groups == NULL) {
			dinfo->disp_info->groups = pdb_search_groups(
				dinfo->disp_info);
			if (dinfo->disp_info->groups == NULL) {
				unbecome_root();
				return NT_STATUS_ACCESS_DENIED;
			}
			DEBUG(10,("_samr_GetDisplayEnumerationIndex: "
				"starting group enumeration at index %u\n",
				(unsigned int)enum_context));
		} else {
			DEBUG(10,("_samr_GetDisplayEnumerationIndex: "
				"using cached group enumeration at index %u\n",
				(unsigned int)enum_context));
		}
		num_account = pdb_search_entries(dinfo->disp_info->groups,
						 enum_context, max_entries,
						 &entries);
		break;
	default:
		unbecome_root();
		smb_panic("info class changed");
		break;
	}

	unbecome_root();

	/* Ensure we cache this enumeration. */
	set_disp_info_cache_timeout(dinfo->disp_info, DISP_INFO_CACHE_TIMEOUT);

	DEBUG(10,("_samr_GetDisplayEnumerationIndex: looking for :%s\n",
		r->in.name->string));

	for (i=0; i<num_account; i++) {
		if (strequal(entries[i].account_name, r->in.name->string)) {
			DEBUG(10,("_samr_GetDisplayEnumerationIndex: "
				"found %s at idx %d\n",
				r->in.name->string, i));
			*r->out.idx = i;
			return NT_STATUS_OK;
		}
	}

	/* assuming account_name lives at the very end */
	*r->out.idx = num_account;

	return NT_STATUS_NO_MORE_ENTRIES;
}

/****************************************************************
 _samr_GetDisplayEnumerationIndex2
****************************************************************/

NTSTATUS _samr_GetDisplayEnumerationIndex2(struct pipes_struct *p,
					   struct samr_GetDisplayEnumerationIndex2 *r)
{
	struct samr_GetDisplayEnumerationIndex q;

	q.in.domain_handle	= r->in.domain_handle;
	q.in.level		= r->in.level;
	q.in.name		= r->in.name;

	q.out.idx		= r->out.idx;

	return _samr_GetDisplayEnumerationIndex(p, &q);
}

/****************************************************************
 _samr_RidToSid
****************************************************************/

NTSTATUS _samr_RidToSid(struct pipes_struct *p,
			struct samr_RidToSid *r)
{
	struct samr_info *dinfo;
	NTSTATUS status;
	struct dom_sid sid;

	dinfo = samr_policy_handle_find(p,
					r->in.domain_handle,
					SAMR_HANDLE_DOMAIN,
					0,
					NULL,
					&status);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (!sid_compose(&sid, &dinfo->sid, r->in.rid)) {
		return NT_STATUS_NO_MEMORY;
	}

	*r->out.sid = dom_sid_dup(p->mem_ctx, &sid);
	if (!*r->out.sid) {
		return NT_STATUS_NO_MEMORY;
	}

	return NT_STATUS_OK;
}

/****************************************************************
****************************************************************/

static enum samr_ValidationStatus samr_ValidatePassword_Change(TALLOC_CTX *mem_ctx,
							       const struct samr_PwInfo *dom_pw_info,
							       const struct samr_ValidatePasswordReq2 *req,
							       struct samr_ValidatePasswordRepCtr *rep)
{
	NTSTATUS status;

	if (req->password.string == NULL) {
		return SAMR_VALIDATION_STATUS_SUCCESS;
	}
	if (strlen(req->password.string) < dom_pw_info->min_password_length) {
		ZERO_STRUCT(rep->info);
		return SAMR_VALIDATION_STATUS_PWD_TOO_SHORT;
	}
	if (dom_pw_info->password_properties & DOMAIN_PASSWORD_COMPLEX) {
		status = check_password_complexity(req->account.string,
						   NULL, /* full_name */
						   req->password.string,
						   NULL);
		if (!NT_STATUS_IS_OK(status)) {
			ZERO_STRUCT(rep->info);
			return SAMR_VALIDATION_STATUS_NOT_COMPLEX_ENOUGH;
		}
	}

	return SAMR_VALIDATION_STATUS_SUCCESS;
}

/****************************************************************
****************************************************************/

static enum samr_ValidationStatus samr_ValidatePassword_Reset(TALLOC_CTX *mem_ctx,
							      const struct samr_PwInfo *dom_pw_info,
							      const struct samr_ValidatePasswordReq3 *req,
							      struct samr_ValidatePasswordRepCtr *rep)
{
	NTSTATUS status;

	if (req->password.string == NULL) {
		return SAMR_VALIDATION_STATUS_SUCCESS;
	}
	if (strlen(req->password.string) < dom_pw_info->min_password_length) {
		ZERO_STRUCT(rep->info);
		return SAMR_VALIDATION_STATUS_PWD_TOO_SHORT;
	}
	if (dom_pw_info->password_properties & DOMAIN_PASSWORD_COMPLEX) {
		status = check_password_complexity(req->account.string,
						   NULL, /* full_name */
						   req->password.string,
						   NULL);
		if (!NT_STATUS_IS_OK(status)) {
			ZERO_STRUCT(rep->info);
			return SAMR_VALIDATION_STATUS_NOT_COMPLEX_ENOUGH;
		}
	}

	return SAMR_VALIDATION_STATUS_SUCCESS;
}

/****************************************************************
 _samr_ValidatePassword
****************************************************************/

NTSTATUS _samr_ValidatePassword(struct pipes_struct *p,
				struct samr_ValidatePassword *r)
{
	struct dcesrv_call_state *dce_call = p->dce_call;
	enum dcerpc_AuthLevel auth_level = DCERPC_AUTH_LEVEL_NONE;
	union samr_ValidatePasswordRep *rep;
	NTSTATUS status;
	struct samr_GetDomPwInfo pw;
	struct samr_PwInfo dom_pw_info;

	if (p->transport != NCACN_IP_TCP && p->transport != NCALRPC) {
		p->fault_state = DCERPC_FAULT_ACCESS_DENIED;
		return NT_STATUS_ACCESS_DENIED;
	}

	dcesrv_call_auth_info(dce_call, NULL, &auth_level);

	if (auth_level != DCERPC_AUTH_LEVEL_PRIVACY) {
		p->fault_state = DCERPC_FAULT_ACCESS_DENIED;
		return NT_STATUS_ACCESS_DENIED;
	}

	if (r->in.level < 1 || r->in.level > 3) {
		return NT_STATUS_INVALID_INFO_CLASS;
	}

	pw.in.domain_name = NULL;
	pw.out.info = &dom_pw_info;

	status = _samr_GetDomPwInfo(p, &pw);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	rep = talloc_zero(p->mem_ctx, union samr_ValidatePasswordRep);
	if (!rep) {
		return NT_STATUS_NO_MEMORY;
	}

	switch (r->in.level) {
	case 1:
		status = NT_STATUS_NOT_SUPPORTED;
		break;
	case 2:
		rep->ctr2.status = samr_ValidatePassword_Change(p->mem_ctx,
								&dom_pw_info,
								&r->in.req->req2,
								&rep->ctr2);
		break;
	case 3:
		rep->ctr3.status = samr_ValidatePassword_Reset(p->mem_ctx,
							       &dom_pw_info,
							       &r->in.req->req3,
							       &rep->ctr3);
		break;
	default:
		status = NT_STATUS_INVALID_INFO_CLASS;
		break;
	}

	if (!NT_STATUS_IS_OK(status)) {
		talloc_free(rep);
		return status;
	}

	*r->out.rep = rep;

	return NT_STATUS_OK;
}

/****************************************************************
****************************************************************/

NTSTATUS _samr_Shutdown(struct pipes_struct *p,
			struct samr_Shutdown *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
	return NT_STATUS_NOT_IMPLEMENTED;
}

/****************************************************************
****************************************************************/

NTSTATUS _samr_SetMemberAttributesOfGroup(struct pipes_struct *p,
					  struct samr_SetMemberAttributesOfGroup *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
	return NT_STATUS_NOT_IMPLEMENTED;
}

/****************************************************************
****************************************************************/

NTSTATUS _samr_TestPrivateFunctionsDomain(struct pipes_struct *p,
					  struct samr_TestPrivateFunctionsDomain *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
	return NT_STATUS_NOT_IMPLEMENTED;
}

/****************************************************************
****************************************************************/

NTSTATUS _samr_TestPrivateFunctionsUser(struct pipes_struct *p,
					struct samr_TestPrivateFunctionsUser *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
	return NT_STATUS_NOT_IMPLEMENTED;
}

/****************************************************************
****************************************************************/

NTSTATUS _samr_AddMultipleMembersToAlias(struct pipes_struct *p,
					 struct samr_AddMultipleMembersToAlias *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
	return NT_STATUS_NOT_IMPLEMENTED;
}

/****************************************************************
****************************************************************/

NTSTATUS _samr_RemoveMultipleMembersFromAlias(struct pipes_struct *p,
					      struct samr_RemoveMultipleMembersFromAlias *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
	return NT_STATUS_NOT_IMPLEMENTED;
}

/****************************************************************
****************************************************************/

NTSTATUS _samr_SetBootKeyInformation(struct pipes_struct *p,
				     struct samr_SetBootKeyInformation *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
	return NT_STATUS_NOT_IMPLEMENTED;
}

/****************************************************************
****************************************************************/

NTSTATUS _samr_GetBootKeyInformation(struct pipes_struct *p,
				     struct samr_GetBootKeyInformation *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
	return NT_STATUS_NOT_IMPLEMENTED;
}

/****************************************************************
****************************************************************/

NTSTATUS _samr_SetDsrmPassword(struct pipes_struct *p,
			       struct samr_SetDsrmPassword *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
	return NT_STATUS_NOT_IMPLEMENTED;
}

void _samr_Opnum68NotUsedOnWire(struct pipes_struct *p,
				struct samr_Opnum68NotUsedOnWire *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
}

void _samr_Opnum69NotUsedOnWire(struct pipes_struct *p,
				struct samr_Opnum69NotUsedOnWire *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
}

void _samr_Opnum70NotUsedOnWire(struct pipes_struct *p,
				struct samr_Opnum70NotUsedOnWire *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
}

void _samr_Opnum71NotUsedOnWire(struct pipes_struct *p,
				struct samr_Opnum71NotUsedOnWire *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
}

void _samr_Opnum72NotUsedOnWire(struct pipes_struct *p,
				struct samr_Opnum72NotUsedOnWire *r)
{
	p->fault_state = DCERPC_FAULT_OP_RNG_ERROR;
}

NTSTATUS _samr_ChangePasswordUser4(struct pipes_struct *p,
				   struct samr_ChangePasswordUser4 *r)
{
	TALLOC_CTX *frame = talloc_stackframe();
	struct dcesrv_call_state *dce_call = p->dce_call;
	struct dcesrv_connection *dcesrv_conn = dce_call->conn;
	const struct tsocket_address *remote_address =
		dcesrv_connection_get_remote_address(dcesrv_conn);
	char *rhost = NULL;
	struct samu *sampass = NULL;
	char *username = NULL;
	uint32_t acct_ctrl = 0;
	const uint8_t *nt_pw = NULL;
	gnutls_datum_t nt_key;
	gnutls_datum_t salt = {
		.data = r->in.password->salt,
		.size = sizeof(r->in.password->salt),
	};
	uint8_t cdk_data[16] = {0};
	DATA_BLOB cdk = {
		.data = cdk_data,
		.length = sizeof(cdk_data),
	};
	char *new_passwd = NULL;
	bool updated_badpw = false;
	NTSTATUS update_login_attempts_status;
	char *mutex_name_by_user = NULL;
	struct named_mutex *mtx = NULL;
	NTSTATUS status = NT_STATUS_WRONG_PASSWORD;
	bool ok;
	int rc;

	r->out.result = NT_STATUS_WRONG_PASSWORD;

	DBG_NOTICE("_samr_ChangePasswordUser4\n");

	if (r->in.account->string == NULL) {
		return NT_STATUS_INVALID_PARAMETER;
	}
	if (r->in.password == NULL) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	if (r->in.password->PBKDF2Iterations < 5000 ||
	    r->in.password->PBKDF2Iterations > 1000000) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	(void)map_username(frame, r->in.account->string, &username);
	if (username == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	rhost = tsocket_address_inet_addr_string(remote_address, frame);
	if (rhost == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}
	sampass = samu_new(frame);
	if (sampass == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}

	become_root();
	ok = pdb_getsampwnam(sampass, username);
	unbecome_root();
	if (!ok) {
		status = NT_STATUS_NO_SUCH_USER;
		goto done;
	}

	acct_ctrl = pdb_get_acct_ctrl(sampass);
	if (acct_ctrl & ACB_AUTOLOCK) {
		status = NT_STATUS_ACCOUNT_LOCKED_OUT;
		goto done;
	}

	nt_pw = pdb_get_nt_passwd(sampass);
	nt_key = (gnutls_datum_t){
		.data = discard_const_p(uint8_t, nt_pw),
		.size = NT_HASH_LEN,
	};

	rc = gnutls_pbkdf2(GNUTLS_MAC_SHA512,
			   &nt_key,
			   &salt,
			   r->in.password->PBKDF2Iterations,
			   cdk.data,
			   cdk.length);
	if (rc < 0) {
		BURN_DATA(cdk_data);
		status = NT_STATUS_WRONG_PASSWORD;
		goto done;
	}

	status = samr_set_password_aes(frame,
				       &cdk,
				       r->in.password,
				       &new_passwd);
	BURN_DATA(cdk_data);

	/*
	 * We must re-load the sam account information under a mutex
	 * lock to ensure we don't miss any concurrent account lockout
	 * changes.
	 */

	/* Clear out old sampass info. */
	TALLOC_FREE(sampass);

	sampass = samu_new(frame);
	if (sampass == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}

	mutex_name_by_user = talloc_asprintf(frame,
					     "check_sam_security_mutex_%s",
					     username);
	if (mutex_name_by_user == NULL) {
		status = NT_STATUS_NO_MEMORY;
		goto done;
	}

	/* Grab the named mutex under root with 30 second timeout. */
	become_root();
	mtx = grab_named_mutex(frame, mutex_name_by_user, 30);
	if (mtx != NULL) {
		/* Re-load the account information if we got the mutex. */
		ok = pdb_getsampwnam(sampass, username);
	}
	unbecome_root();

	/* Everything from here on until mtx is freed is done under the mutex.*/

	if (mtx == NULL) {
		DBG_ERR("Acquisition of mutex %s failed "
			"for user %s\n",
			mutex_name_by_user,
			username);
		status = NT_STATUS_INTERNAL_ERROR;
		goto done;
	}

	if (!ok) {
		/*
		 * Re-load of account failed. This could only happen if the
		 * user was deleted in the meantime.
		 */
		DBG_NOTICE("reload of user '%s' in passdb failed.\n",
			   username);
		status = NT_STATUS_NO_SUCH_USER;
		goto done;
	}

	/*
	 * Check if the account is now locked out - now under the mutex.
	 * This can happen if the server is under
	 * a password guess attack and the ACB_AUTOLOCK is set by
	 * another process.
	 */
	if (pdb_get_acct_ctrl(sampass) & ACB_AUTOLOCK) {
		DBG_NOTICE("Account for user %s was locked out.\n", username);
		status = NT_STATUS_ACCOUNT_LOCKED_OUT;
		goto done;
	}

	/*
	 * Notify passdb backend of login success/failure. If not
	 * NT_STATUS_OK the backend doesn't like the login
	 */
	update_login_attempts_status = pdb_update_login_attempts(
		sampass, NT_STATUS_IS_OK(status));

	if (!NT_STATUS_IS_OK(status)) {
		bool increment_bad_pw_count = false;

		if (NT_STATUS_EQUAL(status, NT_STATUS_WRONG_PASSWORD) &&
		    (pdb_get_acct_ctrl(sampass) & ACB_NORMAL) &&
		    NT_STATUS_IS_OK(update_login_attempts_status))
		{
			increment_bad_pw_count = true;
		}

		if (increment_bad_pw_count) {
			pdb_increment_bad_password_count(sampass);
			updated_badpw = true;
		} else {
			pdb_update_bad_password_count(sampass,
						      &updated_badpw);
		}
	} else {
		if ((pdb_get_acct_ctrl(sampass) & ACB_NORMAL) &&
		    (pdb_get_bad_password_count(sampass) > 0))
		{
			pdb_set_bad_password_count(sampass, 0, PDB_CHANGED);
			pdb_set_bad_password_time(sampass, 0, PDB_CHANGED);
			updated_badpw = true;
		}
	}

	if (updated_badpw) {
		NTSTATUS update_status;
		become_root();
		update_status = pdb_update_sam_account(sampass);
		unbecome_root();

		if (!NT_STATUS_IS_OK(update_status)) {
			DEBUG(1, ("Failed to modify entry: %s\n",
				  nt_errstr(update_status)));
		}
	}

	if (!NT_STATUS_IS_OK(status)) {
		goto done;
	}

	become_root();
	status = change_oem_password(sampass,
				     rhost,
				     NULL,
				     new_passwd,
				     true,
				     NULL);
	unbecome_root();
	TALLOC_FREE(new_passwd);

done:
	TALLOC_FREE(frame);

	if (NT_STATUS_EQUAL(status, NT_STATUS_NO_SUCH_USER)) {
		return NT_STATUS_WRONG_PASSWORD;
	}

	return status;
}

/* include the generated boilerplate */
#include "librpc/gen_ndr/ndr_samr_scompat.c"
