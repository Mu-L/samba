/*
   Unix SMB/CIFS implementation.
   SMB NT Security Descriptor / Unix permission conversion.
   Copyright (C) Jeremy Allison 1994-2009.
   Copyright (C) Andreas Gruenbacher 2002.
   Copyright (C) Simo Sorce <idra@samba.org> 2009.

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
#include "system/filesys.h"
#include "../libcli/security/security.h"
#include "trans2.h"
#include "passdb/lookup_sid.h"
#include "auth.h"
#include "../librpc/gen_ndr/idmap.h"
#include "../librpc/gen_ndr/ndr_smb_acl.h"
#include "lib/param/loadparm.h"

extern const struct generic_mapping file_generic_mapping;

#undef  DBGC_CLASS
#define DBGC_CLASS DBGC_ACLS

/****************************************************************************
 Data structures representing the internal ACE format.
****************************************************************************/

enum ace_owner {UID_ACE, GID_ACE, WORLD_ACE};
enum ace_attribute {ALLOW_ACE, DENY_ACE}; /* Used for incoming NT ACLS. */

typedef struct canon_ace {
	struct canon_ace *next, *prev;
	SMB_ACL_TAG_T type;
	mode_t perms; /* Only use S_I(R|W|X)USR mode bits here. */
	struct dom_sid trustee;
	enum ace_owner owner_type;
	enum ace_attribute attr;
	struct unixid unix_ug;
	uint8_t ace_flags; /* From windows ACE entry. */
} canon_ace;

#define ALL_ACE_PERMS (S_IRUSR|S_IWUSR|S_IXUSR)

/*
 * EA format of user.SAMBA_PAI (Samba_Posix_Acl_Interitance)
 * attribute on disk - version 1.
 * All values are little endian.
 *
 * |  1   |  1   |   2         |         2           |  ....
 * +------+------+-------------+---------------------+-------------+--------------------+
 * | vers | flag | num_entries | num_default_entries | ..entries.. | default_entries... |
 * +------+------+-------------+---------------------+-------------+--------------------+
 *
 * Entry format is :
 *
 * |  1   |       4           |
 * +------+-------------------+
 * | value|  uid/gid or world |
 * | type |  value            |
 * +------+-------------------+
 *
 * Version 2 format. Stores extra Windows metadata about an ACL.
 *
 * |  1   |  2       |   2         |         2           |  ....
 * +------+----------+-------------+---------------------+-------------+--------------------+
 * | vers | ace      | num_entries | num_default_entries | ..entries.. | default_entries... |
 * |   2  |  type    |             |                     |             |                    |
 * +------+----------+-------------+---------------------+-------------+--------------------+
 *
 * Entry format is :
 *
 * |  1   |  1   |       4           |
 * +------+------+-------------------+
 * | ace  | value|  uid/gid or world |
 * | flag | type |  value            |
 * +------+-------------------+------+
 *
 */

#define PAI_VERSION_OFFSET			0

#define PAI_V1_FLAG_OFFSET			1
#define PAI_V1_NUM_ENTRIES_OFFSET		2
#define PAI_V1_NUM_DEFAULT_ENTRIES_OFFSET	4
#define PAI_V1_ENTRIES_BASE			6
#define PAI_V1_ACL_FLAG_PROTECTED		0x1
#define PAI_V1_ENTRY_LENGTH			5

#define PAI_V1_VERSION				1

#define PAI_V2_TYPE_OFFSET			1
#define PAI_V2_NUM_ENTRIES_OFFSET		3
#define PAI_V2_NUM_DEFAULT_ENTRIES_OFFSET	5
#define PAI_V2_ENTRIES_BASE			7
#define PAI_V2_ENTRY_LENGTH			6

#define PAI_V2_VERSION				2

/*
 * In memory format of user.SAMBA_PAI attribute.
 */

struct pai_entry {
	struct pai_entry *next, *prev;
	uint8_t ace_flags;
	enum ace_owner owner_type;
	struct unixid unix_ug;
};

struct pai_val {
	uint16_t sd_type;
	unsigned int num_entries;
	struct pai_entry *entry_list;
	unsigned int num_def_entries;
	struct pai_entry *def_entry_list;
};

/************************************************************************
 Return a uint32_t of the pai_entry principal.
************************************************************************/

static uint32_t get_pai_entry_val(struct pai_entry *paie)
{
	switch (paie->owner_type) {
		case UID_ACE:
			DEBUG(10,("get_pai_entry_val: uid = %u\n", (unsigned int)paie->unix_ug.id ));
			return (uint32_t)paie->unix_ug.id;
		case GID_ACE:
			DEBUG(10,("get_pai_entry_val: gid = %u\n", (unsigned int)paie->unix_ug.id ));
			return (uint32_t)paie->unix_ug.id;
		case WORLD_ACE:
		default:
			DEBUG(10,("get_pai_entry_val: world ace\n"));
			return (uint32_t)-1;
	}
}

/************************************************************************
 Return a uint32_t of the entry principal.
************************************************************************/

static uint32_t get_entry_val(canon_ace *ace_entry)
{
	switch (ace_entry->owner_type) {
		case UID_ACE:
			DEBUG(10,("get_entry_val: uid = %u\n", (unsigned int)ace_entry->unix_ug.id ));
			return (uint32_t)ace_entry->unix_ug.id;
		case GID_ACE:
			DEBUG(10,("get_entry_val: gid = %u\n", (unsigned int)ace_entry->unix_ug.id ));
			return (uint32_t)ace_entry->unix_ug.id;
		case WORLD_ACE:
		default:
			DEBUG(10,("get_entry_val: world ace\n"));
			return (uint32_t)-1;
	}
}

/************************************************************************
 Create the on-disk format (always v2 now). Caller must free.
************************************************************************/

static char *create_pai_buf_v2(canon_ace *file_ace_list,
				canon_ace *dir_ace_list,
				uint16_t sd_type,
				size_t *store_size)
{
	char *pai_buf = NULL;
	canon_ace *ace_list = NULL;
	char *entry_offset = NULL;
	unsigned int num_entries = 0;
	unsigned int num_def_entries = 0;
	unsigned int i;

	for (ace_list = file_ace_list; ace_list; ace_list = ace_list->next) {
		num_entries++;
	}

	for (ace_list = dir_ace_list; ace_list; ace_list = ace_list->next) {
		num_def_entries++;
	}

	DEBUG(10,("create_pai_buf_v2: num_entries = %u, num_def_entries = %u\n", num_entries, num_def_entries ));

	*store_size = PAI_V2_ENTRIES_BASE +
		((num_entries + num_def_entries)*PAI_V2_ENTRY_LENGTH);

	pai_buf = talloc_array(talloc_tos(), char, *store_size);
	if (!pai_buf) {
		return NULL;
	}

	/* Set up the header. */
	memset(pai_buf, '\0', PAI_V2_ENTRIES_BASE);
	SCVAL(pai_buf,PAI_VERSION_OFFSET,PAI_V2_VERSION);
	SSVAL(pai_buf,PAI_V2_TYPE_OFFSET, sd_type);
	SSVAL(pai_buf,PAI_V2_NUM_ENTRIES_OFFSET,num_entries);
	SSVAL(pai_buf,PAI_V2_NUM_DEFAULT_ENTRIES_OFFSET,num_def_entries);

	DEBUG(10,("create_pai_buf_v2: sd_type = 0x%x\n",
			(unsigned int)sd_type ));

	entry_offset = pai_buf + PAI_V2_ENTRIES_BASE;

	i = 0;
	for (ace_list = file_ace_list; ace_list; ace_list = ace_list->next) {
		uint8_t type_val = (uint8_t)ace_list->owner_type;
		uint32_t entry_val = get_entry_val(ace_list);

		SCVAL(entry_offset,0,ace_list->ace_flags);
		SCVAL(entry_offset,1,type_val);
		SIVAL(entry_offset,2,entry_val);
		DEBUG(10,("create_pai_buf_v2: entry %u [0x%x] [0x%x] [0x%x]\n",
			i,
			(unsigned int)ace_list->ace_flags,
			(unsigned int)type_val,
			(unsigned int)entry_val ));
		i++;
		entry_offset += PAI_V2_ENTRY_LENGTH;
	}

	for (ace_list = dir_ace_list; ace_list; ace_list = ace_list->next) {
		uint8_t type_val = (uint8_t)ace_list->owner_type;
		uint32_t entry_val = get_entry_val(ace_list);

		SCVAL(entry_offset,0,ace_list->ace_flags);
		SCVAL(entry_offset,1,type_val);
		SIVAL(entry_offset,2,entry_val);
		DEBUG(10,("create_pai_buf_v2: entry %u [0x%x] [0x%x] [0x%x]\n",
			i,
			(unsigned int)ace_list->ace_flags,
			(unsigned int)type_val,
			(unsigned int)entry_val ));
		i++;
		entry_offset += PAI_V2_ENTRY_LENGTH;
	}

	return pai_buf;
}

/************************************************************************
 Store the user.SAMBA_PAI attribute on disk.
************************************************************************/

static void store_inheritance_attributes(files_struct *fsp,
					canon_ace *file_ace_list,
					canon_ace *dir_ace_list,
					uint16_t sd_type)
{
	int ret;
	size_t store_size;
	char *pai_buf;

	if (!lp_map_acl_inherit(SNUM(fsp->conn))) {
		return;
	}

	pai_buf = create_pai_buf_v2(file_ace_list, dir_ace_list,
				sd_type, &store_size);

	ret = SMB_VFS_FSETXATTR(fsp, SAMBA_POSIX_INHERITANCE_EA_NAME,
				pai_buf, store_size, 0);

	TALLOC_FREE(pai_buf);

	DEBUG(10,("store_inheritance_attribute: type 0x%x for file %s\n",
		(unsigned int)sd_type,
		fsp_str_dbg(fsp)));

	if (ret == -1 && !no_acl_syscall_error(errno)) {
		DEBUG(1,("store_inheritance_attribute: Error %s\n", strerror(errno) ));
	}
}

/************************************************************************
 Delete the in memory inheritance info.
************************************************************************/

static void free_inherited_info(struct pai_val *pal)
{
	if (pal) {
		struct pai_entry *paie, *paie_next;
		for (paie = pal->entry_list; paie; paie = paie_next) {
			paie_next = paie->next;
			TALLOC_FREE(paie);
		}
		for (paie = pal->def_entry_list; paie; paie = paie_next) {
			paie_next = paie->next;
			TALLOC_FREE(paie);
		}
		TALLOC_FREE(pal);
	}
}

/************************************************************************
 Get any stored ACE flags.
************************************************************************/

static uint16_t get_pai_flags(struct pai_val *pal, canon_ace *ace_entry, bool default_ace)
{
	struct pai_entry *paie;

	if (!pal) {
		return 0;
	}

	/* If the entry exists it is inherited. */
	for (paie = (default_ace ? pal->def_entry_list : pal->entry_list); paie; paie = paie->next) {
		if (ace_entry->owner_type == paie->owner_type &&
				get_entry_val(ace_entry) == get_pai_entry_val(paie))
			return paie->ace_flags;
	}
	return 0;
}

/************************************************************************
 Ensure an attribute just read is valid - v1.
************************************************************************/

static bool check_pai_ok_v1(const char *pai_buf, size_t pai_buf_data_size)
{
	uint16_t num_entries;
	uint16_t num_def_entries;

	if (pai_buf_data_size < PAI_V1_ENTRIES_BASE) {
		/* Corrupted - too small. */
		return false;
	}

	if (CVAL(pai_buf,PAI_VERSION_OFFSET) != PAI_V1_VERSION) {
		return false;
	}

	num_entries = SVAL(pai_buf,PAI_V1_NUM_ENTRIES_OFFSET);
	num_def_entries = SVAL(pai_buf,PAI_V1_NUM_DEFAULT_ENTRIES_OFFSET);

	/* Check the entry lists match. */
	/* Each entry is 5 bytes (type plus 4 bytes of uid or gid). */

	if (((num_entries + num_def_entries)*PAI_V1_ENTRY_LENGTH) +
			PAI_V1_ENTRIES_BASE != pai_buf_data_size) {
		return false;
	}

	return true;
}

/************************************************************************
 Ensure an attribute just read is valid - v2.
************************************************************************/

static bool check_pai_ok_v2(const char *pai_buf, size_t pai_buf_data_size)
{
	uint16_t num_entries;
	uint16_t num_def_entries;

	if (pai_buf_data_size < PAI_V2_ENTRIES_BASE) {
		/* Corrupted - too small. */
		return false;
	}

	if (CVAL(pai_buf,PAI_VERSION_OFFSET) != PAI_V2_VERSION) {
		return false;
	}

	num_entries = SVAL(pai_buf,PAI_V2_NUM_ENTRIES_OFFSET);
	num_def_entries = SVAL(pai_buf,PAI_V2_NUM_DEFAULT_ENTRIES_OFFSET);

	/* Check the entry lists match. */
	/* Each entry is 6 bytes (flags + type + 4 bytes of uid or gid). */

	if (((num_entries + num_def_entries)*PAI_V2_ENTRY_LENGTH) +
			PAI_V2_ENTRIES_BASE != pai_buf_data_size) {
		return false;
	}

	return true;
}

/************************************************************************
 Decode the owner.
************************************************************************/

static bool get_pai_owner_type(struct pai_entry *paie, const char *entry_offset)
{
	paie->owner_type = (enum ace_owner)CVAL(entry_offset,0);
	switch( paie->owner_type) {
		case UID_ACE:
			paie->unix_ug.type = ID_TYPE_UID;
			paie->unix_ug.id = (uid_t)IVAL(entry_offset,1);
			DEBUG(10,("get_pai_owner_type: uid = %u\n",
				(unsigned int)paie->unix_ug.id ));
			break;
		case GID_ACE:
			paie->unix_ug.type = ID_TYPE_GID;
			paie->unix_ug.id = (gid_t)IVAL(entry_offset,1);
			DEBUG(10,("get_pai_owner_type: gid = %u\n",
				(unsigned int)paie->unix_ug.id ));
			break;
		case WORLD_ACE:
			paie->unix_ug.type = ID_TYPE_NOT_SPECIFIED;
			paie->unix_ug.id = -1;
			DEBUG(10,("get_pai_owner_type: world ace\n"));
			break;
		default:
			DEBUG(10,("get_pai_owner_type: unknown type %u\n",
				(unsigned int)paie->owner_type ));
			return false;
	}
	return true;
}

/************************************************************************
 Process v2 entries.
************************************************************************/

static const char *create_pai_v1_entries(struct pai_val *paiv,
				const char *entry_offset,
				bool def_entry)
{
	unsigned int i;

	for (i = 0; i < paiv->num_entries; i++) {
		struct pai_entry *paie = talloc(talloc_tos(), struct pai_entry);
		if (!paie) {
			return NULL;
		}

		paie->ace_flags = SEC_ACE_FLAG_INHERITED_ACE;
		if (!get_pai_owner_type(paie, entry_offset)) {
			TALLOC_FREE(paie);
			return NULL;
		}

		if (!def_entry) {
			DLIST_ADD(paiv->entry_list, paie);
		} else {
			DLIST_ADD(paiv->def_entry_list, paie);
		}
		entry_offset += PAI_V1_ENTRY_LENGTH;
	}
	return entry_offset;
}

/************************************************************************
 Convert to in-memory format from version 1.
************************************************************************/

static struct pai_val *create_pai_val_v1(const char *buf, size_t size)
{
	const char *entry_offset;
	struct pai_val *paiv = NULL;

	if (!check_pai_ok_v1(buf, size)) {
		return NULL;
	}

	paiv = talloc(talloc_tos(), struct pai_val);
	if (!paiv) {
		return NULL;
	}

	memset(paiv, '\0', sizeof(struct pai_val));

	paiv->sd_type = (CVAL(buf,PAI_V1_FLAG_OFFSET) == PAI_V1_ACL_FLAG_PROTECTED) ?
			SEC_DESC_DACL_PROTECTED : 0;

	paiv->num_entries = SVAL(buf,PAI_V1_NUM_ENTRIES_OFFSET);
	paiv->num_def_entries = SVAL(buf,PAI_V1_NUM_DEFAULT_ENTRIES_OFFSET);

	entry_offset = buf + PAI_V1_ENTRIES_BASE;

	DEBUG(10,("create_pai_val: num_entries = %u, num_def_entries = %u\n",
			paiv->num_entries, paiv->num_def_entries ));

	entry_offset = create_pai_v1_entries(paiv, entry_offset, false);
	if (entry_offset == NULL) {
		free_inherited_info(paiv);
		return NULL;
	}
	entry_offset = create_pai_v1_entries(paiv, entry_offset, true);
	if (entry_offset == NULL) {
		free_inherited_info(paiv);
		return NULL;
	}

	return paiv;
}

/************************************************************************
 Process v2 entries.
************************************************************************/

static const char *create_pai_v2_entries(struct pai_val *paiv,
				unsigned int num_entries,
				const char *entry_offset,
				bool def_entry)
{
	unsigned int i;

	for (i = 0; i < num_entries; i++) {
		struct pai_entry *paie = talloc(talloc_tos(), struct pai_entry);
		if (!paie) {
			return NULL;
		}

		paie->ace_flags = CVAL(entry_offset,0);

		if (!get_pai_owner_type(paie, entry_offset+1)) {
			TALLOC_FREE(paie);
			return NULL;
		}
		if (!def_entry) {
			DLIST_ADD(paiv->entry_list, paie);
		} else {
			DLIST_ADD(paiv->def_entry_list, paie);
		}
		entry_offset += PAI_V2_ENTRY_LENGTH;
	}
	return entry_offset;
}

/************************************************************************
 Convert to in-memory format from version 2.
************************************************************************/

static struct pai_val *create_pai_val_v2(const char *buf, size_t size)
{
	const char *entry_offset;
	struct pai_val *paiv = NULL;

	if (!check_pai_ok_v2(buf, size)) {
		return NULL;
	}

	paiv = talloc(talloc_tos(), struct pai_val);
	if (!paiv) {
		return NULL;
	}

	memset(paiv, '\0', sizeof(struct pai_val));

	paiv->sd_type = SVAL(buf,PAI_V2_TYPE_OFFSET);

	paiv->num_entries = SVAL(buf,PAI_V2_NUM_ENTRIES_OFFSET);
	paiv->num_def_entries = SVAL(buf,PAI_V2_NUM_DEFAULT_ENTRIES_OFFSET);

	entry_offset = buf + PAI_V2_ENTRIES_BASE;

	DEBUG(10,("create_pai_val_v2: sd_type = 0x%x num_entries = %u, num_def_entries = %u\n",
			(unsigned int)paiv->sd_type,
			paiv->num_entries, paiv->num_def_entries ));

	entry_offset = create_pai_v2_entries(paiv, paiv->num_entries,
				entry_offset, false);
	if (entry_offset == NULL) {
		free_inherited_info(paiv);
		return NULL;
	}
	entry_offset = create_pai_v2_entries(paiv, paiv->num_def_entries,
				entry_offset, true);
	if (entry_offset == NULL) {
		free_inherited_info(paiv);
		return NULL;
	}

	return paiv;
}

/************************************************************************
 Convert to in-memory format - from either version 1 or 2.
************************************************************************/

static struct pai_val *create_pai_val(const char *buf, size_t size)
{
	if (size < 1) {
		return NULL;
	}
	if (CVAL(buf,PAI_VERSION_OFFSET) == PAI_V1_VERSION) {
		return create_pai_val_v1(buf, size);
	} else if (CVAL(buf,PAI_VERSION_OFFSET) == PAI_V2_VERSION) {
		return create_pai_val_v2(buf, size);
	} else {
		return NULL;
	}
}

/************************************************************************
 Load the user.SAMBA_PAI attribute.
************************************************************************/

static struct pai_val *fload_inherited_info(files_struct *fsp)
{
	char *pai_buf;
	size_t pai_buf_size = 1024;
	struct pai_val *paiv = NULL;
	ssize_t ret;

	if (!lp_map_acl_inherit(SNUM(fsp->conn))) {
		return NULL;
	}

	if ((pai_buf = talloc_array(talloc_tos(), char, pai_buf_size)) == NULL) {
		return NULL;
	}

	do {
		ret = SMB_VFS_FGETXATTR(fsp, SAMBA_POSIX_INHERITANCE_EA_NAME,
					pai_buf, pai_buf_size);
		if (ret == -1) {
			if (errno != ERANGE) {
				break;
			}
			/* Buffer too small - enlarge it. */
			pai_buf_size *= 2;
			TALLOC_FREE(pai_buf);
			if (pai_buf_size > 1024*1024) {
				return NULL; /* Limit malloc to 1mb. */
			}
			if ((pai_buf = talloc_array(talloc_tos(), char, pai_buf_size)) == NULL)
				return NULL;
		}
	} while (ret == -1);

