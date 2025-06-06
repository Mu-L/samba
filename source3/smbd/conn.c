/*
   Unix SMB/CIFS implementation.
   Manage connections_struct structures
   Copyright (C) Andrew Tridgell 1998
   Copyright (C) Alexander Bokovoy 2002
   Copyright (C) Jeremy Allison 2010

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
#include "lib/util/bitmap.h"

static void conn_free_internal(connection_struct *conn);

/****************************************************************************
 * Remove a conn struct from conn->sconn->connections
 * if not already done.
****************************************************************************/

static int conn_struct_destructor(connection_struct *conn)
{
        if (conn->sconn != NULL) {
		DLIST_REMOVE(conn->sconn->connections, conn);
		SMB_ASSERT(conn->sconn->num_connections > 0);
		conn->sconn->num_connections--;
		conn->sconn = NULL;
	}
	conn_free_internal(conn);
	return 0;
}

/****************************************************************************
 Return the number of open connections.
****************************************************************************/

int conn_num_open(struct smbd_server_connection *sconn)
{
	return sconn->num_connections;
}

/****************************************************************************
 Check if a snum is in use.
****************************************************************************/

bool conn_snum_used(struct smbd_server_connection *sconn,
		    int snum)
{
	struct connection_struct *conn;

	for (conn=sconn->connections; conn; conn=conn->next) {
		if (conn->params->service == snum) {
			return true;
		}
	}

	return false;
}

enum protocol_types conn_protocol(struct smbd_server_connection *sconn)
{
	if ((sconn != NULL) &&
	    (sconn->client != NULL) &&
	    (sconn->client->connections != NULL)) {
		return sconn->client->connections->protocol;
	}
	/*
	 * Default to what source3/lib/util.c has as default for the
	 * static Protocol variable to not change behaviour.
	 */
	return PROTOCOL_COREPLUS;
}

bool conn_using_smb2(struct smbd_server_connection *sconn)
{
	enum protocol_types proto = conn_protocol(sconn);
	return (proto >= PROTOCOL_SMB2_02);
}

/****************************************************************************
 Find first available connection slot, starting from a random position.
 The randomisation stops problems with the server dying and clients
 thinking the server is still available.
****************************************************************************/

connection_struct *conn_new(struct smbd_server_connection *sconn)
{
	connection_struct *conn = NULL;

	conn = talloc_zero(NULL, connection_struct);
	if (conn == NULL) {
		DBG_ERR("talloc_zero failed\n");
		return NULL;
	}
	conn->params = talloc(conn, struct share_params);
	if (conn->params == NULL) {
		DBG_ERR("talloc_zero failed\n");
		TALLOC_FREE(conn);
		return NULL;
	}
	conn->vuid_cache = talloc_zero(conn, struct vuid_cache);
	if (conn->vuid_cache == NULL) {
		DBG_ERR("talloc_zero failed\n");
		TALLOC_FREE(conn);
		return NULL;
	}
	conn->connectpath = talloc_strdup(conn, "");
	if (conn->connectpath == NULL) {
		DBG_ERR("talloc_zero failed\n");
		TALLOC_FREE(conn);
		return NULL;
	}
	conn->cwd_fsp = talloc_zero(conn, struct files_struct);
	if (conn->cwd_fsp == NULL) {
		DBG_ERR("talloc_zero failed\n");
		TALLOC_FREE(conn);
		return NULL;
	}
	conn->cwd_fsp->fsp_name = synthetic_smb_fname(conn->cwd_fsp,
						      ".",
						      NULL,
						      NULL,
						      0,
						      0);
	if (conn->cwd_fsp->fsp_name == NULL) {
		TALLOC_FREE(conn);
		return NULL;
	}
	conn->cwd_fsp->fh = fd_handle_create(conn->cwd_fsp);
	if (conn->cwd_fsp->fh == NULL) {
		DBG_ERR("talloc_zero failed\n");
		TALLOC_FREE(conn);
		return NULL;
	}
	conn->sconn = sconn;
	conn->force_group_gid = (gid_t)-1;
	fsp_set_fd(conn->cwd_fsp, -1);
	conn->cwd_fsp->fnum = FNUM_FIELD_INVALID;
	conn->cwd_fsp->conn = conn;

	DLIST_ADD(sconn->connections, conn);
	sconn->num_connections++;

	/*
	 * Catches the case where someone forgets to call
	 * conn_free().
	 */
	talloc_set_destructor(conn, conn_struct_destructor);
	return conn;
}

