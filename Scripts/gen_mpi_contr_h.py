import os
import sys
import re
import json
import random

if len(sys.argv) < 3:
    print(f"Insufficient arguments!\nUsage: {os.path.basename(__file__)} <outpath> <mpi.h location> [identifier]")
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
safetylist = [
]


function_decls = {}
function_contracts = {}

# Get all function definitions
with open(inputh_location) as f:
    h_iter = iter(f.readlines())
    for line in h_iter:
        if re.match(r"[ \tA-z0-9()\"]+.* MPI_[\w]+[ \t]*\([^\(]*$", line):
            # Found a function definition. Get the function name
            res = re.search(r" (MPI_[a-zA-Z_0-9]+) ?\(", line)
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
                if ";" in funcdec or "{" in funcdec:
                    break
                line = next(h_iter)
                funcdec += line

            # Check if definition, not declaration
            if "{" in funcdec:
                while not "}" in line:
                    line = next(h_iter)
                continue

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
def add_contract(func: str, scope: str, contr: str):
    if func in function_contracts:
        function_contracts[func][scope].append(contr)
    else:
        print(f"WARNING: Function \"{func}\" not found in MPI header! Probably an outdated MPI implementation!", file=sys.stderr)

# Call MPI initializer
for func in function_decls.keys():
    if func in ["MPI_Init", "MPI_Init_thread", "MPI_Session_init"]:
        add_contract(func, "TAGS", "mpi_init")
        continue
    add_contract(func, "PRE", "call_tag!(mpi_init) MSG \"Missing Initialization call\"")

# Call MPI finalizer
for func in function_decls.keys():
    if func in ["MPI_Finalize", "MPI_Abort"]:
        add_contract(func, "TAGS", "mpi_exit")
        continue
    add_contract(func, "POST", "call_tag!(mpi_exit) MSG \"Missing Finalization call\"")

# No request reuse until p2pcomplete for persistent comms, request must be completed
tag_reqgen = [("MPI_Iallgather", 7),
              ("MPI_Iallreduce", 6),
              ("MPI_Ialltoall", 7),
              ("MPI_Ibarrier", 1),
              ("MPI_Ibcast", 5),
              ("MPI_Igather", 8),
              ("MPI_Ireduce", 7),
              ("MPI_Ibsend", 6),
              ("MPI_Isend", 6),
              ("MPI_Irsend", 6),
              ("MPI_Issend", 6),
              ("MPI_Irecv", 6),
              ("MPI_Imrecv", 4),
              ("MPI_Isendrecv", 11),
              ("MPI_Start", 0)]
for func, tag_idx in tag_reqgen:
    add_contract(func, "POST", f"(no! (call_tag!(request_gen,$:{tag_idx})) until! (call_tag!(req_complete,$:{tag_idx})) | \
                                  no! (call_tag!(request_gen,$:{tag_idx})) until! (call_tag!(req_complete_noidx))) MSG \"Double Request Use\"")
    add_contract(func, "POST", f"(call_tag!(req_complete,$:{tag_idx}) | call_tag!(req_complete_noidx)) MSG \"Request Leak\"")
    add_contract(func, "TAGS", f"request_gen({tag_idx})")
# Also add Rget, Rput to req_gen to satisfy unmatched wait, but not the other contrs
add_contract("MPI_Rget", "TAGS", f"request_gen(8)")
add_contract("MPI_Rput", "TAGS", f"request_gen(8)")
add_contract("MPI_Raccumulate", "TAGS", f"request_gen(9)")
# Unmatched Wait
add_contract("MPI_Wait", "PRE", f"call_tag!(request_gen,$:0) MSG \"Unmatched Wait\"")

