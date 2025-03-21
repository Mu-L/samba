/*
   Unix SMB/CIFS implementation.
   Core SMB2 server

   Copyright (C) Stefan Metzmacher 2009

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
#include "smbd/smbd.h"
#include "smbd/globals.h"
#include "../libcli/smb/smb_common.h"
#include "../lib/util/tevent_ntstatus.h"
#include "rpc_server/srv_pipe_hnd.h"
#include "libcli/security/security.h"

#undef DBGC_CLASS
#define DBGC_CLASS DBGC_SMB2

static struct tevent_req *smbd_smb2_write_send(TALLOC_CTX *mem_ctx,
					       struct tevent_context *ev,
					       struct smbd_smb2_request *smb2req,
					       struct files_struct *in_fsp,
					       DATA_BLOB in_data,
					       off_t in_offset,
					       uint32_t in_flags);
static NTSTATUS smbd_smb2_write_recv(struct tevent_req *req,
				     uint32_t *out_count);

static void smbd_smb2_request_write_done(struct tevent_req *subreq);
NTSTATUS smbd_smb2_request_process_write(struct smbd_smb2_request *req)
{
	struct smbXsrv_connection *xconn = req->xconn;
	NTSTATUS status;
	const uint8_t *inbody;
	uint16_t in_data_offset;
	uint32_t in_data_length;
	DATA_BLOB in_data_buffer;
	off_t in_offset;
	uint64_t in_file_id_persistent;
	uint64_t in_file_id_volatile;
	struct files_struct *in_fsp;
	uint32_t in_flags;
	size_t in_dyn_len = 0;
	uint8_t *in_dyn_ptr = NULL;
	struct tevent_req *subreq;

	status = smbd_smb2_request_verify_sizes(req, 0x31);
	if (!NT_STATUS_IS_OK(status)) {
		return smbd_smb2_request_error(req, status);
	}
	inbody = SMBD_SMB2_IN_BODY_PTR(req);

	in_data_offset		= SVAL(inbody, 0x02);
	in_data_length		= IVAL(inbody, 0x04);
	in_offset		= PULL_LE_I64(inbody, 0x08);
	in_file_id_persistent	= BVAL(inbody, 0x10);
	in_file_id_volatile	= BVAL(inbody, 0x18);
	in_flags		= IVAL(inbody, 0x2C);

	if (in_data_offset != (SMB2_HDR_BODY + SMBD_SMB2_IN_BODY_LEN(req))) {
		return smbd_smb2_request_error(req, NT_STATUS_INVALID_PARAMETER);
	}

	if (req->smb1req != NULL && req->smb1req->unread_bytes > 0) {
		in_dyn_ptr = NULL;
		in_dyn_len = req->smb1req->unread_bytes;
	} else {
		in_dyn_ptr = SMBD_SMB2_IN_DYN_PTR(req);
		in_dyn_len = SMBD_SMB2_IN_DYN_LEN(req);
	}

	if (in_data_length > in_dyn_len) {
		return smbd_smb2_request_error(req, NT_STATUS_INVALID_PARAMETER);
	}

	/* check the max write size */
	if (in_data_length > xconn->smb2.server.max_write) {
		DEBUG(2,("smbd_smb2_request_process_write : "
			"client ignored max write :%s: 0x%08X: 0x%08X\n",
			__location__, in_data_length, xconn->smb2.server.max_write));
		return smbd_smb2_request_error(req, NT_STATUS_INVALID_PARAMETER);
	}

	/*
	 * Note: that in_dyn_ptr is NULL for the recvfile case.
	 */
	in_data_buffer.data = in_dyn_ptr;
	in_data_buffer.length = in_data_length;

	status = smbd_smb2_request_verify_creditcharge(req, in_data_length);
	if (!NT_STATUS_IS_OK(status)) {
		return smbd_smb2_request_error(req, status);
	}

	in_fsp = file_fsp_smb2(req, in_file_id_persistent, in_file_id_volatile);
	if (in_fsp == NULL) {
		return smbd_smb2_request_error(req, NT_STATUS_FILE_CLOSED);
	}

	subreq = smbd_smb2_write_send(req, req->sconn->ev_ctx,
				      req, in_fsp,
				      in_data_buffer,
				      in_offset,
				      in_flags);
	if (subreq == NULL) {
		return smbd_smb2_request_error(req, NT_STATUS_NO_MEMORY);
	}
	tevent_req_set_callback(subreq, smbd_smb2_request_write_done, req);

	return smbd_smb2_request_pending_queue(req, subreq, 500);
}

