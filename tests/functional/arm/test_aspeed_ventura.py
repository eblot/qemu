#!/usr/bin/env python3
#
# Functional test that boots the ASPEED machines
#
# Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
#
# SPDX-License-Identifier: GPL-2.0-or-later

from qemu_test import Asset
from qemu_test import exec_command_and_wait_for_pattern
from aspeed import AspeedTest


class VenturaMachine(AspeedTest):

    # TODO: the boot image currently lives on a personal fork; move it under
    # legoater/qemu-aspeed-boot before upstreaming.
    ASSET_VENTURA_FLASH = Asset(
        'https://github.com/eblot/qemu-aspeed-boot/raw/refs/heads/ventura-bmc/images/ventura-bmc/openbmc-20260807025044/'
            'obmc-phosphor-image-ventura-20260807025044.static.mtd.xz',
        'b9c097d2f09d5e074bf41d57f2bcf03adb010cbb9556e48d431fbb19791fafc3')

    PROMPT = "root@ventura:~#"

    # The only device tree in this image's FIT is the AST2600 EVB one, so
    # the guest never probes the board I2C topology: no FRU EEPROM, sensor
    # or I/O expander shows up under /sys/bus/i2c. Until an image carrying
    # the board device tree is published this covers the boot path only —
    # flash, RAM, SoC and console. Add the device assertions then.
    def test_arm_ast2600_ventura_openbmc(self):
        image_path = self.uncompress(self.ASSET_VENTURA_FLASH)

        self.do_test_arm_aspeed_openbmc('ventura-bmc', image=image_path,
                                        uboot='2019.04', cpu_id='0xf00',
                                        soc='AST2600 rev A3')

        exec_command_and_wait_for_pattern(self, 'root', 'Password:')
        exec_command_and_wait_for_pattern(self, '0penBmc', self.PROMPT)


if __name__ == '__main__':
    AspeedTest.main()
