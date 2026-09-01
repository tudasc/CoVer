module;

#include <clang/AST/ASTContext.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclarationName.h>
#include <clang/Basic/IdentifierTable.h>
#include <clang/Basic/SourceLocation.h>
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
#include <ranges>
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
    std::map<std::string,std::set<int>> CallSentinels;
    std::set<std::string> PreCallChecks;
    std::set<std::string> PostAllocProcessing;
    std::set<std::string> PostFreeProcessing;
};

std::string PostCallChecks;

struct ReleaseInfo {
    std::set<FunctionDecl const*> forbFuncs;
    std::set<FunctionDecl const*> relFuncs;
    std::string relStr;
    std::string origExpr;
};
std::map<FunctionDecl const*, ReleaseInfo> ReleaseChecks;

#define LOC_PRIOR_SEMI(CI, SM, decl) (Lexer::getLocForEndOfToken(SM.getExpansionRange(decl->getSourceRange()).getEnd(), 0, SM, CI->getASTContext().getLangOpts()))

constexpr std::string_view COVER_OPTMP_PREFIX = "COVER_TMP_";

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
            return FD->getCanonicalDecl();
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

  return Name + "(" + Args + ");";
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

std::set<FunctionDecl const*> createSentinelsForCallOp(std::shared_ptr<CallOperation const> const& cOP, ContractInfo const& DB) {
    if (cOP->type() == FormulaType::CALL) {
        FunctionDecl const* target_func = lookupDecl(cOP->Function);
        if (target_func) DeclToMods[target_func].CallSentinels[cOP->Function] = {};
        return {target_func};
    } else {
        for (FunctionDecl const* target : DB.TagsToDecl.at(cOP->Function)) {
            std::set<int> indices;
            for (TagUnit const& tag : DB.DeclToTags.at(target)) {
                if (tag.tag != cOP->Function) continue;
                if (tag.param) indices.insert(*tag.param);
            }
            DeclToMods[target].CallSentinels[cOP->Function] = indices;
        }
        return DB.TagsToDecl.at(cOP->Function);
    }
}

