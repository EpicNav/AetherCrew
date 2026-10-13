// Games by Hyper 2026.

#include "HyperAIStudioMaterialsRecipes.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioMaterialsGraphGate.h"
#include "HyperAIStudioNames.h"
#include "HyperAIStudioResultPreview.h"
#include "HyperAIStudioSourceControl.h"
#include "MaterialEditingLibrary.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionLandscapeLayerBlend.h"
#include "Materials/MaterialExpressionLandscapeLayerCoords.h"
#include "Materials/MaterialExpressionSingleLayerWaterMaterialOutput.h"
#include "Materials/MaterialExpressionSubstrate.h"
#include "Materials/MaterialFunction.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "RenderUtils.h"
#include "RHI.h"
#include "ScopedTransaction.h"
#include "Engine/Texture.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/StrongObjectPtr.h"

#define LOCTEXT_NAMESPACE "HyperAIStudioMaterialsRecipes"

namespace HyperAIStudio::Materials::Recipes
{
	namespace
	{
		const TCHAR* const DefaultDiffuse = TEXT("/Engine/EngineMaterials/DefaultDiffuse.DefaultDiffuse");
		const TCHAR* const DefaultNormal = TEXT("/Engine/EngineMaterials/DefaultNormal.DefaultNormal");
		const TCHAR* const DefaultMasks = TEXT("/Engine/EngineMaterials/DefaultDiffuse_TC_Masks.DefaultDiffuse_TC_Masks");
		constexpr int32 MaxCodeChars = 16 * 1024;
		constexpr int32 MaxLayers = 8;
		constexpr int32 MaxInputs = 8;

		struct FSetting
		{
			const TCHAR* Key;
			const TCHAR* Default;
			const TCHAR* Help;
		};

		struct FRecipeDef
		{
			const TCHAR* Id;
			const TCHAR* Summary;
			TArray<FSetting> Settings;
		};

		const TArray<FRecipeDef>& Defs()
		{
			static const TArray<FRecipeDef> Table = {
				{TEXT("layered_pbr"), TEXT("Opaque PBR surface: base colour (tinted), normal with a detail normal blended over it, ORM (R occlusion, G roughness, B metallic), and tiling."),
					{{TEXT("base_color_texture"), DefaultDiffuse, TEXT("BaseColor texture")},
					{TEXT("normal_texture"), DefaultNormal, TEXT("Normal texture")},
					{TEXT("orm_texture"), DefaultMasks, TEXT("ORM texture: R occlusion, G roughness, B metallic")},
					{TEXT("tint"), TEXT("1,1,1"), TEXT("Tint over the base colour")},
					{TEXT("tiling"), TEXT("1"), TEXT("UV tiling of the main maps")},
					{TEXT("detail_normal_texture"), DefaultNormal, TEXT("DetailNormal texture")},
					{TEXT("detail_tiling"), TEXT("8"), TEXT("UV tiling of the detail normal")},
					{TEXT("detail_strength"), TEXT("0.5"), TEXT("How much of the detail normal shows, 0-1")},
					{TEXT("roughness_scale"), TEXT("1"), TEXT("Multiplies the ORM roughness")}}},
				{TEXT("water"), TEXT("Single Layer Water: shallow colour, depth absorption and scattering, two panning normal samples blended, roughness and refraction."),
					{{TEXT("shallow_color"), TEXT("0.02,0.06,0.07"), TEXT("ShallowColor, the surface's base colour")},
					{TEXT("scattering_color"), TEXT("0.04,0.12,0.14"), TEXT("ScatteringColor, light the water scatters back")},
					{TEXT("absorption"), TEXT("0.45,0.07,0.04"), TEXT("DepthAbsorption per metre, R G B: the depth colour (red absorbed first reads blue-green)")},
					{TEXT("normal_texture"), DefaultNormal, TEXT("WaterNormal texture, sampled twice")},
					{TEXT("normal_tiling"), TEXT("4"), TEXT("NormalTiling of the ripples")},
					{TEXT("pan_speed"), TEXT("0.03"), TEXT("How fast the ripples pan, UV per second (built into the panners)")},
					{TEXT("normal_strength"), TEXT("0.4"), TEXT("NormalStrength, 0 flat to 1")},
					{TEXT("roughness"), TEXT("0.05"), TEXT("Roughness")},
					{TEXT("ior"), TEXT("1.33"), TEXT("RefractionIOR")}}},
				{TEXT("decal"), TEXT("Deferred decal: tinted colour with its alpha times Opacity, a normal blended in by NormalStrength, and roughness."),
					{{TEXT("base_color_texture"), DefaultDiffuse, TEXT("DecalColor texture (alpha is the mask)")},
					{TEXT("normal_texture"), DefaultNormal, TEXT("DecalNormal texture")},
					{TEXT("tint"), TEXT("1,1,1"), TEXT("Tint")},
					{TEXT("opacity"), TEXT("1"), TEXT("Opacity, 0-1")},
					{TEXT("normal_strength"), TEXT("1"), TEXT("NormalStrength, 0 keeps the surface's normal")},
					{TEXT("roughness"), TEXT("0.6"), TEXT("Roughness")},
					{TEXT("tiling"), TEXT("1"), TEXT("UV tiling")}}},
				{TEXT("landscape_layers"), TEXT("Landscape material: N paint layers blended by height (a Landscape Layer Blend), each a tinted albedo with its own height; names match hyper_landscape_apply_plan's paint layers."),
					{{TEXT("layers"), TEXT("Grass,Rock,Snow"), TEXT("Paint layer names, 1-8, as landscape.add_paint_layer names them")},
					{TEXT("texture"), DefaultDiffuse, TEXT("Albedo every layer starts from (each layer gets its own <Layer>Albedo parameter)")},
					{TEXT("mapping_scale"), TEXT("8"), TEXT("Landscape coordinates' mapping scale")},
					{TEXT("roughness"), TEXT("0.85"), TEXT("Roughness")}}},
				{TEXT("custom_hlsl"), TEXT("A Custom HLSL node with inputs fed by parameters; the code is compiled in the dry run, so errors come back before anything is made."),
					{{TEXT("inputs"), TEXT(""), TEXT("Inputs as Name=default separated by ';': one number makes a scalar parameter, three or four a vector (Tint=1,0.5,0.2; Amount=0.5)")},
					{TEXT("output_type"), TEXT("float3"), TEXT("float1, float2, float3 or float4")},
					{TEXT("output"), TEXT("base_color"), TEXT("Where the result goes: base_color, emissive, roughness, metallic, opacity or normal")},
					{TEXT("description"), TEXT("Custom"), TEXT("The node's title")}}},
				{TEXT("substrate_slab"), TEXT("Substrate slab BSDF into the Front Material: diffuse albedo, F0, roughness and a normal map. Only when Substrate is on in the project."),
					{{TEXT("albedo"), TEXT("0.5,0.5,0.5"), TEXT("DiffuseAlbedo")},
					{TEXT("f0"), TEXT("0.04"), TEXT("F0, the reflectance at normal incidence")},
					{TEXT("roughness"), TEXT("0.5"), TEXT("Roughness")},
					{TEXT("normal_texture"), DefaultNormal, TEXT("Normal texture")},
					{TEXT("tiling"), TEXT("1"), TEXT("UV tiling")}}},
				{TEXT("material_function"), TEXT("A material function: typed inputs, a Custom HLSL body, one output. With caller_path, also a material calling it with a parameter per input; the dry run compiles it through a caller either way."),
					{{TEXT("inputs"), TEXT("Color:vector3"), TEXT("Inputs as Name:type separated by ',': scalar, vector2, vector3 or vector4")},
					{TEXT("output_type"), TEXT("float3"), TEXT("float1, float2, float3 or float4")},
					{TEXT("output_name"), TEXT("Result"), TEXT("The function's output")},
					{TEXT("caller_output"), TEXT("emissive"), TEXT("The caller's property the output feeds: emissive or base_color")},
					{TEXT("description"), TEXT(""), TEXT("The function's description in the palette")}}},
			};
			return Table;
		}

		const FRecipeDef* FindDef(const FString& Id)
		{
			return Defs().FindByPredicate([&Id](const FRecipeDef& Def) { return Id == Def.Id; });
		}

		bool IsIdentifier(const FString& Value)
		{
			if (Value.IsEmpty() || Value.Len() > 64 || !(FChar::IsAlpha(Value[0]) || Value[0] == TEXT('_'))) return false;
			for (const TCHAR Char : Value)
			{
				if (!FChar::IsAlnum(Char) && Char != TEXT('_')) return false;
			}
			return true;
		}

		bool ParseNumber(const FString& Value, double& Out)
		{
			const FString Trimmed = Value.TrimStartAndEnd();
			return !Trimmed.IsEmpty() && LexTryParseString(Out, *Trimmed) && FMath::IsFinite(Out);
		}

		/** 1, 3 or 4 numbers separated by commas. */
		bool ParseNumbers(const FString& Value, TArray<double>& Out)
		{
			TArray<FString> Parts;
			Value.ParseIntoArray(Parts, TEXT(","));
			Out.Reset();
			for (const FString& Part : Parts)
			{
				double Number = 0.0;
				if (!ParseNumber(Part, Number)) return false;
				Out.Add(Number);
			}
			return Out.Num() == 1 || Out.Num() == 3 || Out.Num() == 4;
		}

		FString Number(const double Value)
		{
			return FString::SanitizeFloat(Value);
		}

