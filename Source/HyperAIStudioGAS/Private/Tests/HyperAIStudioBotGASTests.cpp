// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystemComponent.h"
#include "AbilitySystemTestAttributeSet.h"
#include "Abilities/GameplayAbility.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameplayEffect.h"
#include "GameplayTagsManager.h"
#include "HyperAIStudioBot.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioBotGASAssertsTest,
	"HyperAIStudio.Bot.GASAsserts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioBotGASAssertsTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld=*/false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	ON_SCOPE_EXIT
	{
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(/*bInformEngineOfWorld=*/false);
	};
	FActorSpawnParameters Spawn;
	Spawn.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Hero = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
	AActor* Plain = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Spawn);
	if (!TestNotNull(TEXT("actors spawned"), Hero) || !Plain)
	{
		return false;
	}
	Hero->Tags.Add(TEXT("Hero"));
	Plain->Tags.Add(TEXT("Plain"));
	UAbilitySystemComponent* AbilitySystem = NewObject<UAbilitySystemComponent>(Hero, TEXT("AbilitySystem"));
	Hero->AddInstanceComponent(AbilitySystem);
	AbilitySystem->RegisterComponent();
	AbilitySystem->InitAbilityActorInfo(Hero, Hero);
	UAbilitySystemTestAttributeSet* Set = NewObject<UAbilitySystemTestAttributeSet>(Hero);
	Set->Health = 75.f;
	Set->Mana.SetBaseValue(50.f);
	Set->Mana.SetCurrentValue(42.f);
	AbilitySystem->AddSpawnedAttribute(Set);
	const FGameplayTag Stunned = UGameplayTagsManager::Get().AddNativeGameplayTag(TEXT("HyperAITest.Bot.Stunned"));
	const FGameplayTag Burning = UGameplayTagsManager::Get().AddNativeGameplayTag(TEXT("HyperAITest.Bot.Burning"));
	AbilitySystem->AddLooseGameplayTag(Stunned);
	AbilitySystem->GiveAbility(FGameplayAbilitySpec(UGameplayAbility::StaticClass(), 3));
	UGameplayEffect* Effect = NewObject<UGameplayEffect>(GetTransientPackage(), TEXT("GE_HyperAIBotTest"));
	Effect->DurationPolicy = EGameplayEffectDurationType::Infinite;
	AbilitySystem->ApplyGameplayEffectToSelf(Effect, 1.f, AbilitySystem->MakeEffectContext());
	AbilitySystem->ApplyGameplayEffectToSelf(Effect, 1.f, AbilitySystem->MakeEffectContext());

	FHyperAIBotExecutor Bot(*World, FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("BotGAS")));
	auto Check = [&Bot](const EHyperAIBotAction Action, const FString& Target, const FString& Property, const FString& Compare, const FString& Value)
	{
		FHyperAIBotStep Step;
		Step.Action = Action;
		Step.Target = Target;
		Step.Property = Property;
		Step.Compare = Compare;
		Step.Value = Value;
		return Bot.Begin(Step);
	};
	auto Passed = [](const FHyperAIBotResult& Result) { return Result.State == FHyperAIBotResult::EState::Passed; };

	FHyperAIBotResult Result = Check(EHyperAIBotAction::AssertAttribute, TEXT("Hero"), TEXT("Mana"), TEXT("=="), TEXT("42"));
	TestTrue(FString::Printf(TEXT("Mana reads its current value: %s"), *Result.Detail), Passed(Result));
	Result = Check(EHyperAIBotAction::AssertAttribute, TEXT("Hero"), TEXT("AbilitySystemTestAttributeSet.Mana.Base"), TEXT(">="), TEXT("50"));
	TestTrue(FString::Printf(TEXT("a set's Mana base value: %s"), *Result.Detail), Passed(Result));
	Result = Check(EHyperAIBotAction::AssertAttribute, TEXT("Hero"), TEXT("Health"), TEXT("<"), TEXT("50"));
	TestEqual(TEXT("Health 75 is not under 50"), Result.Code, FString(TEXT("assertion_failed")));
	Result = Check(EHyperAIBotAction::AssertAttribute, TEXT("Hero"), TEXT("Stamina"), TEXT("=="), TEXT("1"));
	TestEqual(TEXT("an attribute it lacks"), Result.Code, FString(TEXT("attribute_not_found")));
	TestTrue(TEXT("and the ones it has are named"), Result.Detail.Contains(TEXT("Mana")));
	Result = Check(EHyperAIBotAction::AssertAttribute, TEXT("Plain"), TEXT("Health"), TEXT("=="), TEXT("1"));
	TestEqual(TEXT("an actor with no ability system"), Result.Code, FString(TEXT("no_ability_system")));

	Result = Check(EHyperAIBotAction::AssertTag, TEXT("Hero"), TEXT("HyperAITest.Bot.Stunned"), TEXT("=="), TEXT(""));
	TestTrue(FString::Printf(TEXT("the stunned tag is present: %s"), *Result.Detail), Passed(Result));
	Result = Check(EHyperAIBotAction::AssertTag, TEXT("Hero"), TEXT("HyperAITest.Bot"), TEXT("=="), TEXT("present"));
	TestTrue(TEXT("a parent tag matches its child"), Passed(Result));
	Result = Check(EHyperAIBotAction::AssertTag, TEXT("Hero"), TEXT("HyperAITest.Bot.Burning"), TEXT("=="), TEXT("absent"));
	TestTrue(TEXT("the burning tag is absent"), Passed(Result));
	Result = Check(EHyperAIBotAction::AssertTag, TEXT("Hero"), TEXT("HyperAITest.Bot.Burning"), TEXT("=="), TEXT("present"));
	TestEqual(TEXT("asking for an absent tag fails"), Result.Code, FString(TEXT("assertion_failed")));

	Result = Check(EHyperAIBotAction::AssertAbility, TEXT("Hero"), TEXT("GameplayAbility"), TEXT("=="), TEXT(""));
	TestTrue(FString::Printf(TEXT("the ability is granted: %s"), *Result.Detail), Passed(Result));
	TestTrue(TEXT("with its level"), Result.Detail.Contains(TEXT("level 3")));
	Result = Check(EHyperAIBotAction::AssertAbility, TEXT("Hero"), TEXT("GameplayAbility"), TEXT("=="), TEXT("active"));
	TestEqual(TEXT("granted but not running"), Result.Code, FString(TEXT("assertion_failed")));
	Result = Check(EHyperAIBotAction::AssertAbility, TEXT("Hero"), TEXT("GA_Missing"), TEXT("=="), TEXT("absent"));
	TestTrue(TEXT("an ability never granted is absent"), Passed(Result));

	Result = Check(EHyperAIBotAction::AssertEffect, TEXT("Hero"), TEXT("GameplayEffect"), TEXT("=="), TEXT(""));
	TestTrue(FString::Printf(TEXT("the effect is applied: %s"), *Result.Detail), Passed(Result));
	Result = Check(EHyperAIBotAction::AssertEffect, TEXT("Hero"), TEXT("GameplayEffect"), TEXT("=="), TEXT("2"));
	TestTrue(FString::Printf(TEXT("two applications, two stacks: %s"), *Result.Detail), Passed(Result));
	Result = Check(EHyperAIBotAction::AssertEffect, TEXT("Hero"), TEXT("GE_Other"), TEXT("=="), TEXT("0"));
	TestTrue(TEXT("an effect not applied has no stacks"), Passed(Result));

	// WaitUntil takes the new assertions.
	FHyperAIBotStep Wait;
	Wait.Action = EHyperAIBotAction::WaitUntil;
	Wait.Until = EHyperAIBotAction::AssertTag;
	Wait.Target = TEXT("Hero");
	Wait.Property = TEXT("HyperAITest.Bot.Stunned");
	Wait.TimeoutSeconds = 1.f;
	Result = Bot.Begin(Wait);
	TestTrue(FString::Printf(TEXT("WaitUntil AssertTag passes at once: %s"), *Result.Detail), Passed(Result));
	Bot.Stop();
	return true;
}

#endif
