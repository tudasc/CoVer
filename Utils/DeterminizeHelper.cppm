export module DeterminizeHelper;
import std;
import LLVMModule;

using namespace llvm;

// Makes the analysis independent of how the module was loaded: A deterministic order of the functions, blocks, use lists
// and debug info of the module, and of IR values for containers whose iteration order affects the analysis.
// Ordering by address depends on the allocation order of the values, which differs e.g. between bitcode and textual IR input.
namespace DeterminizeHelper {

DenseMap<Value const*, unsigned> order;

void numberConstant(Constant const* C) {
    if (isa<GlobalValue>(C) || order.contains(C)) return;
    order[C] = order.size() + 1;
    for (Use const& U : C->operands()) {
        if (Constant const* Op = dyn_cast<Constant>(U.get())) numberConstant(Op);
    }
}

// Position of V. Values not numbered yet (e.g. constants created by the analysis) are numbered when first seen,
// which is deterministic as long as the analysis itself is
export unsigned of(Value const* V) {
    auto [it, inserted] = order.try_emplace(V, order.size() + 1);
    return it->second;
}

export struct Less {
    bool operator()(Value const* A, Value const* B) const {
        return A != B && of(A) < of(B);
    }
    template <typename T>
    bool operator()(std::pair<T, int> const& A, std::pair<T, int> const& B) const {
        if (A.first != B.first) return (*this)(A.first, B.first);
        return A.second < B.second;
    }
};

void sortBlocks(llvm::Function &F) {
  if (F.isDeclaration() || F.size() <= 1)
    return;

  llvm::ReversePostOrderTraversal<llvm::Function *> RPOT(&F);
  llvm::BasicBlock *Prev = nullptr;
  for (llvm::BasicBlock *BB : RPOT) {
    if (Prev)
      BB->moveAfter(Prev);
    Prev = BB;
  }
}

// Numbers the values of M in module order and sorts all use lists by the position of their users.
// The function list and the blocks of each function must already be in a deterministic order.
void numberValues(Module& M) {
    order.clear();
    for (GlobalValue const& GV : M.global_values()) order[&GV] = order.size() + 1;
    for (GlobalVariable const& GV : M.globals()) {
        if (GV.hasInitializer()) numberConstant(GV.getInitializer());
    }
    for (Function const& F : M) {
        for (Argument const& A : F.args()) order[&A] = order.size() + 1;
        for (Instruction const& I : instructions(F)) {
            order[&I] = order.size() + 1;
            for (Use const& U : I.operands()) {
                if (Constant const* C = dyn_cast<Constant>(U.get())) numberConstant(C);
            }
        }
    }

    // Use lists in the order of their users, e.g. to visit the callsites of a function in a deterministic order
    auto useLess = [](Use const& L, Use const& R) {
        if (L.getUser() != R.getUser()) return of(L.getUser()) < of(R.getUser());
        return L.getOperandNo() < R.getOperandNo();
    };
    for (GlobalValue& GV : M.global_values()) GV.sortUseList(useLess);
    for (Function& F : M) {
        for (Argument& A : F.args()) A.sortUseList(useLess);
        for (Instruction& I : instructions(F)) I.sortUseList(useLess);
    }
}

// Sorts the functions, blocks, use lists and debug info of M, and numbers its values for Less
export void determinize(Module& M) {
    M.getFunctionList().sort([](const llvm::Function &A, const llvm::Function &B) {
        return A.getName() < B.getName();
    });
    for (Function& F : M) {
        sortBlocks(F);
        // Position in the function is index for use list
        DenseMap<BasicBlock const*, int> Order;
        int Idx = 0;
        for (BasicBlock const& BB : F) Order[&BB] = Idx++;

        auto predKey = [&](const Use &U) -> unsigned {
            // A block value's users are terminators; map each back to its parent (the pred).
            if (Instruction* I = dyn_cast<Instruction>(U.getUser())) return Order.lookup(I->getParent());
            return std::numeric_limits<unsigned>::max(); // blockaddress / non-terminator users sink to the end
        };

        for (BasicBlock &BB : F) {
            BB.sortUseList([&](const Use &L, const Use &R) {
                return predKey(L) < predKey(R);
            });
        }
    }
    numberValues(M);

    NamedMDNode *CUs = M.getNamedMetadata("llvm.dbg.cu");
    SmallVector<MDNode *, 8> Nodes(CUs->operands());

    llvm::sort(Nodes, [](MDNode *A, MDNode *B) {
        DICompileUnit* CA = cast<DICompileUnit>(A);
        DICompileUnit* CB = cast<DICompileUnit>(B);
        return CA->getFile()->getFilename() < CB->getFile()->getFilename();
    });

    CUs->clearOperands();
    for (MDNode *N : Nodes)
        CUs->addOperand(N);

    for (DICompileUnit *CU : M.debug_compile_units()) {
        if (!CU->getGlobalVariables()) continue;
        SmallVector<Metadata *, 16> GVs(CU->getGlobalVariables()->operands());
        llvm::sort(GVs, [](Metadata *A, Metadata *B) {
            DIGlobalVariable* GA = cast<DIGlobalVariableExpression>(A)->getVariable();
            DIGlobalVariable* GB = cast<DIGlobalVariableExpression>(B)->getVariable();
            return GA->getName() < GB->getName();
        });
        CU->replaceGlobalVariables(MDTuple::get(M.getContext(), GVs));
    }
}

}