		/** The effective settings: the recipe's defaults, overridden by the request's; unknown keys refused. */
		bool Resolve(const FRecipeDef& Def, const TMap<FString, FString>& In, TMap<FString, FString>& Out, FString& OutError)
		{
			TArray<FString> Keys;
			for (const FSetting& Setting : Def.Settings)
			{
				Out.Add(Setting.Key, Setting.Default);
				Keys.Add(Setting.Key);
			}
			for (const TPair<FString, FString>& Pair : In)
			{
				if (!Out.Contains(Pair.Key))
				{
					OutError = FString::Printf(TEXT("%s has no setting '%s'. Settings: %s."), Def.Id, *Pair.Key, *FString::Join(Keys, TEXT(", ")));
					return false;
				}
				Out[Pair.Key] = Pair.Value.TrimStartAndEnd();
			}
			return true;
		}

		/** Writes graphs in a few lines per idea. Node ids are the recipe's own; nothing here touches an object. */
		struct FBuilder
		{
			FGraph& G;
			const TMap<FString, FString>& S;
			FString Error;

			FNode& Node(const FString& Id, const TCHAR* Kind, std::initializer_list<TPair<FString, FString>> Properties = {})
			{
				FNode& Out = G.Nodes.AddDefaulted_GetRef();
				Out.Id = Id;
				Out.Kind = Kind;
				Out.Properties = Properties;
				return Out;
			}

			void Wire(const FString& From, const FString& FromOutput, const FString& To, const FString& ToInput)
			{
				G.Wires.Add({From, FromOutput, To, ToInput});
			}

			void Out(const TCHAR* Property, const FString& From, const FString& FromOutput = FString())
			{
				G.Outputs.Add({Property, From, FromOutput});
			}

			bool Num(const TCHAR* Key, double& OutValue)
			{
				if (ParseNumber(S.FindRef(Key), OutValue)) return true;
				Error = FString::Printf(TEXT("Setting %s must be a number, not '%s'."), Key, *S.FindRef(Key).Left(64));
				return false;
			}

			bool Color(const TCHAR* Key, FString& OutValue)
			{
				TArray<double> Parts;
				if (ParseNumbers(S.FindRef(Key), Parts) && Parts.Num() >= 3)
				{
					OutValue = FString::Printf(TEXT("%s,%s,%s,%s"), *Number(Parts[0]), *Number(Parts[1]), *Number(Parts[2]),
						*Number(Parts.Num() > 3 ? Parts[3] : 1.0));
					return true;
				}
				Error = FString::Printf(TEXT("Setting %s must be a colour 'r,g,b', not '%s'."), Key, *S.FindRef(Key).Left(64));
				return false;
			}

			bool Path(const TCHAR* Key, FString& OutValue)
			{
				OutValue = S.FindRef(Key);
				FString Reason;
				if (FHyperAIStudioNames::IsObjectPath(OutValue, Key, &Reason) && FPackageName::IsValidObjectPath(OutValue)) return true;
				Error = Reason.IsEmpty() ? FString::Printf(TEXT("Setting %s must be a texture's object path, such as %s."), Key, DefaultDiffuse) : Reason;
				return false;
			}

			void Scalar(const FString& Id, const FString& Name, const double Default, const TCHAR* Group)
			{
				Node(Id, TEXT("scalar_parameter"), {{TEXT("name"), Name}, {TEXT("default"), Number(Default)}, {TEXT("group"), Group}});
				G.Parameters.Add(Name + TEXT(" (scalar)"));
			}

			void Vector(const FString& Id, const FString& Name, const FString& Default, const TCHAR* Group)
			{
				Node(Id, TEXT("vector_parameter"), {{TEXT("name"), Name}, {TEXT("default"), Default}, {TEXT("group"), Group}});
				G.Parameters.Add(Name + TEXT(" (vector)"));
			}

			void Texture(const FString& Id, const FString& Name, const FString& Texture, const FString& Uv, const TCHAR* Group)
			{
				Node(Id, TEXT("texture_parameter"), {{TEXT("texture"), Texture}, {TEXT("name"), Name}, {TEXT("group"), Group}});
				if (!G.Parameters.Contains(Name + TEXT(" (texture)"))) G.Parameters.Add(Name + TEXT(" (texture)"));
				if (!Uv.IsEmpty()) Wire(Uv, FString(), Id, TEXT("Coordinates"));
			}

			/** UV0 times a Tiling-style scalar parameter. */
			FString TiledUv(const FString& Prefix, const FString& ParameterName, const double Tiling, const TCHAR* Group)
			{
				if (!G.Nodes.ContainsByPredicate([](const FNode& Node) { return Node.Id == TEXT("uv"); }))
				{
					Node(TEXT("uv"), TEXT("texcoord"));
				}
				Scalar(Prefix + TEXT("_tiling"), ParameterName, Tiling, Group);
				Node(Prefix + TEXT("_uv"), TEXT("multiply"));
				Wire(TEXT("uv"), FString(), Prefix + TEXT("_uv"), TEXT("A"));
				Wire(Prefix + TEXT("_tiling"), FString(), Prefix + TEXT("_uv"), TEXT("B"));
				return Prefix + TEXT("_uv");
			}

			/** Whiteout blend of two tangent-space normals: normalize(float3(a.xy + b.xy, a.z * b.z)). */
			FString BlendNormals(const FString& Prefix, const FString& A, const FString& AOutput, const FString& B, const FString& BOutput)
			{
				auto Mask = [this](const FString& Id, const FString& From, const FString& FromOutput, const bool bXY)
				{
					Node(Id, TEXT("component_mask"), {{TEXT("r"), bXY ? TEXT("true") : TEXT("false")}, {TEXT("g"), bXY ? TEXT("true") : TEXT("false")},
						{TEXT("b"), bXY ? TEXT("false") : TEXT("true")}, {TEXT("a"), TEXT("false")}});
					Wire(From, FromOutput, Id, FString());
				};
				Mask(Prefix + TEXT("_a_xy"), A, AOutput, true);
				Mask(Prefix + TEXT("_b_xy"), B, BOutput, true);
				Mask(Prefix + TEXT("_a_z"), A, AOutput, false);
				Mask(Prefix + TEXT("_b_z"), B, BOutput, false);
				Node(Prefix + TEXT("_xy"), TEXT("add"));
				Wire(Prefix + TEXT("_a_xy"), FString(), Prefix + TEXT("_xy"), TEXT("A"));
				Wire(Prefix + TEXT("_b_xy"), FString(), Prefix + TEXT("_xy"), TEXT("B"));
				Node(Prefix + TEXT("_z"), TEXT("multiply"));
				Wire(Prefix + TEXT("_a_z"), FString(), Prefix + TEXT("_z"), TEXT("A"));
				Wire(Prefix + TEXT("_b_z"), FString(), Prefix + TEXT("_z"), TEXT("B"));
				Node(Prefix + TEXT("_join"), TEXT("append"));
				Wire(Prefix + TEXT("_xy"), FString(), Prefix + TEXT("_join"), TEXT("A"));
				Wire(Prefix + TEXT("_z"), FString(), Prefix + TEXT("_join"), TEXT("B"));
				Node(Prefix, TEXT("normalize"));
				Wire(Prefix + TEXT("_join"), FString(), Prefix, FString());
				return Prefix;
			}

			/** lerp(flat, Normal, Strength): a normal faded towards flat by a scalar parameter. */
			FString FadeNormal(const FString& Prefix, const FString& Normal, const FString& NormalOutput, const FString& ParameterName,
				const double Strength, const TCHAR* Group)
			{
				Node(Prefix + TEXT("_flat"), TEXT("constant3"), {{TEXT("color"), TEXT("0,0,1,1")}});
				Scalar(Prefix + TEXT("_strength"), ParameterName, Strength, Group);
				Node(Prefix, TEXT("lerp"));
				Wire(Prefix + TEXT("_flat"), FString(), Prefix, TEXT("A"));
				Wire(Normal, NormalOutput, Prefix, TEXT("B"));
				Wire(Prefix + TEXT("_strength"), FString(), Prefix, TEXT("Alpha"));
				return Prefix;
			}
		};

