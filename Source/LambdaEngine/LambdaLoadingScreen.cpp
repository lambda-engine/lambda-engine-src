#include "LambdaLoadingScreen.h"

#include "LambdaEngine.h"
#include "LambdaFonts.h"
#include "LambdaFileSystem.h"
#include "LambdaLoadProgress.h"
#include "LambdaUITextures.h"

#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "MoviePlayer.h"
#include "Styling/SlateBrush.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScaleBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	// SourceScheme.res, the same colours the menu and console are drawn in.
	const FLinearColor GTextColour(216 / 255.0f, 222 / 255.0f, 211 / 255.0f, 1.0f);		// BaseText
	const FLinearColor GBarFill(1.0f, 176 / 255.0f, 0.0f, 1.0f);						// the orange of the logo
	const FLinearColor GBarBack(1.0f, 1.0f, 1.0f, 0.12f);

	/** How wide the bar is and how tall, in the same 480-tall space the HUD and menu are laid out in. */
	constexpr float BarWidth = 340.0f;
	constexpr float BarHeight = 6.0f;
	constexpr float LogoSize = 168.0f;

	/** The logo brush, built once. Its texture is rooted, so the brush can hold it for the life of the process. */
	FSlateBrush* GetLogoBrush()
	{
		static FSlateBrush* Brush = nullptr;
		static bool bTried = false;
		if (!bTried)
		{
			bTried = true;
			// console/logo is what Source calls the game's mark on its startup screen
			// (engine/sys_getmodes.cpp, CVideoMode_Common::DrawStartupGraphic).
			if (UTexture2D* Texture = FLambdaUITextures::Get(TEXT("console/logo")))
			{
				Brush = new FSlateBrush();
				Brush->SetResourceObject(Texture);
				Brush->ImageSize = FVector2D(LogoSize, LogoSize);
				Brush->DrawAs = ESlateBrushDrawType::Image;
			}
		}
		return Brush;
	}

	/**
	 * Source's loading dialog: the game's picture, a bar, and a line saying what it is doing.
	 *
	 * Everything it shows is read from FLambdaLoadProgress every frame, because the thing filling that in is the
	 * game thread, which is busy loading the map while this is on screen.
	 */
	class SLambdaLoadingScreen : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SLambdaLoadingScreen) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			BackgroundBrush = FSlateColorBrush(FLinearColor::Black);
			BarBackBrush = FSlateColorBrush(GBarBack);
			BarFillBrush = FSlateColorBrush(GBarFill);

			// The same faces the menu is written in, so the two screens read as one thing.
			UFont* Status = FLambdaFonts::GetMenuFont();
			UFont* Title = FLambdaFonts::GetTitleFont();
			const FSlateFontInfo StatusFont = Status ? FSlateFontInfo(Status, 15) : FCoreStyle::GetDefaultFontStyle("Regular", 15);
			const FSlateFontInfo TitleFont = Title ? FSlateFontInfo(Title, 30) : FCoreStyle::GetDefaultFontStyle("Regular", 30);

			TSharedRef<SVerticalBox> Column = SNew(SVerticalBox);

			// The logo, where Source puts the game's loading picture.
			if (FSlateBrush* Logo = GetLogoBrush())
			{
				Column->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 28.0f)
				[
					SNew(SBox).WidthOverride(LogoSize).HeightOverride(LogoSize)
					[
						SNew(SImage).Image(Logo)
					]
				];
			}

			// The mod's name, from gameinfo.txt - the same string the main menu is titled with.
			const FString GameName = FLambdaFileSystem::Get().GetGameName();
			if (!GameName.IsEmpty())
			{
				Column->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 0.0f, 0.0f, 24.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(GameName.ToUpper()))
					.Font(TitleFont)
					.ColorAndOpacity(FSlateColor(FLinearColor::White))
				];
			}

			// The bar. Two rectangles: the trough, and a fill clipped to however far the load has got.
			Column->AddSlot().AutoHeight().HAlign(HAlign_Center)
			[
				SNew(SBox).WidthOverride(BarWidth).HeightOverride(BarHeight)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SImage).Image(&BarBackBrush)
					]
					+ SOverlay::Slot().HAlign(HAlign_Left)
					[
						SNew(SBox)
						.WidthOverride(this, &SLambdaLoadingScreen::GetFillWidth)
						.HeightOverride(BarHeight)
						[
							SNew(SImage).Image(&BarFillBrush)
						]
					]
				]
			];

			// What it is doing, under the bar.
			Column->AddSlot().AutoHeight().HAlign(HAlign_Center).Padding(0.0f, 14.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.Text(this, &SLambdaLoadingScreen::GetStatusText)
				.Font(StatusFont)
				.ColorAndOpacity(FSlateColor(GTextColour))
			];

			ChildSlot
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SNew(SImage).Image(&BackgroundBrush)
				]
				+ SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Center)
				[
					Column
				]
			];
		}

		virtual void Tick(const FGeometry& Geometry, const double CurrentTime, const float DeltaTime) override
		{
			SCompoundWidget::Tick(Geometry, CurrentTime, DeltaTime);
			Elapsed += DeltaTime;
		}

	private:
		FOptionalSize GetFillWidth() const
		{
			if (FLambdaLoadProgress::IsMeasured())
			{
				return FOptionalSize(BarWidth * FMath::Clamp(FLambdaLoadProgress::GetFraction(), 0.0f, 1.0f));
			}
			// Nothing has said how far along it is - loading the engine's own entry level, say - so the bar
			// sweeps instead of filling, which is all Source does when it has no number either.
			const float Sweep = 0.5f - 0.5f * FMath::Cos(Elapsed * 2.2f);
			return FOptionalSize(BarWidth * (0.06f + 0.34f * Sweep));
		}

		FText GetStatusText() const
		{
			const FString Status = FLambdaLoadProgress::GetStatus();
			const FString Map = FLambdaLoadProgress::GetMapName();
			if (Status.IsEmpty())
			{
				return FText::FromString(TEXT("Loading..."));
			}
			if (Map.IsEmpty())
			{
				return FText::FromString(Status + TEXT("..."));
			}
			return FText::FromString(FString::Printf(TEXT("%s: %s..."), *Map, *Status));
		}

		FSlateColorBrush BackgroundBrush = FSlateColorBrush(FLinearColor::Black);
		FSlateColorBrush BarBackBrush = FSlateColorBrush(FLinearColor::Black);
		FSlateColorBrush BarFillBrush = FSlateColorBrush(FLinearColor::Black);
		float Elapsed = 0.0f;
	};
}

void FLambdaLoadingScreen::Arm()
{
	// GIsEditor rules the movie player out, so this does nothing under the editor; the packaged game and -game
	// runs get it. Nothing else here is safe to do without Slate either.
	if (!IsMoviePlayerEnabled() || !FSlateApplication::IsInitialized())
	{
		return;
	}

	FLoadingScreenAttributes Attributes;
	Attributes.bAutoCompleteWhenLoadingCompletes = true;
	Attributes.bWaitForManualStop = false;
	Attributes.bAllowInEarlyStartup = true;
	// Long enough that a map which loads in a blink still reads as something rather than a flash of black.
	Attributes.MinimumLoadingScreenDisplayTime = 0.75f;
	Attributes.WidgetLoadingScreen = SNew(SLambdaLoadingScreen);

	GetMoviePlayer()->SetupLoadingScreen(Attributes);
}
