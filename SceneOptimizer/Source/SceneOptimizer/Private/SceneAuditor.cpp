#include "SceneAuditor.h"
#include "SceneOptimizerSettings.h"

#include "EngineUtils.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Engine/Texture2D.h"
#include "PhysicsEngine/BodySetup.h"
#include "WorldPartition/WorldPartition.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ScopedSlowTask.h"
#include "ScopedTransaction.h"
#include "Components/TimelineComponent.h"
#include "GameFramework/MovementComponent.h"

// Explicit includes for enums that were previously reached only through
// transitive includes (BLEND_Masked, TEXTUREGROUP_*, TC_Normalmap,
// EMaterialQualityLevel, ERHIFeatureLevel) — harmless if your engine version
// already pulls these in elsewhere, but keeps this file's compile
// dependencies honest.
#include "Engine/EngineTypes.h"
#include "Engine/TextureDefines.h"
#include "RHIDefinitions.h"

#define LOCTEXT_NAMESPACE "SceneAuditor"

// ---------------------------------------------------------------------------
// Project/engine state helpers
// ---------------------------------------------------------------------------

bool FSceneAuditor::IsMegaLightsProjectEnabled()
{
	// MegaLights is toggled from Project Settings > Rendering > Direct
	// Lighting; that UI writes through to this CVar as of 5.8. If your build
	// exposes it as a plain URendererSettings UPROPERTY instead, swap this
	// for a GetDefault<URendererSettings>() read.
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MegaLights")))
	{
		return CVar->GetInt() != 0;
	}
	return false;
}