	DEBUG(10,("load_inherited_info: ret = %lu for file %s\n",
		  (unsigned long)ret, fsp_str_dbg(fsp)));

	if (ret == -1) {
		/* No attribute or not supported. */
#if defined(ENOATTR)
		if (errno != ENOATTR)
			DEBUG(10,("load_inherited_info: Error %s\n", strerror(errno) ));
#else
		if (errno != ENOSYS)
			DEBUG(10,("load_inherited_info: Error %s\n", strerror(errno) ));
#endif
		TALLOC_FREE(pai_buf);
		return NULL;
	}

	paiv = create_pai_val(pai_buf, ret);

	if (paiv) {
		DEBUG(10,("load_inherited_info: ACL type is 0x%x for file %s\n",
			  (unsigned int)paiv->sd_type, fsp_str_dbg(fsp)));
	}

	TALLOC_FREE(pai_buf);
	return paiv;
}

/****************************************************************************
 Functions to manipulate the internal ACE format.
****************************************************************************/

/****************************************************************************
 Count a linked list of canonical ACE entries.
****************************************************************************/

static size_t count_canon_ace_list( canon_ace *l_head )
{
	size_t count = 0;
	canon_ace *ace;

	for (ace = l_head; ace; ace = ace->next)
		count++;

	return count;
}

/****************************************************************************
 Free a linked list of canonical ACE entries.
****************************************************************************/

static void free_canon_ace_list( canon_ace *l_head )
{
	canon_ace *list, *next;

	for (list = l_head; list; list = next) {
		next = list->next;
		DLIST_REMOVE(l_head, list);
		TALLOC_FREE(list);
	}
}

/****************************************************************************
 Function to duplicate a canon_ace entry.
****************************************************************************/

static canon_ace *dup_canon_ace( canon_ace *src_ace)
{
	canon_ace *dst_ace = talloc(talloc_tos(), canon_ace);

	if (dst_ace == NULL)
		return NULL;

	*dst_ace = *src_ace;
	dst_ace->prev = dst_ace->next = NULL;
	return dst_ace;
}

/****************************************************************************
 Print out a canon ace.
****************************************************************************/

static void print_canon_ace(canon_ace *pace, int num)
{
	struct dom_sid_buf buf;
	dbgtext( "canon_ace index %d. Type = %s ", num, pace->attr == ALLOW_ACE ? "allow" : "deny" );
	dbgtext( "SID = %s ", dom_sid_str_buf(&pace->trustee, &buf));
	if (pace->owner_type == UID_ACE) {
		dbgtext( "uid %u ", (unsigned int)pace->unix_ug.id);
	} else if (pace->owner_type == GID_ACE) {
		dbgtext( "gid %u ", (unsigned int)pace->unix_ug.id);
	} else
		dbgtext( "other ");
	switch (pace->type) {
		case SMB_ACL_USER:
			dbgtext( "SMB_ACL_USER ");
			break;
		case SMB_ACL_USER_OBJ:
			dbgtext( "SMB_ACL_USER_OBJ ");
			break;
		case SMB_ACL_GROUP:
			dbgtext( "SMB_ACL_GROUP ");
			break;
		case SMB_ACL_GROUP_OBJ:
			dbgtext( "SMB_ACL_GROUP_OBJ ");
			break;
		case SMB_ACL_OTHER:
			dbgtext( "SMB_ACL_OTHER ");
			break;
		default:
			dbgtext( "MASK " );
			break;
	}

	dbgtext( "ace_flags = 0x%x ", (unsigned int)pace->ace_flags);
	dbgtext( "perms ");
	dbgtext( "%c", pace->perms & S_IRUSR ? 'r' : '-');
	dbgtext( "%c", pace->perms & S_IWUSR ? 'w' : '-');
	dbgtext( "%c\n", pace->perms & S_IXUSR ? 'x' : '-');
}

/****************************************************************************
 Print out a canon ace list.
****************************************************************************/

static void print_canon_ace_list(const char *name, canon_ace *ace_list)
{
	int count = 0;

	if( DEBUGLVL( 10 )) {
		dbgtext( "print_canon_ace_list: %s\n", name );
		for (;ace_list; ace_list = ace_list->next, count++)
			print_canon_ace(ace_list, count );
	}
}

/****************************************************************************
 Map POSIX ACL perms to canon_ace permissions (a mode_t containing only S_(R|W|X)USR bits).
****************************************************************************/

static mode_t convert_permset_to_mode_t(SMB_ACL_PERMSET_T permset)
{
	mode_t ret = 0;

	ret |= (sys_acl_get_perm(permset, SMB_ACL_READ) ? S_IRUSR : 0);
	ret |= (sys_acl_get_perm(permset, SMB_ACL_WRITE) ? S_IWUSR : 0);
	ret |= (sys_acl_get_perm(permset, SMB_ACL_EXECUTE) ? S_IXUSR : 0);

	return ret;
}

/****************************************************************************
 Map generic UNIX permissions to canon_ace permissions (a mode_t containing only S_(R|W|X)USR bits).
****************************************************************************/

mode_t unix_perms_to_acl_perms(mode_t mode, int r_mask, int w_mask, int x_mask)
{
	mode_t ret = 0;

	if (mode & r_mask)
		ret |= S_IRUSR;
	if (mode & w_mask)
		ret |= S_IWUSR;
	if (mode & x_mask)
		ret |= S_IXUSR;

	return ret;
}

/****************************************************************************
 Map canon_ace permissions (a mode_t containing only S_(R|W|X)USR bits) to
 an SMB_ACL_PERMSET_T.
****************************************************************************/

int map_acl_perms_to_permset(mode_t mode, SMB_ACL_PERMSET_T *p_permset)
{
	if (sys_acl_clear_perms(*p_permset) ==  -1)
		return -1;
	if (mode & S_IRUSR) {
		if (sys_acl_add_perm(*p_permset, SMB_ACL_READ) == -1)
			return -1;
	}
	if (mode & S_IWUSR) {
		if (sys_acl_add_perm(*p_permset, SMB_ACL_WRITE) == -1)
			return -1;
	}
	if (mode & S_IXUSR) {
		if (sys_acl_add_perm(*p_permset, SMB_ACL_EXECUTE) == -1)
			return -1;
	}
	return 0;
}

/****************************************************************************
 Function to create owner and group SIDs from a SMB_STRUCT_STAT.
****************************************************************************/

static void create_file_sids(const SMB_STRUCT_STAT *psbuf,
			     struct dom_sid *powner_sid,
			     struct dom_sid *pgroup_sid)
{
	uid_to_sid( powner_sid, psbuf->st_ex_uid );
	gid_to_sid( pgroup_sid, psbuf->st_ex_gid );
}

/****************************************************************************
 Merge aces with a common UID or GID - if both are allow or deny, OR the permissions together and
 delete the second one. If the first is deny, mask the permissions off and delete the allow
 if the permissions become zero, delete the deny if the permissions are non zero.
****************************************************************************/

static void merge_aces( canon_ace **pp_list_head, bool dir_acl)
{
	canon_ace *l_head = *pp_list_head;
	canon_ace *curr_ace_outer;
	canon_ace *curr_ace_outer_next;

	/*
	 * First, merge allow entries with identical SIDs, and deny entries
	 * with identical SIDs.
	 */

	for (curr_ace_outer = l_head; curr_ace_outer; curr_ace_outer = curr_ace_outer_next) {
		canon_ace *curr_ace;
		canon_ace *curr_ace_next;

		curr_ace_outer_next = curr_ace_outer->next; /* Save the link in case we delete. */

		for (curr_ace = curr_ace_outer->next; curr_ace; curr_ace = curr_ace_next) {
			bool can_merge = false;

			curr_ace_next = curr_ace->next; /* Save the link in case of delete. */

			/* For file ACLs we can merge if the SIDs and ALLOW/DENY
			 * types are the same. For directory acls we must also
			 * ensure the POSIX ACL types are the same.
			 *
			 * For the IDMAP_BOTH case, we must not merge
			 * the UID and GID ACE values for same SID
			 */

			if (!dir_acl) {
				can_merge = (curr_ace->unix_ug.id == curr_ace_outer->unix_ug.id &&
					     curr_ace->owner_type == curr_ace_outer->owner_type &&
					     (curr_ace->attr == curr_ace_outer->attr));
			} else {
				can_merge = (curr_ace->unix_ug.id == curr_ace_outer->unix_ug.id &&
					     curr_ace->owner_type == curr_ace_outer->owner_type &&
					     (curr_ace->type == curr_ace_outer->type) &&
					     (curr_ace->attr == curr_ace_outer->attr));
			}

			if (can_merge) {
				if( DEBUGLVL( 10 )) {
					dbgtext("merge_aces: Merging ACE's\n");
					print_canon_ace( curr_ace_outer, 0);
					print_canon_ace( curr_ace, 0);
				}

				/* Merge two allow or two deny ACE's. */

				/* Theoretically we shouldn't merge a dir ACE if
				 * one ACE has the CI flag set, and the other
				 * ACE has the OI flag set, but this is rare
				 * enough we can ignore it. */

				curr_ace_outer->perms |= curr_ace->perms;
				curr_ace_outer->ace_flags |= curr_ace->ace_flags;
				DLIST_REMOVE(l_head, curr_ace);
				TALLOC_FREE(curr_ace);
				curr_ace_outer_next = curr_ace_outer->next; /* We may have deleted the link. */
			}
		}
	}

	/*
	 * Now go through and mask off allow permissions with deny permissions.
	 * We can delete either the allow or deny here as we know that each SID
	 * appears only once in the list.
	 */

	for (curr_ace_outer = l_head; curr_ace_outer; curr_ace_outer = curr_ace_outer_next) {
		canon_ace *curr_ace;
		canon_ace *curr_ace_next;

		curr_ace_outer_next = curr_ace_outer->next; /* Save the link in case we delete. */

		for (curr_ace = curr_ace_outer->next; curr_ace; curr_ace = curr_ace_next) {

			curr_ace_next = curr_ace->next; /* Save the link in case of delete. */

			/*
			 * Subtract ACE's with different entries. Due to the ordering constraints
			 * we've put on the ACL, we know the deny must be the first one.
			 */

			if (curr_ace->unix_ug.id == curr_ace_outer->unix_ug.id &&
			    (curr_ace->owner_type == curr_ace_outer->owner_type) &&
			    (curr_ace_outer->attr == DENY_ACE) && (curr_ace->attr == ALLOW_ACE)) {

				if( DEBUGLVL( 10 )) {
					dbgtext("merge_aces: Masking ACE's\n");
					print_canon_ace( curr_ace_outer, 0);
					print_canon_ace( curr_ace, 0);
				}

				curr_ace->perms &= ~curr_ace_outer->perms;

				if (curr_ace->perms == 0) {

					/*
					 * The deny overrides the allow. Remove the allow.
					 */

					DLIST_REMOVE(l_head, curr_ace);
					TALLOC_FREE(curr_ace);
					curr_ace_outer_next = curr_ace_outer->next; /* We may have deleted the link. */

				} else {

					/*
					 * Even after removing permissions, there
					 * are still allow permissions - delete the deny.
					 * It is safe to delete the deny here,
					 * as we are guaranteed by the deny first
					 * ordering that all the deny entries for
					 * this SID have already been merged into one
					 * before we can get to an allow ace.
					 */

					DLIST_REMOVE(l_head, curr_ace_outer);
					TALLOC_FREE(curr_ace_outer);
					break;
				}
			}

		} /* end for curr_ace */
	} /* end for curr_ace_outer */

	/* We may have modified the list. */

	*pp_list_head = l_head;
}

/****************************************************************************
 Map canon_ace perms to permission bits NT.
 The attr element is not used here - we only process deny entries on set,
 not get. Deny entries are implicit on get with ace->perms = 0.
****************************************************************************/

uint32_t map_canon_ace_perms(int snum,
				enum security_ace_type *pacl_type,
				mode_t perms,
				bool directory_ace)
{
	uint32_t nt_mask = 0;

	*pacl_type = SEC_ACE_TYPE_ACCESS_ALLOWED;

	if (lp_acl_map_full_control(snum) && ((perms & ALL_ACE_PERMS) == ALL_ACE_PERMS)) {
		if (directory_ace) {
			nt_mask = UNIX_DIRECTORY_ACCESS_RWX;
		} else {
			nt_mask = (UNIX_ACCESS_RWX & ~DELETE_ACCESS);
		}
	} else if ((perms & ALL_ACE_PERMS) == (mode_t)0) {
		/*
		 * Windows NT refuses to display ACEs with no permissions in them (but
		 * they are perfectly legal with Windows 2000). If the ACE has empty
		 * permissions we cannot use 0, so we use the otherwise unused
		 * WRITE_OWNER permission, which we ignore when we set an ACL.
		 * We abstract this into a #define of UNIX_ACCESS_NONE to allow this
		 * to be changed in the future.
		 */

		nt_mask = 0;
	} else {
		if (directory_ace) {
			nt_mask |= ((perms & S_IRUSR) ? UNIX_DIRECTORY_ACCESS_R : 0 );
			nt_mask |= ((perms & S_IWUSR) ? UNIX_DIRECTORY_ACCESS_W : 0 );
			nt_mask |= ((perms & S_IXUSR) ? UNIX_DIRECTORY_ACCESS_X : 0 );
		} else {
			nt_mask |= ((perms & S_IRUSR) ? UNIX_ACCESS_R : 0 );
			nt_mask |= ((perms & S_IWUSR) ? UNIX_ACCESS_W : 0 );
			nt_mask |= ((perms & S_IXUSR) ? UNIX_ACCESS_X : 0 );
		}
	}

	if ((perms & S_IWUSR) && lp_dos_filemode(snum)) {
		nt_mask |= (SEC_STD_WRITE_DAC|SEC_STD_WRITE_OWNER|DELETE_ACCESS);
	}

	DEBUG(10,("map_canon_ace_perms: Mapped (UNIX) %x to (NT) %x\n",
			(unsigned int)perms, (unsigned int)nt_mask ));

	return nt_mask;
}

/****************************************************************************
 Map NT perms to a UNIX mode_t.
****************************************************************************/

#define FILE_SPECIFIC_READ_BITS (FILE_READ_DATA|FILE_READ_EA)
#define FILE_SPECIFIC_WRITE_BITS (FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA)
#define FILE_SPECIFIC_EXECUTE_BITS (FILE_EXECUTE)

static mode_t map_nt_perms( uint32_t *mask, int type)
{
	mode_t mode = 0;

	switch(type) {
	case S_IRUSR:
		if((*mask) & GENERIC_ALL_ACCESS)
			mode = S_IRUSR|S_IWUSR|S_IXUSR;
		else {
			mode |= ((*mask) & (GENERIC_READ_ACCESS|FILE_SPECIFIC_READ_BITS)) ? S_IRUSR : 0;
			mode |= ((*mask) & (GENERIC_WRITE_ACCESS|FILE_SPECIFIC_WRITE_BITS)) ? S_IWUSR : 0;
			mode |= ((*mask) & (GENERIC_EXECUTE_ACCESS|FILE_SPECIFIC_EXECUTE_BITS)) ? S_IXUSR : 0;
		}
		break;
	case S_IRGRP:
		if((*mask) & GENERIC_ALL_ACCESS)
			mode = S_IRGRP|S_IWGRP|S_IXGRP;
		else {
			mode |= ((*mask) & (GENERIC_READ_ACCESS|FILE_SPECIFIC_READ_BITS)) ? S_IRGRP : 0;
			mode |= ((*mask) & (GENERIC_WRITE_ACCESS|FILE_SPECIFIC_WRITE_BITS)) ? S_IWGRP : 0;
			mode |= ((*mask) & (GENERIC_EXECUTE_ACCESS|FILE_SPECIFIC_EXECUTE_BITS)) ? S_IXGRP : 0;
		}
		break;
	case S_IROTH:
		if((*mask) & GENERIC_ALL_ACCESS)
			mode = S_IROTH|S_IWOTH|S_IXOTH;
		else {
			mode |= ((*mask) & (GENERIC_READ_ACCESS|FILE_SPECIFIC_READ_BITS)) ? S_IROTH : 0;
			mode |= ((*mask) & (GENERIC_WRITE_ACCESS|FILE_SPECIFIC_WRITE_BITS)) ? S_IWOTH : 0;
			mode |= ((*mask) & (GENERIC_EXECUTE_ACCESS|FILE_SPECIFIC_EXECUTE_BITS)) ? S_IXOTH : 0;
		}
		break;
	}

	return mode;
}

/****************************************************************************
 Unpack a struct security_descriptor into a UNIX owner and group.
****************************************************************************/

static NTSTATUS unpack_nt_owners(struct connection_struct *conn,
				 uid_t *puser, gid_t *pgrp,
				 uint32_t security_info_sent,
				 const struct security_descriptor *psd)
{
	*puser = (uid_t)-1;
	*pgrp = (gid_t)-1;

	if(security_info_sent == 0) {
		DBG_NOTICE("no security info sent !\n");
		return NT_STATUS_OK;
	}

	/*
	 * Validate the owner and group SID's.
	 */

	DEBUG(5,("unpack_nt_owners: validating owner_sids.\n"));

	/*
	 * Don't immediately fail if the owner sid cannot be validated.
	 * This may be a group chown only set.
	 */

	if (security_info_sent & SECINFO_OWNER) {
		if (!sid_to_uid(psd->owner_sid, puser)) {
			if (lp_force_unknown_acl_user(SNUM(conn))) {
				/* this allows take ownership to work
				 * reasonably */
				*puser = get_current_uid(conn);
			} else {
				struct dom_sid_buf buf;
				DBG_NOTICE("unable to validate"
					   " owner sid for %s\n",
					   dom_sid_str_buf(psd->owner_sid,
							   &buf));
				return NT_STATUS_INVALID_OWNER;
			}
		}
		DEBUG(3,("unpack_nt_owners: owner sid mapped to uid %u\n",
			 (unsigned int)*puser ));
 	}

	/*
	 * Don't immediately fail if the group sid cannot be validated.
	 * This may be an owner chown only set.
	 */

	if (security_info_sent & SECINFO_GROUP) {
		if (!sid_to_gid(psd->group_sid, pgrp)) {
			if (lp_force_unknown_acl_user(SNUM(conn))) {
				/* this allows take group ownership to work
				 * reasonably */
				*pgrp = get_current_gid(conn);
			} else {
				DEBUG(3,("unpack_nt_owners: unable to validate"
					 " group sid.\n"));
				return NT_STATUS_INVALID_OWNER;
			}
		}
		DEBUG(3,("unpack_nt_owners: group sid mapped to gid %u\n",
			 (unsigned int)*pgrp));
 	}

	DEBUG(5,("unpack_nt_owners: owner_sids validated.\n"));

	return NT_STATUS_OK;
}


static void trim_ace_perms(canon_ace *pace)
{
	pace->perms = pace->perms & (S_IXUSR|S_IWUSR|S_IRUSR);
}

static void ensure_minimal_owner_ace_perms(const bool is_directory,
					   canon_ace *pace)
{
	pace->perms |= S_IRUSR;
	if (is_directory) {
		pace->perms |= (S_IWUSR|S_IXUSR);
	}
}

/****************************************************************************
 Check if a given uid/SID is in a group gid/SID. This is probably very
 expensive and will need optimisation. A *lot* of optimisation :-). JRA.
****************************************************************************/

static bool uid_entry_in_group(connection_struct *conn, canon_ace *uid_ace, canon_ace *group_ace )
{
	bool is_sid = false;
	bool has_sid = false;
	struct security_token *security_token = NULL;

	/* "Everyone" always matches every uid. */

	if (dom_sid_equal(&group_ace->trustee, &global_sid_World))
		return True;

	security_token = conn->session_info->security_token;
	/* security_token should not be NULL */
	SMB_ASSERT(security_token);
	is_sid = security_token_is_sid(security_token,
				       &uid_ace->trustee);
	if (is_sid) {
		has_sid = security_token_has_sid(security_token,
						 &group_ace->trustee);

		if (has_sid) {
			return true;
		}
	}

	/*
	 * if it's the current user, we already have the unix token
	 * and don't need to do the complex user_in_group_sid() call
	 */
	if (uid_ace->unix_ug.id == get_current_uid(conn)) {
		const struct security_unix_token *curr_utok = NULL;
		size_t i;

		if (group_ace->unix_ug.id == get_current_gid(conn)) {
			return True;
		}

		curr_utok = get_current_utok(conn);
		for (i=0; i < curr_utok->ngroups; i++) {
			if (group_ace->unix_ug.id == curr_utok->groups[i]) {
				return True;
			}
		}
	}

	/*
	 * user_in_group_sid() uses create_token_from_sid()
	 * which creates an artificial NT token given just a username,
	 * so this is not reliable for users from foreign domains
	 * exported by winbindd!
	 */
	return user_sid_in_group_sid(&uid_ace->trustee, &group_ace->trustee);
}

