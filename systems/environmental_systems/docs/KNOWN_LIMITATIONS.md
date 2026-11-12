# Environmental Systems — Known Limitations

## Not done / not verified

* **No Unreal compilation, editor run or networked session.** Authority,
  dedup, versioning, disconnects and replication deltas are tested
  in-process only.
* **Project systems absent:** mission, tactical AI, LLSignalNetwork, Robot
  Damage System and profile/persistence code were not in the repository.
  Ports exist; adapters do not. Comm arrays have no LLSignalNetwork effect yet.
* **Determinism** verified only on GCC 13 / x86-64 Linux, same build.
* **MSVC** flags configured, not tried.
* This branch depends on the unmerged AIS PR and adds an `Alarm` category to
  AIS.

## Model simplifications (by design)

* Power is a gameplay abstraction (kW budgets, no voltage, phases or line
  losses). Within an island supply is pooled regardless of topology. Breaker
  load counts served demand downstream; with redundant feeders, a consumer's
  load is counted under every upstream breaker that can reach it
  (conservative).
* Load shedding is first-fit by priority. A big consumer may be shed while a
  smaller, lower-priority one still runs.
* Generator overload is island-wide: all running generators in the island
  share the same trip timer.
* Batteries have one rate per solve; partial charge is integrated lazily and
  can be off by less than one tick of energy.
* Emergency lights reset to a full battery whenever mains return.
* Security sensors react to interactions and to presence the host reports
  (overlap events); they have no vision cone.
* A door losing power mid-motion snaps back to its previous resting state.
* Client mirror approximates `health` from damaged/destroyed flags only.
* Mission snapshots are in-memory structs (no binary serialisation format yet);
  world state uses a simple versioned text format.

## Gameplay tuning caveats

* Relay Outpost numbers (fuel 60 s, 12 kW light breaker, 40%-health backup)
  were chosen to showcase consequences in a short demo and need playtesting.
* Damaged generator backfire chance is deterministic per attempt count. Players
  can learn it within a session, which may be fine, but it is worth knowing.
