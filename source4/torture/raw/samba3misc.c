/*
   Unix SMB/CIFS implementation.
   Test some misc Samba3 code paths
   Copyright (C) Volker Lendecke 2006

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
#include "torture/torture.h"
#include "libcli/raw/libcliraw.h"
#include "libcli/raw/raw_proto.h"
#include "system/time.h"
#include "system/filesys.h"
#include "libcli/libcli.h"
#include "torture/util.h"
#include "lib/events/events.h"
#include "param/param.h"
#include "torture/raw/proto.h"

/*
 The next 2 functions are stolen from source4/libcli/raw/rawfile.c
 but allow us to send a raw data blob instead of an OpenX name.
*/

#define SETUP_REQUEST(cmd, wct, buflen) do { \
        req = smbcli_request_setup(tree, cmd, wct, buflen); \
        if (!req) return NULL; \
} while (0)

static struct smbcli_request *smb_raw_openX_name_blob_send(struct smbcli_tree *tree,
					union smb_open *parms,
					const DATA_BLOB *pname_blob)
{
        struct smbcli_request *req = NULL;

	if (parms->generic.level != RAW_OPEN_OPENX) {
		return NULL;
	}

	SETUP_REQUEST(SMBopenX, 15, 0);
	SSVAL(req->out.vwv, VWV(0), SMB_CHAIN_NONE);
	SSVAL(req->out.vwv, VWV(1), 0);
	SSVAL(req->out.vwv, VWV(2), parms->openx.in.flags);
	SSVAL(req->out.vwv, VWV(3), parms->openx.in.open_mode);
	SSVAL(req->out.vwv, VWV(4), parms->openx.in.search_attrs);
	SSVAL(req->out.vwv, VWV(5), parms->openx.in.file_attrs);
	raw_push_dos_date3(tree->session->transport,
			req->out.vwv, VWV(6), parms->openx.in.write_time);
	SSVAL(req->out.vwv, VWV(8), parms->openx.in.open_func);
	SIVAL(req->out.vwv, VWV(9), parms->openx.in.size);
	SIVAL(req->out.vwv, VWV(11),parms->openx.in.timeout);
	SIVAL(req->out.vwv, VWV(13),0); /* reserved */
	smbcli_req_append_blob(req, pname_blob);

	if (!smbcli_request_send(req)) {
		smbcli_request_destroy(req);
		return NULL;
	}

	return req;
}

static NTSTATUS smb_raw_openX_name_blob(struct smbcli_tree *tree,
			TALLOC_CTX *mem_ctx,
			union smb_open *parms,
			const DATA_BLOB *pname_blob)
{
	struct smbcli_request *req = smb_raw_openX_name_blob_send(tree, parms, pname_blob);
	return smb_raw_open_recv(req, mem_ctx, parms);
}

static NTSTATUS raw_smbcli_openX_name_blob(struct smbcli_tree *tree,
				const DATA_BLOB *pname_blob,
				int flags,
				int share_mode,
				int *fnum)
{
        union smb_open open_parms;
        unsigned int openfn=0;
        unsigned int accessmode=0;
        TALLOC_CTX *mem_ctx;
        NTSTATUS status;

        mem_ctx = talloc_init("raw_openX_name_blob");
        if (!mem_ctx) return NT_STATUS_NO_MEMORY;

        if (flags & O_CREAT) {
                openfn |= OPENX_OPEN_FUNC_CREATE;
        }
        if (!(flags & O_EXCL)) {
                if (flags & O_TRUNC) {
                        openfn |= OPENX_OPEN_FUNC_TRUNC;
                } else {
                        openfn |= OPENX_OPEN_FUNC_OPEN;
                }
        }

        accessmode = (share_mode<<OPENX_MODE_DENY_SHIFT);

        if ((flags & O_ACCMODE) == O_RDWR) {
                accessmode |= OPENX_MODE_ACCESS_RDWR;
        } else if ((flags & O_ACCMODE) == O_WRONLY) {
                accessmode |= OPENX_MODE_ACCESS_WRITE;
        } else if ((flags & O_ACCMODE) == O_RDONLY) {
                accessmode |= OPENX_MODE_ACCESS_READ;
	}

#if defined(O_SYNC)
        if ((flags & O_SYNC) == O_SYNC) {
                accessmode |= OPENX_MODE_WRITE_THRU;
        }
#endif

        if (share_mode == DENY_FCB) {
                accessmode = OPENX_MODE_ACCESS_FCB | OPENX_MODE_DENY_FCB;
        }

        open_parms.openx.level = RAW_OPEN_OPENX;
        open_parms.openx.in.flags = 0;
        open_parms.openx.in.open_mode = accessmode;
        open_parms.openx.in.search_attrs = FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN;
        open_parms.openx.in.file_attrs = 0;
        open_parms.openx.in.write_time = 0;
        open_parms.openx.in.open_func = openfn;
        open_parms.openx.in.size = 0;
        open_parms.openx.in.timeout = 0;
        open_parms.openx.in.fname = NULL;

        status = smb_raw_openX_name_blob(tree, mem_ctx, &open_parms, pname_blob);
        talloc_free(mem_ctx);

        if (fnum && NT_STATUS_IS_OK(status)) {
                *fnum = open_parms.openx.out.file.fnum;
        }

        return status;
}


#define CHECK_STATUS(torture, status, correct) do {	\
	if (!NT_STATUS_EQUAL(status, correct)) { \
		torture_result(torture, TORTURE_FAIL, "%s: Incorrect status %s - should be %s\n", \
		       __location__, nt_errstr(status), nt_errstr(correct)); \
		ret = false; \
	} \
} while (0)

