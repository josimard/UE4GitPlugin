// Copyright (c) 2014-2018 Sebastien Rombauts (sebastien.rombauts@gmail.com)

#pragma once

#include "CoreMinimal.h"
#include "ISourceControlProvider.h"

class FToolBarBuilder;
class FMenuBuilder;

/** Git extension of the Source Control toolbar menu */
class FGitSourceControlMenu
{
public:
	void Register();
	void Unregister();
	
	/** This functions will be bound to appropriate Command. */
	void PushClicked();
	void SyncClicked();
	void RefreshClicked();

private:
	bool IsSourceControlConnected() const;

	void				UnlinkSyncAndReloadPackages();
	TArray<UPackage*>	UnlinkPackages(const TArray<FString>& InPackageNames);
	void				ReloadPackages(TArray<UPackage*>& InLoadedPackages);

	void AddMenuExtension(FMenuBuilder& Builder);

	TSharedRef<class FExtender> OnExtendLevelEditorViewMenu(const TSharedRef<class FUICommandList> CommandList);

	void DisplayInProgressNotification(const FText& InOperationInProgressString);
	void RemoveInProgressNotification();
	void DisplaySucessNotification(const FName& InOperationName);
	void DisplayFailureNotification(const FName& InOperationName);

private:
	FDelegateHandle ViewMenuExtenderHandle;

	/** Current source control operation from extended menu if any */
	TWeakPtr<class SNotificationItem> OperationInProgressNotification;

	/** Delegate called when a source control operation has completed */
	void OnSourceControlOperationComplete(const FSourceControlOperationRef& InOperation, ECommandResult::Type InResult);
};
