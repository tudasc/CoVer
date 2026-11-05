# Graphical UI of the interactive mode using Qt6

find_package(Qt6 REQUIRED COMPONENTS Widgets)

add_library(CoVerQtFrontend SHARED GUIFrontend.cpp)
target_sources(CoVerQtFrontend PRIVATE FILE_SET CXX_MODULES FILES
  GUIManager.cppm
  GUITrace.cppm
  GUIChat.cppm
  GUISettings.cppm
)
target_link_libraries(CoVerQtFrontend PRIVATE CoVerCore Qt6::Widgets)
target_compile_definitions(CoVerQtFrontend PRIVATE QT_NO_KEYWORDS)

# Next to the core library, which loads it from there (in the build tree as well)
set_target_properties(CoVerQtFrontend PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}" INSTALL_RPATH "$ORIGIN")
install(TARGETS CoVerQtFrontend LIBRARY DESTINATION lib)
