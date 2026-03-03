// RUN: %clangContracts %run_common

#include "mpi_contracts.h"

int main(int argc, char** argv) {
    MPI_Finalize();
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK: Contract violation detected!
// CHECK: Missing Initialization call
// CHECK: CoVer: Total Tool Runtime


// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK: Contract violation detected!
// CHECK: Missing Initialization call
// Dont check for analysis finished, MPI implementation might crash.
