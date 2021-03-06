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

#include <libknot/descriptor.h>
#include <libknot/rrtype/rdname.h>
#include <libknot/packet/wire.h>
#include <libknot/descriptor.h>
#include <libknot/rrtype/aaaa.h>

#include "lib/zonecut.h"
#include "lib/rplan.h"
#include "lib/defines.h"
#include "lib/layer.h"
#include "lib/generic/pack.h"

/* Root hint descriptor. */
struct hint_info {
	const knot_dname_t *name;
	const uint8_t *addr;
};

/* Initialize with SBELT name servers. */
#define U8(x) (const uint8_t *)(x)
#define HINT_COUNT 13
#define HINT_ADDRLEN sizeof(struct in_addr)
static const struct hint_info SBELT[HINT_COUNT] = {
        { U8("\x01""a""\x0c""root-servers""\x03""net"), U8("\xc6)\x00\x04")    }, /* 198.41.0.4 */
        { U8("\x01""b""\x0c""root-servers""\x03""net"), U8("\xc0\xe4O\xc9")    }, /* 192.228.79.201 */
        { U8("\x01""c""\x0c""root-servers""\x03""net"), U8("\xc6)\x00\x04")    }, /* 192.33.4.12 */
        { U8("\x01""d""\x0c""root-servers""\x03""net"), U8("\xc7\x07[\r")      }, /* 199.7.91.13 */
        { U8("\x01""e""\x0c""root-servers""\x03""net"), U8("\xc0\xcb\xe6\n")   }, /* 192.203.230.10 */
        { U8("\x01""f""\x0c""root-servers""\x03""net"), U8("\xc0\x05\x05\xf1") }, /* 192.5.5.241 */
        { U8("\x01""g""\x0c""root-servers""\x03""net"), U8("\xc0p$\x04")       }, /* 192.112.36.4 */
        { U8("\x01""h""\x0c""root-servers""\x03""net"), U8("\x80?\x025")       }, /* 128.63.2.53 */
        { U8("\x01""i""\x0c""root-servers""\x03""net"), U8("\xc0$\x94\x11")    }, /* 192.36.148.17 */
        { U8("\x01""j""\x0c""root-servers""\x03""net"), U8("\xc0:\x80\x1e")    }, /* 192.58.128.30 */
        { U8("\x01""k""\x0c""root-servers""\x03""net"), U8("\xc1\x00\x0e\x81") }, /* 193.0.14.129 */
        { U8("\x01""l""\x0c""root-servers""\x03""net"), U8("\xc7\x07S*")       }, /* 199.7.83.42 */
        { U8("\x01""m""\x0c""root-servers""\x03""net"), U8("\xca\x0c\x1b!")    }, /* 202.12.27.33 */
};

static void update_cut_name(struct kr_zonecut *cut, const knot_dname_t *name)
{
	if (knot_dname_is_equal(name, cut->name)) {
		return;
	}
	knot_dname_t *next_name = knot_dname_copy(name, cut->pool);
	mm_free(cut->pool, cut->name);
	cut->name = next_name;
}

int kr_zonecut_init(struct kr_zonecut *cut, const knot_dname_t *name, mm_ctx_t *pool)
{
	if (!cut || !name) {
		return kr_error(EINVAL);
	}

	cut->name = knot_dname_copy(name, pool);
	cut->pool = pool;
	cut->nsset = map_make();
	cut->nsset.malloc = (map_alloc_f) mm_alloc;
	cut->nsset.free = (map_free_f) mm_free;
	cut->nsset.baton = pool;
	return kr_ok();
}

static int free_addr_set(const char *k, void *v, void *baton)
{
	pack_t *pack = v;
	pack_clear_mm(*pack, mm_free, baton);
	mm_free(baton, pack);
	return kr_ok();
}

void kr_zonecut_deinit(struct kr_zonecut *cut)
{
	if (!cut) {
		return;
	}
	mm_free(cut->pool, cut->name);
	map_walk(&cut->nsset, free_addr_set, cut->pool);
	map_clear(&cut->nsset);
}

void kr_zonecut_set(struct kr_zonecut *cut, const knot_dname_t *name)
{
	if (!cut || !name) {
		return;
	}
	kr_zonecut_deinit(cut);
	kr_zonecut_init(cut, name, cut->pool);
}

