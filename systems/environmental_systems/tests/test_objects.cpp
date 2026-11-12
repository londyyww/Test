// Interactive objects: doors, gates, emergency lights, alarms, sensors, terminals, extraction.
#include "EnvTestHelpers.h"
#include "TestFramework.h"

using namespace llenv;
using envtest::Header;
using envtest::Node;
using envtest::Rig;
using A = InteractionAction;
using R = InteractionResult;

namespace {

FacilityConfig Loose() {
    FacilityConfig c;
    c.requestBurst = 1000.0;
    return c;
}

std::string SecurityBlock() {
    return Header() + Node("G", "Generator", "A", "", "capacity_kw = 50\nstarts_running = true\nstartup_ticks = 10\n") +
           Node("BRK", "Breaker", "A", "G", "capacity_kw = 40\n") +
           Node("ALARM", "Alarm", "A", "BRK", "demand_kw = 1\nalarm_interval_ticks = 30\nalarm_duration_ticks = 300\nnoise_db = 96\n", "0, 0, 300") +
           Node("SECURE", "Door", "A", "BRK",
                "demand_kw = 1\nstarts_locked = true\nfail_mode = FailSecure\nkey_item = Keycard\nforce_item = Crowbar\nalarm = ALARM\nmotor_ticks = 30\n",
                "1000, 0, 0") +
           Node("SAFE", "Door", "A", "BRK", "demand_kw = 1\nstarts_locked = true\nfail_mode = FailSafe\nmanual_work = 120\n", "2000, 0, 0") +
           Node("GATE", "SecurityGate", "A", "BRK",
                "demand_kw = 3\nmanual_participants = 2\nmanual_work = 200\ninteract_range_cm = 400\n", "3000, 0, 0") +
           Node("EL", "EmergencyLight", "A", "BRK", "demand_kw = 0.2\nemergency_battery_ticks = 100\nemergency_intensity = 0.3\nlight_radius_cm = 1000\n", "500, 0, 300") +
           Node("LAMP", "Light", "A", "BRK", "demand_kw = 2\nlight_radius_cm = 1000\n", "500, 0, 300");
}

}  // namespace

AIS_TEST(Door_PoweredMotorVersusUnpoweredCrank) {
    Rig r(SecurityBlock(), AuthorityRole::Server, Loose());
    CHECK(r.load.ok);
    r.Join(1, "SAFE");
    r.Run(1);
    // Powered and locked fail-safe door: lock holds while powered.
    CHECK(r.f->IsLocked(r.N("SAFE")));
    CHECK(r.Do(1, "SAFE", A::Activate) == R::RejectedLocked);
    CHECK(r.Do(1, "SAFE", A::Crank) == R::RejectedInvalidState);  // motor available
    // Cut power: fail-safe lock drops, motor is dead, cranking works.
    r.MoveTo(1, "BRK");
    CHECK(r.Do(1, "BRK", A::Deactivate) == R::Applied);
    CHECK(!r.f->IsLocked(r.N("SAFE")));
    r.MoveTo(1, "SAFE");
    CHECK(r.Do(1, "SAFE", A::Activate) == R::RejectedNoPower);
    CHECK(r.Do(1, "SAFE", A::Crank) == R::ChannelStarted);
    r.Run(119);
    CHECK(r.S("SAFE").door == DoorState::Closed);
    CHECK(!r.f->IsPassable(r.N("SAFE")));
    r.Run(1);
    CHECK(r.S("SAFE").door == DoorState::Open);
    CHECK(r.f->IsPassable(r.N("SAFE")));
    CHECK(r.rec.CountEvents(FacilityEventType::NavBlockChanged, r.N("SAFE")) == 1);
    // Power back: lock re-engages its *intent*, but the door is already open.
    r.MoveTo(1, "BRK");
    CHECK(r.Do(1, "BRK", A::Activate) == R::Applied);
    CHECK(r.f->IsLocked(r.N("SAFE")));
    r.MoveTo(1, "SAFE");
    CHECK(r.Do(1, "SAFE", A::Deactivate) == R::Applied);  // motor closes it
    CHECK(r.S("SAFE").door == DoorState::Closing);
    r.Run(45);
    CHECK(r.S("SAFE").door == DoorState::Closed);
}

