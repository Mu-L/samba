/*
   Unix SMB/CIFS implementation.

   PAC Glue between Samba and the KDC

   Copyright (C) Andrew Bartlett <abartlet@samba.org> 2005-2009
   Copyright (C) Simo Sorce <idra@samba.org> 2010

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
#include "kdc/authn_policy_util.h"
#include "kdc/kdc-glue.h"
#include "kdc/db-glue.h"
#include "kdc/pac-glue.h"
#include "sdb.h"
#include "sdb_hdb.h"
#include "librpc/gen_ndr/auth.h"
#include <krb5_locl.h>
#include "lib/replace/system/filesys.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_KERBEROS

static bool samba_wdc_is_s4u2self_req(astgs_request_t r)
{
	const KDC_REQ *req = kdc_request_get_req(r);
	const PA_DATA *pa_for_user = NULL;

	if (req->msg_type != krb_tgs_req) {
		return false;
	}

	if (req->padata != NULL) {
		int idx = 0;

		pa_for_user = krb5_find_padata(req->padata->val,
					       req->padata->len,
					       KRB5_PADATA_FOR_USER,
					       &idx);
	}

	if (pa_for_user != NULL) {
		return true;
	}

	return false;
}

/*
 * Given the right private pointer from hdb_samba4,
 * get a PAC from the attached ldb messages.
 *
 * For PKINIT we also get pk_reply_key and can add PAC_CREDENTIAL_INFO.
 */
static krb5_error_code samba_wdc_get_pac(void *priv,
					 astgs_request_t r,
					 hdb_entry *client,
					 hdb_entry *server,
					 const krb5_keyblock *pk_reply_key,
					 uint64_t pac_attributes,
					 krb5_pac *pac)
{
	krb5_context context = kdc_request_get_context((kdc_request_t)r);
	TALLOC_CTX *mem_ctx;
	krb5_error_code ret;
	struct samba_kdc_entry *client_entry =
		talloc_get_type_abort(client->context,
		struct samba_kdc_entry);
	const struct samba_kdc_entry *server_entry =
		talloc_get_type_abort(server->context,
		struct samba_kdc_entry);
	struct samba_kdc_entry_pac device_pac_entry = {};
	bool is_s4u2self = samba_wdc_is_s4u2self_req(r);
	uint32_t flags = 0;
	struct authn_audit_info *server_audit_info = NULL;
	NTSTATUS reply_status = NT_STATUS_OK;

	if (pac == NULL) {
		return EINVAL;
	}

	if (is_s4u2self) {
		flags |= SAMBA_KDC_FLAG_PROTOCOL_TRANSITION;
	}

	if (kdc_request_get_pkinit_freshness_used(r)) {
		flags |= SAMBA_KDC_FLAG_PKINIT_FRESHNESS_USED;
	}

	mem_ctx = talloc_named(client->context, 0, "samba_wdc_get_pac context");
	if (!mem_ctx) {
		return ENOMEM;
	}

	device_pac_entry = samba_kdc_get_device_pac(r);

	ret = krb5_pac_init(context, pac);
	if (ret != 0) {
		talloc_free(mem_ctx);
		return ret;
	}

	ret = samba_kdc_get_pac(mem_ctx,
				context,
				server_entry->kdc_db_ctx,
				flags,
				client_entry,
				server->principal,
				server_entry,
				device_pac_entry,
				pk_reply_key,
				pac_attributes,
				*pac,
				&server_audit_info,
				&reply_status);
	if (server_audit_info != NULL) {
		krb5_error_code ret2;

		ret2 = hdb_samba4_set_steal_server_audit_info(r, server_audit_info);
		if (ret == 0) {
			ret = ret2;
		}
	}
	if (!NT_STATUS_IS_OK(reply_status)) {
		krb5_error_code ret2;

		ret2 = hdb_samba4_set_ntstatus(r, reply_status, ret);
		if (ret == 0) {
			ret = ret2;
		}
	}
	if (ret) {
		krb5_pac_free(context, *pac);
		*pac = NULL;
		talloc_free(mem_ctx);
		return ret;
	}

	talloc_free(mem_ctx);
	return ret;
}

