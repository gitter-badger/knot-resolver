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

/**
 * @file stats.c
 * @brief Storage for various counters and metrics from query resolution. 
 *
 * You can either reuse this module to compute statistics or store custom metrics
 * in it via the extensions.
 */

#include <libknot/packet/pkt.h>
#include <ccan/json/json.h>

#include "lib/layer/iterate.h"
#include "lib/rplan.h"
#include "lib/module.h"
#include "lib/layer.h"

/** @internal Compatibility wrapper for Lua < 5.2 */
#if LUA_VERSION_NUM < 502
#define lua_rawlen(L, obj) lua_objlen((L), (obj))
#endif

/* Defaults */
#define DEBUG_MSG(qry, fmt...) QRDEBUG(qry, "stat",  fmt)
#define FREQUENT_COUNT 1000  /* Size of frequent tables */
#define FREQUENT_PSAMPLE 100 /* Sampling rate, 1 in N */

/** @cond internal Fixed-size map of predefined metrics. */
#define CONST_METRICS(X) \
	X(answer,total) X(answer,noerror) X(answer,nxdomain) X(answer,servfail) \
	X(answer,cached) X(answer,10ms) X(answer,100ms) X(answer,1000ms) X(answer,slow) \
	X(query,edns) X(query,dnssec) \
	X(const,end)

enum const_metric {
	#define X(a,b) metric_ ## a ## _ ## b,
	CONST_METRICS(X)
	#undef X
};
struct const_metric_elm {
	const char *key;
	size_t val;
};
static struct const_metric_elm const_metrics[] = {
	#define X(a,b) [metric_ ## a ## _ ## b] = { #a "." #b, 0 },
	CONST_METRICS(X)
	#undef X
};
/** @endcond */

/** @internal LRU hash of most frequent names. */
typedef lru_hash(unsigned) namehash_t;

/** @internal Stats data structure. */
struct stat_data {
	map_t map;
	struct {
		namehash_t *names;
	} frequent;
};

/** @internal Subtract time (best effort) */
float time_diff(struct timeval *begin, struct timeval *end)
{
	return (end->tv_sec - begin->tv_sec) * 1000 +
	       (end->tv_usec - begin->tv_usec) / 1000.0;

}

/** @internal Add to const map counter */
static inline void stat_const_add(struct stat_data *data, enum const_metric key, ssize_t incr)
{
	const_metrics[key].val += incr;
}

static int begin(knot_layer_t *ctx, void *module_param)
{
	ctx->data = module_param;
	return ctx->state;
}

static int collect_answer(struct stat_data *data, knot_pkt_t *pkt)
{
	stat_const_add(data, metric_answer_total, 1);
	/* Count per-rcode */
	switch(knot_wire_get_rcode(pkt->wire)) {
	case KNOT_RCODE_NOERROR:  stat_const_add(data, metric_answer_noerror, 1); break;
	case KNOT_RCODE_NXDOMAIN: stat_const_add(data, metric_answer_nxdomain, 1); break;
	case KNOT_RCODE_SERVFAIL: stat_const_add(data, metric_answer_servfail, 1); break;
	default: break;
	}

	return kr_ok();
}

static inline int collect_key(char *key, const knot_dname_t *name, uint16_t type)
{
	memcpy(key, &type, sizeof(type));
	int key_len = knot_dname_to_wire((uint8_t *)key + sizeof(type), name, KNOT_DNAME_MAXLEN);
	if (key_len > 0) {
		return key_len + sizeof(type);
	}
	return key_len;
}

static void collect_sample(struct stat_data *data, struct kr_rplan *rplan, knot_pkt_t *pkt)
{
	/* Sample key = {[2] type, [1-255] owner} */
	char key[sizeof(uint16_t) + KNOT_DNAME_MAXLEN];
	/* Sample queries leading to iteration or expiring */
	struct kr_query *qry = NULL;
	WALK_LIST(qry, rplan->resolved) {
		if (!(qry->flags & QUERY_CACHED) || (qry->flags & QUERY_EXPIRING)) {
			int key_len = collect_key(key, qry->sname, qry->stype);
			unsigned *count = lru_set(data->frequent.names, key, key_len);
			if (count) {
				*count += 1;
			}
		}
	}
}

