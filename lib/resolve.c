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

#include <stdio.h>
#include <sys/fcntl.h>

#include <libknot/internal/mempool.h>
#include <libknot/rrtype/rdname.h>
#include <libknot/descriptor.h>
#include <libknot/internal/net.h>

#include "lib/layer.h"
#include "lib/rplan.h"
#include "lib/resolve.h"

#define DEBUG_MSG(fmt...) QRDEBUG(kr_rplan_current(rplan), "resl",  fmt)

/** Invalidate current NS/addr pair. */
static int invalidate_ns(struct kr_rplan *rplan, struct kr_query *qry)
{
	uint8_t *addr = kr_nsrep_inaddr(qry->ns.addr);
	size_t addr_len = kr_nsrep_inaddr_len(qry->ns.addr);
	knot_rdata_t rdata[knot_rdata_array_size(addr_len)];
	knot_rdata_init(rdata, addr_len, addr, 0);
	return kr_zonecut_del(&qry->zone_cut, qry->ns.name, rdata);
}

static int ns_resolve_addr(struct kr_query *qry, struct kr_request *param)
{
	struct kr_rplan *rplan = &param->rplan;
	if (kr_rplan_satisfies(qry, qry->ns.name, KNOT_CLASS_IN, KNOT_RRTYPE_A) ||
	    kr_rplan_satisfies(qry, qry->ns.name, KNOT_CLASS_IN, KNOT_RRTYPE_AAAA) ||
	    qry->flags & QUERY_AWAIT_ADDR) {
		DEBUG_MSG("=> dependency loop, bailing out\n");
		kr_rplan_pop(rplan, qry);
		return KNOT_STATE_PRODUCE;
	}

	(void) kr_rplan_push(rplan, qry, qry->ns.name, KNOT_CLASS_IN, KNOT_RRTYPE_AAAA);
	(void) kr_rplan_push(rplan, qry, qry->ns.name, KNOT_CLASS_IN, KNOT_RRTYPE_A);
	qry->flags |= QUERY_AWAIT_ADDR;
	return KNOT_STATE_PRODUCE;
}

static void prepare_layers(struct kr_request *param)
{
	struct kr_context *ctx = param->ctx;
	for (size_t i = 0; i < ctx->modules->len; ++i) {
		struct kr_module *mod = &ctx->modules->at[i];
		if (mod->layer) {
			knot_overlay_add(&param->overlay, mod->layer(), param);
		}
	}
}

static int connected(struct sockaddr *addr, int proto, struct timeval *timeout)
{
	unsigned flags = (proto == SOCK_STREAM) ? O_NONBLOCK : 0;
	int fd = net_connected_socket(proto, (struct sockaddr_storage *)addr, NULL, flags);
	if (fd < 0) {
		return kr_error(ECONNREFUSED);
	}

	/* Workaround for timeout, as we have no control over
	 * connect() time limit in blocking mode. */
	if (proto == SOCK_STREAM) {
		fd_set set;
		FD_ZERO(&set);
		FD_SET(fd, &set);
		int ret = select(fd + 1, NULL, &set, NULL, timeout);
		if (ret == 0) {
			close(fd);
			return kr_error(ETIMEDOUT);
		}
		if (ret < 0) {
			close(fd);
			return kr_error(ECONNREFUSED);
		}
		fcntl(fd, F_SETFL, 0);
	}

	return fd;
}

static int sendrecv(struct sockaddr *addr, int proto, const knot_pkt_t *query, knot_pkt_t *resp)
{
	struct timeval timeout = { KR_CONN_RTT_MAX / 1000, 0 };
	auto_close int fd = connected(addr, proto, &timeout);
	resp->size = 0;
	if (fd < 0) {
		return fd;
	}

	/* Send packet */
	int ret = 0;
	if (proto == SOCK_STREAM) {
		ret = tcp_send_msg(fd, query->wire, query->size, &timeout);
	} else {
		ret = udp_send_msg(fd, query->wire, query->size, NULL);
	}
	if (ret != query->size) {
		return kr_error(EIO);
	}

	/* Receive it */
	if (proto == SOCK_STREAM) {
		ret = tcp_recv_msg(fd, resp->wire, resp->max_size, &timeout);
	} else {
		ret = udp_recv_msg(fd, resp->wire, resp->max_size, &timeout);
	}
	if (ret <= 0) {
		return kr_error(ETIMEDOUT);
	}

	/* Parse and return */
	resp->size = ret;
	return knot_pkt_parse(resp, 0);
}

