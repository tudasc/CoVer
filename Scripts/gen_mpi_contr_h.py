import os
import sys
import re

if len(sys.argv) < 3:
    print("Insufficient arguments!\nUsage: gen_mpi_contr_h.py <outpath> <mpi.h location> [identifier]")
    exit(1)

# Output filepath for mpi_contracts.h
output_path = sys.argv[1]

# Filepath for mpi.h
mpih_location = sys.argv[2]

ver_identifier = "Not given"
if len(sys.argv) > 3:
    ver_identifier = sys.argv[3]

# Deprecated functions
# From: https://www.mpi-forum.org/docs/mpi-3.1/mpi31-report/node34.htm#Node34
ignorelist = [
    "MPI_ADDRESS",
    "MPI_TYPE_HINDEXED",
    "MPI_TYPE_HVECTOR",
    "MPI_TYPE_STRUCT",
    "MPI_TYPE_EXTENT",
    "MPI_TYPE_UB",
    "MPI_TYPE_LB",
    "MPI_LB1",
    "MPI_UB1",
    "MPI_ERRHANDLER_CREATE",
    "MPI_ERRHANDLER_GET",
    "MPI_ERRHANDLER_SET",
    "MPI_Handler_function2",
    "MPI_KEYVAL_CREATE",
    "MPI_KEYVAL_FREE",
    "MPI_DUP_FN3",
    "MPI_NULL_COPY_FN3",
    "MPI_NULL_DELETE_FN3",
    "MPI_Copy_function2",
    "MPI_Delete_function2",
    "MPI_ATTR_DELETE",
    "MPI_ATTR_GET",
    "MPI_ATTR_PUT",
    "MPI_KEYVAL_CREATE",
]


function_decls = {}
function_contracts = {}

# Get all function definitions
with open(mpih_location) as f:
    mpih_iter = iter(f.readlines())
    for line in mpih_iter:
        if re.match(r"[ \tA-z0-9()\"]+ .* MPI_[A-z0-9_]+[ \t]*\(.*", line):
            # Found a function definition. Get the function name
            res = re.search(r" (MPI_.*)\(", line)
            funcname = res.group(1).strip()

            # MPI Tools interface
            if "MPI_T_" in funcname:
                continue

            # Deprecated functions
            if any([x.lower() == funcname.lower() for x in ignorelist]):
                continue

            # Extract full function
            funcdec = line
            while True:
                if ";" in funcdec:
                    break;
                newline = next(mpih_iter)
                funcdec += newline

            # Cleanup

            # Remove newlines in declaration
            funcdec = funcdec.replace("\n", " ")
            # Only one space in param list
            funcdec = re.sub(",[ \t]*", ", ", funcdec)
            # Remove any postfixes
            funcdec = re.sub(r"(MPI_[A-z0-9_]+[ \t]*\(.*\)) .*;", r"\g<1>;", funcdec)
            # Remove any prefixes
            funcdec = re.sub(r".* (.* MPI_[A-z0-9_]+[ \t]*\(.*\))", r"\g<1>", funcdec)
            # Clear whitespace
            funcdec = funcdec.strip()

            # Add to dicts
            function_decls[funcname] = funcdec
            function_contracts[funcname] = { "PRE": [], "POST": [], "TAGS": []}

# Finally, add contracts

# Call MPI_Init
for func in function_decls.keys():
    if func in ["MPI_Init", "MPI_Init_thread"]:
        continue
    function_contracts[func]["PRE"].append("called!(MPI_Init)")

# Call MPI_Finalize
for func in function_decls.keys():
    if func in ["MPI_Finalize", "MPI_Abort"]:
        continue
    function_contracts[func]["POST"].append("called!(MPI_Finalize)")

# No request reuse until MPI_Wait for persistent comms
tag_reqgen = [("MPI_Iallgather", 7), ("MPI_Iallreduce", 6), ("MPI_Ialltoall", 7), ("MPI_Ibarrier", 1), ("MPI_Ibcast", 5), ("MPI_Igather", 8), ("MPI_Ibsend", 6), ("MPI_Irecv", 6), ("MPI_Isend", 6), ("MPI_Isendrecv", 11), ("MPI_Start", 0)]
for func, tag_idx in tag_reqgen:
    function_contracts[func]["POST"].append(f"no! (called_tag!(request_gen,$:{tag_idx})) until! (called!(MPI_Wait,0:{tag_idx}))")
    function_contracts[func]["TAGS"].append(f"request_gen({tag_idx})")

# Local data races - P2P
tag_buffers = [("MPI_Isend", 0, 6, "W", "R"),
               ("MPI_Irecv", 0, 6, "RW", "W"),
               ("MPI_Iallreduce", 0, 6, "W", "R"), # Send buffer - No writing
               ("MPI_Iallreduce", 1, 6, "RW", "W"), # Recv buffer - No RW
               ]
for func, buf_idx, req_idx, forbid, action in tag_buffers:
    if "R" in forbid:
        function_contracts[func]["POST"].append(f"no! (read!(*{buf_idx})) until! (called!(MPI_Wait,0:{req_idx}))")
        function_contracts[func]["POST"].append(f"no! (called_tag!(buf_read,$:{buf_idx})) until! (called!(MPI_Wait,0:{req_idx}))")
    if "W" in forbid:
        function_contracts[func]["POST"].append(f"no! (write!(*{buf_idx})) until! (called!(MPI_Wait,0:{req_idx}))")
        function_contracts[func]["POST"].append(f"no! (called_tag!(buf_write,$:{buf_idx})) until! (called!(MPI_Wait,0:{req_idx}))")
    if "R" in action:
        function_contracts[func]["TAGS"].append(f"buf_read({buf_idx})")
    if "W" in action:
        function_contracts[func]["TAGS"].append(f"buf_write({buf_idx})")

# Make sure types are committed
tag_typegen = [("MPI_Type_contiguous", 2)]
for func, tag_idx in tag_typegen:
    function_contracts[func]["POST"].append(f"no! (called_tag!(type_use,$:*{tag_idx})) until! (called!(MPI_Type_commit,0:{tag_idx}))")
    function_contracts[func]["TAGS"].append(f"type_gen({tag_idx})")

tag_typeuse = [("MPI_Send", 2), ("MPI_Isend", 2)]
for func, tag_idx in tag_typeuse:
    function_contracts[func]["TAGS"].append(f"type_use({tag_idx})")

# Output file
boilerplate_header = f"""
// Automatically generated by gen_mpi_contr_h.py
// Instead of modifying this file, consider modifying the generation script
// Identifier: {ver_identifier}

#pragma once
#include "Contracts.h"
#include <mpi.h>

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

with open(f"{output_path}/mpi_contracts.h", "w") as mpi_contr_file:
    mpi_contr_file.write(header_output)