/****************************************************************************
 A well formed POSIX file or default ACL has at least 3 entries, a
 SMB_ACL_USER_OBJ, SMB_ACL_GROUP_OBJ, SMB_ACL_OTHER_OBJ.
 In addition, the owner must always have at least read access.
 When using this call on get_acl, the pst struct is valid and contains
 the mode of the file.
****************************************************************************/

static bool ensure_canon_entry_valid_on_get(connection_struct *conn,
					canon_ace **pp_ace,
					const struct dom_sid *pfile_owner_sid,
					const struct dom_sid *pfile_grp_sid,
					const SMB_STRUCT_STAT *pst)
{
	canon_ace *pace;
	bool got_user = false;
	bool got_group = false;
	bool got_other = false;

	for (pace = *pp_ace; pace; pace = pace->next) {
		if (pace->type == SMB_ACL_USER_OBJ) {
			got_user = true;
		} else if (pace->type == SMB_ACL_GROUP_OBJ) {
			got_group = true;
		} else if (pace->type == SMB_ACL_OTHER) {
			got_other = true;
		}
	}

	if (!got_user) {
		if ((pace = talloc(talloc_tos(), canon_ace)) == NULL) {
			DEBUG(0,("malloc fail.\n"));
			return false;
		}

		ZERO_STRUCTP(pace);
		pace->type = SMB_ACL_USER_OBJ;
		pace->owner_type = UID_ACE;
		pace->unix_ug.type = ID_TYPE_UID;
		pace->unix_ug.id = pst->st_ex_uid;
		pace->trustee = *pfile_owner_sid;
		pace->attr = ALLOW_ACE;
		pace->perms = unix_perms_to_acl_perms(pst->st_ex_mode, S_IRUSR, S_IWUSR, S_IXUSR);
		DLIST_ADD(*pp_ace, pace);
	}

	if (!got_group) {
		if ((pace = talloc(talloc_tos(), canon_ace)) == NULL) {
			DEBUG(0,("malloc fail.\n"));
			return false;
		}

		ZERO_STRUCTP(pace);
		pace->type = SMB_ACL_GROUP_OBJ;
		pace->owner_type = GID_ACE;
		pace->unix_ug.type = ID_TYPE_GID;
		pace->unix_ug.id = pst->st_ex_gid;
		pace->trustee = *pfile_grp_sid;
		pace->attr = ALLOW_ACE;
		pace->perms = unix_perms_to_acl_perms(pst->st_ex_mode, S_IRGRP, S_IWGRP, S_IXGRP);
		DLIST_ADD(*pp_ace, pace);
	}

	if (!got_other) {
		if ((pace = talloc(talloc_tos(), canon_ace)) == NULL) {
			DEBUG(0,("malloc fail.\n"));
			return false;
		}

		ZERO_STRUCTP(pace);
		pace->type = SMB_ACL_OTHER;
		pace->owner_type = WORLD_ACE;
		pace->unix_ug.type = ID_TYPE_NOT_SPECIFIED;
		pace->unix_ug.id = -1;
		pace->trustee = global_sid_World;
		pace->attr = ALLOW_ACE;
		pace->perms = unix_perms_to_acl_perms(pst->st_ex_mode, S_IROTH, S_IWOTH, S_IXOTH);
		DLIST_ADD(*pp_ace, pace);
	}

	return true;
}

/****************************************************************************
 A well formed POSIX file or default ACL has at least 3 entries, a
 SMB_ACL_USER_OBJ, SMB_ACL_GROUP_OBJ, SMB_ACL_OTHER_OBJ.
 In addition, the owner must always have at least read access.
 When using this call on set_acl, the pst struct has
 been modified to have a mode containing the default for this file or directory
 type.
****************************************************************************/

static bool ensure_canon_entry_valid_on_set(connection_struct *conn,
					canon_ace **pp_ace,
					bool is_default_acl,
					const struct share_params *params,
					const bool is_directory,
					const struct dom_sid *pfile_owner_sid,
					const struct dom_sid *pfile_grp_sid,
					const SMB_STRUCT_STAT *pst)
{
	canon_ace *pace;
	canon_ace *pace_user = NULL;
	canon_ace *pace_group = NULL;
	canon_ace *pace_other = NULL;
	bool got_duplicate_user = false;
	bool got_duplicate_group = false;

	for (pace = *pp_ace; pace; pace = pace->next) {
		trim_ace_perms(pace);
		if (pace->type == SMB_ACL_USER_OBJ) {
			ensure_minimal_owner_ace_perms(is_directory, pace);
			pace_user = pace;
		} else if (pace->type == SMB_ACL_GROUP_OBJ) {
			pace_group = pace;
		} else if (pace->type == SMB_ACL_OTHER) {
			pace_other = pace;
		}
	}

	if (!pace_user) {
		canon_ace *pace_iter;

		if ((pace = talloc(talloc_tos(), canon_ace)) == NULL) {
			DEBUG(0,("talloc fail.\n"));
			return false;
		}

		ZERO_STRUCTP(pace);
		pace->type = SMB_ACL_USER_OBJ;
		pace->owner_type = UID_ACE;
		pace->unix_ug.type = ID_TYPE_UID;
		pace->unix_ug.id = pst->st_ex_uid;
		pace->trustee = *pfile_owner_sid;
		pace->attr = ALLOW_ACE;
		/* Start with existing user permissions, principle of least
		   surprises for the user. */
		pace->perms = unix_perms_to_acl_perms(pst->st_ex_mode, S_IRUSR, S_IWUSR, S_IXUSR);

		/* See if the owning user is in any of the other groups in
		   the ACE, or if there's a matching user entry (by uid
		   or in the case of ID_TYPE_BOTH by SID).
		   If so, OR in the permissions from that entry. */


		for (pace_iter = *pp_ace; pace_iter; pace_iter = pace_iter->next) {
			if (pace_iter->type == SMB_ACL_USER &&
					pace_iter->unix_ug.id == pace->unix_ug.id) {
				pace->perms |= pace_iter->perms;
			} else if (pace_iter->type == SMB_ACL_GROUP_OBJ || pace_iter->type == SMB_ACL_GROUP) {
				if (dom_sid_equal(&pace->trustee, &pace_iter->trustee)) {
					pace->perms |= pace_iter->perms;
				} else if (uid_entry_in_group(conn, pace, pace_iter)) {
					pace->perms |= pace_iter->perms;
				}
			}
		}

		if (pace->perms == 0) {
			/* If we only got an "everyone" perm, just use that. */
			if (pace_other)
				pace->perms = pace_other->perms;
		}

		/*
		 * Ensure we have default parameters for the
		 * user (owner) even on default ACLs.
		 */
		ensure_minimal_owner_ace_perms(is_directory, pace);

		DLIST_ADD(*pp_ace, pace);
		pace_user = pace;
	}

	if (!pace_group) {
		if ((pace = talloc(talloc_tos(), canon_ace)) == NULL) {
			DEBUG(0,("talloc fail.\n"));
			return false;
		}

		ZERO_STRUCTP(pace);
		pace->type = SMB_ACL_GROUP_OBJ;
		pace->owner_type = GID_ACE;
		pace->unix_ug.type = ID_TYPE_GID;
		pace->unix_ug.id = pst->st_ex_gid;
		pace->trustee = *pfile_grp_sid;
		pace->attr = ALLOW_ACE;

		/* If we only got an "everyone" perm, just use that. */
		if (pace_other) {
			pace->perms = pace_other->perms;
		} else {
			pace->perms = 0;
		}

		DLIST_ADD(*pp_ace, pace);
		pace_group = pace;
	}

	if (!pace_other) {
		if ((pace = talloc(talloc_tos(), canon_ace)) == NULL) {
			DEBUG(0,("talloc fail.\n"));
			return false;
		}

		ZERO_STRUCTP(pace);
		pace->type = SMB_ACL_OTHER;
		pace->owner_type = WORLD_ACE;
		pace->unix_ug.type = ID_TYPE_NOT_SPECIFIED;
		pace->unix_ug.id = -1;
		pace->trustee = global_sid_World;
		pace->attr = ALLOW_ACE;
		pace->perms = 0;

		DLIST_ADD(*pp_ace, pace);
		pace_other = pace;
	}

	/* Ensure when setting a POSIX ACL, that the uid for a
	   SMB_ACL_USER_OBJ ACE (the owner ACE entry) has a duplicate
	   permission entry as an SMB_ACL_USER, and a gid for a
	   SMB_ACL_GROUP_OBJ ACE (the primary group ACE entry) also has
	   a duplicate permission entry as an SMB_ACL_GROUP. If not,
	   then if the ownership or group ownership of this file or
	   directory gets changed, the user or group can lose their
	   access. */

	for (pace = *pp_ace; pace; pace = pace->next) {
		if (pace->type == SMB_ACL_USER &&
				pace->unix_ug.id == pace_user->unix_ug.id) {
			/* Already got one. */
			got_duplicate_user = true;
		} else if (pace->type == SMB_ACL_GROUP &&
				pace->unix_ug.id == pace_group->unix_ug.id) {
			/* Already got one. */
			got_duplicate_group = true;
		} else if ((pace->type == SMB_ACL_GROUP)
			   && (dom_sid_equal(&pace->trustee, &pace_user->trustee))) {
			/* If the SID owning the file appears
			 * in a group entry, then we have
			 * enough duplication, they will still
			 * have access */
			got_duplicate_user = true;
		}
	}

	/* If the SID is equal for the user and group that we need
	   to add the duplicate for, add only the group */
	if (!got_duplicate_user && !got_duplicate_group
			&& dom_sid_equal(&pace_group->trustee,
					&pace_user->trustee)) {
		/* Add a duplicate SMB_ACL_GROUP entry, this
		 * will cover the owning SID as well, as it
		 * will always be mapped to both a uid and
		 * gid. */

		if ((pace = talloc(talloc_tos(), canon_ace)) == NULL) {
			DEBUG(0,("talloc fail.\n"));
			return false;
		}

		ZERO_STRUCTP(pace);
		pace->type = SMB_ACL_GROUP;;
		pace->owner_type = GID_ACE;
		pace->unix_ug.type = ID_TYPE_GID;
		pace->unix_ug.id = pace_group->unix_ug.id;
		pace->trustee = pace_group->trustee;
		pace->attr = pace_group->attr;
		pace->perms = pace_group->perms;

		DLIST_ADD(*pp_ace, pace);

		/* We're done here, make sure the
		   statements below are not executed. */
		got_duplicate_user = true;
		got_duplicate_group = true;
	}

	if (!got_duplicate_user) {
		/* Add a duplicate SMB_ACL_USER entry. */
		if ((pace = talloc(talloc_tos(), canon_ace)) == NULL) {
			DEBUG(0,("talloc fail.\n"));
			return false;
		}

		ZERO_STRUCTP(pace);
		pace->type = SMB_ACL_USER;;
		pace->owner_type = UID_ACE;
		pace->unix_ug.type = ID_TYPE_UID;
		pace->unix_ug.id = pace_user->unix_ug.id;
		pace->trustee = pace_user->trustee;
		pace->attr = pace_user->attr;
		pace->perms = pace_user->perms;

		DLIST_ADD(*pp_ace, pace);

		got_duplicate_user = true;
	}

	if (!got_duplicate_group) {
		/* Add a duplicate SMB_ACL_GROUP entry. */
		if ((pace = talloc(talloc_tos(), canon_ace)) == NULL) {
			DEBUG(0,("talloc fail.\n"));
			return false;
		}

		ZERO_STRUCTP(pace);
		pace->type = SMB_ACL_GROUP;;
		pace->owner_type = GID_ACE;
		pace->unix_ug.type = ID_TYPE_GID;
		pace->unix_ug.id = pace_group->unix_ug.id;
		pace->trustee = pace_group->trustee;
		pace->attr = pace_group->attr;
		pace->perms = pace_group->perms;

		DLIST_ADD(*pp_ace, pace);

		got_duplicate_group = true;
	}

	return true;
}

/****************************************************************************
 Check if a POSIX ACL has the required SMB_ACL_USER_OBJ and SMB_ACL_GROUP_OBJ entries.
 If it does not have them, check if there are any entries where the trustee is the
 file owner or the owning group, and map these to SMB_ACL_USER_OBJ and SMB_ACL_GROUP_OBJ.
 Note we must not do this to default directory ACLs.
****************************************************************************/

static void check_owning_objs(canon_ace *ace, struct dom_sid *pfile_owner_sid, struct dom_sid *pfile_grp_sid)
{
	bool got_user_obj, got_group_obj;
	canon_ace *current_ace;
	int i, entries;

	entries = count_canon_ace_list(ace);
	got_user_obj = False;
	got_group_obj = False;

	for (i=0, current_ace = ace; i < entries; i++, current_ace = current_ace->next) {
		if (current_ace->type == SMB_ACL_USER_OBJ)
			got_user_obj = True;
		else if (current_ace->type == SMB_ACL_GROUP_OBJ)
			got_group_obj = True;
	}
	if (got_user_obj && got_group_obj) {
		DEBUG(10,("check_owning_objs: ACL had owning user/group entries.\n"));
		return;
	}

	for (i=0, current_ace = ace; i < entries; i++, current_ace = current_ace->next) {
		if (!got_user_obj && current_ace->owner_type == UID_ACE &&
				dom_sid_equal(&current_ace->trustee, pfile_owner_sid)) {
			current_ace->type = SMB_ACL_USER_OBJ;
			got_user_obj = True;
		}
		if (!got_group_obj && current_ace->owner_type == GID_ACE &&
				dom_sid_equal(&current_ace->trustee, pfile_grp_sid)) {
			current_ace->type = SMB_ACL_GROUP_OBJ;
			got_group_obj = True;
		}
	}
	if (!got_user_obj)
		DEBUG(10,("check_owning_objs: ACL is missing an owner entry.\n"));
	if (!got_group_obj)
		DEBUG(10,("check_owning_objs: ACL is missing an owning group entry.\n"));
}

static bool add_current_ace_to_acl(files_struct *fsp, struct security_ace *psa,
				   canon_ace **file_ace, canon_ace **dir_ace,
				   bool *got_file_allow, bool *got_dir_allow,
				   bool *all_aces_are_inherit_only,
				   canon_ace *current_ace)
{

	/*
	 * Map the given NT permissions into a UNIX mode_t containing only
	 * S_I(R|W|X)USR bits.
	 */

	current_ace->perms |= map_nt_perms( &psa->access_mask, S_IRUSR);
	current_ace->attr = (psa->type == SEC_ACE_TYPE_ACCESS_ALLOWED) ? ALLOW_ACE : DENY_ACE;

	/* Store the ace_flag. */
	current_ace->ace_flags = psa->flags;

	/*
	 * Now add the created ace to either the file list, the directory
	 * list, or both. We *MUST* preserve the order here (hence we use
	 * DLIST_ADD_END) as NT ACLs are order dependent.
	 */

	if (fsp->fsp_flags.is_directory) {

		/*
		 * We can only add to the default POSIX ACE list if the ACE is
		 * designed to be inherited by both files and directories.
		 */

		if ((psa->flags & (SEC_ACE_FLAG_OBJECT_INHERIT|SEC_ACE_FLAG_CONTAINER_INHERIT)) ==
		    (SEC_ACE_FLAG_OBJECT_INHERIT|SEC_ACE_FLAG_CONTAINER_INHERIT)) {

			canon_ace *current_dir_ace = current_ace;
			DLIST_ADD_END(*dir_ace, current_ace);

			/*
			 * Note if this was an allow ace. We can't process
			 * any further deny ace's after this.
			 */

			if (current_ace->attr == ALLOW_ACE)
				*got_dir_allow = True;

			if ((current_ace->attr == DENY_ACE) && *got_dir_allow) {
				DEBUG(0,("add_current_ace_to_acl: "
					 "malformed ACL in "
					 "inheritable ACL! Deny entry "
					 "after Allow entry. Failing "
					 "to set on file %s.\n",
					 fsp_str_dbg(fsp)));
				return False;
			}

			if( DEBUGLVL( 10 )) {
				dbgtext("add_current_ace_to_acl: adding dir ACL:\n");
				print_canon_ace( current_ace, 0);
			}

			/*
			 * If this is not an inherit only ACE we need to add a duplicate
			 * to the file acl.
			 */

			if (!(psa->flags & SEC_ACE_FLAG_INHERIT_ONLY)) {
				canon_ace *dup_ace = dup_canon_ace(current_ace);

				if (!dup_ace) {
					DEBUG(0,("add_current_ace_to_acl: malloc fail !\n"));
					return False;
				}

				/*
				 * We must not free current_ace here as its
				 * pointer is now owned by the dir_ace list.
				 */
				current_ace = dup_ace;
				/* We've essentially split this ace into two,
				 * and added the ace with inheritance request
				 * bits to the directory ACL. Drop those bits for
				 * the ACE we're adding to the file list. */
				current_ace->ace_flags &= ~(SEC_ACE_FLAG_OBJECT_INHERIT|
							    SEC_ACE_FLAG_CONTAINER_INHERIT|
							    SEC_ACE_FLAG_INHERIT_ONLY);
			} else {
				/*
				 * We must not free current_ace here as its
				 * pointer is now owned by the dir_ace list.
				 */
				current_ace = NULL;
			}

			/*
			 * current_ace is now either owned by file_ace
			 * or is NULL. We can safely operate on current_dir_ace
			 * to treat mapping for default acl entries differently
			 * than access acl entries.
			 */

			if (current_dir_ace->owner_type == UID_ACE) {
				/*
				 * We already decided above this is a uid,
				 * for default acls ace's only CREATOR_OWNER
				 * maps to ACL_USER_OBJ. All other uid
				 * ace's are ACL_USER.
				 */
				if (dom_sid_equal(&current_dir_ace->trustee,
						  &global_sid_Creator_Owner)) {
					current_dir_ace->type = SMB_ACL_USER_OBJ;
				} else {
					current_dir_ace->type = SMB_ACL_USER;
				}
			}

			if (current_dir_ace->owner_type == GID_ACE) {
				/*
				 * We already decided above this is a gid,
				 * for default acls ace's only CREATOR_GROUP
				 * maps to ACL_GROUP_OBJ. All other uid
				 * ace's are ACL_GROUP.
				 */
				if (dom_sid_equal(&current_dir_ace->trustee,
						  &global_sid_Creator_Group)) {
					current_dir_ace->type = SMB_ACL_GROUP_OBJ;
				} else {
					current_dir_ace->type = SMB_ACL_GROUP;
				}
			}
		}
	}

	/*
	 * Only add to the file ACL if not inherit only.
	 */

	if (current_ace && !(psa->flags & SEC_ACE_FLAG_INHERIT_ONLY)) {
		DLIST_ADD_END(*file_ace, current_ace);

		/*
		 * Note if this was an allow ace. We can't process
		 * any further deny ace's after this.
		 */

		if (current_ace->attr == ALLOW_ACE)
			*got_file_allow = True;

		if ((current_ace->attr == DENY_ACE) && *got_file_allow) {
			DEBUG(0,("add_current_ace_to_acl: malformed "
				 "ACL in file ACL ! Deny entry after "
				 "Allow entry. Failing to set on file "
				 "%s.\n", fsp_str_dbg(fsp)));
			return False;
		}

		if( DEBUGLVL( 10 )) {
			dbgtext("add_current_ace_to_acl: adding file ACL:\n");
			print_canon_ace( current_ace, 0);
		}
		*all_aces_are_inherit_only = False;
		/*
		 * We must not free current_ace here as its
		 * pointer is now owned by the file_ace list.
		 */
		current_ace = NULL;
	}

	/*
	 * Free if ACE was not added.
	 */

	TALLOC_FREE(current_ace);
	return true;
}

/****************************************************************************
 Unpack a struct security_descriptor into two canonical ace lists.
****************************************************************************/

