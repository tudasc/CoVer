# Language Support

Currently, the following languages are supported:
- C/C++
- Fortran (1990, 2008(ts), 2018)

When using the `--predefined-contracts` option, the Fortran compile wrapper assumes support for Fortran 2018 to be present even if not used.
If that is not the case, you will need to manually include the relevant predefined contract source file;
for example, there are three possibilites for MPI:
- `mpi_contracts.f90`: For F90 using `mpif.h` or `use mpi`
- `mpi_contracts_f08.f90`: For F08 with `use mpi_f08` without support for TS29113
- `mpi_contracts_f08ts.f90`: For F08 with TS29113, F18 and upwards
