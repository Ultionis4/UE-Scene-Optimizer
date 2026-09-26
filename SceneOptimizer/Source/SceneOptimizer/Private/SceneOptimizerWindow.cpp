#include "SceneOptimizerWindow.h"

#include "SceneAuditor.h"
#include "SceneOptimizerExport.h"
#include "Editor.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Views/STableRow.h"
#include "EditorStyleSet.h"

#define LOCTEXT_NAMESPACE "SceneOptimizerWindow"

namespace
{
	FText SeverityColorTag(ESceneIssueSeverity Severity)
	{
		switch (Severity)
		{
		case ESceneIssueSeverity::Critical: return LOCTEXT("SevCritical", "[Critical]");
		case ESceneIssueSeverity::Warning: return LOCTEXT("SevWarning", "[Warning]");
		default: return LOCTEXT("SevInfo", "[Info]");
		}
	}
}

ESceneIssueCategory SSceneOptimizerWindow::CategoryOrder(int32 Index)
{
	// Fixed walk order for the review phase: geometry first (usually the biggest
	// win), then lighting, then textures, then anything uncategorized.
	static const ESceneIssueCategory Order[] =
	{
		ESceneIssueCategory::Geometry,
		ESceneIssueCategory::Lighting,
		ESceneIssueCategory::Textures,
		ESceneIssueCategory::Misc
	};
	return Order[FMath::Clamp(Index, 0, 3)];
}

FString SSceneOptimizerWindow::ConfidenceLabel(uint8 Confidence)
{
	if (Confidence < 40) return TEXT("low");
	if (Confidence < 70) return TEXT("medium");
	return TEXT("high");
}

FString SSceneOptimizerWindow::FormatImpact(const FSceneIssueImpact& Impact)
{
	FString MagnitudePart;
	switch (Impact.Metric)
	{
	case EImpactMetric::Triangles:
		MagnitudePart = FString::Printf(TEXT("~%s triangles addressable"), *FText::AsNumber(FMath::RoundToInt(Impact.EstimatedMagnitude)).ToString());
		break;
	case EImpactMetric::DrawCalls:
		MagnitudePart = FString::Printf(TEXT("~%d draw calls saved"), FMath::RoundToInt(Impact.EstimatedMagnitude));
		break;
	case EImpactMetric::TextureMemoryMB:
		MagnitudePart = FString::Printf(TEXT("~%.0f MB"), Impact.EstimatedMagnitude);
		break;
	case EImpactMetric::ShadowInvalidations:
		MagnitudePart = FString::Printf(TEXT("~%d shadow-invalidation source(s)"), FMath::RoundToInt(Impact.EstimatedMagnitude));
		break;
	case EImpactMetric::AnimUpdateCost:
		MagnitudePart = FString::Printf(TEXT("~%d animated actor(s)"), FMath::RoundToInt(Impact.EstimatedMagnitude));
		break;
	case EImpactMetric::ActorCount:
		MagnitudePart = FString::Printf(TEXT("~%d actor(s)"), FMath::RoundToInt(Impact.EstimatedMagnitude));
		break;
	default:
		MagnitudePart = TEXT("qualitative only");
		break;
	}

	return FString::Printf(TEXT("Estimated impact: %s (confidence: %s, priority %d/100)"),
		*MagnitudePart, *ConfidenceLabel(Impact.Confidence), Impact.PriorityScore);
}

