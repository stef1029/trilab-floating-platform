# Floating Platform Orienting System — Project Specification

A head-fixed behavioural platform for studying sensory-guided spatial orienting in the mouse.

> **Status:** Greenfield design specification. This document defines the scientific intent,
> design principles, and requirements for the system. It deliberately commits to *what* the
> system must achieve and *why*, while leaving concrete engineering choices open where they
> are not yet constrained by the science.

---

## 1. Background and Motivation

### 1.1 The problem of spatial orienting

Orienting towards environmental stimuli is a fundamental behaviour conserved across the
animal kingdom: an animal turns its eyes, head, and body to bring a salient stimulus into
its sensory and motor focus. Although subjectively effortless, orienting requires the
nervous system to solve several hard computational problems at once:

- **Coordinate-frame transformation.** Different senses encode space in incompatible
  reference frames — vision in retinotopic coordinates, hearing in head-centred coordinates,
  touch in body-centred coordinates — yet the motor system needs a single unified target.
- **Sensorimotor transformation.** A spatial estimate must be converted into a coordinated
  motor command spanning effectors with very different dynamics (fast, low-range eyes; slower,
  wider-range head; slow, widest-range body).
- **Target selection.** When multiple stimuli compete, the system must commit to a single
  target — the body cannot turn two ways at once.

### 1.2 The superior colliculus (SC)

The SC is an evolutionarily ancient midbrain hub (circuits >500 My old) that sits at the
centre of this problem. It receives topographically aligned, convergent input from
essentially every sensory modality and contains a motor map from which electrical or
optogenetic stimulation reliably evokes orienting movements toward the corresponding
location. Beyond driving movement, accumulating evidence implicates the SC in **spatial
priority / attention**: it is thought to maintain a *priority map* that integrates bottom-up
salience with top-down task relevance, gating both descending orienting commands and
ascending thalamo-cortical signals.

### 1.3 The methodological gap this system addresses

Mouse genetics (Cre-driver lines, optogenetics, optotagging, calcium imaging, two-photon,
high-density electrophysiology) make the mouse SC uniquely tractable for circuit dissection.
But existing mouse paradigms force a trade-off:

| Approach | Strength | Weakness |
|---|---|---|
| **Head-fixed** (2-photon, Neuropixels, VR) | Maximal experimental control, high trial counts, recording/manipulation access | Sacrifices the natural whole-body orienting repertoire; reporting movements are abstract (e.g. licking, wheel turns) |
| **Freely moving** (escape, prey capture, social orienting) | Full natural movement repertoire, ethological validity | Low trial counts, behavioural adaptation/disengagement, hard to standardise and track, limited tool access |

**Design thesis:** a head-fixed preparation that *preserves rotational orienting kinematics*
by mounting the animal on a low-friction, air-floated platform can recover much of the
ethological character of orienting while retaining the recording/manipulation access and
trial structure of head fixation. This is the niche the floating platform system is intended
to fill. Closely related precedents exist in the ferret sound-localisation literature
(approach-to-target paradigms) and in air-lifted platform preparations for rodents; the
novelty here is combining controlled rotational orienting with head fixation and modern
mouse circuit tools.

> **Terminology.** This document follows the *orienting* vs *localisation* distinction
> (Nodal et al., 2008). *Orienting* is the unconditioned, continuously-varying movement of
> eyes/head/body toward a stimulus. *Localisation* (approach-to-target) is the conditioned
> report of perceived location via a discrete choice. This platform is designed to capture
> the **orienting movement itself as a continuous dependent variable**, not merely the
> categorical choice.

---

## 2. Scientific Design Principles

These principles flow directly from the science above and should govern every engineering
trade-off.

1. **Preserve orienting kinematics.** The animal must be able to produce naturalistic
   rotational orienting movements. The platform's mechanics (mass, inertia, friction) must be
   matched to mouse motor capability so that orienting is *natural and low-effort*, not a
   novel motor skill the animal must laboriously acquire. **Movement accessibility is a
   first-order scientific requirement, not an engineering nicety.**

2. **Multiple spatial targets.** Move beyond binary left/right. The animal must be able to
   orient toward stimuli distributed around it, so the system can probe the full spatial map
   of the SC.

3. **Movement as the primary readout.** The dependent variable is the *trajectory* of the
   orienting movement (bearing, latency, amplitude, velocity profile), not just an endpoint.
   This demands continuous, high-resolution measurement of platform orientation.

4. **Multimodal cueing.** Support visual and auditory cues (and ideally the means to add
   somatosensory), so cue modality and coordinate frame can be dissociated — central to
   testing how heterogeneous sensory inputs are transformed into a common motor currency.

5. **Recording- and manipulation-ready from day one.** The system exists to support
   electrophysiology, optogenetics/chemogenetics, and imaging. Compatibility with these tools
   — physical access, electromagnetic compatibility, and above all **temporal
   synchronisation** — must be designed in, not retrofitted.

