# Analyser library
add_subdirectory(Analysis/)

# Launcher
add_executable(CoVerDynamicLauncher Launcher.cpp)
install(TARGETS CoVerDynamicLauncher)
