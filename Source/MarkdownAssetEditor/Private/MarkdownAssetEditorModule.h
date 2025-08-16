// Copyright (C) 2024 Gwaredd Mountain - All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"

class MARKDOWNASSETEDITOR_API FMarkdownAssetEditorModule : public IModuleInterface 
{
	
public:
	
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FText ReadTextFromFile(const FString& FilePath)
	{
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *FilePath))
		{
			return FText::FromString(Text);
		}
		return FText::GetEmpty();
	}

	static bool WriteTextToFile(const FString& FilePath, const FText& Content)
	{
		return FFileHelper::SaveStringToFile(Content.ToString(), *FilePath);
	}

	static bool IsFileReadOnly(const FString& FilePath)
	{
		return IFileManager::Get().IsReadOnly(*FilePath);
	}

	static bool CanWriteToFile(const FString& FilePath)
	{
		// Early return for empty paths
		if (FilePath.IsEmpty())
		{
			return false;
		}
		
		// Check if file exists and is read-only, or if directory is writable for new files
		if (FPaths::FileExists(FilePath))
		{
			return !IsFileReadOnly(FilePath);
		}
		
		// For new files, check if we can write to the directory
		FString Directory = FPaths::GetPath(FilePath);
		if (Directory.IsEmpty())
		{
			return false;
		}
		
		// Check if the directory exists and is writable
		return FPaths::DirectoryExists(Directory) && !IFileManager::Get().IsReadOnly(*Directory);
	}

protected:

	/** Registers main menu and toolbar menu extensions. */
	void RegisterMenuExtensions();

	/** Register the EditorSettings screen. */
	void RegisterSettings();

	/** Unregister on mode shutdown */
	void UnregisterMenuExtensions();
	void UnregisterSettings();

	void EditorAction_OpenProjectDocumentation();
	void EditorAction_OpenAssetDocumentation(UAssetEditorToolkitMenuContext* ExecutionContext);


};