bool torture_samba3_checkfsp(struct torture_context *torture, struct smbcli_state *cli)
{
	const char *fname = "test.txt";
	const char *dirname = "testdir";
	int fnum;
	NTSTATUS status;
	bool ret = true;
	TALLOC_CTX *mem_ctx;
	ssize_t nread;
	char buf[16];
	struct smbcli_tree *tree2;

	torture_assert(torture, mem_ctx = talloc_init("torture_samba3_checkfsp"), "talloc_init failed\n");

	torture_assert_ntstatus_equal(torture, torture_second_tcon(torture, cli->session,
								   torture_setting_string(torture, "share", NULL),
								   &tree2),
				      NT_STATUS_OK,
				      "creating second tcon");

	/* Try a read on an invalid FID */

	nread = smbcli_read(cli->tree, 4711, buf, 0, sizeof(buf));
	CHECK_STATUS(torture, smbcli_nt_error(cli->tree), NT_STATUS_INVALID_HANDLE);

	/* Try a read on a directory handle */

	torture_assert(torture, torture_setup_dir(cli, dirname), "creating test directory");

	/* Open the directory */
	{
		union smb_open io;
		io.generic.level = RAW_OPEN_NTCREATEX;
		io.ntcreatex.in.flags = NTCREATEX_FLAGS_EXTENDED;
		io.ntcreatex.in.root_fid.fnum = 0;
		io.ntcreatex.in.security_flags = 0;
		io.ntcreatex.in.open_disposition = NTCREATEX_DISP_CREATE;
		io.ntcreatex.in.access_mask = SEC_RIGHTS_FILE_ALL;
		io.ntcreatex.in.alloc_size = 0;
		io.ntcreatex.in.file_attr = FILE_ATTRIBUTE_DIRECTORY;
		io.ntcreatex.in.share_access = NTCREATEX_SHARE_ACCESS_NONE;
		io.ntcreatex.in.open_disposition = NTCREATEX_DISP_OPEN;
		io.ntcreatex.in.create_options = 0;
		io.ntcreatex.in.impersonation = NTCREATEX_IMPERSONATION_ANONYMOUS;
		io.ntcreatex.in.fname = dirname;
		status = smb_raw_open(cli->tree, mem_ctx, &io);
		if (!NT_STATUS_IS_OK(status)) {
			torture_result(torture, TORTURE_FAIL, "smb_open on the directory failed: %s\n",
				 nt_errstr(status));
			ret = false;
			goto done;
		}
		fnum = io.ntcreatex.out.file.fnum;
	}

	/* Try a read on the directory */

	nread = smbcli_read(cli->tree, fnum, buf, 0, sizeof(buf));
	if (nread >= 0) {
		torture_result(torture, TORTURE_FAIL, "smbcli_read on a directory succeeded, expected "
			 "failure\n");
		ret = false;
	}

	CHECK_STATUS(torture, smbcli_nt_error(cli->tree),
		     NT_STATUS_INVALID_DEVICE_REQUEST);

	/* Same test on the second tcon */

	nread = smbcli_read(tree2, fnum, buf, 0, sizeof(buf));
	if (nread >= 0) {
		torture_result(torture, TORTURE_FAIL, "smbcli_read on a directory succeeded, expected "
			 "failure\n");
		ret = false;
	}

	CHECK_STATUS(torture, smbcli_nt_error(tree2), NT_STATUS_INVALID_HANDLE);

	smbcli_close(cli->tree, fnum);

	/* Try a normal file read on a second tcon */

	fnum = smbcli_open(cli->tree, fname, O_RDWR|O_CREAT, DENY_NONE);
	if (fnum == -1) {
		torture_result(torture, TORTURE_FAIL, "Failed to create %s - %s\n", fname,
			 smbcli_errstr(cli->tree));
		ret = false;
		goto done;
	}

	nread = smbcli_read(tree2, fnum, buf, 0, sizeof(buf));
	CHECK_STATUS(torture, smbcli_nt_error(tree2), NT_STATUS_INVALID_HANDLE);

	smbcli_close(cli->tree, fnum);

 done:
	smbcli_deltree(cli->tree, dirname);
	talloc_free(mem_ctx);

	return ret;
}

static NTSTATUS raw_smbcli_open(struct smbcli_tree *tree, const char *fname, int flags, int share_mode, int *fnum)
{
        union smb_open open_parms;
        unsigned int openfn=0;
        unsigned int accessmode=0;
        TALLOC_CTX *mem_ctx;
        NTSTATUS status;

        mem_ctx = talloc_init("raw_open");
        if (!mem_ctx) return NT_STATUS_NO_MEMORY;

        if (flags & O_CREAT) {
                openfn |= OPENX_OPEN_FUNC_CREATE;
        }
        if (!(flags & O_EXCL)) {
                if (flags & O_TRUNC) {
                        openfn |= OPENX_OPEN_FUNC_TRUNC;
                } else {
                        openfn |= OPENX_OPEN_FUNC_OPEN;
                }
        }

        accessmode = (share_mode<<OPENX_MODE_DENY_SHIFT);

        if ((flags & O_ACCMODE) == O_RDWR) {
                accessmode |= OPENX_MODE_ACCESS_RDWR;
        } else if ((flags & O_ACCMODE) == O_WRONLY) {
                accessmode |= OPENX_MODE_ACCESS_WRITE;
        } else if ((flags & O_ACCMODE) == O_RDONLY) {
                accessmode |= OPENX_MODE_ACCESS_READ;
	}

#if defined(O_SYNC)
        if ((flags & O_SYNC) == O_SYNC) {
                accessmode |= OPENX_MODE_WRITE_THRU;
        }
#endif

        if (share_mode == DENY_FCB) {
                accessmode = OPENX_MODE_ACCESS_FCB | OPENX_MODE_DENY_FCB;
        }

        open_parms.openx.level = RAW_OPEN_OPENX;
        open_parms.openx.in.flags = 0;
        open_parms.openx.in.open_mode = accessmode;
        open_parms.openx.in.search_attrs = FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN;
        open_parms.openx.in.file_attrs = 0;
        open_parms.openx.in.write_time = 0;
        open_parms.openx.in.open_func = openfn;
        open_parms.openx.in.size = 0;
        open_parms.openx.in.timeout = 0;
        open_parms.openx.in.fname = fname;

        status = smb_raw_open(tree, mem_ctx, &open_parms);
        talloc_free(mem_ctx);

        if (fnum && NT_STATUS_IS_OK(status)) {
                *fnum = open_parms.openx.out.file.fnum;
        }

        return status;
}

