// Copyright Epic Games, Inc. All Rights Reserved.

#include "CheatsToolsetTest.h"

#include "CheatsToolsetDefinition.h"
#include "GameFramework/CheatManager.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_DEV_AUTOMATION_TESTS

BEGIN_DEFINE_SPEC(FCheatsToolsetSpec,
	"AIAssistant.Toolsets.CheatsToolset.CheatsToolset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	// Roots the owning player controller for the test's lifetime. UCheatManager is Within=PlayerController
	// so it needs a PlayerController outer; the rooted PC also keeps the manager (and its extensions) alive
	// via its CheatManager UPROPERTY, which MakeTestManager sets -- an outer alone does not prevent GC.
	TStrongObjectPtr<APlayerController> OwningPC;
	UCheatManager* CheatManager = nullptr;

	// Creates a test cheat manager parented to a fresh transient player controller and assigned to that
	// controller's CheatManager UPROPERTY, so the rooted PC keeps it alive.
	UCheatsToolsetTestManager* MakeTestManager()
	{
		OwningPC = TStrongObjectPtr<APlayerController>(NewObject<APlayerController>(GetTransientPackage()));
		UCheatsToolsetTestManager* Manager = NewObject<UCheatsToolsetTestManager>(OwningPC.Get());
		OwningPC->CheatManager = Manager;
		return Manager;
	}

	// Returns the first cheat in Cheats whose name matches, or nullptr.
	static const FCheatInfo* FindCheat(const TArray<FCheatInfo>& Cheats, const TCHAR* Name)
	{
		return Cheats.FindByPredicate([Name](const FCheatInfo& Info) { return Info.CheatName == Name; });
	}

END_DEFINE_SPEC(FCheatsToolsetSpec)

void FCheatsToolsetSpec::Define()
{
	// -------------------------------------------------------------------------
	// CollectCheats: enumeration / filtering against a constructed cheat manager.
	// No PIE needed -- a transient player controller and its cheat manager suffice.
	// -------------------------------------------------------------------------
	Describe(TEXT("CollectCheats with a manager and no extensions"), [this]()
	{
		BeforeEach([this]()
		{
			CheatManager = MakeTestManager();
		});

		It(TEXT("lists the manager's exec cheats"), [this]()
		{
			const TArray<FCheatInfo> Cheats = UCheatsToolset::CollectCheats(CheatManager, TEXT(""), /*bLocalHasAuthority=*/true);
			TestNotNull(TEXT("TestManagerCheat present"), FindCheat(Cheats, TEXT("TestManagerCheat")));
			TestNotNull(TEXT("TestAuthorityCheat present"), FindCheat(Cheats, TEXT("TestAuthorityCheat")));
		});

		It(TEXT("omits authority-only cheats when the local player lacks authority"), [this]()
		{
			// With authority, the authority-only cheat is listed.
			const TArray<FCheatInfo> WithAuthority =
				UCheatsToolset::CollectCheats(CheatManager, TEXT(""), /*bLocalHasAuthority=*/true);
			TestNotNull(TEXT("Authority cheat present with authority"),
				FindCheat(WithAuthority, TEXT("TestAuthorityCheat")));

			// Without authority, it is filtered out (CallCheat would reject it), while a plain cheat stays.
			const TArray<FCheatInfo> WithoutAuthority =
				UCheatsToolset::CollectCheats(CheatManager, TEXT(""), /*bLocalHasAuthority=*/false);
			TestNull(TEXT("Authority cheat omitted without authority"),
				FindCheat(WithoutAuthority, TEXT("TestAuthorityCheat")));
			TestNotNull(TEXT("Plain cheat still present without authority"),
				FindCheat(WithoutAuthority, TEXT("TestManagerCheat")));
		});

		It(TEXT("includes inherited base UCheatManager cheats (IncludeSuper)"), [this]()
		{
			const TArray<FCheatInfo> Cheats = UCheatsToolset::CollectCheats(CheatManager, TEXT(""), /*bLocalHasAuthority=*/true);
			// "Fly" is a stock exec cheat on the base UCheatManager.
			TestNotNull(TEXT("Inherited Fly cheat present"), FindCheat(Cheats, TEXT("Fly")));
		});

		It(TEXT("reports the declaring class"), [this]()
		{
			const TArray<FCheatInfo> Cheats = UCheatsToolset::CollectCheats(CheatManager, TEXT(""), /*bLocalHasAuthority=*/true);
			const FCheatInfo* Cheat = FindCheat(Cheats, TEXT("TestManagerCheat"));
			if (TestNotNull(TEXT("TestManagerCheat"), Cheat))
			{
				TestTrue(TEXT("Source class is the manager"),
					Cheat->SourceClass == UCheatsToolsetTestManager::StaticClass());
			}
		});

		It(TEXT("builds an args hint naming each parameter"), [this]()
		{
			const TArray<FCheatInfo> Cheats = UCheatsToolset::CollectCheats(CheatManager, TEXT(""), /*bLocalHasAuthority=*/true);

			const FCheatInfo* WithParam = FindCheat(Cheats, TEXT("TestManagerCheat"));
			if (TestNotNull(TEXT("TestManagerCheat"), WithParam))
			{
				// Args carries only the parameter portion -- not the cheat name (that's in CheatName).
				TestFalse(TEXT("Args omits the cheat name"), WithParam->Args.Contains(TEXT("TestManagerCheat")));
				TestTrue(TEXT("Args names the parameter"), WithParam->Args.Contains(TEXT("Count")));
			}

			const FCheatInfo* NoParam = FindCheat(Cheats, TEXT("TestAuthorityCheat"));
			if (TestNotNull(TEXT("TestAuthorityCheat"), NoParam))
			{
				// No parameters, so Args is empty.
				TestTrue(TEXT("Args is empty"), NoParam->Args.IsEmpty());
			}
		});

		It(TEXT("returns results sorted by cheat name"), [this]()
		{
			const TArray<FCheatInfo> Cheats = UCheatsToolset::CollectCheats(CheatManager, TEXT(""), /*bLocalHasAuthority=*/true);
			for (int32 i = 1; i < Cheats.Num(); ++i)
			{
				TestTrue(TEXT("Sorted"), Cheats[i - 1].CheatName <= Cheats[i].CheatName);
			}
		});

		It(TEXT("does not list cheats from unregistered extensions"), [this]()
		{
			const TArray<FCheatInfo> Cheats = UCheatsToolset::CollectCheats(CheatManager, TEXT(""), /*bLocalHasAuthority=*/true);
			TestNull(TEXT("Extension cheat absent before registration"),
				FindCheat(Cheats, TEXT("TestExtensionCheat")));
		});
	});

	// -------------------------------------------------------------------------
	// CollectCheats with a registered extension.
	// -------------------------------------------------------------------------
	Describe(TEXT("CollectCheats with a registered extension"), [this]()
	{
		BeforeEach([this]()
		{
			CheatManager = MakeTestManager();
			// Within=CheatManager: the extension's outer must be the manager.
			UCheatManagerExtension* Extension =
				NewObject<UCheatsToolsetTestExtension>(CheatManager);
			CheatManager->AddCheatManagerExtension(Extension);
		});

		It(TEXT("lists the extension's cheats with the extension as source"), [this]()
		{
			const TArray<FCheatInfo> Cheats = UCheatsToolset::CollectCheats(CheatManager, TEXT(""), /*bLocalHasAuthority=*/true);
			const FCheatInfo* Cheat = FindCheat(Cheats, TEXT("TestExtensionCheat"));
			if (TestNotNull(TEXT("TestExtensionCheat present"), Cheat))
			{
				TestTrue(TEXT("Source is the extension class"),
					Cheat->SourceClass == UCheatsToolsetTestExtension::StaticClass());
			}
		});

		It(TEXT("de-duplicates a name shared by manager and extension, manager wins"), [this]()
		{
			const TArray<FCheatInfo> Cheats = UCheatsToolset::CollectCheats(CheatManager, TEXT(""), /*bLocalHasAuthority=*/true);
			int32 Count = 0;
			for (const FCheatInfo& Info : Cheats)
			{
				if (Info.CheatName == TEXT("SharedNameCheat"))
				{
					++Count;
				}
			}
			TestEqual(TEXT("SharedNameCheat listed once"), Count, 1);

			const FCheatInfo* Shared = FindCheat(Cheats, TEXT("SharedNameCheat"));
			if (TestNotNull(TEXT("SharedNameCheat"), Shared))
			{
				TestTrue(TEXT("Manager takes precedence"),
					Shared->SourceClass == UCheatsToolsetTestManager::StaticClass());
			}
		});
	});

	// -------------------------------------------------------------------------
	// CollectCheats filtering.
	// -------------------------------------------------------------------------
	Describe(TEXT("CollectCheats filtering"), [this]()
	{
		BeforeEach([this]()
		{
			CheatManager = MakeTestManager();
			UCheatManagerExtension* Extension =
				NewObject<UCheatsToolsetTestExtension>(CheatManager);
			CheatManager->AddCheatManagerExtension(Extension);
		});

		It(TEXT("name filter keeps only matching cheats (case-insensitive)"), [this]()
		{
			const TArray<FCheatInfo> Cheats =
				UCheatsToolset::CollectCheats(CheatManager, TEXT("extensioncheat"), /*bLocalHasAuthority=*/true);
			TestTrue(TEXT("Non-empty"), Cheats.Num() > 0);
			for (const FCheatInfo& Info : Cheats)
			{
				TestTrue(TEXT("Every result matches the name filter"),
					Info.CheatName.Contains(TEXT("ExtensionCheat"), ESearchCase::IgnoreCase));
			}
		});

		It(TEXT("a non-matching filter yields an empty result"), [this]()
		{
			const TArray<FCheatInfo> Cheats =
				UCheatsToolset::CollectCheats(CheatManager, TEXT("NoCheatHasThisSubstringXYZ"), /*bLocalHasAuthority=*/true);
			TestEqual(TEXT("Empty"), Cheats.Num(), 0);
		});
	});

	// -------------------------------------------------------------------------
	// ResolveCheat: name -> (function, owner) with manager-first precedence.
	// -------------------------------------------------------------------------
	Describe(TEXT("ResolveCheat"), [this]()
	{
		BeforeEach([this]()
		{
			CheatManager = MakeTestManager();
		});

		It(TEXT("resolves a manager cheat to the manager"), [this]()
		{
			UObject* Owner = nullptr;
			FString Inactive;
			UFunction* Function =
				UCheatsToolset::ResolveCheat(CheatManager, TEXT("TestManagerCheat"), Owner, Inactive);
			TestNotNull(TEXT("Function resolved"), Function);
			TestEqual(TEXT("Owner is the manager"), Owner, static_cast<UObject*>(CheatManager));
			TestTrue(TEXT("No inactive class"), Inactive.IsEmpty());
		});

		It(TEXT("resolves a registered extension cheat to the extension instance"), [this]()
		{
			UCheatManagerExtension* Extension =
				NewObject<UCheatsToolsetTestExtension>(CheatManager);
			CheatManager->AddCheatManagerExtension(Extension);

			UObject* Owner = nullptr;
			FString Inactive;
			UFunction* Function =
				UCheatsToolset::ResolveCheat(CheatManager, TEXT("TestExtensionCheat"), Owner, Inactive);
			TestNotNull(TEXT("Function resolved"), Function);
			TestEqual(TEXT("Owner is the extension"), Owner, static_cast<UObject*>(Extension));
		});

		It(TEXT("returns null for an unknown cheat with no inactive class"), [this]()
		{
			UObject* Owner = nullptr;
			FString Inactive;
			UFunction* Function =
				UCheatsToolset::ResolveCheat(CheatManager, TEXT("NoSuchCheatXYZ"), Owner, Inactive);
			TestNull(TEXT("No function"), Function);
			TestNull(TEXT("No owner"), Owner);
			TestTrue(TEXT("No inactive class"), Inactive.IsEmpty());
		});

		It(TEXT("reports the declaring class when a cheat's extension is not registered"), [this]()
		{
			// The extension class is loaded but not added to this manager.
			UObject* Owner = nullptr;
			FString Inactive;
			UFunction* Function =
				UCheatsToolset::ResolveCheat(CheatManager, TEXT("TestExtensionCheat"), Owner, Inactive);
			TestNull(TEXT("No active function"), Function);
			TestEqual(TEXT("Names the inactive extension class"), Inactive,
				TEXT("CheatsToolsetTestExtension"));
		});
	});

	// -------------------------------------------------------------------------
	// Defensive: a null cheat manager yields empty / null rather than crashing.
	// -------------------------------------------------------------------------
	Describe(TEXT("With a null cheat manager"), [this]()
	{
		It(TEXT("CollectCheats returns empty"), [this]()
		{
			const TArray<FCheatInfo> Cheats =
				UCheatsToolset::CollectCheats(nullptr, TEXT(""), /*bLocalHasAuthority=*/true);
			TestEqual(TEXT("Empty"), Cheats.Num(), 0);
		});

		It(TEXT("ResolveCheat returns null"), [this]()
		{
			UObject* Owner = nullptr;
			FString Inactive;
			UFunction* Function =
				UCheatsToolset::ResolveCheat(nullptr, TEXT("TestManagerCheat"), Owner, Inactive);
			TestNull(TEXT("No function"), Function);
			TestNull(TEXT("No owner"), Owner);
		});
	});
}

#endif  // WITH_DEV_AUTOMATION_TESTS
