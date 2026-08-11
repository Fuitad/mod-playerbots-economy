# Make Playerbots Gather Nearby Buildout

Created: 2026-08-11
Author: magitekrr@gmail.com
Agent: Codex
Status: VERIFIED
Approved: Yes
Rounds: 2
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
- [x] Criterion 9a: At hand back, the base `mod-playerbots` checkout contains only the exact three-file staged Quest Share candidate Pierre directed this run to preserve, with no economy changes or overlapping unstaged changes and with the final index blobs recorded as evidence.
- [x] Criterion 9b: The economy module repository checks, the full AzerothCore integration test command, and Medivh `composer ci:check` each exit successfully, with their exact commands and results recorded.
- [x] Criterion 9c: A deterministic circulation test proves item quantities and gold reconcile after gathering, use, listing, purchase, sale, and expiration.
- [x] Criterion 10 (oracle): With the deployed economy build and approximately 200 active random bots, a ten minute observation records at least one authoritative Gathered event from a bot that encountered an eligible herb, mineral node, or skinnable corpse.

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
- [x] Task 6: Make live nearby gathering trigger from eligible world resources.

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
- Round 2: Pierre introduced a separately verified Quest Share candidate after the original clean base baseline, so Criterion 9a changed from requiring a clean base checkout to requiring preservation of that exact three-file staged candidate. Task 6 closed the live gathering gap after five failed candidates and one behavior neutral diagnostic deployment. Judge: 26 of 26 criteria pass. The oracle passed 21 seconds into the final observation with 240 bots when Ahldskyf skinned one Medium Leather and Medivh projected authoritative event `evt_393ff6c9cda3becb`.
- Post hand back live observation: reopened after approximately 200 active bots produced zero Gathered events in ten minutes. All 49 currently sampled gathering actors ended on `gathering_destination_wrong_map`, no gathering trip became active, and the ordinary gather action log contained only failed executions. Added Task 6 and the live oracle as Criterion 10 without changing the previously judged criteria.
- Task 6 first deployment: commit `cb3377d59427c840cbeb59e5c41b216038418a80` built, passed the focused gathering suite and full CTest, and was deployed after Pierre disconnected. The ten minute oracle still recorded zero Gathered events with 210 bots. Live inspection proved `add gathering loot` executed 1,336 times and failed every time, while 52 gathering assignments all reported `gathering_destination_wrong_map`. Career professions and gathering tools matched for every gathering actor. The catalog's direct path preflight also produced mass missing navmesh tile loads before any travel target could own routing, so Task 6 remains open at the destination eligibility boundary.
- Task 6 second deployment: commit `b3aa760c58fb1d9169c4d2a19a9db4555e659f2b` removed the remote path preflight and was deployed from the clean build. The next ten minute oracle still recorded zero Gathered events, but gathering travel became active immediately, coordinator claims rose from zero to 57, and actors reached `gathering_resource`. Live position correlation then showed actors waiting at inactive pooled spawn points and repeating a failed gathering action for the full five minute destination lease. The remaining work is to search the destination's other unvisited points until an actually spawned resource enters the ordinary loot flow.
- Task 6 third deployment: commit `d065dc78f42a44de799cb84d8fc6d7e4a0dd2458` rotated gathering work through unvisited static points and was deployed in the combined worldserver candidate with the staged Quest Share patch and Social commit `b348d3d11e9235d6a46b84e8d08da596dbdc945a`. The installed binary hash was `d6a6e5d0a6c462e0db4d58022383ffd43b57d4b824a0b92a3e5ad11a52d302c2`. With 240 bots online, the ten minute authoritative observation advanced the trace generation from 108 to 220 but still recorded zero Gathered events. A subsequent fifteen sample boundary trace advanced generation from 281 to 315. Every sample contained two to nine `gathering_search` actors and zero `gathering_resource` actors. Source tracing then proved each rotated target still used the multi point parent destination for arrival. Standing on the old point therefore satisfied `TravelDestination::isIn`, returned the new target to work without movement, and made rotation consume pointers while the bot stayed still. The next fix gives each travel target a point specific destination while retaining the parent catalog and shared point visitor accounting.
- Task 6 fourth deployment: commit `9415e4e88fdb98db6afe89c512e3ddaa00af890e` gave each gathering target a point specific destination and was deployed in the same combined pipeline. The installed binary hash was `72fbad71d1b2dd72bc08de4d553a8af1f116af1b540739b624f021af711167bc`, and worldserver ran as PID 50291 with all three listeners. With 240 bots online, the ten minute authoritative observation advanced the trace from 92 to 207 across 115 new events but still recorded zero Gathered events. Live actors now reached `gathering_complete`. Direct source tracing then proved `StoreLootAction` queues `CMSG_AUTOSTORE_LOOT_ITEM` and immediately broadcasts the Loot event before the session applies the inventory change. The module therefore checked the authoritative item count too early and received no later event to retry it. The next fix defers the same inventory delta confirmation by the configured loot delay on the owning player event processor.
- Task 6 fifth deployment: commit `2960e698918366fe47e653ec669405df04e5b9e7` deferred loot confirmation by the configured loot delay and was deployed in the same combined pipeline. The installed binary hash was `7235b96328ad71ee985d1967cf9eaf3bbb1f29eafaf05b98b4d4414e46b30ec7`, and worldserver ran as PID 56968 with all three listeners. With 240 bots online, the 619 second authoritative observation advanced the trace from 18 to 205 across 187 new events but still recorded zero Gathered events. Gathering actors again reached `gathering_complete`, while a post oracle inspection found only failed `add gathering loot` as the latest attempt for the sampled actors. The next step instruments the nearby claim boundary narrowly enough to distinguish an in range missing path rejection from a downstream ordinary loot stack failure without changing behavior.
- Task 6 diagnostic deployment: commit `55ec33a` added one behavior neutral debug line for an otherwise eligible claim rejected as missing path while already in interaction range. The diagnostic binary hash was `08c18b85dd01f51c5e59616fbb1ab20130ed10d5996fb44439e9d7ccdd0891c0`, and worldserver ran as PID 62515 with all three listeners. With 240 bots online, the line fired for actor 828 at zero yards from a resource while the actor was safe, on the same map and phase, had gathering affinity 32, and had skill 167 against required skill 80. This proves the nearby claim required navmesh path availability after travel had already completed and no path was needed. The production fix retains the path gate outside interaction range and removes the diagnostic.

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

