// Games by Hyper 2026.

#include "HyperAIStudioWorldPartitionToolset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "DataLayer/DataLayerEditorSubsystem.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSourceControl.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Package.h"
#include "WorldPartition/DataLayer/DataLayerAsset.h"
#include "WorldPartition/DataLayer/DataLayerInstance.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/HLOD/HLODActor.h"
#include "WorldPartition/HLOD/HLODBuilder.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionEditorLoaderAdapter.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioWorldPartition, Log, All);

#define LOCTEXT_NAMESPACE "HyperAIStudioWorldPartition"

FString FHyperAIStudioWorldPartitionContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioWorldPartition.HyperAIStudioWorldPartitionToolset");
}

TArray<FString> FHyperAIStudioWorldPartitionContracts::GetToolNames()
{
	return {InspectToolName, PlanToolName};
}

bool FHyperAIStudioWorldPartitionContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

namespace HyperAIStudio::WorldPartitionTools::Private
{
	using C = FHyperAIStudioWorldPartitionContracts;

	/** Regions this tool loads carry this at the start of their label, so region.unload can find them. */
	const TCHAR* const RegionPrefix = TEXT("HyperAI ");
	const TCHAR* const DefaultDataLayerFolder = TEXT("/Game/DataLayers/");
	/** Editor regions reach this far up and down, cm. */
	constexpr double RegionReach = 10000000.0;

	FString KindOf(const FHyperAIWorldPartitionOp& Op)
	{
		return Op.Kind.TrimStartAndEnd().ToLower();
	}

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	bool IsNone(const FString& Value)
	{
		return Value.TrimStartAndEnd().Equals(TEXT("none"), ESearchCase::IgnoreCase);
	}

