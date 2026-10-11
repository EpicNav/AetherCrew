// Games by Hyper 2026.

#pragma once

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"

namespace HyperAIStudio::Tests
{
	/** The lines one log category writes while it lives, as they reach the log. */
	class FLogCapture : public FOutputDevice
	{
	public:
		explicit FLogCapture(const TCHAR* InCategory)
			: Category(InCategory)
		{
			GLog->AddOutputDevice(this);
		}

		virtual ~FLogCapture() override
		{
			GLog->RemoveOutputDevice(this);
		}

		virtual void Serialize(const TCHAR* Value, ELogVerbosity::Type Verbosity, const FName& InCategory) override
		{
			if (InCategory == Category && Value)
			{
				FScopeLock Lock(&Mutex);
				Lines.Add(Value);
			}
		}

		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		/** How many lines so far contain Text. */
		int32 Count(const TCHAR* Text)
		{
			GLog->Flush();
			FScopeLock Lock(&Mutex);
			return Lines.FilterByPredicate([Text](const FString& Line) { return Line.Contains(Text); }).Num();
		}

	private:
		FName Category;
		FCriticalSection Mutex;
		TArray<FString> Lines;
	};
}

#endif
