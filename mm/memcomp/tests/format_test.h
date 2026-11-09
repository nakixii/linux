/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _MM_MEMCOMP_TESTS_FORMAT_TEST_H
#define _MM_MEMCOMP_TESTS_FORMAT_TEST_H

#include <kunit/test.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "../format.h"

/* Registration is supplied by the enclosing memcomp KUnit suite. */
static void memcomp_test_format_layout(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sizeof(struct mc16_slot), (size_t)16);
	KUNIT_EXPECT_EQ(test, sizeof(struct mc16_member), (size_t)24);
	KUNIT_EXPECT_EQ(test, sizeof(struct mc16_record), (size_t)64);
	KUNIT_EXPECT_EQ(test, __alignof__(struct mc16_slot), (size_t)8);
	KUNIT_EXPECT_EQ(test, __alignof__(struct mc16_member), (size_t)8);
	KUNIT_EXPECT_EQ(test, __alignof__(struct mc16_record), (size_t)8);
	KUNIT_EXPECT_EQ(test, offsetof(struct mc16_slot, refs), (size_t)8);
	KUNIT_EXPECT_EQ(test, offsetof(struct mc16_slot, length), (size_t)12);
	KUNIT_EXPECT_EQ(test, offsetof(struct mc16_slot, flags), (size_t)14);
	KUNIT_EXPECT_EQ(test, offsetof(struct mc16_member, owner_folio), (size_t)8);
	KUNIT_EXPECT_EQ(test, offsetof(struct mc16_member, length), (size_t)16);
	KUNIT_EXPECT_EQ(test, offsetof(struct mc16_member, flags), (size_t)18);
	KUNIT_EXPECT_EQ(test, offsetof(struct mc16_member, payload_charge), (size_t)20);
	KUNIT_EXPECT_EQ(test, offsetof(struct mc16_record, member), (size_t)64);
	KUNIT_EXPECT_EQ(test, (u64)MC16_STATE_MASK, 0x7ULL);
	KUNIT_EXPECT_EQ(test, (u64)MC16_ORDER_MASK, 0x1c0ULL);
	KUNIT_EXPECT_EQ(test, MC16_ORDER_SHIFT, 6);
	KUNIT_EXPECT_EQ(test, (u64)MC16_RESERVED_MASK, 0xf800ULL);
}

static void memcomp_test_format_states_orders(struct kunit *test)
{
	static const u64 modifiers[] = {
		0,
		MC16_F_OWNER,
		MC16_F_RESIDENT,
		MC16_F_REFRESH_PENDING,
		MC16_F_REFERENCED,
		MC16_F_INCOMPRESSIBLE,
		MC16_MODIFIER_MASK,
	};
	static const enum mc16_state states[] = {
		MC16_EMPTY, MC16_PENDING, MC16_STORED, MC16_WRITEBACK, MC16_SWAPPED, MC16_SAMEFILL,
	};
	unsigned int state, order, i;
	u16 flags;

	for (state = 0; state < ARRAY_SIZE(states); state++) {
		KUNIT_EXPECT_EQ(test, (unsigned int)states[state], state);
		for (order = 0; order <= MC16_MAX_ORDER; order++) {
			KUNIT_ASSERT_TRUE(test, mc16_flags_encode(states[state], order, 0, &flags));
			KUNIT_EXPECT_TRUE(test, mc16_flags_valid(flags));
			KUNIT_EXPECT_EQ(test, (u64)flags, (u64)state | ((u64)order << 6));
			KUNIT_EXPECT_EQ(test, mc16_flags_state(flags), state);
			KUNIT_EXPECT_EQ(test, mc16_flags_order(flags), order);
		}
	}
	/* Attached effective payloads can retain OWNER in their member flags. */
	for (i = 0; i < ARRAY_SIZE(modifiers); i++) {
		KUNIT_ASSERT_TRUE(test, mc16_flags_encode(MC16_PENDING, 4, modifiers[i], &flags));
		KUNIT_EXPECT_TRUE(test, mc16_flags_valid(flags));
		KUNIT_EXPECT_EQ(test, (u64)(flags & MC16_MODIFIER_MASK), modifiers[i]);
		KUNIT_EXPECT_EQ(test, mc16_flags_state(flags), (unsigned int)MC16_PENDING);
		KUNIT_EXPECT_EQ(test, mc16_flags_order(flags), 4U);
	}
}

