cmake_minimum_required(VERSION 3.28)

project(CoVerStdCXXConverter LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIR})

##
## Frontend
##

add_llvm_library(CoVerStdCXXFrontend NO_EXPORT PLUGIN_TOOL MODULE clang
  Frontend/Main.cpp
)
target_sources(CoVerStdCXXFrontend PRIVATE FILE_SET cover_front_cxx_modules TYPE CXX_MODULES FILES
  Frontend/ContractCollector.cppm
  Frontend/ContractConverter.cppm
  Frontend/ContractValueCollector.cppm
  Include/ContractInfo.cppm
  Include/FunctionMods.cppm
)

target_link_libraries(CoVerStdCXXFrontend PRIVATE CoVerLang)

install(TARGETS CoVerStdCXXFrontend
  LIBRARY DESTINATION lib/
)

##
## Backend
##

find_program(COVER_PLUGIN_HOST_GCC
  NAMES g++ gcc
  DOC "GCC whose plugin headers the backend is built against"
)

if(COVER_PLUGIN_HOST_GCC)
  execute_process(COMMAND ${COVER_PLUGIN_HOST_GCC} -print-file-name=plugin
    OUTPUT_VARIABLE GCC_PLUGIN_PREFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
endif()

find_path(GCCPlugin_INCLUDE_DIR gcc-plugin.h
  HINTS "${GCC_PLUGIN_PREFIX}/include"
  DOC "Directory holding the GCC plugin development headers"
)

if(GCCPlugin_INCLUDE_DIR)
  add_library(CoVerStdCXXBackend MODULE
    Backend/Main.cpp
    Backend/ReplaceCalls.cpp
    Backend/DiscardCoVerAnnots.cpp
  )

  target_include_directories(CoVerStdCXXBackend SYSTEM PRIVATE
    ${GCCPlugin_INCLUDE_DIR}
  )

  target_compile_options(CoVerStdCXXBackend PRIVATE -fno-rtti)

  # Remove lib prefix
  set_target_properties(CoVerStdCXXBackend PROPERTIES PREFIX "")

  install(TARGETS CoVerStdCXXBackend
    LIBRARY DESTINATION lib/
  )
else()
  message(FATAL_ERROR "GCC plugin headers not found, cannot build backend!")
endif()

##
## Wrapper
##

set(COVER_STDCXX_FRONTEND_PATH "${CMAKE_INSTALL_PREFIX}/lib/CoVerStdCXXFrontend.so")
set(COVER_STDCXX_BACKEND_PATH "${CMAKE_INSTALL_PREFIX}/lib/CoVerStdCXXBackend.so")
set(EXECUTABLE_WRAPPER_NAME CoVerStdCXX)
configure_file(Scripts/CoVerStdCXXWrapper.cpp ${CMAKE_CURRENT_BINARY_DIR}/CoVerStdCXXWrapper_configured.cpp @ONLY)
add_executable(${EXECUTABLE_WRAPPER_NAME} ${CMAKE_CURRENT_BINARY_DIR}/CoVerStdCXXWrapper_configured.cpp)
target_link_libraries(${EXECUTABLE_WRAPPER_NAME} LLVM)
install(TARGETS CoVerStdCXX)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Scripts/bash-autocomplete.sh" "${CMAKE_CURRENT_BINARY_DIR}/bash-completion/completions/${EXECUTABLE_WRAPPER_NAME}" @ONLY)
install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bash-completion/" DESTINATION share/bash-completion)
