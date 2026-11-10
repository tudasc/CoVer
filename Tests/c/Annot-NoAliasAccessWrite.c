// RUN: %CoVerCC %run_compile
// RUN: %run_dynamic

#include <stdlib.h>
#include <mpi.h>

#define N 4

// Like Correct-AnnotNoAliasAccess, but the last section is not advanced: Its writes go to the buffer of the second send, which is still in flight.
// The no-alias annotation of the writes is wrong, hiding the data race from the static analysis. The runtime verification has to report it.
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
// CHECK: Annotation verification failed!
// CHECK: Alias group 0 has two values which alias even though they should not
// CHECK: First value found at {{.*}}Annot-NoAliasAccessWrite.c:31
// CHECK: Second value found at {{.*}}Annot-NoAliasAccessWrite.c:36
// Dont check for analysis finished, MPI implementation might crash.
