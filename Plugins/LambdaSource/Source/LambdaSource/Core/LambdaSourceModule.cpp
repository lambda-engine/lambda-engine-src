#include "Core/LambdaSourceModule.h"
#include "FileSystem/LambdaFileSystem.h"

DEFINE_LOG_CATEGORY(LogLambdaSource);

void FLambdaSourceModule::StartupModule()
{
	UE_LOG(LogLambdaSource, Log, TEXT("LambdaSource module started"));
}

void FLambdaSourceModule::ShutdownModule()
{
	UE_LOG(LogLambdaSource, Log, TEXT("LambdaSource module shut down"));
}

IMPLEMENT_MODULE(FLambdaSourceModule, LambdaSource)
