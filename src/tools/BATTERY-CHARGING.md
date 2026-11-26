# Adaptive Battery Charging

[中文](BATTERY-CHARGING.zh.md)

Adaptive Charging under **Settings → Power & Battery** is off by default. The
manual start and stop thresholds remain available. On supported hardware,
adaptive mode follows ChromiumOS `powerd`'s Adaptive Charging framework: the
`ADAPTIVE_CHARGING_20230314` model and its `ExamplePreprocessor` configuration.
`battery-history` runs that model locally in pure C; no Python or TensorFlow
runtime is involved. It uses local charging history to recognize prolonged use
on AC, holds the battery near 80% rather than keeping it full, and resumes
charging to 100% before the predicted unplug time. It does not deliberately
drain a battery that is already above the target. Without enough history or
verified model files, normal and configured manual charging stay in effect.

**Charge to Full Now** temporarily requests 100% while connected to AC on
supported hardware, in both manual and adaptive modes. It changes neither the
manual settings nor the learned history. The request ends when unplugged or at
its displayed deadline (at most 24 hours). The driver may still delay charging
because of its firmware policy or temperature limits.

## Model Files

The ChromiumOS model files are never shipped in the LOCachy source tree or in
the ISO. They are downloaded only after the user explicitly opts in
(`battery_charge_adaptive="1"`).

- **Pins.** `/usr/local/share/locachy/battery/manifest.json` (installed from
  `src/link/battery-adaptive-manifest.json`) and the copy compiled into
  `battery-history` (`src/progs/battery-model-pins.h`) pin the same two files:
  - `mlservice-model-adaptive_charging-20230314.tflite`: 124,324 bytes
  - `mlservice-model-adaptive_charging-20230314-preprocessor.pb`: 31,082 bytes

  Each file is also pinned to an exact SHA-512 digest. The download source is
  ChromiumOS's distfiles mirror (`storage.googleapis.com/chromeos-localmirror/distfiles/`).
- **Fetch.** The only download path is the system job `battery-model-fetch`
  (no arguments; see [JOBS.md](JOBS.md)), which runs
  `locachy-battery-model fetch` as root. The helper downloads each file over
  HTTPS with `curl` inside the `locachy-net` network namespace, as the
  unprivileged user `nobody` (uid and gid 65534, no supplementary groups,
  `no_new_privs`). Downloads are limited to HTTPS (TLS 1.2 or later, with
  redirects only to HTTPS) and to the pinned size. Both files are downloaded to
  staging, and each must match its pinned size and SHA-512 before either is
  installed. `manifest.installed.json` is written beside them afterwards.
- **Location.** Verified files live in `/var/lib/locachy/battery/model/`.
- **Remove.** The system job `battery-model-remove` runs
  `locachy-battery-model remove` as root, with no network access. It deletes the
  two model files, the install record and any leftover staging files. Turning
  adaptive charging off does not delete the files; the removal job does.
- **Reload.** After a fetch or remove changes the files, the helper sends
  `SIGHUP` to PID 1, which reloads its settings and forwards `SIGHUP` to
  `battery-history`, so the new state applies at once.
- **Offline checks.** `locachy-battery-model status [--json]` and
  `locachy-battery-model verify` check the installed files locally and make no
  network connection.

`battery-history` never downloads anything. It verifies the files itself on
startup, on reload, and at each adaptive evaluation.

## Fail-Closed Verification

Adaptive control runs only when both files pass every check. Each must be a
regular file (not a symbolic link, FIFO or directory) of exactly its pinned size,
its SHA-512 must match the value compiled into `battery-history`, and both the
TFLite FlatBuffer and the preprocessor protobuf must parse within the supported
subset (FLOAT32 tensors, FULLY_CONNECTED and SOFTMAX operators).

If any requirement is not met, adaptive control applies no hold level and never
lowers the charge limit. It restores the manual limits, or the thresholds saved
before adaptive control took over. Those saved values sit in a hidden
`.charge-*` file under `/var/lib/locachy/battery/` until they are restored. The
states are:

- `model-missing`: adaptive is on, but one or both model files are absent.
- `model-invalid`: a model file or the model directory is a symbolic link, is not
  a regular file, has the wrong size, fails SHA-512, or does not parse. When one
  file is missing and the other is invalid, the state is `model-invalid`.
- `unsupported`: the machine does not have exactly one inserted system battery
  with a usable serial number (not empty, `0`, or `unknown`), or lacks the
  standard Linux `charge_control_end_threshold` (and, when a start threshold
  exists, `charge_control_start_threshold`). Multiple packs are not assumed to
  have independent controllers.
- `error`: reading or writing the saved baseline, sysfs readback, the pack
  identity check, or a threshold write failed.

## Controller Criteria

The controller is a port of ChromiumOS `powerd`'s `ChargeHistory` and
`AdaptiveChargingController`. Slow charging and the Charge Limit feature are omitted,
because LOCachy's sysfs path has no EC current-limit or force-discharge control.

- **Hold level:** `max(80%, current percent)`. When a start threshold exists, it
  is the end threshold minus 5. A battery that is full, or at 96% or above, does
  not begin a hold.
- **Eligibility:** at least **14 days** of retained history, and a combined
  full-plus-hold share of AC time of at least **50%** (`>= 0.5`). Until both are
  met, the state is `learning` and ordinary charging applies.
- **Prediction:** 201 features built from local history (durations floored to
  15-minute intervals) feed the
  9-bucket unplug-time model on AC plug-in and about every 30 minutes after. The
  bucket probabilities are summed from bucket 0 until the total reaches
  **>= 0.35**.
- **Delay cap:** measured from the start of holding, the predicted time until
  unplug is capped at the **30th percentile** of completed AC session durations.
- **Finish:** **2 hours** before the predicted (or capped) unplug time, charging
  resumes to 100% (start 99%, end 100%). The state is `top-up`.
- **Retention:** up to **30 days** of daily summaries and **50** completed AC
  sessions per pack, saved to `/var/lib/locachy/battery/charge-history-<pack_id>.bin`
  only while adaptive charging is enabled.

While Charge to Full Now is active on AC, the thresholds are written as start 99%
and end 100%.

## Charge States

`battery-history` writes each battery's state to `/var/lib/locachy/battery/charge-status`:

| State | Meaning |
|---|---|
| `manual` | Adaptive charging is off. Manual limits, if any, apply. |
| `full` | Charge to Full Now is active on AC. |
| `model-missing`, `model-invalid`, `unsupported`, `error` | See Fail-Closed Verification. |
| `on-battery` | AC is offline. |
| `learning` | The eligibility criteria are not yet met. |
| `hold` | Holding near the hold level. |
| `top-up` | Charging was held during this AC session and has resumed toward 100%. |
| `inactive` | Adaptive charging is on and AC is online, but no hold is active: for example, the battery was at 96% or above before a hold began, the predicted unplug is too soon for a hold, the prediction failed, or the AC state is unknown. |

## History and Clock Changes

Sleep, shutdown, and missing AC samples are not evidence that the machine stayed
plugged in. Wall-clock steps are checked against monotonic boot time. The service
does not wake a sleeping machine just to change the charge target. Older history
without verified AC data is shown, but it does not train the policy.

## Configuration

- `battery_charge_adaptive="0"`: set to `"1"` to opt in on supported hardware.
- `battery_charge_start` and `battery_charge_end`: saved manual limits, in percent.
- `battery_charge_full_until`: temporary deadline written by **Charge to Full Now**,
  not a setting to keep as a permanent charge limit.

Turning adaptive mode off restores manual limits or the thresholds that were in
force before adaptive charging took control. Battery readings, history curves and
existing manual controls remain available where supported.

Software tests cover the pure-C inference engine (`test-battery-tflite.c`), the
controller state machine (`test-battery-controller.c`), the manifest and
downloader checks (`test-battery-model.py`), and the threshold writes, readbacks
and restore path, using simulated power-supply devices. They do not establish
that a specific laptop's firmware EC stops or resumes charging at the written
threshold.