		bool BuildLayeredPbr(FBuilder& B)
		{
			double Tiling = 1, DetailTiling = 8, DetailStrength = 0.5, RoughnessScale = 1;
			FString Tint, BaseTexture, NormalTexture, OrmTexture, DetailTexture;
			if (!B.Num(TEXT("tiling"), Tiling) || !B.Num(TEXT("detail_tiling"), DetailTiling) || !B.Num(TEXT("detail_strength"), DetailStrength)
				|| !B.Num(TEXT("roughness_scale"), RoughnessScale) || !B.Color(TEXT("tint"), Tint) || !B.Path(TEXT("base_color_texture"), BaseTexture)
				|| !B.Path(TEXT("normal_texture"), NormalTexture) || !B.Path(TEXT("orm_texture"), OrmTexture)
				|| !B.Path(TEXT("detail_normal_texture"), DetailTexture))
			{
				return false;
			}
			const FString Uv = B.TiledUv(TEXT("main"), TEXT("Tiling"), Tiling, TEXT("Surface"));
			B.Texture(TEXT("base"), TEXT("BaseColor"), BaseTexture, Uv, TEXT("Surface"));
			B.Vector(TEXT("tint"), TEXT("Tint"), Tint, TEXT("Surface"));
			B.Node(TEXT("base_tinted"), TEXT("multiply"));
			B.Wire(TEXT("base"), TEXT("RGB"), TEXT("base_tinted"), TEXT("A"));
			B.Wire(TEXT("tint"), FString(), TEXT("base_tinted"), TEXT("B"));
			B.Out(TEXT("base_color"), TEXT("base_tinted"));

			B.Texture(TEXT("normal"), TEXT("Normal"), NormalTexture, Uv, TEXT("Surface"));
			const FString DetailUv = B.TiledUv(TEXT("detail"), TEXT("DetailTiling"), DetailTiling, TEXT("Detail"));
			B.Texture(TEXT("detail"), TEXT("DetailNormal"), DetailTexture, DetailUv, TEXT("Detail"));
			const FString Detail = B.FadeNormal(TEXT("detail_faded"), TEXT("detail"), TEXT("RGB"), TEXT("DetailNormalStrength"), DetailStrength, TEXT("Detail"));
			B.Out(TEXT("normal"), B.BlendNormals(TEXT("normal_blend"), TEXT("normal"), TEXT("RGB"), Detail, FString()));

			B.Texture(TEXT("orm"), TEXT("ORM"), OrmTexture, Uv, TEXT("Surface"));
			B.Out(TEXT("ao"), TEXT("orm"), TEXT("R"));
			B.Scalar(TEXT("roughness_scale"), TEXT("RoughnessScale"), RoughnessScale, TEXT("Surface"));
			B.Node(TEXT("roughness"), TEXT("multiply"));
			B.Wire(TEXT("orm"), TEXT("G"), TEXT("roughness"), TEXT("A"));
			B.Wire(TEXT("roughness_scale"), FString(), TEXT("roughness"), TEXT("B"));
			B.Out(TEXT("roughness"), TEXT("roughness"));
			B.Out(TEXT("metallic"), TEXT("orm"), TEXT("B"));
			return true;
		}

		bool BuildWater(FBuilder& B)
		{
			double NormalTiling = 4, PanSpeed = 0.03, NormalStrength = 0.4, Roughness = 0.05, Ior = 1.33;
			FString Shallow, Scattering, Absorption, NormalTexture;
			if (!B.Num(TEXT("normal_tiling"), NormalTiling) || !B.Num(TEXT("pan_speed"), PanSpeed) || !B.Num(TEXT("normal_strength"), NormalStrength)
				|| !B.Num(TEXT("roughness"), Roughness) || !B.Num(TEXT("ior"), Ior) || !B.Color(TEXT("shallow_color"), Shallow)
				|| !B.Color(TEXT("scattering_color"), Scattering) || !B.Color(TEXT("absorption"), Absorption)
				|| !B.Path(TEXT("normal_texture"), NormalTexture))
			{
				return false;
			}
			B.G.ShadingModel = TEXT("single_layer_water");
			B.Vector(TEXT("shallow"), TEXT("ShallowColor"), Shallow, TEXT("Water"));
			B.Out(TEXT("base_color"), TEXT("shallow"));
			B.Node(TEXT("water_output"), TEXT("slw_output"));
			B.Vector(TEXT("scattering"), TEXT("ScatteringColor"), Scattering, TEXT("Water"));
			B.Wire(TEXT("scattering"), FString(), TEXT("water_output"), TEXT("ScatteringCoefficients"));
			B.Vector(TEXT("absorption"), TEXT("DepthAbsorption"), Absorption, TEXT("Water"));
			B.Wire(TEXT("absorption"), FString(), TEXT("water_output"), TEXT("AbsorptionCoefficients"));
			B.Node(TEXT("phase"), TEXT("constant"), {{TEXT("r"), TEXT("0")}});
			B.Wire(TEXT("phase"), FString(), TEXT("water_output"), TEXT("PhaseG"));
			B.Node(TEXT("behind"), TEXT("constant"), {{TEXT("r"), TEXT("1")}});
			B.Wire(TEXT("behind"), FString(), TEXT("water_output"), TEXT("ColorScaleBehindWater"));

			// Two samples of one normal map panning different ways, at different scales, so the ripples never repeat in step.
			const FString Uv = B.TiledUv(TEXT("ripple"), TEXT("NormalTiling"), NormalTiling, TEXT("Ripples"));
			B.Node(TEXT("pan_a"), TEXT("panner"), {{TEXT("speed_x"), Number(PanSpeed)}, {TEXT("speed_y"), Number(PanSpeed * 0.6)}});
			B.Wire(Uv, FString(), TEXT("pan_a"), TEXT("Coordinate"));
			B.Node(TEXT("uv_b"), TEXT("multiply"), {{TEXT("const_b"), TEXT("1.37")}});
			B.Wire(Uv, FString(), TEXT("uv_b"), TEXT("A"));
			B.Node(TEXT("pan_b"), TEXT("panner"), {{TEXT("speed_x"), Number(-PanSpeed * 0.7)}, {TEXT("speed_y"), Number(PanSpeed * 0.4)}});
			B.Wire(TEXT("uv_b"), FString(), TEXT("pan_b"), TEXT("Coordinate"));
			B.Texture(TEXT("normal_a"), TEXT("WaterNormal"), NormalTexture, TEXT("pan_a"), TEXT("Ripples"));
			B.Texture(TEXT("normal_b"), TEXT("WaterNormal"), NormalTexture, TEXT("pan_b"), TEXT("Ripples"));
			const FString Blended = B.BlendNormals(TEXT("ripples"), TEXT("normal_a"), TEXT("RGB"), TEXT("normal_b"), TEXT("RGB"));
			B.Out(TEXT("normal"), B.FadeNormal(TEXT("ripples_faded"), Blended, FString(), TEXT("NormalStrength"), NormalStrength, TEXT("Ripples")));
			B.Scalar(TEXT("roughness"), TEXT("Roughness"), Roughness, TEXT("Water"));
			B.Out(TEXT("roughness"), TEXT("roughness"));
			B.Scalar(TEXT("ior"), TEXT("RefractionIOR"), Ior, TEXT("Water"));
			B.Out(TEXT("refraction"), TEXT("ior"));
			B.G.Notes.Add(TEXT("Single Layer Water needs the deferred renderer (desktop and consoles); put it on a water body or any flat mesh."));
			return true;
		}

		bool BuildDecal(FBuilder& B)
		{
			double Opacity = 1, NormalStrength = 1, Roughness = 0.6, Tiling = 1;
			FString Tint, BaseTexture, NormalTexture;
			if (!B.Num(TEXT("opacity"), Opacity) || !B.Num(TEXT("normal_strength"), NormalStrength) || !B.Num(TEXT("roughness"), Roughness)
				|| !B.Num(TEXT("tiling"), Tiling) || !B.Color(TEXT("tint"), Tint) || !B.Path(TEXT("base_color_texture"), BaseTexture)
				|| !B.Path(TEXT("normal_texture"), NormalTexture))
			{
				return false;
			}
			B.G.Domain = TEXT("deferred_decal");
			B.G.BlendMode = TEXT("translucent");
			const FString Uv = B.TiledUv(TEXT("main"), TEXT("Tiling"), Tiling, TEXT("Decal"));
			B.Texture(TEXT("base"), TEXT("DecalColor"), BaseTexture, Uv, TEXT("Decal"));
			B.Vector(TEXT("tint"), TEXT("Tint"), Tint, TEXT("Decal"));
			B.Node(TEXT("base_tinted"), TEXT("multiply"));
			B.Wire(TEXT("base"), TEXT("RGB"), TEXT("base_tinted"), TEXT("A"));
			B.Wire(TEXT("tint"), FString(), TEXT("base_tinted"), TEXT("B"));
			B.Out(TEXT("base_color"), TEXT("base_tinted"));
			B.Scalar(TEXT("opacity_scale"), TEXT("Opacity"), Opacity, TEXT("Decal"));
			B.Node(TEXT("opacity"), TEXT("multiply"));
			B.Wire(TEXT("base"), TEXT("A"), TEXT("opacity"), TEXT("A"));
			B.Wire(TEXT("opacity_scale"), FString(), TEXT("opacity"), TEXT("B"));
			B.Out(TEXT("opacity"), TEXT("opacity"));
			B.Texture(TEXT("normal"), TEXT("DecalNormal"), NormalTexture, Uv, TEXT("Decal"));
			B.Out(TEXT("normal"), B.FadeNormal(TEXT("normal_faded"), TEXT("normal"), TEXT("RGB"), TEXT("NormalStrength"), NormalStrength, TEXT("Decal")));
			B.Scalar(TEXT("roughness"), TEXT("Roughness"), Roughness, TEXT("Decal"));
			B.Out(TEXT("roughness"), TEXT("roughness"));
			B.G.Notes.Add(TEXT("Place it with a Decal actor (or a DecalComponent) whose box crosses the surface."));
			return true;
		}

		FString LayerTint(const FString& Layer, const int32 Index)
		{
			const FString Lower = Layer.ToLower();
			if (Lower.Contains(TEXT("grass"))) return TEXT("0.22,0.38,0.1,1");
			if (Lower.Contains(TEXT("rock")) || Lower.Contains(TEXT("stone")) || Lower.Contains(TEXT("cliff"))) return TEXT("0.4,0.38,0.36,1");
			if (Lower.Contains(TEXT("snow")) || Lower.Contains(TEXT("ice"))) return TEXT("0.9,0.92,0.95,1");
			if (Lower.Contains(TEXT("dirt")) || Lower.Contains(TEXT("mud")) || Lower.Contains(TEXT("soil"))) return TEXT("0.33,0.24,0.15,1");
			if (Lower.Contains(TEXT("sand"))) return TEXT("0.76,0.69,0.5,1");
			static const TCHAR* const Palette[] = {TEXT("0.5,0.45,0.35,1"), TEXT("0.3,0.35,0.25,1"), TEXT("0.55,0.5,0.45,1"), TEXT("0.25,0.2,0.18,1")};
			return Palette[Index % UE_ARRAY_COUNT(Palette)];
		}