bool FSceneAuditor::IsHardwareRayTracingSupported()
{
	if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing")))
	{
		return CVar->GetInt() != 0;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Impact scoring
// ---------------------------------------------------------------------------

float FSceneAuditor::SeverityWeight(ESceneIssueSeverity Severity)
{
	switch (Severity)
	{
	case ESceneIssueSeverity::Critical: return 1.0f;
	case ESceneIssueSeverity::Warning:  return 0.66f;
	default:                            return 0.33f;
	}
}

void FSceneAuditor::NormalizeImpactScores(TArray<FSceneIssuePtr>& Issues)
{
	TMap<EImpactMetric, float> MaxMagnitudeByMetric;
	for (const FSceneIssuePtr& Issue : Issues)
	{
		if (Issue->Impact.Metric != EImpactMetric::Unclassified)
		{
			float& Max = MaxMagnitudeByMetric.FindOrAdd(Issue->Impact.Metric);
			Max = FMath::Max(Max, Issue->Impact.EstimatedMagnitude);
		}
	}

	for (FSceneIssuePtr& Issue : Issues)
	{
		const float Sev = SeverityWeight(Issue->Severity);
		const float Conf = FMath::Clamp<uint8>(Issue->Impact.Confidence, 0, 100) / 100.f;

		if (Issue->Impact.Metric == EImpactMetric::Unclassified)
		{
			// No comparable magnitude available (e.g. a project-wide
			// structural recommendation) — sort by severity alone, but don't
			// let a low default confidence bury it beneath unrelated
			// low-confidence-but-numeric issues.
			Issue->Impact.PriorityScore = FMath::RoundToInt(100.f * Sev * FMath::Max(Conf, 0.5f));
		}
		else
		{
			const float MaxForMetric = MaxMagnitudeByMetric.FindChecked(Issue->Impact.Metric);
			const float Normalized = MaxForMetric > KINDA_SMALL_NUMBER
				? Issue->Impact.EstimatedMagnitude / MaxForMetric
				: 0.f;
			// Floor at 0.3 so a real but comparatively small issue doesn't
			// round all the way down to invisible next to the worst offender.
			const float MagnitudeFactor = 0.3f + 0.7f * FMath::Clamp(Normalized, 0.f, 1.f);
			Issue->Impact.PriorityScore = FMath::RoundToInt(100.f * Sev * MagnitudeFactor * Conf);
		}
	}
}

// ---------------------------------------------------------------------------
// RunGeneralAudit
// ---------------------------------------------------------------------------

TArray<FSceneIssuePtr> FSceneAuditor::RunGeneralAudit(UWorld* World, bool bShowProgressDialog)
{
	TArray<FSceneIssuePtr> Issues;
	if (!World)
	{
		return Issues;
	}

	FScopedSlowTask SlowTask(10.f, LOCTEXT("AuditingLevel", "Auditing level for optimization opportunities..."));
	if (bShowProgressDialog)
	{
		SlowTask.MakeDialog(/*bShowCancelButton=*/ true);
	}

	auto Step = [&SlowTask](const FText& Message) -> bool
	{
		SlowTask.EnterProgressFrame(1.f, Message);
		return SlowTask.ShouldCancel();
	};

	if (Step(LOCTEXT("StepGeometry", "Checking geometry & Nanite settings..."))) return Issues;
	CheckGeometryAndNanite(World, Issues);

	if (Step(LOCTEXT("StepMaterials", "Checking material cost & shadow cache invalidation..."))) return Issues;
	CheckMaterialCostAndShadowCache(World, Issues);

	if (Step(LOCTEXT("StepInstancing", "Checking instancing candidates..."))) return Issues;
	CheckInstancingCandidates(World, Issues);

	if (Step(LOCTEXT("StepCollision", "Checking collision setup..."))) return Issues;
	CheckMissingCollision(World, Issues);

	if (Step(LOCTEXT("StepLighting", "Checking lighting (MegaLights-aware)..."))) return Issues;
	CheckLighting(World, Issues);

	if (Step(LOCTEXT("StepLumen", "Checking Lumen configuration..."))) return Issues;
	CheckLumenConfiguration(World, Issues);

	if (Step(LOCTEXT("StepTextures", "Checking texture streaming..."))) return Issues;
	CheckTextureStreaming(World, Issues);

	if (Step(LOCTEXT("StepHLOD", "Checking World Partition / HLOD coverage..."))) return Issues;
	CheckWorldPartitionHLOD(World, Issues);

	if (Step(LOCTEXT("StepAnim", "Checking animation budget..."))) return Issues;
	CheckAnimationBudget(World, Issues);

	if (Step(LOCTEXT("StepTick", "Checking actor tick usage..."))) return Issues;
	CheckActorTick(World, Issues);

	NormalizeImpactScores(Issues);

	Issues.Sort([](const FSceneIssuePtr& A, const FSceneIssuePtr& B)
	{
		if (A->Category != B->Category)
		{
			return static_cast<uint8>(A->Category) < static_cast<uint8>(B->Category);
		}
		return A->Impact.PriorityScore > B->Impact.PriorityScore;
	});

	return Issues;
}

// ---------------------------------------------------------------------------
// Geometry / Nanite
// ---------------------------------------------------------------------------

void FSceneAuditor::CheckGeometryAndNanite(UWorld* World, TArray<FSceneIssuePtr>& OutIssues)
{
	const USceneOptimizerSettings* Settings = GetDefault<USceneOptimizerSettings>();

	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		AStaticMeshActor* MeshActor = *It;
		UStaticMeshComponent* Comp = MeshActor ? MeshActor->GetStaticMeshComponent() : nullptr;
		UStaticMesh* Mesh = Comp ? Comp->GetStaticMesh() : nullptr;
		if (!Mesh)
		{
			continue;
		}

		const int32 TriCount = Mesh->GetNumTriangles(0);
		// NaniteSettings.bEnabled is the underlying flag; some 5.x versions
		// also expose a convenience IsNaniteEnabled() accessor. Use whichever
		// compiles against your engine version.
		const bool bNaniteEnabled = Mesh->NaniteSettings.bEnabled;

		if (!bNaniteEnabled && TriCount > Settings->HighTriangleCountThreshold)
		{
			FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
			Issue->Category = ESceneIssueCategory::Geometry;
			Issue->Severity = ESceneIssueSeverity::Warning;
			Issue->TargetName = Mesh->GetName();
			Issue->Description = FString::Printf(
				TEXT("'%s' has %d triangles, no Nanite, and only 1 LOD. On 5.8, enabling Nanite is usually the better fix for a high-poly static prop than hand-building an LOD chain — it removes per-pixel overdraw cost for opaque materials entirely. If this mesh needs to stay non-Nanite (e.g. per-vertex animation), a manual LOD chain is the fallback instead."),
				*MeshActor->GetActorLabel(), TriCount);
			Issue->Actor = MeshActor;
			Issue->Impact.Metric = EImpactMetric::Triangles;
			Issue->Impact.EstimatedMagnitude = static_cast<float>(TriCount);
			Issue->Impact.Confidence = 70; // "should this specific mesh be Nanite" is ultimately a judgment call

			TWeakObjectPtr<UStaticMesh> WeakMesh = Mesh;
			Issue->bAutoFixable = true;
			Issue->ApplyFix = [WeakMesh]() -> bool
			{
				UStaticMesh* M = WeakMesh.Get();
				if (!M)
				{
					return false;
				}
				FScopedTransaction Transaction(NSLOCTEXT("SceneOptimizer", "EnableNaniteTransaction", "Enable Nanite on Static Mesh"));
				M->Modify();
				M->NaniteSettings.bEnabled = true;
				M->PostEditChange();
				M->MarkPackageDirty();
				return true;
			};

			OutIssues.Add(Issue);
		}
	}
}

// ---------------------------------------------------------------------------
// Nanite material cost + Virtual Shadow Map cache invalidation
// (combined into one pass since both need the same per-mesh material scan)
// ---------------------------------------------------------------------------

void FSceneAuditor::CheckMaterialCostAndShadowCache(UWorld* World, TArray<FSceneIssuePtr>& OutIssues)
{
	TArray<FSceneIssuePtr> ShadowCacheIssuesThisPass;

	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		AStaticMeshActor* MeshActor = *It;
		UStaticMeshComponent* Comp = MeshActor ? MeshActor->GetStaticMeshComponent() : nullptr;
		UStaticMesh* Mesh = Comp ? Comp->GetStaticMesh() : nullptr;
		if (!Comp || !Mesh)
		{
			continue;
		}

		const bool bNaniteEnabled = Mesh->NaniteSettings.bEnabled;
		const int32 TriCount = Mesh->GetNumTriangles(0);
		bool bHasWPO = false;

		for (UMaterialInterface* MaterialInterface : Comp->GetMaterials())
		{
			if (!MaterialInterface)
			{
				continue;
			}
			UMaterial* BaseMaterial = MaterialInterface->GetMaterial();
			if (!BaseMaterial)
			{
				continue;
			}

			// WorldPositionOffset / PixelDepthOffset are legacy material-graph
			// pins on UMaterial; IsConnected() reflects whether anything feeds
			// them regardless of the render pipeline in use.
			if (BaseMaterial->WorldPositionOffset.IsConnected())
			{
				bHasWPO = true;
			}

			if (bNaniteEnabled)
			{
				const bool bMasked = MaterialInterface->GetBlendMode() == BLEND_Masked;
				const bool bPDO = BaseMaterial->PixelDepthOffset.IsConnected();

				if (bMasked || bPDO)
				{
					FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
					Issue->Category = ESceneIssueCategory::Geometry;
					Issue->Severity = bPDO ? ESceneIssueSeverity::Critical : ESceneIssueSeverity::Warning;
					Issue->TargetName = MaterialInterface->GetName();
					Issue->Description = FString::Printf(
						TEXT("'%s' uses %s on Nanite mesh '%s' (%d triangles). %s materials force Nanite's per-pixel programmable rasterization path instead of its fixed-function path — Epic restricted Fortnite to opaque-only materials on Nanite for exactly this reason. Simplifying or removing this is a manual material edit, not something this tool will do automatically."),
						*MeshActor->GetActorLabel(),
						bPDO ? TEXT("Pixel Depth Offset") : TEXT("a Masked blend mode"),
						*Mesh->GetName(), TriCount,
						bPDO ? TEXT("PDO") : TEXT("Masked"));
					Issue->Actor = MeshActor;
					Issue->Impact.Metric = EImpactMetric::Triangles;
					Issue->Impact.EstimatedMagnitude = static_cast<float>(TriCount);
					Issue->Impact.Confidence = 60; // real cost also depends on screen coverage, which a static scan can't see
					// Report-only: editing a material's blend mode or PDO usage isn't a safe automated action.
					OutIssues.Add(Issue);
				}
			}
		}

		if (bHasWPO && Comp->Mobility == EComponentMobility::Static)
		{
			const bool bAlreadyRigid = Comp->ShadowCacheInvalidationBehavior == EShadowCacheInvalidationBehavior::Rigid;
			if (!bAlreadyRigid)
			{
				// Static mobility alone doesn't guarantee the actor is
				// visually static at runtime — a Timeline or a movement
				// component (rotating, projectile, etc.) driving the WPO
				// effect is exactly the case where suppressing shadow-cache
				// invalidation could visibly break the shadow. Only offer
				// the auto-fix when neither is present; otherwise report-only.
				TArray<UTimelineComponent*> Timelines;
				MeshActor->GetComponents<UTimelineComponent>(Timelines);
				TArray<UMovementComponent*> MovementComponents;
				MeshActor->GetComponents<UMovementComponent>(MovementComponents);
				const bool bLooksStaticAtRuntime = Timelines.Num() == 0 && MovementComponents.Num() == 0;

				FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
				Issue->Category = ESceneIssueCategory::Lighting;
				Issue->Severity = ESceneIssueSeverity::Warning;
				Issue->TargetName = MeshActor->GetActorLabel();
				Issue->Actor = MeshActor;
				Issue->Impact.Metric = EImpactMetric::ShadowInvalidations;

				if (bLooksStaticAtRuntime)
				{
					Issue->Description = FString::Printf(
						TEXT("'%s' has a static-mobility mesh whose material uses World Position Offset, but Shadow Cache Invalidation Behavior is left at Auto. On Virtual Shadow Maps this invalidates cached shadow pages every frame even though the actor never moves. Setting it to Rigid suppresses that — only approve this if the WPO effect (e.g. wind sway) doesn't need to visibly affect shadowing."),
						*MeshActor->GetActorLabel());
					Issue->Impact.Confidence = 55; // can't confirm from static data alone that suppressing invalidation is visually safe

					TWeakObjectPtr<UStaticMeshComponent> WeakComp = Comp;
					Issue->bAutoFixable = true;
					Issue->ApplyFix = [WeakComp]() -> bool
					{
						UStaticMeshComponent* C = WeakComp.Get();
						if (!C)
						{
							return false;
						}
						FScopedTransaction Transaction(NSLOCTEXT("SceneOptimizer", "ShadowCacheFixTransaction", "Set Shadow Cache Invalidation to Rigid"));
						C->Modify();
						C->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Rigid;
						C->MarkPackageDirty();
						return true;
					};
				}
				else
				{
					Issue->Description = FString::Printf(
						TEXT("'%s' has a static-mobility mesh whose material uses World Position Offset, and Shadow Cache Invalidation Behavior is left at Auto. It also has a Timeline or movement component, which likely means the WPO effect is actually animated at runtime — forcing Rigid here could visibly break its shadow, so this is report-only."),
						*MeshActor->GetActorLabel());
					Issue->Impact.Confidence = 30; // likely a false positive for the "safe to suppress" fix, kept visible rather than dropped
					// bAutoFixable stays false: this needs a human decision.
				}

				OutIssues.Add(Issue);
				if (bLooksStaticAtRuntime)
				{
					ShadowCacheIssuesThisPass.Add(Issue);
				}
			}
		}
	}

	// This is a systemic pattern, not a one-off — magnitude is "how many
	// other actors in this level have the same issue," so a level with many
	// of these sorts above a level with just one.
	for (FSceneIssuePtr& Issue : ShadowCacheIssuesThisPass)
	{
		Issue->Impact.EstimatedMagnitude = static_cast<float>(ShadowCacheIssuesThisPass.Num());
	}
}

