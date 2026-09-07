/*
 * Minimal UEFI definitions for the ravynOS AArch64 booter.
 *
 * Only what the booter uses is declared; field order follows the UEFI
 * specification so the structures line up with the firmware's tables. On
 * AArch64 the UEFI calling convention is the standard AAPCS64, so no
 * calling-convention attributes are needed.
 */

#ifndef RAVYN_BOOTER_EFI_H
#define RAVYN_BOOTER_EFI_H

#include <stdint.h>
#include <stddef.h>

typedef uint8_t   UINT8;
typedef uint16_t  UINT16;
typedef uint32_t  UINT32;
typedef uint64_t  UINT64;
typedef int16_t   INT16;
typedef int32_t   INT32;
typedef int64_t   INT64;
typedef uint64_t  UINTN;
typedef int64_t   INTN;
typedef uint8_t   BOOLEAN;
typedef uint16_t  CHAR16;
typedef void      *EFI_HANDLE;
typedef void      *EFI_EVENT;
typedef UINTN     EFI_STATUS;
typedef UINT64    EFI_PHYSICAL_ADDRESS;
typedef UINT64    EFI_VIRTUAL_ADDRESS;
typedef UINTN     EFI_TPL;

#define EFI_SUCCESS             0
#define EFI_ERROR_BIT           (1ULL << 63)
#define EFI_LOAD_ERROR          (EFI_ERROR_BIT | 1)
#define EFI_INVALID_PARAMETER   (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED         (EFI_ERROR_BIT | 3)
#define EFI_BUFFER_TOO_SMALL    (EFI_ERROR_BIT | 5)
#define EFI_NOT_FOUND           (EFI_ERROR_BIT | 14)
#define EFI_ERROR(s)            (((INTN)(s)) < 0)

typedef struct {
	UINT32 Data1;
	UINT16 Data2;
	UINT16 Data3;
	UINT8  Data4[8];
} EFI_GUID;

typedef struct {
	UINT64 Signature;
	UINT32 Revision;
	UINT32 HeaderSize;
	UINT32 CRC32;
	UINT32 Reserved;
} EFI_TABLE_HEADER;

typedef enum {
	AllocateAnyPages,
	AllocateMaxAddress,
	AllocateAddress,
	MaxAllocateType
} EFI_ALLOCATE_TYPE;

typedef enum {
	EfiReservedMemoryType,
	EfiLoaderCode,
	EfiLoaderData,
	EfiBootServicesCode,
	EfiBootServicesData,
	EfiRuntimeServicesCode,
	EfiRuntimeServicesData,
	EfiConventionalMemory,
	EfiUnusableMemory,
	EfiACPIReclaimMemory,
	EfiACPIMemoryNVS,
	EfiMemoryMappedIO,
	EfiMemoryMappedIOPortSpace,
	EfiPalCode,
	EfiPersistentMemory,
	EfiMaxMemoryType
} EFI_MEMORY_TYPE;

typedef struct {
	UINT32               Type;
	UINT32               Pad;
	EFI_PHYSICAL_ADDRESS PhysicalStart;
	EFI_VIRTUAL_ADDRESS  VirtualStart;
	UINT64               NumberOfPages;
	UINT64               Attribute;
} EFI_MEMORY_DESCRIPTOR;

