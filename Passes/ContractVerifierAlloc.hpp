#pragma once

#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include "llvm/IR/PassManager.h"
#include <algorithm>
#include <llvm/IR/InstrTypes.h>
#include <map>
#include <ranges>
#include <set>
#include <utility>

using namespace ContractTree;

namespace llvm {

class ContractVerifierAllocPass : public PassInfoMixin<ContractVerifierAllocPass> {
    public:
        enum struct AllocStatusVal { ALLOC, UNDEF, ERROR };
        class AllocStatus {
            public: struct AllocInfo;
            private: std::map<Value const*, AllocInfo> candidate_set;
            public:
                struct AllocInfo {
                    std::set<Value const*> parents;
                    std::set<Value const*> children;
                    ParamAccess acc;
                    bool operator==(const AllocInfo other) const {
                        return true;
                    }
                };
                AllocStatusVal CurVal = AllocStatusVal::UNDEF;
                AllocInfo const& getAllocInfo(Value const* V) const { return candidate_set.at(V); }
                bool hasAllocInfo(Value const* V) const { return candidate_set.contains(V); }
                auto candidates() const {
                    return std::ranges::subrange(candidate_set.cbegin(), candidate_set.cend());
                }
                void addCopy(Value const* Copy, Value const* Parent, ParamAccess acc) {
                    if (candidate_set.contains(Copy)) {
                        candidate_set[Copy].parents.insert(Parent);
                    } else {
                        candidate_set[Copy] = {{Parent}, {}, acc};
                        candidate_set[Parent].children.insert(Copy);
                    }
                }
                void addAllocatedValue(Value const* V, ParamAccess acc = ParamAccess::NORMAL) {
                    candidate_set[V] = {{}, {}, acc};
                }
                void freeValue(Value const* V) {
                    if (!hasAllocInfo(V)) return;
                    std::set<Value const*> to_erase = {V};
                    while (!to_erase.empty()) {
                        V = *to_erase.begin();
                        for (Value const* C : candidate_set[V].children) {
                            candidate_set[C].parents.erase(V);
                            if (candidate_set[C].parents.empty()) to_erase.insert(C);
                        }
                        candidate_set.erase(V);
                        to_erase.erase(V);
                    }
                }
                AllocStatus intersect(AllocStatus const& other) {
                    std::map<Value const*, AllocInfo> candidate_intersect;
                    for (std::pair<Value const*, AllocStatus::AllocInfo> Candidate : this->candidates()) {
                        if (other.hasAllocInfo(Candidate.first)) {
                            AllocInfo AIres;
                            AIres.acc = Candidate.second.acc;
                            AllocInfo AI = other.getAllocInfo(Candidate.first);
                            std::set_intersection(AI.children.begin(), AI.children.end(), Candidate.second.children.begin(), Candidate.second.children.end(), std::inserter(AIres.children, AIres.children.end()));
                            std::set_intersection(AI.parents.begin(), AI.parents.end(), Candidate.second.parents.begin(), Candidate.second.parents.end(), std::inserter(AIres.parents, AIres.parents.end()));
                            candidate_intersect[Candidate.first] = AIres;
                        }
                    }
                    AllocStatus res;
                    res.candidate_set = candidate_intersect;
                    res.CurVal = std::max(this->CurVal, other.CurVal);
                    return res;
                }
        };
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);

    private:
        ContractManagerAnalysis::ContractDatabase* DB;
        ModuleAnalysisManager* MAM;

        std::map<const Value*, std::set<const AllocOperation*>> AllocFuncs;
        std::map<const Value*, std::set<const FreeOperation*>> FreeFuncs;

        AllocStatus transferAllocStat(AllocStatus s, const Instruction* I, void* data);
        std::pair<AllocStatus,bool> mergeAllocStat(AllocStatus prev, AllocStatus cur, const Instruction* I, void* data);

        AllocStatusVal checkAllocReq(const AllocOperation* AllocOp, Module const& M, const Function* F, std::string& err);

};

} // namespace llvm
