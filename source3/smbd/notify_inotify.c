/*
   Unix SMB/CIFS implementation.

   Copyright (C) Andrew Tridgell 2006

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

/*
  notify implementation using inotify
*/

#include "includes.h"
#include "../librpc/gen_ndr/notify.h"
#include "smbd/smbd.h"
#include "lib/util/sys_rw.h"
#include "smbd/globals.h"

#include <sys/inotify.h>

/* glibc < 2.5 headers don't have these defines */
#ifndef IN_ONLYDIR
#define IN_ONLYDIR 0x01000000
#endif
#ifndef IN_MASK_ADD
#define IN_MASK_ADD 0x20000000
#endif

struct inotify_private {
	struct sys_notify_context *ctx;
	int fd;
	struct inotify_watch_context *watches;
};

struct inotify_watch_context {
	struct inotify_watch_context *next, *prev;
	struct inotify_private *in;
	int wd;
	void (*callback)(struct sys_notify_context *ctx,
			 void *private_data,
			 struct notify_event *ev,
			 uint32_t filter);
	void *private_data;
	uint32_t mask; /* the inotify mask */
	uint32_t filter; /* the windows completion filter */
	const char *path;

	char *last_mkdir;
	struct inotify_event *moved_from_event;
};


/*
  map from a change notify mask to a inotify mask. Remove any bits
  which we can handle
*/
static const struct {
	uint32_t notify_mask;
	uint32_t inotify_mask;
} inotify_mapping[] = {
	{FILE_NOTIFY_CHANGE_FILE_NAME,   IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO},
	{FILE_NOTIFY_CHANGE_DIR_NAME,    IN_CREATE|IN_DELETE|IN_MOVED_FROM|IN_MOVED_TO},
	{FILE_NOTIFY_CHANGE_ATTRIBUTES,  IN_ATTRIB|IN_MOVED_TO|IN_MOVED_FROM|IN_MODIFY},
	{FILE_NOTIFY_CHANGE_LAST_WRITE,  IN_ATTRIB},
	{FILE_NOTIFY_CHANGE_LAST_ACCESS, IN_ATTRIB},
	{FILE_NOTIFY_CHANGE_EA,          IN_ATTRIB},
	{FILE_NOTIFY_CHANGE_SECURITY,    IN_ATTRIB}
};

static uint32_t inotify_map(uint32_t *filter)
{
	size_t i;
	uint32_t out=0;
	for (i=0;i<ARRAY_SIZE(inotify_mapping);i++) {
		if (inotify_mapping[i].notify_mask & *filter) {
			out |= inotify_mapping[i].inotify_mask;
			*filter &= ~inotify_mapping[i].notify_mask;
		}
	}
	return out;
}

/*
 * Map inotify mask back to filter. This returns all filters that
 * could have created the inotify watch.
 */
static uint32_t inotify_map_mask_to_filter(uint32_t mask)
{
	size_t i;
	uint32_t filter = 0;

	for (i = 0; i < ARRAY_SIZE(inotify_mapping); i++) {
		if (inotify_mapping[i].inotify_mask & mask) {
			filter |= inotify_mapping[i].notify_mask;
		}
	}

	if (mask & IN_ISDIR) {
		filter &= ~FILE_NOTIFY_CHANGE_FILE_NAME;
	} else {
		filter &= ~FILE_NOTIFY_CHANGE_DIR_NAME;
	}

	return filter;
}

/*
  destroy the inotify private context
*/
static int inotify_destructor(struct inotify_private *in)
{
	close(in->fd);
	return 0;
}


/*
  see if a particular event from inotify really does match a requested
  notify event in SMB
*/
static bool filter_match(struct inotify_watch_context *w,
			 struct inotify_event *e)
{
	bool ok;

	DEBUG(10, ("filter_match: e->mask=%x, w->mask=%x, w->filter=%x\n",
		   e->mask, w->mask, w->filter));

	if ((e->mask & w->mask) == 0) {
		/* this happens because inotify_add_watch() coalesces watches on the same
		   path, oring their masks together */
		return False;
	}

	/* SMB separates the filters for files and directories */
	if (e->mask & IN_ISDIR) {
		ok = ((w->filter & FILE_NOTIFY_CHANGE_DIR_NAME) != 0);
		return ok;
	}

