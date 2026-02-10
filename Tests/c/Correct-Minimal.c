// RUN: %binaries/clangContracts --predefined-contracts --instrument-contracts %s -o %t.exe > %t.test_out 2>&1 && %t.exe >> %t.test_out 2>&1 && FileCheck %s < %t.test_out

#include "mpi_contracts.h"

int main(int argc, char** argv) {
    MPI_Init(NULL, NULL);
    MPI_Finalize();
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK-NOT: Contract violation detected!
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK-NOT: Contract violation detected!
// CHECK: Analysis finished.
