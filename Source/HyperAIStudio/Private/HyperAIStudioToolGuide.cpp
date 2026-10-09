// Games by Hyper 2026.

#include "HyperAIStudioToolGuide.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

namespace HyperAIStudio::ToolGuide
{
	namespace
	{
		/** Field names as the MCP schema spells them: the registry standardizes case the same way. */
		FString WireName(const FProperty& Property)
		{
			return FJsonObjectConverter::StandardizeCase(Property.GetName());
		}

		const UEnum* EnumOf(const FProperty* Property)
		{
			if (const FEnumProperty* Enum = CastField<FEnumProperty>(Property)) return Enum->GetEnum();
			if (const FNumericProperty* Number = CastField<FNumericProperty>(Property)) return Number->GetIntPropertyEnum();
			return nullptr;
		}

		/** An enum's values as the wire takes them: a guessed name fails the whole call before any tool runs. */
		FString EnumValues(const UEnum& Enum)
		{
			TArray<FString> Names;
			const int32 Count = Enum.ContainsExistingMax() ? Enum.NumEnums() - 1 : Enum.NumEnums();
			for (int32 Index = 0; Index < Count; ++Index)
			{
				if (!Enum.HasMetaData(TEXT("Hidden"), Index))
				{
					Names.Add(Enum.GetNameStringByIndex(Index));
				}
			}
			return FString::Join(Names, TEXT("|"));
		}

		/** A field's type; each HyperAI struct it names is queued for the Shapes section, listed once there. */
		FString TypeName(const FProperty* Property, TArray<const UScriptStruct*>& Shapes)
		{
			if (!Property) return TEXT("value");
			if (Property->IsA<FBoolProperty>()) return TEXT("bool");
			if (const UEnum* Enum = EnumOf(Property)) return TEXT("enum(") + EnumValues(*Enum) + TEXT(")");
			if (const FNumericProperty* Number = CastField<FNumericProperty>(Property))
			{
				return Number->IsInteger() ? TEXT("int") : TEXT("number");
			}
			if (Property->IsA<FStrProperty>() || Property->IsA<FNameProperty>() || Property->IsA<FTextProperty>()) return TEXT("string");
			if (const FArrayProperty* Array = CastField<FArrayProperty>(Property)) return TEXT("[") + TypeName(Array->Inner, Shapes) + TEXT("]");
			if (const FStructProperty* Struct = CastField<FStructProperty>(Property))
			{
				if (!Struct->Struct) return TEXT("object");
				// Engine structs such as Vector keep the registry's own converters; only HyperAI's are spelled out.
				if (Struct->Struct->GetName().StartsWith(TEXT("HyperAI")))
				{
					Shapes.AddUnique(Struct->Struct);
				}
				return Struct->Struct->GetName();
			}
			if (Property->IsA<FMapProperty>()) return TEXT("map");
			if (Property->IsA<FObjectPropertyBase>()) return TEXT("object path");
			return TEXT("value");
		}

		FString DescribeFields(const UStruct& Struct, TArray<const UScriptStruct*>& Shapes)
		{
			TArray<FString> Fields;
			for (TFieldIterator<FProperty> It(&Struct); It; ++It)
			{
				Fields.Add(FString::Printf(TEXT("%s: %s"), *WireName(**It), *TypeName(*It, Shapes)));
			}
			return FString::Join(Fields, TEXT(", "));
		}

		/** One parameter: a struct request shows its top-level fields; anything else its type. */
		FString DescribeParameter(const FProperty& Parameter, TArray<const UScriptStruct*>& Shapes)
		{
			const FStructProperty* Struct = CastField<FStructProperty>(&Parameter);
			if (!Struct || !Struct->Struct)
			{
				return FString::Printf(TEXT("%s: %s"), *WireName(Parameter), *TypeName(&Parameter, Shapes));
			}
			return FString::Printf(TEXT("%s: {%s}"), *WireName(Parameter), *DescribeFields(*Struct->Struct, Shapes));
		}

		const UFunction* FindTool(const FString& QualifiedToolset, const FString& ToolName)
		{
			FString Module;
			FString ClassName;
			if (!QualifiedToolset.Split(TEXT("."), &Module, &ClassName))
			{
				return nullptr;
			}
			const UClass* Class = FindObject<UClass>(nullptr, *FString::Printf(TEXT("/Script/%s.%s"), *Module, *ClassName));
			return Class ? Class->FindFunctionByName(FName(*ToolName)) : nullptr;
		}

