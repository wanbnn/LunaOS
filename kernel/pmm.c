#include "pmm.h"
#include <string.h>
#define EINVAL (-22)
#define ENOMEM (-12)
#define EBUSY (-16)
#define TAIL UINT32_MAX
static bool enabled(const PmmRange *r) { return (r->attributes & 1) && r->length; }
static bool valid_map(const PmmRange *map, unsigned count) {
    if (!map || !count || count > PMM_MAP_MAX) return false;
    for (unsigned i=0; i<count; i++)
        if (enabled(map+i) && map[i].length > UINT64_MAX-map[i].base) return false;
    return true;
}
static uint64_t range_end(const PmmRange *r) { return r->base+r->length; }
static uint32_t span_pages(const PmmRange *map, unsigned count) {
    uint64_t end=0;
    for (unsigned i=0; i<count; i++) if (enabled(map+i) && map[i].type==1) {
        uint64_t top=range_end(map+i);
        if (top>PMM_PHYSICAL_END) top=PMM_PHYSICAL_END;
        if (map[i].base<top && top>end) end=top;
    }
    return end/PMM_PAGE;
}
size_t pmm_metadata_bytes(const PmmRange *map, unsigned count) {
    if (!valid_map(map,count)) return 0;
    uint32_t pages=span_pages(map,count);
    return pages ? ((pages+7u)/8u+3u)/4u*4u+(size_t)pages*8u : 0;
}
static bool overlap(uint64_t start,uint64_t end,uint64_t base,uint64_t bytes) {
    return bytes && base<end && start<base+bytes;
}
int pmm_metadata_region(const PmmRange *map,unsigned count,const PmmReservation *reserved,
                         unsigned reservations,size_t bytes,uint32_t *out) {
    if (!out || !bytes || bytes>UINT32_MAX-PMM_PAGE || !valid_map(map,count) ||
        (reservations && !reserved) || reservations>PMM_MAP_MAX) return EINVAL;
    for (unsigned i=0; i<reservations; i++)
        if (reserved[i].length>UINT64_MAX-reserved[i].base) return EINVAL;
    uint64_t needed=((uint64_t)bytes+PMM_PAGE-1)&~(uint64_t)(PMM_PAGE-1), best=0;
    bool found=false;
    for (unsigned i=0; i<count; i++) {
        if (!enabled(map+i) || map[i].type!=1) continue;
        uint64_t low=map[i].base, top=range_end(map+i);
        if (low>=PMM_PHYSICAL_END) continue;
        if (top>PMM_PHYSICAL_END) top=PMM_PHYSICAL_END;
        top &= ~(uint64_t)(PMM_PAGE-1);
        while (top>=needed && top-needed>=low) {
            uint64_t candidate=top-needed, next=top;
            for (unsigned j=0; j<count; j++)
                if (enabled(map+j) && map[j].type!=1 &&
                    overlap(candidate,top,map[j].base,map[j].length) && map[j].base<next)
                    next=map[j].base;
            for (unsigned j=0; j<reservations; j++)
                if (overlap(candidate,top,reserved[j].base,reserved[j].length) && reserved[j].base<next)
                    next=reserved[j].base;
            if (next==top) {
                if (!found || candidate>best) { best=candidate; found=true; }
                break;
            }
            top=next&~(uint64_t)(PMM_PAGE-1);
        }
    }
    if (!found) return ENOMEM;
    *out=(uint32_t)best; return 0;
}
static bool is_free(const Pmm *p,uint32_t page) {
    return (p->free_bits[page/8]>>(page%8))&1;
}
static void set_free(Pmm *p,uint32_t page,bool value) {
    uint8_t mask=(uint8_t)(1u<<(page%8));
    if (value) p->free_bits[page/8]|=mask;
    else p->free_bits[page/8]&=(uint8_t)~mask;
}
static void remove_range(Pmm *p,uint64_t base,uint64_t length) {
    if (!length || base>=PMM_PHYSICAL_END) return;
    uint64_t end=base+length;
    if (end>PMM_PHYSICAL_END) end=PMM_PHYSICAL_END;
    uint32_t first=base/PMM_PAGE;
    uint64_t last=(end+PMM_PAGE-1)/PMM_PAGE;
    if (last>p->page_count) last=p->page_count;
    for (uint32_t i=first; i<last; i++) set_free(p,i,false);
}
/* Count the union of usable firmware ranges, with non-RAM overlaps taking
 * precedence. This includes RAM above 4 GiB in discovery, never in allocation. */
