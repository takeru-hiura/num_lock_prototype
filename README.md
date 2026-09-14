# TM4C123 Numeric Lock Prototype

Embedded firmware for a **TM4C123GH6PM-based electronic lock prototype** that combines keypad authentication, analog validation, servo actuation, intrusion detection, status LEDs, UART diagnostics, and an inactivity timeout.

The project integrates GPIO, ADC, PWM, UART, interrupts, and the watchdog peripheral through TI TivaWare. It is intended as a compact demonstration of real-time peripheral integration and finite-state control on a resource-constrained microcontroller.

<p align="center">
  <img src="docs/images/system-architecture.svg" alt="System architecture" width="900">
</p>

## Highlights

- 4x4 matrix keypad credential entry
- 12-bit ADC validation using a potentiometer on `PE4 / AIN9`
- Servo-controlled lock driven by `PWM1`
- Moore-style controller with `LOCKED`, `UNLOCKED`, and `INTRUDER` states
- Active-low IR intrusion detection on `PF0`
- Red/green status indication on `PF3` and `PF2`
- UART0 diagnostics at 115200 baud
- Watchdog-based partial-entry timeout
- Automatic relocking after a successful unlock

## System behavior

The controller starts in the `LOCKED` state. A user enters a four-key sequence while the firmware samples the potentiometer through ADC0. Access is granted only when both the keypad credential and analog threshold check are satisfied.

On a successful access attempt, the servo moves to the calibrated unlocked position and the green LED turns on. After approximately five seconds, the controller returns to `LOCKED` and re-engages the servo.

An active-low IR sensor generates a GPIO falling-edge interrupt. When triggered, the firmware enters the `INTRUDER` state, keeps the lock engaged, illuminates the red LED, and emits an alert over UART. The current prototype intentionally leaves this state latched; a production design would add an authenticated recovery path.

<p align="center">
  <img src="docs/images/state-machine.svg" alt="Lock controller state machine" width="850">
</p>

## Hardware interface

| Function | TM4C123 pin(s) | Interface |
| --- | --- | --- |
| IR intrusion sensor | `PF0` | GPIO falling-edge interrupt |
| Servo control | `PF1` | `PWM1 / M1PWM5` |
| Green status LED | `PF2` | GPIO output |
| Red status LED | `PF3` | GPIO output |
| Potentiometer | `PE4` | `ADC0 / AIN9` |
| Keypad rows | `PE0-PE3` | GPIO outputs |
| Keypad columns | `PC4-PC7` | GPIO inputs with pull-downs |
| UART receive | `PA0` | `UART0 RX` |
| UART transmit | `PA1` | `UART0 TX` |

The potentiometer threshold is set to `1241`, corresponding to approximately 1 V on a 3.3 V, 12-bit ADC scale. The servo uses two calibrated PWM pulse widths for the locked and unlocked positions.

## Firmware architecture

The implementation is intentionally small, but it separates the major peripheral responsibilities into dedicated initialization and service functions:

- **GPIO** - keypad scanning, status LEDs, and IR input
- **ADC0** - potentiometer sampling on AIN9
- **PWM1** - servo position control
- **UART0** - runtime diagnostics and intrusion alerts
- **GPIO interrupt** - asynchronous IR sensor handling
- **Watchdog0** - partial-credential inactivity timeout
- **State machine** - lock behavior and output selection

Application-level constants, pin assignments, thresholds, and timing values are grouped near the top of [`src/main.c`](src/main.c) rather than embedded throughout the implementation.

## Repository structure

```text
.
├── README.md
├── .gitignore
├── src/
│   └── main.c
└── docs/
    ├── technical-report.md
    ├── technical-report.pdf
    └── images/
        ├── hardware-schematic.png
        ├── state-machine.svg
        └── system-architecture.svg
```

## Documentation

A detailed design and verification report is available in both GitHub-friendly Markdown and PDF form:

- [Technical design report](docs/technical-report.md)
- [Technical design report (PDF)](docs/technical-report.pdf)
- [Prototype hardware schematic](docs/images/hardware-schematic.png)

## Build requirements

The firmware targets the **Texas Instruments TM4C123GH6PM** and uses the **TI TivaWare Peripheral Driver Library**. A build environment must provide the standard `inc/` and `driverlib/` headers and libraries, along with the startup/vector-table file for the selected toolchain.

The source can be integrated into a Code Composer Studio, Keil, or ARM GCC project configured for the TM4C123GH6PM. This repository intentionally does not include generated IDE metadata or a board-specific startup project.

Key TivaWare dependencies include:

```text
inc/hw_memmap.h
inc/tm4c123gh6pm.h
driverlib/adc.h
driverlib/gpio.h
driverlib/interrupt.h
driverlib/pin_map.h
driverlib/pwm.h
driverlib/sysctl.h
driverlib/uart.h
driverlib/watchdog.h
```

## Configuration

The default access code and core timing values are defined near the top of `src/main.c`:

```c
#define PASSWORD_LENGTH                  4U
#define UART_BAUD_RATE                   115200U
#define POTENTIOMETER_UNLOCK_THRESHOLD   1241U
#define SERVO_PWM_PERIOD_TICKS           40000U
#define SERVO_LOCKED_PULSE_TICKS         5000U
#define SERVO_UNLOCKED_PULSE_TICKS       1000U
#define UNLOCK_DURATION_SECONDS          5U
#define WATCHDOG_TIMEOUT_SECONDS          10U
```

The default keypad credential is:

```c
static const int kPassword[PASSWORD_LENGTH] = {1, 2, 3, 4};
```

## Verification summary

The prototype was validated incrementally before full integration. Reported tests included:

- observing the ADC across its expected 0-4095 range,
- confirming keypad row/column mapping through UART output,
- verifying falling-edge IR events triggered the intrusion handler,
- validating LED behavior against controller state,
- tuning servo PWM for repeatable lock/unlock positions, and
- confirming incomplete keypad entries were cleared by the timeout mechanism.

Full methodology and results are documented in [`docs/technical-report.md`](docs/technical-report.md).

## Implementation notes

The repository version has been refactored for readability without changing the intended system architecture. Two clear inconsistencies between the original prototype source and its design documentation were also corrected:

1. The analog access threshold is applied to the **ADC sample**, rather than the keypad input index.
2. The watchdog reload calculation now matches the documented **10-second** inactivity interval.

The firmware remains a prototype. A production access-control system should replace the hard-coded password, remove blocking timing, add credential lockout and secure storage, define a deliberate intrusion-recovery path, and include stronger electrical/mechanical protections.

## Authors

- **Takeru Hiura**
- **Shane Duffy**