static int edns_put(knot_pkt_t *pkt)
{
	/* Reclaim reserved size. */
	int ret = knot_pkt_reclaim(pkt, knot_edns_wire_size(pkt->opt_rr));
	if (ret != 0) {
		return ret;
	}
	/* Write to packet. */
	assert(pkt->current == KNOT_ADDITIONAL);
	return knot_pkt_put(pkt, KNOT_COMPR_HINT_NONE, pkt->opt_rr, KNOT_PF_FREE);
}

static int edns_create(knot_pkt_t *pkt, knot_pkt_t *template)
{
	/* Create empty OPT RR */
	pkt->opt_rr = mm_alloc(&pkt->mm, sizeof(*pkt->opt_rr));
	if (!pkt->opt_rr) {
		return kr_error(ENOMEM);
	}
	int ret = knot_edns_init(pkt->opt_rr, KR_EDNS_PAYLOAD, 0, KR_EDNS_VERSION, &pkt->mm);
	if (ret != 0) {
		return ret;
	}
	/* Set DO bit if set (DNSSEC requested). */
	if (knot_pkt_has_dnssec(template)) {
		knot_edns_set_do(pkt->opt_rr);
	}
	return knot_pkt_reserve(pkt, knot_edns_wire_size(pkt->opt_rr));
}

static int answer_prepare(knot_pkt_t *answer, knot_pkt_t *query)
{
	if (!knot_wire_get_rd(query->wire)) {
		return kr_error(ENOSYS); /* Only recursive service */
	}
	if (knot_pkt_init_response(answer, query) != 0) {
		return kr_error(ENOMEM); /* Failed to initialize answer */
	}
	/* Handle EDNS in the query */
	if (knot_pkt_has_edns(query)) {
		int ret = edns_create(answer, query);
		if (ret != 0){
			return ret;
		}
	}
	return kr_ok();
}

static int answer_finalize(knot_pkt_t *answer)
{
	knot_pkt_begin(answer, KNOT_ADDITIONAL);
	if (answer->opt_rr) {
		return edns_put(answer);

	}
	return kr_ok();
}

static int query_finalize(struct kr_request *request, knot_pkt_t *pkt)
{
	int ret = 0;
	struct kr_query *qry = kr_rplan_current(&request->rplan);
	knot_pkt_begin(pkt, KNOT_ADDITIONAL);
	if (!(qry->flags & QUERY_SAFEMODE)) {
		ret = edns_create(pkt, request->answer);
		if (ret == 0) {
			ret = edns_put(pkt);
		}
	}
	return ret;
}

int kr_resolve(struct kr_context* ctx, knot_pkt_t *answer,
               const knot_dname_t *qname, uint16_t qclass, uint16_t qtype)
{
	if (ctx == NULL || answer == NULL || qname == NULL) {
		return kr_error(EINVAL);
	}

	/* Create memory pool */
	mm_ctx_t pool;
	mm_ctx_mempool(&pool, MM_DEFAULT_BLKSIZE);
	knot_pkt_t *query = knot_pkt_new(NULL, KNOT_EDNS_MAX_UDP_PAYLOAD, &pool);
	knot_pkt_t *resp = knot_pkt_new(NULL, KNOT_WIRE_MAX_PKTSIZE, &pool);
	if (!query || !resp) {
		mp_delete(pool.ctx);
		return kr_error(ENOMEM);
	}

	/* Initialize context. */
	struct kr_request request;
	request.pool = pool;
	kr_resolve_begin(&request, ctx, answer);
#ifndef NDEBUG
	struct kr_rplan *rplan = &request.rplan; /* for DEBUG_MSG */
#endif
	/* Resolve query, iteratively */
	int proto = 0;
	struct sockaddr *addr = NULL;
	unsigned iter_count = 0;
	int state = kr_resolve_query(&request, qname, qclass, qtype);
	while (state == KNOT_STATE_PRODUCE) {
		/* Hardlimit on iterative queries */
		if (++iter_count > KR_ITER_LIMIT) {
			DEBUG_MSG("iteration limit %d reached\n", KR_ITER_LIMIT);
			state = KNOT_STATE_FAIL;
			break;
		}
		/* Produce next query or finish */
		state = kr_resolve_produce(&request, &addr, &proto, query);
		while (state == KNOT_STATE_CONSUME) {
			/* Get answer from nameserver and consume it */
			int ret = sendrecv(addr, proto, query, resp);
			if (ret != 0) {
				DEBUG_MSG("sendrecv: %s\n", kr_strerror(ret));
			}
			state = kr_resolve_consume(&request, resp);
			knot_pkt_clear(resp);
		}
		knot_pkt_clear(query);
	}

	/* Cleanup */
	kr_resolve_finish(&request, state);
	mp_delete(pool.ctx);
	return state == KNOT_STATE_DONE ? 0 : kr_error(EIO);
}

