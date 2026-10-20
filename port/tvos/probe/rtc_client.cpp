/* WebRTC-only fixture receiver. The fixture harness supplies a local
   tracker and a generated payload. Ordinary TCP/uTP/DHT and web seeds are
   disabled: completion must come from an RTC peer, not a fallback. */
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>

int main(int argc, char **argv)
{
    namespace lt = libtorrent;
    if (argc != 4) {
        std::cerr << "rtc_client <magnet> <empty-download-dir> <expected-file>\n";
        return 2;
    }
    try {
        lt::settings_pack settings;
        settings.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
        settings.set_str(lt::settings_pack::webtorrent_stun_server, "");
        settings.set_bool(lt::settings_pack::enable_dht, false);
        settings.set_bool(lt::settings_pack::enable_lsd, false);
        settings.set_bool(lt::settings_pack::enable_upnp, false);
        settings.set_bool(lt::settings_pack::enable_natpmp, false);
        settings.set_bool(lt::settings_pack::enable_incoming_tcp, false);
        settings.set_bool(lt::settings_pack::enable_outgoing_tcp, false);
        settings.set_bool(lt::settings_pack::enable_incoming_utp, false);
        settings.set_bool(lt::settings_pack::enable_outgoing_utp, false);
        settings.set_int(lt::settings_pack::connections_limit, 8);
        settings.set_int(lt::settings_pack::alert_mask,
            static_cast<std::uint32_t>(lt::alert_category::error | lt::alert_category::status | lt::alert_category::tracker));
        lt::session session(settings);
        auto parameters = lt::parse_magnet_uri(argv[1]);
        parameters.save_path = argv[2];
        parameters.url_seeds.clear();
        parameters.http_seeds.clear();
        parameters.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
        parameters.flags |= lt::torrent_flags::override_web_seeds | lt::torrent_flags::override_trackers;
        auto handle = session.add_torrent(parameters);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(75);
        while (!handle.status().is_seeding) {
            std::vector<lt::alert *> alerts;
            session.pop_alerts(&alerts);
            for (auto *alert : alerts) std::cerr << alert->message() << '\n';
            auto status = handle.status();
            if (status.errc) throw std::runtime_error(status.errc.message());
            if (std::chrono::steady_clock::now() > deadline) throw std::runtime_error("WebRTC fixture timeout");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        auto info = handle.torrent_file();
        if (!info || info->files().num_files() != 1) throw std::runtime_error("expected exactly one fixture file");
        auto name = info->files().file_path(lt::file_index_t{0});
        if (name != "fixture.bin") throw std::runtime_error("unexpected fixture path");
        std::ifstream expected_file(argv[3], std::ios::binary);
        std::vector<char> expected(std::istreambuf_iterator<char>(expected_file), {}), actual;
        if (expected.empty()) throw std::runtime_error("missing expected fixture");
        handle.flush_cache();
        do {
            std::ifstream input(std::filesystem::path(argv[2]) / name, std::ios::binary);
            actual.assign(std::istreambuf_iterator<char>(input), {});
            if (actual == expected) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while (std::chrono::steady_clock::now() < deadline);
        if (actual != expected) throw std::runtime_error("WebRTC payload byte mismatch");
        if (handle.status().total_payload_download < std::int64_t(expected.size()))
            throw std::runtime_error("fixture already existed; no actual transfer proved");
        std::cout << "PASS: WebRTC-only magnet metadata, verified pieces and byte comparison ("
                  << actual.size() << " bytes)\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
