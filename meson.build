# frigg is header-only, so we do not need a compiler.
project('frigg')

incl = include_directories('include')

if not get_option('frigg_no_install')
	install_headers(
		'include/frg/algorithm.hpp',
		'include/frg/allocation.hpp',
		'include/frg/array.hpp',
		'include/frg/bitops.hpp',
		'include/frg/cmdline.hpp',
		'include/frg/container_of.hpp',
		'include/frg/detection.hpp',
		'include/frg/dyn_array.hpp',
		'include/frg/eternal.hpp',
		'include/frg/expected.hpp',
		'include/frg/formatting.hpp',
		'include/frg/functional.hpp',
		'include/frg/hash.hpp',
		'include/frg/hash_map.hpp',
		'include/frg/interval_tree.hpp',
		'include/frg/intrusive.hpp',
		'include/frg/list.hpp',
		'include/frg/logging.hpp',
		'include/frg/macros.hpp',
		'include/frg/manual_box.hpp',
		'include/frg/mutex.hpp',
		'include/frg/optional.hpp',
		'include/frg/pairing_heap.hpp',
		'include/frg/printf.hpp',
		'include/frg/random.hpp',
		'include/frg/rbtree.hpp',
		'include/frg/rcu.hpp',
		'include/frg/rcu_btree.hpp',
		'include/frg/rcu_radixtree.hpp',
		'include/frg/ringbuffer.hpp',
		'include/frg/safe_int.hpp',
		'include/frg/scope_exit.hpp',
		'include/frg/sharded_slab.hpp',
		'include/frg/shared_ptr.hpp',
		'include/frg/slab.hpp',
		'include/frg/small_vector.hpp',
		'include/frg/span.hpp',
		'include/frg/spinlock.hpp',
		'include/frg/stack.hpp',
		'include/frg/std_compat.hpp',
		'include/frg/string.hpp',
		'include/frg/string_stub.hpp',
		'include/frg/tuple.hpp',
		'include/frg/unique.hpp',
		'include/frg/utility.hpp',
		'include/frg/variant.hpp',
		'include/frg/vector.hpp',
		subdir: 'frg')

	pkg = import('pkgconfig')
	pkg.generate(
		name: 'frigg',
		description: 'Lightweight C++ utilities and algorithms for system programming',
	)
endif

frigg_dep = declare_dependency(include_directories: incl)


build_tests = get_option('build_tests').enabled()

if get_option('build_tests').auto()
	build_tests = not meson.is_subproject()
endif

build_slab_analyzer = get_option('build_slab_analyzer').enabled()

if get_option('build_slab_analyzer').auto()
	build_slab_analyzer = not meson.is_subproject()
endif


if build_tests or build_slab_analyzer
	# We need C++ only for the analyzer and test suite.
	add_languages('cpp')
endif


if build_tests
	subdir('tests')
	subdir('benchmarks')
endif

if build_slab_analyzer
	analyzer = executable(
			'slab_trace_analyzer',
			'slab_trace_analyzer.cpp',
			override_options: ['cpp_std=c++20'],
			native: true)
endif
