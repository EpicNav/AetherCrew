// Games by Hyper 2026.

#include "HyperAIStudioAnimationBlendSpace.h"
#include "HyperAIStudioAnimationTimeline.h"

#include "Animation/AnimSequence.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Factories/BlendSpaceFactory1D.h"
#include "Factories/BlendSpaceFactoryNew.h"
#include "FileHelpers.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioNames.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioAnimationBlendSpace"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAnimationBlendSpace, Log, All);

namespace HyperAIStudio::Animation::BlendSpace
{
	namespace
	{
		using EKind = EHyperAIStudioAnimationOperationKind;

		UBlendSpace* FindLoaded(const FString& Path)
		{
			UBlendSpace* Space = FindObject<UBlendSpace>(nullptr, *Path);
			return IsValid(Space) && Space->GetPathName() == Path ? Space : nullptr;
		}

		template <typename T>
		T* LoadExact(const FString& Path)
		{
			T* Object = Cast<T>(FSoftObjectPath(Path).TryLoad());
			return IsValid(Object) && Object->GetPathName() == Path ? Object : nullptr;
		}

		void AppendToken(FString& Out, const FString& Value)
		{
			Out += FString::Printf(TEXT("%d:%s|"), Value.Len(), *Value);
		}

		UBlendSpace* Create(const FHyperAIStudioAnimationBackendOperation& Operation, FString& OutError)
		{
			USkeleton* Skeleton = LoadExact<USkeleton>(Operation.ReferencePath);
			if (!Skeleton)
			{
				OutError = FString::Printf(TEXT("%s no longer resolves to a Skeleton."), *Operation.ReferencePath);
				return nullptr;
			}
			const FString PackageName = FPackageName::ObjectPathToPackageName(Operation.TargetPath);
			// Free in memory too: an unsaved asset there of another class would stop the editor when the blend space is made.
			if (FindLoaded(Operation.TargetPath) || !FHyperAIStudioNames::IsFreePackage(PackageName))
			{
				OutError = FString::Printf(TEXT("%s appeared after planning."), *Operation.TargetPath);
				return nullptr;
			}
			UPackage* Package = CreatePackage(*PackageName);
			const FName Name(*FPackageName::ObjectPathToObjectName(Operation.TargetPath));
			const EObjectFlags Flags = RF_Public | RF_Standalone | RF_Transactional;
			UObject* Created = nullptr;
			if (Operation.bFlag)
			{
				UBlendSpaceFactory1D* Factory = NewObject<UBlendSpaceFactory1D>();
				Factory->TargetSkeleton = Skeleton;
				Created = Factory->FactoryCreateNew(UBlendSpace1D::StaticClass(), Package, Name, Flags, nullptr, GWarn);
			}
			else
			{
				UBlendSpaceFactoryNew* Factory = NewObject<UBlendSpaceFactoryNew>();
				Factory->TargetSkeleton = Skeleton;
				Created = Factory->FactoryCreateNew(UBlendSpace::StaticClass(), Package, Name, Flags, nullptr, GWarn);
			}
			UBlendSpace* Space = Cast<UBlendSpace>(Created);
			if (!Space)
			{
				OutError = TEXT("The blend space factory created nothing.");
				return nullptr;
			}
			FAssetRegistryModule::AssetCreated(Space);
			Package->MarkPackageDirty();
			return Space;
		}

		/** BlendParameters is protected; this writes it the way the Details panel does, by reflection with an edit event. */
		bool SetAxis(UBlendSpace& Space, const FHyperAIStudioAnimationBackendOperation& Operation, FString& OutError)
		{
			const int32 Axes = Space.IsA<UBlendSpace1D>() ? 1 : 2;
			if (Operation.AxisIndex >= Axes)
			{
				OutError = FString::Printf(TEXT("%s has %d axis%s, so axis_index %d does not exist."), *Space.GetName(), Axes,
					Axes == 1 ? TEXT("") : TEXT("es"), Operation.AxisIndex);
				return false;
			}
			FStructProperty* Property = FindFProperty<FStructProperty>(UBlendSpace::StaticClass(), TEXT("BlendParameters"));
			if (!Property || Property->Struct != FBlendParameter::StaticStruct() || Property->ArrayDim < Axes)
			{
				OutError = TEXT("This engine's blend space has no BlendParameters axes to set.");
				return false;
			}
			Space.PreEditChange(Property);
			FBlendParameter& Axis = *Property->ContainerPtrToValuePtr<FBlendParameter>(&Space, Operation.AxisIndex);
			Axis.DisplayName = Operation.Name;
			Axis.Min = static_cast<float>(Operation.Minimum);
			Axis.Max = static_cast<float>(Operation.Maximum);
			FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
			Space.PostEditChangeProperty(Event);
			return true;
		}

