// Games by Hyper 2026.

#include "HyperAIStudioGASAuthoring.h"

#include "Abilities/GameplayAbility.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AttributeSet.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameplayAbilityBlueprint.h"
#include "GameplayEffect.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/BlockAbilityTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"
#include "GameplayTagsManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSourceControl.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioGASAuthoring"

namespace HyperAIStudio::GAS::Authoring
{
	namespace
	{
		using C = FHyperAIStudioGASContracts;

		enum class EKind : uint8
		{
			CreateEffect,
			CreateAbility,
			EffectDuration,
			EffectModifiers,
			EffectCues,
			EffectCueFlags,
			EffectAssetTags,
			EffectGrantedTags,
			EffectBlockedAbilityTags,
			AbilityAssetTags,
			AbilityTags,
			AbilityProperty
		};

		struct FKindInfo
		{
			const TCHAR* Type;
			EKind Kind;
			bool bEffect;
			bool bCreates;
		};

		constexpr FKindInfo Kinds[] = {
			{TEXT("create_gameplay_effect_blueprint"), EKind::CreateEffect, true, true},
			{TEXT("create_gameplay_ability_blueprint"), EKind::CreateAbility, false, true},
			{TEXT("set_effect_duration"), EKind::EffectDuration, true, false},
			{TEXT("replace_effect_modifiers"), EKind::EffectModifiers, true, false},
			{TEXT("replace_effect_cues"), EKind::EffectCues, true, false},
			{TEXT("set_effect_cue_flags"), EKind::EffectCueFlags, true, false},
			{TEXT("replace_effect_asset_tags"), EKind::EffectAssetTags, true, false},
			{TEXT("replace_effect_granted_tags"), EKind::EffectGrantedTags, true, false},
			{TEXT("replace_effect_blocked_ability_tags"), EKind::EffectBlockedAbilityTags, true, false},
			{TEXT("set_ability_asset_tags"), EKind::AbilityAssetTags, false, false},
			{TEXT("set_ability_tags"), EKind::AbilityTags, false, false},
			{TEXT("set_ability_property"), EKind::AbilityProperty, false, false}};

		const FKindInfo* FindKind(const FString& Type)
		{
			for (const FKindInfo& Info : Kinds)
			{
				if (Type.Equals(Info.Type, ESearchCase::IgnoreCase))
				{
					return &Info;
				}
			}
			return nullptr;
		}

		/** The ability's tag containers, by the name a plan gives them and the property the Details panel edits. */
		const TMap<FString, FName>& AbilityTagProperties()
		{
			static const TMap<FString, FName> Properties = {
				{TEXT("activation_owned"), TEXT("ActivationOwnedTags")},
				{TEXT("activation_required"), TEXT("ActivationRequiredTags")},
				{TEXT("activation_blocked"), TEXT("ActivationBlockedTags")},
				{TEXT("cancel_abilities_with"), TEXT("CancelAbilitiesWithTag")},
				{TEXT("block_abilities_with"), TEXT("BlockAbilitiesWithTag")},
				{TEXT("source_required"), TEXT("SourceRequiredTags")},
				{TEXT("source_blocked"), TEXT("SourceBlockedTags")},
				{TEXT("target_required"), TEXT("TargetRequiredTags")},
				{TEXT("target_blocked"), TEXT("TargetBlockedTags")}};
			return Properties;
		}

		FString PackageOf(const FString& Path)
		{
			FString Package = Path.TrimStartAndEnd();
			int32 Dot = INDEX_NONE;
			if (Package.FindChar(TEXT('.'), Dot))
			{
				Package.LeftInline(Dot);
			}
			return Package;
		}

		FString ObjectPathOf(const FString& Path)
		{
			const FString Trimmed = Path.TrimStartAndEnd();
			return Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
		}

		UBlueprint* LoadBlueprint(const FString& Path)
		{
			// Loaded when nothing is in memory: an FName past its length, or a package with "//", stops the editor.
			return !FHyperAIStudioNames::IsObjectPath(Path.TrimStartAndEnd(), nullptr) ? nullptr
				: LoadObject<UBlueprint>(nullptr, *ObjectPathOf(Path), nullptr, LOAD_NoWarn | LOAD_Quiet);
		}

		bool IsEffectBlueprint(const UBlueprint* Blueprint)
		{
			return Blueprint && Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(UGameplayEffect::StaticClass());
		}

		bool IsAbilityBlueprint(const UBlueprint* Blueprint)
		{
			return Blueprint && Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(UGameplayAbility::StaticClass());
		}

		/** A class for a parent, a cost or a cooldown: a Blueprint by its asset path, its class (_C), or a native class. */
		UClass* ResolveClass(const FString& Path, const UClass* Base, const TMap<FString, UBlueprint*>& Planned)
		{
			const FString Trimmed = Path.TrimStartAndEnd();
			if (Trimmed.IsEmpty() || !FHyperAIStudioNames::IsObjectPath(Trimmed, nullptr))
			{
				return nullptr;
			}
			if (UBlueprint* const* Made = Planned.Find(PackageOf(Trimmed).ToLower()))
			{
				return *Made && (*Made)->GeneratedClass && (*Made)->GeneratedClass->IsChildOf(Base) ? (*Made)->GeneratedClass.Get() : nullptr;
			}
			UClass* Class = FindObject<UClass>(nullptr, *Trimmed);
			if (!Class && Trimmed.EndsWith(TEXT("_C")))
			{
				Class = LoadObject<UClass>(nullptr, *Trimmed, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
			if (!Class)
			{
				if (const UBlueprint* Blueprint = LoadBlueprint(Trimmed))
				{
					Class = Blueprint->GeneratedClass;
				}
			}
			return Class && Class->IsChildOf(Base) ? Class : nullptr;
		}

		bool ResolveTags(const TArray<FString>& Names, FGameplayTagContainer& Out, FString& OutError)
		{
			Out.Reset();
			for (const FString& Name : Names)
			{
				if (!FHyperAIStudioNames::IsName(Name.TrimStartAndEnd(), TEXT("A tag"), &OutError)) return false;
				const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*Name.TrimStartAndEnd()), /*ErrorIfNotFound=*/false);
				if (!Tag.IsValid())
				{
					OutError = FString::Printf(TEXT("The tag '%s' is not registered; add it with Epic's Gameplay Tags tools first."), *Name);
					return false;
				}
				Out.AddTag(Tag);
			}
			return true;
		}

