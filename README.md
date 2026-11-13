# Dumper: эксперимент WDM / Windows 11 x64

`driver/dumper.c` создаёт `\\Device\\Dumper` и ссылку
`\\DosDevices\\Dumper`. Клиент открывает `\\.\Dumper`.
Запрос `IOCTL_DUMP_MEMORY` использует `METHOD_BUFFERED`, вход —
`{ ULONG pid; ULONG64 address; ULONG size; }`, размер структуры при
стандартном x64 выравнивании — **24 байта**. Не включать упаковку в 1 байт.
Выход — ровно `size` байт, максимум 1 MiB за запрос.

## Сборка

Требуются Windows, Visual Studio C++ Build Tools, совместимые Windows SDK
и WDK с одинаковой установленной версией каталогов Include/Lib.
В **x64 Native Tools Command Prompt** из корня репозитория:

```bat
build.cmd 10.0.22621.0
```

Аргумент заменяется на фактически установленную версию SDK/WDK.
Результат: `build/dumper.sys`, `build/dumper.exe`. Скрипт не подписывает
драйвер. Перед загрузкой используйте подпись, принимаемую вашей тестовой
Windows. Из административной консоли после подписания:

```bat
sc.exe create Dumper type= kernel start= demand binPath= C:\absolute\path\dumper.sys
sc.exe start Dumper
build\dumper.exe 1234 0x0000012340000000 4096 output.bin
sc.exe stop Dumper
sc.exe delete Dumper
```

PID и адрес берутся из вашего запущенного приложения. Чтобы проверить
результат, разместите в приложении известный массив байт и сравните дамп
с исходным массивом. Дополнительно проверьте недоступный адрес, размер 0,
размер больше 1 MiB и PID процесса, для которого у вызывающего нет права
`PROCESS_VM_READ`: запросы должны завершиться ошибкой без выходных байт.

## Почему нужен промежуточный буфер

`MmCopyVirtualMemory(..., UserMode, ...)` проверяет пользовательские адреса
обоих концов. `Irp->AssociatedIrp.SystemBuffer` находится в адресном
пространстве ядра, поэтому напрямую передавать его как TargetAddress
с режимом UserMode нельзя. Драйвер выделяет пользовательский буфер через
`ZwAllocateVirtualMemory`, копирует в него, затем под SEH переносит данные
в SystemBuffer и освобождает выделение. Обработка синхронная, только на
PASSIVE_LEVEL и в контексте процесса-заявителя. Все выходы после успешного
`PsLookupProcessByProcessId` проходят через `ObDereferenceObject`.
Частичный результат не возвращается: запрос имеет контракт «всё или ошибка».

Клиент выполняет `OpenProcess(PROCESS_VM_READ)`, затем `DeviceIoControl`.
Драйвер отдельно открывает процесс с `OBJ_FORCE_ACCESS_CHECK`, чтобы не
полагаться на добровольную проверку в клиенте. Запросы к адресам ядра
отклоняются. Чтение не является атомарным снимком: при изменениях памяти
целевого приложения байты могут относиться к разным моментам времени.

## PEB и модули, native x64

В `driver/peb_layout_x64.h` даны отдельные частичные представления ABI,
не переопределяющие системные теги `_PEB` и другие типы WDK.
Для семейства Windows 11 23H2 **22631.x**, native x64, используемые поля:

| Структура | Поле | Смещение |
| --- | --- | --- |
| PEB | ImageBaseAddress | 0x10 |
| PEB | Ldr | 0x18 |
| PEB_LDR_DATA | InLoadOrderModuleList | 0x10 |
| PEB_LDR_DATA | InMemoryOrderModuleList | 0x20 |
| LDR_DATA_TABLE_ENTRY | InLoadOrderLinks | 0x00 |
| LDR_DATA_TABLE_ENTRY | InMemoryOrderLinks | 0x10 |
| LDR_DATA_TABLE_ENTRY | DllBase | 0x30 |
| LDR_DATA_TABLE_ENTRY | FullDllName | 0x48 |
| LDR_DATA_TABLE_ENTRY | BaseDllName | 0x58 |

`PsGetProcessPeb(process)` возвращает адрес пользовательского PEB, а не
готовую безопасную для разыменования структуру ядра. Основная база EXE
находится по PEB+0x10. Для DLL прочитайте Ldr, затем список; для
InMemoryOrderLinks вычитайте 0x10 из адреса узла, получая адрес начала
LDR_DATA_TABLE_ENTRY, и читайте DllBase по +0x30. Все удалённые поля и
строки нужно читать копированием с проверкой результата, ограничивая
число узлов и длины строк. Список может изменяться одновременно с чтением.
Эти определения не подходят для WOW64 и ARM64.

Это недокументированный ABI. 23H2 — семейство обновляемых сборок, а не
один точный бинарный файл. Таблица не проверена в этой Linux-среде по PDB
вашего конкретного 22631.x. Перед использованием сверьте символы целевой
системы в WinDbg:

```text
.symfix
.reload /f ntdll.dll
dt ntdll!_PEB
dt ntdll!_PEB_LDR_DATA
dt ntdll!_LDR_DATA_TABLE_ENTRY
```

`MmCopyVirtualMemory` и `PsGetProcessPeb` — недокументированные экспорты;
объявления для эксперимента вынесены в заголовки. Проверка экспортов,
сборка WDK, подпись и фактическая загрузка выполняются на целевой Windows.
В текущей Linux-среде Windows SDK/WDK отсутствуют, поэтому успешная
компиляция и работа на Windows здесь не заявляются.
