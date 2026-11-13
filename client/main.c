#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#define IOCTL_DUMP_MEMORY CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS)
#define DUMPER_MAX_SIZE (1024u * 1024u)
typedef struct { ULONG pid; ULONG64 address; ULONG size; } DUMP_REQUEST;
typedef char request_size_must_be_24[(sizeof(DUMP_REQUEST) == 24) ? 1 : -1];

static int Parse(const char *s, unsigned long long *value)
{
    char *end;
    if (!s[0] || s[0] == '-') return 0;
    errno = 0;
    *value = strtoull(s, &end, 0);
    return errno == 0 && *end == '\0';
}

int main(int argc, char **argv)
{
    DUMP_REQUEST request = {0};
    unsigned long long pid, address, size;
    HANDLE process, device;
    BYTE *output;
    DWORD returned = 0;
    FILE *file;
    int result = 1;
    if (argc != 5 || !Parse(argv[1], &pid) || !Parse(argv[2], &address) ||
        !Parse(argv[3], &size) || pid == 0 || pid > MAXDWORD ||
        size == 0 || size > DUMPER_MAX_SIZE) {
        fprintf(stderr, "Usage: dumper.exe PID ADDRESS SIZE output.bin\nNumbers accept decimal or 0x hexadecimal.\n");
        return 2;
    }
    request.pid = (ULONG)pid;
    request.address = address;
    request.size = (ULONG)size;
    process = OpenProcess(PROCESS_VM_READ, FALSE, request.pid);
    if (!process) {
        fprintf(stderr, "OpenProcess: %lu\n", GetLastError());
        return 1;
    }
    device = CreateFileW(L"\\\\.\\Dumper", GENERIC_READ, 0, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (device == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateFile: %lu\n", GetLastError());
        CloseHandle(process);
        return 1;
    }
    output = (BYTE *)malloc(request.size);
    if (!output) goto cleanup;
    if (!DeviceIoControl(device, IOCTL_DUMP_MEMORY, &request, sizeof(request),
        output, request.size, &returned, NULL)) {
        fprintf(stderr, "DeviceIoControl: %lu\n", GetLastError());
        goto cleanup;
    }
    if (returned != request.size) {
        fprintf(stderr, "Unexpected byte count: %lu\n", returned);
        goto cleanup;
    }
    if (fopen_s(&file, argv[4], "wb") != 0) {
        fprintf(stderr, "Cannot open output file\n");
        goto cleanup;
    }
    {
        size_t written = fwrite(output, 1, returned, file);
        int closed = fclose(file);
        if (written != returned || closed != 0) {
            fprintf(stderr, "Output write failed\n");
            goto cleanup;
        }
    }
    printf("Dumped %lu bytes\n", returned);
    result = 0;
cleanup:
    free(output);
    CloseHandle(device);
    CloseHandle(process);
    return result;
}
