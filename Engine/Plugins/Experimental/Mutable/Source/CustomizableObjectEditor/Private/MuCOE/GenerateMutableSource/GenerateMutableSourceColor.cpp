// Copyright Epic Games, Inc. All Rights Reserved.

#include "MuCOE/GenerateMutableSource/GenerateMutableSourceColor.h"

#include "MuCOE/CustomizableObjectCompiler.h"
#include "MuCOE/EdGraphSchema_CustomizableObject.h"
#include "MuCOE/GenerateMutableSource/GenerateMutableSourceFloat.h"
#include "MuCOE/GenerateMutableSource/GenerateMutableSourceImage.h"
#include "MuCOE/GenerateMutableSource/GenerateMutableSourceTable.h"
#include "MuCOE/GraphTraversal.h"
#include "MuCOE/Nodes/CustomizableObjectNodeColorArithmeticOp.h"
#include "MuCOE/Nodes/CustomizableObjectNodeColorConstant.h"
#include "MuCOE/Nodes/CustomizableObjectNodeColorFromFloats.h"
#include "MuCOE/Nodes/CustomizableObjectNodeColorParameter.h"
#include "MuCOE/Nodes/CustomizableObjectNodeColorSwitch.h"
#include "MuCOE/Nodes/CustomizableObjectNodeColorVariation.h"
#include "MuCOE/Nodes/CustomizableObjectNodeTable.h"
#include "MuCOE/Nodes/CustomizableObjectNodeTextureSample.h"
#include "MuT/NodeColourArithmeticOperation.h"
#include "MuT/NodeColourConstant.h"
#include "MuT/NodeColourFromScalars.h"
#include "MuT/NodeColourParameter.h"
#include "MuT/NodeColourSampleImage.h"
#include "MuT/NodeColourSwitch.h"
#include "MuT/NodeColourTable.h"
#include "MuT/NodeColourVariation.h"
#include "MuT/NodeImageBinarise.h"


#define LOCTEXT_NAMESPACE "CustomizableObjectEditor"


