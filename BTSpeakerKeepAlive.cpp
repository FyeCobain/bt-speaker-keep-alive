#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <propvarutil.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr wchar_t WINDOW_CLASS[] = L"BTSpeakerKeepAliveTrayWindow";
    constexpr UINT TRAY_CALLBACK = WM_APP + 1;
    constexpr UINT NEW_DEVICE_MESSAGE = WM_APP + 2;
    constexpr UINT WAKE_STATUS_MESSAGE = WM_APP + 3;
    constexpr UINT COMMAND_ENGLISH = 1001;
    constexpr UINT COMMAND_SPANISH = 1002;
    constexpr UINT COMMAND_GERMAN = 1003;
    constexpr UINT COMMAND_JAPANESE = 1004;
    constexpr UINT COMMAND_EXIT = 1005;
    constexpr UINT COMMAND_OPEN_FOLDER = 1006;
    constexpr UINT COMMAND_START_WITH_WINDOWS = 1007;
    constexpr UINT COMMAND_PROMPT_NEW_DEVICES = 1008;
    constexpr UINT COMMAND_DEVICE_BASE = 2000;
    constexpr UINT DEVICE_SCAN_TIMER = 1;
    constexpr WORD TRAY_ACTIVE_ICON_RESOURCE = 102;
    constexpr WORD TRAY_IDLE_ICON_RESOURCE = 103;
    constexpr wchar_t STARTUP_REGISTRY_KEY[] =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    // Keep this registry value name stable across display-name and exe renames.
    constexpr wchar_t STARTUP_VALUE_NAME[] = L"BT Speaker KeepAlive";

    const PROPERTYKEY FRIENDLY_NAME_KEY =
    {
        { 0xA45C254E, 0xDF1C, 0x4EFD,
          { 0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0 } },
        14
    };

    std::atomic<bool> running{true};
    HWND mainWindow = nullptr;
    IMMDeviceEnumerator* discoveryEnumerator = nullptr;
    NOTIFYICONDATAW trayIcon = {};
    bool trayIconAdded = false;
    HICON trayIconAwake = nullptr;
    HICON trayIconIdle = nullptr;
    HICON menuIconAwake = nullptr;
    HICON menuIconIdle = nullptr;
    std::atomic<unsigned> activeKeepAliveStreams{0};
    std::wstring appDirectory;
    std::wstring language = L"en";
    bool promptForNewDevices = true;
    std::unordered_map<std::wstring, std::wstring> strings;

    struct RegisteredDevice
    {
        std::wstring id;
        std::wstring name;
        bool enabled = false;
    };

    struct PendingDevice
    {
        std::wstring id;
        std::wstring name;
    };

    struct DeviceGroup
    {
        std::wstring name;
        bool enabled = false;
        bool current = false;
    };

    std::mutex devicesMutex;
    std::vector<RegisteredDevice> registeredDevices;
    std::unordered_set<std::wstring> pendingDeviceIds;
    std::vector<std::wstring> pendingDeviceNames;
    std::unordered_map<UINT, std::wstring> menuDeviceCommands;

    bool SameDeviceName(const std::wstring& left, const std::wstring& right)
    {
        return CompareStringOrdinal(
            left.c_str(), static_cast<int>(left.size()),
            right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
    }

    bool FindGroupEnabledLocked(const std::wstring& name, bool& enabled)
    {
        bool found = false;
        enabled = false;
        for (const RegisteredDevice& device : registeredDevices)
        {
            if (SameDeviceName(device.name, name))
            {
                found = true;
                enabled = enabled || device.enabled;
            }
        }
        return found;
    }

    void SetGroupEnabledLocked(const std::wstring& name, bool enabled)
    {
        for (RegisteredDevice& device : registeredDevices)
        {
            if (SameDeviceName(device.name, name))
                device.enabled = enabled;
        }
    }

    std::wstring Trim(std::wstring value)
    {
        const auto isSpace = [](wchar_t c) { return std::iswspace(c) != 0; };
        while (!value.empty() && isSpace(value.front()))
            value.erase(value.begin());
        while (!value.empty() && isSpace(value.back()))
            value.pop_back();
        return value;
    }

    std::wstring ExecutableDirectory()
    {
        std::vector<wchar_t> path(32768);
        const DWORD length = GetModuleFileNameW(
            nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0 || length >= path.size())
            return L"";

        std::wstring result(path.data(), length);
        const size_t slash = result.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            return L"";
        result.resize(slash + 1);
        return result;
    }

    bool ReadUtf8File(const std::wstring& path, std::wstring& contents)
    {
        HANDLE file = CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;

        LARGE_INTEGER size = {};
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 65536)
        {
            CloseHandle(file);
            return false;
        }

        std::vector<char> bytes(static_cast<size_t>(size.QuadPart));
        DWORD bytesRead = 0;
        const BOOL readOk = ReadFile(
            file, bytes.data(), static_cast<DWORD>(bytes.size()), &bytesRead, nullptr);
        CloseHandle(file);
        if (!readOk || bytesRead != bytes.size())
            return false;

        const int offset = bytesRead >= 3 &&
            static_cast<unsigned char>(bytes[0]) == 0xEF &&
            static_cast<unsigned char>(bytes[1]) == 0xBB &&
            static_cast<unsigned char>(bytes[2]) == 0xBF ? 3 : 0;
        const int wideLength = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + offset,
            static_cast<int>(bytesRead) - offset, nullptr, 0);
        if (wideLength <= 0)
            return false;

        std::vector<wchar_t> wide(static_cast<size_t>(wideLength));
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + offset,
                static_cast<int>(bytesRead) - offset, wide.data(), wideLength) <= 0)
            return false;

        contents.assign(wide.begin(), wide.end());
        return true;
    }

    bool LoadTranslationFile(const std::wstring& lang)
    {
        const std::wstring filename = L"translations\\" + lang + L".ini";
        const std::wstring candidates[] =
        {
            appDirectory + filename,
            appDirectory + L"..\\..\\" + filename,
            filename
        };

        std::wstring contents;
        bool loaded = false;
        for (const std::wstring& candidate : candidates)
        {
            if (ReadUtf8File(candidate, contents))
            {
                loaded = true;
                break;
            }
        }
        if (!loaded)
            return false;

        std::unordered_map<std::wstring, std::wstring> parsed;
        size_t start = 0;
        while (start <= contents.size())
        {
            const size_t end = contents.find(L'\n', start);
            std::wstring line = Trim(contents.substr(
                start, end == std::wstring::npos ? end : end - start));
            if (!line.empty() && line.front() != L'#' && line.front() != L';')
            {
                const size_t equals = line.find(L'=');
                if (equals != std::wstring::npos)
                {
                    const std::wstring key = Trim(line.substr(0, equals));
                    const std::wstring value = Trim(line.substr(equals + 1));
                    if (!key.empty())
                        parsed[key] = value;
                }
            }
            if (end == std::wstring::npos)
                break;
            start = end + 1;
        }

        strings.swap(parsed);
        return true;
    }

    std::wstring Text(const wchar_t* key)
    {
        const auto found = strings.find(key);
        return found == strings.end() ? key : found->second;
    }

    bool IsStartWithWindowsEnabled()
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REGISTRY_KEY, 0,
                          KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
            return false;

        DWORD type = 0;
        DWORD size = 0;
        const LSTATUS status = RegQueryValueExW(
            key, STARTUP_VALUE_NAME, nullptr, &type, nullptr, &size);
        RegCloseKey(key);
        return status == ERROR_SUCCESS && size > 0;
    }

    bool SetStartWithWindowsEnabled(bool enabled)
    {
        HKEY key = nullptr;
        if (enabled)
        {
            DWORD disposition = 0;
            if (RegCreateKeyExW(HKEY_CURRENT_USER, STARTUP_REGISTRY_KEY, 0,
                                nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                nullptr, &key, &disposition) != ERROR_SUCCESS)
                return false;

            std::vector<wchar_t> path(32768);
            const DWORD length = GetModuleFileNameW(
                nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0 || length >= path.size())
            {
                RegCloseKey(key);
                return false;
            }

            std::wstring commandLine = L"\"";
            commandLine.append(path.data(), length);
            commandLine += L"\"";
            const DWORD dataSize = static_cast<DWORD>(
                (commandLine.size() + 1) * sizeof(wchar_t));
            const LSTATUS status = RegSetValueExW(
                key, STARTUP_VALUE_NAME, 0, REG_SZ,
                reinterpret_cast<const BYTE*>(commandLine.c_str()), dataSize);
            RegCloseKey(key);
            return status == ERROR_SUCCESS;
        }

        if (RegOpenKeyExW(HKEY_CURRENT_USER, STARTUP_REGISTRY_KEY, 0,
                          KEY_SET_VALUE, &key) != ERROR_SUCCESS)
            return true;
        const LSTATUS status = RegDeleteValueW(key, STARTUP_VALUE_NAME);
        RegCloseKey(key);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }

    void ToggleStartWithWindows(HWND window)
    {
        if (!SetStartWithWindowsEnabled(!IsStartWithWindowsEnabled()))
        {
            const std::wstring message = Text(L"startup_error");
            const std::wstring title = Text(L"menu_title");
            MessageBoxW(window, message.c_str(), title.c_str(), MB_OK | MB_ICONERROR);
        }
    }

    bool DecodeHex(const std::wstring& value, std::wstring& result)
    {
        if (value.size() % 4 != 0)
            return false;

        result.clear();
        for (size_t i = 0; i < value.size(); i += 4)
        {
            unsigned int code = 0;
            for (size_t j = 0; j < 4; ++j)
            {
                const wchar_t c = value[i + j];
                unsigned int digit = 0;
                if (c >= L'0' && c <= L'9') digit = static_cast<unsigned int>(c - L'0');
                else if (c >= L'A' && c <= L'F') digit = static_cast<unsigned int>(c - L'A' + 10);
                else if (c >= L'a' && c <= L'f') digit = static_cast<unsigned int>(c - L'a' + 10);
                else return false;
                code = (code << 4) | digit;
            }
            result.push_back(static_cast<wchar_t>(code));
        }
        return true;
    }

    bool DecodeName(const std::wstring& value, std::wstring& result)
    {
        if (value.compare(0, 5, L"utf8:") != 0)
            return DecodeHex(value, result);

        std::vector<char> bytes;
        for (size_t i = 5; i < value.size(); ++i)
        {
            if (value[i] == L'%')
            {
                if (i + 2 >= value.size())
                    return false;
                const auto hexDigit = [](wchar_t c) -> int
                {
                    if (c >= L'0' && c <= L'9') return c - L'0';
                    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
                    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
                    return -1;
                };
                const int high = hexDigit(value[i + 1]);
                const int low = hexDigit(value[i + 2]);
                if (high < 0 || low < 0)
                    return false;
                bytes.push_back(static_cast<char>((high << 4) | low));
                i += 2;
            }
            else if (value[i] <= 0x7F)
            {
                bytes.push_back(static_cast<char>(value[i]));
            }
            else
            {
                return false;
            }
        }

        if (bytes.empty())
        {
            result.clear();
            return true;
        }
        const int wideCount = MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()),
            nullptr, 0);
        if (wideCount <= 0)
            return false;
        std::vector<wchar_t> wide(static_cast<size_t>(wideCount));
        if (MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()),
                wide.data(), wideCount) <= 0)
            return false;
        result.assign(wide.begin(), wide.end());
        return true;
    }

    void SaveSettings()
    {
        const std::wstring path = appDirectory + L"settings.ini";
        HANDLE file = CreateFileW(
            path.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return;

        std::wstring wideContents = L"language=" + language + L"\r\n";
        wideContents += L"prompt_new_devices=";
        wideContents += promptForNewDevices ? L"1\r\n" : L"0\r\n";
        {
            std::lock_guard<std::mutex> lock(devicesMutex);
            std::vector<DeviceGroup> groups;
            for (const RegisteredDevice& device : registeredDevices)
            {
                auto group = std::find_if(groups.begin(), groups.end(),
                    [&device](const DeviceGroup& item)
                    {
                        return SameDeviceName(item.name, device.name);
                    });
                if (group == groups.end())
                {
                    DeviceGroup entry;
                    entry.name = device.name;
                    entry.enabled = device.enabled;
                    groups.push_back(entry);
                }
                else
                    group->enabled = group->enabled || device.enabled;
            }
            for (const DeviceGroup& group : groups)
            {
                wideContents += L"device=";
                wideContents += group.enabled ? L"1," : L"0,";
                wideContents += group.name;
                wideContents += L"\r\n";
            }
        }
        const int byteCount = WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, wideContents.data(),
            static_cast<int>(wideContents.size()), nullptr, 0, nullptr, nullptr);
        if (byteCount <= 0)
        {
            CloseHandle(file);
            return;
        }
        std::vector<char> contents(static_cast<size_t>(byteCount));
        if (WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, wideContents.data(),
                static_cast<int>(wideContents.size()), contents.data(), byteCount,
                nullptr, nullptr) <= 0)
        {
            CloseHandle(file);
            return;
        }
        DWORD written = 0;
        WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr);
        CloseHandle(file);
    }

    void LoadSettings()
    {
        std::wstring contents;
        if (!ReadUtf8File(appDirectory + L"settings.ini", contents))
            return;

        bool migrateSettings = false;
        size_t start = 0;
        while (start <= contents.size())
        {
            const size_t end = contents.find(L'\n', start);
            const std::wstring line = Trim(contents.substr(
                start, end == std::wstring::npos ? end : end - start));
            if (line.compare(0, 9, L"language=") == 0)
            {
                const std::wstring value = Trim(line.substr(9));
                if (value == L"eng" || value == L"en")
                {
                    language = L"en";
                    migrateSettings = migrateSettings || value == L"eng";
                }
                else if (value == L"spa" || value == L"es")
                {
                    language = L"es";
                    migrateSettings = migrateSettings || value == L"spa";
                }
                else if (value == L"de" || value == L"ja")
                    language = value;
            }
            else if (line.compare(0, 19, L"prompt_new_devices=") == 0)
                promptForNewDevices = Trim(line.substr(19)) == L"1";
            else if (line.compare(0, 7, L"device=") == 0)
            {
                const size_t firstComma = line.find(L',', 7);
                const size_t secondComma = line.find(L',', firstComma == std::wstring::npos ? firstComma : firstComma + 1);
                if (firstComma != std::wstring::npos &&
                    (line[7] == L'0' || line[7] == L'1'))
                {
                    RegisteredDevice device;
                    device.enabled = line[7] == L'1';
                    // New format stores only the friendly name. Accept the previous
                    // ID,name format too so existing settings migrate automatically.
                    const size_t nameStart = secondComma == std::wstring::npos
                        ? firstComma + 1 : secondComma + 1;
                    migrateSettings = migrateSettings ||
                        secondComma != std::wstring::npos;
                    const std::wstring encodedName = line.substr(nameStart);
                    migrateSettings = migrateSettings ||
                        encodedName.compare(0, 5, L"utf8:") == 0;
                    const bool decoded = secondComma != std::wstring::npos ||
                        encodedName.compare(0, 5, L"utf8:") == 0;
                    const bool validName = decoded
                        ? DecodeName(encodedName, device.name)
                        : (device.name = encodedName, true);
                    if (validName && !device.name.empty())
                    {
                        std::lock_guard<std::mutex> lock(devicesMutex);
                        const auto duplicate = std::find_if(
                            registeredDevices.begin(), registeredDevices.end(),
                            [&device](const RegisteredDevice& item)
                            {
                                return SameDeviceName(item.name, device.name);
                            });
                        if (duplicate == registeredDevices.end())
                            registeredDevices.push_back(device);
                        else
                        {
                            duplicate->enabled = duplicate->enabled || device.enabled;
                        }
                    }
                }
            }
            if (end == std::wstring::npos)
                break;
            start = end + 1;
        }
        if (migrateSettings)
            SaveSettings();
    }

    void UpdateTrayIcon()
    {
        if (!trayIconAdded)
            return;
        const bool awake = activeKeepAliveStreams.load() > 0;
        HICON statusIcon = awake ? trayIconAwake : trayIconIdle;
        if (statusIcon)
            trayIcon.hIcon = statusIcon;
        trayIcon.uFlags = NIF_ICON | NIF_TIP;
        const std::wstring tip = Text(awake ? L"tray_tip_active" : L"tray_tip_idle");
        std::wcsncpy(trayIcon.szTip, tip.c_str(), ARRAYSIZE(trayIcon.szTip) - 1);
        trayIcon.szTip[ARRAYSIZE(trayIcon.szTip) - 1] = L'\0';
        Shell_NotifyIconW(NIM_MODIFY, &trayIcon);
    }

    void SetLanguage(const std::wstring& lang)
    {
        if (lang == language)
            return;
        language = lang;
        LoadTranslationFile(language);
        SaveSettings();
        UpdateTrayIcon();
    }

    void DiscoverOutputDevices();
    void HandleNewDevice(PendingDevice* device);
    void ToggleRegisteredDevice(UINT command);

    std::wstring DefaultRenderDeviceId()
    {
        if (!discoveryEnumerator)
            return L"";

        IMMDevice* device = nullptr;
        if (FAILED(discoveryEnumerator->GetDefaultAudioEndpoint(
                eRender, eConsole, &device)) || !device)
            return L"";

        std::wstring id;
        LPWSTR rawId = nullptr;
        if (SUCCEEDED(device->GetId(&rawId)) && rawId)
        {
            id = rawId;
            CoTaskMemFree(rawId);
        }
        device->Release();
        return id;
    }

    void ShowTrayMenu(HWND window)
    {
        HMENU menu = CreatePopupMenu();
        HMENU languageMenu = CreatePopupMenu();
        HMENU devicesMenu = CreatePopupMenu();
        if (!menu || !languageMenu || !devicesMenu)
        {
            if (menu) DestroyMenu(menu);
            if (languageMenu) DestroyMenu(languageMenu);
            if (devicesMenu) DestroyMenu(devicesMenu);
            return;
        }

        AppendMenuW(languageMenu, MF_STRING | (language == L"en" ? MF_CHECKED : 0),
                    COMMAND_ENGLISH, Text(L"language_english").c_str());
        AppendMenuW(languageMenu, MF_STRING | (language == L"es" ? MF_CHECKED : 0),
                    COMMAND_SPANISH, Text(L"language_spanish").c_str());
        AppendMenuW(languageMenu, MF_STRING | (language == L"de" ? MF_CHECKED : 0),
                    COMMAND_GERMAN, Text(L"language_german").c_str());
        AppendMenuW(languageMenu, MF_STRING | (language == L"ja" ? MF_CHECKED : 0),
                    COMMAND_JAPANESE, Text(L"language_japanese").c_str());

        const std::wstring currentDeviceId = DefaultRenderDeviceId();
        std::vector<DeviceGroup> groups;
        {
            std::lock_guard<std::mutex> lock(devicesMutex);
            for (const RegisteredDevice& device : registeredDevices)
            {
                auto group = std::find_if(groups.begin(), groups.end(),
                    [&device](const DeviceGroup& item)
                    {
                        return SameDeviceName(item.name, device.name);
                    });
                if (group == groups.end())
                {
                    DeviceGroup entry;
                    entry.name = device.name;
                    entry.enabled = device.enabled;
                    entry.current = !currentDeviceId.empty() &&
                                    device.id == currentDeviceId;
                    groups.push_back(entry);
                }
                else
                {
                    group->enabled = group->enabled || device.enabled;
                    group->current = group->current ||
                                     (!currentDeviceId.empty() &&
                                      device.id == currentDeviceId);
                }
            }
        }
        menuDeviceCommands.clear();
        if (groups.empty())
        {
            AppendMenuW(devicesMenu, MF_STRING | MF_GRAYED, 0,
                        Text(L"menu_no_devices").c_str());
        }
        else
        {
            for (size_t i = 0; i < groups.size() && i < 50000; ++i)
            {
                const UINT command = COMMAND_DEVICE_BASE + static_cast<UINT>(i);
                const UINT state = groups[i].enabled ? MF_CHECKED : 0;
                AppendMenuW(devicesMenu, MF_OWNERDRAW | state, command,
                            reinterpret_cast<LPCWSTR>(&groups[i]));
                menuDeviceCommands[command] = groups[i].name;
            }
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(devicesMenu),
                    Text(L"menu_devices").c_str());
        AppendMenuW(menu, MF_STRING |
                    (promptForNewDevices ? MF_CHECKED : 0),
                    COMMAND_PROMPT_NEW_DEVICES,
                    Text(L"menu_prompt_new_devices").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING |
                    (IsStartWithWindowsEnabled() ? MF_CHECKED : 0),
                    COMMAND_START_WITH_WINDOWS,
                    Text(L"menu_start_with_windows").c_str());
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(languageMenu),
                    Text(L"menu_language").c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, COMMAND_OPEN_FOLDER,
                    Text(L"menu_open_folder").c_str());
        AppendMenuW(menu, MF_STRING, COMMAND_EXIT, Text(L"menu_exit").c_str());

        POINT cursor = {};
        GetCursorPos(&cursor);
        SetForegroundWindow(window);
        const UINT command = TrackPopupMenu(
            menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN | TPM_RETURNCMD,
            cursor.x, cursor.y, 0, window, nullptr);
        PostMessageW(window, WM_NULL, 0, 0);
        DestroyMenu(menu);

        if (command == COMMAND_ENGLISH)
            SetLanguage(L"en");
        else if (command == COMMAND_SPANISH)
            SetLanguage(L"es");
        else if (command == COMMAND_GERMAN)
            SetLanguage(L"de");
        else if (command == COMMAND_JAPANESE)
            SetLanguage(L"ja");
        else if (command == COMMAND_EXIT)
            DestroyWindow(window);
        else if (command == COMMAND_OPEN_FOLDER)
            ShellExecuteW(window, L"open", appDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (command == COMMAND_START_WITH_WINDOWS)
            ToggleStartWithWindows(window);
        else if (command == COMMAND_PROMPT_NEW_DEVICES)
        {
            promptForNewDevices = !promptForNewDevices;
            SaveSettings();
        }
        else if (command >= COMMAND_DEVICE_BASE)
            ToggleRegisteredDevice(command);

        menuDeviceCommands.clear();
    }

    bool MeasureDeviceMenuItem(MEASUREITEMSTRUCT* item)
    {
        if (!item || item->CtlType != ODT_MENU || !item->itemData)
            return false;

        const DeviceGroup* group = reinterpret_cast<const DeviceGroup*>(item->itemData);
        HDC dc = GetDC(mainWindow);
        if (!dc)
            return false;
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ previousFont = SelectObject(dc, font);
        SIZE textSize = {};
        GetTextExtentPoint32W(dc, group->name.c_str(),
                              static_cast<int>(group->name.size()), &textSize);
        if (previousFont)
            SelectObject(dc, previousFont);
        ReleaseDC(mainWindow, dc);

        item->itemWidth = static_cast<UINT>(textSize.cx + 60);
        item->itemHeight = static_cast<UINT>((std::max)(
            (std::max)(GetSystemMetrics(SM_CYMENU),
                       static_cast<int>(textSize.cy) + 6), 22));
        return true;
    }

    void DrawCurrentDeviceIndicator(HDC dc, const RECT& row)
    {
        const int centerY = (row.top + row.bottom) / 2;
        const int left = row.left + 29;
        const bool awake = activeKeepAliveStreams.load() > 0;
        HICON statusIcon = awake ? menuIconAwake : menuIconIdle;
        if (statusIcon)
            DrawIconEx(dc, left - 2, centerY - 10, statusIcon, 20, 20,
                       0, nullptr, DI_NORMAL);
    }

    bool DrawDeviceMenuItem(DRAWITEMSTRUCT* item)
    {
        if (!item || item->CtlType != ODT_MENU || !item->itemData)
            return false;

        const DeviceGroup* group = reinterpret_cast<const DeviceGroup*>(item->itemData);
        const bool selected = (item->itemState & ODS_SELECTED) != 0;
        const bool disabled = (item->itemState & ODS_DISABLED) != 0;
        FillRect(item->hDC, &item->rcItem,
                 GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_MENU));

        if (item->itemState & ODS_CHECKED)
        {
            RECT checkRect = item->rcItem;
            checkRect.left += 4;
            checkRect.right = checkRect.left + 18;
            checkRect.top += 2;
            checkRect.bottom -= 2;
            DrawFrameControl(item->hDC, &checkRect, DFC_MENU,
                             DFCS_MENUCHECK | (disabled ? DFCS_INACTIVE : 0));
        }

        if (group->current)
            DrawCurrentDeviceIndicator(item->hDC, item->rcItem);

        SetBkMode(item->hDC, TRANSPARENT);
        const COLORREF textColor = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) :
            (disabled ? GetSysColor(COLOR_GRAYTEXT) : GetSysColor(COLOR_MENUTEXT));
        SetTextColor(item->hDC, textColor);
        RECT textRect = item->rcItem;
        textRect.left += 50;
        textRect.right -= 4;
        DrawTextW(item->hDC, group->name.c_str(),
                  static_cast<int>(group->name.size()), &textRect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        if (item->itemState & ODS_FOCUS)
            DrawFocusRect(item->hDC, &item->rcItem);
        return true;
    }

    LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case TRAY_CALLBACK:
            if (lParam == WM_LBUTTONDBLCLK)
            {
                ShellExecuteW(window, L"open", appDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU || lParam == NIN_KEYSELECT)
                ShowTrayMenu(window);
            return 0;

        case WM_MEASUREITEM:
            if (wParam == 0 && MeasureDeviceMenuItem(
                    reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)))
                return TRUE;
            break;

        case WM_DRAWITEM:
            if (wParam == 0 && DrawDeviceMenuItem(
                    reinterpret_cast<DRAWITEMSTRUCT*>(lParam)))
                return TRUE;
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case COMMAND_ENGLISH: SetLanguage(L"en"); return 0;
            case COMMAND_SPANISH: SetLanguage(L"es"); return 0;
            case COMMAND_GERMAN: SetLanguage(L"de"); return 0;
            case COMMAND_JAPANESE: SetLanguage(L"ja"); return 0;
            case COMMAND_EXIT: DestroyWindow(window); return 0;
            }
            if (LOWORD(wParam) >= COMMAND_DEVICE_BASE)
            {
                ToggleRegisteredDevice(LOWORD(wParam));
                return 0;
            }
            break;

        case NEW_DEVICE_MESSAGE:
            HandleNewDevice(reinterpret_cast<PendingDevice*>(lParam));
            return 0;

        case WAKE_STATUS_MESSAGE:
            UpdateTrayIcon();
            return 0;

        case WM_TIMER:
            if (wParam == DEVICE_SCAN_TIMER)
            {
                DiscoverOutputDevices();
                return 0;
            }
            break;

        case WM_DESTROY:
            running.store(false);
            KillTimer(window, DEVICE_SCAN_TIMER);
            if (trayIconAdded)
            {
                Shell_NotifyIconW(NIM_DELETE, &trayIcon);
                trayIconAdded = false;
            }
            if (trayIconAwake)
            {
                DestroyIcon(trayIconAwake);
                trayIconAwake = nullptr;
            }
            if (trayIconIdle)
            {
                DestroyIcon(trayIconIdle);
                trayIconIdle = nullptr;
            }
            if (menuIconAwake)
            {
                DestroyIcon(menuIconAwake);
                menuIconAwake = nullptr;
            }
            if (menuIconIdle)
            {
                DestroyIcon(menuIconIdle);
                menuIconIdle = nullptr;
            }
            {
                MSG queued = {};
                while (PeekMessageW(&queued, window, NEW_DEVICE_MESSAGE,
                                    NEW_DEVICE_MESSAGE, PM_REMOVE))
                    delete reinterpret_cast<PendingDevice*>(queued.lParam);
            }
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    std::wstring GetPropertyString(IPropertyStore* properties, const PROPERTYKEY& key)
    {
        PROPVARIANT value;
        PropVariantInit(&value);
        std::wstring result;
        if (SUCCEEDED(properties->GetValue(key, &value)) &&
            value.vt == VT_LPWSTR && value.pwszVal)
            result = value.pwszVal;
        PropVariantClear(&value);
        return result;
    }

    struct DeviceInfo
    {
        std::wstring id;
        std::wstring name;
    };

    DeviceInfo GetDeviceInfo(IMMDevice* device)
    {
        DeviceInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id)
        {
            info.id = id;
            CoTaskMemFree(id);
        }

        IPropertyStore* properties = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties)))
        {
            info.name = GetPropertyString(properties, FRIENDLY_NAME_KEY);
            properties->Release();
        }
        if (info.name.empty())
            info.name = L"Unnamed output device";

        return info;
    }

    bool IsDeviceEnabled(const std::wstring& id)
    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        const auto found = std::find_if(
            registeredDevices.begin(), registeredDevices.end(),
            [&id](const RegisteredDevice& device) { return device.id == id; });
        return found != registeredDevices.end() && found->enabled;
    }

    std::wstring ReplaceToken(std::wstring text, const std::wstring& token,
                              const std::wstring& value)
    {
        const size_t position = text.find(token);
        if (position != std::wstring::npos)
            text.replace(position, token.size(), value);
        return text;
    }

    void DiscoverOutputDevices()
    {
        if (!discoveryEnumerator || !running.load())
            return;

        IMMDeviceCollection* collection = nullptr;
        if (FAILED(discoveryEnumerator->EnumAudioEndpoints(
                eRender, DEVICE_STATE_ACTIVE, &collection)))
            return;

        UINT count = 0;
        if (FAILED(collection->GetCount(&count)))
        {
            collection->Release();
            return;
        }

        bool settingsChanged = false;
        for (UINT index = 0; index < count && running.load(); ++index)
        {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(index, &device)))
                continue;

            const DeviceInfo info = GetDeviceInfo(device);
            device->Release();
            if (info.id.empty())
                continue;

            PendingDevice* pending = nullptr;
            {
                std::lock_guard<std::mutex> lock(devicesMutex);
                const auto known = std::find_if(
                    registeredDevices.begin(), registeredDevices.end(),
                    [&info](const RegisteredDevice& item) { return item.id == info.id; });
                if (known != registeredDevices.end())
                {
                    if (known->name != info.name)
                    {
                        known->name = info.name;
                        bool groupEnabled = false;
                        FindGroupEnabledLocked(info.name, groupEnabled);
                        groupEnabled = groupEnabled || known->enabled;
                        SetGroupEnabledLocked(info.name, groupEnabled);
                        settingsChanged = true;
                    }
                }
                else
                {
                    bool groupEnabled = false;
                    if (FindGroupEnabledLocked(info.name, groupEnabled))
                    {
                        const auto unbound = std::find_if(
                            registeredDevices.begin(), registeredDevices.end(),
                            [&info](const RegisteredDevice& item)
                            {
                                return item.id.empty() &&
                                       SameDeviceName(item.name, info.name);
                            });
                        if (unbound != registeredDevices.end())
                        {
                            unbound->id = info.id;
                            unbound->name = info.name;
                            unbound->enabled = groupEnabled;
                        }
                        else
                        {
                            RegisteredDevice entry;
                            entry.id = info.id;
                            entry.name = info.name;
                            entry.enabled = groupEnabled;
                            registeredDevices.push_back(entry);
                        }
                    }
                    else if (pendingDeviceIds.find(info.id) == pendingDeviceIds.end() &&
                             std::none_of(pendingDeviceNames.begin(), pendingDeviceNames.end(),
                                 [&info](const std::wstring& name)
                                 {
                                     return SameDeviceName(name, info.name);
                                 }))
                    {
                        pendingDeviceIds.insert(info.id);
                        pendingDeviceNames.push_back(info.name);
                        pending = new PendingDevice{info.id, info.name};
                    }
                }
            }

            if (pending && !PostMessageW(mainWindow, NEW_DEVICE_MESSAGE, 0,
                                         reinterpret_cast<LPARAM>(pending)))
            {
                std::lock_guard<std::mutex> lock(devicesMutex);
                pendingDeviceIds.erase(info.id);
                pendingDeviceNames.erase(
                    std::remove_if(pendingDeviceNames.begin(), pendingDeviceNames.end(),
                        [&info](const std::wstring& name)
                        {
                            return SameDeviceName(name, info.name);
                        }),
                    pendingDeviceNames.end());
                delete pending;
            }
        }

        collection->Release();
        if (settingsChanged)
            SaveSettings();
    }

    void HandleNewDevice(PendingDevice* device)
    {
        if (!device)
            return;

        bool alreadyRegistered = false;
        bool saved = false;
        {
            std::lock_guard<std::mutex> lock(devicesMutex);
            const auto exact = std::find_if(
                registeredDevices.begin(), registeredDevices.end(),
                [device](const RegisteredDevice& item) { return item.id == device->id; });
            alreadyRegistered = exact != registeredDevices.end();
            if (!alreadyRegistered)
            {
                bool groupEnabled = false;
                if (FindGroupEnabledLocked(device->name, groupEnabled))
                {
                    RegisteredDevice entry;
                    entry.id = device->id;
                    entry.name = device->name;
                    entry.enabled = groupEnabled;
                    registeredDevices.push_back(entry);
                    alreadyRegistered = true;
                    saved = true;
                }
            }
        }

        if (!alreadyRegistered && running.load())
        {
            int answer = IDNO;
            if (promptForNewDevices)
            {
                std::wstring prompt = ReplaceToken(
                    Text(L"new_device_question"), L"{device}", device->name);
                answer = MessageBoxW(
                    mainWindow, prompt.c_str(), Text(L"menu_title").c_str(),
                    MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST);
            }

            {
                std::lock_guard<std::mutex> lock(devicesMutex);
                const auto found = std::find_if(
                    registeredDevices.begin(), registeredDevices.end(),
                    [device](const RegisteredDevice& item) { return item.id == device->id; });
                if (found == registeredDevices.end())
                {
                    SetGroupEnabledLocked(device->name, answer == IDYES);
                    RegisteredDevice entry;
                    entry.id = device->id;
                    entry.name = device->name;
                    entry.enabled = answer == IDYES;
                    registeredDevices.push_back(entry);
                }
                pendingDeviceIds.erase(device->id);
                pendingDeviceNames.erase(
                    std::remove_if(pendingDeviceNames.begin(), pendingDeviceNames.end(),
                        [device](const std::wstring& name)
                        {
                            return SameDeviceName(name, device->name);
                        }),
                    pendingDeviceNames.end());
            }
            delete device;
            SaveSettings();
            return;
        }

        {
            std::lock_guard<std::mutex> lock(devicesMutex);
            pendingDeviceIds.erase(device->id);
            pendingDeviceNames.erase(
                std::remove_if(pendingDeviceNames.begin(), pendingDeviceNames.end(),
                    [device](const std::wstring& name)
                    {
                        return SameDeviceName(name, device->name);
                    }),
                pendingDeviceNames.end());
        }
        delete device;
        if (saved)
            SaveSettings();
    }

    void ToggleRegisteredDevice(UINT command)
    {
        const auto selected = menuDeviceCommands.find(command);
        if (selected == menuDeviceCommands.end())
            return;

        const std::wstring name = selected->second;
        {
            std::lock_guard<std::mutex> lock(devicesMutex);
            bool enabled = false;
            if (!FindGroupEnabledLocked(name, enabled))
                return;
            SetGroupEnabledLocked(name, !enabled);
        }
        SaveSettings();
    }

    bool PlayKeepAlive(IMMDeviceEnumerator* enumerator, const std::wstring& deviceId)
    {
        IMMDevice* device = nullptr;
        IAudioClient* audio = nullptr;
        IAudioRenderClient* render = nullptr;
        WAVEFORMATEX* format = nullptr;
        bool started = false;
        bool countedAsActive = false;

        HRESULT hr = enumerator->GetDevice(deviceId.c_str(), &device);
        if (FAILED(hr))
            return false;

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&audio));
        if (FAILED(hr)) goto cleanup;

        hr = audio->GetMixFormat(&format);
        if (FAILED(hr)) goto cleanup;

        hr = audio->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, format, nullptr);
        if (FAILED(hr)) goto cleanup;

        hr = audio->GetService(__uuidof(IAudioRenderClient),
                               reinterpret_cast<void**>(&render));
        if (FAILED(hr)) goto cleanup;

        hr = audio->Start();
        if (FAILED(hr)) goto cleanup;
        started = true;
        countedAsActive = true;
        if (activeKeepAliveStreams.fetch_add(1) == 0 && mainWindow)
            PostMessageW(mainWindow, WAKE_STATUS_MESSAGE, 0, 0);

        while (running.load() && IsDeviceEnabled(deviceId))
        {
            UINT32 bufferFrames = 0;
            UINT32 padding = 0;
            hr = audio->GetBufferSize(&bufferFrames);
            if (FAILED(hr)) break;
            hr = audio->GetCurrentPadding(&padding);
            if (FAILED(hr)) break;
            if (padding > bufferFrames) break;

            const UINT32 available = bufferFrames - padding;
            if (available > 0)
            {
                BYTE* data = nullptr;
                hr = render->GetBuffer(available, &data);
                if (FAILED(hr)) break;
                hr = render->ReleaseBuffer(available, AUDCLNT_BUFFERFLAGS_SILENT);
                if (FAILED(hr)) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

    cleanup:
        if (countedAsActive && activeKeepAliveStreams.fetch_sub(1) == 1 && mainWindow)
            PostMessageW(mainWindow, WAKE_STATUS_MESSAGE, 0, 0);
        if (audio && started)
            audio->Stop();
        if (render) render->Release();
        if (format) CoTaskMemFree(format);
        if (audio) audio->Release();
        if (device) device->Release();
        return SUCCEEDED(hr);
    }

    void DeviceWorker(const std::wstring& deviceId)
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr))
        {
            OutputDebugStringW(L"BT Speaker Keep-Alive: COM initialization failed.\n");
            return;
        }

        IMMDeviceEnumerator* enumerator = nullptr;
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr))
        {
            OutputDebugStringW(L"BT Speaker Keep-Alive: audio enumerator creation failed.\n");
            CoUninitialize();
            return;
        }

        while (running.load() && IsDeviceEnabled(deviceId))
        {
            PlayKeepAlive(enumerator, deviceId);

            for (int i = 0; i < 10 && running.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        enumerator->Release();
        CoUninitialize();
    }

    struct ActiveDeviceWorker
    {
        std::wstring id;
        std::shared_ptr<std::atomic<bool>> finished;
        std::chrono::steady_clock::time_point started;
        std::thread worker;
    };

    void AudioWorker()
    {
        std::vector<ActiveDeviceWorker> workers;
        while (running.load())
        {
            std::vector<std::wstring> enabledIds;
            {
                std::lock_guard<std::mutex> lock(devicesMutex);
                for (const RegisteredDevice& device : registeredDevices)
                {
                    if (device.enabled)
                        enabledIds.push_back(device.id);
                }
            }

            for (const std::wstring& id : enabledIds)
            {
                const auto existing = std::find_if(
                    workers.begin(), workers.end(),
                    [&id](const ActiveDeviceWorker& item) { return item.id == id; });
                if (existing != workers.end())
                    continue;

                ActiveDeviceWorker task;
                task.id = id;
                task.finished = std::make_shared<std::atomic<bool>>(false);
                task.started = std::chrono::steady_clock::now();
                const std::shared_ptr<std::atomic<bool>> finished = task.finished;
                task.worker = std::thread([id, finished]()
                {
                    DeviceWorker(id);
                    finished->store(true);
                });
                workers.push_back(std::move(task));
            }

            const auto now = std::chrono::steady_clock::now();
            auto task = workers.begin();
            while (task != workers.end())
            {
                const bool disabled = !IsDeviceEnabled(task->id);
                const bool finished = task->finished->load();
                const bool retryDelayPassed = now - task->started >= std::chrono::seconds(1);
                if (disabled || (finished && retryDelayPassed))
                {
                    if (task->worker.joinable())
                        task->worker.join();
                    task = workers.erase(task);
                }
                else
                {
                    ++task;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        for (ActiveDeviceWorker& task : workers)
        {
            if (task.worker.joinable())
                task.worker.join();
        }
    }

    bool AddTrayIcon(HWND window, HINSTANCE instance)
    {
        trayIcon = {};
        trayIcon.cbSize = sizeof(trayIcon);
        trayIcon.hWnd = window;
        trayIcon.uID = 1;
        trayIcon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        trayIcon.uCallbackMessage = TRAY_CALLBACK;
        trayIconAwake = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(TRAY_ACTIVE_ICON_RESOURCE),
            IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
        trayIconIdle = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(TRAY_IDLE_ICON_RESOURCE),
            IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
        menuIconAwake = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(TRAY_ACTIVE_ICON_RESOURCE),
            IMAGE_ICON, 20, 20, LR_DEFAULTCOLOR));
        menuIconIdle = static_cast<HICON>(LoadImageW(
            instance, MAKEINTRESOURCEW(TRAY_IDLE_ICON_RESOURCE),
            IMAGE_ICON, 20, 20, LR_DEFAULTCOLOR));
        const bool awake = activeKeepAliveStreams.load() > 0;
        trayIcon.hIcon = awake ? trayIconAwake : trayIconIdle;
        if (!trayIcon.hIcon)
            trayIcon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        const std::wstring tip = Text(awake ? L"tray_tip_active" : L"tray_tip_idle");
        std::wcsncpy(trayIcon.szTip, tip.c_str(), ARRAYSIZE(trayIcon.szTip) - 1);
        trayIcon.szTip[ARRAYSIZE(trayIcon.szTip) - 1] = L'\0';
        trayIconAdded = Shell_NotifyIconW(NIM_ADD, &trayIcon) != FALSE;
        return trayIconAdded;
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int)
{
    HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult))
    {
        MessageBoxW(nullptr, L"Could not initialize Windows audio services.",
                    L"BT Speaker Keep-Alive", MB_ICONERROR | MB_OK);
        return 1;
    }

    comResult = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator),
                                 reinterpret_cast<void**>(&discoveryEnumerator));
    if (FAILED(comResult))
    {
        MessageBoxW(nullptr, L"Could not create the audio device enumerator.",
                    L"BT Speaker Keep-Alive", MB_ICONERROR | MB_OK);
        CoUninitialize();
        return 1;
    }

    appDirectory = ExecutableDirectory();
    LoadSettings();
    // Refresh an existing startup command so it points to the current exe path.
    if (IsStartWithWindowsEnabled())
        SetStartWithWindowsEnabled(true);

    if (!LoadTranslationFile(language))
    {
        language = L"en";
        LoadTranslationFile(language);
    }

    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = WINDOW_CLASS;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&windowClass))
    {
        MessageBoxW(nullptr, L"Could not register the tray window.",
                    L"BT Speaker Keep-Alive", MB_ICONERROR | MB_OK);
        discoveryEnumerator->Release();
        CoUninitialize();
        return 1;
    }

    mainWindow = CreateWindowExW(
        0, WINDOW_CLASS, L"BT Speaker Keep-Alive", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!mainWindow)
    {
        MessageBoxW(nullptr, L"Could not create the tray window.",
                    L"BT Speaker Keep-Alive", MB_ICONERROR | MB_OK);
        discoveryEnumerator->Release();
        CoUninitialize();
        return 1;
    }

    if (!AddTrayIcon(mainWindow, instance))
    {
        MessageBoxW(nullptr, L"Could not add the icon to the notification area.",
                    L"BT Speaker Keep-Alive", MB_ICONERROR | MB_OK);
        DestroyWindow(mainWindow);
        discoveryEnumerator->Release();
        CoUninitialize();
        return 1;
    }

    SetTimer(mainWindow, DEVICE_SCAN_TIMER, 2000, nullptr);
    DiscoverOutputDevices();

    std::thread audioThread(AudioWorker);
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    running.store(false);
    if (audioThread.joinable())
        audioThread.join();
    discoveryEnumerator->Release();
    CoUninitialize();
    return 0;
}
