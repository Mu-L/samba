/*
   Unix SMB/CIFS implementation.

   Winbind client API

   Copyright (C) Gerald (Jerry) Carter 2007
   Copyright (C) Volker Lendecke 2010


   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* Required Headers */

#include "replace.h"
#include "libwbclient.h"
#include "../winbind_client.h"
#include "lib/util/smb_strtox.h"

/* Convert a sid to a string into a buffer. Return the string
 * length. If buflen is too small, return the string length that would
 * result if it was long enough. */
_PUBLIC_
int wbcSidToStringBuf(const struct wbcDomainSid *sid, char *buf, int buflen)
{
	uint64_t id_auth;
	int i, ofs;

	if (!sid) {
		strlcpy(buf, "(NULL SID)", buflen);
		return 10;	/* strlen("(NULL SID)") */
	}

	id_auth = (uint64_t)sid->id_auth[5] +
		((uint64_t)sid->id_auth[4] << 8) +
		((uint64_t)sid->id_auth[3] << 16) +
		((uint64_t)sid->id_auth[2] << 24) +
		((uint64_t)sid->id_auth[1] << 32) +
		((uint64_t)sid->id_auth[0] << 40);

	ofs = snprintf(buf, buflen, "S-%hhu-", (unsigned char)sid->sid_rev_num);
	if (id_auth >= UINT32_MAX) {
		ofs += snprintf(buf + ofs, MAX(buflen - ofs, 0), "0x%llx",
				(unsigned long long)id_auth);
	} else {
		ofs += snprintf(buf + ofs, MAX(buflen - ofs, 0), "%llu",
				(unsigned long long)id_auth);
	}

	for (i = 0; i < sid->num_auths; i++) {
		ofs += snprintf(buf + ofs, MAX(buflen - ofs, 0), "-%u",
				(unsigned int)sid->sub_auths[i]);
	}
	return ofs;
}

/* Convert a binary SID to a character string */
_PUBLIC_
wbcErr wbcSidToString(const struct wbcDomainSid *sid,
		      char **sid_string)
{
	char buf[WBC_SID_STRING_BUFLEN];
	char *result;
	int len;

	if (!sid) {
		return WBC_ERR_INVALID_SID;
	}

	len = wbcSidToStringBuf(sid, buf, sizeof(buf));

	if (len >= WBC_SID_STRING_BUFLEN) {
		return WBC_ERR_INVALID_SID;
	}

	result = (char *)wbcAllocateMemory(len+1, 1, NULL);
	if (result == NULL) {
		return WBC_ERR_NO_MEMORY;
	}
	memcpy(result, buf, len+1);

	*sid_string = result;
	return WBC_ERR_SUCCESS;
}

#define AUTHORITY_MASK	(~(0xffffffffffffULL))

/* Convert a character string to a binary SID */
_PUBLIC_
wbcErr wbcStringToSid(const char *str,
		      struct wbcDomainSid *sid)
{
	const char *p;
	char *q;
	int error = 0;
	uint64_t x;
	wbcErr wbc_status = WBC_ERR_UNKNOWN_FAILURE;

	if (!sid) {
		wbc_status = WBC_ERR_INVALID_PARAM;
		BAIL_ON_WBC_ERROR(wbc_status);
	}

	/* Sanity check for either "S-" or "s-" */

	if (!str
	    || (str[0]!='S' && str[0]!='s')
	    || (str[1]!='-'))
	{
		wbc_status = WBC_ERR_INVALID_PARAM;
		BAIL_ON_WBC_ERROR(wbc_status);
	}

	/* Get the SID revision number */

	p = str+2;
	x = (uint64_t)smb_strtoul(p, &q, 10, &error, SMB_STR_STANDARD);
	if (x == 0 || x > UINT8_MAX || !q || *q != '-' || error != 0) {
		wbc_status = WBC_ERR_INVALID_SID;
		BAIL_ON_WBC_ERROR(wbc_status);
	}
	sid->sid_rev_num = (uint8_t)x;

