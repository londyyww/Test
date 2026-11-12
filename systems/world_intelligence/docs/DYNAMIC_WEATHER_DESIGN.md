# Dynamic Weather — Design

## Goal

Weather changes how the raid plays. Each condition affects **different
channels**; nothing is a blanket "rain = everything worse" debuff, and
weather never makes anyone invisible.

## States and effects (defaults; all data-driven in `[weather.<Kind>]`)

| Kind | rain | wind | fog | visual × | thermal × | radio × | other |
|---|---|---|---|---|---|---|---|
| Clear | 0 | 0.10 | 0 | 1.00 | 1.00 | 1.00 | surfaces dry out |
| LightRain | 0.35 | 0.20 | 0.05 | 0.85 | 0.90 | 1.00 | slow wetting, exposed fires slowed |
| HeavyRain | 0.90 | 0.40 | 0.10 | 0.60 | 0.70 | 0.90 | puddles, rare gusts, weak exposure, fires suppressed |
| Fog | 0 | 0.05 | 0.85 | 0.30 | 0.90 | 1.00 | sight only; ears and thermal barely affected |
| StrongWind | 0 | 0.85 | 0 | 0.95 | 1.00 | 0.95 | frequent gusts (loose metal bangs), fire spreads ×1.8 |
| Storm | 1.00 | 0.90 | 0.20 | 0.45 | 0.65 | 0.60 | thunder, strong infrastructure exposure, gusts |

How each channel works:

* **Acoustics:** `rain`/`wind` are pushed into AIS's existing
  `WeatherState`. AIS already models rain hiss (high band) and wind rumble
  (low band) noise floors and attenuation, so masking, detection
  thresholds and localisation error all come from AIS itself and aren't
  duplicated here. Distant acoustic *reports* lose confidence in rain and
  wind (`acoustic_confidence_loss`, `acoustic_confidence_ref_cm`).
* **Wind disturbances:** gusts at designer anchors (`[gust.<name>]`,
  `metallic = true` for loose sheet metal) become AIS events
  (`MetalImpact` or `Environmental`). Robots may investigate false alarms,
  and players can move under cover of them. Storm thunder is a loud
  `Environmental` event.
* **Vision:** `visual_range_mul` (fog strongest). The integration layer also
  applies smoke and lighting, with floors (`min_visual_range_factor`,
  `min_detection_range_cm`).
* **Thermal:** a separate `thermal_range_mul`. Cold rain hurts it; fog
  barely does; smoke much less than vision; fire clutter hurts it.
* **Surfaces:** wetness accumulates while it rains and dries afterwards.
  Above `puddle_wetness`, player footsteps on ordinary ground become
  `Water`-surface footsteps: louder (splashing), though rain may still
  mask them while it falls. This is the post-rain risk window.
* **Infrastructure:** `electrical_exposure` (health/s) wears down exposed
  power-grid nodes that are *already damaged* (`[world] exposed_nodes`).
* **Hazards:** `fire_spread_mul`, `fire_rain_suppression`, wind
  dispersing gas and smoke, wetness extending electrical hazards and
  spreading chemicals.
* **Squads:** `comms_range_mul` scales radio and relay range.

## Transitions

* Linear blend between two profiles over `transition_ticks`.
  `RequestWeather(kind, ticks)` is the server/mission-director hook; a
  request for the current or already-targeted state is rejected as a
  duplicate.
* With `auto_advance`, a deterministic Markov chain (`next.<Kind>` weights,
  `hold_min_ticks..hold_max_ticks`) picks the next state using a seeded
  SplitMix hash.

## Cost

Evaluated every `update_interval_ticks` (default 1 s) with no per-frame
work. It never touches robots or objects directly; consumers read one
`WeatherSnapshot` and react to `WeatherChanged` when its `version` bumps.
Gust and thunder timers are single scheduled ticks.

## Known limitations

* Weather is global: no regional cells, no indoor shelter beyond each
  consumer's own `exposed`/interior handling.
* No time of day; lighting comes from the power grid only.
* Thunder positions are uniform within bounds.
* Wetness is global (no per-surface drainage).
