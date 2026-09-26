#pragma once

#include "CoreMinimal.h"
#include "SceneIssue.h"

/**
 * Writes an audit result out to disk so it can be tracked over time or fed
 * into CI. Read-only with respect to the level — these never touch the
 * level itself, only the FSceneIssue list already produced by an audit.
 */
class FSceneOptimizerExport
{
public:
	static bool ExportToJson(const TArray<FSceneIssuePtr>& Issues, const FString& FilePath);
	static bool ExportToCsv(const TArray<FSceneIssuePtr>& Issues, const FString& FilePath);

private:
	static FString ImpactMetricToString(EImpactMetric Metric);
};
