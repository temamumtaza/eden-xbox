// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/hle/service/ns/language.h"

TEST_CASE("NS application language follows the requested priority", "[core][service][ns]") {
    const auto* const priority =
        Service::NS::GetApplicationLanguagePriorityList(Service::NS::ApplicationLanguage::AmericanEnglish);
    REQUIRE(priority != nullptr);

    SECTION("choose American English when the title supports it") {
        const u32 supported = Service::NS::GetSupportedLanguageFlag(
                                  Service::NS::ApplicationLanguage::AmericanEnglish) |
                              Service::NS::GetSupportedLanguageFlag(
                                  Service::NS::ApplicationLanguage::Spanish);
        const auto selected = Service::NS::SelectApplicationLanguage(*priority, supported);
        REQUIRE(selected == Service::NS::ApplicationLanguage::AmericanEnglish);
    }

    SECTION("fall back by the English priority when American English is absent") {
        const u32 supported = Service::NS::GetSupportedLanguageFlag(
                                  Service::NS::ApplicationLanguage::BritishEnglish) |
                              Service::NS::GetSupportedLanguageFlag(
                                  Service::NS::ApplicationLanguage::Spanish);
        const auto selected = Service::NS::SelectApplicationLanguage(*priority, supported);
        REQUIRE(selected == Service::NS::ApplicationLanguage::BritishEnglish);
    }

    SECTION("an unknown title mask retains the first requested language") {
        const auto selected = Service::NS::SelectApplicationLanguage(*priority, 0);
        REQUIRE(selected == Service::NS::ApplicationLanguage::AmericanEnglish);
    }
}
