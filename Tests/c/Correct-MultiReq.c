// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic
// RUN: %CoVerStdCXX %run_stdcxx_compile
// RUN: %run_stdcxx_dynamic

#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>

int main(int argc, char** argv) {
    int rank;
    int* a;
    int* b;
    MPI_Request req1, req2;

    MPI_Init(NULL, NULL);

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    a = (int*)malloc(sizeof(int));
    b = (int*)malloc(sizeof(int));
    *a = 1;
    *b = 2;
    if (rank == 0) {
        MPI_Isend(a, 1, MPI_INT, 1, 0, MPI_COMM_WORLD, &req1);
        MPI_Isend(b, 1, MPI_INT, 1, 1, MPI_COMM_WORLD, &req2);
        MPI_Wait(&req2, MPI_STATUS_IGNORE);
        *b = 24; /* req2 completed, req1 is unrelated */
        MPI_Wait(&req1, MPI_STATUS_IGNORE);
    } else {
        MPI_Recv(a, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Recv(b, 1, MPI_INT, 0, 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }

    MPI_Finalize();
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK-NOT: Contract violation detected!
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK-NOT: Contract violation detected!
// CHECK: Analysis finished.

// STDCXX: CoVer contract(s) encountered
// STDCXX-NOT: CoVer-StdCXX: ## Contract violation detected! ##