void SSceneOptimizerWindow::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)

		// --- Top bar: run audit / begin review / export ---
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("RunAudit", "Run Audit"))
				.OnClicked(this, &SSceneOptimizerWindow::OnRunAuditClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("BeginReview", "Begin Review"))
				.IsEnabled_Lambda([this]() { return AllIssues.Num() > 0 && !bReviewing; })
				.OnClicked(this, &SSceneOptimizerWindow::OnBeginReviewClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("ExportJson", "Export JSON"))
				.IsEnabled_Lambda([this]() { return AllIssues.Num() > 0; })
				.OnClicked(this, &SSceneOptimizerWindow::OnExportJsonClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("ExportCsv", "Export CSV"))
				.IsEnabled_Lambda([this]() { return AllIssues.Num() > 0; })
				.OnClicked(this, &SSceneOptimizerWindow::OnExportCsvClicked)
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8, 0)
		[
			SAssignNew(StatusText, STextBlock)
			.Text(LOCTEXT("StatusIdle", "Run an audit to scan the currently open level."))
		]

		// --- Per-category impact rollup, shown after a run ---
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8, 4)
		[
			SAssignNew(RollupText, STextBlock)
			.AutoWrapText(true)
		]

		// --- Report list (phase 1) ---
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(8)
		[
			SAssignNew(ReportListView, SListView<FSceneIssuePtr>)
			.ListItemsSource(&AllIssues)
			.OnGenerateRow(this, &SSceneOptimizerWindow::OnGenerateRow)
			.SelectionMode(ESelectionMode::Single)
		]

		// --- Review panel (phase 2), only visible while stepping through issues ---
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(8)
		[
			SNew(SBorder)
			.Visibility(this, &SSceneOptimizerWindow::GetReviewPanelVisibility)
			.Padding(10)
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
				[
					SAssignNew(ReviewCategoryText, STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
				]

				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 8)
				[
					SAssignNew(ReviewDescriptionText, STextBlock)
					.AutoWrapText(true)
				]

				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("Approve", "Approve && Apply"))
						.OnClicked(this, &SSceneOptimizerWindow::OnApproveClicked)
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("Skip", "Skip"))
						.OnClicked(this, &SSceneOptimizerWindow::OnSkipClicked)
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("SkipCategory", "Skip Rest of Category"))
						.OnClicked(this, &SSceneOptimizerWindow::OnSkipCategoryClicked)
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("StopReview", "Stop Review"))
						.OnClicked(this, &SSceneOptimizerWindow::OnStopReviewClicked)
					]
				]
			]
		]
	];
}

FReply SSceneOptimizerWindow::OnRunAuditClicked()
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	if (!World)
	{
		StatusText->SetText(LOCTEXT("NoWorld", "No editor world found — is a level open?"));
		return FReply::Handled();
	}

	// FSceneAuditor::RunGeneralAudit already sorts by category then by
	// estimated PriorityScore (highest first) and shows its own cancellable
	// progress dialog for large levels.
	AllIssues = FSceneAuditor::RunGeneralAudit(World, /*bShowProgressDialog=*/ true);

	bReviewing = false;
	ReviewQueue.Reset();
	ReviewIndex = INDEX_NONE;

	RefreshStatusText();
	RefreshRollupText();
	ReportListView->RequestListRefresh();
	return FReply::Handled();
}

void SSceneOptimizerWindow::RefreshStatusText()
{
	int32 Fixable = 0;
	for (const FSceneIssuePtr& Issue : AllIssues)
	{
		if (Issue->bAutoFixable)
		{
			Fixable++;
		}
	}

	StatusText->SetText(FText::Format(
		LOCTEXT("StatusFmt", "{0} issue(s) found, {1} auto-fixable. Click Begin Review to walk through them, highest estimated impact first within each category."),
		AllIssues.Num(), Fixable));
}

