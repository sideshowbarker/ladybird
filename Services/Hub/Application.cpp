/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <Hub/Application.h>
#include <Hub/ConnectionFromUI.h>
#include <Hub/Proxies.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibIPC/Decoder.h>
#include <LibWebView/Extensions/Extension.h>
#include <LibWebView/Extensions/ExtensionRegistry.h>
#include <LibWebView/Extensions/ExtensionSidePanelHost.h>
#include <LibWebView/UpdateCheck.h>
#include <LibWebView/WebContentPage.h>

namespace Hub {

Application::Application(ApplicationHost* host)
    : m_host(host)
{
}

void Application::create_platform_arguments(Core::ArgsParser& args_parser)
{
    args_parser.add_option(m_encoded_ui_info, "What the UI tells the Hub to start with", "ui-info", 0, "hex");
    args_parser.add_option(m_ui_mach_server_name, "Mach server to get the connection to the UI from", "ui-mach-server", 0, "name");
    // NB: main() reads this itself, before the arguments are parsed.
    args_parser.add_option(m_ui_pid, "Process of the UI, which the Hub ends with", "ui-pid", 0, "pid");
}

Optional<pid_t> Application::ui_process_id() const
{
    auto pid = m_ui_pid.to_number<pid_t>();
    if (!pid.has_value() || *pid <= 0)
        return {};
    return pid;
}

Hub::UIInfo const* Application::ui_info() const
{
    if (!m_ui_info.has_value() && !m_encoded_ui_info.is_empty()) {
        auto bytes = decode_hex(m_encoded_ui_info);
        if (bytes.is_error())
            return nullptr;
        FixedMemoryStream stream { bytes.value().bytes() };
        Queue<IPC::Attachment> attachments;
        IPC::Decoder decoder { stream, attachments };
        auto info = decoder.decode<Hub::UIInfo>();
        if (info.is_error())
            return nullptr;
        m_ui_info = info.release_value();
    }
    return m_ui_info.has_value() ? &*m_ui_info : nullptr;
}

bool Application::ui_has_location_source() const
{
    auto const* info = ui_info();
    return info && info->has_location_source;
}

bool Application::ui_has_translator() const
{
    auto const* info = ui_info();
    return info && info->has_translator;
}

void Application::set_ui_info(Hub::UIInfo info)
{
    auto scrollbar_track_press_jumps_changed = !m_ui_info.has_value() || m_ui_info->scrollbar_track_press_jumps != info.scrollbar_track_press_jumps;
    auto scrollbars_always_shown_changed = !m_ui_info.has_value() || m_ui_info->scrollbars_always_shown != info.scrollbars_always_shown;
    auto prefers_reduced_motion_changed = !m_ui_info.has_value() || m_ui_info->prefers_reduced_motion != info.prefers_reduced_motion;
    auto prefers_more_contrast_changed = !m_ui_info.has_value() || m_ui_info->prefers_more_contrast != info.prefers_more_contrast;
    m_ui_info = move(info);
    if (scrollbar_track_press_jumps_changed)
        platform_scrollbar_track_press_behavior_changed();
    if (scrollbars_always_shown_changed)
        platform_scrollbar_visibility_changed();
    if (prefers_reduced_motion_changed)
        platform_motion_preference_changed();
    if (prefers_more_contrast_changed)
        platform_contrast_preference_changed();
}

View& Application::create_view(View::InitialState initial_state, Optional<WebView::CanonicalTraversable&> traversable)
{
    auto view = make<View>(move(initial_state));
    auto& view_reference = *view;
    m_views.set(view->view_id(), move(view));
    view_reference.initialize(traversable);
    return view_reference;
}

void Application::notify_webdriver_window_created(String const& handle)
{
    // NB: WebDriver drives a window from the moment it hears of it. So, it must not hear of one before the UI has asked for
    //     what its windows start out showing, or that would replace the page WebDriver went to.
    if (ConnectionFromUI::the() && !m_ui_did_start) {
        m_webdriver_windows_awaiting_ui.append(handle);
        return;
    }
    WebView::Application::notify_webdriver_window_created(handle);
}

void Application::ui_did_start()
{
    m_ui_did_start = true;
    for (auto const& handle : m_webdriver_windows_awaiting_ui)
        WebView::Application::notify_webdriver_window_created(handle);
    m_webdriver_windows_awaiting_ui.clear();
    if (m_openssl_mismatch_awaiting_ui.has_value())
        display_openssl_mismatch(m_openssl_mismatch_awaiting_ui.release_value());
}

void Application::set_active_view(Optional<u64> view_id)
{
    m_active_view_id = view_id;
}

Optional<WebView::ViewImplementation&> Application::active_web_view() const
{
    if (!m_active_view_id.has_value())
        return {};
    return WebView::ViewImplementation::find_view_by_id(*m_active_view_id);
}

Optional<WebView::ViewImplementation&> Application::open_blank_new_tab(Web::HTML::ActivateTab activate_tab, WebView::ViewImplementation const* related_view) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui)
        return WebView::Application::open_blank_new_tab(activate_tab, related_view);

    // NB: A tab opened for a page starts out like that page's view, with its privacy, whichever view is active by now.
    View::InitialState initial_state;
    if (related_view)
        initial_state = static_cast<View const&>(*related_view).initial_state_for_related_view();
    else if (auto active_view = active_web_view(); active_view.has_value())
        initial_state = static_cast<View&>(*active_view).initial_state_for_related_view();
    auto& view = const_cast<Application&>(*this).create_view(move(initial_state));
    Optional<u64> source_view_id;
    if (related_view)
        source_view_id = related_view->view_id();
    ui->async_app_open_view_in_new_tab(view.view_id(), source_view_id, view.is_private(), activate_tab);
    return view;
}

