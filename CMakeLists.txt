cmake_minimum_required(VERSION 3.28)

project(CoVer LANGUAGES CXX C)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

##
## Parse requirements
##

find_package(LLVM REQUIRED CONFIG)
set(LLVM_VERSION_MIN 21)
if (${LLVM_VERSION_MAJOR} LESS ${LLVM_VERSION_MIN})
  message(FATAL_ERROR "LLVM/Clang version ${LLVM_VERSION_MAJOR} is unsupported, minimum required is ${LLVM_VERSION_MIN}!")
else()
  message("Detected LLVM/Clang version ${LLVM_VERSION_MAJOR}")
endif()

add_subdirectory(Externals/CoVerLang)
add_subdirectory(Externals/FTXUI)
add_subdirectory(Externals/llvm-dsa)

##
## Configure static analysis project
##

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIR})

add_llvm_pass_plugin(CoVerPlugin
  Passes/Registrar.cpp
  Passes/ContractManager.cpp
  Passes/ContractVerifierPreCall.cpp
  Passes/ContractVerifierPostCall.cpp
  Passes/ContractVerifierRelease.cpp
  Passes/ContractVerifierParam.cpp
  Passes/ContractVerifierAlloc.cpp
  Passes/ContractPostProcess.cpp
  Passes/Intrinsics.cpp
  Passes/Instrument.cpp
)

target_sources(CoVerPlugin PRIVATE FILE_SET cover_cxx_modules TYPE CXX_MODULES FILES
  Passes/AnalysisCleanup.cppm
  Passes/BasicTypes.cppm
  Utils/ContractPassUtility.cppm
  Utils/TUITrace.cppm
  Utils/TUIManager.cppm
  Utils/LLVMModule.cppm
  Utils/ErrorMessage.cppm
)

target_include_directories(CoVerPlugin
  PUBLIC
    Include/
    Externals/llvm-dsa/include
)

target_compile_options (CoVerPlugin PUBLIC -fexceptions)

##
## Runtime Dependencies
##

target_link_libraries(CoVerPlugin PRIVATE CoVerLang ftxui::screen ftxui::dom ftxui::component)

# Attempt finding system version of JsonCpp
find_package(PkgConfig)
if (PKGCONFIG_FOUND AND (NOT BUNDLED_JSONCPP))
    pkg_check_modules(JSONCPP jsoncpp)
    if (JSONCPP_FOUND)
        include_directories(${JSONCPP_INCLUDE_DIRS})
        target_link_libraries(CoVerPlugin PUBLIC ${JSONCPP_LDFLAGS})
    endif(JSONCPP_FOUND)
endif(PKGCONFIG_FOUND AND (NOT BUNDLED_JSONCPP))

# No system version: Enable bundled
if((NOT JSONCPP_FOUND) OR BUNDLED_JSONCPP)
    set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
    add_subdirectory(Externals/jsoncpp)
    set_property(TARGET jsoncpp_static PROPERTY POSITION_INDEPENDENT_CODE ON)
    include_directories(Externals/jsoncpp/include)
    target_link_libraries(CoVerPlugin PRIVATE jsoncpp_static)
endif((NOT JSONCPP_FOUND) OR BUNDLED_JSONCPP)

install(
  FILES
    Include/Contracts.h
    Include/DynamicAnalysis.h
    Include/Contracts.F90
    Passes/ContractManager.hpp
  DESTINATION include
  PERMISSIONS
    OWNER_READ OWNER_WRITE
    GROUP_READ
    WORLD_READ
)

##
## CoVer Intrinsics definitions
##

add_subdirectory(Intrinsics)

##
## Dynamic project
##

add_subdirectory(Dynamic)

##
## Dynamic verification of annotations
##

add_subdirectory(AnnotVerifier)

##
## Premade MPI header for convenience and quick testing
##

