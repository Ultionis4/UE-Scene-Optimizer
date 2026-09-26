#pragma once

#include "CoreMinimal.h"

class AActor;

/** Broad grouping used to walk the review flow one category at a time. */
enum class ESceneIssueCategory : uint8
{
	Geometry,
	Lighting,
	Textures,
	Misc
};

enum class ESceneIssueSeverity : uint8
{
	Info,
	Warning,
	Critical
};

/**
 * What unit an issue's estimated impact is expressed in. Kept coarse and
 * heuristic on purpose — these are static-analysis proxies for cost, not
 * profiler measurements, and issues are only ever compared within the same
 * metric after normalization (see FSceneAuditor::NormalizeImpactScores).
 */
enum class EImpactMetric : uint8
{
	DrawCalls,
	Triangles,
	TextureMemoryMB,
	ShadowInvalidations,
	AnimUpdateCost,
	ActorCount,
	Unclassified
};

inline const TCHAR* LexToString(ESceneIssueCategory Category)
{
	switch (Category)
	{
	case ESceneIssueCategory::Geometry: return TEXT("Geometry");
	case ESceneIssueCategory::Lighting: return TEXT("Lighting");
	case ESceneIssueCategory::Textures: return TEXT("Textures");
	default: return TEXT("Misc");
	}
}

inline const TCHAR* LexToString(ESceneIssueSeverity Severity)
{
	switch (Severity)
	{
	case ESceneIssueSeverity::Critical: return TEXT("Critical");
	case ESceneIssueSeverity::Warning: return TEXT("Warning");
	default: return TEXT("Info");
	}
}

/**
 * Estimated effectiveness of fixing a given issue, derived only from data the
 * audit already gathered while scanning (never a separate profiling pass).
 * Always surfaced to the user as an estimate, never as a measured number.
 */
struct FSceneIssueImpact
{
	EImpactMetric Metric = EImpactMetric::Unclassified;

	/** In the metric's natural unit (triangles, MB, draw calls, etc.). */
	float EstimatedMagnitude = 0.f;

	/**
	 * 0-100. Lower for anything that depends on runtime behavior the static
	 * scan can't see (e.g. "is this actually static at runtime?"), higher
	 * for things directly measurable from asset/actor data.
	 */
	uint8 Confidence = 50;

	/**
	 * 0-100, normalized across this audit run so issues can be sorted within
	 * a category regardless of metric type. Populated by
	 * FSceneAuditor::NormalizeImpactScores after all checks have run — do not
	 * set this directly from an individual Check* function.
	 */
	int32 PriorityScore = 0;
};

/**
 * A single detected optimization opportunity in the currently open level.
 * Created by FSceneAuditor, consumed by SSceneOptimizerWindow.
 */
struct FSceneIssue
{
	ESceneIssueCategory Category = ESceneIssueCategory::Misc;
	ESceneIssueSeverity Severity = ESceneIssueSeverity::Warning;

	/** Shown in the report list and the review panel. */
	FString Description;

	/** Actor label / asset name shown alongside the description. */
	FString TargetName;

	/** Weak so a deleted actor between audit and review doesn't crash us. */
	TWeakObjectPtr<AActor> Actor;

	/** True if ApplyFix is bound and safe to run without extra context. */
	bool bAutoFixable = false;

	/** Runs only when the user clicks Approve in the review panel. Returns success. */
	TFunction<bool()> ApplyFix;

	/** True once the user has approved and ApplyFix has run (for the report view). */
	bool bResolved = false;

	/** Estimated effectiveness of fixing this issue, see FSceneIssueImpact. */
	FSceneIssueImpact Impact;
};

using FSceneIssuePtr = TSharedPtr<FSceneIssue>;