	/*
	 * Next the Identifier Authority.  This is stored big-endian in a
	 * 6 byte array. If the authority value is >= UINT_MAX, then it should
	 * be expressed as a hex value, according to MS-DTYP.
	 */
	p = q+1;
	x = smb_strtoull(p, &q, 0, &error, SMB_STR_STANDARD);
	if (!q || *q != '-' || (x & AUTHORITY_MASK) || error != 0) {
		wbc_status = WBC_ERR_INVALID_SID;
		BAIL_ON_WBC_ERROR(wbc_status);
	}
	sid->id_auth[5] = (x & 0x0000000000ffULL);
	sid->id_auth[4] = (x & 0x00000000ff00ULL) >> 8;
	sid->id_auth[3] = (x & 0x000000ff0000ULL) >> 16;
	sid->id_auth[2] = (x & 0x0000ff000000ULL) >> 24;
	sid->id_auth[1] = (x & 0x00ff00000000ULL) >> 32;
	sid->id_auth[0] = (x & 0xff0000000000ULL) >> 40;

	/* now read the subauthorities */
	p = q +1;
	sid->num_auths = 0;
	while (sid->num_auths < WBC_MAXSUBAUTHS) {
		x = smb_strtoull(p, &q, 10, &error, SMB_STR_ALLOW_NO_CONVERSION);
		if (p == q)
			break;
		if (x > UINT32_MAX || error != 0) {
			wbc_status = WBC_ERR_INVALID_SID;
			BAIL_ON_WBC_ERROR(wbc_status);
		}
		sid->sub_auths[sid->num_auths++] = x;

		if (*q != '-') {
			break;
		}
		p = q + 1;
	}

	/* IF we ended early, then the SID could not be converted */

	if (q && *q!='\0') {
		wbc_status = WBC_ERR_INVALID_SID;
		BAIL_ON_WBC_ERROR(wbc_status);
	}

	wbc_status = WBC_ERR_SUCCESS;

done:
	return wbc_status;

}


/* Convert a domain and name to SID */
_PUBLIC_
wbcErr wbcCtxLookupName(struct wbcContext *ctx,
			const char *domain,
			const char *name,
			struct wbcDomainSid *sid,
			enum wbcSidType *name_type)
{
	struct winbindd_request request;
	struct winbindd_response response;
	wbcErr wbc_status = WBC_ERR_UNKNOWN_FAILURE;

	if (!sid || !name_type) {
		wbc_status = WBC_ERR_INVALID_PARAM;
		BAIL_ON_WBC_ERROR(wbc_status);
	}

	/* Initialize request */

	ZERO_STRUCT(request);
	ZERO_STRUCT(response);

	/* dst is already null terminated from the memset above */

	strncpy(request.data.name.dom_name, domain,
		sizeof(request.data.name.dom_name)-1);
	strncpy(request.data.name.name, name,
		sizeof(request.data.name.name)-1);

	wbc_status = wbcRequestResponse(ctx, WINBINDD_LOOKUPNAME,
					&request,
					&response);
	BAIL_ON_WBC_ERROR(wbc_status);

	*name_type = (enum wbcSidType)response.data.sid.type;
	if (*name_type == WBC_SID_NAME_UNKNOWN) {
		return WBC_ERR_NOT_MAPPED;
	}

	wbc_status = wbcStringToSid(response.data.sid.sid, sid);
	BAIL_ON_WBC_ERROR(wbc_status);

	wbc_status = WBC_ERR_SUCCESS;

 done:
	return wbc_status;
}

_PUBLIC_
wbcErr wbcLookupName(const char *domain,
		     const char *name,
		     struct wbcDomainSid *sid,
		     enum wbcSidType *name_type)
{
	return wbcCtxLookupName(NULL, domain, name, sid, name_type);
}


/* Convert a SID to a domain and name */
_PUBLIC_
wbcErr wbcCtxLookupSid(struct wbcContext *ctx,
		       const struct wbcDomainSid *sid,
		       char **pdomain,
		       char **pname,
		       enum wbcSidType *pname_type)
{
	struct winbindd_request request;
	struct winbindd_response response;
	wbcErr wbc_status = WBC_ERR_UNKNOWN_FAILURE;
	char *domain, *name;

	if (!sid) {
		return WBC_ERR_INVALID_PARAM;
	}

	/* Initialize request */

	ZERO_STRUCT(request);
	ZERO_STRUCT(response);

	wbcSidToStringBuf(sid, request.data.sid, sizeof(request.data.sid));

	/* Make request */

	wbc_status = wbcRequestResponse(ctx, WINBINDD_LOOKUPSID,
					&request,
					&response);
	if (!WBC_ERROR_IS_OK(wbc_status)) {
		return wbc_status;
	}

