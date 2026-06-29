# This software is dual-licensed under GPLv3 and a commercial
# license. See the file LICENSE.md distributed with this software for
# full license information.

# --- NIC backend selection ---
set(SOEM_NIC_BACKEND "AF_PACKET" CACHE STRING "NIC backend: AF_PACKET or AF_XDP")
set_property(CACHE SOEM_NIC_BACKEND PROPERTY STRINGS AF_PACKET AF_XDP)

target_sources(soem PRIVATE
  osal/linux/osal.c
  osal/linux/osal_defs.h
  oshw/linux/oshw.c
  oshw/linux/oshw.h
  oshw/linux/nicdrv.h
)

if(SOEM_NIC_BACKEND STREQUAL "AF_XDP")
  target_sources(soem PRIVATE
    oshw/linux/nicdrv_xdp.c
    oshw/linux/ec_xsk.c
  )
  # EC_USE_XDP must be PUBLIC, not PRIVATE: it gates the `ec_xsk_t xsk` member
  # in the *public* header nicdrv.h (struct ecx_portt). If it were PRIVATE, the
  # soem library would compile ecx_portt with the xsk member while samples /
  # external consumers (which only inherit PUBLIC/INTERFACE definitions) would
  # compile the smaller layout. Passing such a mismatched ecx_contextt across
  # the ABI boundary corrupts memory (e.g. crashes in ecx_config_init).
  target_compile_definitions(soem PUBLIC EC_USE_XDP)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBXDP REQUIRED libxdp)
  pkg_check_modules(LIBBPF REQUIRED libbpf)
  target_include_directories(soem PUBLIC ${LIBXDP_INCLUDE_DIRS} ${LIBBPF_INCLUDE_DIRS})
  target_link_libraries(soem PUBLIC ${LIBXDP_LIBRARIES} ${LIBBPF_LIBRARIES})
  include(${CMAKE_CURRENT_LIST_DIR}/BuildEbpf.cmake)
else()
  target_sources(soem PRIVATE oshw/linux/nicdrv_pkt.c)
endif()

target_include_directories(soem PUBLIC
  $<BUILD_INTERFACE:${SOEM_SOURCE_DIR}/osal/linux>
  $<BUILD_INTERFACE:${SOEM_SOURCE_DIR}/oshw/linux>
  $<INSTALL_INTERFACE:include/soem>
)

foreach(target IN ITEMS
    soem
    ec_sample
    eepromtool
    eni_test
    eoe_test
    firm_update
    simple_ng
    slaveinfo)
  if (TARGET ${target})
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
    )
  endif()
endforeach()

target_link_libraries(soem PUBLIC pthread rt)

install(FILES
  osal/linux/osal_defs.h
  oshw/linux/nicdrv.h
  DESTINATION include/soem
)