		/** An attribute by its set's class and its name; the class is loaded when it is a Blueprint's. */
		bool ResolveAttribute(const FString& OwnerClassPath, const FString& Name, FGameplayAttribute& Out, FString& OutError)
		{
			UClass* Owner = FHyperAIStudioNames::IsObjectPath(OwnerClassPath.TrimStartAndEnd(), nullptr) && FHyperAIStudioNames::IsName(Name.TrimStartAndEnd(), nullptr)
				? FSoftClassPath(OwnerClassPath.TrimStartAndEnd()).TryLoadClass<UAttributeSet>() : nullptr;
			FProperty* Property = Owner ? FindFProperty<FProperty>(Owner, FName(*Name.TrimStartAndEnd())) : nullptr;
			if (!Owner || !Property || !FGameplayAttribute::IsSupportedProperty(Property))
			{
				OutError = !Owner ? FString::Printf(TEXT("%s is not an Attribute Set class (native, such as /Script/MyGame.MyAttributeSet)."), *OwnerClassPath)
					: FString::Printf(TEXT("%s has no attribute %s."), *Owner->GetName(), *Name);
				return false;
			}
			Out = FGameplayAttribute(Property);
			return Out.IsValid();
		}

		bool ParseDuration(const FString& Token, EGameplayEffectDurationType& Out)
		{
			const FString Lower = Token.TrimStartAndEnd().ToLower();
			if (Lower == TEXT("instant")) { Out = EGameplayEffectDurationType::Instant; return true; }
			if (Lower == TEXT("infinite")) { Out = EGameplayEffectDurationType::Infinite; return true; }
			if (Lower == TEXT("duration") || Lower == TEXT("has_duration")) { Out = EGameplayEffectDurationType::HasDuration; return true; }
			return false;
		}

		bool ParseModOp(const FString& Token, EGameplayModOp::Type& Out)
		{
			const FString Lower = Token.TrimStartAndEnd().ToLower();
			if (Lower == TEXT("additive") || Lower == TEXT("add")) { Out = EGameplayModOp::Additive; return true; }
			if (Lower == TEXT("multiplicative") || Lower == TEXT("multiply")) { Out = EGameplayModOp::Multiplicitive; return true; }
			if (Lower == TEXT("division") || Lower == TEXT("divide")) { Out = EGameplayModOp::Division; return true; }
			if (Lower == TEXT("override")) { Out = EGameplayModOp::Override; return true; }
			return false;
		}

		FString ModOpName(const EGameplayModOp::Type Op)
		{
			switch (Op)
			{
			case EGameplayModOp::Additive: return TEXT("additive");
			case EGameplayModOp::Multiplicitive: return TEXT("multiplicative");
			case EGameplayModOp::Division: return TEXT("division");
			case EGameplayModOp::Override: return TEXT("override");
			default: return TEXT("other");
			}
		}

		bool ParseInstancing(const FString& Token, uint8& Out)
		{
			const FString Lower = Token.TrimStartAndEnd().ToLower();
			if (Lower == TEXT("per_actor") || Lower == TEXT("instanced_per_actor")) { Out = EGameplayAbilityInstancingPolicy::InstancedPerActor; return true; }
			if (Lower == TEXT("per_execution") || Lower == TEXT("instanced_per_execution")) { Out = EGameplayAbilityInstancingPolicy::InstancedPerExecution; return true; }
			return false;
		}

		bool ParseNetExecution(const FString& Token, uint8& Out)
		{
			const FString Lower = Token.TrimStartAndEnd().ToLower();
			if (Lower == TEXT("local_predicted")) { Out = EGameplayAbilityNetExecutionPolicy::LocalPredicted; return true; }
			if (Lower == TEXT("local_only")) { Out = EGameplayAbilityNetExecutionPolicy::LocalOnly; return true; }
			if (Lower == TEXT("server_initiated")) { Out = EGameplayAbilityNetExecutionPolicy::ServerInitiated; return true; }
			if (Lower == TEXT("server_only")) { Out = EGameplayAbilityNetExecutionPolicy::ServerOnly; return true; }
			return false;
		}

		FString TagList(const FGameplayTagContainer& Tags)
		{
			TArray<FString> Names;
			for (const FGameplayTag& Tag : Tags)
			{
				Names.Add(Tag.ToString());
			}
			return Names.IsEmpty() ? FString(TEXT("none")) : FString::Join(Names, TEXT(", "));
		}

		/** One property of a class default object, set the way the Details panel sets it: announced before and after. */
		template <typename TProperty, typename TSetter>
		bool SetDefaultProperty(UObject& Defaults, const FName Name, TSetter&& Set)
		{
			TProperty* Property = FindFProperty<TProperty>(Defaults.GetClass(), Name);
			if (!Property)
			{
				return false;
			}
			Defaults.Modify();
			Defaults.PreEditChange(Property);
			Set(*Property);
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Defaults.PostEditChangeProperty(Event);
			return true;
		}

