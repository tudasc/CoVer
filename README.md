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

| Reference  | Additional Info |
| ---------- | --------------- |
| [Verifying MPI API Usage Requirements with Contracts](https://doi.org/10.1145/3731599.3767360) | First CoVer Publication    |
| [Coupling Static and Dynamic MPI Correctness Tools to Optimize Accuracy and Overhead](https://doi.org/10.1007/978-3-032-07194-1_4) | Corresponds to [PR 1](https://github.com/tudasc/CoVer/pull/1)|
