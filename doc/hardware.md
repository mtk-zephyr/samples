# Board setup

## How Zephyr runs on a Genio EVK

Zephyr is not the only thing on the board. Linux boots first and runs the
**Jailhouse** hypervisor; Zephyr runs as an *inmate* on one Cortex-A55 core,
with a slice of memory and a short list of devices handed to it. Everything else
— storage, networking, the other five cores — stays with Linux.

This has consequences worth knowing before you debug anything:

- **A device Zephyr has not been granted does not fault politely.** Depending on
  the device, an access is either silently discarded or stops the inmate dead.
  If a driver initialises cleanly and then does nothing, suspect the grant
  before you suspect the driver.
- **The image is loaded, not flashed.** `jailhouse cell load` copies it into the
  granted window at `0x8000` and `cell start` releases the core. Nothing
  persists across a board reboot.
- **A cell can be restarted repeatedly** without rebooting Linux, which makes
  the edit-build-run loop fast.

`tools/genio-inmate.sh` does the bring-up, picking the cells from the board's
hostname. Run it on the board:

```bash
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin'
```

To check what state things are in:

```bash
adb shell 'jailhouse cell list'
```

`running` means the core is executing. `failed` usually means the image does not
fit the window the cell grants — see `system/memory_window`.

## Connectors

Three micro-USB sockets carry UART trace, each through its own USB-to-UART
bridge:

| Connector | UART | Whose |
|---|---|---|
| CN3201 | UART1 | **Zephyr's console** |
| CN3202 | UART2 | spare |
| CN3203 | UART0 | Linux console |

All of them enumerate as `/dev/ttyUSB*`. If you have more than one attached,
use `/dev/serial/by-id/` — the adapters have distinct serial numbers, and
guessing wrong looks exactly like a board that never booted.

Console settings: **115200 8N1**, no flow control.

## GPIO wiring

The inmate is granted exactly two pins of GPIO bank 1: **GPIO 38** (bank pin 6)
and **GPIO 40** (bank pin 8).

The board devicetree leaves every GPIO bank **disabled**, because which pins an
inmate gets is a property of the cell it runs under rather than of the board. An
application that wants them says so itself, in a devicetree overlay that enables
bank 1, selects the GPIO function for the two pins, and reserves the rest of the
bank so the driver refuses them. Copy
[`gpio/loopback/boards/`](../gpio/loopback/boards) — `gpio/loopback` checks that
refusal, so the overlay is proven rather than assumed.

Both come out on the 40-pin Raspberry Pi HAT header:

| SoC GPIO | Bank pin | Header pin |
|---|---|---|
| GPIO 38 | 6 | 22 |
| GPIO 40 | 8 | 18 |

**Use a wire, not a jumper block.** Header pins 18 and 22 are both on the
even-numbered row with **pin 20, a ground pin, between them**. A two-position
jumper cannot bridge them, and one fitted across 18-20 or 20-22 ties a usable
pin to ground.

## Audio (eTDM) wiring

The eTDM ports reach these pins:

| Port | Signals | Pins |
|---|---|---|
| eTDM_IN1 | MCK, BCK, LRCK, DI | 125, 126, 127, 128 |
| eTDM_IN2 | MCK, BCK, WS, D0 | 107, 108, 109, 110 |
| eTDM_OUT1 | MCK, BCK, WS, D0 | 4, 5, 6, 11 |
| eTDM_OUT2 | MCK, BCK, WS, D0 | 114, 115, 116, 117 |

A loopback joins an output port to an input port with wires — for example
eTDM_OUT1 to eTDM_IN1: pin 5 to 126, pin 6 to 127, pin 11 to 128.

Three things about audio that are not guessable:

- **The Audio Front End is off by default** and is enabled with the `mtk-afe`
  snippet: `west build -b <board> -S mtk-afe <app>`. The default image is
  deliberately left alone, because the ordinary cell grants none of the
  registers the AFE driver touches.
- **An AFE image needs a cell that grants the AFE.** Under the ordinary cell the
  inmate is stopped on its first register access.
- **The `audio` power domain is off on a stock boot**, and neither the cell nor
  the Zephyr driver can raise it. Ask Linux to hold it up first:

  ```bash
  adb shell 'echo on > /sys/devices/platform/soc/10b10000.afe/power/control'
  ```

  Without this every register write is silently dropped while every driver call
  still returns success — the driver looks fine and no audio moves.

## When there is no output at all

Silence is ambiguous: it is both a real failure signature and what a dead serial
capture looks like. Before concluding anything:

```bash
fuser /dev/ttyUSB0                 # is anything holding the port?
adb shell 'jailhouse cell list'    # running, or failed?
```

- `failed` — the image likely does not fit the granted window.
- `running` with no output — the console never came up: pin control, the
  infra-ao clock gate, or the UART driver.
- Nothing holding the port, or two things holding it — fix that first and
  re-run. Two readers each get a fraction of the bytes, which is
  indistinguishable from a dead console.
