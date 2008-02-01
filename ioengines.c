/*
 * The io parts of the fio tool, includes workers for sync and mmap'ed
 * io, as well as both posix and linux libaio support.
 *
 * sync io is implemented on top of aio.
 *
 * This is not really specific to fio, if the get_io_u/put_io_u and
 * structures was pulled into this as well it would be a perfectly
 * generic io engine that could be used for other projects.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <dlfcn.h>
#include <assert.h>

#include "fio.h"

static LIST_HEAD(engine_list);

static int check_engine_ops(struct ioengine_ops *ops)
{
	if (ops->version != FIO_IOOPS_VERSION) {
		log_err("bad ioops version %d (want %d)\n", ops->version, FIO_IOOPS_VERSION);
		return 1;
	}

	if (!ops->queue) {
		log_err("%s: no queue handler\n", ops->name);
		return 1;
	}

	/*
	 * sync engines only need a ->queue()
	 */
	if (ops->flags & FIO_SYNCIO)
		return 0;
	
	if (!ops->event) {
		log_err("%s: no event handler\n", ops->name);
		return 1;
	}
	if (!ops->getevents) {
		log_err("%s: no getevents handler\n", ops->name);
		return 1;
	}
	if (!ops->queue) {
		log_err("%s: no queue handler\n", ops->name);
		return 1;
	}
		
	return 0;
}

void unregister_ioengine(struct ioengine_ops *ops)
{
	dprint(FD_IO, "ioengine %s unregistered\n", ops->name);
	list_del(&ops->list);
	INIT_LIST_HEAD(&ops->list);
}

void register_ioengine(struct ioengine_ops *ops)
{
	dprint(FD_IO, "ioengine %s registered\n", ops->name);
	INIT_LIST_HEAD(&ops->list);
	list_add_tail(&ops->list, &engine_list);
}

static struct ioengine_ops *find_ioengine(const char *name)
{
	struct ioengine_ops *ops;
	struct list_head *entry;

	list_for_each(entry, &engine_list) {
		ops = list_entry(entry, struct ioengine_ops, list);
		if (!strcmp(name, ops->name))
			return ops;
	}

	return NULL;
}

static struct ioengine_ops *dlopen_ioengine(struct thread_data *td,
					    const char *engine_lib)
{
	struct ioengine_ops *ops;
	void *dlhandle;

	dprint(FD_IO, "dload engine %s\n", engine_lib);

	dlerror();
	dlhandle = dlopen(engine_lib, RTLD_LAZY);
	if (!dlhandle) {
		td_vmsg(td, -1, dlerror(), "dlopen");
		return NULL;
	}

	/*
	 * Unlike the included modules, external engines should have a
	 * non-static ioengine structure that we can reference.
	 */
	ops = dlsym(dlhandle, "ioengine");
	if (!ops) {
		td_vmsg(td, -1, dlerror(), "dlsym");
		dlclose(dlhandle);
		return NULL;
	}

	ops->dlhandle = dlhandle;
	return ops;
}

struct ioengine_ops *load_ioengine(struct thread_data *td, const char *name)
{
	struct ioengine_ops *ops, *ret;
	char engine[16];

	dprint(FD_IO, "load ioengine %s\n", name);

	strncpy(engine, name, sizeof(engine) - 1);

	/*
	 * linux libaio has alias names, so convert to what we want
	 */
	if (!strncmp(engine, "linuxaio", 8) || !strncmp(engine, "aio", 3))
		strcpy(engine, "libaio");

	ops = find_ioengine(engine);
	if (!ops)
		ops = dlopen_ioengine(td, name);

	if (!ops) {
		log_err("fio: engine %s not loadable\n", name);
		return NULL;
	}

	/*
	 * Check that the required methods are there.
	 */
	if (check_engine_ops(ops))
		return NULL;

	ret = malloc(sizeof(*ret));
	memcpy(ret, ops, sizeof(*ret));
	ret->data = NULL;

	return ret;
}

void close_ioengine(struct thread_data *td)
{
	dprint(FD_IO, "close ioengine %s\n", td->io_ops->name);

	if (td->io_ops->cleanup)
		td->io_ops->cleanup(td);

	if (td->io_ops->dlhandle)
		dlclose(td->io_ops->dlhandle);

	free(td->io_ops);
	td->io_ops = NULL;
}

int td_io_prep(struct thread_data *td, struct io_u *io_u)
{
	dprint_io_u(io_u, "prep");
	fio_ro_check(td, io_u);

	if (td->io_ops->prep) {
		int ret = td->io_ops->prep(td, io_u);

		dprint(FD_IO, "->prep(%p)=%d\n", io_u, ret);
		return ret;
	}

	return 0;
}

int td_io_getevents(struct thread_data *td, unsigned int min, unsigned int max,
		    struct timespec *t)
{
	int r = 0;

	if (min > 0 && td->io_ops->commit) {
		r = td->io_ops->commit(td);
		if (r < 0)
			goto out;
	}

