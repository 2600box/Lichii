// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <map>
#include <utility>
#include "common/recommended_settings.h"

namespace Settings {

std::vector<RecommendedSettingChange> ApplyRecommendedSettings(Values& values) {
    std::vector<RecommendedSettingChange> changes;
    std::string backup;
    ForEachRecommendedSetting(values, [&](auto& setting, const auto& value) {
        std::string previous = setting.ToStringGlobal();
        if (!backup.empty()) {
            backup += ',';
        }
        backup.append(setting.GetLabel()).append(":").append(previous);
        if (setting.GetValue(true) == value) {
            return;
        }
        const bool using_global = setting.UsingGlobal();
        setting.SetGlobal(true);
        setting.SetValue(value);
        setting.SetGlobal(using_global);
        changes.push_back({&setting, std::move(previous), setting.ToStringGlobal()});
    });
    if (!changes.empty() && values.recommended_settings_backup.GetValue().empty()) {
        values.recommended_settings_backup = backup;
    }
    return changes;
}

std::vector<RecommendedSettingChange> RestorePreviousSettings(Values& values) {
    std::map<std::string, std::string> saved;
    const std::string& backup = values.recommended_settings_backup.GetValue();
    for (std::size_t begin = 0; begin < backup.size();) {
        const std::size_t end = (std::min)(backup.find(',', begin), backup.size());
        const std::size_t colon = backup.find(':', begin);
        if (colon < end) {
            saved.emplace(backup.substr(begin, colon - begin),
                          backup.substr(colon + 1, end - colon - 1));
        }
        begin = end + 1;
    }

    std::vector<RecommendedSettingChange> changes;
    ForEachRecommendedSetting(values, [&](auto& setting, const auto&) {
        const auto it = saved.find(setting.GetLabel());
        if (it == saved.end() || it->second.empty()) {
            return;
        }
        std::string previous = setting.ToStringGlobal();
        const bool using_global = setting.UsingGlobal();
        setting.SetGlobal(true);
        setting.LoadString(it->second);
        setting.SetGlobal(using_global);
        if (setting.ToStringGlobal() != previous) {
            changes.push_back({&setting, std::move(previous), setting.ToStringGlobal()});
        }
    });
    values.recommended_settings_backup = std::string{};
    return changes;
}

} // namespace Settings
