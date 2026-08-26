#include <stdint.h>
#include <vector>

#include <frg/rcu_radixtree.hpp>
#include <frg/std_compat.hpp>

#include <gtest/gtest.h>

struct fake_rcu_policy {
	template<typename T, typename D>
	struct obj_base {
		void retire(D d = D()) {
			auto derived = static_cast<T *>(this);
			d(derived);
		}
	};
};

using tree_type = frg::rcu_radixtree<int, frg::stl_allocator, fake_rcu_policy>;

TEST(rcu_radixtree, insert_and_find) {
	tree_type tree;

	// Keys that share the same top 60 bits (same entry_node).
	tree.insert(1, 42);
	tree.insert(2, 100);
	tree.insert(15, 7);

	auto *p0 = tree.find(1);
	auto *p1 = tree.find(2);
	auto *p2 = tree.find(15);

	ASSERT_NE(p0, nullptr);
	ASSERT_NE(p1, nullptr);
	ASSERT_NE(p2, nullptr);
	EXPECT_EQ(*p0, 42);
	EXPECT_EQ(*p1, 100);
	EXPECT_EQ(*p2, 7);

	// Keys that require different entry_nodes (different second-to-last nibble).
	tree_type tree2;
	tree2.insert(0x10, 1);
	tree2.insert(0x20, 2);

	auto *q0 = tree2.find(0x10);
	auto *q1 = tree2.find(0x20);
	ASSERT_NE(q0, nullptr);
	ASSERT_NE(q1, nullptr);
	EXPECT_EQ(*q0, 1);
	EXPECT_EQ(*q1, 2);
}

TEST(rcu_radixtree, find_missing) {
	tree_type tree;

	tree.insert(1, 42);

	EXPECT_EQ(tree.find(0), nullptr);
	EXPECT_EQ(tree.find(2), nullptr);
	EXPECT_EQ(tree.find(0xFFFF'FFFF'FFFF'FFFFULL), nullptr);
}

TEST(rcu_radixtree, find_or_insert_existing) {
	tree_type tree;

	auto [p1, inserted1] = tree.find_or_insert(5, 10);
	EXPECT_TRUE(inserted1);
	EXPECT_EQ(*p1, 10);

	auto [p2, inserted2] = tree.find_or_insert(5, 99);
	EXPECT_FALSE(inserted2);
	EXPECT_EQ(*p2, 10);
	EXPECT_EQ(p1, p2);
}

TEST(rcu_radixtree, erase) {
	tree_type tree;

	tree.insert(3, 42);
	ASSERT_NE(tree.find(3), nullptr);

	tree.erase(3);
	EXPECT_EQ(tree.find(3), nullptr);
}

TEST(rcu_radixtree, iteration) {
	tree_type tree;

	tree.insert(0, 1);
	tree.insert(1, 2);
	tree.insert(2, 3);

	int sum = 0;
	int count = 0;
	for (auto it = tree.begin(); it != tree.end(); ++it) {
		sum += *it;
		++count;
	}
	EXPECT_EQ(count, 3);
	EXPECT_EQ(sum, 6);
}

TEST(rcu_radixtree, lower_bound) {
	tree_type tree;

	EXPECT_TRUE(tree.lower_bound(0) == tree.end());

	// 1 and 7 share an entry_node, the other keys need their own.
	tree.insert(1, 10);
	tree.insert(7, 70);
	tree.insert(0x0123'4567'89AB'CDEFULL, 80);
	tree.insert(0xFFFF'FFFF'FFFF'FFFFULL, 90);

	// Exact matches and keys that fall into the gaps.
	EXPECT_EQ(tree.lower_bound(7).key(), 7u);
	EXPECT_EQ(tree.lower_bound(0).key(), 1u);
	EXPECT_EQ(*tree.lower_bound(2), 70);
	EXPECT_EQ(tree.lower_bound(8).key(), 0x0123'4567'89AB'CDEFULL);
	EXPECT_EQ(tree.lower_bound(0x0123'4567'89AB'CDF0ULL).key(), 0xFFFF'FFFF'FFFF'FFFFULL);

	// Incrementing past the largest possible key must not wrap around.
	auto it = tree.lower_bound(0xFFFF'FFFF'FFFF'FFFFULL);
	++it;
	EXPECT_TRUE(it == tree.end());
}

TEST(rcu_radixtree, lower_bound_empty_entry_node) {
	tree_type tree;

	tree.insert(0x100, 1);
	tree.insert(0x500, 2);

	// Erasure does not remove the entry_node, hence lower_bound() has to skip the
	// empty node that is left behind.
	tree.erase(0x100);

	EXPECT_EQ(tree.lower_bound(0).key(), 0x500u);
	EXPECT_EQ(tree.lower_bound(0x100).key(), 0x500u);
	EXPECT_TRUE(tree.lower_bound(0x501) == tree.end());
}

TEST(rcu_radixtree, lower_bound_deep_backtracking) {
	tree_type tree;

	// Keys that differ from zero only in nibble d yield a link_node at depth d,
	// i.e., inserting all of them builds a tree of maximal height.
	tree.insert(0, 0);
	for (unsigned int d = 0; d < 15; d++)
		tree.insert(uint64_t(1) << (64 - (d + 1) * 4), 1);

	// Erase everything but the largest key: lower_bound() now has to backtrack over
	// the full height of the tree.
	tree.erase(0);
	for (unsigned int d = 1; d < 15; d++)
		tree.erase(uint64_t(1) << (64 - (d + 1) * 4));

	EXPECT_EQ(tree.lower_bound(0).key(), uint64_t(1) << 60);
	EXPECT_TRUE(tree.lower_bound((uint64_t(1) << 60) + 1) == tree.end());
}

TEST(rcu_radixtree, iteration_across_entry_nodes) {
	tree_type tree;

	EXPECT_TRUE(tree.unsafe_begin() == tree.unsafe_end());

	tree.insert(1, 1);
	tree.insert(0x10, 2);
	tree.insert(0xFFFF'FFFF'FFFF'FFFFULL, 3);

	std::vector<uint64_t> expected{1, 0x10, 0xFFFF'FFFF'FFFF'FFFFULL};

	std::vector<uint64_t> keys;
	for (auto it = tree.begin(); it != tree.end(); ++it)
		keys.push_back(it.key());
	EXPECT_EQ(keys, expected);

	keys.clear();
	for (auto it = tree.unsafe_begin(); it != tree.unsafe_end(); ++it)
		keys.push_back(it.key());
	EXPECT_EQ(keys, expected);
}
