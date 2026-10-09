// Games by Hyper 2026.

#include "HyperAIStudioRetargetToolset.h"

#include "Animation/AnimationAsset.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSourceControl.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/IKRetargetSettings.h"
#include "Retargeter/RetargetOps/PinBoneOp.h"
#include "Retargeter/RetargetOps/RootMotionGeneratorOp.h"
#include "Retargeter/RetargetOps/RunIKRigOp.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "Rig/IKRigDefinition.h"
#include "RigEditor/IKRigAutoCharacterizer.h"
#include "RigEditor/IKRigController.h"
#include "ScopedTransaction.h"
#include "StructUtils/InstancedStruct.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioRetarget, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioRetarget"

FString FHyperAIStudioRetargetContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioRetarget.HyperAIStudioRetargetToolset");
}

TArray<FString> FHyperAIStudioRetargetContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName};
}

bool FHyperAIStudioRetargetContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::Retarget::Private
{
	using C = FHyperAIStudioRetargetContracts;

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

	FString ObjectPath(const FString& PackageOrObject)
	{
		const FString Trimmed = PackageOrObject.TrimStartAndEnd();
		return Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
	}

	/** How a plan names an asset, whatever the case or object suffix. */
	FString KeyOf(const FString& Path)
	{
		return PackageOf(Path).ToLower();
	}

	FString KindOf(const FHyperAIRetargetOp& Op)
	{
		return Op.Kind.TrimStartAndEnd().ToLower();
	}

	bool IsProjectPath(const FString& Path, FString& OutError)
	{
		const FString Package = PackageOf(Path);
		FText Reason;
		FString NameReason;
		if (!FHyperAIStudioNames::IsObjectPath(Path.TrimStartAndEnd(), TEXT("The path"), &NameReason))
		{
			OutError = NameReason;
			return false;
		}
		if (!FPackageName::IsValidLongPackageName(Package, /*bIncludeReadOnlyRoots=*/false, &Reason)
			|| Package.StartsWith(TEXT("/Script/")) || Package.StartsWith(TEXT("/Engine/")) || Package.StartsWith(TEXT("/Temp/")))
		{
			OutError = FString::Printf(TEXT("'%s' is not a path in the project, such as /Game/Retarget/IK_Hero. %s"), *Path, *Reason.ToString());
			return false;
		}
		return true;
	}

	bool IsNewAsset(const FString& Path, FString& OutError)
	{
		if (!IsProjectPath(Path, OutError))
		{
			return false;
		}
		const FString Package = PackageOf(Path);
		if (FindObject<UObject>(nullptr, *ObjectPath(Package)) || FPackageName::DoesPackageExist(Package))
		{
			OutError = FString::Printf(TEXT("%s exists already."), *Package);
			return false;
		}
		return true;
	}

	template <typename T>
	T* Load(const FString& Path)
	{
		// The load makes an FName of each part, and the package it looked for: past 1023 characters, or with "//", either
		// stops the editor.
		return !FHyperAIStudioNames::IsObjectPath(Path.TrimStartAndEnd(), nullptr) ? nullptr : LoadObject<T>(nullptr, *ObjectPath(Path), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/**
	 * Whether every copy retarget.animations makes fits the name rule. The batch names a copy as the engine's duplication
	 * rule does, Prefix + the name with every Search replaced + Suffix, in OutputFolder or the animation's own folder, and
	 * makes FNames of that package and path: each replacement lengthens the name, so it is worked out per animation, and
	 * for what a montage or blend space brings along when bIncludeReferenced is set.
	 */
	bool CopiesFitNames(const FHyperAIRetargetOp& Op, FString& OutError)
	{
		TArray<UAnimationAsset*> Copied;
		for (const FString& Path : Op.Animations)
		{
			if (UAnimationAsset* Animation = Load<UAnimationAsset>(Path))
			{
				Copied.AddUnique(Animation);
			}
		}
		// Found as the batch finds them; each adds what it references to the end of the list.
		for (int32 Index = 0; Op.bIncludeReferenced && Index < Copied.Num(); ++Index)
		{
			Copied[Index]->HandleAnimReferenceCollection(Copied, /*bRecursive=*/true);
		}
		const FString Folder = Op.OutputFolder.TrimStartAndEnd();
		for (const UAnimationAsset* Animation : Copied)
		{
			if (!IsValid(Animation))
			{
				continue;
			}
			// FString::Replace, as the rule calls it: every match, whatever its case.
			const FString Name = Op.Prefix + (Op.Search.IsEmpty() ? Animation->GetName() : Animation->GetName().Replace(*Op.Search, *Op.Replace)) + Op.Suffix;
			const FString Package = (Folder.IsEmpty() ? FPackageName::GetLongPackagePath(Animation->GetPathName()) : Folder) + TEXT("/") + Name;
			if (Package.Len() > FHyperAIStudioNames::MaxLength)
			{
				OutError = FString::Printf(TEXT("The copy of %s would be %s..., %d characters long; a path is at most %d. Shorten OutputFolder, Prefix or Suffix%s."),
					*Animation->GetName(), *Package.Left(80), Package.Len(), FHyperAIStudioNames::MaxLength,
					Op.Search.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", or Replace, which goes in for each '%s' in the name"), *Op.Search));
				return false;
			}
		}
		return true;
	}

	bool HasBone(const USkeletalMesh* Mesh, const FString& Bone)
	{
		return Mesh && !Bone.TrimStartAndEnd().IsEmpty() && Mesh->GetRefSkeleton().FindBoneIndex(FName(*Bone.TrimStartAndEnd())) != INDEX_NONE;
	}

	FString NameOf(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString(TEXT("none"));
	}

	FName NameOrNone(const FString& Name)
	{
		const FString Trimmed = Name.TrimStartAndEnd();
		return Trimmed.IsEmpty() ? NAME_None : FName(*Trimmed);
	}

	/** What Epic's auto-characterizer makes of a rig's mesh: the template it matches, its chains, the feet to keep flat, the bones to pin. */
	FAutoCharacterizeResults Characterize(const UIKRigDefinition& Rig)
	{
		FAutoCharacterizeResults Results;
		if (const UIKRigController* Controller = UIKRigController::GetController(&Rig))
		{
			Controller->AutoGenerateRetargetDefinition(Results);
		}
		return Results;
	}

	FString DescribeTemplate(const FAutoCharacterizeResults& Results)
	{
		if (!Results.bUsedTemplate)
		{
			return Results.AutoRetargetDefinition.RetargetDefinition.BoneChains.IsEmpty() ? FString(TEXT("none (no skeleton to match)"))
				: FString(TEXT("none matched; automatic chains are guessed from bone names"));
		}
		return FString::Printf(TEXT("%s (%d%% of its bones matched%s)"), *Results.BestTemplateName.ToString(),
			FMath::RoundToInt(Results.BestPercentageOfTemplateScore * 100.f),
			Results.MissingBones.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", %d missing"), Results.MissingBones.Num()));
	}

	/** Epic's auto-characterizer names the chains and the pelvis: from a known skeleton's template, or guessed from the bone names. */
	bool AutoChains(const UIKRigController& Controller, const FString& RigName, TArray<FString>& OutNotes)
	{
		FAutoCharacterizeResults Results;
		Controller.AutoGenerateRetargetDefinition(Results);
		const FRetargetDefinition& Definition = Results.AutoRetargetDefinition.RetargetDefinition;
		if (Definition.BoneChains.IsEmpty())
		{
			return false;
		}
		Controller.SetRetargetDefinition(Definition);
		OutNotes.Add(Results.bUsedTemplate
			? FString::Printf(TEXT("%s: %d chains from Epic's %s template."), *RigName, Definition.BoneChains.Num(), *DescribeTemplate(Results))
			: FString::Printf(TEXT("%s: no known skeleton matched, so %d chains were guessed from the bone names; check them with hyper_retarget_inspect."),
				*RigName, Definition.BoneChains.Num()));
		return true;
	}

	TArray<FString> ChainLines(const UIKRigController& Controller)
	{
		TArray<FString> Lines;
		for (const FBoneChain& Chain : Controller.GetRetargetChains())
		{
			Lines.Add(FString::Printf(TEXT("%s: %s -> %s%s"), *Chain.ChainName.ToString(), *Chain.StartBone.BoneName.ToString(), *Chain.EndBone.BoneName.ToString(),
				Chain.IKGoalName.IsNone() ? TEXT("") : *FString::Printf(TEXT(" (goal %s)"), *Chain.IKGoalName.ToString())));
		}
		return Lines;
	}

	/** The chain map of the first op that has one (FK Chains, in the default stack); the controller's own lookup stops at the first op. */
	const FRetargetChainMapping* ChainMappingOf(const UIKRetargeterController& Controller)
	{
		for (int32 Index = 0; Index < Controller.GetNumRetargetOps(); ++Index)
		{
			if (const FRetargetChainMapping* Mapping = Controller.GetChainMapping(Controller.GetOpName(Index)))
			{
				return Mapping;
			}
		}
		return nullptr;
	}

	TArray<FString> ChainMapLines(const UIKRetargeterController& Controller, TArray<FString>* OutUnmapped = nullptr)
	{
		TArray<FString> Lines;
		if (const FRetargetChainMapping* Mapping = ChainMappingOf(Controller))
		{
			for (const FRetargetChainPair& Pair : Mapping->GetChainPairs())
			{
				Lines.Add(FString::Printf(TEXT("%s <- %s"), *Pair.TargetChainName.ToString(),
					Pair.SourceChainName.IsNone() ? TEXT("(nothing)") : *Pair.SourceChainName.ToString()));
				if (OutUnmapped && Pair.SourceChainName.IsNone())
				{
					OutUnmapped->Add(Pair.TargetChainName.ToString());
				}
			}
		}
		return Lines;
	}

	FString OpType(const UIKRetargeterController& Controller, const int32 Index)
	{
		const FInstancedStruct* Op = Controller.GetRetargetOpStructAtIndex(Index);
		return Op && Op->GetScriptStruct() ? Op->GetScriptStruct()->GetStructCPPName() : FString();
	}

	/** An op by its name (Run IK Rig) or type (FIKRetargetRunIKRigOp, or without the F). */
	int32 FindOp(const UIKRetargeterController& Controller, const FString& NameOrType)
	{
		const FString Wanted = NameOrType.TrimStartAndEnd();
		for (int32 Index = 0; Index < Controller.GetNumRetargetOps(); ++Index)
		{
			const FString Type = OpType(Controller, Index);
			if (Controller.GetOpName(Index).ToString().Equals(Wanted, ESearchCase::IgnoreCase) || Type.Equals(Wanted, ESearchCase::IgnoreCase)
				|| (Type.StartsWith(TEXT("F")) && Type.Mid(1).Equals(Wanted, ESearchCase::IgnoreCase)))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	int32 FindOpOfType(const UIKRetargeterController& Controller, const UScriptStruct* Type)
	{
		for (int32 Index = 0; Index < Controller.GetNumRetargetOps(); ++Index)
		{
			const FInstancedStruct* Op = Controller.GetRetargetOpStructAtIndex(Index);
			if (Op && Op->GetScriptStruct() && Op->GetScriptStruct()->IsChildOf(Type))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	FHyperAIRetargetAssetInfo DescribeRig(const UIKRigDefinition& Rig)
	{
		FHyperAIRetargetAssetInfo Info;
		Info.Path = Rig.GetPathName();
		Info.Kind = TEXT("ik_rig");
		UIKRigController* Controller = UIKRigController::GetController(&Rig);
		const USkeletalMesh* Mesh = Controller ? Controller->GetSkeletalMesh() : nullptr;
		Info.Mesh = NameOf(Mesh);
		Info.Skeleton = Mesh && Mesh->GetSkeleton() ? Mesh->GetSkeleton()->GetPathName() : FString();
		Info.BoneCount = Mesh ? Mesh->GetRefSkeleton().GetNum() : 0;
		if (Controller)
		{
			Info.Template = DescribeTemplate(Characterize(Rig));
			Info.RetargetRoot = Controller->GetRetargetRoot().IsNone() ? FString() : Controller->GetRetargetRoot().ToString();
			Info.Chains = ChainLines(*Controller);
			for (const UIKRigEffectorGoal* Goal : Controller->GetAllGoals())
			{
				if (Goal)
				{
					Info.Goals.Add(FString::Printf(TEXT("%s on %s"), *Goal->GoalName.ToString(), *Goal->BoneName.ToString()));
				}
			}
			Info.SolverCount = Controller->GetNumSolvers();
		}
		return Info;
	}

	FHyperAIRetargetAssetInfo DescribeRetargeter(const UIKRetargeter& Retargeter)
	{
		FHyperAIRetargetAssetInfo Info;
		Info.Path = Retargeter.GetPathName();
		Info.Kind = TEXT("retargeter");
		if (const UIKRetargeterController* Controller = UIKRetargeterController::GetController(&Retargeter))
		{
			Info.SourceRig = NameOf(Controller->GetIKRig(ERetargetSourceOrTarget::Source));
			Info.TargetRig = NameOf(Controller->GetIKRig(ERetargetSourceOrTarget::Target));
			Info.SourceMesh = NameOf(Controller->GetPreviewMesh(ERetargetSourceOrTarget::Source));
			Info.TargetMesh = NameOf(Controller->GetPreviewMesh(ERetargetSourceOrTarget::Target));
			for (int32 Index = 0; Index < Controller->GetNumRetargetOps(); ++Index)
			{
				Info.Ops.Add(FString::Printf(TEXT("%s (%s)%s"), *Controller->GetOpName(Index).ToString(), *OpType(*Controller, Index),
					Controller->GetRetargetOpEnabled(Index) ? TEXT("") : TEXT(", off")));
			}
			Info.ChainMap = ChainMapLines(*Controller);
		}
		return Info;
	}

	FString Summarize(const FHyperAIRetargetAssetInfo& Info)
	{
		if (Info.Kind == TEXT("ik_rig"))
		{
			return FString::Printf(TEXT("ik_rig %s: mesh %s; root %s; chains %s; %d goal(s); %d solver(s)"), *Info.Path, *Info.Mesh,
				Info.RetargetRoot.IsEmpty() ? TEXT("none") : *Info.RetargetRoot, Info.Chains.IsEmpty() ? TEXT("none") : *FString::Join(Info.Chains, TEXT(", ")),
				Info.Goals.Num(), Info.SolverCount);
		}
		return FString::Printf(TEXT("retargeter %s: %s (%s) to %s (%s); ops %s; chains %s"), *Info.Path, *Info.SourceRig, *Info.SourceMesh,
			*Info.TargetRig, *Info.TargetMesh, Info.Ops.IsEmpty() ? TEXT("none") : *FString::Join(Info.Ops, TEXT(", ")),
			Info.ChainMap.IsEmpty() ? TEXT("none") : *FString::Join(Info.ChainMap, TEXT(", ")));
	}

	/** Epic's auto-retarget pose: the target's retarget pose reset, every bone aligned to the source, then the template's feet put back flat. */
	void AlignTargetPose(const UIKRetargeterController& Controller, const FAutoCharacterizeResults& Target)
	{
		Controller.ResetRetargetPose(Controller.GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target), TArray<FName>(), ERetargetSourceOrTarget::Target);
		Controller.AutoAlignAllBones(ERetargetSourceOrTarget::Target);
		const TArray<FName>& KeepFlat = Target.AutoRetargetDefinition.BonesToExcludeFromAutoPose;
		if (Target.bUsedTemplate && !KeepFlat.IsEmpty())
		{
			Controller.ResetRetargetPose(Controller.GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target), KeepFlat, ERetargetSourceOrTarget::Target);
		}
	}

	/**
	 * A new retargeter set up the way Epic's auto-retarget sets one up (SRetargetAnimAssetsWindow): default ops, chains mapped by
	 * exact name, the target pose aligned, the IK pass off, root motion from the pelvis when the source has no root bone, and the
	 * template's bones pinned.
	 */
	void SetUpRetargeter(const UIKRetargeterController& Controller, const UIKRigDefinition& Source, const UIKRigDefinition& Target, const FString& Name,
		TArray<FString>& OutNotes)
	{
		const FAutoCharacterizeResults TargetTraits = Characterize(Target);
		FScopedReinitializeIKRetargeter Reinitialize(&Controller, ERetargetRefreshMode::ProcessorAndOpStack);
		Controller.RemoveAllOps();
		Controller.AddDefaultOps();
		// Exact names only, as Epic's auto-retarget maps them: fuzzy matching can map toes to fingers.
		Controller.AutoMapChains(EAutoMapChainType::Exact, /*bForceRemap=*/true);
		AlignTargetPose(Controller, TargetTraits);

		const int32 RunIK = FindOpOfType(Controller, FIKRetargetRunIKRigOp::StaticStruct());
		if (RunIK != INDEX_NONE)
		{
			Controller.SetRetargetOpEnabled(RunIK, false);
		}

		const TArray<FName>& SourceBones = Source.GetSkeleton().BoneNames;
		const TArray<FName>& TargetBones = Target.GetSkeleton().BoneNames;
		const UIKRigController* SourceRig = UIKRigController::GetController(&Source);
		const UIKRigController* TargetRig = UIKRigController::GetController(&Target);
		const int32 RootIndex = FindOpOfType(Controller, FIKRetargetRootMotionOp::StaticStruct());
		FIKRetargetRootMotionOp* RootOp = Controller.GetFirstRetargetOpOfType<FIKRetargetRootMotionOp>();
		if (RootOp && SourceRig && TargetRig && !SourceBones.IsEmpty() && !TargetBones.IsEmpty())
		{
			if (TargetBones[0] == TargetRig->GetRetargetRoot())
			{
				// The pelvis is the root, so the pelvis op carries the root motion.
				Controller.SetRetargetOpEnabled(RootIndex, false);
			}
			else if (SourceBones[0] == SourceRig->GetRetargetRoot())
			{
				RootOp->Settings.RootMotionSource = ERootMotionSource::GenerateFromTargetPelvis;
				RootOp->Settings.RootHeightSource = ERootMotionHeightSource::SnapToGround;
				RootOp->Settings.bRotateWithPelvis = true;
				RootOp->Settings.bMaintainOffsetFromPelvis = true;
				OutNotes.Add(FString::Printf(TEXT("%s: the source has no root bone of its own, so root motion is made from the target's pelvis."), *Name));
			}
		}

		const TArray<FBoneToPin>& Pins = TargetTraits.AutoRetargetDefinition.BonesToPin.GetBonesToPin();
		if (TargetTraits.bUsedTemplate && !Pins.IsEmpty())
		{
			Controller.AddRetargetOp(FIKRetargetPinBoneOp::StaticStruct());
			if (FIKRetargetPinBoneOp* PinOp = Controller.GetFirstRetargetOpOfType<FIKRetargetPinBoneOp>())
			{
				PinOp->Settings.TranslationMode = EPinBoneTranslationMode::CopyGlobalPosition;
				PinOp->Settings.RotationMode = EPinBoneRotationMode::CopyGlobalRotation;
				for (const FBoneToPin& Pin : Pins)
				{
					FPinBoneData Data;
					Data.BoneToCopyFrom.BoneName = Pin.BoneToPinTo;
					Data.BoneToCopyTo.BoneName = Pin.BoneToPin;
					PinOp->Settings.BonesToPin.Add(Data);
				}
			}
		}

		TArray<FString> Unmapped;
		ChainMapLines(Controller, &Unmapped);
		OutNotes.Add(FString::Printf(TEXT("%s: the IK pass is off, as Epic's auto-retarget leaves it; turn it on with retargeter.enable_op (FIKRetargetRunIKRigOp) once the rigs' IK is tuned.%s"),
			*Name, Unmapped.IsEmpty() ? TEXT("")
				: *FString::Printf(TEXT(" Target chains with no source chain of the same name stay unmapped: %s; map them with retargeter.map_chain, or retargeter.auto_map with Mode fuzzy."),
					*FString::Join(Unmapped, TEXT(", ")))));
	}

	/** What a plan knows of each rig and retargeter it names, as it is on disk or as the plan's earlier ops leave it. */
	struct FPlannedAsset
	{
		bool bRetargeter = false;
		/** A rig's mesh, for bone checks. */
		const USkeletalMesh* Mesh = nullptr;
		/** A retargeter's preview meshes and rigs (as keys). */
		const USkeletalMesh* SourceMesh = nullptr;
		const USkeletalMesh* TargetMesh = nullptr;
		FString SourceRig;
		FString TargetRig;
		/** A rig's chains, lower case; unknown once automatic naming will make them. */
		TSet<FString> Chains;
		bool bChainsKnown = true;
		/** A retargeter's op names and types, lower case; unknown for one the plan makes. */
		TSet<FString> Ops;
		bool bOpsKnown = true;
	};

	class FPlanCheck
	{
	public:
		/** The asset as the plan has it so far, read from disk the first time; null, with why, when it is not the kind the op needs. */
		FPlannedAsset* Find(const FString& Path, const bool bRetargeter, FString& OutWhy)
		{
			return Track(Path, bRetargeter, OutWhy) ? Assets.Find(KeyOf(Path)) : nullptr;
		}

		FPlannedAsset* FindKey(const FString& Key)
		{
			return Assets.Find(Key);
		}

		bool Contains(const FString& Path) const
		{
			return Assets.Contains(KeyOf(Path));
		}

		void Add(const FString& Path, FPlannedAsset&& Asset)
		{
			Assets.Add(KeyOf(Path), MoveTemp(Asset));
		}

	private:
		bool Track(const FString& Path, const bool bRetargeter, FString& OutWhy)
		{
			const TCHAR* Wanted = bRetargeter ? TEXT("an IK Retargeter") : TEXT("an IK Rig");
			if (const FPlannedAsset* Planned = Assets.Find(KeyOf(Path)))
			{
				if (Planned->bRetargeter != bRetargeter)
				{
					OutWhy = FString::Printf(TEXT("%s is not %s."), *Path, Wanted);
					return false;
				}
				return true;
			}
			FPlannedAsset Asset;
			Asset.bRetargeter = bRetargeter;
			if (bRetargeter)
			{
				const UIKRetargeter* Retargeter = Load<UIKRetargeter>(Path);
				const UIKRetargeterController* Controller = Retargeter ? UIKRetargeterController::GetController(Retargeter) : nullptr;
				if (!Controller)
				{
					OutWhy = FString::Printf(TEXT("'%s' is not %s, and the plan does not make it."), *Path, Wanted);
					return false;
				}
				Asset.SourceMesh = Controller->GetPreviewMesh(ERetargetSourceOrTarget::Source);
				Asset.TargetMesh = Controller->GetPreviewMesh(ERetargetSourceOrTarget::Target);
				for (int32 Index = 0; Index < Controller->GetNumRetargetOps(); ++Index)
				{
					const FString Type = OpType(*Controller, Index).ToLower();
					Asset.Ops.Add(Controller->GetOpName(Index).ToString().ToLower());
					Asset.Ops.Add(Type);
					Asset.Ops.Add(Type.StartsWith(TEXT("f")) ? Type.Mid(1) : Type);
				}
				for (const ERetargetSourceOrTarget Side : {ERetargetSourceOrTarget::Source, ERetargetSourceOrTarget::Target})
				{
					if (const UIKRigDefinition* Rig = Controller->GetIKRig(Side))
					{
						(Side == ERetargetSourceOrTarget::Source ? Asset.SourceRig : Asset.TargetRig) = KeyOf(Rig->GetPathName());
						if (!Track(Rig->GetPathName(), false, OutWhy))
						{
							return false;
						}
					}
				}
			}
			else
			{
				const UIKRigDefinition* Rig = Load<UIKRigDefinition>(Path);
				const UIKRigController* Controller = Rig ? UIKRigController::GetController(Rig) : nullptr;
				if (!Controller)
				{
					OutWhy = FString::Printf(TEXT("'%s' is not %s, and the plan does not make it."), *Path, Wanted);
					return false;
				}
				Asset.Mesh = Controller->GetSkeletalMesh();
				for (const FBoneChain& Chain : Controller->GetRetargetChains())
				{
					Asset.Chains.Add(Chain.ChainName.ToString().ToLower());
				}
			}
			Assets.Add(KeyOf(Path), MoveTemp(Asset));
			return true;
		}

		TMap<FString, FPlannedAsset> Assets;
	};

	/** Every op checked before anything changes: its kind, its assets, bones, chains, ops and animations. */
	bool Check(const FHyperAIRetargetPlanRequest& Request, FString& OutError)
	{
		if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
			return false;
		}
		FPlanCheck Plan;
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAIRetargetOp& Op = Request.Operations[Index];
			const FString Kind = KindOf(Op);
			FString Why;
			auto Fail = [&OutError, &Op, Index](const FString& Message)
			{
				OutError = FString::Printf(TEXT("Operation %d (%s): %s Nothing was changed."), Index, *Op.Kind, *Message);
				return false;
			};
			if (Kind == TEXT("ik_rig.create") || Kind == TEXT("retargeter.create"))
			{
				if (!IsNewAsset(Op.Asset, Why))
				{
					return Fail(Why);
				}
				if (Plan.Contains(Op.Asset))
				{
					return Fail(FString::Printf(TEXT("The plan makes %s twice."), *Op.Asset));
				}
			}
			if (Kind == TEXT("ik_rig.create"))
			{
				FPlannedAsset Rig;
				Rig.Mesh = Load<USkeletalMesh>(Op.Mesh);
				Rig.bChainsKnown = !Op.bAutoChains;
				if (!Rig.Mesh)
				{
					return Fail(FString::Printf(TEXT("'%s' is not a skeletal mesh."), *Op.Mesh));
				}
				Plan.Add(Op.Asset, MoveTemp(Rig));
				continue;
			}
			if (Kind == TEXT("retargeter.create"))
			{
				if (!Plan.Find(Op.SourceRig, false, Why) || !Plan.Find(Op.TargetRig, false, Why))
				{
					return Fail(Why);
				}
				// Both rigs are tracked now, so these lookups add nothing and the pointers hold.
				const FPlannedAsset* Source = Plan.Find(Op.SourceRig, false, Why);
				const FPlannedAsset* Target = Plan.Find(Op.TargetRig, false, Why);
				FPlannedAsset Retargeter;
				Retargeter.bRetargeter = true;
				Retargeter.SourceMesh = Op.SourceMesh.TrimStartAndEnd().IsEmpty() ? Source->Mesh : Load<USkeletalMesh>(Op.SourceMesh);
				Retargeter.TargetMesh = Op.TargetMesh.TrimStartAndEnd().IsEmpty() ? Target->Mesh : Load<USkeletalMesh>(Op.TargetMesh);
				Retargeter.SourceRig = KeyOf(Op.SourceRig);
				Retargeter.TargetRig = KeyOf(Op.TargetRig);
				Retargeter.bOpsKnown = false;
				if (!Retargeter.SourceMesh || !Retargeter.TargetMesh)
				{
					return Fail(TEXT("Each rig needs a skeletal mesh, or give SourceMesh and TargetMesh."));
				}
				Plan.Add(Op.Asset, MoveTemp(Retargeter));
				continue;
			}
			if (Kind.StartsWith(TEXT("ik_rig.")))
			{
				FPlannedAsset* Rig = Plan.Find(Op.Asset, false, Why);
				if (!Rig)
				{
					return Fail(Why);
				}
				const FString Chain = Op.Chain.TrimStartAndEnd().ToLower();
				auto MissingBone = [Rig](const FString& Bone)
				{
					return FString::Printf(TEXT("%s has no bone '%s'."), *NameOf(Rig->Mesh), *Bone);
				};
				if (Kind == TEXT("ik_rig.add_chain") || Kind == TEXT("ik_rig.set_chain"))
				{
					const bool bAdding = Kind == TEXT("ik_rig.add_chain");
					if (Chain.IsEmpty())
					{
						return Fail(TEXT("Name the Chain."));
					}
					if ((bAdding || !Op.StartBone.TrimStartAndEnd().IsEmpty()) && !HasBone(Rig->Mesh, Op.StartBone))
					{
						return Fail(MissingBone(Op.StartBone));
					}
					if ((bAdding || !Op.EndBone.TrimStartAndEnd().IsEmpty()) && !HasBone(Rig->Mesh, Op.EndBone))
					{
						return Fail(MissingBone(Op.EndBone));
					}
					if (!bAdding && Op.StartBone.TrimStartAndEnd().IsEmpty() && Op.EndBone.TrimStartAndEnd().IsEmpty() && Op.Goal.TrimStartAndEnd().IsEmpty())
					{
						return Fail(TEXT("Give StartBone, EndBone or Goal to change."));
					}
					if (Rig->bChainsKnown && bAdding == Rig->Chains.Contains(Chain))
					{
						return Fail(bAdding ? FString::Printf(TEXT("The rig has a chain %s already."), *Op.Chain)
							: FString::Printf(TEXT("The rig has no chain %s."), *Op.Chain));
					}
					Rig->Chains.Add(Chain);
				}
				else if (Kind == TEXT("ik_rig.remove_chain"))
				{
					if (Chain.IsEmpty() || (Rig->bChainsKnown && !Rig->Chains.Contains(Chain)))
					{
						return Fail(FString::Printf(TEXT("The rig has no chain '%s'."), *Op.Chain));
					}
					Rig->Chains.Remove(Chain);
				}
				else if (Kind == TEXT("ik_rig.set_root"))
				{
					if (!HasBone(Rig->Mesh, Op.Bone))
					{
						return Fail(MissingBone(Op.Bone));
					}
				}
				else if (Kind == TEXT("ik_rig.auto_chains") || Kind == TEXT("ik_rig.auto_full_body_ik"))
				{
					if (!Rig->Mesh)
					{
						return Fail(TEXT("The rig has no skeletal mesh."));
					}
					Rig->bChainsKnown = Rig->bChainsKnown && Kind != TEXT("ik_rig.auto_chains");
				}
				else
				{
					return Fail(TEXT("Unknown ik_rig op; see hyper_retarget_apply_plan's description."));
				}
				continue;
			}
			if (Kind.StartsWith(TEXT("retargeter.")))
			{
				FPlannedAsset* Retargeter = Plan.Find(Op.Asset, true, Why);
				if (!Retargeter)
				{
					return Fail(Why);
				}
				if (Kind == TEXT("retargeter.map_chain"))
				{
					const FPlannedAsset* TargetRig = Plan.FindKey(Retargeter->TargetRig);
					const FPlannedAsset* SourceRig = Plan.FindKey(Retargeter->SourceRig);
					const FString TargetChain = Op.TargetChain.TrimStartAndEnd().ToLower();
					const FString SourceChain = Op.SourceChain.TrimStartAndEnd().ToLower();
					if (TargetChain.IsEmpty() || (TargetRig && TargetRig->bChainsKnown && !TargetRig->Chains.Contains(TargetChain)))
					{
						return Fail(FString::Printf(TEXT("The target rig has no chain '%s'."), *Op.TargetChain));
					}
					if (!SourceChain.IsEmpty() && SourceRig && SourceRig->bChainsKnown && !SourceRig->Chains.Contains(SourceChain))
					{
						return Fail(FString::Printf(TEXT("The source rig has no chain '%s'."), *Op.SourceChain));
					}
				}
				else if (Kind == TEXT("retargeter.auto_map"))
				{
					const FString Mode = Op.Mode.TrimStartAndEnd().ToLower();
					if (!Mode.IsEmpty() && Mode != TEXT("fuzzy") && Mode != TEXT("exact") && Mode != TEXT("clear"))
					{
						return Fail(TEXT("Mode is exact, fuzzy or clear."));
					}
				}
				else if (Kind == TEXT("retargeter.enable_op"))
				{
					const FString Wanted = Op.OpName.TrimStartAndEnd().ToLower();
					if (Wanted.IsEmpty() || (Retargeter->bOpsKnown && !Retargeter->Ops.Contains(Wanted)))
					{
						return Fail(FString::Printf(TEXT("The retargeter has no op '%s'; hyper_retarget_inspect lists them."), *Op.OpName));
					}
				}
				else if (Kind != TEXT("retargeter.auto_align"))
				{
					return Fail(TEXT("Unknown retargeter op; see hyper_retarget_apply_plan's description."));
				}
				continue;
			}
			if (Kind == TEXT("retarget.animations"))
			{
				const FPlannedAsset* Retargeter = Plan.Find(Op.Asset, true, Why);
				if (!Retargeter)
				{
					return Fail(Why);
				}
				if (Op.Animations.IsEmpty() || Op.Animations.Num() > C::MaxAnimations)
				{
					return Fail(FString::Printf(TEXT("Give 1 to %d Animations."), C::MaxAnimations));
				}
				for (const FString& Path : Op.Animations)
				{
					if (!Load<UAnimationAsset>(Path))
					{
						return Fail(FString::Printf(TEXT("'%s' is not an animation sequence, blend space, montage, composite or pose asset."), *Path));
					}
				}
				const USkeletalMesh* Source = Op.SourceMesh.TrimStartAndEnd().IsEmpty() ? Retargeter->SourceMesh : Load<USkeletalMesh>(Op.SourceMesh);
				const USkeletalMesh* Target = Op.TargetMesh.TrimStartAndEnd().IsEmpty() ? Retargeter->TargetMesh : Load<USkeletalMesh>(Op.TargetMesh);
				if (!Source || !Target || Source == Target)
				{
					return Fail(TEXT("Retargeting needs two different skeletal meshes: SourceMesh and TargetMesh, or the retargeter's preview meshes."));
				}
				if (!Op.OutputFolder.TrimStartAndEnd().IsEmpty() && !IsProjectPath(Op.OutputFolder, Why))
				{
					return Fail(Why);
				}
				if (Op.Prefix.Len() + Op.Suffix.Len() + Op.Replace.Len() > 128)
				{
					return Fail(TEXT("Prefix, Suffix and Replace together are at most 128 characters."));
				}
				// Replace goes in once for each match, so only the names themselves say whether the packages' FNames fit.
				if (!CopiesFitNames(Op, Why))
				{
					return Fail(Why);
				}
				continue;
			}
			return Fail(TEXT("Unknown op; see hyper_retarget_apply_plan's description."));
		}
		return true;
	}

	/** One rig or retargeter op, inside the plan's undo step. */
	bool ApplyEdit(const FHyperAIRetargetOp& Op, TMap<FString, UObject*>& Made, TArray<UObject*>& Touched, TArray<FString>& OutNotes, FString& OutError)
	{
		const FString Kind = KindOf(Op);
		auto Resolve = [&Made](const FString& Path) -> UObject*
		{
			UObject* const* Planned = Made.Find(KeyOf(Path));
			return Planned ? *Planned : Load<UObject>(Path);
		};
		if (Kind == TEXT("ik_rig.create") || Kind == TEXT("retargeter.create"))
		{
			const FString Package = PackageOf(Op.Asset);
			UPackage* Outer = CreatePackage(*Package);
			const FName Name(*FPackageName::GetShortName(Package));
			UObject* Asset = Kind == TEXT("ik_rig.create")
				? static_cast<UObject*>(NewObject<UIKRigDefinition>(Outer, Name, RF_Public | RF_Standalone | RF_Transactional))
				: static_cast<UObject*>(NewObject<UIKRetargeter>(Outer, Name, RF_Public | RF_Standalone | RF_Transactional));
			FAssetRegistryModule::AssetCreated(Asset);
			Outer->MarkPackageDirty();
			Made.Add(KeyOf(Package), Asset);
			Touched.AddUnique(Asset);
			if (UIKRigDefinition* Rig = Cast<UIKRigDefinition>(Asset))
			{
				UIKRigController* Controller = UIKRigController::GetController(Rig);
				USkeletalMesh* Mesh = Load<USkeletalMesh>(Op.Mesh);
				if (!Controller || !Mesh || !Controller->SetSkeletalMesh(Mesh))
				{
					OutError = FString::Printf(TEXT("%s would not take the mesh %s."), *Rig->GetName(), *Op.Mesh);
					return false;
				}
				if (Op.bAutoChains && !AutoChains(*Controller, Rig->GetName(), OutNotes))
				{
					OutNotes.Add(FString::Printf(TEXT("%s: no chains could be found; add them with ik_rig.add_chain and set the root with ik_rig.set_root."), *Rig->GetName()));
				}
				if (Op.bAutoFullBodyIK && !Controller->ApplyAutoFBIK())
				{
					OutNotes.Add(FString::Printf(TEXT("%s: full body IK was set up with problems; check its goals with hyper_retarget_inspect."), *Rig->GetName()));
				}
				return true;
			}
			UIKRetargeter* Retargeter = CastChecked<UIKRetargeter>(Asset);
			const UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
			UIKRigDefinition* Source = Cast<UIKRigDefinition>(Resolve(Op.SourceRig));
			UIKRigDefinition* Target = Cast<UIKRigDefinition>(Resolve(Op.TargetRig));
			if (!Controller || !Source || !Target)
			{
				OutError = TEXT("Its IK Rigs could not be loaded.");
				return false;
			}
			Controller->SetIKRig(ERetargetSourceOrTarget::Source, Source);
			Controller->SetIKRig(ERetargetSourceOrTarget::Target, Target);
			if (USkeletalMesh* SourceMesh = Load<USkeletalMesh>(Op.SourceMesh))
			{
				Controller->SetPreviewMesh(ERetargetSourceOrTarget::Source, SourceMesh);
			}
			if (USkeletalMesh* TargetMesh = Load<USkeletalMesh>(Op.TargetMesh))
			{
				Controller->SetPreviewMesh(ERetargetSourceOrTarget::Target, TargetMesh);
			}
			SetUpRetargeter(*Controller, *Source, *Target, Retargeter->GetName(), OutNotes);
			return true;
		}
		if (Kind.StartsWith(TEXT("ik_rig.")))
		{
			UIKRigDefinition* Rig = Cast<UIKRigDefinition>(Resolve(Op.Asset));
			UIKRigController* Controller = Rig ? UIKRigController::GetController(Rig) : nullptr;
			if (!Controller)
			{
				OutError = FString::Printf(TEXT("%s is not an IK Rig."), *Op.Asset);
				return false;
			}
			Touched.AddUnique(Rig);
			const FName Chain = NameOrNone(Op.Chain);
			bool bDone = true;
			if (Kind == TEXT("ik_rig.add_chain"))
			{
				bDone = Controller->AddRetargetChain(Chain, NameOrNone(Op.StartBone), NameOrNone(Op.EndBone), NameOrNone(Op.Goal)) == Chain;
			}
			else if (Kind == TEXT("ik_rig.set_chain"))
			{
				bDone = (Op.StartBone.TrimStartAndEnd().IsEmpty() || Controller->SetRetargetChainStartBone(Chain, NameOrNone(Op.StartBone)))
					&& (Op.EndBone.TrimStartAndEnd().IsEmpty() || Controller->SetRetargetChainEndBone(Chain, NameOrNone(Op.EndBone)))
					&& (Op.Goal.TrimStartAndEnd().IsEmpty() || Controller->SetRetargetChainGoal(Chain, NameOrNone(Op.Goal)));
			}
			else if (Kind == TEXT("ik_rig.remove_chain"))
			{
				bDone = Controller->RemoveRetargetChain(Chain);
			}
			else if (Kind == TEXT("ik_rig.set_root"))
			{
				bDone = Controller->SetRetargetRoot(NameOrNone(Op.Bone));
			}
			else if (Kind == TEXT("ik_rig.auto_chains"))
			{
				if (!AutoChains(*Controller, Rig->GetName(), OutNotes))
				{
					OutError = TEXT("No chains could be found for this skeleton; add them with ik_rig.add_chain.");
					return false;
				}
			}
			else
			{
				bDone = Controller->ApplyAutoFBIK();
			}
			if (!bDone)
			{
				OutError = TEXT("The IK Rig refused it; check the chain, bone and goal names with hyper_retarget_inspect.");
				return false;
			}
			return true;
		}
		UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Resolve(Op.Asset));
		const UIKRetargeterController* Controller = Retargeter ? UIKRetargeterController::GetController(Retargeter) : nullptr;
		if (!Controller)
		{
			OutError = FString::Printf(TEXT("%s is not an IK Retargeter."), *Op.Asset);
			return false;
		}
		Touched.AddUnique(Retargeter);
		if (Kind == TEXT("retargeter.map_chain"))
		{
			if (!Controller->SetSourceChain(NameOrNone(Op.SourceChain), NameOrNone(Op.TargetChain)))
			{
				OutError = FString::Printf(TEXT("No op maps a target chain %s."), *Op.TargetChain);
				return false;
			}
		}
		else if (Kind == TEXT("retargeter.auto_map"))
		{
			const FString Mode = Op.Mode.TrimStartAndEnd().ToLower();
			Controller->AutoMapChains(Mode == TEXT("fuzzy") ? EAutoMapChainType::Fuzzy : Mode == TEXT("clear") ? EAutoMapChainType::Clear
				: EAutoMapChainType::Exact, /*bForceRemap=*/true);
		}
		else if (Kind == TEXT("retargeter.enable_op"))
		{
			const int32 OpIndex = FindOp(*Controller, Op.OpName);
			if (OpIndex == INDEX_NONE || !Controller->SetRetargetOpEnabled(OpIndex, Op.bEnabled))
			{
				OutError = FString::Printf(TEXT("The retargeter has no op '%s'."), *Op.OpName);
				return false;
			}
		}
		else
		{
			const UIKRigDefinition* Target = Controller->GetIKRig(ERetargetSourceOrTarget::Target);
			AlignTargetPose(*Controller, Target ? Characterize(*Target) : FAutoCharacterizeResults());
		}
		return true;
	}

	/** The plan for real: the rigs and retargeters as one undo step, then the batch retargets, then everything saved. */
	FHyperAIRetargetPlanReport Execute(const FHyperAIRetargetPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIRetargetPlanReport Report;
		Report.OperationId = OperationId;
		TMap<FString, UObject*> Made;
		TArray<UObject*> Touched;
		auto Fail = [&Report](const int32 Index, const FHyperAIRetargetOp& Op, const FString& Error)
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = FString::Printf(TEXT("Operation %d (%s): %s%s"), Index, *Op.Kind, *Error,
				Report.Applied > 0 ? TEXT(" The operations before it stay applied, unsaved; Edit > Undo takes the rig and retargeter changes back.") : TEXT(""));
			return Report;
		};
		{
			const FScopedTransaction Transaction(LOCTEXT("RetargetPlan", "HyperAI: Set Up Retargeting"));
			for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
			{
				const FHyperAIRetargetOp& Op = Request.Operations[Index];
				if (KindOf(Op) == TEXT("retarget.animations"))
				{
					continue;
				}
				FString Error;
				if (!ApplyEdit(Op, Made, Touched, Report.Notes, Error))
				{
					return Fail(Index, Op, Error);
				}
				++Report.Applied;
			}
		}
		const int32 Edited = Touched.Num();
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAIRetargetOp& Op = Request.Operations[Index];
			if (KindOf(Op) != TEXT("retarget.animations"))
			{
				continue;
			}
			UObject* const* Planned = Made.Find(KeyOf(Op.Asset));
			UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Planned ? *Planned : Load<UObject>(Op.Asset));
			const UIKRetargeterController* Controller = Retargeter ? UIKRetargeterController::GetController(Retargeter) : nullptr;
			if (!Controller)
			{
				return Fail(Index, Op, FString::Printf(TEXT("%s is not an IK Retargeter."), *Op.Asset));
			}
			// The batch reads unsaved rigs and retargeters from memory, so a plan can make them and retarget with them.
			FIKRetargetBatchOperationInputs Inputs;
			for (const FString& Path : Op.Animations)
			{
				if (UAnimationAsset* Animation = Load<UAnimationAsset>(Path))
				{
					Inputs.AssetsToRetarget.Add(FAssetData(Animation));
				}
			}
			Inputs.SourceMesh = Op.SourceMesh.TrimStartAndEnd().IsEmpty() ? Controller->GetPreviewMesh(ERetargetSourceOrTarget::Source) : Load<USkeletalMesh>(Op.SourceMesh);
			Inputs.TargetMesh = Op.TargetMesh.TrimStartAndEnd().IsEmpty() ? Controller->GetPreviewMesh(ERetargetSourceOrTarget::Target) : Load<USkeletalMesh>(Op.TargetMesh);
			Inputs.IKRetargetAsset = Retargeter;
			Inputs.Prefix = Op.Prefix;
			Inputs.Suffix = Op.Suffix;
			Inputs.Search = Op.Search;
			Inputs.Replace = Op.Replace;
			Inputs.TargetPath = Op.OutputFolder.TrimStartAndEnd();
			Inputs.bUseSourcePath = Inputs.TargetPath.IsEmpty();
			Inputs.bIncludeReferencedAssets = Op.bIncludeReferenced;
			Inputs.bOverwriteExistingFiles = Op.bOverwrite;
			if (!Inputs.SourceMesh || !Inputs.TargetMesh)
			{
				return Fail(Index, Op, TEXT("The retargeter has no preview mesh for the source or target; give SourceMesh and TargetMesh."));
			}
			const TArray<FAssetData> Retargeted = UIKRetargetBatchOperation::RunBatchRetarget(Inputs);
			if (Retargeted.IsEmpty())
			{
				return Fail(Index, Op, TEXT("Nothing was retargeted (the batch was cancelled or found nothing to copy); check the retargeter's ChainMap with hyper_retarget_inspect."));
			}
			for (const FAssetData& Asset : Retargeted)
			{
				Report.Retargeted.Add(Asset.GetObjectPathString());
				if (UObject* Loaded = Asset.GetAsset())
				{
					Touched.AddUnique(Loaded);
				}
			}
			++Report.Applied;
		}
		for (int32 Index = 0; Index < Touched.Num(); ++Index)
		{
			UObject* Asset = Touched[Index];
			if (Index < Edited)
			{
				if (const UIKRigDefinition* Rig = Cast<UIKRigDefinition>(Asset))
				{
					Report.Made.Add(Summarize(DescribeRig(*Rig)));
				}
				else if (const UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Asset))
				{
					Report.Made.Add(Summarize(DescribeRetargeter(*Retargeter)));
				}
			}
			FString Error;
			if (!FHyperAIStudioSourceControl::SaveAsset(*Asset, Error))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = Error;
				return Report;
			}
			Report.Saved.Add(Asset->GetPathName());
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) applied and saved%s."), Report.Applied,
			Report.Retargeted.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; %d animation(s) retargeted"), Report.Retargeted.Num()));
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAIRetargetPlanReport& Done)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Done.bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = C::PlanToolName;
		Entry.Target = Done.Saved.IsEmpty() ? FString() : Done.Saved[0];
		Entry.OperationId = OperationId;
		Entry.StatusCode = Done.bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Done.Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}
}