### Round 2 Judge Evidence

- Criteria 1a through 1c: the current full AzerothCore suite passes 12,372 tests, including registered herb, mineral, and skinning actions, authoritative post inventory confirmation, bot removal cleanup, the in range path regression, and every existing gathering guard.
- Criteria 2a through 2c: the same current suite passes the reserve, immediate use, bounded surplus, cloth, herb, ore, leather, and controlled item policy and runtime cases.
- Criteria 3a and 3b: the current suite passes all indivisible stack purchase cases and every account, access, reserve, budget, and price rejection.
- Criteria 4a and 4b: the current suite passes unknown relevant recipe acquisition and learned spell confirmation plus all known, unusable, irrelevant, duplicate, account, budget, and price rejections.
- Criteria 5a through 5c: the current suite passes uncommon equipment utility, same tier real upgrade, final equip, and every compatibility, account, budget, price, and nonupgrade rejection.
- Criteria 6a through 6c: the current suite passes authoritative trace boundaries for gathered and every market outcome, while the Medivh fixtures retain distinct capability and attempt blocker scopes.
- Criteria 7a through 7c: the unchanged Medivh authoritative Auction House projection remains covered by `EconomyPageTest`, including live database connections, pagination, complete counts, invalid input, expiry, and stale telemetry exclusion.
- Criteria 8a through 8c: the unchanged browser artifact and component fixtures retain the recorded desktop and mobile pagination, overflow, lifecycle outcome, blocker scope, gathered identity, quantity, and provenance evidence.
- Criterion 9a: base Playerbots remains at `9c25a871498363838346e691dcc0315348c27b05` with only staged blobs `df4f9e6847dc50b9034c820e4a585734f4f981be`, `c74345df8b62a967e6e88ad2968c77163346cd34`, and `8ffa83bb664c053d11287064abd64c80128dac53`; `git diff --cached --check` passes and no unstaged overlap exists.
- Criterion 9b: the current economy repository checks, standalone CTest, cppcheck, Actionlint, Clang Format 22, integrated worldserver and unit test build, and full 12,372 test CTest pass. The unchanged Medivh `composer ci:check` evidence remains recorded from Round 1 and is rerun in final verification.
- Criterion 9c: the current full suite passes the deterministic gathering through sale and expiration reconciliation scenario.
- Criterion 10: deployed economy commit `660fcfee15768c4e76987ffa14f1fda54465cf33` and binary `366f0e07274f1d09a206312c08c9d868dbbe3b26136cc711c261c37c82ff11a2` ran as PID 64576 with 240 bots. At 21 seconds, Medivh trace generation advanced from 139 to 146 and projected `evt_393ff6c9cda3becb`: bot Ahldskyf, GUID 712, skinned one Medium Leather, item 2319, with the live inventory inspection also reporting one Medium Leather.