		bool SetTagContainer(UGameplayAbility& Ability, const FName Name, const FGameplayTagContainer& Tags)
		{
			return SetDefaultProperty<FStructProperty>(Ability, Name, [&Ability, &Tags](FStructProperty& Property)
			{
				if (Property.Struct == FGameplayTagContainer::StaticStruct())
				{
					*Property.ContainerPtrToValuePtr<FGameplayTagContainer>(&Ability) = Tags;
				}
			});
		}

		const FGameplayTagContainer* ReadTagContainer(const UGameplayAbility& Ability, const FName Name)
		{
			const FStructProperty* Property = FindFProperty<FStructProperty>(Ability.GetClass(), Name);
			return Property && Property->Struct == FGameplayTagContainer::StaticStruct()
				? Property->ContainerPtrToValuePtr<FGameplayTagContainer>(&Ability) : nullptr;
		}

		FString DescribeEffect(const UBlueprint& Blueprint)
		{
			const UGameplayEffect* Effect = Blueprint.GeneratedClass ? GetDefault<UGameplayEffect>(Blueprint.GeneratedClass) : nullptr;
			if (!Effect)
			{
				return FString::Printf(TEXT("effect %s: no class defaults"), *Blueprint.GetPathName());
			}
			TArray<FString> Parts;
			float Seconds = 0.f;
			Parts.Add(Effect->DurationPolicy == EGameplayEffectDurationType::Instant ? FString(TEXT("instant"))
				: Effect->DurationPolicy == EGameplayEffectDurationType::Infinite ? FString(TEXT("infinite"))
				: Effect->DurationMagnitude.GetStaticMagnitudeIfPossible(1.f, Seconds) ? FString::Printf(TEXT("%g s"), Seconds) : FString(TEXT("duration")));
			const float Period = Effect->Period.GetValueAtLevel(1.f);
			if (Period > 0.f)
			{
				Parts.Add(FString::Printf(TEXT("every %g s%s"), Period, Effect->bExecutePeriodicEffectOnApplication ? TEXT(" from application") : TEXT("")));
			}
			for (const FGameplayModifierInfo& Modifier : Effect->Modifiers)
			{
				FName Caller;
				float Magnitude = 0.f;
				FString Amount = Modifier.ModifierMagnitude.GetStaticMagnitudeIfPossible(1.f, Magnitude) ? FString::Printf(TEXT("%g"), Magnitude)
					: Modifier.ModifierMagnitude.GetMagnitudeCalculationType() == EGameplayEffectMagnitudeCalculation::SetByCaller
					? TEXT("set by caller ") + Modifier.ModifierMagnitude.GetSetByCallerFloat().DataTag.ToString() : FString(TEXT("calculated"));
				Parts.Add(FString::Printf(TEXT("%s %s %s"), Modifier.Attribute.IsValid() ? *Modifier.Attribute.GetName() : TEXT("(no attribute)"),
					*ModOpName(Modifier.ModifierOp), *Amount));
			}
			Parts.Add(TEXT("asset tags ") + TagList(Effect->GetAssetTags()));
			Parts.Add(TEXT("granted tags ") + TagList(Effect->GetGrantedTags()));
			Parts.Add(TEXT("blocks abilities with ") + TagList(Effect->GetBlockedAbilityTags()));
			for (const FGameplayEffectCue& Cue : Effect->GameplayCues)
			{
				Parts.Add(FString::Printf(TEXT("cue %s (levels %g-%g)"), *TagList(Cue.GameplayCueTags), Cue.MinLevel, Cue.MaxLevel));
			}
			return FString::Printf(TEXT("effect %s: %s"), *Blueprint.GetPathName(), *FString::Join(Parts, TEXT("; ")));
		}

		FString DescribeAbility(const UBlueprint& Blueprint)
		{
			const UGameplayAbility* Ability = Blueprint.GeneratedClass ? GetDefault<UGameplayAbility>(Blueprint.GeneratedClass) : nullptr;
			if (!Ability)
			{
				return FString::Printf(TEXT("ability %s: no class defaults"), *Blueprint.GetPathName());
			}
			TArray<FString> Parts;
			Parts.Add(TEXT("asset tags ") + TagList(Ability->GetAssetTags()));
			for (const TPair<FString, FName>& Container : AbilityTagProperties())
			{
				const FGameplayTagContainer* Tags = ReadTagContainer(*Ability, Container.Value);
				if (Tags && !Tags->IsEmpty())
				{
					Parts.Add(Container.Key + TEXT(" ") + TagList(*Tags));
				}
			}
			Parts.Add(Ability->GetInstancingPolicy() == EGameplayAbilityInstancingPolicy::InstancedPerExecution ? TEXT("per_execution") : TEXT("per_actor"));
			const EGameplayAbilityNetExecutionPolicy::Type Net = Ability->GetNetExecutionPolicy();
			Parts.Add(Net == EGameplayAbilityNetExecutionPolicy::LocalPredicted ? TEXT("local_predicted") : Net == EGameplayAbilityNetExecutionPolicy::LocalOnly
				? TEXT("local_only") : Net == EGameplayAbilityNetExecutionPolicy::ServerInitiated ? TEXT("server_initiated") : TEXT("server_only"));
			if (const UGameplayEffect* Cost = Ability->GetCostGameplayEffect())
			{
				Parts.Add(TEXT("cost ") + Cost->GetClass()->GetPathName());
			}
			if (const UGameplayEffect* Cooldown = Ability->GetCooldownGameplayEffect())
			{
				Parts.Add(TEXT("cooldown ") + Cooldown->GetClass()->GetPathName());
			}
			return FString::Printf(TEXT("ability %s: %s"), *Blueprint.GetPathName(), *FString::Join(Parts, TEXT("; ")));
		}

