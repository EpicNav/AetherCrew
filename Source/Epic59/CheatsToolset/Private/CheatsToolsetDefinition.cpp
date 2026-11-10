// Copyright Epic Games, Inc. All Rights Reserved.

#include "CheatsToolsetDefinition.h"

#include "CheatsToolset.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/CheatManager.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/StringOutputDevice.h"
#include "UObject/UObjectIterator.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(CheatsToolsetDefinition)

namespace
{
	/// Finds the local player controller to target with cheats (they route to the local player by engine
	/// convention, so we resolve it rather than take it as a parameter). Prefers a live PIE world, falling
	/// back to a standalone Game world; editor/preview worlds are ignored. Null if none is live.
	APlayerController* ResolveLocalPlayerController()
	{
		if (!GEngine)
		{
			return nullptr;
		}

		UWorld* PieWorld = nullptr;
		UWorld* GameWorld = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (!World)
			{
				continue;
			}
			if (Context.WorldType == EWorldType::PIE)
			{
				PieWorld = World;
				break;
			}
			if (Context.WorldType == EWorldType::Game)
			{
				GameWorld = World;
			}
		}

		UWorld* TargetWorld = PieWorld ? PieWorld : GameWorld;
		return TargetWorld ? TargetWorld->GetFirstPlayerController() : nullptr;
	}

	/// Resolves the local player controller and its cheat manager, raising a clear script error on any
	/// failure. Returns true only when both are valid.
	bool ResolvePCAndCheatManager(APlayerController*& OutPC, UCheatManager*& OutCM)
	{
		OutPC = ResolveLocalPlayerController();
		if (!OutPC)
		{
			UKismetSystemLibrary::RaiseScriptError(
				TEXT("No local player controller found. Start a Play In Editor session first."));
			return false;
		}

		OutCM = OutPC->CheatManager;
		if (!OutCM)
		{
			UKismetSystemLibrary::RaiseScriptError(
				TEXT("The local player controller has no cheat manager. Cheats are likely disabled -- "
				"run a single-player PIE session or enable cheats (e.g. the 'EnableCheats' console "
				"command) and try again."));
			return false;
		}

		return true;
	}

	/// Builds a console-style argument hint, e.g. "<float F>" or "" (no params). Each parameter is shown
	/// as "<type name>"; skips the return value.
	FString BuildArgs(const UFunction* Function)
	{
		TArray<FString> Params;
		for (TFieldIterator<FProperty> It(Function);
			It && ((It->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm); ++It)
		{
			Params.Add(FString::Printf(TEXT("<%s %s>"), *It->GetCPPType(), *It->GetName()));
		}
		return FString::Join(Params, TEXT(" "));
	}
}

TArray<FCheatInfo> UCheatsToolset::ListCheats(const FString& NameFilter)
{
	APlayerController* PC = nullptr;
	UCheatManager* CM = nullptr;
	if (!ResolvePCAndCheatManager(PC, CM))
	{
		return {};
	}

	// A cheat is runnable only if the local player can actually invoke it; authority-only cheats are
	// omitted for a non-authority (client) local player, matching CallCheat's pre-check.
	const bool bLocalHasAuthority = (PC->GetLocalRole() >= ROLE_Authority);
	return CollectCheats(CM, NameFilter, bLocalHasAuthority);
}