// NB: Which window a tab goes to must not depend on what is active once the UI reads the message. A tab must stay in
//     the browsing session of the page that opened it. With no such page, the active view here is the one that was
//     active when the UI sent what led to this, since the UI's messages arrive in order.
Application::NewTabSource Application::new_tab_source(WebView::ViewImplementation const* source_view) const
{
    auto active_view = active_web_view();
    if (!source_view && active_view.has_value())
        source_view = &*active_view;
    if (!source_view)
        return {};
    return { source_view->view_id(), source_view->is_private() };
}

void Application::open_url_in_new_tab(URL::URL const& url, Web::HTML::ActivateTab activate_tab, WebView::ViewImplementation const* source_view) const
{
    if (auto* ui = ConnectionFromUI::the()) {
        auto source = new_tab_source(source_view);
        ui->async_app_open_url_in_new_tab(url, activate_tab, source.view_id, source.is_private);
    } else {
        WebView::Application::open_url_in_new_tab(url, activate_tab, source_view);
    }
}

void Application::activate_or_open_tab_with_url(URL::URL const& url) const
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_activate_or_open_tab_with_url(url);
    else
        WebView::Application::activate_or_open_tab_with_url(url);
}

void Application::open_urls_in_new_tabs(ReadonlySpan<URL::URL> urls) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::open_urls_in_new_tabs(urls);
        return;
    }
    Vector<URL::URL> urls_to_open;
    urls_to_open.append(urls.data(), urls.size());
    ui->async_app_open_urls_in_new_tabs(move(urls_to_open));
}

void Application::open_url_in_new_window(URL::URL const& url, WebView::IsPrivate is_private)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_open_url_in_new_window(url, is_private);
}

void Application::open_navigation_in_new_tab(Web::HTML::PreparedNavigationDescriptor navigation, Web::HTML::ActivateTab activate_tab, WebView::ViewImplementation const* source_view) const
{
    if (auto* ui = ConnectionFromUI::the()) {
        auto source = new_tab_source(source_view);
        ui->async_app_open_navigation_in_new_tab(move(navigation), activate_tab, source.view_id, source.is_private);
    } else {
        WebView::Application::open_navigation_in_new_tab(move(navigation), activate_tab, source_view);
    }
}

void Application::open_navigation_in_new_window(Web::HTML::PreparedNavigationDescriptor navigation, WebView::IsPrivate is_private)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_open_navigation_in_new_window(move(navigation), is_private);
}

NonnullRefPtr<Core::Promise<ByteString>> Application::ask_user_for_download_path(ByteString const& file) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui)
        return WebView::Application::ask_user_for_download_path(file);

    auto promise = Core::Promise<ByteString>::construct();
    auto request_id = m_next_ui_request_id++;
    m_pending_download_paths.set(request_id, promise);
    ui->async_app_ask_user_for_download_path(request_id, file);
    return promise;
}

