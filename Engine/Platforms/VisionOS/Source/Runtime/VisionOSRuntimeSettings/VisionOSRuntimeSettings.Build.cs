// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class VisionOSRuntimeSettings : ModuleRules
{
	public VisionOSRuntimeSettings(ReadOnlyTargetRules Target) : base(Target)
	{
        PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
			}
		);
	}
}