static krb5_error_code samba_wdc_verify_pac2(astgs_request_t r,
					     krb5_const_principal client_principal,
					     const hdb_entry *delegated_proxy,
					     const hdb_entry *client,
					     const hdb_entry *krbtgt,
					     const krb5_pac pac,
					     krb5_cksumtype ctype)
{
	krb5_context context = kdc_request_get_context((kdc_request_t)r);
	struct samba_kdc_entry *client_skdc_entry = NULL;
	struct samba_kdc_entry *krbtgt_skdc_entry =
		talloc_get_type_abort(krbtgt->context, struct samba_kdc_entry);
	struct samba_kdc_entry_pac client_pac_entry = {};
	TALLOC_CTX *mem_ctx = NULL;
	krb5_error_code ret;
	bool is_s4u2self = samba_wdc_is_s4u2self_req(r);
	bool is_in_db = false;
	bool is_trusted = false;
	uint32_t flags = 0;

	if (pac == NULL) {
		return EINVAL;
	}

	mem_ctx = talloc_named(NULL, 0, "samba_wdc_verify_pac2 context");
	if (mem_ctx == NULL) {
		return ENOMEM;
	}

	if (client != NULL) {
		client_skdc_entry = talloc_get_type_abort(client->context,
							  struct samba_kdc_entry);
	}

	/*
	 * If the krbtgt was generated by an RODC, and we are not that
	 * RODC, then we need to regenerate the PAC - we can't trust
	 * it, and confirm that the RODC was permitted to print this ticket
	 *
	 * Because of the samba_kdc_validate_pac_blob() step we can be
	 * sure that the record in 'client' matches the SID in the
	 * original PAC.
	 */
	ret = samba_krbtgt_is_in_db(krbtgt_skdc_entry, &is_in_db, &is_trusted);
	if (ret != 0) {
		goto out;
	}

	krb5_pac_set_trusted(pac, is_trusted);
	client_pac_entry = samba_kdc_entry_pac(pac,
					       client_principal,
					       client_skdc_entry,
					       krbtgt_skdc_entry);

	if (is_s4u2self) {
		flags |= SAMBA_KDC_FLAG_PROTOCOL_TRANSITION;
	}

	if (delegated_proxy != NULL) {
		krb5_enctype etype;
		Key *key = NULL;

		if (!is_in_db) {
			/*
			 * The RODC-issued PAC was signed by a KDC entry that we
			 * don't have a key for. The server signature is not
			 * trustworthy, since it could have been created by the
			 * server we got the ticket from. We must not proceed as
			 * otherwise the ticket signature is unchecked.
			 */
			ret = HDB_ERR_NOT_FOUND_HERE;
			goto out;
		}

		/* Fetch the correct key depending on the checksum type. */
		if (ctype == CKSUMTYPE_HMAC_MD5) {
			etype = ENCTYPE_ARCFOUR_HMAC;
		} else {
			ret = krb5_cksumtype_to_enctype(context,
							ctype,
							&etype);
			if (ret != 0) {
				goto out;
			}
		}
		ret = hdb_enctype2key(context, krbtgt, NULL, etype, &key);
		if (ret != 0) {
			goto out;
		}

		/* Check the KDC, whole-PAC and ticket signatures. */
		ret = krb5_pac_verify(context,
				      pac,
				      0,
				      NULL,
				      NULL,
				      &key->key);
		if (ret != 0) {
			DBG_WARNING("PAC KDC signature failed to verify\n");
			goto out;
		}

		flags |= SAMBA_KDC_FLAG_CONSTRAINED_DELEGATION;
	}

	ret = samba_kdc_verify_pac(mem_ctx,
				   context,
				   krbtgt_skdc_entry->kdc_db_ctx,
				   flags,
				   client_pac_entry,
				   krbtgt_skdc_entry);
	if (ret != 0) {
		goto out;
	}

out:
	talloc_free(mem_ctx);
	return ret;
}

/* Re-sign (and reform, including possibly new groups) a PAC */

