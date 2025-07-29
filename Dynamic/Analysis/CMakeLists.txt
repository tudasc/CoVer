add_library(CoVerDynamicAnalyzer SHARED 
  Analyses/PostCallAnalysis.cpp
  Analyses/PreCallAnalysis.cpp
  Analyses/ReleaseAnalysis.cpp
  Hooks.cpp
  DynamicUtils.cpp
)
set_property(TARGET CoVerDynamicAnalyzer PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(CoVerDynamicAnalyzer PUBLIC ../../Include/)
install(TARGETS CoVerDynamicAnalyzer DESTINATION lib)
