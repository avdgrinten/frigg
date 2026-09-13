#pragma once

#include <atomic>
#include <limits>
#include <type_traits>
#include <utility>
#include <frg/allocation.hpp>
#include <frg/macros.hpp>
#include <frg/rcu.hpp>

namespace frg FRG_VISIBILITY {

// K must be an integer type.
// V is loaded atomically by readers: it must be trivially copyable and std::atomic<V> lock-free.
template<typename K, typename V, typename Allocator, rcu_policy Rcu>
struct rcu_btree {
	static_assert(std::is_trivially_copyable_v<V>);
	static_assert(std::atomic<V>::is_always_lock_free);

private:
	static constexpr int nkeys = 7;
	// Minimum number of keys of non-root nodes.
	static constexpr int min_keys = 3;
	// Number of keys that remain in the left node when splitting.
	static constexpr int split = (nkeys + 1) / 2;

	// Leaves are never empty (lower_bound relies on this).
	static_assert(min_keys >= 1);
	// Splits do not produce underfull nodes (the right half of an inner node is the smallest).
	static_assert(nkeys + 1 - split >= min_keys && nkeys - split >= min_keys);
	// Merging an underfull node with a sibling that cannot lend a key fits into one node:
	// (min_keys - 1) + min_keys keys, plus the separator for inner nodes.
	static_assert(2 * min_keys <= nkeys);

	struct inner_node;

	// Part of the node that is common to inner nodes and leaves.
	struct node {
		node(bool leaf)
		: leaf{leaf} { }

		// Whether this is a leaf_node or an inner_node.
		bool leaf;
		int num_keys{0};
		union {
			// Parent pointer; non-atomic, hence writer-only.
			inner_node *parent{nullptr};
			// Link for the list of replaced nodes (see defer_retire_()).
			node *retire_next;
		};
		K keys[nkeys];
	};

	// Entry i of a node consists of keys[i] and links[i + link_offset].
	// Inner nodes additionally have a leading link links[0] that belongs to no entry.
	struct inner_node
	: node, Rcu::template obj_base<inner_node, destruct_with_allocator<Allocator>> {
		using link_type = node *;
		static constexpr bool is_leaf = false;
		static constexpr int link_offset = 1;

		inner_node()
		: node{false} { }

		std::atomic<node *> links[nkeys + 1]{};
	};

	struct leaf_node
	: node, Rcu::template obj_base<leaf_node, destruct_with_allocator<Allocator>> {
		using link_type = V;
		static constexpr bool is_leaf = true;
		static constexpr int link_offset = 0;

		leaf_node()
		: node{true} { }

		// The values associated with the keys.
		std::atomic<V> links[nkeys]{};
	};

	// A separator key together with the two children that it separates.
	// This is what splitting a node yields and what borrowing rotates through a parent.
	struct separator {
		node *left;
		K key;
		node *right;
	};

public:
	constexpr rcu_btree(Allocator allocator = Allocator())
	: allocator_{std::move(allocator)}, root_{nullptr} {}

	rcu_btree(const rcu_btree &) = delete;
	rcu_btree &operator=(const rcu_btree &) = delete;

	~rcu_btree() {
		// Iterative post-order traversal: destroyed nodes are unlinked from their parents,
		// hence the first non-null link is the next subtree to visit.
		auto n = root_.load(std::memory_order_relaxed);
		while(n) {
			node *child = nullptr;
			if(!n->leaf) {
				auto inner = static_cast<inner_node *>(n);
				for(int i = 0; i <= n->num_keys && !child; ++i)
					child = child_(inner, i);
			}
			if(child) {
				n = child;
				continue;
			}

			auto p = n->parent;
			if(p)
				set_link_(p, find_child_index_(p, n), nullptr);
			retire_(n);
			n = p;
		}
	}

	// ------------------------------------------------------------------------
	// Traversal with RCU support.
	// ------------------------------------------------------------------------
public:
	// Iterators are valid until the end of the RCU critical section in which they were obtained
	// (or, for the writer: until the next mutation).
	// Iteration is not a snapshot: each increment observes the live tree, but the visited keys are strictly increasing.
	struct iterator {
		friend struct rcu_btree;

		iterator() = default;

