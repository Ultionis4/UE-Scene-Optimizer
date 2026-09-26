#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SceneOptimizerSettings.generated.h"

/**
 * Tunable thresholds for the Scene Optimizer audit, exposed under
 * Project Settings > Plugins > Scene Optimizer so a team can tune them
 * per-project without recompiling the plugin.
 */
UCLASS(Config = Editor, DefaultConfig, meta = (DisplayName = "Scene Optimizer"))
class SCENEOPTIMIZER_API USceneOptimizerSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	USceneOptimizerSettings();

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	/** Non-Nanite meshes above this triangle count with only 1 LOD are flagged. */
	UPROPERTY(EditAnywhere, Config, Category = "Geometry", meta = (ClampMin = "0"))
	int32 HighTriangleCountThreshold = 5000;

	/** Minimum number of actors sharing one static mesh before flagging an instancing candidate. */
	UPROPERTY(EditAnywhere, Config, Category = "Geometry", meta = (ClampMin = "2"))
	int32 InstancingCandidateMinCount = 5;

	/** Textures wider or taller than this with no max-size override are flagged. */
	UPROPERTY(EditAnywhere, Config, Category = "Textures", meta = (ClampMin = "256"))
	int32 LargeTextureThreshold = 2048;

	/** Max size applied by the "cap oversized texture" auto-fix. */
	UPROPERTY(EditAnywhere, Config, Category = "Textures", meta = (ClampMin = "256"))
	int32 TextureMaxSizeFix = 2048;

	/** Legacy (non-MegaLights) path: movable shadow-casting light count that triggers a level-wide summary. */
	UPROPERTY(EditAnywhere, Config, Category = "Lighting", meta = (ClampMin = "1"))
	int32 LegacyShadowLightCountThreshold = 8;

	/** MegaLights path: radius (cm) used to detect overlapping light clusters. */
	UPROPERTY(EditAnywhere, Config, Category = "Lighting", meta = (ClampMin = "1"))
	float MegaLightsOverlapRadius = 500.f;

	/** MegaLights path: number of nearby lights within the radius above before flagging a dense cluster. */
	UPROPERTY(EditAnywhere, Config, Category = "Lighting", meta = (ClampMin = "2"))
	int32 MegaLightsOverlapCountThreshold = 6;

	/**
	 * MegaLights path: the pairwise overlap-clustering check is O(n^2), so it's
	 * skipped above this many lights in the level to avoid a long stall on a
	 * single audit step. Raise it if your levels have more lights than this
	 * and you're willing to trade audit time for the check.
	 */
	UPROPERTY(EditAnywhere, Config, Category = "Lighting", meta = (ClampMin = "0"))
	int32 MegaLightsMaxLightsForOverlapCheck = 2000;

	/** Skip the World Partition / HLOD coverage check below this many static mesh actors. */
	UPROPERTY(EditAnywhere, Config, Category = "World Partition", meta = (ClampMin = "0"))
	int32 MinActorsBeforeHLODWarning = 50;

	/** Skeletal mesh actor count before recommending the Animation Budget Allocator. */
	UPROPERTY(EditAnywhere, Config, Category = "Animation", meta = (ClampMin = "0"))
	int32 SkeletalMeshCountForBudgetWarning = 20;

	/** Whether to flag actors that tick every frame with no Tick Interval set. */
	UPROPERTY(EditAnywhere, Config, Category = "Gameplay")
	bool bWarnOnFullRateTick = true;
};