/****************************************************************************
 Clear a vuid out of the connection's vuid cache
****************************************************************************/

static void conn_clear_vuid_cache(connection_struct *conn, uint64_t vuid)
{
	struct vuid_cache_entry *ent = NULL;
	int i;

	for (i=0; i<VUID_CACHE_SIZE; i++) {
		ent = &conn->vuid_cache->array[i];
		if (ent->vuid == vuid) {
			break;
		}
	}
	if (i == VUID_CACHE_SIZE) {
		return;
	}

	ent->vuid = UID_FIELD_INVALID;

	/*
	 * We need to keep conn->session_info around
	 * if it's equal to ent->session_info as a SMBulogoff
	 * is often followed by a SMBtdis (with an invalid
	 * vuid). The debug code (or regular code in
	 * vfs_full_audit) wants to refer to the
	 * conn->session_info pointer to print debug
	 * statements. Theoretically this is a bug,
	 * as once the vuid is gone the session_info
	 * on the conn struct isn't valid any more,
	 * but there's enough code that assumes
	 * conn->session_info is never null that
	 * it's easier to hold onto the old pointer
	 * until we get a new sessionsetupX.
	 * As everything is hung off the
	 * conn pointer as a talloc context we're not
	 * leaking memory here. See bug #6315. JRA.
	 */
	if (conn->session_info == ent->session_info) {
		ent->session_info = NULL;
	} else {
		TALLOC_FREE(ent->session_info);
	}
	ent->read_only = False;
	ent->share_access = 0;
	TALLOC_FREE(ent->veto_list);
	TALLOC_FREE(ent->hide_list);
}

/****************************************************************************
 Clear a vuid out of the validity cache, and as the 'owner' of a connection.

 Called from invalidate_vuid()
****************************************************************************/

void conn_clear_vuid_caches(struct smbd_server_connection *sconn, uint64_t vuid)
{
	connection_struct *conn;

	for (conn=sconn->connections; conn;conn=conn->next) {
		if (conn->vuid == vuid) {
			conn->vuid = UID_FIELD_INVALID;
		}
		conn_clear_vuid_cache(conn, vuid);
	}
}

/****************************************************************************
 Free a conn structure - internal part.
****************************************************************************/

static void conn_free_internal(connection_struct *conn)
{
	vfs_handle_struct *handle = NULL, *thandle = NULL;
	struct trans_state *state = NULL;

	/* Free vfs_connection_struct */
	handle = conn->vfs_handles;
	while(handle) {
		thandle = handle->next;
		DLIST_REMOVE(conn->vfs_handles, handle);
		if (handle->free_data)
			handle->free_data(&handle->data);
		handle = thandle;
	}

	/* Free any pending transactions stored on this conn. */
	for (state = conn->pending_trans; state; state = state->next) {
		/* state->setup is a talloc child of state. */
		SAFE_FREE(state->param);
		SAFE_FREE(state->data);
	}

	ZERO_STRUCTP(conn);
}

/****************************************************************************
 Free a conn structure.
****************************************************************************/

void conn_free(connection_struct *conn)
{
	TALLOC_FREE(conn);
}

/*
 * Correctly initialize a share with case options.
 */
void conn_setup_case_options(connection_struct *conn)
{
	int snum = conn->params->service;

	if (lp_case_sensitive(snum) == Auto) {
		/* We will be setting this per packet. Set to be case
		* insensitive for now. */
		conn->case_sensitive = false;
	} else {
		conn->case_sensitive = (bool)lp_case_sensitive(snum);
	}

	conn->case_preserve = lp_preserve_case(snum);
	conn->short_case_preserve = lp_short_preserve_case(snum);
}
