// Games by Hyper 2026.

#include "HyperAIStudioNiagaraRecipes.h"

#include "HyperAIStudioNiagaraExternalEditGate.h"
#include "Misc/PackageName.h"
#include "NiagaraScript.h"
#include "UObject/SoftObjectPath.h"

namespace HyperAIStudio::Niagara::Recipes
{
	namespace
	{
		const TCHAR* const Fountain = TEXT("/Niagara/DefaultAssets/Templates/Emitters/Fountain.Fountain");
		const TCHAR* const SingleParticle = TEXT("/Niagara/DefaultAssets/Templates/Emitters/SingleLoopingParticle.SingleLoopingParticle");
		const TCHAR* const Burst = TEXT("/Niagara/DefaultAssets/Templates/Emitters/OmnidirectionalBurst.OmnidirectionalBurst");
		const TCHAR* const RandomRange = TEXT("/Niagara/DynamicInputs/UniformRange/V2/RandomRangeFloat.RandomRangeFloat");
		const TCHAR* const ColorFromCurve = TEXT("/Niagara/DynamicInputs/ValueFromCurve/ColorFromCurve.ColorFromCurve");
		const TCHAR* const ColorModule = TEXT("/Niagara/Modules/Update/Color/Color.Color");
		const TCHAR* const FallbackMaterial = TEXT("/Niagara/DefaultAssets/DefaultSpriteMaterial.DefaultSpriteMaterial");
		const TCHAR* const RibbonFallbackMaterial = TEXT("/Niagara/DefaultAssets/DefaultRIbbonMaterial.DefaultRIbbonMaterial");
		const TCHAR* const RibbonRenderer = TEXT("/Script/Niagara.NiagaraRibbonRendererProperties");
		const TCHAR* const Spawn = TEXT("ParticleSpawnScript");
		const TCHAR* const Update = TEXT("ParticleUpdateScript");
		const TCHAR* const EmitterUpdate = TEXT("EmitterUpdateScript");

		/** The materials the recipes draw with, each one hyper_material_recipe makes. */
		struct FMaterialSlot
		{
			const TCHAR* Setting;
			const TCHAR* DefaultPath;
			const TCHAR* Recipe;
			const TCHAR* RecipeSettings;
		};

		const FMaterialSlot Additive = {TEXT("material_additive"), TEXT("/Game/VFX/Materials/M_HyperAI_ParticleAdditive.M_HyperAI_ParticleAdditive"),
			TEXT("particle_additive"), TEXT("{}")};
		const FMaterialSlot Smoke = {TEXT("material_smoke"), TEXT("/Game/VFX/Materials/M_HyperAI_ParticleSmoke.M_HyperAI_ParticleSmoke"),
			TEXT("particle_translucent"), TEXT("{\"noise\": \"0.7\", \"softness\": \"1.5\"}")};
		const FMaterialSlot Ring = {TEXT("material_ring"), TEXT("/Game/VFX/Materials/M_HyperAI_ParticleRing.M_HyperAI_ParticleRing"),
			TEXT("particle_additive"), TEXT("{\"shape\": \"ring\", \"softness\": \"1.5\"}")};

		FRecipeSetting MaterialSetting(const FMaterialSlot& Slot)
		{
			return {Slot.Setting, Slot.DefaultPath, FString::Printf(TEXT("Material for its %s particles; hyper_material_recipe recipe %s settings %s makes the default"),
				Slot.Recipe == FString(TEXT("particle_translucent")) ? TEXT("smoke") : TEXT("glowing"), Slot.Recipe, Slot.RecipeSettings)};
		}

		TArray<FRecipeSetting> Common(const TCHAR* Color, TArray<FRecipeSetting> More)
		{
			TArray<FRecipeSetting> Out = {
				{TEXT("scale"), TEXT("1"), TEXT("Size of the whole effect: 1 is about a metre across; positions, sizes and speeds scale with it")},
				{TEXT("intensity"), TEXT("1"), TEXT("Particle counts and spawn rates multiply by this")},
				{TEXT("color"), Color, TEXT("The main hue, r,g,b 0-1; brightness comes from the recipe")}};
			Out.Append(MoveTemp(More));
			return Out;
		}