#define EFI_MEMORY_WB 0x0000000000000008ULL

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
	EFI_STATUS (*Reset)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
	EFI_STATUS (*OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
	EFI_STATUS (*TestString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
	EFI_STATUS (*QueryMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber, UINTN *Columns, UINTN *Rows);
	EFI_STATUS (*SetMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber);
	EFI_STATUS (*SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
	EFI_STATUS (*ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
	EFI_STATUS (*SetCursorPosition)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column, UINTN Row);
	EFI_STATUS (*EnableCursor)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN Visible);
	void *Mode;
};

typedef struct {
	EFI_TABLE_HEADER Hdr;
	EFI_TPL    (*RaiseTPL)(EFI_TPL NewTpl);
	void       (*RestoreTPL)(EFI_TPL OldTpl);
	EFI_STATUS (*AllocatePages)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
	EFI_STATUS (*FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
	EFI_STATUS (*GetMemoryMap)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap, UINTN *MapKey, UINTN *DescriptorSize, UINT32 *DescriptorVersion);
	EFI_STATUS (*AllocatePool)(EFI_MEMORY_TYPE PoolType, UINTN Size, void **Buffer);
	EFI_STATUS (*FreePool)(void *Buffer);
	void *CreateEvent;
	void *SetTimer;
	void *WaitForEvent;
	void *SignalEvent;
	void *CloseEvent;
	void *CheckEvent;
	void *InstallProtocolInterface;
	void *ReinstallProtocolInterface;
	void *UninstallProtocolInterface;
	EFI_STATUS (*HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
	void *Reserved;
	void *RegisterProtocolNotify;
	void *LocateHandle;
	void *LocateDevicePath;
	void *InstallConfigurationTable;
	void *LoadImage;
	void *StartImage;
	void *Exit;
	void *UnloadImage;
	EFI_STATUS (*ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
	void *GetNextMonotonicCount;
	EFI_STATUS (*Stall)(UINTN Microseconds);
	EFI_STATUS (*SetWatchdogTimer)(UINTN Timeout, UINT64 WatchdogCode, UINTN DataSize, CHAR16 *WatchdogData);
	void *ConnectController;
	void *DisconnectController;
	void *OpenProtocol;
	void *CloseProtocol;
	void *OpenProtocolInformation;
	void *ProtocolsPerHandle;
	void *LocateHandleBuffer;
	EFI_STATUS (*LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
	void *InstallMultipleProtocolInterfaces;
	void *UninstallMultipleProtocolInterfaces;
	void *CalculateCrc32;
	void *CopyMem;
	void *SetMem;
	void *CreateEventEx;
} EFI_BOOT_SERVICES;

typedef struct {
	EFI_GUID VendorGuid;
	void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

typedef struct {
	EFI_TABLE_HEADER Hdr;
	CHAR16 *FirmwareVendor;
	UINT32  FirmwareRevision;
	EFI_HANDLE ConsoleInHandle;
	void *ConIn;
	EFI_HANDLE ConsoleOutHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
	EFI_HANDLE StandardErrorHandle;
	EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
	void *RuntimeServices;
	EFI_BOOT_SERVICES *BootServices;
	UINTN NumberOfTableEntries;
	EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
	UINT32 Revision;
	EFI_HANDLE ParentHandle;
	EFI_SYSTEM_TABLE *SystemTable;
	EFI_HANDLE DeviceHandle;
	void *FilePath;
	void *Reserved;
	UINT32 LoadOptionsSize;
	void *LoadOptions;
	void *ImageBase;
	UINT64 ImageSize;
	EFI_MEMORY_TYPE ImageCodeType;
	EFI_MEMORY_TYPE ImageDataType;
	void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS (*Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle, CHAR16 *FileName, UINT64 OpenMode, UINT64 Attributes);
	EFI_STATUS (*Close)(EFI_FILE_PROTOCOL *This);
	EFI_STATUS (*Delete)(EFI_FILE_PROTOCOL *This);
	EFI_STATUS (*Read)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
	EFI_STATUS (*Write)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
	EFI_STATUS (*GetPosition)(EFI_FILE_PROTOCOL *This, UINT64 *Position);
	EFI_STATUS (*SetPosition)(EFI_FILE_PROTOCOL *This, UINT64 Position);
	EFI_STATUS (*GetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN *BufferSize, void *Buffer);
	EFI_STATUS (*SetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType, UINTN BufferSize, void *Buffer);
	EFI_STATUS (*Flush)(EFI_FILE_PROTOCOL *This);
};

#define EFI_FILE_MODE_READ 0x0000000000000001ULL

typedef struct {
	UINT16 Year;
	UINT8  Month, Day, Hour, Minute, Second, Pad1;
	UINT32 Nanosecond;
	INT16  TimeZone;
	UINT8  Daylight, Pad2;
} EFI_TIME;

typedef struct {
	UINT64 Size;
	UINT64 FileSize;
	UINT64 PhysicalSize;
	EFI_TIME CreateTime;
	EFI_TIME LastAccessTime;
	EFI_TIME ModificationTime;
	UINT64 Attribute;
	CHAR16 FileName[1];
} EFI_FILE_INFO;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
	UINT64 Revision;
	EFI_STATUS (*OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This, EFI_FILE_PROTOCOL **Root);
};

typedef enum {
	PixelRedGreenBlueReserved8BitPerColor,
	PixelBlueGreenRedReserved8BitPerColor,
	PixelBitMask,
	PixelBltOnly,
	PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
	UINT32 RedMask, GreenMask, BlueMask, ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
	UINT32 Version;
	UINT32 HorizontalResolution;
	UINT32 VerticalResolution;
	EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
	EFI_PIXEL_BITMASK PixelInformation;
	UINT32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
	UINT32 MaxMode;
	UINT32 Mode;
	EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
	UINTN SizeOfInfo;
	EFI_PHYSICAL_ADDRESS FrameBufferBase;
	UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
	void *QueryMode;
	void *SetMode;
	void *Blt;
	EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
	{ 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
	{ 0x0964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_FILE_INFO_GUID \
	{ 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
	{ 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }
#define EFI_DT_TABLE_GUID \
	{ 0xb1b621d5, 0xf19c, 0x41a5, { 0x83, 0x0b, 0xd9, 0x15, 0x2c, 0x69, 0xaa, 0xe0 } }
#define EFI_RNG_PROTOCOL_GUID \
	{ 0x3152bca5, 0xeade, 0x433d, { 0x86, 0x2e, 0xc0, 0x1c, 0xdc, 0x29, 0x1f, 0x44 } }

typedef struct EFI_RNG_PROTOCOL EFI_RNG_PROTOCOL;
struct EFI_RNG_PROTOCOL {
	EFI_STATUS (*GetInfo)(EFI_RNG_PROTOCOL *This, UINTN *RNGAlgorithmListSize, EFI_GUID *RNGAlgorithmList);
	EFI_STATUS (*GetRNG)(EFI_RNG_PROTOCOL *This, EFI_GUID *RNGAlgorithm, UINTN RNGValueLength, UINT8 *RNGValue);
};

#define EFI_ACPI_20_TABLE_GUID \
	{ 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } }

#define EFI_PAGE_SIZE   4096ULL
#define EFI_PAGE_SHIFT  12
#define EFI_SIZE_TO_PAGES(s) (((s) + EFI_PAGE_SIZE - 1) >> EFI_PAGE_SHIFT)

#endif /* RAVYN_BOOTER_EFI_H */
