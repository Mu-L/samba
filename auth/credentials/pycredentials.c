/*
   Unix SMB/CIFS implementation.
   Copyright (C) Jelmer Vernooij <jelmer@samba.org> 2007

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

#include "lib/replace/system/python.h"
#include "python/py3compat.h"
#include "includes.h"
#include "python/modules.h"
#include "pycredentials.h"
#include "param/param.h"
#include "auth/credentials/credentials_internal.h"
#include "auth/credentials/credentials_krb5.h"
#include "librpc/gen_ndr/dcerpc.h"
#include "librpc/gen_ndr/samr.h" /* for struct samr_Password */
#include "librpc/gen_ndr/netlogon.h"
#include "libcli/util/pyerrors.h"
#include "libcli/auth/libcli_auth.h"
#include "param/pyparam.h"
#include <tevent.h>
#include "libcli/auth/libcli_auth.h"
#include "system/kerberos.h"
#include "auth/kerberos/kerberos.h"
#include "libcli/smb/smb_constants.h"

static PyObject *py_creds_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
	return pytalloc_steal(type, cli_credentials_init(NULL));
}

static PyObject *PyCredentials_from_cli_credentials(struct cli_credentials *creds)
{
	return pytalloc_reference(&PyCredentials, creds);
}

static PyObject *py_creds_get_username(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	return PyString_FromStringOrNULL(cli_credentials_get_username(creds));
}

static PyObject *py_creds_set_username(PyObject *self, PyObject *args)
{
	char *newval;
	enum credentials_obtained obt = CRED_SPECIFIED;
	int _obt = obt;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "s|i", &newval, &_obt)) {
		return NULL;
	}
	obt = _obt;

	return PyBool_FromLong(cli_credentials_set_username(creds, newval, obt));
}

static PyObject *py_creds_get_ntlm_username_domain(PyObject *self, PyObject *unused)
{
	TALLOC_CTX *frame = talloc_stackframe();
	const char *user = NULL;
	const char *domain = NULL;
	PyObject *ret = NULL;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	cli_credentials_get_ntlm_username_domain(creds,
						 frame, &user, &domain);
	ret = Py_BuildValue("(ss)",
			    user,
			    domain);

	TALLOC_FREE(frame);
	return ret;
}

static PyObject *py_creds_get_ntlm_response(PyObject *self, PyObject *args, PyObject *kwargs)
{
	TALLOC_CTX *frame = talloc_stackframe();
	PyObject *ret = NULL;
	int flags;
	struct timeval tv_now;
	NTTIME server_timestamp;
	DATA_BLOB challenge = data_blob_null;
	DATA_BLOB target_info = data_blob_null;
	NTSTATUS status;
	DATA_BLOB lm_response = data_blob_null;
	DATA_BLOB nt_response = data_blob_null;
	DATA_BLOB lm_session_key = data_blob_null;
	DATA_BLOB nt_session_key = data_blob_null;
	const char *kwnames[] = { "flags", "challenge",
				  "target_info",
				  NULL };
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	tv_now = timeval_current();
	server_timestamp = timeval_to_nttime(&tv_now);

	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "is#|s#",
					 discard_const_p(char *, kwnames),
					 &flags,
					 &challenge.data,
					 &challenge.length,
					 &target_info.data,
					 &target_info.length)) {
		return NULL;
	}

	status = cli_credentials_get_ntlm_response(creds,
						   frame, &flags,
						   challenge,
						   &server_timestamp,
						   target_info,
						   &lm_response, &nt_response,
						   &lm_session_key, &nt_session_key);

	if (!NT_STATUS_IS_OK(status)) {
		PyErr_SetNTSTATUS(status);
		TALLOC_FREE(frame);
		return NULL;
	}

	ret = Py_BuildValue("{sis" PYARG_BYTES_LEN "s" PYARG_BYTES_LEN
			            "s" PYARG_BYTES_LEN "s" PYARG_BYTES_LEN "}",
			    "flags", flags,
			    "lm_response",
			    (const char *)lm_response.data, lm_response.length,
			    "nt_response",
			    (const char *)nt_response.data, nt_response.length,
			    "lm_session_key",
			    (const char *)lm_session_key.data, lm_session_key.length,
			    "nt_session_key",
			    (const char *)nt_session_key.data, nt_session_key.length);
	TALLOC_FREE(frame);
	return ret;
}

static PyObject *py_creds_get_principal(PyObject *self, PyObject *unused)
{
	TALLOC_CTX *frame = talloc_stackframe();
	PyObject *ret = NULL;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	ret = PyString_FromStringOrNULL(cli_credentials_get_principal(creds, frame));
	TALLOC_FREE(frame);
	return ret;
}

static PyObject *py_creds_set_principal(PyObject *self, PyObject *args)
{
	char *newval;
	enum credentials_obtained obt = CRED_SPECIFIED;
	int _obt = obt;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "s|i", &newval, &_obt)) {
		return NULL;
	}
	obt = _obt;

	return PyBool_FromLong(cli_credentials_set_principal(creds, newval, obt));
}

static PyObject *py_creds_get_password(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	return PyString_FromStringOrNULL(cli_credentials_get_password(creds));
}

static PyObject *py_creds_set_password(PyObject *self, PyObject *args)
{
	const char *newval = NULL;
	enum credentials_obtained obt = CRED_SPECIFIED;
	int _obt = obt;
	PyObject *result = NULL;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, PYARG_STR_UNI"|i", "utf8", &newval, &_obt)) {
		return NULL;
	}
	obt = _obt;

	result = PyBool_FromLong(cli_credentials_set_password(creds, newval, obt));
	PyMem_Free(discard_const_p(void*, newval));
	return result;
}

static PyObject *py_creds_set_utf16_password(PyObject *self, PyObject *args)
{
	enum credentials_obtained obt = CRED_SPECIFIED;
	int _obt = obt;
	PyObject *newval = NULL;
	DATA_BLOB blob = data_blob_null;
	Py_ssize_t size =  0;
	int result;
	bool ok;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "O|i", &newval, &_obt)) {
		return NULL;
	}
	obt = _obt;

	result = PyBytes_AsStringAndSize(newval, (char **)&blob.data, &size);
	if (result != 0) {
		PyErr_SetString(PyExc_RuntimeError, "Failed to convert passed value to Bytes");
		return NULL;
	}
	blob.length = size;

	ok = cli_credentials_set_utf16_password(creds,
						&blob, obt);

	return PyBool_FromLong(ok);
}

static PyObject *py_creds_get_old_password(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	return PyString_FromStringOrNULL(cli_credentials_get_old_password(creds));
}

static PyObject *py_creds_set_old_password(PyObject *self, PyObject *args)
{
	char *oldval;
	enum credentials_obtained obt = CRED_SPECIFIED;
	int _obt = obt;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "s|i", &oldval, &_obt)) {
		return NULL;
	}
	obt = _obt;

	return PyBool_FromLong(cli_credentials_set_old_password(creds, oldval, obt));
}

static PyObject *py_creds_set_old_utf16_password(PyObject *self, PyObject *args)
{
	PyObject *oldval = NULL;
	DATA_BLOB blob = data_blob_null;
	Py_ssize_t size =  0;
	int result;
	bool ok;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "O", &oldval)) {
		return NULL;
	}

	result = PyBytes_AsStringAndSize(oldval, (char **)&blob.data, &size);
	if (result != 0) {
		PyErr_SetString(PyExc_RuntimeError, "Failed to convert passed value to Bytes");
		return NULL;
	}
	blob.length = size;

	ok = cli_credentials_set_old_utf16_password(creds,
						    &blob);

	return PyBool_FromLong(ok);
}

static PyObject *py_creds_get_domain(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	return PyString_FromStringOrNULL(cli_credentials_get_domain(creds));
}

static PyObject *py_creds_set_domain(PyObject *self, PyObject *args)
{
	char *newval;
	enum credentials_obtained obt = CRED_SPECIFIED;
	int _obt = obt;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "s|i", &newval, &_obt)) {
		return NULL;
	}
	obt = _obt;

	return PyBool_FromLong(cli_credentials_set_domain(creds, newval, obt));
}

static PyObject *py_creds_get_realm(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	return PyString_FromStringOrNULL(cli_credentials_get_realm(creds));
}

static PyObject *py_creds_set_realm(PyObject *self, PyObject *args)
{
	char *newval;
	enum credentials_obtained obt = CRED_SPECIFIED;
	int _obt = obt;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "s|i", &newval, &_obt)) {
		return NULL;
	}
	obt = _obt;

	return PyBool_FromLong(cli_credentials_set_realm(creds, newval, obt));
}

static PyObject *py_creds_get_bind_dn(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	return PyString_FromStringOrNULL(cli_credentials_get_bind_dn(creds));
}

static PyObject *py_creds_set_bind_dn(PyObject *self, PyObject *args)
{
	char *newval;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	if (!PyArg_ParseTuple(args, "z", &newval))
		return NULL;

	return PyBool_FromLong(cli_credentials_set_bind_dn(creds, newval));
}

static PyObject *py_creds_get_workstation(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	return PyString_FromStringOrNULL(cli_credentials_get_workstation(creds));
}

static PyObject *py_creds_set_workstation(PyObject *self, PyObject *args)
{
	char *newval;
	enum credentials_obtained obt = CRED_SPECIFIED;
	int _obt = obt;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "s|i", &newval, &_obt)) {
		return NULL;
	}
	obt = _obt;

	return PyBool_FromLong(cli_credentials_set_workstation(creds, newval, obt));
}

static PyObject *py_creds_is_anonymous(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	return PyBool_FromLong(cli_credentials_is_anonymous(creds));
}

static PyObject *py_creds_set_anonymous(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	cli_credentials_set_anonymous(creds);
	Py_RETURN_NONE;
}

static PyObject *py_creds_authentication_requested(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
        return PyBool_FromLong(cli_credentials_authentication_requested(creds));
}

static PyObject *py_creds_wrong_password(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
         return PyBool_FromLong(cli_credentials_wrong_password(creds));
}

static PyObject *py_creds_set_cmdline_callbacks(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
        return PyBool_FromLong(cli_credentials_set_cmdline_callbacks(creds));
}

static PyObject *py_creds_parse_string(PyObject *self, PyObject *args)
{
	char *newval;
	enum credentials_obtained obt = CRED_SPECIFIED;
	int _obt = obt;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "s|i", &newval, &_obt)) {
		return NULL;
	}
	obt = _obt;

	cli_credentials_parse_string(creds, newval, obt);
	Py_RETURN_NONE;
}

static PyObject *py_creds_parse_file(PyObject *self, PyObject *args)
{
	char *newval;
	enum credentials_obtained obt = CRED_SPECIFIED;
	int _obt = obt;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "s|i", &newval, &_obt)) {
		return NULL;
	}
	obt = _obt;

	cli_credentials_parse_file(creds, newval, obt);
	Py_RETURN_NONE;
}

static PyObject *py_cli_credentials_set_password_will_be_nt_hash(PyObject *self, PyObject *args)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	PyObject *py_val = NULL;
	bool val = false;

	if (!PyArg_ParseTuple(args, "O!", &PyBool_Type, &py_val)) {
		return NULL;
	}
	val = PyObject_IsTrue(py_val);

	cli_credentials_set_password_will_be_nt_hash(creds, val);
	Py_RETURN_NONE;
}

