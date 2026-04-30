#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

// Annotation Interface
void __attribute__((visibility("default"))) CoVer_AnnotFP(void* ptr, void* func);
void __attribute__((visibility("default"))) CoVer_AnnotAlias(void* ptr, bool shouldAlias, int group);

// Stack allocation intrinsics (i.e. for IR allocas)
void __attribute__((visibility("default"))) CoVer_AllocStack(void const* ptr, size_t size) {};
void __attribute__((visibility("default"))) CoVer_FreeStack(void const* ptr) {};

// Global "allocation" intrinsic (i.e. for Global variables or pseudoglobals in fortran)
void __attribute__((visibility("default"))) CoVer_RegisterGlobal(void const* ptr, int64_t size) {};

// Fortran intrinsics - allocate(), wrapped by CoVer_FPointerAllocate
// Example IR for allocate(buf(2,2)):
//   call void @_FortranAPointerSetBounds(ptr @_QFEbuf, i32 0, i64 2, i64 2), !dbg !82
//   call void @_FortranAPointerSetBounds(ptr @_QFEbuf, i32 1, i64 2, i64 2), !dbg !82
//   %23 = call i32 @_FortranAPointerAllocate(ptr @_QFEbuf, i1 false, ptr null, ptr @_QQclX3a61d3c3006198469069f977f45ff921, i32 15), !dbg !82
void __attribute__((weak)) _FortranAPointerSetBounds(void*, int32_t, int64_t, int64_t);
int32_t __attribute__((weak)) _FortranAPointerAllocate(void*, bool, void*, void*, int32_t);
int32_t __attribute__((weak)) _FortranAPointerDeallocate(void*, bool, void*, void*, int32_t);

int32_t __attribute__((visibility("default"))) CoVer_FPointerAllocate(void* ptr, int64_t size, int num_dims, ...) {
    va_list list;
    va_start(list, num_dims);
    for (int i = 0; i < num_dims; i++) {
        int dim_idx = va_arg(list, int32_t);
        int64_t start_idx = va_arg(list, int64_t);
        int64_t end_idx = va_arg(list, int64_t);
        _FortranAPointerSetBounds(ptr, dim_idx, start_idx, end_idx);
    }
    bool palloc_arg1 = va_arg(list, int);
    void* palloc_arg2 = va_arg(list, void*);
    void* fileArg = va_arg(list, void*);
    int32_t palloc_arg4 = va_arg(list, int32_t);
    return _FortranAPointerAllocate(ptr, palloc_arg1, palloc_arg2, fileArg, palloc_arg4);
}

// Fortran intrinsics - deallocate(), wrapped by CoVer_FPointerDeallocate
// Example IR for deallocate(buf):
//   %54 = call i32 @_FortranAPointerDeallocate(ptr @_QFEbuf, i1 false, ptr null, ptr @_QQclX67d9a87547f1793fa4a21c08cb286920, i32 18), !dbg !92
int32_t __attribute__((visibility("default"))) CoVer_FPointerDeallocate(void* ptr, bool arg1, void* arg2, void* fileArg, int32_t arg4) {
    return _FortranAPointerDeallocate(ptr, arg1, arg2, fileArg, arg4);
}
