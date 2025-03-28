/*
   Unix SMB/CIFS implementation.
   test suite for lsa rpc operations

   Copyright (C) Andrew Tridgell 2003
   Copyright (C) Andrew Bartlett <abartlet@samba.org> 2004-2005

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
#include "librpc/gen_ndr/ndr_lsa_c.h"

#include "libcli/auth/libcli_auth.h"
#include "torture/rpc/torture_rpc.h"
#include "lib/cmdline/cmdline.h"
#include "param/param.h"

static void init_lsa_String(struct lsa_String *name, const char *s)
{
	name->string = s;
}

static bool test_CreateSecret_basic(struct dcerpc_pipe *p,
				    struct torture_context *tctx,
				    struct policy_handle *handle,
				    struct policy_handle *sec_handle)
{
	NTSTATUS status;
	struct lsa_CreateSecret r;
	struct lsa_SetSecret r3;
	struct lsa_QuerySecret r4;
	struct lsa_DATA_BUF buf1;
	struct lsa_DATA_BUF_PTR bufp1;
	DATA_BLOB enc_key;
	DATA_BLOB session_key;
	NTTIME old_mtime, new_mtime;
	DATA_BLOB blob1;
	const char *secret1 = "abcdef12345699qwerty";
	char *secret2;
	char *secname;
	struct dcerpc_binding_handle *b = p->binding_handle;

	secname = talloc_asprintf(tctx, "torturesecret-%08x", (unsigned int)random());

	torture_comment(tctx, "Testing CreateSecret of %s\n", secname);

	init_lsa_String(&r.in.name, secname);

	r.in.handle = handle;
	r.in.access_mask = SEC_FLAG_MAXIMUM_ALLOWED;
	r.out.sec_handle = sec_handle;

	torture_assert_ntstatus_ok(tctx, dcerpc_lsa_CreateSecret_r(b, tctx, &r),
		"CreateSecret failed");
	torture_assert_ntstatus_ok(tctx, r.out.result, "CreateSecret failed");

	status = dcerpc_binding_handle_transport_session_key(b, tctx, &session_key);
	torture_assert_ntstatus_ok(tctx, status, "transport_session_key failed");

	enc_key = sess_encrypt_string(secret1, &session_key);

	r3.in.sec_handle = sec_handle;
	r3.in.new_val = &buf1;
	r3.in.old_val = NULL;
	r3.in.new_val->data = enc_key.data;
	r3.in.new_val->length = enc_key.length;
	r3.in.new_val->size = enc_key.length;

	torture_comment(tctx, "Testing SetSecret\n");

	torture_assert_ntstatus_ok(tctx, dcerpc_lsa_SetSecret_r(b, tctx, &r3),
		"SetSecret failed");
	torture_assert_ntstatus_ok(tctx, r3.out.result, "SetSecret failed");

	r3.in.sec_handle = sec_handle;
	r3.in.new_val = &buf1;
	r3.in.old_val = NULL;
	r3.in.new_val->data = enc_key.data;
	r3.in.new_val->length = enc_key.length;
	r3.in.new_val->size = enc_key.length;

	/* break the encrypted data */
	enc_key.data[0]++;

	torture_comment(tctx, "Testing SetSecret with broken key\n");

	torture_assert_ntstatus_ok(tctx, dcerpc_lsa_SetSecret_r(b, tctx, &r3),
		"SetSecret failed");
	torture_assert_ntstatus_equal(tctx, r3.out.result, NT_STATUS_UNKNOWN_REVISION,
		"SetSecret should have failed UNKNOWN_REVISION");

	data_blob_free(&enc_key);

	ZERO_STRUCT(new_mtime);
	ZERO_STRUCT(old_mtime);

	/* fetch the secret back again */
	r4.in.sec_handle = sec_handle;
	r4.in.new_val = &bufp1;
	r4.in.new_mtime = &new_mtime;
	r4.in.old_val = NULL;
	r4.in.old_mtime = NULL;

	bufp1.buf = NULL;

	torture_comment(tctx, "Testing QuerySecret\n");
	torture_assert_ntstatus_ok(tctx, dcerpc_lsa_QuerySecret_r(b, tctx, &r4),
		"QuerySecret failed");
	torture_assert_ntstatus_ok(tctx, r4.out.result, "QuerySecret failed");
	if (r4.out.new_val == NULL || r4.out.new_val->buf == NULL)
		torture_fail(tctx, "No secret buffer returned");
	blob1.data = r4.out.new_val->buf->data;
	blob1.length = r4.out.new_val->buf->size;

	secret2 = sess_decrypt_string(tctx, &blob1, &session_key);

	torture_assert_str_equal(tctx, secret1, secret2, "Returned secret invalid");

	return true;
}

