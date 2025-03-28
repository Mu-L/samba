/*
   Unix SMB/CIFS implementation.

   DNS Server

   Copyright (C) Amitay Isaacs 2011

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
#include "dnsserver.h"
#include "lib/util/dlinklist.h"
#include "librpc/gen_ndr/ndr_dnsp.h"
#include "librpc/gen_ndr/ndr_security.h"
#include "librpc/gen_ndr/ndr_misc.h"
#include "dsdb/samdb/samdb.h"
#include "libcli/security/security.h"
#include "dsdb/common/util.h"

#undef strcasecmp

/* There are only 2 fixed partitions for DNS */
struct dnsserver_partition *dnsserver_db_enumerate_partitions(TALLOC_CTX *mem_ctx,
							struct dnsserver_serverinfo *serverinfo,
							struct ldb_context *samdb)
{
	struct dnsserver_partition *partitions, *p;

	partitions = NULL;

	/* Domain partition */
	p = talloc_zero(mem_ctx, struct dnsserver_partition);
	if (p == NULL) {
		goto failed;
	}

	p->partition_dn = ldb_dn_new(p, samdb, serverinfo->pszDomainDirectoryPartition);
	if (p->partition_dn == NULL) {
		goto failed;
	}

	p->pszDpFqdn = samdb_dn_to_dns_domain(p, p->partition_dn);
	p->dwDpFlags = DNS_DP_AUTOCREATED | DNS_DP_DOMAIN_DEFAULT | DNS_DP_ENLISTED;
	p->is_forest = false;

	DLIST_ADD_END(partitions, p);

	/* Forest Partition */
	p = talloc_zero(mem_ctx, struct dnsserver_partition);
	if (p == NULL) {
		goto failed;
	}

	p->partition_dn = ldb_dn_new(p, samdb, serverinfo->pszForestDirectoryPartition);
	if (p->partition_dn == NULL) {
		goto failed;
	}

	p->pszDpFqdn = samdb_dn_to_dns_domain(p, p->partition_dn);
	p->dwDpFlags = DNS_DP_AUTOCREATED | DNS_DP_FOREST_DEFAULT | DNS_DP_ENLISTED;
	p->is_forest = true;

	DLIST_ADD_END(partitions, p);

	return partitions;

failed:
	return NULL;

}


/* Search for all dnsZone records */
struct dnsserver_zone *dnsserver_db_enumerate_zones(TALLOC_CTX *mem_ctx,
						struct ldb_context *samdb,
						struct dnsserver_partition *p)
{
	TALLOC_CTX *tmp_ctx;
	const char * const attrs[] = {"name", "dNSProperty", NULL};
	struct ldb_dn *dn;
	struct ldb_result *res;
	struct dnsserver_zone *zones, *z;
	int i, j, ret;

	tmp_ctx = talloc_new(mem_ctx);
	if (tmp_ctx == NULL) {
		return NULL;
	}

	dn = ldb_dn_copy(tmp_ctx, p->partition_dn);
	if (dn == NULL) {
		goto failed;
	}
	if (!ldb_dn_add_child_fmt(dn, "CN=MicrosoftDNS")) {
		goto failed;
	}

	ret = ldb_search(samdb, tmp_ctx, &res, dn, LDB_SCOPE_SUBTREE,
			  attrs, "(objectClass=dnsZone)");
	if (ret != LDB_SUCCESS) {
		DEBUG(0, ("dnsserver: Failed to find DNS Zones in %s\n",
			ldb_dn_get_linearized(dn)));
		goto failed;
	}

	zones = NULL;
	for(i=0; i<res->count; i++) {
		char *name;
		struct ldb_message_element *element = NULL;
		struct dnsp_DnsProperty *props = NULL;
		enum ndr_err_code err;
		z = talloc_zero(mem_ctx, struct dnsserver_zone);
		if (z == NULL) {
			goto failed;
		}

		z->partition = p;
		name = talloc_strdup(z,
				ldb_msg_find_attr_as_string(res->msgs[i],
							    "name", NULL));
		if (strcmp(name, "..TrustAnchors") == 0) {
			talloc_free(z);
			continue;
		}
		if (strcmp(name, "RootDNSServers") == 0) {
			talloc_free(name);
			z->name = talloc_strdup(z, ".");
		} else {
			z->name = name;
		}
		z->zone_dn = talloc_steal(z, res->msgs[i]->dn);

		DLIST_ADD_END(zones, z);
		DEBUG(2, ("dnsserver: Found DNS zone %s\n", z->name));

		element = ldb_msg_find_element(res->msgs[i], "dNSProperty");
		if(element != NULL){
			props = talloc_zero_array(tmp_ctx,
						  struct dnsp_DnsProperty,
						  element->num_values);
			for (j = 0; j < element->num_values; j++ ) {
				err = ndr_pull_struct_blob(
					&(element->values[j]),
					mem_ctx,
					&props[j],
					(ndr_pull_flags_fn_t)
						ndr_pull_dnsp_DnsProperty);
				if (!NDR_ERR_CODE_IS_SUCCESS(err)){
					/*
					 * Per Microsoft we must
					 * ignore invalid data here
					 * and continue as a Windows
					 * server can put in a
					 * structure with an invalid
					 * length.
					 *
					 * We can safely fill in an
					 * extra empty property here
					 * because
					 * dns_zoneinfo_load_zone_property()
					 * just ignores
					 * DSPROPERTY_ZONE_EMPTY
					 */
					ZERO_STRUCT(props[j]);
					props[j].id = DSPROPERTY_ZONE_EMPTY;
					continue;
				}
			}
			z->tmp_props = props;
			z->num_props = element->num_values;
		}
	}
	return zones;

failed:
	talloc_free(tmp_ctx);
	return NULL;
}