6. **Trial structure and throughput.** Standardised, self-paced trials with clean
   trial/inter-trial separation, at counts sufficient for statistically powerful,
   circuit-level conclusions.

7. **Respect the animal.** Welfare-conscious head fixation and handling; training timelines
   that are realistic for a cohort within a project's lifetime.

---

## 3. Requirements

### 3.1 Functional requirements (behaviour)

- **F1 — Rotational freedom.** The mouse, while head-fixed, can rotate the floating platform
  beneath it to produce orienting movements through its full natural range, with friction low
  enough that movement is effortless.
- **F2 — Constrained degrees of freedom.** Platform motion is constrained to the behaviourally
  meaningful axes (rotation; optionally fore/aft translation) and prevented in axes that add
  uncontrolled variance.
- **F3 — Multi-target cueing.** Present discrete spatial cues at ≥4 (target: 6) locations
  distributed around the animal, with unambiguous angular separation.
- **F4 — Multimodal cues.** Independently addressable **visual** (per-location LED) and
  **auditory** (per-location or spatialised speaker) cue channels; spare capacity for a third
  modality.
- **F5 — Reward delivery.** Precise, low-latency liquid reward (calibrated µL volumes) at each
  target location, with reliable consumption detection.
- **F6 — Response detection.** Detect the animal's chosen orientation/target reliably and with
  low latency (event timing to ≤1 ms), with no false positives/negatives from the sensing
  modality.
- **F7 — Self-paced trial initiation.** A clear, animal-driven readiness/initiation event that
  separates trials from inter-trial intervals and standardises starting conditions.
- **F8 — Configurable protocol.** Cue duration, cue modality, target set, reward contingency,
  timeout, and inter-trial interval all configurable per session and per training phase,
  without code surgery.

### 3.2 Measurement requirements

- **M1 — Continuous orientation tracking.** Platform angular position sampled continuously at
  high temporal resolution (target ≥100 Hz, ideally faster) and angular precision sufficient
  to reconstruct full orienting trajectories — not just endpoints.
- **M2 — Absolute reference where possible.** Orientation measurement should resolve absolute
  bearing (not only relative drift), or provide robust per-trial re-zeroing against a known
  reference event.
- **M3 — Low-light operation.** Behaviour runs under controlled, mouse-appropriate lighting
  (e.g. long-wavelength / IR illumination); the tracking method must work under these
  conditions without motion blur or illumination that interferes with the visual task.
- **M4 — Video record.** Synchronised overhead and/or side video for behavioural scoring and
  markerless pose estimation.

### 3.3 Technical / systems requirements

- **T1 — Unified timing backbone (highest priority).** A single timing authority records all
  event streams — cue onset/offset, reward, response, orientation samples, video frame
  triggers, and external equipment (ephys/laser) sync — on one clock with **≤1 ms** alignment
  across streams. *Lack of hardware-level synchronisation is the single most likely failure
  mode of a behavioural-neuroscience rig and must be solved at the architecture level.*
- **T2 — Drop detection on every stream.** Every data stream (serial messages, camera frames)
  must carry sequence IDs / frame counters so that dropped samples are detectable and
  correctable post-hoc, never silently misaligning data.
- **T3 — External-equipment sync.** Generate and/or record TTL sync with electrophysiology
  acquisition and optogenetic laser control, so neural data and behaviour share a common time
  base.
- **T4 — Electromagnetic compatibility.** Construction and grounding compatible with
  electrophysiology (shielding / Faraday behaviour; low 50 Hz pickup; no conductive paths to
  electrodes).
- **T5 — Deterministic real-time control.** Time-critical operations (reward, response
  sensing, cue timing) handled with deterministic latency; high-level trial logic and logging
  separated from real-time control.
- **T6 — Automated session lifecycle.** One-command session start that configures and launches
  all subsystems (control, acquisition, cameras) in the correct order; automated logging with
  crash recovery.
- **T7 — Standardised, analysis-ready data.** Output to a documented, standard format (e.g.
  NWB) with an accompanying pipeline from raw streams → synchronised, trial-segmented dataset
  → quality-control metrics → figures, minimising manual steps.
- **T8 — Maintainability & reproducibility.** Modular, serviceable hardware (swappable
  per-target modules); liquid-tolerant construction; documented build so additional rigs can
  be replicated by the lab.
- **T9 — Session endurance.** Sustain a full behavioural session (target: continuous operation
  for multi-hour daily use) without power, thermal, or buffer constraints forcing early
  termination.

### 3.4 Non-functional requirements

- **N1 — Welfare.** Graduated head-fixation habituation; comfortable, low-stress fixation;
  bounded session durations.
