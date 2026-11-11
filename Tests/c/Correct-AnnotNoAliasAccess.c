// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic

#include <stdlib.h>
#include <mpi.h>

#define N 4

// Three messages are staged in one heap buffer. Each section is filled while the sends of the earlier sections are still in flight.
// The writes are annotated as not aliasing the buffers in flight, but the writes to a section alias its own buffer, which is sent afterwards.
// As the annotated write values are only used for stores, the runtime verification only checks them against the buffers annotated before.
int main(int argc, char** argv) {
    double* buf = malloc(3 * N * sizeof(double));
    double in[N];
    MPI_Request r0, r1, r2;
    int rank;

    MPI_Init(NULL, NULL);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    int peer = 1 - rank;

    double* dst = buf;
    for (int i = 0; i < N; i++) dst[i] = i;
    CoVer_AnnotAlias(dst, false, 0);
    MPI_Isend(dst, N, MPI_DOUBLE, peer, 0, MPI_COMM_WORLD, &r0);

    dst += N;
    for (int i = 0; i < N; i++) {
        CoVer_AnnotAlias(&dst[i], false, 0);
        dst[i] = i;
    }
    CoVer_AnnotAlias(dst, false, 0);
    MPI_Isend(dst, N, MPI_DOUBLE, peer, 1, MPI_COMM_WORLD, &r1);

    dst += N;
    for (int i = 0; i < N; i++) {
        CoVer_AnnotAlias(&dst[i], false, 0);
        dst[i] = i;
    }
    MPI_Isend(dst, N, MPI_DOUBLE, peer, 2, MPI_COMM_WORLD, &r2);

    for (int t = 0; t < 3; t++) MPI_Recv(in, N, MPI_DOUBLE, peer, t, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    MPI_Wait(&r0, MPI_STATUS_IGNORE);
    MPI_Wait(&r1, MPI_STATUS_IGNORE);
    MPI_Wait(&r2, MPI_STATUS_IGNORE);

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
// CHECK: CoVer-AnnotVerifier: Info: Checked annotation coverage of 2 processes
// CHECK-NOT: CoVer-AnnotVerifier: Warning
