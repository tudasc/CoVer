! RUN: %flangContracts %run_common

program PreCallMissingInit
    use mpi_f08
    implicit none
    integer :: ierr

    call MPI_Finalize(ierr)
end program PreCallMissingInit

! CHECK-LABEL: Running Contract Manager on Module
! CHECK: Contract violation detected!
! CHECK: Missing Initialization call
! CHECK: CoVer: Total Tool Runtime

! CHECK-LABEL: CoVer-Dynamic: Initializing...
! CHECK: Contract violation detected!
! CHECK: Missing Initialization call
! Dont check if analysis finished, MPI implementation might crash.