	template <typename T>
	T* Load(const FString& Path)
	{
		const FString Trimmed = Path.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !Trimmed.StartsWith(TEXT("/"))) return nullptr;
		// The load makes an FName of each part, and the package it looked for: past 1023 characters, or with a double
		// slash, either stops the editor.
		if (!FHyperAIStudioNames::IsObjectPath(Trimmed, TEXT("Path"))) return nullptr;
		const FString ObjectPath = Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
		return LoadObject<T>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** The package of an asset reference: /Game/A/B.B and /Game/A/B name the same asset. */
	FString PackageOf(const FString& Reference)
	{
		FString Path = Reference.TrimStartAndEnd();
		int32 Dot = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), Dot)) Path.LeftInline(Dot);
		return Path;
	}

	bool IsNewAssetPath(const FString& Path)
	{
		return Path.StartsWith(TEXT("/Game/")) && FHyperAIStudioNames::IsPackagePath(Path, nullptr);
	}

	bool IsIdentifier(const FString& Name)
	{
		if (Name.IsEmpty() || Name.Len() > 64 || !(FChar::IsAlpha(Name[0]) || Name[0] == TEXT('_'))) return false;
		for (const TCHAR Char : Name)
		{
			if (!FChar::IsAlnum(Char) && Char != TEXT('_')) return false;
		}
		return true;
	}

	/** true, false, or unset for empty; false with why for anything else. */
	bool ParseFlag(const FString& Text, const TCHAR* Field, TOptional<bool>& Out, FString& OutError)
	{
		const FString Value = Text.TrimStartAndEnd().ToLower();
		if (Value.IsEmpty()) return true;
		if (Value == TEXT("true") || Value == TEXT("yes") || Value == TEXT("1")) Out = true;
		else if (Value == TEXT("false") || Value == TEXT("no") || Value == TEXT("0")) Out = false;
		else
		{
			OutError = FString::Printf(TEXT("%s is true or false, not '%s'."), Field, *Text);
			return false;
		}
		return true;
	}

	bool ParseRuntimeState(const FString& Text, EDataLayerRuntimeState& Out)
	{
		const FString Value = Text.TrimStartAndEnd().ToLower();
		if (Value == TEXT("unloaded")) Out = EDataLayerRuntimeState::Unloaded;
		else if (Value == TEXT("loaded")) Out = EDataLayerRuntimeState::Loaded;
		else if (Value == TEXT("activated") || Value == TEXT("active")) Out = EDataLayerRuntimeState::Activated;
		else return false;
		return true;
	}

	FString RuntimeStateName(const EDataLayerRuntimeState State)
	{
		switch (State)
		{
		case EDataLayerRuntimeState::Unloaded: return TEXT("Unloaded");
		case EDataLayerRuntimeState::Loaded: return TEXT("Loaded");
		case EDataLayerRuntimeState::Activated: return TEXT("Activated");
		default: return TEXT("?");
		}
	}

	bool IsSetting(const FProperty& Property)
	{
		return Property.HasAnyPropertyFlags(CPF_Edit) && !Property.HasAnyPropertyFlags(CPF_EditConst | CPF_Deprecated | CPF_DisableEditOnInstance);
	}

	/** An editable property by its name, any case, with or without a bool's leading b. */
	FProperty* FindSetting(const UStruct& Type, const FString& Name)
	{
		const FString Wanted = Name.TrimStartAndEnd();
		for (TFieldIterator<FProperty> It(&Type); It; ++It)
		{
			if (IsSetting(**It) && (It->GetName().Equals(Wanted, ESearchCase::IgnoreCase) || It->GetName().Equals(TEXT("b") + Wanted, ESearchCase::IgnoreCase)))
			{
				return *It;
			}
		}
		return nullptr;
	}

	FString SettingNames(const UStruct& Type)
	{
		TArray<FString> Names;
		for (TFieldIterator<FProperty> It(&Type); It; ++It)
		{
			if (IsSetting(**It)) Names.Add(It->GetName());
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

	TArray<FString> ExportSettings(const UStruct& Type, const void* Container, UObject* Owner)
	{
		TArray<FString> Lines;
		for (TFieldIterator<FProperty> It(&Type); It; ++It)
		{
			if (IsSetting(**It)) Lines.Add(FString::Printf(TEXT("%s = %s"), *It->GetName(), *ExportValue(**It, Container, Owner)));
		}
		return Lines;
	}

	/**
	 * Each setting written into Container, an instance of Type owned by Owner, in Unreal's text form; false, with why, at
	 * the first that does not take. With bNotify the owner hears of each change as the details panel would tell it.
	 */
	bool ApplySettings(const UStruct& Type, void* Container, UObject* Owner, const TArray<FHyperAIWorldPartitionSetting>& Settings, const bool bNotify, FString& OutError)
	{
		for (const FHyperAIWorldPartitionSetting& Setting : Settings)
		{
			FProperty* Property = FindSetting(Type, Setting.Name);
			if (!Property)
			{
				OutError = FString::Printf(TEXT("%s has no setting '%s'; its settings are %s."), *Type.GetName(), *Setting.Name, *SettingNames(Type));
				return false;
			}
			const FString Before = ExportValue(*Property, Container, Owner);
			// The import makes names of the text, and loads an asset it names (making the package when nothing is there).
			if (!FHyperAIStudioNames::CanImportText(*Property, Setting.Value, *Setting.Name, &OutError)) return false;
			if (bNotify && Owner) Owner->PreEditChange(Property);
			const TCHAR* Rest = Property->ImportText_InContainer(*Setting.Value, Container, Owner, PPF_None);
			if (bNotify && Owner)
			{
				FPropertyChangedEvent Event(Property, EPropertyChangeType::ValueSet);
				Owner->PostEditChangeProperty(Event);
			}
			if (!Rest || !FString(Rest).TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is not a value for %s (%s); it reads %s."), *Setting.Value, *Property->GetName(), *Property->GetCPPType(), *Before);
				return false;
			}
		}
		return true;
	}

	UDataLayerEditorSubsystem* DataLayers()
	{
		return UDataLayerEditorSubsystem::Get();
	}

	UDataLayerInstance* FindDataLayer(const FString& Name)
	{
		UDataLayerEditorSubsystem* Subsystem = DataLayers();
		if (!Subsystem) return nullptr;
		for (UDataLayerInstance* Layer : Subsystem->GetAllDataLayers())
		{
			if (Layer && Layer->GetDataLayerShortName().Equals(Name.TrimStartAndEnd(), ESearchCase::IgnoreCase)) return Layer;
		}
		return nullptr;
	}

	/** The loaded actor with the label; with why when none, or several, has it. */
	AActor* FindActor(UWorld& World, const FString& Label, FString& OutError)
	{
		AActor* Found = nullptr;
		int32 Count = 0;
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			if (It->GetActorLabel().Equals(Label.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				Found = *It;
				++Count;
			}
		}
		if (Count == 1) return Found;
		if (Count > 1)
		{
			OutError = FString::Printf(TEXT("%d loaded actors are labelled %s; give each a unique label first."), Count, *Label);
			return nullptr;
		}
		bool bUnloaded = false;
		if (UWorldPartition* Partition = World.GetWorldPartition())
		{
			FWorldPartitionHelpers::ForEachActorDescInstance(Partition, AActor::StaticClass(), [&Label, &bUnloaded](const FWorldPartitionActorDescInstance* Desc)
			{
				bUnloaded = Desc->GetActorLabelString().Equals(Label.TrimStartAndEnd(), ESearchCase::IgnoreCase);
				return !bUnloaded;
			});
		}
		OutError = bUnloaded
			? FString::Printf(TEXT("%s is not loaded in the editor; load its area with region.load in an earlier plan."), *Label)
			: FString::Printf(TEXT("No actor is labelled %s."), *Label);
		return nullptr;
	}

	/** The runtime hash's grid (spatial hash) or partition (hash set) by name, or the first; with its owner to notify. */
	struct FGridRef
	{
		FString Name;
		const UStruct* Type = nullptr;
		void* Container = nullptr;
		UObject* Owner = nullptr;
		FProperty* Array = nullptr;
	};

	TArray<FGridRef> Grids(UWorldPartition& Partition)
	{
		TArray<FGridRef> Out;
		UWorldPartitionRuntimeHash* Hash = Partition.RuntimeHash;
		if (!Hash) return Out;
		if (FArrayProperty* Array = FindFProperty<FArrayProperty>(Hash->GetClass(), TEXT("Grids")))
		{
			const FStructProperty* Inner = CastField<FStructProperty>(Array->Inner);
			const FNameProperty* NameProperty = Inner ? CastField<FNameProperty>(Inner->Struct->FindPropertyByName(TEXT("GridName"))) : nullptr;
			FScriptArrayHelper Helper(Array, Array->ContainerPtrToValuePtr<void>(Hash));
			for (int32 Index = 0; Inner && Index < Helper.Num(); ++Index)
			{
				FGridRef& Ref = Out.AddDefaulted_GetRef();
				Ref.Container = Helper.GetRawPtr(Index);
				Ref.Type = Inner->Struct;
				Ref.Owner = Hash;
				Ref.Array = Array;
				Ref.Name = NameProperty ? NameProperty->GetPropertyValue_InContainer(Ref.Container).ToString() : FString::FromInt(Index);
			}
		}
		else if (FArrayProperty* Partitions = FindFProperty<FArrayProperty>(Hash->GetClass(), TEXT("RuntimePartitions")))
		{
			const FStructProperty* Inner = CastField<FStructProperty>(Partitions->Inner);
			const FNameProperty* NameProperty = Inner ? CastField<FNameProperty>(Inner->Struct->FindPropertyByName(TEXT("Name"))) : nullptr;
			const FObjectPropertyBase* LayerProperty = Inner ? CastField<FObjectPropertyBase>(Inner->Struct->FindPropertyByName(TEXT("MainLayer"))) : nullptr;
			FScriptArrayHelper Helper(Partitions, Partitions->ContainerPtrToValuePtr<void>(Hash));
			for (int32 Index = 0; Inner && LayerProperty && Index < Helper.Num(); ++Index)
			{
				UObject* Layer = LayerProperty->GetObjectPropertyValue_InContainer(Helper.GetRawPtr(Index));
				if (!Layer) continue;
				FGridRef& Ref = Out.AddDefaulted_GetRef();
				Ref.Container = Layer;
				Ref.Type = Layer->GetClass();
				Ref.Owner = Layer;
				Ref.Name = NameProperty ? NameProperty->GetPropertyValue_InContainer(Helper.GetRawPtr(Index)).ToString() : FString::FromInt(Index);
			}
		}
		return Out;
	}

	bool FindGrid(UWorldPartition& Partition, const FString& Name, FGridRef& Out, FString& OutError)
	{
		const TArray<FGridRef> All = Grids(Partition);
		if (All.IsEmpty())
		{
			OutError = TEXT("This level's runtime hash has no grids or partitions to set.");
			return false;
		}
		if (Name.TrimStartAndEnd().IsEmpty())
		{
			Out = All[0];
			return true;
		}
		for (const FGridRef& Ref : All)
		{
			if (Ref.Name.Equals(Name.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				Out = Ref;
				return true;
			}
		}
		TArray<FString> Names;
		for (const FGridRef& Ref : All) Names.Add(Ref.Name);
		OutError = FString::Printf(TEXT("No runtime grid is named %s; the grids are %s."), *Name, *FString::Join(Names, TEXT(", ")));
		return false;
	}

	FString DescribeGrid(const FGridRef& Ref)
	{
		auto Read = [&Ref](const TCHAR* Name) -> FString
		{
			const FProperty* Property = Ref.Type->FindPropertyByName(Name);
			return Property ? ExportValue(*Property, Ref.Container, Ref.Owner) : FString();
		};
		FString Line = FString::Printf(TEXT("%s: CellSize %s cm, LoadingRange %s cm"), *Ref.Name,
			*(Read(TEXT("CellSize")).IsEmpty() ? FString(TEXT("-")) : Read(TEXT("CellSize"))), *(Read(TEXT("LoadingRange")).IsEmpty() ? FString(TEXT("-")) : Read(TEXT("LoadingRange"))));
		const FString HLOD = Read(TEXT("HLODLayer"));
		if (!HLOD.IsEmpty() && HLOD != TEXT("None")) Line += TEXT(", HLODLayer ") + HLOD;
		if (Ref.Owner && !Ref.Array) Line += FString::Printf(TEXT(" (%s)"), *Ref.Owner->GetClass()->GetName());
		return Line;
	}

	FString LayerTypeName(const UHLODLayer& Layer)
	{
		const FProperty* Property = UHLODLayer::StaticClass()->FindPropertyByName(TEXT("LayerType"));
		return Property ? ExportValue(*Property, &Layer, const_cast<UHLODLayer*>(&Layer)) : FString();
	}

	FHyperAIHLODLayerInfo DescribeHLODLayer(UHLODLayer& Layer, const int32 Actors, const bool bSettings)
	{
		FHyperAIHLODLayerInfo Info;
		Info.Asset = Layer.GetPathName();
		Info.LayerType = LayerTypeName(Layer);
		Info.bSpatiallyLoaded = Layer.IsSpatiallyLoaded();
		Info.CellSize = Layer.GetCellSize();
		Info.LoadingRange = Layer.IsSpatiallyLoaded() ? Layer.GetLoadingRange() : 0.0;
		Info.Parent = Layer.GetParentLayer() ? Layer.GetParentLayer()->GetPathName() : FString();
		Info.Actors = Actors;
		if (bSettings)
		{
			Info.Settings = ExportSettings(*UHLODLayer::StaticClass(), &Layer, &Layer);
			if (UHLODBuilderSettings* Builder = const_cast<UHLODBuilderSettings*>(Layer.GetHLODBuilderSettings()))
			{
				for (const FString& Line : ExportSettings(*Builder->GetClass(), Builder, Builder)) Info.Settings.Add(TEXT("Builder.") + Line);
			}
		}
		return Info;
	}

	FString Summarize(const FHyperAIHLODLayerInfo& Info)
	{
		return FString::Printf(TEXT("%s: %s, %s"), *Info.Asset, *Info.LayerType,
			Info.bSpatiallyLoaded ? *FString::Printf(TEXT("cells %d cm, loading range %.0f cm"), Info.CellSize, Info.LoadingRange) : TEXT("always loaded"));
	}

	/** An HLOD layer made outside the undo step (so undo leaves it whole), with builder settings for its type. */
	UHLODLayer* MakeHLODLayer(UObject* Outer, const FName Name, const EObjectFlags Flags, const TArray<FHyperAIWorldPartitionSetting>& Settings,
		const TArray<FHyperAIWorldPartitionSetting>& BuilderSettings, FString& OutError)
	{
		UHLODLayer* Layer = NewObject<UHLODLayer>(Outer, Name, Flags);
		if (!ApplySettings(*UHLODLayer::StaticClass(), Layer, Layer, Settings, /*bNotify=*/true, OutError)) return nullptr;
		if (!Layer->GetHLODBuilderSettings())
		{
			// A new layer gets its builder settings when its type changes, as the details panel would change it.
			// UHLODLayer keeps its override private; UObject's is public and reaches it.
			FPropertyChangedEvent Event(UHLODLayer::StaticClass()->FindPropertyByName(TEXT("LayerType")), EPropertyChangeType::ValueSet);
			static_cast<UObject*>(Layer)->PostEditChangeProperty(Event);
		}
		if (!BuilderSettings.IsEmpty())
		{
			UHLODBuilderSettings* Builder = const_cast<UHLODBuilderSettings*>(Layer->GetHLODBuilderSettings());
			if (!Builder)
			{
				OutError = TEXT("This HLOD layer type has no builder settings to set.");
				return nullptr;
			}
			if (!ApplySettings(*Builder->GetClass(), Builder, Builder, BuilderSettings, /*bNotify=*/true, OutError)) return nullptr;
		}
		return Layer;
	}

	/** What the check knows of the plan so far: data layers it makes or deletes, HLOD layers it makes. */
	struct FPlanned
	{
		TMap<FString, bool> NewLayers;
		TSet<FString> DeletedLayers;
		TSet<FString> NewHLODLayers;
		bool bLoadsRegion = false;
	};

	/** Whether the plan leaves a data layer there by then, and whether it is a runtime one. */
	bool KnowsLayer(const FPlanned& Plan, const FString& Name, bool& bOutRuntime)
	{
		const FString Key = Name.TrimStartAndEnd().ToLower();
		if (Plan.DeletedLayers.Contains(Key)) return false;
		if (const bool* bRuntime = Plan.NewLayers.Find(Key))
		{
			bOutRuntime = *bRuntime;
			return true;
		}
		const UDataLayerInstance* Layer = FindDataLayer(Name);
		if (!Layer) return false;
		bOutRuntime = Layer->IsRuntime();
		return true;
	}

	bool KnowsHLODLayer(const FPlanned& Plan, const FString& Reference)
	{
		return IsNone(Reference) || Plan.NewHLODLayers.Contains(PackageOf(Reference).ToLower()) || Load<UHLODLayer>(Reference) != nullptr;
	}

	bool CheckLayerFields(const FPlanned& Plan, const FHyperAIWorldPartitionOp& Op, const bool bRuntime, FString& OutError)
	{
		TOptional<bool> Flag;
		if (!ParseFlag(Op.Visible, TEXT("Visible"), Flag, OutError) || !ParseFlag(Op.LoadedInEditor, TEXT("LoadedInEditor"), Flag, OutError)) return false;
		if (!Op.InitialRuntimeState.IsEmpty())
		{
			EDataLayerRuntimeState State;
			if (!ParseRuntimeState(Op.InitialRuntimeState, State))
			{
				OutError = TEXT("InitialRuntimeState is Unloaded, Loaded or Activated.");
				return false;
			}
			if (!bRuntime)
			{
				OutError = TEXT("Only a runtime data layer has an InitialRuntimeState.");
				return false;
			}
		}
		if (!Op.Parent.IsEmpty() && !IsNone(Op.Parent))
		{
			bool bParentRuntime = false;
			if (!KnowsLayer(Plan, Op.Parent, bParentRuntime))
			{
				OutError = FString::Printf(TEXT("No data layer is named %s."), *Op.Parent);
				return false;
			}
			if (bParentRuntime != bRuntime)
			{
				OutError = TEXT("A data layer's parent is of its own type: runtime under runtime, editor under editor.");
				return false;
			}
			if (Op.Parent.TrimStartAndEnd().Equals(Op.DataLayer.TrimStartAndEnd(), ESearchCase::IgnoreCase))
			{
				OutError = TEXT("A data layer cannot be its own parent.");
				return false;
			}
		}
		return true;
	}

	bool Check(UWorld& World, const FHyperAIWorldPartitionPlanRequest& Request, FString& OutError)
	{
		if (Request.Operations.IsEmpty() || Request.Operations.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations."), C::MaxOperations);
			return false;
		}
		UWorldPartition* Partition = World.GetWorldPartition();
		if (!Partition)
		{
			OutError = TEXT("The open level does not use World Partition, so it has no data layers, HLOD layers or streaming grids. Nothing was changed.");
			return false;
		}
		FPlanned Plan;
		// Regions load before the plan's edits, wherever they come in it.
		Plan.bLoadsRegion = Request.Operations.ContainsByPredicate([](const FHyperAIWorldPartitionOp& Op) { return KindOf(Op) == TEXT("region.load"); });
		for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
		{
			const FHyperAIWorldPartitionOp& Op = Request.Operations[Index];
			const FString Kind = KindOf(Op);
			auto Fail = [&OutError, &Op, Index](const FString& Message)
			{
				OutError = FString::Printf(TEXT("Operation %d (%s): %s Nothing was changed."), Index, *Op.Kind, *Message);
				return false;
			};
			FString Why;
			if (Op.Actors.Num() > C::MaxActorsPerOp) return Fail(FString::Printf(TEXT("An op names at most %d actors."), C::MaxActorsPerOp));
			for (const FString& Label : Op.Actors)
			{
				// An actor in a region the plan loads first is still unknown to the editor now.
				if (!FindActor(World, Label, Why) && !(Plan.bLoadsRegion && Why.Contains(TEXT("not loaded")))) return Fail(Why);
			}
			if (Kind == TEXT("data_layer.create"))
			{
				const FString Path = Op.Asset.TrimStartAndEnd().IsEmpty() ? FString(DefaultDataLayerFolder) + Op.DataLayer.TrimStartAndEnd() : PackageOf(Op.Asset);
				const FString Name = FPackageName::GetShortName(Path);
				if (!IsIdentifier(Name)) return Fail(TEXT("Name the DataLayer: letters, digits and _."));
				if (!Op.DataLayer.IsEmpty() && !Op.DataLayer.TrimStartAndEnd().Equals(Name, ESearchCase::IgnoreCase))
				{
					return Fail(FString::Printf(TEXT("A data layer is named after its asset: this one would be %s."), *Name));
				}
				bool bKnownRuntime = false;
				if (KnowsLayer(Plan, Name, bKnownRuntime)) return Fail(FString::Printf(TEXT("A data layer is named %s already."), *Name));
				const FString Type = Op.Type.TrimStartAndEnd().ToLower();
				if (!Type.IsEmpty() && Type != TEXT("runtime") && Type != TEXT("editor")) return Fail(TEXT("Type is Runtime or Editor."));
				bool bRuntime = Type != TEXT("editor");
				if (const UDataLayerAsset* Existing = Load<UDataLayerAsset>(Path))
				{
					if (!Type.IsEmpty() && Existing->IsRuntime() != bRuntime) return Fail(FString::Printf(TEXT("%s is an existing %s data layer asset."), *Path, Existing->IsRuntime() ? TEXT("runtime") : TEXT("editor")));
					bRuntime = Existing->IsRuntime();
				}
				// Free in memory too: an unsaved asset there of another class would stop the editor when the layer is made.
				else if (!IsNewAssetPath(Path) || !FHyperAIStudioNames::IsFreePackage(Path))
				{
					return Fail(FString::Printf(TEXT("%s is not a data layer asset, nor a free /Game path for one."), *Path));
				}
				FHyperAIWorldPartitionOp Named = Op;
				Named.DataLayer = Name;
				if (!CheckLayerFields(Plan, Named, bRuntime, Why)) return Fail(Why);
				Plan.NewLayers.Add(Name.ToLower(), bRuntime);
				Plan.DeletedLayers.Remove(Name.ToLower());
			}
			else if (Kind.StartsWith(TEXT("data_layer.")))
			{
				bool bRuntime = false;
				if (!KnowsLayer(Plan, Op.DataLayer, bRuntime)) return Fail(FString::Printf(TEXT("No data layer is named %s."), *Op.DataLayer));
				if (Kind == TEXT("data_layer.set"))
				{
					if (Op.Parent.IsEmpty() && Op.InitialRuntimeState.IsEmpty() && Op.Visible.IsEmpty() && Op.LoadedInEditor.IsEmpty())
					{
						return Fail(TEXT("Give what to change: Parent, InitialRuntimeState, Visible or LoadedInEditor."));
					}
					if (!CheckLayerFields(Plan, Op, bRuntime, Why)) return Fail(Why);
				}
				else if (Kind == TEXT("data_layer.delete"))
				{
					Plan.DeletedLayers.Add(Op.DataLayer.TrimStartAndEnd().ToLower());
					Plan.NewLayers.Remove(Op.DataLayer.TrimStartAndEnd().ToLower());
				}
				else if (Kind == TEXT("data_layer.add_actors") || Kind == TEXT("data_layer.remove_actors"))
				{
					if (Op.Actors.IsEmpty()) return Fail(TEXT("Name the Actors."));
				}
				else
				{
					return Fail(TEXT("Unknown op; see hyper_world_partition_apply_plan's description."));
				}
			}
			else if (Kind == TEXT("actors.set"))
			{
				if (Op.Actors.IsEmpty()) return Fail(TEXT("Name the Actors."));
				TOptional<bool> Spatial;
				if (!ParseFlag(Op.SpatiallyLoaded, TEXT("SpatiallyLoaded"), Spatial, Why)) return Fail(Why);
				if (!Spatial.IsSet() && Op.RuntimeGrid.IsEmpty() && Op.HLODLayer.IsEmpty()) return Fail(TEXT("Give what to change: SpatiallyLoaded, RuntimeGrid or HLODLayer."));
				if (!Op.HLODLayer.IsEmpty() && !KnowsHLODLayer(Plan, Op.HLODLayer)) return Fail(FString::Printf(TEXT("%s is not an HLOD layer asset."), *Op.HLODLayer));
				if (Spatial.IsSet())
				{
					for (const FString& Label : Op.Actors)
					{
						const AActor* Actor = FindActor(World, Label, Why);
						if (Actor && Actor->GetIsSpatiallyLoaded() != Spatial.GetValue() && !Actor->CanChangeIsSpatiallyLoadedFlag())
						{
							return Fail(FString::Printf(TEXT("%s (%s) cannot change whether it is spatially loaded."), *Label, *Actor->GetClass()->GetName()));
						}
					}
				}
			}
			else if (Kind == TEXT("hlod_layer.create") || Kind == TEXT("hlod_layer.set"))
			{
				const FString Path = PackageOf(Op.Asset);
				const bool bCreate = Kind == TEXT("hlod_layer.create");
				if (bCreate)
				{
					if (!IsNewAssetPath(Path) || !IsIdentifier(FPackageName::GetShortName(Path))) return Fail(TEXT("A new HLOD layer goes under /Game, as /Game/Folder/HLOD_Name."));
					if (Plan.NewHLODLayers.Contains(Path.ToLower()) || Load<UHLODLayer>(Path) || !FHyperAIStudioNames::IsFreePackage(Path))
					{
						return Fail(FString::Printf(TEXT("%s exists already; hlod_layer.set changes it."), *Path));
					}
				}
				else
				{
					if (!Plan.NewHLODLayers.Contains(Path.ToLower()) && !Load<UHLODLayer>(Path)) return Fail(FString::Printf(TEXT("%s is not an HLOD layer asset."), *Op.Asset));
					if (Op.Settings.IsEmpty() && Op.BuilderSettings.IsEmpty()) return Fail(TEXT("Give the Settings or BuilderSettings to change."));
				}
				// The settings, tried on a scratch layer that nothing keeps.
				if (!MakeHLODLayer(GetTransientPackage(), NAME_None, RF_Transient, Op.Settings, Op.BuilderSettings, Why)) return Fail(Why);
				if (bCreate) Plan.NewHLODLayers.Add(Path.ToLower());
			}
			else if (Kind == TEXT("world_partition.set"))
			{
				if (Op.HLODLayer.IsEmpty()) return Fail(TEXT("Give the default HLODLayer, or none."));
				if (!KnowsHLODLayer(Plan, Op.HLODLayer)) return Fail(FString::Printf(TEXT("%s is not an HLOD layer asset."), *Op.HLODLayer));
			}
			else if (Kind == TEXT("world_partition.set_grid"))
			{
				FGridRef Grid;
				if (!FindGrid(*Partition, Op.Grid, Grid, Why)) return Fail(Why);
				if (Op.Settings.IsEmpty()) return Fail(TEXT("Give the grid's Settings to change: CellSize, LoadingRange (cm)."));
				// Tried on a copy of the grid, so the check changes nothing.
				TArray<uint8> Scratch;
				Scratch.SetNumZeroed(Grid.Type->GetStructureSize());
				if (const UScriptStruct* Struct = Cast<UScriptStruct>(Grid.Type))
				{
					Struct->InitializeStruct(Scratch.GetData());
					Struct->CopyScriptStruct(Scratch.GetData(), Grid.Container);
					const bool bTaken = ApplySettings(*Struct, Scratch.GetData(), Grid.Owner, Op.Settings, /*bNotify=*/false, Why);
					Struct->DestroyStruct(Scratch.GetData());
					if (!bTaken) return Fail(Why);
				}
				else if (UObject* Copy = DuplicateObject<UObject>(Grid.Owner, GetTransientPackage()))
				{
					Copy->SetFlags(RF_Transient);
					if (!ApplySettings(*Copy->GetClass(), Copy, Copy, Op.Settings, /*bNotify=*/false, Why)) return Fail(Why);
				}
			}
			else if (Kind == TEXT("region.load"))
			{
				if (!(Op.Size.X > 0.0 && Op.Size.Y > 0.0)) return Fail(TEXT("Give the region's Size in meters and its Center in cm."));
			}
			else if (Kind != TEXT("region.unload"))
			{
				return Fail(TEXT("Unknown op; see hyper_world_partition_apply_plan's description."));
			}
		}
		return true;
	}

	bool IsRegionOp(const FHyperAIWorldPartitionOp& Op)
	{
		return KindOf(Op).StartsWith(TEXT("region."));
	}

	FString RegionLabel(const FString& Label)
	{
		const FString Trimmed = Label.TrimStartAndEnd();
		return FString(RegionPrefix) + (Trimmed.IsEmpty() ? FString(TEXT("region")) : Trimmed);
	}

	/** Region ops change what the editor has loaded, not the level: they run outside the undo step. */
	void ApplyRegion(UWorld& World, const FHyperAIWorldPartitionOp& Op, FHyperAIWorldPartitionPlanReport& Report)
	{
		UWorldPartition* Partition = World.GetWorldPartition();
		if (!Partition) return;
		if (KindOf(Op) == TEXT("region.load"))
		{
			const FVector2D Center(Op.Center);
			const FVector2D Half = Op.Size * 50.0;
			const FBox Box(FVector(Center - Half, -RegionReach), FVector(Center + Half, RegionReach));
			const FString Label = RegionLabel(Op.Label);
			UWorldPartitionEditorLoaderAdapter* Adapter = Partition->CreateEditorLoaderAdapter<FLoaderAdapterShape>(&World, Box, Label);
			if (IWorldPartitionActorLoaderInterface::ILoaderAdapter* Loader = Adapter ? Adapter->GetLoaderAdapter() : nullptr)
			{
				Loader->SetUserCreated(true);
				Loader->Load();
				Report.Notes.Add(FString::Printf(TEXT("Region '%s' loaded: %g x %g m at (%.0f, %.0f)."), *Label, Op.Size.X, Op.Size.Y, Op.Center.X, Op.Center.Y));
			}
			return;
		}
		const FString Wanted = Op.Label.TrimStartAndEnd().IsEmpty() ? FString() : RegionLabel(Op.Label);
		TArray<UWorldPartitionEditorLoaderAdapter*> Release;
		for (UWorldPartitionEditorLoaderAdapter* Adapter : Partition->GetRegisteredEditorLoaderAdapters())
		{
			const IWorldPartitionActorLoaderInterface::ILoaderAdapter* Loader = Adapter ? Adapter->GetLoaderAdapter() : nullptr;
			const FString Label = Loader && Loader->GetLabel().IsSet() ? Loader->GetLabel().GetValue() : FString();
			if (Wanted.IsEmpty() ? Label.StartsWith(RegionPrefix) : Label.Equals(Wanted, ESearchCase::IgnoreCase)) Release.Add(Adapter);
		}
		for (UWorldPartitionEditorLoaderAdapter* Adapter : Release)
		{
			Adapter->GetLoaderAdapter()->Unload();
			Partition->ReleaseEditorLoaderAdapter(Adapter);
		}
		Report.Notes.Add(FString::Printf(TEXT("%d region(s) unloaded."), Release.Num()));
	}

	TArray<AActor*> ActorsOf(UWorld& World, const FHyperAIWorldPartitionOp& Op, FString& OutError)
	{
		TArray<AActor*> Actors;
		for (const FString& Label : Op.Actors)
		{
			AActor* Actor = FindActor(World, Label, OutError);
			if (!Actor) return {};
			Actors.AddUnique(Actor);
		}
		return Actors;
	}

	void NotifyActorChange(AActor& Actor, const FName Property)
	{
		if (FProperty* Changed = FindFProperty<FProperty>(AActor::StaticClass(), Property))
		{
			FPropertyChangedEvent Event(Changed, EPropertyChangeType::ValueSet);
			Actor.PostEditChangeProperty(Event);
		}
	}

	bool ApplyLayerFields(const FHyperAIWorldPartitionOp& Op, UDataLayerInstance& Layer, FString& OutError)
	{
		UDataLayerEditorSubsystem* Subsystem = DataLayers();
		if (!Op.Parent.IsEmpty())
		{
			UDataLayerInstance* Parent = IsNone(Op.Parent) ? nullptr : FindDataLayer(Op.Parent);
			if (!IsNone(Op.Parent) && !Parent)
			{
				OutError = FString::Printf(TEXT("No data layer is named %s."), *Op.Parent);
				return false;
			}
			FText Reason;
			if (Parent && !Layer.CanBeChildOf(Parent, &Reason))
			{
				OutError = Reason.ToString();
				return false;
			}
			Subsystem->SetParentDataLayer(&Layer, Parent);
		}
		EDataLayerRuntimeState State;
		if (!Op.InitialRuntimeState.IsEmpty() && ParseRuntimeState(Op.InitialRuntimeState, State))
		{
			Subsystem->SetDataLayerInitialRuntimeState(&Layer, State);
		}
		TOptional<bool> Visible;
		TOptional<bool> Loaded;
		ParseFlag(Op.Visible, TEXT("Visible"), Visible, OutError);
		ParseFlag(Op.LoadedInEditor, TEXT("LoadedInEditor"), Loaded, OutError);
		if (Visible.IsSet())
		{
			Subsystem->SetDataLayerIsInitiallyVisible(&Layer, Visible.GetValue());
			Subsystem->SetDataLayerVisibility(&Layer, Visible.GetValue());
		}
		if (Loaded.IsSet()) Subsystem->SetDataLayerIsLoadedInEditor(&Layer, Loaded.GetValue(), /*bIsFromUserChange=*/true);
		return true;
	}

	FString DescribeLayer(const UDataLayerInstance& Layer)
	{
		return FString::Printf(TEXT("%s (%s%s%s%s)"), *Layer.GetDataLayerShortName(), Layer.IsRuntime() ? TEXT("runtime, starts ") : TEXT("editor"),
			Layer.IsRuntime() ? *RuntimeStateName(Layer.GetInitialRuntimeState()) : TEXT(""), Layer.GetParent() ? *(TEXT(", under ") + Layer.GetParent()->GetDataLayerShortName()) : TEXT(""),
			Layer.IsVisible() ? TEXT(", shown") : TEXT(", hidden"));
	}

	/** One op, inside the plan's undo step. */
	bool Apply(UWorld& World, const FHyperAIWorldPartitionOp& Op, FHyperAIWorldPartitionPlanReport& Report, TArray<UObject*>& Assets, FString& OutError)
	{
		const FString Kind = KindOf(Op);
		UWorldPartition* Partition = World.GetWorldPartition();
		UDataLayerEditorSubsystem* Subsystem = DataLayers();
		if (!Partition || !Subsystem)
		{
			OutError = TEXT("The open level no longer uses World Partition.");
			return false;
		}
		if (Kind == TEXT("data_layer.create"))
		{
			const FString Path = Op.Asset.TrimStartAndEnd().IsEmpty() ? FString(DefaultDataLayerFolder) + Op.DataLayer.TrimStartAndEnd() : PackageOf(Op.Asset);
			UDataLayerAsset* Asset = Load<UDataLayerAsset>(Path);
			if (!Asset)
			{
				// Made outside the undo step, so undo leaves the asset whole.
				UPackage* Package = CreatePackage(*Path);
				Asset = NewObject<UDataLayerAsset>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone | RF_Transactional);
				Asset->SetType(Op.Type.TrimStartAndEnd().Equals(TEXT("editor"), ESearchCase::IgnoreCase) ? EDataLayerType::Editor : EDataLayerType::Runtime);
				FAssetRegistryModule::AssetCreated(Asset);
				Package->MarkPackageDirty();
				Assets.AddUnique(Asset);
			}
			FDataLayerCreationParameters Parameters;
			Parameters.DataLayerAsset = Asset;
			Parameters.WorldDataLayers = World.GetWorldDataLayers();
			UDataLayerInstance* Layer = Parameters.WorldDataLayers ? Subsystem->CreateDataLayerInstance(Parameters) : nullptr;
			if (!Layer)
			{
				OutError = FString::Printf(TEXT("Unreal would not make a data layer for %s in this level."), *Asset->GetPathName());
				return false;
			}
			if (!ApplyLayerFields(Op, *Layer, OutError)) return false;
			Report.Notes.Add(TEXT("Data layer made: ") + DescribeLayer(*Layer) + TEXT("."));
			return true;
		}
		if (Kind.StartsWith(TEXT("data_layer.")))
		{
			UDataLayerInstance* Layer = FindDataLayer(Op.DataLayer);
			if (!Layer)
			{
				OutError = FString::Printf(TEXT("No data layer is named %s."), *Op.DataLayer);
				return false;
			}
			if (Kind == TEXT("data_layer.set"))
			{
				if (!ApplyLayerFields(Op, *Layer, OutError)) return false;
				Report.Notes.Add(TEXT("Data layer set: ") + DescribeLayer(*Layer) + TEXT("."));
				return true;
			}
			if (Kind == TEXT("data_layer.delete"))
			{
				const FString Name = Layer->GetDataLayerShortName();
				Subsystem->DeleteDataLayer(Layer);
				Report.Notes.Add(FString::Printf(TEXT("Data layer %s deleted; its actors stay in the level."), *Name));
				return true;
			}
			const TArray<AActor*> Actors = ActorsOf(World, Op, OutError);
			if (Actors.IsEmpty()) return false;
			const bool bAdd = Kind == TEXT("data_layer.add_actors");
			int32 Refused = 0;
			for (AActor* Actor : Actors)
			{
				if (bAdd && !Layer->CanAddActor(Actor)) ++Refused;
			}
			if (Refused > 0)
			{
				OutError = FString::Printf(TEXT("%d of the actors cannot go in data layer %s."), Refused, *Layer->GetDataLayerShortName());
				return false;
			}
			if (bAdd) Subsystem->AddActorsToDataLayer(Actors, Layer);
			else Subsystem->RemoveActorsFromDataLayer(Actors, Layer);
			Report.Notes.Add(FString::Printf(TEXT("%d actor(s) %s data layer %s."), Actors.Num(), bAdd ? TEXT("added to") : TEXT("taken out of"), *Layer->GetDataLayerShortName()));
			return true;
		}
		if (Kind == TEXT("actors.set"))
		{
			const TArray<AActor*> Actors = ActorsOf(World, Op, OutError);
			if (Actors.IsEmpty()) return false;
			TOptional<bool> Spatial;
			ParseFlag(Op.SpatiallyLoaded, TEXT("SpatiallyLoaded"), Spatial, OutError);
			UHLODLayer* HLODLayer = Op.HLODLayer.IsEmpty() || IsNone(Op.HLODLayer) ? nullptr : Load<UHLODLayer>(Op.HLODLayer);
			if (!Op.HLODLayer.IsEmpty() && !IsNone(Op.HLODLayer) && !HLODLayer)
			{
				OutError = FString::Printf(TEXT("%s is not an HLOD layer asset."), *Op.HLODLayer);
				return false;
			}
			for (AActor* Actor : Actors)
			{
				Actor->Modify();
				if (Spatial.IsSet() && Actor->GetIsSpatiallyLoaded() != Spatial.GetValue())
				{
					if (!Actor->CanChangeIsSpatiallyLoadedFlag())
					{
						OutError = FString::Printf(TEXT("%s cannot change whether it is spatially loaded."), *Actor->GetActorLabel());
						return false;
					}
					Actor->SetIsSpatiallyLoaded(Spatial.GetValue());
					NotifyActorChange(*Actor, TEXT("bIsSpatiallyLoaded"));
				}
				if (!Op.RuntimeGrid.IsEmpty())
				{
					Actor->SetRuntimeGrid(IsNone(Op.RuntimeGrid) ? NAME_None : FName(*Op.RuntimeGrid.TrimStartAndEnd()));
					NotifyActorChange(*Actor, AActor::GetRuntimeGridPropertyName());
				}
				if (!Op.HLODLayer.IsEmpty())
				{
					Actor->SetHLODLayer(HLODLayer);
					NotifyActorChange(*Actor, TEXT("HLODLayer"));
				}
			}
			Report.Notes.Add(FString::Printf(TEXT("%d actor(s) set%s%s%s."), Actors.Num(),
				Spatial.IsSet() ? (Spatial.GetValue() ? TEXT(": spatially loaded") : TEXT(": always loaded")) : TEXT(""),
				Op.RuntimeGrid.IsEmpty() ? TEXT("") : *(TEXT(", runtime grid ") + Op.RuntimeGrid),
				Op.HLODLayer.IsEmpty() ? TEXT("") : *(TEXT(", HLOD layer ") + (HLODLayer ? HLODLayer->GetName() : FString(TEXT("none"))))));
			return true;
		}
		if (Kind == TEXT("hlod_layer.create"))
		{
			const FString Path = PackageOf(Op.Asset);
			UPackage* Package = CreatePackage(*Path);
			// Made without undo records, so undo leaves the new asset whole; it takes undo from here on.
			UHLODLayer* Layer = MakeHLODLayer(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone, Op.Settings, Op.BuilderSettings, OutError);
			if (!Layer) return false;
			Layer->SetFlags(RF_Transactional);
			FAssetRegistryModule::AssetCreated(Layer);
			Package->MarkPackageDirty();
			Assets.AddUnique(Layer);
			Report.Notes.Add(TEXT("HLOD layer made: ") + Summarize(DescribeHLODLayer(*Layer, 0, false)) + TEXT("."));
			return true;
		}
		if (Kind == TEXT("hlod_layer.set"))
		{
			UHLODLayer* Layer = Load<UHLODLayer>(Op.Asset);
			if (!Layer)
			{
				OutError = FString::Printf(TEXT("%s is not an HLOD layer asset."), *Op.Asset);
				return false;
			}
			Layer->Modify();
			if (!ApplySettings(*UHLODLayer::StaticClass(), Layer, Layer, Op.Settings, /*bNotify=*/true, OutError)) return false;
			if (!Op.BuilderSettings.IsEmpty())
			{
				UHLODBuilderSettings* Builder = const_cast<UHLODBuilderSettings*>(Layer->GetHLODBuilderSettings());
				if (!Builder)
				{
					OutError = TEXT("This HLOD layer type has no builder settings to set.");
					return false;
				}
				Builder->Modify();
				if (!ApplySettings(*Builder->GetClass(), Builder, Builder, Op.BuilderSettings, /*bNotify=*/true, OutError)) return false;
			}
			Assets.AddUnique(Layer);
			Report.Notes.Add(TEXT("HLOD layer set: ") + Summarize(DescribeHLODLayer(*Layer, 0, false)) + TEXT("."));
			return true;
		}
		if (Kind == TEXT("world_partition.set"))
		{
			UHLODLayer* Layer = IsNone(Op.HLODLayer) ? nullptr : Load<UHLODLayer>(Op.HLODLayer);
			if (!IsNone(Op.HLODLayer) && !Layer)
			{
				OutError = FString::Printf(TEXT("%s is not an HLOD layer asset."), *Op.HLODLayer);
				return false;
			}
			Partition->Modify();
			Partition->SetDefaultHLODLayer(Layer);
			Report.Notes.Add(FString::Printf(TEXT("Default HLOD layer: %s."), Layer ? *Layer->GetPathName() : TEXT("none")));
			return true;
		}
		if (Kind == TEXT("world_partition.set_grid"))
		{
			FGridRef Grid;
			if (!FindGrid(*Partition, Op.Grid, Grid, OutError)) return false;
			Grid.Owner->Modify();
			if (Grid.Array)
			{
				// A spatial hash grid lives in the hash's Grids array: the hash hears of the array's change.
				Grid.Owner->PreEditChange(Grid.Array);
				const bool bTaken = ApplySettings(*Grid.Type, Grid.Container, Grid.Owner, Op.Settings, /*bNotify=*/false, OutError);
				FPropertyChangedEvent Event(Grid.Array, EPropertyChangeType::ValueSet);
				Grid.Owner->PostEditChangeProperty(Event);
				if (!bTaken) return false;
			}
			else if (!ApplySettings(*Grid.Type, Grid.Container, Grid.Owner, Op.Settings, /*bNotify=*/true, OutError))
			{
				return false;
			}
			Report.Notes.Add(TEXT("Runtime grid set: ") + DescribeGrid(Grid) + TEXT("."));
			return true;
		}
		OutError = TEXT("Unknown op.");
		return false;
	}

	/** The plan for real: regions loaded, every edit as one undo step, regions unloaded, then saved as asked. */
	FHyperAIWorldPartitionPlanReport Execute(const FHyperAIWorldPartitionPlanRequest& Request, const FString& OperationId)
	{
		FHyperAIWorldPartitionPlanReport Report;
		Report.OperationId = OperationId;
		UWorld* World = EditorWorld();
		if (!World || !World->GetWorldPartition())
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = TEXT("No World Partition level is open in the editor.");
			return Report;
		}
		for (const FHyperAIWorldPartitionOp& Op : Request.Operations)
		{
			if (KindOf(Op) == TEXT("region.load"))
			{
				ApplyRegion(*World, Op, Report);
				++Report.Applied;
			}
		}
		TArray<UObject*> Assets;
		{
			const FScopedTransaction Transaction(LOCTEXT("WorldPartitionPlan", "HyperAI: Edit World Partition"));
			for (int32 Index = 0; Index < Request.Operations.Num(); ++Index)
			{
				const FHyperAIWorldPartitionOp& Op = Request.Operations[Index];
				if (IsRegionOp(Op)) continue;
				FString Error;
				if (!Apply(*World, Op, Report, Assets, Error))
				{
					Report.Status = TEXT("operation_failed");
					Report.Diagnostic = FString::Printf(TEXT("Operation %d (%s): %s%s"), Index, *Op.Kind, *Error,
						Report.Applied > 0 ? TEXT(" The operations before it stay applied, unsaved; Edit > Undo takes the plan back.") : TEXT(""));
					return Report;
				}
				++Report.Applied;
			}
		}
		for (const FHyperAIWorldPartitionOp& Op : Request.Operations)
		{
			if (KindOf(Op) == TEXT("region.unload"))
			{
				ApplyRegion(*World, Op, Report);
				++Report.Applied;
			}
		}
		for (UObject* Asset : Assets)
		{
			FString Error;
			if (!FHyperAIStudioSourceControl::SaveAsset(*Asset, Error))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = Error;
				return Report;
			}
			Report.Saved.Add(Asset->GetPathName());
		}
		if (Request.bSave)
		{
			if (!UEditorLoadingAndSavingUtils::SaveDirtyPackages(/*bSaveMapPackages=*/true, /*bSaveContentPackages=*/false))
			{
				Report.Status = TEXT("save_failed");
				Report.Diagnostic = TEXT("The level could not be saved; the changes are in memory.");
				return Report;
			}
			Report.Saved.Add(World->GetOutermost()->GetName());
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("%d operation(s) applied%s."), Report.Applied, Request.bSave ? TEXT(" and saved") : TEXT("; the level is unsaved"));
		return Report;
	}

	void Record(const FString& OperationId, const FHyperAIWorldPartitionPlanReport& Done)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = Done.bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = C::PlanToolName;
		Entry.Target = Done.Notes.IsEmpty() ? FString() : Done.Notes[0].Left(200);
		Entry.OperationId = OperationId;
		Entry.StatusCode = Done.bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Done.Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}
}

