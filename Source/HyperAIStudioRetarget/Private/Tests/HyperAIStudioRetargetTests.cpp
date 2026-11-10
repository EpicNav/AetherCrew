// Games by Hyper 2026.

#if WITH_DEV_AUTOMATION_TESTS

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioRetargetToolset.h"
#include "HyperAIStudioSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "UObject/Package.h"

namespace HyperAIStudio::RetargetTests
{
	using C = FHyperAIStudioRetargetContracts;

	FString ObjectPathOf(const FString& Package)
	{
		return Package + TEXT(".") + FPackageName::GetShortName(Package);
	}

	FHyperAIRetargetOp Op(const TCHAR* Kind, const FString& Asset)
	{
		FHyperAIRetargetOp Out;
		Out.Kind = Kind;
		Out.Asset = Asset;
		return Out;
	}

	FHyperAIRetargetPlanReport Run(TArray<FHyperAIRetargetOp> Ops, const bool bDryRun = false)
	{
		FHyperAIRetargetPlanRequest Plan;
		Plan.Operations = MoveTemp(Ops);
		Plan.bDryRun = bDryRun;
		return C::ApplyPlan(Plan);
	}

	FHyperAIRetargetAssetInfo Inspect(const FString& Path)
	{
		FHyperAIRetargetInspectRequest Look;
		Look.Paths = {Path};
		const FHyperAIRetargetInspectReport Report = C::Inspect(Look);
		return Report.bOk && Report.Assets.Num() == 1 ? Report.Assets[0] : FHyperAIRetargetAssetInfo();
	}

	bool IsProjectAsset(const FAssetData& Asset)
	{
		const FString Package = Asset.PackageName.ToString();
		return Package.StartsWith(TEXT("/Game/")) && !Package.StartsWith(TEXT("/Game/__"));
	}

