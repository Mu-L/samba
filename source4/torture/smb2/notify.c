/*
   Unix SMB/CIFS implementation.

   SMB2 notify test suite

   Copyright (C) Stefan Metzmacher 2006
   Copyright (C) Andrew Tridgell 2009

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
#include "libcli/smb2/smb2.h"
#include "libcli/smb2/smb2_calls.h"
#include "../libcli/smb/smbXcli_base.h"

#include "torture/torture.h"
#include "torture/smb2/proto.h"
#include "librpc/gen_ndr/ndr_security.h"
#include "libcli/security/security.h"
#include "torture/util.h"

#include "system/filesys.h"
#include "auth/credentials/credentials.h"
#include "lib/cmdline/cmdline.h"
#include "librpc/gen_ndr/security.h"

#include "lib/events/events.h"

#include "libcli/raw/libcliraw.h"
#include "libcli/raw/raw_proto.h"
#include "libcli/libcli.h"

#define CHECK_STATUS(status, correct) do { \
	if (!NT_STATUS_EQUAL(status, correct)) { \
		torture_result(torture, TORTURE_FAIL, \
		       "(%s) Incorrect status %s - should be %s\n", \
		       __location__, nt_errstr(status), nt_errstr(correct)); \
		ret = false; \
		goto done; \
	}} while (0)

#define CHECK_VAL(v, correct) do { \
	if ((v) != (correct)) { \
		torture_result(torture, TORTURE_FAIL, \
		       "(%s) wrong value for %s  0x%x should be 0x%x\n", \
		       __location__, #v, (int)v, (int)correct); \
		ret = false; \
		goto done; \
	}} while (0)

#define CHECK_WIRE_STR(field, value) do { \
	if (!field.s || strcmp(field.s, value)) { \
		torture_result(torture, TORTURE_FAIL, \
			"(%s) %s [%s] != %s\n",  __location__, #field, \
			field.s, value); \
		ret = false; \
		goto done; \
	}} while (0)

#define WAIT_FOR_ASYNC_RESPONSE(req) \
	while (!req->cancel.can_cancel && req->state <= SMB2_REQUEST_RECV) { \
		if (tevent_loop_once(torture->ev) != 0) { \
			break; \
		} \
	}

#define BASEDIR "test_notify"
#define FNAME "smb2-notify01.dat"

static bool test_valid_request(struct torture_context *torture,
			       struct smb2_tree *tree)
{
	bool ret = true;
	NTSTATUS status;
	struct smb2_handle dh;
	struct smb2_notify n;
	struct smb2_request *req;
	uint32_t max_buffer_size;

	torture_comment(torture, "TESTING VALIDITY OF CHANGE NOTIFY REQUEST\n");

	smb2_transport_credits_ask_num(tree->session->transport, 256);

	smb2_util_unlink(tree, FNAME);

	status = smb2_util_roothandle(tree, &dh);
	CHECK_STATUS(status, NT_STATUS_OK);

	max_buffer_size =
		smb2cli_conn_max_trans_size(tree->session->transport->conn);

	n.in.recursive		= 0x0000;
	n.in.buffer_size	= max_buffer_size;
	n.in.file.handle	= dh;
	n.in.completion_filter	= FILE_NOTIFY_CHANGE_ALL;
	n.in.unknown		= 0x00000000;
	req = smb2_notify_send(tree, &n);

	while (!req->cancel.can_cancel && req->state <= SMB2_REQUEST_RECV) {
		if (tevent_loop_once(torture->ev) != 0) {
			break;
		}
	}

	status = torture_setup_simple_file(torture, tree, FNAME);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, torture, &n);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(n.out.num_changes, 1);
	CHECK_VAL(n.out.changes[0].action, NOTIFY_ACTION_ADDED);
	CHECK_WIRE_STR(n.out.changes[0].name, FNAME);

	/*
	 * if the change response doesn't fit in the buffer
	 * NOTIFY_ENUM_DIR is returned.
	 */
	n.in.buffer_size	= 0x00000000;
	req = smb2_notify_send(tree, &n);

	while (!req->cancel.can_cancel && req->state <= SMB2_REQUEST_RECV) {
		if (tevent_loop_once(torture->ev) != 0) {
			break;
		}
	}

	status = torture_setup_simple_file(torture, tree, FNAME);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, torture, &n);
	CHECK_STATUS(status, NT_STATUS_NOTIFY_ENUM_DIR);

	/*
	 * if the change response fits in the buffer we get
	 * NT_STATUS_OK again
	 */
	n.in.buffer_size	= max_buffer_size;
	req = smb2_notify_send(tree, &n);

	while (!req->cancel.can_cancel && req->state <= SMB2_REQUEST_RECV) {
		if (tevent_loop_once(torture->ev) != 0) {
			break;
		}
	}

	status = torture_setup_simple_file(torture, tree, FNAME);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, torture, &n);
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(n.out.num_changes, 3);
	CHECK_VAL(n.out.changes[0].action, NOTIFY_ACTION_REMOVED);
	CHECK_WIRE_STR(n.out.changes[0].name, FNAME);
	CHECK_VAL(n.out.changes[1].action, NOTIFY_ACTION_ADDED);
	CHECK_WIRE_STR(n.out.changes[1].name, FNAME);
	CHECK_VAL(n.out.changes[2].action, NOTIFY_ACTION_MODIFIED);
	CHECK_WIRE_STR(n.out.changes[2].name, FNAME);

	/* if the first notify returns NOTIFY_ENUM_DIR, all do */
	status = smb2_util_close(tree, dh);
	CHECK_STATUS(status, NT_STATUS_OK);
	status = smb2_util_roothandle(tree, &dh);
	CHECK_STATUS(status, NT_STATUS_OK);

	n.in.recursive		= 0x0000;
	n.in.buffer_size	= 0x00000001;
	n.in.file.handle	= dh;
	n.in.completion_filter	= FILE_NOTIFY_CHANGE_ALL;
	n.in.unknown		= 0x00000000;
	req = smb2_notify_send(tree, &n);

	while (!req->cancel.can_cancel && req->state <= SMB2_REQUEST_RECV) {
		if (tevent_loop_once(torture->ev) != 0) {
			break;
		}
	}

	status = torture_setup_simple_file(torture, tree, FNAME);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, torture, &n);
	CHECK_STATUS(status, NT_STATUS_NOTIFY_ENUM_DIR);

	n.in.buffer_size        = max_buffer_size;
	req = smb2_notify_send(tree, &n);
	while (!req->cancel.can_cancel && req->state <= SMB2_REQUEST_RECV) {
		if (tevent_loop_once(torture->ev) != 0) {
			break;
		}
	}

	status = torture_setup_simple_file(torture, tree, FNAME);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, torture, &n);
	CHECK_STATUS(status, NT_STATUS_NOTIFY_ENUM_DIR);

	/* if the buffer size is too large, we get invalid parameter */
	n.in.recursive		= 0x0000;
	n.in.buffer_size	= max_buffer_size + 1;
	n.in.file.handle	= dh;
	n.in.completion_filter	= FILE_NOTIFY_CHANGE_ALL;
	n.in.unknown		= 0x00000000;
	req = smb2_notify_send(tree, &n);
	status = smb2_notify_recv(req, torture, &n);
	CHECK_STATUS(status, NT_STATUS_INVALID_PARAMETER);

done:
	return ret;
}

/*
   basic testing of change notify on directories
*/

#define BASEDIR_DIR BASEDIR "_DIR"

