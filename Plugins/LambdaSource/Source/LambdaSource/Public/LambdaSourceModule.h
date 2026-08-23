#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

LAMBDASOURCE_API DECLARE_LOG_CATEGORY_EXTERN(LogLambdaSource, Log, All);

class FLambdaSourceModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
