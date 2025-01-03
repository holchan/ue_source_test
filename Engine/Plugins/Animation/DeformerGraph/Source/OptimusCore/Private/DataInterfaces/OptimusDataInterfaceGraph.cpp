// Copyright Epic Games, Inc. All Rights Reserved.

#include "OptimusDataInterfaceGraph.h"

#include "Components/SkinnedMeshComponent.h"
#include "ComputeFramework/ComputeMetadataBuilder.h"
#include "ComputeFramework/ShaderParameterMetadataAllocation.h"
#include "ComputeFramework/ShaderParamTypeDefinition.h"
#include "OptimusDeformerInstance.h"
#include "OptimusHelpers.h"
#include "OptimusVariableDescription.h"
#include "ShaderParameterMetadataBuilder.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(OptimusDataInterfaceGraph)

void UOptimusGraphDataInterface::Init(TArray<FOptimusGraphVariableDescription> const& InVariables)
{
	Variables = InVariables;

	FShaderParametersMetadataBuilder Builder;
	TArray<FShaderParametersMetadata*> AllocatedStructMetadatas;
	TArray<FShaderParametersMetadata*> NestedStructMetadatas; 
	for (FOptimusGraphVariableDescription const& Variable : Variables)
	{
		ComputeFramework::AddParamForType(Builder, *Variable.Name, Variable.ValueType, NestedStructMetadatas);
	}
	
	FShaderParametersMetadata* ShaderParameterMetadata = Builder.Build(FShaderParametersMetadata::EUseCase::ShaderParameterStruct, TEXT("UGraphDataInterface"));
	AllocatedStructMetadatas.Add(ShaderParameterMetadata);
	AllocatedStructMetadatas.Append(NestedStructMetadatas);
	

	TArray<FShaderParametersMetadata::FMember> const& Members = ShaderParameterMetadata->GetMembers();
	for (int32 VariableIndex = 0; VariableIndex < Variables.Num(); ++VariableIndex)
	{
		check(Variables[VariableIndex].Name == Members[VariableIndex].GetName());
		Variables[VariableIndex].Offset = Members[VariableIndex].GetOffset();
	}

	ParameterBufferSize = ShaderParameterMetadata->GetSize();

	for (const FShaderParametersMetadata* AllocatedData : AllocatedStructMetadatas)
	{
		delete AllocatedData;
	}
}

void UOptimusGraphDataInterface::GetSupportedInputs(TArray<FShaderFunctionDefinition>& OutFunctions) const
{
	OutFunctions.Reserve(OutFunctions.Num() + Variables.Num());
	for (FOptimusGraphVariableDescription const& Variable : Variables)
	{
		OutFunctions.AddDefaulted_GetRef()
			.SetName(FString::Printf(TEXT("Read%s"), *Variable.Name))
			.AddReturnType(Variable.ValueType);
	}
}

void UOptimusGraphDataInterface::GetShaderParameters(TCHAR const* UID, FShaderParametersMetadataBuilder& InOutBuilder, FShaderParametersMetadataAllocations& InOutAllocations) const
{
	// Build metadata nested structure containing all variables.
	FShaderParametersMetadataBuilder Builder;
	TArray<FShaderParametersMetadata*> NestedStructs;
	for (FOptimusGraphVariableDescription const& Variable : Variables)
	{
		ComputeFramework::AddParamForType(Builder, *Variable.Name, Variable.ValueType, NestedStructs);
	}

	FShaderParametersMetadata* ShaderParameterMetadata = Builder.Build(FShaderParametersMetadata::EUseCase::ShaderParameterStruct, TEXT("UGraphDataInterface"));
	// Add the metadata to InOutAllocations so that it is released when we are done.
	InOutAllocations.ShaderParameterMetadatas.Add(ShaderParameterMetadata);
	InOutAllocations.ShaderParameterMetadatas.Append(NestedStructs);

	// Add the generated nested struct to our builder.
	InOutBuilder.AddNestedStruct(UID, ShaderParameterMetadata);
}

void UOptimusGraphDataInterface::GetHLSL(FString& OutHLSL, FString const& InDataInterfaceName) const
{
	// Add uniforms.
	for (FOptimusGraphVariableDescription const& Variable : Variables)
	{
		OutHLSL += FString::Printf(TEXT("%s %s_%s;\n"), *Variable.ValueType->ToString(), *InDataInterfaceName, *Variable.Name);
	}
	// Add function getters.
	for (FOptimusGraphVariableDescription const& Variable : Variables)
	{
		OutHLSL += FString::Printf(TEXT("%s Read%s_%s()\n{\n\treturn %s_%s;\n}\n"), *Variable.ValueType->ToString(), *Variable.Name, *InDataInterfaceName, *InDataInterfaceName, *Variable.Name);
	}
}

