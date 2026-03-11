! RUN: %flangContracts %run_common

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
