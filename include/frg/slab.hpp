#pragma once

#include <stddef.h>
#include <stdint.h>
#include <frg/bitops.hpp>
#include <frg/string_stub.hpp>
#include <frg/macros.hpp>
#include <frg/mutex.hpp>
#include <frg/rbtree.hpp>
#include <frg/detection.hpp>

namespace frg FRG_VISIBILITY {

namespace slab {

template<typename P>
concept has_poisoning_support = requires(P policy) {
	policy.poison(nullptr, size_t{0});
	policy.unpoison(nullptr, size_t{0});
};

template<typename Policy>
concept has_trace_support = requires (Policy p) { p.enable_trace(); }
	&& requires (Policy p, void *buffer, size_t size) { p.output_trace(buffer, size); }
	&& requires (Policy p) { p.walk_stack([] (uintptr_t) {}); };

template<typename Policy>
void trace(Policy &plcy, char c, void *ptr, size_t size) {
	if constexpr (has_trace_support<Policy>) {
		if (!plcy.enable_trace())
			return;

		const int num_frames = 12;
		const size_t bufsize = 1 // Record type.
			+ 16                 // Pointer and size.
			+ num_frames * 8     // Stack trace.
			+ 8;                 // Terminator.
		uint8_t buffer[bufsize];
		size_t n = 0;

		auto add_byte = [&] (uint8_t val) {
			FRG_ASSERT(n + 1 <= bufsize);
			buffer[n++] = val;
		};

		auto add_word = [&] (uintptr_t val) {
			FRG_ASSERT(n + 8 <= bufsize);
			for (int i = 0; i < 8; i++)
				buffer[n++] = (val >> (i * 8)) & 0xFF;
		};

		add_byte(c);
		add_word(reinterpret_cast<uintptr_t>(ptr));
		if (c == 'a')
			add_word(size);

		int k = 0;
		plcy.walk_stack([&](uintptr_t val){
			if(k >= num_frames)
				return;
			add_word(val);
			++k;
		});

		add_word(0xA5A5A5A5A5A5A5A5);

		plcy.output_trace(buffer, n);
	}
}

} // namespace slab

namespace {
	// TODO: We need a frigg logging mechanism to enable this.
	//constexpr bool logAllocations = true;

	constexpr bool enable_checking = false;
}

template<typename T, size_t N>
constexpr size_t array_size(const T (&)[N]) {
	return N;
}

template<typename Policy>
using policy_slabsize_t = decltype(Policy::slabsize);

template<typename Policy>
using policy_pagesize_t = decltype(Policy::pagesize);

template<typename Policy>
using policy_num_buckets_t = decltype(Policy::num_buckets);

template<typename Policy>
using policy_poison_t = decltype(std::declval<Policy>().poison(nullptr, size_t(0)));

template<typename Policy>
using policy_map_aligned_t = decltype(std::declval<Policy>().map(size_t(0), size_t(0)));

// Common configuration for slab allocators (slab_pool and shared_slab_pool).
template<typename Policy>
struct slab_policy_traits {
	// The following variables configure the size of the buckets.
	// Bucket size increases both with small_base_exp and small_step_exp. Furthermore,
	// small_step_exp controls how many buckets are between any two power-of-2 buckets.
	// The first bucket has a size of size_t(1) << (small_base_exp + small_step_exp).
	// This approach is taken from jemalloc.
#ifdef __clang__
	static constexpr size_t tiny_sizes[4] = {8, 16, 32, 64};
#else
	static constexpr size_t tiny_sizes[] = {8, 16, 32, 64};
#endif
	static constexpr unsigned int small_base_exp = 6;
	static constexpr unsigned int small_step_exp = 0;

	static_assert(tiny_sizes[array_size(tiny_sizes) - 1]
			== (static_cast<size_t>(1) << (small_base_exp + small_step_exp)),
		"Last tiny bucket must match first small bucket");

