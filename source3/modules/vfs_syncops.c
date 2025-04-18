/*
 * ensure meta data operations are performed synchronously
 *
 * Copyright (C) Andrew Tridgell     2007
 * Copyright (C) Christian Ambach, 2010-2011
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "includes.h"
#include "system/filesys.h"
#include "smbd/smbd.h"
#include "source3/smbd/dir.h"

/*

  Some filesystems (even some journaled filesystems) require that a
  fsync() be performed on many meta data operations to ensure that the
  operation is guaranteed to remain in the filesystem after a power
  failure. This is particularly important for some cluster filesystems
  which are participating in a node failover system with clustered
  Samba.

  On those filesystems this module provides a way to perform those
  operations safely.

  Most of the performance loss with this module is in fsync on close().
  You can disable that with
     syncops:onclose = no
  that can be set either globally or per share.

  On certain filesystems that only require the last data written to be
  fsync()'ed, you can disable the metadata synchronization of this module with
     syncops:onmeta = no
  This option can be set either globally or per share.

  You can also disable the module completely for a share with
     syncops:disable = true

 */

struct syncops_config_data {
	bool onclose;
	bool onmeta;
	bool disable;
};

/*
  given a filename, find the parent directory
 */
static char *parent_dir(TALLOC_CTX *mem_ctx, const char *name)
{
	const char *p = strrchr(name, '/');
	if (p == NULL) {
		return talloc_strdup(mem_ctx, ".");
	}
	return talloc_strndup(mem_ctx, name, (p+1) - name);
}

/*
  fsync a directory by name
 */
static void syncops_sync_directory(connection_struct *conn,
				   char *dname)
{
	struct smb_Dir *dir_hnd = NULL;
	struct files_struct *dirfsp = NULL;
	struct smb_filename smb_dname = { .base_name = dname };
	NTSTATUS status;

	status = OpenDir(talloc_tos(),
			 conn,
			 &smb_dname,
			 "*",
			 0,
			 &dir_hnd);
	if (!NT_STATUS_IS_OK(status)) {
		errno = map_errno_from_nt_status(status);
		return;
	}

	dirfsp = dir_hnd_fetch_fsp(dir_hnd);

	smb_vfs_fsync_sync(dirfsp);

	TALLOC_FREE(dir_hnd);
}

/*
  sync two meta data changes for 2 names
 */
static void syncops_two_names(connection_struct *conn,
			      const struct smb_filename *name1,
			      const struct smb_filename *name2)
{
	TALLOC_CTX *tmp_ctx = talloc_new(NULL);
	char *parent1, *parent2;
	parent1 = parent_dir(tmp_ctx, name1->base_name);
	parent2 = parent_dir(tmp_ctx, name2->base_name);
	if (!parent1 || !parent2) {
		talloc_free(tmp_ctx);
		return;
	}
	syncops_sync_directory(conn, parent1);
	if (strcmp(parent1, parent2) != 0) {
		syncops_sync_directory(conn, parent2);
	}
	talloc_free(tmp_ctx);
}

/*
  sync two meta data changes for 1 names
 */
static void syncops_smb_fname(connection_struct *conn,
			      const struct smb_filename *smb_fname)
{
	char *parent = NULL;
	if (smb_fname != NULL) {
		parent = parent_dir(NULL, smb_fname->base_name);
		if (parent != NULL) {
			syncops_sync_directory(conn, parent);
			talloc_free(parent);
		}
	}
}


/*
  renameat needs special handling, as we may need to fsync two directories
 */
