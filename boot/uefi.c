#include "uefi.h"
#include "uefi_trampoline.h"
#include "uefi_layout.h"
#include "../kernel/image_layout.h"
#define CORE_BYTES (32u*1024*1024)
#define ESP_BYTES (34u*1024*1024)
#define IMAGE_BYTES LUNA_IMAGE_BYTES
#define IMAGE_INFO_OFFSET (LUNA_IMAGE_INFO_SECTOR * 512u)
#define KERNEL_OFFSET (LUNA_KERNEL_FIRST_SECTOR * 512u)
#define LOW_END 0x4000000ull
static EFI_BOOT_SERVICES *bs;
static EFI_SYSTEM_TABLE *system_table;
static EFI_GUID loaded_guid={0x5b1b31a1,0x9562,0x11d2,{0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static void *volatile relocation_anchor=&loaded_guid;
static EFI_GUID fs_guid={0x964e5b22,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static EFI_GUID block_guid={0x964e5b21,0x6459,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static EFI_GUID path_guid={0x09576e91,0x6d3f,0x11d2,{0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static EFI_GUID gop_guid={0x9042a9de,0x23dc,0x4a38,{0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
void *memcpy(void *out,const void *in,size_t n) {uint8_t *d=out;const uint8_t *s=in;while(n--) *d++=*s++;return out;}
void *memset(void *out,int c,size_t n) {uint8_t *d=out;while(n--) *d++=c;return out;}
static int equal(const void *a,const void *b,size_t n) {const uint8_t *x=a,*y=b;while(n--) if(*x++!=*y++) return 0;return 1;}
static uint8_t inb(uint16_t port) {uint8_t v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(port));return v;}
static void outb(uint16_t port,uint8_t v) {__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(port));}
static void say(const char *s) {
 CHAR16 buffer[160];unsigned i=0;
 for(;s[i] && i<158;i++) {
  buffer[i]=(uint8_t)s[i];
  for(unsigned j=0;j<100000;j++) if(inb(0x3fd)&32) {outb(0x3f8,s[i]);break;}
 }
 buffer[i]=0;if(system_table->ConOut) system_table->ConOut->OutputString(system_table->ConOut,buffer);
}
static EFI_STATUS error(const char *s) {say("Luna UEFI ERROR: ");say(s);say("\r\n");return 0x8000000000000001ull;}
static void *high_alloc(uint64_t bytes) {
 uint64_t address=0xffffffff, pages=(bytes+4095)/4096;
 if(EFI_ERROR(bs->AllocatePages(1,2,pages,&address))) return 0;
 if(address<LOW_END) {bs->FreePages(address,pages);return 0;}
 memset((void *)(uintptr_t)address,0,pages*4096);return (void *)(uintptr_t)address;
}
static unsigned path_size(EFI_DEVICE_PATH *p) {
 if(!p) return 0;unsigned n=0;
 while(n<4096) {
  EFI_DEVICE_PATH *node=(void *)((uint8_t *)p+n);
  if(node->Type==0x7f) return n;
  if(node->Length<4 || node->Length>4096-n) return 0;
  n+=node->Length;
 }
 return 0;
}
static int read_blocks(EFI_BLOCK_IO *io,uint64_t offset,uint64_t bytes,void *out) {
 if(!io || !io->Media || !io->Media->Present || !io->Media->BlockSize) return 0;
 uint64_t block=io->Media->BlockSize;
 if(offset%block || bytes%block || offset/block>io->Media->LastBlock ||
    bytes/block>io->Media->LastBlock+1-offset/block) return 0;
 return !EFI_ERROR(io->ReadBlocks(io,io->Media->MediaId,offset/block,bytes,out));
}
static int valid_core(uint8_t *image) {
 uint32_t *meta=(void *)(image+IMAGE_INFO_OFFSET);
 return image[510]==0x55 && image[511]==0xaa && equal(meta,"LUNAIMG1",8) &&
     meta[2]==1 && meta[3]==IMAGE_BYTES && meta[6]>0 && meta[6]<=IMAGE_INFO_OFFSET-KERNEL_OFFSET;
}
/* Prefer the exact parent medium; fallback CORE is for virtual removable media. */
static int load_parent_core(EFI_HANDLE volume,uint8_t *rom,uint8_t *scratch) {
 EFI_DEVICE_PATH *child=0;EFI_HANDLE *handles=0;uint64_t count=0;
 if(EFI_ERROR(bs->HandleProtocol(volume,&path_guid,(void **)&child))) return 0;
 unsigned child_len=path_size(child),best=0;EFI_BLOCK_IO *chosen=0;uint64_t chosen_offset=0;
 if(EFI_ERROR(bs->LocateHandleBuffer(2,&block_guid,0,&count,&handles))) return 0;
 for(uint64_t i=0;i<count;i++) {
  EFI_BLOCK_IO *io=0;EFI_DEVICE_PATH *path=0;
  if(handles[i]==volume || EFI_ERROR(bs->HandleProtocol(handles[i],&block_guid,(void **)&io)) ||
      EFI_ERROR(bs->HandleProtocol(handles[i],&path_guid,(void **)&path))) continue;
  unsigned n=path_size(path);
  if(!n || n>child_len || n<best || !equal(path,child,n) || !io->Media->Present ||
      (io->Media->BlockSize!=512 && io->Media->BlockSize!=2048) || io->Media->IoAlign>4096) continue;
  uint64_t offset=0;
  if(!read_blocks(io,0,4096,scratch)) continue;
  if(scratch[510]!=0x55 || scratch[511]!=0xaa) {
   offset=64u*2048;
   if(!read_blocks(io,offset,4096,scratch) || scratch[510]!=0x55 || scratch[511]!=0xaa) continue;
  }
  uint32_t block=io->Media->BlockSize;
  uint32_t within_block=IMAGE_INFO_OFFSET%block;
  if(!read_blocks(io,offset+IMAGE_INFO_OFFSET-within_block,block,scratch)) continue;
  uint8_t *meta=scratch+within_block;
  if(!equal(meta,"LUNAIMG1",8) || *(uint32_t *)(meta+12)!=IMAGE_BYTES) continue;
  chosen=io;best=n;chosen_offset=offset;
 }
 int ok=chosen && read_blocks(chosen,chosen_offset,CORE_BYTES,rom) && valid_core(rom);
 bs->FreePool(handles);return ok;
}
static void hex(uint64_t value) {char s[19]="0x0000000000000000";for(unsigned i=0;i<16;i++) s[17-i]="0123456789abcdef"[(value>>(i*4))&15];say(s);}
static unsigned color_pos(uint32_t mask) {unsigned n=0;while(mask && !(mask&1)) {mask>>=1;n++;}return mask==255?n:255;}
static int setup_gop(uint8_t *handoff) {
 EFI_GOP *gop=0;if(EFI_ERROR(bs->LocateProtocol(&gop_guid,0,(void **)&gop))) return 0;
 uint32_t selected=0xffffffff,best_area=0xffffffff;
 for(uint32_t i=0;i<gop->Mode->MaxMode;i++) {
  EFI_GOP_INFO *info=0;uint64_t size=0;
  if(EFI_ERROR(gop->QueryMode(gop,i,&size,&info))) continue;
  uint32_t area=info->Width*info->Height;
  if(info->Width>=1024 && info->Height>=720 && info->Width<=1920 && info->Height<=1080 &&
      info->Format!=3 && area<best_area) {selected=i;best_area=area;}
  if(info->Width==1024 && info->Height==768 && info->Format!=3) {selected=i;best_area=0;}
  bs->FreePool(info);
 }
 if(selected==0xffffffff || EFI_ERROR(gop->SetMode(gop,selected))) return 0;
 EFI_GOP_INFO *info=gop->Mode->Info;
 if(!gop->Mode->Base || gop->Mode->Base>0xffffffffull || gop->Mode->Size<(uint64_t)info->Pitch*info->Height*4) return 0;
 unsigned r=16,g=8,b=0;
 if(info->Format==0) {r=0;b=16;}
 else if(info->Format==2) {r=color_pos(info->Red);g=color_pos(info->Green);b=color_pos(info->Blue);}
 if(r>24 || g>24 || b>24) return 0;
 uint8_t *v=handoff+0x1000;
 *(uint16_t *)(v+16)=info->Pitch*4;*(uint16_t *)(v+18)=info->Width;*(uint16_t *)(v+20)=info->Height;
 v[25]=32;v[32]=r;v[34]=g;v[36]=b;*(uint32_t *)(v+40)=gop->Mode->Base;
 return 1;
}
static uint32_t memory_type(uint32_t type) {
 if(type==1 || type==2 || type==3 || type==4 || type==7) return 1;
 if(type==9) return 3;if(type==10) return 4;return 2;
}
typedef struct {uint64_t base,length;uint32_t type,attr;} Region;
static Region regions[256];
static int memory_handoff(uint8_t *buffer,uint64_t size,uint64_t stride,uint8_t *out) {
 unsigned count=0;
 if(stride<sizeof(EFI_MEMORY)) return 0;
 for(uint64_t off=0;off+sizeof(EFI_MEMORY)<=size;off+=stride) {
  EFI_MEMORY *d=(void *)(buffer+off);
  if(!d->Pages) continue;
  uint32_t type=memory_type(d->Type);
  uint64_t end=d->Physical+d->Pages*4096;
  if(((d->Physical<LUNA_LOW_END && end>0x100000) || (d->Physical<LUNA_BSS_END && end>LUNA_HIGH_START) || (d->Physical<LOW_END && end>0x3d00000)) && (type!=1 || (d->Attribute>>63))) {say("Reserved low region: ");hex(d->Physical);say(" type ");hex(d->Type);say(" end ");hex(end);say("\r\n");return 0;}
  if(count==256) return 0;
  regions[count++]=(Region){d->Physical,d->Pages*4096,type,1};
 }
 for(unsigned i=1;i<count;i++) {Region item=regions[i];unsigned j=i;while(j && regions[j-1].base>item.base) {regions[j]=regions[j-1];j--;}regions[j]=item;}
 unsigned used=0;
 for(unsigned i=0;i<count;i++) {
  if(used && regions[used-1].type==regions[i].type && regions[used-1].base+regions[used-1].length==regions[i].base)
   regions[used-1].length+=regions[i].length;
  else regions[used++]=regions[i];
 }
 if(used>128) return 0;
 *(uint32_t *)out=used;memcpy(out+4,regions,used*sizeof(Region));return 1;
}
/* ACPI table addresses survive ExitBootServices in ACPI reclaim/NVS memory. */
static uint64_t find_rsdp(EFI_SYSTEM_TABLE *table) {
 const EFI_GUID acpi2={0x8868e871,0xe4f1,0x11d3,{0xbc,0x22,0x00,0x80,0xc7,0x3c,0x88,0x81}};
 const EFI_GUID acpi1={0xeb9d2d30,0x2d88,0x11d3,{0x9a,0x16,0x00,0x90,0x27,0x3f,0xc1,0x4d}};
 typedef struct {EFI_GUID guid;void *address;} Entry;
 Entry *entries=table->Configuration;uint64_t legacy=0;
 for(uint64_t i=0;i<table->Entries && i<4096;i++) {
  if(equal(&entries[i].guid,&acpi2,sizeof(acpi2)))return (uintptr_t)entries[i].address;
  if(equal(&entries[i].guid,&acpi1,sizeof(acpi1)))legacy=(uintptr_t)entries[i].address;
 }
 return legacy;
}
EFI_STATUS efi_main(EFI_HANDLE image,EFI_SYSTEM_TABLE *table) {
 system_table=table;bs=table->Boot;
 if(!relocation_anchor) return EFI_NOT_FOUND;
 outb(0x3f9,0);outb(0x3fb,0x80);outb(0x3f8,1);outb(0x3f9,0);outb(0x3fb,3);outb(0x3fc,0x0b);
 say("Luna UEFI x64 loader v1\r\n");bs->SetWatchdogTimer(0,0,0,0);
 uint64_t cr4;__asm__ volatile("mov %%cr4,%0":"=r"(cr4));
 if(cr4&(1u<<12)) return error("5-level paging is not supported by this bridge");
 EFI_LOADED_IMAGE *loaded=0;EFI_FS *fs=0;EFI_FILE *root=0,*file=0;EFI_BLOCK_IO *volume=0;
 if(EFI_ERROR(bs->HandleProtocol(image,&loaded_guid,(void **)&loaded)) ||
    EFI_ERROR(bs->HandleProtocol(loaded->Device,&fs_guid,(void **)&fs)) ||
    EFI_ERROR(fs->OpenVolume(fs,&root))) return error("boot filesystem unavailable");
 uint8_t *rom=high_alloc(IMAGE_BYTES),*scratch=high_alloc(4096),*prefix=high_alloc(8192);
 uint8_t *map=high_alloc(65536),*bootstrap=high_alloc(65536);
 if(!rom || !scratch || !prefix || !map || !bootstrap) return error("requires at least 256 MiB RAM for UEFI image handoff");
 CHAR16 core_name[]={'\\','L','U','N','A','C','O','R','E','.','B','I','N',0};
 if(EFI_ERROR(root->Open(root,&file,core_name,1,0))) return error("LUNACORE.BIN missing");
 uint64_t n=CORE_BYTES;
 EFI_STATUS status=file->Read(file,&n,rom);file->Close(file);root->Close(root);
 if(EFI_ERROR(status) || n!=CORE_BYTES || !valid_core(rom)) return error("invalid core image");
 say(load_parent_core(loaded->Device,rom,scratch)?"UEFI parent image identified\r\n":"UEFI virtual-media core fallback\r\n");
 if(EFI_ERROR(bs->HandleProtocol(loaded->Device,&block_guid,(void **)&volume)) ||
    !read_blocks(volume,0,ESP_BYTES,rom+CORE_BYTES)) return error("cannot preserve EFI system partition");
 if(!setup_gop(prefix)) return error("no supported GOP framebuffer below 4 GiB");
 prefix[0x1f00]=0x80;memcpy(prefix+0x1f04,rom+440,4);memcpy(prefix+0x1f08,rom+444,2);memcpy(prefix+0x1f0c,rom+432,4);
 uint32_t *boot=(void *)(prefix+0x1f10);boot[0]=0x55454649;boot[1]=1;boot[2]=(uintptr_t)rom;boot[3]=IMAGE_BYTES;
 uint64_t rsdp=find_rsdp(table);boot[4]=(uint32_t)rsdp;boot[5]=(uint32_t)(rsdp>>32);
 uint64_t *pml4=(void *)(bootstrap+8192),*pdpt=(void *)(bootstrap+12288);
 pml4[0]=(uintptr_t)pdpt|3;
 for(unsigned i=0;i<4;i++) {
  uint64_t *pd=(void *)(bootstrap+16384+i*4096);pdpt[i]=(uintptr_t)pd|3;
  for(unsigned j=0;j<512;j++) pd[j]=((uint64_t)i<<30)|((uint64_t)j<<21)|0x83;
 }
 memcpy(bootstrap,uefi_trampoline,sizeof(uefi_trampoline));
 uint64_t *args=(void *)(bootstrap+4096);
 args[0]=(uintptr_t)(rom+KERNEL_OFFSET);args[1]=*(uint32_t *)(rom+IMAGE_INFO_OFFSET+24);
 args[2]=(uintptr_t)(bootstrap+65536-16);args[3]=(uintptr_t)pml4;args[4]=(uintptr_t)prefix;
 say("UEFI GOP ready; exiting firmware services\r\n");
 for(unsigned attempt=0;attempt<4;attempt++) {
  uint64_t map_size=65536,key,stride;uint32_t version;
  status=bs->GetMemoryMap(&map_size,map,&key,&stride,&version);
  if(EFI_ERROR(status) || !memory_handoff(map,map_size,stride,prefix)) return error("unsafe low-memory layout");
  status=bs->ExitBootServices(image,key);
  if(!EFI_ERROR(status)) {
   typedef void (*Transfer)(void *);
   ((Transfer)(uintptr_t)bootstrap)(args);
   for(;;) __asm__ volatile("cli; hlt");
  }
 }
 return error("ExitBootServices failed");
}
