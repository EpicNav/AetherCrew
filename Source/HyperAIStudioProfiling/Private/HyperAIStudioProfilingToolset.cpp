// Games by Hyper 2026.

#include "HyperAIStudioProfilingToolset.h"

#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HyperAIStudioAgentActivity.h"
#include "HyperAIStudioApprovalGate.h"
#include "HyperAIStudioCapabilityRuntimeIndex.h"
#include "HyperAIStudioExtensionRuntime.h"
#include "HyperAIStudioProfileAnalysis.h"
#include "HyperAIStudioProfileLive.h"
#include "HyperAIStudioProfileSession.h"
#include "HyperAIStudioRequestFit.h"
#include "JsonObjectConverter.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogHyperAIStudioProfiling, Log, All);

FString FHyperAIStudioProfilingContracts::GetQualifiedToolsetName()
{
	return TEXT("HyperAIStudioProfiling.HyperAIStudioProfilingToolset");
}

TArray<FString> FHyperAIStudioProfilingContracts::GetToolNames()
{
	return {SessionToolName, CompareToolName, LiveToolName};
}

bool FHyperAIStudioProfilingContracts::IsRegistrationAllowed(const bool bAllowSourceCandidateForDev)
{
	return FHyperAIStudioExtensionRuntime::IsExactGeneratedCohortRegistrationAllowed(PackId, AtomicCohortId, GetToolNames(), bAllowSourceCandidateForDev);
}

FString FHyperAIStudioProfilingContracts::GetProfilesDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("HyperAIStudio/Profiles"));
}

namespace HyperAIStudio::Profiling::Tools
{
	using C = FHyperAIStudioProfilingContracts;

	/** Ids name folders, so they keep to letters, digits, dot, dash and underscore. */
	bool IsSafeId(const FString& Id)
	{
		if (Id.IsEmpty() || Id.Len() > 64 || Id.StartsWith(TEXT("."))) return false;
		for (const TCHAR Character : Id)
		{
			if (!FChar::IsAlnum(Character) && Character != TEXT('-') && Character != TEXT('_') && Character != TEXT('.')) return false;
		}
		return true;
	}