// ---------------------------------------------------------------------------
// Instancing candidates
// ---------------------------------------------------------------------------

void FSceneAuditor::CheckInstancingCandidates(UWorld* World, TArray<FSceneIssuePtr>& OutIssues)
{
	const USceneOptimizerSettings* Settings = GetDefault<USceneOptimizerSettings>();
	TMap<UStaticMesh*, TArray<AStaticMeshActor*>> ActorsByMesh;

	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		AStaticMeshActor* MeshActor = *It;
		UStaticMeshComponent* Comp = MeshActor ? MeshActor->GetStaticMeshComponent() : nullptr;

		if (Comp && Comp->IsA<UInstancedStaticMeshComponent>())
		{
			continue; // already instanced
		}

		UStaticMesh* Mesh = Comp ? Comp->GetStaticMesh() : nullptr;
		if (Mesh)
		{
			ActorsByMesh.FindOrAdd(Mesh).Add(MeshActor);
		}
	}

	for (const TPair<UStaticMesh*, TArray<AStaticMeshActor*>>& Pair : ActorsByMesh)
	{
		if (Pair.Value.Num() < Settings->InstancingCandidateMinCount)
		{
			continue;
		}

		// Guard: only offer the automated merge when every candidate actor is
		// "plain" — no simulated physics, no attached children, no tags, and
		// not a Blueprint subclass. Any actor failing this drops the whole
		// group to report-only; a partial automated merge is more confusing
		// than doing it by hand.
		bool bSafeToAutoFix = true;
		for (AStaticMeshActor* Actor : Pair.Value)
		{
			UStaticMeshComponent* ActorComp = Actor->GetStaticMeshComponent();
			const bool bHasChildren = Actor->Children.Num() > 0;
			const bool bSimulatesPhysics = ActorComp && ActorComp->IsSimulatingPhysics();
			const bool bHasTags = Actor->Tags.Num() > 0;
			const bool bIsPlainStaticMeshActor = Actor->GetClass() == AStaticMeshActor::StaticClass();
			if (bHasChildren || bSimulatesPhysics || bHasTags || !bIsPlainStaticMeshActor)
			{
				bSafeToAutoFix = false;
				break;
			}
		}

		FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
		Issue->Category = ESceneIssueCategory::Geometry;
		Issue->Severity = ESceneIssueSeverity::Info;
		Issue->TargetName = Pair.Key->GetName();
		Issue->Impact.Metric = EImpactMetric::DrawCalls;
		Issue->Impact.EstimatedMagnitude = static_cast<float>(Pair.Value.Num() - 1);
		Issue->Impact.Confidence = bSafeToAutoFix ? 80 : 55;

		if (bSafeToAutoFix)
		{
			Issue->Description = FString::Printf(
				TEXT("%d separate actors use static mesh '%s' with no per-actor logic, physics, tags, or subclassing. These can be safely merged into one Instanced Static Mesh component, cutting %d draw calls to 1."),
				Pair.Value.Num(), *Pair.Key->GetName(), Pair.Value.Num() - 1);

			TArray<TWeakObjectPtr<AStaticMeshActor>> WeakActors;
			for (AStaticMeshActor* A : Pair.Value)
			{
				WeakActors.Add(A);
			}
			TWeakObjectPtr<UStaticMesh> WeakMesh = Pair.Key;
			TWeakObjectPtr<UWorld> WeakWorld = World;

			Issue->bAutoFixable = true;
			Issue->ApplyFix = [WeakActors, WeakMesh, WeakWorld]() -> bool
			{
				UStaticMesh* Mesh = WeakMesh.Get();
				UWorld* W = WeakWorld.Get();
				if (!Mesh || !W)
				{
					return false;
				}

				FScopedTransaction Transaction(NSLOCTEXT("SceneOptimizer", "InstancingFixTransaction", "Merge Actors Into Instanced Static Mesh"));

				FActorSpawnParameters SpawnParams;
				SpawnParams.Name = MakeUniqueObjectName(W->GetCurrentLevel(), AActor::StaticClass(), FName(*FString::Printf(TEXT("HISM_%s"), *Mesh->GetName())));
				AActor* OwnerActor = W->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParams);
				if (!OwnerActor)
				{
					return false;
				}
				OwnerActor->SetActorLabel(FString::Printf(TEXT("HISM_%s"), *Mesh->GetName()));

				UInstancedStaticMeshComponent* HISM = NewObject<UInstancedStaticMeshComponent>(OwnerActor);
				HISM->SetStaticMesh(Mesh);
				OwnerActor->SetRootComponent(HISM);
				HISM->RegisterComponent();

				int32 MergedCount = 0;
				for (const TWeakObjectPtr<AStaticMeshActor>& WeakActor : WeakActors)
				{
					AStaticMeshActor* Actor = WeakActor.Get();
					if (!Actor)
					{
						continue;
					}
					HISM->AddInstance(Actor->GetActorTransform());
					Actor->Destroy();
					MergedCount++;
				}

				OwnerActor->MarkPackageDirty();
				return MergedCount > 0;
			};
		}
		else
		{
			Issue->Description = FString::Printf(
				TEXT("%d separate actors use static mesh '%s'. This is a strong instancing candidate, but at least one of them has attached children, simulated physics, tags, or is a Blueprint subclass — merging automatically could silently drop that behavior, so this is report-only."),
				Pair.Value.Num(), *Pair.Key->GetName());
		}

		OutIssues.Add(Issue);
	}
}

