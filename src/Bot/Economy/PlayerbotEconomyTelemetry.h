/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_PLAYERBOTECONOMYTELEMETRY_H
#define PLAYERBOTS_PLAYERBOTECONOMYTELEMETRY_H

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

inline constexpr std::uint8_t PLAYERBOT_ECONOMY_FAILURE_QUARANTINE_THRESHOLD = 5;

class PlayerbotEconomyFailureTracker
{
public:
    void RecordFailure(std::string fingerprint);
    void RecordUnrelatedSuccess() const;
    [[nodiscard]] std::uint8_t Count() const;
    [[nodiscard]] bool IsQuarantined() const;

private:
    std::string fingerprint;
    std::uint8_t count = 0;
};

enum class PlayerbotEconomyOutcome : std::uint8_t
{
    None,
    NoCandidate,
    Scheduled,
    Operation,
    FailedPrecondition,
    Released,
    Quarantined
};

enum class PlayerbotEconomyTelemetryPhase : std::uint8_t
{
    None,
    CollectAuctionMail,
    Craft,
    BuyReagent,
    BuyRecipe,
    BuyFinishedGood,
    UseFinishedGood,
    RecoverFinishedGood,
    SellSurplus,
    MarketMaking,
    Gather
};

struct PlayerbotEconomyObservation
{
    std::uint64_t sequence = 0;
    PlayerbotEconomyOutcome outcome = PlayerbotEconomyOutcome::None;
    PlayerbotEconomyTelemetryPhase phase = PlayerbotEconomyTelemetryPhase::None;
    std::string chainPublicId;
    std::string operationIdentity;
    std::uint32_t marketId = 0;
    std::string itemFamily;
    std::uint32_t workOrderSpellId = 0;
    std::uint32_t remainingQuantity = 0;
    std::uint64_t claimAgeSeconds = 0;
    std::string blockerCode;
    std::uint8_t consecutiveFailures = 0;
    std::uint64_t cooldownSeconds = 0;
    std::uint64_t nextEligibleTime = 0;
    bool quarantined = false;

    bool operator==(PlayerbotEconomyObservation const&) const = default;
};

enum class PlayerbotCareerTelemetryStatus : std::uint8_t
{
    Pending,
    Valid
};

enum class PlayerbotCareerTelemetrySource : std::uint8_t
{
    None,
    Loaded,
    Saved
};

struct PlayerbotCareerObservation
{
    PlayerbotCareerTelemetryStatus status = PlayerbotCareerTelemetryStatus::Pending;
    PlayerbotCareerTelemetrySource source = PlayerbotCareerTelemetrySource::None;
    std::uint32_t version = 0;
    std::string candidateToken;
    std::vector<std::uint16_t> primarySkills;
    std::vector<std::uint16_t> primarySkillAmendments;
    std::vector<std::uint16_t> secondarySkills;
    std::uint8_t spendingStyle = 0;
    bool marketEligible = false;
    std::uint8_t engagement = 0;

    bool operator==(PlayerbotCareerObservation const&) const = default;
};

class PlayerbotEconomyTelemetry
{
public:
    void Publish(std::uint32_t characterGuid, PlayerbotEconomyObservation observation);
    [[nodiscard]] std::optional<PlayerbotEconomyObservation> Find(std::uint32_t characterGuid) const;
    void PublishCareerPending(std::uint32_t characterGuid);
    void PublishCareer(std::uint32_t characterGuid, PlayerbotCareerObservation observation);
    [[nodiscard]] std::optional<PlayerbotCareerObservation> FindCareer(std::uint32_t characterGuid) const;

private:
    mutable std::mutex mutex;
    std::unordered_map<std::uint32_t, PlayerbotEconomyObservation> observations;
    std::unordered_map<std::uint32_t, PlayerbotCareerObservation> careerObservations;
};

PlayerbotEconomyTelemetry& GetPlayerbotEconomyTelemetry();

#endif
