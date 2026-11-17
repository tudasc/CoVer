// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic

#include <stdio.h>
#include <stdlib.h>
#include <mpi.h>

// MPI_Isend takes a const buffer, MPI_Irecv a non-const one, so one of them has to be cast
typedef int (*p2p_t)(void*, int, MPI_Datatype, int, int, MPI_Comm, MPI_Request*);

int main(int argc, char** argv) {
    int rank;
    int* buf;
    MPI_Request req;
    p2p_t p2p;

    MPI_Init(NULL, NULL);

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    buf = (int*)malloc(sizeof(int));
    buf[0] = 42;
    if (rank == 0) {
        p2p = (p2p_t)MPI_Isend;
    } else {
        p2p = MPI_Irecv;
    }
    p2p(buf, 1, MPI_INT, 1 - rank, 0, MPI_COMM_WORLD, &req);
    MPI_Wait(&req, MPI_STATUS_IGNORE);

    MPI_Finalize();
    return 0;
}

// The static analysis cannot resolve the call
// CHECK-LABEL: Running Contract Manager on Module
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK-NOT: Contract violation detected!
// CHECK: Analysis finished.