		const TArray<FRecipeInfo>& Table()
		{
			static const TArray<FRecipeInfo> Recipes = {
				{TEXT("fire"), TEXT("A looping campfire: rising, flickering flame sprites that cool from white-yellow to deep red, sparks of embers drifting up, and an optional plume of dark smoke above."),
					Common(TEXT("1,0.42,0.1"), {{TEXT("smoke"), TEXT("true"), TEXT("A smoke plume above the flames")}, MaterialSetting(Additive), MaterialSetting(Smoke)})},
				{TEXT("explosion"), TEXT("A one-shot explosion: a white flash, a fireball that expands and cools, a shockwave ring, sparks flung out and falling, and smoke that rises and thins."),
					Common(TEXT("1,0.45,0.12"), {{TEXT("part"), TEXT("1"), TEXT("1 makes the System with the flash, fireball and shockwave; 2 adds the sparks and smoke to it (a second plan: the whole effect is past one plan's time budget)")},
						MaterialSetting(Additive), MaterialSetting(Smoke), MaterialSetting(Ring)})},
				{TEXT("magic_burst"), TEXT("A one-shot spell burst: a glowing core, an expanding ring, and motes that fly out, swirl and shift from the main colour to the accent colour as they fade."),
					Common(TEXT("0.3,0.6,1"), {{TEXT("accent"), TEXT("1,0.3,0.9"), TEXT("The colour the motes turn as they fade, r,g,b")}, MaterialSetting(Additive), MaterialSetting(Ring)})},
				{TEXT("impact_sparks"), TEXT("A one-shot bullet or blade impact on a surface below: a bright flash, hot streaking sparks thrown up and out that fall with gravity, and a small dust puff."),
					Common(TEXT("1,0.6,0.25"), {MaterialSetting(Additive), MaterialSetting(Smoke)})},
				{TEXT("smoke_plume"), TEXT("A looping column of soft smoke that rises, spreads, drifts with the wind and thins out."),
					Common(TEXT("0.13,0.125,0.12"), {{TEXT("wind"), TEXT("40,0,0"), TEXT("Drift in cm/s, x,y,z")}, MaterialSetting(Smoke)})},
				{TEXT("ribbon_trail"), TEXT("A glowing ribbon trail that tapers and fades behind its source; orbit_radius makes it circle on its own so it shows without moving the System."),
					Common(TEXT("0.3,0.75,1"), {{TEXT("orbit_radius"), TEXT("80"), TEXT("Circle radius in cm, 0 to trail only when the System moves")}, MaterialSetting(Additive)})},
			};
			return Recipes;
		}

		FString Num(const double Value)
		{
			return FString::SanitizeFloat(FMath::RoundToDouble(Value * 1000.0) / 1000.0);
		}

		FString Vec(const FVector& Value)
		{
			return Num(Value.X) + TEXT(",") + Num(Value.Y) + TEXT(",") + Num(Value.Z);
		}

		FString Rgba(const FVector& Rgb, const double Alpha)
		{
			return Vec(Rgb) + TEXT(",") + Num(Alpha);
		}

		/** Writes ops a few lines per idea; nothing here touches an object. */
		struct FBuilder
		{
			TArray<FHyperAINiagaraEditOp>& Ops;
			TMap<FString, FString> Settings;
			TArray<FString>& Materials;
			TArray<FString>& Notes;
			FString CurveInput;
			FString Error;
			double Scale = 1;
			double Intensity = 1;
			FVector Color = FVector::OneVector;
			/** False for a recipe part that adds to a System an earlier part made. */
			bool bCreates = true;

			FHyperAINiagaraEditOp& Add(const TCHAR* Kind, const FString& Emitter = FString(), const TCHAR* Script = TEXT(""), const FString& Module = FString())
			{
				FHyperAINiagaraEditOp& Op = Ops.AddDefaulted_GetRef();
				Op.Kind = Kind;
				Op.EmitterName = Emitter;
				Op.ScriptName = Script;
				Op.ModuleName = Module;
				return Op;
			}

			void Emitter(const FString& Name, const TCHAR* Template)
			{
				FHyperAINiagaraEditOp& Op = Add(TEXT("add_emitter"));
				Op.AssetPath = Template;
				Op.Name = Name;
			}

			void Set(const FString& Emitter, const TCHAR* Script, const TCHAR* Module, const TArray<FString>& Stack, const TCHAR* Type, const FString& Value,
				const FString& Asset = FString())
			{
				FHyperAINiagaraEditOp& Op = Add(TEXT("set_input_value"), Emitter, Script, Module);
				Op.InputNameStack = Stack;
				Op.ValueType = Type;
				Op.Value = Value;
				Op.AssetPath = Asset;
			}

			void Float(const FString& Emitter, const TCHAR* Script, const TCHAR* Module, const TCHAR* Input, const double Value)
			{
				Set(Emitter, Script, Module, {Input}, TEXT("float"), Num(Value));
			}

			/** A uniform random value per particle between Min and Max. */
			void Random(const FString& Emitter, const TCHAR* Script, const TCHAR* Module, const TCHAR* Input, const double Min, const double Max)
			{
				Set(Emitter, Script, Module, {Input}, TEXT("dynamic_input"), FString(), RandomRange);
				Set(Emitter, Script, Module, {Input, TEXT("Minimum")}, TEXT("float"), Num(Min));
				Set(Emitter, Script, Module, {Input, TEXT("Maximum")}, TEXT("float"), Num(Max));
			}

			void Disable(const FString& Emitter, const TCHAR* Script, const TCHAR* Module)
			{
				Add(TEXT("set_module_enabled"), Emitter, Script, Module).bEnabled = false;
			}

			void Hlsl(const FString& Emitter, const TCHAR* Script, const FString& Name, const FString& Code)
			{
				FHyperAINiagaraEditOp& Op = Add(TEXT("add_hlsl_module"), Emitter, Script);
				Op.Name = Name;
				Op.Hlsl = Code;
			}

			void Renderer(const FString& Emitter, const int32 Index, const TCHAR* Property, const FString& Value)
			{
				FHyperAINiagaraEditOp& Op = Add(TEXT("set_renderer_property"), Emitter);
				Op.RendererIndex = Index;
				Op.Name = Property;
				Op.Value = Value;
			}

