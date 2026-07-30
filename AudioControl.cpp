#include <windows.h>

#include <fcntl.h>
#include <io.h>

#include <audioclient.h>
#include <devicetopology.h>
#include <endpointvolume.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

// PolicyConfig is the Windows shell's COM interface for changing the default
// endpoint. It is not in the Windows SDK, so its ABI is declared locally.
MIDL_INTERFACE("F8679F50-850A-41CF-9C72-430F290290C8")
IPolicyConfig : public IUnknown {
 public:
  virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

const CLSID CLSID_PolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

class ComApartment {
 public:
  ComApartment() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
      throw std::runtime_error("CoInitializeEx failed");
    }
  }

  ~ComApartment() { CoUninitialize(); }

  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;
};

class HResultError : public std::runtime_error {
 public:
  HResultError(const std::string& operation, HRESULT hr)
      : std::runtime_error(operation + " failed (HRESULT 0x" + Hex(hr) + ")"), hr_(hr) {}

  HRESULT code() const { return hr_; }

 private:
  static std::string Hex(HRESULT hr) {
    char buffer[9]{};
    snprintf(buffer, sizeof(buffer), "%08lX", static_cast<unsigned long>(hr));
    return buffer;
  }

  HRESULT hr_;
};

void Check(HRESULT hr, const char* operation) {
  if (FAILED(hr)) {
    throw HResultError(operation, hr);
  }
}

struct Device {
  ComPtr<IMMDevice> endpoint;
  std::wstring id;
  std::wstring name;
};

std::wstring ToLower(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  return value;
}

std::wstring FriendlyName(IMMDevice* device) {
  ComPtr<IPropertyStore> properties;
  Check(device->OpenPropertyStore(STGM_READ, &properties), "OpenPropertyStore");

  PROPVARIANT value;
  PropVariantInit(&value);
  Check(properties->GetValue(PKEY_Device_FriendlyName, &value), "GetValue(FriendlyName)");
  std::wstring name = value.vt == VT_LPWSTR && value.pwszVal ? value.pwszVal : L"<unnamed>";
  PropVariantClear(&value);
  return name;
}

std::vector<Device> EnumerateDevices(EDataFlow flow) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&enumerator)),
        "Create MMDeviceEnumerator");

  ComPtr<IMMDeviceCollection> collection;
  Check(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection),
        "EnumAudioEndpoints");

  UINT count = 0;
  Check(collection->GetCount(&count), "Get device count");
  std::vector<Device> devices;
  devices.reserve(count);
  for (UINT i = 0; i < count; ++i) {
    Device device;
    Check(collection->Item(i, &device.endpoint), "Get device");
    LPWSTR id = nullptr;
    Check(device.endpoint->GetId(&id), "Get device ID");
    device.id = id;
    CoTaskMemFree(id);
    device.name = FriendlyName(device.endpoint.Get());
    devices.push_back(std::move(device));
  }
  return devices;
}

bool ParseIndex(const std::wstring& text, size_t* index) {
  if (text.empty() ||
      !std::all_of(text.begin(), text.end(), [](wchar_t ch) { return std::iswdigit(ch) != 0; })) {
    return false;
  }
  try {
    size_t consumed = 0;
    const unsigned long long value = std::stoull(text, &consumed);
    if (consumed != text.size() || value > std::numeric_limits<size_t>::max()) {
      return false;
    }
    *index = static_cast<size_t>(value);
    return true;
  } catch (...) {
    return false;
  }
}

Device SelectDevice(EDataFlow flow, const std::wstring& selector) {
  auto devices = EnumerateDevices(flow);
  if (devices.empty()) {
    throw std::runtime_error("No active audio devices were found");
  }

  size_t index = 0;
  if (ParseIndex(selector, &index)) {
    if (index >= devices.size()) {
      throw std::runtime_error("Device index is out of range; run 'audioctl list' first");
    }
    return std::move(devices[index]);
  }

  for (auto& device : devices) {
    if (device.id == selector) {
      return std::move(device);
    }
  }

  const std::wstring wanted = ToLower(selector);
  std::vector<size_t> matches;
  for (size_t i = 0; i < devices.size(); ++i) {
    if (ToLower(devices[i].name).find(wanted) != std::wstring::npos) {
      matches.push_back(i);
    }
  }
  if (matches.size() == 1) {
    return std::move(devices[matches.front()]);
  }
  if (matches.empty()) {
    throw std::runtime_error("No device name or ID matches the selector");
  }
  throw std::runtime_error("Device selector is ambiguous; use its index or full endpoint ID");
}