	FString NewId(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s-%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12).ToLower());
	}

	FString ProfilePath(const FString& Id)
	{
		return C::GetProfilesDirectory() / Id / TEXT("profile.json");
	}

	bool Store(FHyperAIPerfReport& Report)
	{
		Report.ReportPath = ProfilePath(Report.ProfileId);
		FString Json;
		if (!FJsonObjectConverter::UStructToJsonObjectString(Report, Json)) return false;
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Report.ReportPath), /*Tree=*/true);
		return FFileHelper::SaveStringToFile(Json, *Report.ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	bool Load(const FString& Id, FHyperAIPerfReport& Out)
	{
		FString Json;
		return IsSafeId(Id) && FFileHelper::LoadFileToString(Json, *ProfilePath(Id)) && FJsonObjectConverter::JsonObjectStringToUStruct(Json, &Out);
	}

	/** A CSV path inside this project, made absolute; empty with Why when it is not one. */
	FString ResolveCsv(const FString& Path, FString& OutWhy)
	{
		FString Full = Path.TrimStartAndEnd().Replace(TEXT("\\"), TEXT("/"));
		if (FPaths::IsRelative(Full)) Full = FPaths::ProjectDir() / Full;
		Full = FPaths::ConvertRelativePathToFull(Full);
		FPaths::NormalizeFilename(Full);
		FPaths::CollapseRelativeDirectories(Full);
		const FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		if (!FPaths::IsUnderDirectory(Full, Project))
		{
			OutWhy = TEXT("The CSV must be inside this project (Saved/Profiling/CSV, say).");
			return FString();
		}
		if (!Full.EndsWith(TEXT(".csv"), ESearchCase::IgnoreCase))
		{
			OutWhy = TEXT("Only uncompressed .csv files from the CSV profiler are read.");
			return FString();
		}
		if (!IFileManager::Get().FileExists(*Full))
		{
			OutWhy = FString::Printf(TEXT("No file at %s."), *Full);
			return FString();
		}
		return Full;
	}

	/** Analyses a CSV file into Out (status completed), or says why not. */
	bool AnalyseCsvFile(const FString& Path, const FAnalysisOptions& Options, FHyperAIPerfReport& Out)
	{
		FString Why;
		const FString Full = ResolveCsv(Path, Why);
		if (Full.IsEmpty())
		{
			Out.Status = TEXT("invalid_request");
			Out.Diagnostic = Why;
			return false;
		}
		FString Text;
		FCsvCapture Csv;
		if (!FFileHelper::LoadFileToString(Text, *Full) || !ParseCsv(Text, Csv, Why))
		{
			Out.Status = TEXT("invalid_request");
			Out.Diagnostic = Why.IsEmpty() ? FString::Printf(TEXT("%s could not be read."), *Full) : Why;
			return false;
		}
		FCaptureContext Context;
		Analyse(Csv, Context, Options, Out);
		Out.Source = TEXT("csv");
		Out.CsvPath = Full;
		Out.HotFunctionsNote = TEXT("A CSV alone has no trace: hot functions come from a session with bHotFunctions.");
		Out.Notes.Add(TEXT("Analysed from a file: the frame limits and GPU availability in effect when it was recorded are unknown, so GPU time counts as available when the file has it."));
		Advise(Out, nullptr);
		Out.bOk = Out.Frames > 0;
		Out.Status = Out.bOk ? TEXT("completed") : TEXT("capture_failed");
		Out.Diagnostic = Out.bOk ? Out.Verdict.Summary : TEXT("The CSV has no frames with a FrameTime.");
		return Out.bOk;
	}

	/** A profile by id, or a CSV path analysed now. */
	bool Resolve(const FString& Reference, FHyperAIPerfReport& Out, FString& OutWhy)
	{
		const FString Trimmed = Reference.TrimStartAndEnd();
		if (Trimmed.EndsWith(TEXT(".csv"), ESearchCase::IgnoreCase) || Trimmed.Contains(TEXT("/")) || Trimmed.Contains(TEXT("\\")))
		{
			FAnalysisOptions Options;
			if (!AnalyseCsvFile(Trimmed, Options, Out))
			{
				OutWhy = Out.Diagnostic;
				return false;
			}
			Out.ProfileId = FPaths::GetBaseFilename(Trimmed);
			return true;
		}
		if (!Load(Trimmed, Out))
		{
			OutWhy = FString::Printf(TEXT("No profile %s in %s."), *Trimmed, *C::GetProfilesDirectory());
			return false;
		}
		return true;
	}

	struct FSessionSettings
	{
		FString Id;
		FString Label;
		FAnalysisOptions Analysis;
	};

	/** The session's report from what the run recorded: analysis, hot functions, Niagara, the level's cost and advice. */
	FHyperAIPerfReport BuildReport(Session::FRunOutcome& Outcome, const FSessionSettings& Settings)
	{
		FHyperAIPerfReport Report;
		Report.ProfileId = Settings.Id;
		Report.Label = Settings.Label;
		Report.CreatedUtc = FDateTime::UtcNow().ToIso8601();
		Report.Level = Outcome.Level;
		Report.Source = TEXT("session");
		Report.Mode = Outcome.Mode;
		Report.Status = Outcome.Status;
		Report.CsvPath = Outcome.CsvPath;
		Report.TracePath = Outcome.bTraced ? Outcome.TracePath : FString();
		if (Outcome.bCsvParsed)
		{
			Analyse(Outcome.Csv, Outcome.Context, Settings.Analysis, Report);
		}
		Report.HotFunctions = Outcome.Hot.Session;
		Report.HotFunctionsInWorstFrame = Outcome.Hot.WorstFrame;
		Report.WorstTraceFrameMs = Outcome.Hot.WorstFrameMs;
		Report.HotFunctionsNote = !Outcome.Hot.Note.IsEmpty() ? Outcome.Hot.Note : FString(TEXT("Not asked for (bHotFunctions false)."));
		Report.FxSystems = Outcome.Fx;
		FSceneCostSummary Scene;
		if (!Outcome.SceneCostJson.IsEmpty() && ReadSceneCost(Outcome.SceneCostJson, Scene))
		{
			Report.SceneCost = DescribeSceneCost(Scene);
		}
		else if (!Outcome.SceneCostJson.IsEmpty())
		{
			Report.Notes.Add(TEXT("The level's cost report answered, but not with a completed report."));
		}
		Advise(Report, Scene.bValid ? &Scene : nullptr);
		Report.Bot = Outcome.Bot;
		Report.Notes.Append(Outcome.Notes);
		Report.bOk = Report.Status == TEXT("completed") && Report.Frames > 0;
		if (Report.bOk)
		{
			Report.Diagnostic = FString::Printf(TEXT("%s %d hitch(es) in %.1f s."), *Report.Verdict.Summary, Report.HitchCount, Report.CapturedSeconds);
		}
		else
		{
			Report.Diagnostic = Outcome.Diagnostic.IsEmpty() ? FString(TEXT("The session measured no frames.")) : Outcome.Diagnostic;
		}
		if (Report.Frames > 0 && !Store(Report))
		{
			Report.Notes.Add(TEXT("The profile could not be saved, so hyper_profile_compare cannot read it by id."));
			Report.ReportPath.Reset();
		}
		return Report;
	}

	/** A session request checked and fitted: either its answer now, or the run to start. */
	struct FPrepared
	{
		FHyperAIPerfReport Report;
		bool bRun = false;
		Session::FRunOptions Options;
		FSessionSettings Settings;
		/** What was fitted, for the answer. */
		FString FitNotes;
	};

	FPrepared Prepare(const FHyperAIPerfSessionRequest& In)
	{
		FHyperAIPerfSessionRequest Request = In;
		FHyperAIStudioRequestFit Fit;
		FPrepared Prepared;
		FHyperAIPerfReport& Report = Prepared.Report;
		auto Refuse = [&Prepared, &Fit](const TCHAR* Status, const FString& Why)
		{
			Prepared.Report.Status = Status;
			Prepared.Report.Diagnostic = Fit.AppendTo(Why);
			return Prepared;
		};
		if (!IsInGameThread()) return Refuse(TEXT("invalid_request"), TEXT("hyper_profile_session runs on the game thread."));

		Fit.ClampOrDefault(Request.TopN, 1, 30, 10, TEXT("top_n"));
		Fit.ClampOrDefault(Request.HitchThresholdMs, 5.f, 1000.f, 50.f, TEXT("hitch_threshold_ms"));
		FAnalysisOptions Analysis;
		Analysis.HitchThresholdMs = Request.HitchThresholdMs;
		Analysis.TopN = Request.TopN;
		Analysis.MaxHitches = Request.TopN;

		if (!Request.ProfileId.TrimStartAndEnd().IsEmpty())
		{
			const FString Id = Request.ProfileId.TrimStartAndEnd();
			if (!Load(Id, Report))
			{
				return Refuse(TEXT("not_found"), FString::Printf(TEXT("No profile %s in %s; a session that ran keeps its profile there."), *Id, *C::GetProfilesDirectory()));
			}
			Report.Source = TEXT("stored");
			return Prepared;
		}
		const FString Id = Request.OperationId.TrimStartAndEnd().IsEmpty() ? NewId(TEXT("profile")) : Request.OperationId.TrimStartAndEnd();
		if (!IsSafeId(Id))
		{
			return Refuse(TEXT("invalid_request"), TEXT("operation_id names the profile's folder: 1 to 64 letters, digits, dot, dash or underscore."));
		}
		Report.ProfileId = Id;
		Report.Label = Request.Label.Left(200);
		if (!Request.CsvPath.TrimStartAndEnd().IsEmpty())
		{
			if (AnalyseCsvFile(Request.CsvPath, Analysis, Report))
			{
				Report.ProfileId = Id;
				Report.CreatedUtc = FDateTime::UtcNow().ToIso8601();
				if (!Store(Report)) Report.Notes.Add(TEXT("The profile could not be saved."));
			}
			Report.Diagnostic = Fit.AppendTo(Report.Diagnostic);
			return Prepared;
		}

		const FString Mode = Request.Mode.TrimStartAndEnd().ToLower();
		if (!Mode.IsEmpty() && Mode != TEXT("auto") && Mode != TEXT("attach") && Mode != TEXT("start"))
		{
			return Refuse(TEXT("invalid_request"), TEXT("mode is auto, attach or start."));
		}
		const bool bStartPie = Mode == TEXT("start") || ((Mode.IsEmpty() || Mode == TEXT("auto")) && !(GEditor && GEditor->PlayWorld));
		Fit.ClampOrDefault(Request.DurationSeconds, 1.f, 60.f, 10.f, TEXT("duration_seconds"));
		if (Request.WarmupSeconds < 0.f) Request.WarmupSeconds = bStartPie ? 3.f : 0.f;
		Fit.Clamp(Request.WarmupSeconds, 0.f, 30.f, TEXT("warmup_seconds"));
		if (Request.Bot.Num() > C::MaxBotSteps)
		{
			return Refuse(TEXT("invalid_request"), FString::Printf(TEXT("bot holds at most %d steps; split the route or measure part of it."), C::MaxBotSteps));
		}
		for (int32 Index = 0; Index < Request.Bot.Num(); ++Index)
		{
			FString Why;
			if (!FHyperAIBotExecutor::Validate(Request.Bot[Index], Why))
			{
				return Refuse(TEXT("invalid_request"), FString::Printf(TEXT("bot[%d]: %s"), Index, *Why));
			}
		}

		Session::FRunOptions Options;
		Options.Id = Id;
		Options.bStartPie = bStartPie;
		Options.WarmupSeconds = Request.WarmupSeconds;
		Options.DurationSeconds = Request.DurationSeconds;
		Options.bTrace = Request.bHotFunctions;
		Options.bFx = true;
		Options.bSceneCost = Request.bSceneCost;
		Options.TopN = Request.TopN;
		Options.LiveTopN = Request.TopN;
		Options.Bot = Request.Bot;
		Options.Directory = C::GetProfilesDirectory() / Id;
		Options.CsvCategories = {TEXT("NaniteStreaming"), TEXT("NaniteStreamingDetail"), TEXT("VSM")};
		Options.ToolName = C::SessionToolName;
		FString Status;
		FString Why;
		if (!Session::CanStart(Options, Status, Why)) return Refuse(*Status, Why);
		Report.Mode = bStartPie ? TEXT("start") : TEXT("attach");

		FSessionSettings Settings;
		Settings.Id = Id;
		Settings.Label = Report.Label;
		Settings.Analysis = Analysis;
		const bool bEffects = bStartPie || !Request.Bot.IsEmpty();
		if (bEffects && FHyperAIStudioApprovalGate::IsApprovalRequired(EHyperAIStudioDomainSafety::Edit))
		{
			// Playing the game is an edit to the editor's state, as a playtest is: it waits for the user who approves edits.
			FHyperAIStudioApprovalSummary Summary;
			Summary.PackId = C::PackId;
			Summary.ToolName = C::SessionToolName;
			Summary.VariantId = TEXT("profile.session.v1");
			Summary.Safety = EHyperAIStudioDomainSafety::Edit;
			const UWorld* EditorWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			Summary.EffectTarget = EditorWorld ? EditorWorld->GetPathName() : FString(TEXT("Play In Editor"));
			Summary.Effects.Add(FString::Printf(TEXT("%s, warm up %.0f s, measure %.0f s with the CSV profiler%s%s"),
				bStartPie ? TEXT("Start Play In Editor") : TEXT("Attach to Play In Editor"), Options.WarmupSeconds, Options.DurationSeconds,
				Options.bTrace ? TEXT(" and an Insights trace") : TEXT(""), bStartPie ? TEXT(", then stop it") : TEXT("")));
			for (const FHyperAIBotStep& Step : Request.Bot)
			{
				Summary.Effects.Add(FString::Printf(TEXT("bot %s %s"), *FHyperAIBotExecutor::ActionName(Step.Action), *Step.Target).TrimEnd());
			}
			Summary.Effects.Add(TEXT("write the profile under Saved/HyperAIStudio/Profiles/") + Id);
			FString ApprovalError;
			if (!FHyperAIStudioApprovalGate::RequestAction(Id, Summary, [Options, Settings]()
				{
					FString StartStatus;
					FString StartWhy;
					const bool bStarted = Session::Start(Options, [Settings](Session::FRunOutcome& Outcome)
					{
						const FHyperAIPerfReport Done = BuildReport(Outcome, Settings);
						Outcome.Diagnostic = FString::Printf(TEXT("%s Read the profile with hyper_profile_session and profile_id %s."), *Done.Diagnostic, *Settings.Id);
					}, StartStatus, StartWhy);
					if (!bStarted)
					{
						FHyperAIStudioActivityEntry Entry;
						Entry.Kind = EHyperAIStudioActivityKind::Failed;
						Entry.PackId = C::PackId;
						Entry.ToolName = C::SessionToolName;
						Entry.Target = TEXT("Play In Editor");
						Entry.OperationId = Settings.Id;
						Entry.StatusCode = StartStatus;
						Entry.Detail = StartWhy;
						FHyperAIStudioAgentActivityLog::Record(MoveTemp(Entry));
					}
				}, ApprovalError))
			{
				return Refuse(TEXT("approval_not_queued"), ApprovalError);
			}
			Report.bOk = true;
			Report.Status = TEXT("awaiting_user_approval");
			Report.Diagnostic = Fit.AppendTo(FString::Printf(TEXT("Playing the game waits for approval in HyperAI Chat, Activity panel; nothing ran yet. hyper_operation_wait with operation_id %s and bWaitThroughApproval true waits for the approval and the session, then hyper_profile_session with profile_id %s reads the profile."), *Id, *Id));
			FHyperAIStudioApprovalGate::DescribeHeld(Id, Report.Status, Report.Diagnostic);
			return Prepared;
		}
		Prepared.bRun = true;
		Prepared.Options = MoveTemp(Options);
		Prepared.Settings = MoveTemp(Settings);
		Prepared.FitNotes = Fit.AppendTo(FString());
		return Prepared;
	}
}

