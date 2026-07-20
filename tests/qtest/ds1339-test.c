/*
 * QTest testcase for the DS1339 RTC
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/bcd.h"
#include "libqos/i2c.h"

#define DS1339_ADDR 0x68

/* DS1339 register map */
#define DS1339_SECONDS   0x00
#define DS1339_CONTROL   0x0e
#define DS1339_STATUS    0x0f
#define DS1339_NUM_REGS  0x11

#define DS1339_STATUS_OSF 0x80

/* The time registers are shared with the DS1338. */
static void test_time(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;
    uint8_t resp[7];
    time_t now = time(NULL);
    struct tm *tm_ptr = gmtime(&now);

    i2c_read_block(i2cdev, DS1339_SECONDS, resp, sizeof(resp));

    g_assert_cmpuint(from_bcd(resp[4]), ==, tm_ptr->tm_mday);
    g_assert_cmpuint(from_bcd(resp[5]), ==, 1 + tm_ptr->tm_mon);
    g_assert_cmpuint(2000 + from_bcd(resp[6]), ==, 1900 + tm_ptr->tm_year);
}

/* The control register lives at 0x0e on the DS1339 (0x07 on the DS1338). */
static void test_control_register(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;

    i2c_set8(i2cdev, DS1339_CONTROL, 0x1c);
    g_assert_cmphex(i2c_get8(i2cdev, DS1339_CONTROL), ==, 0x1c);

    /* Bit 6 is reserved and reads back zero. */
    i2c_set8(i2cdev, DS1339_CONTROL, 0xff);
    g_assert_cmphex(i2c_get8(i2cdev, DS1339_CONTROL), ==, 0xbf);

    i2c_set8(i2cdev, DS1339_CONTROL, 0x00);
    g_assert_cmphex(i2c_get8(i2cdev, DS1339_CONTROL), ==, 0x00);
}

/*
 * The oscillator-stop flag is set at power-on and can only be cleared,
 * never set by a write.
 */
static void test_osf_write_protect(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;

    g_assert_cmphex(i2c_get8(i2cdev, DS1339_STATUS) & DS1339_STATUS_OSF,
                    ==, DS1339_STATUS_OSF);
    i2c_set8(i2cdev, DS1339_STATUS, DS1339_STATUS_OSF);
    g_assert_cmphex(i2c_get8(i2cdev, DS1339_STATUS) & DS1339_STATUS_OSF,
                    ==, DS1339_STATUS_OSF);

    i2c_set8(i2cdev, DS1339_STATUS, 0x00);
    g_assert_cmphex(i2c_get8(i2cdev, DS1339_STATUS) & DS1339_STATUS_OSF, ==, 0);
    i2c_set8(i2cdev, DS1339_STATUS, DS1339_STATUS_OSF);
    g_assert_cmphex(i2c_get8(i2cdev, DS1339_STATUS) & DS1339_STATUS_OSF, ==, 0);
}

/* The register pointer should wrap at 0x11 */
static void test_address_wrap(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *i2cdev = (QI2CDevice *)obj;

    i2c_set8(i2cdev, DS1339_CONTROL, 0x2a);
    g_assert_cmphex(i2c_get8(i2cdev, DS1339_CONTROL + DS1339_NUM_REGS),
                    ==, 0x2a);
}

static void ds1339_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "address=0x68"
    };
    add_qi2c_address(&opts, &(QI2CAddress) { DS1339_ADDR });

    qos_node_create_driver("ds1339", i2c_device_create);
    qos_node_consumes("ds1339", "i2c-bus", &opts);

    qos_add_test("time", "ds1339", test_time, NULL);
    qos_add_test("control-register", "ds1339", test_control_register, NULL);
    qos_add_test("osf-write-protect", "ds1339", test_osf_write_protect, NULL);
    qos_add_test("address-wrap", "ds1339", test_address_wrap, NULL);
}

libqos_init(ds1339_register_nodes);