			/** Particles.Color over the particle's life, from colour keys "age:r,g,b,a; ...", through Niagara's Color module. */
			void ColorOverLife(const FString& Emitter, const TArray<TPair<double, FString>>& Keys)
			{
				FHyperAINiagaraEditOp& Module = Add(TEXT("add_module"), Emitter, Update);
				Module.AssetPath = ColorModule;
				Set(Emitter, Update, TEXT("Color"), {TEXT("Color")}, TEXT("dynamic_input"), FString(), ColorFromCurve);
				TArray<FString> Text;
				for (const TPair<double, FString>& Key : Keys)
				{
					Text.Add(Num(Key.Key) + TEXT(":") + Key.Value);
				}
				Set(Emitter, Update, TEXT("Color"), {TEXT("Color"), CurveInput}, TEXT("color_curve"), FString::Join(Text, TEXT("; ")));
			}

			/** Sprite size times (1 + Rate) a second: grows when positive, shrinks when negative. */
			void SizeOverLife(const FString& Emitter, const double Rate)
			{
				Hlsl(Emitter, Update, Emitter + TEXT("Size"), FString::Printf(
					TEXT("// Sprite size changes by %s of itself a second.\nParticles.SpriteSize *= max(0.0, 1.0 + %s * Engine.DeltaTime);"), *Num(Rate), *Num(Rate)));
			}

			/** The common bones of every emitter from a Fountain or burst template. */
			void Particles(const FString& Emitter, const double LifeMin, const double LifeMax, const double SizeMin, const double SizeMax)
			{
				Float(Emitter, Spawn, TEXT("InitializeParticle"), TEXT("Lifetime Min"), LifeMin);
				Float(Emitter, Spawn, TEXT("InitializeParticle"), TEXT("Lifetime Max"), LifeMax);
				Float(Emitter, Spawn, TEXT("InitializeParticle"), TEXT("Uniform Sprite Size Min"), SizeMin * Scale);
				Float(Emitter, Spawn, TEXT("InitializeParticle"), TEXT("Uniform Sprite Size Max"), SizeMax * Scale);
				// The Color module takes over colour, so the templates' own colour scaling would only fight it.
				Disable(Emitter, Update, TEXT("ScaleColor"));
			}

			void Motion(const FString& Emitter, const double SpeedMin, const double SpeedMax, const FVector& Gravity, const double Drag, const double Radius)
			{
				Random(Emitter, Spawn, TEXT("AddVelocity"), TEXT("Velocity Speed"), SpeedMin * Scale, SpeedMax * Scale);
				Set(Emitter, Update, TEXT("GravityForce"), {TEXT("Gravity")}, TEXT("vector"), Vec(Gravity * Scale));
				Float(Emitter, Update, TEXT("Drag"), TEXT("Drag"), Drag);
				Float(Emitter, Spawn, TEXT("ShapeLocation"), TEXT("Sphere Radius"), FMath::Max(1.0, Radius * Scale));
			}

			void Count(const FString& Emitter, const double Count)
			{
				Set(Emitter, EmitterUpdate, TEXT("SpawnBurst_Instantaneous"), {TEXT("Spawn Count")}, TEXT("int32"), FString::FromInt(FMath::Max(1, FMath::RoundToInt(Count * Intensity))));
			}

			void Rate(const FString& Emitter, const double PerSecond)
			{
				Float(Emitter, EmitterUpdate, TEXT("SpawnRate"), TEXT("SpawnRate"), FMath::Max(0.5, PerSecond * Intensity));
			}

			/** A burst emitter that plays once instead of looping. */
			void Once(const FString& Emitter)
			{
				Set(Emitter, EmitterUpdate, TEXT("EmitterState"), {TEXT("Loop Behavior")}, TEXT("enum"), TEXT("Once"));
			}

			/**
			 * One still particle that plays once (a flash, a core, a ring): Niagara's Single Looping Particle template, whose
			 * stack has no forces or colour scaling to switch off, so it costs a handful of ops.
			 */
			void Single(const FString& Emitter, const double Lifetime, const double Size)
			{
				this->Emitter(Emitter, SingleParticle);
				// The template follows the System's life cycle, which hides Loop Behavior; give the emitter its own first.
				Set(Emitter, EmitterUpdate, TEXT("EmitterState"), {TEXT("Life Cycle Mode")}, TEXT("enum"), TEXT("Self"));
				Once(Emitter);
				Float(Emitter, Spawn, TEXT("InitializeParticle"), TEXT("Lifetime"), Lifetime);
				Hlsl(Emitter, Spawn, Emitter + TEXT("Birth"), FString::Printf(TEXT("// One size, set at birth.\nParticles.SpriteSize = float2(%s, %s);"),
					*Num(Size * Scale), *Num(Size * Scale)));
			}