/* Find DNS partition information */
struct dnsserver_partition_info *dnsserver_db_partition_info(TALLOC_CTX *mem_ctx,
							struct ldb_context *samdb,
							struct dnsserver_partition *p)
{
	const char * const attrs[] = { "instanceType", "msDs-masteredBy", NULL };
	const char * const attrs_none[] = { NULL };
	struct ldb_result *res;
	struct ldb_message_element *el;
	struct ldb_dn *dn;
	struct dnsserver_partition_info *partinfo;
	int i, ret, instance_type;
	TALLOC_CTX *tmp_ctx;

	tmp_ctx = talloc_new(mem_ctx);
	if (tmp_ctx == NULL) {
		return NULL;
	}

	partinfo = talloc_zero(mem_ctx, struct dnsserver_partition_info);
	if (partinfo == NULL) {
		talloc_free(tmp_ctx);
		return NULL;
	}

	/* Search for the active replica and state */
	ret = ldb_search(samdb, tmp_ctx, &res, p->partition_dn, LDB_SCOPE_BASE,
			attrs, NULL);
	if (ret != LDB_SUCCESS || res->count != 1) {
		goto failed;
	}

	/* Set the state of the partition */
	instance_type = ldb_msg_find_attr_as_int(res->msgs[0], "instanceType", -1);
	if (instance_type == -1) {
		partinfo->dwState = DNS_DP_STATE_UNKNOWN;
	} else if (instance_type & INSTANCE_TYPE_NC_COMING) {
		partinfo->dwState = DNS_DP_STATE_REPL_INCOMING;
	} else if (instance_type & INSTANCE_TYPE_NC_GOING) {
		partinfo->dwState = DNS_DP_STATE_REPL_OUTGOING;
	} else {
		partinfo->dwState = DNS_DP_OKAY;
	}

	el = ldb_msg_find_element(res->msgs[0], "msDs-masteredBy");
	if (el == NULL) {
		partinfo->dwReplicaCount = 0;
		partinfo->ReplicaArray = NULL;
	} else {
		partinfo->dwReplicaCount = el->num_values;
		partinfo->ReplicaArray = talloc_zero_array(partinfo,
							   struct DNS_RPC_DP_REPLICA *,
							   el->num_values);
		if (partinfo->ReplicaArray == NULL) {
			goto failed;
		}
		for (i=0; i<el->num_values; i++) {
			partinfo->ReplicaArray[i] = talloc_zero(partinfo,
							struct DNS_RPC_DP_REPLICA);
			if (partinfo->ReplicaArray[i] == NULL) {
				goto failed;
			}
			partinfo->ReplicaArray[i]->pszReplicaDn = talloc_strdup(
									partinfo,
									(const char *)el->values[i].data);
			if (partinfo->ReplicaArray[i]->pszReplicaDn == NULL) {
				goto failed;
			}
		}
	}
	talloc_free(res);

	/* Search for cross-reference object */
	dn = ldb_dn_copy(tmp_ctx, ldb_get_config_basedn(samdb));
	if (dn == NULL) {
		goto failed;
	}

	ret = ldb_search(samdb, tmp_ctx, &res, dn, LDB_SCOPE_DEFAULT, attrs_none,
			"(nCName=%s)", ldb_dn_get_linearized(p->partition_dn));
	if (ret != LDB_SUCCESS || res->count != 1) {
		goto failed;
	}
	partinfo->pszCrDn = talloc_strdup(partinfo, ldb_dn_get_linearized(res->msgs[0]->dn));
	if (partinfo->pszCrDn == NULL) {
		goto failed;
	}
	talloc_free(res);

	talloc_free(tmp_ctx);
	return partinfo;

failed:
	talloc_free(tmp_ctx);
	talloc_free(partinfo);
	return NULL;
}


/* Increment serial number and update timestamp */
static unsigned int dnsserver_update_soa(TALLOC_CTX *mem_ctx,
				struct ldb_context *samdb,
				struct dnsserver_zone *z,
				WERROR *werr)
{
	const char * const attrs[] = { "dnsRecord", NULL };
	struct ldb_result *res;
	struct dnsp_DnssrvRpcRecord rec;
	struct ldb_message_element *el;
	enum ndr_err_code ndr_err;
	int ret, i, serial = -1;

	*werr = WERR_INTERNAL_DB_ERROR;

	ret = ldb_search(samdb, mem_ctx, &res, z->zone_dn, LDB_SCOPE_ONELEVEL, attrs,
			"(&(objectClass=dnsNode)(name=@))");
	if (ret != LDB_SUCCESS || res->count == 0) {
		return -1;
	}

	el = ldb_msg_find_element(res->msgs[0], "dnsRecord");
	if (el == NULL) {
		return -1;
	}

	for (i=0; i<el->num_values; i++) {
		ndr_err = ndr_pull_struct_blob(&el->values[i], mem_ctx, &rec,
					(ndr_pull_flags_fn_t)ndr_pull_dnsp_DnssrvRpcRecord);
		if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
			continue;
		}

		if (rec.wType == DNS_TYPE_SOA) {
			serial = rec.data.soa.serial + 1;
			rec.dwSerial = serial;
			rec.dwTimeStamp = 0;
			rec.data.soa.serial = serial;

			ndr_err = ndr_push_struct_blob(&el->values[i], mem_ctx, &rec,
					(ndr_push_flags_fn_t)ndr_push_dnsp_DnssrvRpcRecord);
			if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
				*werr = WERR_NOT_ENOUGH_MEMORY;
				return -1;
			}
			break;
		}
	}

	if (serial != -1) {
		el->flags = LDB_FLAG_MOD_REPLACE;
		ret = ldb_modify(samdb, res->msgs[0]);
		if (ret != LDB_SUCCESS) {
			if (ret == LDB_ERR_INSUFFICIENT_ACCESS_RIGHTS) {
				*werr = WERR_ACCESS_DENIED;
			}
			return -1;
		}
	}

	*werr = WERR_OK;

	return serial;
}


