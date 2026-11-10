// Copyright Epic Games, Inc. All Rights Reserved.

#include "WidgetAnimationToolset.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/PanelSlot.h"
#include "Components/Widget.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetNameValidators.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MovieScene.h"
#include "MovieScenePossessable.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/ToolsetLibrary.h"
#include "WidgetBlueprint.h"
#include "WidgetBlueprintEditorUtils.h"

#define LOCTEXT_NAMESPACE "WidgetAnimationToolset"

namespace
{

/** Fill a FWidgetAnimationInfo from a UWidgetAnimation. */
FWidgetAnimationInfo MakeAnimationInfo(UWidgetAnimation* Animation)
{
	FWidgetAnimationInfo Info;
	if (!Animation)
	{
		return Info;
	}
	Info.Animation = Animation;
	Info.AnimationName = Animation->GetFName();
#if WITH_EDITOR
	Info.DisplayLabel = Animation->GetDisplayLabel();
#endif
	if (UMovieScene* MovieScene = Animation->GetMovieScene())
	{
		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		const FFrameNumber EndFrame = MovieScene->GetPlaybackRange().GetUpperBoundValue();
		const FFrameNumber StartFrame = MovieScene->GetPlaybackRange().GetLowerBoundValue();
		Info.LengthSeconds = static_cast<float>((EndFrame.Value - StartFrame.Value) / TickResolution.AsDecimal());
	}
	Info.BindingCount = Animation->GetBindings().Num();
	return Info;
}

/** Look for an existing animation on a Widget Blueprint by FName. */
UWidgetAnimation* FindAnimationByName(UWidgetBlueprint* WidgetBlueprint, FName AnimationName)
{
	if (!WidgetBlueprint)
	{
		return nullptr;
	}
	for (UWidgetAnimation* Anim : WidgetBlueprint->Animations)
	{
		if (Anim && Anim->GetFName() == AnimationName)
		{
			return Anim;
		}
	}
	return nullptr;
}

/**
 * Headless mirror of FWidgetBlueprintEditor's VerifyAnimationRename
 * (UMGEditor/Private/TabFactory/AnimationTabSummoner.cpp::VerifyAnimationRename).
 * Catches every collision the Designer rejects, except the function-name check,
 * which requires a live PreviewWidget the Designer maintains. Returns true if
 * the name is valid; on false, OutError is populated.
 */
bool VerifyAnimationName(UWidgetBlueprint* WidgetBlueprint, const FString& NewAnimationName, FString& OutError)
{
	if (NewAnimationName != SlugStringForValidName(NewAnimationName))
	{
		FString FoundInvalidCharacters;
		const FString InvalidCharacters = INVALID_OBJECTNAME_CHARACTERS;
		for (int32 Index = 0; Index < InvalidCharacters.Len(); ++Index)
		{
			const FString OneChar = InvalidCharacters.Mid(Index, 1);
			if (NewAnimationName.Contains(OneChar))
			{
				FoundInvalidCharacters += OneChar;
			}
		}
		OutError = FString::Printf(TEXT("The object name may not contain the following characters: %s"), *FoundInvalidCharacters);
		return false;
	}

	if (WidgetBlueprint && FindObject<UWidgetAnimation>(WidgetBlueprint, *NewAnimationName, EFindObjectFlags::ExactClass))
	{
		OutError = TEXT("An animation with this name already exists");
		return false;
	}

	const FName NewAnimationNameAsName(*NewAnimationName);
	if (WidgetBlueprint && WidgetBlueprint->WidgetTree
		&& WidgetBlueprint->WidgetTree->FindWidget<UWidget>(NewAnimationNameAsName) != nullptr)
	{
		OutError = TEXT("A widget with this name already exists");
		return false;
	}

	// BindWidgetAnim properties on the parent class take precedence over the name validator -
	// it's legal (and required) to name an animation after one of them so it can fulfil the
	// BindWidgetAnim contract at runtime.
	if (WidgetBlueprint && WidgetBlueprint->ParentClass)
	{
		const FProperty* Property = WidgetBlueprint->ParentClass->FindPropertyByName(NewAnimationNameAsName);
		if (Property && FWidgetBlueprintEditorUtils::IsBindWidgetAnimProperty(Property))
		{
			return true;
		}
	}

	FKismetNameValidator Validator(WidgetBlueprint);
	const EValidatorResult ValidationResult = Validator.IsValid(NewAnimationName);
	if (ValidationResult != EValidatorResult::Ok)
	{
		OutError = FKismetNameValidator::GetErrorString(NewAnimationName, ValidationResult);
		return false;
	}

	return true;
}

} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