		bool BuildLandscapeLayers(FBuilder& B)
		{
			double MappingScale = 8, Roughness = 0.85;
			FString Texture;
			if (!B.Num(TEXT("mapping_scale"), MappingScale) || !B.Num(TEXT("roughness"), Roughness) || !B.Path(TEXT("texture"), Texture))
			{
				return false;
			}
			TArray<FString> Layers;
			B.S.FindRef(TEXT("layers")).ParseIntoArray(Layers, TEXT(","));
			TSet<FString> Seen;
			for (FString& Layer : Layers)
			{
				Layer.TrimStartAndEndInline();
				bool bDuplicate = false;
				Seen.Add(Layer.ToLower(), &bDuplicate);
				if (!IsIdentifier(Layer) || bDuplicate)
				{
					B.Error = FString::Printf(TEXT("Layer names are unique letters, digits and underscores, such as Grass,Rock,Snow; '%s' is not."), *Layer.Left(64));
					return false;
				}
			}
			if (Layers.IsEmpty() || Layers.Num() > MaxLayers)
			{
				B.Error = FString::Printf(TEXT("layers names 1 to %d paint layers, such as Grass,Rock,Snow."), MaxLayers);
				return false;
			}
			B.Node(TEXT("coords"), TEXT("landscape_coords"), {{TEXT("mapping_scale"), Number(MappingScale)}});
			B.Node(TEXT("blend"), TEXT("landscape_layer_blend"), {{TEXT("layers"), FString::Join(Layers, TEXT(","))}});
			for (int32 Index = 0; Index < Layers.Num(); ++Index)
			{
				const FString& Layer = Layers[Index];
				const FString Id = Layer.ToLower();
				B.Texture(Id + TEXT("_albedo"), Layer + TEXT("Albedo"), Texture, TEXT("coords"), TEXT("Layers"));
				B.Vector(Id + TEXT("_tint"), Layer + TEXT("Tint"), LayerTint(Layer, Index), TEXT("Layers"));
				B.Node(Id + TEXT("_color"), TEXT("multiply"));
				B.Wire(Id + TEXT("_albedo"), TEXT("RGB"), Id + TEXT("_color"), TEXT("A"));
				B.Wire(Id + TEXT("_tint"), FString(), Id + TEXT("_color"), TEXT("B"));
				B.Wire(Id + TEXT("_color"), FString(), TEXT("blend"), TEXT("Layer ") + Layer);
				// The albedo's red channel as height, scaled per layer, so a higher layer wins where both are painted.
				B.Scalar(Id + TEXT("_height_scale"), Layer + TEXT("Height"), 1.0, TEXT("Layers"));
				B.Node(Id + TEXT("_height"), TEXT("multiply"));
				B.Wire(Id + TEXT("_albedo"), TEXT("R"), Id + TEXT("_height"), TEXT("A"));
				B.Wire(Id + TEXT("_height_scale"), FString(), Id + TEXT("_height"), TEXT("B"));
				B.Wire(Id + TEXT("_height"), FString(), TEXT("blend"), TEXT("Height ") + Layer);
			}
			B.Out(TEXT("base_color"), TEXT("blend"));
			B.Scalar(TEXT("roughness"), TEXT("Roughness"), Roughness, TEXT("Surface"));
			B.Out(TEXT("roughness"), TEXT("roughness"));
			B.G.Notes.Add(FString::Printf(TEXT("Paint layers %s: set this material on the landscape and add paint layers with these exact names "
				"(hyper_landscape_apply_plan landscape.set_material, then landscape.add_paint_layer)."), *FString::Join(Layers, TEXT(", "))));
			return true;
		}

		/** "Tint=1,0.5,0.2; Amount=0.5" into (name, default) pairs; one number is a scalar, three or four a vector. */
		bool ParseCodeInputs(const FString& Text, TArray<TPair<FString, TArray<double>>>& Out, FString& OutError)
		{
			TArray<FString> Entries;
			Text.ParseIntoArray(Entries, TEXT(";"));
			for (FString& Entry : Entries)
			{
				FString Name;
				FString Value;
				TArray<double> Numbers;
				if (!Entry.TrimStartAndEnd().Split(TEXT("="), &Name, &Value) || !IsIdentifier(Name.TrimStartAndEnd()) || !ParseNumbers(Value, Numbers))
				{
					OutError = FString::Printf(TEXT("inputs is Name=default entries separated by ';', such as Tint=1,0.5,0.2; Amount=0.5. '%s' is not one."), *Entry.Left(64));
					return false;
				}
				Out.Add({Name.TrimStartAndEnd(), Numbers});
			}
			if (Out.Num() > MaxInputs)
			{
				OutError = FString::Printf(TEXT("At most %d inputs."), MaxInputs);
				return false;
			}
			return true;
		}

		bool IsOutputType(const FString& Value)
		{
			return Value == TEXT("float1") || Value == TEXT("float2") || Value == TEXT("float3") || Value == TEXT("float4");
		}

		bool CheckCode(const FString& Code, FString& OutError)
		{
			if (Code.TrimStartAndEnd().IsEmpty() || Code.Len() > MaxCodeChars)
			{
				OutError = FString::Printf(TEXT("Code is the HLSL body, 1 to %d characters, that returns the output (return Tint * Amount;)."), MaxCodeChars);
				return false;
			}
			return true;
		}

		bool BuildCustomHlsl(FBuilder& B, const FString& Code)
		{
			TArray<TPair<FString, TArray<double>>> Inputs;
			const FString OutputType = B.S.FindRef(TEXT("output_type")).ToLower();
			const FString Output = B.S.FindRef(TEXT("output")).ToLower();
			static const TArray<FString> Outputs = {TEXT("base_color"), TEXT("emissive"), TEXT("roughness"), TEXT("metallic"), TEXT("opacity"), TEXT("normal")};
			if (!CheckCode(Code, B.Error) || !ParseCodeInputs(B.S.FindRef(TEXT("inputs")), Inputs, B.Error))
			{
				return false;
			}
			if (!IsOutputType(OutputType) || !Outputs.Contains(Output))
			{
				B.Error = TEXT("output_type is float1, float2, float3 or float4; output is base_color, emissive, roughness, metallic, opacity or normal.");
				return false;
			}
			TArray<FString> Names;
			for (const TPair<FString, TArray<double>>& Input : Inputs) Names.Add(Input.Key);
			B.Node(TEXT("code"), TEXT("custom_hlsl"), {{TEXT("code"), Code}, {TEXT("output_type"), OutputType}, {TEXT("inputs"), FString::Join(Names, TEXT(","))},
				{TEXT("description"), B.S.FindRef(TEXT("description")).Left(128)}});
			for (const TPair<FString, TArray<double>>& Input : Inputs)
			{
				const FString Id = TEXT("in_") + Input.Key.ToLower();
				if (Input.Value.Num() == 1)
				{
					B.Scalar(Id, Input.Key, Input.Value[0], TEXT("Code"));
					B.Wire(Id, FString(), TEXT("code"), Input.Key);
				}
				else
				{
					const TArray<double>& V = Input.Value;
					B.Vector(Id, Input.Key, FString::Printf(TEXT("%s,%s,%s,%s"), *Number(V[0]), *Number(V[1]), *Number(V[2]), *Number(V.Num() > 3 ? V[3] : 1.0)), TEXT("Code"));
					B.Wire(Id, V.Num() > 3 ? TEXT("RGBA") : FString(), TEXT("code"), Input.Key);
				}
			}
			B.Out(*Output, TEXT("code"));
			if (Output == TEXT("opacity"))
			{
				B.G.BlendMode = TEXT("translucent");
			}
			return true;
		}

		bool BuildSubstrate(FBuilder& B)
		{
			double F0 = 0.04, Roughness = 0.5, Tiling = 1;
			FString Albedo, NormalTexture;
			if (!B.Num(TEXT("f0"), F0) || !B.Num(TEXT("roughness"), Roughness) || !B.Num(TEXT("tiling"), Tiling) || !B.Color(TEXT("albedo"), Albedo)
				|| !B.Path(TEXT("normal_texture"), NormalTexture))
			{
				return false;
			}
			B.G.ShadingModel = TEXT("substrate");
			B.Node(TEXT("slab"), TEXT("substrate_slab"));
			B.Vector(TEXT("albedo"), TEXT("DiffuseAlbedo"), Albedo, TEXT("Slab"));
			B.Wire(TEXT("albedo"), FString(), TEXT("slab"), TEXT("Diffuse Albedo"));
			B.Scalar(TEXT("f0"), TEXT("F0"), F0, TEXT("Slab"));
			B.Wire(TEXT("f0"), FString(), TEXT("slab"), TEXT("F0"));
			B.Scalar(TEXT("roughness"), TEXT("Roughness"), Roughness, TEXT("Slab"));
			B.Wire(TEXT("roughness"), FString(), TEXT("slab"), TEXT("Roughness"));
			const FString Uv = B.TiledUv(TEXT("main"), TEXT("Tiling"), Tiling, TEXT("Slab"));
			B.Texture(TEXT("normal"), TEXT("Normal"), NormalTexture, Uv, TEXT("Slab"));
			B.Wire(TEXT("normal"), TEXT("RGB"), TEXT("slab"), TEXT("Normal"));
			B.Out(TEXT("front_material"), TEXT("slab"));
			return true;
		}