	/* Copy out result */

	wbc_status = WBC_ERR_NO_MEMORY;
	domain = NULL;
	name = NULL;

	domain = wbcStrDup(response.data.name.dom_name);
	if (domain == NULL) {
		goto done;
	}
	name = wbcStrDup(response.data.name.name);
	if (name == NULL) {
		goto done;
	}
	if (pdomain != NULL) {
		*pdomain = domain;
		domain = NULL;
	}
	if (pname != NULL) {
		*pname = name;
		name = NULL;
	}
	if (pname_type != NULL) {
		*pname_type = (enum wbcSidType)response.data.name.type;
	}
	wbc_status = WBC_ERR_SUCCESS;
done:
	wbcFreeMemory(name);
	wbcFreeMemory(domain);
	return wbc_status;
}

_PUBLIC_
wbcErr wbcLookupSid(const struct wbcDomainSid *sid,
		    char **pdomain,
		    char **pname,
		    enum wbcSidType *pname_type)
{
	return wbcCtxLookupSid(NULL, sid, pdomain, pname, pname_type);
}

static void wbcDomainInfosDestructor(void *ptr)
{
	struct wbcDomainInfo *i = (struct wbcDomainInfo *)ptr;

	while (i->short_name != NULL) {
		wbcFreeMemory(i->short_name);
		wbcFreeMemory(i->dns_name);
		i += 1;
	}
}

static void wbcTranslatedNamesDestructor(void *ptr)
{
	struct wbcTranslatedName *n = (struct wbcTranslatedName *)ptr;

	while (n->name != NULL) {
		wbcFreeMemory(n->name);
		n += 1;
	}
}

_PUBLIC_
wbcErr wbcCtxLookupSids(struct wbcContext *ctx,
			const struct wbcDomainSid *sids, int num_sids,
			struct wbcDomainInfo **pdomains, int *pnum_domains,
			struct wbcTranslatedName **pnames)
{
	struct winbindd_request request;
	struct winbindd_response response;
	wbcErr wbc_status = WBC_ERR_UNKNOWN_FAILURE;
	int buflen, i, extra_len, num_domains, num_names;
	char *sidlist, *p, *q, *extra_data;
	struct wbcDomainInfo *domains = NULL;
	struct wbcTranslatedName *names = NULL;
	int error = 0;

	buflen = num_sids * (WBC_SID_STRING_BUFLEN + 1) + 1;

	sidlist = (char *)malloc(buflen);
	if (sidlist == NULL) {
		return WBC_ERR_NO_MEMORY;
	}

	p = sidlist;

	for (i=0; i<num_sids; i++) {
		int remaining;
		int len;

		remaining = buflen - (p - sidlist);

		len = wbcSidToStringBuf(&sids[i], p, remaining);
		if (len > remaining) {
			free(sidlist);
			return WBC_ERR_UNKNOWN_FAILURE;
		}

		p += len;
		*p++ = '\n';
	}
	*p++ = '\0';

	ZERO_STRUCT(request);
	ZERO_STRUCT(response);

	request.extra_data.data = sidlist;
	request.extra_len = p - sidlist;

	wbc_status = wbcRequestResponse(ctx, WINBINDD_LOOKUPSIDS,
					&request, &response);
	free(sidlist);
	if (!WBC_ERROR_IS_OK(wbc_status)) {
		return wbc_status;
	}

	extra_len = response.length - sizeof(struct winbindd_response);
	extra_data = (char *)response.extra_data.data;

	if ((extra_len <= 0) || (extra_data[extra_len-1] != '\0')) {
		goto wbc_err_invalid;
	}

	p = extra_data;

	num_domains = smb_strtoul(p, &q, 10, &error, SMB_STR_STANDARD);
	if (*q != '\n' || error != 0) {
		goto wbc_err_invalid;
	}
	p = q+1;

	domains = (struct wbcDomainInfo *)wbcAllocateMemory(
		num_domains+1, sizeof(struct wbcDomainInfo),
		wbcDomainInfosDestructor);
	if (domains == NULL) {
		wbc_status = WBC_ERR_NO_MEMORY;
		goto fail;
	}

