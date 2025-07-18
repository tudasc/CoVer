import os
import sys
import re

if len(sys.argv) < 3:
    print("Insufficient arguments!\nUsage: {os.path.basename(__file__)} <outpath> <mpi.h location> [identifier]")
    exit(1)

# Output filepath for header file
output_path = sys.argv[1]

# Filepath for input header
inputh_location = sys.argv[2]

ver_identifier = "Not given"
if len(sys.argv) > 3:
    ver_identifier = sys.argv[3]

# Deprecated / Ignored functions
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
with open(inputh_location) as f:
    h_iter = iter(f.readlines())
    for line in h_iter:
        if re.match(r"[ \tA-z0-9()\"]+.* MPI_[\w]+[ \t]*\([^\(]*$", line):
            # Found a function definition. Get the function name
            res = re.search(r" (MPI_.*?)\(", line)
            funcname = res.group(1).strip()
            if "c2f" in funcname or "f2c" in funcname: continue # Causes issues with MPICH

            # MPI Tools interface
            if "MPI_T_" in funcname:
                continue

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
            parammatch = re.match(r".* MPI_[\w]+[ \t]*(\([^\(]*\))", funcdec)
            if parammatch:
                funcdec = funcdec[0:parammatch.end():] + ";"
            # Remove any prefixes
            funcdec = re.sub(r".* (.* MPI_[A-z0-9_]+[ \t]*\(.*\))", r"\g<1>", funcdec)
            # Clear whitespace
            funcdec = funcdec.strip()

            # Add to dicts
            function_decls[funcname] = funcdec
            function_contracts[funcname] = { "PRE": [], "POST": [], "TAGS": []}

# Finally, add contracts

# Call MPI initializer
for func in function_decls.keys():
    if func in ["MPI_Init", "MPI_Init_thread", "MPI_Session_init"]:
        function_contracts[func]["TAGS"].append("mpi_init")
        continue
    function_contracts[func]["PRE"].append("call_tag!(mpi_init) MSG \"Missing Initialization call\"")

# Call MPI_Finalize
for func in function_decls.keys():
    if func in ["MPI_Finalize", "MPI_Abort"]:
        continue
    function_contracts[func]["POST"].append("call!(MPI_Finalize) MSG \"Missing Finalization call\"")

# No request reuse until p2pcomplete for persistent comms, request must be completed
tag_reqgen = [("MPI_Iallgather", 7), ("MPI_Iallreduce", 6), ("MPI_Ialltoall", 7), ("MPI_Ibarrier", 1), ("MPI_Ibcast", 5), ("MPI_Igather", 8), ("MPI_Ibsend", 6), ("MPI_Irecv", 6), ("MPI_Isend", 6), ("MPI_Isendrecv", 11), ("MPI_Start", 0)]
for func, tag_idx in tag_reqgen:
    function_contracts[func]["POST"].append(f"no! (call_tag!(request_gen,$:{tag_idx})) until! (call_tag!(req_complete,$:{tag_idx})) MSG \"Double Request Use\"")
    function_contracts[func]["POST"].append(f"call_tag!(req_complete,$:{tag_idx}) MSG \"Request Leak\"")
    function_contracts[func]["TAGS"].append(f"request_gen({tag_idx})")
# Also add Rget, Rput to req_gen to satisfy unmatched wait, but not the other contrs
function_contracts["MPI_Rget"]["TAGS"].append(f"request_gen(8)")
function_contracts["MPI_Rput"]["TAGS"].append(f"request_gen(8)")
# Unmatched Wait
function_contracts["MPI_Wait"]["PRE"].append(f"call_tag!(request_gen,$:0) MSG \"Unmatched Wait\"")

