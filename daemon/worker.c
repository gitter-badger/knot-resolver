/*  Copyright (C) 2014 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <uv.h>
#include <libknot/packet/pkt.h>
#include <libknot/internal/net.h>
#include <libknot/internal/mempool.h>

#include "daemon/worker.h"
#include "daemon/engine.h"
#include "daemon/io.h"

/** @internal Query resolution task. */
struct qr_task
{
	struct kr_request req;
	knot_pkt_t *next_query;
	uv_handle_t *next_handle;
	uv_timer_t timeout;
	union {
		uv_write_t tcp_send;
		uv_udp_send_t udp_send;
		uv_connect_t connect;
	} ioreq;
	struct {
		union {
			struct sockaddr_in ip4;
			struct sockaddr_in6 ip6;
		} addr;
		uv_handle_t *handle;
	} source;
	uint16_t iter_count;
	uint16_t flags;
};

/* Forward decls */
static int qr_task_step(struct qr_task *task, knot_pkt_t *packet);

static int parse_query(knot_pkt_t *query)
{
	/* Parse query packet. */
	int ret = knot_pkt_parse(query, 0);
	if (ret != KNOT_EOK) {
		return kr_error(EPROTO); /* Ignore malformed query. */
	}

	/* Check if at least header is parsed. */
	if (query->parsed < query->size) {
		return kr_error(EMSGSIZE);
	}

	return kr_ok();
}

static struct qr_task *qr_task_create(struct worker_ctx *worker, uv_handle_t *handle, const struct sockaddr *addr)
{
	mm_ctx_t pool;
	mm_ctx_mempool(&pool, MM_DEFAULT_BLKSIZE);

	/* Create worker task */
	struct engine *engine = worker->engine;
	struct qr_task *task = mm_alloc(&pool, sizeof(*task));
	memset(task, 0, sizeof(*task));
	if (!task) {
		mp_delete(pool.ctx);
		return NULL;
	}
	task->req.pool = pool;
	task->source.handle = handle;
	if (addr) {
		memcpy(&task->source.addr, addr, sockaddr_len(addr));
	}

	/* Create buffers */
	knot_pkt_t *next_query = knot_pkt_new(NULL, KNOT_EDNS_MAX_UDP_PAYLOAD, &task->req.pool);
	knot_pkt_t *answer = knot_pkt_new(NULL, KNOT_WIRE_MAX_PKTSIZE, &task->req.pool);
	if (!next_query || !answer) {
		mp_delete(pool.ctx);
		return NULL;
	}
	task->req.answer = answer;
	task->next_query = next_query;

	/* Start resolution */
	uv_timer_init(handle->loop, &task->timeout);
	task->timeout.data = task;
	kr_resolve_begin(&task->req, &engine->resolver, answer);
	return task;
}

static void qr_task_free(uv_handle_t *handle)
{
	struct qr_task *task = handle->data;
	mp_delete(task->req.pool.ctx);
}

static void qr_task_timeout(uv_timer_t *req)
{
	struct qr_task *task = req->data;
	if (task->next_handle) {
		io_stop_read(task->next_handle);
		qr_task_step(task, NULL);
	}
}

static void qr_task_on_send(uv_req_t* req, int status)
{
	struct qr_task *task = req->data;
	if (task) {
		/* Start reading answer */
		if (task->req.overlay.state != KNOT_STATE_NOOP) {
			if (status == 0 && task->next_handle) {
				io_start_read(task->next_handle);
			}
		} else { /* Finalize task */
			uv_timer_stop(&task->timeout);
			uv_close((uv_handle_t *)&task->timeout, qr_task_free);
		}
	}
}

