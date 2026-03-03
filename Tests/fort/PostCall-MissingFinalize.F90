! RUN: %flangContracts %run_common

program PostCallMissingFinalize
    use mpi_f08
    implicit none
    integer :: ierr

    call MPI_Init(ierr)
end program PostCallMissingFinalize

! CHECK-LABEL: Running Contract Manager on Module
! CHECK: Contract violation detected!
! CHECK: CoVer: Total Tool Runtime

! CHECK-LABEL: CoVer-Dynamic: Initializing...
! CHECK: Contract violation detected!
! CHECK: Missing Finalization call
! Dont check if analysis finished, MPI implementation might crash.