void Application::did_get_download_path(u64 request_id, Optional<ByteString> path)
{
    auto promise = m_pending_download_paths.take(request_id);
    if (!promise.has_value())
        return;
    if (path.has_value())
        (*promise)->resolve(path.release_value());
    else
        (*promise)->reject(Error::from_errno(ECANCELED));
}

void Application::ask_user_for_extension_directory(Function<void(Optional<ByteString>)> callback) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::ask_user_for_extension_directory(move(callback));
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_extension_directories.set(request_id, move(callback));
    ui->async_app_ask_user_for_extension_directory(request_id);
}

void Application::did_get_extension_directory(u64 request_id, Optional<ByteString> path)
{
    if (auto callback = m_pending_extension_directories.take(request_id); callback.has_value())
        (*callback)(move(path));
}

void Application::ask_user_for_extension_file(Function<void(Optional<ByteString>)> callback) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::ask_user_for_extension_file(move(callback));
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_extension_files.set(request_id, move(callback));
    ui->async_app_ask_user_for_extension_file(request_id);
}

void Application::did_get_extension_file(u64 request_id, Optional<ByteString> path)
{
    if (auto callback = m_pending_extension_files.take(request_id); callback.has_value())
        (*callback)(move(path));
}

void Application::set_extension_tabs_from_ui(Vector<WebView::ExtensionTab> tabs, Vector<i64> window_focus_order)
{
    m_extension_tabs_from_ui = move(tabs);
    m_extension_window_focus_order_from_ui = move(window_focus_order);
}

Vector<WebView::ExtensionTab> Application::extension_tabs() const
{
    if (ConnectionFromUI::the())
        return m_extension_tabs_from_ui;
    return WebView::Application::extension_tabs();
}

Vector<i64> Application::extension_window_focus_order() const
{
    if (ConnectionFromUI::the())
        return m_extension_window_focus_order_from_ui;
    return WebView::Application::extension_window_focus_order();
}

void Application::extension_action_list_changed()
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_extension_action_list_changed();
}

void Application::extension_action_changed(String const& extension_id, Optional<i64> tab_id)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_extension_action_changed(extension_id, tab_id);
}

View::InitialState Application::initial_state_for_extension_view(i64 tab_id) const
{
    Optional<WebView::ViewImplementation&> related;
    for (auto const& tab : extension_tabs()) {
        if (tab.id == tab_id && tab.view_id.has_value())
            related = WebView::ViewImplementation::find_view_by_id(*tab.view_id);
    }
    if (!related.has_value())
        related = active_web_view();
    View::InitialState initial_state;
    if (related.has_value()) {
        initial_state = static_cast<View&>(*related).initial_state_for_related_view();
        initial_state.owner_view_id = related->view_id();
    }
    initial_state.is_tab = false;
    return initial_state;
}

bool Application::show_extension_popup(WebView::ExtensionPopupRequest const& request)
{
    if (!ConnectionFromUI::the())
        return WebView::Application::show_extension_popup(request);

    auto& view = create_view(initial_state_for_extension_view(request.tab_id));
    auto window_id = request.window_id;
    auto generation = request.generation;
    view.set_reports_content_size(WebView::minimum_extension_popup_size, WebView::maximum_extension_popup_size);
    view.set_is_closable_by_script(true);
    view.on_content_size_change = [window_id, generation](Gfx::IntSize size) {
        extension_popup_host().content_size_changed(window_id, generation, size);
    };
    view.on_close = [window_id, generation] {
        Core::deferred_invoke([window_id, generation] {
            if (is_initialized())
                extension_popup_host().close(window_id, generation, WebView::ExtensionPopupHost::CloseReason::WindowClose);
        });
    };
    m_extension_popups_in_ui.set(window_id, make<ExtensionPopupInUI>(view, request.extension_id, window_id, generation));
    view.load_from_browser_ui(request.url);
    return true;
}

void Application::resize_extension_popup(i64 window_id, Gfx::IntSize size)
{
    if (auto popup = m_extension_popups_in_ui.get(window_id); popup.has_value())
        (*popup)->set_size(size);
    else
        WebView::Application::resize_extension_popup(window_id, size);
}