		bool BuildFunction(FBuilder& B, const FString& Code)
		{
			const FString OutputType = B.S.FindRef(TEXT("output_type")).ToLower();
			const FString OutputName = B.S.FindRef(TEXT("output_name"));
			if (!CheckCode(Code, B.Error))
			{
				return false;
			}
			if (!IsOutputType(OutputType) || !IsIdentifier(OutputName))
			{
				B.Error = TEXT("output_type is float1, float2, float3 or float4; output_name is letters, digits and underscores.");
				return false;
			}
			TArray<FString> Entries;
			B.S.FindRef(TEXT("inputs")).ParseIntoArray(Entries, TEXT(","));
			static const TArray<FString> Types = {TEXT("scalar"), TEXT("vector2"), TEXT("vector3"), TEXT("vector4")};
			TArray<FString> Names;
			for (const FString& Entry : Entries)
			{
				FString Name;
				FString Type;
				if (!Entry.TrimStartAndEnd().Split(TEXT(":"), &Name, &Type) || !IsIdentifier(Name.TrimStartAndEnd())
					|| !Types.Contains(Type.TrimStartAndEnd().ToLower()) || Names.Contains(Name.TrimStartAndEnd()))
				{
					B.Error = FString::Printf(TEXT("inputs is unique Name:type entries separated by ',', type scalar, vector2, vector3 or vector4 (Color:vector3, Strength:scalar). '%s' is not one."),
						*Entry.Left(64));
					return false;
				}
				Names.Add(Name.TrimStartAndEnd());
				B.G.FunctionInputs.Add({Name.TrimStartAndEnd(), Type.TrimStartAndEnd().ToLower()});
			}
			if (Names.Num() > MaxInputs)
			{
				B.Error = FString::Printf(TEXT("At most %d inputs."), MaxInputs);
				return false;
			}
			B.G.bFunction = true;
			B.G.FunctionOutput = OutputName;
			B.Node(TEXT("code"), TEXT("custom_hlsl"), {{TEXT("code"), Code}, {TEXT("output_type"), OutputType}, {TEXT("inputs"), FString::Join(Names, TEXT(","))},
				{TEXT("description"), OutputName}});
			for (int32 Index = 0; Index < B.G.FunctionInputs.Num(); ++Index)
			{
				const TPair<FString, FString>& Input = B.G.FunctionInputs[Index];
				const FString Id = TEXT("in_") + Input.Key.ToLower();
				B.Node(Id, TEXT("function_input"), {{TEXT("name"), Input.Key}, {TEXT("type"), Input.Value}, {TEXT("sort"), FString::FromInt(Index)}});
				B.Wire(Id, FString(), TEXT("code"), Input.Key);
			}
			B.Node(TEXT("out"), TEXT("function_output"), {{TEXT("name"), OutputName}});
			B.Wire(TEXT("code"), FString(), TEXT("out"), FString());
			B.G.Parameters.Add(FString::Printf(TEXT("%s (function output)"), *OutputName));
			return true;
		}

		/** Ignores spaces and case, and a function call's " (V3)" type suffix. */
		FString Squash(const FString& Name)
		{
			FString Left = Name;
			int32 Paren = INDEX_NONE;
			if (Left.FindChar(TEXT('('), Paren) && Paren > 0) Left.LeftInline(Paren);
			return Left.Replace(TEXT(" "), TEXT("")).ToLower();
		}

		bool Connect(UMaterialExpression& From, const FString& FromOutput, UMaterialExpression& To, const FString& ToInput, FString& OutError)
		{
			int32 OutputIndex = FromOutput.IsEmpty() ? 0 : INDEX_NONE;
			for (int32 Index = 0; Index < From.Outputs.Num() && OutputIndex == INDEX_NONE; ++Index)
			{
				if (From.Outputs[Index].OutputName.ToString().Equals(FromOutput, ESearchCase::IgnoreCase)) OutputIndex = Index;
			}
			if (OutputIndex == INDEX_NONE || From.Outputs.IsEmpty())
			{
				OutError = FString::Printf(TEXT("%s has no output %s."), *From.GetClass()->GetName(), *FromOutput);
				return false;
			}
			TArray<FString> Names;
			for (int32 Index = 0; Index < 64; ++Index)
			{
				FExpressionInput* Input = To.GetInput(Index);
				if (!Input) break;
				const FString Name = To.GetInputName(Index).ToString();
				Names.Add(Name);
				if (ToInput.IsEmpty() ? Index == 0 : Squash(Name) == Squash(ToInput))
				{
					Input->Connect(OutputIndex, &From);
					return true;
				}
			}
			OutError = FString::Printf(TEXT("%s has no input %s (inputs: %s)."), *To.GetClass()->GetName(), *ToInput, *FString::Join(Names, TEXT(", ")));
			return false;
		}

		EMaterialProperty PropertyOf(const FString& Name)
		{
			return Name == TEXT("front_material") ? MP_FrontMaterial : Gate::OutputPropertyFromName(Name);
		}

		UClass* ClassOf(const FString& Kind)
		{
			if (Kind == TEXT("slw_output")) return UMaterialExpressionSingleLayerWaterMaterialOutput::StaticClass();
			if (Kind == TEXT("landscape_coords")) return UMaterialExpressionLandscapeLayerCoords::StaticClass();
			if (Kind == TEXT("landscape_layer_blend")) return UMaterialExpressionLandscapeLayerBlend::StaticClass();
			if (Kind == TEXT("substrate_slab")) return UMaterialExpressionSubstrateSlabBSDF::StaticClass();
			if (Kind == TEXT("function_input")) return UMaterialExpressionFunctionInput::StaticClass();
			if (Kind == TEXT("function_output")) return UMaterialExpressionFunctionOutput::StaticClass();
			const Gate::FNodeKindInfo* Info = Gate::FindNodeKind(Kind);
			return Info ? Gate::ResolveKindClass(*Info) : nullptr;
		}

		FString PropertyValue(const FNode& Node, const TCHAR* Key)
		{
			const TPair<FString, FString>* Found = Node.Properties.FindByPredicate([Key](const TPair<FString, FString>& Pair) { return Pair.Key == Key; });
			return Found ? Found->Value : FString();
		}

		/** The nodes the catalog does not make, set field by field; nothing here is chosen by name at run time. */
		bool ConfigureSpecial(UMaterialExpression& Expression, const FNode& Node, FString& OutError)
		{
			if (auto* Coords = Cast<UMaterialExpressionLandscapeLayerCoords>(&Expression))
			{
				double Scale = 0.0;
				ParseNumber(PropertyValue(Node, TEXT("mapping_scale")), Scale);
				Coords->MappingScale = static_cast<float>(Scale);
				return true;
			}
			if (auto* Blend = Cast<UMaterialExpressionLandscapeLayerBlend>(&Expression))
			{
				TArray<FString> Layers;
				PropertyValue(Node, TEXT("layers")).ParseIntoArray(Layers, TEXT(","));
				for (int32 Index = 0; Index < Layers.Num(); ++Index)
				{
					FLayerBlendInput& Layer = Blend->Layers.AddDefaulted_GetRef();
					Layer.LayerName = FName(*Layers[Index]);
					Layer.BlendType = LB_HeightBlend;
					Layer.PreviewWeight = Index == 0 ? 1.f : 0.f;
					Layer.ConstHeightInput = 0.5f;
				}
				return true;
			}
			if (auto* Input = Cast<UMaterialExpressionFunctionInput>(&Expression))
			{
				const FString Type = PropertyValue(Node, TEXT("type"));
				Input->InputName = FName(*PropertyValue(Node, TEXT("name")));
				Input->InputType = Type == TEXT("scalar") ? FunctionInput_Scalar : Type == TEXT("vector2") ? FunctionInput_Vector2
					: Type == TEXT("vector4") ? FunctionInput_Vector4 : FunctionInput_Vector3;
				LexTryParseString(Input->SortPriority, *PropertyValue(Node, TEXT("sort")));
				Input->PreviewValue = FVector4f(1.f, 1.f, 1.f, 1.f);
				return true;
			}
			if (auto* Output = Cast<UMaterialExpressionFunctionOutput>(&Expression))
			{
				Output->OutputName = FName(*PropertyValue(Node, TEXT("name")));
				return true;
			}
			if (Expression.IsA<UMaterialExpressionSingleLayerWaterMaterialOutput>() || Expression.IsA<UMaterialExpressionSubstrateSlabBSDF>())
			{
				return true;
			}
			OutError = FString::Printf(TEXT("%s is not a recipe node."), *Node.Kind);
			return false;
		}

		bool ApplySettings(const FGraph& Graph, UMaterial& Material, FString& OutError)
		{
			if (Graph.Domain == TEXT("deferred_decal")) Material.MaterialDomain = MD_DeferredDecal;
			else if (Graph.Domain != TEXT("surface"))
			{
				OutError = FString::Printf(TEXT("Unknown domain %s."), *Graph.Domain);
				return false;
			}
			Material.BlendMode = Graph.BlendMode == TEXT("translucent") ? BLEND_Translucent : BLEND_Opaque;
			if (Graph.ShadingModel == TEXT("single_layer_water")) Material.SetShadingModel(MSM_SingleLayerWater);
			return true;
		}
	}

