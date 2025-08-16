// Copyright (C) 2024 Gwaredd Mountain - All Rights Reserved.

#include "SMarkdownAssetEditor.h"

#include "Fonts/SlateFontInfo.h"
#include "Internationalization/Text.h"
#include "MarkdownAsset.h"
#include "UObject/Class.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Interfaces/IPluginManager.h"
#include "MarkdownAssetEditorSettings.h"
#include "MarkdownAssetEditorModule.h"
#include "MarkdownBinding.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Internationalization/Regex.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Styling/AppStyle.h"
#include "LogChannels/MarkdownLogChannels.h"

#define LOCTEXT_NAMESPACE "SMarkdownAssetEditor"

namespace
{
	static FString MarkdownToFileUrl(const FString& Path)
	{
		FString Abs = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*Path);
		Abs.ReplaceInline(TEXT("\\"), TEXT("/"));
		return FString::Printf(TEXT("<%s>"), *Abs);

	}

	static void RewriteImageLinksRelativeTo(const FString& BaseDir, FString& InOutMarkdown)
	{
		// Handles simple patterns: ![alt](path) where path has no spaces and no nested parentheses
		const FRegexPattern Pattern(TEXT("!\\[[^\\]]*\\]\\(([^)\\s]+)\\)"));
		FRegexMatcher Matcher(Pattern, InOutMarkdown);

		TArray<TPair<FString, FString>> Replacements;
		while (Matcher.FindNext())
		{
			const FString Url = Matcher.GetCaptureGroup(1);
			// Skip absolute URLs (scheme:)
			if (Url.Contains(TEXT("://")))
			{
				continue;
			}

			const FString Resolved = FPaths::Combine(BaseDir, Url);
			const FString FileUrl = MarkdownToFileUrl(Resolved);
			Replacements.Add({ Url, FileUrl });
		}

		for (const auto& Pair : Replacements)
		{
			InOutMarkdown.ReplaceInline(*Pair.Key, *Pair.Value, ESearchCase::CaseSensitive);
		}
	}
}

//---------------------------------------------------------------------------------------------------------------------

SMarkdownAssetEditor::~SMarkdownAssetEditor()
{
	FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll( this );

	if( WebBrowser.IsValid() )
	{
		WebBrowser->CloseBrowser();
	}
}

//---------------------------------------------------------------------------------------------------------------------

void SMarkdownAssetEditor::Construct( const FArguments& InArgs, UMarkdownAsset* InMarkdownAsset, const TSharedRef<ISlateStyle>& InStyle )
{
	MarkdownAsset = InMarkdownAsset;

	if( !FModuleManager::Get().IsModuleLoaded( "WebBrowser" ) )
	{
        FMessageDialog::Open( EAppMsgType::Ok, LOCTEXT( "WebBrowserModuleMissing", "You need to enable the WebBrowser plugin to run the Markdown editor." ) );
        return;
	}

	auto Settings = GetDefault<UMarkdownAssetEditorSettings>();

	FString ContentDir = IPluginManager::Get().FindPlugin( TEXT( "MarkdownAsset" ) )->GetContentDir();
	FString FullPath   = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead( *ContentDir );
	FString URL        = FullPath / ( Settings->bDarkSkin ? TEXT( "dark.html" ) : TEXT( "light.html" ) );

	WebBrowser = SAssignNew( WebBrowser, SWebBrowserView )
		.InitialURL( URL )
		.BackgroundColor( Settings->bDarkSkin ? FColor( 0.1f, 0.1f, 0.1f, 1.0f ) : FColor( 1.0f, 1.0f, 1.0f, 1.0f ) )
		.OnConsoleMessage( this, &SMarkdownAssetEditor::HandleConsoleMessage )
	;

	// setup binding
	UMarkdownBinding* Binding = NewObject<UMarkdownBinding>();
	Binding->Text = MarkdownAsset->Text;
	
	// Modified binding lambda to write back to disk for file-based markdown assets
	Binding->OnSetText.AddLambda( [this, Binding]() { 
		MarkdownAsset->MarkPackageDirty(); 
		MarkdownAsset->Text = Binding->GetText(); 
		
		// If this is a link asset pointing to a local file, write back to disk
		UMarkdownLinkAsset* LinkAsset = Cast<UMarkdownLinkAsset>(MarkdownAsset);
		if (LinkAsset && IsCurrentFileALocalFile())
		{
			if (FMarkdownAssetEditorModule::CanWriteToFile(LinkAsset->URL))
			{
				if (FMarkdownAssetEditorModule::WriteTextToFile(LinkAsset->URL, Binding->GetText()))
				{
					UE_LOG(MarkdownStaticsLog, Log, TEXT("Successfully saved markdown file: %s"), *LinkAsset->URL);
				}
				else
				{
					UE_LOG(MarkdownStaticsLog, Warning, TEXT("Failed to save markdown file: %s"), *LinkAsset->URL);
					
					// Show user notification about save failure
					FNotificationInfo Info(LOCTEXT("SaveFailedNotification", "Failed to save markdown file to disk"));
					Info.ExpireDuration = 5.0f;
					Info.bUseLargeFont = false;
					Info.Image = FAppStyle::Get().GetBrush(TEXT("MessageLog.Warning"));
					FSlateNotificationManager::Get().AddNotification(Info);
				}
			}
			else
			{
				UE_LOG(MarkdownStaticsLog, Warning, TEXT("Cannot write to read-only file: %s"), *LinkAsset->URL);
				
				// Show user notification about read-only file
				FNotificationInfo Info(LOCTEXT("ReadOnlyFileNotification", "Cannot save to read-only file"));
				Info.ExpireDuration = 5.0f;
				Info.bUseLargeFont = false;
				Info.Image = FAppStyle::Get().GetBrush(TEXT("MessageLog.Warning"));
				FSlateNotificationManager::Get().AddNotification(Info);
			}
		}
	});
	
	WebBrowser->BindUObject( TEXT( "MarkdownBinding" ), Binding, true );

	UMarkdownLinkAsset* LinkAsset = Cast<UMarkdownLinkAsset>(MarkdownAsset);
	if (LinkAsset)
	{
		OpenMarkdownAssetLink(*LinkAsset, *Binding, LinkAsset->URL);
		
		ChildSlot
			[
				SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
							+ SHorizontalBox::Slot()
							.FillWidth(1.0f)
							[
								SAssignNew(LinkTextBox, SEditableTextBox)
									.Text(FText::FromString(LinkAsset->URL))
									.OnTextCommitted_Lambda([this, LinkAsset, Binding](const FText& Text, ETextCommit::Type CommitType) {
										OpenMarkdownAssetLink(*LinkAsset, *Binding, Text.ToString());
									})
									.Font(FSlateFontInfo(InStyle->GetFontStyle("MarkdownAssetEditor.Font")))
							]
					]
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						WebBrowser.ToSharedRef()
					]
			];
	}
	else {
		ChildSlot
			[
				SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.FillHeight(1.0f)
					[
						WebBrowser.ToSharedRef()
					]
			];
	}

	FCoreUObjectDelegates::OnObjectPropertyChanged.AddSP( this, &SMarkdownAssetEditor::HandleMarkdownAssetPropertyChanged );
}

