// LAST LIGHT - Acoustic Intelligence System (AIS)
#include "ais/RobotAcousticBrain.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ais {

namespace {

constexpr double kTwoPi = 6.283185307179586;
constexpr double kGoldenAngle = 2.399963229728653;

bool IsMovementFamily(StimulusCategory c) {
    return c == StimulusCategory::Footstep || c == StimulusCategory::Sprint || c == StimulusCategory::JumpLanding;
}

bool Compatible(StimulusCategory a, StimulusCategory b) {
    return a == b || (IsMovementFamily(a) && IsMovementFamily(b));
}

Tick AbsDiff(Tick a, Tick b) { return a > b ? a - b : b - a; }

}  // namespace

const char* ToString(AwarenessState s) {
    switch (s) {
        case AwarenessState::Unaware: return "Unaware";
        case AwarenessState::Suspicious: return "Suspicious";
        case AwarenessState::Investigating: return "Investigating";
        case AwarenessState::Searching: return "Searching";
        case AwarenessState::Combat: return "Combat";
        case AwarenessState::ReturningToPatrol: return "ReturningToPatrol";
    }
    return "Unknown";
}

const char* ToString(DirectiveReason r) {
    switch (r) {
        case DirectiveReason::None: return "None";
        case DirectiveReason::HeardSomething: return "HeardSomething";
        case DirectiveReason::InvestigatingSound: return "InvestigatingSound";
        case DirectiveReason::TargetUpdated: return "TargetUpdated";
        case DirectiveReason::ArrivedNoSource: return "ArrivedNoSource";
        case DirectiveReason::SourceLost: return "SourceLost";
        case DirectiveReason::SearchPoint: return "SearchPoint";
        case DirectiveReason::SearchExhausted: return "SearchExhausted";
        case DirectiveReason::AcousticCombat: return "AcousticCombat";
        case DirectiveReason::VisualCombat: return "VisualCombat";
        case DirectiveReason::CombatContactLost: return "CombatContactLost";
        case DirectiveReason::ReturningHome: return "ReturningHome";
        case DirectiveReason::ObjectiveHeld: return "ObjectiveHeld";
        case DirectiveReason::SupportingAlly: return "SupportingAlly";
        case DirectiveReason::LostInterest: return "LostInterest";
    }
    return "Unknown";
}

RobotAcousticBrain::RobotAcousticBrain(EntityId robotId, const RobotSensoryProfile& profile, std::uint64_t seed)
    : id_(robotId), profile_(profile), seed_(HashCombine(seed, robotId)) {
    inbox_.reserve(profile_.inboxCapacity);
    traces_.reserve(profile_.memoryCapacity);
}

void RobotAcousticBrain::Enqueue(const PerceivedStimulus& s) {
    if (inbox_.size() < profile_.inboxCapacity) {
        inbox_.push_back(s);
        return;
    }
    ++droppedPerceptions_;
    // Keep the most salient perceptions when flooded.
    auto weakest = std::min_element(inbox_.begin(), inbox_.end(), [](const PerceivedStimulus& a, const PerceivedStimulus& b) {
        return a.effectiveSnrDb != b.effectiveSnrDb ? a.effectiveSnrDb < b.effectiveSnrDb : a.eventId > b.eventId;
    });
    if (s.effectiveSnrDb > weakest->effectiveSnrDb) *weakest = s;
}

bool RobotAcousticBrain::IsUpdateDue(Tick now) const {
    const Tick interval = std::max<Tick>(1, profile_.sensoryUpdateIntervalTicks);
    // Staggered by id so robots of one type do not all think on the same tick.
    return (now + id_) % interval == 0;
}

const AcousticTrace* RobotAcousticBrain::FindTrace(std::uint32_t traceId) const {
    for (const auto& t : traces_) {
        if (t.traceId == traceId) return &t;
    }
    return nullptr;
}

void RobotAcousticBrain::Decay(Tick now) {
    if (!hasUpdated_ || now <= lastUpdateTick_) return;
    const double dt = static_cast<double>(now - lastUpdateTick_);
    suspicion_ *= std::pow(0.5, dt / profile_.suspicionHalfLifeTicks);
    const double confFactor = std::pow(0.5, dt / profile_.confidenceHalfLifeTicks);
    for (auto& t : traces_) {
        t.confidence *= confFactor;
        t.uncertaintyCm += profile_.uncertaintyGrowthCmPerTick * dt;
    }
    const double forget = profile_.forgetConfidence;
    traces_.erase(std::remove_if(traces_.begin(), traces_.end(),
                                 [forget](const AcousticTrace& t) { return t.confidence < forget; }),
                  traces_.end());
}