		// The value is loaded atomically since replace() can change it concurrently.
		std::pair<K, V> operator*() const {
			FRG_ASSERT(n_);
			return {n_->keys[idx_], n_->links[idx_].load(std::memory_order_acquire)};
		}

		iterator &operator++() {
			FRG_ASSERT(n_);
			auto key = n_->keys[idx_];
			if(++idx_ < n_->num_keys)
				return *this;

			// Leaves have no sibling links and parent pointers are writer-only: re-descend.
			if(key == std::numeric_limits<K>::max()) {
				*this = iterator{};
			} else {
				*this = tree_->lower_bound(key + 1);
			}
			return *this;
		}

		bool operator==(const iterator &other) const {
			return n_ == other.n_ && idx_ == other.idx_;
		}

	private:
		iterator(rcu_btree *tree, leaf_node *n, int idx)
		: tree_{tree}, n_{n}, idx_{idx} { }

		rcu_btree *tree_{nullptr};
		leaf_node *n_{nullptr};
		int idx_{0};
	};

	iterator begin() {
		return lower_bound(std::numeric_limits<K>::min());
	}

	iterator end() {
		return iterator{};
	}

	// Returns: An iterator to the entry with the given key, or end() if there is none.
	iterator find(K key) {
		auto leaf = descend_<std::memory_order_acquire>(
			root_.load(std::memory_order_acquire), key);
		if(!leaf)
			return end();
		int pos = find_position_(leaf, key);
		if(pos < leaf->num_keys && leaf->keys[pos] == key)
			return {this, leaf, pos};
		return end();
	}

	// Returns: An iterator to the first entry with a key >= key, or end() if there is none.
	iterator lower_bound(K key) {
		// Descend along probe (initially key) while tracking bound, the smallest separator to
		// the right of the path: the leaf only holds keys < bound. If it has no key >= key, all
		// such keys are >= bound, so re-descend along bound. Absent concurrent mutations, this
		// reaches a (non-empty) leaf whose keys are all >= bound. probe increases strictly.
		K probe = key;
		while(true) {
			auto cur = root_.load(std::memory_order_acquire);
			if(!cur)
				return end();

			bool has_bound = false;
			K bound{};
			while(!cur->leaf) {
				auto inner = static_cast<inner_node *>(cur);
				int i = find_inner_child_(inner, probe);
				if(i < inner->num_keys) {
					bound = inner->keys[i];
					has_bound = true;
				}
				cur = child_<std::memory_order_acquire>(inner, i);
			}

			auto leaf = static_cast<leaf_node *>(cur);
			int pos = find_position_(leaf, key);
			if(pos < leaf->num_keys)
				return {this, leaf, pos};
			if(!has_bound)
				return end();
			probe = bound;
		}
	}

private:
	template<std::memory_order Order = std::memory_order_relaxed>
	static node *child_(inner_node *n, int i) {
		return n->links[i].load(Order);
	}

	// Returns: The first i with keys[i] >= key, or num_keys if there is none.
	static int find_position_(node *n, K key) {
		int i = 0;
		while(i < n->num_keys && n->keys[i] < key)
			++i;
		return i;
	}

	// Returns: The index of the child whose subtree covers key, i.e., the first i with
	// key < keys[i], or num_keys if there is none.
	static int find_inner_child_(node *n, K key) {
		int i = 0;
		while(i < n->num_keys && !(key < n->keys[i]))
			++i;
		return i;
	}

	// Returns: The leaf below cur that would contain key, or nullptr if cur is null.
	template<std::memory_order Order>
	static leaf_node *descend_(node *cur, K key) {
		if(!cur)
			return nullptr;
		while(!cur->leaf) {
			auto inner = static_cast<inner_node *>(cur);
			cur = child_<Order>(inner, find_inner_child_(inner, key));
		}
		return static_cast<leaf_node *>(cur);
	}

