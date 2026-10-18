# CM3588 HDMI-CEC remote bridge

A dedicated HDMI-CEC-to-uinput daemon for RK3588-based Android TV installations. The daemon uses `/dev/cec0` and creates a virtual Linux keyboard that Weston can forward to Android.

Related projects: [Android image build](https://github.com/fviolence/cm3588-waydroid-atv) · [WayDroid host install](https://github.com/fviolence/cm3588-waydroid-host).

> The Android image must include the HWC key-range fix described below.
>
> CEC is per HDMI controller, `/dev/cec0` existing and the systemd daemon running do **not** prove that the active HDMI output has a working CEC adapter.

## Key mapping

The bridge receives CEC `USER_CONTROL_PRESSED`/`USER_CONTROL_RELEASED` on `/dev/cec0`, and reports Linux key events through `/dev/uinput`.

| HDMI-CEC control | Linux key | Android meaning |
|---|---|---|
| Up/Down/Left/Right | `KEY_UP`, `KEY_DOWN`, `KEY_LEFT`, `KEY_RIGHT` | D-pad navigation |
| OK/Select | `KEY_SELECT` (353) | `KEYCODE_DPAD_CENTER` (23) |
| Back | `KEY_BACK` (158) | `KEYCODE_BACK` |
| Device root menu | `KEY_HOMEPAGE` | Home/menu where supported |

`KEY_ESC` normally becomes Android `KEYCODE_ESCAPE`, which need not navigate back in Android TV menus. The source must therefore return `KEY_BACK` for `CEC_OP_UI_CMD_BACK` **and** declare `KEY_BACK` using `UI_SET_KEYBIT` when creating the virtual keyboard.

`KEY_ENTER` (28) maps to Android `KEYCODE_ENTER`, which dismisses the on-screen keyboard instead of selecting the highlighted key. `KEY_SELECT` maps to `KEYCODE_DPAD_CENTER`, provided the Android HWC forwards the full Linux keycode. The [HWC key-range patch](https://github.com/fviolence/cm3588-waydroid-atv/blob/master/patches/hardware/waydroid/0007-hwcomposer-use-current-display-refresh-and-correct-k.patch) replaces the truncated 239-entry key-state array with `KEY_CNT` and removes the `uint8_t` keycode cast. Both changes are needed for keycode 353.

> Some TVs handle their own Home key internally without sending a CEC command.

## 1. Install compiler and device utilities

On Debian ARM64:

```bash
sudo apt update
sudo apt install build-essential cmake linux-libc-dev v4l-utils
```

## 2. Build the daemon

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

## 3. Check CEC and uinput (before installing the service)

On the Debian CM3588 host, identify the *connected* video connector and the available CEC adapters:

```bash
grep -H . /sys/class/drm/card*-HDMI-A-*/status
cec-ctl --list-devices
ls -l /dev/cec*
```

Inspect the adapter. Do not configure or override its physical address by hand:

```bash
cec-ctl -d /dev/cec0
udevadm info --query=path --name=/dev/cec0
```

**Required for remote operation:** `Physical Address` must be valid (e.g. `1.0.0.0`, not `f.f.f.f`), and a playback logical address must register.

> When a suitable *different* adapter is available, the daemon accepts its path as its first argument. Update the service's `ExecStart` via a systemd override rather than editing the installed vendor unit directly.

Verify uinput independently:

```bash
sudo modprobe uinput
ls -l /dev/uinput
```

If `uinput` is a loadable module, arrange for it to load at boot **before** starting the service:

```bash
printf 'uinput\n' | sudo tee /etc/modules-load.d/cm3588-cec-uinput.conf
```

## 4. Install and enable service

On a **new host** where no working version is installed:

```bash
if sudo test -e /usr/local/bin/cm3588-cec-remote ||
  sudo test -e /etc/systemd/system/cm3588-cec-remote.service; then
  echo 'CEC daemon already installed; do not overwrite it.' >&2
fi
```

```bash
sudo install -m 0755 build/cm3588-cec-remote /usr/local/bin/cm3588-cec-remote
sudo install -m 0644 systemd/cm3588-cec-remote.service /etc/systemd/system/cm3588-cec-remote.service
sudo systemctl daemon-reload
sudo systemctl enable --now cm3588-cec-remote.service
```

```bash
journalctl -u cm3588-cec-remote.service -b --no-pager
```

## 5. Verify remote input

Confirm D-pad navigation, OK/Select (also in Android's on-screen keyboard), Back, release events and long presses.

Then check the virtual device:

```bash
cat /proc/bus/input/devices | grep -A8 -B3 'CM3588 HDMI-CEC Remote'
```