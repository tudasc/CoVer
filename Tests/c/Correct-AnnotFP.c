// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic

#include <stdlib.h>
#include <mpi.h>

// Buffer and request are created by the caller, and used through an indirect call.
// Matching them needs the call chain through the annotated function pointer
void sendTo(int* buf, int peer, MPI_Request* req) {
    MPI_Isend(buf, 1, MPI_INT, peer, 0, MPI_COMM_WORLD, req);
}

void recvFrom(int* buf, int peer, MPI_Request* req) {
    MPI_Irecv(buf, 1, MPI_INT, peer, 0, MPI_COMM_WORLD, req);
}

int main(int argc, char** argv) {
    int rank;
    int* buf;
    MPI_Request req;
    void (*comm)(int*, int, MPI_Request*);

    MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    buf = malloc(sizeof(int));
    *buf = 42;

    if (rank == 0) comm = sendTo;
    else comm = recvFrom;
    CoVer_AnnotFP(comm, 2, sendTo, recvFrom);
    comm(buf, 1 - rank, &req);
    MPI_Wait(&req, MPI_STATUS_IGNORE);

    MPI_Finalize();
    free(buf);
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK-NOT: Contract violation detected!
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK-NOT: Contract violation detected!
// CHECK: Analysis finished.
