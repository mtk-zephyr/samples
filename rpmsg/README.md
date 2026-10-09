# RPMsg between Linux and Zephyr

Zephyr and Linux exchange messages over RPMsg, with Zephyr as the **remote**.
Linux sends "Hello from Linux!" and Zephyr answers "Hello from Zephyr!", each
numbered, about every two seconds.

It is the sample to read for how a Zephyr inmate and Linux share memory and
doorbells under Jailhouse, and the one to run to see that the path works on a
board.

| | |
|---|---|
| [`zephyr/`](zephyr) | The Zephyr application. OpenAMP over the shared memory of an ivshmem-v2 device |
| [`linux/`](linux) | `rpmsg-test`, the Linux side |
| [`include/common.h`](include/common.h) | The memory layout both sides agree on |

## How it fits together

The Jailhouse **rpmsg cells** give the inmate a virtual PCI host bridge with one
ivshmem-v2 device behind it, and a read-write section of shared memory. Zephyr
puts the OpenAMP resource table at the start of that section, followed by two
vrings of 4 KiB. It announces the service `rpmsg-raw`.

Linux's BSP kernel binds the same device with `mtk_jh_rproc`, as a **detached
remoteproc** named `0001:00:00.0`. Attaching it brings up virtio rpmsg, and the
service announcement creates `/dev/rpmsg0` through `rpmsg_char`. No kernel
change is needed.

The shared memory is read from the device, so the application follows whatever
regions the cell grants. The board overlays name only the host bridge at
`0x6b800000` and the interrupt, GIC SPI 74, which is the vPCI interrupt the
cells assign. The two boards' overlays are identical.

## Build

Zephyr, from a workspace made as the [top-level README](../README.md) describes:

```bash
west build -b mt8390_genio_700_evk/mt8188/a55 samples/rpmsg/zephyr   # or mt8370_genio_510_evk
```

The Linux tool, with an aarch64 toolchain:

```bash
make -C samples/rpmsg/linux                       # aarch64-linux-gnu-gcc, the default
make -C samples/rpmsg/linux CROSS_COMPILE=...     # another prefix, e.g. a Yocto SDK's
```

The result is `samples/rpmsg/linux/build/rpmsg-test`.

## Run

Use the **rpmsg cell** of your board, `genio-<board>-evk-zephyr-rpmsg.cell`, not
the plain `-zephyr` one: only it provides the ivshmem device. The root cell is
the board's ordinary `genio-<board>-evk.cell`, the cell is named `zephyr`, and the
image is loaded at the cell's entry, `0x8000`. [`tools/genio-inmate.sh`](../tools/genio-inmate.sh)
does all of that:

```bash
adb push build/zephyr/zephyr.bin                  /root/zephyr.bin
adb push samples/rpmsg/linux/build/rpmsg-test     /root/rpmsg-test
adb push samples/tools/genio-inmate.sh            /root/genio-inmate.sh
adb shell 'sh /root/genio-inmate.sh /root/zephyr.bin genio-700-evk-zephyr-rpmsg.cell'
```

Then, **on the board**, attach Linux. The remoteproc number differs between the
boards, because the 510 image has no SCP remoteproc and the 700 has one, so find
it by name rather than by number:

```sh
n=$(grep -l 0001:00:00.0 /sys/class/remoteproc/*/name | sed 's|.*/remoteproc\([0-9]*\)/name|\1|')
[ -n "$n" ] && /root/rpmsg-test -p "$n"
```

`rpmsg-test` writes `start` to the remoteproc, waits for `/dev/rpmsg0`, and then
prints each message it receives. The Zephyr console, UART1 on **CN3201**, prints
the messages Linux sends. The message numbers rise by one each time. Stop
`rpmsg-test` with Ctrl-C.

Each attach logs "Allocated carveout doesn't fit device address request" twice.
The exchange works regardless.

## Three rules for operating it

These are not obvious, and breaking the third leaves the board needing a
recovery sequence.

1. **Start the cell, then attach Linux.** Not the other way round.
2. **Attach once per cell start.** `rpmsg-test` writes `start` every time it runs,
   and on an attached remoteproc that does not fail: it takes another reference.
   Run it again and you owe a second detach below.
3. **Detach before you destroy the cell.** The BSP driver has no `stop`, so
   `echo stop` fails with `EINVAL`. Detach instead, repeating until the state
   reads `detached`:

   ```sh
   echo detach > /sys/class/remoteproc/remoteproc$n/state
   cat /sys/class/remoteproc/remoteproc$n/state      # until this says "detached"
   jailhouse cell destroy zephyr
   ```

   One `detach` is needed per `start` you issued, and a detach that is not the
   last returns success and changes nothing, so check the state rather than
   counting.

### If the cell was destroyed first

Linux does not notice. The remoteproc stays `attached` and keeps the dead
instance's rings, `/dev/rpmsg0` stays, and the next cell waits forever at
"Awaiting VIRTIO config ready" while Linux logs `virtio_rpmsg_bus virtio0: msg
received with no recipient`.

To recover: detach until the state reads `detached`, destroy the cell, and start
again from the top.

**This includes re-running `genio-inmate.sh`.** The script destroys any existing
cell called `zephyr` before creating the new one, so run the detach first.

## What is not here

A way to stop the exchange from the Zephyr side, and automatic detaching when
the cell goes away. Both would belong in the BSP driver, which could watch the
ivshmem peer state; the Zephyr image cannot fix it.
