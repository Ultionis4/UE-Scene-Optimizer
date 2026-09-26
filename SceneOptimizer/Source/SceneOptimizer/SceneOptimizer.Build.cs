using UnrealBuildTool;

public class SceneOptimizer : ModuleRules
{
	public SceneOptimizer(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"InputCore",
			"DeveloperSettings"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Projects",
			"UnrealEd",
			"EditorStyle",
			"LevelEditor",
			"ToolMenus",
			"EditorSubsystem",
			"RenderCore",
			"RHI",
			"Json",
			"JsonUtilities",
			"DesktopPlatform"
		});

		// Note: module names (esp. "EditorStyle" vs "AppStyle" usage in code) can shift
		// slightly between 5.0 - 5.8+. If you hit a compile error about FEditorStyle /
		// FAppStyle, check the Epic release notes for your exact engine version — the
		// fix is almost always a one-line rename, not a structural change.
	}
}