static NTSTATUS raw_smbcli_t2open(struct smbcli_tree *tree, const char *fname, int flags, int share_mode, int *fnum)
{
        union smb_open io;
        unsigned int openfn=0;
        unsigned int accessmode=0;
        TALLOC_CTX *mem_ctx;
        NTSTATUS status;

        mem_ctx = talloc_init("raw_t2open");
        if (!mem_ctx) return NT_STATUS_NO_MEMORY;

        if (flags & O_CREAT) {
                openfn |= OPENX_OPEN_FUNC_CREATE;
        }
        if (!(flags & O_EXCL)) {
                if (flags & O_TRUNC) {
                        openfn |= OPENX_OPEN_FUNC_TRUNC;
                } else {
                        openfn |= OPENX_OPEN_FUNC_OPEN;
                }
        }

        accessmode = (share_mode<<OPENX_MODE_DENY_SHIFT);

        if ((flags & O_ACCMODE) == O_RDWR) {
                accessmode |= OPENX_MODE_ACCESS_RDWR;
        } else if ((flags & O_ACCMODE) == O_WRONLY) {
                accessmode |= OPENX_MODE_ACCESS_WRITE;
        } else if ((flags & O_ACCMODE) == O_RDONLY) {
                accessmode |= OPENX_MODE_ACCESS_READ;
	}

#if defined(O_SYNC)
        if ((flags & O_SYNC) == O_SYNC) {
                accessmode |= OPENX_MODE_WRITE_THRU;
        }
#endif

        if (share_mode == DENY_FCB) {
                accessmode = OPENX_MODE_ACCESS_FCB | OPENX_MODE_DENY_FCB;
        }

	memset(&io, '\0', sizeof(io));
        io.t2open.level = RAW_OPEN_T2OPEN;
        io.t2open.in.flags = 0;
        io.t2open.in.open_mode = accessmode;
        io.t2open.in.search_attrs = FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_HIDDEN;
        io.t2open.in.file_attrs = 0;
        io.t2open.in.write_time = 0;
        io.t2open.in.open_func = openfn;
        io.t2open.in.size = 0;
        io.t2open.in.timeout = 0;
        io.t2open.in.fname = fname;

        io.t2open.in.num_eas = 1;
	io.t2open.in.eas = talloc_array(mem_ctx, struct ea_struct, io.t2open.in.num_eas);
	io.t2open.in.eas[0].flags = 0;
	io.t2open.in.eas[0].name.s = ".CLASSINFO";
	io.t2open.in.eas[0].value = data_blob_talloc(mem_ctx, "first value", 11);

        status = smb_raw_open(tree, mem_ctx, &io);
        talloc_free(mem_ctx);

        if (fnum && NT_STATUS_IS_OK(status)) {
                *fnum = io.openx.out.file.fnum;
        }

        return status;
}

static NTSTATUS raw_smbcli_ntcreate(struct smbcli_tree *tree, const char *fname, int *fnum)
{
        union smb_open io;
        TALLOC_CTX *mem_ctx;
        NTSTATUS status;

        mem_ctx = talloc_init("raw_t2open");
        if (!mem_ctx) return NT_STATUS_NO_MEMORY;

	memset(&io, '\0', sizeof(io));
        io.generic.level = RAW_OPEN_NTCREATEX;
	io.ntcreatex.in.flags = NTCREATEX_FLAGS_EXTENDED;
	io.ntcreatex.in.root_fid.fnum = 0;
	io.ntcreatex.in.access_mask = SEC_RIGHTS_FILE_ALL;
	io.ntcreatex.in.alloc_size = 0;
	io.ntcreatex.in.file_attr = FILE_ATTRIBUTE_NORMAL;
	io.ntcreatex.in.share_access = NTCREATEX_SHARE_ACCESS_READ | NTCREATEX_SHARE_ACCESS_WRITE;
	io.ntcreatex.in.open_disposition = NTCREATEX_DISP_CREATE;
	io.ntcreatex.in.create_options = 0;
	io.ntcreatex.in.impersonation = NTCREATEX_IMPERSONATION_ANONYMOUS;
	io.ntcreatex.in.security_flags = 0;
	io.ntcreatex.in.fname = fname;

        status = smb_raw_open(tree, mem_ctx, &io);
        talloc_free(mem_ctx);

        if (fnum && NT_STATUS_IS_OK(status)) {
                *fnum = io.openx.out.file.fnum;
        }

        return status;
}


