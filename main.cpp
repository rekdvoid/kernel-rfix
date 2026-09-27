#include "structs.h"       
#include <ntstrsafe.h>  // <-- Add this line here
#include <mountmgr.h>
#include <mountdev.h>

#define REG_MAX_KEY_LENGTH 256
#define MAX_NIC_DRIVERS 64

NDIS_VERSION_OFFSETS g_NdisOffsets = { 0 };
TCPIP_VERSION_OFFSETS g_TcpipOffsets = { 0 };
PDRIVER_DISPATCH DiskControlOriginal = 0, MountControlOriginal = 0, PartControlOriginal = 0, NsiControlOriginal = 0, GpuControlOriginal = 0;
PDRIVER_DISPATCH OriginalQueryVolumeInformation = NULL; //NTFS
PDRIVER_DISPATCH g_OriginalMonitorDispatch = NULL;
ULONG GlobalVolumeSerialNumber = 0;

struct {
	DWORD Length;
	NIC_DRIVER Drivers[0xFF];
} NICs = { 0 };

static PDEVICE_OBJECT g_CachedDevice[16] = { NULL };
static char           g_CachedSerial[16][12] = { { 0 } };
static BOOLEAN        g_CachedInit[16] = { FALSE };

/**** DISKS ****/
NTSTATUS PartInfoIoc(PDEVICE_OBJECT device, PIRP irp, PVOID context) {
	if (context) {
		IOC_REQUEST request = *(PIOC_REQUEST)context;
		ExFreePool(context);

		if (request.BufferLength >= sizeof(PARTITION_INFORMATION_EX)) {
			PPARTITION_INFORMATION_EX info = (PPARTITION_INFORMATION_EX)request.Buffer;
			if (PARTITION_STYLE_GPT == info->PartitionStyle) {
				memset(&info->Gpt.PartitionId, 0, sizeof(GUID));
			}
		}

		if (request.OldRoutine && irp->StackCount > 1) {
			return request.OldRoutine(device, irp, request.OldContext);
		}
	}

	return STATUS_SUCCESS;
}

NTSTATUS PartLayoutIoc(PDEVICE_OBJECT device, PIRP irp, PVOID context) {
	if (context) {
		IOC_REQUEST request = *(PIOC_REQUEST)context;
		ExFreePool(context);

		if (request.BufferLength >= sizeof(DRIVE_LAYOUT_INFORMATION_EX)) {
			PDRIVE_LAYOUT_INFORMATION_EX info = (PDRIVE_LAYOUT_INFORMATION_EX)request.Buffer;
			if (PARTITION_STYLE_GPT == info->PartitionStyle) {
				memset(&info->Gpt.DiskId, 0, sizeof(GUID));
			}
		}

		if (request.OldRoutine && irp->StackCount > 1) {
			return request.OldRoutine(device, irp, request.OldContext);
		}
	}

	return STATUS_SUCCESS;
}

NTSTATUS PartControl(PDEVICE_OBJECT device, PIRP irp) {
	PIO_STACK_LOCATION ioc = IoGetCurrentIrpStackLocation(irp);
	switch (ioc->Parameters.DeviceIoControl.IoControlCode) {
	case IOCTL_DISK_GET_PARTITION_INFO_EX:
		ChangeIoc(ioc, irp, PartInfoIoc);
		break;
	case IOCTL_DISK_GET_DRIVE_LAYOUT_EX:
		ChangeIoc(ioc, irp, PartLayoutIoc);
		break;
	}

	return PartControlOriginal(device, irp);
}

NTSTATUS StorageQueryIoc(PDEVICE_OBJECT device, PIRP irp, PVOID context) {
	if (context) {
		IOC_REQUEST request = *(PIOC_REQUEST)context;
		ExFreePool(context);

		if (request.BufferLength >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
			PSTORAGE_DEVICE_DESCRIPTOR desc = (PSTORAGE_DEVICE_DESCRIPTOR)request.Buffer;
			ULONG offset = desc->SerialNumberOffset;
			if (offset && offset < request.BufferLength) {
			    SIZE_T remaining = request.BufferLength - offset;
			    SIZE_T copyLen = min(strlen(SERIAL), remaining - 1);
			    RtlCopyMemory((PCHAR)desc + offset, SERIAL, copyLen);
			    *((PCHAR)desc + offset + copyLen) = '\0';
			}
		}

		if (request.OldRoutine && irp->StackCount > 1) {
			return request.OldRoutine(device, irp, request.OldContext);
		}
	}

	return STATUS_SUCCESS;
}

NTSTATUS AtaPassIoc(PDEVICE_OBJECT device, PIRP irp, PVOID context) {
	if (context) {
		IOC_REQUEST request = *(PIOC_REQUEST)context;
		ExFreePool(context);

		if (request.BufferLength >= sizeof(ATA_PASS_THROUGH_EX) + sizeof(PIDENTIFY_DEVICE_DATA)) {
			PATA_PASS_THROUGH_EX pte = (PATA_PASS_THROUGH_EX)request.Buffer;
			ULONG offset = (ULONG)pte->DataBufferOffset;
			if (offset && offset < request.BufferLength) {
				PCHAR serial = (PCHAR)((PIDENTIFY_DEVICE_DATA)((PBYTE)request.Buffer + offset))->SerialNumber;
				SwapEndianess(serial, SERIAL);

				DbgPrint("handled AtaPassIoc\n");
			}
		}

		if (request.OldRoutine && irp->StackCount > 1) {
			return request.OldRoutine(device, irp, request.OldContext);
		}
	}

	return STATUS_SUCCESS;
}

NTSTATUS SmartDataIoc(PDEVICE_OBJECT device, PIRP irp, PVOID context) {
	if (context) {
		IOC_REQUEST request = *(PIOC_REQUEST)context;
		ExFreePool(context);

		if (request.BufferLength >= sizeof(SENDCMDOUTPARAMS)) {
			PCHAR serial = ((PIDSECTOR)((PSENDCMDOUTPARAMS)request.Buffer)->bBuffer)->sSerialNumber;
			SwapEndianess(serial, SERIAL);

			DbgPrint("handled SmartDataIoc\n");
		}

		if (request.OldRoutine && irp->StackCount > 1) {
			return request.OldRoutine(device, irp, request.OldContext);
		}
	}

	return STATUS_SUCCESS;
}

NTSTATUS DiskControl(PDEVICE_OBJECT device, PIRP irp) {
	PIO_STACK_LOCATION ioc = IoGetCurrentIrpStackLocation(irp);
	switch (ioc->Parameters.DeviceIoControl.IoControlCode) {
	case IOCTL_STORAGE_QUERY_PROPERTY:
		if (StorageDeviceProperty == ((PSTORAGE_PROPERTY_QUERY)irp->AssociatedIrp.SystemBuffer)->PropertyId) {
			ChangeIoc(ioc, irp, StorageQueryIoc);
		}
		break;
	case IOCTL_ATA_PASS_THROUGH:
		ChangeIoc(ioc, irp, AtaPassIoc);
		break;
	case SMART_RCV_DRIVE_DATA:
		ChangeIoc(ioc, irp, SmartDataIoc);
		break;
	}

	return DiskControlOriginal(device, irp);
}