static int qr_task_send(struct qr_task *task, uv_handle_t *handle, struct sockaddr *addr, knot_pkt_t *pkt)
{
	if (handle->type == UV_UDP) {
		uv_buf_t buf = { (char *)pkt->wire, pkt->size };
		uv_udp_send_t *req = &task->ioreq.udp_send;
		req->data = task;
		return uv_udp_send(req, (uv_udp_t *)handle, &buf, 1, addr, (uv_udp_send_cb)qr_task_on_send);
	} else {
		uint16_t pkt_size = htons(pkt->size);
		uv_buf_t buf[2] = {
			{ (char *)&pkt_size, sizeof(pkt_size) },
			{ (char *)pkt->wire, pkt->size }
		};
		uv_write_t *req = &task->ioreq.tcp_send;
		req->data = task;
		return uv_write(req, (uv_stream_t *)handle, buf, 2, (uv_write_cb)qr_task_on_send);
	}
}

static void qr_task_on_connect(uv_connect_t *connect, int status)
{
	uv_stream_t *handle = connect->handle;
	struct qr_task *task = connect->data;
	if (status != 0) { /* Failed to connect */
		qr_task_step(task, NULL);
	} else {
		qr_task_send(task, (uv_handle_t *)handle, NULL, task->next_query);
	}
}

static int qr_task_finalize(struct qr_task *task, int state)
{
	kr_resolve_finish(&task->req, state);
	qr_task_send(task, task->source.handle, (struct sockaddr *)&task->source.addr, task->req.answer);
	return state == KNOT_STATE_DONE ? 0 : kr_error(EIO);
}

static int qr_task_step(struct qr_task *task, knot_pkt_t *packet)
{
	/* Cancel timeout if active, close handle. */
	if (task->next_handle) {
		uv_close(task->next_handle, (uv_close_cb) free);
		uv_timer_stop(&task->timeout);
		task->next_handle = NULL;
	}

	/* Consume input and produce next query */
	int sock_type = -1;
	struct sockaddr *addr = NULL;
	knot_pkt_t *next_query = task->next_query;
	int state = kr_resolve_consume(&task->req, packet);
	while (state == KNOT_STATE_PRODUCE) {
		state = kr_resolve_produce(&task->req, &addr, &sock_type, next_query);
	}

	/* We're done, no more iterations needed */
	if (state & (KNOT_STATE_DONE|KNOT_STATE_FAIL)) {
		return qr_task_finalize(task, state);
	} else if (++task->iter_count > KR_ITER_LIMIT) {
		return qr_task_finalize(task, KNOT_STATE_FAIL);
	}

	/* Create connection for iterative query */
	uv_handle_t *source_handle = task->source.handle;
	task->next_handle = io_create(source_handle->loop, sock_type);
	if (task->next_handle == NULL) {
		return qr_task_finalize(task, KNOT_STATE_FAIL);
	}

	/* Connect or issue query datagram */
	task->next_handle->data = task;
	if (sock_type == SOCK_STREAM) {
		uv_connect_t *connect = &task->ioreq.connect;
		if (uv_tcp_connect(connect, (uv_tcp_t *)task->next_handle, addr, qr_task_on_connect) != 0) {
			return qr_task_step(task, NULL);
		}
		connect->data = task;
	} else {
		if (qr_task_send(task, task->next_handle, addr, next_query) != 0) {
			return qr_task_step(task, NULL);
		}
	}

	/* Start next step with timeout */
	uv_timer_start(&task->timeout, qr_task_timeout, KR_CONN_RTT_MAX, 0);
	return kr_ok();
}

int worker_exec(struct worker_ctx *worker, uv_handle_t *handle, knot_pkt_t *query, const struct sockaddr* addr)
{
	if (!worker) {
		return kr_error(EINVAL);
	}

	/* Parse query */
	int ret = parse_query(query);

	/* Start new task on master sockets, or resume existing */
	struct qr_task *task = handle->data;
	bool is_master_socket = (!task);
	if (is_master_socket) {
		/* Ignore badly formed queries or responses. */
		if (ret != 0 || knot_wire_get_qr(query->wire)) {
			return kr_error(EINVAL); /* Ignore. */
		}
		task = qr_task_create(worker, handle, addr);
		if (!task) {
			return kr_error(ENOMEM);
		}
	}

	/* Consume input and produce next query */
	return qr_task_step(task, query);
}