static void memcomp_test_format_flags_reject(struct kunit *test)
{
	static const struct {
		u64 state;
		u64 order;
		u64 modifiers;
	} bad[] = {
		{ 6, 0, 0 },
		{ 7, 0, 0 },
		{ BIT_ULL(32), 0, 0 },
		{ U64_MAX, 0, 0 },
		{ MC16_PENDING, 5, 0 },
		{ MC16_PENDING, 7, 0 },
		{ MC16_PENDING, BIT_ULL(32), 0 },
		{ MC16_PENDING, U64_MAX, 0 },
		{ MC16_PENDING, 0, MC16_STATE_MASK },
		{ MC16_PENDING, 0, MC16_ORDER_MASK },
		{ MC16_PENDING, 0, BIT_ULL(11) },
		{ MC16_PENDING, 0, BIT_ULL(15) },
		{ MC16_PENDING, 0, BIT_ULL(16) },
		{ MC16_PENDING, 0, BIT_ULL(63) },
	};
	unsigned int i;
	u16 flags;

	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		flags = 0x5a5a;
		KUNIT_EXPECT_FALSE(test, mc16_flags_encode(bad[i].state, bad[i].order,
							   bad[i].modifiers, &flags));
		KUNIT_EXPECT_EQ(test, flags, (u16)0x5a5a);
	}
	for (i = 11; i < 64; i++)
		KUNIT_EXPECT_FALSE(test, mc16_flags_valid(BIT_ULL(i)));
	for (i = 5; i <= 7; i++)
		KUNIT_EXPECT_FALSE(test, mc16_flags_valid((u64)i << MC16_ORDER_SHIFT));
	KUNIT_EXPECT_FALSE(test, mc16_flags_valid(6));
	KUNIT_EXPECT_FALSE(test, mc16_flags_valid(7));
	KUNIT_EXPECT_FALSE(test, mc16_flags_valid(U64_MAX));
}

static void memcomp_test_format_length_bounds(struct kunit *test)
{
	static const u64 good[] = { 1, 2, PAGE_SIZE - 1, PAGE_SIZE };
	static const u64 bad_bytes[] = {
		0, PAGE_SIZE + 1, BIT_ULL(16) + 1, BIT_ULL(32), U64_MAX,
	};
	static const u64 bad_encoded[] = {
		PAGE_SIZE,
		BIT_ULL(16),
		BIT_ULL(32),
		U64_MAX,
	};
	unsigned int i;
	u16 encoded;
	u32 bytes;

	for (i = 0; i < ARRAY_SIZE(good); i++) {
		KUNIT_ASSERT_TRUE(test, mc16_length_encode(good[i], &encoded));
		KUNIT_EXPECT_EQ(test, (u64)encoded, good[i] - 1);
		KUNIT_ASSERT_TRUE(test, mc16_length_decode(encoded, &bytes));
		KUNIT_EXPECT_EQ(test, (u64)bytes, good[i]);
	}
	for (i = 0; i < ARRAY_SIZE(bad_bytes); i++) {
		encoded = 0x5a5a;
		KUNIT_EXPECT_FALSE(test, mc16_length_encode(bad_bytes[i], &encoded));
		KUNIT_EXPECT_EQ(test, encoded, (u16)0x5a5a);
	}
	for (i = 0; i < ARRAY_SIZE(bad_encoded); i++) {
		bytes = 0x12345678;
		KUNIT_EXPECT_FALSE(test, mc16_length_decode(bad_encoded[i], &bytes));
		KUNIT_EXPECT_EQ(test, bytes, 0x12345678U);
	}
	KUNIT_ASSERT_TRUE(test, mc16_length_decode(0, &bytes));
	KUNIT_EXPECT_EQ(test, bytes, 1U);
}

