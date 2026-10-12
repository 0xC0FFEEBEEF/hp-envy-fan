# hp-envy-fan

Fan control for the **HP ENVY x360 Convertible 15-ee0xxx / 15m-ee0xxx** (AMD Ryzen 4000, board ID `876F`) on Linux:
a kernel driver that lays your own fan curve over the laptop's and exposes HP's thermal modes (Performance,
Balanced, Cool, Quiet) as a standard Linux platform profile, and a KDE Plasma 6 tray widget to set both and to show
how close the CPU is to thermal throttling.

On this laptop nothing else works: `hp-wmi`'s fan control returns `EINVAL`, HP's thermal-profile WMI call is broken
in the BIOS, and NBFC has no working profile. Tested on BIOS F.24 only.

## What it does

- Once a second the driver compares the fan level your curve asks for at the current CPU temperature with the
  level HP's embedded controller (EC) chose, and raises any fan that is below the curve. It never lowers a fan, so
  the fans never run slower than HP would run them, and CPU power limits and clocks are untouched.
- Rising temperatures apply at once; falling ones ease off over about 8 seconds. After that the EC ramps the fans
  down on its own.
- `curve_smooth` sets how many seconds of heat the driver sits through before speeding the fans up: while it is
  set, speeding up follows the lowest temperature seen in that window, so the brief spike from opening an
  application never reaches the fans, while real load is followed in full a few seconds later. Slowing down is
  unaffected, and so is HP's own control underneath. The widget has it as "Sit out short spikes".
- On unload, suspend or shutdown, or if an EC call fails, it stops and the EC carries on as shipped.
- It registers platform profile `hp-envy-fan` with HP's four thermal modes, so power-profiles-daemon and KDE's
  power profile switch drive them (performance, balanced, and power-saver as quiet). The EC forgets the mode
  across a suspend, so the driver puts it back on resume and tells userspace to re-read it. Measured on this
  machine:

  | Mode | Sustained power | Burst | HP's fans |
  |---|---|---|---|
  | performance | 37 W | 48 W | 4100 RPM from ~72 °C |
  | balanced (HP default) | 25 W | 48 W | 4100 RPM from ~100 °C |
  | cool | 17 W | 25 W | 4100 RPM from ~48 °C |
  | quiet | 15 W | 25 W | lower speeds, later |
- It watches for HP's EC emergency signals (fan failure, battery overheating and three other faults), which the
  EC reports by forcing the hidden thermal zone `\_TZ.TSZ2` to its `_HOT` point. Windows shuts down on that; Linux
  ignores it. The driver logs it, shows it in `emergency` and the widget, and with `emergency_shutdown=1` shuts
  down.
- It reports battery details the EC keeps and ACPI leaves out: charge cycles, health, cell voltages and
  manufacture date.
- The widget shows the CPU temperature and its distance from the throttle point, both fans' drive levels, clocks
  and load. It switches between HP's own control, a curve (two points set with sliders) and a fixed minimum, and it
  sends a notification when the CPU is throttling or getting close.

## Install

Needs `dkms`, kernel headers and, for the widget, Plasma 6.

```sh
sudo ./install.sh driver   # DKMS module, udev rule, load at boot
./install.sh widget        # Plasma widget for your user
```

The udev rule lets members of `wheel` change the curve without root. On distributions that use another group for
admins (`sudo` on Debian and Ubuntu), edit `udev/61-hp-envy-fan.rules` first.

The driver only loads on board `876F`. It also checks the EC's RAM layout against the DSDT's own fan reading
before it writes anything, and refuses to load if they disagree.

## Using it without the widget

The driver's hwmon device (`name` is `hp_envy_fan`) has:

| File | |
|---|---|
| `temp1_input` | CPU temperature from the EC |
| `fan1_input`, `fan2_input` | fan speed as the EC last reported it, RPM (goes stale while the curve holds a fan up) |
| `pwm1`, `pwm2` | fan drive level, 0-255 |
| `curve` | `temp:percent` points, up to 8, e.g. `60:40 70:55 80:75 90:100`; below the first point the EC decides |
| `curve_enable` | `1` for the curve, `0` for HP's own control |
| `curve_floor` | the level the curve asks for right now, percent |
| `curve_smooth` | seconds of temperature spike to ignore before speeding up, 0-30; `0` reacts at once |
| `emergency` | `none`, or the EC faults seen since load: `fan-failure battery-overheat power-fault thermal-trip fan-fault` |
| `battery_cycles` | charge cycles from the EC (ACPI reports 0 on this laptop) |
| `battery_health` | full-charge capacity as a percentage of design capacity |
| `battery_cells` | cell voltages, mV |
| `battery_manufactured` | manufacture date, `YYYY-MM-DD` |

HP's thermal mode is `/sys/firmware/acpi/platform_profile`, or `profile` under the `platform-profile` device named
`hp-envy-fan` (the udev rule makes that one writable by `wheel` too).