AIS_TEST(Door_FailSecureNeedsKeycardOrForceAndForcingRaisesAlarm) {
    Rig r(SecurityBlock(), AuthorityRole::Server, Loose());
    r.Join(1, "SECURE");
    r.Run(1);
    CHECK(r.Do(1, "SECURE", A::Activate) == R::RejectedMissingItem);
    CHECK(r.Do(1, "SECURE", A::Force) == R::RejectedLocked);  // live maglock
    r.inv.Give(1, "Keycard");
    CHECK(r.Do(1, "SECURE", A::Activate) == R::Applied);
    CHECK_EQ(r.inv.Count(1, "Keycard"), 1);  // keycards are not consumed
    r.Run(30);
    CHECK(r.S("SECURE").door == DoorState::Open);

    // Second rig: no keycard; cut power -> fail-secure stays locked -> force it.
    Rig q(SecurityBlock(), AuthorityRole::Server, Loose());
    q.Join(1, "BRK");
    q.Run(1);
    CHECK(q.Do(1, "BRK", A::Deactivate) == R::Applied);
    q.MoveTo(1, "SECURE");
    CHECK(q.f->IsLocked(q.N("SECURE")));
    CHECK(q.Do(1, "SECURE", A::Crank) == R::RejectedLocked);
    CHECK(q.Do(1, "SECURE", A::Force) == R::RejectedMissingItem);
    q.inv.Give(1, "Crowbar");
    CHECK(q.Do(1, "SECURE", A::Force) == R::Applied);
    CHECK(!q.f->IsLocked(q.N("SECURE")));
    CHECK_EQ(q.rec.CountSounds(SoundKind::DoorForced), std::size_t{1});
    // The alarm is latched but silent: its circuit is dead.
    CHECK(q.S("ALARM").alarmActive);
    CHECK_EQ(q.rec.CountSounds(SoundKind::AlarmPulse), std::size_t{0});
    CHECK_EQ(q.f->ActiveAlarms(), std::size_t{0});
    // Restoring power later sets it off (unintended consequence).
    q.MoveTo(1, "BRK");
    CHECK(q.Do(1, "BRK", A::Activate) == R::Applied);
    q.Run(2);
    CHECK_EQ(q.f->ActiveAlarms(), std::size_t{1});
    CHECK(q.rec.CountSounds(SoundKind::AlarmPulse) >= 1);
}

AIS_TEST(Door_PowerLossMidMotionRevertsAndReportsState) {
    Rig r(SecurityBlock(), AuthorityRole::Server, Loose());
    r.Join(1, "SECURE");
    r.inv.Give(1, "Keycard");
    r.Run(1);
    CHECK(r.Do(1, "SECURE", A::Activate) == R::Applied);
    r.Run(10);
    CHECK(r.S("SECURE").door == DoorState::Opening);
    CHECK(r.Do(1, "SECURE", A::Deactivate) == R::RejectedBusy);
    CHECK(r.f->ServerApplyDamage(r.N("G"), 1.0));
    CHECK(r.S("SECURE").door == DoorState::Closed);
    r.Run(60);
    CHECK(r.S("SECURE").door == DoorState::Closed);  // stale motion timer ignored
}

AIS_TEST(Gate_RequiresTwoPlayersToCrankAndMoreHandsAreFaster) {
    Rig r(SecurityBlock(), AuthorityRole::Server, Loose());
    r.Join(1, "BRK");
    r.Run(1);
    CHECK(r.Do(1, "BRK", A::Deactivate) == R::Applied);
    for (PlayerId p = 1; p <= 4; ++p) r.f->ConnectPlayer(p == 1 ? 1 : p, r.f->Def(r.N("GATE")).position, 0);
    r.MoveTo(1, "GATE");
    CHECK(r.Do(1, "GATE", A::Crank) == R::ChannelStarted);
    r.Run(50);
    CHECK_NEAR(r.S("GATE").progress, 0.0, 1e-9);  // one player cannot move it
    CHECK(r.Do(2, "GATE", A::Crank) == R::ChannelJoined);
    r.Run(50);
    CHECK_NEAR(r.S("GATE").progress, 100.0, 1e-9);
    CHECK(r.Do(3, "GATE", A::Crank) == R::ChannelJoined);
    CHECK(r.Do(4, "GATE", A::Crank) == R::ChannelJoined);
    r.Run(25);
    CHECK(r.S("GATE").door == DoorState::Open);  // 100 + 25*4 = 200
}

