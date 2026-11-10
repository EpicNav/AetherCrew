// Copyright Epic Games, Inc. All Rights Reserved.

#include "AbilitySystemInspectorToolsetTest59.h"

#include "AbilitySystemInspectorToolset59.h"
#include "AbilitySystemComponent.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

BEGIN_DEFINE_SPEC(FAbilitySystemInspectorToolsetSpec59, // UE 5.8: renamed ...59; 5.8 registers a test of the 5.9 name
	"AIAssistant.Toolsets.GASToolsets.AbilitySystemInspectorToolset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
	AActor* TestActor = nullptr;
END_DEFINE_SPEC(FAbilitySystemInspectorToolsetSpec59)

void FAbilitySystemInspectorToolsetSpec59::Define()
{
	// -------------------------------------------------------------------------
	// Null actor: all tools must return empty.
	// -------------------------------------------------------------------------
	Describe(TEXT("With a null actor"), [this]()
	{
		It(TEXT("GetAttributeValues returns empty"), [this]()
		{
			TArray<FRuntimeAttributeValue59> Values =
				UAbilitySystemInspectorToolset59::GetAttributeValues(nullptr);
			TestEqual(TEXT("Empty"), Values.Num(), 0);
		});

		It(TEXT("GetActiveEffects returns empty"), [this]()
		{
			TArray<FActiveEffectInfo59> Effects =
				UAbilitySystemInspectorToolset59::GetActiveEffects(nullptr);
			TestEqual(TEXT("Empty"), Effects.Num(), 0);
		});

		It(TEXT("GetGrantedAbilities returns empty"), [this]()
		{
			TArray<FGrantedAbilityInfo59> Abilities =
				UAbilitySystemInspectorToolset59::GetGrantedAbilities(nullptr);
			TestEqual(TEXT("Empty"), Abilities.Num(), 0);
		});

		It(TEXT("GetActiveTags returns empty"), [this]()
		{
			TArray<FString> Tags = UAbilitySystemInspectorToolset59::GetActiveTags(nullptr);
			TestEqual(TEXT("Empty"), Tags.Num(), 0);
		});
	});

	// -------------------------------------------------------------------------
	// Actor with no ASC: all tools must return empty.
	// -------------------------------------------------------------------------
	Describe(TEXT("With an actor that has no AbilitySystemComponent"), [this]()
	{
		BeforeEach([this]()
		{
			if (!GEditor) return;

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) return;

			FActorSpawnParameters Params;
			Params.ObjectFlags = RF_Transient;
			TestActor = World->SpawnActor<AActor>(AActor::StaticClass(),
				FTransform::Identity, Params);
		});

		AfterEach([this]()
		{
			if (TestActor)
			{
				TestActor->Destroy();
				TestActor = nullptr;
			}
		});

		It(TEXT("GetAttributeValues returns empty"), [this]()
		{
			if (!TestNotNull(TEXT("TestActor"), TestActor)) return;
			TArray<FRuntimeAttributeValue59> Values =
				UAbilitySystemInspectorToolset59::GetAttributeValues(TestActor);
			TestEqual(TEXT("Empty"), Values.Num(), 0);
		});

		It(TEXT("GetActiveEffects returns empty"), [this]()
		{
			if (!TestNotNull(TEXT("TestActor"), TestActor)) return;
			TArray<FActiveEffectInfo59> Effects =
				UAbilitySystemInspectorToolset59::GetActiveEffects(TestActor);
			TestEqual(TEXT("Empty"), Effects.Num(), 0);
		});

		It(TEXT("GetGrantedAbilities returns empty"), [this]()
		{
			if (!TestNotNull(TEXT("TestActor"), TestActor)) return;
			TArray<FGrantedAbilityInfo59> Abilities =
				UAbilitySystemInspectorToolset59::GetGrantedAbilities(TestActor);
			TestEqual(TEXT("Empty"), Abilities.Num(), 0);
		});

		It(TEXT("GetActiveTags returns empty"), [this]()
		{
			if (!TestNotNull(TEXT("TestActor"), TestActor)) return;
			TArray<FString> Tags = UAbilitySystemInspectorToolset59::GetActiveTags(TestActor);
			TestEqual(TEXT("Empty"), Tags.Num(), 0);
		});
	});

	// -------------------------------------------------------------------------
	// Actor with a live ASC and the test attribute set registered.
	// -------------------------------------------------------------------------
	Describe(TEXT("With an actor that has an AbilitySystemComponent"), [this]()
	{
		BeforeEach([this]()
		{
			if (!GEditor) return;

			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World) return;

			FActorSpawnParameters Params;
			Params.ObjectFlags = RF_Transient;
			TestActor = World->SpawnActor<AGASToolsetsTestActor59>(
				AGASToolsetsTestActor59::StaticClass(), FTransform::Identity, Params);

			if (AGASToolsetsTestActor59* GASActor = Cast<AGASToolsetsTestActor59>(TestActor))
			{
				GASActor->AbilitySystemComponent->InitStats(
					UGASToolsetsTestAttributeSet59::StaticClass(), nullptr);
			}
		});

		AfterEach([this]()
		{
			if (TestActor)
			{
				TestActor->Destroy();
				TestActor = nullptr;
			}
		});

		// --- GetAttributeValues --------------------------------------------------

		It(TEXT("GetAttributeValues returns one entry per attribute"), [this]()
		{
			if (!TestNotNull(TEXT("TestActor"), TestActor)) return;
			TArray<FRuntimeAttributeValue59> Values =
				UAbilitySystemInspectorToolset59::GetAttributeValues(TestActor);
			TestEqual(TEXT("Two attributes"), Values.Num(), 2);
		});

		It(TEXT("GetAttributeValues includes Health and MaxHealth"), [this]()
		{
			if (!TestNotNull(TEXT("TestActor"), TestActor)) return;
			TArray<FRuntimeAttributeValue59> Values =
				UAbilitySystemInspectorToolset59::GetAttributeValues(TestActor);

			const bool bHasHealth = Values.ContainsByPredicate(
				[](const FRuntimeAttributeValue59& V)
				{
					return V.AttributeName == TEXT("Health");
				});
			const bool bHasMaxHealth = Values.ContainsByPredicate(
				[](const FRuntimeAttributeValue59& V)
				{
					return V.AttributeName == TEXT("MaxHealth");
				});

			TestTrue(TEXT("Has Health"), bHasHealth);
			TestTrue(TEXT("Has MaxHealth"), bHasMaxHealth);
		});

		It(TEXT("GetAttributeValues populates SetClassName and FullName"), [this]()
		{
			if (!TestNotNull(TEXT("TestActor"), TestActor)) return;
			TArray<FRuntimeAttributeValue59> Values =
				UAbilitySystemInspectorToolset59::GetAttributeValues(TestActor);
			if (!TestTrue(TEXT("Has values"), Values.Num() > 0)) return;

			for (const FRuntimeAttributeValue59& Value : Values)
			{
				TestEqual(TEXT("SetClassName"), Value.SetClassName,
					TEXT("GASToolsetsTestAttributeSet59")); // UE 5.8: the test set is renamed ...59
				TestEqual(TEXT("FullName"),
					Value.FullName,
					FString::Printf(TEXT("GASToolsetsTestAttributeSet59.%s"), *Value.AttributeName)); // UE 5.8: renamed ...59
			}
		});

		It(TEXT("GetAttributeValues results are sorted by FullName"), [this]()
		{
			if (!TestNotNull(TEXT("TestActor"), TestActor)) return;
			TArray<FRuntimeAttributeValue59> Values =
				UAbilitySystemInspectorToolset59::GetAttributeValues(TestActor);
			for (int32 i = 1; i < Values.Num(); ++i)
			{
				TestTrue(TEXT("Sorted"), Values[i - 1].FullName <= Values[i].FullName);
			}
		});

		// --- Baseline empty results for other tools ------------------------------

		It(TEXT("GetActiveEffects returns empty with no active effects"), [this]()
		{
			if (!TestNotNull(TEXT("TestActor"), TestActor)) return;
			TArray<FActiveEffectInfo59> Effects =
				UAbilitySystemInspectorToolset59::GetActiveEffects(TestActor);
			TestEqual(TEXT("No effects"), Effects.Num(), 0);
		});

		It(TEXT("GetGrantedAbilities returns empty with no granted abilities"), [this]()
		{
			if (!TestNotNull(TEXT("TestActor"), TestActor)) return;
			TArray<FGrantedAbilityInfo59> Abilities =
				UAbilitySystemInspectorToolset59::GetGrantedAbilities(TestActor);
			TestEqual(TEXT("No abilities"), Abilities.Num(), 0);
		});

		It(TEXT("GetActiveTags returns empty with no tags"), [this]()
		{
			if (!TestNotNull(TEXT("TestActor"), TestActor)) return;
			TArray<FString> Tags = UAbilitySystemInspectorToolset59::GetActiveTags(TestActor);
			TestEqual(TEXT("No tags"), Tags.Num(), 0);
		});
	});
}

#endif  // WITH_DEV_AUTOMATION_TESTS