static krb5_error_code samba_wdc_reget_pac(void *priv, astgs_request_t r,
					   krb5_const_principal client_principal,
					   hdb_entry *delegated_proxy,
					   krb5_const_pac delegated_proxy_pac,
					   hdb_entry *client,
					   hdb_entry *server,
					   hdb_entry *krbtgt,
					   krb5_pac *pac)
{
	krb5_context context = kdc_request_get_context((kdc_request_t)r);
	struct samba_kdc_entry *delegated_proxy_skdc_entry = NULL;
	const struct samba_kdc_entry *delegated_proxy_krbtgt_entry = NULL;
	krb5_const_principal delegated_proxy_principal = NULL;
	struct samba_kdc_entry_pac delegated_proxy_pac_entry = {};
	struct samba_kdc_entry *client_skdc_entry = NULL;
	struct samba_kdc_entry_pac client_pac_entry = {};
	struct samba_kdc_entry_pac device = {};
	const struct samba_kdc_entry *server_skdc_entry =
		talloc_get_type_abort(server->context, struct samba_kdc_entry);
	const struct samba_kdc_entry *krbtgt_skdc_entry =
		talloc_get_type_abort(krbtgt->context, struct samba_kdc_entry);
	TALLOC_CTX *mem_ctx = NULL;
	krb5_pac new_pac = NULL;
	struct authn_audit_info *server_audit_info = NULL;
	krb5_error_code ret;
	NTSTATUS reply_status = NT_STATUS_OK;
	uint32_t flags = 0;

	if (pac == NULL) {
		return EINVAL;
	}

	mem_ctx = talloc_named(NULL, 0, "samba_wdc_reget_pac context");
	if (mem_ctx == NULL) {
		return ENOMEM;
	}

	if (delegated_proxy != NULL) {
		delegated_proxy_skdc_entry = talloc_get_type_abort(delegated_proxy->context,
								   struct samba_kdc_entry);
		delegated_proxy_principal = delegated_proxy->principal;

		/*
		 * The S4U2Proxy
		 * evidence ticket could
		 * not have been signed
		 * or issued by a krbtgt
		 * trust account.
		 */
		if (!krbtgt_skdc_entry->is_krbtgt) {
			return EINVAL;
		}
		delegated_proxy_krbtgt_entry = krbtgt_skdc_entry;
	}

	delegated_proxy_pac_entry = samba_kdc_entry_pac(delegated_proxy_pac,
							delegated_proxy_principal,
							delegated_proxy_skdc_entry,
							delegated_proxy_krbtgt_entry);

	if (client != NULL) {
		client_skdc_entry = talloc_get_type_abort(client->context,
							  struct samba_kdc_entry);
	}

	device = samba_kdc_get_device_pac(r);

	ret = krb5_pac_init(context, &new_pac);
	if (ret != 0) {
		new_pac = NULL;
		goto out;
	}

	client_pac_entry = samba_kdc_entry_pac(*pac,
					       client_principal,
					       client_skdc_entry,
					       krbtgt_skdc_entry);

	if (kdc_request_get_explicit_armor_present(r)) {
		flags |= SAMBA_KDC_FLAG_EXPLICIT_ARMOR_PRESENT;
	}

	ret = samba_kdc_update_pac(mem_ctx,
				   context,
				   krbtgt_skdc_entry->kdc_db_ctx,
				   flags,
				   client_pac_entry,
				   server->principal,
				   server_skdc_entry,
				   delegated_proxy_pac_entry,
				   device,
				   new_pac,
				   &server_audit_info,
				   &reply_status);
	if (server_audit_info != NULL) {
		krb5_error_code ret2;

		ret2 = hdb_samba4_set_steal_server_audit_info(r, server_audit_info);
		if (ret == 0) {
			ret = ret2;
		}
	}
	if (!NT_STATUS_IS_OK(reply_status)) {
		krb5_error_code ret2;

		ret2 = hdb_samba4_set_ntstatus(r, reply_status, ret);
		if (ret == 0) {
			ret = ret2;
		}
	}
	if (ret != 0) {
		krb5_pac_free(context, new_pac);
		if (ret == ENOATTR) {
			krb5_pac_free(context, *pac);
			*pac = NULL;
			ret = 0;
		}
		goto out;
	}

	/* Replace the pac */
	krb5_pac_free(context, *pac);
	*pac = new_pac;

out:
	talloc_free(mem_ctx);
	return ret;
}

/* Verify a PAC's SID and signatures */

