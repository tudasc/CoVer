#pragma once

#include "ContractTree.hpp"
#include "ContractManager.hpp"
#include <algorithm>
#include <flat_map>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <utility>

import LLVMModule;

using namespace ContractTree;

namespace llvm {

class ContractVerifierAllocPass : public PassInfoMixin<ContractVerifierAllocPass> {
    public:
        enum struct AllocStatusVal { ALLOC, UNDEF, ERROR };
        class AllocStatus {
            public:
                struct AllocInfo {
                    std::set<Value const*> parents;
                    std::set<Value const*> children;
                    ParamAccess acc;
                };
                // Infos are shared between copies of the candidates, so they are replaced instead of modified
                using InfoPtr = std::shared_ptr<AllocInfo const>;
                using CandidateMap = std::flat_map<Value const*, InfoPtr>;
            private:
                // Copy-on-write: The status is copied for every instruction, but the candidates rarely change
                std::shared_ptr<CandidateMap> candidate_set = std::make_shared<CandidateMap>();
                // Candidates for modification, copied first if shared with another status
                CandidateMap& mutCandidates() {
                    if (candidate_set.use_count() > 1) candidate_set = std::make_shared<CandidateMap>(*candidate_set);
                    return *candidate_set;
                }
                // Replace the info of V by a modifiable copy
                static AllocInfo& replaceInfo(CandidateMap& candidates, Value const* V) {
                    std::shared_ptr<AllocInfo> info = std::make_shared<AllocInfo>(*candidates.at(V));
                    candidates[V] = info;
                    return *info;
                }
                // Whether a contains all candidates of b, with all their parents and children
                static bool includes(CandidateMap const& a, CandidateMap const& b) {
                    return std::ranges::all_of(b, [&](auto const& Candidate) {
                        auto it = a.find(Candidate.first);
                        if (it == a.end()) return false;
                        if (it->second == Candidate.second) return true;
                        AllocInfo const &infoA = *it->second, &infoB = *Candidate.second;
                        return infoA.acc == infoB.acc &&
                               std::ranges::includes(infoA.parents, infoB.parents) &&
                               std::ranges::includes(infoA.children, infoB.children);
                    });
                }
            public:
                AllocStatusVal CurVal = AllocStatusVal::UNDEF;
                AllocInfo const& getAllocInfo(Value const* V) const { return *candidate_set->at(V); }
                bool hasAllocInfo(Value const* V) const { return candidate_set->contains(V); }
                size_t size() const { return candidate_set->size(); }
                CandidateMap const& candidates() const { return *candidate_set; }
                bool sameCandidates(AllocStatus const& other) const { return candidate_set == other.candidate_set; }
                void addCopy(Value const* Copy, Value const* Parent, ParamAccess acc) {
                    if (hasAllocInfo(Copy) && getAllocInfo(Copy).parents.contains(Parent)) return; // Nothing new
                    CandidateMap& candidates = mutCandidates();
                    if (candidates.contains(Copy)) {
                        replaceInfo(candidates, Copy).parents.insert(Parent);
                    } else {
                        candidates[Copy] = std::make_shared<AllocInfo const>(AllocInfo{{Parent}, {}, acc});
                        replaceInfo(candidates, Parent).children.insert(Copy);
                    }
                }
                void addAllocatedValue(Value const* V, ParamAccess acc = ParamAccess::NORMAL) {
                    if (hasAllocInfo(V)) {
                        AllocInfo const& info = getAllocInfo(V);
                        if (info.acc == acc && info.parents.empty() && info.children.empty()) return; // Nothing new
                    }
                    mutCandidates()[V] = std::make_shared<AllocInfo const>(AllocInfo{{}, {}, acc});
                }
                void freeValue(Value const* V) {
                    if (!hasAllocInfo(V)) return;
                    CandidateMap& candidates = mutCandidates();
                    std::set<Value const*> to_erase = {V};
                    while (!to_erase.empty()) {
                        V = *to_erase.begin();
                        to_erase.erase(V);
                        if (!candidates.contains(V)) continue; // Already freed
                        InfoPtr info = candidates.at(V); // Keep alive while V is erased
                        for (Value const* C : info->children) {
                            if (!candidates.contains(C)) continue;
                            AllocInfo& CInfo = replaceInfo(candidates, C);
                            CInfo.parents.erase(V);
                            if (CInfo.parents.empty()) to_erase.insert(C);
                        }
                        for (Value const* p : info->parents) freeValue(p);
                        candidates.erase(V);
                    }
                }
                // Add candidates of other not yet present
                void addCandidates(AllocStatus const& other) {
                    if (sameCandidates(other)) return;
                    for (auto const& [V, info] : other.candidates()) {
                        if (!hasAllocInfo(V)) mutCandidates().emplace(V, info);
                    }
                }
                AllocStatus intersect(AllocStatus const& other) const {
                    AllocStatus res;
                    res.CurVal = std::max(this->CurVal, other.CurVal);
                    // Keep sharing the candidates if the intersection is one of the inputs
                    if (sameCandidates(other) || includes(*other.candidate_set, *candidate_set)) {
                        res.candidate_set = candidate_set;
                        return res;
                    }
                    if (includes(*candidate_set, *other.candidate_set)) {
                        res.candidate_set = other.candidate_set;
                        return res;
                    }
                    res.candidate_set = std::make_shared<CandidateMap>();
                    for (auto const& [V, info] : this->candidates()) {
                        if (other.hasAllocInfo(V)) {
                            AllocInfo AIres;
                            AIres.acc = info->acc;
                            AllocInfo const& AI = other.getAllocInfo(V);
                            std::set_intersection(AI.children.begin(), AI.children.end(), info->children.begin(), info->children.end(), std::inserter(AIres.children, AIres.children.end()));
                            std::set_intersection(AI.parents.begin(), AI.parents.end(), info->parents.begin(), info->parents.end(), std::inserter(AIres.parents, AIres.parents.end()));
                            res.candidate_set->emplace_hint(res.candidate_set->end(), V, std::make_shared<AllocInfo const>(std::move(AIres)));
                        }
                    }
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
        std::pair<AllocStatus,bool> mergeAllocStat(AllocStatus const& prev, AllocStatus const& cur, const Instruction* I, void* data);
        AllocStatus callReturnAllocStat(AllocStatus caller, AllocStatus callee, const CallBase* CB, void* data);

        AllocStatusVal checkAllocReq(const AllocOperation* AllocOp, ContractManagerAnalysis::LinearizedContract const& C, ContractExpression const& Expr, Module const& M, const Function* F, std::string& err);

};

} // namespace llvm
