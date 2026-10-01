# MediaTek Genio Zephyr samples

Runnable applications that exercise the Zephyr drivers on the MediaTek Genio
EVKs, each one checking itself and printing a verdict. Use them to confirm a
board is wired and configured correctly, to check a driver change did not break
anything, or as worked examples of the interfaces.

They build against the [Genio Zephyr tree](https://github.com/mtk-zephyr/mtk-zephyr),
and this repository is the west manifest for that pairing — so one command gets
you the tree, its modules and these samples, already matched.

```bash
west init -m https://github.com/mtk-zephyr/samples genio-workspace
cd genio-workspace
west update
```

That gives you:

```
genio-workspace/
├── zephyr/      the Genio Zephyr tree
├── modules/     HALs and modules it needs
└── samples/     this repository
```

Which branch of the tree you get is set by `revision:` in `west.yml`:
`mtk-genio-dev` (default; where features land first, rebased and force-pushed)
or `mtk-v4.4.2` (stable, only ever appended to). Pin a tag rather than a branch
for anything you have to support.

## Building and running one

```bash
west build -b mt8390_genio_700_evk/mt8188/a55 samples/gpio/loopback
```

Boards: `mt8390_genio_700_evk/mt8188/a55` and `mt8370_genio_510_evk/mt8188/a55`.

Zephyr runs on **one Cortex-A55 core as a Jailhouse inmate**, alongside Linux —
so "flashing" means copying the image to the board and starting a cell, not
writing to storage:

```bash
adb push build/zephyr/zephyr.bin    /root/zephyr.bin
adb push samples/tools/genio-inmate.sh /root/genio-inmate.sh
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin'
```

The console is **UART1 at 115200 8N1**, on connector **CN3201** — a different
micro-USB socket from the one Linux logs to. Watch it with any terminal:

```bash
picocom -b 115200 /dev/ttyUSB0
```

Only one process may hold the serial port. If a sample's host script reports the
port busy, something else is attached — `fuser -k /dev/ttyUSB0`.

[`doc/hardware.md`](doc/hardware.md) covers the board setup in full: cells,
connectors, jumpers and the audio wiring.

## The samples

| Sample | Verifies | Needs |
|---|---|---|
| [`system/boot_and_timer`](system/boot_and_timer) | Boot, board identity, `cntfrq`, timer accuracy | host script |
| [`system/memory_window`](system/memory_window) | The inmate really is granted the memory it declares | — |
| [`uart/rx_interrupt`](uart/rx_interrupt) | Interrupt-driven RX under load, byte-for-byte, with ISR counts | host script |
| [`uart/reconfigure`](uart/reconfigure) | `uart_configure()` changes the rate **on the wire** | host script |
| [`gpio/loopback`](gpio/loopback) | GPIO output, input, toggle, and all four EINT trigger modes | a wire |
| [`audio/`](audio) | The Audio Front End: playback, capture, and full-duplex eTDM loopbacks that check every captured frame | wires, a cell, and Linux's help |

Each has its own README with the exact expected output.

**Audio has prerequisites the others do not** — a build snippet, a hypervisor
cell that grants the AFE, and two things Linux has to do first. Every one of
them fails silently, with the driver returning success throughout, so work
through [`audio/README.md`](audio/README.md) before concluding anything about
the driver.

**Three of them need a host-side script**, because the property being tested
cannot be observed from the target. Timer accuracy measured with the timer under
test always looks perfect; a UART echo has to be compared against what was sent;
and a baud change is only real if the old rate stops working. Those scripts live
in each sample's `host/` directory and need `pyserial`.

## Checking they all still build

```bash
west twister --build-only -T samples -p mt8390_genio_700_evk/mt8188/a55
```

Useful after moving the tree to a new revision.

On a workspace tracking **`mtk-v4.4.2`** this reports `No testsuites found at the
specified location`, which looks like a wrong path and is not: these samples are
described by `tests.yaml`, a name twister only learned in April 2026, and that
branch's twister still looks for `sample.yaml`. Build each sample with
`west build` there instead. The file name stays as it is — upstream has
converted all of its own definitions to `tests.yaml` and calls the older names
deprecated.

## What is not here

**The in-tree test suites.** `tests/drivers/uart/uart_basic_api`,
`uart_interrupt_api` and `tests/drivers/gpio/gpio_basic_api` live in the Zephyr
tree and are run from there. They complement these samples: the samples cover
what is most likely wrong in *these* drivers, the in-tree suites cover whether
the drivers honour the *API contract*. Note `uart_basic_api` is declared
`harness: keyboard`, so twister cannot run it unattended — it waits for a human
to type.

**Anything that needs our lab automation.** These samples are meant to be run by
hand, by you, on a board in front of you.