enum struct ConstructMode { PRE, POSTCALL, RELEASE };
std::string constructFormula(std::shared_ptr<ContractFormula> const& form, ConstructMode mode, FunctionDecl const* decl, ContractInfo const& DB) {
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
            case FormulaType::ALLOC: {
                std::shared_ptr<AllocOperation const> aOP = std::static_pointer_cast<AllocOperation const>(expr->OP);
                if (mode == ConstructMode::PRE) {
                    return ("TERM(" + COVER_OPTMP_PREFIX + "Allocs.contains((std::uintptr_t)" + decl->getParamDecl(aOP->contrP)->getNameAsString() + "), \"PRE{" + expr->ExprStr + "}\")").str();
                } else {
                    std::string base_ptr = aOP->contrP == 99 ? "_ret" : decl->getParamDecl(aOP->contrP)->getNameAsString();
                    std::string allocSizeStr;
                    std::shared_ptr<MathExpr> cur = aOP->size;
                    while (cur) {
                        std::string value = cur->isArg ? decl->getParamDecl(cur->value)->getNameAsString() : std::to_string(cur->value);
                        allocSizeStr += value;
                        if (cur->type == MathType::UNARY_VALUE) break;
                        allocSizeStr += "*";
                        cur = cur->other;
                    }
                    std::string storeAlloc = ("    " + COVER_OPTMP_PREFIX + "Allocs.insert((std::uintptr_t)" + base_ptr + ", (std::uintptr_t)" + allocSizeStr + ");").str();
                    DeclToMods[decl].PostAllocProcessing.insert(storeAlloc);
                    return "true";
                }
                break;
            }
            case FormulaType::FREE:
                if (mode == ConstructMode::PRE) {
                    llvm_unreachable("Did not expect freeop in precondition!");
                } else {
                    std::shared_ptr<FreeOperation const> fOP = std::static_pointer_cast<FreeOperation const>(expr->OP);
                    std::string base_ptr = fOP->contrP == 99 ? "_ret" : decl->getParamDecl(fOP->contrP)->getNameAsString();
                    std::string eraseCall = ("    " + COVER_OPTMP_PREFIX + "Allocs.erase((std::uintptr_t)" + base_ptr + ");").str();
                    DeclToMods[decl].PostFreeProcessing.insert(eraseCall);
                    return "true";
                }
                break;
            case FormulaType::CALL:
            case FormulaType::CALLTAG: {
                std::shared_ptr<CallOperation const> cOP = std::static_pointer_cast<CallOperation const>(expr->OP);
                createSentinelsForCallOp(cOP, DB);
                if (mode == ConstructMode::PRE) {
                    DeclToMods[decl].PreCallChecks.insert(cOP->Function);
                    // Currently building precond, so need to return the sentinel as the boolean expr
                    std::string callCheck = (COVER_OPTMP_PREFIX + "Callsites_" + cOP->Function).str();
                    // Also check params
                    for (CallParam const& param : cOP->Params) {
                        callCheck += (" && " + COVER_OPTMP_PREFIX + "Callsites_" + cOP->Function).str();
                        if (!param.callPisTagVar) {
                            callCheck += ".checkMatchParam(" + std::to_string(param.callP) + ", (uintptr_t)" + decl->getParamDecl(param.contrP)->getNameAsString();
                        } else {
                            callCheck += ".checkMatchTag((uintptr_t)" + decl->getParamDecl(param.contrP)->getNameAsString();
                        }
                        callCheck += ", " + std::to_string((int)param.contrParamAccess) + ")";
                    }
                    return "TERM(" + callCheck + ", \"PRE{" + expr->ExprStr + "}\")";
                } else if (mode == ConstructMode::POSTCALL) {
                    DeclToMods[decl].CallSentinels[decl->getNameAsString()] = {};
                    return ("TERM((!" + COVER_OPTMP_PREFIX + "Callsites_" + decl->getNameAsString() + " || " + COVER_OPTMP_PREFIX + "Callsites_" + cOP->Function + "), \"POST{" + expr->ExprStr + "}\")").str();
                } else return "true";
                break;
            }
            case FormulaType::RELEASE: {
                if (mode == ConstructMode::PRE) llvm_unreachable("Unexpected releaseOp in precondition!");
                if (mode == ConstructMode::POSTCALL) return "true"; // Dont interfere here, current return will be used for supplier not forbop
                std::shared_ptr<ReleaseOperation const> rOP = std::static_pointer_cast<ReleaseOperation const>(expr->OP);
                switch (rOP->Forbidden->type()) {
                    case FormulaType::READ:
                    case FormulaType::WRITE:
                        #warning TODO
                        return "true";
                    case FormulaType::CALL:
                    case FormulaType::CALLTAG: {
                        // Ensure CallSentinels exist for forbOp
                        std::set<FunctionDecl const*> forbCalls = createSentinelsForCallOp(std::static_pointer_cast<CallOperation const>(rOP->Forbidden), DB);
                        ReleaseChecks[decl].forbFuncs.insert(forbCalls.begin(), forbCalls.end());
                        ReleaseChecks[decl].origExpr = expr->ExprStr;
                        break;
                    }
                    default: llvm_unreachable("Unexpected forbidden operation!");
                }
                // For relop, dont need sentinels. They just need to delete the matching funccalls on match
                std::shared_ptr<CallOperation const> rcOp = std::static_pointer_cast<CallOperation const>(rOP->Until);
                if (rcOp->type() == FormulaType::CALL) ReleaseChecks[decl].relFuncs.insert(lookupDecl(rcOp->Function));
                else ReleaseChecks[decl].relFuncs.insert(DB.TagsToDecl.at(rcOp->Function).begin(), DB.TagsToDecl.at(rcOp->Function).end());
                return "false"; // Later, at forb func, adds to "!supplier || false. TODO Add param support"
            }
            case FormulaType::PARAM: {
                std::shared_ptr<ParamOperation const> pOP = std::static_pointer_cast<ParamOperation const>(expr->OP);
                if (mode == ConstructMode::PRE) {
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
            res += act_sep + constructFormula(child, mode, decl, DB);
            act_sep = sep;
        }
        return "(" + res + postfix + ")";
    }
}

std::string createCallsiteParamStr(FunctionDecl const* decl, std::string name, std::set<int> indices) {
    std::string res;
    res += ("    " + COVER_OPTMP_PREFIX + "Callsites_" + name + ".addCallsite({{").str();
    for (int i = 0; i+1 < decl->getNumParams(); i++) {
        res += "(uintptr_t)" + decl->getParamDecl(i)->getNameAsString() + ", ";
    }
    if (decl->getNumParams() != 0) res += "(uintptr_t)" + decl->getParamDecl(decl->getNumParams() - 1)->getNameAsString();
    res += "}, {";
    for (int idx : indices) {
        res += std::to_string(idx) + ", ";
    }
    res += "}});\n";
    return res;
}

