# Make Playerbots Gather Nearby Buildout

Created: 2026-08-11
Author: magitekrr@gmail.com
Agent: Codex
Status: PENDING
Approved: Yes
Rounds: 1
Worktree: No
Type: Build

## Summary

**Goal:** Make Playerbots gather nearby resources, use or trade real surplus, buy worthwhile materials, recipes, and equipment, and show authoritative Auction House inventory and truthful outcomes in Medivh.

This Buildout spans the economy module and Medivh. The economy module owns gathering integration, economic decisions, and lifecycle evidence. Medivh owns the authoritative current listing projection and the operator interface. The existing Playerbot extension seam must be sufficient, so the base `mod-playerbots` repository remains unchanged.

## Base Repository Baseline

The base `mod-playerbots` checkout baseline was recorded before implementation.

```text
HEAD bf57a512f65b3f0baef7dfecf4ceeb046ffd700b
git status --short --untracked-files=all: no output
git diff HEAD: no output
git diff --cached: no output
```

## Acceptance Criteria

- [x] Criterion 1a: AzerothCore integration tests invoke nearby herb, mineral node, and skinnable corpse gathering through the registered economy action context without changing the base `mod-playerbots` repository.
- [x] Criterion 1b: The gathering action rejects unsafe, profession incompatible, unavailable, and duplicate resource claims.
- [x] Criterion 1c: A gathered success event is emitted only when the test observes the authoritative inventory or profession skill delta for the claimed resource.
- [x] Criterion 2a: Policy and runtime tests prove every profession input retains its immediate use quantity and configured profession reserve.
- [x] Criterion 2b: Tests prove genuinely unused Linen Cloth, Wool Cloth, herbs, ore, and leather produce Auction House listing quantities bounded by the post reserve surplus.
- [x] Criterion 2c: Tests prove no reserved quantity and no controlled item appears in a sale decision.
- [x] Criterion 3a: Material purchase tests accept an indivisible listing larger than the immediate recipe deficit when post purchase inventory remains at or below the configured reserve ceiling, covering deficits of 1, 2, and 3 against stacks of 20, 20, and 6.
- [x] Criterion 3b: Material purchase tests reject same account, inaccessible, over reserve, over budget, and over price listings.
- [x] Criterion 4a: Recipe acquisition tests accept an unknown and usable pattern, plan, schematic, formula, recipe, or manual that matches a planned profession and the configured market price corridor, then observe that it is learned.
- [x] Criterion 4b: Recipe acquisition tests reject known, unusable, irrelevant profession, duplicate, same account, over budget, and unsupported price listings.
- [x] Criterion 5a: Finished equipment tests accept a compatible uncommon armor or weapon only when utility comparison identifies a real upgrade, buy it, and observe that the purchased item is equipped.
- [x] Criterion 5b: A test with weaker or equivalent owned gear in the same level tier still accepts the real upgrade.
- [x] Criterion 5c: Equipment tests reject unusable, nonupgrade, same account, over budget, and over price listings.
- [x] Criterion 6a: Trace tests prove gathered, listed, and purchased events are recorded only after their corresponding authoritative game operation boundary.
- [x] Criterion 6b: Trace tests prove sold and expired events are recorded only after the authoritative auction mail or expiry boundary.
- [x] Criterion 6c: Projection tests prove an attempt level scheduling failure is represented as an attempt level blocker and never as a capability wide failure.
- [x] Criterion 7a: Medivh feature tests prove current Auction House rows are queried through the existing read only AzerothCore character and world database connections, not telemetry storage.
- [x] Criterion 7b: Tests prove the response reports the complete active listing count and returns validated pages without accepting invalid page parameters.
- [x] Criterion 7c: Tests prove expired rows are excluded and capped or stale telemetry cannot populate current inventory.
- [x] Criterion 8a: Browser verification at 1440 by 1000 and 390 by 844 shows the authoritative active listing total and allows navigation through the returned listing pages without horizontal overflow.
- [x] Criterion 8b: Browser verification distinguishes Listed, Bought, Sold, and Expired activity using fixtures containing each outcome.
- [x] Criterion 8c: Browser tests use fixtures containing a documented destination eligibility failure, a documented post assignment scheduling failure, and a gathered success event. They prove the rendered blocker copy preserves the distinction between the two failure scopes and that gathered supply displays the authoritative item identity, quantity, and event provenance from the fixture.
- [x] Criterion 9a: At hand back, the base `mod-playerbots` checkout has no additional tracked or untracked changes relative to the recorded pre-run HEAD, status, working tree diff, and staged diff baseline, with the final status and diffs recorded as evidence.
- [x] Criterion 9b: The economy module repository checks, the full AzerothCore integration test command, and Medivh `composer ci:check` each exit successfully, with their exact commands and results recorded.
- [x] Criterion 9c: A deterministic circulation test proves item quantities and gold reconcile after gathering, use, listing, purchase, sale, and expiration.
- [ ] Criterion 10 (oracle): With the deployed economy build and approximately 200 active random bots, a ten minute observation records at least one authoritative Gathered event from a bot that encountered an eligible herb, mineral node, or skinnable corpse.