static bool create_canon_ace_lists(files_struct *fsp,
					const SMB_STRUCT_STAT *pst,
					struct dom_sid *pfile_owner_sid,
					struct dom_sid *pfile_grp_sid,
					canon_ace **ppfile_ace,
					canon_ace **ppdir_ace,
					const struct security_acl *dacl)
{
	bool all_aces_are_inherit_only = (fsp->fsp_flags.is_directory);
	canon_ace *file_ace = NULL;
	canon_ace *dir_ace = NULL;
	canon_ace *current_ace = NULL;
	bool got_dir_allow = False;
	bool got_file_allow = False;
	uint32_t i, j;

	*ppfile_ace = NULL;
	*ppdir_ace = NULL;

	/*
	 * Convert the incoming ACL into a more regular form.
	 */

	for(i = 0; i < dacl->num_aces; i++) {
		struct security_ace *psa = &dacl->aces[i];

		if((psa->type != SEC_ACE_TYPE_ACCESS_ALLOWED) && (psa->type != SEC_ACE_TYPE_ACCESS_DENIED)) {
			DEBUG(3,("create_canon_ace_lists: unable to set anything but an ALLOW or DENY ACE.\n"));
			return False;
		}
	}

	/*
	 * Deal with the fact that NT 4.x re-writes the canonical format
	 * that we return for default ACLs. If a directory ACE is identical
	 * to a inherited directory ACE then NT changes the bits so that the
	 * first ACE is set to OI|IO and the second ACE for this SID is set
	 * to CI. We need to repair this. JRA.
	 */

	for(i = 0; i < dacl->num_aces; i++) {
		struct security_ace *psa1 = &dacl->aces[i];

		for (j = i + 1; j < dacl->num_aces; j++) {
			struct security_ace *psa2 = &dacl->aces[j];

			if (psa1->access_mask != psa2->access_mask)
				continue;

			if (!dom_sid_equal(&psa1->trustee, &psa2->trustee))
				continue;

			/*
			 * Ok - permission bits and SIDs are equal.
			 * Check if flags were re-written.
			 */

			if (psa1->flags & SEC_ACE_FLAG_INHERIT_ONLY) {

				psa1->flags |= (psa2->flags & (SEC_ACE_FLAG_CONTAINER_INHERIT|SEC_ACE_FLAG_OBJECT_INHERIT));
				psa2->flags &= ~(SEC_ACE_FLAG_CONTAINER_INHERIT|SEC_ACE_FLAG_OBJECT_INHERIT);

			} else if (psa2->flags & SEC_ACE_FLAG_INHERIT_ONLY) {

				psa2->flags |= (psa1->flags & (SEC_ACE_FLAG_CONTAINER_INHERIT|SEC_ACE_FLAG_OBJECT_INHERIT));
				psa1->flags &= ~(SEC_ACE_FLAG_CONTAINER_INHERIT|SEC_ACE_FLAG_OBJECT_INHERIT);

			}
		}
	}

	for(i = 0; i < dacl->num_aces; i++) {
		struct security_ace *psa = &dacl->aces[i];

		/*
		 * Create a canon_ace entry representing this NT DACL ACE.
		 */

		if ((current_ace = talloc(talloc_tos(), canon_ace)) == NULL) {
			free_canon_ace_list(file_ace);
			free_canon_ace_list(dir_ace);
			DEBUG(0,("create_canon_ace_lists: malloc fail.\n"));
			return False;
		}

		ZERO_STRUCTP(current_ace);

		sid_copy(&current_ace->trustee, &psa->trustee);

		/*
		 * Try and work out if the SID is a user or group
		 * as we need to flag these differently for POSIX.
		 * Note what kind of a POSIX ACL this should map to.
		 */

		if( dom_sid_equal(&current_ace->trustee, &global_sid_World)) {
			current_ace->owner_type = WORLD_ACE;
			current_ace->unix_ug.type = ID_TYPE_NOT_SPECIFIED;
			current_ace->unix_ug.id = -1;
			current_ace->type = SMB_ACL_OTHER;
		} else if (dom_sid_equal(&current_ace->trustee, &global_sid_Creator_Owner)) {
			current_ace->owner_type = UID_ACE;
			current_ace->unix_ug.type = ID_TYPE_UID;
			current_ace->unix_ug.id = pst->st_ex_uid;
			current_ace->type = SMB_ACL_USER_OBJ;

			/*
			 * The Creator Owner entry only specifies inheritable permissions,
			 * never access permissions. WinNT doesn't always set the ACE to
			 * INHERIT_ONLY, though.
			 */

			psa->flags |= SEC_ACE_FLAG_INHERIT_ONLY;

		} else if (dom_sid_equal(&current_ace->trustee, &global_sid_Creator_Group)) {
			current_ace->owner_type = GID_ACE;
			current_ace->unix_ug.type = ID_TYPE_GID;
			current_ace->unix_ug.id = pst->st_ex_gid;
			current_ace->type = SMB_ACL_GROUP_OBJ;

			/*
			 * The Creator Group entry only specifies inheritable permissions,
			 * never access permissions. WinNT doesn't always set the ACE to
			 * INHERIT_ONLY, though.
			 */
			psa->flags |= SEC_ACE_FLAG_INHERIT_ONLY;

		} else {
			struct unixid unixid;

			if (!sids_to_unixids(&current_ace->trustee, 1, &unixid)) {
				struct dom_sid_buf buf;
				free_canon_ace_list(file_ace);
				free_canon_ace_list(dir_ace);
				TALLOC_FREE(current_ace);
				DBG_ERR("sids_to_unixids failed for %s "
					"(allocation failure)\n",
					dom_sid_str_buf(&current_ace->trustee,
							&buf));
				return false;
			}

			if (unixid.type == ID_TYPE_BOTH) {
				/*
				 * We must add both a user and group
				 * entry POSIX_ACL.
				 * This is due to the fact that in POSIX
				 * user entries are more specific than
				 * groups.
				 */
				current_ace->owner_type = UID_ACE;
				current_ace->unix_ug.type = ID_TYPE_UID;
				current_ace->unix_ug.id = unixid.id;
				current_ace->type =
					(unixid.id == pst->st_ex_uid) ?
						SMB_ACL_USER_OBJ :
						SMB_ACL_USER;

				/* Add the user object to the posix ACL,
				   and proceed to the group mapping
				   below. This handles the talloc_free
				   of current_ace if not added for some
				   reason */
				if (!add_current_ace_to_acl(fsp,
						psa,
						&file_ace,
						&dir_ace,
						&got_file_allow,
						&got_dir_allow,
						&all_aces_are_inherit_only,
						current_ace)) {
					free_canon_ace_list(file_ace);
					free_canon_ace_list(dir_ace);
					return false;
				}

				if ((current_ace = talloc(talloc_tos(),
						canon_ace)) == NULL) {
					free_canon_ace_list(file_ace);
					free_canon_ace_list(dir_ace);
					DEBUG(0,("create_canon_ace_lists: "
						"malloc fail.\n"));
					return False;
				}

				ZERO_STRUCTP(current_ace);

				sid_copy(&current_ace->trustee, &psa->trustee);

				current_ace->unix_ug.type = ID_TYPE_GID;
				current_ace->unix_ug.id = unixid.id;
				current_ace->owner_type = GID_ACE;
				/* If it's the primary group, this is a
				   group_obj, not a group. */
				if (current_ace->unix_ug.id == pst->st_ex_gid) {
					current_ace->type = SMB_ACL_GROUP_OBJ;
				} else {
					current_ace->type = SMB_ACL_GROUP;
				}

			} else if (unixid.type == ID_TYPE_UID) {
				current_ace->owner_type = UID_ACE;
				current_ace->unix_ug.type = ID_TYPE_UID;
				current_ace->unix_ug.id = unixid.id;
				/* If it's the owning user, this is a user_obj,
				   not a user. */
				if (current_ace->unix_ug.id == pst->st_ex_uid) {
					current_ace->type = SMB_ACL_USER_OBJ;
				} else {
					current_ace->type = SMB_ACL_USER;
				}
			} else if (unixid.type == ID_TYPE_GID) {
				current_ace->unix_ug.type = ID_TYPE_GID;
				current_ace->unix_ug.id = unixid.id;
				current_ace->owner_type = GID_ACE;
				/* If it's the primary group, this is a
				   group_obj, not a group. */
				if (current_ace->unix_ug.id == pst->st_ex_gid) {
					current_ace->type = SMB_ACL_GROUP_OBJ;
				} else {
					current_ace->type = SMB_ACL_GROUP;
				}
			} else {
				struct dom_sid_buf buf;
				/*
				 * Silently ignore map failures in non-mappable SIDs (NT Authority, BUILTIN etc).
				 */

				if (non_mappable_sid(&psa->trustee)) {
					DBG_DEBUG("ignoring "
						  "non-mappable SID %s\n",
						  dom_sid_str_buf(
							  &psa->trustee,
							  &buf));
					TALLOC_FREE(current_ace);
					continue;
				}

				if (lp_force_unknown_acl_user(SNUM(fsp->conn))) {
					DBG_DEBUG("ignoring unknown or "
						  "foreign SID %s\n",
						  dom_sid_str_buf(
							  &psa->trustee,
							  &buf));
					TALLOC_FREE(current_ace);
					continue;
				}

				free_canon_ace_list(file_ace);
				free_canon_ace_list(dir_ace);
				DBG_ERR("unable to map SID %s to uid or "
					"gid.\n",
					dom_sid_str_buf(&current_ace->trustee,
							&buf));
				TALLOC_FREE(current_ace);
				return false;
			}
		}

		/* handles the talloc_free of current_ace if not added for some reason */
		if (!add_current_ace_to_acl(fsp, psa, &file_ace, &dir_ace,
					    &got_file_allow, &got_dir_allow,
					    &all_aces_are_inherit_only, current_ace)) {
			free_canon_ace_list(file_ace);
			free_canon_ace_list(dir_ace);
			return false;
		}
	}

	if (fsp->fsp_flags.is_directory && all_aces_are_inherit_only) {
		/*
		 * Windows 2000 is doing one of these weird 'inherit acl'
		 * traverses to conserve NTFS ACL resources. Just pretend
		 * there was no DACL sent. JRA.
		 */

		DEBUG(10,("create_canon_ace_lists: Win2k inherit acl traverse. Ignoring DACL.\n"));
		free_canon_ace_list(file_ace);
		free_canon_ace_list(dir_ace);
		file_ace = NULL;
		dir_ace = NULL;
	} else {
		/*
		 * Check if we have SMB_ACL_USER_OBJ and SMB_ACL_GROUP_OBJ entries in
		 * the file ACL. If we don't have them, check if any SMB_ACL_USER/SMB_ACL_GROUP
		 * entries can be converted to *_OBJ. Don't do this for the default
		 * ACL, we will create them separately for this if needed inside
		 * ensure_canon_entry_valid_on_set().
		 */
		if (file_ace) {
			check_owning_objs(file_ace, pfile_owner_sid, pfile_grp_sid);
		}
	}

	*ppfile_ace = file_ace;
	*ppdir_ace = dir_ace;

	return True;
}

/****************************************************************************
 ASCII art time again... JRA :-).

 We have 4 cases to process when moving from an NT ACL to a POSIX ACL. Firstly,
 we insist the ACL is in canonical form (ie. all DENY entries precede ALLOW
 entries). Secondly, the merge code has ensured that all duplicate SID entries for
 allow or deny have been merged, so the same SID can only appear once in the deny
 list or once in the allow list.

 We then process as follows :

 ---------------------------------------------------------------------------
 First pass - look for a Everyone DENY entry.

 If it is deny all (rwx) trunate the list at this point.
 Else, walk the list from this point and use the deny permissions of this
 entry as a mask on all following allow entries. Finally, delete
 the Everyone DENY entry (we have applied it to everything possible).

 In addition, in this pass we remove any DENY entries that have
 no permissions (ie. they are a DENY nothing).
 ---------------------------------------------------------------------------
 Second pass - only deal with deny user entries.

 DENY user1 (perms XXX)

 new_perms = 0
 for all following allow group entries where user1 is in group
	new_perms |= group_perms;

 user1 entry perms = new_perms & ~ XXX;

 Convert the deny entry to an allow entry with the new perms and
 push to the end of the list. Note if the user was in no groups
 this maps to a specific allow nothing entry for this user.

 The common case from the NT ACL chooser (userX deny all) is
 optimised so we don't do the group lookup - we just map to
 an allow nothing entry.

 What we're doing here is inferring the allow permissions the
 person setting the ACE on user1 wanted by looking at the allow
 permissions on the groups the user is currently in. This will
 be a snapshot, depending on group membership but is the best
 we can do and has the advantage of failing closed rather than
 open.
 ---------------------------------------------------------------------------
 Third pass - only deal with deny group entries.

 DENY group1 (perms XXX)

 for all following allow user entries where user is in group1
   user entry perms = user entry perms & ~ XXX;

 If there is a group Everyone allow entry with permissions YYY,
 convert the group1 entry to an allow entry and modify its
 permissions to be :

 new_perms = YYY & ~ XXX

 and push to the end of the list.

 If there is no group Everyone allow entry then convert the
 group1 entry to a allow nothing entry and push to the end of the list.

 Note that the common case from the NT ACL chooser (groupX deny all)
 cannot be optimised here as we need to modify user entries who are
 in the group to change them to a deny all also.

 What we're doing here is modifying the allow permissions of
 user entries (which are more specific in POSIX ACLs) to mask
 out the explicit deny set on the group they are in. This will
 be a snapshot depending on current group membership but is the
 best we can do and has the advantage of failing closed rather
 than open.
 ---------------------------------------------------------------------------
 Fourth pass - cope with cumulative permissions.

 for all allow user entries, if there exists an allow group entry with
 more permissive permissions, and the user is in that group, rewrite the
 allow user permissions to contain both sets of permissions.

 Currently the code for this is #ifdef'ed out as these semantics make
 no sense to me. JRA.
 ---------------------------------------------------------------------------

 Note we *MUST* do the deny user pass first as this will convert deny user
 entries into allow user entries which can then be processed by the deny
 group pass.

 The above algorithm took a *lot* of thinking about - hence this
 explanation :-). JRA.
****************************************************************************/

/****************************************************************************
 Process a canon_ace list entries. This is very complex code. We need
 to go through and remove the "deny" permissions from any allow entry that matches
 the id of this entry. We have already refused any NT ACL that wasn't in correct
 order (DENY followed by ALLOW). If any allow entry ends up with zero permissions,
 we just remove it (to fail safe). We have already removed any duplicate ace
 entries. Treat an "Everyone" DENY_ACE as a special case - use it to mask all
 allow entries.
****************************************************************************/

static void process_deny_list(connection_struct *conn, canon_ace **pp_ace_list )
{
	canon_ace *ace_list = *pp_ace_list;
	canon_ace *curr_ace = NULL;
	canon_ace *curr_ace_next = NULL;

	/* Pass 1 above - look for an Everyone, deny entry. */

	for (curr_ace = ace_list; curr_ace; curr_ace = curr_ace_next) {
		canon_ace *allow_ace_p;

		curr_ace_next = curr_ace->next; /* So we can't lose the link. */

		if (curr_ace->attr != DENY_ACE)
			continue;

		if (curr_ace->perms == (mode_t)0) {

			/* Deny nothing entry - delete. */

			DLIST_REMOVE(ace_list, curr_ace);
			continue;
		}

		if (!dom_sid_equal(&curr_ace->trustee, &global_sid_World))
			continue;

		/* JRATEST - assert. */
		SMB_ASSERT(curr_ace->owner_type == WORLD_ACE);

		if (curr_ace->perms == ALL_ACE_PERMS) {

			/*
			 * Optimisation. This is a DENY_ALL to Everyone. Truncate the
			 * list at this point including this entry.
			 */

			canon_ace *prev_entry = DLIST_PREV(curr_ace);

			free_canon_ace_list( curr_ace );
			if (prev_entry)
				DLIST_REMOVE(ace_list, prev_entry);
			else {
				/* We deleted the entire list. */
				ace_list = NULL;
			}
			break;
		}

		for (allow_ace_p = curr_ace->next; allow_ace_p; allow_ace_p = allow_ace_p->next) {

			/*
			 * Only mask off allow entries.
			 */

			if (allow_ace_p->attr != ALLOW_ACE)
				continue;

			allow_ace_p->perms &= ~curr_ace->perms;
		}

		/*
		 * Now it's been applied, remove it.
		 */

		DLIST_REMOVE(ace_list, curr_ace);
	}

	/* Pass 2 above - deal with deny user entries. */

	for (curr_ace = ace_list; curr_ace; curr_ace = curr_ace_next) {
		mode_t new_perms = (mode_t)0;
		canon_ace *allow_ace_p;

		curr_ace_next = curr_ace->next; /* So we can't lose the link. */

		if (curr_ace->attr != DENY_ACE)
			continue;

		if (curr_ace->owner_type != UID_ACE)
			continue;

		if (curr_ace->perms == ALL_ACE_PERMS) {

			/*
			 * Optimisation - this is a deny everything to this user.
			 * Convert to an allow nothing and push to the end of the list.
			 */

			curr_ace->attr = ALLOW_ACE;
			curr_ace->perms = (mode_t)0;
			DLIST_DEMOTE(ace_list, curr_ace);
			continue;
		}

		for (allow_ace_p = curr_ace->next; allow_ace_p; allow_ace_p = allow_ace_p->next) {

			if (allow_ace_p->attr != ALLOW_ACE)
				continue;

			/* We process GID_ACE and WORLD_ACE entries only. */

			if (allow_ace_p->owner_type == UID_ACE)
				continue;

			if (uid_entry_in_group(conn, curr_ace, allow_ace_p))
				new_perms |= allow_ace_p->perms;
		}

		/*
		 * Convert to a allow entry, modify the perms and push to the end
		 * of the list.
		 */

		curr_ace->attr = ALLOW_ACE;
		curr_ace->perms = (new_perms & ~curr_ace->perms);
		DLIST_DEMOTE(ace_list, curr_ace);
	}

	/* Pass 3 above - deal with deny group entries. */

	for (curr_ace = ace_list; curr_ace; curr_ace = curr_ace_next) {
		canon_ace *allow_ace_p;
		canon_ace *allow_everyone_p = NULL;

		curr_ace_next = curr_ace->next; /* So we can't lose the link. */

		if (curr_ace->attr != DENY_ACE)
			continue;

		if (curr_ace->owner_type != GID_ACE)
			continue;

		for (allow_ace_p = curr_ace->next; allow_ace_p; allow_ace_p = allow_ace_p->next) {

			if (allow_ace_p->attr != ALLOW_ACE)
				continue;

			/* Store a pointer to the Everyone allow, if it exists. */
			if (allow_ace_p->owner_type == WORLD_ACE)
				allow_everyone_p = allow_ace_p;

			/* We process UID_ACE entries only. */

			if (allow_ace_p->owner_type != UID_ACE)
				continue;

			/* Mask off the deny group perms. */

			if (uid_entry_in_group(conn, allow_ace_p, curr_ace))
				allow_ace_p->perms &= ~curr_ace->perms;
		}

		/*
		 * Convert the deny to an allow with the correct perms and
		 * push to the end of the list.
		 */

		curr_ace->attr = ALLOW_ACE;
		if (allow_everyone_p)
			curr_ace->perms = allow_everyone_p->perms & ~curr_ace->perms;
		else
			curr_ace->perms = (mode_t)0;
		DLIST_DEMOTE(ace_list, curr_ace);
	}

	/* Doing this fourth pass allows Windows semantics to be layered
	 * on top of POSIX semantics. I'm not sure if this is desirable.
	 * For example, in W2K ACLs there is no way to say, "Group X no
	 * access, user Y full access" if user Y is a member of group X.
	 * This seems completely broken semantics to me.... JRA.
	 */

#if 0
	/* Pass 4 above - deal with allow entries. */

	for (curr_ace = ace_list; curr_ace; curr_ace = curr_ace_next) {
		canon_ace *allow_ace_p;

		curr_ace_next = curr_ace->next; /* So we can't lose the link. */

		if (curr_ace->attr != ALLOW_ACE)
			continue;

		if (curr_ace->owner_type != UID_ACE)
			continue;

		for (allow_ace_p = ace_list; allow_ace_p; allow_ace_p = allow_ace_p->next) {

			if (allow_ace_p->attr != ALLOW_ACE)
				continue;

			/* We process GID_ACE entries only. */

			if (allow_ace_p->owner_type != GID_ACE)
				continue;

			/* OR in the group perms. */

			if (uid_entry_in_group(conn, curr_ace, allow_ace_p))
				curr_ace->perms |= allow_ace_p->perms;
		}
	}
#endif

	*pp_ace_list = ace_list;
}

/****************************************************************************
 Unpack a struct security_descriptor into two canonical ace lists. We don't depend on this
 succeeding.
****************************************************************************/

