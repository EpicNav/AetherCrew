// Games by Hyper 2026.

#include "HyperAIStudioNiagaraDiscover.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "HyperAIStudioNiagaraExternalEditGate.h"
#include "HyperAIStudioNiagaraRecipes.h"
#include "NiagaraCommon.h"
#include "NiagaraDataInterface.h"
#include "NiagaraEditorUtilities.h"
#include "NiagaraEmitter.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::Niagara::Discover
{
	namespace
	{
		constexpr int32 MaxDescriptionChars = 300;
		constexpr int32 MaxPropertyDefaultChars = 200;
		const TCHAR* const TemplatesRoot = TEXT("/Niagara/DefaultAssets/Templates");

		FString Tag(const FAssetData& Asset, const TCHAR* Name)
		{
			const FAssetTagValueRef Value = Asset.TagsAndValues.FindTag(FName(Name));
			return Value.IsSet() ? Value.AsText().ToString() : FString();
		}

		FString Clip(const FString& Text, int32 MaxChars = MaxDescriptionChars)
		{
			FString Flat = Text.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" "));
			return Flat.Len() > MaxChars ? Flat.Left(MaxChars - 3) + TEXT("...") : Flat;
		}

		FHyperAINiagaraDiscoverReport Fail(const TCHAR* Status, const FString& Diagnostic)
		{
			FHyperAINiagaraDiscoverReport Report;
			Report.Status = Status;
			Report.Diagnostic = Diagnostic;
			return Report;
		}

		bool Add(FHyperAINiagaraDiscoverReport& Report, const FHyperAINiagaraDiscoverRequest& Request, FHyperAINiagaraDiscoverEntry&& Entry)
		{
			if (!Matches(Request.Query, Entry.Path + TEXT(" ") + Entry.Description))
			{
				return true;
			}
			if (Report.Entries.Num() >= Request.MaxResults)
			{
				Report.bTruncated = true;
				return false;
			}
			Report.Entries.Add(MoveTemp(Entry));
			return true;
		}

		void ListTemplates(const FHyperAINiagaraDiscoverRequest& Request, FHyperAINiagaraDiscoverReport& Report)
		{
			IAssetRegistry& Registry = IAssetRegistry::GetChecked();
			FARFilter Filter;
			Filter.PackagePaths.Add(TemplatesRoot);
			Filter.bRecursivePaths = true;
			Filter.ClassPaths.Add(UNiagaraEmitter::StaticClass()->GetClassPathName());
			Filter.ClassPaths.Add(UNiagaraSystem::StaticClass()->GetClassPathName());
			TArray<FAssetData> Assets;
			Registry.GetAssets(Filter, Assets);
			Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetObjectPathString() < B.GetObjectPathString(); });
			for (const FAssetData& Asset : Assets)
			{
				FHyperAINiagaraDiscoverEntry Entry;
				Entry.Path = Asset.GetObjectPathString();
				const bool bSystem = Asset.AssetClassPath == UNiagaraSystem::StaticClass()->GetClassPathName();
				Entry.Kind = bSystem ? TEXT("system_template") : TEXT("emitter_template");
				Entry.Description = Clip(Tag(Asset, TEXT("TemplateAssetDescription")));
				if (Entry.Path.Contains(TEXT("/CascadeConversion/")) || Entry.Path.Contains(TEXT("Lightweight")))
				{
					// Cascade scaffolding and stateless emitters; the edit ops do not address lightweight emitters.
					continue;
				}
				if (!Add(Report, Request, MoveTemp(Entry)))
				{
					break;
				}
			}
			Report.Notes.Add(TEXT("Start a System with a create_system op (asset_path a system_template, value the emitters to keep), or add_emitter an emitter_template into one."));
		}

		void ListScripts(const FHyperAINiagaraDiscoverRequest& Request, FHyperAINiagaraDiscoverReport& Report, ENiagaraScriptUsage Usage)
		{
			FNiagaraEditorUtilities::FGetFilteredScriptAssetsOptions Options;
			Options.ScriptUsageToInclude = Usage;
			Options.bIncludeNonLibraryScripts = false;
			TArray<FAssetData> Assets;
			FNiagaraEditorUtilities::GetFilteredScriptAssets(Options, Assets);
			Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.GetObjectPathString() < B.GetObjectPathString(); });
			for (const FAssetData& Asset : Assets)
			{
				FHyperAINiagaraDiscoverEntry Entry;
				Entry.Path = Asset.GetObjectPathString();
				Entry.Kind = Usage == ENiagaraScriptUsage::DynamicInput ? TEXT("dynamic_input") : TEXT("module");
				const FString Category = Tag(Asset, TEXT("Category"));
				Entry.Description = Clip((Category.IsEmpty() ? FString() : Category + TEXT(": ")) + Tag(Asset, TEXT("Description")));
				if (!Add(Report, Request, MoveTemp(Entry)))
				{
					break;
				}
			}
		}

		FString PropertyDefault(const FProperty& Property, const UObject& Defaults)
		{
			FString Text;
			Property.ExportTextItem_InContainer(Text, &Defaults, nullptr, nullptr, PPF_None);
			return Clip(Text, MaxPropertyDefaultChars);
		}

		/** Editable properties of a renderer or data interface class; attribute bindings report the attribute they read. */
		void ListProperties(const UClass& Class, const FHyperAINiagaraDiscoverRequest& Request, FHyperAINiagaraDiscoverReport& Report)
		{
			const UObject* Defaults = Class.GetDefaultObject();
			for (TFieldIterator<FProperty> It(&Class); It; ++It)
			{
				const FProperty& Property = **It;
				if (!Property.HasAnyPropertyFlags(CPF_Edit) || Property.HasAnyPropertyFlags(CPF_Deprecated | CPF_EditConst))
				{
					continue;
				}
				FHyperAINiagaraPropertyInfo Info;
				Info.Name = Property.GetName();
				const FStructProperty* Struct = CastField<FStructProperty>(&Property);
				if (Struct && Struct->Struct == FNiagaraVariableAttributeBinding::StaticStruct())
				{
					Info.Type = TEXT("binding");
					const FNiagaraVariableAttributeBinding* Binding = Struct->ContainerPtrToValuePtr<FNiagaraVariableAttributeBinding>(Defaults);
					Info.Default = Binding ? Binding->GetParamMapBindableVariable().GetName().ToString() : FString();
				}
				else
				{
					Info.Type = Property.GetCPPType();
					Info.Default = PropertyDefault(Property, *Defaults);
				}
				Info.Description = Clip(Property.GetToolTipText().ToString());
				if (!Matches(Request.Query, Info.Name + TEXT(" ") + Info.Description))
				{
					continue;
				}
				if (Report.Properties.Num() >= Request.MaxResults)
				{
					Report.bTruncated = true;
					break;
				}
				Report.Properties.Add(MoveTemp(Info));
			}
		}

		UClass* ResolveClass(const FString& Path, UClass* Base)
		{
			UClass* Class = FSoftClassPath(Path).TryLoadClass<UObject>();
			if (!Class)
			{
				// A short name: SpriteRenderer, NiagaraSpriteRendererProperties, NiagaraDataInterfaceCurve.
				for (TObjectIterator<UClass> It; It; ++It)
				{
					if (It->IsChildOf(Base) && (It->GetName() == Path || It->GetName() == TEXT("Niagara") + Path + TEXT("Properties")
						|| It->GetName() == TEXT("NiagaraDataInterface") + Path))
					{
						Class = *It;
						break;
					}
				}
			}
			return Class && Class->IsChildOf(Base) && !Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated) ? Class : nullptr;
		}
	}

	bool Matches(const FString& Query, const FString& Text)
	{
		TArray<FString> Words;
		Query.ParseIntoArrayWS(Words);
		for (const FString& Word : Words)
		{
			if (!Text.Contains(Word, ESearchCase::IgnoreCase))
			{
				return false;
			}
		}
		return true;
	}

	FHyperAINiagaraDiscoverReport Run(const FHyperAINiagaraDiscoverRequest& Request)
	{
		if (!IsInGameThread())
		{
			return Fail(TEXT("game_thread_required"), TEXT("Discovery reads editor state on the game thread."));
		}
		const FString Kind = Request.Kind.TrimStartAndEnd().ToLower();
		FHyperAINiagaraDiscoverReport Report;
		Report.Status = TEXT("completed");
		if (Kind == TEXT("templates"))
		{
			ListTemplates(Request, Report);
		}
		else if (Kind == TEXT("modules"))
		{
			ListScripts(Request, Report, ENiagaraScriptUsage::Module);
			Report.Notes.Add(TEXT("add_module takes the path; kind script_schema with asset_path lists its inputs and the value_type each takes."));
		}
		else if (Kind == TEXT("dynamic_inputs"))
		{
			if (Request.ValueType.IsEmpty())
			{
				ListScripts(Request, Report, ENiagaraScriptUsage::DynamicInput);
			}
			else
			{
				TArray<FString> Paths;
				FString Error;
				if (!ExternalEditGate::ListDynamicInputs(Request.ValueType.TrimStartAndEnd().ToLower(), Paths, Error))
				{
					return Fail(TEXT("invalid_value_type"), Error);
				}
				for (const FString& Path : Paths)
				{
					FHyperAINiagaraDiscoverEntry Entry;
					Entry.Path = Path;
					Entry.Kind = TEXT("dynamic_input");
					if (!Add(Report, Request, MoveTemp(Entry)))
					{
						break;
					}
				}
			}
			Report.Notes.Add(TEXT("set_input_value with value_type dynamic_input and asset_path sets one; then set its own inputs with input_name_stack [input, nested input]."));
		}
		else if (Kind == TEXT("script_schema"))
		{
			const UNiagaraScript* Script = Cast<UNiagaraScript>(FSoftObjectPath(Request.AssetPath).TryLoad());
			if (!Script)
			{
				return Fail(TEXT("script_not_found"), FString::Printf(TEXT("No Niagara module or dynamic input script at '%s'."), *Request.AssetPath.Left(256)));
			}
			FString Error;
			if (!ExternalEditGate::ReadScriptSchema(*Script, Report.Inputs, Report.Outputs, Error))
			{
				return Fail(TEXT("schema_unavailable"), Error);
			}
			FHyperAINiagaraDiscoverEntry Entry;
			Entry.Path = Script->GetPathName();
			Entry.Kind = Script->GetUsage() == ENiagaraScriptUsage::DynamicInput ? TEXT("dynamic_input") : TEXT("module");
			if (const FVersionedNiagaraScriptData* Data = Script->GetLatestScriptData())
			{
				Entry.Description = Clip(Data->Description.ToString(), 600);
			}
			Report.Entries.Add(MoveTemp(Entry));
		}
		else if (Kind == TEXT("renderers"))
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				if (It->IsChildOf(UNiagaraRendererProperties::StaticClass()) && !It->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)
					&& !It->GetName().StartsWith(TEXT("SKEL_")) && !It->GetName().StartsWith(TEXT("REINST_")))
				{
					FHyperAINiagaraDiscoverEntry Entry;
					Entry.Path = It->GetPathName();
					Entry.Kind = TEXT("renderer");
					Entry.Description = Clip(It->GetToolTipText().ToString());
					Add(Report, Request, MoveTemp(Entry));
				}
			}
			Report.Entries.Sort([](const FHyperAINiagaraDiscoverEntry& A, const FHyperAINiagaraDiscoverEntry& B) { return A.Path < B.Path; });
		}
		else if (Kind == TEXT("renderer_schema") || Kind == TEXT("data_interface_schema"))
		{
			const bool bRenderer = Kind == TEXT("renderer_schema");
			UClass* Class = ResolveClass(Request.AssetPath.TrimStartAndEnd(), bRenderer ? UNiagaraRendererProperties::StaticClass() : UNiagaraDataInterface::StaticClass());
			if (!Class)
			{
				return Fail(TEXT("class_not_found"), FString::Printf(TEXT("'%s' is not a concrete %s class; give a class path such as %s."), *Request.AssetPath.Left(256),
					bRenderer ? TEXT("Niagara renderer") : TEXT("Niagara data interface"),
					bRenderer ? TEXT("/Script/Niagara.NiagaraSpriteRendererProperties") : TEXT("/Script/Niagara.NiagaraDataInterfaceCurve")));
			}
			ListProperties(*Class, Request, Report);
			Report.Notes.Add(bRenderer
				? TEXT("set_renderer_property writes a value; set_renderer_binding (name = a binding property, value = an attribute such as Particles.Color) rebinds a binding.")
				: TEXT("A data_interface input value is a JSON object of these properties; curves take the curve, color_curve and vector_curve shorthands."));
		}
		else if (Kind == TEXT("recipes"))
		{
			for (const Recipes::FRecipeInfo& Info : Recipes::List())
			{
				FHyperAINiagaraDiscoverEntry Entry;
				Entry.Path = Info.Id;
				Entry.Kind = TEXT("recipe");
				Entry.Description = Info.Summary;
				if (Add(Report, Request, MoveTemp(Entry)) && Request.Query.IsEmpty())
				{
					for (const Recipes::FRecipeSetting& Setting : Info.Settings)
					{
						FHyperAINiagaraPropertyInfo Property;
						Property.Name = Info.Id + TEXT(".") + Setting.Key;
						Property.Type = TEXT("setting");
						Property.Default = Setting.Default;
						Property.Description = Setting.Help;
						Report.Properties.Add(MoveTemp(Property));
					}
				}
			}
			Report.Notes.Add(TEXT("hyper_niagara_apply_plan with recipe, recipe_settings and a new target_path builds one; kind recipe shows its ops first."));
		}
		else if (Kind == TEXT("recipe"))
		{
			FString Error;
			if (!Recipes::Compile(Request.Recipe.TrimStartAndEnd().ToLower(), Request.RecipeSettings,
				Request.TargetPath.IsEmpty() ? FString(TEXT("/Game/VFX/NS_Recipe.NS_Recipe")) : Request.TargetPath,
				Report.Ops, Report.Materials, Report.Notes, Error))
			{
				return Fail(TEXT("invalid_recipe"), Error);
			}
		}
		else
		{
			return Fail(TEXT("invalid_kind"), TEXT("kind must be templates, modules, dynamic_inputs, script_schema, renderers, renderer_schema, data_interface_schema, recipes or recipe."));
		}
		Report.bOk = true;
		Report.Diagnostic = FString::Printf(TEXT("%d entr%s, %d input%s, %d propert%s, %d op%s%s."),
			Report.Entries.Num(), Report.Entries.Num() == 1 ? TEXT("y") : TEXT("ies"),
			Report.Inputs.Num(), Report.Inputs.Num() == 1 ? TEXT("") : TEXT("s"),
			Report.Properties.Num(), Report.Properties.Num() == 1 ? TEXT("y") : TEXT("ies"),
			Report.Ops.Num(), Report.Ops.Num() == 1 ? TEXT("") : TEXT("s"),
			Report.bTruncated ? TEXT("; more matched: narrow query or raise max_results") : TEXT(""));
		return Report;
	}
}
