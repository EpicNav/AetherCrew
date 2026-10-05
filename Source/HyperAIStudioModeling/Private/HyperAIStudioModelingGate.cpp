// Games by Hyper 2026.

#include "HyperAIStudioModelingGate.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetUtils/CreateStaticMeshUtil.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "GeometryScript/CollisionFunctions.h"
#include "GeometryScript/GeometryScriptSelectionTypes.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "GeometryScript/MeshAssetFunctions.h"
#include "GeometryScript/MeshBasicEditFunctions.h"
#include "GeometryScript/MeshBooleanFunctions.h"
#include "GeometryScript/MeshDeformFunctions.h"
#include "GeometryScript/MeshMaterialFunctions.h"
#include "GeometryScript/MeshModelingFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshQueryFunctions.h"
#include "GeometryScript/MeshRemeshFunctions.h"
#include "GeometryScript/MeshRepairFunctions.h"
#include "GeometryScript/MeshSamplingFunctions.h"
#include "GeometryScript/MeshSelectionFunctions.h"
#include "GeometryScript/MeshSimplifyFunctions.h"
#include "GeometryScript/MeshSubdivideFunctions.h"
#include "GeometryScript/MeshTransformFunctions.h"
#include "GeometryScript/MeshUVFunctions.h"
#include "GeometryScript/MeshVertexColorFunctions.h"
#include "GeometryScript/MeshVoxelFunctions.h"
#include "Hash/xxhash.h"
#include "HyperAIStudioSourceControl.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionVertexColor.h"
#include "Math/RandomStream.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "ObjectTools.h"
#include "PhysicsEngine/BodySetup.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshCompiler.h"
#include "StaticMeshResources.h"
#include "UDynamicMesh.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"

namespace HyperAIStudio::Modeling::Gate
{
	using C = FHyperAIStudioModelingContracts;

	/** The fields an op kind takes, beside Mesh, which every kind names. */
	namespace Field
	{
		constexpr uint32 Source = 1 << 0;
		constexpr uint32 Location = 1 << 1;
		constexpr uint32 Rotation = 1 << 2;
		constexpr uint32 Scale = 1 << 3;
		constexpr uint32 Size = 1 << 4;
		constexpr uint32 Points = 1 << 5;
		constexpr uint32 Path = 1 << 6;
		constexpr uint32 Select = 1 << 7;
		constexpr uint32 Asset = 1 << 8;
		constexpr uint32 Placed = Location | Rotation | Scale;
	}

	enum class EParam : uint8 { Int, Float, Bool, Choice, Vector, Color, Text };

	struct FParamSpec
	{
		const TCHAR* Name;
		EParam Type;
		double Min;
		double Max;
		/** Choice: the words, |-separated. Text: what it takes. */
		const TCHAR* Choices;
		/** The default, as inspect shows it; empty when the param is required. */
		const TCHAR* Default;
	};

	struct FKindSpec
	{
		const TCHAR* Kind;
		uint32 Required;
		uint32 Allowed;
		TArray<FParamSpec> Params;
		const TCHAR* What;
	};

	/** Placeholders read as light stone until coloured. */
	const TCHAR* const DefaultColorText = TEXT("#b3b3b3");

	FParamSpec P(const TCHAR* Name, const EParam Type, const double Min, const double Max, const TCHAR* Default, const TCHAR* Choices = TEXT(""))
	{
		return {Name, Type, Min, Max, Choices, Default};
	}

	FParamSpec Required(const TCHAR* Name, const EParam Type, const double Min, const double Max, const TCHAR* Choices = TEXT(""))
	{
		return {Name, Type, Min, Max, Choices, TEXT("")};
	}

	/** A primitive's own params, and the ones every primitive takes. */
	TArray<FParamSpec> Shape(TArray<FParamSpec> Own, const bool bOrigin = true)
	{
		Own.Add(P(TEXT("Material"), EParam::Int, 0, 15, TEXT("0")));
		Own.Add(P(TEXT("Color"), EParam::Color, 0, 0, DefaultColorText));
		Own.Add(P(TEXT("Groups"), EParam::Choice, 0, 0, TEXT("face"), TEXT("face|quad|single")));
		if (bOrigin) Own.Add(P(TEXT("Origin"), EParam::Choice, 0, 0, TEXT("base"), TEXT("base|center")));
		return Own;
	}

	const TArray<FKindSpec>& Specs()
	{
		using namespace Field;
		static const TArray<FKindSpec> List = {
			{TEXT("add.box"), 0, Placed | Size, Shape({P(TEXT("Steps"), EParam::Int, 0, 64, TEXT("0"))}), TEXT("a box, Size cm (100 by default)")},
			{TEXT("add.sphere"), 0, Placed | Size, Shape({P(TEXT("Steps"), EParam::Int, 3, 128, TEXT("16 around, or 8 per side with Style=box")),
				P(TEXT("Style"), EParam::Choice, 0, 0, TEXT("latlong"), TEXT("latlong|box"))}), TEXT("a sphere or ellipsoid filling Size; Style=box spreads triangles evenly, best before noise")},
			{TEXT("add.cylinder"), 0, Placed | Size, Shape({P(TEXT("Steps"), EParam::Int, 3, 128, TEXT("16")), P(TEXT("HeightSteps"), EParam::Int, 0, 64, TEXT("0")),
				P(TEXT("Capped"), EParam::Bool, 0, 0, TEXT("true"))}), TEXT("a cylinder: Size X and Y across, Z tall")},
			{TEXT("add.cone"), 0, Placed | Size, Shape({P(TEXT("Top"), EParam::Float, 0, 1, TEXT("0.05")), P(TEXT("Steps"), EParam::Int, 3, 128, TEXT("16")),
				P(TEXT("HeightSteps"), EParam::Int, 0, 64, TEXT("1")), P(TEXT("Capped"), EParam::Bool, 0, 0, TEXT("true"))}),
				TEXT("a cone or frustum: Size X and Y across the base, Z tall; Top is the top's radius as a share of the base's")},
			{TEXT("add.capsule"), 0, Placed | Size, Shape({P(TEXT("Steps"), EParam::Int, 3, 128, TEXT("16")), P(TEXT("HemisphereSteps"), EParam::Int, 1, 32, TEXT("4")),
				P(TEXT("HeightSteps"), EParam::Int, 0, 64, TEXT("0"))}), TEXT("a capsule: Size X and Y across, Z tall overall")},
			{TEXT("add.torus"), 0, Placed | Size, Shape({P(TEXT("Steps"), EParam::Int, 3, 128, TEXT("24")), P(TEXT("MinorSteps"), EParam::Int, 3, 64, TEXT("12")),
				P(TEXT("Degrees"), EParam::Float, 1, 360, TEXT("360"))}), TEXT("a ring: Size X and Y across the outside, Z the tube's thickness; Degrees under 360 for an arc")},
			{TEXT("add.disc"), 0, Placed | Size, Shape({P(TEXT("Steps"), EParam::Int, 3, 128, TEXT("24")), P(TEXT("Hole"), EParam::Float, 0, 0.95, TEXT("0"))}),
				TEXT("a flat disc facing up (open, one-sided): Size X and Y across; Hole is the inner radius as a share")},
			{TEXT("add.stairs"), 0, Placed | Size, Shape({P(TEXT("Steps"), EParam::Int, 1, 64, TEXT("8")), P(TEXT("Floating"), EParam::Bool, 0, 0, TEXT("false"))}),
				TEXT("straight stairs: Size X the run, Y the width, Z the rise, climbing toward +X")},
			{TEXT("add.curved_stairs"), 0, Placed | Size, Shape({P(TEXT("Steps"), EParam::Int, 1, 64, TEXT("8")), P(TEXT("InnerRadius"), EParam::Float, 0, 100000, TEXT("150")),
				P(TEXT("Angle"), EParam::Float, 1, 360, TEXT("90")), P(TEXT("Floating"), EParam::Bool, 0, 0, TEXT("false"))}),
				TEXT("curved stairs: Size Y the width, Z the rise, turning Angle degrees about a well InnerRadius wide")},
			{TEXT("add.revolve"), Points, Placed | Points, Shape({P(TEXT("Steps"), EParam::Int, 3, 128, TEXT("24")), P(TEXT("Degrees"), EParam::Float, 1, 360, TEXT("360")),
				P(TEXT("Capped"), EParam::Bool, 0, 0, TEXT("true")), P(TEXT("Ring"), EParam::Float, 0, 100000, TEXT("0")), P(TEXT("Hard"), EParam::Bool, 0, 0, TEXT("false"))}, false),
				TEXT("a lathe shape: the Points profile (radius, height) turned about Z; with Ring, Points is a closed cross-section turned at that radius (arches, rims)")},
			{TEXT("add.sweep"), Points | Path, Placed | Points | Path, Shape({P(TEXT("StartScale"), EParam::Float, 0.01, 100, TEXT("1")),
				P(TEXT("EndScale"), EParam::Float, 0.01, 100, TEXT("1")), P(TEXT("Twist"), EParam::Float, -3600, 3600, TEXT("0")), P(TEXT("Loop"), EParam::Bool, 0, 0, TEXT("false")),
				P(TEXT("Capped"), EParam::Bool, 0, 0, TEXT("true"))}, false), TEXT("the Points cross-section swept along Path, scaled from StartScale to EndScale (trunks, branches, roots, pipes, rails)")},
			{TEXT("add.extrude_polygon"), Points, Placed | Points, Shape({P(TEXT("Height"), EParam::Float, 0.1, 100000, TEXT("100")),
				P(TEXT("HeightSteps"), EParam::Int, 0, 64, TEXT("0")), P(TEXT("Capped"), EParam::Bool, 0, 0, TEXT("true"))}, false), TEXT("the Points outline extruded up Height cm")},
			{TEXT("append"), Source, Source | Placed, {}, TEXT("a copy of Source added into Mesh, moved by Location, Rotation and Scale")},
			{TEXT("copy"), Source, Source | Placed, {}, TEXT("Mesh made (or replaced) as a copy of Source, moved by Location, Rotation and Scale")},
			{TEXT("array.linear"), Source, Source | Placed, {Required(TEXT("Count"), EParam::Int, 1, C::MaxArrayCount)},
				TEXT("Count copies of Source added into Mesh, each moved one more step of Location, Rotation and Scale")},
			{TEXT("array.radial"), Source, Source | Location, {Required(TEXT("Count"), EParam::Int, 1, C::MaxArrayCount), P(TEXT("Radius"), EParam::Float, 0, 100000, TEXT("0")),
				P(TEXT("Angle"), EParam::Float, 1, 360, TEXT("360")), P(TEXT("Align"), EParam::Bool, 0, 0, TEXT("true")), P(TEXT("Jitter"), EParam::Float, 0, 1, TEXT("0")),
				P(TEXT("Seed"), EParam::Int, -1000000000, 1000000000, TEXT("0"))},
				TEXT("Count copies of Source in a ring about Location's Z axis, Radius out, turned to face out with Align; Jitter varies turn, scale and lean")},
			{TEXT("scatter_on"), Source, Source, {P(TEXT("Count"), EParam::Int, 1, C::MaxScatterCount, TEXT("12")), P(TEXT("Spacing"), EParam::Float, 0.1, 100000, TEXT("Source's size")),
				P(TEXT("Seed"), EParam::Int, -1000000000, 1000000000, TEXT("0")), P(TEXT("ScaleMin"), EParam::Float, 0.01, 100, TEXT("0.7")),
				P(TEXT("ScaleMax"), EParam::Float, 0.01, 100, TEXT("1.2")), P(TEXT("Tilt"), EParam::Float, 0, 90, TEXT("15")), P(TEXT("Up"), EParam::Float, 0, 180, TEXT("180")),
				P(TEXT("Sink"), EParam::Float, 0, 100000, TEXT("0")), P(TEXT("Align"), EParam::Choice, 0, 0, TEXT("normal"), TEXT("normal|up"))},
				TEXT("copies of Source strewn over Mesh's surface and added into it: at most Count, Spacing apart, where the surface faces within Up degrees of up; random turn, Tilt and scale")},
			{TEXT("boolean"), Source, Source, {Required(TEXT("Op"), EParam::Choice, 0, 0, TEXT("union|subtract|intersect")), P(TEXT("Fill"), EParam::Bool, 0, 0, TEXT("true")),
				P(TEXT("AllowEmpty"), EParam::Bool, 0, 0, TEXT("false"))}, TEXT("Source joined to, cut out of, or intersected with Mesh; Source stays (drop it after)")},
			{TEXT("self_union"), 0, 0, {P(TEXT("Fill"), EParam::Bool, 0, 0, TEXT("true"))}, TEXT("Mesh's overlapping pieces fused into one watertight surface")},
			{TEXT("plane_cut"), 0, Location, {P(TEXT("Normal"), EParam::Vector, 0, 0, TEXT("0,0,1")), P(TEXT("Fill"), EParam::Bool, 0, 0, TEXT("true"))},
				TEXT("everything on the side Normal points to, from the plane through Location, cut away; the hole filled")},
			{TEXT("mirror"), 0, Location, {P(TEXT("Normal"), EParam::Vector, 0, 0, TEXT("1,0,0")), P(TEXT("Cut"), EParam::Bool, 0, 0, TEXT("true")),
				P(TEXT("Weld"), EParam::Bool, 0, 0, TEXT("true"))}, TEXT("the side opposite Normal kept and mirrored across the plane through Location")},
			{TEXT("extrude"), 0, Select, {Required(TEXT("Distance"), EParam::Float, -100000, 100000), P(TEXT("Direction"), EParam::Text, 0, 0, TEXT("normal"), TEXT("normal or x,y,z")),
				P(TEXT("Area"), EParam::Choice, 0, 0, TEXT("all"), TEXT("all|group|face"))}, TEXT("the selected faces pushed out Distance cm, with new side walls")},
			{TEXT("inset"), 0, Select, {Required(TEXT("Distance"), EParam::Float, -100000, 100000), P(TEXT("Area"), EParam::Choice, 0, 0, TEXT("all"), TEXT("all|group|face"))},
				TEXT("the selected faces shrunk inward Distance cm inside a new border (negative grows them)")},
			{TEXT("bevel"), 0, Select, {P(TEXT("Distance"), EParam::Float, 0.01, 10000, TEXT("2")), P(TEXT("Subdivisions"), EParam::Int, 0, 8, TEXT("0")),
				P(TEXT("Round"), EParam::Float, 0, 1, TEXT("1"))}, TEXT("edges bevelled Distance cm: every polygroup edge, or with Select the border of the selected faces")},
			{TEXT("shell"), 0, 0, {Required(TEXT("Thickness"), EParam::Float, -100000, 100000)}, TEXT("a surface given Thickness cm, inside and outside walls joined")},
			{TEXT("noise"), 0, Select, {P(TEXT("Magnitude"), EParam::Float, 0, 100000, TEXT("5")), P(TEXT("Frequency"), EParam::Float, 0.00001, 10, TEXT("0.02")),
				P(TEXT("Seed"), EParam::Int, -1000000000, 1000000000, TEXT("0"))}, TEXT("vertices pushed along their normals by Perlin noise up to Magnitude cm; Frequency is 1 / feature size in cm")},
			{TEXT("bend"), 0, Location | Rotation, {Required(TEXT("Angle"), EParam::Float, -360, 360), P(TEXT("Extent"), EParam::Float, 0.1, 100000, TEXT("the mesh's height")),
				P(TEXT("Lock"), EParam::Bool, 0, 0, TEXT("true"))}, TEXT("the mesh bent Angle degrees over Extent cm up the frame's Z axis (Location defaults to its base); Lock keeps what is below still")},
			{TEXT("twist"), 0, Location | Rotation, {Required(TEXT("Angle"), EParam::Float, -3600, 3600), P(TEXT("Extent"), EParam::Float, 0.1, 100000, TEXT("the mesh's height")),
				P(TEXT("Lock"), EParam::Bool, 0, 0, TEXT("true"))}, TEXT("the mesh twisted Angle degrees about the frame's Z axis over Extent cm")},
			{TEXT("flare"), 0, Location | Rotation, {Required(TEXT("Percent"), EParam::Float, -100, 1000), P(TEXT("Extent"), EParam::Float, 0.1, 100000, TEXT("half the mesh's height")),
				P(TEXT("Mode"), EParam::Choice, 0, 0, TEXT("sin"), TEXT("sin|square|triangle"))}, TEXT("the mesh bulged out (or pinched in) by Percent across Extent cm about Location (its center)")},
			{TEXT("smooth"), 0, Select, {P(TEXT("Iterations"), EParam::Int, 1, 200, TEXT("10")), P(TEXT("Alpha"), EParam::Float, 0, 1, TEXT("0.2"))}, TEXT("the surface relaxed")},
			{TEXT("tessellate"), 0, 0, {P(TEXT("Level"), EParam::Int, 1, C::MaxTessellateLevel, TEXT("1")), P(TEXT("Mode"), EParam::Choice, 0, 0, TEXT("uniform"), TEXT("uniform|pn"))},
				TEXT("each triangle split into (Level+1)^2; pn also rounds the surface")},
			{TEXT("remesh"), 0, 0, {Required(TEXT("Triangles"), EParam::Int, 100, C::MaxRemeshTriangles)}, TEXT("the surface rebuilt from even triangles, about Triangles of them")},
			{TEXT("simplify"), 0, 0, {P(TEXT("Triangles"), EParam::Int, 4, C::MaxTrianglesPerMesh, TEXT("")), P(TEXT("Planar"), EParam::Bool, 0, 0, TEXT("false")),
				P(TEXT("Tolerance"), EParam::Float, 0.001, 10000, TEXT(""))}, TEXT("fewer triangles: down to Triangles, flat areas merged with Planar, or within Tolerance cm of the surface (give one)")},
			{TEXT("solidify"), 0, 0, {P(TEXT("Resolution"), EParam::Int, 8, C::MaxGridResolution, TEXT("64")), P(TEXT("Thicken"), EParam::Float, 0, 100000, TEXT("0")),
				P(TEXT("Winding"), EParam::Float, 0.01, 0.99, TEXT("0.5"))}, TEXT("a watertight copy rebuilt through a voxel grid Resolution cells across; Thicken gives open surfaces that many cm")},
			{TEXT("morphology"), 0, 0, {Required(TEXT("Op"), EParam::Choice, 0, 0, TEXT("dilate|contract|close|open")), Required(TEXT("Distance"), EParam::Float, 0.01, 100000),
				P(TEXT("Resolution"), EParam::Int, 8, C::MaxGridResolution, TEXT("64"))}, TEXT("grown, shrunk, gaps closed or thin parts opened by Distance cm, through a voxel grid")},
			{TEXT("transform"), 0, Placed, {}, TEXT("the whole mesh scaled, turned, then moved")},
			{TEXT("pivot"), 0, 0, {P(TEXT("At"), EParam::Text, 0, 0, TEXT("base"), TEXT("base, center or x,y,z"))}, TEXT("the mesh moved so At becomes its origin; base is the bottom's center")},
			{TEXT("normals"), 0, 0, {Required(TEXT("Mode"), EParam::Choice, 0, 0, TEXT("smooth|hard|split")), P(TEXT("Angle"), EParam::Float, 1, 180, TEXT("30"))},
				TEXT("shading: smooth, hard (faceted) or split at edges sharper than Angle")},
			{TEXT("repair"), 0, 0, {P(TEXT("Tolerance"), EParam::Float, 0, 100, TEXT("0.01")), P(TEXT("Fill"), EParam::Bool, 0, 0, TEXT("true")),
				P(TEXT("MinTriangles"), EParam::Int, 0, 100000, TEXT("0"))}, TEXT("seams welded within Tolerance cm, holes filled, pieces under MinTriangles dropped, slivers removed")},
			{TEXT("uv"), 0, Select, {P(TEXT("Mode"), EParam::Choice, 0, 0, TEXT("box"), TEXT("box|planar|cylinder|auto")), P(TEXT("Scale"), EParam::Float, 1, 100000, TEXT("100"))},
				TEXT("UV channel 0 projected, one texture tile per Scale cm; auto unwraps")},
			{TEXT("material"), 0, Select, {Required(TEXT("Id"), EParam::Int, 0, 15)}, TEXT("the selected faces given material slot Id")},
			{TEXT("color"), 0, Select, {Required(TEXT("Color"), EParam::Color, 0, 0), P(TEXT("Glow"), EParam::Float, 0, 1, TEXT("0"))},
				TEXT("the selected faces' vertex colour (#rrggbb or r,g,b in 0-1); with the default material it is the base colour, and Glow makes it emissive")},
			{TEXT("drop"), 0, 0, {}, TEXT("the working mesh discarded (recipes drop their parts)")},
			{TEXT("recipe"), 0, Placed, {Required(TEXT("Shape"), EParam::Text, 0, 0, TEXT("a recipe name")), P(TEXT("Size"), EParam::Float, 1, 100000, TEXT("per recipe")),
				P(TEXT("Seed"), EParam::Int, -1000000000, 1000000000, TEXT("0")), P(TEXT("Detail"), EParam::Int, 1, 3, TEXT("2")), P(TEXT("Tint"), EParam::Color, 0, 0, TEXT("per recipe")),
				P(TEXT("Accent"), EParam::Color, 0, 0, TEXT("per recipe"))}, TEXT("a whole prop built into Mesh from ordinary ops, its pivot at its base, then placed by Location, Rotation and Scale")},
			{TEXT("write_asset"), Asset, Asset, {P(TEXT("Nanite"), EParam::Bool, 0, 0, TEXT("false")),
				P(TEXT("Collision"), EParam::Text, 0, 0, TEXT("convex"), TEXT("none, box, convex, hulls:N, auto or complex")),
				P(TEXT("Materials"), EParam::Text, 0, 0, TEXT("the vertex-colour material"), TEXT("slot:path pairs, comma-separated: 0:/Game/M/MI_Stone,1:/Game/M/MI_Moss")),
				P(TEXT("Replace"), EParam::Bool, 0, 0, TEXT("false")), P(TEXT("LightmapUV"), EParam::Bool, 0, 0, TEXT("false"))},
				TEXT("Mesh saved as the static mesh Asset, with collision; Replace overwrites an existing static mesh in the plan's undo step")},
		};
		return List;
	}

