using UnrealBuildTool;

public class CrateRunTarget : TargetRules
{
	public CrateRunTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V5;
		ExtraModuleNames.Add("CrateRun");
	}
}