static PyObject *py_creds_get_nt_hash(PyObject *self, PyObject *unused)
{
	PyObject *ret;
	struct samr_Password *ntpw = NULL;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	ntpw = cli_credentials_get_nt_hash(creds, creds);
	if (ntpw == NULL) {
		Py_RETURN_NONE;
	}

	ret = PyBytes_FromStringAndSize(discard_const_p(char, ntpw->hash), 16);
	TALLOC_FREE(ntpw);
	return ret;
}

static PyObject *py_creds_set_nt_hash(PyObject *self, PyObject *args)
{
	PyObject *py_cp = Py_None;
	const struct samr_Password *pwd = NULL;
	enum credentials_obtained obt = CRED_SPECIFIED;
	int _obt = obt;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "O|i", &py_cp, &_obt)) {
		return NULL;
	}
	obt = _obt;

	if (!py_check_dcerpc_type(py_cp, "samba.dcerpc.samr", "Password")) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	pwd = pytalloc_get_ptr(py_cp);

	return PyBool_FromLong(cli_credentials_set_nt_hash(creds, pwd, obt));
}

static PyObject *py_creds_get_old_nt_hash(PyObject *self, PyObject *unused)
{
	PyObject *ret;
	struct samr_Password *ntpw = NULL;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	ntpw = cli_credentials_get_old_nt_hash(creds, creds);
	if (ntpw == NULL) {
		Py_RETURN_NONE;
	}

	ret = PyBytes_FromStringAndSize(discard_const_p(char, ntpw->hash), 16);
	TALLOC_FREE(ntpw);
	return ret;
}

static PyObject *py_creds_set_old_nt_hash(PyObject *self, PyObject *args)
{
	PyObject *py_cp = Py_None;
	const struct samr_Password *pwd = NULL;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "O", &py_cp)) {
		return NULL;
	}

	if (!py_check_dcerpc_type(py_cp, "samba.dcerpc.samr", "Password")) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	pwd = pytalloc_get_ptr(py_cp);

	return PyBool_FromLong(cli_credentials_set_old_nt_hash(creds, pwd));
}

static PyObject *py_creds_get_kerberos_state(PyObject *self, PyObject *unused)
{
	int state;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	state = cli_credentials_get_kerberos_state(creds);
	return PyLong_FromLong(state);
}

static PyObject *py_creds_set_kerberos_state(PyObject *self, PyObject *args)
{
	int state;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	if (!PyArg_ParseTuple(args, "i", &state))
		return NULL;

	cli_credentials_set_kerberos_state(creds, state, CRED_SPECIFIED);
	Py_RETURN_NONE;
}

static PyObject *py_creds_set_krb_forwardable(PyObject *self, PyObject *args)
{
	int state;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	if (!PyArg_ParseTuple(args, "i", &state))
		return NULL;

	cli_credentials_set_krb_forwardable(creds, state);
	Py_RETURN_NONE;
}


static PyObject *py_creds_get_forced_sasl_mech(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	return PyString_FromStringOrNULL(cli_credentials_get_forced_sasl_mech(creds));
}

static PyObject *py_creds_set_forced_sasl_mech(PyObject *self, PyObject *args)
{
	char *newval;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "s", &newval)) {
		return NULL;
	}

	cli_credentials_set_forced_sasl_mech(creds, newval);
	Py_RETURN_NONE;
}

static PyObject *py_creds_set_conf(PyObject *self, PyObject *args)
{
	PyObject *py_lp_ctx = Py_None;
	struct loadparm_context *lp_ctx;
	TALLOC_CTX *mem_ctx;
	struct cli_credentials *creds;
	bool ok;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "|O", &py_lp_ctx)) {
		return NULL;
	}

	mem_ctx = talloc_new(NULL);
	if (mem_ctx == NULL) {
		PyErr_NoMemory();
		return NULL;
	}

	lp_ctx = lpcfg_from_py_object(mem_ctx, py_lp_ctx);
	if (lp_ctx == NULL) {
		talloc_free(mem_ctx);
		return NULL;
	}

	ok = cli_credentials_set_conf(creds, lp_ctx);
	talloc_free(mem_ctx);
	if (!ok) {
		return NULL;
	}

	Py_RETURN_NONE;
}

static PyObject *py_creds_guess(PyObject *self, PyObject *args)
{
	PyObject *py_lp_ctx = Py_None;
	struct loadparm_context *lp_ctx;
	TALLOC_CTX *mem_ctx;
	struct cli_credentials *creds;
	bool ok;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "|O", &py_lp_ctx))
		return NULL;

	mem_ctx = talloc_new(NULL);
	if (mem_ctx == NULL) {
		PyErr_NoMemory();
		return NULL;
	}

	lp_ctx = lpcfg_from_py_object(mem_ctx, py_lp_ctx);
	if (lp_ctx == NULL) {
		talloc_free(mem_ctx);
		return NULL;
	}

	ok = cli_credentials_guess(creds, lp_ctx);
	talloc_free(mem_ctx);
	if (!ok) {
		return NULL;
	}

	Py_RETURN_NONE;
}

static PyObject *py_creds_set_machine_account(PyObject *self, PyObject *args)
{
	PyObject *py_lp_ctx = Py_None;
	struct loadparm_context *lp_ctx;
	NTSTATUS status;
	struct cli_credentials *creds;
	TALLOC_CTX *mem_ctx;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "|O", &py_lp_ctx))
		return NULL;

	mem_ctx = talloc_new(NULL);
	if (mem_ctx == NULL) {
		PyErr_NoMemory();
		return NULL;
	}

	lp_ctx = lpcfg_from_py_object(mem_ctx, py_lp_ctx);
	if (lp_ctx == NULL) {
		talloc_free(mem_ctx);
		return NULL;
	}

	status = cli_credentials_set_machine_account(creds, lp_ctx);
	talloc_free(mem_ctx);

	PyErr_NTSTATUS_IS_ERR_RAISE(status);

	Py_RETURN_NONE;
}

static PyObject *PyCredentialCacheContainer_from_ccache_container(struct ccache_container *ccc)
{
	return pytalloc_reference(&PyCredentialCacheContainer, ccc);
}


static PyObject *py_creds_get_named_ccache(PyObject *self, PyObject *args)
{
	PyObject *py_lp_ctx = Py_None;
	char *ccache_name = NULL;
	struct loadparm_context *lp_ctx;
	struct ccache_container *ccc;
	struct tevent_context *event_ctx;
	int ret;
	const char *error_string;
	struct cli_credentials *creds;
	TALLOC_CTX *mem_ctx;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "|Os", &py_lp_ctx, &ccache_name))
		return NULL;

	mem_ctx = talloc_new(NULL);
	if (mem_ctx == NULL) {
		PyErr_NoMemory();
		return NULL;
	}

	lp_ctx = lpcfg_from_py_object(mem_ctx, py_lp_ctx);
	if (lp_ctx == NULL) {
		talloc_free(mem_ctx);
		return NULL;
	}

	event_ctx = samba_tevent_context_init(mem_ctx);

	ret = cli_credentials_get_named_ccache(creds, event_ctx, lp_ctx,
					       ccache_name, &ccc, &error_string);
	talloc_unlink(mem_ctx, lp_ctx);
	if (ret == 0) {
		talloc_steal(ccc, event_ctx);
		talloc_free(mem_ctx);
		return PyCredentialCacheContainer_from_ccache_container(ccc);
	}

	PyErr_SetString(PyExc_RuntimeError, error_string?error_string:"NULL");

	talloc_free(mem_ctx);
	return NULL;
}

static PyObject *py_creds_set_named_ccache(PyObject *self, PyObject *args)
{
	struct loadparm_context *lp_ctx = NULL;
	enum credentials_obtained obt = CRED_SPECIFIED;
	const char *error_string = NULL;
	TALLOC_CTX *mem_ctx = NULL;
	char *newval = NULL;
	PyObject *py_lp_ctx = Py_None;
	int _obt = obt;
	int ret;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "s|iO", &newval, &_obt, &py_lp_ctx))
		return NULL;
	obt = _obt;

	mem_ctx = talloc_new(NULL);
	if (mem_ctx == NULL) {
		PyErr_NoMemory();
		return NULL;
	}

	lp_ctx = lpcfg_from_py_object(mem_ctx, py_lp_ctx);
	if (lp_ctx == NULL) {
		talloc_free(mem_ctx);
		return NULL;
	}

	ret = cli_credentials_set_ccache(creds,
					 lp_ctx,
					 newval, obt,
					 &error_string);

	if (ret != 0) {
		PyErr_SetString(PyExc_RuntimeError,
				error_string != NULL ? error_string : "NULL");
		talloc_free(mem_ctx);
		return NULL;
	}

	talloc_free(mem_ctx);
	Py_RETURN_NONE;
}

static PyObject *py_creds_set_gensec_features(PyObject *self, PyObject *args)
{
	unsigned int gensec_features;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "I", &gensec_features))
		return NULL;

	cli_credentials_set_gensec_features(creds,
					    gensec_features,
					    CRED_SPECIFIED);

	Py_RETURN_NONE;
}

static PyObject *py_creds_get_gensec_features(PyObject *self, PyObject *args)
{
	unsigned int gensec_features;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	gensec_features = cli_credentials_get_gensec_features(creds);
	return PyLong_FromLong(gensec_features);
}

static PyObject *py_creds_new_client_authenticator(PyObject *self,
						   PyObject *args)
{
	struct netr_Authenticator auth;
	struct cli_credentials *creds = NULL;
	struct netlogon_creds_CredentialState *nc = NULL;
	PyObject *ret = NULL;
	NTSTATUS status;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_SetString(PyExc_RuntimeError,
				"Failed to get credentials from python");
		return NULL;
	}

	nc = creds->netlogon_creds;
	if (nc == NULL) {
		PyErr_SetString(PyExc_ValueError,
				"No netlogon credentials cannot make "
				"client authenticator");
		return NULL;
	}

	status = netlogon_creds_client_authenticator(nc, &auth);
	if (!NT_STATUS_IS_OK(status)) {
		PyErr_SetString(PyExc_ValueError,
				"Failed to create client authenticator");
		return NULL;
	}

	ret = Py_BuildValue("{s"PYARG_BYTES_LEN"si}",
			    "credential",
			    (const char *) &auth.cred, sizeof(auth.cred),
			    "timestamp", auth.timestamp);
	return ret;
}

static PyObject *py_creds_set_secure_channel_type(PyObject *self, PyObject *args)
{
	unsigned int channel_type;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "I", &channel_type))
		return NULL;

	cli_credentials_set_secure_channel_type(
		creds,
		channel_type);

	Py_RETURN_NONE;
}

static PyObject *py_creds_get_secure_channel_type(PyObject *self, PyObject *args)
{
	enum netr_SchannelType channel_type = SEC_CHAN_NULL;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	channel_type = cli_credentials_get_secure_channel_type(creds);

	return PyLong_FromLong(channel_type);
}