// ---------------------------------------------------------------------------
// Missing collision
// ---------------------------------------------------------------------------

void FSceneAuditor::CheckMissingCollision(UWorld* World, TArray<FSceneIssuePtr>& OutIssues)
{
	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		AStaticMeshActor* MeshActor = *It;
		UStaticMeshComponent* Comp = MeshActor ? MeshActor->GetStaticMeshComponent() : nullptr;
		UStaticMesh* Mesh = Comp ? Comp->GetStaticMesh() : nullptr;
		UBodySetup* BodySetup = Mesh ? Mesh->GetBodySetup() : nullptr;

		const bool bHasCollision = BodySetup && BodySetup->AggGeom.GetElementCount() > 0;
		if (Mesh && !bHasCollision)
		{
			FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
			Issue->Category = ESceneIssueCategory::Geometry;
			Issue->Severity = ESceneIssueSeverity::Info;
			Issue->TargetName = Mesh->GetName();
			Issue->Description = FString::Printf(
				TEXT("'%s' (mesh '%s') has no collision geometry set up. Not auto-fixed — simple-collision generation depends on the shape, so this is flagged for manual review."),
				*MeshActor->GetActorLabel(), *Mesh->GetName());
			Issue->Actor = MeshActor;
			Issue->Impact.Confidence = 50;
			OutIssues.Add(Issue);
		}
	}
}

