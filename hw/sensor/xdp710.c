/*
 * Infineon XDP710 Hot-Swap Controller and Digital Power Monitor with PMBus
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * The XDP710 is a wide-input-voltage (5.5 V to 80 V) PMBus hot-swap
 * controller that reports input/output voltage, output current, input
 * power and temperature telemetry.  The standard PMBus command set is
 * handled by the generic PMBus device core (hw/i2c/pmbus_device.c); this
 * file implements the reset defaults, the direct-mode data coefficients
 * and the manufacturer-specific configuration registers.
 *
 * The direct-mode coefficients and manufacturer register map are taken
 * from the Infineon XDP710 datasheet (Rev. 2.0, 2022-11-22), Table 29
 * "PMBus coefficients", cross-checked against the Linux xdp710 hwmon
 * driver (drivers/hwmon/pmbus/xdp710.c).
 *
 * Table 29 normalises the current and power coefficients to a 1 mOhm
 * sense resistor and to the 12.5 mV current-sense range.  The effective
 * coefficients therefore depend on the configured sense resistor
 * ("rsense" property) and current-sense range ("cs-range" property), and
 * are computed at realize time exactly as the Linux driver derives them
 * from the CS_RNG and REG_CFG registers.
 */

#include "qemu/osdep.h"
#include "hw/i2c/pmbus_device.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/log.h"
#include "hw/sensor/xdp710.h"

/*
 * Manufacturer-specific configuration registers, read as words.
 * Addresses and bit fields per the Linux xdp710 driver.
 */
#define XDP710_REG_CFG                  0xD3 /* [5:0]  sense-resistor index */
#define XDP710_V_SNS_CFG                0xD4 /* [1:0]  VTLM_RNG voltage range */
#define XDP710_CS_RNG                   0xD5 /* [7:6]  CS_RNG current range */

/*
 * Base direct-mode coefficients from Table 29, valid for the 88 V voltage
 * telemetry range (VTLM_RNG = 0), the 12.5 mV current-sense range
 * (CS_RNG = 0) and a 1 mOhm sense resistor.
 */
#define XDP710_M_VIN_BASE               4653
#define XDP710_M_IOUT_BASE              23165
#define XDP710_M_PIN_BASE               4211
#define XDP710_M_TEMP                   52
#define XDP710_B_TEMP                   14321

/* Defaults */
#define XDP710_OPERATION_DEFAULT        0x80
#define XDP710_CAPABILITY_NO_PEC        0x20
#define XDP710_DIRECT_MODE              0x40
#define XDP710_PMBUS_REVISION_DEFAULT   0x22
#define XDP710_HIGH_LIMIT_DEFAULT       0x7FFF
#define XDP710_MFR_ID_DEFAULT           "Infineon"
#define XDP710_MODEL_DEFAULT            "XDP710"
#define XDP710_MFR_REVISION_DEFAULT     "1"

/* Default configuration matches the datasheet worked example. */
#define XDP710_RSENSE_DEFAULT           500   /* micro-ohms (0.5 mOhm) */
#define XDP710_CS_RANGE_DEFAULT         12500 /* micro-volts (12.5 mV) */

#define XDP710_VOLT_DEFAULT             12000 /* mV */
#define XDP710_IOUT_DEFAULT             25000 /* mA */
#define XDP710_PWR_DEFAULT              300   /* W: 12V * 25A */
#define XDP710_TEMP_DEFAULT             25000 /* milli-degrees C */

/*
 * REG_CFG selects the sense resistor from this table (values in
 * micro-ohms).  The index is what the guest reads back in REG_CFG[5:0].
 */
static const uint32_t xdp710_rsense_uohm[] = {
    200, 250, 300, 330, 400, 470, 500, 600,
    670, 700, 750, 800, 900, 1000, 1100, 1200,
    1250, 1300, 1400, 1500, 1600, 1700, 1800, 1900,
    2000, 2100, 2200, 2300, 2400, 2500, 2600, 2700,
    2800, 3000, 3100, 3200, 3300, 3400, 3500, 3600,
    3700, 3800, 3900, 4000, 4100, 4200, 4300, 4400,
    4500, 4600, 4700, 4800, 4900, 5000, 5500, 6000,
    6500, 7000, 7500, 8000, 8500, 9000, 9500, 10000,
};

OBJECT_DECLARE_SIMPLE_TYPE(XDP710State, XDP710)

