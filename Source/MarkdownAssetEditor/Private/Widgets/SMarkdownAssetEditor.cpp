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


SMarkdownAssetEditor::~SMarkdownAssetEditor()
{
	FCoreUObjectDelegates::OnObjectPropertyChanged.RemoveAll(this);

	if (WebBrowser.IsValid())
	{
		WebBrowser->CloseBrowser();
	}
}

void SMarkdownAssetEditor::Construct(const FArguments& InArgs, UMarkdownAsset* InMarkdownAsset, const TSharedRef<ISlateStyle>& InStyle)
{
	MarkdownAsset = InMarkdownAsset;

	if (!FModuleManager::Get().IsModuleLoaded("WebBrowser"))
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("WebBrowserModuleMissing", "You need to enable the WebBrowser plugin to run the Markdown editor."));
		return;
	}

	auto Settings = GetDefault<UMarkdownAssetEditorSettings>();

	FString ContentDir = IPluginManager::Get().FindPlugin(TEXT("MarkdownAsset"))->GetContentDir();
	FString FullPath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*ContentDir);
	FString URL = FullPath / (Settings->bDarkSkin ? TEXT("dark.html") : TEXT("light.html"));

	WebBrowser = SAssignNew(WebBrowser, SWebBrowserView)
		.InitialURL(URL)
		.BackgroundColor(Settings->bDarkSkin ? FColor(0.1f, 0.1f, 0.1f, 1.0f) : FColor(1.0f, 1.0f, 1.0f, 1.0f))
		.OnConsoleMessage(this, &SMarkdownAssetEditor::HandleConsoleMessage)
		.OnLoadCompleted(FSimpleDelegate::CreateSP(this, &SMarkdownAssetEditor::HandleBrowserLoadCompleted));

	// Setup binding
	UMarkdownBinding* Binding = NewObject<UMarkdownBinding>();
	Binding->Text = MarkdownAsset->Text;

	// Only mark dirty & write when text actually changes
	Binding->OnSetText.AddLambda([this, Binding]()
	{
		FText EditedText = Binding->GetText();

		// Only proceed if content truly changed
		if (!EditedText.EqualTo(MarkdownAsset->Text))
		{
			MarkdownAsset->Text = EditedText;
			MarkdownAsset->MarkPackageDirty();

			UMarkdownLinkAsset* LinkAsset = Cast<UMarkdownLinkAsset>(MarkdownAsset);
			if (LinkAsset && IsCurrentFileALocalFile())
			{
				if (FMarkdownAssetEditorModule::CanWriteToFile(LinkAsset->URL))
				{
					if (FMarkdownAssetEditorModule::WriteTextToFile(LinkAsset->URL, EditedText))
					{
						UE_LOG(MarkdownStaticsLog, Log, TEXT("Saved markdown file (changed content): %s"), *LinkAsset->URL);
					}
					else
					{
						UE_LOG(MarkdownStaticsLog, Warning, TEXT("Failed to save markdown file: %s"), *LinkAsset->URL);
						FNotificationInfo Info(LOCTEXT("SaveFailedNotification", "Failed to save markdown file to disk"));
						Info.ExpireDuration = 5.0f;
						Info.Image = FAppStyle::Get().GetBrush(TEXT("MessageLog.Warning"));
						FSlateNotificationManager::Get().AddNotification(Info);
					}
				}
				else
				{
					UE_LOG(MarkdownStaticsLog, Warning, TEXT("Cannot write to read-only file: %s"), *LinkAsset->URL);
					FNotificationInfo Info(LOCTEXT("ReadOnlyFileNotification", "Cannot save to read-only file"));
					Info.ExpireDuration = 5.0f;
					Info.Image = FAppStyle::Get().GetBrush(TEXT("MessageLog.Warning"));
					FSlateNotificationManager::Get().AddNotification(Info);
				}
			}
		}
	});

	WebBrowser->BindUObject(TEXT("MarkdownBinding"), Binding, true);

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
						.OnTextCommitted_Lambda([this, LinkAsset, Binding](const FText& Text, ETextCommit::Type CommitType)
						{
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
	else
	{
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

	FCoreUObjectDelegates::OnObjectPropertyChanged.AddSP(this, &SMarkdownAssetEditor::HandleMarkdownAssetPropertyChanged);
}

//---------------------------------------------------------------------------------------------------------------------

FReply SMarkdownAssetEditor::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Tilde)
	{
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

//---------------------------------------------------------------------------------------------------------------------

void SMarkdownAssetEditor::HandleMarkdownAssetPropertyChanged(UObject* Object, FPropertyChangedEvent& PropertyChangedEvent)
{
	// Currently unused, left for potential future synchronization.
}

void SMarkdownAssetEditor::HandleConsoleMessage(const FString& Message, const FString& Source, int32 Line, EWebBrowserConsoleLogSeverity Serverity)
{
	UE_LOG(MarkdownStaticsLog, Warning, TEXT("Markdown Browser: %s (Source: %s:%d)"), *Message, *Source, Line);
}

bool SMarkdownAssetEditor::IsCurrentFileALocalFile() const
{
	UMarkdownLinkAsset* LinkAsset = Cast<UMarkdownLinkAsset>(MarkdownAsset);
	if (LinkAsset && !LinkAsset->URL.IsEmpty())
	{
		return !LinkAsset->URL.Contains(TEXT("://"));
	}
	return false;
}

//---------------------------------------------------------------------------------------------------------------------
// Helper: compute base href for relative resources
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

// Called when the dark/light template finishes loading
void SMarkdownAssetEditor::HandleBrowserLoadCompleted()
{
	bBrowserTemplateLoaded = true;

	UMarkdownLinkAsset* LinkAsset = Cast<UMarkdownLinkAsset>(MarkdownAsset);
	if (!LinkAsset) { return; }

	const FString BaseHref = ComputeBaseHref(LinkAsset->URL);
	if (WebBrowser.IsValid() && !BaseHref.IsEmpty())
	{
		const FString Script = FString::Printf(
			TEXT("(function(){var head=document.head||document.getElementsByTagName('head')[0]; if(!head){return;} var b=document.querySelector('base'); if(!b){b=document.createElement('base'); head.appendChild(b);} b.href='%s'; console.log('Set base to', b.href); if(window.refreshMarkdown){refreshMarkdown();}})();"),
			*BaseHref
		);
		WebBrowser->ExecuteJavascript(Script);
	}
}

// Open or refresh a link asset without forcing dirty unless URL changed
void SMarkdownAssetEditor::OpenMarkdownAssetLink(UMarkdownLinkAsset& LinkAsset, UMarkdownBinding& Binding, const FString& Url)
{
	bool bUrlChanged = (LinkAsset.URL != Url);
	if (bUrlChanged)
	{
		LinkAsset.URL = Url;
		// Mark dirty only if user actually changed the URL through the UI
		LinkAsset.MarkPackageDirty();
		UE_LOG(MarkdownStaticsLog, Log, TEXT("Markdown link URL changed -> marking dirty: %s"), *Url);
	}

	// Load file content (mirror) – DO NOT mark package dirty just for syncing external file
	FText FileText = FMarkdownAssetEditorModule::ReadTextFromFile(LinkAsset.URL);

	// Update asset's cached text only if different (no dirty flag)
	if (!FileText.EqualTo(LinkAsset.Text))
	{
		LinkAsset.Text = FileText;
	}

	// Push into binding (will not mark dirty unless user edits later)
	Binding.SetText(FileText);

	// If template already loaded inject/refresh base
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

	UE_LOG(MarkdownStaticsLog, Log, TEXT("MarkdownAssetEditor: Opened link '%s' (URLChanged=%s, TemplateLoaded=%s)"),
		*LinkAsset.URL,
		bUrlChanged ? TEXT("Yes") : TEXT("No"),
		bBrowserTemplateLoaded ? TEXT("Yes") : TEXT("No"));
}

#undef LOCTEXT_NAMESPACE
