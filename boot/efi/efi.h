/* efi.h - the part of the UEFI specification lpboot.c uses, written out.
 *
 * gnu-efi and EDK2 are the usual way to get these, and neither is on the
 * build host or worth making a dependency: a boot menu needs a dozen
 * tables and a handful of GUIDs, and writing them down here means the
 * build is clang and lld-link and nothing else. Field order is the whole
 * contract with the firmware, so every table is complete up to the last
 * member lpboot.c touches (UEFI 2.10, chapters 4, 7, 8, 9, 12, 13).
 *
 * The target is x86_64-unknown-windows, whose default calling
 * convention is the Microsoft x64 one UEFI requires, so the function
 * pointers need no EFIAPI annotation. CHAR16 is wchar_t under
 * -fshort-wchar, which lets L"..." literals be UEFI strings.
 */
#ifndef LP_EFI_H
#define LP_EFI_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        s8;
typedef short              s16;
typedef int                s32;
typedef long long          s64;
typedef _Bool              bool;
#define true  1
#define false 0

typedef u64            UINTN;
typedef u64            EFI_STATUS;
typedef void          *EFI_HANDLE;
typedef void          *EFI_EVENT;
typedef unsigned short CHAR16;
typedef u8             BOOLEAN;

typedef struct { u32 d1; u16 d2, d3; u8 d4[8]; } EFI_GUID;

#define EFI_ERR(n)            (0x8000000000000000ull | (n))
#define EFI_SUCCESS           0
#define EFI_LOAD_ERROR        EFI_ERR(1)
#define EFI_INVALID_PARAMETER EFI_ERR(2)
#define EFI_UNSUPPORTED       EFI_ERR(3)
#define EFI_BUFFER_TOO_SMALL  EFI_ERR(5)
#define EFI_NOT_READY         EFI_ERR(6)
#define EFI_DEVICE_ERROR      EFI_ERR(7)
#define EFI_OUT_OF_RESOURCES  EFI_ERR(9)
#define EFI_NOT_FOUND         EFI_ERR(14)
#define EFI_ACCESS_DENIED     EFI_ERR(15)
#define EFI_SECURITY_VIOLATION EFI_ERR(26)

typedef struct {
    u64 Signature;
    u32 Revision, HeaderSize, CRC32, Reserved;
} EFI_TABLE_HEADER;

