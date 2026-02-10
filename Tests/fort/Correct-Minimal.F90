! RUN: %binaries/flangContracts --predefined-contracts --instrument-contracts %s -o %t.exe > %t.test_out 2>&1 && %t.exe >> %t.test_out 2>&1 && FileCheck %s < %t.test_out

program CorrectMinimal
    use mpi_f08
    implicit none
    integer :: ierr

    call MPI_Init(ierr)
    call MPI_Finalize(ierr)
end program CorrectMinimal

! CHECK-LABEL: Running Contract Manager on Module
! CHECK-NOT: Contract violation detected!
! CHECK: CoVer: Total Tool Runtime

! CHECK-LABEL: CoVer-Dynamic: Initializing...
! CHECK-NOT: Contract violation detected!
! CHECK: Analysis finished.