Device DefaultDevice(EDataFlow flow) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         IID_PPV_ARGS(&enumerator)),
        "Create MMDeviceEnumerator");
  Device device;
  Check(enumerator->GetDefaultAudioEndpoint(flow, eMultimedia, &device.endpoint),
        "Get default endpoint");
  LPWSTR id = nullptr;
  Check(device.endpoint->GetId(&id), "Get device ID");
  device.id = id;
  CoTaskMemFree(id);
  device.name = FriendlyName(device.endpoint.Get());
  return device;
}

Device ResolveOptionalDevice(EDataFlow flow, int argc, wchar_t* argv[], int selectorIndex) {
  return argc > selectorIndex ? SelectDevice(flow, argv[selectorIndex]) : DefaultDevice(flow);
}

void ListDevices(EDataFlow flow, const wchar_t* heading) {
  std::wcout << heading << L":\n";
  auto devices = EnumerateDevices(flow);

  std::wstring defaultId;
  try {
    defaultId = DefaultDevice(flow).id;
  } catch (...) {
    // Listing remains useful even if Windows currently has no default endpoint.
  }

  for (size_t i = 0; i < devices.size(); ++i) {
    const wchar_t* marker = devices[i].id == defaultId ? L" *" : L"";
    std::wcout << L"  [" << i << L"]" << marker << L" " << devices[i].name << L"\n"
               << L"      " << devices[i].id << L"\n";
  }
  if (devices.empty()) {
    std::wcout << L"  <none>\n";
  }
}

void SetDefaultDevice(const Device& device) {
  ComPtr<IPolicyConfig> policy;
  Check(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
                         __uuidof(IPolicyConfig), &policy),
        "Create PolicyConfigClient");
  for (ERole role : {eConsole, eMultimedia, eCommunications}) {
    Check(policy->SetDefaultEndpoint(device.id.c_str(), role), "SetDefaultEndpoint");
  }
  std::wcout << L"Default device changed to: " << device.name << L"\n";
}

double ParseNumber(const wchar_t* text, const char* label) {
  try {
    size_t consumed = 0;
    const double value = std::stod(text, &consumed);
    if (text[consumed] != L'\0' || !std::isfinite(value)) {
      throw std::invalid_argument("not finite");
    }
    return value;
  } catch (...) {
    throw std::runtime_error(std::string(label) + " must be a valid number");
  }
}

void SetEndpointVolume(const Device& device, double percent) {
  if (percent < 0.0 || percent > 100.0) {
    throw std::runtime_error("Volume must be between 0 and 100");
  }
  ComPtr<IAudioEndpointVolume> volume;
  Check(device.endpoint->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(volume.GetAddressOf())),
        "Activate IAudioEndpointVolume");
  Check(volume->SetMasterVolumeLevelScalar(static_cast<float>(percent / 100.0), nullptr),
        "SetMasterVolumeLevelScalar");
  std::wcout << L"Volume for " << device.name << L" set to " << std::fixed
             << std::setprecision(1) << percent << L"%\n";
}

struct GainControl {
  ComPtr<IAudioVolumeLevel> volume;
  std::wstring name;
  UINT localId = 0;
  float minimum = 0.0f;
  float maximum = 0.0f;
  float step = 0.0f;
  int score = 0;
};

std::wstring PartName(IPart* part) {
  LPWSTR rawName = nullptr;
  if (FAILED(part->GetName(&rawName)) || rawName == nullptr) {
    return L"<unnamed control>";
  }
  std::wstring name(rawName);
  CoTaskMemFree(rawName);
  return name;
}

bool ContainsAny(const std::wstring& value, const std::vector<std::wstring>& needles) {
  return std::any_of(needles.begin(), needles.end(),
                     [&](const std::wstring& needle) { return value.find(needle) != std::wstring::npos; });
}

