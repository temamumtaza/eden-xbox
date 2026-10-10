#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later

import contextlib
import io
import pathlib
import runpy
import tempfile
import unittest


HELPER = pathlib.Path(__file__).parents[1] / "eden-xbox-macos.py"
ANALYZE_LOGS = runpy.run_path(str(HELPER))["analyze_logs"]


class AnalyzeLogsTests(unittest.TestCase):
    def analyze(self, filename: str, content: str) -> str:
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / filename
            path.write_text(content, encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                ANALYZE_LOGS([path])
            return output.getvalue()

    def test_app_budget_warns_about_per_app_game_type(self) -> None:
        output = self.analyze(
            "eden_uwp_diag.txt",
            "[eden-uwp] app memory 24 MiB of 1024 MiB current limit, expected cap 1024 MiB\n",
        )
        self.assertIn("Xbox resource budget: App-class (~1 GiB)", output)
        self.assertIn("App type to Game in Dev Home", output)

    def test_game_budget_is_reported(self) -> None:
        output = self.analyze(
            "eden_uwp_diag.txt",
            "[eden-uwp] app memory 1986 MiB of 5120 MiB current limit, expected cap 5120 MiB\n",
        )
        self.assertIn("Xbox resource budget: Game-class (~5 GiB)", output)

    def test_diag_summary_uses_the_latest_boot(self) -> None:
        output = self.analyze(
            "eden_uwp_diag.txt",
            "[eden-uwp] [+0ms] BootView::Run entered\n"
            "[eden-uwp] [+10ms] CRASH: stale prior session\n"
            "[eden-uwp] [+15ms] app memory 2000 MiB of 5120 MiB current limit, expected cap 5120 MiB\n"
            "[eden-uwp] [+0ms] BootView::Run entered\n"
            "[eden-uwp] [+15ms] app memory 24 MiB of 1024 MiB current limit, expected cap 1024 MiB\n",
        )
        self.assertIn("latest boot crash markers: 0", output)
        self.assertIn("Xbox resource budget: App-class (~1 GiB)", output)

    def test_diag_reports_app_language_override(self) -> None:
        output = self.analyze(
            "eden_uwp_diag.txt",
            "[eden-uwp] [+0ms] BootView::Run entered\n"
            "[eden-uwp] [+1ms] app language override: requested en-US, readback en-US\n",
        )
        self.assertIn(
            "latest app language override: requested en-US, readback en-US", output
        )

    def test_diag_reports_app_language_override_failure(self) -> None:
        output = self.analyze(
            "eden_uwp_diag.txt",
            "[eden-uwp] [+0ms] BootView::Run entered\n"
            "[eden-uwp] [+1ms] app language override: failed (unsupported language)\n",
        )
        self.assertIn("latest app language override: failed (unsupported language)", output)

    def test_diag_reports_predicted_language_for_the_loaded_title(self) -> None:
        output = self.analyze(
            "eden_uwp_diag.txt",
            "[eden-uwp] [+0ms] BootView::Run entered\n"
            "[eden-uwp] [+20ms] game language metadata: title 010015100B514000, NACP 0000FFFF, "
            "en-US supported true, system language EnglishAmerican (index 1, "
            "code 000000000000454E5553), NACP-priority candidate English (US) (index 0)\n",
        )
        self.assertIn("title 010015100B514000", output)
        self.assertIn("requested English (US) (index 1)", output)
        self.assertIn("candidate American English (index 0)", output)
        self.assertIn("languages 0000FFFF (en-US supported true)", output)

    def test_guest_language_selection_is_translated(self) -> None:
        output = self.analyze(
            "eden_log.txt",
            "GetApplicationDesiredLanguage: requested system language index 1 "
            "(code 000000000000454E5553); selected application language index 0 "
            "from supported mask 000070FD\n",
        )
        self.assertIn(
            "latest NS language response (GetApplicationDesiredLanguage)", output
        )
        self.assertIn("requested English (US) (index 1)", output)
        self.assertIn("selected American English (index 0)", output)
        self.assertIn("supported mask 000070FD", output)

    def test_spanish_fallback_is_translated(self) -> None:
        output = self.analyze(
            "eden_log.txt",
            "GetApplicationDesiredLanguage: requested system language index 1 "
            "(code 000000000000454E5553); selected application language index 5 "
            "from supported mask 000070FD\n",
        )
        self.assertIn("requested English (US) (index 1)", output)
        self.assertIn("selected Latin American Spanish (index 5)", output)

    def test_language_choice_is_attributed_to_the_latest_title(self) -> None:
        output = self.analyze(
            "eden_log.txt",
            'Control data for 010015100B514000: name="Super Mario Bros. Wonder" '
            "supported_languages=0000FFFF\n"
            "GetApplicationDesiredLanguage: requested system language index 1 "
            "(code 000000000000454E5553); selected application language index 5 "
            "from supported mask 0000FFFF\n"
            'Control data for 0100152000022000: name="Mario Kart 8 Deluxe" '
            "supported_languages=0000FFFF\n"
            "GetApplicationDesiredLanguage: requested system language index 1 "
            "(code 000000000000454E5553); selected application language index 0 "
            "from supported mask 0000FFFF\n",
        )
        self.assertIn("latest game title: Mario Kart 8 Deluxe (0100152000022000)", output)
        self.assertIn("selected American English (index 0)", output)
        self.assertNotIn("latest game title: Super Mario Bros. Wonder", output)

    def test_old_language_log_is_not_mistaken_for_a_selection(self) -> None:
        output = self.analyze(
            "eden_log.txt.old.txt",
            "GetApplicationDesiredLanguage: called with supported_languages=000070FD\n",
        )
        self.assertIn("call found, but the log does not record the selected language", output)

    def test_old_language_call_keeps_its_nearest_title(self) -> None:
        output = self.analyze(
            "eden_log.txt.old.txt",
            'Control data for 010015100B514000: name="Super Mario Bros. Wonder" '
            "supported_languages=0000FFFF\n"
            "GetApplicationDesiredLanguage: called with supported_languages=0000FFFF\n",
        )
        self.assertIn("latest game title: Super Mario Bros. Wonder (010015100B514000)", output)
        self.assertIn("call found, but the log does not record the selected language", output)


if __name__ == "__main__":
    unittest.main()