## Out of Scope

- Deploying or restarting the live worldserver, telemetry consumer, or Medivh. Live deployment and lifecycle operations require separate authorization.
- Creating synthetic items, gold, auctions, buyers, sellers, or guaranteed liquidity.
- Refactoring or adding economy specific behavior to the base `mod-playerbots` repository.
- Replacing ordinary unlimited gold vendor supply with Auction House or gathering demand.
- Building a general player facing Auction House replacement outside the authenticated Medivh economy view.

## Progress Tracking

- [x] Task 1: Integrate nearby gathering inside the economy module.
- [x] Task 2: Correct material retention and surplus listing.
- [x] Task 3: Build healthy material, recipe, and equipment demand.
- [x] Task 4: Record complete and truthful economy lifecycle evidence.
- [x] Task 5: Project authoritative listings and outcomes in Medivh, then verify circulation end to end.
- [ ] Task 6: Make live nearby gathering trigger from eligible world resources.

## Implementation Tasks

### Task 1: Integrate nearby gathering inside the economy module

**Objective:** Extend the ordinary gathering action through the registered economy action context so nearby herbs, mineral nodes, and skinnable corpses participate in economy claims without modifying base Playerbots. Settle claims and emit success from observed game state changes, not from an accepted action name.

### Task 2: Correct material retention and surplus listing

**Objective:** Preserve profession reserves and immediate use before exposing inventory to sale. Treat genuinely unused cloth and gathered trade goods as Auction House surplus, and choose bounded listing quantities that never consume reserved or controlled inventory.

### Task 3: Build healthy material, recipe, and equipment demand

**Objective:** Let buyers accept indivisible material stacks within bounded reserves, relevant unknown recipes within market based price corridors, and genuine equipment upgrades after utility comparison. Keep account separation, accessibility, budget, price, compatibility, and final use safeguards authoritative.

### Task 4: Record complete and truthful economy lifecycle evidence

**Objective:** Add gathered and expired outcomes and tighten existing market events so every success follows a real inventory, auction, or mail boundary. Replace capability wide interpretations of attempt level scheduling failures in the projected vocabulary.

### Task 5: Project authoritative listings and outcomes in Medivh, then verify circulation end to end

**Objective:** Read current listings through Medivh's existing read only AzerothCore connections with validated pagination, while retaining telemetry for historical economy events. Exercise the responsive interface, automated gates, integration tests, and deterministic conservation scenario across both repositories.

### Task 6: Make live nearby gathering trigger from eligible world resources

**Objective:** Reproduce the deployed zero gathering outcome at the actual discovery and destination boundaries, correct the minimal module owned cause, and prove the result with an authoritative Gathered event under the same ten minute live observation that exposed the failure.

## Round Log

- Round 1: all five tasks completed. Judge: 25 of 25 criteria pass.
- Post hand back live observation: reopened after approximately 200 active bots produced zero Gathered events in ten minutes. All 49 currently sampled gathering actors ended on `gathering_destination_wrong_map`, no gathering trip became active, and the ordinary gather action log contained only failed executions. Added Task 6 and the live oracle as Criterion 10 without changing the previously judged criteria.
- Task 6 first deployment: commit `cb3377d59427c840cbeb59e5c41b216038418a80` built, passed the focused gathering suite and full CTest, and was deployed after Pierre disconnected. The ten minute oracle still recorded zero Gathered events with 210 bots. Live inspection proved `add gathering loot` executed 1,336 times and failed every time, while 52 gathering assignments all reported `gathering_destination_wrong_map`. Career professions and gathering tools matched for every gathering actor. The catalog's direct path preflight also produced mass missing navmesh tile loads before any travel target could own routing, so Task 6 remains open at the destination eligibility boundary.
- Task 6 second deployment: commit `b3aa760c58fb1d9169c4d2a19a9db4555e659f2b` removed the remote path preflight and was deployed from the clean build. The next ten minute oracle still recorded zero Gathered events, but gathering travel became active immediately, coordinator claims rose from zero to 57, and actors reached `gathering_resource`. Live position correlation then showed actors waiting at inactive pooled spawn points and repeating a failed gathering action for the full five minute destination lease. The remaining work is to search the destination's other unvisited points until an actually spawned resource enters the ordinary loot flow.

