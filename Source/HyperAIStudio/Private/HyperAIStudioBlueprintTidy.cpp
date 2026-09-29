// Games by Hyper 2026.

#include "HyperAIStudioBlueprintTidy.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "HyperAIStudioSettings.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioBlueprintTidy, Log, All);

namespace HyperAIStudio::BlueprintTidy::Private
{
	struct FState
	{
		/** Per Blueprint, per graph, the nodes that were there when HyperAI last looked. */
		TMap<TWeakObjectPtr<UBlueprint>, TMap<FGuid, TSet<FGuid>>> Known;
		TArray<TWeakObjectPtr<UBlueprint>> Pending;
		FDelegateHandle Loaded;
		FDelegateHandle Created;
		FDelegateHandle PreCompile;
		FDelegateHandle EngineInit;
		FTSTicker::FDelegateHandle Tick;
	};

	FState& State()
	{
		static FState Instance;
		return Instance;
	}

	TArray<UEdGraph*> GraphsOf(UBlueprint* Blueprint)
	{
		TArray<UEdGraph*> Graphs;
		for (const TArray<TObjectPtr<UEdGraph>>* List : {&Blueprint->UbergraphPages, &Blueprint->FunctionGraphs, &Blueprint->MacroGraphs})
		{
			for (UEdGraph* Graph : *List)
			{
				if (Graph)
				{
					Graphs.Add(Graph);
				}
			}
		}
		return Graphs;
	}

	FBox2D Bounds(const UEdGraphNode* Node)
	{
		const float Width = Node->NodeWidth > 0 ? Node->NodeWidth : UEdGraphSchema_K2::EstimateNodeWidth(Node);
		const float Height = Node->NodeHeight > 0 ? Node->NodeHeight : UEdGraphSchema_K2::EstimateNodeHeight(const_cast<UEdGraphNode*>(Node));
		const FVector2D Min(Node->NodePosX, Node->NodePosY);
		return FBox2D(Min, Min + FVector2D(Width, Height));
	}

	bool Placeable(const UEdGraphNode* Node)
	{
		return Node && !Node->IsA<UEdGraphNode_Comment>();
	}

	/** True when Node sits over another placeable node of the graph. */
	bool Overlaps(const UEdGraph& Graph, const UEdGraphNode* Node)
	{
		const FBox2D Mine = Bounds(Node);
		for (const UEdGraphNode* Other : Graph.Nodes)
		{
			if (Other != Node && Placeable(Other) && Mine.Intersect(Bounds(Other)))
			{
				return true;
			}
		}
		return false;
	}

	/** Runs Epic's layered formatter over each group of nodes: one group per graph, the other nodes as anchors. */
	bool Arrange(const TArray<TArray<FString>>& Groups)
	{
		TArray<FString> Lines = {
			TEXT("import unreal"),
			TEXT("from editor_toolset.toolsets.blueprint import BlueprintTools"),
			TEXT("from editor_toolset.toolsets.blueprint_layout import GraphFormatter"),
			TEXT("for paths in GROUPS:"),
			TEXT("    nodes = [n for n in (unreal.find_object(None, p) for p in paths) if n]"),
			TEXT("    if not nodes:"),
			TEXT("        continue"),
			TEXT("    all_nodes = BlueprintTools.find_nodes(nodes[0].get_outer())"),
			TEXT("    GraphFormatter("),
			TEXT("        get_out_pins=lambda n: BlueprintTools._list_pins(n, unreal.EdGraphPinDirection.EGPD_OUTPUT),"),
			TEXT("        get_connected_pins=lambda pin: pin.list_connected_pins(),"),
			TEXT("        get_pin_owner=lambda pin: pin.get_owning_node(),"),
			TEXT("        get_node_pos=lambda n: (n.get_node_pos().x, n.get_node_pos().y),"),
			TEXT("        set_node_pos=lambda n, x, y: n.set_node_pos(unreal.IntPoint(x, y)),"),
			TEXT("        get_node_size=lambda n: (int(n.get_node_size().x), int(n.get_node_size().y)),"),
			TEXT("    ).arrange(all_nodes, set(nodes))"),
			TEXT("    unreal.log('HyperAI arranged %d of %d new Blueprint node(s)' % (len(nodes), len(paths)))"),
		};
		TArray<FString> Encoded;
		for (const TArray<FString>& Group : Groups)
		{
			TArray<FString> Quoted;
			for (const FString& Path : Group)
			{
				Quoted.Add(TEXT("r'") + Path.Replace(TEXT("'"), TEXT("")) + TEXT("'"));
			}
			Encoded.Add(TEXT("[") + FString::Join(Quoted, TEXT(", ")) + TEXT("]"));
		}
		Lines.Insert(TEXT("GROUPS = [") + FString::Join(Encoded, TEXT(", ")) + TEXT("]"), 3);
		const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Tidy/arrange_nodes.py"));
		if (!FFileHelper::SaveStringToFile(FString::Join(Lines, TEXT("\n")) + TEXT("\n"), *File, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return false;
		}
		// ponytail: Epic's formatter is Python, run through the py command so this module needs no Python link; a C++
		// port would let the layout run without the Python plugin.
		return GEngine && GEngine->Exec(nullptr, *FString::Printf(TEXT("py \"%s\""), *File));
	}

	void RunPending()
	{
		TArray<TWeakObjectPtr<UBlueprint>> Pending = MoveTemp(State().Pending);
		State().Pending.Reset();
		for (const TWeakObjectPtr<UBlueprint>& Blueprint : Pending)
		{
			if (Blueprint.IsValid())
			{
				FHyperAIStudioBlueprintTidy::Tidy(Blueprint.Get());
			}
		}
	}

	void OnPreCompile(UBlueprint* Blueprint)
	{
		if (!Blueprint || !GetDefault<UHyperAIStudioSettings>()->bArrangeAgentBlueprintNodes)
		{
			return;
		}
		// After the compile, on the next tick: never inside the edit or the script that caused it.
		State().Pending.AddUnique(Blueprint);
		if (!State().Tick.IsValid())
		{
			State().Tick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
			{
				State().Tick.Reset();
				RunPending();
				return false;
			}));
		}
	}

	void OnObject(UObject* Object)
	{
		if (UBlueprint* Blueprint = Cast<UBlueprint>(Object))
		{
			FHyperAIStudioBlueprintTidy::Remember(Blueprint);
		}
	}
}