/* Add DNS record to the database */
static WERROR dnsserver_db_do_add_rec(TALLOC_CTX *mem_ctx,
				struct ldb_context *samdb,
				struct ldb_dn *dn,
				int num_rec,
				struct dnsp_DnssrvRpcRecord *rec)
{
	struct ldb_message *msg;
	struct ldb_val v;
	int ret;
	enum ndr_err_code ndr_err;
	int i;

	msg = ldb_msg_new(mem_ctx);
	W_ERROR_HAVE_NO_MEMORY(msg);

	msg->dn = dn;
	ret = ldb_msg_add_string(msg, "objectClass", "dnsNode");
	if (ret != LDB_SUCCESS) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	if (num_rec > 0 && rec) {
		for (i=0; i<num_rec; i++) {
			ndr_err = ndr_push_struct_blob(&v, mem_ctx, &rec[i],
					(ndr_push_flags_fn_t)ndr_push_dnsp_DnssrvRpcRecord);
			if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
				return WERR_GEN_FAILURE;
			}

			ret = ldb_msg_add_value(msg, "dnsRecord", &v, NULL);
			if (ret != LDB_SUCCESS) {
				return WERR_NOT_ENOUGH_MEMORY;
			}
		}
	}

	ret = ldb_add(samdb, msg);
	if (ret != LDB_SUCCESS) {
		return WERR_INTERNAL_DB_ERROR;
	}

	return WERR_OK;
}


/* Add dnsNode record to the database with DNS record */
WERROR dnsserver_db_add_empty_node(TALLOC_CTX *mem_ctx,
					struct ldb_context *samdb,
					struct dnsserver_zone *z,
					const char *name)
{
	const char * const attrs[] = { "name", NULL };
	struct ldb_result *res;
	struct ldb_dn *dn;
	char *encoded_name = ldb_binary_encode_string(mem_ctx, name);
	struct ldb_val name_val = data_blob_string_const(name);
	int ret;

	ret = ldb_search(samdb, mem_ctx, &res, z->zone_dn, LDB_SCOPE_BASE, attrs,
			"(&(objectClass=dnsNode)(name=%s))",
			 encoded_name);
	if (ret != LDB_SUCCESS) {
		return WERR_INTERNAL_DB_ERROR;
	}

	if (res->count > 0) {
		talloc_free(res);
		return WERR_DNS_ERROR_RECORD_ALREADY_EXISTS;
	}

	dn = ldb_dn_copy(mem_ctx, z->zone_dn);
	W_ERROR_HAVE_NO_MEMORY(dn);

	if (!ldb_dn_add_child_val(dn, "DC", name_val)) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	return dnsserver_db_do_add_rec(mem_ctx, samdb, dn, 0, NULL);
}

static void set_record_rank(struct dnsserver_zone *z,
			    const char *name,
			    struct dnsp_DnssrvRpcRecord *rec)
{
	if (z->zoneinfo->dwZoneType == DNS_ZONE_TYPE_PRIMARY) {
		if (strcmp(name, "@") != 0 && rec->wType == DNS_TYPE_NS) {
			rec->rank = DNS_RANK_NS_GLUE;
		} else {
			rec->rank = DNS_RANK_ZONE;
		}
	} else if (strcmp(z->name, ".") == 0) {
		rec->rank = DNS_RANK_ROOT_HINT;
	}
}


/* Add a DNS record */
WERROR dnsserver_db_add_record(TALLOC_CTX *mem_ctx,
					struct ldb_context *samdb,
					struct dnsserver_zone *z,
					const char *name,
					struct DNS_RPC_RECORD *add_record)
{
	const char * const attrs[] = { "dnsRecord", "dNSTombstoned", NULL };
	struct ldb_result *res;
	struct dnsp_DnssrvRpcRecord *rec = NULL;
	struct ldb_message_element *el;
	struct ldb_dn *dn;
	enum ndr_err_code ndr_err;
	int ret, i;
	int serial;
	WERROR werr;
	bool was_tombstoned = false;
	char *encoded_name = ldb_binary_encode_string(mem_ctx, name);

	werr = dns_to_dnsp_convert(mem_ctx, add_record, &rec, true);
	if (!W_ERROR_IS_OK(werr)) {
		return werr;
	}

	/* Set the correct rank for the record. */
	set_record_rank(z, name, rec);

	serial = dnsserver_update_soa(mem_ctx, samdb, z, &werr);
	if (serial < 0) {
		return werr;
	}

	rec->dwSerial = serial;
	rec->dwTimeStamp = 0;

	ret = ldb_search(samdb, mem_ctx, &res, z->zone_dn, LDB_SCOPE_ONELEVEL, attrs,
			"(&(objectClass=dnsNode)(name=%s))",
			 encoded_name);
	if (ret != LDB_SUCCESS) {
		return WERR_INTERNAL_DB_ERROR;
	}

	if (res->count == 0) {
		dn = dnsserver_name_to_dn(mem_ctx, z, name);
		W_ERROR_HAVE_NO_MEMORY(dn);

		return dnsserver_db_do_add_rec(mem_ctx, samdb, dn, 1, rec);
	}

	el = ldb_msg_find_element(res->msgs[0], "dnsRecord");
	if (el == NULL) {
		ret = ldb_msg_add_empty(res->msgs[0], "dnsRecord", 0, &el);
		if (ret != LDB_SUCCESS) {
			return WERR_NOT_ENOUGH_MEMORY;
		}
	}

