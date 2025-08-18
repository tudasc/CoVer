set(DYNAMIC_ANALYSER_SOURCES
  Analyses/PostCallAnalysis.cpp
  Analyses/PreCallAnalysis.cpp
  Analyses/ReleaseAnalysis.cpp
  Hooks.cpp
  DynamicUtils.cpp
)

add_library(CoVerDynamicAnalyzer STATIC ${DYNAMIC_ANALYSER_SOURCES})
set_property(TARGET CoVerDynamicAnalyzer PROPERTY CXX_STANDARD 23)
set_property(TARGET CoVerDynamicAnalyzer PROPERTY UNITY_BUILD ON)
target_include_directories(CoVerDynamicAnalyzer PUBLIC ../../Include/)
target_compile_options(CoVerDynamicAnalyzer PUBLIC -fno-rtti -fno-exceptions)

if (CMAKE_ADDR2LINE)
  target_compile_definitions(CoVerDynamicAnalyzer PUBLIC CMAKE_ADDR2LINE="${CMAKE_ADDR2LINE}")
  message("addr2line will be used to resolve file references.")
else()
  message(WARNING "addr2line not found! Required to resolve file references\nWill still compile, but error messages will be opaque.")
endif(CMAKE_ADDR2LINE)

set_property(TARGET CoVerDynamicAnalyzer PROPERTY POSITION_INDEPENDENT_CODE ON)
install(TARGETS CoVerDynamicAnalyzer DESTINATION lib)
