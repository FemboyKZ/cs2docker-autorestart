# CS2Docker AutoRestart

A native **Metamod:Source 2.0** plugin that auto-restarts a CS2 server when a game or plugin update is detected, or at a configured daily time.

Intended to be used with [CS2Docker](https://github.com/Szwagi/cs2docker), will not work on its own.

## Behaviour

Every 10 seconds the plugin checks whether a restart is warranted:

- **Out of date** - `/watchdog/cs2/latest.txt` differs from the `build_ver` env var,
  or any `/watchdog/layers/*/latest.txt` changed since the plugin loaded.
- **Daily restart** - the current UTC time has passed `daily_restart_time` (once per day).
- **Already scheduled** - a daily restart was previously triggered.

When a restart is warranted:

- If there are **no players**, it runs `quit` immediately.
- Otherwise it prints a chat warning once and waits; it then runs `quit` at the next map change.

## Configuration

All read from the environment (set by CS2Docker):

| Env var              | Required | Description                                                         |
| -------------------- | -------- | ------------------------------------------------------------------- |
| `build_ver`          | yes      | Current server build version (provided by cs2docker).               |
| `daily_restart_time` | no       | UTC time `HH:mm` (or `HH:mm:ss`) for a daily restart.               |
| `discord_webhook`    | no       | Discord webhook URL; a message posted once per restart decision.    |

## Building

Requires the submodules (Metamod:Source + HL2SDK-CS2, with the nested `hl2sdk-manifests`):

```sh
git submodule update --init --recursive
```

### Docker (recommended)

Produces the Linux `linuxsteamrt64` `.so` and the Metamod plugin layout:

```sh
docker build .
```

Result:

```text
dist/addons/metamod/autorestart.vdf
dist/addons/autorestart/bin/linuxsteamrt64/autorestart.so
```

Drop the contents of `dist/addons/` into the server's `game/csgo/addons/` (this is what the CS2Docker `autorestart` layer ships).

### Local (AMBuild)

Needs Python 3, [AMBuild](https://github.com/alliedmodders/ambuild), and clang:

```sh
mkdir build && cd build
python3 ../configure.py \
  --sdks cs2 \
  --targets x86_64 \
  --mms_path ../metamod-source \
  --hl2sdk-manifests ../metamod-source/hl2sdk-manifests \
  --enable-optimize
ambuild
```

The packaged output is in `build/package/cs2/`.
