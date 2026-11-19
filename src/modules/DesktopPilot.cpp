#include "modules/DesktopPilot.hpp"

#include "core/Log.hpp"

#include <oleauto.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace companion {
namespace {

// Identifiants UI Automation définis ici : pas de dépendance à la bibliothèque qui les fournit (MSVC / MinGW).
constexpr GUID kClsidUia = {0xff48dba4, 0x60ef, 0x4201, {0xaa, 0x87, 0x54, 0x10, 0x3e, 0xef, 0x59, 0x4e}};
constexpr GUID kIidUia = {0x30cbe57d, 0xd9d0, 0x452a, {0xab, 0x13, 0x7a, 0xc5, 0xac, 0x48, 0x25, 0xee}};
// Ses propres frappes : Windows les marque (hDevice nul en Raw Input) ; on ignore AUSSI tout ce qui arrive pendant
// qu'il injecte (+80 ms), pour les systèmes qui ne les marquent pas (Wine, certains pilotes).
std::atomic<ULONGLONG> g_injectUntil{0};
UINT Inject(UINT n, INPUT* in) {
    g_injectUntil = GetTickCount64() + 80;
    const UINT sent = SendInput(n, in, sizeof(INPUT));
    g_injectUntil = GetTickCount64() + 80;
    return sent;
}

constexpr GUID kIidInvoke = {0xfb377fbe, 0x8ea6, 0x46d5, {0x9c, 0x73, 0x64, 0x99, 0x64, 0x2d, 0x30, 0x59}};

std::string Utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Minuscules sans accents (« Paramètres » == « parametres ») pour comparer des noms de boutons.
std::wstring Fold(std::wstring s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        c = static_cast<wchar_t>(std::towlower(c));
        switch (c) {
            case L'é': case L'è': case L'ê': case L'ë': c = L'e'; break;
            case L'à': case L'â': case L'ä': c = L'a'; break;
            case L'î': case L'ï': c = L'i'; break;
            case L'ô': case L'ö': c = L'o'; break;
            case L'ù': case L'û': case L'ü': c = L'u'; break;
            case L'ç': c = L'c'; break;
            case L'&': continue;  // « &Fichier » (accélérateur)
            default: break;
        }
        out += c;
    }
    while (!out.empty() && (out.back() == L' ' || out.back() == L'.')) out.pop_back();
    return out;
}

DWORD PidOf(HWND h) {
    DWORD pid = 0;
    if (h) GetWindowThreadProcessId(h, &pid);
    return pid;
}

// Éléments cliquables visibles (boutons, menus, onglets, éléments de liste, liens…) de la fenêtre.
ComPtr<IUIAutomationElementArray> FindClickables(IUIAutomation* uia, HWND hwnd, bool full) {
    ComPtr<IUIAutomationElement> root;
    if (FAILED(uia->ElementFromHandle(hwnd, &root)) || !root) return nullptr;
    ComPtr<IUIAutomationCondition> types;
    for (const int t : {UIA_ButtonControlTypeId, UIA_MenuItemControlTypeId, UIA_TabItemControlTypeId, UIA_ListItemControlTypeId,
                        UIA_HyperlinkControlTypeId, UIA_CheckBoxControlTypeId, UIA_RadioButtonControlTypeId,
                        UIA_SplitButtonControlTypeId, UIA_TreeItemControlTypeId, UIA_ComboBoxControlTypeId}) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_I4;
        v.lVal = t;
        ComPtr<IUIAutomationCondition> c;
        if (FAILED(uia->CreatePropertyCondition(UIA_ControlTypePropertyId, v, &c))) continue;
        if (!types) {
            types = c;
        } else {
            ComPtr<IUIAutomationCondition> both;
            if (SUCCEEDED(uia->CreateOrCondition(types.Get(), c.Get(), &both))) types = both;
        }
    }
    VARIANT off;
    VariantInit(&off);
    off.vt = VT_BOOL;
    off.boolVal = VARIANT_FALSE;
    ComPtr<IUIAutomationCondition> visible, cond;
    if (!types || FAILED(uia->CreatePropertyCondition(UIA_IsOffscreenPropertyId, off, &visible)) ||
        FAILED(uia->CreateAndCondition(types.Get(), visible.Get(), &cond)))
        return nullptr;
    ComPtr<IUIAutomationCacheRequest> cache;
    if (FAILED(uia->CreateCacheRequest(&cache))) return nullptr;
    cache->AddProperty(UIA_NamePropertyId);
    cache->AddProperty(UIA_BoundingRectanglePropertyId);
    if (!full) cache->put_AutomationElementMode(AutomationElementMode_None);
    ComPtr<IUIAutomationElementArray> found;
    if (FAILED(root->FindAllBuildCache(TreeScope_Descendants, cond.Get(), cache.Get(), &found))) return nullptr;
    return found;
}