# Local data races
tag_buf = [("RMAWIN", "MPI_Put", 0, 7, "W", "R"),
           ("RMAWIN", "MPI_Get", 0, 7, "RW", "W"),
           ("RMAWIN", "MPI_Get_accumulate", 0, 11, "W", "R"),
           ("RMAWIN", "MPI_Get_accumulate", 3, 11, "RW", "W"),
           ("RMAWIN", "MPI_Accumulate", 0, 8, "W", "R"),
           ("RMAWIN", "MPI_Fetch_and_op", 0, 6, "W", "R"),
           ("RMAWIN", "MPI_Fetch_and_op", 1, 6, "RW", "W"),
           ("RMAWIN", "MPI_Compare_and_swap", 0, 6, "W", "R"),
           ("RMAWIN", "MPI_Compare_and_swap", 1, 6, "W", "R"),
           ("RMAWIN", "MPI_Compare_and_swap", 2, 6, "RW", "W"),
           ("REQ", "MPI_Isend", 0, 6, "W", "R"),
           ("REQ", "MPI_Ibsend", 0, 6, "W", "R"),
           ("REQ", "MPI_Irsend", 0, 6, "W", "R"),
           ("REQ", "MPI_Issend", 0, 6, "W", "R"),
           ("REQ", "MPI_Irecv", 0, 6, "RW", "W"),
           ("REQ", "MPI_Imrecv", 0, 4, "RW", "W"),
           ("REQ", "MPI_Ibcast", 0, 5, "W", "R"),
           ("REQ", "MPI_Igather", 0, 8, "W", "R"),
           ("REQ", "MPI_Igather", 3, 8, "RW", "W"),
           ("REQ", "MPI_Iscan", 0, 6, "W", "R"),
           ("REQ", "MPI_Iscan", 1, 6, "RW", "W"),
           ("REQ", "MPI_Iscatter", 0, 8, "W", "R"),
           ("REQ", "MPI_Iscatter", 3, 8, "RW", "W"),
           ("REQ", "MPI_Ialltoall", 0, 7, "W", "R"),
           ("REQ", "MPI_Ialltoall", 3, 7, "RW", "W"),
           ("REQ", "MPI_Iallreduce", 0, 6, "W", "R"), # Send buffer - No writing
           ("REQ", "MPI_Iallreduce", 1, 6, "RW", "W"), # Recv buffer - No RW
           ("REQ", "MPI_Ireduce", 0, 7, "W", "R"),
           ("REQ", "MPI_Ireduce", 1, 7, "RW", "W"),
]
for calltype, func, buf_idx, mark_idx, forbid, action in tag_buf:
    if calltype == "RMAWIN": completiontag = "rma_complete"
    if calltype == "REQ": completiontag = "req_complete"
    if calltype != "EITHER":
        if "R" in forbid:
            if calltype == "REQ":
                add_contract(func, "POST", f"(no! (read!(*{buf_idx})) until! (call_tag!({completiontag},$:{mark_idx})) | \
                                              no! (read!(*{buf_idx})) until! (call_tag!(req_complete_noidx))) MSG \"Local Data Race - Local read\"")
                add_contract(func, "POST", f"(no! (call_tag!(buf_read,$:{buf_idx})) until! (call_tag!({completiontag},$:{mark_idx})) | \
                                              no! (call_tag!(buf_read,$:{buf_idx})) until! (call_tag!(req_complete_noidx))) MSG \"Local Data Race - Local read by call\"")
            else:
                add_contract(func, "POST", f"no! (read!(*{buf_idx})) until! (call_tag!({completiontag},$:{mark_idx})) MSG \"Local Data Race - Local read\"")
                add_contract(func, "POST", f"no! (call_tag!(buf_read,$:{buf_idx})) until! (call_tag!({completiontag},$:{mark_idx})) MSG \"Local Data Race - Local read by call\"")
        if "W" in forbid:
            if calltype == "REQ":
                add_contract(func, "POST", f"(no! (write!(*{buf_idx})) until! (call_tag!({completiontag},$:{mark_idx})) | \
                                              no! (write!(*{buf_idx})) until! (call_tag!(req_complete_noidx))) MSG \"Local Data Race - Local write\"")
                add_contract(func, "POST", f"(no! (call_tag!(buf_write,$:{buf_idx})) until! (call_tag!({completiontag},$:{mark_idx})) | \
                                              no! (call_tag!(buf_write,$:{buf_idx})) until! (call_tag!(req_complete_noidx))) MSG \"Local Data Race - Local write by call\"")
            else:
                add_contract(func, "POST", f"no! (write!(*{buf_idx})) until! (call_tag!({completiontag},$:{mark_idx})) MSG \"Local Data Race - Local write\"")
                add_contract(func, "POST", f"no! (call_tag!(buf_write,$:{buf_idx})) until! (call_tag!({completiontag},$:{mark_idx})) MSG \"Local Data Race - Local write by call\"")
    if "R" in action:
        add_contract(func, "TAGS", f"buf_read({buf_idx})")
    if "W" in action:
        add_contract(func, "TAGS", f"buf_write({buf_idx})")