	for (i=0; i<num_domains; i++) {

		q = strchr(p, ' ');
		if (q == NULL) {
			goto wbc_err_invalid;
		}
		*q = '\0';
		wbc_status = wbcStringToSid(p, &domains[i].sid);
		if (!WBC_ERROR_IS_OK(wbc_status)) {
			goto fail;
		}
		p = q+1;

		q = strchr(p, '\n');
		if (q == NULL) {
			goto wbc_err_invalid;
		}
		*q = '\0';
		domains[i].short_name = wbcStrDup(p);
		if (domains[i].short_name == NULL) {
			wbc_status = WBC_ERR_NO_MEMORY;
			goto fail;
		}
		p = q+1;
	}

	num_names = smb_strtoul(p, &q, 10, &error, SMB_STR_STANDARD);
	if (*q != '\n' || error != 0) {
		goto wbc_err_invalid;
	}
	p = q+1;

	if (num_names != num_sids) {
		goto wbc_err_invalid;
	}

	names = (struct wbcTranslatedName *)wbcAllocateMemory(
		num_names+1, sizeof(struct wbcTranslatedName),
		wbcTranslatedNamesDestructor);
	if (names == NULL) {
		wbc_status = WBC_ERR_NO_MEMORY;
		goto fail;
	}

	for (i=0; i<num_names; i++) {

		names[i].domain_index = smb_strtoul(p,
						    &q,
						    10,
						    &error,
						    SMB_STR_STANDARD);
		if (names[i].domain_index < 0 || error != 0) {
			goto wbc_err_invalid;
		}
		if (names[i].domain_index >= num_domains) {
			goto wbc_err_invalid;
		}

		if (*q != ' ') {
			goto wbc_err_invalid;
		}
		p = q+1;

		names[i].type = smb_strtoul(p, &q, 10, &error, SMB_STR_STANDARD);
		if (*q != ' ' || error != 0) {
			goto wbc_err_invalid;
		}
		p = q+1;

		q = strchr(p, '\n');
		if (q == NULL) {
			goto wbc_err_invalid;
		}
		*q = '\0';
		names[i].name = wbcStrDup(p);
		if (names[i].name == NULL) {
			wbc_status = WBC_ERR_NO_MEMORY;
			goto fail;
		}
		p = q+1;
	}
	if (*p != '\0') {
		goto wbc_err_invalid;
	}

	*pdomains = domains;
	*pnames = names;
	winbindd_free_response(&response);
	return WBC_ERR_SUCCESS;

wbc_err_invalid:
	wbc_status = WBC_ERR_INVALID_RESPONSE;
fail:
	winbindd_free_response(&response);
	wbcFreeMemory(domains);
	wbcFreeMemory(names);
	return wbc_status;
}

_PUBLIC_
wbcErr wbcLookupSids(const struct wbcDomainSid *sids, int num_sids,
		     struct wbcDomainInfo **pdomains, int *pnum_domains,
		     struct wbcTranslatedName **pnames)
{
	return wbcCtxLookupSids(NULL, sids, num_sids, pdomains,
				pnum_domains, pnames);
}

/* Translate a collection of RIDs within a domain to names */

_PUBLIC_
wbcErr wbcCtxLookupRids(struct wbcContext *ctx, struct wbcDomainSid *dom_sid,
		     int num_rids,
		     uint32_t *rids,
		     const char **pp_domain_name,
		     const char ***pnames,
		     enum wbcSidType **ptypes)
{
	int i;
	size_t  len, ridbuf_size;
	char *ridlist;
	char *p;
	int error = 0;
	struct winbindd_request request;
	struct winbindd_response response;
	char *domain_name = NULL;
	const char **names = NULL;
	enum wbcSidType *types = NULL;
	wbcErr wbc_status = WBC_ERR_UNKNOWN_FAILURE;

	/* Initialise request */

	ZERO_STRUCT(request);
	ZERO_STRUCT(response);

	if (!dom_sid || (num_rids == 0)) {
		wbc_status = WBC_ERR_INVALID_PARAM;
		BAIL_ON_WBC_ERROR(wbc_status);
	}

	wbcSidToStringBuf(dom_sid, request.data.sid, sizeof(request.data.sid));

	/* Even if all the Rids were of maximum 32bit values,
	   we would only have 11 bytes per rid in the final array
	   ("4294967296" + \n).  Add one more byte for the
	   terminating '\0' */

	ridbuf_size = (sizeof(char)*11) * num_rids + 1;