	was_tombstoned = ldb_msg_find_attr_as_bool(res->msgs[0],
						   "dNSTombstoned", false);
	if (was_tombstoned) {
		el->num_values = 0;
	}

	for (i=0; i<el->num_values; i++) {
		struct dnsp_DnssrvRpcRecord rec2;

		ndr_err = ndr_pull_struct_blob(&el->values[i], mem_ctx, &rec2,
						(ndr_pull_flags_fn_t)ndr_pull_dnsp_DnssrvRpcRecord);
		if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
			return WERR_GEN_FAILURE;
		}

		if (dns_record_match(rec, &rec2)) {
			break;
		}
	}
	if (i < el->num_values) {
		return WERR_DNS_ERROR_RECORD_ALREADY_EXISTS;
	}
	if (i == el->num_values) {
		/* adding a new value */
		el->values = talloc_realloc(el, el->values, struct ldb_val, el->num_values+1);
		W_ERROR_HAVE_NO_MEMORY(el->values);
		el->num_values++;
	}

	ndr_err = ndr_push_struct_blob(&el->values[i], mem_ctx, rec,
					(ndr_push_flags_fn_t)ndr_push_dnsp_DnssrvRpcRecord);
	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		return WERR_GEN_FAILURE;
	}

	el->flags = LDB_FLAG_MOD_REPLACE;

	el = ldb_msg_find_element(res->msgs[0], "dNSTombstoned");
	if (el != NULL) {
		el->flags = LDB_FLAG_MOD_DELETE;
	}

	ret = ldb_modify(samdb, res->msgs[0]);
	if (ret != LDB_SUCCESS) {
		return WERR_INTERNAL_DB_ERROR;
	}

	return WERR_OK;
}


/* Update a DNS record */
WERROR dnsserver_db_update_record(TALLOC_CTX *mem_ctx,
					struct ldb_context *samdb,
					struct dnsserver_zone *z,
					const char *name,
					struct DNS_RPC_RECORD *add_record,
					struct DNS_RPC_RECORD *del_record)
{
	const char * const attrs[] = { "dnsRecord", NULL };
	struct ldb_result *res;
	struct dnsp_DnssrvRpcRecord rec2;
	struct dnsp_DnssrvRpcRecord *arec = NULL, *drec = NULL;
	struct ldb_message_element *el;
	enum ndr_err_code ndr_err;
	int ret, i;
	int serial;
	WERROR werr;
	bool updating_ttl = false;
	char *encoded_name = ldb_binary_encode_string(mem_ctx, name);

	werr = dns_to_dnsp_convert(mem_ctx, add_record, &arec, true);
	if (!W_ERROR_IS_OK(werr)) {
		return werr;
	}

	werr = dns_to_dnsp_convert(mem_ctx, del_record, &drec, true);
	if (!W_ERROR_IS_OK(werr)) {
		return werr;
	}

	ret = ldb_search(samdb, mem_ctx, &res, z->zone_dn, LDB_SCOPE_ONELEVEL, attrs,
			"(&(objectClass=dnsNode)(name=%s)(!(dNSTombstoned=TRUE)))",
			 encoded_name);
	if (ret != LDB_SUCCESS) {
		return WERR_INTERNAL_DB_ERROR;
	}

	if (res->count == 0) {
		return WERR_DNS_ERROR_RECORD_DOES_NOT_EXIST;
	}

	el = ldb_msg_find_element(res->msgs[0], "dnsRecord");
	if (el == NULL || el->num_values == 0) {
		return WERR_DNS_ERROR_RECORD_DOES_NOT_EXIST;
	}

	for (i=0; i<el->num_values; i++) {
		ndr_err = ndr_pull_struct_blob(&el->values[i], mem_ctx, &rec2,
						(ndr_pull_flags_fn_t)ndr_pull_dnsp_DnssrvRpcRecord);
		if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
			return WERR_GEN_FAILURE;
		}

		if (dns_record_match(arec, &rec2)) {
			break;
		}
	}
	if (i < el->num_values) {
		/*
		 * The record already exists, which is an error UNLESS we are
		 * doing an in-place update.
		 *
		 * Therefore we need to see if drec also matches, in which
		 * case it's OK, though we can only update dwTtlSeconds and
		 * reset the timestamp to zero.
		 */
		updating_ttl = dns_record_match(drec, &rec2);
		if (! updating_ttl) {
			return WERR_DNS_ERROR_RECORD_ALREADY_EXISTS;
		}
		/* In this case the next loop is redundant */
	}

	for (i=0; i<el->num_values; i++) {
		ndr_err = ndr_pull_struct_blob(&el->values[i], mem_ctx, &rec2,
						(ndr_pull_flags_fn_t)ndr_pull_dnsp_DnssrvRpcRecord);
		if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
			return WERR_GEN_FAILURE;
		}

		if (dns_record_match(drec, &rec2)) {
			/*
			 * we are replacing this one with arec, which is done
			 * by pushing arec into el->values[i] below, after the
			 * various manipulations.
			 */
			break;
		}
	}
	if (i == el->num_values) {
		return WERR_DNS_ERROR_RECORD_DOES_NOT_EXIST;
	}

	/*
	 * If we're updating a SOA record, use the specified serial.
	 *
	 * Otherwise, if we are updating ttl in place (i.e., not changing
	 * .wType and .data on a record), we should increment the existing
	 * serial, and save to the SOA.
	 *
	 * Outside of those two cases, we look for the zone's SOA record and
	 * use its serial.
	 */
	if (arec->wType != DNS_TYPE_SOA) {
		if (updating_ttl) {
			/*
			 * In this case, we keep some of the old values.
			 */
			arec->dwSerial = rec2.dwSerial;
			arec->dwReserved = rec2.dwReserved;
			/*
			 * TODO: if the old TTL and the new TTL are
			 * different, the serial number is incremented.
			 */
		} else {
			arec->dwReserved = 0;
			serial = dnsserver_update_soa(mem_ctx, samdb, z, &werr);
			if (serial < 0) {
				return werr;
			}
			arec->dwSerial = serial;
		}
	}

	/* Set the correct rank for the record. */
	set_record_rank(z, name, arec);
	/*
	 * Successful RPC updates *always* zero timestamp and flags and set
	 * version.
	 */
	arec->dwTimeStamp = 0;
	arec->version = 5;
	arec->flags = 0;

	ndr_err = ndr_push_struct_blob(&el->values[i], mem_ctx, arec,
					(ndr_push_flags_fn_t)ndr_push_dnsp_DnssrvRpcRecord);
	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		return WERR_GEN_FAILURE;
	}

	el->flags = LDB_FLAG_MOD_REPLACE;
	ret = ldb_modify(samdb, res->msgs[0]);
	if (ret != LDB_SUCCESS) {
		return WERR_INTERNAL_DB_ERROR;
	}

	return WERR_OK;
}