static krb5_error_code samba_wdc_verify_pac(void *priv, astgs_request_t r,
					    krb5_const_principal client_principal,
					    hdb_entry *delegated_proxy,
					    hdb_entry *client,
					    hdb_entry *_server,
					    hdb_entry *krbtgt,
					    EncTicketPart *ticket,
					    krb5_pac pac)
{
	krb5_context context = kdc_request_get_context((kdc_request_t)r);
	krb5_kdc_configuration *config = kdc_request_get_config((kdc_request_t)r);
	struct samba_kdc_entry *krbtgt_skdc_entry =
		talloc_get_type_abort(krbtgt->context,
				      struct samba_kdc_entry);
	krb5_error_code ret;
	krb5_cksumtype ctype = CKSUMTYPE_NONE;
	hdb_entry signing_krbtgt_hdb;

	if (delegated_proxy) {
		uint16_t pac_kdc_signature_rodc_id;
		const unsigned int local_tgs_rodc_id = krbtgt_skdc_entry->kdc_db_ctx->my_krbtgt_number;
		const uint16_t header_ticket_rodc_id = krbtgt->kvno >> 16;

		/*
		 * We're using delegated_proxy for the moment to indicate cases
		 * where the ticket was encrypted with the server key, and not a
		 * krbtgt key. This cannot be trusted, so we need to find a
		 * krbtgt key that signs the PAC in order to trust the ticket.
		 *
		 * The krbtgt passed in to this function refers to the krbtgt
		 * used to decrypt the ticket of the server requesting
		 * S4U2Proxy.
		 *
		 * When we implement service ticket renewal, we need to check
		 * the PAC, and this will need to be updated.
		 */
		ret = krb5_pac_get_kdc_checksum_info(context,
						     pac,
						     &ctype,
						     &pac_kdc_signature_rodc_id);
		if (ret != 0) {
			DBG_WARNING("Failed to get PAC checksum info\n");
			return ret;
		}

		/*
		 * We need to check the KDC and ticket signatures, fetching the
		 * correct key based on the enctype.
		 */
		if (local_tgs_rodc_id != 0) {
			/*
			 * If we are an RODC, and we are not the KDC that signed
			 * the evidence ticket, then we need to proxy the
			 * request.
			 */
			if (local_tgs_rodc_id != pac_kdc_signature_rodc_id) {
				return HDB_ERR_NOT_FOUND_HERE;
			}
		} else {
			/*
			 * If we are a DC, the ticket may have been signed by a
			 * different KDC than the one that issued the header
			 * ticket.
			 */
			if (pac_kdc_signature_rodc_id != header_ticket_rodc_id) {
				struct sdb_entry signing_krbtgt_sdb;

				/*
				 * Fetch our key from the database. To support
				 * key rollover, we're going to need to try
				 * multiple keys by trial and error. For now,
				 * krbtgt keys aren't assumed to change.
				 */
				ret = samba_kdc_fetch(context,
						      krbtgt_skdc_entry->kdc_db_ctx,
						      krbtgt->principal,
						      SDB_F_GET_KRBTGT | SDB_F_RODC_NUMBER_SPECIFIED | SDB_F_CANON,
						      ((uint32_t)pac_kdc_signature_rodc_id) << 16,
						      &signing_krbtgt_sdb);
				if (ret != 0) {
					return ret;
				}

				ret = sdb_entry_to_hdb_entry(context,
							     &signing_krbtgt_sdb,
							     &signing_krbtgt_hdb);
				sdb_entry_free(&signing_krbtgt_sdb);
				if (ret != 0) {
					return ret;
				}

				/*
				 * Replace the krbtgt entry with our own entry
				 * for further processing.
				 */
				krbtgt = &signing_krbtgt_hdb;
			}
		}
	} else if (!krbtgt_skdc_entry->is_trust) {
		/*
		 * We expect to have received a TGT, so check that we haven't
		 * been given a kpasswd ticket instead. We don't need to do this
		 * check for an incoming trust, as they use a different secret
		 * and can't be confused with a normal TGT.
		 */

		struct timeval now = krb5_kdc_get_time();

		/*
		 * Check if the ticket is in the last two minutes of its
		 * life.
		 */
		KerberosTime lifetime = rk_time_sub(ticket->endtime, now.tv_sec);
		if (lifetime <= CHANGEPW_LIFETIME) {
			/*
			 * This ticket has at most two minutes left to live. It
			 * may be a kpasswd ticket rather than a TGT, so don't
			 * accept it.
			 */
			kdc_audit_addreason((kdc_request_t)r,
					    "Ticket is not a ticket-granting ticket");
			return KRB5KRB_AP_ERR_TKT_EXPIRED;
		}
	}

	ret = samba_wdc_verify_pac2(r,
				    client_principal,
				    delegated_proxy,
				    client,
				    krbtgt,
				    pac,
				    ctype);

	if (krbtgt == &signing_krbtgt_hdb) {
		hdb_free_entry(context, config->db[0], &signing_krbtgt_hdb);
	}

	return ret;
}

