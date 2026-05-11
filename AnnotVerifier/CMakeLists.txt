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

set_property(TARGET CoVerAnnotVerifier PROPERTY POSITION_INDEPENDENT_CODE ON)
install(TARGETS CoVerAnnotVerifier DESTINATION lib)
