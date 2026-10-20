# Power, sleep and battery (v1.0)

Nothing in this file has been measured on a device yet. Every current and the battery-life figure are labelled
ASSUMPTION; docs/KNOWN_LIMITATIONS.md lists what still has to be measured and how.

## Battery capacity
750 mAh single-cell Li-ion. Source: Seeed reTerminal Sticky Hardware Overview, Power System table
(https://www.seeedstudio.com/sticky/docs/en/device-guide/hardware-overview/). The local Seeed demo
sources (BQ27220 driver) read Design/FullCharge capacity from the gauge at runtime but do not
hard-code a value, so the gauge's FullChargeCapacity should be checked on the device.

## Wake sources (ESP32-S3: only RTC GPIO 0..21 wake from deep sleep)
| Source | GPIO | RTC-capable | Used | Notes |
|---|---|---|---|---|
| AI / OK button | 4 | yes | EXT1 any-low | always armed |
| UP / DOWN | 5 / 6 | yes | EXT1 any-low | armed for normal sleep, not for battery shutdown |
| GT911 touch INT | 21 | yes | only with `-DMT_TOUCH_WAKE=1` | touch rail is switched off in sleep (Seeed does the same); keeping it on costs extra current (unmeasured) |
| USB present | 9 | yes | EXT0, opposite of current level | plug-in wakes a sleeping device |
| PCF8563 INT | none | n/a | no | not routed to a GPIO in Seeed's pin map (pin_config.h lists only SCL 0 / SDA 1 / addr 0x51) |
| RTC timer | n/a | n/a | yes | ESP32 RC slow clock; drift corrected from the PCF8563 at every wake |

## Next wake
`core::planWake`: earliest of the dose engine's next transition (due time, ladder repeat, snooze end,
grace expiry, schedule regeneration), the nightly full refresh (03:00 local), the daily NTP slot
(04:00 local; only when Wi-Fi is set up and "automatic time" is on) and a 6 h maximum. Wake time = target -
(20 s + 5 % of the sleep span) to cover boot time and ASSUMPTION worst-case RC drift; never less than
15 s of sleep. After a timer wake with no input the device goes back to sleep after 20 s
(`MT_TIMER_WAKE_IDLE_MS`). Early wakes (before the target) are counted.

Sleep is refused while: an e-paper refresh runs, an alarm is due or an alert is playing or queued,
log/snapshot writes are pending, the clock is unset; and (unless the user asked with a 2 s OK hold)
while USB powered (unless `settings.sleepOnUsb`), off the Home screen, or idle < 5 min.

On every boot/wake the PCF8563 is read; when valid the system time is set from it and the
difference is logged and kept as `lastDriftSec`/`maxDriftSec`.

## Catch-up
At wake the engine processes all transitions up to now in time order. Events older than 60 s are
marked stale. `core::planAlerts` turns doses that are still due into one catch-up alert per person
(audible unless quiet hours apply and nothing is critical); doses that passed their grace window
while asleep are logged as Missed by the engine and announced with a toast
("N missed while asleep - see History"). Nothing is dropped silently.

## Battery policy (`core::BatteryMonitor`)
| Level | SoC | Action |
|---|---|---|
| Low | <= 20 % | banner |
| VeryLow | <= 10 % | persistent banner + one urgent alert (ignores quiet hours) |
| Critical | <= 5 % (2 consecutive readings, not on USB) | "Charge now" screen (stays on the e-paper), flush, deep sleep, AI button wake only |
Recovery needs +3 % hysteresis. An unreadable gauge never triggers a shutdown. On USB the warnings
clear above 20 % and no shutdown happens. The status bar shows the battery %, the charging icon
while USB is present and "USB nn%" when powered but not charging.

## Measurement
Self-test (hold UP+DOWN 3 s): "8 Counters" shows boots, wakes by source, sleeps, early wakes,
total sleep/awake time, awake share, drift, catch-up alarms, missed-while-asleep and battery
shutdowns (RTC memory, mirrored to NVS before each sleep; UP+DOWN resets). "9 Sleep current"
sleeps with all rails off and no timer so the current can be measured with a meter.

## Battery-life estimate
All currents below are ASSUMPTIONS until measured with the sleep-current test and a USB meter.

| Item | Assumed current | ASSUMPTION basis |
|---|---|---|
| Deep sleep, whole board | 0.15 mA | ESP32-S3 ~10 uA + BQ27220 + PCF8563 + regulator/charger leakage |
| Awake, idle UI (80 MHz, radios off) | 25 mA | ESP32-S3 at 80 MHz without Wi-Fi |
| E-paper FAST refresh | +30 mA for 0.5 s | SSD1677 class panels |
| E-paper FULL refresh | +30 mA for 3 s | |
| Buzzer sounding | +30 mA | magnetic buzzer at 50 % duty |

Scenario: 2 people, 8 scheduled doses/day, about 10 min of browsing per day.

| Per day | Time | Charge |
|---|---|---|
| Dose interactions: 8 x (1 min alarm/confirm + 5 min idle timeout) | 48 min awake | 20.0 mAh |
| Ladder repeats / other timer wakes: 8 x 25 s | 200 s | 1.4 mAh |
| Browsing | 10 min | 4.2 mAh |
| Nightly refresh + 6 h status wakes: 4 x 25 s | 100 s | 0.7 mAh |
| Refreshes: 60 FAST + 10 FULL | | 0.5 mAh |
| Buzzer: 16 alerts x 3 s | | 0.4 mAh |
| Deep sleep: ~22.7 h x 0.15 mA | | 3.4 mAh |
| **Total** | | **~30.6 mAh/day** |

Usable charge: 750 mAh x ~85 % (cut-off at 5 %, ageing, temperature) = ~640 mAh, so about
**21 days**. With a 1 min idle timeout (`-DMT_IDLE_SLEEP_MS=60000`) the awake share
drops to ~17 mAh/day, about **37 days**. If the real sleep current is 1 mA, add 23 mAh/day: about
**12 days**. The 5 min idle timeout dominates: it is the first thing to tune after measuring.
