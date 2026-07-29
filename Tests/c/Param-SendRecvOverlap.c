// Copied from MPI-BugBench: InvalidParam-Buffer-mpi_sendrecv-001

// RUN: %clangContracts %run_common

#include <mpi.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  int nprocs = -1;
  int rank = -1;

  MPI_Init(&argc, &argv);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  if (nprocs < 2)
    printf(
        "MBB ERROR: This test needs at least 2 processes to produce a bug!\n");

  int *buf = (int *)calloc(10, sizeof(int));

  int *recv_buf = (int *)calloc(10, sizeof(int));

  if (rank == 0) {
    /*MBBERROR_BEGIN*/ MPI_Sendrecv(buf, 10, MPI_INT, 1, 0, buf, 10, MPI_INT, 1,
                                    0, MPI_COMM_WORLD,
                                    MPI_STATUS_IGNORE); /*MBBERROR_END*/
  }
  if (rank == 1) {
    /*MBBERROR_BEGIN*/ MPI_Sendrecv(buf, 10, MPI_INT, 0, 0, buf, 10, MPI_INT, 0,
                                    0, MPI_COMM_WORLD,
                                    MPI_STATUS_IGNORE); /*MBBERROR_END*/
  }

  free(buf);

  free(recv_buf);

  MPI_Finalize();
  printf("Rank %d finished normally\n", rank);
  return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK: Contract violation detected!
// CHECK: Buffer is null or same as send buffer
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK: Contract violation detected!
// CHECK: Buffer is null or same as send buffer
// Dont check for analysis finished, MPI implementation might crash.
