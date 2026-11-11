// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic
// RUN: ls %t_coverage | FileCheck %s --check-prefix=FILES --allow-empty

#include <stdlib.h>
#include <mpi.h>

void sendTo(int* buf, int peer, MPI_Request* req) {
    MPI_Isend(buf, 1, MPI_INT, peer, 0, MPI_COMM_WORLD, req);
}

void recvFrom(int* buf, int peer, MPI_Request* req) {
    MPI_Irecv(buf, 1, MPI_INT, peer, 0, MPI_COMM_WORLD, req);
}

void neverCalled(int* buf, int peer, MPI_Request* req) {
    MPI_Isend(buf, 1, MPI_INT, peer, 0, MPI_COMM_WORLD, req);
}

// Each target of the first annotation is only called by one rank: The coverage of both ranks has to be merged.
// The third target is never called, and the second annotation is never visited
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
    CoVer_AnnotFP(comm, 3, sendTo, recvFrom, neverCalled);
    comm(buf, 1 - rank, &req);
    MPI_Wait(&req, MPI_STATUS_IGNORE);

    if (argc > 100) {
        CoVer_AnnotFP(comm, 2, sendTo, recvFrom);
        comm(buf, 1 - rank, &req);
        MPI_Wait(&req, MPI_STATUS_IGNORE);
    }

    MPI_Finalize();
    free(buf);
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK-NOT: Contract violation detected!
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK-NOT: Annotation verification failed!
// CHECK: CoVer-AnnotVerifier: Info: Checked annotation coverage of 2 processes
// CHECK-NOT: never called its target sendTo
// CHECK-NOT: never called its target recvFrom
// CHECK-DAG: Warning: Function pointer annotation at {{.*}}Annot-FPCoverage.c:35 never called its target neverCalled!
// CHECK-DAG: Warning: Function pointer annotation at {{.*}}Annot-FPCoverage.c:40 was never visited!
// CHECK-NOT: CoVer-AnnotVerifier: Info: Checked annotation coverage

// The reporting process deletes the coverage files
// FILES-NOT: CoVerAnnot_