# Local data races
tag_buf = [("RMAWIN", "MPI_Put", 0, 7, "W", "R"),
           ("EITHER", "MPI_Rput", 0, 7, "W", "R"),
           ("RMAWIN", "MPI_Get", 0, 7, "RW", "W"),
           ("EITHER", "MPI_Rget", 0, 7, "RW", "W"),
           ("RMAWIN", "MPI_Get_accumulate", 0, 11, "W", "R"),
           ("RMAWIN", "MPI_Get_accumulate", 3, 11, "RW", "W"),
           ("RMAWIN", "MPI_Accumulate", 0, 8, "W", "R"),
           ("RMAWIN", "MPI_Fetch_and_op", 0, 6, "W", "R"),
           ("RMAWIN", "MPI_Fetch_and_op", 1, 6, "RW", "W"),
           ("RMAWIN", "MPI_Compare_and_swap", 0, 6, "W", "R"),
           ("RMAWIN", "MPI_Compare_and_swap", 1, 6, "W", "R"),
           ("RMAWIN", "MPI_Compare_and_swap", 2, 6, "RW", "W"),
           ("REQ", "MPI_Isend", 0, 6, "W", "R"),
           ("REQ", "MPI_Irecv", 0, 6, "RW", "W"),
           ("REQ", "MPI_Iallreduce", 0, 6, "W", "R"), # Send buffer - No writing
           ("REQ", "MPI_Iallreduce", 1, 6, "RW", "W")] # Recv buffer - No RW
for calltype, func, buf_idx, mark_idx, forbid, action in tag_buf:
    if calltype == "RMAWIN": completiontag = "rma_complete"
    if calltype == "REQ": completiontag = "req_complete"
    if calltype != "EITHER":
        if "R" in forbid:
            function_contracts[func]["POST"].append(f"no! (read!(*{buf_idx})) until! (call_tag!({completiontag},$:{mark_idx})) MSG \"Local Data Race - Local read\"")
            function_contracts[func]["POST"].append(f"no! (call_tag!(buf_read,$:{buf_idx})) until! (call_tag!({completiontag},$:{mark_idx})) MSG \"Local Data Race - Local read by call\"")
        if "W" in forbid:
            function_contracts[func]["POST"].append(f"no! (write!(*{buf_idx})) until! (call_tag!({completiontag},$:{mark_idx})) MSG \"Local Data Race - Local write\"")
            function_contracts[func]["POST"].append(f"no! (call_tag!(buf_write,$:{buf_idx})) until! (call_tag!({completiontag},$:{mark_idx})) MSG \"Local Data Race - Local write by call\"")
    if "R" in action:
        function_contracts[func]["TAGS"].append(f"buf_read({buf_idx})")
    if "W" in action:
        function_contracts[func]["TAGS"].append(f"buf_write({buf_idx})")

# Special handling of EITHER (Rput, Rget)
# Both may not be written
function_contracts["MPI_Rput"]["POST"].append(f"( no! (write!(*0)) until! (call_tag!(rma_complete,$:7)) | \
                                                  no! (write!(*0)) until! (call_tag!(req_complete,$:8)) ) MSG \"Local Data Race - Local write\"")
function_contracts["MPI_Rget"]["POST"].append(f"( no! (write!(*0)) until! (call_tag!(rma_complete,$:7)) | \
                                                  no! (write!(*0)) until! (call_tag!(req_complete,$:8)) ) MSG \"Local Data Race - Local write\"")
function_contracts["MPI_Rput"]["POST"].append(f"( no! (call_tag!(buf_write,$:0)) until! (call_tag!(rma_complete,$:7)) | \
                                                  no! (call_tag!(buf_write,$:0)) until! (call_tag!(req_complete,$:8)) ) MSG \"Local Data Race - Local write by MPI call\"")
function_contracts["MPI_Rget"]["POST"].append(f"( no! (call_tag!(buf_write,$:0)) until! (call_tag!(rma_complete,$:7)) | \
                                                  no! (call_tag!(buf_write,$:0)) until! (call_tag!(req_complete,$:8)) ) MSG \"Local Data Race - Local write by MPI call\"")
# Rget can also not be read
function_contracts["MPI_Rget"]["POST"].append(f"( no! (read!(*0)) until! (call_tag!(rma_complete,$:7)) | \
                                                  no! (read!(*0)) until! (call_tag!(req_complete,$:8)) ) MSG \"Local Data Race - Local read\"")
function_contracts["MPI_Rget"]["POST"].append(f"( no! (call_tag!(buf_read,$:0)) until! (call_tag!(rma_complete,$:7)) | \
                                                  no! (call_tag!(buf_read,$:0)) until! (call_tag!(req_complete,$:8)) ) MSG \"Local Data Race - Local read by MPI call\"")

