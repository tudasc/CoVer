// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic

#include <mpi.h>

typedef int (*p2p_t)(void*, int, MPI_Datatype, int, int, MPI_Comm, MPI_Request*);

int main(int argc, char** argv) {
    int rank;
    int buf[4] = {1, 2, 3, 4};
    MPI_Request req;
    p2p_t comm;

    MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    // The annotation is placed right after the assignment, the call follows later with nontrivial arguments.
    // The dynamic analysis only sees the communication calls if the annotated indirect call is instrumented
    comm = rank == 0 ? (p2p_t)MPI_Isend : MPI_Irecv;
    CoVer_AnnotFP(comm, 2, MPI_Isend, MPI_Irecv);
    int count = rank + 4 - rank;
    comm(&buf[rank * 0], count, MPI_INT, 1 - rank, count * 0 + 7, MPI_COMM_WORLD, &req);
    MPI_Wait(&req, MPI_STATUS_IGNORE);

    MPI_Finalize();
    return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK-NOT: Contract violation detected!
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK-NOT: Contract violation detected!
// CHECK-NOT: Annotation verification failed!
// CHECK: Analysis finished.
// CHECK: CoVer-AnnotVerifier: Info: Checked annotation coverage of 2 processes
// CHECK-NOT: CoVer-AnnotVerifier: Warning