	TArray<FHyperAIMaterialRecipeInfo> List(const FString& Only)
	{
		TArray<FHyperAIMaterialRecipeInfo> Out;
		for (const FRecipeDef& Def : Defs())
		{
			if (!Only.IsEmpty() && Only != Def.Id) continue;
			FHyperAIMaterialRecipeInfo& Info = Out.AddDefaulted_GetRef();
			Info.Id = Def.Id;
			Info.Summary = Def.Summary;
			for (const FSetting& Setting : Def.Settings)
			{
				Info.Settings.Add(FString::Printf(TEXT("%s (%s): %s"), Setting.Key, *FString(Setting.Default).Left(80), Setting.Help));
			}
		}
		return Out;
	}

	bool IsRecipe(const FString& Id)
	{
		return FindDef(Id) != nullptr;
	}

	bool IsCodeRecipe(const FString& Id)
	{
		return Id == TEXT("custom_hlsl") || Id == TEXT("material_function");
	}

	bool BuildGraph(const FString& Recipe, const TMap<FString, FString>& Settings, const FString& Code, FGraph& Out, FString& OutError)
	{
		const FRecipeDef* Def = FindDef(Recipe);
		if (!Def)
		{
			OutError = FString::Printf(TEXT("'%s' is not a recipe. Recipes: layered_pbr, water, decal, landscape_layers, custom_hlsl, substrate_slab, material_function."), *Recipe.Left(64));
			return false;
		}
		TMap<FString, FString> Effective;
		if (!Resolve(*Def, Settings, Effective, OutError))
		{
			return false;
		}
		Out = FGraph();
		Out.Recipe = Recipe;
		FBuilder Builder{Out, Effective};
		const bool bBuilt = Recipe == TEXT("layered_pbr") ? BuildLayeredPbr(Builder)
			: Recipe == TEXT("water") ? BuildWater(Builder)
			: Recipe == TEXT("decal") ? BuildDecal(Builder)
			: Recipe == TEXT("landscape_layers") ? BuildLandscapeLayers(Builder)
			: Recipe == TEXT("custom_hlsl") ? BuildCustomHlsl(Builder, Code)
			: Recipe == TEXT("substrate_slab") ? BuildSubstrate(Builder)
			: BuildFunction(Builder, Code);
		if (!bBuilt)
		{
			OutError = Builder.Error;
		}
		return bBuilt;
	}

	bool BuildCallerGraph(const FGraph& Function, const FString& FunctionPath, const TMap<FString, FString>& Settings, FGraph& Out, FString& OutError)
	{
		const FString Target = Settings.Contains(TEXT("caller_output")) ? Settings[TEXT("caller_output")].ToLower() : FString(TEXT("emissive"));
		if (Target != TEXT("emissive") && Target != TEXT("base_color"))
		{
			OutError = TEXT("caller_output is emissive or base_color.");
			return false;
		}
		Out = FGraph();
		Out.Recipe = Function.Recipe;
		const TMap<FString, FString> NoSettings;
		FBuilder Builder{Out, NoSettings};
		Builder.Node(TEXT("call"), TEXT("function_call"), {{TEXT("function"), FunctionPath}});
		for (const TPair<FString, FString>& Input : Function.FunctionInputs)
		{
			const FString Id = TEXT("in_") + Input.Key.ToLower();
			if (Input.Value == TEXT("scalar"))
			{
				Builder.Scalar(Id, Input.Key, 1.0, TEXT("Function"));
				Builder.Wire(Id, FString(), TEXT("call"), Input.Key);
				continue;
			}
			Builder.Vector(Id, Input.Key, TEXT("1,1,1,1"), TEXT("Function"));
			if (Input.Value == TEXT("vector2"))
			{
				Builder.Node(Id + TEXT("_xy"), TEXT("component_mask"), {{TEXT("r"), TEXT("true")}, {TEXT("g"), TEXT("true")}, {TEXT("b"), TEXT("false")}, {TEXT("a"), TEXT("false")}});
				Builder.Wire(Id, FString(), Id + TEXT("_xy"), FString());
				Builder.Wire(Id + TEXT("_xy"), FString(), TEXT("call"), Input.Key);
			}
			else
			{
				Builder.Wire(Id, Input.Value == TEXT("vector4") ? TEXT("RGBA") : FString(), TEXT("call"), Input.Key);
			}
		}
		Builder.Out(*Target, TEXT("call"), Function.FunctionOutput);
		return true;
	}

	TArray<FString> Describe(const FGraph& Graph)
	{
		TArray<FString> Lines;
		for (const FNode& Node : Graph.Nodes)
		{
			TArray<FString> Shown;
			for (const TPair<FString, FString>& Property : Node.Properties)
			{
				Shown.Add(FString::Printf(TEXT("%s=%s"), *Property.Key, *Property.Value.Left(Property.Key == TEXT("code") ? 48 : 96).Replace(TEXT("\n"), TEXT(" "))));
			}
			Lines.Add(FString::Printf(TEXT("%s %s%s"), *Node.Id, *Node.Kind, Shown.IsEmpty() ? TEXT("") : *(TEXT(" ") + FString::Join(Shown, TEXT(" ")))));
		}
		for (const FWire& Wire : Graph.Wires)
		{
			Lines.Add(FString::Printf(TEXT("%s%s -> %s.%s"), *Wire.From, Wire.FromOutput.IsEmpty() ? TEXT("") : *(TEXT(".") + Wire.FromOutput),
				*Wire.To, Wire.ToInput.IsEmpty() ? TEXT("in") : *Wire.ToInput));
		}
		for (const FOutput& Output : Graph.Outputs)
		{
			Lines.Add(FString::Printf(TEXT("%s%s -> %s"), *Output.From, Output.FromOutput.IsEmpty() ? TEXT("") : *(TEXT(".") + Output.FromOutput), *Output.Property));
		}
		return Lines;
	}

	bool CheckAssets(const FGraph& Graph, FString& OutError)
	{
		for (const FNode& Node : Graph.Nodes)
		{
			const FString Texture = PropertyValue(Node, TEXT("texture"));
			if (!Texture.IsEmpty() && !Cast<UTexture>(FSoftObjectPath(Texture).TryLoad()))
			{
				OutError = FString::Printf(TEXT("%s is not a texture that loads."), *Texture.Left(256));
				return false;
			}
		}
		return true;
	}

	bool Instantiate(const FGraph& Graph, UMaterial* Material, UMaterialFunction* Function, FString& OutError)
	{
		if ((Material == nullptr) == (Function == nullptr) || (Function != nullptr) != Graph.bFunction)
		{
			OutError = TEXT("A recipe builds into exactly one empty material or material function.");
			return false;
		}
		if (Material && !ApplySettings(Graph, *Material, OutError))
		{
			return false;
		}
		if (Function)
		{
			Function->Description = TEXT("Made by hyper_material_recipe.");
		}
		TMap<FString, UMaterialExpression*> Made;
		int32 Column = 0;
		for (const FNode& Node : Graph.Nodes)
		{
			UClass* Class = ClassOf(Node.Kind);
			UMaterialExpression* Expression = Class
				? UMaterialEditingLibrary::CreateMaterialExpressionEx(Material, Function, Class, nullptr, -300 * (1 + Column % 6), 160 * (Column / 6))
				: nullptr;
			++Column;
			if (!Expression)
			{
				OutError = FString::Printf(TEXT("Could not make node %s (%s)."), *Node.Id, *Node.Kind);
				return false;
			}
			if (Gate::FindNodeKind(Node.Kind))
			{
				// A function call builds its pins from its function, which needs the owning material: the one key written with notification.
				const bool bNotify = Node.Kind == TEXT("function_call");
				for (const TPair<FString, FString>& Property : Node.Properties)
				{
					if (!Gate::WriteProperty(*Expression, Node.Kind, Property.Key, Property.Value, bNotify, OutError))
					{
						OutError = FString::Printf(TEXT("Node %s: %s"), *Node.Id, *OutError);
						return false;
					}
				}
			}
			else if (!ConfigureSpecial(*Expression, Node, OutError))
			{
				return false;
			}
			Made.Add(Node.Id, Expression);
		}
		for (const FWire& Wire : Graph.Wires)
		{
			UMaterialExpression* From = Made.FindRef(Wire.From);
			UMaterialExpression* To = Made.FindRef(Wire.To);
			if (!From || !To || !Connect(*From, Wire.FromOutput, *To, Wire.ToInput, OutError))
			{
				OutError = FString::Printf(TEXT("Wire %s -> %s.%s: %s"), *Wire.From, *Wire.To, *Wire.ToInput, From && To ? *OutError : TEXT("no such node"));
				return false;
			}
		}
		for (const FOutput& Output : Graph.Outputs)
		{
			UMaterialExpression* From = Made.FindRef(Output.From);
			const EMaterialProperty Property = PropertyOf(Output.Property);
			FExpressionInput* Input = Material && Property != MP_MAX ? Material->GetExpressionInputForProperty(Property) : nullptr;
			int32 OutputIndex = Output.FromOutput.IsEmpty() ? 0 : INDEX_NONE;
			for (int32 Index = 0; From && Index < From->Outputs.Num() && OutputIndex == INDEX_NONE; ++Index)
			{
				if (From->Outputs[Index].OutputName.ToString().Equals(Output.FromOutput, ESearchCase::IgnoreCase)) OutputIndex = Index;
			}
			if (!From || !Input || OutputIndex == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Output %s from %s.%s could not be wired."), *Output.Property, *Output.From, *Output.FromOutput);
				return false;
			}
			Input->Connect(OutputIndex, From);
		}
		if (Material)
		{
			UMaterialEditingLibrary::LayoutMaterialExpressions(Material);
		}
		else
		{
			UMaterialEditingLibrary::LayoutMaterialFunctionExpressions(Function);
			UMaterialEditingLibrary::UpdateMaterialFunction(Function, nullptr);
		}
		return true;
	}

