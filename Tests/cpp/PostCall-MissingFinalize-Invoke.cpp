// RUN: %CoVerCXX %run_compile
// RUN: %run_dynamic

#include <mpi.h>

// Non-trivial destructor: Calls in scope of a Guard become invokes
struct Guard {
    ~Guard() {}
};

void barrier() {
    MPI_Barrier(MPI_COMM_WORLD);
}

int main(int argc, char** argv) {
    Guard g;
    // Analysis must continue at the normal destination of these invokes to find the missing finalize
    MPI_Init(NULL, NULL);
    barrier();
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
