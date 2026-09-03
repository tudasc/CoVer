module;

#include <map>
#include <set>
#include <clang/AST/Decl.h>
#include "ContractTree.hpp"

export module ContractInfo;
import ContractDataExtractor;

using namespace clang;
using namespace ContractTree;

export enum struct ValueKind {
    Integer,
    PointerConstant,
    SymbolAddress,
    Unresolved,
};

export struct ContractValue {
    std::string Identifier;
    std::string GlobalName;
    int64_t CounterID = -1;

    ValueKind Kind = ValueKind::Unresolved;
    /// Numeric payload for Integer / PointerConstant, and the addend for SymbolAddress.
    int64_t IntValue = 0;
    /// Symbol name for SymbolAddress.
    std::string Symbol;
    /// Payload as it appears in IR after the (void*) conversion, e.g. "0xfffffffffffffffe".
    std::string RawPointer;
    /// Source-level spelling of the payload expression after the (void*) casts are peeled off.
    std::string Expression;
    /// Type the payload had before the macro converted it to void*, e.g. "int" or "MPI_Comm".
    std::string PayloadType;
    /// Rendered value, e.g. "-2" or "&ompi_mpi_comm_world".
    std::string ValueText;
    /// Where the payload tokens are actually written, i.e. the macro definition in mpi.h.
    std::string PayloadSpelling;
};

export struct ContractInfo {
    std::map<FunctionDecl const*, std::vector<TagUnit>> DeclToTags;
    std::map<std::string, std::set<FunctionDecl const*>> TagsToDecl;
    std::map<FunctionDecl*, ContractData> Contracts;
    std::map<std::string,std::string> ContractValToGlobal;
};
