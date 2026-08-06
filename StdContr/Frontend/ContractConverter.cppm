module;

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclarationName.h>
#include <clang/Basic/IdentifierTable.h>
#include <clang/Basic/SourceManager.h>
#include <filesystem>
#include <fstream>
#include <llvm/ADT/RewriteBuffer.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/raw_ostream.h>
#include <map>
#include <clang/AST/Decl.h>
#include <clang/AST/Mangle.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <clang/Lex/Lexer.h>
#include <llvm/Support/ErrorHandling.h>
#include <memory>
#include <set>
#include <system_error>
#include <vector>

#include "ContractTree.hpp"

export module ContractConverter;
import ContractDataExtractor;
import ContractInfo;
import FunctionMods;

using namespace clang;
using namespace llvm;
using namespace ContractTree;

namespace {

CompilerInstance* CI;

struct DeclModifiers {
    std::set<std::string> PreCallSentinels;
    std::set<std::string> PreCallChecks;
};

#define LOC_PRIOR_SEMI(CI, SM, decl) (Lexer::getLocForEndOfToken(SM.getExpansionRange(decl->getSourceRange()).getEnd(), 0, SM, CI->getASTContext().getLangOpts()))

constexpr std::string_view COVER_PREFIX = "COVER_SENTINEL_";

std::map<FunctionDecl const*,DeclModifiers> DeclToMods;
std::map<FunctionDecl const*,std::string> DeclToPreConds;

// Copied from ContractManager
const std::vector<std::shared_ptr<ContractExpression>> linearizeContractFormula(const std::shared_ptr<ContractFormula> contrF) {
    if (contrF->Children.empty()) {
        return { std::static_pointer_cast<ContractExpression>(contrF) };
    }
    std::vector<std::shared_ptr<ContractExpression>> exprs;
    for (std::shared_ptr<ContractFormula> form : contrF->Children ) {
        std::vector<std::shared_ptr<ContractExpression>> linChild = linearizeContractFormula(form);
        exprs.insert( exprs.end(), linChild.begin(), linChild.end() );
    }
    return exprs;
}

FunctionDecl* lookupDecl(std::string target) {
    IdentifierInfo& II = CI->getASTContext().Idents.get(target);
    DeclarationName DN(&II);
    auto res = CI->getASTContext().getTranslationUnitDecl()->lookup(DN);
    for (NamedDecl* ND : res) {
        if (FunctionDecl* FD = dyn_cast<FunctionDecl>(ND)) {
            return FD;
        }
    }
    return nullptr;
}

std::string getMangledName(const NamedDecl *ND, ASTContext &Ctx) {
  std::unique_ptr<MangleContext> MC(Ctx.createMangleContext());

  // Some decls shouldn't be mangled (e.g. extern "C", main).
  if (!MC->shouldMangleDeclName(ND))
    return ND->getNameAsString();

  std::string Mangled;
  llvm::raw_string_ostream OS(Mangled);

  if (const auto *CtorD = dyn_cast<CXXConstructorDecl>(ND))
    MC->mangleName(GlobalDecl(CtorD, Ctor_Complete), OS);
  else if (const auto *DtorD = dyn_cast<CXXDestructorDecl>(ND))
    MC->mangleName(GlobalDecl(DtorD, Dtor_Complete), OS);
  else
    MC->mangleName(GlobalDecl(ND), OS);

  return OS.str();
}

std::string buildForwardingCall(FunctionDecl const* decl,
                                ASTContext const& Ctx) {
  std::string Args;
  std::string sep;
  for (ParmVarDecl const* param : decl->parameters()) {
    Args += sep + param->getNameAsString();
    sep = ", ";
  }

  std::string Prefix;
  std::string Name;

  if (CXXMethodDecl const* MD = dyn_cast<CXXMethodDecl>(decl)) {
    if (MD->isStatic()) {
      // Static method: qualified name is directly callable.
      llvm::raw_string_ostream OS(Name);
      MD->getNameForDiagnostic(OS, Ctx.getPrintingPolicy(),
                               /*Qualified=*/true);
      OS.flush();
    } else {
        Prefix= "this->";
        Name = MD->getNameAsString();
    }
  } else {
    // Free function: fully-qualified name.
    llvm::raw_string_ostream OS(Name);
    decl->getNameForDiagnostic(OS, Ctx.getPrintingPolicy(),
                                 /*Qualified=*/true);
    OS.flush();
  }

  std::string RetKeyword;
  if (!decl->getReturnType()->isVoidType())
    RetKeyword = "return ";

  return RetKeyword + Name + "(" + Args + ");";
}

std::string constructFormula(std::shared_ptr<ContractFormula> const& form, bool isPre, FunctionDecl const* decl) {
    if (form->Children.empty()) {
        std::shared_ptr<ContractExpression> expr = std::static_pointer_cast<ContractExpression>(form);
        switch (expr->OP->type()) {
            case FormulaType::AND:
            case FormulaType::OR:
            case FormulaType::XOR:
                llvm_unreachable("Unexpected connective in expression!");
                break;
            case FormulaType::RWOP:
            case FormulaType::READ:
            case FormulaType::WRITE:
                break;
            case FormulaType::ALLOC:
                break;
            case FormulaType::FREE:
                break;
            case FormulaType::CALL: {
                std::shared_ptr<CallOperation const> cOP = std::static_pointer_cast<CallOperation const>(expr->OP);
                if (isPre) {
                    DeclToMods[decl].PreCallChecks.insert(cOP->Function);
                    DeclToMods[lookupDecl(cOP->Function)].PreCallSentinels.insert(cOP->Function);
                    // Currently building precond, so need to return the sentinel as the boolean expr
                    return COVER_PREFIX + cOP->Function;
                } else {

                }
                break;
            }
            case FormulaType::CALLTAG: {
                break;
            }
            case FormulaType::RELEASE: {
                break;
            }
            case FormulaType::PARAM: {
                break;
            }
        }
    } else {
        std::string sep = "";
        std::string postfix = "";
        switch (form->type) {
            case FormulaType::AND:
                sep = "&&";
                break;
            case FormulaType::OR:
                sep = "||";
                break;
            case FormulaType::XOR:
                sep = "+";
                postfix = " == 1";
                break;
            default:
                llvm_unreachable("Non-connective type with no children!");
                break;
        }
        std::string_view act_sep;
        std::string res;
        for (std::shared_ptr<ContractFormula> child : form->Children) {
            res += constructFormula(child, isPre, decl);
            act_sep = sep;
        }
        return "(" + res + postfix + ")";
    }
    return "";
}

void performOutput(std::string output_path) {
    SourceManager& SM = CI->getSourceManager();
    Rewriter R(SM, CI->getLangOpts());

    std::filesystem::create_directories(output_path);
    std::ofstream backendfile(output_path + "/cover_wrapfile");

    std::set<std::string> sentinel_strs;
    std::set<FunctionDecl const*> declRename;
    std::map<FunctionDecl const*,std::string> functionBodies;

    // Apply modifications per declaration
    for (auto& [decl, mods] : DeclToMods) {
        // Remove CoVer contract annotation
        if (AnnotateAttr* AA = decl->getAttr<AnnotateAttr>())
            R.RemoveText(SM.getExpansionRange(AA->getRange()));

        // PreCallCheck is contained in precond. Only need to add to wrap list
        if (!mods.PreCallChecks.empty()) declRename.insert(decl);
        // PreCallSentinel needs wrap + set of sentinel value + decl of sentinel value
        if (!mods.PreCallSentinels.empty()) {
            declRename.insert(decl);
            for (std::string sentinel : mods.PreCallSentinels) {
                sentinel_strs.insert(sentinel);
                functionBodies[decl] += ("    " + COVER_PREFIX + sentinel + " = 1;\n").str();
            }
        }
    }

    // Add include to original file
    R.InsertTextBefore(SM.getLocForStartOfFile(SM.getMainFileID()), "#include \"" + SM.getFileEntryForID(SM.getMainFileID())->tryGetRealPathName().str() + "\"\n");

    // Add global sentinel values
    R.InsertTextBefore(SM.getLocForStartOfFile(SM.getMainFileID()), "#include <cstdint>\n");
    for (std::string sentinel : sentinel_strs) {
        R.InsertTextAfter(SM.getLocForStartOfFile(SM.getMainFileID()), ("int8_t " + COVER_PREFIX + sentinel + " = 0;\n").str());
    }

    // Rename functions to wrapper names, add call to orig, and add empty braces to turn into definition
    // Also, generate wrapfile
    for (FunctionDecl const* decl : declRename) {
        functionBodies[decl];
        std::string mangledName = getMangledName(decl, CI->getASTContext());
        backendfile << mangledName << "\n";

        // Remove function definition
        if (decl->isThisDeclarationADefinition()) {
            SourceRange range = decl->getBody()->getSourceRange();
            SourceLocation end = Lexer::getLocForEndOfToken( range.getEnd(), 0, R.getSourceMgr(), R.getLangOpts());
            R.RemoveText(SourceRange(range.getBegin(), end));
        }

        // Rename to wrapper func
        R.InsertTextBefore(decl->getLocation(), "CoVer_Wrapper_");

        // Get declaration and insert with asm + contract
        std::string decl_str = R.getRewrittenText(SM.getExpansionRange(decl->getSourceRange()));
        std::stringstream rewriteDeclStr;
        rewriteDeclStr << decl_str;
        if (DeclToPreConds.contains(decl)) rewriteDeclStr << " pre(" << DeclToPreConds[decl] << ")";
        rewriteDeclStr << "asm(\"CoVer_Wrapper_" + mangledName + "\")";
        rewriteDeclStr << ";\n";
        R.InsertTextBefore(decl->getBeginLoc(), rewriteDeclStr.str());

        // Insert modified function body with forward call
        R.InsertTextAfter(LOC_PRIOR_SEMI(CI, SM, decl), " {\n" + functionBodies[decl] + "    " + buildForwardingCall(decl, CI->getASTContext()) + "\n}");
    }

    // Perform output for include file
    FileID FID = SM.getMainFileID();
    std::error_code EC;
    llvm::raw_fd_ostream Out(output_path + "/include.cpp", EC, llvm::sys::fs::OF_Text);
    R.getEditBuffer(FID).write(Out);
}

}

export namespace ContractConverter {
    void Convert(ContractInfo DB, CompilerInstance& _CI, std::string output_path) {
        CI = &_CI;
        for (auto& [Decl, Data] : DB.Contracts) {
            errs() << "Converting contract for " << Decl->getName() << "\n";
            std::string preCond = constructFormula(Data.Pre, true, Decl);
            DeclToPreConds[Decl] = preCond;
        }
        performOutput(output_path);
    }
}
