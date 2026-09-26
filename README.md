[README.md](https://github.com/user-attachments/files/32676808/README.md)
# Scene Optimizer (UE 5.8 Editor Plugin)

An editor-only C++ plugin that audits the currently open level against
UE 5.8-era performance methodology (Nanite, MegaLights, Virtual Shadow Maps,
World Partition/HLOD, Animation Budget Allocator), ranks issues by estimated
impact, and walks you through fixes one at a time — nothing is changed
without you clicking Approve.

## Install

1. Copy the `SceneOptimizer` folder into your project's `Plugins/` directory
   (create that folder at the project root if it doesn't exist yet):
   ```
   YourProject/
     Plugins/
       SceneOptimizer/
         SceneOptimizer.uplugin
         Source/...
   ```
2. Right-click your `.uproject` file → **Generate Visual Studio project files**
   (or regenerate via Rider), then open the project — Unreal will prompt to
   rebuild the plugin's module.
3. Once compiled, open **Window → Scene Optimizer** in the editor.
4. Optional: tune thresholds under **Project Settings → Plugins → Scene
   Optimizer** before your first run.

## How it works

1. **Run Audit** scans every actor in the currently open level under a
   cancellable progress dialog and produces a categorized report (Geometry /
   Lighting / Textures / Misc), each item tagged Info / Warning / Critical
   and carrying an **estimated impact** (e.g. "~48,000 triangles
   addressable", "~112 MB", confidence: medium) derived from data already
   gathered during the scan — never a profiler measurement, always labeled
   as an estimate. Issues within each category are sorted highest-impact
   first. A per-category rollup sits above the list so you can see where the
   biggest wins are before starting review.
2. **Begin Review** walks only the *auto-fixable* issues, highest impact
   first within each category, in a fixed order (Geometry → Lighting →
   Textures → Misc). For each one you can:
   - **Approve & Apply** — runs that issue's fix immediately, wrapped in an
     undoable transaction (Ctrl+Z reverts it like any other editor edit).
   - **Skip** — leaves it untouched, moves to the next.
   - **Skip Rest of Category** — jumps straight to the next category.
   - **Stop Review** — pauses; clicking Begin Review again resumes from the
     current position (already-resolved issues are excluded).

   Report-only issues (things the tool deliberately doesn't auto-fix, like
   masked/PDO materials on Nanite meshes, HLOD coverage gaps, or instancing
   candidates with per-actor logic) stay visible in the list above so you
   can act on them by hand.
3. **Export JSON / Export CSV** writes the full report to disk for tracking
   over time or archiving as a build artifact.

## Checks included

| Category | Check | Auto-fixable? | Notes |
|----------|-------|----------------|-------|
| Geometry | Non-Nanite, high-triangle mesh with 1 LOD | Yes (enables Nanite) | 5.8-aware: prefers enabling Nanite over hand-built LOD chains |
| Geometry | Masked / Pixel Depth Offset material on a Nanite mesh | No (report only) | Forces Nanite off its fixed-function raster path |
| Geometry | 5+ actors sharing one static mesh | Yes, if all actors are "plain" (no physics/children/tags/subclass) | Falls back to report-only per-group otherwise |
| Geometry | Static mesh with no collision geometry | No (report only) | Shape-dependent, needs a human decision |
| Lighting | Shadow Cache Invalidation left at Auto on static WPO geometry | Yes (sets Rigid) | Reduces unnecessary Virtual Shadow Map page churn |
| Lighting | MegaLights on: dense per-pixel light overlap clusters | No (report only) | MegaLights cost is ~constant regardless of count; overlap density is the real signal |
| Lighting | MegaLights on: per-light MegaLights opt-out | No (report only) | Flags lights falling back to a more expensive path |
| Lighting | MegaLights off: movable, shadow-casting lights | Yes (sets Stationary) | Also suggests evaluating MegaLights project-wide past a threshold |
| Lighting | Standard-quality Lumen in use | No (report only) | Suggests evaluating Lumen Lite for scalability |
| Textures | Oversized texture with no max-size override | Yes (caps size) | |
| Textures | `NeverStream` on a large, non-UI/lightmap texture | No (report only) | Permanently reserves VRAM outside the streaming pool |
| Textures | Normal map with wrong compression setting | Yes (fixes compression) | |
| Textures | Level's textures over the configured streaming pool | No (report only) | Project-wide budget note |
| Misc | World Partition level with little/no HLOD coverage | No (report only) | Usually the single highest-leverage fix for a large level |
| Misc | Many skeletal meshes without Animation Budget Allocator enabled | No (report only) | |
| Misc | Actors ticking every frame with no Tick Interval | No (report only) | Gameplay-visible, needs a human decision |

Thresholds for all of the above live in **Project Settings → Plugins →
Scene Optimizer** (`USceneOptimizerSettings`).

## Effectiveness estimation

Every issue carries an `Impact` (see `SceneIssue.h`): a metric (triangles,
draw calls, MB, etc.), an estimated magnitude computed from data the audit
already gathered, a confidence level (lower when the check can't verify
runtime behavior from static data alone), and a normalized 0–100
`PriorityScore` used to sort the report and review queue. This is a static
heuristic, not a profiler result — always shown to the user with the word
"estimated" and a confidence label, never as a hard number.

## Headless / CI usage

```
UnrealEditor-Cmd.exe YourProject.uproject -run=SceneOptimizerAudit \
    -Map=/Game/Maps/YourLevel -Out=Saved/SceneOptimizerReport.json
```

Exits non-zero if any Critical issues are found, so a CI pipeline can fail
the build on a regression. `-Out` is optional; when given, the full issue
list is written as JSON.

## Extending it

Each check is a private `static` function in `FSceneAuditor`
(`SceneAuditor.h` / `.cpp`). To add a new one:

1. Write `void CheckSomething(UWorld* World, TArray<FSceneIssuePtr>& OutIssues)`.
2. Populate an `FSceneIssue` — set `Category`, `Severity`, `Description`,
   `TargetName`, and optionally `Actor`.
3. Populate `Issue->Impact.Metric` / `EstimatedMagnitude` / `Confidence`
   from data you already have in the function — don't add a new profiling
   pass. Leave `Metric` at `Unclassified` if there's no comparable number.
4. If it's safe to automate, set `bAutoFixable = true` and bind `ApplyFix`
   to a lambda that captures a `TWeakObjectPtr` (never a raw pointer — the
   level can change between audit and review), wraps its edit in
   `FScopedTransaction`, and returns `true`/`false`.
5. Call your new function from `RunGeneralAudit`, and add a matching
   `SlowTask.EnterProgressFrame` step.

Good next candidates: Mesh Terrain / Procedural Vegetation checks once those
5.8 features leave experimental status, overlapping/duplicate actors,
unused material instances, and a profiler-integrated mode that correlates
Unreal Insights capture data with these static checks for a much higher-
confidence impact estimate.

## Notes / caveats

- Targets UE 5.8. A few accessors (`r.MegaLights`, `bAllowMegaLights`,
  `r.Lumen.Lite`, `a.Budget.Enabled`) are named to the best available
  information at time of writing and are flagged inline in
  `SceneAuditor.cpp` — see `CHANGELOG.md`'s "Known follow-ups" section if
  any fail to compile against your exact point release.
- The plugin only touches the level when you click **Approve & Apply** (or
  run a commandlet, which never calls `ApplyFix` — it only reports). All
  scanning is read-only, and every fix is wrapped in an undoable transaction.
- `ApplyFix` lambdas call `MarkPackageDirty()` / `PostEditChange()` but don't
  save automatically — review your changes and save (or diff in source
  control) before committing.
