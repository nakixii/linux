/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_MEMCOMP_FORMAT_H
#define _MM_MEMCOMP_FORMAT_H

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/limits.h>
#include <linux/minmax.h>
#include <linux/refcount.h>
#include <linux/stddef.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/types.h>

#include <asm/page.h>

struct anon_vma;
struct folio;
struct obj_cgroup;

enum mc16_state {
	MC16_EMPTY = 0,
	MC16_PENDING = 1,
	MC16_STORED = 2,
	MC16_WRITEBACK = 3,
	MC16_SWAPPED = 4,
	MC16_SAMEFILL = 5,
};

#define MC16_STATE_MASK		GENMASK(2, 0)
#define MC16_F_OWNER		BIT(3)
#define MC16_F_RESIDENT		BIT(4)
#define MC16_F_REFRESH_PENDING	BIT(5)
#define MC16_ORDER_SHIFT	6
#define MC16_ORDER_MASK		GENMASK(8, 6)
#define MC16_F_REFERENCED	BIT(9)
#define MC16_F_INCOMPRESSIBLE	BIT(10)
#define MC16_RESERVED_MASK	GENMASK(15, 11)
#define MC16_MAX_ORDER		4
#define MC16_MAX_PAGES		BIT(MC16_MAX_ORDER)
#define MC16_MODIFIER_MASK	(MC16_F_OWNER | MC16_F_RESIDENT | \
				 MC16_F_REFRESH_PENDING | MC16_F_REFERENCED | \
				 MC16_F_INCOMPRESSIBLE)
#define MC16_FLAGS_MASK		(MC16_STATE_MASK | MC16_ORDER_MASK | \
				 MC16_MODIFIER_MASK)

#define MC16_RECORD_ANON_VMA	BIT(0)
#define MC16_RECORD_SHMEM	BIT(1)

/* Zero tokens have no slot. word preserves every bit of a zs handle/fill. */
struct mc16_slot {
	u64 word;
	refcount_t refs;
	u16 length;
	u16 flags;
} __aligned(8);

struct mc16_member {
	u64 word;
	struct folio *owner_folio;
	u16 length;
	u16 flags;
	u32 payload_charge;
} __aligned(8);

struct mc16_record {
	refcount_t members;
	u32 token_base;
	struct folio *producer;
	struct anon_vma *anon_vma;
	struct obj_cgroup *objcg;
	u64 alloc_bytes;
	u64 charge_bytes;
	u16 nid;
	u16 nr_pages;
	u32 record_flags;
	u8 reserved[8];
	struct mc16_member member[];
} __aligned(8);

static_assert(PAGE_SHIFT >= 12 && PAGE_SHIFT <= 16);
static_assert(MAX_SWAPFILES_SHIFT == 5);
static_assert(sizeof(refcount_t) == 4);
static_assert(sizeof(void *) == 8);

static_assert(sizeof(struct mc16_slot) == 16);
static_assert(__alignof__(struct mc16_slot) == 8);
static_assert(offsetof(struct mc16_slot, word) == 0);
static_assert(offsetof(struct mc16_slot, refs) == 8);
static_assert(offsetof(struct mc16_slot, length) == 12);
static_assert(offsetof(struct mc16_slot, flags) == 14);

static_assert(sizeof(struct mc16_member) == 24);
static_assert(__alignof__(struct mc16_member) == 8);
static_assert(offsetof(struct mc16_member, word) == 0);
static_assert(offsetof(struct mc16_member, owner_folio) == 8);
static_assert(offsetof(struct mc16_member, length) == 16);
static_assert(offsetof(struct mc16_member, flags) == 18);
static_assert(offsetof(struct mc16_member, payload_charge) == 20);

/* These are C layout sizes, not allocator bucket or charge sizes. */
static_assert(sizeof(struct mc16_record) == 64);
static_assert(__alignof__(struct mc16_record) == 8);
static_assert(offsetof(struct mc16_record, members) == 0);
static_assert(offsetof(struct mc16_record, token_base) == 4);
static_assert(offsetof(struct mc16_record, producer) == 8);
static_assert(offsetof(struct mc16_record, anon_vma) == 16);
static_assert(offsetof(struct mc16_record, objcg) == 24);
static_assert(offsetof(struct mc16_record, alloc_bytes) == 32);
static_assert(offsetof(struct mc16_record, charge_bytes) == 40);
static_assert(offsetof(struct mc16_record, nid) == 48);
static_assert(offsetof(struct mc16_record, nr_pages) == 50);
static_assert(offsetof(struct mc16_record, record_flags) == 52);
static_assert(offsetof(struct mc16_record, reserved) == 56);
static_assert(sizeof_field(struct mc16_record, reserved) == 8);
static_assert(offsetof(struct mc16_record, member) == 64);

/*
 * These helpers only check representation. The caller serializes access and
 * checks state-specific ownership; a member may retain OWNER for an effective
 * payload while its physical slot is PENDING. No helper takes a lock or ref.
 * Checked encoders leave every output untouched on failure.
 */
static inline bool mc16_flags_valid(u64 flags)
{
	return !(flags & ~(u64)MC16_FLAGS_MASK) &&
	       (flags & MC16_STATE_MASK) <= MC16_SAMEFILL &&
	       ((flags & MC16_ORDER_MASK) >> MC16_ORDER_SHIFT) <= MC16_MAX_ORDER;
}

