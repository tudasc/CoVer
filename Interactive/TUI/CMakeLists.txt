# Terminal UI of the interactive mode using FTXUI

# Add FTXUI
add_subdirectory("${PROJECT_SOURCE_DIR}/Externals/FTXUI" "${PROJECT_BINARY_DIR}/Externals/FTXUI")

add_library(CoVerTUIFrontend SHARED TUIFrontend.cpp)
target_sources(CoVerTUIFrontend PRIVATE FILE_SET CXX_MODULES FILES
  TUIManager.cppm
  TUITrace.cppm
  TUIChat.cppm
  TUISettings.cppm
)
target_link_libraries(CoVerTUIFrontend PRIVATE CoVerCore ftxui::screen ftxui::dom ftxui::component)
# Next to the core library, which loads it from there (in the build tree as well)
set_target_properties(CoVerTUIFrontend PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}" INSTALL_RPATH "$ORIGIN")
install(TARGETS CoVerTUIFrontend LIBRARY DESTINATION lib)