	// Computes the size of a given bucket.
	static constexpr size_t bucket_to_size(unsigned int idx) {
		// First, we handle the hard-coded tiny sizes.
		auto tc = array_size(tiny_sizes);
		if(idx < tc)
			return tiny_sizes[idx];

		// Next, we handle the small sizes.
		auto s = 1 << small_step_exp;
		auto ip = (idx - tc + 1) >> small_step_exp;
		auto is = (idx - tc + 1) & (s - 1);
		auto f = small_base_exp + ip;
		return static_cast<size_t>(s + is) << f;
	}

	// The "inverse" of bucket_to_size().
	static constexpr size_t size_to_bucket(size_t size) {
		// First, we handle the hard-coded tiny sizes.
		auto tc = array_size(tiny_sizes);
		if(size <= bucket_to_size(tc - 1)) {
			for(unsigned int i = 0; i < tc - 1; i++)
				if(size <= bucket_to_size(i))
					return i;
			return tc - 1;
		}

		// Next, we handle the small sizes. Variables correspond to those in bucket_to_size().
		auto e = floor_log2(size);
		auto f = e - small_step_exp;
		auto ip = ((f - small_base_exp) << small_step_exp);
		auto is = ((size - (static_cast<size_t>(1) << e)) + (static_cast<size_t>(1) << f) - 1) >> f;
		return tc - 1 + ip + is;
	}

	// Documentation only: aligning a size to 2^a implies that the resulting bucket is also aligned to 2^a.
	static constexpr bool aligned_size_implies_aligned_bucket() {
		// Proof: Let M be the padded size.
		// Let e be such that 2^e <= M < 2^(e+1). Note that a <= e (otherwise M >= 2^(e+1) since M is a multiple of 2^a).
		// By construction, the size classes between 2^e and 2^(e+1) are 2^e + L * 2^g(e) with some function g.
		// If a <= g(e), we are done since then 2^a divides both 2^e and 2^g(e).
		// Hence, consider the case where a > g(e).
		// Since M is aligned to 2^a, we can write M as M = 2^e + k * 2^a.
		// Since a > g(e), 2^g(e) divides 2^a. But in this case, L = k * 2^a / 2^g(e) is an integer
		// and thus M is a size class itself.
		return true;
	}

	// This variable controls the number of buckets that we actually use.
	static constexpr int num_buckets = [](){
		if constexpr (is_detected_v<policy_num_buckets_t, Policy>)
			return Policy::num_buckets;
		else
			return 13;
	}();

	static constexpr size_t max_bucket_size = bucket_to_size(num_buckets - 1);

	// Here, we perform some compile-time verification of the bucket size calculation.
	static constexpr bool test_bucket_calculation(unsigned int n) {
		for(unsigned int i = 0; i < n; i++) {
			if(size_to_bucket(bucket_to_size(i)) != i)
				return false;
			if(size_to_bucket(bucket_to_size(i) + 1) != i + 1)
				return false;
		}
		return true;
	}

	static_assert(test_bucket_calculation(num_buckets),
		"The bucket size calculation seems to be broken");
};

template<typename Policy, typename Mutex>
class slab_pool {
public:
	constexpr slab_pool(Policy &plcy);

	slab_pool(const slab_pool &) = delete;

	slab_pool &operator= (const slab_pool &) = delete;

	void *allocate(size_t length);
	void *realloc(void *pointer, size_t new_length);
	void free(void *pointer);
	void deallocate(void *pointer, size_t size);
	size_t get_size(void *pointer);

	size_t numUsedPages() {
		return _usedPages;
	}

private:
	using policy_traits = slab_policy_traits<Policy>;

	static constexpr size_t page_size = [](){
		if constexpr (is_detected_v<policy_pagesize_t, Policy>)
			return Policy::pagesize;
		else
			return 0x1000;
	}();

	static constexpr size_t sb_size = [] {
		if constexpr (requires { Policy::sb_size; }) {
			return Policy::sb_size;
		}else{
			return 1 << 18;
		}
	}();