## Changes Review Closure

The changes review reported two required fixes and one recommended fix. All three are closed without another build round because they repair verification gaps in the completed round.

1. Medivh now accepts the chainless gathered event emitted by the real telemetry serializer. It still requires an opaque chain identity for every chained event. The feature test covers both sides of that contract.
2. The registered gathering action now has functional coverage through the action creator, the extension Loot event, an authoritative inventory increase, and bot removal cleanup. The test covers herbalism, mining, and skinning.
3. Recipe learning start and confirmation are now private runtime helpers because they have only one production caller. This removes the untested public class and keeps the recipe behavior behind the existing economy runtime boundary.

## Verification Record

The final verification pass completed on 2026-08-11.

Profile: Full

Live target: Tier 1 passed. The installed worldserver, the authenticated production Medivh economy page, and the running Medivh collector and telemetry consumer were all available and exercised directly. No fallback tier was needed.

1. Economy repository checks passed with five Python checks, the standalone CMake build, the standalone CTest target, the repository cppcheck suppression contract, Actionlint, and Clang Format 22 on every changed C++ file.

```text
python3 -m unittest tests/python/test_check_repository.py
python3 tools/check_repository.py
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 4
ctest --test-dir build --output-on-failure
cppcheck --enable=warning,style,performance,portability --error-exitcode=1 --inline-suppr --suppressions-list=.suppress.cppcheck --suppress=missingInclude --suppress=missingIncludeSystem --std=c++20 src tests/cpp
actionlint
/opt/homebrew/opt/llvm/bin/clang-format --version
git diff --name-only HEAD~4..HEAD -- '*.cpp' '*.h' | xargs /opt/homebrew/opt/llvm/bin/clang-format --dry-run --Werror
```

2. The full AzerothCore build and test target passed. The worldserver and unit test executables rebuilt successfully, and CTest reported zero failures.

```text
cmake --build /Users/pierre/Workspace/azerothcore-wotlk/build-playerbot-claude-tests --target worldserver unit_tests --parallel 4
ctest --test-dir /Users/pierre/Workspace/azerothcore-wotlk/build-playerbot-claude-tests --output-on-failure
```

3. Telemetry repository checks passed with five Python checks, its standalone CMake build, its standalone CTest target, cppcheck, and Actionlint.

4. Medivh `composer ci:check` passed. Pint and PHPStan passed. Pest passed 143 tests with 996 assertions. Vitest passed 186 tests across nine files. ESLint, Prettier, TypeScript, Knip, and the production Vite build also passed.

5. Authenticated browser verification at `https://medivh.pierrelucbrunet.com/economy` opened the live page in Pierre's existing Chrome session. The first snapshot showed 102 authoritative listings across six pages and current skinning, mining, and herbalism events. Navigation to page 2 succeeded. The refreshed response then showed 132 listings across seven pages and a new four unit Wool Cloth listing. The 1440 by 1000 and 390 by 844 viewports had no horizontal overflow. `impeccable detect --json` reported zero findings against the captured rendered DOM. The only console errors came from invalidated third party ad blocking extension contexts, not Medivh.

6. The base `mod-playerbots` repository is at `9c25a871498363838346e691dcc0315348c27b05` and contains only Pierre's staged Quest Share candidate. `git diff --cached --check` passes, the working tree has no unstaged change, and the final index blobs are `df4f9e6847dc50b9034c820e4a585734f4f981be`, `c74345df8b62a967e6e88ad2968c77163346cd34`, and `8ffa83bb664c053d11287064abd64c80128dac53`.

7. The independent final changes review returned PASS with high compliance, quality, and goal scores. It verified all four final truths and reported no issues.

8. The retained build artifact and installed worldserver both have SHA256 `366f0e07274f1d09a206312c08c9d868dbbe3b26136cc711c261c37c82ff11a2`. Worldserver PID 64576 was still running with listeners on 8085, 8888, and 24601. The corrected deployment receipt records economy commit `660fcfee15768c4e76987ffa14f1fda54465cf33`, and that commit exists at the repository HEAD. The oracle event `evt_393ff6c9cda3becb` appeared after 21 seconds with 240 bots when Ahldskyf skinned one Medium Leather. Continued observation later showed multiple live skinning, mining, and herbalism events plus new cloth listings.

## Not Verified

The repository CI uses Clang Format 18. That binary is not installed on this machine. Every changed C++ file passes Clang Format 22, but exact Clang Format 18 parity was not executed locally.

The separately staged Quest Share candidate is included in the running combined worldserver and has its source blobs preserved, but its eligible client acceptance still requires Pierre's in game action. This is outside the economy acceptance criteria.

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
