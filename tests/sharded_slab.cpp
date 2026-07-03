#include <atomic>
#include <sys/mman.h>
#include <thread>
#include <vector>

#include <frg/sharded_slab.hpp>
#include <frg/spinlock.hpp>
#include <gtest/gtest.h>

// Plain mapping policy without over-alignment support.
struct sharded_slab_base_policy {
	void *map(size_t size) {
		void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
		               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED)
			return nullptr;
		return p;
	}

	void unmap(void *p, size_t size) {
		munmap(p, size);
	}
};

// The policy used by most tests: opts into over-aligned allocations with an aligned map().
struct sharded_slab_policy : sharded_slab_base_policy {
	static constexpr bool support_overaligned = true;
	using mutex_type = frg::simple_spinlock;

	using sharded_slab_base_policy::map;

	// Over-map by the alignment and trim to an aligned region.
	void *map(size_t size, size_t alignment) {
		size_t over = size + alignment;
		void *raw = mmap(nullptr, over, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (raw == MAP_FAILED)
			return nullptr;
		auto base = reinterpret_cast<uintptr_t>(raw);
		auto aligned = (base + alignment - 1) & ~(alignment - 1);
		if (aligned > base)
			munmap(reinterpret_cast<void *>(base), aligned - base);
		if (aligned + size < base + over)
			munmap(reinterpret_cast<void *>(aligned + size), (base + over) - (aligned + size));
		return reinterpret_cast<void *>(aligned);
	}
};

using domain_type = frg::sharded_slab::domain<sharded_slab_policy>;
using pool_type = frg::sharded_slab::pool<sharded_slab_policy>;

// Opts in but provides no aligned map(): over-aligned objects use the over-allocation fallback.
struct sharded_slab_fallback_policy : sharded_slab_base_policy {
	static constexpr bool support_overaligned = true;
	using mutex_type = frg::simple_spinlock;
};

using fallback_domain_type = frg::sharded_slab::domain<sharded_slab_fallback_policy>;
using fallback_pool_type = frg::sharded_slab::pool<sharded_slab_fallback_policy>;

// Check all powers of two from 2^0 to 2^30 to check whether all size classes and large allocations work.
TEST(sharded_slab, multiple_sizes) {
	domain_type dom;
	pool_type pool{&dom};

	for (int s = 0; s <= 30; s++) {
		size_t size = size_t{1} << s;
		void *obj = pool.allocate(size);
		ASSERT_NE(obj, nullptr);
		memset(obj, 0xFF, size);
		pool.deallocate(obj);
	}
}

// Allocate with alignments below chunk_boundary (slab and large paths).
TEST(sharded_slab, alignment) {
	domain_type dom;
	pool_type pool{&dom};

	for (int a = 0; a < frg::floor_log2(pool_type::chunk_boundary); a++) {
		size_t alignment = size_t{1} << a;
		for (size_t size : {alignment / 2, alignment, alignment * 2}) {
			if (!size)
				continue;
			void *obj = pool.allocate(size, alignment);
			ASSERT_NE(obj, nullptr);
			EXPECT_EQ(reinterpret_cast<uintptr_t>(obj) & (alignment - 1), uintptr_t{0});
			memset(obj, 0xFF, size);
			pool.deallocate(obj);
		}
	}
}

// Allocate with alignments at and above chunk_boundary (shared side structure).
TEST(sharded_slab, overalignment) {
	domain_type dom;
	pool_type pool{&dom};

	for (int a = frg::floor_log2(pool_type::chunk_boundary); a <= 24; a++) {
		size_t alignment = size_t{1} << a;
		for (size_t size : {alignment / 2, alignment, alignment * 2}) {
			void *obj = pool.allocate(size, alignment);
			ASSERT_NE(obj, nullptr);
			EXPECT_EQ(reinterpret_cast<uintptr_t>(obj) & (alignment - 1), uintptr_t{0});
			EXPECT_GE(pool.get_size(obj), size);
			memset(obj, 0xFF, size);
			pool.deallocate(obj);
		}
	}
}

// Over-aligned objects from one pool can be freed by another in the same domain.
TEST(sharded_slab, overaligned_cross_thread) {
	constexpr size_t count = 256;
	size_t alignment = pool_type::chunk_boundary;

	domain_type dom;
	pool_type main_pool{&dom};
	std::vector<void *> objs(count);

	for (size_t i = 0; i < count; i++) {
		objs[i] = main_pool.allocate(alignment, alignment);
		ASSERT_NE(objs[i], nullptr);
		EXPECT_EQ(reinterpret_cast<uintptr_t>(objs[i]) & (alignment - 1), uintptr_t{0});
		memset(objs[i], 0xFF, alignment);
	}
	std::thread t([&] {
		pool_type thread_pool{&dom};
		for (size_t i = 0; i < count; i++)
			thread_pool.deallocate(objs[i]);
	});
	t.join();
}

// Over-aligned objects fall back to over-allocation when map() has no aligned overload.
TEST(sharded_slab, overalignment_fallback) {
	fallback_domain_type dom;
	fallback_pool_type pool{&dom};

	for (int a = frg::floor_log2(fallback_pool_type::chunk_boundary); a <= 22; a++) {
		size_t alignment = size_t{1} << a;
		for (size_t size : {alignment / 2, alignment, alignment * 2}) {
			void *obj = pool.allocate(size, alignment);
			ASSERT_NE(obj, nullptr);
			EXPECT_EQ(reinterpret_cast<uintptr_t>(obj) & (alignment - 1), uintptr_t{0});
			EXPECT_GE(pool.get_size(obj), size);
			memset(obj, 0xFF, size);
			pool.deallocate(obj);
		}
	}
}

// Without opt-in, normal allocations still work but over-aligned requests fail.
TEST(sharded_slab, overaligned_unsupported) {
	frg::sharded_slab::pool<sharded_slab_base_policy> pool;

	void *p = pool.allocate(128);
	ASSERT_NE(p, nullptr);
	pool.deallocate(p);

	EXPECT_EQ(pool.allocate(128, pool_type::chunk_boundary), nullptr);
}

// Allocate enough objects to exhaust one chunk.
TEST(sharded_slab, exhaust_chunk) {
	constexpr size_t count = 20000;

	domain_type dom;
	pool_type pool{&dom};
	std::vector<void *> objs(count);

	for (size_t i = 0; i < 5; ++i) {
		for (size_t i = 0; i < count; i++) {
			objs[i] = pool.allocate(128);
			ASSERT_NE(objs[i], nullptr);
			memset(objs[i], 0xFF, 128);
		}
		for (size_t i = 0; i < count; i++)
			pool.deallocate(objs[i]);
	}
}

TEST(sharded_slab, pointer_uniqueness) {
	constexpr size_t count = 1000;

	domain_type dom;
	pool_type pool{&dom};
	std::vector<void *> objs(count);

	for (size_t i = 0; i < 5; ++i) {
		for (size_t i = 0; i < count; i++) {
			objs[i] = pool.allocate(128);
			ASSERT_NE(objs[i], nullptr);
			memset(objs[i], 0xFF, 128);
		}
		for (size_t i = 0; i < count; i++) {
			for (size_t j = i + 1; j < count; j++) {
				EXPECT_NE(objs[i], objs[j]);
			}
		}
		for (size_t i = 0; i < count; i++)
			pool.deallocate(objs[i]);
	}
}

TEST(sharded_slab, cross_thread_deallocation) {
	constexpr size_t count = 20000;

	domain_type dom;
	pool_type main_pool{&dom};
	std::vector<void *> objs(count);

	for (size_t i = 0; i < 5; ++i) {
		// Allocate in main thread + free in another thread.
		for (size_t i = 0; i < count; i++) {
			objs[i] = main_pool.allocate(128);
			ASSERT_NE(objs[i], nullptr);
			memset(objs[i], 0xFF, 128);
		}
		std::thread t([&] {
			pool_type thread_pool{&dom};
			for (size_t i = 0; i < count; i++)
				thread_pool.deallocate(objs[i]);
		});
		t.join();

		// Allocate in main thread + free in main thread.
		for (size_t i = 0; i < count; i++) {
			objs[i] = main_pool.allocate(128);
			ASSERT_NE(objs[i], nullptr);
			memset(objs[i], 0xFF, 128);
		}
		for (size_t i = 0; i < count; i++)
			main_pool.deallocate(objs[i]);
	}
}

TEST(sharded_slab, reallocate) {
	domain_type dom;
	pool_type pool{&dom};

	size_t delta = 15;
	std::array<size_t, 2> sizes = {256 - delta, 1024 * 1024 - delta};

	for (size_t size : sizes) {
		void *p = pool.reallocate(nullptr, size);
		ASSERT_NE(p, nullptr);
		memset(p, 0x42, size);

		// Grow by small amount.
		size_t grow_size = size + delta;
		void *p_grow = pool.reallocate(p, grow_size);
		ASSERT_NE(p_grow, nullptr);
		memset(static_cast<unsigned char *>(p_grow) + size, 0x42, grow_size - size);
		for(size_t i = 0; i < grow_size; ++i) {
			EXPECT_EQ(static_cast<unsigned char *>(p_grow)[i], 0x42);
		}

		// Shrink in place.
		void *p_shrink = pool.reallocate(p_grow, size / 2);
		EXPECT_EQ(p_shrink, p_grow);

		pool.reallocate(p_shrink, 0);
	}

	for (size_t size : sizes) {
		void *p = pool.reallocate(nullptr, size);
		ASSERT_NE(p, nullptr);
		memset(p, 0x42, size);

		// Grow by large amount.
		size_t grow_size = 3 * size;
		void *p_grow = pool.reallocate(p, grow_size);
		ASSERT_NE(p_grow, nullptr);
		memset(static_cast<unsigned char *>(p_grow) + size, 0x42, grow_size - size);
		for(size_t i = 0; i < grow_size; ++i) {
			EXPECT_EQ(static_cast<unsigned char *>(p_grow)[i], 0x42);
		}

		// Shrink in place.
		void *p_shrink = pool.reallocate(p_grow, size / 2);
		EXPECT_EQ(p_shrink, p_grow);

		pool.reallocate(p_shrink, 0);
	}
}

TEST(sharded_slab, get_size) {
	domain_type dom;
	pool_type pool{&dom};

	EXPECT_EQ(pool.get_size(nullptr), 0);

	size_t small_size = 127;
	void *p_small = pool.allocate(small_size);
	EXPECT_GE(pool.get_size(p_small), small_size);
	pool.deallocate(p_small);

	size_t large_size = 1024 * 1024 - 1;
	void *p_large = pool.allocate(large_size);
	EXPECT_GE(pool.get_size(p_large), large_size);
	pool.deallocate(p_large);
}

struct poison_policy : sharded_slab_base_policy {
	static inline std::vector<std::pair<uintptr_t, uintptr_t>> ranges;

