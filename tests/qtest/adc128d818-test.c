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

/* Advanced Configuration Register bits */
#define ADV_CONFIG_EXT_REF_EN   (1u << 0u)

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

/* Test: INT_CLEAR stops the round-robin monitoring loop */
static void test_int_clear(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint8_t status;

    /* Set up a limit violation */
    i2c_set8(dev, REG_LIMIT_BASE, 0x10);
    qmp_adc128d818_set("ain0", 2560);

    /*
     * Start with INT_ENABLE and INT_CLEAR both set. Per the datasheet,
     * INT_CLEAR stops the monitoring loop, so no conversion runs and the
     * violation is never recorded.
     */
    i2c_set8(dev, REG_CONFIG,
             CONFIG_START | CONFIG_INT_ENABLE | CONFIG_INT_CLEAR);
    status = i2c_get8(dev, REG_INT_STATUS);
    test_log("INT_STATUS with INT_CLEAR set = 0x%02x (expect 0x00)", status);
    g_assert_cmphex(status, ==, 0x00);

    /* Clearing INT_CLEAR resumes monitoring; the violation is now recorded */
    i2c_set8(dev, REG_CONFIG, CONFIG_START | CONFIG_INT_ENABLE);
    status = i2c_get8(dev, REG_INT_STATUS);
    test_log("INT_STATUS after INT_CLEAR cleared = 0x%02x (expect bit 0)",
             status);
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

    /* The one-shot register is not a data register and always reads zero */
    g_assert_cmphex(i2c_get8(dev, REG_ONE_SHOT), ==, 0x00);

    /* The written value is irrelevant: even 0x00 triggers a conversion */
    i2c_set8(dev, REG_ONE_SHOT, 0x00);
    test_log("Triggered one-shot conversion with value 0x00");

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

/*
 * Test: All channels with distinct voltages
 *
 * Exercises all 8 channels simultaneously with different values to verify
 * independent conversion.  Mode 1 is used so channel 7 is voltage, not
 * temperature.
 *
 * DOUT = ain * 4096 / 2560, reading = DOUT << 4
 */
static void test_all_channels(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    /* clang-format off */
    static const uint16_t ain_mv[NUM_CHANNELS] = {
        0, 320, 640, 960, 1280, 1920, 2240, 2560
    };
    static const uint16_t expect[NUM_CHANNELS] = {
        0x0000, 0x2000, 0x4000, 0x6000, 0x8000, 0xC000, 0xE000, 0xFFF0
    };
    /* clang-format on */
    uint16_t reading;
    unsigned ch;

    i2c_set8(dev, REG_CONFIG, CONFIG_INITIALIZATION);
    i2c_set8(dev, REG_ADV_CONFIG, ADV_CONFIG_MODE_1);

    for (ch = 0u; ch < NUM_CHANNELS; ch++) {
        char name[8];
        snprintf(name, sizeof(name), "ain%u", ch);
        qmp_adc128d818_set(name, ain_mv[ch]);
    }

    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    for (ch = 0u; ch < NUM_CHANNELS; ch++) {
        reading = i2c_get16(dev, REG_CH_READING_BASE + ch);
        test_log("ch%u: ain %u mV -> raw 0x%04x (expect 0x%04x)",
                 ch, ain_mv[ch], reading, expect[ch]);
        g_assert_cmphex(reading, ==, expect[ch]);
    }
}

/*
 * Test: Voltage conversion edge cases
 *
 * - Over-range input (> VREF) clamps to 0xFFF0
 * - 1 LSB: ain = VREF/4096 rounded -> smallest non-zero DOUT
 * - Just below full scale
 */
static void test_voltage_edges(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    i2c_set8(dev, REG_CONFIG, CONFIG_INITIALIZATION);
    i2c_set8(dev, REG_ADV_CONFIG, ADV_CONFIG_MODE_1);

    /* Over-range: 3000 mV with 2560 mV VREF -> clamps to 4095 */
    qmp_adc128d818_set("ain0", 3000);
    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("Over-range 3000 mV: raw 0x%04x (expect 0xFFF0)", reading);
    g_assert_cmphex(reading, ==, 0xFFF0);

    /* 1 LSB: VREF/4096 = 0.625 mV, so 1 mV -> DOUT = 1*4096/2560 = 1 */
    qmp_adc128d818_set("ain1", 1);
    reading = i2c_get16(dev, REG_CH_READING_BASE + 1u);
    test_log("1 mV: raw 0x%04x (expect 0x0010)", reading);
    g_assert_cmphex(reading, ==, 0x0010);

    /* Quarter scale: 640 mV -> DOUT = 640*4096/2560 = 1024 = 0x400 */
    qmp_adc128d818_set("ain2", 640);
    reading = i2c_get16(dev, REG_CH_READING_BASE + 2u);
    test_log("640 mV (quarter): raw 0x%04x (expect 0x4000)", reading);
    g_assert_cmphex(reading, ==, 0x4000);

    /* Three-quarter scale: 1920 mV -> DOUT = 3072 = 0xC00 */
    qmp_adc128d818_set("ain3", 1920);
    reading = i2c_get16(dev, REG_CH_READING_BASE + 3u);
    test_log("1920 mV (3/4): raw 0x%04x (expect 0xC000)", reading);
    g_assert_cmphex(reading, ==, 0xC000);
}

/*
 * Test: Temperature conversion edge cases
 *
 * Format: 9-bit two's complement, 0.5 deg C per LSb, bits [15:7].
 *  +127.5 C = 255 * 500 mC = 127500 mC -> raw 0xFF, reading = 0x7F80
 *  -128.0 C = -256 * 500 mC = -128000 mC -> raw 0x100, reading = 0x8000
 *  0 C -> raw 0, reading = 0x0000
 *  -0.5 C = -500 mC -> raw = -1 = 0x1FF, reading = 0xFF80
 */
static void test_temperature_edges(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    i2c_set8(dev, REG_CONFIG, CONFIG_INITIALIZATION);

    /* 0 deg C */
    qmp_adc128d818_set("temperature", 0);
    i2c_set8(dev, REG_CONFIG, CONFIG_START);
    reading = i2c_get16(dev, REG_CH_READING_BASE + 7u);
    test_log("0 C: raw 0x%04x (expect 0x0000)", reading);
    g_assert_cmphex(reading, ==, 0x0000);

    /* -25 deg C = -50 * 500 mC, raw = -50 & 0x1FF = 0x1CE, << 7 = 0xE700 */
    qmp_adc128d818_set("temperature", -25000);
    reading = i2c_get16(dev, REG_CH_READING_BASE + 7u);
    test_log("-25 C: raw 0x%04x (expect 0xE700)", reading);
    g_assert_cmphex(reading, ==, 0xE700);

    /* +127.5 C (max positive): raw = 255, reading = 0x7F80 */
    qmp_adc128d818_set("temperature", 127500);
    reading = i2c_get16(dev, REG_CH_READING_BASE + 7u);
    test_log("+127.5 C: raw 0x%04x (expect 0x7F80)", reading);
    g_assert_cmphex(reading, ==, 0x7F80);

    /* -128 C (max negative): raw = -256, reading = 0x8000 */
    qmp_adc128d818_set("temperature", -128000);
    reading = i2c_get16(dev, REG_CH_READING_BASE + 7u);
    test_log("-128 C: raw 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);

    /* Clamp beyond max: 200 C -> clamped to +127.5 C */
    qmp_adc128d818_set("temperature", 200000);
    reading = i2c_get16(dev, REG_CH_READING_BASE + 7u);
    test_log("200 C (clamped): raw 0x%04x (expect 0x7F80)", reading);
    g_assert_cmphex(reading, ==, 0x7F80);

    /* Clamp beyond min: -200 C -> clamped to -128 C */
    qmp_adc128d818_set("temperature", -200000);
    reading = i2c_get16(dev, REG_CH_READING_BASE + 7u);
    test_log("-200 C (clamped): raw 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);
}

/*
 * Test: External voltage reference
 *
 * With ext-vref-mv = 4096 mV, the conversion becomes 1 mV per LSB:
 *   DOUT = ain * 4096 / 4096 = ain
 */
static void test_ext_vref(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    i2c_set8(dev, REG_CONFIG, CONFIG_INITIALIZATION);
    i2c_set8(dev, REG_ADV_CONFIG, ADV_CONFIG_MODE_1);

    qmp_adc128d818_set("ext-vref-mv", 4096);
    i2c_set8(dev, REG_ADV_CONFIG, ADV_CONFIG_EXT_REF_EN | ADV_CONFIG_MODE_1);

    /* 1000 mV with 4096 mV VREF -> DOUT = 1000, reading = 0x3E80 */
    qmp_adc128d818_set("ain0", 1000);
    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("1000 mV / 4096 mV VREF: raw 0x%04x (expect 0x3E80)", reading);
    g_assert_cmphex(reading, ==, 0x3E80);

    /* 2048 mV -> DOUT = 2048, reading = 0x8000 */
    qmp_adc128d818_set("ain1", 2048);
    reading = i2c_get16(dev, REG_CH_READING_BASE + 1u);
    test_log("2048 mV / 4096 mV VREF: raw 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);
}

/*
 * Test: One-shot conversion works in deep shutdown
 *
 * Per the datasheet, a one-shot write initiates a single conversion when the
 * device is in shutdown OR deep shutdown, after which it returns to that mode.
 * Continuous monitoring, however, does not run while (deep) shut down.
 */
static void test_deep_shutdown(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    i2c_set8(dev, REG_CONFIG, CONFIG_INITIALIZATION);

    /* First convert a known value */
    qmp_adc128d818_set("ain0", 1280);
    i2c_set8(dev, REG_CONFIG, CONFIG_START);
    reading = i2c_get16(dev, REG_CH_READING_BASE);
    g_assert_cmphex(reading, ==, 0x8000);

    /* Enter deep shutdown and change the input */
    i2c_set8(dev, REG_CONFIG, 0x00);
    i2c_set8(dev, REG_DEEP_SHUTDOWN, 0x01);
    qmp_adc128d818_set("ain0", 0);

    /*
     * The QOM input change must NOT auto-convert while deep shut down: the
     * reading still holds the previous value.
     */
    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("Deep shutdown, no one-shot: raw 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);

    /* A one-shot write still converts, even in deep shutdown */
    i2c_set8(dev, REG_ONE_SHOT, 0x01);
    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("Deep shutdown one-shot: raw 0x%04x (expect 0x0000)", reading);
    g_assert_cmphex(reading, ==, 0x0000);

    /* The device remains in deep shutdown after the one-shot */
    g_assert_cmphex(i2c_get8(dev, REG_DEEP_SHUTDOWN), ==, 0x01);

    /* Exit deep shutdown; one-shot in plain shutdown also works */
    i2c_set8(dev, REG_DEEP_SHUTDOWN, 0x00);
    qmp_adc128d818_set("ain0", 1280);
    i2c_set8(dev, REG_ONE_SHOT, 0x01);
    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("After exit shutdown: raw 0x%04x (expect 0x8000)", reading);
    g_assert_cmphex(reading, ==, 0x8000);
}

/*
 * Test: BUSY_STATUS NOT_READY clears after first conversion
 */
static void test_busy_status(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;

    i2c_set8(dev, REG_CONFIG, CONFIG_INITIALIZATION);

    /* After reset, NOT_READY (bit 1) should be set */
    g_assert_cmphex(i2c_get8(dev, REG_BUSY_STATUS) & 0x02, ==, 0x02);
    test_log("After reset: BUSY_STATUS = 0x%02x (NOT_READY set)",
             i2c_get8(dev, REG_BUSY_STATUS));

    /* Run a conversion */
    qmp_adc128d818_set("ain0", 0);
    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    /* NOT_READY should now be cleared */
    g_assert_cmphex(i2c_get8(dev, REG_BUSY_STATUS) & 0x02, ==, 0x00);
    test_log("After conversion: BUSY_STATUS = 0x%02x (NOT_READY cleared)",
             i2c_get8(dev, REG_BUSY_STATUS));
}

/*
 * Test: Low-limit interrupt triggers correctly
 *
 * The voltage interrupt condition is "reading > high limit OR reading <=
 * low limit"; the low-limit comparison is inclusive.
 */
static void test_low_limit(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint8_t status;

    i2c_set8(dev, REG_CONFIG, CONFIG_INITIALIZATION);

    /* Set ch1 low limit to 0x80 (MSB of reading must be > 0x80) */
    i2c_set8(dev, REG_LIMIT_BASE + 3u, 0x80);
    test_log("Set ch1 low limit = 0x80");

    /* Set ch2 low limit to 0x80 to exercise the inclusive (==) boundary */
    i2c_set8(dev, REG_LIMIT_BASE + 5u, 0x80);
    test_log("Set ch2 low limit = 0x80");

    /* ch1: 640 mV -> DOUT = 1024 = 0x400, MSB = 0x40 < 0x80 -> violation */
    qmp_adc128d818_set("ain1", 640);
    /* ch2: 1280 mV -> DOUT = 2048 = 0x800, MSB = 0x80 == 0x80 -> violation */
    qmp_adc128d818_set("ain2", 1280);
    /* ch0: 1280 mV -> MSB = 0x80, above its default low limit of 0x00 */
    qmp_adc128d818_set("ain0", 1280);
    i2c_set8(dev, REG_CONFIG, CONFIG_START | CONFIG_INT_ENABLE);

    status = i2c_get8(dev, REG_INT_STATUS);
    test_log("INT_STATUS = 0x%02x (expect bits 1 and 2 set)", status);
    g_assert_cmphex(status & 0x02u, ==, 0x02);
    g_assert_cmphex(status & 0x04u, ==, 0x04);

    /* ch0 is above its low limit and below its high limit -> no violation */
    g_assert_cmphex(status & 0x01u, ==, 0x00);
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

/* Test: Programming Channel Disable resets channel readings */
static void test_chan_disable_clears(void *obj, void *data,
                                     QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    i2c_set8(dev, REG_CONFIG, CONFIG_INITIALIZATION);

    /* Convert a non-zero value on ch0 */
    qmp_adc128d818_set("ain0", 1280);
    i2c_set8(dev, REG_CONFIG, CONFIG_START);
    g_assert_cmphex(i2c_get16(dev, REG_CH_READING_BASE), ==, 0x8000);

    /*
     * Stop, then program Channel Disable. Even ch0, which stays enabled, has
     * its reading reset to the default (section 8.6.6).
     */
    i2c_set8(dev, REG_CONFIG, 0x00);
    i2c_set8(dev, REG_CH_DISABLE, 0x02);
    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("After CH_DISABLE write: ch0 raw 0x%04x (expect 0x0000)", reading);
    g_assert_cmphex(reading, ==, 0x0000);
}

/*
 * Test: Programming Advanced Configuration always resets channel readings
 *
 * The reset happens whenever the register is written, even if the mode bits
 * are unchanged or only the external-reference bit toggles (section 8.6.9).
 */
static void test_adv_config_clears(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint16_t reading;

    i2c_set8(dev, REG_CONFIG, CONFIG_INITIALIZATION);

    /* Mode 0 (default): convert a value */
    qmp_adc128d818_set("ain0", 1280);
    i2c_set8(dev, REG_CONFIG, CONFIG_START);
    g_assert_cmphex(i2c_get16(dev, REG_CH_READING_BASE), ==, 0x8000);

    /* Rewrite the SAME mode (mode 0): readings still reset */
    i2c_set8(dev, REG_CONFIG, 0x00);
    i2c_set8(dev, REG_ADV_CONFIG, 0x00);
    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("After same-mode ADV_CONFIG: ch0 0x%04x (expect 0x0000)", reading);
    g_assert_cmphex(reading, ==, 0x0000);

    /* Convert again, then toggle only the external-reference bit */
    i2c_set8(dev, REG_CONFIG, CONFIG_START);
    g_assert_cmphex(i2c_get16(dev, REG_CH_READING_BASE), ==, 0x8000);
    i2c_set8(dev, REG_CONFIG, 0x00);
    qmp_adc128d818_set("ext-vref-mv", 4096);
    i2c_set8(dev, REG_ADV_CONFIG, ADV_CONFIG_EXT_REF_EN);
    reading = i2c_get16(dev, REG_CH_READING_BASE);
    test_log("After ext-vref toggle: ch0 0x%04x (expect 0x0000)", reading);
    g_assert_cmphex(reading, ==, 0x0000);
}

/*
 * Test: Temperature high-limit interrupt with hysteresis
 *
 * The alarm asserts when the reading > Thot and keeps re-asserting on every
 * conversion until the reading falls to <= Thot_hyst (section 9.1.3.3).
 */
static void test_temp_hysteresis(void *obj, void *data,
                                 QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;
    uint8_t status;

    i2c_set8(dev, REG_CONFIG, CONFIG_INITIALIZATION);

    /* Thot = +50 C (0x32), Thot_hyst = +40 C (0x28) */
    i2c_set8(dev, REG_LIMIT_BASE + 7u * 2u, 0x32);
    i2c_set8(dev, REG_LIMIT_BASE + 7u * 2u + 1u, 0x28);

    qmp_adc128d818_set("temperature", 25000);
    i2c_set8(dev, REG_CONFIG, CONFIG_START);

    /* 25 C < Thot: no temperature alarm */
    status = i2c_get8(dev, REG_INT_STATUS);
    test_log("25 C: INT_STATUS = 0x%02x (temp bit expect clear)", status);
    g_assert_cmphex(status & 0x80u, ==, 0x00);

    /* 55 C > Thot: alarm asserts */
    qmp_adc128d818_set("temperature", 55000);
    status = i2c_get8(dev, REG_INT_STATUS);
    test_log("55 C: INT_STATUS = 0x%02x (temp bit expect set)", status);
    g_assert_cmphex(status & 0x80u, ==, 0x80);

    /* 45 C: between Thot_hyst and Thot, alarm re-asserts after the read */
    qmp_adc128d818_set("temperature", 45000);
    status = i2c_get8(dev, REG_INT_STATUS);
    test_log("45 C (hysteresis): INT_STATUS = 0x%02x (temp bit expect set)",
             status);
    g_assert_cmphex(status & 0x80u, ==, 0x80);

    /* 35 C <= Thot_hyst: alarm clears */
    qmp_adc128d818_set("temperature", 35000);
    status = i2c_get8(dev, REG_INT_STATUS);
    test_log("35 C: INT_STATUS = 0x%02x (temp bit expect clear)", status);
    g_assert_cmphex(status & 0x80u, ==, 0x00);
}

/*
 * Test: Conversion Rate register may only be programmed while in shutdown
 */
static void test_conv_rate(void *obj, void *data, QGuestAllocator *alloc)
{
    QI2CDevice *dev = (QI2CDevice *)obj;

    i2c_set8(dev, REG_CONFIG, CONFIG_INITIALIZATION);

    /* Selectable while stopped */
    i2c_set8(dev, REG_CONV_RATE, 0x01);
    g_assert_cmphex(i2c_get8(dev, REG_CONV_RATE), ==, 0x01);

    /* Once started, writes are ignored */
    i2c_set8(dev, REG_CONFIG, CONFIG_START);
    i2c_set8(dev, REG_CONV_RATE, 0x00);
    test_log("CONV_RATE while running: 0x%02x (expect unchanged 0x01)",
             i2c_get8(dev, REG_CONV_RATE));
    g_assert_cmphex(i2c_get8(dev, REG_CONV_RATE), ==, 0x01);
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
    qos_add_test("all-channels", "adc128d818", test_all_channels, NULL);
    qos_add_test("voltage-edges", "adc128d818", test_voltage_edges, NULL);
    qos_add_test("temperature-edges", "adc128d818", test_temperature_edges,
                 NULL);
    qos_add_test("ext-vref", "adc128d818", test_ext_vref, NULL);
    qos_add_test("deep-shutdown", "adc128d818", test_deep_shutdown, NULL);
    qos_add_test("busy-status", "adc128d818", test_busy_status, NULL);
    qos_add_test("low-limit", "adc128d818", test_low_limit, NULL);
    qos_add_test("chan-disable-clears", "adc128d818", test_chan_disable_clears,
                 NULL);
    qos_add_test("adv-config-clears", "adc128d818", test_adv_config_clears,
                 NULL);
    qos_add_test("temp-hysteresis", "adc128d818", test_temp_hysteresis, NULL);
    qos_add_test("conv-rate", "adc128d818", test_conv_rate, NULL);
    qos_add_test("ain-property", "adc128d818", test_ain_property, NULL);
}
libqos_init(adc128d818_register_nodes);