		/** Replaces every sample, the order the blend space editor adds them in. */
		bool SetSamples(UBlendSpace& Space, const FHyperAIStudioAnimationBackendOperation& Operation, FString& OutError)
		{
			while (Space.GetNumberOfBlendSamples() > 0)
			{
				Space.DeleteSample(Space.GetNumberOfBlendSamples() - 1);
			}
			for (const FHyperAIAnimationBlendSampleSpec& Sample : Operation.Samples)
			{
				UAnimSequence* Sequence = LoadExact<UAnimSequence>(Sample.SequencePath);
				if (!Sequence)
				{
					OutError = FString::Printf(TEXT("%s no longer resolves to an Animation Sequence."), *Sample.SequencePath);
					return false;
				}
				if (!Space.IsAnimationCompatibleWithSkeleton(Sequence))
				{
					OutError = FString::Printf(TEXT("%s uses another skeleton than %s."), *Sequence->GetName(), *Space.GetName());
					return false;
				}
				if (Space.AddSample(Sequence, Sample.Position) == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("The sample %s at (%g, %g) sits on or too close to another sample, or its additive type differs."),
						*Sequence->GetName(), Sample.Position.X, Sample.Position.Y);
					return false;
				}
			}
			return true;
		}
	}

	bool IsExecutablePlan(const TArray<FHyperAIStudioAnimationBackendOperation>& Operations)
	{
		// ponytail: one blend space per plan keeps the effect target, save and fresh check exact; add more on demand.
		return !Operations.IsEmpty() && Operations.FindByPredicate([&Operations](const FHyperAIStudioAnimationBackendOperation& Operation)
		{
			return (Operation.Kind != EKind::BlendSpaceCreate && Operation.Kind != EKind::BlendSpaceSetAxis
				&& Operation.Kind != EKind::BlendSpaceSetSamples) || Operation.TargetPath != Operations[0].TargetPath;
		}) == nullptr;
	}

	FString EffectTarget(const FHyperAIStudioAnimationTypedPayload& Payload)
	{
		return Payload.Operations.IsEmpty() ? FString() : Payload.Operations[0].TargetPath;
	}

	FString ContentKey(const FHyperAIStudioAnimationTypedPayload& Payload)
	{
		const UBlendSpace* Space = FindLoaded(EffectTarget(Payload));
		if (!Space) return FString();
		FString Canonical;
		AppendToken(Canonical, TEXT("hyperai.animation.blend-space-content.v1"));
		AppendToken(Canonical, Space->GetPathName());
		AppendToken(Canonical, Space->GetClass()->GetPathName());
		AppendToken(Canonical, Space->GetSkeleton() ? Space->GetSkeleton()->GetPathName() : FString());
		for (int32 Axis = 0; Axis < (Space->IsA<UBlendSpace1D>() ? 1 : 2); ++Axis)
		{
			const FBlendParameter& Parameter = Space->GetBlendParameter(Axis);
			AppendToken(Canonical, FString::Printf(TEXT("%s|%.9g|%.9g|%d"), *Parameter.DisplayName, Parameter.Min, Parameter.Max, Parameter.GridNum));
		}
		for (const FBlendSample& Sample : Space->GetBlendSamples())
		{
			AppendToken(Canonical, FString::Printf(TEXT("%s|%.9g|%.9g|%d"), Sample.Animation ? *Sample.Animation->GetPathName() : TEXT(""),
				Sample.SampleValue.X, Sample.SampleValue.Y, Sample.bIsValid ? 1 : 0));
		}
		return FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	}

	FHyperAIStudioDomainAdapterResult ExecutePhase(const EHyperAIStudioDomainExecutionActionKind Phase, const FHyperAIStudioAnimationTypedPayload& Payload)
	{
		FHyperAIStudioDomainAdapterResult Result;
		auto Finish = [&](const EHyperAIStudioDomainDispatchOutcome Outcome, const FString& Status, const FString& Diagnostic,
			const TCHAR* ResultPhase = nullptr, const FString& Key = FString())
		{
			UE_LOG(LogHyperAIStudioAnimationBlendSpace, Log, TEXT("Blend space phase %d finished %s. %s"), static_cast<int32>(Phase), *Status, *Diagnostic);
			Result.Outcome = Outcome;
			Result.StatusCode = Status.Left(FHyperAIStudioDomainLimits::MaxStatusCodeChars);
			Result.Diagnostic = Diagnostic.Left(FHyperAIStudioDomainLimits::MaxDiagnosticChars);
			if (Outcome == EHyperAIStudioDomainDispatchOutcome::Succeeded)
			{
				const TSharedRef<FHyperAIStudioAnimationResultPayload, ESPMode::ThreadSafe> Output =
					MakeShared<FHyperAIStudioAnimationResultPayload, ESPMode::ThreadSafe>();
				Output->Phase = ResultPhase;
				Output->Revision = Key;
				Output->bValid = true;
				Result.Payload = Output;
			}
			return Result;
		};
		const bool bApply = Phase == EHyperAIStudioDomainExecutionActionKind::Apply;
		const EHyperAIStudioDomainDispatchOutcome FailedOutcome = bApply
			? EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect
			: EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect;
		if (!IsInGameThread())
		{
			return Finish(FailedOutcome, TEXT("game_thread_required"), TEXT("Blend space edits run on the game thread."));
		}
		const FString Target = EffectTarget(Payload);
		if (bApply)
		{
			const bool bCreates = Payload.Operations[0].Kind == EKind::BlendSpaceCreate;
			UBlendSpace* Space = FindLoaded(Target);
			if (bCreates ? (Space || !FHyperAIStudioNames::IsFreePackage(FPackageName::ObjectPathToPackageName(Target))) : !Space)
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("target_changed"), bCreates
					? FString::Printf(TEXT("%s appeared after planning; nothing was created."), *Target)
					: FString::Printf(TEXT("%s is no longer loaded; nothing was changed."), *Target));
			}
			FString Error;
			if (!bCreates)
			{
				FHyperAIStudioAnimationValueSnapshot Snapshot;
				FString CaptureStatus;
				if (!FHyperAIStudioAnimationFacade::CaptureLoaded({Target}, TEXT("all"), true, Snapshot, CaptureStatus, Error, 250)
					|| Snapshot.Records.Num() != 1 || Snapshot.Records[0].Revision != Payload.Operations[0].ExpectedRevision)
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::RejectedBeforeEffect, TEXT("stale_revision"),
						FString::Printf(TEXT("%s changed after planning; nothing was applied. Inspect it again."), *Target));
				}
			}
			{
				const FScopedTransaction Transaction(LOCTEXT("ApplyBlendSpacePlan", "HyperAI: Apply Blend Space Plan"));
				for (const FHyperAIStudioAnimationBackendOperation& Operation : Payload.Operations)
				{
					bool bDone = true;
					if (Operation.Kind == EKind::BlendSpaceCreate)
					{
						Space = Create(Operation, Error);
						bDone = Space != nullptr;
					}
					else if (Operation.Kind == EKind::BlendSpaceSetAxis)
					{
						Space->Modify();
						bDone = SetAxis(*Space, Operation, Error);
					}
					else
					{
						Space->Modify();
						bDone = SetSamples(*Space, Operation, Error);
					}
					if (!bDone)
					{
						return Finish(Space ? EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect : EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect,
							TEXT("edit_op_failed"), Error + TEXT(" Undo reverts what the plan changed."));
					}
				}
				Space->ValidateSampleData();
				Space->ResampleData();
				Space->MarkPackageDirty();
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("applied"),
				FString::Printf(TEXT("%d ops applied to %s: %d samples."), Payload.Operations.Num(), *Space->GetName(), Space->GetNumberOfBlendSamples()),
				TEXT("applied"));
		}
		UBlendSpace* Space = FindLoaded(Target);
		if (!Space)
		{
			return Finish(FailedOutcome, TEXT("target_not_loaded"), FString::Printf(TEXT("%s is no longer loaded."), *Target));
		}
		switch (Phase)
		{
		case EHyperAIStudioDomainExecutionActionKind::Validate:
		{
			Space->ValidateSampleData();
			TArray<FString> Invalid;
			for (int32 Index = 0; Index < Space->GetBlendSamples().Num(); ++Index)
			{
				const FBlendSample& Sample = Space->GetBlendSamples()[Index];
				if (!Sample.bIsValid)
				{
					Invalid.Add(FString::Printf(TEXT("%d (%s at %g, %g)"), Index, Sample.Animation ? *Sample.Animation->GetName() : TEXT("no animation"),
						Sample.SampleValue.X, Sample.SampleValue.Y));
				}
			}
			if (!Invalid.IsEmpty())
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("invalid_samples"),
					TEXT("Samples outside the axes, overlapping, or with a mismatched additive type: ") + FString::Join(Invalid, TEXT(", "))
					+ TEXT(". Undo reverts the edit."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("validated"),
				FString::Printf(TEXT("All %d samples are valid."), Space->GetNumberOfBlendSamples()), TEXT("validated"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Save:
			if (!UEditorLoadingAndSavingUtils::SavePackages({Space->GetOutermost()}, /*bOnlyDirty=*/false))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("save_failed"),
					TEXT("The blend space changed but did not save; it remains in memory."));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("saved"), TEXT("Package saved."), TEXT("saved"));
		case EHyperAIStudioDomainExecutionActionKind::VerifyFresh:
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("fresh_captured"), TEXT("Blend space captured for verification."),
				TEXT("completed"), ContentKey(Payload));
		default:
			return Finish(FailedOutcome, TEXT("unsupported_phase"), TEXT("Blend space plans have no compile phase."));
		}
	}
}