mu::NodeColourPtr GenerateMutableSourceColor(const UEdGraphPin* Pin, FMutableGraphGenerationContext& GenerationContext)
{
	check(Pin)
	RETURN_ON_CYCLE(*Pin, GenerationContext)

	CheckNumOutputs(*Pin, GenerationContext);
	
	const UEdGraphSchema_CustomizableObject* Schema = GetDefault<UEdGraphSchema_CustomizableObject>();

	UCustomizableObjectNode* Node = CastChecked<UCustomizableObjectNode>(Pin->GetOwningNode());

	const FGeneratedKey Key(reinterpret_cast<void*>(&GenerateMutableSourceColor), *Pin, *Node, GenerationContext);
	if (const FGeneratedData* Generated = GenerationContext.Generated.Find(Key))
	{
		return static_cast<mu::NodeColour*>(Generated->Node.get());
	}

	if (Node->IsNodeOutDatedAndNeedsRefresh())
	{
		Node->SetRefreshNodeWarning();
	}

	mu::NodeColourPtr Result;

	if (const UCustomizableObjectNodeColorConstant* TypedNodeColorConst = Cast<UCustomizableObjectNodeColorConstant>(Node))
	{
		mu::NodeColourConstantPtr ColorNode = new mu::NodeColourConstant();
		Result = ColorNode;

		ColorNode->SetValue(TypedNodeColorConst->Value);
	}

	else if (const UCustomizableObjectNodeColorParameter* TypedNodeColorParam = Cast<UCustomizableObjectNodeColorParameter>(Node))
	{
		mu::NodeColourParameterPtr ColorNode = new mu::NodeColourParameter();
		Result = ColorNode;

		GenerationContext.AddParameterNameUnique(Node, TypedNodeColorParam->ParameterName);

		ColorNode->SetName(TypedNodeColorParam->ParameterName);
		ColorNode->SetUid(GenerationContext.GetNodeIdUnique(Node).ToString());
		ColorNode->SetDefaultValue(TypedNodeColorParam->DefaultValue);

		GenerationContext.ParameterUIDataMap.Add(TypedNodeColorParam->ParameterName, FParameterUIData(
			TypedNodeColorParam->ParameterName,
			TypedNodeColorParam->ParamUIMetadata,
			EMutableParameterType::Color));
	}

	else if (const UCustomizableObjectNodeColorSwitch* TypedNodeColorSwitch = Cast<UCustomizableObjectNodeColorSwitch>(Node))
	{
		Result = [&]()
		{
			if (const int32 NumParameters = FollowInputPinArray(*TypedNodeColorSwitch->SwitchParameter()).Num();
				NumParameters != 1)
			{
				const FText Message = NumParameters
					? LOCTEXT("NoEnumParamInSwitch", "Switch nodes must have an enum switch parameter. Please connect an enum and refesh the switch node.")
					: LOCTEXT("InvalidEnumInSwitch", "Switch nodes must have a single enum with all the options inside. Please remove all the enums but one and refresh the switch node.");

				GenerationContext.Compiler->CompilerLog(Message, Node);
				return Result;
			}

			const UEdGraphPin* EnumPin = FollowInputPin(*TypedNodeColorSwitch->SwitchParameter());
			mu::NodeScalarPtr SwitchParam = EnumPin ? GenerateMutableSourceFloat(EnumPin, GenerationContext) : nullptr;

			// Switch Param not generated
			if (!SwitchParam)
			{
				// Warn about a failure.
				if (EnumPin)
				{
					const FText Message = LOCTEXT("FailedToGenerateSwitchParam", "Could not generate switch enum parameter. Please refesh the switch node and connect an enum.");
					GenerationContext.Compiler->CompilerLog(Message, Node);
				}

				return Result;
			}

			if (SwitchParam->GetType() != mu::NodeScalarEnumParameter::GetStaticType())
			{
				const FText Message = LOCTEXT("WrongSwitchParamType", "Switch parameter of incorrect type.");
				GenerationContext.Compiler->CompilerLog(Message, Node);

				return Result;
			}

			const int32 NumSwitchOptions = TypedNodeColorSwitch->GetNumElements();

			mu::NodeScalarEnumParameter* EnumParameter = static_cast<mu::NodeScalarEnumParameter*>(SwitchParam.get());
			if (NumSwitchOptions != EnumParameter->GetValueCount())
			{
				const FText Message = LOCTEXT("MismatchedSwitch", "Switch enum and switch node have different number of options. Please refresh the switch node to make sure the outcomes are labeled properly.");
				GenerationContext.Compiler->CompilerLog(Message, Node);
			}

			mu::NodeColourSwitchPtr SwitchNode = new mu::NodeColourSwitch;
			SwitchNode->SetParameter(SwitchParam);
			SwitchNode->SetOptionCount(NumSwitchOptions);

			for (int SelectorIndex = 0; SelectorIndex < NumSwitchOptions; ++SelectorIndex)
			{
				const UEdGraphPin* const ColorPin = TypedNodeColorSwitch->GetElementPin(SelectorIndex);
				if (const UEdGraphPin* ConnectedPin = FollowInputPin(*ColorPin))
				{
					SwitchNode->SetOption(SelectorIndex, GenerateMutableSourceColor(ConnectedPin, GenerationContext));
				}
			}

			return static_cast<mu::NodeColourPtr>(SwitchNode);
		}(); // invoke lambda;
	}

	else if (const UCustomizableObjectNodeTextureSample* TypedNodeTexSample = Cast<UCustomizableObjectNodeTextureSample>(Node))
	{
		mu::NodeColourSampleImagePtr ColorNode = new mu::NodeColourSampleImage();
		Result = ColorNode;

		if (const UEdGraphPin* ConnectedPin = FollowInputPin(*TypedNodeTexSample->TexturePin()))
		{
			mu::NodeImagePtr TextureNode = GenerateMutableSourceImage(ConnectedPin, GenerationContext, 0);
			ColorNode->SetImage(TextureNode);
		}

		if (const UEdGraphPin* ConnectedPin = FollowInputPin(*TypedNodeTexSample->XPin()))
		{
			mu::NodeScalarPtr XNode = GenerateMutableSourceFloat(ConnectedPin, GenerationContext);
			ColorNode->SetX(XNode);
		}

		if (const UEdGraphPin* ConnectedPin = FollowInputPin(*TypedNodeTexSample->YPin()))
		{
			mu::NodeScalarPtr YNode = GenerateMutableSourceFloat(ConnectedPin, GenerationContext);
			ColorNode->SetY(YNode);
		}
	}

	else if (const UCustomizableObjectNodeColorArithmeticOp* TypedNodeColorArith = Cast<UCustomizableObjectNodeColorArithmeticOp>(Node))
	{
		mu::NodeColourArithmeticOperationPtr OpNode = new mu::NodeColourArithmeticOperation();
		Result = OpNode;

		if (const UEdGraphPin* ConnectedPin = FollowInputPin(*TypedNodeColorArith->XPin()))
		{
			mu::NodeColourPtr XNode = GenerateMutableSourceColor(ConnectedPin, GenerationContext);
			OpNode->SetA(XNode);
		}

		if (const UEdGraphPin* ConnectedPin = FollowInputPin(*TypedNodeColorArith->YPin()))
		{
			mu::NodeColourPtr YNode = GenerateMutableSourceColor(ConnectedPin, GenerationContext);
			OpNode->SetB(YNode);
		}

		switch (TypedNodeColorArith->Operation)
		{
		case EColorArithmeticOperation::E_Add:
			OpNode->SetOperation(mu::NodeColourArithmeticOperation::OPERATION::AO_ADD);
			break;

		case EColorArithmeticOperation::E_Sub:
			OpNode->SetOperation(mu::NodeColourArithmeticOperation::OPERATION::AO_SUBTRACT);
			break;

		case EColorArithmeticOperation::E_Mul:
			OpNode->SetOperation(mu::NodeColourArithmeticOperation::OPERATION::AO_MULTIPLY);
			break;

		case EColorArithmeticOperation::E_Div:
			OpNode->SetOperation(mu::NodeColourArithmeticOperation::OPERATION::AO_DIVIDE);
			break;
		}
	}

	else if (const UCustomizableObjectNodeColorFromFloats* TypedNodeFrom = Cast<UCustomizableObjectNodeColorFromFloats>(Node))
	{
		mu::NodeColourFromScalarsPtr OpNode = new mu::NodeColourFromScalars();
		Result = OpNode;

		if (const UEdGraphPin* ConnectedPin = FollowInputPin(*TypedNodeFrom->RPin()))
		{
			mu::NodeScalarPtr FloatNode = GenerateMutableSourceFloat(ConnectedPin, GenerationContext);
			OpNode->SetX(FloatNode);
		}

		if (const UEdGraphPin* ConnectedPin = FollowInputPin(*TypedNodeFrom->GPin()))
		{
			mu::NodeScalarPtr FloatNode = GenerateMutableSourceFloat(ConnectedPin, GenerationContext);
			OpNode->SetY(FloatNode);
		}

		if (const UEdGraphPin* ConnectedPin = FollowInputPin(*TypedNodeFrom->BPin()))
		{
			mu::NodeScalarPtr FloatNode = GenerateMutableSourceFloat(ConnectedPin, GenerationContext);
			OpNode->SetZ(FloatNode);
		}

		if (const UEdGraphPin* ConnectedPin = FollowInputPin(*TypedNodeFrom->APin()))
		{
			mu::NodeScalarPtr FloatNode = GenerateMutableSourceFloat(ConnectedPin, GenerationContext);
			OpNode->SetW(FloatNode);
		}
	}

	else if (const UCustomizableObjectNodeColorVariation* TypedNodeColorVar = Cast<const UCustomizableObjectNodeColorVariation>(Node))
	{
		mu::NodeColourVariationPtr ColorNode = new mu::NodeColourVariation();
		Result = ColorNode;

		if (const UEdGraphPin* ConnectedPin = FollowInputPin(*TypedNodeColorVar->DefaultPin()))
		{
			mu::NodeColourPtr ChildNode = GenerateMutableSourceColor(ConnectedPin, GenerationContext);
			if (ChildNode)
			{
				ColorNode->SetDefaultColour(ChildNode.get());
			}
			else
			{
				GenerationContext.Compiler->CompilerLog(LOCTEXT("ColorFailed", "Color generation failed."), Node);
			}
		}

		const int32 NumVariations = TypedNodeColorVar->GetNumVariations();
		ColorNode->SetVariationCount(NumVariations);
		for (int VariationIndex = 0; VariationIndex < NumVariations; ++VariationIndex)
		{
			const UEdGraphPin* VariationPin = TypedNodeColorVar->VariationPin(VariationIndex);
			if (!VariationPin) continue;

			ColorNode->SetVariationTag(VariationIndex, TypedNodeColorVar->GetVariation(VariationIndex).Tag);
			if (const UEdGraphPin* ConnectedPin = FollowInputPin(*VariationPin))
			{
				mu::NodeColourPtr ChildNode = GenerateMutableSourceColor(ConnectedPin, GenerationContext);
				ColorNode->SetVariationColour(VariationIndex, ChildNode.get());
			}
		}
	}

	else if (const UCustomizableObjectNodeTable* TypedNodeTable = Cast<UCustomizableObjectNodeTable>(Node))
	{
		//This node will add a white color in case of error
		mu::NodeColourConstantPtr WhiteColorNode = new mu::NodeColourConstant();
		WhiteColorNode->SetValue(FVector4f(1.0f, 1.0f, 1.0f, 1.0f));

		Result = WhiteColorNode;

		bool bSuccess = true;
		UDataTable* DataTable = GetDataTable(TypedNodeTable, GenerationContext);

		if (DataTable)
		{
			FString ColumnName = Pin->PinFriendlyName.ToString();
			FProperty* Property = DataTable->FindTableProperty(FName(*ColumnName));

			if (!Property)
			{
				FString Msg = FString::Printf(TEXT("Couldn't find the column [%s] in the data table's struct."), *ColumnName);
				GenerationContext.Compiler->CompilerLog(FText::FromString(Msg), Node);

				bSuccess = false;
			}

			if (bSuccess)
			{
				// Generating a new data table if not exists
				mu::TablePtr Table;
				Table = GenerateMutableSourceTable(DataTable, TypedNodeTable, GenerationContext);

				if (Table)
				{
					mu::NodeColourTablePtr ColorTableNode = new mu::NodeColourTable();

					// Generating a new Color column if not exists
					if (Table->FindColumn(ColumnName) == INDEX_NONE)
					{
						int32 Dummy = -1; // TODO MTBL-1512
						bool Dummy2 = false;
						bSuccess = GenerateTableColumn(TypedNodeTable, Pin, Table, ColumnName, Property, Dummy, Dummy, GenerationContext.CurrentLOD, Dummy, Dummy2, GenerationContext);

						if (!bSuccess)
						{
							FString Msg = FString::Printf(TEXT("Failed to generate the mutable table column [%s]"), *ColumnName);
							GenerationContext.Compiler->CompilerLog(FText::FromString(Msg), Node);
						}
					}

					if (bSuccess)
					{
						Result = ColorTableNode;

						ColorTableNode->SetTable(Table);
						ColorTableNode->SetColumn(ColumnName);
						ColorTableNode->SetParameterName(TypedNodeTable->ParameterName);
						ColorTableNode->SetNoneOption(TypedNodeTable->bAddNoneOption);
						ColorTableNode->SetDefaultRowName(TypedNodeTable->DefaultRowName.ToString());
					}
				}
				else
				{
					FString Msg = FString::Printf(TEXT("Couldn't generate a mutable table."), *ColumnName);
					GenerationContext.Compiler->CompilerLog(FText::FromString(Msg), Node);
				}
			}
		}
		else
		{
			GenerationContext.Compiler->CompilerLog(LOCTEXT("ColorTableError", "Couldn't find the data table of the node."), Node);
		}
	}

	else
	{
		GenerationContext.Compiler->CompilerLog(LOCTEXT("UnimplementedNode", "Node type not implemented yet."), Node);
	}

	FGeneratedData CacheData = FGeneratedData(Node, Result);
	GenerationContext.Generated.Add(Key, CacheData);
	GenerationContext.GeneratedNodes.Add(Node);
	
	if (Result)
	{
		Result->SetMessageContext(Node);
	}

	return Result;
}

#undef LOCTEXT_NAMESPACE