static void smbd_smb2_request_write_done(struct tevent_req *subreq)
{
	struct smbd_smb2_request *req = tevent_req_callback_data(subreq,
					struct smbd_smb2_request);
	DATA_BLOB outbody;
	DATA_BLOB outdyn;
	uint32_t out_count = 0;
	NTSTATUS status;
	NTSTATUS error; /* transport error */

	status = smbd_smb2_write_recv(subreq, &out_count);
	TALLOC_FREE(subreq);
	if (!NT_STATUS_IS_OK(status)) {
		error = smbd_smb2_request_error(req, status);
		if (!NT_STATUS_IS_OK(error)) {
			smbd_server_connection_terminate(req->xconn,
							 nt_errstr(error));
			return;
		}
		return;
	}

	outbody = smbd_smb2_generate_outbody(req, 0x10);
	if (outbody.data == NULL) {
		error = smbd_smb2_request_error(req, NT_STATUS_NO_MEMORY);
		if (!NT_STATUS_IS_OK(error)) {
			smbd_server_connection_terminate(req->xconn,
							 nt_errstr(error));
			return;
		}
		return;
	}

	SSVAL(outbody.data, 0x00, 0x10 + 1);	/* struct size */
	SSVAL(outbody.data, 0x02, 0);		/* reserved */
	SIVAL(outbody.data, 0x04, out_count);	/* count */
	SIVAL(outbody.data, 0x08, 0);		/* remaining */
	SSVAL(outbody.data, 0x0C, 0);		/* write channel info offset */
	SSVAL(outbody.data, 0x0E, 0);		/* write channel info length */

	outdyn = data_blob_const(NULL, 0);

	error = smbd_smb2_request_done(req, outbody, &outdyn);
	if (!NT_STATUS_IS_OK(error)) {
		smbd_server_connection_terminate(req->xconn, nt_errstr(error));
		return;
	}
}

struct smbd_smb2_write_state {
	struct smbd_smb2_request *smb2req;
	struct smb_request *smbreq;
	files_struct *fsp;
	bool write_through;
	uint32_t in_length;
	off_t in_offset;
	uint32_t out_count;
};

static void smbd_smb2_write_pipe_done(struct tevent_req *subreq);

static NTSTATUS smb2_write_complete_internal(struct tevent_req *req,
					     ssize_t nwritten, int err,
					     bool do_sync)
{
	NTSTATUS status;
	struct smbd_smb2_write_state *state = tevent_req_data(req,
					struct smbd_smb2_write_state);
	files_struct *fsp = state->fsp;

	if (nwritten == -1) {
		if (err == EOVERFLOW && fsp_is_alternate_stream(fsp)) {
			status = NT_STATUS_FILE_SYSTEM_LIMITATION;
		} else {
			status = map_nt_error_from_unix(err);
		}

		DEBUG(2, ("smb2_write failed: %s, file %s, "
			  "length=%lu offset=%lu nwritten=-1: %s\n",
			  fsp_fnum_dbg(fsp),
			  fsp_str_dbg(fsp),
			  (unsigned long)state->in_length,
			  (unsigned long)state->in_offset,
			  nt_errstr(status)));

		return status;
	}

	DEBUG(3,("smb2: %s, file %s, "
		"length=%lu offset=%lu wrote=%lu\n",
		fsp_fnum_dbg(fsp),
		fsp_str_dbg(fsp),
		(unsigned long)state->in_length,
		(unsigned long)state->in_offset,
		(unsigned long)nwritten));

	if ((nwritten == 0) && (state->in_length != 0)) {
		DEBUG(5,("smb2: write [%s] disk full\n",
			fsp_str_dbg(fsp)));
		return NT_STATUS_DISK_FULL;
	}

	if (do_sync) {
		status = sync_file(fsp->conn, fsp, state->write_through);
		if (!NT_STATUS_IS_OK(status)) {
			DEBUG(5,("smb2: sync_file for %s returned %s\n",
				 fsp_str_dbg(fsp),
				 nt_errstr(status)));
			return status;
		}
	}

	state->out_count = nwritten;

	return NT_STATUS_OK;
}