		FString OneLine(const FString& Text, const int32 MaxCharacters)
		{
			FString Line = Text.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" "));
			while (Line.ReplaceInline(TEXT("  "), TEXT(" ")) > 0) {}
			Line.TrimStartAndEndInline();
			return Line.Len() > MaxCharacters ? Line.Left(MaxCharacters - 3) + TEXT("...") : Line;
		}

		/** A string field, empty when absent; GetStringField would log an error for every tool without a description. */
		FString StringField(const FJsonObject& Object, const TCHAR* Name)
		{
			FString Value;
			Object.TryGetStringField(Name, Value);
			return Value;
		}

		/** A JSON-schema type in the guide's shorthand; objects spell out their fields, "?" marking the optional ones. */
		FString SchemaType(const TSharedPtr<FJsonObject>& Schema, const int32 Depth);

		FString SchemaFields(const FJsonObject& Schema, const int32 Depth)
		{
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (!Schema.TryGetObjectField(TEXT("properties"), Properties) || !Properties->IsValid())
			{
				return FString();
			}
			TSet<FString> Required;
			const TArray<TSharedPtr<FJsonValue>>* RequiredValues = nullptr;
			if (Schema.TryGetArrayField(TEXT("required"), RequiredValues))
			{
				for (const TSharedPtr<FJsonValue>& Value : *RequiredValues) Required.Add(Value->AsString());
			}
			TArray<FString> Fields;
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : (*Properties)->Values)
			{
				const TSharedPtr<FJsonObject>* FieldSchema = nullptr;
				Fields.Add(FString::Printf(TEXT("%s%s: %s"), *Field.Key, Required.Contains(Field.Key) ? TEXT("") : TEXT("?"),
					*SchemaType(Field.Value->TryGetObject(FieldSchema) ? *FieldSchema : nullptr, Depth)));
			}
			return FString::Join(Fields, TEXT(", "));
		}

		FString SchemaType(const TSharedPtr<FJsonObject>& Schema, const int32 Depth)
		{
			if (!Schema.IsValid()) return TEXT("value");
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (Schema->TryGetArrayField(TEXT("enum"), Values))
			{
				TArray<FString> Names;
				for (const TSharedPtr<FJsonValue>& Value : *Values) Names.Add(Value->AsString());
				return TEXT("enum(") + FString::Join(Names, TEXT("|")) + TEXT(")");
			}
			const FString Type = StringField(*Schema, TEXT("type"));
			if (Type == TEXT("array"))
			{
				const TSharedPtr<FJsonObject>* Items = nullptr;
				return TEXT("[") + SchemaType(Schema->TryGetObjectField(TEXT("items"), Items) ? *Items : nullptr, Depth) + TEXT("]");
			}
			// ponytail: nesting past four levels shows as "object"; describe_toolset has the rest.
			if (Type == TEXT("object") && Depth < 4 && Schema->HasField(TEXT("properties")))
			{
				return TEXT("{") + SchemaFields(*Schema, Depth + 1) + TEXT("}");
			}
			return Type == TEXT("integer") ? FString(TEXT("int")) : Type == TEXT("boolean") ? FString(TEXT("bool"))
				: Type.IsEmpty() ? FString(TEXT("value")) : Type;
		}
	}

	FString Build()
	{
		TArray<FHyperAIStudioRuntimeToolBinding> Bindings;
		TArray<FString> Diagnostics;
		FHyperAIStudioCapabilityRuntimeIndex::BuildSnapshot(Bindings, Diagnostics);
		TMap<FString, TArray<const FHyperAIStudioRuntimeToolBinding*>> ByToolset;
		int32 ToolCount = 0;
		for (const FHyperAIStudioRuntimeToolBinding& Binding : Bindings)
		{
			if (Binding.bToolEnabled)
			{
				ByToolset.FindOrAdd(Binding.QualifiedToolset).Add(&Binding);
				++ToolCount;
			}
		}
		ByToolset.KeySort([](const FString& A, const FString& B) { return A < B; });

		FString Text;
		Text += TEXT("# HyperAIStudio Tool Guide\n\n");
		Text += FString::Printf(TEXT("Generated by HyperAIStudio from the tools registered in this editor: %d tools in %d toolsets. ")
			TEXT("It is rewritten whenever the tools change, so do not edit it.\n\n"), ToolCount, ByToolset.Num());
		Text += TEXT("## How to call a tool\n\n");
		Text += TEXT("- Call every tool through `call_tool` with `toolset_name` (a heading below), `tool_name`, and `arguments` shaped as listed. ")
			TEXT("Without `toolset_name` a tool is \"not found\", `hyper_operation_wait` included. Nothing listed here needs `list_toolsets` first.\n");
		Text += TEXT("- Top-level fields are listed with each tool; the structs they name are spelled out once under Shapes at the end. ")
			TEXT("An enum takes exactly one of the names listed: any other name fails the call before the tool runs.\n");
		Text += TEXT("- To edit, call the pack's `*_apply_plan` with `bDryRun: false` and a fresh `operationId`: it plans, applies and waits for the edit ")
			TEXT("in one call (`expectedPlanHash` is optional), and replies `completed` or why not. Then call the pack's validate tool. ")
			TEXT("Only a reply still `queued`, `running` or `awaiting_user_approval` needs `hyper_operation_wait` with that `operationId`. ")
			TEXT("When the user has turned on auto-approval for that kind of request, it is approved without a click, and `hyper_operation_status` says `approvedBy: auto_approval`. ")
			TEXT("Dry-run first only when you want to review a plan before it runs.\n");
		Text += TEXT("- A limit out of range, or 0 for one you do not care about, is fitted rather than refused, and the reply says what was used.\n");
		Text += TEXT("- Epic's own toolsets, whose names do not start with `HyperAIStudio`, are in `Epic-ToolGuide.md` beside this file with every field: ")
			TEXT("grep it for the tool you need instead of calling `list_toolsets` or `describe_toolset`.\n");
		TArray<const UScriptStruct*> Shapes;
		for (const TPair<FString, TArray<const FHyperAIStudioRuntimeToolBinding*>>& Toolset : ByToolset)
		{
			Text += FString::Printf(TEXT("\n## `%s`\n\n"), *Toolset.Key);
			for (const FHyperAIStudioRuntimeToolBinding* Binding : Toolset.Value)
			{
				TArray<FString> Parameters;
				if (const UFunction* Function = FindTool(Binding->QualifiedToolset, Binding->ToolName))
				{
					for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
					{
						if (!It->HasAnyPropertyFlags(CPF_ReturnParm))
						{
							Parameters.Add(DescribeParameter(**It, Shapes));
						}
					}
				}
				Text += FString::Printf(TEXT("- `%s` `{%s}`: %s\n"), *Binding->ToolName, *FString::Join(Parameters, TEXT(", ")),
					*OneLine(Binding->Description, 240));
			}
		}
		if (!Shapes.IsEmpty())
		{
			Text += TEXT("\n## Shapes\n\n");
			// Describing a shape can queue more, so the list grows while it is walked.
			for (int32 Index = 0; Index < Shapes.Num(); ++Index)
			{
				const FString Fields = DescribeFields(*Shapes[Index], Shapes);
				Text += FString::Printf(TEXT("- `%s` `{%s}`\n"), *Shapes[Index]->GetName(), *Fields);
			}
		}
		return Text;
	}

	FString BuildEpic()
	{
		TArray<TSharedPtr<FJsonValue>> Toolsets;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(UToolsetRegistry::GetAllToolsetJsonSchemas()), Toolsets))
		{
			return FString();
		}
		TArray<TSharedPtr<FJsonObject>> Epic;
		for (const TSharedPtr<FJsonValue>& Value : Toolsets)
		{
			const TSharedPtr<FJsonObject>* Toolset = nullptr;
			if (Value->TryGetObject(Toolset) && !StringField(**Toolset, TEXT("name")).StartsWith(TEXT("HyperAIStudio")))
			{
				Epic.Add(*Toolset);
			}
		}
		Epic.Sort([](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B)
		{
			return StringField(*A, TEXT("name")) < StringField(*B, TEXT("name"));
		});

		FString Text;
		Text += TEXT("# Epic Toolsets\n\n");
		Text += FString::Printf(TEXT("Generated by HyperAIStudio from the schemas Unreal's toolset registry serves: %d toolsets. ")
			TEXT("It is rewritten whenever they change, so do not edit it.\n\n"), Epic.Num());
		Text += TEXT("Grep for a tool name. Call it through `call_tool` with its `## ` heading as `toolset_name`, `tool_name`, and `arguments` ")
			TEXT("shaped as listed: a field marked `?` is optional, every other one is required, and an enum takes exactly one of the names listed.\n");
		for (const TSharedPtr<FJsonObject>& Toolset : Epic)
		{
			Text += FString::Printf(TEXT("\n## `%s`\n\n"), *StringField(*Toolset, TEXT("name")));
			const FString Description = OneLine(StringField(*Toolset, TEXT("description")), 300);
			if (!Description.IsEmpty())
			{
				Text += Description + TEXT("\n\n");
			}
			const TArray<TSharedPtr<FJsonValue>>* Tools = nullptr;
			if (!Toolset->TryGetArrayField(TEXT("tools"), Tools))
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& ToolValue : *Tools)
			{
				const TSharedPtr<FJsonObject>* Tool = nullptr;
				if (!ToolValue->TryGetObject(Tool))
				{
					continue;
				}
				FString Name = StringField(**Tool, TEXT("name"));
				int32 Dot = INDEX_NONE;
				if (Name.FindLastChar(TEXT('.'), Dot))
				{
					Name.RightChopInline(Dot + 1);
				}
				const TSharedPtr<FJsonObject>* Input = nullptr;
				const FString Fields = (*Tool)->TryGetObjectField(TEXT("inputSchema"), Input) ? SchemaFields(**Input, 0) : FString();
				Text += FString::Printf(TEXT("- `%s` `{%s}`: %s\n"), *Name, *Fields, *OneLine(StringField(**Tool, TEXT("description")), 240));
			}
		}
		return Text;
	}

	FString GetPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".hyperai/docs/HyperAIStudio-ToolGuide.md"));
	}

	FString GetEpicPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT(".hyperai/docs/Epic-ToolGuide.md"));
	}

	bool Ensure()
	{
		auto Write = [](const FString& Path, const FString& Text)
		{
			FString Existing;
			if (FFileHelper::LoadFileToString(Existing, *Path) && Existing == Text)
			{
				return true;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
			return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		};
		const FString Epic = BuildEpic();
		return Write(GetPath(), Build()) && (Epic.IsEmpty() || Write(GetEpicPath(), Epic));
	}
}