UComputeDataProvider* UOptimusGraphDataInterface::CreateDataProvider(TObjectPtr<UObject> InBinding, uint64 InInputMask, uint64 InOutputMask) const
{
	UOptimusGraphDataProvider* Provider = NewObject<UOptimusGraphDataProvider>();
	Provider->MeshComponent = Cast<UMeshComponent>(InBinding);
	Provider->Variables = Variables;

	for (FOptimusGraphVariableDescription& Variable : Provider->Variables)
	{
		// When source object was introduced, we also appended a unique index to the value name provided by each value provider
		// so instead of using the name directly, we need to do this extra step
		if (!Variable.SourceObject.IsNull())
		{
			Variable.CachedSourceValueName = Optimus::ExtractSourceValueName(Variable.Name);
		}
	}
	
	Provider->ParameterBufferSize = ParameterBufferSize;
	return Provider;
}


void UOptimusGraphDataProvider::SetConstant(TSoftObjectPtr<UObject> InSourceObject, TArray<uint8> const& InValue)
{
	for (int32 VariableIndex = 0; VariableIndex < Variables.Num(); ++VariableIndex)
	{
		if (Variables[VariableIndex].SourceObject == InSourceObject)
		{
			if (ensure(Variables[VariableIndex].Value.Num() == InValue.Num()))
			{
				Variables[VariableIndex].Value = InValue;
				break;
			}
		}
	}
}

FComputeDataProviderRenderProxy* UOptimusGraphDataProvider::GetRenderProxy()
{
	return new FOptimusGraphDataProviderProxy(DeformerInstance, Variables, ParameterBufferSize);
}

void UOptimusGraphDataProvider::SetDeformerInstance(UOptimusDeformerInstance* InInstance)
{
	DeformerInstance = InInstance;
}

UOptimusDeformerInstance* UOptimusGraphDataProvider::GetDeformerInstance() const
{
	return DeformerInstance;
}

FOptimusGraphDataProviderProxy::FOptimusGraphDataProviderProxy(UOptimusDeformerInstance const* DeformerInstance, TArray<FOptimusGraphVariableDescription> const& Variables, int32 ParameterBufferSize)
{
	// Get all variables from deformer instance and fill buffer.
	ParameterData.AddZeroed(ParameterBufferSize);

	if (DeformerInstance == nullptr)
	{
		return;
	}

	TArray<UOptimusVariableDescription*> const& VariableValues = DeformerInstance->GetVariables();
	for (FOptimusGraphVariableDescription const& Variable : Variables)
	{
		if (Variable.Value.Num())
		{
			// Use the constant value.
			if (ensure(ParameterData.Num() >= Variable.Offset + Variable.Value.Num()))
			{
				FMemory::Memcpy(&ParameterData[Variable.Offset], Variable.Value.GetData(), Variable.Value.Num());
			}
		}
		else
		{
			// Find value from variables on the deformer instance.
			// todo[CF]: Use a map for more efficient look up? Or something even faster like having a fixed location per variable?
			for (UOptimusVariableDescription const* VariableValue : VariableValues)
			{
				if (VariableValue != nullptr)
				{
					if (Variable.ValueType != VariableValue->DataType->ShaderValueType)
					{
						continue;
					}

					bool bNameMatch = false;
					
					// Once upon a time when these values had no source objects, they also just have simple names
					// so we can directly use the name to find the matching variable
					if (Variable.SourceObject.IsNull())
					{
						// Using GetPlainNameString here because back then variables in the graph data interface were
						// also generated using GetPlainNameString. This certainly creates an issue where
						// multiple variables were sharing the same name, but at least the first matching variable
						// would still work
						if (Variable.Name == VariableValue->VariableName.GetPlainNameString())
						{
							bNameMatch = true;
						}	
					}
					else
					{
						// When source object was introduced, we also appended a unique index to the value name
						// so instead of using the name directly we use the source value name
						if (Variable.CachedSourceValueName == VariableValue->VariableName)
						{
							bNameMatch = true;
						}	
					}

					if (bNameMatch)
					{
						if (ensure(ParameterData.Num() >= Variable.Offset + VariableValue->ValueData.Num()))
						{
							FMemory::Memcpy(&ParameterData[Variable.Offset], VariableValue->ValueData.GetData(), VariableValue->ValueData.Num());
						}
						break;
					}
				}
			}
		}
	}
}

bool FOptimusGraphDataProviderProxy::IsValid(FValidationData const& InValidationData) const
{
	if (ParameterData.Num() == 0)
	{
		// todo[CF]: Why can we end up here? Remove this condition if possible.
		return false;
	}

	if (InValidationData.ParameterStructSize != ParameterData.Num())
	{
		return false;
	}

	return true;
}

void FOptimusGraphDataProviderProxy::GatherDispatchData(FDispatchData const& InDispatchData)
{
	for (int32 InvocationIndex = 0; InvocationIndex < InDispatchData.NumInvocations; ++InvocationIndex)
	{
		void* ParameterBuffer = (void*)(InDispatchData.ParameterBuffer + InDispatchData.ParameterBufferOffset + InDispatchData.ParameterBufferStride * InvocationIndex);
		FMemory::Memcpy(ParameterBuffer, ParameterData.GetData(), ParameterData.Num());
	}
}
