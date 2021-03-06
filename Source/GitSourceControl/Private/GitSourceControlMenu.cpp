// Copyright (c) 2014-2018 Sebastien Rombauts (sebastien.rombauts@gmail.com)

#include "GitSourceControlPrivatePCH.h"

#include "GitSourceControlMenu.h"

#include "GitSourceControlModule.h"
#include "GitSourceControlProvider.h"
#include "GitSourceControlOperations.h"

#include "ISourceControlOperation.h"
#include "SourceControlOperations.h"

#include "LevelEditor.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Framework/Notifications/NotificationManager.h"
#include "EditorStyleSet.h"

#include "PackageTools.h"
#include "FileHelpers.h"

#include "Logging/MessageLog.h"

static const FName GitSourceControlMenuTabName("GitSourceControlMenu");

#define LOCTEXT_NAMESPACE "GitSourceControl"

void FGitSourceControlMenu::Register()
{
	// Register the extension with the level editor
	FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
	if (LevelEditorModule)
	{
		FLevelEditorModule::FLevelEditorMenuExtender ViewMenuExtender = FLevelEditorModule::FLevelEditorMenuExtender::CreateRaw(this, &FGitSourceControlMenu::OnExtendLevelEditorViewMenu);
		auto& MenuExtenders = LevelEditorModule->GetAllLevelEditorToolbarSourceControlMenuExtenders();
		MenuExtenders.Add(ViewMenuExtender);
		ViewMenuExtenderHandle = MenuExtenders.Last().GetHandle();
	}
}

void FGitSourceControlMenu::Unregister()
{
	// Unregister the level editor extensions
	FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
	if (LevelEditorModule)
	{
		LevelEditorModule->GetAllLevelEditorToolbarSourceControlMenuExtenders().RemoveAll([=](const FLevelEditorModule::FLevelEditorMenuExtender& Extender) { return Extender.GetHandle() == ViewMenuExtenderHandle; });
	}
}

bool FGitSourceControlMenu::IsSourceControlConnected() const
{
	const ISourceControlProvider& Provider = ISourceControlModule::Get().GetProvider();
	return Provider.IsEnabled() && Provider.IsAvailable();
}

void FGitSourceControlMenu::UnlinkSyncAndReloadPackages()
{
	// Prompt to save or discard all packages
	bool bOkToExit = false;
	bool bHadPackagesToSave = false;
	{
		const bool bPromptUserToSave = true;
		const bool bSaveMapPackages = true;
		const bool bSaveContentPackages = true;
		const bool bFastSave = false;
		const bool bNotifyNoPackagesSaved = false;
		const bool bCanBeDeclined = true; // If the user clicks "don't save" this will continue and lose their changes
		bOkToExit = FEditorFileUtils::SaveDirtyPackages(bPromptUserToSave, bSaveMapPackages, bSaveContentPackages, bFastSave, bNotifyNoPackagesSaved, bCanBeDeclined, &bHadPackagesToSave);
	}

	// bOkToExit can be true if the user selects to not save an asset by unchecking it and clicking "save"
	if (bOkToExit)
	{
		TArray<UPackage*> DirtyPackages;
		FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
		FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);
		bOkToExit = DirtyPackages.Num() == 0;
	}

	if (bOkToExit)
	{
		FGitSourceControlModule& GitSourceControl = FModuleManager::LoadModuleChecked<FGitSourceControlModule>("GitSourceControl");
		FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();

		// Unload all packages in Content directory
		TArray<FString> PackageRelativePaths;
		FPackageName::FindPackagesInDirectory(PackageRelativePaths, *FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir()));

		TArray<FString> PackageNames;
		PackageNames.Reserve(PackageRelativePaths.Num());
		for(const FString& Path : PackageRelativePaths)
		{
			FString PackageName;
			FString FailureReason;
			if (FPackageName::TryConvertFilenameToLongPackageName(Path, PackageName, &FailureReason))
			{
				PackageNames.Add(PackageName);
			}
			else
			{
				FMessageLog("GitSourceControl").Error(FText::FromString(FailureReason));
			}
		}

		// Inspired from ContentBrowserUtils.cpp ContentBrowserUtils::SyncPathsFromSourceControl()
		TArray<UPackage*> LoadedPackages = UnlinkPackages(PackageNames);

		// Execute a Source Control "Sync" synchronously, displaying an ongoing notification during the whole operation
		TSharedRef<FSync, ESPMode::ThreadSafe> SyncOperation = ISourceControlOperation::Create<FSync>();
		DisplayInProgressNotification(SyncOperation->GetInProgressString());
		const ECommandResult::Type Result = Provider.Execute(SyncOperation, TArray<FString>(), EConcurrency::Asynchronous);
		OnSourceControlOperationComplete(SyncOperation, Result);

		// Reload all packages
		ReloadPackages(LoadedPackages);
	}
	else
	{
		FMessageLog ErrorMessage("GitSourceControl");
		ErrorMessage.Warning(LOCTEXT("SourceControlMenu_Sync_Unsaved", "Save All Assets before attempting to Sync!"));
		ErrorMessage.Notify();
	}

}

