#include <atomic>
#include <barrier>
#include <cstdlib>
#include <mutex>
#include <sys/mman.h>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>
#include <frg/random.hpp>
#include <frg/sharded_slab.hpp>
#include <frg/slab.hpp>
#include <frg/spinlock.hpp>
#include <mimalloc.h>

// Helper data structures.

struct message_node {
	std::atomic<message_node *> next{nullptr};
};

struct message_queue {
	std::atomic<message_node *> head{nullptr};

	void push(message_node *node) {
		message_node *old_head = head.load(std::memory_order_relaxed);
		do {
			node->next.store(old_head, std::memory_order_relaxed);
		} while (!head.compare_exchange_weak(
			old_head, node,
			std::memory_order_release,
			std::memory_order_relaxed));
	}

	message_node *pop_all() {
		return head.exchange(nullptr, std::memory_order_acquire);
	}
};

// Data structures for frg::slab_pool.

struct slab_policy {
	uintptr_t map(size_t size) {
		void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
		                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (ptr == MAP_FAILED)
			return 0;
		return reinterpret_cast<uintptr_t>(ptr);
	}

	void unmap(uintptr_t ptr, size_t size) {
		munmap(reinterpret_cast<void *>(ptr), size);
	}
};

slab_policy global_slab_policy;
frg::slab_pool<slab_policy, std::mutex> global_slab_pool{global_slab_policy};

struct slab_instance {
	void *allocate(size_t size) {
		return global_slab_pool.allocate(size);
	}

	void deallocate(void *ptr) {
		global_slab_pool.free(ptr);
	}
};

// Data structures for frg::sharded_slab_pool.

struct sharded_slab_policy {
	void *map(size_t size) {
		void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
		                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (ptr == MAP_FAILED)
			return nullptr;
		return ptr;
	}

	void *map(size_t size, size_t alignment) {
		size_t over = size + alignment;
		void *raw = mmap(nullptr, over, PROT_READ | PROT_WRITE,
		                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

	void unmap(void *ptr, size_t size) {
		munmap(ptr, size);
	}

	static constexpr bool support_overaligned = true;
	using mutex_type = frg::simple_spinlock;
};

struct sharded_slab_instance {
	frg::sharded_slab::domain<sharded_slab_policy> domain;
	frg::sharded_slab::pool<sharded_slab_policy> pool{&domain};

	void *allocate(size_t size) {
		return pool.allocate(size);
	}

	void *allocate(size_t size, size_t alignment) {
		return pool.allocate(size, alignment);
	}

	void deallocate(void *ptr) {
		pool.deallocate(ptr);
	}
};

// Data structures for system allocator.

struct system_instance {
	void *allocate(size_t size) {
		return std::malloc(size);
	}

	void *allocate(size_t size, size_t alignment) {
		return std::aligned_alloc(alignment, size);
	}

	void deallocate(void *ptr) {
		std::free(ptr);
	}
};

// Data structures for mimalloc.

struct mimalloc_instance {
	void *allocate(size_t size) {
		return mi_malloc(size);
	}

	void *allocate(size_t size, size_t alignment) {
		return mi_malloc_aligned(size, alignment);
	}

	void deallocate(void *ptr) {
		mi_free(ptr);
	}
};

template <typename Instance>
static void BM_Allocators_MsgPass(benchmark::State &state) {
	constexpr size_t objects_per_thread = 10000;

	size_t num_threads = state.range(0);

	std::atomic<bool> running{true};
	std::barrier<> iter_barrier(num_threads + 1);
	std::barrier<> done_barrier(num_threads + 1);
	std::barrier<> phase_barrier(num_threads);
	std::vector<message_queue> queues(num_threads);

	auto thread_main = [&] (size_t thread_id) {
		Instance instance;
		frg::pcg_basic32 rng(0);
		size_t pass = 0;

		while (true) {
			iter_barrier.arrive_and_wait();
			if (!running.load(std::memory_order_relaxed))
				break;
			rng.seed(thread_id + pass * num_threads);
			pass++;

			// Allocation phase: allocate objects and push to random queues.
			for (size_t i = 0; i < objects_per_thread; i++) {
				void *ptr = instance.allocate(sizeof(message_node));
				auto *node = new (ptr) message_node{};
				size_t target = rng(num_threads);
				queues[target].push(node);
			}

			// Wait for all threads to finish allocation.
			phase_barrier.arrive_and_wait();

			// Deallocation phase: free all objects from our own queue.
			message_node *node = queues[thread_id].pop_all();
			while (node) {
				message_node *next = node->next.load(std::memory_order_relaxed);
				instance.deallocate(node);
				node = next;
			}

			done_barrier.arrive_and_wait();
		}
	};

	std::vector<std::thread> threads;
	for (size_t i = 0; i < num_threads; i++)
		threads.emplace_back(thread_main, i);

	auto iteration = [&] {
		// Signal workers to start.
		iter_barrier.arrive_and_wait();
		// Wait for workers to finish this iteration.
		done_barrier.arrive_and_wait();
	};

	// Warm up.
	for (size_t i = 0; i < 3; ++i)
		iteration();
	// Timed benchmark.
	for (auto _ : state)
		iteration();

	// Signal workers to terminate.
	running.store(false, std::memory_order_relaxed);
	iter_barrier.arrive_and_wait();

	for (auto &t : threads)
		t.join();

	state.SetItemsProcessed(state.iterations() * num_threads * objects_per_thread);
}

BENCHMARK(BM_Allocators_MsgPass<slab_instance>)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)
    ->Unit(benchmark::kMillisecond)
    ->MeasureProcessCPUTime();

BENCHMARK(BM_Allocators_MsgPass<sharded_slab_instance>)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)
    ->Unit(benchmark::kMillisecond)
    ->MeasureProcessCPUTime();

BENCHMARK(BM_Allocators_MsgPass<system_instance>)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)
    ->Unit(benchmark::kMillisecond)
    ->MeasureProcessCPUTime();

BENCHMARK(BM_Allocators_MsgPass<mimalloc_instance>)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8)
    ->Unit(benchmark::kMillisecond)
    ->MeasureProcessCPUTime();

// Round-trip latency of an over-aligned allocation with alignment == size.
// Pages are never touched, so this measures the allocation path, not faulting in memory.
template <typename Instance>
static void BM_Overaligned(benchmark::State &state) {
	size_t alignment = size_t{1} << state.range(0);

	Instance instance;
	for (auto _ : state) {
		void *ptr = instance.allocate(alignment, alignment);
		if (!ptr) {
			state.SkipWithError("over-aligned allocation failed");
			break;
		}
		benchmark::DoNotOptimize(ptr);
		instance.deallocate(ptr);
	}

	state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_Overaligned<sharded_slab_instance>)
    ->Arg(21)  // 2 MiB
    ->Arg(30)  // 1 GiB
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_Overaligned<system_instance>)
    ->Arg(21)  // 2 MiB
    ->Arg(30)  // 1 GiB
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_Overaligned<mimalloc_instance>)
    ->Arg(21)  // 2 MiB
    ->Arg(30)  // 1 GiB
    ->Unit(benchmark::kMicrosecond);