static void memcomp_test_format_payload_states(struct kunit *test)
{
	static const u64 states[] = {
		MC16_EMPTY,   MC16_PENDING,  MC16_STORED, MC16_WRITEBACK,
		MC16_SWAPPED, MC16_SAMEFILL, 6,		  7,
		BIT_ULL(32),  U64_MAX,
	};
	unsigned int i;
	u16 encoded;
	u32 bytes;
	bool payload;

	for (i = 0; i < ARRAY_SIZE(states); i++) {
		payload = states[i] == MC16_STORED || states[i] == MC16_WRITEBACK ||
			  states[i] == MC16_SWAPPED;
		encoded = 0x5a5a;
		bytes = 0x12345678;
		if (payload) {
			KUNIT_ASSERT_TRUE(test, mc16_payload_length_encode(states[i], PAGE_SIZE,
									   &encoded));
			KUNIT_EXPECT_EQ(test, (u64)encoded, (u64)PAGE_SIZE - 1);
			KUNIT_ASSERT_TRUE(test,
					  mc16_payload_length_decode(states[i], encoded, &bytes));
			KUNIT_EXPECT_EQ(test, (u64)bytes, (u64)PAGE_SIZE);
		} else if (states[i] == MC16_SAMEFILL) {
			KUNIT_ASSERT_TRUE(test, mc16_payload_length_encode(states[i], 0, &encoded));
			KUNIT_EXPECT_EQ(test, encoded, (u16)0);
			KUNIT_ASSERT_TRUE(test,
					  mc16_payload_length_decode(states[i], encoded, &bytes));
			KUNIT_EXPECT_EQ(test, bytes, 0U);
		} else {
			KUNIT_EXPECT_FALSE(test,
					   mc16_payload_length_encode(states[i], 1, &encoded));
			KUNIT_EXPECT_EQ(test, encoded, (u16)0x5a5a);
			KUNIT_EXPECT_FALSE(test, mc16_payload_length_decode(states[i], 0, &bytes));
			KUNIT_EXPECT_EQ(test, bytes, 0x12345678U);
		}
	}
	encoded = 0x5a5a;
	bytes = 0x12345678;
	KUNIT_EXPECT_FALSE(test, mc16_payload_length_encode(MC16_SAMEFILL, 1, &encoded));
	KUNIT_EXPECT_EQ(test, encoded, (u16)0x5a5a);
	KUNIT_EXPECT_FALSE(test, mc16_payload_length_decode(MC16_SAMEFILL, 1, &bytes));
	KUNIT_EXPECT_EQ(test, bytes, 0x12345678U);
	KUNIT_EXPECT_FALSE(test, mc16_payload_length_encode(MC16_STORED, 0, &encoded));
	KUNIT_EXPECT_EQ(test, encoded, (u16)0x5a5a);
}

static void memcomp_test_format_full_words(struct kunit *test)
{
	static const u64 words[] = {
		U64_MAX,
		BIT_ULL(63),
		0xfedcba9876543210ULL,
		0x0101010101010101ULL,
		0x8000000000000001ULL,
	};
	static const enum mc16_state states[] = {
		MC16_SAMEFILL,
		MC16_STORED,
		MC16_WRITEBACK,
	};
	struct mc16_slot slot = {};
	unsigned int i, j;
	u32 bytes;

	for (i = 0; i < ARRAY_SIZE(states); i++) {
		for (j = 0; j < ARRAY_SIZE(words); j++) {
			slot.word = words[j];
			KUNIT_ASSERT_TRUE(test, mc16_flags_encode(states[i], 0, 0, &slot.flags));
			bytes = states[i] == MC16_SAMEFILL ? 0 : PAGE_SIZE;
			KUNIT_ASSERT_TRUE(test, mc16_payload_length_encode(states[i], bytes,
									   &slot.length));
			KUNIT_ASSERT_TRUE(test, mc16_payload_length_decode(states[i], slot.length,
									   &bytes));
			KUNIT_EXPECT_EQ(test, slot.word, words[j]);
			KUNIT_EXPECT_EQ(test, bytes,
					states[i] == MC16_SAMEFILL ? 0U : (u32)PAGE_SIZE);
			if (states[i] == MC16_SAMEFILL)
				KUNIT_EXPECT_EQ(test, slot.length, (u16)0);
		}
	}
}

