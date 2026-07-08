#!/usr/bin/env python3
#
# Functional test that boots the ASPEED machines
#
# Copyright (c) 2026 Meta Platforms, Inc. and affiliates.
#
# SPDX-License-Identifier: GPL-2.0-or-later

from qemu_test import Asset
from qemu_test import exec_command_and_wait_for_pattern
from aspeed import FacebookAspeedTest


class CatalinaMachine(FacebookAspeedTest):

    ASSET_CATALINA_FLASH = Asset(
        'https://github.com/legoater/qemu-aspeed-boot/raw/a866feb5ef81245b4827a214584bf6bcc72939f6/images/catalina-bmc/obmc-phosphor-image-catalina-20250619123021.static.mtd.xz',
        '287402e1ba021991e06be1d098f509444a02a3d81a73a932f66528b159e864f9')

    PROMPT = "root@catalina:~#"

    # io_expander0: pca9555@20 on i2c2, a 16-bit expander directly on the bus.
    IOEXP0_QOM = "/machine/soc/i2c/bus[2]/aspeed.i2c.bus.2/0x20"
    IOEXP0_GPIODETECT = r"\[2-0020\]"

    def test_arm_ast2600_catalina_openbmc(self):
        image_path = self.uncompress(self.ASSET_CATALINA_FLASH)

        self.do_test_arm_aspeed_openbmc('catalina-bmc', image=image_path,
                                        uboot='2019.04', cpu_id='0xf00',
                                        soc='AST2600 rev A3')

        exec_command_and_wait_for_pattern(self, 'root', 'Password:')
        exec_command_and_wait_for_pattern(self, '0penBmc', self.PROMPT)

        # pca9555@20 spans two 8-bit ports (pins 0-7 and 8-15).
        self.check_ioexp(self.IOEXP0_QOM, self.IOEXP0_GPIODETECT, (0, 7, 8, 15))

    def check_ioexp(self, qom, gpiodetect, pins):
        # The guest uses these expander lines as inputs (with pull-ups), so
        # this only exercises the input direction: set_pin injects an external
        # level and get_pin reports it back from the input register. get_pin
        # is not checked against a chip-driven output value because the guest
        # never configures these lines as outputs; that path (the input
        # register mirroring the OUTPUT register) is covered by the qtests,
        # which read the same register get_pin returns. set_pin is a no-op on
        # an output-configured pin, so a passing round-trip also confirms the
        # lines really are inputs.
        chip = self.resolve_gpiochip(gpiodetect)
        for pin in pins:
            self.set_pin(qom, pin, "low")
            self.assert_pin(qom, pin, "low")
            self.assert_gpio_line(chip, pin, 0)
            self.set_pin(qom, pin, "high")
            self.assert_pin(qom, pin, "high")
            self.assert_gpio_line(chip, pin, 1)

        # Drive two pins to opposite levels at once to confirm they are
        # reported back independently.
        lo, hi = pins[0], pins[-1]
        self.set_pin(qom, lo, "low")
        self.set_pin(qom, hi, "high")
        self.assert_pin(qom, lo, "low")
        self.assert_pin(qom, hi, "high")
        self.assert_gpio_line(chip, lo, 0)
        self.assert_gpio_line(chip, hi, 1)

    def read_catalina_console(self, command):
        return exec_command_and_wait_for_pattern(self, command, self.PROMPT)

    def resolve_gpiochip(self, gpiodetect):
        # awk already prints the "gpiochipN" name; just pick it out of the
        # console output (the command echo and prompt hold no such token).
        out = self.read_catalina_console(
            f"gpiodetect | awk '/{gpiodetect}/{{print $1}}'")
        for token in out.split():
            if token.startswith(b"gpiochip"):
                return token.decode()
        self.fail(f"could not resolve gpiochip {gpiodetect}")

    def set_pin(self, qom, pin, level):
        self.vm.cmd("qom-set", path=qom, property=f"pin{pin}", value=level)

    def get_pin(self, qom, pin):
        return self.vm.cmd("qom-get", path=qom, property=f"pin{pin}")

    def assert_pin(self, qom, pin, expected):
        level = self.get_pin(qom, pin)
        self.assertEqual(level, expected,
                         f"pin{pin} QOM read {level!r}, expected {expected!r}")

    def assert_gpio_line(self, chip, pin, expected):
        out = self.read_catalina_console(
            f"echo LINE{pin}=$(gpioget {chip} {pin})")
        self.assertIn(f"LINE{pin}={expected}".encode(), out)


if __name__ == '__main__':
    FacebookAspeedTest.main()