static int syncops_renameat(vfs_handle_struct *handle,
			files_struct *srcfsp,
			const struct smb_filename *smb_fname_src,
			files_struct *dstfsp,
			const struct smb_filename *smb_fname_dst,
			const struct vfs_rename_how *how)
{

	int ret;
	struct smb_filename *full_fname_src = NULL;
	struct smb_filename *full_fname_dst = NULL;
	struct syncops_config_data *config;

	SMB_VFS_HANDLE_GET_DATA(handle, config,
				struct syncops_config_data,
				return -1);

	ret = SMB_VFS_NEXT_RENAMEAT(handle,
			srcfsp,
			smb_fname_src,
			dstfsp,
			smb_fname_dst,
			how);
	if (ret == -1) {
		return ret;
	}
	if (config->disable) {
		return ret;
	}
	if (!config->onmeta) {
		return ret;
	}

	full_fname_src = full_path_from_dirfsp_atname(talloc_tos(),
						      srcfsp,
						      smb_fname_src);
	if (full_fname_src == NULL) {
		errno = ENOMEM;
		return ret;
	}
	full_fname_dst = full_path_from_dirfsp_atname(talloc_tos(),
						      dstfsp,
						      smb_fname_dst);
	if (full_fname_dst == NULL) {
		TALLOC_FREE(full_fname_src);
		errno = ENOMEM;
		return ret;
	}
	syncops_two_names(handle->conn,
			  full_fname_src,
			  full_fname_dst);
	TALLOC_FREE(full_fname_src);
	TALLOC_FREE(full_fname_dst);
	return ret;
}

#define SYNCOPS_NEXT_SMB_FNAME(op, fname, args) do {   \
	int ret; \
	struct smb_filename *full_fname = NULL; \
	struct syncops_config_data *config; \
	SMB_VFS_HANDLE_GET_DATA(handle, config, \
				struct syncops_config_data, \
				return -1); \
	ret = SMB_VFS_NEXT_ ## op args; \
	if (ret != 0) { \
		return ret; \
	} \
	if (config->disable) { \
		return ret; \
	} \
	if (!config->onmeta) { \
		return ret; \
	} \
	full_fname = full_path_from_dirfsp_atname(talloc_tos(), \
				dirfsp, \
				smb_fname); \
	if (full_fname == NULL) { \
		return ret; \
	} \
	syncops_smb_fname(dirfsp->conn, full_fname); \
	TALLOC_FREE(full_fname); \
	return ret; \
} while (0)

static int syncops_symlinkat(vfs_handle_struct *handle,
			const struct smb_filename *link_contents,
			struct files_struct *dirfsp,
			const struct smb_filename *smb_fname)
{
	SYNCOPS_NEXT_SMB_FNAME(SYMLINKAT,
			smb_fname,
				(handle,
				link_contents,
				dirfsp,
				smb_fname));
}

static int syncops_linkat(vfs_handle_struct *handle,
			files_struct *srcfsp,
			const struct smb_filename *old_smb_fname,
			files_struct *dstfsp,
			const struct smb_filename *new_smb_fname,
			int flags)
{
	int ret;
	struct syncops_config_data *config;
	struct smb_filename *old_full_fname = NULL;
	struct smb_filename *new_full_fname = NULL;

	SMB_VFS_HANDLE_GET_DATA(handle, config,
				struct syncops_config_data,
				return -1);

	ret = SMB_VFS_NEXT_LINKAT(handle,
			srcfsp,
			old_smb_fname,
			dstfsp,
			new_smb_fname,
			flags);

	if (ret == -1) {
		return ret;
	}
	if (config->disable) {
		return ret;
	}
	if (!config->onmeta) {
		return ret;
	}

	old_full_fname = full_path_from_dirfsp_atname(talloc_tos(),
						      srcfsp,
						      old_smb_fname);
	if (old_full_fname == NULL) {
		return ret;
	}
	new_full_fname = full_path_from_dirfsp_atname(talloc_tos(),
						      dstfsp,
						      new_smb_fname);
	if (new_full_fname == NULL) {
		TALLOC_FREE(old_full_fname);
		return ret;
	}
	syncops_two_names(handle->conn,
			  old_full_fname,
			  new_full_fname);
	TALLOC_FREE(old_full_fname);
	TALLOC_FREE(new_full_fname);
	return ret;
}

