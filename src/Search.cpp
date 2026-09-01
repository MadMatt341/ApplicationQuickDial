#include "Search.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <string>
#include <utility>

namespace quickdial {
namespace {

std::wstring Trim(std::wstring_view value) {
  std::size_t first = 0;
  while (first < value.size() && std::iswspace(value[first])) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first && std::iswspace(value[last - 1])) {
    --last;
  }
  return std::wstring(value.substr(first, last - first));
}

std::wstring FoldCase(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }

  const int needed = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(),
                                   static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr, 0);
  if (needed <= 0) {
    return std::wstring(value);
  }
  std::wstring result(static_cast<std::size_t>(needed), L'\0');
  LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, value.data(), static_cast<int>(value.size()),
                result.data(), needed, nullptr, nullptr, 0);
  return result;
}

bool IsWordBoundary(wchar_t value) {
  return !std::iswalnum(value);
}

int MatchScore(std::wstring_view candidate, std::wstring_view query) {
  if (candidate == query) {
    return 0;
  }
  if (candidate.starts_with(query)) {
    return 1;
  }

  std::size_t position = candidate.find(query);
  while (position != std::wstring_view::npos) {
    if (position > 0 && IsWordBoundary(candidate[position - 1])) {
      return 2;
    }
    position = candidate.find(query, position + 1);
  }
  if (candidate.find(query) != std::wstring_view::npos) {
    return 3;
  }
  return std::numeric_limits<int>::max();
}

int BestMatchScore(
    const std::vector<std::wstring>& candidates, std::wstring_view query, int scoreOffset) {
  int score = std::numeric_limits<int>::max();
  for (const auto& candidate : candidates) {
    const int candidateScore = MatchScore(FoldCase(candidate), query);
    if (candidateScore != std::numeric_limits<int>::max()) {
      score = std::min(score, scoreOffset + candidateScore);
    }
  }
  return score;
}

std::wstring HumanizeIdentifier(std::wstring_view value) {
  std::wstring result;
  result.reserve(value.size());
  wchar_t previous = L'\0';
  for (const wchar_t character : value) {
    if (!std::iswalnum(character)) {
      if (!result.empty() && result.back() != L' ') {
        result.push_back(L' ');
      }
      previous = L'\0';
      continue;
    }
    if (previous != L'\0' && std::iswlower(previous) && std::iswupper(character) &&
        result.back() != L' ') {
      result.push_back(L' ');
    }
    result.push_back(character);
    previous = character;
  }
  while (!result.empty() && result.back() == L' ') {
    result.pop_back();
  }
  return result;
}

std::wstring TargetSearchValue(std::wstring_view target) {
  const std::size_t schemeEnd = target.find(L"://");
  if (schemeEnd != std::wstring_view::npos) {
    const std::wstring_view scheme = target.substr(0, schemeEnd);
    const std::wstring foldedScheme = FoldCase(scheme);
    if (foldedScheme == L"http" || foldedScheme == L"https" || foldedScheme == L"file") {
      return {};
    }
    return std::wstring(scheme);
  }

  const std::size_t separator = target.find_last_of(L"\\/");
  std::wstring_view leaf =
      separator == std::wstring_view::npos ? target : target.substr(separator + 1);
  const std::size_t extension = leaf.find_last_of(L'.');
  if (extension != std::wstring_view::npos && extension > 0 &&
      !FoldCase(target).starts_with(L"shell:appsfolder\\")) {
    leaf = leaf.substr(0, extension);
  }
  return std::wstring(leaf);
}

int IdentifierMatchScore(
    const ApplicationEntry& application, std::wstring_view query, int scoreOffset) {
  int score = std::numeric_limits<int>::max();
  const auto consider = [&](std::wstring_view candidate) {
    const int candidateScore = MatchScore(FoldCase(candidate), query);
    if (candidateScore != std::numeric_limits<int>::max()) {
      score = std::min(score, scoreOffset + candidateScore);
    }
  };

  if (application.identity && !application.identity->empty()) {
    consider(*application.identity);
    consider(HumanizeIdentifier(*application.identity));
  }

  const std::wstring target = TargetSearchValue(application.target);
  if (!target.empty()) {
    consider(target);
    consider(HumanizeIdentifier(target));
  }
  return score;
}

}  // namespace

std::vector<std::size_t> RankApplications(
    const Catalog& catalog, std::wstring_view query, std::size_t limit) {
  const std::wstring foldedQuery = FoldCase(Trim(query));
  std::vector<std::pair<int, std::size_t>> scored;
  scored.reserve(catalog.applications.size());

  for (std::size_t index = 0; index < catalog.applications.size(); ++index) {
    if (foldedQuery.empty()) {
      scored.emplace_back(0, index);
      continue;
    }

    const ApplicationEntry& application = catalog.applications[index];
    int score = MatchScore(FoldCase(application.name), foldedQuery);
    if (score == std::numeric_limits<int>::max()) {
      score = BestMatchScore(application.aliases, foldedQuery, 4);
    }
    if (score == std::numeric_limits<int>::max()) {
      score = IdentifierMatchScore(application, foldedQuery, 8);
    }
    if (score != std::numeric_limits<int>::max()) {
      scored.emplace_back(score, index);
    }
  }

  std::stable_sort(scored.begin(), scored.end(), [](const auto& left, const auto& right) {
    return left.first < right.first;
  });

  std::vector<std::size_t> results;
  const std::size_t resultCount = std::min(limit, scored.size());
  results.reserve(resultCount);
  for (std::size_t index = 0; index < resultCount; ++index) {
    results.push_back(scored[index].second);
  }
  return results;
}

}  // namespace quickdial
