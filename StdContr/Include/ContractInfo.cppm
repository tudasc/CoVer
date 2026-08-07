module;

#include <map>
#include <clang/AST/Decl.h>
#include "ContractTree.hpp"

export module ContractInfo;
import ContractDataExtractor;

using namespace clang;
using namespace ContractTree;

export struct ContractInfo {
    std::map<FunctionDecl*, std::vector<TagUnit>> DeclToTags;
    std::map<std::string, std::vector<FunctionDecl*>> TagsToDecl;
    std::map<FunctionDecl*, ContractData> Contracts;
};
