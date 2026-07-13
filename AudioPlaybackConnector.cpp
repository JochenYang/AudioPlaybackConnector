#include "pch.h"
#include "AudioPlaybackConnector.h"

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void SetupFlyout();
void SetupMenu();
winrt::fire_and_forget ConnectDevice(DevicePicker, std::wstring_view);
winrt::fire_and_forget ReconnectDeviceTask(std::wstring);
void SetupDevicePicker();
void SetupSvgIcon();
void UpdateNotifyIcon();
winrt::Windows::Foundation::IAsyncOperation<bool> WaitForConnectionStateAsync(
	winrt::Windows::Media::Audio::AudioPlaybackConnection connection,
	winrt::Windows::Media::Audio::AudioPlaybackConnectionState expectedState,
	std::chrono::milliseconds timeout,
	std::chrono::milliseconds pollInterval = std::chrono::milliseconds(50));

static std::mutex g_connMutex;
static UINT WM_UI_UPDATE = 0;
static std::unordered_set<std::wstring> g_pendingConnections;
// Per-device timestamp of the most recent disconnect. Used to enforce a
// cooldown before the next connect, so the underlying A2DP audio route has
// time to fully release. Without this, a manual disconnect followed by an
// immediate reconnect can land on a half-released endpoint that reports
// Opened but produces no audio - the "must connect twice" symptom.
static std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> g_lastDisconnectTime;
static constexpr auto kInitialOpenDelay = std::chrono::milliseconds(500);
static constexpr auto kReconnectOpenDelay = std::chrono::milliseconds(500);
static constexpr auto kOpenedWaitTimeout = std::chrono::milliseconds(1500);
// Minimum gap between a disconnect and the next connect on the SAME device.
// Covers the manual disconnect -> manual reconnect path that previously had
// no release window at all (only the auto-reconnect path was protected).
static constexpr auto kPostDisconnectCooldown = std::chrono::milliseconds(3000);
// Hard ceiling on a single OpenAsync call. Some Bluetooth stacks (notably
// Realtek) can leave OpenAsync pending indefinitely on driver hiccups, which
// would strand the device id in g_pendingConnections and silently dedupe
// every subsequent user retry (the "click does nothing" lockup). The
// watchdog cancels the op after this timeout so the outer catch can clean
// up and surface a retryable error to the UI.
static constexpr auto kOpenAsyncTimeout = std::chrono::seconds(5);
// Flipped in WM_DESTROY before any cleanup begins. fire_and_forget coroutines
// running on the WinRT thread-pool check this at their earliest resumption
// point and bail out, preventing accesses to already-destroyed globals.
static bool g_shuttingDown = false;

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ int       nCmdShow)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);

	g_hInst = hInstance;

	winrt::init_apartment();

	bool supported = false;
	try
	{
		using namespace winrt::Windows::Foundation::Metadata;

		supported = ApiInformation::IsTypePresent(winrt::name_of<DesktopWindowXamlSource>()) &&
			ApiInformation::IsTypePresent(winrt::name_of<AudioPlaybackConnection>());
	}
	catch (winrt::hresult_error const&)
	{
		supported = false;
		LOG_CAUGHT_EXCEPTION();
	}
	if (!supported)
	{
		TaskDialog(nullptr, nullptr, _(L"Unsupported Operating System"), nullptr, _(L"AudioPlaybackConnector is not supported on this operating system version."), TDCBF_OK_BUTTON, TD_ERROR_ICON, nullptr);
		return EXIT_FAILURE;
	}

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_AUDIOPLAYBACKCONNECTOR)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"AudioPlaybackConnector",
		.hIconSm = wcex.hIcon
	};

	RegisterClassExW(&wcex);

	WM_UI_UPDATE = RegisterWindowMessageW(L"WM_UI_UPDATE");
	LOG_LAST_ERROR_IF(WM_UI_UPDATE == 0);

	// When parent window size is 0x0 or invisible, the dpi scale of menu is incorrect. Here we set window size to 1x1 and use WS_EX_LAYERED to make window looks like invisible.
	g_hWnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TOPMOST, L"AudioPlaybackConnector", nullptr, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	FAIL_FAST_LAST_ERROR_IF_NULL(g_hWnd);
	FAIL_FAST_IF_WIN32_BOOL_FALSE(SetLayeredWindowAttributes(g_hWnd, 0, 0, LWA_ALPHA));

	DesktopWindowXamlSource desktopSource;
	auto desktopSourceNative2 = desktopSource.as<IDesktopWindowXamlSourceNative2>();
	winrt::check_hresult(desktopSourceNative2->AttachToWindow(g_hWnd));
	winrt::check_hresult(desktopSourceNative2->get_WindowHandle(&g_hWndXaml));

	g_xamlCanvas = Canvas();
	desktopSource.Content(g_xamlCanvas);

	LoadSettings();
	SetupFlyout();
	SetupMenu();
	SetupDevicePicker();
	SetupSvgIcon();

	g_nid.hWnd = g_niid.hWnd = g_hWnd;
	wcscpy_s(g_nid.szTip, _(L"AudioPlaybackConnector"));
	UpdateNotifyIcon();

	WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
	LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

	PostMessageW(g_hWnd, WM_CONNECTDEVICE, 0, 0);

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		BOOL processed = FALSE;
		winrt::check_hresult(desktopSourceNative2->PreTranslateMessage(&msg, &processed));
		if (!processed)
		{
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	if (WM_UI_UPDATE && message == WM_UI_UPDATE)
	{
		auto p = reinterpret_cast<std::pair<std::wstring, winrt::Windows::Devices::Enumeration::DeviceInformation>*>(wParam);
		if (p)
		{
			bool isActive = (lParam == 1);
			if (isActive)
			{
				g_devicePicker.SetDisplayStatus(p->second, _(L"Connected"),
					DevicePickerDisplayStatusOptions::ShowDisconnectButton);
			}
			else
			{
				g_devicePicker.SetDisplayStatus(p->second, {}, DevicePickerDisplayStatusOptions::None);
			}
			delete p;
		}
		return 0;
	}

	switch (message)
	{
	case WM_DESTROY:
	{
		g_shuttingDown = true;
		std::vector<std::pair<DeviceInformation, AudioPlaybackConnection>> connectionsToClose;
		{
			std::lock_guard<std::mutex> lock(g_connMutex);
			for (const auto& connection : g_audioPlaybackConnections)
			{
				connectionsToClose.push_back(connection.second);
			}
			// SaveSettings encodes the current g_reconnect flag into the JSON;
			// LoadSettings on next startup decides whether to actually reconnect.
			// Both branches were identical, collapsed to remove the dead split.
			SaveSettings();
			g_audioPlaybackConnections.clear();
			g_pendingConnections.clear();
			g_lastDisconnectTime.clear();
		}
		for (const auto& connection : connectionsToClose)
		{
			connection.second.Close();
			g_devicePicker.SetDisplayStatus(connection.first, {}, DevicePickerDisplayStatusOptions::None);
		}
	}
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		if (g_hIconLight) { DestroyIcon(g_hIconLight); g_hIconLight = nullptr; }
		if (g_hIconDark) { DestroyIcon(g_hIconDark); g_hIconDark = nullptr; }
		PostQuitMessage(0);
		break;
	case WM_SETTINGCHANGE:
		if (lParam && CompareStringOrdinal(reinterpret_cast<LPCWCH>(lParam), -1, L"ImmersiveColorSet", -1, TRUE) == CSTR_EQUAL)
		{
			UpdateNotifyIcon();
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		{
			using namespace winrt::Windows::UI::Popups;

			RECT iconRect;
			auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
			if (FAILED(hr))
			{
				LOG_HR(hr);
				break;
			}

			auto dpi = GetDpiForWindow(hWnd);
			Rect rect = {
				static_cast<float>(iconRect.left * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(iconRect.top * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), SWP_HIDEWINDOW);
			SetForegroundWindow(hWnd);
			g_devicePicker.Show(rect, Placement::Above);
		}
		break;
		case WM_RBUTTONUP: // Menu activated by mouse click
			g_menuFocusState = FocusState::Pointer;
			break;
		case WM_CONTEXTMENU:
		{
			if (g_menuFocusState == FocusState::Unfocused)
				g_menuFocusState = FocusState::Keyboard;

			auto dpi = GetDpiForWindow(hWnd);
			Point point = {
				static_cast<float>(GET_X_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi),
				static_cast<float>(GET_Y_LPARAM(wParam) * USER_DEFAULT_SCREEN_DPI / dpi)
			};

			SetWindowPos(g_hWndXaml, 0, 0, 0, 0, 0, SWP_NOZORDER | SWP_SHOWWINDOW);
			SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 1, 1, SWP_SHOWWINDOW);
			SetForegroundWindow(hWnd);

			g_xamlMenu.ShowAt(g_xamlCanvas, point);
		}
		break;
		}
		break;
	case WM_CONNECTDEVICE:
		if (g_reconnect)
		{
			for (const auto& i : g_lastDevices)
			{
				// Reuse the reconnect path on startup so the Bluetooth audio route
				// has time to fully release after the previous process exits.
				ReconnectDeviceTask(i);
			}
			g_lastDevices.clear();
		}
		break;
	case WM_RECONNECTDEVICE:
	{
		auto deviceIdStr = reinterpret_cast<std::wstring*>(wParam);
		if (deviceIdStr)
		{
			ReconnectDeviceTask(std::move(*deviceIdStr));
			delete deviceIdStr; // Free the allocated std::wstring
		}
	}
	break;
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			UpdateNotifyIcon();
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

void SetupFlyout()
{
	TextBlock textBlock;
	textBlock.Text(_(L"All connections will be closed.\nExit anyway?"));
	textBlock.Margin({ 0, 0, 0, 12 });

	CheckBox checkbox;
	checkbox.IsChecked(g_reconnect);
	checkbox.Content(winrt::box_value(_(L"Reconnect on next start")));

	Button button;
	button.Content(winrt::box_value(_(L"Exit")));
	button.HorizontalAlignment(HorizontalAlignment::Right);
	button.Click([&](const auto&, const auto&) {
		g_reconnect = checkbox.IsChecked().Value();
		PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
	});

	StackPanel stackPanel;
	stackPanel.Children().Append(textBlock);
	stackPanel.Children().Append(checkbox);
	stackPanel.Children().Append(button);

	Flyout flyout;
	flyout.ShouldConstrainToRootBounds(false);
	flyout.Content(stackPanel);

	g_xamlFlyout = flyout;
}

void SetupMenu()
{
	// https://docs.microsoft.com/en-us/windows/uwp/design/style/segoe-ui-symbol-font
	FontIcon settingsIcon;
	settingsIcon.Glyph(L"\xE713");

	MenuFlyoutItem settingsItem;
	settingsItem.Text(_(L"Bluetooth Settings"));
	settingsItem.Icon(settingsIcon);
	settingsItem.Click([](const auto&, const auto&) {
		winrt::Windows::System::Launcher::LaunchUriAsync(Uri(L"ms-settings:bluetooth"));
	});

	FontIcon closeIcon;
	closeIcon.Glyph(L"\xE8BB");

	MenuFlyoutItem exitItem;
	exitItem.Text(_(L"Exit"));
	exitItem.Icon(closeIcon);
	exitItem.Click([](const auto&, const auto&) {
		{
			std::lock_guard lock(g_connMutex);
			if (g_audioPlaybackConnections.size() == 0)
			{
				PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
				return;
			}
		}

		RECT iconRect;
		auto hr = Shell_NotifyIconGetRect(&g_niid, &iconRect);
		if (FAILED(hr))
		{
			LOG_HR(hr);
			return;
		}

		auto dpi = GetDpiForWindow(g_hWnd);

		SetWindowPos(g_hWnd, HWND_TOPMOST, iconRect.left, iconRect.top, 0, 0, SWP_HIDEWINDOW);
		g_xamlCanvas.Width(static_cast<float>((iconRect.right - iconRect.left) * USER_DEFAULT_SCREEN_DPI / dpi));
		g_xamlCanvas.Height(static_cast<float>((iconRect.bottom - iconRect.top) * USER_DEFAULT_SCREEN_DPI / dpi));

		g_xamlFlyout.ShowAt(g_xamlCanvas);
	});

	MenuFlyout menu;
	menu.Items().Append(settingsItem);
	menu.Items().Append(exitItem);
	menu.Opened([](const auto& sender, const auto&) {
		auto menuItems = sender.as<MenuFlyout>().Items();
		auto itemsCount = menuItems.Size();
		if (itemsCount > 0)
		{
			menuItems.GetAt(itemsCount - 1).Focus(g_menuFocusState);
		}
		g_menuFocusState = FocusState::Unfocused;
	});
	menu.Closed([](const auto&, const auto&) {
		ShowWindow(g_hWnd, SW_HIDE);
	});

	g_xamlMenu = menu;
}

winrt::Windows::Foundation::IAsyncOperation<bool> WaitForConnectionStateAsync(
	AudioPlaybackConnection connection,
	AudioPlaybackConnectionState expectedState,
	std::chrono::milliseconds timeout,
	std::chrono::milliseconds pollInterval)
{
	if (connection.State() == expectedState)
	{
		co_return true;
	}

	auto elapsed = std::chrono::milliseconds(0);
	while (elapsed < timeout)
	{
		co_await winrt::resume_after(pollInterval);
		elapsed += pollInterval;

		if (connection.State() == expectedState)
		{
			co_return true;
		}

		if (connection.State() == AudioPlaybackConnectionState::Closed)
		{
			co_return false;
		}
	}

	co_return connection.State() == expectedState;
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, DeviceInformation device)
{
	if (g_shuttingDown) co_return;
	picker.SetDisplayStatus(device, _(L"Connecting"), DevicePickerDisplayStatusOptions::ShowProgress | DevicePickerDisplayStatusOptions::ShowDisconnectButton);

	bool openRequested = false;
	std::wstring errorMessage;
	auto deviceId = std::wstring(device.Id());

	try
	{
		{
			std::lock_guard lock(g_connMutex);
			if (g_pendingConnections.contains(deviceId))
			{
				co_return;
			}

			auto existing = g_audioPlaybackConnections.find(deviceId);
			if (existing != g_audioPlaybackConnections.end())
			{
				if (existing->second.second.State() == AudioPlaybackConnectionState::Opened)
				{
					picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
				}
				co_return;
			}

			g_pendingConnections.insert(deviceId);
		}

		// Step 1-2-3 post-disconnect cooldown (executed OUTSIDE the lock):
		// 1. Look up when this device was last disconnected.
		// 2. If still within the cooldown window, await the remaining time so
		//    the Bluetooth stack can finish releasing the prior A2DP route.
		// 3. Skipping this lets OpenAsync land on a half-released endpoint
		//    that reports Opened but routes no audio - the root cause of the
		//    "first reconnect has no sound" symptom on consumer BT chips.
		std::chrono::milliseconds remainingCooldown(0);
		{
			std::lock_guard lock(g_connMutex);
			auto it = g_lastDisconnectTime.find(deviceId);
			if (it != g_lastDisconnectTime.end())
			{
				auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
					std::chrono::steady_clock::now() - it->second);
				if (elapsed < kPostDisconnectCooldown)
				{
					remainingCooldown = kPostDisconnectCooldown - elapsed;
				}
				else
				{
					g_lastDisconnectTime.erase(it);
				}
			}
		}
		if (remainingCooldown.count() > 0)
		{
			co_await winrt::resume_after(remainingCooldown);
		}

		// USB Bluetooth dongles cache A2DP endpoint state in their firmware.
		// Creating a temp connection and immediately closing it before the
		// real one prods the dongle to flush stale state from prior sessions
		// so the subsequent OpenAsync lands on a fresh internal endpoint.
		{
			auto flushConn = AudioPlaybackConnection::TryCreateFromId(device.Id());
			if (flushConn) flushConn.Close();
		}

		auto connection = AudioPlaybackConnection::TryCreateFromId(device.Id());
		if (connection)
		{
			{
				std::lock_guard lock(g_connMutex);
				auto [_, inserted] = g_audioPlaybackConnections.emplace(deviceId, std::pair(device, connection));
				if (!inserted)
				{
					g_pendingConnections.erase(deviceId);
					if (connection.State() == AudioPlaybackConnectionState::Opened)
					{
						picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
					}
					co_return;
				}
			}

			connection.StateChanged([](const auto& sender, const auto&) {
				auto deviceId = std::wstring(sender.DeviceId());
				std::lock_guard lock(g_connMutex);
				auto it = g_audioPlaybackConnections.find(deviceId);
				if (it == g_audioPlaybackConnections.end())
				{
					return; // Device already removed or doesn't exist
				}

				// Reject stale StateChanged events from a PREVIOUS connection
				// instance for the same device. After Close(), the old connection's
				// StateChanged(Closed) may fire asynchronously on the WinRT thread
				// pool AFTER a new connection's map entry has already been inserted.
				// Without this identity check the stale callback would wrongly erase
				// the new connection from the map and clobber its tracking state.
				if (it->second.second != sender)
				{
					return;
				}

				if (sender.State() == AudioPlaybackConnectionState::Opened)
				{
					g_pendingConnections.erase(deviceId);
					// Marshal UI operation to main thread via PostMessage (StateChanged may execute on arbitrary WinRT thread)
					auto deviceCopy = it->second.first;
					auto p = new std::pair<std::wstring, winrt::Windows::Devices::Enumeration::DeviceInformation>(deviceId, std::move(deviceCopy));
					if (!PostMessageW(g_hWnd, WM_UI_UPDATE, reinterpret_cast<WPARAM>(p), 1))
					{
						delete p;
					}
				}
				else if (sender.State() == AudioPlaybackConnectionState::Closed)
				{
					g_pendingConnections.erase(deviceId);
					// Stamp the disconnect time so a follow-up connect on the
					// same device waits out kPostDisconnectCooldown for the
					// A2DP route to fully release before reopening.
					g_lastDisconnectTime[deviceId] = std::chrono::steady_clock::now();
					// Marshal UI operation to main thread via PostMessage (StateChanged may execute on arbitrary WinRT thread)
					auto deviceCopy = it->second.first;
					auto p = new std::pair<std::wstring, winrt::Windows::Devices::Enumeration::DeviceInformation>(deviceId, std::move(deviceCopy));
					if (!PostMessageW(g_hWnd, WM_UI_UPDATE, reinterpret_cast<WPARAM>(p), 0))
					{
						delete p;
					}
					// Post message to main thread for delayed reconnect - avoids race condition on g_audioPlaybackConnections
					auto deviceIdAlloc = new std::wstring(deviceId);
					if (!PostMessageW(g_hWnd, WM_RECONNECTDEVICE, reinterpret_cast<WPARAM>(deviceIdAlloc), 0))
					{
						delete deviceIdAlloc;
					}
					g_audioPlaybackConnections.erase(it);
				}
			});

			try
			{
				co_await connection.StartAsync();
				co_await winrt::resume_after(kInitialOpenDelay);
				// Step 1-2-3 watchdog around OpenAsync to defeat driver hangs:
				// 1. Capture the in-flight op so we can race it.
				// 2. Spawn a fire-and-forget timer that cancels the op if it
				//    hasn't completed within kOpenAsyncTimeout.
				// 3. Awaiting a cancelled op throws hresult_canceled, which the
				//    outer catch turns into a retryable error - critical for
				//    breaking the "next click does nothing" lockup caused by a
				//    coroutine stuck on a non-responsive Bluetooth stack.
				auto openOp = connection.OpenAsync();
				[](winrt::Windows::Foundation::IAsyncOperation<AudioPlaybackConnectionOpenResult> op) -> winrt::fire_and_forget {
					co_await winrt::resume_after(kOpenAsyncTimeout);
					if (op.Status() == winrt::Windows::Foundation::AsyncStatus::Started)
					{
						op.Cancel();
					}
				}(openOp);
				auto result = co_await openOp;

				switch (result.Status())
				{
				case AudioPlaybackConnectionOpenResultStatus::Success:
					openRequested = true;
					if (co_await WaitForConnectionStateAsync(connection, AudioPlaybackConnectionState::Opened, kOpenedWaitTimeout))
					{
						std::lock_guard lock(g_connMutex);
						g_pendingConnections.erase(deviceId);
						// Successful open clears any stale cooldown stamp so a
						// later disconnect+reconnect cycle restarts the timer.
						g_lastDisconnectTime.erase(deviceId);
						picker.SetDisplayStatus(device, _(L"Connected"), DevicePickerDisplayStatusOptions::ShowDisconnectButton);
					}
					else
					{
						// Step 1-2-3 fallback when Opened is not observed in time:
						// 1. Mark the attempt as failed so the outer cleanup runs.
						// 2. Surface a retryable error instead of leaving "Connecting".
						// 3. Cleanup will Close() the half-open connection, which
						//    fires StateChanged Closed and triggers auto-reconnect.
						openRequested = false;
						errorMessage = _(L"The request timed out");
					}
					break;
				case AudioPlaybackConnectionOpenResultStatus::RequestTimedOut:
					errorMessage = _(L"The request timed out");
					break;
				case AudioPlaybackConnectionOpenResultStatus::DeniedBySystem:
					errorMessage = _(L"The operation was denied by the system");
					break;
				case AudioPlaybackConnectionOpenResultStatus::UnknownFailure:
					winrt::throw_hresult(result.ExtendedError());
					break;
				}
			}
			catch (...)
			{
				// OpenAsync or StartAsync threw an exception - clean up the map entry before propagating
				std::lock_guard lock(g_connMutex);
				g_pendingConnections.erase(deviceId);
				auto it = g_audioPlaybackConnections.find(deviceId);
				if (it != g_audioPlaybackConnections.end())
				{
					try {
						it->second.second.Close();
					} catch (...) {
						// Log but don't let Close failure mask the original exception
						LOG_CAUGHT_EXCEPTION();
					}
					g_audioPlaybackConnections.erase(it);
				}
				throw; // re-throw to let outer catch handle error message display
			}
		}
		else
		{
			errorMessage = _(L"Unknown error");
		}
	}
	catch (winrt::hresult_error const& ex)
	{
		// Guard the swprintf retry loop with a hard upper bound so a format
		// error (e.g. malformed UTF-16 in the error message) that causes
		// swprintf to keep returning -1 won't balloon the buffer to OOM.
		errorMessage.resize(64);
		constexpr size_t kMaxErrorSize = 8192;
		while (true)
		{
			auto result = swprintf(errorMessage.data(), errorMessage.size(), L"%s (0x%08X)", ex.message().c_str(), static_cast<uint32_t>(ex.code()));
			if (result < 0)
			{
				if (errorMessage.size() >= kMaxErrorSize)
				{
					// Fallback: at least emit the error code so diagnostics aren't lost
					errorMessage = L"System error (0x00000000)";
					swprintf(errorMessage.data(), errorMessage.size(), L"System error (0x%08X)", static_cast<uint32_t>(ex.code()));
					errorMessage.resize(wcslen(errorMessage.data()));
					break;
				}
				errorMessage.resize(errorMessage.size() * 2);
			}
			else
			{
				errorMessage.resize(result);
				break;
			}
		}
		LOG_CAUGHT_EXCEPTION();
	}

	if (!openRequested)
	{
		std::lock_guard lock(g_connMutex);
		g_pendingConnections.erase(deviceId);
		auto it = g_audioPlaybackConnections.find(deviceId);
		if (it != g_audioPlaybackConnections.end())
		{
			it->second.second.Close();
			g_audioPlaybackConnections.erase(it);
		}
		picker.SetDisplayStatus(device, errorMessage, DevicePickerDisplayStatusOptions::ShowRetryButton);
	}
}

winrt::fire_and_forget ConnectDevice(DevicePicker picker, std::wstring_view deviceId)
{
	if (g_shuttingDown) co_return;
	auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
	if (device.Name().empty())
	{
		picker.SetDisplayStatus(device, _(L"Device not available"), DevicePickerDisplayStatusOptions::ShowRetryButton);
		co_return;
	}
	ConnectDevice(picker, device);
}

winrt::fire_and_forget ReconnectDeviceTask(std::wstring deviceId)
{
	if (g_shuttingDown) co_return;
	// Delay to allow audio subsystem to release resources before allowing reconnect
	co_await winrt::resume_after(kReconnectOpenDelay);

	auto device = co_await DeviceInformation::CreateFromIdAsync(deviceId);
	if (!device.Name().empty())
	{
		ConnectDevice(g_devicePicker, device);
	}
}

void SetupDevicePicker()
{
	g_devicePicker = DevicePicker();
	winrt::check_hresult(g_devicePicker.as<IInitializeWithWindow>()->Initialize(g_hWnd));

	g_devicePicker.Filter().SupportedDeviceSelectors().Append(AudioPlaybackConnection::GetDeviceSelector());
	g_devicePicker.DevicePickerDismissed([](const auto&, const auto&) {
		SetWindowPos(g_hWnd, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_HIDEWINDOW);
	});
	g_devicePicker.DeviceSelected([](const auto& sender, const auto& args) {
		ConnectDevice(sender, args.SelectedDevice());
	});
	g_devicePicker.DisconnectButtonClicked([](const auto& sender, const auto& args) {
		auto device = args.Device();
		auto deviceIdStr = std::wstring(device.Id());
		AudioPlaybackConnection connectionToClose = nullptr;
		{
			std::lock_guard lock(g_connMutex);
			g_pendingConnections.erase(deviceIdStr);
			auto it = g_audioPlaybackConnections.find(deviceIdStr);
			if (it != g_audioPlaybackConnections.end())
			{
				connectionToClose = it->second.second;
				g_audioPlaybackConnections.erase(it);
				// Stamp the disconnect time so an immediate user-driven
				// reconnect on the same device waits for the A2DP route to
				// release. Without this, the next OpenAsync may complete on
				// a stale endpoint that reports Opened but produces no audio.
				g_lastDisconnectTime[deviceIdStr] = std::chrono::steady_clock::now();
				// Cap the map at kMaxDisconnectEntries; evict the oldest
				// entry when the threshold is exceeded. Prevents unbounded
				// memory growth from devices the user disconnected once and
				// never reconnects to.
				constexpr size_t kMaxDisconnectEntries = 32;
				while (g_lastDisconnectTime.size() > kMaxDisconnectEntries)
				{
					auto oldest = std::min_element(
						g_lastDisconnectTime.begin(),
						g_lastDisconnectTime.end(),
						[](const auto& a, const auto& b) { return a.second < b.second; });
					if (oldest != g_lastDisconnectTime.end())
						g_lastDisconnectTime.erase(oldest);
				}
			}
		}
		if (connectionToClose) connectionToClose.Close();
		sender.SetDisplayStatus(device, {}, DevicePickerDisplayStatusOptions::None);
	});
}

void SetupSvgIcon()
{
	auto hRes = FindResourceW(g_hInst, MAKEINTRESOURCEW(1), L"SVG");
	FAIL_FAST_LAST_ERROR_IF_NULL(hRes);

	auto size = SizeofResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF(size == 0);

	auto hResData = LoadResource(g_hInst, hRes);
	FAIL_FAST_LAST_ERROR_IF_NULL(hResData);

	auto svgData = reinterpret_cast<const char*>(LockResource(hResData));
	FAIL_FAST_IF_NULL_ALLOC(svgData);

	const std::string_view svg(svgData, size);
	const int width = GetSystemMetrics(SM_CXSMICON), height = GetSystemMetrics(SM_CYSMICON);

	g_hIconLight = SvgTohIcon(svg, width, height, { 0, 0, 0, 1 });
	g_hIconDark = SvgTohIcon(svg, width, height, { 1, 1, 1, 1 });
}

void UpdateNotifyIcon()
{
	DWORD value = 0, cbValue = sizeof(value);
	LOG_IF_WIN32_ERROR(RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &cbValue));
	g_nid.hIcon = value != 0 ? g_hIconLight : g_hIconDark;

	if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
	{
		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}
	}
}