void FHyperAIStudioBlueprintTidy::Remember(UBlueprint* Blueprint)
{
	using namespace HyperAIStudio::BlueprintTidy::Private;
	if (!Blueprint)
	{
		return;
	}
	TMap<FGuid, TSet<FGuid>>& Graphs = State().Known.FindOrAdd(Blueprint);
	Graphs.Reset();
	for (UEdGraph* Graph : GraphsOf(Blueprint))
	{
		TSet<FGuid>& Nodes = Graphs.Add(Graph->GraphGuid);
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node)
			{
				Nodes.Add(Node->NodeGuid);
			}
		}
	}
}

int32 FHyperAIStudioBlueprintTidy::Tidy(UBlueprint* Blueprint)
{
	using namespace HyperAIStudio::BlueprintTidy::Private;
	if (!Blueprint)
	{
		return 0;
	}
	const TMap<FGuid, TSet<FGuid>>* Known = State().Known.Find(Blueprint);
	const bool bOpen = GEditor && GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->FindEditorForAsset(Blueprint, /*bFocusIfOpen=*/false) != nullptr;
	TArray<TArray<FString>> Groups;
	int32 Moved = 0;
	for (UEdGraph* Graph : GraphsOf(Blueprint))
	{
		const TSet<FGuid>* Before = Known ? Known->Find(Graph->GraphGuid) : nullptr;
		TArray<UEdGraphNode*> Added;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			// A Blueprint HyperAI never saw before: only nodes lying on others are known to be misplaced.
			const bool bNew = Known ? (!Before || !Before->Contains(Node->NodeGuid)) : Overlaps(*Graph, Node);
			if (Placeable(Node) && bNew)
			{
				Added.Add(Node);
			}
		}
		// In an open editor a person may have placed them: move them only if they lie on something.
		if (bOpen && !Added.ContainsByPredicate([Graph](const UEdGraphNode* Node) { return Overlaps(*Graph, Node); }))
		{
			Added.Reset();
		}
		if (Added.IsEmpty())
		{
			continue;
		}
		TArray<FString>& Paths = Groups.AddDefaulted_GetRef();
		for (const UEdGraphNode* Node : Added)
		{
			Paths.Add(Node->GetPathName());
		}
		Moved += Added.Num();
	}
	if (!Groups.IsEmpty())
	{
		if (Arrange(Groups))
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			UE_LOG(LogHyperAIStudioBlueprintTidy, Log, TEXT("Arranged %d new node(s) in %s."), Moved, *Blueprint->GetPathName());
		}
		else
		{
			UE_LOG(LogHyperAIStudioBlueprintTidy, Warning, TEXT("Could not arrange the new nodes of %s: Python and Epic's EditorToolset are needed."), *Blueprint->GetPathName());
			Moved = 0;
		}
	}
	Remember(Blueprint);
	return Moved;
}

void FHyperAIStudioBlueprintTidy::Startup()
{
	using namespace HyperAIStudio::BlueprintTidy::Private;
	FState& Current = State();
	Current.Loaded = FCoreUObjectDelegates::OnAssetLoaded.AddStatic(&OnObject);
	if (FAssetRegistryModule* Registry = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
	{
		Current.Created = Registry->Get().OnInMemoryAssetCreated().AddStatic(&OnObject);
	}
	// The module starts before the editor engine exists; the compile hook waits for it.
	auto Bind = []()
	{
		if (GEditor && !State().PreCompile.IsValid())
		{
			State().PreCompile = GEditor->OnBlueprintPreCompile().AddStatic(&OnPreCompile);
		}
	};
	if (GEditor)
	{
		Bind();
	}
	else
	{
		Current.EngineInit = FCoreDelegates::GetOnPostEngineInit().AddLambda(Bind);
	}
}

void FHyperAIStudioBlueprintTidy::Shutdown()
{
	using namespace HyperAIStudio::BlueprintTidy::Private;
	FState& Current = State();
	FCoreUObjectDelegates::OnAssetLoaded.Remove(Current.Loaded);
	if (FAssetRegistryModule* Registry = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
	{
		Registry->Get().OnInMemoryAssetCreated().Remove(Current.Created);
	}
	FCoreDelegates::GetOnPostEngineInit().Remove(Current.EngineInit);
	if (GEditor)
	{
		GEditor->OnBlueprintPreCompile().Remove(Current.PreCompile);
	}
	Current.PreCompile.Reset();
	if (Current.Tick.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(Current.Tick);
		Current.Tick.Reset();
	}
	Current.Known.Reset();
	Current.Pending.Reset();
}