FHyperAIRetargetInspectReport FHyperAIStudioRetargetContracts::Inspect(const FHyperAIRetargetInspectRequest& Request)
{
	using namespace HyperAIStudio::Retarget::Private;
	FHyperAIRetargetInspectReport Report;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	if (Request.Paths.IsEmpty() || Request.Paths.Num() > 64)
	{
		Report.Diagnostic = TEXT("Give 1 to 64 Paths: IK Rigs, IK Retargeters or skeletal meshes.");
		return Report;
	}
	for (const FString& Path : Request.Paths)
	{
		const UObject* Asset = Load<UObject>(Path);
		if (const UIKRigDefinition* Rig = Cast<UIKRigDefinition>(Asset))
		{
			Report.Assets.Add(DescribeRig(*Rig));
		}
		else if (const UIKRetargeter* Retargeter = Cast<UIKRetargeter>(Asset))
		{
			Report.Assets.Add(DescribeRetargeter(*Retargeter));
		}
		else if (const USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset))
		{
			FHyperAIRetargetAssetInfo& Info = Report.Assets.AddDefaulted_GetRef();
			Info.Path = Mesh->GetPathName();
			Info.Kind = TEXT("skeletal_mesh");
			Info.Mesh = Info.Path;
			Info.Skeleton = Mesh->GetSkeleton() ? Mesh->GetSkeleton()->GetPathName() : FString();
			Info.BoneCount = Mesh->GetRefSkeleton().GetNum();
		}
		else
		{
			Report.Diagnostic = FString::Printf(TEXT("'%s' is not an IK Rig, IK Retargeter or skeletal mesh."), *Path);
			return Report;
		}
	}
	Report.bOk = true;
	return Report;
}