	if ((e->mask & IN_ATTRIB) &&
	    (w->filter & (FILE_NOTIFY_CHANGE_ATTRIBUTES|
			  FILE_NOTIFY_CHANGE_LAST_WRITE|
			  FILE_NOTIFY_CHANGE_LAST_ACCESS|
			  FILE_NOTIFY_CHANGE_EA|
			  FILE_NOTIFY_CHANGE_SECURITY))) {
		return True;
	}
	if ((e->mask & IN_MODIFY) &&
	    (w->filter & FILE_NOTIFY_CHANGE_ATTRIBUTES)) {
		return True;
	}

	ok = ((w->filter & FILE_NOTIFY_CHANGE_FILE_NAME) != 0);
	return ok;
}

static void trigger_orphaned_moved_from(struct inotify_watch_context *w)
{
	struct notify_event ne = {};
	struct inotify_event *e = w->moved_from_event;

	ne = (struct notify_event){
		.action = NOTIFY_ACTION_REMOVED,
		.path = e->name,
		.dir = w->path,
	};

	w->callback(w->in->ctx,
		    w->private_data,
		    &ne,
		    inotify_map_mask_to_filter(e->mask));
}

static void moved_from_timeout(struct tevent_context *ev,
			       struct tevent_timer *te,
			       struct timeval now,
			       void *private_data)
{
	struct inotify_watch_context *w = talloc_get_type_abort(
		private_data, struct inotify_watch_context);

	trigger_orphaned_moved_from(w);
	TALLOC_FREE(w->moved_from_event);
}

static void save_moved_from(struct tevent_context *ev,
			    struct inotify_watch_context *w,
			    struct inotify_event *e)
{
	if (w->moved_from_event != NULL) {
		trigger_orphaned_moved_from(w);
		TALLOC_FREE(w->moved_from_event);
	}

	w->moved_from_event = talloc_memdup(
		w, e, sizeof(struct inotify_event) + e->len);
	if (w->moved_from_event == NULL) {
		/*
		 * Not much we can do here
		 */
		return;
	}

	tevent_add_timer(ev,
			 w->moved_from_event,
			 tevent_timeval_current_ofs(0, 100000),
			 moved_from_timeout,
			 w);
}

static bool handle_local_rename(struct inotify_watch_context *w,
				struct inotify_event *to)
{
	struct inotify_private *in = w->in;
	struct inotify_event *from = w->moved_from_event;
	struct notify_event ne = {};
	uint32_t filter;

	if ((w->last_mkdir != NULL) && (w->moved_from_event != NULL) &&
	    IS_SMBD_TMPNAME(w->moved_from_event->name, NULL))
	{
		if (strcmp(to->name, w->last_mkdir) == 0) {
			/*
			 * Assume this is a rename() from smbd after a
			 * mkdir of the real target directory. See the
			 * comment about RENAME_NOREPLACE in
			 * mkdir_internals(). We have already sent out
			 * the mkdir notify event, this MOVED_FROM/TO
			 * pair is just internal fluff that the client
			 * should not get wind of via notify.
			 */
			TALLOC_FREE(w->last_mkdir);
			TALLOC_FREE(w->moved_from_event);
			return true;
		}

		if (strcmp(w->moved_from_event->name, w->last_mkdir) == 0) {
			/*
			 * Assume this is a renameat2() from smbd's
			 * mkdir_internal().
			 */
			TALLOC_FREE(w->last_mkdir);
			TALLOC_FREE(w->moved_from_event);

			/*
			 * Pretend this is not a rename but a new
			 * directory.
			 */
			to->mask = IN_ISDIR | IN_CREATE;

			return false;
		}
	}

	ne = (struct notify_event){
		.action = NOTIFY_ACTION_OLD_NAME,
		.path = from->name,
		.dir = w->path,
	};

	if (filter_match(w, from)) {
		filter = inotify_map_mask_to_filter(to->mask);
		w->callback(in->ctx, w->private_data, &ne, filter);
	}

	ne = (struct notify_event){
		.action = NOTIFY_ACTION_NEW_NAME,
		.path = to->name,
		.dir = w->path,
	};

	filter = inotify_map_mask_to_filter(to->mask);

	if (filter_match(w, to)) {
		w->callback(in->ctx, w->private_data, &ne, filter);
	}

	if (to->mask & IN_ISDIR) {
		return true;
	}
	if ((w->filter & FILE_NOTIFY_CHANGE_CREATION) == 0) {
		return true;
	}

