# Alias Annotations

This document demonstrates the annotation framework for CoVer, specifically for annotating aliases.

## Overview

These functions are used in a contract such that `f` must occur before `g`, and must be called with the same input:

```c
void f(int* a) {};
void g(int* b) CONTRACT(
    PRE {
        call!(f,0:0)
    }
) {};
```

The `aliasgenerator` function artificially defeats the alias analysis of the tool:

```c
void aliasgenerator(int** a, int** b) {
    *a = *b;
}

int main() {
    int x;
    int* a;
    int* b = &x;
    aliasgenerator(&a, &b);
    f(a);
    g(b);
    return 0;
}
```

The static analysis will report an error here:
The alias analysis is unable to match `a` and `b` due to the flow through `aliasgenerator`.
To fix it, alias annotations using `CoVer_AnnotAlias` can be used.

## Annotated IR

`CoVer_AnnotAlias` allows annotating aliasing as either alias or no-alias.
This is done in groups: the values of an alias group all alias each other, the values of a no-alias group do not alias each other.
Below is the corresponding IR, with annotations prefixed by `+`:

```llvm
; Function Attrs: noinline nounwind sspstrong uwtable
define i32 @main() #0 {
  %1 = alloca i32, align 4
  %2 = alloca ptr, align 8
  %3 = alloca ptr, align 8
  store ptr %1, ptr %3, align 8
  call void @aliasgenerator(ptr noundef %2, ptr noundef %3)
  %4 = load ptr, ptr %2, align 8
+ call void @CoVer_AnnotAlias(ptr %4, i1 true, i32 0)
  call void @f(ptr noundef %4)
  %5 = load ptr, ptr %3, align 8
+ call void @CoVer_AnnotAlias(ptr %5, i1 true, i32 0)
  call void @g(ptr noundef %5)
  ret i32 0
}
```

## Source Level Annotations

Alias annotations can also be written directly in the source code, annotating a variable after it was assigned:

```c
MPI_Request* req = acquireRequest();
CoVer_AnnotAlias(req, true, 0);
MPI_Isend(buf, 1, MPI_INT, 1, 0, MPI_COMM_WORLD, req);

MPI_Request* pending = lastRequest();
CoVer_AnnotAlias(pending, true, 0);
MPI_Wait(pending, MPI_STATUS_IGNORE);
```

Without optimization, every use of a variable loads it again, so the annotated value and the values passed to `MPI_Isend` and `MPI_Wait` are distinct IR values.
If a value is not annotated itself, the static analysis therefore treats it as a member of a group if it is computed the same way as an annotated value
from the same variable contents, e.g. another load of the same variable, or `&reqs[i]` for an annotated `&reqs[i]` with no change to `i` in between.
This also distinguishes annotations of the same expression with different values of the variables involved:

```c
CoVer_AnnotAlias(&reqs[idx], false, 0);
MPI_Isend(&a, 1, MPI_INT, 1, 0, MPI_COMM_WORLD, &reqs[idx]); // Matches the first annotation
idx++;
CoVer_AnnotAlias(&reqs[idx], false, 0);
MPI_Isend(&b, 1, MPI_INT, 1, 1, MPI_COMM_WORLD, &reqs[idx]); // Matches the second annotation
```

In a no-alias group, two values are only separated if they match different annotations, e.g. two annotated loads of `buf` with `buf += N` in between.

## Alias or No-Alias

- Use an **alias** group (`true`) when the analysis does not match values that are the same at runtime,
  e.g. the request passed to `MPI_Wait` is not recognised as the one passed to `MPI_Isend`.
- Use a **no-alias** group (`false`) when the analysis matches values that differ at runtime,
  e.g. the requests `&reqs[0]` and `&reqs[1]` of two `MPI_Isend` calls, or a store into another part of an array taken as a write to the buffer of an `MPI_Isend`.

## Static Analysis

Each value the analysis compares is looked up in the groups: it matches a member if it is that annotated value or computed the same way, as described above.
- If two values match members of the same alias group, the analysis takes them as the same.
- If two values match *different* members of the same no-alias group, the analysis takes them as different.
  Two values matching the same member are not separated.
- A group only applies if both values match one of its members.
  If one of them matches no member, the group does not affect the comparison and the analysis matches the two values as without it.
  E.g. if the requests of several `MPI_Isend` calls are members of a no-alias group, but the request passed to `MPI_Wait` is not,
  the `MPI_Wait` request is still matched with the `MPI_Isend` requests the same way as before.

The order in which the annotations are added does not matter,
only the order in which they are executed.