static uint64_t firmware_total(const PmmRange *map,unsigned count,uint64_t ceiling) {
    uint64_t bounds[PMM_MAP_MAX*2], total=0; unsigned n=0;
    for (unsigned i=0; i<count; i++) if (enabled(map+i)) {
        bounds[n++]=map[i].base; bounds[n++]=range_end(map+i);
    }
    for (unsigned i=1; i<n; i++) {
        uint64_t value=bounds[i]; unsigned j=i;
        while (j && bounds[j-1]>value) { bounds[j]=bounds[j-1]; j--; }
        bounds[j]=value;
    }
    for (unsigned i=1; i<n; i++) {
        uint64_t begin=bounds[i-1], end=bounds[i]; bool ram=false,banned=false;
        if (begin>=ceiling) continue;
        if (end>ceiling) end=ceiling;
        if (begin==end) continue;
        for (unsigned j=0; j<count; j++) if (enabled(map+j) && map[j].base<=begin && range_end(map+j)>=end) {
            if (map[j].type==1) ram=true; else banned=true;
        }
        if (ram && !banned) total+=end-begin;
    }
    return total;
}
int pmm_init(Pmm *p,const PmmRange *map,unsigned count,const PmmReservation *reserved,
               unsigned reservations,void *metadata,size_t bytes) {
    if (!p || p->ready) return p ? EBUSY : EINVAL;
    size_t needed=pmm_metadata_bytes(map,count);
    if (!needed || !metadata || ((uintptr_t)metadata&3) || bytes<needed ||
        (reservations && !reserved) || reservations>PMM_MAP_MAX) return EINVAL;
    for (unsigned i=0; i<reservations; i++)
        if (reserved[i].length>UINT64_MAX-reserved[i].base) return EINVAL;
    memset(p,0,sizeof(*p)); memset(metadata,0,needed);
    p->page_count=span_pages(map,count); p->free_bits=metadata;
    size_t bitmap=((p->page_count+7u)/8u+3u)/4u*4u;
    p->sizes=(uint32_t *)((uint8_t *)metadata+bitmap);
    p->cookies=p->sizes+p->page_count;
    for (unsigned j=0; j<count; j++) if (enabled(map+j) && map[j].type==1) {
        uint64_t first=map[j].base/PMM_PAGE+(map[j].base%PMM_PAGE!=0), last=range_end(map+j)/PMM_PAGE;
        if (last>p->page_count) last=p->page_count;
        for (uint64_t i=first; i<last; i++) set_free(p,(uint32_t)i,true);
    }
    for (unsigned j=0; j<count; j++)
        if (enabled(map+j) && map[j].type!=1) remove_range(p,map[j].base,map[j].length);
    for (uint32_t i=0; i<p->page_count; i++) if (is_free(p,i)) p->addressable_usable+=PMM_PAGE;
    for (unsigned j=0; j<reservations; j++) remove_range(p,reserved[j].base,reserved[j].length);
    for (uint32_t i=0; i<p->page_count; i++) if (is_free(p,i)) p->free_pages++;
    p->firmware_usable=firmware_total(map,count,UINT64_MAX);
    p->above_4g_usable=p->firmware_usable-firmware_total(map,count,PMM_PHYSICAL_END);
    p->ready=true; return 0;
}
static bool power2(uint32_t n) { return n && !(n&(n-1)); }
int pmm_allocate(Pmm *p,uint32_t bytes,uint32_t align,uint32_t mask,uint32_t boundary,
                  bool high,PmmAllocation *out) {
    if (!p || !out || !p->ready || !bytes || bytes==TAIL || !power2(align) ||
        (boundary && (!power2(boundary) || boundary<bytes))) return EINVAL;
    if (out->cookie || out->address || out->bytes) return EBUSY;
    uint32_t pages=((uint64_t)bytes+PMM_PAGE-1)/PMM_PAGE;
    if (pages>p->free_pages) return ENOMEM;
    if (align<PMM_PAGE) align=PMM_PAGE;
    uint64_t limit=(uint64_t)p->page_count*PMM_PAGE, dma_end=(uint64_t)mask+1;
    if (limit>dma_end) limit=dma_end;
    uint64_t span=(uint64_t)pages*PMM_PAGE;
    if (limit<span) return ENOMEM;
    uint64_t address=high ? (limit-span)&~(uint64_t)(align-1) : 0;
    for (;;) {
        uint64_t last=address+bytes-1;
        if (last>mask || address+span>limit) break;
        if (boundary && address/boundary!=last/boundary) {
            if (high) {
                uint64_t end=last/boundary*boundary;
                if (end<span) break;
                address=(end-span)&~(uint64_t)(align-1);
            } else address=(address/boundary+1)*boundary;
            continue;
        }
        uint32_t first=address/PMM_PAGE, n=0;
        while (n<pages && is_free(p,first+n)) n++;
        if (n==pages) {
            if (!++p->generation) ++p->generation;
            for (uint32_t i=0; i<pages; i++) {
                set_free(p,first+i,false); p->sizes[first+i]=i ? TAIL : bytes;
            }
            p->cookies[first]=p->generation; p->free_pages-=pages; p->allocation_count++;
            *out=(PmmAllocation){(uint32_t)address,bytes,p->generation}; return 0;
        }
        if (high) {
            uint64_t end=(uint64_t)(first+n)*PMM_PAGE;
            if (end<span) break;
            address=(end-span)&~(uint64_t)(align-1);
        } else address=((uint64_t)(first+n+1)*PMM_PAGE+align-1)&~(uint64_t)(align-1);
    }
    return ENOMEM;
}
int pmm_lookup(const Pmm *p,uint32_t address,PmmAllocation *out) {
    if (!p || !p->ready || !out || address%PMM_PAGE) return EINVAL;
    uint32_t first=address/PMM_PAGE;
    if (first>=p->page_count || !p->sizes[first] || p->sizes[first]==TAIL ||
        is_free(p,first) || !p->cookies[first]) return EINVAL;
    *out=(PmmAllocation){address,p->sizes[first],p->cookies[first]}; return 0;
}
int pmm_release(Pmm *p,const PmmAllocation *allocation) {
    if (!allocation || !allocation->cookie) return EINVAL;
    PmmAllocation actual;
    if (pmm_lookup(p,allocation->address,&actual) || actual.bytes!=allocation->bytes ||
        actual.cookie!=allocation->cookie) return EINVAL;
    uint32_t first=actual.address/PMM_PAGE, pages=((uint64_t)actual.bytes+PMM_PAGE-1)/PMM_PAGE;
    if (pages>p->page_count-first) return EINVAL;
    for (uint32_t i=1; i<pages; i++)
        if (p->sizes[first+i]!=TAIL || is_free(p,first+i)) return EINVAL;
    for (uint32_t i=0; i<pages; i++) {
        p->sizes[first+i]=p->cookies[first+i]=0; set_free(p,first+i,true);
    }
    p->free_pages+=pages; p->allocation_count--; return 0;
}
