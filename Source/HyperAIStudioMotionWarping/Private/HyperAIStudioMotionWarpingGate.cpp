// Games by Hyper 2026.

#include "HyperAIStudioMotionWarpingGate.h"

#include "AnimNotifyState_MotionWarping.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Character.h"
#include "HyperAIStudioNames.h"
#include "Misc/PackageName.h"
#include "Misc/StringOutputDevice.h"
#include "MotionWarpingComponent.h"
#include "RootMotionModifier.h"
#include "RootMotionModifier_AdjustmentBlendWarp.h"
#include "RootMotionModifier_PrecomputedWarp.h"
#include "RootMotionModifier_SkewWarp.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::MotionWarping::Gate
{
	using C = FHyperAIStudioMotionWarpingContracts;

	namespace
	{
		FString Lower(const FString& Text)
		{
			return Text.TrimStartAndEnd().ToLower();
		}

		bool ParseBool(const FString& Text, bool& Out)
		{
			const FString Word = Lower(Text);
			if (Word == TEXT("true") || Word == TEXT("1") || Word == TEXT("yes") || Word == TEXT("on")) { Out = true; return true; }
			if (Word == TEXT("false") || Word == TEXT("0") || Word == TEXT("no") || Word == TEXT("off")) { Out = false; return true; }
			return false;
		}

		FString Number(const double Value)
		{
			FString Text = FString::Printf(TEXT("%.4f"), Value);
			while (Text.Contains(TEXT(".")) && (Text.EndsWith(TEXT("0")) || Text.EndsWith(TEXT(".")))) Text.LeftChopInline(1);
			return Text;
		}

		const UEnum* EnumOf(const FProperty& Property)
		{
			if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(&Property)) return EnumProperty->GetEnum();
			if (const FByteProperty* Byte = CastField<FByteProperty>(&Property)) return Byte->Enum;
			return nullptr;
		}

		/** The enum's values a person can pick: not the _MAX, not the hidden (deprecated) ones. */
		TArray<int32> PickableValues(const UEnum& Enum)
		{
			TArray<int32> Out;
			for (int32 Index = 0; Index < Enum.NumEnums() - (Enum.ContainsExistingMax() ? 1 : 0); ++Index)
			{
#if WITH_METADATA
				if (Enum.HasMetaData(TEXT("Hidden"), Index)) continue;
#endif
				Out.Add(Index);
			}
			return Out;
		}

		bool ParseNumbers(const FString& Text, const int32 Count, TArray<double>& Out)
		{
			TArray<FString> Parts;
			Text.ParseIntoArray(Parts, TEXT(","), /*InCullEmpty=*/false);
			if (Parts.Num() != Count) return false;
			Out.Reset();
			for (const FString& Part : Parts)
			{
				double Value = 0.0;
				if (!LexTryParseString(Value, *Part.TrimStartAndEnd()) || !FMath::IsFinite(Value)) return false;
				Out.Add(Value);
			}
			return true;
		}

		FString TypeName(const FProperty& Property)
		{
			if (const UEnum* Enum = EnumOf(Property))
			{
				TArray<FString> Names;
				for (const int32 Index : PickableValues(*Enum)) Names.Add(Enum->GetAuthoredNameStringByIndex(Index));
				return FString::Printf(TEXT("one of %s"), *FString::Join(Names, TEXT(", ")));
			}
			if (Property.IsA<FBoolProperty>()) return TEXT("bool");
			if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property)) return Numeric->IsFloatingPoint() ? TEXT("float") : TEXT("integer");
			if (Property.IsA<FNameProperty>()) return TEXT("name");
			if (Property.IsA<FStrProperty>() || Property.IsA<FTextProperty>()) return TEXT("text");
			if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(&Property))
			{
				return FString::Printf(TEXT("%s asset path or None"), Object->PropertyClass ? *Object->PropertyClass->GetName() : TEXT("object"));
			}
			if (const FArrayProperty* Array = CastField<FArrayProperty>(&Property))
			{
				return FString::Printf(TEXT("list of %s, \"a,b\""), *TypeName(*Array->Inner));
			}
			if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
			{
				if (Struct->Struct == TBaseStructure<FVector>::Get()) return TEXT("vector \"x,y,z\"");
				if (Struct->Struct == TBaseStructure<FRotator>::Get()) return TEXT("rotator \"pitch,yaw,roll\"");
				return FString::Printf(TEXT("%s in Unreal's text form"), Struct->Struct ? *Struct->Struct->GetName() : TEXT("struct"));
			}
			return TEXT("value in Unreal's text form");
		}
	}

	const TArray<FKindInfo>& Kinds()
	{
		static const TArray<FKindInfo> All = {
			{TEXT("window.add"), TEXT("A Motion Warping window from StartSeconds to EndSeconds on a montage or sequence, warping to WarpTargetName with Modifier (skew_warp by default) and its Params, on Track.")},
			{TEXT("window.set"), TEXT("Change a window picked by Index or WarpTargetName: StartSeconds, EndSeconds (-1 keeps), Modifier (a new one, keeping its warp target) and Params (WarpTargetName=New renames it).")},
			{TEXT("window.remove"), TEXT("Remove a window picked by Index or WarpTargetName.")},
			{TEXT("component.add"), TEXT("Add a Motion Warping component (ComponentName, MotionWarping by default) to a character Blueprint's construction script, with Params, and compile it; refused when it has one.")},
			{TEXT("component.set"), TEXT("Change the character Blueprint's own Motion Warping component's settings with Params, and compile it.")}};
		return All;
	}

	bool IsKind(const FString& Kind)
	{
		const FString Wanted = Lower(Kind);
		return Kinds().ContainsByPredicate([&Wanted](const FKindInfo& Each) { return Wanted == Each.Kind; });
	}

	FString KindList()
	{
		TArray<FString> Names;
		for (const FKindInfo& Each : Kinds()) Names.Add(Each.Kind);
		return FString::Join(Names, TEXT(", "));
	}

	const TArray<FModifierInfo>& Modifiers()
	{
		static const TArray<FModifierInfo> All = {
			{TEXT("skew_warp"), TEXT("Bends the root motion's translation (skewed toward the target) and rotation so the character arrives at the warp target when the window ends. The default, and the right one for vaults, mantles and attack lunges."),
				URootMotionModifier_SkewWarp::StaticClass(), true, false},
			{TEXT("scale"), TEXT("Scales the root motion's translation by Scale (x,y,z) during the window; it takes no warp target."),
				URootMotionModifier_Scale::StaticClass(), false, false},
			{TEXT("adjustment_blend"), TEXT("Experimental: warps with adjustment blending, which can also move IK bones (bWarpIKBones, IKBones). Epic hides it from the notify's modifier list."),
				URootMotionModifier_AdjustmentBlendWarp::StaticClass(), true, true},
			{TEXT("precomputed_warp"), TEXT("Experimental: works out the whole path to the target on the window's first frame, then follows it; only for targets that do not move."),
				URootMotionModifier_PrecomputedWarp::StaticClass(), true, true}};
		return All;
	}

	const FModifierInfo* FindModifier(const FString& Word)
	{
		const FString Wanted = Lower(Word).Replace(TEXT(" "), TEXT("_"));
		return Modifiers().FindByPredicate([&Wanted](const FModifierInfo& Each) { return Wanted == Each.Word; });
	}

	FString ModifierList()
	{
		TArray<FString> Names;
		for (const FModifierInfo& Each : Modifiers()) Names.Add(Each.Word);
		return FString::Join(Names, TEXT(", "));
	}

	FString ModifierWord(const UClass* Class)
	{
		if (!Class) return TEXT("none");
		const FModifierInfo* Found = Modifiers().FindByPredicate([Class](const FModifierInfo& Each) { return Each.Class == Class; });
		return Found ? FString(Found->Word) : Class->GetName();
	}

	bool IsSettable(const FProperty& Property)
	{
		if (!Property.HasAnyPropertyFlags(CPF_Edit) || Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | CPF_DisableEditOnTemplate)) return false;
		if (Property.IsA<FDelegateProperty>() || Property.IsA<FMulticastDelegateProperty>() || Property.IsA<FMapProperty>() || Property.IsA<FSetProperty>()) return false;
		// Instanced objects (asset user data, sub-objects) are made, not typed in.
		if (Property.HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference)) return false;
		return Property.ArrayDim == 1;
	}

	TArray<FProperty*> Settable(const UClass& Class, const bool bOwnOnly)
	{
		TArray<FProperty*> Out;
		for (TFieldIterator<FProperty> It(&Class, bOwnOnly ? EFieldIteratorFlags::ExcludeSuper : EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			if (IsSettable(**It)) Out.Add(*It);
		}
		return Out;
	}

	FProperty* FindSetting(const UClass& Class, const FString& Name)
	{
		const FString Wanted = Name.TrimStartAndEnd();
		for (FProperty* Property : Settable(Class, /*bOwnOnly=*/false))
		{
			if (Property->GetName().Equals(Wanted, ESearchCase::IgnoreCase)) return Property;
		}
#if WITH_EDITOR
		for (FProperty* Property : Settable(Class, /*bOwnOnly=*/false))
		{
			if (Property->GetDisplayNameText().ToString().Replace(TEXT(" "), TEXT("")).Equals(Wanted.Replace(TEXT(" "), TEXT("")), ESearchCase::IgnoreCase)) return Property;
		}
#endif
		return nullptr;
	}

	FString ValueText(const FProperty& Property, const void* Container)
	{
		const void* Value = Property.ContainerPtrToValuePtr<void>(Container);
		if (const UEnum* Enum = EnumOf(Property))
		{
			const FNumericProperty* Underlying = Property.IsA<FEnumProperty>()
				? CastField<FEnumProperty>(&Property)->GetUnderlyingProperty() : CastField<FNumericProperty>(&Property);
			const int64 Raw = Underlying ? Underlying->GetSignedIntPropertyValue(Value) : 0;
			const int32 Index = Enum->GetIndexByValue(Raw);
			return Index == INDEX_NONE ? LexToString(Raw) : Enum->GetAuthoredNameStringByIndex(Index);
		}
		if (const FBoolProperty* Bool = CastField<FBoolProperty>(&Property))
		{
			return Bool->GetPropertyValue(Value) ? TEXT("true") : TEXT("false");
		}
		if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
		{
			return Numeric->IsFloatingPoint() ? Number(Numeric->GetFloatingPointPropertyValue(Value)) : LexToString(Numeric->GetSignedIntPropertyValue(Value));
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(&Property))
		{
			if (Struct->Struct == TBaseStructure<FVector>::Get())
			{
				const FVector& Vector = *static_cast<const FVector*>(Value);
				return FString::Printf(TEXT("%s,%s,%s"), *Number(Vector.X), *Number(Vector.Y), *Number(Vector.Z));
			}
			if (Struct->Struct == TBaseStructure<FRotator>::Get())
			{
				const FRotator& Rotator = *static_cast<const FRotator*>(Value);
				return FString::Printf(TEXT("%s,%s,%s"), *Number(Rotator.Pitch), *Number(Rotator.Yaw), *Number(Rotator.Roll));
			}
		}
		FString Text;
		Property.ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_None);
		return Text.Len() > 300 ? Text.Left(300) + TEXT("...") : Text;
	}

	TArray<FString> SettingsOf(const UObject& Object, const bool bOwnOnly)
	{
		TArray<FString> Out;
		for (const FProperty* Property : Settable(*Object.GetClass(), bOwnOnly))
		{
			Out.Add(Property->GetName() + TEXT("=") + ValueText(*Property, &Object));
		}
		return Out;
	}

	TArray<FString> DescribeSettings(const UClass& Class, const bool bOwnOnly)
	{
		TArray<FString> Out;
		const UObject* Defaults = Class.GetDefaultObject();
		for (const FProperty* Property : Settable(Class, bOwnOnly))
		{
			Out.Add(FString::Printf(TEXT("%s: %s, default %s"), *Property->GetName(), *TypeName(*Property), Defaults ? *ValueText(*Property, Defaults) : TEXT("?")));
		}
		return Out;
	}

	bool SplitParams(const FString& Text, TArray<TPair<FString, FString>>& Out, FString& OutError)
	{
		Out.Reset();
		TArray<FString> Pairs;
		Text.ParseIntoArray(Pairs, TEXT(";"), /*InCullEmpty=*/true);
		for (const FString& Raw : Pairs)
		{
			const FString Pair = Raw.TrimStartAndEnd();
			if (Pair.IsEmpty()) continue;
			FString Name;
			FString Value;
			if (!Pair.Split(TEXT("="), &Name, &Value) || Name.TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is not Name=Value; Params are \"Name=Value; Name=Value\"."), *Pair.Left(80));
				return false;
			}
			Name.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			if (Out.ContainsByPredicate([&Name](const TPair<FString, FString>& Each) { return Each.Key.Equals(Name, ESearchCase::IgnoreCase); }))
			{
				OutError = FString::Printf(TEXT("%s is given twice."), *Name);
				return false;
			}
			Out.Emplace(Name, Value);
		}
		return true;
	}

	bool CoerceValue(const FProperty& Property, const FString& Text, FString& OutImport, FString& OutError)
	{
		OutImport = Text.TrimStartAndEnd();
		if (const UEnum* Enum = EnumOf(Property))
		{
			FString Wanted = OutImport;
			int32 Colons = INDEX_NONE;
			if (Wanted.FindLastChar(TEXT(':'), Colons)) Wanted = Wanted.Mid(Colons + 1);
			TArray<FString> Names;
			for (const int32 Index : PickableValues(*Enum))
			{
				const FString Authored = Enum->GetAuthoredNameStringByIndex(Index);
				Names.Add(Authored);
				if (Authored.Equals(Wanted, ESearchCase::IgnoreCase) || Enum->GetNameStringByIndex(Index).Equals(OutImport, ESearchCase::IgnoreCase)
					|| Enum->GetDisplayNameTextByIndex(Index).ToString().Replace(TEXT(" "), TEXT("")).Equals(Wanted.Replace(TEXT(" "), TEXT("")), ESearchCase::IgnoreCase))
				{
					OutImport = Enum->GetNameStringByIndex(Index);
					return true;
				}
			}
			OutError = FString::Printf(TEXT("takes one of: %s"), *FString::Join(Names, TEXT(", ")));
			return false;
		}
		if (Property.IsA<FBoolProperty>())
		{
			bool bValue = false;
			if (!ParseBool(OutImport, bValue))
			{
				OutError = TEXT("takes true or false");
				return false;
			}
			OutImport = bValue ? TEXT("True") : TEXT("False");
			return true;
		}
		if (const FStructProperty* Struct = CastField<FStructProperty>(&Property); Struct && !OutImport.StartsWith(TEXT("(")))
		{
			TArray<double> Values;
			if (Struct->Struct == TBaseStructure<FVector>::Get())
			{
				if (!ParseNumbers(OutImport, 3, Values))
				{
					OutError = TEXT("takes a vector \"x,y,z\"");
					return false;
				}
				OutImport = FString::Printf(TEXT("(X=%s,Y=%s,Z=%s)"), *Number(Values[0]), *Number(Values[1]), *Number(Values[2]));
			}
			else if (Struct->Struct == TBaseStructure<FRotator>::Get())
			{
				if (!ParseNumbers(OutImport, 3, Values))
				{
					OutError = TEXT("takes a rotator \"pitch,yaw,roll\"");
					return false;
				}
				OutImport = FString::Printf(TEXT("(Pitch=%s,Yaw=%s,Roll=%s)"), *Number(Values[0]), *Number(Values[1]), *Number(Values[2]));
			}
		}
		if (Property.IsA<FArrayProperty>() && !OutImport.StartsWith(TEXT("(")))
		{
			OutImport = TEXT("(") + OutImport + TEXT(")");
		}
		if (Property.IsA<FObjectPropertyBase>())
		{
			if (OutImport.IsEmpty() || OutImport.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				OutImport = TEXT("None");
			}
			else
			{
				FString Reason;
				if (!FHyperAIStudioNames::IsObjectPath(OutImport, TEXT("The value"), &Reason))
				{
					OutError = Reason;
					return false;
				}
				const FString ObjectPath = OutImport.Contains(TEXT(".")) ? OutImport : OutImport + TEXT(".") + FPackageName::GetShortName(OutImport);
				const UClass* Wanted = CastField<FObjectPropertyBase>(&Property)->PropertyClass;
				UObject* Found = FindObject<UObject>(nullptr, *ObjectPath);
				// A path with nothing on disk is never loaded: the failed load would leave an empty package behind.
				if (!Found && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ObjectPath)))
				{
					Found = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
				}
				if (!Found || (Wanted && !Found->IsA(Wanted)))
				{
					OutError = FString::Printf(TEXT("needs a %s, and %s is not one (or is not there)"), Wanted ? *Wanted->GetName() : TEXT("asset"), *OutImport);
					return false;
				}
				OutImport = Found->GetPathName();
			}
		}
		// The import makes names of the text, and loads an asset it names (making the package when nothing is there).
		if (!FHyperAIStudioNames::CanImportText(Property, OutImport, TEXT("The value"), &OutError)) return false;
		void* Scratch = FMemory::Malloc(Property.GetSize(), Property.GetMinAlignment());
		Property.InitializeValue(Scratch);
		FStringOutputDevice Errors;
		const TCHAR* End = Property.ImportText_Direct(*OutImport, Scratch, nullptr, PPF_None, &Errors);
		bool bOk = End != nullptr && FString(End).TrimStartAndEnd().IsEmpty() && Errors.IsEmpty();
		if (bOk)
		{
			if (const FNumericProperty* Numeric = CastField<FNumericProperty>(&Property))
			{
				const double Value = Numeric->IsFloatingPoint() ? Numeric->GetFloatingPointPropertyValue(Scratch)
					: static_cast<double>(Numeric->GetSignedIntPropertyValue(Scratch));
#if WITH_EDITOR
				const FString Min = Property.GetMetaData(TEXT("ClampMin"));
				const FString Max = Property.GetMetaData(TEXT("ClampMax"));
#else
				const FString Min, Max;
#endif
				if (!FMath::IsFinite(Value) || (!Min.IsEmpty() && Value < FCString::Atod(*Min)) || (!Max.IsEmpty() && Value > FCString::Atod(*Max)))
				{
					bOk = false;
					OutError = Min.IsEmpty() && Max.IsEmpty() ? FString(TEXT("needs a finite number"))
						: FString::Printf(TEXT("needs a number from %s to %s"), Min.IsEmpty() ? TEXT("any") : *Min, Max.IsEmpty() ? TEXT("any") : *Max);
				}
			}
		}
		else
		{
			OutError = FString::Printf(TEXT("does not take '%s' (%s)"), *OutImport.Left(64), *TypeName(Property));
		}
		Property.DestroyValue(Scratch);
		FMemory::Free(Scratch);
		return bOk;
	}

	bool ParseSettings(const UClass& Class, const FString& Params, TArray<FSetting>& Out, FString& OutError)
	{
		TArray<TPair<FString, FString>> Pairs;
		if (!SplitParams(Params, Pairs, OutError)) return false;
		for (const TPair<FString, FString>& Pair : Pairs)
		{
			FProperty* Property = FindSetting(Class, Pair.Key);
			if (!Property)
			{
				TArray<FString> Names;
				for (const FProperty* Each : Settable(Class, /*bOwnOnly=*/false)) Names.Add(Each->GetName());
				OutError = FString::Printf(TEXT("%s takes no setting '%s'; it takes %s."), *Class.GetName(), *Pair.Key, Names.IsEmpty() ? TEXT("none") : *FString::Join(Names, TEXT(", ")));
				return false;
			}
			FSetting Setting;
			Setting.Property = Property;
			FString Why;
			if (!CoerceValue(*Property, Pair.Value, Setting.Import, Why))
			{
				OutError = FString::Printf(TEXT("%s=%s: %s %s."), *Property->GetName(), *Pair.Value.Left(80), *Property->GetName(), *Why);
				return false;
			}
			Out.RemoveAll([Property](const FSetting& Each) { return Each.Property == Property; });
			Out.Add(Setting);
		}
		return true;
	}

	bool WriteSettings(UObject& Object, const TArray<FSetting>& Settings, FString& OutError)
	{
		for (const FSetting& Setting : Settings)
		{
			if (!Setting.Property || !Object.GetClass()->IsChildOf(Setting.Property->GetOwnerClass()))
			{
				OutError = FString::Printf(TEXT("%s has no setting %s."), *Object.GetClass()->GetName(), Setting.Property ? *Setting.Property->GetName() : TEXT("?"));
				return false;
			}
			FStringOutputDevice Errors;
			const TCHAR* End = Setting.Property->ImportText_Direct(*Setting.Import, Setting.Property->ContainerPtrToValuePtr<void>(&Object), &Object, PPF_None, &Errors);
			if (!End || !Errors.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s did not take '%s'."), *Setting.Property->GetName(), *Setting.Import.Left(80));
				return false;
			}
		}
		return true;
	}

	FString AssetKindName(const EAssetKind Kind)
	{
		switch (Kind)
		{
		case EAssetKind::Sequence: return TEXT("sequence");
		case EAssetKind::Character: return TEXT("character_blueprint");
		default: return TEXT("montage");
		}
	}

	TArray<FWindowRef> WindowsOf(const UAnimSequenceBase& Asset)
	{
		TArray<FWindowRef> Out;
		for (const FAnimNotifyEvent& Event : Asset.Notifies)
		{
			UAnimNotifyState_MotionWarping* State = Cast<UAnimNotifyState_MotionWarping>(Event.NotifyStateClass);
			if (!State) continue;
			FWindowRef& Window = Out.AddDefaulted_GetRef();
			Window.State = State;
			Window.Start = Event.GetTime();
			Window.End = Event.GetTime() + Event.GetDuration();
			Window.TrackIndex = Event.TrackIndex;
		}
		Out.StableSort([](const FWindowRef& A, const FWindowRef& B) { return A.Start < B.Start; });
		return Out;
	}

	int32 FindEvent(const UAnimSequenceBase& Asset, const UAnimNotifyState_MotionWarping* State)
	{
		return Asset.Notifies.IndexOfByPredicate([State](const FAnimNotifyEvent& Event) { return State && Event.NotifyStateClass == State; });
	}

	FHyperAIMotionWarpingWindow DescribeWindow(const UAnimSequenceBase& Asset, const FWindowRef& Window, const int32 Index)
	{
		FHyperAIMotionWarpingWindow Out;
		Out.Index = Index;
#if WITH_EDITORONLY_DATA
		Out.Track = Asset.AnimNotifyTracks.IsValidIndex(Window.TrackIndex) ? Asset.AnimNotifyTracks[Window.TrackIndex].TrackName.ToString() : FString::FromInt(Window.TrackIndex);
#else
		Out.Track = FString::FromInt(Window.TrackIndex);
#endif
		Out.StartSeconds = Window.Start;
		Out.EndSeconds = Window.End;
		const URootMotionModifier* Modifier = Window.State ? Window.State->RootMotionModifier.Get() : nullptr;
		Out.Modifier = ModifierWord(Modifier ? Modifier->GetClass() : nullptr);
		Out.ModifierClass = Modifier ? Modifier->GetClass()->GetName() : FString();
		if (const URootMotionModifier_Warp* Warp = Cast<URootMotionModifier_Warp>(Modifier))
		{
			Out.WarpTargetName = Warp->WarpTargetName.IsNone() ? FString() : Warp->WarpTargetName.ToString();
		}
		if (Modifier) Out.Settings = SettingsOf(*Modifier, /*bOwnOnly=*/false);
		return Out;
	}

	FAnimView ViewAnim(const UAnimSequenceBase& Asset)
	{
		FAnimView View;
		View.Length = Asset.GetPlayLength();
		const TArray<FWindowRef> Windows = WindowsOf(Asset);
		for (int32 Index = 0; Index < Windows.Num(); ++Index)
		{
			FHyperAIMotionWarpingWindow Window = DescribeWindow(Asset, Windows[Index], Index);
			const FString Label = FString::Printf(TEXT("Window %d%s"), Index, Window.WarpTargetName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Window.WarpTargetName));
			if (Window.StartSeconds < -KINDA_SMALL_NUMBER || Window.EndSeconds > View.Length + 0.001f)
			{
				View.Warnings.Add(FString::Printf(TEXT("%s runs from %.3f s to %.3f s, outside the asset's %.3f s; the part past the end never plays."), *Label, Window.StartSeconds,
					Window.EndSeconds, View.Length));
			}
			if (Window.Modifier == TEXT("none"))
			{
				View.Warnings.Add(FString::Printf(TEXT("%s has no root motion modifier, so it does nothing; window.set it a Modifier."), *Label));
			}
			else if (Cast<URootMotionModifier_Warp>(Windows[Index].State->RootMotionModifier) && Window.WarpTargetName.IsEmpty())
			{
				View.Warnings.Add(FString::Printf(TEXT("%s names no warp target, so it never warps; set Params WarpTargetName=Name."), *Label));
			}
			if (!Window.WarpTargetName.IsEmpty()) View.WarpTargets.AddUnique(Window.WarpTargetName);
			View.Windows.Add(MoveTemp(Window));
		}
		View.WarpTargets.Sort();
		if (const UAnimMontage* Montage = Cast<UAnimMontage>(&Asset))
		{
			TArray<FString> Without;
			for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
			{
				for (const FAnimSegment& Segment : Slot.AnimTrack.AnimSegments)
				{
					if (const UAnimSequence* Sequence = Cast<UAnimSequence>(Segment.GetAnimReference()))
					{
						if (Sequence->bEnableRootMotion) View.bRootMotion = true;
						else Without.AddUnique(Sequence->GetName());
					}
				}
			}
			if (!View.bRootMotion)
			{
				View.Warnings.Add(FString::Printf(TEXT("%s plays no sequence with root motion on%s: warping bends root motion, so its windows move nothing. Turn it on with hyper_animation_apply_plan sequence.set_property root_motion."),
					*Asset.GetName(), Without.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *FString::Join(Without, TEXT(", ")))));
			}
		}
		else if (const UAnimSequence* Sequence = Cast<UAnimSequence>(&Asset))
		{
			View.bRootMotion = Sequence->bEnableRootMotion;
			if (!View.bRootMotion)
			{
				View.Warnings.Add(FString::Printf(TEXT("%s has root motion off: warping bends root motion, so its windows move nothing. Turn it on with hyper_animation_apply_plan sequence.set_property root_motion."),
					*Asset.GetName()));
			}
			if (!View.Windows.IsEmpty())
			{
				View.Warnings.Add(TEXT("Windows on a sequence warp only when it plays inside a montage and the character's Motion Warping component has bSearchForWindowsInAnimsWithinMontages on (component.set)."));
			}
		}
		return View;
	}

	bool IsCharacterBlueprint(const UBlueprint& Blueprint)
	{
		return Blueprint.ParentClass && Blueprint.ParentClass->IsChildOf(ACharacter::StaticClass()) && Blueprint.SimpleConstructionScript;
	}

	bool FindComponent(const UBlueprint& Blueprint, FComponentFound& Out)
	{
		Out = FComponentFound();
		if (Blueprint.SimpleConstructionScript)
		{
			for (USCS_Node* Node : Blueprint.SimpleConstructionScript->GetAllNodes())
			{
				if (const UMotionWarpingComponent* Template = Node ? Cast<UMotionWarpingComponent>(Node->ComponentTemplate) : nullptr)
				{
					Out.Template = Template;
					Out.OwnNode = Node;
					Out.Name = Node->GetVariableName().ToString();
					Out.Source = TEXT("this Blueprint");
					return true;
				}
			}
		}
		for (const UClass* Class = Blueprint.ParentClass; Class; Class = Class->GetSuperClass())
		{
			if (const UBlueprintGeneratedClass* Generated = Cast<UBlueprintGeneratedClass>(Class))
			{
				if (!Generated->SimpleConstructionScript) continue;
				for (USCS_Node* Node : Generated->SimpleConstructionScript->GetAllNodes())
				{
					if (const UMotionWarpingComponent* Template = Node ? Cast<UMotionWarpingComponent>(Node->ComponentTemplate) : nullptr)
					{
						Out.Template = Template;
						Out.Name = Node->GetVariableName().ToString();
						Out.Source = Generated->ClassGeneratedBy ? Generated->ClassGeneratedBy->GetPathName() : Generated->GetPathName();
						return true;
					}
				}
				continue;
			}
			// The first native class: its defaults hold every native component it and its parents make.
			TArray<UObject*> Subobjects;
			if (UObject* Defaults = Class->GetDefaultObject(/*bCreateIfNeeded=*/false)) Defaults->GetDefaultSubobjects(Subobjects);
			for (UObject* Subobject : Subobjects)
			{
				if (const UMotionWarpingComponent* Template = Cast<UMotionWarpingComponent>(Subobject))
				{
					Out.Template = Template;
					Out.Name = Template->GetName();
					Out.Source = FString::Printf(TEXT("native class %s"), *Class->GetName());
					return true;
				}
			}
			break;
		}
		return false;
	}

	FString FStep::Describe() const
	{
		return FString::Printf(TEXT("Operation %d (%s)"), Index, *Kind);
	}

	UObject* LoadAsset(const FString& Path, FString& OutError)
	{
		const FString Trimmed = Path.TrimStartAndEnd();
		if (!FHyperAIStudioNames::IsObjectPath(Trimmed, TEXT("Asset"), &OutError)) return nullptr;
		const FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
		UObject* Found = FindObject<UObject>(nullptr, *ObjectPath);
		// A path with nothing on disk is never loaded: the failed load would leave an empty package behind.
		if (!Found && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ObjectPath)))
		{
			Found = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		}
		if (!IsValid(Found))
		{
			OutError = FString::Printf(TEXT("%s is not there: no montage, animation sequence or Blueprint at that path."), *Trimmed);
			return nullptr;
		}
		return Found;
	}

	namespace
	{
		bool CheckTimes(const UAnimSequenceBase& Asset, const float Start, const float End, FString& OutError)
		{
			const float Length = Asset.GetPlayLength();
			if (Start < 0.f || End <= Start)
			{
				OutError = FString::Printf(TEXT("A window needs 0 <= StartSeconds < EndSeconds; it was %s to %s."), *Number(Start), *Number(End));
				return false;
			}
			if (End > Length + 0.001f)
			{
				OutError = FString::Printf(TEXT("%s is %s s long; a window to %s s runs past its end."), *Asset.GetName(), *Number(Length), *Number(End));
				return false;
			}
			return true;
		}

		bool IsVariableName(const FString& Name)
		{
			if (Name.IsEmpty() || Name.Len() > 100 || FChar::IsDigit(Name[0])) return false;
			for (const TCHAR Char : Name)
			{
				if (!FChar::IsAlnum(Char) && Char != TEXT('_')) return false;
			}
			return true;
		}
	}

	bool Check(const FHyperAIMotionWarpingPlanRequest& Request, FPlan& OutPlan, FString& OutError)
	{
		OutPlan = FPlan();
		const FString Path = Request.Asset.TrimStartAndEnd();
		if (Path.IsEmpty())
		{
			OutError = TEXT("Give Asset, the montage, animation sequence or character Blueprint to change, as /Game/Folder/AM_Name.");
			return false;
		}
		if (!Path.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(TEXT("%s is not project content: a plan changes montages, sequences and character Blueprints under /Game only, never Engine or plugin content."), *Path);
			return false;
		}
		UObject* Asset = LoadAsset(Path, OutError);
		if (!Asset) return false;
		OutPlan.Asset = Asset;
		OutPlan.AssetPath = Asset->GetPathName();
		UAnimSequenceBase* Anim = nullptr;
		UBlueprint* Blueprint = nullptr;
		if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
		{
			OutPlan.Kind = EAssetKind::Montage;
			Anim = Montage;
		}
		else if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset))
		{
			OutPlan.Kind = EAssetKind::Sequence;
			Anim = Sequence;
		}
		else if ((Blueprint = Cast<UBlueprint>(Asset)) != nullptr)
		{
			if (!IsCharacterBlueprint(*Blueprint))
			{
				OutError = FString::Printf(TEXT("%s is a Blueprint whose parent, %s, is not a Character; the Motion Warping component goes on character Blueprints."), *Path,
					Blueprint->ParentClass ? *Blueprint->ParentClass->GetName() : TEXT("none"));
				return false;
			}
			OutPlan.Kind = EAssetKind::Character;
		}
		else
		{
			OutError = FString::Printf(TEXT("%s is a %s, not a montage, animation sequence or character Blueprint."), *Path, *Asset->GetClass()->GetName());
			return false;
		}
		if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
			return false;
		}
		const TArray<FWindowRef> Before = Anim ? WindowsOf(*Anim) : TArray<FWindowRef>();
		for (const FWindowRef& Window : Before)
		{
			if (const URootMotionModifier_Warp* Warp = Cast<URootMotionModifier_Warp>(Window.State->RootMotionModifier))
			{
				if (!Warp->WarpTargetName.IsNone()) OutPlan.TargetsBefore.AddUnique(Warp->WarpTargetName.ToString());
			}
		}
		TSet<const UAnimNotifyState_MotionWarping*> Removed;
		FComponentFound Existing;
		const bool bHadComponent = Blueprint && FindComponent(*Blueprint, Existing);
		bool bComponentAdded = false;
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAIMotionWarpingOp& Op = Request.Operations[Index];
			FStep& Step = OutPlan.Steps.AddDefaulted_GetRef();
			Step.Index = Index;
			Step.Op = Op;
			Step.Kind = Lower(Op.Kind);
			auto Fail = [&OutError, &Step](const FString& Message)
			{
				OutError = FString::Printf(TEXT("%s: %s Nothing was changed."), *Step.Describe(), *Message);
				return false;
			};
			if (!IsKind(Step.Kind)) return Fail(FString::Printf(TEXT("'%s' is not an op kind; the kinds are %s."), *Op.Kind, *KindList()));
			const bool bWindowOp = Step.Kind.StartsWith(TEXT("window."));
			if (bWindowOp && !Anim) return Fail(TEXT("window ops take a montage or animation sequence; a character Blueprint takes component.add and component.set."));
			if (!bWindowOp && !Blueprint) return Fail(TEXT("component ops take a character Blueprint; a montage or sequence takes window.add, window.set and window.remove."));
			FString Why;
			if (!FHyperAIStudioNames::IsName(Op.Track, TEXT("Track"), &Why) || !FHyperAIStudioNames::IsName(Op.WarpTargetName, TEXT("WarpTargetName"), &Why)) return Fail(Why);

			if (Step.Kind == TEXT("window.add"))
			{
				const FModifierInfo* Modifier = FindModifier(Op.Modifier.TrimStartAndEnd().IsEmpty() ? FString(TEXT("skew_warp")) : Op.Modifier);
				if (!Modifier || !Modifier->Class) return Fail(FString::Printf(TEXT("'%s' is not a modifier; the modifiers are %s."), *Op.Modifier, *ModifierList()));
				Step.ModifierClass = Modifier->Class;
				if (Op.StartSeconds < 0.0 || Op.EndSeconds < 0.0) return Fail(TEXT("window.add needs StartSeconds and EndSeconds, in seconds from the asset's start."));
				Step.Start = static_cast<float>(Op.StartSeconds);
				Step.End = static_cast<float>(Op.EndSeconds);
				Step.bTimes = true;
				if (!CheckTimes(*Anim, Step.Start, Step.End, Why)) return Fail(Why);
				const FString Target = Op.WarpTargetName.TrimStartAndEnd();
				if (!Target.IsEmpty())
				{
					if (!Modifier->bWarp) return Fail(FString::Printf(TEXT("%s takes no warp target; leave WarpTargetName out, or use skew_warp."), Modifier->Word));
					FSetting Name;
					Name.Property = FindSetting(*Modifier->Class, TEXT("WarpTargetName"));
					if (!Name.Property || !CoerceValue(*Name.Property, Target, Name.Import, Why)) return Fail(FString::Printf(TEXT("WarpTargetName '%s' %s."), *Target, *Why));
					Step.Settings.Add(Name);
				}
				if (!ParseSettings(*Modifier->Class, Op.Params, Step.Settings, Why)) return Fail(Why);
				if (Modifier->bWarp && !Step.Settings.ContainsByPredicate([](const FSetting& Each) { return Each.Property->GetFName() == TEXT("WarpTargetName") && Each.Import != TEXT("None") && !Each.Import.IsEmpty(); }))
				{
					return Fail(FString::Printf(TEXT("%s needs WarpTargetName, the name game code gives the target (AddOrUpdateWarpTargetFromLocation and the like)."), Modifier->Word));
				}
				continue;
			}

			if (Step.Kind == TEXT("window.set") || Step.Kind == TEXT("window.remove"))
			{
				const FString Target = Op.WarpTargetName.TrimStartAndEnd();
				if (Op.Index >= 0 && !Target.IsEmpty()) return Fail(TEXT("Pick the window by Index or by WarpTargetName, not both; rename it with Params WarpTargetName=NewName."));
				if (Op.Index >= 0)
				{
					if (!Before.IsValidIndex(Op.Index))
					{
						return Fail(Before.IsEmpty() ? FString::Printf(TEXT("%s has no Motion Warping window."), *Anim->GetName())
							: FString::Printf(TEXT("%s has %d window(s): Index 0 to %d, as inspect lists them."), *Anim->GetName(), Before.Num(), Before.Num() - 1));
					}
					Step.Window = Before[Op.Index].State;
				}
				else if (!Target.IsEmpty())
				{
					TArray<int32> Matches;
					for (int32 Each = 0; Each < Before.Num(); ++Each)
					{
						const URootMotionModifier_Warp* Warp = Cast<URootMotionModifier_Warp>(Before[Each].State->RootMotionModifier);
						if (Warp && Warp->WarpTargetName.ToString().Equals(Target, ESearchCase::IgnoreCase)) Matches.Add(Each);
					}
					if (Matches.IsEmpty())
					{
						return Fail(FString::Printf(TEXT("No window warps to %s; the windows warp to %s."), *Target,
							OutPlan.TargetsBefore.IsEmpty() ? TEXT("nothing") : *FString::Join(OutPlan.TargetsBefore, TEXT(", "))));
					}
					if (Matches.Num() > 1) return Fail(FString::Printf(TEXT("%d windows warp to %s; pick one by Index."), Matches.Num(), *Target));
					Step.Window = Before[Matches[0]].State;
				}
				else
				{
					return Fail(TEXT("Pick the window by Index (as inspect lists them) or by WarpTargetName."));
				}
				if (Removed.Contains(Step.Window)) return Fail(TEXT("That window is removed earlier in this plan."));
				if (Step.Kind == TEXT("window.remove"))
				{
					Removed.Add(Step.Window);
					continue;
				}
				const FWindowRef* Current = Before.FindByPredicate([&Step](const FWindowRef& Each) { return Each.State == Step.Window; });
				const UClass* SettingsClass = Step.Window->RootMotionModifier ? Step.Window->RootMotionModifier->GetClass() : nullptr;
				if (!Op.Modifier.TrimStartAndEnd().IsEmpty())
				{
					const FModifierInfo* Modifier = FindModifier(Op.Modifier);
					if (!Modifier || !Modifier->Class) return Fail(FString::Printf(TEXT("'%s' is not a modifier; the modifiers are %s."), *Op.Modifier, *ModifierList()));
					if (Modifier->Class != SettingsClass)
					{
						Step.ModifierClass = Modifier->Class;
						SettingsClass = Modifier->Class;
					}
				}
				if (!Op.Params.TrimStartAndEnd().IsEmpty())
				{
					if (!SettingsClass) return Fail(TEXT("The window has no modifier to take Params; give Modifier too."));
					if (!ParseSettings(*SettingsClass, Op.Params, Step.Settings, Why)) return Fail(Why);
				}
				Step.Start = Op.StartSeconds >= 0.0 ? static_cast<float>(Op.StartSeconds) : Current->Start;
				Step.End = Op.EndSeconds >= 0.0 ? static_cast<float>(Op.EndSeconds) : Current->End;
				Step.bTimes = Op.StartSeconds >= 0.0 || Op.EndSeconds >= 0.0;
				if (Step.bTimes && !CheckTimes(*Anim, Step.Start, Step.End, Why)) return Fail(Why);
				if (!Step.bTimes && !Step.ModifierClass && Step.Settings.IsEmpty())
				{
					return Fail(TEXT("window.set changes nothing; give StartSeconds, EndSeconds, Modifier or Params."));
				}
				continue;
			}

			if (Step.Kind == TEXT("component.add"))
			{
				if (bHadComponent || bComponentAdded)
				{
					return Fail(bHadComponent ? FString::Printf(TEXT("%s already has a Motion Warping component (%s, from %s); change it with component.set."), *Blueprint->GetName(), *Existing.Name, *Existing.Source)
						: FString(TEXT("The plan adds a Motion Warping component once.")));
				}
				Step.ComponentName = Op.ComponentName.TrimStartAndEnd().IsEmpty() ? FString(TEXT("MotionWarping")) : Op.ComponentName.TrimStartAndEnd();
				if (!IsVariableName(Step.ComponentName)) return Fail(FString::Printf(TEXT("ComponentName '%s' is not a variable name: letters, digits and _, not starting with a digit."), *Step.ComponentName.Left(100)));
				if (!ParseSettings(*UMotionWarpingComponent::StaticClass(), Op.Params, Step.Settings, Why)) return Fail(Why);
				bComponentAdded = true;
				continue;
			}

			// component.set
			if (!bComponentAdded && !(bHadComponent && Existing.OwnNode))
			{
				return Fail(bHadComponent ? FString::Printf(TEXT("The Motion Warping component %s comes from %s; change it there."), *Existing.Name, *Existing.Source)
					: FString::Printf(TEXT("%s has no Motion Warping component; add it with component.add first."), *Blueprint->GetName()));
			}
			if (Op.Params.TrimStartAndEnd().IsEmpty())
			{
				return Fail(FString::Printf(TEXT("component.set needs Params; the component takes %s."), *FString::Join(DescribeSettings(*UMotionWarpingComponent::StaticClass(), true), TEXT("; "))));
			}
			if (!ParseSettings(*UMotionWarpingComponent::StaticClass(), Op.Params, Step.Settings, Why)) return Fail(Why);
		}
		return true;
	}
}
