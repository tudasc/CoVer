! RUN: %flangContracts %run_common

program main
    use mpi_f08
    use iso_c_binding
    implicit none

    integer :: rank
    type(c_ptr) :: winbuf
    integer, pointer :: buf(:)
    integer :: integer_size
    type(MPI_Win) :: win

    call MPI_Init()

    call MPI_Type_size(MPI_INT, integer_size)
    call MPI_Comm_rank(MPI_COMM_WORLD, rank)

    call MPI_Win_allocate(int(integer_size, kind=mpi_address_kind), integer_size, MPI_INFO_NULL, MPI_COMM_WORLD, winbuf, win)
    call c_f_pointer(winbuf, buf, [1])
    buf(1) = 42

    call MPI_Win_fence(0, win)
    if (rank == 0) then
        call MPI_Put(buf, 1, MPI_INT, 1, int(integer_size, kind=mpi_address_kind), 1, MPI_INT, win)
        print *, "Buf: ", buf(1)
    end if
    call MPI_Win_fence(0, win)

    call MPI_Finalize()
end program

! CHECK-LABEL: Running Contract Manager on Module
! CHECK-NOT: Contract violation detected!
! CHECK: CoVer: Total Tool Runtime

! CHECK-LABEL: CoVer-Dynamic: Initializing...
! CHECK-NOT: Contract violation detected!
! CHECK: Analysis finished.
