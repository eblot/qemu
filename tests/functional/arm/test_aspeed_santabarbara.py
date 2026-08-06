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


class SantabarbaraMachine(AspeedTest):

    # TODO: the boot image currently lives on a personal fork; move it under
    # legoater/qemu-aspeed-boot before upstreaming.
    ASSET_SANTABARBARA_FLASH = Asset(
        'https://github.com/eblot/qemu-aspeed-boot/raw/refs/heads/santabarbara-bmc/images/santabarbara-bmc/openbmc-20260806025102/'
            'obmc-phosphor-image-santabarbara-20260806025102.static.mtd.xz',
        '7a330a0536ea6a5c7c7a517f48fcd9a4717128b4f5d71ed8f6e8722a51a6cabe')

    # The OCP NIC TMP421 sits directly on i2c11 at address 0x1f, the first
    # slave instantiated on the bus (child[1], after the controller's own
    # child[0]); it enumerates in the guest as device 11-001f.
    TMP421_QOM_PATH = "/machine/soc/i2c/bus[11]/aspeed.i2c.bus.11/child[1]"
    TMP421_HWMON = "/sys/bus/i2c/devices/11-001f/hwmon/hwmon*"

    # An ADC128D818 lives on the first "Rainbow" module (i2c6), behind the
    # PCA9546 at 0x71 (child[2]) on mux channel 3, at address 0x1d. That
    # channel is device-tree alias i2c51, so the guest sees it as 51-001d.
    ADC_QOM_PATH = ("/machine/soc/i2c/bus[6]/aspeed.i2c.bus.6"
                    "/child[2]/i2c.3/child[0]")
    ADC_HWMON = "/sys/bus/i2c/devices/51-001d/hwmon/hwmon*"
    PROMPT = "root@santabarbara:~#"

    def test_arm_ast2600_santabarbara_openbmc(self):
        image_path = self.uncompress(self.ASSET_SANTABARBARA_FLASH)

        self.do_test_arm_aspeed_openbmc('santabarbara-bmc', image=image_path,
                                        uboot='2019.04', cpu_id='0xf00',
                                        soc='AST2600 rev A3')

        exec_command_and_wait_for_pattern(self, 'root', 'Password:')
        exec_command_and_wait_for_pattern(self, '0penBmc', self.PROMPT)

        self.assertIn(b"tmp421", self.read_hwmon(self.TMP421_HWMON, "name"))

        # temperature0 is the local channel (hwmon temp1_input), temperature1
        # the first remote channel (temp2_input), both in millidegrees C. The
        # TMP421 resolution is 1/16 C, so a set point reads back quantised to
        # the nearest LSB (e.g. 30.000 C -> 29.938 C).
        for (local_mc, local_rb), (remote_mc, remote_rb) in \
                (((30000, 29938), (55000, 54938)),
                 ((25000, 24938), (45000, 44938))):
            self.vm.cmd("qom-set", path=self.TMP421_QOM_PATH,
                        property="temperature0", value=local_mc)
            self.vm.cmd("qom-set", path=self.TMP421_QOM_PATH,
                        property="temperature1", value=remote_mc)
            self.wait_hwmon_value(self.TMP421_HWMON, "temp1_input", local_rb)
            self.wait_hwmon_value(self.TMP421_HWMON, "temp2_input", remote_rb)

        self.assertIn(b"adc128d818", self.read_hwmon(self.ADC_HWMON, "name"))

        # ainN sets analog input channel N in mV; the driver exposes it as the
        # zero-based inN_input. With the internal 2560 mV reference and 12-bit
        # resolution, values that are multiples of the LSB read back exactly.
        for ain0_mv, ain1_mv in ((1000, 2000), (500, 1500)):
            self.vm.cmd("qom-set", path=self.ADC_QOM_PATH,
                        property="ain0", value=ain0_mv)
            self.vm.cmd("qom-set", path=self.ADC_QOM_PATH,
                        property="ain1", value=ain1_mv)
            self.wait_hwmon_value(self.ADC_HWMON, "in0_input", ain0_mv)
            self.wait_hwmon_value(self.ADC_HWMON, "in1_input", ain1_mv)


if __name__ == '__main__':
    AspeedTest.main()
