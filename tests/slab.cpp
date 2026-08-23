#include <cstdint>
#include <sys/mman.h>

#include <frg/slab.hpp>
#include <frg/spinlock.hpp>
#include <gtest/gtest.h>

namespace {

struct slab_policy {
	uintptr_t map(size_t length) {
		if (fail_maps)
			return 0;
		void *p = mmap(nullptr, length, PROT_READ | PROT_WRITE,
		               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (p == MAP_FAILED)
			return 0;
		return reinterpret_cast<uintptr_t>(p);
	}

	void unmap(uintptr_t address, size_t length) {
		munmap(reinterpret_cast<void *>(address), length);
	}

	bool fail_maps = false;
};

using pool = frg::slab_pool<slab_policy, frg::simple_spinlock>;
using allocator = frg::slab_allocator<slab_policy, frg::simple_spinlock>;

} // namespace

TEST(slab, allocation_failure) {
	slab_policy plcy;
	pool p{plcy};
	allocator alloc{&p};

	void *small = alloc.allocate(64);
	EXPECT_NE(small, nullptr);
	alloc.free(small);

	EXPECT_EQ(alloc.allocate(SIZE_MAX / 4), nullptr);

	plcy.fail_maps = true;
	EXPECT_EQ(alloc.allocate(1024 * 1024), nullptr);
	plcy.fail_maps = false;

	void *after = alloc.allocate(64);
	EXPECT_NE(after, nullptr);
	alloc.free(after);
}

TEST(slab, oversized_allocations_do_not_wrap) {
	slab_policy plcy;
	pool p{plcy};
	allocator alloc{&p};

	EXPECT_EQ(alloc.allocate(SIZE_MAX), nullptr);
	EXPECT_EQ(alloc.allocate(SIZE_MAX - 4095), nullptr);

	EXPECT_EQ(alloc.allocate(SIZE_MAX & ~size_t{4095}), nullptr);
}