	ridlist = (char *)malloc(ridbuf_size);
	BAIL_ON_PTR_ERROR(ridlist, wbc_status);

	len = 0;
	for (i=0; i<num_rids; i++) {
		len += snprintf(ridlist + len, ridbuf_size - len, "%u\n",
				rids[i]);
	}
	ridlist[len] = '\0';
	len += 1;

	request.extra_data.data = ridlist;
	request.extra_len = len;

	wbc_status = wbcRequestResponse(ctx, WINBINDD_LOOKUPRIDS,
					&request,
					&response);
	free(ridlist);
	BAIL_ON_WBC_ERROR(wbc_status);

	domain_name = wbcStrDup(response.data.domain_name);
	BAIL_ON_PTR_ERROR(domain_name, wbc_status);

	names = wbcAllocateStringArray(num_rids);
	BAIL_ON_PTR_ERROR(names, wbc_status);

	types = (enum wbcSidType *)wbcAllocateMemory(
		num_rids, sizeof(enum wbcSidType), NULL);
	BAIL_ON_PTR_ERROR(types, wbc_status);

	p = (char *)response.extra_data.data;

	for (i=0; i<num_rids; i++) {
		char *q;

		if (*p == '\0') {
			wbc_status = WBC_ERR_INVALID_RESPONSE;
			goto done;
		}

		types[i] = (enum wbcSidType)smb_strtoul(p,
							&q,
							10,
							&error,
							SMB_STR_STANDARD);

		if (*q != ' ' || error != 0) {
			wbc_status = WBC_ERR_INVALID_RESPONSE;
			goto done;
		}

		p = q+1;

		if ((q = strchr(p, '\n')) == NULL) {
			wbc_status = WBC_ERR_INVALID_RESPONSE;
			goto done;
		}

		*q = '\0';

		names[i] = strdup(p);
		BAIL_ON_PTR_ERROR(names[i], wbc_status);

		p = q+1;
	}

	if (*p != '\0') {
		wbc_status = WBC_ERR_INVALID_RESPONSE;
		goto done;
	}

	wbc_status = WBC_ERR_SUCCESS;

 done:
	winbindd_free_response(&response);

	if (WBC_ERROR_IS_OK(wbc_status)) {
		*pp_domain_name = domain_name;
		*pnames = names;
		*ptypes = types;
	}
	else {
		wbcFreeMemory(domain_name);
		wbcFreeMemory(names);
		wbcFreeMemory(types);
	}

	return wbc_status;
}

_PUBLIC_
wbcErr wbcLookupRids(struct wbcDomainSid *dom_sid,
		     int num_rids,
		     uint32_t *rids,
		     const char **pp_domain_name,
		     const char ***pnames,
		     enum wbcSidType **ptypes)
{
	return wbcCtxLookupRids(NULL, dom_sid, num_rids, rids,
				pp_domain_name, pnames, ptypes);
}

/* Get the groups a user belongs to */
_PUBLIC_
wbcErr wbcCtxLookupUserSids(struct wbcContext *ctx,
			    const struct wbcDomainSid *user_sid,
			    bool domain_groups_only,
			    uint32_t *num_sids,
			    struct wbcDomainSid **_sids)
{
	uint32_t i;
	const char *s;
	struct winbindd_request request;
	struct winbindd_response response;
	struct wbcDomainSid *sids = NULL;
	wbcErr wbc_status = WBC_ERR_UNKNOWN_FAILURE;
	int cmd;

	/* Initialise request */

	ZERO_STRUCT(request);
	ZERO_STRUCT(response);

	if (!user_sid) {
		wbc_status = WBC_ERR_INVALID_PARAM;
		BAIL_ON_WBC_ERROR(wbc_status);
	}

	wbcSidToStringBuf(user_sid, request.data.sid, sizeof(request.data.sid));

	if (domain_groups_only) {
		cmd = WINBINDD_GETUSERDOMGROUPS;
	} else {
		cmd = WINBINDD_GETUSERSIDS;
	}

	wbc_status = wbcRequestResponse(ctx, cmd,
					&request,
					&response);
	BAIL_ON_WBC_ERROR(wbc_status);

	if (response.data.num_entries &&
	    !response.extra_data.data) {
		wbc_status = WBC_ERR_INVALID_RESPONSE;
		BAIL_ON_WBC_ERROR(wbc_status);
	}