			/** The material a setting names, or Niagara's default when it does not exist yet (and how to make it). */
			FString Material(const FMaterialSlot& Slot, const bool bRibbon = false)
			{
				const FString Path = Settings.FindRef(Slot.Setting);
				const bool bExists = FSoftObjectPath(Path).ResolveObject() || FPackageName::DoesPackageExist(FPackageName::ObjectPathToPackageName(Path));
				const FString Line = FString::Printf(TEXT("%s: %s. hyper_material_recipe with recipe %s, settings %s and path %s makes it."), *Path,
					bExists ? TEXT("exists") : TEXT("missing, so Niagara's default material stands in"), Slot.Recipe, Slot.RecipeSettings,
					*FPackageName::ObjectPathToPackageName(Path));
				Materials.AddUnique(Line);
				return bExists ? Path : FString(bRibbon ? RibbonFallbackMaterial : FallbackMaterial);
			}

			bool Number(const TCHAR* Key, double& Out, const double Min, const double Max)
			{
				const FString Text = Settings.FindRef(Key);
				if (LexTryParseString(Out, *Text) && FMath::IsFinite(Out) && Out >= Min && Out <= Max) return true;
				Error = FString::Printf(TEXT("Setting %s must be a number from %s to %s, not '%s'."), Key, *Num(Min), *Num(Max), *Text.Left(32));
				return false;
			}

			bool Vector(const TCHAR* Key, FVector& Out, const double Min, const double Max)
			{
				TArray<FString> Parts;
				Settings.FindRef(Key).ParseIntoArray(Parts, TEXT(","));
				double Values[3] = {0, 0, 0};
				bool bOk = Parts.Num() == 3;
				for (int32 Index = 0; bOk && Index < 3; ++Index)
				{
					bOk = LexTryParseString(Values[Index], *Parts[Index].TrimStartAndEnd()) && FMath::IsFinite(Values[Index]) && Values[Index] >= Min && Values[Index] <= Max;
				}
				if (bOk)
				{
					Out = FVector(Values[0], Values[1], Values[2]);
					return true;
				}
				Error = FString::Printf(TEXT("Setting %s must be three numbers x,y,z from %s to %s."), Key, *Num(Min), *Num(Max));
				return false;
			}

			bool Flag(const TCHAR* Key, bool& Out)
			{
				const FString Text = Settings.FindRef(Key).ToLower();
				Out = Text == TEXT("true");
				if (Out || Text == TEXT("false")) return true;
				Error = FString::Printf(TEXT("Setting %s is true or false."), Key);
				return false;
			}
		};

		/** Hot at birth, through the main colour, to nothing: the colour keys of things that burn out. */
		TArray<TPair<double, FString>> Burn(const FVector& Color, const double Peak, const double Mid = 0.35)
		{
			const FVector Hot = FMath::Lerp(Color, FVector::OneVector, 0.25) * Peak;
			return {{0.0, Rgba(Hot, 1)}, {Mid, Rgba(Color * Peak * 0.7, 1)}, {0.75, Rgba(Color * Color * Peak * 0.25, 1)}, {1.0, Rgba(FVector::ZeroVector, 0)}};
		}

		/** Fades in quickly, holds, and thins out: the colour keys of smoke. */
		TArray<TPair<double, FString>> Thin(const FVector& Color, const double Alpha)
		{
			return {{0.0, Rgba(Color * 0.8, 0)}, {0.12, Rgba(Color * 0.85, Alpha)}, {0.6, Rgba(Color, Alpha * 0.55)}, {1.0, Rgba(Color * 1.1, 0)}};
		}

		/** Flicker: a sideways push that wanders with age and height. */
		FString Flicker(const double Strength)
		{
			return FString::Printf(TEXT("// Flicker: a sideways push that wanders with age and height.\n"
				"float Phase = Particles.Age * 7.0 + Particles.Position.z * 0.04;\n"
				"Particles.Velocity.xy += float2(sin(Phase), cos(Phase * 1.3)) * %s * Engine.DeltaTime;"), *Num(Strength));
		}

		/** Sparks drawn as streaks: velocity-aligned sprites as long as their speed. */
		void Streaks(FBuilder& B, const FString& Emitter, const double Width, const double Stretch)
		{
			B.Renderer(Emitter, 0, TEXT("Alignment"), TEXT("VelocityAligned"));
			B.Hlsl(Emitter, Update, Emitter + TEXT("Streak"), FString::Printf(
				TEXT("// Thin and as long as the spark is fast.\nParticles.SpriteSize = float2(%s, %s + length(Particles.Velocity) * %s);"),
				*Num(Width * B.Scale), *Num(Width * B.Scale), *Num(Stretch)));
		}

