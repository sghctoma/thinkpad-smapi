ThinkPad SMAPI for FreeBSD
==========================

This is a ThinkPad SMAPI driver for FreeBSD. It allows one to control how their
ThinkPad battery is charged. Controlling the battery is done through the
following sysctls:

* hw.thinkpad_smapi.batX.start_threshold: the battery won't charge until the
  charge is below the given percentage.
* hw.thinkpad_smapi.batX.stop_threshold: the battery stops charging when it
  reaches the given percentage.
* hw.thinkpad_smapi.batX.inhibit_charge_minutes: the battery won't charge for
  given minutes (this is available for test purposes).
* hw.thinkpad_smapi.batX.force_discharge: the value 1 enables forced discharge,
  and the value 0 disables it (this is available for test purposes).

batX is "bat0" for the primary battery, and "bat1" for the secondary.

Example:
In order to start charging the primary battery only if the charge is below 20%,
and stop charging when the charge reaches 80%, one has to use the following
commands:

        \# sysctl hw.thinkpad_smapi.bat0.start_threshold=20
        \# sysctl hw.thinkpad_smapi.bat0.stop_threshold=80