void SSceneOptimizerWindow::RefreshRollupText()
{
	struct FCatStats
	{
		int32 Count = 0;
		TMap<EImpactMetric, float> MagnitudeByMetric;
	};
	TMap<ESceneIssueCategory, FCatStats> Stats;

	for (const FSceneIssuePtr& Issue : AllIssues)
	{
		FCatStats& S = Stats.FindOrAdd(Issue->Category);
		S.Count++;
		if (Issue->Impact.Metric != EImpactMetric::Unclassified)
		{
			S.MagnitudeByMetric.FindOrAdd(Issue->Impact.Metric) += Issue->Impact.EstimatedMagnitude;
		}
	}

	FString Result;
	for (int32 CatIdx = 0; CatIdx < 4; ++CatIdx)
	{
		const ESceneIssueCategory Cat = CategoryOrder(CatIdx);
		const FCatStats* S = Stats.Find(Cat);
		if (!S || S->Count == 0)
		{
			continue;
		}

		FString MetricsPart;
		for (const TPair<EImpactMetric, float>& Pair : S->MagnitudeByMetric)
		{
			FSceneIssueImpact TempImpact;
			TempImpact.Metric = Pair.Key;
			TempImpact.EstimatedMagnitude = Pair.Value;
			const FString OneMetric = FormatImpact(TempImpact);
			// FormatImpact returns the full "Estimated impact: X (confidence...)" sentence;
			// we only want the magnitude fragment for this rollup, so re-derive it directly.
			FString MagnitudeOnly;
			switch (Pair.Key)
			{
			case EImpactMetric::Triangles: MagnitudeOnly = FString::Printf(TEXT("~%s triangles"), *FText::AsNumber(FMath::RoundToInt(Pair.Value)).ToString()); break;
			case EImpactMetric::DrawCalls: MagnitudeOnly = FString::Printf(TEXT("~%d draw calls"), FMath::RoundToInt(Pair.Value)); break;
			case EImpactMetric::TextureMemoryMB: MagnitudeOnly = FString::Printf(TEXT("~%.0f MB"), Pair.Value); break;
			case EImpactMetric::ShadowInvalidations: MagnitudeOnly = FString::Printf(TEXT("~%d shadow sources"), FMath::RoundToInt(Pair.Value)); break;
			case EImpactMetric::AnimUpdateCost: MagnitudeOnly = FString::Printf(TEXT("~%d animated actors"), FMath::RoundToInt(Pair.Value)); break;
			case EImpactMetric::ActorCount: MagnitudeOnly = FString::Printf(TEXT("~%d actors"), FMath::RoundToInt(Pair.Value)); break;
			default: break;
			}

			if (!MagnitudeOnly.IsEmpty())
			{
				MetricsPart += MetricsPart.IsEmpty() ? MagnitudeOnly : FString::Printf(TEXT(", %s"), *MagnitudeOnly);
			}
		}

		Result += FString::Printf(TEXT("%s: %d issue(s)%s\n"),
			LexToString(Cat), S->Count,
			MetricsPart.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (est. %s addressable)"), *MetricsPart));
	}

	RollupText->SetText(FText::FromString(Result));
}

FReply SSceneOptimizerWindow::OnBeginReviewClicked()
{
	BuildReviewQueue();
	if (ReviewQueue.Num() == 0)
	{
		StatusText->SetText(LOCTEXT("NothingFixable", "No auto-fixable issues to review — check the report list above for manual items."));
		return FReply::Handled();
	}

	bReviewing = true;
	ReviewIndex = 0;
	RefreshReviewPanel();
	return FReply::Handled();
}

void SSceneOptimizerWindow::BuildReviewQueue()
{
	ReviewQueue.Reset();
	// Only fixable issues go into the interactive queue; report-only issues
	// (e.g. instancing candidates without a safe auto-fix, missing collision,
	// masked/PDO materials on Nanite) stay visible in the list above for the
	// user to handle by hand. AllIssues is already sorted by category then by
	// PriorityScore descending, so this preserves highest-impact-first order.
	for (int32 CatIdx = 0; CatIdx < 4; ++CatIdx)
	{
		const ESceneIssueCategory Cat = CategoryOrder(CatIdx);
		for (const FSceneIssuePtr& Issue : AllIssues)
		{
			if (Issue->Category == Cat && Issue->bAutoFixable && !Issue->bResolved)
			{
				ReviewQueue.Add(Issue);
			}
		}
	}
}

void SSceneOptimizerWindow::RefreshReviewPanel()
{
	if (!ReviewQueue.IsValidIndex(ReviewIndex))
	{
		bReviewing = false;
		StatusText->SetText(LOCTEXT("ReviewDone", "Review complete. Run Audit again to check for anything remaining."));
		return;
	}

	const FSceneIssuePtr& Issue = ReviewQueue[ReviewIndex];
	ReviewCategoryText->SetText(FText::Format(
		LOCTEXT("ReviewHeaderFmt", "Reviewing {0}  —  item {1} of {2}   {3}"),
		FText::FromString(LexToString(Issue->Category)),
		ReviewIndex + 1,
		ReviewQueue.Num(),
		SeverityColorTag(Issue->Severity)));

	ReviewDescriptionText->SetText(FText::Format(
		LOCTEXT("ReviewBodyFmt", "{0}\n\nTarget: {1}\n{2}"),
		FText::FromString(Issue->Description),
		FText::FromString(Issue->TargetName),
		FText::FromString(FormatImpact(Issue->Impact))));
}

FReply SSceneOptimizerWindow::OnApproveClicked()
{
	if (ReviewQueue.IsValidIndex(ReviewIndex))
	{
		FSceneIssuePtr Issue = ReviewQueue[ReviewIndex];
		if (Issue->ApplyFix)
		{
			// Each ApplyFix implementation wraps its own edit in a
			// FScopedTransaction, so the result is undoable via Ctrl+Z like
			// any other editor action.
			Issue->bResolved = Issue->ApplyFix();
		}
	}
	AdvanceReview();
	return FReply::Handled();
}

FReply SSceneOptimizerWindow::OnSkipClicked()
{
	AdvanceReview();
	return FReply::Handled();
}

FReply SSceneOptimizerWindow::OnSkipCategoryClicked()
{
	if (ReviewQueue.IsValidIndex(ReviewIndex))
	{
		const ESceneIssueCategory CurrentCat = ReviewQueue[ReviewIndex]->Category;
		while (ReviewQueue.IsValidIndex(ReviewIndex) && ReviewQueue[ReviewIndex]->Category == CurrentCat)
		{
			ReviewIndex++;
		}
	}
	RefreshReviewPanel();
	ReportListView->RequestListRefresh();
	return FReply::Handled();
}

FReply SSceneOptimizerWindow::OnStopReviewClicked()
{
	bReviewing = false;
	StatusText->SetText(LOCTEXT("ReviewStopped", "Review stopped. Click Begin Review to resume from where you left off."));
	ReportListView->RequestListRefresh();
	return FReply::Handled();
}

FReply SSceneOptimizerWindow::OnExportJsonClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return FReply::Handled();
	}

	TArray<FString> OutFiles;
	const bool bSaved = DesktopPlatform->SaveFileDialog(
		FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
		TEXT("Export Scene Optimizer Report"), TEXT(""), TEXT("SceneOptimizerReport.json"),
		TEXT("JSON Files (*.json)|*.json"), 0, OutFiles);

	if (bSaved && OutFiles.Num() > 0)
	{
		const bool bOk = FSceneOptimizerExport::ExportToJson(AllIssues, OutFiles[0]);
		StatusText->SetText(bOk
			? FText::Format(LOCTEXT("ExportOk", "Exported {0} issue(s) to {1}"), AllIssues.Num(), FText::FromString(OutFiles[0]))
			: LOCTEXT("ExportFail", "Export failed — check the log for details."));
	}
	return FReply::Handled();
}

