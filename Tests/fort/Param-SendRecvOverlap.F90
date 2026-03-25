! Copied from MPI-BugBench: InvalidParam-Buffer-mpi_sendrecv-001

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
    integer, pointer :: buf(:)
    integer, pointer :: recv_buf(:)

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

    allocate (recv_buf(0:(10) - 1))

    if (rank == 0) then
! MBBERROR_BEGIN
        call MPI_Sendrecv(buf, 10, MPI_INTEGER, 1, 0, buf, 10, MPI_INTEGER, 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE, ierr)
! MBBERROR_END
    end if
    if (rank == 1) then
! MBBERROR_BEGIN
        call MPI_Sendrecv(buf, 10, MPI_INTEGER, 0, 0, buf, 10, MPI_INTEGER, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE, ierr)
! MBBERROR_END
    end if

    deallocate (buf)

    deallocate (recv_buf)

    call MPI_Finalize(ierr)
    print *, "Rank ", rank, " finished normally"
end program

! CHECK-LABEL: Running Contract Manager on Module
! CHECK: Contract violation detected!
! CHECK: Buffer is null or same as send buffer
! CHECK: CoVer: Total Tool Runtime

! CHECK-LABEL: CoVer-Dynamic: Initializing...
! CHECK: Contract violation detected!
! CHECK: Buffer is null or same as send buffer
! Dont check if analysis finished, MPI implementation might crash.
