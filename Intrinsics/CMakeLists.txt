add_library(CoVerIntrinsics STATIC
  Intrinsics.c
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(TARGET CoVerIntrinsics PROPERTY UNITY_BUILD OFF)
else()
    set_property(TARGET CoVerIntrinsics PROPERTY UNITY_BUILD ON)
endif()

set_property(TARGET CoVerIntrinsics PROPERTY C_VISIBILITY_PRESET hidden)
set_property(TARGET CoVerIntrinsics PROPERTY VISIBILITY_INLINES_HIDDEN ON)

target_compile_options(CoVerIntrinsics PUBLIC -fno-rtti -fno-exceptions -fomit-frame-pointer -fno-stack-protector)

set_property(TARGET CoVerIntrinsics PROPERTY POSITION_INDEPENDENT_CODE ON)
install(TARGETS CoVerIntrinsics)