FReply SSceneOptimizerWindow::OnExportCsvClicked()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (!DesktopPlatform)
	{
		return FReply::Handled();
	}

	TArray<FString> OutFiles;
	const bool bSaved = DesktopPlatform->SaveFileDialog(
		FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
		TEXT("Export Scene Optimizer Report"), TEXT(""), TEXT("SceneOptimizerReport.csv"),
		TEXT("CSV Files (*.csv)|*.csv"), 0, OutFiles);

	if (bSaved && OutFiles.Num() > 0)
	{
		const bool bOk = FSceneOptimizerExport::ExportToCsv(AllIssues, OutFiles[0]);
		StatusText->SetText(bOk
			? FText::Format(LOCTEXT("ExportOk", "Exported {0} issue(s) to {1}"), AllIssues.Num(), FText::FromString(OutFiles[0]))
			: LOCTEXT("ExportFail", "Export failed — check the log for details."));
	}
	return FReply::Handled();
}

void SSceneOptimizerWindow::AdvanceReview()
{
	ReviewIndex++;
	RefreshReviewPanel();
	ReportListView->RequestListRefresh();
}

EVisibility SSceneOptimizerWindow::GetReviewPanelVisibility() const
{
	return bReviewing ? EVisibility::Visible : EVisibility::Collapsed;
}

TSharedRef<ITableRow> SSceneOptimizerWindow::OnGenerateRow(FSceneIssuePtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const FString Line = FString::Printf(
		TEXT("[%s] %s %s — %s%s\n    %s"),
		LexToString(Item->Category),
		*SeverityColorTag(Item->Severity).ToString(),
		*Item->TargetName,
		*Item->Description,
		Item->bResolved ? TEXT("  (fixed)") : (Item->bAutoFixable ? TEXT("") : TEXT("  (manual)")),
		*FormatImpact(Item->Impact));

	return SNew(STableRow<FSceneIssuePtr>, OwnerTable)
	[
		SNew(STextBlock)
		.Text(FText::FromString(Line))
		.AutoWrapText(true)
		.ColorAndOpacity(Item->bResolved ? FLinearColor::Green : FLinearColor::White)
	];
}

#undef LOCTEXT_NAMESPACE