struct XDP710State {
    PMBusDevice parent;

    /* Configuration (device properties) */
    uint32_t rsense;    /* sense resistor value, micro-ohms */
    uint32_t cs_range;  /* current-sense range VSNS_CS, micro-volts */

    /* Configuration registers derived from the properties */
    uint8_t reg_cfg;    /* REG_CFG[5:0]: sense-resistor table index */
    uint8_t cs_rng;     /* CS_RNG[7:6]: current-sense range select (0..3) */
    uint8_t vtlm_rng;   /* V_SNS_CFG[1:0]: voltage range select (0..2) */

    /* Effective direct-mode coefficients (configuration dependent) */
    PMBusCoefficients coeff_v;
    PMBusCoefficients coeff_i;
    PMBusCoefficients coeff_p;
    PMBusCoefficients coeff_t;
};

/*
 * Fold the configured sense resistor (micro-ohms) and current-sense range
 * into a base coefficient.  Mirrors the Linux driver:
 *   m = round((base * rsense_uohm >> cs_rng) / 1000)
 */
static int32_t xdp710_scaled_m(int32_t base, uint32_t rsense, uint8_t cs_rng)
{
    uint64_t v = ((uint64_t)base * rsense) >> cs_rng;
    return (int32_t)((v + 500) / 1000);
}

static void xdp710_compute_coefficients(XDP710State *s)
{
    s->coeff_v = (PMBusCoefficients){ XDP710_M_VIN_BASE << s->vtlm_rng, 0, -2 };
    s->coeff_i = (PMBusCoefficients){
        xdp710_scaled_m(XDP710_M_IOUT_BASE, s->rsense, s->cs_rng), 0, -2
    };
    s->coeff_p = (PMBusCoefficients){
        xdp710_scaled_m(XDP710_M_PIN_BASE, s->rsense, s->cs_rng), 0, -2
    };
    s->coeff_t = (PMBusCoefficients){ XDP710_M_TEMP, XDP710_B_TEMP, -1 };
}

static uint16_t xdp710_millivolts_to_direct(XDP710State *s, uint32_t value)
{
    PMBusCoefficients c = s->coeff_v;
    c.b = c.b * 1000;
    c.R = c.R - 3;
    return pmbus_data2direct_mode(c, value);
}

static uint32_t xdp710_direct_to_millivolts(XDP710State *s, uint16_t value)
{
    PMBusCoefficients c = s->coeff_v;
    c.b = c.b * 1000;
    c.R = c.R - 3;
    return pmbus_direct_mode2data(c, value);
}

static uint16_t xdp710_milliamps_to_direct(XDP710State *s, uint32_t value)
{
    PMBusCoefficients c = s->coeff_i;
    c.b = c.b * 1000;
    c.R = c.R - 3;
    return pmbus_data2direct_mode(c, value);
}

static uint32_t xdp710_direct_to_milliamps(XDP710State *s, uint16_t value)
{
    PMBusCoefficients c = s->coeff_i;
    c.b = c.b * 1000;
    c.R = c.R - 3;
    return pmbus_direct_mode2data(c, value);
}

static uint16_t xdp710_watts_to_direct(XDP710State *s, uint32_t value)
{
    return pmbus_data2direct_mode(s->coeff_p, value);
}

static uint32_t xdp710_direct_to_watts(XDP710State *s, uint16_t value)
{
    return pmbus_direct_mode2data(s->coeff_p, value);
}

static uint16_t xdp710_millicelsius_to_direct(XDP710State *s, uint32_t value)
{
    PMBusCoefficients c = s->coeff_t;
    c.b = c.b * 1000;
    c.R = c.R - 3;
    return pmbus_data2direct_mode(c, value);
}

static uint32_t xdp710_direct_to_millicelsius(XDP710State *s, uint16_t value)
{
    PMBusCoefficients c = s->coeff_t;
    c.b = c.b * 1000;
    c.R = c.R - 3;
    return pmbus_direct_mode2data(c, value);
}