void Application::close_extension_popup(i64 window_id)
{
    auto popup = m_extension_popups_in_ui.take(window_id);
    if (!popup.has_value()) {
        WebView::Application::close_extension_popup(window_id);
        return;
    }
    // Nothing of a closed popup reaches the host again, even before the UI gives the view back.
    (*popup)->view().on_content_size_change = nullptr;
    (*popup)->view().on_close = nullptr;
}

WebView::ViewImplementation* Application::extension_popup_view(i64 window_id) const
{
    if (auto popup = m_extension_popups_in_ui.get(window_id); popup.has_value())
        return &(*popup)->view();
    return WebView::Application::extension_popup_view(window_id);
}

void Application::did_close_extension_popup(u64 view_id, WebView::ExtensionPopupHost::CloseReason reason)
{
    if (auto* popup = ExtensionPopupInUI::find(view_id))
        extension_popup_host().close(popup->window_id(), popup->generation(), reason);
}

Optional<i64> Application::extension_window_for_new_tab(WebView::ExtensionTabCreation const& creation) const
{
    auto tabs = extension_tabs();
    auto window_exists = [&](i64 id) { return any_of(tabs, [&](auto const& tab) { return tab.window_id == id; }); };
    auto window_is_private = [&](i64 id) { return any_of(tabs, [&](auto const& tab) { return tab.window_id == id && tab.is_private == WebView::IsPrivate::Yes; }); };
    if (creation.window_id.has_value() && window_exists(*creation.window_id))
        return creation.window_id;
    auto focus_order = extension_window_focus_order();
    if (!focus_order.is_empty() && window_exists(focus_order.first()) && (!window_is_private(focus_order.first()) || creation.may_open_in_private_window))
        return focus_order.first();
    for (auto const& tab : tabs) {
        if (tab.is_private == WebView::IsPrivate::No && !tab.in_popup_window)
            return tab.window_id;
    }
    return {};
}

void Application::create_extension_tab(WebView::ExtensionTabCreation const& creation, ExtensionTabCallback callback)
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::create_extension_tab(creation, move(callback));
        return;
    }
    auto window_id = extension_window_for_new_tab(creation);
    // The new tab's view starts out like its window's other tabs', with the window's privacy; a new window is a normal one.
    View::InitialState initial_state;
    if (window_id.has_value()) {
        for (auto const& tab : extension_tabs()) {
            if (tab.window_id != *window_id)
                continue;
            initial_state.is_private = tab.is_private;
            if (!tab.view_id.has_value())
                continue;
            if (auto view = WebView::ViewImplementation::find_view_by_id(*tab.view_id); view.has_value()) {
                initial_state = static_cast<View&>(*view).initial_state_for_related_view();
                break;
            }
        }
    }
    initial_state.owner_view_id = {};
    initial_state.is_tab = true;
    auto& view = create_view(move(initial_state));
    auto request_id = m_next_ui_request_id++;
    m_pending_extension_tab_commands.set(request_id, { move(callback), creation.url.value_or(settings().new_tab_page_url()) });
    ui->async_extension_create_tab(request_id, view.view_id(), window_id, creation.index, creation.active, creation.pinned);
}

void Application::update_extension_tab(i64 tab_id, WebView::ExtensionTabUpdate const& update, ExtensionTabCallback callback)
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::update_extension_tab(tab_id, update, move(callback));
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_extension_tab_commands.set(request_id, { move(callback), update.url });
    ui->async_extension_update_tab(request_id, tab_id, update);
}

void Application::close_extension_tab(i64 tab_id, ExtensionTabActionCallback callback)
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::close_extension_tab(tab_id, move(callback));
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_extension_tab_actions.set(request_id, move(callback));
    ui->async_extension_close_tab(request_id, tab_id);
}

void Application::reload_extension_tab(i64 tab_id, bool bypass_cache, ExtensionTabActionCallback callback)
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::reload_extension_tab(tab_id, bypass_cache, move(callback));
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_extension_tab_actions.set(request_id, move(callback));
    ui->async_extension_reload_tab(request_id, tab_id, bypass_cache);
}

void Application::did_finish_extension_tab_action(u64 request_id, Optional<String> error)
{
    auto callback = m_pending_extension_tab_actions.take(request_id);
    if (!callback.has_value())
        return;
    if (error.has_value())
        (*callback)(error.release_value());
    else
        (*callback)({});
}