static int copy_addr_set(const char *k, void *v, void *baton)
{
	pack_t *addr_set = v;
	struct kr_zonecut *dst = baton;
	/* Clone addr_set pack */
	pack_t *new_set = mm_alloc(dst->pool, sizeof(*new_set));
	if (!new_set) {
		return kr_error(ENOMEM);
	}
	pack_init(*new_set);
	/* Clone data only if needed */
	if (addr_set->len > 0) {
		new_set->at = mm_alloc(dst->pool, addr_set->len);
		if (!new_set->at) {
			mm_free(dst->pool, new_set);
			return kr_error(ENOMEM);
		}
		memcpy(new_set->at, addr_set->at, addr_set->len);
		new_set->len = addr_set->len;
		new_set->cap = addr_set->len;
	}
	/* Reinsert */
	if (map_set(&dst->nsset, k, new_set) != 0) {
		pack_clear_mm(*new_set, mm_free, dst->pool);
		mm_free(dst->pool, new_set);
		return kr_error(ENOMEM);
	}
	return kr_ok();
}

int kr_zonecut_copy(struct kr_zonecut *dst, const struct kr_zonecut *src)
{
	if (!dst || !src) {
		return kr_error(EINVAL);
	}
	/* We're not touching src nsset, I promise */
	return map_walk((map_t *)&src->nsset, copy_addr_set, dst);
}

/** @internal Filter ANY or loopback addresses. */
static bool is_valid_addr(uint8_t *addr, size_t len)
{
	if (len == sizeof(struct in_addr)) {
		/* Filter ANY and 127.0.0.0/8 */
		uint32_t ip_host = ntohl(*(uint32_t *)(addr));
		if (ip_host == 0 || (ip_host & 0xff000000) == 0x7f000000) {
			return false;
		}
	} else if (len == sizeof(struct in6_addr)) {
		struct in6_addr ip6_mask;
		memset(&ip6_mask, 0, sizeof(ip6_mask));
		/* All except last byte are zeroed, last byte defines ANY/::1 */
		if (memcmp(addr, ip6_mask.s6_addr, sizeof(ip6_mask.s6_addr) - 1) == 0) {
			return (addr[len - 1] > 1);
		}
	}
	return true;
}

int kr_zonecut_add(struct kr_zonecut *cut, const knot_dname_t *ns, const knot_rdata_t *rdata)
{
	if (!cut || !ns) {
		return kr_error(EINVAL);
	}
	/* Fetch/insert nameserver. */
	pack_t *pack = kr_zonecut_find(cut, ns);
	if (pack == NULL) {
		pack = mm_alloc(cut->pool, sizeof(*pack));
		if (!pack || (map_set(&cut->nsset, (const char *)ns, pack) != 0)) {
			mm_free(cut->pool, pack);
			return kr_error(ENOMEM);
		}
		pack_init(*pack);
	}
	/* Insert data (if has any) */
	if (rdata == NULL) {
		return kr_ok();
	}
	/* Check for invalid */
	uint16_t rdlen = knot_rdata_rdlen(rdata);
	uint8_t *raw_addr = knot_rdata_data(rdata);
	if (!is_valid_addr(raw_addr, rdlen)) {
		return kr_error(EILSEQ);
	}
	/* Check for duplicates */
	if (pack_obj_find(pack, raw_addr, rdlen)) {
		return kr_ok();
	}
	/* Push new address */
	int ret = pack_reserve_mm(*pack, 1, rdlen, mm_reserve, cut->pool);
	if (ret != 0) {
		return kr_error(ENOMEM);
	}
	return pack_obj_push(pack, knot_rdata_data(rdata), rdlen);
}

int kr_zonecut_del(struct kr_zonecut *cut, const knot_dname_t *ns, const knot_rdata_t *rdata)
{
	if (!cut || !ns) {
		return kr_error(EINVAL);
	}

	/* Find the address list. */
	int ret = kr_ok();
	pack_t *pack = kr_zonecut_find(cut, ns);
	if (pack == NULL) {
		return kr_error(ENOENT);
	}
	/* Remove address from the pack. */
	if (rdata) {
		ret = pack_obj_del(pack, knot_rdata_data(rdata), knot_rdata_rdlen(rdata));
	}
	/* No servers left, remove NS from the set. */
	if (pack->len == 0) {
		free_addr_set((const char *)ns, pack, cut->pool);
		return map_del(&cut->nsset, (const char *)ns);
	}

	return ret;
}