AIS_TEST(EmergencyLighting_ActivatesOnLossThenDrainsAndResets) {
    Rig r(SecurityBlock(), AuthorityRole::Server, Loose());
    r.Join(1, "BRK");
    r.Run(1);
    const Vec3 spot{500, 0, 0};
    CHECK(r.S("LAMP").lit && !r.S("EL").lit);
    const double full = r.f->IlluminationAt(spot);
    CHECK(r.Do(1, "BRK", A::Deactivate) == R::Applied);
    CHECK(!r.S("LAMP").lit);
    CHECK(r.S("EL").lit && r.S("EL").onEmergency);
    const double dim = r.f->IlluminationAt(spot);
    CHECK(dim > 0.0 && dim < full);
    r.Run(99);
    CHECK(r.S("EL").lit);
    r.Run(1);
    CHECK(!r.S("EL").lit);
    CHECK_NEAR(r.f->IlluminationAt(spot), 0.0, 1e-12);
    CHECK(r.Do(1, "BRK", A::Activate) == R::Applied);
    CHECK(r.S("LAMP").lit && !r.S("EL").lit && !r.S("EL").onEmergency);

    // A facility that starts dark never had mains: emergency lights stay dead.
    Rig dark(Header() + Node("G", "Generator", "A", "", "capacity_kw = 5\n") +
                 Node("EL", "EmergencyLight", "A", "G", "demand_kw = 0.1\n"),
             AuthorityRole::Server, Loose());
    dark.Run(10);
    CHECK(!dark.S("EL").lit);
}

AIS_TEST(Alarm_PulsesAcousticEventsRaisesAlertAndAutoSilences) {
    Rig r(SecurityBlock(), AuthorityRole::Server, Loose());
    r.Join(1, "ALARM");
    r.Run(1);
    CHECK(r.Do(1, "ALARM", A::TriggerAlarm) == R::Applied);
    CHECK(r.Do(1, "ALARM", A::TriggerAlarm) == R::RejectedInvalidState);
    CHECK_EQ(r.f->ActiveAlarms(), std::size_t{1});
    CHECK(r.rec.CountEvents(FacilityEventType::AlertLevelChanged) == 1);
    r.Run(299);
    // First pulse immediately, then every 30 ticks.
    CHECK_EQ(r.rec.CountSounds(SoundKind::AlarmPulse), std::size_t{10});
    for (const auto& s : r.rec.sounds) {
        if (s.kind == SoundKind::AlarmPulse) CHECK_NEAR(s.loudnessDb, 96.0, 1e-12);
    }
    r.Run(1);
    CHECK(!r.S("ALARM").alarmActive);
    CHECK_EQ(r.f->ActiveAlarms(), std::size_t{0});
    CHECK(r.rec.CountEvents(FacilityEventType::AlarmSilenced) == 1);
    r.Run(200);
    CHECK_EQ(r.rec.CountSounds(SoundKind::AlarmPulse), std::size_t{10});
    // Manual silence.
    CHECK(r.Do(1, "ALARM", A::TriggerAlarm) == R::Applied);
    CHECK(r.Do(1, "ALARM", A::SilenceAlarm) == R::Applied);
    CHECK(r.Do(1, "ALARM", A::SilenceAlarm) == R::RejectedInvalidState);
}