static bool unpack_canon_ace(files_struct *fsp,
				const SMB_STRUCT_STAT *pst,
				struct dom_sid *pfile_owner_sid,
				struct dom_sid *pfile_grp_sid,
				canon_ace **ppfile_ace,
				canon_ace **ppdir_ace,
				uint32_t security_info_sent,
				const struct security_descriptor *psd)
{
	canon_ace *file_ace = NULL;
	canon_ace *dir_ace = NULL;
	bool ok;

	*ppfile_ace = NULL;
	*ppdir_ace = NULL;

	if(security_info_sent == 0) {
		DEBUG(0,("unpack_canon_ace: no security info sent !\n"));
		return False;
	}

	/*
	 * If no DACL then this is a chown only security descriptor.
	 */

	if(!(security_info_sent & SECINFO_DACL) || !psd->dacl)
		return True;

	/*
	 * Now go through the DACL and create the canon_ace lists.
	 */

	if (!create_canon_ace_lists(fsp, pst, pfile_owner_sid, pfile_grp_sid,
				    &file_ace, &dir_ace, psd->dacl)) {
		return False;
	}

	if ((file_ace == NULL) && (dir_ace == NULL)) {
		/* W2K traverse DACL set - ignore. */
		return True;
	}

	/*
	 * Go through the canon_ace list and merge entries
	 * belonging to identical users of identical allow or deny type.
	 * We can do this as all deny entries come first, followed by
	 * all allow entries (we have mandated this before accepting this acl).
	 */

	print_canon_ace_list( "file ace - before merge", file_ace);
	merge_aces( &file_ace, false);

	print_canon_ace_list( "dir ace - before merge", dir_ace);
	merge_aces( &dir_ace, true);

	/*
	 * NT ACLs are order dependent. Go through the acl lists and
	 * process DENY entries by masking the allow entries.
	 */

	print_canon_ace_list( "file ace - before deny", file_ace);
	process_deny_list(fsp->conn, &file_ace);

	print_canon_ace_list( "dir ace - before deny", dir_ace);
	process_deny_list(fsp->conn, &dir_ace);

	/*
	 * A well formed POSIX file or default ACL has at least 3 entries, a
	 * SMB_ACL_USER_OBJ, SMB_ACL_GROUP_OBJ, SMB_ACL_OTHER_OBJ
	 * and optionally a mask entry. Ensure this is the case.
	 */

	print_canon_ace_list( "file ace - before valid", file_ace);

	ok = ensure_canon_entry_valid_on_set(
		fsp->conn,
		&file_ace,
		false,
		fsp->conn->params,
		fsp->fsp_flags.is_directory,
		pfile_owner_sid,
		pfile_grp_sid,
		pst);
	if (!ok) {
		free_canon_ace_list(file_ace);
		free_canon_ace_list(dir_ace);
		return False;
	}

	print_canon_ace_list( "dir ace - before valid", dir_ace);

	if (dir_ace != NULL) {
		ok = ensure_canon_entry_valid_on_set(
			fsp->conn,
			&dir_ace,
			true,
			fsp->conn->params,
			fsp->fsp_flags.is_directory,
			pfile_owner_sid,
			pfile_grp_sid,
			pst);
		if (!ok) {
			free_canon_ace_list(file_ace);
			free_canon_ace_list(dir_ace);
			return False;
		}
	}

	print_canon_ace_list( "file ace - return", file_ace);
	print_canon_ace_list( "dir ace - return", dir_ace);

	*ppfile_ace = file_ace;
	*ppdir_ace = dir_ace;
	return True;

}

/******************************************************************************
 When returning permissions, try and fit NT display
 semantics if possible. Note that the canon_entries here must have been malloced.
 The list format should be - first entry = owner, followed by group and other user
 entries, last entry = other.

 Note that this doesn't exactly match the NT semantics for an ACL. As POSIX entries
 are not ordered, and match on the most specific entry rather than walking a list,
 then a simple POSIX permission of rw-r--r-- should really map to 5 entries,

 Entry 0: owner : deny all except read and write.
 Entry 1: owner : allow read and write.
 Entry 2: group : deny all except read.
 Entry 3: group : allow read.
 Entry 4: Everyone : allow read.

 But NT cannot display this in their ACL editor !
********************************************************************************/

static void arrange_posix_perms(const char *filename, canon_ace **pp_list_head)
{
	canon_ace *l_head = *pp_list_head;
	canon_ace *owner_ace = NULL;
	canon_ace *other_ace = NULL;
	canon_ace *ace = NULL;

	for (ace = l_head; ace; ace = ace->next) {
		if (ace->type == SMB_ACL_USER_OBJ)
			owner_ace = ace;
		else if (ace->type == SMB_ACL_OTHER) {
			/* Last ace - this is "other" */
			other_ace = ace;
		}
	}

	if (!owner_ace || !other_ace) {
		DEBUG(0,("arrange_posix_perms: Invalid POSIX permissions for file %s, missing owner or other.\n",
			filename ));
		return;
	}

	/*
	 * The POSIX algorithm applies to owner first, and other last,
	 * so ensure they are arranged in this order.
	 */

	if (owner_ace) {
		DLIST_PROMOTE(l_head, owner_ace);
	}

	if (other_ace) {
		DLIST_DEMOTE(l_head, other_ace);
	}

	/* We have probably changed the head of the list. */

	*pp_list_head = l_head;
}

/****************************************************************************
 Create a linked list of canonical ACE entries.
****************************************************************************/

static canon_ace *canonicalise_acl(struct connection_struct *conn,
				   const char *fname, SMB_ACL_T posix_acl,
				   const SMB_STRUCT_STAT *psbuf,
				   const struct dom_sid *powner, const struct dom_sid *pgroup, struct pai_val *pal, SMB_ACL_TYPE_T the_acl_type)
{
	mode_t acl_mask = (S_IRUSR|S_IWUSR|S_IXUSR);
	canon_ace *l_head = NULL;
	canon_ace *ace = NULL;
	canon_ace *next_ace = NULL;
	int entry_id = SMB_ACL_FIRST_ENTRY;
	bool is_default_acl = (the_acl_type == SMB_ACL_TYPE_DEFAULT);
	SMB_ACL_ENTRY_T entry;
	size_t ace_count;

	while ( posix_acl && (sys_acl_get_entry(posix_acl, entry_id, &entry) == 1)) {
		SMB_ACL_TAG_T tagtype;
		SMB_ACL_PERMSET_T permset;
		struct dom_sid sid;
		struct unixid unix_ug;
		enum ace_owner owner_type;

		entry_id = SMB_ACL_NEXT_ENTRY;

		if (sys_acl_get_tag_type(entry, &tagtype) == -1)
			continue;

		if (sys_acl_get_permset(entry, &permset) == -1)
			continue;

		/* Decide which SID to use based on the ACL type. */
		switch(tagtype) {
			case SMB_ACL_USER_OBJ:
				/* Get the SID from the owner. */
				sid_copy(&sid, powner);
				unix_ug.type = ID_TYPE_UID;
				unix_ug.id = psbuf->st_ex_uid;
				owner_type = UID_ACE;
				break;
			case SMB_ACL_USER:
				{
					uid_t *puid = (uid_t *)sys_acl_get_qualifier(entry);
					if (puid == NULL) {
						DEBUG(0,("canonicalise_acl: Failed to get uid.\n"));
						continue;
					}
					uid_to_sid( &sid, *puid);
					unix_ug.type = ID_TYPE_UID;
					unix_ug.id = *puid;
					owner_type = UID_ACE;
					break;
				}
			case SMB_ACL_GROUP_OBJ:
				/* Get the SID from the owning group. */
				sid_copy(&sid, pgroup);
				unix_ug.type = ID_TYPE_GID;
				unix_ug.id = psbuf->st_ex_gid;
				owner_type = GID_ACE;
				break;
			case SMB_ACL_GROUP:
				{
					gid_t *pgid = (gid_t *)sys_acl_get_qualifier(entry);
					if (pgid == NULL) {
						DEBUG(0,("canonicalise_acl: Failed to get gid.\n"));
						continue;
					}
					gid_to_sid( &sid, *pgid);
					unix_ug.type = ID_TYPE_GID;
					unix_ug.id = *pgid;
					owner_type = GID_ACE;
					break;
				}
			case SMB_ACL_MASK:
				acl_mask = convert_permset_to_mode_t(permset);
				continue; /* Don't count the mask as an entry. */
			case SMB_ACL_OTHER:
				/* Use the Everyone SID */
				sid = global_sid_World;
				unix_ug.type = ID_TYPE_NOT_SPECIFIED;
				unix_ug.id = -1;
				owner_type = WORLD_ACE;
				break;
			default:
				DEBUG(0,("canonicalise_acl: Unknown tagtype %u\n", (unsigned int)tagtype));
				continue;
		}

		/*
		 * Add this entry to the list.
		 */

		if ((ace = talloc(talloc_tos(), canon_ace)) == NULL)
			goto fail;

		*ace = (canon_ace) {
			.type = tagtype,
			.perms = convert_permset_to_mode_t(permset),
			.attr = ALLOW_ACE,
			.trustee = sid,
			.unix_ug = unix_ug,
			.owner_type = owner_type
		};
		ace->ace_flags = get_pai_flags(pal, ace, is_default_acl);

		DLIST_ADD(l_head, ace);
	}

	/*
	 * This next call will ensure we have at least a user/group/world set.
	 */

	if (!ensure_canon_entry_valid_on_get(conn, &l_head,
				      powner, pgroup,
				      psbuf))
		goto fail;

	/*
	 * Now go through the list, masking the permissions with the
	 * acl_mask. Ensure all DENY Entries are at the start of the list.
	 */

	DEBUG(10,("canonicalise_acl: %s ace entries before arrange :\n", is_default_acl ?  "Default" : "Access"));

	for ( ace_count = 0, ace = l_head; ace; ace = next_ace, ace_count++) {
		next_ace = ace->next;

		/* Masks are only applied to entries other than USER_OBJ and OTHER. */
		if (ace->type != SMB_ACL_OTHER && ace->type != SMB_ACL_USER_OBJ)
			ace->perms &= acl_mask;

		if (ace->perms == 0) {
			DLIST_PROMOTE(l_head, ace);
		}

		if( DEBUGLVL( 10 ) ) {
			print_canon_ace(ace, ace_count);
		}
	}

	arrange_posix_perms(fname,&l_head );

	print_canon_ace_list( "canonicalise_acl: ace entries after arrange", l_head );

	return l_head;

  fail:

	free_canon_ace_list(l_head);
	return NULL;
}

/****************************************************************************
 Check if the current user group list contains a given group.
****************************************************************************/

bool current_user_in_group(connection_struct *conn, gid_t gid)
{
	uint32_t i;
	const struct security_unix_token *utok = get_current_utok(conn);

	for (i = 0; i < utok->ngroups; i++) {
		if (utok->groups[i] == gid) {
			return True;
		}
	}

	return False;
}

/****************************************************************************
 Should we override a deny ? Check 'acl group control' and 'dos filemode'.
****************************************************************************/

static bool acl_group_override_fsp(files_struct *fsp)
{
	if ((errno != EPERM) && (errno != EACCES)) {
		return false;
	}

	/* file primary group == user primary or supplementary group */
	if (lp_acl_group_control(SNUM(fsp->conn)) &&
	    current_user_in_group(fsp->conn, fsp->fsp_name->st.st_ex_gid)) {
		return true;
	}

	/* user has writeable permission */
	if (lp_dos_filemode(SNUM(fsp->conn)) && can_write_to_fsp(fsp)) {
		return true;
	}

	return false;
}

/****************************************************************************
 Attempt to apply an ACL to a file or directory.
****************************************************************************/

static bool set_canon_ace_list(files_struct *fsp,
				canon_ace *the_ace,
				bool default_ace,
				const SMB_STRUCT_STAT *psbuf,
				bool *pacl_set_support)
{
	bool ret = False;
	SMB_ACL_T the_acl = sys_acl_init(talloc_tos());
	canon_ace *p_ace;
	int i;
	SMB_ACL_ENTRY_T mask_entry;
	bool got_mask_entry = False;
	SMB_ACL_PERMSET_T mask_permset;
	SMB_ACL_TYPE_T the_acl_type = (default_ace ? SMB_ACL_TYPE_DEFAULT : SMB_ACL_TYPE_ACCESS);
	bool needs_mask = False;
	int sret;

	/* Use the psbuf that was passed in. */
	if (psbuf != &fsp->fsp_name->st) {
		fsp->fsp_name->st = *psbuf;
	}

#if defined(POSIX_ACL_NEEDS_MASK)
	/* HP-UX always wants to have a mask (called "class" there). */
	needs_mask = True;
#endif

	if (the_acl == NULL) {
		DEBUG(0, ("sys_acl_init failed to allocate an ACL\n"));
		return false;
	}

	if( DEBUGLVL( 10 )) {
		dbgtext("set_canon_ace_list: setting ACL:\n");
		for (i = 0, p_ace = the_ace; p_ace; p_ace = p_ace->next, i++ ) {
			print_canon_ace( p_ace, i);
		}
	}

	for (i = 0, p_ace = the_ace; p_ace; p_ace = p_ace->next, i++ ) {
		SMB_ACL_ENTRY_T the_entry;
		SMB_ACL_PERMSET_T the_permset;

		/*
		 * ACLs only "need" an ACL_MASK entry if there are any named user or
		 * named group entries. But if there is an ACL_MASK entry, it applies
		 * to ACL_USER, ACL_GROUP, and ACL_GROUP_OBJ entries. Set the mask
		 * so that it doesn't deny (i.e., mask off) any permissions.
		 */

		if (p_ace->type == SMB_ACL_USER || p_ace->type == SMB_ACL_GROUP) {
			needs_mask = True;
		}

		/*
		 * Get the entry for this ACE.
		 */

		if (sys_acl_create_entry(&the_acl, &the_entry) == -1) {
			DEBUG(0,("set_canon_ace_list: Failed to create entry %d. (%s)\n",
				i, strerror(errno) ));
			goto fail;
		}

		if (p_ace->type == SMB_ACL_MASK) {
			mask_entry = the_entry;
			got_mask_entry = True;
		}

		/*
		 * Ok - we now know the ACL calls should be working, don't
		 * allow fallback to chmod.
		 */

		*pacl_set_support = True;

		/*
		 * Initialise the entry from the canon_ace.
		 */

		/*
		 * First tell the entry what type of ACE this is.
		 */

		if (sys_acl_set_tag_type(the_entry, p_ace->type) == -1) {
			DEBUG(0,("set_canon_ace_list: Failed to set tag type on entry %d. (%s)\n",
				i, strerror(errno) ));
			goto fail;
		}

		/*
		 * Only set the qualifier (user or group id) if the entry is a user
		 * or group id ACE.
		 */

		if ((p_ace->type == SMB_ACL_USER) || (p_ace->type == SMB_ACL_GROUP)) {
			if (sys_acl_set_qualifier(the_entry,(void *)&p_ace->unix_ug.id) == -1) {
				DEBUG(0,("set_canon_ace_list: Failed to set qualifier on entry %d. (%s)\n",
					i, strerror(errno) ));
				goto fail;
			}
		}

		/*
		 * Convert the mode_t perms in the canon_ace to a POSIX permset.
		 */

		if (sys_acl_get_permset(the_entry, &the_permset) == -1) {
			DEBUG(0,("set_canon_ace_list: Failed to get permset on entry %d. (%s)\n",
				i, strerror(errno) ));
			goto fail;
		}

		if (map_acl_perms_to_permset(p_ace->perms, &the_permset) == -1) {
			DEBUG(0,("set_canon_ace_list: Failed to create permset for mode (%u) on entry %d. (%s)\n",
				(unsigned int)p_ace->perms, i, strerror(errno) ));
			goto fail;
		}

		/*
		 * ..and apply them to the entry.
		 */

		if (sys_acl_set_permset(the_entry, the_permset) == -1) {
			DEBUG(0,("set_canon_ace_list: Failed to add permset on entry %d. (%s)\n",
				i, strerror(errno) ));
			goto fail;
		}

		if( DEBUGLVL( 10 ))
			print_canon_ace( p_ace, i);

	}

	if (needs_mask && !got_mask_entry) {
		if (sys_acl_create_entry(&the_acl, &mask_entry) == -1) {
			DEBUG(0,("set_canon_ace_list: Failed to create mask entry. (%s)\n", strerror(errno) ));
			goto fail;
		}

		if (sys_acl_set_tag_type(mask_entry, SMB_ACL_MASK) == -1) {
			DEBUG(0,("set_canon_ace_list: Failed to set tag type on mask entry. (%s)\n",strerror(errno) ));
			goto fail;
		}

		if (sys_acl_get_permset(mask_entry, &mask_permset) == -1) {
			DEBUG(0,("set_canon_ace_list: Failed to get mask permset. (%s)\n", strerror(errno) ));
			goto fail;
		}

		if (map_acl_perms_to_permset(S_IRUSR|S_IWUSR|S_IXUSR, &mask_permset) == -1) {
			DEBUG(0,("set_canon_ace_list: Failed to create mask permset. (%s)\n", strerror(errno) ));
			goto fail;
		}

		if (sys_acl_set_permset(mask_entry, mask_permset) == -1) {
			DEBUG(0,("set_canon_ace_list: Failed to add mask permset. (%s)\n", strerror(errno) ));
			goto fail;
		}
	}

	/*
	 * Finally apply it to the file or directory.
	 */
	sret = SMB_VFS_SYS_ACL_SET_FD(fsp, the_acl_type, the_acl);
	if (sret == -1) {
		/*
		 * Some systems allow all the above calls and only fail with no ACL support
		 * when attempting to apply the acl. HPUX with HFS is an example of this. JRA.
		 */
		if (no_acl_syscall_error(errno)) {
			*pacl_set_support = false;
		}

		if (acl_group_override_fsp(fsp)) {
			DBG_DEBUG("acl group control on and current user in "
				  "file [%s] primary group.\n",
				  fsp_str_dbg(fsp));

			become_root();
			sret = SMB_VFS_SYS_ACL_SET_FD(fsp,
						      the_acl_type,
						      the_acl);
			unbecome_root();
			if (sret == 0) {
				ret = true;
			}
		}

		if (ret == false) {
			DBG_WARNING("sys_acl_set_file on file [%s]: (%s)\n",
				    fsp_str_dbg(fsp), strerror(errno));
			goto fail;
		}
	}

	ret = True;

  fail:

	if (the_acl != NULL) {
		TALLOC_FREE(the_acl);
	}

	return ret;
}

/****************************************************************************

****************************************************************************/

SMB_ACL_T free_empty_sys_acl(connection_struct *conn, SMB_ACL_T the_acl)
{
	SMB_ACL_ENTRY_T entry;

	if (!the_acl)
		return NULL;
	if (sys_acl_get_entry(the_acl, SMB_ACL_FIRST_ENTRY, &entry) != 1) {
		TALLOC_FREE(the_acl);
		return NULL;
	}
	return the_acl;
}

/****************************************************************************
 Convert a canon_ace to a generic 3 element permission - if possible.
****************************************************************************/

#define MAP_PERM(p,mask,result) (((p) & (mask)) ? (result) : 0 )

