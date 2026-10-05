// Games by Hyper 2026.

#include "HyperAIStudioModelingHandoff.h"

#include "Algo/Sort.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CollisionQueryParams.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/ComponentEditorUtils.h"
#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "LevelUtils.h"
#include "Misc/App.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"
#include "ScopedTransaction.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Tools/EdModeInteractiveToolsContext.h"
#include "Tools/Modes.h"
#include "Tools/UEdMode.h"
#include "UObject/Package.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioModeling"

namespace HyperAIStudio::Modeling::Handoff
{
	using C = FHyperAIStudioModelingContracts;

	namespace
	{
		/**
		 * A Modeling Mode tool the reply can point a person at. Each edits the selected mesh in place, so an Accept changes
		 * the asset itself; tools that make new meshes (Boolean, Duplicate) are left out. In 5.8 the ids are the private
		 * strings ModelingToolsEditorMode.cpp registers (606-783), the palettes ModelingToolsEditorModeToolkit.cpp's
		 * (1717-1737, 1918-1992), the buttons ModelingToolsManagerActions.cpp's labels.
		 */
		struct FToolHint
		{
			const TCHAR* Name;
			const TCHAR* Id;
			const TCHAR* Palette;
			const TCHAR* Button;
			const TCHAR* For;
		};

		constexpr FToolHint ToolHints[] = {
			{TEXT("PolyEdit"), TEXT("BeginPolyEditTool"), TEXT("PolyModel"), TEXT("PolyGroup Edit"), TEXT("faces, edges and vertices by polygroup: extrude, inset, bevel, cut")},
			{TEXT("TriEdit"), TEXT("BeginTriEditTool"), TEXT("TriModel"), TEXT("Triangle Edit"), TEXT("single triangles, edges and vertices")},
			{TEXT("PolyGroups"), TEXT("BeginPolyGroupsTool"), TEXT("Attributes"), TEXT("Generate PolyGroups"), TEXT("new polygroups, so PolyGroup Edit picks sensible faces")},
			{TEXT("Sculpt"), TEXT("BeginSculptMeshTool"), TEXT("Deform"), TEXT("Vertex Sculpt"), TEXT("sculpting with brushes")},
			{TEXT("Smooth"), TEXT("BeginSmoothMeshTool"), TEXT("Deform"), TEXT("Smooth"), TEXT("smoothing the surface")},
			{TEXT("Remesh"), TEXT("BeginRemeshMeshTool"), TEXT("MeshOps"), TEXT("Remesh"), TEXT("even triangles at a new density")},
			{TEXT("Simplify"), TEXT("BeginSimplifyMeshTool"), TEXT("MeshOps"), TEXT("Simplify"), TEXT("fewer triangles")},
			{TEXT("UVs"), TEXT("BeginGlobalUVGenerateTool"), TEXT("UVs"), TEXT("AutoUV"), TEXT("new UVs")},
		};

		const FToolHint* FindToolHint(const FString& Name)
		{
			for (const FToolHint& Hint : ToolHints)
			{
				if (Name.Equals(Hint.Name, ESearchCase::IgnoreCase)) return &Hint;
			}
			return nullptr;
		}

		FString ToolNames()
		{
			TArray<FString> Names;
			for (const FToolHint& Hint : ToolHints) Names.Add(Hint.Name);
			return FString::Join(Names, TEXT(", "));
		}

#if WITH_DEV_AUTOMATION_TESTS
		struct FHooks
		{
			bool bAllowUnattended = false;
			FName ModeId;
		};

		FHooks& Hooks()
		{
			static FHooks Value;
			return Value;
		}
#endif

		bool AllowsUnattended()
		{
#if WITH_DEV_AUTOMATION_TESTS
			return Hooks().bAllowUnattended;
#else
			return false;
#endif
		}

		/** A test named another mode: the plugin is not asked about, only whether that mode is registered. */
		bool IsModeHooked()
		{
#if WITH_DEV_AUTOMATION_TESTS
			return !Hooks().ModeId.IsNone();
#else
			return false;
#endif
		}

		FName ModeId()
		{
#if WITH_DEV_AUTOMATION_TESTS
			if (IsModeHooked()) return Hooks().ModeId;
#endif
			return FName(C::ModelingModeId);
		}

		/** A hand-off this session: the asset as it was handed over. */
		struct FRecord
		{
			FString Fingerprint;
			int32 Triangles = 0;
			FString Actor;
			FDateTime When;
		};

		constexpr int32 MaxRecords = 32;

		/** By the mesh's object path; game thread only. */
		TMap<FString, FRecord>& Records()
		{
			static TMap<FString, FRecord> Value;
			return Value;
		}

		void Remember(const FString& AssetPath, const FRecord& Record)
		{
			TMap<FString, FRecord>& All = Records();
			All.Add(AssetPath, Record);
			while (All.Num() > MaxRecords)
			{
				FString Oldest;
				FDateTime OldestWhen = FDateTime::MaxValue();
				for (const TPair<FString, FRecord>& Entry : All)
				{
					if (Entry.Value.When < OldestWhen)
					{
						OldestWhen = Entry.Value.When;
						Oldest = Entry.Key;
					}
				}
				All.Remove(Oldest);
			}
		}

