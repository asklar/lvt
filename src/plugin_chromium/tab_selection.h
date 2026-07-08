#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

struct ChromiumTabSelection {
    int tab_id = -1;
    std::string url;
    std::string title;
};

static std::string chromium_trim(std::string s) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), is_space));
    s.erase(std::find_if_not(s.rbegin(), s.rend(), is_space).base(), s.end());
    return s;
}

static std::string chromium_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string normalize_chromium_tab_selector(const char* filter) {
    if (!filter) return {};
    std::string selector = chromium_trim(filter);
    auto lower = chromium_lower(selector);
    constexpr const char* prefixes[] = {"chromium.tab=", "tab="};
    for (auto* prefix : prefixes) {
        std::string p(prefix);
        if (lower.rfind(p, 0) == 0)
            return chromium_trim(selector.substr(p.size()));
    }
    return selector;
}

static bool chromium_is_debuggable_url(const std::string& url) {
    auto lower = chromium_lower(url);
    return !lower.empty() &&
           lower.rfind("chrome://", 0) != 0 &&
           lower.rfind("edge://", 0) != 0 &&
           lower.rfind("about:", 0) != 0 &&
           lower.rfind("chrome-extension://", 0) != 0;
}

static bool chromium_wildcard_match(const std::string& pattern, const std::string& value) {
    size_t p = 0, v = 0, star = std::string::npos, match = 0;
    while (v < value.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == value[v])) {
            ++p;
            ++v;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = v;
        } else if (star != std::string::npos) {
            p = star + 1;
            v = ++match;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

static bool chromium_matches_selector(const std::string& selector,
                                      const std::string& url,
                                      const std::string& title) {
    std::string pattern = chromium_trim(selector);
    auto lowerPattern = chromium_lower(pattern);
    enum class Field { Any, Url, Title };
    Field field = Field::Any;
    if (lowerPattern.rfind("url:", 0) == 0) {
        field = Field::Url;
        pattern = chromium_trim(pattern.substr(4));
    } else if (lowerPattern.rfind("title:", 0) == 0) {
        field = Field::Title;
        pattern = chromium_trim(pattern.substr(6));
    }

    if (pattern.empty()) return false;

    auto p = chromium_lower(pattern);
    auto u = chromium_lower(url);
    auto t = chromium_lower(title);
    bool wildcard = p.find_first_of("*?") != std::string::npos;
    auto matches = [&](const std::string& value) {
        return wildcard ? chromium_wildcard_match(p, value) : value.find(p) != std::string::npos;
    };

    if (field == Field::Url) return matches(u);
    if (field == Field::Title) return matches(t);
    return matches(u) || matches(t);
}

static std::optional<ChromiumTabSelection>
select_chromium_tab_target(const nlohmann::json& targetList,
                           const std::string& selector,
                           std::string& error) {
    const nlohmann::json* tabs = &targetList;
    if (targetList.is_object() && targetList.contains("tabs"))
        tabs = &targetList["tabs"];
    if (!tabs->is_array()) {
        error = "Chromium tab list response did not contain a tabs array";
        return std::nullopt;
    }

    for (const auto& tab : *tabs) {
        int id = tab.value("id", tab.value("tabId", -1));
        std::string url = tab.value("url", "");
        std::string title = tab.value("title", "");
        if (id < 0 || !chromium_is_debuggable_url(url))
            continue;
        if (chromium_matches_selector(selector, url, title))
            return ChromiumTabSelection{id, url, title};
    }

    error = "No Chromium tab matches '" + selector + "'";
    return std::nullopt;
}
