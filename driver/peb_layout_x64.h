#pragma once
#include <ntifs.h>

/* Native x64 layout only; not the WOW64 PEB. These are private ABI views,
   not replacements for the WDK's forward declarations. */
typedef struct _DUMPER_PEB_X64 {
    UCHAR Reserved00[0x10];
    PVOID ImageBaseAddress;
    PVOID Ldr;
} DUMPER_PEB_X64;

typedef struct _DUMPER_PEB_LDR_DATA_X64 {
    UCHAR Reserved00[0x10];
    LIST_ENTRY InLoadOrderModuleList;
    LIST_ENTRY InMemoryOrderModuleList;
    LIST_ENTRY InInitializationOrderModuleList;
} DUMPER_PEB_LDR_DATA_X64;

typedef struct _DUMPER_LDR_DATA_TABLE_ENTRY_X64 {
    LIST_ENTRY InLoadOrderLinks;
    LIST_ENTRY InMemoryOrderLinks;
    LIST_ENTRY InInitializationOrderLinks;
    PVOID DllBase;
    PVOID EntryPoint;
    ULONG SizeOfImage;
    ULONG Padding;
    UNICODE_STRING FullDllName;
    UNICODE_STRING BaseDllName;
} DUMPER_LDR_DATA_TABLE_ENTRY_X64;

C_ASSERT(sizeof(PVOID) == 8);
C_ASSERT(FIELD_OFFSET(DUMPER_PEB_X64, ImageBaseAddress) == 0x10);
C_ASSERT(FIELD_OFFSET(DUMPER_PEB_X64, Ldr) == 0x18);
C_ASSERT(FIELD_OFFSET(DUMPER_PEB_LDR_DATA_X64, InLoadOrderModuleList) == 0x10);
C_ASSERT(FIELD_OFFSET(DUMPER_PEB_LDR_DATA_X64, InMemoryOrderModuleList) == 0x20);
C_ASSERT(FIELD_OFFSET(DUMPER_LDR_DATA_TABLE_ENTRY_X64, DllBase) == 0x30);
C_ASSERT(FIELD_OFFSET(DUMPER_LDR_DATA_TABLE_ENTRY_X64, FullDllName) == 0x48);
C_ASSERT(FIELD_OFFSET(DUMPER_LDR_DATA_TABLE_ENTRY_X64, BaseDllName) == 0x58);

NTKERNELAPI PPEB NTAPI PsGetProcessPeb(PEPROCESS Process);
