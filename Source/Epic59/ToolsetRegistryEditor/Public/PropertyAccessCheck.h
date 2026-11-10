// Copyright Epic Games, Inc. All Rights Reserved.

// UE 5.8: UE::PropertyEditor::IsClassAllowed from UE 5.9's PropertyEditor/Public/PropertyAccessCheck.h, which
// 5.8 does not have. Copied into the compat ToolsetRegistryEditor module (exported from it, not from
// PropertyEditor). 5.9's property gates (CheckCanSetProperty, CheckCanGetProperty, CanReferenceAsset,
// ShouldEnforceEditorPropertyAccess) are not here; Tools/Epic59/port_toolset.py flags their use.

#pragma once

#include "HAL/Platform.h"

class UClass;

namespace UE::PropertyEditor
{
	/**
	 * Whether Class passes the global Class Viewer filter, the same one the editor's class pickers consult.
	 * Permissive when no filter is registered; an invalid Class is never allowed.
	 */
	TOOLSETREGISTRYEDITOR_API bool IsClassAllowed(const UClass* Class);
}