# Blocking functions that write/read a buffer
tag_buf_blocking = [("MPI_Send", 0, "R"), ("MPI_Recv", 0, "W"), ("MPI_Reduce", 0, "R"), ("MPI_Reduce", 1, "W"), ("MPI_Allreduce", 0, "R"), ("MPI_Allreduce", 1, "W"),
                    ("MPI_Bcast", 0, "RW")]
for func, idx, access in tag_buf_blocking:
    if "R" in access: add_contract(func, "TAGS", f"buf_read({idx})")
    if "W" in access: add_contract(func, "TAGS", f"buf_write({idx})")

# Special handling of request-based RMA (Allow completion using both RMA sync and request)
tag_buf_either = [("MPI_Rput", 0, 7, 8, "W", "R"),
                  ("MPI_Rget", 0, 7, 8, "RW", "W"),
                  ("MPI_Raccumulate", 0, 8, 9, "W", "R"),
                  ("MPI_Rget_accumulate", 0, 11, 12, "W", "R"),
                  ("MPI_Rget_accumulate", 3, 11, 12, "RW", "W"),
]
for func, buf_idx, win_idx, req_idx, forbid, action in tag_buf_either:
    if "R" in forbid:
        add_contract(func, "POST", f"( no! (read!(*{buf_idx})) until! (call_tag!(rma_complete,$:{win_idx})) | no! (read!(*{buf_idx})) until! (call_tag!(req_complete,$:{req_idx})) ) MSG \"Local Data Race - Local read\"")
        add_contract(func, "POST", f"( no! (call_tag!(buf_read,$:{buf_idx})) until! (call_tag!(rma_complete,$:{win_idx})) | no! (call_tag!(buf_read,$:{buf_idx})) until! (call_tag!(req_complete,$:{req_idx})) ) MSG \"Local Data Race - Local read by MPI call\"")
    if "W" in forbid:
        add_contract(func, "POST", f"( no! (write!(*{buf_idx})) until! (call_tag!(rma_complete,$:{win_idx})) | no! (write!(*{buf_idx})) until! (call_tag!(req_complete,$:{req_idx})) ) MSG \"Local Data Race - Local write\"")
        add_contract(func, "POST", f"( no! (call_tag!(buf_write,$:{buf_idx})) until! (call_tag!(rma_complete,$:{win_idx})) | no! (call_tag!(buf_write,$:{buf_idx})) until! (call_tag!(req_complete,$:{req_idx})) ) MSG \"Local Data Race - Local write by MPI call\"")
    if "R" in action:
        add_contract(func, "TAGS", f"buf_read({buf_idx})")
    if "W" in action:
        add_contract(func, "TAGS", f"buf_write({buf_idx})")

tag_rmacomplete = [("MPI_Win_fence", 1), ("MPI_Win_unlock", 1), ("MPI_Win_unlock_all", 0), ("MPI_Win_flush", 1), ("MPI_Win_flush_all", 0), ("MPI_Win_flush_local", 1), ("MPI_Win_flush_local_all", 0), ("MPI_Win_complete", 0)]
for func, win_idx in tag_rmacomplete:
    add_contract(func, "TAGS", f"rma_complete({win_idx})")
tag_p2pcomplete = [("MPI_Wait", 0), ("MPI_Test", 0)]
for func, req_idx in tag_p2pcomplete:
    add_contract(func, "TAGS", f"req_complete({req_idx})")

tag_p2pcomplete_noidx = [("MPI_Waitall")]
for func in tag_p2pcomplete_noidx:
    add_contract(func, "TAGS", f"req_complete_noidx")

# RMA Epoch needs to be open
tag_needrmaepoch = [("MPI_Put", 7),
                    ("MPI_Get", 7),
                    ("MPI_Get_accumulate", 11),
                    ("MPI_Fetch_and_op", 6),
                    ("MPI_Compare_and_swap", 6)]
for func, win_idx in tag_needrmaepoch:
    add_contract(func, "PRE", f"( call_tag!(epoch_fence_create,$:{win_idx}) MSG \"No fence epoch\" ^ \
                                  call_tag!(epoch_lock_create,$:{win_idx})  MSG \"No lock epoch\" ^ \
                                  call_tag!(epoch_pscw_create,$:{win_idx})  MSG \"No PSCW epoch\") MSG \"Mixed sync or missing epoch\"")
tag_createfencermaepoch = [("MPI_Win_fence", 1)]
for func, win_idx in tag_createfencermaepoch:
    add_contract(func, "TAGS", f"epoch_fence_create({win_idx})")
    add_contract(func, "PRE", f"call_tag!(rma_createwin,$:&{win_idx}) MSG \"Did not create window before fence!\"")
