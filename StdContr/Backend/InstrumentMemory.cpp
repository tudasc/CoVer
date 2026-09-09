#include <string>
#include <vector>

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
#include <gimple-walk.h>
#include <cgraph.h>
#include <gimplify.h>
#include <gimplify-me.h>
#include <fold-const.h>
#include <tree-cfg.h>
#include <stringpool.h>

static bool runtime_tu = false;
bool cover_is_internal_name(std::string const& mangled);
tree cover_intrinsic_decl(std::string name, tree fntype);

/* void (uintptr_t), the signature ContractConverter gives the dummies it emits
   into include.cpp. */
static tree mem_dummy_type() { return build_function_type_list(void_type_node, pointer_sized_int_node, NULL_TREE); }

/* Address the access OP touches, or NULL_TREE if no contract could ever name it.
   The result is still a tree expression, it needs gimplifying before use. */
static tree access_address(tree op) {
    /* Bitfields and the halves of a complex have no address of their own, so
       report the object holding them instead. A view conversion shares the
       address of what it reinterprets. */
    while (TREE_CODE(op) == BIT_FIELD_REF || TREE_CODE(op) == REALPART_EXPR || TREE_CODE(op) == IMAGPART_EXPR ||
           TREE_CODE(op) == VIEW_CONVERT_EXPR || (TREE_CODE(op) == COMPONENT_REF && DECL_BIT_FIELD(TREE_OPERAND(op, 1))))
        op = TREE_OPERAND(op, 0);

    tree base = get_base_address(op);
    if (!base) return NULL_TREE;

    if (DECL_P(base)) {
        /* A local that never had its address taken cannot be the pointer a
           contract talks about, and forcing it into the frame just to say so
           would cost optimizations. Globals and statics always have an address,
           RegisterMemory announces them. */
        if (!TREE_STATIC(base) && !DECL_EXTERNAL(base) && !TREE_ADDRESSABLE(base)) return NULL_TREE;
    } else if (TREE_CODE(base) != MEM_REF && TREE_CODE(base) != TARGET_MEM_REF) {
        return NULL_TREE; // String literals, constant pool entries, ...
    }

    return build_fold_addr_expr(op);
}

namespace {

// Accesses of one statement, gathered before anything is inserted
struct access_lists {
    std::vector<tree> reads;
    std::vector<tree> writes;
};

bool collect_read(gimple*, tree, tree op, void* data) {
    if (tree addr = access_address(op)) static_cast<access_lists*>(data)->reads.push_back(addr);
    return false;
}

bool collect_write(gimple*, tree, tree op, void* data) {
    if (tree addr = access_address(op)) static_cast<access_lists*>(data)->writes.push_back(addr);
    return false;
}

} // namespace

// Hand ADDR to DUMMY, converted to the integer type it takes
static void add_notification(gimple_seq* seq, tree dummy, tree addr) {
    gimple_seq pre = NULL;
    addr = force_gimple_operand(fold_convert(pointer_sized_int_node, addr), &pre, true, NULL_TREE);
    gimple_seq_add_seq(seq, pre);

    gimple_seq_add_stmt(seq, gimple_build_call(dummy, 1, addr));
}

/* The dummies report accesses that already happened, so they belong behind the
   statement. A statement ending its block has no room behind it: a call may
   leave through an EH edge, so the notification goes onto the fallthrough. A
   return or a condition has no fallthrough, there the access is complete by the
   time the statement itself runs and reporting in front of it is equivalent. */
static void insert_notifications(basic_block bb, gimple_stmt_iterator* gsi, gimple_seq seq) {
    if (!stmt_ends_bb_p(gsi_stmt(*gsi))) {
        gsi_insert_seq_after(gsi, seq, GSI_CONTINUE_LINKING);
        return;
    }
    if (edge fallthru = find_fallthru_edge(bb->succs)) {
        gsi_insert_seq_on_edge_immediate(fallthru, seq);
        return;
    }
    gsi_insert_seq_before(gsi, seq, GSI_SAME_STMT);
}

namespace {

const pass_data meminstr_pass_data = {
    GIMPLE_PASS,
    "cover_meminstr",
    OPTGROUP_NONE,
    TV_NONE,
    PROP_ssa | PROP_cfg,
    0,
    0,
    0,
    TODO_update_ssa | TODO_rebuild_cgraph_edges,
};

struct meminstr_pass : gimple_opt_pass {
    meminstr_pass(gcc::context* ctxt) : gimple_opt_pass(meminstr_pass_data, ctxt) {}

    bool gate(function*) override { return !runtime_tu; }

    unsigned int execute(function* fun) override {
        tree fndecl = fun->decl;
        tree asm_id = DECL_ASSEMBLER_NAME(fndecl);
        if (!asm_id) return 0;

        // Dont instrument intrinsics or wrappers, except CoVer_RealMain
        if (cover_is_internal_name(IDENTIFIER_POINTER(asm_id))) return 0;

        tree memR = cover_intrinsic_decl("CoVer_MemRDummy", mem_dummy_type());
        tree memW = cover_intrinsic_decl("CoVer_MemWDummy", mem_dummy_type());

        /* Reporting onto an edge can split it, which appends to the block list.
           Collect the blocks up front so only the original code is walked. */
        std::vector<basic_block> blocks;
        basic_block bb;
        FOR_EACH_BB_FN(bb, fun) blocks.push_back(bb);

        for (basic_block bb : blocks) {
            for (gimple_stmt_iterator gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
                gimple* stmt = gsi_stmt(gsi);
                if (is_gimple_debug(stmt)) continue;
                // Clobbers end a lifetime, they store no value
                if (gimple_clobber_p(stmt)) continue;

                access_lists accs;
                walk_stmt_load_store_ops(stmt, &accs, collect_read, collect_write);
                if (accs.reads.empty() && accs.writes.empty()) continue;

                // Operands are read before the result is stored, keep that order
                gimple_seq seq = NULL;
                for (tree addr : accs.reads) add_notification(&seq, memR, addr);
                for (tree addr : accs.writes) add_notification(&seq, memW, addr);

                annotate_all_with_location(seq, gimple_location(stmt));
                insert_notifications(bb, &gsi, seq);
            }
        }

        return 0;
    }
};

} // namespace

void setup_meminstr_pass(struct plugin_name_args* plugin_info, bool is_runtime_tu) {
    runtime_tu = is_runtime_tu;

    struct register_pass_info pass_info;
    pass_info.pass = new meminstr_pass(g);
    pass_info.reference_pass_name = "optimized";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_BEFORE;

    register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP, NULL, &pass_info);
}
