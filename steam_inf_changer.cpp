#include "imgui/imgui.h"
#include "imgui/imgui_impl_win32.h"
#include "imgui/imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <regex>
#include <vector>
#include <commdlg.h>

namespace fs = std::filesystem;

static HWND g_hwnd = nullptr;

std::vector<std::wstring> GetLogicalDrivesList() {
    std::vector<std::wstring> drives;
    DWORD size = GetLogicalDriveStringsW(0, nullptr);
    if (size == 0) return drives;
    
    std::vector<wchar_t> buffer(size);
    if (GetLogicalDriveStringsW(size, buffer.data()) == 0) return drives;
    
    wchar_t* drive = buffer.data();
    while (*drive) {
        drives.push_back(drive);
        drive += wcslen(drive) + 1;
    }
    return drives;
}

fs::path SearchDrivesForSteamInf() {
    std::vector<std::wstring> drives = GetLogicalDrivesList();
    std::vector<std::wstring> common_subpaths = {
        L"SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\csgo\\steam.inf",
        L"Steam\\steamapps\\common\\Counter-Strike Global Offensive\\csgo\\steam.inf",
        L"Program Files\\Steam\\steamapps\\common\\Counter-Strike Global Offensive\\csgo\\steam.inf",
        L"Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive\\csgo\\steam.inf",
        L"Games\\Steam\\steamapps\\common\\Counter-Strike Global Offensive\\csgo\\steam.inf",
        L"Games\\SteamLibrary\\steamapps\\common\\Counter-Strike Global Offensive\\csgo\\steam.inf"
    };

    for (const auto& drive : drives) {
        for (const auto& subpath : common_subpaths) {
            fs::path p(drive);
            p /= subpath;
            if (fs::exists(p)) {
                return p;
            }
        }
    }
    return "";
}

fs::path BrowseForSteamInf(HWND hwndOwner) {
    wchar_t szFile[260] = { 0 };
    OPENFILENAMEW ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwndOwner;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = L"steam.inf\0steam.inf\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFileTitle = nullptr;
    ofn.nMaxFileTitle = 0;
    ofn.lpstrInitialDir = nullptr;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameW(&ofn) == TRUE) {
        return fs::path(szFile);
    }
    return "";
}

// Global DirectX 11 pointers
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// Helper function declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Global application variables
fs::path TARGET_PATH;
fs::path BACKUP_PATH;
std::string current_client_ver = "Unknown";
std::string current_server_ver = "Unknown";
std::string backup_status = "Unknown";
std::string path_display = "Searching...";
std::string status_message = "Ready";
int status_code = 0; // 0 = Info (Gray), 1 = Success (Green), 2 = Error (Red)