### Round 1 Judge Evidence

- Criterion 1a: `PlayerbotProfessionInteractionTest.RegisteredGatheringActionRecordsOnlyObservedLootForEveryProfession` creates the registered action and drives the real extension Loot event for herbalism, mining, and skinning. `PlayerbotGatheringActionTest.NearbyAutonomousGatheringClaimsEveryProfessionWithoutAGroup` independently covers the pure claim boundary.
- Criterion 1b: `PlayerbotGatheringActionTest.NearbyGatheringPreservesSafetyProfessionAndDuplicateClaimGuards` passes.
- Criterion 1c: `PlayerbotProfessionInteractionTest.RegisteredGatheringActionRecordsOnlyObservedLootForEveryProfession` proves a Loot event is silent before inventory changes, then records the exact authoritative inventory increase. `PlayerbotProfessionInteractionTest.BotRemovalReleasesObservedGatheringWithoutRecordingLoot` proves cleanup cannot create a late success.
- Criteria 2a through 2c: the production reserve, post reserve surplus, circulation material, controlled item, and runtime reserve tests pass in `PlayerbotEconomyPolicyTest`.
- Criteria 3a and 3b: the indivisible stack and purchase guard tests pass for the three specified deficit and stack pairs.
- Criteria 4a and 4b: the career recipe selection, concrete Auction House recipe, Wrath recipe decoder, and learned spell confirmation tests pass.
- Criteria 5a through 5c: the market equipment, real upgrade, same tier supply, rejection, and final use tests pass.
- Criteria 6a and 6b: trace boundary tests reject failed operations and accept gathered, listed, purchased, settled, and expired events only after successful boundaries.
- Criterion 6c: telemetry and Medivh fixture tests render `no_candidate` as an attempt result while preserving destination and profession capability failures.
- Criteria 7a through 7c: `EconomyPageTest` passes with the read only AzerothCore connections, 25 active rows over two pages, invalid page rejection, expired row exclusion, and stale telemetry exclusion.
- Criterion 8a: the authenticated `https://medivh.test/economy` browser run showed 70 active rows over four pages at 1440 by 1000, navigated to page 2, and reported no horizontal overflow at 1440 by 1000 and 390 by 844.
- Criteria 8b and 8c: the Economy browser component fixture test renders Listed, Bought, Sold, Expired, and Gathered events plus distinct destination eligibility and post assignment scheduling blockers with item identity, quantity, actor, and trace provenance.
- Criterion 9a: base `mod-playerbots` is still clean at `bf57a512f65b3f0baef7dfecf4ceeb046ffd700b`; status, working tree diff, and staged diff are empty.
- Criterion 9b: economy standalone checks, telemetry standalone checks, the full AzerothCore worldserver and unit test build, the 12,362 test suite, and Medivh `composer ci:check` pass.
- Criterion 9c: `PlayerbotEconomyScenarioTest.GatheringThroughSaleAndExpirationReconcilesItemsAndGold` passes with zero auction or mail residue and conserved item and gold ledgers.

## Changes Review Closure

The changes review reported two required fixes and one recommended fix. All three are closed without another build round because they repair verification gaps in the completed round.

1. Medivh now accepts the chainless gathered event emitted by the real telemetry serializer. It still requires an opaque chain identity for every chained event. The feature test covers both sides of that contract.
2. The registered gathering action now has functional coverage through the action creator, the extension Loot event, an authoritative inventory increase, and bot removal cleanup. The test covers herbalism, mining, and skinning.
3. Recipe learning start and confirmation are now private runtime helpers because they have only one production caller. This removes the untested public class and keeps the recipe behavior behind the existing economy runtime boundary.

## Verification Record

The final verification pass completed on 2026-08-11.

1. Economy repository checks passed with five Python checks, the standalone CMake build, the standalone CTest target, the repository cppcheck suppression contract, Actionlint, and Clang Format 22 on every changed C++ file.

```text
python3 -m unittest tests/python/test_check_repository.py
python3 tools/check_repository.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cppcheck --enable=warning,style,performance,portability --error-exitcode=1 --inline-suppr --suppressions-list=.suppress.cppcheck --suppress=missingInclude --suppress=missingIncludeSystem --std=c++20 src tests/cpp
actionlint
```