		struct FPlanned
		{
			bool bEffect = false;
			bool bCreated = false;
		};

		/** The plan's hard size bounds, checked before anything is resolved or loaded. */
		bool CheckBounds(const FHyperAIGASApplyPlanRequest& Request, FString& OutError)
		{
			if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
			{
				OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
				return false;
			}
			for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
			{
				const FHyperAIGASPlanOperation& Op = Request.Operations[Index];
				bool bOver = Op.Type.Len() > 64 || Op.TargetPath.Len() > C::MaxPathCharacters || Op.ParentClassPath.Len() > C::MaxPathCharacters
					|| Op.Property.Len() > 64 || Op.Value.Len() > C::MaxPathCharacters || Op.DurationPolicy.Len() > 32 || Op.ExpectedRevision.Len() > 71
					|| Op.Tags.Num() > C::MaxTagsPerSet || Op.Modifiers.Num() > C::MaxModifiers || Op.Cues.Num() > C::MaxCues;
				for (const FString& Tag : Op.Tags)
				{
					bOver |= Tag.Len() > C::MaxTagCharacters;
				}
				for (const FHyperAIGASModifierSpec& Modifier : Op.Modifiers)
				{
					bOver |= Modifier.AttributeOwnerClassPath.Len() > C::MaxPathCharacters || Modifier.AttributeName.Len() > 128
						|| Modifier.Operation.Len() > 32 || Modifier.SetByCallerTag.Len() > C::MaxTagCharacters;
				}
				for (const FHyperAIGASCueSpec& Cue : Op.Cues)
				{
					bOver |= Cue.CueTags.Num() > C::MaxCueTags || Cue.MagnitudeAttributeOwnerClassPath.Len() > C::MaxPathCharacters
						|| Cue.MagnitudeAttributeName.Len() > 128;
					for (const FString& Tag : Cue.CueTags)
					{
						bOver |= Tag.Len() > C::MaxTagCharacters;
					}
				}
				if (bOver)
				{
					OutError = FString::Printf(TEXT("Operation %d exceeds a size bound: paths %d characters, tags %d per list, modifiers %d, cues %d."), Index,
						C::MaxPathCharacters, C::MaxTagsPerSet, C::MaxModifiers, C::MaxCues);
					return false;
				}
			}
			return true;
		}

