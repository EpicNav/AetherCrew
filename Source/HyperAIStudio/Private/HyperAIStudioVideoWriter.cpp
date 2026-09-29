// Games by Hyper 2026.

#include "HyperAIStudioVideoWriter.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/AllowWindowsPlatformAtomics.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include "Windows/HideWindowsPlatformAtomics.h"
#include "Windows/HideWindowsPlatformTypes.h"
#include "Windows/WindowsPlatformMisc.h"
#endif

struct FHyperAIStudioVideoWriter::FImpl
{
#if PLATFORM_WINDOWS
	IMFSinkWriter* SinkWriter = nullptr;
	DWORD StreamIndex = 0;
	bool bStarted = false;
#endif
};

namespace
{
#if PLATFORM_WINDOWS
	FString Describe(const TCHAR* Step, const HRESULT Result)
	{
		return FString::Printf(TEXT("Media Foundation %s failed (0x%08x)."), Step, static_cast<uint32>(Result));
	}

	template <typename T>
	void Release(T*& Object)
	{
		if (Object)
		{
			Object->Release();
			Object = nullptr;
		}
	}
#endif

	/** 100-nanosecond units, Media Foundation's clock. */
	constexpr int64 Ticks = 10000000;
}

FHyperAIStudioVideoWriter::FHyperAIStudioVideoWriter()
	: Impl(MakeUnique<FImpl>())
{
}

FHyperAIStudioVideoWriter::~FHyperAIStudioVideoWriter()
{
	FString Ignored;
	Close(Ignored);
}

bool FHyperAIStudioVideoWriter::IsOpen() const
{
#if PLATFORM_WINDOWS
	return Impl->SinkWriter != nullptr;
#else
	return false;
#endif
}

bool FHyperAIStudioVideoWriter::Open(const FString& Path, const int32 InWidth, const int32 InHeight, const int32 FramesPerSecond, FString& OutError)
{
#if PLATFORM_WINDOWS
	if (IsOpen())
	{
		OutError = TEXT("The video writer is already open.");
		return false;
	}
	Width = InWidth & ~1;
	Height = InHeight & ~1;
	Fps = FMath::Clamp(FramesPerSecond, 1, 60);
	FrameCount = 0;
	if (Width < 16 || Height < 16)
	{
		OutError = TEXT("A video needs frames of at least 16 x 16 pixels.");
		return false;
	}
	// The DLLs are delay-loaded, and calling into a missing one crashes rather than failing, so check first.
	if (!FPlatformProcess::GetDllHandle(TEXT("mfplat.dll")) || !FPlatformProcess::GetDllHandle(TEXT("mfreadwrite.dll")))
	{
		OutError = TEXT("Windows Media Foundation is not installed (Windows N editions need the Media Feature Pack).");
		return false;
	}
	FWindowsPlatformMisc::CoInitialize();
	HRESULT Result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
	if (FAILED(Result))
	{
		OutError = Describe(TEXT("startup (Windows N editions need the Media Feature Pack)"), Result);
		return false;
	}
	IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);

	IMFMediaType* Output = nullptr;
	IMFMediaType* Input = nullptr;
	auto Fail = [&](const TCHAR* Step)
	{
		OutError = Describe(Step, Result);
		Release(Output);
		Release(Input);
		Release(Impl->SinkWriter);
		MFShutdown();
		return false;
	};
	if (FAILED(Result = MFCreateSinkWriterFromURL(*Path, nullptr, nullptr, &Impl->SinkWriter))) return Fail(TEXT("sink writer"));

	// ponytail: a fixed 4 Mbit/s suits review clips up to 1080p; expose it when someone needs archival quality.
	if (FAILED(Result = MFCreateMediaType(&Output))
		|| FAILED(Result = Output->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
		|| FAILED(Result = Output->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264))
		|| FAILED(Result = Output->SetUINT32(MF_MT_AVG_BITRATE, 4000000))
		|| FAILED(Result = Output->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))
		|| FAILED(Result = MFSetAttributeSize(Output, MF_MT_FRAME_SIZE, Width, Height))
		|| FAILED(Result = MFSetAttributeRatio(Output, MF_MT_FRAME_RATE, Fps, 1))
		|| FAILED(Result = MFSetAttributeRatio(Output, MF_MT_PIXEL_ASPECT_RATIO, 1, 1))
		|| FAILED(Result = Impl->SinkWriter->AddStream(Output, &Impl->StreamIndex)))
	{
		return Fail(TEXT("H.264 stream"));
	}
	// RGB32 is bottom-up unless the stride is positive; ours runs top row first.
	if (FAILED(Result = MFCreateMediaType(&Input))
		|| FAILED(Result = Input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
		|| FAILED(Result = Input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32))
		|| FAILED(Result = Input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))
		|| FAILED(Result = MFSetAttributeSize(Input, MF_MT_FRAME_SIZE, Width, Height))
		|| FAILED(Result = MFSetAttributeRatio(Input, MF_MT_FRAME_RATE, Fps, 1))
		|| FAILED(Result = MFSetAttributeRatio(Input, MF_MT_PIXEL_ASPECT_RATIO, 1, 1))
		|| FAILED(Result = Input->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(Width * 4)))
		|| FAILED(Result = Impl->SinkWriter->SetInputMediaType(Impl->StreamIndex, Input, nullptr)))
	{
		return Fail(TEXT("frame format"));
	}
	if (FAILED(Result = Impl->SinkWriter->BeginWriting())) return Fail(TEXT("begin writing"));
	Release(Output);
	Release(Input);
	Impl->bStarted = true;
	return true;