FHyperAIRetargetPlanReport FHyperAIStudioRetargetContracts::ApplyPlan(const FHyperAIRetargetPlanRequest& Request)
{
	using namespace HyperAIStudio::Retarget::Private;
	FHyperAIRetargetPlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("retarget-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	FString Error;
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
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) checked; nothing was changed."), Request.Operations.Num());
		return Report;
	}
	if (FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = PlanToolName;
		Summary.VariantId = TEXT("retarget.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Request.Operations[0].Asset;
		Summary.Effects.Add(FString::Printf(TEXT("Set up retargeting: %d operation(s); IK Rigs and retargeters as one undo step, retargeted animations as new assets; all saved"),
			Request.Operations.Num()));
		for (int32 Index = 0; Index < Request.Operations.Num() && Index < 24; ++Index)
		{
			const FHyperAIRetargetOp& Op = Request.Operations[Index];
			Summary.Effects.Add(FString::Printf(TEXT("| %s %s%s%s"), *Op.Kind, *Op.Asset,
				Op.Animations.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%d animations)"), Op.Animations.Num()),
				Op.bOverwrite && KindOf(Op) == TEXT("retarget.animations") ? TEXT(", replacing assets that already have the new names") : TEXT("")));
			Summary.Touches.AddUnique(Op.Asset);
		}
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				// The editor may have moved on while the plan waited (an asset deleted, a path taken, the level closed),
				// so it is checked again before anything changes.
				FString Problem;
				if (!Check(Request, Problem))
				{
					FHyperAIRetargetPlanReport Refused;
					Refused.OperationId = OperationId;
					Refused.Status = TEXT("invalid_request");
					Refused.Diagnostic = TEXT("The editor changed while the plan waited for approval: ") + Problem;
					Record(OperationId, Refused);
					return;
				}
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

FHyperAIRetargetInspectReport UHyperAIStudioRetargetToolset::hyper_retarget_inspect(const FHyperAIRetargetInspectRequest& Request)
{
	return FHyperAIStudioRetargetContracts::Inspect(Request);
}

FHyperAIRetargetPlanReport UHyperAIStudioRetargetToolset::hyper_retarget_apply_plan(const FHyperAIRetargetPlanRequest& Request)
{
	return FHyperAIStudioRetargetContracts::ApplyPlan(Request);
}

void FHyperAIStudioRetargetRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioRetargetRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioRetargetRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioRetargetToolset::StaticClass(),
			FHyperAIStudioRetargetContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioRetarget, Warning, TEXT("Could not unregister the retargeting toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioRetargetRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioRetargetContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioRetarget, Verbose, TEXT("The retargeting source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioRetargetToolset::StaticClass(),
		FHyperAIStudioRetargetContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioRetarget, Error, TEXT("Retargeting cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