	void clear_range(uintptr_t start, uintptr_t end) {
		std::vector<std::pair<uintptr_t, uintptr_t>> new_ranges;
		for (const auto &r : ranges) {
			if (r.second <= start || r.first >= end) {
				// No overlap.
				new_ranges.push_back(r);
			} else {
				// Overlap.
				if (r.first < start)
					new_ranges.emplace_back(r.first, start);
				if (r.second > end)
					new_ranges.emplace_back(end, r.second);
			}
		}
		ranges = std::move(new_ranges);
	}

	void poison(void *p, size_t size) {
		uintptr_t address = reinterpret_cast<uintptr_t>(p);
		clear_range(address, address + size);
	}

	void unpoison(void *p, size_t size) {
		uintptr_t address = reinterpret_cast<uintptr_t>(p);
		clear_range(address, address + size);
		ranges.emplace_back(address, address + size);
	}

	void unpoison_expand(void *p, size_t size) {
		uintptr_t address = reinterpret_cast<uintptr_t>(p);
		clear_range(address, address + size);
		ranges.emplace_back(address, address + size);
	}
};

TEST(sharded_slab, poisoning) {
	frg::sharded_slab::pool<poison_policy> pool;

	// Range must be unpoisoned after allocate().
	size_t size = 128;
	void *p = pool.allocate(size);
	auto address = reinterpret_cast<uintptr_t>(p);
	bool exact_match = std::any_of(
		poison_policy::ranges.begin(),
		poison_policy::ranges.end(),
		[&] (const auto &r) {
			return r.first == address && r.second == address + size;
		}
	);
	EXPECT_TRUE(exact_match);

	// Range must be poisoned after allocate().
	pool.deallocate(p);
	// Note: Ideally we would be able to test for non-overlap here.
	//       However, that is not possible since the freelists are always unpoisoned.
	exact_match = std::any_of(
		poison_policy::ranges.begin(),
		poison_policy::ranges.end(),
		[&] (const auto &r) {
			return r.first == address && r.second == address + size;
		}
	);
	EXPECT_FALSE(exact_match);
}

struct trace_policy : sharded_slab_base_policy {
	static inline std::vector<uint8_t> buffer;

