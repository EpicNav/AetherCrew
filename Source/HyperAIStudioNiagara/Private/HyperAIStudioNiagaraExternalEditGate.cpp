// Games by Hyper 2026.

// The only translation unit in the plugin that includes NiagaraExternalSystemEditorUtilities.h. Epic marks that
// header "EXPERIMENTAL! DO NOT USE EXCEPT FOR TESTING!", and its one in-engine caller is Epic's own NiagaraToolsets
// plugin (NiagaraToolset_System.cpp). Keep every FNiagaraExt_* type inside this file.

#include "HyperAIStudioNiagaraExternalEditGate.h"

#include "Misc/EngineVersionComparison.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "NiagaraEmitter.h"
#include "NiagaraEmitterHandle.h"
#include "NiagaraExternalSystemEditorUtilities.h"
#include "NiagaraRendererProperties.h"
#include "NiagaraScript.h"
#include "NiagaraSystem.h"
#include "NiagaraTypes.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HyperAIStudioNiagaraAssetsGate.h"
#include "HyperAIStudioNiagaraGraphGate.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/SoftObjectPath.h"

#include <type_traits>

#define LOCTEXT_NAMESPACE "HyperAIStudioNiagaraExternalEditGate"

static_assert(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8,
	"The Niagara external-edit gate is pinned to the audited UE 5.8 UNiagaraExternalEditUtilities API. "
	"Re-audit NiagaraExternalSystemEditorUtilities.h before raising this pin.");

// A silent signature change in the experimental API becomes a compile error here instead of a runtime surprise.
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::GetSystemSummary),
	void (*)(UNiagaraSystem*, FNiagaraExt_SystemSummary&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::GetEmitterTopology),
	void (*)(const FNiagaraExt_StackItemReference&, FNiagaraExt_EmitterTopology&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::GetSystemCompileState),
	void (*)(UNiagaraSystem*, FNiagaraExt_SystemCompileState&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::GetStackIssues),
	void (*)(UNiagaraSystem*, FNiagaraExt_StackIssues&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::SetModuleEnabled),
	void (*)(const FNiagaraExt_StackItemReference&, bool, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::AddModule),
	void (*)(const FNiagaraExt_StackItemReference&, const UNiagaraScript*, FNiagaraExt_ModuleTopology&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::AddRenderer),
	void (*)(const FNiagaraExt_StackItemReference&, const TSubclassOf<UNiagaraRendererProperties>, FNiagaraExt_RendererRef&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::AddEmitter),
	void (*)(UNiagaraEmitter*, FName, FNiagaraExt_EmitterTopology&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::SetStackInputData),
	void (*)(const FNiagaraExt_StackItemReference&, const FNiagaraExt_StackInputValue&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::ApplyStackIssueFix),
	void (*)(UNiagaraSystem*, const FString&, const FString&, FNiagaraExt_ApplyStackIssueFixResult&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::GetStackInputTopology),
	void (*)(const FNiagaraExt_StackItemReference&, FNiagaraExt_StackInputTopology&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::GetModuleInputValues),
	void (*)(const FNiagaraExt_StackItemReference&, FNiagaraExt_ModuleInputValues&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::GetDynamicInputChain),
	void (*)(const FNiagaraExt_StackItemReference&, FNiagaraExt_DynamicInputChainRef&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::CreateNiagaraSystem),
	UNiagaraSystem* (*)(const FString&, const FString&, UNiagaraSystem*, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::RemoveEmitter),
	void (*)(const FNiagaraExt_StackItemReference&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::RemoveModule),
	void (*)(const FNiagaraExt_StackItemReference&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::RemoveRenderer),
	void (*)(const FNiagaraExt_StackItemReference&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::AddUserVariable),
	void (*)(UNiagaraSystem*, const FNiagaraExt_UserVariable&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::RemoveUserVariable),
	void (*)(UNiagaraSystem*, const FNiagaraExt_Variable&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::AddSetParametersModule),
	void (*)(const FNiagaraExt_StackItemReference&, const TArray<FNiagaraExt_SetParameterEntry>&, FNiagaraExt_ModuleTopology&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::AddSetParameterEntry),
	void (*)(const FNiagaraExt_StackItemReference&, const FNiagaraExt_SetParameterEntry&, FNiagaraExt_ModuleTopology&, FNiagaraExternalEditContext&)>);
static_assert(std::is_same_v<decltype(&UNiagaraExternalEditUtilities::GetAvailableDynamicInputs),
	void (*)(const FNiagaraTypeDefinition&, TArray<UNiagaraScript*>&, FNiagaraExternalEditContext&)>);

namespace HyperAIStudio::Niagara::ExternalEditGate
{
	namespace
	{
		constexpr int32 MaxNameChars = 128;
		constexpr int32 MaxPathChars = 512;
		constexpr int32 MaxIdChars = 256;
		constexpr int32 MaxSummaryChars = 512;
		constexpr int32 MaxDescriptionChars = 1024;

		const TSet<FString>& ScriptNames()
		{
			static const TSet<FString> Names = {
				TEXT("SystemSpawnScript"), TEXT("SystemUpdateScript"),
				TEXT("EmitterSpawnScript"), TEXT("EmitterUpdateScript"),
				TEXT("ParticleSpawnScript"), TEXT("ParticleUpdateScript")};
			return Names;
		}

		bool IsSystemScript(const FString& ScriptName)
		{
			return ScriptName.StartsWith(TEXT("System"), ESearchCase::CaseSensitive);
		}

		bool IsBoundedText(const FString& Value, int32 MaxChars)
		{
			if (Value.Len() > MaxChars)
			{
				return false;
			}
			for (const TCHAR Char : Value)
			{
				if (Char < TEXT(' ') || Char == 0x7F)
				{
					return false;
				}
			}
			return true;
		}

		FName ToName(const FString& Value)
		{
			return Value.IsEmpty() ? NAME_None : FName(*Value);
		}

		FString JoinErrors(const FNiagaraExternalEditContext& Context, int32 FromIndex)
		{
			FString Joined;
			for (int32 Index = FromIndex; Index < Context.Errors.Num(); ++Index)
			{
				if (!Joined.IsEmpty())
				{
					Joined += TEXT(" | ");
				}
				Joined += Context.Errors[Index].ToString();
			}
			return Joined.Left(MaxDescriptionChars);
		}

		bool ParseFloats(const FString& Text, int32 Count, float* Out)
		{
			TArray<FString> Parts;
			Text.ParseIntoArray(Parts, TEXT(","), false);
			if (Parts.Num() != Count)
			{
				return false;
			}
			for (int32 Index = 0; Index < Count; ++Index)
			{
				const FString Part = Parts[Index].TrimStartAndEnd();
				if (Part.IsEmpty() || !Part.IsNumeric())
				{
					return false;
				}
				Out[Index] = FCString::Atof(*Part);
			}
			return true;
		}

		/** Local values ride in the instanced struct as the plain Niagara or core type the input already uses. */
		bool BuildInputValue(const FString& ValueType, const FString& Text, FInstancedStruct& OutValue)
		{
			const FString Trimmed = Text.TrimStartAndEnd();
			if (ValueType == TEXT("vector2"))
			{
				FVector2f Value;
				if (!ParseFloats(Trimmed, 2, &Value.X))
				{
					return false;
				}
				OutValue.InitializeAs(TVariantStructure<FVector2f>::Get(), reinterpret_cast<const uint8*>(&Value));
				return true;
			}
			if (ValueType == TEXT("vector4"))
			{
				FVector4f Value;
				if (!ParseFloats(Trimmed, 4, &Value.X))
				{
					return false;
				}
				OutValue.InitializeAs(TVariantStructure<FVector4f>::Get(), reinterpret_cast<const uint8*>(&Value));
				return true;
			}
			if (ValueType == TEXT("position"))
			{
				FNiagaraPosition Value;
				if (!ParseFloats(Trimmed, 3, &Value.X))
				{
					return false;
				}
				const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/Niagara.NiagaraPosition"));
				OutValue.InitializeAs(Struct, reinterpret_cast<const uint8*>(&Value));
				return Struct != nullptr;
			}
			if (ValueType == TEXT("float"))
			{
				FNiagaraFloat Value;
				if (!ParseFloats(Trimmed, 1, &Value.Value))
				{
					return false;
				}
				const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/Niagara.NiagaraFloat"));
				OutValue.InitializeAs(Struct, reinterpret_cast<const uint8*>(&Value));
				return Struct != nullptr;
			}
			if (ValueType == TEXT("int32"))
			{
				if (Trimmed.IsEmpty() || !Trimmed.IsNumeric() || Trimmed.Contains(TEXT(".")))
				{
					return false;
				}
				FNiagaraInt32 Value;
				Value.Value = FCString::Atoi(*Trimmed);
				const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/Niagara.NiagaraInt32"));
				OutValue.InitializeAs(Struct, reinterpret_cast<const uint8*>(&Value));
				return Struct != nullptr;
			}
			if (ValueType == TEXT("bool"))
			{
				if (Trimmed != TEXT("true") && Trimmed != TEXT("false"))
				{
					return false;
				}
				FNiagaraBool Value;
				Value.SetValue(Trimmed == TEXT("true"));
				const UScriptStruct* Struct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/Niagara.NiagaraBool"));
				OutValue.InitializeAs(Struct, reinterpret_cast<const uint8*>(&Value));
				return Struct != nullptr;
			}
			if (ValueType == TEXT("vector"))
			{
				FVector3f Value;
				if (!ParseFloats(Trimmed, 3, &Value.X))
				{
					return false;
				}
				OutValue.InitializeAs(TVariantStructure<FVector3f>::Get(), reinterpret_cast<const uint8*>(&Value));
				return true;
			}
			if (ValueType == TEXT("color"))
			{
				FLinearColor Value;
				if (!ParseFloats(Trimmed, 4, &Value.R))
				{
					return false;
				}
				OutValue.InitializeAs(TBaseStructure<FLinearColor>::Get(), reinterpret_cast<const uint8*>(&Value));
				return true;
			}
			return false;
		}