TArray<UPackage*> FGitSourceControlMenu::UnlinkPackages(const TArray<FString>& InPackageNames)
{
	TArray<UPackage*> LoadedPackages;

	if (InPackageNames.Num() > 0)
	{
		// Form a list of loaded packages to reload...
		LoadedPackages.Reserve(InPackageNames.Num());
		for(const FString& PackageName : InPackageNames)
		{
			UPackage* Package = FindPackage(nullptr, *PackageName);
			if (Package)
			{
				LoadedPackages.Emplace(Package);

				// Detach the linkers of any loaded packages so that SCC can overwrite the files...
				if (!Package->IsFullyLoaded())
				{
					FlushAsyncLoading();
					Package->FullyLoad();
				}
				ResetLoaders(Package);
			}
		}
		UE_LOG(LogSourceControl, Log, TEXT("Reseted Loader for %d Packages"), LoadedPackages.Num());
	}

	return LoadedPackages;
}

void FGitSourceControlMenu::ReloadPackages(TArray<UPackage*>& InLoadedPackages)
{
	UE_LOG(LogSourceControl, Log, TEXT("Reloading %d Packages..."), InLoadedPackages.Num());

	// Syncing may have deleted some packages, so we need to unload those rather than re-load them...
	TArray<UPackage*> PackagesToUnload;
	InLoadedPackages.RemoveAll([&](UPackage* InPackage) -> bool
	{
		const FString PackageExtension = InPackage->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
		const FString PackageFilename = FPackageName::LongPackageNameToFilename(InPackage->GetName(), PackageExtension);
		if (!FPaths::FileExists(PackageFilename))
		{
			PackagesToUnload.Emplace(InPackage);
			return true; // remove package
		}
		return false; // keep package
	});

	// Hot-reload the new packages...
	PackageTools::ReloadPackages(InLoadedPackages);

	// Unload any deleted packages...
	PackageTools::UnloadPackages(PackagesToUnload);
}

void FGitSourceControlMenu::SyncClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		UnlinkSyncAndReloadPackages();
	}
	else
	{
		FMessageLog LogSourceControl("LogSourceControl");
		LogSourceControl.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		LogSourceControl.Notify();
	}
}

void FGitSourceControlMenu::PushClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		// Launch a "Push" Operation
		FGitSourceControlModule& GitSourceControl = FModuleManager::LoadModuleChecked<FGitSourceControlModule>("GitSourceControl");
		FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
		TSharedRef<FGitPush, ESPMode::ThreadSafe> PushOperation = ISourceControlOperation::Create<FGitPush>();
		ECommandResult::Type Result = Provider.Execute(PushOperation, TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
		if (Result == ECommandResult::Succeeded)
		{
			// Display an ongoing notification during the whole operation
			DisplayInProgressNotification(PushOperation->GetInProgressString());
		}
		else
		{
			// Report failure with a notification
			DisplayFailureNotification(PushOperation->GetName());
		}
	}
	else
	{
		FMessageLog LogSourceControl("LogSourceControl");
		LogSourceControl.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		LogSourceControl.Notify();
	}
}

void FGitSourceControlMenu::RefreshClicked()
{
	if (!OperationInProgressNotification.IsValid())
	{
		// Launch an "UpdateStatus" Operation
		FGitSourceControlModule& GitSourceControl = FModuleManager::LoadModuleChecked<FGitSourceControlModule>("GitSourceControl");
		FGitSourceControlProvider& Provider = GitSourceControl.GetProvider();
		TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> RefreshOperation = ISourceControlOperation::Create<FUpdateStatus>();
		RefreshOperation->SetCheckingAllFiles(true);
		RefreshOperation->SetGetOpenedOnly(true);
		ECommandResult::Type Result = Provider.Execute(RefreshOperation, TArray<FString>(), EConcurrency::Asynchronous, FSourceControlOperationComplete::CreateRaw(this, &FGitSourceControlMenu::OnSourceControlOperationComplete));
		if (Result == ECommandResult::Succeeded)
		{
			// Display an ongoing notification during the whole operation
			DisplayInProgressNotification(RefreshOperation->GetInProgressString());
		}
		else
		{
			// Report failure with a notification
			DisplayFailureNotification(RefreshOperation->GetName());
		}
	}
	else
	{
		FMessageLog LogSourceControl("LogSourceControl");
		LogSourceControl.Warning(LOCTEXT("SourceControlMenu_InProgress", "Source control operation already in progress"));
		LogSourceControl.Notify();
	}
}

