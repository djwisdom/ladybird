/*
 * Copyright (c) 2022, Dex♪ <dexes.ttp@gmail.com>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Application.h"
#include "TestWeb.h"
#include "TestWebView.h"

#include <AK/ByteBuffer.h>
#include <AK/Enumerate.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibDiff/Format.h>
#include <LibDiff/Generator.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/SystemTheme.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWebView/Utilities.h>

namespace TestWeb {

static Vector<ByteString> s_skipped_tests;

static constexpr StringView test_result_to_string(TestResult result)
{
    switch (result) {
    case TestResult::Pass:
        return "Pass"sv;
    case TestResult::Fail:
        return "Fail"sv;
    case TestResult::Skipped:
        return "Skipped"sv;
    case TestResult::Timeout:
        return "Timeout"sv;
    case TestResult::Crashed:
        return "Crashed"sv;
    }
    VERIFY_NOT_REACHED();
}

static ErrorOr<void> load_test_config(StringView test_root_path)
{
    auto config_path = LexicalPath::join(test_root_path, "TestConfig.ini"sv);
    auto config_or_error = Core::ConfigFile::open(config_path.string());

    if (config_or_error.is_error()) {
        if (config_or_error.error().code() == ENOENT)
            return {};
        warnln("Unable to open test config {}", config_path);
        return config_or_error.release_error();
    }

    auto config = config_or_error.release_value();
    for (auto const& group : config->groups()) {
        if (group == "Skipped"sv) {
            for (auto& key : config->keys(group))
                s_skipped_tests.append(TRY(FileSystem::real_path(LexicalPath::join(test_root_path, key).string())));
        } else {
            warnln("Unknown group '{}' in config {}", group, config_path);
        }
    }

    return {};
}

static bool is_valid_test_name(StringView test_name)
{
    auto valid_test_file_suffixes = { ".htm"sv, ".html"sv, ".svg"sv, ".xhtml"sv, ".xht"sv };
    return AK::any_of(valid_test_file_suffixes, [&](auto suffix) { return test_name.ends_with(suffix); });
}

static ErrorOr<void> collect_dump_tests(Application const& app, Vector<Test>& tests, StringView path, StringView trail, TestMode mode)
{
    Core::DirIterator it(ByteString::formatted("{}/input/{}", path, trail), Core::DirIterator::Flags::SkipDots);

    while (it.has_next()) {
        auto name = it.next_path();
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/input/{}/{}", path, trail, name)));

        if (FileSystem::is_directory(input_path)) {
            TRY(collect_dump_tests(app, tests, path, ByteString::formatted("{}/{}", trail, name), mode));
            continue;
        }

        if (!is_valid_test_name(name))
            continue;

        auto expectation_path = ByteString::formatted("{}/expected/{}/{}.txt", path, trail, LexicalPath::title(name));
        auto relative_path = LexicalPath::relative_path(input_path, app.test_root_path).release_value();
        tests.append({ mode, input_path, move(expectation_path), move(relative_path) });
    }

    return {};
}

static ErrorOr<void> collect_ref_tests(Application const& app, Vector<Test>& tests, StringView path, StringView trail)
{
    Core::DirIterator it(ByteString::formatted("{}/input/{}", path, trail), Core::DirIterator::Flags::SkipDots);
    while (it.has_next()) {
        auto name = it.next_path();
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/input/{}/{}", path, trail, name)));

        if (FileSystem::is_directory(input_path)) {
            TRY(collect_ref_tests(app, tests, path, ByteString::formatted("{}/{}", trail, name)));
            continue;
        }

        auto relative_path = LexicalPath::relative_path(input_path, app.test_root_path).release_value();
        tests.append({ TestMode::Ref, input_path, {}, move(relative_path) });
    }

    return {};
}

static ErrorOr<void> collect_crash_tests(Application const& app, Vector<Test>& tests, StringView path, StringView trail)
{
    Core::DirIterator it(ByteString::formatted("{}/{}", path, trail), Core::DirIterator::Flags::SkipDots);
    while (it.has_next()) {
        auto name = it.next_path();
        auto input_path = TRY(FileSystem::real_path(ByteString::formatted("{}/{}/{}", path, trail, name)));

        if (FileSystem::is_directory(input_path)) {
            TRY(collect_crash_tests(app, tests, path, ByteString::formatted("{}/{}", trail, name)));
            continue;
        }
        if (!is_valid_test_name(name))
            continue;

        auto relative_path = LexicalPath::relative_path(input_path, app.test_root_path).release_value();
        tests.append({ TestMode::Crash, input_path, {}, move(relative_path) });
    }

    return {};
}

static void clear_test_callbacks(TestWebView& view)
{
    view.on_load_finish = {};
    view.on_test_finish = {};
    view.on_web_content_crashed = {};
}

static void run_dump_test(TestWebView& view, Test& test, URL::URL const& url, int timeout_in_milliseconds)
{
    auto timer = Core::Timer::create_single_shot(timeout_in_milliseconds, [&view, &test]() {
        view.on_load_finish = {};
        view.on_test_finish = {};
        view.on_set_test_timeout = {};
        view.reset_zoom();

        view.on_test_complete({ test, TestResult::Timeout });
    });

    auto handle_completed_test = [&test, url]() -> ErrorOr<TestResult> {
        if (test.expectation_path.is_empty()) {
            if (test.mode != TestMode::Crash)
                outln("{}", test.text);
            return TestResult::Pass;
        }

        auto open_expectation_file = [&](auto mode) {
            auto expectation_file_or_error = Core::File::open(test.expectation_path, mode);
            if (expectation_file_or_error.is_error())
                warnln("Failed opening '{}': {}", test.expectation_path, expectation_file_or_error.error());

            return expectation_file_or_error;
        };

        ByteBuffer expectation;

        if (auto expectation_file = open_expectation_file(Core::File::OpenMode::Read); !expectation_file.is_error()) {
            expectation = TRY(expectation_file.value()->read_until_eof());

            auto result_trimmed = StringView { test.text }.trim("\n"sv, TrimMode::Right);
            auto expectation_trimmed = StringView { expectation }.trim("\n"sv, TrimMode::Right);

            if (result_trimmed == expectation_trimmed)
                return TestResult::Pass;
        } else if (!Application::the().rebaseline) {
            return expectation_file.release_error();
        }

        if (Application::the().rebaseline) {
            TRY(Core::Directory::create(LexicalPath { test.expectation_path }.parent().string(), Core::Directory::CreateDirectories::Yes));

            auto expectation_file = TRY(open_expectation_file(Core::File::OpenMode::Write));
            TRY(expectation_file->write_until_depleted(test.text));

            return TestResult::Pass;
        }

        auto const color_output = TRY(Core::System::isatty(STDOUT_FILENO)) ? Diff::ColorOutput::Yes : Diff::ColorOutput::No;

        if (color_output == Diff::ColorOutput::Yes)
            outln("\n\033[33;1mTest failed\033[0m: {}", url);
        else
            outln("\nTest failed: {}", url);

        auto hunks = TRY(Diff::from_text(expectation, test.text, 3));
        auto out = TRY(Core::File::standard_output());

        TRY(Diff::write_unified_header(test.expectation_path, test.expectation_path, *out));
        for (auto const& hunk : hunks)
            TRY(Diff::write_unified(hunk, *out, color_output));

        return TestResult::Fail;
    };

    auto on_test_complete = [&view, &test, timer, handle_completed_test]() {
        view.reset_zoom();
        clear_test_callbacks(view);
        timer->stop();

        if (auto result = handle_completed_test(); result.is_error())
            view.on_test_complete({ test, TestResult::Fail });
        else
            view.on_test_complete({ test, result.value() });
    };

    view.on_web_content_crashed = [&view, &test, timer]() {
        clear_test_callbacks(view);
        timer->stop();

        view.on_test_complete({ test, TestResult::Crashed });
    };

    if (test.mode == TestMode::Layout) {
        view.on_load_finish = [&view, &test, url, on_test_complete = move(on_test_complete)](auto const& loaded_url) {
            // We don't want subframe loads to trigger the test finish.
            if (!url.equals(loaded_url, URL::ExcludeFragment::Yes))
                return;

            // NOTE: We take a screenshot here to force the lazy layout of SVG-as-image documents to happen.
            //       It also causes a lot more code to run, which is good for finding bugs. :^)
            view.take_screenshot()->when_resolved([&view, &test, on_test_complete = move(on_test_complete)](auto) {
                auto promise = view.request_internal_page_info(WebView::PageInfoType::LayoutTree | WebView::PageInfoType::PaintTree | WebView::PageInfoType::StackingContextTree);

                promise->when_resolved([&test, on_test_complete = move(on_test_complete)](auto const& text) {
                    test.text = text;
                    on_test_complete();
                });
            });
        };
    } else if (test.mode == TestMode::Text) {
        view.on_load_finish = [&view, &test, on_test_complete, url](auto const& loaded_url) {
            // We don't want subframe loads to trigger the test finish.
            if (!url.equals(loaded_url, URL::ExcludeFragment::Yes))
                return;

            test.did_finish_loading = true;

            if (test.expectation_path.is_empty()) {
                auto promise = view.request_internal_page_info(WebView::PageInfoType::Text);

                promise->when_resolved([&test, on_test_complete = move(on_test_complete)](auto const& text) {
                    test.text = text;
                    on_test_complete();
                });
            } else if (test.did_finish_test) {
                on_test_complete();
            }
        };

        view.on_test_finish = [&test, on_test_complete](auto const& text) {
            test.text = text;
            test.did_finish_test = true;

            if (test.did_finish_loading)
                on_test_complete();
        };
    } else if (test.mode == TestMode::Crash) {
        view.on_load_finish = [on_test_complete, url, &view, &test](auto const& loaded_url) {
            // We don't want subframe loads to trigger the test finish.
            if (!url.equals(loaded_url, URL::ExcludeFragment::Yes))
                return;
            test.did_finish_loading = true;
            static String wait_for_crash_test_completion = R"(
   document.fonts.ready.then(() => {
        requestAnimationFrame(function() {
            requestAnimationFrame(function() {
                internals.signalTestIsDone("PASS");
            });
        });
    });
)"_string;
            view.run_javascript(wait_for_crash_test_completion);
            if (test.did_finish_test)
                on_test_complete();
        };

        view.on_test_finish = [&test, on_test_complete](auto const&) {
            test.did_finish_test = true;

            if (test.did_finish_loading)
                on_test_complete();
        };
    }

    view.on_set_test_timeout = [timer, timeout_in_milliseconds](double milliseconds) {
        if (milliseconds <= timeout_in_milliseconds)
            return;
        timer->stop();
        timer->start(milliseconds);
    };

    view.on_set_browser_zoom = [&view](double factor) {
        view.set_zoom(factor);
    };

    view.load(url);
    timer->start();
}

static String wait_for_reftest_completion = R"(
function hasReftestWaitClass() {
    return document.documentElement.classList.contains('reftest-wait');
}

if (!hasReftestWaitClass()) {
    document.fonts.ready.then(() => {
        requestAnimationFrame(function() {
            requestAnimationFrame(function() {
                internals.signalTestIsDone("PASS");
            });
        });
    });
} else {
    const observer = new MutationObserver(() => {
        if (!hasReftestWaitClass()) {
            internals.signalTestIsDone("PASS");
        }
    });

    observer.observe(document.documentElement, {
        attributes: true,
        attributeFilter: ['class'],
    });
}
)"_string;

static void run_ref_test(TestWebView& view, Test& test, URL::URL const& url, int timeout_in_milliseconds)
{
    auto timer = Core::Timer::create_single_shot(timeout_in_milliseconds, [&view, &test]() {
        view.on_load_finish = {};
        view.on_test_finish = {};
        view.on_set_test_timeout = {};
        view.reset_zoom();

        view.on_test_complete({ test, TestResult::Timeout });
    });

    auto handle_completed_test = [&view, &test, url]() -> ErrorOr<TestResult> {
        VERIFY(test.ref_test_expectation_type.has_value());
        auto should_match = test.ref_test_expectation_type == RefTestExpectationType::Match;
        auto screenshot_matches = fuzzy_screenshot_match(
            view.url(), *test.actual_screenshot, *test.expectation_screenshot, test.fuzzy_matches);
        if (should_match == screenshot_matches)
            return TestResult::Pass;

        if (Application::the().dump_failed_ref_tests) {
            warnln("\033[33;1mRef test {} failed; dumping screenshots\033[0m", test.relative_path);

            auto dump_screenshot = [&](Gfx::Bitmap const& bitmap, StringView path) -> ErrorOr<void> {
                auto screenshot_file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
                auto encoded_data = TRY(Gfx::PNGWriter::encode(bitmap));
                TRY(screenshot_file->write_until_depleted(encoded_data));

                outln("\033[33;1mDumped {}\033[0m", TRY(FileSystem::real_path(path)));
                return {};
            };

            TRY(Core::Directory::create("test-dumps"sv, Core::Directory::CreateDirectories::Yes));

            auto title = LexicalPath::title(URL::percent_decode(url.serialize_path()));
            TRY(dump_screenshot(*test.actual_screenshot, ByteString::formatted("test-dumps/{}.png", title)));
            TRY(dump_screenshot(*test.expectation_screenshot, ByteString::formatted("test-dumps/{}-ref.png", title)));
        }

        return TestResult::Fail;
    };

    auto on_test_complete = [&view, &test, timer, handle_completed_test]() {
        clear_test_callbacks(view);
        timer->stop();

        if (auto result = handle_completed_test(); result.is_error())
            view.on_test_complete({ test, TestResult::Fail });
        else
            view.on_test_complete({ test, result.value() });
    };

    view.on_web_content_crashed = [&view, &test, timer]() {
        clear_test_callbacks(view);
        timer->stop();

        view.on_test_complete({ test, TestResult::Crashed });
    };

    view.on_load_finish = [&view](auto const&) {
        view.run_javascript(wait_for_reftest_completion);
    };

    view.on_test_finish = [&view, &test, on_test_complete = move(on_test_complete)](auto const&) {
        if (test.actual_screenshot) {
            // The reference has finished loading; take another screenshot and move on to handling the result.
            view.take_screenshot()->when_resolved([&view, &test, on_test_complete = move(on_test_complete)](RefPtr<Gfx::Bitmap const> screenshot) {
                test.expectation_screenshot = move(screenshot);
                view.reset_zoom();
                on_test_complete();
            });
        } else {
            // When the test initially finishes, we take a screenshot and request the reference test metadata.
            view.take_screenshot()->when_resolved([&view, &test](RefPtr<Gfx::Bitmap const> screenshot) {
                test.actual_screenshot = move(screenshot);
                view.reset_zoom();
                view.run_javascript("internals.loadReferenceTestMetadata();"_string);
            });
        }
    };

    view.on_reference_test_metadata = [&view, &test](JsonValue const& metadata) {
        auto metadata_object = metadata.as_object();

        auto match_references = metadata_object.get_array("match_references"sv);
        auto mismatch_references = metadata_object.get_array("mismatch_references"sv);
        VERIFY(!match_references->is_empty() || !mismatch_references->is_empty());

        // Read fuzzy configurations.
        test.fuzzy_matches.clear_with_capacity();
        auto fuzzy_values = metadata_object.get_array("fuzzy"sv);
        for (size_t i = 0; i < fuzzy_values->size(); ++i) {
            auto fuzzy_configuration = fuzzy_values->at(i).as_object();

            Optional<URL::URL> reference_url;
            auto reference = fuzzy_configuration.get_string("reference"sv);
            if (reference.has_value())
                reference_url = URL::Parser::basic_parse(reference.release_value());

            auto content = fuzzy_configuration.get_string("content"sv).release_value();
            auto fuzzy_match_or_error = parse_fuzzy_match(reference_url, content);
            if (fuzzy_match_or_error.is_error()) {
                warnln("Failed to parse fuzzy configuration '{}' (reference: {})", content, reference_url);
                continue;
            }

            test.fuzzy_matches.append(fuzzy_match_or_error.release_value());
        }

        // Read (mis)match reference tests to load.
        // FIXME: Currently we only support single match or mismatch reference.
        String reference_to_load;
        if (!match_references->is_empty()) {
            if (match_references->size() > 1)
                dbgln("FIXME: Only a single ref test match reference is supported");

            test.ref_test_expectation_type = RefTestExpectationType::Match;
            reference_to_load = match_references->at(0).as_string();
        } else {
            if (mismatch_references->size() > 1)
                dbgln("FIXME: Only a single ref test mismatch reference is supported");

            test.ref_test_expectation_type = RefTestExpectationType::Mismatch;
            reference_to_load = mismatch_references->at(0).as_string();
        }
        view.load(URL::Parser::basic_parse(reference_to_load).release_value());
    };

    view.on_set_test_timeout = [timer, timeout_in_milliseconds](double milliseconds) {
        if (milliseconds <= timeout_in_milliseconds)
            return;
        timer->stop();
        timer->start(milliseconds);
    };

    view.on_set_browser_zoom = [&view](double factor) {
        view.set_zoom(factor);
    };

    view.load(url);
    timer->start();
}

static void run_test(TestWebView& view, Test& test, Application& app)
{
    // Clear the current document.
    // FIXME: Implement a debug-request to do this more thoroughly.
    auto promise = Core::Promise<Empty>::construct();

    view.on_load_finish = [promise](auto const& url) {
        if (!url.equals(URL::about_blank()))
            return;

        Core::deferred_invoke([promise]() {
            promise->resolve({});
        });
    };

    view.on_test_finish = {};

    promise->when_resolved([&view, &test, &app](auto) {
        auto url = URL::create_with_file_scheme(MUST(FileSystem::real_path(test.input_path))).release_value();

        switch (test.mode) {
        case TestMode::Crash:
        case TestMode::Text:
        case TestMode::Layout:
            run_dump_test(view, test, url, app.per_test_timeout_in_seconds * 1000);
            return;
        case TestMode::Ref:
            run_ref_test(view, test, url, app.per_test_timeout_in_seconds * 1000);
            return;
        }

        VERIFY_NOT_REACHED();
    });

    view.load(URL::about_blank());
}

static void set_ui_callbacks_for_tests(TestWebView& view)
{
    view.on_request_file_picker = [&](auto const& accepted_file_types, auto allow_multiple_files) {
        // Create some dummy files for tests.
        Vector<Web::HTML::SelectedFile> selected_files;

        bool add_txt_files = accepted_file_types.filters.is_empty();
        bool add_cpp_files = false;

        for (auto const& filter : accepted_file_types.filters) {
            filter.visit(
                [](Web::HTML::FileFilter::FileType) {},
                [&](Web::HTML::FileFilter::MimeType const& mime_type) {
                    if (mime_type.value == "text/plain"sv)
                        add_txt_files = true;
                },
                [&](Web::HTML::FileFilter::Extension const& extension) {
                    if (extension.value == "cpp"sv)
                        add_cpp_files = true;
                });
        }

        if (add_txt_files) {
            selected_files.empend("file1"sv, MUST(ByteBuffer::copy("Contents for file1"sv.bytes())));

            if (allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes) {
                selected_files.empend("file2"sv, MUST(ByteBuffer::copy("Contents for file2"sv.bytes())));
                selected_files.empend("file3"sv, MUST(ByteBuffer::copy("Contents for file3"sv.bytes())));
                selected_files.empend("file4"sv, MUST(ByteBuffer::copy("Contents for file4"sv.bytes())));
            }
        }

        if (add_cpp_files) {
            selected_files.empend("file1.cpp"sv, MUST(ByteBuffer::copy("int main() {{ return 1; }}"sv.bytes())));

            if (allow_multiple_files == Web::HTML::AllowMultipleFiles::Yes) {
                selected_files.empend("file2.cpp"sv, MUST(ByteBuffer::copy("int main() {{ return 2; }}"sv.bytes())));
            }
        }

        view.file_picker_closed(move(selected_files));
    };

    view.on_request_alert = [&](auto const&) {
        // For tests, just close the alert right away to unblock JS execution.
        view.alert_closed();
    };
}

static ErrorOr<int> run_tests(Core::AnonymousBuffer const& theme, Web::DevicePixelSize window_size)
{
    auto& app = Application::the();
    TRY(load_test_config(app.test_root_path));

    Vector<Test> tests;

    for (auto& glob : app.test_globs)
        glob = ByteString::formatted("*{}*", glob);
    if (app.test_globs.is_empty())
        app.test_globs.append("*"sv);

    TRY(collect_dump_tests(app, tests, ByteString::formatted("{}/Layout", app.test_root_path), "."sv, TestMode::Layout));
    TRY(collect_dump_tests(app, tests, ByteString::formatted("{}/Text", app.test_root_path), "."sv, TestMode::Text));
    TRY(collect_ref_tests(app, tests, ByteString::formatted("{}/Ref", app.test_root_path), "."sv));
    TRY(collect_crash_tests(app, tests, ByteString::formatted("{}/Crash", app.test_root_path), "."sv));
    TRY(collect_ref_tests(app, tests, ByteString::formatted("{}/Screenshot", app.test_root_path), "."sv));

    tests.remove_all_matching([&](auto const& test) {
        static constexpr Array support_file_patterns {
            "*/wpt-import/*/support/*"sv,
            "*/wpt-import/*/resources/*"sv,
            "*/wpt-import/common/*"sv,
            "*/wpt-import/images/*"sv,
        };
        bool is_support_file = any_of(support_file_patterns, [&](auto pattern) { return test.input_path.matches(pattern); });
        bool match_glob = any_of(app.test_globs, [&](auto const& glob) { return test.relative_path.matches(glob, CaseSensitivity::CaseSensitive); });
        return is_support_file || !match_glob;
    });

    if (app.shuffle)
        shuffle(tests);

    if (app.test_dry_run) {
        outln("Found {} tests...", tests.size());

        for (auto const& [i, test] : enumerate(tests))
            outln("{}/{}: {}", i + 1, tests.size(), test.relative_path);

        return 0;
    }

    if (tests.is_empty()) {
        if (app.test_globs.is_empty())
            return Error::from_string_literal("No tests found");
        return Error::from_string_literal("No tests found matching filter");
    }

    auto concurrency = min(app.test_concurrency, tests.size());
    size_t loaded_web_views = 0;

    Vector<NonnullOwnPtr<TestWebView>> views;
    views.ensure_capacity(concurrency);

    for (size_t i = 0; i < concurrency; ++i) {
        auto view = TestWebView::create(theme, window_size);
        view->on_load_finish = [&](auto const&) { ++loaded_web_views; };

        views.unchecked_append(move(view));
    }

    // We need to wait for the initial about:blank load to complete before starting the tests, otherwise we may load the
    // test URL before the about:blank load completes. WebContent currently cannot handle this, and will drop the test URL.
    Core::EventLoop::current().spin_until([&]() {
        return loaded_web_views == concurrency;
    });

    size_t pass_count = 0;
    size_t fail_count = 0;
    size_t timeout_count = 0;
    size_t crashed_count = 0;
    size_t skipped_count = 0;

    // Keep clearing and reusing the same line if stdout is a TTY.
    bool log_on_one_line = app.verbosity < Application::VERBOSITY_LEVEL_LOG_TEST_DURATION && TRY(Core::System::isatty(STDOUT_FILENO));
    outln("Running {} tests...", tests.size());

    auto all_tests_complete = Core::Promise<Empty>::construct();
    auto tests_remaining = tests.size();
    auto current_test = 0uz;

    Vector<TestCompletion> non_passing_tests;

    for (auto& view : views) {
        set_ui_callbacks_for_tests(*view);
        view->clear_content_filters();

        auto run_next_test = [&]() {
            auto index = current_test++;
            if (index >= tests.size())
                return;

            auto& test = tests[index];
            test.start_time = UnixDateTime::now();
            test.index = index + 1;

            if (log_on_one_line) {
                out("\33[2K\r{}/{}: {}", test.index, tests.size(), test.relative_path);
                (void)fflush(stdout);
            } else {
                outln("{}/{}:  Start {}", test.index, tests.size(), test.relative_path);
            }

            Core::deferred_invoke([&]() mutable {
                if (s_skipped_tests.contains_slow(test.input_path))
                    view->on_test_complete({ test, TestResult::Skipped });
                else
                    run_test(*view, test, app);
            });
        };

        view->test_promise().when_resolved([&, run_next_test](auto result) {
            result.test.end_time = UnixDateTime::now();

            if (app.verbosity >= Application::VERBOSITY_LEVEL_LOG_TEST_DURATION) {
                auto duration = result.test.end_time - result.test.start_time;
                outln("{}/{}: Finish {}: {}ms", result.test.index, tests.size(), result.test.relative_path, duration.to_milliseconds());
            }

            switch (result.result) {
            case TestResult::Pass:
                ++pass_count;
                break;
            case TestResult::Fail:
                ++fail_count;
                break;
            case TestResult::Timeout:
                ++timeout_count;
                break;
            case TestResult::Crashed:
                ++crashed_count;
                break;
            case TestResult::Skipped:
                ++skipped_count;
                break;
            }

            if (result.result != TestResult::Pass)
                non_passing_tests.append(move(result));

            if (--tests_remaining == 0)
                all_tests_complete->resolve({});
            else
                run_next_test();
        });

        Core::deferred_invoke([run_next_test]() {
            run_next_test();
        });
    }

    MUST(all_tests_complete->await());

    if (log_on_one_line)
        outln("\33[2K\rDone!");

    outln("==========================================================");
    outln("Pass: {}, Fail: {}, Skipped: {}, Timeout: {}, Crashed: {}", pass_count, fail_count, skipped_count, timeout_count, crashed_count);
    outln("==========================================================");

    for (auto const& non_passing_test : non_passing_tests) {
        if (non_passing_test.result == TestResult::Skipped && app.verbosity < Application::VERBOSITY_LEVEL_LOG_SKIPPED_TESTS)
            continue;

        outln("{}: {}", test_result_to_string(non_passing_test.result), non_passing_test.test.relative_path);
    }

    if (app.verbosity >= Application::VERBOSITY_LEVEL_LOG_SLOWEST_TESTS) {
        auto tests_to_print = min(10uz, tests.size());
        outln("\nSlowest {} tests:", tests_to_print);

        quick_sort(tests, [&](auto const& lhs, auto const& rhs) {
            auto lhs_duration = lhs.end_time - lhs.start_time;
            auto rhs_duration = rhs.end_time - rhs.start_time;
            return lhs_duration > rhs_duration;
        });

        for (auto const& test : tests.span().trim(tests_to_print)) {
            auto duration = test.end_time - test.start_time;

            outln("{}: {}ms", test.relative_path, duration.to_milliseconds());
        }
    }

    if (app.dump_gc_graph) {
        for (auto& view : views) {
            if (auto path = view->dump_gc_graph(); path.is_error())
                warnln("Failed to dump GC graph: {}", path.error());
            else
                outln("GC graph dumped to {}", path.value());
        }
    }

    return fail_count + timeout_count + crashed_count;
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestWeb::Application::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestWeb::Application::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));

    auto const& browser_options = TestWeb::Application::browser_options();
    Web::DevicePixelSize window_size { browser_options.window_width, browser_options.window_height };

    VERIFY(!app->test_root_path.is_empty());

    app->test_root_path = LexicalPath::absolute_path(TRY(FileSystem::current_working_directory()), app->test_root_path);
    TRY(app->launch_test_fixtures());

    return TestWeb::run_tests(theme, window_size);
}