AcousticTrace& RobotAcousticBrain::MergeOrCreate(const PerceivedStimulus& s, Tick now, bool& outCreated) {
    AcousticTrace* best = nullptr;
    double bestD = std::numeric_limits<double>::max();
    for (auto& t : traces_) {
        if (!Compatible(t.category, s.apparentCategory)) continue;
        const double d = Distance(t.estimate, s.estimatedPosition);
        const double gate = profile_.traceMergeRadiusCm + 0.5 * (t.uncertaintyCm + s.uncertaintyCm);
        if (d <= gate && d < bestD) {
            best = &t;
            bestD = d;
        }
    }
    if (best != nullptr) {
        outCreated = false;
        // Inverse-variance fusion: repeated hearing tightens the estimate.
        const double u1 = std::max(best->uncertaintyCm, profile_.minUncertaintyCm);
        const double u2 = std::max(s.uncertaintyCm, profile_.minUncertaintyCm);
        const double w1 = 1.0 / (u1 * u1);
        const double w2 = (s.fromSharedReport ? profile_.sharedReportWeight : 1.0) / (u2 * u2);
        if (w2 > 0.0) {
            best->estimate = (best->estimate * w1 + s.estimatedPosition * w2) / (w1 + w2);
            best->uncertaintyCm = std::max(profile_.minUncertaintyCm, std::sqrt(1.0 / (w1 + w2)));
        }
        if (IsMovementFamily(s.apparentCategory) && s.threat > best->threat) best->category = s.apparentCategory;
        return *best;
    }

    outCreated = true;
    if (traces_.size() >= profile_.memoryCapacity) {
        auto victim = std::min_element(traces_.begin(), traces_.end(), [](const AcousticTrace& a, const AcousticTrace& b) {
            if (a.confidence != b.confidence) return a.confidence < b.confidence;
            if (a.lastTick != b.lastTick) return a.lastTick < b.lastTick;
            return a.traceId < b.traceId;
        });
        traces_.erase(victim);
    }
    AcousticTrace t;
    t.traceId = nextTraceId_++;
    t.category = s.apparentCategory;
    t.estimate = s.estimatedPosition;
    t.uncertaintyCm = std::max(s.uncertaintyCm, profile_.minUncertaintyCm);
    t.firstTick = s.tick;
    t.lastTick = s.tick;
    (void)now;
    traces_.push_back(t);
    return traces_.back();
}

double RobotAcousticBrain::InterestFor(const AcousticTrace& t) const {
    double interest = profile_.categoryInterest[Index(t.category)];
    if (t.suspectedArtificial) interest *= profile_.artificialInterestScale;
    if (t.fruitlessSearches > 0) interest *= std::pow(profile_.habituationFactor, static_cast<double>(t.fruitlessSearches));
    return interest;
}

