// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic

#include <stdlib.h>
#include <mpi.h>

void fill(int* buf) {
    buf[0] = 24;
}

int main(int argc, char** argv) {
    int rank;
    int* buf = (int*)malloc(sizeof(int));
    MPI_Request req;

    MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    buf[0] = 42;
    MPI_Isend(buf, 1, MPI_INT, rank ^ 1, 0, MPI_COMM_WORLD, &req);
    if (rank == 1) {
        // Correct: fill is first analysed after the request completed
        MPI_Wait(&req, MPI_STATUS_IGNORE);
        fill(buf);
    } else {
        // Race: fill must be analysed again with the request pending
        fill(buf);
        MPI_Wait(&req, MPI_STATUS_IGNORE);
    }
    MPI_Recv(buf, 1, MPI_INT, rank ^ 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    MPI_Finalize();
    free(buf);
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK: Contract violation detected!
// CHECK: Local Data Race - Local write
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK: Contract violation detected!
// CHECK: Local Data Race - Local write
// Dont check for analysis finished, MPI implementation might crash.
