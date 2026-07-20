# 1. Platform timing architecture

Each reward port free-runs on its own oscillator. The hub emits a periodic SYNC
pulse and holds a two-parameter clock model per port, fitted online, mapping local
counter values onto platform time. No frequency reference is distributed.

## Reward port

- Local crystal or oscillator module, nominal 16 MHz.
- One hardware timer free-running from reset. Never reset, reloaded or adjusted
  during a session.
- SYNC capture and event capture on **two channels of the same timer**.
- Hardware input capture only. No time counted in an ISR; no time reconstructed
  from bus messages.
- Timer input filter enabled to reject glitches.
- Reports raw capture counter values. No sequence numbers.

## SYNC

| Item | Requirement |
|---|---|
| Generation | Hub timer, output compare + auto-reload. No software GPIO toggle. |
| Period | N hub ticks; derive N from timer/prescaler registers, not intended seconds. Pulse *k* emitted at `k × N`. Start at 1 s. |
| Distribution | One line to a capture pin on every port + common ground. Differential (RS-485) for long or noisy runs. |
| Length matching | Not required (ns/m propagation). |
| Capture | Same edge polarity on all modules. |

## Pairing

Hub accepts a report for pulse *k* only within acceptance window `W` after
emitting *k*. Default `W` = half the pulse interval.

- Reports arriving in the remaining **dead zone** are discarded, never deferred.
- Two reports from one module in one window: both discarded.
- If report latency approaches `W`, lengthen the pulse interval rather than
  widening `W`.

Cross-check via counter delta. Elapsed intervals at the port:

$$m = \operatorname{round}\!\left(\frac{L_k - L_{k-1}}{N_{\text{local}}}\right)$$

Unambiguous to ~1e-4. Require *m* to equal the pulses the hub emitted between the
two accepted reports.

A missed edge yields no report; the hub times out and pairs the next report
normally. Pairing derives from hub emission schedule, not any count held at the
port.

## Model

$$T = a_i \cdot t_i + b_i$$

| Term | Definition |
|---|---|
| $T$ | platform (hub) time |
| $t_i$ | port local time = captured counter ÷ nominal timer frequency |
| $a_i$ | rate coefficient, dimensionless, within tens of ppm of 1 |
| $b_i$ | offset, platform time at $t_i = 0$ |

Both parameters required. Offset-only correction leaves a sawtooth of order
60 µs/s for two ±30 ppm oscillators.

$a_i$ moves only with board temperature. $b_i$ is physically constant while
powered; its estimate wobbles between refits (joint fit pivots about the
centroid) — monitor converted timestamp continuity, not coefficients. Module reset
invalidates $b_i$.

## Fitting

Ring buffer of 16–32 recent $(t_k, T_k)$ pairs per module, refit each pulse:

$$a = \frac{\sum_k (t_k - \bar{t})(T_k - \bar{T})}{\sum_k (t_k - \bar{t})^2}
\qquad
b = \bar{T} - a\bar{t}$$

Conditioning (slope must resolve to sub-ppm):

- Use the deviation form above, not
  $a = (N\sum tT - \sum t \sum T)/(N\sum t^2 - (\sum t)^2)$.
- Anchor local times to a recent reference, not counter zero.
- Double precision throughout.
- Optionally fit $(T_k - t_k)$ vs $t_k$; slope is then $(a-1)$ directly in ppm.

Window length trades noise rejection against tracking thermal drift. Tens of
seconds; tune against residuals.

**Admission screening.** Before adding a point, predict $T$ from current
coefficients and compare against actual emission time. Reject beyond a few
multiples of RMS residual, or 100 µs absolute floor. A mispairing shows as ~1 s
against ~1 µs residuals.

**Acquisition** (no model to screen against):

1. Collect a full window with every round clean — in-window, single report,
   consistent delta.
2. Validate: $a$ within plausible oscillator range (e.g. ±200 ppm), RMS residual
   at noise level.
3. On failure, discard the window and restart.

Buffer or flag events until acquisition completes.

**Residual monitoring.** RMS residual
$\sqrt{\frac{1}{N}\sum_k (T_k - a t_k - b)^2}$ should sit at capture-noise level,
~1 µs. Catches gradual degradation that screening misses.

## Conversion

$$T_{\text{event}} = a_i \cdot t_{\text{event}} + b_i$$

Applied at the hub. Monotonic and continuous, as counters never step. Flag events
occurring during a fault rather than converting with a stale model.

## Fault detection

Reward port:

- SYNC timeout
- capture register overrun
- timer overflow-extension inconsistency
- reset during active session

Hub, per module:

| Check | Model-dependent |
|---|---|
| Report timeout | no |
| Late report (dead zone arrival) | no |
| Duplicate report in one window | no |
| Counter-delta mismatch | no |
| Admission screen rejection | yes |
| RMS residual above threshold | yes |
| Implausible or discontinuous $a_i$ | yes |
| Counter regression | no |

Model-independent checks cover acquisition.

Isolated timeouts are not faults. Trigger on timeout *rate* within a rolling
interval, or consecutive-timeout count. Define both thresholds explicitly.

Ports must report a boot counter or first-report-after-reset flag. Counter
regression usually reveals a reset but does not cover all cases. Undetected resets
cause the hub to fit across the discontinuity — the most damaging failure mode.

On clock failure the port:

1. forces valve outputs off
2. cancels or disarms future time-critical outputs
3. preserves existing event records where possible
4. creates a `CLOCK_FAULT` event
5. reports fault status when communication is available
6. requires explicit recovery or reinitialisation

On reset or sustained fault, the hub discards that module's window and rebuilds
from scratch.

## Counter extension

Timer runs at full 16 MHz (62.5 ns/tick); do not prescale to 1 MHz.

32-bit counter wraps at $2^{32}/(16\times10^{6}) \approx 268$ s. Extend with a
software overflow word:

```c
uint64_t timestamp =
    ((uint64_t)overflow_count << 32) | hardware_counter;
```

64-bit span ≈ 36,000 years.

**Overflow race:** a capture just before wrap, with the overflow ISR serviced
first, combines a low capture with an incremented high word (~268 s error). Read
capture value, overflow count and overflow flag together; correct the high word
when a low capture coincides with a pending or just-serviced overflow. Cover with
a targeted test.

## Background

Identical oscillator part numbers do not give identical frequencies — each unit
lands independently within tolerance (±30 to ±50 ppm typical), plus temperature
differences between boards. At 1 ppm = 1 µs/s:

| Relative error | Accumulated |
|---|---|
| 1 ppm | 1 ms in ~17 min |
| 30 ppm | 1 ms in ~33 s |
| 30 ppm | 108 ms in 1 hour |