FHyperAIWorldPartitionInspectReport FHyperAIStudioWorldPartitionContracts::Inspect(const FHyperAIWorldPartitionInspectRequest& Request)
{
	using namespace HyperAIStudio::WorldPartitionTools::Private;
	FHyperAIWorldPartitionInspectReport Report;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report)) return Report;
	UWorld* World = EditorWorld();
	if (!World)
	{
		Report.Diagnostic = TEXT("No level is open in the editor.");
		return Report;
	}
	UWorldPartition* Partition = World->GetWorldPartition();
	Report.bOk = true;
	if (!Partition)
	{
		Report.Diagnostic = TEXT("The open level does not use World Partition: no streaming, data layers or HLOD layers.");
		return Report;
	}
	Report.bWorldPartition = true;
	Report.bStreaming = Partition->IsStreamingEnabled();
	if (const UWorldPartitionRuntimeHash* Hash = Partition->RuntimeHash)
	{
		const FString Class = Hash->GetClass()->GetName();
		Report.RuntimeHash = Class.Contains(TEXT("SpatialHash")) ? FString(TEXT("spatial hash (grids)"))
			: Class.Contains(TEXT("HashSet")) ? FString(TEXT("hash set (runtime partitions)")) : Class;
	}
	for (const FGridRef& Grid : Grids(*Partition))
	{
		Report.Grids.Add(DescribeGrid(Grid));
	}
	if (const UHLODLayer* Default = Partition->GetDefaultHLODLayer()) Report.DefaultHLODLayer = Default->GetPathName();

	// One pass over every actor in the level, loaded or not.
	TMap<FName, int32> PerDataLayer;
	TMap<FSoftObjectPath, int32> PerHLODLayer;
	FWorldPartitionHelpers::ForEachActorDescInstance(Partition, AActor::StaticClass(), [&Report, &PerDataLayer, &PerHLODLayer, &Request](const FWorldPartitionActorDescInstance* Desc)
	{
		if (Desc->GetActorNativeClass() && Desc->GetActorNativeClass()->IsChildOf(AWorldPartitionHLOD::StaticClass()))
		{
			++Report.HLODActors;
			return true;
		}
		++Report.Actors;
		if (Desc->IsLoaded()) ++Report.LoadedActors;
		if (!Desc->GetIsSpatiallyLoaded())
		{
			++Report.AlwaysLoaded;
			if (Request.bActors && Report.AlwaysLoadedActors.Num() < 50) Report.AlwaysLoadedActors.Add(Desc->GetActorLabelString());
		}
		for (const FName& Name : Desc->GetDataLayerInstanceNames())
		{
			++PerDataLayer.FindOrAdd(Name);
		}
		if (Desc->GetHLODLayer().IsValid()) ++PerHLODLayer.FindOrAdd(Desc->GetHLODLayer());
		return true;
	});
	if (UDataLayerEditorSubsystem* Subsystem = DataLayers())
	{
		for (UDataLayerInstance* Layer : Subsystem->GetAllDataLayers())
		{
			if (!Layer) continue;
			FHyperAIDataLayerInfo& Info = Report.DataLayers.AddDefaulted_GetRef();
			Info.Name = Layer->GetDataLayerShortName();
			Info.Asset = Layer->GetAsset() ? Layer->GetAsset()->GetPathName() : FString();
			Info.Type = Layer->IsRuntime() ? TEXT("Runtime") : TEXT("Editor");
			Info.InitialRuntimeState = Layer->IsRuntime() ? RuntimeStateName(Layer->GetInitialRuntimeState()) : FString();
			Info.bVisible = Layer->IsVisible();
			Info.bLoadedInEditor = Layer->IsLoadedInEditor();
			Info.Parent = Layer->GetParent() ? Layer->GetParent()->GetDataLayerShortName() : FString();
			Info.LoadedActors = Subsystem->GetActorsFromDataLayer(Layer).Num();
			Info.Actors = PerDataLayer.FindRef(Layer->GetDataLayerFName());
		}
		Report.DataLayers.Sort([](const FHyperAIDataLayerInfo& A, const FHyperAIDataLayerInfo& B) { return A.Name < B.Name; });
	}
	if (const UHLODLayer* Default = Partition->GetDefaultHLODLayer()) PerHLODLayer.FindOrAdd(FSoftObjectPath(Default));
	for (const TPair<FSoftObjectPath, int32>& Pair : PerHLODLayer)
	{
		if (UHLODLayer* Layer = Cast<UHLODLayer>(Pair.Key.TryLoad()))
		{
			Report.HLODLayers.Add(DescribeHLODLayer(*Layer, Pair.Value, Request.bSettings));
		}
	}
	for (UWorldPartitionEditorLoaderAdapter* Adapter : Partition->GetRegisteredEditorLoaderAdapters())
	{
		const IWorldPartitionActorLoaderInterface::ILoaderAdapter* Loader = Adapter ? Adapter->GetLoaderAdapter() : nullptr;
		if (!Loader) continue;
		const TOptional<FBox> Bounds = Loader->GetBoundingBox();
		Report.Regions.Add(FString::Printf(TEXT("%s%s%s"), Loader->GetLabel().IsSet() ? *Loader->GetLabel().GetValue() : TEXT("(unnamed)"),
			Bounds.IsSet() ? *FString::Printf(TEXT(": %.0f x %.0f m at (%.0f, %.0f)"), Bounds->GetSize().X / 100.0, Bounds->GetSize().Y / 100.0, Bounds->GetCenter().X, Bounds->GetCenter().Y) : TEXT(""),
			Loader->IsLoaded() ? TEXT(", loaded") : TEXT(", unloaded")));
	}
	Report.BuildHLODs = FString::Printf(TEXT("In the editor: Build > Build HLODs (it saves, closes and reopens the level). Or run: UnrealEditor-Cmd \"%s\" %s -run=WorldPartitionBuilderCommandlet -Builder=WorldPartitionHLODsBuilder -SetupHLODs -BuildHLODs -AllowCommandletRendering"),
		*FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()), *World->GetOutermost()->GetName());
	Report.Diagnostic = FString::Printf(TEXT("World Partition, %s: %d actor(s) (%d loaded, %d always loaded), %d data layer(s), %d HLOD layer(s) in use, %d HLOD actor(s) built, %d editor region(s)."),
		Report.bStreaming ? TEXT("streaming") : TEXT("streaming off"), Report.Actors, Report.LoadedActors, Report.AlwaysLoaded, Report.DataLayers.Num(),
		Report.HLODLayers.Num(), Report.HLODActors, Report.Regions.Num());
	return Report;
}