		bool IsLocalValueType(const FString& ValueType)
		{
			static const TSet<FString> Types = {TEXT("float"), TEXT("int32"), TEXT("bool"), TEXT("vector2"), TEXT("vector"),
				TEXT("vector4"), TEXT("position"), TEXT("color")};
			return Types.Contains(ValueType);
		}

		bool IsCurveValueType(const FString& ValueType)
		{
			return ValueType == TEXT("curve") || ValueType == TEXT("color_curve") || ValueType == TEXT("vector_curve");
		}

		/** The Niagara type an op's value_type names, for user parameters and Set Parameters entries. */
		FNiagaraTypeDefinition TypeOfValueType(const FString& ValueType)
		{
			if (ValueType == TEXT("float")) return FNiagaraTypeDefinition::GetFloatDef();
			if (ValueType == TEXT("int32")) return FNiagaraTypeDefinition::GetIntDef();
			if (ValueType == TEXT("bool")) return FNiagaraTypeDefinition::GetBoolDef();
			if (ValueType == TEXT("vector2")) return FNiagaraTypeDefinition::GetVec2Def();
			if (ValueType == TEXT("vector")) return FNiagaraTypeDefinition::GetVec3Def();
			if (ValueType == TEXT("vector4")) return FNiagaraTypeDefinition::GetVec4Def();
			if (ValueType == TEXT("position")) return FNiagaraTypeDefinition::GetPositionDef();
			if (ValueType == TEXT("color")) return FNiagaraTypeDefinition::GetColorDef();
			return FNiagaraTypeDefinition();
		}

		/** Values that are text, JSON or curve keys may run longer than a path and span lines. */
		bool IsLongValueType(const FString& ValueType)
		{
			return ValueType == TEXT("hlsl_expression") || ValueType == TEXT("data_interface") || IsCurveValueType(ValueType);
		}

		bool IsBoundedLongText(const FString& Value, int32 MaxChars)
		{
			if (Value.Len() > MaxChars)
			{
				return false;
			}
			for (const TCHAR Char : Value)
			{
				if ((Char < TEXT(' ') && Char != TEXT('\n') && Char != TEXT('\r') && Char != TEXT('\t')) || Char == 0x7F)
				{
					return false;
				}
			}
			return true;
		}

		/** Particles.NormalizedAge, Emitter.Age, User.Tint, Engine.DeltaTime: dotted names of letters, digits, _ and spaces. */
		bool IsParameterName(const FString& Value)
		{
			if (Value.IsEmpty() || Value.Len() > MaxNameChars || !Value.Contains(TEXT(".")) || Value.StartsWith(TEXT("."))
				|| Value.EndsWith(TEXT(".")) || Value.Contains(TEXT("..")))
			{
				return false;
			}
			for (const TCHAR Char : Value)
			{
				if (!FChar::IsAlnum(Char) && Char != TEXT('_') && Char != TEXT('.') && Char != TEXT(' '))
				{
					return false;
				}
			}
			return true;
		}

		bool HasBalancedBrackets(const FString& Text)
		{
			TArray<TCHAR> Open;
			for (const TCHAR Char : Text)
			{
				if (Char == TEXT('(') || Char == TEXT('['))
				{
					Open.Add(Char);
				}
				else if (Char == TEXT(')') || Char == TEXT(']'))
				{
					if (Open.IsEmpty() || Open.Pop() != (Char == TEXT(')') ? TEXT('(') : TEXT('[')))
					{
						return false;
					}
				}
			}
			return Open.IsEmpty();
		}

		/**
		 * Curve keys as "time:value; time:value" (curve), "time:r,g,b,a" (color_curve) or "time:x,y,z" (vector_curve),
		 * written as the JSON the data interface's property provider reads: one FRichCurve per channel, linear keys.
		 */
		bool BuildCurveJson(const FString& ValueType, const FString& Text, FString& OutJson, FString& OutError)
		{
			const int32 Channels = ValueType == TEXT("curve") ? 1 : ValueType == TEXT("color_curve") ? 4 : 3;
			static const TCHAR* const ColorNames[] = {TEXT("RedCurve"), TEXT("GreenCurve"), TEXT("BlueCurve"), TEXT("AlphaCurve")};
			static const TCHAR* const VectorNames[] = {TEXT("XCurve"), TEXT("YCurve"), TEXT("ZCurve")};
			TArray<FString> Keys;
			Text.ParseIntoArray(Keys, Channels == 1 && !Text.Contains(TEXT(";")) ? TEXT(",") : TEXT(";"));
			if (Keys.IsEmpty() || Keys.Num() > 64)
			{
				OutError = TEXT("A curve needs 1-64 keys.");
				return false;
			}
			TArray<TArray<FString>> ChannelKeys;
			ChannelKeys.SetNum(Channels);
			float LastTime = -MAX_flt;
			for (const FString& Key : Keys)
			{
				FString TimeText;
				FString ValuesText;
				float Time = 0.f;
				float Values[4] = {0.f, 0.f, 0.f, 1.f};
				const int32 Given = Channels;
				if (!Key.TrimStartAndEnd().Split(TEXT(":"), &TimeText, &ValuesText) || !ParseFloats(TimeText, 1, &Time)
					|| (!ParseFloats(ValuesText, Given, Values) && !(Channels == 4 && ParseFloats(ValuesText, 3, Values))))
				{
					OutError = FString::Printf(TEXT("Curve key '%s' must be time:%s."), *Key.Left(64),
						Channels == 1 ? TEXT("value") : Channels == 4 ? TEXT("r,g,b,a") : TEXT("x,y,z"));
					return false;
				}
				if (Time <= LastTime)
				{
					OutError = TEXT("Curve key times must increase.");
					return false;
				}
				LastTime = Time;
				for (int32 Channel = 0; Channel < Channels; ++Channel)
				{
					ChannelKeys[Channel].Add(FString::Printf(TEXT("{\"Time\":%s,\"Value\":%s}"), *FString::SanitizeFloat(Time), *FString::SanitizeFloat(Values[Channel])));
				}
			}
			TArray<FString> Fields;
			for (int32 Channel = 0; Channel < Channels; ++Channel)
			{
				const TCHAR* Name = Channels == 1 ? TEXT("Curve") : Channels == 4 ? ColorNames[Channel] : VectorNames[Channel];
				Fields.Add(FString::Printf(TEXT("\"%s\":{\"Keys\":[%s]}"), Name, *FString::Join(ChannelKeys[Channel], TEXT(","))));
			}
			OutJson = TEXT("{") + FString::Join(Fields, TEXT(",")) + TEXT("}");
			return true;
		}

		bool IsJsonObject(const FString& Text)
		{
			TSharedPtr<FJsonObject> Object;
			return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object.IsValid();
		}

