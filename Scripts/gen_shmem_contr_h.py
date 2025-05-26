import os
import sys
import re

if len(sys.argv) < 3:
    print(f"Insufficient arguments!\nUsage: {os.path.basename(__file__)} <outpath> <shmem.h location> [identifier]")
    exit(1)

# Output filepath for header file
output_path = sys.argv[1]

# Filepath for input header
inputh_location = sys.argv[2]

ver_identifier = "Not given"
if len(sys.argv) > 3:
    ver_identifier = sys.argv[3]

# Deprecated / Ignored functions
ignorelist = [
]


function_decls = {}
function_contracts = {}

# Get all function definitions
with open(inputh_location) as f:
    h_iter = iter(f.readlines())
    for line in h_iter:
        if re.match(r"[ \tA-z0-9()\"]+ .* shmem_[\w]+[ \t]*\(.*", line):
            # Found a function definition. Get the function name
            res = re.search(r" (shmem_.*?)\(", line)
            funcname = res.group(1).strip()

            # Deprecated or otherwise ignored functions
            if any([x.lower() == funcname.lower() for x in ignorelist]):
                continue

            # Extract full function
            funcdec = line
            while True:
                if ";" in funcdec:
                    break;
                newline = next(h_iter)
                funcdec += newline

            # Cleanup

            # Remove newlines in declaration
            funcdec = funcdec.replace("\n", " ")
            # Only one space in param list
            funcdec = re.sub(",[ \t]*", ", ", funcdec)
            # Remove any postfixes
            parammatch = re.match(r".* shmem_[\w]+[ \t]*(\(.*\))", funcdec)
            if parammatch:
                funcdec = funcdec[1:parammatch.end():] + ";"
            # Remove any prefixes
            funcdec = re.sub(r".*? *(((un)?signed )?(long )?[\w*]+ shmem_[\w]+[ \t]*\(.*\))", r"\g<1>", funcdec)
            # Clear whitespace
            funcdec = funcdec.strip()

            # Add to dicts
            function_decls[funcname] = funcdec
            function_contracts[funcname] = { "PRE": [], "POST": [], "TAGS": []}

# Finally, add contracts

# Call shmem_init
for func in function_decls.keys():
    if func in ["shmem_init", "shmem_init_thread"]:
        function_contracts[func]["TAGS"].append("shmem_init")
        continue
    function_contracts[func]["PRE"].append("call_tag!(shmem_init) MSG \"Missing Initialization call\"")

# Call shmem_finalize
for func in function_decls.keys():
    if func in ["shmem_finalize", "shmem_global_exit"]:
        continue
    function_contracts[func]["POST"].append("call!(shmem_finalize) MSG \"Missing Finalization call\"")

# Local data races
tag_buf = [("shmem_int_put_nbi", 1, "W", "R"),
           ("shmem_int_put_signal_nbi", 1, "W", "R"),
           ("shmem_int_get_nbi", 0, "RW", "W"),
           ("shmem_int_atomic_fetch_inc_nbi", 0, "RW", "RW"),
           ("shmem_int_atomic_fetch_nbi", 0, "RW", "RW"),
           ("shmem_int_atomic_compare_swap_nbi", 0, "RW", "RW"),
            ]
for func, buf_idx, forbid, action in tag_buf:
    if "R" in forbid:
        function_contracts[func]["POST"].append(f"no! (read!(*{buf_idx})) until! (call_tag!(shmem_complete)) MSG \"Local Data Race - Local read\"")
        function_contracts[func]["POST"].append(f"no! (call_tag!(buf_read,$:{buf_idx})) until! (call_tag!(shmem_complete)) MSG \"Local Data Race - Local read by call\"")
    if "W" in forbid:
        function_contracts[func]["POST"].append(f"no! (write!(*{buf_idx})) until! (call_tag!(shmem_complete)) MSG \"Local Data Race - Local write\"")
        function_contracts[func]["POST"].append(f"no! (call_tag!(buf_write,$:{buf_idx})) until! (call_tag!(shmem_complete)) MSG \"Local Data Race - Local write by call\"")
    if "R" in action:
        function_contracts[func]["TAGS"].append(f"buf_read({buf_idx})")
    if "W" in action:
        function_contracts[func]["TAGS"].append(f"buf_write({buf_idx})")

tag_shmemcomplete = [("shmem_barrier_all"), ("shmem_barrier"), ("shmem_quiet")]
for func in tag_shmemcomplete:
    function_contracts[func]["TAGS"].append(f"shmem_complete")

# No inflight calls when freeing
for func, buf_idx, _, _ in tag_buf:
    function_contracts[func]["POST"].append(f"no! (call!(shmem_free,0:{buf_idx})) until! (call_tag!(shmem_complete)) MSG \"Possible inflight call at shmem_free\"")

# Make sure contexts are created and freed
tag_ctxuse = [("shmem_ctx_get8", 0), ("shmem_ctx_get16", 0), ("shmem_ctx_get32", 0), ("shmem_ctx_get64", 0), ("shmem_ctx_get128", 0), ("shmem_ctx_getmem", 0),
              ("shmem_ctx_put8", 0), ("shmem_ctx_put16", 0), ("shmem_ctx_put32", 0), ("shmem_ctx_put64", 0), ("shmem_ctx_put128", 0), ("shmem_ctx_putmem", 0)]
tag_ctxuse_nbi = []
for func, ctx_idx in tag_ctxuse:
    tag_ctxuse_nbi.append((func + "_nbi", ctx_idx))
tag_ctxuse += tag_ctxuse_nbi
for func, ctx_idx in tag_ctxuse:
    function_contracts[func]["PRE"].append(f"call!(shmem_ctx_create,1:&{ctx_idx})")
function_contracts["shmem_ctx_create"]["POST"].append(f"call!(shmem_ctx_destroy,0:*1) MSG \"Context leak\"")

# Make sure teams are freed
tag_teamcreate = [("shmem_team_split_strided", 6), ("shmem_team_split_2d", 4), ("shmem_team_split_2d", 7)]
for func, team_idx in tag_teamcreate:
    function_contracts[func]["POST"].append(f"call!(shmem_team_destroy,0:*{team_idx}) MSG \"Team leak\"")

# Output file
boilerplate_header = f"""
// Automatically generated by {os.path.basename(__file__)}
// Instead of modifying this file, consider modifying the generation script
// Identifier: {ver_identifier}

#pragma once
#include "Contracts.h"
#include <shmem.h>

"""

header_output = boilerplate_header

def create_contract_output_for_func(types, contrs):
    out = ""
    for c_type in types:
        if not contrs[c_type]: continue
        out += f"    {c_type} {{\n"
        for c in contrs[c_type][:-1]:
            out += f"        {c},\n"
        out += f"        {contrs[c_type][-1]}\n"
        out += "    }\n"
    return out

for func, contrs in function_contracts.items():
    if not contrs["PRE"] and not contrs["POST"] and not contrs["TAGS"]:
        continue # No contracts, no need to output
    header_output += function_decls[func][:-1] + " CONTRACT(\n"
    header_output += create_contract_output_for_func(contrs.keys(), contrs)
    header_output += ");\n\n"

with open(f"{output_path}/shmem_contracts.h", "w") as contr_file:
    contr_file.write(header_output)