	// ------------------------------------------------------------------------
	// Insertion and replacement (must use external locking).
	// ------------------------------------------------------------------------
public:
	// Precondition: Single writer (via external locking); key is not present.
	// Returns: An iterator to the new entry.
	iterator insert(K key, V value) {
		auto cur = descend_<std::memory_order_relaxed>(
			root_.load(std::memory_order_relaxed), key);

		if(!cur) {
			// Empty tree: create a single leaf.
			auto n = make_node_<leaf_node>();
			append_(n, key, value);
			root_.store(n, std::memory_order_release);
			return {this, n, 0};
		}

		int pos = find_position_(cur, key);
		FRG_ASSERT(pos == cur->num_keys || cur->keys[pos] != key);

		if(cur->num_keys < nkeys)
			return {this, insert_into_leaf_(cur, pos, key, value), pos};

		// Full leaf: split it and insert the separator into the parent, splitting full parents in turn.
		auto sep = split_leaf_(cur, pos, key, value);
		iterator it = (pos < split)
				? iterator{this, static_cast<leaf_node *>(sep.left), pos}
				: iterator{this, static_cast<leaf_node *>(sep.right), pos - split};
		node *retire_head = nullptr;
		node *old_child = cur;
		while(true) {
			auto parent = old_child->parent;
			defer_retire_(retire_head, old_child);

			if(!parent) {
				// Split cascaded all the way to the root: create a new root.
				root_.store(make_inner_(sep), std::memory_order_release);
				break;
			}

			int child_idx = find_child_index_(parent, old_child);
			if(parent->num_keys < nkeys) {
				// insert_into_inner_ publishes and retires parent itself.
				insert_into_inner_(parent, child_idx, sep);
				break;
			}

			sep = split_inner_(parent, child_idx, sep);
			old_child = parent;
		}
		retire_list_(retire_head);
		return it;
	}

	// Replaces the value of the entry that it points to.
	// Precondition: Single writer (via external locking); it was obtained after the last mutation.
	// Returns: The previous value.
	V replace(iterator it, V value) {
		auto &link = it.n_->links[it.idx_];
		auto old = link.load(std::memory_order_relaxed);
		link.store(value, std::memory_order_release);
		return old;
	}

private:
	// Inserts key at pos into a copy of the non-full leaf cur, publishes it and retires cur.
	// Returns: The new leaf.
	leaf_node *insert_into_leaf_(leaf_node *cur, int pos, K key, V value) {
		auto new_leaf = copy_(cur, 0, pos);
		append_(new_leaf, key, value);
		append_entries_(new_leaf, cur, pos, cur->num_keys);
		publish_(cur, new_leaf);
		cur->retire({allocator_});
		return new_leaf;
	}

	// Replaces the child at child_idx by sep in a copy of the non-full parent,
	// publishes it and retires parent.
	void insert_into_inner_(inner_node *parent, int child_idx, separator sep) {
		auto new_parent = copy_(parent, 0, child_idx);
		append_separator_(new_parent, sep);
		append_entries_(new_parent, parent, child_idx, parent->num_keys);
		fix_children_parents_(new_parent);
		publish_(parent, new_parent);
		parent->retire({allocator_});
	}

	// Splits the full leaf cur, with key inserted at pos, into a left leaf with split entries
	// and a right leaf with the rest.
	// Returns: The separator of the two new (unpublished) leaves.
	separator split_leaf_(leaf_node *cur, int pos, K key, V value) {
		leaf_node *left_leaf;
		leaf_node *right_leaf;
		if(pos < split) {
			left_leaf = copy_(cur, 0, pos);
			append_(left_leaf, key, value);
			append_entries_(left_leaf, cur, pos, split - 1);
			right_leaf = copy_(cur, split - 1, nkeys);
		} else {
			left_leaf = copy_(cur, 0, split);
			right_leaf = copy_(cur, split, pos);
			append_(right_leaf, key, value);
			append_entries_(right_leaf, cur, pos, nkeys);
		}
		return {left_leaf, right_leaf->keys[0], right_leaf};
	}

	// Splits the full inner node parent, with sep replacing the child at child_idx. Of the
	// nkeys + 1 keys, the left node gets split, the next one is promoted and the right node
	// gets the rest.
	// Returns: The promoted key with the two new (unpublished) inner nodes.
	separator split_inner_(inner_node *parent, int child_idx, separator sep) {
		inner_node *left_inner;
		inner_node *right_inner;
		K key;
		if(child_idx < split) {
			left_inner = copy_(parent, 0, child_idx);
			append_separator_(left_inner, sep);
			append_entries_(left_inner, parent, child_idx, split - 1);
			key = parent->keys[split - 1];
			right_inner = copy_(parent, split, nkeys);
		} else if(child_idx > split) {
			left_inner = copy_(parent, 0, split);
			key = parent->keys[split];
			right_inner = copy_(parent, split + 1, child_idx);
			append_separator_(right_inner, sep);
			append_entries_(right_inner, parent, child_idx, nkeys);
		} else {
			// sep.key is promoted. The child at child_idx is the last child of the left
			// and the first child of the right half: replace it by sep's children.
			left_inner = copy_(parent, 0, split);
			set_last_child_(left_inner, sep.left);
			key = sep.key;
			right_inner = copy_(parent, split, nkeys);
			set_link_(right_inner, 0, sep.right);
		}

		fix_children_parents_(left_inner);
		fix_children_parents_(right_inner);
		return {left_inner, key, right_inner};
	}