		/** The rich value kinds: checked for shape without the System. */
		bool CheckRichValue(const FHyperAINiagaraEditOp& Op, bool bResolveAssets, FString& OutError)
		{
			const FString Trimmed = Op.Value.TrimStartAndEnd();
			if (Op.ValueType == TEXT("enum"))
			{
				OutError = TEXT("enum takes value, the entry's name as the Niagara editor shows it or its C++ name (e.g. Once, ENiagaraLoopBehavior::Once).");
				return !Trimmed.IsEmpty() && Trimmed.Len() <= MaxNameChars;
			}
			if (Op.ValueType == TEXT("linked"))
			{
				OutError = TEXT("linked takes value, a parameter such as Particles.NormalizedAge, Emitter.Age, User.Tint or Engine.DeltaTime.");
				return IsParameterName(Trimmed);
			}
			if (Op.ValueType == TEXT("hlsl_expression"))
			{
				OutError = TEXT("hlsl_expression takes value, one rvalue of the input's type such as float3(0, 0, 100) * Particles.NormalizedAge; no declarations or return.");
				return !Trimmed.IsEmpty() && !Trimmed.Contains(TEXT(";")) && !Trimmed.StartsWith(TEXT("return")) && HasBalancedBrackets(Trimmed);
			}
			if (Op.ValueType == TEXT("dynamic_input"))
			{
				OutError = TEXT("dynamic_input takes asset_path, a dynamic input script (hyper_niagara_discover kind dynamic_inputs lists those for a type). Set its inputs with further ops whose input_name_stack adds the nested input's name.");
				if (Op.AssetPath.IsEmpty())
				{
					return false;
				}
				if (bResolveAssets)
				{
					const UNiagaraScript* Script = Cast<UNiagaraScript>(FSoftObjectPath(Op.AssetPath).TryLoad());
					if (!Script || Script->GetUsage() != ENiagaraScriptUsage::DynamicInput)
					{
						OutError = FString::Printf(TEXT("No Niagara dynamic input script at %s."), *Op.AssetPath);
						return false;
					}
				}
				return true;
			}
			if (Op.ValueType == TEXT("data_interface"))
			{
				OutError = TEXT("data_interface takes value, a JSON object of the data interface's properties ({} keeps its defaults); hyper_niagara_discover kind data_interface_schema lists them.");
				return IsJsonObject(Trimmed);
			}
			if (IsCurveValueType(Op.ValueType))
			{
				FString Json;
				return BuildCurveJson(Op.ValueType, Trimmed, Json, OutError);
			}
			OutError = TEXT("value_type must be float, int32, bool, vector2, vector (x,y,z), vector4, position, color (r,g,b,a), enum, linked, hlsl_expression, dynamic_input, data_interface, curve, color_curve or vector_curve.");
			return false;
		}

		/** The UEnum entry Text names, by its short name, C++ name or display name, ignoring case. */
		bool FindEnumEntry(const UEnum& Enum, const FString& Text, FName& OutName)
		{
			const FString Wanted = Text.TrimStartAndEnd();
			for (int32 Index = 0; Index < Enum.NumEnums() - (Enum.ContainsExistingMax() ? 1 : 0); ++Index)
			{
				const FString Full = Enum.GetNameStringByIndex(Index);
				const FString Short = Enum.GetNameByIndex(Index).ToString();
				if (Wanted.Equals(Full, ESearchCase::IgnoreCase) || Wanted.Equals(Short, ESearchCase::IgnoreCase)
					|| Wanted.Equals(Enum.GetDisplayNameTextByIndex(Index).ToString(), ESearchCase::IgnoreCase))
				{
					OutName = Enum.GetNameByIndex(Index);
					return true;
				}
			}
			return false;
		}

		FString EnumChoices(const UEnum& Enum)
		{
			TArray<FString> Names;
			for (int32 Index = 0; Index < Enum.NumEnums() - (Enum.ContainsExistingMax() ? 1 : 0) && Names.Num() < 24; ++Index)
			{
				Names.Add(Enum.GetNameStringByIndex(Index));
			}
			return FString::Join(Names, TEXT(", "));
		}

		/** What value_type an input takes, from its Niagara type: float, color, enum:ENiagaraLoopBehavior, data_interface:NiagaraDataInterfaceCurve. */
		FString DescribeType(const FNiagaraTypeDefinition& Type)
		{
			if (!Type.IsValid()) return TEXT("unknown");
			if (const UEnum* Enum = Type.GetEnum()) return TEXT("enum:") + Enum->GetName();
			if (Type.IsDataInterface()) return TEXT("data_interface:") + (Type.GetClass() ? Type.GetClass()->GetName() : FString());
			if (Type.IsUObject()) return TEXT("object:") + (Type.GetClass() ? Type.GetClass()->GetName() : FString());
			if (Type == FNiagaraTypeDefinition::GetFloatDef()) return TEXT("float");
			if (Type == FNiagaraTypeDefinition::GetIntDef()) return TEXT("int32");
			if (Type == FNiagaraTypeDefinition::GetBoolDef()) return TEXT("bool");
			if (Type == FNiagaraTypeDefinition::GetVec2Def()) return TEXT("vector2");
			if (Type == FNiagaraTypeDefinition::GetVec3Def()) return TEXT("vector");
			if (Type == FNiagaraTypeDefinition::GetVec4Def()) return TEXT("vector4");
			if (Type == FNiagaraTypeDefinition::GetPositionDef()) return TEXT("position");
			if (Type == FNiagaraTypeDefinition::GetColorDef()) return TEXT("color");
			if (Type == FNiagaraTypeDefinition::GetQuatDef()) return TEXT("quat");
			return Type.GetName();
		}

		/** 1, 0.5, -2.25: a number as short as it reads. */
		FString Compact(const double Value)
		{
			FString Text = FString::SanitizeFloat(Value);
			Text.RemoveFromEnd(TEXT(".0"));
			return Text;
		}

		FString DescribeLocal(const FInstancedStruct& Value)
		{
			const UScriptStruct* Struct = Value.GetScriptStruct();
			const uint8* Memory = Value.GetMemory();
			if (!Struct || !Memory) return TEXT("unset");
			auto Floats = [Memory](int32 Count)
			{
				TArray<FString> Parts;
				for (int32 Index = 0; Index < Count; ++Index)
				{
					Parts.Add(Compact(reinterpret_cast<const float*>(Memory)[Index]));
				}
				return FString::Join(Parts, TEXT(","));
			};
			const FString Name = Struct->GetName();
			if (Name == TEXT("NiagaraFloat")) return TEXT("float ") + Floats(1);
			if (Name == TEXT("NiagaraInt32")) return FString::Printf(TEXT("int32 %d"), *reinterpret_cast<const int32*>(Memory));
			if (Name == TEXT("NiagaraBool")) return reinterpret_cast<const FNiagaraBool*>(Memory)->GetValue() ? TEXT("bool true") : TEXT("bool false");
			if (Name == TEXT("Vector2f")) return TEXT("vector2 ") + Floats(2);
			if (Name == TEXT("Vector3f")) return TEXT("vector ") + Floats(3);
			if (Name == TEXT("NiagaraPosition")) return TEXT("position ") + Floats(3);
			if (Name == TEXT("Vector4f") || Name == TEXT("Quat4f")) return TEXT("vector4 ") + Floats(4);
			if (Name == TEXT("LinearColor")) return TEXT("color ") + Floats(4);
			FString Text;
			Struct->ExportText(Text, Memory, nullptr, nullptr, PPF_None, nullptr);
			return Name + TEXT(" ") + Text;
		}