UHyperAIPerfSessionResult* UHyperAIStudioProfilingToolset::hyper_profile_session(const FHyperAIPerfSessionRequest& Request)
{
	using namespace HyperAIStudio::Profiling;
	UHyperAIPerfSessionResult* Result = NewObject<UHyperAIPerfSessionResult>();
	Tools::FPrepared Prepared = Tools::Prepare(Request);
	if (!Prepared.bRun)
	{
		Result->SetValue(Prepared.Report);
		return Result;
	}
	// The session runs over many frames; the MCP server holds only this object, so the run keeps it alive until it answers.
	const TStrongObjectPtr<UHyperAIPerfSessionResult> Pending(Result);
	FString Status;
	FString Why;
	if (!Session::Start(Prepared.Options, [Pending, Settings = Prepared.Settings, FitNotes = Prepared.FitNotes](Session::FRunOutcome& Outcome)
	{
		FHyperAIPerfReport Report = Tools::BuildReport(Outcome, Settings);
		if (!FitNotes.IsEmpty()) Report.Diagnostic += TEXT(" ") + FitNotes;
		Outcome.Diagnostic = Report.Diagnostic;
		Pending->SetValue(Report);
	}, Status, Why))
	{
		Prepared.Report.Status = Status;
		Prepared.Report.Diagnostic = Why;
		Result->SetValue(Prepared.Report);
	}
	return Result;
}

