// RUN: %clangContracts %run_common

#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>

int main(int argc, char** argv) {
    int rank;
    int* buf;
    MPI_Win win;

    MPI_Init(NULL, NULL);

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    MPI_Win_allocate(sizeof(int), sizeof(int), MPI_INFO_NULL, MPI_COMM_WORLD, &buf, &win);
    buf[0] = 42;

    MPI_Win_fence(0, win);
    if (rank == 0) {
        MPI_Put(buf, 1, MPI_INT, 1, sizeof(int), 1, MPI_INT, win);
        printf("Buf: %d\n", buf[0]);
    }
    MPI_Win_fence(0, win);

    MPI_Finalize();
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK-NOT: Contract violation detected!
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK-NOT: Contract violation detected!
// CHECK: Analysis finished.