		void BuildFire(FBuilder& B)
		{
			bool bSmoke = true;
			if (!B.Flag(TEXT("smoke"), bSmoke)) return;
			const FString Glow = B.Material(Additive);
			B.Emitter(TEXT("Flames"), Fountain);
			B.Rate(TEXT("Flames"), 50);
			B.Particles(TEXT("Flames"), 0.5, 0.95, 26, 48);
			B.Motion(TEXT("Flames"), 20, 70, FVector(0, 0, 280), 1.6, 14);
			B.ColorOverLife(TEXT("Flames"), Burn(B.Color, 3.2));
			B.SizeOverLife(TEXT("Flames"), -0.9);
			B.Hlsl(TEXT("Flames"), Update, TEXT("FlamesFlicker"), Flicker(90 * B.Scale));
			B.Renderer(TEXT("Flames"), 0, TEXT("Material"), Glow);

			B.Emitter(TEXT("Embers"), Fountain);
			B.Rate(TEXT("Embers"), 9);
			B.Particles(TEXT("Embers"), 1.2, 2.3, 3, 6);
			B.Motion(TEXT("Embers"), 60, 160, FVector(0, 0, 170), 0.9, 18);
			B.ColorOverLife(TEXT("Embers"), Burn(B.Color, 7.0, 0.25));
			B.Hlsl(TEXT("Embers"), Update, TEXT("EmbersFlicker"), Flicker(220 * B.Scale));
			B.Renderer(TEXT("Embers"), 0, TEXT("Material"), Glow);

			if (bSmoke)
			{
				const FString Soot = B.Material(Smoke);
				B.Emitter(TEXT("Smoke"), Fountain);
				B.Rate(TEXT("Smoke"), 6);
				B.Particles(TEXT("Smoke"), 2.4, 3.6, 40, 75);
				B.Motion(TEXT("Smoke"), 10, 40, FVector(0, 0, 140), 0.7, 18);
				B.Hlsl(TEXT("Smoke"), Spawn, TEXT("SmokeAboveFlames"), FString::Printf(TEXT("// Born above the flames.\nParticles.Position.z += %s;"), *Num(70 * B.Scale)));
				B.ColorOverLife(TEXT("Smoke"), Thin(FVector(0.09, 0.085, 0.08), 0.5));
				B.SizeOverLife(TEXT("Smoke"), 0.55);
				B.Renderer(TEXT("Smoke"), 0, TEXT("Material"), Soot);
			}
			B.Notes.Add(TEXT("Loops forever; place it and leave it. Flames and Embers are additive, so they read best against darker surroundings."));
		}

		void BuildDebris(FBuilder& B, const FString& Glow, const FString& Soot);

		void BuildExplosion(FBuilder& B)
		{
			const FString Glow = B.Material(Additive);
			const FString Soot = B.Material(Smoke);
			const FString Wave = B.Material(Ring);
			double Part = 1;
			if (!B.Number(TEXT("part"), Part, 1, 2)) return;
			B.bCreates = Part == 1;
			if (Part == 2)
			{
				BuildDebris(B, Glow, Soot);
				B.Notes.Add(TEXT("Part 2 of 2: send it to the System part 1 made, with that System's revision from hyper_niagara_inspect."));
				return;
			}

			B.Single(TEXT("Flash"), 0.18, 280);
			B.ColorOverLife(TEXT("Flash"), {{0.0, Rgba(FMath::Lerp(B.Color, FVector::OneVector, 0.5) * 9.0, 1)}, {0.4, Rgba(B.Color * 6.0, 1)}, {1.0, Rgba(FVector::ZeroVector, 0)}});
			B.Renderer(TEXT("Flash"), 0, TEXT("Material"), Glow);

			B.Emitter(TEXT("Fireball"), Burst);
			B.Once(TEXT("Fireball"));
			B.Count(TEXT("Fireball"), 26);
			B.Particles(TEXT("Fireball"), 0.6, 1.1, 50, 105);
			B.Motion(TEXT("Fireball"), 150, 460, FVector(0, 0, 160), 5.0, 22);
			B.ColorOverLife(TEXT("Fireball"), Burn(B.Color, 4.5, 0.25));
			B.SizeOverLife(TEXT("Fireball"), 1.3);
			B.Renderer(TEXT("Fireball"), 0, TEXT("Material"), Glow);

			B.Single(TEXT("Shockwave"), 0.435, 60);
			B.ColorOverLife(TEXT("Shockwave"), {{0.0, Rgba(FMath::Lerp(B.Color, FVector::OneVector, 0.6) * 3.0, 1)}, {1.0, Rgba(FVector::ZeroVector, 0)}});
			B.Hlsl(TEXT("Shockwave"), Update, TEXT("ShockwaveGrow"), FString::Printf(
				TEXT("// The ring races outward, slowing as it goes.\nParticles.SpriteSize += float2(1.0, 1.0) * %s * (1.0 - Particles.NormalizedAge) * Engine.DeltaTime;"), *Num(2600 * B.Scale)));
			B.Renderer(TEXT("Shockwave"), 0, TEXT("Material"), Wave);

			B.Notes.Add(TEXT("Part 1 of 2. Then the same recipe with part=2, targetPath this System and its new revision from hyper_niagara_inspect adds the sparks and smoke."));
		}

		/** The explosion's second plan: sparks flung out and falling, and smoke that rises and thins. */
		void BuildDebris(FBuilder& B, const FString& Glow, const FString& Soot)
		{
			B.Emitter(TEXT("Sparks"), Burst);
			B.Once(TEXT("Sparks"));
			B.Count(TEXT("Sparks"), 70);
			B.Particles(TEXT("Sparks"), 0.5, 1.4, 4, 6);
			B.Motion(TEXT("Sparks"), 700, 1700, FVector(0, 0, -980), 1.3, 10);
			B.ColorOverLife(TEXT("Sparks"), Burn(B.Color, 9.0, 0.3));
			Streaks(B, TEXT("Sparks"), 3.0, 0.035);
			B.Renderer(TEXT("Sparks"), 0, TEXT("Material"), Glow);

			B.Emitter(TEXT("Smoke"), Burst);
			B.Once(TEXT("Smoke"));
			B.Count(TEXT("Smoke"), 18);
			B.Particles(TEXT("Smoke"), 1.8, 3.2, 80, 150);
			B.Motion(TEXT("Smoke"), 60, 260, FVector(0, 0, 90), 2.6, 30);
			B.ColorOverLife(TEXT("Smoke"), Thin(FVector(0.1, 0.095, 0.09), 0.55));
			B.SizeOverLife(TEXT("Smoke"), 0.6);
			B.Renderer(TEXT("Smoke"), 0, TEXT("Material"), Soot);
			B.Notes.Add(TEXT("Plays once (about 3 s): reactivate the component, or spawn it at a location, for each explosion."));
		}

