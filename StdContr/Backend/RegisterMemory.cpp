#include <string>

#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <tree-pass.h>
#include <context.h>
#include <function.h>
#include <basic-block.h>
#include <gimple.h>
#include <gimple-iterator.h>
#include <gimple-expr.h>
#include <gimplify.h>
#include <fold-const.h>
#include <tree-cfg.h>
#include <cgraph.h>
#include <stringpool.h>

// Extern declaration of an intrinsic, explicitly set asm name
static tree intrinsic_decl(const char* name, tree fntype) {
    tree id = get_identifier(name);
    tree decl = build_decl(UNKNOWN_LOCATION, FUNCTION_DECL, id, fntype);
    TREE_PUBLIC(decl) = 1;
    DECL_EXTERNAL(decl) = 1;
    TREE_USED(decl) = 1;
    SET_DECL_ASSEMBLER_NAME(decl, id);
    return decl;
}

static tree alloc_stack_type() { return build_function_type_list(void_type_node, ptr_type_node, size_type_node, NULL_TREE); }

static tree free_stack_type() { return build_function_type_list(void_type_node, ptr_type_node, NULL_TREE); }

static tree register_global_type() { return build_function_type_list(void_type_node, ptr_type_node, long_long_integer_type_node, NULL_TREE); }

// Only variables whose address is taken can reach a contract as a pointer, so
// frames holding none of those need no announcement. Parameters count as well,
// they are spilled into the frame once something takes their address.
static bool has_stack_locals(function* fun) {
    unsigned i;
    tree var;
    FOR_EACH_LOCAL_DECL(fun, i, var) {
        if (!VAR_P(var)) continue;
        if (TREE_STATIC(var) || DECL_EXTERNAL(var)) continue;
        if (TREE_ADDRESSABLE(var)) return true;
    }
    for (tree parm = DECL_ARGUMENTS(fun->decl); parm; parm = DECL_CHAIN(parm))
        if (TREE_ADDRESSABLE(parm)) return true;
    return false;
}

/* Announce the whole frame as one range: [stack pointer, frame address). Every
   local lives inside it, so a single pair of calls covers the function. */
static void instrument_frame(function* fun) {
    tree stack_save = builtin_decl_explicit(BUILT_IN_STACK_SAVE);
    tree frame_address = builtin_decl_explicit(BUILT_IN_FRAME_ADDRESS);
    if (!stack_save || !frame_address) return;

    tree stack_ptr = create_tmp_var(ptr_type_node, "cover_stack_ptr");
    tree frame_ptr = create_tmp_var(ptr_type_node, "cover_frame_ptr");
    tree stack_int = create_tmp_var(size_type_node, "cover_stack_int");
    tree frame_int = create_tmp_var(size_type_node, "cover_frame_int");
    tree frame_size = create_tmp_var(size_type_node, "cover_frame_size");

    gimple_seq seq = NULL;

    gcall* save_call = gimple_build_call(stack_save, 0);
    gimple_call_set_lhs(save_call, stack_ptr);
    gimple_seq_add_stmt(&seq, save_call);

    gcall* frame_call = gimple_build_call(frame_address, 1, build_int_cst(unsigned_type_node, 0));
    gimple_call_set_lhs(frame_call, frame_ptr);
    gimple_seq_add_stmt(&seq, frame_call);

    gimple_seq_add_stmt(&seq, gimple_build_assign(stack_int, NOP_EXPR, stack_ptr));
    gimple_seq_add_stmt(&seq, gimple_build_assign(frame_int, NOP_EXPR, frame_ptr));
    gimple_seq_add_stmt(&seq, gimple_build_assign(frame_size, MINUS_EXPR, frame_int, stack_int));
    gimple_seq_add_stmt(&seq, gimple_build_call(intrinsic_decl("CoVer_AllocStack", alloc_stack_type()), 2, stack_ptr, frame_size));

    /* Inserting on the entry edge splits it if the first block is also reached
       from elsewhere, so the sequence runs exactly once. */
    gsi_insert_seq_on_edge_immediate(single_succ_edge(ENTRY_BLOCK_PTR_FOR_FN(fun)), seq);

    /* Frames left through an exception keep their range: EH has been lowered by
       now, so only returns are reachable insertion points. */
    basic_block bb;
    FOR_EACH_BB_FN(bb, fun) {
        gimple_stmt_iterator gsi = gsi_last_bb(bb);
        if (gsi_end_p(gsi)) continue;
        if (gimple_code(gsi_stmt(gsi)) != GIMPLE_RETURN) continue;
        gcall* free_call = gimple_build_call(intrinsic_decl("CoVer_FreeStack", free_stack_type()), 1, stack_ptr);
        gsi_insert_before(&gsi, free_call, GSI_SAME_STMT);
    }
}