tag_rmacomplete = [("MPI_Win_fence", 1), ("MPI_Win_unlock", 1), ("MPI_Win_unlock_all", 0), ("MPI_Win_flush", 1), ("MPI_Win_flush_all", 0), ("MPI_Win_flush_local", 1), ("MPI_Win_flush_local_all", 0), ("MPI_Win_complete", 0)]
for func, win_idx in tag_rmacomplete:
    function_contracts[func]["TAGS"].append(f"rma_complete({win_idx})")
tag_p2pcomplete = [("MPI_Wait", 0), ("MPI_Test", 0)]
for func, req_idx in tag_p2pcomplete:
    function_contracts[func]["TAGS"].append(f"req_complete({req_idx})")

# RMA Epoch needs to be open
tag_needrmaepoch = [("MPI_Put", 7),
                    ("MPI_Get", 7),
                    ("MPI_Get_accumulate", 11),
                    ("MPI_Fetch_and_op", 6),
                    ("MPI_Compare_and_swap", 6)]
for func, win_idx in tag_needrmaepoch:
    function_contracts[func]["PRE"].append(f"( call_tag!(epoch_fence_create,$:{win_idx}) MSG \"No fence epoch\" ^ \
                                               call_tag!(epoch_lock_create,$:{win_idx})  MSG \"No lock epoch\" ^ \
                                               call_tag!(epoch_pscw_create,$:{win_idx})  MSG \"No PSCW epoch\") MSG \"Mixed sync or missing epoch\"")
tag_createfencermaepoch = [("MPI_Win_fence", 1)]
for func, win_idx in tag_createfencermaepoch:
    function_contracts[func]["TAGS"].append(f"epoch_fence_create({win_idx})")
tag_createlockrmaepoch = [("MPI_Win_lock", 3), ("MPI_Win_lock_all", 1)]
for func, win_idx in tag_createlockrmaepoch:
    function_contracts[func]["TAGS"].append(f"epoch_lock_create({win_idx})")
tag_createpscwepoch = [("MPI_Win_start", 2)]
for func, win_idx in tag_createpscwepoch:
    function_contracts[func]["TAGS"].append(f"epoch_pscw_create({win_idx})")
# RMA Window needs to be created
tag_rmawin = [("MPI_Put", 7),
              ("MPI_Get", 7),
              ("MPI_Accumulate", 8),
              ("MPI_Get_accumulate", 11),
              ("MPI_Fetch_and_op", 6),
              ("MPI_Compare_and_swap", 6)]
for func, win_idx in tag_rmawin:
    function_contracts[func]["PRE"].append(f"call_tag!(rma_createwin,$:&{win_idx}) MSG \"Window initialization missing\"")
tag_createwin = [("MPI_Win_create", 5), ("MPI_Win_allocate", 5)]
for func, win_idx in tag_createwin:
    function_contracts[func]["TAGS"].append(f"rma_createwin({win_idx})")

# No inflight calls when freeing
for func, win_idx in tag_rmawin:
    function_contracts[func]["POST"].append(f"no! (call!(MPI_Win_free,0:&{win_idx})) until! (call_tag!(rma_complete,$:{win_idx})) MSG \"Possible inflight call at MPI_Win_free\"")

# Make sure types are committed
tag_typegen = [("MPI_Type_contiguous", 2), ("MPI_Type_vector", 4)]
for func, tag_idx in tag_typegen:
    function_contracts[func]["POST"].append(f"no! (call_tag!(type_use,$:*{tag_idx})) until! (call!(MPI_Type_commit,0:{tag_idx})) MSG \"Type not committed before use\"")
    function_contracts[func]["POST"].append(f"no! (call!(MPI_Finalize)) until! (call!(MPI_Type_free,0:{tag_idx})) MSG \"Data type leak, free function not called\"")
    function_contracts[func]["POST"].append(f"no! (call_tag!(type_gen)) until! (call!(MPI_Type_free,0:{tag_idx})) MSG \"Data type leak, type handle lost\"")
    function_contracts[func]["TAGS"].append(f"type_gen({tag_idx})")

tag_typeuse = [("MPI_Send", 2), ("MPI_Isend", 2)]
for func, tag_idx in tag_typeuse:
    function_contracts[func]["TAGS"].append(f"type_use({tag_idx})")

# Output file
boilerplate_header = f"""
// Automatically generated by {os.path.basename(__file__)}
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

with open(f"{output_path}/mpi_contracts.h", "w") as contr_file:
    contr_file.write(header_output)
