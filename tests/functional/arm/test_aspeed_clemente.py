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


class ClementeMachine(AspeedTest):

    # TODO: the boot image currently lives on a personal fork; move it under
    # legoater/qemu-aspeed-boot before upstreaming.
    ASSET_CLEMENTE_FLASH = Asset(
        'https://github.com/eblot/qemu-aspeed-boot/raw/refs/heads/clemente-bmc/images/clemente-bmc/openbmc-20260806025108/'
            'obmc-phosphor-image-clemente-20260806025108.static.mtd.xz',
        'a8df75515a412696d3d51c573f2de5806702c5e19349f4c712d21342b2b52b51')

    # The OCP NIC TMP421 sensors sit directly on i2c10 and i2c15 at address
    # 0x1f; on each bus the sensor is the first slave instantiated (child[1],
    # after the controller's own child[0]). They enumerate in the guest as
    # devices 10-001f and 15-001f.
    NIC0_QOM_PATH = "/machine/soc/i2c/bus[10]/aspeed.i2c.bus.10/child[1]"
    NIC0_HWMON = "/sys/bus/i2c/devices/10-001f/hwmon/hwmon*"
    NIC1_QOM_PATH = "/machine/soc/i2c/bus[15]/aspeed.i2c.bus.15/child[1]"
    NIC1_HWMON = "/sys/bus/i2c/devices/15-001f/hwmon/hwmon*"
    PROMPT = "root@clemente:~#"

    def test_arm_ast2600_clemente_openbmc(self):
        image_path = self.uncompress(self.ASSET_CLEMENTE_FLASH)

        self.do_test_arm_aspeed_openbmc('clemente-bmc', image=image_path,
                                        uboot='2019.04', cpu_id='0xf00',
                                        soc='AST2600 rev A3')

        exec_command_and_wait_for_pattern(self, 'root', 'Password:')
        exec_command_and_wait_for_pattern(self, '0penBmc', self.PROMPT)

        # temperature0 is the local channel (hwmon temp1_input), temperature1
        # the first remote channel (temp2_input), both in millidegrees C. The
        # TMP421 resolution is 1/16 C, so a set point reads back quantised to
        # the nearest LSB (e.g. 30.000 C -> 29.938 C).
        for qom, hwmon in ((self.NIC0_QOM_PATH, self.NIC0_HWMON),
                           (self.NIC1_QOM_PATH, self.NIC1_HWMON)):
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
