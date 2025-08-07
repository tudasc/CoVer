add_library(CoVerDynamicAnalyzer SHARED 
  Analyses/PostCallAnalysis.cpp
  Analyses/PreCallAnalysis.cpp
  Analyses/ReleaseAnalysis.cpp
  Hooks.cpp
  DynamicUtils.cpp
)

if (CMAKE_ADDR2LINE)
  target_compile_definitions(CoVerDynamicAnalyzer PUBLIC CMAKE_ADDR2LINE="${CMAKE_ADDR2LINE}")
  message("addr2line will be used to resolve file references.")
else()
  message(WARNING "addr2line not found! Required to resolve file references\nWill still compile, but error messages will be opaque.")
endif(CMAKE_ADDR2LINE)

set_property(TARGET CoVerDynamicAnalyzer PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(CoVerDynamicAnalyzer PUBLIC ../../Include/)
install(TARGETS CoVerDynamicAnalyzer DESTINATION lib)