- **N2 — Trainability.** A naïve mouse should reach criterion performance within a timeframe
  practical for a 10–12 animal cohort. *(Because mechanical accessibility (F1) directly
  governs trainability, the platform's inertia/friction budget should be validated against
  mouse motor output early.)*
- **N3 — Cost & scalability.** Favour designs that allow multiple rigs to run in parallel on
  shared infrastructure without duplicating the most expensive components.

---

## 4. Proposed System Description

A concrete architecture that satisfies the requirements above. Subsystems are described by
*function and constraints*; exact components are deliberately left open where the science does
not yet dictate them.

### 4.1 Mechanical core — the floating platform

- A lightweight, rigid platform (e.g. machined low-density foam) suspended on a **cushion of
  air** above a perforated, regulated air table, giving near-frictionless motion.
- Air supply regulated to lift the loaded platform (platform + onboard electronics) at a
  stable hover height with negligible residual friction.
- **Motion constrained** to the behaviourally relevant axes — primarily rotation (and
  optionally controlled fore/aft translation) — using physical guides, while blocking
  lateral/vertical excursions.
- **Inertia budget:** the rotational moment of inertia of the loaded platform must be small
  relative to the torque a mouse can comfortably generate. This is the central mechanical
  constraint and should be minimised (light platform, minimal onboard mass, balanced about the
  rotation axis) and validated empirically against mouse motor capability before committing.
- A low surrounding wall prevents falls without restricting movement and provides a mounting
  surface for cue/reward modules and/or tracking references.

### 4.2 Head-fixation assembly

- Standard, welfare-conscious head-bar fixation (off-the-shelf optomechanics) positioning the
  animal centrally over the platform's rotation axis, leaving the body free to drive platform
  rotation.
- Fixation height and posture chosen so that orienting is produced by natural weight-shift /
  stepping, not an artificial motor strategy.

### 4.3 Target / cue / reward modules

- A ring of **modular, independently serviceable target stations** at fixed angular intervals
  (target: 6 at 60°, minimum 4 at 90°).
- Each station integrates: a **visual cue** (high-brightness LED), an **auditory cue**
  (speaker), a **liquid reward** spout (solenoid/pinch-valve, gravity- or pump-fed, calibrated
  volume), and a **response/consumption sensor** (e.g. IR beam-break — preferred over
  capacitive sensing for reliability).
- Modularity allows a faulty station to be swapped without disturbing the rig and supports
  rapid prototyping of new configurations.

### 4.4 Orientation tracking

Continuous platform-orientation measurement is mandatory (M1–M3). The architecture should
support — and ideally fuse — complementary methods, chosen for robustness under low-light
operation:

- An **inertial measurement unit (IMU)** mounted on the platform for high-rate relative
  orientation, re-zeroed each trial against a known reference event (giving high within-trial
  accuracy).
- And/or an **absolute optical reference** (e.g. fiducial markers read by a camera, or an
  optical/magnetic angular encoder on the rotation axis) for absolute bearing and drift
  correction — noting that any camera-based method must tolerate the IR/low-light regime and
  platform motion without blur.
- **Markerless pose estimation** (e.g. DeepLabCut) on synchronised video as an independent
  cross-check and for body/limb kinematics.

### 4.5 Control and acquisition architecture

The defining architectural commitment: **separate real-time control from timing/logging.**

- **Real-time controller** (microcontroller): owns deterministic, time-critical hardware
  control — cue timing, reward actuation, response sensing — and nothing else. Prefer a
  microcontroller with **native USB** (low, deterministic serial latency) over legacy
  serial-to-USB bridges.
- **Independent acquisition / timing backbone (DAQ):** a dedicated device that does nothing
  but sample *all* digital/analogue lines (cue states, sensor states, reward signals) plus
  external sync (camera frame TTLs, ephys clock, laser control) at high rate (target ≥1 kHz,
  ideally ≥10 kHz) onto **one clock**. Every message carries a sequence ID (T2).
- **Host computer:** runs high-level trial logic / state machine, configuration, multi-rig
  orchestration, real-time performance metrics, and all file writing (offloading I/O from
  embedded devices). Launches the whole session with one command (T6).
- **Cameras:** programmatically controlled (not via a GUI), emitting a TTL per frame into the
  DAQ and saving hardware frame IDs, so dropped frames are detectable and video aligns to the
  behavioural timeline to the frame.

This separation is what delivers T1–T3: a single timestamped record into which neural,
video, and behavioural events all map without clock drift.

### 4.6 Software, data, and analysis pipeline

- **Configurable protocol engine** encoding training phases and trial parameters via
  configuration, not code edits (F8).
- **Standardised output** (NWB or equivalent) bundling synchronised event streams, orientation
  traces, video references, and trial metadata, with crash-recovery/backup.
- **Automated post-processing**: parse DAQ logs → reconstruct trials → align video via frame
  IDs → run pose estimation → emit a trial-segmented dataset plus QC metrics, with minimal
  manual intervention.