	/** Two skeletal meshes of one skeleton under /Game and a clip of that skeleton: the Third Person template's Manny and Quinn are. */
	bool FindCast(USkeletalMesh*& OutSource, USkeletalMesh*& OutTarget, UAnimSequence*& OutClip)
	{
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		auto Sorted = [&Registry](const UClass* Class)
		{
			TArray<FAssetData> Assets;
			Registry.GetAssetsByClass(Class->GetClassPathName(), Assets);
			Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });
			return Assets;
		};
		TMap<const USkeleton*, TArray<USkeletalMesh*>> BySkeleton;
		for (const FAssetData& Asset : Sorted(USkeletalMesh::StaticClass()))
		{
			USkeletalMesh* Mesh = IsProjectAsset(Asset) ? Cast<USkeletalMesh>(Asset.GetAsset()) : nullptr;
			if (!Mesh || !Mesh->GetSkeleton())
			{
				continue;
			}
			TArray<USkeletalMesh*>& Found = BySkeleton.FindOrAdd(Mesh->GetSkeleton());
			Found.Add(Mesh);
			if (Found.Num() != 2)
			{
				continue;
			}
			for (const FAssetData& ClipAsset : Sorted(UAnimSequence::StaticClass()))
			{
				UAnimSequence* Clip = IsProjectAsset(ClipAsset) ? Cast<UAnimSequence>(ClipAsset.GetAsset()) : nullptr;
				if (Clip && !Clip->IsValidAdditive() && Clip->GetSkeleton() == Mesh->GetSkeleton())
				{
					OutSource = Found[0];
					OutTarget = Found[1];
					OutClip = Clip;
					return true;
				}
			}
		}
		return false;
	}

	/** The first bone below the mesh's second bone (spine_01 under pelvis on a mannequin), or the second bone itself. */
	FString GrandchildBone(const USkeletalMesh& Mesh)
	{
		const FReferenceSkeleton& Bones = Mesh.GetRefSkeleton();
		for (int32 Index = 2; Index < Bones.GetNum(); ++Index)
		{
			if (Bones.GetParentIndex(Index) == 1)
			{
				return Bones.GetBoneName(Index).ToString();
			}
		}
		return Bones.GetBoneName(FMath::Min(1, Bones.GetNum() - 1)).ToString();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FHyperAIStudioRetargetEndToEndTest,
	"HyperAIStudio.NativeTools.AnimationRigging.RetargetEndToEnd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FHyperAIStudioRetargetEndToEndTest::RunTest(const FString& Parameters)
{
	using namespace HyperAIStudio::RetargetTests;
	USkeletalMesh* SourceMesh = nullptr;
	USkeletalMesh* TargetMesh = nullptr;
	UAnimSequence* Clip = nullptr;
	if (!FindCast(SourceMesh, TargetMesh, Clip))
	{
		AddInfo(TEXT("Skipped: no skeleton under /Game has two skeletal meshes and a non-additive Animation Sequence."));
		return true;
	}
	const FString Folder = TEXT("/Game/__HyperAIStudioTests/Retarget_") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8).ToLower();
	const FString SourceRig = Folder + TEXT("/IK_Source");
	const FString TargetRig = Folder + TEXT("/IK_Target");
	const FString Retargeter = Folder + TEXT("/RTG_SourceToTarget");
	const FString Output = Folder + TEXT("/Anims");
	TArray<FString> Made = {ObjectPathOf(SourceRig), ObjectPathOf(TargetRig), ObjectPathOf(Retargeter)};
	UHyperAIStudioSettings* Settings = GetMutableDefault<UHyperAIStudioSettings>();
	const bool bApproval = Settings->bRequireApprovalForAgentEdits;
	Settings->bRequireApprovalForAgentEdits = false;
	ON_SCOPE_EXIT
	{
		Settings->bRequireApprovalForAgentEdits = bApproval;
		TArray<UObject*> Live;
		for (const FString& Path : Made)
		{
			UObject* Object = FSoftObjectPath(Path).ResolveObject();
			if (IsValid(Object))
			{
				Live.AddUnique(Object);
			}
		}
		if (!Live.IsEmpty())
		{
			ObjectTools::ForceDeleteObjects(Live, /*ShowConfirmation=*/false);
		}
	};

	FHyperAIRetargetOp MakeSource = Op(TEXT("ik_rig.create"), SourceRig);
	MakeSource.Mesh = SourceMesh->GetPathName();
	FHyperAIRetargetOp MakeTarget = Op(TEXT("ik_rig.create"), TargetRig);
	MakeTarget.Mesh = TargetMesh->GetPathName();
	FHyperAIRetargetOp MakeRetargeter = Op(TEXT("retargeter.create"), Retargeter);
	MakeRetargeter.SourceRig = SourceRig;
	MakeRetargeter.TargetRig = TargetRig;
	FHyperAIRetargetOp Copy = Op(TEXT("retarget.animations"), Retargeter);
	Copy.Animations = {Clip->GetPathName()};
	Copy.OutputFolder = Output;
	Copy.Suffix = TEXT("_Retargeted");

	// Refused before anything changes: a bone the mesh lacks, a rig nobody makes, a mesh retargeted onto itself.
	const FString RootBone = TargetMesh->GetRefSkeleton().GetBoneName(0).ToString();
	FHyperAIRetargetOp NoSuchBone = Op(TEXT("ik_rig.add_chain"), TargetRig);
	NoSuchBone.Chain = TEXT("HyperAITail");
	NoSuchBone.StartBone = RootBone;
	NoSuchBone.EndBone = TEXT("definitely_not_a_bone");
	const FHyperAIRetargetPlanReport Refused = Run({MakeSource, MakeTarget, NoSuchBone});
	TestTrue(TEXT("A bone the mesh lacks is refused by name, and no rig is made: ") + Refused.Diagnostic, !Refused.bOk
		&& Refused.Status == TEXT("invalid_request") && Refused.Diagnostic.Contains(TEXT("definitely_not_a_bone"))
		&& !FindObject<UObject>(nullptr, *ObjectPathOf(SourceRig)) && !FPackageName::DoesPackageExist(SourceRig));
	FHyperAIRetargetOp Orphan = MakeRetargeter;
	Orphan.SourceRig = Folder + TEXT("/IK_Nobody");
	TestFalse(TEXT("A retargeter of a rig that does not exist is refused"), Run({MakeTarget, Orphan}, /*bDryRun=*/true).bOk);
	FHyperAIRetargetOp OntoItself = Copy;
	OntoItself.SourceMesh = SourceMesh->GetPathName();
	OntoItself.TargetMesh = SourceMesh->GetPathName();
	TestFalse(TEXT("and so is retargeting a mesh onto itself"), Run({MakeSource, MakeTarget, MakeRetargeter, OntoItself}, /*bDryRun=*/true).bOk);

	// Replace goes in for each match of Search, so a short Replace can carry a copy's package past an FName's length. In
	// this folder the copy's path fits with one replacement and not with two.
	const FString ClipName = Clip->GetName();
	FString Search;
	int32 Matches = 0;
	for (const TCHAR Char : ClipName)
	{
		const FString One = FString::ChrN(1, Char);
		const int32 Count = ClipName.Replace(*One, TEXT("..")).Len() - ClipName.Len();
		if (Count > Matches)
		{
			Matches = Count;
			Search = One;
		}
	}
	FHyperAIRetargetOp Lengthened = Copy;
	Lengthened.Suffix.Reset();
	Lengthened.Replace = FString::ChrN(100, TEXT('x'));
	const int32 FolderLength = FHyperAIStudioNames::MaxLength - 1 - ClipName.Len() - (Lengthened.Replace.Len() - 1);
	Lengthened.OutputFolder = Folder + TEXT("/") + FString::ChrN(FolderLength - Folder.Len() - 1, TEXT('a'));
	const FHyperAIRetargetPlanReport LongFolder = Run({MakeSource, MakeTarget, MakeRetargeter, Lengthened}, /*bDryRun=*/true);
	TestTrue(TEXT("A long output folder checks while the names fit it: ") + LongFolder.Diagnostic, LongFolder.bOk);
	if (Matches >= 2)
	{
		Lengthened.Search = Search;
		const FHyperAIRetargetPlanReport Grown = Run({MakeSource, MakeTarget, MakeRetargeter, Lengthened}, /*bDryRun=*/true);
		TestTrue(FString::Printf(TEXT("and a Replace that goes in %d times is refused before the batch names a package past the rule: %s"), Matches, *Grown.Diagnostic),
			!Grown.bOk && Grown.Status == TEXT("invalid_request") && Grown.Diagnostic.Contains(TEXT("characters long")));
	}
	else
	{
		AddInfo(FString::Printf(TEXT("%s repeats no character, so the growth from repeated replacement is not shown."), *ClipName));
	}
	const FHyperAIRetargetPlanReport Checked = Run({MakeSource, MakeTarget, MakeRetargeter, Copy}, /*bDryRun=*/true);
	TestTrue(TEXT("The whole plan checks: ") + Checked.Diagnostic, Checked.bOk && Checked.Status == TEXT("checked")
		&& !FindObject<UObject>(nullptr, *ObjectPathOf(SourceRig)));

	// One plan: two rigs named automatically, a retargeter set up as Epic's auto-retarget sets one up, and a clip copied across.
	const FHyperAIRetargetPlanReport Done = Run({MakeSource, MakeTarget, MakeRetargeter, Copy});
	Made.Append(Done.Saved);
	if (!TestTrue(TEXT("Rigs, retargeter and a retargeted clip in one plan: ") + Done.Diagnostic + TEXT(" | ") + FString::Join(Done.Notes, TEXT(" | ")),
		Done.bOk && Done.Status == TEXT("completed") && Done.Made.Num() == 3 && !Done.Retargeted.IsEmpty()))
	{
		return false;
	}
	const FHyperAIRetargetAssetInfo Rig = Inspect(TargetRig);
	TestTrue(TEXT("The target rig has its mesh, a root and named chains: ") + FString::Join(Rig.Chains, TEXT(", ")), Rig.Kind == TEXT("ik_rig")
		&& Rig.Mesh == TargetMesh->GetPathName() && !Rig.RetargetRoot.IsEmpty() && Rig.Chains.Num() >= 5);
	const FHyperAIRetargetAssetInfo Setup = Inspect(Retargeter);
	TestTrue(TEXT("The retargeter runs the default ops with the IK pass off: ") + FString::Join(Setup.Ops, TEXT(", ")), Setup.Kind == TEXT("retargeter")
		&& Setup.SourceMesh == SourceMesh->GetPathName() && Setup.TargetMesh == TargetMesh->GetPathName()
		&& Setup.Ops.ContainsByPredicate([](const FString& Line) { return Line.Contains(TEXT("(FIKRetargetRunIKRigOp), off")); })
		&& Setup.Ops.ContainsByPredicate([](const FString& Line) { return Line.EndsWith(TEXT("(FIKRetargetFKChainsOp)")); }));
	TestTrue(TEXT("and with one skeleton on both sides every chain maps by name: ") + FString::Join(Setup.ChainMap, TEXT(", ")),
		Setup.ChainMap.Num() == Rig.Chains.Num() && !Setup.ChainMap.ContainsByPredicate([](const FString& Line) { return Line.EndsWith(TEXT("(nothing)")); }));
	const UAnimSequence* Copied = LoadObject<UAnimSequence>(nullptr, *Done.Retargeted[0], nullptr, LOAD_NoWarn | LOAD_Quiet);
	TestTrue(TEXT("The retargeted clip is a new, saved sequence in the output folder: ") + Done.Retargeted[0], Copied && Copied != Clip
		&& Done.Retargeted[0].StartsWith(Output + TEXT("/")) && Done.Retargeted[0].Contains(TEXT("_Retargeted"))
		&& Done.Saved.Contains(Copied->GetPathName()) && FPackageName::DoesPackageExist(Copied->GetOutermost()->GetName()));

	// A second plan edits them: a chain added then moved, the IK pass turned on, one chain unmapped. One undo step takes it back.
	const FString SecondBone = TargetMesh->GetRefSkeleton().GetBoneName(1).ToString();
	const FString ThirdBone = GrandchildBone(*TargetMesh);
	FHyperAIRetargetOp AddTail = Op(TEXT("ik_rig.add_chain"), TargetRig);
	AddTail.Chain = TEXT("HyperAITail");
	AddTail.StartBone = RootBone;
	AddTail.EndBone = SecondBone;
	FHyperAIRetargetOp Longer = Op(TEXT("ik_rig.set_chain"), TargetRig);
	Longer.Chain = TEXT("HyperAITail");
	Longer.EndBone = ThirdBone;
	FHyperAIRetargetOp IKOn = Op(TEXT("retargeter.enable_op"), Retargeter);
	IKOn.OpName = TEXT("IKRetargetRunIKRigOp");
	FString FirstChain;
	Rig.Chains[0].Split(TEXT(":"), &FirstChain, nullptr);
	FHyperAIRetargetOp Unmap = Op(TEXT("retargeter.map_chain"), Retargeter);
	Unmap.TargetChain = FirstChain;
	FHyperAIRetargetOp Twice = AddTail;
	TestFalse(TEXT("A chain the rig has already is refused"), Run({AddTail, Twice}, /*bDryRun=*/true).bOk);
	FHyperAIRetargetOp NoSuchOp = IKOn;
	NoSuchOp.OpName = TEXT("Make It Look Good");
	TestFalse(TEXT("and so is an op the retargeter does not have"), Run({NoSuchOp}, /*bDryRun=*/true).bOk);

	const FHyperAIRetargetPlanReport Edited = Run({AddTail, Longer, IKOn, Unmap});
	TestTrue(TEXT("The edit plan applies: ") + Edited.Diagnostic, Edited.bOk && Edited.Applied == 4);
	const FString Tail = FString::Printf(TEXT("HyperAITail: %s -> %s"), *RootBone, *ThirdBone);
	const FHyperAIRetargetAssetInfo RigAfter = Inspect(TargetRig);
	TestTrue(TEXT("The new chain reads back with its moved end bone: ") + FString::Join(RigAfter.Chains, TEXT(", ")), RigAfter.Chains.Contains(Tail));
	const FHyperAIRetargetAssetInfo SetupAfter = Inspect(Retargeter);
	TestTrue(TEXT("The IK pass is on and the first chain unmapped: ") + FString::Join(SetupAfter.Ops, TEXT(", ")) + TEXT(" | ") + FString::Join(SetupAfter.ChainMap, TEXT(", ")),
		SetupAfter.Ops.ContainsByPredicate([](const FString& Line) { return Line.EndsWith(TEXT("(FIKRetargetRunIKRigOp)")); })
		&& SetupAfter.ChainMap.Contains(FirstChain + TEXT(" <- (nothing)")));
	if (GEditor)
	{
		GEditor->UndoTransaction();
		TestFalse(TEXT("Edit > Undo takes the whole edit plan back: ") + FString::Join(Inspect(TargetRig).Chains, TEXT(", ")),
			Inspect(TargetRig).Chains.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("HyperAITail:")); }));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