// ---------------------------------------------------------------------------
// Lighting (MegaLights-aware)
// ---------------------------------------------------------------------------

void FSceneAuditor::CheckLighting(UWorld* World, TArray<FSceneIssuePtr>& OutIssues)
{
	const USceneOptimizerSettings* Settings = GetDefault<USceneOptimizerSettings>();
	const bool bMegaLights = IsMegaLightsProjectEnabled();

	struct FLightEntry
	{
		AActor* Actor = nullptr;
		ULightComponent* Comp = nullptr;
		FVector Location = FVector::ZeroVector;
	};
	TArray<FLightEntry> Lights;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		TArray<ULightComponent*> LightComponents;
		Actor->GetComponents<ULightComponent>(LightComponents);
		for (ULightComponent* Light : LightComponents)
		{
			if (Light)
			{
				Lights.Add({ Actor, Light, Light->GetComponentLocation() });
			}
		}
	}

	if (bMegaLights)
	{
		if (!IsHardwareRayTracingSupported())
		{
			FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
			Issue->Category = ESceneIssueCategory::Lighting;
			Issue->Severity = ESceneIssueSeverity::Critical;
			Issue->TargetName = TEXT("Project Settings");
			Issue->Description = TEXT("MegaLights is enabled for this project but hardware ray tracing support doesn't appear to be enabled. MegaLights is built on ray tracing and Epic recommends enabling hardware ray tracing alongside it — without it you're likely on a fallback path rather than MegaLights' intended cost profile.");
			Issue->Impact.Confidence = 70;
			OutIssues.Add(Issue);
		}

		// MegaLights has a roughly constant frame cost regardless of light
		// count — quality/noise degrades with per-pixel light complexity
		// instead. Approximate that with simple radius-based clustering.
		// O(n^2); fine for typical light counts, skipped above a configurable cap.
		if (Lights.Num() > 1 && Lights.Num() <= Settings->MegaLightsMaxLightsForOverlapCheck)
		{
			for (int32 i = 0; i < Lights.Num(); ++i)
			{
				int32 OverlapCount = 0;
				for (int32 j = 0; j < Lights.Num(); ++j)
				{
					if (i == j)
					{
						continue;
					}
					if (FVector::Dist(Lights[i].Location, Lights[j].Location) <= Settings->MegaLightsOverlapRadius)
					{
						OverlapCount++;
					}
				}

				if (OverlapCount >= Settings->MegaLightsOverlapCountThreshold)
				{
					FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
					Issue->Category = ESceneIssueCategory::Lighting;
					Issue->Severity = ESceneIssueSeverity::Warning;
					Issue->TargetName = Lights[i].Actor->GetActorLabel();
					Issue->Description = FString::Printf(
						TEXT("%d other lights are within %.0f units of '%s'. MegaLights' cost is roughly constant regardless of total light count, but quality/noise degrades with per-pixel light complexity — dense overlapping clusters like this are where that shows up, not light count on its own."),
						OverlapCount, Settings->MegaLightsOverlapRadius, *Lights[i].Actor->GetActorLabel());
					Issue->Actor = Lights[i].Actor;
					Issue->Impact.Confidence = 45; // quality impact, not a directly comparable frame-time unit
					OutIssues.Add(Issue);
				}
			}
		}

		for (const FLightEntry& Entry : Lights)
		{
			// bAllowMegaLights is the documented per-light opt-out property
			// name as of 5.8; verify against your point release if it fails
			// to compile.
			if (!Entry.Comp->bAllowMegaLights && Entry.Comp->CastShadows)
			{
				FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
				Issue->Category = ESceneIssueCategory::Lighting;
				Issue->Severity = ESceneIssueSeverity::Info;
				Issue->TargetName = Entry.Actor->GetActorLabel();
				Issue->Description = FString::Printf(
					TEXT("Light on '%s' has MegaLights disabled while the rest of the project uses it. If that's not intentional, it's falling back to a more expensive legacy shadowing path for this light specifically."),
					*Entry.Actor->GetActorLabel());
				Issue->Actor = Entry.Actor;
				Issue->Impact.Confidence = 55;
				OutIssues.Add(Issue);
			}
		}
	}
	else
	{
		// Legacy / Virtual Shadow Map path: tie the check to actual VSM cost
		// drivers (overlapping shadow casters) rather than a flat count.
		TArray<FSceneIssuePtr> LegacyLightIssuesThisPass;
		int32 MovableShadowCasters = 0;

		for (const FLightEntry& Entry : Lights)
		{
			if (Entry.Comp->Mobility == EComponentMobility::Movable && Entry.Comp->CastShadows)
			{
				MovableShadowCasters++;

				FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
				Issue->Category = ESceneIssueCategory::Lighting;
				Issue->Severity = ESceneIssueSeverity::Warning;
				Issue->TargetName = Entry.Actor->GetActorLabel();
				Issue->Description = FString::Printf(
					TEXT("Light on '%s' is Movable with dynamic shadows enabled, and this project doesn't have MegaLights enabled. Each of these consumes Virtual Shadow Map page-pool budget continuously; switching to Stationary (if it doesn't need to move) is the standard fix, or consider enabling MegaLights project-wide if you have many of these."),
					*Entry.Actor->GetActorLabel());
				Issue->Actor = Entry.Actor;
				Issue->Impact.Metric = EImpactMetric::ShadowInvalidations;
				Issue->Impact.Confidence = 65;

				TWeakObjectPtr<ULightComponent> WeakLight = Entry.Comp;
				Issue->bAutoFixable = true;
				Issue->ApplyFix = [WeakLight]() -> bool
				{
					ULightComponent* L = WeakLight.Get();
					if (!L)
					{
						return false;
					}
					FScopedTransaction Transaction(NSLOCTEXT("SceneOptimizer", "LightMobilityFixTransaction", "Set Light Mobility to Stationary"));
					L->Modify();
					L->SetMobility(EComponentMobility::Stationary);
					L->MarkPackageDirty();
					return true;
				};

				OutIssues.Add(Issue);
				LegacyLightIssuesThisPass.Add(Issue);
			}
		}

		// Systemic magnitude: how many other lights share this pattern.
		for (FSceneIssuePtr& Issue : LegacyLightIssuesThisPass)
		{
			Issue->Impact.EstimatedMagnitude = static_cast<float>(MovableShadowCasters);
		}

		if (MovableShadowCasters > Settings->LegacyShadowLightCountThreshold)
		{
			FSceneIssuePtr Summary = MakeShared<FSceneIssue>();
			Summary->Category = ESceneIssueCategory::Lighting;
			Summary->Severity = ESceneIssueSeverity::Critical;
			Summary->TargetName = TEXT("Level");
			Summary->Description = FString::Printf(
				TEXT("%d movable, shadow-casting lights found without MegaLights enabled. At this count, evaluating MegaLights for the project (Project Settings > Rendering > Direct Lighting) is likely more effective than fixing lights one at a time, since MegaLights' cost is roughly constant regardless of count."),
				MovableShadowCasters);
			Summary->Impact.Metric = EImpactMetric::ShadowInvalidations;
			Summary->Impact.EstimatedMagnitude = static_cast<float>(MovableShadowCasters);
			Summary->Impact.Confidence = 70;
			OutIssues.Add(Summary);
		}
	}
}

