add_executable(simple_ng simple_ng.c)
target_link_libraries(simple_ng soem)
if(UNIX)
  # sqrt() from <math.h> needs the math library on Unix-like systems
  target_link_libraries(simple_ng m)
endif()
install(TARGETS simple_ng DESTINATION bin)
