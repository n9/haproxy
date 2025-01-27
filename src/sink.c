/*
 * Event sink management
 *
 * Copyright (C) 2000-2019 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <sys/uio.h>
#include <common/compat.h>
#include <common/config.h>
#include <common/ist.h>
#include <common/mini-clist.h>
#include <proto/log.h>
#include <proto/sink.h>

struct list sink_list = LIST_HEAD_INIT(sink_list);

struct sink *sink_find(const char *name)
{
	struct sink *sink;

	list_for_each_entry(sink, &sink_list, sink_list)
		if (strcmp(sink->name, name) == 0)
			return sink;
	return NULL;
}

/* creates a new sink and adds it to the list, it's still generic and not fully
 * initialized. Returns NULL on allocation failure. If another one already
 * exists with the same name, it will be returned. The caller can detect it as
 * a newly created one has type SINK_TYPE_NEW.
 */
static struct sink *__sink_new(const char *name, const char *desc, enum sink_fmt fmt)
{
	struct sink *sink;

	sink = sink_find(name);
	if (sink)
		goto end;

	sink = malloc(sizeof(*sink));
	if (!sink)
		goto end;

	sink->name = name;
	sink->desc = desc;
	sink->fmt  = fmt;
	sink->type = SINK_TYPE_NEW;
	/* set defaults for syslog ones */
	sink->syslog_facility = 0;
	sink->syslog_minlvl   = 0;
	sink->maxlen = MAX_SYSLOG_LEN;
	/* address will be filled by the caller if needed */
	sink->ctx.fd = -1;
	sink->ctx.dropped = 0;
	HA_RWLOCK_INIT(&sink->ctx.lock);
	LIST_ADDQ(&sink_list, &sink->sink_list);
 end:
	return sink;
}

/* creates a sink called <name> of type FD associated to fd <fd>, format <fmt>,
 * and description <desc>. Returns NULL on allocation failure or conflict.
 * Perfect duplicates are merged (same type, fd, and name).
 */
struct sink *sink_new_fd(const char *name, const char *desc, enum sink_fmt fmt, int fd)
{
	struct sink *sink;

	sink = __sink_new(name, desc, fmt);
	if (!sink || (sink->type == SINK_TYPE_FD && sink->ctx.fd == fd))
		goto end;

	if (sink->type != SINK_TYPE_NEW) {
		sink = NULL;
		goto end;
	}

	sink->type = SINK_TYPE_FD;
	sink->ctx.fd = fd;
 end:
	return sink;
}

/* tries to send <nmsg> message parts (up to 8, ignored above) from message
 * array <msg> to sink <sink>. Formating according to the sink's preference is
 * done here. Lost messages are accounted for in the sink's counter.
 */
void sink_write(struct sink *sink, const struct ist msg[], size_t nmsg)
{
	struct iovec iovec[10];
	char short_hdr[4];
	size_t maxlen = sink->maxlen ? sink->maxlen : ~0;
	size_t sent = 0;
	int vec = 0;

	/* keep one char for a possible trailing '\n' in any case */
	maxlen--;

	if (sink->fmt == SINK_FMT_SHORT) {
		short_hdr[0] = '<';
		short_hdr[1] = '0' + sink->syslog_minlvl;
		short_hdr[2] = '>';

		iovec[vec].iov_base = short_hdr;
		iovec[vec].iov_len  = MIN(maxlen, 3);
		maxlen -= iovec[vec].iov_len;
		vec++;
	}

	/* copy the remaining entries from the original message. Skip empty fields and
	 * truncate the whole message to maxlen.
	 */
	while (nmsg && vec < (sizeof(iovec) / sizeof(iovec[0]) - 1)) {
		iovec[vec].iov_base = msg->ptr;
		iovec[vec].iov_len  = MIN(maxlen, msg->len);
		maxlen -= iovec[vec].iov_len;
		if (iovec[vec].iov_len)
			vec++;
		msg++; nmsg--;
	}

	if (sink->type == SINK_TYPE_FD) {
		/* For the FD we always emit the trailing \n. It was already provisioned above. */
		iovec[vec].iov_base = "\n";
		iovec[vec].iov_len  = 1;
		vec++;

		HA_RWLOCK_WRLOCK(LOGSRV_LOCK, &sink->ctx.lock);
		sent = writev(sink->ctx.fd, iovec, vec);
		HA_RWLOCK_WRUNLOCK(LOGSRV_LOCK, &sink->ctx.lock);
		/* sent > 0 if the message was delivered */
	}

	/* account for errors now */
	if (sent <= 0)
		HA_ATOMIC_ADD(&sink->ctx.dropped, 1);
}

static void sink_init()
{
	sink_new_fd("stdout", "standard output (fd#1)", SINK_FMT_RAW, 1);
	sink_new_fd("stderr", "standard output (fd#2)", SINK_FMT_RAW, 2);
}

INITCALL0(STG_REGISTER, sink_init);

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
