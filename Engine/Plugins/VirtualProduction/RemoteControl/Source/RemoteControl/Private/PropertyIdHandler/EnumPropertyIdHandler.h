// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "IPropertyIdHandler.h"

class FEnumPropertyIdHandler : public IPropertyIdHandler
{
public:
	//~ Begin IPropertyIdHandler
	virtual bool IsPropertySupported(const FProperty* InProperty) const override;
	virtual EPropertyBagPropertyType GetPropertyType(const FProperty* InProperty) const override;
	virtual FName GetPropertySuperTypeName(const FProperty* InProperty) const override;
	virtual FName GetPropertySubTypeName(const FProperty* InProperty) const override;
	virtual UObject* GetPropertyTypeObject(const FProperty* InProperty) const override;
	//~ End IPropertyIdHandler
};