void RobotAcousticBrain::Absorb(const PerceivedStimulus& s, const RobotHostState& host, Tick now) {
    bool created = false;
    AcousticTrace& t = MergeOrCreate(s, now, created);
    const bool direct = !s.fromSharedReport;

    if (!created && direct && t.directHits > 0 && s.tick > t.lastDirectTick) {
        const Tick interval = s.tick - t.lastDirectTick;
        if (s.regularityIsSuspicious && t.lastInterval > 0 && AbsDiff(interval, t.lastInterval) <= 1) {
            ++t.regularStreak;
        } else {
            t.regularStreak = 0;
        }
        t.lastInterval = interval;
    }
    ++t.hitCount;
    if (direct) {
        ++t.directHits;
        t.lastDirectTick = std::max(t.lastDirectTick, s.tick);
    }
    t.lastTick = std::max(t.lastTick, s.tick);
    t.threat = std::max(t.threat, s.threat);
    t.hostile = t.hostile || s.hostileCategory;

    // Decoy discrimination: synthetic timbre and metronomic repetition.
    if (direct) {
        double evidence = s.artificialCue * profile_.decoyDiscrimination;
        if (t.regularStreak >= 2) {
            evidence += 0.2 * profile_.decoyDiscrimination * static_cast<double>(std::min<std::uint32_t>(t.regularStreak, 6));
        }
        t.artificialEvidence += evidence;
    } else if (s.sharedSuspectedArtificial) {
        t.artificialEvidence += profile_.artificialEvidenceThreshold * profile_.sharedReportWeight;
    }
    t.artificialEvidence = std::min(t.artificialEvidence, 3.0 * profile_.artificialEvidenceThreshold);
    if (t.artificialEvidence >= profile_.artificialEvidenceThreshold) t.suspectedArtificial = true;

    const double weight = direct ? 1.0 : profile_.sharedReportWeight;
    const double snrFactor = Clamp((s.effectiveSnrDb - profile_.detectionSnrDb) / 18.0 + 0.25, 0.25, 1.0);
    t.confidence = 1.0 - (1.0 - t.confidence) * (1.0 - 0.6 * snrFactor * weight);

    const double dist = Distance(host.position, s.estimatedPosition);
    const double distanceFactor = Clamp(1.0 - dist / (profile_.maxInvestigationRangeCm * 3.0), 0.4, 1.0);
    const std::size_t repeats = std::min<std::size_t>(t.hitCount - 1, profile_.repetitionCap);
    const double repetition = 1.0 + profile_.repetitionBonus * static_cast<double>(repeats);
    double gain = profile_.baseEventGain * profile_.suspicionGainScale * snrFactor * distanceFactor *
                  InterestFor(t) * std::max(0.05, s.threat) * repetition * weight;
    gain = std::min(gain, profile_.maxSingleEventGain);
    suspicion_ = std::min(100.0, suspicion_ + gain);
}

double RobotAcousticBrain::ScoreTrace(const AcousticTrace& t, const Vec3& from) const {
    const double dist = Distance(from, t.estimate);
    return t.confidence * InterestFor(t) * std::max(0.05, t.threat) / (1.0 + dist / profile_.maxInvestigationRangeCm);
}

AcousticTrace* RobotAcousticBrain::SelectTarget(const Vec3& from) {
    AcousticTrace* best = nullptr;
    double bestScore = -1.0;
    AcousticTrace* current = nullptr;
    for (auto& t : traces_) {
        const double score = ScoreTrace(t, from);
        if (t.traceId == targetTraceId_) current = &t;
        if (score > bestScore || (score == bestScore && best != nullptr && t.traceId < best->traceId)) {
            best = &t;
            bestScore = score;
        }
    }
    // Hysteresis: do not flip-flop between similar sounds.
    if (current != nullptr && best != current && ScoreTrace(*current, from) * profile_.retargetHysteresis >= bestScore) {
        best = current;
    }
    return best;
}

bool RobotAcousticBrain::CombatGate(const AcousticTrace* t, const Vec3& from) const {
    if (t == nullptr) return false;
    return suspicion_ >= profile_.combatThreshold && t->hostile && t->directHits >= 1 &&
           t->uncertaintyCm <= profile_.combatMaxUncertaintyCm &&
           Distance(from, t->estimate) <= profile_.maxInvestigationRangeCm;
}

bool RobotAcousticBrain::WantsToInvestigate(const AcousticTrace* t, const RobotHostState& host) const {
    if (t == nullptr || t->confidence < profile_.lostConfidence) return false;
    if (Distance(host.position, t->estimate) > profile_.maxInvestigationRangeCm) return false;
    const double drive = suspicion_ * InterestFor(*t);
    return drive >= profile_.investigateThreshold && drive >= host.objectivePriority * profile_.objectiveCommitment;
}

void RobotAcousticBrain::EnterState(AwarenessState s, Tick now) {
    state_ = s;
    stateEnterTick_ = now;
    if (s == AwarenessState::Unaware) hasPatrolAnchor_ = false;
}