//---------------------------------------------------------------------------------------------------------------------

FReply SMarkdownAssetEditor::OnKeyDown( const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent )
{
	// consume tilde key to prevent it from being passed to unreal and opening the console

	if( InKeyEvent.GetKey() == EKeys::Tilde )
	{
		return FReply::Handled();
	}

	return FReply::Unhandled();
}

//---------------------------------------------------------------------------------------------------------------------

void SMarkdownAssetEditor::HandleMarkdownAssetPropertyChanged( UObject* Object, FPropertyChangedEvent& PropertyChangedEvent )
{
	//if( Object == MarkdownAsset )
	//{
	//	EditableTextBox->SetText( MarkdownAsset->Text );
	//}
}

void SMarkdownAssetEditor::HandleConsoleMessage( const FString& Message, const FString& Source, int32 Line, EWebBrowserConsoleLogSeverity Serverity )
{
	UE_LOG(MarkdownStaticsLog, Warning, TEXT("Markdown Browser: %s (Source: %s:%d)"), *Message, *Source, Line);
}

bool SMarkdownAssetEditor::IsCurrentFileALocalFile() const
{
	UMarkdownLinkAsset* LinkAsset = Cast<UMarkdownLinkAsset>(MarkdownAsset);
	if (LinkAsset && !LinkAsset->URL.IsEmpty())
	{
		// Check if it's a URL (contains ://) or a local file path
		return !LinkAsset->URL.Contains(TEXT("://"));
	}
	return false;
}

void SMarkdownAssetEditor::OpenMarkdownAssetLink(UMarkdownLinkAsset& LinkAsset, UMarkdownBinding& Binding, const FString& Url)
{
	LinkAsset.URL = Url;
	LinkAsset.MarkPackageDirty();
	// Read markdown content from the provided path/URL
	LinkAsset.Text = FMarkdownAssetEditorModule::ReadTextFromFile(LinkAsset.URL);
	// Preprocess image links to be absolute file URLs when relative
	FString M = LinkAsset.Text.ToString();
	RewriteImageLinksRelativeTo(FPaths::GetPath(LinkAsset.URL), M);
	LinkAsset.Text = FText::FromString(M);

	// Inject a <base> href so relative image paths in the markdown resolve correctly
	FString BaseHref;
	const FString& UrlString = LinkAsset.URL;
	if (UrlString.Contains(TEXT("://")))
	{
		int32 SlashIndex = INDEX_NONE;
		if (UrlString.FindLastChar('/', SlashIndex))
		{
			BaseHref = UrlString.Left(SlashIndex + 1);
		}
	}
	else
	{
		FString BaseDir = FPaths::GetPath(UrlString);
		if (!BaseDir.IsEmpty())
		{
			FString Abs = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*BaseDir);
			Abs.ReplaceInline(TEXT("\\"), TEXT("/"));
			// Ensure trailing slash without using EndsWith to avoid analyzer confusion
			if (Abs.Right(1) != TEXT("/"))
			{
				Abs += TEXT("/");
			}
			BaseHref = FString::Printf(TEXT("file:///%s"), *Abs);
		}
	}

	if (WebBrowser.IsValid() && !BaseHref.IsEmpty())
	{
		const FString Script = FString::Printf(
			TEXT("(function(){var b=document.querySelector('base'); if(!b){b=document.createElement('base'); document.head.appendChild(b);} b.href='%s'; console.log('Set base to', b.href);})();"),
			*BaseHref
		);
		WebBrowser->ExecuteJavascript(Script);
	}

	Binding.SetText(LinkAsset.Text);
	WebBrowser->Reload();

	UE_LOG(MarkdownStaticsLog, Log, TEXT("MarkdownAssetEditor: Opened link '%s' with text '%s'"), *LinkAsset.URL, *LinkAsset.Text.ToString());
}

#undef LOCTEXT_NAMESPACE
