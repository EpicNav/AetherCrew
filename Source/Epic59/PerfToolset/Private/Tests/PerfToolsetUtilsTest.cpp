// Copyright Epic Games, Inc. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonValue.h"
#include "Misc/AutomationTest.h"
#include "PerfToolsetUtils.h"

BEGIN_DEFINE_SPEC(FMakeMsValueSpec,
	"AIAssistant.Toolsets.PerfToolset.MakeMsValue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

	FString StoredString(float Ms)
	{
		FString Out;
		UE::PerfToolset::MakeMsValue(Ms)->TryGetString(Out);
		return Out;
	}

END_DEFINE_SPEC(FMakeMsValueSpec)

void FMakeMsValueSpec::Define()
{
	It(TEXT("formats round values cleanly"), [this]()
	{
		TestEqual(TEXT("16.5f -> '16.5'"), StoredString(16.5f), TEXT("16.5"));
	});

	It(TEXT("collapses float-precision noise to 4 significant digits"), [this]()
	{
		// The headline regression case: a JSON writer's default %.17g would
		// emit "16.500000076293945" for this exact bit pattern. %.4g keeps
		// the agent payload terse.
		TestEqual(TEXT("16.500000076f -> '16.5'"), StoredString(16.500000076f), TEXT("16.5"));
	});

	It(TEXT("renders zero as plain '0'"), [this]()
	{
		TestEqual(TEXT("0.f -> '0'"), StoredString(0.f), TEXT("0"));
	});

	It(TEXT("keeps sub-ms values in decimal form"), [this]()
	{
		// GPU pass timings are commonly fractions of a millisecond; %.4g
		// keeps them out of scientific notation for typical perf values.
		TestEqual(TEXT("0.05f -> '0.05'"), StoredString(0.05f), TEXT("0.05"));
	});

	It(TEXT("flags itself as EJson::Number so the writer emits it unquoted"), [this]()
	{
		// The JSON writer dispatches on FJsonValue::Type; FJsonValueNumberString
		// reports EJson::Number and emits its pre-formatted string verbatim.
		// If MakeMsValue were swapped for FJsonValueString the Type would be
		// EJson::String and the writer would emit "16.5" with quotes —
		// agent-side this stops reading as a number. (FJsonValueNumber would
		// keep the unquoted output but re-format at %.17g, reintroducing the
		// bloat we're avoiding; covered by the other It blocks here.)
		TestTrue(TEXT("Type == EJson::Number"),
			UE::PerfToolset::MakeMsValue(16.5f)->Type == EJson::Number);
	});
}

#endif // WITH_DEV_AUTOMATION_TESTS