static int collect(knot_layer_t *ctx)
{
	struct kr_request *param = ctx->data;
	struct kr_module *module = ctx->api->data;
	struct kr_rplan *rplan = &param->rplan;
	struct stat_data *data = module->data;

	/* Collect data on final answer */
	collect_answer(data, param->answer);
	/* Probabilistic sampling of queries */
	if (kr_rand_uint(FREQUENT_PSAMPLE) <= 1) {
		collect_sample(data, rplan, param->answer);
	}
	/* Count cached and unresolved */
	if (!EMPTY_LIST(rplan->resolved)) {
		struct kr_query *last = TAIL(rplan->resolved);
		if (last->flags & QUERY_CACHED) {
			stat_const_add(data, metric_answer_cached, 1);
		}
		/* Histogram of answer latency. */
		struct kr_query *first = HEAD(rplan->resolved);
		struct timeval now;
		gettimeofday(&now, NULL);
		float elapsed = time_diff(&first->timestamp, &now);
		if (elapsed < 10.0) {
			stat_const_add(data, metric_answer_10ms, 1);
		} else if (elapsed < 100.0) {
			stat_const_add(data, metric_answer_100ms, 1);
		} else if (elapsed < 1000.0) {
			stat_const_add(data, metric_answer_1000ms, 1);
		} else {
			stat_const_add(data, metric_answer_slow, 1);
		}
	}
	/* Query parameters and transport mode */
	if (knot_pkt_has_edns(param->answer)) {
		stat_const_add(data, metric_query_edns, 1);
		if (knot_pkt_has_dnssec(param->answer)) {
			stat_const_add(data, metric_query_dnssec, 1);
		}
	}
	return ctx->state;
}

/**
 * Set nominal value of a key.
 *
 * Input:  { key, val }
 *
 */
static char* stats_set(void *env, struct kr_module *module, const char *args)
{
	struct stat_data *data = module->data;
	auto_free char *pair = strdup(args);
	char *val = strchr(pair, ' ');
	if (val) {
		*val = '\0';
		size_t number = strtoul(val + 1, NULL, 10);
		for (unsigned i = 0; i < metric_const_end; ++i) {
			if (strcmp(const_metrics[i].key, pair) == 0) {
				const_metrics[i].val = number;
				return NULL;
			}
		}
		map_set(&data->map, pair, (void *)number);
	}

	return NULL;
}

/**
 * Retrieve metrics by key.
 *
 * Input:  string key
 * Output: number value
 */
static char* stats_get(void *env, struct kr_module *module, const char *args)
{
	struct stat_data *data = module->data;

	/* Expecting CHAR_BIT to be 8, this is a safe bet */
	char *ret = malloc(3 * sizeof(size_t) + 2);
	if (!ret) {
		return NULL;
	}

	/* Check if it exists in const map. */
	for (unsigned i = 0; i < metric_const_end; ++i) {
		if (strcmp(const_metrics[i].key, args) == 0) {
			sprintf(ret, "%zu", const_metrics[i].val);
			return ret;
		}
	}
	/* Check in variable map */
	if (!map_contains(&data->map, args)) {
		free(ret);
		return NULL;
	}
	void *val = map_get(&data->map, args);
	sprintf(ret, "%zu", (size_t) val);
	return ret;
}

static int list_entry(const char *key, void *val, void *baton)
{
	JsonNode *root = baton;
	size_t number = (size_t) val;
	json_append_member(root, key, json_mknumber(number));
	return 0;
}

/**
 * List observed metrics.
 *
 * Output: { key: val, ... }
 */
static char* stats_list(void *env, struct kr_module *module, const char *args)
{
	struct stat_data *data = module->data;
	JsonNode *root = json_mkobject();
	/* Walk const metrics map */
	size_t args_len = args ? strlen(args) : 0;
	for (unsigned i = 0; i < metric_const_end; ++i) {
		struct const_metric_elm *elm = &const_metrics[i];
		if (strncmp(elm->key, args, args_len) == 0) {
			json_append_member(root, elm->key, json_mknumber(elm->val));
		}
	}
	map_walk_prefixed(&data->map, (args_len > 0) ? args : "", list_entry, root);
	char *ret = json_encode(root);
	json_delete(root);
	return ret;
}