static void xdp710_reset_exit(Object *obj, ResetType type)
{
    XDP710State *s = XDP710(obj);
    PMBusDevice *pmdev = PMBUS_DEVICE(obj);

    pmdev->page = 0;
    pmdev->capability = XDP710_CAPABILITY_NO_PEC;

    pmdev->pages[0].operation = XDP710_OPERATION_DEFAULT;
    pmdev->pages[0].revision = XDP710_PMBUS_REVISION_DEFAULT;
    pmdev->pages[0].vout_mode = XDP710_DIRECT_MODE;

    pmdev->pages[0].vout_ov_warn_limit = XDP710_HIGH_LIMIT_DEFAULT;
    pmdev->pages[0].vout_uv_warn_limit = 0;
    pmdev->pages[0].iout_oc_warn_limit = XDP710_HIGH_LIMIT_DEFAULT;
    pmdev->pages[0].ot_fault_limit = XDP710_HIGH_LIMIT_DEFAULT;
    pmdev->pages[0].ot_warn_limit = XDP710_HIGH_LIMIT_DEFAULT;
    pmdev->pages[0].vin_ov_warn_limit = XDP710_HIGH_LIMIT_DEFAULT;
    pmdev->pages[0].vin_uv_warn_limit = 0;
    pmdev->pages[0].pin_op_warn_limit = XDP710_HIGH_LIMIT_DEFAULT;

    pmdev->pages[0].status_word = 0;
    pmdev->pages[0].status_vout = 0;
    pmdev->pages[0].status_iout = 0;
    pmdev->pages[0].status_input = 0;
    pmdev->pages[0].status_temperature = 0;

    pmdev->pages[0].read_vin =
        xdp710_millivolts_to_direct(s, XDP710_VOLT_DEFAULT);
    pmdev->pages[0].read_vout =
        xdp710_millivolts_to_direct(s, XDP710_VOLT_DEFAULT);
    pmdev->pages[0].read_iout =
        xdp710_milliamps_to_direct(s, XDP710_IOUT_DEFAULT);
    pmdev->pages[0].read_pin = xdp710_watts_to_direct(s, XDP710_PWR_DEFAULT);
    pmdev->pages[0].read_temperature_1 =
        xdp710_millicelsius_to_direct(s, XDP710_TEMP_DEFAULT);

    pmdev->pages[0].mfr_id = XDP710_MFR_ID_DEFAULT;
    pmdev->pages[0].mfr_model = XDP710_MODEL_DEFAULT;
    pmdev->pages[0].mfr_revision = XDP710_MFR_REVISION_DEFAULT;
}

static uint8_t xdp710_read_byte(PMBusDevice *pmdev)
{
    XDP710State *s = XDP710(pmdev);

    switch (pmdev->code) {
    case XDP710_REG_CFG:
        pmbus_send16(pmdev, s->reg_cfg);
        break;

    case XDP710_V_SNS_CFG:
        pmbus_send16(pmdev, s->vtlm_rng);
        break;

    case XDP710_CS_RNG:
        pmbus_send16(pmdev, (uint16_t)s->cs_rng << 6);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: reading from unsupported register: 0x%02x\n",
                      __func__, pmdev->code);
        return PMBUS_ERR_BYTE;
    }

    return 0;
}

static void xdp710_get(Object *obj, Visitor *v, const char *name,
                       void *opaque, Error **errp)
{
    XDP710State *s = XDP710(obj);
    uint16_t value;

    if (strcmp(name, "vin") == 0 || strcmp(name, "vout") == 0) {
        value = xdp710_direct_to_millivolts(s, *(uint16_t *)opaque);
    } else if (strcmp(name, "iout") == 0) {
        value = xdp710_direct_to_milliamps(s, *(uint16_t *)opaque);
    } else if (strcmp(name, "pin") == 0) {
        value = xdp710_direct_to_watts(s, *(uint16_t *)opaque);
    } else if (strcmp(name, "temp") == 0) {
        value = xdp710_direct_to_millicelsius(s, *(uint16_t *)opaque);
    } else {
        value = *(uint16_t *)opaque;
    }

    visit_type_uint16(v, name, &value, errp);
}

static void xdp710_set(Object *obj, Visitor *v, const char *name,
                       void *opaque, Error **errp)
{
    XDP710State *s = XDP710(obj);
    PMBusDevice *pmdev = PMBUS_DEVICE(obj);
    uint16_t *internal = opaque;
    uint16_t value;

    if (!visit_type_uint16(v, name, &value, errp)) {
        return;
    }

    if (strcmp(name, "vin") == 0 || strcmp(name, "vout") == 0) {
        *internal = xdp710_millivolts_to_direct(s, value);
    } else if (strcmp(name, "iout") == 0) {
        *internal = xdp710_milliamps_to_direct(s, value);
    } else if (strcmp(name, "pin") == 0) {
        *internal = xdp710_watts_to_direct(s, value);
    } else if (strcmp(name, "temp") == 0) {
        *internal = xdp710_millicelsius_to_direct(s, value);
    } else {
        *internal = value;
    }

    pmbus_check_limits(pmdev);
}

