/*
 * QTests for the Infineon XDP710 hot-swap controller
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * NOTE: This is a stub test exercising the standard PMBus command surface
 * of the XDP710 model.  It should be extended once the manufacturer-specific
 * register map and direct-mode coefficients are confirmed against the
 * datasheet (see FIXME markers in hw/sensor/xdp710.c).
 */

#include "qemu/osdep.h"
#include "hw/i2c/pmbus_device.h"
#include "libqtest-single.h"
#include "libqos/qgraph.h"
#include "libqos/i2c.h"
#include "qobject/qdict.h"

#define TEST_ID   "xdp710-test"
#define TEST_ADDR (0x10)

#define XDP710_MFR_ID_DEFAULT       "Infineon"
#define XDP710_MODEL_DEFAULT        "XDP710"
#define XDP710_DIRECT_MODE          0x40

static uint16_t xdp710_i2c_get16(QI2CDevice *i2cdev, uint8_t reg)
{
    uint8_t resp[2];
    i2c_read_block(i2cdev, reg, resp, sizeof(resp));
    return (resp[1] << 8) | resp[0];
}

static void xdp710_i2c_set16(QI2CDevice *i2cdev, uint8_t reg, uint16_t value)
{
    uint8_t data[2];
    data[0] = value & 0xFF;
    data[1] = value >> 8;
    i2c_write_block(i2cdev, reg, data, sizeof(data));
}

/* Read back the manufacturer identity and telemetry defaults. */
static void test_defaults(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    uint8_t block[16] = { 0 };
    uint8_t mode;

    mode = i2c_get8(i2cdev, PMBUS_VOUT_MODE);
    g_assert_cmphex(mode, ==, XDP710_DIRECT_MODE);

    /*
     * Block reads are length-prepended and not NUL-terminated.  Read exactly
     * length+1 bytes; over-reading a PMBus block leaves the core mid-stream
     * and corrupts the following transaction.
     */
    i2c_read_block(i2cdev, PMBUS_MFR_ID, block,
                   1 + strlen(XDP710_MFR_ID_DEFAULT));
    g_assert_cmpuint(block[0], ==, strlen(XDP710_MFR_ID_DEFAULT));
    block[1 + block[0]] = '\0';
    g_assert_cmpstr((const char *)block + 1, ==, XDP710_MFR_ID_DEFAULT);

    memset(block, 0, sizeof(block));
    i2c_read_block(i2cdev, PMBUS_MFR_MODEL, block,
                   1 + strlen(XDP710_MODEL_DEFAULT));
    g_assert_cmpuint(block[0], ==, strlen(XDP710_MODEL_DEFAULT));
    block[1 + block[0]] = '\0';
    g_assert_cmpstr((const char *)block + 1, ==, XDP710_MODEL_DEFAULT);
}

/* Telemetry (READ_*) registers are read-only: writes must not stick. */
static void test_ro_regs(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    uint16_t init, val;

    init = xdp710_i2c_get16(i2cdev, PMBUS_READ_VIN);
    xdp710_i2c_set16(i2cdev, PMBUS_READ_VIN, 0xBEEF);
    val = xdp710_i2c_get16(i2cdev, PMBUS_READ_VIN);
    g_assert_cmphex(init, ==, val);

    init = xdp710_i2c_get16(i2cdev, PMBUS_READ_IOUT);
    xdp710_i2c_set16(i2cdev, PMBUS_READ_IOUT, 0x1234);
    val = xdp710_i2c_get16(i2cdev, PMBUS_READ_IOUT);
    g_assert_cmphex(init, ==, val);

    init = xdp710_i2c_get16(i2cdev, PMBUS_READ_PIN);
    xdp710_i2c_set16(i2cdev, PMBUS_READ_PIN, 0xDEAD);
    val = xdp710_i2c_get16(i2cdev, PMBUS_READ_PIN);
    g_assert_cmphex(init, ==, val);
}

/* Warn-limit registers are read/write. */
static void test_rw_regs(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    uint16_t val;

    xdp710_i2c_set16(i2cdev, PMBUS_VIN_OV_WARN_LIMIT, 0x0abc);
    val = xdp710_i2c_get16(i2cdev, PMBUS_VIN_OV_WARN_LIMIT);
    g_assert_cmphex(val, ==, 0x0abc);

    xdp710_i2c_set16(i2cdev, PMBUS_IOUT_OC_WARN_LIMIT, 0x0123);
    val = xdp710_i2c_get16(i2cdev, PMBUS_IOUT_OC_WARN_LIMIT);
    g_assert_cmphex(val, ==, 0x0123);
}

static void xdp710_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "id=" TEST_ID ",address=0x10"
    };
    add_qi2c_address(&opts, &(QI2CAddress) { TEST_ADDR });

    qos_node_create_driver("xdp710", i2c_device_create);
    qos_node_consumes("xdp710", "i2c-bus", &opts);

    qos_add_test("test_defaults", "xdp710", test_defaults, NULL);
    qos_add_test("test_ro_regs", "xdp710", test_ro_regs, NULL);
    qos_add_test("test_rw_regs", "xdp710", test_rw_regs, NULL);
}
libqos_init(xdp710_register_nodes);
