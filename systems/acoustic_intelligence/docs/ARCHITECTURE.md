# AIS Architecture

## Design goals

1. **Hear before see.** Sound is the primary early-warning channel.
2. **Imperfect knowledge.** Robots receive estimates with error, never truth.
3. **Readable and exploitable.** Players can reason about it: quiet surfaces,
   rain, walls, generators, decoys and mistakes all matter in predictable ways.
4. **Server authority, cheap, bounded.** Fixed costs per tick, nothing a
   client can inject.

## Data flow (one server tick)

```
 server gameplay ──SubmitEvent()──►  AcousticEventBus  ◄── DecoySystem.Update()
 (weapons, movement,                 authority check
  doors, impacts…)                   validation
                                     dedup (source+category+place+window)
                                     token bucket per source / per-tick cap
                                     bounded priority queue (evicts lowest)
                                           │ Drain (≤ maxDrainPerTick)
                                           ▼
                          AcousticWorld::Step()
             ┌──────────── broadphase: uniform grid, per-band audible radius
             │             budget: maxPerceptionChecksPerTick (defers excess)
             ▼
   AcousticPropagationModel::Evaluate(event, robot)     (one occlusion trace)
        per band: source + surface − spreading − air − walls − weather − transition
        noise floor: base(int/ext) ⊕ rain ⊕ wind ⊕ ambient sources
             │ effective SNR = max_b(SNR_b + robot sensitivity_b)
             ▼
   PerceivedStimulus  (estimate = truth + hashed error ≤ uncertainty radius)
             │ robot inbox (bounded; keeps the loudest)
             ▼
   RobotAcousticBrain::Update()   (every N ticks, staggered by id)
        decay → fuse into traces → suspicion → state machine → AcousticDirective
             │                                    │
             │ AcousticReport (estimate only)     ▼
             ▼                             host AI (LLTacticalDecision adapter)
   ISignalNetworkPort (latency/range/rate)  moves, looks, fights
             │
             ▼ next ticks: shared perceptions (weighted, inflated uncertainty)

   Separately: CosmeticAudioCue list per client (relevancy only) → audio playback
```

## Modules

| File | Responsibility |
|---|---|
| `AisTypes.h` | `Vec3` (cm, +Z up), `Tick`, band arrays, SplitMix64 hashing, `StateHasher` |
| `AcousticEvent.h` | 13 categories (12 original + `Alarm` for facility sirens), 9 surfaces, the `AcousticEvent` record |
| `AcousticConfig.h/.cpp` | All coefficients, SENTINEL/WRAITH, default decoys, validation, strict INI loader |
| `AcousticPropagation.h/.cpp` | `IAcousticEnvironment` port, `BoxAcousticEnvironment` reference implementation, propagation model |
| `AcousticEventBus.h/.cpp` | Server intake |
| `RobotAcousticBrain.h/.cpp` | Memory + state machine per robot |
| `SignalRelay.h/.cpp` | `ISignalNetworkPort` + `LocalSignalRelay` stand-in |
| `DecoySystem.h/.cpp` | Deployable artificial emitters |
| `AcousticWorld.h/.cpp` | Orchestrator, broadphase, budgets, cosmetic relevancy, state hash |

## Stimulus model

Each event carries: server-assigned id, true category, *apparent* category
(what it sounds like — differs only for decoys), position, emit tick,
loudness (gameplay dB @ 1 m), per-band spectrum offsets (low/mid/high),
surface, duration, optional source id and an artificiality score (0..1).
Defaults per category live in `CategoryDescriptor` (loudness, spectrum,
duration, threat, queue priority, hostile flag, whether metronomic repetition
is suspicious).

Robots only ever see the apparent category, an estimated position, an
uncertainty radius, an effective SNR and an "artificial cue" proportional to
how clearly they heard a synthetic signal.

## Propagation (gameplay approximation)

Per band *b*:

```
received_b = loudness + spectrum_b + surface_b
           − 20·k·log10(max(d, 1 m) / 1 m)      k = 1.0 open, 0.85 inside one room
           − air_b · d/100 m
           − Σ walls (loss_b + lossPerMetre_b · thickness)   capped at 70 dB
           − (rain·rainAtt_b + wind·windAtt_b) · d/100 m     if path is exposed
           − 6 dB if the path crosses an interior/exterior boundary
floor_b    = base_b(int|ext) ⊕ rain·rainNoise_b ⊕ wind·windNoise_b ⊕ ambient_b   (energetic sum)
heard      ⇔ max_b(received_b − floor_b + sensitivity_b) ≥ detectionSnr
```