NTSTATUS smb2_write_complete(struct tevent_req *req, ssize_t nwritten, int err)
{
	return smb2_write_complete_internal(req, nwritten, err, true);
}

NTSTATUS smb2_write_complete_nosync(struct tevent_req *req, ssize_t nwritten,
				    int err)
{
	return smb2_write_complete_internal(req, nwritten, err, false);
}


static bool smbd_smb2_write_cancel(struct tevent_req *req)
{
	struct smbd_smb2_write_state *state =
		tevent_req_data(req,
		struct smbd_smb2_write_state);

	return cancel_smb2_aio(state->smbreq);
}

static struct tevent_req *smbd_smb2_write_send(TALLOC_CTX *mem_ctx,
					       struct tevent_context *ev,
					       struct smbd_smb2_request *smb2req,
					       struct files_struct *fsp,
					       DATA_BLOB in_data,
					       off_t in_offset,
					       uint32_t in_flags)
{
	NTSTATUS status;
	struct tevent_req *req = NULL;
	struct smbd_smb2_write_state *state = NULL;
	struct smb_request *smbreq = NULL;
	connection_struct *conn = smb2req->tcon->compat;
	ssize_t nwritten;
	struct lock_struct lock;

	req = tevent_req_create(mem_ctx, &state,
				struct smbd_smb2_write_state);
	if (req == NULL) {
		return NULL;
	}
	state->smb2req = smb2req;
	if (smb2req->xconn->protocol >= PROTOCOL_SMB3_02) {
		if (in_flags & SMB2_WRITEFLAG_WRITE_UNBUFFERED) {
			state->write_through = true;
		}
	}
	if (in_flags & SMB2_WRITEFLAG_WRITE_THROUGH) {
		state->write_through = true;
	}
	state->in_length = in_data.length;
	state->in_offset = in_offset;
	state->out_count = 0;

	DEBUG(10,("smbd_smb2_write: %s - %s\n",
		  fsp_str_dbg(fsp), fsp_fnum_dbg(fsp)));

	smbreq = smbd_smb2_fake_smb_request(smb2req, fsp);
	if (tevent_req_nomem(smbreq, req)) {
		return tevent_req_post(req, ev);
	}
	state->smbreq = smbreq;

	state->fsp = fsp;

	if (IS_IPC(smbreq->conn)) {
		struct tevent_req *subreq = NULL;
                bool ok;

		if (!fsp_is_np(fsp)) {
			tevent_req_nterror(req, NT_STATUS_FILE_CLOSED);
			return tevent_req_post(req, ev);
		}

		subreq = np_write_send(state, ev,
				       fsp->fake_file_handle,
				       in_data.data,
				       in_data.length);
		if (tevent_req_nomem(subreq, req)) {
			return tevent_req_post(req, ev);
		}
		tevent_req_set_callback(subreq,
					smbd_smb2_write_pipe_done,
					req);

		/*
		 * Make sure we mark the fsp as having outstanding async
		 * activity so we don't crash on shutdown close.
		 */

		ok = aio_add_req_to_fsp(fsp, req);
		if (!ok) {
			tevent_req_nterror(req, NT_STATUS_NO_MEMORY);
			return tevent_req_post(req, ev);
		}

		return req;
	}

	status = check_any_access_fsp(fsp, FILE_WRITE_DATA|FILE_APPEND_DATA);
	if (!NT_STATUS_IS_OK(status)) {
		tevent_req_nterror(req, status);
		return tevent_req_post(req, ev);
	}

	if (state->in_offset == VFS_PWRITE_APPEND_OFFSET &&
	    !fsp->fsp_flags.posix_append)
	{
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return tevent_req_post(req, ev);
	} else if (fsp->fsp_flags.posix_append &&
		   state->in_offset != VFS_PWRITE_APPEND_OFFSET)
	{
		tevent_req_nterror(req, NT_STATUS_INVALID_PARAMETER);
		return tevent_req_post(req, ev);
	}

	/* Try and do an asynchronous write. */
	status = schedule_aio_smb2_write(conn,
					smbreq,
					fsp,
					in_offset,
					in_data,
					state->write_through);

	if (NT_STATUS_IS_OK(status)) {
		/*
		 * Doing an async write, allow this
		 * request to be canceled
		 */
		tevent_req_set_cancel_fn(req, smbd_smb2_write_cancel);
		return req;
	}

	if (!NT_STATUS_EQUAL(status, NT_STATUS_RETRY)) {
		/* Real error in setting up aio. Fail. */
		tevent_req_nterror(req, status);
		return tevent_req_post(req, ev);
	}

	/* Fallback to synchronous. */
	init_strict_lock_struct(fsp,
				fsp->op->global->open_persistent_id,
				in_offset,
				in_data.length,
				WRITE_LOCK,
				&lock);

	if (!SMB_VFS_STRICT_LOCK_CHECK(conn, fsp, &lock)) {
		tevent_req_nterror(req, NT_STATUS_FILE_LOCK_CONFLICT);
		return tevent_req_post(req, ev);
	}

	/*
	 * Note: in_data.data is NULL for the recvfile case.
	 */
	nwritten = write_file(smbreq, fsp,
			      (const char *)in_data.data,
			      in_offset,
			      in_data.length);

	status = smb2_write_complete(req, nwritten, errno);

	DEBUG(10,("smb2: write on "
		"file %s, offset %.0f, requested %u, written = %u\n",
		fsp_str_dbg(fsp),
		(double)in_offset,
		(unsigned int)in_data.length,
		(unsigned int)nwritten ));

	if (!NT_STATUS_IS_OK(status)) {
		tevent_req_nterror(req, status);
	} else {
		/* Success. */
		tevent_req_done(req);
	}

	return tevent_req_post(req, ev);
}

