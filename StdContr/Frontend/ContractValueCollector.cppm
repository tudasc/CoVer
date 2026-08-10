module;

#include <clang/AST/APValue.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/raw_ostream.h>

#include <string>

export module ContractValueCollector;
import ContractInfo;

using namespace clang;
using namespace llvm;

namespace {

constexpr std::string_view VALUE_INFO_PREFIX = "ContractValueInfo_";

// Peels off parentheses, implicit conversions and every explicit cast, so that
// CONTRACT_VALUE_PAIR(x, (MPI_Status*)0) ends up at the bare 0.
const Expr* stripCasts(const Expr* E) {
    while (E) {
        const Expr* Prev = E;
        E = E->IgnoreParenImpCasts();
        if (const ExplicitCastExpr* CE = dyn_cast<ExplicitCastExpr>(E)) {
            E = CE->getSubExpr();
        }
        if (E == Prev) break;
    }
    return E;
}

/// The expression as the user wrote it, minus the (void*) the macro adds around it.
const Expr* stripMacroCast(const Expr* E) {
    E = E->IgnoreParenImpCasts();
    if (const CStyleCastExpr* CE = dyn_cast<CStyleCastExpr>(E)) {
        if (CE->getType()->isVoidPointerType()) return CE->getSubExpr()->IgnoreParenImpCasts();
    }
    return E;
}

std::string printExpr(const Expr* E, const ASTContext& Ctx) {
    std::string Out;
    raw_string_ostream OS(Out);
    E->printPretty(OS, nullptr, Ctx.getPrintingPolicy());
    return Out;
}

std::string printLoc(SourceLocation Loc, const SourceManager& SM) {
    if (Loc.isInvalid()) return "<unknown>";
    return Loc.printToString(SM);
}

// Two's complement of the payload in pointer width, which is what the value looks like in IR.
std::string toRawPointer(int64_t Value, const ASTContext& Ctx) {
    unsigned Width = Ctx.getTypeSize(Ctx.VoidPtrTy);
    uint64_t Raw = static_cast<uint64_t>(Value);
    if (Width < 64) Raw &= (uint64_t(1) << Width) - 1;
    std::string Out;
    raw_string_ostream OS(Out);
    OS << format_hex(Raw, Width / 4 + 2);
    return Out;
}

// Extracts the identifier the pair was registered under, i.e. the "MPI_PROC_NULL" of the pair.
bool extractIdentifier(const Expr* E, std::string& Out) {
    if (const clang::StringLiteral* SL = dyn_cast<clang::StringLiteral>(E->IgnoreParenImpCasts())) {
        Out = SL->getString().str();
        return true;
    }
    return false;
}

// Resolves the payload to an integer, a pointer constant or a symbol address.
void extractValue(const Expr* Payload, const ASTContext& Ctx, ContractValue& Entry) {
    const Expr* Bare = stripCasts(Payload);
    const Expr* UserPayload = stripMacroCast(Payload);
    Entry.Expression = printExpr(Bare, Ctx);
    Entry.PayloadType = UserPayload->getType().getAsString(Ctx.getPrintingPolicy());
    // Where the constant is actually written down
    Entry.PayloadSpelling = printLoc(Ctx.getSourceManager().getSpellingLoc(Bare->getBeginLoc()), Ctx.getSourceManager());

    // Plain integer constants as well as pointers
    // spelled as a cast integer ((void*)1) resolve directly.
    if (Bare->getType()->isIntegerType()) {
        Expr::EvalResult ER;
        if (Bare->EvaluateAsInt(ER, Ctx) && ER.Val.isInt()) {
            Entry.Kind = UserPayload->getType()->isPointerType() ? ValueKind::PointerConstant : ValueKind::Integer;
            Entry.IntValue = ER.Val.getInt().getExtValue();
            Entry.RawPointer = toRawPointer(Entry.IntValue, Ctx);
            Entry.ValueText = std::to_string(Entry.IntValue);
            return;
        }
    }

    // Everything else has to be constant folded, such as &symbol payloads
    Expr::EvalResult ER;
    if (Payload->EvaluateAsRValue(ER, Ctx, /*InConstantContext=*/true)) {
        if (ER.Val.isInt()) {
            Entry.Kind = ValueKind::Integer;
            Entry.IntValue = ER.Val.getInt().getExtValue();
            Entry.RawPointer = toRawPointer(Entry.IntValue, Ctx);
            Entry.ValueText = std::to_string(Entry.IntValue);
            return;
        }
        if (ER.Val.isLValue()) {
            APValue::LValueBase Base = ER.Val.getLValueBase();
            int64_t Offset = ER.Val.getLValueOffset().getQuantity();
            if (!Base) {
                Entry.Kind = ValueKind::PointerConstant;
                Entry.IntValue = Offset;
                Entry.RawPointer = toRawPointer(Offset, Ctx);
                Entry.ValueText = std::to_string(Offset);
                return;
            }
            if (const ValueDecl* VD = Base.dyn_cast<const ValueDecl*>()) {
                Entry.Kind = ValueKind::SymbolAddress;
                Entry.Symbol = VD->getNameAsString();
                Entry.IntValue = Offset;
                Entry.ValueText = "&" + Entry.Symbol;
                if (Offset != 0) Entry.ValueText += (Offset > 0 ? " + " : " - ") + std::to_string(Offset > 0 ? Offset : -Offset);
                return;
            }
        }
    }

    Entry.Kind = ValueKind::Unresolved;
    Entry.ValueText = Entry.Expression;
}

// Recovers the __COUNTER__ suffix the macro appended to the global name.
int64_t extractCounter(StringRef Name) {
    StringRef Suffix = Name.drop_front(VALUE_INFO_PREFIX.size());
    int64_t ID = -1;
    if (Suffix.getAsInteger(10, ID)) return -1;
    return ID;
}

} // namespace

namespace ContractValueCollector {

export class VarDeclVisitor : public RecursiveASTVisitor<VarDeclVisitor> {
    public:
        VarDeclVisitor(ASTContext& _Ctx, ContractInfo& _info) : Ctx(_Ctx), contractInfo(_info) {}
        bool VisitVarDecl(VarDecl* VD) {
            if (!VD->getIdentifier()) return true;
            if (!VD->getName().starts_with(VALUE_INFO_PREFIX)) return true;

            // Guard against unrelated globals that happen to share the prefix.
            const RecordType* RT = VD->getType().getCanonicalType()->getAs<RecordType>();

            const Expr* Init = VD->getAnyInitializer();
            if (!Init) return true;
            const InitListExpr* ILE = dyn_cast<InitListExpr>(Init->IgnoreParenImpCasts());
            if (ILE && !ILE->isSemanticForm() && ILE->getSemanticForm()) ILE = ILE->getSemanticForm();
            if (!ILE || ILE->getNumInits() < 2) return true;

            ContractValue Entry;
            Entry.GlobalName = VD->getNameAsString();
            Entry.CounterID = extractCounter(VD->getName());

            if (!extractIdentifier(ILE->getInit(0), Entry.Identifier)) {
                errs() << "Could not get identifier for contract value " << VD->getName() << "!\n";
                return true;
            }

            contractInfo.ContractValToGlobal[Entry.Identifier] = VD->getNameAsString();
            extractValue(ILE->getInit(1), Ctx, Entry);

            // Entries.push_back(std::move(Entry));
            return true;
        }
    
    private:
        ASTContext& Ctx;
        ContractInfo& contractInfo;
};

}