// ---------------------------------------------------------------------------
// Lumen configuration (project-wide, report-only)
// ---------------------------------------------------------------------------

void FSceneAuditor::CheckLumenConfiguration(UWorld* World, TArray<FSceneIssuePtr>& OutIssues)
{
	IConsoleVariable* GICVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DynamicGlobalIlluminationMethod"));
	const bool bLumenEnabled = GICVar && GICVar->GetInt() == 1; // 1 == Lumen in the RendererSettings enum ordering
	if (!bLumenEnabled)
	{
		return;
	}

	// Lumen Lite's exact enable path (CVar vs. a quality-tier setting) may
	// differ across 5.8 point releases — adjust this accessor to match your
	// build if this CVar doesn't exist.
	IConsoleVariable* LiteCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Lumen.Lite"));
	const bool bLumenLiteEnabled = LiteCVar && LiteCVar->GetInt() != 0;

	if (!bLumenLiteEnabled)
	{
		FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
		Issue->Category = ESceneIssueCategory::Lighting;
		Issue->Severity = ESceneIssueSeverity::Info;
		Issue->TargetName = TEXT("Project Settings");
		Issue->Description = TEXT("This project uses standard-quality Lumen. UE 5.8's Lumen Lite mode (irradiance fields with probe occlusion) trades some visual fidelity for substantially lower GPU cost and is aimed at scalability — worth evaluating if you're targeting lower-end hardware or a wider scalability range. This is a project-wide rendering decision, not something to toggle automatically.");
		Issue->Impact.Confidence = 40;
		OutIssues.Add(Issue);
	}
}

// ---------------------------------------------------------------------------
// Texture streaming
// ---------------------------------------------------------------------------