ComPtr<IUIAutomation> Uia() {
    ComPtr<IUIAutomation> uia;
    CoCreateInstance(kClsidUia, nullptr, CLSCTX_INPROC_SERVER, kIidUia, reinterpret_cast<void**>(uia.GetAddressOf()));
    return uia;
}

std::wstring CachedName(IUIAutomationElement* e) {
    BSTR b = nullptr;
    std::wstring s;
    if (SUCCEEDED(e->get_CachedName(&b)) && b) s.assign(b, SysStringLen(b));
    SysFreeString(b);
    return s;
}

void MouseTo(int x, int y, DWORD extra = 0) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = std::max(2, GetSystemMetrics(SM_CXVIRTUALSCREEN)), vh = std::max(2, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = static_cast<LONG>((static_cast<double>(x - vx) * 65535.0) / (vw - 1));
    in.mi.dy = static_cast<LONG>((static_cast<double>(y - vy) * 65535.0) / (vh - 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | extra;
    Inject(1, &in);
}

// IDispatch en liaison tardive (AutoCAD n'a pas besoin d'être installé pour compiler).
HRESULT Dispatch(IDispatch* obj, const wchar_t* name, WORD flags, VARIANT* args, UINT nargs, VARIANT* result) {
    DISPID id;
    LPOLESTR n = const_cast<LPOLESTR>(name);
    HRESULT hr = obj->GetIDsOfNames(IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id);
    if (FAILED(hr)) return hr;
    DISPPARAMS p{args, nullptr, nargs, 0};
    for (int attempt = 0; attempt < 10; ++attempt) {  // AutoCAD occupé : « appel rejeté », on réessaie
        hr = obj->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &p, result, nullptr, nullptr);
        if (hr != RPC_E_CALL_REJECTED && hr != RPC_E_SERVERCALL_RETRYLATER) break;
        Sleep(300);
    }
    return hr;
}

}  // namespace

// ---------------------------------------------------------------------------
PilotTarget DesktopPilot::Foreground() {
    PilotTarget t;
    t.hwnd = GetAncestor(GetForegroundWindow(), GA_ROOT);
    if (!t.hwnd) return t;
    t.pid = PidOf(t.hwnd);
    wchar_t buf[512];
    const int n = GetWindowTextW(t.hwnd, buf, 512);
    t.title = Utf8(std::wstring_view(buf, static_cast<size_t>(std::max(n, 0))));
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, t.pid)) {
        DWORD len = 512;
        if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
            std::wstring full(buf, len);
            t.path = full;
            t.exe = full.substr(full.find_last_of(L"\\/") + 1);
            for (auto& c : t.exe) c = static_cast<wchar_t>(std::towlower(c));
        }
        CloseHandle(h);
    }
    if (t.exe.starts_with(L"acad")) t.app = PilotApp::AutoCad;
    else if (t.exe == L"fl64.exe" || t.exe == L"fl.exe") t.app = PilotApp::FlStudio;
    else if (t.exe == L"afterfx.exe") t.app = PilotApp::AfterEffects;
    return t;
}

