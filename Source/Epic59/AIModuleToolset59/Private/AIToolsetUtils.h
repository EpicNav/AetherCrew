// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"

// Shared helpers for the AIModuleToolset toolsets. Group functionality by concern in a nested
// namespace (Graph for graph node/pin utilities); add further sub-namespaces as shared needs grow.
namespace UE::AIModuleToolset::Graph
{
	/** A class is placeable when it is a concrete subclass of the expected base; spawning an
	 *  abstract class would crash in NewObject, so the Add tools gate on this. */
	inline bool IsPlaceableClass(const UClass* NodeClass, const UClass* BaseClass)
	{
		return NodeClass && BaseClass && NodeClass->IsChildOf(BaseClass)
			&& !NodeClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
	}

	/** First pin on Node matching Dir, or nullptr. */
	inline UEdGraphPin* FindPin(UEdGraphNode* Node, EEdGraphPinDirection Dir)
	{
		if (!Node)
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Dir)
			{
				return Pin;
			}
		}
		return nullptr;
	}
}
