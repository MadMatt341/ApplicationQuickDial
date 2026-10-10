#include "CatalogRegistration.h"
#include <objbase.h>
#include "Catalog.h"
#include "TextUtilities.h"
#include <windows.h>
#include <sddl.h>
#include <appmodel.h>
#include <fstream>
#include <memory>
#include <vector>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

namespace quickdial {
namespace {
using namespace winrt::Windows::Data::Json;
struct Close { void operator()(void* value) const { if(value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
using Handle = std::unique_ptr<void,Close>;
std::wstring Read(const std::filesystem::path& path) {
  std::ifstream stream(path,std::ios::binary);
  if(!stream) throw std::runtime_error("Could not read the requested file");
  std::string data((std::istreambuf_iterator<char>(stream)),{});
  if(data.size()>4*1024*1024) throw std::runtime_error("Catalog/request exceeds the 4 MiB registration limit");
  if(data.starts_with("\xef\xbb\xbf")) data.erase(0,3);
  return winrt::to_hstring(data).c_str();
}
void Write(const std::filesystem::path& path, std::wstring_view text) {
  std::ofstream stream(path,std::ios::binary|std::ios::trunc);
  const auto utf8=winrt::to_string(text);
  stream.write(utf8.data(),static_cast<std::streamsize>(utf8.size()));
  stream.flush();
  if(!stream) throw std::runtime_error("Could not write the requested file");
}
JsonObject LoadRoot(const std::filesystem::path& path) {
  if(!std::filesystem::exists(path)) return JsonObject::Parse(L"{\"version\":1,\"discoverInstalled\":true,\"applications\":[]}");
  const auto text=Read(path);
  const auto parsed=ParseCatalogJson(winrt::to_string(text));
  if(!parsed) throw winrt::hresult_invalid_argument(parsed.error);
  return JsonObject::Parse(text);
}
JsonObject ValidateEntry(std::wstring_view text) {
  auto entry=JsonObject::Parse(text);
  const auto id=entry.GetNamedString(L"id",L"");
  if(id.empty()) throw winrt::hresult_invalid_argument(L"Registration requires a stable, nonempty id");
  JsonObject root;
  root.SetNamedValue(L"version",JsonValue::CreateNumberValue(1));
  JsonArray applications; applications.Append(entry);
  root.SetNamedValue(L"applications",applications);
  const auto parsed=ParseCatalogJson(winrt::to_string(root.Stringify()));
  if(!parsed) throw winrt::hresult_invalid_argument(parsed.error);
  return entry;
}
std::wstring MutexName(const std::filesystem::path& path) {
  const auto text=FoldCase(std::filesystem::absolute(path).lexically_normal().wstring());
  std::uint64_t hash=14695981039346656037ULL;
  for(const wchar_t c:text) { hash^=static_cast<std::uint16_t>(c); hash*=1099511628211ULL; }
  return L"Local\\ApplicationQuickDial.CatalogUpdate."+std::to_wstring(hash);
}
}

std::wstring CurrentAccountSid() {
  HANDLE token=nullptr;
  if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return {};
  Handle owner(token);
  DWORD bytes=0; GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
  std::vector<std::byte> data(bytes);
  if(!bytes || !GetTokenInformation(token,TokenUser,data.data(),bytes,&bytes)) return {};
  PWSTR text=nullptr;
  if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&text)) return {};
  std::wstring result(text); LocalFree(text); return result;
}
bool IsPackagedProcess() {
  UINT32 length=0;
  return GetCurrentPackageFullName(&length,nullptr)!=APPMODEL_ERROR_NO_PACKAGE;
}

RegistrationResult RegisterApplicationJson(const std::filesystem::path& path,std::wstring_view applicationJson) {
  RegistrationResult result;
  std::filesystem::path temporary;
  try {
    const auto entry=ValidateEntry(applicationJson);
    const auto name=MutexName(path);
    Handle lock(CreateMutexW(nullptr,FALSE,name.c_str()));
    if(!lock) throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    const auto wait=WaitForSingleObject(lock.get(),3000);
    if(wait!=WAIT_OBJECT_0 && wait!=WAIT_ABANDONED)
      throw winrt::hresult_invalid_argument(L"Another catalog update is busy. Try registration again.");
    struct Unlock { HANDLE handle; ~Unlock(){ReleaseMutex(handle);} } unlock{lock.get()};
    auto root=LoadRoot(path);
    const auto applications=root.GetNamedArray(L"applications");
    JsonArray updated;
    bool replaced=false;
    for(const auto& value:applications) {
      const auto original=value.GetObject();
      // Different commands for the same executable are legitimate separate apps.
      const auto originalId = original.GetNamedString(L"id", L"");
      const bool legacyTargetMatch = originalId.empty() &&
          FoldCase(original.GetNamedString(L"target").c_str()) == FoldCase(entry.GetNamedString(L"target").c_str()) &&
          original.GetNamedArray(L"arguments", JsonArray{}).Stringify() == entry.GetNamedArray(L"arguments", JsonArray{}).Stringify();
      const bool match = FoldCase(originalId.c_str()) == FoldCase(entry.GetNamedString(L"id").c_str()) || legacyTargetMatch;
      if(!match) { updated.Append(original); continue; }
      if(replaced) continue;
      auto merged=JsonObject::Parse(original.Stringify());
      for(const auto& property:entry) merged.SetNamedValue(property.Key(),property.Value());
      updated.Append(merged); replaced=true;
    }
    if(!replaced) updated.Append(entry);
    root.SetNamedValue(L"applications",updated);
    const auto text=root.Stringify();
    if(!ParseCatalogJson(winrt::to_string(text))) throw winrt::hresult_invalid_argument(L"Updated catalog failed validation");
    std::filesystem::create_directories(path.parent_path());
    GUID guid{}; if(FAILED(CoCreateGuid(&guid))) throw std::runtime_error("Could not create registration identity");
    wchar_t id[40]{}; StringFromGUID2(guid,id,40);
    temporary=path.wstring()+L"."+id+L".tmp";
    Write(temporary,text.c_str());
    if(std::filesystem::exists(path)) {
      result.backup=path.wstring()+L".before-registration."+id+L".json";
      if(!ReplaceFileW(path.c_str(),temporary.c_str(),result.backup.c_str(),0,nullptr,nullptr))
        throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    } else if(!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH))
      throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    result.success=true;
  } catch(const winrt::hresult_error& error) { result.error=error.message().c_str(); }
    catch(const std::exception& error) { result.error=winrt::to_hstring(error.what()).c_str(); }
  if(!temporary.empty()) { std::error_code ignored; std::filesystem::remove(temporary,ignored); }
  return result;
}

int RunCatalogCommand(const std::wstring& mode,const std::filesystem::path& request,
                      const std::filesystem::path& output,std::wstring_view expectedAccount) {
  JsonObject report;
  report.SetNamedValue(L"success",JsonValue::CreateBooleanValue(false));
  try {
    const auto sid=CurrentAccountSid();
    report.SetNamedValue(L"account",JsonValue::CreateStringValue(sid));
    if(sid.empty() || sid!=expectedAccount || IsPackagedProcess())
      throw winrt::hresult_invalid_argument(L"Use Register-LauncherApp.ps1 from the intended account; packaged/inherited catalog writes are not verified.");
    const auto entry=ValidateEntry(Read(request));
    const auto path=GetCatalogPath();
    report.SetNamedValue(L"catalogPath",JsonValue::CreateStringValue(path.wstring()));
    if(mode==L"--register-app") {
      const auto result=RegisterApplicationJson(path,entry.Stringify().c_str());
      if(!result.success) throw winrt::hresult_invalid_argument(result.error);
      report.SetNamedValue(L"backupPath",JsonValue::CreateStringValue(result.backup.wstring()));
    }
    const auto root=LoadRoot(path);
    JsonArray matches;
    for(const auto& value:root.GetNamedArray(L"applications")) {
      const auto actual=value.GetObject();
      if(FoldCase(actual.GetNamedString(L"id",L"").c_str())!=FoldCase(entry.GetNamedString(L"id").c_str())) continue;
      for(const auto& property:entry) {
        if(!actual.HasKey(property.Key()) || actual.GetNamedValue(property.Key()).Stringify()!=property.Value().Stringify())
          throw winrt::hresult_invalid_argument(L"The registered entry differs from the request");
      }
      matches.Append(actual);
    }
    if(matches.Size()!=1) throw winrt::hresult_invalid_argument(L"The requested application is missing or duplicated");
    report.SetNamedValue(L"entry",matches.GetAt(0));
    report.SetNamedValue(L"success",JsonValue::CreateBooleanValue(true));
    Write(output,report.Stringify().c_str()); return 0;
  } catch(const winrt::hresult_error& error) { report.SetNamedValue(L"error",JsonValue::CreateStringValue(error.message())); }
    catch(const std::exception& error) { report.SetNamedValue(L"error",JsonValue::CreateStringValue(winrt::to_hstring(error.what()))); }
  try { Write(output,report.Stringify().c_str()); } catch(...) {}
  return 1;
}
}