static bool torture_smb2_notify_dir(struct torture_context *torture,
			      struct smb2_tree *tree1,
			      struct smb2_tree *tree2)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io;
	union smb_close cl;
	int i, count;
	struct smb2_handle h1 = {{0}};
	struct smb2_handle h2 = {{0}};
	struct smb2_request *req, *req2;
	const char *fname = BASEDIR_DIR "\\subdir-name";
	extern int torture_numops;

	torture_comment(torture, "TESTING CHANGE NOTIFY ON DIRECTORIES\n");

	smb2_deltree(tree1, BASEDIR_DIR);
	smb2_util_rmdir(tree1, BASEDIR_DIR);
	/*
	  get a handle on the directory
	*/
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_DIR;

	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	io.smb2.in.create_disposition = NTCREATEX_DISP_OPEN;
	io.smb2.in.desired_access = SEC_RIGHTS_FILE_READ;
	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h2 = io.smb2.out.file.handle;

	/* ask for a change notify,
	   on file or directory name changes */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.smb2.in.file.handle = h1;
	notify.smb2.in.recursive = true;

	torture_comment(torture, "Testing notify cancel\n");

	req = smb2_notify_send(tree1, &(notify.smb2));
	smb2_cancel(req);
	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_CANCELLED);

	torture_comment(torture, "Testing notify mkdir\n");

	req = smb2_notify_send(tree1, &(notify.smb2));
	smb2_util_mkdir(tree2, fname);

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_ADDED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subdir-name");

	torture_comment(torture, "Testing notify rmdir\n");

	req = smb2_notify_send(tree1, &(notify.smb2));
	smb2_util_rmdir(tree2, fname);

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_REMOVED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subdir-name");

	torture_comment(torture,
		"Testing notify mkdir - rmdir - mkdir - rmdir\n");

	smb2_util_mkdir(tree2, fname);
	smb2_util_rmdir(tree2, fname);
	smb2_util_mkdir(tree2, fname);
	smb2_util_rmdir(tree2, fname);
	smb_msleep(200);
	req = smb2_notify_send(tree1, &(notify.smb2));
	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(notify.smb2.out.num_changes, 4);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_ADDED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subdir-name");
	CHECK_VAL(notify.smb2.out.changes[1].action, NOTIFY_ACTION_REMOVED);
	CHECK_WIRE_STR(notify.smb2.out.changes[1].name, "subdir-name");
	CHECK_VAL(notify.smb2.out.changes[2].action, NOTIFY_ACTION_ADDED);
	CHECK_WIRE_STR(notify.smb2.out.changes[2].name, "subdir-name");
	CHECK_VAL(notify.smb2.out.changes[3].action, NOTIFY_ACTION_REMOVED);
	CHECK_WIRE_STR(notify.smb2.out.changes[3].name, "subdir-name");

	count = torture_numops;
	torture_comment(torture,
		"Testing buffered notify on create of %d files\n", count);
	for (i=0;i<count;i++) {
		struct smb2_handle h12;
		char *fname2 = talloc_asprintf(torture,
						BASEDIR_DIR "\\test%d.txt",
						i);

		ZERO_STRUCT(io.smb2);
		io.generic.level = RAW_OPEN_SMB2;
	        io.smb2.in.create_flags = 0;
		io.smb2.in.desired_access = SEC_FILE_ALL;
	        io.smb2.in.create_options =
		    NTCREATEX_OPTIONS_NON_DIRECTORY_FILE;
		io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	        io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
					NTCREATEX_SHARE_ACCESS_WRITE;
		io.smb2.in.alloc_size = 0;
	        io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
		io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	        io.smb2.in.security_flags = 0;
		io.smb2.in.fname = fname2;

		status = smb2_create(tree1, torture, &(io.smb2));
		if (!NT_STATUS_EQUAL(status, NT_STATUS_OK)) {
			torture_comment(torture, "Failed to create %s \n",
			       fname);
			ret = false;
			goto done;
		}
		h12 = io.smb2.out.file.handle;
		talloc_free(fname2);
		smb2_util_close(tree1, h12);
	}

	/* (1st notify) setup a new notify on a different directory handle.
	   This new notify won't see the events above. */
	notify.smb2.in.file.handle = h2;
	req2 = smb2_notify_send(tree1, &(notify.smb2));

	/* (2nd notify) whereas this notify will see the above buffered events,
	   and it directly returns the buffered events */
	notify.smb2.in.file.handle = h1;
	req = smb2_notify_send(tree1, &(notify.smb2));

	status = smb2_util_unlink(tree1, BASEDIR_DIR "\\nonexistent.txt");
	CHECK_STATUS(status, NT_STATUS_OBJECT_NAME_NOT_FOUND);

	/* (1st unlink) as the 2nd notify directly returns,
	   this unlink is only seen by the 1st notify and
	   the 3rd notify (later) */
	torture_comment(torture,
		"Testing notify on unlink for the first file\n");
	status = smb2_util_unlink(tree2, BASEDIR_DIR "\\test0.txt");
	CHECK_STATUS(status, NT_STATUS_OK);

	/* receive the reply from the 2nd notify */
	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	CHECK_VAL(notify.smb2.out.num_changes, count);
	for (i=1;i<count;i++) {
		CHECK_VAL(notify.smb2.out.changes[i].action,
			  NOTIFY_ACTION_ADDED);
	}
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "test0.txt");

	torture_comment(torture, "and now from the 1st notify\n");
	status = smb2_notify_recv(req2, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_REMOVED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "test0.txt");

	torture_comment(torture,
		"(3rd notify) this notify will only see the 1st unlink\n");
	req = smb2_notify_send(tree1, &(notify.smb2));

	status = smb2_util_unlink(tree1, BASEDIR_DIR "\\nonexistent.txt");
	CHECK_STATUS(status, NT_STATUS_OBJECT_NAME_NOT_FOUND);

	for (i=1;i<count;i++) {
		char *fname2 = talloc_asprintf(torture,
			      BASEDIR_DIR "\\test%d.txt", i);
		status = smb2_util_unlink(tree2, fname2);
		CHECK_STATUS(status, NT_STATUS_OK);
		talloc_free(fname2);
	}

	/* receive the 3rd notify */
	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_REMOVED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "test0.txt");

	/* and we now see the rest of the unlink calls on both
	 * directory handles */
	notify.smb2.in.file.handle = h1;
	sleep(3);
	req = smb2_notify_send(tree1, &(notify.smb2));
	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(notify.smb2.out.num_changes, count-1);
	for (i=0;i<notify.smb2.out.num_changes;i++) {
		CHECK_VAL(notify.smb2.out.changes[i].action,
			  NOTIFY_ACTION_REMOVED);
	}
	notify.smb2.in.file.handle = h2;
	req = smb2_notify_send(tree1, &(notify.smb2));
	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(notify.smb2.out.num_changes, count-1);
	for (i=0;i<notify.smb2.out.num_changes;i++) {
		CHECK_VAL(notify.smb2.out.changes[i].action,
			  NOTIFY_ACTION_REMOVED);
	}

	torture_comment(torture,
	"Testing if a close() on the dir handle triggers the notify reply\n");

	notify.smb2.in.file.handle = h1;
	req = smb2_notify_send(tree1, &(notify.smb2));

	ZERO_STRUCT(cl.smb2);
	cl.smb2.level = RAW_CLOSE_SMB2;
	cl.smb2.in.file.handle = h1;
	status = smb2_close(tree1, &(cl.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_NOTIFY_CLEANUP);
	CHECK_VAL(notify.smb2.out.num_changes, 9);

done:
	smb2_util_close(tree1, h1);
	smb2_util_close(tree1, h2);
	smb2_deltree(tree1, BASEDIR_DIR);
	return ret;
}

static struct smb2_handle custom_smb2_create(struct smb2_tree *tree,
						struct torture_context *torture,
						struct smb2_create *smb2)
{
	struct smb2_handle h1;
	bool ret = true;
	NTSTATUS status;
	smb2_deltree(tree, smb2->in.fname);
	status = smb2_create(tree, torture, smb2);
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = smb2->out.file.handle;
done:
	if (!ret) {
		h1 = (struct smb2_handle) {
			.data = { 0 , 0},
		};
	}
	return h1;
}

/*
   testing of recursive change notify
*/

#define BASEDIR_REC BASEDIR "_REC"