		void BuildMagic(FBuilder& B)
		{
			FVector Accent = FVector(1, 0.3, 0.9);
			if (!B.Vector(TEXT("accent"), Accent, 0, 1)) return;
			const FString Glow = B.Material(Additive);
			const FString Wave = B.Material(Ring);

			B.Single(TEXT("Core"), 0.525, 180);
			B.ColorOverLife(TEXT("Core"), {{0.0, Rgba(FMath::Lerp(B.Color, FVector::OneVector, 0.3) * 4.0, 1)}, {0.5, Rgba(B.Color * 2.0, 1)}, {1.0, Rgba(FVector::ZeroVector, 0)}});
			B.SizeOverLife(TEXT("Core"), -1.2);
			B.Renderer(TEXT("Core"), 0, TEXT("Material"), Glow);

			B.Single(TEXT("Ring"), 0.625, 40);
			B.ColorOverLife(TEXT("Ring"), {{0.0, Rgba(B.Color * 4.0, 1)}, {0.5, Rgba(Accent * 2.5, 1)}, {1.0, Rgba(FVector::ZeroVector, 0)}});
			B.Hlsl(TEXT("Ring"), Update, TEXT("RingGrow"), FString::Printf(
				TEXT("// The ring opens fast, then eases.\nParticles.SpriteSize += float2(1.0, 1.0) * %s * (1.0 - Particles.NormalizedAge) * Engine.DeltaTime;"), *Num(1400 * B.Scale)));
			B.Renderer(TEXT("Ring"), 0, TEXT("Material"), Wave);

			B.Emitter(TEXT("Motes"), Burst);
			B.Once(TEXT("Motes"));
			B.Count(TEXT("Motes"), 90);
			B.Particles(TEXT("Motes"), 0.9, 1.8, 6, 14);
			B.Motion(TEXT("Motes"), 250, 650, FVector::ZeroVector, 3.2, 12);
			B.ColorOverLife(TEXT("Motes"), {{0.0, Rgba(FMath::Lerp(B.Color, FVector::OneVector, 0.3) * 5.0, 1)}, {0.45, Rgba(B.Color * 3.5, 1)},
				{0.8, Rgba(Accent * 3.0, 1)}, {1.0, Rgba(FVector::ZeroVector, 0)}});
			B.Hlsl(TEXT("Motes"), Update, TEXT("MotesSwirl"), FString::Printf(
				TEXT("// Swirl around the vertical axis and float up a little.\nfloat3 P = Particles.Position - Engine.Owner.Position;\n"
					"Particles.Velocity += (float3(-P.y, P.x, 0.0) * 3.0 + float3(0.0, 0.0, %s)) * Engine.DeltaTime;"), *Num(120 * B.Scale)));
			B.Renderer(TEXT("Motes"), 0, TEXT("Material"), Glow);
			B.Notes.Add(TEXT("Plays once (about 2 s). The motes swirl around the System's position."));
		}

		void BuildImpact(FBuilder& B)
		{
			const FString Glow = B.Material(Additive);
			const FString Soot = B.Material(Smoke);

			B.Single(TEXT("Flash"), 0.09, 90);
			B.ColorOverLife(TEXT("Flash"), {{0.0, Rgba(FMath::Lerp(B.Color, FVector::OneVector, 0.5) * 9.0, 1)}, {1.0, Rgba(FVector::ZeroVector, 0)}});
			B.Renderer(TEXT("Flash"), 0, TEXT("Material"), Glow);

			B.Emitter(TEXT("Sparks"), Burst);
			B.Once(TEXT("Sparks"));
			B.Count(TEXT("Sparks"), 40);
			B.Particles(TEXT("Sparks"), 0.3, 0.8, 3, 5);
			B.Motion(TEXT("Sparks"), 350, 1100, FVector(0, 0, -980), 2.0, 3);
			B.Hlsl(TEXT("Sparks"), Spawn, TEXT("SparksUpward"), FString::Printf(
				TEXT("// Thrown off the surface: never into it.\nParticles.Velocity.z = abs(Particles.Velocity.z) + %s;"), *Num(150 * B.Scale)));
			B.ColorOverLife(TEXT("Sparks"), Burn(B.Color, 9.0, 0.3));
			Streaks(B, TEXT("Sparks"), 2.0, 0.03);
			B.Renderer(TEXT("Sparks"), 0, TEXT("Material"), Glow);

			B.Emitter(TEXT("Dust"), Burst);
			B.Once(TEXT("Dust"));
			B.Count(TEXT("Dust"), 7);
			B.Particles(TEXT("Dust"), 0.7, 1.2, 25, 45);
			B.Motion(TEXT("Dust"), 40, 140, FVector(0, 0, 30), 3.0, 5);
			B.Hlsl(TEXT("Dust"), Spawn, TEXT("DustUpward"), TEXT("// Puffs off the surface.\nParticles.Velocity.z = abs(Particles.Velocity.z);"));
			B.ColorOverLife(TEXT("Dust"), Thin(FVector(0.32, 0.3, 0.27), 0.45));
			B.SizeOverLife(TEXT("Dust"), 0.9);
			B.Renderer(TEXT("Dust"), 0, TEXT("Material"), Soot);
			B.Notes.Add(TEXT("Plays once. The surface is the System's XY plane (+Z up): rotate the System to the hit normal."));
		}

