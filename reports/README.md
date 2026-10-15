# Execution reports

[English](README.md) | [한국어](README.ko.md)

These reports record actual Unreal Engine execution of the original map, Blueprint functions, widgets, AI controllers and navigation. They cover specified scenarios with controlled fixtures. Counts describe behavior assertions or sampled events, rather than machine-learning accuracy or complete coverage of arbitrary play.

Start with [results and measurement boundaries](../docs/results.md), [source provenance](../docs/source-provenance.md) and [reproduction commands](../docs/reproduce.md). [Blueprint inspection material](../inspections/README.md) provides the graph and Data Table evidence behind the expectations.

## Source restoration and final execution

| Report | Evidence |
| --- | --- |
| [Fresh-restoration run](fresh-restoration/run.json) | A separate restored project, actual Windows source-build check, Blueprint preflight and all seven native gameplay suites |
| [Fresh Blueprint inspection](fresh-restoration/blueprints.json) | Actual editor asset loading and compilation |
| [Fresh world inventory](fresh-restoration/world.json) | Original map, possessed player and original NPC instances |
| [Fresh state contracts](fresh-restoration/states.json) | Clock, dialogue, trust, follow and schedule fixtures |
| [Fresh interactions](fresh-restoration/interactions.json) | Target selection, actual UMG transactions and item sampling |
| [Fresh quest contracts](fresh-restoration/quests.json) | Quest requirements and reward state changes |
| [Fresh quest-proposal sampling](fresh-restoration/probability.json) | Conditional quest proposals in the actual Talk path |
| [Fresh navigation](fresh-restoration/navigation.json) | Actual patrol, follow and home movement |
| [Fresh inventory input](fresh-restoration/inventory-input.json) | Actual input stack, shop display threshold and real item respawn delay |
| [Working-project functional run](functional-run.json) | Separate native run summary from the maintained working project |

The fresh-restoration summary records successful execution, including seven successful automation suites. The suite count is not the sum of individual assertions. Source identity, build success and gameplay assertions are distinct checks. The runner checks report assertions and automation results as well as process exit codes.

These are native Windows Unreal Engine 5.4.4 runs on the maintainer's PC. A fresh restored directory and source build establish more than a copied-binary check, but do not establish independent-machine or virtual-machine certification. Tool-level Windows/Linux checks do not certify Linux game execution. A source-build entry that finds an existing build up to date is not a cold-build timing.

## Initial and corrected behavior

| Report | Result and interpretation |
| --- | --- |
| [Initial state contracts](initial-state-contracts-valid.json) | 69 passed and 9 failed out of 78; a valid failing baseline |
| [Corrected state contracts](corrected-state-contracts.json) | 78 passed out of the same 78 contracts |
| [Initial Blueprint inspection](initial-blueprints.json) | Original compilation observations, including vendor warnings |
| [Final Blueprint inspection](final-blueprints.json) | Maintained asset compilation observations |
| [World inventory](world-inventory.json) | Actual PIE actor and player setup |
| [Interaction contracts](corrected-interaction-contracts.json) | Target matrix, widget transactions, food use and rare-item sampling |
| [Quest contracts](corrected-quest-contracts.json) | Eight composite checkpoints for requirement boundaries and duplicate-reward prevention |
| [Quest-proposal sampling](corrected-quest-probability.json) | 1,003 proposals in 10,000 eligible Talk calls, seed 378 |
| [Navigation contracts](final-navigation-contracts.json) | Three stages with observed movement and native path state |
| [Inventory input contracts](final-inventory-input-contracts.json) | Six checkpoints with native E press/release and the original 60-second respawn |

The initial report's failed status is intentional evidence: eight expectations concern critical ordinary dialogue being displaced by the new-quest roll, and one concerns home intent during an open interaction. Invalid fixture attempts are not counted as that baseline or merged into the corrected totals.

The target matrix covers 30 predefined actor-forward conditions and excludes exact angular ties. It does not measure camera targeting. Quest and trade fixtures use temporary state to reach specified boundaries; they do not claim every possible quest, NPC sequence or UI layout. Sampling counts have their own denominators: 10,000 eligible Talk calls are not 10,000 completed quests, while the 976 rare-item outcomes are from a separate 10,000-call experiment.

Inventory input uses the actual PlayerController input stack and widgets, rather than physical keyboard events from the operating system. Navigation uses actual world ticks and path following. The home test supplies a reachable nearby home fixture and does not certify every authored home destination. The speed oracle reads the authored Behavior Tree task instance, preserving its speed of 150 despite a class default of 300.

## Declined optimization and final profiles

| Evidence | Interpretation |
| --- | --- |
| [Predefined lookup experiment plan](declined-lookup/performance-experiment-plan.json) | Adoption threshold fixed before candidate measurements |
| [Candidate application](declined-lookup/candidate-application.json) | Reviewable lookup-consolidation application |
| [Lookup candidate decision](declined-lookup/lookup-candidate-decision.json) | 12.97435 ms to 12.45090 ms, a 4.03% reduction below the 5% gate; candidate declined |
| [Initial and candidate raw runs](declined-lookup/) | Three 50-NPC baseline and three candidate measurements, retained separately |
| [Final profile batch](final-profile-batch.json) | Nine validated runs: 5, 20 and 50 NPCs, three repetitions each |
| [Final raw profiles](profiles/) | Frame samples, controller and brain counts, per-actor movement and path observations |

The lookup experiment used the same initial-source workload for both sides. Its active flags did not prove valid walking paths. The original lookup structure was restored after the candidate missed the adoption threshold. These measurements are not comparable to the final navigation-restored workload as a performance gain or regression.

All nine final profiles passed their capture checks. Each run observed every requested NPC and confirmed the requested count of AI controllers and running brains. Movement acceptance requires at least one same actor to have both more than 25 uu of 2D displacement and an observed moving complete path. It does not require all NPCs to walk continuously. The five-NPC runs observed 5 of 5 such actors; the twenty-NPC runs observed 15 to 18, and the fifty-NPC runs 40 to 41.

The final runs use rendered D3D12 at an actual 1280 × 720 viewport, a 10-second warmup and a 15-second capture. World actor-tick spans include all actors and intervening world work. Editor end-frame intervals also include editor, rendering waits and observation overhead. Movement probes run outside the actor-tick span but contribute to end-frame intervals. These are neither isolated NPC CPU timings nor packaged-game FPS.

## Published record integrity

[Report provenance](report-provenance.json) maps each published JSON to its source report and records both hashes. Local filesystem prefixes are redacted and JSON formatting is normalized. Gameplay values, numeric measurements and sample arrays are retained, including the initial failing status and the declined optimization decision. The curated reports preserve the evidence needed to review the project without publishing unrelated working history.