tag_createlockrmaepoch = [("MPI_Win_lock", 3), ("MPI_Win_lock_all", 1)]
for func, win_idx in tag_createlockrmaepoch:
    add_contract(func, "TAGS", f"epoch_lock_create({win_idx})")
tag_createpscwepoch = [("MPI_Win_start", 2)]
for func, win_idx in tag_createpscwepoch:
    add_contract(func, "TAGS", f"epoch_pscw_create({win_idx})")
# RMA Window needs to be created
tag_rmawin = [("MPI_Put", 7),
              ("MPI_Get", 7),
              ("MPI_Accumulate", 8),
              ("MPI_Get_accumulate", 11),
              ("MPI_Fetch_and_op", 6),
              ("MPI_Compare_and_swap", 6)]
for func, win_idx in tag_rmawin:
    add_contract(func, "PRE", f"call_tag!(rma_createwin,$:&{win_idx}) MSG \"Window initialization missing\"")
tag_createwin = [("MPI_Win_create", 5), ("MPI_Win_allocate", 5)]
for func, win_idx in tag_createwin:
    add_contract(func, "TAGS", f"rma_createwin({win_idx})")

# No inflight calls when freeing
for func, win_idx in tag_rmawin:
    add_contract(func, "POST", f"no! (call!(MPI_Win_free,0:&{win_idx})) until! (call_tag!(rma_complete,$:{win_idx})) MSG \"Possible inflight call at MPI_Win_free\"")

# Make sure types are committed
tag_typegen = [("MPI_Type_contiguous", 2), ("MPI_Type_vector", 4)]
for func, tag_idx in tag_typegen:
    add_contract(func, "POST", f"no! (call_tag!(type_use,$:*{tag_idx})) until! (call!(MPI_Type_commit,0:{tag_idx})) MSG \"Type not committed before use\"")
    add_contract(func, "POST", f"no! (call!(MPI_Finalize)) until! (call!(MPI_Type_free,0:{tag_idx})) MSG \"Data type leak, free function not called\"")
    add_contract(func, "POST", f"no! (call_tag!(type_gen,$:{tag_idx})) until! (call!(MPI_Type_free,0:{tag_idx})) MSG \"Data type leak, type handle lost\"")
    add_contract(func, "TAGS", f"type_gen({tag_idx})")

tag_typeuse = [("MPI_Send", 2), ("MPI_Isend", 2)]
for func, tag_idx in tag_typeuse:
    add_contract(func, "TAGS", f"type_use({tag_idx})")

# Parameter Errors
paramerror_comm = [
    ("MPI_Send", 5),
    ("MPI_Isend", 5),
    ("MPI_Recv", 5),
    ("MPI_Irecv", 5),
    ("MPI_Sendrecv", 10),
    ("MPI_Allgather", 6),
    ("MPI_Cart_get", 0),
]
for func, idx in paramerror_comm:
    add_contract(func, "PRE", f"param!({idx}:!=NULL,!=MPI_COMM_NULL) MSG \"Communicator is invalid\"")

# MPI_PROC_NULL uses negative special value in OpenMPI, add exception for it
paramerror_rank_send = [
    ("MPI_Send", 3),
    ("MPI_Get", 3),
    ("MPI_Reduce", 5),
]
for func, idx in paramerror_rank_send:
    add_contract(func, "PRE", f"param!({idx}:^=MPI_PROC_NULL,>=0) MSG \"Rank is invalid\"")

# MPI_ANY_SOURCE uses negative special value in OpenMPI, add exception for it
paramerror_rank_recv = [
    ("MPI_Recv", 3),
]
for func, idx in paramerror_rank_recv:
    add_contract(func, "PRE", f"param!({idx}:^=MPI_PROC_NULL,^=MPI_ANY_SOURCE,>=0) MSG \"Rank is invalid\"")

# MPI_STATUSES_IGNORE == NULL == MPI_STATUS_IGNORE in OpenMPI. Add exception for MPI_STATUS_IGNORE
paramerror_status = [
    ("MPI_Wait", 1),
]
for func, idx in paramerror_status:
    add_contract(func, "PRE", f"param!({idx}:^=MPI_STATUS_IGNORE,!=NULL,!=MPI_STATUSES_IGNORE) MSG \"Status is invalid\"")

