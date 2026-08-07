/*
 * Facebook Bletchley BMC
 *
 * Copyright (c) 2022 Meta Platforms, Inc. and affiliates
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Two board revisions are modelled, selected with the "board-revision"
 * machine property. They carry the same devices and differ only in their
 * FRU content.
 *
 * Current limitations: the following devices from the device tree have no
 * QEMU model and are therefore not emulated:
 *   - mps,mp5023 power monitors
 *   - fcs,fusb302 USB Type-C PD controllers
 *   - adi,adm1278 hot-swap controller
 *   - infineon,slb9670 TPM (SPI)
 *   - ipmb-dev IPMB interface
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/aspeed.h"
#include "hw/arm/aspeed_soc.h"
#include "hw/gpio/pca9552.h"
#include "hw/gpio/pca9554.h"
#include "hw/nvram/eeprom_at24c.h"
#include "hw/rtc/ds1338.h"
#include "hw/sensor/ina230.h"
#include "hw/sensor/tmp421.h"

/* Bletchley hardware value */
#define BLETCHLEY_BMC_HW_STRAP1 0x00002000
#define BLETCHLEY_BMC_HW_STRAP2 0x00000801
#define BLETCHLEY_BMC_RAM_SIZE ASPEED_RAM_SIZE(2 * GiB)

#define TYPE_BLETCHLEY_MACHINE MACHINE_TYPE_NAME("bletchley-bmc")
OBJECT_DECLARE_SIMPLE_TYPE(BletchleyMachineState, BLETCHLEY_MACHINE)

struct BletchleyMachineState {
    AspeedMachineState parent_obj;

    bool v1_5;
};

static void bletchley_bmc_i2c_init(AspeedMachineState *bmc)
{
    AspeedSoCState *soc = bmc->soc;
    I2CBus *i2c[13] = {};
    for (int i = 0; i < 13; i++) {
        if ((i == 8) || (i == 11)) {
            continue;
        }
        i2c[i] = aspeed_i2c_get_bus(&soc->i2c, i);
    }

    /* Bus 0 - 5 all have the same config. */
    for (int i = 0; i < 6; i++) {
        i2c_slave_create_simple(i2c[i], TYPE_INA230, 0x45);
        /* Missing model: mps,mp5023 @ 0x40 */
        i2c_slave_create_simple(i2c[i], TYPE_TMP421, 0x4f);
        i2c_slave_create_simple(i2c[i], TYPE_PCA9536, 0x41);
        i2c_slave_create_simple(i2c[i], TYPE_PCA9539, 0x76);
        i2c_slave_create_simple(i2c[i], TYPE_PCA9552, 0x67);
        /* Missing model: fsc,fusb302 @ 0x22 */
        at24c_eeprom_init(i2c[i], 0x54, 8 * KiB);
    }

    /* Bus 6 */
    at24c_eeprom_init(i2c[6], 0x56, 8 * KiB);
    /* Missing model: nxp,pcf85263 @ 0x51 , but ds1338 works enough */
    i2c_slave_create_simple(i2c[6], TYPE_DS1338, 0x51);


    /* Bus 7 */
    at24c_eeprom_init(i2c[7], 0x54, 8 * KiB);

    /* Bus 9 */
    i2c_slave_create_simple(i2c[9], TYPE_TMP421, 0x4f);

    /* Bus 10 */
    i2c_slave_create_simple(i2c[10], TYPE_TMP421, 0x4f);
    i2c_slave_create_simple(i2c[10], TYPE_PCA9552, 0x67);

    /* Bus 12 */
    /* Missing model: adi,adm1278 @ 0x11 */
    i2c_slave_create_simple(i2c[12], TYPE_TMP421, 0x4c);
    i2c_slave_create_simple(i2c[12], TYPE_TMP421, 0x4d);
    i2c_slave_create_simple(i2c[12], TYPE_PCA9552, 0x67);

    /* Bus 13 */
    /* Missing model: ipmb-dev @ 0x10 */
}

static char *bletchley_get_board_revision(Object *obj, Error **errp)
{
    BletchleyMachineState *bmc = BLETCHLEY_MACHINE(obj);

    return g_strdup(bmc->v1_5 ? "1.5" : "1.0");
}

static void bletchley_set_board_revision(Object *obj, const char *value,
                                         Error **errp)
{
    BletchleyMachineState *bmc = BLETCHLEY_MACHINE(obj);

    if (!strcmp(value, "1.0")) {
        bmc->v1_5 = false;
    } else if (!strcmp(value, "1.5")) {
        bmc->v1_5 = true;
    } else {
        error_setg(errp, "Bad value for \"board-revision\" property, "
                   "expected \"1.0\" or \"1.5\"");
    }
}

static void aspeed_machine_bletchley_class_init(ObjectClass *oc,
                                                const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AspeedMachineClass *amc = ASPEED_MACHINE_CLASS(oc);

    mc->desc       = "Facebook Bletchley BMC (Cortex-A7)";
    amc->soc_name  = "ast2600-a3";
    amc->hw_strap1 = BLETCHLEY_BMC_HW_STRAP1;
    amc->hw_strap2 = BLETCHLEY_BMC_HW_STRAP2;
    amc->fmc_model = "w25q01jvq";
    amc->spi_model = NULL;
    amc->num_cs    = 2;
    amc->macs_mask = ASPEED_MAC2_ON;
    amc->i2c_init  = bletchley_bmc_i2c_init;
    mc->default_ram_size = BLETCHLEY_BMC_RAM_SIZE;
    aspeed_machine_class_init_cpus_defaults(mc);

    object_class_property_add_str(oc, "board-revision",
                                  bletchley_get_board_revision,
                                  bletchley_set_board_revision);
    object_class_property_set_description(oc, "board-revision",
                           "Board revision, \"1.0\" (default) or \"1.5\"");
}

static const TypeInfo aspeed_ast2600_bletchley_types[] = {
    {
        .name          = TYPE_BLETCHLEY_MACHINE,
        .parent        = TYPE_ASPEED_MACHINE,
        .instance_size = sizeof(BletchleyMachineState),
        .class_init    = aspeed_machine_bletchley_class_init,
        .interfaces    = arm_machine_interfaces,
    }
};

DEFINE_TYPES(aspeed_ast2600_bletchley_types)