		/**
		 * Every op checked before anything changes: its kind and fields, the tags, attributes and classes it names, and its
		 * target, as the asset is now or as the plan's earlier ops make it.
		 */
		bool Check(const FHyperAIGASApplyPlanRequest& Request, FString& OutError)
		{
			if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
			{
				OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
				return false;
			}
			TMap<FString, FPlanned> Targets;
			TMap<FString, UBlueprint*> NoneMade;
			for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
			{
				const FHyperAIGASPlanOperation& Op = Request.Operations[Index];
				FString Error;
				auto Fail = [&](const FString& Why)
				{
					OutError = FString::Printf(TEXT("Operation %d (%s): %s Nothing was changed."), Index, *Op.Type, *Why);
					return false;
				};
				if (Op.Type == TEXT("create_attribute_set_blueprint") || Op.Type == TEXT("edit_attribute_set_blueprint"))
				{
					return Fail(TEXT("Attribute sets are C++ classes in the Gameplay Ability System (FGameplayAttributeData properties with their accessors); write one in code."));
				}
				const FKindInfo* Kind = FindKind(Op.Type);
				if (!Kind)
				{
					return Fail(TEXT("Unknown type; see hyper_gas_apply_plan's description."));
				}
				const FString Package = PackageOf(Op.TargetPath);
				const FString Key = Package.ToLower();
				if (Kind->bCreates)
				{
					FText Reason;
					if (!FPackageName::IsValidLongPackageName(Package, false, &Reason) || !Package.StartsWith(TEXT("/Game/")))
					{
						return Fail(FString::Printf(TEXT("TargetPath '%s' is not a path in the project, such as /Game/GAS/GE_Damage."), *Op.TargetPath));
					}
					if (Targets.Contains(Key) || FPackageName::DoesPackageExist(Package) || FindObject<UObject>(nullptr, *ObjectPathOf(Package)))
					{
						return Fail(FString::Printf(TEXT("%s exists already."), *Package));
					}
					const UClass* Base = Kind->bEffect ? UGameplayEffect::StaticClass() : UGameplayAbility::StaticClass();
					if (!Op.ParentClassPath.TrimStartAndEnd().IsEmpty() && !ResolveClass(Op.ParentClassPath, Base, NoneMade))
					{
						return Fail(FString::Printf(TEXT("ParentClassPath %s is not a %s class or Blueprint."), *Op.ParentClassPath, *Base->GetName()));
					}
					Targets.Add(Key, FPlanned{Kind->bEffect, true});
					continue;
				}
				FPlanned* Target = Targets.Find(Key);
				if (!Target)
				{
					UBlueprint* Blueprint = LoadBlueprint(Op.TargetPath);
					if (!IsEffectBlueprint(Blueprint) && !IsAbilityBlueprint(Blueprint))
					{
						return Fail(FString::Printf(TEXT("%s is not a Gameplay Effect or Gameplay Ability Blueprint, and the plan does not make one."), *Op.TargetPath));
					}
					Target = &Targets.Add(Key, FPlanned{IsEffectBlueprint(Blueprint), false});
					if (!Op.ExpectedRevision.IsEmpty())
					{
						FHyperAIGASInspectRequest Look;
						Look.AssetPaths = {Blueprint->GetPathName()};
						const FHyperAIStudioGASValueSnapshot Now = C::CaptureLoadedSnapshot(Look);
						if (Now.Records.Num() != 1 || Now.Records[0].Revision != Op.ExpectedRevision)
						{
							return Fail(TEXT("The asset changed since that revision; inspect it again."));
						}
					}
				}
				if (Target->bEffect != Kind->bEffect)
				{
					return Fail(FString::Printf(TEXT("%s is a Gameplay %s, and this op is for a Gameplay %s."), *Op.TargetPath,
						Target->bEffect ? TEXT("Effect") : TEXT("Ability"), Kind->bEffect ? TEXT("Effect") : TEXT("Ability")));
				}
				FGameplayTagContainer Tags;
				switch (Kind->Kind)
				{
				case EKind::EffectDuration:
				{
					EGameplayEffectDurationType Policy;
					if (!ParseDuration(Op.DurationPolicy, Policy))
					{
						return Fail(TEXT("DurationPolicy is instant, infinite or duration."));
					}
					if (Policy == EGameplayEffectDurationType::HasDuration && !(Op.DurationSeconds > 0.0))
					{
						return Fail(TEXT("A duration effect needs DurationSeconds above 0."));
					}
					if (Policy == EGameplayEffectDurationType::Instant && Op.PeriodSeconds > 0.0)
					{
						return Fail(TEXT("An instant effect has no period."));
					}
					break;
				}
				case EKind::EffectModifiers:
					for (const FHyperAIGASModifierSpec& Modifier : Op.Modifiers)
					{
						FGameplayAttribute Attribute;
						EGameplayModOp::Type ModOp;
						FGameplayTagContainer Caller;
						if (!ResolveAttribute(Modifier.AttributeOwnerClassPath, Modifier.AttributeName, Attribute, Error))
						{
							return Fail(Error);
						}
						if (!ParseModOp(Modifier.Operation, ModOp) || !FMath::IsFinite(Modifier.Magnitude))
						{
							return Fail(TEXT("A modifier's Operation is additive, multiplicative, division or override, with a finite Magnitude."));
						}
						if (!Modifier.SetByCallerTag.IsEmpty() && !ResolveTags({Modifier.SetByCallerTag}, Caller, Error))
						{
							return Fail(Error);
						}
					}
					break;
				case EKind::EffectCues:
					for (const FHyperAIGASCueSpec& Cue : Op.Cues)
					{
						if (Cue.CueTags.IsEmpty() || !ResolveTags(Cue.CueTags, Tags, Error))
						{
							return Fail(Error.IsEmpty() ? FString(TEXT("Each cue needs its GameplayCue tags.")) : Error);
						}
						FGameplayAttribute Attribute;
						if (!Cue.MagnitudeAttributeName.IsEmpty() && !ResolveAttribute(Cue.MagnitudeAttributeOwnerClassPath, Cue.MagnitudeAttributeName, Attribute, Error))
						{
							return Fail(Error);
						}
					}
					break;
				case EKind::EffectCueFlags:
					if (!Op.bSetRequireModifierSuccessToTriggerCues && !Op.bSetSuppressStackingCues)
					{
						return Fail(TEXT("Set bSetRequireModifierSuccessToTriggerCues, bSetSuppressStackingCues or both."));
					}
					break;
				case EKind::EffectAssetTags:
				case EKind::EffectGrantedTags:
				case EKind::EffectBlockedAbilityTags:
				case EKind::AbilityAssetTags:
					if (!ResolveTags(Op.Tags, Tags, Error))
					{
						return Fail(Error);
					}
					break;
				case EKind::AbilityTags:
					if (!AbilityTagProperties().Contains(Op.Property.TrimStartAndEnd().ToLower()))
					{
						return Fail(TEXT("Property is activation_owned, activation_required, activation_blocked, cancel_abilities_with, block_abilities_with, ")
							TEXT("source_required, source_blocked, target_required or target_blocked."));
					}
					if (!ResolveTags(Op.Tags, Tags, Error))
					{
						return Fail(Error);
					}
					break;
				case EKind::AbilityProperty:
				{
					const FString Property = Op.Property.TrimStartAndEnd().ToLower();
					uint8 Value = 0;
					if (Property == TEXT("instancing_policy"))
					{
						if (!ParseInstancing(Op.Value, Value))
						{
							return Fail(TEXT("instancing_policy is per_actor or per_execution."));
						}
					}
					else if (Property == TEXT("net_execution_policy"))
					{
						if (!ParseNetExecution(Op.Value, Value))
						{
							return Fail(TEXT("net_execution_policy is local_predicted, local_only, server_initiated or server_only."));
						}
					}
					else if (Property == TEXT("cost_effect") || Property == TEXT("cooldown_effect"))
					{
						// Empty clears it; an effect made earlier in the plan counts.
						const FPlanned* Planned = Targets.Find(PackageOf(Op.Value).ToLower());
						if (!Op.Value.TrimStartAndEnd().IsEmpty() && !(Planned && Planned->bEffect)
							&& !ResolveClass(Op.Value, UGameplayEffect::StaticClass(), NoneMade))
						{
							return Fail(FString::Printf(TEXT("%s is not a Gameplay Effect."), *Op.Value));
						}
					}
					else
					{
						return Fail(TEXT("Property is instancing_policy, net_execution_policy, cost_effect or cooldown_effect."));
					}
					break;
				}
				default:
					break;
				}
			}
			return true;
		}