# Buffer should never be null
paramerror_null = [
    ("MPI_Initialized", 0),
    ("MPI_Send", 0),
    ("MPI_Isend", 0),
    ("MPI_Recv", 0),
    ("MPI_Irecv", 0),
    ("MPI_Sendrecv", 0),
    ("MPI_Win_create", 0),
    ("MPI_Win_allocate", 4),
    ("MPI_Get", 0),
]
for func, idx in paramerror_null:
    add_contract(func, "PRE", f"param!({idx}:!=NULL,!=MPI_IN_PLACE) MSG \"Parameter is null\"")

# Allow MPI_IN_PLACE for recv buffer of sendrecv
add_contract("MPI_Sendrecv", "PRE", f"param!(5:^=MPI_IN_PLACE,!=NULL) MSG \"Buffer is null\"")

# Comm buffer should be allocated
paramerror_null = [
    ("MPI_Send", 0),
    ("MPI_Recv", 0),
    ("MPI_Sendrecv", 0),
    ("MPI_Sendrecv", 5),
    ("MPI_Get", 0),
    ("MPI_Put", 0),
]
for func, idx in paramerror_null:
    add_contract(func, "PRE", f"alloc!({idx}) MSG \"Buffer is not allocated\"")

allocators = [
    ("MPI_Win_allocate", 4),
]
for func, idx in allocators:
    add_contract(func, "POST", f"alloc!(&{idx})")


# Datatype should not be null when doing communication
paramerror_datatype = [
    ("MPI_Get", 6),
    ("MPI_Bcast", 2),
]
for func, idx in paramerror_datatype:
    add_contract(func, "PRE", f"param!({idx}:!=NULL,!=MPI_DATATYPE_NULL) MSG \"Data type is invalid\"")

# When doing P2P sends, the tag should never be MPI_ANY_TAG
paramerror_tag_send = [
    ("MPI_Send", 4),
    ("MPI_Isend", 4),
]
for func, idx in paramerror_tag_send:
    add_contract(func, "PRE", f"param!({idx}:!=MPI_ANY_TAG,>=0) MSG \"Tag is invalid\"")

# MPI_OP_NULL should not be used for concrete operations
paramerror_op = [
    ("MPI_Reduce", 4),
]
for func, idx in paramerror_op:
    add_contract(func, "PRE", f"param!({idx}:!=NULL,!=MPI_OP_NULL) MSG \"Operation is invalid\"")

# Output file
boilerplate_header_c = f"""
// Automatically generated by {os.path.basename(__file__)}
// Instead of modifying this file, consider modifying the generation script
// Identifier: {ver_identifier}

#pragma once

#include "Contracts.h"
#include <mpi.h>

#define MACRO_SAFETY(x) (x)

// Constant values needed for contracts
CONTRACT_VALUE_PAIR(MPI_PROC_NULL, MPI_PROC_NULL)
CONTRACT_VALUE_PAIR(MPI_ANY_SOURCE, MPI_ANY_SOURCE)
CONTRACT_VALUE_PAIR(MPI_ANY_TAG, MPI_ANY_TAG)
CONTRACT_VALUE_PAIR(NULL, NULL)
CONTRACT_VALUE_PAIR(MPI_IN_PLACE,MPI_IN_PLACE)
CONTRACT_VALUE_PAIR(MPI_REQUEST_NULL,MPI_REQUEST_NULL)
CONTRACT_VALUE_PAIR(MPI_OP_NULL,MPI_OP_NULL)
CONTRACT_VALUE_PAIR(MPI_COMM_NULL,MPI_COMM_NULL)
CONTRACT_VALUE_PAIR(MPI_STATUSES_IGNORE,MPI_STATUSES_IGNORE)
CONTRACT_VALUE_PAIR(MPI_STATUS_IGNORE,MPI_STATUS_IGNORE)
CONTRACT_VALUE_PAIR(MPI_DATATYPE_NULL,MPI_DATATYPE_NULL)

void* calloc(size_t num, size_t size) CONTRACT( POST {{ alloc!(99) }});
void* malloc(size_t size) CONTRACT( POST {{ alloc!(99) }});

"""

header_output_c = boilerplate_header_c

boilerplate_header_fort = f"""
! Automatically generated by {os.path.basename(__file__)}
! Instead of modifying this file, consider modifying the generation script
! Identifier: {ver_identifier}

subroutine CONTRACT_DEFINITIONS_FORT_@RANDOM_UNIQUE@
    use contract_helper
"""