	// Size of the content of a slab.
	static constexpr size_t slabsize = [](){
		if constexpr (is_detected_v<policy_slabsize_t, Policy>)
			return Policy::slabsize;
		else
			return 1 << 18;
	}();

	static_assert(sb_size >= slabsize);

	static_assert(!(sb_size & (page_size - 1)),
			"Superblock size must be a multiple of the page size");
	static_assert(!(slabsize & (page_size - 1)),
			"Slab size must be a multiple of the page size");

	// TODO: Refactor the huge frame padding.
	static constexpr size_t huge_padding = page_size;

	struct freelist {
		freelist()
		: link{nullptr} { }

		freelist(const freelist &) = delete;

		freelist &operator= (const freelist &) = delete;

		freelist *link;
	};

	enum class frame_type {
		null,
		slab,
		large
	};

	// By frame we mean either a slab_frame or a large memory object.
	// Frames are stored in a tree to allow fast lookup by in-frame addresses.
	struct frame {
		frame(frame_type type_, uintptr_t address_, size_t length_)
		: type{type_}, address{address_}, length{length_} { }

		frame(const frame &) = delete;

		frame &operator= (const frame &) = delete;

		bool contains(void *p) {
			auto adr = reinterpret_cast<uintptr_t>(p);
			return adr >= address && adr < address + length;
		}

		const frame_type type;

		// Base address of this superblock.
		// This is the address that is returned by Policy::map().
		uintptr_t sb_base;

		// Memory reserved for this superblock. Generally larger than 'length'.
		// This is the length passed to Policy::map();
		size_t sb_reservation;

		const uintptr_t address;
		const size_t length;
#ifdef FRG_SLAB_TRACK_REGIONS
		rbtree_hook frame_hook;
#endif
	};
	static_assert(sizeof(frame) <= huge_padding, "Padding too small");

	struct slab_frame : frame {
		slab_frame(uintptr_t address_, size_t length_, int index_)
		: frame{frame_type::slab, address_, length_},
				index{index_}, num_reserved{0}, available{nullptr} { }

		slab_frame(const slab_frame &) = delete;

		slab_frame &operator= (const slab_frame &) = delete;

		const int index;
		unsigned int num_reserved;
		freelist *available;
		rbtree_hook partial_hook;
	};

	struct frame_less {
		bool operator() (const frame &a, const frame &b) {
			return a.address < b.address;
		}
	};

#ifdef FRG_SLAB_TRACK_REGIONS
	using frame_tree_type = frg::rbtree<
		frame,
		&frame::frame_hook,
		frame_less
	>;
#endif // FRG_SLAB_TRACK_REGIONS

	using partial_tree_type = frg::rbtree<
		slab_frame,
		&slab_frame::partial_hook,
		frame_less
	>;

	// Like in jemalloc, we always allocate a slab completely (the head_slb) before
	// moving to the next slab. This reduces external fragmentation.
	struct bucket {
		bucket()
		: head_slb{nullptr} { }

		Mutex bucket_mutex;
		slab_frame *head_slb;
		partial_tree_type partial_tree;
	};

private:
	//--------------------------------------------------------------------------------------
	// Slab handling.
	//--------------------------------------------------------------------------------------

	slab_frame *_construct_slab(int index);

	bool reallocate_in_slab_(slab_frame *slb, void *p, size_t new_size) {
		size_t item_size = policy_traits::bucket_to_size(slb->index);
		FRG_ASSERT(slb->contains(p));
		FRG_ASSERT(!enable_checking
				|| !((reinterpret_cast<uintptr_t>(p) - slb->address) % item_size));

		if(new_size > item_size)
			return false;

		if constexpr (slab::has_poisoning_support<Policy>) {
			_plcy.unpoison_expand(p, item_size);
			_plcy.poison(p, item_size);
			_plcy.unpoison(p, new_size);
		}
		return true;
	}