static PyObject *py_creds_get_netlogon_creds(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = NULL;
	struct netlogon_creds_CredentialState *ncreds = NULL;
	PyObject *py_ncreds = Py_None;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (creds->netlogon_creds == NULL) {
		Py_RETURN_NONE;
	}

	ncreds = netlogon_creds_copy(NULL, creds->netlogon_creds);
	if (ncreds == NULL) {
		PyErr_NoMemory();
		return NULL;
	}

	py_ncreds = py_return_ndr_struct("samba.dcerpc.schannel",
					 "netlogon_creds_CredentialState",
					 ncreds,
					 ncreds);
	if (py_ncreds == NULL) {
		TALLOC_FREE(ncreds);
		return NULL;
	}

	return py_ncreds;
}

static PyObject *py_creds_set_netlogon_creds(PyObject *self, PyObject *args)
{
	struct cli_credentials *creds = NULL;
	const struct netlogon_creds_CredentialState *ncreds = NULL;
	PyObject *py_ncreds = Py_None;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "O", &py_ncreds))
		return NULL;

	if (py_ncreds == Py_None) {
		ncreds = NULL;
	} else {
		bool ok;

		ok = py_check_dcerpc_type(py_ncreds,
				  "samba.dcerpc.schannel",
				  "netlogon_creds_CredentialState");
		if (!ok) {
			/* py_check_dcerpc_type sets TypeError */
			return NULL;
		}

		ncreds = pytalloc_get_type(py_ncreds,
					   struct netlogon_creds_CredentialState);
		if (ncreds == NULL) {
			/* pytalloc_get_type sets TypeError */
			return NULL;
		}
	}

	cli_credentials_set_netlogon_creds(creds, ncreds);
	if (ncreds != NULL && creds->netlogon_creds == NULL) {
		PyErr_NoMemory();
		return NULL;
	}

	Py_RETURN_NONE;
}

static PyObject *py_creds_set_kerberos_salt_principal(PyObject *self, PyObject *args)
{
	char *salt_principal = NULL;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "s", &salt_principal))
		return NULL;

	cli_credentials_set_salt_principal(
		creds,
		salt_principal);

	Py_RETURN_NONE;
}

static PyObject *py_creds_get_kerberos_salt_principal(PyObject *self, PyObject *unused)
{
	TALLOC_CTX *mem_ctx;
	PyObject *ret = NULL;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	mem_ctx = talloc_new(NULL);
	if (mem_ctx == NULL) {
		PyErr_NoMemory();
		return NULL;
	}

	ret = PyString_FromStringOrNULL(cli_credentials_get_salt_principal(creds, mem_ctx));

	TALLOC_FREE(mem_ctx);

	return ret;
}

static PyObject *py_creds_get_kerberos_key_current_or_old(PyObject *self, PyObject *args, bool old)
{
	struct loadparm_context *lp_ctx = NULL;
	TALLOC_CTX *mem_ctx = NULL;
	PyObject *py_lp_ctx = Py_None;
	DATA_BLOB key;
	int code;
	int enctype;
	PyObject *ret = NULL;
	struct cli_credentials *creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "i|O", &enctype, &py_lp_ctx))
		return NULL;

	mem_ctx = talloc_new(NULL);
	if (mem_ctx == NULL) {
		PyErr_NoMemory();
		return NULL;
	}

	lp_ctx = lpcfg_from_py_object(mem_ctx, py_lp_ctx);
	if (lp_ctx == NULL) {
		talloc_free(mem_ctx);
		return NULL;
	}

	code = cli_credentials_get_kerberos_key(creds,
						mem_ctx,
						lp_ctx,
						enctype,
						old,
						&key);
	if (code != 0) {
		PyErr_SetString(PyExc_RuntimeError,
				"Failed to generate Kerberos key");
		talloc_free(mem_ctx);
		return NULL;
	}

	ret = PyBytes_FromStringAndSize((const char *)key.data,
					key.length);
	talloc_free(mem_ctx);
	return ret;
}

static PyObject *py_creds_get_kerberos_key(PyObject *self, PyObject *args)
{
	return py_creds_get_kerberos_key_current_or_old(self, args, false);
}

static PyObject *py_creds_get_old_kerberos_key(PyObject *self, PyObject *args)
{
	return py_creds_get_kerberos_key_current_or_old(self, args, true);
}

static PyObject *py_creds_encrypt_netr_crypt_password(PyObject *self,
						      PyObject *args)
{
	struct cli_credentials    *creds  = NULL;
	struct netr_CryptPassword *pwd    = NULL;
	struct samr_CryptPassword spwd;
	enum dcerpc_AuthType auth_type = DCERPC_AUTH_TYPE_NONE;
	enum dcerpc_AuthLevel auth_level = DCERPC_AUTH_LEVEL_NONE;
	NTSTATUS status;
	PyObject *py_cp = Py_None;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (!PyArg_ParseTuple(args, "O", &py_cp)) {
		return NULL;
	}

	if (!py_check_dcerpc_type(py_cp, "samba.dcerpc.netlogon", "netr_CryptPassword")) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	pwd = pytalloc_get_ptr(py_cp);
	if (pwd == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	memcpy(spwd.data, pwd->data, 512);
	PUSH_LE_U32(spwd.data, 512, pwd->length);

	status = netlogon_creds_encrypt_samr_CryptPassword(creds->netlogon_creds,
							   &spwd,
							   auth_type,
							   auth_level);

	memcpy(pwd->data, spwd.data, 512);
	pwd->length = PULL_LE_U32(spwd.data, 512);
	ZERO_STRUCT(spwd);

	PyErr_NTSTATUS_IS_ERR_RAISE(status);

	Py_RETURN_NONE;
}

static PyObject *py_creds_encrypt_netr_PasswordInfo(PyObject *self,
						    PyObject *args,
						    PyObject *kwargs)
{
	const char * const kwnames[] = {
		"info",
		"auth_type",
		"auth_level",
		NULL
	};
	struct cli_credentials *creds = NULL;
	PyObject *py_info = Py_None;
	enum netr_LogonInfoClass level = NetlogonInteractiveInformation;
	union netr_LogonLevel logon = { .password = NULL, };
	uint8_t auth_type = DCERPC_AUTH_TYPE_NONE;
	uint8_t auth_level = DCERPC_AUTH_LEVEL_NONE;
	NTSTATUS status;
	bool ok;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	if (creds->netlogon_creds == NULL) {
		PyErr_Format(PyExc_ValueError, "NetLogon credentials not set");
		return NULL;
	}

	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "Obb",
					 discard_const_p(char *, kwnames),
					 &py_info, &auth_type, &auth_level))
	{
		return NULL;
	}

	ok = py_check_dcerpc_type(py_info,
				  "samba.dcerpc.netlogon",
				  "netr_PasswordInfo");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	logon.password = pytalloc_get_type(py_info, struct netr_PasswordInfo);
	if (logon.password == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	status = netlogon_creds_encrypt_samlogon_logon(creds->netlogon_creds,
						       level,
						       &logon,
						       auth_type,
						       auth_level);

	PyErr_NTSTATUS_IS_ERR_RAISE(status);

	Py_RETURN_NONE;
}

static PyObject *py_creds_get_smb_signing(PyObject *self, PyObject *unused)
{
	enum smb_signing_setting signing_state;
	struct cli_credentials *creds = NULL;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	signing_state = cli_credentials_get_smb_signing(creds);
	return PyLong_FromLong(signing_state);
}

static PyObject *py_creds_set_smb_signing(PyObject *self, PyObject *args)
{
	enum smb_signing_setting signing_state;
	struct cli_credentials *creds = NULL;
	enum credentials_obtained obt = CRED_SPECIFIED;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	if (!PyArg_ParseTuple(args, "i|i", &signing_state, &obt)) {
		return NULL;
	}

	switch (signing_state) {
	case SMB_SIGNING_DEFAULT:
	case SMB_SIGNING_OFF:
	case SMB_SIGNING_IF_REQUIRED:
	case SMB_SIGNING_DESIRED:
	case SMB_SIGNING_REQUIRED:
		break;
	default:
		PyErr_Format(PyExc_TypeError, "Invalid signing state value");
		return NULL;
	}

	cli_credentials_set_smb_signing(creds, signing_state, obt);
	Py_RETURN_NONE;
}

static PyObject *py_creds_get_smb_ipc_signing(PyObject *self, PyObject *unused)
{
	enum smb_signing_setting signing_state;
	struct cli_credentials *creds = NULL;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	signing_state = cli_credentials_get_smb_ipc_signing(creds);
	return PyLong_FromLong(signing_state);
}

static PyObject *py_creds_set_smb_ipc_signing(PyObject *self, PyObject *args)
{
	enum smb_signing_setting signing_state;
	struct cli_credentials *creds = NULL;
	enum credentials_obtained obt = CRED_SPECIFIED;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	if (!PyArg_ParseTuple(args, "i|i", &signing_state, &obt)) {
		return NULL;
	}

	switch (signing_state) {
	case SMB_SIGNING_DEFAULT:
	case SMB_SIGNING_OFF:
	case SMB_SIGNING_IF_REQUIRED:
	case SMB_SIGNING_DESIRED:
	case SMB_SIGNING_REQUIRED:
		break;
	default:
		PyErr_Format(PyExc_TypeError, "Invalid signing state value");
		return NULL;
	}

	cli_credentials_set_smb_ipc_signing(creds, signing_state, obt);
	Py_RETURN_NONE;
}

static PyObject *py_creds_get_smb_encryption(PyObject *self, PyObject *unused)
{
	enum smb_encryption_setting encryption_state;
	struct cli_credentials *creds = NULL;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	encryption_state = cli_credentials_get_smb_encryption(creds);
	return PyLong_FromLong(encryption_state);
}

static PyObject *py_creds_set_smb_encryption(PyObject *self, PyObject *args)
{
	enum smb_encryption_setting encryption_state;
	struct cli_credentials *creds = NULL;
	enum credentials_obtained obt = CRED_SPECIFIED;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	if (!PyArg_ParseTuple(args, "i|i", &encryption_state, &obt)) {
		return NULL;
	}

	switch (encryption_state) {
	case SMB_ENCRYPTION_DEFAULT:
	case SMB_ENCRYPTION_OFF:
	case SMB_ENCRYPTION_IF_REQUIRED:
	case SMB_ENCRYPTION_DESIRED:
	case SMB_ENCRYPTION_REQUIRED:
		break;
	default:
		PyErr_Format(PyExc_TypeError, "Invalid encryption state value");
		return NULL;
	}

	(void)cli_credentials_set_smb_encryption(creds, encryption_state, obt);
	Py_RETURN_NONE;
}

static PyObject *py_creds_get_krb5_fast_armor_credentials(PyObject *self, PyObject *unused)
{
	struct cli_credentials *creds = NULL;
	struct cli_credentials *fast_creds = NULL;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	fast_creds = cli_credentials_get_krb5_fast_armor_credentials(creds);
	if (fast_creds == NULL) {
		Py_RETURN_NONE;
	}

	return PyCredentials_from_cli_credentials(fast_creds);
}

