// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Expressions/TG_Expression.h"
#include "TextureGraph.h"
#include "TextureGraphTools.h"
#include "TG_Node.h"
#include "Templates/SubclassOf.h"
#include "UObject/Package.h"

namespace TextureGraphToolsetTest
{
	// Expression class paths shared across all specs.
	constexpr const TCHAR* OutputClassPath = TEXT("/Script/TextureGraph.TG_Expression_Output");
	constexpr const TCHAR* ScalarClassPath = TEXT("/Script/TextureGraph.TG_Expression_Scalar");
	constexpr const TCHAR* ColorClassPath  = TEXT("/Script/TextureGraph.TG_Expression_Color");
	constexpr const TCHAR* VectorClassPath = TEXT("/Script/TextureGraph.TG_Expression_Vector");
	constexpr const TCHAR* NoiseClassPath  = TEXT("/Script/TextureGraph.TG_Expression_Noise");
	constexpr const TCHAR* AddClassPath    = TEXT("/Script/TextureGraph.TG_Expression_Add");
	constexpr const TCHAR* MADClassPath    = TEXT("/Script/TextureGraph.TG_Expression_MAD");

	/** Resolves an expression class path to a typed class for the AICallable signatures. */
	inline TSubclassOf<UTG_Expression> ExprClass(const TCHAR* ClassPath)
	{
		return LoadClass<UTG_Expression>(nullptr, ClassPath);
	}

	/** Adds an Output node to a test graph via the toolset (AddNode rejects Output classes). */
	inline UTG_Node* AddOutputNode(UTextureGraph* Graph, FName Name = NAME_None)
	{
		return UTextureGraphTools::AddOutput(Graph, Name);
	}

	/** Adds an input-parameter node of the given class path via the toolset (AddNode rejects them). */
	inline UTG_Node* AddInputParamNode(UTextureGraph* Graph, const TCHAR* ClassPath, FName Name = NAME_None)
	{
		return UTextureGraphTools::AddInputParam(Graph, ExprClass(ClassPath), Name);
	}

	const auto Flags =
		EAutomationTestFlags::EditorContext |
		EAutomationTestFlags::ProductFilter |
		EAutomationTestFlags::CriticalPriority;

	/** Creates a TextureGraph in the transient package (not saved to disk). */
	inline UTextureGraph* MakeTransientGraph(const FString& Name = TEXT("TransientGraph"))
	{
		UTextureGraph* Graph = NewObject<UTextureGraph>(
			GetTransientPackage(), UTextureGraph::StaticClass(), FName(*Name));
		Graph->Construct(Name);
		return Graph;
	}

	/** Creates a TextureGraphInstance in the transient package (not saved to disk). */
	inline UTextureGraphInstance* MakeTransientInstance(
		UTextureGraphBase* Parent, const FString& Name = TEXT("TransientInstance"))
	{
		UTextureGraphInstance* Instance = NewObject<UTextureGraphInstance>(
			GetTransientPackage(), UTextureGraphInstance::StaticClass(), FName(*Name));
		Instance->Construct(Name);
		Instance->SetParent(Parent);
		return Instance;
	}

}  // namespace TextureGraphToolsetTest

#endif  // WITH_DEV_AUTOMATION_TESTS
