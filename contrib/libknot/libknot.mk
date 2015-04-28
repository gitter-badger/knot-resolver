# API modules
libknot_EMBED := \
	errcode.c            \
	consts.c             \
	descriptor.c         \
	dname.c              \
	rdata.c              \
	rdataset.c           \
	rrset.c              \
	rrtype/opt.c         \
	packet/pkt.c         \
	packet/compr.c       \
	packet/rrset-wire.c  \
	processing/layer.c   \
	processing/overlay.c

# Internal modules
libknot_EMBED += \
	internal/net.c                \
	internal/utils.c              \
	internal/errcode.c            \
	internal/lists.c              \
	internal/strlcpy.c            \
	internal/tolower.c            \
	internal/sockaddr.c           \
	internal/mempattern.c         \
	internal/mempool.c            \
	internal/namedb/mdb.c         \
	internal/namedb/midl.c        \
	internal/namedb/namedb_lmdb.c

# Define embedded target
libknot_EMBED := $(addprefix contrib/libknot/,$(libknot_EMBED))
HAS_libknot  := embed