int kr_resolve_begin(struct kr_request *request, struct kr_context *ctx, knot_pkt_t *answer)
{
	/* Initialize request */
	kr_rplan_init(&request->rplan, ctx, &request->pool);
	knot_overlay_init(&request->overlay, &request->pool);
	request->ctx = ctx;
	request->answer = answer;
	prepare_layers(request);

	/* Expect first query */
	return KNOT_STATE_CONSUME;
}

int kr_resolve_query(struct kr_request *request, const knot_dname_t *qname, uint16_t qclass, uint16_t qtype)
{
	struct kr_rplan *rplan = &request->rplan;
	struct kr_query *qry = kr_rplan_push(rplan, NULL, qname, qclass, qtype);
	if (!qry) {
		return KNOT_STATE_FAIL;
	}

	/* Initialize answer packet */
	knot_pkt_t *answer = request->answer;
	knot_wire_set_qr(answer->wire);
	knot_wire_clear_aa(answer->wire);
	knot_wire_set_ra(answer->wire);
	knot_wire_set_rcode(answer->wire, KNOT_RCODE_NOERROR);

	/* Expect answer */
	return KNOT_STATE_PRODUCE;
}

int kr_resolve_consume(struct kr_request *request, knot_pkt_t *packet)
{
	struct kr_rplan *rplan = &request->rplan;
	struct kr_query *qry = kr_rplan_current(rplan);

	/* Empty resolution plan, push packet as the new query */
	if (packet && kr_rplan_empty(rplan)) {
		if (answer_prepare(request->answer, packet) != 0) {
			return KNOT_STATE_FAIL;
		}
		/* Start query resolution */
		const knot_dname_t *qname = knot_pkt_qname(packet);
		uint16_t qclass = knot_pkt_qclass(packet);
		uint16_t qtype = knot_pkt_qtype(packet);
		return kr_resolve_query(request, qname, qclass, qtype);
	}

	/* Different processing for network error */
	int state = KNOT_STATE_FAIL;
	if (!packet || packet->size == 0) {
		/* Network error, retry over TCP. */
		if (!(qry->flags & QUERY_TCP)) {
			/** @todo This should just penalize UDP and elect next best. */
			DEBUG_MSG("=> ns unreachable, retrying over TCP\n");
			qry->flags |= QUERY_TCP;
			return KNOT_STATE_CONSUME; /* Try again */
		}
	} else {
		state = knot_overlay_consume(&request->overlay, packet);
	}

	/* Resolution failed, invalidate current NS and reset to UDP. */
	if (state == KNOT_STATE_FAIL) {
		DEBUG_MSG("=> resolution failed, invalidating\n");
		if (invalidate_ns(rplan, qry) == 0) {
			qry->flags &= ~QUERY_TCP;
		}
	}

	/* Pop query if resolved. */
	if (qry->flags & QUERY_RESOLVED) {
		kr_rplan_pop(rplan, qry);
	}

	knot_overlay_reset(&request->overlay);
	return kr_rplan_empty(&request->rplan) ? KNOT_STATE_DONE : KNOT_STATE_PRODUCE;
}