	void free_in_slab_(slab_frame *slb, void *p) {
		size_t item_size = policy_traits::bucket_to_size(slb->index);
		FRG_ASSERT(slb->contains(p));
		FRG_ASSERT(!enable_checking
				|| !((reinterpret_cast<uintptr_t>(p) - slb->address) % item_size));

		if constexpr (slab::has_poisoning_support<Policy>) {
			_plcy.unpoison_expand(p, item_size);
			_plcy.poison(p, item_size);
			_plcy.unpoison(p, sizeof(freelist));
		}
		auto object = new (p) freelist;

		auto bkt = &_bkts[slb->index];
		unique_lock<Mutex> bucket_guard(bkt->bucket_mutex);
		{
			bool reinsert_into_bucket = !slb->available;
			FRG_ASSERT(slb->num_reserved);

			FRG_ASSERT(!slb->available || slb->contains(slb->available));
			object->link = slb->available;
			slb->available = object;

			if(reinsert_into_bucket) {
				bkt->partial_tree.insert(slb);
				if(!bkt->head_slb || slb->address < bkt->head_slb->address)
					bkt->head_slb = slb;
			}
		}
	}

	//--------------------------------------------------------------------------------------
	// Huge superblock handling.
	//--------------------------------------------------------------------------------------

	frame *_construct_large(size_t area_size);

	bool reallocate_huge_(frame *sup, void *p, size_t new_size) {
		FRG_ASSERT(sup->address == reinterpret_cast<uintptr_t>(p));

		if(new_size > sup->length)
			return false;

		if constexpr (slab::has_poisoning_support<Policy>) {
			_plcy.unpoison_expand(p, sup->length);
			_plcy.poison(p, sup->length);
			_plcy.unpoison(p, new_size);
		}
		return true;
	}

	void free_huge_(frame *sup, void *p) {
		FRG_ASSERT(sup->address == reinterpret_cast<uintptr_t>(p));

		// Remove the virtual area from the area-list.
		{
			unique_lock<Mutex> tree_guard(_tree_mutex);

#ifdef FRG_SLAB_TRACK_REGIONS
			_frame_tree.remove(sup);
#endif
			_usedPages -= (sup->length + huge_padding) / page_size;
		}

		// Note: we cannot access sup after poison().
		auto sb_base = sup->sb_base;
		auto sb_reservation = sup->sb_reservation;
		auto obj_address = sup->address;
		auto obj_size = sup->length;
		if constexpr (slab::has_poisoning_support<Policy>) {
			_plcy.poison(sup, sizeof(frame));
			_plcy.poison(reinterpret_cast<void *>(obj_address), obj_size);
		}
		_plcy.unmap(sb_base, sb_reservation);
	}

	//--------------------------------------------------------------------------------------

	void _verify_integrity();
	void _verify_frame_integrity(frame *fra);

private:
	Policy &_plcy;