/* Delete a DNS record */
WERROR dnsserver_db_delete_record(TALLOC_CTX *mem_ctx,
					struct ldb_context *samdb,
					struct dnsserver_zone *z,
					const char *name,
					struct DNS_RPC_RECORD *del_record)
{
	const char * const attrs[] = { "dnsRecord", NULL };
	struct ldb_result *res;
	struct dnsp_DnssrvRpcRecord *rec = NULL;
	struct ldb_message_element *el;
	enum ndr_err_code ndr_err;
	int ret, i;
	int serial;
	WERROR werr;

	serial = dnsserver_update_soa(mem_ctx, samdb, z, &werr);
	if (serial < 0) {
		return werr;
	}

	werr = dns_to_dnsp_convert(mem_ctx, del_record, &rec, false);
	if (!W_ERROR_IS_OK(werr)) {
		return werr;
	}

	ret = ldb_search(samdb, mem_ctx, &res, z->zone_dn, LDB_SCOPE_ONELEVEL, attrs,
			"(&(objectClass=dnsNode)(name=%s))",
			 ldb_binary_encode_string(mem_ctx, name));
	if (ret != LDB_SUCCESS) {
		return WERR_INTERNAL_DB_ERROR;
	}

	if (res->count == 0) {
		return WERR_DNS_ERROR_RECORD_DOES_NOT_EXIST;
	}
	if (res->count > 1) {
		return WERR_DNS_ERROR_RCODE_SERVER_FAILURE;
	}

	el = ldb_msg_find_element(res->msgs[0], "dnsRecord");
	if (el == NULL || el->num_values == 0) {
		return WERR_DNS_ERROR_RECORD_DOES_NOT_EXIST;
	}

	for (i=0; i<el->num_values; i++) {
		struct dnsp_DnssrvRpcRecord rec2;

		ndr_err = ndr_pull_struct_blob(&el->values[i], mem_ctx, &rec2,
						(ndr_pull_flags_fn_t)ndr_pull_dnsp_DnssrvRpcRecord);
		if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
			return WERR_GEN_FAILURE;
		}

		if (dns_record_match(rec, &rec2)) {
			break;
		}
	}
	if (i == el->num_values) {
		return WERR_DNS_ERROR_RECORD_DOES_NOT_EXIST;
	}
	if (i < el->num_values-1) {
		memmove(&el->values[i], &el->values[i+1], sizeof(el->values[0])*((el->num_values-1)-i));
	}
	el->num_values--;

	if (el->num_values == 0) {
		ret = ldb_delete(samdb, res->msgs[0]->dn);
	} else {
		el->flags = LDB_FLAG_MOD_REPLACE;
		ret = ldb_modify(samdb, res->msgs[0]);
	}
	if (ret != LDB_SUCCESS) {
		return WERR_INTERNAL_DB_ERROR;
	}

	return WERR_OK;
}


static bool dnsserver_db_msg_add_dnsproperty(TALLOC_CTX *mem_ctx,
					     struct ldb_message *msg,
					     struct dnsp_DnsProperty *prop)
{
	DATA_BLOB *prop_blob;
	enum ndr_err_code ndr_err;
	int ret;

	prop_blob = talloc_zero(mem_ctx, DATA_BLOB);
	if (prop_blob == NULL) return false;

	ndr_err = ndr_push_struct_blob(prop_blob, mem_ctx, prop,
			(ndr_push_flags_fn_t)ndr_push_dnsp_DnsProperty);
	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		return false;
	}
	ret = ldb_msg_add_steal_value(msg, "dNSProperty", prop_blob);
	if (ret != LDB_SUCCESS) {
		return false;
	}
	return true;
}

WERROR dnsserver_db_do_reset_dword(struct ldb_context *samdb,
				   struct dnsserver_zone *z,
				   struct DNS_RPC_NAME_AND_PARAM *n_p)
{
	struct ldb_message_element *element = NULL;
	struct dnsp_DnsProperty *prop = NULL;
	enum ndr_err_code err;
	TALLOC_CTX *tmp_ctx = NULL;
	const char * const attrs[] = {"dNSProperty", NULL};
	struct ldb_result *res = NULL;
	int i, ret, prop_id;

	if (strcasecmp(n_p->pszNodeName, "Aging") == 0) {
		z->zoneinfo->fAging = n_p->dwParam;
		prop_id = DSPROPERTY_ZONE_AGING_STATE;
	} else if (strcasecmp(n_p->pszNodeName, "RefreshInterval") == 0) {
		z->zoneinfo->dwRefreshInterval = n_p->dwParam;
		prop_id = DSPROPERTY_ZONE_REFRESH_INTERVAL;
	} else if (strcasecmp(n_p->pszNodeName, "NoRefreshInterval") == 0) {
		z->zoneinfo->dwNoRefreshInterval = n_p->dwParam;
		prop_id = DSPROPERTY_ZONE_NOREFRESH_INTERVAL;
	} else if (strcasecmp(n_p->pszNodeName, "AllowUpdate") == 0) {
		z->zoneinfo->fAllowUpdate = n_p->dwParam;
		prop_id = DSPROPERTY_ZONE_ALLOW_UPDATE;
	} else {
		return WERR_UNKNOWN_PROPERTY;
	}

