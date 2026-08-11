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
#include <llvm/Support/FileSystem.h>
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
#include <sstream>
#include <string>
#include <string_view>
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
    std::set<std::string> CallSentinels;
    std::set<std::string> PreCallChecks;
};

std::map<FunctionDecl const*, std::set<std::string>> PostCallChecks;

#define LOC_PRIOR_SEMI(CI, SM, decl) (Lexer::getLocForEndOfToken(SM.getExpansionRange(decl->getSourceRange()).getEnd(), 0, SM, CI->getASTContext().getLangOpts()))

constexpr std::string_view COVER_SENTINEL_PREFIX = "COVER_SENTINEL_";

std::map<FunctionDecl const*,DeclModifiers> DeclToMods;
std::map<FunctionDecl const*,std::string> DeclToPreConds;

std::set<std::string> ContractVars;

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

std::string comparatorToString(Comparator const& comp) {
    switch (comp) {
        case NEQ: return "!=";
        case GT: return ">";
        case GTEQ: return ">=";
        case LT: return "<";
        case LTEQ: return "<=";
        case EXEQ: return "==";
        case EQ: return "==";
    }
}

std::string constructFormula(std::shared_ptr<ContractFormula> const& form, bool isPre, FunctionDecl const* decl, ContractInfo const& DB) {
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
                llvm_unreachable("Unexpected memoryop in expression!");
                break;
            case FormulaType::ALLOC:
                break;
            case FormulaType::FREE:
                break;
            case FormulaType::CALL: {
                std::shared_ptr<CallOperation const> cOP = std::static_pointer_cast<CallOperation const>(expr->OP);
                FunctionDecl const* target_func = lookupDecl(cOP->Function);
                if (target_func) DeclToMods[target_func].CallSentinels.insert(cOP->Function);
                if (isPre) {
                    DeclToMods[decl].PreCallChecks.insert(cOP->Function);
                    // Currently building precond, so need to return the sentinel as the boolean expr
                    return ("TERM(" + COVER_SENTINEL_PREFIX + cOP->Function + ", \"PRE{" + expr->ExprStr + "}\")").str();
                } else {
                    PostCallChecks[decl].insert(cOP->Function);
                    DeclToMods[decl].CallSentinels.insert(decl->getNameAsString());
                    return "true";
                }
                break;
            }
            case FormulaType::CALLTAG: {
                std::shared_ptr<CallTagOperation const> ctOP = std::static_pointer_cast<CallTagOperation const>(expr->OP);
                for (FunctionDecl* target : DB.TagsToDecl.at(ctOP->Function)) {
                    DeclToMods[target].CallSentinels.insert(ctOP->Function);
                }
                if (isPre) {
                    DeclToMods[decl].PreCallChecks.insert(ctOP->Function);
                    return ("TERM(" + COVER_SENTINEL_PREFIX + ctOP->Function + ", \"PRE{" + expr->ExprStr + "}\")").str();
                } else {
                    PostCallChecks[decl].insert(ctOP->Function);
                    DeclToMods[decl].CallSentinels.insert(decl->getNameAsString());
                    return "true";
                }
                break;
            }
            case FormulaType::RELEASE: {
                break;
            }
            case FormulaType::PARAM: {
                std::shared_ptr<ParamOperation const> pOP = std::static_pointer_cast<ParamOperation const>(expr->OP);
                if (isPre) {
                    std::string exceptions;
                    std::string paramreq_str;
                    std::string callVal = decl->getParamDecl(pOP->idx)->getNameAsString();
                    for (ParamRequirement const& req : pOP->reqs) {
                        std::string compstr = comparatorToString(req.comp);
                        std::string compVal = req.value;
                        if (req.isArg) compVal = decl->getParamDecl(std::stoi(req.value))->getNameAsString();
                        else if (DB.ContractValToGlobal.contains(req.value)) compVal = "(" + decl->getParamDecl(pOP->idx)->getType().getAsString() + ")(uintptr_t)" + DB.ContractValToGlobal.at(req.value) + ".value";
                        if (!req.isArg) ContractVars.insert(req.value);
                        if (req.comp == Comparator::EXEQ) {
                            exceptions += callVal + compstr + compVal + " || ";
                        } else {
                            paramreq_str += callVal + compstr + compVal + " && ";
                        }
                    }
                    return "TERM(((" + exceptions + " false) || (" + paramreq_str + " true)), \"PRE{" + expr->ExprStr + "}\")";
                } else {
                    llvm_unreachable("Unexpected ParamOp in postcondition!\n");
                }
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
            res += act_sep + constructFormula(child, isPre, decl, DB);
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
        if (!mods.CallSentinels.empty()) {
            declRename.insert(decl);
            for (std::string sentinel : mods.CallSentinels) {
                sentinel_strs.insert(sentinel);
                functionBodies[decl] += ("    " + COVER_SENTINEL_PREFIX + sentinel + " = 1;\n").str();
            }
        }
    }

    // PostCall requires postcondition in main func
    std::string postcallcheck;
    for (std::pair<FunctionDecl const*, std::set<std::string>> PCC : PostCallChecks) {
        postcallcheck += ("(!" + COVER_SENTINEL_PREFIX + PCC.first->getNameAsString() + " || (").str();
        for (std::string target : PCC.second) {
            postcallcheck += ("TERM(" + COVER_SENTINEL_PREFIX + target + ", \"" + PCC.first->getNameAsString() + ": call(_tag)!(" + target + ")\") && ").str();
        }
        postcallcheck += "true)) && ";
    }
    if (!postcallcheck.empty()) postcallcheck += "true";

    // The backend pass renames the original main to CoVer_RealMain and this one to main
    std::string mainFunc = "extern \"C\" int CoVer_RealMain(int argc, char** argv);\n"
                           "extern \"C\" int CoVer_GeneratedMain(int argc, char** argv)";
    if (!postcallcheck.empty()) {
        // Need to check for postcalls
        mainFunc += " post(cr::check{}(" + postcallcheck + "))";
    }
    mainFunc += " { int rc = CoVer_RealMain(argc, argv); COVER_EXIT_SENTINEL = 1; return rc; }\n";
    R.InsertTextAfter(SM.getLocForEndOfFile(SM.getMainFileID()), mainFunc);

    // Add includes: orig file, ContractReport.hpp
    R.InsertTextBefore(SM.getLocForStartOfFile(SM.getMainFileID()), "#include \"" + SM.getFileEntryForID(SM.getMainFileID())->tryGetRealPathName().str() + "\"\n");
    R.InsertTextBefore(SM.getLocForStartOfFile(SM.getMainFileID()), "#include \"ContractReport.hpp\"\n");

    // Add global sentinel values
    R.InsertTextBefore(SM.getLocForStartOfFile(SM.getMainFileID()), "#include <cstdint>\n");
    R.InsertTextAfter(SM.getLocForStartOfFile(SM.getMainFileID()), "int8_t COVER_EXIT_SENTINEL = 0;\n");
    for (std::string sentinel : sentinel_strs) {
        R.InsertTextAfter(SM.getLocForStartOfFile(SM.getMainFileID()), ("int8_t " + COVER_SENTINEL_PREFIX + sentinel + " = 0;\n").str());
    }

    // Rename functions to wrapper names, add call to orig, and add empty braces to turn into definition
    // Also, generate wrapfile
    std::ofstream backendfile(output_path + "/cover_wrapfile");
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
        if (DeclToPreConds.contains(decl)) rewriteDeclStr << " pre(cr::check{}(" << DeclToPreConds[decl] << "))";
        rewriteDeclStr << "asm(\"CoVer_Wrapper_" + mangledName + "\")";
        rewriteDeclStr << ";\n";
        R.InsertTextBefore(decl->getBeginLoc(), rewriteDeclStr.str());

        // Insert modified function body with forward call
        R.InsertTextAfter(LOC_PRIOR_SEMI(CI, SM, decl), " {\n" + functionBodies[decl] + "    " + buildForwardingCall(decl, CI->getASTContext()) + "\n}");
    }

    // Copy in the report function and output ContractReport header
    const char reportUtil[] = {
        #embed "../Templates/ContractReport.hpp"
        , '\0'
    };
    std::error_code EC;
    raw_fd_ostream out_rep(output_path + "/ContractReport.hpp", EC, sys::fs::OF_Text);
    out_rep << reportUtil << "\n";
    const char reportFunc[] = {
        #embed "../Templates/ReportFunc.cpp"
        , '\0'
    };
    R.InsertTextAfter(SM.getLocForEndOfFile(SM.getMainFileID()), reportFunc);

    // Perform output for include file
    FileID FID = SM.getMainFileID();
    raw_fd_ostream Out(output_path + "/include.cpp", EC, sys::fs::OF_Text);
    R.getEditBuffer(FID).write(Out);
}

}

export namespace ContractConverter {
    void Convert(ContractInfo DB, CompilerInstance& _CI, std::string output_path) {
        CI = &_CI;
        for (auto& [Decl, Data] : DB.Contracts) {
            errs() << "Converting contract for " << Decl->getDeclName() << "\n";
            if (Data.Pre) {
                DeclToPreConds[Decl] = constructFormula(Data.Pre, true, Decl, DB);
            }
            if (Data.Post) {
                constructFormula(Data.Post, false, Decl, DB);
            }
        }
        performOutput(output_path);
    }
}