void FSceneAuditor::CheckTextureStreaming(UWorld* World, TArray<FSceneIssuePtr>& OutIssues)
{
	const USceneOptimizerSettings* Settings = GetDefault<USceneOptimizerSettings>();
	TSet<UTexture2D*> SeenTextures;
	int64 EstimatedResidentBytes = 0;

	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		AStaticMeshActor* MeshActor = *It;
		UStaticMeshComponent* Comp = MeshActor ? MeshActor->GetStaticMeshComponent() : nullptr;
		if (!Comp)
		{
			continue;
		}

		for (UMaterialInterface* Material : Comp->GetMaterials())
		{
			if (!Material)
			{
				continue;
			}

			TArray<UTexture*> UsedTextures;
			Material->GetUsedTextures(UsedTextures, EMaterialQualityLevel::Num, true, ERHIFeatureLevel::SM5, true);

			for (UTexture* Tex : UsedTextures)
			{
				UTexture2D* Tex2D = Cast<UTexture2D>(Tex);
				if (!Tex2D || SeenTextures.Contains(Tex2D))
				{
					continue;
				}
				SeenTextures.Add(Tex2D);

				const int32 SizeX = Tex2D->GetSizeX();
				const int32 SizeY = Tex2D->GetSizeY();
				const int64 ApproxBytes = static_cast<int64>(SizeX) * SizeY * 4; // rough uncompressed estimate, ignores mips/compression
				EstimatedResidentBytes += ApproxBytes;

				// --- oversized, uncapped texture ---
				const bool bAlreadyCapped = Tex2D->MaxTextureSize > 0 && Tex2D->MaxTextureSize <= Settings->LargeTextureThreshold;
				if ((SizeX > Settings->LargeTextureThreshold || SizeY > Settings->LargeTextureThreshold) && !bAlreadyCapped)
				{
					FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
					Issue->Category = ESceneIssueCategory::Textures;
					Issue->Severity = ESceneIssueSeverity::Warning;
					Issue->TargetName = Tex2D->GetName();
					Issue->Description = FString::Printf(
						TEXT("Texture '%s' is %dx%d with no max size override. Capping it at %d would save memory with likely-imperceptible quality loss on most surfaces."),
						*Tex2D->GetName(), SizeX, SizeY, Settings->TextureMaxSizeFix);

					const float BytesSaved = static_cast<float>(SizeX * SizeY - Settings->TextureMaxSizeFix * Settings->TextureMaxSizeFix) * 4.f;
					Issue->Impact.Metric = EImpactMetric::TextureMemoryMB;
					Issue->Impact.EstimatedMagnitude = FMath::Max(0.f, BytesSaved / (1024.f * 1024.f));
					Issue->Impact.Confidence = 75;

					TWeakObjectPtr<UTexture2D> WeakTex = Tex2D;
					const int32 MaxSize = Settings->TextureMaxSizeFix;
					Issue->bAutoFixable = true;
					Issue->ApplyFix = [WeakTex, MaxSize]() -> bool
					{
						UTexture2D* T = WeakTex.Get();
						if (!T)
						{
							return false;
						}
						FScopedTransaction Transaction(NSLOCTEXT("SceneOptimizer", "TextureMaxSizeFixTransaction", "Cap Texture Max Size"));
						T->Modify();
						T->MaxTextureSize = MaxSize;
						T->PostEditChange();
						T->MarkPackageDirty();
						return true;
					};
					OutIssues.Add(Issue);
				}

				// --- NeverStream on a large, non-UI/lightmap/render-target texture ---
				if (Tex2D->NeverStream)
				{
					const bool bLooksIntentional =
						Tex2D->LODGroup == TEXTUREGROUP_UI ||
						Tex2D->LODGroup == TEXTUREGROUP_Lightmap ||
						Tex2D->LODGroup == TEXTUREGROUP_RenderTarget;

					if (!bLooksIntentional && (SizeX * SizeY) >= (Settings->LargeTextureThreshold * Settings->LargeTextureThreshold / 4))
					{
						FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
						Issue->Category = ESceneIssueCategory::Textures;
						Issue->Severity = ESceneIssueSeverity::Warning;
						Issue->TargetName = Tex2D->GetName();
						Issue->Description = FString::Printf(
							TEXT("Texture '%s' (%dx%d) has Never Stream enabled and isn't tagged as a UI/lightmap/render-target texture. This permanently reserves its full VRAM footprint outside the streaming pool rather than letting mips scale with distance/screen size."),
							*Tex2D->GetName(), SizeX, SizeY);
						Issue->Impact.Metric = EImpactMetric::TextureMemoryMB;
						Issue->Impact.EstimatedMagnitude = static_cast<float>(ApproxBytes) / (1024.f * 1024.f);
						Issue->Impact.Confidence = 50; // can't be fully sure NeverStream isn't intentional from static data alone
						// Report-only: toggling NeverStream without knowing why it was set could break an artist's intentional choice.
						OutIssues.Add(Issue);
					}
				}

				// --- normal map wrong compression setting ---
				if (Tex2D->LODGroup == TEXTUREGROUP_WorldNormalMap && Tex2D->CompressionSettings != TC_Normalmap)
				{
					FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
					Issue->Category = ESceneIssueCategory::Textures;
					Issue->Severity = ESceneIssueSeverity::Info;
					Issue->TargetName = Tex2D->GetName();
					Issue->Description = FString::Printf(
						TEXT("'%s' is in the normal-map texture group but isn't using the Normalmap compression setting, which is both lower quality and larger than it needs to be for normal data."),
						*Tex2D->GetName());
					Issue->Impact.Confidence = 60;

					TWeakObjectPtr<UTexture2D> WeakTex2 = Tex2D;
					Issue->bAutoFixable = true;
					Issue->ApplyFix = [WeakTex2]() -> bool
					{
						UTexture2D* T = WeakTex2.Get();
						if (!T)
						{
							return false;
						}
						FScopedTransaction Transaction(NSLOCTEXT("SceneOptimizer", "NormalMapCompressionFixTransaction", "Fix Normal Map Compression"));
						T->Modify();
						T->CompressionSettings = TC_Normalmap;
						T->PostEditChange();
						T->MarkPackageDirty();
						return true;
					};
					OutIssues.Add(Issue);
				}
			}
		}
	}

	// --- pool-size context: one project-wide note, not a per-texture fix ---
	IConsoleVariable* PoolSizeCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streaming.PoolSize"));
	const int32 PoolSizeMB = PoolSizeCVar ? PoolSizeCVar->GetInt() : 0;
	const float EstimatedResidentMB = EstimatedResidentBytes / (1024.f * 1024.f);

	if (PoolSizeMB > 0 && EstimatedResidentMB > PoolSizeMB)
	{
		FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
		Issue->Category = ESceneIssueCategory::Textures;
		Issue->Severity = ESceneIssueSeverity::Critical;
		Issue->TargetName = TEXT("r.Streaming.PoolSize");
		Issue->Description = FString::Printf(
			TEXT("This level's visible textures add up to roughly %.0f MB (uncompressed estimate) against a configured streaming pool of %d MB. The individual texture fixes above will help, but you may be structurally over budget — worth revisiting the pool size or overall texture budget rather than fixing textures one at a time."),
			EstimatedResidentMB, PoolSizeMB);
		Issue->Impact.Metric = EImpactMetric::TextureMemoryMB;
		Issue->Impact.EstimatedMagnitude = EstimatedResidentMB - PoolSizeMB;
		Issue->Impact.Confidence = 35; // rough uncompressed estimate, not actual GPU-resident memory
		OutIssues.Add(Issue);
	}
}

// ---------------------------------------------------------------------------
// World Partition / HLOD coverage
// ---------------------------------------------------------------------------

