# Usage

## Compiling with Contract Support

When compiling, the annotations must be parsed by the tool to enable error output.
This can be done using the supplied `clangContracts` and `clangContracts++` wrapper scripts for C and C++ respectively.

By default, these do **NOT** include the predefined contract definitions!
They only ensure that the code passes through the plugin during compilation.
See [this section](#adding-contracts-to-code) on how to add the contracts to the compilation process.

## Defining Contracts

Contracts are typically added to API function declaration.
Examples are given in the `mpi_contracts.h` and `shmem_contracts.h` headers.
More contracts can be defined by adding annotations to a function declaration (`__attribute__((annotate("CONTRACT{" <Contract String> "}")))`).
The function the contract is attached to is called the 'contract supplier'.
For ease of use, the macros defined in `Contracts.h` abstracts this syntax into a more readable form: `CONTRACT( <Contract String> )`.

The contract string defines the actual requirements placed on the code.
In general, three types of requirements are currently supported:
- `Precall(f)`: A function `f` must be called prior to the contract supplier.
  - Example: `PRE { call!(MPI_Init) }` at each MPI call, ensuring initialization.
- `PostCall(f)`: A function `f` must be called after the contract supplier.
  - Example: `POST { call!(MPI_Finalize) }` at each MPI call, ensuring finalization.
- `Release(f,op)`: After the contract supplier, an operation `op` must not occur until a function `f` is called.
  - Example: `POST { no! (call!(MPI_Isend)) until! (call!(MPI_type_commit)) }` at each MPI type constructor, ensuring the type is committed before use.

Multiple requirements are grouped by the scope (`PRE`, `POST`):
- Example: `POST { no! (call!(MPI_Isend)) until! (call!(MPI_type_commit)), call!(MPI_Finalize) }`
  This contract will enforce *both* requirements. For a disjunction, use `|` instead of a comma, and for an exclusive or `^`.

For more complicated constructions (disjunctions, XOR, parameter matching, tagging, read/write operations) see the examples given in the predefined headers.

## Adding Contracts to Code

After defining the contracts, they must be added to the code.
Some possible methods for C/C++ code in order of recommendation:
- Add separate headers with the same function signatures and appended contracts, and include using `-include` preprocessor flag
- Add separate headers, include in code
- Replace includes of API header (`mpi.h`, `shmem.h`) with the contract headers
- Modify API header directly
For Fortran, the contracts are defined using calls to `Declare_Contract`.
See the predefined contract sources for examples.

The predefined contracts can be included simply using the compile wrappers' `--predefined-contracts` option,
with some limitations. See [language support](LanguageSupport.md) for details.

## Adding Analyses

TODO