FWidgetAnimationInfo UWidgetAnimationToolset::CreateWidgetAnimation(
	UWidgetBlueprint* WidgetBlueprint,
	FName AnimationName,
	float LengthSeconds)
{
	if (!WidgetBlueprint)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CreateWidgetAnimation: WidgetBlueprint is null."));
		return FWidgetAnimationInfo();
	}
	if (AnimationName.IsNone())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("CreateWidgetAnimation: AnimationName is empty."));
		return FWidgetAnimationInfo();
	}

	// Mirror VerifyAnimationRename's checks (UMGEditor/Private/TabFactory/AnimationTabSummoner.cpp).
	// The Designer also disallows animation names that collide with existing widgets, BindWidgetAnim
	// property names, or violate FKismetNameValidator - this matches that contract from headless code.
	{
		FString NameError;
		if (!VerifyAnimationName(WidgetBlueprint, AnimationName.ToString(), NameError))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("CreateWidgetAnimation: %s"), *NameError));
			return FWidgetAnimationInfo();
		}
	}

	const FScopedTransaction Transaction(LOCTEXT("CreateWidgetAnimation", "Create Widget Animation"));

	// Modify() BEFORE NewObject so the undo record encompasses the object creation.
	WidgetBlueprint->Modify();

	UWidgetAnimation* NewAnim = NewObject<UWidgetAnimation>(
		WidgetBlueprint, AnimationName, RF_Transactional | RF_Public);
	if (!NewAnim)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("CreateWidgetAnimation: Failed to create animation '%s'."),
			*AnimationName.ToString()));
		return FWidgetAnimationInfo();
	}

	NewAnim->MovieScene = NewObject<UMovieScene>(
		NewAnim, AnimationName, RF_Transactional | RF_Public);

#if WITH_EDITOR
	// Match the Designer (SUMGAnimationList::OnNewAnimationClicked - UMGEditor/Private/
	// TabFactory/AnimationTabSummoner.cpp:638): DisplayLabel defaults to the animation name.
	// SetWidgetAnimationDisplayLabel lets callers override it later.
	NewAnim->SetDisplayLabel(AnimationName.ToString());
#endif

	if (LengthSeconds > 0.0f)
	{
		// End-exclusive playback range, matching the Designer (UMGEditor/Private/TabFactory/
		// AnimationTabSummoner.cpp:648 stores OutFrame+1 as the upper bound).
		const FFrameRate TickResolution = NewAnim->MovieScene->GetTickResolution();
		const FFrameNumber EndFrame = (LengthSeconds * TickResolution).FloorToFrame();
		NewAnim->MovieScene->SetPlaybackRange(FFrameNumber(0), EndFrame.Value + 1, /*bAlwaysMarkDirty=*/false);
	}

	WidgetBlueprint->Animations.Add(NewAnim);
	// Animation names share the Widget Blueprint's variable namespace alongside named
	// widgets, so they need an entry in WidgetVariableNameToGuidMap. The Designer
	// calls OnVariableAdded the same way (see SWidgetAnimationListItem::OnNameTextCommited
	// in UMGEditor/Private/TabFactory/AnimationTabSummoner.cpp). Without it, the next
	// blueprint compile ensures in FWidgetBlueprintCompilerContext::ValidateAndFixUpVariableGuids.
	if (!WidgetBlueprint->WidgetVariableNameToGuidMap.Contains(NewAnim->GetFName()))
	{
		WidgetBlueprint->OnVariableAdded(NewAnim->GetFName());
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	return MakeAnimationInfo(NewAnim);
}

TArray<FWidgetAnimationInfo> UWidgetAnimationToolset::ListWidgetAnimations(UWidgetBlueprint* WidgetBlueprint)
{
	TArray<FWidgetAnimationInfo> Result;
	if (!WidgetBlueprint)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("ListWidgetAnimations: WidgetBlueprint is null."));
		return Result;
	}
	Result.Reserve(WidgetBlueprint->Animations.Num());
	for (UWidgetAnimation* Anim : WidgetBlueprint->Animations)
	{
		if (Anim)
		{
			Result.Add(MakeAnimationInfo(Anim));
		}
	}
	return Result;
}

