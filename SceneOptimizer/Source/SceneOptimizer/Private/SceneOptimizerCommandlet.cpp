#include "SceneOptimizerCommandlet.h"

#include "SceneAuditor.h"
#include "SceneOptimizerExport.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Misc/Parse.h"

int32 USceneOptimizerAuditCommandlet::Main(const FString& Params)
{
	FString MapName;
	if (!FParse::Value(*Params, TEXT("Map="), MapName) || MapName.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("SceneOptimizerAudit: pass -Map=<PackagePath>, e.g. -Map=/Game/Maps/MyLevel"));
		return 1;
	}

	UPackage* MapPackage = LoadPackage(nullptr, *MapName, LOAD_None);
	if (!MapPackage)
	{
		UE_LOG(LogTemp, Error, TEXT("SceneOptimizerAudit: failed to load map package '%s'"), *MapName);
		return 1;
	}

	UWorld* World = UWorld::FindWorldInPackage(MapPackage);
	if (!World)
	{
		UE_LOG(LogTemp, Error, TEXT("SceneOptimizerAudit: no UWorld found in package '%s'"), *MapName);
		return 1;
	}

	// No progress dialog in a headless run — there's no editor UI to attach it to.
	TArray<FSceneIssuePtr> Issues = FSceneAuditor::RunGeneralAudit(World, /*bShowProgressDialog=*/ false);

	int32 CriticalCount = 0;
	int32 WarningCount = 0;
	for (const FSceneIssuePtr& Issue : Issues)
	{
		if (Issue->Severity == ESceneIssueSeverity::Critical) CriticalCount++;
		else if (Issue->Severity == ESceneIssueSeverity::Warning) WarningCount++;
	}

	UE_LOG(LogTemp, Display, TEXT("SceneOptimizerAudit: %d issue(s) found (%d Critical, %d Warning) in '%s'."),
		Issues.Num(), CriticalCount, WarningCount, *MapName);

	FString OutputPath;
	if (FParse::Value(*Params, TEXT("Out="), OutputPath) && !OutputPath.IsEmpty())
	{
		if (FSceneOptimizerExport::ExportToJson(Issues, OutputPath))
		{
			UE_LOG(LogTemp, Display, TEXT("SceneOptimizerAudit: wrote report to '%s'"), *OutputPath);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("SceneOptimizerAudit: failed to write report to '%s'"), *OutputPath);
		}
	}

	return CriticalCount > 0 ? 1 : 0;
}