	// ------------------------------------------------------------------------
	// Deletion (must use external locking).
	// ------------------------------------------------------------------------
public:
	// Erases the entry that it points to.
	// Precondition: Single writer (via external locking); it was obtained after the last mutation.
	// Returns: The value of the erased entry.
	V erase(iterator it) {
		auto cur = it.n_;
		FRG_ASSERT(cur);
		auto value = link_(cur, it.idx_);

		auto new_leaf = copy_(cur, 0, it.idx_);
		append_entries_(new_leaf, cur, it.idx_ + 1, cur->num_keys);
		replace_node_(cur, new_leaf);
		return value;
	}

private:
	// Replaces the published leaf old by the unpublished leaf n, rebalancing if n is underfull.
	// A merge removes a key from the parent, which is replaced (and rebalanced) in turn.
	// Replaced nodes are retired only after the final publish.
	// Precondition: n has at least min_keys - 1 keys.
	void replace_node_(leaf_node *old, leaf_node *n) {
		node *retire_head = nullptr;
		auto up = rebalance_(old, n, retire_head);
		while(up.old)
			up = rebalance_(up.old, up.n, retire_head);
		retire_list_(retire_head);
	}

	// Pending replacement of a published inner node by an unpublished one.
	struct pending_replacement {
		inner_node *old{nullptr};
		inner_node *n{nullptr};
	};

	// One level of replace_node_(): publishes n in place of old after borrowing from or merging
	// with a sibling if n is underfull. As n is unpublished, it is modified in place or freed
	// without a grace period.
	// Returns: The replacement of the parent if a merge removed a key from it, else {}.
	template<typename N>
	pending_replacement rebalance_(N *old, N *n, node *&retire_head) {
		auto parent = old->parent;

		// The root is exempt from min_keys but must not be empty.
		if(n->num_keys >= (parent ? min_keys : 1)) {
			fix_children_parents_(n);
			publish_(old, n);
			defer_retire_(retire_head, old);
			return {};
		}

		if(!parent) {
			// Empty root: the tree becomes empty (leaf) or the only child becomes the root (inner).
			node *child = nullptr;
			if constexpr (!N::is_leaf) {
				child = child_(n, 0);
				child->parent = nullptr;
			}
			root_.store(child, std::memory_order_release);
			frg::destruct(allocator_, n);
			defer_retire_(retire_head, old);
			return {};
		}

		int idx = find_child_index_(parent, old);
		auto left = (idx > 0) ? static_cast<N *>(child_(parent, idx - 1)) : nullptr;
		auto right = (idx < parent->num_keys) ? static_cast<N *>(child_(parent, idx + 1)) : nullptr;

		if(left && left->num_keys > min_keys) {
			// Rotate the last entry of left into n (through parent for inner nodes).
			int lc = left->num_keys;
			prepend_(n, N::is_leaf ? left->keys[lc - 1] : parent->keys[idx - 1],
					last_link_(left));
			auto new_left = copy_(left, 0, lc - 1);
			fix_children_parents_(new_left);
			fix_children_parents_(n);
			replace_pair_(parent, idx - 1, {new_left, left->keys[lc - 1], n});
			defer_retire_(retire_head, left);
			defer_retire_(retire_head, old);
			return {};
		}
		if(right && right->num_keys > min_keys) {
			// Rotate the first entry of right into n (through parent for inner nodes).
			int rc = right->num_keys;
			append_(n, N::is_leaf ? right->keys[0] : parent->keys[idx], link_(right, 0));
			auto new_right = copy_(right, 1, rc);
			fix_children_parents_(n);
			fix_children_parents_(new_right);
			// For leaves, the new separator is the new first key of right.
			replace_pair_(parent, idx, {n, right->keys[N::is_leaf ? 1 : 0], new_right});
			defer_retire_(retire_head, right);
			defer_retire_(retire_head, old);
			return {};
		}

		// No sibling can lend an entry: merge n with a sibling.
		// In parent, the pair and its separator collapse into the merged node.
		int sep_idx = left ? idx - 1 : idx;
		N *merged;
		if(left) {
			merged = copy_(left);
			append_node_(merged, parent->keys[sep_idx], n);
			frg::destruct(allocator_, n);
			defer_retire_(retire_head, left);
		} else {
			// n is unpublished, hence right can be appended in place.
			merged = n;
			append_node_(merged, parent->keys[sep_idx], right);
			defer_retire_(retire_head, right);
		}
		fix_children_parents_(merged);
		defer_retire_(retire_head, old);

		// new_parent replaces parent but may be underfull.
		auto new_parent = copy_(parent, 0, sep_idx);
		set_last_child_(new_parent, merged);
		append_entries_(new_parent, parent, sep_idx + 1, parent->num_keys);
		return {parent, new_parent};
	}

