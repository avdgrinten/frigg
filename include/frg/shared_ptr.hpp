#pragma once

#include <atomic>
#include <concepts>
#include <utility>

#include <frg/allocation.hpp>
#include <frg/macros.hpp>

namespace frg FRG_VISIBILITY {

struct adopt_rc_t { };
inline constexpr adopt_rc_t adopt_rc;

struct intrusive_rc {
	friend void ref_rc(intrusive_rc *obj) {
		auto rc = obj->rc_.fetch_add(1, std::memory_order_relaxed);
		FRG_ASSERT(rc);
	}

	// Returns true if the caller should destroy the object.
	friend bool unref_rc(intrusive_rc *obj) {
		auto rc = obj->rc_.fetch_sub(1, std::memory_order_acq_rel);
		return rc == 1;
	}

protected:
	~intrusive_rc() = default;

private:
	std::atomic<unsigned int> rc_{1};
};

template<typename T, typename Allocator>
requires std::derived_from<T, intrusive_rc>
struct intrusive_shared_ptr {
	intrusive_shared_ptr() = default;

	intrusive_shared_ptr(adopt_rc_t, T *ptr, Allocator alloc = {})
	: ptr_{ptr}, alloc_{std::move(alloc)} { }

	intrusive_shared_ptr(const intrusive_shared_ptr &other)
	: ptr_{other.ptr_}, alloc_{other.alloc_} {
		if(ptr_)
			ref_rc(ptr_);
	}

	intrusive_shared_ptr(intrusive_shared_ptr &&other)
	: intrusive_shared_ptr{} {
		std::swap(ptr_, other.ptr_);
		std::swap(alloc_, other.alloc_);
	}

	~intrusive_shared_ptr() {
		if(!ptr_)
			return;
		if (unref_rc(ptr_))
			frg::destruct(alloc_, ptr_);
	}

	intrusive_shared_ptr &operator=(intrusive_shared_ptr other) {
		std::swap(ptr_, other.ptr_);
		std::swap(alloc_, other.alloc_);
		return *this;
	}

	explicit operator bool() const {
		return ptr_ != nullptr;
	}

	T *get() const {
		return ptr_;
	}

	// Releases ownership of the reference held by this pointer.
	// The caller now owns the reference (i.e., the refcount is not decremented).
	T *release() {
		return std::exchange(ptr_, nullptr);
	}

	T *operator->() const {
		return ptr_;
	}

	T &operator*() const {
		return *ptr_;
	}

private:
	T *ptr_{nullptr};
	FRG_NO_UNIQUE_ADDRESS Allocator alloc_{};
};

template<typename T, typename Allocator, typename... Args>
intrusive_shared_ptr<T, Allocator> allocate_intrusive_shared(Allocator alloc, Args&&... args) {
	auto ptr = frg::construct<T>(alloc, std::forward<Args>(args)...);
	return intrusive_shared_ptr<T, Allocator>{adopt_rc, ptr, std::move(alloc)};
}

} // namespace frg
