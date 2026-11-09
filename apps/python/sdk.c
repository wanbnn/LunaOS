#include "luna.h"
#include "devkit.h"
#include "lunasdk_hardware.h"
#include <stdio.h>
#include "py/runtime.h"
#include "py/mperrno.h"

static unsigned nonnegative(mp_obj_t arg) {
    mp_int_t value = mp_obj_get_int(arg);
    if (value < 0)
        mp_raise_ValueError(MP_ERROR_TEXT("expected a non-negative integer"));
    return (unsigned)value;
}

static mp_obj_t pci_count(void) {
    unsigned count;
    if (lunasdk_pci_count(&count))
        mp_raise_OSError(MP_EIO);
    return mp_obj_new_int_from_uint(count);
}
static MP_DEFINE_CONST_FUN_OBJ_0(pci_count_obj, pci_count);

static mp_obj_t pci_get(mp_obj_t index) {
    LunaPciInfo d;
    if (lunasdk_pci_get(nonnegative(index), &d))
        return mp_const_none;
    char text[96];
    int len = snprintf(text, sizeof(text), "PCI %u:%u.%u vendor=%u device=%u class=%u",
                       (unsigned)d.bus, (unsigned)d.slot, (unsigned)d.function,
                       (unsigned)d.vendor, (unsigned)d.device, (unsigned)d.class_code);
    return mp_obj_new_str(text, len);
}
static MP_DEFINE_CONST_FUN_OBJ_1(pci_get_obj, pci_get);

static mp_obj_t sdk_pci_read(size_t argc, const mp_obj_t *args) {
    (void)argc;
    uint32_t value;
    if (lunasdk_pci_read(nonnegative(args[0]), nonnegative(args[1]), nonnegative(args[2]),
                        nonnegative(args[3]), nonnegative(args[4]), &value))
        mp_raise_ValueError(MP_ERROR_TEXT("invalid PCI read"));
    return mp_obj_new_int_from_uint(value);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(pci_read_obj, 5, 5, sdk_pci_read);

static const char *cstr(mp_obj_t arg) {
    size_t len;
    const char *text = mp_obj_str_get_data(arg, &len);
    if (strlen(text) != len)
        mp_raise_ValueError(MP_ERROR_TEXT("embedded NUL"));
    return text;
}

static mp_obj_t device_read(mp_obj_t name) {
    char value[128];
    if (ldev_read(cstr(name), value, sizeof(value)))
        return mp_const_none;
    return mp_obj_new_str(value, strlen(value));
}
static MP_DEFINE_CONST_FUN_OBJ_1(device_read_obj, device_read);

static mp_obj_t device_write(mp_obj_t name, mp_obj_t value) {
    return mp_obj_new_bool(!ldev_write(cstr(name), cstr(value)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(device_write_obj, device_write);

static mp_obj_t beep(mp_obj_t freq, mp_obj_t length) {
    unsigned hz = nonnegative(freq), ms = nonnegative(length);
    if (hz < 20 || hz > 20000 || ms < 1 || ms > 250)
        mp_raise_ValueError(MP_ERROR_TEXT("beep out of range"));
    audio_beep(hz, ms);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(beep_obj, beep);

#define SDK_ENTRY(name, value) { MP_ROM_QSTR(name), MP_ROM_PTR(&(value)) }
#define SDK_MODULE(name) \
    static MP_DEFINE_CONST_DICT(name##_dict, name##_entries); \
    const mp_obj_module_t name##_module = { \
        .base = { &mp_type_module }, .globals = (mp_obj_dict_t *)&name##_dict \
    }

static const mp_rom_map_elem_t pci_entries[] = {
    SDK_ENTRY(MP_QSTR_count, pci_count_obj),
    SDK_ENTRY(MP_QSTR_get, pci_get_obj),
    SDK_ENTRY(MP_QSTR_read, pci_read_obj),
};
SDK_MODULE(pci);

static const mp_rom_map_elem_t hardware_entries[] = {
    SDK_ENTRY(MP_QSTR_pci, pci_module),
};
SDK_MODULE(hardware);

static const mp_rom_map_elem_t device_entries[] = {
    SDK_ENTRY(MP_QSTR_read, device_read_obj),
    SDK_ENTRY(MP_QSTR_write, device_write_obj),
};
SDK_MODULE(device);

static const mp_rom_map_elem_t audio_entries[] = {
    SDK_ENTRY(MP_QSTR_beep, beep_obj),
};
SDK_MODULE(audio);

static const mp_rom_map_elem_t luna_entries[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_luna) },
    SDK_ENTRY(MP_QSTR_hardware, hardware_module),
    SDK_ENTRY(MP_QSTR_device, device_module),
    SDK_ENTRY(MP_QSTR_audio, audio_module),
};
SDK_MODULE(luna);
MP_REGISTER_MODULE(MP_QSTR_luna, luna_module);