/**
 * List frequent names.
 *
 * Output: [{ count: <counter>, name: <qname>, type: <qtype>}, ... ]
 */
static char* freq_list(void *env, struct kr_module *module, const char *args)
{
	struct stat_data *data = module->data;
	namehash_t *freq_table = data->frequent.names;
	if (!freq_table) {
		return NULL;
	}
	uint16_t key_type = 0;
	char key_name[KNOT_DNAME_MAXLEN];
	JsonNode *root = json_mkarray();
	for (unsigned i = 0; i < freq_table->size; ++i) {
		struct lru_slot *slot = lru_slot_at((struct lru_hash_base *)freq_table, i);
		if (slot->key) {
			/* Extract query name, type and counter */
			memcpy(&key_type, slot->key, sizeof(key_type));
			knot_dname_to_str(key_name, (uint8_t *)slot->key + sizeof(key_type), sizeof(key_name));
			unsigned *slot_val = lru_slot_val(slot, lru_slot_offset(freq_table));
			/* Convert to JSON object */
			JsonNode *json_val = json_mkobject();
			json_append_member(json_val, "count", json_mknumber(*slot_val));
			json_append_member(json_val, "name",  json_mkstring(key_name));
			json_append_member(json_val, "type",  json_mknumber(key_type));
			json_append_element(root, json_val);
		}
	}
	char *ret = json_encode(root);
	json_delete(root);
	return ret;
}

static char* freq_turnover(void *env, struct kr_module *module, const char *args)
{
	struct stat_data *data = module->data;
	namehash_t *freq_table = data->frequent.names;
	if (!freq_table) {
		return NULL;
	}
	JsonNode *root = json_mknumber(freq_table->evictions);
	char *ret = json_encode(root);
	json_delete(root);
	return ret;
}

static char* freq_clear(void *env, struct kr_module *module, const char *args)
{
	struct stat_data *data = module->data;
	namehash_t *freq_table = data->frequent.names;
	if (!freq_table) {
		return NULL;
	}
	lru_deinit(freq_table);
	lru_init(freq_table, FREQUENT_COUNT);
	return NULL;
}

/*
 * Module implementation.
 */

const knot_layer_api_t *stats_layer(struct kr_module *module)
{
	static knot_layer_api_t _layer = {
		.begin = &begin,
		.finish = &collect,
	};
	/* Store module reference */
	_layer.data = module;
	return &_layer;
}

int stats_init(struct kr_module *module)
{
	struct stat_data *data = malloc(sizeof(*data));
	if (!data) {
		return kr_error(ENOMEM);
	}
	data->map = map_make();
	module->data = data;
	data->frequent.names = malloc(lru_size(namehash_t, FREQUENT_COUNT));
	if (data->frequent.names) {
		lru_init(data->frequent.names, FREQUENT_COUNT);
	}
	return kr_ok();
}

int stats_deinit(struct kr_module *module)
{
	struct stat_data *data = module->data;
	if (data) {
		map_clear(&data->map);
		lru_deinit(data->frequent.names);
		free(data->frequent.names);
		free(data);
	}
	return kr_ok();
}

struct kr_prop *stats_props(void)
{
	static struct kr_prop prop_list[] = {
	    { &stats_set,     "set", "Set {key, val} metrics.", },
	    { &stats_get,     "get", "Get metrics for given key.", },
	    { &stats_list,    "list", "List observed metrics.", },
	    { &freq_list,     "queries", "List most frequent queries.", },
	    { &freq_clear,    "queries_clear", "Clear most frequent queries.", },
	    { &freq_turnover, "queries_turnover", "Turnover of the frequent queries.", },
	    { NULL, NULL, NULL }
	};
	return prop_list;
}

KR_MODULE_EXPORT(stats);