		void BuildSmokePlume(FBuilder& B)
		{
			FVector Wind = FVector(40, 0, 0);
			if (!B.Vector(TEXT("wind"), Wind, -2000, 2000)) return;
			const FString Soot = B.Material(Smoke);
			B.Emitter(TEXT("Smoke"), Fountain);
			B.Rate(TEXT("Smoke"), 14);
			B.Particles(TEXT("Smoke"), 3.0, 4.5, 40, 80);
			B.Motion(TEXT("Smoke"), 20, 60, FVector(0, 0, 90), 0.6, 25);
			B.ColorOverLife(TEXT("Smoke"), Thin(B.Color, 0.4));
			B.SizeOverLife(TEXT("Smoke"), 0.45);
			B.Hlsl(TEXT("Smoke"), Update, TEXT("SmokeWind"), FString::Printf(
				TEXT("// Drift with the wind, gently swaying.\nParticles.Velocity += (float3(%s, %s, %s) - Particles.Velocity * float3(1.0, 1.0, 0.0)) * 0.5 * Engine.DeltaTime\n"
					"\t+ float3(sin(Particles.Age * 1.7 + Particles.Position.z * 0.01), cos(Particles.Age * 1.3), 0.0) * 15.0 * Engine.DeltaTime;"),
				*Num(Wind.X), *Num(Wind.Y), *Num(Wind.Z)));
			B.Renderer(TEXT("Smoke"), 0, TEXT("Material"), Soot);
			B.Notes.Add(TEXT("Loops forever. Smoke is alpha-blended: for dark smoke pick a darker color, for steam a lighter one."));
		}

		void BuildRibbon(FBuilder& B)
		{
			double Orbit = 80;
			if (!B.Number(TEXT("orbit_radius"), Orbit, 0, 5000)) return;
			const FString Glow = B.Material(Additive, /*bRibbon=*/true);
			B.Emitter(TEXT("Trail"), Fountain);
			B.Rate(TEXT("Trail"), 70);
			B.Float(TEXT("Trail"), Spawn, TEXT("InitializeParticle"), TEXT("Lifetime Min"), 0.7);
			B.Float(TEXT("Trail"), Spawn, TEXT("InitializeParticle"), TEXT("Lifetime Max"), 0.75);
			B.Disable(TEXT("Trail"), Update, TEXT("ScaleColor"));
			B.Disable(TEXT("Trail"), Spawn, TEXT("AddVelocity"));
			B.Disable(TEXT("Trail"), Update, TEXT("GravityForce"));
			B.Disable(TEXT("Trail"), Update, TEXT("Drag"));
			B.Float(TEXT("Trail"), Spawn, TEXT("ShapeLocation"), TEXT("Sphere Radius"), 0.1);
			// The ribbon reads Particles.RibbonWidth; a Set Parameters module makes it, and the taper below shrinks it.
			FHyperAINiagaraEditOp& Width = B.Add(TEXT("add_set_parameters"), TEXT("Trail"), Spawn);
			Width.Name = TEXT("Particles.RibbonWidth");
			Width.ValueType = TEXT("float");
			Width.Value = Num(18 * B.Scale);
			if (Orbit > 0)
			{
				B.Hlsl(TEXT("Trail"), Spawn, TEXT("TrailOrbit"), FString::Printf(
					TEXT("// Born on a circle that turns with time, so the ribbon draws a loop by itself.\nfloat T = Emitter.Age * 4.0;\n"
						"Particles.Position += float3(cos(T) * %s, sin(T) * %s, sin(T * 0.5) * %s);"), *Num(Orbit * B.Scale), *Num(Orbit * B.Scale), *Num(Orbit * 0.3 * B.Scale)));
			}
			B.Hlsl(TEXT("Trail"), Update, TEXT("TrailTaper"), FString::Printf(
				TEXT("// Thins to nothing at the tail.\nParticles.RibbonWidth = %s * (1.0 - Particles.NormalizedAge);"), *Num(18 * B.Scale)));
			B.ColorOverLife(TEXT("Trail"), {{0.0, Rgba(FMath::Lerp(B.Color, FVector::OneVector, 0.3) * 3.0, 1)}, {0.3, Rgba(B.Color * 2.2, 1)}, {1.0, Rgba(FVector::ZeroVector, 0)}});
			FHyperAINiagaraEditOp& Drop = B.Add(TEXT("remove_renderer"), TEXT("Trail"));
			Drop.RendererIndex = 0;
			FHyperAINiagaraEditOp& Ribbon = B.Add(TEXT("add_renderer"), TEXT("Trail"));
			Ribbon.AssetPath = RibbonRenderer;
			B.Renderer(TEXT("Trail"), 0, TEXT("Material"), Glow);
			B.Notes.Add(TEXT("Loops forever. Attach it to what should leave the trail; set orbit_radius 0 so it follows only that."));
		}
	}

