#include "dumper.h"
#include "peb_layout_x64.h"

static UNICODE_STRING DeviceName = RTL_CONSTANT_STRING(L"\\Device\\Dumper");
static UNICODE_STRING LinkName = RTL_CONSTANT_STRING(L"\\DosDevices\\Dumper");

static NTSTATUS Complete(PIRP Irp, NTSTATUS Status, ULONG_PTR Bytes)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Bytes;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS DumperCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    return Complete(Irp, STATUS_SUCCESS, 0);
}

/* Destination is kernel memory; Staging is a caller-process user VA.
   Never dereference remote PEB pointers directly in the kernel. */
static NTSTATUS ReadRemote(PEPROCESS Process, ULONG_PTR Address,
    PVOID Destination, SIZE_T Length, PVOID Staging)
{
    SIZE_T copied = 0;
    NTSTATUS status;
    if (Length == 0 || Length > PAGE_SIZE || Address == 0 ||
        Address > (ULONG_PTR)MmHighestUserAddress ||
        Length - 1 > (ULONG_PTR)MmHighestUserAddress - Address)
        return STATUS_INVALID_PARAMETER;
    status = MmCopyVirtualMemory(Process, (PVOID)Address,
        PsGetCurrentProcess(), Staging, Length, UserMode, &copied);
    if (!NT_SUCCESS(status)) return status;
    if (copied != Length) return STATUS_PARTIAL_COPY;
    __try {
        ProbeForRead(Staging, Length, 1);
        RtlCopyMemory(Destination, Staging, Length);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        status = GetExceptionCode();
    }
    return status;
}