void Application::did_finish_extension_tab_command(u64 request_id, Optional<WebView::ExtensionTab> tab, Optional<String> error)
{
    auto command = m_pending_extension_tab_commands.take(request_id);
    if (!command.has_value())
        return;
    if (!tab.has_value()) {
        command->callback(error.value_or("The tab couldn't be opened"_string));
        return;
    }
    // The page loads once the tab is in its window, which has a view for it by now.
    if (command->url.has_value()) {
        if (tab->view_id.has_value()) {
            if (auto view = WebView::ViewImplementation::find_view_by_id(*tab->view_id); view.has_value())
                view->load_extension_page(*command->url);
        }
        tab->url = *command->url;
        // The tab will show the new page, so it has none of the old one's title.
        tab->title = {};
    }
    command->callback(tab.release_value());
}

Vector<WebView::ExtensionManifest::IconPath> Application::resolve_extension_icon_files(String const& extension_id, Vector<WebView::ExtensionManifest::IconPath> const& paths)
{
    Vector<WebView::ExtensionManifest::IconPath> files;
    auto extension = extension_registry().find(extension_id);
    if (!extension)
        return files;
    for (auto const& path : paths) {
        auto file = extension->resolve_resource_path(ByteString::formatted("/{}", path.path));
        if (file.is_error())
            continue;
        files.append({ .size = path.size, .path = MUST(String::from_byte_string(file.value())) });
    }
    return files;
}

bool Application::show_extension_side_panel(WebView::ExtensionSidePanelRequest const& request)
{
    if (!ConnectionFromUI::the())
        return WebView::Application::show_extension_side_panel(request);

    auto icon_files = resolve_extension_icon_files(request.extension_id, request.icon);
    // The window's panel showing the same extension again keeps its view, with the new page and header.
    if (auto panel = m_extension_side_panels_in_ui.get(request.window_id); panel.has_value() && (*panel)->extension_id() == request.extension_id) {
        (*panel)->set_generation(request.generation);
        (*panel)->set_header(request.title, icon_files);
        (*panel)->view().load_from_browser_ui(request.url);
        return true;
    }
    // Another extension's panel in the window goes first.
    m_extension_side_panels_in_ui.remove(request.window_id);

    auto active_tab_id = extension_active_tab_of_window(request.window_id);
    auto& view = create_view(initial_state_for_extension_view(active_tab_id.value_or(-1)));
    view.set_is_closable_by_script(true);
    // The panel's page can close itself, and the window's panel may be another one by the time that lands.
    view.on_close = [view_id = view.view_id()] {
        Core::deferred_invoke([view_id] {
            if (auto* panel = ExtensionSidePanelInUI::find(view_id); panel && is_initialized())
                extension_side_panel_host().close(panel->window_id(), panel->generation(), WebView::ExtensionSidePanelHost::CloseReason::PageClosed);
        });
    };
    m_extension_side_panels_in_ui.set(request.window_id, make<ExtensionSidePanelInUI>(view, request.extension_id, request.window_id, request.generation, request.title, icon_files));
    view.load_from_browser_ui(request.url);
    return true;
}

void Application::navigate_extension_side_panel(i64 window_id, URL::URL const& url)
{
    if (auto panel = m_extension_side_panels_in_ui.get(window_id); panel.has_value())
        (*panel)->view().load_from_browser_ui(url);
    else
        WebView::Application::navigate_extension_side_panel(window_id, url);
}

void Application::close_extension_side_panel(i64 window_id)
{
    auto panel = m_extension_side_panels_in_ui.take(window_id);
    if (!panel.has_value()) {
        WebView::Application::close_extension_side_panel(window_id);
        return;
    }
    // Nothing of a closed panel reaches the host again, even before the UI gives the view back.
    (*panel)->view().on_close = nullptr;
}

WebView::ViewImplementation* Application::extension_side_panel_view(i64 window_id) const
{
    if (auto panel = m_extension_side_panels_in_ui.get(window_id); panel.has_value())
        return &(*panel)->view();
    return WebView::Application::extension_side_panel_view(window_id);
}

void Application::did_close_extension_side_panel(u64 view_id)
{
    if (auto* panel = ExtensionSidePanelInUI::find(view_id))
        extension_side_panel_host().close(panel->window_id(), panel->generation(), WebView::ExtensionSidePanelHost::CloseReason::UserClosed);
}

