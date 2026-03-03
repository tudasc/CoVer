// RUN: %clangContracts %run_common

#include "mpi_contracts.h"

int main(int argc, char** argv) {
    MPI_Init(NULL, NULL);
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK: Contract violation detected!
// CHECK: Missing Finalization call
// CHECK: CoVer: Total Tool Runtime


// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK: Contract violation detected!
// CHECK: Missing Finalization call
// Dont check for analysis finished, MPI implementation might crash.