	r = 0;
	if (td->io_ops->getevents)
		r = td->io_ops->getevents(td, min, max, t);
out:
	dprint(FD_IO, "getevents: %d\n", r);
	return r;
}

int td_io_queue(struct thread_data *td, struct io_u *io_u)
{
	int ret;

	dprint_io_u(io_u, "queue");
	fio_ro_check(td, io_u);

	assert((io_u->flags & IO_U_F_FLIGHT) == 0);
	io_u->flags |= IO_U_F_FLIGHT;

	assert(io_u->file->flags & FIO_FILE_OPEN);

	io_u->error = 0;
	io_u->resid = 0;

	if (td->io_ops->flags & FIO_SYNCIO) {
		fio_gettime(&io_u->issue_time, NULL);
		memcpy(&td->last_issue, &io_u->issue_time, sizeof(struct timeval));

		/*
		 * for a sync engine, set the timeout upfront
		 */
		if (mtime_since(&td->timeout_end, &io_u->issue_time) < IO_U_TIMEOUT)
			io_u_set_timeout(td);
	}

	if (io_u->ddir != DDIR_SYNC)
		td->io_issues[io_u->ddir]++;

	io_u_mark_depth(td, io_u);

	ret = td->io_ops->queue(td, io_u);

	if (ret == FIO_Q_QUEUED) {
		int r;

		td->io_u_queued++;
		if (td->io_u_queued > td->o.iodepth_batch) {
			r = td_io_commit(td);
			if (r < 0)
				return r;
		}
	}

	if ((td->io_ops->flags & FIO_SYNCIO) == 0) {
		fio_gettime(&io_u->issue_time, NULL);
		memcpy(&td->last_issue, &io_u->issue_time, sizeof(struct timeval));

		/*
		 * async engine, set the timeout here
		 */
		if (ret == FIO_Q_QUEUED &&
		    mtime_since(&td->timeout_end, &io_u->issue_time) < IO_U_TIMEOUT)
			io_u_set_timeout(td);
	}

	return ret;
}

int td_io_init(struct thread_data *td)
{
	int ret = 0;

	if (td->io_ops->init) {
		ret = td->io_ops->init(td);
		if (ret && td->o.iodepth > 1)
			log_err("fio: io engine init failed. Perhaps try reducing io depth?\n");
	}

	return ret;
}

int td_io_commit(struct thread_data *td)
{
	dprint(FD_IO, "calling ->commit(), depth %d\n", td->cur_depth);

	if (!td->cur_depth)
		return 0;

	td->io_u_queued = 0;
	if (td->io_ops->commit)
		return td->io_ops->commit(td);

	return 0;
}

int td_io_open_file(struct thread_data *td, struct fio_file *f)
{
	if (td->io_ops->open_file(td, f)) {
		if (td->error == EINVAL && td->o.odirect)
			log_err("fio: destination does not support O_DIRECT\n");
		if (td->error == EMFILE)
			log_err("fio: try reducing/setting openfiles (failed at %u of %u)\n", td->nr_open_files, td->o.nr_files);

		return 1;
	}

	if (f->filetype == FIO_TYPE_PIPE) {
		if (td_random(td)) {
			log_err("fio: can't seek on pipes (no random io)\n");
			goto err;
		}
	}

	f->last_free_lookup = 0;
	f->last_completed_pos = 0;
	f->last_pos = f->file_offset;
	f->flags |= FIO_FILE_OPEN;
	f->flags &= ~FIO_FILE_CLOSING;

	if (td->io_ops->flags & FIO_DISKLESSIO)
		goto done;

	if (td->o.invalidate_cache && file_invalidate_cache(td, f))
		goto err;

	if (td->o.fadvise_hint &&
	    (f->filetype == FIO_TYPE_BD || f->filetype == FIO_TYPE_FILE)) {
		
		int flags;

		if (td_random(td))
			flags = POSIX_FADV_RANDOM;
		else
			flags = POSIX_FADV_SEQUENTIAL;

		if (fadvise(f->fd, f->file_offset, f->io_size, flags) < 0) {
			td_verror(td, errno, "fadvise");
			goto err;
		}
	}

	if (f->file_map)
		memset(f->file_map, 0, f->num_maps * sizeof(long));

done:
	log_file(td, f, FIO_LOG_OPEN_FILE);
	td->nr_open_files++;
	get_file(f);
	return 0;
err:
	if (td->io_ops->close_file)
		td->io_ops->close_file(td, f);
	return 1;
}

void td_io_close_file(struct thread_data *td, struct fio_file *f)
{
	if (!(f->flags & FIO_FILE_CLOSING))
		log_file(td, f, FIO_LOG_CLOSE_FILE);

	/*
	 * mark as closing, do real close when last io on it has completed
	 */
	f->flags |= FIO_FILE_CLOSING;

	put_file(td, f);
}