void WalkTopology(IPart* part, std::set<UINT>* visited, std::vector<GainControl>* controls) {
  UINT localId = 0;
  if (FAILED(part->GetLocalId(&localId)) || !visited->insert(localId).second) {
    return;
  }

  ComPtr<IAudioVolumeLevel> level;
  if (SUCCEEDED(part->Activate(CLSCTX_ALL, __uuidof(IAudioVolumeLevel),
                               reinterpret_cast<void**>(level.GetAddressOf())))) {
    UINT channels = 0;
    float minimum = 0.0f;
    float maximum = 0.0f;
    float step = 0.0f;
    if (SUCCEEDED(level->GetChannelCount(&channels)) && channels > 0 &&
        SUCCEEDED(level->GetLevelRange(0, &minimum, &maximum, &step))) {
      GainControl control;
      control.volume = level;
      control.name = PartName(part);
      control.localId = localId;
      control.minimum = minimum;
      control.maximum = maximum;
      control.step = step;

      const std::wstring lowerName = ToLower(control.name);
      if (ContainsAny(lowerName, {L"boost", L"gain", L"增益", L"增强", L"加强", L"提升"})) {
        control.score += 100;
      }
      if (minimum >= -0.01f && maximum > 0.0f) {
        control.score += 40;
      }
      if (ContainsAny(lowerName, {L"volume", L"音量"})) {
        control.score -= 10;
      }
      controls->push_back(std::move(control));
    }
  }

  auto walkList = [&](bool incoming) {
    ComPtr<IPartsList> parts;
    const HRESULT hr = incoming ? part->EnumPartsIncoming(&parts) : part->EnumPartsOutgoing(&parts);
    if (FAILED(hr) || !parts) {
      return;
    }
    UINT count = 0;
    if (FAILED(parts->GetCount(&count))) {
      return;
    }
    for (UINT i = 0; i < count; ++i) {
      ComPtr<IPart> adjacent;
      if (SUCCEEDED(parts->GetPart(i, &adjacent))) {
        WalkTopology(adjacent.Get(), visited, controls);
      }
    }
  };
  walkList(true);
  walkList(false);
}

std::vector<GainControl> FindGainControls(IMMDevice* endpoint) {
  ComPtr<IDeviceTopology> topology;
  Check(endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(topology.GetAddressOf())),
        "Activate IDeviceTopology");

  UINT connectorCount = 0;
  Check(topology->GetConnectorCount(&connectorCount), "GetConnectorCount");
  std::set<UINT> visited;
  std::vector<GainControl> controls;
  for (UINT i = 0; i < connectorCount; ++i) {
    ComPtr<IConnector> connector;
    if (FAILED(topology->GetConnector(i, &connector))) {
      continue;
    }
    ComPtr<IConnector> connected;
    if (FAILED(connector->GetConnectedTo(&connected))) {
      continue;
    }
    ComPtr<IPart> part;
    if (SUCCEEDED(connected.As(&part))) {
      WalkTopology(part.Get(), &visited, &controls);
    }
  }
  return controls;
}

void PrintGainControls(const Device& device, const std::vector<GainControl>& controls) {
  std::wcout << L"Hardware gain controls for " << device.name << L":\n";
  if (controls.empty()) {
    std::wcout << L"  <none>\n";
    return;
  }
  for (size_t i = 0; i < controls.size(); ++i) {
    std::wcout << L"  [" << i << L"] " << controls[i].name << L"  " << std::fixed
               << std::setprecision(2) << controls[i].minimum << L".." << controls[i].maximum
               << L" dB (step " << controls[i].step << L")\n";
  }
}

void SetMicrophoneGain(const Device& device, double requestedDb) {
  auto controls = FindGainControls(device.endpoint.Get());
  if (controls.empty()) {
    throw std::runtime_error("This microphone exposes no hardware gain/boost control");
  }

  const auto best = std::max_element(controls.begin(), controls.end(),
                                     [](const GainControl& a, const GainControl& b) {
                                       return a.score < b.score;
                                     });
  if (best->score < 30) {
    PrintGainControls(device, controls);
    throw std::runtime_error(
        "No gain/boost control could be identified (the listed control may be microphone volume)");
  }
  if (requestedDb < best->minimum || requestedDb > best->maximum) {
    std::ostringstream message;
    message << "Gain is outside the selected control's range (" << best->minimum << ".."
            << best->maximum << " dB)";
    throw std::runtime_error(message.str());
  }

  UINT channelCount = 0;
  Check(best->volume->GetChannelCount(&channelCount), "Get gain channel count");
  float applied = static_cast<float>(requestedDb);
  if (best->step > 0.0f) {
    applied = best->minimum +
              std::round((applied - best->minimum) / best->step) * best->step;
    applied = (std::max)(best->minimum, (std::min)(best->maximum, applied));
  }
  for (UINT channel = 0; channel < channelCount; ++channel) {
    Check(best->volume->SetLevel(channel, applied, nullptr), "Set microphone gain");
  }
  std::wcout << L"Gain control '" << best->name << L"' for " << device.name << L" set to "
             << std::fixed << std::setprecision(2) << applied << L" dB\n";
}