	/*
	 * SMB expects a file rename to generate three events, two for
	 * the rename and the other for a modify of the
	 * destination. Strange!
	 */

	ne.action = NOTIFY_ACTION_MODIFIED;
	to->mask = IN_ATTRIB;

	if (filter_match(w, to)) {
		w->callback(in->ctx, w->private_data, &ne, filter);
	}

	return true;
}

/*
  dispatch one inotify event

  the cookies are used to correctly handle renames
*/
static void inotify_dispatch(struct tevent_context *ev,
			     struct inotify_private *in,
			     struct inotify_event *e)
{
	struct inotify_watch_context *w, *next;
	struct notify_event ne;
	uint32_t filter;

	DBG_DEBUG("called with mask=%x, name=[%s]\n",
		  e->mask, e->len ? e->name : "");

	/* ignore extraneous events, such as unmount and IN_IGNORED events */
	if ((e->mask & (IN_ATTRIB|IN_MODIFY|IN_CREATE|IN_DELETE|
			IN_MOVED_FROM|IN_MOVED_TO)) == 0) {
		return;
	}

	if (e->mask & IN_MOVED_FROM) {
		for (w = in->watches; w != NULL; w = w->next) {
			if (e->wd != w->wd) {
				continue;
			}
			save_moved_from(ev, w, e);
		}
		return;
	}

	if (e->mask & IN_MOVED_TO) {
		for (w = in->watches; w != NULL; w = w->next) {
			if ((w->wd == e->wd) &&
			    (w->moved_from_event != NULL) &&
			    (w->moved_from_event->cookie == e->cookie))
			{
				bool handled = handle_local_rename(w, e);
				if (handled) {
					return;
				}
			}
		}
	}

	if ((e->mask & IN_CREATE) && (e->mask & IN_ISDIR)) {
		for (w = in->watches; w != NULL; w = w->next) {
			if (w->wd != e->wd) {
				continue;
			}
			TALLOC_FREE(w->last_mkdir);
			w->last_mkdir = talloc_strdup(w, e->name);
		}

		if (IS_SMBD_TMPNAME(e->name, NULL)) {
			return;
		}
	}

	if (e->mask & IN_CREATE) {
		ne.action = NOTIFY_ACTION_ADDED;
	} else if (e->mask & IN_DELETE) {
		ne.action = NOTIFY_ACTION_REMOVED;
	} else if (e->mask & IN_MOVED_TO) {
		ne.action = NOTIFY_ACTION_ADDED;
	} else {
		ne.action = NOTIFY_ACTION_MODIFIED;
	}
	ne.path = e->name;

	filter = inotify_map_mask_to_filter(e->mask);

	DBG_DEBUG("ne.action = %d, ne.path = %s, filter = %d\n",
		  ne.action, ne.path, filter);

	/* find any watches that have this watch descriptor */
	for (w=in->watches;w;w=next) {
		next = w->next;
		if (w->wd == e->wd && filter_match(w, e)) {
			ne.dir = w->path;
			w->callback(in->ctx, w->private_data, &ne, filter);
		}
	}
}

/*
  called when the kernel has some events for us
*/
static void inotify_handler(struct tevent_context *ev, struct tevent_fd *fde,
			    uint16_t flags, void *private_data)
{
	struct inotify_private *in = talloc_get_type(private_data,
						     struct inotify_private);
	char buf[sizeof(struct inotify_event) + NAME_MAX + 1];
	int bufsize = 0;
	struct inotify_event *e = NULL;
	ssize_t ret;

	ret = sys_read(in->fd, buf, sizeof(buf));
	if (ret == -1) {
		DEBUG(0, ("Failed to read all inotify data - %s\n",
			  strerror(errno)));
		/* the inotify fd will now be out of sync,
		 * can't keep reading data off it */
		TALLOC_FREE(fde);
		return;
	}
	bufsize = ret;

	e = (struct inotify_event *)buf;

	/* we can get more than one event in the buffer */
	while (bufsize >= sizeof(struct inotify_event)) {
		size_t e_len = sizeof(struct inotify_event) + e->len;

		if ((e_len < sizeof(struct inotify_event)) ||
		    (e_len > bufsize))
		{
			DBG_ERR("Invalid data from inotify\n");
			TALLOC_FREE(fde);
			return;
		}

		inotify_dispatch(ev, in, e);

		e = (struct inotify_event *)((char *)e + e_len);
		bufsize -= e_len;
	}
}