static void memcomp_test_format_swap_raw(struct kunit *test)
{
	const u64 offsets[] = { 1, mc16_swap_offset_limit() };
	static const u64 lengths[] = { 1, PAGE_SIZE - 1, PAGE_SIZE };
	const u64 types[] = { 0, MAX_SWAPFILES - 1 };
	struct mc16_swap_fields fields;
	unsigned int i, j, k;
	u16 length;
	u64 word;

	KUNIT_EXPECT_EQ(test, mc16_swap_offset_limit(),
			min_t(u64, SWP_OFFSET_MASK, U64_MAX >> (PAGE_SHIFT + 6)));
	for (i = 0; i < ARRAY_SIZE(types); i++) {
		for (j = 0; j < ARRAY_SIZE(offsets); j++) {
			for (k = 0; k < ARRAY_SIZE(lengths); k++) {
				KUNIT_ASSERT_TRUE(test, mc16_swap_encode(types[i], offsets[j], 0,
									 lengths[k], false, &word,
									 &length));
				KUNIT_EXPECT_EQ(test, word,
						types[i] | (offsets[j] << (PAGE_SHIFT + 6)));
				KUNIT_ASSERT_TRUE(test, mc16_swap_decode(word, length, &fields));
				KUNIT_EXPECT_EQ(test, fields.swap_offset, offsets[j]);
				KUNIT_EXPECT_EQ(test, (u64)fields.type, types[i]);
				KUNIT_EXPECT_EQ(test, fields.byte_offset, 0U);
				KUNIT_EXPECT_EQ(test, (u64)fields.length, lengths[k]);
				KUNIT_EXPECT_FALSE(test, fields.packed);
			}
		}
	}
}

static void memcomp_test_format_swap_packed(struct kunit *test)
{
	static const struct {
		u64 byte_offset;
		u64 bytes;
	} edges[] = {
		{ 0, 1 },
		{ 0, PAGE_SIZE - 1 },
		{ 1, PAGE_SIZE - 1 },
		{ PAGE_SIZE - 1, 1 },
	};
	struct mc16_swap_fields fields;
	u64 offset = mc16_swap_offset_limit();
	u64 type = MAX_SWAPFILES - 1;
	unsigned int i;
	u16 length;
	u64 word;

	for (i = 0; i < ARRAY_SIZE(edges); i++) {
		KUNIT_ASSERT_TRUE(test, mc16_swap_encode(type, offset, edges[i].byte_offset,
							 edges[i].bytes, true, &word, &length));
		KUNIT_EXPECT_EQ(test, word,
				type | (edges[i].byte_offset << 5) | BIT_ULL(PAGE_SHIFT + 5) |
					(offset << (PAGE_SHIFT + 6)));
		KUNIT_ASSERT_TRUE(test, mc16_swap_decode(word, length, &fields));
		KUNIT_EXPECT_EQ(test, fields.swap_offset, offset);
		KUNIT_EXPECT_EQ(test, (u64)fields.type, type);
		KUNIT_EXPECT_EQ(test, (u64)fields.byte_offset, edges[i].byte_offset);
		KUNIT_EXPECT_EQ(test, (u64)fields.length, edges[i].bytes);
		KUNIT_EXPECT_TRUE(test, fields.packed);
	}
}

