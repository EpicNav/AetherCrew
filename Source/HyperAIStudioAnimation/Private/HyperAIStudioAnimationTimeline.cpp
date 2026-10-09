// Games by Hyper 2026.

#include "HyperAIStudioAnimationTimeline.h"

#include "Animation/AnimComposite.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimationBlueprintLibrary.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Factories/AnimCompositeFactory.h"
#include "Factories/AnimMontageFactory.h"
#include "FileHelpers.h"
#include "HyperAIStudioAnimationBlendSpace.h"
#include "HyperAIStudioNames.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioAnimationTimeline"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioAnimationTimeline, Log, All);

namespace HyperAIStudio::Animation::Timeline
{
	namespace
	{
		using EKind = EHyperAIStudioAnimationOperationKind;
		using FOperation = FHyperAIStudioAnimationBackendOperation;

		bool IsMontageKind(const EKind Kind)
		{
			return Kind == EKind::MontageCreate || Kind == EKind::MontageSetSegments || Kind == EKind::MontageSetProperty
				|| Kind == EKind::MontageAddSection || Kind == EKind::MontageRemoveSection || Kind == EKind::MontageRenameSection
				|| Kind == EKind::MontageLinkSections || Kind == EKind::MontageAddNotify || Kind == EKind::MontageRemoveNotify;
		}

		bool IsSequenceKind(const EKind Kind)
		{
			return Kind == EKind::SequenceAddNotify || Kind == EKind::SequenceRemoveNotify || Kind == EKind::SequenceAddCurve
				|| Kind == EKind::SequenceRemoveCurve || Kind == EKind::SequenceSetProperty || Kind == EKind::SequenceAddSyncMarker
				|| Kind == EKind::SequenceRemoveSyncMarker;
		}

		bool IsCompositeKind(const EKind Kind)
		{
			return Kind == EKind::CompositeCreate || Kind == EKind::CompositeSetSegments;
		}

		bool IsSkeletonKind(const EKind Kind)
		{
			return Kind == EKind::SkeletonAddCompatible || Kind == EKind::SkeletonRemoveCompatible || Kind == EKind::SkeletonAddSlot;
		}

		bool IsTimelineKind(const EKind Kind)
		{
			return IsMontageKind(Kind) || IsSequenceKind(Kind) || IsCompositeKind(Kind) || IsSkeletonKind(Kind);
		}

		bool IsBlendSpaceKind(const EKind Kind)
		{
			return Kind == EKind::BlendSpaceCreate || Kind == EKind::BlendSpaceSetAxis || Kind == EKind::BlendSpaceSetSamples;
		}

		template <typename T>
		T* LoadExact(const FString& Path)
		{
			T* Object = Path.IsEmpty() ? nullptr : Cast<T>(FSoftObjectPath(Path).TryLoad());
			return IsValid(Object) && Object->GetPathName() == Path ? Object : nullptr;
		}

		UObject* FindTarget(const FString& Path)
		{
			UObject* Object = FindObject<UObject>(nullptr, *Path);
			return IsValid(Object) && Object->GetPathName() == Path ? Object : nullptr;
		}

		UPackage* NewPackage(const FString& ObjectPath, FName& OutName, FString& OutError)
		{
			const FString PackageName = FPackageName::ObjectPathToPackageName(ObjectPath);
			if (FindTarget(ObjectPath) || FPackageName::DoesPackageExist(PackageName))
			{
				OutError = FString::Printf(TEXT("%s appeared after planning; nothing was created."), *ObjectPath);
				return nullptr;
			}
			OutName = FName(*FPackageName::ObjectPathToObjectName(ObjectPath));
			return CreatePackage(*PackageName);
		}

