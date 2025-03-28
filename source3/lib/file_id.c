/*
   Unix SMB/CIFS implementation.

   file_id structure handling

   Copyright (C) Andrew Tridgell 2007

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
#include "lib/file_id.h"

/*
  return True if two file_id structures are equal
 */
bool file_id_equal(const struct file_id *id1, const struct file_id *id2)
{
	return id1->inode == id2->inode && id1->devid == id2->devid &&
	    id1->extid == id2->extid;
}

char *file_id_str_buf(struct file_id fid, struct file_id_buf *dst)
{
	snprintf(dst->buf,
		 sizeof(dst->buf),
		 "%"PRIu64":%"PRIu64":%"PRIu64,
		 fid.devid,
		 fid.inode,
		 fid.extid);
	return dst->buf;
}

/*
  push a 16 byte version of a file id into a buffer.  This ignores the extid
  and is needed when dev/inodes are stored in persistent storage (tdbs).
 */
void push_file_id_16(uint8_t *buf, const struct file_id *id)
{
	SIVAL(buf,  0, id->devid&0xFFFFFFFF);
	SIVAL(buf,  4, id->devid>>32);
	SIVAL(buf,  8, id->inode&0xFFFFFFFF);
	SIVAL(buf, 12, id->inode>>32);
}