struct secret_settings {
	uint32_t bindoptions;
	bool keyexchange;
	bool ntlm2;
	bool lm_key;
};

static bool test_secrets(struct torture_context *torture, const void *_data)
{
        struct dcerpc_pipe *p;
	struct policy_handle *handle;
	struct dcerpc_binding *binding;
	const struct secret_settings *settings =
		(const struct secret_settings *)_data;
	NTSTATUS status;
	struct dcerpc_binding_handle *b;
	struct policy_handle sec_handle = {0};
	bool ok;

	lpcfg_set_cmdline(torture->lp_ctx, "ntlmssp client:keyexchange", settings->keyexchange?"True":"False");
	lpcfg_set_cmdline(torture->lp_ctx, "ntlmssp_client:ntlm2", settings->ntlm2?"True":"False");
	lpcfg_set_cmdline(torture->lp_ctx, "ntlmssp_client:lm_key", settings->lm_key?"True":"False");

	torture_assert_ntstatus_ok(torture, torture_rpc_binding(torture, &binding),
				   "Getting bindoptions");

	status = dcerpc_binding_set_flags(binding, settings->bindoptions, 0);
	torture_assert_ntstatus_ok(torture, status, "dcerpc_binding_set_flags");

	status = dcerpc_pipe_connect_b(torture, &p, binding,
				       &ndr_table_lsarpc,
				       samba_cmdline_get_creds(),
				       torture->ev,
				       torture->lp_ctx);

	torture_assert_ntstatus_ok(torture, status, "connect");
	b = p->binding_handle;

	if (!test_lsa_OpenPolicy2(b, torture, &handle)) {
		talloc_free(p);
		return false;
	}

	torture_assert(torture, handle, "OpenPolicy2 failed.  This test cannot run against this server");

	ok = test_CreateSecret_basic(p, torture, handle, &sec_handle);

	if (is_valid_policy_hnd(&sec_handle)) {
		struct lsa_DeleteObject d;

		d.in.handle = &sec_handle;
		d.out.handle = &sec_handle;

		status = dcerpc_lsa_DeleteObject_r(b, torture, &d);
		if (!NT_STATUS_IS_OK(status) ||
		    !NT_STATUS_IS_OK(d.out.result)) {
			torture_warning(torture,
					"Failed to delete secrets object");
		}
	}

	talloc_free(p);
	return ok;
}

static struct torture_tcase *add_test(struct torture_suite *suite, uint32_t bindoptions,
				     bool keyexchange, bool ntlm2, bool lm_key)
{
	char *name = NULL;
	struct secret_settings *settings;

	settings = talloc_zero(suite, struct secret_settings);
	settings->bindoptions = bindoptions;

	if (bindoptions == DCERPC_PUSH_BIGENDIAN)
		name = talloc_strdup(suite, "bigendian");
	else if (bindoptions == DCERPC_SEAL)
		name = talloc_strdup(suite, "seal");
	else if (bindoptions == 0)
		name = talloc_strdup(suite, "none");
	else
		name = talloc_strdup(suite, "unknown");

	name = talloc_asprintf_append_buffer(name, " keyexchange:%s", keyexchange?"yes":"no");
	settings->keyexchange = keyexchange;

	name = talloc_asprintf_append_buffer(name, " ntlm2:%s", ntlm2?"yes":"no");
	settings->ntlm2 = ntlm2;

	name = talloc_asprintf_append_buffer(name, " lm_key:%s", lm_key?"yes":"no");
	settings->lm_key = lm_key;

	return torture_suite_add_simple_tcase_const(suite, name, test_secrets,
			settings);
}

static const bool bool_vals[] = { true, false };

/* TEST session key correctness by pushing and pulling secrets */
struct torture_suite *torture_rpc_lsa_secrets(TALLOC_CTX *mem_ctx)
{
	struct torture_suite *suite = torture_suite_create(mem_ctx, "lsa.secrets");
	int keyexchange, ntlm2, lm_key;

	for (keyexchange = 0; keyexchange < ARRAY_SIZE(bool_vals); keyexchange++) {
		for (ntlm2 = 0; ntlm2 < ARRAY_SIZE(bool_vals); ntlm2++) {
			for (lm_key = 0; lm_key < ARRAY_SIZE(bool_vals); lm_key++) {
				add_test(suite, DCERPC_PUSH_BIGENDIAN, bool_vals[keyexchange], bool_vals[ntlm2],
					 bool_vals[lm_key]);
				add_test(suite, DCERPC_SEAL, bool_vals[keyexchange], bool_vals[ntlm2], bool_vals[lm_key]);
				add_test(suite, 0, bool_vals[keyexchange], bool_vals[ntlm2], bool_vals[lm_key]);
			}
		}
	}

	return suite;
}