2. The full AzerothCore build and test target passed. The worldserver and unit test executables rebuilt successfully, and CTest reported zero failures.

```text
cmake --build /Users/pierre/Workspace/azerothcore-wotlk/build-playerbot-claude-tests --target worldserver unit_tests --parallel 4
ctest --test-dir /Users/pierre/Workspace/azerothcore-wotlk/build-playerbot-claude-tests --output-on-failure
```

3. Telemetry repository checks passed with five Python checks, its standalone CMake build, its standalone CTest target, cppcheck, and Actionlint.

4. Medivh `composer ci:check` passed. Pint and PHPStan passed. Pest passed 143 tests with 996 assertions. Vitest passed 186 tests across nine files. ESLint, Prettier, TypeScript, Knip, and the production Vite build also passed.

5. Authenticated browser verification at `https://medivh.test/economy` showed 70 active listings across four pages. Page navigation worked. The 1440 by 1000 and 390 by 844 viewports had no horizontal overflow. The rendered activity distinguished listed, bought, sold, expired, and gathered outcomes.

6. The base `mod-playerbots` repository remains unchanged at `bf57a512f65b3f0baef7dfecf4ceeb046ffd700b`. Its status, working tree diff, and staged diff are empty.

## Not Verified

The live worldserver, telemetry consumer, and deployed Medivh instance were not restarted or deployed because lifecycle operations are outside this Buildout. The production bot population has therefore not yet been observed gathering, learning recipes, equipping purchases, or circulating these goods after this change.

The repository CI uses Clang Format 18. That binary is not installed on this machine. Every changed C++ file passes Clang Format 22, but exact Clang Format 18 parity was not executed locally.

## Changed Files

- `docs/builds/2026-08-11-make-playerbots-gather-nearby.md`
- `src/Ai/Base/Actions/EconomyGatheringAction.cpp`
- `src/Ai/Base/Actions/EconomyGatheringAction.h`
- `src/Bot/Economy/PlayerbotEconomyGathering.cpp`
- `src/Bot/Economy/PlayerbotEconomyGathering.h`
- `src/Bot/Economy/PlayerbotEconomyConfig.h`
- `src/Bot/Economy/PlayerbotEconomyPolicy.cpp`
- `src/Bot/Economy/PlayerbotEconomyPolicy.h`
- `src/Bot/Economy/PlayerbotEconomyConsumption.cpp`
- `src/Bot/Economy/PlayerbotEconomyConsumption.h`
- `src/Bot/Economy/PlayerbotEconomyRuntime.cpp`
- `src/Bot/Economy/PlayerbotEconomyTrace.cpp`
- `src/Bot/Economy/PlayerbotEconomyTrace.h`
- `src/PlayerbotsEconomyExtension.cpp`
- `conf/mod_playerbots_economy.conf.dist`
- `tests/cpp/PlayerbotCareerPlanTest.cpp`
- `tests/cpp/PlayerbotEconomyConfigTest.cpp`
- `tests/cpp/PlayerbotEconomyConsumptionTest.cpp`
- `tests/cpp/PlayerbotEconomyPolicyTest.cpp`
- `tests/cpp/PlayerbotEconomyScenarioTest.cpp`
- `tests/cpp/PlayerbotEconomyTraceTest.cpp`
- `tests/cpp/PlayerbotGatheringActionTest.cpp`
- `../mod-playerbots-telemetry/src/Bot/Telemetry/PlayerbotEconomyTelemetry.cpp`
- `../mod-playerbots-telemetry/tests/cpp/PlayerbotTelemetryTest.cpp`
- `../../../medivh/app/Telemetry/EconomyEvidenceTelemetryPayload.php`
- `../../../medivh/app/Http/Controllers/EconomyController.php`
- `../../../medivh/app/Http/Requests/EconomyPageRequest.php`
- `../../../medivh/app/Repositories/AzerothAuctionHouseRepository.php`
- `../../../medivh/resources/js/components/economy/economy-overview.tsx`
- `../../../medivh/resources/js/pages/economy.tsx`
- `../../../medivh/resources/js/types/economy.ts`
- `../../../medivh/tests/Feature/Http/EconomyPageTest.php`
- `../../../medivh/tests/Feature/Telemetry/TelemetryCollectorTest.php`
- `../../../medivh/tests/js/economy-page.test.tsx`
