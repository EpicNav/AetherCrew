// Games by Hyper 2026.

#include "HyperAIStudioAutoApprovalKeepOn.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HyperAIStudioEditorRestart.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
#include <wincrypt.h>
#include <dpapi.h>
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
#endif

#define LOCTEXT_NAMESPACE "HyperAIStudioAutoApproval"

namespace HyperAIStudio::AutoApproval::KeepOn
{
	namespace
	{
		constexpr const TCHAR* Format = TEXT("hyperai.autoapproval.keepon/1");
		/** Bound into the DPAPI entropy with the project path: a blob sealed by another program for another purpose fails. */
		constexpr const TCHAR* EntropyPrefix = TEXT("HyperAIStudio.AutoApproval.KeepOn/1|");
		/** How far a timed record's expiry may be from its turned-on time plus its duration. */
		constexpr double ExpiryToleranceSeconds = 2.0;

		constexpr EHyperAIStudioApprovalCategory Categories[] = {
			EHyperAIStudioApprovalCategory::Edits,
			EHyperAIStudioApprovalCategory::Scripts,
			EHyperAIStudioApprovalCategory::Destructive,
			EHyperAIStudioApprovalCategory::ExternalEffects,
			EHyperAIStudioApprovalCategory::SandboxSaves};

		uint8 Bit(const EHyperAIStudioApprovalCategory Category)
		{
			return static_cast<uint8>(1u << static_cast<uint8>(Category));
		}

		uint8 AllBits()
		{
			uint8 Mask = 0;
			for (const EHyperAIStudioApprovalCategory Category : Categories)
			{
				Mask |= Bit(Category);
			}
			return Mask;
		}

		FString DurationWords(const EHyperAIStudioAutoApprovalDuration Duration, const double CustomHours)
		{
			switch (Duration)
			{
			case EHyperAIStudioAutoApprovalDuration::FifteenMinutes: return TEXT("15 minutes");
			case EHyperAIStudioAutoApprovalDuration::FourHours: return TEXT("4 hours");
			case EHyperAIStudioAutoApprovalDuration::EightHours: return TEXT("8 hours");
			case EHyperAIStudioAutoApprovalDuration::Custom: return FString::Printf(TEXT("%g hours"), CustomHours);
			case EHyperAIStudioAutoApprovalDuration::UntilEditorCloses: return TEXT("until the editor closes");
			default: return TEXT("1 hour");
			}
		}

		FString Iso(const FDateTime& Utc)
		{
			return Utc.ToIso8601();
		}

		bool ParseTime(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, FDateTime& Out)
		{
			FString Text;
			return Object->TryGetStringField(Field, Text) && FDateTime::ParseIso8601(*Text, Out);
		}

		bool ParseCount(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, int32& Out)
		{
			double Value = 0.0;
			if (!Object->TryGetNumberField(Field, Value) || Value != FMath::FloorToDouble(Value) || FMath::Abs(Value) > 1.0e6)
			{
				return false;
			}
			Out = static_cast<int32>(Value);
			return true;
		}

		FString Entropy(const FString& ProjectPath)
		{
			return EntropyPrefix + ProjectPath;
		}

		/** Minutes, rounded, for the log. */
		int32 Minutes(const double Seconds)
		{
			return FMath::RoundToInt(Seconds / 60.0);
		}
	}

	double CloseWindowSeconds()
	{
		return FHyperAIStudioEditorRestart::AutoResumeWindowSeconds;
	}

	const TCHAR* DurationKey(const EHyperAIStudioAutoApprovalDuration Duration)
	{
		switch (Duration)
		{
		case EHyperAIStudioAutoApprovalDuration::FifteenMinutes: return TEXT("fifteen_minutes");
		case EHyperAIStudioAutoApprovalDuration::FourHours: return TEXT("four_hours");
		case EHyperAIStudioAutoApprovalDuration::EightHours: return TEXT("eight_hours");
		case EHyperAIStudioAutoApprovalDuration::Custom: return TEXT("custom");
		case EHyperAIStudioAutoApprovalDuration::UntilEditorCloses: return TEXT("until_editor_closes");
		default: return TEXT("one_hour");
		}
	}