static void mc16_format_expect_swap_reject(struct kunit *test, u64 type, u64 offset,
					   u64 byte_offset, u64 bytes, bool packed)
{
	u64 word = 0x123456789abcdef0ULL;
	u16 length = 0x5a5a;

	KUNIT_EXPECT_FALSE(test, mc16_swap_encode(type, offset, byte_offset, bytes, packed, &word,
						  &length));
	KUNIT_EXPECT_EQ(test, word, 0x123456789abcdef0ULL);
	KUNIT_EXPECT_EQ(test, length, (u16)0x5a5a);
}

static void memcomp_test_format_swap_encode_reject(struct kunit *test)
{
	static const u64 bad_types[] = {
		MAX_SWAPFILES,
		32,
		BIT_ULL(32),
		U64_MAX,
	};
	static const u64 bad_bytes[] = {
		0, PAGE_SIZE + 1, BIT_ULL(16) + 1, BIT_ULL(32), U64_MAX,
	};
	static const u64 bad_byte_offsets[] = {
		PAGE_SIZE,
		BIT_ULL(16),
		BIT_ULL(32),
		U64_MAX,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(bad_types); i++)
		mc16_format_expect_swap_reject(test, bad_types[i], 1, 0, 1, false);
	for (i = 0; i < ARRAY_SIZE(bad_bytes); i++)
		mc16_format_expect_swap_reject(test, 0, 1, 0, bad_bytes[i], false);
	for (i = 0; i < ARRAY_SIZE(bad_byte_offsets); i++)
		mc16_format_expect_swap_reject(test, 0, 1, bad_byte_offsets[i], 1, true);
	mc16_format_expect_swap_reject(test, 0, mc16_swap_offset_limit() + 1, 0, 1, false);
	mc16_format_expect_swap_reject(test, 0, U64_MAX, 0, 1, false);
	mc16_format_expect_swap_reject(test, 0, 1, 1, 1, false);
	mc16_format_expect_swap_reject(test, 0, 1, 0, PAGE_SIZE, true);
	mc16_format_expect_swap_reject(test, 0, 1, PAGE_SIZE - 1, 2, true);
}

static void mc16_format_expect_decode_reject(struct kunit *test, u64 word, u64 length)
{
	struct mc16_swap_fields fields;
	u8 before[sizeof(fields)];

	memset(&fields, 0xa5, sizeof(fields));
	memcpy(before, &fields, sizeof(fields));
	KUNIT_EXPECT_FALSE(test, mc16_swap_decode(word, length, &fields));
	KUNIT_EXPECT_MEMEQ(test, &fields, before, sizeof(fields));
}

static void memcomp_test_format_swap_decode_reject(struct kunit *test)
{
	u16 length;
	u64 word;

	KUNIT_ASSERT_TRUE(test, mc16_swap_encode(0, 1, 0, 1, false, &word, &length));
	mc16_format_expect_decode_reject(test, (word & ~MC16_SWAP_TYPE_MASK) | MAX_SWAPFILES,
					 length);
	mc16_format_expect_decode_reject(test, word | BIT_ULL(MC16_SWAP_BYTE_SHIFT), length);
	mc16_format_expect_decode_reject(test, word | MC16_SWAP_PACKED, PAGE_SIZE - 1);
	word |= MC16_SWAP_PACKED;
	mc16_format_expect_decode_reject(test,
					 word | ((u64)(PAGE_SIZE - 1) << MC16_SWAP_BYTE_SHIFT), 1);
	mc16_format_expect_decode_reject(test, word, PAGE_SIZE);
	mc16_format_expect_decode_reject(test, word, BIT_ULL(32));
	mc16_format_expect_decode_reject(test, word, U64_MAX);
}