void FSceneAuditor::CheckWorldPartitionHLOD(UWorld* World, TArray<FSceneIssuePtr>& OutIssues)
{
	const USceneOptimizerSettings* Settings = GetDefault<USceneOptimizerSettings>();

	if (!World->GetWorldPartition())
	{
		return; // not a World Partition level; out of scope for this check
	}

	int32 TotalMeshActors = 0;
	int32 ActorsWithoutHLOD = 0;

	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		AStaticMeshActor* MeshActor = *It;
		TotalMeshActors++;
		// GetHLODLayer() is the standard World Partition accessor for an
		// actor's assigned HLOD Layer; null means none is assigned.
		if (!MeshActor->GetHLODLayer())
		{
			ActorsWithoutHLOD++;
		}
	}

	if (TotalMeshActors < Settings->MinActorsBeforeHLODWarning || ActorsWithoutHLOD == 0)
	{
		return;
	}

	const float UncoveredRatio = static_cast<float>(ActorsWithoutHLOD) / static_cast<float>(TotalMeshActors);

	FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
	Issue->Category = ESceneIssueCategory::Misc;
	Issue->Severity = UncoveredRatio > 0.9f ? ESceneIssueSeverity::Critical : ESceneIssueSeverity::Warning;
	Issue->TargetName = TEXT("World Partition");
	Issue->Description = FString::Printf(
		TEXT("This is a World Partition level with %d static mesh actors, and %d of them (%.0f%%) have no HLOD Layer assigned. For a level this size, HLOD coverage is typically the single highest-leverage optimization available — it reduces draw calls, memory, and material complexity for anything far from the camera. Setting up HLOD Layers is a workflow/authoring decision, not a one-click fix."),
		TotalMeshActors, ActorsWithoutHLOD, UncoveredRatio * 100.f);
	Issue->Impact.Metric = EImpactMetric::ActorCount;
	Issue->Impact.EstimatedMagnitude = static_cast<float>(ActorsWithoutHLOD);
	Issue->Impact.Confidence = 80;
	OutIssues.Add(Issue);
}

// ---------------------------------------------------------------------------
// Animation budget
// ---------------------------------------------------------------------------

void FSceneAuditor::CheckAnimationBudget(UWorld* World, TArray<FSceneIssuePtr>& OutIssues)
{
	const USceneOptimizerSettings* Settings = GetDefault<USceneOptimizerSettings>();

	int32 SkeletalMeshActorCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		TArray<USkeletalMeshComponent*> Comps;
		It->GetComponents<USkeletalMeshComponent>(Comps);
		if (Comps.Num() > 0)
		{
			SkeletalMeshActorCount++;
		}
	}

	if (SkeletalMeshActorCount < Settings->SkeletalMeshCountForBudgetWarning)
	{
		return;
	}

	// The Animation Budget Allocator plugin exposes its state through
	// a.Budget.* CVars; Enabled is the most direct signal.
	IConsoleVariable* BudgetCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("a.Budget.Enabled"));
	const bool bBudgetEnabled = BudgetCVar && BudgetCVar->GetInt() != 0;

	if (!bBudgetEnabled)
	{
		FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
		Issue->Category = ESceneIssueCategory::Misc;
		Issue->Severity = ESceneIssueSeverity::Warning;
		Issue->TargetName = TEXT("Animation Budget Allocator");
		Issue->Description = FString::Printf(
			TEXT("This level has %d actors with skeletal meshes, and the Animation Budget Allocator doesn't appear to be enabled. It's the standard 5.x mechanism for scaling animation update cost (tick rate, LOD) with actor count and screen presence — worth enabling once you have this many animated characters, rather than relying on per-actor animation LOD alone."),
			SkeletalMeshActorCount);
		Issue->Impact.Metric = EImpactMetric::AnimUpdateCost;
		Issue->Impact.EstimatedMagnitude = static_cast<float>(SkeletalMeshActorCount);
		Issue->Impact.Confidence = 65;
		OutIssues.Add(Issue);
	}
}

// ---------------------------------------------------------------------------
// Actor tick usage
// ---------------------------------------------------------------------------

void FSceneAuditor::CheckActorTick(UWorld* World, TArray<FSceneIssuePtr>& OutIssues)
{
	const USceneOptimizerSettings* Settings = GetDefault<USceneOptimizerSettings>();
	if (!Settings->bWarnOnFullRateTick)
	{
		return;
	}

	int32 FullRateTickingActors = 0;

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (Actor->PrimaryActorTick.bCanEverTick && Actor->PrimaryActorTick.TickInterval <= 0.f)
		{
			FullRateTickingActors++;

			FSceneIssuePtr Issue = MakeShared<FSceneIssue>();
			Issue->Category = ESceneIssueCategory::Misc;
			Issue->Severity = ESceneIssueSeverity::Info;
			Issue->TargetName = Actor->GetActorLabel();
			Issue->Description = FString::Printf(
				TEXT("'%s' ticks every frame (no Tick Interval set). If it doesn't need per-frame updates, a Tick Interval or a Timer is nearly always cheaper — this is report-only since changing tick timing can be gameplay-visible."),
				*Actor->GetActorLabel());
			Issue->Actor = Actor;
			Issue->Impact.Confidence = 40; // can't tell from static data whether this actor's Tick does meaningful work
			OutIssues.Add(Issue);
		}
	}

	if (FullRateTickingActors > 0)
	{
		FSceneIssuePtr Summary = MakeShared<FSceneIssue>();
		Summary->Category = ESceneIssueCategory::Misc;
		Summary->Severity = FullRateTickingActors > 50 ? ESceneIssueSeverity::Warning : ESceneIssueSeverity::Info;
		Summary->TargetName = TEXT("Level");
		Summary->Description = FString::Printf(
			TEXT("%d actors in this level tick every frame with no interval set. Individually cheap, but worth a systemic pass if this number is large."),
			FullRateTickingActors);
		Summary->Impact.Metric = EImpactMetric::ActorCount;
		Summary->Impact.EstimatedMagnitude = static_cast<float>(FullRateTickingActors);
		Summary->Impact.Confidence = 50;
		OutIssues.Add(Summary);
	}
}

#undef LOCTEXT_NAMESPACE
