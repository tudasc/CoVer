// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic
// RUN: %CoVerStdCXX %run_stdcxx_compile
// RUN: %run_stdcxx_dynamic

#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>

int main(int argc, char** argv) {
    int rank;
    int* buf;
    int* old;
    MPI_Request req;

    MPI_Init(NULL, NULL);

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    buf = (int*)malloc(sizeof(int));
    // Keeps the block after buf in use, so realloc cannot grow in place and has to move it
    int* blocker = (int*)malloc(sizeof(int));
    old = buf;
    buf = (int*)realloc(buf, 1024 * sizeof(int));
    buf[0] = 42;
    if (rank == 0) {
        MPI_Isend(old, 1, MPI_INT, 1, 0, MPI_COMM_WORLD, &req);
        printf("Buf: %d\n", buf[0]);
    } else {
        MPI_Irecv(buf, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, &req);
    }
    MPI_Wait(&req, MPI_STATUS_IGNORE);

    free(blocker);
    free(buf);
    MPI_Finalize();
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK: Contract violation detected!
// CHECK: Buffer is not allocated
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK: Contract violation detected!
// CHECK: Buffer is not allocated
// Dont check for analysis finished, MPI implementation might crash.

// STDCXX: CoVer-StdCXX: ## Contract violation detected! ##
// STDCXX: CoVer-StdCXX: Message: Buffer is not allocated
