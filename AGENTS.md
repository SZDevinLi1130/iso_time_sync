# AGENTS.md

nRF Connect SDK app: BLE Isochronous (BIS/CIS) time synchronization on
nRF54L15. A BIS broadcaster plus sync receivers share a microsecond timestamp
via a software clock servo. UI docs are in Chinese; code/comments are English.

## This is a customized fork — read the right docs
- `README.rst` is byte-identical to the upstream NCS sample
  (`nrf/samples/bluetooth/iso_time_sync`). It is stale and does NOT describe
  this app. Do not rely on it for current behavior.
- `BIS_TIME_SYNC_CONTEXT.md` is useful, but its first half describes an earlier
  8-BIS / 10 ms / PD 8000 us stage. Current code is **1 BIS** (`num_bis=1`),
  5 ms SDU interval, PD 1500 us, plus the shared-timestamp servo (last section).
  When docs disagree with `prj.conf`/`src/`, trust the code.

## Toolchain and build
- NCS v3.4.0 at `/home/devin/ncs/v3.4.0` (a v3.0.2 tree also exists — do not use it).
- `west` lives at `/home/devin/ncs/toolchains/fbf7391cab/usr/local/bin/west`.
- Default board `nrf54l15dk/nrf54l15/cpuapp`. Build:
  ```
  west build --build-dir build . --pristine --board nrf54l15dk/nrf54l15/cpuapp
  ```
  Extra CMake args go after `--` (the recorded build added
  `-DCONFIG_DEBUG_THREAD_INFO=y` and `-DBOARD_ROOT=...`).
- `build/` and `build_1/` are two pre-existing, gitignored build dirs with
  different configs. Pass `--build-dir <dir>` and `--pristine` when switching.
- No host tests, lint, formatter, or CI. `sample.yaml` is twister on-target
  builds only; real verification is on hardware + a logic analyzer.

## Git
- HEAD is detached at `92282bf` (same commit as `main`). Run `git checkout main`
  before committing, or you will create dangling commits.

## Architecture (src/)
- Role is picked at boot by sampling `sw1` (P1.09): low → BIS transmitter,
  otherwise receiver (`main.c`). No console menu. `sw0` (P1.13) requests an
  immediate sync event on the transmitter.
- SDU payload is 9 bytes: `trigger_val(1)`, `counter le32(4)`, `tx_ts le32(4)`
  (`include/iso_time_sync.h`).
- `time_sync.c` is the servo: trimmed-mean offset + least-squares drift mapping
  local GRTC to the broadcaster timebase (`shared = local + offset`). Keep
  `time_sync_to_shared()` wrap-safe unsigned 32-bit; do not sign-extend (raw
  time passes 2^31 after ~35.8 min).
- `iso_rx.c` parses with `net_buf_remove_*`, which consumes from the END of the
  buffer. The remove order (tx_ts, counter, trigger) is deliberate.
- Controller-timed `led1` GPIO is the precision reference. Never use printk
  timestamps or host GPIO paths as a timing measurement.

## Config couplings (easy to break)
- `CONFIG_SDU_INTERVAL_US` is duplicated as `TS_SDU_INTERVAL_US` in
  `time_sync.c` (currently 5000). Change both together.
- Presentation delay must satisfy `ts_offset + delay < SDU interval`;
  `CONFIG_TIMED_LED_PRESENTATION_DELAY_US=1500` leaves ~0.5 ms at the 5 ms
  minimum interval. See the comment block in `prj.conf`.
- Do not enable `CONFIG_LED_TOGGLE_IMMEDIATELY_ON_SEND_OR_RECEIVE` during
  precision captures (host GPIO toggles pollute the measurement).

## Generated docs / artifacts
- `gen_docs.py`, `gen_ppt.py`, `gen_onepage.py` regenerate the Chinese
  `.docx`/`.pptx` deliverables (need `python-pptx` + `python-docx`, installed).
  They hardcode absolute paths and write into the repo root.
- Large generated binaries (`.pptx`, `.docx`, `.pdf`, `scene_images/*.png`) are
  tracked in git. `build*/` and `*.dsl` logic-analyzer captures are gitignored.
