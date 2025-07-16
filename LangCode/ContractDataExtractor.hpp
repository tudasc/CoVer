#include <optional>

#include "ContractTree.hpp"

// This header separates LLVM from ANTLR
std::optional<ContractTree::ContractData> getContractData(std::string contract);