void RobotAcousticBrain::BeginSearch(const Vec3& center, double uncertaintyCm, Tick now) {
    searchRadius_ = Clamp(uncertaintyCm * 1.2, profile_.searchRadiusMinCm, profile_.searchRadiusMaxCm);
    searchCenter_ = center;
    searchPoints_.clear();
    const std::size_t n = profile_.searchPointCount;
    const double angle0 = kTwoPi * HashToUnit(HashCombine(seed_, now));
    for (std::size_t i = 0; i < n; ++i) {
        const double a = angle0 + kGoldenAngle * static_cast<double>(i);
        const double r = searchRadius_ * std::sqrt((static_cast<double>(i) + 0.5) / static_cast<double>(n));
        searchPoints_.push_back(center + Vec3{std::cos(a) * r, std::sin(a) * r, 0.0});
    }
    searchIndex_ = 0;
    searchTraceId_ = targetTraceId_;
    EnterState(AwarenessState::Searching, now);
}

Vec3 RobotAcousticBrain::SupportOffset(const Vec3& target, double radius) const {
    const double a = kTwoPi * HashToUnit(HashCombine(seed_, 0x5u));
    return target + Vec3{std::cos(a) * radius, std::sin(a) * radius, 0.0};
}

void RobotAcousticBrain::NotifyAllyInvestigating(EntityId ally, const Vec3& area, Tick now) {
    // Lower id leads; higher ids take flanking positions. Deterministic.
    if (ally == id_ || ally > id_) return;
    supportingAlly_ = ally;
    supportArea_ = area;
    supportUntil_ = now + profile_.supportClaimTicks;
}

