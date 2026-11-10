// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioApprovalCategory.h"
#include "HyperAIStudioAutoApproval.h"

/**
 * "Stay on after the editor restarts": the record a kept-on auto-approval leaves under the project's Saved folder, and
 * the checks an editor makes before it brings one back.
 *
 * The record is JSON sealed with Windows DPAPI (CryptProtectData) for this Windows user, with entropy bound to HyperAI
 * and this project's path, so a plain file write can neither create nor change one: whatever does not decrypt, parse
 * and name this project and user is refused. A program running as the same Windows user can still call DPAPI itself
 * and forge one, so this raises the bar; it is not a sandbox. Nothing here is reflected, configured or registered.
 */
namespace HyperAIStudio::AutoApproval::KeepOn
{
	/** A kept-on auto-approval never comes back later than this after the user turned it on, whatever its duration. */
	constexpr double MaxAgeSeconds = 24.0 * 60.0 * 60.0;
	/** How far in the future a record's times may be (a clock set back a little) before it is refused. */
	constexpr double ClockSkewSeconds = 5.0 * 60.0;
	/**
	 * How often a kept-on "until the editor closes" record is written again while it runs, so that after a crash, which
	 * records no close time, the last write stands for the close.
	 */
	constexpr double HeartbeatSeconds = 60.0;
	/**
	 * "Until the editor closes" comes back only if the editor opens again this soon after it closed: the window in which
	 * a restart's chats resume by themselves (FHyperAIStudioEditorRestart::AutoResumeWindowSeconds).
	 */
	double CloseWindowSeconds();

	/** What a kept-on auto-approval leaves behind, as the editor that turned it on last wrote it. */
	struct FRecord
	{
		uint8 CategoryMask = 0;
		EHyperAIStudioAutoApprovalDuration Duration = EHyperAIStudioAutoApprovalDuration::OneHour;
		/** The hours, when Duration is Custom; the record holds them only then. */
		double CustomHours = HyperAIStudio::AutoApproval::DefaultCustomHours;
		/** When the user turned it on; a restore keeps the first time, so the 24-hour cap spans every restart. */
		FDateTime TurnedOnUtc;
		/** When a timed duration ends, in wall-clock time, so time the editor spent closed counts; unset until it closes. */
		TOptional<FDateTime> ExpiresUtc;
		/** The runaway guard's counts, so its limits trip at the same totals across restarts. */
		int32 Approved = 0;
		int32 ExternalApproved = 0;
		int32 ConsecutiveFailures = 0;
		int32 SandboxWrites = 0;
		/** Fingerprint of the spend settings it was confirmed under; a change while the editor was closed ends it too. */
		FString SpendFingerprint;
		FString ProjectPath;
		FString WindowsUser;
		/** When this was last written: at least as late as the editor was last seen with it on. */
		FDateTime WrittenUtc;
		/** When the editor closed with it on; unset after a crash, where WrittenUtc stands for the close. */
		TOptional<FDateTime> ClosedUtc;
	};

	/** Why a record does not bring auto-approval back. Every refusal deletes the record. */
	enum class ERefusal : uint8
	{
		None,
		NoRecord,
		/** The file could not be read, or is empty. */
		Unreadable,
		/** DPAPI refused it: changed, written as plain text, or sealed for another Windows user or project. */
		DoesNotDecrypt,
		/** It decrypted, but is not a complete record of this format. */
		Unparsable,
		WrongProject,
		WrongUser,
		/** Its fields contradict each other: no category, an unknown one, negative counts, an expiry its duration cannot give. */
		Inconsistent,
		/** It says it was turned on, written or closed later than now: the clock was set back. */
		FromTheFuture,
		/** Turned on 24 hours ago or more. */
		TooOld,
		/** A timed duration whose end passed, counting the time the editor was closed. */
		Expired,
		/** "Until the editor closes", and the editor stayed closed 30 minutes or more. */
		ClosedTooLong,
		SpendSettingsChanged,
		/** One of the runaway guard's limits was already reached. */
		LimitReached
	};

	/** What a record is checked against: this editor, now. */
	struct FContext
	{
		FDateTime NowUtc;
		FString ProjectPath;
		FString WindowsUser;
		FString SpendFingerprint;
	};

	/** fifteen_minutes, one_hour, four_hours, eight_hours, custom, until_editor_closes: as the record and the dialog's saved choices name them. */
	const TCHAR* DurationKey(EHyperAIStudioAutoApprovalDuration Duration);
	bool ParseDuration(const FString& Key, EHyperAIStudioAutoApprovalDuration& OutDuration);

	/** The project file's full path, normalized and lower-cased, as records and the DPAPI entropy name it. */
	FString CurrentProject();
	/** The Windows account this editor runs as. */
	FString CurrentUser();
	/** A hash of HyperAI's spend settings as text, so the record holds no prices or service names. */
	FString Fingerprint(const FString& SpendSettings);
	/** Saved/HyperAIStudio/AutoApproval/<hash of the Windows user>.keepon: one record per user and project. */
	FString DefaultPath();

	FString ToJson(const FRecord& Record);
	/** False unless Json is a complete record of this format. */
	bool FromJson(const FString& Json, FRecord& OutRecord);
	/** DPAPI with the UI forbidden, for this Windows user, with entropy bound to HyperAI and ProjectPath. */
	bool Protect(const TArray<uint8>& Plain, const FString& ProjectPath, TArray<uint8>& OutSealed, FString& OutError);
	bool Unprotect(const TArray<uint8>& Sealed, const FString& ProjectPath, TArray<uint8>& OutPlain, FString& OutError);

	/** Seals Record for its own project and writes it to Path; false, with nothing left at Path, when that fails. */
	bool Write(const FString& Path, const FRecord& Record, FString& OutError);
	/** Reads, decrypts, parses and checks the record at Path; OutRecord is filled as far as it could be read. */
	ERefusal Read(const FString& Path, const FContext& Context, FRecord& OutRecord);
	/** The checks after decryption: the project, the user, its own consistency, the clock, the spend settings, the limits. */
	ERefusal Check(const FRecord& Record, const FContext& Context);
	bool Delete(const FString& Path);

	/** Why it did not come back, as a clause in the log's English ("its 1 hour ran out while the editor was closed"). */
	FString DescribeRefusal(ERefusal Refusal, const FRecord& Record, const FContext& Context);
	/** The same for the notification. */
	FText DescribeRefusalText(ERefusal Refusal);
	/** A refusal that suggests the file was written by something other than HyperAI, rather than time passing. */
	bool IsSuspicious(ERefusal Refusal);
}
