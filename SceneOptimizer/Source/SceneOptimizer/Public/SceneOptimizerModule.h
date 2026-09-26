#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FSceneOptimizerModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static const FName TabId;

private:
	TSharedRef<class SDockTab> OnSpawnTab(const class FSpawnTabArgs& Args);
	void RegisterMenus();
};