/* ── Console input ── */
typedef struct { u16 ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;
#define SCAN_UP     0x01
#define SCAN_DOWN   0x02
#define SCAN_RIGHT  0x03
#define SCAN_LEFT   0x04
#define SCAN_HOME   0x05
#define SCAN_END    0x06
#define SCAN_ESC    0x17

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (*Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (*ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, EFI_INPUT_KEY *);
    EFI_EVENT  WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (*Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (*OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
    void *TestString, *QueryMode, *SetMode, *SetAttribute;
    EFI_STATUS (*ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);
    void *SetCursorPosition;
    EFI_STATUS (*EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

/* ── Runtime services (variables) ── */
#define EFI_VARIABLE_NON_VOLATILE        0x1
#define EFI_VARIABLE_BOOTSERVICE_ACCESS  0x2
#define EFI_VARIABLE_RUNTIME_ACCESS      0x4

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime, *SetTime, *GetWakeupTime, *SetWakeupTime;
    void *SetVirtualAddressMap, *ConvertPointer;
    EFI_STATUS (*GetVariable)(CHAR16 *, EFI_GUID *, u32 *, UINTN *, void *);
    void *GetNextVariableName;
    EFI_STATUS (*SetVariable)(CHAR16 *, EFI_GUID *, u32, UINTN, void *);
    void *GetNextHighMonotonicCount;
    void (*ResetSystem)(int, EFI_STATUS, UINTN, void *);
} EFI_RUNTIME_SERVICES;

/* ── Boot services ── */
#define EVT_TIMER          0x80000000u
#define TPL_APPLICATION    4
typedef enum { TimerCancel, TimerPeriodic, TimerRelative } EFI_TIMER_DELAY;
typedef enum { AllHandles, ByRegisterNotify, ByProtocol } EFI_LOCATE_SEARCH_TYPE;
typedef enum { EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData } EFI_MEMORY_TYPE;

typedef struct EFI_DEVICE_PATH_PROTOCOL {
    u8 Type, SubType, Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL;
    void *AllocatePages, *FreePages, *GetMemoryMap;
    EFI_STATUS (*AllocatePool)(EFI_MEMORY_TYPE, UINTN, void **);
    EFI_STATUS (*FreePool)(void *);
    EFI_STATUS (*CreateEvent)(u32, UINTN, void *, void *, EFI_EVENT *);
    EFI_STATUS (*SetTimer)(EFI_EVENT, EFI_TIMER_DELAY, u64);
    EFI_STATUS (*WaitForEvent)(UINTN, EFI_EVENT *, UINTN *);
    void *SignalEvent;
    EFI_STATUS (*CloseEvent)(EFI_EVENT);
    EFI_STATUS (*CheckEvent)(EFI_EVENT);
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath;
    void *InstallConfigurationTable;
    EFI_STATUS (*LoadImage)(BOOLEAN, EFI_HANDLE, EFI_DEVICE_PATH_PROTOCOL *, void *, UINTN, EFI_HANDLE *);
    EFI_STATUS (*StartImage)(EFI_HANDLE, UINTN *, CHAR16 **);
    void *Exit;
    EFI_STATUS (*UnloadImage)(EFI_HANDLE);
    void *ExitBootServices, *GetNextMonotonicCount;
    EFI_STATUS (*Stall)(UINTN);
    EFI_STATUS (*SetWatchdogTimer)(UINTN, u64, UINTN, CHAR16 *);
    void *ConnectController, *DisconnectController;
    void *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    EFI_STATUS (*LocateHandleBuffer)(EFI_LOCATE_SEARCH_TYPE, EFI_GUID *, void *, UINTN *, EFI_HANDLE **);
    EFI_STATUS (*LocateProtocol)(EFI_GUID *, void *, void **);
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    u32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
} EFI_SYSTEM_TABLE;

/* ── Loaded image ── */
typedef struct {
    u32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    EFI_DEVICE_PATH_PROTOCOL *FilePath;
    void *Reserved;
    u32 LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    u64 ImageSize;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ── Graphics output ── */
typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    u32 Version, HorizontalResolution, VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    u32 RedMask, GreenMask, BlueMask, ReservedMask;
    u32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    u32 MaxMode, Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN SizeOfInfo;
    u64 FrameBufferBase;
    UINTN FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef enum { EfiBltVideoFill, EfiBltVideoToBltBuffer, EfiBltBufferToVideo, EfiBltVideoToVideo } EFI_BLT_OP;

typedef struct EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_STATUS (*QueryMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, u32, UINTN *,
                            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **);
    EFI_STATUS (*SetMode)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, u32);
    EFI_STATUS (*Blt)(struct EFI_GRAPHICS_OUTPUT_PROTOCOL *, void *, EFI_BLT_OP,
                      UINTN, UINTN, UINTN, UINTN, UINTN, UINTN, UINTN);
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef struct { u32 SizeOfEdid; u8 *Edid; } EFI_EDID_ACTIVE_PROTOCOL;

/* ── Pointers: a mouse (relative) and a touch screen or tablet (absolute) ── */
typedef struct {
    s32 RelativeMovementX, RelativeMovementY, RelativeMovementZ;
    BOOLEAN LeftButton, RightButton;
} EFI_SIMPLE_POINTER_STATE;
typedef struct {
    u64 ResolutionX, ResolutionY, ResolutionZ;
    BOOLEAN LeftButton, RightButton;
} EFI_SIMPLE_POINTER_MODE;
typedef struct EFI_SIMPLE_POINTER_PROTOCOL {
    EFI_STATUS (*Reset)(struct EFI_SIMPLE_POINTER_PROTOCOL *, BOOLEAN);
    EFI_STATUS (*GetState)(struct EFI_SIMPLE_POINTER_PROTOCOL *, EFI_SIMPLE_POINTER_STATE *);
    EFI_EVENT WaitForInput;
    EFI_SIMPLE_POINTER_MODE *Mode;
} EFI_SIMPLE_POINTER_PROTOCOL;

typedef struct {
    u64 CurrentX, CurrentY, CurrentZ;
    u32 ActiveButtons;
} EFI_ABSOLUTE_POINTER_STATE;
#define EFI_ABSP_TouchActive 0x1
typedef struct {
    u64 AbsoluteMinX, AbsoluteMinY, AbsoluteMinZ;
    u64 AbsoluteMaxX, AbsoluteMaxY, AbsoluteMaxZ;
    u32 Attributes;
} EFI_ABSOLUTE_POINTER_MODE;
typedef struct EFI_ABSOLUTE_POINTER_PROTOCOL {
    EFI_STATUS (*Reset)(struct EFI_ABSOLUTE_POINTER_PROTOCOL *, BOOLEAN);
    EFI_STATUS (*GetState)(struct EFI_ABSOLUTE_POINTER_PROTOCOL *, EFI_ABSOLUTE_POINTER_STATE *);
    EFI_EVENT WaitForInput;
    EFI_ABSOLUTE_POINTER_MODE *Mode;
} EFI_ABSOLUTE_POINTER_PROTOCOL;

/* ── Files ── */
#define EFI_FILE_MODE_READ 0x1ull
typedef struct EFI_FILE_PROTOCOL {
    u64 Revision;
    EFI_STATUS (*Open)(struct EFI_FILE_PROTOCOL *, struct EFI_FILE_PROTOCOL **, CHAR16 *, u64, u64);
    EFI_STATUS (*Close)(struct EFI_FILE_PROTOCOL *);
    void *Delete;
    EFI_STATUS (*Read)(struct EFI_FILE_PROTOCOL *, UINTN *, void *);
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    u64 Revision;
    EFI_STATUS (*OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* ── GUIDs ── */
#define EFI_LOADED_IMAGE_GUID    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }
#define EFI_GOP_GUID             { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } }
#define EFI_EDID_ACTIVE_GUID     { 0xbd8c1056, 0x9f36, 0x44ec, { 0x92, 0xa8, 0xa6, 0x33, 0x7f, 0x81, 0x79, 0x86 } }
#define EFI_SIMPLE_FS_GUID       { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_DEVICE_PATH_GUID     { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }
#define EFI_SIMPLE_POINTER_GUID  { 0x31878c87, 0x0b75, 0x11d5, { 0x9a, 0x4f, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } }
#define EFI_ABSOLUTE_POINTER_GUID { 0x8D59D32B, 0xC655, 0x4AE9, { 0x9B, 0x15, 0xF2, 0x59, 0x04, 0x99, 0x2A, 0x43 } }

#endif
