cmake_minimum_required(VERSION 3.15)

project(LLVMContractsPlugin LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 17)
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
  LangCode/ContractLangErrorListener.cpp
  ${ANTLR4_SRC_FILES_ContractLex}
  ${ANTLR4_SRC_FILES_ContractPars}
)

target_include_directories(LLVMContractsPlugin PUBLIC
    ${ANTLR4_INCLUDE_DIR_ContractLex}
    ${ANTLR4_INCLUDE_DIR_ContractPars}
    ${ANTLR4_INCLUDE_DIR})
target_link_libraries(LLVMContractsPlugin PRIVATE antlr4-runtime)
target_compile_options (LLVMContractsPlugin PUBLIC -fexceptions)