static const VMStateDescription vmstate_xdp710 = {
    .name = "XDP710",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]){
        VMSTATE_PMBUS_DEVICE(parent, XDP710State),
        VMSTATE_END_OF_LIST()
    }
};

static void xdp710_init(Object *obj)
{
    PMBusDevice *pmdev = PMBUS_DEVICE(obj);
    uint64_t flags = PB_HAS_VOUT_MODE | PB_HAS_VOUT | PB_HAS_VIN |
                     PB_HAS_IOUT | PB_HAS_PIN | PB_HAS_TEMPERATURE |
                     PB_HAS_MFR_INFO;

    pmbus_page_config(pmdev, 0, flags);

    object_property_add(obj, "vin", "uint16", xdp710_get, xdp710_set, NULL,
                        &pmdev->pages[0].read_vin);
    object_property_add(obj, "vout", "uint16", xdp710_get, xdp710_set, NULL,
                        &pmdev->pages[0].read_vout);
    object_property_add(obj, "iout", "uint16", xdp710_get, xdp710_set, NULL,
                        &pmdev->pages[0].read_iout);
    object_property_add(obj, "pin", "uint16", xdp710_get, xdp710_set, NULL,
                        &pmdev->pages[0].read_pin);
    object_property_add(obj, "temp", "uint16", xdp710_get, xdp710_set, NULL,
                        &pmdev->pages[0].read_temperature_1);
}

/* Look up the REG_CFG index for a sense resistor value, or -1 if unlisted. */
static int xdp710_rsense_index(uint32_t rsense)
{
    for (unsigned idx = 0; idx < ARRAY_SIZE(xdp710_rsense_uohm); idx++) {
        if (xdp710_rsense_uohm[idx] == rsense) {
            return idx;
        }
    }
    return -1;
}

static void xdp710_realize(DeviceState *dev, Error **errp)
{
    XDP710State *s = XDP710(dev);
    int reg_cfg = xdp710_rsense_index(s->rsense);

    /* Map the sense-resistor property to a REG_CFG index. */
    if (reg_cfg < 0) {
        error_setg(errp, "xdp710: unsupported rsense %u uOhm", s->rsense);
        return;
    }
    s->reg_cfg = reg_cfg;

    /* Map the current-sense range property to a CS_RNG selection. */
    switch (s->cs_range) {
    case 12500:
        s->cs_rng = 0;
        break;
    case 25000:
        s->cs_rng = 1;
        break;
    case 50000:
        s->cs_rng = 2;
        break;
    case 100000:
        s->cs_rng = 3;
        break;
    default:
        error_setg(errp, "xdp710: unsupported cs-range %u uV", s->cs_range);
        return;
    }

    /* Only the 88 V telemetry range is modelled. */
    s->vtlm_rng = 0;

    xdp710_compute_coefficients(s);
}

static const Property xdp710_properties[] = {
    DEFINE_PROP_UINT32("rsense", XDP710State, rsense, XDP710_RSENSE_DEFAULT),
    DEFINE_PROP_UINT32("cs-range", XDP710State, cs_range,
                       XDP710_CS_RANGE_DEFAULT),
};

static void xdp710_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);
    PMBusDeviceClass *k = PMBUS_DEVICE_CLASS(klass);

    dc->desc = "Infineon XDP710 Hot Swap controller";
    dc->vmsd = &vmstate_xdp710;
    dc->realize = xdp710_realize;
    device_class_set_props(dc, xdp710_properties);
    k->receive_byte = xdp710_read_byte;
    k->device_num_pages = 1;

    rc->phases.exit = xdp710_reset_exit;
}

static const TypeInfo xdp710_types[] = {
    {
        .name = TYPE_XDP710,
        .parent = TYPE_PMBUS_DEVICE,
        .instance_size = sizeof(XDP710State),
        .instance_init = xdp710_init,
        .class_init = xdp710_class_init,
    },
};

DEFINE_TYPES(xdp710_types)