FHyperAIPerfCompareReport UHyperAIStudioProfilingToolset::hyper_profile_compare(const FHyperAIPerfCompareRequest& In)
{
	using namespace HyperAIStudio::Profiling;
	FHyperAIPerfCompareReport Report;
	FHyperAIPerfCompareRequest Request = In;
	FHyperAIStudioRequestFit Fit;
	Fit.ClampOrDefault(Request.ThresholdPercent, 1.f, 50.f, 5.f, TEXT("threshold_percent"));
	Fit.ClampOrDefault(Request.TopN, 1, 30, 10, TEXT("top_n"));
	Report.Baseline = Request.Baseline.TrimStartAndEnd();
	Report.Candidate = Request.Candidate.TrimStartAndEnd();
	if (!IsInGameThread() || Report.Baseline.IsEmpty() || Report.Candidate.IsEmpty())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("baseline and candidate are each a profile id (from hyper_profile_session) or a CSV path inside this project.");
		return Report;
	}
	FHyperAIPerfReport Before;
	FHyperAIPerfReport After;
	FString Why;
	if (!Tools::Resolve(Report.Baseline, Before, Why) || !Tools::Resolve(Report.Candidate, After, Why))
	{
		Report.Status = Why.StartsWith(TEXT("No profile")) ? TEXT("not_found") : TEXT("invalid_request");
		Report.Diagnostic = Why;
		return Report;
	}
	if (Before.Frames == 0 || After.Frames == 0)
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("A profile with no frames cannot be compared.");
		return Report;
	}
	Compare(Before, After, Request.ThresholdPercent, Request.TopN, Report);
	Report.bOk = true;
	Report.Status = TEXT("compared");
	Report.Diagnostic = Fit.AppendTo(Report.Summary);
	return Report;
}

