// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "AnimNotifyState_MotionWarping.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Factories/AnimMontageFactory.h"
#include "GameFramework/Character.h"
#include "HyperAIStudioMotionWarpingGate.h"
#include "HyperAIStudioMotionWarpingToolset.h"
#include "HyperAIStudioSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "MotionWarpingComponent.h"
#include "ReferenceSkeleton.h"
#include "RootMotionModifier.h"
#include "RootMotionModifier_SkewWarp.h"

namespace HyperAIStudio::MotionWarpingTests
{
	using C = FHyperAIStudioMotionWarpingContracts;
	namespace Gate = HyperAIStudio::MotionWarping::Gate;
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	FString TestAsset(const TCHAR* Name)
	{
		return FString::Printf(TEXT("/Game/HyperAITests/MotionWarping/%s_%s"), Name, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
	}

	/** A 2 s montage of one 2 s sequence on a one-bone skeleton, made in memory; nothing is saved. */
	struct FFixture
	{
		USkeleton* Skeleton = nullptr;
		UAnimSequence* Sequence = nullptr;
		UAnimMontage* Montage = nullptr;

		explicit FFixture(const bool bRootMotion = true)
		{
			const FString SkeletonPath = TestAsset(TEXT("SK_Warp"));
			Skeleton = NewObject<USkeleton>(CreatePackage(*SkeletonPath), *FPackageName::GetShortName(SkeletonPath), RF_Transient);
			{
				FReferenceSkeletonModifier Bones(Skeleton);
				Bones.Add(FMeshBoneInfo(TEXT("root"), TEXT("root"), INDEX_NONE), FTransform::Identity);
			}
			const FString SequencePath = TestAsset(TEXT("A_Warp"));
			Sequence = NewObject<UAnimSequence>(CreatePackage(*SequencePath), *FPackageName::GetShortName(SequencePath), RF_Transient);
			Sequence->SetSkeleton(Skeleton);
			IAnimationDataController& Controller = Sequence->GetController();
			Controller.OpenBracket(FText::FromString(TEXT("Motion warping test sequence")), /*bShouldTransact=*/false);
			Controller.InitializeModel();
			Controller.SetFrameRate(FFrameRate(30, 1), false);
			Controller.SetNumberOfFrames(60, false);
			Controller.CloseBracket(false);
			Sequence->WaitOnExistingCompression();
			Sequence->bEnableRootMotion = bRootMotion;
			UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
			Factory->SourceAnimation = Sequence;
			UFactory* AsFactory = Factory;
			const FString MontagePath = TestAsset(TEXT("AM_Warp"));
			Montage = Cast<UAnimMontage>(AsFactory->FactoryCreateNew(UAnimMontage::StaticClass(), CreatePackage(*MontagePath), *FPackageName::GetShortName(MontagePath),
				RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
		}

		~FFixture()
		{
			// Nothing here is saved: the assets are let go for the garbage collector, not deleted (the sequence can be rooted while
			// its compression finishes, and deleting assets resets the undo buffer other tests count on).
			for (UObject* Object : TArray<UObject*>({Montage, Sequence, Skeleton}))
			{
				if (!IsValid(Object)) continue;
				Object->ClearFlags(RF_Public | RF_Standalone);
				Object->GetPackage()->SetDirtyFlag(false);
			}
		}

		FString Path() const
		{
			return Montage ? Montage->GetPathName() : FString();
		}

		TArray<Gate::FWindowRef> Windows() const
		{
			return Montage ? Gate::WindowsOf(*Montage) : TArray<Gate::FWindowRef>();
		}
	};

	FHyperAIMotionWarpingOp Add(const TCHAR* Target, const double Start, const double End, const TCHAR* Params = TEXT(""), const TCHAR* Modifier = TEXT(""))
	{
		FHyperAIMotionWarpingOp Op;
		Op.Kind = TEXT("window.add");
		Op.WarpTargetName = Target;
		Op.StartSeconds = Start;
		Op.EndSeconds = End;
		Op.Params = Params;
		Op.Modifier = Modifier;
		return Op;
	}

	FHyperAIMotionWarpingOp Op(const TCHAR* Kind, const TCHAR* Params = TEXT(""))
	{
		FHyperAIMotionWarpingOp Out;
		Out.Kind = Kind;
		Out.Params = Params;
		return Out;
	}

	/** Runs a request with approval off, as an agent with one-shot edits allowed would. */
	FHyperAIMotionWarpingPlanReport Run(const FString& Asset, TArray<FHyperAIMotionWarpingOp> Ops, const bool bDryRun)
	{
		FHyperAIMotionWarpingPlanRequest Request;
		Request.Asset = Asset;
		Request.Operations = MoveTemp(Ops);
		Request.bDryRun = bDryRun;
		UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
		const bool bApproval = Settings->bRequireApprovalForAgentEdits;
		Settings->bRequireApprovalForAgentEdits = false;
		FHyperAIMotionWarpingPlanReport Report = C::ApplyPlan(Request);
		Settings->bRequireApprovalForAgentEdits = bApproval;
		return Report;
	}

	bool Refused(const FHyperAIMotionWarpingPlanReport& Report, const TCHAR* Says)
	{
		return !Report.bOk && Report.Status == TEXT("invalid_request") && Report.Diagnostic.Contains(Says);
	}

	bool Warns(const TArray<FString>& Warnings, const TCHAR* Says)
	{
		return Warnings.ContainsByPredicate([Says](const FString& Each) { return Each.Contains(Says); });
	}

	FName TargetOf(const Gate::FWindowRef& Window)
	{
		const URootMotionModifier_Warp* Warp = Cast<URootMotionModifier_Warp>(Window.State->RootMotionModifier);
		return Warp ? Warp->WarpTargetName : NAME_None;
	}

	/** The plans' undo steps taken back, whatever the test found. */
	void UndoAll(const int32 Steps)
	{
		for (int32 Index = 0; Index < Steps && GEditor; ++Index) GEditor->UndoTransaction();
	}

	USCS_Node* WarpNode(const UBlueprint& Blueprint)
	{
		for (USCS_Node* Node : Blueprint.SimpleConstructionScript->GetAllNodes())
		{
			if (Node && Node->ComponentTemplate && Node->ComponentTemplate->IsA<UMotionWarpingComponent>()) return Node;
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioMotionWarpingParamsTest, "HyperAIStudio.NativeTools.MotionWarping.Params", HyperAIStudio::MotionWarpingTests::Flags)

bool FHyperAIStudioMotionWarpingParamsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::MotionWarpingTests;
	TArray<TPair<FString, FString>> Pairs;
	FString Error;
	TestTrue(TEXT("params split: ") + Error, Gate::SplitParams(TEXT("bIgnoreZAxis=false; RotationType = Facing;"), Pairs, Error) && Pairs.Num() == 2 && Pairs[1].Value == TEXT("Facing"));
	TestFalse(TEXT("a pair without = is refused"), Gate::SplitParams(TEXT("bIgnoreZAxis"), Pairs, Error));
	TestFalse(TEXT("a name given twice is refused"), Gate::SplitParams(TEXT("Scale=1,1,1; scale=2,2,2"), Pairs, Error));

	const UClass& Skew = *URootMotionModifier_SkewWarp::StaticClass();
	FProperty* Rotation = Gate::FindSetting(Skew, TEXT("rotationtype"));
	FProperty* Translation = Gate::FindSetting(Skew, TEXT("Warp Translation"));
	if (!TestTrue(TEXT("settings are found in any case, and by display name"), Rotation && Translation && Translation->GetName() == TEXT("bWarpTranslation"))) return false;
	FString Import;
	TestTrue(TEXT("an enum by its short name: ") + Error, Gate::CoerceValue(*Rotation, TEXT("Facing"), Import, Error) && Import.EndsWith(TEXT("Facing")));
	TestTrue(TEXT("or its full name"), Gate::CoerceValue(*Rotation, TEXT("EMotionWarpRotationType::Facing"), Import, Error));
	TestFalse(TEXT("a deprecated, hidden value is refused"), Gate::CoerceValue(*Rotation, TEXT("OppositeFacing"), Import, Error));
	TestTrue(TEXT("naming the values it takes: ") + Error, Error.Contains(TEXT("Default")) && Error.Contains(TEXT("Facing")) && !Error.Contains(TEXT("Opposite")));
	TestTrue(TEXT("a bool from no"), Gate::CoerceValue(*Translation, TEXT("no"), Import, Error) && Import == TEXT("False"));
	TestFalse(TEXT("a bool from a word that is not one"), Gate::CoerceValue(*Translation, TEXT("maybe"), Import, Error));
	FProperty* Multiplier = Gate::FindSetting(Skew, TEXT("WarpRotationTimeMultiplier"));
	TestTrue(TEXT("a number"), Multiplier && Gate::CoerceValue(*Multiplier, TEXT("0.5"), Import, Error));
	TestFalse(TEXT("a word is not a number"), Multiplier && Gate::CoerceValue(*Multiplier, TEXT("fast"), Import, Error));
	FProperty* Scale = Gate::FindSetting(*URootMotionModifier_Scale::StaticClass(), TEXT("Scale"));
	TestTrue(TEXT("a vector x,y,z: ") + Import, Scale && Gate::CoerceValue(*Scale, TEXT("1, 2, 0.5"), Import, Error) && Import == TEXT("(X=1,Y=2,Z=0.5)"));
	TestFalse(TEXT("two numbers are not a vector"), Scale && Gate::CoerceValue(*Scale, TEXT("1,2"), Import, Error));
	FProperty* Offset = Gate::FindSetting(Skew, TEXT("AdditionalRotationOffset"));
	TestTrue(TEXT("a rotator pitch,yaw,roll: ") + Import, Offset && Gate::CoerceValue(*Offset, TEXT("0,180,0"), Import, Error) && Import == TEXT("(Pitch=0,Yaw=180,Roll=0)"));
	FProperty* Curve = Gate::FindSetting(Skew, TEXT("AddTranslationEasingCurve"));
	TestFalse(TEXT("a missing curve asset is refused"), Curve && Gate::CoerceValue(*Curve, TEXT("/Game/Nowhere/Curve_Missing"), Import, Error));
	TestTrue(TEXT("None is taken"), Curve && Gate::CoerceValue(*Curve, TEXT("None"), Import, Error));

	TArray<Gate::FSetting> Settings;
	TestTrue(TEXT("settings parse: ") + Error, Gate::ParseSettings(Skew, TEXT("bIgnoreZAxis=false; RotationType=Facing; WarpTargetName=Vault"), Settings, Error) && Settings.Num() == 3);
	Settings.Reset();
	TestFalse(TEXT("an unknown setting is refused"), Gate::ParseSettings(Skew, TEXT("Explode=true"), Settings, Error));
	TestTrue(TEXT("listing the ones it takes: ") + Error, Error.Contains(TEXT("Explode")) && Error.Contains(TEXT("bIgnoreZAxis")) && Error.Contains(TEXT("WarpTargetName")));
	TestTrue(TEXT("the component's own setting is there"), Gate::FindSetting(*UMotionWarpingComponent::StaticClass(), TEXT("bSearchForWindowsInAnimsWithinMontages")) != nullptr);

	// The modifiers a window takes, and the op kinds, as inspect lists them.
	const Gate::FModifierInfo* Default = Gate::FindModifier(TEXT("Skew Warp"));
	TestTrue(TEXT("skew_warp is the skew warp class"), Default && Default->Class == URootMotionModifier_SkewWarp::StaticClass() && Default->bWarp && !Default->bExperimental);
	TestNull(TEXT("an unknown modifier is not one"), Gate::FindModifier(TEXT("teleport")));
	TestTrue(TEXT("every modifier has its class"), !Gate::Modifiers().ContainsByPredicate([](const Gate::FModifierInfo& Each) { return Each.Class == nullptr; }));
	const FHyperAIMotionWarpingInspectReport Listed = C::Inspect(FHyperAIMotionWarpingInspectRequest());
	TestTrue(TEXT("inspect without an asset lists the kinds and modifiers: ") + Listed.Diagnostic, Listed.bOk && Listed.Kinds.Num() == Gate::Kinds().Num()
		&& Listed.Modifiers.Num() == Gate::Modifiers().Num());
	const FHyperAIMotionWarpingKind* Listing = Listed.Modifiers.FindByPredicate([](const FHyperAIMotionWarpingKind& Each) { return Each.Kind == TEXT("skew_warp"); });
	TestTrue(TEXT("with their settings, types and defaults"), Listing && Listing->Settings.ContainsByPredicate([](const FString& Line)
		{
			return Line.StartsWith(TEXT("RotationType: one of Default, Facing, default Default"));
		}) && Listing->Settings.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("bIgnoreZAxis: bool, default true")); }));
	const FHyperAIMotionWarpingKind* Component = Listed.Kinds.FindByPredicate([](const FHyperAIMotionWarpingKind& Each) { return Each.Kind == TEXT("component.add"); });
	TestTrue(TEXT("and the component's settings"), Component && Component->Settings.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("bSearchForWindowsInAnimsWithinMontages: bool")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioMotionWarpingDryRunTest, "HyperAIStudio.NativeTools.MotionWarping.DryRun", HyperAIStudio::MotionWarpingTests::Flags)

bool FHyperAIStudioMotionWarpingDryRunTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::MotionWarpingTests;
	FFixture Fixture;
	if (!TestTrue(TEXT("the test montage is made, 2 s long"), Fixture.Montage && FMath::IsNearlyEqual(Fixture.Montage->GetPlayLength(), 2.f, 0.01f))) return false;
	const int32 UndoBefore = GEditor && GEditor->Trans ? (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) : 0;
	const int32 NotifiesBefore = Fixture.Montage->Notifies.Num();
	const FHyperAIMotionWarpingPlanReport Checked = Run(Fixture.Path(), {Add(TEXT("Vault"), 0.2, 0.8, TEXT("bIgnoreZAxis=false"))}, /*bDryRun=*/true);
	TestTrue(TEXT("the dry run checks: ") + Checked.Diagnostic, Checked.bOk && Checked.Status == TEXT("checked"));
	TestTrue(TEXT("and says what it would do"), Checked.Notes.Num() == 1 && Checked.Notes[0].Contains(TEXT("would add a skew_warp window to Vault")));
	TestEqual(TEXT("a montage"), Checked.AssetKind, FString(TEXT("montage")));
	TestEqual(TEXT("nothing is added"), Fixture.Montage->Notifies.Num(), NotifiesBefore);
	TestTrue(TEXT("the windows are as they were: none"), Checked.Windows.IsEmpty() && Fixture.Windows().IsEmpty());
	if (GEditor && GEditor->Trans) TestEqual(TEXT("and no undo step is made"), (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()), UndoBefore);
	TestFalse(TEXT("root motion is on, so no warning about it"), Checked.Warnings.ContainsByPredicate([](const FString& Each) { return Each.Contains(TEXT("root motion")); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioMotionWarpingWindowsTest, "HyperAIStudio.NativeTools.MotionWarping.WindowsApplyAndUndo", HyperAIStudio::MotionWarpingTests::Flags)

bool FHyperAIStudioMotionWarpingWindowsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::MotionWarpingTests;
	if (!GEditor || !GEditor->Trans || GEditor->PlayWorld)
	{
		AddError(TEXT("Needs the editor with its undo buffer, and no play session."));
		return false;
	}
	FFixture Fixture;
	if (!TestNotNull(TEXT("the test montage is made"), Fixture.Montage)) return false;
	const int32 UndoBefore = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount());
	ON_SCOPE_EXIT
	{
		// Whatever failed below: the plans taken back.
		UndoAll(GEditor->Trans->GetQueueLength() - UndoBefore - GEditor->Trans->GetUndoCount());
	};
	FHyperAIMotionWarpingOp Vault = Add(TEXT("Vault"), 0.2, 0.8, TEXT("bIgnoreZAxis=false; RotationType=Facing"));
	Vault.Track = TEXT("Warp");
	const FHyperAIMotionWarpingPlanReport Done = Run(Fixture.Path(), {
		Vault,
		Add(TEXT("Lunge"), 1.0, 1.5, TEXT("WarpRotationTimeMultiplier=0.5")),
		Add(TEXT(""), 1.6, 1.9, TEXT("Scale=1,1,0"), TEXT("scale"))}, false);
	if (!TestTrue(TEXT("the plan runs: ") + Done.Diagnostic, Done.bOk && Done.Status == TEXT("completed"))) return false;
	TestEqual(TEXT("in one undo step"), (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) - UndoBefore, 1);
	TestTrue(TEXT("named for the plan"), GEditor->Trans->GetUndoContext().Title.ToString().Contains(TEXT("Motion Warping Plan")));
	TArray<Gate::FWindowRef> Windows = Fixture.Windows();
	if (!TestEqual(TEXT("three windows on the montage"), Windows.Num(), 3)) return false;
	const URootMotionModifier_SkewWarp* First = Cast<URootMotionModifier_SkewWarp>(Windows[0].State->RootMotionModifier);
	TestTrue(TEXT("the first a skew warp to Vault"), First && First->WarpTargetName == TEXT("Vault"));
	TestTrue(TEXT("with Z warped and facing the target"), First && !First->bIgnoreZAxis && First->RotationType == EMotionWarpRotationType::Facing);
	TestTrue(TEXT("from 0.2 s to 0.8 s"), FMath::IsNearlyEqual(Windows[0].Start, 0.2f, 0.001f) && FMath::IsNearlyEqual(Windows[0].End, 0.8f, 0.001f));
	TestTrue(TEXT("whose notify state is the montage's"), Windows[0].State->GetOuter() == Fixture.Montage);
	TestTrue(TEXT("on the Warp track"), Fixture.Montage->AnimNotifyTracks.IsValidIndex(Windows[0].TrackIndex)
		&& Fixture.Montage->AnimNotifyTracks[Windows[0].TrackIndex].TrackName == TEXT("Warp"));
	const URootMotionModifier_SkewWarp* Second = Cast<URootMotionModifier_SkewWarp>(Windows[1].State->RootMotionModifier);
	TestTrue(TEXT("the second rotates twice as fast"), Second && Second->WarpTargetName == TEXT("Lunge") && FMath::IsNearlyEqual(Second->WarpRotationTimeMultiplier, 0.5f));
	const URootMotionModifier_Scale* Third = Cast<URootMotionModifier_Scale>(Windows[2].State->RootMotionModifier);
	TestTrue(TEXT("the third scales the translation, Z out"), Third && Third->Scale.Equals(FVector(1.0, 1.0, 0.0)));
	TestTrue(TEXT("the reply lists the warp targets"), Done.WarpTargets == TArray<FString>({TEXT("Lunge"), TEXT("Vault")}));
	TestTrue(TEXT("and warns of nothing: root motion is on"), Done.Warnings.IsEmpty());
	// Read back through the tool, as an agent would.
	FHyperAIMotionWarpingInspectRequest Look;
	Look.Asset = Fixture.Path();
	const FHyperAIMotionWarpingInspectReport Read = C::Inspect(Look);
	TestTrue(TEXT("inspect reads the windows: ") + Read.Diagnostic, Read.bOk && Read.Windows.Num() == 3 && Read.bRootMotion && Read.AssetKind == TEXT("montage"));
	TestTrue(TEXT("with their modifier, target, track and settings"), Read.Windows.Num() == 3 && Read.Windows[0].Modifier == TEXT("skew_warp") && Read.Windows[0].WarpTargetName == TEXT("Vault")
		&& Read.Windows[0].Track == TEXT("Warp") && Read.Windows[0].Settings.Contains(TEXT("RotationType=Facing")) && Read.Windows[0].Settings.Contains(TEXT("bIgnoreZAxis=false"))
		&& Read.Windows[2].Modifier == TEXT("scale") && Read.Windows[2].WarpTargetName.IsEmpty());

	// One undo takes the plan back, redo brings it again.
	if (!TestTrue(TEXT("undo"), GEditor->UndoTransaction())) return false;
	TestTrue(TEXT("undo removes the three windows"), Fixture.Windows().IsEmpty());
	if (!TestTrue(TEXT("redo"), GEditor->RedoTransaction())) return false;
	Windows = Fixture.Windows();
	TestTrue(TEXT("redo brings them back whole"), Windows.Num() == 3 && TargetOf(Windows[0]) == TEXT("Vault")
		&& Cast<URootMotionModifier_SkewWarp>(Windows[0].State->RootMotionModifier) && !Cast<URootMotionModifier_SkewWarp>(Windows[0].State->RootMotionModifier)->bIgnoreZAxis);

	// Change one, remove one, give the scale window a skew warp: picked by index and by name, as they were before the plan.
	FHyperAIMotionWarpingOp Retime = Op(TEXT("window.set"), TEXT("WarpTargetName=Mantle"));
	Retime.Index = 0;
	Retime.StartSeconds = 0.1;
	Retime.EndSeconds = 0.7;
	FHyperAIMotionWarpingOp Remove = Op(TEXT("window.remove"));
	Remove.WarpTargetName = TEXT("Lunge");
	FHyperAIMotionWarpingOp Swap = Op(TEXT("window.set"), TEXT("WarpTargetName=Pounce"));
	Swap.Index = 2;
	Swap.Modifier = TEXT("skew_warp");
	const int32 Before = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount());
	const FHyperAIMotionWarpingPlanReport Changed = Run(Fixture.Path(), {Retime, Remove, Swap}, false);
	if (!TestTrue(TEXT("the second plan runs: ") + Changed.Diagnostic, Changed.bOk)) return false;
	TestEqual(TEXT("in one more undo step"), (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) - Before, 1);
	Windows = Fixture.Windows();
	TestTrue(TEXT("two windows: Mantle from 0.1 s and Pounce"), Windows.Num() == 2 && TargetOf(Windows[0]) == TEXT("Mantle") && FMath::IsNearlyEqual(Windows[0].Start, 0.1f, 0.001f)
		&& FMath::IsNearlyEqual(Windows[0].End, 0.7f, 0.001f) && TargetOf(Windows[1]) == TEXT("Pounce"));
	TestTrue(TEXT("Mantle keeps its settings"), Windows.Num() == 2 && Cast<URootMotionModifier_SkewWarp>(Windows[0].State->RootMotionModifier)
		&& Cast<URootMotionModifier_SkewWarp>(Windows[0].State->RootMotionModifier)->RotationType == EMotionWarpRotationType::Facing);
	TestTrue(TEXT("Pounce is a skew warp now"), Windows.Num() == 2 && Windows[1].State->RootMotionModifier && Windows[1].State->RootMotionModifier->IsA<URootMotionModifier_SkewWarp>());
	TestTrue(TEXT("the reply warns of the targets no window uses now"), Warns(Changed.Warnings, TEXT("Vault is used by no window")) && Warns(Changed.Warnings, TEXT("Lunge is used by no window")));
	if (!TestTrue(TEXT("undo the second plan"), GEditor->UndoTransaction())) return false;
	Windows = Fixture.Windows();
	TestTrue(TEXT("puts back the three windows as they were"), Windows.Num() == 3 && TargetOf(Windows[0]) == TEXT("Vault") && FMath::IsNearlyEqual(Windows[0].Start, 0.2f, 0.001f)
		&& Cast<URootMotionModifier_Scale>(Windows[2].State->RootMotionModifier) != nullptr);
	GEditor->UndoTransaction();
	TestTrue(TEXT("and the first undo leaves none"), Fixture.Windows().IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioMotionWarpingComponentTest, "HyperAIStudio.NativeTools.MotionWarping.ComponentApplyAndUndo", HyperAIStudio::MotionWarpingTests::Flags)

bool FHyperAIStudioMotionWarpingComponentTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::MotionWarpingTests;
	if (!GEditor || !GEditor->Trans || GEditor->PlayWorld)
	{
		AddError(TEXT("Needs the editor with its undo buffer, and no play session."));
		return false;
	}
	auto MakeBlueprint = [](UClass* Parent, const TCHAR* Name)
	{
		const FString Path = TestAsset(Name);
		UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(Parent, CreatePackage(*Path), FName(*FPackageName::GetShortName(Path)),
			BPTYPE_Normal, UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		FKismetEditorUtilities::CompileBlueprint(Blueprint);
		return Blueprint;
	};
	UBlueprint* Hero = MakeBlueprint(ACharacter::StaticClass(), TEXT("BP_WarpHero"));
	UBlueprint* Crate = MakeBlueprint(AActor::StaticClass(), TEXT("BP_WarpCrate"));
	const int32 UndoBefore = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount());
	ON_SCOPE_EXIT
	{
		UndoAll(GEditor->Trans->GetQueueLength() - UndoBefore - GEditor->Trans->GetUndoCount());
		// Let go, not deleted: deleting assets resets the undo buffer.
		for (UObject* Object : {static_cast<UObject*>(Hero), static_cast<UObject*>(Crate)})
		{
			if (!IsValid(Object)) continue;
			Object->ClearFlags(RF_Public | RF_Standalone);
			Object->GetPackage()->SetDirtyFlag(false);
		}
	};
	if (!TestTrue(TEXT("the test Blueprints are made"), Hero && Crate)) return false;
	FHyperAIMotionWarpingInspectRequest Look;
	Look.Asset = Hero->GetPathName();
	const FHyperAIMotionWarpingInspectReport Bare = C::Inspect(Look);
	TestTrue(TEXT("inspect finds no component, and warns: ") + Bare.Diagnostic, Bare.bOk && Bare.AssetKind == TEXT("character_blueprint") && !Bare.bHasComponent
		&& Warns(Bare.Warnings, TEXT("no Motion Warping component")));

	const FHyperAIMotionWarpingPlanReport Done = Run(Hero->GetPathName(), {Op(TEXT("component.add"), TEXT("bSearchForWindowsInAnimsWithinMontages=true"))}, false);
	if (!TestTrue(TEXT("component.add runs: ") + Done.Diagnostic, Done.bOk && Done.Status == TEXT("completed"))) return false;
	TestEqual(TEXT("in one undo step"), (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) - UndoBefore, 1);
	TestTrue(TEXT("named for the plan"), GEditor->Trans->GetUndoContext().Title.ToString().Contains(TEXT("Motion Warping Plan")));
	USCS_Node* Node = WarpNode(*Hero);
	if (!TestNotNull(TEXT("the construction script has the component"), Node)) return false;
	TestEqual(TEXT("named MotionWarping"), Node->GetVariableName().ToString(), FString(TEXT("MotionWarping")));
	TestTrue(TEXT("searching the montages' sequences"), CastChecked<UMotionWarpingComponent>(Node->ComponentTemplate)->bSearchForWindowsInAnimsWithinMontages);
	TestTrue(TEXT("the Blueprint compiled"), Hero->Status == BS_UpToDate || Hero->Status == BS_UpToDateWithWarnings);
	const FHyperAIMotionWarpingInspectReport Read = C::Inspect(Look);
	TestTrue(TEXT("inspect reads it back: ") + Read.Diagnostic, Read.bOk && Read.bHasComponent && Read.ComponentName == TEXT("MotionWarping") && Read.ComponentSource == TEXT("this Blueprint")
		&& Read.ComponentSettings.Contains(TEXT("bSearchForWindowsInAnimsWithinMontages=true")));
	TestTrue(TEXT("a second component.add is refused"), Refused(Run(Hero->GetPathName(), {Op(TEXT("component.add"))}, false), TEXT("already has a Motion Warping component")));

	const int32 BeforeSet = (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount());
	const FHyperAIMotionWarpingPlanReport Set = Run(Hero->GetPathName(), {Op(TEXT("component.set"), TEXT("bSearchForWindowsInAnimsWithinMontages=false"))}, false);
	TestTrue(TEXT("component.set runs: ") + Set.Diagnostic, Set.bOk && (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) - BeforeSet == 1);
	TestFalse(TEXT("and turns the search off"), CastChecked<UMotionWarpingComponent>(Node->ComponentTemplate)->bSearchForWindowsInAnimsWithinMontages);
	if (!TestTrue(TEXT("undo the set"), GEditor->UndoTransaction())) return false;
	TestTrue(TEXT("puts the search back on"), CastChecked<UMotionWarpingComponent>(Node->ComponentTemplate)->bSearchForWindowsInAnimsWithinMontages);
	if (!TestTrue(TEXT("undo the add"), GEditor->UndoTransaction())) return false;
	TestNull(TEXT("takes the component out of the construction script"), WarpNode(*Hero));
	TestFalse(TEXT("and inspect sees none"), C::Inspect(Look).bHasComponent);
	if (!TestTrue(TEXT("redo"), GEditor->RedoTransaction())) return false;
	TestNotNull(TEXT("brings it back"), WarpNode(*Hero));
	GEditor->UndoTransaction();

	TestTrue(TEXT("a Blueprint that is not a character is refused"), Refused(Run(Crate->GetPathName(), {Op(TEXT("component.add"))}, false), TEXT("is not a Character")));
	TestTrue(TEXT("component.set without a component is refused"), Refused(Run(Hero->GetPathName(), {Op(TEXT("component.set"), TEXT("bSearchForWindowsInAnimsWithinMontages=true"))}, false),
		TEXT("add it with component.add first")));
	TestTrue(TEXT("window ops on a Blueprint are refused"), Refused(Run(Hero->GetPathName(), {Add(TEXT("Vault"), 0.1, 0.2)}, false), TEXT("window ops take a montage")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioMotionWarpingRefusalsTest, "HyperAIStudio.NativeTools.MotionWarping.Refusals", HyperAIStudio::MotionWarpingTests::Flags)

bool FHyperAIStudioMotionWarpingRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::MotionWarpingTests;
	FFixture Fixture(/*bRootMotion=*/false);
	if (!TestNotNull(TEXT("the test montage is made"), Fixture.Montage)) return false;
	const FString Path = Fixture.Path();
	const int32 UndoBefore = GEditor && GEditor->Trans ? (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) : 0;
	TestTrue(TEXT("Engine content is refused"), Refused(Run(TEXT("/Engine/Animation/AM_Nothing"), {Add(TEXT("Vault"), 0.1, 0.5)}, false), TEXT("not project content")));
	TestTrue(TEXT("a missing asset is refused"), Refused(Run(TEXT("/Game/Nowhere/AM_Missing"), {Add(TEXT("Vault"), 0.1, 0.5)}, false), TEXT("is not there")));
	TestTrue(TEXT("an unknown kind is refused, with the kinds"), Refused(Run(Path, {Op(TEXT("window.explode"))}, false), TEXT("window.add")));
	TestTrue(TEXT("an unknown modifier is refused, with the modifiers"), Refused(Run(Path, {Add(TEXT("Vault"), 0.1, 0.5, TEXT(""), TEXT("teleport"))}, false), TEXT("skew_warp")));
	TestTrue(TEXT("a bad setting is refused"), Refused(Run(Path, {Add(TEXT("Vault"), 0.1, 0.5, TEXT("RotationType=Sideways"))}, false), TEXT("takes one of")));
	TestTrue(TEXT("an unknown setting is refused"), Refused(Run(Path, {Add(TEXT("Vault"), 0.1, 0.5, TEXT("Explode=true"))}, false), TEXT("takes no setting 'Explode'")));
	TestTrue(TEXT("a missing referenced asset is refused"), Refused(Run(Path, {Add(TEXT("Vault"), 0.1, 0.5, TEXT("AddTranslationEasingCurve=/Game/Nowhere/Curve_Missing"))}, false),
		TEXT("Curve_Missing")));
	TestTrue(TEXT("a window past the end is refused"), Refused(Run(Path, {Add(TEXT("Vault"), 1.5, 2.5)}, false), TEXT("runs past its end")));
	TestTrue(TEXT("a window that ends before it starts is refused"), Refused(Run(Path, {Add(TEXT("Vault"), 0.5, 0.2)}, false), TEXT("StartSeconds < EndSeconds")));
	TestTrue(TEXT("a warp window needs its target"), Refused(Run(Path, {Add(TEXT(""), 0.1, 0.5)}, false), TEXT("needs WarpTargetName")));
	TestTrue(TEXT("scale takes none"), Refused(Run(Path, {Add(TEXT("Vault"), 0.1, 0.5, TEXT(""), TEXT("scale"))}, false), TEXT("takes no warp target")));
	FHyperAIMotionWarpingOp Missing = Op(TEXT("window.set"), TEXT("bIgnoreZAxis=false"));
	Missing.Index = 4;
	TestTrue(TEXT("a window that is not there is refused"), Refused(Run(Path, {Missing}, false), TEXT("has no Motion Warping window")));
	FHyperAIMotionWarpingOp ByName = Op(TEXT("window.remove"));
	ByName.WarpTargetName = TEXT("Nobody");
	TestTrue(TEXT("so is one picked by a name no window has"), Refused(Run(Path, {ByName}, false), TEXT("No window warps to Nobody")));
	TestTrue(TEXT("component ops on a montage are refused"), Refused(Run(Path, {Op(TEXT("component.add"))}, false), TEXT("component ops take a character Blueprint")));
	TestTrue(TEXT("an empty plan is refused"), Refused(Run(Path, {}, false), TEXT("1 to 32 operations")));
	TestTrue(TEXT("refused plans change nothing"), Fixture.Windows().IsEmpty() && (!GEditor || !GEditor->Trans || (GEditor->Trans->GetQueueLength() - GEditor->Trans->GetUndoCount()) == UndoBefore));

	// The warnings: no root motion on the montage's sequence.
	const FHyperAIMotionWarpingPlanReport Checked = Run(Path, {Add(TEXT("Vault"), 0.1, 0.5)}, true);
	TestTrue(TEXT("a montage without root motion is warned of: ") + FString::Join(Checked.Warnings, TEXT(" | ")), Checked.bOk && Warns(Checked.Warnings, TEXT("no sequence with root motion on")));
	FHyperAIMotionWarpingInspectRequest Look;
	Look.Asset = Path;
	const FHyperAIMotionWarpingInspectReport Read = C::Inspect(Look);
	TestTrue(TEXT("inspect says root motion is off"), Read.bOk && !Read.bRootMotion && Warns(Read.Warnings, TEXT("root motion")));
	FHyperAIMotionWarpingInspectRequest Sequence;
	Sequence.Asset = Fixture.Sequence->GetPathName();
	const FHyperAIMotionWarpingInspectReport SequenceRead = C::Inspect(Sequence);
	TestTrue(TEXT("a sequence reads too: ") + SequenceRead.Diagnostic, SequenceRead.bOk && SequenceRead.AssetKind == TEXT("sequence") && !SequenceRead.bRootMotion);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FHyperAIStudioMotionWarpingSkillTest, "HyperAIStudio.NativeTools.MotionWarping.Skill", HyperAIStudio::MotionWarpingTests::Flags)

bool FHyperAIStudioMotionWarpingSkillTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::MotionWarpingTests;
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("HyperAIStudio"));
	FString Skill;
	if (!TestTrue(TEXT("the motion warping skill ships with the plugin"), Plugin.IsValid()
		&& FFileHelper::LoadFileToString(Skill, *FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources/Skills/unreal-motion-warping/SKILL.md")))))
	{
		return false;
	}
	TestTrue(TEXT("it is a skill"), Skill.StartsWith(TEXT("---")) && Skill.Contains(TEXT("name: unreal-motion-warping")));
	for (const FString& Name : C::GetToolNames())
	{
		TestTrue(FString::Printf(TEXT("it names %s"), *Name), Skill.Contains(FString::Printf(TEXT("`%s`"), *Name)));
	}
	for (const Gate::FKindInfo& Info : Gate::Kinds())
	{
		TestTrue(FString::Printf(TEXT("it explains %s"), Info.Kind), Skill.Contains(FString::Printf(TEXT("`%s`"), Info.Kind)));
	}
	for (const Gate::FModifierInfo& Info : Gate::Modifiers())
	{
		TestTrue(FString::Printf(TEXT("and the modifier %s"), Info.Word), Skill.Contains(FString::Printf(TEXT("`%s`"), Info.Word)));
	}
	for (const TCHAR* Call : {TEXT("AddOrUpdateWarpTargetFromLocation"), TEXT("AddOrUpdateWarpTargetFromLocationAndRotation"), TEXT("AddOrUpdateWarpTargetFromTransform"),
		TEXT("AddOrUpdateWarpTargetFromComponent"), TEXT("RemoveWarpTarget"), TEXT("hyper_animation_apply_plan"), TEXT("hyper_animation_inspect")})
	{
		TestTrue(FString::Printf(TEXT("and %s"), Call), Skill.Contains(Call));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
