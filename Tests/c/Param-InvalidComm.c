// RUN: %clangContracts %run_common

#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>

int main(int argc, char** argv) {
    int rank;
    int* buf;
    MPI_Request req;

    MPI_Init(NULL, NULL);

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    buf = (int*)malloc(sizeof(int));
    buf[0] = 42;
    if (rank == 0) {
        MPI_Isend(buf, 1, MPI_INT, 1, 0, MPI_COMM_NULL, &req);
        printf("Buf: %d\n", buf[0]);
    } else {
        MPI_Irecv(buf, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, &req);
    }
    MPI_Wait(&req, MPI_STATUS_IGNORE);

    MPI_Finalize();
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK: Contract violation detected!
// CHECK: Communicator is invalid
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK: Contract violation detected!
// CHECK: Communicator is invalid
// Dont check for analysis finished, MPI implementation might crash.
