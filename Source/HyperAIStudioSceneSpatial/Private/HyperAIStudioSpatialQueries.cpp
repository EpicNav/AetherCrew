// Games by Hyper 2026.

#include "HyperAIStudioSpatialQueries.h"

#include "CollisionShape.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/ShapeComponent.h"
#include "Debug/DebugDrawComponent.h"
#include "Engine/Brush.h"
#include "Engine/CollisionProfile.h"
#include "Engine/HitResult.h"
#include "Engine/Light.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Info.h"
#include "GameFramework/WorldSettings.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioSceneSpatialToolset.h"
#include "InstancedFoliageActor.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "Misc/Paths.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UObject/UObjectHash.h"
#include "WorldPartition/HLOD/HLODActor.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionActorDescInstance.h"
#include "WorldPartition/WorldPartitionHelpers.h"

namespace HyperAIStudio::SceneSpatial::Spatial
{
	namespace
	{
		/** Candidate spots a clear_spot search tries before it gives up, and actors its bounds mode compares against. */
		constexpr int32 MaxCandidates = 2048;
		constexpr int32 MaxBoundsActors = 4096;
		/** Actors a nearest query reads. */
		constexpr int32 MaxNearestScan = 50000;

		float SlopeOf(const FVector& Normal)
		{
			return static_cast<float>(FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(Normal.Z, -1.0, 1.0))));
		}

		/**
		 * The first surface below Start. A trace that starts inside a mesh meets it at once, so what it starts inside is
		 * passed through, and bOutStartedInside says so.
		 */
		bool TraceDown(const UWorld& World, const FVector& Start, const FVector& End, const ECollisionChannel Channel, const FCollisionQueryParams& Params,
			FHitResult& OutHit, bool& bOutStartedInside)
		{
			auto IsInside = [](const FHitResult& Hit) { return Hit.bStartPenetrating || Hit.Distance <= UE_KINDA_SMALL_NUMBER; };
			bOutStartedInside = false;
			if (!World.LineTraceSingleByChannel(OutHit, Start, End, Channel, Params)) return false;
			if (!IsInside(OutHit)) return true;
			bOutStartedInside = true;
			FCollisionQueryParams Through = Params;
			for (int32 Attempt = 0; Attempt < 4; ++Attempt)
			{
				const UPrimitiveComponent* Inside = OutHit.GetComponent();
				if (!Inside) return false;
				Through.AddIgnoredComponent(Inside);
				if (!World.LineTraceSingleByChannel(OutHit, Start, End, Channel, Through)) return false;
				if (!IsInside(OutHit)) return true;
			}
			return false;
		}

		FHyperAISpatialHit ToHit(const FHitResult& Hit)
		{
			FHyperAISpatialHit Out;
			if (const AActor* Actor = Hit.GetActor())
			{
				Out.Actor = LabelOf(*Actor);
				Out.ActorPath = Actor->GetPathName();
			}
			if (const UPrimitiveComponent* Component = Hit.GetComponent())
			{
				Out.Component = Component->GetName();
				Out.Item = Component->IsA<UInstancedStaticMeshComponent>() ? Hit.Item : INDEX_NONE;
			}
			Out.PhysMaterial = Hit.PhysMaterial.IsValid() ? Hit.PhysMaterial->GetName() : FString();
			Out.Location = Hit.ImpactPoint;
			Out.Normal = Hit.ImpactNormal;
			Out.Distance = Hit.Distance;
			Out.SlopeDegrees = SlopeOf(Hit.ImpactNormal);
			Out.bStartPenetrating = Hit.bStartPenetrating;
			Out.bBlocking = Hit.bBlockingHit;
			return Out;
		}

		/**
		 * The editor world gives meshes set to NoCollision query-only bodies with every channel ignored, so they can be
		 * clicked. Channel queries never see them; an object-type query that meets one has not met collision, so the hit is dropped.
		 */
		bool IsEditorOnlyHit(const UPrimitiveComponent* Component)
		{
			return Component && Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision;
		}

		/**
		 * A shape that blocks no kind of object, such as a pickup's overlap sphere: never drawn, and nothing stands on it.
		 * Only the engine's object types are asked, since a channel a project never set up answers Block.
		 */
		bool IsTriggerShape(const UPrimitiveComponent& Component)
		{
			if (!Component.IsA<UShapeComponent>()) return false;
			if (!Component.IsCollisionEnabled()) return true;
			for (const ECollisionChannel Type : {ECC_WorldStatic, ECC_WorldDynamic, ECC_Pawn, ECC_PhysicsBody, ECC_Vehicle, ECC_Destructible})
			{
				if (Component.GetCollisionResponseToChannel(Type) == ECR_Block) return false;
			}
			return true;
		}

		FString Lower(const FString& Value)
		{
			return Value.TrimStartAndEnd().ToLower();
		}

		/** The world box of a box of half size Half about Center, turned by Turn. */
		FBox TurnedBox(const FVector& Center, const FVector& Half, const FQuat& Turn)
		{
			FBox Box(ForceInit);
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector Offset((Corner & 1) ? Half.X : -Half.X, (Corner & 2) ? Half.Y : -Half.Y, (Corner & 4) ? Half.Z : -Half.Z);
				Box += Center + Turn.RotateVector(Offset);
			}
			return Box;
		}

		/** What a bounds-mode clear spot search counts as in the way: placed things, not the ground, volumes, lights or level infrastructure. */
		bool IsObstacle(const AActor& Actor, const UClass* PartitionClass)
		{
			return !Actor.IsA<ALandscapeProxy>() && !Actor.IsA<ABrush>() && !Actor.IsA<AInfo>() && !Actor.IsA<ALight>()
				&& !Actor.IsA<AInstancedFoliageActor>() && !(PartitionClass && Actor.IsA(PartitionClass)) && !Actor.IsA<AWorldPartitionHLOD>();
		}

		bool MakeShape(const FHyperAISpatialQuery& Query, FCollisionShape& Out, FString& OutError)
		{
			const FString Shape = Lower(Query.Shape);
			const FVector& Extent = Query.Extent;
			if (Shape.IsEmpty() || Shape == TEXT("box"))
			{
				if (Extent.GetMin() <= 0.0)
				{
					OutError = TEXT("A box needs Extent: its half size in X, Y and Z, cm.");
					return false;
				}
				Out = FCollisionShape::MakeBox(Extent);
				return true;
			}
			if (Shape == TEXT("sphere"))
			{
				if (Extent.X <= 0.0)
				{
					OutError = TEXT("A sphere needs its radius in Extent X, cm.");
					return false;
				}
				Out = FCollisionShape::MakeSphere(Extent.X);
				return true;
			}
			if (Shape == TEXT("capsule"))
			{
				if (Extent.X <= 0.0 || Extent.Z <= 0.0)
				{
					OutError = TEXT("A capsule needs its radius in Extent X and its half height in Extent Z, cm.");
					return false;
				}
				Out = FCollisionShape::MakeCapsule(Extent.X, FMath::Max(Extent.Z, Extent.X));
				return true;
			}
			OutError = TEXT("Shape is box, sphere or capsule.");
			return false;
		}

		UClass* FindActorClass(const FString& Name)
		{
			const FString Wanted = Name.TrimStartAndEnd();
			// Each lookup makes an FName of the name, and of the name with _C, which stops the editor past 1023 characters.
			if (Wanted.IsEmpty() || !FHyperAIStudioNames::IsName(Wanted, TEXT("ClassFilter"))) return nullptr;
			UClass* Class = nullptr;
			if (Wanted.StartsWith(TEXT("/")))
			{
				if (!FHyperAIStudioNames::IsObjectPath(Wanted, TEXT("ClassFilter"))) return nullptr;
				Class = FindObject<UClass>(nullptr, *Wanted);
				if (!Class && !Wanted.Contains(TEXT(".")))
				{
					// A Blueprint by its asset path: its generated class.
					const FString Short = FPaths::GetBaseFilename(Wanted);
					Class = FindObject<UClass>(nullptr, *FString::Printf(TEXT("%s.%s_C"), *Wanted, *Short));
				}
			}
			else
			{
				Class = FindFirstObject<UClass>(*Wanted, EFindFirstObjectOptions::NativeFirst);
				if (!Class) Class = FindFirstObject<UClass>(*(Wanted + TEXT("_C")), EFindFirstObjectOptions::None);
				if (!Class && Wanted.StartsWith(TEXT("A"), ESearchCase::CaseSensitive)) Class = FindFirstObject<UClass>(*Wanted.Mid(1), EFindFirstObjectOptions::NativeFirst);
			}
			return Class && Class->IsChildOf(AActor::StaticClass()) ? Class : nullptr;
		}
	}

	bool CheckNames(const FHyperAISpatialQuery& Query, FString& OutError)
	{
		for (const FString& Name : Query.IgnoreActors)
		{
			if (!FHyperAIStudioNames::IsName(Name.TrimStartAndEnd(), TEXT("IgnoreActors"), &OutError)) return false;
		}
		return FHyperAIStudioNames::IsName(Query.ActorA.TrimStartAndEnd(), TEXT("ActorA"), &OutError)
			&& FHyperAIStudioNames::IsName(Query.ActorB.TrimStartAndEnd(), TEXT("ActorB"), &OutError)
			&& FHyperAIStudioNames::IsName(Query.ClassFilter.TrimStartAndEnd(), TEXT("ClassFilter"), &OutError)
			&& FHyperAIStudioNames::IsName(Query.TagFilter.TrimStartAndEnd(), TEXT("TagFilter"), &OutError);
	}

	FString LabelOf(const AActor& Actor)
	{
		return Actor.GetActorLabel();
	}

	FString FormatVector(const FVector& Value)
	{
		return FString::Printf(TEXT("(%.1f, %.1f, %.1f)"), Value.X, Value.Y, Value.Z);
	}

	FString FormatRotator(const FRotator& Value)
	{
		return FString::Printf(TEXT("%.1f/%.1f/%.1f"), Value.Pitch, Value.Yaw, Value.Roll);
	}

	bool ResolveChannel(const FString& Name, const ECollisionChannel Fallback, ECollisionChannel& OutChannel, FString& OutError)
	{
		FString Wanted = Name.TrimStartAndEnd().Replace(TEXT(" "), TEXT(""));
		if (Wanted.IsEmpty())
		{
			OutChannel = Fallback;
			return true;
		}
		Wanted.RemoveFromStart(TEXT("ECC_"), ESearchCase::IgnoreCase);
		// The profile's index lookup is not exported, so the names are compared one by one.
		const UCollisionProfile* Profile = UCollisionProfile::Get();
		for (int32 Index = 0; Profile && Index < ECC_MAX; ++Index)
		{
			const FName Channel = Profile->ReturnChannelNameFromContainerIndex(Index);
			if (!Channel.IsNone() && Channel.ToString().Replace(TEXT(" "), TEXT("")).Equals(Wanted, ESearchCase::IgnoreCase))
			{
				OutChannel = static_cast<ECollisionChannel>(Index);
				return true;
			}
		}
		OutError = FString::Printf(TEXT("%s is not a collision channel; use one of %s."), *Name, *DescribeChannels());
		return false;
	}

	FString DescribeChannels()
	{
		TArray<FString> Names;
		const UCollisionProfile* Profile = UCollisionProfile::Get();
		for (int32 Index = 0; Profile && Index < ECC_MAX; ++Index)
		{
			const FName Channel = Profile->ReturnChannelNameFromContainerIndex(Index);
			const FString Text = Channel.ToString();
			if (Channel.IsNone() || Text.StartsWith(TEXT("GameTraceChannel")) || Text.StartsWith(TEXT("EngineTraceChannel")) || Text.Contains(TEXT("Deprecated")))
			{
				continue;
			}
			Names.Add(Text);
		}
		return FString::Join(Names, TEXT(", "));
	}

	AActor* FindActor(UWorld& World, const FString& LabelOrPath, FString& OutError)
	{
		const FString Wanted = LabelOrPath.TrimStartAndEnd();
		if (Wanted.IsEmpty())
		{
			OutError = TEXT("An actor's label or path is empty.");
			return nullptr;
		}
		// No label or path is this long, and a lookup by it would make an FName past 1023 characters, which stops the editor.
		if (!FHyperAIStudioNames::IsName(Wanted, TEXT("An actor's label or path"), &OutError)) return nullptr;
		// A path the engine cannot look up (a double slash, say) is still compared as a label below.
		if (FHyperAIStudioNames::IsObjectPath(Wanted, nullptr))
		{
			AActor* ByPath = FindObject<AActor>(nullptr, *Wanted);
			if (IsValid(ByPath) && ByPath->GetWorld() == &World)
			{
				return ByPath;
			}
		}
		TArray<AActor*> Found;
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			if (IsValid(*It) && It->GetActorLabel().Equals(Wanted, ESearchCase::IgnoreCase))
			{
				Found.Add(*It);
			}
		}
		if (Found.Num() == 1)
		{
			return Found[0];
		}
		if (Found.Num() > 1)
		{
			OutError = FString::Printf(TEXT("%d actors are labelled %s (%s); name one by its path."), Found.Num(), *Wanted,
				*FString::JoinBy(Found, TEXT(", "), [](const AActor* Actor) { return Actor->GetPathName(); }));
			return nullptr;
		}
		bool bUnloaded = false;
		if (UWorldPartition* Partition = World.GetWorldPartition())
		{
			FWorldPartitionHelpers::ForEachActorDescInstance(Partition, AActor::StaticClass(), [&Wanted, &bUnloaded](const FWorldPartitionActorDescInstance* Desc)
			{
				bUnloaded = !Desc->IsLoaded() && (Desc->GetActorLabelString().Equals(Wanted, ESearchCase::IgnoreCase)
					|| Desc->GetActorSoftPath().ToString().Equals(Wanted, ESearchCase::IgnoreCase));
				return !bUnloaded;
			});
		}
		OutError = bUnloaded
			? FString::Printf(TEXT("%s is in an unloaded region; load it with hyper_world_partition_apply_plan (region.load)."), *Wanted)
			: FString::Printf(TEXT("No loaded actor is labelled %s."), *Wanted);
		return nullptr;
	}

	FName PCGGeneratedTag()
	{
		static const FName Tag(TEXT("PCG Generated Component"));
		return Tag;
	}

	UClass* PCGPartitionActorClass()
	{
		return FindObject<UClass>(nullptr, TEXT("/Script/PCG.PCGPartitionActor"));
	}

	bool IsPCG(const UPrimitiveComponent& Component, const UClass* PartitionClass)
	{
		const AActor* Owner = Component.GetOwner();
		return Component.ComponentHasTag(PCGGeneratedTag()) || (PartitionClass && Owner && Owner->IsA(PartitionClass));
	}

	bool IsFoliageOrPCG(const UPrimitiveComponent& Component)
	{
		const AActor* Owner = Component.GetOwner();
		return (Owner && Owner->IsA<AInstancedFoliageActor>()) || IsPCG(Component, PCGPartitionActorClass());
	}

	bool IgnoreFoliage(UWorld& World, FCollisionQueryParams& Params)
	{
		int32 Count = 0;
		for (TActorIterator<AInstancedFoliageActor> It(&World); It; ++It)
		{
			if (++Count > MaxIgnored) return false;
			Params.AddIgnoredActor(*It);
		}
		UClass* PartitionClass = PCGPartitionActorClass();
		for (TActorIterator<AActor> It(&World, PartitionClass); PartitionClass && It; ++It)
		{
			if (++Count > MaxIgnored) return false;
			Params.AddIgnoredActor(*It);
		}
		// A PCG volume generated in one piece keeps its instances on itself, tagged.
		bool bCapped = false;
		ForEachObjectOfClass(UInstancedStaticMeshComponent::StaticClass(), [&World, &Params, &Count, &bCapped, PartitionClass](UObject* Object)
		{
			const UInstancedStaticMeshComponent* Component = static_cast<const UInstancedStaticMeshComponent*>(Object);
			if (bCapped || !IsValid(Component) || Component->GetWorld() != &World || !Component->ComponentHasTag(PCGGeneratedTag()))
			{
				return;
			}
			const AActor* Owner = Component->GetOwner();
			if (Owner && (Owner->IsA<AInstancedFoliageActor>() || (PartitionClass && Owner->IsA(PartitionClass))))
			{
				return;
			}
			if (++Count > MaxIgnored)
			{
				bCapped = true;
				return;
			}
			Params.AddIgnoredComponent(static_cast<const UPrimitiveComponent*>(Component));
		});
		return !bCapped;
	}

	bool IsLandscapePending(UWorld& World)
	{
		for (TActorIterator<ALandscape> It(&World); It; ++It)
		{
			if (IsValid(*It) && !It->IsUpToDate()) return true;
		}
		return false;
	}

	FGround FindGround(const UWorld& World, const FVector& Point, const ECollisionChannel Channel, const FCollisionQueryParams& Params)
	{
		FGround Out;
		FHitResult Hit;
		// Below the point first, so a floor under a roof is found; then from 50 m up, for a point under the surface.
		bool bAbove = false;
		const bool bHit = TraceDown(World, Point + FVector(0.0, 0.0, 10.0), Point - FVector(0.0, 0.0, 50000.0), Channel, Params, Hit, Out.bStartedInside)
			|| TraceDown(World, Point + FVector(0.0, 0.0, 5000.0), Point - FVector(0.0, 0.0, 50000.0), Channel, Params, Hit, bAbove);
		if (!bHit) return Out;
		Out.bFound = true;
		Out.Point = Hit.ImpactPoint;
		Out.Normal = Hit.ImpactNormal;
		Out.Slope = SlopeOf(Hit.ImpactNormal);
		Out.Actor = Hit.GetActor();
		Out.Hit = Hit;
		return Out;
	}

	FString FClearSpot::Rejected() const
	{
		return FString::Printf(TEXT("blocked:%d, no_ground:%d, too_steep:%d"), Blocked, NoGround, TooSteep);
	}

	FClearSpot FindClearSpot(UWorld& World, const FClearSpotSettings& Settings)
	{
		FClearSpot Out;
		const FVector Half = Settings.Extent.ComponentMax(FVector(1.0)) + FVector(FMath::Max(Settings.Clearance, 0.f));
		const double Step = Settings.Step > 0.f ? Settings.Step : FMath::Max(50.0, 2.0 * FMath::Max(Settings.Extent.X, Settings.Extent.Y) + Settings.Clearance);
		const int32 Rings = FMath::FloorToInt32(FMath::Max(Settings.MaxRadius, 0.f) / Step);
		const FQuat Turn(FRotator(0.0, Settings.Yaw, 0.0));
		const FCollisionShape Shape = FCollisionShape::MakeBox(Half);
		const double MaxTan = FMath::Tan(FMath::DegreesToRadians(FMath::Clamp<double>(Settings.MaxSlope, 0.0, 89.0)));

		TArray<TPair<const AActor*, FBox>> Others;
		if (Settings.bBounds)
		{
			const double Reach = Settings.MaxRadius + Half.Size() + 100.0;
			const FBox Area(Settings.Point - FVector(Reach, Reach, Reach + 50000.0), Settings.Point + FVector(Reach, Reach, Reach + 50000.0));
			const UClass* PartitionClass = PCGPartitionActorClass();
			for (TActorIterator<AActor> It(&World); It && Others.Num() < MaxBoundsActors; ++It)
			{
				const AActor* Actor = *It;
				if (!IsValid(Actor) || Settings.IgnoreBounds.Contains(Actor) || !IsObstacle(*Actor, PartitionClass)) continue;
				const FBox Box = WorldBounds(*Actor);
				if (Box.IsValid && Box.Intersect(Area)) Others.Emplace(Actor, Box);
			}
		}

		// Ring 0 is the point itself; ring r has at least 8 spots, about Step apart, always tried in the same order.
		for (int32 Ring = 0; Ring <= Rings; ++Ring)
		{
			const double Radius = Ring * Step;
			const int32 Count = Ring == 0 ? 1 : FMath::Max(8, FMath::CeilToInt32(2.0 * UE_DOUBLE_PI * Radius / Step));
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (Out.Candidates >= MaxCandidates || (Settings.Deadline > 0.0 && FPlatformTime::Seconds() > Settings.Deadline))
				{
					Out.bDeadline = Out.Candidates < MaxCandidates;
					return Out;
				}
				++Out.Candidates;
				const double Angle = 2.0 * UE_DOUBLE_PI * Index / Count;
				FVector Floor(Settings.Point.X + Radius * FMath::Cos(Angle), Settings.Point.Y + Radius * FMath::Sin(Angle), Settings.Point.Z);
				FVector Normal = FVector::UpVector;
				const AActor* Ground = nullptr;
				if (Settings.bSnapToGround)
				{
					const FGround Found = FindGround(World, Floor, Settings.GroundChannel, Settings.GroundParams);
					// A spot whose point is inside something is in that thing's way, whatever lies under it.
					if (Found.bStartedInside)
					{
						++Out.Blocked;
						continue;
					}
					if (!Found.bFound)
					{
						++Out.NoGround;
						continue;
					}
					if (Found.Slope > Settings.MaxSlope)
					{
						++Out.TooSteep;
						continue;
					}
					Floor = Found.Point;
					Normal = Found.Normal;
					Ground = Found.Actor;
				}
				// Raised by the slope across the box and 2 cm more, so the ground itself is never what is in the way.
				const double Tan = Normal.Z > UE_KINDA_SMALL_NUMBER ? FMath::Sqrt(FMath::Max(0.0, 1.0 - Normal.Z * Normal.Z)) / Normal.Z : MaxTan;
				const double Lift = 2.0 + FVector2D(Half.X, Half.Y).Size() * FMath::Min(Tan, MaxTan);
				const FVector Center = Floor + FVector(0.0, 0.0, Half.Z + Lift);
				const FBox Space = TurnedBox(Center, Half, Turn);
				bool bBlocked = Settings.Claimed.ContainsByPredicate([&Space](const FBox& Taken) { return Taken.Intersect(Space); });
				if (!bBlocked && Settings.bCollision)
				{
					bBlocked = World.OverlapBlockingTestByChannel(Center, Turn, Settings.Channel, Shape, Settings.Params);
				}
				if (!bBlocked && Settings.bBounds)
				{
					bBlocked = Others.ContainsByPredicate([&Space, Ground](const TPair<const AActor*, FBox>& Other) { return Other.Key != Ground && Other.Value.Intersect(Space); });
				}
				if (bBlocked)
				{
					++Out.Blocked;
					continue;
				}
				Out.bFound = true;
				Out.Location = Floor;
				Out.Normal = Normal;
				Out.Slope = SlopeOf(Normal);
				Out.Box = Space;
				return Out;
			}
		}
		return Out;
	}

	bool IsHelperPrimitive(const UPrimitiveComponent& Component)
	{
		// Debug drawing covers subsystem overlays such as Smart Objects', whose bounds span the world.
		return Component.IsEditorOnly() || Component.IsVisualizationComponent() || Component.IsA<UDebugDrawComponent>();
	}

	FBox WorldBounds(const AActor& Actor)
	{
		// Trigger shapes count only for an actor that has nothing else, such as a trigger box.
		FBox Box(ForceInit);
		FBox Triggers(ForceInit);
		Actor.ForEachComponent<UPrimitiveComponent>(/*bIncludeFromChildActors=*/true, [&Box, &Triggers](const UPrimitiveComponent* Component)
		{
			if (Component->IsRegistered() && !IsHelperPrimitive(*Component))
			{
				(IsTriggerShape(*Component) ? Triggers : Box) += Component->Bounds.GetBox();
			}
		});
		return Box.IsValid ? Box : Triggers;
	}

	FBox LocalBounds(const AActor& Actor)
	{
		FBox Box(ForceInit);
		FBox Triggers(ForceInit);
		const FTransform WorldToActor = Actor.GetActorTransform().Inverse();
		Actor.ForEachComponent<UPrimitiveComponent>(/*bIncludeFromChildActors=*/true, [&Box, &Triggers, &WorldToActor](const UPrimitiveComponent* Component)
		{
			if (Component->IsRegistered() && !IsHelperPrimitive(*Component))
			{
				(IsTriggerShape(*Component) ? Triggers : Box) += Component->CalcBounds(Component->GetComponentTransform() * WorldToActor).GetBox();
			}
		});
		return Box.IsValid ? Box : Triggers;
	}

	bool PlanDrop(const UWorld& World, const FDropSubject& Subject, const FDropSettings& Settings, FTransform& OutTransform)
	{
		const FTransform& From = Subject.Transform;
		const FVector Pivot = From.GetLocation();
		const FVector Scale = From.GetScale3D();
		const bool bHasBox = Subject.LocalBox.IsValid != 0;
		// An actor with nothing visible drops by its pivot.
		const FBox WorldBox = bHasBox ? Subject.LocalBox.TransformBy(From) : FBox(Pivot, Pivot);
		const FVector Center = WorldBox.GetCenter();
		const FVector Half = WorldBox.GetExtent();

		TArray<FVector2D, TInlineAllocator<5>> Samples;
		Samples.Add(FVector2D(Center.X, Center.Y));
		if (Settings.bCorners && bHasBox)
		{
			// The corners inset by a tenth, so a trace does not slip down the side of a neighbour.
			for (const double SignX : {-1.0, 1.0})
			{
				for (const double SignY : {-1.0, 1.0})
				{
					Samples.Add(FVector2D(Center.X + SignX * 0.9 * Half.X, Center.Y + SignY * 0.9 * Half.Y));
				}
			}
		}
		const double Top = WorldBox.Max.Z + 10.0;
		const double Bottom = WorldBox.Min.Z - Settings.TraceDistance;
		int32 Hits = 0;
		FVector Contact = FVector::ZeroVector;
		FVector NormalSum = FVector::ZeroVector;
		for (const FVector2D& Sample : Samples)
		{
			FHitResult Hit;
			bool bInside = false;
			if (!TraceDown(World, FVector(Sample.X, Sample.Y, Top), FVector(Sample.X, Sample.Y, Bottom), Settings.Channel, Settings.Params, Hit, bInside))
			{
				continue;
			}
			if (Hits == 0 || Hit.ImpactPoint.Z > Contact.Z) Contact = Hit.ImpactPoint;
			NormalSum += Hit.ImpactNormal;
			++Hits;
		}
		if (Hits == 0) return false;
		FVector Normal = NormalSum.GetSafeNormal();
		if (Normal.Z < 0.05) Normal = FVector::UpVector;

		FQuat Rotation = From.GetRotation();
		if (Settings.bAlign)
		{
			// Turned onto the normal at most MaxAlignDegrees, keeping its yaw; its pitch and roll are replaced.
			FQuat Tilt = FQuat::FindBetweenNormals(FVector::UpVector, Normal);
			const double Angle = Tilt.GetAngle();
			const double Limit = FMath::DegreesToRadians(FMath::Clamp<double>(Settings.MaxAlignDegrees, 0.0, 89.0));
			if (Angle > Limit && Angle > UE_KINDA_SMALL_NUMBER)
			{
				Tilt = FQuat::Slerp(FQuat::Identity, Tilt, Limit / Angle);
			}
			Rotation = Tilt * FQuat(FRotator(0.0, From.Rotator().Yaw, 0.0));
			Rotation.Normalize();
		}

		FVector Location = Pivot;
		if (!bHasBox)
		{
			Location.Z = Contact.Z - Settings.Sink;
		}
		else
		{
			// The box's lowest point relative to the pivot, turned and scaled as it will be: along the surface's normal
			// when aligned, straight down otherwise.
			const FVector Down = Settings.bAlign ? Normal : FVector::UpVector;
			const FBox& Local = Subject.LocalBox;
			double Lowest = TNumericLimits<double>::Max();
			for (int32 Corner = 0; Corner < 8; ++Corner)
			{
				const FVector Point((Corner & 1) ? Local.Max.X : Local.Min.X, (Corner & 2) ? Local.Max.Y : Local.Min.Y, (Corner & 4) ? Local.Max.Z : Local.Min.Z);
				Lowest = FMath::Min(Lowest, FVector::DotProduct(Rotation.RotateVector(Point * Scale), Down));
			}
			if (Settings.bAlign)
			{
				// The pivot height that puts that point on the surface's plane through the contact, X and Y kept.
				Location.Z = Contact.Z + (-Settings.Sink - Lowest - (Pivot.X - Contact.X) * Normal.X - (Pivot.Y - Contact.Y) * Normal.Y) / Normal.Z;
			}
			else
			{
				Location.Z = Contact.Z - Lowest - Settings.Sink;
			}
		}
		OutTransform = FTransform(Rotation, Location, Scale);
		return true;
	}

	void Run(UWorld& World, const FHyperAISpatialQuery& Query, const double Deadline, FHyperAISpatialResult& Out)
	{
		const FString Kind = Lower(Query.Kind);
		Out.Kind = Kind;
		auto Fail = [&Out](const FString& Why)
		{
			Out.bOk = false;
			Out.Diagnostic = Why;
		};
		const bool bKnown = Kind == TEXT("trace") || Kind == TEXT("sweep") || Kind == TEXT("overlap") || Kind == TEXT("ground")
			|| Kind == TEXT("clear_spot") || Kind == TEXT("measure") || Kind == TEXT("nearest");
		if (!bKnown)
		{
			Fail(TEXT("Kind is trace, sweep, overlap, ground, clear_spot, measure or nearest."));
			return;
		}
		FString Error;
		FCollisionQueryParams Params(SCENE_QUERY_STAT(HyperAISpatialQuery), Query.bTraceComplex);
		Params.bReturnPhysicalMaterial = true;
		// Each name is looked for among every loaded actor, so the list is bounded.
		if (Query.IgnoreActors.Num() > FHyperAIStudioSceneSpatialContracts::MaxScopeActors)
		{
			Fail(FString::Printf(TEXT("IgnoreActors holds at most %d actors."), FHyperAIStudioSceneSpatialContracts::MaxScopeActors));
			return;
		}
		for (const FString& Name : Query.IgnoreActors)
		{
			const AActor* Ignored = FindActor(World, Name, Error);
			if (!Ignored)
			{
				Fail(TEXT("IgnoreActors: ") + Error);
				return;
			}
			Params.AddIgnoredActor(Ignored);
		}
		FString Note;
		if (Query.bIgnoreFoliage && !IgnoreFoliage(World, Params))
		{
			Note = FString::Printf(TEXT(" More than %d foliage and PCG actors and components; the rest were not passed through."), MaxIgnored);
		}
		const bool bOverlapKind = Kind == TEXT("overlap") || Kind == TEXT("clear_spot");
		ECollisionChannel Channel = ECC_Visibility;
		if (!ResolveChannel(Query.Channel, bOverlapKind ? ECC_WorldDynamic : ECC_Visibility, Channel, Error))
		{
			Fail(Error);
			return;
		}
		FCollisionObjectQueryParams Objects;
		for (const FString& Name : Query.ObjectTypes)
		{
			if (Name.TrimStartAndEnd().IsEmpty()) continue;
			ECollisionChannel Type = ECC_WorldStatic;
			if (!ResolveChannel(Name, ECC_WorldStatic, Type, Error))
			{
				Fail(Error);
				return;
			}
			if (UEngineTypes::ConvertToObjectType(Type) == ObjectTypeQuery_MAX)
			{
				Fail(FString::Printf(TEXT("%s is a trace channel, not an object type; give it as Channel instead."), *Name));
				return;
			}
			Objects.AddObjectTypesToQuery(Type);
		}
		const bool bByObject = Objects.IsValid();
		const int32 MaxResults = FMath::Clamp(Query.MaxResults <= 0 ? 8 : Query.MaxResults, 1, 32);

		if (Kind == TEXT("trace") || Kind == TEXT("sweep"))
		{
			if (Query.Start.Equals(Query.End, 0.01))
			{
				Fail(TEXT("Start and End are the same point."));
				return;
			}
			const bool bSweep = Kind == TEXT("sweep");
			FCollisionShape Shape;
			if (bSweep && !MakeShape(Query, Shape, Error))
			{
				Fail(Error);
				return;
			}
			const FQuat Rotation(Query.Rotation);
			TArray<FHitResult> Hits;
			if (bByObject)
			{
				if (bSweep) World.SweepMultiByObjectType(Hits, Query.Start, Query.End, Rotation, Objects, Shape, Params);
				else World.LineTraceMultiByObjectType(Hits, Query.Start, Query.End, Objects, Params);
				Hits.RemoveAll([](const FHitResult& Hit) { return IsEditorOnlyHit(Hit.GetComponent()); });
				if (!Query.bMulti && Hits.Num() > 1) Hits.SetNum(1);
			}
			else if (Query.bMulti)
			{
				if (bSweep) World.SweepMultiByChannel(Hits, Query.Start, Query.End, Rotation, Channel, Shape, Params);
				else World.LineTraceMultiByChannel(Hits, Query.Start, Query.End, Channel, Params);
			}
			else
			{
				FHitResult Hit;
				const bool bHit = bSweep ? World.SweepSingleByChannel(Hit, Query.Start, Query.End, Rotation, Channel, Shape, Params)
					: World.LineTraceSingleByChannel(Hit, Query.Start, Query.End, Channel, Params);
				if (bHit) Hits.Add(Hit);
			}
			const FHitResult* Stop = Hits.FindByPredicate([bByObject](const FHitResult& Hit) { return bByObject || Hit.bBlockingHit; });
			Out.bHit = Stop != nullptr;
			// Where the line, or the shape's center, comes to rest.
			Out.Location = Stop ? FVector(Stop->Location) : Query.End;
			for (int32 Index = 0; Index < Hits.Num() && Index < MaxHits; ++Index)
			{
				Out.Hits.Add(ToHit(Hits[Index]));
			}
			Out.bOk = true;
			Out.Diagnostic = (Stop ? FString::Printf(TEXT("Stopped by %s after %.1f cm."), Stop->GetActor() ? *LabelOf(*Stop->GetActor()) : TEXT("something"), Stop->Distance)
				: FString(TEXT("Nothing in the way."))) + Note;
			return;
		}
		if (Kind == TEXT("overlap"))
		{
			FCollisionShape Shape;
			if (!MakeShape(Query, Shape, Error))
			{
				Fail(Error);
				return;
			}
			const FQuat Rotation(Query.Rotation);
			TArray<FOverlapResult> Overlaps;
			if (bByObject)
			{
				World.OverlapMultiByObjectType(Overlaps, Query.Point, Rotation, Objects, Shape, Params);
				Overlaps.RemoveAll([](const FOverlapResult& Overlap) { return IsEditorOnlyHit(Overlap.GetComponent()); });
			}
			else
			{
				World.OverlapMultiByChannel(Overlaps, Query.Point, Rotation, Channel, Shape, Params);
			}
			// Blocking first, so an actor that both blocks and overlaps is listed as blocking.
			Overlaps.StableSort([](const FOverlapResult& A, const FOverlapResult& B) { return A.bBlockingHit && !B.bBlockingHit; });
			TSet<const AActor*> Seen;
			for (const FOverlapResult& Overlap : Overlaps)
			{
				const AActor* Actor = Overlap.GetActor();
				if (Actor && Seen.Contains(Actor)) continue;
				if (Out.Hits.Num() >= MaxResults)
				{
					Note += FString::Printf(TEXT(" More than %d actors overlap; the first %d are listed."), MaxResults, MaxResults);
					break;
				}
				Seen.Add(Actor);
				FHyperAISpatialHit& Hit = Out.Hits.AddDefaulted_GetRef();
				if (Actor)
				{
					Hit.Actor = LabelOf(*Actor);
					Hit.ActorPath = Actor->GetPathName();
					Hit.Location = Actor->GetActorLocation();
				}
				if (const UPrimitiveComponent* Component = Overlap.GetComponent())
				{
					Hit.Component = Component->GetName();
					Hit.Item = Component->IsA<UInstancedStaticMeshComponent>() ? Overlap.GetItemIndex() : INDEX_NONE;
				}
				Hit.bBlocking = bByObject || Overlap.bBlockingHit;
			}
			Out.bHit = bByObject ? !Overlaps.IsEmpty() : World.OverlapBlockingTestByChannel(Query.Point, Rotation, Channel, Shape, Params);
			Out.bOk = true;
			Out.Diagnostic = FString::Printf(TEXT("%d actor(s) overlap%s."), Out.Hits.Num(), Out.bHit ? TEXT(", something blocks") : TEXT(", nothing blocks")) + Note;
			return;
		}
		if (Kind == TEXT("ground"))
		{
			const FGround Ground = FindGround(World, Query.Point, Channel, Params);
			Out.bOk = true;
			Out.bHit = Ground.bFound;
			if (!Ground.bFound)
			{
				Out.Diagnostic = TEXT("No ground below the point within 500 m, nor above it within 50 m.") + Note;
				return;
			}
			Out.Location = Ground.Point;
			Out.GroundZ = Ground.Point.Z;
			Out.HeightAboveGround = Query.Point.Z - Ground.Point.Z;
			Out.Normal = Ground.Normal;
			Out.SlopeDegrees = Ground.Slope;
			Out.Hits.Add(ToHit(Ground.Hit));
			Out.Diagnostic = FString::Printf(TEXT("Ground at Z %.1f (%s), sloping %.1f degrees%s."), Ground.Point.Z,
				Ground.Actor ? *LabelOf(*Ground.Actor) : TEXT("no actor"), Ground.Slope,
				Ground.bStartedInside ? TEXT("; the point is inside something, and this is the surface under it") : TEXT("")) + Note;
			return;
		}
		if (Kind == TEXT("clear_spot"))
		{
			if (Query.Extent.GetMin() <= 0.0)
			{
				Fail(TEXT("clear_spot needs Extent: half the size of what must fit, cm."));
				return;
			}
			const FString Mode = Lower(Query.Mode);
			if (!Mode.IsEmpty() && Mode != TEXT("collision") && Mode != TEXT("bounds") && Mode != TEXT("both"))
			{
				Fail(TEXT("Mode is collision, bounds or both."));
				return;
			}
			FClearSpotSettings Settings;
			Settings.Point = Query.Point;
			Settings.Extent = Query.Extent;
			Settings.Yaw = Query.Rotation.Yaw;
			Settings.Clearance = FMath::Max(Query.Clearance, 0.f);
			Settings.Step = FMath::Max(Query.Step, 0.f);
			Settings.MaxRadius = FMath::Clamp(Query.MaxRadius, 0.f, 50000.f);
			Settings.MaxSlope = FMath::Clamp(Query.MaxSlope, 0.f, 89.f);
			Settings.bSnapToGround = Query.bSnapToGround;
			Settings.bCollision = Mode != TEXT("bounds");
			Settings.bBounds = Mode == TEXT("bounds") || Mode == TEXT("both");
			Settings.Channel = Channel;
			Settings.Params = Params;
			Settings.GroundParams = Params;
			Settings.Deadline = Deadline;
			const FClearSpot Spot = FindClearSpot(World, Settings);
			Out.Candidates = Spot.Candidates;
			Out.Rejected = Spot.Rejected();
			if (!Spot.bFound)
			{
				Fail(FString::Printf(TEXT("No clear spot within %.0f cm after %d tries (%s)%s."), Settings.MaxRadius, Spot.Candidates, *Out.Rejected,
					Spot.bDeadline ? TEXT("; the deadline stopped the search") : TEXT("")) + Note);
				return;
			}
			Out.bOk = true;
			Out.bHit = true;
			Out.Location = Spot.Location;
			Out.Rotation = FRotator(0.0, Settings.Yaw, 0.0);
			Out.GroundZ = Spot.Location.Z;
			Out.Normal = Spot.Normal;
			Out.SlopeDegrees = Spot.Slope;
			Out.Distance = FVector::Dist2D(Spot.Location, Query.Point);
			Out.Diagnostic = FString::Printf(TEXT("Clear at %s, %.0f cm from the point, after %d tries."), *FormatVector(Spot.Location), Out.Distance, Spot.Candidates) + Note;
			return;
		}
		if (Kind == TEXT("measure"))
		{
			FVector A = Query.Point;
			FVector B = Query.End;
			FBox BoxA(A, A);
			FBox BoxB(B, B);
			auto Resolve = [&World, &Error](const FString& Name, FVector& Point, FBox& Box)
			{
				if (Name.TrimStartAndEnd().IsEmpty()) return true;
				const AActor* Actor = FindActor(World, Name, Error);
				if (!Actor) return false;
				Point = Actor->GetActorLocation();
				const FBox Bounds = WorldBounds(*Actor);
				Box = Bounds.IsValid ? Bounds : FBox(Point, Point);
				return true;
			};
			if (!Resolve(Query.ActorA, A, BoxA) || !Resolve(Query.ActorB, B, BoxB))
			{
				Fail(Error);
				return;
			}
			Out.bOk = true;
			Out.Distance = FVector::Dist(A, B);
			Out.Horizontal = FVector::Dist2D(A, B);
			Out.Vertical = B.Z - A.Z;
			Out.Gap = FMath::Sqrt(BoxA.ComputeSquaredDistanceToBox(BoxB));
			Out.Diagnostic = FString::Printf(TEXT("%.1f cm apart (%.1f across, %.1f up), %.1f cm of free space between them."), Out.Distance, Out.Horizontal, Out.Vertical, Out.Gap);
			return;
		}

		// nearest
		const double Radius = Query.Radius > 0.f ? Query.Radius : 5000.0;
		UClass* Class = AActor::StaticClass();
		if (!Query.ClassFilter.TrimStartAndEnd().IsEmpty())
		{
			if (!FHyperAIStudioNames::IsName(Query.ClassFilter.TrimStartAndEnd(), TEXT("ClassFilter"), &Error))
			{
				Fail(Error);
				return;
			}
			Class = FindActorClass(Query.ClassFilter);
			if (!Class)
			{
				Fail(FString::Printf(TEXT("%s is not a loaded actor class; give its name (StaticMeshActor) or path (/Script/Engine.StaticMeshActor)."), *Query.ClassFilter));
				return;
			}
		}
		const FString TagText = Query.TagFilter.TrimStartAndEnd();
		if (!FHyperAIStudioNames::IsName(TagText, TEXT("TagFilter"), &Error))
		{
			Fail(Error);
			return;
		}
		// Found, not added: a tag no actor has is no name yet, so every unmatched filter does not stay in the name table.
		const FName Tag = TagText.IsEmpty() ? NAME_None : FName(*TagText, FNAME_Find);
		const bool bTagUnmatched = !TagText.IsEmpty() && Tag.IsNone();
		const FString LabelText = Query.LabelFilter.TrimStartAndEnd();
		const ABrush* DefaultBrush = World.GetDefaultBrush();
		int32 Scanned = 0;
		bool bCapped = false;
		for (TActorIterator<AActor> It(&World, Class); It; ++It)
		{
			if (++Scanned > MaxNearestScan)
			{
				bCapped = true;
				break;
			}
			const AActor* Actor = *It;
			if (!IsValid(Actor) || Actor == DefaultBrush || Actor->IsA<AWorldSettings>()) continue;
			if (bTagUnmatched || (!Tag.IsNone() && !Actor->ActorHasTag(Tag))) continue;
			if (!LabelText.IsEmpty() && !LabelOf(*Actor).Contains(LabelText)) continue;
			const FBox Box = WorldBounds(*Actor);
			const double Distance = Box.IsValid ? FMath::Sqrt(Box.ComputeSquaredDistanceToPoint(Query.Point)) : FVector::Dist(Actor->GetActorLocation(), Query.Point);
			if (Distance > Radius) continue;
			// Kept sorted by distance, then label, and no longer than MaxResults.
			FHyperAISpatialActor Entry;
			Entry.Label = LabelOf(*Actor);
			Entry.Path = Actor->GetPathName();
			Entry.Class = Actor->GetClass()->GetName();
			Entry.Location = Actor->GetActorLocation();
			Entry.Distance = static_cast<float>(Distance);
			Entry.Size = Box.IsValid ? Box.GetSize() : FVector::ZeroVector;
			int32 At = 0;
			while (At < Out.Nearest.Num() && (Out.Nearest[At].Distance < Entry.Distance
				|| (Out.Nearest[At].Distance == Entry.Distance && Out.Nearest[At].Label <= Entry.Label)))
			{
				++At;
			}
			if (At >= MaxResults) continue;
			Out.Nearest.Insert(MoveTemp(Entry), At);
			if (Out.Nearest.Num() > MaxResults) Out.Nearest.SetNum(MaxResults);
		}
		if (UWorldPartition* Partition = World.GetWorldPartition())
		{
			const FBox Area(Query.Point - FVector(Radius), Query.Point + FVector(Radius));
			int32 Unloaded = 0;
			FWorldPartitionHelpers::ForEachIntersectingActorDescInstance(Partition, Area, AActor::StaticClass(), [&Unloaded](const FWorldPartitionActorDescInstance* Desc)
			{
				Unloaded += Desc->IsLoaded() ? 0 : 1;
				return true;
			});
			Out.UnloadedNearby = Unloaded;
		}
		Out.bOk = true;
		Out.bHit = !Out.Nearest.IsEmpty();
		Out.Diagnostic = FString::Printf(TEXT("%d actor(s) within %.0f cm%s%s."), Out.Nearest.Num(), Radius,
			bCapped ? *FString::Printf(TEXT("; only the first %d actors were read"), MaxNearestScan) : TEXT(""),
			Out.UnloadedNearby > 0 ? *FString::Printf(TEXT("; %d more nearby are in unloaded regions"), Out.UnloadedNearby) : TEXT(""));
	}

	int64 EstimateBytes(const FHyperAISpatialQueryReport& Report)
	{
		auto Bytes = [](const FString& Value) { return 16 + 2ll * Value.Len(); };
		int64 Size = 200 + Bytes(Report.Diagnostic) + Bytes(Report.LevelPath);
		for (const FHyperAISpatialResult& Result : Report.Results)
		{
			Size += 320 + Bytes(Result.Kind) + Bytes(Result.Diagnostic) + Bytes(Result.Rejected);
			for (const FHyperAISpatialHit& Hit : Result.Hits)
			{
				Size += 160 + Bytes(Hit.Actor) + Bytes(Hit.ActorPath) + Bytes(Hit.Component) + Bytes(Hit.PhysMaterial);
			}
			for (const FHyperAISpatialActor& Actor : Result.Nearest)
			{
				Size += 120 + Bytes(Actor.Label) + Bytes(Actor.Path) + Bytes(Actor.Class);
			}
		}
		return Size;
	}

	void RunAll(UWorld& World, const FHyperAISpatialQueryRequest& Request, FHyperAISpatialQueryReport& Out)
	{
		const double Deadline = FPlatformTime::Seconds() + Request.DeadlineMs / 1000.0;
		Out.LevelPath = World.GetOutermost()->GetName();
		int32 Answered = 0;
		for (const FHyperAISpatialQuery& Query : Request.Queries)
		{
			FHyperAISpatialResult& Result = Out.Results.AddDefaulted_GetRef();
			if (FPlatformTime::Seconds() > Deadline)
			{
				Result.Kind = Lower(Query.Kind);
				Result.Diagnostic = TEXT("deadline_reached: not answered; ask again with fewer queries or a longer DeadlineMs.");
				Out.bTruncated = true;
				continue;
			}
			Run(World, Query, Deadline, Result);
			++Answered;
		}
		Out.bLandscapePending = IsLandscapePending(World);
		Out.bOk = true;
		Out.Status = TEXT("completed");
		bool bCut = false;
		auto Describe = [&Out, &bCut, &Request, Answered]()
		{
			Out.Diagnostic = FString::Printf(TEXT("%d of %d queries answered%s%s."), Answered, Request.Queries.Num(),
				bCut ? TEXT("; hits were cut to fit MaxOutputBytes") : TEXT(""),
				Out.bLandscapePending ? TEXT("; a landscape is still merging its edit layers, so ground found on it may lag") : TEXT(""));
		};
		Describe();
		// Cut hits and actors from the last answers first until the reply fits.
		while (EstimateBytes(Out) > Request.MaxOutputBytes)
		{
			FHyperAISpatialResult* Last = nullptr;
			for (int32 Index = Out.Results.Num() - 1; Index >= 0 && !Last; --Index)
			{
				if (!Out.Results[Index].Hits.IsEmpty() || !Out.Results[Index].Nearest.IsEmpty()) Last = &Out.Results[Index];
			}
			if (!Last) break;
			if (Last->Nearest.Num() > Last->Hits.Num()) Last->Nearest.Pop();
			else Last->Hits.Pop();
			if (!bCut)
			{
				bCut = true;
				Out.bTruncated = true;
				Describe();
			}
		}
	}
}