AIS_TEST(Alarm_SecurityRebootOnPowerRestoreAndSensorsReact) {
    const std::string ini = Header() + Node("G", "Generator", "A", "", "capacity_kw = 20\nstarts_running = true\n") +
                            Node("BRK", "Breaker", "A", "G", "capacity_kw = 20\n") +
                            Node("ALARM", "Alarm", "A", "BRK", "demand_kw = 1\ntrigger_on_power_restore = true\n") +
                            Node("SENS", "SecuritySensor", "A", "BRK", "demand_kw = 0.5\nalarm = ALARM\nsensor_radius_cm = 500\nrearm_on_power_restore = true\n", "2000, 0, 0") +
                            Node("DOOR", "Door", "A", "BRK", "demand_kw = 1\n", "2200, 0, 0") +
                            Node("FAR", "Door", "A", "BRK", "demand_kw = 1\n", "9000, 0, 0");
    Rig r(ini, AuthorityRole::Server, Loose());
    r.Join(1, "FAR");
    r.Run(1);
    CHECK(r.Do(1, "FAR", A::Activate) == R::Applied);  // far from the sensor
    CHECK(!r.S("ALARM").alarmActive);
    r.MoveTo(1, "DOOR");
    CHECK(r.Do(1, "DOOR", A::Activate) == R::Applied);  // inside sensor radius
    CHECK(r.S("ALARM").alarmActive);
    r.MoveTo(1, "ALARM");
    CHECK(r.Do(1, "ALARM", A::SilenceAlarm) == R::Applied);
    // Disarm the sensor, then the door near it is safe.
    r.MoveTo(1, "SENS");
    CHECK(r.Do(1, "SENS", A::Deactivate) == R::Applied);
    r.Run(50);  // let the door finish opening (45 ticks)
    r.MoveTo(1, "DOOR");
    CHECK(r.Do(1, "DOOR", A::Deactivate) == R::Applied);
    CHECK(!r.S("ALARM").alarmActive);
    // Re-arm; cutting power disarms; restoring power reboots security: alarm.
    r.MoveTo(1, "SENS");
    CHECK(r.Do(1, "SENS", A::Activate) == R::Applied);
    r.MoveTo(1, "BRK");
    CHECK(r.Do(1, "BRK", A::Deactivate) == R::Applied);
    CHECK(r.rec.events.back().type == FacilityEventType::SecurityArmed || r.rec.CountEvents(FacilityEventType::SecurityArmed) >= 2);
    CHECK(!r.S("ALARM").alarmActive);
    CHECK(r.Do(1, "BRK", A::Activate) == R::Applied);
    CHECK(r.S("ALARM").alarmActive);
    CHECK(r.f->ServerNotifyPlayerPresence(1, {2100, 0, 0}));
}

AIS_TEST(Terminal_HackThenCommandRemoteGateAndBreaker) {
    const std::string ini = Header() + Node("G", "Generator", "A", "", "capacity_kw = 60\nstarts_running = true\n") +
                            Node("BUS", "Junction", "A", "G") +
                            Node("BRK_IND", "Breaker", "A", "BUS", "capacity_kw = 30\nstarts_closed = false\n") +
                            Node("PUMP", "Machinery", "A", "BRK_IND", "demand_kw = 10\nnoise_db = 80\nstarts_on = true\n", "5000, 0, 0") +
                            Node("GATE", "SecurityGate", "A", "BUS", "demand_kw = 3\nstarts_locked = true\nmotor_ticks = 60\n", "8000, 0, 0") +
                            Node("TERM", "Terminal", "A", "BUS", "demand_kw = 1\nhack_ticks = 90\ncontrols = GATE, BRK_IND\n") +
                            Node("TERM2", "Terminal", "A", "BUS", "demand_kw = 1\ncontrols = PUMP\nrequired_permissions = 4\n", "100, 0, 0");
    Rig r(ini, AuthorityRole::Server, Loose());
    CHECK(r.load.ok);
    r.Join(1, "TERM");
    r.Run(1);
    CHECK(r.Command(1, "TERM", "GATE", A::Activate) == R::RejectedLocked);
    CHECK(r.Do(1, "TERM", A::Hack) == R::ChannelStarted);
    r.Run(90);
    CHECK(r.S("TERM").hacked);
    CHECK(r.Command(1, "TERM", "PUMP", A::Activate) == R::RejectedUnsupported);  // not in controls
    CHECK(r.Command(1, "TERM", "GATE", A::Activate) == R::Applied);              // remote, 80 m away
    CHECK(!r.f->IsLocked(r.N("GATE")));
    r.Run(60);
    CHECK(r.S("GATE").door == DoorState::Open);
    // Remote breaker: the pump (still switched on) roars to life.
    CHECK(r.Command(1, "TERM", "BRK_IND", A::Activate) == R::Applied);
    CHECK(r.S("PUMP").powered && r.S("PUMP").soundActive);
    CHECK(!r.rec.continuous.empty() && r.rec.continuous.back().first == r.N("PUMP"));
    // Permissions on the second terminal.
    CHECK(r.Do(1, "TERM2", A::Activate) == R::RejectedPermission);
    // Unpowered terminal refuses.
    CHECK(r.f->ServerApplyDamage(r.N("G"), 1.0));
    CHECK(r.Command(1, "TERM", "GATE", A::Deactivate) == R::RejectedNoPower);
}

