# Digital Function Generator - NXP LPC1768

## Setup

1. Install the NXP LPC1768 support packages for Keil uVision 5.
2. Create an LPC1768 project and copy-paste the code from `main.c` into it.
3. Build and flash it to the board. With the hardware connected, it is ready to run.

The project clock must be 96 MHz: `main.c` assumes a 24 MHz peripheral clock (`PCLK_HZ`). Include the LPC1768 startup/system files in the Keil project.

## Controls

| Control | Action |
| --- | --- |
| Keypad `1` | Sine wave |
| Keypad `2` | Square wave |
| Keypad `3` | Triangle wave |
| Keypad `4` | Sawtooth wave |
| Keypad `A` / `B` | Frequency up / down through 10, 50, 100, 200, 500, 1000 Hz |
| 10 kΩ potentiometer | Amplitude, 10–100% |

The single-digit display shows the waveform number (1–4); the TM1637 display shows frequency in Hz. Startup is sine at 100 Hz, with amplitude set by the potentiometer. Frequency stepping stops at the ends of the list.

## Serial (UART)

Connect the mbed board through its USB connector and open its USB serial port at **9600 baud, 8 data bits, no parity, 1 stop bit (8N1), no flow control**. The firmware uses UART0 (`P0.2` TX / `P0.3` RX), connected to the mbed USB interface; these are not the external `p9`/`p10` serial pins. See the [mbed schematic](https://www.nxp.com/downloads/en/design-support/ARM_mbed_LPC1768_Schematic.pdf).

### Windows (PowerShell)

With Python 3 installed, install pySerial and list the available ports:

```powershell
py -m pip install --user pyserial
py -m serial.tools.list_ports -v
```

Replace `COM3` with the board's port from the list, then connect:

```powershell
py -m serial.tools.miniterm COM3 9600 --parity N --eol CRLF
```

### macOS (Terminal)

With Python 3 installed, create a small environment, install pySerial, and list ports:

```sh
python3 -m venv .uart-env
source .uart-env/bin/activate
python -m pip install pyserial
python -m serial.tools.list_ports -v
```

Replace `/dev/cu.usbmodem1234` with the board's actual `/dev/cu.*` device, then connect:

```sh
python -m serial.tools.miniterm /dev/cu.usbmodem1234 9600 --parity N --eol CRLF
```

On either system, type a command below in miniterm and press **Enter**. Exit with **Ctrl + ]**. Local echo stays off because the firmware echoes input. Port listing and terminal options follow the [pySerial documentation](https://pyserial.readthedocs.io/en/latest/tools.html).

| UART command | Action |
| --- | --- |
| `1`, `2`, `3`, `4` | Select sine, square, triangle, sawtooth respectively |
| `A` / `B` | Step frequency up / down through the same preset list as the keypad |
| `F500` | Set 500 Hz; use `F` followed by an integer from 1 to 1000 for another frequency |
| `?` | Show help and current waveform, frequency, and amplitude |

The code also accepts `+` / `-` for frequency stepping and `H` for help. Letter commands are case-insensitive. Amplitude is controlled by the potentiometer.

## Pin Connections

`P0.x` / `P2.x` are LPC1768 port names; `pN` means the **mbed LPC1768 board's DIP/header pin**, not the microcontroller package pin. Signal assignments below were checked against the definitions and initialization code in [main.c](main.c); header mappings were checked against [Arm's LPC1768 pin definitions](https://github.com/ARMmbed/mbed-os/blob/master/targets/TARGET_NXP/TARGET_LPC176X/TARGET_MBED_LPC1768/PinNames.h).

| Connection | LPC1768 pin | mbed header pin | Code reference |
| --- | --- | --- | --- |
| Keypad R1 | P0.4 | p30 | `R1`, `Keypad_Init` |
| Keypad R2 | P0.5 | p29 | `R2`, `Keypad_Init` |
| Keypad R3 | P0.6 | p8 | `R3`, `Keypad_Init` |
| Keypad R4 | P0.7 | p7 | `R4`, `Keypad_Init` |
| Keypad C1 | P0.8 | p6 | `C1`, `Keypad_Init` |
| Keypad C2 | P0.9 | p5 | `C2`, `Keypad_Init` |
| Keypad C3 | P0.10 | p28 | `C3`, `Keypad_Init` |
| Keypad C4 | P0.11 | p27 | `C4`, `Keypad_Init` |
| Potentiometer wiper | P0.23 (AD0.0) | p15 | `ADC_Init` |
| Waveform output / oscilloscope probe tip | P0.26 (AOUT) | p18 | `DAC_Init` |
| Single-digit segment a | P0.15 | p13 | `SEG_A`, `Seg_Init` |
| Single-digit segment b | P0.16 | p14 | `SEG_B`, `Seg_Init` |
| Single-digit segment c | P0.17 | p12 | `SEG_C`, `Seg_Init` |
| Single-digit segment d | P0.18 | p11 | `SEG_D`, `Seg_Init` |
| Single-digit segment e | P0.1 | p10 | `SEG_E`, `Seg_Init` |
| Single-digit segment f | P0.0 | p9 | `SEG_F`, `Seg_Init` |
| Single-digit segment g | P0.24 | p16 | `SEG_G`, `Seg_Init` |
| TM1637 CLK | P2.0 | p26 | `TM_CLK`, `TM1637_Init` |
| TM1637 DIO | P2.1 | p25 | `TM_DIO`, `TM1637_Init` |
| UART0 TX to onboard USB interface | P0.2 (TXD0) | Internal connection | `UART0_Init` |
| UART0 RX from onboard USB interface | P0.3 (RXD0) | Internal connection | `UART0_Init` |
| Potentiometer outer ends; TM1637 VCC | 3.3 V / GND for pot; 3.3 V for VCC | Vout p40 / GND p1 | Board power rails |
| Single-digit common cathode; TM1637 GND; oscilloscope ground | GND | p1 | Common ground |

**Wiring notes:** Use a common-cathode single-digit display with a current-limiting resistor for each segment. Connect the potentiometer's outer ends to 3.3 V and GND, and its wiper to p15. Identify keypad rows/columns and display segment leads from the actual component pinout; connector order varies.

**Correction to the code's header comment:** Segment a is **P0.15 / p13**, and segment b is **P0.16 / p14**. The header comment swaps p13 and p14; the table above follows `SEG_A` / `SEG_B` and Arm's board mapping.

## Demo Photos

![Demo 2](photos/demo-2.jpg)

![Demo 3](photos/demo-3.jpg)

![Demo 4](photos/demo-4.jpg)

![Demo 5](photos/demo-5.jpg)

![Demo 6](photos/demo-6.jpg)

![Demo 7](photos/demo-7.jpg)

![Demo 8](photos/demo-8.jpg)
