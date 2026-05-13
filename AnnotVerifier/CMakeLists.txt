add_library(CoVerAnnotVerifier STATIC
    AnnotVerifier.cpp
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(TARGET CoVerAnnotVerifier PROPERTY UNITY_BUILD OFF)
else()
    set_property(TARGET CoVerAnnotVerifier PROPERTY UNITY_BUILD ON)
endif()
target_include_directories(CoVerAnnotVerifier PUBLIC ../Include/)

set_property(TARGET CoVerAnnotVerifier PROPERTY CXX_VISIBILITY_PRESET hidden)
set_property(TARGET CoVerAnnotVerifier PROPERTY C_VISIBILITY_PRESET hidden)
set_property(TARGET CoVerAnnotVerifier PROPERTY VISIBILITY_INLINES_HIDDEN ON)

target_compile_options(CoVerAnnotVerifier PUBLIC -fno-rtti -fno-exceptions -fomit-frame-pointer -fno-stack-protector -fno-math-errno -fno-trapping-math)

if (CMAKE_ADDR2LINE)
  target_compile_definitions(CoVerAnnotVerifier PUBLIC CMAKE_ADDR2LINE="${CMAKE_ADDR2LINE}")
  message("addr2line will be used to resolve file references.")
else()
  message(WARNING "addr2line not found! Required to resolve file references\nWill still compile, but error messages will be opaque.")
endif(CMAKE_ADDR2LINE)

set_property(TARGET CoVerAnnotVerifier PROPERTY POSITION_INDEPENDENT_CODE ON)
install(TARGETS CoVerAnnotVerifier DESTINATION lib)