static void memcomp_test_format_pending_record(struct kunit *test)
{
	static const enum mc16_state effective[] = {
		MC16_STORED,
		MC16_WRITEBACK,
		MC16_SWAPPED,
		MC16_SAMEFILL,
	};
	struct mc16_slot slot = {};
	struct mc16_record *record;
	struct mc16_member *member;
	struct folio *owner;
	unsigned int i;
	u32 index, bytes;
	u16 flags;
	u64 word;

	record = kunit_kzalloc(test, sizeof(*record) + MC16_MAX_PAGES * sizeof(record->member[0]),
			       GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, record);
	owner = kunit_kzalloc(test, sizeof(*owner), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, owner);
	record->token_base = 32;
	record->nr_pages = MC16_MAX_PAGES;
	record->producer = owner;
	record->record_flags = MC16_RECORD_ANON_VMA;
	refcount_set(&record->members, MC16_MAX_PAGES);
	refcount_set(&slot.refs, 1);
	slot.word = (u64)(unsigned long)record;
	KUNIT_ASSERT_TRUE(test,
			  mc16_flags_encode(MC16_PENDING, MC16_MAX_ORDER,
					    MC16_F_OWNER | MC16_F_RESIDENT | MC16_F_REFRESH_PENDING,
					    &slot.flags));
	KUNIT_EXPECT_TRUE(test, mc16_flags_valid(slot.flags));
	KUNIT_EXPECT_EQ(test, mc16_flags_state(slot.flags), (unsigned int)MC16_PENDING);
	KUNIT_EXPECT_EQ(test, mc16_flags_order(slot.flags), 4U);
	KUNIT_EXPECT_PTR_EQ(test, (void *)(unsigned long)slot.word, record);
	KUNIT_EXPECT_EQ(test, slot.length, (u16)0);

	/* PENDING length is not a direct payload encoding. */
	flags = 0x5a5a;
	KUNIT_EXPECT_FALSE(test, mc16_payload_length_encode(MC16_PENDING, PAGE_SIZE, &flags));
	KUNIT_EXPECT_EQ(test, flags, (u16)0x5a5a);
	for (i = 0; i < record->nr_pages; i++) {
		enum mc16_state state = effective[i % ARRAY_SIZE(effective)];

		KUNIT_ASSERT_TRUE(test, mc16_member_index(record, record->token_base + i, &index));
		KUNIT_ASSERT_EQ(test, index, i);
		member = &record->member[index];
		member->owner_folio = owner;
		KUNIT_ASSERT_TRUE(test, mc16_flags_encode(state, MC16_MAX_ORDER, MC16_F_OWNER,
							  &member->flags));
		word = 0x8000000000000001ULL + i;
		if (state == MC16_SWAPPED) {
			KUNIT_ASSERT_TRUE(test, mc16_swap_encode(0, 1, 0, 1, true, &word,
								 &member->length));
		} else {
			bytes = state == MC16_SAMEFILL ? 0 : 1;
			KUNIT_ASSERT_TRUE(test, mc16_payload_length_encode(state, bytes,
									   &member->length));
		}
		member->word = word;
		member->payload_charge = state == MC16_SAMEFILL ? 0 : 1;
		KUNIT_EXPECT_TRUE(test, mc16_flags_valid(member->flags));
		KUNIT_EXPECT_EQ(test, mc16_flags_state(member->flags), (unsigned int)state);
		KUNIT_EXPECT_TRUE(test, member->flags & MC16_F_OWNER);
		KUNIT_EXPECT_PTR_EQ(test, member->owner_folio, owner);
		KUNIT_EXPECT_EQ(test, member->word, word);
		KUNIT_ASSERT_TRUE(test, mc16_payload_length_decode(state, member->length, &bytes));
		KUNIT_EXPECT_EQ(test, bytes, state == MC16_SAMEFILL ? 0U : 1U);
	}
	KUNIT_EXPECT_EQ(test, refcount_read(&record->members), MC16_MAX_PAGES);
	KUNIT_EXPECT_EQ(test, refcount_read(&slot.refs), 1U);
	KUNIT_EXPECT_PTR_EQ(test, record->producer, owner);
}

