/*
 * QTest testcase for the ADC128D818 ADC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqos/i2c.h"
#include "libqos/qgraph.h"
#include "libqtest-single.h"
#include "qobject/qdict.h"

/* clang-format off */
#define ADC128D818_TEST_ID      "adc128d818-test"
#define ADC128D818_TEST_ADDR    0x1F

/* Register addresses */
#define REG_CONFIG              0x00u
#define REG_INT_STATUS          0x01u
#define REG_INT_MASK            0x03u
#define REG_CONV_RATE           0x07u
#define REG_CH_DISABLE          0x08u
#define REG_ONE_SHOT            0x09u
#define REG_DEEP_SHUTDOWN       0x0Au
#define REG_ADV_CONFIG          0x0Bu
#define REG_BUSY_STATUS         0x0Cu

#define REG_CH_READING_BASE     0x20u
#define REG_LIMIT_BASE          0x2Au

#define REG_MANUFACTURER_ID     0x3Eu
#define REG_REVISION_ID         0x3Fu

/* Configuration Register bits */
#define CONFIG_START            (1u << 0u)
#define CONFIG_INT_ENABLE       (1u << 1u)
#define CONFIG_INT_CLEAR        (1u << 3u)
#define CONFIG_INITIALIZATION   (1u << 7u)

/* Advanced Configuration Register bits */
#define ADV_CONFIG_MODE_1       (1u << 1u)
#define ADV_CONFIG_MODE_2       (2u << 1u)
#define ADV_CONFIG_MODE_3       (3u << 1u)

/* Number of channels */
#define NUM_CHANNELS            8u

/* Internal VREF in mV */
#define INTERNAL_VREF_MV        2560u

