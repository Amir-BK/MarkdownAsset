// Copyright (C) 2024 Gwaredd Mountain - All Rights Reserved.

#include "MarkdownLinkAssetFactoryNew.h"
#include "MarkdownAsset.h"

UMarkdownLinkAssetFactoryNew::UMarkdownLinkAssetFactoryNew(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	SupportedClass = UMarkdownLinkAsset::StaticClass();
	bCreateNew     = true;
	bEditAfterNew  = true;
}

UObject* UMarkdownLinkAssetFactoryNew::FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* /*Context*/, FFeedbackContext* /*Warn*/)
{
	UMarkdownLinkAsset* LinkAsset = NewObject<UMarkdownLinkAsset>(InParent, InClass, InName, Flags | RF_Transactional);

	if (!URL.IsEmpty())
	{
		LinkAsset->URL = URL;
	}

	return LinkAsset;
}

bool UMarkdownLinkAssetFactoryNew::ShouldShowInNewMenu() const
{
	return true;
}
