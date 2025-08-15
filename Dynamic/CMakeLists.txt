# Analyser library
add_subdirectory(Analysis/)

# Launcher
set(COVER_DYNAMIC_ANALYSER_PATH ${CMAKE_INSTALL_PREFIX}/lib/libCoVerDynamicAnalyzer.so)
configure_file("Launcher.cpp" "${CMAKE_CURRENT_BINARY_DIR}/Launcher.cpp" @ONLY)
add_executable(CoVerDynamicLauncher "${CMAKE_CURRENT_BINARY_DIR}/Launcher.cpp")
install(TARGETS CoVerDynamicLauncher)