- **Cohort/session data model** giving programmatic access (query by animal, date, completion
  status) so analyses run over whole cohorts in little code.

### 4.7 Behavioural protocol (intended)

1. **Initiation.** The animal produces a defined, self-paced readiness event that standardises
   the starting orientation and separates trials from inter-trial intervals (F7). The
   initiation requirement must be designed to genuinely standardise starting conditions
   (minimising preparatory movement before cue onset).
2. **Cue.** One target is cued (visual and/or auditory), with configurable duration.
3. **Orienting response.** The animal rotates the platform to bring the cued target into
   reach; the full trajectory is recorded.
4. **Outcome.** Correct orientation → immediate calibrated reward. Incorrect → brief timeout,
   no reward.
5. **Reset.** Return to initiation for the next self-paced trial.

---

## 5. Key Open Design Questions / Risks

Flagged up front because they drive the riskiest decisions:

- **Mechanical accessibility vs. control (F1/N2).** Can the inertia/friction budget be made
  low enough that a head-fixed mouse produces natural orienting rotations *and* learns the task
  in a cohort-feasible timeframe? This is the make-or-break biological-mechanical question and
  should be de-risked first with simple motor-capability tests.
- **Tracking under low light (M1–M3).** Reconciling absolute, blur-free, high-rate orientation
  measurement with the dim/IR lighting the visual task requires. IMU + absolute-encoder fusion
  is the conservative hedge.
- **Initiation standardisation (F7).** Defining an initiation behaviour that truly fixes
  starting orientation without permitting covert preparatory movement.
- **Synchronisation throughput (T1).** Achieving the target sample/frame rates without
  message loss; choice of controller and DAQ should leave headroom above the minimum.

---

## 6. Key References

Foundational citations that justify the design intent.

**Superior colliculus, orienting, and spatial priority**
- May, P. J. (2006). The mammalian superior colliculus: laminar structure and connections.
  *Prog. Brain Res.* 151. — SC anatomy and laminar organisation.
- Sparks, D. L. & Mays, L. E. (1990). Signal transformations required for the generation of
  saccadic eye movements. *Annu. Rev. Neurosci.* 13. — SC sensorimotor transformation.
- Fecteau, J. H. & Munoz, D. P. (2006). Salience, relevance, and firing: a priority map for
  target selection. *Trends Cogn. Sci.* 10. — priority-map concept.
- Krauzlis, R. J., Lovejoy, L. P. & Zénon, A. (2013). Superior colliculus and visual spatial
  attention. *Annu. Rev. Neurosci.* 36. — SC role in attention beyond movement.
- Knudsen, E. I. (2018). Neural circuits that mediate selective attention: a comparative
  perspective. *Trends Neurosci.* 41. — gating and selective attention.
- de Malmazet, D. & Tripodi, M. (2023). Collicular circuits supporting the perceptual, motor
  and cognitive demands of ethological environments. *Curr. Opin. Neurobiol.* 82.
- Masullo, L. et al. (2019). Genetically defined functional modules for spatial orienting in
  the mouse superior colliculus. *Curr. Biol.* 29. — Pitx2+ SC output neurons; target
  population for manipulation.

**Behavioural paradigms and precedents**
- Parsons, C. H., Lanyon, R. G., Schnupp, J. W. H. & King, A. J. (1999). *J. Neurophysiol.* 82.
  — ferret sound-localisation / approach-to-target precedent.
- Nodal, F. R. et al. (2008). Sound localisation behaviour in ferrets: acoustic orientation vs
  approach-to-target. *Neuroscience* 154. — the orienting vs localisation distinction.
- Robbins, T. W. (2002). The 5-choice serial reaction time task. *Psychopharmacology* 163. —
  multi-choice spatial reporting paradigm.
- Guo, Z. V. et al. (2014). Procedures for behavioral experiments in head-fixed mice. *PLoS
  ONE* 9. — head-fixed behavioural standards.
- Kislin, M. et al. (2014). Flat-floored air-lifted platform: combining behaviour with
  microscopy/electrophysiology on awake freely moving rodents. *JoVE*. — air-lifted platform
  precedent.
- Angelaki, D. et al. (2025). A brain-wide map of neural activity during complex behaviour.
  *Nature* 645. — mice performing complex standardised tasks.

**Tools and infrastructure**
- Denman, D. J. et al. (2017). Chromatic pathways in the mouse dLGN. *J. Neurosci.* 37. —
  basis for long-wavelength lighting choices for mouse vision.
- Mathis, A. et al. (2018). DeepLabCut: markerless pose estimation. *Nat. Neurosci.* 21.
- Rübel, O. et al. (2022). The Neurodata Without Borders ecosystem. *eLife* 11. — standardised
  data format.
