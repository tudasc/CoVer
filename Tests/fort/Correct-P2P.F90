! RUN: %flangContracts %run_common

program main
    use mpi_f08
    integer :: rank
    integer, pointer :: buf(:)
    type(MPI_Request) :: req

    call MPI_Init()

    call MPI_Comm_rank(MPI_COMM_WORLD, rank)

    allocate(buf(1))
    buf(1) = 42
    if (rank == 0) then
        call MPI_Isend(buf, 1, MPI_INT, 1, 0, MPI_COMM_WORLD, req)
        print *, "Buf: ", buf(1)
    else
        call MPI_Irecv(buf, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, req)
    end if
    call MPI_Wait(req, MPI_STATUS_IGNORE)

    call MPI_Finalize()
end program

! CHECK-LABEL: Running Contract Manager on Module
! CHECK-NOT: Contract violation detected!
! CHECK: CoVer: Total Tool Runtime

! CHECK-LABEL: CoVer-Dynamic: Initializing...
! CHECK-NOT: Contract violation detected!
! CHECK: Analysis finished.
