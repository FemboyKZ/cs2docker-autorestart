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

## Build

### Prerequisites

- This repository is cloned recursively (ie. has submodules)
- [python3](https://www.python.org/)
- [ambuild](https://github.com/alliedmodders/ambuild), make sure `ambuild` is in your `PATH`
- MSVC (VS build tools) on Windows / Clang on Linux

### AMBuild

```bash
mkdir -p build && cd build
python3 ../configure.py --enable-optimize
ambuild
```

### Docker

```bash
docker compose run --rm build
```