static NTSTATUS GetModuleBase(const MODULE_BASE_REQUEST *Request, PULONG64 Base)
{
    HANDLE accessHandle = NULL;
    PEPROCESS process = NULL;
    OBJECT_ATTRIBUTES attributes;
    CLIENT_ID clientId;
    DUMPER_PEB_X64 peb;
    DUMPER_PEB_LDR_DATA_X64 ldr;
    DUMPER_LDR_DATA_TABLE_ENTRY_X64 entry;
    DUMPER_LDR_DATA_TABLE_ENTRY_X64 *remoteEntry;
    UNICODE_STRING wanted, actual;
    WCHAR name[64];
    ULONG nameLength, count;
    ULONG_PTR pebAddress, head, node;
    PVOID staging = NULL;
    SIZE_T regionSize = PAGE_SIZE, freeSize = 0;
    NTSTATUS status;

    *Base = 0;
    for (nameLength = 0; nameLength < RTL_NUMBER_OF(Request->module_name); ++nameLength)
        if (Request->module_name[nameLength] == L'\0') break;
    if (Request->pid == 0 || nameLength == 0 ||
        nameLength == RTL_NUMBER_OF(Request->module_name))
        return STATUS_INVALID_PARAMETER;
    wanted.Buffer = (PWCH)Request->module_name;
    wanted.Length = (USHORT)(nameLength * sizeof(WCHAR));
    wanted.MaximumLength = wanted.Length;

    clientId.UniqueProcess = ULongToHandle(Request->pid);
    clientId.UniqueThread = NULL;
    InitializeObjectAttributes(&attributes, NULL,
        OBJ_KERNEL_HANDLE | OBJ_FORCE_ACCESS_CHECK, NULL, NULL);
    status = ZwOpenProcess(&accessHandle, PROCESS_VM_READ, &attributes, &clientId);
    if (!NT_SUCCESS(status)) return status;
    status = PsLookupProcessByProcessId(clientId.UniqueProcess, &process);
    if (!NT_SUCCESS(status)) goto cleanup;
    if (PsGetProcessWow64Process(process) != NULL) {
        status = STATUS_NOT_SUPPORTED;
        goto cleanup;
    }
    pebAddress = (ULONG_PTR)PsGetProcessPeb(process);
    if (pebAddress == 0) {
        status = STATUS_NOT_FOUND;
        goto cleanup;
    }
    status = ZwAllocateVirtualMemory(NtCurrentProcess(), &staging, 0,
        &regionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(status)) goto cleanup;
    status = ReadRemote(process, pebAddress, &peb, sizeof(peb), staging);
    if (!NT_SUCCESS(status)) goto cleanup;
    status = ReadRemote(process, (ULONG_PTR)peb.Ldr, &ldr, sizeof(ldr), staging);
    if (!NT_SUCCESS(status)) goto cleanup;
    if (!ldr.Initialized) {
        status = STATUS_RETRY;
        goto cleanup;
    }
    head = (ULONG_PTR)peb.Ldr +
        FIELD_OFFSET(DUMPER_PEB_LDR_DATA_X64, InLoadOrderModuleList);
    node = (ULONG_PTR)ldr.InLoadOrderModuleList.Flink;
    /* Bounded walk: the loader list is mutable and may contain bad links. */
    for (count = 0; count < 1024; ++count) {
        if (node == head) {
            status = STATUS_NOT_FOUND;
            goto cleanup;
        }
        /* Calculate a remote address only; do not dereference remoteEntry. */
        remoteEntry = CONTAINING_RECORD((PLIST_ENTRY)node,
            DUMPER_LDR_DATA_TABLE_ENTRY_X64, InLoadOrderLinks);
        status = ReadRemote(process, (ULONG_PTR)remoteEntry,
            &entry, sizeof(entry), staging);
        if (!NT_SUCCESS(status)) goto cleanup;
        if ((entry.BaseDllName.Length % sizeof(WCHAR)) != 0 ||
            entry.BaseDllName.Length > entry.BaseDllName.MaximumLength) {
            status = STATUS_DATA_ERROR;
            goto cleanup;
        }
        if (entry.BaseDllName.Length == wanted.Length) {
            status = ReadRemote(process, (ULONG_PTR)entry.BaseDllName.Buffer,
                name, entry.BaseDllName.Length, staging);
            if (!NT_SUCCESS(status)) goto cleanup;
            actual.Buffer = name;
            actual.Length = entry.BaseDllName.Length;
            actual.MaximumLength = sizeof(name);
            if (RtlEqualUnicodeString(&wanted, &actual, TRUE)) {
                if (entry.DllBase == NULL ||
                    (ULONG_PTR)entry.DllBase > (ULONG_PTR)MmHighestUserAddress) {
                    status = STATUS_DATA_ERROR;
                    goto cleanup;
                }
                *Base = (ULONG64)(ULONG_PTR)entry.DllBase;
                status = STATUS_SUCCESS;
                goto cleanup;
            }
        }
        node = (ULONG_PTR)entry.InLoadOrderLinks.Flink;
    }
    status = STATUS_RETRY;
cleanup:
    if (staging != NULL)
        (VOID)ZwFreeVirtualMemory(NtCurrentProcess(), &staging, &freeSize, MEM_RELEASE);
    if (process != NULL) ObDereferenceObject(process);
    if (accessHandle != NULL) ZwClose(accessHandle);
    return status;
}