UHyperAIPerfLiveResult* UHyperAIStudioProfilingToolset::hyper_profile_live(const FHyperAIPerfLiveRequest& In)
{
	using namespace HyperAIStudio::Profiling;
	UHyperAIPerfLiveResult* Result = NewObject<UHyperAIPerfLiveResult>();
	FHyperAIPerfLiveRequest Request = In;
	FHyperAIStudioRequestFit Fit;
	FHyperAIPerfLiveReport Report;
	if (!IsInGameThread())
	{
		Report.Status = TEXT("invalid_request");
		Report.Diagnostic = TEXT("hyper_profile_live runs on the game thread.");
		Result->SetValue(Report);
		return Result;
	}
	UWorld* World = GEditor ? GEditor->PlayWorld.Get() : nullptr;
	if (!World)
	{
		Report.Status = TEXT("no_pie_world");
		Report.Diagnostic = TEXT("Play In Editor is not running: this reads the game while it plays. Start it (hyper_playtest_run, or the Play button) and ask again.");
		Result->SetValue(Report);
		return Result;
	}
	Fit.ClampOrDefault(Request.TopN, 1, 50, 10, TEXT("top_n"));
	if (Request.SampleSeconds < 0.f) Request.SampleSeconds = 0.f;
	Fit.Clamp(Request.SampleSeconds, 0.f, 10.f, TEXT("sample_seconds"));
	Report.Level = World->GetOutermost()->GetName();

	auto ReadNow = [Request](UWorld& PlayWorld, FHyperAIPerfLiveReport& Out)
	{
		if (Request.bNanite) Live::ReadNanite(PlayWorld, Request.TopN, Out.Nanite);
		if (Request.bNiagara) Live::ReadNiagara(PlayWorld, Request.TopN, Out.Niagara);
		if (Request.bRenderer) Out.Renderer = Live::ReadRenderer();
	};

	FString Status;
	FString Why;
	Session::FRunOptions Options;
	Options.Id = Tools::NewId(TEXT("live"));
	Options.bStartPie = false;
	Options.DurationSeconds = Request.SampleSeconds;
	Options.bFx = Request.bNiagara;
	Options.bLive = true;
	Options.TopN = Request.TopN;
	Options.LiveTopN = Request.TopN;
	Options.Directory = FHyperAIStudioProfilingContracts::GetProfilesDirectory() / TEXT("live") / Options.Id;
	Options.CsvCategories = {TEXT("NaniteStreaming"), TEXT("NaniteStreamingDetail"), TEXT("VSM")};
	Options.ToolName = FHyperAIStudioProfilingContracts::LiveToolName;
	if (Request.SampleSeconds <= 0.f || !Session::CanStart(Options, Status, Why))
	{
		ReadNow(*World, Report);
		if (Request.SampleSeconds > 0.f)
		{
			Report.Notes.Add(TEXT("Read now without a sample: ") + Why);
		}
		else if (Request.bNiagara)
		{
			Report.Niagara.Notes.Add(TEXT("Per-system game, render and GPU cost needs sample_seconds 1 to 10."));
		}
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Report.Diagnostic = Fit.AppendTo(FString::Printf(TEXT("%d Niagara component(s) playing, %lld particles; %d Nanite and %d fallback mesh component(s)."),
			Report.Niagara.ActiveComponents, Report.Niagara.Particles, Report.Nanite.NanitePrimitives, Report.Nanite.FallbackPrimitives));
		Result->SetValue(Report);
		return Result;
	}
	const TStrongObjectPtr<UHyperAIPerfLiveResult> Pending(Result);
	const FString Directory = Options.Directory;
	const FString FitNotes = Fit.AppendTo(FString());
	if (!Session::Start(Options, [Pending, Request, Directory, FitNotes, Level = Report.Level](Session::FRunOutcome& Outcome)
	{
		FHyperAIPerfLiveReport Done;
		Done.Level = Level;
		Done.SampledSeconds = Outcome.CapturedSeconds;
		Done.SampledFrames = Outcome.bCsvParsed ? Outcome.Csv.NumFrames() : 0;
		if (Outcome.bLiveRead)
		{
			if (Request.bNanite) Done.Nanite = Outcome.Nanite;
			if (Request.bNiagara) Done.Niagara = Outcome.Niagara;
			if (Request.bRenderer) Done.Renderer = Outcome.Renderer;
		}
		if (Outcome.bCsvParsed)
		{
			if (Request.bNanite) Live::ApplyNaniteSample(Outcome.Csv, Done.Nanite);
			if (Request.bRenderer) Live::ApplyRendererSample(Outcome.Csv, Done.Renderer);
		}
		if (Request.bNiagara)
		{
			Live::ApplyFxCosts(Outcome.Fx, Request.TopN, Done.Niagara);
		}
		Done.Notes = Outcome.Notes;
		Done.bOk = Outcome.bLiveRead;
		Done.Status = Outcome.bLiveRead ? TEXT("completed") : (Outcome.Status.IsEmpty() ? FString(TEXT("stopped")) : Outcome.Status);
		Done.Diagnostic = Outcome.bLiveRead
			? FString::Printf(TEXT("Sampled %.1f s: %d Niagara component(s) playing, %lld particles; %d Nanite and %d fallback mesh component(s)."),
				Done.SampledSeconds, Done.Niagara.ActiveComponents, Done.Niagara.Particles, Done.Nanite.NanitePrimitives, Done.Nanite.FallbackPrimitives)
			: Outcome.Diagnostic;
		if (!FitNotes.IsEmpty()) Done.Diagnostic += TEXT(" ") + FitNotes;
		// The sample's files served only this answer.
		IFileManager::Get().DeleteDirectory(*Directory, /*RequireExists=*/false, /*Tree=*/true);
		Pending->SetValue(Done);
	}, Status, Why))
	{
		ReadNow(*World, Report);
		Report.Notes.Add(TEXT("Read now without a sample: ") + Why);
		Report.bOk = true;
		Report.Status = TEXT("completed");
		Result->SetValue(Report);
	}
	return Result;
}