std::vector<std::string> DesktopPilot::ClickableNames(HWND hwnd, std::size_t max) {
    std::vector<std::string> names;
    const auto uia = Uia();
    if (!uia || !hwnd) return names;
    const auto found = FindClickables(uia.Get(), hwnd, false);
    if (!found) return names;
    int len = 0;
    found->get_Length(&len);
    std::vector<std::wstring> seen;
    for (int i = 0; i < len && names.size() < max; ++i) {
        ComPtr<IUIAutomationElement> e;
        if (FAILED(found->GetElement(i, &e)) || !e) continue;
        std::wstring name = CachedName(e.Get());
        if (name.empty() || name.size() > 40) continue;
        const std::wstring key = Fold(name);
        if (std::ranges::find(seen, key) != seen.end()) continue;
        seen.push_back(key);
        names.push_back(Utf8(name));
    }
    return names;
}

// ---------------------------------------------------------------------------
bool DesktopPilot::SendChord(const KeyChord& k) {
    std::vector<INPUT> in;
    const auto key = [&](WORD vk, bool up) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        const bool extended = (vk >= 0x21 && vk <= 0x28) || vk == 0x2D || vk == 0x2E;  // flèches, début/fin, inser/suppr
        i.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
        in.push_back(i);
    };
    for (auto m : k.mods) key(m, false);
    key(k.vk, false);
    key(k.vk, true);
    for (auto it = k.mods.rbegin(); it != k.mods.rend(); ++it) key(*it, true);
    return Inject(static_cast<UINT>(in.size()), in.data()) == in.size();
}

bool DesktopPilot::TypeText(const std::string& utf8) {
    const std::wstring w = Wide(utf8);
    int n = 0;
    for (wchar_t c : w) {
        INPUT in[2]{};
        for (int k = 0; k < 2; ++k) {
            in[k].type = INPUT_KEYBOARD;
            if (c == L'\n') {
                in[k].ki.wVk = VK_RETURN;
                in[k].ki.dwFlags = k ? KEYEVENTF_KEYUP : 0;
            } else {
                in[k].ki.wScan = c;
                in[k].ki.dwFlags = KEYEVENTF_UNICODE | (k ? KEYEVENTF_KEYUP : 0);
            }
        }
        if (Inject(2, in) != 2) return false;
        if (++n % 16 == 0) Sleep(8);  // ne pas noyer la file de messages de l'appli
    }
    return true;
}

bool DesktopPilot::ClickByName(HWND hwnd, const std::string& name, std::string& error) {
    const auto uia = Uia();
    if (!uia) {
        error = "accessibilité Windows indisponible";
        return false;
    }
    const auto found = FindClickables(uia.Get(), hwnd, true);
    int len = 0;
    if (!found || FAILED(found->get_Length(&len)) || len == 0) {
        error = "cette appli ne décrit pas ses boutons à Windows";
        return false;
    }
    const std::wstring want = Fold(Wide(name));
    ComPtr<IUIAutomationElement> exact, partial;
    size_t partialLen = SIZE_MAX;
    for (int i = 0; i < len && !exact; ++i) {
        ComPtr<IUIAutomationElement> e;
        if (FAILED(found->GetElement(i, &e)) || !e) continue;
        const std::wstring n = Fold(CachedName(e.Get()));
        if (n.empty()) continue;
        if (n == want) exact = e;
        else if (n.find(want) != std::wstring::npos && n.size() < partialLen) {
            partial = e;
            partialLen = n.size();
        }
    }
    ComPtr<IUIAutomationElement> el = exact ? exact : partial;
    if (!el) {
        error = "bouton « " + name + " » introuvable";
        return false;
    }
    // 1) L'élément sait s'invoquer : pas besoin de bouger ta souris.
    ComPtr<IUIAutomationInvokePattern> invoke;
    if (SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId, kIidInvoke,
                                          reinterpret_cast<void**>(invoke.GetAddressOf()))) &&
        invoke && SUCCEEDED(invoke->Invoke()))
        return true;
    // 2) Sinon, vrai clic au centre, puis la souris revient où elle était.
    RECT r{};
    if (FAILED(el->get_CachedBoundingRectangle(&r)) || r.right <= r.left || r.bottom <= r.top) {
        error = "bouton « " + name + " » sans position à l'écran";
        return false;
    }
    POINT back{};
    GetCursorPos(&back);
    const int x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
    MouseTo(x, y);
    Sleep(30);
    MouseTo(x, y, MOUSEEVENTF_LEFTDOWN);
    MouseTo(x, y, MOUSEEVENTF_LEFTUP);
    Sleep(30);
    MouseTo(back.x, back.y);
    return true;
}

