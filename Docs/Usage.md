# Usage

## Compiling with Contract Support

When compiling, the annotations must be parsed by the tool to enable error output.
This can be done using the supplied `CoVerCC`, `CoVerFC` and `CoVerFC` wrapper scripts for C, C++ and Fortran respectively.

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

## Interactive Mode

The static analysis results can be inspected interactively by passing `--interactive` to the compile wrappers.
Two frontends are available:
- `--interactive` (or `--interactive=tui`): Terminal UI
- `--interactive=gui`: Graphical UI.
  This requires CoVer to be built with Qt6,
  which CMake enables automatically if Qt is found.
  If no display is available, the TUI is used instead.

Both frontends provide the same workflow: Inspecting the contracts before the analysis, browsing the reported errors with references into the source code, and debugging the worklist analysis trace of a violated contract.
The trace debugger allows adding annotations (function pointer targets, alias groups, allocations), rerunning the analysis using them (`reanalyse`),
and write them to a patch file (`diff`) to apply later using `--ir-patches`.
Type `help` in the trace debugger for a list of all commands.

The GUI additionally shows the trace as a graph.
Block 0 is where the trace starts, edges lead to the blocks executed before it.
If a block can be reached from multiple predecessors ("children"), the alternatives not currently followed are shown as dashed sibling nodes, double-click one to follow it instead.
Loops are shown as dotted edges to the repeated block.
Nodes can be dragged with the mouse, double-clicking a block shows its instructions, and right-clicking offers the annotation actions.
All GUI actions are issued as trace commands, so they appear in the command history and patch files just as in the TUI, and the command line is available in the GUI as well.

## Runtime Analysis

Runtime analysis depends on the `addr2line` utility.
While compilation is possible without it, this will cause file references to be missing in error reports.

To perform runtime analysis, the code must be instrumented.
This can be done by passing the option `--instrument-contracts` to the CoVer compile wrapper.

Then, launch the program as usual.
The analysis should run automatically.
To further check for coverage issues (using static-dynamic interaction, see TODO ref), run the same executable again including only the `--cover-check-coverage` flag.
This will make it read off the generated coverage files.