bool torture_samba3_badpath(struct torture_context *torture)
{
	struct smbcli_state *cli_nt = NULL;
	struct smbcli_state *cli_dos = NULL;
	const char *fname = "test.txt";
	const char *fname1 = "test1.txt";
	const char *dirname = "testdir";
	char *fpath;
	char *fpath1;
	int fnum;
	NTSTATUS status;
	bool ret = true;
	TALLOC_CTX *mem_ctx;
	bool nt_status_support;
	bool client_ntlmv2_auth;

	torture_assert(torture, mem_ctx = talloc_init("torture_samba3_badpath"), "talloc_init failed");

	nt_status_support = lpcfg_nt_status_support(torture->lp_ctx);
	client_ntlmv2_auth = lpcfg_client_ntlmv2_auth(torture->lp_ctx);

	torture_assert_goto(torture, lpcfg_set_cmdline(torture->lp_ctx, "nt status support", "yes"), ret, fail, "Could not set 'nt status support = yes'\n");
	torture_assert_goto(torture, lpcfg_set_cmdline(torture->lp_ctx, "client ntlmv2 auth", "yes"), ret, fail, "Could not set 'client ntlmv2 auth = yes'\n");

	torture_assert_goto(torture, torture_open_connection(&cli_nt, torture, 0), ret, fail, "Could not open NTSTATUS connection\n");

	torture_assert_goto(torture, lpcfg_set_cmdline(torture->lp_ctx, "nt status support", "no"), ret, fail, "Could not set 'nt status support = no'\n");
	torture_assert_goto(torture, lpcfg_set_cmdline(torture->lp_ctx, "client ntlmv2 auth", "no"), ret, fail, "Could not set 'client ntlmv2 auth = no'\n");

	torture_assert_goto(torture, torture_open_connection(&cli_dos, torture, 1), ret, fail, "Could not open DOS connection\n");

	torture_assert_goto(torture, lpcfg_set_cmdline(torture->lp_ctx, "nt status support",
						       nt_status_support ? "yes":"no"),
			    ret, fail, "Could not set 'nt status support' back to where it was\n");
	torture_assert_goto(torture, lpcfg_set_cmdline(torture->lp_ctx, "client ntlmv2 auth",
						       client_ntlmv2_auth ? "yes":"no"),
			    ret, fail, "Could not set 'client ntlmv2 auth' back to where it was\n");

	torture_assert(torture, torture_setup_dir(cli_nt, dirname), "creating test directory");

	status = smbcli_chkpath(cli_nt->tree, dirname);
	CHECK_STATUS(torture, status, NT_STATUS_OK);

	status = smbcli_chkpath(cli_nt->tree,
				talloc_asprintf(mem_ctx, "%s\\bla", dirname));
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_NOT_FOUND);

	status = smbcli_chkpath(cli_dos->tree,
				talloc_asprintf(mem_ctx, "%s\\bla", dirname));
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRbadpath));

	status = smbcli_chkpath(cli_nt->tree,
				talloc_asprintf(mem_ctx, "%s\\bla\\blub",
						dirname));
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_PATH_NOT_FOUND);
	status = smbcli_chkpath(cli_dos->tree,
				talloc_asprintf(mem_ctx, "%s\\bla\\blub",
						dirname));
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRbadpath));

	torture_assert_goto(torture, fpath = talloc_asprintf(mem_ctx, "%s\\%s", dirname, fname),
			    ret, fail, "Could not allocate fpath\n");

	fnum = smbcli_open(cli_nt->tree, fpath, O_RDWR | O_CREAT, DENY_NONE);
	if (fnum == -1) {
		torture_result(torture, TORTURE_FAIL, "Could not create file %s: %s\n", fpath,
			 smbcli_errstr(cli_nt->tree));
		goto fail;
	}
	smbcli_close(cli_nt->tree, fnum);

	if (!(fpath1 = talloc_asprintf(mem_ctx, "%s\\%s", dirname, fname1))) {
		goto fail;
	}
	fnum = smbcli_open(cli_nt->tree, fpath1, O_RDWR | O_CREAT, DENY_NONE);
	if (fnum == -1) {
		torture_result(torture, TORTURE_FAIL, "Could not create file %s: %s\n", fpath1,
			 smbcli_errstr(cli_nt->tree));
		goto fail;
	}
	smbcli_close(cli_nt->tree, fnum);

	/*
	 * Do a whole bunch of error code checks on chkpath
	 */

	status = smbcli_chkpath(cli_nt->tree, fpath);
	CHECK_STATUS(torture, status, NT_STATUS_NOT_A_DIRECTORY);
	status = smbcli_chkpath(cli_dos->tree, fpath);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRbadpath));

	status = smbcli_chkpath(cli_nt->tree, "..");
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_PATH_SYNTAX_BAD);
	status = smbcli_chkpath(cli_dos->tree, "..");
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRinvalidpath));

	status = smbcli_chkpath(cli_nt->tree, ".");
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = smbcli_chkpath(cli_dos->tree, ".");
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRbadpath));

	status = smbcli_chkpath(cli_nt->tree, "\t");
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = smbcli_chkpath(cli_dos->tree, "\t");
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRbadpath));

	status = smbcli_chkpath(cli_nt->tree, "\t\\bla");
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = smbcli_chkpath(cli_dos->tree, "\t\\bla");
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRbadpath));

	status = smbcli_chkpath(cli_nt->tree, "<");
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = smbcli_chkpath(cli_dos->tree, "<");
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRbadpath));

	status = smbcli_chkpath(cli_nt->tree, "<\\bla");
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = smbcli_chkpath(cli_dos->tree, "<\\bla");
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRbadpath));

	/*
	 * .... And the same gang against getatr. Note that the DOS error codes
	 * differ....
	 */

	status = smbcli_getatr(cli_nt->tree, fpath, NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OK);
	status = smbcli_getatr(cli_dos->tree, fpath, NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OK);

	status = smbcli_getatr(cli_nt->tree, "..", NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_PATH_SYNTAX_BAD);
	status = smbcli_getatr(cli_dos->tree, "..", NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRinvalidpath));

	status = smbcli_getatr(cli_nt->tree, ".", NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = smbcli_getatr(cli_dos->tree, ".", NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRinvalidname));

	status = smbcli_getatr(cli_nt->tree, "\t", NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = smbcli_getatr(cli_dos->tree, "\t", NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRinvalidname));

	status = smbcli_getatr(cli_nt->tree, "\t\\bla", NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = smbcli_getatr(cli_dos->tree, "\t\\bla", NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRinvalidname));

	status = smbcli_getatr(cli_nt->tree, "<", NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = smbcli_getatr(cli_dos->tree, "<", NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRinvalidname));

	status = smbcli_getatr(cli_nt->tree, "<\\bla", NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = smbcli_getatr(cli_dos->tree, "<\\bla", NULL, NULL, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRinvalidname));

	/* Try the same set with openX. */

	status = raw_smbcli_open(cli_nt->tree, "..", O_RDONLY, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_PATH_SYNTAX_BAD);
	status = raw_smbcli_open(cli_dos->tree, "..", O_RDONLY, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRinvalidpath));

	status = raw_smbcli_open(cli_nt->tree, ".", O_RDONLY, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = raw_smbcli_open(cli_dos->tree, ".", O_RDONLY, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRinvalidname));

	status = raw_smbcli_open(cli_nt->tree, "\t", O_RDONLY, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = raw_smbcli_open(cli_dos->tree, "\t", O_RDONLY, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRinvalidname));

	status = raw_smbcli_open(cli_nt->tree, "\t\\bla", O_RDONLY, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = raw_smbcli_open(cli_dos->tree, "\t\\bla", O_RDONLY, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRinvalidname));

	status = raw_smbcli_open(cli_nt->tree, "<", O_RDONLY, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = raw_smbcli_open(cli_dos->tree, "<", O_RDONLY, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRinvalidname));

	status = raw_smbcli_open(cli_nt->tree, "<\\bla", O_RDONLY, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_INVALID);
	status = raw_smbcli_open(cli_dos->tree, "<\\bla", O_RDONLY, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS, ERRinvalidname));

	/* Let's test EEXIST error code mapping. */
	status = raw_smbcli_open(cli_nt->tree, fpath, O_RDONLY | O_CREAT| O_EXCL, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_COLLISION);
	status = raw_smbcli_open(cli_dos->tree, fpath, O_RDONLY | O_CREAT| O_EXCL, DENY_NONE, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS,ERRfilexists));

	status = raw_smbcli_t2open(cli_nt->tree, fpath, O_RDONLY | O_CREAT| O_EXCL, DENY_NONE, NULL);
	if (!NT_STATUS_EQUAL(status, NT_STATUS_EAS_NOT_SUPPORTED)
	    || !torture_setting_bool(torture, "samba3", false)) {
		/* Against samba3, treat EAS_NOT_SUPPORTED as acceptable */
		CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_COLLISION);
	}
	status = raw_smbcli_t2open(cli_dos->tree, fpath, O_RDONLY | O_CREAT| O_EXCL, DENY_NONE, NULL);
	if (!NT_STATUS_EQUAL(status, NT_STATUS_DOS(ERRDOS,ERReasnotsupported))
	    || !torture_setting_bool(torture, "samba3", false)) {
		/* Against samba3, treat EAS_NOT_SUPPORTED as acceptable */
		CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS,ERRfilexists));
	}

	status = raw_smbcli_ntcreate(cli_nt->tree, fpath, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_COLLISION);
	status = raw_smbcli_ntcreate(cli_dos->tree, fpath, NULL);
	CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS,ERRfilexists));

	/* Try the rename test. */
	{
		union smb_rename io;
		memset(&io, '\0', sizeof(io));
		io.rename.in.pattern1 = fpath1;
		io.rename.in.pattern2 = fpath;

		/* Try with SMBmv rename. */
		status = smb_raw_rename(cli_nt->tree, &io);
		CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_COLLISION);
		status = smb_raw_rename(cli_dos->tree, &io);
		CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS,ERRrename));

		/* Try with NT rename. */
		io.generic.level = RAW_RENAME_NTRENAME;
		io.ntrename.in.old_name = fpath1;
		io.ntrename.in.new_name = fpath;
		io.ntrename.in.attrib = 0;
		io.ntrename.in.cluster_size = 0;
		io.ntrename.in.flags = RENAME_FLAG_RENAME;

		status = smb_raw_rename(cli_nt->tree, &io);
		CHECK_STATUS(torture, status, NT_STATUS_OBJECT_NAME_COLLISION);
		status = smb_raw_rename(cli_dos->tree, &io);
		CHECK_STATUS(torture, status, NT_STATUS_DOS(ERRDOS,ERRrename));
	}

	goto done;

 fail:
	ret = false;

 done:
	if (cli_nt != NULL) {
		smbcli_deltree(cli_nt->tree, dirname);
		torture_close_connection(cli_nt);
	}
	if (cli_dos != NULL) {
		torture_close_connection(cli_dos);
	}
	talloc_free(mem_ctx);

	return ret;
}

