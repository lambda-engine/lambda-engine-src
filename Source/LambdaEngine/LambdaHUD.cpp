#include "LambdaHUD.h"
#include "LambdaCharacter.h"
#include "LambdaWeapon.h"
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

	ALambdaCharacter* Player = Cast<ALambdaCharacter>(GetOwningPawn());
	if (!Player)
	{
		return;
	}

	const float Margin = 24.0f;
	const float PanelH = 58.0f;
	const float Bottom = H - Margin - PanelH;

	// ---- Health / suit, bottom left ----
	{
		const float PanelW = 240.0f;
		DrawPanel(Margin, Bottom, PanelW, PanelH);

		const int32 Health = FMath::RoundToInt(Player->GetHealth());
		const int32 Armour = FMath::RoundToInt(Player->GetArmor());
		const FLinearColor HealthColour = (Health <= 25) ? LowColour : HudColour;

		DrawLabelledValue(Margin + 14.0f, Bottom + 8.0f, TEXT("HEALTH"), FString::FromInt(Health), HealthColour, 1.6f);
		DrawLabelledValue(Margin + 130.0f, Bottom + 8.0f, TEXT("SUIT"), FString::FromInt(Armour), HudColour, 1.6f);
	}

	// ---- Ammo, bottom right ----
	if (ALambdaWeapon* Weapon = Player->GetActiveWeapon())
	{
		const float PanelW = 210.0f;
		const float X = W - Margin - PanelW;
		DrawPanel(X, Bottom, PanelW, PanelH);

		const int32 Reserve = Player->GetAmmoCount(Weapon->GetPrimaryAmmoType());

		if (Weapon->UsesClipsForAmmo1())
		{
			const int32 Clip = Weapon->GetClip1();
			const FLinearColor ClipColour = (Clip <= 0) ? LowColour : HudColour;
			DrawLabelledValue(X + 14.0f, Bottom + 8.0f, TEXT("AMMO"), FString::FromInt(Clip), ClipColour, 1.6f);
			DrawLabelledValue(X + 118.0f, Bottom + 8.0f, TEXT("RESERVE"), FString::FromInt(Reserve), HudColour, 1.6f);
		}
		else
		{
			DrawLabelledValue(X + 14.0f, Bottom + 8.0f, TEXT("AMMO"), FString::FromInt(Reserve), HudColour, 1.6f);
		}
	}
}