		/** Whole centimeters, "x,y,z". */
		FString Vec(const FVector& Value)
		{
			auto Num = [](const double Part)
			{
				const double Rounded = FMath::RoundToDouble(Part);
				return FString::Printf(TEXT("%.0f"), FMath::IsNearlyZero(Rounded) ? 0.0 : Rounded);
			};
			return Num(Value.X) + TEXT(",") + Num(Value.Y) + TEXT(",") + Num(Value.Z);
		}

		bool Refuse(FHyperAIModelHandoffReport& Report, const TCHAR* Status, const FString& Why)
		{
			Report.bOk = false;
			Report.Status = Status;
			Report.Diagnostic = Why;
			return false;
		}

		/** What a hand-off resolved its request to; it lives for one call on the game thread. */
		struct FTarget
		{
			UStaticMesh* Mesh = nullptr;
			/** The actor handed over as it stands; null when one is put in the level. */
			AActor* Actor = nullptr;
			FString Label;
			FVector Location = FVector::ZeroVector;
			bool bPlace = false;
			int32 SharedBy = 0;
			const FToolHint* Tool = nullptr;
			Gate::FGeometryPrint Print;
			TArray<FString> Notes;
		};

		bool IsSelectable(AActor& Actor)
		{
			return GEditor->CanSelectActor(&Actor, /*bInSelected=*/true, /*bSelectEvenIfHidden=*/true, /*bWarnIfLevelLocked=*/false) && !Actor.GetRootSelectionParent();
		}

		/** Modeling Mode's own input filter (ModelingToolsEditorMode.cpp:330-351 in 5.8): a construction script's component is not a target. */
		bool IsEditable(const UStaticMeshComponent& Component)
		{
			const USceneComponent* Parent = Component.GetAttachParent();
			if (Parent && Parent->GetOwner() != Component.GetOwner()) Parent = nullptr;
			return FComponentEditorUtils::CanEditComponentInstance(&Component, Parent, /*bAllowUserContructionScript=*/true);
		}

		/** The actor's one plain static mesh component with a mesh, or null; how many plain and instanced ones it has. */
		UStaticMeshComponent* PlainMeshComponent(AActor& Actor, int32& OutPlain, int32& OutInstanced)
		{
			OutPlain = 0;
			OutInstanced = 0;
			UStaticMeshComponent* Found = nullptr;
			TInlineComponentArray<UStaticMeshComponent*> Components(&Actor);
			for (UStaticMeshComponent* Component : Components)
			{
				if (!Component || !Component->GetStaticMesh()) continue;
				if (Component->IsA<UInstancedStaticMeshComponent>())
				{
					++OutInstanced;
					continue;
				}
				++OutPlain;
				Found = Component;
			}
			return OutPlain == 1 ? Found : nullptr;
		}

		bool Shows(AActor& Actor, const UStaticMesh& Mesh)
		{
			TInlineComponentArray<UStaticMeshComponent*> Components(&Actor);
			return Components.ContainsByPredicate([&Mesh](const UStaticMeshComponent* Component) { return Component && Component->GetStaticMesh() == &Mesh; });
		}

		bool IsLabelTaken(UWorld& World, const FString& Label)
		{
			for (TActorIterator<AActor> It(&World); It; ++It)
			{
				if (IsValid(*It) && It->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase)) return true;
			}
			return false;
		}

		bool IsLabelWellFormed(const FString& Label)
		{
			if (Label.IsEmpty() || Label.Len() > 64) return false;
			for (const TCHAR Char : Label)
			{
				if (!FChar::IsAlnum(Char) || Char > 127)
				{
					if (Char != TEXT('_') && Char != TEXT('-')) return false;
				}
			}
			return true;
		}

		/** A person's tool open in Modeling Mode or the level editor's tools context: its button or id; empty when none is. */
		FString OpenTool()
		{
			FEditorModeTools& Tools = GLevelEditorModeTools();
			TArray<UEditorInteractiveToolsContext*, TInlineAllocator<3>> Contexts;
			if (UEdMode* Mode = Tools.GetActiveScriptableMode(ModeId()))
			{
				Contexts.Add(Mode->GetInteractiveToolsContext(EToolsContextScope::EdMode));
				Contexts.Add(Mode->GetInteractiveToolsContext(EToolsContextScope::Editor));
			}
			Contexts.Add(Tools.GetInteractiveToolsContext());
			for (UEditorInteractiveToolsContext* Context : Contexts)
			{
				if (!Context || !Context->HasActiveTool()) continue;
				const FString Id = Context->GetActiveToolName();
				for (const FToolHint& Hint : ToolHints)
				{
					if (Id == Hint.Id) return Hint.Button;
				}
				return Id.IsEmpty() ? FString(TEXT("a tool")) : Id;
			}
			return FString();
		}