void performOutput(std::string output_path) {
    SourceManager& SM = CI->getSourceManager();
    Rewriter R(SM, CI->getLangOpts());

    std::filesystem::create_directories(output_path);

    std::set<std::string> sentinel_strs;
    std::set<FunctionDecl const*> declRename;
    std::map<FunctionDecl const*,std::string> functionBodiesPre;
    std::map<FunctionDecl const*,std::string> functionBodiesPost;

    // Apply modifications per declaration
    for (auto& [decl, mods] : DeclToMods) {
        declRename.insert(decl);

        // PreCallSentinel needs wrap + set of sentinel value + decl of sentinel value
        if (!mods.CallSentinels.empty()) {
            for (std::pair<std::string,std::set<int>> sentinel : mods.CallSentinels) {
                sentinel_strs.insert(sentinel.first);
                functionBodiesPre[decl] += createCallsiteParamStr(decl, sentinel.first, sentinel.second);
            }
        }

        // PostFreeProcessing removes alloc'd value
        if (!mods.PostFreeProcessing.empty()) {            
            for (std::string removeAlloc : mods.PostFreeProcessing) {
                functionBodiesPost[decl] += removeAlloc + "\n";
            }
        }

        // PostAllocProcessing needs to compute alloc size and store it
        if (!mods.PostAllocProcessing.empty()) {
            for (std::string storeAlloc : mods.PostAllocProcessing) {
                functionBodiesPost[decl] += storeAlloc + "\n";
            }
        }
    }

    // Add contracts to forbidden funcs for release
    for (auto& [supplier, info] : ReleaseChecks) {
        functionBodiesPre[supplier] += createCallsiteParamStr(supplier, "REL" + supplier->getNameAsString(), {});
        sentinel_strs.insert("REL" + supplier->getNameAsString());
        for (FunctionDecl const* forbF : info.forbFuncs) {
            if (!DeclToPreConds[forbF].empty()) DeclToPreConds[forbF] += " && ";
            DeclToPreConds[forbF] += ("TERM((!" + COVER_OPTMP_PREFIX + "Callsites_REL" + supplier->getNameAsString() + " || " + info.relStr + "), \"POST{" + info.origExpr + "}\")").str();
        }
        for (FunctionDecl const* relF : info.relFuncs) {
            functionBodiesPost[relF] += ("    " + COVER_OPTMP_PREFIX + "Callsites_REL" + supplier->getNameAsString() + ".clear();\n").str();
        }
    }

    // Also wrap all funcs that have a precond/postcond
    for (FunctionDecl const* decl : DeclToPreConds | std::views::keys) declRename.insert(decl);

    // The backend pass renames the original main to CoVer_RealMain and this one to main
    std::string mainFunc = "\nextern \"C\" int CoVer_RealMain(int argc, char** argv);\n"
                           "extern \"C\" int CoVer_GeneratedMain(int argc, char** argv)";
    if (!PostCallChecks.empty()) {
        // Need to check for postcalls
        mainFunc += " post(cr::check{}(" + PostCallChecks + "))";
    }
    mainFunc += " { int rc = CoVer_RealMain(argc, argv); COVER_EXIT_SENTINEL = 1; return rc; }\n";
    R.InsertTextAfter(SM.getLocForEndOfFile(SM.getMainFileID()), mainFunc);

    // Add includes: orig file, util headers
    R.InsertTextBefore(SM.getLocForStartOfFile(SM.getMainFileID()), "#include \"" + SM.getFileEntryForID(SM.getMainFileID())->tryGetRealPathName().str() + "\"\n");

    // Add global sentinel values and operation stores
    R.InsertTextBefore(SM.getLocForStartOfFile(SM.getMainFileID()), "#include <cstdint>\n");
    R.InsertTextBefore(SM.getLocForStartOfFile(SM.getMainFileID()), "#include <map>\n");
    R.InsertTextAfter(SM.getLocForStartOfFile(SM.getMainFileID()), "int8_t COVER_EXIT_SENTINEL = 0;\n");
    R.InsertTextAfter(SM.getLocForStartOfFile(SM.getMainFileID()), ("range_set " + COVER_OPTMP_PREFIX + "Allocs" + ";\n").str());
    for (std::string sentinel : sentinel_strs) {
        R.InsertTextAfter(SM.getLocForStartOfFile(SM.getMainFileID()), ("FuncCallsites " + COVER_OPTMP_PREFIX + "Callsites_" + sentinel + ";\n").str());
    }

    // Rename functions to wrapper names, add call to orig, and add empty braces to turn into definition
    // Also, generate wrapfile
    std::ofstream backendfile(output_path + "/cover_wrapfile");
    for (FunctionDecl const* decl : declRename) {
        // Remove CoVer contract annotation, if it exists
        if (AnnotateAttr* AA = decl->getAttr<AnnotateAttr>())
            R.RemoveText(SM.getExpansionRange(AA->getRange()));

        functionBodiesPre[decl];
        functionBodiesPost[decl];
        std::string mangledName = getMangledName(decl, CI->getASTContext());
        backendfile << mangledName << "\n";

        // Remove function definition
        if (decl->isThisDeclarationADefinition()) {
            SourceRange range = decl->getBody()->getSourceRange();
            SourceLocation end = Lexer::getLocForEndOfToken( range.getEnd(), 0, R.getSourceMgr(), R.getLangOpts());
            R.RemoveText(SourceRange(range.getBegin(), end));
        }

        // Rename to wrapper func
        R.ReplaceText(decl->getNameInfo().getSourceRange(), "CoVer_Wrapper_" + mangledName);

        // Get declaration and insert with asm + contract
        std::string decl_str = R.getRewrittenText(SM.getExpansionRange(decl->getSourceRange()));
        std::stringstream rewriteDeclStr;
        rewriteDeclStr << decl_str;
        if (DeclToPreConds.contains(decl)) rewriteDeclStr << " pre(cr::check{}(" << DeclToPreConds[decl] << "))";
        rewriteDeclStr << "asm(\"CoVer_Wrapper_" + mangledName + "\")";
        rewriteDeclStr << ";\n";
        R.InsertTextBefore(decl->getBeginLoc(), rewriteDeclStr.str());

        // Insert modified function body with forward call
        std::string fullFunctionBody = "{\n" +
                                       functionBodiesPre[decl] +
                                       (decl->getReturnType()->isVoidType() ? "    " + buildForwardingCall(decl, CI->getASTContext()) + "\n" :
                                                                              "    auto _ret = " + buildForwardingCall(decl, CI->getASTContext()) + "\n") +
                                       functionBodiesPost[decl] +
                                       (decl->getReturnType()->isVoidType() ? "" : "    return _ret;\n") +
                                       "}";
        R.InsertTextAfter(LOC_PRIOR_SEMI(CI, SM, decl), fullFunctionBody);
    }

    // Copy in the report function and output utility headers
    std::error_code EC;
    const char rangeSet[] = {
        #embed "../Templates/RangeSet.hpp"
        , '\0'
    };
    const char callsiteUtil[] = {
        #embed "../Templates/Callsites.hpp"
        , '\0'
    };
    const char reportUtil[] = {
        #embed "../Templates/ContractReport.hpp"
        , '\0'
    };
    const std::pair<std::string, const char *> templates[] = {
        {"RangeSet.hpp",       rangeSet},
        {"Callsites.hpp",      callsiteUtil},
        {"ContractReport.hpp", reportUtil},
    };
    for (auto &[name, data] : templates) {
        std::error_code EC;
        raw_fd_ostream outfile(output_path + "/" + name, EC, sys::fs::OF_Text);
        outfile << data << "\n";
        R.InsertTextBefore(SM.getLocForStartOfFile(SM.getMainFileID()), "#include \"" + name + "\"\n");
    }

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
                DeclToPreConds[Decl] = constructFormula(Data.Pre, ConstructMode::PRE, Decl, DB);
            }
            if (Data.Post) {
                PostCallChecks += constructFormula(Data.Post, ConstructMode::POSTCALL, Decl, DB) + " && ";
                std::string newRel = constructFormula(Data.Post, ConstructMode::RELEASE, Decl, DB);
                if (ReleaseChecks.contains(Decl)) ReleaseChecks[Decl].relStr = newRel; // Avoid adding useless conditions if decl did not contain relops
            }
        }
        if (!PostCallChecks.empty()) PostCallChecks.erase(PostCallChecks.size() - 4); // Remove trailing " && "
        performOutput(output_path);
    }
}