static int syncops_openat(struct vfs_handle_struct *handle,
			  const struct files_struct *dirfsp,
			  const struct smb_filename *smb_fname,
			  struct files_struct *fsp,
			  const struct vfs_open_how *how)
{
	SYNCOPS_NEXT_SMB_FNAME(OPENAT, (how->flags & O_CREAT ? smb_fname : NULL),
			       (handle, dirfsp, smb_fname, fsp, how));
}

static int syncops_unlinkat(vfs_handle_struct *handle,
			files_struct *dirfsp,
			const struct smb_filename *smb_fname,
			int flags)
{
        SYNCOPS_NEXT_SMB_FNAME(UNLINKAT,
			smb_fname,
				(handle,
				dirfsp,
				smb_fname,
				flags));
}

static int syncops_mknodat(vfs_handle_struct *handle,
			files_struct *dirfsp,
			const struct smb_filename *smb_fname,
			mode_t mode,
			SMB_DEV_T dev)
{
        SYNCOPS_NEXT_SMB_FNAME(MKNODAT,
			smb_fname,
				(handle,
				dirfsp,
				smb_fname,
				mode,
				dev));
}

static int syncops_mkdirat(vfs_handle_struct *handle,
			struct files_struct *dirfsp,
			const struct smb_filename *smb_fname,
			mode_t mode)
{
        SYNCOPS_NEXT_SMB_FNAME(MKDIRAT,
			full_fname,
				(handle,
				dirfsp,
				smb_fname,
				mode));
}

/* close needs to be handled specially */
static int syncops_close(vfs_handle_struct *handle, files_struct *fsp)
{
	struct syncops_config_data *config;

	SMB_VFS_HANDLE_GET_DATA(handle, config,
				struct syncops_config_data,
				return -1);

	if (fsp->fsp_flags.can_write && config->onclose) {
		/* ideally we'd only do this if we have written some
		 data, but there is no flag for that in fsp yet. */
		fsync(fsp_get_io_fd(fsp));
	}
	return SMB_VFS_NEXT_CLOSE(handle, fsp);
}

static int syncops_connect(struct vfs_handle_struct *handle, const char *service,
			   const char *user)
{

	struct syncops_config_data *config;
	int ret = SMB_VFS_NEXT_CONNECT(handle, service, user);
	if (ret < 0) {
		return ret;
	}

	config = talloc_zero(handle->conn, struct syncops_config_data);
	if (!config) {
		SMB_VFS_NEXT_DISCONNECT(handle);
		DEBUG(0, ("talloc_zero() failed\n"));
		return -1;
	}

	config->onclose = lp_parm_bool(SNUM(handle->conn), "syncops",
					"onclose", true);

	config->onmeta = lp_parm_bool(SNUM(handle->conn), "syncops",
					"onmeta", true);

	config->disable = lp_parm_bool(SNUM(handle->conn), "syncops",
					"disable", false);

	SMB_VFS_HANDLE_SET_DATA(handle, config,
				NULL, struct syncops_config_data,
				return -1);

	return 0;

}

static struct vfs_fn_pointers vfs_syncops_fns = {
	.connect_fn = syncops_connect,
	.mkdirat_fn = syncops_mkdirat,
	.openat_fn = syncops_openat,
	.renameat_fn = syncops_renameat,
	.unlinkat_fn = syncops_unlinkat,
	.symlinkat_fn = syncops_symlinkat,
	.linkat_fn = syncops_linkat,
	.mknodat_fn = syncops_mknodat,
	.close_fn = syncops_close,
};

static_decl_vfs;
NTSTATUS vfs_syncops_init(TALLOC_CTX *ctx)
{
	NTSTATUS ret;

	ret = smb_register_vfs(SMB_VFS_INTERFACE_VERSION, "syncops",
			       &vfs_syncops_fns);

	if (!NT_STATUS_IS_OK(ret))
		return ret;

	return ret;
}
