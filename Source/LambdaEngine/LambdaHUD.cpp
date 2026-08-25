#include "LambdaHUD.h"
#include "LambdaCharacter.h"
#include "LambdaWeapon.h"
#include "Weapons/SourceWeaponScript.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Materials/LambdaMaterialLibrary.h"
#include "FileSystem/LambdaFileSystem.h"
#include "LambdaEngine.h"
#include "LambdaConsole.h"
#include "LambdaMainMenu.h"
#include "Engine/GameInstance.h"
#include "Engine/FontFace.h"
#include "EngineFontServices.h"
#include "Fonts/FontMeasure.h"
#include "LambdaFonts.h"
#include "LambdaUITextures.h"

ALambdaHUD::ALambdaHUD()
{
	HudFont = GEngine ? GEngine->GetLargeFont() : nullptr;
}

void ALambdaHUD::LoadSchemeFont()
{
	// The scheme's face, shared with the loading screen and kept across map loads - see FLambdaFonts.
	SchemeFont = FLambdaFonts::GetSchemeFont();
}

void ALambdaHUD::DrawTextAtHeight(const FString& Text, const FLinearColor& Colour, float X, float Y, UFont* Font, float PixelHeight)
{
	if (!Canvas || !Font || Text.IsEmpty())
	{
		return;
	}
	const FSlateFontInfo Info(Font, FMath::Max(1, FMath::RoundToInt(PixelHeight)));
	FCanvasTextItem Item(FVector2D(X, Y), FText::FromString(Text), Info, Colour);
	Item.EnableShadow(FLinearColor::Transparent);
	Canvas->DrawItem(Item);
}

