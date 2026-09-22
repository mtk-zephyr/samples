# Inmate memory window

Confirms the memory the board devicetree declares is memory the hypervisor
actually granted.

## Run it

```bash
west build -b mt8370_genio_510_evk/mt8188/a55 samples/system/memory_window
adb push build/zephyr/zephyr.bin /root/zephyr.bin
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin'
```

Nothing to drive from the host — watch the console.

## Expected

```
MEMTEST board=mt8370_genio_510_evk/mt8188/a55
MEMTEST window base=0x8000 size=0x800000 (8 MB)
MEMTEST probe array 6 MB at 0x29ab8..0x629ab4
MEMTEST .bss zeroing already touched every byte above
MEMTEST DONE mismatches=0 -> PASS
```

If the window reads **2 MB**, the cell is granting less than the board declares.

## Why it is written this way

A plain boot cannot tell an 8 MB grant from a 2 MB one: `hello_world` never
touches a high address, so an over-declared window boots cleanly and faults much
later, under memory pressure, somewhere unrelated.

Nor does simply writing to a high address work. `arch/arm64/core/mmu.c` maps
`_image_ram_start.._image_ram_end` — the *image extent*, not the whole SRAM
region — so anything past the image is unmapped and faults for a reason that has
nothing to do with the grant.

What does work is placing a 6 MB array in `.bss`, so the linker extends the
image across the window and boot-time zeroing touches every byte. If the grant
is short the cell drops to `failed` **before the console exists**, so the symptom
is no output at all rather than an error message. Check with:

```bash
adb shell 'jailhouse cell list'
```
