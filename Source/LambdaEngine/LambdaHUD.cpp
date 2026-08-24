#include "LambdaHUD.h"
#include "LambdaCharacter.h"
#include "LambdaWeapon.h"
#include "SourceWeaponScript.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"

ALambdaHUD::ALambdaHUD()
{
	HudFont = GEngine ? GEngine->GetLargeFont() : nullptr;
}

void ALambdaHUD::DrawPanel(float X, float Y, float W, float H)
{
	DrawRect(PanelColour, X, Y, W, H);
}

void ALambdaHUD::DrawLabelledValue(float X, float Y, const FString& Label, const FString& Value, const FLinearColor& Colour, float ValueScale)
{
	if (!HudFont)
	{
		return;
	}
	// Small caption above a large number, the way the HL2 panels read.
	DrawText(Label, Colour * FLinearColor(1.0f, 1.0f, 1.0f, 0.75f), X, Y, HudFont, 1.0f);
	DrawText(Value, Colour, X, Y + 16.0f, HudFont, ValueScale);
}

void ALambdaHUD::DrawCrosshair(float CenterX, float CenterY)
{
	// A simple four-tick crosshair with a gap, like the pistol's.
	const float Gap = 6.0f;
	const float Len = 7.0f;
	const float Thick = 2.0f;
	const FLinearColor Colour = HudColour;

	DrawRect(Colour, CenterX - Gap - Len, CenterY - Thick * 0.5f, Len, Thick);	// left
	DrawRect(Colour, CenterX + Gap, CenterY - Thick * 0.5f, Len, Thick);		// right
	DrawRect(Colour, CenterX - Thick * 0.5f, CenterY - Gap - Len, Thick, Len);	// top
	DrawRect(Colour, CenterX - Thick * 0.5f, CenterY + Gap, Thick, Len);		// bottom
}

void ALambdaHUD::DrawDamageIndicator(ALambdaCharacter* Player, float W, float H, float Now)
{
	// CHudDamageIndicator: red slabs at the screen edge on the side the blow came from, fading out over a
	// second; big hits (over 25, DAMAGE_HIGH) read stronger.
	const float Age = Now - Player->GetLastDamageTime();
	if (Age < 0.0f || Age > 1.0f)
	{
		return;
	}
	const float Strength = (1.0f - Age) * (Player->GetLastDamageAmount() > 25.0f ? 0.7f : 0.4f);
	const FLinearColor Red(1.0f, 0.1f, 0.05f, Strength);
	const float Yaw = Player->GetLastDamageYaw();	// 0 ahead, +right, -left, +/-180 behind
	const float Thick = 42.0f;

	if (Yaw > 30.0f && Yaw < 150.0f)
	{
		DrawRect(Red, W - Thick, H * 0.2f, Thick, H * 0.6f);				// from the right
	}
	else if (Yaw < -30.0f && Yaw > -150.0f)
	{
		DrawRect(Red, 0.0f, H * 0.2f, Thick, H * 0.6f);						// from the left
	}
	else if (FMath::Abs(Yaw) >= 150.0f)
	{
		DrawRect(Red, W * 0.2f, H - Thick, W * 0.6f, Thick);				// from behind
	}
	else
	{
		DrawRect(Red, W * 0.2f, 0.0f, W * 0.6f, Thick);						// head on
	}
}

void ALambdaHUD::DrawWeaponSelection(ALambdaCharacter* Player, float W, float Now)
{
	// CHudWeaponSelection: a row of boxes along the top, one per carried weapon in bucket order, the selection
	// drawn large with its name; the attack confirms. Numbers are the bucket keys.
	if (!Player->IsWeaponSelectionActive() || !HudFont)
	{
		return;
	}
	const TArray<TObjectPtr<ALambdaWeapon>>& Arsenal = Player->GetWeapons();
	ALambdaWeapon* Selected = Player->GetSelectedWeapon();

	const float SmallW = 90.0f, LargeW = 160.0f, BoxH = 48.0f, Gap = 8.0f;
	float TotalW = 0.0f;
	for (const TObjectPtr<ALambdaWeapon>& Weapon : Arsenal)
	{
		TotalW += (Weapon == Selected ? LargeW : SmallW) + Gap;
	}
	float X = (W - TotalW) * 0.5f;
	const float Y = 36.0f;

	for (const TObjectPtr<ALambdaWeapon>& Weapon : Arsenal)
	{
		if (!Weapon)
		{
			continue;
		}
		const bool bSelected = Weapon == Selected;
		const float BoxW = bSelected ? LargeW : SmallW;
		DrawRect(bSelected ? FLinearColor(HudColour.R, HudColour.G, HudColour.B, 0.35f) : PanelColour, X, Y, BoxW, BoxH);

		// "#HL2_Pistol" reads as PISTOL; the bucket number sits in the corner like HL2's.
		FString Name = Weapon->GetWeaponInfo().PrintName;
		int32 Underscore;
		if (Name.FindLastChar(TEXT('_'), Underscore))
		{
			Name = Name.Mid(Underscore + 1);
		}
		DrawText(FString::FromInt(Weapon->GetWeaponInfo().Bucket + 1),
			HudColour * FLinearColor(1, 1, 1, 0.6f), X + 5.0f, Y + 3.0f, HudFont, 0.9f);
		DrawText(Name.ToUpper(), bSelected ? HudColour : HudColour * FLinearColor(1, 1, 1, 0.7f),
			X + 18.0f, Y + (bSelected ? 16.0f : 18.0f), HudFont, bSelected ? 1.3f : 1.0f);
		X += BoxW + Gap;
	}
}

