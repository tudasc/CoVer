// RUN: %binaries/clangContracts --predefined-contracts --instrument-contracts %s -o %t.exe > %t.test_out 2>&1 && COVER_COVERAGE_FOLDER="%t_coverage" %t.exe >> %t.test_out 2>&1 && FileCheck %s < %t.test_out

#include "mpi_contracts.h"

int main(int argc, char** argv) {
    MPI_Init(NULL, NULL);
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK: Contract violation detected!
// CHECK: CoVer: Total Tool Runtime


// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK: Contract violation detected!
// CHECK: Missing Finalization call
// Dont check for analysis finished, MPI implementation might crash.
