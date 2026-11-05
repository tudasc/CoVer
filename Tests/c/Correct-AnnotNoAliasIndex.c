// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic

#include <mpi.h>

// Requests are taken from an array using an index variable that is incremented after each use.
// The annotations all read the same variable, but its value differs between them
int main(int argc, char** argv) {
    int rank;
    int a = 1;
    int b = 2;
    int idx = 0;
    MPI_Request reqs[2];

    MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    CoVer_AnnotAlias(&reqs[idx], false, 0);
    if (rank == 0) MPI_Isend(&a, 1, MPI_INT, 1, 0, MPI_COMM_WORLD, &reqs[idx]);
    else MPI_Irecv(&a, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, &reqs[idx]);
    idx++;

    CoVer_AnnotAlias(&reqs[idx], false, 0);
    if (rank == 0) MPI_Isend(&b, 1, MPI_INT, 1, 1, MPI_COMM_WORLD, &reqs[idx]);
    else MPI_Irecv(&b, 1, MPI_INT, 0, 1, MPI_COMM_WORLD, &reqs[idx]);
    idx++;

    MPI_Waitall(idx, reqs, MPI_STATUSES_IGNORE);

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
