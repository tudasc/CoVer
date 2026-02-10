! RUN: %binaries/flangContracts --predefined-contracts --instrument-contracts %s -o %t.exe > %t.test_out 2>&1 && %t.exe >> %t.test_out 2>&1 && FileCheck %s < %t.test_out

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
! Dont check for analysis finished, MPI implementation might crash.