	// ------------------------------------------------------------------------
	// Invariant validation (must use external locking).
	// ------------------------------------------------------------------------
public:
	// Returns: Whether the tree satisfies all structural invariants (see check_invariant_()).
	bool check_invariant() {
		auto root = root_.load(std::memory_order_relaxed);
		if(!root)
			return true;
		if(root->parent)
			return false;
		int depth;
		return check_invariant_(root, nullptr, nullptr, depth);
	}

private:
	// Checks the subtree rooted at n: key order, key ranges, number of keys, parent pointers
	// and uniform leaf depth. Keys must be >= *lower and < *upper (if non-null).
	// On success, depth is set to the height of n.
	bool check_invariant_(node *n, const K *lower, const K *upper, int &depth) {
		// The root is exempt from min_keys but must not be empty.
		if(n->num_keys < (n->parent ? min_keys : 1) || n->num_keys > nkeys)
			return false;
		for(int i = 0; i < n->num_keys; ++i) {
			if(i > 0 && !(n->keys[i - 1] < n->keys[i]))
				return false;
			if((lower && n->keys[i] < *lower) || (upper && !(n->keys[i] < *upper)))
				return false;
		}
		if(n->leaf) {
			depth = 0;
			return true;
		}

		// Children must point back to n and all leaves must be at the same depth.
		auto inner = static_cast<inner_node *>(n);
		for(int i = 0; i <= n->num_keys; ++i) {
			auto child = child_(inner, i);
			if(!child || child->parent != inner)
				return false;
			int child_depth;
			if(!check_invariant_(child, i > 0 ? &n->keys[i - 1] : lower,
					i < n->num_keys ? &n->keys[i] : upper, child_depth))
				return false;
			if(i > 0 && child_depth + 1 != depth)
				return false;
			depth = child_depth + 1;
		}
		return true;
	}

	// ------------------------------------------------------------------------
	// Internal helpers for mutation.
	//
	// New nodes are built front to back: copy a range of an existing node, then append to it,
	// such that each entry is copied only once. Only unpublished nodes are modified; source
	// nodes are only read.
	// ------------------------------------------------------------------------
private:
	template<typename N>
	N *make_node_() {
		return frg::construct<N>(allocator_);
	}

	// Returns: A new inner node with sep as its only entry.
	inner_node *make_inner_(separator sep) {
		auto n = make_node_<inner_node>();
		append_separator_(n, sep);
		fix_children_parents_(n);
		return n;
	}

	// Returns: A new node with entries [first, last) of src
	// (for inner nodes: with links[first] as leading link).
	template<typename N>
	N *copy_(N *src, int first, int last) {
		auto n = make_node_<N>();
		if constexpr (!N::is_leaf)
			set_link_(n, 0, link_(src, first));
		append_entries_(n, src, first, last);
		return n;
	}

	template<typename N>
	N *copy_(N *src) {
		return copy_(src, 0, src->num_keys);
	}

	template<typename N>
	static typename N::link_type link_(N *n, int i) {
		return n->links[i].load(std::memory_order_relaxed);
	}

	template<typename N>
	static void set_link_(N *n, int i, typename N::link_type link) {
		n->links[i].store(link, std::memory_order_relaxed);
	}