	bool enable_trace() { return true; }

	void output_trace(void *buf, size_t size) {
		const uint8_t *b = static_cast<const uint8_t *>(buf);
		buffer.insert(buffer.end(), b, b + size);
	}

	template<typename F>
	void walk_stack(F fn) {
		fn(0x1234);
	}
};

TEST(sharded_slab, tracing) {
	frg::sharded_slab::pool<trace_policy> pool;
	uint64_t word;

	// Verify trace of allocation.
	void *p = pool.allocate(128);

	// Expected: 'a' (1) + ptr (8) + size (8) + stack (8) + term (8) = 33 bytes.
	ASSERT_EQ(trace_policy::buffer.size(), 33);
	EXPECT_EQ(trace_policy::buffer[0], 'a');

	memcpy(&word, &trace_policy::buffer[1], 8);
	EXPECT_EQ(word, reinterpret_cast<uintptr_t>(p));

	memcpy(&word, &trace_policy::buffer[9], 8);
	EXPECT_EQ(word, 128);

	memcpy(&word, &trace_policy::buffer[17], 8);
	EXPECT_EQ(word, 0x1234);

	memcpy(&word, &trace_policy::buffer[25], 8);
	EXPECT_EQ(word, 0xA5A5A5A5A5A5A5A5ULL);

	// Verify trace of deallocation.
	trace_policy::buffer.clear();
	pool.deallocate(p);

	// Expected: 'f' (1) + ptr (8) + stack (8) + term (8) = 25 bytes.
	ASSERT_EQ(trace_policy::buffer.size(), 25);
	EXPECT_EQ(trace_policy::buffer[0], 'f');

	memcpy(&word, &trace_policy::buffer[1], 8);
	EXPECT_EQ(word, reinterpret_cast<uintptr_t>(p));

	memcpy(&word, &trace_policy::buffer[9], 8);
	EXPECT_EQ(word, 0x1234);

	memcpy(&word, &trace_policy::buffer[17], 8);
	EXPECT_EQ(word, 0xA5A5A5A5A5A5A5A5ULL);
}
