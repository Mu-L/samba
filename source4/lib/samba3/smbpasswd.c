/* 
   Unix SMB/CIFS implementation.
   smbpasswd file format routines

   Copyright (C) Andrew Tridgell 1992-1998 
   Modified by Jeremy Allison 1995.
   Modified by Gerald (Jerry) Carter 2000-2001
   Copyright (C) Tim Potter 2001
   Copyright (C) Andrew Bartlett <abartlet@samba.org> 2005
   Copyright (C) Jelmer Vernooij <jelmer@samba.org> 2005
   
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

/*! \file lib/smbpasswd.c

   The smbpasswd file is used to store encrypted passwords in a similar
   fashion to the /etc/passwd file.  The format is colon separated fields
   with one user per line like so:

   <username>:<uid>:<lanman hash>:<nt hash>:<acb info>:<last change time>

   The username and uid must correspond to an entry in the /etc/passwd
   file.  The lanman and nt password hashes are 32 hex digits corresponding
   to the 16-byte lanman and nt hashes respectively.  

   The password last change time is stored as a string of the format
   LCD-<change time> where the change time is expressed as an 

   'N'    No password
   'D'    Disabled
   'H'    Homedir required
   'T'    Temp account.
   'U'    User account (normal) 
   'M'    MNS logon user account - what is this ? 
   'W'    Workstation account
   'S'    Server account 
   'L'    Locked account
   'X'    No Xpiry on password 
   'I'    Interdomain trust account

*/

#include "includes.h"
#include "system/locale.h"
#include "lib/samba3/samba3.h"

/*! Convert 32 hex characters into a 16 byte array. */

struct samr_Password *smbpasswd_gethexpwd(TALLOC_CTX *mem_ctx, const char *p)
{
	struct samr_Password *pwd = NULL;
	size_t len;

	if (p == NULL) {
		return NULL;
	}

	pwd = talloc(mem_ctx, struct samr_Password);
	if (pwd == NULL) {
		return NULL;
	}

	len = strhex_to_str((char *)pwd->hash,
			    sizeof(pwd->hash),
			    p,
			    sizeof(pwd->hash) * 2);
	if (len != sizeof(pwd->hash)) {
		TALLOC_FREE(pwd);
		return NULL;
	}

	return pwd;
}

/*! Convert a 16-byte array into 32 hex characters. */
char *smbpasswd_sethexpwd(TALLOC_CTX *mem_ctx, struct samr_Password *pwd, uint16_t acb_info)
{
	char *p;
	if (pwd != NULL) {
		p = hex_encode_talloc(mem_ctx, pwd->hash, sizeof(pwd->hash));
	} else {
		if (acb_info & ACB_PWNOTREQ)
			p = talloc_strdup(mem_ctx, "NO PASSWORDXXXXXXXXXXXXXXXXXXXXX");
		else
			p = talloc_strdup(mem_ctx, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
	}
	return p;
}
