cmake_minimum_required(VERSION 3.15)

project(LLVMContractsPlugin LANGUAGES CXX C)
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

find_package(antlr4-generator REQUIRED)
find_package(antlr4-runtime REQUIRED)

set(ANTLR4_JAR_LOCATION /usr/share/java/antlr-complete.jar)

##
## Configure main project
##

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})

antlr4_generate(ContractLex ${CMAKE_CURRENT_SOURCE_DIR}/Grammars/ContractLexer.g4 LEXER)
antlr4_generate(ContractPars ${CMAKE_CURRENT_SOURCE_DIR}/Grammars/ContractParser.g4 PARSER "" "VisitorArg" "" "${ANTLR4_TOKEN_FILES_ContractLex}" "${ANTLR4_TOKEN_DIRECTORY_ContractLex}")

add_llvm_pass_plugin(LLVMContractsPlugin
  Passes/Registrar.cpp
  Passes/ContractManager.cpp
  Passes/ContractVerifierPreCall.cpp
  Passes/ContractVerifierRelease.cpp
  Passes/ContractPostProcess.cpp
  LangCode/ContractDataVisitor.cpp
  LangCode/ContractLangErrorListener.cpp
  Include/ContractPassUtility.hpp
  ${ANTLR4_SRC_FILES_ContractLex}
  ${ANTLR4_SRC_FILES_ContractPars}
)

target_include_directories(LLVMContractsPlugin
  PUBLIC
    Include/
    ${ANTLR4_INCLUDE_DIR_ContractLex}
    ${ANTLR4_INCLUDE_DIR_ContractPars}
    ${ANTLR4_INCLUDE_DIR}
)
target_link_libraries(LLVMContractsPlugin PRIVATE antlr4-runtime)
target_compile_options (LLVMContractsPlugin PUBLIC -fexceptions)

install(TARGETS LLVMContractsPlugin LIBRARY DESTINATION lib)
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
## Utility Script for convenience
##

set(CONTR_PLUGIN_PATH "${CMAKE_INSTALL_PREFIX}/lib/LLVMContractsPlugin.so")
set(CONTR_INCLUDE_PATH "${CMAKE_INSTALL_PREFIX}/include/")

set(COMPILER_WRAP_EXPR "\${MPICC:-mpicc}")
set(COMPILE_SRC_FILE_ENDINGS "\\.c")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Scripts/clangContracts.sh.in" "${CMAKE_CURRENT_BINARY_DIR}/clangContracts.configured" @ONLY)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/clangContracts" INPUT "${CMAKE_CURRENT_BINARY_DIR}/clangContracts.configured")

set(COMPILER_WRAP_EXPR "\${MPICXX:-mpicxx}")
set(COMPILE_SRC_FILE_ENDINGS "\\.cpp|\\.cc|\\.cxx")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Scripts/clangContracts.sh.in" "${CMAKE_CURRENT_BINARY_DIR}/clangContracts++.configured" @ONLY)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/clangContracts++" INPUT "${CMAKE_CURRENT_BINARY_DIR}/clangContracts++.configured")

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

##
## Premade MPI header for convenience and quick testing
##


find_package(MPI REQUIRED)

execute_process(COMMAND git log --pretty=format:%h -n 1 OUTPUT_VARIABLE GIT_REV)
add_custom_target(
  genMPIContracts ALL
  SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/gen_mpi_contr_h.py"
  COMMAND python "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/gen_mpi_contr_h.py" "${CMAKE_CURRENT_BINARY_DIR}" "${MPI_C_HEADER_DIR}/mpi.h" "LLVMContractsPlugin ${GIT_REV}"
)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/mpi_contracts.h" DESTINATION include
  PERMISSIONS
    OWNER_READ OWNER_WRITE
    GROUP_READ
    WORLD_READ
)
