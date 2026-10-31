#include "direct_api_settings.h"

#include <cJSON.h>

namespace DirectApiSettings {

bool ValidId(const std::string& id) {
    if (id.size() < 2 || id.size() > 4 || id[0] != 'p') return false;
    for (size_t i = 1; i < id.size(); i++) {
        if (id[i] < '0' || id[i] > '9') return false;
    }
    return true;
}

static Provider ParseProvider(const std::string& id, const std::string& json) {
    Provider p;
    p.id = id;
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr) return p;
    auto str = [root](const char* key) {
        cJSON* v = cJSON_GetObjectItem(root, key);
        return std::string(cJSON_IsString(v) ? v->valuestring : "");
    };
    p.name = str("name");
    p.preset = str("preset");
    p.url = str("url");
    cJSON* m = nullptr;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(root, "models")) {
        if (cJSON_IsString(m)) p.models.push_back(m->valuestring);
    }
    cJSON_Delete(root);
    return p;
}

std::vector<Provider> LoadProviders(Settings& s) {
    std::vector<Provider> list;
    std::string ids = s.GetString(kProviderList);
    size_t pos = 0;
    while (pos < ids.size() && list.size() < kMaxProviders) {
        size_t comma = ids.find(',', pos);
        std::string id = ids.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? ids.size() : comma + 1;
        if (!ValidId(id)) continue;
        Provider p = ParseProvider(id, s.GetString(EntryOf(id)));
        if (!p.url.empty()) list.push_back(std::move(p));
    }
    return list;
}

std::string SerializeProvider(const Provider& p) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name", p.name.c_str());
    cJSON_AddStringToObject(root, "preset", p.preset.c_str());
    cJSON_AddStringToObject(root, "url", p.url.c_str());
    cJSON* models = cJSON_AddArrayToObject(root, "models");
    for (const auto& m : p.models) cJSON_AddItemToArray(models, cJSON_CreateString(m.c_str()));
    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    std::string out = json ? json : "";
    cJSON_free(json);
    return out;
}

void SaveProviders(Settings& s, const std::vector<Provider>& old_list, const std::vector<Provider>& list) {
    std::string ids;
    for (const auto& p : list) {
        s.SetString(EntryOf(p.id), SerializeProvider(p));
        ids += (ids.empty() ? "" : ",") + p.id;
    }
    for (const auto& old : old_list) {
        bool kept = false;
        for (const auto& p : list) kept = kept || p.id == old.id;
        if (!kept) {
            s.EraseKey(EntryOf(old.id));
            s.EraseKey(KeyOf(old.id));
        }
    }
    s.SetString(kProviderList, ids);
}

Endpoint ResolveRole(Settings& s, const Role& role) {
    std::string id = s.GetString(role.provider_key);
    if (!ValidId(id)) return {};
    std::string json = s.GetString(EntryOf(id));
    if (json.empty()) return {};
    Provider p = ParseProvider(id, json);
    std::string url = p.url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    return {url, s.GetString(KeyOf(id)), s.GetString(role.model_key)};
}

}  // namespace DirectApiSettings