void FHyperAIStudioProfilingRegistration::Startup()
{
	if (bStarted)
	{
		return;
	}
	bStarted = true;
	PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FHyperAIStudioProfilingRegistration::RegisterAfterEngineInit);
	if (GIsRunning && GEngine && UObjectInitialized())
	{
		RegisterAfterEngineInit();
	}
}

void FHyperAIStudioProfilingRegistration::Shutdown()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	HyperAIStudio::Profiling::Session::Shutdown();
	if (bOwnsToolset && IsInGameThread() && UObjectInitialized() && UToolsetRegistry::IsAvailable())
	{
		FString Error;
		if (!FHyperAIStudioCapabilityRuntimeIndex::UnregisterOwned(UHyperAIStudioProfilingToolset::StaticClass(),
			FHyperAIStudioProfilingContracts::GetQualifiedToolsetName(), Error))
		{
			UE_LOG(LogHyperAIStudioProfiling, Warning, TEXT("Could not unregister the profiling toolset: %s"), *Error);
		}
	}
	bOwnsToolset = false;
	bStarted = false;
}

void FHyperAIStudioProfilingRegistration::RegisterAfterEngineInit()
{
	if (!bStarted || bOwnsToolset || !IsInGameThread() || IsEngineExitRequested() || !UObjectInitialized() || !UToolsetRegistry::IsAvailable())
	{
		return;
	}
	if (!FHyperAIStudioProfilingContracts::IsRegistrationAllowed(FHyperAIStudioExtensionRuntime::IsPendingNativeToolsDevModeEnabled()))
	{
		UE_LOG(LogHyperAIStudioProfiling, Verbose, TEXT("The profiling source cohort is not catalog-admitted; registration remains fail-closed."));
		return;
	}
	FString Error;
	if (!FHyperAIStudioCapabilityRuntimeIndex::RegisterOwnedToolsetClass(UHyperAIStudioProfilingToolset::StaticClass(),
		FHyperAIStudioProfilingContracts::GetQualifiedToolsetName(), Error))
	{
		UE_LOG(LogHyperAIStudioProfiling, Error, TEXT("Profiling cohort registration failed closed: %s"), *Error);
		return;
	}
	bOwnsToolset = true;
}