// Helper to convert wstring to string
std::string wstring_to_string(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

// Truncate path to fit nicely in UI without scrollbars
std::string GetShortPath(const std::string& full_path) {
    if (full_path.length() <= 40) return full_path;
    size_t pos = full_path.find("steamapps");
    if (pos != std::string::npos) {
        return "..." + full_path.substr(pos);
    }
    return "..." + full_path.substr(full_path.length() - 30);
}

// Registry helper
std::wstring GetRegistryString(HKEY hKeyParent, const std::wstring& subKey, const std::wstring& valueName) {
    HKEY hKey;
    std::wstring result = L"";
    if (RegOpenKeyExW(hKeyParent, subKey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buffer[512];
        DWORD bufferSize = sizeof(buffer);
        DWORD type;
        if (RegQueryValueExW(hKey, valueName.c_str(), NULL, &type, (LPBYTE)buffer, &bufferSize) == ERROR_SUCCESS) {
            if (type == REG_SZ || type == REG_EXPAND_SZ) {
                result = std::wstring(buffer);
            }
        }
        RegCloseKey(hKey);
    }
    return result;
}

// Find path dynamically
fs::path FindSteamInfPath() {
    // 1. Try Uninstall keys directly for CS:GO (App ID 730)
    std::vector<std::pair<HKEY, std::wstring>> reg_keys = {
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 730"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 730"},
    };

    for (const auto& entry : reg_keys) {
        std::wstring install_loc = GetRegistryString(entry.first, entry.second, L"InstallLocation");
        if (!install_loc.empty()) {
            fs::path p(install_loc);
            p /= L"csgo\\steam.inf";
            if (fs::exists(p)) {
                return p;
            }
        }
    }

    // 2. Try Steam path from HKCU or HKLM to scan libraries
    std::vector<std::pair<HKEY, std::wstring>> steam_keys = {
        {HKEY_CURRENT_USER, L"Software\\Valve\\Steam"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Wow6432Node\\Valve\\Steam"},
        {HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam"}
    };

    for (const auto& entry : steam_keys) {
        std::wstring steam_path = GetRegistryString(entry.first, entry.second, entry.first == HKEY_CURRENT_USER ? L"SteamPath" : L"InstallPath");
        if (!steam_path.empty()) {
            fs::path p(steam_path);
            
            // Check default location under this Steam path
            fs::path default_game_path = p / L"steamapps\\common\\Counter-Strike Global Offensive\\csgo\\steam.inf";
            if (fs::exists(default_game_path)) {
                return default_game_path;
            }

            // Check libraryfolders.vdf (Steam library list)
            fs::path vdf_path = p / L"steamapps\\libraryfolders.vdf";
            if (fs::exists(vdf_path)) {
                std::ifstream vdf_file(vdf_path);
                if (vdf_file.is_open()) {
                    std::string line;
                    std::regex path_regex(R"raw("path"\s+"([^"]+)")raw");
                    while (std::getline(vdf_file, line)) {
                        std::smatch match;
                        if (std::regex_search(line, match, path_regex)) {
                            std::string path_str = match[1].str();
                            fs::path lib_path(path_str);
                            fs::path check_path = lib_path / L"steamapps\\common\\Counter-Strike Global Offensive\\csgo\\steam.inf";
                            if (fs::exists(check_path)) {
                                return check_path;
                            }
                        }
                    }
                }
            }
        }
    }

    // 3. Fallback to hardcoded default path
    fs::path fallback(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Counter-Strike Global Offensive\\csgo\\steam.inf");
    if (fs::exists(fallback)) {
        return fallback;
    }

    // 4. Try scanning all active drives for common steam directories
    fs::path drive_path = SearchDrivesForSteamInf();
    if (!drive_path.empty()) {
        return drive_path;
    }

    return "";
}

// Refresh state variables
void UpdateStatusUI() {
    if (TARGET_PATH.empty() || !fs::exists(TARGET_PATH)) {
        TARGET_PATH = FindSteamInfPath();
    }
    if (TARGET_PATH.empty() || !fs::exists(TARGET_PATH)) {
        path_display = "steam.inf not found!";
        current_client_ver = "N/A";
        current_server_ver = "N/A";
        backup_status = "N/A";
        status_message = "Error: steam.inf location not found!";
        status_code = 2;
        return;
    }

    BACKUP_PATH = TARGET_PATH;
    BACKUP_PATH += L".bak";
    
    std::string full_path = wstring_to_string(TARGET_PATH.wstring());
    path_display = GetShortPath(full_path);

    std::ifstream infile(TARGET_PATH, std::ios::in | std::ios::binary);
    if (!infile.is_open()) {
        current_client_ver = "Error";
        current_server_ver = "Error";
        status_message = "Error: Failed to read file. Run as Admin.";
        status_code = 2;
        return;
    }

    std::string content((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());
    infile.close();

    std::regex client_regex("ClientVersion=(\\d+)");
    std::regex server_regex("ServerVersion=(\\d+)");
    std::smatch match;

    if (std::regex_search(content, match, client_regex)) {
        current_client_ver = match[1].str();
    } else {
        current_client_ver = "Unknown";
    }

    if (std::regex_search(content, match, server_regex)) {
        current_server_ver = match[1].str();
    } else {
        current_server_ver = "Unknown";
    }

    bool backup_exists = fs::exists(BACKUP_PATH);
    backup_status = backup_exists ? "OK" : "None";
}

// Action commands
void OnUpdate() {
    if (TARGET_PATH.empty() || !fs::exists(TARGET_PATH)) {
        status_message = "Error: steam.inf file not found!";
        status_code = 2;
        return;
    }

    if (!fs::exists(BACKUP_PATH)) {
        try {
            fs::copy_file(TARGET_PATH, BACKUP_PATH);
        } catch (...) {
            status_message = "Error: Failed to create backup! Run as Admin.";
            status_code = 2;
            return;
        }
    }

    std::ifstream infile(TARGET_PATH, std::ios::in | std::ios::binary);
    if (!infile.is_open()) {
        status_message = "Error: Failed to read steam.inf! Run as Admin.";
        status_code = 2;
        return;
    }
    std::string content((std::istreambuf_iterator<char>(infile)), std::istreambuf_iterator<char>());
    infile.close();

    std::string updated = std::regex_replace(content, std::regex("ClientVersion=\\d+"), "ClientVersion=2000258");
    updated = std::regex_replace(updated, std::regex("ServerVersion=\\d+"), "ServerVersion=2000258");

    std::ofstream outfile(TARGET_PATH, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!outfile.is_open()) {
        status_message = "Error: Failed to write steam.inf! Run as Admin.";
        status_code = 2;
        return;
    }
    outfile.write(updated.c_str(), updated.length());
    outfile.close();

    status_message = "Version changed to 2000258!";
    status_code = 1;
    UpdateStatusUI();
}

void OnRestore() {
    if (BACKUP_PATH.empty() || !fs::exists(BACKUP_PATH)) {
        status_message = "Error: No backup file (.bak) found to restore from!";
        status_code = 2;
        return;
    }

    try {
        fs::copy_file(BACKUP_PATH, TARGET_PATH, fs::copy_options::overwrite_existing);
        status_message = "Version restored!";
        status_code = 1;
    } catch (...) {
        status_message = "Error: Failed to restore backup! Run as Admin.";
        status_code = 2;
    }
    UpdateStatusUI();
}

// Main entry point
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Register window
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"ImGuiChangerClass", nullptr };
    RegisterClassExW(&wc);
    
    // Adjust window rect to get precise client area of 480x200
    RECT rect = { 0, 0, 480, 200 };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME, FALSE);

    HWND hwnd = CreateWindowW(
        L"ImGuiChangerClass", 
        L"STEAM.INF VERSION CHANGER", 
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME, // Non-resizable border
        CW_USEDEFAULT, CW_USEDEFAULT, 
        rect.right - rect.left, rect.bottom - rect.top, 
        nullptr, nullptr, hInstance, nullptr
    );

    g_hwnd = hwnd;

    // Initialize D3D11
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    // Modern Premium Obsidian & Crimson Red Palette
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.GrabRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ItemSpacing = ImVec2(10.0f, 6.0f);
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.09f, 0.12f, 1.0f);
    style.Colors[ImGuiCol_Border] = ImVec4(0.18f, 0.22f, 0.28f, 1.0f);
    style.Colors[ImGuiCol_ChildBg] = ImVec4(0.12f, 0.15f, 0.19f, 0.8f);
    style.Colors[ImGuiCol_Separator] = ImVec4(0.18f, 0.22f, 0.28f, 1.0f);
    style.Colors[ImGuiCol_Text] = ImVec4(0.94f, 0.96f, 0.98f, 1.0f);
    style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.60f, 0.70f, 1.0f);
    
    style.Colors[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.11f, 0.14f, 1.0f);
    style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.13f, 0.16f, 0.21f, 1.0f);
    style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.18f, 0.22f, 0.29f, 1.0f);

    style.Colors[ImGuiCol_Button] = ImVec4(0.16f, 0.21f, 0.28f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.29f, 0.38f, 1.0f);
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.12f, 0.16f, 0.22f, 1.0f);

    // Setup backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Load fonts
    ImFont* fontTitle = nullptr;
    ImFont* fontRegular = nullptr;
    ImFont* fontSubtitle = nullptr;
    
    if (fs::exists("C:\\Windows\\Fonts\\segoeuib.ttf")) {
        fontTitle = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeuib.ttf", 20.0f);
    } else {
        fontTitle = io.Fonts->AddFontDefault();
    }
    if (fs::exists("C:\\Windows\\Fonts\\segoeui.ttf")) {
        fontRegular = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 15.0f);
        fontSubtitle = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 13.0f);
    } else {
        fontRegular = io.Fonts->AddFontDefault();
        fontSubtitle = io.Fonts->AddFontDefault();
    }

    UpdateStatusUI();

    // App Loop
    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Pin ImGui frame exactly to client area bounds
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(480, 200));
        
        // Remove scrollbars and decoration flags to keep it clean and compact
        ImGui::Begin("Main", nullptr, 
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | 
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | 
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        // Header Title
        ImGui::PushFont(fontTitle);
        ImGui::TextColored(ImVec4(0.89f, 0.12f, 0.21f, 1.0f), "STEAM.INF VERSION CHANGER");
        ImGui::PopFont();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Info child card panel (Height fits perfectly, scrollbars disabled)
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.15f, 0.19f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        
        // height of child panel set to 66 to fit 3 items cleanly
        ImGui::BeginChild("InfoCard", ImVec2(0, 68), true, 
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        
        ImGui::PushFont(fontRegular);
        
        ImGui::Text("Path:");
        ImGui::SameLine();
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 1));
        if (ImGui::Button("Browse")) {
            fs::path chosen = BrowseForSteamInf(g_hwnd);
            if (!chosen.empty() && fs::exists(chosen)) {
                TARGET_PATH = chosen;
                status_message = "Loaded path manually.";
                status_code = 0;
                UpdateStatusUI();
            }
        }
        ImGui::PopStyleVar(2);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", path_display.c_str());

        ImGui::Text("Version:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.89f, 0.12f, 0.21f, 1.0f), "%s", current_client_ver.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
        ImGui::Text("Server:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.89f, 0.12f, 0.21f, 1.0f), "%s", current_server_ver.c_str());


        
        ImGui::PopFont();
        ImGui::EndChild();
        
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        ImGui::Spacing();

        // Control Buttons side-by-side
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        ImGui::PushFont(fontRegular);

        // Update Button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.89f, 0.12f, 0.21f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1.00f, 0.22f, 0.31f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.72f, 0.07f, 0.14f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.07f, 0.09f, 0.12f, 1.0f));
        
        if (ImGui::Button("Update to 2000258", ImVec2(218, 35))) {
            OnUpdate();
        }
        ImGui::PopStyleColor(4);

        ImGui::SameLine(242);

        // Restore Button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.23f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.24f, 0.31f, 0.40f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.14f, 0.18f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.94f, 0.96f, 0.98f, 1.0f));
        
        if (ImGui::Button("Restore Original (1575)", ImVec2(218, 35))) {
            OnRestore();
        }
        ImGui::PopStyleColor(4);
        
        ImGui::PopFont();
        ImGui::PopStyleVar();

        // Clean single-line status indicator at the bottom
        ImGui::Spacing();
        ImGui::PushFont(fontSubtitle);
        ImGui::TextDisabled("Status:");
        ImGui::SameLine();
        if (status_code == 1) {
            ImGui::TextColored(ImVec4(0.24f, 0.80f, 0.44f, 1.0f), "%s", status_message.c_str());
        } else if (status_code == 2) {
            ImGui::TextColored(ImVec4(0.90f, 0.30f, 0.35f, 1.0f), "%s", status_message.c_str());
        } else {
            ImGui::Text("%s", status_message.c_str());
        }
        ImGui::PopFont();

        ImGui::End();

        // D3D Render
        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.07f, 0.09f, 0.12f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// D3D11 Setup Helpers
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, 
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, 
        &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
    );
    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, 
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, 
            &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
        );
    }
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// WndProc window hook
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_SIZE:
            if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_KEYMENU)
                return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
