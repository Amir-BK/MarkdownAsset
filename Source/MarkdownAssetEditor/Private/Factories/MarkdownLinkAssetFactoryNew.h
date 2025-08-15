// Copyright (C) 2024 Gwaredd Mountain - All Rights Reserved.

#pragma once

#include "Factories/Factory.h"
#include "UObject/ObjectMacros.h"
#include "MarkdownLinkAssetFactoryNew.generated.h"

UCLASS(hidecategories = Object)
class UMarkdownLinkAssetFactoryNew : public UFactory
{
	GENERATED_UCLASS_BODY()

public:
	// UFactory
	virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
	virtual bool ShouldShowInNewMenu() const override;

	// Optional: initialize the new asset with this URL
	UPROPERTY(EditAnywhere, Category = "External")
	FString URL;
};
