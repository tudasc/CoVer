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

## `CoVer_AnnotAlias` Arguments

| Position | Description |
|----------|-------------|
| 1st | The value being annotated |
| 2nd | Whether the group is an alias/no-alias annotation (`true`/`false`) |
| 3rd | The group it belongs to |

## `CoVer_AnnotAliasEpoch`

`CoVer_AnnotAliasEpoch(int group)` starts a new epoch for an alias group.
The runtime annotation verifier forgets all values of the group seen so far, so values from before the epoch are not compared against values after it.
This is useful if the same memory is reused for different purposes over time, e.g. a request array that is reused for each round of communication after all requests have been completed:

```llvm
define void @comm_round() {
+ call void @CoVer_AnnotAliasEpoch(i32 0)
  ...
}
```

## Constraints

- All alias annotations **must** share the same type within a group (aliasing or not aliasing).
- All alias annotations **must** occur directly after the value they reference is created (i.e. the value first occurs).