static bool convert_canon_ace_to_posix_perms( files_struct *fsp, canon_ace *file_ace_list, mode_t *posix_perms)
{
	size_t ace_count = count_canon_ace_list(file_ace_list);
	canon_ace *ace_p;
	canon_ace *owner_ace = NULL;
	canon_ace *group_ace = NULL;
	canon_ace *other_ace = NULL;

	if (ace_count > 5) {
		DEBUG(3,("convert_canon_ace_to_posix_perms: Too many ACE "
			 "entries for file %s to convert to posix perms.\n",
			 fsp_str_dbg(fsp)));
		return False;
	}

	for (ace_p = file_ace_list; ace_p; ace_p = ace_p->next) {
		if (ace_p->owner_type == UID_ACE)
			owner_ace = ace_p;
		else if (ace_p->owner_type == GID_ACE)
			group_ace = ace_p;
		else if (ace_p->owner_type == WORLD_ACE)
			other_ace = ace_p;
	}

	if (!owner_ace || !group_ace || !other_ace) {
		DEBUG(3,("convert_canon_ace_to_posix_perms: Can't get "
			 "standard entries for file %s.\n", fsp_str_dbg(fsp)));
		return False;
	}

	/*
	 * Ensure all ACE entries are owner, group or other.
	 * We can't set if there are any other SIDs.
	 */
	for (ace_p = file_ace_list; ace_p; ace_p = ace_p->next) {
		if (ace_p == owner_ace || ace_p == group_ace ||
				ace_p == other_ace) {
			continue;
		}
		if (ace_p->owner_type == UID_ACE) {
			if (ace_p->unix_ug.id != owner_ace->unix_ug.id) {
				DEBUG(3,("Invalid uid %u in ACE for file %s.\n",
					(unsigned int)ace_p->unix_ug.id,
					fsp_str_dbg(fsp)));
				return false;
			}
		} else if (ace_p->owner_type == GID_ACE) {
			if (ace_p->unix_ug.id != group_ace->unix_ug.id) {
				DEBUG(3,("Invalid gid %u in ACE for file %s.\n",
					(unsigned int)ace_p->unix_ug.id,
					fsp_str_dbg(fsp)));
				return false;
			}
		} else {
			/*
			 * There should be no duplicate WORLD_ACE entries.
			 */

			DEBUG(3,("Invalid type %u, uid %u in "
				"ACE for file %s.\n",
				(unsigned int)ace_p->owner_type,
				(unsigned int)ace_p->unix_ug.id,
				fsp_str_dbg(fsp)));
			return false;
		}
	}

	*posix_perms = (mode_t)0;

	*posix_perms |= owner_ace->perms;
	*posix_perms |= MAP_PERM(group_ace->perms, S_IRUSR, S_IRGRP);
	*posix_perms |= MAP_PERM(group_ace->perms, S_IWUSR, S_IWGRP);
	*posix_perms |= MAP_PERM(group_ace->perms, S_IXUSR, S_IXGRP);
	*posix_perms |= MAP_PERM(other_ace->perms, S_IRUSR, S_IROTH);
	*posix_perms |= MAP_PERM(other_ace->perms, S_IWUSR, S_IWOTH);
	*posix_perms |= MAP_PERM(other_ace->perms, S_IXUSR, S_IXOTH);

	/* The owner must have at least read access. */

	*posix_perms |= S_IRUSR;
	if (fsp->fsp_flags.is_directory)
		*posix_perms |= (S_IWUSR|S_IXUSR);

	DEBUG(10,("convert_canon_ace_to_posix_perms: converted u=%o,g=%o,w=%o "
		  "to perm=0%o for file %s.\n", (int)owner_ace->perms,
		  (int)group_ace->perms, (int)other_ace->perms,
		  (int)*posix_perms, fsp_str_dbg(fsp)));

	return True;
}

/****************************************************************************
  Incoming NT ACLs on a directory can be split into a default POSIX acl (CI|OI|IO) and
  a normal POSIX acl. Win2k needs these split acls re-merging into one ACL
  with CI|OI set so it is inherited and also applies to the directory.
  Based on code from "Jim McDonough" <jmcd@us.ibm.com>.
****************************************************************************/

static size_t merge_default_aces( struct security_ace *nt_ace_list, size_t num_aces)
{
	size_t i, j;

	for (i = 0; i < num_aces; i++) {
		for (j = i+1; j < num_aces; j++) {
			uint32_t i_flags_ni = (nt_ace_list[i].flags & ~SEC_ACE_FLAG_INHERITED_ACE);
			uint32_t j_flags_ni = (nt_ace_list[j].flags & ~SEC_ACE_FLAG_INHERITED_ACE);
			bool i_inh = (nt_ace_list[i].flags & SEC_ACE_FLAG_INHERITED_ACE) ? True : False;
			bool j_inh = (nt_ace_list[j].flags & SEC_ACE_FLAG_INHERITED_ACE) ? True : False;

			/* We know the lower number ACE's are file entries. */
			if ((nt_ace_list[i].type == nt_ace_list[j].type) &&
				(nt_ace_list[i].size == nt_ace_list[j].size) &&
				(nt_ace_list[i].access_mask == nt_ace_list[j].access_mask) &&
				dom_sid_equal(&nt_ace_list[i].trustee, &nt_ace_list[j].trustee) &&
				(i_inh == j_inh) &&
				(i_flags_ni == 0) &&
				(j_flags_ni == (SEC_ACE_FLAG_OBJECT_INHERIT|
						  SEC_ACE_FLAG_CONTAINER_INHERIT|
						  SEC_ACE_FLAG_INHERIT_ONLY))) {
				/*
				 * W2K wants to have access allowed zero access ACE's
				 * at the end of the list. If the mask is zero, merge
				 * the non-inherited ACE onto the inherited ACE.
				 */

				if (nt_ace_list[i].access_mask == 0) {
					nt_ace_list[j].flags = SEC_ACE_FLAG_OBJECT_INHERIT|SEC_ACE_FLAG_CONTAINER_INHERIT|
								(i_inh ? SEC_ACE_FLAG_INHERITED_ACE : 0);
					ARRAY_DEL_ELEMENT(nt_ace_list, i, num_aces);

					DEBUG(10,("merge_default_aces: Merging zero access ACE %u onto ACE %u.\n",
						(unsigned int)i, (unsigned int)j ));

					/*
					 * If we remove the i'th element, we
					 * should decrement i so that we don't
					 * skip over the succeeding element.
					*/
					i--;
					num_aces--;
					break;
				} else {
					/*
					 * These are identical except for the flags.
					 * Merge the inherited ACE onto the non-inherited ACE.
					 */

					nt_ace_list[i].flags = SEC_ACE_FLAG_OBJECT_INHERIT|SEC_ACE_FLAG_CONTAINER_INHERIT|
								(i_inh ? SEC_ACE_FLAG_INHERITED_ACE : 0);
					ARRAY_DEL_ELEMENT(nt_ace_list, j, num_aces);

					DEBUG(10,("merge_default_aces: Merging ACE %u onto ACE %u.\n",
						(unsigned int)j, (unsigned int)i ));

					/*
					 * If we remove the j'th element, we
					 * should decrement j and continue
					 * around the loop, so as not to skip
					 * subsequent elements.
					 */
					j--;
					num_aces--;
				}
			}
		}
	}

	return num_aces;
}


/****************************************************************************
 Reply to query a security descriptor from an fsp. If it succeeds it allocates
 the space for the return elements and returns the size needed to return the
 security descriptor. This should be the only external function needed for
 the UNIX style get ACL.
****************************************************************************/

static NTSTATUS posix_get_nt_acl_common(struct connection_struct *conn,
				      const char *name,
				      const SMB_STRUCT_STAT *sbuf,
				      struct pai_val *pal,
				      SMB_ACL_T posix_acl,
				      SMB_ACL_T def_acl,
				      uint32_t security_info,
				      TALLOC_CTX *mem_ctx,
				      struct security_descriptor **ppdesc)
{
	struct dom_sid owner_sid;
	struct dom_sid group_sid;
	size_t sd_size = 0;
	struct security_acl *psa = NULL;
	size_t num_acls = 0;
	size_t num_def_acls = 0;
	size_t num_aces = 0;
	canon_ace *file_ace = NULL;
	canon_ace *dir_ace = NULL;
	struct security_ace *nt_ace_list = NULL;
	struct security_descriptor *psd = NULL;

	/*
	 * Get the owner, group and world SIDs.
	 */

	create_file_sids(sbuf, &owner_sid, &group_sid);

	if (security_info & SECINFO_DACL) {

		/*
		 * In the optimum case Creator Owner and Creator Group would be used for
		 * the ACL_USER_OBJ and ACL_GROUP_OBJ entries, respectively, but this
		 * would lead to usability problems under Windows: The Creator entries
		 * are only available in browse lists of directories and not for files;
		 * additionally the identity of the owning group couldn't be determined.
		 * We therefore use those identities only for Default ACLs.
		 */

		/* Create the canon_ace lists. */
		file_ace = canonicalise_acl(conn, name, posix_acl, sbuf,
					    &owner_sid, &group_sid, pal,
					    SMB_ACL_TYPE_ACCESS);

		/* We must have *some* ACLS. */

		if (count_canon_ace_list(file_ace) == 0) {
			DEBUG(0,("get_nt_acl : No ACLs on file (%s) !\n", name));
			goto done;
		}

		if (S_ISDIR(sbuf->st_ex_mode) && def_acl) {
			dir_ace = canonicalise_acl(conn, name, def_acl,
						   sbuf,
						   &global_sid_Creator_Owner,
						   &global_sid_Creator_Group,
						   pal, SMB_ACL_TYPE_DEFAULT);
		}

		/*
		 * Create the NT ACE list from the canonical ace lists.
		 */

		{
			canon_ace *ace;
			enum security_ace_type nt_acl_type;

			num_acls = count_canon_ace_list(file_ace);
			num_def_acls = count_canon_ace_list(dir_ace);

			nt_ace_list = talloc_zero_array(
				talloc_tos(), struct security_ace,
				num_acls + num_def_acls);

			if (nt_ace_list == NULL) {
				DEBUG(0,("get_nt_acl: Unable to malloc space for nt_ace_list.\n"));
				goto done;
			}

			/*
			 * Create the NT ACE list from the canonical ace lists.
			 */

			for (ace = file_ace; ace != NULL; ace = ace->next) {
				uint32_t acc = map_canon_ace_perms(SNUM(conn),
						&nt_acl_type,
						ace->perms,
						S_ISDIR(sbuf->st_ex_mode));
				init_sec_ace(&nt_ace_list[num_aces++],
					&ace->trustee,
					nt_acl_type,
					acc,
					ace->ace_flags);
			}

			for (ace = dir_ace; ace != NULL; ace = ace->next) {
				uint32_t acc = map_canon_ace_perms(SNUM(conn),
						&nt_acl_type,
						ace->perms,
						S_ISDIR(sbuf->st_ex_mode));
				init_sec_ace(&nt_ace_list[num_aces++],
					&ace->trustee,
					nt_acl_type,
					acc,
					ace->ace_flags |
					SEC_ACE_FLAG_OBJECT_INHERIT|
					SEC_ACE_FLAG_CONTAINER_INHERIT|
					SEC_ACE_FLAG_INHERIT_ONLY);
			}

			/*
			 * Merge POSIX default ACLs and normal ACLs into one NT ACE.
			 * Win2K needs this to get the inheritance correct when replacing ACLs
			 * on a directory tree. Based on work by Jim @ IBM.
			 */

			num_aces = merge_default_aces(nt_ace_list, num_aces);
		}

		if (num_aces) {
			if((psa = make_sec_acl( talloc_tos(), NT4_ACL_REVISION, num_aces, nt_ace_list)) == NULL) {
				DEBUG(0,("get_nt_acl: Unable to malloc space for acl.\n"));
				goto done;
			}
		}
	} /* security_info & SECINFO_DACL */

	psd = make_standard_sec_desc(mem_ctx,
			(security_info & SECINFO_OWNER) ? &owner_sid : NULL,
			(security_info & SECINFO_GROUP) ? &group_sid : NULL,
			psa,
			&sd_size);

	if(!psd) {
		DEBUG(0,("get_nt_acl: Unable to malloc space for security descriptor.\n"));
		sd_size = 0;
		goto done;
	}

	/*
	 * Windows 2000: The DACL_PROTECTED flag in the security
	 * descriptor marks the ACL as non-inheriting, i.e., no
	 * ACEs from higher level directories propagate to this
	 * ACL. In the POSIX ACL model permissions are only
	 * inherited at file create time, so ACLs never contain
	 * any ACEs that are inherited dynamically. The DACL_PROTECTED
	 * flag doesn't seem to bother Windows NT.
	 * Always set this if map acl inherit is turned off.
	 */
	if (pal == NULL || !lp_map_acl_inherit(SNUM(conn))) {
		psd->type |= SEC_DESC_DACL_PROTECTED;
	} else {
		psd->type |= pal->sd_type;
	}

	if (psd->dacl) {
		dacl_sort_into_canonical_order(psd->dacl->aces, (unsigned int)psd->dacl->num_aces);
	}

	*ppdesc = psd;

 done:

	if (posix_acl) {
		TALLOC_FREE(posix_acl);
	}
	if (def_acl) {
		TALLOC_FREE(def_acl);
	}
	free_canon_ace_list(file_ace);
	free_canon_ace_list(dir_ace);
	free_inherited_info(pal);
	TALLOC_FREE(nt_ace_list);

	return NT_STATUS_OK;
}

NTSTATUS posix_fget_nt_acl(struct files_struct *fsp, uint32_t security_info,
			   TALLOC_CTX *mem_ctx,
			   struct security_descriptor **ppdesc)
{
	SMB_STRUCT_STAT sbuf;
	SMB_ACL_T posix_acl = NULL;
	SMB_ACL_T def_acl = NULL;
	struct pai_val *pal;
	TALLOC_CTX *frame = talloc_stackframe();
	NTSTATUS status;

	*ppdesc = NULL;

	DEBUG(10,("posix_fget_nt_acl: called for file %s\n",
		  fsp_str_dbg(fsp)));

	/* Get the stat struct for the owner info. */
	if(SMB_VFS_FSTAT(fsp, &sbuf) != 0) {
		TALLOC_FREE(frame);
		return map_nt_error_from_unix(errno);
	}

	/* Get the ACL from the fd. */
	posix_acl = SMB_VFS_SYS_ACL_GET_FD(fsp,
					   SMB_ACL_TYPE_ACCESS,
					   frame);

	/* If it's a directory get the default POSIX ACL. */
	if(fsp->fsp_flags.is_directory) {
		def_acl = SMB_VFS_SYS_ACL_GET_FD(fsp,
						 SMB_ACL_TYPE_DEFAULT,
						 frame);
		def_acl = free_empty_sys_acl(fsp->conn, def_acl);
	}

	pal = fload_inherited_info(fsp);

	status = posix_get_nt_acl_common(fsp->conn, fsp->fsp_name->base_name,
					 &sbuf, pal, posix_acl, def_acl,
					 security_info, mem_ctx, ppdesc);
	TALLOC_FREE(frame);
	return status;
}

/****************************************************************************
 Try to chown a file. We will be able to chown it under the following conditions.

  1) If we have root privileges, then it will just work.
  2) If we have SeRestorePrivilege we can change the user + group to any other user.
  3) If we have SeTakeOwnershipPrivilege we can change the user to the current user.
  4) If we have write permission to the file and dos_filemodes is set
     then allow chown to the currently authenticated user.
****************************************************************************/

static NTSTATUS try_chown(files_struct *fsp, uid_t uid, gid_t gid)
{
	NTSTATUS status;
	int ret;

	if(!CAN_WRITE(fsp->conn)) {
		return NT_STATUS_MEDIA_WRITE_PROTECTED;
	}

	/* Case (1). */
	ret = SMB_VFS_FCHOWN(fsp, uid, gid);
	if (ret == 0) {
		return NT_STATUS_OK;
	}

	/* Case (2) / (3) */
	if (lp_enable_privileges()) {
		bool has_take_ownership_priv = security_token_has_privilege(
						get_current_nttok(fsp->conn),
						SEC_PRIV_TAKE_OWNERSHIP);
		bool has_restore_priv = security_token_has_privilege(
						get_current_nttok(fsp->conn),
						SEC_PRIV_RESTORE);

		if (has_restore_priv) {
			; /* Case (2) */
		} else if (has_take_ownership_priv) {
			/* Case (3) */
			if (uid == get_current_uid(fsp->conn)) {
				gid = (gid_t)-1;
			} else {
				has_take_ownership_priv = false;
			}
		}

		if (has_take_ownership_priv || has_restore_priv) {
			status = NT_STATUS_OK;
			become_root();
			ret = SMB_VFS_FCHOWN(fsp, uid, gid);
			if (ret != 0) {
				status = map_nt_error_from_unix(errno);
			}
			unbecome_root();
			return status;
		}
	}

	/* Case (4). */
	/* If "dos filemode" isn't set, we're done. */
	if (!lp_dos_filemode(SNUM(fsp->conn))) {
		return NT_STATUS_ACCESS_DENIED;
	}
	/*
	 * If we have a writable handle, obviously we
	 * can write to the file.
	 */
	if (!fsp->fsp_flags.can_write) {
		/*
		 * If we don't have a writable handle, we
		 * need to read the ACL on the file to
		 * see if we can write to it.
		 */
		if (!can_write_to_fsp(fsp)) {
			return NT_STATUS_ACCESS_DENIED;
		}
	}

	/* only allow chown to the current user. This is more secure,
	   and also copes with the case where the SID in a take ownership ACL is
	   a local SID on the users workstation
	*/
	if (uid != get_current_uid(fsp->conn)) {
		return NT_STATUS_INVALID_OWNER;
	}

	status = NT_STATUS_OK;
	become_root();
	/* Keep the current file gid the same. */
	ret = SMB_VFS_FCHOWN(fsp, uid, (gid_t)-1);
	if (ret != 0) {
		status = map_nt_error_from_unix(errno);
	}
	unbecome_root();

	return status;
}

/*
 * Check whether a chown is needed and if so, attempt the chown
 * A returned error indicates that the chown failed.
 * NT_STATUS_OK with did_chown == false indicates that the chown was skipped.
 * NT_STATUS_OK with did_chown == true indicates that the chown succeeded
 */
NTSTATUS chown_if_needed(files_struct *fsp, uint32_t security_info_sent,
			 const struct security_descriptor *psd,
			 bool *did_chown)
{
	NTSTATUS status;
	uid_t uid = (uid_t)-1;
	gid_t gid = (gid_t)-1;

	status = unpack_nt_owners(fsp->conn, &uid, &gid, security_info_sent, psd);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	if (((uid == (uid_t)-1) || (fsp->fsp_name->st.st_ex_uid == uid)) &&
	    ((gid == (gid_t)-1) || (fsp->fsp_name->st.st_ex_gid == gid))) {
		/*
		 * Skip chown
		 */
		*did_chown = false;
		return NT_STATUS_OK;
	}

	DBG_NOTICE("chown %s. uid = %u, gid = %u.\n",
		   fsp_str_dbg(fsp), (unsigned int) uid, (unsigned int)gid);

	status = try_chown(fsp, uid, gid);
	if (!NT_STATUS_IS_OK(status)) {
		DBG_INFO("chown %s, %u, %u failed. Error = %s.\n",
			 fsp_str_dbg(fsp), (unsigned int) uid,
			 (unsigned int)gid, nt_errstr(status));
		return status;
	}

	/*
	 * Recheck the current state of the file, which may have changed.
	 * (owner and suid/sgid bits, for instance)
	 */

	status = vfs_stat_fsp(fsp);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	*did_chown = true;
	return NT_STATUS_OK;
}

/****************************************************************************
 Reply to set a security descriptor on an fsp. security_info_sent is the
 description of the following NT ACL.
 This should be the only external function needed for the UNIX style set ACL.
 We make a copy of psd_orig as internal functions modify the elements inside
 it, even though it's a const pointer.
****************************************************************************/

