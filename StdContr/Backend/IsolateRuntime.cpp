#include <vector>

#include <gcc-plugin.h>
#include <plugin-version.h>
#include <tree.h>
#include <tree-pass.h>
#include <context.h>
#include <function.h>
#include <cgraph.h>

static bool runtime_tu = false;

/* Take one symbol out of its COMDAT group, following what ipa-visibility does
   when it privatizes a symbol it has proved local. The visibility pass runs
   right after this one and settles the rest. */
static void privatize(symtab_node* node) {
    // A group with several members has to be taken apart before any of them moves
    node->dissolve_same_comdat_group_list();
    node->set_comdat_group(NULL_TREE);
    // An alias has no section of its own, it shares the one its target sits in
    if (!node->alias) node->set_section(static_cast<const char*>(nullptr));
    node->make_decl_local();
    node->externally_visible = false;
    node->forced_by_abi = false;
    node->resolution = LDPR_PREVAILING_DEF_IRONLY;
}

static void privatize_vague_linkage() {
    /* Dissolving a group relinks its members, so decide on the whole set first. */
    std::vector<symtab_node*> targets;

    cgraph_node* node;
    FOR_EACH_DEFINED_FUNCTION(node) {
        // A body that lives in another unit is not ours to rename
        if (DECL_EXTERNAL(node->decl)) continue;
        // A weakref is a reference, it defines nothing to privatize
        if (node->weakref) continue;
        if (!DECL_COMDAT(node->decl)) continue;

        targets.push_back(node);
    }

    for (symtab_node* target : targets) privatize(target);
}

namespace {

const pass_data isolate_pass_data = {
    SIMPLE_IPA_PASS,
    "cover_isolate_runtime",
    OPTGROUP_NONE,
    TV_NONE,
    0,
    0,
    0,
    0,
    0,
};

struct isolate_pass : simple_ipa_opt_pass {
    isolate_pass(gcc::context* ctxt) : simple_ipa_opt_pass(isolate_pass_data, ctxt) {}

    bool gate(function*) override { return runtime_tu; }

    unsigned int execute(function*) override {
        privatize_vague_linkage();
        return 0;
    }
};

} // namespace

void setup_runtime_isolation_pass(struct plugin_name_args* plugin_info, bool is_runtime_tu) {
    runtime_tu = is_runtime_tu;

    struct register_pass_info pass_info;
    pass_info.pass = new isolate_pass(g);
    /* Ahead of the visibility pass, so it sees these as the local symbols they
       now are and can narrow them further. */
    pass_info.reference_pass_name = "visibility";
    pass_info.ref_pass_instance_number = 1;
    pass_info.pos_op = PASS_POS_INSERT_BEFORE;

    register_callback(plugin_info->base_name, PLUGIN_PASS_MANAGER_SETUP, NULL, &pass_info);
}
