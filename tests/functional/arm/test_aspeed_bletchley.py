#!/usr/bin/env python3
#
# Functional test that boots the ASPEED machines
#
# SPDX-License-Identifier: GPL-2.0-or-later

from qemu_test import Asset
from qemu_test import exec_command_and_wait_for_pattern
from aspeed import AspeedTest


class BletchleyMachine(AspeedTest):

    ASSET_BLETCHLEY_FLASH = Asset(
        'https://github.com/legoater/qemu-aspeed-boot/raw/master/images/bletchley-bmc/openbmc-20250128071329/obmc-phosphor-image-bletchley-20250128071329.static.mtd.xz',
        'db21d04d47d7bb2a276f59d308614b4dfb70b9c7c81facbbca40a3977a2d8844')

    # The sled INA230 power monitor is the first slave instantiated on i2c0,
    # hence child[1] (child[0] is the controller's own slave); it enumerates
    # in the guest as device 0-0045.
    INA230_QOM_PATH = "/machine/soc/i2c/bus[0]/aspeed.i2c.bus.0/child[1]"
    INA230_HWMON = "/sys/bus/i2c/devices/0-0045/hwmon/hwmon*"

    # The two front-panel TMP421 sensors are the first slaves instantiated on
    # i2c12, at 0x4c and 0x4d, hence child[1] and child[2].
    TMP421_0_QOM_PATH = "/machine/soc/i2c/bus[12]/aspeed.i2c.bus.12/child[1]"
    TMP421_0_HWMON = "/sys/bus/i2c/devices/12-004c/hwmon/hwmon*"
    TMP421_1_QOM_PATH = "/machine/soc/i2c/bus[12]/aspeed.i2c.bus.12/child[2]"
    TMP421_1_HWMON = "/sys/bus/i2c/devices/12-004d/hwmon/hwmon*"
    PROMPT = "root@bletchley:~#"

    def test_arm_ast2600_bletchley_openbmc(self):
        image_path = self.uncompress(self.ASSET_BLETCHLEY_FLASH)

        self.do_test_arm_aspeed_openbmc('bletchley-bmc', image=image_path,
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

        # temperature0 is the local channel (hwmon temp1_input), temperature1
        # the first remote channel (temp2_input), both in millidegrees C. The
        # TMP421 resolution is 1/16 C, so a set point reads back quantised to
        # the nearest LSB (e.g. 30.000 C -> 29.938 C).
        for qom, hwmon in ((self.TMP421_0_QOM_PATH, self.TMP421_0_HWMON),
                           (self.TMP421_1_QOM_PATH, self.TMP421_1_HWMON)):
            self.assertIn(b"tmp421", self.read_hwmon(hwmon, "name"))

            for (local_mc, local_rb), (remote_mc, remote_rb) in \
                    (((30000, 29938), (55000, 54938)),
                     ((25000, 24938), (45000, 44938))):
                self.vm.cmd("qom-set", path=qom,
                            property="temperature0", value=local_mc)
                self.vm.cmd("qom-set", path=qom,
                            property="temperature1", value=remote_mc)
                self.wait_hwmon_value(hwmon, "temp1_input", local_rb)
                self.wait_hwmon_value(hwmon, "temp2_input", remote_rb)


if __name__ == '__main__':
    AspeedTest.main()