NTSTATUS DumperDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    DUMP_REQUEST request;
    PEPROCESS process = NULL;
    HANDLE accessHandle = NULL;
    OBJECT_ATTRIBUTES attributes;
    CLIENT_ID clientId;
    PVOID staging = NULL;
    SIZE_T regionSize, copied = 0, freeSize = 0;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(DeviceObject);

    if (stack->Parameters.DeviceIoControl.IoControlCode != IOCTL_DUMP_MEMORY &&
        stack->Parameters.DeviceIoControl.IoControlCode != IOCTL_GET_MODULE_BASE)
        return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || Irp->RequestorMode != UserMode ||
        IoGetRequestorProcess(Irp) != PsGetCurrentProcess())
        return Complete(Irp, STATUS_INVALID_DEVICE_STATE, 0);
    if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_GET_MODULE_BASE) {
        MODULE_BASE_REQUEST moduleRequest;
        ULONG64 base = 0;
        if (stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(moduleRequest) ||
            Irp->AssociatedIrp.SystemBuffer == NULL)
            return Complete(Irp, STATUS_INFO_LENGTH_MISMATCH, 0);
        if (stack->Parameters.DeviceIoControl.OutputBufferLength < sizeof(base))
            return Complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        RtlCopyMemory(&moduleRequest, Irp->AssociatedIrp.SystemBuffer, sizeof(moduleRequest));
        status = GetModuleBase(&moduleRequest, &base);
        if (NT_SUCCESS(status))
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &base, sizeof(base));
        return Complete(Irp, status, NT_SUCCESS(status) ? sizeof(base) : 0);
    }
    if (stack->Parameters.DeviceIoControl.InputBufferLength != sizeof(request) ||
        Irp->AssociatedIrp.SystemBuffer == NULL)
        return Complete(Irp, STATUS_INFO_LENGTH_MISMATCH, 0);

    RtlCopyMemory(&request, Irp->AssociatedIrp.SystemBuffer, sizeof(request));
    if (request.pid == 0 || request.size == 0 || request.size > DUMPER_MAX_SIZE ||
        request.address > (ULONG64)(ULONG_PTR)MmHighestUserAddress ||
        (ULONG64)request.size - 1 >
            (ULONG64)(ULONG_PTR)MmHighestUserAddress - request.address)
        return Complete(Irp, STATUS_INVALID_PARAMETER, 0);
    if (stack->Parameters.DeviceIoControl.OutputBufferLength < request.size)
        return Complete(Irp, STATUS_BUFFER_TOO_SMALL, 0);

    /* Check the caller's access, including when a client skips OpenProcess. */
    clientId.UniqueProcess = ULongToHandle(request.pid);
    clientId.UniqueThread = NULL;
    InitializeObjectAttributes(&attributes, NULL,
        OBJ_KERNEL_HANDLE | OBJ_FORCE_ACCESS_CHECK, NULL, NULL);
    status = ZwOpenProcess(&accessHandle, PROCESS_VM_READ, &attributes, &clientId);
    if (!NT_SUCCESS(status))
        return Complete(Irp, status, 0);
    status = PsLookupProcessByProcessId(clientId.UniqueProcess, &process);
    if (!NT_SUCCESS(status))
        goto cleanup;

    /* UserMode probes both endpoints: SystemBuffer is a kernel address.
       Allocate a user VA in the synchronous requestor's process instead. */
    regionSize = request.size;
    status = ZwAllocateVirtualMemory(NtCurrentProcess(), &staging, 0,
        &regionSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!NT_SUCCESS(status))
        goto cleanup;
    status = MmCopyVirtualMemory(process, (PVOID)(ULONG_PTR)request.address,
        PsGetCurrentProcess(), staging, request.size, UserMode, &copied);
    if (NT_SUCCESS(status) && copied == request.size) {
        __try {
            ProbeForRead(staging, copied, 1);
            RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, staging, copied);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            status = GetExceptionCode();
            copied = 0;
        }
    } else {
        /* All-or-error contract: do not expose partial output. */
        if (NT_SUCCESS(status)) status = STATUS_PARTIAL_COPY;
        copied = 0;
    }

cleanup:
    if (staging != NULL)
        (VOID)ZwFreeVirtualMemory(NtCurrentProcess(), &staging, &freeSize, MEM_RELEASE);
    if (process != NULL)
        ObDereferenceObject(process);
    if (accessHandle != NULL)
        ZwClose(accessHandle);
    return Complete(Irp, status, NT_SUCCESS(status) ? copied : 0);
}

VOID DumperUnload(PDRIVER_OBJECT DriverObject)
{
    IoDeleteSymbolicLink(&LinkName);
    if (DriverObject->DeviceObject != NULL)
        IoDeleteDevice(DriverObject->DeviceObject);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    PDEVICE_OBJECT device = NULL;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(RegistryPath);
    status = IoCreateDevice(DriverObject, 0, &DeviceName, FILE_DEVICE_UNKNOWN,
        FILE_DEVICE_SECURE_OPEN, FALSE, &device);
    if (!NT_SUCCESS(status)) return status;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = DumperCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = DumperCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DumperDeviceControl;
    DriverObject->DriverUnload = DumperUnload;
    status = IoCreateSymbolicLink(&LinkName, &DeviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(device);
        return status;
    }
    device->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}
