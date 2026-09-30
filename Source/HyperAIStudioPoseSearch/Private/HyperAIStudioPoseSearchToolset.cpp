// Games by Hyper 2026.

#include "HyperAIStudioPoseSearchToolset.h"

#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"
#include "Animation/MirrorDataTable.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioSourceControl.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "PoseSearch/PoseSearchDatabase.h"
#include "PoseSearch/PoseSearchDerivedData.h"
#include "PoseSearch/PoseSearchFeatureChannel.h"
#include "PoseSearch/PoseSearchIndex.h"
#include "PoseSearch/PoseSearchSchema.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioPoseSearch, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioPoseSearch"

FString FHyperAIStudioPoseSearchContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioPoseSearch.HyperAIStudioPoseSearchToolset");
}

TArray<FString> FHyperAIStudioPoseSearchContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName};
}

bool FHyperAIStudioPoseSearchContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::MotionMatching::Private
{
	using C = FHyperAIStudioPoseSearchContracts;

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

	FString KindOf(const FHyperAIPoseSearchOp& Op)
	{
		return Op.Kind.TrimStartAndEnd().ToLower();
	}

	bool IsProjectPath(const FString& Path, FString& OutError)
	{
		const FString Package = PackageOf(Path);
		FText Reason;
		if (!FPackageName::IsValidLongPackageName(Package, /*bIncludeReadOnlyRoots=*/false, &Reason)
			|| Package.StartsWith(TEXT("/Script/")) || Package.StartsWith(TEXT("/Engine/")) || Package.StartsWith(TEXT("/Temp/")))
		{
			OutError = FString::Printf(TEXT("'%s' is not a path in the project, such as /Game/MotionMatching/PSD_Locomotion. %s"), *Path, *Reason.ToString());
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
		return Path.TrimStartAndEnd().IsEmpty() ? nullptr : LoadObject<T>(nullptr, *ObjectPath(Path), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	FString NameOf(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString(TEXT("none"));
	}

	/** Sequences, composites, montages and blend spaces: what a database searches. */
	UObject* LoadAnimation(const FString& Path)
	{
		UObject* Asset = Load<UObject>(Path);
		return Cast<UAnimSequenceBase>(Asset) || Cast<UBlendSpace>(Asset) ? Asset : nullptr;
	}

	const USkeleton* SkeletonOf(const UObject* Animation)
	{
		if (const UAnimationAsset* Asset = Cast<UAnimationAsset>(Animation))
		{
			return Asset->GetSkeleton();
		}
		return nullptr;
	}

	bool IsSkeletonFor(const USkeleton* Schema, const USkeleton* Animation)
	{
		return Schema && Animation && (Schema == Animation || Schema->IsCompatibleForEditor(Animation));
	}

	/** A channel type by its short name (Position) or class name (PoseSearchFeatureChannel_Position). */
	UClass* ChannelClass(const FString& Name)
	{
		const FString Wanted = Name.TrimStartAndEnd();
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (Class->IsChildOf(UPoseSearchFeatureChannel::StaticClass()) && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
				&& (Class->GetName().Equals(Wanted, ESearchCase::IgnoreCase) || Class->GetName().Equals(TEXT("PoseSearchFeatureChannel_") + Wanted, ESearchCase::IgnoreCase)))
			{
				return Class;
			}
		}
		return nullptr;
	}

	FString ChannelName(const UClass& Class)
	{
		FString Name = Class.GetName();
		Name.RemoveFromStart(TEXT("PoseSearchFeatureChannel_"));
		return Name;
	}

	FString ChannelTypes()
	{
		TArray<FString> Names;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			if (It->IsChildOf(UPoseSearchFeatureChannel::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				Names.Add(ChannelName(**It));
			}
		}
		Names.Sort();
		return FString::Join(Names, TEXT(", "));
	}

	bool IsSetting(const FProperty& Property)
	{
		return Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | CPF_DisableEditOnInstance);
	}

	/** An editable property by its name, any case, with or without a bool's leading b. */
	const FProperty* FindSetting(const UStruct& Type, const FString& Name, const TSet<FName>& Hidden)
	{
		const FString Wanted = Name.TrimStartAndEnd();
		for (TFieldIterator<FProperty> It(&Type); It; ++It)
		{
			if (IsSetting(**It) && !Hidden.Contains(It->GetFName())
				&& (It->GetName().Equals(Wanted, ESearchCase::IgnoreCase) || It->GetName().Equals(TEXT("b") + Wanted, ESearchCase::IgnoreCase)))
			{
				return *It;
			}
		}
		return nullptr;
	}

	FString SettingNames(const UStruct& Type, const TSet<FName>& Hidden)
	{
		TArray<FString> Names;
		for (TFieldIterator<FProperty> It(&Type); It; ++It)
		{
			if (IsSetting(**It) && !Hidden.Contains(It->GetFName()))
			{
				Names.Add(It->GetName());
			}
		}
		return FString::Join(Names, TEXT(", "));
	}

	FString ExportValue(const FProperty& Property, const void* Container, UObject* Owner)
	{
		// The container as its own delta: against null, a value equal to its type's zero exports as nothing.
		FString Value;
		Property.ExportText_InContainer(0, Value, Container, Container, Owner, PPF_None);
		return Value.Len() > 300 ? Value.Left(300) + TEXT("...") : Value;
	}

	/** Each setting written into Container (an instance of Type) in Unreal's text form; false, with why, at the first that does not take. */
	bool ImportSettings(const UStruct& Type, void* Container, UObject* Owner, const TArray<FHyperAIPoseSearchSetting>& Settings, const TSet<FName>& Hidden,
		FString& OutError)
	{
		for (const FHyperAIPoseSearchSetting& Setting : Settings)
		{
			const FProperty* Property = FindSetting(Type, Setting.Name, Hidden);
			if (!Property)
			{
				OutError = FString::Printf(TEXT("%s has no setting '%s'; its settings are %s."), *Type.GetName(), *Setting.Name, *SettingNames(Type, Hidden));
				return false;
			}
			const FString Before = ExportValue(*Property, Container, Owner);
			const TCHAR* Rest = Property->ImportText_InContainer(*Setting.Value, Container, Owner, PPF_None);
			if (!Rest || !FString(Rest).TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is not a value for %s (%s); it reads %s."), *Setting.Value, *Property->GetName(), *Property->GetCPPType(), *Before);
				return false;
			}
		}
		return true;
	}

	TArray<FString> ExportSettings(const UStruct& Type, const void* Container, UObject* Owner, const TSet<FName>& Hidden)
	{
		TArray<FString> Lines;
		for (TFieldIterator<FProperty> It(&Type); It; ++It)
		{
			if (IsSetting(**It) && !Hidden.Contains(It->GetFName()))
			{
				Lines.Add(FString::Printf(TEXT("%s = %s"), *It->GetName(), *ExportValue(**It, Container, Owner)));
			}
		}
		return Lines;
	}

	const TSet<FName>& SchemaHidden()
	{
		static const TSet<FName> Hidden = {TEXT("Channels")};
		return Hidden;
	}

	const TSet<FName>& DatabaseHidden()
	{
		static const TSet<FName> Hidden = {TEXT("Schema")};
		return Hidden;
	}

	const TSet<FName>& EntryHidden()
	{
		static const TSet<FName> Hidden = {TEXT("AnimAsset")};
		return Hidden;
	}

	const TSet<FName>& NothingHidden()
	{
		static const TSet<FName> Hidden;
		return Hidden;
	}

	/**
	 * The schema's own channel list, through the Channels property the schema editor edits: the engine keeps the array private,
	 * and GetChannels() returns the finalized list, injected channels and all.
	 */
	TArray<TObjectPtr<UPoseSearchFeatureChannel>>* AuthoredChannels(const UPoseSearchSchema& Schema)
	{
		const FArrayProperty* Property = FindFProperty<FArrayProperty>(UPoseSearchSchema::StaticClass(), TEXT("Channels"));
		const FObjectPropertyBase* Inner = Property ? CastField<FObjectPropertyBase>(Property->Inner) : nullptr;
		return Inner && Inner->PropertyClass->IsChildOf(UPoseSearchFeatureChannel::StaticClass())
			? Property->ContainerPtrToValuePtr<TArray<TObjectPtr<UPoseSearchFeatureChannel>>>(const_cast<UPoseSearchSchema*>(&Schema)) : nullptr;
	}

	/** Bones the schema's channels sample that its skeletons lack; a pose built from a missing bone matches nothing. */
	TArray<FString> MissingBones(const UPoseSearchSchema& Schema)
	{
		TArray<FString> Notes;
		for (const FPoseSearchRoledSkeleton& Roled : Schema.GetRoledSkeletons())
		{
			if (!Roled.Skeleton)
			{
				Notes.Add(TEXT("A skeleton slot of the schema is empty."));
				continue;
			}
			TArray<FString> Missing;
			for (const FBoneReference& Bone : Roled.BoneReferences)
			{
				if (!Bone.BoneName.IsNone() && Roled.Skeleton->GetReferenceSkeleton().FindBoneIndex(Bone.BoneName) == INDEX_NONE)
				{
					Missing.AddUnique(Bone.BoneName.ToString());
				}
			}
			if (!Missing.IsEmpty())
			{
				Notes.Add(FString::Printf(TEXT("%s has no bone %s that a channel samples; set the channel's bones with schema.set_channel."),
					*Roled.Skeleton->GetName(), *FString::Join(Missing, TEXT(", "))));
			}
		}
		return Notes;
	}

	FHyperAIPoseSearchAssetInfo DescribeSchema(UPoseSearchSchema& Schema)
	{
		FHyperAIPoseSearchAssetInfo Info;
		Info.Path = Schema.GetPathName();
		Info.Kind = TEXT("schema");
		for (const FPoseSearchRoledSkeleton& Roled : Schema.GetRoledSkeletons())
		{
			Info.Skeletons.Add(FString::Printf(TEXT("%s%s%s"), *NameOf(Roled.Skeleton),
				Roled.MirrorDataTable ? *(TEXT(", mirrored by ") + Roled.MirrorDataTable->GetPathName()) : TEXT(""),
				Roled.Role.IsNone() ? TEXT("") : *(TEXT(", role ") + Roled.Role.ToString())));
		}
		const TArray<TObjectPtr<UPoseSearchFeatureChannel>>* Channels = AuthoredChannels(Schema);
		for (int32 Index = 0; Channels && Index < Channels->Num(); ++Index)
		{
			UPoseSearchFeatureChannel* Channel = (*Channels)[Index];
			Info.Channels.Add(Channel ? FString::Printf(TEXT("%d %s: %s"), Index, *ChannelName(*Channel->GetClass()),
				*FString::Join(ExportSettings(*Channel->GetClass(), Channel, Channel, NothingHidden()), TEXT("; "))) : FString::Printf(TEXT("%d (empty)"), Index));
		}
		Info.Settings = ExportSettings(*UPoseSearchSchema::StaticClass(), &Schema, &Schema, SchemaHidden());
		Info.Notes = MissingBones(Schema);
		return Info;
	}

	/**
	 * The search index, through the engine's index manager (the only safe way to read it): built with its pose count, building,
	 * or failed. bBuild builds it now and waits; otherwise an index the editor has not built yet starts building, as it does when
	 * the database is opened.
	 */
	FString IndexState(const UPoseSearchDatabase& Database, const bool bBuild)
	{
		using namespace UE::PoseSearch;
		if (!Database.Schema)
		{
			return TEXT("none: the database has no schema");
		}
		const EAsyncBuildIndexResult Result = FAsyncPoseSearchDatabasesManagement::RequestAsyncBuildIndex(&Database,
			bBuild ? ERequestAsyncBuildFlag::NewRequest | ERequestAsyncBuildFlag::WaitForCompletion : ERequestAsyncBuildFlag::ContinueRequest);
		if (Result == EAsyncBuildIndexResult::Success)
		{
			return FString::Printf(TEXT("built: %d poses from %d animation entries"), Database.GetSearchIndex().GetNumPoses(), Database.GetSearchIndex().Assets.Num());
		}
		return Result == EAsyncBuildIndexResult::InProgress ? FString(TEXT("building; inspect again, or database.build waits for it"))
			: FString(TEXT("failed: check the schema's bones and the animations' skeletons with hyper_pose_search_inspect"));
	}

	FHyperAIPoseSearchAssetInfo DescribeDatabase(UPoseSearchDatabase& Database, const bool bIndex)
	{
		FHyperAIPoseSearchAssetInfo Info;
		Info.Path = Database.GetPathName();
		Info.Kind = TEXT("database");
		Info.Schema = NameOf(Database.Schema);
		for (int32 Index = 0; Index < Database.GetNumAnimationAssets(); ++Index)
		{
			const FPoseSearchDatabaseAnimationAsset* Entry = Database.GetDatabaseAnimationAsset(Index);
			if (!Entry)
			{
				continue;
			}
			Info.Animations.Add(FString::Printf(TEXT("%d %s: %s%s"), Index, *NameOf(Entry->GetAnimationAsset()),
				*FString::Join(ExportSettings(*FPoseSearchDatabaseAnimationAsset::StaticStruct(), Entry, &Database, EntryHidden()), TEXT("; ")),
				Entry->IsSynchronizedWithExternalDependency() ? TEXT(" (added by a branch-in notify)") : TEXT("")));
			if (Database.Schema && Entry->GetAnimationAsset() && !Entry->IsSkeletonCompatible(Database.Schema))
			{
				Info.Notes.Add(FString::Printf(TEXT("%s is not of the schema's skeleton; the index leaves it out."), *Entry->GetAnimationAsset()->GetName()));
			}
		}
		if (!Database.Schema)
		{
			Info.Notes.Add(TEXT("The database has no schema; give it one with database.set Schema."));
		}
		Info.Settings = ExportSettings(*UPoseSearchDatabase::StaticClass(), &Database, &Database, DatabaseHidden());
		if (bIndex)
		{
			Info.Index = IndexState(Database, /*bBuild=*/false);
		}
		return Info;
	}

	FString Summarize(const FHyperAIPoseSearchAssetInfo& Info)
	{
		if (Info.Kind == TEXT("schema"))
		{
			TArray<FString> Channels;
			for (const FString& Channel : Info.Channels)
			{
				FString Head;
				Channel.Split(TEXT(":"), &Head, nullptr);
				Channels.Add(Head.IsEmpty() ? Channel : Head);
			}
			return FString::Printf(TEXT("schema %s: skeleton %s; channels %s%s"), *Info.Path, *FString::Join(Info.Skeletons, TEXT(" + ")),
				Channels.IsEmpty() ? TEXT("none") : *FString::Join(Channels, TEXT(", ")),
				Info.Notes.IsEmpty() ? TEXT("") : *(TEXT("; ") + FString::Join(Info.Notes, TEXT(" "))));
		}
		return FString::Printf(TEXT("database %s: schema %s; %d animation entr%s%s"), *Info.Path, *Info.Schema, Info.Animations.Num(),
			Info.Animations.Num() == 1 ? TEXT("y") : TEXT("ies"), Info.Notes.IsEmpty() ? TEXT("") : *(TEXT("; ") + FString::Join(Info.Notes, TEXT(" "))));
	}

	/** Scratch instances, never saved, that settings are checked against before the real assets change. */
	struct FScratch
	{
		UPoseSearchSchema* Schema = nullptr;
		UPoseSearchDatabase* Database = nullptr;
		TMap<UClass*, UPoseSearchFeatureChannel*> Channels;

		UPoseSearchSchema& GetSchema()
		{
			if (!Schema)
			{
				Schema = NewObject<UPoseSearchSchema>(GetTransientPackage(), NAME_None, RF_Transient);
			}
			return *Schema;
		}

		UPoseSearchDatabase& GetDatabase()
		{
			if (!Database)
			{
				Database = NewObject<UPoseSearchDatabase>(GetTransientPackage(), NAME_None, RF_Transient);
			}
			return *Database;
		}

		UPoseSearchFeatureChannel& GetChannel(UClass& Class)
		{
			UPoseSearchFeatureChannel*& Channel = Channels.FindOrAdd(&Class);
			if (!Channel)
			{
				Channel = NewObject<UPoseSearchFeatureChannel>(GetTransientPackage(), &Class, NAME_None, RF_Transient);
			}
			return *Channel;
		}
	};

	/** What a plan knows of each schema and database it names, as on disk or as the plan's earlier ops leave it. */
	struct FPlanned
	{
		bool bDatabase = false;
		/** A schema's skeleton, or a database's schema's (for animation checks). */
		const USkeleton* Skeleton = nullptr;
		/** A schema's channel types, in order. */
		TArray<UClass*> Channels;
		/** A database's schema (as a key) and its animations (lower-case paths), and those a branch-in notify added. */
		FString Schema;
		TArray<FString> Animations;
		TSet<FString> Synchronized;
	};

	class FPlanCheck
	{
	public:
		/** The asset as the plan has it so far, read from disk the first time; null, with why, when it is not the kind the op needs. */
		FPlanned* Find(const FString& Path, const bool bDatabase, FString& OutWhy)
		{
			const TCHAR* Wanted = bDatabase ? TEXT("a Pose Search database") : TEXT("a Pose Search schema");
			if (FPlanned* Planned = Assets.Find(KeyOf(Path)))
			{
				if (Planned->bDatabase != bDatabase)
				{
					OutWhy = FString::Printf(TEXT("%s is not %s."), *Path, Wanted);
					return nullptr;
				}
				return Planned;
			}
			FPlanned Asset;
			Asset.bDatabase = bDatabase;
			if (bDatabase)
			{
				const UPoseSearchDatabase* Database = Load<UPoseSearchDatabase>(Path);
				if (!Database)
				{
					OutWhy = FString::Printf(TEXT("'%s' is not %s, and the plan does not make it."), *Path, Wanted);
					return nullptr;
				}
				if (Database->Schema)
				{
					Asset.Schema = KeyOf(Database->Schema->GetPathName());
					Asset.Skeleton = Database->Schema->GetSkeleton(Database->Schema->GetDefaultRole());
				}
				for (int32 Index = 0; Index < Database->GetNumAnimationAssets(); ++Index)
				{
					const FPoseSearchDatabaseAnimationAsset* Entry = Database->GetDatabaseAnimationAsset(Index);
					const FString Animation = Entry && Entry->GetAnimationAsset() ? Entry->GetAnimationAsset()->GetPathName().ToLower() : FString();
					Asset.Animations.Add(Animation);
					if (Entry && Entry->IsSynchronizedWithExternalDependency())
					{
						Asset.Synchronized.Add(Animation);
					}
				}
			}
			else
			{
				const UPoseSearchSchema* Schema = Load<UPoseSearchSchema>(Path);
				if (!Schema)
				{
					OutWhy = FString::Printf(TEXT("'%s' is not %s, and the plan does not make it."), *Path, Wanted);
					return nullptr;
				}
				Asset.Skeleton = Schema->GetSkeleton(Schema->GetDefaultRole());
				const TArray<TObjectPtr<UPoseSearchFeatureChannel>>* Channels = AuthoredChannels(*Schema);
				for (int32 Index = 0; Channels && Index < Channels->Num(); ++Index)
				{
					const UPoseSearchFeatureChannel* Channel = (*Channels)[Index];
					Asset.Channels.Add(Channel ? Channel->GetClass() : nullptr);
				}
			}
			return &Assets.Add(KeyOf(Path), MoveTemp(Asset));
		}

		bool Contains(const FString& Path) const
		{
			return Assets.Contains(KeyOf(Path));
		}

		void Add(const FString& Path, FPlanned&& Asset)
		{
			Assets.Add(KeyOf(Path), MoveTemp(Asset));
		}

	private:
		TMap<FString, FPlanned> Assets;
	};

	/** Every op checked before anything changes: its kind, its assets, channels, settings and animations. */
	bool Check(const FHyperAIPoseSearchPlanRequest& Request, FString& OutError)
	{
		if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
			return false;
		}
		FPlanCheck Plan;
		FScratch Scratch;
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAIPoseSearchOp& Op = Request.Operations[Index];
			const FString Kind = KindOf(Op);
			FString Why;
			auto Fail = [&OutError, &Op, Index](const FString& Message)
			{
				OutError = FString::Printf(TEXT("Operation %d (%s): %s Nothing was changed."), Index, *Op.Kind, *Message);
				return false;
			};
			if ((Kind == TEXT("schema.create") || Kind == TEXT("database.create")) && (!IsNewAsset(Op.Asset, Why) || Plan.Contains(Op.Asset)))
			{
				return Fail(Why.IsEmpty() ? FString::Printf(TEXT("The plan makes %s twice."), *Op.Asset) : Why);
			}
			if (Kind == TEXT("schema.create"))
			{
				FPlanned Schema;
				Schema.Skeleton = Load<USkeleton>(Op.Skeleton);
				if (!Schema.Skeleton)
				{
					return Fail(FString::Printf(TEXT("'%s' is not a skeleton."), *Op.Skeleton));
				}
				if (!Op.MirrorTable.TrimStartAndEnd().IsEmpty() && !Load<UMirrorDataTable>(Op.MirrorTable))
				{
					return Fail(FString::Printf(TEXT("'%s' is not a mirror data table."), *Op.MirrorTable));
				}
				if (!ImportSettings(*UPoseSearchSchema::StaticClass(), &Scratch.GetSchema(), &Scratch.GetSchema(), Op.Settings, SchemaHidden(), Why))
				{
					return Fail(Why);
				}
				if (Op.bDefaultChannels)
				{
					Schema.Channels = {ChannelClass(TEXT("Trajectory")), ChannelClass(TEXT("Pose"))};
				}
				Plan.Add(Op.Asset, MoveTemp(Schema));
				continue;
			}
			if (Kind.StartsWith(TEXT("schema.")))
			{
				FPlanned* Schema = Plan.Find(Op.Asset, false, Why);
				if (!Schema)
				{
					return Fail(Why);
				}
				if (Kind == TEXT("schema.set"))
				{
					if (Op.Settings.IsEmpty() || !ImportSettings(*UPoseSearchSchema::StaticClass(), &Scratch.GetSchema(), &Scratch.GetSchema(), Op.Settings, SchemaHidden(), Why))
					{
						return Fail(Why.IsEmpty() ? FString(TEXT("Give the Settings to change.")) : Why);
					}
				}
				else if (Kind == TEXT("schema.add_channel"))
				{
					UClass* Class = ChannelClass(Op.Channel);
					if (!Class)
					{
						return Fail(FString::Printf(TEXT("'%s' is not a channel type; the types are %s."), *Op.Channel, *ChannelTypes()));
					}
					UPoseSearchFeatureChannel& Channel = Scratch.GetChannel(*Class);
					if (!ImportSettings(*Class, &Channel, &Channel, Op.Settings, NothingHidden(), Why))
					{
						return Fail(Why);
					}
					Schema->Channels.Add(Class);
				}
				else if (Kind == TEXT("schema.set_channel") || Kind == TEXT("schema.remove_channel"))
				{
					if (!Schema->Channels.IsValidIndex(Op.ChannelIndex) || !Schema->Channels[Op.ChannelIndex])
					{
						return Fail(FString::Printf(TEXT("The schema has no channel %d; it has %d."), Op.ChannelIndex, Schema->Channels.Num()));
					}
					if (Kind == TEXT("schema.remove_channel"))
					{
						Schema->Channels.RemoveAt(Op.ChannelIndex);
						continue;
					}
					UPoseSearchFeatureChannel& Channel = Scratch.GetChannel(*Schema->Channels[Op.ChannelIndex]);
					if (Op.Settings.IsEmpty() || !ImportSettings(*Channel.GetClass(), &Channel, &Channel, Op.Settings, NothingHidden(), Why))
					{
						return Fail(Why.IsEmpty() ? FString(TEXT("Give the Settings to change.")) : Why);
					}
				}
				else
				{
					return Fail(TEXT("Unknown schema op; see hyper_pose_search_apply_plan's description."));
				}
				continue;
			}
			if (Kind == TEXT("database.create"))
			{
				FPlanned Database;
				Database.bDatabase = true;
				const FPlanned* Schema = Plan.Find(Op.Schema, false, Why);
				if (!Schema)
				{
					return Fail(Why);
				}
				Database.Schema = KeyOf(Op.Schema);
				Database.Skeleton = Schema->Skeleton;
				if (!ImportSettings(*UPoseSearchDatabase::StaticClass(), &Scratch.GetDatabase(), &Scratch.GetDatabase(), Op.Settings, DatabaseHidden(), Why))
				{
					return Fail(Why);
				}
				Plan.Add(Op.Asset, MoveTemp(Database));
				continue;
			}
			if (Kind.StartsWith(TEXT("database.")))
			{
				if (!Plan.Find(Op.Asset, true, Why))
				{
					return Fail(Why);
				}
				if (Kind == TEXT("database.set"))
				{
					const FPlanned* NewSchema = Op.Schema.TrimStartAndEnd().IsEmpty() ? nullptr : Plan.Find(Op.Schema, false, Why);
					if (!Why.IsEmpty())
					{
						return Fail(Why);
					}
					const USkeleton* NewSkeleton = NewSchema ? NewSchema->Skeleton : nullptr;
					FPlanned* Database = Plan.Find(Op.Asset, true, Why);
					if (NewSchema)
					{
						Database->Schema = KeyOf(Op.Schema);
						Database->Skeleton = NewSkeleton;
					}
					if ((Op.Settings.IsEmpty() && !NewSchema)
						|| !ImportSettings(*UPoseSearchDatabase::StaticClass(), &Scratch.GetDatabase(), &Scratch.GetDatabase(), Op.Settings, DatabaseHidden(), Why))
					{
						return Fail(Why.IsEmpty() ? FString(TEXT("Give a Schema or the Settings to change.")) : Why);
					}
					continue;
				}
				FPlanned* Database = Plan.Find(Op.Asset, true, Why);
				if (Kind == TEXT("database.build"))
				{
					if (Database->Schema.IsEmpty())
					{
						return Fail(TEXT("The database has no schema to index with."));
					}
					continue;
				}
				if (Kind != TEXT("database.add_animations") && Kind != TEXT("database.set_animations") && Kind != TEXT("database.remove_animations"))
				{
					return Fail(TEXT("Unknown database op; see hyper_pose_search_apply_plan's description."));
				}
				if (Op.Animations.IsEmpty() || Op.Animations.Num() > C::MaxAnimations)
				{
					return Fail(FString::Printf(TEXT("Give 1 to %d Animations."), C::MaxAnimations));
				}
				FPoseSearchDatabaseAnimationAsset Entry;
				if (Kind != TEXT("database.remove_animations")
					&& !ImportSettings(*FPoseSearchDatabaseAnimationAsset::StaticStruct(), &Entry, &Scratch.GetDatabase(), Op.Settings, EntryHidden(), Why))
				{
					return Fail(Why);
				}
				if (Kind == TEXT("database.set_animations") && Op.Settings.IsEmpty())
				{
					return Fail(TEXT("Give the Settings to change."));
				}
				for (const FString& Path : Op.Animations)
				{
					const UObject* Animation = LoadAnimation(Path);
					if (!Animation)
					{
						return Fail(FString::Printf(TEXT("'%s' is not an animation sequence, composite, montage or blend space."), *Path));
					}
					const FString Key = Animation->GetPathName().ToLower();
					if (Kind == TEXT("database.add_animations"))
					{
						if (Database->Skeleton && !IsSkeletonFor(Database->Skeleton, SkeletonOf(Animation)))
						{
							return Fail(FString::Printf(TEXT("%s is of skeleton %s, not the schema's %s."), *Animation->GetName(), *NameOf(SkeletonOf(Animation)),
								*NameOf(Database->Skeleton)));
						}
						Database->Animations.Add(Key);
						continue;
					}
					if (!Database->Animations.Contains(Key))
					{
						return Fail(FString::Printf(TEXT("The database has no %s."), *Animation->GetName()));
					}
					if (Kind == TEXT("database.remove_animations"))
					{
						if (Database->Synchronized.Contains(Key))
						{
							return Fail(FString::Printf(TEXT("A branch-in notify in %s adds it; remove the notify instead."), *Animation->GetName()));
						}
						Database->Animations.Remove(Key);
					}
				}
				continue;
			}
			return Fail(TEXT("Unknown op; see hyper_pose_search_apply_plan's description."));
		}
		return true;
	}

	/** One schema or database saved to its package. */
	bool Save(UObject& Asset, FString& OutError)
	{
		UPackage* Package = Asset.GetOutermost();
		if (!FHyperAIStudioSourceControl::MakeEditable({Package->GetName()}, OutError))
		{
			return false;
		}
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		if (!UPackage::SavePackage(Package, &Asset, *File, Args))
		{
			OutError = FString::Printf(TEXT("%s could not be saved."), *Package->GetName());
			return false;
		}
		return true;
	}

	/** One schema or database op, inside the plan's undo step. */
	bool ApplyEdit(const FHyperAIPoseSearchOp& Op, TMap<FString, UObject*>& Made, TArray<UObject*>& Touched, FString& OutError)
	{
		const FString Kind = KindOf(Op);
		auto Resolve = [&Made](const FString& Path) -> UObject*
		{
			UObject* const* Planned = Made.Find(KeyOf(Path));
			return Planned ? *Planned : Load<UObject>(Path);
		};
		if (Kind == TEXT("schema.create") || Kind == TEXT("database.create"))
		{
			const FString Package = PackageOf(Op.Asset);
			UPackage* Outer = CreatePackage(*Package);
			const FName Name(*FPackageName::GetShortName(Package));
			UObject* Asset = nullptr;
			if (Kind == TEXT("schema.create"))
			{
				UPoseSearchSchema* Schema = NewObject<UPoseSearchSchema>(Outer, Name, RF_Public | RF_Standalone | RF_Transactional);
				Schema->AddSkeleton(Load<USkeleton>(Op.Skeleton), Load<UMirrorDataTable>(Op.MirrorTable));
				if (Op.bDefaultChannels)
				{
					Schema->AddDefaultChannels();
				}
				if (!ImportSettings(*UPoseSearchSchema::StaticClass(), Schema, Schema, Op.Settings, SchemaHidden(), OutError))
				{
					return false;
				}
				Schema->PostEditChange();
				Asset = Schema;
			}
			else
			{
				UPoseSearchDatabase* Database = NewObject<UPoseSearchDatabase>(Outer, Name, RF_Public | RF_Standalone | RF_Transactional);
				Database->Schema = Cast<UPoseSearchSchema>(Resolve(Op.Schema));
				if (!Database->Schema)
				{
					OutError = FString::Printf(TEXT("%s is not a Pose Search schema."), *Op.Schema);
					return false;
				}
				if (!ImportSettings(*UPoseSearchDatabase::StaticClass(), Database, Database, Op.Settings, DatabaseHidden(), OutError))
				{
					return false;
				}
				Database->PostEditChange();
				Asset = Database;
			}
			FAssetRegistryModule::AssetCreated(Asset);
			Outer->MarkPackageDirty();
			Made.Add(KeyOf(Package), Asset);
			Touched.AddUnique(Asset);
			return true;
		}
		if (Kind.StartsWith(TEXT("schema.")))
		{
			UPoseSearchSchema* Schema = Cast<UPoseSearchSchema>(Resolve(Op.Asset));
			if (!Schema)
			{
				OutError = FString::Printf(TEXT("%s is not a Pose Search schema."), *Op.Asset);
				return false;
			}
			Touched.AddUnique(Schema);
			Schema->Modify();
			if (Kind == TEXT("schema.set"))
			{
				if (!ImportSettings(*UPoseSearchSchema::StaticClass(), Schema, Schema, Op.Settings, SchemaHidden(), OutError))
				{
					return false;
				}
			}
			else if (Kind == TEXT("schema.add_channel"))
			{
				UClass* Class = ChannelClass(Op.Channel);
				UPoseSearchFeatureChannel* Channel = Class ? NewObject<UPoseSearchFeatureChannel>(Schema, Class, NAME_None, RF_Transactional) : nullptr;
				if (!Channel || !ImportSettings(*Class, Channel, Channel, Op.Settings, NothingHidden(), OutError))
				{
					OutError = OutError.IsEmpty() ? FString::Printf(TEXT("'%s' is not a channel type."), *Op.Channel) : OutError;
					return false;
				}
				Schema->AddChannel(Channel);
			}
			else if (TArray<TObjectPtr<UPoseSearchFeatureChannel>>* Channels = AuthoredChannels(*Schema);
				!Channels || !Channels->IsValidIndex(Op.ChannelIndex) || !(*Channels)[Op.ChannelIndex])
			{
				OutError = FString::Printf(TEXT("The schema has no channel %d."), Op.ChannelIndex);
				return false;
			}
			else if (Kind == TEXT("schema.set_channel"))
			{
				UPoseSearchFeatureChannel* Channel = (*Channels)[Op.ChannelIndex];
				Channel->Modify();
				if (!ImportSettings(*Channel->GetClass(), Channel, Channel, Op.Settings, NothingHidden(), OutError))
				{
					return false;
				}
				Channel->PostEditChange();
			}
			else
			{
				Channels->RemoveAt(Op.ChannelIndex);
			}
			Schema->PostEditChange();
			return true;
		}
		UPoseSearchDatabase* Database = Cast<UPoseSearchDatabase>(Resolve(Op.Asset));
		if (!Database)
		{
			OutError = FString::Printf(TEXT("%s is not a Pose Search database."), *Op.Asset);
			return false;
		}
		Touched.AddUnique(Database);
		Database->Modify();
		if (Kind == TEXT("database.set"))
		{
			if (!Op.Schema.TrimStartAndEnd().IsEmpty())
			{
				Database->Schema = Cast<UPoseSearchSchema>(Resolve(Op.Schema));
				if (!Database->Schema)
				{
					OutError = FString::Printf(TEXT("%s is not a Pose Search schema."), *Op.Schema);
					return false;
				}
			}
			if (!ImportSettings(*UPoseSearchDatabase::StaticClass(), Database, Database, Op.Settings, DatabaseHidden(), OutError))
			{
				return false;
			}
		}
		else
		{
			for (const FString& Path : Op.Animations)
			{
				UObject* Animation = LoadAnimation(Path);
				if (!Animation)
				{
					OutError = FString::Printf(TEXT("'%s' could not be loaded."), *Path);
					return false;
				}
				if (Kind == TEXT("database.add_animations"))
				{
					FPoseSearchDatabaseAnimationAsset Entry;
					Entry.AnimAsset = Animation;
					if (!ImportSettings(*FPoseSearchDatabaseAnimationAsset::StaticStruct(), &Entry, Database, Op.Settings, EntryHidden(), OutError))
					{
						return false;
					}
					Database->AddAnimationAsset(Entry);
					continue;
				}
				for (int32 Index = Database->GetNumAnimationAssets() - 1; Index >= 0; --Index)
				{
					if (Database->GetAnimationAsset(Index) != Animation)
					{
						continue;
					}
					if (Kind == TEXT("database.remove_animations"))
					{
						Database->RemoveAnimationAssetAt(Index);
					}
					else if (FPoseSearchDatabaseAnimationAsset* Entry = Database->GetMutableDatabaseAnimationAsset(Index))
					{
						if (!ImportSettings(*FPoseSearchDatabaseAnimationAsset::StaticStruct(), Entry, Database, Op.Settings, EntryHidden(), OutError))
						{
							return false;
						}
					}
				}
			}
		}
		Database->PostEditChange();
		return true;
	}

	/** The plan for real: every schema and database edit as one undo step, saved, then the requested indexes built. */
	FHyperAIPoseSearchPlanReport Execute(const FHyperAIPoseSearchPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIPoseSearchPlanReport Report;
		Report.OperationId = OperationId;
		TMap<FString, UObject*> Made;
		TArray<UObject*> Touched;
		{
			const FScopedTransaction Transaction(LOCTEXT("PoseSearchPlan", "HyperAI: Edit Motion Matching Data"));
			for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
			{
				const FHyperAIPoseSearchOp& Op = Request.Operations[Index];
				if (KindOf(Op) == TEXT("database.build"))
				{
					continue;
				}
				FString Error;
				if (!ApplyEdit(Op, Made, Touched, Error))
				{
					Report.Status = TEXT("operation_failed");
					Report.Diagnostic = FString::Printf(TEXT("Operation %d (%s): %s%s"), Index, *Op.Kind, *Error,
						Report.Applied > 0 ? TEXT(" The operations before it stay applied, unsaved; Edit > Undo takes the plan back.") : TEXT(""));
					return Report;
				}
				++Report.Applied;
			}
		}
		for (UObject* Asset : Touched)
		{
			if (UPoseSearchSchema* Schema = Cast<UPoseSearchSchema>(Asset))
			{
				const FHyperAIPoseSearchAssetInfo Info = DescribeSchema(*Schema);
				Report.Made.Add(Summarize(Info));
				Report.Notes.Append(Info.Notes);
			}
			else if (UPoseSearchDatabase* Database = Cast<UPoseSearchDatabase>(Asset))
			{
				const FHyperAIPoseSearchAssetInfo Info = DescribeDatabase(*Database, /*bIndex=*/false);
				Report.Made.Add(Summarize(Info));
				Report.Notes.Append(Info.Notes);
			}
			FString Error;
			if (!Save(*Asset, Error))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = Error;
				return Report;
			}
			Report.Saved.Add(Asset->GetPathName());
		}
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAIPoseSearchOp& Op = Request.Operations[Index];
			if (KindOf(Op) != TEXT("database.build"))
			{
				continue;
			}
			UObject* const* Planned = Made.Find(KeyOf(Op.Asset));
			const UPoseSearchDatabase* Database = Cast<UPoseSearchDatabase>(Planned ? *Planned : Load<UObject>(Op.Asset));
			const FString State = Database ? IndexState(*Database, /*bBuild=*/true) : FString(TEXT("failed: not a Pose Search database"));
			if (!State.StartsWith(TEXT("built")))
			{
				Report.Status = TEXT("build_failed");
				Report.Diagnostic = FString::Printf(TEXT("Operation %d (%s): the index %s. The edits before it are applied and saved."), Index, *Op.Kind, *State);
				return Report;
			}
			Report.Built.Add(FString::Printf(TEXT("%s: %s"), *Database->GetName(), *State));
			++Report.Applied;
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) applied and saved%s."), Report.Applied,
			Report.Built.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; %d index(es) built"), Report.Built.Num()));
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAIPoseSearchPlanReport& Done)
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