static char *get_netbios_name(TALLOC_CTX *mem_ctx, HostAddresses *addrs)
{
	char *nb_name = NULL;
	size_t len;
	unsigned int i;

	for (i = 0; addrs && i < addrs->len; i++) {
		if (addrs->val[i].addr_type != KRB5_ADDRESS_NETBIOS) {
			continue;
		}
		len = MIN(addrs->val[i].address.length, 15);
		nb_name = talloc_strndup(mem_ctx,
					 addrs->val[i].address.data, len);
		if (nb_name) {
			break;
		}
	}

	if ((nb_name == NULL) || (nb_name[0] == '\0')) {
		return NULL;
	}

	/* Strip space padding */
	for (len = strlen(nb_name) - 1;
	     (len > 0) && (nb_name[len] == ' ');
	     --len) {
		nb_name[len] = '\0';
	}

	return nb_name;
}

static krb5_error_code samba_wdc_check_client_access(void *priv,
						     astgs_request_t r)
{
	krb5_context context = kdc_request_get_context((kdc_request_t)r);
	TALLOC_CTX *tmp_ctx = NULL;
	const hdb_entry *_client = NULL;
	struct samba_kdc_entry *client = NULL;
	struct samba_kdc_db_context *kdc_db_ctx = NULL;
	const hdb_entry *_server = NULL;
	struct samba_kdc_entry *server = NULL;
	struct samba_kdc_entry_pac device = {};
	struct authn_audit_info *client_audit_info = NULL;
	bool password_change;
	char *workstation;
	NTSTATUS nt_status;
	NTSTATUS check_device_status = NT_STATUS_OK;
	krb5_error_code ret = 0;

	/*
	 * Note _kdc_as_rep() calls _kdc_check_access()
	 * only after client and server are found
	 * in the database!
	 */
	_client = kdc_request_get_client(r);
	client = talloc_get_type_abort(_client->context, struct samba_kdc_entry);
	kdc_db_ctx = client->kdc_db_ctx;
	_server = kdc_request_get_server(r);
	server = talloc_get_type_abort(_server->context, struct samba_kdc_entry);
	/* We only have a single database! */
	SMB_ASSERT(server->kdc_db_ctx == kdc_db_ctx);

	tmp_ctx = talloc_named(client, 0, "samba_wdc_check_client_access");
	if (tmp_ctx == NULL) {
		return ENOMEM;
	}

	device = samba_kdc_get_device_pac(r);

	ret = samba_kdc_check_device(tmp_ctx,
				     context,
				     kdc_db_ctx,
				     device,
				     client->client_policy,
				     &client_audit_info,
				     &check_device_status);
	if (client_audit_info != NULL) {
		krb5_error_code ret2;

		ret2 = hdb_samba4_set_steal_client_audit_info(r, client_audit_info);
		if (ret2) {
			ret = ret2;
		}
	}
	client->reject_status = check_device_status;
	if (!NT_STATUS_IS_OK(check_device_status)) {
		krb5_error_code ret2;

		/*
		 * Add the NTSTATUS to the request so we can return it in the
		 * ‘e-data’ field later.
		 */
		ret2 = hdb_samba4_set_ntstatus(r, check_device_status, ret);
		if (ret2) {
			ret = ret2;
		}
	}

	if (ret) {
		/*
		 * As we didn’t get far enough to check the server policy, only
		 * the client policy will be referenced in the authentication
		 * log message.
		 */

		talloc_free(tmp_ctx);
		return ret;
	}

	workstation = get_netbios_name(tmp_ctx,
				       kdc_request_get_req(r)->req_body.addresses);
	password_change = _server->flags.change_pw;

	nt_status = samba_kdc_check_client_access(client,
						  kdc_request_get_cname((kdc_request_t)r),
						  workstation,
						  password_change);

	client->reject_status = nt_status;
	if (!NT_STATUS_IS_OK(nt_status)) {
		krb5_error_code ret2;

		if (NT_STATUS_EQUAL(nt_status, NT_STATUS_NO_MEMORY)) {
			talloc_free(tmp_ctx);
			return ENOMEM;
		}

		ret = samba_kdc_map_policy_err(nt_status);

		/*
		 * Add the NTSTATUS to the request so we can return it in the
		 * ‘e-data’ field later.
		 */
		ret2 = hdb_samba4_set_ntstatus(r, nt_status, ret);
		if (ret2) {
			ret = ret2;
		}

		talloc_free(tmp_ctx);
		return ret;
	}

	/* Now do the standard Heimdal check */
	talloc_free(tmp_ctx);
	return KRB5_PLUGIN_NO_HANDLE;
}

