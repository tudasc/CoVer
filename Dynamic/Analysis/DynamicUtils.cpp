#include "DynamicUtils.h"

#include <iostream>
#include <map>
#include <ostream>
#include <set>
#include <string>

#include "DynamicAnalysis.h"

namespace DynamicUtils {
    std::map<void*, std::set<Tag_t>> func_to_tags;
    std::map<std::string, std::set<void*>> tags_to_func;

    void Initialize(ContractDB_t* DB) {
        // Initialize Tags
        for (int i = 0; i < DB->tags.count; i++) {
            void* function = DB->tags.functions[i];
            if (!func_to_tags.contains(function)) func_to_tags[function] = {DB->tags.tags[i]};
            else func_to_tags[function].insert(DB->tags.tags[i]);
            if (!tags_to_func.contains(DB->tags.tags[i].tag)) tags_to_func[DB->tags.tags[i].tag] = {function};
            else tags_to_func[DB->tags.tags[i].tag].insert(function);
        }
    }

    std::map<void*,std::string> recurseGetFunctions(ContractFormula_t contr) {
        std::map<void*,std::string> ret;
        for (int i = 0; i < contr.num_children; i++) {
            std::map<void*,std::string> new_funcs = recurseGetFunctions(contr.children[i]);
            ret.insert(new_funcs.begin(), new_funcs.end());
        }
        if (contr.num_children == 0) {
            switch (contr.conn) {
                case UNARY_CALL: {
                    CallOp_t* op = (CallOp_t*)contr.data;
                    ret[op->target_function] = op->function_name;
                    break;
                }
                case UNARY_CALLTAG: {
                    #warning TODO
                    break;
                }
                case UNARY_RELEASE: {
                    ReleaseOp_t* op = (ReleaseOp_t*)contr.data;
                    ContractFormula_t forbidden = {NULL, 0, (ContractConnective)op->forbidden_op_kind, NULL, op->forbidden_op};
                    std::map<void*, std::string> forbidden_res = recurseGetFunctions(forbidden);
                    ContractFormula_t until = {NULL, 0, (ContractConnective)op->release_op_kind, NULL, op->release_op};
                    std::map<void*, std::string> until_res = recurseGetFunctions(until);
                    ret.insert(forbidden_res.begin(), forbidden_res.end());
                    ret.insert(until_res.begin(), until_res.end());
                    break;
                }
                case UNARY_READ:
                case UNARY_WRITE:
                    // No additional functions
                    break;
                default:
                    std::cerr << "Unhandled expression type: " << contr.conn << "\n";
                    break;
            }
        }
        return ret;
    }

    bool checkParamMatch(ParamAccess acc, void* contrP, void* callP) {
        switch (acc) {
            case ParamAccess::NORMAL:
                return contrP == callP;
            case ParamAccess::DEREF:
                return *(void**)contrP == callP;
            case ParamAccess::ADDROF:
                return *(void**)callP == contrP;
        }
    }

    std::set<void*> getFunctionsForTag(std::string tag) {
        if (tags_to_func.contains(tag))
            return tags_to_func[tag];
        return {};
    }

    std::set<Tag_t> getTagsForFunction(void* func) {
        if (func_to_tags.contains(func))
            return func_to_tags[func];
        return {};
    }

    void createMessage(std::string msg) {
        std::cerr << "CoVer-Dynamic: " << msg << "\n";
        std::flush(std::cerr);
    }
}