/* TAP-compatible diagnostic output (g_test_message is silent in TAP mode) */
#define test_log(fmt, ...) \
    fprintf(stderr, "# " fmt "\n", ## __VA_ARGS__)
/* clang-format on */

/* QMP helpers for setting device properties */

static void qmp_adc128d818_set(const char *property, int value)
{
    QDict *resp;

    resp = qmp("{ 'execute': 'qom-set', 'arguments':"
               " { 'path': %s, 'property': %s, 'value': %d } }",
               ADC128D818_TEST_ID, property, value);
    g_assert(qdict_haskey(resp, "return"));
    qobject_unref(resp);
}

static int qmp_adc128d818_get(const char *property)
{
    QDict *resp;
    int ret;

    resp = qmp("{ 'execute': 'qom-get', 'arguments':"
               " { 'path': %s, 'property': %s } }",
               ADC128D818_TEST_ID, property);
    g_assert(qdict_haskey(resp, "return"));
    ret = qdict_get_int(resp, "return");
    qobject_unref(resp);
    return ret;
}

/* Test: Manufacturer and Revision ID registers */
static void test_id_registers(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;

    g_assert_cmphex(i2c_get8(dev, REG_MANUFACTURER_ID), ==, 0x01);
    g_assert_cmphex(i2c_get8(dev, REG_REVISION_ID), ==, 0x09);
}

/* Test: Power-on-reset default values */
static void test_defaults(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    unsigned ch;

    g_assert_cmphex(i2c_get8(dev, REG_CONFIG), ==, 0x08);
    g_assert_cmphex(i2c_get8(dev, REG_INT_STATUS), ==, 0x00);
    g_assert_cmphex(i2c_get8(dev, REG_INT_MASK), ==, 0x00);
    g_assert_cmphex(i2c_get8(dev, REG_CONV_RATE), ==, 0x00);
    g_assert_cmphex(i2c_get8(dev, REG_CH_DISABLE), ==, 0x00);
    g_assert_cmphex(i2c_get8(dev, REG_DEEP_SHUTDOWN), ==, 0x00);
    g_assert_cmphex(i2c_get8(dev, REG_ADV_CONFIG), ==, 0x00);
    g_assert_cmphex(i2c_get8(dev, REG_BUSY_STATUS), ==, 0x02);

    for (ch = 0u; ch < NUM_CHANNELS; ch++) {
        g_assert_cmphex(i2c_get8(dev, REG_LIMIT_BASE + ch * 2u), ==, 0xFF);
        g_assert_cmphex(i2c_get8(dev, REG_LIMIT_BASE + ch * 2u + 1u), ==, 0x00);
    }
}

/* Test: Software reset via INITIALIZATION bit */
static void test_soft_reset(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;

    /* Write non-default values */
    i2c_set8(dev, REG_INT_MASK, 0xAA);
    i2c_set8(dev, REG_CH_DISABLE, 0x55);
    i2c_set8(dev, REG_LIMIT_BASE, 0x42);

    g_assert_cmphex(i2c_get8(dev, REG_INT_MASK), ==, 0xAA);
    g_assert_cmphex(i2c_get8(dev, REG_CH_DISABLE), ==, 0x55);
    g_assert_cmphex(i2c_get8(dev, REG_LIMIT_BASE), ==, 0x42);

    /* Trigger software reset */
    i2c_set8(dev, REG_CONFIG, CONFIG_INITIALIZATION);

    /* Verify defaults restored */
    g_assert_cmphex(i2c_get8(dev, REG_CONFIG), ==, 0x08);
    g_assert_cmphex(i2c_get8(dev, REG_INT_MASK), ==, 0x00);
    g_assert_cmphex(i2c_get8(dev, REG_CH_DISABLE), ==, 0x00);
    g_assert_cmphex(i2c_get8(dev, REG_LIMIT_BASE), ==, 0xFF);
    g_assert_cmphex(i2c_get8(dev, REG_BUSY_STATUS), ==, 0x02);
}

/*
 * Test: Voltage conversion
 *
 * With internal VREF = 2560 mV:
 *   ain = 1280 mV -> DOUT = 1280 * 4096 / 2560 = 2048
 *   16-bit reading = 2048 << 4 = 0x8000
 */
static void test_voltage_conversion(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    qmp_adc128d818_set("ain0", 1280);
    test_log("Injected ain0 = 1280 mV");
    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("Read ch0: raw 0x%04x -> %u mV", reading,
             (reading >> 4u) * INTERNAL_VREF_MV / 4096u);
    g_assert_cmphex(reading, ==, 0x8000);

    /* Full scale: 2560 mV -> DOUT = 4095, reading = 0xFFF0 */
    qmp_adc128d818_set("ain1", 2560);
    test_log("Injected ain1 = 2560 mV");
    reading = i2c_get16(dev, REG_CH_READING_BASE + 1u);
    test_log("Read ch1: raw 0x%04x -> %u mV", reading,
             (reading >> 4u) * INTERNAL_VREF_MV / 4096u);
    g_assert_cmphex(reading, ==, 0xFFF0);

    /* Zero: 0 mV -> DOUT = 0, reading = 0x0000 */
    qmp_adc128d818_set("ain2", 0);
    test_log("Injected ain2 = 0 mV");
    reading = i2c_get16(dev, REG_CH_READING_BASE + 2u);
    test_log("Read ch2: raw 0x%04x -> %u mV", reading,
             (reading >> 4u) * INTERNAL_VREF_MV / 4096u);
    g_assert_cmphex(reading, ==, 0x0000);
}

/*
 * Test: Temperature conversion (mode 0, channel 7 = temperature)
 *
 * 25000 mC = 25 deg C -> raw = 50 (0.5 deg C per LSb)
 * 16-bit reading = 50 << 7 = 0x1900
 */
static void
test_temperature_conversion(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    /* Default mode 0: channel 7 is temperature */
    qmp_adc128d818_set("temperature", 25000);
    test_log("Injected temperature = 25000 mC (25.0 deg C)");
    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    reading = i2c_get16(dev, REG_CH_READING_BASE + 7u);
    test_log("Read ch7: raw 0x%04x -> %d mC", reading,
             (int16_t)(reading & 0xFF80u) * 500 / 128);
    g_assert_cmphex(reading, ==, 0x1900);
}

/* Test: Interrupt status set on limit violation, cleared on read */
static void test_interrupt_status(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint8_t status;

    /* Set high limit for ch0 to a low value */
    i2c_set8(dev, REG_LIMIT_BASE, 0x10);
    test_log("Set ch0 high limit = 0x10");

    /* Apply full-scale voltage -> MSB = 0xFF, exceeds 0x10 limit */
    qmp_adc128d818_set("ain0", 2560);
    test_log("Injected ain0 = 2560 mV (exceeds limit)");

    /* Start conversions with INT_ENABLE */
    i2c_set8(dev, REG_CONFIG, CONFIG_START | CONFIG_INT_ENABLE);

    /* INT_STATUS should have bit 0 set */
    status = i2c_get8(dev, REG_INT_STATUS);
    test_log("INT_STATUS = 0x%02x (expect bit 0 set)", status);
    g_assert_cmphex(status & 0x01u, ==, 0x01);

    /* Reading INT_STATUS should clear it */
    status = i2c_get8(dev, REG_INT_STATUS);
    test_log("INT_STATUS after re-read = 0x%02x (expect cleared)", status);
    g_assert_cmphex(status, ==, 0x00);
}

/* Test: INT_CLEAR bit in CONFIG prevents interrupt assertion */
static void test_int_clear(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint8_t status;

    /* Set up a limit violation */
    i2c_set8(dev, REG_LIMIT_BASE, 0x10);
    qmp_adc128d818_set("ain0", 2560);

    /* Start with INT_ENABLE and INT_CLEAR both set */
    i2c_set8(dev, REG_CONFIG,
             CONFIG_START | CONFIG_INT_ENABLE | CONFIG_INT_CLEAR);

    /* INT_STATUS still records the violation */
    status = i2c_get8(dev, REG_INT_STATUS);
    g_assert_cmphex(status & 0x01u, ==, 0x01);
}

/* Test: Channel disable prevents conversion */
static void test_channel_disable(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    /* Disable channel 0 before starting */
    i2c_set8(dev, REG_CH_DISABLE, 0x01);
    test_log("Disabled channel 0");

    qmp_adc128d818_set("ain0", 1280);
    test_log("Injected ain0 = 1280 mV (disabled)");
    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    /* Channel 0 should remain at zero (not converted) */
    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("Read ch0 (disabled): raw 0x%04x", reading);
    g_assert_cmphex(reading, ==, 0x0000);

    /* Channel 1 should still work */
    qmp_adc128d818_set("ain1", 1280);
    test_log("Injected ain1 = 1280 mV (enabled)");
    reading = i2c_get16(dev, REG_CH_READING_BASE + 1u);
    test_log("Read ch1 (enabled): raw 0x%04x -> %u mV", reading,
             (reading >> 4u) * INTERNAL_VREF_MV / 4096u);
    g_assert_cmphex(reading, ==, 0x8000);
}

/* Test: One-shot conversion in shutdown mode */
static void test_one_shot(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    /* Device is not started (shutdown) */
    qmp_adc128d818_set("ain0", 1280);
    test_log("Injected ain0 = 1280 mV (device stopped)");

    /* Channel reading should be zero before one-shot */
    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("Read ch0 before one-shot: raw 0x%04x", reading);
    g_assert_cmphex(reading, ==, 0x0000);

    /* Trigger one-shot */
    i2c_set8(dev, REG_ONE_SHOT, 0x01);
    test_log("Triggered one-shot conversion");

    /* Now channel reading should be converted */
    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("Read ch0 after one-shot: raw 0x%04x -> %u mV", reading,
             (reading >> 4u) * INTERNAL_VREF_MV / 4096u);
    g_assert_cmphex(reading, ==, 0x8000);
}

/* Test: Mode 1 makes channel 7 a voltage input instead of temperature */
static void test_mode_selection(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    /* Set mode 1: all 8 channels are voltage */
    i2c_set8(dev, REG_ADV_CONFIG, ADV_CONFIG_MODE_1);
    test_log("Set mode 1 (all voltage channels)");

    /* Set a voltage on channel 7 and a temperature */
    qmp_adc128d818_set("ain7", 1280);
    qmp_adc128d818_set("temperature", 50000);
    test_log("Injected ain7 = 1280 mV, temperature = 50000 mC");

    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    /* In mode 1, ch7 should reflect voltage (1280 mV), not temperature */
    reading = i2c_get16(dev, REG_CH_READING_BASE + 7u);
    test_log("Read ch7 (mode 1): raw 0x%04x -> %u mV", reading,
             (reading >> 4u) * INTERNAL_VREF_MV / 4096u);
    g_assert_cmphex(reading, ==, 0x8000);
}

/*
 * Test: Mode 2 — 4 pseudo-differential pairs
 *
 * Pair 0: IN0(+) / IN1(-) -> register 0x20
 * Pair 1: IN3(+) / IN2(-) -> register 0x21
 * Pair 2: IN4(+) / IN5(-) -> register 0x22
 * Pair 3: IN7(+) / IN6(-) -> register 0x23
 *
 * With VREF = 2560 mV, ΔV = 1280 mV -> DOUT = 2048, reading = 0x8000.
 */
static void test_mode2_diff(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    i2c_set8(dev, REG_ADV_CONFIG, ADV_CONFIG_MODE_2);
    test_log("Set mode 2 (4 pseudo-differential pairs)");

    /* Pair 0: IN0(+) = 2000 mV, IN1(-) = 720 mV -> ΔV = 1280 mV */
    qmp_adc128d818_set("ain0", 2000);
    qmp_adc128d818_set("ain1", 720);
    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("Pair 0 (IN0-IN1): raw 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);

    /* Pair 1: IN3(+) = 1920 mV, IN2(-) = 640 mV -> ΔV = 1280 mV */
    qmp_adc128d818_set("ain3", 1920);
    qmp_adc128d818_set("ain2", 640);

    reading = i2c_get16(dev, REG_CH_READING_BASE + 1u);
    test_log("Pair 1 (IN3-IN2): raw 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);

    /* Pair 2: IN4(+) = 1500 mV, IN5(-) = 220 mV -> ΔV = 1280 mV */
    qmp_adc128d818_set("ain4", 1500);
    qmp_adc128d818_set("ain5", 220);

    reading = i2c_get16(dev, REG_CH_READING_BASE + 2u);
    test_log("Pair 2 (IN4-IN5): raw 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);

    /* Pair 3: IN7(+) = 2560 mV, IN6(-) = 1280 mV -> ΔV = 1280 mV */
    qmp_adc128d818_set("ain7", 2560);
    qmp_adc128d818_set("ain6", 1280);

    reading = i2c_get16(dev, REG_CH_READING_BASE + 3u);
    test_log("Pair 3 (IN7-IN6): raw 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);

    /* Reserved channels 4-6 should read zero */
    reading = i2c_get16(dev, REG_CH_READING_BASE + 4u);
    test_log("Reserved ch4: raw 0x%04x (expect 0x0000)", reading);
    g_assert_cmphex(reading, ==, 0x0000);

    /* Negative ΔV clips to zero */
    qmp_adc128d818_set("ain0", 500);
    qmp_adc128d818_set("ain1", 1000);

    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("Pair 0 negative ΔV: raw 0x%04x (expect 0x0000)", reading);
    g_assert_cmphex(reading, ==, 0x0000);
}

/*
 * Test: Mode 3 — 4 single-ended + 2 pseudo-differential pairs
 *
 * Channels 0-3: single-ended (same as mode 0)
 * Channel 4: IN4(+) / IN5(-) -> register 0x24
 * Channel 5: IN7(+) / IN6(-) -> register 0x25
 * Channel 6: reserved
 * Channel 7: temperature
 */
static void test_mode3_mixed(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    i2c_set8(dev, REG_ADV_CONFIG, ADV_CONFIG_MODE_3);
    test_log("Set mode 3 (4 single-ended + 2 differential)");

    /* Single-ended channel 0: 1280 mV -> 0x8000 */
    qmp_adc128d818_set("ain0", 1280);
    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("Ch0 single-ended: raw 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);

    /* Differential pair at ch4: IN4(+) = 1500, IN5(-) = 220 -> ΔV = 1280 */
    qmp_adc128d818_set("ain4", 1500);
    qmp_adc128d818_set("ain5", 220);

    reading = i2c_get16(dev, REG_CH_READING_BASE + 4u);
    test_log("Ch4 diff (IN4-IN5): raw 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);

    /* Differential pair at ch5: IN7(+) = 2560, IN6(-) = 1280 -> ΔV = 1280 */
    qmp_adc128d818_set("ain7", 2560);
    qmp_adc128d818_set("ain6", 1280);

    reading = i2c_get16(dev, REG_CH_READING_BASE + 5u);
    test_log("Ch5 diff (IN7-IN6): raw 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);

    /* Reserved channel 6 should read zero */
    reading = i2c_get16(dev, REG_CH_READING_BASE + 6u);
    test_log("Reserved ch6: raw 0x%04x (expect 0x0000)", reading);
    g_assert_cmphex(reading, ==, 0x0000);

    /* Channel 7 is still temperature */
    qmp_adc128d818_set("temperature", 25000);

    reading = i2c_get16(dev, REG_CH_READING_BASE + 7u);
    test_log("Ch7 temperature: raw 0x%04x (expect 0x1900)", reading);
    g_assert_cmphex(reading, ==, 0x1900);
}

/* Test: Mode change resets channel readings and interrupt status */
static void test_mode_change_reset(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;
    uint8_t status;

    /* Start in mode 0, inject voltage, convert */
    qmp_adc128d818_set("ain0", 1280);
    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("Before mode change, ch0: raw 0x%04x", reading);
    g_assert_cmphex(reading, !=, 0x0000);

    /* Force an interrupt status bit */
    i2c_set8(dev, REG_CONFIG, 0x00);
    i2c_set8(dev, REG_LIMIT_BASE, 0x10);
    qmp_adc128d818_set("ain0", 2560);
    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    /* Switch to mode 2 — readings and INT_STATUS should reset */
    i2c_set8(dev, REG_CONFIG, 0x00);
    i2c_set8(dev, REG_ADV_CONFIG, ADV_CONFIG_MODE_2);

    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("After mode change, ch0: raw 0x%04x (expect 0x0000)", reading);
    g_assert_cmphex(reading, ==, 0x0000);

    status = i2c_get8(dev, REG_INT_STATUS);
    test_log("After mode change, INT_STATUS: 0x%02x (expect 0x00)", status);
    g_assert_cmphex(status, ==, 0x00);

    /* Limits should NOT be reset */
    g_assert_cmphex(i2c_get8(dev, REG_LIMIT_BASE), ==, 0x10);
    test_log("Limit register preserved after mode change");
}

/*
 * Test: QOM property changes trigger correct differential conversion
 *
 * Set mode 2, start the device, then update ain pins via QOM and verify
 * the channel reading reflects the differential voltage.
 */
static void test_diff_qom_trigger(void *obj, void *data,
                                  QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    /* Reset to clean state, then configure mode 2 */
    i2c_set8(dev, REG_CONFIG, CONFIG_INITIALIZATION);
    i2c_set8(dev, REG_ADV_CONFIG, ADV_CONFIG_MODE_2);

    /* Start with both pins at known values */
    qmp_adc128d818_set("ain0", 0);
    qmp_adc128d818_set("ain1", 0);
    qmp_adc128d818_set("ain2", 0);
    qmp_adc128d818_set("ain3", 0);
    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    /* Set positive pin: IN0(+) = 2000 mV, IN1(-) = 0 mV -> ΔV = 2000 */
    qmp_adc128d818_set("ain0", 2000);
    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("After ain0=2000, ain1=0: pair0 = 0x%04x (expect 0xC800)",
             reading);
    g_assert_cmphex(reading, ==, 0xC800);

    /* Now set negative pin: ΔV = 2000 - 720 = 1280 mV -> 0x8000 */
    qmp_adc128d818_set("ain1", 720);
    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("After ain1=720: pair0 = 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);

    /* Reversed pair (IN3+/IN2-): set via QOM while running */
    qmp_adc128d818_set("ain3", 1920);
    qmp_adc128d818_set("ain2", 640);
    reading = i2c_get16(dev, REG_CH_READING_BASE + 1u);
    test_log("Pair 1 (IN3-IN2) via QOM: 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);
}

/* Test: Verify ain property readback via QMP */
static void test_ain_property(void *obj, void *data, QGuestAllocator *alloc)
{
    int value;

    qmp_adc128d818_set("ain3", 1500);
    value = qmp_adc128d818_get("ain3");
    test_log("Set ain3 = 1500 mV, readback = %d mV", value);
    g_assert_cmpint(value, ==, 1500);

    qmp_adc128d818_set("temperature", 37500);
    value = qmp_adc128d818_get("temperature");
    test_log("Set temperature = 37500 mC, readback = %d mC", value);
    g_assert_cmpint(value, ==, 37500);
}

static void adc128d818_register_nodes(void)
{
    /* clang-format off */
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "id=" ADC128D818_TEST_ID
                             ",address=0x1f"
    };
    /* clang-format on */
    add_qi2c_address(&opts, &(QI2CAddress) { ADC128D818_TEST_ADDR });

    qos_node_create_driver("adc128d818", i2c_device_create);
    qos_node_consumes("adc128d818", "i2c-bus", &opts);

    qos_add_test("id-registers", "adc128d818", test_id_registers, NULL);
    qos_add_test("defaults", "adc128d818", test_defaults, NULL);
    qos_add_test("soft-reset", "adc128d818", test_soft_reset, NULL);
    qos_add_test("voltage-conversion", "adc128d818", test_voltage_conversion,
                 NULL);
    qos_add_test("temperature-conversion", "adc128d818",
                 test_temperature_conversion, NULL);
    qos_add_test("interrupt-status", "adc128d818", test_interrupt_status, NULL);
    qos_add_test("int-clear", "adc128d818", test_int_clear, NULL);
    qos_add_test("channel-disable", "adc128d818", test_channel_disable, NULL);
    qos_add_test("one-shot", "adc128d818", test_one_shot, NULL);
    qos_add_test("mode-selection", "adc128d818", test_mode_selection, NULL);
    qos_add_test("mode2-diff", "adc128d818", test_mode2_diff, NULL);
    qos_add_test("mode3-mixed", "adc128d818", test_mode3_mixed, NULL);
    qos_add_test("mode-change-reset", "adc128d818", test_mode_change_reset,
                 NULL);
    qos_add_test("diff-qom-trigger", "adc128d818", test_diff_qom_trigger,
                 NULL);
    qos_add_test("ain-property", "adc128d818", test_ain_property, NULL);
}
libqos_init(adc128d818_register_nodes);