void RobotAcousticBrain::RunStateMachine(Tick now, const RobotHostState& host) {
    AcousticTrace* best = SelectTarget(host.position);
    const std::uint32_t previousTarget = targetTraceId_;
    if (best != nullptr) targetTraceId_ = best->traceId;

    AcousticDirective d;
    auto moveTo = [&d](const Vec3& p, double unc, std::uint32_t trace, DirectiveReason why) {
        d.hasMoveTarget = true;
        d.moveTarget = p;
        d.hasLookAt = true;
        d.lookAt = p;
        d.targetUncertaintyCm = unc;
        d.traceId = trace;
        d.reason = why;
    };
    auto lookAt = [&d](const Vec3& p, double unc, std::uint32_t trace, DirectiveReason why) {
        d.hasLookAt = true;
        d.lookAt = p;
        d.targetUncertaintyCm = unc;
        d.traceId = trace;
        d.reason = why;
    };
    auto leaveIdle = [this, &host]() {
        if (!hasPatrolAnchor_) {
            hasPatrolAnchor_ = true;
            patrolAnchor_ = host.position;
        }
    };
    auto enterCombat = [&]() {
        leaveIdle();
        EnterState(AwarenessState::Combat, now);
        lastCombatEvidenceTick_ = now;
    };

    if (host.hasVisualContact) {
        if (state_ != AwarenessState::Combat) enterCombat();
        lastCombatEvidenceTick_ = now;
        lastTarget_ = host.visualContactPosition;
        lastTargetUncertainty_ = profile_.minUncertaintyCm;
        suspicion_ = 100.0;
        moveTo(host.visualContactPosition, profile_.minUncertaintyCm, 0, DirectiveReason::VisualCombat);
    } else {
        if (state_ == AwarenessState::Unaware && suspicion_ >= profile_.suspiciousThreshold) {
            leaveIdle();
            EnterState(AwarenessState::Suspicious, now);
        }

        if (state_ != AwarenessState::Combat && CombatGate(best, host.position)) enterCombat();

        switch (state_) {
            case AwarenessState::Unaware:
                break;

            case AwarenessState::Suspicious:
                if (WantsToInvestigate(best, host)) {
                    EnterState(AwarenessState::Investigating, now);
                    // fall through to the Investigating handling below
                } else if (suspicion_ < profile_.suspiciousThreshold - profile_.calmHysteresis) {
                    EnterState(AwarenessState::Unaware, now);
                    break;
                } else {
                    if (best != nullptr) {
                        const bool blocked = Distance(host.position, best->estimate) <= profile_.maxInvestigationRangeCm &&
                                             suspicion_ * InterestFor(*best) >= profile_.investigateThreshold;
                        lookAt(best->estimate, best->uncertaintyCm, best->traceId,
                               blocked ? DirectiveReason::ObjectiveHeld : DirectiveReason::HeardSomething);
                    }
                    break;
                }
                [[fallthrough]];

            case AwarenessState::Investigating: {
                if (best == nullptr || best->confidence < profile_.lostConfidence) {
                    BeginSearch(lastTarget_, lastTargetUncertainty_, now);
                    d.reason = DirectiveReason::SourceLost;
                    break;
                }
                const double drive = suspicion_ * InterestFor(*best);
                if (drive < host.objectivePriority * profile_.objectiveCommitment) {
                    // Something more important than a doubtful noise.
                    EnterState(AwarenessState::ReturningToPatrol, now);
                    d.reason = DirectiveReason::ObjectiveHeld;
                    break;
                }
                if (drive < 0.5 * profile_.investigateThreshold) {
                    // e.g. the trace was judged artificial or already searched.
                    EnterState(AwarenessState::ReturningToPatrol, now);
                    d.reason = DirectiveReason::LostInterest;
                    break;
                }
                Vec3 target = best->estimate;
                DirectiveReason why = (previousTarget != 0 && previousTarget != best->traceId)
                                          ? DirectiveReason::TargetUpdated
                                          : DirectiveReason::InvestigatingSound;
                if (supportingAlly_ != kNoEntity && now < supportUntil_ &&
                    Distance(supportArea_, best->estimate) <= profile_.traceMergeRadiusCm + best->uncertaintyCm) {
                    target = SupportOffset(best->estimate, std::max(best->uncertaintyCm, profile_.searchRadiusMinCm));
                    why = DirectiveReason::SupportingAlly;
                }
                lastTarget_ = best->estimate;
                lastTargetUncertainty_ = best->uncertaintyCm;
                if (Distance(host.position, target) <= profile_.arrivalRadiusCm) {
                    BeginSearch(best->estimate, best->uncertaintyCm, now);
                    d.reason = DirectiveReason::ArrivedNoSource;
                    break;
                }
                moveTo(target, best->uncertaintyCm, best->traceId, why);
                break;
            }

            case AwarenessState::Searching:
                break;  // handled below so freshly started searches emit a point

            case AwarenessState::Combat: {
                const bool fresh = best != nullptr && best->hostile && best->directHits > 0 &&
                                   now - std::min(now, best->lastDirectTick) <= profile_.combatLoseTicks;
                if (fresh) {
                    lastCombatEvidenceTick_ = std::max(lastCombatEvidenceTick_, best->lastDirectTick);
                    lastTarget_ = best->estimate;
                    lastTargetUncertainty_ = best->uncertaintyCm;
                }
                if (now - std::min(now, lastCombatEvidenceTick_) > profile_.combatLoseTicks) {
                    BeginSearch(lastTarget_, std::max(lastTargetUncertainty_, profile_.searchRadiusMinCm), now);
                    d.reason = DirectiveReason::CombatContactLost;
                    break;
                }
                moveTo(lastTarget_, lastTargetUncertainty_, fresh ? best->traceId : 0, DirectiveReason::AcousticCombat);
                break;
            }

            case AwarenessState::ReturningToPatrol:
                if (best != nullptr && best->lastTick > stateEnterTick_ && WantsToInvestigate(best, host)) {
                    EnterState(AwarenessState::Investigating, now);
                    lastTarget_ = best->estimate;
                    lastTargetUncertainty_ = best->uncertaintyCm;
                    moveTo(best->estimate, best->uncertaintyCm, best->traceId, DirectiveReason::InvestigatingSound);
                } else if (!hasPatrolAnchor_ || Distance(host.position, patrolAnchor_) <= profile_.arrivalRadiusCm) {
                    // Back on the route; stay alert if the area is still noisy.
                    EnterState(suspicion_ >= profile_.suspiciousThreshold ? AwarenessState::Suspicious
                                                                          : AwarenessState::Unaware,
                               now);
                } else {
                    moveTo(patrolAnchor_, 0.0, 0, DirectiveReason::ReturningHome);
                }
                break;
        }

        if (state_ == AwarenessState::Searching) {
            const DirectiveReason entryReason = d.reason;
            const bool newPlace = best != nullptr && best->lastDirectTick > stateEnterTick_ &&
                                  Distance(best->estimate, searchCenter_) > searchRadius_;
            if (newPlace && WantsToInvestigate(best, host)) {
                // The sound came from somewhere else: chase it.
                EnterState(AwarenessState::Investigating, now);
                lastTarget_ = best->estimate;
                lastTargetUncertainty_ = best->uncertaintyCm;
                moveTo(best->estimate, best->uncertaintyCm, best->traceId, DirectiveReason::TargetUpdated);
            } else {
                while (searchIndex_ < searchPoints_.size() &&
                       Distance(host.position, searchPoints_[searchIndex_]) <= profile_.arrivalRadiusCm) {
                    ++searchIndex_;
                }
                const bool timedOut = now - stateEnterTick_ >= profile_.searchDurationTicks;
                if (timedOut || searchIndex_ >= searchPoints_.size()) {
                    for (auto& t : traces_) {
                        if (t.traceId == searchTraceId_ ||
                            Distance(t.estimate, searchCenter_) <= searchRadius_ + profile_.traceMergeRadiusCm) {
                            ++t.fruitlessSearches;
                        }
                    }
                    suspicion_ *= 0.5;
                    EnterState(AwarenessState::ReturningToPatrol, now);
                    if (hasPatrolAnchor_) moveTo(patrolAnchor_, 0.0, 0, DirectiveReason::SearchExhausted);
                    else d.reason = DirectiveReason::SearchExhausted;
                } else {
                    moveTo(searchPoints_[searchIndex_], searchRadius_, searchTraceId_,
                           entryReason != DirectiveReason::None ? entryReason : DirectiveReason::SearchPoint);
                }
            }
        }
    }

    d.state = state_;
    d.suspicion = suspicion_;
    directive_ = d;
}