FString FHyperAIStudioAnimationFreshVerifier::GetOwnerAdapterFingerprint() const
{
	return FHyperAIStudioAnimationContracts::GetBaseAdapterDescriptor().AdapterFingerprint;
}

bool FHyperAIStudioAnimationFreshVerifier::ResolveCanonicalEffectTarget(
	const IHyperAIStudioTypedArtifactPayload& Request, FString& OutCanonicalEffectTarget, FString& OutError)
{
	using Contracts = FHyperAIStudioAnimationContracts;
	OutCanonicalEffectTarget.Reset();
	OutError.Reset();
	if (Request.GetTypeId() != Contracts::PayloadTypeId || Request.GetSchemaFingerprint() != Contracts::PayloadSchemaFingerprint())
	{
		OutError = TEXT("Fresh verifier received the wrong typed animation request schema.");
		return false;
	}
	const FHyperAIStudioAnimationTypedPayload& Typed = static_cast<const FHyperAIStudioAnimationTypedPayload&>(Request);
	const bool bExecutable = HyperAIStudio::Animation::BlendSpace::IsExecutablePlan(Typed.Operations)
		|| HyperAIStudio::Animation::Timeline::IsExecutablePlan(Typed.Operations);
	if (!bExecutable
		|| Contracts::ComputePayloadSemanticFingerprint(Typed.Operations, Typed.BaseRevision) != Typed.SemanticFingerprint
		|| !Contracts::IsCanonicalProjectObjectPath(HyperAIStudio::Animation::BlendSpace::EffectTarget(Typed)))
	{
		OutError = TEXT("Fresh verifier rejected a plan that does not execute, its target, or its semantic seal.");
		return false;
	}
	OutCanonicalEffectTarget = HyperAIStudio::Animation::BlendSpace::EffectTarget(Typed);
	return true;
}

