! Copied from MPI-BugBench: Correct-Rank-001

! RUN: %flangContracts %run_common

program main
    use mpi_f08
    implicit none

    integer :: ierr
    integer :: nprocs = -1
    integer :: rank = -1
    integer :: double_size
    integer :: integer_size
    integer :: logical_size
    integer :: i ! Loop index used by some tests
    type(MPI_Win) :: mpi_win_0
    type(c_ptr) :: winbuf
    integer, pointer :: buf(:)

    call MPI_Init(ierr)
    call MPI_Comm_size(MPI_COMM_WORLD, nprocs, ierr)
    call MPI_Comm_rank(MPI_COMM_WORLD, rank, ierr)
    if (nprocs .lt. 2) then
        print *, "MBB ERROR: This test needs at least 2 processes to produce a bug!\n"
    end if

    call mpi_type_size(MPI_DOUBLE_PRECISION, double_size, ierr)
    call mpi_type_size(MPI_INTEGER, integer_size, ierr)
    call mpi_type_size(MPI_LOGICAL, logical_size, ierr)

    allocate (buf(0:(10) - 1))

    call MPI_Win_allocate(int(10*integer_size, mpi_address_kind), integer_size, MPI_INFO_NULL, MPI_COMM_WORLD, winbuf, mpi_win_0, ierr)
    call MPI_Win_fence(0, mpi_win_0, ierr)
    if (rank == 0) then
        call MPI_Get(buf, 10, MPI_INTEGER, MPI_PROC_NULL, int(0, mpi_address_kind), 10, MPI_INTEGER, mpi_win_0, ierr)
    end if
    call MPI_Win_fence(0, mpi_win_0, ierr)
    call MPI_Win_free(mpi_win_0, ierr)

    call MPI_Finalize(ierr)
    print *, "Rank ", rank, " finished normally"
end program

! CHECK-LABEL: Running Contract Manager on Module
! CHECK-NOT: Contract violation detected!
! CHECK: CoVer: Total Tool Runtime

! CHECK-LABEL: CoVer-Dynamic: Initializing...
! CHECK-NOT: Contract violation detected!
! CHECK: Analysis finished.
