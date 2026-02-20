// RUN: %binaries/clangContracts %run_common

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