		UBlueprint* Create(const FHyperAIGASPlanOperation& Op, const bool bEffect, const TMap<FString, UBlueprint*>& Made, FString& OutError)
		{
			const FString Package = PackageOf(Op.TargetPath);
			const UClass* Base = bEffect ? UGameplayEffect::StaticClass() : UGameplayAbility::StaticClass();
			UClass* Parent = Op.ParentClassPath.TrimStartAndEnd().IsEmpty() ? const_cast<UClass*>(Base) : ResolveClass(Op.ParentClassPath, Base, Made);
			if (!Parent || !FKismetEditorUtilities::CanCreateBlueprintOfClass(Parent))
			{
				OutError = FString::Printf(TEXT("A Blueprint can't be made from %s."), *Op.ParentClassPath);
				return nullptr;
			}
			// Asked again here, as an approval can wait: an asset of another class made there meanwhile would stop the editor.
			if (!FHyperAIStudioNames::IsFreePackage(Package))
			{
				OutError = FString::Printf(TEXT("%s was taken before the Blueprint was made; nothing was made there."), *Package);
				return nullptr;
			}
			UPackage* Outer = CreatePackage(*Package);
			UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(Parent, Outer, FName(*FPackageName::GetShortName(Package)), BPTYPE_Normal,
				bEffect ? UBlueprint::StaticClass() : UGameplayAbilityBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), TEXT("HyperAIStudio"));
			if (!Blueprint || !Blueprint->GeneratedClass)
			{
				OutError = FString::Printf(TEXT("Unreal did not make %s."), *Package);
				return nullptr;
			}
			FAssetRegistryModule::AssetCreated(Blueprint);
			Outer->MarkPackageDirty();
			return Blueprint;
		}

