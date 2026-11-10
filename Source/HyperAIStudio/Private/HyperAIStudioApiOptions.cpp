// Games by Hyper 2026.
#include "HyperAIStudioApiOptions.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace HyperAIStudio::ApiOptions
{
    static FString OptionsFile(const FString& Root) { return Root / TEXT(".hyperai/runtime/api-agent-options.json"); }

    int32 MaxOutputTokens(const FString& ProjectRoot)
    {
        FString Text;
        TSharedPtr<FJsonObject> Json;
        int32 Tokens = 0;
        if (FFileHelper::LoadFileToString(Text, *OptionsFile(ProjectRoot))
            && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json.IsValid())
        {
            Json->TryGetNumberField(TEXT("max_output_tokens"), Tokens);
        }
        return FMath::Max(0, Tokens);
    }

    bool SetMaxOutputTokens(const FString& ProjectRoot, const int32 Tokens)
    {
        if (Tokens < 0) { return false; }
        const FString File = OptionsFile(ProjectRoot);
        FString Text;
        TSharedPtr<FJsonObject> Json;
        if (!FFileHelper::LoadFileToString(Text, *File)
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid())
        {
            Json = MakeShared<FJsonObject>();
        }
        Json->SetNumberField(TEXT("max_output_tokens"), Tokens);
        Text.Reset();
        FJsonSerializer::Serialize(Json.ToSharedRef(), TJsonWriterFactory<>::Create(&Text));
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
        const FString Temp = File + TEXT(".tmp-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        return FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
            && IFileManager::Get().Move(*File, *Temp, true, true);
    }
}
