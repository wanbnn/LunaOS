#include "acpi.h"
#include "storage.h"
#include <uacpi/uacpi.h>
#include <uacpi/resources.h>
#include <uacpi/utilities.h>

static char status_text[2048];
static bool battery_namespace;
static bool attempted;
static unsigned found;
void acpi_storage_status(char *out,size_t cap){append(out,status_text,cap);}
static void record(const char *s){append(status_text,s,sizeof(status_text));serial_write(s);}
static void record_error(const char *stage,uacpi_status rc) {
    record(stage);record(uacpi_status_to_string(rc));record("\n");
}
static uacpi_status optional_method(uacpi_namespace_node *node,const char *method) {
    uacpi_status rc=uacpi_execute_simple(node,method);
    return rc==UACPI_STATUS_NOT_FOUND?UACPI_STATUS_OK:rc;
}
static uacpi_status power_on(uacpi_namespace_node *node) {
    uacpi_object *resources=0;
    uacpi_status rc=uacpi_eval_simple(node,"_PR0",&resources);
    if(rc!=UACPI_STATUS_NOT_FOUND) {
        if(rc)return rc;
        uacpi_object_array array;
        rc=uacpi_object_get_package(resources,&array);
        for(unsigned i=0;!rc && i<array.count;i++) {
            uacpi_namespace_node *power=0;
            rc=uacpi_object_resolve_as_aml_namepath(array.objects[i],node,&power);
            if(!rc)rc=optional_method(power,"_ON");
        }
        uacpi_object_unref(resources);
        if(rc)return rc;
    }
    return optional_method(node,"_PS0");
}
typedef struct {uint64_t base,length;bool conflict;} MmioResource;
static uacpi_iteration_decision resource(void *user,uacpi_resource *r) {
    MmioResource *m=user;uint64_t base=0,len=0;
    if(r->type==UACPI_RESOURCE_TYPE_FIXED_MEMORY32) {
        base=r->fixed_memory32.address;len=r->fixed_memory32.length;
    } else if(r->type==UACPI_RESOURCE_TYPE_MEMORY32 && r->memory32.minimum==r->memory32.maximum) {
        base=r->memory32.minimum;len=r->memory32.length;
    } else if(r->type==UACPI_RESOURCE_TYPE_ADDRESS32 && r->address32.common.type==UACPI_RANGE_MEMORY) {
        base=(uint64_t)r->address32.minimum+r->address32.translation_offset;len=r->address32.address_length;
    } else if(r->type==UACPI_RESOURCE_TYPE_ADDRESS64 && r->address64.common.type==UACPI_RANGE_MEMORY) {
        base=r->address64.minimum+r->address64.translation_offset;len=r->address64.address_length;
    }
    if(len) {
        if(m->length)m->conflict=true; /* do not guess which aperture is SDHCI */
        else {m->base=base;m->length=len;}
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static uacpi_iteration_decision device(void *user,uacpi_namespace_node *node,uacpi_u32 depth) {
    (void)user;(void)depth;
    uacpi_id_string *hid=0,*uid=0;
    if(uacpi_eval_hid(node,&hid))return UACPI_ITERATION_DECISION_CONTINUE;
    (void)uacpi_eval_uid(node,&uid);
    bool mmc=!strcmp(hid->value,"80865ACC") || !strcmp(hid->value,"AMDI0040") ||
        !strcmp(hid->value,"AMDI0041");
    if(!strcmp(hid->value,"80860F14")) {
        /* UID 2 is SDIO (often Wi-Fi), UID 3 is removable SD. */
        mmc=uid && !strcmp(uid->value,"1");
        if(!mmc)goto done;
    }
    if(!strcmp(hid->value,"INT33BB"))goto done; /* SD/SDIO, not eMMC */
    found++;
    record("eMMC/SDHCI ACPI ");record(hid->value);record(": ");
    uacpi_status rc=optional_method(node,"_INI");
    if(!rc)rc=power_on(node);
    if(rc){record_error("power/initialization failed: ",rc);goto done;}
    MmioResource m={0};
    rc=uacpi_for_each_device_resource(node,"_CRS",resource,&m);
    if(rc){record_error("_CRS failed: ",rc);goto done;}
    if(m.conflict || m.base<0x100000 || m.length<0x100 || m.length>0x10000 ||
       m.base>UINT32_MAX || m.base+m.length>0x100000000ull) {
        record("unsupported or ambiguous MMIO resource\n");goto done;
    }
    char n[20];hex(n,(uint32_t)m.base);record(n);record(" ");
    int result=storage_sdhci_platform_probe((uint32_t)m.base,(uint32_t)m.length,mmc);
    if(result>0)record("driver ready\n");
    else if(result==DRV_EALREADY)record("already owned by PCI driver\n");
    else {
        char why[160];storage_sdhci_reason(why,sizeof(why));record(why);record("\n");
    }
done:
    uacpi_free_id_string(hid);if(uid)uacpi_free_id_string(uid);
    return UACPI_ITERATION_DECISION_CONTINUE;
}
void acpi_storage_init(void) {
    if(attempted)return;
    attempted=true;
    uacpi_status rc=uacpi_initialize(UACPI_FLAG_NO_ACPI_MODE|UACPI_FLAG_BAD_CSUM_FATAL);
    if(rc){record_error("ACPI storage: table initialization failed: ",rc);return;}
    rc=uacpi_namespace_load();
    if(rc){record_error("ACPI storage: AML namespace failed: ",rc);return;}
    battery_namespace = true;
    /* Evaluate only storage devices. No global _INI, _PIC, GPE/SCI takeover. */
    const char *const ids[]={"80860F14","80865ACC","AMDI0040","AMDI0041","PNP0D40",0};
    rc=uacpi_find_devices_at(uacpi_namespace_root(),ids,device,0);
    if(rc)record_error("ACPI storage: enumeration failed: ",rc);
    uacpi_status work=uacpi_kernel_wait_for_work_completion();
    if(work)record_error("ACPI storage: deferred work failed: ",work);
    if(!found)record("ACPI storage: no supported present SDHCI device in namespace\n");
}

/* Aggregate capacities only when firmware uses the same units for every pack. */
static uint64_t battery_full, battery_remaining;
static unsigned battery_units, battery_packs;
static bool battery_charging, battery_invalid;
static char battery_text[64];
static bool battery_package(uacpi_namespace_node *node, const char *method,
                            unsigned offset, uint64_t *values, unsigned count) {
    uacpi_object *object = 0;
    if (uacpi_eval_simple(node, method, &object)) return false;
    uacpi_object_array array;
    bool ok = !uacpi_object_get_package(object, &array) && array.count >= offset + count;
    for (unsigned i = 0; ok && i < count; i++) {
        uacpi_u64 v;
        ok = !uacpi_object_get_integer(array.objects[offset + i], &v) && v <= UINT32_MAX;
        if (ok) values[i] = v;
    }
    uacpi_object_unref(object);
    return ok;
}
static uacpi_iteration_decision battery_device(void *user, uacpi_namespace_node *node,
                                              uacpi_u32 depth) {
    (void)user; (void)depth;
    uint64_t info[3], state[3];
    /* _BIX adds a revision before the _BIF capacity fields. */
    if (!battery_package(node, "_BIX", 1, info, 3) &&
        !battery_package(node, "_BIF", 0, info, 3)) return UACPI_ITERATION_DECISION_CONTINUE;
    if (!battery_package(node, "_BST", 0, state, 3) || !info[2] || info[2] == UINT32_MAX || state[0] == UINT32_MAX ||
        state[2] == UINT32_MAX || info[0] > 1)
        return UACPI_ITERATION_DECISION_CONTINUE;
    if (battery_packs && battery_units != info[0]) battery_invalid = true;
    battery_units = info[0]; battery_packs++;
    battery_full += info[2]; battery_remaining += state[2];
    battery_charging |= (state[0] & 2) != 0;
    return UACPI_ITERATION_DECISION_CONTINUE;
}
void acpi_battery_poll(void) {
    static uint32_t next;
    if (!battery_namespace || (int32_t)(frame_clock - next) < 0) return;
    next = frame_clock + 10000;
    battery_full = battery_remaining = battery_packs = 0;
    battery_invalid = battery_charging = false;
    const char *const ids[] = {"PNP0C0A", 0};
    uacpi_find_devices_at(uacpi_namespace_root(), ids, battery_device, 0);
    battery_text[0] = 0;
    if (!battery_packs || battery_invalid || !battery_full) return;
    uint64_t percent = battery_remaining * 100 / battery_full;
    if (percent > 100) percent = 100;
    number(battery_text, (unsigned)percent);
    append(battery_text, battery_charging ? "% +" : "%", sizeof(battery_text));
}
void acpi_battery_text(char *out, size_t cap) {
    if (!cap) return;
    out[0] = 0;
    append(out, battery_text, cap);
}