// Globals of this translation unit, registered once at the top of main
static void instrument_globals(function* fun) {
    gimple_seq seq = NULL;

    varpool_node* node;
    FOR_EACH_VARIABLE(node) {
        tree var = node->decl;
        if (!VAR_P(var)) continue;
        if (DECL_EXTERNAL(var) || !TREE_STATIC(var)) continue; // Not defined here
        if (DECL_THREAD_LOCAL_P(var)) continue;
        if (DECL_ARTIFICIAL(var) || !DECL_NAME(var)) continue; // String literals, vtables, ...
        if (DECL_ONE_ONLY(var) || DECL_COMDAT(var)) continue;  // Another unit may register these

        tree size = DECL_SIZE_UNIT(var);
        if (!size || TREE_CODE(size) != INTEGER_CST || !tree_fits_uhwi_p(size)) continue;

        tree asm_id = DECL_ASSEMBLER_NAME(var);
        if (asm_id && !strncmp(IDENTIFIER_POINTER(asm_id), "COVER_", 6)) continue; // Bookkeeping of the wrappers themselves

        gimple_seq_add_stmt(&seq, gimple_build_call(intrinsic_decl("CoVer_RegisterGlobal", register_global_type()), 2,
                                                    build_fold_addr_expr_with_type(var, ptr_type_node),
                                                    build_int_cst(long_long_integer_type_node, tree_to_uhwi(size))));
    }

    if (seq) gsi_insert_seq_on_edge_immediate(single_succ_edge(ENTRY_BLOCK_PTR_FOR_FN(fun)), seq);
}

namespace {

const pass_data memregister_pass_data = {
    GIMPLE_PASS,
    "cover_memregister",
    OPTGROUP_NONE,
    TV_NONE,
    PROP_gimple_any,
    0,
    0,
    0,
    TODO_update_ssa,
};

struct memregister_pass : gimple_opt_pass {
    memregister_pass(gcc::context* ctxt) : gimple_opt_pass(memregister_pass_data, ctxt) {}

    unsigned int execute(function* fun) override {
        tree fndecl = fun->decl;
        tree asm_id = DECL_ASSEMBLER_NAME(fndecl);
        if (!asm_id) return 0;
        std::string cur = IDENTIFIER_POINTER(asm_id);

        // Dont instrument intrinsics or wrappers
        if (cur.starts_with("CoVer_")) return 0;

        // Dont instrument sys headers
        if (DECL_IN_SYSTEM_HEADER(fndecl)) return 0;

        // Instrument globals if in main
        if (cur == "main" || cur == "CoVer_RealMain") instrument_globals(fun);

        // Instrument stack
        if (has_stack_locals(fun)) instrument_frame(fun);

        return 0;
    }
};

}

void setup_memregister_pass(struct plugin_name_args* plugin_info) {
    struct register_pass_info pass_info;
    pass_info.pass = new memregister_pass(g);
    /* The calls added here are still under their plain names, so they have to
       pass through the call rewriting before they mean anything. */
    pass_info.reference_pass_name = "rewrite_calls";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_BEFORE;

    register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP, NULL, &pass_info);
}