NTSTATUS set_nt_acl(files_struct *fsp, uint32_t security_info_sent, const struct security_descriptor *psd_orig)
{
	connection_struct *conn = fsp->conn;
	struct dom_sid file_owner_sid;
	struct dom_sid file_grp_sid;
	canon_ace *file_ace_list = NULL;
	canon_ace *dir_ace_list = NULL;
	bool acl_perms = False;
	mode_t orig_mode = (mode_t)0;
	NTSTATUS status;
	bool set_acl_as_root = false;
	bool acl_set_support = false;
	bool ret = false;
	struct security_descriptor *psd = NULL;

	DEBUG(10,("set_nt_acl: called for file %s\n",
		  fsp_str_dbg(fsp)));

	if (!CAN_WRITE(conn)) {
		DEBUG(10,("set acl rejected on read-only share\n"));
		return NT_STATUS_MEDIA_WRITE_PROTECTED;
	}

	if (psd_orig == NULL) {
		return NT_STATUS_INVALID_PARAMETER;
	}

	/*
	 * MS NFS mode, here's the deal: the client merely wants to
	 * modify the mode, but roundtripping get_acl/set/acl would
	 * add additional POSIX ACEs.  So in case we get a request
	 * containing a MS NFS mode SID, we do nothing here.
	 */
	if (security_descriptor_with_ms_nfs(psd_orig)) {
		return NT_STATUS_OK;
	}

	psd = security_descriptor_copy(talloc_tos(), psd_orig);
	if (psd == NULL) {
		return NT_STATUS_NO_MEMORY;
	}

	/*
	 * Get the current state of the file.
	 */

	status = vfs_stat_fsp(fsp);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	/* Save the original element we check against. */
	orig_mode = fsp->fsp_name->st.st_ex_mode;

	/*
	 * Unpack the user/group/world id's.
	 */

	/* POSIX can't cope with missing owner/group. */
	if ((security_info_sent & SECINFO_OWNER) && (psd->owner_sid == NULL)) {
		security_info_sent &= ~SECINFO_OWNER;
	}
	if ((security_info_sent & SECINFO_GROUP) && (psd->group_sid == NULL)) {
		security_info_sent &= ~SECINFO_GROUP;
	}

	/* If UNIX owner is inherited and Windows isn't, then
	 * setting the UNIX owner based on Windows owner conflicts
	 * with the inheritance rule
	 */
	if (lp_inherit_owner(SNUM(conn)) == INHERIT_OWNER_UNIX_ONLY) {
		security_info_sent &= ~SECINFO_OWNER;
	}

	/*
	 * Do we need to chown ? If so this must be done first as the incoming
	 * CREATOR_OWNER acl will be relative to the *new* owner, not the old.
	 * Noticed by Simo.
	 *
	 * If we successfully chowned, we know we must be able to set
	 * the acl, so do it as root (set_acl_as_root).
	 */
	status = chown_if_needed(fsp, security_info_sent, psd, &set_acl_as_root);
	if (!NT_STATUS_IS_OK(status)) {
		return status;
	}

	create_file_sids(&fsp->fsp_name->st, &file_owner_sid, &file_grp_sid);

	if((security_info_sent & SECINFO_DACL) &&
			(psd->type & SEC_DESC_DACL_PRESENT) &&
			(psd->dacl == NULL)) {
		struct security_ace ace[3];

		/* We can't have NULL DACL in POSIX.
		   Use owner/group/Everyone -> full access. */

		init_sec_ace(&ace[0],
				&file_owner_sid,
				SEC_ACE_TYPE_ACCESS_ALLOWED,
				GENERIC_ALL_ACCESS,
				0);
		init_sec_ace(&ace[1],
				&file_grp_sid,
				SEC_ACE_TYPE_ACCESS_ALLOWED,
				GENERIC_ALL_ACCESS,
				0);
		init_sec_ace(&ace[2],
				&global_sid_World,
				SEC_ACE_TYPE_ACCESS_ALLOWED,
				GENERIC_ALL_ACCESS,
				0);
		psd->dacl = make_sec_acl(talloc_tos(),
					NT4_ACL_REVISION,
					3,
					ace);
		if (psd->dacl == NULL) {
			return NT_STATUS_NO_MEMORY;
		}
		security_acl_map_generic(psd->dacl, &file_generic_mapping);
	}

	acl_perms = unpack_canon_ace(fsp, &fsp->fsp_name->st, &file_owner_sid,
				     &file_grp_sid, &file_ace_list,
				     &dir_ace_list, security_info_sent, psd);

	/* Ignore W2K traverse DACL set. */
	if (!file_ace_list && !dir_ace_list) {
		return NT_STATUS_OK;
	}

	if (!acl_perms) {
		DEBUG(3,("set_nt_acl: cannot set permissions\n"));
		free_canon_ace_list(file_ace_list);
		free_canon_ace_list(dir_ace_list);
		return NT_STATUS_ACCESS_DENIED;
	}

	/*
	 * Only change security if we got a DACL.
	 */

	if(!(security_info_sent & SECINFO_DACL) || (psd->dacl == NULL)) {
		free_canon_ace_list(file_ace_list);
		free_canon_ace_list(dir_ace_list);
		return NT_STATUS_OK;
	}

	/*
	 * Try using the POSIX ACL set first. Fall back to chmod if
	 * we have no ACL support on this filesystem.
	 */

	if (acl_perms && file_ace_list) {
		if (set_acl_as_root) {
			become_root();
		}
		ret = set_canon_ace_list(fsp, file_ace_list, false,
					 &fsp->fsp_name->st, &acl_set_support);
		if (set_acl_as_root) {
			unbecome_root();
		}
		if (acl_set_support && ret == false) {
			DEBUG(3,("set_nt_acl: failed to set file acl on file "
				 "%s (%s).\n", fsp_str_dbg(fsp),
				 strerror(errno)));
			free_canon_ace_list(file_ace_list);
			free_canon_ace_list(dir_ace_list);
			return map_nt_error_from_unix(errno);
		}
	}

	if (acl_perms && acl_set_support && fsp->fsp_flags.is_directory) {
		if (dir_ace_list) {
			if (set_acl_as_root) {
				become_root();
			}
			ret = set_canon_ace_list(fsp, dir_ace_list, true,
						 &fsp->fsp_name->st,
						 &acl_set_support);
			if (set_acl_as_root) {
				unbecome_root();
			}
			if (ret == false) {
				DEBUG(3,("set_nt_acl: failed to set default "
					 "acl on directory %s (%s).\n",
					 fsp_str_dbg(fsp), strerror(errno)));
				free_canon_ace_list(file_ace_list);
				free_canon_ace_list(dir_ace_list);
				return map_nt_error_from_unix(errno);
			}
		} else {
			int sret = -1;

			/*
			 * No default ACL - delete one if it exists.
			 */

			if (set_acl_as_root) {
				become_root();
			}
			sret = SMB_VFS_SYS_ACL_DELETE_DEF_FD(fsp);
			if (set_acl_as_root) {
				unbecome_root();
			}
			if (sret == -1) {
				if (acl_group_override_fsp(fsp)) {
					DEBUG(5,("set_nt_acl: acl group "
						 "control on and current user "
						 "in file %s primary group. "
						 "Override delete_def_acl\n",
						 fsp_str_dbg(fsp)));

					become_root();
					sret =
					    SMB_VFS_SYS_ACL_DELETE_DEF_FD(fsp);
					unbecome_root();
				}

				if (sret == -1) {
					DBG_NOTICE("sys_acl_delete_def_fd for "
						"directory %s failed (%s)\n",
						fsp_str_dbg(fsp),
						strerror(errno));
					free_canon_ace_list(file_ace_list);
					free_canon_ace_list(dir_ace_list);
					return map_nt_error_from_unix(errno);
				}
			}
		}
	}

	if (acl_set_support) {
		if (set_acl_as_root) {
			become_root();
		}
		store_inheritance_attributes(fsp,
				file_ace_list,
				dir_ace_list,
				psd->type);
		if (set_acl_as_root) {
			unbecome_root();
		}
	}

	/*
	 * If we cannot set using POSIX ACLs we fall back to checking if we need to chmod.
	 */

	if(!acl_set_support && acl_perms) {
		mode_t posix_perms;

		if (!convert_canon_ace_to_posix_perms( fsp, file_ace_list, &posix_perms)) {
			free_canon_ace_list(file_ace_list);
			free_canon_ace_list(dir_ace_list);
			DEBUG(3,("set_nt_acl: failed to convert file acl to "
				 "posix permissions for file %s.\n",
				 fsp_str_dbg(fsp)));
			return NT_STATUS_ACCESS_DENIED;
		}

		if (orig_mode != posix_perms) {
			int sret = -1;

			DEBUG(3,("set_nt_acl: chmod %s. perms = 0%o.\n",
				 fsp_str_dbg(fsp), (unsigned int)posix_perms));

			if (set_acl_as_root) {
				become_root();
			}
			sret = SMB_VFS_FCHMOD(fsp, posix_perms);
			if (set_acl_as_root) {
				unbecome_root();
			}
			if(sret == -1) {
				if (acl_group_override_fsp(fsp)) {
					DEBUG(5,("set_nt_acl: acl group "
						 "control on and current user "
						 "in file %s primary group. "
						 "Override chmod\n",
						 fsp_str_dbg(fsp)));

					become_root();
					sret = SMB_VFS_FCHMOD(fsp, posix_perms);
					unbecome_root();
				}

				if (sret == -1) {
					DEBUG(3,("set_nt_acl: chmod %s, 0%o "
						 "failed. Error = %s.\n",
						 fsp_str_dbg(fsp),
						 (unsigned int)posix_perms,
						 strerror(errno)));
					free_canon_ace_list(file_ace_list);
					free_canon_ace_list(dir_ace_list);
					return map_nt_error_from_unix(errno);
				}
			}
		}
	}

	free_canon_ace_list(file_ace_list);
	free_canon_ace_list(dir_ace_list);

	/* Ensure the stat struct in the fsp is correct. */
	status = vfs_stat_fsp(fsp);

	return NT_STATUS_OK;
}

/****************************************************************************
 Get the actual group bits stored on a file with an ACL. Has no effect if
 the file has no ACL. Needed in dosmode code where the stat() will return
 the mask bits, not the real group bits, for a file with an ACL.
****************************************************************************/

int get_acl_group_bits(connection_struct *conn,
		       struct files_struct *fsp,
		       mode_t *mode )
{
	int entry_id = SMB_ACL_FIRST_ENTRY;
	SMB_ACL_ENTRY_T entry;
	SMB_ACL_T posix_acl;
	int result = -1;

	posix_acl = SMB_VFS_SYS_ACL_GET_FD(metadata_fsp(fsp),
					   SMB_ACL_TYPE_ACCESS,
					   talloc_tos());
	if (posix_acl == (SMB_ACL_T)NULL)
		return -1;

	while (sys_acl_get_entry(posix_acl, entry_id, &entry) == 1) {
		SMB_ACL_TAG_T tagtype;
		SMB_ACL_PERMSET_T permset;

		entry_id = SMB_ACL_NEXT_ENTRY;

		if (sys_acl_get_tag_type(entry, &tagtype) ==-1)
			break;

		if (tagtype == SMB_ACL_GROUP_OBJ) {
			if (sys_acl_get_permset(entry, &permset) == -1) {
				break;
			} else {
				*mode &= ~(S_IRGRP|S_IWGRP|S_IXGRP);
				*mode |= (sys_acl_get_perm(permset, SMB_ACL_READ) ? S_IRGRP : 0);
				*mode |= (sys_acl_get_perm(permset, SMB_ACL_WRITE) ? S_IWGRP : 0);
				*mode |= (sys_acl_get_perm(permset, SMB_ACL_EXECUTE) ? S_IXGRP : 0);
				result = 0;
				break;
			}
		}
	}
	TALLOC_FREE(posix_acl);
	return result;
}

/****************************************************************************
 Do a chmod by setting the ACL USER_OBJ, GROUP_OBJ and OTHER bits in an ACL
 and set the mask to rwx. Needed to preserve complex ACLs set by NT.
****************************************************************************/

static int chmod_acl_internals(SMB_ACL_T posix_acl, mode_t mode)
{
	int entry_id = SMB_ACL_FIRST_ENTRY;
	SMB_ACL_ENTRY_T entry;
	int num_entries = 0;

	while ( sys_acl_get_entry(posix_acl, entry_id, &entry) == 1) {
		SMB_ACL_TAG_T tagtype;
		SMB_ACL_PERMSET_T permset;
		mode_t perms;

		entry_id = SMB_ACL_NEXT_ENTRY;

		if (sys_acl_get_tag_type(entry, &tagtype) == -1)
			return -1;

		if (sys_acl_get_permset(entry, &permset) == -1)
			return -1;

		num_entries++;

		switch(tagtype) {
			case SMB_ACL_USER_OBJ:
				perms = unix_perms_to_acl_perms(mode, S_IRUSR, S_IWUSR, S_IXUSR);
				break;
			case SMB_ACL_GROUP_OBJ:
				perms = unix_perms_to_acl_perms(mode, S_IRGRP, S_IWGRP, S_IXGRP);
				break;
			case SMB_ACL_MASK:
				/*
				 * FIXME: The ACL_MASK entry permissions should really be set to
				 * the union of the permissions of all ACL_USER,
				 * ACL_GROUP_OBJ, and ACL_GROUP entries. That's what
				 * acl_calc_mask() does, but Samba ACLs doesn't provide it.
				 */
				perms = S_IRUSR|S_IWUSR|S_IXUSR;
				break;
			case SMB_ACL_OTHER:
				perms = unix_perms_to_acl_perms(mode, S_IROTH, S_IWOTH, S_IXOTH);
				break;
			default:
				continue;
		}

		if (map_acl_perms_to_permset(perms, &permset) == -1)
			return -1;

		if (sys_acl_set_permset(entry, permset) == -1)
			return -1;
	}

	/*
	 * If this is a simple 3 element ACL or no elements then it's a standard
	 * UNIX permission set. Just use chmod...
	 */

	if ((num_entries == 3) || (num_entries == 0))
		return -1;

	return 0;
}

/****************************************************************************
 Get the access ACL of FROM, do a chmod by setting the ACL USER_OBJ,
 GROUP_OBJ and OTHER bits in an ACL and set the mask to rwx. Set the
 resulting ACL on TO.  Note that name is in UNIX character set.
****************************************************************************/

static int copy_access_posix_acl(struct files_struct *from,
				 struct files_struct *to,
				 mode_t mode)
{
	SMB_ACL_T posix_acl = NULL;
	int ret = -1;

	posix_acl = SMB_VFS_SYS_ACL_GET_FD(
		from, SMB_ACL_TYPE_ACCESS, talloc_tos());
	if (posix_acl == NULL) {
		return -1;
	}

	ret = chmod_acl_internals(posix_acl, mode);
	if (ret == -1) {
		goto done;
	}

	ret = SMB_VFS_SYS_ACL_SET_FD(metadata_fsp(to),
				     SMB_ACL_TYPE_ACCESS,
				     posix_acl);

 done:

	TALLOC_FREE(posix_acl);
	return ret;
}

/****************************************************************************
 Check for an existing default POSIX ACL on a directory.
****************************************************************************/

static bool directory_has_default_posix_acl(struct files_struct *dirfsp)
{
	SMB_ACL_T def_acl = SMB_VFS_SYS_ACL_GET_FD(
		dirfsp, SMB_ACL_TYPE_DEFAULT, talloc_tos());
	bool has_acl = False;
	SMB_ACL_ENTRY_T entry;

	if (def_acl != NULL && (sys_acl_get_entry(def_acl, SMB_ACL_FIRST_ENTRY, &entry) == 1)) {
		has_acl = True;
	}

	if (def_acl) {
	        TALLOC_FREE(def_acl);
	}
        return has_acl;
}

/****************************************************************************
 If the parent directory has no default ACL but it does have an Access ACL,
 inherit this Access ACL to file name.
****************************************************************************/

int inherit_access_posix_acl(connection_struct *conn,
			     struct files_struct *inherit_from_dirfsp,
			     const struct smb_filename *smb_fname,
			     mode_t mode)
{
	int ret;

	if (directory_has_default_posix_acl(inherit_from_dirfsp))
		return 0;

	ret = copy_access_posix_acl(
		inherit_from_dirfsp, smb_fname->fsp, mode);
	return ret;
}

/****************************************************************************
 Map from wire type to permset.
****************************************************************************/

static bool unix_ex_wire_to_permset(connection_struct *conn, unsigned char wire_perm, SMB_ACL_PERMSET_T *p_permset)
{
	if (wire_perm & ~(SMB_POSIX_ACL_READ|SMB_POSIX_ACL_WRITE|SMB_POSIX_ACL_EXECUTE)) {
		return False;
	}

	if (sys_acl_clear_perms(*p_permset) ==  -1) {
		return False;
	}

	if (wire_perm & SMB_POSIX_ACL_READ) {
		if (sys_acl_add_perm(*p_permset, SMB_ACL_READ) == -1) {
			return False;
		}
	}
	if (wire_perm & SMB_POSIX_ACL_WRITE) {
		if (sys_acl_add_perm(*p_permset, SMB_ACL_WRITE) == -1) {
			return False;
		}
	}
	if (wire_perm & SMB_POSIX_ACL_EXECUTE) {
		if (sys_acl_add_perm(*p_permset, SMB_ACL_EXECUTE) == -1) {
			return False;
		}
	}
	return True;
}

/****************************************************************************
 Map from wire type to tagtype.
****************************************************************************/

static bool unix_ex_wire_to_tagtype(unsigned char wire_tt, SMB_ACL_TAG_T *p_tt)
{
	switch (wire_tt) {
		case SMB_POSIX_ACL_USER_OBJ:
			*p_tt = SMB_ACL_USER_OBJ;
			break;
		case SMB_POSIX_ACL_USER:
			*p_tt = SMB_ACL_USER;
			break;
		case SMB_POSIX_ACL_GROUP_OBJ:
			*p_tt = SMB_ACL_GROUP_OBJ;
			break;
		case SMB_POSIX_ACL_GROUP:
			*p_tt = SMB_ACL_GROUP;
			break;
		case SMB_POSIX_ACL_MASK:
			*p_tt = SMB_ACL_MASK;
			break;
		case SMB_POSIX_ACL_OTHER:
			*p_tt = SMB_ACL_OTHER;
			break;
		default:
			return False;
	}
	return True;
}

/****************************************************************************
 Create a new POSIX acl from wire permissions.
 FIXME ! How does the share mask/mode fit into this.... ?
****************************************************************************/

static SMB_ACL_T create_posix_acl_from_wire(connection_struct *conn,
					    uint16_t num_acls,
					    const char *pdata,
					    TALLOC_CTX *mem_ctx)
{
	unsigned int i;
	SMB_ACL_T the_acl = sys_acl_init(mem_ctx);

	if (the_acl == NULL) {
		return NULL;
	}

	for (i = 0; i < num_acls; i++) {
		SMB_ACL_ENTRY_T the_entry;
		SMB_ACL_PERMSET_T the_permset;
		SMB_ACL_TAG_T tag_type;

		if (sys_acl_create_entry(&the_acl, &the_entry) == -1) {
			DEBUG(0,("create_posix_acl_from_wire: Failed to create entry %u. (%s)\n",
				i, strerror(errno) ));
			goto fail;
		}

		if (!unix_ex_wire_to_tagtype(CVAL(pdata,(i*SMB_POSIX_ACL_ENTRY_SIZE)), &tag_type)) {
			DEBUG(0,("create_posix_acl_from_wire: invalid wire tagtype %u on entry %u.\n",
				CVAL(pdata,(i*SMB_POSIX_ACL_ENTRY_SIZE)), i ));
			goto fail;
		}

		if (sys_acl_set_tag_type(the_entry, tag_type) == -1) {
			DEBUG(0,("create_posix_acl_from_wire: Failed to set tagtype on entry %u. (%s)\n",
				i, strerror(errno) ));
			goto fail;
		}

		/* Get the permset pointer from the new ACL entry. */
		if (sys_acl_get_permset(the_entry, &the_permset) == -1) {
			DEBUG(0,("create_posix_acl_from_wire: Failed to get permset on entry %u. (%s)\n",
                                i, strerror(errno) ));
                        goto fail;
                }

		/* Map from wire to permissions. */
		if (!unix_ex_wire_to_permset(conn, CVAL(pdata,(i*SMB_POSIX_ACL_ENTRY_SIZE)+1), &the_permset)) {
			DEBUG(0,("create_posix_acl_from_wire: invalid permset %u on entry %u.\n",
				CVAL(pdata,(i*SMB_POSIX_ACL_ENTRY_SIZE) + 1), i ));
			goto fail;
		}

		/* Now apply to the new ACL entry. */
		if (sys_acl_set_permset(the_entry, the_permset) == -1) {
			DEBUG(0,("create_posix_acl_from_wire: Failed to add permset on entry %u. (%s)\n",
				i, strerror(errno) ));
			goto fail;
		}

		if (tag_type == SMB_ACL_USER) {
			uint32_t uidval = IVAL(pdata,(i*SMB_POSIX_ACL_ENTRY_SIZE)+2);
			uid_t uid = (uid_t)uidval;
			if (sys_acl_set_qualifier(the_entry,(void *)&uid) == -1) {
				DEBUG(0,("create_posix_acl_from_wire: Failed to set uid %u on entry %u. (%s)\n",
					(unsigned int)uid, i, strerror(errno) ));
				goto fail;
			}
		}

		if (tag_type == SMB_ACL_GROUP) {
			uint32_t gidval = IVAL(pdata,(i*SMB_POSIX_ACL_ENTRY_SIZE)+2);
			gid_t gid = (uid_t)gidval;
			if (sys_acl_set_qualifier(the_entry,(void *)&gid) == -1) {
				DEBUG(0,("create_posix_acl_from_wire: Failed to set gid %u on entry %u. (%s)\n",
					(unsigned int)gid, i, strerror(errno) ));
				goto fail;
			}
		}
	}

	return the_acl;

 fail:

	if (the_acl != NULL) {
		TALLOC_FREE(the_acl);
	}
	return NULL;
}

/****************************************************************************
 Calls from UNIX extensions - Default POSIX ACL set.
 If num_def_acls == 0 and not a directory just return. If it is a directory
 and num_def_acls == 0 then remove the default acl. Else set the default acl
 on the directory.
****************************************************************************/