FWidgetAnimationInfo UWidgetAnimationToolset::FindWidgetAnimation(
	UWidgetBlueprint* WidgetBlueprint,
	FName AnimationName)
{
	if (!WidgetBlueprint)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("FindWidgetAnimation: WidgetBlueprint is null."));
		return FWidgetAnimationInfo();
	}
	UWidgetAnimation* Found = FindAnimationByName(WidgetBlueprint, AnimationName);
	return MakeAnimationInfo(Found);
}

bool UWidgetAnimationToolset::RenameWidgetAnimation(
	UWidgetBlueprint* WidgetBlueprint,
	UWidgetAnimation* Animation,
	FName NewName)
{
	if (!WidgetBlueprint || !Animation)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RenameWidgetAnimation: Null WidgetBlueprint or Animation."));
		return false;
	}
	if (NewName.IsNone())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RenameWidgetAnimation: NewName is empty."));
		return false;
	}
	if (Animation->GetFName() == NewName)
	{
		return true;
	}

	// Use the same name validator as create - covers FindObject, WidgetTree, BindWidgetAnim,
	// FKismetNameValidator. Matches the Designer's SWidgetAnimationListItem::OnVerifyNameTextChanged.
	{
		FString NameError;
		if (!VerifyAnimationName(WidgetBlueprint, NewName.ToString(), NameError))
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("RenameWidgetAnimation: %s"), *NameError));
			return false;
		}
	}

	const FScopedTransaction Transaction(LOCTEXT("RenameWidgetAnimation", "Rename Widget Animation"));
	const FName OldName = Animation->GetFName();
	WidgetBlueprint->Modify();
	Animation->Modify();
	UMovieScene* MovieScene = Animation->GetMovieScene();
	if (MovieScene)
	{
		MovieScene->Modify();
	}

	const FString NewNameStr = NewName.ToString();
	if (!Animation->Rename(*NewNameStr, Animation->GetOuter(), REN_DontCreateRedirectors))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("RenameWidgetAnimation: UObject::Rename failed for '%s' -> '%s'."),
			*OldName.ToString(), *NewNameStr));
		return false;
	}

	// Keep the inner MovieScene's name in sync with the animation, matching the Designer
	// (SWidgetAnimationListItem::OnNameTextCommited - UMGEditor/Private/TabFactory/
	// AnimationTabSummoner.cpp:282). Without this, FindObject by name on the MovieScene
	// breaks and successive renames race against the stale inner name.
	if (MovieScene)
	{
		MovieScene->Rename(*NewNameStr, Animation, REN_DontCreateRedirectors);
	}

#if WITH_EDITOR
	// Match the Designer (SWidgetAnimationListItem::OnNameTextCommited - UMGEditor/Private/
	// TabFactory/AnimationTabSummoner.cpp:280): DisplayLabel is overwritten with the new
	// name on every rename. Callers that want a custom display label call
	// SetWidgetAnimationDisplayLabel after rename, the same way Designer users would.
	Animation->SetDisplayLabel(NewNameStr);
#endif

	WidgetBlueprint->OnVariableRenamed(OldName, NewName);
	// Fix up any Blueprint graph nodes (Play Animation, Get Animation, etc.) that reference
	// the animation by its old variable name. Designer calls this immediately after
	// OnVariableRenamed - AnimationTabSummoner.cpp:302.
	FBlueprintEditorUtils::ReplaceVariableReferences(WidgetBlueprint, OldName, NewName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
	return true;
}

bool UWidgetAnimationToolset::SetWidgetAnimationDisplayLabel(
	UWidgetAnimation* Animation,
	const FString& DisplayLabel)
{
	// Module is EditorNoCommandlet with Editor-only target allowlist, so this whole class
	// only exists in editor builds - no need to gate the body on WITH_EDITOR.
	if (!Animation)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetWidgetAnimationDisplayLabel: Animation is null."));
		return false;
	}
	const FScopedTransaction Transaction(LOCTEXT("SetAnimDisplayLabel", "Set Widget Animation Display Label"));
	Animation->Modify();
	Animation->SetDisplayLabel(DisplayLabel);
	return true;
}