Low frequencies pass walls; highs give localisation and die in rain. So
gunfire through concrete is heard as a thump, rain hides footsteps, and a
generator hides everything near it. Localisation error grows with distance,
walls, interiors (reverb) and low SNR, and shrinks for sustained sounds and
skilled listeners.

## Robot brain

**Traces** are remembered disturbances: estimate, uncertainty, confidence,
hit counts, threat, artificial evidence and fruitless-search count. A new
perception merges into the nearest compatible trace (footstep/sprint/landing
form one "movement" family) using inverse-variance fusion, so repeated
hearing tightens the estimate. Confidence and suspicion decay exponentially
by half-life; uncertainty grows over time because the source may have moved.
Traces below `forgetConfidence` are forgotten; memory is capped.

**Suspicion gain** per perception =
`baseGain · SNR factor · distance factor · interest · threat · repetition`,
capped at `maxSingleEventGain`. Validation enforces
`maxSingleEventGain < combat − suspicious`, so **no single sound can take an
unaware robot to combat**.

**States**

| State | Enter when | Robot does |
|---|---|---|
| Unaware | default / calmed down | host patrol behaviour |
| Suspicious | suspicion ≥ suspicious threshold | looks toward best estimate (`ObjectiveHeld` if it wants to go but its objective outranks the noise) |
| Investigating | drive ≥ investigate threshold **and** drive ≥ objectivePriority × commitment, within range | moves to the estimate; retargets with hysteresis |
| Searching | arrived with nothing found, source lost, or combat contact lost | visits golden-angle points within the uncertainty radius; chases if the sound reappears elsewhere |
| Combat | visual contact (host) **or** suspicion ≥ combat + hostile trace with ≥1 *direct* hit + tight estimate + in range | engages; drops to Searching after `combatLoseTicks` without evidence |
| ReturningToPatrol | search exhausted, objective outranks noise, or lost interest | walks back to where it left its route; then Unaware or Suspicious |

`drive = suspicion × interest(trace)`; interest falls when a trace is judged
artificial (per profile) and after each fruitless search (habituation).

## Archetypes

| | SENTINEL | WRAITH |
|---|---|---|
| Sensitivity (low/mid/high) | +10/+10/+10 balanced | +2/+10/+13 high-band specialist, deaf through walls |
| Localisation | 1.0 | 1.4 (tighter) |
| Investigation range | 40 m | 70 m |
| Repetition bonus | 0.45 (spots repeated disturbances) | 0.15 |
| Decoy discrimination | 0.35 | 0.85 |
| Once it judges a signal artificial | interest × 0.25 (ignores it) | interest × 1.4 (hunts the device and searches wider) |
| Signal network | receives | broadcasts + receives |
| Objective commitment | 1.0 | 0.7 |

New archetypes are pure data (`[robot.NAME]`, optional `base = SENTINEL`).

## Decoys

Decoys are ordinary server events with `category = Decoy`, an apparent
category they mimic and an artificiality score. Robots see them through the
same imperfect hearing. Tells: synthetic timbre (artificiality × clarity ×
discrimination) and metronomic repetition (for categories marked
`regularity_is_suspicious`). Outcomes differ per robot: an idle Sentinel
checks once and habituates; a guarding Sentinel turns to look but holds;
a Wraith flags it quickly, hunts it repeatedly and tells others it is fake —
so killing the Wraith first makes decoys work better.

## Signal network

Broadcasting robots send `AcousticReport`s (estimate, uncertainty,
confidence, apparent category, claim flag, "looks artificial" flag) when
their target trace gets new direct evidence. Receivers treat them as weighted,
vaguer perceptions; shared-only traces can raise suspicion but **never
satisfy the combat gate**. Investigation claims make higher-id robots take
deterministic flanking positions instead of stacking on one point.

## Authority & multiplayer model

* Only `AuthorityRole::Server` worlds tick or accept events; `ClientClaim`
  origin is always rejected. In Unreal the subsystem should not exist on
  clients at all.
* Robot decisions read only server state (events, host state set by server
  AI). Clients receive `CosmeticAudioCue`s, chosen per listener by audible
  radius and capped per tick; cues carry the apparent signature only.
* Determinism: fixed ticks, no clocks, no `rand()`, hashed noise keyed on
  (seed, event id, robot id), robots processed in id order, sorted queues.
  The tick-level `StateHash()` supports replay checks and desync triage.
