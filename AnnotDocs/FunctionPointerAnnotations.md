# Function Pointer Annotations

This document demonstrates the annotation framework for CoVer, specifically for annotating function pointers.

## Overview

These functions are used in a contract such that `f1` or `f2` must occur before `g`, where the input to `g` must match the second or third argument of `f1`/`f2` respectively:

```c
void f1(int* a, int* relevant, int* c) CONTRACT(TAGS {exampletag(1)}) {};
void f2(int* a, int* b, int* relevant) CONTRACT(TAGS {exampletag(2)}) {};
void g(int* b) CONTRACT(
    PRE {
        call_tag!(exampletag,$:0)
    }
) {};
```

The static analysis will fail on the given code due to the use of the function pointer `func`,
since the static analysis does not support indirect function calls:

```c
int main(int argc, char** argv) {
    int a = 0;
    void (*func)(int* a, int* b, int* c);
    if (argc > 1) func = f1;
    else func = f2;
    func(&a,&a,&a);
    g(&a);
}
```

To fix it, function pointer annotations can be used.

## Annotated IR

In the event of an indirect function call,
the `CoVer_AnnotFP` annotation can be used to list all possible function targets,
which the static analysis can use to improve analysis accuracy. 
Below is the corresponding IR, with annotations prefixed by `+`:

```llvm
; Function Attrs: noinline nounwind sspstrong uwtable
define i32 @main(i32 noundef %0, ptr noundef %1) #4 {
  %3 = alloca i32, align 4
  %4 = alloca i32, align 4
  %5 = alloca ptr, align 8
  %6 = alloca i32, align 4
  %7 = alloca ptr, align 8
  store i32 0, ptr %3, align 4
  store i32 %0, ptr %4, align 4
  store ptr %1, ptr %5, align 8
  store i32 0, ptr %6, align 4
  %8 = load i32, ptr %4, align 4
  %9 = icmp sgt i32 %8, 1
  br i1 %9, label %11, label %10

10:                                               ; preds = %2
  store ptr @f2, ptr %7, align 8
  br label %12

11:                                               ; preds = %2
  store ptr @f1, ptr %7, align 8
  br label %12

12:                                               ; preds = %10, %11
  %13 = load ptr, ptr %7, align 8
+ call void (ptr, i32, ...) @CoVer_AnnotFP(ptr %13, i32 2, ptr @f1, ptr @f2)
  call void %13(ptr noundef %6, ptr noundef %6, ptr noundef %6)
  call void @g(ptr noundef %6)
  ret i32 0
}
```

## `CoVer_AnnotFP` Arguments

| Position | Description |
|----------|-------------|
| 1st | The value being annotated |
| 2nd | The number of possible function pointer targets (`n`) |
| 3rd to `(2+n)`th | The possible called functions |

## Constraints

- All function pointer annotations **must** occur directly before the indirect call is made.
- All function pointer annotations **must** include all possible function targets.
