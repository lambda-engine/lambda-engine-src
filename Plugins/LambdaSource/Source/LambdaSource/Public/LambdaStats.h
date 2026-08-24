// Profiling counters for the Source runtime. `stat lambda` in the console breaks the frame down by the work
// this plugin does per tick - posing, skinning, and handing the skinned mesh to the renderer - which is
// otherwise invisible: our ticks carry no engine stat scope, so `stat game` reports the whole cost as an
// untagged lump inside World Tick Time.
#pragma once

#include "Stats/Stats.h"

DECLARE_STATS_GROUP(TEXT("Lambda"), STATGROUP_Lambda, STATCAT_Advanced);

DECLARE_CYCLE_STAT_EXTERN(TEXT("Studio tick"), STAT_LambdaStudioTick, STATGROUP_Lambda, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("Compose pose"), STAT_LambdaComposePose, STATGROUP_Lambda, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("Skin vertices"), STAT_LambdaApplyPose, STATGROUP_Lambda, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("Upload mesh sections"), STAT_LambdaMeshUpload, STATGROUP_Lambda, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("NPC think"), STAT_LambdaNPCThink, STATGROUP_Lambda, );
