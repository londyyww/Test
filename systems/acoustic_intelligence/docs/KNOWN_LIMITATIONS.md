# AIS — Known Limitations

## Not done / not verified

* **No Unreal compilation or runtime test.** The integration contract is a
  specification. UE API names in it are proposals.
* **Project systems absent.** LLTacticalDecision, LLSignalNetwork,
  LLProjectileBallistics, Robot Damage System, existing robot AI and audio
  event interfaces were not in the repository. AIS defines ports for them but
  no adapter exists, and it may overlap with features they already have
  (e.g. a search behaviour or robot messaging). Reconcile before shipping.
* **No multiplayer validation.** Authority rules are enforced and tested in
  the core (client worlds are inert, client-origin events rejected, cosmetic
  cues carry no AI data), but no networked session was run.
* **Determinism** was verified on one compiler and platform (GCC 13,
  x86-64 Linux). The core uses `std::pow`, `std::log10`, `std::sqrt`, `cos`
  and `sin`; results may differ in the last bits across compilers,
  standard libraries or `/fp:fast`-style flags. Replays are reliable on the
  same server build, not across builds or platforms.
* **MSVC** flags are configured but were not tried.

## Model simplifications (by design)

* Straight-line occlusion only: no diffraction around corners, no portals
  through open doors, no reflections. An open doorway next to a closed wall
  is treated as the wall if the segment hits it. A portal/zone graph would be
  the next step if level design needs it.
* Three frequency bands; no Doppler, no reverberation tails beyond a
  localisation penalty and a lower in-room spreading factor.
* Ambient masking ignores occlusion between the ambient source and listener.
* Broadphase radius is conservative (no air absorption, smallest spreading
  factor), so some candidates are evaluated and then rejected.
* Event duration only improves localisation; long sounds are not re-heard
  continuously. Continuous machinery should be modelled as ambient sources
  plus discrete "started/stopped" events.
* `BoxAcousticEnvironment` scans all walls linearly per trace; it is a
  reference/test implementation, not meant for production levels.

## Behavioural caveats

* The investigate/search loop uses straight-line movement in tests and the
  demo; real navigation (unreachable points, doors, stairs) is the host's job
  and may need search points projected to the navmesh.
* Support/flanking roles use "lowest id leads". That is deterministic but not
  tactically aware; LLTacticalDecision should own squad roles if it already
  does.
* A robot standing on a still-sounding decoy will search, habituate and walk
  away; it does not "find" the device. Destroying decoys needs vision/
  interaction logic on the Unreal side.
* Shared reports can raise suspicion above the combat threshold, but never
  trigger combat without the robot's own direct hearing or sight. That is
  intentional and may need tuning for squads.
* Tuning values are first-pass gameplay numbers chosen to produce readable
  ranges in clear weather (footstep ≈ 13 m for a SENTINEL and ≈ 22 m for a WRAITH;
  sprint ≈ 60 m on concrete and further on metal, which may prove too generous; gunfire
  heard at hundreds of metres). They need playtesting.