execute_process(COMMAND git log --pretty=format:%h -n 1 OUTPUT_VARIABLE GIT_REV WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

find_package(MPI COMPONENTS C)
find_package(Python COMPONENTS Interpreter)

if (MPI_C_FOUND AND Python_FOUND)
  if (EXISTS "${MPI_C_HEADER_DIR}/mpi_proto.h")
    set(MPI_FUNC_DECLS_FILE "${MPI_C_HEADER_DIR}/mpi_proto.h") # MPICH
  else()
    set(MPI_FUNC_DECLS_FILE "${MPI_C_HEADER_DIR}/mpi.h") # OpenMPI
  endif(EXISTS "${MPI_C_HEADER_DIR}/mpi_proto.h")
  message("Found MPI header with function definitions in ${MPI_FUNC_DECLS_FILE}, example MPI contracts will be generated.")
  file(DOWNLOAD "https://github.com/mpi-forum/pympistandard/raw/refs/heads/develop/src/pympistandard/data/apis.json" "${CMAKE_CURRENT_BINARY_DIR}/apis.json")
  add_custom_target(
    genMPIContracts ALL
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/gen_mpi_contr_h.py"
    COMMAND ${Python_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/gen_mpi_contr_h.py" "${CMAKE_CURRENT_BINARY_DIR}" "${MPI_FUNC_DECLS_FILE}" "CoVer ${GIT_REV}"
  )
  set(PREMADE_CONTRACT_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/mpi_contracts.h;${CMAKE_CURRENT_BINARY_DIR}/mpi_contracts.f90;${CMAKE_CURRENT_BINARY_DIR}/mpi_contracts_f08.f90;${CMAKE_CURRENT_BINARY_DIR}/mpi_contracts_f08ts.f90")
  if (EXISTS "${MPI_C_HEADER_DIR}/shmem.h" OR DEFINED SHMEM_HEADER_PATH)
    if (DEFINED SHMEM_HEADER_PATH)
      message("Using manually specified OpenSHMEM header ${SHMEM_HEADER_PATH}, example OpenSHMEM contracts will be generated.")
    else()
      set(SHMEM_HEADER_PATH "${MPI_C_HEADER_DIR}/shmem.h")
      message("Found OpenSHMEM header in ${SHMEM_HEADER_PATH}, example OpenSHMEM contracts will be generated.")
    endif(DEFINED SHMEM_HEADER_PATH)
    add_custom_target(
      genSHMEMContracts ALL
      SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/gen_shmem_contr_h.py"
      COMMAND ${Python_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/Scripts/gen_shmem_contr_h.py" "${CMAKE_CURRENT_BINARY_DIR}" "${SHMEM_HEADER_PATH}" "CoVer ${GIT_REV}"
    )
    list(APPEND PREMADE_CONTRACT_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/shmem_contracts.h") # Works for OpenMPI, enough for now
  endif(EXISTS "${MPI_C_HEADER_DIR}/shmem.h" OR DEFINED SHMEM_HEADER_PATH)
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
set(COVER_DYNAMIC_ANALYSER_PATH ${CMAKE_INSTALL_PREFIX}/lib/libCoVerDynamicAnalyzer.a)
set(COVER_ANNOT_VERIFIER_PATH ${CMAKE_INSTALL_PREFIX}/lib/libCoVerAnnotVerifier.a)
set(COVER_INTRINSICS_LIB_PATH ${CMAKE_INSTALL_PREFIX}/lib/libCoVerIntrinsics.a)
set(DSA_PLUGIN_PATH "${CMAKE_INSTALL_PREFIX}/lib/DSA.so")
set(CONTR_INCLUDE_PATH "${CMAKE_INSTALL_PREFIX}/include")

function(create_compile_wrapper WRAPPER_NAME TARGET_NAME FILE_ENDINGS PREMADE_CONTRACT_INCLUDES PREMADE_CONTRACT_SOURCES PASSLIST_PREFIX)
  list(TRANSFORM PREMADE_CONTRACT_INCLUDES PREPEND "${CONTR_INCLUDE_PATH}/" OUTPUT_VARIABLE PREMADE_CONTRACT_INCLUDES)
  list(TRANSFORM PREMADE_CONTRACT_SOURCES PREPEND "${CONTR_INCLUDE_PATH}/" OUTPUT_VARIABLE PREMADE_CONTRACT_SOURCES)
  string(REPLACE ";" "\", \"" PREMADE_CONTRACT_INCLUDES "${PREMADE_CONTRACT_INCLUDES}")
  string(REPLACE ";" "\", \"" PREMADE_CONTRACT_SOURCES "${PREMADE_CONTRACT_SOURCES}")
  set(COMPILER_WRAP_TARGET "${TARGET_NAME}")
  set(COMPILE_SRC_FILE_ENDINGS "${FILE_ENDINGS}")
  set(EXECUTABLE_WRAPPER_NAME "${WRAPPER_NAME}")
  set(OPT_PASSLIST_PREFIX "${PASSLIST_PREFIX}")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Scripts/CoVerWrapper.cpp" "${CMAKE_CURRENT_BINARY_DIR}/${WRAPPER_NAME}.cpp" @ONLY)
  add_executable(${WRAPPER_NAME} "${WRAPPER_NAME}.cpp")
  target_link_libraries(${WRAPPER_NAME} LLVM)
  install(TARGETS ${WRAPPER_NAME})
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Utils/bash-autocomplete.sh" "${CMAKE_CURRENT_BINARY_DIR}/bash-completion/completions/${WRAPPER_NAME}" @ONLY)
  install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bash-completion/" DESTINATION share/bash-completion)
endfunction()

if (MPI_C_FOUND)
  set(C_HEADER_FILES mpi_contracts.h)
  if (EXISTS "${MPI_C_HEADER_DIR}/shmem.h")
    set(C_HEADER_FILES mpi_contracts.h;shmem_contracts.h)
  endif(EXISTS "${MPI_C_HEADER_DIR}/shmem.h")
endif (MPI_C_FOUND)

create_compile_wrapper("CoVerCC" "mpicc" ".*\\\\.c$" "${C_HEADER_FILES}" "" "")
create_compile_wrapper("CoVerCXX" "mpicxx" ".*(\\\\.cpp|\\\\.cc|\\\\.cxx)$" "${C_HEADER_FILES}" "" "")
create_compile_wrapper("CoVerFC" "mpifort" ".*(\\\\.f90|\\\\.F90)$" "" "Contracts.F90;mpi_contracts.f90;mpi_contracts_f08ts.f90" "function(sroa),")
create_compile_wrapper("CoVerFC-no-ts" "mpifort" ".*(\\\\.f90|\\\\.F90)$" "" "Contracts.F90;mpi_contracts.f90;mpi_contracts_f08.f90" "function(sroa),")

##
## Testing
##

option(ENABLE_TESTS "Enable testing" OFF)
if (ENABLE_TESTS)
  enable_testing()
  add_subdirectory(Tests/)
endif(ENABLE_TESTS)
