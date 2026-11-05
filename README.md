# CoVer

CoVer is an extensible contract verification framework for parallel programming models.
Currently, the tool ships with analysis support using premade contracts for MPI and OpenSHMEM.
Further models may be added by adding suitable contract declarations.

## Usage

See [Usage](./Docs/Usage.md)

## Prerequisites

- C++23
- LLVM 22 or newer
- CMake 4.5 or newer with Ninja
- libcurl
- Java (Build dependency only)
- Python (Build dependency only)

Optionally:
- ANTLR4 (provided if not installed)
- MPI (for premade headers)
    - If OpenMPI with OpenSHMEM support is used, premade OpenSHMEM headers are generated as well.
- Qt6 (for the graphical interactive mode)

## Building

Use CMake to build this project locally.
Alternatively, the Docker file provides a known working environment for testing,
and `spack_repo/` contains a Spack package definition.

### Build Options

| Option | Default | Description |
| ------ | ------- | ----------- |
| `ENABLE_TUI` | `OFF` | Build the terminal UI of the interactive mode. |
| `ENABLE_GUI` | `OFF` | Build the graphical UI of the interactive mode. Requires Qt6. |
| `ENABLE_TESTS` | `OFF` | Enable testing |
| `BUNDLED_JSONCPP` | `OFF` | Use bundled jsoncpp instead of the system one. |
| `MPI_APIS_JSON` | unset | Local copy of the pympistandard `apis.json` used for the premade MPI contracts. If unset, it is downloaded during configuration. |
| `SHMEM_HEADER_PATH` | unset | `shmem.h` for the premade OpenSHMEM contracts, if it is not next to `mpi.h`. |

## References

| Reference  | Additional Info |
| ---------- | --------------- |
| [Verifying MPI API Usage Requirements with Contracts](https://doi.org/10.1007/978-3-032-07194-1_4) | First CoVer Publication |
| [Coupling Static and Dynamic MPI Correctness Tools to Optimize Accuracy and Overhead](https://doi.org/10.1145/3731599.3767360) | Corresponds to [PR 1](https://github.com/tudasc/CoVer/pull/1) |
| [Dynamic Contract Analysis for Parallel Programming Models](https://doi.org/10.1109/IPDPSW71298.2026.00041) | Corresponds to [PR 2](https://github.com/tudasc/CoVer/pull/1) |
| [Allocation Tracking and Parameter Checking for Parallel Programming Models Using Contracts](https://doi.org/10.1007/978-3-032-40777-1_4) | Corresponds to [PR 5](https://github.com/tudasc/CoVer/pull/5) |