FVector2D ALambdaHUD::MeasureTextAtHeight(const FString& Text, UFont* Font, float PixelHeight) const
{
	if (!Font || Text.IsEmpty() || !FEngineFontServices::IsInitialized())
	{
		return FVector2D::ZeroVector;
	}
	const TSharedPtr<FSlateFontMeasure> Measure = FEngineFontServices::Get().GetFontMeasure();
	if (!Measure.IsValid())
	{
		return FVector2D::ZeroVector;
	}
	const FSlateFontInfo Info(Font, FMath::Max(1, FMath::RoundToInt(PixelHeight)));
	return FVector2D(Measure->Measure(Text, Info));
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

ULambdaMaterialLibrary* ALambdaHUD::GetMaterials()
{
	if (!Materials.IsValid())
	{
		if (ALambdaCharacter* Player = Cast<ALambdaCharacter>(GetOwningPawn()))
		{
			Materials = Player->GetWorldMaterialLibrary();
		}
	}
	return Materials.Get();
}

void ALambdaHUD::DrawHudIcon(const FString& TextureName, float X, float Y, float Size, const FLinearColor& Colour)
{
	ULambdaMaterialLibrary* Library = GetMaterials();
	if (!Library || !Canvas)
	{
		return;
	}
	if (UTexture2D* Icon = Library->GetTexture(TextureName))
	{
		// The icons are $translucent UnlitGeneric, drawn with $vertexcolor - the sheet is white and the colour
		// comes from the HUD, which is why one texture serves the normal, dim and warning states.
		Canvas->SetDrawColor(Colour.ToFColor(false));
		Canvas->DrawTile(Icon, X, Y, Size, Size, 0.0f, 0.0f, Icon->GetSizeX(), Icon->GetSizeY(),
			BLEND_Translucent);
		Canvas->SetDrawColor(FColor::White);
	}
}

float ALambdaHUD::DrawNumberField(float RightX, float Y, int32 Value, int32 Digits, float PixelHeight,
	const FLinearColor& Bright, const FLinearColor& Dim)
{
	UFont* Font = SchemeFont ? SchemeFont.Get() : HudFont.Get();
	if (!Font)
	{
		return RightX;
	}
	const FString Text = FString::Printf(TEXT("%0*d"), Digits, FMath::Clamp(Value, 0, 999));

	// The scale that puts the face at the height the scheme asks for.
	float Unused = 0.0f, FontHeight = 0.0f;
	GetTextSize(TEXT("0"), Unused, FontHeight, Font, 1.0f);
	const float Scale = FontHeight > 0.0f ? PixelHeight / FontHeight : 1.0f;

	// Where the number really starts: everything before it is a leading zero and is drawn faint.
	int32 FirstSignificant = 0;
	while (FirstSignificant < Text.Len() - 1 && Text[FirstSignificant] == TEXT('0'))
	{
		++FirstSignificant;
	}

	float TotalWidth = 0.0f, CharH = 0.0f;
	GetTextSize(Text, TotalWidth, CharH, Font, Scale);
	float X = RightX - TotalWidth;
	const float StartX = X;
	for (int32 i = 0; i < Text.Len(); ++i)
	{
		const FString Glyph = FString::Chr(Text[i]);
		float GlyphW = 0.0f, GlyphH = 0.0f;
		GetTextSize(Glyph, GlyphW, GlyphH, Font, Scale);
		DrawText(Glyph, i < FirstSignificant ? Dim : Bright, X, Y, Font, Scale);
		X += GlyphW;
	}
	return StartX;
}

/**
 * Black Mesa names its ammo icons for the round itself - ammo_9mm, ammo_357, ammo_buckshot - while the weapon
 * scripts name the ammo pool the HL2 way, "Pistol", "SMG1", "AR2". This is that translation; an ammo type with no
 * icon of its own falls back to the 9mm box.
 */
FString ALambdaHUD::AmmoIconName(const FString& AmmoType)
{
	static const TMap<FString, FString> Icons = {
		{ TEXT("pistol"),       TEXT("ammo_9mm") },
		{ TEXT("smg1"),         TEXT("ammo_9mm") },
		{ TEXT("357"),          TEXT("ammo_357") },
		{ TEXT("buckshot"),     TEXT("ammo_buckshot") },
		{ TEXT("ar2"),          TEXT("ammo_energy") },
		{ TEXT("xbowbolt"),     TEXT("ammo_bolt") },
		{ TEXT("rpg_round"),    TEXT("ammo_grenade_rpg") },
		{ TEXT("grenade"),      TEXT("ammo_grenade_frag") },
		{ TEXT("smg1_grenade"), TEXT("ammo_grenade_mp5") },
		{ TEXT("slam"),         TEXT("ammo_grenade_tripmine") },
	};
	const FString* Found = Icons.Find(AmmoType.ToLower());
	return FString::Printf(TEXT("vgui/hud/%s"), Found ? **Found : TEXT("ammo_9mm"));
}

UFont* ALambdaHUD::UIFont() const
{
	// SchemeFont is loaded from the game directory at runtime, so it is a vector face Slate rasterises at
	// whatever size is asked for. The engine's own is a baked bitmap: blow it up and it looks it.
	return SchemeFont ? SchemeFont.Get() : HudFont.Get();
}

void ALambdaHUD::DrawMainMenu(ULambdaMainMenu* Menu, float W, float H)
{
	UFont* MenuFont = UIFont();
	if (!Menu || !Menu->IsActive() || !MenuFont)
	{
		return;
	}
	if (Menu->IsPauseMenu())
	{
		// Over a running game the game stays visible, dimmed, the way pausing looks in Source.
		DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.6f), 0.0f, 0.0f, W, H);
	}
	else
	{
		// The menu's own background: console/background01, _widescreen for the wide one, which is what Source
		// calls it (engine/cl_main.cpp, CL_GetStartupImage). Stretched to fill, over black in case it is missing
		// or does not reach the corners.
		DrawRect(FLinearColor(0.02f, 0.02f, 0.02f, 1.0f), 0.0f, 0.0f, W, H);
		UTexture2D* Background = FLambdaUITextures::Get(TEXT("console/background01_widescreen"));
		if (!Background)
		{
			Background = FLambdaUITextures::Get(TEXT("console/background01"));
		}
		if (Background && Canvas)
		{
			Canvas->SetDrawColor(FColor::White);
			Canvas->DrawTile(Background, 0.0f, 0.0f, W, H, 0.0f, 0.0f,
				Background->GetSizeX(), Background->GetSizeY(), BLEND_Opaque);
		}
	}

	const float Scale = H / 480.0f;

	// The mod's own name, from gameinfo.txt's "game" key - what Source titles a mod with everywhere else.
	const FString GameName = FLambdaFileSystem::Get().GetGameName();
	const float TitleHeight = 44.0f * Scale;
	UFont* TitleFont = FLambdaFonts::GetTitleFont();
	DrawTextAtHeight(GameName.IsEmpty() ? TEXT("LAMBDA ENGINE") : GameName.ToUpper(),
		ULambdaMainMenu::TitleColour(), 32.0f * Scale, 48.0f * Scale,
		TitleFont ? TitleFont : MenuFont, TitleHeight);

	// The items, down the bottom left, the way the old menu reads.
	const float ItemHeight = 22.0f * Scale;
	const float TextHeight = ItemHeight * 0.8f;
	UFont* ItemFont = FLambdaFonts::GetMenuFont();
	if (!ItemFont)
	{
		ItemFont = MenuFont;
	}
	TArray<FLambdaMenuItem>& Items = Menu->GetMutableItems();
	float Y = H - 40.0f * Scale - Items.Num() * ItemHeight;

	for (int32 i = 0; i < Items.Num(); ++i)
	{
		const bool bSelected = (i == Menu->GetSelected());
		const float X = 32.0f * Scale;
		const FVector2D Size = MeasureTextAtHeight(Items[i].Label, ItemFont, TextHeight);

		// Remember where it went, so the mouse lands on what was drawn.
		Items[i].Bounds = FBox2D(FVector2D(X, Y), FVector2D(X + FMath::Max(Size.X, 120.0f * Scale), Y + Size.Y));

		if (bSelected)
		{
			// Source marks the one you are on with a bar down its left.
			DrawRect(ULambdaMainMenu::SelectedColour(), X - 10.0f * Scale, Y, 3.0f * Scale, Size.Y);
		}
		DrawTextAtHeight(Items[i].Label, bSelected ? ULambdaMainMenu::SelectedColour() : ULambdaMainMenu::ItemColour(),
			X, Y, ItemFont, TextHeight);
		Y += ItemHeight;
	}

	// Whatever the mouse is over is what is selected - but only once it has moved, or it would fight the arrow
	// keys for the selection every frame.
	if (APlayerController* PC = GetOwningPlayerController())
	{
		FVector2D Mouse;
		if (PC->GetMousePosition(Mouse.X, Mouse.Y))
		{
			if (!LastMenuMouse.Equals(Mouse, 0.5f))
			{
				LastMenuMouse = Mouse;
				Menu->SelectAt(Mouse);
			}
		}
	}
}

