#pragma once

#include <ntifs.h>
#include <ntddk.h>
#include <windef.h>
#include <ntdddisk.h>
#include <ntddscsi.h>
#include <ata.h>
#include <scsi.h>
#include <ntddndis.h>
#include <mountmgr.h>
#include <mountdev.h>
#include <classpnp.h>
#include <ntimage.h>

#include "util.h"

// --- Global Configuration ---
static DWORD SEED = 0;
static CHAR SERIAL[] = "123456789";

#define LENGTH(a) (sizeof(a) / sizeof(a[0]))
#define RELATIVE_ADDR(addr, size) ((PVOID)((PBYTE)addr + *(PINT)((PBYTE)addr + (size - (INT)sizeof(INT))) + size))

// --- [NEW] Filter Driver Extension Structure ---
// Used dynamically by your disk and monitor filters (replaces DiskControlOriginal / PartControlOriginal)
typedef struct _DISK_FILTER_EXTENSION {
	PDEVICE_OBJECT TargetDeviceObject;
	PDEVICE_OBJECT PhysicalDeviceObject;
} DISK_FILTER_EXTENSION, * PDISK_FILTER_EXTENSION;

// --- [NEW] SMBIOS Parsing Structures ---
typedef struct _SMBIOS_HEADER {
	BYTE Type;
	BYTE Length;
	USHORT Handle;
} SMBIOS_HEADER, * PSMBIOS_HEADER;

typedef struct _SMBIOS_STRUCTURE_TABLE {
	SMBIOS_HEADER Header;
} SMBIOS_STRUCTURE_TABLE, * PSMBIOS_STRUCTURE_TABLE;

// --- [NEW] NSI Network / ARP Structures ---
typedef struct _NSI_PARAMS_FIXED {
	PVOID Unknown1;          // 8 bytes (on x64)
	PVOID Unknown2;          // 8 bytes
	PVOID Unknown3;          // 8 bytes
	DWORD Type;              // 4 bytes <-- Total before this is exactly 24 bytes (0x18)!
	DWORD OutputBufferLength;// 4 bytes
	PVOID Unknown4;          // 8 bytes
	PVOID UserBuffer;        // 8 bytes <-- This is what we need to read/modify the MACs
} NSI_PARAMS_FIXED, * PNSI_PARAMS_FIXED;

typedef struct _NSI_NEIGHBOR_ENTRY {
	PVOID NetworkKey;
	ULONG InterfaceIndex;
	ULONG IPAddressLength;
	BYTE IPAddress[16];
	BYTE MacAddress[6];
	ULONG State;
} NSI_NEIGHBOR_ENTRY, * PNSI_NEIGHBOR_ENTRY;

typedef struct _NSI_INTERFACE_ENTRY {
	char _padding[0x10];
	BYTE PermanentPhysicalAddress[6];
	BYTE CurrentPhysicalAddress[6]; // The operational MAC address
} NSI_INTERFACE_ENTRY, * PNSI_INTERFACE_ENTRY;

// NdisOffsets
typedef struct _NDIS_VERSION_OFFSETS {
	ULONG FilterToMiniport;
	ULONG MiniportToIfBlock;
} NDIS_VERSION_OFFSETS, * PNDIS_VERSION_OFFSETS;

extern NDIS_VERSION_OFFSETS g_NdisOffsets;

//TCPIP OFFSETS
typedef struct _TCPIP_VERSION_OFFSETS {
	ULONG BuildNumber;
	ULONG InterfaceList;       // Replaces 0x4DC8
	ULONG InterfaceListEntry;  // Replaces 0xA0
	ULONG NeighborTable;       // Replaces 0x158
	ULONG NeighborList;        // Replaces 0x38
	ULONG NeighborListEntry;   // Replaces 0x40
	ULONG IpAddress;           // Replaces 0xA8
	ULONG MacV4;               // Replaces 0xAC
	ULONG MacV6;               // Replaces 0xB8
} TCPIP_VERSION_OFFSETS, * PTCI_VERSION_OFFSETS;

// Global variable to store current system parameters
extern TCPIP_VERSION_OFFSETS g_TcpipOffsets;

// --- Remaining Legacy DKOM Hooks Tracking ---
// Kept strictly for modules we haven't converted to filters yet (e.g., MountMgr, Nsi, GPU)
typedef struct _SWAP {
	UNICODE_STRING Name;
	PVOID* Swap;
	PVOID Original;
} SWAP, * PSWAP;

static struct {
	SWAP Buffer[0xFF];
	ULONG Length;
} SWAPS = { 0 };

typedef struct _NIC_DRIVER {
	PDRIVER_OBJECT DriverObject;
	PDRIVER_DISPATCH Original;
} NIC_DRIVER, * PNIC_DRIVER;

typedef struct _RTL_PROCESS_MODULE_INFORMATION {
	HANDLE Section;
	PVOID MappedBase;
	PVOID ImageBase;
	ULONG ImageSize;
	ULONG Flags;
	USHORT LoadOrderIndex;
	USHORT InitOrderIndex;
	USHORT LoadCount;
	USHORT OffsetToFileName;
	UCHAR  FullPathName[256];
} RTL_PROCESS_MODULE_INFORMATION, * PRTL_PROCESS_MODULE_INFORMATION;

typedef struct _RTL_PROCESS_MODULES {
	ULONG NumberOfModules;
	RTL_PROCESS_MODULE_INFORMATION Modules[1];
} RTL_PROCESS_MODULES, * PRTL_PROCESS_MODULES;