	FHyperAIMaterialCompileStats CompileNow(UMaterial& Material)
	{
		// RecompileMaterial's own steps, less its garbage collection and texture streaming rebuild: a collection here would
		// take the transient materials a dry run compiles.
		{
			FMaterialUpdateContext UpdateContext;
			UpdateContext.AddMaterial(&Material);
			Material.PreEditChange(nullptr);
			Material.PostEditChange();
		}
		FMaterialResource* Resource = Material.GetMaterialResource(GMaxRHIShaderPlatform);
		if (Resource)
		{
			Resource->FinishCompilation();
		}
		FHyperAIMaterialCompileStats Stats = Gate::ReadStats(Material);
		// The compiler files its errors on the resource of the quality level it compiled; read them all.
		for (int32 Quality = 0; Quality < EMaterialQualityLevel::Num; ++Quality)
		{
			const FMaterialResource* Level = Material.GetMaterialResource(GMaxRHIShaderPlatform, static_cast<EMaterialQualityLevel::Type>(Quality));
			for (const FString& Error : Level ? Level->GetCompileErrors() : TArray<FString>())
			{
				if (Stats.CompileErrors.Num() < 16) Stats.CompileErrors.AddUnique(Error.Left(512));
			}
		}
		if (Stats.CompileErrors.IsEmpty() && Resource && !Resource->IsGameThreadShaderMapComplete())
		{
			Stats.CompileErrors.Add(TEXT("The shaders did not compile; the Output Log has the compiler's messages (LogShaderCompilers)."));
		}
		if (!Stats.CompileErrors.IsEmpty())
		{
			Stats.Status = TEXT("compile_errors");
		}
		return Stats;
	}

	bool IsSubstrateOn()
	{
		return Substrate::IsSubstrateEnabled();
	}
}

namespace HyperAIStudio::Materials::RecipeFlow
{
	using namespace HyperAIStudio::Materials::Recipes;
	using C = FHyperAIStudioMaterialsContracts;

	FString NewOperationId(const FString& Requested)
	{
		const FString Trimmed = Requested.TrimStartAndEnd();
		return Trimmed.IsEmpty() ? FString::Printf(TEXT("matrecipe-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower()) : Trimmed;
	}

	FString ObjectPathOf(const FString& Package)
	{
		return Package + TEXT(".") + FPackageName::GetShortName(Package);
	}

	void Record(const FString& OperationId, const FString& Target, const bool bOk, const FString& Diagnostic)
	{
		FHyperAIStudioActivityEntry Entry;
		Entry.Kind = bOk ? EHyperAIStudioActivityKind::Completed : EHyperAIStudioActivityKind::Failed;
		Entry.PackId = C::PackId;
		Entry.ToolName = C::RecipeToolName;
		Entry.Target = Target.Left(200);
		Entry.OperationId = OperationId;
		Entry.StatusCode = bOk ? TEXT("completed") : TEXT("failed");
		Entry.Detail = Diagnostic.Left(4000);
		FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
	}

	template <typename T>
	T* NewAsset(const FString& Package)
	{
		UPackage* Outer = CreatePackage(*Package);
		T* Asset = Outer ? NewObject<T>(Outer, FName(*FPackageName::GetShortName(Package)), RF_Public | RF_Standalone | RF_Transactional) : nullptr;
		if (Asset)
		{
			FAssetRegistryModule::AssetCreated(Asset);
			Outer->MarkPackageDirty();
		}
		return Asset;
	}

	/**
	 * Builds the recipe off to the side in the transient package and compiles it: a code recipe's dry run, so HLSL errors
	 * come back before anything is made. A function compiles through a caller that uses it.
	 */
	void CompileAside(const FGraph& Graph, const TMap<FString, FString>& Settings, FHyperAIMaterialRecipeReport& Report)
	{
		FString Error;
		UMaterial* Target = nullptr;
		// Held until the compile is read: nothing else refers to these.
		TStrongObjectPtr<UMaterialFunction> KeepFunction;
		TStrongObjectPtr<UMaterial> KeepMaterial;
		if (Graph.bFunction)
		{
			UMaterialFunction* Function = NewObject<UMaterialFunction>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(),
				UMaterialFunction::StaticClass(), TEXT("MF_HyperAIRecipeCheck")), RF_Transient);
			KeepFunction.Reset(Function);
			FGraph Caller;
			if (!Instantiate(Graph, nullptr, Function, Error) || !BuildCallerGraph(Graph, Function->GetPathName(), Settings, Caller, Error))
			{
				Report.Stats.CompileErrors.Add(Error);
				return;
			}
			Target = NewObject<UMaterial>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UMaterial::StaticClass(), TEXT("M_HyperAIRecipeCheck")), RF_Transient);
			KeepMaterial.Reset(Target);
			if (!Instantiate(Caller, Target, nullptr, Error))
			{
				Report.Stats.CompileErrors.Add(Error);
				return;
			}
		}
		else
		{
			Target = NewObject<UMaterial>(GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UMaterial::StaticClass(), TEXT("M_HyperAIRecipeCheck")), RF_Transient);
			KeepMaterial.Reset(Target);
			if (!Instantiate(Graph, Target, nullptr, Error))
			{
				Report.Stats.CompileErrors.Add(Error);
				return;
			}
		}
		KeepMaterial.Reset(Target);
		Report.Stats = CompileNow(*Target);
	}

	struct FPlan
	{
		FString Recipe;
		FString Package;
		FString CallerPackage;
		FGraph Graph;
		FGraph Caller;
		TMap<FString, FString> Settings;
	};

	FHyperAIMaterialRecipeReport Execute(const FPlan& Plan, const FString& OperationId)
	{
		FHyperAIMaterialRecipeReport Report;
		Report.OperationId = OperationId;
		Report.Recipe = Plan.Recipe;
		Report.Path = ObjectPathOf(Plan.Package);
		Report.Parameters = Plan.Graph.Parameters;
		Report.Notes = Plan.Graph.Notes;
		Report.bSubstrateEnabled = IsSubstrateOn();
		if (!FHyperAIStudioNames::IsFreePackage(Plan.Package) || (!Plan.CallerPackage.IsEmpty() && !FHyperAIStudioNames::IsFreePackage(Plan.CallerPackage)))
		{
			Report.Status = TEXT("operation_failed");
			Report.Diagnostic = TEXT("An asset appeared at the path while the plan waited; nothing was made over it.");
			return Report;
		}
		FString Error;
		UMaterial* Material = nullptr;
		UMaterialFunction* Function = nullptr;
		UMaterial* Caller = nullptr;
		{
			const FScopedTransaction Transaction(LOCTEXT("MaterialRecipe", "HyperAI: Material Recipe"));
			bool bBuilt = false;
			if (Plan.Graph.bFunction)
			{
				Function = NewAsset<UMaterialFunction>(Plan.Package);
				bBuilt = Function && Instantiate(Plan.Graph, nullptr, Function, Error);
				if (bBuilt && !Plan.CallerPackage.IsEmpty())
				{
					Caller = NewAsset<UMaterial>(Plan.CallerPackage);
					bBuilt = Caller && Instantiate(Plan.Caller, Caller, nullptr, Error);
				}
			}
			else
			{
				Material = NewAsset<UMaterial>(Plan.Package);
				bBuilt = Material && Instantiate(Plan.Graph, Material, nullptr, Error);
			}
			if (!bBuilt)
			{
				Report.Status = TEXT("operation_failed");
				Report.Diagnostic = Error.IsEmpty() ? FString(TEXT("The asset could not be created.")) : Error;
				return Report;
			}
		}
		UMaterial* Compiled = Material ? Material : Caller;
		if (!Compiled)
		{
			// A function alone compiles through a caller that is never saved.
			FHyperAIMaterialRecipeReport Aside;
			CompileAside(Plan.Graph, Plan.Settings, Aside);
			Report.Stats = Aside.Stats;
		}
		else
		{
			Report.Stats = CompileNow(*Compiled);
		}
		if (!Report.Stats.CompileErrors.IsEmpty())
		{
			Report.Status = TEXT("compile_errors");
			Report.Diagnostic = FString::Printf(TEXT("%s was made but does not compile, so it was not saved: %s Fix it with hyper_material_apply_plan edit_graph, or undo."),
				*Report.Path, *FString::Join(Report.Stats.CompileErrors, TEXT(" | ")).Left(1500));
			return Report;
		}
		FString PreviewError;
		if (Compiled && !FHyperAIStudioResultPreview::WriteAssetPreview(*Compiled, Report.PreviewImagePath, PreviewError))
		{
			Report.PreviewImagePath.Reset();
			Report.Notes.Add(TEXT("No preview: ") + PreviewError);
		}
		for (UObject* Asset : TArray<UObject*>{Function, Material, Caller})
		{
			if (!Asset) continue;
			if (!FHyperAIStudioSourceControl::SaveAsset(*Asset, Error))
			{
				Report.Status = TEXT("operation_failed");
				Report.Diagnostic = FString::Printf(TEXT("%s was made and compiles but could not be saved: %s"), *Asset->GetPathName(), *Error);
				return Report;
			}
			Report.Saved.Add(Asset->GetPathName());
		}
		if (Caller)
		{
			Report.CallerPath = Caller->GetPathName();
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = FString::Printf(TEXT("Recipe %s made %s (%d nodes) in one undo step, compiled clean (%d pixel shader instructions) and saved. "
			"Parameters: %s. Open the preview; make instances with hyper_material_apply_plan create_material_instance."),
			*Plan.Recipe, *Report.Path, Plan.Graph.Nodes.Num() + Plan.Caller.Nodes.Num(), Report.Stats.NumPixelShaderInstructions,
			*FString::Join(Report.Parameters, TEXT(", ")));
		return Report;
	}
}