static bool torture_smb2_notify_recursive(struct torture_context *torture,
				struct smb2_tree *tree1,
				struct smb2_tree *tree2)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io, io1;
	union smb_setfileinfo sinfo;
	struct smb2_handle h1;
	struct smb2_request *req1, *req2;

	smb2_deltree(tree1, BASEDIR_REC);
	smb2_util_rmdir(tree1, BASEDIR_REC);

	torture_comment(torture, "TESTING CHANGE NOTIFY WITH RECURSION\n");

	/*
	  get a handle on the directory
	*/
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_REC;

	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify, on file or directory name
	   changes. Setup both with and without recursion */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME |
				FILE_NOTIFY_CHANGE_ATTRIBUTES |
				FILE_NOTIFY_CHANGE_CREATION;
	notify.smb2.in.file.handle = h1;

	notify.smb2.in.recursive = true;
	req1 = smb2_notify_send(tree1, &(notify.smb2));
	smb2_cancel(req1);
	status = smb2_notify_recv(req1, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_CANCELLED);

	notify.smb2.in.recursive = false;
	req2 = smb2_notify_send(tree1, &(notify.smb2));
	smb2_cancel(req2);
	status = smb2_notify_recv(req2, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_CANCELLED);

	ZERO_STRUCT(io1.smb2);
	io1.generic.level = RAW_OPEN_SMB2;
	io1.smb2.in.create_flags = NTCREATEX_FLAGS_EXTENDED;
	io1.smb2.in.desired_access = SEC_RIGHTS_FILE_READ |
				SEC_RIGHTS_FILE_WRITE|
				SEC_RIGHTS_FILE_ALL;
	io1.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io1.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io1.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE |
				NTCREATEX_SHARE_ACCESS_DELETE;
	io1.smb2.in.alloc_size = 0;
	io1.smb2.in.create_disposition = NTCREATEX_DISP_OPEN_IF;
	io1.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io1.smb2.in.security_flags = 0;
	io1.smb2.in.fname = BASEDIR_REC "\\subdir-name";
	status = smb2_create(tree2, torture, &(io1.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	smb2_util_close(tree2, io1.smb2.out.file.handle);

	io1.smb2.in.fname = BASEDIR_REC "\\subdir-name\\subname1";
	status = smb2_create(tree2, torture, &(io1.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	ZERO_STRUCT(sinfo);
	sinfo.rename_information.level = RAW_SFILEINFO_RENAME_INFORMATION;
	sinfo.rename_information.in.file.handle = io1.smb2.out.file.handle;
	sinfo.rename_information.in.overwrite = 0;
	sinfo.rename_information.in.root_fid = 0;
	sinfo.rename_information.in.new_name =
				BASEDIR_REC "\\subdir-name\\subname1-r";
	status = smb2_setinfo_file(tree2, &sinfo);
	CHECK_STATUS(status, NT_STATUS_OK);

	io1.smb2.in.create_options = NTCREATEX_OPTIONS_NON_DIRECTORY_FILE;
	io1.smb2.in.fname = BASEDIR_REC "\\subdir-name\\subname2";
	status = smb2_create(tree2, torture, &(io1.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	ZERO_STRUCT(sinfo);
	sinfo.rename_information.level = RAW_SFILEINFO_RENAME_INFORMATION;
	sinfo.rename_information.in.file.handle = io1.smb2.out.file.handle;
	sinfo.rename_information.in.overwrite = true;
	sinfo.rename_information.in.root_fid = 0;
	sinfo.rename_information.in.new_name = BASEDIR_REC "\\subname2-r";
	status = smb2_setinfo_file(tree2, &sinfo);
	CHECK_STATUS(status, NT_STATUS_OK);

	io1.smb2.in.fname = BASEDIR_REC "\\subname2-r";
	io1.smb2.in.create_disposition = NTCREATEX_DISP_OPEN;
	status = smb2_create(tree2, torture, &(io1.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	ZERO_STRUCT(sinfo);
	sinfo.rename_information.level = RAW_SFILEINFO_RENAME_INFORMATION;
	sinfo.rename_information.in.file.handle = io1.smb2.out.file.handle;
	sinfo.rename_information.in.overwrite = true;
	sinfo.rename_information.in.root_fid = 0;
	sinfo.rename_information.in.new_name = BASEDIR_REC "\\subname3-r";
	status = smb2_setinfo_file(tree2, &sinfo);
	CHECK_STATUS(status, NT_STATUS_OK);

	notify.smb2.in.completion_filter = 0;
	notify.smb2.in.recursive = true;
	smb_msleep(200);
	req1 = smb2_notify_send(tree1, &(notify.smb2));

	status = smb2_util_rmdir(tree2,
		BASEDIR_REC "\\subdir-name\\subname1-r");
	CHECK_STATUS(status, NT_STATUS_OK);
	status = smb2_util_rmdir(tree2,
		BASEDIR_REC "\\subdir-name");
	CHECK_STATUS(status, NT_STATUS_OK);
	status = smb2_util_unlink(tree2, BASEDIR_REC "\\subname3-r");
	CHECK_STATUS(status, NT_STATUS_OK);

	notify.smb2.in.recursive = false;
	req2 = smb2_notify_send(tree1, &(notify.smb2));

	status = smb2_notify_recv(req1, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	CHECK_VAL(notify.smb2.out.num_changes, 9);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_ADDED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subdir-name");
	CHECK_VAL(notify.smb2.out.changes[1].action, NOTIFY_ACTION_ADDED);
	CHECK_WIRE_STR(notify.smb2.out.changes[1].name, "subdir-name\\subname1");
	CHECK_VAL(notify.smb2.out.changes[2].action, NOTIFY_ACTION_OLD_NAME);
	CHECK_WIRE_STR(notify.smb2.out.changes[2].name, "subdir-name\\subname1");
	CHECK_VAL(notify.smb2.out.changes[3].action, NOTIFY_ACTION_NEW_NAME);
	CHECK_WIRE_STR(notify.smb2.out.changes[3].name, "subdir-name\\subname1-r");
	CHECK_VAL(notify.smb2.out.changes[4].action, NOTIFY_ACTION_ADDED);
	CHECK_WIRE_STR(notify.smb2.out.changes[4].name, "subdir-name\\subname2");
	CHECK_VAL(notify.smb2.out.changes[5].action, NOTIFY_ACTION_REMOVED);
	CHECK_WIRE_STR(notify.smb2.out.changes[5].name, "subdir-name\\subname2");
	CHECK_VAL(notify.smb2.out.changes[6].action, NOTIFY_ACTION_ADDED);
	CHECK_WIRE_STR(notify.smb2.out.changes[6].name, "subname2-r");
	CHECK_VAL(notify.smb2.out.changes[7].action, NOTIFY_ACTION_OLD_NAME);
	CHECK_WIRE_STR(notify.smb2.out.changes[7].name, "subname2-r");
	CHECK_VAL(notify.smb2.out.changes[8].action, NOTIFY_ACTION_NEW_NAME);
	CHECK_WIRE_STR(notify.smb2.out.changes[8].name, "subname3-r");

done:
	smb2_deltree(tree1, BASEDIR_REC);
	return ret;
}

/*
   testing of change notify mask change
*/

#define BASEDIR_MC BASEDIR "_MC"

static bool torture_smb2_notify_mask_change(struct torture_context *torture,
					    struct smb2_tree *tree1,
					    struct smb2_tree *tree2)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io, io1;
	struct smb2_handle h1;
	struct smb2_request *req1, *req2;
	union smb_setfileinfo sinfo;

	smb2_deltree(tree1, BASEDIR_MC);
	smb2_util_rmdir(tree1, BASEDIR_MC);

	torture_comment(torture, "TESTING CHANGE NOTIFY WITH MASK CHANGE\n");

	/*
	  get a handle on the directory
	*/
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_MC;

	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify, on file or directory name
	   changes. Setup both with and without recursion */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_ATTRIBUTES;
	notify.smb2.in.file.handle = h1;

	notify.smb2.in.recursive = true;
	req1 = smb2_notify_send(tree1, &(notify.smb2));

	smb2_cancel(req1);
	status = smb2_notify_recv(req1, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_CANCELLED);


	notify.smb2.in.recursive = false;
	req2 = smb2_notify_send(tree1, &(notify.smb2));

	smb2_cancel(req2);
	status = smb2_notify_recv(req2, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_CANCELLED);

	notify.smb2.in.recursive = true;
	req1 = smb2_notify_send(tree1, &(notify.smb2));

	/* Set to hidden then back again. */
	ZERO_STRUCT(io1.smb2);
	io1.generic.level = RAW_OPEN_SMB2;
	io1.smb2.in.create_flags = 0;
	io1.smb2.in.desired_access = SEC_RIGHTS_FILE_READ |
				SEC_RIGHTS_FILE_WRITE|
				SEC_RIGHTS_FILE_ALL;
	io1.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io1.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE |
				NTCREATEX_SHARE_ACCESS_DELETE;
	io1.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io1.smb2.in.security_flags = 0;
	io1.smb2.in.create_options = NTCREATEX_OPTIONS_NON_DIRECTORY_FILE;
	io1.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io1.smb2.in.fname = BASEDIR_MC "\\tname1";

	smb2_util_close(tree1,
		custom_smb2_create(tree1, torture, &(io1.smb2)));
	status = smb2_util_setatr(tree1, BASEDIR_MC "\\tname1",
				FILE_ATTRIBUTE_HIDDEN);
	CHECK_STATUS(status, NT_STATUS_OK);
	smb2_util_unlink(tree1, BASEDIR_MC "\\tname1");

	status = smb2_notify_recv(req1, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_MODIFIED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "tname1");

	/* Now try and change the mask to include other events.
	 * This should not work - once the mask is set on a directory
	 * h1 it seems to be fixed until the fnum is closed. */

	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME |
					FILE_NOTIFY_CHANGE_ATTRIBUTES |
					FILE_NOTIFY_CHANGE_CREATION;
	notify.smb2.in.recursive = true;
	req1 = smb2_notify_send(tree1, &(notify.smb2));

	notify.smb2.in.recursive = false;
	req2 = smb2_notify_send(tree1, &(notify.smb2));

	io1.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io1.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io1.smb2.in.fname = BASEDIR_MC "\\subdir-name";
	status = smb2_create(tree2, torture, &(io1.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	smb2_util_close(tree2, io1.smb2.out.file.handle);

	ZERO_STRUCT(sinfo);
	io1.smb2.in.fname = BASEDIR_MC "\\subdir-name\\subname1";
	io1.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io1.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	status = smb2_create(tree2, torture, &(io1.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	sinfo.rename_information.level = RAW_SFILEINFO_RENAME_INFORMATION;
	sinfo.rename_information.in.file.handle = io1.smb2.out.file.handle;
	sinfo.rename_information.in.overwrite = true;
	sinfo.rename_information.in.root_fid = 0;
	sinfo.rename_information.in.new_name =
				BASEDIR_MC "\\subdir-name\\subname1-r";
	status = smb2_setinfo_file(tree2, &sinfo);
	CHECK_STATUS(status, NT_STATUS_OK);

	io1.smb2.in.fname = BASEDIR_MC "\\subdir-name\\subname2";
	io1.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io1.smb2.in.create_options = NTCREATEX_OPTIONS_NON_DIRECTORY_FILE;
	status = smb2_create(tree2, torture, &(io1.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	sinfo.rename_information.in.file.handle = io1.smb2.out.file.handle;
	sinfo.rename_information.in.new_name = BASEDIR_MC "\\subname2-r";
	status = smb2_setinfo_file(tree2, &sinfo);
	CHECK_STATUS(status, NT_STATUS_OK);
	smb2_util_close(tree2, io1.smb2.out.file.handle);

	io1.smb2.in.fname = BASEDIR_MC "\\subname2-r";
	io1.smb2.in.create_disposition = NTCREATEX_DISP_OPEN;
	status = smb2_create(tree2, torture, &(io1.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	sinfo.rename_information.in.file.handle = io1.smb2.out.file.handle;
	sinfo.rename_information.in.new_name = BASEDIR_MC "\\subname3-r";
	status = smb2_setinfo_file(tree2, &sinfo);
	CHECK_STATUS(status, NT_STATUS_OK);
	smb2_util_close(tree2, io1.smb2.out.file.handle);

	status = smb2_util_rmdir(tree2, BASEDIR_MC "\\subdir-name\\subname1-r");
	CHECK_STATUS(status, NT_STATUS_OK);
	status = smb2_util_rmdir(tree2, BASEDIR_MC "\\subdir-name");
	CHECK_STATUS(status, NT_STATUS_OK);
	status = smb2_util_unlink(tree2, BASEDIR_MC "\\subname3-r");
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req1, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_MODIFIED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subname2-r");

	status = smb2_notify_recv(req2, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_MODIFIED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subname3-r");

	if (!ret) {
		goto done;
	}

done:
	smb2_deltree(tree1, BASEDIR_MC);
	return ret;
}

/*
   testing of mask bits for change notify
*/

#define BASEDIR_MSK BASEDIR "_MSK"

static bool torture_smb2_notify_mask(struct torture_context *torture,
				     struct smb2_tree *tree1,
				     struct smb2_tree *tree2)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io, io1;
	struct smb2_handle h1, h2;
	int i;
	char c = 1;
	union smb_setfileinfo sinfo;

	smb2_deltree(tree1, BASEDIR_MSK);
	smb2_util_rmdir(tree1, BASEDIR_MSK);

	torture_comment(torture, "TESTING CHANGE NOTIFY COMPLETION FILTERS\n");


	ZERO_STRUCT(h1);
	ZERO_STRUCT(h2);
	/*
	  get a handle on the directory
	*/
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_OPEN_IF;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_MSK;

	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.recursive = true;

#define NOTIFY_MASK_TEST(test_name, setup, op, cleanup, Action, \
			 expected, nchanges) \
	do { \
	do { for (i=0;i<32;i++) { \
		struct smb2_request *req; \
		status = smb2_create(tree1, torture, &(io.smb2)); \
		CHECK_STATUS(status, NT_STATUS_OK); \
		h1 = io.smb2.out.file.handle; \
		setup \
		notify.smb2.in.file.handle = h1;	\
		notify.smb2.in.completion_filter = ((uint32_t)1<<i); \
		/* cancel initial requests so the buffer is setup */	\
		req = smb2_notify_send(tree1, &(notify.smb2)); \
		smb2_cancel(req); \
		status = smb2_notify_recv(req, torture, &(notify.smb2)); \
		CHECK_STATUS(status, NT_STATUS_CANCELLED); \
		/* send the change notify request */ \
		req = smb2_notify_send(tree1, &(notify.smb2)); \
		op \
		smb_msleep(200); smb2_cancel(req); \
		status = smb2_notify_recv(req, torture, &(notify.smb2)); \
		cleanup \
		smb2_util_close(tree1, h1); \
		if (NT_STATUS_EQUAL(status, NT_STATUS_CANCELLED)) continue; \
		CHECK_STATUS(status, NT_STATUS_OK); \
		/* special case to cope with file rename behaviour */ \
		if (nchanges == 2 && notify.smb2.out.num_changes == 1 && \
		    notify.smb2.out.changes[0].action == \
			NOTIFY_ACTION_MODIFIED && \
		    ((expected) & FILE_NOTIFY_CHANGE_ATTRIBUTES) && \
		    Action == NOTIFY_ACTION_OLD_NAME) { \
			torture_comment(torture, \
				"(rename file special handling OK)\n"); \
		} else if (nchanges != notify.smb2.out.num_changes) { \
			torture_result(torture, TORTURE_FAIL, \
			       "ERROR: nchanges=%d expected=%d "\
			       "action=%d filter=0x%08x\n", \
			       notify.smb2.out.num_changes, \
			       nchanges, \
			       notify.smb2.out.changes[0].action, \
			       notify.smb2.in.completion_filter); \
			ret = false; \
			goto done; \
		} else if (notify.smb2.out.changes[0].action != Action) { \
			torture_result(torture, TORTURE_FAIL, \
			       "ERROR: nchanges=%d action=%d " \
			       "expectedAction=%d filter=0x%08x\n", \
			       notify.smb2.out.num_changes, \
			       notify.smb2.out.changes[0].action, \
			       Action, \
			       notify.smb2.in.completion_filter); \
			ret = false; \
			goto done; \
		} else if (strcmp(notify.smb2.out.changes[0].name.s, \
			   "tname1") != 0) { \
			torture_result(torture, TORTURE_FAIL, \
			       "ERROR: nchanges=%d action=%d " \
			       "filter=0x%08x name=%s\n", \
			       notify.smb2.out.num_changes, \
			       notify.smb2.out.changes[0].action, \
			       notify.smb2.in.completion_filter, \
			       notify.smb2.out.changes[0].name.s);	\
			ret = false; \
			goto done; \
		} \
	} \
	} while (0); \
	} while (0);

	torture_comment(torture, "Testing mkdir\n");
	NOTIFY_MASK_TEST("Testing mkdir",;,
			 smb2_util_mkdir(tree2, BASEDIR_MSK "\\tname1");,
			 smb2_util_rmdir(tree2, BASEDIR_MSK "\\tname1");,
			 NOTIFY_ACTION_ADDED,
			 FILE_NOTIFY_CHANGE_DIR_NAME, 1);

	torture_comment(torture, "Testing create file\n");
	ZERO_STRUCT(io1.smb2);
	io1.generic.level = RAW_OPEN_SMB2;
	io1.smb2.in.create_flags = 0;
	io1.smb2.in.desired_access = SEC_FILE_ALL;
	io1.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io1.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io1.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io1.smb2.in.security_flags = 0;
	io1.smb2.in.create_options = NTCREATEX_OPTIONS_NON_DIRECTORY_FILE;
	io1.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io1.smb2.in.fname = BASEDIR_MSK "\\tname1";

	NOTIFY_MASK_TEST("Testing create file",;,
			 smb2_util_close(tree2, custom_smb2_create(tree2,
						torture, &(io1.smb2)));,
			 smb2_util_unlink(tree2, BASEDIR_MSK "\\tname1");,
			 NOTIFY_ACTION_ADDED,
			 FILE_NOTIFY_CHANGE_FILE_NAME, 1);

	torture_comment(torture, "Testing unlink\n");
	NOTIFY_MASK_TEST("Testing unlink",
			 smb2_util_close(tree2, custom_smb2_create(tree2,
						torture, &(io1.smb2)));,
			 smb2_util_unlink(tree2, BASEDIR_MSK "\\tname1");,
			 ;,
			 NOTIFY_ACTION_REMOVED,
			 FILE_NOTIFY_CHANGE_FILE_NAME, 1);

	torture_comment(torture, "Testing rmdir\n");
	NOTIFY_MASK_TEST("Testing rmdir",
			 smb2_util_mkdir(tree2, BASEDIR_MSK "\\tname1");,
			 smb2_util_rmdir(tree2, BASEDIR_MSK "\\tname1");,
			 ;,
			 NOTIFY_ACTION_REMOVED,
			 FILE_NOTIFY_CHANGE_DIR_NAME, 1);

	torture_comment(torture, "Testing rename file\n");
	ZERO_STRUCT(sinfo);
	sinfo.rename_information.level = RAW_SFILEINFO_RENAME_INFORMATION;
	sinfo.rename_information.in.overwrite = true;
	sinfo.rename_information.in.root_fid = 0;
	sinfo.rename_information.in.new_name = BASEDIR_MSK "\\tname2";
	NOTIFY_MASK_TEST("Testing rename file",
			 torture_smb2_testfile(tree2, BASEDIR_MSK "\\tname1", &h2);,
			 (sinfo.rename_information.in.file.handle = h2, smb2_setinfo_file(tree2, &sinfo));,
			 smb2_util_unlink(tree2, BASEDIR_MSK "\\tname2");,
			 NOTIFY_ACTION_OLD_NAME,
			 FILE_NOTIFY_CHANGE_FILE_NAME, 2);

	torture_comment(torture, "Testing rename dir\n");
	ZERO_STRUCT(sinfo);
	sinfo.rename_information.level = RAW_SFILEINFO_RENAME_INFORMATION;
	sinfo.rename_information.in.overwrite = true;
	sinfo.rename_information.in.root_fid = 0;
	sinfo.rename_information.in.new_name = BASEDIR_MSK "\\tname2";
	NOTIFY_MASK_TEST("Testing rename dir",
		torture_smb2_testdir(tree2, BASEDIR_MSK "\\tname1", &h2);,
		(sinfo.rename_information.in.file.handle = h2, smb2_setinfo_file(tree2, &sinfo));,
		(smb2_util_close(tree2, h2), smb2_util_rmdir(tree2, BASEDIR_MSK "\\tname2"));,
		NOTIFY_ACTION_OLD_NAME,
		FILE_NOTIFY_CHANGE_DIR_NAME, 2);

	torture_comment(torture, "Testing set path attribute\n");
	NOTIFY_MASK_TEST("Testing set path attribute",
		torture_setup_simple_file(torture, tree2, BASEDIR_MSK "\\tname1");,
		smb2_util_setatr(tree2, BASEDIR_MSK "\\tname1",
				 FILE_ATTRIBUTE_HIDDEN);,
		smb2_util_unlink(tree2, BASEDIR_MSK "\\tname1");,
		NOTIFY_ACTION_MODIFIED,
		FILE_NOTIFY_CHANGE_ATTRIBUTES, 1);

	torture_comment(torture, "Testing set path write time\n");
	ZERO_STRUCT(sinfo);
	sinfo.generic.level = RAW_SFILEINFO_BASIC_INFORMATION;
	sinfo.basic_info.in.write_time = 1000;
	NOTIFY_MASK_TEST("Testing set path write time",
		torture_setup_simple_file(torture, tree2, BASEDIR_MSK "\\tname1");,
		(sinfo.generic.in.file.handle = h2, smb2_setinfo_file(tree2, &sinfo));,
		smb2_util_unlink(tree2, BASEDIR_MSK "\\tname1");,
		NOTIFY_ACTION_MODIFIED,
		FILE_NOTIFY_CHANGE_LAST_WRITE, 1);

	if (torture_setting_bool(torture, "samba3", false)) {
		torture_comment(torture,
		       "Samba3 does not yet support create times "
		       "everywhere\n");
	}
	else {
		ZERO_STRUCT(sinfo);
	        sinfo.generic.level = RAW_SFILEINFO_BASIC_INFORMATION;
	        sinfo.basic_info.in.create_time = 0;
		torture_comment(torture, "Testing set file create time\n");
		NOTIFY_MASK_TEST("Testing set file create time",
			smb2_create_complex_file(torture, tree2,
			BASEDIR_MSK "\\tname1", &h2);,
			(sinfo.generic.in.file.handle = h2, smb2_setinfo_file(tree2, &sinfo));,
			(smb2_util_close(tree2, h2),
			 smb2_util_unlink(tree2, BASEDIR_MSK "\\tname1"));,
			NOTIFY_ACTION_MODIFIED,
			FILE_NOTIFY_CHANGE_CREATION, 1);
	}

	ZERO_STRUCT(sinfo);
	sinfo.generic.level = RAW_SFILEINFO_BASIC_INFORMATION;
	sinfo.basic_info.in.access_time = 0;
	torture_comment(torture, "Testing set file access time\n");
	NOTIFY_MASK_TEST("Testing set file access time",
		smb2_create_complex_file(torture,
			tree2,
			BASEDIR_MSK "\\tname1",
			&h2);,
		(sinfo.generic.in.file.handle = h2, smb2_setinfo_file(tree2, &sinfo));,
		(smb2_util_close(tree2, h2),
		smb2_util_unlink(tree2, BASEDIR_MSK "\\tname1"));,
		NOTIFY_ACTION_MODIFIED,
		FILE_NOTIFY_CHANGE_LAST_ACCESS, 1);

	ZERO_STRUCT(sinfo);
	sinfo.generic.level = RAW_SFILEINFO_BASIC_INFORMATION;
	sinfo.basic_info.in.change_time = 0;
	torture_comment(torture, "Testing set file change time\n");
	NOTIFY_MASK_TEST("Testing set file change time",
		smb2_create_complex_file(torture,
			tree2,
			BASEDIR_MSK "\\tname1",
			&h2);,
		(sinfo.generic.in.file.handle = h2, smb2_setinfo_file(tree2, &sinfo));,
		(smb2_util_close(tree2, h2),
		smb2_util_unlink(tree2, BASEDIR_MSK "\\tname1"));,
		NOTIFY_ACTION_MODIFIED,
		0, 1);


	torture_comment(torture, "Testing write\n");
	NOTIFY_MASK_TEST("Testing write",
		smb2_create_complex_file(torture,
			tree2,
			BASEDIR_MSK "\\tname1",
			&h2);,
		smb2_util_write(tree2, h2, &c, 10000, 1);,
		(smb2_util_close(tree2, h2),
		smb2_util_unlink(tree2, BASEDIR_MSK "\\tname1"));,
		NOTIFY_ACTION_MODIFIED,
		0, 1);

done:
	smb2_deltree(tree1, BASEDIR_MSK);
	return ret;
}

#define BASEDIR_FL BASEDIR "_FL"
/*
  basic testing of change notify on files
*/
static bool torture_smb2_notify_file(struct torture_context *torture,
				struct smb2_tree *tree)
{
	NTSTATUS status;
	bool ret = true;
	union smb_open io;
	union smb_close cl;
	union smb_notify notify;
	struct smb2_request *req;
	struct smb2_handle h1;
	const char *fname = BASEDIR_FL "\\file.txt";

	smb2_deltree(tree, BASEDIR_FL);
	smb2_util_rmdir(tree, BASEDIR_FL);

	torture_comment(torture, "TESTING CHANGE NOTIFY ON FILES\n");
	status = torture_smb2_testdir(tree, BASEDIR_FL, &h1);
	CHECK_STATUS(status, NT_STATUS_OK);

	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FLAG_MAXIMUM_ALLOWED;
	io.smb2.in.create_options = 0;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = fname;
	status = smb2_create(tree, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify,
	   on file or directory name changes */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.file.handle = h1;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_STREAM_NAME;
	notify.smb2.in.recursive = false;

	torture_comment(torture,
	"Testing if notifies on file handles are invalid (should be)\n");

	req = smb2_notify_send(tree, &(notify.smb2));
	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_INVALID_PARAMETER);

	ZERO_STRUCT(cl.smb2);
	cl.close.level = RAW_CLOSE_SMB2;
	cl.close.in.file.handle = h1;
	status = smb2_close(tree, &(cl.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_util_unlink(tree, fname);
	CHECK_STATUS(status, NT_STATUS_OK);

done:
	smb2_deltree(tree, BASEDIR_FL);
	return ret;
}
/*
  basic testing of change notifies followed by a tdis
*/

#define BASEDIR_TD BASEDIR "_TD"

static bool torture_smb2_notify_tree_disconnect(
		struct torture_context *torture,
		struct smb2_tree *tree)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io;
	struct smb2_handle h1;
	struct smb2_request *req;

	smb2_deltree(tree, BASEDIR_TD);
	smb2_util_rmdir(tree, BASEDIR_TD);

	torture_comment(torture, "TESTING CHANGE NOTIFY+CANCEL FOLLOWED BY "
			"TREE-DISCONNECT\n");

	/*
	  get a handle on the directory
	*/
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_TD;

	status = smb2_create(tree, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify,
	   on file or directory name changes */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.smb2.in.file.handle = h1;
	notify.smb2.in.recursive = true;

	req = smb2_notify_send(tree, &(notify.smb2));
	smb2_cancel(req);
	status = smb2_notify_recv(req, torture, &(notify.smb2));

	status = smb2_tdis(tree);
	CHECK_STATUS(status, NT_STATUS_OK);

	req = smb2_notify_send(tree, &(notify.smb2));

	smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(notify.smb2.out.num_changes, 0);

done:
	smb2_deltree(tree, BASEDIR_TD);
	return ret;
}

/*
  testing of change notifies followed by a tdis - no cancel
*/

#define BASEDIR_NTDIS BASEDIR "_NTDIS"

static bool torture_smb2_notify_tree_disconnect_1(
		struct torture_context *torture,
		struct smb2_tree *tree)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io;
	struct smb2_handle h1;
	struct smb2_request *req;

	smb2_deltree(tree, BASEDIR_NTDIS);
	smb2_util_rmdir(tree, BASEDIR_NTDIS);

	torture_comment(torture, "TESTING CHANGE NOTIFY ASYNC FOLLOWED BY "
			"TREE-DISCONNECT\n");

	/*
	  get a handle on the directory
	*/
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_NTDIS;

	status = smb2_create(tree, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify,
	   on file or directory name changes */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.smb2.in.file.handle = h1;
	notify.smb2.in.recursive = true;

	req = smb2_notify_send(tree, &(notify.smb2));
	WAIT_FOR_ASYNC_RESPONSE(req);

	status = smb2_tdis(tree);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_NOTIFY_CLEANUP);
	CHECK_VAL(notify.smb2.out.num_changes, 0);

done:
	smb2_deltree(tree, BASEDIR_NTDIS);
	return ret;
}

/*
  basic testing of change notifies followed by a close
*/

#define BASEDIR_CNC BASEDIR "_CNC"

static bool torture_smb2_notify_close(struct torture_context *torture,
				struct smb2_tree *tree1)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io;
	struct smb2_handle h1;
	struct smb2_request *req;

	smb2_deltree(tree1, BASEDIR_CNC);
	smb2_util_rmdir(tree1, BASEDIR_CNC);

	torture_comment(torture, "TESTING CHANGE NOTIFY FOLLOWED BY ULOGOFF\n");

	/*
	  get a handle on the directory
	*/
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_CNC;

	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	io.smb2.in.create_disposition = NTCREATEX_DISP_OPEN;
	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify,
	   on file or directory name changes */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.smb2.in.file.handle = h1;
	notify.smb2.in.recursive = true;

	req = smb2_notify_send(tree1, &(notify.smb2));

	WAIT_FOR_ASYNC_RESPONSE(req);

	status = smb2_util_close(tree1, h1);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_NOTIFY_CLEANUP);
	CHECK_VAL(notify.smb2.out.num_changes, 0);

done:
	smb2_deltree(tree1, BASEDIR_CNC);
	return ret;
}

/*
  basic testing of change notifies followed by a ulogoff
*/

#define BASEDIR_NUL BASEDIR "_NUL"
static bool torture_smb2_notify_ulogoff(struct torture_context *torture,
				struct smb2_tree *tree1)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io;
	struct smb2_handle h1;
	struct smb2_request *req;

	smb2_deltree(tree1, BASEDIR_NUL);
	smb2_util_rmdir(tree1, BASEDIR_NUL);

	torture_comment(torture, "TESTING CHANGE NOTIFY FOLLOWED BY ULOGOFF\n");

	/*
	  get a handle on the directory
	*/
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_NUL;

	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	io.smb2.in.create_disposition = NTCREATEX_DISP_OPEN;
	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify,
	   on file or directory name changes */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.smb2.in.file.handle = h1;
	notify.smb2.in.recursive = true;

	req = smb2_notify_send(tree1, &(notify.smb2));

	WAIT_FOR_ASYNC_RESPONSE(req);

	status = smb2_logoff(tree1->session);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_NOTIFY_CLEANUP);
	CHECK_VAL(notify.smb2.out.num_changes, 0);

done:
	smb2_deltree(tree1, BASEDIR_NUL);
	return ret;
}

/*
  basic testing of change notifies followed by a session reconnect
*/

#define BASEDIR_NSR BASEDIR "_NSR"

static bool torture_smb2_notify_session_reconnect(struct torture_context *torture,
				struct smb2_tree *tree1)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io;
	struct smb2_handle h1;
	struct smb2_request *req;
	uint64_t previous_session_id = 0;
	struct smb2_session *session2 = NULL;

	smb2_deltree(tree1, BASEDIR_NSR);
	smb2_util_rmdir(tree1, BASEDIR_NSR);

	torture_comment(torture, "TESTING CHANGE NOTIFY FOLLOWED BY SESSION RECONNECT\n");

	/*
	  get a handle on the directory
	*/
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_NSR;

	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	io.smb2.in.create_disposition = NTCREATEX_DISP_OPEN;
	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify,
	   on file or directory name changes */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.smb2.in.file.handle = h1;
	notify.smb2.in.recursive = true;

	req = smb2_notify_send(tree1, &(notify.smb2));

	WAIT_FOR_ASYNC_RESPONSE(req);

	previous_session_id = smb2cli_session_current_id(tree1->session->smbXcli);
	torture_assert(torture, torture_smb2_session_setup(torture,
		       tree1->session->transport,
		       previous_session_id,
		       torture, &session2),
		       "session setup with previous_session_id failed");

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_NOTIFY_CLEANUP);
	CHECK_VAL(notify.smb2.out.num_changes, 0);

	status = smb2_logoff(tree1->session);
	CHECK_STATUS(status, NT_STATUS_USER_SESSION_DELETED);

	status = smb2_logoff(session2);
	CHECK_STATUS(status, NT_STATUS_OK);
done:
	smb2_deltree(tree1, BASEDIR_NSR);
	return ret;
}

/*
  basic testing of change notifies followed by an invalid reauth
*/

#define BASEDIR_IR BASEDIR "_IR"

static bool torture_smb2_notify_invalid_reauth(struct torture_context *torture,
					       struct smb2_tree *tree1,
					       struct smb2_tree *tree2)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io;
	struct smb2_handle h1;
	struct smb2_request *req;
	struct cli_credentials *invalid_creds;

	smb2_deltree(tree2, BASEDIR_IR);
	smb2_util_rmdir(tree2, BASEDIR_IR);

	torture_comment(torture, "TESTING CHANGE NOTIFY FOLLOWED BY invalid REAUTH\n");

	/*
	  get a handle on the directory
	*/
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_IR;

	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	io.smb2.in.create_disposition = NTCREATEX_DISP_OPEN;
	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify,
	   on file or directory name changes */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.smb2.in.file.handle = h1;
	notify.smb2.in.recursive = true;

	req = smb2_notify_send(tree1, &(notify.smb2));

	WAIT_FOR_ASYNC_RESPONSE(req);

	invalid_creds = cli_credentials_init(torture);
	torture_assert(torture, (invalid_creds != NULL), "talloc error");
	cli_credentials_set_username(invalid_creds, "__none__invalid__none__", CRED_SPECIFIED);
	cli_credentials_set_domain(invalid_creds, "__none__invalid__none__", CRED_SPECIFIED);
	cli_credentials_set_password(invalid_creds, "__none__invalid__none__", CRED_SPECIFIED);
	cli_credentials_set_realm(invalid_creds, NULL, CRED_SPECIFIED);
	cli_credentials_set_workstation(invalid_creds, "", CRED_UNINITIALISED);

	status = smb2_session_setup_spnego(tree1->session,
					   invalid_creds,
					   0 /* previous_session_id */);
	CHECK_STATUS(status, NT_STATUS_LOGON_FAILURE);

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_NOTIFY_CLEANUP);
	CHECK_VAL(notify.smb2.out.num_changes, 0);

	/*
	 * Demonstrate that the session is no longer valid.
	 */
	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_USER_SESSION_DELETED);
done:
	smb2_deltree(tree2, BASEDIR_IR);
	return ret;
}

static void tcp_dis_handler(struct smb2_transport *t, void *p)
{
	struct smb2_tree *tree = (struct smb2_tree *)p;
	smb2_transport_dead(tree->session->transport,
			NT_STATUS_LOCAL_DISCONNECT);
	t = NULL;
	tree = NULL;
}

/*
  basic testing of change notifies followed by tcp disconnect
*/

#define BASEDIR_NTCPD BASEDIR "_NTCPD"

static bool torture_smb2_notify_tcp_disconnect(
		struct torture_context *torture,
		struct smb2_tree *tree)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io;
	struct smb2_handle h1;
	struct smb2_request *req;

	smb2_deltree(tree, BASEDIR_NTCPD);
	smb2_util_rmdir(tree, BASEDIR_NTCPD);

	torture_comment(torture,
		"TESTING CHANGE NOTIFY FOLLOWED BY TCP DISCONNECT\n");

	/*
	  get a handle on the directory
	*/
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_OPEN_IF;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_NTCPD;

	status = smb2_create(tree, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify,
	   on file or directory name changes */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.smb2.in.file.handle = h1;
	notify.smb2.in.recursive = true;

	req = smb2_notify_send(tree, &(notify.smb2));
	smb2_cancel(req);
	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_CANCELLED);

	notify.smb2.in.recursive = true;
	req = smb2_notify_send(tree, &(notify.smb2));
	smb2_transport_idle_handler(tree->session->transport,
				tcp_dis_handler, 250000, tree);
	tree = NULL;
	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_LOCAL_DISCONNECT);

done:
	return ret;
}

/*
   test setting up two change notify requests on one handle
*/

#define BASEDIR_NDOH BASEDIR "_NDOH"

static bool torture_smb2_notify_double(struct torture_context *torture,
			struct smb2_tree *tree1,
			struct smb2_tree *tree2)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io;
	struct smb2_handle h1;
	struct smb2_request *req1, *req2;

	smb2_deltree(tree1, BASEDIR_NDOH);
	smb2_util_rmdir(tree1, BASEDIR_NDOH);

	torture_comment(torture,
		"TESTING CHANGE NOTIFY TWICE ON ONE DIRECTORY\n");

	/*
	  get a handle on the directory
	*/
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_RIGHTS_FILE_READ|
				SEC_RIGHTS_FILE_WRITE|
				SEC_RIGHTS_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_NDOH;

	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify,
	   on file or directory name changes */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.smb2.in.file.handle = h1;
	notify.smb2.in.recursive = true;

	req1 = smb2_notify_send(tree1, &(notify.smb2));
	smb2_cancel(req1);
	status = smb2_notify_recv(req1, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_CANCELLED);

	req2 = smb2_notify_send(tree1, &(notify.smb2));
	smb2_cancel(req2);
	status = smb2_notify_recv(req2, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_CANCELLED);

	smb2_util_mkdir(tree2, BASEDIR_NDOH "\\subdir-name");
	req1 = smb2_notify_send(tree1, &(notify.smb2));
	req2 = smb2_notify_send(tree1, &(notify.smb2));

	status = smb2_notify_recv(req1, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subdir-name");

	smb2_util_mkdir(tree2, BASEDIR_NDOH "\\subdir-name2");

	status = smb2_notify_recv(req2, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subdir-name2");

done:
	smb2_deltree(tree1, BASEDIR_NDOH);
	return ret;
}


/*
   test multiple change notifies at different depths and with/without recursion
*/

#define BASEDIR_TREE BASEDIR "_TREE"

static bool torture_smb2_notify_tree(struct torture_context *torture,
			     struct smb2_tree *tree)
{
	bool ret = true;
	union smb_notify notify;
	union smb_open io;
	struct smb2_request *req;
	struct timeval tv;
	struct {
		const char *path;
		bool recursive;
		uint32_t filter;
		int expected;
		struct smb2_handle h1;
		int counted;
	} dirs[] = {
		{
			.path      = BASEDIR_TREE "\\abc",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 30,
		},
		{
			.path      = BASEDIR_TREE "\\zqy",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 8,
		},
		{
			.path      = BASEDIR_TREE "\\atsy",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 4,
		},
		{
			.path      = BASEDIR_TREE "\\abc\\foo",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 2,
		},
		{
			.path      = BASEDIR_TREE "\\abc\\blah",
			.recursive = true,
			.filter    =  FILE_NOTIFY_CHANGE_NAME,
			.expected  = 13,
		},
		{
			.path      = BASEDIR_TREE "\\abc\\blah",
			.recursive = false,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 7,
		},
		{
			.path      = BASEDIR_TREE "\\abc\\blah\\a",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 2,
		},
		{
			.path      = BASEDIR_TREE "\\abc\\blah\\b",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 2,
		},
		{
			.path      = BASEDIR_TREE "\\abc\\blah\\c",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 2,
		},
		{
			.path      = BASEDIR_TREE "\\abc\\fooblah",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 2,
		},
		{
			.path      = BASEDIR_TREE "\\zqy\\xx",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 2,
		},
		{
			.path      = BASEDIR_TREE "\\zqy\\yyy",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 2,
		},
		{
			.path      = BASEDIR_TREE "\\zqy\\..",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 40,
		},
		{
			.path      = BASEDIR_TREE,
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 40,
		},
		{
			.path      = BASEDIR_TREE,
			.recursive = false,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 6,
		},
		{
			.path      = BASEDIR_TREE "\\atsy",
			.recursive = false,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 4,
		},
		{
			.path      = BASEDIR_TREE "\\abc",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 24,
		},
		{
			.path      = BASEDIR_TREE "\\abc",
			.recursive = false,
			.filter    = FILE_NOTIFY_CHANGE_FILE_NAME,
			.expected  = 0,
		},
		{
			.path      = BASEDIR_TREE "\\abc",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_FILE_NAME,
			.expected  = 0,
		},
		{
			.path      = BASEDIR_TREE "\\abc",
			.recursive = true,
			.filter    = FILE_NOTIFY_CHANGE_NAME,
			.expected  = 24,
		},
	};
	int i;
	NTSTATUS status;
	bool all_done = false;

	smb2_deltree(tree, BASEDIR_TREE);
	smb2_util_rmdir(tree, BASEDIR_TREE);

	torture_comment(torture, "TESTING NOTIFY FOR DIFFERENT DEPTHS\n");

	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_OPEN_IF;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_TREE;
	status = smb2_create(tree, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 20000;

	/*
	  setup the directory tree, and the notify buffer on each directory
	*/
	for (i=0;i<ARRAY_SIZE(dirs);i++) {
		io.smb2.in.fname = dirs[i].path;
		status = smb2_create(tree, torture, &(io.smb2));
		CHECK_STATUS(status, NT_STATUS_OK);
		dirs[i].h1 = io.smb2.out.file.handle;

		notify.smb2.in.completion_filter = dirs[i].filter;
		notify.smb2.in.file.handle = dirs[i].h1;
		notify.smb2.in.recursive = dirs[i].recursive;
		req = smb2_notify_send(tree, &(notify.smb2));
		smb2_cancel(req);
		status = smb2_notify_recv(req, torture, &(notify.smb2));
		CHECK_STATUS(status, NT_STATUS_CANCELLED);
	}

	/* trigger 2 events in each dir */
	for (i=0;i<ARRAY_SIZE(dirs);i++) {
		char *path = talloc_asprintf(torture, "%s\\test.dir",
					     dirs[i].path);
		smb2_util_mkdir(tree, path);
		smb2_util_rmdir(tree, path);
		talloc_free(path);
	}

	/* give a bit of time for the events to propagate */
	tv = timeval_current();

	do {
		/* count events that have happened in each dir */
		for (i=0;i<ARRAY_SIZE(dirs);i++) {
			notify.smb2.in.completion_filter = dirs[i].filter;
			notify.smb2.in.file.handle = dirs[i].h1;
			notify.smb2.in.recursive = dirs[i].recursive;
			req = smb2_notify_send(tree, &(notify.smb2));
			smb2_cancel(req);
			notify.smb2.out.num_changes = 0;
			status = smb2_notify_recv(req, torture,
				 &(notify.smb2));
			dirs[i].counted += notify.smb2.out.num_changes;
		}

		all_done = true;

		for (i=0;i<ARRAY_SIZE(dirs);i++) {
			if (dirs[i].counted != dirs[i].expected) {
				all_done = false;
			}
		}
	} while (!all_done && timeval_elapsed(&tv) < 20);

	torture_comment(torture, "took %.4f seconds to propagate all events\n",
			timeval_elapsed(&tv));

	for (i=0;i<ARRAY_SIZE(dirs);i++) {
		if (dirs[i].counted != dirs[i].expected) {
			torture_comment(torture,
				"ERROR: i=%d expected %d got %d for '%s'\n",
				i, dirs[i].expected, dirs[i].counted,
				dirs[i].path);
			ret = false;
		}
	}

	/*
	  run from the back, closing and deleting
	*/
	for (i=ARRAY_SIZE(dirs)-1;i>=0;i--) {
		smb2_util_close(tree, dirs[i].h1);
		smb2_util_rmdir(tree, dirs[i].path);
	}

done:
	smb2_deltree(tree, BASEDIR_TREE);
	smb2_util_rmdir(tree, BASEDIR_TREE);
	return ret;
}

/*
   Test response when cached server events exceed single NT NOTFIY response
   packet size.
*/

#define BASEDIR_OVF BASEDIR "_OVF"

static bool torture_smb2_notify_overflow(struct torture_context *torture,
				struct smb2_tree *tree)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io;
	struct smb2_handle h1, h2;
	int count = 100;
	struct smb2_request *req1;
	int i;

	smb2_deltree(tree, BASEDIR_OVF);
	smb2_util_rmdir(tree, BASEDIR_OVF);

	torture_comment(torture, "TESTING CHANGE NOTIFY EVENT OVERFLOW\n");

	/* get a handle on the directory */
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
			    NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_OVF;

	status = smb2_create(tree, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify, on name changes. */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_NTTRANS;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.smb2.in.file.handle = h1;

	notify.smb2.in.recursive = true;
	req1 = smb2_notify_send(tree, &(notify.smb2));

	/* cancel initial requests so the buffer is setup */
	smb2_cancel(req1);
	status = smb2_notify_recv(req1, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_CANCELLED);

	/* open a lot of files, filling up the server side notify buffer */
	torture_comment(torture,
		"Testing overflowed buffer notify on create of %d files\n",
		count);

	for (i=0;i<count;i++) {
		char *fname = talloc_asprintf(torture,
			      BASEDIR_OVF "\\test%d.txt", i);
		union smb_open io1;
		ZERO_STRUCT(io1.smb2);
	        io1.generic.level = RAW_OPEN_SMB2;
		io1.smb2.in.create_flags = 0;
	        io1.smb2.in.desired_access = SEC_FILE_ALL;
		io1.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
		io1.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	        io1.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				    NTCREATEX_SHARE_ACCESS_WRITE;
	        io1.smb2.in.alloc_size = 0;
	        io1.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	        io1.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	        io1.smb2.in.security_flags = 0;
		io1.smb2.in.fname = fname;

		h2 = custom_smb2_create(tree, torture, &(io1.smb2));
		talloc_free(fname);
		smb2_util_close(tree, h2);
	}

	req1 = smb2_notify_send(tree, &(notify.smb2));
	status = smb2_notify_recv(req1, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_NOTIFY_ENUM_DIR);
	CHECK_VAL(notify.smb2.out.num_changes, 0);

done:
	smb2_deltree(tree, BASEDIR_OVF);
	return ret;
}

/*
   Test if notifications are returned for changes to the base directory.
   They shouldn't be.
*/

#define BASEDIR_BAS BASEDIR "_BAS"

static bool torture_smb2_notify_basedir(struct torture_context *torture,
				struct smb2_tree *tree1,
				struct smb2_tree *tree2)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io;
	struct smb2_handle h1;
	struct smb2_request *req1;

	smb2_deltree(tree1, BASEDIR_BAS);
	smb2_util_rmdir(tree1, BASEDIR_BAS);

	torture_comment(torture, "TESTING CHANGE NOTIFY BASEDIR EVENTS\n");

	/* get a handle on the directory */
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
	    NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_OPEN_IF;
	io.smb2.in.impersonation_level = NTCREATEX_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_BAS;

	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* create a test file that will also be modified */
	io.smb2.in.fname = BASEDIR_BAS "\\tname1";
	io.smb2.in.create_options = NTCREATEX_OPTIONS_NON_DIRECTORY_FILE;
	status =  smb2_create(tree2, torture, &(io.smb2));
	CHECK_STATUS(status,NT_STATUS_OK);
	smb2_util_close(tree2, io.smb2.out.file.handle);

	/* ask for a change notify, on attribute changes. */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_ATTRIBUTES;
	notify.smb2.in.file.handle = h1;
	notify.smb2.in.recursive = true;

	req1 = smb2_notify_send(tree1, &(notify.smb2));

	/* set attribute on the base dir */
	smb2_util_setatr(tree2, BASEDIR_BAS, FILE_ATTRIBUTE_HIDDEN);

	/* set attribute on a file to assure we receive a notification */
	smb2_util_setatr(tree2, BASEDIR_BAS "\\tname1", FILE_ATTRIBUTE_HIDDEN);
	smb_msleep(200);

	/* check how many responses were given, expect only 1 for the file */
	status = smb2_notify_recv(req1, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_MODIFIED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "tname1");

done:
	smb2_deltree(tree1, BASEDIR_BAS);
	return ret;
}

/*
   very simple change notify test
*/

#define BASEDIR_TCON BASEDIR "_TCON"

static bool torture_smb2_notify_tcon(struct torture_context *torture,
				  struct smb2_tree *tree)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io;
	struct smb2_handle h1 = {{0}};
	struct smb2_request *req = NULL;
	struct smb2_tree *tree1 = NULL;
	const char *fname = BASEDIR_TCON "\\subdir-name";

	smb2_deltree(tree, BASEDIR_TCON);
	smb2_util_rmdir(tree, BASEDIR_TCON);

	torture_comment(torture, "TESTING SIMPLE CHANGE NOTIFY\n");

	/*
	  get a handle on the directory
	*/

	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_RIGHTS_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL |
				FILE_ATTRIBUTE_DIRECTORY;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_OPEN_IF;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_TCON;

	status = smb2_create(tree, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify,
	   on file or directory name changes */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.smb2.in.file.handle = h1;
	notify.smb2.in.recursive = true;

	torture_comment(torture, "Testing notify mkdir\n");
	req = smb2_notify_send(tree, &(notify.smb2));
	smb2_cancel(req);
	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_CANCELLED);

	notify.smb2.in.recursive = true;
	req = smb2_notify_send(tree, &(notify.smb2));
	status = smb2_util_mkdir(tree, fname);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_ADDED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subdir-name");

	torture_comment(torture, "Testing notify rmdir\n");
	req = smb2_notify_send(tree, &(notify.smb2));
	status = smb2_util_rmdir(tree, fname);
	CHECK_STATUS(status, NT_STATUS_OK);

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_REMOVED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subdir-name");

	torture_comment(torture, "SIMPLE CHANGE NOTIFY OK\n");

	torture_comment(torture, "TESTING WITH SECONDARY TCON\n");
	if (!torture_smb2_tree_connect(torture, tree->session, tree, &tree1)) {
		torture_warning(torture, "couldn't reconnect to share, bailing\n");
		ret = false;
		goto done;
	}

	torture_comment(torture, "tid1=%d tid2=%d\n",
			smb2cli_tcon_current_id(tree->smbXcli),
			smb2cli_tcon_current_id(tree1->smbXcli));

	torture_comment(torture, "Testing notify mkdir\n");
	req = smb2_notify_send(tree, &(notify.smb2));
	smb2_util_mkdir(tree1, fname);

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_ADDED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subdir-name");

	torture_comment(torture, "Testing notify rmdir\n");
	req = smb2_notify_send(tree, &(notify.smb2));
	smb2_util_rmdir(tree, fname);

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_REMOVED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subdir-name");

	torture_comment(torture, "CHANGE NOTIFY WITH TCON OK\n");

	torture_comment(torture, "Disconnecting secondary tree\n");
	status = smb2_tdis(tree1);
	CHECK_STATUS(status, NT_STATUS_OK);
	talloc_free(tree1);

	torture_comment(torture, "Testing notify mkdir\n");
	req = smb2_notify_send(tree, &(notify.smb2));
	smb2_util_mkdir(tree, fname);

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);

	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_ADDED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subdir-name");

	torture_comment(torture, "Testing notify rmdir\n");
	req = smb2_notify_send(tree, &(notify.smb2));
	smb2_util_rmdir(tree, fname);

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	CHECK_VAL(notify.smb2.out.num_changes, 1);
	CHECK_VAL(notify.smb2.out.changes[0].action, NOTIFY_ACTION_REMOVED);
	CHECK_WIRE_STR(notify.smb2.out.changes[0].name, "subdir-name");

	torture_comment(torture, "CHANGE NOTIFY WITH TDIS OK\n");
done:
	smb2_util_close(tree, h1);
	smb2_deltree(tree, BASEDIR_TCON);

	return ret;
}

#define BASEDIR_RMD BASEDIR "_RMD"

static bool torture_smb2_notify_rmdir(struct torture_context *torture,
				      struct smb2_tree *tree1,
				      struct smb2_tree *tree2,
				      bool initial_delete_on_close)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify = {};
	union smb_setfileinfo sfinfo = {};
	union smb_open io = {};
	struct smb2_handle h = {};
	struct smb2_request *req;

	torture_comment(torture, "TESTING NOTIFY CANCEL FOR DELETED DIR\n");

	smb2_deltree(tree1, BASEDIR_RMD);
	smb2_util_rmdir(tree1, BASEDIR_RMD);

	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_ALL;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access =
		NTCREATEX_SHARE_ACCESS_READ |
		NTCREATEX_SHARE_ACCESS_WRITE |
		NTCREATEX_SHARE_ACCESS_DELETE ;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_RMD;

	status = smb2_create(tree1, torture, &(io.smb2));
	CHECK_STATUS(status, NT_STATUS_OK);
	h = io.smb2.out.file.handle;

	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.smb2.in.file.handle = h;
	notify.smb2.in.recursive = false;

	io.smb2.in.desired_access |= SEC_STD_DELETE;
	io.smb2.in.create_disposition = NTCREATEX_DISP_OPEN;
	req = smb2_notify_send(tree1, &(notify.smb2));

	if (initial_delete_on_close) {
		status = smb2_util_rmdir(tree2, BASEDIR_RMD);
		CHECK_STATUS(status, NT_STATUS_OK);
	} else {
		status = smb2_create(tree2, torture, &(io.smb2));
		CHECK_STATUS(status, NT_STATUS_OK);

		sfinfo.generic.level = RAW_SFILEINFO_DISPOSITION_INFORMATION;
		sfinfo.generic.in.file.handle = io.smb2.out.file.handle;
		sfinfo.disposition_info.in.delete_on_close = 1;
		status = smb2_setinfo_file(tree2, &sfinfo);
		CHECK_STATUS(status, NT_STATUS_OK);

		smb2_util_close(tree2, io.smb2.out.file.handle);
	}

	status = smb2_notify_recv(req, torture, &(notify.smb2));
	CHECK_STATUS(status, NT_STATUS_DELETE_PENDING);

done:

	smb2_util_close(tree1, h);
	smb2_deltree(tree1, BASEDIR_RMD);

	return ret;
}

static bool torture_smb2_notify_rmdir1(struct torture_context *torture,
				       struct smb2_tree *tree)
{
	return torture_smb2_notify_rmdir(torture, tree, tree, false);
}

static bool torture_smb2_notify_rmdir2(struct torture_context *torture,
				       struct smb2_tree *tree)
{
	return torture_smb2_notify_rmdir(torture, tree, tree, true);
}

static bool torture_smb2_notify_rmdir3(struct torture_context *torture,
				       struct smb2_tree *tree1,
				       struct smb2_tree *tree2)
{
	return torture_smb2_notify_rmdir(torture, tree1, tree2, false);
}

static bool torture_smb2_notify_rmdir4(struct torture_context *torture,
				       struct smb2_tree *tree1,
				       struct smb2_tree *tree2)
{
	return torture_smb2_notify_rmdir(torture, tree1, tree2, true);
}

static void notify_timeout(struct tevent_context *ev,
			   struct tevent_timer *te,
			   struct timeval current_time,
			   void *private_data)
{
	struct smb2_request *req = talloc_get_type_abort(
		private_data, struct smb2_request);

	smb2_cancel(req);
}

#define BASEDIR_INR BASEDIR "_INR"

static bool torture_smb2_inotify_rename(struct torture_context *torture,
					struct smb2_tree *tree1,
					struct smb2_tree *tree2)
{
	NTSTATUS status;
	struct smb2_notify notify;
	struct notify_changes change1 = {0};
	struct notify_changes change2 = {0};
	struct smb2_create create;
	union smb_setfileinfo sinfo;
	struct smb2_handle h1 = {{0}};
	struct smb2_handle h2 = {{0}};
	struct smb2_request *req;
	struct tevent_timer *te = NULL;
	bool ok = false;

	smb2_deltree(tree1, BASEDIR_INR);

	torture_comment(torture, "Testing change notify of a rename with inotify\n");

	status = torture_smb2_testdir_access(
		tree1,
		BASEDIR_INR,
		&h1,
		SEC_RIGHTS_DIR_ALL & ~SEC_STD_DELETE);
	torture_assert_ntstatus_ok_goto(torture, status, ok, done, "torture_smb2_testdir failed");

	ZERO_STRUCT(create);
	create.in.desired_access = SEC_RIGHTS_FILE_READ |
		SEC_RIGHTS_FILE_WRITE|
		SEC_RIGHTS_FILE_ALL;
	create.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	create.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	create.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
		NTCREATEX_SHARE_ACCESS_WRITE |
		NTCREATEX_SHARE_ACCESS_DELETE;
	create.in.create_disposition = NTCREATEX_DISP_OPEN_IF;
	create.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	create.in.fname = BASEDIR_INR "\\subdir-name";

	status = smb2_create(tree2, torture, &create);
	torture_assert_ntstatus_ok_goto(torture, status, ok, done, "smb2_create failed\n");
	h2 = create.out.file.handle;

	ZERO_STRUCT(notify);
	notify.level = RAW_NOTIFY_SMB2;
	notify.in.buffer_size = 4096;
	notify.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.in.file.handle = h1;
	notify.in.recursive = true;
	req = smb2_notify_send(tree1, &notify);
	torture_assert_not_null_goto(torture, req, ok, done, "smb2_notify_send failed\n");

	while (!NT_STATUS_EQUAL(req->status, NT_STATUS_PENDING)) {
		if (tevent_loop_once(torture->ev) != 0) {
			goto done;
		}
	}

	ZERO_STRUCT(sinfo);
	sinfo.rename_information.level = RAW_SFILEINFO_RENAME_INFORMATION;
	sinfo.rename_information.in.file.handle = h2;
	sinfo.rename_information.in.new_name = BASEDIR_INR "\\subdir-name-r";

	status = smb2_setinfo_file(tree2, &sinfo);
	torture_assert_ntstatus_ok_goto(torture, status, ok, done, "smb2_setinfo_file failed\n");

	smb2_util_close(tree2, h2);

	te = tevent_add_timer(torture->ev,
			      tree1,
			      tevent_timeval_current_ofs(1, 0),
			      notify_timeout,
			      req);
	torture_assert_not_null_goto(torture, te, ok, done, "tevent_add_timer failed\n");

	status = smb2_notify_recv(req, torture, &notify);
	torture_assert_ntstatus_ok_goto(torture, status, ok, done, "smb2_notify_recv failed\n");

	torture_assert_goto(torture, notify.out.num_changes == 1 || notify.out.num_changes == 2,
			    ok, done, "bad notify\n");

	change1 = notify.out.changes[0];
	if (notify.out.num_changes == 2) {
		change2 = notify.out.changes[1];
	} else {
		/*
		 * We may only get one event at a time, so check for the
		 * matching second event for the oldname/newname or
		 * removed/added pair.
		 */
		ZERO_STRUCT(notify);
		notify.level = RAW_NOTIFY_SMB2;
		notify.in.buffer_size = 4096;
		notify.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
		notify.in.file.handle = h1;
		notify.in.recursive = true;
		req = smb2_notify_send(tree1, &notify);
		torture_assert_not_null_goto(torture, req, ok, done, "smb2_notify_send failed\n");

		status = smb2_notify_recv(req, torture, &notify);
		torture_assert_ntstatus_ok_goto(torture, status, ok, done, "smb2_notify_recv failed\n");

		torture_assert_goto(torture, notify.out.num_changes == 1, ok, done,
				    "bad notify\n");

		change2 = notify.out.changes[0];
	}

	if ((change1.action != NOTIFY_ACTION_OLD_NAME) &&
	    (change1.action != NOTIFY_ACTION_REMOVED))
	{
		torture_fail_goto(torture, done, "bad change notification\n");
	}
	torture_assert_str_equal_goto(torture, change1.name.s, "subdir-name",
			    ok, done, "bad change notification\n");

	if ((change2.action != NOTIFY_ACTION_NEW_NAME) &&
	    (change2.action != NOTIFY_ACTION_ADDED))
	{
		torture_fail_goto(torture, done, "bad change notification\n");
	}
	torture_assert_str_equal_goto(torture, change2.name.s, "subdir-name-r",
			    ok, done, "bad change notification\n");

	ok = true;
done:
	if (!smb2_util_handle_empty(h1)) {
		smb2_util_close(tree1, h1);
	}
	if (!smb2_util_handle_empty(h2)) {
		smb2_util_close(tree2, h2);
	}

	smb2_deltree(tree1, BASEDIR_INR);
	return ok;
}

/*
  Test asking for a change notify on a handle without permissions.
*/

#define BASEDIR_HPERM BASEDIR "_HPERM"

static bool torture_smb2_notify_handle_permissions(
		struct torture_context *torture,
		struct smb2_tree *tree)
{
	bool ret = true;
	NTSTATUS status;
	union smb_notify notify;
	union smb_open io;
	struct smb2_handle h1 = {{0}};
	struct smb2_request *req;

	smb2_deltree(tree, BASEDIR_HPERM);
	smb2_util_rmdir(tree, BASEDIR_HPERM);

	torture_comment(torture,
		"TESTING CHANGE NOTIFY "
		"ON A HANDLE WITHOUT PERMISSIONS\n");

	/*
	  get a handle on the directory
	*/
	ZERO_STRUCT(io.smb2);
	io.generic.level = RAW_OPEN_SMB2;
	io.smb2.in.create_flags = 0;
	io.smb2.in.desired_access = SEC_FILE_READ_ATTRIBUTE;
	io.smb2.in.create_options = NTCREATEX_OPTIONS_DIRECTORY;
	io.smb2.in.file_attributes = FILE_ATTRIBUTE_NORMAL;
	io.smb2.in.share_access = NTCREATEX_SHARE_ACCESS_READ |
				NTCREATEX_SHARE_ACCESS_WRITE;
	io.smb2.in.alloc_size = 0;
	io.smb2.in.create_disposition = NTCREATEX_DISP_CREATE;
	io.smb2.in.impersonation_level = SMB2_IMPERSONATION_ANONYMOUS;
	io.smb2.in.security_flags = 0;
	io.smb2.in.fname = BASEDIR_HPERM;

	status = smb2_create(tree, torture, &io.smb2);
	CHECK_STATUS(status, NT_STATUS_OK);
	h1 = io.smb2.out.file.handle;

	/* ask for a change notify,
	   on file or directory name changes */
	ZERO_STRUCT(notify.smb2);
	notify.smb2.level = RAW_NOTIFY_SMB2;
	notify.smb2.in.buffer_size = 1000;
	notify.smb2.in.completion_filter = FILE_NOTIFY_CHANGE_NAME;
	notify.smb2.in.file.handle = h1;
	notify.smb2.in.recursive = true;

	req = smb2_notify_send(tree, &notify.smb2);
	torture_assert_goto(torture,
			req != NULL,
			ret,
			done,
			"smb2_notify_send failed\n");

	/*
	 * Cancel it, we don't really want to wait.
	 */
	smb2_cancel(req);
	status = smb2_notify_recv(req, torture, &notify.smb2);
	/* Handle h1 doesn't have permissions for ChangeNotify. */
	CHECK_STATUS(status, NT_STATUS_ACCESS_DENIED);

done:
	if (!smb2_util_handle_empty(h1)) {
		smb2_util_close(tree, h1);
	}
	smb2_deltree(tree, BASEDIR_HPERM);
	return ret;
}

/*
   basic testing of SMB2 change notify
*/
struct torture_suite *torture_smb2_notify_init(TALLOC_CTX *ctx)
{
	struct torture_suite *suite = torture_suite_create(ctx, "notify");

	torture_suite_add_1smb2_test(suite, "valid-req", test_valid_request);
	torture_suite_add_1smb2_test(suite, "tcon", torture_smb2_notify_tcon);
	torture_suite_add_2smb2_test(suite, "dir", torture_smb2_notify_dir);
	torture_suite_add_2smb2_test(suite, "mask", torture_smb2_notify_mask);
	torture_suite_add_1smb2_test(suite, "tdis", torture_smb2_notify_tree_disconnect);
	torture_suite_add_1smb2_test(suite, "tdis1", torture_smb2_notify_tree_disconnect_1);
	torture_suite_add_2smb2_test(suite, "mask-change", torture_smb2_notify_mask_change);
	torture_suite_add_1smb2_test(suite, "close", torture_smb2_notify_close);
	torture_suite_add_1smb2_test(suite, "logoff", torture_smb2_notify_ulogoff);
	torture_suite_add_1smb2_test(suite, "session-reconnect", torture_smb2_notify_session_reconnect);
	torture_suite_add_2smb2_test(suite, "invalid-reauth", torture_smb2_notify_invalid_reauth);
	torture_suite_add_1smb2_test(suite, "tree", torture_smb2_notify_tree);
	torture_suite_add_2smb2_test(suite, "basedir", torture_smb2_notify_basedir);
	torture_suite_add_2smb2_test(suite, "double", torture_smb2_notify_double);
	torture_suite_add_1smb2_test(suite, "file", torture_smb2_notify_file);
	torture_suite_add_1smb2_test(suite, "tcp", torture_smb2_notify_tcp_disconnect);
	torture_suite_add_2smb2_test(suite, "rec", torture_smb2_notify_recursive);
	torture_suite_add_1smb2_test(suite, "overflow", torture_smb2_notify_overflow);
	torture_suite_add_1smb2_test(suite, "rmdir1",
				     torture_smb2_notify_rmdir1);
	torture_suite_add_1smb2_test(suite, "rmdir2",
				     torture_smb2_notify_rmdir2);
	torture_suite_add_2smb2_test(suite, "rmdir3",
				     torture_smb2_notify_rmdir3);
	torture_suite_add_2smb2_test(suite, "rmdir4",
				     torture_smb2_notify_rmdir4);
	torture_suite_add_1smb2_test(suite,
				    "handle-permissions",
				    torture_smb2_notify_handle_permissions);

	suite->description = talloc_strdup(suite, "SMB2-NOTIFY tests");

	return suite;
}

/*
   basic testing of SMB2 change notify
*/
struct torture_suite *torture_smb2_notify_inotify_init(TALLOC_CTX *ctx)
{
	struct torture_suite *suite = torture_suite_create(ctx, "notify-inotify");

	suite->description = talloc_strdup(suite, "SMB2-NOTIFY tests that use inotify");

	torture_suite_add_2smb2_test(suite, "inotify-rename", torture_smb2_inotify_rename);

	return suite;
}