void Application::display_download_confirmation_dialog(StringView download_name, LexicalPath const& path) const
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_display_download_confirmation_dialog(MUST(String::from_utf8(download_name)), path.string());
    else
        WebView::Application::display_download_confirmation_dialog(download_name, path);
}

void Application::display_error_dialog(StringView error_message) const
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_display_error_dialog(MUST(String::from_utf8(error_message)));
    else
        WebView::Application::display_error_dialog(error_message);
}

void Application::display_openssl_mismatch(PlatformFeatures::OpenSSLFacts const& facts)
{
    auto* ui = ConnectionFromUI::the();
    if (!ui)
        return;
    if (!m_ui_did_start) {
        m_openssl_mismatch_awaiting_ui = facts;
        return;
    }
    ui->async_app_display_openssl_mismatch(facts.curl_version.value_or({}), facts.linked_version);
}

#if defined(LADYBIRD_HAS_UPDATE_CHECK)
void Application::display_update_check_result(WebView::UpdateCheckResult const& result, WebView::UpdateCheckTrigger trigger)
{
    auto* ui = ConnectionFromUI::the();
    if (!ui)
        return;

    Optional<String> version;
    auto install_method = WebView::InstallMethod::Source;
    if (result.update.has_value()) {
        version = result.update->version;
        install_method = result.update->install_method;
    }
    ui->async_app_display_update_check_result(result.kind, move(version), install_method, trigger);
}
#endif

void Application::set_clipboard_text(String text, ClipboardType type)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_set_clipboard_text(move(text), type);
    else
        WebView::Application::set_clipboard_text(move(text), type);
}

void Application::insert_clipboard_item(Web::Clipboard::SystemClipboardItem item)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_insert_clipboard_item(move(item));
    else
        WebView::Application::insert_clipboard_item(move(item));
}

void Application::read_clipboard_text(ClipboardType type, Function<void(Utf16String)> on_read) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::read_clipboard_text(type, move(on_read));
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_clipboard_text_reads.set(request_id, move(on_read));
    ui->async_app_read_clipboard_text(request_id, type);
}

void Application::did_read_clipboard_text(u64 request_id, Utf16String text)
{
    if (auto on_read = m_pending_clipboard_text_reads.take(request_id); on_read.has_value())
        (*on_read)(move(text));
}

void Application::read_clipboard_item(Function<void(Web::Clipboard::SystemClipboardItem)> on_read) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::read_clipboard_item(move(on_read));
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_clipboard_item_reads.set(request_id, move(on_read));
    ui->async_app_read_clipboard_item(request_id);
}

void Application::did_read_clipboard_item(u64 request_id, Web::Clipboard::SystemClipboardItem item)
{
    if (auto on_read = m_pending_clipboard_item_reads.take(request_id); on_read.has_value())
        (*on_read)(move(item));
}

void Application::update_tabs_display() const
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_update_tabs_display();
}

void Application::did_clear_history_since(UnixDateTime since) const
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_did_clear_history_since(since);
}

void Application::quit_for_signal()
{
    // NB: The UI keeps the session, so it is the UI that quits. We end along with it.
    if (auto* ui = ConnectionFromUI::the()) {
        ui->async_app_quit_for_signal();
        return;
    }
    WebView::Application::quit_for_signal();
}

void Application::destroy_view(u64 view_id)
{
    m_views.remove(view_id);
}

Core::EventLoop& Application::create_platform_event_loop()
{
    // Headless mode has no UI to show anything in.
    if (browser_options().headless_mode.has_value())
        detach_from_host();

    if (m_host)
        m_host->will_create_event_loop(arguments());
    auto& event_loop = WebView::Application::create_platform_event_loop();
    if (m_host)
        m_host->did_create_event_loop(event_loop);
    return event_loop;
}

NonnullOwnPtr<WebView::DefaultBrowser> Application::create_default_browser()
{
    if (ui_info())
        return make<DefaultBrowserInUI>();
    return WebView::Application::create_default_browser();
}

NonnullOwnPtr<WebView::MicrophoneAuthorizer> Application::create_microphone_authorizer()
{
    // NB: The UI tells of no authorization where the system has no say over the microphone.
    if (auto const* info = ui_info(); info && info->microphone_authorization.has_value())
        return make<MicrophoneAuthorizerInUI>(*info->microphone_authorization);
    return WebView::Application::create_microphone_authorizer();
}

