#pragma once

#include "CoreMinimal.h"
#include "SceneIssue.h"

class UWorld;
class AStaticMeshActor;
class ULightComponent;
class UMaterial;

/**
 * Stateless scan of a UWorld for common performance problems, updated for
 * UE 5.8-era methodology (Nanite material cost, MegaLights, Virtual Shadow
 * Map cache invalidation, World Partition/HLOD coverage, the Animation
 * Budget Allocator).
 *
 * Nothing in here mutates the level — that only happens later, from
 * FSceneIssue::ApplyFix, and only after the user approves it in the UI.
 * Every ApplyFix wraps its edit in FScopedTransaction so it's undoable.
 *
 * Extend by adding another Check* function, calling it from RunGeneralAudit,
 * and populating Impact on every issue you create (see SceneIssue.h).
 */
class FSceneAuditor
{
public:
	/**
	 * Runs every check below and returns the combined issue list, sorted by
	 * category and then by estimated PriorityScore (highest first).
	 *
	 * @param bShowProgressDialog  Shows a cancellable FScopedSlowTask dialog.
	 *        Pass false for headless/commandlet use (see
	 *        USceneOptimizerAuditCommandlet), since a modal dialog has
	 *        nothing to attach to outside the editor UI.
	 */
	static TArray<FSceneIssuePtr> RunGeneralAudit(UWorld* World, bool bShowProgressDialog = true);

private:
	// --- checks ---
	static void CheckGeometryAndNanite(UWorld* World, TArray<FSceneIssuePtr>& OutIssues);
	static void CheckMaterialCostAndShadowCache(UWorld* World, TArray<FSceneIssuePtr>& OutIssues);
	static void CheckInstancingCandidates(UWorld* World, TArray<FSceneIssuePtr>& OutIssues);
	static void CheckMissingCollision(UWorld* World, TArray<FSceneIssuePtr>& OutIssues);
	static void CheckLighting(UWorld* World, TArray<FSceneIssuePtr>& OutIssues);
	static void CheckLumenConfiguration(UWorld* World, TArray<FSceneIssuePtr>& OutIssues);
	static void CheckTextureStreaming(UWorld* World, TArray<FSceneIssuePtr>& OutIssues);
	static void CheckWorldPartitionHLOD(UWorld* World, TArray<FSceneIssuePtr>& OutIssues);
	static void CheckAnimationBudget(UWorld* World, TArray<FSceneIssuePtr>& OutIssues);
	static void CheckActorTick(UWorld* World, TArray<FSceneIssuePtr>& OutIssues);

	// --- project/engine state helpers ---
	// CVar names below match the most likely 5.8 accessors as of this
	// writing; if your build renamed one, these are the only places to fix.
	static bool IsMegaLightsProjectEnabled();
	static bool IsHardwareRayTracingSupported();

	// --- impact scoring ---
	static float SeverityWeight(ESceneIssueSeverity Severity);

	/**
	 * Populates Impact.PriorityScore on every issue, normalizing magnitude
	 * within each EImpactMetric bucket found across this audit run. Call
	 * exactly once, after every Check* function has run.
	 */
	static void NormalizeImpactScores(TArray<FSceneIssuePtr>& Issues);
};
