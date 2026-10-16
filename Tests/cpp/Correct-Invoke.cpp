// RUN: %CoVerCXX %run_compile
// RUN: %run_dynamic

#include <mpi.h>

// Non-trivial destructor: Calls in scope of a Guard become invokes
struct Guard {
    ~Guard() {}
};

void finalize() {
    MPI_Finalize();
}

int main(int argc, char** argv) {
    Guard g;
    // Analysis must continue at the normal destination of these invokes
    MPI_Init(NULL, NULL);
    finalize();
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK-NOT: Contract violation detected!
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK-NOT: Contract violation detected!
// CHECK: Analysis finished.