static void smbd_smb2_write_pipe_done(struct tevent_req *subreq)
{
	struct tevent_req *req = tevent_req_callback_data(subreq,
				 struct tevent_req);
	struct smbd_smb2_write_state *state = tevent_req_data(req,
					      struct smbd_smb2_write_state);
	NTSTATUS status;
	ssize_t nwritten = -1;

	status = np_write_recv(subreq, &nwritten);
	TALLOC_FREE(subreq);
	if (!NT_STATUS_IS_OK(status)) {
		NTSTATUS old = status;
		status = nt_status_np_pipe(old);
		tevent_req_nterror(req, status);
		return;
	}

	if ((nwritten == 0 && state->in_length != 0) || (nwritten < 0)) {
		tevent_req_nterror(req, NT_STATUS_ACCESS_DENIED);
		return;
	}

	state->out_count = nwritten;

	tevent_req_done(req);
}

static NTSTATUS smbd_smb2_write_recv(struct tevent_req *req,
				     uint32_t *out_count)
{
	NTSTATUS status;
	struct smbd_smb2_write_state *state = tevent_req_data(req,
					      struct smbd_smb2_write_state);

	if (tevent_req_is_nterror(req, &status)) {
		tevent_req_received(req);
		return status;
	}

	*out_count = state->out_count;

	tevent_req_received(req);
	return NT_STATUS_OK;
}
