// Copied from MPI-BugBench: Correct-Rank-001

// RUN: %clangContracts %run_common

#include <mpi.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  int nprocs = -1;
  int rank = -1;
  MPI_Win mpi_win_0;
  int *winbuf;

  MPI_Init(&argc, &argv);
  MPI_Comm_size(MPI_COMM_WORLD, &nprocs);
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  if (nprocs < 2)
    printf(
        "MBB ERROR: This test needs at least 2 processes to produce a bug!\n");

  int *buf = (int *)calloc(10, sizeof(int));

  MPI_Win_allocate(10 * sizeof(int), sizeof(int), MPI_INFO_NULL, MPI_COMM_WORLD,
                   &winbuf, &mpi_win_0);
  MPI_Win_fence(0, mpi_win_0);
  if (rank == 0) {
    MPI_Get(buf, 10, MPI_INT, MPI_PROC_NULL, 0, 10, MPI_INT, mpi_win_0);
  }
  MPI_Win_fence(0, mpi_win_0);
  MPI_Win_free(&mpi_win_0);

  MPI_Finalize();
  printf("Rank %d finished normally\n", rank);
  return 0;
}

// CHECK-LABEL: Running Contract Manager on Module
// CHECK-NOT: Contract violation detected!
// CHECK: CoVer: Total Tool Runtime

// CHECK-LABEL: CoVer-Dynamic: Initializing...
// CHECK-NOT: Contract violation detected!
// CHECK: Analysis finished.