	bool ParseDuration(const FString& Key, EHyperAIStudioAutoApprovalDuration& OutDuration)
	{
		for (const EHyperAIStudioAutoApprovalDuration Duration : {EHyperAIStudioAutoApprovalDuration::FifteenMinutes,
			EHyperAIStudioAutoApprovalDuration::OneHour, EHyperAIStudioAutoApprovalDuration::FourHours,
			EHyperAIStudioAutoApprovalDuration::EightHours, EHyperAIStudioAutoApprovalDuration::Custom,
			EHyperAIStudioAutoApprovalDuration::UntilEditorCloses})
		{
			if (Key == DurationKey(Duration))
			{
				OutDuration = Duration;
				return true;
			}
		}
		return false;
	}

	FString CurrentProject()
	{
		FString Path = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		FPaths::NormalizeFilename(Path);
		return Path.ToLower();
	}

	FString CurrentUser()
	{
		return FPlatformProcess::UserName(/*bOnlyAlphaNumeric=*/false);
	}

	FString Fingerprint(const FString& SpendSettings)
	{
		const FTCHARToUTF8 Utf8(*SpendSettings);
		return FSHA1::HashBuffer(Utf8.Get(), Utf8.Length()).ToString();
	}

	FString DefaultPath()
	{
		// The user's name is hashed so the file name is safe whatever characters it holds.
		const FString User = Fingerprint(CurrentUser().ToLower()).Left(16).ToLower();
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/AutoApproval") / (User + TEXT(".keepon")));
	}