bool UWidgetAnimationToolset::SetWidgetAnimationLength(
	UWidgetAnimation* Animation,
	float LengthSeconds)
{
	if (!Animation)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetWidgetAnimationLength: Animation is null."));
		return false;
	}
	if (LengthSeconds <= 0.0f)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetWidgetAnimationLength: LengthSeconds must be > 0."));
		return false;
	}
	UMovieScene* MovieScene = Animation->GetMovieScene();
	if (!MovieScene)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("SetWidgetAnimationLength: Animation has no MovieScene."));
		return false;
	}

	const FScopedTransaction Transaction(LOCTEXT("SetAnimLength", "Set Widget Animation Length"));
	// Modify the owning blueprint too so the asset's dirty indicator updates and undo
	// captures both layers. Animations live under their WBP in normal use; bail gracefully
	// for transient animations (e.g. mid-test scratch state).
	if (UWidgetBlueprint* OwningBlueprint = Animation->GetTypedOuter<UWidgetBlueprint>())
	{
		OwningBlueprint->Modify();
	}
	MovieScene->Modify();

	// Match the Designer's end-exclusive range convention (SUMGAnimationList::OnNewAnimationClicked
	// at UMGEditor/Private/TabFactory/AnimationTabSummoner.cpp:648 stores OutFrame+1 as the upper bound).
	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	const FFrameNumber EndFrame = (LengthSeconds * TickResolution).FloorToFrame();
	MovieScene->SetPlaybackRange(FFrameNumber(0), EndFrame.Value + 1, /*bAlwaysMarkDirty=*/false);
	return true;
}

bool UWidgetAnimationToolset::RemoveWidgetAnimation(
	UWidgetBlueprint* WidgetBlueprint,
	UWidgetAnimation* Animation)
{
	if (!WidgetBlueprint || !Animation)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RemoveWidgetAnimation: Null WidgetBlueprint or Animation."));
		return false;
	}
	if (!WidgetBlueprint->Animations.Contains(Animation))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RemoveWidgetAnimation: Animation is not on this Widget Blueprint."));
		return false;
	}

	const FScopedTransaction Transaction(LOCTEXT("RemoveWidgetAnimation", "Remove Widget Animation"));
	const FName AnimFName = Animation->GetFName();
	WidgetBlueprint->Modify();
	// Match the Designer (SUMGAnimationList::OnDeleteAnimation): rename the animation into
	// the transient package so the name is freed up immediately for reuse and the asset
	// can't be looked up via FindObject on the blueprint outer.
	Animation->Rename(nullptr, GetTransientPackage());
	WidgetBlueprint->Animations.Remove(Animation);
	WidgetBlueprint->OnVariableRemoved(AnimFName);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);
	return true;
}

// ============================================================================
// Bindings
// ============================================================================