	sids = (struct wbcDomainSid *)wbcAllocateMemory(
		response.data.num_entries, sizeof(struct wbcDomainSid),
		NULL);
	BAIL_ON_PTR_ERROR(sids, wbc_status);

	s = (const char *)response.extra_data.data;
	for (i = 0; i < response.data.num_entries; i++) {
		char *n = strchr(s, '\n');
		if (n) {
			*n = '\0';
		}
		wbc_status = wbcStringToSid(s, &sids[i]);
		BAIL_ON_WBC_ERROR(wbc_status);
		s += strlen(s) + 1;
	}

	*num_sids = response.data.num_entries;
	*_sids = sids;
	sids = NULL;
	wbc_status = WBC_ERR_SUCCESS;

 done:
	winbindd_free_response(&response);
	if (sids) {
		wbcFreeMemory(sids);
	}

	return wbc_status;
}

_PUBLIC_
wbcErr wbcLookupUserSids(const struct wbcDomainSid *user_sid,
			 bool domain_groups_only,
			 uint32_t *num_sids,
			 struct wbcDomainSid **_sids)
{
	return wbcCtxLookupUserSids(NULL, user_sid, domain_groups_only,
				    num_sids, _sids);
}

static inline
wbcErr _sid_to_rid(struct wbcDomainSid *sid, uint32_t *rid)
{
	if (sid->num_auths < 1) {
		return WBC_ERR_INVALID_RESPONSE;
	}
	*rid = sid->sub_auths[sid->num_auths - 1];

	return WBC_ERR_SUCCESS;
}

/* Get alias membership for sids */
_PUBLIC_
wbcErr wbcCtxGetSidAliases(struct wbcContext *ctx,
			   const struct wbcDomainSid *dom_sid,
			   struct wbcDomainSid *sids,
			   uint32_t num_sids,
			   uint32_t **alias_rids,
			   uint32_t *num_alias_rids)
{
	uint32_t i;
	const char *s;
	struct winbindd_request request;
	struct winbindd_response response;
	ssize_t extra_data_len = 0;
	char * extra_data = NULL;
	ssize_t buflen = 0;
	struct wbcDomainSid sid;
	wbcErr wbc_status = WBC_ERR_UNKNOWN_FAILURE;
	uint32_t * rids = NULL;

	/* Initialise request */

	ZERO_STRUCT(request);
	ZERO_STRUCT(response);

	if (!dom_sid) {
		wbc_status = WBC_ERR_INVALID_PARAM;
		goto done;
	}

	wbcSidToStringBuf(dom_sid, request.data.sid, sizeof(request.data.sid));

	/* Lets assume each sid is around 57 characters
	 * S-1-5-21-AAAAAAAAAAA-BBBBBBBBBBB-CCCCCCCCCCC-DDDDDDDDDDD\n */
	buflen = 57 * num_sids;
	extra_data = (char *)malloc(buflen);
	if (!extra_data) {
		wbc_status = WBC_ERR_NO_MEMORY;
		goto done;
	}

	/* Build the sid list */
	for (i=0; i<num_sids; i++) {
		char sid_str[WBC_SID_STRING_BUFLEN];
		size_t sid_len;

		sid_len = wbcSidToStringBuf(&sids[i], sid_str, sizeof(sid_str));

		if (buflen < extra_data_len + sid_len + 2) {
			char * tmp_data = NULL;
			buflen *= 2;
			tmp_data = (char *)realloc(extra_data, buflen);
			if (!tmp_data) {
				wbc_status = WBC_ERR_NO_MEMORY;
				BAIL_ON_WBC_ERROR(wbc_status);
			}
			extra_data = tmp_data;
		}

		strncpy(&extra_data[extra_data_len], sid_str,
			buflen - extra_data_len);
		extra_data_len += sid_len;
		extra_data[extra_data_len++] = '\n';
		extra_data[extra_data_len] = '\0';
	}
	extra_data_len += 1;

	request.extra_data.data = extra_data;
	request.extra_len = extra_data_len;

	wbc_status = wbcRequestResponse(ctx, WINBINDD_GETSIDALIASES,
					&request,
					&response);
	BAIL_ON_WBC_ERROR(wbc_status);

	if (response.data.num_entries &&
	    !response.extra_data.data) {
		wbc_status = WBC_ERR_INVALID_RESPONSE;
		goto done;
	}

	rids = (uint32_t *)wbcAllocateMemory(response.data.num_entries,
					     sizeof(uint32_t), NULL);
	BAIL_ON_PTR_ERROR(rids, wbc_status);

	s = (const char *)response.extra_data.data;
	for (i = 0; i < response.data.num_entries; i++) {
		char *n = strchr(s, '\n');
		if (n) {
			*n = '\0';
		}
		wbc_status = wbcStringToSid(s, &sid);
		BAIL_ON_WBC_ERROR(wbc_status);
		wbc_status = _sid_to_rid(&sid, &rids[i]);
		BAIL_ON_WBC_ERROR(wbc_status);
		s += strlen(s) + 1;
	}

	*num_alias_rids = response.data.num_entries;
	*alias_rids = rids;
	rids = NULL;
	wbc_status = WBC_ERR_SUCCESS;

 done:
	free(extra_data);
	winbindd_free_response(&response);
	wbcFreeMemory(rids);
	return wbc_status;
}