NTSTATUS set_unix_posix_default_acl(connection_struct *conn,
				files_struct *fsp,
				uint16_t num_def_acls,
				const char *pdata)
{
	SMB_ACL_T def_acl = NULL;
	NTSTATUS status;
	int ret;

	if (!fsp->fsp_flags.is_directory) {
		return NT_STATUS_INVALID_HANDLE;
	}

	if (!num_def_acls) {
		/* Remove the default ACL. */
		ret = SMB_VFS_SYS_ACL_DELETE_DEF_FD(fsp);
		if (ret == -1) {
			status = map_nt_error_from_unix(errno);
			DBG_INFO("acl_delete_def_fd failed on "
				"directory %s (%s)\n",
				fsp_str_dbg(fsp),
				strerror(errno));
			return status;
		}
		return NT_STATUS_OK;
	}

	def_acl = create_posix_acl_from_wire(conn,
					num_def_acls,
					pdata,
					talloc_tos());
	if (def_acl == NULL) {
		return map_nt_error_from_unix(errno);
	}

	ret = SMB_VFS_SYS_ACL_SET_FD(fsp,
				     SMB_ACL_TYPE_DEFAULT,
				     def_acl);
	if (ret == -1) {
		status = map_nt_error_from_unix(errno);
		DBG_INFO("acl_set_file failed on directory %s (%s)\n",
			fsp_str_dbg(fsp),
			strerror(errno));
	        TALLOC_FREE(def_acl);
		return status;
	}

	DBG_DEBUG("set default acl for file %s\n",
		fsp_str_dbg(fsp));
	TALLOC_FREE(def_acl);
	return NT_STATUS_OK;
}

/****************************************************************************
 Remove an ACL from a file. As we don't have acl_delete_entry() available
 we must read the current acl and copy all entries except MASK, USER and GROUP
 to a new acl, then set that. This (at least on Linux) causes any ACL to be
 removed.
 FIXME ! How does the share mask/mode fit into this.... ?
****************************************************************************/

static NTSTATUS remove_posix_acl(connection_struct *conn,
			files_struct *fsp)
{
	SMB_ACL_T file_acl = NULL;
	int entry_id = SMB_ACL_FIRST_ENTRY;
	SMB_ACL_ENTRY_T entry;
	/* Create a new ACL with only 3 entries, u/g/w. */
	SMB_ACL_T new_file_acl = NULL;
	SMB_ACL_ENTRY_T user_ent = NULL;
	SMB_ACL_ENTRY_T group_ent = NULL;
	SMB_ACL_ENTRY_T other_ent = NULL;
	NTSTATUS status;
	int ret;

	new_file_acl = sys_acl_init(talloc_tos());
	if (new_file_acl == NULL) {
		status = map_nt_error_from_unix(errno);
		DBG_INFO("failed to init new ACL with 3 entries "
			"for file %s %s.\n",
			fsp_str_dbg(fsp),
			strerror(errno));
		goto done;
	}

	/* Now create the u/g/w entries. */
	ret = sys_acl_create_entry(&new_file_acl, &user_ent);
	if (ret == -1) {
		status = map_nt_error_from_unix(errno);
		DBG_INFO("Failed to create user entry for file %s. (%s)\n",
			fsp_str_dbg(fsp),
			strerror(errno));
		goto done;
	}
	ret = sys_acl_set_tag_type(user_ent, SMB_ACL_USER_OBJ);
	if (ret == -1) {
		status = map_nt_error_from_unix(errno);
		DBG_INFO("Failed to set user entry for file %s. (%s)\n",
			fsp_str_dbg(fsp),
			strerror(errno));
		goto done;
	}

	ret = sys_acl_create_entry(&new_file_acl, &group_ent);
	if (ret == -1) {
		status = map_nt_error_from_unix(errno);
		DBG_INFO("Failed to create group entry for file %s. (%s)\n",
			fsp_str_dbg(fsp),
			strerror(errno));
		goto done;
	}
	ret = sys_acl_set_tag_type(group_ent, SMB_ACL_GROUP_OBJ);
	if (ret == -1) {
		status = map_nt_error_from_unix(errno);
		DBG_INFO("Failed to set group entry for file %s. (%s)\n",
			fsp_str_dbg(fsp),
			strerror(errno));
		goto done;
	}

	ret = sys_acl_create_entry(&new_file_acl, &other_ent);
	if (ret == -1) {
		status = map_nt_error_from_unix(errno);
		DBG_INFO("Failed to create other entry for file %s. (%s)\n",
			fsp_str_dbg(fsp),
			strerror(errno));
		goto done;
	}
	ret = sys_acl_set_tag_type(other_ent, SMB_ACL_OTHER);
	if (ret == -1) {
		status = map_nt_error_from_unix(errno);
		DBG_INFO("Failed to set other entry for file %s. (%s)\n",
			fsp_str_dbg(fsp),
			strerror(errno));
		goto done;
	}

	/* Get the current file ACL. */
	file_acl = SMB_VFS_SYS_ACL_GET_FD(fsp,
					  SMB_ACL_TYPE_ACCESS,
					  talloc_tos());

	if (file_acl == NULL) {
		status = map_nt_error_from_unix(errno);
		/* This is only returned if an error occurred. Even for a file with
		   no acl a u/g/w acl should be returned. */
		DBG_INFO("failed to get ACL from file %s (%s).\n",
			fsp_str_dbg(fsp),
			strerror(errno));
		goto done;
	}

	while ( sys_acl_get_entry(file_acl, entry_id, &entry) == 1) {
		SMB_ACL_TAG_T tagtype;
		SMB_ACL_PERMSET_T permset;

		entry_id = SMB_ACL_NEXT_ENTRY;

		ret = sys_acl_get_tag_type(entry, &tagtype);
		if (ret == -1) {
			status = map_nt_error_from_unix(errno);
			DBG_INFO("failed to get tagtype from ACL "
				"on file %s (%s).\n",
				fsp_str_dbg(fsp),
				strerror(errno));
			goto done;
		}

		ret = sys_acl_get_permset(entry, &permset);
		if (ret == -1) {
			status = map_nt_error_from_unix(errno);
			DBG_INFO("failed to get permset from ACL "
				"on file %s (%s).\n",
				fsp_str_dbg(fsp),
				strerror(errno));
			goto done;
		}

		if (tagtype == SMB_ACL_USER_OBJ) {
			ret = sys_acl_set_permset(user_ent, permset);
			if (ret == -1) {
				status = map_nt_error_from_unix(errno);
				DBG_INFO("failed to set permset from ACL "
					"on file %s (%s).\n",
					fsp_str_dbg(fsp),
					strerror(errno));
				goto done;
			}
		} else if (tagtype == SMB_ACL_GROUP_OBJ) {
			ret = sys_acl_set_permset(group_ent, permset);
			if (ret == -1) {
				status = map_nt_error_from_unix(errno);
				DBG_INFO("failed to set permset from ACL "
					"on file %s (%s).\n",
					fsp_str_dbg(fsp),
					strerror(errno));
				goto done;
			}
		} else if (tagtype == SMB_ACL_OTHER) {
			ret = sys_acl_set_permset(other_ent, permset);
			if (ret == -1) {
				status = map_nt_error_from_unix(errno);
				DBG_INFO("failed to set permset from ACL "
					"on file %s (%s).\n",
					fsp_str_dbg(fsp),
					strerror(errno));
				goto done;
			}
		}
	}

	/* Set the new empty file ACL. */
	ret = SMB_VFS_SYS_ACL_SET_FD(fsp, SMB_ACL_TYPE_ACCESS, new_file_acl);
	if (ret == -1) {
		status = map_nt_error_from_unix(errno);
		DBG_INFO("acl_set_file failed on %s (%s)\n",
			fsp_str_dbg(fsp),
			strerror(errno));
		goto done;
	}

	status = NT_STATUS_OK;

 done:

	TALLOC_FREE(file_acl);
	TALLOC_FREE(new_file_acl);
	return status;
}

/****************************************************************************
 Calls from UNIX extensions - POSIX ACL set.
 If num_def_acls == 0 then read/modify/write acl after removing all entries
 except SMB_ACL_USER_OBJ, SMB_ACL_GROUP_OBJ, SMB_ACL_OTHER.
****************************************************************************/

NTSTATUS set_unix_posix_acl(connection_struct *conn,
			files_struct *fsp,
			uint16_t num_acls,
			const char *pdata)
{
	SMB_ACL_T file_acl = NULL;
	int ret;
	NTSTATUS status;

	if (!num_acls) {
		/* Remove the ACL from the file. */
		return remove_posix_acl(conn, fsp);
	}

	file_acl = create_posix_acl_from_wire(conn,
					num_acls,
					pdata,
					talloc_tos());
	if (file_acl == NULL) {
		return map_nt_error_from_unix(errno);
	}

	ret = SMB_VFS_SYS_ACL_SET_FD(fsp, SMB_ACL_TYPE_ACCESS, file_acl);
	if (ret == -1) {
		status = map_nt_error_from_unix(errno);
		DBG_INFO("acl_set_file failed on %s (%s)\n",
			fsp_str_dbg(fsp),
			strerror(errno));
		TALLOC_FREE(file_acl);
		return status;
	}

	DBG_DEBUG("set acl for file %s\n",
		fsp_str_dbg(fsp));

	TALLOC_FREE(file_acl);
	return NT_STATUS_OK;
}

int posix_sys_acl_blob_get_file(vfs_handle_struct *handle,
				const struct smb_filename *smb_fname_in,
				TALLOC_CTX *mem_ctx,
				char **blob_description,
				DATA_BLOB *blob)
{
	int ret;
	TALLOC_CTX *frame = talloc_stackframe();
	/* Initialise this to zero, in a portable way */
	struct smb_acl_wrapper acl_wrapper = {
		0
	};
	struct smb_filename *smb_fname = cp_smb_filename_nostream(frame,
						smb_fname_in);
	if (smb_fname == NULL) {
		TALLOC_FREE(frame);
		errno = ENOMEM;
		return -1;
	}

	ret = smb_vfs_call_stat(handle, smb_fname);
	if (ret == -1) {
		TALLOC_FREE(frame);
		return -1;
	}

	acl_wrapper.owner = smb_fname->st.st_ex_uid;
	acl_wrapper.group = smb_fname->st.st_ex_gid;
	acl_wrapper.mode = smb_fname->st.st_ex_mode;

	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_push_struct_blob(blob, mem_ctx,
							  &acl_wrapper,
							  (ndr_push_flags_fn_t)ndr_push_smb_acl_wrapper))) {
		errno = EINVAL;
		TALLOC_FREE(frame);
		return -1;
	}

	*blob_description = talloc_strdup(mem_ctx, "posix_acl");
	if (!*blob_description) {
		errno = EINVAL;
		TALLOC_FREE(frame);
		return -1;
	}

	TALLOC_FREE(frame);
	return 0;
}

int posix_sys_acl_blob_get_fd(vfs_handle_struct *handle,
			      files_struct *fsp,
			      TALLOC_CTX *mem_ctx,
			      char **blob_description,
			      DATA_BLOB *blob)
{
	SMB_STRUCT_STAT sbuf;
	TALLOC_CTX *frame;
	struct smb_acl_wrapper acl_wrapper = { 0 };
	int ret;

	frame = talloc_stackframe();

	acl_wrapper.access_acl = smb_vfs_call_sys_acl_get_fd(handle,
					fsp,
					SMB_ACL_TYPE_ACCESS,
					frame);

	if (fsp->fsp_flags.is_directory) {
		acl_wrapper.default_acl = smb_vfs_call_sys_acl_get_fd(handle,
						fsp,
						SMB_ACL_TYPE_DEFAULT,
						frame);
	}

	ret = smb_vfs_call_fstat(handle, fsp, &sbuf);
	if (ret == -1) {
		TALLOC_FREE(frame);
		return -1;
	}

	acl_wrapper.owner = sbuf.st_ex_uid;
	acl_wrapper.group = sbuf.st_ex_gid;
	acl_wrapper.mode = sbuf.st_ex_mode;

	if (!NDR_ERR_CODE_IS_SUCCESS(ndr_push_struct_blob(blob, mem_ctx,
							  &acl_wrapper,
							  (ndr_push_flags_fn_t)ndr_push_smb_acl_wrapper))) {
		errno = EINVAL;
		TALLOC_FREE(frame);
		return -1;
	}

	*blob_description = talloc_strdup(mem_ctx, "posix_acl");
	if (!*blob_description) {
		errno = EINVAL;
		TALLOC_FREE(frame);
		return -1;
	}

	TALLOC_FREE(frame);
	return 0;
}

static NTSTATUS make_default_acl_posix(TALLOC_CTX *ctx,
				       const char *name,
				       const SMB_STRUCT_STAT *psbuf,
				       struct security_descriptor **ppdesc)
{
	struct dom_sid owner_sid, group_sid;
	size_t size = 0;
	struct security_ace aces[4] = {};
	uint32_t access_mask = 0;
	mode_t mode = psbuf->st_ex_mode;
	struct security_acl *new_dacl = NULL;
	int idx = 0;

	DBG_DEBUG("file %s mode = 0%o\n",name, (int)mode);

	uid_to_sid(&owner_sid, psbuf->st_ex_uid);
	gid_to_sid(&group_sid, psbuf->st_ex_gid);

	/*
	 We provide up to 4 ACEs
		- Owner
		- Group
		- Everyone
		- NT System
	*/

	if (mode & S_IRUSR) {
		if (mode & S_IWUSR) {
			access_mask |= SEC_RIGHTS_FILE_ALL;
		} else {
			access_mask |= SEC_RIGHTS_FILE_READ | SEC_FILE_EXECUTE;
		}
	}
	if (mode & S_IWUSR) {
		access_mask |= SEC_RIGHTS_FILE_WRITE | SEC_STD_DELETE;
	}

	init_sec_ace(&aces[idx],
			&owner_sid,
			SEC_ACE_TYPE_ACCESS_ALLOWED,
			access_mask,
			0);
	idx++;

	access_mask = 0;
	if (mode & S_IRGRP) {
		access_mask |= SEC_RIGHTS_FILE_READ | SEC_FILE_EXECUTE;
	}
	if (mode & S_IWGRP) {
		/* note that delete is not granted - this matches posix behaviour */
		access_mask |= SEC_RIGHTS_FILE_WRITE;
	}
	if (access_mask) {
		init_sec_ace(&aces[idx],
			&group_sid,
			SEC_ACE_TYPE_ACCESS_ALLOWED,
			access_mask,
			0);
		idx++;
	}

	access_mask = 0;
	if (mode & S_IROTH) {
		access_mask |= SEC_RIGHTS_FILE_READ | SEC_FILE_EXECUTE;
	}
	if (mode & S_IWOTH) {
		access_mask |= SEC_RIGHTS_FILE_WRITE;
	}
	if (access_mask) {
		init_sec_ace(&aces[idx],
			&global_sid_World,
			SEC_ACE_TYPE_ACCESS_ALLOWED,
			access_mask,
			0);
		idx++;
	}

	init_sec_ace(&aces[idx],
			&global_sid_System,
			SEC_ACE_TYPE_ACCESS_ALLOWED,
			SEC_RIGHTS_FILE_ALL,
			0);
	idx++;

	new_dacl = make_sec_acl(ctx,
			NT4_ACL_REVISION,
			idx,
			aces);

	if (!new_dacl) {
		return NT_STATUS_NO_MEMORY;
	}

	*ppdesc = make_sec_desc(ctx,
			SECURITY_DESCRIPTOR_REVISION_1,
			SEC_DESC_SELF_RELATIVE|SEC_DESC_DACL_PRESENT,
			&owner_sid,
			&group_sid,
			NULL,
			new_dacl,
			&size);
	if (!*ppdesc) {
		return NT_STATUS_NO_MEMORY;
	}
	return NT_STATUS_OK;
}

static NTSTATUS make_default_acl_windows(TALLOC_CTX *ctx,
					 const char *name,
					 const SMB_STRUCT_STAT *psbuf,
					 struct security_descriptor **ppdesc)
{
	struct dom_sid owner_sid, group_sid;
	size_t size = 0;
	struct security_ace aces[4] = {0};
	uint32_t access_mask = 0;
	mode_t mode = psbuf->st_ex_mode;
	struct security_acl *new_dacl = NULL;
	int idx = 0;

	DBG_DEBUG("file [%s] mode [0%o]\n", name, (int)mode);

	uid_to_sid(&owner_sid, psbuf->st_ex_uid);
	gid_to_sid(&group_sid, psbuf->st_ex_gid);

	/*
	 * We provide 2 ACEs:
	 * - Owner
	 * - NT System
	 */

	if (mode & S_IRUSR) {
		if (mode & S_IWUSR) {
			access_mask |= SEC_RIGHTS_FILE_ALL;
		} else {
			access_mask |= SEC_RIGHTS_FILE_READ | SEC_FILE_EXECUTE;
		}
	}
	if (mode & S_IWUSR) {
		access_mask |= SEC_RIGHTS_FILE_WRITE | SEC_STD_DELETE;
	}

	init_sec_ace(&aces[idx],
		     &owner_sid,
		     SEC_ACE_TYPE_ACCESS_ALLOWED,
		     access_mask,
		     0);
	idx++;

	init_sec_ace(&aces[idx],
		     &global_sid_System,
		     SEC_ACE_TYPE_ACCESS_ALLOWED,
		     SEC_RIGHTS_FILE_ALL,
		     0);
	idx++;

	new_dacl = make_sec_acl(ctx,
				NT4_ACL_REVISION,
				idx,
				aces);

	if (!new_dacl) {
		return NT_STATUS_NO_MEMORY;
	}

	*ppdesc = make_sec_desc(ctx,
				SECURITY_DESCRIPTOR_REVISION_1,
				SEC_DESC_SELF_RELATIVE|SEC_DESC_DACL_PRESENT,
				&owner_sid,
				&group_sid,
				NULL,
				new_dacl,
				&size);
	if (!*ppdesc) {
		return NT_STATUS_NO_MEMORY;
	}
	return NT_STATUS_OK;
}

static NTSTATUS make_default_acl_everyone(TALLOC_CTX *ctx,
					  const char *name,
					  const SMB_STRUCT_STAT *psbuf,
					  struct security_descriptor **ppdesc)
{
	struct dom_sid owner_sid, group_sid;
	size_t size = 0;
	struct security_ace aces[1] = {0};
	mode_t mode = psbuf->st_ex_mode;
	struct security_acl *new_dacl = NULL;
	int idx = 0;

	DBG_DEBUG("file [%s] mode [0%o]\n", name, (int)mode);

	uid_to_sid(&owner_sid, psbuf->st_ex_uid);
	gid_to_sid(&group_sid, psbuf->st_ex_gid);

	/*
	 * We provide one ACEs: full access for everyone
	 */

	init_sec_ace(&aces[idx],
		     &global_sid_World,
		     SEC_ACE_TYPE_ACCESS_ALLOWED,
		     SEC_RIGHTS_FILE_ALL,
		     0);
	idx++;

	new_dacl = make_sec_acl(ctx,
				NT4_ACL_REVISION,
				idx,
				aces);

	if (!new_dacl) {
		return NT_STATUS_NO_MEMORY;
	}

	*ppdesc = make_sec_desc(ctx,
				SECURITY_DESCRIPTOR_REVISION_1,
				SEC_DESC_SELF_RELATIVE|SEC_DESC_DACL_PRESENT,
				&owner_sid,
				&group_sid,
				NULL,
				new_dacl,
				&size);
	if (!*ppdesc) {
		return NT_STATUS_NO_MEMORY;
	}
	return NT_STATUS_OK;
}

static const struct enum_list default_acl_style_list[] = {
	{DEFAULT_ACL_POSIX,	"posix"},
	{DEFAULT_ACL_WINDOWS,	"windows"},
	{DEFAULT_ACL_EVERYONE,	"everyone"},
};

const struct enum_list *get_default_acl_style_list(void)
{
	return default_acl_style_list;
}

NTSTATUS make_default_filesystem_acl(
	TALLOC_CTX *ctx,
	enum default_acl_style acl_style,
	const char *name,
	const SMB_STRUCT_STAT *psbuf,
	struct security_descriptor **ppdesc)
{
	NTSTATUS status;

	switch (acl_style) {
	case DEFAULT_ACL_POSIX:
		status =  make_default_acl_posix(ctx, name, psbuf, ppdesc);
		break;

	case DEFAULT_ACL_WINDOWS:
		status =  make_default_acl_windows(ctx, name, psbuf, ppdesc);
		break;

	case DEFAULT_ACL_EVERYONE:
		status =  make_default_acl_everyone(ctx, name, psbuf, ppdesc);
		break;

	default:
		DBG_ERR("unknown acl style %d\n", acl_style);
		status = NT_STATUS_INTERNAL_ERROR;
		break;
	}

	return status;
}
