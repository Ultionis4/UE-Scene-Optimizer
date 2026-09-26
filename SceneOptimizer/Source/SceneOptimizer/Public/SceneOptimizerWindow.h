#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"
#include "SceneIssue.h"

/**
 * Two-phase UI:
 *   1) "Run Audit" scans the level and lists every issue found, grouped by
 *      category and sorted by estimated PriorityScore within each category,
 *      with a per-category impact rollup shown up top.
 *   2) "Begin Review" walks the fixable issues one at a time, highest
 *      estimated impact first within each category, and only calls
 *      FSceneIssue::ApplyFix when the user clicks Approve.
 */
class SSceneOptimizerWindow : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSceneOptimizerWindow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	TArray<FSceneIssuePtr> AllIssues;      // full report, sorted by category then PriorityScore
	TArray<FSceneIssuePtr> ReviewQueue;    // subset walked during review (fixable only)
	int32 ReviewIndex = INDEX_NONE;
	bool bReviewing = false;

	TSharedPtr<SListView<FSceneIssuePtr>> ReportListView;
	TSharedPtr<class STextBlock> StatusText;
	TSharedPtr<class STextBlock> RollupText;
	TSharedPtr<class STextBlock> ReviewCategoryText;
	TSharedPtr<class STextBlock> ReviewDescriptionText;
	TSharedPtr<class SWidget> ReviewPanel;

	FReply OnRunAuditClicked();
	FReply OnBeginReviewClicked();
	FReply OnApproveClicked();
	FReply OnSkipClicked();
	FReply OnSkipCategoryClicked();
	FReply OnStopReviewClicked();
	FReply OnExportJsonClicked();
	FReply OnExportCsvClicked();

	void BuildReviewQueue();
	void AdvanceReview();
	void RefreshReviewPanel();
	void RefreshStatusText();
	void RefreshRollupText();
	EVisibility GetReviewPanelVisibility() const;

	TSharedRef<ITableRow> OnGenerateRow(FSceneIssuePtr Item, const TSharedRef<STableViewBase>& OwnerTable);

	static ESceneIssueCategory CategoryOrder(int32 Index);
	static FString FormatImpact(const struct FSceneIssueImpact& Impact);
	static FString ConfidenceLabel(uint8 Confidence);
};