FHyperAIMaterialRecipeReport FHyperAIStudioMaterialsContracts::Recipe(const FHyperAIMaterialRecipeRequest& Request)
{
	using namespace HyperAIStudio::Materials::Recipes;
	using namespace HyperAIStudio::Materials::RecipeFlow;
	FHyperAIMaterialRecipeReport Report;
	Report.OperationId = NewOperationId(Request.OperationId);
	if (FHyperAIStudioNames::RefuseRequest(Request, Report, {TEXT("Value")}))
	{
		return Report;
	}
	Report.bSubstrateEnabled = IsSubstrateOn();
	const FString RecipeId = Request.Recipe.TrimStartAndEnd().ToLower();
	Report.Recipe = RecipeId;
	if (RecipeId.IsEmpty())
	{
		Report.bOk = true;
		Report.Status = TEXT("listed");
		Report.Recipes = List();
		Report.Diagnostic = FString::Printf(TEXT("%d recipes; pick one with recipe and path, settings as key/value. Substrate is %s in this project."),
			Report.Recipes.Num(), Report.bSubstrateEnabled ? TEXT("on") : TEXT("off"));
		return Report;
	}
	auto Refuse = [&Report](const TCHAR* Status, const FString& Why)
	{
		Report.bOk = false;
		Report.Status = Status;
		Report.Diagnostic = Why;
		return Report;
	};
	if (!IsRecipe(RecipeId))
	{
		Report.Recipes = List();
		return Refuse(TEXT("invalid_request"), FString::Printf(TEXT("'%s' is not a recipe; Recipes lists them."), *RecipeId.Left(64)));
	}
	Report.Recipes = List(RecipeId);
	if (!IsInGameThread())
	{
		return Refuse(TEXT("invalid_request"), TEXT("hyper_material_recipe runs on the game thread."));
	}
	if (GEditor && GEditor->PlayWorld)
	{
		return Refuse(TEXT("play_in_editor_running"), TEXT("Stop Play In Editor first: materials compiled now would hitch the running game."));
	}
	if (RecipeId == TEXT("substrate_slab") && !Report.bSubstrateEnabled)
	{
		return Refuse(TEXT("substrate_disabled"), TEXT("Substrate is off in this project (Project Settings > Rendering > Substrate, r.Substrate, needs a restart), "
			"so a Substrate slab would not render as one. Use layered_pbr, or turn Substrate on and restart the editor first."));
	}
	FString Reason;
	const FString Package = Request.Path.TrimStartAndEnd();
	if (!FHyperAIStudioNames::IsPackagePath(Package, TEXT("Path"), &Reason) || !Package.StartsWith(TEXT("/Game/")))
	{
		return Refuse(TEXT("invalid_request"), Reason.IsEmpty() ? FString(TEXT("Path is the new asset's package path under /Game, such as /Game/Materials/M_Rock.")) : Reason);
	}
	if (!FHyperAIStudioNames::IsFreePackage(Package))
	{
		return Refuse(TEXT("asset_exists"), FString::Printf(TEXT("%s is taken; a recipe makes a new asset, so give a path of its own (or edit that one with hyper_material_apply_plan)."), *Package));
	}
	const FString CallerPackage = Request.CallerPath.TrimStartAndEnd();
	if (!CallerPackage.IsEmpty())
	{
		if (RecipeId != TEXT("material_function"))
		{
			return Refuse(TEXT("invalid_request"), TEXT("caller_path goes only with material_function."));
		}
		if (!FHyperAIStudioNames::IsPackagePath(CallerPackage, TEXT("CallerPath"), &Reason) || !CallerPackage.StartsWith(TEXT("/Game/")) || CallerPackage == Package)
		{
			return Refuse(TEXT("invalid_request"), Reason.IsEmpty() ? FString(TEXT("caller_path is another new package path under /Game.")) : Reason);
		}
		if (!FHyperAIStudioNames::IsFreePackage(CallerPackage))
		{
			return Refuse(TEXT("asset_exists"), FString::Printf(TEXT("%s is taken."), *CallerPackage));
		}
	}

	FPlan Plan;
	Plan.Recipe = RecipeId;
	Plan.Package = Package;
	Plan.CallerPackage = CallerPackage;
	for (const FHyperAIMaterialNodeProperty& Setting : Request.Settings)
	{
		Plan.Settings.Add(Setting.Key.TrimStartAndEnd().ToLower(), Setting.Value);
	}
	FString Error;
	if (!BuildGraph(RecipeId, Plan.Settings, Request.Code, Plan.Graph, Error))
	{
		return Refuse(TEXT("invalid_request"), Error);
	}
	if (!CallerPackage.IsEmpty() && !BuildCallerGraph(Plan.Graph, ObjectPathOf(Package), Plan.Settings, Plan.Caller, Error))
	{
		return Refuse(TEXT("invalid_request"), Error);
	}
	if (!CheckAssets(Plan.Graph, Error))
	{
		return Refuse(TEXT("invalid_request"), Error);
	}
	Report.Path = ObjectPathOf(Package);
	Report.CallerPath = CallerPackage.IsEmpty() ? FString() : ObjectPathOf(CallerPackage);
	Report.Graph = Describe(Plan.Graph);
	for (const FString& Line : Describe(Plan.Caller))
	{
		Report.Graph.Add(TEXT("caller: ") + Line);
	}
	Report.Parameters = Plan.Graph.Parameters;
	Report.Parameters.Append(Plan.Caller.Parameters);
	Report.Notes = Plan.Graph.Notes;

	if (Request.bDryRun)
	{
		if (IsCodeRecipe(RecipeId))
		{
			CompileAside(Plan.Graph, Plan.Settings, Report);
			if (!Report.Stats.CompileErrors.IsEmpty())
			{
				return Refuse(TEXT("compile_errors"), TEXT("The code does not compile; nothing was made. Fix it and dry-run again: ")
					+ FString::Join(Report.Stats.CompileErrors, TEXT(" | ")).Left(1500));
			}
		}
		Report.bOk = true;
		Report.Status = TEXT("checked");
		Report.Diagnostic = FString::Printf(TEXT("Would make %s from %s: %d nodes, %d parameters%s; nothing was changed. Resubmit with bDryRun false and an operation_id."),
			*Report.Path, *RecipeId, Plan.Graph.Nodes.Num() + Plan.Caller.Nodes.Num(), Report.Parameters.Num(),
			IsCodeRecipe(RecipeId) ? TEXT(", and the code compiles") : TEXT(""));
		return Report;
	}

	const FString OperationId = Report.OperationId;
	const FString Target = Report.Path;
	auto Run = [Plan, OperationId]() { return Execute(Plan, OperationId); };
	if (!FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
	{
		FHyperAIMaterialRecipeReport Done = Run();
		Record(OperationId, Target, Done.bOk, Done.Diagnostic);
		return Done;
	}
	FHyperAIStudioApprovalSummary Summary;
	Summary.PackId = C::PackId;
	Summary.ToolName = C::RecipeToolName;
	Summary.VariantId = C::RecipeVariantId;
	Summary.Safety = EHyperAIStudioDomainSafety::Edit;
	Summary.EffectTarget = Target;
	Summary.Effects.Add(FString::Printf(TEXT("%s %s from the %s recipe (%d nodes), compiled, previewed and saved"),
		Plan.Graph.bFunction ? TEXT("Material function") : TEXT("Material"), *Package, *RecipeId, Plan.Graph.Nodes.Num()));
	if (!CallerPackage.IsEmpty())
	{
		Summary.Effects.Add(FString::Printf(TEXT("Material %s calling it"), *CallerPackage));
	}
	Summary.Effects.Add(TEXT("| parameters: ") + FString::Join(Report.Parameters, TEXT(", ")).Left(400));
	Summary.Touches.Add(Package);
	if (!CallerPackage.IsEmpty()) Summary.Touches.Add(CallerPackage);
	if (!FHyperAIStudioApprovalGate::RequestAction(OperationId, Summary, [Run, OperationId, Target]()
		{
			const FHyperAIMaterialRecipeReport Done = Run();
			Record(OperationId, Target, Done.bOk, Done.Diagnostic);
		}, Error))
	{
		return Refuse(TEXT("approval_not_queued"), Error);
	}
	Report.bOk = true;
	Report.Status = TEXT("awaiting_user_approval");
	Report.Diagnostic = TEXT("Agent edits need approval here. Waiting in HyperAI Chat, Activity panel; nothing ran yet. Call hyper_operation_wait with operation_id and bWaitThroughApproval true.");
	FHyperAIStudioApprovalGate::DescribeHeld(OperationId, Report.Status, Report.Diagnostic);
	return Report;
}

#undef LOCTEXT_NAMESPACE