bool FHyperAIStudioAnimationFreshVerifier::VerifyFreshExact(
	const IHyperAIStudioTypedArtifactPayload& Request, const IHyperAIStudioDomainResultPayload& Result, FString& OutPostconditionHash, FString& OutError)
{
	using Contracts = FHyperAIStudioAnimationContracts;
	namespace BlendSpace = HyperAIStudio::Animation::BlendSpace;
	OutPostconditionHash.Reset();
	OutError.Reset();
	if (!IsInGameThread())
	{
		OutError = TEXT("Fresh blend space verification requires the game thread.");
		return false;
	}
	FString Target;
	if (!ResolveCanonicalEffectTarget(Request, Target, OutError)
		|| Result.GetTypeId() != Contracts::ResultTypeId || Result.GetSchemaFingerprint() != Contracts::ResultSchemaFingerprint())
	{
		if (OutError.IsEmpty()) OutError = TEXT("Fresh verifier received the wrong result schema.");
		return false;
	}
	const FHyperAIStudioAnimationTypedPayload& Typed = static_cast<const FHyperAIStudioAnimationTypedPayload&>(Request);
	const FHyperAIStudioAnimationResultPayload& TypedResult = static_cast<const FHyperAIStudioAnimationResultPayload&>(Result);
	if (TypedResult.Phase != TEXT("completed") || !Contracts::IsCanonicalSha256(TypedResult.Revision))
	{
		OutError = TEXT("Only a completed fresh-capture result may be verified.");
		return false;
	}
	const bool bBlendSpace = BlendSpace::IsExecutablePlan(Typed.Operations);
	const FString Key = bBlendSpace ? BlendSpace::ContentKey(Typed) : HyperAIStudio::Animation::Timeline::ContentKey(Typed);
	if (Key != TypedResult.Revision)
	{
		OutError = TEXT("The asset changed between fresh capture and verification.");
		return false;
	}
	const UObject* Asset = FindObject<UObject>(nullptr, *Target);
	if (!Asset || Asset->GetOutermost()->IsDirty())
	{
		OutError = TEXT("The asset still has unsaved changes after the save phase.");
		return false;
	}
	FString Canonical = (bBlendSpace ? TEXT("hyperai.animation.blend-space-postcondition.v1|") : TEXT("hyperai.animation.timeline-postcondition.v1|"))
		+ Typed.SemanticFingerprint + TEXT("|") + Key;
	OutPostconditionHash = FHyperAIStudioExtensionRuntime::ComputeBoundedSha256(Canonical);
	if (!Contracts::IsCanonicalSha256(OutPostconditionHash))
	{
		OutError = TEXT("Bounded postcondition hashing failed.");
		OutPostconditionHash.Reset();
		return false;
	}
	return true;
}

#undef LOCTEXT_NAMESPACE