		UAnimMontage* CreateMontage(const FOperation& Operation, FString& OutError)
		{
			UObject* Reference = LoadExact<UObject>(Operation.ReferencePath);
			UAnimSequence* Source = Cast<UAnimSequence>(Reference);
			USkeleton* Skeleton = Source ? Source->GetSkeleton() : Cast<USkeleton>(Reference);
			if (!Skeleton)
			{
				OutError = FString::Printf(TEXT("%s is neither an Animation Sequence nor a Skeleton."), *Operation.ReferencePath);
				return nullptr;
			}
			FName Name;
			UPackage* Package = NewPackage(Operation.TargetPath, Name, OutError);
			if (!Package)
			{
				return nullptr;
			}
			UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
			Factory->TargetSkeleton = Source ? nullptr : Skeleton;
			Factory->SourceAnimation = Source;
			UAnimMontage* Montage = Cast<UAnimMontage>(Factory->FactoryCreateNew(UAnimMontage::StaticClass(), Package, Name,
				RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
			if (!Montage)
			{
				OutError = TEXT("The montage factory created nothing.");
				return nullptr;
			}
			if (!Operation.Name.IsEmpty() && !Montage->SlotAnimTracks.IsEmpty())
			{
				Montage->SlotAnimTracks[0].SlotName = FName(*Operation.Name);
			}
			FAssetRegistryModule::AssetCreated(Montage);
			Package->MarkPackageDirty();
			return Montage;
		}

		UAnimComposite* CreateComposite(const FOperation& Operation, FString& OutError)
		{
			USkeleton* Skeleton = LoadExact<USkeleton>(Operation.ReferencePath);
			FName Name;
			UPackage* Package = Skeleton ? NewPackage(Operation.TargetPath, Name, OutError) : nullptr;
			if (!Package)
			{
				OutError = OutError.IsEmpty() ? FString::Printf(TEXT("%s is not a Skeleton."), *Operation.ReferencePath) : OutError;
				return nullptr;
			}
			UAnimCompositeFactory* Factory = NewObject<UAnimCompositeFactory>();
			Factory->TargetSkeleton = Skeleton;
			UAnimComposite* Composite = Cast<UAnimComposite>(Factory->FactoryCreateNew(UAnimComposite::StaticClass(), Package, Name,
				RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
			if (!Composite)
			{
				OutError = TEXT("The composite factory created nothing.");
				return nullptr;
			}
			FAssetRegistryModule::AssetCreated(Composite);
			Package->MarkPackageDirty();
			return Composite;
		}

		/** Replaces a track's segments, back to back from its start; returns the track's length. */
		bool SetSegments(FAnimTrack& Track, const TArray<FHyperAIAnimationSegmentSpec>& Segments, float& OutLength, FString& OutError)
		{
			Track.AnimSegments.Reset();
			float Position = 0.f;
			for (const FHyperAIAnimationSegmentSpec& Spec : Segments)
			{
				UAnimSequence* Sequence = LoadExact<UAnimSequence>(Spec.SequencePath);
				if (!Sequence)
				{
					OutError = FString::Printf(TEXT("%s no longer resolves to an Animation Sequence."), *Spec.SequencePath);
					return false;
				}
				FAnimSegment Segment;
				Segment.SetAnimReference(Sequence, /*bInitialize=*/true);
				Segment.AnimStartTime = static_cast<float>(Spec.StartSeconds);
				Segment.AnimEndTime = Spec.EndSeconds == -1.0 ? Sequence->GetPlayLength() : static_cast<float>(Spec.EndSeconds);
				Segment.AnimPlayRate = static_cast<float>(Spec.PlayRate);
				Segment.LoopingCount = Spec.LoopCount;
				Segment.StartPos = Position;
				Position += Segment.GetLength();
				Track.AnimSegments.Add(Segment);
			}
			OutLength = Position;
			return true;
		}

		/** The notify track by name, made when missing; empty picks the first, making "1" when there is none. */
		int32 EnsureTrack(UAnimSequenceBase& Asset, const FString& Name)
		{
			if (Name.IsEmpty() && !Asset.AnimNotifyTracks.IsEmpty())
			{
				return 0;
			}
			const FName TrackName(Name.IsEmpty() ? TEXT("1") : *Name);
			const int32 Existing = Asset.AnimNotifyTracks.IndexOfByPredicate([TrackName](const FAnimNotifyTrack& Track) { return Track.TrackName == TrackName; });
			if (Existing != INDEX_NONE)
			{
				return Existing;
			}
			FAnimNotifyTrack Track;
			Track.TrackName = TrackName;
			Track.TrackColor = FLinearColor::White;
			return Asset.AnimNotifyTracks.Add(Track);
		}

		UClass* ResolveNotifyClass(const FString& Path, const bool bState)
		{
			UClass* Class = FindObject<UClass>(nullptr, *Path);
			Class = Class ? Class : LoadObject<UClass>(nullptr, *Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
			if (!Class)
			{
				if (const UBlueprint* Blueprint = LoadExact<UBlueprint>(Path))
				{
					Class = Blueprint->GeneratedClass;
				}
			}
			const UClass* Base = bState ? UAnimNotifyState::StaticClass() : UAnimNotify::StaticClass();
			return Class && Class->IsChildOf(Base) && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated) ? Class : nullptr;
		}

		/** A notify added the way the notify panel adds one: linked to its segment, its trigger offset set, on its track. */
		bool AddNotify(UAnimSequenceBase& Asset, const FHyperAIAnimationNotifySpec& Spec, const bool bMontage, FString& OutError)
		{
			const float Time = static_cast<float>(Spec.TimeSeconds);
			if (Time > Asset.GetPlayLength() + KINDA_SMALL_NUMBER)
			{
				OutError = FString::Printf(TEXT("%s is %.3f s long; a notify at %.3f s is past its end."), *Asset.GetName(), Asset.GetPlayLength(), Time);
				return false;
			}
			const int32 TrackIndex = EnsureTrack(Asset, Spec.Track);
			UObject* Instance = nullptr;
			if (Spec.Kind != TEXT("named_event"))
			{
				const bool bState = Spec.Kind == TEXT("notify_state");
				UClass* Class = ResolveNotifyClass(Spec.NotifyClass, bState);
				if (!Class)
				{
					OutError = FString::Printf(TEXT("%s is not an %s class."), *Spec.NotifyClass, bState ? TEXT("AnimNotifyState") : TEXT("AnimNotify"));
					return false;
				}
				// Made without undo tracking until its properties are set, so undo and redo never meet it half made.
				Instance = NewObject<UObject>(&Asset, Class, NAME_None, RF_NoFlags);
				for (const FHyperAIAnimationNameMapping& Property : Spec.Properties)
				{
					FProperty* Found = FindFProperty<FProperty>(Class, FName(*Property.Source));
					// The text import makes names of it and loads what it references.
					if (Found && !FHyperAIStudioNames::CanImportText(*Found, Property.Target, *Property.Source, &OutError)) return false;
					// Only a reference is loaded first. A name, string or text property takes any text, a path with a double
					// slash too, and loading that would make its package and stop the editor.
					if (Found && (Found->IsA<FObjectPropertyBase>() || Found->IsA<FInterfaceProperty>()) && Property.Target.StartsWith(TEXT("/"))
						&& Property.Target.Contains(TEXT(".")) && FHyperAIStudioNames::IsObjectPath(Property.Target, nullptr))
					{
						FSoftObjectPath(Property.Target).TryLoad();
					}
					if (!Found || !Found->ImportText_InContainer(*Property.Target, Instance, Instance, PPF_None))
					{
						OutError = FString::Printf(TEXT("%s's %s does not take '%s'."), *Class->GetName(), *Property.Source, *Property.Target);
						return false;
					}
				}
				Instance->SetFlags(RF_Transactional);
			}
			FAnimNotifyEvent& Event = Asset.Notifies.AddDefaulted_GetRef();
			Event.Guid = FGuid::NewGuid();
			Event.Link(&Asset, Time);
			Event.TriggerTimeOffset = GetTriggerTimeOffsetForType(Asset.CalculateOffsetForNotify(Time));
			Event.TrackIndex = TrackIndex;
			if (UAnimNotifyState* State = Cast<UAnimNotifyState>(Instance))
			{
				Event.NotifyStateClass = State;
				Event.NotifyName = FName(*State->GetNotifyName());
				Event.SetDuration(static_cast<float>(Spec.DurationSeconds));
				Event.EndLink.Link(&Asset, Event.EndLink.GetTime());
			}
			else if (UAnimNotify* Notify = Cast<UAnimNotify>(Instance))
			{
				Event.Notify = Notify;
				Event.NotifyName = FName(*Notify->GetNotifyName());
			}
			else
			{
				Event.NotifyName = FName(*Spec.Name);
			}
			if (bMontage && Spec.bBranchingPoint)
			{
				Event.MontageTickType = EMontageNotifyTickType::BranchingPoint;
			}
			Asset.RefreshCacheData();
			return true;
		}

		/** A notify by the stable id inspect gave it: its GUID, or its index for one without. */
		bool RemoveNotify(UAnimSequenceBase& Asset, const FString& StableId, FString& OutError)
		{
			const FString Prefix = TEXT("notify:") + Asset.GetPathName() + TEXT(":");
			const FString Tail = StableId.StartsWith(Prefix) ? StableId.Mid(Prefix.Len()) : FString();
			FGuid Guid;
			int32 Index = INDEX_NONE;
			if (FGuid::Parse(Tail, Guid))
			{
				Index = Asset.Notifies.IndexOfByPredicate([&Guid](const FAnimNotifyEvent& Event) { return Event.Guid == Guid; });
			}
			else if (Tail.IsNumeric())
			{
				Index = FCString::Atoi(*Tail);
			}
			if (!Asset.Notifies.IsValidIndex(Index))
			{
				OutError = FString::Printf(TEXT("%s has no notify %s now; inspect it again."), *Asset.GetName(), *StableId);
				return false;
			}
			Asset.Notifies.RemoveAt(Index);
			Asset.RefreshCacheData();
			return true;
		}

		int32 FindSection(const UAnimMontage& Montage, const FString& Name)
		{
			return Montage.CompositeSections.IndexOfByPredicate([Name = FName(*Name)](const FCompositeSection& Section) { return Section.SectionName == Name; });
		}

		bool ApplyMontageOp(UAnimMontage& Montage, const FOperation& Operation, FString& OutError)
		{
			switch (Operation.Kind)
			{
			case EKind::MontageSetSegments:
			{
				FSlotAnimationTrack* Slot = Operation.Name.IsEmpty() ? (Montage.SlotAnimTracks.IsEmpty() ? nullptr : &Montage.SlotAnimTracks[0])
					: Montage.SlotAnimTracks.FindByPredicate([Name = FName(*Operation.Name)](const FSlotAnimationTrack& Track) { return Track.SlotName == Name; });
				if (!Slot)
				{
					OutError = FString::Printf(TEXT("The montage has no slot track %s."), *Operation.Name);
					return false;
				}
				float Length = 0.f;
				if (!SetSegments(Slot->AnimTrack, Operation.Segments, Length, OutError))
				{
					return false;
				}
				// Public on the composite base; the montage overrides them privately.
				UAnimCompositeBase& Base = Montage;
				Base.SetCompositeLength(Length);
				Base.UpdateCommonTargetFrameRate();
				return true;
			}
			case EKind::MontageSetProperty:
			{
				const FString& Value = Operation.Value;
				const float Number = FCString::Atof(*Value);
				if (Operation.Name == TEXT("blend_in")) Montage.BlendIn.SetBlendTime(Number);
				else if (Operation.Name == TEXT("blend_out")) Montage.BlendOut.SetBlendTime(Number);
				else if (Operation.Name == TEXT("blend_out_trigger")) Montage.BlendOutTriggerTime = Number;
				else if (Operation.Name == TEXT("auto_blend_out")) Montage.bEnableAutoBlendOut = Value == TEXT("true");
				else if (Operation.Name == TEXT("rate_scale")) Montage.RateScale = Number;
				else if (Operation.Name == TEXT("slot") && !Montage.SlotAnimTracks.IsEmpty()) Montage.SlotAnimTracks[0].SlotName = FName(*Value);
				else
				{
					OutError = FString::Printf(TEXT("The montage has no property %s."), *Operation.Name);
					return false;
				}
				return true;
			}
			case EKind::MontageAddSection:
				if (Montage.AddAnimCompositeSection(FName(*Operation.Name), static_cast<float>(Operation.Minimum)) == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("The montage has a section %s already."), *Operation.Name);
					return false;
				}
				return true;
			case EKind::MontageRemoveSection:
			case EKind::MontageRenameSection:
			case EKind::MontageLinkSections:
			{
				const int32 Index = FindSection(Montage, Operation.Name);
				if (Index == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("The montage has no section %s."), *Operation.Name);
					return false;
				}
				const FName Old = Montage.CompositeSections[Index].SectionName;
				if (Operation.Kind == EKind::MontageLinkSections)
				{
					Montage.CompositeSections[Index].NextSectionName = Operation.SecondaryName.IsEmpty() ? NAME_None : FName(*Operation.SecondaryName);
					return true;
				}
				// A removed or renamed section's links follow it.
				const FName New = Operation.Kind == EKind::MontageRenameSection ? FName(*Operation.SecondaryName) : NAME_None;
				for (FCompositeSection& Section : Montage.CompositeSections)
				{
					if (Section.NextSectionName == Old)
					{
						Section.NextSectionName = New;
					}
				}
				if (Operation.Kind == EKind::MontageRenameSection)
				{
					Montage.CompositeSections[Index].SectionName = New;
				}
				else
				{
					Montage.DeleteAnimCompositeSection(Index);
				}
				return true;
			}
			case EKind::MontageAddNotify:
				return AddNotify(Montage, Operation.Notify, /*bMontage=*/true, OutError);
			case EKind::MontageRemoveNotify:
				return RemoveNotify(Montage, Operation.StableId, OutError);
			default:
				OutError = TEXT("That op is not for a montage.");
				return false;
			}
		}

		/** The montage the way its editor leaves it: sections in time order from 0, every section and notify linked to its segment. */
		void FinishMontage(UAnimMontage& Montage)
		{
			Montage.CompositeSections.Sort([](const FCompositeSection& A, const FCompositeSection& B) { return A.GetTime() < B.GetTime(); });
			UAnimMontageFactory::EnsureStartingSection(&Montage);
			Montage.UpdateLinkableElements();
			Montage.RefreshCacheData();
		}

		bool ApplySequenceOp(UAnimSequence& Sequence, const FOperation& Operation, FString& OutError)
		{
			switch (Operation.Kind)
			{
			case EKind::SequenceAddNotify:
				return AddNotify(Sequence, Operation.Notify, /*bMontage=*/false, OutError);
			case EKind::SequenceRemoveNotify:
				return RemoveNotify(Sequence, Operation.StableId, OutError);
			case EKind::SequenceAddCurve:
			{
				const FName Name(*Operation.Name);
				UAnimationBlueprintLibrary::AddCurve(&Sequence, Name, ERawCurveTrackTypes::RCT_Float, /*bMetaDataCurve=*/false);
				if (!Operation.Keys.IsEmpty())
				{
					TArray<float> Times;
					TArray<float> Values;
					for (const FHyperAIAnimationCurveKey& Key : Operation.Keys)
					{
						Times.Add(static_cast<float>(Key.TimeSeconds));
						Values.Add(static_cast<float>(Key.Value));
					}
					UAnimationBlueprintLibrary::AddFloatCurveKeys(&Sequence, Name, Times, Values);
				}
				return true;
			}
			case EKind::SequenceRemoveCurve:
				UAnimationBlueprintLibrary::RemoveCurve(&Sequence, FName(*Operation.Name), /*bRemoveNameFromSkeleton=*/false);
				return true;
			case EKind::SequenceSetProperty:
			{
				const bool bOn = Operation.Value == TEXT("true");
				if (Operation.Name == TEXT("root_motion")) UAnimationBlueprintLibrary::SetRootMotionEnabled(&Sequence, bOn);
				else if (Operation.Name == TEXT("force_root_lock")) UAnimationBlueprintLibrary::SetIsRootMotionLockForced(&Sequence, bOn);
				else if (Operation.Name == TEXT("root_lock"))
				{
					UAnimationBlueprintLibrary::SetRootMotionLockType(&Sequence, Operation.Value == TEXT("anim_first_frame") ? ERootMotionRootLock::AnimFirstFrame
						: Operation.Value == TEXT("zero") ? ERootMotionRootLock::Zero : ERootMotionRootLock::RefPose);
				}
				else if (Operation.Name == TEXT("loop")) Sequence.bLoop = bOn;
				else if (Operation.Name == TEXT("rate_scale")) Sequence.RateScale = FCString::Atof(*Operation.Value);
				else
				{
					OutError = FString::Printf(TEXT("The sequence has no property %s."), *Operation.Name);
					return false;
				}
				return true;
			}
			case EKind::SequenceAddSyncMarker:
			{
				const int32 Track = EnsureTrack(Sequence, Operation.SecondaryName);
				UAnimationBlueprintLibrary::AddAnimationSyncMarker(&Sequence, FName(*Operation.Name), static_cast<float>(Operation.Minimum),
					Sequence.AnimNotifyTracks[Track].TrackName);
				return true;
			}
			case EKind::SequenceRemoveSyncMarker:
				if (UAnimationBlueprintLibrary::RemoveAnimationSyncMarkersByName(&Sequence, FName(*Operation.Name)) == 0)
				{
					OutError = FString::Printf(TEXT("The sequence has no sync marker %s."), *Operation.Name);
					return false;
				}
				return true;
			default:
				OutError = TEXT("That op is not for an Animation Sequence.");
				return false;
			}
		}

		bool ApplySkeletonOp(USkeleton& Skeleton, const FOperation& Operation, FString& OutError)
		{
			if (Operation.Kind == EKind::SkeletonAddSlot)
			{
				const FName Group = Operation.SecondaryName.IsEmpty() ? FAnimSlotGroup::DefaultGroupName : FName(*Operation.SecondaryName);
				if (!Skeleton.FindAnimSlotGroup(Group))
				{
					Skeleton.AddSlotGroupName(Group);
				}
				Skeleton.RegisterSlotNode(FName(*Operation.Name));
				Skeleton.SetSlotGroupName(FName(*Operation.Name), Group);
				if (!Skeleton.ContainsSlotName(FName(*Operation.Name)))
				{
					OutError = FString::Printf(TEXT("Unreal did not add the slot %s."), *Operation.Name);
					return false;
				}
				return true;
			}
			USkeleton* Other = LoadExact<USkeleton>(Operation.ReferencePath);
			if (!Other)
			{
				OutError = FString::Printf(TEXT("%s is not a Skeleton."), *Operation.ReferencePath);
				return false;
			}
			if (Operation.Kind == EKind::SkeletonAddCompatible)
			{
				Skeleton.AddCompatibleSkeleton(Other);
			}
			else
			{
				Skeleton.RemoveCompatibleSkeleton(Other);
			}
			return true;
		}

		FString Seconds(const double Value)
		{
			return FString::Printf(TEXT("%g s"), Value);
		}
	}

	bool AreExecutableKinds(const TArray<FHyperAIStudioAnimationBackendOperation>& Operations)
	{
		return !Operations.IsEmpty() && !Operations.ContainsByPredicate([](const FOperation& Operation)
		{
			return !IsTimelineKind(Operation.Kind) && !IsBlendSpaceKind(Operation.Kind);
		});
	}

	bool IsExecutablePlan(const TArray<FHyperAIStudioAnimationBackendOperation>& Operations)
	{
		// ponytail: one asset per plan keeps the effect target, the save and the fresh check exact, as blend spaces do.
		return !Operations.IsEmpty() && !Operations.ContainsByPredicate([&Operations](const FOperation& Operation)
		{
			return !IsTimelineKind(Operation.Kind) || Operation.TargetPath != Operations[0].TargetPath;
		});
	}

	FString ContentKey(const FHyperAIStudioAnimationTypedPayload& Payload)
	{
		const FString Target = BlendSpace::EffectTarget(Payload);
		FHyperAIStudioAnimationValueSnapshot Snapshot;
		FString Status;
		FString Diagnostic;
		if (Target.IsEmpty() || !FindTarget(Target)
			|| !FHyperAIStudioAnimationFacade::CaptureLoaded({Target}, TEXT("all"), true, Snapshot, Status, Diagnostic, 250)
			|| Snapshot.Records.Num() != 1 || !FHyperAIStudioAnimationContracts::IsCanonicalSha256(Snapshot.Records[0].Revision))
		{
			return FString();
		}
		return Snapshot.Records[0].Revision;
	}

	FString Describe(const FHyperAIStudioAnimationBackendOperation& Operation)
	{
		switch (Operation.Kind)
		{
		case EKind::MontageCreate: return FString::Printf(TEXT("create a montage from %s"), *FPackageName::ObjectPathToObjectName(Operation.ReferencePath));
		case EKind::CompositeCreate: return TEXT("create a composite");
		case EKind::MontageSetSegments:
		case EKind::CompositeSetSegments: return FString::Printf(TEXT("replace the segments with %d"), Operation.Segments.Num());
		case EKind::MontageSetProperty:
		case EKind::SequenceSetProperty: return FString::Printf(TEXT("%s = %s"), *Operation.Name, *Operation.Value);
		case EKind::MontageAddSection: return FString::Printf(TEXT("section %s at %s"), *Operation.Name, *Seconds(Operation.Minimum));
		case EKind::MontageRemoveSection: return FString::Printf(TEXT("remove section %s"), *Operation.Name);
		case EKind::MontageRenameSection: return FString::Printf(TEXT("rename section %s to %s"), *Operation.Name, *Operation.SecondaryName);
		case EKind::MontageLinkSections: return FString::Printf(TEXT("section %s then %s"), *Operation.Name,
			Operation.SecondaryName.IsEmpty() ? TEXT("ends") : *Operation.SecondaryName);
		case EKind::MontageAddNotify:
		case EKind::SequenceAddNotify: return FString::Printf(TEXT("%s %s at %s"), *Operation.Notify.Kind,
			Operation.Notify.NotifyClass.IsEmpty() ? *Operation.Notify.Name : *FPackageName::ObjectPathToObjectName(Operation.Notify.NotifyClass),
			*Seconds(Operation.Notify.TimeSeconds));
		case EKind::MontageRemoveNotify:
		case EKind::SequenceRemoveNotify: return TEXT("remove a notify");
		case EKind::SequenceAddCurve: return FString::Printf(TEXT("curve %s with %d keys"), *Operation.Name, Operation.Keys.Num());
		case EKind::SequenceRemoveCurve: return FString::Printf(TEXT("remove curve %s"), *Operation.Name);
		case EKind::SequenceAddSyncMarker: return FString::Printf(TEXT("sync marker %s at %s"), *Operation.Name, *Seconds(Operation.Minimum));
		case EKind::SequenceRemoveSyncMarker: return FString::Printf(TEXT("remove sync markers %s"), *Operation.Name);
		case EKind::SkeletonAddSlot: return FString::Printf(TEXT("slot %s"), *Operation.Name);
		case EKind::SkeletonAddCompatible: return FString::Printf(TEXT("compatible with %s"), *FPackageName::ObjectPathToObjectName(Operation.ReferencePath));
		case EKind::SkeletonRemoveCompatible: return FString::Printf(TEXT("no longer compatible with %s"), *FPackageName::ObjectPathToObjectName(Operation.ReferencePath));
		default: return Operation.Type;
		}
	}

	FHyperAIStudioDomainAdapterResult ExecutePhase(const EHyperAIStudioDomainExecutionActionKind Phase, const FHyperAIStudioAnimationTypedPayload& Payload)
	{
		FHyperAIStudioDomainAdapterResult Result;
		auto Finish = [&](const EHyperAIStudioDomainDispatchOutcome Outcome, const FString& Status, const FString& Diagnostic,
			const TCHAR* ResultPhase = nullptr, const FString& Key = FString())
		{
			UE_LOG(LogHyperAIStudioAnimationTimeline, Log, TEXT("Animation plan phase %d finished %s. %s"), static_cast<int32>(Phase), *Status, *Diagnostic);
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
			return Finish(FailedOutcome, TEXT("game_thread_required"), TEXT("Animation asset edits run on the game thread."));
		}
		const FString Target = BlendSpace::EffectTarget(Payload);
		if (bApply)
		{
			const bool bCreates = Payload.Operations[0].bCreatesTarget;
			UObject* Asset = FindTarget(Target);
			if (bCreates ? (Asset || FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Target))) : !Asset)
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
				const FScopedTransaction Transaction(LOCTEXT("ApplyAnimationPlan", "HyperAI: Apply Animation Plan"));
				for (const FOperation& Operation : Payload.Operations)
				{
					bool bDone = true;
					if (Operation.Kind == EKind::MontageCreate)
					{
						Asset = CreateMontage(Operation, Error);
						bDone = Asset != nullptr;
					}
					else if (Operation.Kind == EKind::CompositeCreate)
					{
						Asset = CreateComposite(Operation, Error);
						bDone = Asset != nullptr;
					}
					else if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset); Montage && IsMontageKind(Operation.Kind))
					{
						Montage->Modify();
						bDone = ApplyMontageOp(*Montage, Operation, Error);
					}
					else if (UAnimComposite* Composite = Cast<UAnimComposite>(Asset); Composite && Operation.Kind == EKind::CompositeSetSegments)
					{
						Composite->Modify();
						float Length = 0.f;
						bDone = SetSegments(Composite->AnimationTrack, Operation.Segments, Length, Error);
						if (bDone)
						{
							UAnimCompositeBase& Base = *Composite;
							Base.SetCompositeLength(Length);
							Base.UpdateCommonTargetFrameRate();
						}
					}
					else if (UAnimSequence* Sequence = Cast<UAnimSequence>(Asset); Sequence && IsSequenceKind(Operation.Kind))
					{
						Sequence->Modify();
						bDone = ApplySequenceOp(*Sequence, Operation, Error);
					}
					else if (USkeleton* Skeleton = Cast<USkeleton>(Asset); Skeleton && IsSkeletonKind(Operation.Kind))
					{
						Skeleton->Modify();
						bDone = ApplySkeletonOp(*Skeleton, Operation, Error);
					}
					else
					{
						bDone = false;
						Error = FString::Printf(TEXT("%s does not suit %s."), *Operation.Type, Asset ? *Asset->GetClass()->GetName() : TEXT("a missing asset"));
					}
					if (!bDone)
					{
						return Finish(Asset ? EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect : EHyperAIStudioDomainDispatchOutcome::FailedBeforeEffect,
							TEXT("edit_op_failed"), Error + TEXT(" Undo reverts what the plan changed."));
					}
				}
				if (UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
				{
					FinishMontage(*Montage);
				}
				else if (UAnimSequenceBase* Timeline = Cast<UAnimSequenceBase>(Asset))
				{
					Timeline->RefreshCacheData();
				}
				Asset->PostEditChange();
				Asset->MarkPackageDirty();
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("applied"),
				FString::Printf(TEXT("%d ops applied to %s."), Payload.Operations.Num(), *Asset->GetName()), TEXT("applied"));
		}
		UObject* Asset = FindTarget(Target);
		if (!Asset)
		{
			return Finish(FailedOutcome, TEXT("target_not_loaded"), FString::Printf(TEXT("%s is no longer loaded."), *Target));
		}
		switch (Phase)
		{
		case EHyperAIStudioDomainExecutionActionKind::Validate:
		{
			if (const UAnimMontage* Montage = Cast<UAnimMontage>(Asset))
			{
				TArray<FString> Problems;
				for (const FSlotAnimationTrack& Slot : Montage->SlotAnimTracks)
				{
					for (const FAnimSegment& Segment : Slot.AnimTrack.AnimSegments)
					{
						if (!Segment.GetAnimReference())
						{
							Problems.Add(FString::Printf(TEXT("a segment in slot %s plays nothing"), *Slot.SlotName.ToString()));
						}
					}
				}
				if (Montage->CompositeSections.IsEmpty() || Montage->CompositeSections[0].GetTime() > KINDA_SMALL_NUMBER)
				{
					Problems.Add(TEXT("no section starts at 0"));
				}
				for (const FCompositeSection& Section : Montage->CompositeSections)
				{
					if (!Section.NextSectionName.IsNone() && Montage->GetSectionIndex(Section.NextSectionName) == INDEX_NONE)
					{
						Problems.Add(FString::Printf(TEXT("section %s links to a missing %s"), *Section.SectionName.ToString(), *Section.NextSectionName.ToString()));
					}
				}
				if (!Problems.IsEmpty())
				{
					return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("invalid_montage"),
						FString::Join(Problems, TEXT("; ")) + TEXT(". Undo reverts the edit."));
				}
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("validated"), FString::Printf(TEXT("%s checked."), *Asset->GetName()), TEXT("validated"));
		}
		case EHyperAIStudioDomainExecutionActionKind::Save:
			if (!UEditorLoadingAndSavingUtils::SavePackages({Asset->GetOutermost()}, /*bOnlyDirty=*/false))
			{
				return Finish(EHyperAIStudioDomainDispatchOutcome::FailedAfterKnownEffect, TEXT("save_failed"),
					FString::Printf(TEXT("%s changed but did not save; it remains in memory."), *Asset->GetName()));
			}
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("saved"), TEXT("Package saved."), TEXT("saved"));
		case EHyperAIStudioDomainExecutionActionKind::VerifyFresh:
			return Finish(EHyperAIStudioDomainDispatchOutcome::Succeeded, TEXT("fresh_captured"), TEXT("Asset captured for verification."),
				TEXT("completed"), ContentKey(Payload));
		default:
			return Finish(FailedOutcome, TEXT("unsupported_phase"), TEXT("Animation asset plans have no compile phase."));
		}
	}
}

#undef LOCTEXT_NAMESPACE
