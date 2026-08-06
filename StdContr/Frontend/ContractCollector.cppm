module;

#include "ContractTree.hpp"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"

#include "llvm/Support/raw_ostream.h"
#include <clang/AST/Attrs.inc>
#include <optional>
#include <map>

export module ContractCollector;
import ContractDataExtractor;
import ContractInfo;

using namespace clang;
using namespace llvm;

namespace ContractCollector {

export class FuncDeclVisitor : public RecursiveASTVisitor<FuncDeclVisitor> {
  public:
    FuncDeclVisitor(ASTContext& Ctx) : SM(Ctx.getSourceManager()) {}

    /// Uninstantiated templates are not traversed by default, but their
    /// patterns are declarations we want to see as well.
    bool shouldVisitTemplateInstantiations() const { return true; }

    bool VisitFunctionDecl(FunctionDecl* FD) {
        report(FD);
        return true;
    }

    ContractInfo getContractInfo() const { return contractInfo; }

  private:
    void report(FunctionDecl* FD) {
        SourceLocation Loc = FD->getLocation();
        if (FD->isImplicit() || !SM.isInMainFile(Loc))
            return;

        if (AnnotateAttr* AA = FD->getAttr<AnnotateAttr>()) {
            StringRef annot = AA->getAnnotation();
            std::optional<ContractTree::ContractData> data = getContractData(annot.str());
            if (!data) {
                errs() << "Found non-CoVer annotation at " << SM.getFilename(Loc) << ":" << SM.getSpellingLineNumber(Loc) << " for function " << FD->getName() << "\n";
            }
            contractInfo.Contracts.insert({FD, *data});
        }
    }

    SourceManager& SM;
    ContractInfo contractInfo;
};

}
