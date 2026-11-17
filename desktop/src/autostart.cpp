#include "autostart.h"

#include <windows.h>

#include "common.h"

namespace crg {
namespace {

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kValue = L"CliResume";

std::wstring exePathW() {
  wchar_t buf[MAX_PATH] = {0};
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (!n) return L"";
  return std::wstring(buf, n);
}

}  // namespace

bool autostartEnabled() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
  wchar_t buf[1024] = {0};
  DWORD bytes = sizeof(buf);
  DWORD type = 0;
  const LONG r = RegQueryValueExW(key, kValue, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &bytes);
  RegCloseKey(key);
  return r == ERROR_SUCCESS && type == REG_SZ && buf[0] != 0;
}

bool setAutostart(bool on, std::string& err) {
  HKEY key = nullptr;
  LONG r = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE,
                           nullptr, &key, nullptr);
  if (r != ERROR_SUCCESS) {
    err = "打不开 HKCU\\Run（" + std::to_string(r) + "）";
    return false;
  }

  if (on) {
    const std::wstring exe = exePathW();
    if (exe.empty()) {
      RegCloseKey(key);
      err = "拿不到自己的路径";
      return false;
    }
    // 路径带空格必须加引号；--tray 让它开机后直接进托盘，不抢焦点
    const std::wstring cmd = L"\"" + exe + L"\" --tray";
    r = RegSetValueExW(key, kValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                       static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
  } else {
    r = RegDeleteValueW(key, kValue);
    if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;  // 本来就没开
  }
  RegCloseKey(key);

  if (r != ERROR_SUCCESS) {
    err = "写入注册表失败（" + std::to_string(r) + "）";
    return false;
  }
  logf(std::string("autostart ") + (on ? "on" : "off"));
  return true;
}

}  // namespace crg