void SpoofRaidUnits(RU_REGISTER_INTERFACES RaidUnitRegisterInterfaces, BYTE RaidUnitExtension_SerialNumber_offset) {
	UNICODE_STRING storahci_str = RTL_CONSTANT_STRING(L"\\Driver\\storahci");
	PDRIVER_OBJECT storahci_object = 0;

	// Enumerate RaidPorts in storahci
	NTSTATUS status = ObReferenceObjectByName(&storahci_str, OBJ_CASE_INSENSITIVE, 0, 0, *IoDriverObjectType, KernelMode, 0, &storahci_object);
	if (NT_SUCCESS(status)) {
		ULONG length = 0;
		if (STATUS_BUFFER_TOO_SMALL == (status = IoEnumerateDeviceObjectList(storahci_object, 0, 0, &length)) && length) {
			ULONG size = length * sizeof(PDEVICE_OBJECT);
			PDEVICE_OBJECT* devices = ExAllocatePool(NonPagedPool, size);
			if (devices) {
				if (NT_SUCCESS(status = IoEnumerateDeviceObjectList(storahci_object, devices, size, &length)) && length) {
					for (ULONG i = 0; i < length; ++i) {
						PDEVICE_OBJECT raidport_object = devices[i];

						BYTE buffer[MAX_PATH] = { 0 };
						if (NT_SUCCESS(ObQueryNameString(raidport_object, (POBJECT_NAME_INFORMATION)buffer, sizeof(buffer), &size))) {
							PUNICODE_STRING raidport_str = (PUNICODE_STRING)buffer;

							// Enumerate devices for each RaidPort
							if (wcsstr(raidport_str->Buffer, L"\\RaidPort")) {
								DWORD total = 0, success = 0;
								for (PDEVICE_OBJECT device = raidport_object->DriverObject->DeviceObject; device; device = device->NextDevice) {
									if (FILE_DEVICE_DISK == device->DeviceType) {
										PSTRING serial = (PSTRING)((PBYTE)device->DeviceExtension + RaidUnitExtension_SerialNumber_offset);
										if (serial->Buffer && serial->MaximumLength > (USHORT)strlen(SERIAL)) {
											RtlStringCchCopyA(serial->Buffer, serial->MaximumLength, SERIAL);
											serial->Length = (USHORT)strlen(SERIAL);
										}
										
										if (NT_SUCCESS(status = RaidUnitRegisterInterfaces(device->DeviceExtension))) {
											++success;
										}
										else {
											DbgPrint("! RaidUnitRegisterInterfaces failed: %p !\n", status);
										}

										++total;
									}
								}

								DbgPrint("%wZ: RaidUnitRegisterInterfaces succeeded for %d/%d\n", raidport_str, success, total);
							}
						}

						ObDereferenceObject(raidport_object);
					}
				}
				else {
					DbgPrint("! failed to get storahci devices (got %d): %p !\n", length, status);
				}

				ExFreePool(devices);
			}
			else {
				DbgPrint("! failed to allocated %d storahci devices !\n", length);
			}
		}
		else {
			DbgPrint("! failed to get storahci device list size (got %d): %p !\n", length, status);
		}

		ObDereferenceObject(storahci_object);
	}
	else {
		DbgPrint("! failed to get %wZ: %p !\n", &storahci_object, status);
	}
}

void SpoofDisks() {
	SwapControl(RTL_CONSTANT_STRING(L"\\Driver\\partmgr"), PartControl, PartControlOriginal);

	UNICODE_STRING disk_str = RTL_CONSTANT_STRING(L"\\Driver\\Disk");
	PDRIVER_OBJECT disk_object = 0;

	NTSTATUS status = ObReferenceObjectByName(&disk_str, OBJ_CASE_INSENSITIVE, 0, 0, *IoDriverObjectType, KernelMode, 0, &disk_object);
	if (!NT_SUCCESS(status)) {
		DbgPrint("! failed to get %wZ: %p !\n", &disk_str, status);
		return;
	}

	AppendSwap(disk_str, &disk_object->MajorFunction[IRP_MJ_DEVICE_CONTROL], DiskControl, DiskControlOriginal);

	DISK_FAIL_PREDICTION DiskEnableDisableFailurePrediction = (DISK_FAIL_PREDICTION)FindPatternImage(disk_object->DriverStart, "\x48\x89\x00\x24\x10\x48\x89\x74\x24\x18\x57\x48\x81\xEC\x90\x00", "xx?xxxxxxxxxxxxx");
	if (DiskEnableDisableFailurePrediction) {
		ULONG length = 0;
		if (STATUS_BUFFER_TOO_SMALL == (status = IoEnumerateDeviceObjectList(disk_object, 0, 0, &length)) && length) {
			ULONG size = length * sizeof(PDEVICE_OBJECT);
			PDEVICE_OBJECT* devices = ExAllocatePool(NonPagedPool, size);
			if (devices) {
				if (NT_SUCCESS(status = IoEnumerateDeviceObjectList(disk_object, devices, size, &length)) && length) {
					ULONG success = 0, total = 0;

					for (ULONG i = 0; i < length; ++i) {
						PDEVICE_OBJECT device = devices[i];

						// Update disk properties for disk ID
						PDEVICE_OBJECT disk = IoGetAttachedDeviceReference(device);
						if (disk) {
							KEVENT event = { 0 };
							KeInitializeEvent(&event, NotificationEvent, FALSE);

							PIRP irp = IoBuildDeviceIoControlRequest(IOCTL_DISK_UPDATE_PROPERTIES, disk, 0, 0, 0, 0, 0, &event, 0);
							if (irp) {
								if (STATUS_PENDING == IoCallDriver(disk, irp)) {
									KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, 0);
								}
							}
							else {
								DbgPrint("! failed to build IoControlRequest !\n");
							}

							ObDereferenceObject(disk);
						}

						PFUNCTIONAL_DEVICE_EXTENSION ext = device->DeviceExtension;
						if (ext) {
							ULONG offset = ext->DeviceDescriptor->SerialNumberOffset;
						    ULONG size = ext->DeviceDescriptor->Size;
						    if (offset && offset < size) {
						        SIZE_T remaining = size - offset;
						        if (remaining > strlen(SERIAL) + 1)
						            RtlStringCchCopyA((PCHAR)ext->DeviceDescriptor + offset, remaining, SERIAL);
						    }

							// Disables SMART
							if (NT_SUCCESS(status = DiskEnableDisableFailurePrediction(ext, FALSE))) {
								++success;
							}
							else {
								DbgPrint("! DiskEnableDisableFailurePrediction failed: %p !\n", status);
							}

							++total;
						}

						ObDereferenceObject(device);
					}

					DbgPrint("disabling smart succeeded for %d/%d\n", success, total);
				}
				else {
					DbgPrint("! failed to get disk devices (got %d): %p !\n", length, status);
				}

				ExFreePool(devices);
			}
			else {
				DbgPrint("! failed to allocated %d disk devices !\n", length);
			}
		}
		else {
			DbgPrint("! failed to get disk device list size (got %d): %p !\n", length, status);
		}
	}
	else {
		DbgPrint("! failed to find DiskEnableDisableFailurePrediction !\n");
	}

	ObDereferenceObject(disk_object);

	// RaidUnitRegisterInterfaces -> Registry
	PVOID storport = GetBaseAddress("storport.sys", 0);
	if (storport) {
		RU_REGISTER_INTERFACES RaidUnitRegisterInterfaces = (RU_REGISTER_INTERFACES)FindPatternImage(storport, "\x48\x8B\xCB\xE8\x00\x00\x00\x00\x48\x8B\xCB\xE8\x00\x00\x00\x00\x85\xC0", "xxxx????xxxx????xx");
		if (RaidUnitRegisterInterfaces) {
			PBYTE RaidUnitExtension_SerialNumber = FindPatternImage(storport, "\x66\x39\x2C\x41", "xxxx");
			if (RaidUnitExtension_SerialNumber) {
				RaidUnitExtension_SerialNumber = FindPattern((PCHAR)RaidUnitExtension_SerialNumber, 32, "\x4C\x8D\x4F", "xxx");
				if (RaidUnitExtension_SerialNumber) {
					BYTE RaidUnitExtension_SerialNumber_offset = *(RaidUnitExtension_SerialNumber + 3);
					RaidUnitRegisterInterfaces = (RU_REGISTER_INTERFACES)((PBYTE)RaidUnitRegisterInterfaces + 8 + *(PINT)((PBYTE)RaidUnitRegisterInterfaces + 4));

					SpoofRaidUnits(RaidUnitRegisterInterfaces, RaidUnitExtension_SerialNumber_offset);
				}
				else {
					DbgPrint("! failed to find RaidUnitExtension_SerialNumber (1) !\n");
				}
			}
			else {
				DbgPrint("! failed to find RaidUnitExtension_SerialNumber (0) !\n");
			}
		}
		else {
			DbgPrint("! failed to find RaidUnitRegisterInterfaces !\n");
		}
	}
	else {
		DbgPrint("! failed to get \"storport.sys\" !\n");
	}
}

//Spoofing NTFS
VOID InitializeRandomSerial()
{
	LARGE_INTEGER tickCount;
	KeQueryTickCount(&tickCount);

	// Use the low part of the system tick count as a pseudo-random seed
	ULONG seed = tickCount.LowPart;

	// Generate the 32-bit random volume serial
	GlobalVolumeSerialNumber = RtlRandomEx(&seed);
}

// Hook Function
NTSTATUS HookedQueryVolumeInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp) {
	PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
	NTSTATUS status = OriginalQueryVolumeInformation(DeviceObject, Irp);

	if (NT_SUCCESS(status) && stack->Parameters.QueryVolume.FsInformationClass == FileFsVolumeInformation) {
		if (Irp->AssociatedIrp.SystemBuffer != NULL) {
			PFILE_FS_VOLUME_INFORMATION volInfo = (PFILE_FS_VOLUME_INFORMATION)Irp->AssociatedIrp.SystemBuffer;

			// Overwrite with our persistent randomized 32-bit serial
			volInfo->VolumeSerialNumber = GlobalVolumeSerialNumber;
		}
	}
	return status;
}