int kr_resolve_produce(struct kr_request *request, struct sockaddr **dst, int *type, knot_pkt_t *packet)
{
	struct kr_rplan *rplan = &request->rplan;
	struct kr_query *qry = kr_rplan_current(rplan);
	
	/* No query left for resolution */
	if (kr_rplan_empty(rplan)) {
		return KNOT_STATE_FAIL;
	}

#ifndef NDEBUG
	char name_str[KNOT_DNAME_MAXLEN], type_str[16];
	knot_dname_to_str(name_str, qry->sname, sizeof(name_str));
	knot_rrtype_to_string(qry->stype, type_str, sizeof(type_str));
	DEBUG_MSG("query '%s %s'\n", type_str, name_str);
#endif

	/* Resolve current query and produce dependent or finish */
	int state = knot_overlay_produce(&request->overlay, packet);
	if (state != KNOT_STATE_FAIL && knot_wire_get_qr(packet->wire)) {
		/* Produced an answer, consume it. */
		request->overlay.state = KNOT_STATE_CONSUME;
		state = knot_overlay_consume(&request->overlay, packet);
	}
	switch(state) {
	case KNOT_STATE_FAIL: return state; break;
	case KNOT_STATE_CONSUME: break;
	case KNOT_STATE_DONE:
	default: /* Current query is done */
		knot_overlay_reset(&request->overlay);
		kr_rplan_pop(rplan, qry);
		return kr_rplan_empty(rplan) ? KNOT_STATE_DONE : KNOT_STATE_PRODUCE;
	}

	/* Elect best nameserver candidate */
	kr_nsrep_elect(&qry->ns, &qry->zone_cut.nsset);
	if (qry->ns.score < KR_NS_VALID) {
		DEBUG_MSG("=> no valid NS left\n");
		knot_overlay_reset(&request->overlay);
		kr_rplan_pop(rplan, qry);
		return KNOT_STATE_PRODUCE;
	} else {
		if (qry->ns.addr.ip.sa_family == AF_UNSPEC) {
			DEBUG_MSG("=> ns missing A/AAAA, fetching\n");
			knot_overlay_reset(&request->overlay);
			return ns_resolve_addr(qry, request);
		}
	}

#ifndef NDEBUG
	char qname_str[KNOT_DNAME_MAXLEN], zonecut_str[KNOT_DNAME_MAXLEN], ns_str[SOCKADDR_STRLEN];
	knot_dname_to_str(qname_str, knot_pkt_qname(packet), sizeof(qname_str));
	struct sockaddr *addr = &qry->ns.addr.ip;
	inet_ntop(addr->sa_family, kr_nsrep_inaddr(qry->ns.addr), ns_str, sizeof(ns_str));
	knot_dname_to_str(zonecut_str, qry->zone_cut.name, sizeof(zonecut_str));
	DEBUG_MSG("=> querying: '%s' zone cut: '%s' m12n: '%s'\n", ns_str, zonecut_str, qname_str);
#endif

	/* Prepare additional query */
	int ret = query_finalize(request, packet);
	if (ret != 0) {
		return KNOT_STATE_FAIL;
	}
	*dst = &qry->ns.addr.ip;
	*type = (qry->flags & QUERY_TCP) ? SOCK_STREAM : SOCK_DGRAM;
	return state;
}

int kr_resolve_finish(struct kr_request *request, int state)
{
#ifndef NDEBUG
	struct kr_rplan *rplan = &request->rplan;
	DEBUG_MSG("finished: %d, mempool: %zu B\n", state, (size_t) mp_total_size(request->pool.ctx));
#endif
	/* Finalize answer */
	if (answer_finalize(request->answer) != 0) {
		state = KNOT_STATE_FAIL;
	}
	/* Error during procesing, internal failure */
	knot_overlay_finish(&request->overlay);
	if (state != KNOT_STATE_DONE) {
		knot_pkt_t *answer = request->answer;
		if (knot_wire_get_rcode(answer->wire) == KNOT_RCODE_NOERROR) {
			knot_wire_set_rcode(answer->wire, KNOT_RCODE_SERVFAIL);
		}
	}
	/* Clean up. */
	knot_overlay_deinit(&request->overlay);
	request->overlay.state = KNOT_STATE_NOOP;
	kr_rplan_deinit(&request->rplan);
	return KNOT_STATE_DONE;
}