static void memcomp_test_format_member_index_orders(struct kunit *test)
{
	struct mc16_record record = { .token_base = 32 };
	unsigned int order, i;
	u32 index;

	for (order = 0; order <= MC16_MAX_ORDER; order++) {
		record.nr_pages = BIT(order);
		for (i = 0; i < record.nr_pages; i++) {
			KUNIT_ASSERT_TRUE(test, mc16_member_index(&record, record.token_base + i,
								  &index));
			KUNIT_EXPECT_EQ(test, index, i);
		}
		index = 0x12345678;
		KUNIT_EXPECT_FALSE(test,
				   mc16_member_index(&record, record.token_base + record.nr_pages,
						     &index));
		KUNIT_EXPECT_EQ(test, index, 0x12345678U);
	}
}

static void memcomp_test_format_member_index_reject(struct kunit *test)
{
	static const struct {
		u32 base;
		u16 nr;
	} bad[] = {
		{ 0, 1 }, { 32, 0 }, { 32, 3 }, { 32, 32 }, { 33, 16 }, { U32_MAX, 16 },
	};
	const u64 bad_tokens[] = { 0, 31, 48, BIT_ULL(32), U64_MAX };
	struct mc16_record record = { .token_base = 32, .nr_pages = 16 };
	unsigned int i;
	u32 index;

	for (i = 0; i < ARRAY_SIZE(bad_tokens); i++) {
		index = 0x12345678;
		KUNIT_EXPECT_FALSE(test, mc16_member_index(&record, bad_tokens[i], &index));
		KUNIT_EXPECT_EQ(test, index, 0x12345678U);
	}
	for (i = 0; i < ARRAY_SIZE(bad); i++) {
		record.token_base = bad[i].base;
		record.nr_pages = bad[i].nr;
		index = 0x12345678;
		KUNIT_EXPECT_FALSE(test, mc16_member_index(&record, bad[i].base, &index));
		KUNIT_EXPECT_EQ(test, index, 0x12345678U);
	}
	/* Highest aligned span ends exactly at the u32 token boundary. */
	record.token_base = U32_MAX - 15;
	record.nr_pages = 16;
	KUNIT_ASSERT_TRUE(test, mc16_member_index(&record, U32_MAX, &index));
	KUNIT_EXPECT_EQ(test, index, 15U);
}

static void memcomp_test_format_null_outputs(struct kunit *test)
{
	struct mc16_record record = { .token_base = 1, .nr_pages = 1 };
	u64 word = 0x123456789abcdef0ULL;
	u16 length = 0x5a5a;
	u32 index = 0x12345678;

	KUNIT_EXPECT_FALSE(test, mc16_flags_encode(MC16_PENDING, 0, 0, NULL));
	KUNIT_EXPECT_FALSE(test, mc16_length_encode(1, NULL));
	KUNIT_EXPECT_FALSE(test, mc16_length_decode(0, NULL));
	KUNIT_EXPECT_FALSE(test, mc16_payload_length_encode(MC16_SAMEFILL, 0, NULL));
	KUNIT_EXPECT_FALSE(test, mc16_payload_length_decode(MC16_SAMEFILL, 0, NULL));
	KUNIT_EXPECT_FALSE(test, mc16_swap_encode(0, 1, 0, 1, false, NULL, &length));
	KUNIT_EXPECT_EQ(test, length, (u16)0x5a5a);
	KUNIT_EXPECT_FALSE(test, mc16_swap_encode(0, 1, 0, 1, false, &word, NULL));
	KUNIT_EXPECT_EQ(test, word, 0x123456789abcdef0ULL);
	KUNIT_EXPECT_FALSE(test, mc16_swap_decode(word, 0, NULL));
	KUNIT_EXPECT_FALSE(test, mc16_member_index(&record, 1, NULL));
	KUNIT_EXPECT_FALSE(test, mc16_member_index(NULL, 1, &index));
	KUNIT_EXPECT_EQ(test, index, 0x12345678U);
}

#endif /* _MM_MEMCOMP_TESTS_FORMAT_TEST_H */
