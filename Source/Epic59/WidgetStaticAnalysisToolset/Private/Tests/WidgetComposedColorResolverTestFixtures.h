// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/TextBlock.h"
#include "WidgetComposedColorResolverTestFixtures.generated.h"

/** Test-only native UTextBlock subclass, standing in for style-driven subclasses such as CommonUI's text block. */
UCLASS()
class UWidgetComposedColorResolverTestTextBlock : public UTextBlock
{
	GENERATED_BODY()
};
