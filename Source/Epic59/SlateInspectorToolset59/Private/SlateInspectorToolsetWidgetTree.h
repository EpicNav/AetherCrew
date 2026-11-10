// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

class FChildren;
class SWidget;

namespace UE::SlateInspectorToolset::Private
{
	/**
	 * The children to walk under Widget. A caching SInvalidationPanel or a retaining SRetainerWidget reports none,
	 * which would hide everything under it (eg: graph editor nodes), so those fall back to the reflector's list.
	 */
	FChildren* GetChildrenToWalk(SWidget& Widget);
} // namespace UE::SlateInspectorToolset::Private
