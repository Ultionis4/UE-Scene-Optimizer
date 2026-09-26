#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "SceneOptimizerCommandlet.generated.h"

/**
 * Headless entry point for CI: runs the same audit as the editor UI against
 * a given map and fails the build (non-zero exit code) if any Critical
 * issues are found.
 *
 * Usage:
 *   UnrealEditor-Cmd.exe YourProject.uproject -run=SceneOptimizerAudit
 *       -Map=/Game/Maps/YourLevel -Out=Saved/SceneOptimizerReport.json
 *
 * -Out is optional; when given, the full issue list is written as JSON
 * (see FSceneOptimizerExport) so CI can archive it as a build artifact.
 */
UCLASS()
class SCENEOPTIMIZER_API USceneOptimizerAuditCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
