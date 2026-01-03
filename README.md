# joypadmouse

Tiny Linux daemon that turns a **gamepad into a mouse** (via `/dev/uinput`), with a **long-press toggle** (hold **Back/Select + LB** for 500 ms by default).

This project was created to work around a practical issue when streaming with **Sunshine** (host) + **Moonlight** (client): some Moonlight clients implement a “mouse mode” toggle (e.g. Moonlight Android), but **Moonlight New** shipped via **PortMaster** on handhelds like the **Anbernic RG35XX H** may not expose that feature. `joypadmouse` moves the toggle to the **host side**, so you can still navigate a desktop UI comfortably.

To avoid conflicting with clients that already use **Start long-press** for mouse mode (like Moonlight Android), the recommended default toggle for `joypadmouse` is **Back/Select + LB long-press**.

## How it works

- Reads controller input from the Linux joystick API (`/dev/input/js*`)
- Creates a virtual mouse using Linux uinput (`/dev/uinput`)
- In “mouse mode”, it emits `REL_X/REL_Y` (cursor), `REL_WHEEL` (scroll), and mouse buttons
- Shows a small desktop toast on toggle using `notify-send` (best-effort; ignored if unavailable)

## Default mapping

Xbox-style layout (works well with Sunshine’s default virtual Xbox controller):

- **Toggle mouse mode (default)**: hold **Back/Select + LB** for 500 ms
- **MangoHud toggle HUD (default)**: **LB + RB + Back/Select** (sends `Shift_R+F12` after release + short idle window)
- **Move cursor**: Left Stick
- **Scroll**: Right Stick Y
- **Left click**: A
- **Right click**: B
- **Middle click**: X
- **Slow cursor**: hold LB
- **Fast cursor**: hold RB

If the MangoHud chord overlaps with the mouse toggle chord, MangoHud takes priority while the chord is held.

## Build

```bash
make
```

## Install (local user)

```bash
make install
```

This installs:

- `joypadmouse` → `~/.local/bin/joypadmouse`
- Sunshine helper scripts → `~/.local/bin/sunshine-joypadmouse-start` and `~/.local/bin/sunshine-joypadmouse-stop`

## Runtime permissions

You need access to:

- `/dev/uinput` (to create the virtual mouse)
- `/dev/input/js*` (to read the gamepad)

How you grant access depends on your distro setup (udev rules, ACLs, groups). On many distros, adding your user to the `input` group is enough:

```bash
sudo usermod -aG input "$USER"
```

Then log out/in.

## Sunshine integration

The recommended approach is to create a dedicated Sunshine “app” for the clients that need mouse mode (e.g. `RG35XXH`), and attach the start/stop scripts using *Command Preparations (Do/Undo)*.

Example `apps.json` entry (simplified):

```json
{
  "name": "RG35XXH",
  "prep-cmd": [
    { "do": "sunshine-joypadmouse-start", "undo": "sunshine-joypadmouse-stop" }
  ],
  "auto-detach": "true"
}
```

Why a dedicated Sunshine app?

- Sunshine does **not** expose the paired client name (e.g. “RG35XX H”) to prep commands as an environment variable.
- Having a separate app entry makes it explicit and avoids impacting other Moonlight clients.

## Tuning

You can adjust parameters either by calling `joypadmouse` directly or via environment variables used by the Sunshine helper script:

- `JOYPADMOUSE_SPEED` (default `300`) — cursor speed (lower is slower)
- `JOYPADMOUSE_WHEEL_RATE` (default `4.5`) — scroll speed (lower is slower)
- `JOYPADMOUSE_HOLD_MS` (default `500`) — long-press toggle time
- `JOYPADMOUSE_MOUSE_TOGGLE` (default `back+lb`) — mouse toggle chord (`start+lb`, `start`, `start+rb`, `lb+rb`, `start+back`, `back`, `back+lb`)
- `JOYPADMOUSE_MANGOHUD_TOGGLE` (default `lb+rb+back`) — MangoHud chord (`lb+rb+start`, `lb+rb+back`, `start`, `back`, `start+back`, `none`)
- `JOYPADMOUSE_MANGOHUD_REPEAT` (default `1`) — how many times to send the MangoHud hotkey
- `JOYPADMOUSE_MANGOHUD_DELAY_MS` (default `80`) — delay between repeated MangoHud hotkeys
- `JOYPADMOUSE_MANGOHUD_HOLD_MS` (default `0`) — hold time for the MangoHud hotkey before release
- `JOYPADMOUSE_MANGOHUD_PREKEY` (default `shift`) — optional pre-key before the MangoHud hotkey (`shift`, `f12`, `esc`, `tab`, `space`, `enter`, `none`)
- `JOYPADMOUSE_MANGOHUD_PREKEY_DELAY_MS` (default `200`) — delay between the pre-key and MangoHud hotkey
- `JOYPADMOUSE_LOG_EVENTS` (default `0`) — set to `1` to log button events to the logfile
- `JOYPADMOUSE_DEADZONE` (default `8000`) — stick deadzone
- `JOYPADMOUSE_POLL_HZ` (default `125`) — polling rate
- `JOYPADMOUSE_DEVICE` (default `auto`) — force a joystick device (e.g. `/dev/input/js2`)

Example:

```bash
JOYPADMOUSE_SPEED=200 JOYPADMOUSE_WHEEL_RATE=3 sunshine-joypadmouse-start
```

If you need a Start-based MangoHud toggle, switch to **LB+RB+Start**:

```bash
JOYPADMOUSE_MANGOHUD_TOGGLE=lb+rb+start sunshine-joypadmouse-start
```

If a game needs a keyboard event before it will accept the MangoHud hotkey, add a pre-key:

```bash
JOYPADMOUSE_MANGOHUD_PREKEY=shift JOYPADMOUSE_MANGOHUD_PREKEY_DELAY_MS=150 sunshine-joypadmouse-start
```

Logs go to `XDG_RUNTIME_DIR/joypadmouse.log` (or `/run/user/UID/joypadmouse.log` if `XDG_RUNTIME_DIR` is not set).

## Known limitations

- The button/axis mapping is “Xbox-style” and may differ on some controllers/clients.
- Uses `/dev/input/js*` (legacy joystick interface). It’s simple and widely available, but not as expressive as `evdev`.

## License

MIT — see `LICENSE`.