	tmp_ctx = talloc_new(NULL);
	if (tmp_ctx == NULL) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	ret = ldb_search(samdb, tmp_ctx, &res, z->zone_dn, LDB_SCOPE_BASE,
			 attrs, "(objectClass=dnsZone)");
	if (ret != LDB_SUCCESS) {
		DBG_ERR("dnsserver: no zone: %s\n",
			ldb_dn_get_linearized(z->zone_dn));
		TALLOC_FREE(tmp_ctx);
		return WERR_INTERNAL_DB_ERROR;
	}

	if (res->count != 1) {
		DBG_ERR("dnsserver: duplicate zone: %s\n",
			ldb_dn_get_linearized(z->zone_dn));
		TALLOC_FREE(tmp_ctx);
		return WERR_GEN_FAILURE;
	}

	element = ldb_msg_find_element(res->msgs[0], "dNSProperty");
	if (element == NULL) {
		DBG_ERR("dnsserver: zone %s has no properties.\n",
			ldb_dn_get_linearized(z->zone_dn));
		TALLOC_FREE(tmp_ctx);
		return WERR_INTERNAL_DB_ERROR;
	}

	for (i = 0; i < element->num_values; i++) {
		prop = talloc_zero(element, struct dnsp_DnsProperty);
		if (prop == NULL) {
			TALLOC_FREE(tmp_ctx);
			return WERR_NOT_ENOUGH_MEMORY;
		}
		err = ndr_pull_struct_blob(
			&(element->values[i]),
			tmp_ctx,
			prop,
			(ndr_pull_flags_fn_t)ndr_pull_dnsp_DnsProperty);
		if (!NDR_ERR_CODE_IS_SUCCESS(err)){
			/*
			 * If we can't pull it then try again parsing
			 * it again.  A Windows server in the domain
			 * will permit the addition of an invalidly
			 * formed property with a 0 length and cause a
			 * failure here
			 */
			struct dnsp_DnsProperty_short
				*short_property
				= talloc_zero(element,
					      struct dnsp_DnsProperty_short);
			if (short_property == NULL) {
				TALLOC_FREE(tmp_ctx);
				return WERR_NOT_ENOUGH_MEMORY;
			}
			err = ndr_pull_struct_blob_all(
				&(element->values[i]),
				tmp_ctx,
				short_property,
				(ndr_pull_flags_fn_t)ndr_pull_dnsp_DnsProperty_short);
			if (!NDR_ERR_CODE_IS_SUCCESS(err)) {
				/*
				 * Unknown invalid data should be
				 * ignored and logged to match Windows
				 * behaviour
				 */
				DBG_NOTICE("dnsserver: couldn't PULL "
					   "dnsProperty value#%d in "
					   "zone %s while trying to "
					   "reset id %d\n",
					   i,
					   ldb_dn_get_linearized(z->zone_dn),
					   prop_id);
				continue;
			}

			/*
			 * Initialise the parts of the property not
			 * overwritten by value() in the IDL for
			 * re-push
			 */
			*prop = (struct dnsp_DnsProperty){
				.namelength = short_property->namelength,
				.id = short_property->id,
				.name = short_property->name
				/* .data will be filled in below */
			};
		}

		if (prop->id == prop_id) {
			switch (prop_id) {
			case DSPROPERTY_ZONE_AGING_STATE:
				prop->data.aging_enabled = n_p->dwParam;
				break;
			case DSPROPERTY_ZONE_NOREFRESH_INTERVAL:
				prop->data.norefresh_hours = n_p->dwParam;
				break;
			case DSPROPERTY_ZONE_REFRESH_INTERVAL:
				prop->data.refresh_hours = n_p->dwParam;
				break;
			case DSPROPERTY_ZONE_ALLOW_UPDATE:
				prop->data.allow_update_flag = n_p->dwParam;
				break;
			}

			err = ndr_push_struct_blob(
				&(element->values[i]),
				tmp_ctx,
				prop,
				(ndr_push_flags_fn_t)ndr_push_dnsp_DnsProperty);
			if (!NDR_ERR_CODE_IS_SUCCESS(err)){
				DBG_ERR("dnsserver: couldn't PUSH dns prop id "
					"%d in zone %s\n",
					prop->id,
					ldb_dn_get_linearized(z->zone_dn));
				TALLOC_FREE(tmp_ctx);
				return WERR_INTERNAL_DB_ERROR;
			}
		}
	}

	element->flags = LDB_FLAG_MOD_REPLACE;
	ret = ldb_modify(samdb, res->msgs[0]);
	if (ret != LDB_SUCCESS) {
		TALLOC_FREE(tmp_ctx);
		DBG_ERR("dnsserver: Failed to modify zone %s prop %s: %s\n",
			z->name,
			n_p->pszNodeName,
			ldb_errstring(samdb));
		return WERR_INTERNAL_DB_ERROR;
	}
	TALLOC_FREE(tmp_ctx);

	return WERR_OK;
}

