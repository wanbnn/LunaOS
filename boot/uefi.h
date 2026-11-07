#ifndef LUNA_EFI_H
#define LUNA_EFI_H
#include <stdint.h>
#include <stddef.h>
typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint16_t CHAR16;
typedef struct {uint32_t a;uint16_t b,c;uint8_t d[8];} EFI_GUID;
#define EFI_ERROR(x) ((x)>>63)
#define EFI_SUCCESS 0
#define EFI_NOT_FOUND (0x800000000000000eull)
typedef struct {uint64_t signature;uint32_t revision,size,crc,reserved;} EFI_HEADER;
typedef struct {
 EFI_HEADER hdr;
 void *RaiseTPL,*RestoreTPL;
 EFI_STATUS (*AllocatePages)(uint32_t,uint32_t,uint64_t,uint64_t *);
 EFI_STATUS (*FreePages)(uint64_t,uint64_t);
 EFI_STATUS (*GetMemoryMap)(uint64_t *,void *,uint64_t *,uint64_t *,uint32_t *);
 EFI_STATUS (*AllocatePool)(uint32_t,uint64_t,void **);
 EFI_STATUS (*FreePool)(void *);
 void *CreateEvent,*SetTimer,*WaitForEvent,*SignalEvent,*CloseEvent,*CheckEvent;
 void *InstallProtocolInterface,*ReinstallProtocolInterface,*UninstallProtocolInterface;
 EFI_STATUS (*HandleProtocol)(EFI_HANDLE,EFI_GUID *,void **);
 void *Reserved,*RegisterProtocolNotify,*LocateHandle,*LocateDevicePath,*InstallConfigurationTable;
 void *LoadImage,*StartImage,*Exit,*UnloadImage;
 EFI_STATUS (*ExitBootServices)(EFI_HANDLE,uint64_t);
 void *GetNextMonotonicCount,*Stall;
 EFI_STATUS (*SetWatchdogTimer)(uint64_t,uint64_t,uint64_t,CHAR16 *);
 void *ConnectController,*DisconnectController,*OpenProtocol,*CloseProtocol,*OpenProtocolInformation;
 void *ProtocolsPerHandle;
 EFI_STATUS (*LocateHandleBuffer)(uint32_t,EFI_GUID *,void *,uint64_t *,EFI_HANDLE **);
 EFI_STATUS (*LocateProtocol)(EFI_GUID *,void *,void **);
 void *InstallMultipleProtocolInterfaces,*UninstallMultipleProtocolInterfaces;
 void *CalculateCrc32,*CopyMem,*SetMem,*CreateEventEx;
} EFI_BOOT_SERVICES;
typedef struct {void *Reset;EFI_STATUS (*OutputString)(void *,CHAR16 *);void *TestString,*QueryMode,*SetMode,*SetAttribute,*ClearScreen,*SetCursorPosition,*EnableCursor,*Mode;} EFI_TEXT;
typedef struct {
 EFI_HEADER hdr;CHAR16 *Vendor;uint32_t Revision;EFI_HANDLE ConsoleInHandle;void *ConIn;
 EFI_HANDLE ConsoleOutHandle;EFI_TEXT *ConOut;EFI_HANDLE StdErrHandle;void *StdErr;
 void *Runtime;EFI_BOOT_SERVICES *Boot;uint64_t Entries;void *Configuration;
} EFI_SYSTEM_TABLE;
typedef struct {
 uint32_t Revision;EFI_HANDLE Parent;EFI_SYSTEM_TABLE *SystemTable;EFI_HANDLE Device;
 void *FilePath,*Reserved;uint32_t LoadOptionsSize;void *LoadOptions,*ImageBase;
 uint64_t ImageSize;uint32_t ImageCodeType,ImageDataType;void *Unload;
} EFI_LOADED_IMAGE;
typedef struct EFI_FILE EFI_FILE;
struct EFI_FILE {
 uint64_t Revision;
 EFI_STATUS (*Open)(EFI_FILE *,EFI_FILE **,CHAR16 *,uint64_t,uint64_t);
 EFI_STATUS (*Close)(EFI_FILE *);
 void *Delete;
 EFI_STATUS (*Read)(EFI_FILE *,uint64_t *,void *);
 void *Write,*GetPosition,*SetPosition,*GetInfo,*SetInfo,*Flush;
};
typedef struct {uint64_t Revision;EFI_STATUS (*OpenVolume)(void *,EFI_FILE **);} EFI_FS;
typedef struct {
 uint32_t MediaId;uint8_t Removable,Present,Logical,ReadOnly,WriteCaching;
 uint32_t BlockSize,IoAlign;uint64_t LastBlock;
} EFI_MEDIA;
typedef struct EFI_BLOCK_IO EFI_BLOCK_IO;
struct EFI_BLOCK_IO {
 uint64_t Revision;EFI_MEDIA *Media;void *Reset;
 EFI_STATUS (*ReadBlocks)(EFI_BLOCK_IO *,uint32_t,uint64_t,uint64_t,void *);
 void *WriteBlocks,*FlushBlocks;
};
typedef struct {uint32_t Version,Width,Height,Format;uint32_t Red,Green,Blue,Reserved,Pitch;} EFI_GOP_INFO;
typedef struct {uint32_t MaxMode,Mode;EFI_GOP_INFO *Info;uint64_t InfoSize,Base,Size;} EFI_GOP_MODE;
typedef struct EFI_GOP EFI_GOP;
struct EFI_GOP {
 EFI_STATUS (*QueryMode)(EFI_GOP *,uint32_t,uint64_t *,EFI_GOP_INFO **);
 EFI_STATUS (*SetMode)(EFI_GOP *,uint32_t);
 void *Blt;EFI_GOP_MODE *Mode;
};
typedef struct {uint32_t Type,Pad;uint64_t Physical,Virtual,Pages,Attribute;} EFI_MEMORY;
typedef struct {uint8_t Type,SubType;uint16_t Length;} EFI_DEVICE_PATH;
#endif
