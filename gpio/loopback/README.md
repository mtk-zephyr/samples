# GPIO and EINT loopback

Drives one pin, senses it on another through a wire, and checks the GPIO driver
and the external interrupt controller together: output, input, toggle, pin
reservation, and all four interrupt trigger modes.

## Wiring

**A wire between GPIO 38 and GPIO 40** — header pins **22** and **18** on the
40-pin Raspberry Pi HAT header. GPIO 38 drives, GPIO 40 senses.

Not a jumper block: pins 18 and 22 are both on the even row with **pin 20, a
ground pin, between them**. A two-position jumper cannot bridge them, and one
fitted across 18-20 or 20-22 ties a usable pin to ground.

Without the wire every sense reads back wrong and the failures say nothing about
the code.

## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 samples/gpio/loopback
adb push build/zephyr/zephyr.bin /root/zephyr.bin
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin'
```

Nothing to drive from the host — watch the console.

## Expected

```
GPIOTEST device ready, drive=pin6 sense=pin8
PASS B6-reserved-pin configure(pin0) = -22, want -EINVAL
PASS B2-drive-high drove 1, sensed 1
...
PASS B4-both-rising 4 rising edges produced 4 event(s)
GPIOTEST DONE passed=19 failed=0 -> PASS
```

Nineteen checks: reserved pins refused with `-EINVAL`, output drives and input
reads it back, `gpio_pin_toggle` moves the pad, level-triggered interrupts
refused with `-ENOTSUP`, rising-only and falling-only each firing on their own
edge and ignoring the other, both-edges, and interrupt disable actually
silencing delivery.

## Why software edges rather than a button

The controller detects one condition per line, so both-edges is emulated by
flipping the polarity inside the handler. **An inverted flip still produces
events** — on one edge only — which reads as working code right up until someone
counts them. Driving the pin in software gives exact counts with no bounce; a
button gives neither. The test asserts four rising and four falling separately.

## Why only two pins

Every other pin in bank 1 is listed in `gpio-reserved-ranges` in **this
sample's overlay**, so the driver refuses it. `B6-reserved-pin` checks that
refusal rather than taking it on trust.

The overlay is doing three things, because the board devicetree leaves every
GPIO bank disabled: it enables bank 1, selects the GPIO function for pins 38
and 40 — every other function of those two is a JTAG signal — and declares the
reserved ranges. GPIO 38 and 40 are what the inmate cell grants, which is a
property of how the image is run rather than of the board, so the application
that uses them is where that is written down.
