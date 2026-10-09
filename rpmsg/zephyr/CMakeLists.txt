# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(rpmsg-test)

target_include_directories(app PRIVATE ../include)

# arch_page_phys_get() is declared in kernel/include/kernel_arch_interface.h,
# a private kernel header with no public equivalent in 4.4.2. The ivshmem-v2
# driver maps its shared memory with k_mem_map_phys_bare(), so the virtual
# address it hands back is not the physical one Linux needs in the vring
# descriptors, and the translation cannot be avoided. In-tree callers such as
# lib/posix/options/mmap.c include the same header.
target_include_directories(app PRIVATE
	${ZEPHYR_BASE}/kernel/include
	${ZEPHYR_BASE}/arch/${ARCH}/include)

target_sources(app PRIVATE src/main.c)
