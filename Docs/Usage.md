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
- `--interactive` (or `--interactive=tui`): Terminal UI, built with the CMake option `-DENABLE_TUI=ON` (Spack: `+tui`).
- `--interactive=gui`: Graphical UI, built with `-DENABLE_GUI=ON` (Spack: `+gui`), which requires Qt6.
Both options are off by default.

Both frontends provide the same workflow: Inspecting the contracts before the analysis, browsing the reported errors with references into the source code, and debugging the worklist analysis trace of a violated contract.
The trace debugger allows adding annotations (function pointer targets, alias groups, allocations), rerunning the analysis using them (`reanalyse`,
which skips the start menu and opens the trace again if it is still reported),
and write them to a patch file (`diff`) to apply later using `--ir-patches`.
The patch file can also be written from the list of reports, without opening a trace.
Type `help` in the trace debugger for a list of all commands.

The GUI additionally shows the trace as a graph.
Block 0 is where the trace starts, edges lead to the blocks executed before it.
If a block can be reached from multiple predecessors ("children"), the alternatives not currently followed are shown as dashed sibling nodes, double-click one to follow it instead.
Loops are shown as dotted edges to the repeated block.
Nodes can be dragged with the mouse, double-clicking a block shows its instructions, and right-clicking offers the annotation actions.
All GUI actions are issued as trace commands, so they appear in the command history and patch files just as in the TUI, and the command line is available in the GUI as well.

### LLM Chat

While debugging a trace, the `chat` command (or "Ask LLM..." in the GUI toolbar) opens a chat with a language model about the trace.
The model knows the contract, the violated formula and the error report, and can inspect the trace, the LLVM IR and the program's source files.
It can explain why an error is reported, judge whether it is a false positive, and add annotations.
Annotations added by the model are issued as trace commands, so they are recorded in the command history and patch files like your own,
and they are shown in the trace once the chat is closed. Reanalyse afterwards to check their effect, which the model cannot do itself.
The conversation lasts for the whole interactive session until CoVer exits.
The model is told about commands you ran in between, and when the trace changed or the program was reanalysed.

The server must support the Chat Completions API,
and the selected model must support tool use.
Note that the chat sends the trace, IR and source code requested by the model to the configured server.
The target server can be configured in CoVer's settings, accessed via the GUI or the `settings` command.

### MCP Server

Instead of the built-in chat, external agents supporting the [Model Context Protocol](https://modelcontextprotocol.io) can work on the reports.
Unlike the chat, they can work through all reports by themselves: list the reports, open their traces, add annotations and reanalyse, until only real bugs are left.
Annotations are issued as trace commands, so they are recorded in the command history and patch files like your own.

The MCP server runs during interactive mode on port 7341 (configurable as `mcp_port` in the user settings).
Its URL and the authorization header clients have to send are shown below the reports and in the "MCP" tab of the debugger (GUI),
or in the MCP view (TUI), opened with the `mcp` command of the trace debugger or the "Start MCP mode" entry of the report list.
If the server cannot be started (e.g. the port is in use), the interactive mode continues without it, and the button / command retries.

The token is generated on first use and stored as the `mcp_token` entry of CoVer's user settings,
remove the entry to generate a new token.

Then ask the agent to investigate the error.
Agents should call the `get_debug_context` tool first, which explains traces, annotations and the workflow.
If they detect a false positive, they can annotate independently.

In the GUI, tool calls are run right away and the trace is updated live, also while you use the GUI. The "MCP" tab of the debugger lists all tool calls.
In the TUI, tool calls are only run in MCP mode, which shows a view listing the tool calls instead of the reports and traces. Leave it with Escape to take over:
you then see the current report list or trace, and tool calls are refused until MCP mode is started again.

## Runtime Analysis

Runtime analysis depends on the `addr2line` utility.
While compilation is possible without it, this will cause file references to be missing in error reports.

To perform runtime analysis, the code must be instrumented.
This can be done by passing the option `--instrument-contracts` to the CoVer compile wrapper.

Then, launch the program as usual.
The analysis should run automatically.
To further check for coverage issues (using static-dynamic interaction, see TODO ref), run the same executable again including only the `--cover-check-coverage` flag.
This will make it read off the generated coverage files.
