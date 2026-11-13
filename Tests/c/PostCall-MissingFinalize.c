// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic
// RUN: %CoVerStdCXX %run_stdcxx_compile
// RUN: %run_stdcxx_dynamic

#include <mpi.h>

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

// STDCXX: CoVer-StdCXX: ## Contract violation detected! ##
// STDCXX: CoVer-StdCXX: Message: Missing Finalization call
