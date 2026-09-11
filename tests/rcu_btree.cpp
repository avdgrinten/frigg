#include <cstdint>
#include <limits>
#include <utility>

#include <frg/rcu_btree.hpp>
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

using tree_type = frg::rcu_btree<int, int, frg::stl_allocator, fake_rcu_policy>;

TEST(rcu_btree, insert_and_find) {
	tree_type tree;

	EXPECT_EQ(tree.find(5), tree.end());

	EXPECT_EQ(*tree.insert(5, 50), std::pair(5, 50));
	EXPECT_EQ(*tree.insert(9, 90), std::pair(9, 90));
	EXPECT_EQ(*tree.find(5), std::pair(5, 50));
	EXPECT_EQ(*tree.find(9), std::pair(9, 90));
	EXPECT_EQ(tree.find(7), tree.end());

	// replace() returns the previous value.
	EXPECT_EQ(tree.replace(tree.find(5), 51), 50);
	EXPECT_EQ(*tree.find(5), std::pair(5, 51));
}

TEST(rcu_btree, value_types) {
	struct point {
		uint32_t x, y;
		bool operator==(const point &) const = default;
	};

	int a, b;
	frg::rcu_btree<int, int *, frg::stl_allocator, fake_rcu_policy> pointers;
	pointers.insert(1, &a);
	EXPECT_EQ(pointers.replace(pointers.find(1), &b), &a);
	EXPECT_EQ(*pointers.find(1), std::pair(1, &b));

	frg::rcu_btree<int, point, frg::stl_allocator, fake_rcu_policy> points;
	points.insert(1, {2, 3});
	EXPECT_EQ(points.erase(points.find(1)), (point{2, 3}));
	EXPECT_EQ(points.begin(), points.end());
}

TEST(rcu_btree, split) {
	// Enough entries to split leaves and to cascade splits through inner nodes.
	constexpr int n = 200;

	tree_type ascending;
	tree_type descending;
	for (int i = 0; i < n; ++i) {
		EXPECT_EQ(*ascending.insert(i, i), std::pair(i, i));
		EXPECT_EQ(*descending.insert(n - 1 - i, n - 1 - i), std::pair(n - 1 - i, n - 1 - i));
	}

	for (int i = 0; i < n; ++i) {
		EXPECT_EQ(*ascending.find(i), std::pair(i, i));
		EXPECT_EQ(*descending.find(i), std::pair(i, i));
	}
	EXPECT_EQ(ascending.find(n), ascending.end());
}

TEST(rcu_btree, lower_bound) {
	tree_type tree;

	EXPECT_EQ(tree.lower_bound(0), tree.end());

	// Even keys 2, 4, ..., 400, i.e., enough for multiple levels of inner nodes.
	constexpr int n = 200;
	for (int i = 0; i < n; ++i)
		tree.insert(2 * (i + 1), i);

	// Exact matches and keys that fall into the gaps (including gaps between leaves).
	for (int i = 0; i < n; ++i) {
		EXPECT_EQ(*tree.lower_bound(2 * (i + 1)), std::pair(2 * (i + 1), i));
		EXPECT_EQ(*tree.lower_bound(2 * i + 1), std::pair(2 * (i + 1), i));
	}
	EXPECT_EQ(tree.lower_bound(2 * n + 1), tree.end());
}

TEST(rcu_btree, iterate) {
	tree_type tree;
	EXPECT_EQ(tree.begin(), tree.end());

	// Enough entries such that iteration crosses many leaves.
	constexpr int n = 200;
	for (int i = 0; i < n; ++i)
		tree.insert(2 * i, i);

	int i = 0;
	for (auto [k, v] : tree) {
		EXPECT_EQ(k, 2 * i);
		EXPECT_EQ(v, i);
		++i;
	}
	EXPECT_EQ(i, n);

	// Incrementing past the maximal key yields end().
	auto last = tree.insert(std::numeric_limits<int>::max(), n);
	EXPECT_EQ(++last, tree.end());
}

TEST(rcu_btree, erase) {
	tree_type tree;

	for (int i = 0; i < 8; ++i)
		tree.insert(i, i);

	// Erase from a leaf that keeps other keys.
	EXPECT_EQ(tree.erase(tree.find(2)), 2);
	EXPECT_EQ(tree.find(2), tree.end());
	EXPECT_EQ(*tree.lower_bound(2), std::pair(3, 3));

	// Erase everything else; this borrows from and merges with sibling leaves.
	for (int i = 0; i < 8; ++i) {
		if (i == 2)
			continue;
		EXPECT_EQ(tree.erase(tree.find(i)), i);
		for (int j = 0; j < 8; ++j)
			EXPECT_EQ(tree.find(j) != tree.end(), j > i && j != 2);
	}

	// The emptied tree accepts insertions again.
	tree.insert(0, 0);
	EXPECT_EQ(*tree.find(0), std::pair(0, 0));
}

TEST(rcu_btree, erase_rebalancing) {
	// Erasing from either end exercises borrowing from left/right siblings and
	// merging, both for leaves and for inner nodes.
	constexpr int n = 200;

	tree_type ascending;
	tree_type descending;
	for (int i = 0; i < n; ++i) {
		ascending.insert(i, i);
		descending.insert(i, i);
	}

	for (int i = 0; i < n; ++i) {
		EXPECT_EQ(ascending.erase(ascending.find(i)), i);
		EXPECT_EQ(descending.erase(descending.find(n - 1 - i)), n - 1 - i);
		for (int j = 0; j < n; ++j) {
			EXPECT_EQ(ascending.find(j) != ascending.end(), j > i);
			EXPECT_EQ(descending.find(j) != descending.end(), j < n - 1 - i);
		}
	}
}