header_output_fort = boilerplate_header_fort + "    use mpi\n    implicit none\n"
header_output_fort_f08 = boilerplate_header_fort + "    use mpi_f08\n    implicit none\n"
header_output_fort_f08ts = header_output_fort_f08

exclude_fortran = [
    "MPI_Intercomm_create_from_groups",
    "MPI_Session_set_info",
    "MPI_Session_get_num_psets",
    "MPI_Status_get_error",
    "MPI_Status_set_error",
    "MPI_Status_get_source",
    "MPI_Status_set_source",
    "MPI_Status_get_tag",
    "MPI_Status_set_tag",
    "MPI_Status_f2f08",
    "MPI_Status_f082f",
    "MPI_Pcontrol",
    "MPI_DUP_FN"
] # HACK: Not currently implemented for fortran in major mpi impls
exclude_fortran_nof08 = [
    "MPI_Comm_attach_buffer",
    "MPI_Session_attach_buffer"
] # Only supported in modern MPI

with open("apis.json", "r") as api_file:
    mpi_funcs = json.load(api_file)

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

def has_buffer(func: str) -> bool:
    searchfunc = func.lower()
    if searchfunc.endswith("_c"): searchfunc = searchfunc[:-2]
    api_func = mpi_funcs[searchfunc]
    for p in api_func["parameters"]:
        if p["kind"] == "BUFFER": return True
    return False

def create_f08_contract(func: str, contract: str, support_ts: bool):
    call_op = re.search("call!\\(([^),]+)([^)]*)\\)", contract)
    contract_str_fort_f08 = contract
    if call_op:
        func_callop = call_op.group(1)
        suffix = "_f08ts" if support_ts and has_buffer(func_callop) else "_f08"
        contract_str_fort_f08 = re.sub("call!\\(([^),]+)([^)]*)\\)", r"call!(\1" + suffix + r"\2)", contract)
    contract_func_suffix = "_f08ts" if has_buffer(func) and support_ts else "_f08"
    return f"    call Declare_Contract({func}{contract_func_suffix}, \"{contract_str_fort_f08}\")\n"

for func, contrs in function_contracts.items():
    if not contrs["PRE"] and not contrs["POST"] and not contrs["TAGS"]:
        continue # No contracts, no need to output
    contract_str = create_contract_output_for_func(contrs.keys(), contrs)
    # First: C Stuff
    new_decl = function_decls[func][:-1]
    if func in safetylist:
        new_decl = new_decl.replace(func, f"MACRO_SAFETY({func})")
    header_output_c += new_decl + " CONTRACT(\n"
    header_output_c += contract_str
    header_output_c += ");\n\n"
    if func in exclude_fortran or "c2f" in func or "f2c" in func or "f082c" in func or "c2f08" in func or func.endswith("_c") or func.endswith("_fromint") or func.endswith("_toint"): continue # Only defined for C
    # Now: Fortran
    contract_str_fort = contract_str.replace('"', '""').replace("\n", "").replace("    ", "").replace("*", "").replace("&", "").replace("read!(", "read!(*").replace("write!(", "write!(*")
    if func not in exclude_fortran_nof08:
        header_output_fort += "    call Declare_Contract(" + func + ", \"" + contract_str_fort + "\")\n"
    # Fortran f08(ts)
    header_output_fort_f08 += create_f08_contract(func, contract_str_fort, False)
    header_output_fort_f08ts += create_f08_contract(func, contract_str_fort, True)

# End fortran subroutine
header_output_fort += "end subroutine\n"
header_output_fort_f08 += "end subroutine\n"
header_output_fort_f08ts += "end subroutine\n"

with open(f"{output_path}/mpi_contracts.h", "w") as contr_file:
    contr_file.write(header_output_c)

with open(f"{output_path}/mpi_contracts.f90", "w") as contr_file:
    contr_file.write(header_output_fort.replace("@RANDOM_UNIQUE@", ''.join(random.choice('0123456789ABCDEF') for i in range(16))))

with open(f"{output_path}/mpi_contracts_f08.f90", "w") as contr_file:
    contr_file.write(header_output_fort_f08.replace("@RANDOM_UNIQUE@", ''.join(random.choice('0123456789ABCDEF') for i in range(16))))

with open(f"{output_path}/mpi_contracts_f08ts.f90", "w") as contr_file:
    contr_file.write(header_output_fort_f08ts.replace("@RANDOM_UNIQUE@", ''.join(random.choice('0123456789ABCDEF') for i in range(16))))