static PyObject *py_creds_set_krb5_fast_armor_credentials(PyObject *self, PyObject *args)
{
	struct cli_credentials *creds = NULL;
	PyObject *pyfast_creds;
	struct cli_credentials *fast_creds = NULL;
	int fast_armor_required = 0;
	NTSTATUS status;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}
	if (!PyArg_ParseTuple(args, "Op", &pyfast_creds, &fast_armor_required)) {
		return NULL;
	}
	if (pyfast_creds == Py_None) {
		fast_creds = NULL;
	} else {
		fast_creds = PyCredentials_AsCliCredentials(pyfast_creds);
		if (fast_creds == NULL) {
			PyErr_Format(PyExc_TypeError, "Credentials expected");
			return NULL;
		}
	}

	status = cli_credentials_set_krb5_fast_armor_credentials(creds,
								 fast_creds,
								 fast_armor_required);

	PyErr_NTSTATUS_IS_ERR_RAISE(status);
	Py_RETURN_NONE;
}

static PyObject *py_creds_get_krb5_require_fast_armor(PyObject *self, PyObject *unused)
{
	bool krb5_fast_armor_required;
	struct cli_credentials *creds = NULL;

	creds = PyCredentials_AsCliCredentials(self);
	if (creds == NULL) {
		PyErr_Format(PyExc_TypeError, "Credentials expected");
		return NULL;
	}

	krb5_fast_armor_required = cli_credentials_get_krb5_require_fast_armor(creds);
	return PyBool_FromLong(krb5_fast_armor_required);
}

static PyMethodDef py_creds_methods[] = {
	{
		.ml_name  = "get_username",
		.ml_meth  = py_creds_get_username,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.get_username() -> username\nObtain username.",
	},
	{
		.ml_name  = "set_username",
		.ml_meth  = py_creds_set_username,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.set_username(name[, credentials.SPECIFIED]) -> None\n"
			    "Change username.",
	},
	{
		.ml_name  = "get_principal",
		.ml_meth  = py_creds_get_principal,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.get_principal() -> user@realm\nObtain user principal.",
	},
	{
		.ml_name  = "set_principal",
		.ml_meth  = py_creds_set_principal,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.set_principal(name[, credentials.SPECIFIED]) -> None\n"
			    "Change principal.",
	},
	{
		.ml_name  = "get_password",
		.ml_meth  = py_creds_get_password,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.get_password() -> password\n"
			    "Obtain password.",
	},
	{
		.ml_name  = "get_ntlm_username_domain",
		.ml_meth  = py_creds_get_ntlm_username_domain,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.get_ntlm_username_domain() -> (domain, username)\n"
			    "Obtain NTLM username and domain, split up either as (DOMAIN, user) or (\"\", \"user@realm\").",
	},
	{
		.ml_name  = "get_ntlm_response",
		.ml_meth  = PY_DISCARD_FUNC_SIG(PyCFunction,
						py_creds_get_ntlm_response),
		.ml_flags = METH_VARARGS | METH_KEYWORDS,
		.ml_doc   = "S.get_ntlm_response"
		            "(flags, challenge[, target_info]) -> "
			    "(flags, lm_response, nt_response, lm_session_key, nt_session_key)\n"
			    "Obtain LM or NTLM response.",
	},
	{
		.ml_name  = "set_password",
		.ml_meth  = py_creds_set_password,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.set_password(password[, credentials.SPECIFIED]) -> None\n"
			    "Change password.",
	},
	{
		.ml_name  = "set_utf16_password",
		.ml_meth  = py_creds_set_utf16_password,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.set_utf16_password(password[, credentials.SPECIFIED]) -> None\n"
			    "Change password.",
	},
	{
		.ml_name  = "get_old_password",
		.ml_meth  = py_creds_get_old_password,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.get_old_password() -> password\n"
			    "Obtain old password.",
	},
	{
		.ml_name  = "set_old_password",
		.ml_meth  = py_creds_set_old_password,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.set_old_password(password[, credentials.SPECIFIED]) -> None\n"
			    "Change old password.",
	},
	{
		.ml_name  = "set_old_utf16_password",
		.ml_meth  = py_creds_set_old_utf16_password,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.set_old_utf16_password(password[, credentials.SPECIFIED]) -> None\n"
			    "Change old password.",
	},
	{
		.ml_name  = "get_domain",
		.ml_meth  = py_creds_get_domain,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.get_domain() -> domain\n"
			    "Obtain domain name.",
	},
	{
		.ml_name  = "set_domain",
		.ml_meth  = py_creds_set_domain,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.set_domain(domain[, credentials.SPECIFIED]) -> None\n"
			    "Change domain name.",
	},
	{
		.ml_name  = "get_realm",
		.ml_meth  = py_creds_get_realm,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.get_realm() -> realm\n"
			    "Obtain realm name.",
	},
	{
		.ml_name  = "set_realm",
		.ml_meth  = py_creds_set_realm,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.set_realm(realm[, credentials.SPECIFIED]) -> None\n"
			    "Change realm name.",
	},
	{
		.ml_name  = "get_bind_dn",
		.ml_meth  = py_creds_get_bind_dn,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.get_bind_dn() -> bind dn\n"
			    "Obtain bind DN.",
	},
	{
		.ml_name  = "set_bind_dn",
		.ml_meth  = py_creds_set_bind_dn,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.set_bind_dn(bind_dn) -> None\n"
			    "Change bind DN.",
	},
	{
		.ml_name  = "is_anonymous",
		.ml_meth  = py_creds_is_anonymous,
		.ml_flags = METH_NOARGS,
	},
	{
		.ml_name  = "set_anonymous",
		.ml_meth  = py_creds_set_anonymous,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.set_anonymous() -> None\n"
			    "Use anonymous credentials.",
	},
	{
		.ml_name  = "get_workstation",
		.ml_meth  = py_creds_get_workstation,
		.ml_flags = METH_NOARGS,
	},
	{
		.ml_name  = "set_workstation",
		.ml_meth  = py_creds_set_workstation,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "authentication_requested",
		.ml_meth  = py_creds_authentication_requested,
		.ml_flags = METH_NOARGS,
	},
	{
		.ml_name  = "wrong_password",
		.ml_meth  = py_creds_wrong_password,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.wrong_password() -> bool\n"
			    "Indicate the returned password was incorrect.",
	},
	{
		.ml_name  = "set_cmdline_callbacks",
		.ml_meth  = py_creds_set_cmdline_callbacks,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.set_cmdline_callbacks() -> bool\n"
			    "Use command-line to obtain credentials not explicitly set.",
	},
	{
		.ml_name  = "parse_string",
		.ml_meth  = py_creds_parse_string,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.parse_string(text[, credentials.SPECIFIED]) -> None\n"
			    "Parse credentials string.",
	},
	{
		.ml_name  = "parse_file",
		.ml_meth  = py_creds_parse_file,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.parse_file(filename[, credentials.SPECIFIED]) -> None\n"
			    "Parse credentials file.",
	},
	{
		.ml_name  = "set_password_will_be_nt_hash",
		.ml_meth  = py_cli_credentials_set_password_will_be_nt_hash,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.set_password_will_be_nt_hash(bool) -> None\n"
			    "Alters the behaviour of S.set_password() "
			    "to expect the NTHASH as hexstring.",
	},
	{
		.ml_name  = "get_nt_hash",
		.ml_meth  = py_creds_get_nt_hash,
		.ml_flags = METH_NOARGS,
	},
	{
		.ml_name  = "set_nt_hash",
		.ml_meth  = py_creds_set_nt_hash,
		.ml_flags = METH_VARARGS,
		.ml_doc = "S.set_nt_hash(samr_Password[, credentials.SPECIFIED]) -> bool\n"
			"Change NT hash.",
	},
	{
		.ml_name  = "get_old_nt_hash",
		.ml_meth  = py_creds_get_old_nt_hash,
		.ml_flags = METH_NOARGS,
	},
	{
		.ml_name  = "set_old_nt_hash",
		.ml_meth  = py_creds_set_old_nt_hash,
		.ml_flags = METH_VARARGS,
		.ml_doc = "S.set_old_nt_hash(samr_Password) -> bool\n"
			"Change old NT hash.",
	},
	{
		.ml_name  = "get_kerberos_state",
		.ml_meth  = py_creds_get_kerberos_state,
		.ml_flags = METH_NOARGS,
	},
	{
		.ml_name  = "set_kerberos_state",
		.ml_meth  = py_creds_set_kerberos_state,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "set_krb_forwardable",
		.ml_meth  = py_creds_set_krb_forwardable,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "set_conf",
		.ml_meth  = py_creds_set_conf,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "guess",
		.ml_meth  = py_creds_guess,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "set_machine_account",
		.ml_meth  = py_creds_set_machine_account,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "get_named_ccache",
		.ml_meth  = py_creds_get_named_ccache,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "set_named_ccache",
		.ml_meth  = py_creds_set_named_ccache,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.set_named_ccache(krb5_ccache_name, obtained, lp) -> None\n"
			    "Set credentials to KRB5 Credentials Cache (by name).",
	},
	{
		.ml_name  = "set_gensec_features",
		.ml_meth  = py_creds_set_gensec_features,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "get_gensec_features",
		.ml_meth  = py_creds_get_gensec_features,
		.ml_flags = METH_NOARGS,
	},
	{
		.ml_name  = "get_forced_sasl_mech",
		.ml_meth  = py_creds_get_forced_sasl_mech,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.get_forced_sasl_mech() -> SASL mechanism\nObtain forced SASL mechanism.",
	},
	{
		.ml_name  = "set_forced_sasl_mech",
		.ml_meth  = py_creds_set_forced_sasl_mech,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.set_forced_sasl_mech(name) -> None\n"
			    "Set forced SASL mechanism.",
	},
	{
		.ml_name  = "new_client_authenticator",
		.ml_meth  = py_creds_new_client_authenticator,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.new_client_authenticator() -> Authenticator\n"
			    "Get a new client NETLOGON_AUTHENTICATOR"},
	{
		.ml_name  = "set_secure_channel_type",
		.ml_meth  = py_creds_set_secure_channel_type,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "get_secure_channel_type",
		.ml_meth  = py_creds_get_secure_channel_type,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "get_netlogon_creds",
		.ml_meth  = py_creds_get_netlogon_creds,
		.ml_flags = METH_NOARGS,
	},
	{
		.ml_name  = "set_netlogon_creds",
		.ml_meth  = py_creds_set_netlogon_creds,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "set_kerberos_salt_principal",
		.ml_meth  = py_creds_set_kerberos_salt_principal,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "get_kerberos_salt_principal",
		.ml_meth  = py_creds_get_kerberos_salt_principal,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "get_kerberos_key",
		.ml_meth  = py_creds_get_kerberos_key,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.get_kerberos_key(enctype, [lp]) -> bytes\n"
			    "Generate a Kerberos key using the current password and\n"
			    "the salt on this credentials object",
	},
	{
		.ml_name  = "get_old_kerberos_key",
		.ml_meth  = py_creds_get_old_kerberos_key,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.get_old_kerberos_key(enctype, [lp]) -> bytes\n"
			    "Generate a Kerberos key using the old (previous) password and\n"
			    "the salt on this credentials object",
	},
	{
		.ml_name  = "encrypt_netr_crypt_password",
		.ml_meth  = py_creds_encrypt_netr_crypt_password,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.encrypt_netr_crypt_password(password) -> None\n"
			    "Encrypt the supplied password using the session key and\n"
			    "the negotiated encryption algorithm in place\n"
			    "i.e. it overwrites the original data"},
	{
		.ml_name  = "encrypt_netr_PasswordInfo",
		.ml_meth  = PY_DISCARD_FUNC_SIG(PyCFunction,
					py_creds_encrypt_netr_PasswordInfo),
		.ml_flags = METH_VARARGS | METH_KEYWORDS,
		.ml_doc   = "S.encrypt_netr_PasswordInfo(info, "
			    "auth_type, auth_level) -> None\n"
			    "Encrypt the supplied password info using the session key and\n"
			    "the negotiated encryption algorithm in place\n"
			    "i.e. it overwrites the original data"
	},
	{
		.ml_name  = "get_smb_signing",
		.ml_meth  = py_creds_get_smb_signing,
		.ml_flags = METH_NOARGS,
	},
	{
		.ml_name  = "set_smb_signing",
		.ml_meth  = py_creds_set_smb_signing,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "get_smb_ipc_signing",
		.ml_meth  = py_creds_get_smb_ipc_signing,
		.ml_flags = METH_NOARGS,
	},
	{
		.ml_name  = "set_smb_ipc_signing",
		.ml_meth  = py_creds_set_smb_ipc_signing,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "get_smb_encryption",
		.ml_meth  = py_creds_get_smb_encryption,
		.ml_flags = METH_NOARGS,
	},
	{
		.ml_name  = "set_smb_encryption",
		.ml_meth  = py_creds_set_smb_encryption,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name  = "get_krb5_fast_armor_credentials",
		.ml_meth  = py_creds_get_krb5_fast_armor_credentials,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.get_krb5_fast_armor_credentials() -> Credentials\n"
			    "Get the Kerberos FAST credentials set on this credentials object"
	},
	{
		.ml_name  = "set_krb5_fast_armor_credentials",
		.ml_meth  = py_creds_set_krb5_fast_armor_credentials,
		.ml_flags = METH_VARARGS,
		.ml_doc   = "S.set_krb5_fast_armor_credentials(credentials, required) -> None\n"
			    "Set Kerberos FAST credentials for this credentials object, and if FAST armoring must be used."
	},
	{
		.ml_name  = "get_krb5_require_fast_armor",
		.ml_meth  = py_creds_get_krb5_require_fast_armor,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "S.get_krb5_fast_armor() -> bool\n"
			    "Indicate if Kerberos FAST armor is required"
	},
	{ .ml_name = NULL }
};

