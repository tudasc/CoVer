module;

#include <map>

export module BasicTypes;
import LLVMModule;

using namespace llvm;

export class BasicTypesAnalysis : public AnalysisInfoMixin<BasicTypesAnalysis> {
    private:
        std::map<Type const*, Metadata*> TypeToMD;
    public:
        static inline llvm::AnalysisKey Key;
        //Result Type
        class Result {
            friend class BasicTypesAnalysis;
            public:
                PointerType* Ptr_Type;
                IntegerType* Bool_Type;
                Constant* getBool(bool x) { return ConstantInt::getBool(Bool_Type, x); }
                IntegerType* Int_Type;
                ConstantInt* getInt(int x) { return ConstantInt::get(Int_Type, x); }
                IntegerType* Int64_Type;
                ConstantInt* getInt64(int x) { return ConstantInt::get(Int64_Type, x); }
                Type* Void_Type;
                Constant* Null_Const;
                bool invalidate(Module &, PreservedAnalyses const&, ModuleAnalysisManager::Invalidator const&) const {
                    return false;
                }
                Metadata* getMDForType(Type const* T) const {
                    if (TypeToMD.contains(T)) return TypeToMD.at(T);
                    errs() << "BasicTypes: Queried unknown type ";
                    T->print(errs());
                    errs() << "!\n";
                    return nullptr;
                }
            private:
                std::map<Type const*, Metadata*> TypeToMD;
        } typedef BasicTypes;
        // Run Analysis
        BasicTypes run(Module &M, ModuleAnalysisManager &AM) {
            BasicTypes types;

            DIBuilder DIB(M);

            // Basic Types
            types.Ptr_Type = PointerType::get(M.getContext(), 0);
            types.TypeToMD[types.Ptr_Type] = DIB.createBasicType("void*", 64, dwarf::DW_TAG_pointer_type);

            types.Int_Type = IntegerType::get(M.getContext(), 32);
            types.TypeToMD[types.Int_Type] = DIB.createBasicType("int32_t", 32, dwarf::DW_ATE_signed);

            types.Int64_Type = IntegerType::get(M.getContext(), 64);
            types.TypeToMD[types.Int64_Type] = DIB.createBasicType("int64_t", 64, dwarf::DW_ATE_signed);

            types.Bool_Type = IntegerType::get(M.getContext(), 1);
            types.TypeToMD[types.Bool_Type] = DIB.createBasicType("bool", 1, dwarf::DW_ATE_boolean);

            types.Void_Type = Type::getVoidTy(M.getContext());
            types.TypeToMD[types.Void_Type] = nullptr;

            // Basic Constants
            types.Null_Const = ConstantPointerNull::getNullValue(types.Ptr_Type);

            DIB.finalize();

            return types;
        }
};