NonnullOwnPtr<WebView::NotificationBackend> Application::create_notification_backend()
{
    if (ConnectionFromUI::the())
        return make<NotificationBackendInUI>();
    return WebView::Application::create_notification_backend();
}

OwnPtr<WebView::MediaSessionPlatformBackend> Application::create_media_session_platform_backend()
{
    if (ConnectionFromUI::the())
        return make<MediaSessionPlatformBackendInUI>();
    return {};
}

void Application::on_devtools_enabled() const
{
    WebView::Application::on_devtools_enabled();
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_did_enable_devtools(*browser_options().devtools_port);
}

void Application::on_devtools_disabled() const
{
    WebView::Application::on_devtools_disabled();
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_did_disable_devtools();
}

void Application::on_compositor_gpu_painting_changed() const
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_gpu_painting_changed(compositor_gpu_painting_enabled());
}

void Application::create_platform_actions()
{
    if (m_host)
        m_host->create_platform_actions();
    else
        WebView::Application::create_platform_actions();
}

Optional<String> Application::ui_font_family() const
{
    if (m_host)
        return m_host->ui_font_family();
    if (auto const* info = ui_info())
        return info->ui_font_family;
    return WebView::Application::ui_font_family();
}

Web::CSS::PreferredColorScheme Application::interface_color_scheme() const
{
    if (m_host)
        return m_host->interface_color_scheme();
    if (auto const* info = ui_info())
        return info->interface_color_scheme;
    return WebView::Application::interface_color_scheme();
}

Optional<String> Application::system_font_family() const
{
    if (m_host)
        return m_host->system_font_family();
    if (auto const* info = ui_info())
        return info->system_font_family;
    return WebView::Application::system_font_family();
}

bool Application::supports_picture_in_picture() const
{
    if (m_host)
        return m_host->supports_picture_in_picture();
    if (auto const* info = ui_info())
        return info->supports_picture_in_picture;
    return WebView::Application::supports_picture_in_picture();
}

OwnPtr<WebView::PictureInPictureWindow> Application::create_picture_in_picture_window(WebView::WebContentPage& requesting_page, WebView::CanonicalTraversable& traversable, Gfx::IntSize video_size)
{
    if (!ConnectionFromUI::the())
        return WebView::Application::create_picture_in_picture_window(requesting_page, traversable, video_size);

    auto& owner_view = static_cast<View&>(requesting_page.view());
    auto initial_state = owner_view.initial_state_for_related_view();
    initial_state.owner_view_id = owner_view.view_id();
    initial_state.is_tab = false;
    initial_state.scales_frames_to_fit = true;
    auto screen_size = owner_view.screen_size();
    auto& view = create_view(move(initial_state), traversable);
    return make<PictureInPictureWindowInUI>(view, owner_view.view_id(), video_size, WebView::PictureInPictureWindow::initial_size(video_size, screen_size));
}

void Application::focus_page_from_notification(WebView::WebContentPage& page)
{
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_focus_view_from_notification(page.view().view_id());
}

void Application::open_url_in_new_tab_from_notification(URL::URL const& url, Web::PageId, WebView::CanonicalTraversable* canonical_traversable, WebView::IsPrivate is_private)
{
    Optional<u64> view_id;
    if (canonical_traversable) {
        if (auto view = canonical_traversable->view(); view.has_value())
            view_id = view->view_id();
    }
    if (auto* ui = ConnectionFromUI::the())
        ui->async_app_open_url_in_new_tab_from_notification(url, view_id, is_private);
}

void Application::resolve_external_url_handler(URL::URL const& url, WebView::ExternalURLHandlerCallback callback) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        WebView::Application::resolve_external_url_handler(url, move(callback));
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_external_url_handlers.set(request_id, move(callback));
    ui->async_external_url_resolve_handler(request_id, url);
}

void Application::did_resolve_external_url_handler(u64 request_id, Optional<u64> handler_id, String application_name, bool is_ladybird)
{
    auto callback = m_pending_external_url_handlers.take(request_id);
    if (!callback.has_value())
        return;
    if (handler_id.has_value())
        (*callback)(adopt_ref(*new ExternalURLHandlerInUI(*handler_id, move(application_name), is_ladybird)));
    else
        (*callback)(nullptr);
}