```sh
H=$(dirname "$(grep -l hp_envy_fan /sys/class/hwmon/hwmon*/name)")
echo "55:40 85:100" > "$H/curve"
```

Module options: `curve=` (the curve at load), `curve_enable=0` (start with HP's control), `emergency_shutdown=1`
(shut down on an EC emergency), `debug=1` (EC RAM dump, writes limited to the fan block, raw thermal-policy calls,
the EC's power limits and a root-only ACPI evaluator, for research).

## How the EC works

Found by reading the DSDT and testing on the hardware. The DSDT's `\_SB.PCI0.LPC0.EC0.FANG(i)` and `FANW(i, v)`
read and write byte `i` of the EC's extended RAM through a mailbox at `0xfe0b0d80`.

| EC RAM | |
|---|---|
| `0x8102` | fan 1 drive level, percent |
| `0x8104` | fan 1 speed, RPM / 100 (the EC copies it to `FRPM`, which `\_TZ.TSZ0.FRSP` reports) |
| `0x8103`, `0x8105` | counters |
| `0x8106` | flags, `0x05` at rest |
| `0x810a`... | HP's step table, apparently `(low °C, high °C, RPM / 100)` triples: `(73,100,41) (76,105,46) (81,110,52)`... |
| `0x8129` | fan 2 drive level, percent |
| `0x812b` | fan 2 speed, RPM / 100 |
| `0x8131`... | fan 2 step table, speeds 43 / 48 / 54 |

A drive level written to `0x8102` or `0x8129` takes effect at once. The EC only notices after a while and then
ramps it back towards its own choice, a few percent every couple of seconds, which is why the driver re-checks
every second. At 100% the fans run at about 6000 RPM; HP's own table stops at 5200.

Other things found in the firmware:

- Battery care is WMI command `0x2b` (`\_SB.WMID.SBCO`/`GBCO`): `0x100` sets EC bit `BTNO`, `0x300` also `SBTC`,
  `0x500` also `SBIS`; `GBCO` reads back 0, 3 and 4. What each does to charging is not yet measured.
- The EC shared memory at `0xfe0b0800` (DSDT `ECMM`) also has two fan flags, throttle bits (`TTHR`, `THTL`, `CTDP`),
  extra temperatures (`EST1`-`EST3`, `SKTB`, `SKTC`), wake enables and a likely keyboard-light bit (`KLSW`, `0x8bb`
  bit 4); these are not decoded yet.
- `FFAL`/`THSD`/`PDPN` live in CMOS (index/data ports `0x72`/`0x73`), so a fan failure flag survives reboots.
- The two WMI devices `WFDE`/`WFTE` are the BIOS vendor's sample devices ("SampleDev", "TestDev"), reused to signal
  Wi-Fi SAR changes.
- WMI `0x10` returns the warranty start date; HP keeps the thermal mode in UEFI variable `HTPS`.

What does **not** work, so nobody has to try it again:

- `hp-wmi` `pwm1_enable`: `EINVAL` for every value; the BIOS doesn't implement HP's gaming-laptop fan commands.
- Thermal profile through WMI command `0x4c`: reading works, but `WRCF` passes the whole WMI buffer to `STPS`
  instead of the data word, so SMM always receives the signature `SECU` as the mode and returns error 6. Calling
  `\_SB.WMID.STPS(mode)` directly works (return code in `\_SB.WMID.HWRC`), which is what the driver does, holding
  `\MSMT` as `WMAA` does. `GTPS` writes only the first byte of the SMM mailbox, leaving the rest of the previous
  call's data, so only that byte is the mode.
- `FWPM` in the EC's shared memory (`0xfe0b0814`, used by `\_TZ.TSZ0.FSSP`): writes land and are ignored.
- The step table in EC RAM: writes land, but the EC does not read it back.
- Writing the speed byte `0x8104`: the readout follows it and the fan does not.
- The ENVY x360 13-bd0xxx (Intel, board `8824`) patch's sequence, `0xff` to `0x8106` for manual mode, then PWM
  0-255 to `0x8102`: here `0x8106` is a flags byte, and `0x8102` takes a percentage and needs no mode switch.

## amd-pstate

The 4500U has the `cppc` CPU flag, but the BIOS has no `_CPC` objects, and more to the point the firmware never
enabled CPPC: `MSR_AMD_CPPC_CAP1` reads 0 on every core and `MSR_AMD_CPPC_STATUS` does not respond. Adding `_CPC`
through an ACPI override would not help, so this stays on `acpi-cpufreq`.

## Throttling

The widget treats the CPU as throttling when Tctl (`k10temp`) is within 3 °C of the throttle point, 105 °C for the
Ryzen 5 4500U by default and adjustable in the widget. "Close" is a margin you set, 10 °C by default. The SMU also
limits clocks for power, which this does not report.

## License

GPL-2.0-only. See `LICENSE`.