	FString ToJson(const FRecord& Record)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("format"), Format);
		TArray<TSharedPtr<FJsonValue>> Names;
		for (const EHyperAIStudioApprovalCategory Category : Categories)
		{
			if (Record.CategoryMask & Bit(Category))
			{
				Names.Add(MakeShared<FJsonValueString>(LexToString(Category)));
			}
		}
		Object->SetArrayField(TEXT("categories"), Names);
		Object->SetStringField(TEXT("duration"), DurationKey(Record.Duration));
		if (Record.Duration == EHyperAIStudioAutoApprovalDuration::Custom)
		{
			Object->SetNumberField(TEXT("customHours"), Record.CustomHours);
		}
		Object->SetBoolField(TEXT("keepOnAfterRestart"), true);
		Object->SetStringField(TEXT("turnedOnUtc"), Iso(Record.TurnedOnUtc));
		if (Record.ExpiresUtc.IsSet())
		{
			Object->SetStringField(TEXT("expiresUtc"), Iso(*Record.ExpiresUtc));
		}
		Object->SetNumberField(TEXT("approved"), Record.Approved);
		Object->SetNumberField(TEXT("externalEffects"), Record.ExternalApproved);
		Object->SetNumberField(TEXT("failuresInARow"), Record.ConsecutiveFailures);
		Object->SetNumberField(TEXT("sandboxWrites"), Record.SandboxWrites);
		Object->SetStringField(TEXT("spendFingerprint"), Record.SpendFingerprint);
		Object->SetStringField(TEXT("project"), Record.ProjectPath);
		Object->SetStringField(TEXT("windowsUser"), Record.WindowsUser);
		Object->SetStringField(TEXT("writtenUtc"), Iso(Record.WrittenUtc));
		if (Record.ClosedUtc.IsSet())
		{
			Object->SetStringField(TEXT("closedUtc"), Iso(*Record.ClosedUtc));
		}
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return Text;
	}

	bool FromJson(const FString& Json, FRecord& OutRecord)
	{
		TSharedPtr<FJsonObject> Object;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) || !Object.IsValid())
		{
			return false;
		}
		FString Value;
		bool bKeepOn = false;
		if (!Object->TryGetStringField(TEXT("format"), Value) || Value != Format
			|| !Object->TryGetBoolField(TEXT("keepOnAfterRestart"), bKeepOn) || !bKeepOn)
		{
			return false;
		}
		FRecord Record;
		const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
		if (!Object->TryGetArrayField(TEXT("categories"), Names))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Name : *Names)
		{
			const EHyperAIStudioApprovalCategory* Found = nullptr;
			const FString Text = Name.IsValid() ? Name->AsString() : FString();
			for (const EHyperAIStudioApprovalCategory& Category : Categories)
			{
				Found = Text == LexToString(Category) ? &Category : Found;
			}
			if (!Found)
			{
				return false;
			}
			Record.CategoryMask |= Bit(*Found);
		}
		if (!Object->TryGetStringField(TEXT("duration"), Value) || !ParseDuration(Value, Record.Duration)
			|| (Record.Duration == EHyperAIStudioAutoApprovalDuration::Custom && !Object->TryGetNumberField(TEXT("customHours"), Record.CustomHours))
			|| !ParseTime(Object, TEXT("turnedOnUtc"), Record.TurnedOnUtc)
			|| !ParseTime(Object, TEXT("writtenUtc"), Record.WrittenUtc)
			|| !ParseCount(Object, TEXT("approved"), Record.Approved)
			|| !ParseCount(Object, TEXT("externalEffects"), Record.ExternalApproved)
			|| !ParseCount(Object, TEXT("failuresInARow"), Record.ConsecutiveFailures)
			|| !ParseCount(Object, TEXT("sandboxWrites"), Record.SandboxWrites)
			|| !Object->TryGetStringField(TEXT("spendFingerprint"), Record.SpendFingerprint)
			|| !Object->TryGetStringField(TEXT("project"), Record.ProjectPath)
			|| !Object->TryGetStringField(TEXT("windowsUser"), Record.WindowsUser))
		{
			return false;
		}
		FDateTime Time;
		if (Object->HasField(TEXT("expiresUtc")))
		{
			if (!ParseTime(Object, TEXT("expiresUtc"), Time))
			{
				return false;
			}
			Record.ExpiresUtc = Time;
		}
		if (Object->HasField(TEXT("closedUtc")))
		{
			if (!ParseTime(Object, TEXT("closedUtc"), Time))
			{
				return false;
			}
			Record.ClosedUtc = Time;
		}
		OutRecord = MoveTemp(Record);
		return true;
	}

	bool Protect(const TArray<uint8>& Plain, const FString& ProjectPath, TArray<uint8>& OutSealed, FString& OutError)
	{
		OutSealed.Reset();
#if PLATFORM_WINDOWS
		const FTCHARToUTF8 Salt(*Entropy(ProjectPath));
		DATA_BLOB In;
		In.cbData = static_cast<DWORD>(Plain.Num());
		In.pbData = const_cast<BYTE*>(Plain.GetData());
		DATA_BLOB Extra;
		Extra.cbData = static_cast<DWORD>(Salt.Length());
		Extra.pbData = reinterpret_cast<BYTE*>(const_cast<ANSICHAR*>(Salt.Get()));
		DATA_BLOB Out = {0, nullptr};
		if (!::CryptProtectData(&In, L"HyperAI Studio auto-approval", &Extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Out))
		{
			OutError = FString::Printf(TEXT("Windows could not protect the record (CryptProtectData error %u)."), ::GetLastError());
			return false;
		}
		OutSealed.Append(Out.pbData, static_cast<int32>(Out.cbData));
		::LocalFree(Out.pbData);
		return true;
#else
		OutError = TEXT("Keeping auto-approval on after a restart needs Windows.");
		return false;
#endif
	}

	bool Unprotect(const TArray<uint8>& Sealed, const FString& ProjectPath, TArray<uint8>& OutPlain, FString& OutError)
	{
		OutPlain.Reset();
#if PLATFORM_WINDOWS
		const FTCHARToUTF8 Salt(*Entropy(ProjectPath));
		DATA_BLOB In;
		In.cbData = static_cast<DWORD>(Sealed.Num());
		In.pbData = const_cast<BYTE*>(Sealed.GetData());
		DATA_BLOB Extra;
		Extra.cbData = static_cast<DWORD>(Salt.Length());
		Extra.pbData = reinterpret_cast<BYTE*>(const_cast<ANSICHAR*>(Salt.Get()));
		DATA_BLOB Out = {0, nullptr};
		if (Sealed.IsEmpty() || !::CryptUnprotectData(&In, nullptr, &Extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &Out))
		{
			OutError = FString::Printf(TEXT("CryptUnprotectData error %u"), Sealed.IsEmpty() ? 0u : ::GetLastError());
			return false;
		}
		OutPlain.Append(Out.pbData, static_cast<int32>(Out.cbData));
		::LocalFree(Out.pbData);
		return true;
#else
		OutError = TEXT("Keeping auto-approval on after a restart needs Windows.");
		return false;
#endif
	}

	bool Write(const FString& Path, const FRecord& Record, FString& OutError)
	{
		OutError.Reset();
		const FTCHARToUTF8 Utf8(*ToJson(Record));
		TArray<uint8> Plain;
		Plain.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		TArray<uint8> Sealed;
		if (!Protect(Plain, Record.ProjectPath, Sealed, OutError))
		{
			Delete(Path);
			return false;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		if (!FFileHelper::SaveArrayToFile(Sealed, *Path))
		{
			// Never an older count left behind: a record that cannot be kept current is no record.
			Delete(Path);
			OutError = FString::Printf(TEXT("Could not write %s."), *Path);
			return false;
		}
		return true;
	}

	ERefusal Read(const FString& Path, const FContext& Context, FRecord& OutRecord)
	{
		OutRecord = FRecord();
		if (!IFileManager::Get().FileExists(*Path))
		{
			return ERefusal::NoRecord;
		}
		TArray<uint8> Sealed;
		if (!FFileHelper::LoadFileToArray(Sealed, *Path) || Sealed.IsEmpty())
		{
			return ERefusal::Unreadable;
		}
		TArray<uint8> Plain;
		FString Error;
		if (!Unprotect(Sealed, Context.ProjectPath, Plain, Error))
		{
			return ERefusal::DoesNotDecrypt;
		}
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Plain.GetData()), Plain.Num());
		if (!FromJson(FString(Text.Length(), Text.Get()), OutRecord))
		{
			return ERefusal::Unparsable;
		}
		return Check(OutRecord, Context);
	}

	ERefusal Check(const FRecord& Record, const FContext& Context)
	{
		if (!Record.ProjectPath.Equals(Context.ProjectPath, ESearchCase::IgnoreCase))
		{
			return ERefusal::WrongProject;
		}
		if (!Record.WindowsUser.Equals(Context.WindowsUser, ESearchCase::IgnoreCase))
		{
			return ERefusal::WrongUser;
		}
		const bool bTimed = Record.Duration != EHyperAIStudioAutoApprovalDuration::UntilEditorCloses;
		if (Record.CategoryMask == 0 || (Record.CategoryMask & ~AllBits()) != 0
			|| Record.Approved < 0 || Record.ExternalApproved < 0 || Record.ConsecutiveFailures < 0 || Record.SandboxWrites < 0
			|| Record.ExternalApproved > Record.Approved || Record.SandboxWrites > Record.Approved
			|| bTimed != Record.ExpiresUtc.IsSet() || Record.SpendFingerprint.IsEmpty()
			|| Record.WrittenUtc < Record.TurnedOnUtc
			|| (Record.ClosedUtc.IsSet() && *Record.ClosedUtc < Record.TurnedOnUtc))
		{
			return ERefusal::Inconsistent;
		}
		// A custom duration past the field's limits is not one the dialog can give, so it is never read as the nearest it could.
		if (Record.Duration == EHyperAIStudioAutoApprovalDuration::Custom
			&& !(Record.CustomHours >= HyperAIStudio::AutoApproval::MinCustomHours && Record.CustomHours <= HyperAIStudio::AutoApproval::MaxCustomHours))
		{
			return ERefusal::Inconsistent;
		}
		if (bTimed && FMath::Abs((*Record.ExpiresUtc - Record.TurnedOnUtc).GetTotalSeconds() - HyperAIStudio::AutoApproval::DurationSeconds(Record.Duration, Record.CustomHours)) > ExpiryToleranceSeconds)
		{
			return ERefusal::Inconsistent;
		}
		const FTimespan Skew = FTimespan::FromSeconds(ClockSkewSeconds);
		if (Record.TurnedOnUtc > Context.NowUtc + Skew || Record.WrittenUtc > Context.NowUtc + Skew
			|| (Record.ClosedUtc.IsSet() && *Record.ClosedUtc > Context.NowUtc + Skew))
		{
			return ERefusal::FromTheFuture;
		}
		if ((Context.NowUtc - Record.TurnedOnUtc).GetTotalSeconds() >= MaxAgeSeconds)
		{
			return ERefusal::TooOld;
		}
		if (bTimed && Context.NowUtc >= *Record.ExpiresUtc)
		{
			return ERefusal::Expired;
		}
		// After a crash nothing recorded the close; the last write is the latest the editor is known to have run, so the
		// time since then is at least as long as the editor was closed.
		const FDateTime Closed = Record.ClosedUtc.Get(Record.WrittenUtc);
		if (!bTimed && (Context.NowUtc - Closed).GetTotalSeconds() >= CloseWindowSeconds())
		{
			return ERefusal::ClosedTooLong;
		}
		if (Record.SpendFingerprint != Context.SpendFingerprint)
		{
			return ERefusal::SpendSettingsChanged;
		}
		if (Record.Approved >= FHyperAIStudioAutoApproval::GetMaxApprovals()
			|| Record.ExternalApproved >= FHyperAIStudioAutoApproval::MaxExternalEffects
			|| Record.ConsecutiveFailures >= FHyperAIStudioAutoApproval::MaxConsecutiveFailures)
		{
			return ERefusal::LimitReached;
		}
		return ERefusal::None;
	}

	bool Delete(const FString& Path)
	{
		return !Path.IsEmpty() && IFileManager::Get().FileExists(*Path)
			&& IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
	}

	FString DescribeRefusal(const ERefusal Refusal, const FRecord& Record, const FContext& Context)
	{
		switch (Refusal)
		{
		case ERefusal::None: return FString();
		case ERefusal::NoRecord: return TEXT("there is no record of one kept on");
		case ERefusal::Unreadable: return TEXT("its record could not be read");
		case ERefusal::DoesNotDecrypt:
			return TEXT("its record does not decrypt for this Windows user and project: it was changed, written as plain text, or sealed for another user or project");
		case ERefusal::Unparsable: return TEXT("its record decrypted but is not a complete auto-approval record");
		case ERefusal::WrongProject: return FString::Printf(TEXT("its record names another project (%s)"), *Record.ProjectPath);
		case ERefusal::WrongUser: return FString::Printf(TEXT("its record names another Windows user (%s)"), *Record.WindowsUser);
		case ERefusal::Inconsistent: return TEXT("its record contradicts itself (its categories, counts, duration or times)");
		case ERefusal::FromTheFuture: return TEXT("its record has times later than now; the clock may have been set back");
		case ERefusal::TooOld:
			return FString::Printf(TEXT("it was turned on %.1f hours ago, and a kept-on auto-approval never comes back 24 hours or more after it was turned on"),
				(Context.NowUtc - Record.TurnedOnUtc).GetTotalHours());
		case ERefusal::Expired:
			return FString::Printf(TEXT("its %s ran out at %s UTC, counting the time the editor was closed"),
				*DurationWords(Record.Duration, Record.CustomHours), Record.ExpiresUtc.IsSet() ? *Record.ExpiresUtc->ToString(TEXT("%Y-%m-%d %H:%M")) : TEXT("?"));
		case ERefusal::ClosedTooLong:
			return Record.ClosedUtc.IsSet()
				? FString::Printf(TEXT("it lasted until the editor closed, and the editor stayed closed %d min; it comes back only if the editor opens again within %d min"),
					Minutes((Context.NowUtc - *Record.ClosedUtc).GetTotalSeconds()), Minutes(CloseWindowSeconds()))
				: FString::Printf(TEXT("it lasted until the editor closed, the editor did not close cleanly, and its record was last written %d min ago; it comes back only within %d min"),
					Minutes((Context.NowUtc - Record.WrittenUtc).GetTotalSeconds()), Minutes(CloseWindowSeconds()));
		case ERefusal::SpendSettingsChanged:
			return TEXT("the spend cap, an external service, an image model or the approval limit changed in HyperAI Studio's settings since it was turned on");
		case ERefusal::LimitReached: return TEXT("it had already reached one of its limits (approvals, external effects or failures in a row)");
		default: return TEXT("its record was refused");
		}
	}

	FText DescribeRefusalText(const ERefusal Refusal)
	{
		switch (Refusal)
		{
		case ERefusal::TooOld: return LOCTEXT("KeepOnTooOld", "it was turned on 24 hours ago or more");
		case ERefusal::Expired: return LOCTEXT("KeepOnExpired", "its time ran out while the editor was closed");
		case ERefusal::ClosedTooLong: return LOCTEXT("KeepOnClosedTooLong", "it lasted until the editor closed, and the editor stayed closed 30 minutes or more");
		case ERefusal::SpendSettingsChanged: return LOCTEXT("KeepOnSpendChanged", "the spend cap, an external service, an image model or the approval limit changed since it was turned on");
		case ERefusal::LimitReached: return LOCTEXT("KeepOnLimitReached", "it had already reached one of its limits");
		case ERefusal::FromTheFuture: return LOCTEXT("KeepOnFuture", "its saved times are later than now; the clock may have been set back");
		default: return LOCTEXT("KeepOnInvalid", "its saved record was not valid for you and this project (it was changed, or written by something else)");
		}
	}

	bool IsSuspicious(const ERefusal Refusal)
	{
		switch (Refusal)
		{
		case ERefusal::Unreadable:
		case ERefusal::DoesNotDecrypt:
		case ERefusal::Unparsable:
		case ERefusal::WrongProject:
		case ERefusal::WrongUser:
		case ERefusal::Inconsistent:
			return true;
		default:
			return false;
		}
	}
}

#undef LOCTEXT_NAMESPACE