PyTypeObject PyCredentials = {
	.tp_name = "credentials.Credentials",
	.tp_new = py_creds_new,
	.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_methods = py_creds_methods,
};

static PyObject *py_ccache_name(PyObject *self, PyObject *unused)
{
	struct ccache_container *ccc = NULL;
	char *name = NULL;
	PyObject *py_name = NULL;
	int ret;

	ccc = pytalloc_get_type(self, struct ccache_container);

	ret = krb5_cc_get_full_name(ccc->smb_krb5_context->krb5_context,
				    ccc->ccache, &name);
	if (ret == 0) {
		py_name = PyString_FromStringOrNULL(name);
		krb5_free_string(ccc->smb_krb5_context->krb5_context, name);
	} else {
		PyErr_SetString(PyExc_RuntimeError,
				"Failed to get ccache name");
		return NULL;
	}
	return py_name;
}

static PyMethodDef py_ccache_container_methods[] = {
	{ "get_name", py_ccache_name, METH_NOARGS,
	  "S.get_name() -> name\nObtain KRB5 credentials cache name." },
	{0}
};

PyTypeObject PyCredentialCacheContainer = {
	.tp_name = "credentials.CredentialCacheContainer",
	.tp_flags = Py_TPFLAGS_DEFAULT,
	.tp_methods = py_ccache_container_methods,
};

static PyObject *py_netlogon_creds_kerberos_init(PyObject *module,
						 PyObject *args,
						 PyObject *kwargs)
{
	const char * const kwnames[] = {
		"client_account",
		"client_computer_name",
		"secure_channel_type",
		"client_requested_flags",
		"negotiate_flags",
		NULL,
	};
	const char *client_account = NULL;
	const char *client_computer_name = NULL;
	unsigned short secure_channel_type = 0;
	unsigned int client_requested_flags = 0;
	unsigned int negotiate_flags = 0;
	struct netlogon_creds_CredentialState *ncreds = NULL;
	PyObject *py_ncreds = Py_None;
	bool ok;

	ok = PyArg_ParseTupleAndKeywords(args, kwargs, "ssHII",
					 discard_const_p(char *, kwnames),
					 &client_account,
					 &client_computer_name,
					 &secure_channel_type,
					 &client_requested_flags,
					 &negotiate_flags);
	if (!ok) {
		return NULL;
	}

	ncreds = netlogon_creds_kerberos_init(NULL,
					      client_account,
					      client_computer_name,
					      secure_channel_type,
					      client_requested_flags,
					      NULL, /* client_sid */
					      negotiate_flags);
	if (ncreds == NULL) {
		PyErr_NoMemory();
		return NULL;
	}

	py_ncreds = py_return_ndr_struct("samba.dcerpc.schannel",
					 "netlogon_creds_CredentialState",
					 ncreds,
					 ncreds);
	if (py_ncreds == NULL) {
		TALLOC_FREE(ncreds);
		return NULL;
	}

	return py_ncreds;
}

static PyObject *py_netlogon_creds_random_challenge(PyObject *module,
						    PyObject *unused)
{
	struct netr_Credential *challenge = NULL;
	PyObject *py_challenge = Py_None;

	challenge = talloc(NULL, struct netr_Credential);
	if (challenge == NULL) {
		PyErr_NoMemory();
		return NULL;
	}
	netlogon_creds_random_challenge(challenge);

	py_challenge = py_return_ndr_struct("samba.dcerpc.netlogon",
					    "netr_Credential",
					    challenge,
					    challenge);
	if (py_challenge == NULL) {
		TALLOC_FREE(challenge);
		return NULL;
	}

	return py_challenge;
}

