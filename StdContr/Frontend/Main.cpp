#include <clang/AST/ASTConsumer.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendPluginRegistry.h>
#include <memory>

import ContractCollector;
import ContractValueCollector;
import ContractConverter;
import ContractInfo;

using namespace clang;
using namespace llvm;

class ContractConsumer : public ASTConsumer {
  public:
    explicit ContractConsumer(CompilerInstance& _CI, std::string _output_folder) : CI(_CI), output_path(_output_folder) {}

    void HandleTranslationUnit(ASTContext& Ctx) override {
        errs() << "Started CoVerStdCXXConverter\n";
        
        // Parsed result holder
        ContractInfo info;

        // Get contracts from the func annots
        ContractCollector::FuncDeclVisitor FuncVisitor(Ctx, info);
        FuncVisitor.TraverseDecl(Ctx.getTranslationUnitDecl());

        // Get the values from the global contract values
        ContractValueCollector::VarDeclVisitor VarVisitor(Ctx, info);
        VarVisitor.TraverseDecl(Ctx.getTranslationUnitDecl());

        llvm::outs() << "-- " << info.Contracts.size() << " CoVer contract(s) encountered\n";
        ContractConverter::Convert(info, CI, output_path);
    }

  private:
    CompilerInstance& CI;
    std::string output_path;
};

class CoVerStdCXXFrontend : public PluginASTAction {
  public:
    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance& CI, llvm::StringRef) override {
        return std::make_unique<ContractConsumer>(CI, output_path);
    }

    bool ParseArgs(const CompilerInstance& CI, const std::vector<std::string>& Args) override {
        if (!Args.empty()) {
            for (int i = 0; i < Args.size(); i++) {
                constexpr std::string_view OUTPUT_ARG = "output-folder=";
                if (Args[i].starts_with(OUTPUT_ARG)) {
                    output_path = Args[i].substr(OUTPUT_ARG.size());
                    if (output_path.empty()) {
                        errs() << "No output folder specified!\n";
                        return false;
                    }
                } else {
                    errs() << "Unrecognized option " << Args[i] << "!\n";
                    return false;
                }
            }
        }
        return true;
    }

    /// Run alongside the normal compilation instead of replacing it.
    PluginASTAction::ActionType getActionType() override { return ReplaceAction; }

  private:
    std::string output_path;
};

static FrontendPluginRegistry::Add<CoVerStdCXXFrontend> X("CoVerStdCXXFrontend", "Transform CoVer to C++26 contracts, and generate wrapper files as necessary");
