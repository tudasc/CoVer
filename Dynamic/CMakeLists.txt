add_library(CoVerDynamicAnalyzer STATIC
  Analyses/PostCallAnalysis.cpp
  Analyses/PreCallAnalysis.cpp
  Analyses/ReleaseAnalysis.cpp
  Hooks.cpp
  DynamicUtils.cpp
)

set_property(TARGET CoVerDynamicAnalyzer PROPERTY CXX_STANDARD 20)
set_property(TARGET CoVerDynamicAnalyzer PROPERTY UNITY_BUILD ON)
target_include_directories(CoVerDynamicAnalyzer PUBLIC ../Include/)

set_property(TARGET CoVerDynamicAnalyzer PROPERTY CXX_VISIBILITY_PRESET hidden)
set_property(TARGET CoVerDynamicAnalyzer PROPERTY C_VISIBILITY_PRESET hidden)
set_property(TARGET CoVerDynamicAnalyzer PROPERTY VISIBILITY_INLINES_HIDDEN ON)

target_compile_options(CoVerDynamicAnalyzer PUBLIC -fno-rtti -fno-exceptions -fomit-frame-pointer -fno-stack-protector)

if (CMAKE_ADDR2LINE)
  target_compile_definitions(CoVerDynamicAnalyzer PUBLIC CMAKE_ADDR2LINE="${CMAKE_ADDR2LINE}")
  message("addr2line will be used to resolve file references.")
else()
  message(WARNING "addr2line not found! Required to resolve file references\nWill still compile, but error messages will be opaque.")
endif(CMAKE_ADDR2LINE)

set_property(TARGET CoVerDynamicAnalyzer PROPERTY POSITION_INDEPENDENT_CODE ON)
install(TARGETS CoVerDynamicAnalyzer DESTINATION lib)
