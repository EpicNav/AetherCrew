// Games by Hyper 2026.

#include "HyperAIStudioBotGameplay.h"

#include "Components/ActorComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "GameplayTagAssetInterface.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::Bot::Gameplay
{
	namespace
	{
		/** The engine's own lookup, UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent, called by reflection: interface first, then components. */
		UActorComponent* LibraryLookup(AActor& Actor)
		{
			static TWeakObjectPtr<UClass> Library;
			if (!Library.IsValid())
			{
				Library = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.AbilitySystemBlueprintLibrary"));
			}
			UClass* Class = Library.Get();
			UFunction* Function = Class ? Class->FindFunctionByName(TEXT("GetAbilitySystemComponent")) : nullptr;
			const FObjectPropertyBase* Input = Function ? CastField<FObjectPropertyBase>(Function->FindPropertyByName(TEXT("Actor"))) : nullptr;
			const FObjectPropertyBase* Return = Function ? CastField<FObjectPropertyBase>(Function->GetReturnProperty()) : nullptr;
			if (!Input || !Return)
			{
				return nullptr;
			}
			TArray<uint8> Parameters;
			Parameters.SetNumZeroed(FMath::Max<int32>(Function->ParmsSize, 1));
			Function->InitializeStruct(Parameters.GetData());
			Input->SetObjectPropertyValue_InContainer(Parameters.GetData(), &Actor);
			Class->GetDefaultObject()->ProcessEvent(Function, Parameters.GetData());
			UActorComponent* Found = Cast<UActorComponent>(Return->GetObjectPropertyValue_InContainer(Parameters.GetData()));
			Function->DestroyStruct(Parameters.GetData());
			return Found;
		}

		UActorComponent* ComponentSearch(const AActor& Actor)
		{
			for (UActorComponent* Component : Actor.GetComponents())
			{
				if (Component && IsAbilitySystemClass(Component->GetClass()))
				{
					return Component;
				}
			}
			return nullptr;
		}

		UActorComponent* On(AActor& Actor)
		{
			UActorComponent* Found = LibraryLookup(Actor);
			return Found ? Found : ComponentSearch(Actor);
		}

		const void* Field(const UStruct& Type, const void* Container, const TCHAR* Name, FProperty*& OutProperty)
		{
			OutProperty = FindFProperty<FProperty>(&Type, Name);
			return OutProperty ? OutProperty->ContainerPtrToValuePtr<void>(Container) : nullptr;
		}

		bool Number(const FProperty* Property, const void* Value, double& Out)
		{
			const FNumericProperty* Numeric = CastField<FNumericProperty>(Property);
			if (!Numeric || !Value || Numeric->IsEnum())
			{
				return false;
			}
			Out = Numeric->IsFloatingPoint() ? Numeric->GetFloatingPointPropertyValue(Value) : static_cast<double>(Numeric->GetSignedIntPropertyValue(Value));
			return true;
		}

		FString ClassName(const UObject* Object)
		{
			if (!Object)
			{
				return TEXT("none");
			}
			const UClass* Class = Object->IsA<UClass>() ? static_cast<const UClass*>(Object) : Object->GetClass();
			FString Name = Class->GetName();
			Name.RemoveFromEnd(TEXT("_C"));
			return Name;
		}

		/** Each element of an array property inside Container, with its struct. */
		template <typename FVisit>
		void ForEachStruct(const UStruct& Type, const void* Container, const TCHAR* ArrayName, FVisit&& Visit)
		{
			FProperty* Property = nullptr;
			const void* Value = Field(Type, Container, ArrayName, Property);
			const FArrayProperty* Array = CastField<FArrayProperty>(Property);
			const FStructProperty* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
			if (!Inner || !Value)
			{
				return;
			}
			FScriptArrayHelper Helper(Array, Value);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				Visit(*Inner->Struct, static_cast<const void*>(Helper.GetRawPtr(Index)));
			}
		}

		/** The attribute sets the component spawned, from its reflected SpawnedAttributes. */
		TArray<UObject*> AttributeSets(UActorComponent& AbilitySystem)
		{
			TArray<UObject*> Sets;
			FProperty* Property = nullptr;
			const void* Value = Field(*AbilitySystem.GetClass(), &AbilitySystem, TEXT("SpawnedAttributes"), Property);
			const FArrayProperty* Array = CastField<FArrayProperty>(Property);
			const FObjectPropertyBase* Inner = Array ? CastField<FObjectPropertyBase>(Array->Inner) : nullptr;
			if (!Inner || !Value)
			{
				return Sets;
			}
			FScriptArrayHelper Helper(Array, Value);
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				if (UObject* Set = Inner->GetObjectPropertyValue(Helper.GetRawPtr(Index)))
				{
					Sets.Add(Set);
				}
			}
			return Sets;
		}

		/** An attribute's value: a GameplayAttributeData's CurrentValue (or BaseValue), or a plain number. */
		bool AttributeValue(const FProperty& Property, const void* Value, const bool bBase, double& Out)
		{
			if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
			{
				FProperty* Inner = nullptr;
				const void* InnerValue = Field(*Struct->Struct, Value, bBase ? TEXT("BaseValue") : TEXT("CurrentValue"), Inner);
				return Number(Inner, InnerValue, Out);
			}
			return !bBase && Number(&Property, Value, Out);
		}
	}

	bool IsAbilitySystemClass(const UClass* Class)
	{
		for (const UClass* At = Class; At; At = At->GetSuperClass())
		{
			if (At->GetFName() == TEXT("AbilitySystemComponent"))
			{
				return true;
			}
		}
		return false;
	}

	UActorComponent* FindAbilitySystem(AActor& Target)
	{
		if (UActorComponent* Own = On(Target))
		{
			return Own;
		}
		// Many games keep the player's ability system on its player state.
		APlayerState* State = nullptr;
		if (const APawn* Pawn = Cast<APawn>(&Target))
		{
			State = Pawn->GetPlayerState();
		}
		else if (const AController* Controller = Cast<AController>(&Target))
		{
			State = Controller->PlayerState;
		}
		return State ? On(*State) : nullptr;
	}

	Private::FReading ReadAttribute(UActorComponent& AbilitySystem, const FString& InName)
	{
		Private::FReading Out;
		FString Name = InName.TrimStartAndEnd();
		bool bBase = false;
		if (Name.EndsWith(TEXT(".Base"), ESearchCase::IgnoreCase))
		{
			bBase = true;
			Name.LeftChopInline(5);
		}
		else if (Name.EndsWith(TEXT(".Current"), ESearchCase::IgnoreCase))
		{
			Name.LeftChopInline(8);
		}
		FString SetName;
		FString Attribute = Name;
		Name.Split(TEXT("."), &SetName, &Attribute);
		const TArray<UObject*> Sets = AttributeSets(AbilitySystem);
		for (UObject* Set : Sets)
		{
			if (!SetName.IsEmpty() && !NameMatches(Set->GetClass()->GetName(), SetName) && !Set->GetName().Equals(SetName, ESearchCase::IgnoreCase))
			{
				continue;
			}
			for (TFieldIterator<FProperty> It(Set->GetClass()); It; ++It)
			{
				if (!It->GetName().Equals(Attribute, ESearchCase::IgnoreCase) && !It->GetAuthoredName().Equals(Attribute, ESearchCase::IgnoreCase))
				{
					continue;
				}
				double Value = 0.0;
				if (AttributeValue(**It, It->ContainerPtrToValuePtr<void>(Set), bBase, Value))
				{
					Out.bOk = true;
					Out.bNumeric = true;
					Out.Number = Value;
					Out.Text = FString::SanitizeFloat(Value);
					return Out;
				}
			}
		}
		const TArray<FString> Known = ListAttributes(AbilitySystem);
		Out.Error = Sets.IsEmpty()
			? FString::Printf(TEXT("the ability system on %s has no attribute sets"), *Private::Describe(AbilitySystem.GetOwner()))
			: FString::Printf(TEXT("no attribute %s; it has %s"), *InName, *FString::Join(Known, TEXT(", ")));
		return Out;
	}

	TArray<FString> ListAttributes(UActorComponent& AbilitySystem, const int32 Max)
	{
		TArray<FString> Lines;
		for (UObject* Set : AttributeSets(AbilitySystem))
		{
			for (TFieldIterator<FProperty> It(Set->GetClass()); It && Lines.Num() < Max; ++It)
			{
				double Value = 0.0;
				const FStructProperty* Struct = CastField<FStructProperty>(*It);
				// Attributes only: GameplayAttributeData and what derives from it.
				bool bAttribute = false;
				for (const UStruct* Type = Struct ? Struct->Struct : nullptr; Type; Type = Type->GetSuperStruct())
				{
					bAttribute |= Type->GetFName() == TEXT("GameplayAttributeData");
				}
				if (bAttribute && AttributeValue(**It, It->ContainerPtrToValuePtr<void>(Set), false, Value))
				{
					Lines.Add(FString::Printf(TEXT("%s.%s = %g"), *ClassName(Set), *It->GetName(), Value));
				}
			}
		}
		return Lines;
	}

	bool ReadTags(AActor& Target, FGameplayTagContainer& OutTags)
	{
		bool bAny = false;
		if (UActorComponent* AbilitySystem = FindAbilitySystem(Target))
		{
			if (const IGameplayTagAssetInterface* Tagged = Cast<IGameplayTagAssetInterface>(AbilitySystem))
			{
				Tagged->GetOwnedGameplayTags(OutTags);
				bAny = true;
			}
		}
		if (const IGameplayTagAssetInterface* Own = Cast<IGameplayTagAssetInterface>(&Target))
		{
			FGameplayTagContainer Mine;
			Own->GetOwnedGameplayTags(Mine);
			OutTags.AppendTags(Mine);
			bAny = true;
		}
		return bAny;
	}

	TArray<FAbility> ReadAbilities(UActorComponent& AbilitySystem)
	{
		TArray<FAbility> Out;
		FProperty* Container = nullptr;
		const void* Specs = Field(*AbilitySystem.GetClass(), &AbilitySystem, TEXT("ActivatableAbilities"), Container);
		const FStructProperty* Struct = CastField<FStructProperty>(Container);
		if (!Struct || !Specs)
		{
			return Out;
		}
		ForEachStruct(*Struct->Struct, Specs, TEXT("Items"), [&Out](const UStruct& Spec, const void* Item)
		{
			FAbility& Ability = Out.AddDefaulted_GetRef();
			FProperty* Property = nullptr;
			const void* Value = Field(Spec, Item, TEXT("Ability"), Property);
			const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property);
			Ability.Name = ClassName(Object && Value ? Object->GetObjectPropertyValue(Value) : nullptr);
			double Read = 0.0;
			Value = Field(Spec, Item, TEXT("Level"), Property);
			Ability.Level = Number(Property, Value, Read) ? static_cast<int32>(Read) : 0;
			Value = Field(Spec, Item, TEXT("ActiveCount"), Property);
			Ability.ActiveCount = Number(Property, Value, Read) ? static_cast<int32>(Read) : 0;
		});
		return Out;
	}

	TArray<FEffect> ReadEffects(UActorComponent& AbilitySystem)
	{
		TArray<FEffect> Out;
		FProperty* Container = nullptr;
		const void* Active = Field(*AbilitySystem.GetClass(), &AbilitySystem, TEXT("ActiveGameplayEffects"), Container);
		const FStructProperty* Struct = CastField<FStructProperty>(Container);
		if (!Struct || !Active)
		{
			return Out;
		}
		ForEachStruct(*Struct->Struct, Active, TEXT("GameplayEffects_Internal"), [&Out](const UStruct& Type, const void* Item)
		{
			FEffect& Effect = Out.AddDefaulted_GetRef();
			FProperty* Property = nullptr;
			const void* Spec = Field(Type, Item, TEXT("Spec"), Property);
			const FStructProperty* SpecStruct = CastField<FStructProperty>(Property);
			if (SpecStruct && Spec)
			{
				const void* Value = Field(*SpecStruct->Struct, Spec, TEXT("Def"), Property);
				const FObjectPropertyBase* Def = CastField<FObjectPropertyBase>(Property);
				Effect.Name = ClassName(Def && Value ? Def->GetObjectPropertyValue(Value) : nullptr);
				double Read = 0.0;
				Value = Field(*SpecStruct->Struct, Spec, TEXT("StackCount"), Property);
				Effect.Stacks = Number(Property, Value, Read) ? static_cast<int32>(Read) : 1;
			}
			const void* Inhibited = Field(Type, Item, TEXT("bIsInhibited"), Property);
			if (const FBoolProperty* Bool = CastField<FBoolProperty>(Property); Bool && Inhibited)
			{
				Effect.bInhibited = Bool->GetPropertyValue(Inhibited);
			}
		});
		return Out;
	}

	bool NameMatches(const FString& Reflected, const FString& Wanted)
	{
		FString A = Reflected;
		FString B = Wanted.TrimStartAndEnd();
		// A path names the asset last: /Game/Abilities/GA_Dash.GA_Dash_C.
		int32 Dot = INDEX_NONE;
		if (B.FindLastChar(TEXT('.'), Dot))
		{
			B.RightChopInline(Dot + 1);
		}
		for (FString* Name : {&A, &B})
		{
			Name->RemoveFromStart(TEXT("Default__"));
			Name->RemoveFromEnd(TEXT("_C"));
		}
		return !B.IsEmpty() && A.Equals(B, ESearchCase::IgnoreCase);
	}
}
