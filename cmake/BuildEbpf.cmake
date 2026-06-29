# This software is dual-licensed under GPLv3 and a commercial
# license. See the file LICENSE.md distributed with this software for
# full license information.

# Compile the XDP/eBPF kernel program to BPF bytecode with clang.
find_program(CLANG_EXECUTABLE clang REQUIRED)

set(EC_XDP_KERN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/oshw/linux/ec_xdp_kern.c)
set(EC_XDP_KERN_OBJ ${CMAKE_BINARY_DIR}/ec_xdp_kern.o)

add_custom_command(
  OUTPUT  ${EC_XDP_KERN_OBJ}
  COMMAND ${CLANG_EXECUTABLE} -O2 -g -Wall -target bpf
          # -D__aarch64__ is required when building on/for an OE-based aarch64
          # rootfs: clang -target bpf does not define __aarch64__, which causes
          # glibc's bits/wordsize.h to misjudge __WORDSIZE=32 and pull the
          # non-existent asm/types-32.h. Defining it explicitly fixes the path.
          #
          # -D__TARGET_ARCH_arm64 is the BPF-world convention for telling
          # libbpf BPF headers (bpf_tracing.h etc.) which target ISA to use;
          # it is arch-independent at the bytecode level but needed by those
          # headers' arch-dispatch macros.
          -D__aarch64__ -D__TARGET_ARCH_arm64
          -c ${EC_XDP_KERN_SRC} -o ${EC_XDP_KERN_OBJ}
  DEPENDS ${EC_XDP_KERN_SRC}
  COMMENT "Building eBPF object ec_xdp_kern.o"
)
add_custom_target(ec_xdp_kern ALL DEPENDS ${EC_XDP_KERN_OBJ})
add_dependencies(soem ec_xdp_kern)