static void count_fn(struct clilist_file_info *info, const char *name,
		     void *private_data)
{
	int *counter = (int *)private_data;
	*counter += 1;
}

bool torture_samba3_caseinsensitive(struct torture_context *torture, struct smbcli_state *cli)
{
	TALLOC_CTX *mem_ctx;
	const char *dirname = "insensitive";
	const char *ucase_dirname = "InSeNsItIvE";
	const char *fname = "foo";
	char *fpath;
	int fnum;
	int counter = 0;
	bool ret = false;

	if (!(mem_ctx = talloc_init("torture_samba3_caseinsensitive"))) {
		torture_result(torture, TORTURE_FAIL, "talloc_init failed\n");
		return false;
	}

	torture_assert(torture, torture_setup_dir(cli, dirname), "creating test directory");

	if (!(fpath = talloc_asprintf(mem_ctx, "%s\\%s", dirname, fname))) {
		goto done;
	}
	fnum = smbcli_open(cli->tree, fpath, O_RDWR | O_CREAT, DENY_NONE);
	if (fnum == -1) {
		torture_result(torture, TORTURE_FAIL,
			"Could not create file %s: %s", fpath,
			 smbcli_errstr(cli->tree));
		goto done;
	}
	smbcli_close(cli->tree, fnum);

	smbcli_list(cli->tree, talloc_asprintf(
			    mem_ctx, "%s\\*", ucase_dirname),
		    FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_HIDDEN
		    |FILE_ATTRIBUTE_SYSTEM,
		    count_fn, (void *)&counter);

	if (counter == 3) {
		ret = true;
	}
	else {
		torture_result(torture, TORTURE_FAIL,
			"expected 3 entries, got %d", counter);
		ret = false;
	}

 done:
	talloc_free(mem_ctx);
	return ret;
}

