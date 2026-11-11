export module InstrumentAnnotations;
import std;
import LLVMModule;
import ContractPassUtility;
import BasicTypes;

using namespace llvm;

namespace {

// An alias annotation, as checked by the runtime annotation verifier
struct AliasSite {
    CallBase* annot; // Null once converted to access checks
    std::optional<std::string> ref;
    std::int32_t group;
    bool shouldAlias;
    std::vector<Instruction*> visitors; // The runtime calls that visit the annotation
};

// A function pointer annotation, as checked by the runtime annotation verifier
struct FPSite {
    std::optional<std::string> ref;
    std::vector<Function*> targets; // Aliases are resolved, they have the same address at runtime
};

// The source location of a runtime call as "file:line", which the runtime matches against the location of the call's return address.
// Calls without a location (e.g. annotations of arguments, placed at the function entry) get the location of their function
std::optional<std::string> ensureRef(Instruction* I) {
    if (!I->getDebugLoc()) {
        DISubprogram* SP = I->getFunction()->getSubprogram();
        if (!SP) return std::nullopt;
        I->setDebugLoc(DILocation::get(I->getContext(), SP->getScopeLine() ? SP->getScopeLine() : SP->getLine(), 0, SP));
    }
    DILocation const* loc = I->getDebugLoc().get();
    if (loc->getLine() == 0) return std::nullopt;
    std::filesystem::path file = std::filesystem::path(loc->getDirectory().str()) / loc->getFilename().str();
    return file.string() + ":" + std::to_string(loc->getLine());
}

// Collects the loads and stores accessing memory through V, following GEPs and casts. Each access is recorded with the annotated value it was reached from.
// Returns false if V is used in another way, i.e. it may be kept beyond the access (call argument, stored, merged with other values, ...)
bool collectAccesses(Value* V, Value* annotated, std::map<Instruction*, Value*>& accesses) {
    for (User* U : V->users()) {
        if (CallBase* CB = dyn_cast<CallBase>(U)) {
            if (CB->getCalledOperand()->getName().starts_with("CoVer_Annot")) continue;
            return false;
        }
        if (LoadInst* LI = dyn_cast<LoadInst>(U)) {
            accesses.try_emplace(LI, annotated);
        } else if (StoreInst* SI = dyn_cast<StoreInst>(U)) {
            if (SI->getValueOperand() == V) return false;
            accesses.try_emplace(SI, annotated);
        } else if (isa<GetElementPtrInst>(U) || isa<BitCastInst>(U) || isa<AddrSpaceCastInst>(U)) {
            if (!collectAccesses(U, annotated, accesses)) return false;
        } else if (!isa<ICmpInst>(U)) {
            return false;
        }
    }
    return true;
}

// The global holding str. It is named for str, so each string is only created once
Constant* createString(Module& M, std::string const& str) {
    std::string name = "CoVer_AnnotStr_" + str;
    if (GlobalVariable* existing = M.getNamedGlobal(name)) return existing;
    Constant* init = ConstantDataArray::getString(M.getContext(), str);
    GlobalVariable* GV = new GlobalVariable(M, init->getType(), true, GlobalValue::PrivateLinkage, init, name);
    GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
    return GV;
}

}