		/** Where the level viewport looks: what it hits within HandoffTraceDistance, else a few times the mesh's size ahead. */
		bool LookAt(UWorld& World, const UStaticMesh& Mesh, FVector& OutLocation, TArray<FString>& OutNotes)
		{
			const FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
			if (!Client || !Client->IsPerspective())
			{
				OutLocation = FVector::ZeroVector;
				OutNotes.Add(TEXT("No perspective level viewport to look from, so the actor goes at the origin."));
				return true;
			}
			const FVector From = Client->GetViewLocation();
			const FVector Direction = Client->GetViewRotation().Vector();
			FHitResult Hit;
			const FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAIModelHandoff), /*bTraceComplex=*/true);
			const FVector At = World.LineTraceSingleByChannel(Hit, From, From + Direction * C::HandoffTraceDistance, ECC_Visibility, Params)
				? Hit.ImpactPoint : From + Direction * FMath::Clamp(Mesh.GetBounds().SphereRadius * 3.0, 200.0, C::HandoffTraceDistance);
			OutLocation = FVector(FMath::RoundToDouble(At.X), FMath::RoundToDouble(At.Y), FMath::RoundToDouble(At.Z));
			return OutLocation.GetAbsMax() <= C::MaxHandoffCoordinate;
		}

		/** The mesh behind an Asset path: under /Game, a static mesh with its source mesh; a redirector is followed. */
		UStaticMesh* ResolveAsset(const FString& Requested, FHyperAIModelHandoffReport& Report)
		{
			const FString Path = Requested.TrimStartAndEnd();
			const FString NotGame = TEXT(" is not under /Game: Modeling Mode would change content other projects share. Copy it into /Game and hand over the copy.");
			if (!Path.StartsWith(TEXT("/Game/")))
			{
				Refuse(Report, TEXT("invalid_request"), Path + NotGame);
				return nullptr;
			}
			const FString ObjectPath = Path.Contains(TEXT(".")) ? Path : Path + TEXT(".") + FPackageName::GetShortName(Path);
			IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
			const FAssetData Data = Registry.GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
			UStaticMesh* Mesh = FindObject<UStaticMesh>(nullptr, *ObjectPath);
			if (!Data.IsValid() && !Mesh)
			{
				Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("No asset at %s."), *Path));
				return nullptr;
			}
			if (!Mesh && !Data.IsRedirector() && Data.AssetClassPath != UStaticMesh::StaticClass()->GetClassPathName())
			{
				Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("%s is a %s, not a static mesh: only static meshes go to Modeling Mode."), *Path,
					*Data.AssetClassPath.GetAssetName().ToString()));
				return nullptr;
			}
			// Never loaded from a path with nothing on disk: the failed load would leave an empty package behind.
			if (!Mesh && FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(ObjectPath)))
			{
				Mesh = LoadObject<UStaticMesh>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
			}
			if (!Mesh)
			{
				Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("%s does not load as a static mesh: only static meshes go to Modeling Mode."), *Path));
				return nullptr;
			}
			// A redirector leads elsewhere, which must be under /Game too.
			if (!Mesh->GetPathName().StartsWith(TEXT("/Game/")))
			{
				Refuse(Report, TEXT("invalid_request"), Mesh->GetPathName() + NotGame);
				return nullptr;
			}
			return Mesh;
		}

		bool HasSource(UStaticMesh& Mesh)
		{
			return Mesh.GetNumSourceModels() > 0 && Mesh.GetMeshDescription(0) != nullptr;
		}

		/**
		 * Every check a hand-off makes, in order, and the target it resolves to. A refusal sets Status and Diagnostic and
		 * returns false; nothing is changed either way.
		 */
		bool Prepare(const FHyperAIModelHandoffRequest& Request, FTarget& Target, FHyperAIModelHandoffReport& Report)
		{
			// The editor: Modeling Mode needs a person at an interactive editor with a level and a viewport.
			// (Editor.h's GLevelEditorModeToolsIsValid is declared but not defined in 5.8; GLevelEditorModeTools is made on first use.)
			if (!GEditor || IsRunningCommandlet() || !FSlateApplication::IsInitialized())
			{
				return Refuse(Report, TEXT("unavailable"), TEXT("No interactive Unreal Editor is running here (a commandlet, or no editor window): Modeling Mode needs one. Nothing was changed."));
			}
			if (FApp::IsUnattended() && !AllowsUnattended())
			{
				return Refuse(Report, TEXT("unavailable"), TEXT("This editor runs unattended, so nobody is at it to edit the mesh. Hand it over in an editor a person is using. Nothing was changed."));
			}
			// Modeling Mode refuses its tools then anyway (UEdMode::ShouldToolStartBeAllowed).
			if (GEditor->PlayWorld || GEditor->IsPlaySessionInProgress())
			{
				return Refuse(Report, TEXT("invalid_request"), TEXT("Play in Editor is running; stop it, then ask again. Nothing was changed."));
			}
			UWorld* World = GEditor->GetEditorWorldContext().World();
			if (!World)
			{
				return Refuse(Report, TEXT("unavailable"), TEXT("No level is open in the editor; open one, then ask again. Nothing was changed."));
			}
			if (GEditor->GetLevelViewportClients().IsEmpty())
			{
				return Refuse(Report, TEXT("unavailable"), TEXT("The level editor has no viewport to show the mesh in; open the level editor, then ask again. Nothing was changed."));
			}
			if (!IsModeHooked())
			{
				const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(C::ModelingModePlugin);
				if (!Plugin.IsValid() || !Plugin->IsEnabled())
				{
					return Refuse(Report, TEXT("unavailable"),
						TEXT("Modeling Mode is not available: enable the Modeling Tools Editor Mode plugin (Edit > Plugins, Beta) and restart the editor. Nothing was changed."));
				}
			}
			FEditorModeInfo Info;
			const UAssetEditorSubsystem* Editors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			if (!Editors || !Editors->FindEditorModeInfo(ModeId(), Info))
			{
				return Refuse(Report, TEXT("unavailable"), FString::Printf(TEXT("Modeling Mode is not available: its mode (%s) is not registered in this editor. Nothing was changed."),
					*ModeId().ToString()));
			}
			// Changing the selection under an open tool could lose the person's work.
			const FString Open = OpenTool();
			if (!Open.IsEmpty())
			{
				return Refuse(Report, TEXT("unavailable"), FString::Printf(TEXT("Modeling Mode has a tool open (%s); the person should Accept or Cancel it first, then ask again. Nothing was changed."),
					*Open));
			}

			// The request.
			const FString AssetPath = Request.Asset.TrimStartAndEnd();
			const FString ActorName = Request.Actor.TrimStartAndEnd();
			const FString LocationText = Request.Location.TrimStartAndEnd();
			const FString Label = Request.Label.TrimStartAndEnd();
			if (AssetPath.IsEmpty() == ActorName.IsEmpty())
			{
				return Refuse(Report, TEXT("invalid_request"), AssetPath.IsEmpty() ? TEXT("Give Asset (a static mesh under /Game) or Actor (an actor in the open level).")
					: TEXT("Give Asset (a static mesh under /Game) or Actor (an actor in the open level), not both."));
			}
			const FString ToolName = Request.Tool.TrimStartAndEnd();
			if (!ToolName.IsEmpty())
			{
				Target.Tool = FindToolHint(ToolName);
				if (!Target.Tool)
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("Tool '%s' is not one of %s."), *ToolName, *ToolNames()));
				}
			}
			if (!LocationText.IsEmpty())
			{
				TArray<double> Values;
				if (!Gate::ParseNumbers(LocationText, Values) || Values.Num() != 3)
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("Location '%s' is not x,y,z in cm."), *LocationText));
				}
				Target.Location = FVector(Values[0], Values[1], Values[2]);
				if (Target.Location.GetAbsMax() > C::MaxHandoffCoordinate)
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("Location %s is more than 20 km from the origin."), *LocationText));
				}
			}
			if (!Label.IsEmpty())
			{
				if (!IsLabelWellFormed(Label))
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("Label '%s' is not 1 to 64 letters, digits, _ or -."), *Label));
				}
				if (IsLabelTaken(*World, Label))
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("An actor is labelled %s already; give another Label, or leave it empty."), *Label));
				}
			}
			if (!ActorName.IsEmpty() && (!LocationText.IsEmpty() || !Label.IsEmpty()))
			{
				return Refuse(Report, TEXT("invalid_request"), TEXT("Location and Label put a new actor in the level; with Actor, the one named is handed over where it stands."));
			}

			if (!ActorName.IsEmpty())
			{
				// An actor in the open level, by label, else by name or full path.
				TArray<AActor*> Labelled;
				AActor* Named = nullptr;
				for (TActorIterator<AActor> It(World); It; ++It)
				{
					AActor* Actor = *It;
					if (!IsValid(Actor) || Actor->IsTemplate()) continue;
					if (Actor->GetActorLabel().Equals(ActorName, ESearchCase::IgnoreCase)) Labelled.Add(Actor);
					else if (!Named && Actor->GetName().Equals(ActorName, ESearchCase::IgnoreCase)) Named = Actor;
				}
				if (Labelled.IsEmpty() && !Named && ActorName.StartsWith(TEXT("/")))
				{
					AActor* Found = FindObject<AActor>(nullptr, *ActorName);
					if (IsValid(Found) && Found->GetWorld() == World) Named = Found;
				}
				if (Labelled.Num() > 1)
				{
					TArray<FString> Paths;
					for (int32 Index = 0; Index < Labelled.Num() && Index < 5; ++Index) Paths.Add(Labelled[Index]->GetPathName());
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("%d actors are labelled %s: %s. Give one's path as Actor."), Labelled.Num(), *ActorName,
						*FString::Join(Paths, TEXT(", "))));
				}
				AActor* Actor = Labelled.IsEmpty() ? Named : Labelled[0];
				if (!Actor)
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("No actor in the open level is labelled or named %s."), *ActorName));
				}
				const FString Shown = Actor->GetActorLabel();
				int32 Plain = 0;
				int32 Instanced = 0;
				UStaticMeshComponent* Component = PlainMeshComponent(*Actor, Plain, Instanced);
				if (Plain == 0 && Instanced == 0)
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("%s shows no static mesh, so there is nothing for Modeling Mode to edit."), *Shown));
				}
				if (Plain == 0)
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("%s shows its mesh as instances; Modeling Mode edits a plain static mesh component. Hand over the asset (Asset) instead, which puts one in the level."), *Shown));
				}
				if (!Component)
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("%s has %d static mesh components; Modeling Mode edits one mesh at a time. Hand over the asset (Asset) instead."), *Shown, Plain));
				}
				UStaticMesh* Mesh = Component->GetStaticMesh();
				if (!Mesh->GetPathName().StartsWith(TEXT("/Game/")))
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("%s shows %s, which is not under /Game: Modeling Mode would change content other projects share. Copy it into /Game and hand over the copy."),
						*Shown, *Mesh->GetPathName()));
				}
				if (!HasSource(*Mesh))
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("%s has no source mesh (it was cooked, or imported without one), so Modeling Mode cannot edit it."), *Mesh->GetPathName()));
				}
				if (!IsEditable(*Component))
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("%s's static mesh component is made by its Blueprint's construction script, which Modeling Mode cannot edit; hand over the asset (Asset) instead."), *Shown));
				}
				if (Actor->IsHiddenEd())
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("%s is hidden in the editor; show it, or hand over the asset (Asset) instead."), *Shown));
				}
				if (!IsSelectable(*Actor))
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("%s cannot be selected (its level is locked or hidden, or it belongs to another actor); unlock it, or hand over the asset (Asset) instead."), *Shown));
				}
				Target.Mesh = Mesh;
				Target.Actor = Actor;
				Target.Label = Shown;
				Target.Location = Actor->GetActorLocation();
			}
			else
			{
				Target.Mesh = ResolveAsset(AssetPath, Report);
				if (!Target.Mesh) return false;
				if (!HasSource(*Target.Mesh))
				{
					return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("%s has no source mesh (it was cooked, or imported without one), so Modeling Mode cannot edit it."), *AssetPath));
				}
				// An actor already showing it is used as it stands, unless the request asks for a new one.
				Target.bPlace = !LocationText.IsEmpty() || !Label.IsEmpty();
				if (!Target.bPlace)
				{
					TArray<AActor*> Usable;
					for (TActorIterator<AActor> It(World); It; ++It)
					{
						AActor* Actor = *It;
						if (!IsValid(Actor) || Actor->IsTemplate() || Actor->IsHiddenEd()) continue;
						int32 Plain = 0;
						int32 Instanced = 0;
						const UStaticMeshComponent* Component = PlainMeshComponent(*Actor, Plain, Instanced);
						if (Component && Component->GetStaticMesh() == Target.Mesh && IsEditable(*Component) && IsSelectable(*Actor)) Usable.Add(Actor);
					}
					// The hand-off's own actor first, then by label, so the same level always gives the same one.
					const FString Preferred = FString(C::HandoffLabelPrefix) + Target.Mesh->GetName();
					Algo::Sort(Usable, [](const AActor* A, const AActor* B) { return A->GetActorLabel() < B->GetActorLabel(); });
					AActor* const* Own = Usable.FindByPredicate([&Preferred](const AActor* Actor) { return Actor->GetActorLabel().Equals(Preferred, ESearchCase::IgnoreCase); });
					Target.Actor = Own ? *Own : Usable.IsEmpty() ? nullptr : Usable[0];
					Target.bPlace = Target.Actor == nullptr;
					if (Target.Actor)
					{
						Target.Label = Target.Actor->GetActorLabel();
						Target.Location = Target.Actor->GetActorLocation();
					}
				}
				if (Target.bPlace)
				{
					ULevel* Level = World->GetCurrentLevel();
					if (!Level || FLevelUtils::IsLevelLocked(Level) || !FLevelUtils::IsLevelVisible(Level))
					{
						return Refuse(Report, TEXT("invalid_request"), TEXT("The current level is locked or hidden, so an actor put there could not be selected; unlock or show it, or make another level current. Nothing was changed."));
					}
					Target.Label = Label;
					if (Target.Label.IsEmpty())
					{
						const FString Base = (FString(C::HandoffLabelPrefix) + Target.Mesh->GetName()).Left(64);
						Target.Label = Base;
						for (int32 Suffix = 2; IsLabelTaken(*World, Target.Label); ++Suffix)
						{
							if (Suffix > 99)
							{
								return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("Actors are labelled %s to %s_99 already; give a Label."), *Base, *Base));
							}
							Target.Label = FString::Printf(TEXT("%s_%d"), *Base.Left(60), Suffix);
						}
					}
					if (LocationText.IsEmpty() && !LookAt(*World, *Target.Mesh, Target.Location, Target.Notes))
					{
						return Refuse(Report, TEXT("invalid_request"), FString::Printf(TEXT("The level viewport looks at %s, more than 20 km from the origin; give Location."), *Vec(Target.Location)));
					}
				}
			}

			// What the reply says about the mesh.
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				if (IsValid(*It) && !It->IsTemplate() && *It != Target.Actor && Shows(**It, *Target.Mesh)) ++Target.SharedBy;
			}
			Target.Print = Gate::Fingerprint(*Target.Mesh);
			if (Target.Print.Triangles > C::HeavyHandoffTriangles)
			{
				Target.Notes.Add(FString::Printf(TEXT("%s has %d triangles: Modeling Mode's tools are slow on it; Simplify or Remesh first."), *Target.Mesh->GetName(), Target.Print.Triangles));
			}
			if (Target.Mesh->GetPackage()->IsDirty())
			{
				Target.Notes.Add(FString::Printf(TEXT("%s has unsaved changes already; saving after the edit keeps those too."), *Target.Mesh->GetName()));
			}
			if (Target.Mesh->GetPackage()->GetMetaData().FindValue(Target.Mesh, C::PlanHashKey))
			{
				Target.Notes.Add(TEXT("Made by hyper_model_apply_plan: a Replace write of that path would overwrite the person's edit; build on the edited asset instead."));
			}
			return true;
		}

		void Fill(const FTarget& Target, FHyperAIModelHandoffReport& Report)
		{
			Report.Asset = Target.Mesh->GetPathName();
			Report.ActorLabel = Target.Label;
			Report.ActorPath = Target.Actor ? Target.Actor->GetPathName() : FString();
			Report.bPlaced = Target.bPlace;
			Report.Location = Vec(Target.Location);
			if (Target.Tool) Report.ToolHint = FString::Printf(TEXT("%s > %s"), Target.Tool->Palette, Target.Tool->Button);
			Report.Fingerprint = Target.Print.Hash;
			Report.Triangles = Target.Print.Triangles;
			Report.Vertices = Target.Print.Vertices;
			Report.SharedBy = Target.SharedBy;
			Report.Notes = Target.Notes;
		}

		AStaticMeshActor* Spawn(UWorld& World, UStaticMesh& Mesh, const FString& Label, const FVector& Location)
		{
			FActorSpawnParameters Params;
			Params.ObjectFlags |= RF_Transactional;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(Location), Params);
			if (!Actor) return nullptr;
			// A registered static component refuses a new mesh, so it is set the way the mesh actor factory does.
			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->UnregisterComponent();
			Component->SetStaticMesh(&Mesh);
			Component->RegisterComponent();
			Actor->SetActorLabel(Label);
			Actor->SetFolderPath(FName(C::HandoffFolder));
			return Actor;
		}

		/**
		 * The level editor's viewport tab brought in front of HyperAI Studio's: its tab manager brings the level editor's
		 * major tab forward too (TabManager.cpp:1477-1516 in 5.8). No tab is opened or closed, so the layout is not saved.
		 */
		FString ShowLevelViewport(TArray<FString>& OutNotes)
		{
			FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
			const TSharedPtr<SDockTab> EditorTab = LevelEditor ? LevelEditor->GetLevelEditorTab() : nullptr;
			if (!EditorTab)
			{
				OutNotes.Add(TEXT("No level editor tab was found: bring the level editor forward by hand to see the mesh."));
				return TEXT("no_level_editor_tab");
			}
			const TSharedPtr<FTabManager> Inner = LevelEditor->GetLevelEditorTabManager();
			const TSharedPtr<SDockTab> ViewportTab = Inner ? Inner->FindExistingLiveTab(FTabId(LevelEditorTabIds::LevelEditorViewport)) : nullptr;
			if (EditorTab->IsForeground() && (!ViewportTab || ViewportTab->IsForeground()))
			{
				return TEXT("already_in_front");
			}
			if (ViewportTab) ViewportTab->DrawAttention();
			else EditorTab->DrawAttention();
			OutNotes.Add(TEXT("HyperAI Studio is behind the level editor now; its tab is beside it (Ctrl+Alt+H brings it back)."));
			return TEXT("brought_to_front");
		}

		void Record(const FString& OperationId, const FHyperAIModelHandoffReport& Done)
		{
			FHyperAIStudioActivityEntry Entry;
			Entry.Kind = Done.bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
			Entry.PackId = C::PackId;
			Entry.ToolName = C::HandoffToolName;
			Entry.Target = Done.Asset.Left(200);
			Entry.OperationId = OperationId;
			Entry.StatusCode = Done.bOk ? TEXT("completed") : TEXT("failed");
			Entry.Detail = Done.Diagnostic.Left(4000);
			FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
		}

		FString PickTool(const FToolHint* Tool)
		{
			return Tool ? FString::Printf(TEXT("Open the %s palette and click %s (%s)."), Tool->Palette, Tool->Button, Tool->For)
				: FString(TEXT("Pick a tool from Modeling Mode's palettes: PolyModel > PolyGroup Edit for faces and edges, Deform > Vertex Sculpt to sculpt, MeshOps > Remesh or Simplify for its triangles."));
		}

		/** The hand-off itself, on the game thread: the request checked again (after an approval the editor may have moved on), then placed, opened, selected and framed. */
		FHyperAIModelHandoffReport Execute(const FHyperAIModelHandoffRequest& Resolved, const FString& OperationId)
		{
			FHyperAIModelHandoffReport Report;
			Report.OperationId = OperationId;
			FTarget Target;
			if (!Prepare(Resolved, Target, Report)) return Report;
			Fill(Target, Report);
			UWorld* World = GEditor->GetEditorWorldContext().World();
			AActor* Actor = Target.Actor;
			if (Target.bPlace)
			{
				FScopedTransaction Transaction(LOCTEXT("ModelHandoff", "HyperAI: Place for Modeling Mode"));
				Actor = Spawn(*World, *Target.Mesh, Target.Label, Target.Location);
				if (!Actor)
				{
					Transaction.Cancel();
					Report.Status = TEXT("operation_failed");
					Report.Diagnostic = FString::Printf(TEXT("%s could not be put in the level. Nothing was changed."), *Target.Label);
					return Report;
				}
			}
			Report.ActorPath = Actor->GetPathName();
			const FString Placed = Target.bPlace ? TEXT("; it was put in the level: Edit > Undo takes it away") : TEXT("");

			// Mode, selection, cameras and tabs are none of them undo steps; the reply reports them.
			FEditorModeTools& Tools = GLevelEditorModeTools();
			const FName Id = ModeId();
			if (Tools.IsModeActive(Id)) Report.Notes.Add(TEXT("Modeling Mode was open already."));
			else Tools.ActivateMode(Id);
			Report.bModeActive = Tools.IsModeActive(Id);

			const int32 Before = GEditor->GetSelectedActorCount() - (Actor->IsSelected() ? 1 : 0);
			GEditor->SelectNone(/*bNoteSelectionChange=*/false, /*bDeselectBSPSurfs=*/true, /*WarnAboutManyActors=*/false);
			GEditor->SelectActor(Actor, /*bInSelected=*/true, /*bNotify=*/false, /*bSelectEvenIfHidden=*/true);
			GEditor->NoteSelectionChange();
			Report.bSelected = GEditor->GetSelectedActorCount() == 1 && Actor->IsSelected();
			if (Before > 0) Report.Notes.Add(FString::Printf(TEXT("%d other selected actor(s) were deselected."), Before));
			if (!Report.bModeActive)
			{
				Report.Status = TEXT("operation_failed");
				Report.Diagnostic = FString::Printf(TEXT("Modeling Mode did not start (the Output Log may say why). %s is %s%s. Open Modeling Mode from the level editor toolbar's mode menu."),
					*Target.Label, Report.bSelected ? TEXT("selected") : TEXT("not selected"), *Placed);
				return Report;
			}
			if (!Report.bSelected)
			{
				Report.Status = TEXT("operation_failed");
				Report.Diagnostic = FString::Printf(TEXT("Modeling Mode is open, but %s could not be selected (its level may be locked); select it in the Outliner to edit it%s."), *Target.Label, *Placed);
				return Report;
			}

			GEditor->MoveViewportCamerasToActor(*Actor, /*bActiveViewportOnly=*/false);
			// The viewports MoveViewportCamerasToBox moves (EditorServer.cpp:5207-5214 in 5.8): not those locked to an actor, unless current.
			for (const FLevelEditorViewportClient* Client : GEditor->GetLevelViewportClients())
			{
				if (Client && (!Client->IsAnyActorLocked() || Client == GCurrentLevelEditingViewportClient)) ++Report.FramedViewports;
			}
			GEditor->RedrawLevelEditingViewports();
			Report.Viewport = Resolved.bLeaveTabsAlone ? TEXT("left_alone") : ShowLevelViewport(Report.Notes);

			// The check the palette's button makes to enable itself (UEdMode::RegisterTool, UEdMode.cpp:114-118 in 5.8). A start
			// is only queued until the viewport ticks, and a hidden viewport does not, so nothing is started here.
			if (Target.Tool)
			{
				UEdMode* Mode = Tools.GetActiveScriptableMode(Id);
				UEditorInteractiveToolsContext* Context = Mode ? Mode->GetInteractiveToolsContext() : nullptr;
				Report.bToolReady = Context && Mode->ShouldToolStartBeAllowed(Target.Tool->Id) && Context->CanStartTool(Target.Tool->Id);
				if (!Report.bToolReady)
				{
					Report.Notes.Add(FString::Printf(TEXT("Modeling Mode says %s cannot start on this selection; the person can still pick another tool."), Target.Tool->Button));
				}
			}

			FRecord Handed;
			Handed.Fingerprint = Target.Print.Hash;
			Handed.Triangles = Target.Print.Triangles;
			Handed.Actor = Target.Label;
			Handed.When = FDateTime::Now();
			Remember(Target.Mesh->GetPathName(), Handed);

			const FString AssetName = Target.Mesh->GetName();
			if (!FApp::IsUnattended())
			{
				const FText ToolPart = Target.Tool ? FText::Format(LOCTEXT("HandoffToastTool", "Open {0} > {1}. "), FText::FromString(Target.Tool->Palette),
					FText::FromString(Target.Tool->Button)) : FText::GetEmpty();
				FNotificationInfo Toast(FText::Format(LOCTEXT("HandoffToast", "HyperAI: {0} is selected in Modeling Mode. {1}Edit it, click Accept, then save {2}."),
					FText::FromString(Target.Label), ToolPart, FText::FromString(AssetName)));
				Toast.ExpireDuration = 8.0f;
				FSlateNotificationManager::Get().AddNotification(Toast);
			}

			const FString Shared = Target.SharedBy > 0 ? FString::Printf(TEXT(", and the %d other actor(s) showing it change too"), Target.SharedBy) : FString();
			const FString Print = Target.Print.Hash.IsEmpty() ? FString::Printf(TEXT("no fingerprint: %s"), *Target.Print.Why)
				: FString::Printf(TEXT("Fingerprint was %s, %d triangles"), *Target.Print.Hash, Target.Print.Triangles);
			Report.NextSteps = {
				FString::Printf(TEXT("Modeling Mode is open in the level editor with %s selected."), *Target.Label),
				PickTool(Target.Tool),
				FString::Printf(TEXT("Edit, then click Accept: it writes the edit into %s itself%s. Cancel leaves it as it was. Tools that make new meshes (Boolean, Duplicate) write new assets: tell the agent their paths."),
					*Report.Asset, *Shared),
				FString::Printf(TEXT("Save %s: File > Save All."), *AssetName),
				TEXT("Leave Modeling Mode from the level editor toolbar's mode menu (Selection Mode)."),
				FString::Printf(TEXT("Then the agent re-reads it: hyper_model_inspect with Asset (%s) and hyper_model_validate."), *Print),
			};
			const FString Where = Target.bPlace ? FString::Printf(TEXT("was put at %s (Edit > Undo takes it away)"), *Report.Location) : FString(TEXT("was used where it stands"));
			const FString Tabs = Report.Viewport == TEXT("brought_to_front") || Report.Viewport == TEXT("already_in_front") ? TEXT("; the level editor is in front of HyperAI Studio")
				: Report.Viewport == TEXT("left_alone") ? TEXT("; the tabs were left as they are") : TEXT("; bring the level editor forward to see it");
			const FString Then = Target.Print.Hash.IsEmpty() ? FString::Printf(TEXT("No fingerprint (%s)"), *Target.Print.Why)
				: FString::Printf(TEXT("Fingerprint %s (%d triangles)"), *Target.Print.Hash, Target.Print.Triangles);
			Report.bOk = true;
			Report.Status = TEXT("completed");
			Report.Diagnostic = FString::Printf(TEXT("%s is in Modeling Mode: %s %s and is the only actor selected, framed in %d level viewport(s)%s. Next: %s Edit, click Accept, then save %s (File > Save All). %s: hyper_model_inspect with Asset reads it again afterwards, and hyper_model_validate checks it."),
				*AssetName, *Target.Label, *Where, Report.FramedViewports, *Tabs, *PickTool(Target.Tool), *AssetName, *Then);
			return Report;
		}
	}

	FString DescribeSince(const FString& AssetPath, const Gate::FGeometryPrint& Now)
	{
		const FRecord* Then = Records().Find(AssetPath);
		if (!Then) return FString();
		const FString Was = FString::Printf(TEXT("Handed to Modeling Mode at %s as %s (%d triangles, actor %s)"), *Then->When.ToString(TEXT("%H:%M:%S")),
			Then->Fingerprint.IsEmpty() ? TEXT("no fingerprint") : *Then->Fingerprint, Then->Triangles, *Then->Actor);
		if (Now.Hash.IsEmpty() || Then->Fingerprint.IsEmpty())
		{
			return Was + TEXT("; without a fingerprint, whether it changed since cannot be told.");
		}
		return Now.Hash == Then->Fingerprint ? Was + TEXT("; unchanged since.")
			: FString::Printf(TEXT("%s; it has changed since: now %s (%d triangles)."), *Was, *Now.Hash, Now.Triangles);
	}
}

