# Changelog — UE 5.8 upgrade + impact-ranked review

## Part 1 — UE 5.8-aware methodology

- **Geometry/Nanite** (`CheckGeometryAndNanite`): replaced the old "assign a
  legacy LOD group" fix with a Nanite-first check. Non-Nanite, high-triangle
  meshes are now flagged with an "enable Nanite" auto-fix instead, since
  triangle count stops being the primary cost driver once Nanite is on.
- **Nanite material cost** (`CheckMaterialCostAndShadowCache`, new): flags
  Masked blend mode and Pixel Depth Offset on Nanite meshes, since both force
  Nanite off its fixed-function rasterization path. Report-only — editing a
  material's blend mode isn't a safe automated action.
- **Shadow cache invalidation** (same function): flags static-mobility
  meshes using World Position Offset whose `ShadowCacheInvalidationBehavior`
  is left at `Auto`, with an auto-fix that sets it to `Rigid`. Combined into
  the same pass as the material-cost check since both need the same
  per-mesh material scan.
- **Lighting** (`CheckLighting`, rewritten): now branches on whether
  MegaLights is enabled for the project. With MegaLights on, light *count*
  is no longer flagged (its cost is roughly constant); instead the check
  looks for dense per-pixel light overlap clusters and per-light MegaLights
  opt-outs. Without MegaLights, the old movable-shadow-casting-light check
  is kept but its magnitude is now tied to the systemic count of similar
  lights rather than treated as an isolated one-off.
- **Lumen configuration** (`CheckLumenConfiguration`, new): project-wide,
  report-only note recommending evaluation of Lumen Lite when standard Lumen
  is enabled, since it's a scalability-vs-fidelity tradeoff, not a per-actor
  fix.
- **World Partition / HLOD** (`CheckWorldPartitionHLOD`, new): detects
  World Partition levels with little or no HLOD Layer coverage and raises a
  level-wide issue scaled by uncovered actor count. Report-only — HLOD setup
  is a workflow decision.
- **Instancing** (`CheckInstancingCandidates`, extended): added a guarded
  auto-fix path. A group of actors sharing a mesh is only offered the
  automated HISM merge when none of them have attached children, simulated
  physics, tags, or are a Blueprint subclass; otherwise the group falls back
  to report-only with the reason stated.
- **Texture streaming** (`CheckTextureStreaming`, expanded): added
  `NeverStream` misuse detection, normal-map compression-setting checks, and
  a project-wide streaming-pool-budget note alongside the existing
  oversized-texture check.
- **Animation budget** (`CheckAnimationBudget`, new): recommends enabling
  the Animation Budget Allocator once a level has enough skeletal mesh
  actors to matter.
- **Actor tick** (`CheckActorTick`, new): flags actors ticking every frame
  with no Tick Interval set, plus a level-wide count. Report-only, since
  tick timing can be gameplay-visible.

## Part 2 — Effectiveness estimation

- Added `FSceneIssueImpact` (`EImpactMetric`, `EstimatedMagnitude`,
  `Confidence`, `PriorityScore`) to `FSceneIssue` in `SceneIssue.h`.
- Every check now populates `Metric`/`EstimatedMagnitude`/`Confidence` from
  data already gathered during that check (triangle counts, actor-group
  sizes, texture dimensions, etc.) — no separate profiling pass.
- `FSceneAuditor::NormalizeImpactScores` runs once, after all checks, and
  converts each issue's raw magnitude into a 0–100 `PriorityScore`,
  normalized against the largest magnitude seen for that metric in the same
  audit run, weighted by severity and confidence. Unclassified issues
  (project-wide structural notes with no comparable number) are scored from
  severity alone rather than zeroed out.

## Part 3 — UI/workflow

- The report list and review queue are now sorted by `PriorityScore`
  descending within each category (`FSceneAuditor::RunGeneralAudit` sorts
  once; the window's `BuildReviewQueue` preserves that order).
- Every issue's estimated impact is shown inline, always labeled
  "Estimated impact: ... (confidence: ..., priority N/100)" — never
  presented as a measured number.
- Added a per-category impact rollup above the report list.
- Added Export JSON / Export CSV buttons (`SceneOptimizerExport.h/.cpp`).

## Part 4 — Engineering robustness

- Every `ApplyFix` now wraps its edit in `FScopedTransaction`, so approved
  fixes are undoable with Ctrl+Z.
- `RunGeneralAudit` runs under `FScopedSlowTask` with a cancel button
  (`bShowProgressDialog` parameter lets headless callers skip the dialog).
- All thresholds moved from `constexpr` values into `USceneOptimizerSettings`
  (`UDeveloperSettings`), exposed under Project Settings > Plugins > Scene
  Optimizer.
- Added `FSceneOptimizerExport` for JSON/CSV report export.
- Added `USceneOptimizerAuditCommandlet` for headless/CI use
  (`-run=SceneOptimizerAudit -Map=... -Out=...`), returning a non-zero exit
  code when Critical issues are found.

## Follow-up pass — closing gaps found in self-assessment

- **Shadow cache invalidation fix** (`CheckMaterialCostAndShadowCache`): now
  also checks for `UTimelineComponent` and `UMovementComponent` on the actor
  before offering the Rigid auto-fix. Static mobility alone doesn't prove an
  actor is visually static at runtime — a Timeline-driven WPO effect is
  exactly the case where forcing Rigid could visibly break the shadow. If
  either is present, the issue is still reported (lower confidence) but
  `bAutoFixable` stays false.
- **MegaLights overlap-check cap**: the O(n²) light-clustering pass's light
  count cutoff moved from a hardcoded `constexpr` to
  `USceneOptimizerSettings::MegaLightsMaxLightsForOverlapCheck`, consistent
  with every other threshold in the plugin.
- **Explicit includes**: added `Engine/EngineTypes.h`, `Engine/TextureDefines.h`,
  and `RHIDefinitions.h` to `SceneAuditor.cpp` for `BLEND_Masked`,
  `TEXTUREGROUP_*`, `TC_Normalmap`, `EMaterialQualityLevel`, and
  `ERHIFeatureLevel`, which were previously reached only through transitive
  includes.

## Known follow-ups / things to verify against your exact 5.8 point release

A few accessors are named to the best available information at the time of
writing and are called out with inline comments where they're most likely to
need a one-line fix: `r.MegaLights` / `bAllowMegaLights` (MegaLights
enablement), `r.Lumen.Lite` (Lumen Lite), `a.Budget.Enabled` (Animation
Budget Allocator), and `Mesh->NaniteSettings.bEnabled` vs. a possible
`IsNaniteEnabled()` convenience accessor. None of these are structural —
each is a single property/CVar name to confirm against your engine build.
