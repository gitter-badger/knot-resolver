/*  Copyright (C) 2015 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

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

#include <libknot/descriptor.h>
#include <libknot/rrset.h>
#include <libknot/rrtype/soa.h>

#include "lib/layer/iterate.h"
#include "lib/cache.h"
#include "lib/module.h"

#define DEBUG_MSG(fmt...) QRDEBUG(kr_rplan_current(rplan), " pc ",  fmt)

static inline uint8_t get_tag(knot_pkt_t *pkt)
{
	return knot_pkt_has_dnssec(pkt) ? KR_CACHE_SEC : KR_CACHE_PKT;
}

static int begin(knot_layer_t *ctx, void *module_param)
{
	ctx->data = module_param;
	return ctx->state;
}

static int loot_cache(namedb_txn_t *txn, knot_pkt_t *pkt, uint8_t tag, uint32_t timestamp)
{
	const knot_dname_t *qname = knot_pkt_qname(pkt);
	uint16_t rrtype = knot_pkt_qtype(pkt);
	struct kr_cache_entry *entry;
	entry = kr_cache_peek(txn, tag, qname, rrtype, &timestamp);
	if (!entry) { /* Not in the cache */
		return kr_error(ENOENT);
	}

	/* Copy answer, keep the original message id */
	if (entry->count <= pkt->max_size) {
		/* Keep original header and copy cached */
		uint8_t header[KNOT_WIRE_HEADER_SIZE];
		memcpy(header, pkt->wire, sizeof(header));
		memcpy(pkt->wire, entry->data, entry->count);
		pkt->size = entry->count;
		pkt->parsed = 0;
		pkt->reserved = 0;
		/* Restore header bits */
		knot_wire_set_id(pkt->wire, knot_wire_get_id(header));
	}
	return kr_ok();
}

static int peek(knot_layer_t *ctx, knot_pkt_t *pkt)
{
	struct kr_request *req = ctx->data;
	struct kr_rplan *rplan = &req->rplan;
	struct kr_query *qry = kr_rplan_current(rplan);
	if (!qry || ctx->state & (KNOT_STATE_DONE|KNOT_STATE_FAIL)) {
		return ctx->state;
	}

	/* Fetch packet from cache */
	namedb_txn_t txn;
	struct kr_cache *cache = req->ctx->cache;
	if (kr_cache_txn_begin(cache, &txn, NAMEDB_RDONLY) != 0) {
		return ctx->state;
	}
	uint32_t timestamp = qry->timestamp.tv_sec;
	if (loot_cache(&txn, pkt, get_tag(req->answer), timestamp) != 0) {
		kr_cache_txn_abort(&txn);
		return ctx->state;
	}

	/* Mark as solved from cache */
	DEBUG_MSG("=> satisfied from cache\n");
	qry->flags |= QUERY_CACHED;
	knot_wire_set_qr(pkt->wire);
	kr_cache_txn_abort(&txn);
	return KNOT_STATE_DONE;
}

static uint32_t packet_ttl(knot_pkt_t *pkt)
{
	uint32_t ttl = 0;
	/* Fetch SOA from authority. */
	const knot_pktsection_t *ns = knot_pkt_section(pkt, KNOT_AUTHORITY);
	for (unsigned i = 0; i < ns->count; ++i) {
		const knot_rrset_t *rr = knot_pkt_rr(ns, i);
		if (rr->type == KNOT_RRTYPE_SOA) {
			ttl = knot_soa_minimum(&rr->rrs);
			break;
		}
	}
	/* @todo Fetch TTL from NSEC* proof */
	return ttl;
}

static int stash(knot_layer_t *ctx)
{
	struct kr_request *req = ctx->data;
	struct kr_rplan *rplan = &req->rplan;
	if (EMPTY_LIST(rplan->resolved) || ctx->state == KNOT_STATE_FAIL) {
		return ctx->state; /* Don't cache anything if failed. */
	}
	knot_pkt_t *pkt = req->answer;
	struct kr_query *qry = TAIL(rplan->resolved);
	if (qry->flags & QUERY_CACHED || kr_response_classify(pkt) == PKT_NOERROR) {
		return ctx->state; /* Cache only negative, not-cached answers. */
	}
	uint32_t ttl = packet_ttl(pkt);
	if (ttl == 0) {
		return ctx->state; /* No useable TTL, can't cache this. */
	}

	/* Open write transaction and prepare answer */
	namedb_txn_t txn;
	if (kr_cache_txn_begin(req->ctx->cache, &txn, 0) != 0) {
		return ctx->state; /* Couldn't acquire cache, ignore. */
	}
	const knot_dname_t *qname = knot_pkt_qname(pkt);
	uint16_t qtype = knot_pkt_qtype(pkt);
	namedb_val_t data = { pkt->wire, pkt->size };
	struct kr_cache_entry header = {
		.timestamp = qry->timestamp.tv_sec,
		.ttl = ttl,
		.count = data.len
	};

	/* Stash answer in the cache */
	int ret = kr_cache_insert(&txn, get_tag(pkt), qname, qtype, &header, data);	
	if (ret == KNOT_ESPACE) {
		kr_cache_txn_abort(&txn);
	} else {
		DEBUG_MSG("=> answer cached for TTL=%u\n", ttl);
		kr_cache_txn_commit(&txn);
	}
	return ctx->state;
}

/** Module implementation. */
const knot_layer_api_t *pktcache_layer(void)
{
	static const knot_layer_api_t _layer = {
		.begin   = &begin,
		.produce = &peek,
		.finish  = &stash
	};

	return &_layer;
}

KR_MODULE_EXPORT(pktcache)