/* Create dnsZone record to database and set security descriptor */
static WERROR dnsserver_db_do_create_zone(TALLOC_CTX *tmp_ctx,
					  struct ldb_context *samdb,
					  struct ldb_dn *zone_dn,
					  struct dnsserver_zone *z)
{
	const char * const attrs[] = { "objectSID", NULL };
	struct ldb_message *msg;
	struct ldb_result *res;
	struct ldb_message_element *el;
	const char sddl_template[] = "D:AI(A;;RPWPCRCCDCLCLORCWOWDSDDTSW;;;DA)(A;;CC;;;AU)(A;;RPLCLORC;;;WD)(A;;RPWPCRCCDCLCLORCWOWDSDDTSW;;;SY)(A;CI;RPWPCRCCDCLCRCWOWDSDDTSW;;;ED)(A;CIID;RPWPCRCCDCLCRCWOWDSDDTSW;;;%s)(A;CIID;RPWPCRCCDCLCRCWOWDSDDTSW;;;ED)(OA;CIID;RPWPCR;91e647de-d96f-4b70-9557-d63ff4f3ccd8;;PS)(A;CIID;RPWPCRCCDCLCLORCWOWDSDDTSW;;;EA)(A;CIID;LC;;;RU)(A;CIID;RPWPCRCCLCLORCWOWDSDSW;;;BA)S:AI";
	char *sddl;
	struct dom_sid dnsadmins_sid;
	const struct dom_sid *domain_sid;
	struct security_descriptor *secdesc;
	struct dnsp_DnsProperty *prop;
	DATA_BLOB *sd_encoded;
	enum ndr_err_code ndr_err;
	int ret;

	/* Get DnsAdmins SID */
	ret = ldb_search(samdb, tmp_ctx, &res, ldb_get_default_basedn(samdb),
			 LDB_SCOPE_DEFAULT, attrs, "(sAMAccountName=DnsAdmins)");
	if (ret != LDB_SUCCESS || res->count != 1) {
		return WERR_INTERNAL_DB_ERROR;
	}

	el = ldb_msg_find_element(res->msgs[0], "objectSID");
	if (el == NULL || el->num_values != 1) {
		return WERR_INTERNAL_DB_ERROR;
	}

	ndr_err = ndr_pull_struct_blob(&el->values[0], tmp_ctx, &dnsadmins_sid,
				(ndr_pull_flags_fn_t)ndr_pull_dom_sid);
	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		return WERR_INTERNAL_DB_ERROR;
	}

	/* create security descriptor with DnsAdmins GUID in sddl template */
	sddl = talloc_asprintf(tmp_ctx, sddl_template,
			       dom_sid_string(tmp_ctx, &dnsadmins_sid));
	if (sddl == NULL) {
		return WERR_NOT_ENOUGH_MEMORY;
	}
	talloc_free(res);

	domain_sid = samdb_domain_sid(samdb);
	if (domain_sid == NULL) {
		return WERR_INTERNAL_DB_ERROR;
	}

	secdesc = sddl_decode(tmp_ctx, sddl, domain_sid);
	if (secdesc == NULL) {
		return WERR_GEN_FAILURE;
	}

	msg = ldb_msg_new(tmp_ctx);
	W_ERROR_HAVE_NO_MEMORY(msg);

	msg->dn = zone_dn;
	ret = ldb_msg_add_string(msg, "objectClass", "dnsZone");
	if (ret != LDB_SUCCESS) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	sd_encoded = talloc_zero(tmp_ctx, DATA_BLOB);
	W_ERROR_HAVE_NO_MEMORY(sd_encoded);

	ndr_err = ndr_push_struct_blob(sd_encoded, tmp_ctx, secdesc,
			(ndr_push_flags_fn_t)ndr_push_security_descriptor);
	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_err)) {
		return WERR_GEN_FAILURE;
	}

	ret = ldb_msg_add_steal_value(msg, "nTSecurityDescriptor", sd_encoded);
	if (ret != LDB_SUCCESS) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	/* dns zone Properties */
	prop = talloc_zero(tmp_ctx, struct dnsp_DnsProperty);
	W_ERROR_HAVE_NO_MEMORY(prop);

	prop->version = 1;

	/* zone type */
	prop->id = DSPROPERTY_ZONE_TYPE;
	prop->data.zone_type = z->zoneinfo->dwZoneType;
	if (!dnsserver_db_msg_add_dnsproperty(tmp_ctx, msg, prop)) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	/* allow update */
	prop->id = DSPROPERTY_ZONE_ALLOW_UPDATE;
	prop->data.allow_update_flag = z->zoneinfo->fAllowUpdate;
	if (!dnsserver_db_msg_add_dnsproperty(tmp_ctx, msg, prop)) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	/* secure time */
	prop->id = DSPROPERTY_ZONE_SECURE_TIME;
	prop->data.zone_secure_time = 0;
	if (!dnsserver_db_msg_add_dnsproperty(tmp_ctx, msg, prop)) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	/* norefresh interval */
	prop->id = DSPROPERTY_ZONE_NOREFRESH_INTERVAL;
	prop->data.norefresh_hours = 168;
	if (!dnsserver_db_msg_add_dnsproperty(tmp_ctx, msg, prop)) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	/* refresh interval */
	prop->id = DSPROPERTY_ZONE_REFRESH_INTERVAL;
	prop->data.refresh_hours = 168;
	if (!dnsserver_db_msg_add_dnsproperty(tmp_ctx, msg, prop)) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	/* aging state */
	prop->id = DSPROPERTY_ZONE_AGING_STATE;
	prop->data.aging_enabled = z->zoneinfo->fAging;
	if (!dnsserver_db_msg_add_dnsproperty(tmp_ctx, msg, prop)) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	/* aging enabled time */
	prop->id = DSPROPERTY_ZONE_AGING_ENABLED_TIME;
	prop->data.next_scavenging_cycle_hours = 0;
	if (!dnsserver_db_msg_add_dnsproperty(tmp_ctx, msg, prop)) {
		return WERR_NOT_ENOUGH_MEMORY;
	}

	talloc_free(prop);

	ret = ldb_add(samdb, msg);
	if (ret != LDB_SUCCESS) {
		DEBUG(0, ("dnsserver: Failed to create zone (%s): %s\n",
		      z->name, ldb_errstring(samdb)));

		if (ret == LDB_ERR_INSUFFICIENT_ACCESS_RIGHTS) {
			return WERR_ACCESS_DENIED;
		}

		return WERR_INTERNAL_DB_ERROR;
	}

	return WERR_OK;
}


