cmake_minimum_required(VERSION 3.20)
project(lindberghrecomp VERSION 0.1.0 LANGUAGES C)

# Runtime for statically recompiled Sega Lindbergh games.
#
# The runtime cannot be a plain static library: two of its headers
# (recomp_imports.h, recomp_funcs_list.h) are generated from the game's own
# ELF, so the sources only compile in the context of one title. Hence a
# function a game project calls instead:
#
#   add_subdirectory(lindberghrecomp)
#   add_executable(mygame src/host.c ${GENERATED})
#   lindbergh_runtime(mygame games/mygame/generated)
#
# The build must be 32-bit. The CPU model in pcrecomp/runtime/recomp32_cpu is
# flat - a guest register holds a real host address - and a Lindbergh ELF is
# linked at 0x08048000, which only exists as an address in a 32-bit process.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(LINDBERGH_RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/runtime"
    CACHE INTERNAL "")
set(LINDBERGH_CPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/pcrecomp/runtime/recomp32_cpu"
    CACHE INTERNAL "")

if(NOT EXISTS "${LINDBERGH_CPU_DIR}/cpu.h")
    message(FATAL_ERROR
        "pcrecomp submodule is missing (${LINDBERGH_CPU_DIR}/cpu.h).\n"
        "  git submodule update --init --recursive")
endif()

function(lindbergh_runtime target generated_dir)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR
            "${target} is being built 64-bit. Recompiled Lindbergh code holds "
            "guest addresses in host registers and the game maps at 0x08048000, "
            "so the host must be 32-bit: cmake -B build -A Win32")
    endif()
    target_sources(${target} PRIVATE
        "${LINDBERGH_RUNTIME_DIR}/guest.c"
        "${LINDBERGH_RUNTIME_DIR}/syscall.c"
        "${LINDBERGH_RUNTIME_DIR}/hle.c"
        "${LINDBERGH_RUNTIME_DIR}/hle_libc.c"
        "${LINDBERGH_RUNTIME_DIR}/dispatch.c"
        "${LINDBERGH_RUNTIME_DIR}/crash_report.c"
    )
    target_include_directories(${target} PRIVATE
        "${LINDBERGH_RUNTIME_DIR}"
        "${LINDBERGH_CPU_DIR}"
        "${generated_dir}"
    )
    if(MSVC)
        # Generated code is correct and ugly: it converts between widths on
        # every other line and leaves labels no branch reaches.
        target_compile_options(${target} PRIVATE
            /W1 /wd4244 /wd4267 /wd4018 /wd4102 /wd4101 /wd4146 /wd4996)
        # No ASLR: the image is mapped at the addresses it was linked for.
        #
        # LARGEADDRESSAWARE because the guest is a Linux process and Linux
        # gives one 3 GB of user address space - it puts the stack up at
        # 0xBFFF0000. A 32-bit Windows process gets 2 GB by default, so that
        # address simply does not exist and the stack cannot be mapped. With
        # the flag set, a 32-bit process on 64-bit Windows gets the full 4 GB
        # and the guest can be laid out where it expects to be.
        target_link_options(${target} PRIVATE
            /DYNAMICBASE:NO /LARGEADDRESSAWARE /STACK:8388608)
    endif()
endfunction()

# Self-check: builds the runtime against stub generated headers and exercises
# the parts that do not need a game - the dispatch table and the syscall layer.
# guest_load() is covered by the first real title.
enable_testing()
add_executable(lindbergh_rt_selftest tests/selftest/selftest.c)
lindbergh_runtime(lindbergh_rt_selftest "${CMAKE_CURRENT_SOURCE_DIR}/tests/selftest")
add_test(NAME lindbergh_rt_selftest COMMAND lindbergh_rt_selftest)