/* this function allocates 'data' using malloc.
 * The caller is responsible for freeing it */
static krb5_error_code samba_kdc_build_supported_etypes(uint32_t supported_etypes,
							krb5_data *e_data)
{
	e_data->data = malloc(4);
	if (e_data->data == NULL) {
		return ENOMEM;
	}
	e_data->length = 4;

	PUSH_LE_U32(e_data->data, 0, supported_etypes);

	return 0;
}

static krb5_error_code samba_wdc_finalize_reply(void *priv,
						astgs_request_t r)
{
	struct samba_kdc_entry *server_kdc_entry;
	uint32_t supported_enctypes;

	server_kdc_entry = talloc_get_type(kdc_request_get_server(r)->context, struct samba_kdc_entry);

	/*
	 * If the canonicalize flag is set, add PA-SUPPORTED-ENCTYPES padata
	 * type to indicate what encryption types the server supports.
	 */
	supported_enctypes = server_kdc_entry->supported_enctypes;
	if (kdc_request_get_req(r)->req_body.kdc_options.canonicalize && supported_enctypes != 0) {
		krb5_error_code ret;

		PA_DATA md;

		ret = samba_kdc_build_supported_etypes(supported_enctypes, &md.padata_value);
		if (ret != 0) {
			return ret;
		}

		md.padata_type = KRB5_PADATA_SUPPORTED_ETYPES;

		ret = kdc_request_add_encrypted_padata(r, &md);
		krb5_data_free(&md.padata_value);
		if (ret != 0) {
			return ret;
		}
	}

	return 0;
}

static krb5_error_code samba_wdc_plugin_init(krb5_context context, void **ptr)
{
	*ptr = NULL;
	return 0;
}

static void samba_wdc_plugin_fini(void *ptr)
{
	return;
}

static krb5_error_code samba_wdc_referral_policy(void *priv,
						 astgs_request_t r)
{
	return kdc_request_get_error_code((kdc_request_t)r);
}

static krb5_error_code samba_wdc_hwauth_policy(void *priv, astgs_request_t r)
{
	const hdb_entry *client = kdc_request_get_client(r);
	krb5_error_code ret = 0;

	if (client != NULL && client->flags.require_hwauth) {
		krb5_error_code ret2;

		ret = KRB5KDC_ERR_POLICY;
		ret2 = hdb_samba4_set_ntstatus(
			r, NT_STATUS_SMARTCARD_LOGON_REQUIRED, ret);
		if (ret2) {
			ret = ret2;
		}
	}

	return ret;
}

struct krb5plugin_kdc_ftable kdc_plugin_table = {
	.minor_version = KRB5_PLUGIN_KDC_VERSION_12,
	.init = samba_wdc_plugin_init,
	.fini = samba_wdc_plugin_fini,
	.pac_verify = samba_wdc_verify_pac,
	.pac_update = samba_wdc_reget_pac,
	.client_access = samba_wdc_check_client_access,
	.finalize_reply = samba_wdc_finalize_reply,
	.pac_generate = samba_wdc_get_pac,
	.referral_policy = samba_wdc_referral_policy,
	.hwauth_policy = samba_wdc_hwauth_policy,
};
