# Allocation Annotations

This document demonstrates the annotation framework for CoVer, specifically for annotating allocated memory.

## Overview

Consider an MPI program that aligns a heap buffer to 64 bytes by hand before using it for communication:

```c
int main() {
    MPI_Request req;
    MPI_Init(NULL, NULL);
    char* raw = malloc(1024 + 64);
    int* buf = (int*)(((uintptr_t)raw + 63) & ~(uintptr_t)63); // Align to 64 bytes
    MPI_Irecv(buf, 256, MPI_INT, 0, 0, MPI_COMM_WORLD, &req);
    MPI_Wait(&req, MPI_STATUS_IGNORE);
    MPI_Finalize();
    free(raw);
    return 0;
}
```

The static analysis will report an error here:
The analysis tracks allocated pointers through stores, loads and address computations (GEPs), but not through integer arithmetic.
After the alignment computation, it can no longer relate `buf` to the allocation of `raw`, and reports that the buffer passed to `MPI_Irecv` is not allocated.
To fix it, allocation annotations using `CoVer_AnnotAlloc` can be used.

## Annotated IR

`CoVer_AnnotAlloc` marks a pointer as pointing to allocated memory.
For the static analysis, a call to `CoVer_AnnotAlloc(ptr, size)` is treated exactly like `ptr = malloc(size)`:
From the annotation onwards, `ptr` and all values derived from it (e.g. through stores, loads or GEPs) are considered allocated.
Below is the corresponding IR, with annotations prefixed by `+`:

```llvm
define i32 @main() {
  ...
  %5 = call i32 @MPI_Init(ptr noundef null, ptr noundef null)
  %6 = call noalias ptr @malloc(i64 noundef 1088)
  store ptr %6, ptr %3, align 8
  %7 = load ptr, ptr %3, align 8
  %8 = ptrtoint ptr %7 to i64
  %9 = add i64 %8, 63
  %10 = and i64 %9, -64
  %11 = inttoptr i64 %10 to ptr
+ call void @CoVer_AnnotAlloc(ptr %11, i64 1024)
  store ptr %11, ptr %4, align 8
  %12 = load ptr, ptr %4, align 8
  %13 = call i32 @MPI_Irecv(ptr noundef %12, i32 noundef 256, ptr noundef @ompi_mpi_int, i32 noundef 0, i32 noundef 0, ptr noundef @ompi_mpi_comm_world, ptr noundef %2)
  ...
}
```

The annotation is placed directly after the instruction producing the pointer.

## `CoVer_AnnotAlloc` Arguments

| Position | Description |
|----------|-------------|
| 1st | The pointer being annotated as allocated |
| 2nd | The minimum number of bytes allocated at the pointer, or `0` if unknown |

## Constraints

- All allocation annotations **must** occur directly after the value they reference is created.
- The annotated size **must not** exceed the actual allocation size. If the size is unknown, use `0`.