	const FKindSpec* FindSpec(const FString& Kind)
	{
		for (const FKindSpec& Spec : Specs())
		{
			if (Kind == Spec.Kind) return &Spec;
		}
		return nullptr;
	}

	const FParamSpec* FindParam(const FKindSpec& Spec, const FString& Name)
	{
		for (const FParamSpec& Param : Spec.Params)
		{
			if (Name.Equals(Param.Name, ESearchCase::IgnoreCase)) return &Param;
		}
		return nullptr;
	}

	FString KeyOf(const FString& Name)
	{
		return Name.TrimStartAndEnd().ToLower();
	}

	FString Num(const double Value)
	{
		const double Rounded = FMath::RoundToDouble(Value * 10.0) / 10.0;
		return FString::Printf(TEXT("%g"), FMath::IsNearlyZero(Rounded) ? 0.0 : Rounded);
	}

	FString Vec(const FVector& Value)
	{
		return Num(Value.X) + TEXT(",") + Num(Value.Y) + TEXT(",") + Num(Value.Z);
	}

	// --- Parsing --------------------------------------------------------------------------------------------------

	bool ParseNumbers(const FString& Text, TArray<double>& OutValues)
	{
		OutValues.Reset();
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(","), /*bCullEmpty=*/false);
		for (const FString& Part : Parts)
		{
			const FString Trimmed = Part.TrimStartAndEnd();
			double Value = 0.0;
			if (Trimmed.IsEmpty() || !LexTryParseString(Value, *Trimmed) || !FMath::IsFinite(Value)) return false;
			OutValues.Add(Value);
		}
		return !OutValues.IsEmpty();
	}

	bool ParseVector(const FString& Text, FVector& Out)
	{
		TArray<double> Values;
		if (!ParseNumbers(Text, Values) || Values.Num() != 3) return false;
		Out = FVector(Values[0], Values[1], Values[2]);
		return true;
	}

	bool ParseColor(const FString& Text, FLinearColor& OutColor)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		if (Trimmed.StartsWith(TEXT("#")))
		{
			const FString Hex = Trimmed.Mid(1);
			if (Hex.Len() != 6 && Hex.Len() != 8) return false;
			for (const TCHAR Character : Hex)
			{
				if (!FChar::IsHexDigit(Character)) return false;
			}
			const FColor Color = FColor::FromHex(Hex);
			// Vertex colours are kept as written (sRGB); the static mesh build converts them, as the editor's paint does.
			OutColor = FLinearColor(Color.R / 255.f, Color.G / 255.f, Color.B / 255.f, Hex.Len() == 8 ? Color.A / 255.f : 1.f);
			return true;
		}
		TArray<double> Values;
		if (!ParseNumbers(Trimmed, Values) || (Values.Num() != 3 && Values.Num() != 4)) return false;
		for (const double Value : Values)
		{
			if (Value < 0.0 || Value > 1.0) return false;
		}
		OutColor = FLinearColor(Values[0], Values[1], Values[2], Values.Num() == 4 ? Values[3] : 1.0);
		return true;
	}

	bool ParseProfile(const FString& Text, TArray<FVector2D>& OutPoints, FString& OutError)
	{
		OutPoints.Reset();
		const FString Trimmed = Text.TrimStartAndEnd();
		TArray<double> Values;
		if (Trimmed.StartsWith(TEXT("circle:"), ESearchCase::IgnoreCase))
		{
			if (!ParseNumbers(Trimmed.Mid(7), Values) || Values.Num() != 2 || Values[0] <= 0.0 || Values[1] < 3 || Values[1] > C::MaxProfilePoints)
			{
				OutError = FString::Printf(TEXT("circle: takes a radius and 3 to %d sides, like circle:20,12."), C::MaxProfilePoints);
				return false;
			}
			const int32 Sides = FMath::RoundToInt(Values[1]);
			for (int32 Index = 0; Index < Sides; ++Index)
			{
				const double Angle = 2.0 * UE_DOUBLE_PI * Index / Sides;
				OutPoints.Add(FVector2D(FMath::Cos(Angle) * Values[0], FMath::Sin(Angle) * Values[0]));
			}
			return true;
		}
		if (Trimmed.StartsWith(TEXT("rect:"), ESearchCase::IgnoreCase))
		{
			if (!ParseNumbers(Trimmed.Mid(5), Values) || Values.Num() != 2 || Values[0] <= 0.0 || Values[1] <= 0.0)
			{
				OutError = TEXT("rect: takes a width and a depth, like rect:40,20.");
				return false;
			}
			const double X = Values[0] * 0.5;
			const double Y = Values[1] * 0.5;
			OutPoints = {FVector2D(-X, -Y), FVector2D(X, -Y), FVector2D(X, Y), FVector2D(-X, Y)};
			return true;
		}
		TArray<FString> Pairs;
		Trimmed.ParseIntoArray(Pairs, TEXT(";"), /*bCullEmpty=*/true);
		for (const FString& Pair : Pairs)
		{
			if (!ParseNumbers(Pair, Values) || Values.Num() != 2)
			{
				OutError = FString::Printf(TEXT("'%s' is not a point x,y; write points as \"x,y; x,y\"."), *Pair.TrimStartAndEnd());
				return false;
			}
			OutPoints.Add(FVector2D(Values[0], Values[1]));
		}
		if (OutPoints.Num() > C::MaxProfilePoints)
		{
			OutError = FString::Printf(TEXT("%d points; a profile has at most %d."), OutPoints.Num(), C::MaxProfilePoints);
			return false;
		}
		return true;
	}

	bool ParsePath(const FString& Text, TArray<FVector>& OutPoints, FString& OutError)
	{
		OutPoints.Reset();
		TArray<FString> Triples;
		Text.ParseIntoArray(Triples, TEXT(";"), /*bCullEmpty=*/true);
		for (const FString& Triple : Triples)
		{
			FVector Point;
			if (!ParseVector(Triple, Point))
			{
				OutError = FString::Printf(TEXT("'%s' is not a point x,y,z; write a path as \"x,y,z; x,y,z\"."), *Triple.TrimStartAndEnd());
				return false;
			}
			OutPoints.Add(Point);
		}
		if (OutPoints.Num() < 2 || OutPoints.Num() > C::MaxPathPoints)
		{
			OutError = FString::Printf(TEXT("a path has 2 to %d points; this one has %d."), C::MaxPathPoints, OutPoints.Num());
			return false;
		}
		return true;
	}

	bool ParseParams(const FString& Text, TArray<TPair<FString, FString>>& OutPairs, FString& OutError)
	{
		OutPairs.Reset();
		TArray<FString> Parts;
		Text.ParseIntoArray(Parts, TEXT(";"), /*bCullEmpty=*/true);
		for (const FString& Part : Parts)
		{
			FString Name;
			FString Value;
			if (Part.TrimStartAndEnd().IsEmpty()) continue;
			if (!Part.Split(TEXT("="), &Name, &Value) || Name.TrimStartAndEnd().IsEmpty())
			{
				OutError = FString::Printf(TEXT("'%s' is not Name=Value; write Params as \"Name=Value; Name=Value\"."), *Part.TrimStartAndEnd());
				return false;
			}
			Name.TrimStartAndEndInline();
			Value.TrimStartAndEndInline();
			if (OutPairs.ContainsByPredicate([&Name](const TPair<FString, FString>& Pair) { return Pair.Key.Equals(Name, ESearchCase::IgnoreCase); }))
			{
				OutError = FString::Printf(TEXT("%s is given twice."), *Name);
				return false;
			}
			OutPairs.Emplace(Name, Value);
		}
		return true;
	}

	bool ParseBool(const FString& Text, bool& Out)
	{
		const FString Value = Text.TrimStartAndEnd().ToLower();
		if (Value == TEXT("true") || Value == TEXT("yes") || Value == TEXT("1")) Out = true;
		else if (Value == TEXT("false") || Value == TEXT("no") || Value == TEXT("0")) Out = false;
		else return false;
		return true;
	}

	bool ParseSelector(const FString& Text, FSelector& Out, FString& OutError)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		Out = FSelector();
		Out.bGiven = !Trimmed.IsEmpty();
		if (Trimmed.IsEmpty() || Trimmed.Equals(TEXT("all"), ESearchCase::IgnoreCase)) return true;
		if (Trimmed.Equals(TEXT("top"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("bottom"), ESearchCase::IgnoreCase))
		{
			Out.Type = FSelector::EType::Normal;
			Out.A = Trimmed.Equals(TEXT("top"), ESearchCase::IgnoreCase) ? FVector::UpVector : -FVector::UpVector;
			Out.Value = 30.0;
			return true;
		}
		FString Kind;
		FString Rest;
		TArray<double> Values;
		if (!Trimmed.Split(TEXT(":"), &Kind, &Rest) || !ParseNumbers(Rest, Values))
		{
			OutError = FString::Printf(TEXT("Select '%s' is not one of all, top, bottom, normal:nx,ny,nz,deg, box:x0,y0,z0,x1,y1,z1, sphere:x,y,z,r, plane:ox,oy,oz,nx,ny,nz or material:id."), *Trimmed);
			return false;
		}
		Kind = Kind.TrimStartAndEnd().ToLower();
		auto Need = [&](const int32 Count, const TCHAR* Form)
		{
			if (Values.Num() == Count) return true;
			OutError = FString::Printf(TEXT("Select %s takes %s."), *Kind, Form);
			return false;
		};
		if (Kind == TEXT("normal"))
		{
			if (!Need(4, TEXT("nx,ny,nz,maxdeg"))) return false;
			Out.Type = FSelector::EType::Normal;
			Out.A = FVector(Values[0], Values[1], Values[2]);
			Out.Value = Values[3];
			if (Out.A.IsNearlyZero() || Out.Value <= 0.0 || Out.Value > 180.0)
			{
				OutError = TEXT("Select normal: needs a direction and an angle of 0 to 180 degrees.");
				return false;
			}
			Out.A.Normalize();
			return true;
		}
		if (Kind == TEXT("box"))
		{
			if (!Need(6, TEXT("x0,y0,z0,x1,y1,z1"))) return false;
			Out.Type = FSelector::EType::Box;
			Out.A = FVector(FMath::Min(Values[0], Values[3]), FMath::Min(Values[1], Values[4]), FMath::Min(Values[2], Values[5]));
			Out.B = FVector(FMath::Max(Values[0], Values[3]), FMath::Max(Values[1], Values[4]), FMath::Max(Values[2], Values[5]));
			return true;
		}
		if (Kind == TEXT("sphere"))
		{
			if (!Need(4, TEXT("x,y,z,radius"))) return false;
			Out.Type = FSelector::EType::Sphere;
			Out.A = FVector(Values[0], Values[1], Values[2]);
			Out.Value = Values[3];
			if (Out.Value <= 0.0)
			{
				OutError = TEXT("Select sphere: needs a radius above 0.");
				return false;
			}
			return true;
		}
		if (Kind == TEXT("plane"))
		{
			if (!Need(6, TEXT("ox,oy,oz,nx,ny,nz"))) return false;
			Out.Type = FSelector::EType::Plane;
			Out.A = FVector(Values[0], Values[1], Values[2]);
			Out.B = FVector(Values[3], Values[4], Values[5]);
			if (Out.B.IsNearlyZero())
			{
				OutError = TEXT("Select plane: needs a normal.");
				return false;
			}
			Out.B.Normalize();
			return true;
		}
		if (Kind == TEXT("material"))
		{
			if (!Need(1, TEXT("an id"))) return false;
			Out.Type = FSelector::EType::Material;
			Out.Value = FMath::RoundToDouble(Values[0]);
			return true;
		}
		OutError = FString::Printf(TEXT("Select kind '%s' is not all, top, bottom, normal, box, sphere, plane or material."), *Kind);
		return false;
	}

	/** A param's value checked against its spec. */
	bool CheckParam(const FParamSpec& Spec, const FString& Value, FString& OutError)
	{
		double Number = 0.0;
		bool Flag = false;
		FVector Vector;
		FLinearColor Color;
		switch (Spec.Type)
		{
		case EParam::Int:
			if (!LexTryParseString(Number, *Value) || Number != FMath::RoundToDouble(Number) || Number < Spec.Min || Number > Spec.Max)
			{
				OutError = FString::Printf(TEXT("%s is a whole number from %s to %s, not '%s'."), Spec.Name, *Num(Spec.Min), *Num(Spec.Max), *Value);
				return false;
			}
			return true;
		case EParam::Float:
			if (!LexTryParseString(Number, *Value) || !FMath::IsFinite(Number) || Number < Spec.Min || Number > Spec.Max)
			{
				OutError = FString::Printf(TEXT("%s is a number from %s to %s, not '%s'."), Spec.Name, *Num(Spec.Min), *Num(Spec.Max), *Value);
				return false;
			}
			return true;
		case EParam::Bool:
			if (!ParseBool(Value, Flag))
			{
				OutError = FString::Printf(TEXT("%s is true or false, not '%s'."), Spec.Name, *Value);
				return false;
			}
			return true;
		case EParam::Choice:
		{
			TArray<FString> Words;
			FString(Spec.Choices).ParseIntoArray(Words, TEXT("|"));
			if (!Words.ContainsByPredicate([&Value](const FString& Word) { return Word.Equals(Value, ESearchCase::IgnoreCase); }))
			{
				OutError = FString::Printf(TEXT("%s is %s, not '%s'."), Spec.Name, *FString::Join(Words, TEXT(", ")), *Value);
				return false;
			}
			return true;
		}
		case EParam::Vector:
			if (!ParseVector(Value, Vector) || Vector.IsNearlyZero())
			{
				OutError = FString::Printf(TEXT("%s is a direction x,y,z, not '%s'."), Spec.Name, *Value);
				return false;
			}
			return true;
		case EParam::Color:
			if (!ParseColor(Value, Color))
			{
				OutError = FString::Printf(TEXT("%s is a colour, #rrggbb or r,g,b in 0 to 1, not '%s'."), Spec.Name, *Value);
				return false;
			}
			return true;
		case EParam::Text:
		default:
			return true;
		}
	}

	// --- Steps -----------------------------------------------------------------------------------------------------

	bool FStep::Has(const TCHAR* Name) const
	{
		return Params.Contains(FString(Name).ToLower());
	}

	double FStep::Float(const TCHAR* Name, const double Default) const
	{
		const FString* Value = Params.Find(FString(Name).ToLower());
		double Out = Default;
		if (Value) LexTryParseString(Out, **Value);
		return Out;
	}

	int32 FStep::Int(const TCHAR* Name, const int32 Default) const
	{
		return FMath::RoundToInt(Float(Name, Default));
	}

	bool FStep::Bool(const TCHAR* Name, const bool Default) const
	{
		const FString* Value = Params.Find(FString(Name).ToLower());
		bool Out = Default;
		if (Value) ParseBool(*Value, Out);
		return Out;
	}

	FString FStep::Text(const TCHAR* Name, const TCHAR* Default) const
	{
		const FString* Value = Params.Find(FString(Name).ToLower());
		return Value ? *Value : FString(Default);
	}

	FVector FStep::Vector(const TCHAR* Name, const FVector& Default) const
	{
		const FString* Value = Params.Find(FString(Name).ToLower());
		FVector Out = Default;
		if (Value) ParseVector(*Value, Out);
		return Out;
	}

	FLinearColor FStep::Color(const TCHAR* Name, const FLinearColor& Default) const
	{
		const FString* Value = Params.Find(FString(Name).ToLower());
		FLinearColor Out = Default;
		if (Value) ParseColor(*Value, Out);
		return Out;
	}

	FString FStep::Describe() const
	{
		FString Text = FString::Printf(TEXT("Op %d (%s, mesh %s"), Index, *Kind, *Mesh);
		if (bFromRecipe) Text += FString::Printf(TEXT(", from the recipe at request op %d"), From);
		return Text + TEXT(")");
	}

	FLinearColor DefaultColor()
	{
		FLinearColor Color;
		ParseColor(DefaultColorText, Color);
		return Color;
	}

	/** An expanded op as one line, for ExpandedOps and the plan hash. */
	FString OpLine(const int32 Index, const FHyperAIModelOp& Op)
	{
		FString Line = FString::Printf(TEXT("#%d %s %s"), Index, *Op.Kind.TrimStartAndEnd(), *Op.Mesh.TrimStartAndEnd());
		auto Field = [&Line](const TCHAR* Name, const FString& Value)
		{
			if (!Value.TrimStartAndEnd().IsEmpty()) Line += FString::Printf(TEXT(" %s=%s"), Name, *Value.TrimStartAndEnd());
		};
		Field(TEXT("Source"), Op.Source);
		Field(TEXT("Location"), Op.Location);
		Field(TEXT("Rotation"), Op.Rotation);
		Field(TEXT("Scale"), Op.Scale);
		Field(TEXT("Size"), Op.Size);
		Field(TEXT("Points"), Op.Points);
		Field(TEXT("Path"), Op.Path);
		Field(TEXT("Select"), Op.Select);
		Field(TEXT("Asset"), Op.Asset);
		if (!Op.Params.TrimStartAndEnd().IsEmpty()) Line += FString::Printf(TEXT(" [%s]"), *Op.Params.TrimStartAndEnd());
		return Line;
	}

	bool IsValidMeshName(const FString& Name)
	{
		if (Name.IsEmpty() || Name.Len() > 48) return false;
		for (const TCHAR Character : Name)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-')) return false;
		}
		return true;
	}

	FString PackageOf(const FString& Reference)
	{
		FString Path = Reference.TrimStartAndEnd();
		int32 Dot = INDEX_NONE;
		if (Path.FindLastChar(TEXT('.'), Dot)) Path.LeftInline(Dot);
		return Path;
	}

	FString ObjectPathOf(const FString& Reference)
	{
		const FString Trimmed = Reference.TrimStartAndEnd();
		return Trimmed.Contains(TEXT(".")) ? Trimmed : Trimmed + TEXT(".") + FPackageName::GetShortName(Trimmed);
	}

	/** An asset in memory, or loaded from disk; a path with no package on disk is never loaded, which would leave an empty package behind. */
	template <typename T>
	T* LoadQuiet(const FString& Reference)
	{
		const FString Trimmed = Reference.TrimStartAndEnd();
		if (Trimmed.IsEmpty() || !Trimmed.StartsWith(TEXT("/"))) return nullptr;
		if (T* Found = FindObject<T>(nullptr, *ObjectPathOf(Trimmed))) return Found;
		if (!FPackageName::DoesPackageExist(PackageOf(Trimmed))) return nullptr;
		return LoadObject<T>(nullptr, *ObjectPathOf(Trimmed), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}

	/** On disk, or made in memory and not saved yet; an empty package a failed load left behind is no asset. */
	bool PackageExists(const FString& Path)
	{
		if (FPackageName::DoesPackageExist(Path)) return true;
		UPackage* Package = FindPackage(nullptr, *Path);
		return Package && FindObject<UObject>(Package, *FPackageName::GetShortName(Path)) != nullptr;
	}

	/**
	 * The static mesh Replace overwrites: the asset at Path itself. A load follows the redirector a rename or move leaves
	 * behind, but the plan was checked and approved against the path, so a mesh that lives elsewhere is refused.
	 */
	UStaticMesh* ReplaceTarget(const FString& Path, FString& OutError)
	{
		UStaticMesh* Mesh = LoadQuiet<UStaticMesh>(Path);
		if (!Mesh)
		{
			// A redirector made in memory and not saved yet: nothing on disk for the load to follow.
			const UObjectRedirector* Redirector = FindObject<UObjectRedirector>(nullptr, *ObjectPathOf(Path));
			OutError = Redirector && Redirector->DestinationObject
				? FString::Printf(TEXT("%s is a redirector to %s; replace that path, or fix up redirectors first."), *Path, *Redirector->DestinationObject->GetOutermost()->GetName())
				: FString::Printf(TEXT("%s is not a static mesh, so it cannot be replaced."), *Path);
			return nullptr;
		}
		const FString Real = Mesh->GetOutermost()->GetName();
		if (!Real.Equals(Path, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("%s is a redirector to %s; replace that path, or fix up redirectors first."), *Path, *Real);
			return nullptr;
		}
		return Mesh;
	}

	/** write_asset's collision: none, box, convex, hulls:N, auto or complex. */
	bool ParseCollision(const FString& Text, FString& OutMode, int32& OutHulls)
	{
		const FString Value = Text.TrimStartAndEnd().ToLower();
		OutHulls = 1;
		if (Value.StartsWith(TEXT("hulls:")))
		{
			double Count = 0.0;
			if (!LexTryParseString(Count, *Value.Mid(6)) || Count < 1.0 || Count > 16.0 || Count != FMath::RoundToDouble(Count)) return false;
			OutMode = TEXT("hulls");
			OutHulls = FMath::RoundToInt(Count);
			return true;
		}
		if (Value == TEXT("none") || Value == TEXT("box") || Value == TEXT("convex") || Value == TEXT("auto") || Value == TEXT("complex"))
		{
			OutMode = Value;
			return true;
		}
		return false;
	}

	/** write_asset's Materials: "0:/Game/M/MI_Stone,1:/Game/M/MI_Moss". */
	bool ParseMaterials(const FString& Text, TMap<int32, FString>& OutSlots, FString& OutError)
	{
		TArray<FString> Entries;
		Text.ParseIntoArray(Entries, TEXT(","), /*bCullEmpty=*/true);
		for (const FString& Entry : Entries)
		{
			FString Slot;
			FString Path;
			double Id = 0.0;
			if (!Entry.Split(TEXT(":"), &Slot, &Path) || !LexTryParseString(Id, *Slot.TrimStartAndEnd()) || Id < 0.0 || Id > 15.0 || Id != FMath::RoundToDouble(Id))
			{
				OutError = FString::Printf(TEXT("Materials entry '%s' is not slot:path, like 0:/Game/M/MI_Stone."), *Entry.TrimStartAndEnd());
				return false;
			}
			if (!LoadQuiet<UMaterialInterface>(Path))
			{
				OutError = FString::Printf(TEXT("Materials: %s is not a material or material instance."), *Path.TrimStartAndEnd());
				return false;
			}
			OutSlots.Add(FMath::RoundToInt(Id), Path.TrimStartAndEnd());
		}
		return true;
	}

	// --- Triangle predictions ----------------------------------------------------------------------------------------

	/** Triangles Mesh holds after the op, worked out from its params before anything is built. */
	int64 Predict(const FStep& Step, const TMap<FString, int64>& Tris)
	{
		const int64 Current = Tris.FindRef(KeyOf(Step.Mesh));
		const int64 Source = Tris.FindRef(KeyOf(Step.Source));
		const FString& K = Step.Kind;
		const int64 Steps = Step.Int(TEXT("Steps"), 16);
		if (K == TEXT("add.box"))
		{
			const int64 Side = Step.Int(TEXT("Steps"), 0) + 1;
			return Current + 12 * Side * Side;
		}
		if (K == TEXT("add.sphere"))
		{
			if (Step.Text(TEXT("Style"), TEXT("latlong")).Equals(TEXT("box"), ESearchCase::IgnoreCase))
			{
				const int64 Side = Step.Int(TEXT("Steps"), 8) + 1;
				return Current + 12 * Side * Side;
			}
			return Current + 2 * Steps * FMath::Max<int64>(3, Steps / 2);
		}
		if (K == TEXT("add.cylinder") || K == TEXT("add.cone"))
		{
			const int64 Height = Step.Int(TEXT("HeightSteps"), K == TEXT("add.cone") ? 1 : 0);
			return Current + 2 * Steps * (Height + 1) + 2 * Steps;
		}
		if (K == TEXT("add.capsule")) return Current + 2 * Steps * (2 * Step.Int(TEXT("HemisphereSteps"), 4) + Step.Int(TEXT("HeightSteps"), 0) + 1);
		if (K == TEXT("add.torus")) return Current + 2 * Step.Int(TEXT("Steps"), 24) * Step.Int(TEXT("MinorSteps"), 12) + 2 * Step.Int(TEXT("MinorSteps"), 12);
		if (K == TEXT("add.disc")) return Current + 2 * Step.Int(TEXT("Steps"), 24);
		if (K == TEXT("add.stairs") || K == TEXT("add.curved_stairs")) return Current + 24 * Step.Int(TEXT("Steps"), 8) + 8;
		if (K == TEXT("add.revolve"))
		{
			const int64 Around = Step.Int(TEXT("Steps"), 24);
			const int64 Points = Step.Points.Num();
			return Current + 2 * Points * Around + 4 * Points;
		}
		if (K == TEXT("add.sweep")) return Current + 2 * int64(Step.Points.Num()) * Step.Path.Num() + 2 * Step.Points.Num();
		if (K == TEXT("add.extrude_polygon")) return Current + 2 * int64(Step.Points.Num()) * (Step.Int(TEXT("HeightSteps"), 0) + 1) + 2 * Step.Points.Num();
		if (K == TEXT("append")) return Current + Source;
		if (K == TEXT("copy")) return Source;
		if (K == TEXT("array.linear") || K == TEXT("array.radial")) return Current + Source * Step.Int(TEXT("Count"), 1);
		if (K == TEXT("scatter_on")) return Current + Source * Step.Int(TEXT("Count"), 12);
		if (K == TEXT("boolean")) return (Current + Source) * 3 / 2;
		if (K == TEXT("self_union")) return Current * 6 / 5;
		if (K == TEXT("plane_cut")) return Current + Current / 10 + 16;
		if (K == TEXT("mirror")) return 2 * Current;
		if (K == TEXT("extrude") || K == TEXT("inset") || K == TEXT("shell")) return 2 * Current + 16;
		if (K == TEXT("bevel")) return Current * (2 + Step.Int(TEXT("Subdivisions"), 0));
		if (K == TEXT("tessellate"))
		{
			const int64 Split = Step.Int(TEXT("Level"), 1) + 1;
			return Current * Split * Split;
		}
		if (K == TEXT("remesh")) return Step.Int(TEXT("Triangles"), 5000);
		if (K == TEXT("simplify")) return Step.Has(TEXT("Triangles")) ? FMath::Min<int64>(Current, Step.Int(TEXT("Triangles"), 0)) : Current;
		if (K == TEXT("solidify") || K == TEXT("morphology"))
		{
			const int64 Resolution = Step.Int(TEXT("Resolution"), 64);
			return 8 * Resolution * Resolution;
		}
		return Current;
	}

	// --- Check -----------------------------------------------------------------------------------------------------

	/** One request op's fields and params parsed into a step, against its kind's spec. */
	bool ParseStep(const FHyperAIModelOp& Op, FStep& Step, FString& OutError)
	{
		const FKindSpec& Spec = *Step.Spec;
		auto Present = [](const FString& Text) { return !Text.TrimStartAndEnd().IsEmpty(); };
		const TPair<uint32, const FString*> Fields[] = {{Field::Source, &Op.Source}, {Field::Location, &Op.Location}, {Field::Rotation, &Op.Rotation},
			{Field::Scale, &Op.Scale}, {Field::Size, &Op.Size}, {Field::Points, &Op.Points}, {Field::Path, &Op.Path}, {Field::Select, &Op.Select}, {Field::Asset, &Op.Asset}};
		const TCHAR* Names[] = {TEXT("Source"), TEXT("Location"), TEXT("Rotation"), TEXT("Scale"), TEXT("Size"), TEXT("Points"), TEXT("Path"), TEXT("Select"), TEXT("Asset")};
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Fields); ++Index)
		{
			const bool bGiven = Present(*Fields[Index].Value);
			if (bGiven && !(Spec.Allowed & Fields[Index].Key))
			{
				OutError = FString::Printf(TEXT("%s takes no %s."), Spec.Kind, Names[Index]);
				return false;
			}
			if (!bGiven && (Spec.Required & Fields[Index].Key))
			{
				OutError = FString::Printf(TEXT("%s needs %s."), Spec.Kind, Names[Index]);
				return false;
			}
		}
		Step.Source = Op.Source.TrimStartAndEnd();
		Step.Asset = Op.Asset.TrimStartAndEnd();
		if (Present(Op.Location))
		{
			if (!ParseVector(Op.Location, Step.Location))
			{
				OutError = FString::Printf(TEXT("Location '%s' is not x,y,z in cm."), *Op.Location);
				return false;
			}
			Step.bLocation = true;
		}
		if (Present(Op.Rotation))
		{
			FVector Angles;
			if (!ParseVector(Op.Rotation, Angles))
			{
				OutError = FString::Printf(TEXT("Rotation '%s' is not pitch,yaw,roll in degrees."), *Op.Rotation);
				return false;
			}
			Step.Rotation = FRotator(Angles.X, Angles.Y, Angles.Z);
		}
		if (Present(Op.Scale))
		{
			TArray<double> Values;
			if (!ParseNumbers(Op.Scale, Values) || (Values.Num() != 1 && Values.Num() != 3))
			{
				OutError = FString::Printf(TEXT("Scale '%s' is one number or x,y,z."), *Op.Scale);
				return false;
			}
			Step.Scale = Values.Num() == 1 ? FVector(Values[0]) : FVector(Values[0], Values[1], Values[2]);
			if (FMath::Abs(Step.Scale.X) < 0.001 || FMath::Abs(Step.Scale.Y) < 0.001 || FMath::Abs(Step.Scale.Z) < 0.001 || Step.Scale.GetAbsMax() > 1000.0)
			{
				OutError = TEXT("Scale is 0.001 to 1000 on each axis (negative mirrors).");
				return false;
			}
		}
		if (Present(Op.Size))
		{
			TArray<double> Values;
			if (!ParseNumbers(Op.Size, Values) || (Values.Num() != 1 && Values.Num() != 3))
			{
				OutError = FString::Printf(TEXT("Size '%s' is x,y,z in cm, or one number for all three."), *Op.Size);
				return false;
			}
			Step.Size = Values.Num() == 1 ? FVector(Values[0]) : FVector(Values[0], Values[1], Values[2]);
			// A disc ignores Size Z and curved stairs Size X, so those may be 0; a 0 on an axis the shape uses would flatten
			// it to nothing, which builds no render data.
			FVector Used = Step.Size;
			if (Spec.Kind == FString(TEXT("add.disc"))) Used.Z = 1.0;
			if (Spec.Kind == FString(TEXT("add.curved_stairs"))) Used.X = 1.0;
			if (Step.Size.GetMin() < 0.0 || Used.GetMin() < 0.01 || Step.Size.GetMax() > 100000.0)
			{
				OutError = TEXT("Size is above 0 and at most 100000 cm on each axis.");
				return false;
			}
			Step.bSize = true;
		}
		if (Present(Op.Points) && !ParseProfile(Op.Points, Step.Points, OutError)) return false;
		if (Present(Op.Path) && !ParsePath(Op.Path, Step.Path, OutError)) return false;
		if (Present(Op.Select) && !ParseSelector(Op.Select, Step.Select, OutError)) return false;
		TArray<TPair<FString, FString>> Pairs;
		if (!ParseParams(Op.Params, Pairs, OutError)) return false;
		for (const TPair<FString, FString>& Pair : Pairs)
		{
			// Every selecting op takes Invert and Grow beside its own params.
			if ((Spec.Allowed & Field::Select) && (Pair.Key.Equals(TEXT("Invert"), ESearchCase::IgnoreCase) || Pair.Key.Equals(TEXT("Grow"), ESearchCase::IgnoreCase)))
			{
				const FParamSpec Extra = Pair.Key.Equals(TEXT("Invert"), ESearchCase::IgnoreCase) ? P(TEXT("Invert"), EParam::Bool, 0, 0, TEXT("false"))
					: P(TEXT("Grow"), EParam::Int, 0, 64, TEXT("0"));
				if (!CheckParam(Extra, Pair.Value, OutError)) return false;
				Step.Params.Add(Pair.Key.ToLower(), Pair.Value);
				continue;
			}
			const FParamSpec* Param = FindParam(Spec, Pair.Key);
			if (!Param)
			{
				TArray<FString> Known;
				for (const FParamSpec& Each : Spec.Params) Known.Add(Each.Name);
				if (Spec.Allowed & Field::Select) Known.Append({TEXT("Invert"), TEXT("Grow")});
				OutError = FString::Printf(TEXT("%s takes no param %s; it takes %s."), Spec.Kind, *Pair.Key, Known.IsEmpty() ? TEXT("none") : *FString::Join(Known, TEXT(", ")));
				return false;
			}
			if (!CheckParam(*Param, Pair.Value, OutError)) return false;
			Step.Params.Add(Pair.Key.ToLower(), Pair.Value);
		}
		Step.Select.bInvert = Step.Bool(TEXT("Invert"), false);
		Step.Select.Grow = Step.Int(TEXT("Grow"), 0);
		for (const FParamSpec& Param : Spec.Params)
		{
			if (FString(Param.Default).IsEmpty() && !Step.Has(Param.Name) && Spec.Kind != FString(TEXT("simplify")))
			{
				OutError = FString::Printf(TEXT("%s needs the param %s."), Spec.Kind, Param.Name);
				return false;
			}
		}
		return true;
	}

	/** Kind-specific rules a spec cannot say: point counts, sizes that must agree, one of several params. */
	bool CheckShape(const FStep& Step, FString& OutError)
	{
		const FString& K = Step.Kind;
		if (K == TEXT("add.revolve"))
		{
			const bool bRing = Step.Float(TEXT("Ring"), 0.0) > 0.0;
			if (Step.Points.Num() < (bRing ? 3 : 2))
			{
				OutError = bRing ? TEXT("a ring's cross-section needs 3 or more points.") : TEXT("a revolve's profile needs 2 or more points (radius,height).");
				return false;
			}
			if (!bRing && Step.Points.ContainsByPredicate([](const FVector2D& Point) { return Point.X < 0.0; }))
			{
				OutError = TEXT("a revolve's profile radii are 0 or more; Points are radius,height.");
				return false;
			}
		}
		if ((K == TEXT("add.sweep") || K == TEXT("add.extrude_polygon")) && Step.Points.Num() < 3)
		{
			OutError = TEXT("the cross-section needs 3 or more points, or circle:radius,sides or rect:width,depth.");
			return false;
		}
		if (K == TEXT("add.torus") && Step.bSize && (Step.Size.Z >= Step.Size.X || Step.Size.Z >= Step.Size.Y))
		{
			OutError = TEXT("a torus's tube (Size Z) must be thinner than it is across (Size X and Y).");
			return false;
		}
		if (K == TEXT("simplify") && int32(Step.Has(TEXT("Triangles"))) + int32(Step.Bool(TEXT("Planar"), false)) + int32(Step.Has(TEXT("Tolerance"))) != 1)
		{
			OutError = TEXT("simplify takes one of Triangles, Planar=true or Tolerance.");
			return false;
		}
		if (K == TEXT("scatter_on") && Step.Float(TEXT("ScaleMin"), 0.7) > Step.Float(TEXT("ScaleMax"), 1.2))
		{
			OutError = TEXT("ScaleMin is at most ScaleMax.");
			return false;
		}
		if (K == TEXT("extrude"))
		{
			const FString Direction = Step.Text(TEXT("Direction"), TEXT("normal"));
			FVector Fixed;
			if (!Direction.Equals(TEXT("normal"), ESearchCase::IgnoreCase) && (!ParseVector(Direction, Fixed) || Fixed.IsNearlyZero()))
			{
				OutError = FString::Printf(TEXT("Direction is normal or x,y,z, not '%s'."), *Direction);
				return false;
			}
		}
		if (K == TEXT("pivot"))
		{
			const FString At = Step.Text(TEXT("At"), TEXT("base"));
			FVector Point;
			if (!At.Equals(TEXT("base"), ESearchCase::IgnoreCase) && !At.Equals(TEXT("center"), ESearchCase::IgnoreCase) && !ParseVector(At, Point))
			{
				OutError = FString::Printf(TEXT("At is base, center or x,y,z, not '%s'."), *At);
				return false;
			}
		}
		return true;
	}

	/** write_asset's path, materials and collision. */
	bool CheckWrite(const FStep& Step, TSet<FString>& Assets, FString& OutError)
	{
		const FString Path = PackageOf(Step.Asset);
		if (!Path.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Path))
		{
			OutError = FString::Printf(TEXT("Asset '%s' is not a path under /Game, like /Game/HyperAI/Models/SM_Rock."), *Step.Asset);
			return false;
		}
		if (Assets.Contains(Path.ToLower()))
		{
			OutError = FString::Printf(TEXT("%s is written twice in this plan."), *Path);
			return false;
		}
		if (Assets.Num() >= C::MaxAssets)
		{
			OutError = FString::Printf(TEXT("a plan writes at most %d assets."), C::MaxAssets);
			return false;
		}
		// Made by the same write when it is missing, and a static mesh cannot share its name.
		if (Path.Equals(C::VertexColorMaterial, ESearchCase::IgnoreCase))
		{
			OutError = FString::Printf(TEXT("%s is the placeholders' vertex-colour material; write the mesh to another path."), C::VertexColorMaterial);
			return false;
		}
		if (PackageExists(Path))
		{
			if (!Step.Bool(TEXT("Replace"), false))
			{
				OutError = FString::Printf(TEXT("%s exists; give Replace=true to overwrite it, or pick a free path."), *Path);
				return false;
			}
			if (!ReplaceTarget(Path, OutError)) return false;
		}
		FString Mode;
		int32 Hulls = 1;
		if (!ParseCollision(Step.Text(TEXT("Collision"), TEXT("convex")), Mode, Hulls))
		{
			OutError = FString::Printf(TEXT("Collision is none, box, convex, hulls:N (1 to 16), auto or complex, not '%s'."), *Step.Text(TEXT("Collision"), TEXT("")));
			return false;
		}
		TMap<int32, FString> Slots;
		if (!ParseMaterials(Step.Text(TEXT("Materials"), TEXT("")), Slots, OutError)) return false;
		Assets.Add(Path.ToLower());
		return true;
	}

	bool Check(const TArray<FHyperAIModelOp>& Ops, FPlan& OutPlan, FString& OutError)
	{
		OutPlan = FPlan();
		if (Ops.IsEmpty() || Ops.Num() > C::MaxOperations)
		{
			OutError = FString::Printf(TEXT("A plan has 1 to %d operations. Nothing was changed."), C::MaxOperations);
			return false;
		}
		// Recipes expand first, so every op below is an ordinary one.
		struct FExpanded
		{
			FHyperAIModelOp Op;
			int32 From = 0;
			bool bFromRecipe = false;
		};
		TArray<FExpanded> Expanded;
		bool bAnyRecipe = false;
		for (int32 Index = 0; Index < Ops.Num(); ++Index)
		{
			const FHyperAIModelOp& Op = Ops[Index];
			const FString Kind = Op.Kind.TrimStartAndEnd().ToLower();
			if (Kind != TEXT("recipe"))
			{
				Expanded.Add({Op, Index, false});
				continue;
			}
			auto Fail = [&](const FString& Message)
			{
				OutError = FString::Printf(TEXT("Op %d (recipe, mesh %s): %s Nothing was changed."), Index, *Op.Mesh, *Message);
				return false;
			};
			const FString Mesh = Op.Mesh.TrimStartAndEnd();
			if (!IsValidMeshName(Mesh) || Mesh.Len() > 36 || Mesh.Contains(TEXT("__")))
			{
				return Fail(TEXT("a recipe's Mesh is 1 to 36 letters, digits, _ or - (no double underscore: its parts take Mesh__part)."));
			}
			FStep Step;
			Step.Spec = FindSpec(TEXT("recipe"));
			Step.Kind = Kind;
			FString Error;
			if (!ParseStep(Op, Step, Error)) return Fail(Error);
			Recipes::FParams Params;
			Params.Mesh = Mesh;
			Params.Shape = Step.Text(TEXT("Shape"), TEXT("")).ToLower();
			const Recipes::FInfo* Info = Recipes::Find(Params.Shape);
			if (!Info)
			{
				TArray<FString> Names;
				for (const Recipes::FInfo& Each : Recipes::List()) Names.Add(Each.Name);
				return Fail(FString::Printf(TEXT("Shape '%s' is not a recipe; recipes are %s."), *Params.Shape, *FString::Join(Names, TEXT(", "))));
			}
			Params.Size = Step.Float(TEXT("Size"), Info->DefaultSize);
			Params.Seed = Step.Int(TEXT("Seed"), 0);
			Params.Detail = Step.Int(TEXT("Detail"), 2);
			Params.Tint = Step.Color(TEXT("Tint"), FLinearColor::Transparent);
			Params.Accent = Step.Color(TEXT("Accent"), FLinearColor::Transparent);
			Params.Location = Op.Location;
			Params.Rotation = Op.Rotation;
			Params.Scale = Op.Scale;
			TArray<FHyperAIModelOp> Made;
			if (!Recipes::Expand(Params, Made, Error)) return Fail(Error);
			for (FHyperAIModelOp& Each : Made) Expanded.Add({MoveTemp(Each), Index, true});
			bAnyRecipe = true;
		}
		if (Expanded.Num() > C::MaxExpandedOperations)
		{
			OutError = FString::Printf(TEXT("The plan expands to %d ops; at most %d after recipes. Split it into two plans. Nothing was changed."), Expanded.Num(), C::MaxExpandedOperations);
			return false;
		}
		TMap<FString, int64> Tris;
		TMap<FString, FString> Names;
		TSet<FString> Assets;
		TArray<FString> HashLines;
		for (int32 Index = 0; Index < Expanded.Num(); ++Index)
		{
			const FHyperAIModelOp& Op = Expanded[Index].Op;
			FStep& Step = OutPlan.Steps.AddDefaulted_GetRef();
			Step.Index = Index;
			Step.From = Expanded[Index].From;
			Step.bFromRecipe = Expanded[Index].bFromRecipe;
			Step.Kind = Op.Kind.TrimStartAndEnd().ToLower();
			Step.Mesh = Op.Mesh.TrimStartAndEnd();
			auto Fail = [&](const FString& Message)
			{
				OutError = FString::Printf(TEXT("%s: %s Nothing was changed."), *Step.Describe(), *Message);
				return false;
			};
			Step.Spec = FindSpec(Step.Kind);
			if (!Step.Spec || Step.Kind == TEXT("recipe"))
			{
				TArray<FString> Kinds;
				for (const FKindSpec& Spec : Specs()) Kinds.Add(Spec.Kind);
				return Fail(FString::Printf(TEXT("'%s' is not an op kind; kinds are %s."), *Op.Kind, *FString::Join(Kinds, TEXT(", "))));
			}
			if (!IsValidMeshName(Step.Mesh)) return Fail(TEXT("Mesh is 1 to 48 letters, digits, _ or -."));
			if (!Step.bFromRecipe && Step.Mesh.Contains(TEXT("__"))) return Fail(TEXT("names with a double underscore are recipes' parts; pick another name."));
			FString Error;
			if (!ParseStep(Op, Step, Error) || !CheckShape(Step, Error)) return Fail(Error);
			const FString Key = KeyOf(Step.Mesh);
			const bool bMakes = Step.Kind.StartsWith(TEXT("add.")) || Step.Kind == TEXT("append") || Step.Kind == TEXT("copy")
				|| Step.Kind == TEXT("array.linear") || Step.Kind == TEXT("array.radial");
			if (!bMakes && !Tris.Contains(Key))
			{
				TArray<FString> Known;
				Names.GenerateValueArray(Known);
				return Fail(FString::Printf(TEXT("no earlier op made mesh %s; meshes so far: %s."), *Step.Mesh, Known.IsEmpty() ? TEXT("none") : *FString::Join(Known, TEXT(", "))));
			}
			if (Step.Spec->Required & Field::Source)
			{
				if (!IsValidMeshName(Step.Source) || !Tris.Contains(KeyOf(Step.Source)))
				{
					return Fail(FString::Printf(TEXT("Source %s is not a mesh an earlier op made."), *Step.Source));
				}
				if (KeyOf(Step.Source) == Key && Step.Kind != TEXT("append") && Step.Kind != TEXT("copy")) return Fail(TEXT("Source is another mesh than Mesh."));
			}
			if (Step.Kind == TEXT("write_asset"))
			{
				if (!CheckWrite(Step, Assets, Error)) return Fail(Error);
				const int64 Predicted = Tris.FindRef(Key);
				const bool bNanite = Step.Bool(TEXT("Nanite"), false);
				const int64 Cap = bNanite ? C::MaxTrianglesPerNaniteAsset : C::MaxTrianglesPerAsset;
				if (Predicted > Cap)
				{
					return Fail(FString::Printf(TEXT("%s would hold about %lld triangles, over the %lld an asset may have%s."), *Step.Mesh, Predicted, Cap,
						bNanite ? TEXT("") : TEXT(" without Nanite; simplify it, or write it with Nanite=true")));
				}
				++OutPlan.Writes;
			}
			if (Step.Kind == TEXT("drop"))
			{
				Tris.Remove(Key);
				Names.Remove(Key);
			}
			else
			{
				const int64 Predicted = Predict(Step, Tris);
				if (Predicted > C::MaxTrianglesPerMesh)
				{
					return Fail(FString::Printf(TEXT("it would make about %lld triangles in %s, over the %d one mesh may hold; lower Steps, Level, Count or Resolution, or simplify first."),
						Predicted, *Step.Mesh, C::MaxTrianglesPerMesh));
				}
				Tris.Add(Key, Predicted);
				Names.Add(Key, Step.Mesh);
			}
			if (Tris.Num() > C::MaxMeshes) return Fail(FString::Printf(TEXT("more than %d working meshes at once; drop the ones you are done with."), C::MaxMeshes));
			int64 Total = 0;
			for (const TPair<FString, int64>& Pair : Tris) Total += Pair.Value;
			if (Total > C::MaxTrianglesPerPlan)
			{
				return Fail(FString::Printf(TEXT("the plan's meshes would hold about %lld triangles together, over %d; drop or simplify meshes."), Total, C::MaxTrianglesPerPlan));
			}
			HashLines.Add(OpLine(Index, Op));
			if (bAnyRecipe) OutPlan.ExpandedLines.Add(HashLines.Last() + (Step.bFromRecipe ? FString::Printf(TEXT("  <- op %d recipe"), Step.From) : FString()));
		}
		const FTCHARToUTF8 Canonical(*FString::Join(HashLines, TEXT("\n")));
		OutPlan.Hash = FSHA1::HashBuffer(Canonical.Get(), Canonical.Length()).ToString().Left(16).ToLower();
		return true;
	}

	// --- Working meshes --------------------------------------------------------------------------------------------

	FBuilt::FBuilt()
		: Pool(NewObject<UDynamicMeshPool>(GetTransientPackage()))
	{
	}

	FBuilt::~FBuilt()
	{
		if (Pool.IsValid()) Pool->FreeAllMeshes();
	}

	UDynamicMesh* FBuilt::Find(const FString& Name) const
	{
		UDynamicMesh* const* Found = Meshes.Find(KeyOf(Name));
		return Found ? *Found : nullptr;
	}

	UDynamicMesh* FBuilt::FindOrAdd(const FString& Name)
	{
		if (UDynamicMesh* Found = Find(Name)) return Found;
		UDynamicMesh* Mesh = Pool->RequestMesh();
		Meshes.Add(KeyOf(Name), Mesh);
		Names.Add(Name);
		return Mesh;
	}

	void FBuilt::Remove(const FString& Name)
	{
		UDynamicMesh* Mesh = nullptr;
		if (Meshes.RemoveAndCopyValue(KeyOf(Name), Mesh) && Mesh) Pool->ReturnMesh(Mesh);
		Names.RemoveAll([&Name](const FString& Each) { return Each.Equals(Name, ESearchCase::IgnoreCase); });
	}

	UDynamicMesh* FBuilt::RequestTemp()
	{
		return Pool->RequestMesh();
	}

	void FBuilt::ReturnTemp(UDynamicMesh* Mesh)
	{
		if (Mesh) Pool->ReturnMesh(Mesh);
	}

	void FBuilt::Keep(const int32 StepIndex, const UDynamicMesh& Mesh)
	{
		UDynamicMesh* Copy = Pool->RequestMesh();
		Copy->SetMesh(Mesh.GetMeshRef());
		Writes.Add(StepIndex, Copy);
	}

	UDynamicMesh* FBuilt::Kept(const int32 StepIndex) const
	{
		UDynamicMesh* const* Found = Writes.Find(StepIndex);
		return Found ? *Found : nullptr;
	}

	// --- Building --------------------------------------------------------------------------------------------------

	using UPrim = UGeometryScriptLibrary_MeshPrimitiveFunctions;
	using UEdit = UGeometryScriptLibrary_MeshBasicEditFunctions;
	using UQuery = UGeometryScriptLibrary_MeshQueryFunctions;
	using USel = UGeometryScriptLibrary_MeshSelectionFunctions;
	using UXform = UGeometryScriptLibrary_MeshTransformFunctions;
	using UNormals = UGeometryScriptLibrary_MeshNormalsFunctions;
	using UColor = UGeometryScriptLibrary_MeshVertexColorFunctions;

	FGeometryScriptColorFlags AllChannels()
	{
		return FGeometryScriptColorFlags();
	}

	FGeometryScriptCalculateNormalsOptions NormalOptions()
	{
		return FGeometryScriptCalculateNormalsOptions();
	}

	/** The colour a mesh's first vertex carries, to restore after an op that rebuilds the surface without colours. */
	FLinearColor FirstColor(UDynamicMesh& Mesh)
	{
		FLinearColor Out = DefaultColor();
		Mesh.ProcessMesh([&Out](const FDynamicMesh3& Read)
		{
			const UE::Geometry::FDynamicMeshColorOverlay* Colors = Read.HasAttributes() ? Read.Attributes()->PrimaryColors() : nullptr;
			if (Colors && Colors->ElementCount() > 0)
			{
				for (const int32 Element : Colors->ElementIndicesItr())
				{
					const FVector4f Value = Colors->GetElement(Element);
					Out = FLinearColor(Value.X, Value.Y, Value.Z, Value.W);
					break;
				}
			}
		});
		return Out;
	}

	/** After a voxel op, which rebuilds the surface bare: attributes, UV channel 0, normals and a colour again. */
	void Refresh(UDynamicMesh& Mesh, const FLinearColor& Color, UGeometryScriptDebug* Debug)
	{
		Mesh.EditMesh([](FDynamicMesh3& Edit)
		{
			if (!Edit.HasAttributes()) Edit.EnableAttributes();
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
		if (UQuery::GetNumUVSets(&Mesh) < 1) UGeometryScriptLibrary_MeshUVFunctions::SetNumUVSets(&Mesh, 1, Debug);
		UGeometryScriptLibrary_MeshMaterialFunctions::EnableMaterialIDs(&Mesh, Debug);
		UNormals::SetPerVertexNormals(&Mesh, Debug);
		UColor::SetMeshConstantVertexColor(&Mesh, Color, AllChannels(), /*bClearExisting=*/true, Debug);
	}

	bool MakeSelection(UDynamicMesh& Mesh, const FSelector& Selector, FGeometryScriptMeshSelection& Out)
	{
		const EGeometryScriptMeshSelectionType Triangles = EGeometryScriptMeshSelectionType::Triangles;
		switch (Selector.Type)
		{
		case FSelector::EType::Normal:
			USel::SelectMeshElementsByNormalAngle(&Mesh, Out, Selector.A, Selector.Value, Triangles, false, 3);
			break;
		case FSelector::EType::Box:
			USel::SelectMeshElementsInBox(&Mesh, Out, FBox(Selector.A, Selector.B), Triangles, false, 3);
			break;
		case FSelector::EType::Sphere:
			USel::SelectMeshElementsInSphere(&Mesh, Out, Selector.A, Selector.Value, Triangles, false, 3);
			break;
		case FSelector::EType::Plane:
			USel::SelectMeshElementsWithPlane(&Mesh, Out, Selector.A, Selector.B, Triangles, false, 3);
			break;
		case FSelector::EType::Material:
			USel::SelectMeshElementsByMaterialID(&Mesh, FMath::RoundToInt(Selector.Value), Out, Triangles);
			break;
		case FSelector::EType::All:
		default:
			USel::CreateSelectAllMeshSelection(&Mesh, Out, Triangles);
			break;
		}
		if (Selector.bInvert)
		{
			FGeometryScriptMeshSelection Inverted;
			USel::InvertMeshSelection(&Mesh, Out, Inverted, false);
			Out = Inverted;
		}
		if (Selector.Grow > 0)
		{
			FGeometryScriptMeshSelection Grown;
			USel::ExpandContractMeshSelection(&Mesh, Out, Grown, Selector.Grow, false, false);
			Out = Grown;
		}
		return !Out.IsEmpty();
	}

	EGeometryScriptPrimitivePolygroupMode GroupsOf(const FStep& Step)
	{
		const FString Groups = Step.Text(TEXT("Groups"), TEXT("face")).ToLower();
		return Groups == TEXT("quad") ? EGeometryScriptPrimitivePolygroupMode::PerQuad
			: Groups == TEXT("single") ? EGeometryScriptPrimitivePolygroupMode::SingleGroup : EGeometryScriptPrimitivePolygroupMode::PerFace;
	}

	/** An add.* shape built on its own, set on its pivot, coloured, then added into Mesh where the op places it. */
	bool AddShape(const FStep& Step, FBuilt& Built, UDynamicMesh& Target, UGeometryScriptDebug* Debug, FString& OutError)
	{
		UDynamicMesh* Part = Built.RequestTemp();
		ON_SCOPE_EXIT { Built.ReturnTemp(Part); };
		FGeometryScriptPrimitiveOptions Options;
		Options.PolygroupMode = GroupsOf(Step);
		Options.MaterialID = Step.Int(TEXT("Material"), 0);
		const FTransform Identity = FTransform::Identity;
		const EGeometryScriptPrimitiveOriginMode Base = EGeometryScriptPrimitiveOriginMode::Base;
		const FString& K = Step.Kind;
		FVector Size = Step.bSize ? Step.Size : FVector(100.0);
		// Round shapes are made 100 cm across and scaled to Size, so an ellipse is as easy as a circle.
		FVector Stretch = FVector::OneVector;
		bool bRecentre = true;
		if (K == TEXT("add.box"))
		{
			const int32 Steps = Step.Int(TEXT("Steps"), 0);
			UPrim::AppendBox(Part, Options, Identity, Size.X, Size.Y, Size.Z, Steps, Steps, Steps, Base, Debug);
		}
		else if (K == TEXT("add.sphere"))
		{
			if (Step.Text(TEXT("Style"), TEXT("latlong")).Equals(TEXT("box"), ESearchCase::IgnoreCase))
			{
				const int32 Steps = Step.Int(TEXT("Steps"), 8);
				UPrim::AppendSphereBox(Part, Options, Identity, 50.f, Steps, Steps, Steps, EGeometryScriptPrimitiveOriginMode::Center, Debug);
			}
			else
			{
				const int32 Steps = Step.Int(TEXT("Steps"), 16);
				UPrim::AppendSphereLatLong(Part, Options, Identity, 50.f, FMath::Max(3, Steps / 2), Steps, EGeometryScriptPrimitiveOriginMode::Center, Debug);
			}
			Stretch = Size / 100.0;
		}
		else if (K == TEXT("add.cylinder"))
		{
			UPrim::AppendCylinder(Part, Options, Identity, 50.f, Size.Z, Step.Int(TEXT("Steps"), 16), Step.Int(TEXT("HeightSteps"), 0), Step.Bool(TEXT("Capped"), true), Base, Debug);
			Stretch = FVector(Size.X / 100.0, Size.Y / 100.0, 1.0);
		}
		else if (K == TEXT("add.cone"))
		{
			UPrim::AppendCone(Part, Options, Identity, 50.f, 50.f * Step.Float(TEXT("Top"), 0.05), Size.Z, Step.Int(TEXT("Steps"), 16), Step.Int(TEXT("HeightSteps"), 1),
				Step.Bool(TEXT("Capped"), true), Base, Debug);
			Stretch = FVector(Size.X / 100.0, Size.Y / 100.0, 1.0);
		}
		else if (K == TEXT("add.capsule"))
		{
			if (!Step.bSize) Size = FVector(50.0, 50.0, 150.0);
			const double Radius = FMath::Min(Size.X, Size.Y) * 0.5;
			const double Line = FMath::Max(Size.Z - 2.0 * Radius, 0.0);
			UPrim::AppendCapsule(Part, Options, Identity, Radius, Line, Step.Int(TEXT("HemisphereSteps"), 4), Step.Int(TEXT("Steps"), 16), Step.Int(TEXT("HeightSteps"), 0), Base, Debug);
			Stretch = FVector(Size.X / (2.0 * Radius), Size.Y / (2.0 * Radius), Size.Z < 2.0 * Radius ? Size.Z / (2.0 * Radius) : 1.0);
		}
		else if (K == TEXT("add.torus"))
		{
			if (!Step.bSize) Size = FVector(100.0, 100.0, 25.0);
			FGeometryScriptRevolveOptions Revolve;
			Revolve.RevolveDegrees = Step.Float(TEXT("Degrees"), 360.0);
			const double Minor = Size.Z * 0.5;
			const double Major = Size.X * 0.5 - Minor;
			UPrim::AppendTorus(Part, Options, Identity, Revolve, Major, Minor, Step.Int(TEXT("Steps"), 24), Step.Int(TEXT("MinorSteps"), 12), Base, Debug);
			Stretch = FVector(1.0, Size.Y / Size.X, 1.0);
		}
		else if (K == TEXT("add.disc"))
		{
			UPrim::AppendDisc(Part, Options, Identity, 50.f, Step.Int(TEXT("Steps"), 24), 0, 0.f, 360.f, 50.f * Step.Float(TEXT("Hole"), 0.0), Debug);
			Stretch = FVector(Size.X / 100.0, Size.Y / 100.0, 1.0);
		}
		else if (K == TEXT("add.stairs"))
		{
			if (!Step.bSize) Size = FVector(240.0, 100.0, 160.0);
			const int32 Steps = Step.Int(TEXT("Steps"), 8);
			UPrim::AppendLinearStairs(Part, Options, Identity, Size.Y, Size.Z / Steps, Size.X / Steps, Steps, Step.Bool(TEXT("Floating"), false), Debug);
		}
		else if (K == TEXT("add.curved_stairs"))
		{
			if (!Step.bSize) Size = FVector(0.0, 100.0, 160.0);
			const int32 Steps = Step.Int(TEXT("Steps"), 8);
			UPrim::AppendCurvedStairs(Part, Options, Identity, Size.Y, Size.Z / Steps, Step.Float(TEXT("InnerRadius"), 150.0), Step.Float(TEXT("Angle"), 90.0), Steps,
				Step.Bool(TEXT("Floating"), false), Debug);
		}
		else if (K == TEXT("add.revolve"))
		{
			bRecentre = false;
			FGeometryScriptRevolveOptions Revolve;
			Revolve.RevolveDegrees = Step.Float(TEXT("Degrees"), 360.0);
			Revolve.bHardNormals = Step.Bool(TEXT("Hard"), false);
			const double Ring = Step.Float(TEXT("Ring"), 0.0);
			if (Ring > 0.0)
			{
				UPrim::AppendRevolvePolygon(Part, Options, Identity, Step.Points, Revolve, Ring, Step.Int(TEXT("Steps"), 24), Debug);
			}
			else
			{
				// A profile that starts or ends on the axis would leave a pinched, open pole there: those ends move a hair
				// off it, so the caps close them.
				TArray<FVector2D> Profile = Step.Points;
				double Widest = 0.0;
				for (const FVector2D& Point : Profile) Widest = FMath::Max(Widest, Point.X);
				const double Hair = FMath::Max(Widest * 0.002, 0.01);
				for (FVector2D* End : {&Profile[0], &Profile.Last()})
				{
					End->X = FMath::Max(End->X, Hair);
				}
				UPrim::AppendRevolvePath(Part, Options, Identity, Profile, Revolve, Step.Int(TEXT("Steps"), 24), Step.Bool(TEXT("Capped"), true), Debug);
			}
		}
		else if (K == TEXT("add.sweep"))
		{
			bRecentre = false;
			UPrim::AppendSimpleSweptPolygon(Part, Options, Identity, Step.Points, Step.Path, Step.Bool(TEXT("Loop"), false), Step.Bool(TEXT("Capped"), true),
				Step.Float(TEXT("StartScale"), 1.0), Step.Float(TEXT("EndScale"), 1.0), Step.Float(TEXT("Twist"), 0.0), 1.f, Debug);
		}
		else if (K == TEXT("add.extrude_polygon"))
		{
			bRecentre = false;
			UPrim::AppendSimpleExtrudePolygon(Part, Options, Identity, Step.Points, Step.Float(TEXT("Height"), 100.0), Step.Int(TEXT("HeightSteps"), 0),
				Step.Bool(TEXT("Capped"), true), Base, Debug);
		}
		if (Part->GetTriangleCount() == 0)
		{
			OutError = TEXT("the shape came out empty; check its Size and Points.");
			return false;
		}
		if (!Stretch.Equals(FVector::OneVector)) UXform::ScaleMesh(Part, Stretch, FVector::ZeroVector, true, Debug);
		// Profiles and outlines face outward whichever way their points run: a closed shape inside out is turned right.
		if (UQuery::GetNumOpenBorderEdges(Part) == 0)
		{
			float Area = 0.f;
			float Volume = 0.f;
			UQuery::GetMeshVolumeArea(Part, Area, Volume);
			if (Volume < 0.f) UNormals::FlipNormals(Part, Debug);
		}
		if (bRecentre)
		{
			const FBox Box = UQuery::GetMeshBoundingBox(Part);
			const bool bCenter = Step.Text(TEXT("Origin"), TEXT("base")).Equals(TEXT("center"), ESearchCase::IgnoreCase);
			const FVector Pivot = bCenter ? Box.GetCenter() : FVector(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z);
			UXform::TranslateMesh(Part, -Pivot, Debug);
		}
		UColor::SetMeshConstantVertexColor(Part, Step.Color(TEXT("Color"), DefaultColor()), AllChannels(), /*bClearExisting=*/true, Debug);
		UEdit::AppendMesh(&Target, Part, Step.Transform(), false, FGeometryScriptAppendMeshOptions(), Debug);
		return true;
	}

	/** scatter_on's transforms: samples of Mesh's surface facing up enough, each with a random turn, lean and scale. */
	TArray<FTransform> ScatterTransforms(const FStep& Step, UDynamicMesh& Surface, UDynamicMesh& Piece, UGeometryScriptDebug* Debug)
	{
		const int32 Count = Step.Int(TEXT("Count"), 12);
		const FBox PieceBox = UQuery::GetMeshBoundingBox(&Piece);
		const double Spacing = Step.Has(TEXT("Spacing")) ? Step.Float(TEXT("Spacing"), 10.0) : FMath::Max(PieceBox.GetSize().X, PieceBox.GetSize().Y) * 0.8 + 0.1;
		const int32 Seed = Step.Int(TEXT("Seed"), 0);
		FGeometryScriptMeshPointSamplingOptions Options;
		Options.SamplingRadius = FMath::Max(Spacing * 0.5, 0.05);
		Options.MaxNumSamples = Count * 6;
		Options.RandomSeed = Seed;
		TArray<FTransform> Samples;
		FGeometryScriptIndexList Triangles;
		UGeometryScriptLibrary_MeshSamplingFunctions::ComputePointSampling(&Surface, Options, Samples, Triangles, Debug);
		FRandomStream Random(Seed);
		const double MinUp = FMath::Cos(FMath::DegreesToRadians(Step.Float(TEXT("Up"), 180.0)));
		const bool bAlign = Step.Text(TEXT("Align"), TEXT("normal")).Equals(TEXT("normal"), ESearchCase::IgnoreCase);
		const double Tilt = Step.Float(TEXT("Tilt"), 15.0);
		const double ScaleMin = Step.Float(TEXT("ScaleMin"), 0.7);
		const double ScaleMax = Step.Float(TEXT("ScaleMax"), 1.2);
		const double Sink = Step.Float(TEXT("Sink"), 0.0);
		TArray<FTransform> Out;
		for (const FTransform& Sample : Samples)
		{
			if (Out.Num() >= Count) break;
			const FVector Normal = Sample.GetUnitAxis(EAxis::Z);
			if (Normal.Z + 1e-6 < MinUp) continue;
			const FQuat Facing = bAlign ? FQuat::FindBetweenNormals(FVector::UpVector, Normal) : FQuat::Identity;
			const FQuat Turn(FVector::UpVector, Random.FRandRange(0.0, 2.0 * UE_DOUBLE_PI));
			const double LeanAxis = Random.FRandRange(0.0, 2.0 * UE_DOUBLE_PI);
			const FQuat Lean(FVector(FMath::Cos(LeanAxis), FMath::Sin(LeanAxis), 0.0), FMath::DegreesToRadians(Random.FRandRange(0.0, Tilt)));
			const double Scale = Random.FRandRange(ScaleMin, ScaleMax);
			Out.Add(FTransform(Facing * Lean * Turn, Sample.GetLocation() - Normal * Sink, FVector(Scale)));
		}
		return Out;
	}

	/** array.radial's transforms: Count about Location's Z axis, Radius out. */
	TArray<FTransform> RadialTransforms(const FStep& Step)
	{
		const int32 Count = Step.Int(TEXT("Count"), 1);
		const double Radius = Step.Float(TEXT("Radius"), 0.0);
		const double Angle = Step.Float(TEXT("Angle"), 360.0);
		const bool bAlign = Step.Bool(TEXT("Align"), true);
		const double Jitter = Step.Float(TEXT("Jitter"), 0.0);
		FRandomStream Random(Step.Int(TEXT("Seed"), 0));
		const double Spread = Angle >= 359.999 ? Angle / Count : Angle / FMath::Max(Count - 1, 1);
		TArray<FTransform> Out;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const double Yaw = Index * Spread + Random.FRandRange(-1.0, 1.0) * Jitter * Spread * 0.4;
			const double Scale = 1.0 + Random.FRandRange(-1.0, 1.0) * Jitter * 0.35;
			const double Lean = Random.FRandRange(-1.0, 1.0) * Jitter * 15.0;
			const FRotator Facing(Lean, bAlign ? Yaw : 0.0, 0.0);
			const FVector Offset = FRotator(0.0, Yaw, 0.0).RotateVector(FVector(Radius, 0.0, 0.0));
			Out.Add(FTransform(Facing, Step.Location + Offset, FVector(Scale)));
		}
		return Out;
	}

	/** A warp's frame: Location (or the mesh's base, or center for flare) turned by Rotation. */
	FTransform WarpFrame(const FStep& Step, const FBox& Box, const bool bCenter)
	{
		const FVector Origin = Step.bLocation ? Step.Location : (bCenter ? Box.GetCenter() : FVector(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z));
		return FTransform(Step.Rotation, Origin);
	}

	EGeometryScriptPolyOperationArea AreaOf(const FStep& Step)
	{
		const FString Area = Step.Text(TEXT("Area"), TEXT("all")).ToLower();
		return Area == TEXT("group") ? EGeometryScriptPolyOperationArea::PerPolygroup
			: Area == TEXT("face") ? EGeometryScriptPolyOperationArea::PerTriangle : EGeometryScriptPolyOperationArea::EntireSelection;
	}

	bool RunStep(const FStep& Step, FBuilt& Built, UGeometryScriptDebug* Debug, FString& OutError)
	{
		const FString& K = Step.Kind;
		if (K == TEXT("write_asset"))
		{
			const UDynamicMesh* Written = Built.Find(Step.Mesh);
			if (!Written)
			{
				OutError = FString::Printf(TEXT("mesh %s does not exist here."), *Step.Mesh);
				return false;
			}
			Built.Keep(Step.Index, *Written);
			return true;
		}
		if (K == TEXT("drop"))
		{
			Built.Remove(Step.Mesh);
			return true;
		}
		UDynamicMesh* Source = Step.Source.IsEmpty() ? nullptr : Built.Find(Step.Source);
		if (K == TEXT("copy"))
		{
			// Copied into a temporary first, so copying a mesh onto itself works.
			UDynamicMesh* Temp = Built.RequestTemp();
			UEdit::AppendMesh(Temp, Source, Step.Transform(), false, FGeometryScriptAppendMeshOptions(), Debug);
			UDynamicMesh* Target = Built.FindOrAdd(Step.Mesh);
			Target->SetMesh(Temp->GetMeshRef());
			Built.ReturnTemp(Temp);
			return true;
		}
		const bool bMakes = K.StartsWith(TEXT("add.")) || K == TEXT("append") || K == TEXT("array.linear") || K == TEXT("array.radial");
		UDynamicMesh* Mesh = bMakes ? Built.FindOrAdd(Step.Mesh) : Built.Find(Step.Mesh);
		if (!Mesh)
		{
			OutError = FString::Printf(TEXT("mesh %s does not exist here."), *Step.Mesh);
			return false;
		}
		if (K.StartsWith(TEXT("add."))) return AddShape(Step, Built, *Mesh, Debug, OutError);
		if (K == TEXT("append"))
		{
			UEdit::AppendMesh(Mesh, Source, Step.Transform(), false, FGeometryScriptAppendMeshOptions(), Debug);
			return true;
		}
		if (K == TEXT("array.linear"))
		{
			UEdit::AppendMeshRepeated(Mesh, Source, Step.Transform(), Step.Int(TEXT("Count"), 1), /*bApplyTransformToFirstInstance=*/false, false, FGeometryScriptAppendMeshOptions(), Debug);
			return true;
		}
		if (K == TEXT("array.radial"))
		{
			UEdit::AppendMeshTransformed(Mesh, Source, RadialTransforms(Step), FTransform::Identity, true, false, FGeometryScriptAppendMeshOptions(), Debug);
			return true;
		}
		if (K == TEXT("scatter_on"))
		{
			const TArray<FTransform> Transforms = ScatterTransforms(Step, *Mesh, *Source, Debug);
			if (Transforms.IsEmpty())
			{
				OutError = TEXT("no spot on the surface fits: lower Spacing, or raise Up.");
				return false;
			}
			UEdit::AppendMeshTransformed(Mesh, Source, Transforms, FTransform::Identity, true, false, FGeometryScriptAppendMeshOptions(), Debug);
			return true;
		}
		if (K == TEXT("boolean"))
		{
			const FString Op = Step.Text(TEXT("Op"), TEXT("union")).ToLower();
			FGeometryScriptMeshBooleanOptions Options;
			Options.bFillHoles = Step.Bool(TEXT("Fill"), true);
			Options.bAllowEmptyResult = Step.Bool(TEXT("AllowEmpty"), false);
			const EGeometryScriptBooleanOperation Operation = Op == TEXT("subtract") ? EGeometryScriptBooleanOperation::Subtract
				: Op == TEXT("intersect") ? EGeometryScriptBooleanOperation::Intersection : EGeometryScriptBooleanOperation::Union;
			UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshBoolean(Mesh, FTransform::Identity, Source, FTransform::Identity, Operation, Options, Debug);
			if (Mesh->GetTriangleCount() == 0 && !Options.bAllowEmptyResult)
			{
				OutError = TEXT("the boolean left nothing; check the meshes overlap, or give AllowEmpty=true.");
				return false;
			}
			return true;
		}
		if (K == TEXT("self_union"))
		{
			FGeometryScriptMeshSelfUnionOptions Options;
			Options.bFillHoles = Step.Bool(TEXT("Fill"), true);
			UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshSelfUnion(Mesh, Options, Debug);
			return true;
		}
		if (K == TEXT("plane_cut"))
		{
			FGeometryScriptMeshPlaneCutOptions Options;
			Options.bFillHoles = Step.Bool(TEXT("Fill"), true);
			const FVector Normal = Step.Vector(TEXT("Normal"), FVector::UpVector).GetSafeNormal();
			UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshPlaneCut(Mesh, FTransform(FQuat::FindBetweenNormals(FVector::UpVector, Normal), Step.Location), Options, Debug);
			if (Mesh->GetTriangleCount() == 0)
			{
				OutError = TEXT("the cut removed the whole mesh; the plane's Normal points to the side that goes.");
				return false;
			}
			return true;
		}
		if (K == TEXT("mirror"))
		{
			FGeometryScriptMeshMirrorOptions Options;
			Options.bApplyPlaneCut = Step.Bool(TEXT("Cut"), true);
			Options.bWeldAlongPlane = Step.Bool(TEXT("Weld"), true);
			const FVector Normal = Step.Vector(TEXT("Normal"), FVector::ForwardVector).GetSafeNormal();
			UGeometryScriptLibrary_MeshBooleanFunctions::ApplyMeshMirror(Mesh, FTransform(FQuat::FindBetweenNormals(FVector::UpVector, Normal), Step.Location), Options, Debug);
			return true;
		}
		FGeometryScriptMeshSelection Selection;
		const bool bSelecting = (Step.Spec->Allowed & Field::Select) != 0;
		if (bSelecting && Step.Select.bGiven && !MakeSelection(*Mesh, Step.Select, Selection))
		{
			OutError = TEXT("Select picks no faces of this mesh; check it against the mesh's bounds (a dry run reports them).");
			return false;
		}
		if (bSelecting && !Step.Select.bGiven && K != TEXT("bevel") && K != TEXT("noise") && K != TEXT("smooth"))
		{
			MakeSelection(*Mesh, FSelector(), Selection);
		}
		using UModel = UGeometryScriptLibrary_MeshModelingFunctions;
		using UDeform = UGeometryScriptLibrary_MeshDeformFunctions;
		if (K == TEXT("extrude"))
		{
			FGeometryScriptMeshLinearExtrudeOptions Options;
			Options.Distance = Step.Float(TEXT("Distance"), 10.0);
			const FString Direction = Step.Text(TEXT("Direction"), TEXT("normal"));
			FVector Fixed;
			if (ParseVector(Direction, Fixed))
			{
				Options.DirectionMode = EGeometryScriptLinearExtrudeDirection::FixedDirection;
				Options.Direction = Fixed.GetSafeNormal();
			}
			else
			{
				Options.DirectionMode = EGeometryScriptLinearExtrudeDirection::AverageFaceNormal;
			}
			Options.AreaMode = AreaOf(Step);
			UModel::ApplyMeshLinearExtrudeFaces(Mesh, Options, Selection, Debug);
			return true;
		}
		if (K == TEXT("inset"))
		{
			FGeometryScriptMeshInsetOutsetFacesOptions Options;
			Options.Distance = Step.Float(TEXT("Distance"), 5.0);
			Options.AreaMode = AreaOf(Step);
			UModel::ApplyMeshInsetOutsetFaces(Mesh, Options, Selection, Debug);
			return true;
		}
		if (K == TEXT("bevel"))
		{
			if (Step.Select.bGiven)
			{
				FGeometryScriptMeshBevelSelectionOptions Options;
				Options.BevelDistance = Step.Float(TEXT("Distance"), 2.0);
				Options.Subdivisions = Step.Int(TEXT("Subdivisions"), 0);
				Options.RoundWeight = Step.Float(TEXT("Round"), 1.0);
				UModel::ApplyMeshBevelSelection(Mesh, Selection, EGeometryScriptMeshBevelSelectionMode::TriangleArea, Options, Debug);
			}
			else
			{
				FGeometryScriptMeshBevelOptions Options;
				Options.BevelDistance = Step.Float(TEXT("Distance"), 2.0);
				Options.Subdivisions = Step.Int(TEXT("Subdivisions"), 0);
				Options.RoundWeight = Step.Float(TEXT("Round"), 1.0);
				UModel::ApplyMeshPolygroupBevel(Mesh, Options, Debug);
			}
			return true;
		}
		if (K == TEXT("shell"))
		{
			FGeometryScriptMeshOffsetOptions Options;
			Options.OffsetDistance = Step.Float(TEXT("Thickness"), 2.0);
			UModel::ApplyMeshShell(Mesh, Options, Debug);
			return true;
		}
		const FBox Box = UQuery::GetMeshBoundingBox(Mesh);
		if (K == TEXT("noise"))
		{
			FGeometryScriptPerlinNoiseOptions Options;
			Options.BaseLayer.Magnitude = Step.Float(TEXT("Magnitude"), 5.0);
			Options.BaseLayer.Frequency = Step.Float(TEXT("Frequency"), 0.02);
			Options.BaseLayer.RandomSeed = Step.Int(TEXT("Seed"), 0);
			Options.bApplyAlongNormal = true;
			UDeform::ApplyPerlinNoiseToMesh2(Mesh, Selection, Options, Debug);
			UNormals::RecomputeNormals(Mesh, NormalOptions(), false, Debug);
			return true;
		}
		if (K == TEXT("bend") || K == TEXT("twist"))
		{
			const double Extent = Step.Float(TEXT("Extent"), FMath::Max(Box.GetSize().Z, 1.0));
			const FTransform Frame = WarpFrame(Step, Box, false);
			if (K == TEXT("bend"))
			{
				FGeometryScriptBendWarpOptions Options;
				Options.bBidirectional = !Step.Bool(TEXT("Lock"), true);
				UDeform::ApplyBendWarpToMesh(Mesh, Options, Frame, Step.Float(TEXT("Angle"), 0.0), Extent, Debug);
			}
			else
			{
				FGeometryScriptTwistWarpOptions Options;
				Options.bBidirectional = !Step.Bool(TEXT("Lock"), true);
				UDeform::ApplyTwistWarpToMesh(Mesh, Options, Frame, Step.Float(TEXT("Angle"), 0.0), Extent, Debug);
			}
			UNormals::RecomputeNormals(Mesh, NormalOptions(), false, Debug);
			return true;
		}
		if (K == TEXT("flare"))
		{
			FGeometryScriptFlareWarpOptions Options;
			const FString Mode = Step.Text(TEXT("Mode"), TEXT("sin")).ToLower();
			Options.FlareType = Mode == TEXT("square") ? EGeometryScriptFlareType::SinSquaredMode : Mode == TEXT("triangle") ? EGeometryScriptFlareType::TriangleMode
				: EGeometryScriptFlareType::SinMode;
			const double Percent = Step.Float(TEXT("Percent"), 0.0);
			const double Extent = Step.Float(TEXT("Extent"), FMath::Max(Box.GetSize().Z * 0.5, 1.0));
			UDeform::ApplyFlareWarpToMesh(Mesh, Options, WarpFrame(Step, Box, true), Percent, Percent, Extent, Debug);
			UNormals::RecomputeNormals(Mesh, NormalOptions(), false, Debug);
			return true;
		}
		if (K == TEXT("smooth"))
		{
			FGeometryScriptIterativeMeshSmoothingOptions Options;
			Options.NumIterations = Step.Int(TEXT("Iterations"), 10);
			Options.Alpha = Step.Float(TEXT("Alpha"), 0.2);
			UDeform::ApplyIterativeSmoothingToMesh(Mesh, Selection, Options, Debug);
			UNormals::RecomputeNormals(Mesh, NormalOptions(), false, Debug);
			return true;
		}
		if (K == TEXT("tessellate"))
		{
			const int32 Level = Step.Int(TEXT("Level"), 1);
			if (Step.Text(TEXT("Mode"), TEXT("uniform")).Equals(TEXT("pn"), ESearchCase::IgnoreCase))
			{
				UGeometryScriptLibrary_MeshSubdivideFunctions::ApplyPNTessellation(Mesh, FGeometryScriptPNTessellateOptions(), Level, Debug);
			}
			else
			{
				UGeometryScriptLibrary_MeshSubdivideFunctions::ApplyUniformTessellation(Mesh, Level, Debug);
			}
			return true;
		}
		if (K == TEXT("remesh"))
		{
			FGeometryScriptRemeshOptions Options;
			FGeometryScriptUniformRemeshOptions Uniform;
			Uniform.TargetType = EGeometryScriptUniformRemeshTargetType::TriangleCount;
			Uniform.TargetTriangleCount = Step.Int(TEXT("Triangles"), 5000);
			UGeometryScriptLibrary_RemeshingFunctions::ApplyUniformRemesh(Mesh, Options, Uniform, Debug);
			return true;
		}
		if (K == TEXT("simplify"))
		{
			using USimplify = UGeometryScriptLibrary_MeshSimplifyFunctions;
			if (Step.Has(TEXT("Triangles"))) USimplify::ApplySimplifyToTriangleCount(Mesh, Step.Int(TEXT("Triangles"), 1000), FGeometryScriptSimplifyMeshOptions(), Debug);
			else if (Step.Has(TEXT("Tolerance"))) USimplify::ApplySimplifyToTolerance(Mesh, Step.Float(TEXT("Tolerance"), 1.0), FGeometryScriptSimplifyMeshOptions(), Debug);
			else USimplify::ApplySimplifyToPlanar(Mesh, FGeometryScriptPlanarSimplifyOptions(), Debug);
			return true;
		}
		if (K == TEXT("solidify") || K == TEXT("morphology"))
		{
			const FLinearColor Color = FirstColor(*Mesh);
			if (K == TEXT("solidify"))
			{
				FGeometryScriptSolidifyOptions Options;
				Options.GridParameters.SizeMethod = EGeometryScriptGridSizingMethod::GridResolution;
				Options.GridParameters.GridResolution = Step.Int(TEXT("Resolution"), 64);
				Options.WindingThreshold = Step.Float(TEXT("Winding"), 0.5);
				Options.bThickenShells = Step.Float(TEXT("Thicken"), 0.0) > 0.0;
				Options.ShellThickness = Step.Float(TEXT("Thicken"), 0.0);
				UGeometryScriptLibrary_MeshVoxelFunctions::ApplyMeshSolidify(Mesh, Options, Debug);
			}
			else
			{
				FGeometryScriptMorphologyOptions Options;
				Options.SDFGridParameters.SizeMethod = EGeometryScriptGridSizingMethod::GridResolution;
				Options.SDFGridParameters.GridResolution = Step.Int(TEXT("Resolution"), 64);
				const FString Op = Step.Text(TEXT("Op"), TEXT("dilate")).ToLower();
				Options.Operation = Op == TEXT("contract") ? EGeometryScriptMorphologicalOpType::Contract : Op == TEXT("close") ? EGeometryScriptMorphologicalOpType::Close
					: Op == TEXT("open") ? EGeometryScriptMorphologicalOpType::Open : EGeometryScriptMorphologicalOpType::Dilate;
				Options.Distance = Step.Float(TEXT("Distance"), 1.0);
				UGeometryScriptLibrary_MeshVoxelFunctions::ApplyMeshMorphology(Mesh, Options, Debug);
			}
			if (Mesh->GetTriangleCount() == 0)
			{
				OutError = TEXT("the voxel grid found no inside; the mesh may be open (give Thicken) or too thin for the Resolution.");
				return false;
			}
			Refresh(*Mesh, Color, Debug);
			return true;
		}
		if (K == TEXT("transform"))
		{
			UXform::TransformMesh(Mesh, Step.Transform(), true, Debug);
			return true;
		}
		if (K == TEXT("pivot"))
		{
			const FString At = Step.Text(TEXT("At"), TEXT("base"));
			FVector Point;
			if (!ParseVector(At, Point))
			{
				Point = At.Equals(TEXT("center"), ESearchCase::IgnoreCase) ? Box.GetCenter() : FVector(Box.GetCenter().X, Box.GetCenter().Y, Box.Min.Z);
			}
			UXform::TranslateMesh(Mesh, -Point, Debug);
			return true;
		}
		if (K == TEXT("normals"))
		{
			const FString Mode = Step.Text(TEXT("Mode"), TEXT("smooth")).ToLower();
			if (Mode == TEXT("hard"))
			{
				UNormals::SetPerFaceNormals(Mesh, Debug);
			}
			else if (Mode == TEXT("split"))
			{
				FGeometryScriptSplitNormalsOptions Split;
				Split.bSplitByOpeningAngle = true;
				Split.OpeningAngleDeg = Step.Float(TEXT("Angle"), 30.0);
				UNormals::ComputeSplitNormals(Mesh, Split, NormalOptions(), Debug);
			}
			else
			{
				UNormals::SetPerVertexNormals(Mesh, Debug);
			}
			return true;
		}
		if (K == TEXT("repair"))
		{
			using URepair = UGeometryScriptLibrary_MeshRepairFunctions;
			FGeometryScriptWeldEdgesOptions Weld;
			Weld.Tolerance = Step.Float(TEXT("Tolerance"), 0.01);
			URepair::WeldMeshEdges(Mesh, Weld, Debug);
			if (Step.Bool(TEXT("Fill"), true))
			{
				int32 Filled = 0;
				int32 Failed = 0;
				URepair::FillAllMeshHoles(Mesh, FGeometryScriptFillHolesOptions(), Filled, Failed, Debug);
			}
			if (Step.Int(TEXT("MinTriangles"), 0) > 0)
			{
				FGeometryScriptRemoveSmallComponentOptions Small;
				Small.MinTriangleCount = Step.Int(TEXT("MinTriangles"), 0);
				Small.MinVolume = 0.f;
				Small.MinArea = 0.f;
				URepair::RemoveSmallComponents(Mesh, Small, Debug);
			}
			URepair::RepairMeshDegenerateGeometry(Mesh, FGeometryScriptDegenerateTriangleOptions(), Debug);
			return true;
		}
		if (K == TEXT("uv"))
		{
			using UUV = UGeometryScriptLibrary_MeshUVFunctions;
			if (UQuery::GetNumUVSets(Mesh) < 1) UUV::SetNumUVSets(Mesh, 1, Debug);
			const FString Mode = Step.Text(TEXT("Mode"), TEXT("box")).ToLower();
			const double Tile = Step.Float(TEXT("Scale"), 100.0);
			if (Mode == TEXT("auto"))
			{
				UUV::AutoGenerateXAtlasMeshUVs(Mesh, 0, FGeometryScriptXAtlasOptions(), Debug);
			}
			else if (Mode == TEXT("planar"))
			{
				UUV::SetMeshUVsFromPlanarProjection(Mesh, 0, FTransform(FRotator::ZeroRotator, Box.GetCenter(), FVector(Tile)), Selection, Debug);
			}
			else if (Mode == TEXT("cylinder"))
			{
				UUV::SetMeshUVsFromCylinderProjection(Mesh, 0, FTransform(FRotator::ZeroRotator, Box.GetCenter(), FVector(Tile)), Selection, 45.f, Debug);
			}
			else
			{
				UUV::SetMeshUVsFromBoxProjection(Mesh, 0, FTransform(FRotator::ZeroRotator, Box.GetCenter(), FVector(Tile)), Selection, 2, Debug);
			}
			return true;
		}
		if (K == TEXT("material"))
		{
			UGeometryScriptLibrary_MeshMaterialFunctions::EnableMaterialIDs(Mesh, Debug);
			UGeometryScriptLibrary_MeshMaterialFunctions::SetMaterialIDForMeshSelection(Mesh, Selection, Step.Int(TEXT("Id"), 0), false, Debug);
			return true;
		}
		if (K == TEXT("color"))
		{
			FLinearColor Color = Step.Color(TEXT("Color"), DefaultColor());
			// Alpha carries the glow: the vertex-colour material is emissive where alpha is below 1.
			Color.A = 1.f - Step.Float(TEXT("Glow"), 0.0);
			UColor::SetMeshSelectionVertexColor(Mesh, Selection, Color, AllChannels(), /*bCreateColorSeam=*/true, Debug);
			return true;
		}
		OutError = FString::Printf(TEXT("%s is not built here."), *K);
		return false;
	}

	bool Build(const FPlan& Plan, FBuilt& Built, FString& OutError)
	{
		const double Start = FPlatformTime::Seconds();
		const TStrongObjectPtr<UGeometryScriptDebug> Debug(NewObject<UGeometryScriptDebug>(GetTransientPackage()));
		for (const FStep& Step : Plan.Steps)
		{
			// Copies and splits multiply what is there, and an extrude or bevel before them can leave more than Check
			// guessed: these are checked against the meshes as built before they run, not only after.
			const FString& K = Step.Kind;
			if (K == TEXT("append") || K == TEXT("copy") || K == TEXT("array.linear") || K == TEXT("array.radial") || K == TEXT("scatter_on") || K == TEXT("tessellate"))
			{
				TMap<FString, int64> Actual;
				for (const FString* Name : {&Step.Mesh, &Step.Source})
				{
					if (const UDynamicMesh* Each = Name->IsEmpty() ? nullptr : Built.Find(*Name)) Actual.Add(KeyOf(*Name), Each->GetTriangleCount());
				}
				const int64 Predicted = Predict(Step, Actual);
				if (Predicted > C::MaxTrianglesPerMesh)
				{
					OutError = FString::Printf(TEXT("%s would make about %lld triangles in %s from the meshes as built, over the %d one mesh may hold; lower Count or Level, or simplify first. Nothing was changed."),
						*Step.Describe(), Predicted, *Step.Mesh, C::MaxTrianglesPerMesh);
					return false;
				}
			}
			Debug->Messages.Reset();
			FString Error;
			const bool bRan = RunStep(Step, Built, Debug.Get(), Error);
			for (const FGeometryScriptDebugMessage& Message : Debug->Messages)
			{
				if (Message.MessageType == EGeometryScriptDebugMessageType::ErrorMessage && Error.IsEmpty()) Error = Message.Message.ToString();
			}
			if (!bRan || !Error.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s failed: %s Nothing was changed."), *Step.Describe(), *Error);
				return false;
			}
			if (const UDynamicMesh* Mesh = Built.Find(Step.Mesh))
			{
				const int32 Made = Mesh->GetTriangleCount();
				if (Made > C::MaxTrianglesPerMesh)
				{
					OutError = FString::Printf(TEXT("%s made %d triangles in %s, over the %d one mesh may hold. Nothing was changed."), *Step.Describe(), Made, *Step.Mesh,
						C::MaxTrianglesPerMesh);
					return false;
				}
				if (Step.Kind == TEXT("write_asset"))
				{
					const bool bNanite = Step.Bool(TEXT("Nanite"), false);
					const int32 Cap = bNanite ? C::MaxTrianglesPerNaniteAsset : C::MaxTrianglesPerAsset;
					if (Made == 0 || Made > Cap)
					{
						OutError = Made == 0 ? FString::Printf(TEXT("%s: mesh %s is empty, so there is nothing to write. Nothing was changed."), *Step.Describe(), *Step.Mesh)
							: FString::Printf(TEXT("%s: mesh %s has %d triangles, over the %d an asset may have%s. Nothing was changed."), *Step.Describe(), *Step.Mesh, Made, Cap,
								bNanite ? TEXT("") : TEXT(" without Nanite; simplify it or give Nanite=true"));
						return false;
					}
				}
			}
			int64 Total = 0;
			for (const FString& Name : Built.Names)
			{
				if (const UDynamicMesh* Each = Built.Find(Name)) Total += Each->GetTriangleCount();
			}
			if (Total > C::MaxTrianglesPerPlan)
			{
				OutError = FString::Printf(TEXT("%s left %lld triangles across the plan's meshes, over %d. Nothing was changed."), *Step.Describe(), Total, C::MaxTrianglesPerPlan);
				return false;
			}
			const double Elapsed = (FPlatformTime::Seconds() - Start) * 1000.0;
			if (Elapsed > C::MaxBuildMs)
			{
				OutError = FString::Printf(TEXT("the plan took %.0f ms by %s, over the %d ms a plan may take; make it smaller or split it. Nothing was changed."), Elapsed,
					*Step.Describe(), C::MaxBuildMs);
				return false;
			}
		}
		Built.BuildMs = FMath::RoundToInt((FPlatformTime::Seconds() - Start) * 1000.0);
		return true;
	}

	void Describe(const FBuilt& Built, TArray<FHyperAIModelMeshStats>& OutStats)
	{
		for (const FString& Name : Built.Names)
		{
			UDynamicMesh* Mesh = Built.Find(Name);
			if (!Mesh) continue;
			FHyperAIModelMeshStats& Stats = OutStats.AddDefaulted_GetRef();
			Stats.Name = Name;
			Stats.Triangles = Mesh->GetTriangleCount();
			Stats.Vertices = UQuery::GetVertexCount(Mesh);
			const FBox Box = UQuery::GetMeshBoundingBox(Mesh);
			if (Box.IsValid)
			{
				Stats.Size = Vec(Box.GetSize());
				Stats.Min = Vec(Box.Min);
				Stats.Max = Vec(Box.Max);
			}
			Stats.OpenEdges = UQuery::GetNumOpenBorderEdges(Mesh);
			Stats.bClosed = Stats.Triangles > 0 && Stats.OpenEdges == 0;
			Stats.Components = UQuery::GetNumConnectedComponents(Mesh);
			Stats.UVSets = UQuery::GetNumUVSets(Mesh);
			bool bHasIds = false;
			const int32 MaxId = UGeometryScriptLibrary_MeshMaterialFunctions::GetMaxMaterialID(Mesh, bHasIds);
			Stats.MaterialIds = bHasIds ? MaxId + 1 : 1;
		}
	}

	// --- Writing ---------------------------------------------------------------------------------------------------

	/** write_asset's settings, read from its step. */
	struct FWriteSettings
	{
		FString Path;
		bool bNanite = false;
		bool bReplace = false;
		bool bLightmapUV = false;
		FString Collision;
		int32 Hulls = 1;
		TMap<int32, FString> Materials;
	};

	FWriteSettings SettingsOf(const FStep& Step)
	{
		FWriteSettings Out;
		Out.Path = PackageOf(Step.Asset);
		Out.bNanite = Step.Bool(TEXT("Nanite"), false);
		Out.bReplace = Step.Bool(TEXT("Replace"), false) && PackageExists(Out.Path);
		Out.bLightmapUV = Step.Bool(TEXT("LightmapUV"), false);
		ParseCollision(Step.Text(TEXT("Collision"), TEXT("convex")), Out.Collision, Out.Hulls);
		FString Ignored;
		ParseMaterials(Step.Text(TEXT("Materials"), TEXT("")), Out.Materials, Ignored);
		return Out;
	}

	FGeometryScriptCollisionFromMeshOptions CollisionOptions(const FWriteSettings& Settings)
	{
		FGeometryScriptCollisionFromMeshOptions Options;
		Options.bEmitTransaction = false;
		if (Settings.Collision == TEXT("box")) Options.Method = EGeometryScriptCollisionGenerationMethod::AlignedBoxes;
		else if (Settings.Collision == TEXT("auto")) Options.Method = EGeometryScriptCollisionGenerationMethod::MinVolumeShapes;
		else
		{
			Options.Method = EGeometryScriptCollisionGenerationMethod::ConvexHulls;
			Options.MaxConvexHullsPerMesh = Settings.Hulls;
		}
		return Options;
	}

	bool HasSimpleCollision(const FWriteSettings& Settings)
	{
		return Settings.Collision != TEXT("none") && Settings.Collision != TEXT("complex");
	}

	/** Material slots the mesh needs: its highest material id + 1, and any slot Materials names. */
	int32 SlotsOf(UDynamicMesh& Mesh, const FWriteSettings& Settings)
	{
		bool bHasIds = false;
		const int32 MaxId = UGeometryScriptLibrary_MeshMaterialFunctions::GetMaxMaterialID(&Mesh, bHasIds);
		int32 Slots = bHasIds ? MaxId + 1 : 1;
		for (const TPair<int32, FString>& Pair : Settings.Materials) Slots = FMath::Max(Slots, Pair.Key + 1);
		return FMath::Max(Slots, 1);
	}

	void PreviewWrites(const FPlan& Plan, FBuilt& Built, TArray<FHyperAIModelAssetResult>& OutAssets)
	{
		for (const FStep& Step : Plan.Steps)
		{
			if (Step.Kind != TEXT("write_asset")) continue;
			UDynamicMesh* Mesh = Built.Kept(Step.Index);
			if (!Mesh) continue;
			const FWriteSettings Settings = SettingsOf(Step);
			FHyperAIModelAssetResult& Result = OutAssets.AddDefaulted_GetRef();
			Result.Path = Settings.Path;
			Result.Mesh = Step.Mesh;
			Result.Triangles = Mesh->GetTriangleCount();
			Result.Slots = SlotsOf(*Mesh, Settings);
			Result.bNanite = Settings.bNanite;
			Result.Collision = Settings.Collision == TEXT("hulls") ? FString::Printf(TEXT("hulls:%d"), Settings.Hulls) : Settings.Collision;
			Result.bReplaced = Settings.bReplace;
			if (HasSimpleCollision(Settings))
			{
				const FGeometryScriptSimpleCollision Shapes = UGeometryScriptLibrary_CollisionFunctions::GenerateCollisionFromMesh(Mesh, CollisionOptions(Settings), nullptr);
				Result.CollisionShapes = UGeometryScriptLibrary_CollisionFunctions::GetSimpleCollisionShapeCount(Shapes);
			}
		}
	}

	UMaterialInterface* VertexColorMaterial(const bool bCreate, FString& OutError)
	{
		const FString Path = C::VertexColorMaterial;
		if (UMaterialInterface* Found = LoadQuiet<UMaterialInterface>(Path)) return Found;
		if (!bCreate) return nullptr;
		if (PackageExists(Path))
		{
			OutError = FString::Printf(TEXT("%s exists but is not a material; move it, and the vertex-colour material is made again."), C::VertexColorMaterial);
			return nullptr;
		}
		// Made outside any plan's undo step: undo cannot delete an asset, and would only blank its graph.
		TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
		UPackage* Package = CreatePackage(*Path);
		UMaterial* Material = NewObject<UMaterial>(Package, *FPackageName::GetShortName(Path), RF_Public | RF_Standalone);
		// Base colour is the vertex colour; where alpha is below 1 the colour also glows, four times as bright at alpha 0.
		UMaterialExpressionVertexColor* Vertex = NewObject<UMaterialExpressionVertexColor>(Material);
		UMaterialExpressionOneMinus* Glow = NewObject<UMaterialExpressionOneMinus>(Material);
		Glow->Input.Connect(4, Vertex);
		UMaterialExpressionMultiply* Lit = NewObject<UMaterialExpressionMultiply>(Material);
		Lit->A.Connect(0, Vertex);
		Lit->B.Expression = Glow;
		UMaterialExpressionMultiply* Bright = NewObject<UMaterialExpressionMultiply>(Material);
		Bright->A.Expression = Lit;
		Bright->ConstB = 4.f;
		UMaterialExpressionConstant* Rough = NewObject<UMaterialExpressionConstant>(Material);
		Rough->R = 0.8f;
		int32 Row = 0;
		for (UMaterialExpression* Expression : TArray<UMaterialExpression*>{Vertex, Glow, Lit, Bright, Rough})
		{
			Expression->MaterialExpressionEditorX = -500 + (Row % 2) * 220;
			Expression->MaterialExpressionEditorY = Row * 120;
			++Row;
			Material->GetExpressionCollection().AddExpression(Expression);
		}
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Vertex);
		Material->GetEditorOnlyData()->EmissiveColor.Expression = Bright;
		Material->GetEditorOnlyData()->Roughness.Expression = Rough;
		Material->PostEditChange();
		FAssetRegistryModule::AssetCreated(Material);
		Material->MarkPackageDirty();
		if (!FHyperAIStudioSourceControl::SaveAsset(*Material, OutError)) return nullptr;
		Material->SetFlags(RF_Transactional);
		return Material;
	}

	/** Slot materials: those Materials names, the vertex-colour material for the rest. */
	bool MaterialsFor(const FWriteSettings& Settings, const int32 Slots, TArray<UMaterialInterface*>& OutMaterials, FString& OutError)
	{
		OutMaterials.Reset();
		for (int32 Slot = 0; Slot < Slots; ++Slot)
		{
			UMaterialInterface* Material = nullptr;
			if (const FString* Named = Settings.Materials.Find(Slot))
			{
				Material = LoadQuiet<UMaterialInterface>(*Named);
				if (!Material)
				{
					OutError = FString::Printf(TEXT("%s is not a material."), **Named);
					return false;
				}
			}
			else
			{
				Material = VertexColorMaterial(/*bCreate=*/true, OutError);
				if (!Material) return false;
			}
			OutMaterials.Add(Material);
		}
		return true;
	}

	void SetCollision(UDynamicMesh& Mesh, UStaticMesh& Asset, const FWriteSettings& Settings, UGeometryScriptDebug* Debug)
	{
		if (!HasSimpleCollision(Settings)) return;
		FGeometryScriptSetStaticMeshCollisionOptions Apply;
		Apply.bMarkAsCustomized = true;
		UGeometryScriptLibrary_CollisionFunctions::SetStaticMeshCollisionFromMesh(&Mesh, &Asset, CollisionOptions(Settings), Apply, Debug);
	}

	int32 CollisionShapesOf(const UStaticMesh& Asset)
	{
		const UBodySetup* Body = Asset.GetBodySetup();
		return Body ? Body->AggGeom.GetElementCount() : 0;
	}

	bool Write(const FPlan& Plan, FBuilt& Built, TArray<FHyperAIModelAssetResult>& OutAssets, TArray<UStaticMesh*>& OutWritten,
		TArray<UStaticMesh*>& OutCreated, TArray<FString>& OutNotes, FString& OutError)
	{
		const TStrongObjectPtr<UGeometryScriptDebug> Debug(NewObject<UGeometryScriptDebug>(GetTransientPackage()));
		for (const FStep& Step : Plan.Steps)
		{
			if (Step.Kind != TEXT("write_asset")) continue;
			UDynamicMesh* Mesh = Built.Kept(Step.Index);
			if (!Mesh)
			{
				OutError = FString::Printf(TEXT("%s: mesh %s was not built for it."), *Step.Describe(), *Step.Mesh);
				return false;
			}
			const FWriteSettings Settings = SettingsOf(Step);
			const int32 Slots = SlotsOf(*Mesh, Settings);
			TArray<UMaterialInterface*> Materials;
			if (!MaterialsFor(Settings, Slots, Materials, OutError))
			{
				OutError = FString::Printf(TEXT("%s: %s"), *Step.Describe(), *OutError);
				return false;
			}
			Debug->Messages.Reset();
			UStaticMesh* Asset = nullptr;
			if (Settings.bReplace)
			{
				// Checked again: a mesh that is not at the path now is not the one the plan was approved against.
				Asset = ReplaceTarget(Settings.Path, OutError);
				if (!Asset)
				{
					OutError = FString::Printf(TEXT("%s: %s"), *Step.Describe(), *OutError);
					return false;
				}
				if (!FHyperAIStudioSourceControl::MakeEditable({Asset->GetPackage()->GetName()}, OutError))
				{
					OutError = FString::Printf(TEXT("%s: %s"), *Step.Describe(), *OutError);
					return false;
				}
				// Inside the plan's undo step: the asset, its mesh description and its body are recorded before they change.
				Asset->Modify();
				// The plan's mesh is all the asset keeps: LODs of its own, or a Nanite hi-res source, would still show the old
				// model. Their mesh descriptions are recorded first, so undo brings them back.
				TArray<FString> Removed;
				if (Asset->GetNumSourceModels() > 1)
				{
					Removed.Add(FString::Printf(TEXT("its %d other LOD(s)"), Asset->GetNumSourceModels() - 1));
					for (int32 Lod = 1; Lod < Asset->GetNumSourceModels(); ++Lod) Asset->ModifyMeshDescription(Lod);
					Asset->SetNumSourceModels(1);
				}
				if (Asset->IsHiResMeshDescriptionValid())
				{
					Removed.Add(TEXT("its hi-res source"));
					Asset->ModifyHiResMeshDescription();
					Asset->ClearHiResMeshDescription();
					Asset->CommitHiResMeshDescription();
					Asset->GetHiResSourceModel().SourceImportFilename.Empty();
				}
				if (!Removed.IsEmpty()) OutNotes.Add(FString::Printf(TEXT("%s: %s removed, so it shows only the plan's mesh."), *Settings.Path, *FString::Join(Removed, TEXT(" and "))));
				FGeometryScriptCopyMeshToAssetOptions Options;
				Options.bEmitTransaction = false;
				Options.bEnableRecomputeTangents = true;
				Options.bReplaceMaterials = true;
				Options.NewMaterials.Append(Materials);
				Options.bApplyNaniteSettings = true;
				Options.NewNaniteSettings = Asset->GetNaniteSettings();
				Options.NewNaniteSettings.bEnabled = Settings.bNanite;
				Options.GenerateLightmapUVs = Settings.bLightmapUV ? EGeometryScriptGenerateLightmapUVOptions::GenerateLightmapUVs : EGeometryScriptGenerateLightmapUVOptions::DoNotGenerateLightmapUVs;
				EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
				FGeometryScriptMeshWriteLOD Lod;
				Lod.LODIndex = 0;
				UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshToStaticMesh(Mesh, Asset, Options, Lod, Outcome, /*bUseSectionMaterials=*/false, Debug.Get());
				if (Outcome != EGeometryScriptOutcomePins::Success)
				{
					OutError = FString::Printf(TEXT("%s: %s could not take the new mesh%s"), *Step.Describe(), *Settings.Path,
						Debug->Messages.IsEmpty() ? TEXT(".") : *(TEXT(": ") + Debug->Messages[0].Message.ToString()));
					return false;
				}
				if (Settings.Collision != TEXT("none"))
				{
					if (!Asset->GetBodySetup()) Asset->CreateBodySetup();
					if (UBodySetup* Body = Asset->GetBodySetup())
					{
						Body->Modify();
						Body->CollisionTraceFlag = Settings.Collision == TEXT("complex") ? ECollisionTraceFlag::CTF_UseComplexAsSimple : ECollisionTraceFlag::CTF_UseDefault;
						if (!HasSimpleCollision(Settings)) Body->RemoveSimpleCollision();
					}
				}
				else if (UBodySetup* Body = Asset->GetBodySetup())
				{
					Body->Modify();
					Body->RemoveSimpleCollision();
				}
				SetCollision(*Mesh, *Asset, Settings, Debug.Get());
				Asset->GetPackage()->GetMetaData().SetValue(Asset, C::PlanHashKey, *Plan.Hash);
				Asset->PostEditChange();
				Asset->MarkPackageDirty();
			}
			else
			{
				FDynamicMesh3 Copy;
				Mesh->ProcessMesh([&Copy](const FDynamicMesh3& Read) { Copy = Read; });
				UE::AssetUtils::FStaticMeshAssetOptions Options;
				Options.NewAssetPath = Settings.Path;
				Options.NumSourceModels = 1;
				Options.NumMaterialSlots = Slots;
				Options.AssetMaterials = Materials;
				Options.bEnableRecomputeNormals = false;
				Options.bEnableRecomputeTangents = true;
				Options.bGenerateNaniteEnabledMesh = Settings.bNanite;
				Options.NaniteSettings.bEnabled = Settings.bNanite;
				Options.bGenerateLightmapUVs = Settings.bLightmapUV;
				Options.bCreatePhysicsBody = Settings.Collision != TEXT("none");
				Options.CollisionType = Settings.Collision == TEXT("complex") ? ECollisionTraceFlag::CTF_UseComplexAsSimple : ECollisionTraceFlag::CTF_UseDefault;
				Options.SourceMeshes.DynamicMeshes.Add(&Copy);
				Options.bDeferPostEditChange = true;
				UE::AssetUtils::FStaticMeshResults Results;
				if (UE::AssetUtils::CreateStaticMeshAsset(Options, Results) != UE::AssetUtils::ECreateStaticMeshResult::Ok || !Results.StaticMesh)
				{
					OutError = FString::Printf(TEXT("%s: %s could not be made."), *Step.Describe(), *Settings.Path);
					return false;
				}
				Asset = Results.StaticMesh;
				OutCreated.Add(Asset);
				{
					// A new asset takes its settings outside the undo step, as Epic's asset creation does: undo cannot delete it, and would blank them.
					TGuardValue<ITransaction*> NoUndo(GUndo, nullptr);
					SetCollision(*Mesh, *Asset, Settings, Debug.Get());
					Asset->GetPackage()->GetMetaData().SetValue(Asset, C::PlanHashKey, *Plan.Hash);
					Asset->PostEditChange();
				}
				FAssetRegistryModule::AssetCreated(Asset);
				Asset->MarkPackageDirty();
				// Last, so later edits are undoable without undo reaching back into its making.
				Asset->SetFlags(RF_Transactional);
			}
			for (const FGeometryScriptDebugMessage& Message : Debug->Messages)
			{
				OutNotes.Add(FString::Printf(TEXT("%s: %s"), *Settings.Path, *Message.Message.ToString()));
			}
			OutWritten.Add(Asset);
			FHyperAIModelAssetResult& Result = OutAssets.AddDefaulted_GetRef();
			Result.Path = Asset->GetPathName();
			Result.Mesh = Step.Mesh;
			Result.Triangles = Mesh->GetTriangleCount();
			Result.Slots = Asset->GetStaticMaterials().Num();
			Result.bNanite = Settings.bNanite;
			Result.Collision = Settings.Collision == TEXT("hulls") ? FString::Printf(TEXT("hulls:%d"), Settings.Hulls) : Settings.Collision;
			Result.CollisionShapes = CollisionShapesOf(*Asset);
			Result.bReplaced = Settings.bReplace;
		}
		return true;
	}

	void Discard(TArray<UStaticMesh*>& Created)
	{
		TArray<UObject*> Objects;
		for (UStaticMesh* Mesh : Created)
		{
			if (Mesh) Objects.Add(Mesh);
		}
		if (!Objects.IsEmpty()) ObjectTools::ForceDeleteObjects(Objects, /*ShowConfirmation=*/false);
		Created.Reset();
	}

	bool Verify(const TArray<UStaticMesh*>& Written, const TArray<FHyperAIModelAssetResult>& Expected, FString& OutError)
	{
		TArray<UStaticMesh*> Meshes = Written;
		FStaticMeshCompilingManager::Get().FinishCompilation(Meshes);
		for (int32 Index = 0; Index < Written.Num() && Index < Expected.Num(); ++Index)
		{
			UStaticMesh* Mesh = Written[Index];
			const FStaticMeshRenderData* Render = Mesh ? Mesh->GetRenderData() : nullptr;
			if (!Render || Render->LODResources.IsEmpty() || Mesh->GetNumTriangles(0) == 0)
			{
				OutError = FString::Printf(TEXT("%s built no render data."), *Expected[Index].Path);
				return false;
			}
			// A replaced asset keeps none of the old model: no LODs of its own, no hi-res source for Nanite to build from.
			if (Mesh->GetNumSourceModels() != 1 || Mesh->IsHiResMeshDescriptionValid())
			{
				OutError = FString::Printf(TEXT("%s holds %d source LOD(s)%s, not the plan's mesh alone."), *Expected[Index].Path, Mesh->GetNumSourceModels(),
					Mesh->IsHiResMeshDescriptionValid() ? TEXT(" and a hi-res source") : TEXT(""));
				return false;
			}
			// The source mesh, not LOD0: with Nanite, LOD0 is the reduced fallback.
			const FMeshDescription* Source = Mesh->GetMeshDescription(0);
			const int32 Triangles = Source ? Source->Triangles().Num() : 0;
			const int32 Planned = Expected[Index].Triangles;
			if (FMath::Abs(Triangles - Planned) > FMath::Max(1, Planned / 100))
			{
				OutError = FString::Printf(TEXT("%s holds %d triangles, not the %d the plan built."), *Expected[Index].Path, Triangles, Planned);
				return false;
			}
			const FString& Collision = Expected[Index].Collision;
			if (Collision != TEXT("none") && Collision != TEXT("complex") && CollisionShapesOf(*Mesh) == 0)
			{
				OutError = FString::Printf(TEXT("%s got no %s collision."), *Expected[Index].Path, *Collision);
				return false;
			}
		}
		return true;
	}

	// --- Props checked -------------------------------------------------------------------------------------------------

	void CheckAsset(UStaticMesh& Mesh, const bool bProduction, TArray<FHyperAIModelIssue>& OutIssues, FString& OutLine)
	{
		const FString Path = Mesh.GetPathName();
		auto Issue = [&OutIssues, &Path](const TCHAR* Severity, const TCHAR* Code, const FString& Message)
		{
			FHyperAIModelIssue& Out = OutIssues.AddDefaulted_GetRef();
			Out.Asset = Path;
			Out.Severity = Severity;
			Out.Code = Code;
			Out.Message = Message;
		};
		TArray<UStaticMesh*> Compile = {&Mesh};
		FStaticMeshCompilingManager::Get().FinishCompilation(Compile);
		const FStaticMeshRenderData* Render = Mesh.GetRenderData();
		if (!Render || Render->LODResources.IsEmpty())
		{
			Issue(TEXT("error"), TEXT("no_render_data"), TEXT("It has no render data: it was never built, or its build failed."));
			OutLine = Path + TEXT(": no render data");
			return;
		}
		const bool bNanite = Mesh.IsNaniteEnabled();
		// With Nanite, LOD0 is the fallback; the source mesh holds the full count.
		const FMeshDescription* Source = Mesh.GetMeshDescription(0);
		const int32 Triangles = Source ? Source->Triangles().Num() : Mesh.GetNumTriangles(0);
		if (Triangles == 0) Issue(TEXT("error"), TEXT("empty_mesh"), TEXT("It has no triangles."));
		// Production wants heavy meshes on Nanite: past 50000 triangles without it is an error there, a warning for a placeholder.
		const int32 HeavyWithoutNanite = 50000;
		const int32 Cap = bNanite ? C::MaxTrianglesPerNaniteAsset * 4 : bProduction ? HeavyWithoutNanite : C::MaxTrianglesPerAsset;
		if (Triangles > Cap)
		{
			Issue(TEXT("error"), TEXT("too_many_triangles"), FString::Printf(TEXT("%d triangles, over the %d a %s%s prop may have."), Triangles, Cap,
				bProduction ? TEXT("production ") : TEXT(""), bNanite ? TEXT("Nanite") : TEXT("non-Nanite")));
		}
		else if (!bNanite && Triangles > HeavyWithoutNanite)
		{
			Issue(TEXT("warning"), TEXT("nanite_off"), FString::Printf(TEXT("%d triangles without Nanite; turn Nanite on, or simplify."), Triangles));
		}
		const FBox Bounds = Mesh.GetBoundingBox();
		const FVector Size = Bounds.GetSize();
		if (Size.GetMax() < 1.0) Issue(TEXT("error"), TEXT("too_small"), FString::Printf(TEXT("It is %s cm across; is it in meters instead of cm?"), *Vec(Size)));
		if (Size.GetMax() > 10000.0) Issue(TEXT("error"), TEXT("too_large"), FString::Printf(TEXT("It is %s cm across, over 100 m; 100 units are a meter."), *Vec(Size)));
		if (Size.Z > 0.0 && FMath::Abs(Bounds.Min.Z) > FMath::Max(2.0, Size.Z * 0.05))
		{
			Issue(TEXT("warning"), TEXT("pivot_not_at_base"), FString::Printf(TEXT("Its bottom is %s cm from the pivot; props place best with the pivot at the base (pivot At=base)."), *Num(Bounds.Min.Z)));
		}
		const UBodySetup* Body = Mesh.GetBodySetup();
		const int32 Shapes = Body ? Body->AggGeom.GetElementCount() : 0;
		const bool bComplex = Body && Body->CollisionTraceFlag == ECollisionTraceFlag::CTF_UseComplexAsSimple;
		if (Shapes == 0 && !bComplex)
		{
			Issue(bProduction ? TEXT("error") : TEXT("warning"), TEXT("no_collision"), TEXT("It has no simple collision; write it with Collision=convex, hulls:N or box."));
		}
		for (int32 Slot = 0; Slot < Mesh.GetStaticMaterials().Num(); ++Slot)
		{
			if (!Mesh.GetStaticMaterials()[Slot].MaterialInterface)
			{
				Issue(TEXT("error"), TEXT("empty_material_slot"), FString::Printf(TEXT("Material slot %d is empty."), Slot));
			}
		}
		if (Mesh.GetNumUVChannels(0) == 0) Issue(TEXT("warning"), TEXT("no_uvs"), TEXT("It has no UVs; textures will not map (uv Mode=box)."));
		// Unmeasured topology says so, rather than reading as 0 open edges: watertight.
		FString Topology = TEXT("open edges and pieces not measured (no source mesh)");
		if (Source && Triangles > C::MaxTrianglesPerMesh)
		{
			// Copying a production mesh of millions of triangles would hold the game thread for seconds, and its memory.
			Topology = FString::Printf(TEXT("open edges and pieces not measured (over %d triangles)"), C::MaxTrianglesPerMesh);
		}
		else if (Source)
		{
			const TStrongObjectPtr<UDynamicMesh> Copy(NewObject<UDynamicMesh>(GetTransientPackage()));
			FGeometryScriptCopyMeshFromAssetOptions Options;
			Options.bApplyBuildSettings = false;
			FGeometryScriptMeshReadLOD Lod;
			Lod.LODType = EGeometryScriptLODType::SourceModel;
			Lod.LODIndex = 0;
			EGeometryScriptOutcomePins Outcome = EGeometryScriptOutcomePins::Failure;
			UGeometryScriptLibrary_StaticMeshFunctions::CopyMeshFromStaticMeshV2(&Mesh, Copy.Get(), Options, Lod, Outcome, true, nullptr);
			Topology = TEXT("open edges and pieces not measured (its source mesh could not be read)");
			if (Outcome == EGeometryScriptOutcomePins::Success)
			{
				const int32 OpenEdges = UQuery::GetNumOpenBorderEdges(Copy.Get());
				if (OpenEdges > 0) Issue(TEXT("warning"), TEXT("open_edges"), FString::Printf(TEXT("%d open edges: it is not watertight, so light leaks and booleans fail (repair, or solidify)."), OpenEdges));
				Topology = FString::Printf(TEXT("%d open edge(s), %d piece(s)"), OpenEdges, UQuery::GetNumConnectedComponents(Copy.Get()));
			}
			// Freed now, not at the next garbage collection: validating 64 assets holds one copy at a time.
			Copy->Reset();
		}
		OutLine = FString::Printf(TEXT("%s: %d triangles, %s cm, %d slot(s), %d collision shape(s)%s, %s, Nanite %s"), *Path, Triangles, *Vec(Size),
			Mesh.GetStaticMaterials().Num(), Shapes, bComplex ? TEXT(" (complex as simple)") : TEXT(""), *Topology, bNanite ? TEXT("on") : TEXT("off"));
	}

	namespace
	{
		template <typename ValueType>
		void HashValue(const ValueType& Value, FXxHash64Builder& Builder)
		{
			Builder.Update(&Value, sizeof(Value));
		}

		/** A name by its lower-case text, which is the same in every session (an FName's index is not). */
		void HashValue(const FName Value, FXxHash64Builder& Builder)
		{
			const FString Text = Value.ToString().ToLower();
			Builder.Update(*Text, Text.Len() * sizeof(TCHAR));
		}

		/**
		 * Every layer of one type the engine does not reserve, by name: the polygroups and polygroup layers, label layers
		 * and weight maps Modeling Mode's tools write (Generate PolyGroups changes nothing else). Transient ones are left
		 * out, as they are not saved.
		 */
		template <typename ValueType, typename ElementIDType, typename ElementsType>
		void HashLayers(const TAttributesSet<ElementIDType>& Attributes, const ElementsType& Elements, FXxHash64Builder& Builder)
		{
			TArray<FName> Names;
			Attributes.template ForEachByType<ValueType>([&Names](const FName Name, const auto Values)
			{
				if (!FStaticMeshAttributes::IsReservedAttributeName(Name) && !EnumHasAnyFlags(Values.GetFlags(), EMeshAttributeFlags::Transient)) Names.Add(Name);
			});
			Names.Sort(FNameLexicalLess());
			for (const FName Name : Names)
			{
				HashValue(Name, Builder);
				const auto Values = Attributes.template GetAttributesRef<ValueType>(Name);
				for (const ElementIDType Element : Elements) HashValue(Values.Get(Element), Builder);
			}
		}

		/**
		 * One source mesh into the hash: its counts; each triangle's corners (position, every UV set, normal, colour) and
		 * material slot; hard edges; then the layers.
		 */
		void HashSource(const FMeshDescription& Source, FXxHash64Builder& Builder)
		{
			const int32 Counts[2] = {Source.Triangles().Num(), Source.Vertices().Num()};
			Builder.Update(Counts, sizeof(Counts));
			const FStaticMeshConstAttributes Attributes(Source);
			const auto Positions = Source.GetVertexPositions();
			const TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
			const int32 UVSets = UVs.IsValid() ? UVs.GetNumChannels() : 0;
			const TVertexInstanceAttributesConstRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
			const TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
			for (const FTriangleID Triangle : Source.Triangles().GetElementIDs())
			{
				for (const FVertexInstanceID Corner : Source.GetTriangleVertexInstances(Triangle))
				{
					HashValue(Positions[Source.GetVertexInstanceVertex(Corner)], Builder);
					for (int32 Set = 0; Set < UVSets; ++Set) HashValue(UVs.Get(Corner, Set), Builder);
					if (Normals.IsValid()) HashValue(Normals.Get(Corner), Builder);
					if (Colors.IsValid()) HashValue(Colors.Get(Corner), Builder);
				}
				HashValue(Source.GetTrianglePolygonGroup(Triangle).GetValue(), Builder);
			}
			const TEdgeAttributesConstRef<bool> Hard = Attributes.GetEdgeHardnesses();
			if (Hard.IsValid())
			{
				for (const FEdgeID Edge : Source.Edges().GetElementIDs()) HashValue(Hard.Get(Edge), Builder);
			}
			// The main polygroups are a polygon layer (PolyTriGroups), the others triangle layers.
			HashLayers<int32>(Source.PolygonAttributes(), Source.Polygons().GetElementIDs(), Builder);
			HashLayers<int32>(Source.TriangleAttributes(), Source.Triangles().GetElementIDs(), Builder);
			HashLayers<FName>(Source.TriangleAttributes(), Source.Triangles().GetElementIDs(), Builder);
			HashLayers<float>(Source.VertexAttributes(), Source.Vertices().GetElementIDs(), Builder);
		}

		/** A source model's triangles without unpacking it: its unpacked mesh's when it has one, else the count saved at its last commit; -1 when neither is known. */
		int64 KnownTriangles(const FStaticMeshSourceModel& Model)
		{
			if (!Model.IsSourceModelInitialized()) return -1;
			if (const FMeshDescription* Unpacked = Model.GetCachedMeshDescription()) return Unpacked->Triangles().Num();
			return Model.CacheMeshDescriptionTrianglesCount != MAX_uint32 ? int64(Model.CacheMeshDescriptionTrianglesCount) : -1;
		}
	}

	FGeometryPrint Fingerprint(UStaticMesh& Mesh)
	{
		FGeometryPrint Print;
		if (!Mesh.IsMeshDescriptionValid(0))
		{
			Print.Why = TEXT("it has no source mesh");
			return Print;
		}
		// Modeling Mode edits the hi-res source of a mesh that has one, so it counts too.
		const bool bHiRes = Mesh.IsHiResMeshDescriptionValid();
		FStaticMeshSourceModel& HiResModel = Mesh.GetHiResSourceModel();
		// Counted before anything is unpacked: unpacking a source of millions of triangles holds the editor for seconds,
		// and the unpacked mesh stays in memory after.
		const FStaticMeshSourceModel& Lod0Model = Mesh.GetSourceModel(0);
		const int64 Lod0Known = KnownTriangles(Lod0Model);
		const int64 HiResKnown = bHiRes ? KnownTriangles(HiResModel) : 0;
		auto OverCap = [&Print](const int64 Triangles)
		{
			// About 64 bytes a triangle: millions would hold the editor while they hash.
			if (Triangles <= C::MaxFingerprintTriangles) return false;
			Print.Why = FString::Printf(TEXT("over %d triangles"), C::MaxFingerprintTriangles);
			return true;
		};
		if (Lod0Known >= 0 && HiResKnown >= 0 && OverCap(Lod0Known + HiResKnown))
		{
			Print.Triangles = int32(FMath::Min<int64>(Lod0Known, MAX_int32));
			Print.HiResTriangles = int32(FMath::Min<int64>(HiResKnown, MAX_int32));
			Print.Vertices = Lod0Model.CacheMeshDescriptionVerticesCount != MAX_uint32 ? int32(FMath::Min<uint32>(Lod0Model.CacheMeshDescriptionVerticesCount, MAX_int32)) : 0;
			return Print;
		}
		const FMeshDescription* Source = Mesh.GetMeshDescription(0);
		if (!Source)
		{
			Print.Why = TEXT("its source mesh could not be read");
			return Print;
		}
		Print.Triangles = Source->Triangles().Num();
		Print.Vertices = Source->Vertices().Num();
		if (HiResKnown >= 0)
		{
			Print.HiResTriangles = int32(FMath::Min<int64>(HiResKnown, MAX_int32));
			if (OverCap(int64(Print.Triangles) + HiResKnown)) return Print;
		}
		else if (OverCap(Print.Triangles))
		{
			return Print;
		}
		FXxHash64Builder Builder;
		HashSource(*Source, Builder);
		if (bHiRes)
		{
			// Unpacked only to hash it, and let go after; one that Modeling Mode or anything else had unpacked already stays.
			// Its count was unknown only when it was not unpacked, so it meets the cap here.
			const bool bWasUnpacked = HiResModel.GetCachedMeshDescription() != nullptr;
			const FMeshDescription* HiRes = Mesh.GetHiResMeshDescription();
			Print.HiResTriangles = HiRes ? HiRes->Triangles().Num() : 0;
			const bool bOver = OverCap(int64(Print.Triangles) + Print.HiResTriangles);
			if (HiRes && !bOver) HashSource(*HiRes, Builder);
			if (HiRes && !bWasUnpacked) Mesh.ClearHiResMeshDescription();
			if (bOver) return Print;
			if (!HiRes)
			{
				Print.Why = TEXT("its hi-res source mesh could not be read");
				return Print;
			}
		}
		Print.Hash = FString::Printf(TEXT("geo-%016llx"), Builder.Finalize().Hash);
		return Print;
	}

	TArray<FString> DescribeKinds()
	{
		TArray<FString> Lines;
		const TPair<uint32, const TCHAR*> FieldNames[] = {{Field::Source, TEXT("Source")}, {Field::Location, TEXT("Location")}, {Field::Rotation, TEXT("Rotation")},
			{Field::Scale, TEXT("Scale")}, {Field::Size, TEXT("Size")}, {Field::Points, TEXT("Points")}, {Field::Path, TEXT("Path")}, {Field::Select, TEXT("Select")},
			{Field::Asset, TEXT("Asset")}};
		for (const FKindSpec& Spec : Specs())
		{
			TArray<FString> Fields = {TEXT("Mesh")};
			for (const TPair<uint32, const TCHAR*>& Pair : FieldNames)
			{
				if (Spec.Allowed & Pair.Key) Fields.Add(FString(Pair.Value) + ((Spec.Required & Pair.Key) ? TEXT("*") : TEXT("")));
			}
			TArray<FString> Params;
			for (const FParamSpec& Param : Spec.Params)
			{
				FString Text = Param.Name;
				if (Param.Type == EParam::Choice) Text += FString::Printf(TEXT(" %s"), Param.Choices);
				else if (Param.Type == EParam::Int || Param.Type == EParam::Float) Text += FString::Printf(TEXT(" %s-%s"), *Num(Param.Min), *Num(Param.Max));
				else if (Param.Type == EParam::Text && FCString::Strlen(Param.Choices) > 0) Text += FString::Printf(TEXT(" (%s)"), Param.Choices);
				Text += FCString::Strlen(Param.Default) > 0 ? FString::Printf(TEXT(" =%s"), Param.Default) : FString(TEXT(" required"));
				Params.Add(Text);
			}
			if (Spec.Allowed & Field::Select) Params.Append({TEXT("Invert"), TEXT("Grow 0-64")});
			Lines.Add(FString::Printf(TEXT("%s: %s. Fields %s.%s"), Spec.Kind, Spec.What, *FString::Join(Fields, TEXT(", ")),
				Params.IsEmpty() ? TEXT("") : *(TEXT(" Params: ") + FString::Join(Params, TEXT("; ")) + TEXT("."))));
		}
		return Lines;
	}

	TArray<FString> DescribeLimits()
	{
		return {
			FString::Printf(TEXT("%d ops a request, %d after recipes expand; %d working meshes at once; %d assets a plan."), C::MaxOperations, C::MaxExpandedOperations, C::MaxMeshes, C::MaxAssets),
			FString::Printf(TEXT("%d triangles a working mesh, %d a plan's meshes together; an asset %d without Nanite, %d with."), C::MaxTrianglesPerMesh, C::MaxTrianglesPerPlan,
				C::MaxTrianglesPerAsset, C::MaxTrianglesPerNaniteAsset),
			FString::Printf(TEXT("Profiles %d points, paths %d; arrays %d copies, scatters %d; tessellate level %d; voxel grids %d cells; remesh %d triangles."), C::MaxProfilePoints,
				C::MaxPathPoints, C::MaxArrayCount, C::MaxScatterCount, C::MaxTessellateLevel, C::MaxGridResolution, C::MaxRemeshTriangles),
			FString::Printf(TEXT("A plan's meshes build within %d ms; triangles are predicted before anything runs and counted after every op."), C::MaxBuildMs),
			TEXT("Units: cm (100 = 1 m), Z up; shapes and recipes sit on their base unless Origin=center."),
		};
	}
}
