! RUN: %binaries/flangContracts --predefined-contracts --instrument-contracts %s -o %t.exe > %t.test_out 2>&1 && (COVER_COVERAGE_FOLDER="%t_coverage" %t.exe >> %t.test_out 2>&1 || true) && FileCheck %s < %t.test_out

program PreCallMissingInit
    use mpi_f08
    implicit none
    integer :: ierr

    call MPI_Finalize(ierr)
end program PreCallMissingInit

! CHECK-LABEL: Running Contract Manager on Module
! CHECK: Contract violation detected!
! CHECK: CoVer: Total Tool Runtime

! CHECK-LABEL: CoVer-Dynamic: Initializing...
! CHECK: Contract violation detected!
! CHECK: Missing Initialization call
! Dont check if analysis finished, MPI implementation might crash.
