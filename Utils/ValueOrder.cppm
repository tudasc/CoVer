export module ValueOrder;
import std;
import LLVMModule;

using namespace llvm;

// A deterministic order of IR values, for containers whose iteration order affects the analysis.
// Ordering by address depends on the allocation order of the values, which differs e.g. between bitcode and textual IR input.
namespace ValueOrder {

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

// Numbers the values of M in module order and sorts all use lists by the position of their users.
// The function list and the blocks of each function must already be in a deterministic order.
export void determinize(Module& M) {
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

}
