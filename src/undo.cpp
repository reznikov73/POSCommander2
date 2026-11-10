// Журнал переименований: храним обратные пары и возвращаем имена по Ctrl+Z.
#include "undo.h"

#include "ops.h"
#include "util.h"

#include <cstdio>
#include <deque>

namespace {

const size_t kMaxSteps = 20;

std::deque<std::vector<std::pair<std::wstring, std::wstring>>> g_steps;

}  // namespace

void UndoPushRenames(const std::vector<std::pair<std::wstring, std::wstring>>& pairs) {
    std::vector<std::pair<std::wstring, std::wstring>> valid;
    for (const auto& p : pairs) {
        if (p.first.empty() || p.second.empty()) continue;
        if (p.first == p.second) continue;
        valid.push_back(p);
    }
    if (valid.empty()) return;

    g_steps.push_back(std::move(valid));
    while (g_steps.size() > kMaxSteps) g_steps.pop_front();
}

void UndoPushRename(const std::wstring& from, const std::wstring& to) {
    UndoPushRenames({{from, to}});
}

size_t UndoSteps() {
    return g_steps.size();
}

bool UndoLastRename(std::wstring& report) {
    if (g_steps.empty()) {
        report = L"Отменять нечего: журнал переименований пуст.";
        return false;
    }

    const auto batch = g_steps.back();
    g_steps.pop_back();

    size_t restored = 0;
    std::wstring firstError;
    // Возвращаем в обратном порядке: если имена пересекались, так безопаснее.
    for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
        DWORD err = 0;
        if (RenamePath(it->second, it->first, err)) {
            ++restored;
        } else if (firstError.empty()) {
            firstError = it->second + L": " + ErrText(err);
        }
    }

    wchar_t buf[256]{};
    swprintf(buf, 256, L"Возвращено имён: %zu из %zu", restored, batch.size());
    report = buf;
    if (!firstError.empty()) report += L"\n\nПервая ошибка:\n" + firstError;
    return restored > 0;
}
