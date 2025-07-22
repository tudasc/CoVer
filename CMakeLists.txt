cmake_minimum_required(VERSION 3.15)

project(CoVer LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

##
## Parse requirements
##

find_package(LLVM REQUIRED CONFIG)
set(LLVM_VERSION_MIN 18)
if (${LLVM_VERSION_MAJOR} LESS ${LLVM_VERSION_MIN})
  message(FATAL_ERROR "LLVM/Clang version ${LLVM_VERSION_MAJOR} is unsupported, minimum required is ${LLVM_VERSION_MIN}!")
else()
  message("Detected LLVM/Clang version ${LLVM_VERSION_MAJOR}")
endif()

find_package(antlr4-generator QUIET)
find_package(antlr4-runtime QUIET)

if (NOT antlr4-generator_FOUND)
  set(ANTLR4_BAKED_VERSION 4.13.2)
  set(ANTLR4_JAR_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/Externals/antlr-${ANTLR4_BAKED_VERSION}-complete.jar" CACHE STRING "Location of antlr4 jar file")
  set(ANTLR_BUILD_CPP_TESTS OFF)
  set(ANTLR_BUILD_SHARED OFF)
  include(FetchContent)
  set(ANTLR4_INSTALL ON)
  FetchContent_Declare(
    antlr4
    GIT_REPOSITORY https://github.com/antlr/antlr4
    GIT_TAG        "${ANTLR4_BAKED_VERSION}"
    SOURCE_SUBDIR runtime/Cpp
    QUIET
  )
  FetchContent_MakeAvailable(antlr4)
  target_compile_options(antlr4_static PUBLIC -fPIC)
  set(antlr4-generator_DIR "${antlr4_BINARY_DIR}")
  find_package(antlr4-generator REQUIRED)
  set(ANTLR4_INCLUDE_DIR "${antlr4_SOURCE_DIR}/runtime/Cpp/runtime/src")
  set(ANTLR4_LIBRARY antlr4_static CACHE STRING "ANTLR4 target to use")
else()
  set(ANTLR4_JAR_LOCATION "/usr/share/java/antlr-complete.jar" CACHE STRING "Location of antlr4 jar file")
  set(ANTLR4_LIBRARY antlr4-runtime CACHE STRING "ANTLR4 target to use")
endif(NOT antlr4-generator_FOUND)

##
## Configure main project
##

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIR})

antlr4_generate(ContractLex ${CMAKE_CURRENT_SOURCE_DIR}/Grammars/ContractLexer.g4 LEXER)
antlr4_generate(ContractPars ${CMAKE_CURRENT_SOURCE_DIR}/Grammars/ContractParser.g4 PARSER "" "VisitorArg" "" "${ANTLR4_TOKEN_FILES_ContractLex}" "${ANTLR4_TOKEN_DIRECTORY_ContractLex}")

add_library(ContractLanguage
  ${ANTLR4_SRC_FILES_ContractLex}
  ${ANTLR4_SRC_FILES_ContractPars}
  LangCode/ContractDataVisitor.cpp
  LangCode/ContractLangErrorListener.cpp
  LangCode/ContractDataExtractor.cpp
)
target_include_directories(ContractLanguage
  PUBLIC
    Include/
    ${ANTLR4_INCLUDE_DIR_ContractLex}
    ${ANTLR4_INCLUDE_DIR_ContractPars}
    ${ANTLR4_INCLUDE_DIR}
)
set_property(TARGET ContractLanguage PROPERTY POSITION_INDEPENDENT_CODE ON)
target_compile_options (ContractLanguage PUBLIC -fexceptions)

add_llvm_pass_plugin(CoVerPlugin
  Passes/Registrar.cpp
  Passes/ContractManager.cpp
  Passes/ContractVerifierPreCall.cpp
  Passes/ContractVerifierPostCall.cpp
  Passes/ContractVerifierRelease.cpp
  Passes/ContractPostProcess.cpp
  Utils/ContractPassUtility.cpp
  Include/ContractPassUtility.hpp
)

target_include_directories(CoVerPlugin
  PUBLIC
    Include/
)

target_link_libraries(ContractLanguage PRIVATE "${ANTLR4_LIBRARY}")
target_link_libraries(CoVerPlugin PRIVATE ContractLanguage)