#else
	OutError = TEXT("Video recording needs Windows Media Foundation.");
	return false;
#endif
}

bool FHyperAIStudioVideoWriter::WriteFrame(const TConstArrayView<FColor> Pixels, FString& OutError)
{
#if PLATFORM_WINDOWS
	if (!IsOpen())
	{
		OutError = TEXT("The video writer is not open.");
		return false;
	}
	if (Pixels.Num() != Width * Height)
	{
		OutError = FString::Printf(TEXT("A frame must be %d x %d pixels."), Width, Height);
		return false;
	}
	const DWORD Size = static_cast<DWORD>(Pixels.Num() * sizeof(FColor));
	IMFMediaBuffer* Buffer = nullptr;
	IMFSample* Sample = nullptr;
	HRESULT Result = MFCreateMemoryBuffer(Size, &Buffer);
	if (SUCCEEDED(Result))
	{
		BYTE* Data = nullptr;
		if (SUCCEEDED(Result = Buffer->Lock(&Data, nullptr, nullptr)))
		{
			// FColor is B, G, R, A in memory: RGB32's byte order.
			FMemory::Memcpy(Data, Pixels.GetData(), Size);
			Buffer->Unlock();
			Result = Buffer->SetCurrentLength(Size);
		}
	}
	if (SUCCEEDED(Result)) Result = MFCreateSample(&Sample);
	if (SUCCEEDED(Result)) Result = Sample->AddBuffer(Buffer);
	if (SUCCEEDED(Result)) Result = Sample->SetSampleTime(FrameCount * Ticks / Fps);
	if (SUCCEEDED(Result)) Result = Sample->SetSampleDuration(Ticks / Fps);
	if (SUCCEEDED(Result)) Result = Impl->SinkWriter->WriteSample(Impl->StreamIndex, Sample);
	Release(Sample);
	Release(Buffer);
	if (FAILED(Result))
	{
		OutError = Describe(TEXT("write frame"), Result);
		return false;
	}
	++FrameCount;
	return true;
#else
	OutError = TEXT("Video recording needs Windows Media Foundation.");
	return false;
#endif
}

bool FHyperAIStudioVideoWriter::Close(FString& OutError)
{
#if PLATFORM_WINDOWS
	if (!IsOpen())
	{
		return true;
	}
	// A writer that never received a frame has nothing to finalize, and Finalize would fail.
	const HRESULT Result = FrameCount > 0 ? Impl->SinkWriter->Finalize() : S_OK;
	Release(Impl->SinkWriter);
	MFShutdown();
	Impl->bStarted = false;
	if (FAILED(Result))
	{
		OutError = Describe(TEXT("finalize"), Result);
		return false;
	}
	return FrameCount > 0;
#else
	return true;
#endif
}
