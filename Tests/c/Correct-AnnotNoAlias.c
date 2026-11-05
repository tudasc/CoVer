// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic

#include <mpi.h>

// Two requests of the same array are in flight at the same time.
// Without no-alias annotations, the static analysis can not tell the array elements apart (Double Request Use)
int main(int argc, char** argv) {
    int rank;
    int a = 1;
    int b = 2;
    MPI_Request reqs[2];

    MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    CoVer_AnnotAlias(&reqs[0], false, 0);
    CoVer_AnnotAlias(&reqs[1], false, 0);
    if (rank == 0) {
        MPI_Isend(&a, 1, MPI_INT, 1, 0, MPI_COMM_WORLD, &reqs[0]);
        MPI_Isend(&b, 1, MPI_INT, 1, 1, MPI_COMM_WORLD, &reqs[1]);
    } else {
        MPI_Irecv(&a, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, &reqs[0]);
        MPI_Irecv(&b, 1, MPI_INT, 0, 1, MPI_COMM_WORLD, &reqs[1]);
    }
    MPI_Wait(&reqs[0], MPI_STATUS_IGNORE);
    MPI_Wait(&reqs[1], MPI_STATUS_IGNORE);

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