static void close_locked_file(struct tevent_context *ev,
			      struct tevent_timer *te,
			      struct timeval now,
			      void *private_data)
{
	int *pfd = (int *)private_data;

	TALLOC_FREE(te);

	if (*pfd != -1) {
		close(*pfd);
		*pfd = -1;
	}
}

struct lock_result_state {
	NTSTATUS status;
	bool done;
};

static void receive_lock_result(struct smbcli_request *req)
{
	struct lock_result_state *state =
		(struct lock_result_state *)req->async.private_data;

	state->status = smbcli_request_simple_recv(req);
	state->done = true;
}

/*
 * Check that Samba3 correctly deals with conflicting local posix byte range
 * locks on an underlying file via "normal" SMB1 (without unix extensions).
 *
 * Note: This test depends on "posix locking = yes".
 * Note: To run this test, use "--option=torture:localdir=<LOCALDIR>"
 */

bool torture_samba3_posixtimedlock(struct torture_context *tctx, struct smbcli_state *cli)
{
	NTSTATUS status;
	bool ret = true;
	const char *dirname = "posixlock";
	const char *fname = "locked";
	const char *fpath;
	const char *localdir;
	const char *localname;
	int fnum = -1;

	int fd = -1;
	struct flock posix_lock;

	union smb_lock io;
	struct smb_lock_entry lock_entry;
	struct smbcli_request *req;
	struct lock_result_state lock_result;

	struct tevent_timer *te;

	torture_assert(tctx, torture_setup_dir(cli, dirname), "creating test directory");

	if (!(fpath = talloc_asprintf(tctx, "%s\\%s", dirname, fname))) {
		torture_warning(tctx, "talloc failed\n");
		ret = false;
		goto done;
	}
	fnum = smbcli_open(cli->tree, fpath, O_RDWR | O_CREAT, DENY_NONE);
	if (fnum == -1) {
		torture_warning(tctx, "Could not create file %s: %s\n", fpath,
				smbcli_errstr(cli->tree));
		ret = false;
		goto done;
	}

	if (!(localdir = torture_setting_string(tctx, "localdir", NULL))) {
		torture_warning(tctx, "Need 'localdir' setting\n");
		ret = false;
		goto done;
	}

	if (!(localname = talloc_asprintf(tctx, "%s/%s/%s", localdir, dirname,
					  fname))) {
		torture_warning(tctx, "talloc failed\n");
		ret = false;
		goto done;
	}

	/*
	 * Lock a byte range from posix
	 */

	fd = open(localname, O_RDWR);
	if (fd == -1) {
		torture_warning(tctx, "open(%s) failed: %s\n",
				localname, strerror(errno));
		goto done;
	}

	posix_lock.l_type = F_WRLCK;
	posix_lock.l_whence = SEEK_SET;
	posix_lock.l_start = 0;
	posix_lock.l_len = 1;

	if (fcntl(fd, F_SETLK, &posix_lock) == -1) {
		torture_warning(tctx, "fcntl failed: %s\n", strerror(errno));
		ret = false;
		goto done;
	}

	/*
	 * Try a cifs brlock without timeout to see if posix locking = yes
	 */

	io.lockx.in.ulock_cnt = 0;
	io.lockx.in.lock_cnt = 1;

	lock_entry.count = 1;
	lock_entry.offset = 0;
	lock_entry.pid = cli->tree->session->pid;

	io.lockx.level = RAW_LOCK_LOCKX;
	io.lockx.in.mode = LOCKING_ANDX_LARGE_FILES;
	io.lockx.in.timeout = 0;
	io.lockx.in.locks = &lock_entry;
	io.lockx.in.file.fnum = fnum;

	status = smb_raw_lock(cli->tree, &io);

	ret = true;
	CHECK_STATUS(tctx, status, NT_STATUS_LOCK_NOT_GRANTED);

	if (!ret) {
		goto done;
	}

	/*
	 * Now fire off a timed brlock, unlock the posix lock and see if the
	 * timed lock gets through.
	 */

	io.lockx.in.timeout = 5000;

	req = smb_raw_lock_send(cli->tree, &io);
	if (req == NULL) {
		torture_warning(tctx, "smb_raw_lock_send failed\n");
		ret = false;
		goto done;
	}

	lock_result.done = false;
	req->async.fn = receive_lock_result;
	req->async.private_data = &lock_result;

	te = tevent_add_timer(tctx->ev,
			      tctx, timeval_current_ofs(1, 0),
			      close_locked_file, &fd);
	if (te == NULL) {
		torture_warning(tctx, "tevent_add_timer failed\n");
		ret = false;
		goto done;
	}

	while ((fd != -1) || (!lock_result.done)) {
		if (tevent_loop_once(tctx->ev) == -1) {
			torture_warning(tctx, "tevent_loop_once failed: %s\n",
					strerror(errno));
			ret = false;
			goto done;
		}
	}

	CHECK_STATUS(tctx, lock_result.status, NT_STATUS_OK);

 done:
	if (fnum != -1) {
		smbcli_close(cli->tree, fnum);
	}
	if (fd != -1) {
		close(fd);
	}
	smbcli_deltree(cli->tree, dirname);
	return ret;
}