/* Create new dnsZone record and @ record (SOA + NS) */
WERROR dnsserver_db_create_zone(struct ldb_context *samdb,
				struct dnsserver_partition *partitions,
				struct dnsserver_zone *zone,
				struct loadparm_context *lp_ctx)
{
	struct dnsserver_partition *p;
	bool in_forest = false;
	WERROR status;
	struct ldb_dn *dn;
	TALLOC_CTX *tmp_ctx;
	struct dnsp_DnssrvRpcRecord *dns_rec;
	struct dnsp_soa soa;
	char *soa_email = NULL;
	const char *dnsdomain = NULL;
	struct ldb_val name_val = data_blob_string_const(zone->name);

	/* We only support primary zones for now */
	if (zone->zoneinfo->dwZoneType != DNS_ZONE_TYPE_PRIMARY) {
		return WERR_CALL_NOT_IMPLEMENTED;
	}

	/* Get the correct partition */
	if (zone->partition->dwDpFlags & DNS_DP_FOREST_DEFAULT) {
		in_forest = true;
	}
	for (p = partitions; p; p = p->next) {
		if (in_forest == p->is_forest) {
			break;
		}
	}
	if (p == NULL) {
		return WERR_DNS_ERROR_DP_DOES_NOT_EXIST;
	}

	tmp_ctx = talloc_new(NULL);
	W_ERROR_HAVE_NO_MEMORY(tmp_ctx);

	dn = ldb_dn_copy(tmp_ctx, p->partition_dn);
	W_ERROR_HAVE_NO_MEMORY_AND_FREE(dn, tmp_ctx);

	if (!ldb_dn_add_child_fmt(dn, "CN=MicrosoftDNS")) {
		talloc_free(tmp_ctx);
		return WERR_NOT_ENOUGH_MEMORY;
	}

	if (!ldb_dn_add_child_val(dn, "DC", name_val)) {
		talloc_free(tmp_ctx);
		return WERR_NOT_ENOUGH_MEMORY;
	}

	/* Add dnsZone record */
	status = dnsserver_db_do_create_zone(tmp_ctx, samdb, dn, zone);
	if (!W_ERROR_IS_OK(status)) {
		talloc_free(tmp_ctx);
		return status;
	}

	if (!ldb_dn_add_child_fmt(dn, "DC=@")) {
		talloc_free(tmp_ctx);
		return WERR_NOT_ENOUGH_MEMORY;
	}

	dns_rec = talloc_zero_array(tmp_ctx, struct dnsp_DnssrvRpcRecord, 2);
	W_ERROR_HAVE_NO_MEMORY_AND_FREE(dns_rec, tmp_ctx);

	dnsdomain = lpcfg_dnsdomain(lp_ctx);
	if (dnsdomain == NULL) {
		talloc_free(tmp_ctx);
		return WERR_NOT_ENOUGH_MEMORY;
	}
	soa_email = talloc_asprintf(tmp_ctx, "hostmaster.%s", dnsdomain);
	if (soa_email == NULL) {
		talloc_free(tmp_ctx);
		return WERR_NOT_ENOUGH_MEMORY;
	}

	/* SOA Record - values same as defined in provision/sambadns.py */
	soa.serial = 1;
	soa.refresh = 900;
	soa.retry = 600;
	soa.expire = 86400;
	soa.minimum = 3600;
	soa.mname = dnsdomain;
	soa.rname = soa_email;

	dns_rec[0].wType = DNS_TYPE_SOA;
	dns_rec[0].rank = DNS_RANK_ZONE;
	dns_rec[0].dwSerial = soa.serial;
	dns_rec[0].dwTtlSeconds = 3600;
	dns_rec[0].dwTimeStamp = 0;
	dns_rec[0].data.soa = soa;

	/* NS Record */
	dns_rec[1].wType = DNS_TYPE_NS;
	dns_rec[1].rank = DNS_RANK_ZONE;
	dns_rec[1].dwSerial = soa.serial;
	dns_rec[1].dwTtlSeconds = 3600;
	dns_rec[1].dwTimeStamp = 0;
	dns_rec[1].data.ns = dnsdomain;

	/* Add @ Record */
	status = dnsserver_db_do_add_rec(tmp_ctx, samdb, dn, 2, dns_rec);

	talloc_free(tmp_ctx);
	return status;
}


/* Delete dnsZone record and all DNS records in the zone */
WERROR dnsserver_db_delete_zone(struct ldb_context *samdb,
				struct dnsserver_zone *zone)
{
	int ret;

	ret = ldb_transaction_start(samdb);
	if (ret != LDB_SUCCESS) {
		return WERR_INTERNAL_DB_ERROR;
	}

	ret = dsdb_delete(samdb, zone->zone_dn, DSDB_TREE_DELETE);
	if (ret != LDB_SUCCESS) {
		ldb_transaction_cancel(samdb);

		if (ret == LDB_ERR_INSUFFICIENT_ACCESS_RIGHTS) {
			return WERR_ACCESS_DENIED;
		}
		return WERR_INTERNAL_DB_ERROR;
	}

	ret = ldb_transaction_commit(samdb);
	if (ret != LDB_SUCCESS) {
		return WERR_INTERNAL_DB_ERROR;
	}

	return WERR_OK;
}