		/** "Curve: 0:1, 1:0" from a data interface's property JSON, for the curve channels it has. */
		FString SummarizeDataInterface(const FString& Json)
		{
			TSharedPtr<FJsonObject> Object;
			if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) || !Object.IsValid())
			{
				return FString::Printf(TEXT("(%d chars of properties)"), Json.Len());
			}
			TArray<FString> Parts;
			for (const TCHAR* Channel : {TEXT("Curve"), TEXT("RedCurve"), TEXT("GreenCurve"), TEXT("BlueCurve"), TEXT("AlphaCurve"), TEXT("XCurve"), TEXT("YCurve"), TEXT("ZCurve")})
			{
				const TSharedPtr<FJsonObject>* Curve = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
				if (!Object->TryGetObjectField(Channel, Curve) || !(*Curve)->TryGetArrayField(TEXT("Keys"), Keys))
				{
					continue;
				}
				TArray<FString> KeyText;
				for (const TSharedPtr<FJsonValue>& Key : *Keys)
				{
					const TSharedPtr<FJsonObject> KeyObject = Key->AsObject();
					if (KeyObject.IsValid() && KeyText.Num() < 8)
					{
						KeyText.Add(FString::Printf(TEXT("%s:%s"), *Compact(KeyObject->GetNumberField(TEXT("Time"))),
							*Compact(KeyObject->GetNumberField(TEXT("Value")))));
					}
				}
				Parts.Add(FString::Printf(TEXT("%s %s"), Channel, *FString::Join(KeyText, TEXT(", "))));
			}
			return Parts.IsEmpty() ? FString::Printf(TEXT("(%d chars of properties)"), Json.Len()) : FString::Join(Parts, TEXT("; "));
		}

		FString DescribeValue(const FNiagaraExt_StackInputValue& Value)
		{
			if (!Value.IsValid()) return TEXT("unset");
			if (const FNiagaraExt_StackInputData_Enum* Enum = Value.GetPtr<FNiagaraExt_StackInputData_Enum>())
			{
				return TEXT("enum ") + Enum->EnumName.ToString();
			}
			if (const FNiagaraExt_StackInputData_Linked* Linked = Value.GetPtr<FNiagaraExt_StackInputData_Linked>())
			{
				return TEXT("linked ") + Linked->LinkedVariable.Name.ToString();
			}
			if (const FNiagaraExt_StackInputData_HlslExpression* Hlsl = Value.GetPtr<FNiagaraExt_StackInputData_HlslExpression>())
			{
				return TEXT("hlsl_expression ") + Hlsl->HlslExpression;
			}
			if (const FNiagaraExt_StackInputData_DataInterface* Data = Value.GetPtr<FNiagaraExt_StackInputData_DataInterface>())
			{
				return TEXT("data_interface ") + SummarizeDataInterface(Data->PropertyValues);
			}
			if (const FNiagaraExt_StackInputData_DynamicInput* Dynamic = Value.GetPtr<FNiagaraExt_StackInputData_DynamicInput>())
			{
				return TEXT("dynamic_input ") + (Dynamic->DynamicInputAsset ? Dynamic->DynamicInputAsset->GetPathName() : FString(TEXT("none")));
			}
			if (Value.GetPtr<FNiagaraExt_StackInputData_Unsupported>())
			{
				return TEXT("unsupported");
			}
			return DescribeLocal(Value);
		}

		/** A dynamic input and, in braces, its own inputs, to MaxDepth levels. */
		FString DescribeChain(const FNiagaraExt_DynamicInputChain& Chain, int32 Depth)
		{
			FString Text = DescribeValue(Chain.Value);
			if (Chain.Inputs.IsEmpty() || Depth >= MaxInputNameDepth)
			{
				return Text;
			}
			TArray<FString> Children;
			for (const FNiagaraExt_DynamicInputChainRef& Child : Chain.Inputs)
			{
				const FNiagaraExt_DynamicInputChain& Entry = Child.Get();
				if (Entry.bIsVisible)
				{
					Children.Add(Entry.Name.ToString() + TEXT(" = ") + DescribeChain(Entry, Depth + 1));
				}
			}
			return Children.IsEmpty() ? Text : Text + TEXT(" {") + FString::Join(Children, TEXT("; ")) + TEXT("}");
		}

		FNiagaraExt_StackItemReference MakeReference(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op)
		{
			FNiagaraExt_StackItemReference Reference(
				&System, ToName(Op.EmitterName), ToName(Op.ScriptName), ToName(Op.ModuleName));
			for (const FString& InputName : Op.InputNameStack)
			{
				Reference.InputNameStack.Add(ToName(InputName));
			}
			return Reference;
		}

		/** A user parameter's or Set Parameters entry's value from the op's value_type and value. */
		bool MakeVariableValue(const FHyperAINiagaraEditOp& Op, FNiagaraExt_VariableValue& OutValue)
		{
			FInstancedStruct Parsed;
			const FNiagaraTypeDefinition Type = TypeOfValueType(Op.ValueType);
			if (!Type.IsValid() || !BuildInputValue(Op.ValueType, Op.Value, Parsed) || !Parsed.GetScriptStruct())
			{
				return false;
			}
			FNiagaraVariant Variant;
			Variant.SetBytes(Parsed.GetMemory(), Parsed.GetScriptStruct()->GetStructureSize());
			OutValue.Set(Type, Variant);
			return OutValue.IsValid();
		}

		/**
		 * The stack value an op writes. Local values need nothing but the text; the rich kinds read the input's own
		 * type first, since an enum, a link or a data interface must match it.
		 */
		bool BuildStackValue(UNiagaraSystem& System, const FHyperAINiagaraEditOp& Op, FNiagaraExternalEditContext& Context,
			FNiagaraExt_StackInputValue& OutValue, FString& OutError)
		{
			if (IsLocalValueType(Op.ValueType))
			{
				OutError = TEXT("The input value no longer parses.");
				return BuildInputValue(Op.ValueType, Op.Value, OutValue);
			}
			FNiagaraExt_StackInputTopology Input;
			UNiagaraExternalEditUtilities::GetStackInputTopology(MakeReference(System, Op), Input, Context);
			if (Context.HasErrors())
			{
				return false;
			}
			const FString Text = Op.Value.TrimStartAndEnd();
			const FString Path = FString::Join(Op.InputNameStack, TEXT("."));
			if (Op.ValueType == TEXT("enum"))
			{
				UEnum* Enum = Input.Type.GetEnum();
				FName Entry;
				if (!Enum)
				{
					OutError = FString::Printf(TEXT("Input %s is %s, not an enum."), *Path, *DescribeType(Input.Type));
					return false;
				}
				if (!FindEnumEntry(*Enum, Text, Entry))
				{
					OutError = FString::Printf(TEXT("%s has no entry '%s'. Entries: %s."), *Enum->GetName(), *Text.Left(64), *EnumChoices(*Enum));
					return false;
				}
				FNiagaraExt_StackInputData_Enum& Value = OutValue.InitializeAs<FNiagaraExt_StackInputData_Enum>();
				Value.Enum = Enum;
				Value.EnumName = Entry;
				return true;
			}
			if (Op.ValueType == TEXT("linked"))
			{
				FNiagaraExt_StackInputData_Linked& Value = OutValue.InitializeAs<FNiagaraExt_StackInputData_Linked>();
				Value.LinkedVariable.Name = FName(*Text);
				Value.LinkedVariable.Type = Input.Type;
				return true;
			}
			if (Op.ValueType == TEXT("hlsl_expression"))
			{
				OutValue.InitializeAs<FNiagaraExt_StackInputData_HlslExpression>().HlslExpression = Text;
				return true;
			}
			if (Op.ValueType == TEXT("dynamic_input"))
			{
				UNiagaraScript* Script = Cast<UNiagaraScript>(FSoftObjectPath(Op.AssetPath).ResolveObject());
				if (!Script)
				{
					OutError = FString::Printf(TEXT("The dynamic input %s is no longer loaded."), *Op.AssetPath);
					return false;
				}
				OutValue.InitializeAs<FNiagaraExt_StackInputData_DynamicInput>().DynamicInputAsset = Script;
				return true;
			}
			// data_interface and the curve shorthands.
			if (!Input.Type.IsDataInterface())
			{
				OutError = FString::Printf(TEXT("Input %s is %s, not a data interface. A curve over a float or colour input goes through a dynamic input (Float From Curve, Color From Curve) whose Curve input takes it."),
					*Path, *DescribeType(Input.Type));
				return false;
			}
			FString Json = Text;
			if (IsCurveValueType(Op.ValueType) && !BuildCurveJson(Op.ValueType, Text, Json, OutError))
			{
				return false;
			}
			OutValue.InitializeAs<FNiagaraExt_StackInputData_DataInterface>().PropertyValues = Json;
			return true;
		}

		FString StackSeverity(ENiagaraExt_StackIssueSeverity Severity)
		{
			switch (Severity)
			{
			case ENiagaraExt_StackIssueSeverity::Error: return TEXT("error");
			case ENiagaraExt_StackIssueSeverity::Warning: return TEXT("warning");
			default: return TEXT("info");
			}
		}

		void CopyStack(const FNiagaraExt_ScriptStackTopology& Source, FHyperAINiagaraScriptStackTopology& Out, int32& ModuleBudget, bool& bTruncated)
		{
			Out.ScriptName = Source.ScriptName.ToString();
			for (const FNiagaraExt_ModuleTopology& Module : Source.Modules)
			{
				if (ModuleBudget <= 0)
				{
					bTruncated = true;
					return;
				}
				--ModuleBudget;
				FHyperAINiagaraModuleTopology& Entry = Out.Modules.AddDefaulted_GetRef();
				Entry.ModuleName = Module.ModuleName.ToString();
				Entry.bEnabled = Module.Enabled;
				Entry.bIsSetParametersModule = Module.bIsSetParametersModule;
				Entry.ScriptAssetPath = Module.ModuleScript ? Module.ModuleScript->GetPathName() : FString();
				for (const FNiagaraExt_StackInputTopology& Input : Module.Inputs)
				{
					if (Entry.InputNames.Num() >= MaxInputNamesPerModule)
					{
						bTruncated = true;
						break;
					}
					Entry.InputNames.Add(Input.Name.ToString());
					Entry.InputTypes.Add(DescribeType(Input.Type));
				}
			}
		}

		/** Each module's input values, "Name = kind value", with dynamic inputs followed down their chains. */
		void ReadStackValues(UNiagaraSystem& System, const FString& EmitterName, FHyperAINiagaraScriptStackTopology& Stack,
			FNiagaraExternalEditContext& Context)
		{
			for (FHyperAINiagaraModuleTopology& Module : Stack.Modules)
			{
				const FNiagaraExt_StackItemReference ModuleRef(&System, ToName(EmitterName), ToName(Stack.ScriptName), ToName(Module.ModuleName));
				FNiagaraExt_ModuleInputValues Values;
				const int32 ErrorsBefore = Context.Errors.Num();
				UNiagaraExternalEditUtilities::GetModuleInputValues(ModuleRef, Values, Context);
				for (const FNiagaraExt_StackInputValueEntry& Entry : Values.Inputs)
				{
					if (Module.InputValues.Num() >= MaxInputNamesPerModule)
					{
						break;
					}
					FString Text = DescribeValue(Entry.Value);
					if (Entry.Value.GetPtr<FNiagaraExt_StackInputData_DynamicInput>())
					{
						FNiagaraExt_StackItemReference InputRef = ModuleRef;
						InputRef.InputNameStack.Add(Entry.Name);
						FNiagaraExt_DynamicInputChainRef Chain;
						UNiagaraExternalEditUtilities::GetDynamicInputChain(InputRef, Chain, Context);
						Text = DescribeChain(Chain.Get(), 1);
					}
					Module.InputValues.Add((Entry.Name.ToString() + TEXT(" = ") + Text).Left(MaxSummaryChars));
				}
				// A value that does not read is shown as missing; it never fails the topology.
				Context.Errors.SetNum(ErrorsBefore);
			}
		}
	}

	bool IsApiAvailable()
	{
		return UNiagaraExternalEditUtilities::StaticClass() != nullptr;
	}

	bool IsCreatePlan(const TArray<FHyperAINiagaraEditOp>& Ops)
	{
		return Ops.Num() > 0 && Ops[0].Kind == TEXT("create_system");
	}

	TArray<FName> ParseEmitterList(const FString& Value)
	{
		TArray<FString> Parts;
		Value.ParseIntoArray(Parts, TEXT(","));
		TArray<FName> Names;
		for (const FString& Part : Parts)
		{
			if (!Part.TrimStartAndEnd().IsEmpty())
			{
				Names.AddUnique(FName(*Part.TrimStartAndEnd()));
			}
		}
		return Names;
	}

	UNiagaraSystem* CreateSystem(const FString& TargetPath, const FHyperAINiagaraEditOp& Op, FString& OutError)
	{
		check(IsInGameThread());
		OutError.Reset();
		const FString PackageName = FPackageName::ObjectPathToPackageName(TargetPath);
		UNiagaraSystem* Template = nullptr;
		if (!Op.AssetPath.IsEmpty())
		{
			Template = Cast<UNiagaraSystem>(FSoftObjectPath(Op.AssetPath).TryLoad());
			if (!Template)
			{
				OutError = FString::Printf(TEXT("No Niagara System template at %s."), *Op.AssetPath);
				return nullptr;
			}
		}
		const TArray<FName> Keep = ParseEmitterList(Op.Value);
		if (Template)
		{
			for (const FName& Name : Keep)
			{
				if (!Template->GetEmitterHandles().ContainsByPredicate([&Name](const FNiagaraEmitterHandle& Handle) { return Handle.GetName() == Name; }))
				{
					OutError = FString::Printf(TEXT("The template has no emitter %s to keep."), *Name.ToString());
					return nullptr;
				}
			}
		}
		FNiagaraExternalEditContext Context;
		UNiagaraSystem* System = UNiagaraExternalEditUtilities::CreateNiagaraSystem(
			FPackageName::GetLongPackageAssetName(PackageName), FPackageName::GetLongPackagePath(PackageName), Template, Context);
		if (!System || Context.HasErrors())
		{
			OutError = Context.HasErrors() ? JoinErrors(Context, 0) : FString(TEXT("The System was not created."));
			return System;
		}
		if (!Keep.IsEmpty())
		{
			TArray<FName> Drop;
			for (const FNiagaraEmitterHandle& Handle : System->GetEmitterHandles())
			{
				if (!Keep.Contains(Handle.GetName()))
				{
					Drop.Add(Handle.GetName());
				}
			}
			FNiagaraExternalEditContext RemoveContext(System);
			for (const FName& Name : Drop)
			{
				UNiagaraExternalEditUtilities::RemoveEmitter(FNiagaraExt_StackItemReference(System, Name), RemoveContext);
			}
			if (RemoveContext.HasErrors())
			{
				OutError = JoinErrors(RemoveContext, 0);
			}
		}
		return System;
	}

	bool ReadScriptSchema(const UNiagaraScript& Script, TArray<FHyperAINiagaraInputSchema>& OutInputs, TArray<FString>& OutOutputs, FString& OutError)
	{
		check(IsInGameThread());
		OutInputs.Reset();
		OutOutputs.Reset();
		FNiagaraExternalEditContext Context;
		FNiagaraExt_DynamicInputSchema Schema;
		if (Script.GetUsage() == ENiagaraScriptUsage::DynamicInput)
		{
			UNiagaraExternalEditUtilities::GetDynamicInputSchema(&Script, Schema, Context);
		}
		else
		{
			UNiagaraExternalEditUtilities::GetModuleSchema(&Script, Schema, Context);
		}
		if (Context.HasErrors())
		{
			OutError = JoinErrors(Context, 0);
			return false;
		}
		for (const FNiagaraExt_StackInputSchema& Input : Schema.Inputs)
		{
			if (OutInputs.Num() >= MaxInputNamesPerModule)
			{
				break;
			}
			FHyperAINiagaraInputSchema& Out = OutInputs.AddDefaulted_GetRef();
			Out.Name = Input.Name.ToString();
			Out.ValueType = DescribeType(Input.Type);
			Out.Category = Input.Category.ToString().Left(MaxNameChars);
			Out.Description = Input.MetaData.Description.ToString().Left(MaxSummaryChars);
			Out.bSupportsExpressions = Input.bSupportsExpressions;
		}
		for (const FNiagaraExt_Variable& Output : Schema.Outputs)
		{
			if (OutOutputs.Num() < MaxInputNamesPerModule)
			{
				OutOutputs.Add(Output.Name.ToString() + TEXT(" (") + DescribeType(Output.Type) + TEXT(")"));
			}
		}
		return true;
	}

	bool ListDynamicInputs(const FString& ValueType, TArray<FString>& OutPaths, FString& OutError)
	{
		check(IsInGameThread());
		OutPaths.Reset();
		const FNiagaraTypeDefinition Type = ValueType == TEXT("quat") ? FNiagaraTypeDefinition::GetQuatDef() : TypeOfValueType(ValueType);
		if (!Type.IsValid())
		{
			OutError = TEXT("value_type must be float, int32, bool, vector2, vector, vector4, position, color or quat.");
			return false;
		}
		FNiagaraExternalEditContext Context;
		TArray<UNiagaraScript*> Scripts;
		UNiagaraExternalEditUtilities::GetAvailableDynamicInputs(Type, Scripts, Context);
		for (const UNiagaraScript* Script : Scripts)
		{
			if (Script)
			{
				OutPaths.Add(Script->GetPathName());
			}
		}
		OutPaths.Sort();
		return true;
	}

	bool ReadTopology(
		UNiagaraSystem& System,
		TArray<FHyperAINiagaraEmitterTopology>& OutEmitters,
		bool& bOutTruncated,
		FString& OutError,
		bool bIncludeValues)
	{
		OutEmitters.Reset();
		bOutTruncated = false;
		OutError.Reset();
		check(IsInGameThread());

		FNiagaraExternalEditContext Context(&System);
		FNiagaraExt_SystemSummary Summary;
		UNiagaraExternalEditUtilities::GetSystemSummary(&System, Summary, Context);
		if (Context.HasErrors())
		{
			OutError = JoinErrors(Context, 0);
			return false;
		}

		int32 ModuleBudget = MaxTopologyModules;
		for (const FNiagaraExt_EmitterSummary& Emitter : Summary.Emitters)
		{
			if (OutEmitters.Num() >= FHyperAIStudioNiagaraContracts::MaxEmitters)
			{
				bOutTruncated = true;
				break;
			}
			FNiagaraExt_EmitterTopology Topology;
			UNiagaraExternalEditUtilities::GetEmitterTopology(
				FNiagaraExt_StackItemReference(&System, Emitter.EmitterName), Topology, Context);
			if (Context.HasErrors())
			{
				OutError = JoinErrors(Context, 0);
				return false;
			}

			FHyperAINiagaraEmitterTopology& Out = OutEmitters.AddDefaulted_GetRef();
			Out.EmitterName = Topology.EmitterName.ToString();
			Out.bEnabled = Topology.bEnabled;
			Out.SimTarget = Topology.SimTarget == ENiagaraSimTarget::GPUComputeSim ? TEXT("GPUComputeSim") : TEXT("CPUSim");
			for (const FNiagaraExt_ScriptStackTopology* Stack : {
				&Topology.EmitterSpawnScript, &Topology.EmitterUpdateScript,
				&Topology.ParticleSpawnScript, &Topology.ParticleUpdateScript})
			{
				FHyperAINiagaraScriptStackTopology& Copied = Out.ScriptStacks.AddDefaulted_GetRef();
				CopyStack(*Stack, Copied, ModuleBudget, bOutTruncated);
				if (bIncludeValues)
				{
					ReadStackValues(System, Out.EmitterName, Copied, Context);
				}
			}
			// The external API reads only the four fixed stacks; stages come from the emitter and its graph.
			HyperAIStudio::Niagara::GraphGate::ReadSimulationStages(System, Out.EmitterName, Out.SimulationStages, ModuleBudget, bOutTruncated);
			for (const FNiagaraExt_RendererRef& Renderer : Topology.Renderers)
			{
				FHyperAINiagaraRendererTopology& Entry = Out.Renderers.AddDefaulted_GetRef();
				Entry.RendererIndex = Renderer.RendererIndex;
				Entry.RendererClassPath = Renderer.RendererClass ? Renderer.RendererClass->GetPathName() : FString();
			}
		}
		return true;
	}

	bool ReadDiagnostics(UNiagaraSystem& System, int32 MaxIssues, FDiagnostics& OutDiagnostics, FString& OutError)
	{
		OutDiagnostics = FDiagnostics();
		OutError.Reset();
		check(IsInGameThread());

		FNiagaraExternalEditContext Context(&System);
		FNiagaraExt_SystemCompileState Compile;
		UNiagaraExternalEditUtilities::GetSystemCompileState(&System, Compile, Context);
		FNiagaraExt_StackIssues Stack;
		UNiagaraExternalEditUtilities::GetStackIssues(&System, Stack, Context);
		if (Context.HasErrors())
		{
			OutError = JoinErrors(Context, 0);
			return false;
		}

		OutDiagnostics.bCompileStateKnown = Compile.AggregateStatus != ENiagaraExt_ScriptCompileStatus::Unknown;
		OutDiagnostics.bCompiling = Compile.bIsCompiling || Compile.bIsStale;
		OutDiagnostics.bCompileHasErrors = Compile.bHasErrors;
		OutDiagnostics.bCompileHasWarnings = Compile.bHasWarnings;

		auto AddIssue = [&](FHyperAINiagaraIssue&& Issue)
		{
			int32& Counter = Issue.Severity == TEXT("error") ? OutDiagnostics.ErrorCount
				: Issue.Severity == TEXT("warning") ? OutDiagnostics.WarningCount : OutDiagnostics.InfoCount;
			++Counter;
			if (OutDiagnostics.Issues.Num() >= MaxIssues)
			{
				OutDiagnostics.bTruncated = true;
				return;
			}
			Issue.Summary.LeftInline(MaxSummaryChars);
			Issue.Description.LeftInline(MaxDescriptionChars);
			OutDiagnostics.Issues.Add(MoveTemp(Issue));
		};

		for (const FNiagaraExt_StackIssue& StackIssue : Stack.Issues)
		{
			if (StackIssue.bIsDismissed)
			{
				continue;
			}
			if (StackIssue.Severity == ENiagaraExt_StackIssueSeverity::Error)
			{
				++OutDiagnostics.StackErrorCount;
			}
			FHyperAINiagaraIssue Issue;
			Issue.Code = TEXT("stack_issue");
			Issue.Severity = StackSeverity(StackIssue.Severity);
			Issue.StableId = StackIssue.IssueId.Left(MaxIdChars);
			Issue.SourcePath = StackIssue.StackDisplayPath.Left(MaxPathChars);
			Issue.Summary = StackIssue.ShortDescription;
			Issue.Description = StackIssue.LongDescription;
			for (const FNiagaraExt_StackIssueFix& Fix : StackIssue.Fixes)
			{
				// Link-style fixes are navigation hints for a person; ApplyStackIssueFix refuses them.
				if (Fix.Style == ENiagaraExt_StackIssueFixStyle::Fix)
				{
					Issue.FixIds.Add(Fix.FixId.Left(MaxIdChars));
				}
			}
			AddIssue(MoveTemp(Issue));
		}

		for (const FNiagaraExt_ScriptCompileInfo& Script : Compile.Scripts)
		{
			for (const FNiagaraExt_CompileEvent& Event : Script.CompileEvents)
			{
				if (Event.Severity != ENiagaraExt_CompileEventSeverity::Error
					&& Event.Severity != ENiagaraExt_CompileEventSeverity::Warning)
				{
					continue;
				}
				FHyperAINiagaraIssue Issue;
				Issue.Code = TEXT("compile_event");
				Issue.Severity = Event.Severity == ENiagaraExt_CompileEventSeverity::Error ? TEXT("error") : TEXT("warning");
				Issue.SourcePath = FString::Printf(TEXT("%s/%s"), *Script.EmitterName.ToString(), *Script.ScriptName.ToString()).Left(MaxPathChars);
				Issue.Summary = Event.ShortDescription.IsEmpty() ? Event.Message : Event.ShortDescription;
				Issue.Description = Event.Message;
				AddIssue(MoveTemp(Issue));
			}
		}
		return true;
	}

	bool ValidateOp(const FHyperAINiagaraEditOp& Op, bool bResolveAssets, FString& OutStatus, FString& OutDiagnostic)
	{
		OutStatus.Reset();
		OutDiagnostic.Reset();
		auto Fail = [&](const TCHAR* Status, const FString& Diagnostic)
		{
			OutStatus = Status;
			OutDiagnostic = Diagnostic;
			return false;
		};

		// Values carry asset paths and struct text for renderer properties, so they get the path bound. The HLSL body
		// of add_hlsl_module is checked by the graph gate, which allows its newlines and tabs.
		if (!IsBoundedText(Op.EmitterName, MaxNameChars) || !IsBoundedText(Op.ScriptName, MaxNameChars)
			|| !IsBoundedText(Op.StageName, MaxNameChars) || !IsBoundedText(Op.ModuleName, MaxNameChars)
			|| !IsBoundedText(Op.Name, MaxNameChars) || !IsBoundedText(Op.AssetPath, MaxPathChars)
			|| !IsBoundedText(Op.IssueId, MaxIdChars) || !IsBoundedText(Op.FixId, MaxIdChars)
			|| !IsBoundedText(Op.ValueType, MaxNameChars)
			|| !(IsLongValueType(Op.ValueType) ? IsBoundedLongText(Op.Value, MaxLongValueChars) : IsBoundedText(Op.Value, MaxPathChars))
			|| Op.InputNameStack.Num() > MaxInputNameDepth)
		{
			return Fail(TEXT("invalid_op_text"), TEXT("An op field is too long, contains control characters, or nests inputs too deeply."));
		}
		for (const FString& InputName : Op.InputNameStack)
		{
			if (InputName.IsEmpty() || !IsBoundedText(InputName, MaxNameChars))
			{
				return Fail(TEXT("invalid_op_text"), TEXT("input_name_stack entries must be non-empty, bounded names."));
			}
		}
		if (HyperAIStudio::Niagara::GraphGate::IsGraphOp(Op))
		{
			return HyperAIStudio::Niagara::GraphGate::ValidateGraphOp(Op, bResolveAssets, OutStatus, OutDiagnostic);
		}
		if (!Op.StageName.IsEmpty() || !Op.Hlsl.IsEmpty())
		{
			// A stage address on an op that cannot use one would otherwise edit a different stack.
			return Fail(TEXT("invalid_op_address"), Op.Kind == TEXT("set_input_value")
				? TEXT("Inputs of simulation-stage modules cannot be set yet; write the values into an add_hlsl_module in that stage.")
				: TEXT("stage_name goes only with add_module, set_module_enabled and add_hlsl_module; hlsl only with add_hlsl_module and set_hlsl_module."));
		}

		const bool bHasScript = !Op.ScriptName.IsEmpty();
		if (bHasScript && !ScriptNames().Contains(Op.ScriptName))
		{
			return Fail(TEXT("invalid_script_name"), TEXT("script_name must be one of SystemSpawnScript, SystemUpdateScript, EmitterSpawnScript, EmitterUpdateScript, ParticleSpawnScript, ParticleUpdateScript."));
		}
		// Emitter scripts need their emitter; system scripts must not name one.
		const bool bEmitterScopeOk = !bHasScript || (IsSystemScript(Op.ScriptName) == Op.EmitterName.IsEmpty());

		if (Op.Kind == TEXT("set_module_enabled"))
		{
			if (!bHasScript || !bEmitterScopeOk || Op.ModuleName.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("set_module_enabled needs script_name, module_name, and emitter_name for emitter scripts."));
			}
			return true;
		}
		if (Op.Kind == TEXT("add_module"))
		{
			if (!bHasScript || !bEmitterScopeOk || Op.AssetPath.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("add_module needs script_name, asset_path of a Niagara module script, and emitter_name for emitter scripts."));
			}
			if (bResolveAssets && !Cast<UNiagaraScript>(FSoftObjectPath(Op.AssetPath).TryLoad()))
			{
				return Fail(TEXT("module_script_not_found"), FString::Printf(TEXT("No Niagara module script at %s."), *Op.AssetPath));
			}
			return true;
		}
		if (Op.Kind == TEXT("add_renderer"))
		{
			if (Op.EmitterName.IsEmpty() || bHasScript || Op.AssetPath.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("add_renderer needs emitter_name and asset_path of a renderer class, and no script_name."));
			}
			if (bResolveAssets)
			{
				const UClass* RendererClass = FSoftClassPath(Op.AssetPath).TryLoadClass<UNiagaraRendererProperties>();
				if (!RendererClass || RendererClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
				{
					return Fail(TEXT("renderer_class_not_found"), FString::Printf(TEXT("%s is not a concrete Niagara renderer class."), *Op.AssetPath));
				}
			}
			return true;
		}
		if (Op.Kind == TEXT("add_emitter"))
		{
			if (Op.AssetPath.IsEmpty() || Op.Name.IsEmpty() || bHasScript || !Op.EmitterName.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("add_emitter needs asset_path of an emitter template and a new name, and no emitter or script address."));
			}
			if (bResolveAssets && !Cast<UNiagaraEmitter>(FSoftObjectPath(Op.AssetPath).TryLoad()))
			{
				return Fail(TEXT("emitter_template_not_found"), FString::Printf(TEXT("No Niagara emitter at %s."), *Op.AssetPath));
			}
			return true;
		}
		if (Op.Kind == TEXT("set_input_value"))
		{
			if (!bHasScript || !bEmitterScopeOk || Op.ModuleName.IsEmpty() || Op.InputNameStack.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("set_input_value needs script_name, module_name, input_name_stack, and emitter_name for emitter scripts."));
			}
			if (Op.ValueType != TEXT("dynamic_input") && !Op.AssetPath.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("asset_path goes with value_type dynamic_input only."));
			}
			if (IsLocalValueType(Op.ValueType))
			{
				FInstancedStruct Parsed;
				return BuildInputValue(Op.ValueType, Op.Value, Parsed) ? true
					: Fail(TEXT("invalid_input_value"), FString::Printf(TEXT("value '%s' does not parse as %s (vectors and colours are comma separated: 1,2,3 and 1,0.5,0,1)."), *Op.Value.Left(64), *Op.ValueType));
			}
			FString Why;
			if (!CheckRichValue(Op, bResolveAssets, Why))
			{
				return Fail(TEXT("invalid_input_value"), Why);
			}
			if (Op.ValueType != TEXT("dynamic_input") && !Op.AssetPath.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("asset_path goes with value_type dynamic_input only."));
			}
			return true;
		}
		if (Op.Kind == TEXT("create_system"))
		{
			// Ordering (first op, absent target) is checked with the plan; here only the shape.
			if (!Op.EmitterName.IsEmpty() || !Op.ScriptName.IsEmpty() || !Op.ModuleName.IsEmpty() || !Op.InputNameStack.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("create_system takes asset_path (a template System, or empty for an empty System) and value (emitters to keep, comma separated; empty keeps all), and no emitter, script or module."));
			}
			if (bResolveAssets && !Op.AssetPath.IsEmpty() && !Cast<UNiagaraSystem>(FSoftObjectPath(Op.AssetPath).TryLoad()))
			{
				return Fail(TEXT("system_template_not_found"), FString::Printf(TEXT("No Niagara System at %s; hyper_niagara_discover kind templates lists them."), *Op.AssetPath));
			}
			return true;
		}
		if (Op.Kind == TEXT("remove_emitter"))
		{
			if (Op.EmitterName.IsEmpty() || bHasScript || !Op.ModuleName.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("remove_emitter needs emitter_name and nothing else."));
			}
			return true;
		}
		if (Op.Kind == TEXT("remove_module"))
		{
			if (!bHasScript || !bEmitterScopeOk || Op.ModuleName.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("remove_module needs script_name, module_name, and emitter_name for emitter scripts."));
			}
			return true;
		}
		if (Op.Kind == TEXT("remove_renderer"))
		{
			if (Op.EmitterName.IsEmpty() || bHasScript || Op.RendererIndex < 0)
			{
				return Fail(TEXT("invalid_op_address"), TEXT("remove_renderer needs emitter_name and renderer_index; later renderers move down one index."));
			}
			return true;
		}
		if (Op.Kind == TEXT("add_user_parameter") || Op.Kind == TEXT("remove_user_parameter"))
		{
			const FString Name = Op.Name.StartsWith(TEXT("User.")) ? Op.Name : TEXT("User.") + Op.Name;
			FInstancedStruct Parsed;
			if (!Op.EmitterName.IsEmpty() || bHasScript || !FHyperAIStudioNiagaraContracts::IsUserParameterName(Name) || Name.Mid(5).Contains(TEXT(".")))
			{
				return Fail(TEXT("invalid_op_address"), FString::Printf(TEXT("%s needs name (User.Tint or Tint) and value_type, and no emitter or script."), *Op.Kind));
			}
			if (!IsLocalValueType(Op.ValueType) || (Op.Kind == TEXT("add_user_parameter") && !BuildInputValue(Op.ValueType, Op.Value, Parsed)))
			{
				return Fail(TEXT("invalid_input_value"), TEXT("A user parameter's value_type is float, int32, bool, vector2, vector, vector4, position or color, and add_user_parameter's value is its default in that type."));
			}
			return true;
		}
		if (Op.Kind == TEXT("add_set_parameters"))
		{
			FInstancedStruct Parsed;
			if (!bHasScript || !bEmitterScopeOk || !IsParameterName(Op.Name))
			{
				return Fail(TEXT("invalid_op_address"), TEXT("add_set_parameters needs script_name, emitter_name for emitter scripts, and name, the parameter it writes (Particles.Wobble, Emitter.Heat); module_name adds the entry to an existing Set Parameters module instead of a new one."));
			}
			if (!IsLocalValueType(Op.ValueType) || !BuildInputValue(Op.ValueType, Op.Value, Parsed))
			{
				return Fail(TEXT("invalid_input_value"), TEXT("add_set_parameters' value_type is float, int32, bool, vector2, vector, vector4, position or color, and value its default; set_input_value can then make it linked, random or an expression."));
			}
			return true;
		}
		if (Op.Kind == TEXT("apply_stack_issue_fix"))
		{
			if (Op.IssueId.IsEmpty() || Op.FixId.IsEmpty())
			{
				return Fail(TEXT("invalid_op_address"), TEXT("apply_stack_issue_fix needs issue_id and fix_id from hyper_niagara_validate."));
			}
			return true;
		}
		return Fail(TEXT("unsupported_op_kind"), TEXT("kind must be create_system, add_emitter, remove_emitter, add_module, remove_module, set_module_enabled, add_renderer, remove_renderer, set_input_value, add_user_parameter, remove_user_parameter, add_set_parameters, apply_stack_issue_fix, add_simulation_stage, add_hlsl_module, set_hlsl_module, set_emitter_property, set_renderer_property or set_renderer_binding."));
	}

	bool ApplyOps(
		UNiagaraSystem& System,
		const TArray<FHyperAINiagaraEditOp>& Ops,
		int32& OutAppliedCount,
		TArray<FString>& OutPerOpStatus,
		FString& OutError)
	{
		OutAppliedCount = 0;
		OutPerOpStatus.Reset();
		OutError.Reset();
		check(IsInGameThread());

		// One transaction for the whole batch: a single undo step reverts every op together.
		FScopedTransaction Transaction(LOCTEXT("ApplyOpsTransaction", "HyperAI Niagara Edit"));
		System.Modify();
		FNiagaraExternalEditContext Context(&System);

		for (const FHyperAINiagaraEditOp& Op : Ops)
		{
			const int32 ErrorsBefore = Context.Errors.Num();
			FString Status = TEXT("applied");

			if (HyperAIStudio::Niagara::GraphGate::IsGraphOp(Op))
			{
				FString GraphError;
				if (!HyperAIStudio::Niagara::GraphGate::ApplyGraphOp(System, Op, Status, GraphError))
				{
					Context.Error(FText::FromString(GraphError));
				}
			}
			else if (Op.Kind == TEXT("set_module_enabled"))
			{
				UNiagaraExternalEditUtilities::SetModuleEnabled(MakeReference(System, Op), Op.bEnabled, Context);
			}
			else if (Op.Kind == TEXT("add_module"))
			{
				FNiagaraExt_ModuleTopology Added;
				UNiagaraExternalEditUtilities::AddModule(MakeReference(System, Op),
					Cast<UNiagaraScript>(FSoftObjectPath(Op.AssetPath).ResolveObject()), Added, Context);
				Status = TEXT("added_module:") + Added.ModuleName.ToString();
			}
			else if (Op.Kind == TEXT("add_renderer"))
			{
				FNiagaraExt_RendererRef Added;
				UNiagaraExternalEditUtilities::AddRenderer(MakeReference(System, Op),
					FSoftClassPath(Op.AssetPath).ResolveClass(), Added, Context);
				Status = FString::Printf(TEXT("added_renderer:%d"), Added.RendererIndex);
			}
			else if (Op.Kind == TEXT("add_emitter"))
			{
				FNiagaraExt_EmitterTopology Added;
				UNiagaraExternalEditUtilities::AddEmitter(
					Cast<UNiagaraEmitter>(FSoftObjectPath(Op.AssetPath).ResolveObject()), ToName(Op.Name), Added, Context);
				Status = TEXT("added_emitter:") + Added.EmitterName.ToString();
			}
			else if (Op.Kind == TEXT("set_input_value"))
			{
				FNiagaraExt_StackInputValue Value;
				FString ValueError;
				if (BuildStackValue(System, Op, Context, Value, ValueError))
				{
					UNiagaraExternalEditUtilities::SetStackInputData(MakeReference(System, Op), Value, Context);
					Status = TEXT("set_input:") + FString::Join(Op.InputNameStack, TEXT("."));
				}
				else if (Context.Errors.Num() == ErrorsBefore)
				{
					Context.Error(FText::FromString(ValueError));
				}
			}
			else if (Op.Kind == TEXT("create_system"))
			{
				// CreateSystem made the System before this batch; the op stays in the plan so the dry run and approval show it.
				Status = TEXT("created_system");
			}
			else if (Op.Kind == TEXT("remove_emitter"))
			{
				UNiagaraExternalEditUtilities::RemoveEmitter(MakeReference(System, Op), Context);
				Status = TEXT("removed_emitter:") + Op.EmitterName;
			}
			else if (Op.Kind == TEXT("remove_module"))
			{
				UNiagaraExternalEditUtilities::RemoveModule(MakeReference(System, Op), Context);
				Status = TEXT("removed_module:") + Op.ModuleName;
			}
			else if (Op.Kind == TEXT("remove_renderer"))
			{
				FNiagaraExt_StackItemReference Reference = MakeReference(System, Op);
				Reference.RendererIndex = Op.RendererIndex;
				UNiagaraExternalEditUtilities::RemoveRenderer(Reference, Context);
				Status = FString::Printf(TEXT("removed_renderer:%d"), Op.RendererIndex);
			}
			else if (Op.Kind == TEXT("add_user_parameter") || Op.Kind == TEXT("remove_user_parameter"))
			{
				const FString Name = Op.Name.StartsWith(TEXT("User.")) ? Op.Name : TEXT("User.") + Op.Name;
				const FNiagaraTypeDefinition Type = TypeOfValueType(Op.ValueType);
				if (Op.Kind == TEXT("remove_user_parameter"))
				{
					FNiagaraExt_Variable Variable;
					Variable.Name = FName(*Name);
					Variable.Type = Type;
					UNiagaraExternalEditUtilities::RemoveUserVariable(&System, Variable, Context);
					Status = TEXT("removed_user_parameter:") + Name;
				}
				else
				{
					FNiagaraExt_UserVariable Variable;
					Variable.Name = FName(*Name);
					Variable.Type = Type;
					if (MakeVariableValue(Op, Variable.DefaultValue))
					{
						UNiagaraExternalEditUtilities::AddUserVariable(&System, Variable, Context);
						Status = TEXT("added_user_parameter:") + Name;
					}
					else
					{
						Context.Error(LOCTEXT("InvalidUserDefault", "The user parameter's default no longer parses."));
					}
				}
			}
			else if (Op.Kind == TEXT("add_set_parameters"))
			{
				FNiagaraExt_SetParameterEntry Entry;
				Entry.Variable.Name = FName(*Op.Name);
				Entry.Variable.Type = TypeOfValueType(Op.ValueType);
				FNiagaraExt_ModuleTopology Added;
				if (!MakeVariableValue(Op, Entry.DefaultValue))
				{
					Context.Error(LOCTEXT("InvalidSetParameter", "The Set Parameters value no longer parses."));
				}
				else if (Op.ModuleName.IsEmpty())
				{
					UNiagaraExternalEditUtilities::AddSetParametersModule(MakeReference(System, Op), {Entry}, Added, Context);
				}
				else
				{
					UNiagaraExternalEditUtilities::AddSetParameterEntry(MakeReference(System, Op), Entry, Added, Context);
				}
				Status = TEXT("set_parameters:") + Added.ModuleName.ToString();
			}
			else if (HyperAIStudio::Niagara::AssetsGate::IsAssetOp(Op.Kind))
			{
				FString AssetError;
				if (!HyperAIStudio::Niagara::AssetsGate::ApplyAssetOp(System, Op, Status, AssetError))
				{
					Context.Error(FText::FromString(AssetError));
				}
			}
			else if (Op.Kind == TEXT("apply_stack_issue_fix"))
			{
				FNiagaraExt_ApplyStackIssueFixResult Result;
				UNiagaraExternalEditUtilities::ApplyStackIssueFix(&System, Op.IssueId, Op.FixId, Result, Context);
				if (!Result.bApplied && Context.Errors.Num() == ErrorsBefore)
				{
					Context.Error(LOCTEXT("FixNotApplied", "The stack issue fix was not applied."));
				}
			}
			else
			{
				Context.Error(LOCTEXT("UnsupportedOp", "Unsupported op kind."));
			}

			if (Context.Errors.Num() > ErrorsBefore)
			{
				OutPerOpStatus.Add(TEXT("failed"));
				OutError = FString::Printf(TEXT("Op %d (%s) failed: %s"), OutPerOpStatus.Num() - 1, *Op.Kind, *JoinErrors(Context, ErrorsBefore));
				if (OutAppliedCount == 0)
				{
					// The first op was refused before it changed anything: leave no empty step in the undo history.
					Transaction.Cancel();
				}
				return false;
			}
			OutPerOpStatus.Add(Status);
			++OutAppliedCount;
		}
		return true;
	}
}

#undef LOCTEXT_NAMESPACE