//Wpp btbd
typedef struct _WPP_REGISTRATION {
	PVOID* PfnWppTraceMessageAddress; // Address of the pointer inside mountmgr.sys .data section
	PVOID  OriginalTraceFunction;     // Original trace handler to restore or chain
	PVOID  TargetReturnAddress;        // Specific instruction return address inside MountMgr's DeviceControl
} WPP_REGISTRATION, * PWPP_REGISTRATION;

static WPP_REGISTRATION g_WppHook = { 0 };

//NewVolumes Struct
typedef NTSTATUS(*ObReferenceObjectByName_t)(
	PUNICODE_STRING ObjectName,
	ULONG Attributes,
	PACCESS_STATE PassedAccessState,
	ACCESS_MASK DesiredAccess,
	POBJECT_TYPE ObjectType,
	KPROCESSOR_MODE AccessMode,
	PVOID ParseContext,
	PVOID* Object
	);

//Monitor Spoofing
#define WNODE_FLAG_FIXED_INSTANCE_SIZE 0x00000010
#define MAX_MONITOR_HOOKS 16

// Dummy macros/placeholders - replace with your project's actual logging/encryption setup
#define encrypt(str) str
#define logging_output(format, ...)

// Forward declarations for kernel-level functions missing from standard NT headers
//NTSTATUS ObReferenceObjectByName(PUNICODE_STRING, ULONG, PACCESS_STATE, ACCESS_MASK, POBJECT_TYPE, KPROCESSOR_MODE, PVOID, PVOID*);
extern POBJECT_TYPE* IoDriverObjectType;
#pragma warning(push)
#pragma warning(disable: 4201)
typedef struct _WNODE_HEADER {
	ULONG         BufferSize;
	ULONG         ProviderId;
	union {
		ULONG64   HistoricalContext;
		struct { ULONG Version; ULONG Linkage; };
	};
	union {
		ULONG         CountLost;
		HANDLE        KernelHandle;
		LARGE_INTEGER TimeStamp;
	};
	GUID          Guid;
	ULONG         ClientContext;
	ULONG         Flags;
} WNODE_HEADER, * PWNODE_HEADER;

typedef struct {
	ULONG OffsetInstanceData;
	ULONG LengthInstanceData;
} OFFSETINSTANCEDATAANDLENGTH, * POFFSETINSTANCEDATAANDLENGTH;

typedef struct _WNODE_ALL_DATA {
	WNODE_HEADER WnodeHeader;
	ULONG        DataBlockOffset;
	ULONG        InstanceCount;
	ULONG        OffsetInstanceNameOffsets;
	union {
		ULONG                        FixedInstanceSize;
		OFFSETINSTANCEDATAANDLENGTH  OffsetInstanceDataAndLength[1];
	};
} WNODE_ALL_DATA, * PWNODE_ALL_DATA;
#pragma warning(pop)
// Matched exactly to your pasted struct definition
typedef struct _WmiMonitorID {
	USHORT ProductCodeID[16];
	USHORT SerialNumberID[16];
	USHORT ManufacturerName[16];
	UCHAR  WeekOfManufacture;
	USHORT YearOfManufacture;
	USHORT UserFriendlyNameLength;
	USHORT UserFriendlyName[1];
} WmiMonitorID, * PWmiMonitorID;

static struct {
	PDRIVER_OBJECT   drv;
	PDRIVER_DISPATCH original;
} g_MonitorHooks[MAX_MONITOR_HOOKS] = { { NULL, NULL } };
static ULONG g_MonitorHookCount = 0;

static char g_SpoofedSerial[14] = { 0 };
static char g_SpoofedVendor[4] = { 0 };
static char g_SpoofedName[14] = { 0 };
static BOOLEAN g_MonitorStringsInit = FALSE;

//Wpp
#define WPP_FLAG_ALL (0xFFFFFFFFFFFFFFFF)

typedef VOID(*WPP_FILTER)(PCONTEXT, PVOID, PVOID, PVOID);

typedef struct _WPP {
	PVOID ReturnAddress;
	WPP_FILTER Filter;

	PDEVICE_OBJECT* WppGlobal;
	PDEVICE_OBJECT WppGlobalOriginal;

	PVOID* WppTraceMessage;
	PVOID WppTraceMessageOriginal;
} WPP, * PWPP;

VOID WppSet(PVOID returnAddress, WPP_FILTER filter, PDEVICE_OBJECT* wppGlobal, PVOID* wppTraceMessage);
VOID WppUndo();
ULONG WppTraceMessage(VOID* LoggerHandle, ULONG MessageFlags, LPCGUID MessageGuid, USHORT MessageNumber, ...);

// --- DKOM Hooking Macros ---
#define AppendSwap(name, swap, hook, original) { \
	UNICODE_STRING _n = name; \
	PSWAP _s = &SWAPS.Buffer[SWAPS.Length++]; \
	*(PVOID *)&original = _s->Original = InterlockedExchangePointer((PVOID *)(_s->Swap = (PVOID *)swap), (PVOID)hook); \
	_s->Name = _n; \
	printf("swapped %wZ\n", &_n); \
}

#define SwapControl(driver, hook, original) { \
	UNICODE_STRING str = driver; \
	PDRIVER_OBJECT object = 0; \
	NTSTATUS _status = ObReferenceObjectByName(&str, OBJ_CASE_INSENSITIVE, 0, 0, *IoDriverObjectType, KernelMode, 0, &object); \
	if (NT_SUCCESS(_status)) { \
		AppendSwap(str, &object->MajorFunction[IRP_MJ_DEVICE_CONTROL], hook, original); \
		ObDereferenceObject(object); \
	} else { \
		printf("! failed to get %wZ: %p !\n", &str, _status); \
	} \
}
