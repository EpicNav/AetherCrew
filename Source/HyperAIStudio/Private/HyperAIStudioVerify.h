// Games by Hyper 2026.

#pragma once

#include "CoreMinimal.h"
#include "HyperAIStudioBot.h"

/**
 * Verification plays many cases at once, each in its own copy of the open level: a PIE world made through the editor's
 * own play path, with a windowless local player the bot drives, ticked one fixed step per editor frame. The level and
 * its assets never change; the copies are torn down when the batch ends. Game thread only.
 */
namespace HyperAIStudio::Verify
{
	/** PIE keeps per-instance tables sized MAX_PIE_INSTANCES (11); instance 0 stays free for the user's own play. */
	constexpr int32 MaxCases = 10;
	/** The batch size before this machine and level are calibrated. */
	constexpr int32 DefaultLimit = 4;
	constexpr float FrameSeconds = 1.f / 30.f;
	/** Setup and teardown's share of a batch's deadline; a case's own timeout must fit in the rest. */
	constexpr float ReserveSeconds = 5.f;
	constexpr float MinDeadlineSeconds = 10.f;
	constexpr float MaxDeadlineSeconds = 60.f;
	/** Calibration keeps the largest batch that finished within this, which leaves 10 s of the minute spare. */
	constexpr double CalibrationBudgetSeconds = 50.0;

	struct FCase
	{
		FString Name;
		/** Bot steps in order; the first that fails ends the case. */
		TArray<FHyperAIBotStep> Steps;
		/** Simulated seconds the case may run. */
		float TimeoutSeconds = 20.f;
	};

	struct FStepOutcome
	{
		FString Action;
		/** passed, failed or not_run */
		FString Status;
		FString Code;
		FString Detail;
		FString File;
		/** Simulated seconds into the case when the step ended. */
		float AtSeconds = 0.f;
	};

	struct FCaseOutcome
	{
		FString Name;
		/** passed, failed, timed_out, stopped or not_started */
		FString Status;
		FString Diagnostic;
		int32 FailedStep = INDEX_NONE;
		float SimulatedSeconds = 0.f;
		TArray<FStepOutcome> Steps;
	};

	struct FBatchOutcome
	{
		/** completed, deadline or stopped */
		FString Status;
		FString Diagnostic;
		FString Map;
		int32 Limit = 0;
		double WallSeconds = 0.0;
		TArray<FCaseOutcome> Cases;
	};

	/** Before a batch starts: 1 to Limit cases, a deadline of 10 to 60 s, case timeouts that fit it, and valid steps. */
	bool Admit(const TArray<FCase>& Cases, float DeadlineSeconds, int32 Limit, FString& OutError);

	/** Starts a batch now. OnDone runs once on the game thread, when every case has ended or at the deadline. */
	bool Start(TArray<FCase> Cases, float DeadlineSeconds, TFunction<void(const FBatchOutcome&)> OnDone, FString& OutError);

	/** Ends a running batch at once; cases still running report stopped, with the reason. */
	void Stop(const FString& Reason);

	bool IsRunning();

	/** How many cases a batch may hold on this machine and level: the calibrated number, or DefaultLimit. */
	int32 GetLimit();

	/** The largest case count whose batch finished within the budget, from measured seconds by count. At least 1. */
	int32 PickLimit(const TMap<int32, double>& SecondsByCount, double BudgetSeconds);

	struct FCalibration
	{
		/** calibrated, measured (this call's result), not_calibrated or failed */
		FString Status;
		FString Diagnostic;
		FString Map;
		int32 Limit = DefaultLimit;
		FString MeasuredUtc;
		TMap<int32, double> SecondsByCount;
	};

	/** What calibration last found for the open level. */
	FCalibration GetCalibration();

	/**
	 * Runs batches of 1, 2, 4, 6, 8 and 10 reference cases (up to MaxCount), each playing 10 simulated seconds, and
	 * keeps the largest that finished within CalibrationBudgetSeconds as this level's limit. It stays on this machine.
	 */
	bool Calibrate(int32 MaxCount, TFunction<void(const FCalibration&)> OnDone, FString& OutError);
}