FHyperAIPoseSearchInspectReport FHyperAIStudioPoseSearchContracts::Inspect(const FHyperAIPoseSearchInspectRequest& Request)
{
	using namespace HyperAIStudio::MotionMatching::Private;
	FHyperAIPoseSearchInspectReport Report;
	if (Request.Paths.IsEmpty() || Request.Paths.Num() > 64)
	{
		Report.Diagnostic = TEXT("Give 1 to 64 Paths: Pose Search schemas or databases.");
		return Report;
	}
	for (const FString& Path : Request.Paths)
	{
		UObject* Asset = Load<UObject>(Path);
		if (UPoseSearchSchema* Schema = Cast<UPoseSearchSchema>(Asset))
		{
			Report.Assets.Add(DescribeSchema(*Schema));
		}
		else if (UPoseSearchDatabase* Database = Cast<UPoseSearchDatabase>(Asset))
		{
			Report.Assets.Add(DescribeDatabase(*Database, /*bIndex=*/true));
		}
		else
		{
			Report.Diagnostic = FString::Printf(TEXT("'%s' is not a Pose Search schema or database."), *Path);
			return Report;
		}
	}
	Report.bOk = true;
	return Report;
}

FHyperAIPoseSearchPlanReport FHyperAIStudioPoseSearchContracts::ApplyPlan(const FHyperAIPoseSearchPlanRequest& Request)
{
	using namespace HyperAIStudio::MotionMatching::Private;
	FHyperAIPoseSearchPlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("pose-search-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
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
		Summary.VariantId = TEXT("pose_search.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = Request.Operations[0].Asset;
		Summary.Effects.Add(FString::Printf(TEXT("Edit motion matching data: %d operation(s), one undo step, saved; database.build indexes after saving"),
			Request.Operations.Num()));
		for (int32 Index = 0; Index < Request.Operations.Num() && Index < 24; ++Index)
		{
			const FHyperAIPoseSearchOp& Op = Request.Operations[Index];
			Summary.Effects.Add(FString::Printf(TEXT("| %s %s%s"), *Op.Kind, *Op.Asset,
				Op.Animations.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%d animations)"), Op.Animations.Num())));
			Summary.Touches.AddUnique(Op.Asset);
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
		return Report;
	}
	return Execute(Request, Report.OperationId);
}

FHyperAIPoseSearchInspectReport UHyperAIStudioPoseSearchToolset::hyper_pose_search_inspect(const FHyperAIPoseSearchInspectRequest& Request)
{
	return FHyperAIStudioPoseSearchContracts::Inspect(Request);
}

FHyperAIPoseSearchPlanReport UHyperAIStudioPoseSearchToolset::hyper_pose_search_apply_plan(const FHyperAIPoseSearchPlanRequest& Request)
{
	return FHyperAIStudioPoseSearchContracts::ApplyPlan(Request);
}

void FHyperAIStudioPoseSearchRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioPoseSearchRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioPoseSearchRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioPoseSearchToolset::StaticClass(),
			FHyperAIStudioPoseSearchContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioPoseSearch, Warning, TEXT("Could not unregister the Pose Search toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioPoseSearchRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioPoseSearchContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioPoseSearch, Verbose, TEXT("The Pose Search source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioPoseSearchToolset::StaticClass(),
		FHyperAIStudioPoseSearchContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioPoseSearch, Error, TEXT("Pose Search cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
