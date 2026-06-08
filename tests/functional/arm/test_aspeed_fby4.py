#!/usr/bin/env python3
#
# Functional test that boots the ASPEED machines
#
# SPDX-License-Identifier: GPL-2.0-or-later

import re
import time

from qemu_test import Asset
from qemu_test import wait_for_console_pattern
from aspeed import AspeedTest

from qemu_test import wait_for_console_pattern
from qemu_test import exec_command
from qemu_test import exec_command_and_wait_for_pattern

class YosemiteV4Machine(AspeedTest):

    ASSET_YOSEMITE_V4_FLASH = Asset(
        'https://github.com/legoater/qemu-aspeed-boot/raw/refs/heads/master/images/yosemite4-bmc/openbmc-20260505132843/obmc-phosphor-image-yosemite4-20260505132843.static.mtd.xz',
        'dff6946363b41f952b15cfc3156482b89fcfc1b0ecfc3ec8b3ed496a5f001ef9')

    ADC128D818_HWMON = '/sys/bus/i2c/devices/30-001f/hwmon/hwmon*'

    def do_test_arm_aspeed_openbmc_no_network(self, machine, image, uboot,
                                   cpu_id, soc):

        self.set_machine(machine)
        self.vm.set_console()
        self.vm.add_args('-drive', f'file={image},if=mtd,format=raw',
                         '-snapshot')
        self.vm.launch()

        self.wait_for_console_pattern(f'U-Boot {uboot}')
        self.wait_for_console_pattern('## Loading kernel from FIT Image')
        self.wait_for_console_pattern('Starting kernel ...')
        self.wait_for_console_pattern(f'Booting Linux on physical CPU {cpu_id}')
        self.wait_for_console_pattern(f'ASPEED {soc}')
        self.wait_for_console_pattern('/init as init process')
        # yosemite v4 does not emit the hostname log which is
        # different from the other machines.
        self.wait_for_console_pattern('yosemite4 login:')

        # perform login
        exec_command_and_wait_for_pattern(self,
                                          "root", "Password:");

        exec_command_and_wait_for_pattern(self, "0penBmc", "#");

        # MAX31790 test
        exec_command_and_wait_for_pattern(self,
            "cat /sys/class/hwmon/hwmon2/name", "max31790");
        exec_command_and_wait_for_pattern(self,
            "cat /sys/class/hwmon/hwmon2/fan1_input", "7447");
        exec_command_and_wait_for_pattern(self,
            "cat /sys/class/hwmon/hwmon2/fan1_enable", "1");
        exec_command_and_wait_for_pattern(self,
            "cat /sys/class/hwmon/hwmon2/fan1_fault", "0");
        exec_command_and_wait_for_pattern(self,
            "echo 1 > /sys/class/hwmon/hwmon2/pwm1", "root@yosemite4");
        exec_command_and_wait_for_pattern(self,
            "cat /sys/class/hwmon/hwmon2/fan1_input", "140");
        exec_command_and_wait_for_pattern(self,
            "echo 255 > /sys/class/hwmon/hwmon2/pwm1", "root@yosemite4");
        exec_command_and_wait_for_pattern(self,
            "cat /sys/class/hwmon/hwmon2/fan1_input", "10685");

        # MAX11615 test
        exec_command_and_wait_for_pattern(self,
            "cat /sys/bus/i2c/devices/30-0033/iio:device*/name", "max11615");
        exec_command_and_wait_for_pattern(self,
            "cat /sys/bus/i2c/devices/30-0033/iio:device*/in_voltage0_raw", "4095");
        exec_command_and_wait_for_pattern(self,
            "cat /sys/bus/i2c/devices/30-0033/iio:device*/in_voltage_scale", "0.500000000");

        # ADC128D818 test
        exec_command_and_wait_for_pattern(self,
            f"cat {self.ADC128D818_HWMON}/name", "adc128d818");

        adc = self.find_adc128d818_qom_path()
        for ch0_mv, ch1_mv in ((108, 2000), (1280, 500)):
            self.vm.cmd('qom-set', path=adc, property='ain0', value=ch0_mv)
            self.vm.cmd('qom-set', path=adc, property='ain1', value=ch1_mv)
            self.wait_adc128d818_value('in0_input', ch0_mv)
            self.wait_adc128d818_value('in1_input', ch1_mv)

        self.assertEqual(self.read_adc128d818_value('in0_min'), 0)
        self.assertEqual(self.read_adc128d818_value('in0_max'), 2551)

    def find_adc128d818_qom_path(self):
        unattached = '/machine/unattached'
        devices = [child['name'] for child in
                   self.vm.cmd('qom-list', path=unattached)
                   if 'adc128d818' in child['type']]
        devices.sort(key=lambda name: int(name[len('device['):-1]))
        return f'{unattached}/{devices[0]}'

    def read_adc128d818_value(self, attr):
        # split the marker via $m so it only appears in the output, not echo
        exec_command(self,
            f'm=END; cat {self.ADC128D818_HWMON}/{attr}; echo "ADC$m"')
        out = wait_for_console_pattern(self, 'ADCEND')
        match = re.search(rb'(-?\d+)\s+ADCEND', out)
        self.assertIsNotNone(match, f'could not read {attr}')
        return int(match.group(1))

    def wait_adc128d818_value(self, attr, expected, timeout=20):
        deadline = time.monotonic() + timeout
        value = None
        while time.monotonic() < deadline:
            value = self.read_adc128d818_value(attr)
            if value == expected:
                return
            time.sleep(2)
        self.fail(f'{attr} did not reach {expected} (last read {value})')

    def test_arm_ast2600_yosemitev4_openbmc(self):
        image_path = self.uncompress(self.ASSET_YOSEMITE_V4_FLASH)

        self.do_test_arm_aspeed_openbmc_no_network('fby4-bmc', image=image_path,
                                        uboot='2019.04', cpu_id='0xf00',
                                        soc='AST2600 rev A3')

if __name__ == '__main__':
    AspeedTest.main()