//USB Spoofing
NTSTATUS SpoofUSB() {
	UNICODE_STRING drvn;
	RtlInitUnicodeString(&drvn, L"\\Driver\\USBSTOR");

	PDRIVER_OBJECT drv = NULL;
	if (!NT_SUCCESS(ObReferenceObjectByName(&drvn, 64, 0, 0, *IoDriverObjectType, KernelMode, 0, (PVOID*)&drv))) {
		return STATUS_UNSUCCESSFUL;
	}

	ULONG s = (ULONG)__rdtsc();

	__try {
		for (PDEVICE_OBJECT dev = drv->DeviceObject; dev; dev = dev->NextDevice) {
			PUCHAR ext = (PUCHAR)dev->DeviceExtension;
			if (ext && MmIsAddressValid(ext)) {

				ULONG sig = *(PULONG)ext;
				if (sig == 0x214F4450 || sig == 0x214F4446) { // 'PDO!' or 'FDO!'

					// Directly use the standard static hardware descriptors offsets
					PULONG len_ptr = (PULONG)(ext + 0x40);
					if (!MmIsAddressValid(len_ptr)) continue;

					ULONG len = *len_ptr;
					PUCHAR ser = ext + 0x6C;

					if (len > 0 && len < 256 && MmIsAddressValid(ser)) {
						if (MmIsAddressValid(ser + len)) {
							for (ULONG i = 0; i < len; i++) {
								if (ser[i] >= '0' && ser[i] <= 'z') {
									ser[i] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"[RtlRandomEx(&s) % 36];
								}
							}
						}
					}
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		// Safe protection against structural mismatches if anything ever shifts
	}

	ObDereferenceObject(drv);
	return STATUS_SUCCESS;
}

//Volumes and WPP
VOID DiskFilter(PCONTEXT context, PVOID returnAddress, PVOID* frame, PVOID* base) {
	UNREFERENCED_PARAMETER(context);

	for (; *frame != returnAddress; ++frame) {
		if (frame >= base) {
			return;
		}
	}

	PIRP irp = *(frame + 6);
	if (!irp) {
		return;
	}

	PIO_STACK_LOCATION ioc = IoGetCurrentIrpStackLocation(irp);
	switch (ioc->Parameters.DeviceIoControl.IoControlCode) {
	case IOCTL_STORAGE_QUERY_PROPERTY:
		if (((PSTORAGE_PROPERTY_QUERY)irp->AssociatedIrp.SystemBuffer)->PropertyId == StorageDeviceProperty) {
			ChangeIoc(ioc, irp, StorageQueryIoc);
		}
		break;
	case IOCTL_ATA_PASS_THROUGH:
		ChangeIoc(ioc, irp, AtaPassIoc);
		break;
	case SMART_RCV_DRIVE_DATA:
		ChangeIoc(ioc, irp, SmartDataIoc);
		break;
	}
}

NTSTATUS SetDiskWpp() {
	UNICODE_STRING diskStr = RTL_CONSTANT_STRING(L"\\Driver\\Disk");
	PDRIVER_OBJECT diskObject = 0;

	NTSTATUS status = ObReferenceObjectByName(&diskStr, OBJ_CASE_INSENSITIVE, 0, 0, *IoDriverObjectType, KernelMode, 0, &diskObject);
	if (!NT_SUCCESS(status)) {
		DbgPrint("! failed to get %wZ driver object: %x !\n", &diskStr, status);
		return status;
	}

	PVOID wppGlobal = FindPatternImage(diskObject->DriverStart, "\x48\x89\x3D", "xxx"); //WPP_GLOBAL_CONTROL
	if (!wppGlobal) {
		DbgPrint("! failed to find %wZ WppGlobal !\n", &diskStr);

		ObDereferenceObject(diskObject);
		return STATUS_FAILED_DRIVER_ENTRY;
	}

	PVOID wppTraceMessage = FindPatternImage(diskObject->DriverStart, "\x48\x8B\x05\x00\x00\x00\x00\x48\x83", "xxx????xx"); //WPP_MAIN_CB.Dpc.DeferredRoutine
	if (!wppTraceMessage) {
		DbgPrint("! failed to find %wZ WppTraceMessage !\n", &diskStr);

		ObDereferenceObject(diskObject);
		return STATUS_FAILED_DRIVER_ENTRY;
	}

	PVOID returnAddress = FindPatternImage(diskObject->DriverStart, "\x90\xE9\x00\x00\x00\x00\x48\x8D\x54", "xx????xxx"); //nop
	if (!returnAddress) {
		DbgPrint("! failed to find %wZ return address !\n", &diskStr);

		ObDereferenceObject(diskObject);
		return STATUS_FAILED_DRIVER_ENTRY;
	}

	WppSet(returnAddress, DiskFilter, RELATIVE_ADDR(wppGlobal, 7), RELATIVE_ADDR(wppTraceMessage, 7));

	DbgPrint("success for %wZ\n", &diskStr);
	ObDereferenceObject(diskObject);
	return STATUS_SUCCESS;
}

//Mount
NTSTATUS MountPointsIoc(PDEVICE_OBJECT device, PIRP irp, PVOID context) {
	if (context) {
		IOC_REQUEST request = *(PIOC_REQUEST)context;
		ExFreePool(context);

		if (request.BufferLength >= sizeof(MOUNTMGR_MOUNT_POINTS)) {
			PMOUNTMGR_MOUNT_POINTS points = (PMOUNTMGR_MOUNT_POINTS)request.Buffer;
			for (DWORD i = 0; i < points->NumberOfMountPoints; ++i) {
				PMOUNTMGR_MOUNT_POINT point = &points->MountPoints[i];
				if (point->UniqueIdOffset) {
					point->UniqueIdLength = 0;
				}

				if (point->SymbolicLinkNameOffset) {
					point->SymbolicLinkNameLength = 0;
				}
			}
		}

		if (request.OldRoutine && irp->StackCount > 1) {
			return request.OldRoutine(device, irp, request.OldContext);
		}
	}

	return STATUS_SUCCESS;
}

NTSTATUS MountUniqueIoc(PDEVICE_OBJECT device, PIRP irp, PVOID context) {
	if (context) {
		IOC_REQUEST request = *(PIOC_REQUEST)context;
		ExFreePool(context);

		if (request.BufferLength >= sizeof(MOUNTDEV_UNIQUE_ID)) {
			((PMOUNTDEV_UNIQUE_ID)request.Buffer)->UniqueIdLength = 0;
		}

		if (request.OldRoutine && irp->StackCount > 1) {
			return request.OldRoutine(device, irp, request.OldContext);
		}
	}

	return STATUS_SUCCESS;
}

VOID MountFilter(PCONTEXT context, PVOID returnAddress, PVOID* frame, PVOID* base) {
	UNREFERENCED_PARAMETER(returnAddress);
	UNREFERENCED_PARAMETER(frame);
	UNREFERENCED_PARAMETER(base);

	PIRP irp = (PIRP)context->Rdi;
	if (!irp) {
		return;
	}

	PIO_STACK_LOCATION ioc = IoGetCurrentIrpStackLocation(irp);
	switch (ioc->Parameters.DeviceIoControl.IoControlCode) {
	case IOCTL_MOUNTMGR_QUERY_POINTS:
		ChangeIoc(ioc, irp, MountPointsIoc);
		break;
	case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID:
		ChangeIoc(ioc, irp, MountUniqueIoc);
		break;
	}
}

NTSTATUS SetMountWpp() {
	UNICODE_STRING mountStr = RTL_CONSTANT_STRING(L"\\Driver\\mountmgr");
	PDRIVER_OBJECT mountObject = 0;

	NTSTATUS status = ObReferenceObjectByName(&mountStr, OBJ_CASE_INSENSITIVE, 0, 0, *IoDriverObjectType, KernelMode, 0, &mountObject);
	if (!NT_SUCCESS(status)) {
		DbgPrint("! failed to get %wZ driver object: %x !\n", &mountStr, status);
		return status;
	}

	PVOID wppGlobal = FindPatternImage(mountObject->DriverStart, "\x48\x89\x3D", "xxx");
	if (!wppGlobal) {
		DbgPrint("! failed to find %wZ WppGlobal !\n", &mountStr);

		ObDereferenceObject(mountObject);
		return STATUS_FAILED_DRIVER_ENTRY;
	}

	PVOID wppTraceMessage = FindPatternImage(mountObject->DriverStart, "\x48\x8B\x05\x00\x00\x00\x00\x4C\x8D\x05", "xxx????xxx");
	if (!wppTraceMessage) {
		DbgPrint("! failed to find %wZ WppTraceMessage !\n", &mountStr);

		ObDereferenceObject(mountObject);
		return STATUS_FAILED_DRIVER_ENTRY;
	}

	PVOID returnAddress = FindPatternImage(mountObject->DriverStart, "\x45\x8B\xCE\xE8\x00\x00\x00\x00\x90\xE9", "xxxx????xx");
	if (!returnAddress) {
		DbgPrint("! failed to find %wZ return address !\n", &mountStr);

		ObDereferenceObject(mountObject);
		return STATUS_FAILED_DRIVER_ENTRY;
	}
	returnAddress = (PVOID)((ULONG_PTR)returnAddress + 8);

	WppSet((PBYTE)returnAddress + 8, MountFilter, RELATIVE_ADDR(wppGlobal, 7), RELATIVE_ADDR(wppTraceMessage, 7));

	DbgPrint("success for %wZ\n", &mountStr);
	ObDereferenceObject(mountObject);
	return STATUS_SUCCESS;
}

//Wpp
DEVICE_OBJECT fakeWppGlobal = {
	.Timer = (PIO_TIMER)WPP_FLAG_ALL,
};

struct {
	WPP Buffer[0x100];
	ULONG Length;
} wpps = { 0 };

VOID WppSet(PVOID returnAddress, WPP_FILTER filter, PDEVICE_OBJECT* wppGlobal, PVOID* wppTraceMessage) {
	if (wpps.Length >= 0x100) return;  // ADD THIS
	PWPP wpp = &wpps.Buffer[wpps.Length++];

	wpp->ReturnAddress = returnAddress;
	wpp->Filter = filter;

	wpp->WppGlobal = wppGlobal;
	wpp->WppTraceMessage = wppTraceMessage;

	wpp->WppGlobalOriginal = InterlockedExchangePointer(wppGlobal, &fakeWppGlobal);
	wpp->WppTraceMessageOriginal = InterlockedExchangePointer(wppTraceMessage, (PVOID)WppTraceMessage);
}

VOID WppUndo() {
	for (ULONG i = 0; i < wpps.Length; ++i) {
		PWPP wpp = &wpps.Buffer[i];

		InterlockedExchangePointer(wpp->WppGlobal, wpp->WppGlobalOriginal);
		InterlockedExchangePointer(wpp->WppTraceMessage, wpp->WppTraceMessageOriginal);
	}
}

ULONG WppTraceMessage(VOID* loggerHandle, ULONG messageFlags, LPCGUID messageGuid, USHORT messageNumber, ...) {
	UNREFERENCED_PARAMETER(loggerHandle);
	UNREFERENCED_PARAMETER(messageFlags);
	UNREFERENCED_PARAMETER(messageGuid);
	UNREFERENCED_PARAMETER(messageNumber);

	CONTEXT context;
	RtlCaptureContext(&context);

	PVOID returnAddress = 0;
	if (!RtlCaptureStackBackTrace(2, 1, &returnAddress, 0)) {
		return 0;
	}

	for (ULONG i = 0; i < wpps.Length; ++i) {
		PWPP wpp = &wpps.Buffer[i];
		if (wpp->ReturnAddress == returnAddress) {
			wpp->Filter(&context, returnAddress, (PVOID*)_AddressOfReturnAddress(), ((PVOID*)PsGetCurrentThreadStackBase()) - 1);

			break;
		}
	}

	return 0;
}

//NIC
BOOLEAN InitializeNdisOffsets() {
	RTL_OSVERSIONINFOW osInfo = { 0 };
	osInfo.dwOSVersionInfoSize = sizeof(RTL_OSVERSIONINFOW);

	if (!NT_SUCCESS(RtlGetVersion(&osInfo))) {
		return FALSE;
	}

	ULONG buildNumber = osInfo.dwBuildNumber;

	// Windows 10 and 11 share Major Version 10
	if (osInfo.dwMajorVersion == 10) {

		if (buildNumber >= 22621) {
			// Windows 11 (22H2, 23H2, 24H2)
			g_NdisOffsets.FilterToMiniport = 0x10;
			g_NdisOffsets.MiniportToIfBlock = 0x2A8;
		}
		else if (buildNumber >= 22000) {
			// Windows 11 (21H2)
			g_NdisOffsets.FilterToMiniport = 0x10;
			g_NdisOffsets.MiniportToIfBlock = 0x2A0;
		}
		else if (buildNumber >= 19041) {
			// Windows 10 (20H1, 20H2, 21H1, 21H2, 22H2)
			g_NdisOffsets.FilterToMiniport = 0x10;
			g_NdisOffsets.MiniportToIfBlock = 0x280;
		}
		else if (buildNumber >= 17134) {
			// Older Windows 10 (1803, 1809, 1903, 1909)
			g_NdisOffsets.FilterToMiniport = 0x10;
			g_NdisOffsets.MiniportToIfBlock = 0x230;
		}
		else {
			// Build is too old or unsupported
			return FALSE;
		}
		return TRUE;
	}
	return FALSE;
}

NTSTATUS NICIoc(PDEVICE_OBJECT device, PIRP irp, PVOID context) {
	if (context) {
		IOC_REQUEST request = *(PIOC_REQUEST)context;
		ExFreePool(context);

		if (irp->MdlAddress) {
			SpoofBuffer(SEED, (PBYTE)MmGetSystemAddressForMdl(irp->MdlAddress), 6);

			DbgPrint("handled NICIoc\n");
		}

		if (request.OldRoutine && irp->StackCount > 1) {
			return request.OldRoutine(device, irp, request.OldContext);
		}
	}

	return STATUS_SUCCESS;
}

NTSTATUS NICControl(PDEVICE_OBJECT device, PIRP irp) {
	for (DWORD i = 0; i < NICs.Length; ++i) {
		PNIC_DRIVER driver = &NICs.Drivers[i];

		if (driver->Original && driver->DriverObject == device->DriverObject) {
			PIO_STACK_LOCATION ioc = IoGetCurrentIrpStackLocation(irp);
			switch (ioc->Parameters.DeviceIoControl.IoControlCode) {
			case IOCTL_NDIS_QUERY_GLOBAL_STATS: {
				switch (*(PDWORD)irp->AssociatedIrp.SystemBuffer) {
				case OID_802_3_PERMANENT_ADDRESS:
				case OID_802_3_CURRENT_ADDRESS:
				case OID_802_5_PERMANENT_ADDRESS:
				case OID_802_5_CURRENT_ADDRESS:
					ChangeIoc(ioc, irp, NICIoc);
					break;
				}

				break;
			}
			}

			return driver->Original(device, irp);
		}
	}

	return STATUS_SUCCESS;
}

NTSTATUS NsiControl(PDEVICE_OBJECT device, PIRP irp) {
	PIO_STACK_LOCATION ioc = IoGetCurrentIrpStackLocation(irp);

	// Ricochet queries ARP via IOCTL_NSI_PROXY_ARP
	if (ioc->Parameters.DeviceIoControl.IoControlCode == IOCTL_NSI_PROXY_ARP) {
		NTSTATUS status = NsiControlOriginal(device, irp);

		// After the real driver fills the buffer, we wipe it
		if (NT_SUCCESS(status) && irp->UserBuffer) {
			__try {
				ProbeForWrite(
					irp->UserBuffer,
					ioc->Parameters.DeviceIoControl.OutputBufferLength,
					sizeof(UCHAR)
				);
				// Wiping the ARP table prevents Ricochet from seeing 
				// inconsistencies between your MAC and the network
				RtlZeroMemory(irp->UserBuffer, ioc->Parameters.DeviceIoControl.OutputBufferLength);
			}
			__except (EXCEPTION_EXECUTE_HANDLER) {}
		}
		return status;
	}
	return NsiControlOriginal(device, irp);
}

void SpoofNIC() {
    // 1. NSI proxy hook — covers ARP regardless of adapter type
    SwapControl(RTL_CONSTANT_STRING(L"\\Driver\\nsiproxy"), NsiControl, NsiControlOriginal);

    // 2. NDIS version offsets
    if (!InitializeNdisOffsets()) {
        DbgPrint("[hwid] Unsupported Windows version for NDIS walk\n");
        return;
    }

    // 3. NDIS structural walk — randomize permanent MAC in ndis.sys
    PVOID ndisBase = GetBaseAddress("ndis.sys", 0);
    if (ndisBase) {
        PBYTE pList = FindPatternImage(ndisBase,
            "\x48\x8B\x05\x00\x00\x00\x00\x48\x85\xC0\x74\x00\x48\x8B\x40",
            "xxx????xxxx?xxx");

        if (pList) {
            __try {
                PNDIS_FILTER_BLOCK filter = *(PNDIS_FILTER_BLOCK*)(pList + 7 + *(PINT)(pList + 3));
                DWORD count = 0;

                while (filter) {
                    PVOID miniport = *(PVOID*)((PBYTE)filter + g_NdisOffsets.FilterToMiniport);
                    if (miniport && MmIsAddressValid(miniport)) {
                        PNDIS_IF_BLOCK block = *(PNDIS_IF_BLOCK*)((PBYTE)miniport + g_NdisOffsets.MiniportToIfBlock);
                        if (block && MmIsAddressValid(block)) {
                            for (ULONG j = 0; j < 6; j++)
                                block->ifPhysAddress.Address[j] = (BYTE)(RtlRandomEx(&SEED) % 0xFF);
                            for (ULONG j = 0; j < 6; j++)
                                block->PermanentPhysAddress.Address[j] = (BYTE)(RtlRandomEx(&SEED) % 0xFF);
                            count++;
                        }
                    }
                    filter = filter->NextFilter;
                }
                DbgPrint("[hwid] Permanent MAC spoofed for %d interfaces\n", count);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                DbgPrint("[hwid] Failed to walk NDIS filter list\n");
            }
        }
    }
}

//ARP
NTSTATUS InitializeTcpipOffsets() {
	RTL_OSVERSIONINFOW osInfo = { 0 };
	osInfo.dwOSVersionInfoSize = sizeof(osInfo);

	NTSTATUS status = RtlGetVersion(&osInfo);
	if (!NT_SUCCESS(status)) {
		return status;
	}

	ULONG build = osInfo.dwBuildNumber;

	switch (build) {
	case 19041: // Windows 10 20H1 down to 22H2
	case 19042:
	case 19043:
	case 19044:
	case 19045:
		g_TcpipOffsets.BuildNumber = build;
		g_TcpipOffsets.InterfaceList = 0x4DC8;
		g_TcpipOffsets.InterfaceListEntry = 0xA0;
		g_TcpipOffsets.NeighborTable = 0x158;
		g_TcpipOffsets.NeighborList = 0x38;
		g_TcpipOffsets.NeighborListEntry = 0x40;
		g_TcpipOffsets.IpAddress = 0xA8;
		g_TcpipOffsets.MacV4 = 0xAC;
		g_TcpipOffsets.MacV6 = 0xB8;
		break;

	case 22621: // Windows 11 (Offsets will need adjustment for these targets)
	case 22631:
		g_TcpipOffsets.BuildNumber = build;
		g_TcpipOffsets.InterfaceList = 0x4E00; // Placeholders
		g_TcpipOffsets.InterfaceListEntry = 0xA0;
		g_TcpipOffsets.NeighborTable = 0x160;
		g_TcpipOffsets.NeighborList = 0x38;
		g_TcpipOffsets.NeighborListEntry = 0x40;
		g_TcpipOffsets.IpAddress = 0xA8;
		g_TcpipOffsets.MacV4 = 0xAC;
		g_TcpipOffsets.MacV6 = 0xB8;
		break;

	default:
		return STATUS_NOT_SUPPORTED;
	}

	return STATUS_SUCCESS;
}

// 2. Extracted helper function replacing the C++ lambda
void ProcessInterfaceList(PVOID g, BOOLEAN v4, PULONG seed) {
	if (!g || !MmIsAddressValid(g)) return;

	__try {
		PLIST_ENTRY h = (PLIST_ENTRY)((PUCHAR)g + g_TcpipOffsets.InterfaceList);
		if (!MmIsAddressValid(h) || !MmIsAddressValid(h->Flink)) return;

		for (PLIST_ENTRY e = h->Flink; e != h && MmIsAddressValid(e); e = e->Flink) {
			PUCHAR ifc = (PUCHAR)e - g_TcpipOffsets.InterfaceListEntry;
			if (!MmIsAddressValid(ifc)) continue;

			PVOID* ns_ptr = (PVOID*)(ifc + g_TcpipOffsets.NeighborTable);
			if (!MmIsAddressValid(ns_ptr)) continue;

			PVOID ns = *ns_ptr;
			if (MmIsAddressValid(ns)) {
				PLIST_ENTRY nl = (PLIST_ENTRY)((PUCHAR)ns + g_TcpipOffsets.NeighborList);
				if (!MmIsAddressValid(nl) || !MmIsAddressValid(nl->Flink)) continue;

				for (PLIST_ENTRY ne = nl->Flink; ne != nl && MmIsAddressValid(ne); ne = ne->Flink) {
					PUCHAR n = (PUCHAR)ne - g_TcpipOffsets.NeighborListEntry;
					if (!MmIsAddressValid(n)) continue;

					PUCHAR ip = n + g_TcpipOffsets.IpAddress;
					PUCHAR mc = n + (v4 ? g_TcpipOffsets.MacV4 : g_TcpipOffsets.MacV6);

					if (MmIsAddressValid(ip) && MmIsAddressValid(mc)) {
						if (v4 && ip[3] == 1) continue; // Skip gateway

						for (int i = 3; i < 6; i++) {
							mc[i] = (UCHAR)(RtlRandomEx(seed) & 0xFF);
						}
					}
				}
			}
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		// Suppress structure boundary faults safely
	}
}

NTSTATUS SpoofARP() {
	if (g_TcpipOffsets.BuildNumber == 0) {
		return STATUS_NOT_SUPPORTED;
	}

	PVOID b = GetBaseAddress("tcpip.sys", 0);
	if (!b) return STATUS_UNSUCCESSFUL;

	ULONG s = (ULONG)__rdtsc();

	PUCHAR i4 = (PUCHAR)FindPatternImage(b,
		"\x48\x8D\x0D\x00\x00\x00\x00\xE8\x00\x00\x00\x00\x48\x8B\xD8\x48\x85\xC0\x74\x00\x48\x8D\x54\x24\x00\x48\x8B\xC8",
		"xxx????x????xxxxxxx?xxxx?xxx"
	);
	if (i4) {
		ProcessInterfaceList((PVOID)(i4 + 7 + *(LONG*)(i4 + 3)), TRUE, &s);
	}

	PUCHAR i6 = (PUCHAR)FindPatternImage(b,
		"\x48\x8D\x15\x00\x00\x00\x00\x33\xC9\x00\x00\x00\x48\x89\x45",
		"xxx????xx???xxx"
	);
	if (i6) {
		ProcessInterfaceList((PVOID)(i6 + 7 + *(LONG*)(i6 + 3)), FALSE, &s);
	}

	return STATUS_SUCCESS;
}

void SpoofSMBIOS() {
	PVOID base = GetBaseAddress("ntoskrnl.exe", 0);
	if (!base) {
		DbgPrint("! failed to get \"ntoskrnl.exe\" !\n");
		return;
	}

	PPHYSICAL_ADDRESS WmipSMBiosTablePhysicalAddress = NULL;

	// Try multiple patterns for different Windows versions - USING BYTE ARRAYS
	BYTE patterns[][20] = {
		// Windows 10 1809 and earlier (original pattern)
		{0x48, 0x8B, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC9, 0x74, 0x00, 0x8B, 0x15},
		// Windows 10 1903-2004
		{0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x00, 0x8B, 0x15},
		// Windows 10 20H2+
		{0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x8B, 0x15},
		// Windows 11 21H2+
		{0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x44, 0x8B},
		// Alternative pattern for newer builds
		{0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x00, 0x44, 0x8B}
	};

	CHAR* masks[] = {
		"xxx????xxxx?xx",
		"xxx????xxxx?xx",
		"xxx????xxxx????xx",
		"xxx????xxxx????xx",
		"xxx????xxxx?xx"
	};

	for (int i = 0; i < 5; i++) {
		WmipSMBiosTablePhysicalAddress = FindPatternImage(base, (PCHAR)patterns[i], masks[i]);
		if (WmipSMBiosTablePhysicalAddress) {
			break;
		}
	}

	if (WmipSMBiosTablePhysicalAddress) {
		// Calculate the actual address (pattern + offset)
		WmipSMBiosTablePhysicalAddress = (PPHYSICAL_ADDRESS)((PBYTE)WmipSMBiosTablePhysicalAddress + 7 + *(PINT)((PBYTE)WmipSMBiosTablePhysicalAddress + 3));

		// Clear the SMBIOS physical address
		memset(WmipSMBiosTablePhysicalAddress, 0, sizeof(PHYSICAL_ADDRESS));

		DbgPrint("nulled SMBIOS table physical address\n");
	}
	else {
		DbgPrint("! WmipSMBiosTablePhysicalAddress not found with any pattern!\n");
	}

	PBYTE ExpBootEnvironmentInformation = NULL;

	// Fix boot patterns too - USING BYTE ARRAYS
	BYTE bootPatterns[][15] = {
		// Original pattern
		{0x0F, 0x10, 0x05, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x11, 0x00, 0x8B},
		// Windows 1903+ pattern
		{0x0F, 0x10, 0x05, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x11, 0x00, 0x48},
		// Alternative pattern
		{0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0x0F, 0x10, 0x00, 0x0F, 0x11}
	};

	CHAR* bootMasks[] = {
		"xxx????xx?x",
		"xxx????xx?x",
		"xxx????xx?xx"
	};

	for (int i = 0; i < 3; i++) {
		ExpBootEnvironmentInformation = FindPatternImage(base, (PCHAR)bootPatterns[i], bootMasks[i]);
		if (ExpBootEnvironmentInformation) {
			DbgPrint("Found boot pattern %d\n", i);
			break;
		}
	}

	if (ExpBootEnvironmentInformation) {
		ExpBootEnvironmentInformation = ExpBootEnvironmentInformation + 7 + *(PINT)(ExpBootEnvironmentInformation + 3);
		SpoofBuffer(SEED, ExpBootEnvironmentInformation, 16);
		DbgPrint("handled ExpBootEnvironmentInformation\n");
	}
	else {
		DbgPrint("! WmipSMBiosTablePhysicalAddress not found with any pattern!\n");
	}
}

//NVME SECTION
NTSTATUS SpoofSingleNVMeController(HANDLE hKey) {
	CHAR fakeSerial[21] = "S5GXNV0M";
	CHAR fakeModel[41] = "Samsung SSD 970 EVO Plus 1TB";
	UNICODE_STRING valueName;

	// Add random digits to serial - USING EXISTING Random() FUNCTION
	for (int i = 8; i < 16; i++) {
		fakeSerial[i] = "0123456789ABCDEF"[Random(&SEED) % 16];
	}
	fakeSerial[16] = '\0';

	// Spoof SerialNumber
	RtlInitUnicodeString(&valueName, L"SerialNumber");
	ZwSetValueKey(hKey, &valueName, 0, REG_SZ,
		fakeSerial, (ULONG)strlen(fakeSerial) + 1);

	// Spoof FriendlyName  
	RtlInitUnicodeString(&valueName, L"FriendlyName");
	ZwSetValueKey(hKey, &valueName, 0, REG_SZ,
		fakeModel, (ULONG)strlen(fakeModel) + 1);

	DbgPrint("[hwid] NVMe spoofed: %s - %s\n", fakeSerial, fakeModel);
	return STATUS_SUCCESS;
}

NTSTATUS TraverseAndSpoofNVMeRegistry(PUNICODE_STRING registryPath) {
	NTSTATUS status;
	HANDLE hKey = NULL;
	OBJECT_ATTRIBUTES objectAttributes;

	InitializeObjectAttributes(&objectAttributes, registryPath, OBJ_CASE_INSENSITIVE, NULL, NULL);

	status = ZwOpenKey(&hKey, KEY_READ | KEY_SET_VALUE, &objectAttributes);
	if (!NT_SUCCESS(status)) {
		DbgPrint("[hwid] Cannot open NVMe registry path: 0x%X\n", status);
		return status;
	}

	SpoofSingleNVMeController(hKey);
	ZwClose(hKey);

	return STATUS_SUCCESS;
}

NTSTATUS SpoofNVMeDevices() {
	NTSTATUS status;
	UNICODE_STRING nvmePath;

	RtlInitUnicodeString(&nvmePath, L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Enum\\NVME");
	status = TraverseAndSpoofNVMeRegistry(&nvmePath);

	if (!NT_SUCCESS(status)) {
		DbgPrint("[hwid] NVMe spoofing failed: 0x%X\n", status);
	}
	else {
		DbgPrint("[hwid] NVMe spoofing completed\n");
	}

	return status;
}

//GPU
void WriteRandomizedRegString(ULONG relativePath, PCWSTR path, PCWSTR valueName, PCWSTR baseFormat, ULONG seedMod) {
	WCHAR spoofedBuffer[REG_MAX_KEY_LENGTH];
	// Generate a unique format value based off your global driver seed
	RtlStringCchPrintfW(spoofedBuffer, REG_MAX_KEY_LENGTH, baseFormat, (SEED ^ seedMod) % 0xFFFF);

	UNICODE_STRING valueNameString;
	RtlInitUnicodeString(&valueNameString, valueName);

	// Write straight into the targeted hardware registry paths
	RtlWriteRegistryValue(
		relativePath,
		path,
		valueNameString.Buffer,
		REG_SZ,
		spoofedBuffer,
		(ULONG)(wcslen(spoofedBuffer) * sizeof(WCHAR) + sizeof(WCHAR))
	);
}

void SpoofGPU() {
	DbgPrint("[hwid] ++ Initializing GPU structural cleanup...\n");

	// 1. FIXED: Added the missing backslash to \\Machine to fix the escape sequence error
	WriteRandomizedRegString(RTL_REGISTRY_ABSOLUTE, L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000", L"DriverDesc", L"NVIDIA GeForce RTX 40%02d Ti", 0x1111);
	WriteRandomizedRegString(RTL_REGISTRY_ABSOLUTE, L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000", L"ProviderName", L"NVIDIA", 0x2222);

	// 2. Clear out the cached NVIDIA internal UUID logs if present inside the software settings
	UNICODE_STRING nvUuidStr = RTL_CONSTANT_STRING(L"NVIDIA_UUID");
	RtlDeleteRegistryValue(RTL_REGISTRY_ABSOLUTE, L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\0000", nvUuidStr.Buffer);

	// 3. Spoof the Hardware ID strings inside the Plug and Play PCI Enumerator
	WriteRandomizedRegString(RTL_REGISTRY_ABSOLUTE, L"\\Registry\\Machine\\System\\CurrentControlSet\\Enum\\PCI", L"HardwareID", L"PCI\\VEN_10DE&DEV_2704&SUBSYS_000010DE&REV_A1", 0x3333);

	DbgPrint("[hwid] ++ GPU identity registers successfully modified\n");
}

//PURGE GRAPHICS CACHE
void PurgeGraphicsCache(PUNICODE_STRING keyPath) {
	OBJECT_ATTRIBUTES objAttr;
	HANDLE hKey;
	InitializeObjectAttributes(&objAttr, keyPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

	if (NT_SUCCESS(ZwOpenKey(&hKey, KEY_ALL_ACCESS, &objAttr))) {
		ZwDeleteKey(hKey); // Deletes the key and all its cached telemetry values
		ZwClose(hKey);

		// Recreate the root key empty so the OS doesn't throw errors
		HANDLE hNewKey;
		if (NT_SUCCESS(ZwCreateKey(&hNewKey, KEY_ALL_ACCESS, &objAttr, 0, NULL, REG_OPTION_NON_VOLATILE, NULL))) {
			ZwClose(hNewKey);
		}
	}
}

NTSTATUS SpoofMonitorReg() {
	// 2. Open standard hardware DISPLAY key
	UNICODE_STRING masterKeyPath = RTL_CONSTANT_STRING(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Enum\\DISPLAY");
	OBJECT_ATTRIBUTES objAttr;
	HANDLE hMasterKey;

	InitializeObjectAttributes(&objAttr, &masterKeyPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
	NTSTATUS status = ZwOpenKey(&hMasterKey, KEY_ALL_ACCESS, &objAttr);
	if (!NT_SUCCESS(status)) return status;

	ULONG index = 0;
	BYTE buffer[1024];
	PKEY_BASIC_INFORMATION keyInfo = (PKEY_BASIC_INFORMATION)buffer;
	ULONG resultLength;

	while (NT_SUCCESS(ZwEnumerateKey(hMasterKey, index++, KeyBasicInformation, keyInfo, sizeof(buffer), &resultLength))) {
		UNICODE_STRING modelName;
		modelName.Buffer = keyInfo->Name;
		modelName.Length = modelName.MaximumLength = (USHORT)keyInfo->NameLength;

		HANDLE hModelKey;
		OBJECT_ATTRIBUTES modelAttr;
		InitializeObjectAttributes(&modelAttr, &modelName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, hMasterKey, NULL);

		if (NT_SUCCESS(ZwOpenKey(&hModelKey, KEY_ALL_ACCESS, &modelAttr))) {
			ULONG subIndex = 0;
			BYTE subBuffer[1024];
			PKEY_BASIC_INFORMATION subKeyInfo = (PKEY_BASIC_INFORMATION)subBuffer;

			while (NT_SUCCESS(ZwEnumerateKey(hModelKey, subIndex++, KeyBasicInformation, subKeyInfo, sizeof(subBuffer), &resultLength))) {
				UNICODE_STRING instanceName;
				instanceName.Buffer = subKeyInfo->Name;
				instanceName.Length = instanceName.MaximumLength = (USHORT)subKeyInfo->NameLength;

				HANDLE hInstanceKey;
				OBJECT_ATTRIBUTES instanceAttr;
				InitializeObjectAttributes(&instanceAttr, &instanceName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, hModelKey, NULL);

				if (NT_SUCCESS(ZwOpenKey(&hInstanceKey, KEY_ALL_ACCESS, &instanceAttr))) {
					UNICODE_STRING paramsPath = RTL_CONSTANT_STRING(L"Device Parameters");
					HANDLE hParamsKey;
					OBJECT_ATTRIBUTES paramsAttr;
					InitializeObjectAttributes(&paramsAttr, &paramsPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, hInstanceKey, NULL);

					if (NT_SUCCESS(ZwOpenKey(&hParamsKey, KEY_ALL_ACCESS, &paramsAttr))) {
						UNICODE_STRING edidValName = RTL_CONSTANT_STRING(L"EDID");
						UNICODE_STRING badEdidValName = RTL_CONSTANT_STRING(L"BAD_EDID");
						PUNICODE_STRING targetValueName = &edidValName;

						BYTE edidData[1024];
						PKEY_VALUE_PARTIAL_INFORMATION valueInfo = (PKEY_VALUE_PARTIAL_INFORMATION)edidData;

						// Try reading standard "EDID" first; if it fails, query "BAD_EDID"
						status = ZwQueryValueKey(hParamsKey, targetValueName, KeyValuePartialInformation, valueInfo, sizeof(edidData), &resultLength);
						if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
							targetValueName = &badEdidValName;
							status = ZwQueryValueKey(hParamsKey, targetValueName, KeyValuePartialInformation, valueInfo, sizeof(edidData), &resultLength);
						}

						if (NT_SUCCESS(status)) {
							// FIXED: Handle cases where the EDID binary data block length is completely empty/zero
							if (valueInfo->DataLength < 16) {
								valueInfo->DataLength = 16;
								RtlZeroMemory(valueInfo->Data, 16);
							}

							// Randomize Serial Number blocks safely inside the binary block
							PBYTE pSerial = &valueInfo->Data[12];
							for (int i = 0; i < 4; i++) {
								pSerial[i] = (BYTE)(RtlRandomEx(&SEED) % 0xFF);
							}

							ZwSetValueKey(hParamsKey, targetValueName, 0, REG_BINARY, valueInfo->Data, valueInfo->DataLength);
							DbgPrint("[hwid] Spoofed value %wZ under hardware instance.\n", targetValueName);
						}
						ZwClose(hParamsKey);
					}
					ZwClose(hInstanceKey);
				}
			}
			ZwClose(hModelKey);
		}
	}
	ZwClose(hMasterKey);
	return STATUS_SUCCESS;
}

//Monitor
static ULONG xorshift32(ULONG seed)
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return seed;
}

static ULONG MonLCG(ULONG* seed)
{
	*seed = xorshift32(*seed);
	return *seed;
}

static void InitMonitorStrings(void)
{
	if (g_MonitorStringsInit) return;

	static const char* vendors[] = {
		encrypt("AUS"), encrypt("SAM"), encrypt("DEL"), encrypt("LEN"),
		encrypt("AOC"), encrypt("BNQ"), encrypt("HWP"), encrypt("GSM")
	};
	static const char* models[] = {
		encrypt("VG279QM"),  encrypt("VG27AQ"),   encrypt("VG248QG"),
		encrypt("S24D330"),  encrypt("S27F350"),   encrypt("C27F390"),
		encrypt("P2419H"),   encrypt("U2722D"),    encrypt("S2721DGF"),
		encrypt("27GL850"),  encrypt("27GP850"),   encrypt("24GL600F"),
		encrypt("Q27G2S"),   encrypt("CQ27G2S"),   encrypt("AG274QZ"),
		encrypt("HP24mh"),   encrypt("HP27h"),     encrypt("E24d"),
		encrypt("27UK850"),  encrypt("27GL83A"),   encrypt("32GN650")
	};
	static const char alphanum[] = {
		'A','B','C','D','E','F','G','H','I','J','K','L','M',
		'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
		'0','1','2','3','4','5','6','7','8','9', 0
	};
	static const char digits[] = { '0','1','2','3','4','5','6','7','8','9', 0 };

	// Use boot time + tick count so it differs every run
	LARGE_INTEGER tickCount;
	KeQueryTickCount(&tickCount);
	ULONG seed = (ULONG)tickCount.LowPart ^ (ULONG)__rdtsc();

	ULONG vi = (MonLCG(&seed) >> 16) % (sizeof(vendors) / sizeof(vendors[0]));
	ULONG mi = (MonLCG(&seed) >> 16) % (sizeof(models) / sizeof(models[0]));

	RtlCopyMemory(g_SpoofedVendor, vendors[vi], 3);
	g_SpoofedVendor[3] = 0;

	// Use safe copy bound to your 14-byte configuration buffer size
	RtlCopyMemory(g_SpoofedName, models[mi], min(strlen(models[mi]) + 1, sizeof(g_SpoofedName)));
	g_SpoofedName[sizeof(g_SpoofedName) - 1] = 0;

	for (int i = 0; i < 4; i++)
		g_SpoofedSerial[i] = alphanum[(MonLCG(&seed) >> 16) % 26];
	for (int i = 4; i < 6; i++)
		g_SpoofedSerial[i] = digits[(MonLCG(&seed) >> 16) % 10];
	for (int i = 6; i < 9; i++)
		g_SpoofedSerial[i] = alphanum[(MonLCG(&seed) >> 16) % 36];
	for (int i = 9; i < 11; i++)
		g_SpoofedSerial[i] = digits[(MonLCG(&seed) >> 16) % 10];
	for (int i = 11; i < 13; i++)
		g_SpoofedSerial[i] = alphanum[(MonLCG(&seed) >> 16) % 26];
	g_SpoofedSerial[13] = 0;

	g_MonitorStringsInit = TRUE;

	logging_output(encrypt("monitor: vendor=%s model=%s serial=%s"),
		g_SpoofedVendor, g_SpoofedName, g_SpoofedSerial);
}

static void AnsiToWmiString(const char* src, PUSHORT dst, ULONG maxChars)
{
	for (ULONG i = 0; i < maxChars; i++)
		dst[i] = src[i] ? (USHORT)(UCHAR)src[i] : 0;
}

static void PatchEdidDescriptor(PUCHAR edid, ULONG edidLen, UCHAR type, const char* newStr)
{
	if (edidLen < 128) return;

	for (ULONG off = 54; off <= 108; off += 18)
	{
		if (edid[off] != 0x00 || edid[off + 1] != 0x00) continue;
		if (edid[off + 2] != 0x00)                         continue;
		if (edid[off + 3] != type)                         continue;

		PUCHAR data = edid + off + 5;
		RtlZeroMemory(data, 13);
		SIZE_T len = strlen(newStr);
		if (len > 12) len = 12;
		RtlCopyMemory(data, newStr, len);
		data[len] = 0x0A;
	}

	UCHAR sum = 0;
	for (ULONG i = 0; i < 127; i++) sum += edid[i];
	edid[127] = (UCHAR)(0x100 - sum);
}

// --- MAIN LOGIC FUNCTIONS ---

static void PatchRegistryEdid(void)
{
	UNICODE_STRING    displayPath;
	OBJECT_ATTRIBUTES oa;
	HANDLE            hDisplay = NULL;

	RtlInitUnicodeString(&displayPath, encrypt(L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet\\Enum\\DISPLAY"));
	InitializeObjectAttributes(&oa, &displayPath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

	if (!NT_SUCCESS(ZwOpenKey(&hDisplay, KEY_READ, &oa)))
		return;

	ULONG modelIdx = 0;
	while (TRUE)
	{
		ULONG    modelLen = 0;
		NTSTATUS s = ZwEnumerateKey(hDisplay, modelIdx, KeyBasicInformation, NULL, 0, &modelLen);
		if (s == STATUS_NO_MORE_ENTRIES) break;
		if (!modelLen) { modelIdx++; continue; }

		PKEY_BASIC_INFORMATION modelInfo = (PKEY_BASIC_INFORMATION)ExAllocatePoolWithTag(NonPagedPool, modelLen, 'TIDE');
		if (!modelInfo) { modelIdx++; continue; }

		s = ZwEnumerateKey(hDisplay, modelIdx++, KeyBasicInformation, modelInfo, modelLen, &modelLen);
		if (s == STATUS_NO_MORE_ENTRIES) { ExFreePoolWithTag(modelInfo, 'TIDE'); break; }
		if (!NT_SUCCESS(s)) { ExFreePoolWithTag(modelInfo, 'TIDE'); continue; }

		UNICODE_STRING modelName = { (USHORT)modelInfo->NameLength, (USHORT)modelInfo->NameLength, modelInfo->Name };

		HANDLE hModel = NULL;
		InitializeObjectAttributes(&oa, &modelName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, hDisplay, NULL);
		if (!NT_SUCCESS(ZwOpenKey(&hModel, KEY_READ, &oa))) { ExFreePoolWithTag(modelInfo, 'TIDE'); continue; }

		CHAR modelAnsi[64] = { 0 };
		ANSI_STRING modelAs; UNICODE_STRING modelUs = modelName;
		modelAs.Buffer = modelAnsi; modelAs.MaximumLength = sizeof(modelAnsi);
		RtlUnicodeStringToAnsiString(&modelAs, &modelUs, FALSE);
		ExFreePoolWithTag(modelInfo, 'TIDE');

		ULONG instIdx = 0;
		while (TRUE)
		{
			ULONG  instLen = 0;
			s = ZwEnumerateKey(hModel, instIdx, KeyBasicInformation, NULL, 0, &instLen);
			if (s == STATUS_NO_MORE_ENTRIES) break;
			if (!instLen) { instIdx++; continue; }

			PKEY_BASIC_INFORMATION instInfo = (PKEY_BASIC_INFORMATION)ExAllocatePoolWithTag(NonPagedPool, instLen, 'TIDE');
			if (!instInfo) { instIdx++; continue; }

			s = ZwEnumerateKey(hModel, instIdx++, KeyBasicInformation, instInfo, instLen, &instLen);
			if (s == STATUS_NO_MORE_ENTRIES) { ExFreePoolWithTag(instInfo, 'TIDE'); break; }
			if (!NT_SUCCESS(s)) { ExFreePoolWithTag(instInfo, 'TIDE'); continue; }

			UNICODE_STRING instName = { (USHORT)instInfo->NameLength, (USHORT)instInfo->NameLength, instInfo->Name };

			HANDLE hInst = NULL;
			InitializeObjectAttributes(&oa, &instName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, hModel, NULL);
			if (!NT_SUCCESS(ZwOpenKey(&hInst, KEY_READ, &oa))) { ExFreePoolWithTag(instInfo, 'TIDE'); continue; }
			ExFreePoolWithTag(instInfo, 'TIDE');

			UNICODE_STRING devParams;
			RtlInitUnicodeString(&devParams, encrypt(L"Device Parameters"));
			HANDLE hDevParams = NULL;
			InitializeObjectAttributes(&oa, &devParams, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, hInst, NULL);

			NTSTATUS openStatus = ZwOpenKey(&hDevParams, KEY_ALL_ACCESS, &oa);
			if (!NT_SUCCESS(openStatus))
			{
				ULONG disposition = 0;
				openStatus = ZwCreateKey(&hDevParams, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disposition);
			}
			if (!NT_SUCCESS(openStatus)) continue;

			UNICODE_STRING edidVal;
			RtlInitUnicodeString(&edidVal, encrypt(L"EDID"));

			ZwClose(hInst);

			ULONG edidInfoSize = 0;
			ZwQueryValueKey(hDevParams, &edidVal, KeyValueFullInformation, NULL, 0, &edidInfoSize);

			if (edidInfoSize > 0)
			{
				PKEY_VALUE_FULL_INFORMATION edidInfo = (PKEY_VALUE_FULL_INFORMATION)
					ExAllocatePoolWithTag(NonPagedPool, edidInfoSize, 'TIDE');

				if (edidInfo)
				{
					ULONG needed = 0;
					if (NT_SUCCESS(ZwQueryValueKey(hDevParams, &edidVal, KeyValueFullInformation,
						edidInfo, edidInfoSize, &needed)))
					{
						PUCHAR edidData = (PUCHAR)edidInfo + edidInfo->DataOffset;
						ULONG  edidLen = edidInfo->DataLength;

						ULONG seed = 0x12345678 ^ (modelIdx * 0x1337) ^ (instIdx * 0x7331);
						static const char alphanum[] = {
							'A','B','C','D','E','F','G','H','I','J','K','L','M',
							'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
							'0','1','2','3','4','5','6','7','8','9', 0
						};
						static const char digits[] = { '0','1','2','3','4','5','6','7','8','9', 0 };
						char devSerial[14] = { 0 };
						for (int c = 0; c < 4; c++) { seed = xorshift32(seed); devSerial[c] = alphanum[(seed >> 16) % 26]; }
						for (int c = 4; c < 6; c++) { seed = xorshift32(seed); devSerial[c] = digits[(seed >> 16) % 10]; }
						for (int c = 6; c < 9; c++) { seed = xorshift32(seed); devSerial[c] = alphanum[(seed >> 16) % 36]; }
						for (int c = 9; c < 11; c++) { seed = xorshift32(seed); devSerial[c] = digits[(seed >> 16) % 10]; }
						for (int c = 11; c < 13; c++) { seed = xorshift32(seed); devSerial[c] = alphanum[(seed >> 16) % 26]; }

						PatchEdidDescriptor(edidData, edidLen, 0xFF, devSerial);
						PatchEdidDescriptor(edidData, edidLen, 0xFC, g_SpoofedName);
						PatchEdidDescriptor(edidData, edidLen, 0xFE, g_SpoofedVendor);

						ZwSetValueKey(hDevParams, &edidVal, 0, REG_BINARY, edidData, edidLen);
						logging_output(encrypt("monitor edid: %s -> serial=%s"), modelAnsi, devSerial);
					}
					ExFreePoolWithTag(edidInfo, 'TIDE');
				}
			}

			ZwClose(hDevParams);
		}

		ZwClose(hModel);
	}

	ZwClose(hDisplay);
}

void SpoofMonitor(void)
{
	InitMonitorStrings();
	PatchRegistryEdid();
}

//Installation ID
void GenerateRandomGuidString(WCHAR* buffer) {
	const wchar_t* hex = L"0123456789abcdef";
	// Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx (36 characters + null terminator)
	for (int i = 0; i < 36; i++) {
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			buffer[i] = L'-';
		}
		else {
			buffer[i] = hex[RtlRandomEx(&SEED) % 16];
		}
	}
	buffer[36] = L'\0';
}

void SpoofInstallationID() {
	WCHAR randomGuid[37];

	// 1. Spoof Windows InstallationID
	GenerateRandomGuidString(randomGuid);
	RtlWriteRegistryValue(
		RTL_REGISTRY_ABSOLUTE,
		L"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion",
		L"InstallationID",
		REG_SZ,
		randomGuid,
		(ULONG)(wcslen(randomGuid) * sizeof(WCHAR) + sizeof(WCHAR))
	);

	// 2. Spoof MachineGuid (Highly tracked by game launchers and platforms)
	GenerateRandomGuidString(randomGuid);
	RtlWriteRegistryValue(
		RTL_REGISTRY_ABSOLUTE,
		L"\\Registry\\Machine\\Software\\Microsoft\\Cryptography",
		L"MachineGuid",
		REG_SZ,
		randomGuid,
		(ULONG)(wcslen(randomGuid) * sizeof(WCHAR) + sizeof(WCHAR))
	);

	DbgPrint("[hwid] ++ Registry telemetry structures randomized (InstallationID / MachineGuid)\n");
}

void RandomBuffer(PUCHAR b, int l) {
	ULONG s = (ULONG)__rdtsc();
	for (int i = 0; i < l; i++) b[i] = (UCHAR)(RtlRandomEx(&s) & 0xFF);
}

NTSTATUS SpoofTPM() {
	GUID g[] = { {0xeaec226f,0xc9a3,0x477a,{0xa8,0x26,0xdd,0xc7,0x16,0xcd,0xc0,0xe3}}, {0x1b463f9c,0x803c,0x49e4,{0xb4,0x68,0x28,0x68,0x90,0x84,0x79,0xb2}} };
	const wchar_t* v[] = { L"UnlockIDCopy", L"OfflineUniqueIDEKPubCRC", L"OfflineUniqueIDEKPub" };
	for (int i = 0; i < 3; i++) {
		UNICODE_STRING us; RtlInitUnicodeString(&us, v[i]);
		for (int k = 0; k < 2; k++) {
			UCHAR b[1024]; ULONG l = sizeof(b), a = 0;
			if (NT_SUCCESS(ExGetFirmwareEnvironmentVariable(&us, &g[k], b, &l, &a)) && l) {
				RandomBuffer(b, l);
				ExSetFirmwareEnvironmentVariable(&us, &g[k], b, l, a | 1);
				break;
			}
		}
	}
	return 0;
}

NTSTATUS DriverEntry() {
	// Remove all references to 'driver' parameter
	// driver->DriverUnload = DriverUnload;  // ← COMMENT THIS OUT

	ULONG64 time = 0;
	KeQuerySystemTime(&time);
	SEED = (DWORD)time;

	CHAR alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890";
	for (DWORD i = 0, l = (DWORD)strlen(SERIAL); i < l; ++i) {
		SERIAL[i] = alphabet[RtlRandomEx(&SEED) % (sizeof(alphabet) - 1)];
	}

	GlobalVolumeSerialNumber = RtlRandomEx(&SEED);

	DbgPrint("[hwid] ++ loading (serial: %s)\n", SERIAL);

	SpoofDisks();
	SpoofNIC();
	SpoofARP();
	SpoofSMBIOS();
	SpoofGPU();
	SpoofMonitor();
	SpoofNVMeDevices();
	SpoofTPM();
	SpoofInstallationID();

	DbgPrint("[hwid] ++ loaded\n");

	return STATUS_SUCCESS;
}