/*
  setup the inotify handle - called the first time a watch is added on
  this context
*/
static int inotify_setup(struct sys_notify_context *ctx)
{
	struct inotify_private *in;
	struct tevent_fd *fde;

	in = talloc(ctx, struct inotify_private);
	if (in == NULL) {
		return ENOMEM;
	}

	in->fd = inotify_init();
	if (in->fd == -1) {
		int ret = errno;
		DEBUG(0, ("Failed to init inotify - %s\n", strerror(ret)));
		talloc_free(in);
		return ret;
	}
	in->ctx = ctx;
	in->watches = NULL;

	ctx->private_data = in;
	talloc_set_destructor(in, inotify_destructor);

	/* add a event waiting for the inotify fd to be readable */
	fde = tevent_add_fd(ctx->ev, in, in->fd, TEVENT_FD_READ,
			    inotify_handler, in);
	if (fde == NULL) {
		ctx->private_data = NULL;
		TALLOC_FREE(in);
		return ENOMEM;
	}
	return 0;
}

/*
  destroy a watch
*/
static int watch_destructor(struct inotify_watch_context *w)
{
	struct inotify_private *in = w->in;
	int wd = w->wd;
	DLIST_REMOVE(w->in->watches, w);

	for (w=in->watches;w;w=w->next) {
		if (w->wd == wd) {
			/*
			 * Another inotify_watch_context listens on this path,
			 * leave the kernel level watch in place
			 */
			return 0;
		}
	}

	DEBUG(10, ("Deleting inotify watch %d\n", wd));
	if (inotify_rm_watch(in->fd, wd) == -1) {
		DEBUG(1, ("inotify_rm_watch returned %s\n", strerror(errno)));
	}
	return 0;
}


/*
  add a watch. The watch is removed when the caller calls
  talloc_free() on *handle
*/
int inotify_watch(TALLOC_CTX *mem_ctx,
		  struct sys_notify_context *ctx,
		  const char *path,
		  uint32_t *filter,
		  uint32_t *subdir_filter,
		  void (*callback)(struct sys_notify_context *ctx,
				   void *private_data,
				   struct notify_event *ev,
				   uint32_t filter),
		  void *private_data,
		  void *handle_p)
{
	struct inotify_private *in;
	uint32_t mask;
	struct inotify_watch_context *w;
	uint32_t orig_filter = *filter;
	void **handle = (void **)handle_p;

	/* maybe setup the inotify fd */
	if (ctx->private_data == NULL) {
		int ret;
		ret = inotify_setup(ctx);
		if (ret != 0) {
			return ret;
		}
	}

	in = talloc_get_type(ctx->private_data, struct inotify_private);

	mask = inotify_map(filter);
	if (mask == 0) {
		/* this filter can't be handled by inotify */
		return EINVAL;
	}

	/* using IN_MASK_ADD allows us to cope with inotify() returning the same
	   watch descriptor for multiple watches on the same path */
	mask |= (IN_MASK_ADD | IN_ONLYDIR);

	w = talloc(mem_ctx, struct inotify_watch_context);
	if (w == NULL) {
		*filter = orig_filter;
		return ENOMEM;
	}

	*w = (struct inotify_watch_context) {
		.in = in,
		.callback = callback,
		.private_data = private_data,
		.mask = mask,
		.filter = orig_filter,
		.path = talloc_strdup(w, path),
	};

	if (w->path == NULL) {
		*filter = orig_filter;
		TALLOC_FREE(w);
		return ENOMEM;
	}

	/* get a new watch descriptor for this path */
	w->wd = inotify_add_watch(in->fd, path, mask);
	if (w->wd == -1) {
		int err = errno;
		*filter = orig_filter;
		TALLOC_FREE(w);
		DEBUG(1, ("inotify_add_watch returned %s\n", strerror(err)));
		return err;
	}

	DEBUG(10, ("inotify_add_watch for %s mask %x returned wd %d\n",
		   path, mask, w->wd));

	(*handle) = w;

	DLIST_ADD(in->watches, w);

	/* the caller frees the handle to stop watching */
	talloc_set_destructor(w, watch_destructor);

	return 0;
}
