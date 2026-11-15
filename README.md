# CamLink

Video calls on a Nintendo GameCube (work in progress), with a USB webcam plugged into a
USB-to-GameCube controller adapter (an RP2040-Zero board running our own
firmware instead of its controller firmware).

## Step 1: does the webcam talk to the adapter?

- `adapter/` - **CamLink**, the adapter firmware. It checks the webcam on the
  adapter's USB port, shows the result on the board's colour LED, and answers
  the GameCube on the controller port's data wire.
- `gamecube/camtest/` - **camtest**, a GameCube program that finds the adapter
  in any controller port, shows what the webcam reported, and tests how fast
  the controller-port link is.
- `shared/link.h` - the commands the two sides use.

### Putting CamLink on the adapter

1. Unplug the adapter from the GameCube. Connect the board's USB-C port to a
   computer.
2. Hold **BOOT**, tap **RESET**, release BOOT: a drive called **RPI-RP2**
   appears.
3. Drag `camlink.uf2` onto it. The drive disappears when it's done.

Never connect the adapter to a computer and a GameCube at the same time.

To use it as a controller adapter again, flash the GameCube (RP2040-Zero)
firmware from the [Joypad OS releases](https://github.com/joypad-ai/joypad-os/releases)
the same way.

### The LED

| Light | Meaning |
|---|---|
| Steady green, short blue flash every 2 s | The camera, its video stream and microphone |
| Steady green | The camera and its video stream |
| Blinking green | The camera, but no video stream |
| Yellow double blink | A different USB device |
| Slow white blink | No USB device (or no power on the USB port) |

## Step 2: the first live picture

- `adapter/` - CamLink now starts the webcam when the GameCube asks (its light
  comes on), receives its video, and keeps the newest whole picture ready.
- `gamecube/camview/` - **camview**, which starts the camera and shows its
  pictures full screen, with frames per second and the link's speed.

Put the new `camlink.uf2` on the adapter, plug the webcam in, run
`camview.dol` with a wired controller in another port. A turns the camera on
and off; START restarts the GameCube.

The board's LED shows cyan (blinking while the camera starts, steady while
pictures come) and red if the camera didn't take its settings.

## Building

GitHub builds everything on every push (Actions tab): `camtest.dol`,
`camview.dol` and `camlink.uf2`.

## License

The GameCube programs: MIT (see LICENSE). The adapter firmware: GPL 3.0 or
later (adapter/COPYING), since its camera settings come from the Linux gspca
zc3xx driver; it uses joybus-pio (LGPL 3.0) and TinyUSB.