bool DesktopPilot::AcadCommand(const std::string& command, std::string& error) {
    CLSID clsid;
    ComPtr<IUnknown> unk;
    ComPtr<IDispatch> app;
    if (FAILED(CLSIDFromProgID(L"AutoCAD.Application", &clsid)) || FAILED(GetActiveObject(clsid, nullptr, &unk)) ||
        FAILED(unk.As(&app))) {
        error = "AutoCAD n'accepte pas l'automatisation (version LT ?)";
        return false;
    }
    VARIANT docV;
    VariantInit(&docV);
    if (FAILED(Dispatch(app.Get(), L"ActiveDocument", DISPATCH_PROPERTYGET, nullptr, 0, &docV)) || docV.vt != VT_DISPATCH ||
        !docV.pdispVal) {
        VariantClear(&docV);
        error = "pas de dessin ouvert dans AutoCAD";
        return false;
    }
    std::string cmd = command;
    if (cmd.empty() || (cmd.back() != ' ' && cmd.back() != '\n')) cmd += '\n';  // la commande part à l'espace / entrée
    VARIANT arg;
    VariantInit(&arg);
    arg.vt = VT_BSTR;
    arg.bstrVal = SysAllocString(Wide(cmd).c_str());
    const HRESULT hr = Dispatch(docV.pdispVal, L"SendCommand", DISPATCH_METHOD, &arg, 1, nullptr);
    VariantClear(&arg);
    VariantClear(&docV);
    if (FAILED(hr)) {
        error = "AutoCAD a refusé la commande (occupé ?)";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
bool DesktopPilot::Ensure(const PilotTarget& t, std::string& error) {
    HWND fg = GetForegroundWindow();
    if (PidOf(fg) == t.pid) return true;  // la fenêtre, ou une de ses boîtes de dialogue
    if (IsWindow(t.hwnd)) {
        SetForegroundWindow(t.hwnd);
        Sleep(80);
        if (PidOf(GetForegroundWindow()) == t.pid) return true;
    }
    error = "la fenêtre a changé, j'arrête pour ne pas taper ailleurs";
    return false;
}

LRESULT CALLBACK DesktopPilot::WatchProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<DesktopPilot*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_INPUT && self) {
        RAWINPUT ri{};
        UINT size = sizeof(ri);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
            ri.header.hDevice != nullptr && GetTickCount64() >= g_injectUntil.load()) {  // hDevice nul = injectée : ignorée
            if (ri.header.dwType == RIM_TYPEKEYBOARD && !(ri.data.keyboard.Flags & RI_KEY_BREAK)) {
                self->userTookOver_ = true;
            } else if (ri.header.dwType == RIM_TYPEMOUSE) {
                constexpr USHORT kDown = RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN | RI_MOUSE_MIDDLE_BUTTON_DOWN |
                                         RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_WHEEL;
                static thread_local long moved = 0;
                if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) moved += std::abs(ri.data.mouse.lLastX) + std::abs(ri.data.mouse.lLastY);
                if ((ri.data.mouse.usButtonFlags & kDown) || moved > 40) {
                    self->userTookOver_ = true;
                    moved = 0;
                }
            }
        }
    } else if (msg == WM_CLOSE) {
        DestroyWindow(h);
        return 0;
    } else if (msg == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void DesktopPilot::WatchMain(std::stop_token st) {
    log::SetCurrentThreadName(L"Companion.PilotWatch");
    static const ATOM cls = [] {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &DesktopPilot::WatchProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"CompanionPilotWatch";
        return RegisterClassW(&wc);
    }();
    HWND h = CreateWindowExW(0, MAKEINTATOM(cls), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!h) return;
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // Raw Input est PAR PROCESSUS : on garde ce qui était enregistré (ex. le governor pendant un entraînement) pour le remettre.
    std::vector<RAWINPUTDEVICE> before;
    UINT count = 0;
    GetRegisteredRawInputDevices(nullptr, &count, sizeof(RAWINPUTDEVICE));
    if (count) {
        before.resize(count);
        if (GetRegisteredRawInputDevices(before.data(), &count, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1)) before.clear();
        else before.resize(count);
    }
    RAWINPUTDEVICE dev[2] = {{0x01, 0x02, RIDEV_INPUTSINK, h}, {0x01, 0x06, RIDEV_INPUTSINK, h}};  // souris, clavier
    RegisterRawInputDevices(dev, 2, sizeof(RAWINPUTDEVICE));
    watchWnd_ = h;
    std::stop_callback onStop(st, [h] { PostMessageW(h, WM_CLOSE, 0, 0); });
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) DispatchMessageW(&m);
    for (const USHORT usage : {USHORT{0x02}, USHORT{0x06}}) {
        const auto it = std::ranges::find_if(before, [&](const RAWINPUTDEVICE& d) { return d.usUsagePage == 0x01 && d.usUsage == usage; });
        RAWINPUTDEVICE r = it != before.end() && it->hwndTarget && IsWindow(it->hwndTarget) ? *it : RAWINPUTDEVICE{0x01, usage, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&r, 1, sizeof(RAWINPUTDEVICE));
    }
    watchWnd_ = nullptr;
}

