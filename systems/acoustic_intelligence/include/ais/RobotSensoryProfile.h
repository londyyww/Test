// LAST LIGHT - Acoustic Intelligence System (AIS)
// Data-driven per-archetype hearing and investigation parameters.
#pragma once

#include <cstddef>
#include <string>

#include "ais/AcousticEvent.h"
#include "ais/AisTypes.h"

namespace ais {

struct RobotSensoryProfile {
    std::string name = "DEFAULT";

    // --- Hearing -----------------------------------------------------------
    BandArray bandSensitivityDb{10.0, 10.0, 10.0};  // added to per-band SNR
    double detectionSnrDb = 6.0;     // minimum effective SNR to perceive
    double localizationSkill = 1.0;  // >1 = tighter estimates
    Tick sensoryUpdateIntervalTicks = 3;  // brain update period
    std::size_t inboxCapacity = 16;       // perceptions buffered between updates

    // --- Interpretation ----------------------------------------------------
    CategoryArray<double> categoryInterest{1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
                                           1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
    double baseEventGain = 30.0;       // suspicion points for an ideal stimulus
    double suspicionGainScale = 1.0;
    double maxSingleEventGain = 35.0;  // must stay below combatThreshold
    double repetitionBonus = 0.25;     // extra gain per repeat of the same trace
    std::size_t repetitionCap = 4;
    double decoyDiscrimination = 0.3;  // 0..1 ability to notice synthetic sound
    double artificialEvidenceThreshold = 1.0;
    double artificialInterestScale = 0.3;  // interest multiplier once a trace looks fake
    double habituationFactor = 0.35;       // interest multiplier per fruitless search

    // --- Memory ------------------------------------------------------------
    std::size_t memoryCapacity = 8;
    double traceMergeRadiusCm = 600.0;
    double confidenceHalfLifeTicks = 300.0;
    double suspicionHalfLifeTicks = 240.0;
    double uncertaintyGrowthCmPerTick = 2.0;  // the source may have moved
    double minUncertaintyCm = 50.0;
    double forgetConfidence = 0.05;
    double lostConfidence = 0.15;

    // --- State thresholds (suspicion 0..100) -------------------------------
    double suspiciousThreshold = 10.0;
    double investigateThreshold = 35.0;
    double combatThreshold = 85.0;
    double calmHysteresis = 4.0;
    double combatMaxUncertaintyCm = 300.0;
    Tick combatLoseTicks = 150;

    // --- Movement / search -------------------------------------------------
    double maxInvestigationRangeCm = 4000.0;
    double arrivalRadiusCm = 150.0;
    double retargetHysteresis = 1.25;
    Tick searchDurationTicks = 600;
    std::size_t searchPointCount = 4;
    double searchRadiusMinCm = 300.0;
    double searchRadiusMaxCm = 1500.0;

    // --- Signal network ----------------------------------------------------
    bool broadcastsReports = false;
    bool receivesReports = true;
    double broadcastMinConfidence = 0.5;
    double sharedReportWeight = 0.5;
    double sharedUncertaintyInflationCm = 400.0;
    Tick supportClaimTicks = 90;

    // --- Objective commitment ----------------------------------------------
    // Investigation only starts when drive >= objectivePriority * commitment.
    double objectiveCommitment = 1.0;
};

RobotSensoryProfile MakeSentinelProfile();
RobotSensoryProfile MakeWraithProfile();

}  // namespace ais