pack_t *kr_zonecut_find(struct kr_zonecut *cut, const knot_dname_t *ns)
{
	if (!cut || !ns) {
		return NULL;
	}

	const char *key = (const char *)ns;
	map_t *nsset = &cut->nsset;
	return map_get(nsset, key);
}

int kr_zonecut_set_sbelt(struct kr_context *ctx, struct kr_zonecut *cut)
{
	if (!ctx || !cut) {
		return kr_error(EINVAL);
	}

	update_cut_name(cut, U8(""));

	/* Copy root hints from resolution context. */
	if (ctx->root_hints.nsset.root) {
		int ret = kr_zonecut_copy(cut, &ctx->root_hints);
		if (ret == 0) {
			return ret;
		}
	}

	/* Copy compiled-in root hints */
	for (unsigned i = 0; i < HINT_COUNT; ++i) {
		const struct hint_info *hint = &SBELT[i];
		knot_rdata_t rdata[knot_rdata_array_size(HINT_ADDRLEN)];
		knot_rdata_init(rdata, HINT_ADDRLEN, hint->addr, 0);
		int ret = kr_zonecut_add(cut, hint->name, rdata);
		if (ret != 0) {
			return ret;
		}
	}

	return kr_ok();
}

/** Fetch address for zone cut. */
static void fetch_addr(struct kr_zonecut *cut, const knot_dname_t *ns, uint16_t rrtype, struct kr_cache_txn *txn, uint32_t timestamp)
{
	knot_rrset_t cached_rr;
	knot_rrset_init(&cached_rr, (knot_dname_t *)ns, rrtype, KNOT_CLASS_IN);
	if (kr_cache_peek_rr(txn, &cached_rr, &timestamp) != 0) {
		return;
	}

	knot_rdata_t *rd = cached_rr.rrs.data;
	for (uint16_t i = 0; i < cached_rr.rrs.rr_count; ++i) {
		if (knot_rdata_ttl(rd) > timestamp) {
			(void) kr_zonecut_add(cut, ns, rd);
		}
		rd = kr_rdataset_next(rd);
	}
}

/** Fetch best NS for zone cut. */
static int fetch_ns(struct kr_context *ctx, struct kr_zonecut *cut, const knot_dname_t *name, struct kr_cache_txn *txn, uint32_t timestamp)
{
	uint32_t drift = timestamp;
	knot_rrset_t cached_rr;
	knot_rrset_init(&cached_rr, (knot_dname_t *)name, KNOT_RRTYPE_NS, KNOT_CLASS_IN);
	int ret = kr_cache_peek_rr(txn, &cached_rr, &drift);
	if (ret != 0) {
		return ret;
	}

	/* Insert name servers for this zone cut, addresses will be looked up
	 * on-demand (either from cache or iteratively) */
	for (unsigned i = 0; i < cached_rr.rrs.rr_count; ++i) {
		const knot_dname_t *ns_name = knot_ns_name(&cached_rr.rrs, i);
		kr_zonecut_add(cut, ns_name, NULL);
		/* Fetch NS reputation and decide whether to prefetch A/AAAA records. */
		unsigned *cached = lru_get(ctx->cache_rep, (const char *)ns_name, knot_dname_size(ns_name));
		unsigned reputation = (cached) ? *cached : 0;
		if (!(reputation & KR_NS_NOIP4)) {
			fetch_addr(cut, ns_name, KNOT_RRTYPE_A, txn, timestamp);
		}
		if (!(reputation & KR_NS_NOIP6)) {
			fetch_addr(cut, ns_name, KNOT_RRTYPE_AAAA, txn, timestamp);
		}
	}

	/* Always keep SBELT as a backup for root */
	if (name[0] == '\0') {
		kr_zonecut_set_sbelt(ctx, cut);
	}

	return kr_ok();
}

int kr_zonecut_find_cached(struct kr_context *ctx, struct kr_zonecut *cut, const knot_dname_t *name,
                           struct kr_cache_txn *txn, uint32_t timestamp)
{
	if (!ctx || !cut || !name) {
		return kr_error(EINVAL);
	}

	/* Start at QNAME parent. */
	while (txn) {
		if (fetch_ns(ctx, cut, name, txn, timestamp) == 0) {
			update_cut_name(cut, name);
			return kr_ok();
		}
		if (name[0] == '\0') {
			break;
		}
		/* Subtract label from QNAME. */
		name = knot_wire_next_label(name, NULL);
	}

	/* Name server not found, start with SBELT. */
	return kr_zonecut_set_sbelt(ctx, cut);
}