TArray<FCheatInfo> UCheatsToolset::CollectCheats(
	UCheatManager* CM, const FString& NameFilter, bool bLocalHasAuthority)
{
	if (!CM)
	{
		return {};
	}

	TArray<FCheatInfo> Results;
	TSet<FName> Seen;

	// Collects exec UFunctions declared on OwningClass (and its supers) into Results, de-duplicating
	// by name so a cheat is reported once even when surfaced from multiple classes. Authority-only cheats
	// are skipped when the local player lacks authority, since CallCheat would reject them.
	auto CollectFrom = [&Results, &Seen, bLocalHasAuthority](UClass* OwningClass)
	{
		for (TFieldIterator<UFunction> It(OwningClass, EFieldIterationFlags::IncludeSuper); It; ++It)
		{
			UFunction* Function = *It;
			if (!Function->HasAnyFunctionFlags(FUNC_Exec))
			{
				continue;
			}

			if (!bLocalHasAuthority && Function->HasAnyFunctionFlags(FUNC_BlueprintAuthorityOnly))
			{
				continue;
			}

			const FName CheatFName = Function->GetFName();
			bool bAlreadySeen = false;
			Seen.Add(CheatFName, &bAlreadySeen);
			if (bAlreadySeen)
			{
				continue;
			}

			FCheatInfo Info;
			Info.CheatName = CheatFName.ToString();
			Info.Args = BuildArgs(Function);
			// Report the class that actually declares the cheat, even when surfaced via a subclass.
			Info.SourceClass = Function->GetOwnerClass();
#if WITH_EDITOR
			// Tooltip metadata exists only in editor builds; this is an editor-only toolset.
			Info.Description = Function->GetToolTipText().ToString();
#endif
			Results.Add(MoveTemp(Info));
		}
	};

	// The cheat manager itself (its live class walks up through the base UCheatManager).
	CollectFrom(CM->GetClass());

	// Every loaded cheat-manager-extension subclass that is actually registered on this manager.
	// The manager's CheatManagerExtensions array is protected, so we probe candidate classes with the
	// public FindCheatManagerExtension() instead of reading the array directly.
	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Candidate = *ClassIt;
		if (Candidate == UCheatManagerExtension::StaticClass()
			|| !Candidate->IsChildOf(UCheatManagerExtension::StaticClass())
			|| Candidate->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}

		if (CM->FindCheatManagerExtension(Candidate) != nullptr)
		{
			CollectFrom(Candidate);
		}
	}

	// Apply the optional case-insensitive name substring filter.
	if (!NameFilter.IsEmpty())
	{
		Results.RemoveAll([&NameFilter](const FCheatInfo& Info)
		{
			return !Info.CheatName.Contains(NameFilter, ESearchCase::IgnoreCase);
		});
	}

	Results.Sort([](const FCheatInfo& A, const FCheatInfo& B)
	{
		return A.CheatName < B.CheatName;
	});

	return Results;
}

void UCheatsToolset::CallCheat(const FString& CheatName, const FString& Args)
{
	APlayerController* PC = nullptr;
	UCheatManager* CM = nullptr;
	if (!ResolvePCAndCheatManager(PC, CM))
	{
		return;
	}

	// Resolve the owning instance + function (manager first, then live extensions). We invoke directly on
	// the owner rather than routing through ProcessConsoleExec, which would silently forward authority-only
	// cheats to the server on a client.
	UObject* Owner = nullptr;
	UFunction* Function = ResolveCheatOrRaise(CM, CheatName, Owner);
	if (!Function || !Owner)
	{
		return;
	}

	// Explicit authority pre-check. A cheat lives on a UCheatManager/extension (not an AActor), so a
	// direct call does NOT run the actor callspace's silent "absorb" of authority-only functions -- so
	// we must check authority ourselves to avoid running a cheat that should be server-only.
	if (Function->HasAnyFunctionFlags(FUNC_BlueprintAuthorityOnly) && PC->GetLocalRole() < ROLE_Authority)
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("Cheat '%s' is authority-only, but the local player controller is a client "
			"(role < ROLE_Authority). Run it in a single-player or listen-server PIE session."),
			*CheatName));
		return;
	}

	// Recombine into a console command line and run it on the owning object, exactly as the console
	// would: this parses the arguments and imports the typed parameters via the standard exec dispatch.
	const FString CommandLine = Args.IsEmpty() ? CheatName : FString::Printf(TEXT("%s %s"), *CheatName, *Args);
	FStringOutputDevice Output;
	const bool bInvoked = Owner->CallFunctionByNameWithArguments(
		*CommandLine, Output, /*Executor=*/PC, /*bForceCallWithNonExec=*/false);

	if (!bInvoked)
	{
		const FString OutputSuffix = Output.IsEmpty()
			? FString()
			: FString::Printf(TEXT(" Output: %s"), *Output);
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("Failed to run cheat '%s' with args '%s' (argument parse or dispatch failure).%s"),
			*CheatName, *Args, *OutputSuffix));
		return;
	}

	UE_LOGF(LogCheatsToolset, Log, "Ran cheat '%ls' with args '%ls'.%ls",
		*CheatName, *Args, Output.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" Output: %ls"), *Output));
}

