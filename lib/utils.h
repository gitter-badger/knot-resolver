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

#pragma once

#include <stdio.h>

/*
 * General-purpose attributes.
 * @cond internal
 */
#define auto_free __attribute__((cleanup(_cleanup_free)))
extern void _cleanup_free(char **p);
#define auto_close __attribute__((cleanup(_cleanup_close)))
extern void _cleanup_close(int *p);
#define auto_fclose __attribute__((cleanup(_cleanup_fclose)))
extern void _cleanup_fclose(FILE **p);
/* @endcond */

/*
 * Defines.
 */

/** @internal Fast packet reset. */
#define KR_PKT_RECYCLE(pkt) do { \
	(pkt)->rrset_count = 0; \
	(pkt)->size = KNOT_WIRE_HEADER_SIZE; \
	(pkt)->current = KNOT_ANSWER; \
	memset((pkt)->sections, 0, sizeof((pkt)->sections)); \
	knot_pkt_begin((pkt), KNOT_ANSWER); \
	knot_pkt_parse_question((pkt)); \
} while (0)

/** @internal Next RDATA shortcut. */
#define kr_rdataset_next(rd) (rd + knot_rdata_array_size(knot_rdata_rdlen(rd)))

/** Concatenate N strings. */
char* kr_strcatdup(unsigned n, ...);

/** Reseed CSPRNG context. */
int kr_rand_reseed(void);

/** Get pseudo-random value. */
unsigned kr_rand_uint(unsigned max);

/** Memory reservation routine for mm_ctx_t */
int mm_reserve(void *baton, char **mem, size_t elm_size, size_t want, size_t *have);