void PrintUsage() {
  std::wcout
      << L"Audio endpoint control for Windows\n\n"
      << L"Usage:\n"
      << L"  audioctl list [all|speakers|microphones]\n"
      << L"  audioctl 1 <device-index|name|id>\n"
      << L"  audioctl 2 <device-index|name|id>\n"
      << L"  audioctl 3 <0-100> [device-index|name|id]\n"
      << L"  audioctl 4 <0-100> [device-index|name|id]\n"
      << L"  audioctl 5 <dB> [device-index|name|id]\n"
      << L"  audioctl gain-controls [device-index|name|id]\n\n"
      << L"Actions:\n"
      << L"  1  Switch the default speaker\n"
      << L"  2  Switch the default microphone\n"
      << L"  3  Set speaker volume\n"
      << L"  4  Set microphone volume\n"
      << L"  5  Set microphone hardware gain/boost\n\n"
      << L"When an optional device is omitted, the current multimedia default is used.\n"
      << L"The '*' in list output marks the current multimedia default device.\n";
}

int Run(int argc, wchar_t* argv[]) {
  if (argc < 2) {
    PrintUsage();
    return 2;
  }

  const std::wstring action = ToLower(argv[1]);
  if (action == L"help" || action == L"--help" || action == L"-h") {
    PrintUsage();
    return 0;
  }
  if (action == L"list") {
    const std::wstring kind = argc >= 3 ? ToLower(argv[2]) : L"all";
    if (kind == L"all" || kind == L"speakers") {
      ListDevices(eRender, L"Speakers");
    }
    if (kind == L"all") {
      std::wcout << L"\n";
    }
    if (kind == L"all" || kind == L"microphones") {
      ListDevices(eCapture, L"Microphones");
    }
    if (kind != L"all" && kind != L"speakers" && kind != L"microphones") {
      throw std::runtime_error("list accepts only all, speakers, or microphones");
    }
    return 0;
  }
  if (action == L"1" || action == L"2") {
    if (argc != 3) {
      throw std::runtime_error("Actions 1 and 2 require exactly one device selector");
    }
    SetDefaultDevice(SelectDevice(action == L"1" ? eRender : eCapture, argv[2]));
    return 0;
  }
  if (action == L"3" || action == L"4") {
    if (argc < 3 || argc > 4) {
      throw std::runtime_error("Actions 3 and 4 require a volume and an optional device selector");
    }
    const EDataFlow flow = action == L"3" ? eRender : eCapture;
    SetEndpointVolume(ResolveOptionalDevice(flow, argc, argv, 3),
                      ParseNumber(argv[2], "Volume"));
    return 0;
  }
  if (action == L"5") {
    if (argc < 3 || argc > 4) {
      throw std::runtime_error("Action 5 requires a dB value and an optional device selector");
    }
    SetMicrophoneGain(ResolveOptionalDevice(eCapture, argc, argv, 3),
                      ParseNumber(argv[2], "Gain"));
    return 0;
  }
  if (action == L"gain-controls") {
    if (argc > 3) {
      throw std::runtime_error("gain-controls accepts at most one device selector");
    }
    Device device = ResolveOptionalDevice(eCapture, argc, argv, 2);
    PrintGainControls(device, FindGainControls(device.endpoint.Get()));
    return 0;
  }

  throw std::runtime_error("Unknown action; run 'audioctl --help'");
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  SetConsoleOutputCP(CP_UTF8);
  _setmode(_fileno(stdout), _O_U8TEXT);
  try {
    ComApartment apartment;
    return Run(argc, argv);
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << "\n";
    return 1;
  }
}
