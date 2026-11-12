# Robot Squad Coordination — Design

## Goal

Robot groups behave like a coordinated military unit with imperfect,
delayed and sometimes missing information. They are not a hive mind. The
portable core produces **tactical intents**; movement and navigation stay
in Unreal.

## Roles (data-driven)

`[role.<NAME>]` keys: `can_investigate`, `can_observe`, `can_flank`,
`can_defend`, `investigate_weight`, `comms_range_cm`,
`observation_standoff_cm`, `report_quality`.

| Role | Shipped behaviour |
|---|---|
| SENTINEL (built-in) | Investigates (preferred, weight 1.2), flanks, defends, 50 m radio |
| WRAITH (built-in) | Observes from 20 m standoff, prefers watching to walking in (weight 0.6), 90 m radio, better reports |
| WARDEN (data only, `industrial_site_world.ini`) | Cannot investigate; defends; 25 m radio |

Nothing in the coordinator branches on role names.

## Information model

* `ContactReport`: reporter, per-reporter id (dedup), kind (`Acoustic`,
  `Visual`, `NegativeSearch`), threat class (`Disturbance`, `Movement`,
  `Infrastructure`, `Hostile`), estimated position, uncertainty,
  confidence, tick.
* Each robot keeps its **own** contact board. Own reports fuse immediately
  (confidence scaled by `report_quality`).
* Sharing: reports mature after `report_latency_ticks` and are delivered
  **only to robots in the reporter's current comms component**, at
  `shared_confidence_mul` (second-hand is worth less). Robots that are
  cut off never receive them.
* Fusion: nearby reports merge using inverse-variance position weighting,
  combined confidence `1-(1-a)(1-b)`, the maximum threat class, and a
  bitmask of sources. Far-apart reports stay separate contacts (conflicting
  reports).
* `NegativeSearch` ("searched, nothing found") multiplies confidence by
  `negative_report_factor` and marks the contact *cleared* until new
  positive evidence arrives.
* Age: confidence halves every `confidence_half_life_ticks`; uncertainty
  grows over time; contacts below `forget_confidence` are dropped; boards
  are capped at `max_contacts`.
* Integration lowers confidence of distant acoustic reports in rain and
  wind (`WeatherSystem::AcousticReportConfidenceScale`).

## Communications

Recomputed every coordination cycle (default 0.5 s):

* Radio link if `distance ≤ min(range_a, range_b) × weather.comms_range_mul`.
* Damaged comms component: range × `damaged_comms_range_mul` and an
  intermittent deterministic dropout (`damaged_drop_chance` per cycle).
* Destroyed comms or a dead robot: isolated.
* Optional infrastructure relay (`relay_node`): if the power grid reports
  that CommArray as powered and switched on, every robot within
  `relay_range_cm × weather mul` is linked. A power failure removes the
  relay.
* Leader is the lowest robot id with a working radio. Robots outside the
  leader's component are **disconnected** and emit `CommsLost` /
  `CommsRestored` events.

## Assignment (leader's component only)

Each cycle, in deterministic order:

1. Candidate contacts: confidence ≥ `investigate_min_confidence`
   (half that for contacts already being worked and not cleared), sorted by
   `confidence × threat_weight`.
2. Keep `floor(min_patrol_fraction × group)` robots on patrol.
3. Per contact:
   * **Investigate**: the closest capable robot, weighted by role. Sticky:
     the current investigator's cost is halved and a robot already
     observing is discouraged. It switches to **Search** once it is within
     the uncertainty area, and stays in Search.
   * **Observe**: the closest capable scout, at standoff on its side of the
     contact.
   * **Flank**: for confident `Hostile` contacts, a capable robot takes a
     perpendicular offset.
4. **Defend**: when a confident hostile is known, remaining defenders move
   to the squad `defend_point`.
5. Everyone else: **Patrol**, advancing waypoints on arrival. This is how
   patrols are restored when contacts fade or are searched empty.

Disconnected robots keep executing their last squad order for
`stale_order_ticks`. After that they switch to **LocalAutonomous**: they
act on their own board and their own AIS hearing.

## Integration levers (no duplication)

* Perception stays in AIS. The integration layer converts each robot's AIS
  traces into reports (fresh direct hits only), and turns AIS traces that
  were searched without result into `NegativeSearch`.
* Squad to AIS: `ObjectivePriorityFor()` sets AIS
  `RobotHostState::objectivePriority`, the existing lever that stops a robot
  from wandering off. Investigate/Search/Flank/LocalAutonomous → 0,
  Patrol → 55 (only strong local evidence diverts it), Observe/Defend → 90.
* AIS's own robot-to-robot report sharing goes through `SquadSignalPort`,
  which only delivers between robots the squad says can communicate.
* `WorldIntelligence::OrderFor(robot)` merges both. AIS combat, and a
  robot's own investigation while patrolling or autonomous, win; otherwise
  the squad intent is used.
* Robot Damage System (UNVERIFIED): calls
  `SetRobotBody(id, pos, alive, CommsHealth)`.
* LLSignalNetwork (UNVERIFIED, not present): the comms model is
  self-contained. An adapter could replace `ComputeComms` link decisions;
  see the Unreal contract.

## Known limitations

* No line-of-sight or terrain effects on radio, only distance, weather,
  damage and relays.
* Leader election is "lowest id", not tactical.
* Observe and flank points are geometric. The navmesh must project them,
  and they may be unreachable.
* Reports dropped while a robot is disconnected are not re-sent
  (no store-and-forward).
* Up to 32 robots per squad (bitmask of sources).