static PyObject *py_netlogon_creds_client_init(PyObject *module,
					       PyObject *args,
					       PyObject *kwargs)
{
	const char * const kwnames[] = {
		"client_account",
		"client_computer_name",
		"secure_channel_type",
		"client_challenge",
		"server_challenge",
		"machine_password",
		"client_requested_flags",
		"negotiate_flags",
		NULL,
	};
	const char *client_account = NULL;
	const char *client_computer_name = NULL;
	unsigned short secure_channel_type = 0;
	unsigned int client_requested_flags = 0;
	unsigned int negotiate_flags = 0;
	PyObject *py_client_challenge = Py_None;
	const struct netr_Credential *client_challenge = NULL;
	PyObject *py_server_challenge = Py_None;
	const struct netr_Credential *server_challenge = NULL;
	PyObject *py_machine_password = Py_None;
	const struct samr_Password *machine_password = NULL;
	struct netlogon_creds_CredentialState *ncreds = NULL;
	PyObject *py_ncreds = Py_None;
	struct netr_Credential *initial_credential = NULL;
	PyObject *py_initial_credential = Py_None;
	PyObject *py_result = Py_None;
	bool ok;

	ok = PyArg_ParseTupleAndKeywords(args, kwargs, "ssHOOOII",
					 discard_const_p(char *, kwnames),
					 &client_account,
					 &client_computer_name,
					 &secure_channel_type,
					 &py_client_challenge,
					 &py_server_challenge,
					 &py_machine_password,
					 &client_requested_flags,
					 &negotiate_flags);
	if (!ok) {
		return NULL;
	}

	ok = py_check_dcerpc_type(py_client_challenge,
				  "samba.dcerpc.netlogon",
				  "netr_Credential");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	client_challenge = pytalloc_get_type(py_client_challenge,
					     struct netr_Credential);
	if (client_challenge == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	ok = py_check_dcerpc_type(py_server_challenge,
				  "samba.dcerpc.netlogon",
				  "netr_Credential");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	/*
	 * we can't use pytalloc_get_type as
	 * NDR_PULL_ALLOC()/talloc_ptrtype() doesn't set the
	 * correct talloc name because of old
	 * compilers.
	 */
	server_challenge = pytalloc_get_ptr(py_server_challenge);
	if (server_challenge == NULL) {
		return NULL;
	}

	ok = py_check_dcerpc_type(py_machine_password,
				  "samba.dcerpc.samr",
				  "Password");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	machine_password = pytalloc_get_type(py_machine_password,
					     struct samr_Password);
	if (machine_password == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	initial_credential = talloc_zero(NULL, struct netr_Credential);
	if (initial_credential == NULL) {
		PyErr_NoMemory();
		return NULL;
	}

	ncreds = netlogon_creds_client_init(NULL,
					    client_account,
					    client_computer_name,
					    secure_channel_type,
					    client_challenge,
					    server_challenge,
					    machine_password,
					    initial_credential,
					    client_requested_flags,
					    negotiate_flags);
	if (ncreds == NULL) {
		TALLOC_FREE(initial_credential);
		PyErr_NoMemory();
		return NULL;
	}

	py_ncreds = py_return_ndr_struct("samba.dcerpc.schannel",
					 "netlogon_creds_CredentialState",
					 ncreds,
					 ncreds);
	if (py_ncreds == NULL) {
		TALLOC_FREE(initial_credential);
		TALLOC_FREE(ncreds);
		return NULL;
	}

	py_initial_credential = py_return_ndr_struct("samba.dcerpc.netlogon",
						     "netr_Credential",
						     initial_credential,
						     initial_credential);
	if (py_ncreds == NULL) {
		Py_DECREF(py_ncreds);
		TALLOC_FREE(initial_credential);
		return NULL;
	}

	py_result = Py_BuildValue("(OO)",
				  py_ncreds,
				  py_initial_credential);
	if (py_result == NULL) {
		Py_DECREF(py_ncreds);
		Py_DECREF(py_initial_credential);
		return NULL;
	}

	return py_result;
}

static PyObject *py_netlogon_creds_client_update(PyObject *module,
						 PyObject *args,
						 PyObject *kwargs)
{
	const char * const kwnames[] = {
		"netlogon_creds",
		"negotiated_flags",
		"client_rid",
		NULL,
	};
	PyObject *py_ncreds = Py_None;
	struct netlogon_creds_CredentialState *ncreds = NULL;
	unsigned int negotiated_flags = 0;
	unsigned int client_rid = 0;
	bool ok;

	ok = PyArg_ParseTupleAndKeywords(args, kwargs, "OII",
					 discard_const_p(char *, kwnames),
					 &py_ncreds,
					 &negotiated_flags,
					 &client_rid);
	if (!ok) {
		return NULL;
	}

	ok = py_check_dcerpc_type(py_ncreds,
				  "samba.dcerpc.schannel",
				  "netlogon_creds_CredentialState");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	ncreds = pytalloc_get_type(py_ncreds,
				   struct netlogon_creds_CredentialState);
	if (ncreds == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	ncreds->negotiate_flags = negotiated_flags;
	ncreds->client_sid.sub_auths[0] = client_rid;

	Py_RETURN_NONE;
}

static PyObject *py_netlogon_creds_client_authenticator(PyObject *module,
							PyObject *args,
							PyObject *kwargs)
{
	const char * const kwnames[] = {
		"netlogon_creds",
		NULL,
	};
	PyObject *py_ncreds = Py_None;
	struct netlogon_creds_CredentialState *ncreds = NULL;
	struct netlogon_creds_CredentialState _ncreds;
	struct netr_Authenticator _auth;
	struct netr_Authenticator *auth = NULL;
	PyObject *py_auth = Py_None;
	NTSTATUS status;
	bool ok;

	ok = PyArg_ParseTupleAndKeywords(args, kwargs, "O",
					 discard_const_p(char *, kwnames),
					 &py_ncreds);
	if (!ok) {
		return NULL;
	}

	ok = py_check_dcerpc_type(py_ncreds,
				  "samba.dcerpc.schannel",
				  "netlogon_creds_CredentialState");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	ncreds = pytalloc_get_type(py_ncreds,
				   struct netlogon_creds_CredentialState);
	if (ncreds == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	_ncreds = *ncreds;
	status = netlogon_creds_client_authenticator(&_ncreds, &_auth);
	PyErr_NTSTATUS_IS_ERR_RAISE(status);

	auth = talloc(NULL, struct netr_Authenticator);
	if (auth == NULL) {
		PyErr_NoMemory();
		return NULL;
	}
	*auth = _auth;

	py_auth = py_return_ndr_struct("samba.dcerpc.netlogon",
				       "netr_Authenticator",
				       auth,
				       auth);
	if (py_auth == NULL) {
		TALLOC_FREE(auth);
		return NULL;
	}

	*ncreds = _ncreds;
	return py_auth;
}

static PyObject *py_netlogon_creds_client_verify(PyObject *module,
						 PyObject *args,
						 PyObject *kwargs)
{
	const char * const kwnames[] = {
		"netlogon_creds",
		"received_credentials",
		"auth_type",
		"auth_level",
		NULL,
	};
	PyObject *py_ncreds = Py_None;
	struct netlogon_creds_CredentialState *ncreds = NULL;
	PyObject *py_rcreds = Py_None;
	const struct netr_Credential *rcreds = NULL;
	uint8_t _auth_type = DCERPC_AUTH_TYPE_NONE;
	enum dcerpc_AuthType auth_type;
	uint8_t _auth_level = DCERPC_AUTH_LEVEL_NONE;
	enum dcerpc_AuthLevel auth_level;
	NTSTATUS status;
	bool ok;

	ok = PyArg_ParseTupleAndKeywords(args, kwargs, "OObb",
					 discard_const_p(char *, kwnames),
					 &py_ncreds,
					 &py_rcreds,
					 &_auth_type,
					 &_auth_level);
	if (!ok) {
		return NULL;
	}
	auth_type = _auth_type;
	auth_level = _auth_level;

	ok = py_check_dcerpc_type(py_ncreds,
				  "samba.dcerpc.schannel",
				  "netlogon_creds_CredentialState");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	ncreds = pytalloc_get_type(py_ncreds,
				   struct netlogon_creds_CredentialState);
	if (ncreds == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	ok = py_check_dcerpc_type(py_rcreds,
				  "samba.dcerpc.netlogon",
				  "netr_Credential");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	/*
	 * we can't use pytalloc_get_type as
	 * NDR_PULL_ALLOC()/talloc_ptrtype() doesn't set the
	 * correct talloc name because of old
	 * compilers.
	 */
	rcreds = pytalloc_get_ptr(py_rcreds);
	if (rcreds == NULL) {
		return NULL;
	}

	status = netlogon_creds_client_verify(ncreds,
					      rcreds,
					      auth_type,
					      auth_level);
	PyErr_NTSTATUS_IS_ERR_RAISE(status);

	Py_RETURN_NONE;
}

static PyObject *py_netlogon_creds_encrypt_netr_LogonLevel(PyObject *module,
							   PyObject *args,
							   PyObject *kwargs)
{
	const char * const kwnames[] = {
		"netlogon_creds",
		"level",
		"info",
		"auth_type",
		"auth_level",
		NULL,
	};
	PyObject *py_ncreds = Py_None;
	struct netlogon_creds_CredentialState *ncreds = NULL;
	uint8_t _level = 0;
	enum netr_LogonInfoClass level = NetlogonInteractiveInformation;
	PyObject *py_info = Py_None;
	union netr_LogonLevel logon = { .password = NULL, };
	const void *info_ptr = NULL;
	uint8_t _auth_type = DCERPC_AUTH_TYPE_NONE;
	enum dcerpc_AuthType auth_type;
	uint8_t _auth_level = DCERPC_AUTH_LEVEL_NONE;
	enum dcerpc_AuthLevel auth_level;
	NTSTATUS status;
	bool ok;

	ok = PyArg_ParseTupleAndKeywords(args, kwargs, "ObObb",
					 discard_const_p(char *, kwnames),
					 &py_ncreds,
					 &_level,
					 &py_info,
					 &_auth_type,
					 &_auth_level);
	if (!ok) {
		return NULL;
	}
	level = _level;
	auth_type = _auth_type;
	auth_level = _auth_level;

	ok = py_check_dcerpc_type(py_ncreds,
				  "samba.dcerpc.schannel",
				  "netlogon_creds_CredentialState");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	ncreds = pytalloc_get_type(py_ncreds,
				   struct netlogon_creds_CredentialState);
	if (ncreds == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	switch (level) {
	case NetlogonInteractiveInformation:
	case NetlogonInteractiveTransitiveInformation:
	case NetlogonServiceInformation:
	case NetlogonServiceTransitiveInformation:
		ok = py_check_dcerpc_type(py_info,
					  "samba.dcerpc.netlogon",
					  "netr_PasswordInfo");
		if (!ok) {
			/* py_check_dcerpc_type sets TypeError */
			return NULL;
		}

		logon.password = pytalloc_get_type(py_info,
						   struct netr_PasswordInfo);
		if (logon.password == NULL) {
			/* pytalloc_get_type sets TypeError */
			return NULL;
		}
		info_ptr = logon.password;
		break;

	case NetlogonNetworkInformation:
	case NetlogonNetworkTransitiveInformation:
		ok = py_check_dcerpc_type(py_info,
					  "samba.dcerpc.netlogon",
					  "netr_NetworkInfo");
		if (!ok) {
			/* py_check_dcerpc_type sets TypeError */
			return NULL;
		}

		logon.network = pytalloc_get_type(py_info,
						   struct netr_NetworkInfo);
		if (logon.network == NULL) {
			/* pytalloc_get_type sets TypeError */
			return NULL;
		}
		info_ptr = logon.network;
		break;

	case NetlogonGenericInformation:
		ok = py_check_dcerpc_type(py_info,
					  "samba.dcerpc.netlogon",
					  "netr_GenericInfo");
		if (!ok) {
			/* py_check_dcerpc_type sets TypeError */
			return NULL;
		}

		logon.generic = pytalloc_get_type(py_info,
						   struct netr_GenericInfo);
		if (logon.generic == NULL) {
			/* pytalloc_get_type sets TypeError */
			return NULL;
		}
		info_ptr = logon.generic;
		break;

	case NetlogonTicketLogonInformation:
		ok = py_check_dcerpc_type(py_info,
					  "samba.dcerpc.netlogon",
					  "netr_TicketLogonInfo");
		if (!ok) {
			/* py_check_dcerpc_type sets TypeError */
			return NULL;
		}

		logon.ticket = pytalloc_get_type(py_info,
						   struct netr_TicketLogonInfo);
		if (logon.ticket == NULL) {
			/* pytalloc_get_type sets TypeError */
			return NULL;
		}
		info_ptr = logon.ticket;
		break;
	}

	if (info_ptr == NULL) {
		PyErr_SetString(PyExc_ValueError,
				"Invalid netr_LogonInfoClass value");
		return NULL;
	}

	status = netlogon_creds_encrypt_samlogon_logon(ncreds,
						       level,
						       &logon,
						       auth_type,
						       auth_level);
	PyErr_NTSTATUS_IS_ERR_RAISE(status);

	Py_RETURN_NONE;
}

static PyObject *py_netlogon_creds_decrypt_netr_Validation(PyObject *module,
							   PyObject *args,
							   PyObject *kwargs)
{
	const char * const kwnames[] = {
		"netlogon_creds",
		"level",
		"validation",
		"auth_type",
		"auth_level",
		NULL,
	};
	PyObject *py_ncreds = Py_None;
	struct netlogon_creds_CredentialState *ncreds = NULL;
	uint8_t _level = 0;
	enum netr_ValidationInfoClass level = NetlogonValidationUasInfo;
	PyObject *py_validation = Py_None;
	union netr_Validation validation = { .generic = NULL, };
	const void *validation_ptr = NULL;
	uint8_t _auth_type = DCERPC_AUTH_TYPE_NONE;
	enum dcerpc_AuthType auth_type;
	uint8_t _auth_level = DCERPC_AUTH_LEVEL_NONE;
	enum dcerpc_AuthLevel auth_level;
	NTSTATUS status;
	bool ok;

	ok = PyArg_ParseTupleAndKeywords(args, kwargs, "ObObb",
					 discard_const_p(char *, kwnames),
					 &py_ncreds,
					 &_level,
					 &py_validation,
					 &_auth_type,
					 &_auth_level);
	if (!ok) {
		return NULL;
	}
	level = _level;
	auth_type = _auth_type;
	auth_level = _auth_level;

	ok = py_check_dcerpc_type(py_ncreds,
				  "samba.dcerpc.schannel",
				  "netlogon_creds_CredentialState");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	ncreds = pytalloc_get_type(py_ncreds,
				   struct netlogon_creds_CredentialState);
	if (ncreds == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	switch (level) {
	case NetlogonValidationUasInfo:
		break;

	case NetlogonValidationSamInfo:
		ok = py_check_dcerpc_type(py_validation,
					  "samba.dcerpc.netlogon",
					  "netr_SamInfo2");
		if (!ok) {
			/* py_check_dcerpc_type sets TypeError */
			return NULL;
		}

		/*
		 * we can't use pytalloc_get_type as
		 * NDR_PULL_ALLOC()/talloc_ptrtype() doesn't set the
		 * correct talloc name because of old
		 * compilers.
		 */
		validation.sam2 = pytalloc_get_ptr(py_validation);
		if (validation.sam2 == NULL) {
			return NULL;
		}
		validation_ptr = validation.sam2;
		break;

	case NetlogonValidationSamInfo2:
		ok = py_check_dcerpc_type(py_validation,
					  "samba.dcerpc.netlogon",
					  "netr_SamInfo3");
		if (!ok) {
			/* py_check_dcerpc_type sets TypeError */
			return NULL;
		}

		/*
		 * we can't use pytalloc_get_type as
		 * NDR_PULL_ALLOC()/talloc_ptrtype() doesn't set the
		 * correct talloc name because of old
		 * compilers.
		 */
		validation.sam3 = pytalloc_get_ptr(py_validation);
		if (validation.sam3 == NULL) {
			return NULL;
		}
		validation_ptr = validation.sam3;
		break;

	case NetlogonValidationGenericInfo2:
		ok = py_check_dcerpc_type(py_validation,
					  "samba.dcerpc.netlogon",
					  "netr_GenericInfo2");
		if (!ok) {
			/* py_check_dcerpc_type sets TypeError */
			return NULL;
		}

		/*
		 * we can't use pytalloc_get_type as
		 * NDR_PULL_ALLOC()/talloc_ptrtype() doesn't set the
		 * correct talloc name because of old
		 * compilers.
		 */
		validation.generic = pytalloc_get_ptr(py_validation);
		if (validation.generic == NULL) {
			return NULL;
		}
		validation_ptr = validation.generic;
		break;

	case NetlogonValidationSamInfo4:
		ok = py_check_dcerpc_type(py_validation,
					  "samba.dcerpc.netlogon",
					  "netr_SamInfo6");
		if (!ok) {
			/* py_check_dcerpc_type sets TypeError */
			return NULL;
		}

		/*
		 * we can't use pytalloc_get_type as
		 * NDR_PULL_ALLOC()/talloc_ptrtype() doesn't set the
		 * correct talloc name because of old
		 * compilers.
		 */
		validation.sam6 = pytalloc_get_ptr(py_validation);
		if (validation.sam6 == NULL) {
			return NULL;
		}
		validation_ptr = validation.sam6;
		break;

	case NetlogonValidationTicketLogon:
		ok = py_check_dcerpc_type(py_validation,
					  "samba.dcerpc.netlogon",
					  "netr_ValidationTicketLogon");
		if (!ok) {
			/* py_check_dcerpc_type sets TypeError */
			return NULL;
		}

		/*
		 * we can't use pytalloc_get_type as
		 * NDR_PULL_ALLOC()/talloc_ptrtype() doesn't set the
		 * correct talloc name because of old
		 * compilers.
		 */
		validation.ticket = pytalloc_get_ptr(py_validation);
		if (validation.ticket == NULL) {
			return NULL;
		}
		validation_ptr = validation.ticket;
		break;

	}

	if (validation_ptr == NULL) {
		PyErr_SetString(PyExc_RuntimeError,
				"Unexpected netr_Validation value");
		return NULL;
	}

	status = netlogon_creds_decrypt_samlogon_validation(ncreds,
							    level,
							    &validation,
							    auth_type,
							    auth_level);
	PyErr_NTSTATUS_IS_ERR_RAISE(status);

	Py_RETURN_NONE;
}

static PyObject *py_netlogon_creds_decrypt_samr_Password(PyObject *module,
							 PyObject *args,
							 PyObject *kwargs)
{
	const char * const kwnames[] = {
		"netlogon_creds",
		"pwd",
		"auth_type",
		"auth_level",
		NULL,
	};
	PyObject *py_ncreds = Py_None;
	struct netlogon_creds_CredentialState *ncreds = NULL;
	PyObject *py_pwd = Py_None;
	struct samr_Password *pwd = NULL;
	uint8_t _auth_type = DCERPC_AUTH_TYPE_NONE;
	enum dcerpc_AuthType auth_type;
	uint8_t _auth_level = DCERPC_AUTH_LEVEL_NONE;
	enum dcerpc_AuthLevel auth_level;
	NTSTATUS status;
	bool ok;

	ok = PyArg_ParseTupleAndKeywords(args, kwargs, "OObb",
					 discard_const_p(char *, kwnames),
					 &py_ncreds,
					 &py_pwd,
					 &_auth_type,
					 &_auth_level);
	if (!ok) {
		return NULL;
	}
	auth_type = _auth_type;
	auth_level = _auth_level;

	ok = py_check_dcerpc_type(py_ncreds,
				  "samba.dcerpc.schannel",
				  "netlogon_creds_CredentialState");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	ncreds = pytalloc_get_type(py_ncreds,
				   struct netlogon_creds_CredentialState);
	if (ncreds == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	ok = py_check_dcerpc_type(py_pwd,
				  "samba.dcerpc.samr",
				  "Password");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	/*
	 * we can't use pytalloc_get_type as
	 * NDR_PULL_ALLOC()/talloc_ptrtype() doesn't set the
	 * correct talloc name because of old
	 * compilers.
	 */
	pwd = pytalloc_get_ptr(py_pwd);
	if (pwd == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	status = netlogon_creds_decrypt_samr_Password(ncreds,
						      pwd,
						      auth_type,
						      auth_level);
	PyErr_NTSTATUS_IS_ERR_RAISE(status);

	Py_RETURN_NONE;
}

static PyObject *py_netlogon_creds_encrypt_samr_Password(PyObject *module,
							 PyObject *args,
							 PyObject *kwargs)
{
	const char * const kwnames[] = {
		"netlogon_creds",
		"pwd",
		"auth_type",
		"auth_level",
		NULL,
	};
	PyObject *py_ncreds = Py_None;
	struct netlogon_creds_CredentialState *ncreds = NULL;
	PyObject *py_pwd = Py_None;
	struct samr_Password *pwd = NULL;
	uint8_t _auth_type = DCERPC_AUTH_TYPE_NONE;
	enum dcerpc_AuthType auth_type;
	uint8_t _auth_level = DCERPC_AUTH_LEVEL_NONE;
	enum dcerpc_AuthLevel auth_level;
	NTSTATUS status;
	bool ok;

	ok = PyArg_ParseTupleAndKeywords(args, kwargs, "OObb",
					 discard_const_p(char *, kwnames),
					 &py_ncreds,
					 &py_pwd,
					 &_auth_type,
					 &_auth_level);
	if (!ok) {
		return NULL;
	}
	auth_type = _auth_type;
	auth_level = _auth_level;

	ok = py_check_dcerpc_type(py_ncreds,
				  "samba.dcerpc.schannel",
				  "netlogon_creds_CredentialState");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	ncreds = pytalloc_get_type(py_ncreds,
				   struct netlogon_creds_CredentialState);
	if (ncreds == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	ok = py_check_dcerpc_type(py_pwd,
				  "samba.dcerpc.samr",
				  "Password");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	pwd = pytalloc_get_type(py_pwd,
				struct samr_Password);
	if (pwd == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	status = netlogon_creds_encrypt_samr_Password(ncreds,
						      pwd,
						      auth_type,
						      auth_level);
	PyErr_NTSTATUS_IS_ERR_RAISE(status);

	Py_RETURN_NONE;
}

static PyObject *py_netlogon_creds_encrypt_netr_CryptPassword(PyObject *module,
							      PyObject *args,
							      PyObject *kwargs)
{
	const char * const kwnames[] = {
		"netlogon_creds",
		"pwd",
		"auth_type",
		"auth_level",
		NULL,
	};
	PyObject *py_ncreds = Py_None;
	struct netlogon_creds_CredentialState *ncreds = NULL;
	PyObject *py_pwd = Py_None;
	struct netr_CryptPassword *pwd = NULL;
	struct samr_CryptPassword spwd;
	uint8_t _auth_type = DCERPC_AUTH_TYPE_NONE;
	enum dcerpc_AuthType auth_type;
	uint8_t _auth_level = DCERPC_AUTH_LEVEL_NONE;
	enum dcerpc_AuthLevel auth_level;
	NTSTATUS status;
	bool ok;

	ok = PyArg_ParseTupleAndKeywords(args, kwargs, "OObb",
					 discard_const_p(char *, kwnames),
					 &py_ncreds,
					 &py_pwd,
					 &_auth_type,
					 &_auth_level);
	if (!ok) {
		return NULL;
	}
	auth_type = _auth_type;
	auth_level = _auth_level;

	ok = py_check_dcerpc_type(py_ncreds,
				  "samba.dcerpc.schannel",
				  "netlogon_creds_CredentialState");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	ncreds = pytalloc_get_type(py_ncreds,
				   struct netlogon_creds_CredentialState);
	if (ncreds == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	ok = py_check_dcerpc_type(py_pwd,
				  "samba.dcerpc.netlogon",
				  "netr_CryptPassword");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	pwd = pytalloc_get_type(py_pwd,
				struct netr_CryptPassword);
	if (pwd == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	memcpy(spwd.data, pwd->data, 512);
	PUSH_LE_U32(spwd.data, 512, pwd->length);

	status = netlogon_creds_encrypt_samr_CryptPassword(ncreds,
							   &spwd,
							   auth_type,
							   auth_level);

	memcpy(pwd->data, spwd.data, 512);
	pwd->length = PULL_LE_U32(spwd.data, 512);
	ZERO_STRUCT(spwd);

	PyErr_NTSTATUS_IS_ERR_RAISE(status);

	Py_RETURN_NONE;
}

static PyObject *py_netlogon_creds_encrypt_SendToSam(PyObject *module,
						     PyObject *args,
						     PyObject *kwargs)
{
	const char * const kwnames[] = {
		"netlogon_creds",
		"opaque_buffer",
		"auth_type",
		"auth_level",
		NULL,
	};
	PyObject *py_ncreds = Py_None;
	struct netlogon_creds_CredentialState *ncreds = NULL;
	PyObject *py_opaque = Py_None;
	uint8_t *opaque_data = NULL;
	size_t opaque_length = 0;
	uint8_t _auth_type = DCERPC_AUTH_TYPE_NONE;
	enum dcerpc_AuthType auth_type;
	uint8_t _auth_level = DCERPC_AUTH_LEVEL_NONE;
	enum dcerpc_AuthLevel auth_level;
	NTSTATUS status;
	bool ok;

	ok = PyArg_ParseTupleAndKeywords(args, kwargs, "OSbb",
					 discard_const_p(char *, kwnames),
					 &py_ncreds,
					 &py_opaque,
					 &_auth_type,
					 &_auth_level);
	if (!ok) {
		return NULL;
	}
	auth_type = _auth_type;
	auth_level = _auth_level;

	ok = py_check_dcerpc_type(py_ncreds,
				  "samba.dcerpc.schannel",
				  "netlogon_creds_CredentialState");
	if (!ok) {
		/* py_check_dcerpc_type sets TypeError */
		return NULL;
	}

	ncreds = pytalloc_get_type(py_ncreds,
				   struct netlogon_creds_CredentialState);
	if (ncreds == NULL) {
		/* pytalloc_get_type sets TypeError */
		return NULL;
	}

	opaque_data = (uint8_t *)PyBytes_AsString(py_opaque);
	opaque_length = PyBytes_Size(py_opaque);

	status = netlogon_creds_encrypt_SendToSam(ncreds,
						  opaque_data,
						  opaque_length,
						  auth_type,
						  auth_level);
	PyErr_NTSTATUS_IS_ERR_RAISE(status);

	Py_RETURN_NONE;
}

static PyMethodDef py_module_methods[] = {
	{
		.ml_name  = "netlogon_creds_kerberos_init",
		.ml_meth  = PY_DISCARD_FUNC_SIG(PyCFunction,
					py_netlogon_creds_kerberos_init),
		.ml_flags = METH_VARARGS | METH_KEYWORDS,
		.ml_doc   = "credentials.netlogon_creds_kerberos_init("
			    "client_account, client_computer_name,"
			    "secure_channel_type, "
			    "client_requested_flags, negotiate_flags)"
			    "-> netlogon_creds_CredentialState\n"
			    "Create a new state for netr_ServerAuthenticateKerberos()",
	},
	{
		.ml_name  = "netlogon_creds_random_challenge",
		.ml_meth  = py_netlogon_creds_random_challenge,
		.ml_flags = METH_NOARGS,
		.ml_doc   = "credentials.netlogon_creds_random_challenge()"
			    "-> netr_Credential\n"
			    "Create a new random netr_Credential",
	},
	{
		.ml_name  = "netlogon_creds_client_init",
		.ml_meth  = PY_DISCARD_FUNC_SIG(PyCFunction,
					py_netlogon_creds_client_init),
		.ml_flags = METH_VARARGS | METH_KEYWORDS,
		.ml_doc   = "credentials.netlogon_creds_client_init("
			    "client_account, client_computer_name,"
			    "secure_channel_type, "
			    "client_challenge, server_challenge, "
			    "machine_password, "
			    "client_requested_flags, negotiate_flags)"
			    "-> (netlogon_creds_CredentialState, initial_credential)\n"
			    "Create a new state for netr_ServerAuthenticate3()",
	},
	{
		.ml_name  = "netlogon_creds_client_update",
		.ml_meth  = PY_DISCARD_FUNC_SIG(PyCFunction,
					py_netlogon_creds_client_update),
		.ml_flags = METH_VARARGS | METH_KEYWORDS,
		.ml_doc   = "credentials.netlogon_creds_client_update("
			    "netlogon_creds, negotiated_flags, client_rid)"
			    "-> None\n"
			    "Update the negotiated flags and client rid",
	},
	{
		.ml_name  = "netlogon_creds_client_authenticator",
		.ml_meth  = PY_DISCARD_FUNC_SIG(PyCFunction,
					py_netlogon_creds_client_authenticator),
		.ml_flags = METH_VARARGS | METH_KEYWORDS,
		.ml_doc   = "credentials.netlogon_creds_client_authenticator(netlogon_creds) -> Authenticator\n"
			    "Get a new client NETLOGON_AUTHENTICATOR"
	},
	{
		.ml_name  = "netlogon_creds_client_verify",
		.ml_meth  = PY_DISCARD_FUNC_SIG(PyCFunction,
					py_netlogon_creds_client_verify),
		.ml_flags = METH_VARARGS | METH_KEYWORDS,
		.ml_doc   = "credentials.py_netlogon_creds_client_verify(netlogon_creds, "
			    "received_credentials, auth_type, auth_level) -> None\n"
			    "Verify the NETLOGON_AUTHENTICATOR.credentials from a server"
	},
	{
		.ml_name  = "netlogon_creds_encrypt_netr_LogonLevel",
		.ml_meth  = PY_DISCARD_FUNC_SIG(PyCFunction,
					py_netlogon_creds_encrypt_netr_LogonLevel),
		.ml_flags = METH_VARARGS | METH_KEYWORDS,
		.ml_doc   = "credentials.netlogon_creds_encrypt_netr_LogonLevel(netlogon_creds, "
			    "level, info, auth_type, auth_level) -> None\n"
			    "Encrypt the supplied logon info using the session key and\n"
			    "the negotiated encryption algorithm in place\n"
			    "i.e. it overwrites the original data",
	},
	{
		.ml_name  = "netlogon_creds_decrypt_netr_Validation",
		.ml_meth  = PY_DISCARD_FUNC_SIG(PyCFunction,
					py_netlogon_creds_decrypt_netr_Validation),
		.ml_flags = METH_VARARGS | METH_KEYWORDS,
		.ml_doc   = "credentials.netlogon_creds_decrypt_netr_Validation(netlogon_creds, "
			    "level, validation, auth_type, auth_level) -> None\n"
			    "Encrypt the supplied validation info using the session key and\n"
			    "the negotiated encryption algorithm in place\n"
			    "i.e. it overwrites the original data",
	},
	{
		.ml_name  = "netlogon_creds_decrypt_samr_Password",
		.ml_meth  = PY_DISCARD_FUNC_SIG(PyCFunction,
					py_netlogon_creds_decrypt_samr_Password),
		.ml_flags = METH_VARARGS | METH_KEYWORDS,
		.ml_doc   = "credentials.netlogon_creds_decrypt_samr_Password(netlogon_creds, "
			    "pwd, auth_type, auth_level) -> None\n"
			    "Encrypt the supplied samr_Password using the session key and\n"
			    "the negotiated encryption algorithm in place\n"
			    "i.e. it overwrites the original data",
	},
	{
		.ml_name  = "netlogon_creds_encrypt_samr_Password",
		.ml_meth  = PY_DISCARD_FUNC_SIG(PyCFunction,
					py_netlogon_creds_encrypt_samr_Password),
		.ml_flags = METH_VARARGS | METH_KEYWORDS,
		.ml_doc   = "credentials.netlogon_creds_encrypt_samr_Password(netlogon_creds, "
			    "pwd, auth_type, auth_level) -> None\n"
			    "Encrypt the supplied samr_Password using the session key and\n"
			    "the negotiated encryption algorithm in place\n"
			    "i.e. it overwrites the original data",
	},
	{
		.ml_name  = "netlogon_creds_encrypt_netr_CryptPassword",
		.ml_meth  = PY_DISCARD_FUNC_SIG(PyCFunction,
					py_netlogon_creds_encrypt_netr_CryptPassword),
		.ml_flags = METH_VARARGS | METH_KEYWORDS,
		.ml_doc   = "credentials.netlogon_creds_encrypt_netr_CryptPassword(netlogon_creds, "
			    "pwd, auth_type, auth_level) -> None\n"
			    "Encrypt the supplied netr_CryptPassword using the session key and\n"
			    "the negotiated encryption algorithm in place\n"
			    "i.e. it overwrites the original data",
	},
	{
		.ml_name  = "netlogon_creds_encrypt_SendToSam",
		.ml_meth  = PY_DISCARD_FUNC_SIG(PyCFunction,
					py_netlogon_creds_encrypt_SendToSam),
		.ml_flags = METH_VARARGS | METH_KEYWORDS,
		.ml_doc   = "credentials.netlogon_creds_encrypt_SendToSam(netlogon_creds, "
			    "opaque_buffer, auth_type, auth_level) -> None\n"
			    "Encrypt the supplied opaque_buffer using the session key and\n"
			    "the negotiated encryption algorithm in place\n"
			    "i.e. it overwrites the original data",
	},
	{ .ml_name = NULL }
};

static struct PyModuleDef moduledef = {
	PyModuleDef_HEAD_INIT,
	.m_name = "credentials",
	.m_doc = "Credentials management.",
	.m_size = -1,
	.m_methods = py_module_methods,
};

MODULE_INIT_FUNC(credentials)
{
	PyObject *m;
	if (pytalloc_BaseObject_PyType_Ready(&PyCredentials) < 0)
		return NULL;

	if (pytalloc_BaseObject_PyType_Ready(&PyCredentialCacheContainer) < 0)
		return NULL;

	m = PyModule_Create(&moduledef);
	if (m == NULL)
		return NULL;

	PyModule_AddObject(m, "UNINITIALISED", PyLong_FromLong(CRED_UNINITIALISED));
	PyModule_AddObject(m, "SMB_CONF", PyLong_FromLong(CRED_SMB_CONF));
	PyModule_AddObject(m, "CALLBACK", PyLong_FromLong(CRED_CALLBACK));
	PyModule_AddObject(m, "GUESS_ENV", PyLong_FromLong(CRED_GUESS_ENV));
	PyModule_AddObject(m, "GUESS_FILE", PyLong_FromLong(CRED_GUESS_FILE));
	PyModule_AddObject(m, "CALLBACK_RESULT", PyLong_FromLong(CRED_CALLBACK_RESULT));
	PyModule_AddObject(m, "SPECIFIED", PyLong_FromLong(CRED_SPECIFIED));

	PyModule_AddObject(m, "AUTO_USE_KERBEROS", PyLong_FromLong(CRED_USE_KERBEROS_DESIRED));
	PyModule_AddObject(m, "DONT_USE_KERBEROS", PyLong_FromLong(CRED_USE_KERBEROS_DISABLED));
	PyModule_AddObject(m, "MUST_USE_KERBEROS", PyLong_FromLong(CRED_USE_KERBEROS_REQUIRED));

	PyModule_AddObject(m, "AUTO_KRB_FORWARDABLE",  PyLong_FromLong(CRED_AUTO_KRB_FORWARDABLE));
	PyModule_AddObject(m, "NO_KRB_FORWARDABLE",    PyLong_FromLong(CRED_NO_KRB_FORWARDABLE));
	PyModule_AddObject(m, "FORCE_KRB_FORWARDABLE", PyLong_FromLong(CRED_FORCE_KRB_FORWARDABLE));
	PyModule_AddObject(m, "CLI_CRED_NTLM2", PyLong_FromLong(CLI_CRED_NTLM2));
	PyModule_AddObject(m, "CLI_CRED_NTLMv2_AUTH", PyLong_FromLong(CLI_CRED_NTLMv2_AUTH));
	PyModule_AddObject(m, "CLI_CRED_LANMAN_AUTH", PyLong_FromLong(CLI_CRED_LANMAN_AUTH));
	PyModule_AddObject(m, "CLI_CRED_NTLM_AUTH", PyLong_FromLong(CLI_CRED_NTLM_AUTH));
	PyModule_AddObject(m, "CLI_CRED_CLEAR_AUTH", PyLong_FromLong(CLI_CRED_CLEAR_AUTH));

	PyModule_AddObject(m, "SMB_SIGNING_DEFAULT", PyLong_FromLong(SMB_SIGNING_DEFAULT));
	PyModule_AddObject(m, "SMB_SIGNING_OFF", PyLong_FromLong(SMB_SIGNING_OFF));
	PyModule_AddObject(m, "SMB_SIGNING_IF_REQUIRED", PyLong_FromLong(SMB_SIGNING_IF_REQUIRED));
	PyModule_AddObject(m, "SMB_SIGNING_DESIRED", PyLong_FromLong(SMB_SIGNING_DESIRED));
	PyModule_AddObject(m, "SMB_SIGNING_REQUIRED", PyLong_FromLong(SMB_SIGNING_REQUIRED));

	PyModule_AddObject(m, "SMB_ENCRYPTION_DEFAULT", PyLong_FromLong(SMB_ENCRYPTION_DEFAULT));
	PyModule_AddObject(m, "SMB_ENCRYPTION_OFF", PyLong_FromLong(SMB_ENCRYPTION_OFF));
	PyModule_AddObject(m, "SMB_ENCRYPTION_IF_REQUIRED", PyLong_FromLong(SMB_ENCRYPTION_IF_REQUIRED));
	PyModule_AddObject(m, "SMB_ENCRYPTION_DESIRED", PyLong_FromLong(SMB_ENCRYPTION_DESIRED));
	PyModule_AddObject(m, "SMB_ENCRYPTION_REQUIRED", PyLong_FromLong(SMB_ENCRYPTION_REQUIRED));

	PyModule_AddObject(m, "ENCTYPE_ARCFOUR_HMAC", PyLong_FromLong(ENCTYPE_ARCFOUR_HMAC));
	PyModule_AddObject(m, "ENCTYPE_AES128_CTS_HMAC_SHA1_96", PyLong_FromLong(ENCTYPE_AES128_CTS_HMAC_SHA1_96));
	PyModule_AddObject(m, "ENCTYPE_AES256_CTS_HMAC_SHA1_96", PyLong_FromLong(ENCTYPE_AES256_CTS_HMAC_SHA1_96));

	Py_INCREF(&PyCredentials);
	PyModule_AddObject(m, "Credentials", (PyObject *)&PyCredentials);
	Py_INCREF(&PyCredentialCacheContainer);
	PyModule_AddObject(m, "CredentialCacheContainer", (PyObject *)&PyCredentialCacheContainer);
	return m;
}