bool torture_samba3_rootdirfid(struct torture_context *tctx, struct smbcli_state *cli)
{
	uint16_t dnum;
	union smb_open io;
	const char *fname = "testfile";
	bool ret = false;

	smbcli_unlink(cli->tree, fname);

	ZERO_STRUCT(io);
	io.generic.level = RAW_OPEN_NTCREATEX;
	io.ntcreatex.in.flags = NTCREATEX_FLAGS_EXTENDED;
	io.ntcreatex.in.root_fid.fnum = 0;
	io.ntcreatex.in.security_flags = 0;
	io.ntcreatex.in.access_mask =
		SEC_STD_SYNCHRONIZE | SEC_FILE_EXECUTE;
	io.ntcreatex.in.alloc_size = 0;
	io.ntcreatex.in.file_attr = FILE_ATTRIBUTE_DIRECTORY;
	io.ntcreatex.in.share_access =
		NTCREATEX_SHARE_ACCESS_READ
		| NTCREATEX_SHARE_ACCESS_READ;
	io.ntcreatex.in.open_disposition = NTCREATEX_DISP_OPEN;
	io.ntcreatex.in.create_options = 0;
	io.ntcreatex.in.impersonation = NTCREATEX_IMPERSONATION_ANONYMOUS;
	io.ntcreatex.in.fname = "\\";
	torture_assert_ntstatus_equal_goto(tctx, smb_raw_open(cli->tree, tctx, &io),
					   NT_STATUS_OK,
					   ret, done, "smb_open on the directory failed: %s\n");

	dnum = io.ntcreatex.out.file.fnum;

	io.ntcreatex.in.flags =
		NTCREATEX_FLAGS_REQUEST_OPLOCK
		| NTCREATEX_FLAGS_REQUEST_BATCH_OPLOCK;
	io.ntcreatex.in.root_fid.fnum = dnum;
	io.ntcreatex.in.security_flags = 0;
	io.ntcreatex.in.open_disposition = NTCREATEX_DISP_OVERWRITE_IF;
	io.ntcreatex.in.access_mask = SEC_RIGHTS_FILE_ALL;
	io.ntcreatex.in.alloc_size = 0;
	io.ntcreatex.in.file_attr = FILE_ATTRIBUTE_NORMAL;
	io.ntcreatex.in.share_access = NTCREATEX_SHARE_ACCESS_NONE;
	io.ntcreatex.in.create_options = 0;
	io.ntcreatex.in.impersonation = NTCREATEX_IMPERSONATION_ANONYMOUS;
	io.ntcreatex.in.fname = fname;

	torture_assert_ntstatus_equal_goto(tctx, smb_raw_open(cli->tree, tctx, &io),
					   NT_STATUS_OK,
					   ret, done, "smb_open on the file failed");

	smbcli_close(cli->tree, io.ntcreatex.out.file.fnum);
	smbcli_close(cli->tree, dnum);
	smbcli_unlink(cli->tree, fname);

	ret = true;
 done:
	return ret;
}