	template<typename N>
	static typename N::link_type last_link_(N *n) {
		return link_(n, n->num_keys - 1 + N::link_offset);
	}

	// Appends an entry to the non-full node n.
	template<typename N>
	static void append_(N *n, K key, typename N::link_type link) {
		FRG_ASSERT(n->num_keys < nkeys);
		set_link_(n, n->num_keys + N::link_offset, link);
		n->keys[n->num_keys++] = key;
	}

	// Appends entries [first, last) of src to dst.
	template<typename N>
	static void append_entries_(N *dst, N *src, int first, int last) {
		for(int i = first; i < last; ++i)
			append_(dst, src->keys[i], link_(src, i + N::link_offset));
	}

	// Appends all entries of src to dst; for inner nodes, preceded by sep with the leading
	// link of src.
	template<typename N>
	static void append_node_(N *dst, K sep, N *src) {
		if constexpr (!N::is_leaf)
			append_(dst, sep, link_(src, 0));
		append_entries_(dst, src, 0, src->num_keys);
	}

	// Sets the last child (links[num_keys]) of an inner node.
	static void set_last_child_(inner_node *n, node *child) {
		set_link_(n, n->num_keys, child);
	}

	// Replaces the last child of the non-full inner node n by sep.left, sep.key, sep.right.
	static void append_separator_(inner_node *n, separator sep) {
		set_last_child_(n, sep.left);
		append_(n, sep.key, sep.right);
	}

	// Inserts key and link at the front of the non-full node n (for inner nodes, link becomes
	// the leading link). Shifts all entries: only used when borrowing from a sibling.
	template<typename N>
	static void prepend_(N *n, K key, typename N::link_type link) {
		FRG_ASSERT(n->num_keys < nkeys);
		for(int i = n->num_keys; i > 0; --i)
			n->keys[i] = n->keys[i - 1];
		for(int i = n->num_keys + N::link_offset; i > 0; --i)
			set_link_(n, i, link_(n, i - 1));
		n->keys[0] = key;
		set_link_(n, 0, link);
		++n->num_keys;
	}

	// Sets the parent pointers of all children of n to n. No-op for leaves.
	static void fix_children_parents_(leaf_node *) { }

	static void fix_children_parents_(inner_node *n) {
		for(int i = 0; i <= n->num_keys; ++i)
			child_(n, i)->parent = n;
	}

	// Returns: The i with links[i] == child.
	static int find_child_index_(inner_node *parent, node *child) {
		for(int i = 0; i <= parent->num_keys; ++i) {
			if(child_(parent, i) == child)
				return i;
		}
		// Not found: the tree is corrupted.
		__builtin_trap();
	}

	// Publishes new_node in place of old_node by updating the link of the parent (or root_).
	void publish_(node *old_node, node *new_node) {
		new_node->parent = old_node->parent;
		if(old_node->parent) {
			int idx = find_child_index_(old_node->parent, old_node);
			old_node->parent->links[idx].store(new_node, std::memory_order_release);
		} else {
			root_.store(new_node, std::memory_order_release);
		}
	}

	// Replaces the children links[i], links[i + 1] and their separator keys[i] by sep in a copy
	// of the published parent, publishes it and retires parent (but not the children).
	void replace_pair_(inner_node *parent, int i, separator sep) {
		auto new_parent = copy_(parent, 0, i);
		append_separator_(new_parent, sep);
		append_entries_(new_parent, parent, i + 1, parent->num_keys);
		fix_children_parents_(new_parent);
		publish_(parent, new_parent);
		parent->retire({allocator_});
	}

	void retire_(node *n) {
		if(n->leaf) {
			static_cast<leaf_node *>(n)->retire({allocator_});
		} else {
			static_cast<inner_node *>(n)->retire({allocator_});
		}
	}

	// Nodes may only be retired after the publish that makes them unreachable. Mutations that
	// publish at the end of a cascade collect replaced nodes in a list linked through retire_next.
	static void defer_retire_(node *&head, node *n) {
		n->retire_next = head;
		head = n;
	}

	void retire_list_(node *head) {
		while(head) {
			auto next = head->retire_next;
			retire_(head);
			head = next;
		}
	}

private:
	Allocator allocator_;
	std::atomic<node *> root_{nullptr};
};

} // namespace frg
