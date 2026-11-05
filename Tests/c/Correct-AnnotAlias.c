// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic

#include <stdlib.h>
#include <mpi.h>

// Requests are handed out by a pool, and looked up again later for completion.
// Matching the request of the communication call to the one of the wait needs source level alias annotations
static MPI_Request pool[4];
static int next_req = 0;

MPI_Request* acquireRequest(void) {
    return &pool[next_req++];
}

MPI_Request* lastRequest(void) {
    return &pool[next_req - 1];
}

int main(int argc, char** argv) {
    int rank;
    int* buf;

    MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    buf = malloc(sizeof(int));

    for (int round = 0; round < 2; round++) {
        // Each round uses a different pool entry: Start a new alias epoch
        CoVer_AnnotAliasEpoch(0);
        MPI_Request* req = acquireRequest();
        CoVer_AnnotAlias(req, true, 0);
        *buf = 42 + round;
        if (rank == 0) MPI_Isend(buf, 1, MPI_INT, 1, 0, MPI_COMM_WORLD, req);
        else MPI_Irecv(buf, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, req);

        MPI_Request* pending = lastRequest();
        CoVer_AnnotAlias(pending, true, 0);
        MPI_Wait(pending, MPI_STATUS_IGNORE);
    }

    MPI_Finalize();
    free(buf);
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK-NOT: Contract violation detected!
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK-NOT: Contract violation detected!
// CHECK-NOT: Annotation verification failed!
// CHECK: Analysis finished.