UFunction* UCheatsToolset::ResolveCheatOrRaise(
	UCheatManager* CM, const FString& CheatName, UObject*& OutOwner)
{
	OutOwner = nullptr;

	// Find-only: never pollutes the global name table. An unknown name can't match any cheat.
	const FName CheatFName(*CheatName, FNAME_Find);
	if (CheatFName.IsNone())
	{
		UKismetSystemLibrary::RaiseScriptError(FString::Printf(
			TEXT("Cheat '%s' not found. Call ListCheats to see available cheats."), *CheatName));
		return nullptr;
	}

	FString InactiveExtensionClass;
	UFunction* Function = ResolveCheat(CM, CheatFName, OutOwner, InactiveExtensionClass);
	if (!Function || !OutOwner)
	{
		if (!InactiveExtensionClass.IsEmpty())
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("Cheat '%s' is defined by extension '%s', which is not active on the local player's "
				"cheat manager."), *CheatName, *InactiveExtensionClass));
		}
		else
		{
			UKismetSystemLibrary::RaiseScriptError(FString::Printf(
				TEXT("Cheat '%s' not found on the local cheat manager or its active extensions. Call "
				"ListCheats to see available cheats."), *CheatName));
		}
		return nullptr;
	}

	return Function;
}

UFunction* UCheatsToolset::ResolveCheat(
	UCheatManager* CM, FName CheatName, UObject*& OutOwner, FString& OutInactiveExtensionClass)
{
	OutOwner = nullptr;
	OutInactiveExtensionClass.Reset();

	if (!CM || CheatName.IsNone())
	{
		return nullptr;
	}

	// The cheat manager itself takes precedence (matches the engine's ProcessConsoleExec ordering).
	if (UFunction* ManagerFn = CM->FindFunction(CheatName))
	{
		if (ManagerFn->HasAnyFunctionFlags(FUNC_Exec))
		{
			OutOwner = CM;
			return ManagerFn;
		}
	}

	// Then any registered extension. The CheatManagerExtensions array is protected, so we probe loaded
	// extension subclasses with the public FindCheatManagerExtension (which matches by IsA).
	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Candidate = *ClassIt;
		if (Candidate == UCheatManagerExtension::StaticClass()
			|| !Candidate->IsChildOf(UCheatManagerExtension::StaticClass())
			|| Candidate->HasAnyClassFlags(CLASS_Abstract))
		{
			continue;
		}

		UFunction* ExtFn = Candidate->FindFunctionByName(CheatName);
		if (!ExtFn || !ExtFn->HasAnyFunctionFlags(FUNC_Exec))
		{
			continue;
		}

		if (UCheatManagerExtension* Extension = CM->FindCheatManagerExtension(Candidate))
		{
			OutOwner = Extension;
			return ExtFn;
		}

		// Declared by an extension class that exists but is not registered on this manager. Remember the
		// first such class so the caller can produce a precise "extension not active" error.
		if (OutInactiveExtensionClass.IsEmpty())
		{
			OutInactiveExtensionClass = Candidate->GetName();
		}
	}

	return nullptr;
}

bool UCheatsToolset::IsToolsetHidden_Implementation(const TScriptInterface<IToolsetPermissionProvider>& Provider) const
{
	return Provider->IsRestricted() || Provider->GetGrantedRoles().Contains(TEXT("VKCreateUGC"));
}