_PUBLIC_
wbcErr wbcGetSidAliases(const struct wbcDomainSid *dom_sid,
			struct wbcDomainSid *sids,
			uint32_t num_sids,
			uint32_t **alias_rids,
			uint32_t *num_alias_rids)
{
	return wbcCtxGetSidAliases(NULL, dom_sid, sids, num_sids,
				   alias_rids, num_alias_rids);
}


/* Lists Users */
_PUBLIC_
wbcErr wbcCtxListUsers(struct wbcContext *ctx,
		       const char *domain_name,
		       uint32_t *_num_users,
		       const char ***_users)
{
	wbcErr wbc_status = WBC_ERR_UNKNOWN_FAILURE;
	struct winbindd_request request;
	struct winbindd_response response;
	uint32_t num_users = 0;
	const char **users = NULL;
	const char *next;

	/* Initialise request */

	ZERO_STRUCT(request);
	ZERO_STRUCT(response);

	if (domain_name) {
		strncpy(request.domain_name, domain_name,
			sizeof(request.domain_name)-1);
	}

	wbc_status = wbcRequestResponse(ctx, WINBINDD_LIST_USERS,
					&request,
					&response);
	BAIL_ON_WBC_ERROR(wbc_status);

	users = wbcAllocateStringArray(response.data.num_entries);
	if (users == NULL) {
		return WBC_ERR_NO_MEMORY;
	}

	/* Look through extra data */

	next = (const char *)response.extra_data.data;
	while (next) {
		const char *current;
		char *k;

		if (num_users >= response.data.num_entries) {
			wbc_status = WBC_ERR_INVALID_RESPONSE;
			goto done;
		}

		current = next;
		k = strchr(next, ',');

		if (k) {
			k[0] = '\0';
			next = k+1;
		} else {
			next = NULL;
		}

		users[num_users] = strdup(current);
		BAIL_ON_PTR_ERROR(users[num_users], wbc_status);
		num_users += 1;
	}
	if (num_users != response.data.num_entries) {
		wbc_status = WBC_ERR_INVALID_RESPONSE;
		goto done;
	}

	*_num_users = response.data.num_entries;
	*_users = users;
	users = NULL;
	wbc_status = WBC_ERR_SUCCESS;

 done:
	winbindd_free_response(&response);
	wbcFreeMemory(users);
	return wbc_status;
}

_PUBLIC_
wbcErr wbcListUsers(const char *domain_name,
		    uint32_t *_num_users,
		    const char ***_users)
{
	return wbcCtxListUsers(NULL, domain_name, _num_users, _users);
}