		bool ApplyEffectOp(UBlueprint& Blueprint, const FHyperAIGASPlanOperation& Op, const EKind Kind, FString& OutError)
		{
			UGameplayEffect* Effect = Blueprint.GeneratedClass ? Cast<UGameplayEffect>(Blueprint.GeneratedClass->GetDefaultObject()) : nullptr;
			if (!Effect)
			{
				OutError = TEXT("The effect has no class defaults to edit.");
				return false;
			}
			Effect->Modify();
			FGameplayTagContainer Tags;
			switch (Kind)
			{
			case EKind::EffectDuration:
			{
				EGameplayEffectDurationType Policy = EGameplayEffectDurationType::Instant;
				ParseDuration(Op.DurationPolicy, Policy);
				Effect->DurationPolicy = Policy;
				if (Policy == EGameplayEffectDurationType::HasDuration)
				{
					Effect->DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(static_cast<float>(Op.DurationSeconds)));
				}
				if (Op.PeriodSeconds >= 0.0)
				{
					Effect->Period = FScalableFloat(static_cast<float>(Op.PeriodSeconds));
				}
				if (Op.bSetExecutePeriodicEffectOnApplication)
				{
					Effect->bExecutePeriodicEffectOnApplication = Op.bExecutePeriodicEffectOnApplication;
				}
				break;
			}
			case EKind::EffectModifiers:
				Effect->Modifiers.Reset();
				for (const FHyperAIGASModifierSpec& Spec : Op.Modifiers)
				{
					FGameplayModifierInfo& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
					ResolveAttribute(Spec.AttributeOwnerClassPath, Spec.AttributeName, Modifier.Attribute, OutError);
					EGameplayModOp::Type ModOp = EGameplayModOp::Additive;
					ParseModOp(Spec.Operation, ModOp);
					Modifier.ModifierOp = ModOp;
					if (Spec.SetByCallerTag.IsEmpty())
					{
						Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(static_cast<float>(Spec.Magnitude)));
					}
					else
					{
						FSetByCallerFloat Caller;
						Caller.DataTag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*Spec.SetByCallerTag.TrimStartAndEnd()), false);
						Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Caller);
					}
				}
				break;
			case EKind::EffectCues:
				Effect->GameplayCues.Reset();
				for (const FHyperAIGASCueSpec& Spec : Op.Cues)
				{
					FGameplayEffectCue& Cue = Effect->GameplayCues.AddDefaulted_GetRef();
					ResolveTags(Spec.CueTags, Cue.GameplayCueTags, OutError);
					Cue.MinLevel = static_cast<float>(Spec.MinLevel);
					Cue.MaxLevel = static_cast<float>(Spec.MaxLevel);
					if (!Spec.MagnitudeAttributeName.IsEmpty())
					{
						ResolveAttribute(Spec.MagnitudeAttributeOwnerClassPath, Spec.MagnitudeAttributeName, Cue.MagnitudeAttribute, OutError);
					}
				}
				break;
			case EKind::EffectCueFlags:
				if (Op.bSetRequireModifierSuccessToTriggerCues)
				{
					Effect->bRequireModifierSuccessToTriggerCues = Op.bRequireModifierSuccessToTriggerCues;
				}
				if (Op.bSetSuppressStackingCues)
				{
					Effect->bSuppressStackingCues = Op.bSuppressStackingCues;
				}
				break;
			case EKind::EffectAssetTags:
			case EKind::EffectGrantedTags:
			case EKind::EffectBlockedAbilityTags:
			{
				// Since UE 5.3 an effect's tags live in its components; each is found or added and its tags replaced.
				ResolveTags(Op.Tags, Tags, OutError);
				FInheritedTagContainer Changes;
				Changes.Added = Tags;
				if (Kind == EKind::EffectAssetTags)
				{
					UAssetTagsGameplayEffectComponent& Component = Effect->FindOrAddComponent<UAssetTagsGameplayEffectComponent>();
					Component.Modify();
					Component.SetAndApplyAssetTagChanges(Changes);
				}
				else if (Kind == EKind::EffectGrantedTags)
				{
					UTargetTagsGameplayEffectComponent& Component = Effect->FindOrAddComponent<UTargetTagsGameplayEffectComponent>();
					Component.Modify();
					Component.SetAndApplyTargetTagChanges(Changes);
				}
				else
				{
					UBlockAbilityTagsGameplayEffectComponent& Component = Effect->FindOrAddComponent<UBlockAbilityTagsGameplayEffectComponent>();
					Component.Modify();
					Component.SetAndApplyBlockedAbilityTagChanges(Changes);
				}
				break;
			}
			default:
				OutError = TEXT("That op is not for a Gameplay Effect.");
				return false;
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
			return true;
		}

		bool ApplyAbilityOp(UBlueprint& Blueprint, const FHyperAIGASPlanOperation& Op, const EKind Kind, const TMap<FString, UBlueprint*>& Made, FString& OutError)
		{
			UGameplayAbility* Ability = Blueprint.GeneratedClass ? Cast<UGameplayAbility>(Blueprint.GeneratedClass->GetDefaultObject()) : nullptr;
			if (!Ability)
			{
				OutError = TEXT("The ability has no class defaults to edit.");
				return false;
			}
			bool bDone = false;
			FGameplayTagContainer Tags;
			if (Kind == EKind::AbilityAssetTags || Kind == EKind::AbilityTags)
			{
				ResolveTags(Op.Tags, Tags, OutError);
				bDone = SetTagContainer(*Ability, Kind == EKind::AbilityAssetTags ? FName(TEXT("AbilityTags"))
					: AbilityTagProperties().FindRef(Op.Property.TrimStartAndEnd().ToLower()), Tags);
			}
			else if (Kind == EKind::AbilityProperty)
			{
				const FString Property = Op.Property.TrimStartAndEnd().ToLower();
				uint8 Value = 0;
				if (Property == TEXT("instancing_policy") || Property == TEXT("net_execution_policy"))
				{
					const bool bInstancing = Property == TEXT("instancing_policy");
					if (bInstancing ? ParseInstancing(Op.Value, Value) : ParseNetExecution(Op.Value, Value))
					{
						bDone = SetDefaultProperty<FByteProperty>(*Ability, bInstancing ? FName(TEXT("InstancingPolicy")) : FName(TEXT("NetExecutionPolicy")),
							[Ability, Value](FByteProperty& Byte) { Byte.SetPropertyValue_InContainer(Ability, Value); });
					}
				}
				else if (Property == TEXT("cost_effect") || Property == TEXT("cooldown_effect"))
				{
					UClass* Effect = Op.Value.TrimStartAndEnd().IsEmpty() ? nullptr : ResolveClass(Op.Value, UGameplayEffect::StaticClass(), Made);
					bDone = SetDefaultProperty<FClassProperty>(*Ability, Property == TEXT("cost_effect") ? FName(TEXT("CostGameplayEffectClass"))
						: FName(TEXT("CooldownGameplayEffectClass")), [Ability, Effect](FClassProperty& Class) { Class.SetObjectPropertyValue_InContainer(Ability, Effect); });
				}
			}
			if (!bDone)
			{
				OutError = FString::Printf(TEXT("This engine's GameplayAbility has no editable %s."), *Op.Property);
				return false;
			}
			FBlueprintEditorUtils::MarkBlueprintAsModified(&Blueprint);
			return true;
		}

		/** The plan for real: one undo step, then every Blueprint it touched compiled and, when all compile, saved. */
		FHyperAIGASApplyPlanReport Execute(const FHyperAIGASApplyPlanRequest& Request, const FString& OperationId)
		{
			FHyperAIGASApplyPlanReport Report;
			Report.bDryRun = false;
			Report.OperationId = OperationId;
			Report.Effects.OperationCount = Request.Operations.Num();
			TArray<UBlueprint*> Touched;
			TMap<FString, UBlueprint*> Made;
			{
				const FScopedTransaction Transaction(LOCTEXT("GASPlan", "HyperAI: Edit Gameplay Abilities and Effects"));
				for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
				{
					const FHyperAIGASPlanOperation& Op = Request.Operations[Index];
					const FKindInfo* Kind = FindKind(Op.Type);
					const FString Key = PackageOf(Op.TargetPath).ToLower();
					FString Error;
					UBlueprint* Blueprint = Kind && Kind->bCreates ? Create(Op, Kind->bEffect, Made, Error)
						: Made.Contains(Key) ? Made[Key] : LoadBlueprint(Op.TargetPath);
					const bool bApplied = Blueprint && Kind && (Kind->bCreates
						|| (Kind->bEffect ? ApplyEffectOp(*Blueprint, Op, Kind->Kind, Error) : ApplyAbilityOp(*Blueprint, Op, Kind->Kind, Made, Error)));
					if (!bApplied)
					{
						Report.Status = TEXT("operation_failed");
						Report.Diagnostic = FString::Printf(TEXT("Operation %d (%s): %s%s"), Index, *Op.Type, Error.IsEmpty() ? TEXT("its target went missing.") : *Error,
							Index > 0 ? TEXT(" The operations before it stay applied; Edit > Undo takes the whole plan back.") : TEXT(""));
						return Report;
					}
					if (Kind->bCreates)
					{
						Made.Add(Key, Blueprint);
					}
					Touched.AddUnique(Blueprint);
				}
			}
			Report.Effects.TargetCount = Touched.Num();
			for (const UBlueprint* Blueprint : Touched)
			{
				++(IsEffectBlueprint(Blueprint) ? Report.Effects.EffectsUpdated : Report.Effects.AbilitiesUpdated);
			}
			Report.Effects.bTransactionOnce = true;
			for (UBlueprint* Blueprint : Touched)
			{
				FCompilerResultsLog Results;
				Results.bSilentMode = true;
				FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection, &Results);
				if (Blueprint->Status == BS_Error)
				{
					Report.Status = TEXT("compile_failed");
					TArray<FString> Errors;
					for (const TSharedRef<FTokenizedMessage>& Message : Results.Messages)
					{
						if (Message->GetSeverity() == EMessageSeverity::Error)
						{
							Errors.Add(Message->ToText().ToString());
						}
					}
					Report.Diagnostic = FString::Printf(TEXT("%s does not compile, so nothing was saved: %s Edit > Undo takes the plan back."),
						*Blueprint->GetName(), *FString::Join(Errors, TEXT(" ")));
					return Report;
				}
				Report.Made.Add(IsEffectBlueprint(Blueprint) ? DescribeEffect(*Blueprint) : DescribeAbility(*Blueprint));
			}
			Report.Effects.bCompileOnce = true;
			for (UBlueprint* Blueprint : Touched)
			{
				FString Error;
				if (!FHyperAIStudioSourceControl::SaveAsset(*Blueprint, Error))
				{
					Report.Status = TEXT("save_failed");
					Report.Diagnostic = Error;
					return Report;
				}
				Report.Saved.Add(Blueprint->GetPathName());
			}
			Report.Effects.bSaveOnce = true;
			Report.bOk = true;
			Report.Status = TEXT("completed");
			Report.Diagnostic = FString::Printf(TEXT("%d operation(s) applied to %d asset(s), compiled and saved; Made reads each back."),
				Request.Operations.Num(), Touched.Num());
			return Report;
		}

		void Record(const FString& OperationId, const FHyperAIGASApplyPlanReport& Done)
		{
			FHyperAIStudioActivityEntry Entry;
			Entry.Kind = Done.bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
			Entry.PackId = C::PackId;
			Entry.ToolName = TEXT("hyper_gas_apply_plan");
			Entry.Target = Done.Saved.IsEmpty() ? FString() : Done.Saved[0];
			Entry.OperationId = OperationId;
			Entry.StatusCode = Done.bOk ? TEXT("completed") : TEXT("failed");
			Entry.Detail = Done.Diagnostic.Left(4000);
			FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
		}
	}

	void LoadRequested(const TArray<FString>& AssetPaths)
	{
		for (const FString& Path : AssetPaths)
		{
			if (Path.StartsWith(TEXT("/Game/")) && FHyperAIStudioNames::IsObjectPath(Path, nullptr) && !FSoftObjectPath(Path).ResolveObject()
				&& FPackageName::DoesPackageExist(PackageOf(Path)))
			{
				LoadObject<UObject>(nullptr, *ObjectPathOf(Path), nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
		}
	}

	FHyperAIGASApplyPlanReport Run(const FHyperAIGASApplyPlanRequest& Request)
	{
		FHyperAIGASApplyPlanReport Report;
		Report.bDryRun = Request.bDryRun;
		Report.EpicDelegates = C::GetEpicDelegates();
		Report.OperationId = Request.OperationId.IsEmpty()
			? FString::Printf(TEXT("gas-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
		if (!IsInGameThread())
		{
			Report.Status = TEXT("game_thread_required");
			Report.Diagnostic = TEXT("GAS edits run on the game thread.");
			return Report;
		}
		// Tags, attributes and paths become FNames and lookups, which stop the editor past an FName's length; CheckBounds
		// lets a path reach 1024 characters, and Check looks a new Blueprint's path up before anything else does.
		if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
		FString Error;
		if (!CheckBounds(Request, Error))
		{
			Report.Status = TEXT("invalid_request_bounds");
			Report.Diagnostic = Error;
			return Report;
		}
		if (!Check(Request, Error))
		{
			Report.Status = TEXT("invalid_request");
			Report.Diagnostic = Error;
			return Report;
		}
		if (Request.bDryRun)
		{
			Report.bOk = true;
			Report.Status = TEXT("checked");
			Report.Diagnostic = FString::Printf(TEXT("%d operation(s) checked; nothing was changed. Send it again with bDryRun false to apply it."),
				Request.Operations.Num());
			return Report;
		}
		if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
		{
			FHyperAIStudioApprovalSummary Summary;
			Summary.PackId = C::PackId;
			Summary.ToolName = TEXT("hyper_gas_apply_plan");
			Summary.VariantId = TEXT("gas.authoring.v1");
			Summary.Safety = EHyperAIStudioDomainSafety::Edit;
			Summary.EffectTarget = Request.Operations[0].TargetPath;
			Summary.Effects.Add(FString::Printf(TEXT("Make or edit Gameplay Effects and Abilities: %d operation(s), one undo step, compiled and saved"),
				Request.Operations.Num()));
			for (int32 Index = 0; Index < Request.Operations.Num() && Index < 24; ++Index)
			{
				Summary.Effects.Add(FString::Printf(TEXT("| %s %s"), *Request.Operations[Index].Type, *Request.Operations[Index].TargetPath));
				Summary.Touches.AddUnique(Request.Operations[Index].TargetPath);
			}
			const FString OperationId = Report.OperationId;
			if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
				{
					Record(OperationId, Execute(Request, OperationId));
				}, Error))
			{
				Report.Status = TEXT("approval_not_queued");
				Report.Diagnostic = Error;
				return Report;
			}
			Report.bOk = true;
			Report.Status = TEXT("awaiting_user_approval");
			Report.Diagnostic = TEXT("Agent edits need approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
			FHyperAIStudioApprovalGate::DescribeHeld(OperationId, Report.Status, Report.Diagnostic);
			return Report;
		}
		return Execute(Request, Report.OperationId);
	}
}

#undef LOCTEXT_NAMESPACE
