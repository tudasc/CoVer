// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic

#include <stdlib.h>
#include <mpi.h>

int peer;

// Called both while the request is pending and after it completed
int getPeer() {
    return peer;
}

void exchange(int* buf, MPI_Request* req) {
    // Reached with the request completed (or not yet created)
    MPI_Isend(buf, 1, MPI_INT, getPeer(), 0, MPI_COMM_WORLD, req);
    // Reached with the request pending. This state must not reach the Isend of the next exchange through getPeer
    if (getPeer() < 0) abort();
    MPI_Wait(req, MPI_STATUS_IGNORE);
}

int main(int argc, char** argv) {
    int rank;
    int* buf = (int*)malloc(sizeof(int));
    MPI_Request req;

    MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    peer = rank ^ 1;
    *buf = 42;
    exchange(buf, &req);
    exchange(buf, &req);
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
