#include "SceneOptimizerModule.h"

#include "SceneOptimizerWindow.h"
#include "Widgets/Docking/SDockTab.h"
#include "ToolMenus.h"
#include "LevelEditor.h"

#define LOCTEXT_NAMESPACE "FSceneOptimizerModule"

const FName FSceneOptimizerModule::TabId(TEXT("SceneOptimizerTab"));

void FSceneOptimizerModule::StartupModule()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		TabId,
		FOnSpawnTab::CreateRaw(this, &FSceneOptimizerModule::OnSpawnTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Scene Optimizer"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Audit the open level and review optimization fixes"))
		.SetMenuType(ETabSpawnerMenuType::Hidden); // we add our own menu entry below

	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FSceneOptimizerModule::RegisterMenus));
}

void FSceneOptimizerModule::ShutdownModule()
{
	if (FGlobalTabmanager::Get().IsValid())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabId);
	}
	UToolMenus::UnRegisterStartupCallback(this);
}

void FSceneOptimizerModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
	FToolMenuSection& Section = Menu->FindOrAddSection("SceneOptimizer");

	Section.AddMenuEntry(
		"OpenSceneOptimizer",
		LOCTEXT("MenuLabel", "Scene Optimizer"),
		LOCTEXT("MenuTooltip", "Open the scene optimization audit tool"),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateLambda([]()
		{
			FGlobalTabmanager::Get()->TryInvokeTab(FSceneOptimizerModule::TabId);
		}))
	);
}

TSharedRef<SDockTab> FSceneOptimizerModule::OnSpawnTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SSceneOptimizerWindow)
		];
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FSceneOptimizerModule, SceneOptimizer)