void ALambdaHUD::DrawPickupHistory(ALambdaCharacter* Player, float W, float H, float Now)
{
	// CHudHistoryResource: the last few pickups fade out on the right, newest at the bottom.
	if (!HudFont)
	{
		return;
	}
	float Y = H * 0.35f;
	for (const ALambdaCharacter::FPickupEvent& Event : Player->GetPickupHistory())
	{
		const float Age = Now - Event.Time;
		if (Age < 0.0f || Age > 3.0f)
		{
			continue;
		}
		const float Alpha = Age > 2.0f ? 1.0f - (Age - 2.0f) : 1.0f;
		float TextW = 0.0f, TextH = 0.0f;
		GetTextSize(Event.Text, TextW, TextH, HudFont, 1.0f);
		DrawText(Event.Text, HudColour * FLinearColor(1, 1, 1, Alpha), W - TextW - 16.0f, Y, HudFont, 1.0f);
		Y += 20.0f;
	}
}

void ALambdaHUD::DrawHUD()
{
	Super::DrawHUD();

	if (!Canvas || !HudFont)
	{
		return;
	}

	const float W = Canvas->ClipX;
	const float H = Canvas->ClipY;

	DrawCrosshair(W * 0.5f, H * 0.5f);

	// cl_showfps, top right: the frame time is smoothed over ~half a second so the number reads steadily.
	const float FrameTime = GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.0f;
	SmoothedFrameTime = SmoothedFrameTime <= 0.0f ? FrameTime : FMath::Lerp(SmoothedFrameTime, FrameTime, 0.05f);
	if (SmoothedFrameTime > 0.0f)
	{
		const int32 Fps = FMath::RoundToInt(1.0f / SmoothedFrameTime);
		// Green at 60+, amber in the middle, red when it is bad, as cl_showfps colours it.
		const FLinearColor FpsColour = Fps >= 60 ? FLinearColor(0.2f, 1.0f, 0.2f, 1.0f)
			: Fps >= 30 ? HudColour : LowColour;
		const FString FpsText = FString::Printf(TEXT("%d fps"), Fps);
		float TextW = 0.0f, TextH = 0.0f;
		GetTextSize(FpsText, TextW, TextH, HudFont, 1.0f);
		DrawText(FpsText, FpsColour, W - TextW - 12.0f, 8.0f, HudFont, 1.0f);
	}

	ALambdaCharacter* Player = Cast<ALambdaCharacter>(GetOwningPawn());
	if (!Player)
	{
		return;
	}
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

	const float Margin = 24.0f;
	const float PanelH = 58.0f;
	const float Bottom = H - Margin - PanelH;

	// ---- Health / suit, bottom left ----
	{
		const float PanelW = 240.0f;
		DrawPanel(Margin, Bottom, PanelW, PanelH);

		const int32 Health = FMath::RoundToInt(Player->GetHealth());
		const int32 Armour = FMath::RoundToInt(Player->GetArmor());

		// CHudHealth: dropping health starts the damage flash; at 20 and below the number pulses steadily, the
		// HudAnimations "HealthLow" loop.
		if (LastHealthSeen >= 0.0f && Health < LastHealthSeen)
		{
			DamageFlashEndTime = Now + 0.6f;
		}
		LastHealthSeen = Health;

		FLinearColor HealthColour = (Health <= 20) ? LowColour : HudColour;
		if (Now < DamageFlashEndTime)
		{
			HealthColour = FMath::Lerp(FLinearColor::White, HealthColour, 1.0f - (DamageFlashEndTime - Now) / 0.6f);
		}
		else if (Health <= 20)
		{
			HealthColour.A = 0.55f + 0.45f * FMath::Abs(FMath::Sin(Now * 6.0f));
		}

		DrawLabelledValue(Margin + 14.0f, Bottom + 8.0f, TEXT("HEALTH"), FString::FromInt(Health), HealthColour, 1.6f);
		DrawLabelledValue(Margin + 130.0f, Bottom + 8.0f, TEXT("SUIT"), FString::FromInt(Armour), HudColour, 1.6f);
	}

	DrawDamageIndicator(Player, W, H, Now);
	DrawWeaponSelection(Player, W, Now);
	DrawPickupHistory(Player, W, H, Now);

	// ---- Ammo, bottom right ----
	// CHudAmmo hides itself for a weapon that uses no ammo at all - the crowbar owns no panel.
	ALambdaWeapon* ActiveWeapon = Player->GetActiveWeapon();
	if (ActiveWeapon != LastWeaponSeen.Get())
	{
		LastWeaponSeen = ActiveWeapon;
		WeaponFlashEndTime = Now + 0.4f;	// the "WeaponChanged" brighten
	}
	if (ALambdaWeapon* Weapon = ActiveWeapon)
	{
		if (Weapon->GetPrimaryAmmoType().Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			return;
		}
		const float PanelW = 210.0f;
		const float X = W - Margin - PanelW;
		DrawPanel(X, Bottom, PanelW, PanelH);

		const int32 Reserve = Player->GetAmmoCount(Weapon->GetPrimaryAmmoType());

		if (Weapon->UsesClipsForAmmo1())
		{
			const int32 Clip = Weapon->GetClip1();
			FLinearColor ClipColour = (Clip <= 0) ? LowColour : HudColour;
			if (Now < WeaponFlashEndTime)
			{
				ClipColour = FLinearColor::White;
			}
			DrawLabelledValue(X + 14.0f, Bottom + 8.0f, TEXT("AMMO"), FString::FromInt(Clip), ClipColour, 1.6f);
			DrawLabelledValue(X + 118.0f, Bottom + 8.0f, TEXT("RESERVE"), FString::FromInt(Reserve), HudColour, 1.6f);
		}
		else
		{
			DrawLabelledValue(X + 14.0f, Bottom + 8.0f, TEXT("AMMO"), FString::FromInt(Reserve), HudColour, 1.6f);
		}
	}
}
