#include "SceneOptimizerExport.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "GameFramework/Actor.h"

FString FSceneOptimizerExport::ImpactMetricToString(EImpactMetric Metric)
{
	switch (Metric)
	{
	case EImpactMetric::DrawCalls:           return TEXT("DrawCalls");
	case EImpactMetric::Triangles:           return TEXT("Triangles");
	case EImpactMetric::TextureMemoryMB:     return TEXT("TextureMemoryMB");
	case EImpactMetric::ShadowInvalidations: return TEXT("ShadowInvalidations");
	case EImpactMetric::AnimUpdateCost:      return TEXT("AnimUpdateCost");
	case EImpactMetric::ActorCount:          return TEXT("ActorCount");
	default:                                 return TEXT("Unclassified");
	}
}

bool FSceneOptimizerExport::ExportToJson(const TArray<FSceneIssuePtr>& Issues, const FString& FilePath)
{
	TArray<TSharedPtr<FJsonValue>> IssueArray;

	for (const FSceneIssuePtr& Issue : Issues)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("category"), LexToString(Issue->Category));
		Obj->SetStringField(TEXT("severity"), LexToString(Issue->Severity));
		Obj->SetStringField(TEXT("target"), Issue->TargetName);
		Obj->SetStringField(TEXT("description"), Issue->Description);
		Obj->SetBoolField(TEXT("autoFixable"), Issue->bAutoFixable);
		Obj->SetBoolField(TEXT("resolved"), Issue->bResolved);

		TSharedPtr<FJsonObject> ImpactObj = MakeShared<FJsonObject>();
		ImpactObj->SetStringField(TEXT("metric"), ImpactMetricToString(Issue->Impact.Metric));
		ImpactObj->SetNumberField(TEXT("estimatedMagnitude"), Issue->Impact.EstimatedMagnitude);
		ImpactObj->SetNumberField(TEXT("confidence"), Issue->Impact.Confidence);
		ImpactObj->SetNumberField(TEXT("priorityScore"), Issue->Impact.PriorityScore);
		Obj->SetObjectField(TEXT("impact"), ImpactObj);

		if (AActor* Actor = Issue->Actor.Get())
		{
			Obj->SetStringField(TEXT("actorPath"), Actor->GetPathName());
		}

		IssueArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("issueCount"), Issues.Num());
	Root->SetArrayField(TEXT("issues"), IssueArray);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer))
	{
		return false;
	}

	return FFileHelper::SaveStringToFile(Output, *FilePath);
}

bool FSceneOptimizerExport::ExportToCsv(const TArray<FSceneIssuePtr>& Issues, const FString& FilePath)
{
	auto EscapeCsv = [](const FString& In) -> FString
	{
		FString Escaped = In.Replace(TEXT("\""), TEXT("\"\""));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	};

	FString Csv = TEXT("Category,Severity,Target,Description,AutoFixable,Resolved,ImpactMetric,EstimatedMagnitude,Confidence,PriorityScore\n");

	for (const FSceneIssuePtr& Issue : Issues)
	{
		Csv += FString::Printf(
			TEXT("%s,%s,%s,%s,%s,%s,%s,%.2f,%d,%d\n"),
			LexToString(Issue->Category),
			LexToString(Issue->Severity),
			*EscapeCsv(Issue->TargetName),
			*EscapeCsv(Issue->Description),
			Issue->bAutoFixable ? TEXT("true") : TEXT("false"),
			Issue->bResolved ? TEXT("true") : TEXT("false"),
			*ImpactMetricToString(Issue->Impact.Metric),
			Issue->Impact.EstimatedMagnitude,
			Issue->Impact.Confidence,
			Issue->Impact.PriorityScore);
	}

	return FFileHelper::SaveStringToFile(Csv, *FilePath);
}