void ALambdaHUD::DrawConsole(ULambdaConsole* Console, float W, float H)
{
	UFont* MenuFont = UIFont();
	if (!Console || !Console->IsOpen() || !MenuFont)
	{
		return;
	}
	// It covers the top half of the screen, as Source's does, and everything under it carries on.
	const float PanelH = FMath::Min(H * 0.55f, 520.0f);
	DrawRect(ULambdaConsole::BackgroundColour(), 0.0f, 0.0f, W, PanelH);
	// A line under it to separate it from the game.
	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.9f), 0.0f, PanelH, W, 2.0f);

	float Unused = 0.0f, LineHeight = 0.0f;
	GetTextSize(TEXT("Wg"), Unused, LineHeight, MenuFont, 1.0f);
	LineHeight = FMath::Max(LineHeight, 12.0f);

	const float Margin = 8.0f;
	const float EntryY = PanelH - LineHeight - Margin;

	// The line being typed sits at the bottom of the panel behind a "]", with a caret that blinks.
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	const bool bCaret = FMath::Fmod(Now, 1.0f) < 0.5f;
	const FString Entry = FString::Printf(TEXT("] %s%s"), *Console->GetInput(), bCaret ? TEXT("_") : TEXT(""));
	DrawText(Entry, ULambdaConsole::EchoColour(), Margin, EntryY, MenuFont, 1.0f);

	// What it has said, newest at the bottom, oldest scrolling off the top.
	const TArray<FLambdaConsoleLine>& Lines = Console->GetLines();
	const int32 Last = Lines.Num() - 1 - Console->GetScrollBack();
	float Y = EntryY - LineHeight - 4.0f;
	for (int32 i = Last; i >= 0 && Y > Margin - LineHeight; --i)
	{
		if (Lines.IsValidIndex(i))
		{
			DrawText(Lines[i].Text, Lines[i].Color, Margin, Y, MenuFont, 1.0f);
		}
		Y -= LineHeight;
	}

	// Say so when the view is scrolled back, or it looks like the console has stopped saying anything.
	if (Console->GetScrollBack() > 0)
	{
		const FString Note = FString::Printf(TEXT("-- scrolled back %d lines --"), Console->GetScrollBack());
		float NoteW = 0.0f, NoteH = 0.0f;
		GetTextSize(Note, NoteW, NoteH, MenuFont, 1.0f);
		DrawText(Note, ULambdaConsole::WarningColour(), W - NoteW - Margin, EntryY, MenuFont, 1.0f);
	}
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

	LoadSchemeFont();

	if (!Canvas || !HudFont)
	{
		return;
	}

	const float W = Canvas->ClipX;
	const float H = Canvas->ClipY;

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

	// The console sits over everything, and is drawn whether or not there is a player behind it - it has to work
	// on the menu too.
	UGameInstance* Instance = GetGameInstance();
	ULambdaConsole* Console = Instance ? Instance->GetSubsystem<ULambdaConsole>() : nullptr;
	ULambdaMainMenu* Menu = Instance ? Instance->GetSubsystem<ULambdaMainMenu>() : nullptr;

	// Every frame, not only while the menu is up: this is what puts the input mode back after a level change,
	// when the controller is a new one and the movie player has had the viewport. It acts only on a change.
	if (Menu)
	{
		Menu->TickInputState();
	}

	// The HUD belongs to the game, so the game UI covers it rather than sharing the screen with it - Source hides
	// every hud element while it is up. The frame counter above is the engine's, and stays.
	if (Menu && Menu->IsActive())
	{
		DrawMainMenu(Menu, W, H);
		DrawConsole(Console, W, H);
		return;
	}

	DrawCrosshair(W * 0.5f, H * 0.5f);

	ALambdaCharacter* Player = Cast<ALambdaCharacter>(GetOwningPawn());
	if (!Player)
	{
		DrawConsole(Console, W, H);
		return;
	}
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;

	// Black Mesa's hudlayout.res is authored against a 480-tall screen, so everything below is in those units
	// scaled to the real one. "r36" in the file means 36 up from the bottom.
	const float S = H / 480.0f;
	const float DigitHeight = 31.0f * S;	// HudTextLarge: "Alte DIN 1451 Mittelschrift" tall 31

	// ---- Health and suit, bottom left ----
	{
		const int32 Health = FMath::RoundToInt(Player->GetHealth());
		const int32 Armour = FMath::RoundToInt(Player->GetArmor());

		// CHudHealth: dropping health starts the damage flash; at 20 and below the number pulses steadily, the
		// HudAnimations "HealthLow" loop.
		if (LastHealthSeen >= 0.0f && Health < LastHealthSeen)
		{
			DamageFlashEndTime = Now + 0.6f;
		}
		LastHealthSeen = Health;

		// "warnIfLessThan" "25": below that the field turns red, and the low-health pulse rides on top.
		const bool bWarn = Health < 25;
		FLinearColor HealthColour = bWarn ? LowColour : HudColour;
		FLinearColor HealthDim = bWarn ? LowColourDark : HudColourDark;
		if (Now < DamageFlashEndTime)
		{
			HealthColour = FMath::Lerp(FLinearColor::White, HealthColour, 1.0f - (DamageFlashEndTime - Now) / 0.6f);
		}
		else if (Health <= 20)
		{
			HealthColour.A = 0.55f + 0.45f * FMath::Abs(FMath::Sin(Now * 6.0f));
		}

		const float RowY = H - 36.0f * S;
		// The dot that opens the row, then three digits, then the health cross - CHudHealth's DotTexture and
		// CrossTexture.
		DrawHudIcon(TEXT("vgui/hud/hud_dot"), 16.0f * S, RowY + 12.0f * S, 10.0f * S, HealthColour);
		const float HealthRight = (16.0f + 74.0f) * S;
		DrawNumberField(HealthRight, RowY, Health, 3, DigitHeight, HealthColour, HealthDim);
		DrawHudIcon(TEXT("vgui/hud/hud_health_overlay"), HealthRight + 2.0f * S, RowY + 8.0f * S, 14.0f * S, HealthColour);

		// Suit, immediately to its right (CHudArmor xpos 90), with the HEV figure.
		const float ArmourRight = (90.0f + 74.0f) * S;
		DrawNumberField(ArmourRight, RowY, Armour, 3, DigitHeight, Armour > 0 ? HudColour : HudColourDim, HudColourDark);
		DrawHudIcon(TEXT("vgui/hud/hud_hev_overlay"), ArmourRight + 2.0f * S, RowY + 8.0f * S, 14.0f * S,
			Armour > 0 ? HudColour : HudColourDim);
	}

	DrawConsole(Console, W, H);

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
		const int32 Reserve = Player->GetAmmoCount(Weapon->GetPrimaryAmmoType());
		const float RowY = H - 44.0f * S;			// CHudClip ypos r44
		const float ClipRight = W - 73.0f * S;		// xpos r147 + wide 74, east aligned

		const FString AmmoIcon = AmmoIconName(Weapon->GetPrimaryAmmoType());

		if (Weapon->UsesClipsForAmmo1())
		{
			const int32 Clip = Weapon->GetClip1();
			FLinearColor ClipColour = (Clip <= 0) ? LowColour : HudColour;
			const FLinearColor ClipDim = (Clip <= 0) ? LowColourDark : HudColourDark;
			if (Now < WeaponFlashEndTime)
			{
				ClipColour = FLinearColor::White;
			}
			// dot, the clip in three digits, then the icon, then the reserve small beside it.
			DrawHudIcon(TEXT("vgui/hud/hud_dot"), W - 128.0f * S, RowY + 16.0f * S, 10.0f * S, ClipColour);
			DrawNumberField(ClipRight, RowY, Clip, 3, DigitHeight, ClipColour, ClipDim);
			DrawHudIcon(AmmoIcon, W - 62.0f * S, H - 36.0f * S, 15.0f * S, HudColourNormal);
			DrawNumberField(W - 16.0f * S, H - 34.0f * S, Reserve, 3, 15.0f * S, HudColourNormal, HudColourDark);
		}
		else
		{
			DrawHudIcon(TEXT("vgui/hud/hud_dot"), W - 128.0f * S, RowY + 16.0f * S, 10.0f * S, HudColour);
			DrawNumberField(ClipRight, RowY, Reserve, 3, DigitHeight, HudColour, HudColourDark);
			DrawHudIcon(AmmoIcon, W - 43.0f * S, H - 36.0f * S, 15.0f * S, HudColourNormal);
		}
	}
}