/* Lists Groups */
_PUBLIC_
wbcErr wbcCtxListGroups(struct wbcContext *ctx,
			const char *domain_name,
			uint32_t *_num_groups,
			const char ***_groups)
{
	wbcErr wbc_status = WBC_ERR_UNKNOWN_FAILURE;
	struct winbindd_request request;
	struct winbindd_response response;
	uint32_t num_groups = 0;
	const char **groups = NULL;
	const char *next;

	/* Initialise request */

	ZERO_STRUCT(request);
	ZERO_STRUCT(response);

	if (domain_name) {
		strncpy(request.domain_name, domain_name,
			sizeof(request.domain_name)-1);
	}

	wbc_status = wbcRequestResponse(ctx, WINBINDD_LIST_GROUPS,
					&request,
					&response);
	BAIL_ON_WBC_ERROR(wbc_status);

	groups = wbcAllocateStringArray(response.data.num_entries);
	if (groups == NULL) {
		return WBC_ERR_NO_MEMORY;
	}

	/* Look through extra data */

	next = (const char *)response.extra_data.data;
	while (next) {
		const char *current;
		char *k;

		if (num_groups >= response.data.num_entries) {
			wbc_status = WBC_ERR_INVALID_RESPONSE;
			goto done;
		}

		current = next;
		k = strchr(next, ',');

		if (k) {
			k[0] = '\0';
			next = k+1;
		} else {
			next = NULL;
		}

		groups[num_groups] = strdup(current);
		BAIL_ON_PTR_ERROR(groups[num_groups], wbc_status);
		num_groups += 1;
	}
	if (num_groups != response.data.num_entries) {
		wbc_status = WBC_ERR_INVALID_RESPONSE;
		goto done;
	}

	*_num_groups = response.data.num_entries;
	*_groups = groups;
	groups = NULL;
	wbc_status = WBC_ERR_SUCCESS;

 done:
	winbindd_free_response(&response);
	wbcFreeMemory(groups);
	return wbc_status;
}

_PUBLIC_
wbcErr wbcListGroups(const char *domain_name,
		     uint32_t *_num_groups,
		     const char ***_groups)
{
	return wbcCtxListGroups(NULL, domain_name, _num_groups, _groups);
}

_PUBLIC_
wbcErr wbcCtxGetDisplayName(struct wbcContext *ctx,
			    const struct wbcDomainSid *sid,
			    char **pdomain,
			    char **pfullname,
			    enum wbcSidType *pname_type)
{
	wbcErr wbc_status;
	char *domain = NULL;
	char *name = NULL;
	enum wbcSidType name_type;

	wbc_status = wbcCtxLookupSid(ctx, sid, &domain, &name, &name_type);
	BAIL_ON_WBC_ERROR(wbc_status);

	if (name_type == WBC_SID_NAME_USER) {
		uid_t uid;
		struct passwd *pwd;

		wbc_status = wbcCtxSidToUid(ctx, sid, &uid);
		BAIL_ON_WBC_ERROR(wbc_status);

		wbc_status = wbcCtxGetpwuid(ctx, uid, &pwd);
		BAIL_ON_WBC_ERROR(wbc_status);

		wbcFreeMemory(name);

		name = wbcStrDup(pwd->pw_gecos);
		wbcFreeMemory(pwd);
		BAIL_ON_PTR_ERROR(name, wbc_status);
	}

	wbc_status = WBC_ERR_SUCCESS;

 done:
	if (WBC_ERROR_IS_OK(wbc_status)) {
		*pdomain = domain;
		*pfullname = name;
		*pname_type = name_type;
	} else {
		wbcFreeMemory(domain);
		wbcFreeMemory(name);
	}

	return wbc_status;
}

_PUBLIC_
wbcErr wbcGetDisplayName(const struct wbcDomainSid *sid,
			 char **pdomain,
			 char **pfullname,
			 enum wbcSidType *pname_type)
{
	return wbcCtxGetDisplayName(NULL, sid, pdomain, pfullname, pname_type);
}

_PUBLIC_
const char* wbcSidTypeString(enum wbcSidType type)
{
	switch (type) {
	case WBC_SID_NAME_USE_NONE: return "SID_NONE";
	case WBC_SID_NAME_USER:     return "SID_USER";
	case WBC_SID_NAME_DOM_GRP:  return "SID_DOM_GROUP";
	case WBC_SID_NAME_DOMAIN:   return "SID_DOMAIN";
	case WBC_SID_NAME_ALIAS:    return "SID_ALIAS";
	case WBC_SID_NAME_WKN_GRP:  return "SID_WKN_GROUP";
	case WBC_SID_NAME_DELETED:  return "SID_DELETED";
	case WBC_SID_NAME_INVALID:  return "SID_INVALID";
	case WBC_SID_NAME_UNKNOWN:  return "SID_UNKNOWN";
	case WBC_SID_NAME_COMPUTER: return "SID_COMPUTER";
	case WBC_SID_NAME_LABEL:    return "SID_LABEL";
	default:                    return "Unknown type";
	}
}