FHyperAIWorldPartitionPlanReport FHyperAIStudioWorldPartitionContracts::ApplyPlan(const FHyperAIWorldPartitionPlanRequest& Request)
{
	using namespace HyperAIStudio::WorldPartitionTools::Private;
	FHyperAIWorldPartitionPlanReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("world-partition-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
	// Names and paths become FNames and lookups, which stop the editor past an FName's length; refused here first.
	if (FHyperAIStudioNames::RefuseRequest(Request, Report, {TEXT("Value")})) return Report;
	UWorld* World = EditorWorld();
	FString Error;
	if (!World)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("No level is open in the editor.");
		return Report;
	}
	if (!Check(*World, Request, Error))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = Error;
		return Report;
	}
	if (Request.bSave && World->GetOutermost()->GetName().StartsWith(TEXT("/Temp/")))
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("This level has never been saved, so there is nowhere to save it; save the level first or leave bSave off.");
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
		Summary.VariantId = TEXT("world_partition.plan.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		Summary.EffectTarget = World->GetOutermost()->GetName();
		Summary.Effects.Add(FString::Printf(TEXT("Edit World Partition: %d operation(s), one undo step%s"), Request.Operations.Num(), Request.bSave ? TEXT(", then the level saved") : TEXT("")));
		for (int32 Index = 0; Index < Request.Operations.Num() && Index < 24; ++Index)
		{
			const FHyperAIWorldPartitionOp& Op = Request.Operations[Index];
			const FString Target = !Op.DataLayer.IsEmpty() ? Op.DataLayer : !Op.Asset.IsEmpty() ? Op.Asset : !Op.Actors.IsEmpty() ? FString::Join(Op.Actors, TEXT(", ")) : Op.Label;
			Summary.Effects.Add(FString::Printf(TEXT("| %s %s"), *Op.Kind, *Target.Left(160)));
			for (const FString& Label : Op.Actors)
			{
				Summary.Touches.AddUnique(Label);
			}
		}
		const FString OperationId = Report.OperationId;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Request, OperationId]()
			{
				// The editor may have moved on while the plan waited (an asset deleted, a path taken, the level closed),
				// so it is checked again before anything changes.
				FString Problem;
				UWorld* Now = EditorWorld();
				if (!Now || !Check(*Now, Request, Problem))
				{
					FHyperAIWorldPartitionPlanReport Refused;
					Refused.OperationId = OperationId;
					Refused.Status = TEXT("invalid_request");
					Refused.Diagnostic = Now ? TEXT("The editor changed while the plan waited for approval: ") + Problem : FString(TEXT("No level is open in the editor; nothing was changed."));
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

FHyperAIWorldPartitionInspectReport UHyperAIStudioWorldPartitionToolset::hyper_world_partition_inspect(const FHyperAIWorldPartitionInspectRequest& Request)
{
	return FHyperAIStudioWorldPartitionContracts::Inspect(Request);
}

FHyperAIWorldPartitionPlanReport UHyperAIStudioWorldPartitionToolset::hyper_world_partition_apply_plan(const FHyperAIWorldPartitionPlanRequest& Request)
{
	return FHyperAIStudioWorldPartitionContracts::ApplyPlan(Request);
}

void FHyperAIStudioWorldPartitionRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioWorldPartitionRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioWorldPartitionRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioWorldPartitionToolset::StaticClass(),
			FHyperAIStudioWorldPartitionContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioWorldPartition, Warning, TEXT("Could not unregister the World Partition toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioWorldPartitionRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioWorldPartitionContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioWorldPartition, Verbose, TEXT("The World Partition source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioWorldPartitionToolset::StaticClass(),
		FHyperAIStudioWorldPartitionContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioWorldPartition, Error, TEXT("World Partition cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}

#undef LOCTEXT_NAMESPACE
