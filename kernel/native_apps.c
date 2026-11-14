/* Immutable, separately executed application payloads. These mounts do not
 * consume the writable LunaFS snapshot or require a server at runtime. */
#include "luna.h"
#include "heap.h"
#ifdef LUNA_NATIVE_GIT
extern const unsigned char _binary_build_git_native_payload_start[], _binary_build_git_native_payload_end[];
extern const unsigned char _binary_build_git_native_sh_payload_start[], _binary_build_git_native_sh_payload_end[];
extern int uncompress(unsigned char *, unsigned long *, const unsigned char *, unsigned long);
static struct { unsigned char *data; uint32_t size; } images[2];
static bool unpack(unsigned id,const unsigned char *p,const unsigned char *end) {
    if(images[id].data)return true;
    size_t packed=end-p;
    if(packed<5)return false;
    uint32_t bytes=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
    if(!bytes || bytes>16u*1024u*1024u)return false;
    unsigned char *data=heap_allocate_owned(bytes+1u,1);
    if(!data)return false;
    unsigned long size=bytes;
    if(uncompress(data,&size,p+4,packed-4) || size!=bytes) {heap_release(data);return false;}
    data[bytes]=0;images[id].data=data;images[id].size=bytes;return true;
}
void native_apps_mount(void) {
    if(!unpack(0,_binary_build_git_native_payload_start,_binary_build_git_native_payload_end) ||
       !unpack(1,_binary_build_git_native_sh_payload_start,_binary_build_git_native_sh_payload_end)) {
        serial_write("Native application payload could not be unpacked\n");return;
    }
    bool dirty=fs_dirty;
    static const char *directories[]={"/bin","/usr","/usr/libexec","/usr/libexec/git-core",
        "/usr/share","/usr/share/git-core","/usr/share/git-core/templates"};
    for(unsigned i=0;i<sizeof(directories)/sizeof(*directories);i++)
        if(!fs_find(directories[i]))fs_create(directories[i],true);
    /* Constrain background process peaks on the 64 MiB profile. Manual
     * maintenance stays available; users can override the normal Git config. */
    if(!fs_find("/etc/gitconfig"))fs_write("/etc/gitconfig","[maintenance]\n\tauto = false\n",false);
    static const char *git_paths[]={"/bin/git","/usr/libexec/git-core/git-upload-pack",
        "/usr/libexec/git-core/git-receive-pack","/usr/libexec/git-core/git-upload-archive",
        "/usr/libexec/git-core/git-index-pack","/usr/libexec/git-core/git-pack-objects"};
    for(unsigned i=0;i<sizeof(git_paths)/sizeof(*git_paths);i++)
        if(fs_mount_readonly(git_paths[i],images[0].data,images[0].size,0755u))
            serial_write("Native Git mount failed\n");
    if(fs_mount_readonly("/bin/sh",images[1].data,images[1].size,0755u))
        serial_write("Native shell mount failed\n");
    fs_dirty=dirty;
}
#endif