bool torture_samba3_rootdirfid2(struct torture_context *tctx, struct smbcli_state *cli)
{
	int fnum;
	uint16_t dnum;
	union smb_open io;
	const char *dirname1 = "dir1";
	const char *dirname2 = "dir1/dir2";
	const char *path = "dir1/dir2/testfile";
	const char *relname = "dir2/testfile";
	bool ret = false;

	smbcli_deltree(cli->tree, dirname1);

	torture_assert(tctx, torture_setup_dir(cli, dirname1), "creating test directory");
	torture_assert(tctx, torture_setup_dir(cli, dirname2), "creating test directory");

	fnum = smbcli_open(cli->tree, path, O_RDWR | O_CREAT, DENY_NONE);
	if (fnum == -1) {
		torture_result(tctx, TORTURE_FAIL,
			"Could not create file: %s",
			 smbcli_errstr(cli->tree));
		goto done;
	}
	smbcli_close(cli->tree, fnum);

	ZERO_STRUCT(io);
	io.generic.level = RAW_OPEN_NTCREATEX;
	io.ntcreatex.in.flags = NTCREATEX_FLAGS_EXTENDED;
	io.ntcreatex.in.root_fid.fnum = 0;
	io.ntcreatex.in.security_flags = 0;
	io.ntcreatex.in.access_mask =
		SEC_STD_SYNCHRONIZE | SEC_FILE_EXECUTE;
	io.ntcreatex.in.alloc_size = 0;
	io.ntcreatex.in.file_attr = FILE_ATTRIBUTE_DIRECTORY;
	io.ntcreatex.in.share_access =
		NTCREATEX_SHARE_ACCESS_READ
		| NTCREATEX_SHARE_ACCESS_READ;
	io.ntcreatex.in.open_disposition = NTCREATEX_DISP_OPEN;
	io.ntcreatex.in.create_options = 0;
	io.ntcreatex.in.impersonation = NTCREATEX_IMPERSONATION_ANONYMOUS;
	io.ntcreatex.in.fname = dirname1;
	torture_assert_ntstatus_equal_goto(tctx, smb_raw_open(cli->tree, tctx, &io),
					   NT_STATUS_OK,
					   ret, done, "smb_open on the directory failed: %s\n");

	dnum = io.ntcreatex.out.file.fnum;

	io.ntcreatex.in.flags =
		NTCREATEX_FLAGS_REQUEST_OPLOCK
		| NTCREATEX_FLAGS_REQUEST_BATCH_OPLOCK;
	io.ntcreatex.in.root_fid.fnum = dnum;
	io.ntcreatex.in.security_flags = 0;
	io.ntcreatex.in.open_disposition = NTCREATEX_DISP_OPEN;
	io.ntcreatex.in.access_mask = SEC_RIGHTS_FILE_ALL;
	io.ntcreatex.in.alloc_size = 0;
	io.ntcreatex.in.file_attr = FILE_ATTRIBUTE_NORMAL;
	io.ntcreatex.in.share_access = NTCREATEX_SHARE_ACCESS_NONE;
	io.ntcreatex.in.create_options = 0;
	io.ntcreatex.in.impersonation = NTCREATEX_IMPERSONATION_ANONYMOUS;
	io.ntcreatex.in.fname = relname;

	torture_assert_ntstatus_equal_goto(tctx, smb_raw_open(cli->tree, tctx, &io),
					   NT_STATUS_OK,
					   ret, done, "smb_open on the file failed");

	smbcli_close(cli->tree, io.ntcreatex.out.file.fnum);
	smbcli_close(cli->tree, dnum);

	ret = true;
done:
	smbcli_deltree(cli->tree, dirname1);
	return ret;
}

bool torture_samba3_oplock_logoff(struct torture_context *tctx, struct smbcli_state *cli)
{
	union smb_open io;
	const char *fname = "testfile";
	bool ret = false;
	struct smbcli_request *req;
	struct smb_echo echo_req;

	smbcli_unlink(cli->tree, fname);

	ZERO_STRUCT(io);
	io.generic.level = RAW_OPEN_NTCREATEX;
	io.ntcreatex.in.flags = NTCREATEX_FLAGS_EXTENDED;
	io.ntcreatex.in.root_fid.fnum = 0;
	io.ntcreatex.in.security_flags = 0;
	io.ntcreatex.in.access_mask =
		SEC_STD_SYNCHRONIZE | SEC_FILE_EXECUTE;
	io.ntcreatex.in.alloc_size = 0;
	io.ntcreatex.in.file_attr = FILE_ATTRIBUTE_NORMAL;
	io.ntcreatex.in.share_access = NTCREATEX_SHARE_ACCESS_NONE;
	io.ntcreatex.in.open_disposition = NTCREATEX_DISP_OPEN_IF;
	io.ntcreatex.in.create_options = 0;
	io.ntcreatex.in.impersonation = NTCREATEX_IMPERSONATION_ANONYMOUS;
	io.ntcreatex.in.fname = "testfile";
	torture_assert_ntstatus_equal_goto(tctx, smb_raw_open(cli->tree, tctx, &io),
					   NT_STATUS_OK,
					   ret, done, "first smb_open on the file failed");

	/*
	 * Create a conflicting open, causing the one-second delay
	 */

	torture_assert_goto(tctx, req = smb_raw_open_send(cli->tree, &io),
			    ret, done, "smb_raw_open_send on the file failed");

	/*
	 * Pull the VUID from under that request. As of Nov 3, 2008 all Samba3
	 * versions (3.0, 3.2 and master) would spin sending ERRinvuid errors
	 * as long as the client is still connected.
	 */

	torture_assert_ntstatus_equal_goto(tctx, smb_raw_ulogoff(cli->session),
					   NT_STATUS_OK,
					   ret, done, "ulogoff failed failed");

	echo_req.in.repeat_count = 1;
	echo_req.in.size = 1;
	echo_req.in.data = discard_const_p(uint8_t, "");

	torture_assert_ntstatus_equal_goto(tctx, smb_raw_echo(cli->session->transport, &echo_req),
					   NT_STATUS_OK,
					   ret, done, "smb_raw_echo failed");

	ret = true;
 done:
	return ret;
}

bool torture_samba3_check_openX_badname(struct torture_context *tctx, struct smbcli_state *cli)
{
	NTSTATUS status;
	bool ret = false;
	int fnum = -1;
	DATA_BLOB name_blob = data_blob_talloc(cli->tree, NULL, 65535);

	if (name_blob.data == NULL) {
		return false;
	}
	memset(name_blob.data, 0xcc, 65535);
	status = raw_smbcli_openX_name_blob(cli->tree, &name_blob, O_RDWR, DENY_NONE, &fnum);
	CHECK_STATUS(tctx, status, NT_STATUS_OBJECT_NAME_INVALID);
	ret = true;

	return ret;
}
