# Alias Annotations

This document demonstrates the annotation framework for CoVer, specifically for annotating aliases.

## Overview

These functions are used in a contract such that `f` must occur before `g`, and must be called with the same input:

```c
void f(int a) {};
void g(int b) CONTRACT(
    PRE {
        call!(f,0:0)
    }
) {};
```

The `aliasgenerator` function artificially defeats the alias analysis of the tool:

```c
void aliasgenerator(int* a, int* b) {
    *a = *b;
}

int main() {
    int a;
    int b;
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
This is done in groups, with all values in a group being aliases or not aliases.
Below is the corresponding IR, with annotations prefixed by `+`:

```llvm
; Function Attrs: noinline nounwind sspstrong uwtable
define i32 @main() #0 {
  %1 = alloca i32, align 4
  %2 = alloca i32, align 4
  call void @aliasgenerator(ptr noundef %1, ptr noundef %2)
  %3 = load i32, ptr %1, align 4
+ call void @CoVer_AnnotAlias(i32 %3, i1 true, i32 0)
  call void @f(i32 noundef %3)
  %4 = load i32, ptr %2, align 4
+ call void @CoVer_AnnotAlias(i32 %4, i1 true, i32 0)
  call void @g(i32 noundef %4)
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

For no-alias groups, two values referring to the same annotated variable are not separated.

## `CoVer_AnnotAlias` Arguments

| Position | Description |
|----------|-------------|
| 1st | The value being annotated |
| 2nd | Whether the group is an alias/no-alias annotation (`true`/`false`) |
| 3rd | The group it belongs to |

## `CoVer_AnnotAliasEpoch`

`CoVer_AnnotAliasEpoch(int group)` starts a new epoch for an alias group.
The runtime annotation verifier forgets all values of the group seen so far, so values from before the epoch are not compared against values after it.
This is useful if the same memory is reused for different purposes over time, e.g. a request array that is reused for each round of communication after all requests have been completed,
or if an annotated variable refers to a different object in each round:

```llvm
define void @comm_round() {
+ call void @CoVer_AnnotAliasEpoch(i32 0)
  ...
}
```

## Constraints

- All alias annotations **must** share the same type within a group (aliasing or not aliasing).
- All alias annotations in IR **must** occur after the value they reference is created.
- A source level annotation applies to the uses of the variable until the variable may have been written.
  The analysis is conservative here: If the variable's address is passed to a function or written through a pointer, a write in between may not be ruled out.
  In that case, annotate again directly before the use.
- Source level annotations only apply to uses in the same function.
