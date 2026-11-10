// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "ConsoleToolset.h"

#include "Dom/JsonObject.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "ToolsetRegistry/ToolCallExceptionHandler.h"

namespace
{
	TSharedPtr<FJsonObject> ParseJsonObject(const FString& JsonString)
	{
		TSharedPtr<FJsonObject> JsonObject;
		if (JsonString.IsEmpty())
		{
			return nullptr;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
		FJsonSerializer::Deserialize(Reader, JsonObject);
		return JsonObject;
	}

	template<typename CVarTypeT>
	struct FCVarTestFixture
	{
		FCVarTestFixture(const CVarTypeT& InitialValue) :
			CVarNamespace(TEXT("toolset.")),
			CVarName(FString::Printf(TEXT("%stest.Test%s"), *CVarNamespace, GetTypeSuffix())),
			CVar(*CVarName, InitialValue, TEXT("A test cvar."))
		{
			// TAutoConsoleVariable destruction leaves the object in the registry with
			// ECVF_Unregistered set, and re-registration of the same name/type keeps
			// the previous value (see FConsoleManager::AddConsoleObject). Force the
			// requested initial value so each test starts from a known state.
			CVar->AsVariable()->Set(*LexToString(InitialValue), ECVF_SetByConsole);
		}

		// Per-type suffix so fixtures of different types don't collide on the same name.
		// TAutoConsoleVariable's destructor leaves the object in the registry with
		// ECVF_Unregistered (bKeepCVarObject=true), and a same-name re-registration
		// with a different type returns the original — clobbering the new type.
		static const TCHAR* GetTypeSuffix()
		{
			if constexpr (std::is_same_v<CVarTypeT, bool>)    return TEXT("Bool");
			else if constexpr (std::is_same_v<CVarTypeT, int>)     return TEXT("Int");
			else if constexpr (std::is_same_v<CVarTypeT, float>)   return TEXT("Float");
			else if constexpr (std::is_same_v<CVarTypeT, FString>) return TEXT("String");
			else return TEXT("Other");
		}

		TSharedPtr<FJsonObject> FindCVarJsonData() const
		{
			TSharedPtr<FJsonObject> CVarJsonData =
				ParseJsonObject(UConsoleToolset::SearchCVars(CVarNamespace));
			if (!CVarJsonData)
			{
				return nullptr;
			}
			const TSharedPtr<FJsonObject>* CommandJson = nullptr;
			CVarJsonData->TryGetObjectField(CVarName, CommandJson);
			return CommandJson ? *CommandJson : nullptr;
		}

		FString CVarNamespace;
		FString CVarName;
		TAutoConsoleVariable<CVarTypeT> CVar;
	};
}

BEGIN_DEFINE_SPEC(FConsoleToolsetSpec, "AIAssistant.Toolsets.ConsoleToolset.ConsoleToolsetSpec",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
END_DEFINE_SPEC(FConsoleToolsetSpec)

void FConsoleToolsetSpec::Define()
{
	It(TEXT("Can find a bool cvar"), [this]()
	{
		FCVarTestFixture<bool> Fixture(true);
		TSharedPtr<FJsonObject> CommandJson = Fixture.FindCVarJsonData();
		TestTrue("CVar", CommandJson.IsValid());
		if (!CommandJson)
		{
			return;
		}
		TestEqual("CVar", CommandJson->GetStringField(
			FString(TEXT("help"))), Fixture.CVar->GetHelp());
		bool Value = false;
		TestTrue("CVar", CommandJson->TryGetBoolField(
			FString(TEXT("value")), Value));
		TestEqual("CVar", Value, Fixture.CVar->AsVariable()->GetBool());
	});

	It(TEXT("Can find an int cvar"), [this]()
	{
		FCVarTestFixture<int> Fixture(5);
		TSharedPtr<FJsonObject> CommandJson = Fixture.FindCVarJsonData();
		TestTrue("CVar", CommandJson.IsValid());
		if (!CommandJson)
		{
			return;
		}
		TestEqual("CVar", CommandJson->GetStringField(
			FString(TEXT("help"))), Fixture.CVar->GetHelp());
		int32 Value = 0;
		TestTrue("CVar", CommandJson->TryGetNumberField(FString(TEXT("value")), Value));
		TestEqual("CVar", Value, Fixture.CVar->AsVariable()->GetInt());
	});

	It(TEXT("Can find an float cvar"), [this]()
	{
		FCVarTestFixture<float> Fixture(5.0);
		TSharedPtr<FJsonObject> CommandJson = Fixture.FindCVarJsonData();
		TestTrue("CVar", CommandJson.IsValid());
		if (!CommandJson)
		{
			return;
		}
		TestEqual("CVar", CommandJson->GetStringField(
			FString(TEXT("help"))), Fixture.CVar->GetHelp());
		float Value = 0;
		TestTrue("CVar", CommandJson->TryGetNumberField(
			FString(TEXT("value")), Value));
		TestEqual("CVar", Value, Fixture.CVar->AsVariable()->GetFloat());
	});

	It(TEXT("Can find an string cvar"), [this]()
	{
		FCVarTestFixture<FString> Fixture(TEXT("testy"));
		TSharedPtr<FJsonObject> CommandJson = Fixture.FindCVarJsonData();
		TestTrue("CVar", CommandJson.IsValid());
		if (!CommandJson)
		{
			return;
		}
		TestEqual("CVar", CommandJson->GetStringField(
			FString(TEXT("help"))), Fixture.CVar->GetHelp());
		FString Value;
		TestTrue("CVar", CommandJson->TryGetStringField(
			FString(TEXT("value")), Value));
		TestEqual("CVar", Value, Fixture.CVar->AsVariable()->GetString());
	});

	It(TEXT("Can get default values from cvars"), [this]()
	{
		IConsoleManager& ConsoleManager = IConsoleManager::Get();
		FString CVarName(TEXT("toolset.test.TestBit"));
		static uint8 Force0Mask[1] = { 0 };
		static uint8 Force1Mask[1] = { 0 };
		const uint32 BitNumber = 0;
		IConsoleVariable* CVar = ConsoleManager.RegisterConsoleVariableBitRef(
			*CVarName, TEXT("MyFlag"), BitNumber, Force0Mask, Force1Mask, TEXT("Test"));
		TestTrue("CVar", !UConsoleToolset::SearchCVars(CVarName).IsEmpty());
		ConsoleManager.UnregisterConsoleObject(CVar);
	});


	It(TEXT("Skips disabled cvar"), [this]()
	{
		FString CVarName(TEXT("toolset.test.TestDisabled"));
		TAutoConsoleVariable<bool> CVar(*CVarName, true, TEXT("A disabled cvar."), ECVF_Unregistered);
		FString CVarJsonString = UConsoleToolset::SearchCVars(CVarName);
		TSharedPtr<FJsonObject> CVarJsonData = ParseJsonObject(CVarJsonString);
		TestTrue("CVar", CVarJsonData.IsValid());
		TestEqual("CVar", CVarJsonData->Values.Num(), 0);
	});

	It(TEXT("GetCVarValue returns the value of a bool cvar"), [this]()
	{
		FCVarTestFixture<bool> Fixture(true);
		TestEqual("Value", UConsoleToolset::GetCVarValue(Fixture.CVarName), FString(TEXT("true")));
	});

	It(TEXT("GetCVarValue returns the value of an int cvar"), [this]()
	{
		FCVarTestFixture<int> Fixture(42);
		TestEqual("Value", UConsoleToolset::GetCVarValue(Fixture.CVarName), FString(TEXT("42")));
	});

	It(TEXT("GetCVarValue returns the value of a float cvar"), [this]()
	{
		FCVarTestFixture<float> Fixture(1.5f);
		TestEqual("Value", UConsoleToolset::GetCVarValue(Fixture.CVarName), FString(TEXT("1.5")));
	});

	It(TEXT("GetCVarValue returns the value of a string cvar"), [this]()
	{
		FCVarTestFixture<FString> Fixture(TEXT("hello"));
		TestEqual("Value", UConsoleToolset::GetCVarValue(Fixture.CVarName), FString(TEXT("hello")));
	});

	It(TEXT("GetCVarValue raises a script error for a non-existent cvar"), [this]()
	{
		UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
		Handler.CaptureErrorsIn([]
		{
			UConsoleToolset::GetCVarValue(TEXT("toolset.test.DoesNotExist"));
		});
		TestTrue("Script error raised", Handler.GetException().Contains(TEXT("not found")));
	});

	It(TEXT("SetCVarValue updates a bool cvar"), [this]()
	{
		FCVarTestFixture<bool> Fixture(false);
		UConsoleToolset::SetCVarValue(Fixture.CVarName, TEXT("true"));
		TestEqual("Value", Fixture.CVar->AsVariable()->GetBool(), true);
	});

	It(TEXT("SetCVarValue updates an int cvar"), [this]()
	{
		FCVarTestFixture<int> Fixture(0);
		UConsoleToolset::SetCVarValue(Fixture.CVarName, TEXT("123"));
		TestEqual("Value", Fixture.CVar->AsVariable()->GetInt(), 123);
	});

	It(TEXT("SetCVarValue updates an int cvar with a negative value"), [this]()
	{
		FCVarTestFixture<int> Fixture(0);
		UConsoleToolset::SetCVarValue(Fixture.CVarName, TEXT("-7"));
		TestEqual("Value", Fixture.CVar->AsVariable()->GetInt(), -7);
	});

	It(TEXT("SetCVarValue raises a script error for hex or octal text on an int cvar"), [this]()
	{
		FCVarTestFixture<int> Fixture(5);
		for (const TCHAR* Text : { TEXT("0x10"), TEXT("010") })
		{
			UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
			Handler.CaptureErrorsIn([&Fixture, Text]
			{
				UConsoleToolset::SetCVarValue(Fixture.CVarName, Text);
			});
			TestTrue(FString::Printf(TEXT("Script error raised for %s"), Text), Handler.GetException().Contains(TEXT("Expected int")));
			TestEqual(FString::Printf(TEXT("Value unchanged for %s"), Text), Fixture.CVar->AsVariable()->GetInt(), 5);
		}
	});

	It(TEXT("SetCVarValue updates a float cvar"), [this]()
	{
		FCVarTestFixture<float> Fixture(0.f);
		UConsoleToolset::SetCVarValue(Fixture.CVarName, TEXT("2.5"));
		TestEqual("Value", Fixture.CVar->AsVariable()->GetFloat(), 2.5f);
	});

	It(TEXT("SetCVarValue updates a string cvar"), [this]()
	{
		FCVarTestFixture<FString> Fixture(TEXT("before"));
		UConsoleToolset::SetCVarValue(Fixture.CVarName, TEXT("after"));
		TestEqual("Value", Fixture.CVar->AsVariable()->GetString(), FString(TEXT("after")));
	});

	It(TEXT("SetCVarValue raises a script error for a non-existent cvar"), [this]()
	{
		UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
		Handler.CaptureErrorsIn([]
		{
			UConsoleToolset::SetCVarValue(TEXT("toolset.test.DoesNotExist"), TEXT("1"));
		});
		TestTrue("Script error raised", Handler.GetException().Contains(TEXT("not found")));
	});

	It(TEXT("SetCVarValue raises a script error for a read-only cvar"), [this]()
	{
		FString CVarName(TEXT("toolset.test.TestReadOnly"));
		TAutoConsoleVariable<int> CVar(*CVarName, 7, TEXT("A read-only cvar."), ECVF_ReadOnly);
		UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
		Handler.CaptureErrorsIn([&CVarName]
		{
			UConsoleToolset::SetCVarValue(CVarName, TEXT("9"));
		});
		TestTrue("Script error raised", Handler.GetException().Contains(TEXT("read-only")));
		// Underlying value is unchanged.
		TestEqual("Value", CVar->AsVariable()->GetInt(), 7);
	});

	It(TEXT("SetCVarValue rejects non-bool input for a bool cvar"), [this]()
	{
		FCVarTestFixture<bool> Fixture(true);
		UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
		Handler.CaptureErrorsIn([&Fixture]
		{
			UConsoleToolset::SetCVarValue(Fixture.CVarName, TEXT("yes"));
		});
		TestTrue("Script error mentions bool", Handler.GetException().Contains(TEXT("bool")));
		// Underlying value is unchanged.
		TestEqual("Value", Fixture.CVar->AsVariable()->GetBool(), true);
	});

	It(TEXT("SetCVarValue accepts numeric forms for a bool cvar"), [this]()
	{
		FCVarTestFixture<bool> Fixture(true);
		UConsoleToolset::SetCVarValue(Fixture.CVarName, TEXT("0"));
		TestEqual("0 sets false", Fixture.CVar->AsVariable()->GetBool(), false);
		UConsoleToolset::SetCVarValue(Fixture.CVarName, TEXT("1"));
		TestEqual("1 sets true", Fixture.CVar->AsVariable()->GetBool(), true);
	});

	It(TEXT("SetCVarValue rejects non-int input for an int cvar"), [this]()
	{
		FCVarTestFixture<int> Fixture(42);
		UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
		Handler.CaptureErrorsIn([&Fixture]
		{
			UConsoleToolset::SetCVarValue(Fixture.CVarName, TEXT("abc"));
		});
		TestTrue("Script error mentions int", Handler.GetException().Contains(TEXT("int")));
		// Underlying value is unchanged (would silently coerce to 0 without validation).
		TestEqual("Value", Fixture.CVar->AsVariable()->GetInt(), 42);
	});

	It(TEXT("SetCVarValue rejects non-float input for a float cvar"), [this]()
	{
		FCVarTestFixture<float> Fixture(1.5f);
		UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
		Handler.CaptureErrorsIn([&Fixture]
		{
			UConsoleToolset::SetCVarValue(Fixture.CVarName, TEXT("abc"));
		});
		TestTrue("Script error mentions float", Handler.GetException().Contains(TEXT("float")));
		// Underlying value is unchanged.
		TestEqual("Value", Fixture.CVar->AsVariable()->GetFloat(), 1.5f);
	});

	It(TEXT("RunConsoleCommand runs a console command"), [this]()
	{
		FCVarTestFixture<int> Fixture(1);
		UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
		Handler.CaptureErrorsIn([&Fixture]
		{
			UConsoleToolset::RunConsoleCommand(FString::Printf(TEXT("%s 7"), *Fixture.CVarName));
		});
		TestTrue("No script error", Handler.GetException().IsEmpty());
		TestEqual("Value", Fixture.CVar->AsVariable()->GetInt(), 7);
	});

	It(TEXT("RunConsoleCommand refuses Python, in any command of the line"), [this]()
	{
		FCVarTestFixture<int> Fixture(1);
		const FString SetCVar = FString::Printf(TEXT("%s 7"), *Fixture.CVarName);
		const FString Commands[] = {
			TEXT("py print('x')"),
			TEXT("PY print('x')"),
			TEXT("  py print('x')"),
			TEXT("py \"C:/script.py\""),
			SetCVar + TEXT(" | py print('x')"),
			SetCVar + TEXT("\npy print('x')"),
			// Commands that wrap another command must not smuggle Python past the check.
			TEXT("py"),
			SetCVar + TEXT(" script.py"),
			SetCVar + TEXT("|py print('x')"),
			TEXT("ke * py print('x')"),
			SetCVar + TEXT(" \"quoted py here\""),
			TEXT("defer py print('x')"),
			TEXT("DEFER   PY print('x')"),
			TEXT("defer defer py print('x')"),
			SetCVar + TEXT(" | defer py print('x')"),
			TEXT("redirecttofile out.txt py print('x')"),
			TEXT("RedirectToFile \"out dir/out.txt\" py print('x')"),
			TEXT("redirecttofile out.txt defer py print('x')"),
		};
		for (const FString& Command : Commands)
		{
			UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
			Handler.CaptureErrorsIn([&Command]
			{
				UConsoleToolset::RunConsoleCommand(Command);
			});
			TestTrue(*FString::Printf(TEXT("Refused: %s"), *Command), Handler.GetException().Contains(TEXT("Python scripting tool")));
		}
		// A refused line runs none of its commands.
		TestEqual("Value", Fixture.CVar->AsVariable()->GetInt(), 1);
	});

	It(TEXT("RunConsoleCommand doesn't refuse a command that only contains py as part of another word"), [this]()
	{
		FCVarTestFixture<FString> Fixture(TEXT(""));
		const FString Values[] = {
			TEXT("copy"),
			TEXT("py2"),
		};
		for (const FString& Value : Values)
		{
			UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
			Handler.CaptureErrorsIn([&Fixture, &Value]
			{
				UConsoleToolset::RunConsoleCommand(FString::Printf(TEXT("%s %s"), *Fixture.CVarName, *Value));
			});
			TestTrue(*FString::Printf(TEXT("No script error: %s"), *Value), Handler.GetException().IsEmpty());
			TestEqual(*FString::Printf(TEXT("Value set: %s"), *Value), Fixture.CVar->AsVariable()->GetString(), Value);
		}
	});

	It(TEXT("RunConsoleCommand refuses EXEC, in any command of the line"), [this]()
	{
		FCVarTestFixture<int> Fixture(1);
		const FString SetCVar = FString::Printf(TEXT("%s 7"), *Fixture.CVarName);
		const FString Commands[] = {
			TEXT("exec script.txt"),
			TEXT("EXEC script.txt"),
			TEXT("  exec script.txt"),
			TEXT("exec"),
			SetCVar + TEXT(" | exec script.txt"),
			SetCVar + TEXT("\nexec script.txt"),
			TEXT("defer exec script.txt"),
			TEXT("redirecttofile out.txt exec script.txt"),
		};
		for (const FString& Command : Commands)
		{
			UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
			Handler.CaptureErrorsIn([&Command]
			{
				UConsoleToolset::RunConsoleCommand(Command);
			});
			TestTrue(*FString::Printf(TEXT("Refused: %s"), *Command), Handler.GetException().Contains(TEXT("EXEC isn't supported")));
		}
		// A refused line runs none of its commands.
		TestEqual("Value", Fixture.CVar->AsVariable()->GetInt(), 1);
	});

	It(TEXT("RunConsoleCommand doesn't refuse a command that only contains exec as part of another word"), [this]()
	{
		FCVarTestFixture<FString> Fixture(TEXT(""));
		const FString Values[] = {
			TEXT("executor"),
			TEXT("exec2"),
		};
		for (const FString& Value : Values)
		{
			UE::ToolsetRegistry::FToolCallExceptionHandler Handler;
			Handler.CaptureErrorsIn([&Fixture, &Value]
			{
				UConsoleToolset::RunConsoleCommand(FString::Printf(TEXT("%s %s"), *Fixture.CVarName, *Value));
			});
			TestTrue(*FString::Printf(TEXT("No script error: %s"), *Value), Handler.GetException().IsEmpty());
			TestEqual(*FString::Printf(TEXT("Value set: %s"), *Value), Fixture.CVar->AsVariable()->GetString(), Value);
		}
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
