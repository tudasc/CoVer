# Analyser library
add_library(CoVerDynamicAnalyzer SHARED 
  Analysis/Analyses/PostCallAnalysis.cpp
  Analysis/Analyses/PreCallAnalysis.cpp
  Analysis/Analyses/ReleaseAnalysis.cpp
  Analysis/Hooks.cpp
  Analysis/DynamicUtils.cpp
)
set_property(TARGET CoVerDynamicAnalyzer PROPERTY POSITION_INDEPENDENT_CODE ON)
target_include_directories(CoVerDynamicAnalyzer PUBLIC ../Include/)

# Launcher
set(COVER_DYNAMIC_ANALYSER_PATH ${CMAKE_INSTALL_PREFIX}/lib/libCoVerDynamicAnalyzer.so)
configure_file("Launcher.cpp" "${CMAKE_CURRENT_BINARY_DIR}/Launcher.cpp" @ONLY)
add_executable(CoVerDynamicLauncher "${CMAKE_CURRENT_BINARY_DIR}/Launcher.cpp")
install(TARGETS CoVerDynamicLauncher)
install(TARGETS CoVerDynamicAnalyzer DESTINATION lib)