	Mutex _tree_mutex;
#ifdef FRG_SLAB_TRACK_REGIONS
	frame_tree_type _frame_tree;
#endif
	size_t _usedPages;
	bucket _bkts[policy_traits::num_buckets];
};

// --------------------------------------------------------
// slab_pool
// --------------------------------------------------------

template<typename Policy, typename Mutex>
constexpr slab_pool<Policy, Mutex>::slab_pool(Policy &plcy)
: _plcy{plcy}, _usedPages{0} { }

template<typename Policy, typename Mutex>
void *slab_pool<Policy, Mutex>::allocate(size_t length) {
	if(enable_checking)
		_verify_integrity();

	// malloc() is allowed to either return null or a unique value.
	// However, some programs always interpret null returns as failure,
	// so we round up the length.
	if(!length)
		length = 1;

	if(length <= policy_traits::max_bucket_size) {
		int index = policy_traits::size_to_bucket(length);
		FRG_ASSERT(index <= policy_traits::num_buckets);
		auto bkt = &_bkts[index];

		unique_lock<Mutex> bucket_guard(bkt->bucket_mutex);

		freelist *object;
		if(bkt->head_slb) {
			auto slb = bkt->head_slb;

			object = slb->available;
			FRG_ASSERT(object);
			FRG_ASSERT(slb->contains(object));
			if(object->link && !slb->contains(object->link))
				FRG_ASSERT(!"slab_pool corruption. Possible write to unallocated object");
			slb->available = object->link;
			slb->num_reserved++;

			if(!slb->available) {
				bkt->partial_tree.remove(slb);
				bkt->head_slb = bkt->partial_tree.first();
			}
		}else{
			// Call into the Policy without holding locks.
			bucket_guard.unlock();

			auto slb = _construct_slab(index);
			if(!slb)
				return nullptr;

			object = slb->available;
			FRG_ASSERT(object);
			FRG_ASSERT(slb->contains(object));
			if(object->link && !slb->contains(object->link))
				FRG_ASSERT(!"slab_pool corruption. Possible write to unallocated object");
			slb->available = object->link;
			slb->num_reserved++;

			unique_lock<Mutex> tree_guard(_tree_mutex);
#ifdef FRG_SLAB_TRACK_REGIONS
			_frame_tree.insert(slb);
#endif
			_usedPages += (slb->length + huge_padding) / page_size;
			tree_guard.unlock();

			// Finally, re-lock the bucket to attach the new slab.
			bucket_guard.lock();

			FRG_ASSERT(slb->available);
			bkt->partial_tree.insert(slb);
			if(!bkt->head_slb || slb->address < bkt->head_slb->address)
				bkt->head_slb = slb;
		}

		bucket_guard.unlock();

		//if(logAllocations)
		//	std::cout << "frg/slab: Allocate small-object at " << object << std::endl;
		object->~freelist();
		if constexpr (slab::has_poisoning_support<Policy>) {
			_plcy.poison(object, sizeof(freelist));
			_plcy.unpoison(object, length);
		}
		if(enable_checking)
			_verify_integrity();
		slab::trace(_plcy, 'a', object, length);
		return object;
	}else{
		auto area_size = (length + page_size - 1) & ~(page_size - 1);
		auto fra = _construct_large(area_size);
		if(!fra)
			return nullptr;

		unique_lock<Mutex> tree_guard(_tree_mutex);
#ifdef FRG_SLAB_TRACK_REGIONS
		_frame_tree.insert(fra);
#endif
		_usedPages += (fra->length + huge_padding) / page_size;
		tree_guard.unlock();

		//if(logAllocations)
		//	std::cout << "frg/slab: Allocate large-object at " <<
		//			(void *)fra->address << std::endl;
		if(enable_checking)
			_verify_integrity();
		slab::trace(_plcy, 'a', reinterpret_cast<void *>(fra->address), length);
		return reinterpret_cast<void *>(fra->address);
	}
}

template<typename Policy, typename Mutex>
void *slab_pool<Policy, Mutex>::realloc(void *p, size_t new_size) {
	if(enable_checking)
		_verify_integrity();

	// Handle special cases first.
	if(!p) {
		return allocate(new_size);
	}else if(!new_size) {
		free(p);
		return nullptr;
	}

	auto address = reinterpret_cast<uintptr_t>(p);
	auto sup = reinterpret_cast<frame *>((address - 1) & ~(sb_size - 1));
	size_t current_size;
	if(sup->type == frame_type::slab) {
		auto slb = static_cast<slab_frame *>(sup);
		if(reallocate_in_slab_(slb, p, new_size))
			return p;
		current_size = policy_traits::bucket_to_size(slb->index);
	}else{
		FRG_ASSERT(sup->type == frame_type::large);
		if(reallocate_huge_(sup, p, new_size))
			return p;
		current_size = sup->length;
	}
	FRG_ASSERT(current_size < new_size);

	// Fallback path that copies the memory region.
	void *new_p = allocate(new_size);
	if(!new_p)
		return nullptr;
	memcpy(new_p, p, current_size);
	free(p);
	return new_p;
}

template<typename Policy, typename Mutex>
void slab_pool<Policy, Mutex>::free(void *p) {
	if(enable_checking)
		_verify_integrity();

	slab::trace(_plcy, 'f', p, 0);

	if(!p)
		return;

	//if(logAllocations)
	//	std::cout << "frg/slab: Free " << p << std::endl;

	auto address = reinterpret_cast<uintptr_t>(p);
	auto sup = reinterpret_cast<frame *>((address - 1) & ~(sb_size - 1));
	if(sup->type == frame_type::slab) {
		auto slb = static_cast<slab_frame *>(sup);
		free_in_slab_(slb, p);
	}else{
		FRG_ASSERT(sup->type == frame_type::large);
		free_huge_(sup, p);
	}

	if(enable_checking)
		_verify_integrity();
}

template<typename Policy, typename Mutex>
void slab_pool<Policy, Mutex>::deallocate(void *p, size_t size) {
	if(enable_checking)
		_verify_integrity();

	slab::trace(_plcy, 'f', p, 0);

	if(!p)
		return;

	//if(logAllocations)
	//	std::cout << "frg/slab: Free " << p << std::endl;

	auto address = reinterpret_cast<uintptr_t>(p);
	auto sup = reinterpret_cast<frame *>((address - 1) & ~(sb_size - 1));
	if(sup->type == frame_type::slab) {
		auto slb = static_cast<slab_frame *>(sup);
		FRG_ASSERT(size <= policy_traits::bucket_to_size(slb->index));
		free_in_slab_(slb, p);
	}else{
		FRG_ASSERT(sup->type == frame_type::large);
		FRG_ASSERT(size <= sup->length);
		free_huge_(sup, p);
	}

	if(enable_checking)
		_verify_integrity();
}

template<typename Policy, typename Mutex>
size_t slab_pool<Policy, Mutex>::get_size(void *p) {
	if(enable_checking)
		_verify_integrity();

	if(!p)
		return 0;

	auto address = reinterpret_cast<uintptr_t>(p);
	auto sup = reinterpret_cast<frame *>((address - 1) & ~(sb_size - 1));

	if(sup->type == frame_type::slab) {
		auto slb = static_cast<slab_frame *>(sup);
		return policy_traits::bucket_to_size(slb->index);
	}

	FRG_ASSERT(sup->type == frame_type::large);
	return sup->length;
}


template<typename Policy, typename Mutex>
auto slab_pool<Policy, Mutex>::_construct_slab(int index)
-> slab_frame * {
//	frg::infoLogger() << "Allocate new area for " << (void *)area_size << frg::endLog;

	// Allocate virtual memory for the slab.
	size_t sb_reservation;
	uintptr_t sb_base;
	uintptr_t address;
	if constexpr (is_detected_v<policy_map_aligned_t, Policy>) {
		sb_reservation = slabsize;
		sb_base = _plcy.map(sb_reservation, sb_size);
		if(!sb_base)
			return nullptr;
		address = sb_base;
	} else {
		sb_reservation = slabsize + sb_size;
		sb_base = _plcy.map(sb_reservation);
		if(!sb_base)
			return nullptr;
		address = (sb_base + sb_size - 1) & ~(sb_size - 1);
	}

	auto item_size = policy_traits::bucket_to_size(index);
	size_t overhead = 0;
	while(overhead < sizeof(slab_frame)) // FIXME.
		overhead += item_size;
	FRG_ASSERT(overhead < slabsize);

	if constexpr (slab::has_poisoning_support<Policy>)
		_plcy.unpoison(reinterpret_cast<void *>(address), sizeof(slab_frame));
	auto slb = new (reinterpret_cast<void *>(address)) slab_frame(
			address + overhead, slabsize - overhead, index);
	slb->sb_base = sb_base;
	slb->sb_reservation = sb_reservation;

	//if(logAllocations)
	//	std::cout << "frb/slab: New area at " << area << std::endl;

	// Partition the slab into individual objects.
	freelist *first = nullptr;
	for(size_t off = 0; off < slb->length; off += item_size) {
		if constexpr (slab::has_poisoning_support<Policy>)
			_plcy.unpoison(reinterpret_cast<void *>(slb->address + off), sizeof(freelist));
		auto object = new (reinterpret_cast<void *>(slb->address + off)) freelist;
		object->link = first;
		first = object;
		//infoLogger() << "[slab] fill " << chunk << frg::endLog;
	}
	slb->available = first;

	return slb;
}

template<typename Policy, typename Mutex>
auto slab_pool<Policy, Mutex>::_construct_large(size_t area_size)
-> frame * {
//	frg::infoLogger() << "Allocate new area for " << (void *)area_size << frg::endLog;

	// Allocate virtual memory for the frame.
	FRG_ASSERT(!(area_size & (page_size - 1)));
	size_t sb_reservation;
	uintptr_t sb_base;
	uintptr_t address;
	if constexpr (is_detected_v<policy_map_aligned_t, Policy>) {
		sb_reservation = area_size + huge_padding;
		sb_base = _plcy.map(sb_reservation, sb_size);
		if(!sb_base)
			return nullptr;
		address = sb_base;
	} else {
		sb_reservation = area_size + huge_padding + sb_size;
		sb_base = _plcy.map(area_size + huge_padding + sb_size);
		if(!sb_base)
			return nullptr;
		address = (sb_base + sb_size - 1) & ~(sb_size - 1);
	}
	if constexpr (slab::has_poisoning_support<Policy>) {
		_plcy.unpoison(reinterpret_cast<void *>(address), sizeof(frame));
		_plcy.unpoison(reinterpret_cast<void *>(address + huge_padding), area_size);
	}

	auto fra = new ((void *)address) frame(frame_type::large,
			address + huge_padding, area_size);
	fra->sb_base = sb_base;
	fra->sb_reservation = sb_reservation;

	//if(logAllocations)
	//	std::cout << "frb/slab: New area at " << area << std::endl;

	return fra;
}

template<typename Policy, typename Mutex>
void slab_pool<Policy, Mutex>::_verify_integrity() {
#ifdef FRG_SLAB_TRACK_REGIONS
	unique_lock<Mutex> tree_guard(_tree_mutex);
	if(_frame_tree.get_root())
		_verify_frame_integrity(_frame_tree.get_root());
#endif
}

template<typename Policy, typename Mutex>
void slab_pool<Policy, Mutex>::_verify_frame_integrity(frame *fra) {
	if(fra->type == frame_type::slab) {
		auto slb = static_cast<slab_frame *>(fra);
		auto bkt = &_bkts[slb->index];
		unique_lock<Mutex> slab_guard(bkt->bucket_mutex);

		auto object = slb->available;
		while(object) {
			FRG_ASSERT(slb->contains(object));
			object = object->link;
		}
	}

#ifdef FRG_SLAB_TRACK_REGIONS
	if(_frame_tree.get_left(fra))
		_verify_frame_integrity(frame_tree_type::get_left(fra));
	if(_frame_tree.get_right(fra))
		_verify_frame_integrity(frame_tree_type::get_right(fra));
#endif
}

// --------------------------------------------------------
// slab_allocator
// --------------------------------------------------------

template<typename Policy, typename Mutex>
class slab_allocator {
public:
	constexpr slab_allocator(slab_pool<Policy, Mutex> *pool)
	: pool_{pool} { }

	void *allocate(size_t size) {
		return pool_->allocate(size);
	}

	void deallocate(void *pointer, size_t size) {
		pool_->deallocate(pointer, size);
	}

	void free(void *pointer) {
		pool_->free(pointer);
	}

	void *reallocate(void *pointer, size_t new_size) {
		return pool_->realloc(pointer, new_size);
	}

	size_t get_size(void *pointer) {
		return pool_->get_size(pointer);
	}

private:
	slab_pool<Policy, Mutex> *pool_;
};

} // namespace frg