void RobotAcousticBrain::Update(Tick now, const RobotHostState& host, AcousticReport* outReport, bool* outHasReport) {
    if (outHasReport != nullptr) *outHasReport = false;
    Decay(now);

    std::sort(inbox_.begin(), inbox_.end(), [](const PerceivedStimulus& a, const PerceivedStimulus& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        if (a.fromSharedReport != b.fromSharedReport) return !a.fromSharedReport;
        return a.eventId < b.eventId;
    });
    for (const auto& s : inbox_) Absorb(s, host, now);
    inbox_.clear();

    RunStateMachine(now, host);
    lastUpdateTick_ = now;
    hasUpdated_ = true;

    if (!profile_.broadcastsReports || outReport == nullptr || outHasReport == nullptr) return;
    const AcousticTrace* t = FindTrace(targetTraceId_);
    if (t == nullptr || t->directHits == 0 || t->confidence < profile_.broadcastMinConfidence) return;
    if (hasReported_ && t->lastDirectTick <= lastReportedTraceUpdate_) return;  // nothing new to say
    AcousticReport r;
    r.senderId = id_;
    r.senderPosition = host.position;
    r.estimate = t->estimate;
    r.uncertaintyCm = t->uncertaintyCm;
    r.confidence = t->confidence;
    r.category = t->category;
    r.sentTick = now;
    r.claimingInvestigation = state_ == AwarenessState::Investigating || state_ == AwarenessState::Searching;
    r.suspectedArtificial = t->suspectedArtificial;
    *outReport = r;
    *outHasReport = true;
    hasReported_ = true;
    lastReportedTraceUpdate_ = t->lastDirectTick;
}

void RobotAcousticBrain::HashInto(StateHasher& h) const {
    h.Add(id_);
    h.Add(static_cast<std::uint64_t>(state_));
    h.Add(stateEnterTick_);
    h.AddDouble(suspicion_);
    h.Add(targetTraceId_);
    h.Add(searchIndex_);
    h.Add(traces_.size());
    for (const auto& t : traces_) {
        h.Add(t.traceId);
        h.Add(static_cast<std::uint64_t>(t.category));
        h.AddVec(t.estimate);
        h.AddDouble(t.uncertaintyCm);
        h.AddDouble(t.confidence);
        h.Add(t.hitCount);
        h.Add(t.directHits);
        h.AddDouble(t.artificialEvidence);
        h.Add(t.fruitlessSearches);
    }
    h.Add(static_cast<std::uint64_t>(directive_.reason));
    h.Add(directive_.hasMoveTarget ? 1u : 0u);
    h.AddVec(directive_.moveTarget);
    h.AddVec(directive_.lookAt);
}

}  // namespace ais