AIS_TEST(Extraction_CoopCallPausesWithoutPowerAndSignalsMission) {
    const std::string ini = Header() + Node("G", "Generator", "A", "", "capacity_kw = 20\nstarts_running = true\n") +
                            Node("LIFT", "ExtractionPoint", "A", "G",
                                 "demand_kw = 5\ncall_ticks = 100\nmanual_participants = 2\nobjective_signal = lift_called\nnoise_db = 100\n");
    Rig r(ini, AuthorityRole::Server, Loose());
    r.Join(1, "LIFT");
    r.Join(2, "LIFT");
    r.Run(1);
    CHECK(r.Do(1, "LIFT", A::CallExtraction) == R::ChannelStarted);
    CHECK(r.Do(2, "LIFT", A::CallExtraction) == R::ChannelJoined);
    r.Run(25);
    CHECK_NEAR(r.S("LIFT").progress, 50.0, 1e-9);
    CHECK(r.Do(1, "G", A::Deactivate) == R::Applied);
    r.Run(30);
    CHECK_NEAR(r.S("LIFT").progress, 50.0, 1e-9);  // paused, not lost
    CHECK(r.Do(1, "G", A::Activate) == R::Applied);
    r.Run(90 + 25);
    CHECK(r.S("LIFT").completed);
    bool signalled = false;
    for (const auto& e : r.rec.events) signalled = signalled || (e.type == FacilityEventType::ObjectiveSignal && e.text == "lift_called");
    CHECK(signalled);
    CHECK_EQ(r.rec.CountSounds(SoundKind::ExtractionBeacon), std::size_t{1});
    CHECK(r.Do(1, "LIFT", A::CallExtraction) == R::RejectedInvalidState);
}

AIS_TEST(Machinery_RunningProducesNoiseAndBlocksNavigation) {
    const std::string ini = Header() + Node("G", "Generator", "A", "", "capacity_kw = 20\nstarts_running = true\n") +
                            Node("CRUSHER", "Machinery", "A", "G", "demand_kw = 5\nnoise_db = 85\nnoise_pulse_ticks = 30\nblocks_nav_when_running = true\nstarts_on = false\n");
    Rig r(ini, AuthorityRole::Server, Loose());
    r.Join(1, "CRUSHER");
    r.Run(1);
    CHECK(!r.f->BlocksNavigation(r.N("CRUSHER")));
    CHECK(r.Do(1, "CRUSHER", A::Activate) == R::Applied);
    CHECK(r.f->BlocksNavigation(r.N("CRUSHER")));
    r.Run(90);
    CHECK(r.rec.CountSounds(SoundKind::MachineryRunning) >= 3);
    CHECK(r.Do(1, "CRUSHER", A::Deactivate) == R::Applied);
    CHECK(!r.f->BlocksNavigation(r.N("CRUSHER")));
    const auto n = r.rec.CountSounds(SoundKind::MachineryRunning);
    r.Run(90);
    CHECK_EQ(r.rec.CountSounds(SoundKind::MachineryRunning), n);
}
