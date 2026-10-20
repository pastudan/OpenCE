/* A generated fixture, two loopback-only sessions and verified payload.
   This tests ordinary TCP only; compiling RTC code is NOT a WebRTC test. */
#include "swarm_test.h"
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/version.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <vector>

namespace lt = libtorrent;
namespace fs = std::filesystem;

static lt::settings_pack settings()
{
    lt::settings_pack s;
    s.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
    s.set_str(lt::settings_pack::outgoing_interfaces, "127.0.0.1");
    s.set_bool(lt::settings_pack::enable_dht, false);
    s.set_bool(lt::settings_pack::enable_lsd, false);
    s.set_bool(lt::settings_pack::enable_upnp, false);
    s.set_bool(lt::settings_pack::enable_natpmp, false);
    s.set_bool(lt::settings_pack::enable_incoming_utp, false);
    s.set_bool(lt::settings_pack::enable_outgoing_utp, false);
    s.set_int(lt::settings_pack::connections_limit, 8);
    return s;
}

extern "C" int opence_probe_tcp(const char *directory, char *report, unsigned int capacity)
{
    try {
        if (!directory || !report || capacity == 0) return 1;
#if !TORRENT_USE_RTC
#error "WebTorrent support must be compiled into the probe"
#endif
        fs::path root(directory), seed = root / "seed", download = root / "download";
        fs::create_directories(seed);
        fs::create_directories(download);
        // About 1 MiB, nonzero and reproducible. Contains no game data.
        std::vector<char> payload(1024 * 1024 + 137);
        for (std::size_t i = 0; i < payload.size(); ++i)
            payload[i] = char((i * 31 + (i >> 8) * 17) & 255);
        {
            std::ofstream out(seed / "fixture.bin", std::ios::binary);
            out.write(payload.data(), std::streamsize(payload.size()));
            if (!out) throw std::runtime_error("fixture write failed");
        }
        lt::file_storage files;
        files.add_file("fixture.bin", std::int64_t(payload.size()));
        lt::create_torrent create(files, 16 * 1024, lt::create_torrent::v1_only);
        lt::set_piece_hashes(create, seed.string());
        std::vector<char> metadata;
        lt::bencode(std::back_inserter(metadata), create.generate());
        auto info = std::make_shared<lt::torrent_info>(metadata.data(), int(metadata.size()));
        lt::session seeder(settings()), client(settings());
        lt::add_torrent_params source;
        source.ti = info;
        source.save_path = seed.string();
        source.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
        auto seed_handle = seeder.add_torrent(source);
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(40);
        while (!seed_handle.status().is_seeding || seeder.listen_port() == 0) {
            if (std::chrono::steady_clock::now() > end) throw std::runtime_error("seeder setup timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        lt::add_torrent_params destination;
        destination.ti = info;
        destination.save_path = download.string();
        destination.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
        auto handle = client.add_torrent(destination);
        handle.connect_peer({lt::make_address("127.0.0.1"), seeder.listen_port()});
        while (!handle.status().is_seeding) {
            auto status = handle.status();
            if (status.errc) throw std::runtime_error(status.errc.message());
            if (std::chrono::steady_clock::now() > end) throw std::runtime_error("TCP payload timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // Hashes verified above; independent disk byte comparison as well.
        handle.flush_cache();
        std::vector<char> received;
        do {
            std::ifstream in(download / "fixture.bin", std::ios::binary);
            received.assign(std::istreambuf_iterator<char>(in), {});
            if (received == payload) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        } while (std::chrono::steady_clock::now() < end);
        if (received != payload) throw std::runtime_error("payload byte comparison failed");
        std::snprintf(report, capacity,
            "libtorrent %s: TCP fixture verified (%zu bytes); WebTorrent compiled, interop NOT tested", 
            LIBTORRENT_VERSION, payload.size());
        return 0;
    } catch (const std::exception &error) {
        if (report && capacity) std::snprintf(report, capacity, "FAIL: %s", error.what());
        return 1;
    }
}