	const TArray<FRecipeInfo>& List()
	{
		return Table();
	}

	bool Compile(const FString& Recipe, const TArray<FString>& Settings, const FString& TargetPath, TArray<FHyperAINiagaraEditOp>& OutOps,
		TArray<FString>& OutMaterials, TArray<FString>& OutNotes, FString& OutError)
	{
		OutOps.Reset();
		OutMaterials.Reset();
		OutNotes.Reset();
		OutError.Reset();
		const FRecipeInfo* Info = Table().FindByPredicate([&Recipe](const FRecipeInfo& Each) { return Each.Id == Recipe; });
		if (!Info)
		{
			TArray<FString> Ids;
			for (const FRecipeInfo& Each : Table()) Ids.Add(Each.Id);
			OutError = FString::Printf(TEXT("'%s' is not an effect recipe. Recipes: %s."), *Recipe.Left(64), *FString::Join(Ids, TEXT(", ")));
			return false;
		}
		FBuilder B{OutOps, {}, OutMaterials, OutNotes};
		TArray<FString> Keys;
		for (const FRecipeSetting& Setting : Info->Settings)
		{
			B.Settings.Add(Setting.Key, Setting.Default);
			Keys.Add(Setting.Key);
		}
		for (const FString& Setting : Settings)
		{
			FString Key;
			FString Value;
			if (!Setting.Split(TEXT("="), &Key, &Value) || !B.Settings.Contains(Key.TrimStartAndEnd().ToLower()))
			{
				OutError = FString::Printf(TEXT("'%s' is not key=value with a setting of %s: %s."), *Setting.Left(64), *Recipe, *FString::Join(Keys, TEXT(", ")));
				return false;
			}
			B.Settings[Key.TrimStartAndEnd().ToLower()] = Value.TrimStartAndEnd();
		}
		if (!B.Number(TEXT("scale"), B.Scale, 0.05, 50) || !B.Number(TEXT("intensity"), B.Intensity, 0.1, 10) || !B.Vector(TEXT("color"), B.Color, 0, 1))
		{
			OutError = B.Error;
			return false;
		}
		// The Color From Curve input that takes the curve, read from the asset so a renamed input still lines up.
		B.CurveInput = TEXT("Color Curve");
		if (const UNiagaraScript* Curve = Cast<UNiagaraScript>(FSoftObjectPath(ColorFromCurve).TryLoad()))
		{
			TArray<FHyperAINiagaraInputSchema> Inputs;
			TArray<FString> Outputs;
			FString SchemaError;
			if (ExternalEditGate::ReadScriptSchema(*Curve, Inputs, Outputs, SchemaError))
			{
				if (const FHyperAINiagaraInputSchema* Input = Inputs.FindByPredicate([](const FHyperAINiagaraInputSchema& Each) { return Each.ValueType.StartsWith(TEXT("data_interface:")); }))
				{
					B.CurveInput = Input->Name;
				}
			}
		}

		// The plan's target_path is the new System; create_system makes it empty and the emitters come from templates.
		B.Add(TEXT("create_system"));
		const int32 CreateIndex = OutOps.Num() - 1;
		if (Recipe == TEXT("fire")) BuildFire(B);
		else if (Recipe == TEXT("explosion")) BuildExplosion(B);
		else if (Recipe == TEXT("magic_burst")) BuildMagic(B);
		else if (Recipe == TEXT("impact_sparks")) BuildImpact(B);
		else if (Recipe == TEXT("smoke_plume")) BuildSmokePlume(B);
		else BuildRibbon(B);
		if (!B.Error.IsEmpty())
		{
			OutError = B.Error;
			OutOps.Reset();
			return false;
		}
		if (!B.bCreates)
		{
			OutOps.RemoveAt(CreateIndex);
		}
		if (OutOps.Num() > FHyperAIStudioNiagaraContracts::MaxOpsPerPlan)
		{
			OutError = FString::Printf(TEXT("%s compiled to %d ops, past the plan's %d."), *Recipe, OutOps.Num(), FHyperAIStudioNiagaraContracts::MaxOpsPerPlan);
			return false;
		}
		TArray<FString> Emitters;
		for (const FHyperAINiagaraEditOp& Op : OutOps)
		{
			if (Op.Kind == TEXT("add_emitter")) Emitters.Add(Op.Name);
		}
		OutNotes.Add(FString::Printf(TEXT("%d ops: the emitters %s. Every emitter runs on the CPU. hyper_niagara_preview shows the result."),
			OutOps.Num(), *FString::Join(Emitters, TEXT(", "))));
		OutNotes.Add(FString::Printf(TEXT("Send them with hyper_niagara_apply_plan and target_path %s (a path with nothing there yet)."), *TargetPath));
		return true;
	}
}