void Application::launch_external_url(u64 handler_id, URL::URL const& url, Function<void(bool)> callback) const
{
    auto* ui = ConnectionFromUI::the();
    if (!ui) {
        callback(false);
        return;
    }
    auto request_id = m_next_ui_request_id++;
    m_pending_external_url_launches.set(request_id, move(callback));
    ui->async_external_url_launch(handler_id, request_id, url);
}

void Application::did_launch_external_url(u64 request_id, bool launched)
{
    if (auto callback = m_pending_external_url_launches.take(request_id); callback.has_value())
        (*callback)(launched);
}

bool Application::supports_clipboard_type(WebView::Application::ClipboardType type) const
{
    if (m_host)
        return m_host->supports_clipboard_type(type);
    if (auto const* info = ui_info())
        return type == ClipboardType::Text || info->supports_selection_clipboard;
    return WebView::Application::supports_clipboard_type(type);
}

bool Application::supports_system_menu_bar() const
{
    if (m_host)
        return m_host->supports_system_menu_bar();
    if (auto const* info = ui_info())
        return info->supports_system_menu_bar;
    return WebView::Application::supports_system_menu_bar();
}

bool Application::supports_vertical_tabs() const
{
    if (m_host)
        return m_host->supports_vertical_tabs();
    if (auto const* info = ui_info())
        return info->supports_vertical_tabs;
    return WebView::Application::supports_vertical_tabs();
}

bool Application::supports_private_browsing_windows() const
{
    if (m_host)
        return m_host->supports_private_browsing_windows();
    if (auto const* info = ui_info())
        return info->supports_private_browsing_windows;
    return WebView::Application::supports_private_browsing_windows();
}

bool Application::supports_client_side_window_decorations() const
{
    if (m_host)
        return m_host->supports_client_side_window_decorations();
    if (auto const* info = ui_info())
        return info->supports_client_side_window_decorations;
    return WebView::Application::supports_client_side_window_decorations();
}

bool Application::platform_reports_scroll_momentum() const
{
    if (m_host)
        return m_host->platform_reports_scroll_momentum();
    if (auto const* info = ui_info())
        return info->reports_scroll_momentum;
    return WebView::Application::platform_reports_scroll_momentum();
}

bool Application::platform_scrolls_elastically() const
{
    if (m_host)
        return m_host->platform_scrolls_elastically();
    if (auto const* info = ui_info())
        return info->scrolls_elastically;
    return WebView::Application::platform_scrolls_elastically();
}

bool Application::platform_scrollbar_track_press_jumps() const
{
    if (m_host)
        return m_host->platform_scrollbar_track_press_jumps();
    if (auto const* info = ui_info())
        return info->scrollbar_track_press_jumps;
    return WebView::Application::platform_scrollbar_track_press_jumps();
}

bool Application::platform_scrollbars_always_shown() const
{
    if (m_host)
        return m_host->platform_scrollbars_always_shown();
    if (auto const* info = ui_info())
        return info->scrollbars_always_shown;
    return WebView::Application::platform_scrollbars_always_shown();
}

bool Application::platform_prefers_reduced_motion() const
{
    if (m_host)
        return m_host->platform_prefers_reduced_motion();
    if (auto const* info = ui_info())
        return info->prefers_reduced_motion;
    return WebView::Application::platform_prefers_reduced_motion();
}

bool Application::platform_prefers_more_contrast() const
{
    if (m_host)
        return m_host->platform_prefers_more_contrast();
    if (auto const* info = ui_info())
        return info->prefers_more_contrast;
    return WebView::Application::platform_prefers_more_contrast();
}

Vector<String> Application::platform_preferred_languages() const
{
    if (m_host)
        return m_host->platform_preferred_languages();
    if (auto const* info = ui_info())
        return info->preferred_languages;
    return WebView::Application::platform_preferred_languages();
}

Optional<Gfx::GpuDeviceIdentity> Application::platform_presentation_gpu_device() const
{
    if (m_host)
        return m_host->platform_presentation_gpu_device();
    if (auto const* info = ui_info())
        return info->presentation_gpu_device;
    return WebView::Application::platform_presentation_gpu_device();
}

Vector<u64> Application::platform_presentation_drm_format_modifiers() const
{
    if (m_host)
        return m_host->platform_presentation_drm_format_modifiers();
    if (auto const* info = ui_info())
        return info->presentation_drm_format_modifiers;
    return WebView::Application::platform_presentation_drm_format_modifiers();
}

}