// Display an ongoing notification during the whole operation
void FGitSourceControlMenu::DisplayInProgressNotification(const FText& InOperationInProgressString)
{
	if (!OperationInProgressNotification.IsValid())
	{
		FNotificationInfo Info(InOperationInProgressString);
		Info.bFireAndForget = false;
		Info.ExpireDuration = 0.0f;
		Info.FadeOutDuration = 1.0f;
		OperationInProgressNotification = FSlateNotificationManager::Get().AddNotification(Info);
		if (OperationInProgressNotification.IsValid())
		{
			OperationInProgressNotification.Pin()->SetCompletionState(SNotificationItem::CS_Pending);
		}
	}
}

// Remove the ongoing notification at the end of the operation
void FGitSourceControlMenu::RemoveInProgressNotification()
{
	if (OperationInProgressNotification.IsValid())
	{
		OperationInProgressNotification.Pin()->ExpireAndFadeout();
		OperationInProgressNotification.Reset();
	}
}

// Display a temporary success notification at the end of the operation
void FGitSourceControlMenu::DisplaySucessNotification(const FName& InOperationName)
{
	const FText NotificationText = FText::Format(
		LOCTEXT("SourceControlMenu_Success", "{0} operation was successful!"),
		FText::FromName(InOperationName)
	);
	FNotificationInfo Info(NotificationText);
	Info.bUseSuccessFailIcons = true;
	Info.Image = FEditorStyle::GetBrush(TEXT("NotificationList.SuccessImage"));
	FSlateNotificationManager::Get().AddNotification(Info);
	FMessageLog("LogSourceControl").Info(NotificationText);
}

// Display a temporary failure notification at the end of the operation
void FGitSourceControlMenu::DisplayFailureNotification(const FName& InOperationName)
{
	const FText NotificationText = FText::Format(
		LOCTEXT("SourceControlMenu_Failure", "Error: {0} operation failed!"),
		FText::FromName(InOperationName)
	);
	FNotificationInfo Info(NotificationText);
	Info.ExpireDuration = 8.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
	FMessageLog("LogSourceControl").Info(NotificationText);
}

void FGitSourceControlMenu::OnSourceControlOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult)
{
	RemoveInProgressNotification();

	// Report result with a notification
	if (InResult == ECommandResult::Succeeded)
	{
		DisplaySucessNotification(InOperation->GetName());
	}
	else
	{
		DisplayFailureNotification(InOperation->GetName());
	}
}

void FGitSourceControlMenu::AddMenuExtension(FMenuBuilder& Builder)
{
	Builder.AddMenuEntry(
		LOCTEXT("GitPush",				"Push"),
		LOCTEXT("GitPushTooltip",		"Push all local commits to the remote server."),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Submit"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::PushClicked),
			FCanExecuteAction() // TODO if origin configured
		)
	);

	Builder.AddMenuEntry(
		LOCTEXT("GitSync",				"Sync/Pull"),
		LOCTEXT("GitSyncTooltip",		"Update all files in the local repository to the latest version of the remote server."),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Sync"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::SyncClicked),
			FCanExecuteAction() // TODO if origin configured
		)
	);

	Builder.AddMenuEntry(
		LOCTEXT("GitRefresh",			"Refresh"),
		LOCTEXT("GitRefreshTooltip",	"Update the source control status of all files in the local repository."),
		FSlateIcon(FEditorStyle::GetStyleSetName(), "SourceControl.Actions.Refresh"),
		FUIAction(
			FExecuteAction::CreateRaw(this, &FGitSourceControlMenu::RefreshClicked),
			FCanExecuteAction()
		)
	);
}

TSharedRef<FExtender> FGitSourceControlMenu::OnExtendLevelEditorViewMenu(const TSharedRef<FUICommandList> CommandList)
{
	TSharedRef<FExtender> Extender(new FExtender());

	Extender->AddMenuExtension(
		"SourceControlActions",
		EExtensionHook::After,
		nullptr,
		FMenuExtensionDelegate::CreateRaw(this, &FGitSourceControlMenu::AddMenuExtension));

	return Extender;
}

#undef LOCTEXT_NAMESPACE
