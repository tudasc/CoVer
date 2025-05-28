# CoVer

CoVer is an extensible contract verification framework for parallel programming models.
Currently, the tool ships configurations for MPI and OpenSHMEM. Further models may be added by adding suitable contract declarations.

## Usage

See [Usage](./Docs/Usage.md)

## Prerequisites

- LLVM 18 or newer
- Java (Build dependency only)

Optionally:
- ANTLR4 (provided if not installed)
- MPI (for premade headers)
    - If OpenMPI with OpenSHMEM support is used, premade OpenSHMEM headers are generated as well.
- Python (for premade headers)

## Building

Use CMake to build this project locally.
Alternatively, the Docker file provides a known working environment for testing.

## References

- TODO