DesktopPilot::Result DesktopPilot::Run(const PilotTarget& target, const std::vector<PilotStep>& steps,
                                       const std::function<void(const std::string&)>& say, std::stop_token stop,
                                       std::string& error) {
    userTookOver_ = false;
    std::jthread watch([this](std::stop_token st) { WatchMain(st); });
    for (int i = 0; i < 50 && !watchWnd_.load(); ++i) Sleep(10);
    const auto aborted = [&] {
        if (stop.stop_requested()) {
            error = "arrêté";
            return true;
        }
        if (userTookOver_.load()) {
            error = "tu as repris la main";
            return true;
        }
        return false;
    };
    const auto pause = [&](int ms) {
        for (int t = 0; t < ms; t += 25) {
            if (aborted()) return false;
            Sleep(25);
        }
        return !aborted();
    };
    for (const PilotStep& s : steps) {
        if (aborted()) return Result::Aborted;
        switch (s.op) {
            case PilotOp::Say:
                if (say) say(s.arg);
                break;
            case PilotOp::Wait:
                if (!pause(s.ms)) return Result::Aborted;
                break;
            case PilotOp::Key: {
                const auto k = ParseChord(s.arg);
                if (!k) {
                    error = "raccourci inconnu : " + s.arg;
                    return Result::Failed;
                }
                if (!Ensure(target, error)) return Result::Failed;
                if (!SendChord(*k)) {
                    error = "Windows a bloqué le clavier (appli lancée en administrateur ?)";
                    return Result::Failed;
                }
                if (!pause(150)) return Result::Aborted;
                break;
            }
            case PilotOp::Type:
                if (!Ensure(target, error)) return Result::Failed;
                if (!TypeText(s.arg)) {
                    error = "Windows a bloqué le clavier (appli lancée en administrateur ?)";
                    return Result::Failed;
                }
                if (!pause(100)) return Result::Aborted;
                break;
            case PilotOp::Click:
                if (!Ensure(target, error)) return Result::Failed;
                if (!ClickByName(GetAncestor(GetForegroundWindow(), GA_ROOT), s.arg, error)) return Result::Failed;
                if (!pause(300)) return Result::Aborted;
                break;
            case PilotOp::Command: {
                std::string why;
                if (!AcadCommand(s.arg, why)) {
                    // Repli : ligne de commande au clavier (Échap ×2 vide une commande en cours).
                    if (!Ensure(target, error)) return Result::Failed;
                    log::Info("Pilot: {} → commande tapée au clavier", why);
                    const KeyChord esc{{}, VK_ESCAPE};
                    SendChord(esc);
                    SendChord(esc);
                    std::string typed = s.arg;
                    while (!typed.empty() && (typed.back() == ' ' || typed.back() == '\n')) typed.pop_back();
                    if (!TypeText(typed + "\n")) {
                        error = why;
                        return Result::Failed;
                    }
                }
                if (!pause(250)) return Result::Aborted;
                break;
            }
        }
    }
    return Result::Done;
}

}  // namespace companion