// Prepares the alias and function pointer annotations for the runtime annotation verifier.
// No-alias annotations of values that are only accessed are converted to checks at each access.
// All annotations are listed in a table, so the runtime can report annotations that were never visited
export class InstrumentAnnotationsPass : public PassInfoMixin<InstrumentAnnotationsPass> {
    public:
        PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM) {
            Basic_Types = MAM.getResult<BasicTypesAnalysis>(M);
            std::vector<AliasSite> aliasSites;
            for (User* U : M.getFunction("CoVer_AnnotAlias")->users()) {
                CallBase* CB = dyn_cast<CallBase>(U);
                if (!CB || CB->getCalledOperand()->getName() != "CoVer_AnnotAlias") continue;
                ConstantInt* shouldAlias = dyn_cast<ConstantInt>(CB->getArgOperand(1));
                ConstantInt* group = dyn_cast<ConstantInt>(CB->getArgOperand(2));
                aliasSites.push_back({CB, ensureRef(CB), static_cast<std::int32_t>(group->getSExtValue()), !shouldAlias->isZero(), {CB}});
            }
            std::vector<FPSite> fpSites;
            for (User* U : M.getFunction("CoVer_AnnotFP")->users()) {
                CallBase* CB = dyn_cast<CallBase>(U);
                if (!CB || CB->getCalledOperand()->getName() != "CoVer_AnnotFP") continue;
                FPSite site = {ensureRef(CB), {}};
                for (unsigned i = 2; i < CB->arg_size(); i++) {
                    if (Function* target = dyn_cast<Function>(CB->getArgOperand(i)->stripPointerCastsAndAliases())) site.targets.push_back(target);
                }
                fpSites.push_back(std::move(site));
            }
            if (aliasSites.empty() && fpSites.empty()) return PreservedAnalyses::all();

            convertAccessAnnotations(M, aliasSites);
            emitTable(M, aliasSites, fpSites);
            return PreservedAnalyses::none();
        }

    private:
        BasicTypesAnalysis::BasicTypes Basic_Types;

        // No-alias annotations whose value is only used to load or store are replaced by CoVer_AnnotAliasAccess, called directly before each such access.
        // The runtime checks the accessed value against the values of the group that are in use, but does not record it itself
        void convertAccessAnnotations(Module& M, std::vector<AliasSite>& sites) {
            // Classify first: The memory SSA used to find equivalent values is invalidated by the conversion
            struct Conversion {
                AliasSite* site;
                std::map<Instruction*, Value*> accesses;
            };
            std::vector<Conversion> conversions;
            for (AliasSite& site : sites) {
                if (site.shouldAlias) continue;
                Instruction* V = dyn_cast<Instruction>(site.annot->getArgOperand(0));
                if (!V || !V->getType()->isPointerTy()) continue;

                // For source level annotations, the uses load the variable separately: Also consider equivalent values, as the analysis does
                std::vector<Value*> instances = {V};
                for (Instruction& I : instructions(*V->getFunction())) {
                    if (&I != V && ContractPassUtility::equivalentValues(V, &I)) instances.push_back(&I);
                }
                Conversion conv = {&site, {}};
                bool onlyAccesses = std::ranges::all_of(instances, [&conv](Value* inst) { return collectAccesses(inst, inst, conv.accesses); });
                if (onlyAccesses && !conv.accesses.empty()) conversions.push_back(std::move(conv));
            }
            if (conversions.empty()) return;

            FunctionType* accessType = FunctionType::get(Basic_Types.Void_Type, {Basic_Types.Ptr_Type, Basic_Types.Int_Type}, false);
            FunctionCallee accessCallee = M.getOrInsertFunction("CoVer_AnnotAliasAccess", accessType);
            dyn_cast<Function>(accessCallee.getCallee())->setMemoryEffects(MemoryEffects::inaccessibleMemOnly());
            for (Conversion const& conv : conversions) {
                CallBase* annot = conv.site->annot;
                conv.site->visitors.clear();
                for (auto const& [access, annotated] : conv.accesses) {
                    CallInst* CI = CallInst::Create(accessCallee, {annotated, annot->getArgOperand(2)});
                    CI->insertBefore(access->getIterator());
                    CI->setDebugLoc(access->getDebugLoc());
                    conv.site->visitors.push_back(CI);
                }
                ContractPassUtility::invalidateFunctionAnalyses(annot->getFunction());
                annot->eraseFromParent();
                conv.site->annot = nullptr;
            }
        }

        Constant* createArray(Module& M, Type* elemTy, std::vector<Constant*> const& elems, std::string const& name) {
            if (elems.empty()) return Basic_Types.Null_Const;
            ArrayType* arrTy = ArrayType::get(elemTy, elems.size());
            return new GlobalVariable(M, arrTy, true, GlobalValue::PrivateLinkage, ConstantArray::get(arrTy, elems), name);
        }

        // Embeds the table of all annotations as CoVer_AnnotTable, which the runtime annotation verifier reads at startup.
        // The layout must match the structs in AnnotVerifier/AnnotVerifier.cpp
        void emitTable(Module& M, std::vector<AliasSite> const& aliasSites, std::vector<FPSite> const& fpSites) {
            LLVMContext& C = M.getContext();

            StructType* AliasSiteTy = StructType::create(C, {Basic_Types.Ptr_Type, Basic_Types.Ptr_Type, Basic_Types.Int_Type, Basic_Types.Int_Type, Basic_Types.Int_Type}, "AnnotAliasSite_t");
            std::vector<Constant*> aliasConsts;
            for (AliasSite const& site : aliasSites) {
                std::vector<Constant*> visitRefs;
                for (Instruction* visitor : site.visitors) {
                    std::optional<std::string> ref = ensureRef(visitor);
                    if (ref) visitRefs.push_back(createString(M, *ref));
                }
                if (!site.ref || visitRefs.empty()) continue; // Location unknown, the runtime cannot tell whether it was visited
                aliasConsts.push_back(ConstantStruct::get(AliasSiteTy, {
                    createString(M, *site.ref),
                    createArray(M, Basic_Types.Ptr_Type, visitRefs, "CoVer_AnnotVisitRefs"),
                    Basic_Types.getInt(static_cast<int>(visitRefs.size())),
                    Basic_Types.getInt(site.group),
                    Basic_Types.getInt(site.shouldAlias)
                }));
            }

            StructType* FPSiteTy = StructType::create(C, {Basic_Types.Ptr_Type, Basic_Types.Ptr_Type, Basic_Types.Ptr_Type, Basic_Types.Int_Type}, "AnnotFPSite_t");
            std::vector<Constant*> fpConsts;
            for (FPSite const& site : fpSites) {
                if (!site.ref || site.targets.empty()) continue;
                std::vector<Constant*> targets;
                std::vector<Constant*> names;
                for (Function* target : site.targets) {
                    targets.push_back(target);
                    names.push_back(createString(M, demangle(target->getName().str())));
                }
                fpConsts.push_back(ConstantStruct::get(FPSiteTy, {
                    createString(M, *site.ref),
                    createArray(M, Basic_Types.Ptr_Type, targets, "CoVer_AnnotFPTargets"),
                    createArray(M, Basic_Types.Ptr_Type, names, "CoVer_AnnotFPTargetNames"),
                    Basic_Types.getInt(static_cast<int>(targets.size()))
                }));
            }
            if (aliasConsts.empty() && fpConsts.empty()) return;

            StructType* TableTy = StructType::create(C, {Basic_Types.Ptr_Type, Basic_Types.Ptr_Type, Basic_Types.Int_Type, Basic_Types.Int_Type}, "AnnotTable_t");
            GlobalVariable* table = dyn_cast<GlobalVariable>(M.getOrInsertGlobal("CoVer_AnnotTable", TableTy));
            table->setConstant(true);
            table->setInitializer(ConstantStruct::get(TableTy, {
                createArray(M, AliasSiteTy, aliasConsts, "CoVer_AnnotAliasSites"),
                createArray(M, FPSiteTy, fpConsts, "CoVer_AnnotFPSites"),
                Basic_Types.getInt(static_cast<int>(aliasConsts.size())),
                Basic_Types.getInt(static_cast<int>(fpConsts.size()))
            }));
        }
};
