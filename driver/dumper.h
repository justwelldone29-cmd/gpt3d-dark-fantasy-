#pragma once
#include <ntifs.h>

#define IOCTL_DUMP_MEMORY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define DUMPER_MAX_SIZE (1024u * 1024u)

typedef struct _DUMP_REQUEST {
    ULONG pid;
    ULONG64 address;
    ULONG size;
} DUMP_REQUEST;
C_ASSERT(sizeof(DUMP_REQUEST) == 24);

/* Undocumented export; deliberately isolated for this experiment. */
NTKERNELAPI NTSTATUS NTAPI MmCopyVirtualMemory(
    PEPROCESS SourceProcess, PVOID SourceAddress,
    PEPROCESS TargetProcess, PVOID TargetAddress,
    SIZE_T BufferSize, KPROCESSOR_MODE PreviousMode, PSIZE_T ReturnSize);

DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD DumperUnload;
DRIVER_DISPATCH DumperCreateClose;
DRIVER_DISPATCH DumperDeviceControl;
