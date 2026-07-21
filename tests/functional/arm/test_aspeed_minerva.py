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


class MinervaMachine(AspeedTest):

    # TODO: the boot image currently lives on a personal fork; move it under
    # legoater/qemu-aspeed-boot before upstreaming.
    ASSET_MINERVA_FLASH = Asset(
        'https://github.com/eblot/qemu-aspeed-boot/raw/refs/heads/minerva-bmc/images/minerva-bmc/openbmc-20260720025047/'
            'obmc-phosphor-image-minerva-20260720025047.static.mtd.xz',
        'bd713df39d3c09846299a2377315465187a4dcd33c29d3b3d683ef13b59f36bb')

    # The INA230 at i2c0 address 0x40 is the first slave instantiated on the
    # bus, hence child[1] (child[0] is the controller's own slave).
    INA230_QOM_PATH = "/machine/soc/i2c/bus[0]/aspeed.i2c.bus.0/child[1]"
    INA230_HWMON = "/sys/bus/i2c/devices/0-0040/hwmon/hwmon*"

    # The FCB INA238 power monitors sit behind the i2c2 PCA9548 mux. FCB 1 is
    # mux channel 1 (aliased i2c16 in the device tree); on that channel bus the
    # eeprom@50 is child[0] and the 0x40 monitor is the next slave, child[1].
    # In the guest it therefore enumerates as bus 16, device 16-0040.
    INA238_QOM_PATH = ("/machine/soc/i2c/bus[2]/aspeed.i2c.bus.2"
                       "/child[1]/i2c.1/child[1]")
    INA238_HWMON = "/sys/bus/i2c/devices/16-0040/hwmon/hwmon*"
    PROMPT = "root@minerva:~#"

    def test_arm_ast2600_minerva_openbmc(self):
        image_path = self.uncompress(self.ASSET_MINERVA_FLASH)

        self.do_test_arm_aspeed_openbmc('minerva-bmc', image=image_path,
                                        uboot='2019.04', cpu_id='0xf00',
                                        soc='AST2600 rev A3')

        exec_command_and_wait_for_pattern(self, 'root', 'Password:')
        exec_command_and_wait_for_pattern(self, '0penBmc', self.PROMPT)

        self.assertIn(b"ina230", self.read_hwmon(self.INA230_HWMON, "name"))

        # in0_input reports the shunt voltage in mV (2.5 uV/LSB), in1_input
        # the bus voltage in mV (1.25 mV/LSB). Drive both through QOM and
        # check the kernel hwmon interface reports each independently.
        for shunt_nv, bus_uv in ((10000000, 12000000), (40000000, 3300000)):
            self.vm.cmd("qom-set", path=self.INA230_QOM_PATH,
                        property="shunt-voltage", value=shunt_nv)
            self.vm.cmd("qom-set", path=self.INA230_QOM_PATH,
                        property="bus-voltage", value=bus_uv)
            self.wait_hwmon_value(self.INA230_HWMON, "in0_input",
                                  shunt_nv // 1000000)
            self.wait_hwmon_value(self.INA230_HWMON, "in1_input",
                                  bus_uv // 1000)

        self.assertIn(b"ina238", self.read_hwmon(self.INA238_HWMON, "name"))

        # in1_input reports the bus voltage in mV (3.125 mV/LSB) and temp1_input
        # the die temperature in millidegrees C (125 m-degC/LSB). Drive both
        # through QOM and check they read back through the kernel
        # hwmon interface.
        for bus_uv, temp_mc in ((12000000, 30000), (3300000, 55000)):
            self.vm.cmd("qom-set", path=self.INA238_QOM_PATH,
                        property="bus-voltage", value=bus_uv)
            self.vm.cmd("qom-set", path=self.INA238_QOM_PATH,
                        property="die-temperature", value=temp_mc)
            self.wait_hwmon_value(self.INA238_HWMON, "in1_input",
                                  bus_uv // 1000)
            self.wait_hwmon_value(self.INA238_HWMON, "temp1_input", temp_mc)


if __name__ == '__main__':
    AspeedTest.main()