install(TARGETS CoVerPlugin ContractLanguage LIBRARY DESTINATION lib)
install(
  FILES
    Include/ContractTree.hpp
    Include/Contracts.h
    Passes/ContractManager.hpp
  DESTINATION include
  PERMISSIONS
    OWNER_READ OWNER_WRITE
    GROUP_READ
    WORLD_READ
)

##
## Premade MPI header for convenience and quick testing
##

execute_process(COMMAND git log --pretty=format:%h -n 1 OUTPUT_VARIABLE GIT_REV)

find_package(MPI COMPONENTS C)
find_package(Python COMPONENTS Interpreter)

if (MPI_C_FOUND AND Python_FOUND)
  if (EXISTS "${MPI_C_HEADER_DIR}/mpi_proto.h")
    set(MPI_FUNC_DECLS_FILE "${MPI_C_HEADER_DIR}/mpi_proto.h") # MPICH
  else()
    set(MPI_FUNC_DECLS_FILE "${MPI_C_HEADER_DIR}/mpi.h") # OpenMPI
  endif(EXISTS "${MPI_C_HEADER_DIR}/mpi_proto.h")
  add_custom_target(
    genMPIContracts ALL
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/gen_mpi_contr_h.py"
    COMMAND ${Python_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/gen_mpi_contr_h.py" "${CMAKE_CURRENT_BINARY_DIR}" "${MPI_FUNC_DECLS_FILE}" "CoVer ${GIT_REV}"
  )
  set(PREMADE_CONTRACT_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/mpi_contracts.h")
  if (EXISTS "${MPI_C_HEADER_DIR}/shmem.h")
    add_custom_target(
      genSHMEMContracts ALL
      SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/gen_shmem_contr_h.py"
      COMMAND ${Python_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/gen_shmem_contr_h.py" "${CMAKE_CURRENT_BINARY_DIR}" "${MPI_C_HEADER_DIR}/shmem.h" "CoVer ${GIT_REV}"
    )
    list(APPEND PREMADE_CONTRACT_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/shmem_contracts.h") # Works for OpenMPI, enough for now
  endif(EXISTS "${MPI_C_HEADER_DIR}/shmem.h")
  install(
    FILES
      ${PREMADE_CONTRACT_HEADERS}
    DESTINATION include
    PERMISSIONS
      OWNER_READ OWNER_WRITE
      GROUP_READ
      WORLD_READ
  )
endif(MPI_C_FOUND AND Python_FOUND)

##
## Utility Script for convenience
##

set(CONTR_PLUGIN_PATH "${CMAKE_INSTALL_PREFIX}/lib/CoVerPlugin.so")
set(CONTR_INCLUDE_PATH "${CMAKE_INSTALL_PREFIX}/include")

# Replace build folder with install folder
string(REPLACE "${CMAKE_CURRENT_BINARY_DIR}" "${CONTR_INCLUDE_PATH}" PREMADE_CONTRACT_PATHS_EMBED "${PREMADE_CONTRACT_HEADERS}")
# Fix formatting for embed
string(REPLACE ";" "\", \"" PREMADE_CONTRACT_PATHS_EMBED "${PREMADE_CONTRACT_PATHS_EMBED}")

set(COMPILER_WRAP_TARGET "mpicc")
set(COMPILE_SRC_FILE_ENDINGS ".*\\\\.c$")
set(EXECUTABLE_WRAPPER_NAME "clangContracts")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Scripts/clangContracts.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/clangContracts.cpp" @ONLY)
add_executable(clangContracts "clangContracts.cpp")

set(COMPILER_WRAP_TARGET "mpicxx")
set(COMPILE_SRC_FILE_ENDINGS ".*(\\\\.cpp|\\\\.cc|\\\\.cxx)$")
set(EXECUTABLE_WRAPPER_NAME "clangContracts++")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Scripts/clangContracts.cpp.in" "${CMAKE_CURRENT_BINARY_DIR}/clangContracts++.cpp" @ONLY)
add_executable(clangContracts++ "clangContracts++.cpp")

install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/clangContracts"
    "${CMAKE_CURRENT_BINARY_DIR}/clangContracts++"
  DESTINATION bin
  PERMISSIONS
    OWNER_READ OWNER_WRITE OWNER_EXECUTE
    GROUP_READ GROUP_EXECUTE
    WORLD_READ WORLD_EXECUTE
)
