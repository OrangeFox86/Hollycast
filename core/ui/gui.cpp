/*
	Copyright 2019 flyinghead
	Portions Copyright 2026 The Hollycast Authors

	This file is part of Hollycast.

    Hollycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Hollycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Hollycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "gui.h"
#include "rend/osd.h"
#include "cfg/cfg.h"
#include "imgui.h"
#include "imgui_stdlib.h"
#include "network/net_handshake.h"
#include "network/ice.h"
#include "input/gamepad_device.h"
#include "gui_util.h"
#include "gui_font.h"
#include "imgread/common.h"
#include "emulator.h"
#include "mainui.h"
#include "lua/lua.h"
#include "gui_chat.h"
#include "imgui_driver.h"
#if FC_PROFILER
#include "implot.h"
#endif
#include "boxart/boxart.h"
#include "boxart/vmu_icon.h"
#include "profiler/fc_profiler.h"
#include "hw/naomi/card_reader.h"
#include "oslib/resources.h"
#include "achievements/achievements.h"
#include "gui_achievements.h"
#include "IconsFontAwesome6.h"
#include <stb_image_write.h>
#include <stb_image.h>
#include "hw/pvr/Renderer_if.h"
#include "rend/CustomTexture.h"
#include "hw/mem/addrspace.h"
#include "hw/maple/maple_if.h"
#if defined(USE_SDL)
#include "sdl/sdl.h"
#endif
#include "vgamepad.h"
#include "settings.h"
#include "settings_new.h"
#include "oslib/i18n.h"
#include "gui_font.h"
using namespace i18n;
#include "gui_menu.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <mutex>
#include <algorithm>
#include <array>
#include <cctype>

bool game_started;

int insetLeft, insetRight, insetTop, insetBottom;
std::unique_ptr<ImGuiDriver> imguiDriver;

static bool inited = false;
GuiState gui_state = GuiState::Main;
static bool commandLineStart;
std::string launchOnExitUri;
static u32 mouseButtons;
static int mouseX, mouseY;
static float mouseWheel;
static bool mouseTouchscreen;
static std::string error_msg;
static bool error_msg_shown;
static std::string osd_message;
static u64 osd_message_end;
static std::mutex osd_message_mutex;
static void (*showOnScreenKeyboard)(bool show);
static bool keysUpNextFrame[512];
bool uiUserScaleUpdated;
static bool clearActiveIdNextFrame;
bool showExitSaveDialog = false;
static bool showAutoLoadDialog = false;
enum class ExitSaveDialogAction { ReturnToLibrary, ExitEmulator };
static ExitSaveDialogAction exitSaveDialogAction = ExitSaveDialogAction::ReturnToLibrary;

GameScanner scanner;
static BackgroundGameLoader gameLoader;
static Boxart boxart;
static Chat chat;
static std::recursive_mutex guiMutex;
using LockGuard = std::lock_guard<std::recursive_mutex>;

static Toast toast;
static ScheduledThreadRunner<std::chrono::steady_clock::time_point> uiThreadRunner;

static constexpr float GAME_INFO_LONG_PRESS_SECONDS = 3.f;
static constexpr double LIBRARY_GAME_INFO_HOVER_SECONDS = 1.0;
static bool resetLibraryGameInfoHoverState;
struct LibraryLongPressState
{
	std::string gameId;
	GameMedia game;
	float startTime = 0.f;
	bool tracking = false;
	bool opened = false;
};
static LibraryLongPressState libraryLongPress;
static GameMedia selectedGameForInfo;
static bool touchedLibraryItemThisFrame = false;

static void emuEventCallback(Event event, void *)
{
	switch (event)
	{
	case Event::Resume:
		game_started = true;
		vgamepad::startGame();
		break;
	case Event::Start:
		markLibraryGameBooted(settings.content.gameId, settings.content.path);
		GamepadDevice::load_system_mappings();
		break;
	case Event::Terminate:
		GamepadDevice::load_system_mappings();
		game_started = false;
		break;
	default:
		break;
	}
}

static void clearThumbnailCache();

void gui_init()
{
	DEBUG_LOG(COMMON, "gui_init() called");
	if (inited)
		return;
	inited = true;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
#if FC_PROFILER
	ImPlot::CreateContext();
#endif
	ImGuiIO& io = ImGui::GetIO();
	io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

	io.IniFilename = nullptr;

	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;   // Enable Gamepad Controls

    EventManager::listen(Event::Resume, emuEventCallback);
    EventManager::listen(Event::Start, emuEventCallback);
	EventManager::listen(Event::Terminate, emuEventCallback);
    ggpo::receiveChatMessages([](int playerNum, const std::string& msg) { chat.receive(playerNum, msg); });

#ifdef TARGET_UWP
	{
		// Detect when the on-screen keyboard is hidden and clear the text input widget id to validate the edit.
		// Otherwise the user will cancel the edit if he presses B, and must press A in the case of multi-line inputs.
		using namespace Windows::UI::ViewManagement;
		InputPane^ inputPane = InputPane::GetForCurrentView();
		if (inputPane)
		{
			inputPane->Hiding += ref new Windows::Foundation::TypedEventHandler<InputPane^, InputPaneVisibilityEventArgs^>(
				[](InputPane^, InputPaneVisibilityEventArgs^)
				{
					clearActiveIdNextFrame = true;
				});
		}
	}
#endif
}

static ImGuiKey keycodeToImGuiKey(u8 keycode)
{
	switch (keycode)
	{
		case 0x2B: return ImGuiKey_Tab;
		case 0x50: return ImGuiKey_LeftArrow;
		case 0x4F: return ImGuiKey_RightArrow;
		case 0x52: return ImGuiKey_UpArrow;
		case 0x51: return ImGuiKey_DownArrow;
		case 0x4B: return ImGuiKey_PageUp;
		case 0x4E: return ImGuiKey_PageDown;
		case 0x4A: return ImGuiKey_Home;
		case 0x4D: return ImGuiKey_End;
		case 0x49: return ImGuiKey_Insert;
		case 0x4C: return ImGuiKey_Delete;
		case 0x2A: return ImGuiKey_Backspace;
		case 0x2C: return ImGuiKey_Space;
		case 0x28: return ImGuiKey_Enter;
		case 0x29: return ImGuiKey_Escape;
		case 0x04: return ImGuiKey_A;
		case 0x06: return ImGuiKey_C;
		case 0x19: return ImGuiKey_V;
		case 0x1B: return ImGuiKey_X;
		case 0x1C: return ImGuiKey_Y;
		case 0x1D: return ImGuiKey_Z;
		case 0xE0:
		case 0xE4:
			return ImGuiMod_Ctrl;
		case 0xE1:
		case 0xE5:
			return ImGuiMod_Shift;
		case 0xE3:
		case 0xE7:
			return ImGuiMod_Super;
		default: return ImGuiKey_None;
	}
}

void gui_updateStyle()
{
	static float uiScale;

	verify(inited);
	uiThreadRunner.init();

#if !defined(TARGET_UWP) && !defined(__SWITCH__)
	const float dpiScale = std::max(1.f, settings.display.dpi / 100.f * 0.75f);
#if defined(__APPLE__) && !defined(TARGET_IPHONE)
	if (settings.display.pointScale > 1.f)
		// Match macOS point scaling for HiDPI modes.
		settings.display.uiScale = std::max(settings.display.pointScale, dpiScale);
	else
		// Dense 1x modes get only a small physical-DPI boost.
		settings.display.uiScale = 1.f + (dpiScale - 1.f) * 0.15f;
#else
	settings.display.uiScale = dpiScale;
#endif
   	// Limit scaling on small low-res screens
    if (settings.display.width <= 640 || settings.display.height <= 480)
    	settings.display.uiScale = std::min(1.2f, settings.display.uiScale);
#endif
    settings.display.uiScale *= config::UIScaling / 100.f;
	if (settings.display.uiScale == uiScale && ImGui::GetIO().Fonts->IsBuilt())
		return;
	uiScale = settings.display.uiScale;

	// Setup Dear ImGui style
	ImGui::GetStyle() = ImGuiStyle{};

	// Apply the current theme
	applyCurrentTheme();

	ImGui::GetStyle().TabRounding = 5.0f;
	ImGui::GetStyle().FrameRounding = 3.0f;
	ImGui::GetStyle().ItemSpacing = ImVec2(8, 8);		// from 8,4
	ImGui::GetStyle().ItemInnerSpacing = ImVec2(4, 6);	// from 4,4
#if defined(__ANDROID__) || defined(TARGET_IPHONE) || defined(__SWITCH__)
	ImGui::GetStyle().TouchExtraPadding = ImVec2(1, 1);	// from 0,0
#endif
	if (settings.display.uiScale != 1.f)
		ImGui::GetStyle().ScaleAllSizes(settings.display.uiScale);

	gui_loadFonts();

	NOTICE_LOG(RENDERER, "Screen DPI is %.0f, size %d x %d. Scaling by %.2f", settings.display.dpi, settings.display.width, settings.display.height, settings.display.uiScale);
	vgamepad::applyUiScale();
}

void gui_keyboard_input(u32 wc)
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.WantCaptureKeyboard)
		io.AddInputCharacter(wc);
}

void gui_keyboard_inputUTF8(const std::string& s)
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.WantCaptureKeyboard)
		io.AddInputCharactersUTF8(s.c_str());
}

void gui_keyboard_key(u8 keyCode, bool pressed)
{
	if (!inited)
		return;
	ImGuiKey key = keycodeToImGuiKey(keyCode);
	if (key == ImGuiKey_None)
		return;
	if (!pressed && ImGui::IsKeyDown(key))
	{
		keysUpNextFrame[keyCode] = true;
		return;
	}
	ImGuiIO& io = ImGui::GetIO();
	io.AddKeyEvent(key, pressed);
}

bool gui_keyboard_captured() {
	ImGuiIO& io = ImGui::GetIO();
	return io.WantCaptureKeyboard;
}

bool gui_mouse_captured() {
	ImGuiIO& io = ImGui::GetIO();
	return io.WantCaptureMouse;
}

void gui_set_mouse_position(int x, int y, bool touchscreen)
{
	mouseX = std::round(x * settings.display.pointScale);
	mouseY = std::round(y * settings.display.pointScale);
	mouseTouchscreen = touchscreen;
}

bool gui_is_menu_touch_target(float x, float y)
{
	return GuiMenu::isTouchTarget(std::round(x * settings.display.pointScale),
			std::round(y * settings.display.pointScale));
}

void gui_set_mouse_button(int button, bool pressed, bool touchscreen)
{
	if (pressed)
		mouseButtons |= 1 << button;
	else
		mouseButtons &= ~(1 << button);
	mouseTouchscreen = touchscreen;
}

void gui_set_mouse_wheel(float delta) {
	mouseWheel += delta;
	mouseTouchscreen = false;
}

static void gui_newFrame()
{
	imguiDriver->newFrame();
	ImGui::GetIO().DisplaySize.x = settings.display.width;
	ImGui::GetIO().DisplaySize.y = settings.display.height;

	ImGuiIO& io = ImGui::GetIO();

	io.AddMouseSourceEvent(mouseTouchscreen ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
	if (mouseX < 0 || mouseX >= settings.display.width || mouseY < 0 || mouseY >= settings.display.height)
		io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
	else
		io.AddMousePosEvent(mouseX, mouseY);
	if (io.WantCaptureMouse)
	{
		io.AddMouseWheelEvent(0, -mouseWheel / 16);
		mouseWheel = 0;
	}
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, (mouseButtons & (1 << 0)) != 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Right, (mouseButtons & (1 << 1)) != 0);
	io.AddMouseButtonEvent(ImGuiMouseButton_Middle, (mouseButtons & (1 << 2)) != 0);
	io.AddMouseButtonEvent(3, (mouseButtons & (1 << 3)) != 0);

	// shows a popup navigation window even in game because of the OSD
	//io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, ((kcode[0] & DC_BTN_X) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadFaceRight, ((kcode[0] & DC_BTN_B) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadFaceUp, ((kcode[0] & DC_BTN_Y) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadFaceDown, ((kcode[0] & DC_BTN_A) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, ((kcode[0] & DC_DPAD_LEFT) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadDpadRight, ((kcode[0] & DC_DPAD_RIGHT) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadDpadUp, ((kcode[0] & DC_DPAD_UP) == 0));
	io.AddKeyEvent(ImGuiKey_GamepadDpadDown, ((kcode[0] & DC_DPAD_DOWN) == 0));

	float analog;
	analog = joyx[0] < 0 ? -(float)joyx[0] / 32768.f : 0.f;
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, analog > 0.1f, analog);
	analog = joyx[0] > 0 ? (float)joyx[0] / 32768.f : 0.f;
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, analog > 0.1f, analog);
	analog = joyy[0] < 0 ? -(float)joyy[0] / 32768.f : 0.f;
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, analog > 0.1f, analog);
	analog = joyy[0] > 0 ? (float)joyy[0] / 32768.f : 0.f;
	io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, analog > 0.1f, analog);

	// Emergency quit: Shift+Esc or Ctrl+Shift+Esc to force quit (bypasses save dialog)
	// This is a safety measure in case of UI bugs
	if (ImGui::IsKeyPressed(ImGuiKey_Escape) &&
	    (ImGui::GetIO().KeyShift || ImGui::GetIO().KeySuper))
	{
		INFO_LOG(COMMON, "Emergency quit activated");
		showExitSaveDialog = false;  // Clear any pending dialog
		gui_stop_game();  // Stop the game immediately
		dc_exit();  // Exit application
		return;  // Skip the rest of this frame
	}

	ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);

	if (showOnScreenKeyboard != nullptr)
		showOnScreenKeyboard(io.WantTextInput);
	if (clearActiveIdNextFrame && io.WantTextInput)
	{
		ImGui::ClearActiveID();
		clearActiveIdNextFrame = false;
	}
}

// SDL on-screen keyboard: Delay keys up by one frame to allow quick key presses.
static void delayedKeysUp()
{
	ImGuiIO& io = ImGui::GetIO();
	for (u32 i = 0; i < std::size(keysUpNextFrame); i++)
		if (keysUpNextFrame[i])
			io.AddKeyEvent(keycodeToImGuiKey(i), false);
	memset(keysUpNextFrame, 0, sizeof(keysUpNextFrame));
}

static void gui_endFrame(bool gui_open) {
    imguiDriver->renderDrawData(ImGui::GetDrawData(), gui_open);
    delayedKeysUp();
}

void gui_setOnScreenKeyboardCallback(void (*callback)(bool show)) {
	showOnScreenKeyboard = callback;
}

void gui_set_insets(int left, int right, int top, int bottom)
{
	insetLeft = left;
	insetRight = right;
	insetTop = top;
	insetBottom = bottom;
}

#if 0
#include "oslib/timeseries.h"
#include <vector>
TimeSeries renderTimes;
TimeSeries vblankTimes;

void gui_plot_render_time(int width, int height)
{
	std::vector<float> v = renderTimes.data();
	ImGui::PlotLines(T("Render Times"), v.data(), v.size(), 0, "", 0.0, 1.0 / 30.0, ImVec2(300, 50));
	ImGui::Text(T("StdDev: %.1f%%"), renderTimes.stddev() * 100.f / 0.01666666667f);
	v = vblankTimes.data();
	ImGui::PlotLines(T("VBlank"), v.data(), v.size(), 0, "", 0.0, 1.0 / 30.0, ImVec2(300, 50));
	ImGui::Text(T("StdDev: %.1f%%"), vblankTimes.stddev() * 100.f / 0.01666666667f);
}
#endif

static bool gui_resume_game()
{
	if (custom_texture.needsRefresh())
	{
		gui_setState(GuiState::Loading);
		return false;
	}
	gui_setState(GuiState::Closed);
	return true;
}

void gui_open_settings()
{
	const LockGuard lock(guiMutex);
	if (gui_state == GuiState::Closed && !settings.naomi.slave)
	{
		if (!ggpo::active())
		{
			if (achievements::canPause())
			{
				vgamepad::hide();
				try {
					emu.stop();
					gui_setState(GuiState::Commands);
				} catch (const FlycastException& e) {
					gui_stop_game(e.what());
				}
			}
		}
		else
		{
			chat.toggle();
		}
	}
	else if (gui_state == GuiState::VJoyEdit)
	{
		vgamepad::pauseEditing();
		// iOS: force a touch up event to make up for the one eaten by the tap gesture recognizer
		mouseButtons &= ~1;
		gui_setState(GuiState::VJoyEditCommands);
	}
	else if (gui_state == GuiState::Loading)
	{
		gameLoader.cancel();
	}
	else if (gui_state == GuiState::Commands)
	{
		const bool resumeNow = gui_resume_game();
		GamepadDevice::load_system_mappings();
		if (resumeNow)
			emu.start();
	}
	else if (gui_state == GuiState::Pause)
	{
		gui_setState(GuiState::Commands);
	}
	else if (gui_state == GuiState::GameInfo)
	{
		gui_setState(GuiState::Main);
	}
}

void gui_start_game(const std::string& path)
{
	const LockGuard lock(guiMutex);
	if (gui_state != GuiState::Main && gui_state != GuiState::Closed && gui_state != GuiState::Commands
			&& gui_state != GuiState::Pause)
		return;
	showExitSaveDialog = false;
	showAutoLoadDialog = false;
	emu.unloadGame();
	reset_vmus();
    chat.reset();

	// Clear thumbnail cache when starting a new game
	clearThumbnailCache();

	scanner.stop();
	gui_setState(GuiState::Loading);
	gameLoader.load(path);
}

void gui_stop_game(const std::string& message, bool allowAutoSave)
{
	const LockGuard lock(guiMutex);
	showExitSaveDialog = false;
	showAutoLoadDialog = false;
	if (!commandLineStart)
	{
		// Exit to main menu
		emu.unloadGame(allowAutoSave);
		gui_setState(GuiState::Main);
		reset_vmus();

		// Clear thumbnail cache when stopping game
		clearThumbnailCache();

		if (!message.empty())
			gui_error(Ts("Hollycast has stopped.") + "\n\n" + message);
	}
	else
	{
		if (!message.empty())
			ERROR_LOG(COMMON, "Flycast has stopped: %s", message.c_str());
		emu.unloadGame(allowAutoSave);
		// Exit emulator
		dc_exit();
	}
}

static void savestate();
static void savestate(int slot);

static void save_auto_state_before_exit()
{
	if (!dc_savestateAllowed())
		return;

	try {
		if (gui_state == GuiState::Closed)
			emu.stop();
		// Save here before exit so the chosen "Save and Exit" path uses the
		// hidden auto-save slot, then suppress unloadGame() from writing a second
		// auto-save for the same shutdown.
		savestate(dc_getAutoSaveSlot());
		dc_skipAutoSaveOnNextUnload();
	} catch (const FlycastException& e) {
		WARN_LOG(COMMON, "save_auto_state_before_exit: %s", e.what());
	}
}

static void finish_exit_request(bool exitEmulator, bool saveFirst)
{
	if (saveFirst)
		save_auto_state_before_exit();

	if (exitEmulator && !commandLineStart)
	{
		emu.unloadGame(false);
		mainui_stop();
		return;
	}

	gui_stop_game("", false);
}

void gui_request_exit_to_library()
{
	if (dc_savestateAllowed() && config::AutoSaveState)
	{
		finish_exit_request(false, true);
		return;
	}

	if (dc_savestateAllowed() && config::SaveProtection)
	{
		exitSaveDialogAction = ExitSaveDialogAction::ReturnToLibrary;
		showExitSaveDialog = true;
		return;
	}
	gui_stop_game("", false);
}

void gui_request_exit_emulator()
{
	if (!game_started)
	{
		mainui_stop();
		return;
	}

	if (dc_savestateAllowed() && config::AutoSaveState)
	{
		finish_exit_request(true, true);
		return;
	}

	if (!dc_savestateAllowed() || !config::SaveProtection)
	{
		finish_exit_request(true, false);
		return;
	}

	exitSaveDialogAction = ExitSaveDialogAction::ExitEmulator;
	showExitSaveDialog = true;
}

void gui_request_initial_auto_load()
{
	const int autoSaveSlot = dc_getAutoSaveSlot();
	if (!dc_savestateAllowed() || settings.raHardcoreMode
			|| dc_getStateCreationDate(autoSaveSlot) == 0)
	{
		gui_setState(GuiState::Closed);
		return;
	}

	if (config::AutoLoadState)
	{
		dc_loadstate(autoSaveSlot);
		gui_setState(GuiState::Closed);
		return;
	}

	showAutoLoadDialog = true;
	gui_setState(GuiState::Pause);
}

static void appendVectorData(void *context, void *data, int size)
{
	std::vector<u8>& v = *(std::vector<u8> *)context;
	const u8 *bytes = (const u8 *)data;
	v.insert(v.end(), bytes, bytes + size);
}

static void getScreenshot(std::vector<u8>& data, int width = 0)
{
	data.clear();
	std::vector<u8> rawData;
	int height = 0;
	if (renderer == nullptr || !renderer->GetLastFrame(rawData, width, height))
		return;
	stbi_flip_vertically_on_write(0);
	stbi_write_png_to_func(appendVectorData, &data, width, height, 3, &rawData[0], 0);
}

static void savestate(int slot)
{
	// Potential optimization: move screenshot/state compression/write off the UI thread.
	std::vector<u8> pngData;
	getScreenshot(pngData, 640);
	dc_savestate(slot, pngData.empty() ? nullptr : &pngData[0], pngData.size());
	ImguiStateTexture savestatePic(slot);
	savestatePic.invalidate();
}

static void savestate()
{
	savestate(config::SavestateSlot);
}

static void render_exit_save_dialog()
{
	static bool wasDialogShown = false;

	if (!showExitSaveDialog)
	{
		wasDialogShown = false;
		return;
	}

	// Center dialog
	ImGui::SetNextWindowPos(
		ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
		ImGuiCond_Always, ImVec2(0.5f, 0.5f));

	// Only open popup if it's not already open and this is a new request
	if (showExitSaveDialog && !wasDialogShown)
	{
		ImGui::OpenPopup(Tnop("Exit Save Prompt"));
		wasDialogShown = true;
	}

	const bool exitEmulator = exitSaveDialogAction == ExitSaveDialogAction::ExitEmulator;
	const std::string exitGamePopup = std::string(exitEmulator ? T("Exit Emulator?") : T("Exit Game?")) + "###Exit Save Prompt";
	if (ImGui::BeginPopupModal(exitGamePopup.c_str(), NULL,
		ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::TextUnformatted(exitEmulator
			? T("Do you want to save state before exiting the emulator?")
			: T("Do you want to save state before returning to the library?"));
		ImGui::NewLine();

		// Button styling (consistent with existing dialogs)
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(uiScaled(20), ImGui::GetStyle().ItemSpacing.y));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(10, 10));
		const char *saveAndExit = T("Save & Exit");
		const char *exitWithoutSaving = T("Exit Without Saving");
		const char *cancelButton = T("Cancel");
		const float buttonWidth = std::max({
			ImGui::CalcTextSize(saveAndExit).x,
			ImGui::CalcTextSize(exitWithoutSaving).x,
			ImGui::CalcTextSize(cancelButton).x
		}) + ImGui::GetStyle().FramePadding.x * 2 + uiScaled(24.f);

		// Check if save is allowed
		bool canSave = dc_savestateAllowed();

		if (ImGui::Button(saveAndExit, ImVec2(buttonWidth, 0)))
		{
			if (canSave)
			{
				ImGui::CloseCurrentPopup();
				showExitSaveDialog = false;
				finish_exit_request(exitEmulator, true);
			}
			else
			{
				// Show error that save isn't allowed
				ImGui::OpenPopup(Tnop("Save Not Allowed"));
			}
		}
		ImGui::SameLine();
		if (ImGui::Button(exitWithoutSaving, ImVec2(buttonWidth, 0)))
		{
			ImGui::CloseCurrentPopup();
			showExitSaveDialog = false;
			finish_exit_request(exitEmulator, false);
		}
		ImGui::SameLine();
		if (ImGui::Button(cancelButton, ImVec2(buttonWidth, 0)))
		{
			ImGui::CloseCurrentPopup();
			showExitSaveDialog = false;
			exitSaveDialogAction = ExitSaveDialogAction::ReturnToLibrary;
		}

		// Error popup for save not allowed
		const std::string saveNotAllowedPopup = std::string(T("Save Not Allowed")) + "###Save Not Allowed";
		if (ImGui::BeginPopupModal(saveNotAllowedPopup.c_str(), NULL,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
		{
			ImGui::TextUnformatted(T("Cannot save state in current mode."));
			ImGui::TextUnformatted(T("Possible reasons:"));
			ImGui::BulletText("%s", T("No game loaded"));
			ImGui::BulletText("%s", T("Network play active"));
			ImGui::BulletText("%s", T("Multi-board arcade mode"));
			ImGui::NewLine();
			if (ImGui::Button(T("OK"), ImVec2(120, 0)))
			{
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}

		ImGui::PopStyleVar(2);
		ImGui::EndPopup();
	}
}

static void render_auto_load_dialog()
{
	static bool wasDialogShown = false;

	if (!showAutoLoadDialog)
	{
		wasDialogShown = false;
		return;
	}

	ImGui::SetNextWindowPos(
		ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f),
		ImGuiCond_Always, ImVec2(0.5f, 0.5f));

	if (!wasDialogShown)
	{
		ImGui::OpenPopup(Tnop("Auto-Load Prompt"));
		wasDialogShown = true;
	}

	const std::string popupTitle = std::string(T("Load Auto-Save?")) + "###Auto-Load Prompt";
	if (ImGui::BeginPopupModal(popupTitle.c_str(), nullptr,
			ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
	{
		ImGui::TextUnformatted(T("A hidden Auto-Save was found for this game."));
		ImGui::TextUnformatted(T("Do you want to load it?"));
		const int autoSaveSlot = dc_getAutoSaveSlot();
		const time_t savestateDate = dc_getStateCreationDate(autoSaveSlot);
		if (savestateDate != 0)
			ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.f), "%s", timeToShortDateTimeString(savestateDate).c_str());

		ImGui::Spacing();
		ImguiStateTexture savestatePic(autoSaveSlot);
		savestatePic.draw(ScaledVec2(320.f, 0.f));
		ImGui::Spacing();

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(uiScaled(20), ImGui::GetStyle().ItemSpacing.y));
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ScaledVec2(10, 10));
		const char *yesButton = T("Yes");
		const char *noButton = T("No");
		const float buttonWidth = std::max(ImGui::CalcTextSize(yesButton).x, ImGui::CalcTextSize(noButton).x)
			+ ImGui::GetStyle().FramePadding.x * 2 + uiScaled(60.f);

		if (ImGui::Button(yesButton, ImVec2(buttonWidth, 0)))
		{
			dc_loadstate(autoSaveSlot);
			ImGui::CloseCurrentPopup();
			showAutoLoadDialog = false;
			gui_setState(GuiState::Closed);
		}
		ImGui::SameLine();
		if (ImGui::Button(noButton, ImVec2(buttonWidth, 0)))
		{
			ImGui::CloseCurrentPopup();
			showAutoLoadDialog = false;
			gui_setState(GuiState::Closed);
		}

		ImGui::PopStyleVar(2);
		ImGui::EndPopup();
	}
}

static void gui_display_commands()
{
	fullScreenWindow(false);
	ImGui::SetNextWindowBgAlpha(0.8f);
	ImguiStyleVar _{ImGuiStyleVar_WindowBorderSize, 0};

	ImGui::Begin("##commands", nullptr, ImGuiWindowFlags_NoDecoration);
	{
		ImguiStyleVar _{ImGuiStyleVar_ButtonTextAlign, ImVec2(0.f, 0.5f)};	// left aligned

		float columnWidth = std::min(200.f,
				(ImGui::GetContentRegionAvail().x - uiScaled(100 + 150) - ImGui::GetStyle().FramePadding.x * 2)
				/ 2 / uiScaled(1));
		float buttonWidth = 150.f;	// not scaled
		bool lowWidth = ImGui::GetContentRegionAvail().x < uiScaled(100 + buttonWidth * 3)
				+ ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x * 2;
		if (lowWidth)
			buttonWidth = std::min(150.f,
					(ImGui::GetContentRegionAvail().x - ImGui::GetStyle().FramePadding.x * 2 - ImGui::GetStyle().ItemSpacing.x * 2)
					/ 3 / uiScaled(1));
		bool lowHeight = ImGui::GetContentRegionAvail().y < uiScaled(100 + 50 * 2 + buttonWidth * 3 / 4) + ImGui::GetTextLineHeightWithSpacing() * 2
				+ ImGui::GetStyle().ItemSpacing.y * 2 + ImGui::GetStyle().WindowPadding.y;

		GameMedia game;
		game.path = settings.content.path;
		game.fileName = settings.content.fileName;
		GameBoxart art = boxart.getBoxart(game);
		ImguiFileTexture tex(art.boxartPath);
		// Use a fallback image when artwork is unavailable
		tex.draw(ScaledVec2(100, 100));

		ImGui::SameLine();
		if (!lowHeight)
		{
			ImGui::BeginChild("game_info", ScaledVec2(0, 100.f), ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar);
			ImGui::PushFont(nullptr, uiLargeFontSize());
			ImGui::Text("%s", art.name.c_str());
			ImGui::PopFont();
			{
				ImguiStyleColor _(ImGuiCol_Text, ImVec4(0.75f, 0.75f, 0.75f, 1.f));
				ImGui::TextWrapped("%s", art.fileName.c_str());
				if (!art.arcade && !art.uniqueId.empty())
					ImGui::Text(T("UID: %s"), art.uniqueId.c_str());
			}
			ImGui::EndChild();
		}

		if (lowWidth) {
			ImGui::Columns(3, "buttons", false);
		}
		else
		{
			ImGui::Columns(4, "buttons", false);
			ImGui::SetColumnWidth(0, uiScaled(100.f)  + ImGui::GetStyle().ItemSpacing.x);
			ImGui::SetColumnWidth(1, uiScaled(columnWidth));
			ImGui::SetColumnWidth(2, uiScaled(columnWidth));
			const ImVec2 vmuPos = ImGui::GetStyle().WindowPadding + ScaledVec2(0.f, 100.f)
					+ ImVec2(insetLeft, ImGui::GetStyle().ItemSpacing.y + getScaledTopInset());
			ImguiVmuTexture::displayVmus(vmuPos);
			ImGui::NextColumn();
		}
		ImguiStyleVar _1{ImGuiStyleVar_FramePadding, ScaledVec2(12.f, 3.f)};

		// Resume
		if (IconButton(ICON_FA_PLAY, T("Resume"), ScaledVec2(buttonWidth, 50)).realize())
		{
			GamepadDevice::load_system_mappings();
			gui_resume_game();
		}
		// Cheats
		{
			DisabledScope _{settings.network.online || settings.raHardcoreMode};

			if (IconButton(ICON_FA_MASK, T("Cheats"), ScaledVec2(buttonWidth, 50)).realize() && !settings.network.online)
				gui_setState(GuiState::Cheats);
		}
		// Achievements
		{
			DisabledScope _{!achievements::isActive()};

			if (IconButton(ICON_FA_TROPHY, T("Achievements"), ScaledVec2(buttonWidth, 50)).realize() && achievements::isActive())
				gui_setState(GuiState::Achievements);
		}
		// Barcode
		if (card_reader::barcodeAvailable())
		{
			ImGui::Text("%s", T("Barcode Card"));
			char cardBuf[64] {};
			strncpy(cardBuf, card_reader::barcodeGetCard().c_str(), sizeof(cardBuf) - 1);
			ImGui::SetNextItemWidth(uiScaled(buttonWidth));
			if (InputText("##barcode", cardBuf, sizeof(cardBuf), ImGuiInputTextFlags_None))
				card_reader::barcodeSetCard(cardBuf);
		}

		ImGui::NextColumn();

		// Insert/Eject Disk
		std::string disk_label = gdr::isOpen() ? T("Insert Disk") : T("Eject Disk");
		if (IconButton(ICON_FA_COMPACT_DISC, disk_label, ScaledVec2(buttonWidth, 50)).realize())
		{
			if (gdr::isOpen()) {
				gui_setState(GuiState::SelectDisk);
			}
			else {
				emu.openGdrom();
				gui_setState(GuiState::Loading);
			}
		}
			// Settings
			if (ImGui::Button((std::string(ICON_FA_GEAR "  ") + T("Settings")).c_str(), ScaledVec2(buttonWidth, 50)))
			{
				gui_setState(GuiState::Settings);
			}

		// Exit
		if (IconButton(ICON_FA_POWER_OFF, commandLineStart ?  T("Exit") : T("Close Game"), ScaledVec2(buttonWidth, 50)).realize())
			gui_request_exit_to_library();

		ImGui::NextColumn();
		{
			DisabledScope _{!dc_savestateAllowed()};
			ImguiStateTexture savestatePic;
			time_t savestateDate = dc_getStateCreationDate(config::SavestateSlot);

			// Load State
			{
				DisabledScope _{settings.raHardcoreMode || savestateDate == 0};
				if (IconButton(ICON_FA_CLOCK_ROTATE_LEFT, T("Load State"), ScaledVec2(buttonWidth, 50)).realize() && dc_savestateAllowed())
				{
					gui_resume_game();
					dc_loadstate(config::SavestateSlot);
				}
			}

			// Save State
			if (IconButton(ICON_FA_DOWNLOAD, T("Save State"), ScaledVec2(buttonWidth, 50)).realize() && dc_savestateAllowed())
			{
				gui_resume_game();
				savestate();
			}

			// Slot #
			if (ImGui::ArrowButton("##prev-slot", ImGuiDir_Left))
				gui_cycleSaveStateSlot(-1);
			std::string slot = strprintf(T("Slot %d"), (int)config::SavestateSlot + 1);
			float spacingW = (uiScaled(buttonWidth) - ImGui::GetFrameHeight() * 2 - ImGui::CalcTextSize(slot.c_str()).x) / 2;
			ImGui::SameLine(0, spacingW);
			ImGui::Text("%s", slot.c_str());
			ImGui::SameLine(0, spacingW);
			if (ImGui::ArrowButton("##next-slot", ImGuiDir_Right))
				gui_cycleSaveStateSlot(1);
			{
				ImVec4 gray(0.75f, 0.75f, 0.75f, 1.f);
				if (savestateDate == 0)
					ImGui::TextColored(gray, "%s", T("Empty"));
				else
					ImGui::TextColored(gray, "%s", timeToShortDateTimeString(savestateDate).c_str());
			}
			savestatePic.draw(ScaledVec2(buttonWidth, 0.f));
		}

		ImGui::Columns(1, nullptr, false);
	}
	ImGui::End();
}

void error_popup()
{
	if (!error_msg_shown && !error_msg.empty())
	{
		ImVec2 padding = ScaledVec2(20, 20);
		ImguiStyleVar _(ImGuiStyleVar_WindowPadding, padding);
		ImguiStyleVar _1(ImGuiStyleVar_ItemSpacing, padding);
		ImGui::OpenPopup(Tnop("Error"));
		const std::string errorPopup = std::string(T("Error")) + "###Error";
		if (ImGui::BeginPopupModal(errorPopup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar))
		{
			ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + uiScaled(400.f));
			ImGui::TextWrapped("%s", error_msg.c_str());
			{
				ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(16, 3));
				float currentwidth = ImGui::GetContentRegionAvail().x;
				ImGui::SetCursorPosX((currentwidth - uiScaled(80.f)) / 2.f + ImGui::GetStyle().WindowPadding.x);
				if (ImGui::Button(T("OK"), ScaledVec2(80.f, 0)))
				{
					error_msg.clear();
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::SetItemDefaultFocus();
			ImGui::PopTextWrapPos();
			ImGui::EndPopup();
		}
		error_msg_shown = true;
	}
}

static void render_modal_dialogs()
{
	error_popup();
	render_exit_save_dialog();
	render_auto_load_dialog();
}

static void contentpath_warning_popup()
{
    static bool show_contentpath_selection;

    if (scanner.content_path_looks_incorrect)
    {
        ImGui::OpenPopup(Tnop("Incorrect Content Location?"));
        const std::string incorrectContentPopup = std::string(T("Incorrect Content Location?")) + "###Incorrect Content Location?";
        if (ImGui::BeginPopupModal(incorrectContentPopup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
        {
            ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + uiScaled(400.f));
            ImGui::TextWrapped((std::string("  ") + T("Scanned %d folders but no game can be found!") + std::string("  ")).c_str(), scanner.empty_folders_scanned);
			{
				ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(16, 3));
				float currentwidth = ImGui::GetContentRegionAvail().x;
				ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x - uiScaled(55.f));
				if (ImGui::Button(T("Reselect"), ScaledVec2(100.f, 0)))
				{
					scanner.content_path_looks_incorrect = false;
					ImGui::CloseCurrentPopup();
					show_contentpath_selection = true;
				}

				ImGui::SameLine();
				ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x + uiScaled(55.f));
				if (ImGui::Button(T("Cancel"), ScaledVec2(100.f, 0)))
				{
					scanner.content_path_looks_incorrect = false;
					ImGui::CloseCurrentPopup();
					scanner.stop();
					config::ContentPath.get().clear();
				}
			}
            ImGui::SetItemDefaultFocus();
            ImGui::EndPopup();
        }
    }
    if (show_contentpath_selection)
    {
        scanner.stop();
        const char *title = T("Select a Content Folder");
        ImGui::OpenPopup(title);
        select_file_popup(title, [](bool cancelled, std::string selection)
        {
            show_contentpath_selection = false;
            if (!cancelled)
            {
            	config::ContentPath.get().clear();
                config::ContentPath.get().push_back(selection);
            }
            scanner.refresh();
            return true;
        });
    }
}

void os_notify(const char *msg, int durationMs, const char *details)
{
	if (gui_state != GuiState::Closed && gui_state != GuiState::Pause)
	{
		std::lock_guard<std::mutex> _{osd_message_mutex};
		osd_message = msg;
		osd_message_end = getTimeMs() + durationMs;
	}
	else {
		toast.show(msg, details != nullptr ? details : "", durationMs);
	}
}

static std::string get_notification()
{
	std::lock_guard<std::mutex> lock(osd_message_mutex);
	if (!osd_message.empty() && getTimeMs() >= osd_message_end)
		osd_message.clear();
	return osd_message;
}

static void drawPauseIcon()
{
	const char *icon = ICON_FA_PAUSE;
	ImFont *font = ImGui::GetFont();
	const float fontSize = uiScaled(52.f);
	const ScaledVec2 padding(14.f, 6.f);
	const ScaledVec2 margin(12.f, 12.f);
	const ImVec2 size =  ImVec2(fontSize * 0.7, fontSize) + padding * 2;
	const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
	ImVec2 pos(displaySize.x - insetRight - margin.x - size.x, insetTop + margin.y);
	ImDrawList *dl = ImGui::GetForegroundDrawList();
	const ImU32 bgCol = alphaOverride(ImGui::GetColorU32(ImGuiCol_WindowBg), 0.45f);
	const ImU32 shadowCol = alphaOverride(0, 0.65f);
	const ImU32 textCol = alphaOverride(ImGui::GetColorU32(ImGuiCol_Text), 0.95f);

	dl->AddRectFilled(pos, pos + size, bgCol, uiScaled(6.f));

	ImVec2 iconPos = pos + padding + ScaledVec2(2.5f, 2.5f);
	dl->AddText(font, fontSize, iconPos + ScaledVec2(2.5f, 2.5f), shadowCol, icon);
	dl->AddText(font, fontSize, iconPos, textCol, icon);
}

inline static void gui_display_demo() {
	ImGui::ShowDemoWindow();
}

static void gameTooltip(const std::string& tip)
{
    if (ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
        ImGui::TextUnformatted(tip.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static GameBoxart getLibraryDisplayArtwork(const GameMedia& game, double animationClock);

static const char *game_info_value(const std::string& value)
{
	return value.empty() ? T("Unknown") : value.c_str();
}

static ImFont *game_info_title_font()
{
	return settingsRightValueFont != nullptr ? settingsRightValueFont : (largeFont != nullptr ? largeFont : ImGui::GetFont());
}

static ImFont *game_info_body_font()
{
	return settingsTitleFont != nullptr ? settingsTitleFont : ImGui::GetFont();
}

static std::string format_game_info_release_date(const std::string& value)
{
	if (value.size() < 8 || !std::all_of(value.begin(), value.begin() + 8, [](unsigned char c) { return std::isdigit(c); }))
		return value;
	return value.substr(0, 4) + "-" + value.substr(4, 2) + "-" + value.substr(6, 2);
}

static void draw_game_info_stat(const char *label, const std::string& value)
{
	ImGui::PushFont(game_info_body_font());
	ImGui::TextColored(ImVec4(0.52f, 0.70f, 0.82f, 1.0f), "%s", T(label));
	ImGui::TextWrapped("%s", game_info_value(value));
	ImGui::PopFont();
}

struct GameInfoMediaEntry
{
	const char *label;
	std::string path;
	bool selected = false;
};

struct HoverGameInfoCache
{
	std::string gameId;
	GameBoxart art;
	std::string mixImagePath;
	std::string titleImagePath;
	std::string coverPath;
	std::string casePath;
	std::string screenshotPath;
	std::string fanArtPath;
	std::string titleScreenPath;
	std::string physicalPath;
	std::string manualPath;
	std::string primaryImagePath;
	std::string title;
	int selectedMediaIndex = 0;
	bool ready = false;
};

static void draw_game_info_media_label(const char *label, bool present)
{
	const ImVec4 presentColor(0.35f, 0.95f, 0.45f, 1.0f);
	const ImVec4 missingColor(0.45f, 0.45f, 0.52f, 1.0f);
	ImGui::PushFont(game_info_body_font());
	ImGui::TextColored(present ? presentColor : missingColor, "%s", label);
	const bool hasRoomForNextLabel = ImGui::GetContentRegionAvail().x > ImGui::CalcTextSize("Title Screen").x + uiScaled(18.0f);
	ImGui::PopFont();
	if (hasRoomForNextLabel)
		ImGui::SameLine(0.0f, uiScaled(10.0f));
}

static bool draw_game_info_image(const std::string& path, const ImVec2& size)
{
	if (path.empty())
		return false;
	ImguiFileTexture tex(path);
	tex.draw(size);
	return true;
}

static ImVec2 fit_game_info_image(float aspectRatio, ImVec2 boxSize, ImVec2 maxSize)
{
	if (aspectRatio <= 0.0f || boxSize.x <= 0.0f || boxSize.y <= 0.0f)
		return ImVec2(0.0f, 0.0f);

	ImVec2 drawSize(std::min(boxSize.x, maxSize.x), std::min(boxSize.y, maxSize.y));
	if (drawSize.x / drawSize.y > aspectRatio)
		drawSize.x = drawSize.y * aspectRatio;
	else
		drawSize.y = drawSize.x / aspectRatio;
	return drawSize;
}

static std::string game_info_path_filename(const std::string& path)
{
	const size_t pos = path.find_last_of("/\\");
	return pos == std::string::npos ? path : path.substr(pos + 1);
}

static ImTextureID get_game_info_texture_id(const std::string& path)
{
	if (path.empty())
		return {};
	ImguiFileTexture tex(path);
	return tex.getId();
}

static bool draw_game_info_image_contained(const std::string& path, const ImVec2& boxSize, const ImVec2& maxDrawSize, ImVec2 *actualDrawSize = nullptr)
{
	const ImVec2 start = ImGui::GetCursorScreenPos();
	ImGui::Dummy(boxSize);

	ImTextureID id = get_game_info_texture_id(path);
	if (id == ImTextureID{})
		return false;

	const ImVec2 drawSize = fit_game_info_image(imguiDriver->getAspectRatio(id), boxSize, maxDrawSize);
	if (drawSize.x <= 0.0f || drawSize.y <= 0.0f)
		return false;

	if (actualDrawSize != nullptr)
		*actualDrawSize = drawSize;
	const ImVec2 drawPos(start.x + (boxSize.x - drawSize.x) * 0.5f, start.y + (boxSize.y - drawSize.y) * 0.5f);
	ImGui::GetWindowDrawList()->AddImage(id, drawPos, drawPos + drawSize);
	return true;
}

static bool draw_game_info_image_cover_cropped(const std::string& path, ImDrawList *drawList, const ImVec2& pMin, const ImVec2& pMax, ImU32 tint)
{
	ImTextureID id = get_game_info_texture_id(path);
	if (id == ImTextureID{})
		return false;

	const float boxW = pMax.x - pMin.x;
	const float boxH = pMax.y - pMin.y;
	const float imageAspect = imguiDriver->getAspectRatio(id);
	if (boxW <= 0.0f || boxH <= 0.0f || imageAspect <= 0.0f)
		return false;

	const float boxAspect = boxW / boxH;
	ImVec2 uv0(0.0f, 0.0f);
	ImVec2 uv1(1.0f, 1.0f);
	if (imageAspect > boxAspect)
	{
		const float visibleWidth = boxAspect / imageAspect;
		const float crop = (1.0f - visibleWidth) * 0.5f;
		uv0.x = crop;
		uv1.x = 1.0f - crop;
	}
	else
	{
		const float visibleHeight = imageAspect / boxAspect;
		const float crop = (1.0f - visibleHeight) * 0.5f;
		uv0.y = crop;
		uv1.y = 1.0f - crop;
	}
	drawList->AddImage(id, pMin, pMax, uv0, uv1, tint);
	return true;
}

static std::vector<GameInfoMediaEntry> make_game_info_media_entries(const HoverGameInfoCache& cache)
{
	std::vector<GameInfoMediaEntry> entries {
		{ "Mix", cache.mixImagePath, false },
		{ "Cover", !cache.coverPath.empty() ? cache.coverPath : cache.art.boxartPath, false },
		{ "Case", cache.casePath, false },
		{ "Screenshot", cache.screenshotPath, false },
		{ "Marquee", cache.titleImagePath, false },
		{ "Title Screen", cache.titleScreenPath, false },
		{ "Fan Art", cache.fanArtPath, false },
		{ "Physical", cache.physicalPath, false },
		{ "Manual", cache.manualPath, false },
	};
	if (!entries.empty())
		entries[std::clamp(cache.selectedMediaIndex, 0, (int)entries.size() - 1)].selected = true;
	return entries;
}

static std::string selected_game_info_media_path(const std::vector<GameInfoMediaEntry>& mediaEntries)
{
	for (const GameInfoMediaEntry& media : mediaEntries)
		if (media.selected)
			return media.path;
	return {};
}

static const GameInfoMediaEntry *selected_game_info_media(const std::vector<GameInfoMediaEntry>& mediaEntries)
{
	for (const GameInfoMediaEntry& media : mediaEntries)
		if (media.selected)
			return &media;
	return mediaEntries.empty() ? nullptr : &mediaEntries[0];
}

static int default_game_info_media_index(const HoverGameInfoCache& cache)
{
	std::vector<GameInfoMediaEntry> mediaEntries = make_game_info_media_entries(cache);
	for (int i = 0; i < (int)mediaEntries.size(); i++)
		if (!mediaEntries[i].path.empty())
			return i;
	return 0;
}

static std::string best_game_info_hero_background_path(const HoverGameInfoCache& cache)
{
	if (!cache.fanArtPath.empty())
		return cache.fanArtPath;
	if (!cache.screenshotPath.empty())
		return cache.screenshotPath;
	if (!cache.titleScreenPath.empty())
		return cache.titleScreenPath;
	if (!cache.mixImagePath.empty())
		return cache.mixImagePath;
	if (!cache.coverPath.empty())
		return cache.coverPath;
	if (!cache.casePath.empty())
		return cache.casePath;
	return {};
}

static void draw_game_info_chip(const std::string& text)
{
	if (text.empty())
		return;
	ImguiStyleVar rounding(ImGuiStyleVar_FrameRounding, uiScaled(10.0f));
	ImguiStyleVar padding(ImGuiStyleVar_FramePadding, ScaledVec2(10.0f, 4.0f));
	ImguiStyleColor button(ImGuiCol_Button, ImVec4(0.06f, 0.12f, 0.16f, 0.92f));
	ImguiStyleColor buttonHovered(ImGuiCol_ButtonHovered, ImVec4(0.08f, 0.18f, 0.22f, 0.95f));
	ImguiStyleColor buttonActive(ImGuiCol_ButtonActive, ImVec4(0.08f, 0.18f, 0.22f, 0.95f));
	ImguiStyleColor textColor(ImGuiCol_Text, ImVec4(0.72f, 0.86f, 0.92f, 1.0f));
	ImGui::Button(text.c_str());
}

static void draw_game_info_chip_row(const GameMedia& game)
{
	std::vector<std::string> chips;
	if (!game.players.empty())
		chips.push_back(game.players);
	if (!game.releaseDate.empty())
		chips.push_back(format_game_info_release_date(game.releaseDate).substr(0, 4));
	if (!game.genre.empty())
		chips.push_back(game.genre);
	if (!game.developer.empty())
		chips.push_back(game.developer);

	const float spacing = ImGui::GetStyle().ItemSpacing.x;
	const float padding = ImGui::GetStyle().FramePadding.x * 2.0f;
	const float avail = ImGui::GetContentRegionAvail().x;
	while (!chips.empty())
	{
		float totalWidth = spacing * static_cast<float>(chips.size() - 1);
		for (const std::string& chip : chips)
			totalWidth += ImGui::CalcTextSize(chip.c_str()).x + padding;
		if (totalWidth <= avail || chips.size() <= 2)
		{
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (avail - totalWidth) * 0.5f));
			for (size_t i = 0; i < chips.size(); i++)
			{
				if (i != 0)
					ImGui::SameLine();
				draw_game_info_chip(chips[i]);
			}
			break;
		}
		chips.pop_back();
	}
}

static bool draw_game_info_badge(const GameInfoMediaEntry& media, int index)
{
	const bool found = !media.path.empty();
	ImVec4 bg = media.selected ? ImVec4(0.08f, 0.34f, 0.45f, 1.0f)
			: found ? ImVec4(0.04f, 0.20f, 0.10f, 0.88f)
			: ImVec4(0.13f, 0.14f, 0.16f, 0.84f);
	ImVec4 text = media.selected ? ImVec4(0.90f, 0.98f, 1.0f, 1.0f)
			: found ? ImVec4(0.32f, 0.94f, 0.48f, 1.0f)
			: ImVec4(0.50f, 0.53f, 0.58f, 1.0f);

	ImguiStyleVar rounding(ImGuiStyleVar_FrameRounding, uiScaled(11.0f));
	ImguiStyleVar padding(ImGuiStyleVar_FramePadding, ScaledVec2(9.0f, 4.0f));
	ImguiStyleColor button(ImGuiCol_Button, bg);
	ImguiStyleColor buttonHovered(ImGuiCol_ButtonHovered, bg);
	ImguiStyleColor buttonActive(ImGuiCol_ButtonActive, bg);
	ImguiStyleColor textColor(ImGuiCol_Text, text);
	const std::string label = std::string(found ? ICON_FA_CIRCLE " " : ICON_FA_CIRCLE_DOT " ") + media.label
			+ "##library_media_badge_" + std::to_string(index);
	const bool clicked = ImGui::Button(label.c_str());
	if (!found && ImGui::IsItemHovered())
		ImGui::SetTooltip("%s media missing", media.label);
	return clicked;
}

static int draw_game_info_badge_wrap(const std::vector<GameInfoMediaEntry>& mediaEntries)
{
	int clickedIndex = -1;
	for (size_t i = 0; i < mediaEntries.size(); i++)
	{
		const float nextWidth = ImGui::CalcTextSize(mediaEntries[i].label).x + ImGui::GetStyle().FramePadding.x * 2.0f + uiScaled(26.0f);
		if (i != 0 && ImGui::GetContentRegionAvail().x > nextWidth + uiScaled(12.0f))
			ImGui::SameLine();
		if (draw_game_info_badge(mediaEntries[i], (int)i))
			clickedIndex = (int)i;
	}
	return clickedIndex;
}

static void draw_game_info_detail_row(const char *label, const std::string& value)
{
	ImGui::PushFont(game_info_body_font());
	ImGui::TextColored(ImVec4(0.44f, 0.64f, 0.72f, 1.0f), "%s", T(label));
	ImGui::SameLine(uiScaled(110.0f));
	ImGui::TextColored(ImVec4(0.86f, 0.89f, 0.92f, 1.0f), "%s", game_info_value(value));
	ImGui::PopFont();
}

static void draw_library_hover_hero(const HoverGameInfoCache& cache, const GameMedia& game, float width)
{
	const ImVec2 heroSize(width, uiScaled(144.0f));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p + ScaledVec2(0.0f, 4.0f), p + heroSize + ScaledVec2(0.0f, 4.0f),
			IM_COL32(0, 0, 0, 120), uiScaled(14.0f));
	dl->AddRectFilled(p, p + heroSize, IM_COL32(5, 8, 11, 245), uiScaled(14.0f));
	if (draw_game_info_image_cover_cropped(best_game_info_hero_background_path(cache), dl, p, p + heroSize, IM_COL32(255, 255, 255, 62)))
		dl->AddRectFilled(p, p + heroSize, IM_COL32(0, 0, 0, 142), uiScaled(14.0f));
	dl->AddRectFilled(ImVec2(p.x, p.y + heroSize.y - uiScaled(48.0f)), p + heroSize,
			IM_COL32(0, 0, 0, 112), uiScaled(14.0f));
	dl->AddRectFilled(p + ScaledVec2(24.0f, 16.0f), ImVec2(p.x + heroSize.x - uiScaled(24.0f), p.y + uiScaled(84.0f)),
			IM_COL32(0, 170, 220, 24), uiScaled(18.0f));
	dl->AddRect(p, p + heroSize, IM_COL32(18, 145, 205, 135), uiScaled(14.0f), 0, uiScaled(1.0f));

	if (ImGui::BeginChild("##libraryHoverHero", heroSize, false,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::SetCursorPosY(uiScaled(14.0f));
		ImVec2 titleDrawSize;
		const bool drewLogo = draw_game_info_image_contained(cache.titleImagePath,
				ImVec2(ImGui::GetContentRegionAvail().x, uiScaled(78.0f)),
				ImVec2(std::min(width * 0.68f, uiScaled(680.0f)), uiScaled(78.0f)),
				&titleDrawSize);
		if (!drewLogo)
		{
			ImGui::PushFont(game_info_title_font());
			const ImVec2 textSize = ImGui::CalcTextSize(cache.title.c_str(), nullptr, false, ImGui::GetContentRegionAvail().x);
			ImGui::SetCursorPosX(std::max(uiScaled(10.0f), (ImGui::GetContentRegionAvail().x - textSize.x) * 0.5f));
			ImGui::TextWrapped("%s", cache.title.c_str());
			ImGui::PopFont();
		}
		ImGui::SetCursorPosY(uiScaled(106.0f));
		draw_game_info_chip_row(game);
	}
	ImGui::EndChild();
}

static void draw_library_hover_media_card(const HoverGameInfoCache& cache, const std::vector<GameInfoMediaEntry>& mediaEntries, float width, float height)
{
	if (!ImGui::BeginChild("##libraryHoverMedia", ImVec2(width, height), true,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::EndChild();
		return;
	}

	ImGui::PushFont(game_info_body_font());
	ImGui::TextColored(ImVec4(0.45f, 0.66f, 0.74f, 1.0f), "%s", T("Media Preview"));
	ImGui::PopFont();
	const GameInfoMediaEntry *selected = selected_game_info_media(mediaEntries);
	const std::string selectedPath = selected != nullptr ? selected->path : selected_game_info_media_path(mediaEntries);
	const char *selectedLabel = selected != nullptr ? selected->label : T("Media");
	const bool found = !selectedPath.empty();
	const float previewHeight = std::max(uiScaled(180.0f), height - uiScaled(158.0f));
	const ImVec2 previewSize(ImGui::GetContentRegionAvail().x, previewHeight);
	const ImVec2 previewPos = ImGui::GetCursorScreenPos();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(previewPos, previewPos + previewSize, IM_COL32(4, 6, 8, 180), uiScaled(9.0f));
	dl->AddRect(previewPos, previewPos + previewSize, IM_COL32(18, 145, 205, 80), uiScaled(9.0f));
	if (!draw_game_info_image_contained(selectedPath, previewSize, previewSize - ScaledVec2(14.0f, 14.0f)))
	{
		const std::string missingText = std::string(selectedLabel) + " missing";
		const char *missing = missingText.c_str();
		const ImVec2 textSize = ImGui::CalcTextSize(missing);
		dl->AddCircle(previewPos + previewSize * 0.5f - ScaledVec2(0.0f, 22.0f), uiScaled(14.0f),
				IM_COL32(120, 128, 140, 220), 32, uiScaled(2.0f));
		dl->AddText(previewPos + (previewSize - textSize) * 0.5f + ScaledVec2(0.0f, 18.0f),
				IM_COL32(135, 140, 150, 255), missing);
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::PushFont(game_info_body_font());
	ImGui::TextColored(ImVec4(0.44f, 0.64f, 0.72f, 1.0f), "%s", T("Current Media"));
	ImGui::SameLine(uiScaled(128.0f));
	ImGui::TextColored(ImVec4(0.86f, 0.89f, 0.92f, 1.0f), "%s", selectedLabel);
	ImGui::TextColored(ImVec4(0.44f, 0.64f, 0.72f, 1.0f), "%s", T("Status"));
	ImGui::SameLine(uiScaled(128.0f));
	ImGui::TextColored(found ? ImVec4(0.32f, 0.94f, 0.48f, 1.0f) : ImVec4(0.50f, 0.53f, 0.58f, 1.0f),
			"%s", found ? T("Found") : T("Missing"));
	if (found)
	{
		ImGui::TextColored(ImVec4(0.44f, 0.64f, 0.72f, 1.0f), "%s", T("File"));
		ImGui::SameLine(uiScaled(128.0f));
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
		ImGui::TextColored(ImVec4(0.66f, 0.72f, 0.76f, 1.0f), "%s", game_info_path_filename(selectedPath).c_str());
		ImGui::PopTextWrapPos();
	}
	ImGui::PopFont();
	ImGui::EndChild();
}

static void draw_library_hover_description_card(const GameMedia& game, float height)
{
	if (!ImGui::BeginChild("##libraryHoverDetails", ImVec2(0.0f, height), true))
	{
		ImGui::EndChild();
		return;
	}

	ImGui::TextColored(ImVec4(0.45f, 0.66f, 0.74f, 1.0f), "%s", T("Details"));
	ImGui::Spacing();
	draw_game_info_detail_row("Players", game.players);
	draw_game_info_detail_row("Released", format_game_info_release_date(game.releaseDate));
	draw_game_info_detail_row("Genre", game.genre);
	draw_game_info_detail_row("Developer", game.developer);

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextColored(ImVec4(0.45f, 0.66f, 0.74f, 1.0f), "%s", T("Description"));
	ImGui::Spacing();
	const float wrapWidth = std::min(ImGui::GetContentRegionAvail().x, uiScaled(760.0f));
	if (ImGui::BeginChild("##libraryHoverDescriptionText", ImVec2(0.0f, 0.0f), false))
	{
		ImGui::PushFont(game_info_body_font());
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
		if (game.desc.empty())
			ImGui::TextDisabled("%s", T("No description available."));
		else
			ImGui::TextUnformatted(game.desc.c_str());
		ImGui::PopTextWrapPos();
		ImGui::PopFont();
	}
	ImGui::EndChild();
	ImGui::EndChild();
}

static int draw_library_hover_status_strip(const std::vector<GameInfoMediaEntry>& mediaEntries)
{
	int foundCount = 0;
	for (const GameInfoMediaEntry& media : mediaEntries)
		if (!media.path.empty())
			foundCount++;

	if (!ImGui::BeginChild("##libraryHoverMediaStatus", ImVec2(0.0f, uiScaled(76.0f)), true,
			ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse))
	{
		ImGui::EndChild();
		return -1;
	}

	ImGui::PushFont(game_info_body_font());
	ImGui::TextColored(ImVec4(0.45f, 0.66f, 0.74f, 1.0f), "%s", T("Media"));
	ImGui::SameLine();
	ImGui::TextColored(ImVec4(0.70f, 0.78f, 0.82f, 1.0f), "%d / %d found", foundCount, (int)mediaEntries.size());
	ImGui::PopFont();
	int clickedIndex = -1;
	if (ImGui::BeginChild("##libraryHoverMediaBadges", ImVec2(0.0f, uiScaled(34.0f)), false,
			ImGuiWindowFlags_HorizontalScrollbar))
		clickedIndex = draw_game_info_badge_wrap(mediaEntries);
	ImGui::EndChild();
	ImGui::EndChild();
	return clickedIndex;
}

static void draw_library_game_info_hover(const GameMedia& game, const GameBoxart* displayArt = nullptr)
{
#if defined(__ANDROID__)
	(void)game;
	(void)displayArt;
#else
	static std::string hoveredGameId;
	static double hoverStartTime = 0.0;
	static ImVec2 hoverStartMousePos;
	static HoverGameInfoCache hoverCache;
	static ImVec2 hoverPanelPos;
	static ImVec2 hoverPanelMin;
	static ImVec2 hoverPanelMax;
	static bool hasHoverPanelPos = false;

	if (resetLibraryGameInfoHoverState)
	{
		hoveredGameId.clear();
		hoverStartTime = 0.0;
		hoverStartMousePos = ImVec2();
		hoverCache = {};
		hasHoverPanelPos = false;
		hoverPanelMin = ImVec2();
		hoverPanelMax = ImVec2();
		resetLibraryGameInfoHoverState = false;
	}

	const std::string gameId = !game.path.empty() ? game.path : (!game.fileName.empty() ? game.fileName : game.name);
	const ImVec2 mousePos = ImGui::GetMousePos();
	const ImGuiHoveredFlags hoverFlags = ImGuiHoveredFlags_RectOnly | ImGuiHoveredFlags_NoNavOverride;
	const bool itemHovered = ImGui::IsItemHovered(hoverFlags) && !ImGui::IsMouseDown(ImGuiMouseButton_Left);
	const bool panelHovered = hoveredGameId == gameId
			&& mousePos.x >= hoverPanelMin.x && mousePos.x <= hoverPanelMax.x
			&& mousePos.y >= hoverPanelMin.y && mousePos.y <= hoverPanelMax.y;
	if (!itemHovered && !panelHovered)
		return;

	const float mouseDeltaX = mousePos.x - hoverStartMousePos.x;
	const float mouseDeltaY = mousePos.y - hoverStartMousePos.y;
	const float movementThreshold = uiScaled(6.0f);
	if (itemHovered && (hoveredGameId != gameId || mouseDeltaX * mouseDeltaX + mouseDeltaY * mouseDeltaY > movementThreshold * movementThreshold))
	{
		hoveredGameId = gameId;
		hoverStartTime = ImGui::GetTime();
		hoverStartMousePos = mousePos;
		hoverCache = {};
		hasHoverPanelPos = false;
	}
	const double hoverElapsed = ImGui::GetTime() - hoverStartTime;
	if (hoverElapsed >= 1.0 && (!hoverCache.ready || hoverCache.gameId != gameId))
	{
		hoverCache.gameId = gameId;
		hoverCache.art = displayArt != nullptr ? *displayArt : getLibraryDisplayArtwork(game, 0.0);
		hoverCache.mixImagePath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::MixImage);
		hoverCache.titleImagePath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Title);
		hoverCache.coverPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Cover);
		hoverCache.casePath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Case);
		hoverCache.screenshotPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Screenshot);
		hoverCache.fanArtPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::FanArt);
		hoverCache.titleScreenPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::TitleScreen);
		hoverCache.physicalPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Physical);
		hoverCache.manualPath = !game.manualPath.empty()
				? game.manualPath : boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Manual);
		hoverCache.primaryImagePath = !hoverCache.mixImagePath.empty() ? hoverCache.mixImagePath : hoverCache.art.boxartPath;
		hoverCache.title = game.name;
		if (hoverCache.title.empty() || hoverCache.title == game.fileName)
			hoverCache.title = hoverCache.art.name;
		if (hoverCache.title.empty())
			hoverCache.title = game.fileName;
		hoverCache.selectedMediaIndex = default_game_info_media_index(hoverCache);
		hoverCache.ready = true;
	}
	if (hoverElapsed < LIBRARY_GAME_INFO_HOVER_SECONDS || !hoverCache.ready)
		return;

	const ImGuiViewport *viewport = ImGui::GetMainViewport();
	const ImVec2 displaySize = viewport->Size;
	const float hoverWidth = std::clamp(displaySize.x * 0.72f, uiScaled(820.0f), uiScaled(1180.0f));
	const float hoverHeight = std::clamp(displaySize.y * 0.74f, uiScaled(560.0f), uiScaled(720.0f));
	if (itemHovered || !hasHoverPanelPos)
	{
		hoverPanelPos = ImGui::GetMousePos() + ScaledVec2(18.0f, 18.0f);
		if (hoverPanelPos.x + hoverWidth > viewport->Pos.x + displaySize.x - uiScaled(12.0f))
			hoverPanelPos.x = ImGui::GetMousePos().x - hoverWidth - uiScaled(18.0f);
		if (hoverPanelPos.y + hoverHeight > viewport->Pos.y + displaySize.y - uiScaled(12.0f))
			hoverPanelPos.y = viewport->Pos.y + displaySize.y - hoverHeight - uiScaled(12.0f);
		hoverPanelPos.x = std::max(viewport->Pos.x + uiScaled(12.0f), hoverPanelPos.x);
		hoverPanelPos.y = std::max(viewport->Pos.y + uiScaled(12.0f), hoverPanelPos.y);
		hasHoverPanelPos = true;
	}
	hoverPanelMin = hoverPanelPos;
	hoverPanelMax = hoverPanelPos + ImVec2(hoverWidth, hoverHeight);

	ImGui::GetBackgroundDrawList()->AddRectFilled(viewport->Pos, viewport->Pos + viewport->Size, IM_COL32(0, 0, 0, 138));
	ImGui::SetNextWindowPos(hoverPanelPos, ImGuiCond_Always);
	ImGui::SetNextWindowSize(ImVec2(hoverWidth, hoverHeight), ImGuiCond_Always);
	ImguiStyleVar windowPadding(ImGuiStyleVar_WindowPadding, ScaledVec2(18.0f, 16.0f));
	ImguiStyleVar framePadding(ImGuiStyleVar_FramePadding, ScaledVec2(10.0f, 6.0f));
	ImguiStyleVar itemSpacing(ImGuiStyleVar_ItemSpacing, ScaledVec2(12.0f, 8.0f));
	ImguiStyleVar windowRounding(ImGuiStyleVar_WindowRounding, uiScaled(12.0f));
	ImguiStyleVar childRounding(ImGuiStyleVar_ChildRounding, uiScaled(10.0f));
	ImguiStyleVar frameRounding(ImGuiStyleVar_FrameRounding, uiScaled(8.0f));
	ImguiStyleVar borderSize(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImguiStyleColor borderColor(ImGuiCol_Border, ImVec4(0.10f, 0.55f, 0.82f, 0.55f));
	ImguiStyleColor childBgColor(ImGuiCol_ChildBg, ImVec4(0.035f, 0.040f, 0.045f, 0.94f));
	ImguiStyleColor windowBgColor(ImGuiCol_WindowBg, ImVec4(0.015f, 0.018f, 0.022f, 0.97f));
	const ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration
			| ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoNav
			| ImGuiWindowFlags_NoMove;
	if (!ImGui::Begin("##libraryGameInfoHover", nullptr, windowFlags))
	{
		ImGui::End();
		return;
	}

	const float width = ImGui::GetContentRegionAvail().x;
	const std::vector<GameInfoMediaEntry> mediaEntries = make_game_info_media_entries(hoverCache);
	draw_library_hover_hero(hoverCache, game, width);

	ImGui::Spacing();
	const float mediaWidth = std::clamp(width * 0.31f, uiScaled(300.0f), uiScaled(390.0f));
	const float statusHeight = uiScaled(76.0f);
	const float mainHeight = std::max(uiScaled(300.0f), ImGui::GetContentRegionAvail().y - statusHeight - uiScaled(10.0f));
	draw_library_hover_media_card(hoverCache, mediaEntries, mediaWidth, mainHeight);

	ImGui::SameLine(0.0f, uiScaled(14.0f));
	draw_library_hover_description_card(game, mainHeight);

	ImGui::Spacing();
	const int clickedMediaIndex = draw_library_hover_status_strip(mediaEntries);
	if (clickedMediaIndex >= 0)
		hoverCache.selectedMediaIndex = clickedMediaIndex;
	ImGui::End();
#endif
}

static void gui_display_game_info()
{
	const GameMedia game = selectedGameForInfo;
	const GameBoxart art = getLibraryDisplayArtwork(game, 0.0);
	const std::string mixImagePath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::MixImage);
	const std::string titleImagePath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Title);
	const std::string coverPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Cover);
	const std::string casePath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Case);
	const std::string screenshotPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Screenshot);
	const std::string fanArtPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::FanArt);
	const std::string titleScreenPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::TitleScreen);
	const std::string physicalPath = boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Physical);
	const std::string manualPath = !game.manualPath.empty()
			? game.manualPath : boxart.getCustomMediaPath(game, config::LibraryCoverMediaMode::Manual);
	const std::string primaryImagePath = !mixImagePath.empty() ? mixImagePath : art.boxartPath;
	std::string title = game.name;
	if (title.empty() || title == game.fileName)
		title = art.name;
	if (title.empty())
		title = game.fileName;

	ImVec2 windowPos(static_cast<float>(insetLeft), static_cast<float>(insetTop));
	ImVec2 windowSize(std::max(0, settings.display.width - insetLeft - insetRight),
			std::max(0, settings.display.height - insetTop - insetBottom));
	ImGui::SetNextWindowPos(windowPos);
	ImGui::SetNextWindowSize(windowSize);
	ImguiStyleVar _1(ImGuiStyleVar_WindowRounding, 0);
	ImguiStyleVar _2(ImGuiStyleVar_WindowBorderSize, 0);

	if (ImGui::Begin("##gameInfo", nullptr, ImGuiWindowFlags_NoDecoration))
	{
		ImguiStyleVar _3(ImGuiStyleVar_FramePadding, ScaledVec2(18, 12));
		ImguiStyleVar _4(ImGuiStyleVar_ItemSpacing, ScaledVec2(0, 10));

		if (ImGui::Button(T("Close")))
			gui_setState(GuiState::Main);
		if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight))
			gui_setState(GuiState::Main);

		ImGui::Dummy(ScaledVec2(0, 4));
		if (!titleImagePath.empty())
		{
			const float headerWidth = std::min(windowSize.x - ImGui::GetStyle().WindowPadding.x * 2.0f, uiScaled(520.0f));
			draw_game_info_image(titleImagePath, ImVec2(headerWidth, uiScaled(92.0f)));
		}
		else
		{
			ImGui::PushFont(nullptr, uiLargeFontSize());
			ImGui::TextWrapped("%s", title.c_str());
			ImGui::PopFont();
		}
		ImGui::Separator();

		const float contentWidth = ImGui::GetContentRegionAvail().x;
		const float leftWidth = std::min(contentWidth * 0.42f, uiScaled(430.0f));
		const float rightWidth = std::max(uiScaled(260.0f), contentWidth - leftWidth - ImGui::GetStyle().ItemSpacing.x);
		const float panelHeight = ImGui::GetContentRegionAvail().y - uiScaled(8.0f);

		if (ImGui::BeginChild("##gameInfoMedia", ImVec2(leftWidth, panelHeight), false))
		{
			const float previewSize = std::min(leftWidth, panelHeight * 0.78f);
			if (!draw_game_info_image(primaryImagePath, ImVec2(previewSize, previewSize)))
			{
				ImGui::Dummy(ImVec2(previewSize, previewSize * 0.6f));
				ImGui::TextDisabled("%s", T("No media preview"));
			}
		}
		ImGui::EndChild();

		ImGui::SameLine();

		if (ImGui::BeginChild("##gameInfoDetails", ImVec2(rightWidth, panelHeight), false))
		{
			if (ImGui::BeginTable("##gameInfoStats", 2, ImGuiTableFlags_SizingStretchSame))
			{
				ImGui::TableNextColumn();
				draw_game_info_stat("Players", game.players);
				ImGui::TableNextColumn();
				draw_game_info_stat("Released", format_game_info_release_date(game.releaseDate));
				ImGui::TableNextColumn();
				draw_game_info_stat("Genre", game.genre);
				ImGui::TableNextColumn();
				draw_game_info_stat("Developer", game.developer);
				ImGui::EndTable();
			}

			ImGui::Separator();
			ImGui::TextDisabled("%s", T("Description"));
			if (game.desc.empty())
				ImGui::TextDisabled("%s", T("No description available."));
			else
				ImGui::TextWrapped("%s", game.desc.c_str());

			ImGui::Separator();
			ImGui::TextDisabled("%s", T("MEDIA:"));
			ImGui::SameLine(0.0f, uiScaled(10.0f));
			draw_game_info_media_label("Mix", !mixImagePath.empty());
			draw_game_info_media_label("Cover", !coverPath.empty());
			draw_game_info_media_label("Case", !casePath.empty());
			draw_game_info_media_label("Screenshot", !screenshotPath.empty());
			draw_game_info_media_label("Marquee", !titleImagePath.empty());
			draw_game_info_media_label("Title Screen", !titleScreenPath.empty());
			draw_game_info_media_label("Fan Art", !fanArtPath.empty());
			draw_game_info_media_label("Physical", !physicalPath.empty());
			draw_game_info_media_label("Manual", !manualPath.empty());
			ImGui::NewLine();

			if (!manualPath.empty())
			{
				ImGui::TextDisabled("%s", T("Manual"));
				ImGui::TextWrapped("%s", manualPath.c_str());
			}
		}
		ImGui::EndChild();
	}
	ImGui::End();
}

static void gui_set_library_game_info(const GameMedia& game)
{
	selectedGameForInfo = game;
	gui_setState(GuiState::GameInfo);
}

static void resetLibraryLongPress()
{
	libraryLongPress = {};
	touchedLibraryItemThisFrame = false;
}

static void resetLibraryGameInfoHover()
{
	resetLibraryGameInfoHoverState = true;
}

static void updateLibraryLongPress(const GameMedia& game, const std::string& gameId, bool itemActive)
{
#if defined(__ANDROID__)
	if (!mouseTouchscreen || gui_state != GuiState::Main)
		return;
#else
	(void)game;
	(void)gameId;
	(void)itemActive;
#endif

#if defined(__ANDROID__)
	if (!itemActive)
		return;

	touchedLibraryItemThisFrame = true;
	if (!libraryLongPress.tracking || libraryLongPress.gameId != gameId)
	{
		libraryLongPress = {};
		libraryLongPress.tracking = true;
		libraryLongPress.startTime = ImGui::GetTime();
		libraryLongPress.gameId = gameId;
		libraryLongPress.game = game;
		return;
	}

	if (!libraryLongPress.opened && (ImGui::GetTime() - libraryLongPress.startTime >= GAME_INFO_LONG_PRESS_SECONDS))
	{
		libraryLongPress.opened = true;
		libraryLongPress.tracking = false;
		gui_set_library_game_info(game);
	}
#endif
}

static bool gameImageButton(ImguiTexture& texture, const std::string& tooltip, ImVec2 size,
		const std::string& gameName, float fallbackTitleSize = 0.0f)
{
	(void)tooltip;
	bool pressed = texture.button("##imagebutton", size, gameName, ImVec4(0, 0, 0, 0), ImVec4(1, 1, 1, 1),
			fallbackTitleSize);

    return pressed;
}

static double getLibraryIconAnimationClock()
{
	static double animationStart = 0.0;
	static bool libraryWasOpen = false;
	const bool libraryOpen = gui_state == GuiState::Main || gui_state == GuiState::SelectDisk;

	if (libraryOpen && !libraryWasOpen)
		animationStart = ImGui::GetTime();
	libraryWasOpen = libraryOpen;
	return std::max(0.0, ImGui::GetTime() - animationStart);
}

static GameBoxart getLibraryDisplayArtwork(const GameMedia& game, double animationClock)
{
	GameBoxart art;
	if (game.device)
		return art;

	art = boxart.getBoxartAndLoad(game);
	const std::string localCoverOverride = boxart.getLibraryCoverMediaPath(game);
	if (!localCoverOverride.empty())
		art.boxartPath = localCoverOverride;
	const auto source = static_cast<config::LibraryImageSourceMode>(config::LibraryImageSource.get());
	switch (source)
	{
	case config::LibraryImageSourceMode::CurrentArtwork:
		return art;

	case config::LibraryImageSourceMode::VmuSaveIcon:
	case config::LibraryImageSourceMode::VmuThenCurrentArtwork: {
		const bool animate = config::VmuIconMode.get() == static_cast<int>(config::VmuIconPlaybackMode::Active);
		const std::string vmuIconPath = getCachedVmuIconPath(game, art.uniqueId, animate, animationClock);
		if (!vmuIconPath.empty())
			art.boxartPath = vmuIconPath;
		return art;
	}

	case config::LibraryImageSourceMode::CurrentArtworkThenVmu:
		if (art.boxartPath.empty())
		{
			const bool animate = config::VmuIconMode.get() == static_cast<int>(config::VmuIconPlaybackMode::Active);
			const std::string vmuIconPath = getCachedVmuIconPath(game, art.uniqueId, animate, animationClock);
			if (!vmuIconPath.empty())
				art.boxartPath = vmuIconPath;
		}
		return art;
	}

	return art;
}

static std::string formatLibraryRegion(u32 region)
{
	if (region == 0)
		return "Unknown";

	std::string value;
	if (region & GameBoxart::JAPAN)
		value += "JP";
	if (region & GameBoxart::USA)
	{
		if (!value.empty())
			value += "/";
		value += "US";
	}
	if (region & GameBoxart::EUROPE)
	{
		if (!value.empty())
			value += "/";
		value += "EU";
	}
	return value.empty() ? "Unknown" : value;
}

static std::string formatLibrarySize(size_t size)
{
	if (size == 0)
		return "";

	constexpr size_t KiB = 1024;
	constexpr size_t MiB = KiB * 1024;
	if (size < MiB)
		return std::to_string((size + KiB - 1) / KiB) + " KB";
	return std::to_string((size + MiB - 1) / MiB) + " MB";
}

static std::string formatLibraryPlaytime(u64 seconds)
{
	constexpr u64 secondsPerMinute = 60;
	constexpr u64 secondsPerHour = secondsPerMinute * 60;
	constexpr u64 secondsPerDay = secondsPerHour * 24;
	const auto value = static_cast<unsigned long long>(seconds);
	if (seconds >= secondsPerDay)
		return strprintf(T("%llud %lluh"), value / secondsPerDay, (seconds % secondsPerDay) / secondsPerHour);
	if (seconds >= secondsPerHour)
		return strprintf(T("%lluh %llum"), value / secondsPerHour, (seconds % secondsPerHour) / secondsPerMinute);
	if (seconds >= secondsPerMinute)
		return strprintf(T("%llum"), value / secondsPerMinute);
	return strprintf(T("%llus"), value);
}

static void centerTableCellCursor(const ImVec2& contentSize, float rowContentHeight, bool centerX)
{
	ImVec2 pos = ImGui::GetCursorScreenPos();
	pos.y += std::max(0.0f, (rowContentHeight - contentSize.y) * 0.5f);
	if (centerX)
		pos.x += std::max(0.0f, (ImGui::GetContentRegionAvail().x - contentSize.x) * 0.5f);
	ImGui::SetCursorScreenPos(pos);
}

static void textTableCellCentered(const std::string& text, float rowContentHeight, bool centerX = true)
{
	if (text.empty())
		return;
	centerTableCellCursor(ImGui::CalcTextSize(text.c_str()), rowContentHeight, centerX);
	ImGui::TextUnformatted(text.c_str());
}

#ifdef TARGET_UWP
void gui_load_game()
{
	using namespace Windows::Storage;
	using namespace Concurrency;

	auto picker = ref new Pickers::FileOpenPicker();
	picker->ViewMode = Pickers::PickerViewMode::List;

	picker->FileTypeFilter->Append(".chd");
	picker->FileTypeFilter->Append(".gdi");
	picker->FileTypeFilter->Append(".cue");
	picker->FileTypeFilter->Append(".cdi");
	picker->FileTypeFilter->Append(".zip");
	picker->FileTypeFilter->Append(".7z");
	picker->FileTypeFilter->Append(".elf");
	if (!config::HideLegacyNaomiRoms)
	{
		picker->FileTypeFilter->Append(".bin");
		picker->FileTypeFilter->Append(".lst");
		picker->FileTypeFilter->Append(".dat");
	}
	picker->SuggestedStartLocation = Pickers::PickerLocationId::DocumentsLibrary;

	create_task(picker->PickSingleFileAsync()).then([](StorageFile ^file) {
		if (file)
		{
			NOTICE_LOG(COMMON, "Picked file: %S", file->Path->Data());
			nowide::stackstring path;
			if (path.convert(file->Path->Data()))
				gui_start_game(path.get());
		}
	});
}
#endif

static void gui_display_content()
{
#if defined(__ANDROID__)
	touchedLibraryItemThisFrame = false;
#endif
	fullScreenWindow(false);
	ImguiStyleVar _(ImGuiStyleVar_WindowRounding, 0);
	ImguiStyleVar _1(ImGuiStyleVar_WindowBorderSize, 0);

    ImGui::Begin("##main", nullptr, ImGuiWindowFlags_NoDecoration);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(20, 8));
	ImGui::PushFont(largeFont, 18.5f);
    ImGui::AlignTextToFramePadding();
    // Position "GAMES" text and search bar below the menu bar (window is already positioned below menu bar)
    ImGui::SetCursorPosY(ImGui::GetStyle().FramePadding.y);
    ImGui::Indent(10);
    ImGui::Text("%s", T("GAMES"));
    ImGui::Unindent(10);

    static ImGuiTextFilter filter;
	int libraryIconScale = std::clamp(config::LibraryIconScale.get(), 100, 1000);
	static bool libraryHoverSettingsInitialized = false;
	static int lastLibraryIconScale = libraryIconScale;
	static int lastLibraryDisplayStyle = config::LibraryDisplayStyle.get();
	static int lastLibraryImageSource = config::LibraryImageSource.get();
	static int lastLibraryCoverMedia = config::LibraryCoverMedia.get();
	static bool lastBoxartDisplayMode = config::BoxartDisplayMode.get();
	const int currentLibraryDisplayStyle = config::LibraryDisplayStyle.get();
	const int currentLibraryImageSource = config::LibraryImageSource.get();
	const int currentLibraryCoverMedia = config::LibraryCoverMedia.get();
	const bool currentBoxartDisplayMode = config::BoxartDisplayMode.get();
	if (!libraryHoverSettingsInitialized)
	{
		libraryHoverSettingsInitialized = true;
		lastLibraryIconScale = libraryIconScale;
		lastLibraryDisplayStyle = currentLibraryDisplayStyle;
		lastLibraryImageSource = currentLibraryImageSource;
		lastLibraryCoverMedia = currentLibraryCoverMedia;
		lastBoxartDisplayMode = currentBoxartDisplayMode;
	}
	else if (lastLibraryIconScale != libraryIconScale
			|| lastLibraryDisplayStyle != currentLibraryDisplayStyle
			|| lastLibraryImageSource != currentLibraryImageSource
			|| lastLibraryCoverMedia != currentLibraryCoverMedia
			|| lastBoxartDisplayMode != currentBoxartDisplayMode)
	{
		resetLibraryGameInfoHover();
		lastLibraryIconScale = libraryIconScale;
		lastLibraryDisplayStyle = currentLibraryDisplayStyle;
		lastLibraryImageSource = currentLibraryImageSource;
		lastLibraryCoverMedia = currentLibraryCoverMedia;
		lastBoxartDisplayMode = currentBoxartDisplayMode;
	}
    IconButton settingsBtn(ICON_FA_GEAR, T("Settings"));
#if !defined(__ANDROID__) && !defined(TARGET_IPHONE) && !defined(TARGET_UWP) && !defined(__SWITCH__)
	const float iconScaleSliderWidth = 135.0f;
	const float iconScaleControlWidth = iconScaleSliderWidth + ImGui::GetStyle().ItemInnerSpacing.x
			+ ImGui::CalcTextSize("Icon Size").x;
	const float settingsLeft = ImGui::GetContentRegionMax().x - settingsBtn.width();
	const float sliderLeft = settingsLeft - 24.0f - iconScaleControlWidth;
	ImGui::SameLine(0, 32);
	const float availableFilterWidth = sliderLeft - ImGui::GetCursorPosX()
			- ImGui::GetStyle().ItemSpacing.x - ImGui::CalcTextSize(T("Filter")).x;
	const float maxFilterWidth = std::max(80.0f, std::min(availableFilterWidth, 520.0f));
	const float filterWidth = std::clamp(availableFilterWidth * 0.5f, 80.0f, maxFilterWidth);
	filter.Draw(T("Filter"), filterWidth);
	ImGui::SameLine(0, 24.0f);
	ImGui::SetNextItemWidth(iconScaleSliderWidth);
	if (ImGui::SliderInt("Icon Size", &libraryIconScale, 100, 1000, "%d%%"))
		config::LibraryIconScale.set(libraryIconScale);
#endif
    if (gui_state != GuiState::SelectDisk)
    {
#ifdef TARGET_UWP
		ImGui::SameLine(ImGui::GetContentRegionMax().x - settingsBtn.width()
				- ImGui::GetStyle().FramePadding.x * 2.0f  - ImGui::GetStyle().ItemSpacing.x - ImGui::CalcTextSize(T("Load...")).x);
		if (ImGui::Button(T("Load...")))
			gui_load_game();
		ImGui::SameLine();
#elif defined(__SWITCH__)
		IconButton exitBtn(ICON_FA_POWER_OFF, T("Exit"));
		ImGui::SameLine(ImGui::GetContentRegionMax().x - settingsBtn.width()
				- ImGui::GetStyle().ItemSpacing.x - exitBtn.width());
		if (exitBtn.realize())
			dc_exit();
		ImGui::SameLine();
#else
		ImGui::SameLine(ImGui::GetContentRegionMax().x - settingsBtn.width());
#endif
			if (settingsBtn.realize())
			{
				gui_setState(GuiState::Settings);
			}
    }
    else
    {
    	IconButton cancelBtn(T("Cancel"));
		ImGui::SameLine(ImGui::GetContentRegionMax().x - cancelBtn.width());
		if (cancelBtn.realize())
			gui_setState(GuiState::Commands);
    }
	ImGui::PopFont();
    ImGui::PopStyleVar();

    boxart.refreshCustomBoxartIndex(false);
    scanner.fetch_game_list();

	// Only if Filter and Settings aren't focused... ImGui::SetNextWindowFocus();
	ImGui::BeginChild(ImGui::GetID("library"), ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_DragScrolling);
    {
		const bool useListStyle = config::LibraryDisplayStyle.get() == static_cast<int>(config::LibraryDisplayStyleMode::List);
		const float totalWidth = ImGui::GetContentRegionMax().x - (!ImGui::GetCurrentWindow()->ScrollbarY ? ImGui::GetStyle().ScrollbarSize : 0);
		const float libraryIconScaleFactor = libraryIconScale / 100.0f;
		const float libraryTextScaleFactor = 1.5f + (libraryIconScaleFactor - 1.0f) / 3.0f;
		const ImVec2 iconSize(32.0f * libraryIconScaleFactor, 32.0f * libraryIconScaleFactor);
		const double iconAnimationClock = getLibraryIconAnimationClock();

		if (useListStyle)
			ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(5, 4));
		else if (config::BoxartDisplayMode)
			ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0.5f));
		else
			ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 20));
		const float tableTextSize = std::clamp(18.5f * libraryTextScaleFactor, 18.5f, 46.0f);
		const float tableRowContentHeight = std::max(iconSize.y, tableTextSize);
		const float tableRowHeight = tableRowContentHeight + ImGui::GetStyle().CellPadding.y * 2.0f;
		auto calcLibraryTextWidth = [&](const char *text) {
			return largeFont != nullptr ? largeFont->CalcTextSizeA(tableTextSize, FLT_MAX, -1.0f, text).x
					: ImGui::CalcTextSize(text).x;
		};
		const float iconColumnWidth = std::max(iconSize.x, calcLibraryTextWidth("Icon"))
				+ ImGui::GetStyle().CellPadding.x * 2.0f;
		const float lastBootedColumnWidth = calcLibraryTextWidth("12/31/2026 12:59:59 PM")
				+ ImGui::GetStyle().CellPadding.x * 2.0f;
		const float timePlayedColumnWidth = std::max(calcLibraryTextWidth(T("Time Played")), calcLibraryTextWidth("999h 59m"))
				+ ImGui::GetStyle().CellPadding.x * 2.0f;

		int counter = 0;
		bool gameListEmpty = false;
		{
			scanner.get_mutex().lock();
			gameListEmpty = scanner.get_game_list().empty();
			if (useListStyle)
			{
				ImGui::PushFont(largeFont, tableTextSize);
				if (ImGui::BeginTable("libraryTable", 8, ImGuiTableFlags_NoSavedSettings | ImGuiTableFlags_Borders
						| ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit
						| ImGuiTableFlags_ScrollY, ImVec2(0.0f, 0.0f)))
				{
					ImGui::TableSetupColumn("Icon", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, iconColumnWidth);
					ImGui::TableSetupColumn("Product ID", ImGuiTableColumnFlags_WidthFixed, 96.0f);
					ImGui::TableSetupColumn("Title", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthFixed, 78.0f);
					ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthFixed, 72.0f);
					ImGui::TableSetupColumn(T("Time Played"), ImGuiTableColumnFlags_WidthFixed, timePlayedColumnWidth);
					ImGui::TableSetupColumn("Last Booted", ImGuiTableColumnFlags_WidthFixed, lastBootedColumnWidth);
					ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 78.0f);
					ImGui::TableSetColumnWidth(0, iconColumnWidth);
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableHeadersRow();

					const auto& gameList = scanner.get_game_list();
					auto drawTableGame = [&](const GameMedia& game, int rowIndex) -> bool
					{
						if (gui_state == GuiState::SelectDisk)
						{
							std::string extension = get_file_extension(game.path);
							if (!game.device && extension != "gdi" && extension != "chd"
									&& extension != "cdi" && extension != "cue")
								return false;
							if (game.path.empty())
								return false;
						}

						std::string gameName = game.name;
						bool passFilter = filter.PassFilter(gameName.c_str());
						GameBoxart art;
						if (!game.device)
						{
							art = getLibraryDisplayArtwork(game, iconAnimationClock);
							if (!art.name.empty())
								gameName = art.name;
							passFilter = passFilter || filter.PassFilter(gameName.c_str());
						}

						if (!passFilter)
							return false;

						std::string productId;
						std::string region = "Unknown";
						if (!art.uniqueId.empty())
							productId = art.uniqueId;
						if (art.region != 0)
							region = formatLibraryRegion(art.region);

						std::string format = "Unknown";
						if (game.path.empty())
							format = "BIOS";
						else if (game.device)
							format = "Device";
						else
						{
							const std::string extension = get_file_extension(game.path);
							if (!extension.empty())
								format = extension;
						}

						ImguiID _(game.path.empty() ? "bios" : (game.path + "_row"));
						ImGui::TableNextRow(ImGuiTableRowFlags_None, tableRowHeight);
						ImGui::TableSetColumnIndex(0);
						const ImVec2 iconCellPos = ImGui::GetCursorScreenPos();
						const bool rowPressed = ImGui::Selectable(("##row_" + std::to_string(rowIndex)).c_str(),
								false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, tableRowContentHeight));
						ImGui::SetCursorScreenPos(iconCellPos);
						if (!game.device && !art.boxartPath.empty())
						{
							ImguiFileTexture tex(art.boxartPath);
							centerTableCellCursor(iconSize, tableRowContentHeight, true);
							tex.draw(iconSize);
						}

						ImGui::TableSetColumnIndex(1);
						textTableCellCentered(productId, tableRowContentHeight);
						ImGui::TableSetColumnIndex(2);
						textTableCellCentered(gameName, tableRowContentHeight, false);
						ImGui::TableSetColumnIndex(3);
						textTableCellCentered(region, tableRowContentHeight);
						ImGui::TableSetColumnIndex(4);
						textTableCellCentered(format, tableRowContentHeight);
						ImGui::TableSetColumnIndex(5);
						const std::string timePlayed = art.playTimeSeconds.has_value()
								? formatLibraryPlaytime(*art.playTimeSeconds) : std::string();
						textTableCellCentered(timePlayed, tableRowContentHeight);
						ImGui::TableSetColumnIndex(6);
						const time_t lastBootedTime = getLibraryGameLastBooted(game, art.uniqueId);
						const std::string lastBooted = lastBootedTime == 0 ? std::string() : formatShortDateTime(lastBootedTime);
						textTableCellCentered(lastBooted, tableRowContentHeight);
						ImGui::TableSetColumnIndex(7);
						const std::string size = formatLibrarySize(game.size);
						textTableCellCentered(size, tableRowContentHeight);

						if (rowPressed)
						{
							settings.content.title = art.name;
							if (settings.content.title.empty() || settings.content.title == game.fileName)
								settings.content.title = get_file_basename(game.fileName);
							if (gui_state == GuiState::SelectDisk)
							{
								try {
									emu.insertGdrom(game.path);
									gui_setState(GuiState::Closed);
								} catch (const FlycastException& e) {
									gui_error(e.what());
								}
							}
							else
							{
								std::string gamePath(game.path);
								scanner.get_mutex().unlock();
								gui_start_game(gamePath);
								scanner.get_mutex().lock();
								return true;
							}
						}
						return false;
					};

					if (!filter.IsActive() && gui_state != GuiState::SelectDisk)
					{
						ImGuiListClipper clipper;
						clipper.Begin(static_cast<int>(gameList.size()), tableRowHeight);
						bool gameStartedFromRow = false;
						while (clipper.Step() && !gameStartedFromRow)
							for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
								if (drawTableGame(gameList[i], i))
								{
									gameStartedFromRow = true;
									break;
								}
					}
					else
					{
						for (int i = 0; i < static_cast<int>(gameList.size()); i++)
							if (drawTableGame(gameList[i], i))
								break;
					}
					ImGui::EndTable();
				}
				ImGui::PopFont();
			}
			else
			{
				const float gridBoxBaseSize = 112.0f * libraryIconScaleFactor;
				const int itemsPerLine = std::max<int>(totalWidth / (gridBoxBaseSize + ImGui::GetStyle().ItemSpacing.x), 1);
				const float responsiveBoxSize = totalWidth / itemsPerLine - ImGui::GetStyle().FramePadding.x * 2;
				const ImVec2 responsiveBoxVec2 = ImVec2(responsiveBoxSize, responsiveBoxSize);
				const float gridTextSize = std::clamp(13.0f * libraryTextScaleFactor, 13.0f, 38.0f);

				for (const auto& game : scanner.get_game_list())
				{
					if (gui_state == GuiState::SelectDisk)
					{
						std::string extension = get_file_extension(game.path);
						if (!game.device && extension != "gdi" && extension != "chd"
								&& extension != "cdi" && extension != "cue")
							// Only dreamcast disks
							continue;
						if (game.path.empty())
							// Dreamcast BIOS isn't a disk
							continue;
					}
					std::string gameName = game.name;
					bool passFilter = filter.PassFilter(gameName.c_str());
					GameBoxart art;
					if (config::BoxartDisplayMode && !game.device)
					{
						art = getLibraryDisplayArtwork(game, iconAnimationClock);
						gameName = art.name;
						passFilter = passFilter || filter.PassFilter(gameName.c_str());
					}
					if (passFilter)
					{
						ImguiID _(game.path.empty() ? "bios" : game.path);
						bool pressed = false;
						if (config::BoxartDisplayMode)
						{
							if (counter % itemsPerLine != 0)
								ImGui::SameLine();
							counter++;
							// Put the image inside a child window so we can detect when it's fully clipped and doesn't need to be loaded
							if (ImGui::BeginChild("img", ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened))
							{
								ImguiFileTexture tex(art.boxartPath);
								pressed = gameImageButton(tex, game.name, responsiveBoxVec2, gameName, gridTextSize);
								updateLibraryLongPress(game, game.path.empty() ? "bios" : game.path, ImGui::IsItemActive());
								draw_library_game_info_hover(game, !game.device ? &art : nullptr);
							}
							ImGui::EndChild();
						}
						else
						{
							ImGui::PushFont(largeFont, gridTextSize);
							pressed = ImGui::Selectable(gameName.c_str());
							ImGui::PopFont();
							updateLibraryLongPress(game, game.path.empty() ? "bios" : game.path, ImGui::IsItemActive());
							draw_library_game_info_hover(game);
						}
						if (pressed)
						{
							if (!config::BoxartDisplayMode)
								art = boxart.getBoxart(game);
							settings.content.title = art.name;
							if (settings.content.title.empty() || settings.content.title == game.fileName)
								settings.content.title = get_file_basename(game.fileName);
							if (gui_state == GuiState::SelectDisk)
							{
								try {
									emu.insertGdrom(game.path);
									gui_setState(GuiState::Closed);
								} catch (const FlycastException& e) {
									gui_error(e.what());
								}
							}
							else
							{
								std::string gamePath(game.path);
								scanner.get_mutex().unlock();
								gui_start_game(gamePath);
								scanner.get_mutex().lock();
								break;
							}
						}
					}
				}
			}
			scanner.get_mutex().unlock();
		}

#if defined(__ANDROID__)
		if (gui_state == GuiState::Main && (!ImGui::GetIO().MouseDown[ImGuiMouseButton_Left] || !touchedLibraryItemThisFrame))
			resetLibraryLongPress();
#endif
		bool addContent = false;
#if !defined(TARGET_IPHONE)
		if (gameListEmpty && gui_state != GuiState::SelectDisk)
		{
			const char *label = T("Your game list is empty");
			// center horizontally
			const float w = ImGui::GetFont()->CalcTextSizeA(uiLargeFontSize(), FLT_MAX, -1.f, label).x + ImGui::GetStyle().FramePadding.x * 2;
			ImGui::SameLine((ImGui::GetContentRegionMax().x - w) / 2);
			if (ImGui::BeginChild("empty", ImVec2(0, 0), ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_NavFlattened))
			{
				ImGui::PushFont(nullptr, uiLargeFontSize());
				ImGui::NewLine();
				ImGui::Text("%s", label);
				ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(20, 8));
				addContent = ImGui::Button(T("Add Game Folder"));
				ImGui::PopFont();
			}
			ImGui::EndChild();
		}
#endif
        ImGui::PopStyleVar();
        addContentPath(addContent);
    }
    scrollWhenDraggingOnVoid();
    windowDragScroll();
	ImGui::EndChild();
	ImGui::End();

    contentpath_warning_popup();
}

static bool systemdir_selected_callback(bool cancelled, std::string selection)
{
	if (cancelled)
	{
		gui_setState(GuiState::Main);
		return true;
	}
	selection += "/";

	std::string data_path = selection + "data/";
	if (!file_exists(data_path))
	{
		if (!make_directory(data_path))
		{
			WARN_LOG(BOOT, "Cannot create 'data' directory: %s", data_path.c_str());
			gui_error(Ts("Invalid selection:") + '\n' + Ts("Hollycast cannot write to this folder."));
			return false;
		}
	}
	// We might be able to create a directory but not a file. Because ... android
	// So let's test to be sure.
	std::string testPath = data_path + "writetest.txt";
	FILE *file = fopen(testPath.c_str(), "w");
	if (file == nullptr)
	{
		WARN_LOG(BOOT, "Cannot write in the 'data' directory");
		gui_error(Ts("Invalid selection:") + '\n' + Ts("Hollycast cannot write to this folder."));
		return false;
	}
	fclose(file);
	unlink(testPath.c_str());

	set_user_config_dir(selection);
	add_system_data_dir(selection);
	set_user_data_dir(data_path);

	if (config::open())
	{
		config::Settings::instance().load(false);
		// Make sure the renderer type doesn't change mid-flight
		config::RendererType = RenderType::OpenGL;
		gui_setState(GuiState::Main);
		if (config::ContentPath.get().empty())
		{
			scanner.stop();
			config::ContentPath.get().push_back(selection);
		}
		SaveSettings();
	}
	return true;
}

static void gui_display_onboarding()
{
	const char *title = T("Select Hollycast Home Folder");
	ImGui::OpenPopup(title);
	select_file_popup(title, &systemdir_selected_callback);
}

static void drawBoxartBackground()
{
	GameMedia game;
	game.path = settings.content.path;
	game.fileName = settings.content.fileName;
	GameBoxart art = boxart.getBoxart(game);
	ImguiFileTexture tex(art.boxartPath);
	ImDrawList *dl = ImGui::GetBackgroundDrawList();
	tex.draw(dl, ImVec2(0, 0), ImVec2(settings.display.width, settings.display.height), 1.f);
}

static std::future<bool> networkStatus;

static void gui_network_start()
{
	drawBoxartBackground();
	centerNextWindow();
	ImGui::SetNextWindowSize(ScaledVec2(360, 0));
	ImGui::SetNextWindowBgAlpha(0.8f);
	ImguiStyleVar _1(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));

	if (ImGui::Begin("##network", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(20, 10));
		ImGui::AlignTextToFramePadding();
		ImGui::SetCursorPosX(uiScaled(20.f));

		if (networkStatus.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
		{
			ImGui::Text("%s", T("Starting..."));
			try {
				if (networkStatus.get())
					gui_resume_game();
				else
					gui_stop_game();
			} catch (const FlycastException& e) {
				gui_stop_game(e.what());
			}
		}
		else
		{
			ImGui::Text("%s", T("Starting Network..."));
			if (NetworkHandshake::instance->canStartNow())
				ImGui::TextWrapped("%s", T("Press Start to start the game now."));
		}
		ImGui::Text("%s", get_notification().c_str());

		float currentwidth = ImGui::GetContentRegionAvail().x;
		float buttonWidth = ImGui::CalcTextSize(T("Cancel")).x + ImGui::GetStyle().FramePadding.x * 2;
		if (NetworkHandshake::instance != nullptr && NetworkHandshake::instance->canStartNow() && gui_state != GuiState::Closed)
		{
			float startWidth = ImGui::CalcTextSize(T("Start Now")).x + ImGui::GetStyle().FramePadding.x * 2;
			buttonWidth = std::max(buttonWidth, startWidth);
			ImGui::SetCursorPosX((currentwidth - buttonWidth * 2 - ImGui::GetStyle().ItemSpacing.x) / 2.f + ImGui::GetStyle().WindowPadding.x);
			if (ImGui::Button(T("Start Now"), ScaledVec2(buttonWidth, 0)) && NetworkHandshake::instance != nullptr)
				NetworkHandshake::instance->startNow();
			ImGui::SameLine();
		}
		else {
			ImGui::SetCursorPosX((currentwidth - buttonWidth) / 2.f + ImGui::GetStyle().WindowPadding.x);
		}
		if (ImGui::Button(T("Cancel"), ScaledVec2(buttonWidth, 0)) && NetworkHandshake::instance != nullptr)
		{
			NetworkHandshake::instance->stop();
			try {
				networkStatus.get();
			}
			catch (const FlycastException&) {
			}
			gui_stop_game();
		}
	}
	ImGui::End();

	if ((kcode[0] & DC_BTN_START) == 0 && NetworkHandshake::instance != nullptr)
		NetworkHandshake::instance->startNow();
}

#ifdef TARGET_UWP
#include "oslib/http_client.h"

static bool checkUWPProtocolActivation()
{
	// Check for UWP protocol-activated ROM path
	static int checkCount = 90; // Try many times - OnAppActivated may not be called yet
	if (checkCount == 0)
		return false;
	checkCount--;
	char* activationUri = SDL_WinRTGetProtocolActivationURI();
	if (activationUri == nullptr)
		return false;

	std::string uri(activationUri);
	SDL_free(activationUri);
	INFO_LOG(BOOT, "Protocol activation URI: %s", uri.c_str());
	size_t qpos = uri.find('?');
	if (qpos != std::string::npos)
	{
		uri = uri.substr(qpos + 1);
		// Parse launchOnExit parameter
		size_t exitPos = uri.find("launchOnExit=");
		if (exitPos != std::string::npos) {
			exitPos += 13; // Skip "launchOnExit="
			size_t exitEnd = uri.find('&', exitPos);
			if (exitEnd == std::string::npos)
				exitEnd = uri.size();
			std::string exitUri = uri.substr(exitPos, exitEnd - exitPos);
			launchOnExitUri = http::urlDecode(exitUri);
			INFO_LOG(BOOT, "LaunchOnExit URI: %s", launchOnExitUri.c_str());
			// SDL WinRT will automatically handle launchOnExit from the protocol URI
		}

		uri = http::urlDecode(uri);

		// Parse ROM path (first quoted string)
		size_t s = uri.find('"');
		if (s != std::string::npos)
		{
			size_t e = uri.find('"', s + 1);
			if (e != std::string::npos)
			{
				std::string romPath = uri.substr(s + 1, e - (s + 1));
				commandLineStart = true;
				gui_start_game(romPath);
				return true;
			}
		}
	}
	return false;
}
#endif

static void gui_display_loadscreen()
{
	drawBoxartBackground();
	centerNextWindow();
	ImGui::SetNextWindowSize(ScaledVec2(330, 0));
	ImGui::SetNextWindowBgAlpha(0.8f);
	ImguiStyleVar _(ImGuiStyleVar_WindowPadding, ScaledVec2(20, 20));

    if (ImGui::Begin("##loading", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize))
    {
		ImguiStyleVar _(ImGuiStyleVar_FramePadding, ScaledVec2(20, 10));
		ImGui::AlignTextToFramePadding();
		ImGui::SetCursorPosX(uiScaled(20.f));
		try {
			const bool gameReady = gameLoader.ready();
			if (gameReady)
			{
				if (custom_texture.needsRefresh())
				{
					custom_texture.refresh();
				}
				else
					custom_texture.init();
			}
			const char *label = gameLoader.getProgress().label;
			if (label == nullptr)
			{
				if (gameReady)
					label = T("Starting...");
				else
					label = T("Loading...");
			}

			const bool customTexPreloading = custom_texture.isPreloading();
			if (gameReady && !customTexPreloading)
			{
				if (!game_started && NetworkHandshake::instance != nullptr)
				{
					networkStatus = NetworkHandshake::instance->start();
					gui_setState(GuiState::NetworkStart);
				}
				else
				{
					gui_request_initial_auto_load();
					ImGui::Text("%s", label);
				}
			}
			else
			{
				int texLoaded = 0;
				int texTotal = 0;
				size_t loaded_size_b = 0;
				custom_texture.getPreloadProgress(texLoaded, texTotal, loaded_size_b);

				ImGui::Text("%s", label);
				float progress = 0;
				char overlay[64] = "";
				
				if (!gameReady)
				{
					progress = gameLoader.getProgress().progress;
				}
				else if (customTexPreloading)
				{
					ImGui::Spacing();
					ImGui::Text("%s", T("Preloading custom textures"));
					progress = (texTotal == -1 || texTotal == 0) ? 0.f : (float)texLoaded / (float)texTotal;
					if (texTotal == -1)
						snprintf(overlay, sizeof(overlay), "%s", T("Preparing..."));
					else
					{
						float loaded_size_mb = (float)loaded_size_b / (1024 * 1024);
						snprintf(overlay, sizeof(overlay), "%d / %d (%.1f MB)", texLoaded, texTotal, loaded_size_mb);
					}
				}

				ImguiStyleColor _(ImGuiCol_PlotHistogram, ImVec4(0.557f, 0.268f, 0.965f, 1.f));
				ImGui::ProgressBar(progress, ImVec2(-1, uiScaled(20.f)), overlay);

				float currentwidth = ImGui::GetContentRegionAvail().x;
				ImGui::SetCursorPosX((currentwidth - uiScaled(100.f)) / 2.f + ImGui::GetStyle().WindowPadding.x);
				if (ImGui::Button(T("Cancel"), ScaledVec2(100.f, 0)))
					gameLoader.cancel();
			}
		} catch (const FlycastException& ex) {
			ERROR_LOG(BOOT, "%s", ex.what());
#ifdef TEST_AUTOMATION
			die("Game load failed");
#endif
			gui_stop_game(ex.what());
		}
    }
    ImGui::End();
}

void gui_display_ui()
{
	FC_PROFILE_SCOPE;
	const LockGuard lock(guiMutex);

	if (gui_state == GuiState::Closed)
		return;

	// Initialize ImGui frame BEFORE any early returns
	// This ensures the menu bar is always visible, even during auto-start
	gui_newFrame();
	ImGui::NewFrame();
	error_msg_shown = false;
	bool gui_open = gui_is_open();
	auto finishFrame = [&]() {
		ImGui::Render();
		gui_endFrame(gui_open);
		uiThreadRunner.execTasks(std::chrono::steady_clock::now());
		ImguiFileTexture::resetLoadCount();
	};

	// Render menu bar BEFORE any early returns.
	// Hide it for the full-screen GameInfo screen on Android.
	if (gui_state != GuiState::GameInfo)
		GuiMenu::renderMainMenuBar();

	// Check for auto-start after menu bar is rendered
	if (gui_state == GuiState::Main)
	{
#ifdef TARGET_UWP
		if (checkUWPProtocolActivation())
		{
			finishFrame();
			return;
		}
#endif
		if (!settings.content.path.empty() || settings.naomi.slave)
		{
#ifndef __ANDROID__
			commandLineStart = true;
#endif
			if (settings.content.path.substr(0, 7) == "dc_bios")
				gui_start_game("");
			else
				gui_start_game(settings.content.path);
			finishFrame();
			return;
		}
	}

	// Render modal dialogs BEFORE window management
	// This ensures modals are not affected by window stack operations
	render_modal_dialogs();

	switch (gui_state)
	{
	case GuiState::Settings:
		// Settings now always use the new UI through the compatibility facade.
		gui_display_settings();
		break;
	case GuiState::Commands:
		gui_display_commands();
		break;
	case GuiState::Pause:
		toast.draw();
		drawPauseIcon();
		break;
	case GuiState::Main:
		//gui_display_demo();
		gui_display_content();
		break;
	case GuiState::GameInfo:
		gui_display_game_info();
		break;
	case GuiState::Closed:
		break;
	case GuiState::Onboarding:
		gui_display_onboarding();
		break;
	case GuiState::VJoyEdit:
		vgamepad::draw();
		break;
	case GuiState::VJoyEditCommands:
		vgamepad::displayCommands();
		break;
	case GuiState::SelectDisk:
		gui_display_content();
		break;
	case GuiState::Loading:
		gui_display_loadscreen();
		rend_process_custom_texture_preloads();
		break;
	case GuiState::NetworkStart:
		gui_network_start();
		break;
	case GuiState::Cheats:
		gui_cheats();
		break;
	case GuiState::Achievements:
#ifdef USE_RACHIEVEMENTS
		achievements::achievementList();
		break;
#endif
	default:
		die("Unknown UI state");
		break;
	}

	finishFrame();

	if (gui_state == GuiState::Closed)
		emu.start();
}

static u64 LastFPSTime;
static int lastFrameCount = 0;
static float fps = -1;

static std::string getFPSNotification()
{
	if (config::ShowFPS)
	{
		u64 now = getTimeMs();
		if (now - LastFPSTime >= 1000) {
			fps = ((float)MainFrameCount - lastFrameCount) * 1000.f / (now - LastFPSTime);
			LastFPSTime = now;
			lastFrameCount = MainFrameCount;
		}
		if (fps >= 0.f && fps < 9999.f) {
			char text[32];
			snprintf(text, sizeof(text), "F:%4.1f%s", fps, settings.input.fastForwardMode ? " >>" : "");

			return std::string(text);
		}
	}
	return std::string(settings.input.fastForwardMode ? ">>" : "");
}

void gui_draw_osd()
{
	gui_newFrame();
	ImGui::NewFrame();

#ifdef USE_RACHIEVEMENTS
	if (!achievements::notifier.draw())
#endif
		if (!toast.draw())
		{
			std::string message = getFPSNotification();
			if (!message.empty())
			{
				const float maxW = uiScaled(640.f);
				ImDrawList *dl = ImGui::GetForegroundDrawList();
				const ScaledVec2 padding(5.f, 5.f);
				const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(uiLargeFontSize(), FLT_MAX, maxW, &message.front(), &message.back() + 1)
						+ padding * 2.f;
				ImVec2 pos(insetLeft, ImGui::GetIO().DisplaySize.y - size.y);
				constexpr float alpha = 0.7f;
				const ImU32 bg_col = alphaOverride(0x00202020, alpha / 2.f);
				dl->AddRectFilled(pos, pos + size, bg_col, 0.f);
				pos += padding;
				const ImU32 col = alphaOverride(0x0000FFFF, alpha);
				dl->AddText(nullptr, uiLargeFontSize(), pos, col, &message.front(), &message.back() + 1, maxW);
			}
		}

	if (ggpo::active())
	{
		if (config::NetworkStats)
			ggpo::displayStats();
		chat.display();
	}
	else if (config::NetworkStats) {
		ice::displayStats();
	}
	if (!settings.raHardcoreMode)
		lua::overlay();
	vgamepad::draw();
	// Render menu bar on top during gameplay
	GuiMenu::renderMainMenuBar();

	// Render modal dialogs (exit dialog, error popups)
	render_modal_dialogs();

	ImGui::Render();
	uiThreadRunner.execTasks(std::chrono::steady_clock::now());
}

void gui_display_osd() {
	gui_draw_osd();
	gui_endFrame(gui_is_open());
}

void gui_display_profiler()
{
#if FC_PROFILER
	gui_newFrame();
	ImGui::NewFrame();

	const std::string profilerWindowTitle = std::string(T("Profiler")) + "###Profiler";
	ImGui::Begin(profilerWindowTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBackground);

	{
		ImguiStyleColor _(ImGuiCol_Text, ImVec4(0.8f, 0.8f, 0.8f, 1.0f));

		std::unique_lock<std::recursive_mutex> lock(fc_profiler::ProfileThread::s_allThreadsLock);

		for(const fc_profiler::ProfileThread* profileThread : fc_profiler::ProfileThread::s_allThreads)
		{
			char text[256];
			std::snprintf(text, 256, "%.3f : Thread %s", (float)profileThread->cachedTime, profileThread->threadName.c_str());
			ImGui::TreeNode(text);

			ImGui::Indent();
			fc_profiler::drawGUI(profileThread->cachedResultTree);
			ImGui::Unindent();
		}
	}

	for (const fc_profiler::ProfileThread* profileThread : fc_profiler::ProfileThread::s_allThreads)
	{
		fc_profiler::drawGraph(*profileThread);
	}

	ImGui::End();
    ImGui::Render();
	gui_endFrame(true);
#endif
}

void gui_open_onboarding() {
	gui_setState(GuiState::Onboarding);
}

void gui_cancel_load() {
	gameLoader.cancel();
}

void gui_term()
{
	if (inited)
	{
		inited = false;
		SettingsNew::shutdownChdConversionWorker();
		scanner.stop();
		clearThumbnailCache();
		ImGui::DestroyContext();
	    EventManager::unlisten(Event::Resume, emuEventCallback);
	    EventManager::unlisten(Event::Start, emuEventCallback);
	    EventManager::unlisten(Event::Terminate, emuEventCallback);
	    clearVmuIconLookups();
	    boxart.term();
	}
}

void fatal_error(const char* text, ...)
{
    va_list args;

    char temp[2048];
    va_start(args, text);
    vsnprintf(temp, sizeof(temp), text, args);
    va_end(args);
    ERROR_LOG(COMMON, "%s", temp);

    os_notify(T("Fatal Error"), 20000, temp);
}

extern bool subfolders_read;

void gui_refresh_files() {
	scanner.refresh();
	subfolders_read = false;
}

void reset_vmus() {
	for (u32 i = 0; i < std::size(vmu_lcd_status); i++)
		vmu_lcd_status[i] = false;
}

void gui_error(const std::string& what) {
	error_msg = what;
}

void gui_loadState(int slot)
{
	const LockGuard lock(guiMutex);

	if (dc_savestateAllowed())
	{
		try {
			// Close UI if it's open (menu selection scenario)
			if (gui_state != GuiState::Closed)
			{
				gui_setState(GuiState::Closed);
			}

			emu.stop();
			dc_loadstate(slot);
			emu.start();
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
		}
	}
	else
	{
		// User feedback when load not allowed
		WARN_LOG(COMMON, "Load state not allowed: network=%d, multiboard=%d",
				 settings.network.online, settings.naomi.multiboard);
		os_notify(T("Cannot load state during online play"), 3000);
	}
}

void gui_loadState(bool inRam)
{
	gui_loadState(inRam ? -2 : config::SavestateSlot);
}

void gui_saveState(int slot, bool stopRestart)
{
	const LockGuard lock(guiMutex);
	if ((gui_state == GuiState::Closed || !stopRestart) && dc_savestateAllowed())
	{
		try {
			if (stopRestart)
				emu.stop();

			savestate(slot);

			if (stopRestart)
				emu.start();
		} catch (const FlycastException& e) {
			if (stopRestart)
				gui_stop_game(e.what());
			else
				WARN_LOG(COMMON, "gui_saveState: %s", e.what());
		}
	}
}

void gui_saveState(bool stopRestart, bool inRam)
{
	if (inRam)
	{
		const LockGuard lock(guiMutex);
		if ((gui_state == GuiState::Closed || !stopRestart) && dc_savestateAllowed())
		{
			try {
				if (stopRestart)
					emu.stop();
				dc_savestate(-2);
				if (stopRestart)
					emu.start();
			} catch (const FlycastException& e) {
				if (stopRestart)
					gui_stop_game(e.what());
				else
					WARN_LOG(COMMON, "gui_saveState: %s", e.what());
			}
		}
		return;
	}
	gui_saveState(config::SavestateSlot, stopRestart);
}

void gui_cycleSaveStateSlot(int step)
{
	config::SavestateSlot = (config::SavestateSlot + (step % NUM_SAVE_SLOTS) + NUM_SAVE_SLOTS) % NUM_SAVE_SLOTS;
	SaveSettings();
	os_notify(strprintf(T("Save state slot %d"), config::SavestateSlot + 1).c_str(), 2000);
}

void gui_togglePause()
{
	const LockGuard lock(guiMutex);
	if (settings.network.online || settings.naomi.multiboard)
		return;

	try {
		if (gui_state == GuiState::Closed)
		{
			if (!achievements::canPause())
				return;
			vgamepad::hide();
			emu.stop();
			gui_setState(GuiState::Pause);
		}
		else if (gui_state == GuiState::Pause)
		{
			GamepadDevice::load_system_mappings();
			if (gui_resume_game())
				emu.start();
		}
	} catch (const FlycastException& e) {
		gui_stop_game(e.what());
	}
}

void gui_setState(GuiState newState)
{
	if (gui_state != newState)
	{
		if (newState == GuiState::Main && gui_state == GuiState::Closed)
			clearVmuIconLookups();
		resetLibraryLongPress();
		resetLibraryGameInfoHover();
	}
	gui_state = newState;
	if (newState == GuiState::Closed)
	{
		clearThumbnailCache();
		// If the game isn't rendering any frame, these flags won't be updated and keyboard/mouse input will be ignored.
		// So we force them false here. They will be set in the next ImGUI::NewFrame() anyway
		ImGuiIO& io = ImGui::GetIO();
		io.WantCaptureKeyboard = false;
		io.WantCaptureMouse = false;
	}
}

std::string gui_getCurGameBoxartUrl()
{
	GameMedia game;
	game.fileName = settings.content.fileName;
	game.path = settings.content.path;
	GameBoxart art = boxart.getBoxart(game);
	return art.boxartUrl;
}

void gui_refresh_custom_boxart(bool force)
{
	boxart.refreshLibraryPlaytimeDatabase();
	boxart.refreshCustomBoxartIndex(force);
}

void gui_refresh_boxart_cache()
{
	clearVmuIconLookups();
	boxart.refreshCache();
	scanner.fetch_game_list_sync();
	std::vector<GameMedia> games;
	{
		std::lock_guard<std::mutex> guard(scanner.get_mutex());
		games = scanner.get_game_list();
	}
	for (const GameMedia& game : games) {
		if (!game.device)
			boxart.queueBoxart(game);
	}
	boxart.startFetch();
}

void gui_runOnUiThread(std::function<void()> function) {
	uiThreadRunner.runOnThread(function);
}

void gui_runOnUiThread(const std::chrono::steady_clock::time_point& tp, const std::function<void()>& function) {
	uiThreadRunner.runOnThread(tp, function);
}

void gui_takeScreenshot()
{
	if (!game_started)
		return;
	gui_runOnUiThread([]() {
		std::string date = timeToISO8601(time(nullptr));
		std::replace(date.begin(), date.end(), '/', '-');
		std::replace(date.begin(), date.end(), ':', '-');
		std::string name = "Hollycast-" + date + ".png";

		std::vector<u8> data;
		getScreenshot(data);
		if (data.empty()) {
			os_notify(T("No screenshot available"), 2000);
		}
		else
		{
			try {
				hostfs::saveScreenshot(name, data);
				os_notify(T("Screenshot saved"), 2000, name.c_str());
			} catch (const FlycastException& e) {
				os_notify(T("Error saving screenshot"), 5000, e.what());
			}
		}
	});
}

// Cache for save state thumbnails
struct ThumbnailEntry {
    ImTextureID id;
};
static std::map<int, ThumbnailEntry> thumbnailCache;


static ImTextureID loadSaveStateThumbnail(int slot)
{
	const int key = slot;
	// Check cache first
	auto cached = thumbnailCache.find(key);
	if (cached != thumbnailCache.end())
		return cached->second.id;

	// Load screenshot from save state
	std::vector<u8> pngData;
	dc_getStateScreenshot(slot, pngData);
	if (pngData.empty())
		return ImTextureID_Invalid;

	// Decode PNG using stb_image
	int width, height, channels;
	stbi_set_flip_vertically_on_load(0);
	u8* imgData = stbi_load_from_memory(
		pngData.data(), pngData.size(),
		&width, &height, &channels, 4);

	if (!imgData)
		return ImTextureID_Invalid;

	// Create texture using imguiDriver abstraction (works with all renderers)
	std::string texName = "savestate_" + std::to_string(key);
	ImTextureID textureId{};
	try {
		textureId = imguiDriver->updateTextureAndAspectRatio(texName, imgData, width, height, false);
	} catch (...) {
		// Renderer might throw during resize
		free(imgData);
		return ImTextureID_Invalid;
	}

	free(imgData);

	// Cache texture
	thumbnailCache[key] = {textureId};

	return textureId;
}

static void draw_save_state_menu_thumbnail(int slot, float size)
{
	ImTextureID thumbnail = loadSaveStateThumbnail(slot);
	if (thumbnail != ImTextureID_Invalid)
		ImGui::Image(thumbnail, ImVec2(size, size));
	else
		ImGui::Dummy(ImVec2(size, size));
}

static void clearThumbnailCache()
{
	// Delete textures before clearing cache
	for (auto& entry : thumbnailCache)
	{
		std::string texName = "savestate_" + std::to_string(entry.first);
		try {
			imguiDriver->deleteTexture(texName);
		} catch (...) {
			// Ignore errors during shutdown/cleanup
		}
	}

	thumbnailCache.clear();
}

static std::string format_save_state_menu_time(time_t timestamp)
{
	if (timestamp <= 0)
		return T("Unknown");

	struct tm tmInfo {};
#ifdef _WIN32
	if (localtime_s(&tmInfo, &timestamp) != 0)
		return T("Unknown");
#else
	if (localtime_r(&timestamp, &tmInfo) == nullptr)
		return T("Unknown");
#endif

	char timeStr[64];
	if (strftime(timeStr, sizeof(timeStr), "%m/%d/%Y %I:%M %p", &tmInfo) == 0)
		return T("Unknown");

	return timeStr;
}

void render_save_state_slots(bool isSaving)
{
    const float thumbnailSize = uiScaled(18.0f);
    bool hasEntries = false;

    for (int slot = 0; slot < NUM_SAVE_SLOTS; slot++)
    {
        const time_t timestamp = dc_getStateCreationDate(slot);
        const bool isEmpty = (timestamp <= 0);

        // If loading, we only care about occupied slots
        if (!isSaving && isEmpty) {
            continue;
        }

        hasEntries = true;

        std::string dateStr = isEmpty ? T("Empty") : format_save_state_menu_time(timestamp);
        std::string label = isSaving
                          ? strprintf(T("Save Slot %d (%s)"), slot + 1, dateStr.c_str())
                          : strprintf(T("Load Slot %d (%s)"), slot + 1, dateStr.c_str());

        draw_save_state_menu_thumbnail(slot, thumbnailSize);
        ImGui::SameLine(0, uiScaled(6.0f));

        if (ImGui::MenuItem(label.c_str())) {
            config::SavestateSlot = slot;
            if (isSaving) {
                gui_saveState();
            } else {
                gui_loadState();
            }
        }
    }

    if (!isSaving && !hasEntries) {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "%s", T("No Save States"));
    }
}

#ifdef TARGET_UWP
// Ugly but a good workaround for MS stupidity
// UWP doesn't allow the UI thread to wait on a thread/task. When an std::future is ready, it is possible
// that the task has not yet completed. Calling std::future::get() at this point will throw an exception
// AND destroy the std::future at the same time, rendering it invalid and discarding the future result.
bool __cdecl Concurrency::details::_Task_impl_base::_IsNonBlockingThread() {
	return false;
}
#endif