FHyperAIModelHandoffReport FHyperAIStudioModelingContracts::Handoff(const FHyperAIModelHandoffRequest& Request)
{
	using namespace HyperAIStudio::Modeling::Handoff;
	FHyperAIModelHandoffReport Report;
	Report.OperationId = Request.OperationId.IsEmpty()
		? FString::Printf(TEXT("handoff-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Request.OperationId;
	FTarget Target;
	if (!Prepare(Request, Target, Report)) return Report;
	Fill(Target, Report);
	if (Request.bDryRun)
	{
		Report.bOk = true;
		Report.Status = TEXT("checked");
		const FString Use = Target.bPlace ? FString::Printf(TEXT("put %s at %s (one undo step)"), *Target.Label, *Report.Location)
			: FString::Printf(TEXT("use %s where it stands"), *Target.Label);
		Report.Diagnostic = FString::Printf(TEXT("Would %s, select only it, frame it, %s and open Modeling Mode on it. Nothing was changed."), *Use,
			Request.bLeaveTabsAlone ? TEXT("leave the tabs as they are") : TEXT("bring the level editor in front of HyperAI Studio"));
		return Report;
	}
	// What runs after an approval is the placement that was reviewed, though the camera may move while it waits.
	FHyperAIModelHandoffRequest Resolved = Request;
	if (Target.bPlace)
	{
		Resolved.Label = Target.Label;
		Resolved.Location = Report.Location;
	}
	// Only a placement changes the level; using an actor that is there changes nothing, so it runs straight away.
	if (Target.bPlace && FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIStudioApprovalSummary Summary;
		Summary.PackId = PackId;
		Summary.ToolName = HandoffToolName;
		Summary.VariantId = TEXT("model.handoff.v1");
		Summary.Safety = EHyperAIStudioDomainSafety::Edit;
		const FTCHARToUTF8 Canonical(*FString::Printf(TEXT("handoff|%s|%s|%s"), *Report.Asset, *Target.Label, *Report.Location));
		Summary.PlanHash = FSHA1::HashBuffer(Canonical.Get(), Canonical.Length()).ToString().Left(16).ToLower();
		Summary.EffectTarget = Report.Asset;
		Summary.Effects.Add(FString::Printf(TEXT("Hand-off: put %s showing %s at %s (one undo step)"), *Target.Label, *Report.Asset, *Report.Location));
		Summary.Effects.Add(TEXT("| then select only it, frame it and open Modeling Mode (selection and mode are not undo steps)"));
		if (const UWorld* World = GEditor->GetEditorWorldContext().World()) Summary.Touches.Add(World->GetPackage()->GetName());
		const FString OperationId = Report.OperationId;
		FString Error;
		if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Resolved, OperationId]()
			{
				Record(OperationId, Execute(Resolved, OperationId));
			}, Error))
		{
			Report.bOk = false;
			Report.Status = TEXT("approval_not_queued");
			Report.Diagnostic = Error;
			return Report;
		}
		Report.bOk = true;
		Report.Status = TEXT("awaiting_user_approval");
		Report.Diagnostic = FString::Printf(TEXT("Putting %s in the level needs approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true."),
			*Target.Label);
		return Report;
	}
	FHyperAIModelHandoffReport Done = Execute(Resolved, Report.OperationId);
	Record(Report.OperationId, Done);
	return Done;
}

#if WITH_DEV_AUTOMATION_TESTS
void FHyperAIStudioModelingContracts::SetHandoffTestHooks(const bool bAllowUnattended, const FName ModeId)
{
	HyperAIStudio::Modeling::Handoff::Hooks().bAllowUnattended = bAllowUnattended;
	HyperAIStudio::Modeling::Handoff::Hooks().ModeId = ModeId;
}
#endif

#undef LOCTEXT_NAMESPACE