FMovieSceneBindingProxy UWidgetAnimationToolset::AddWidgetToAnimation(
	UWidgetBlueprint* WidgetBlueprint,
	UWidgetAnimation* Animation,
	UObject* ObjectToBind)
{
	if (!WidgetBlueprint || !Animation || !ObjectToBind)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddWidgetToAnimation: Null WidgetBlueprint, Animation, or ObjectToBind."));
		return FMovieSceneBindingProxy();
	}

	UMovieScene* MovieScene = Animation->GetMovieScene();
	if (!MovieScene)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("AddWidgetToAnimation: Animation has no MovieScene."));
		return FMovieSceneBindingProxy();
	}

	FString BindingDisplayName;
	if (UPanelSlot* Slot = Cast<UPanelSlot>(ObjectToBind))
	{
		if (!Slot->Content)
		{
			UKismetSystemLibrary::RaiseScriptError(TEXT(
				"AddWidgetToAnimation: Cannot bind an empty UPanelSlot (Content is null)."));
			return FMovieSceneBindingProxy();
		}
		BindingDisplayName = Slot->Content->GetName();
	}
	else if (UWidget* Widget = Cast<UWidget>(ObjectToBind))
	{
		BindingDisplayName = Widget->GetName();
	}
	else
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT(
			"AddWidgetToAnimation: ObjectToBind must be a UWidget or a UPanelSlot."));
		return FMovieSceneBindingProxy();
	}

	if (!UToolsetLibrary::IsClassAllowed(ObjectToBind->GetClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT(
			"AddWidgetToAnimation: '%s' is not permitted in this project."),
			*ObjectToBind->GetClass()->GetName()));
		return FMovieSceneBindingProxy();
	}

	// Bindings resolve by name against the live UUserWidget's WidgetTree at runtime
	// (UWidgetAnimation::LocateBoundObjects), so binding a widget from another blueprint
	// produces a binding entry that will never resolve. The Designer's picker scopes to its
	// own preview tree implicitly; we have to check explicitly here.
	if (!ObjectToBind->IsIn(WidgetBlueprint))
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(TEXT(
			"AddWidgetToAnimation: '%s' does not belong to '%s'; cross-blueprint bindings "
			"won't resolve at runtime. Bind a widget or slot from this Widget Blueprint instead."),
			*ObjectToBind->GetName(), *WidgetBlueprint->GetName()));
		return FMovieSceneBindingProxy();
	}

	if (!WidgetBlueprint->GeneratedClass ||
		!WidgetBlueprint->GeneratedClass->IsChildOf(UUserWidget::StaticClass()))
	{
		FKismetEditorUtilities::CompileBlueprint(WidgetBlueprint);
	}
	if (!WidgetBlueprint->GeneratedClass ||
		!WidgetBlueprint->GeneratedClass->IsChildOf(UUserWidget::StaticClass()))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT(
			"AddWidgetToAnimation: Widget Blueprint has no valid UUserWidget GeneratedClass (compile failed?)."));
		return FMovieSceneBindingProxy();
	}

	const FScopedTransaction Transaction(LOCTEXT("AddWidgetToAnimation", "Add Widget to Animation"));
	WidgetBlueprint->Modify();
	Animation->Modify();
	MovieScene->Modify();

	const FGuid NewGuid = MovieScene->AddPossessable(BindingDisplayName, ObjectToBind->GetClass());
	if (!NewGuid.IsValid())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT(
			"AddWidgetToAnimation: MovieScene::AddPossessable returned an invalid GUID."));
		return FMovieSceneBindingProxy();
	}

	// UWidgetAnimation::BindPossessableObject (Engine/Source/Runtime/UMG/Private/Animation/
	// WidgetAnimation.cpp:233) uses the Context UUserWidget only to detect "is this the WBP's
	// root widget?" via pointer comparison (&PossessedObject == PreviewWidget). The actual
	// binding entry it appends is name-based (FWidgetAnimationBinding::WidgetName), so the
	// generated class's CDO is sufficient as a sentinel here - no allocation per call.
	// The Designer instead passes its live preview UUserWidget so root-widget bindings flag
	// bIsRootWidget; for this headless surface we don't bind the root widget itself.
	UUserWidget* PreviewContext = WidgetBlueprint->GeneratedClass->GetDefaultObject<UUserWidget>();
	Animation->BindPossessableObject(NewGuid, *ObjectToBind, PreviewContext);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WidgetBlueprint);

	return FMovieSceneBindingProxy(NewGuid, Animation);
}

TArray<FMovieSceneBindingProxy> UWidgetAnimationToolset::GetWidgetAnimationBindings(UWidgetAnimation* Animation)
{
	TArray<FMovieSceneBindingProxy> Result;
	if (!Animation)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("GetWidgetAnimationBindings: Animation is null."));
		return Result;
	}
	UMovieScene* MovieScene = Animation->GetMovieScene();
	if (!MovieScene)
	{
		return Result;
	}
	const int32 Count = MovieScene->GetPossessableCount();
	Result.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FMovieScenePossessable& Possessable = MovieScene->GetPossessable(Index);
		Result.Emplace(Possessable.GetGuid(), Animation);
	}
	return Result;
}

bool UWidgetAnimationToolset::RemoveWidgetBinding(UWidgetAnimation* Animation, FGuid BindingGuid)
{
	if (!Animation)
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RemoveWidgetBinding: Animation is null."));
		return false;
	}
	if (!BindingGuid.IsValid())
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT("RemoveWidgetBinding: BindingGuid is invalid."));
		return false;
	}
	UMovieScene* MovieScene = Animation->GetMovieScene();
	if (!MovieScene)
	{
		return false;
	}
	if (!MovieScene->FindPossessable(BindingGuid))
	{
		UKismetSystemLibrary::RaiseScriptError(TEXT(
			"RemoveWidgetBinding: BindingGuid does not match an existing binding on this animation."));
		return false;
	}

	const FScopedTransaction Transaction(LOCTEXT("RemoveWidgetBinding", "Remove Widget Binding"));
	Animation->Modify();
	MovieScene->Modify();

	Animation->UnbindPossessableObjects(BindingGuid);
	MovieScene->RemovePossessable(BindingGuid);
	return true;
}

#undef LOCTEXT_NAMESPACE
