add_library(CoVerDynamicAnalyzer STATIC
  Analyses/ParamAnalysis.cpp
  Analyses/AllocAnalysis.cpp
  Analyses/PostCallAnalysis.cpp
  Analyses/PreCallAnalysis.cpp
  Analyses/ReleaseAnalysis.cpp
  Hooks.cpp
  DynamicUtils.cpp
)

set_property(TARGET CoVerDynamicAnalyzer PROPERTY CXX_STANDARD 20)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(TARGET CoVerDynamicAnalyzer PROPERTY UNITY_BUILD OFF)
else()
    set_property(TARGET CoVerDynamicAnalyzer PROPERTY UNITY_BUILD ON)
endif()
target_include_directories(CoVerDynamicAnalyzer PUBLIC ../Include/)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFI REQUIRED libffi)
target_include_directories(CoVerDynamicAnalyzer PRIVATE "${FFI_INCLUDE_DIRS}")

set_property(TARGET CoVerDynamicAnalyzer PROPERTY CXX_VISIBILITY_PRESET hidden)
set_property(TARGET CoVerDynamicAnalyzer PROPERTY C_VISIBILITY_PRESET hidden)
set_property(TARGET CoVerDynamicAnalyzer PROPERTY VISIBILITY_INLINES_HIDDEN ON)

target_compile_options(CoVerDynamicAnalyzer PUBLIC -fno-rtti -fno-exceptions -fomit-frame-pointer -fno-stack-protector)
target_link_libraries(CoVerDynamicAnalyzer libffi)
target_include_directories(CoVerDynamicAnalyzer PRIVATE "${FFI_LIBRARIES}")

if (CMAKE_ADDR2LINE)
  target_compile_definitions(CoVerDynamicAnalyzer PUBLIC CMAKE_ADDR2LINE="${CMAKE_ADDR2LINE}")
  message("addr2line will be used to resolve file references.")
else()
  message(WARNING "addr2line not found! Required to resolve file references\nWill still compile, but error messages will be opaque.")
endif(CMAKE_ADDR2LINE)

set_property(TARGET CoVerDynamicAnalyzer PROPERTY POSITION_INDEPENDENT_CODE ON)
install(TARGETS CoVerDynamicAnalyzer DESTINATION lib)