static inline bool mc16_flags_encode(u64 state, u64 order, u64 modifiers,
				     u16 *encoded)
{
	u64 flags;

	if (!encoded || state > MC16_SAMEFILL || order > MC16_MAX_ORDER ||
	    (modifiers & ~(u64)MC16_MODIFIER_MASK))
		return false;

	flags = state | (order << MC16_ORDER_SHIFT) | modifiers;
	*encoded = (u16)flags;
	return true;
}

/* Call only after validating the flags representation. */
static inline unsigned int mc16_flags_state(u16 flags)
{
	return flags & MC16_STATE_MASK;
}

static inline unsigned int mc16_flags_order(u16 flags)
{
	return (flags & MC16_ORDER_MASK) >> MC16_ORDER_SHIFT;
}

/* Only real payloads use bytes - 1; encoded zero means one payload byte. */
static inline bool mc16_length_encode(u64 bytes, u16 *encoded)
{
	if (!encoded || !bytes || bytes > PAGE_SIZE)
		return false;

	*encoded = (u16)(bytes - 1);
	return true;
}

static inline bool mc16_length_decode(u64 encoded, u32 *bytes)
{
	if (!bytes || encoded >= PAGE_SIZE)
		return false;

	*bytes = (u32)encoded + 1;
	return true;
}

/* SAMEFILL keeps its complete word, has length zero and needs no codec. */
static inline bool mc16_payload_length_encode(u64 state, u64 bytes,
					      u16 *encoded)
{
	if (!encoded)
		return false;
	if (state == MC16_SAMEFILL) {
		if (bytes)
			return false;
		*encoded = 0;
		return true;
	}
	if (state != MC16_STORED && state != MC16_WRITEBACK &&
	    state != MC16_SWAPPED)
		return false;

	return mc16_length_encode(bytes, encoded);
}

static inline bool mc16_payload_length_decode(u64 state, u64 encoded,
					      u32 *bytes)
{
	if (!bytes)
		return false;
	if (state == MC16_SAMEFILL) {
		if (encoded)
			return false;
		*bytes = 0;
		return true;
	}
	if (state != MC16_STORED && state != MC16_WRITEBACK &&
	    state != MC16_SWAPPED)
		return false;

	return mc16_length_decode(encoded, bytes);
}

#define MC16_SWAP_TYPE_MASK	GENMASK_ULL(4, 0)
#define MC16_SWAP_BYTE_SHIFT	5
#define MC16_SWAP_BYTE_MASK	((u64)PAGE_SIZE - 1)
#define MC16_SWAP_PACKED		BIT_ULL(PAGE_SHIFT + 5)
#define MC16_SWAP_OFFSET_SHIFT	(PAGE_SHIFT + 6)
#define MC16_SWAP_OFFSET_MASK	(U64_MAX >> MC16_SWAP_OFFSET_SHIFT)

struct mc16_swap_fields {
	u64 swap_offset;
	u32 byte_offset;
	u32 length;
	u8 type;
	bool packed;
};

static inline u64 mc16_swap_offset_limit(void)
{
	return min_t(u64, SWP_OFFSET_MASK, MC16_SWAP_OFFSET_MASK);
}

/* Backing device/slot ownership validation remains the caller's job. */
static inline bool mc16_swap_encode(u64 type, u64 swap_offset,
				    u64 byte_offset, u64 bytes, bool packed,
				    u64 *word, u16 *length)
{
	u16 encoded;
	u64 value;

	if (!word || !length || type > MC16_SWAP_TYPE_MASK ||
	    type >= MAX_SWAPFILES || swap_offset > mc16_swap_offset_limit() ||
	    byte_offset >= PAGE_SIZE || !mc16_length_encode(bytes, &encoded))
		return false;
	if (packed) {
		if (bytes >= PAGE_SIZE || bytes > PAGE_SIZE - byte_offset)
			return false;
	} else if (byte_offset) {
		return false;
	}

	value = type | (byte_offset << MC16_SWAP_BYTE_SHIFT) |
		(swap_offset << MC16_SWAP_OFFSET_SHIFT);
	if (packed)
		value |= MC16_SWAP_PACKED;
	*word = value;
	*length = encoded;
	return true;
}

static inline bool mc16_swap_decode(u64 word, u64 length,
				    struct mc16_swap_fields *fields)
{
	struct mc16_swap_fields decoded;
	u16 checked_length;
	u64 checked_word;

	if (!fields || !mc16_length_decode(length, &decoded.length))
		return false;

	decoded.type = word & MC16_SWAP_TYPE_MASK;
	decoded.swap_offset = word >> MC16_SWAP_OFFSET_SHIFT;
	decoded.byte_offset = (word >> MC16_SWAP_BYTE_SHIFT) &
			      MC16_SWAP_BYTE_MASK;
	decoded.packed = !!(word & MC16_SWAP_PACKED);
	if (!mc16_swap_encode(decoded.type, decoded.swap_offset,
			      decoded.byte_offset, decoded.length, decoded.packed,
			      &checked_word, &checked_length))
		return false;

	*fields = decoded;
	return true;
}

/* Caller owns the record; this only validates immutable span geometry. */
static inline bool mc16_member_index(const struct mc16_record *record,
				     u64 token, u32 *index)
{
	u32 base, nr;
	u64 delta;

	if (!record || !index)
		return false;

	base = record->token_base;
	nr = record->nr_pages;
	if (!base || !nr || nr > MC16_MAX_PAGES || (nr & (nr - 1)) ||
	    (base & (nr - 1)) || base > U32_MAX - (nr - 1) || token < base)
		return false;

	delta = token - base;
	if (delta >= nr)
		return false;

	*index = (u32)delta;
	return true;
}

#endif /* _MM_MEMCOMP_FORMAT_H */