A group must cover both values the analysis compares, so annotate them where they are used, not where the variable is assigned.
For a data race on a buffer, these are the buffer passed to the call and the address of each store into the buffer.
In a no-alias group, a value that is only used as the address of loads and stores, such as a store address,
asserts that each address it accesses must differ from the values the group's other annotations produced before that access (execution order), but annotations running later may produce it again.
So the stores filling a buffer can be in the same group as that buffer, as the buffer's annotation runs after them,
while a store into a buffer that is still in flight violates the group:

```c
CoVer_AnnotAlias(buf, false, 0);
MPI_Isend(buf, N, MPI_DOUBLE, peer, 0, MPI_COMM_WORLD, &r0);
buf += N;
for (int i = 0; i < N; i++) {
    CoVer_AnnotAlias(&buf[i], false, 0); // Not the buffer in flight
    buf[i] = i;
}
CoVer_AnnotAlias(buf, false, 0);
MPI_Isend(buf, N, MPI_DOUBLE, peer, 1, MPI_COMM_WORLD, &r1);
```

The same in IR, with annotations prefixed by `+`:

```llvm
define void @fill_and_send(ptr %0, i32 %1, i32 %2) {
  %4 = alloca ptr
  %5 = alloca i32
  %6 = alloca i32
  %7 = alloca ptr
  %8 = alloca ptr
  %9 = alloca i32
  store ptr %0, ptr %4
  store i32 %1, ptr %5
  store i32 %2, ptr %6
  %10 = load ptr, ptr %4
+ call void @CoVer_AnnotAlias(ptr %10, i1 false, i32 0)
  %11 = load i32, ptr %5
  %12 = load i32, ptr %6
  %13 = call i32 @MPI_Isend(ptr %10, i32 %11, ptr @ompi_mpi_double, i32 %12, i32 0, ptr @ompi_mpi_comm_world, ptr %7)
  %14 = load i32, ptr %5
  %15 = load ptr, ptr %4
  %16 = sext i32 %14 to i64
  %17 = getelementptr double, ptr %15, i64 %16
  store ptr %17, ptr %4
  store i32 0, ptr %9
  br label %18

18: ; preds = %29, %3
  %19 = load i32, ptr %9
  %20 = load i32, ptr %5
  %21 = icmp slt i32 %19, %20
  br i1 %21, label %22, label %32

22: ; preds = %18
  %23 = load i32, ptr %9
  %24 = sitofp i32 %23 to double
  %25 = load ptr, ptr %4
  %26 = load i32, ptr %9
  %27 = sext i32 %26 to i64
  %28 = getelementptr double, ptr %25, i64 %27
+ call void @CoVer_AnnotAlias(ptr %28, i1 false, i32 0) ; Not the buffer in flight
  store double %24, ptr %28
  br label %29

29: ; preds = %22
  %30 = load i32, ptr %9
  %31 = add i32 %30, 1
  store i32 %31, ptr %9
  br label %18

32: ; preds = %18
  %33 = load ptr, ptr %4
+ call void @CoVer_AnnotAlias(ptr %33, i1 false, i32 0)
  %34 = load i32, ptr %5
  %35 = load i32, ptr %6
  %36 = call i32 @MPI_Isend(ptr %33, i32 %34, ptr @ompi_mpi_double, i32 %35, i32 1, ptr @ompi_mpi_comm_world, ptr %8)
  ret void
}
```

## `CoVer_AnnotAlias` Arguments

| Position | Description |
|----------|-------------|
| 1st | The pointer being annotated |
| 2nd | Whether the group is an alias/no-alias annotation (`true`/`false`) |
| 3rd | The group it belongs to |

## `CoVer_AnnotAliasEpoch`

`CoVer_AnnotAliasEpoch(int group)` starts a new epoch for an alias group.
The runtime annotation verifier forgets all values of the group seen so far, so values from before the epoch are not compared against values after it.
The static analysis does not use epochs.
Annotated code that runs more than once, e.g. a function called in every time step, needs an epoch at its start:
each run usually reuses the memory of the previous one, e.g. the same request array, so its values alias those of the previous run.

```llvm
define void @comm_round() {
+ call void @CoVer_AnnotAliasEpoch(i32 0)
  ...
}
```

## Constraints

- Only pointer values can be annotated
- All alias annotations **must** share the same type within a group (aliasing or not aliasing).
- All alias annotations in IR **must** occur after the value they reference is created.
- A source level annotation applies to the uses of the variable until the variable may have been written.
  The analysis is conservative here: If the variable's address is passed to a function or written through a pointer, a write in between may not be ruled out.
  In that case, annotate again directly before the use.
- Source level annotations only apply to uses in the same function.
