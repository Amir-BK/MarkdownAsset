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
	// Note: We've simplified the approach to always preserve original text in the editor
	// and use HTML base href for image resolution instead of modifying the markdown content
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
		.OnLoadCompleted( FSimpleDelegate::CreateSP( this, &SMarkdownAssetEditor::HandleBrowserLoadCompleted ) )
	;

	// setup binding
	UMarkdownBinding* Binding = NewObject<UMarkdownBinding>();
	Binding->Text = MarkdownAsset->Text;
	
	// Modified binding lambda to write back to disk for file-based markdown assets
	Binding->OnSetText.AddLambda( [this, Binding]() { 
		// Get the edited text from the binding (this is always in the original format with relative paths)
		FText EditedText = Binding->GetText();
		
		// Store the edited text in the asset
		MarkdownAsset->MarkPackageDirty();
		MarkdownAsset->Text = EditedText;
		
		// If this is a link asset pointing to a local file, write back to disk
		UMarkdownLinkAsset* LinkAsset = Cast<UMarkdownLinkAsset>(MarkdownAsset);
		if (LinkAsset && IsCurrentFileALocalFile())
		{
			if (FMarkdownAssetEditorModule::CanWriteToFile(LinkAsset->URL))
			{
				// Save the text as-is (preserves relative paths)
				if (FMarkdownAssetEditorModule::WriteTextToFile(LinkAsset->URL, EditedText))
				{
					UE_LOG(MarkdownStaticsLog, Log, TEXT("Successfully saved markdown file with relative paths preserved: %s"), *LinkAsset->URL);
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

FString SMarkdownAssetEditor::ComputeBaseHref(const FString& UrlString) const
{
	FString BaseHref;
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
			if (Abs.Right(1) != TEXT("/"))
			{
				Abs += TEXT("/");
			}
			BaseHref = FString::Printf(TEXT("file:///%s"), *Abs);
		}
	}
	return BaseHref;
}

void SMarkdownAssetEditor::HandleBrowserLoadCompleted()
{
	bBrowserTemplateLoaded = true;

	// When the template (dark/light html) finishes loading, inject base tag if we currently have a link asset open
	UMarkdownLinkAsset* LinkAsset = Cast<UMarkdownLinkAsset>(MarkdownAsset);
	if (!LinkAsset) { return; }

	const FString BaseHref = ComputeBaseHref(LinkAsset->URL);
	if (WebBrowser.IsValid() && !BaseHref.IsEmpty())
	{
		const FString Script = FString::Printf(
			TEXT("(function(){var head=document.head||document.getElementsByTagName('head')[0]; if(!head){return;} var b=document.querySelector('base'); if(!b){b=document.createElement('base'); head.appendChild(b);} b.href='%s'; console.log('Set base to', b.href); if(window.MarkdownBinding){ /* trigger a re-render if your html defines a function */ if(window.refreshMarkdown){refreshMarkdown();}}})();"),
			*BaseHref
		);
		WebBrowser->ExecuteJavascript(Script);
	}
}

void SMarkdownAssetEditor::OpenMarkdownAssetLink(UMarkdownLinkAsset& LinkAsset, UMarkdownBinding& Binding, const FString& Url)
{
	LinkAsset.URL = Url;
	LinkAsset.MarkPackageDirty();
	
	// Read the original markdown content from the file
	FText OriginalText = FMarkdownAssetEditorModule::ReadTextFromFile(LinkAsset.URL);
	LinkAsset.Text = OriginalText; // Store the original text in the asset (for saving)
	
	// IMPORTANT: Always set the binding to the original text with relative paths
	// The user should always be editing the original format
	Binding.SetText(OriginalText);

	// Only attempt to inject base if browser template already loaded; otherwise the load-complete callback will do it
	if (bBrowserTemplateLoaded)
	{
		const FString BaseHref = ComputeBaseHref(LinkAsset.URL);
		if (WebBrowser.IsValid() && !BaseHref.IsEmpty())
		{
			const FString Script = FString::Printf(
				TEXT("(function(){var head=document.head||document.getElementsByTagName('head')[0]; if(!head){return;} var b=document.querySelector('base'); if(!b){b=document.createElement('base'); head.appendChild(b);} b.href='%s'; console.log('Updated base to', b.href);})();"),
				*BaseHref
			);
			WebBrowser->ExecuteJavascript(Script);
		}
	}

	// Remove Reload call – it caused loss of injected base and timing issues
	UE_LOG(MarkdownStaticsLog, Log, TEXT("MarkdownAssetEditor: Opened link '%s' (pending base injection=%s)"), *LinkAsset.URL, bBrowserTemplateLoaded ? TEXT("immediate") : TEXT("on load"));
}

#undef LOCTEXT_NAMESPACE
