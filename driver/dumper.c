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

    if (stack->Parameters.DeviceIoControl.IoControlCode != IOCTL_DUMP_MEMORY)
        return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || Irp->RequestorMode != UserMode ||
        IoGetRequestorProcess(Irp) != PsGetCurrentProcess())
        return Complete(Irp, STATUS_INVALID_DEVICE_STATE, 0);
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
